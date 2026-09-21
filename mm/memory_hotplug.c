// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/memory_hotplug.c
 *
 *  Copyright (C)
 *  中文：版权占位信息；本文件实现通用内存热添加、上线、下线和移除事务。
 */

#include <linux/stddef.h>
/* 依赖按对象层次排列：页/zone、设备与资源树、迁移隔离，以及节点热插拔通知。 */
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/swap.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/writeback.h>
/* writeback 和 slab 等消费者需要在容量变化后重算全局限速或分配状态。 */
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/cpu.h>
#include <linux/memory.h>
#include <linux/memremap.h>
#include <linux/memory_hotplug.h>
/* 公共 hotplug 契约提供 mmop、memory_group、通知器参数和全局锁接口。 */
#include <linux/vmalloc.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/migrate.h>
#include <linux/page-isolation.h>
/* 下线先隔离 pageblock，再迁移仍在使用的可移动 folio。 */
#include <linux/pfn.h>
#include <linux/suspend.h>
#include <linux/mm_inline.h>
#include <linux/firmware-map.h>
#include <linux/stop_machine.h>
#include <linux/hugetlb.h>
#include <linux/memblock.h>
#include <linux/compaction.h>
/* kcompactd/kswapd 生命周期随节点是否仍有内存而变化。 */
#include <linux/rmap.h>
#include <linux/module.h>
#include <linux/node.h>

#include <asm/tlbflush.h>

#include "internal.h"
#include "shuffle.h"

enum {
	/* 参数解析器产生、altmap 规划消费：元数据仍由常规内存承担。 */
	MEMMAP_ON_MEMORY_DISABLE = 0,
	/* 用户 Y/true 产生：仅在天然满足 PMD/pageblock 对齐时把 vmemmap 放进新内存。 */
	MEMMAP_ON_MEMORY_ENABLE,
	/* 用户 force 产生：补齐保留页到 pageblock，牺牲容量换取 self-hosted 布局。 */
	MEMMAP_ON_MEMORY_FORCE,
};

static int memmap_mode __read_mostly = MEMMAP_ON_MEMORY_DISABLE;

/*
 * 业务背景：self-hosted vmemmap 需要先知道描述一个 memory block 要占多少物理空间；
 * 参数：无，block 大小取全局 memory-block 配置，计算过程不取得对象引用；
 * 返回：完整 `struct page` 数组的字节数，供页数换算和平台能力检查继续使用；
 * 注意事项：纯计算、无需锁且可在启动/运行期调用，结果尚未按 PAGE_SIZE/pageblock 对齐。
 */
static inline unsigned long memory_block_memmap_size(void)
{
	return PHYS_PFN(memory_block_size_bytes()) * sizeof(struct page);
}

/*
 * 业务背景：create_altmaps_and_memory_blocks() 要从每块新内存头部划出 vmemmap 保留区；
 * 参数：无，读取只读启动参数 memmap_mode，不接收或转移 altmap ownership；
 * 返回：保留区页数；普通模式按页上取整，FORCE 模式再按 pageblock 上取整；
 * 注意事项：无需锁；FORCE 的额外页永久不进入 buddy，调用者必须把它计入容量损耗。
 */
static inline unsigned long memory_block_memmap_on_memory_pages(void)
{
	unsigned long nr_pages = PFN_UP(memory_block_memmap_size());

	/*
	 * In "forced" memmap_on_memory mode, we add extra pages to align the
	 * vmemmap size to cover full pageblocks. That way, we can add memory
	 * even if the vmemmap size is not properly aligned, however, we might waste
	 * memory.
	 * 中文：强制模式牺牲少量容量，把 vmemmap 保留区补齐到完整 pageblock，
	 * 从而让后续隔离、上线和下线都继续满足 pageblock 粒度约束。
	 */
	if (memmap_mode == MEMMAP_ON_MEMORY_FORCE)
		return pageblock_align(nr_pages);
	return nr_pages;
}

#ifdef CONFIG_MHP_MEMMAP_ON_MEMORY
/*
 * memory_hotplug.memmap_on_memory parameter
 * 中文：解析只读启动参数 memory_hotplug.memmap_on_memory。
 */
/*
 * 业务背景：module_param_cb 在启动期解析 memory_hotplug.memmap_on_memory，选择元数据布局；
 * 参数：val 是框架借用的 NUL 结尾字符串；kp 非空且 kp->arg 指向静态 memmap_mode；
 * 返回：0 表示已发布 DISABLE/ENABLE/FORCE，非法布尔串返回解析 errno 且旧值不变；
 * 注意事项：参数权限 0444，运行中不会并发改布局；FORCE 仅记录一次预期浪费，不创建内存块。
 */
static int set_memmap_mode(const char *val, const struct kernel_param *kp)
{
	int ret, mode;
	bool enabled;

	if (sysfs_streq(val, "force") ||  sysfs_streq(val, "FORCE")) {
		/* force 是三态参数中不能由 kstrtobool 表达的第三种取值。 */
		mode = MEMMAP_ON_MEMORY_FORCE;
	} else {
		ret = kstrtobool(val, &enabled);
		/* 标准 Y/N/布尔串映射为启用或禁用，解析失败保持原参数值。 */
		if (ret < 0)
			return ret;
		if (enabled)
			mode = MEMMAP_ON_MEMORY_ENABLE;
		else
			mode = MEMMAP_ON_MEMORY_DISABLE;
	}
	*((int *)kp->arg) = mode;
	/* 发布后仅报告一次每块内存因对齐浪费的页数，便于容量规划。 */
	if (mode == MEMMAP_ON_MEMORY_FORCE) {
		unsigned long memmap_pages = memory_block_memmap_on_memory_pages();

		pr_info_once("Memory hotplug will waste %ld pages in each memory block\n",
			     memmap_pages - PFN_UP(memory_block_memmap_size()));
	}
	return 0;
}

/*
 * 业务背景：模块参数读取需要把内部三态重新编码成用户可见的 Y/N/force；
 * 参数：buffer 是框架提供的可写输出区，kp->arg 借用静态 memmap_mode，二者均不可空；
 * 返回：sprintf 写入的字符数，不返回 errno，也不转移 buffer ownership；
 * 注意事项：0444 参数在运行期稳定，故无需额外锁；只有 FORCE 输出单词，其余按布尔字符输出。
 */
static int get_memmap_mode(char *buffer, const struct kernel_param *kp)
{
	int mode = *((int *)kp->arg);

	if (mode == MEMMAP_ON_MEMORY_FORCE)
		return sprintf(buffer, "force\n");
	return sprintf(buffer, "%c\n", mode ? 'Y' : 'N');
}

static const struct kernel_param_ops memmap_mode_ops = {
	/* ops 和 arg 均为静态长寿对象，参数核心只借用指针。 */
	.set = set_memmap_mode,
	.get = get_memmap_mode,
};
module_param_cb(memmap_on_memory, &memmap_mode_ops, &memmap_mode, 0444);
/* 0444 只允许启动期设置，运行中不能改变已建立内存块的元数据布局。 */
MODULE_PARM_DESC(memmap_on_memory, "Enable memmap on memory for memory hotplug\n"
		 "With value \"force\" it could result in memory wastage due "
		 "to memmap size limitations (Y/N/force)");

/*
 * 业务背景：add/remove 的 altmap 分支需要统一查询 self-host 功能是否在启动时开启；
 * 参数：无，只读静态 memmap_mode，不取得任何引用；
 * 返回：ENABLE/FORCE 返回 true，DISABLE 返回 false；
 * 注意事项：仅在 CONFIG_MHP_MEMMAP_ON_MEMORY 下是真实查询；纯读、无需锁且无副作用。
 */
static inline bool mhp_memmap_on_memory(void)
{
	return memmap_mode != MEMMAP_ON_MEMORY_DISABLE;
}
#else
/*
 * 业务背景：未配置 self-host 功能时仍需让公共 add/remove 源码保留同一调用点；
 * 参数：无，也不存在运行期参数状态；
 * 返回：恒为 false，使调用者选择普通 vmemmap 路径；
 * 注意事项：条件编译空桩，无锁、无副作用，不能据此创建或查找 altmap。
 */
static inline bool mhp_memmap_on_memory(void)
{
	return false;
}
#endif

enum {
	/* 缺省参数产生：zone_for_pfn_range 延续唯一相交 zone，歧义时通常选 kernel。 */
	ONLINE_POLICY_CONTIG_ZONES = 0,
	/* auto-movable 参数产生：按全局/NUMA MOVABLE:KERNEL_EARLY 预算选择 zone。 */
	ONLINE_POLICY_AUTO_MOVABLE,
};

static const char * const online_policy_to_str[] = {
	/* 枚举值也是数组下标；set/get 回调共享这一双向映射。 */
	[ONLINE_POLICY_CONTIG_ZONES] = "contig-zones",
	[ONLINE_POLICY_AUTO_MOVABLE] = "auto-movable",
};

/*
 * 业务背景：运行期模块参数写入为未显式指定 zone 的 MMOP_ONLINE 请求选择策略；
 * 参数：val 是借用策略字符串；kp->arg 指向静态 online_policy，均由参数框架保证非空；
 * 返回：匹配 contig-zones/auto-movable 后返回 0 并发布下标，否则返回匹配 errno；
 * 注意事项：只接受表内字符串，失败无状态变化；参数框架串行写，本函数不拥有输入对象。
 */
static int set_online_policy(const char *val, const struct kernel_param *kp)
{
	int ret = sysfs_match_string(online_policy_to_str, val);

	if (ret < 0)
		return ret;
	*((int *)kp->arg) = ret;
	return 0;
}

/*
 * 业务背景：用户读取 online_policy 时需要从内部枚举下标恢复稳定策略名；
 * 参数：buffer 是框架可写区，kp->arg 借用静态合法下标，均不可空；
 * 返回：包含换行的输出字符数，无 ownership 或策略状态变化；
 * 注意事项：下标只能由匹配器写入，故无需越界回退；与参数写并发由框架串行。
 */
static int get_online_policy(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "%s\n", online_policy_to_str[*((int *)kp->arg)]);
}

/*
 * memory_hotplug.online_policy: configure online behavior when onlining without
 * specifying a zone (MMOP_ONLINE)
 *
 * "contig-zones": keep zone contiguous
 * "auto-movable": online memory to ZONE_MOVABLE if the configuration
 *                 (auto_movable_ratio, auto_movable_numa_aware) allows for it
 * 中文：未指定目标 zone 的上线请求，可保持 zone 连续，也可在比例允许时选择
 * ZONE_MOVABLE；显式 ONLINE_KERNEL/MOVABLE 不受本策略覆盖。
 */
static int online_policy __read_mostly = ONLINE_POLICY_CONTIG_ZONES;
static const struct kernel_param_ops online_policy_ops = {
	.set = set_online_policy,
	.get = get_online_policy,
};
module_param_cb(online_policy, &online_policy_ops, &online_policy, 0644);
MODULE_PARM_DESC(online_policy,
		"Set the online policy (\"contig-zones\", \"auto-movable\") "
		"Default: \"contig-zones\"");

/*
 * memory_hotplug.auto_movable_ratio: specify maximum MOVABLE:KERNEL ratio
 *
 * The ratio represent an upper limit and the kernel might decide to not
 * online some memory to ZONE_MOVABLE -- e.g., because hotplugged KERNEL memory
 * doesn't allow for more MOVABLE memory.
 * 中文：该百分比是 MOVABLE/KERNEL 的上界；已有可热拔的 kernel 内存不会扩大
 * 可上线为 MOVABLE 的额度，避免设备独立拔出后留下失衡 zone。
 */
static unsigned int auto_movable_ratio __read_mostly = 301;
module_param(auto_movable_ratio, uint, 0644);
MODULE_PARM_DESC(auto_movable_ratio,
		"Set the maximum ratio of MOVABLE:KERNEL memory in the system "
		"in percent for \"auto-movable\" online policy. Default: 301");

/*
 * memory_hotplug.auto_movable_numa_aware: consider numa node stats
 * 中文：启用时除全局比例外，还要求目标 NUMA 节点自己的比例也合格。
 */
#ifdef CONFIG_NUMA
static bool auto_movable_numa_aware __read_mostly = true;
module_param(auto_movable_numa_aware, bool, 0644);
MODULE_PARM_DESC(auto_movable_numa_aware,
		"Consider numa node stats in addition to global stats in "
		"\"auto-movable\" online policy. Default: true");
#endif /* CONFIG_NUMA */

/*
 * online_page_callback contains pointer to current page onlining function.
 * Initially it is generic_online_page(). If it is required it could be
 * changed by calling set_online_page_callback() for callback registration
 * and restore_online_page_callback() for generic callback restore.
 * 中文：online_page_callback 决定上线页最终如何交给分配器；注册和恢复必须串行，
 * mem_hotplug_lock 的读侧还会与整个上线/下线事务的写侧互斥。
 */

static online_page_callback_t online_page_callback = generic_online_page;
static DEFINE_MUTEX(online_page_callback_lock);

DEFINE_STATIC_PERCPU_RWSEM(mem_hotplug_lock);

/*
 * 业务背景：set/restore_online_page_callback() 修改回调前要阻止 online/offline 写事务使用旧指针；
 * 参数：无；锁对象是全局 mem_hotplug_lock，不涉及对象 ownership；
 * 返回：void，但返回时调用者持有 percpu rwsem 读锁，后续必须调用 put_online_mems()；
 * 注意事项：进程上下文可睡眠，不能在原子上下文使用；锁序须早于 callback mutex。
 */
void get_online_mems(void)
{
	percpu_down_read(&mem_hotplug_lock);
}

/*
 * 业务背景：回调指针完成读取/更新后要结束与内存热插拔写事务的互斥；
 * 参数：无，隐含前置条件是当前执行流已成功调用 get_online_mems()；
 * 返回：void；释放读锁后等待的 add/remove 写事务可以继续；
 * 注意事项：必须一一配对且位于 callback mutex 解锁之后，漏调会永久阻塞热插拔。
 */
void put_online_mems(void)
{
	percpu_up_read(&mem_hotplug_lock);
}

bool movable_node_enabled = false;

static int mhp_default_online_type = -1;
/*
 * -1 表示尚未解析；MMOP_OFFLINE 只创建设备，MMOP_ONLINE 交给 online_policy，
 * MMOP_ONLINE_KERNEL/MOVABLE 分别强制 kernel zone/ZONE_MOVABLE。启动参数或
 * Kconfig 产生该值，add_memory_resource() 解锁后消费并驱动 device_online()。
 */
/*
 * 业务背景：新建 memory_block 决定是否及以何种 zone 自动上线时需要统一默认类型；
 * 参数：无，读取启动参数覆盖值或互斥 Kconfig 选择，不取得 memory_block 引用；
 * 返回：合法 MMOP_*；首次调用计算并缓存，之后直接返回缓存值；
 * 注意事项：预期在启动/设备热插拔串行上下文调用；兜底为 MMOP_OFFLINE，函数不执行上线。
 */
enum mmop mhp_get_default_online_type(void)
{
	if (mhp_default_online_type >= 0)
		/* 启动参数或先前查询已完成发布，走高频快路。 */
		return mhp_default_online_type;

	if (IS_ENABLED(CONFIG_MHP_DEFAULT_ONLINE_TYPE_OFFLINE))
		mhp_default_online_type = MMOP_OFFLINE;
	else if (IS_ENABLED(CONFIG_MHP_DEFAULT_ONLINE_TYPE_ONLINE_AUTO))
		mhp_default_online_type = MMOP_ONLINE;
	/* 四个 Kconfig 分支依次表达 offline、策略自动、kernel、movable。 */
	else if (IS_ENABLED(CONFIG_MHP_DEFAULT_ONLINE_TYPE_ONLINE_KERNEL))
		mhp_default_online_type = MMOP_ONLINE_KERNEL;
	else if (IS_ENABLED(CONFIG_MHP_DEFAULT_ONLINE_TYPE_ONLINE_MOVABLE))
		mhp_default_online_type = MMOP_ONLINE_MOVABLE;
	else
		mhp_default_online_type = MMOP_OFFLINE;

	/* 配置互斥由 Kconfig 保证；兜底仍选择最保守的 OFFLINE。 */
	return mhp_default_online_type;
}

/*
 * 业务背景：架构或管理初始化代码需要在自动上线开始前覆盖 Kconfig 默认类型；
 * 参数：online_type 必须是合法 MMOP_* 值，按值传递且无 ownership；
 * 返回：void；后续 mhp_get_default_online_type() 和 add 后 walker 会观察新值；
 * 注意事项：无内部校验/锁，调用者必须保证尚无并发查询，传非法值会破坏 zone 分派协议。
 */
void mhp_set_default_online_type(enum mmop online_type)
{
	mhp_default_online_type = online_type;
}

/*
 * 业务背景：early setup 为 memhp_default_state= 启动参数覆盖默认上线类型；
 * 参数：str 是启动参数解析器借用的 NUL 结尾文本，仅在回调期间有效；
 * 返回：恒 1 表示参数已消费；合法文本发布 MMOP_*，非法文本保持原值；
 * 注意事项：单线程启动期无锁且不得保存 str，返回 1 不代表文本一定合法。
 */
static int __init setup_memhp_default_state(char *str)
{
	const int online_type = mhp_online_type_from_str(str);

	if (online_type >= 0)
		mhp_default_online_type = online_type;

	return 1;
}
__setup("memhp_default_state=", setup_memhp_default_state);

/*
 * 业务背景：add/remove/节点变更必须同时稳定 CPU 拓扑并独占内存热插拔状态；
 * 参数：无，操作全局 cpus_read_lock 与 mem_hotplug_lock，不取得业务对象 ownership；
 * 返回：void；返回时两把锁均已持有，调用者可安全修改 zone/node/memory_block 拓扑；
 * 注意事项：可睡眠且固定“CPU read→memory write”锁序，所有退出路径必须调用 mem_hotplug_done()。
 */
void mem_hotplug_begin(void)
{
	cpus_read_lock();
	percpu_down_write(&mem_hotplug_lock);
}

/*
 * 业务背景：完整热插拔事务提交或回滚后需要退出全局串行区；
 * 参数：无，隐含当前执行流持有 begin 取得的两把锁；
 * 返回：void；先放 memory 写锁再放 CPU read 锁，此后并发拓扑修改可发生；
 * 注意事项：不能漏调、重复调用或交换释放顺序；锁外继续用借用拓扑对象须有其他生命周期保证。
 */
void mem_hotplug_done(void)
{
	percpu_up_write(&mem_hotplug_lock);
	cpus_read_unlock();
}

u64 max_mem_size = U64_MAX;

/* add this memory to iomem resource */
/*
 * 业务背景：__add_memory()/driver-managed 入口在建 memblock/sysfs 前先声明 iomem 所有权；
 * 参数：start/size 是字节半开范围；resource_name 是调用期借用字符串且决定 DRIVER_MANAGED 标志；
 * 返回：成功返回已挂 iomem 树的新 resource；越界/启动期 mem= 限制为 -E2BIG，冲突为 -EEXIST；
 * 注意事项：调用者持 device hotplug 串行；成功对象由上层事务接管，后续失败必须 release。
 */
static struct resource *register_memory_resource(u64 start, u64 size,
						 const char *resource_name)
{
	struct resource *res;
	unsigned long flags =  IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;

	if (strcmp(resource_name, "System RAM"))
		/* 驱动管理资源不能被当作普通固件 System RAM 自动处理。 */
		flags |= IORESOURCE_SYSRAM_DRIVER_MANAGED;

	if (!mhp_range_allowed(start, size, true))
		return ERR_PTR(-E2BIG);

	/*
	 * Make sure value parsed from 'mem=' only restricts memory adding
	 * while booting, so that memory hotplug won't be impacted. Please
	 * refer to document of 'mem=' in kernel-parameters.txt for more
	 * details.
	 * 中文：mem= 只约束启动期探测，系统运行后热插拔仍可使用完整架构范围。
	 */
	if (start + size > max_mem_size && system_state < SYSTEM_RUNNING)
		return ERR_PTR(-E2BIG);

	/*
	 * Request ownership of the new memory range.  This might be
	 * a child of an existing resource that was present but
	 * not marked as busy.
	 * 中文：向 iomem 树取得区间所有权；已有非 busy 父资源时新资源可成为其子项。
	 */
	res = __request_region(&iomem_resource, start, size,
			       resource_name, flags);

	if (!res) {
		/* 冲突时没有资源对象需要释放，调用者据 ERR_PTR 回滚上层步骤。 */
		pr_debug("Unable to reserve System RAM region: %016llx->%016llx\n",
				start, start + size);
		return ERR_PTR(-EEXIST);
	}
	return res;
}

/*
 * 业务背景：add 在架构映射或设备创建失败后必须撤销先前取得的 iomem resource；
 * 参数：res 可为 NULL；非空时由调用者独占并已链接 iomem 树，ownership 将被消费；
 * 返回：void；非空对象先从资源树摘除再 kfree，返回后指针失效；
 * 注意事项：调用者持 device_hotplug_lock；不得传已合并或已释放的 resource，避免双重摘除。
 */
static void release_memory_resource(struct resource *res)
{
	if (!res)
		return;
	release_resource(res);
	kfree(res);
}

/*
 * 业务背景：绕过 memory-block 高层 API 的 arch add/remove 仍必须满足 sparsemem 最小粒度；
 * 参数：pfn 是页号起点，nr_pages 是页数，二者都按值传递且必须 subsection 对齐；
 * 返回：0 表示可继续，-EINVAL 表示对齐错误且没有 sparse/zone 副作用；
 * 注意事项：不检查非零、溢出或用户 block 粒度；这些是更高层调用者契约。
 */
static int check_pfn_span(unsigned long pfn, unsigned long nr_pages)
{
	/*
	 * Disallow all operations smaller than a sub-section.
	 * Note that check_hotplug_memory_range() enforces a larger
	 * memory_block_size_bytes() granularity for memory that will be marked
	 * online, so this check should only fire for direct
	 * arch_{add,remove}_memory() users outside of add_memory_resource().
	 * 中文：用户可上线的内存另有更严格的 memory-block 粒度；这里保卫 sparsemem
	 * 最小 subsection 契约，主要拦截绕过高层 API 的架构调用者。
	 */
	if (!IS_ALIGNED(pfn | nr_pages, PAGES_PER_SUBSECTION))
		return -EINVAL;
	return 0;
}

/*
 * Return page for the valid pfn only if the page is online. All pfn
 * walkers which rely on the fully initialized page->flags and others
 * should use this rather than pfn_valid && pfn_to_page
 * 中文：PFN 遍历者只有在 section 在线、subsection 有效且不是离线 ZONE_DEVICE
 * PFN 时才能取得借用 page；返回 NULL 不代表该物理地址永远不存在。
 */
/*
 * 业务背景：PFN walker 需要排除离线 section、空洞及混合 DEVICE section 中的离线 PFN；
 * 参数：pfn 是单个页号，按值传递；调用者须持 hotplug 同步或能容忍并发状态变化；
 * 返回：在线且已初始化的借用 page，或 NULL；不增加 page 引用；
 * 注意事项：普通 RAM 走 section 快路，混合 DEVICE 需临时 dev_pagemap 引用；返回指针寿命由外部同步保证。
 */
struct page *pfn_to_online_page(unsigned long pfn)
{
	unsigned long nr = pfn_to_section_nr(pfn);
	struct dev_pagemap *pgmap;
	struct mem_section *ms;

	if (nr >= NR_MEM_SECTIONS)
		/* 在解引用 section 表前先封住超出架构最大物理范围的 PFN。 */
		return NULL;

	ms = __nr_to_section(nr);
	if (!online_section(ms))
		return NULL;

	/*
	 * Save some code text when online_section() +
	 * pfn_section_valid() are sufficient.
	 * 中文：具有架构 pfn_valid() 实现时先用其过滤特殊空洞，可减少后续慢路代码。
	 */
	if (IS_ENABLED(CONFIG_HAVE_ARCH_PFN_VALID) && !pfn_valid(pfn))
		return NULL;

	if (!pfn_section_valid(ms, pfn))
		return NULL;

	if (!online_device_section(ms))
		/* 普通 RAM 的在线 section 已足够证明 struct page 可使用。 */
		return pfn_to_page(pfn);

	/*
	 * Slowpath: when ZONE_DEVICE collides with
	 * ZONE_{NORMAL,MOVABLE} within the same section some pfns in
	 * the section may be 'offline' but 'valid'. Only
	 * get_dev_pagemap() can determine sub-section online status.
	 * 中文：同一 section 混合 DEVICE 与普通 zone 时，只有 dev_pagemap 的 subsection
	 * 查询能区分“valid 但离线”的设备 PFN；临时引用在判断前立即归还。
	 */
	pgmap = get_dev_pagemap(pfn);
	put_dev_pagemap(pgmap);

	/* The presence of a pgmap indicates ZONE_DEVICE offline pfn */
	/* 中文：能找到 pgmap 表示该 PFN 属于当前不可作为普通在线页使用的设备区间。 */
	if (pgmap)
		return NULL;

	return pfn_to_page(pfn);
}
EXPORT_SYMBOL_GPL(pfn_to_online_page);

/*
 * 业务背景：各架构 arch_add_memory() 用公共 sparse helper 为物理范围创建 section/vmemmap；
 * 参数：nid 是目标节点；pfn/nr_pages 以页计；params 非空借用，含必需 pgprot 和可空 altmap/pgmap；
 * 返回：0 表示全部 section 已添加；-EINVAL 拒绝保护/altmap/对齐，或返回 sparse_add_section errno；
 * 注意事项：可睡眠并 cond_resched；失败不回滚已添加前缀，调用架构必须用原范围调用 remove 清理。
 */
int __add_pages(int nid, unsigned long pfn, unsigned long nr_pages,
		struct mhp_params *params)
{
	const unsigned long end_pfn = pfn + nr_pages;
	unsigned long cur_nr_pages;
	int err;
	struct vmem_altmap *altmap = params->altmap;

	if (WARN_ON_ONCE(!pgprot_val(params->pgprot)))
		/* 零 pgprot 无法建立有效线性映射，在任何 section 可见前拒绝。 */
		return -EINVAL;

	VM_BUG_ON(!mhp_range_allowed(PFN_PHYS(pfn), nr_pages * PAGE_SIZE, false));

	if (altmap) {
		/*
		 * Validate altmap is within bounds of the total request
		 * 中文：altmap 必须从本次范围起点开始，且自身保留偏移不能越过请求。
		 */
		if (altmap->base_pfn != pfn
				|| vmem_altmap_offset(altmap) > nr_pages) {
			pr_warn_once("memory add fail, invalid altmap\n");
			return -EINVAL;
		}
		altmap->alloc = 0;
		/* 本次 populate 从空分配游标开始；所有权仍属于 params/调用者。 */
	}

	if (check_pfn_span(pfn, nr_pages)) {
		WARN(1, "Misaligned %s start: %#lx end: %#lx\n", __func__, pfn, pfn + nr_pages - 1);
		return -EINVAL;
	}

	for (; pfn < end_pfn; pfn += cur_nr_pages) {
		/* Select all remaining pages up to the next section boundary */
		/* 中文：每次只处理到下一个 section 边界，允许首尾是 subsection。 */
		cur_nr_pages = min(end_pfn - pfn,
				   SECTION_ALIGN_UP(pfn + 1) - pfn);
		err = sparse_add_section(nid, pfn, cur_nr_pages, altmap,
					 params->pgmap);
		if (err)
			/* 已成功的前缀保持存在，上层知道原始范围并负责 remove。 */
			break;
		cond_resched();
	}
	vmemmap_populate_print_last();
	return err;
}

/* find the smallest valid pfn in the range [start_pfn, end_pfn) */
/*
 * 业务背景：删除 zone 首端后，shrink_zone_span() 要寻找新的最小有效页来重建跨度；
 * 参数：nid 和 zone 是必须同时匹配的借用归属；start_pfn/end_pfn 是页单位半开区间；
 * 返回：首个在线且归属匹配的 PFN，0 表示范围内没有候选；
 * 注意事项：调用者持 hotplug 写锁；按 subsection 步进，PFN 0 作为“未找到”哨兵依赖热插拔范围约束。
 */
static unsigned long find_smallest_section_pfn(int nid, struct zone *zone,
				     unsigned long start_pfn,
				     unsigned long end_pfn)
{
	for (; start_pfn < end_pfn; start_pfn += PAGES_PER_SUBSECTION) {
		/* subsection 是可能独立存在的最小扫描步长，不能只检查 section 首页。 */
		if (unlikely(!pfn_to_online_page(start_pfn)))
			continue;

		if (unlikely(pfn_to_nid(start_pfn) != nid))
			continue;

		if (zone != page_zone(pfn_to_page(start_pfn)))
			/* nid 相同仍可能属于另一 zone，两个条件都必须成立。 */
			continue;

		return start_pfn;
	}

	return 0;
}

/* find the biggest valid pfn in the range [start_pfn, end_pfn). */
/*
 * 业务背景：删除 zone 尾端后，shrink_zone_span() 要寻找剩余范围的最大有效页；
 * 参数：nid/zone 是借用归属，start_pfn/end_pfn 是页单位半开区间且 start 小于 end；
 * 返回：最后一个在线且归属匹配的 PFN，0 表示没有候选；
 * 注意事项：调用者持 hotplug 写锁；逆向 unsigned PFN 扫描依赖对齐区间避免下溢成为有效迭代。
 */
static unsigned long find_biggest_section_pfn(int nid, struct zone *zone,
				    unsigned long start_pfn,
				    unsigned long end_pfn)
{
	unsigned long pfn;

	/* pfn is the end pfn of a memory section. */
	/* 中文：半开区间的最后一个 PFN 是 end_pfn - 1，随后按 subsection 逆向扫描。 */
	pfn = end_pfn - 1;
	for (; pfn >= start_pfn; pfn -= PAGES_PER_SUBSECTION) {
		if (unlikely(!pfn_to_online_page(pfn)))
			continue;

		if (unlikely(pfn_to_nid(pfn) != nid))
			/* section 在线但归属其他节点时不能用来界定当前 pgdat。 */
			continue;

		if (zone != page_zone(pfn_to_page(pfn)))
			continue;

		return pfn;
	}

	return 0;
}

/*
 * 业务背景：offline 摘除边界 section 后要让 zone span 继续包住所有剩余在线 subsection；
 * 参数：zone 是借用目标；start_pfn/end_pfn 是已移除的页单位半开区间；
 * 返回：void；首/尾删除会更新 zone_start_pfn/spanned_pages，完全为空则清零；
 * 注意事项：调用者持 hotplug 写锁；中间空洞仍属于 span，本函数不改 present/managed 计数。
 */
static void shrink_zone_span(struct zone *zone, unsigned long start_pfn,
			     unsigned long end_pfn)
{
	unsigned long pfn;
	int nid = zone_to_nid(zone);

	if (zone->zone_start_pfn == start_pfn) {
		/*
		 * If the section is smallest section in the zone, it need
		 * shrink zone->zone_start_pfn and zone->zone_spanned_pages.
		 * In this case, we find second smallest valid mem_section
		 * for shrinking zone.
		 * 中文：删除当前最小 section 时，从其后寻找新的最小有效 PFN，并同步
		 * zone_start_pfn 与 spanned_pages；完全为空则把两者清零。
		 */
		pfn = find_smallest_section_pfn(nid, zone, end_pfn,
						zone_end_pfn(zone));
		if (pfn) {
			/* zone_end_pfn 仍是旧尾端，先据此计算新跨度再移动起点。 */
			zone->spanned_pages = zone_end_pfn(zone) - pfn;
			zone->zone_start_pfn = pfn;
		} else {
			zone->zone_start_pfn = 0;
			zone->spanned_pages = 0;
		}
	} else if (zone_end_pfn(zone) == end_pfn) {
		/*
		 * If the section is biggest section in the zone, it need
		 * shrink zone->spanned_pages.
		 * In this case, we find second biggest valid mem_section for
		 * shrinking zone.
		 * 中文：删除尾端时起点不变，只需寻找删除区间之前仍有效的最大 PFN。
		 */
		pfn = find_biggest_section_pfn(nid, zone, zone->zone_start_pfn,
					       start_pfn);
		if (pfn)
			zone->spanned_pages = pfn - zone->zone_start_pfn + 1;
		else {
			zone->zone_start_pfn = 0;
			zone->spanned_pages = 0;
		}
	}
}

/*
 * 业务背景：单个 zone 收缩后，pgdat 的节点总跨度必须重新覆盖所有非空 zone；
 * 参数：pgdat 是长寿借用节点描述符，node_zones 在 hotplug 写锁期间稳定；
 * 返回：void；重写 node_start_pfn/node_spanned_pages，节点无 zone 时清零；
 * 注意事项：调用者持 hotplug 写锁；只汇总页跨度，不修改 present/managed 或节点状态位。
 */
static void update_pgdat_span(struct pglist_data *pgdat)
{
	unsigned long node_start_pfn = 0, node_end_pfn = 0;
	struct zone *zone;

	for (zone = pgdat->node_zones;
	     zone < pgdat->node_zones + MAX_NR_ZONES; zone++) {
		unsigned long end_pfn = zone_end_pfn(zone);

		/* No need to lock the zones, they can't change. */
		/* 中文：全局热插拔写锁使 zone 边界在本次遍历期间保持稳定。 */
		if (!zone->spanned_pages)
			continue;
		if (!node_end_pfn) {
			/* 首个非空 zone 初始化聚合区间，后续只向两端扩展。 */
			node_start_pfn = zone->zone_start_pfn;
			node_end_pfn = end_pfn;
			continue;
		}

		if (end_pfn > node_end_pfn)
			node_end_pfn = end_pfn;
		if (zone->zone_start_pfn < node_start_pfn)
			node_start_pfn = zone->zone_start_pfn;
	}

	pgdat->node_start_pfn = node_start_pfn;
	/* 没有非空 zone 时两个聚合边界都为 0，节点跨度自然清零。 */
	pgdat->node_spanned_pages = node_end_pfn - node_start_pfn;
}

/*
 * 业务背景：offline 或 self-host 元数据撤销后，要把页描述符恢复为未初始化并修正拓扑跨度；
 * 参数：zone 是借用归属；start_pfn 是页号，nr_pages 是页数，范围必须已不可分配；
 * 返回：void；毒化全部 struct page，普通 zone 同时收缩 zone/pgdat span；
 * 注意事项：调用者持 hotplug 写锁且可睡眠；ZONE_DEVICE 只 poison，不收缩不受支持的 span。
 */
void remove_pfn_range_from_zone(struct zone *zone,
				      unsigned long start_pfn,
				      unsigned long nr_pages)
{
	const unsigned long end_pfn = start_pfn + nr_pages;
	struct pglist_data *pgdat = zone->zone_pgdat;
	unsigned long pfn, cur_nr_pages;

	/* Poison struct pages because they are now uninitialized again. */
	/* 中文：移除后 struct page 回到未初始化状态，毒化可尽早暴露陈旧引用。 */
	for (pfn = start_pfn; pfn < end_pfn; pfn += cur_nr_pages) {
		cond_resched();

		/* Select all remaining pages up to the next section boundary */
		/* 中文：分段避免一次跨越 sparse section 的 memmap 边界。 */
		cur_nr_pages =
			min(end_pfn - pfn, SECTION_ALIGN_UP(pfn + 1) - pfn);
		page_init_poison(pfn_to_page(pfn),
				 sizeof(struct page) * cur_nr_pages);
	}

	/*
	 * Zone shrinking code cannot properly deal with ZONE_DEVICE. So
	 * we will not try to shrink the zones - which is okay as
	 * set_zone_contiguous() cannot deal with ZONE_DEVICE either way.
	 * 中文：设备 zone 不参与普通 zone 连续性模型，因此只完成 poison 后返回。
	 */
	if (zone_is_zone_device(zone))
		return;

	clear_zone_contiguous(zone);
	/* 边界修改期间先撤销连续标记，最后依据新 span 重新计算。 */

	shrink_zone_span(zone, start_pfn, start_pfn + nr_pages);
	update_pgdat_span(pgdat);

	set_zone_contiguous(zone);
}

/**
 * __remove_pages() - remove sections of pages
 * @pfn: starting pageframe (must be aligned to start of a section)
 * @nr_pages: number of pages to remove (must be multiple of section size)
 * @altmap: alternative device page map or %NULL if default memmap is used
 * @pgmap: device page map or %NULL if not ZONE_DEVICE
 *
 * Generic helper function to remove section mappings and sysfs entries
 * for the section of the memory we are removing. Caller needs to make
 * sure that pages are marked reserved and zones are adjust properly by
 * calling offline_pages().
 *
 * 中文：架构移除映射的底层 helper。调用者必须先 offline，使页保留且 zone 计数
 * 已调整；本函数仅按 subsection/section 拆 sparse 映射，不拥有 altmap/pgmap。
 * 业务背景：arch_remove_memory() 和 ZONE_DEVICE teardown 共用此 helper 拆 sparse section；
 * 参数：pfn/nr_pages 以页计并须 subsection 对齐；altmap/pgmap 可空且仅借用、不在此释放；
 * 返回：void；对齐错误仅 WARN 并保持映射，合法范围逐 section 移除；
 * 注意事项：页必须已 Reserved/offline 且 zone 已调整；可 cond_resched，调用者负责对象最终释放。
 */
void __remove_pages(unsigned long pfn, unsigned long nr_pages,
		    struct vmem_altmap *altmap, struct dev_pagemap *pgmap)
{
	const unsigned long end_pfn = pfn + nr_pages;
	unsigned long cur_nr_pages;

	if (check_pfn_span(pfn, nr_pages)) {
		/* void API 用 WARN 暴露调用者契约错误，并在任何拆除前退出。 */
		WARN(1, "Misaligned %s start: %#lx end: %#lx\n", __func__, pfn, pfn + nr_pages - 1);
		return;
	}

	for (; pfn < end_pfn; pfn += cur_nr_pages) {
		cond_resched();
		/* Select all remaining pages up to the next section boundary */
		/* 中文：逐 section 拆除并允许调度，避免大范围移除长期占用 CPU。 */
		cur_nr_pages = min(end_pfn - pfn,
				   SECTION_ALIGN_UP(pfn + 1) - pfn);
		sparse_remove_section(pfn, cur_nr_pages, altmap, pgmap);
	}
}

/*
 * 业务背景：某些子系统要接管新上线页，而非立即由 generic 回调交给 buddy；
 * 参数：callback 是注册者拥有的长寿函数指针，不可空且必须活到 restore 完成；
 * 返回：仅当前回调仍为 generic 时安装并返回 0，否则返回 -EINVAL 且不改指针；
 * 注意事项：可睡眠；先取 hotplug 读锁再取 callback mutex，防上线写事务观察半更新状态。
 */
int set_online_page_callback(online_page_callback_t callback)
{
	int rc = -EINVAL;

	get_online_mems();
	mutex_lock(&online_page_callback_lock);

	if (online_page_callback == generic_online_page) {
		/* callback 由注册者保证长寿，核心只保存借用函数指针。 */
		online_page_callback = callback;
		rc = 0;
	}

	mutex_unlock(&online_page_callback_lock);
	put_online_mems();

	return rc;
}
EXPORT_SYMBOL_GPL(set_online_page_callback);

/*
 * 业务背景：自定义回调的注册模块卸载前必须恢复 generic，避免留下悬空函数指针；
 * 参数：callback 是原注册值，仅借用作身份比较，调用期间必须仍有效；
 * 返回：当前指针匹配时恢复并返回 0，否则 -EINVAL 且不撤销他人的回调；
 * 注意事项：可睡眠且锁序与 set 相同；成功返回后等待的上线事务只会看到 generic。
 */
int restore_online_page_callback(online_page_callback_t callback)
{
	int rc = -EINVAL;

	get_online_mems();
	mutex_lock(&online_page_callback_lock);

	if (online_page_callback == callback) {
		/* 比较避免一个模块误撤销另一个模块安装的回调。 */
		online_page_callback = generic_online_page;
		rc = 0;
	}

	mutex_unlock(&online_page_callback_lock);
	put_online_mems();

	return rc;
}
EXPORT_SYMBOL_GPL(restore_online_page_callback);

/* we are OK calling __meminit stuff here - we have CONFIG_MEMORY_HOTPLUG */
/* 中文：启用热插拔时 __meminit 代码不会在启动后丢弃，可安全从运行期路径调用。 */
/*
 * 业务背景：没有专用消费者时，online_pages_range() 要把初始化完的热添加页交给 buddy；
 * 参数：page 是连续块首页的借用指针，order 表示 2^order 页并受 MAX_PAGE_ORDER 限制；
 * 返回：void；__free_pages_core 后整块 ownership 转给页分配器；
 * 注意事项：在 hotplug 写事务中调用，页须尚未在 buddy；重复调用会造成双重释放。
 */
void generic_online_page(struct page *page, unsigned int order)
{
	__free_pages_core(page, order, MEMINIT_HOTPLUG);
}
EXPORT_SYMBOL_GPL(generic_online_page);

/*
 * 业务背景：online_pages() 提交时要按大阶批量处理新页并在最后发布 section online；
 * 参数：start_pfn/nr_pages 均以页计，范围已绑定 zone 且页仍 Offline/隔离；
 * 返回：void；逐块调用当前 online_page_callback，最后把涉及 section 标为在线；
 * 注意事项：调用者持 hotplug 写锁；回调可延迟交 buddy，但不能失败，section 发布必须晚于全部回调。
 */
static void online_pages_range(unsigned long start_pfn, unsigned long nr_pages)
{
	const unsigned long end_pfn = start_pfn + nr_pages;
	unsigned long pfn;

	/*
	 * Online the pages in MAX_PAGE_ORDER aligned chunks. The callback might
	 * decide to not expose all pages to the buddy (e.g., expose them
	 * later). We account all pages as being online and belonging to this
	 * zone ("present").
	 * When using memmap_on_memory, the range might not be aligned to
	 * MAX_ORDER_NR_PAGES - 1, but pageblock aligned. __ffs() will detect
	 * this and the first chunk to online will be pageblock_nr_pages.
	 * 中文：按 PFN 对齐选择最大块可减少 free 次数；memmap-on-memory 的首段只保证
	 * pageblock 对齐，因此首个 order 可能小于 MAX_PAGE_ORDER。
	 */
	for (pfn = start_pfn; pfn < end_pfn;) {
		struct page *page = pfn_to_page(pfn);
		int order;

		/*
		 * Free to online pages in the largest chunks alignment allows.
		 *
		 * __ffs() behaviour is undefined for 0. start == 0 is
		 * MAX_PAGE_ORDER-aligned, Set order to MAX_PAGE_ORDER for
		 * the case.
		 * 中文：__ffs(0) 未定义，PFN 0 单独视为满足最大阶对齐。
		 */
		if (pfn)
			order = min_t(int, MAX_PAGE_ORDER, __ffs(pfn));
		else
			order = MAX_PAGE_ORDER;

		/*
		 * Exposing the page to the buddy by freeing can cause
		 * issues with debug_pagealloc enabled: some archs don't
		 * like double-unmappings. So treat them like any pages that
		 * were allocated from the buddy.
		 * 中文：先恢复 debug_pagealloc 映射，再调用回调释放，避免架构重复 unmap。
		 */
		debug_pagealloc_map_pages(page, 1 << order);
		(*online_page_callback)(page, order);
		pfn += (1UL << order);
	}

	/* mark all involved sections as online */
	/* 中文：所有页处理完成后才发布 section 在线状态，读者不会看到半提交区间。 */
	online_mem_sections(start_pfn, end_pfn);
}

/*
 * 业务背景：move_pfn_range_to_zone() 初始化 page 前必须让 zone span 先覆盖目标 PFN；
 * 参数：zone 是借用目标，start_pfn/nr_pages 是页单位范围；
 * 返回：void；必要时向左/右扩 zone_start_pfn/spanned_pages，不改 present 计数；
 * 注意事项：调用者持 hotplug 写锁，zone 尚无并发边界修改；先扩 span 是 pageblock helper 的前置条件。
 */
static void __meminit resize_zone_range(struct zone *zone, unsigned long start_pfn,
		unsigned long nr_pages)
{
	unsigned long old_end_pfn = zone_end_pfn(zone);

	if (zone_is_empty(zone) || start_pfn < zone->zone_start_pfn)
		/* 空 zone 或向左扩展时更新起点，尾端由 max 保留原有范围。 */
		zone->zone_start_pfn = start_pfn;

	zone->spanned_pages = max(start_pfn + nr_pages, old_end_pfn) - zone->zone_start_pfn;
}

/*
 * 业务背景：zone 扩展时所属 pgdat 的总 span 也必须覆盖新增物理页；
 * 参数：pgdat 是借用节点对象，start_pfn/nr_pages 是页单位范围；
 * 返回：void；扩 node_start_pfn/node_spanned_pages，不改 present/managed；
 * 注意事项：调用者持 hotplug 写锁；只允许扩张，收缩由 update_pgdat_span() 重新汇总。
 */
static void __meminit resize_pgdat_range(struct pglist_data *pgdat, unsigned long start_pfn,
                                     unsigned long nr_pages)
{
	unsigned long old_end_pfn = pgdat_end_pfn(pgdat);

	if (!pgdat->node_spanned_pages || start_pfn < pgdat->node_start_pfn)
		/* 首段内存建立节点起点，后续添加只向外扩 span。 */
		pgdat->node_start_pfn = start_pfn;

	pgdat->node_spanned_pages = max(start_pfn + nr_pages, old_end_pfn) - pgdat->node_start_pfn;

}

#ifdef CONFIG_ZONE_DEVICE
/*
 * 业务背景：ZONE_DEVICE 与普通 zone 混居一个 section 时，在线查询不能只看 section 位；
 * 参数：pfn 是非 section 对齐的设备范围边界页号；
 * 返回：void；在所属 mem_section 设置永久 DEVICE taint，使 PFN 查询走 dev_pagemap 慢路；
 * 注意事项：调用者持 hotplug 写锁；只设置不清除，section_mem_map 生命周期长于本次设备映射。
 */
static void section_taint_zone_device(unsigned long pfn)
{
	struct mem_section *ms = __pfn_to_section(pfn);

	ms->section_mem_map |= SECTION_TAINT_ZONE_DEVICE;
}
#else
/*
 * 业务背景：无 ZONE_DEVICE 配置仍保留 move_pfn_range_to_zone() 的统一源码调用点；
 * 参数：pfn 按值接收但不使用；
 * 返回：void，无任何状态变化；
 * 注意事项：条件编译空桩，不需要锁，也不能据其推断 section 已设置 taint。
 */
static inline void section_taint_zone_device(unsigned long pfn)
{
}
#endif

/*
 * Associate the pfn range with the given zone, initializing the memmaps
 * and resizing the pgdat/zone data to span the added pages. After this
 * call, all affected pages are PageOffline().
 *
 * All aligned pageblocks are initialized to the specified migratetype
 * (usually MIGRATE_MOVABLE). Besides setting the migratetype, no related
 * zone stats (e.g., nr_isolate_pageblock) are touched.
 * 中文：把新增 PFN 绑定到目标 zone，先扩展 zone/pgdat span，再初始化每个
 * struct page 与 pageblock migratetype；返回时页仍 PageOffline，尚未交给 buddy。
 * 业务背景：online/ZONE_DEVICE add 在 sparse 页存在后，要建立 page→zone 归属和迁移类型；
 * 参数：zone 为借用目标；start_pfn/nr_pages 以页计；altmap 可空借用；migratetype 为 pageblock 类型；
 * 返回：void；扩展 span 并初始化 memmap，返回时页仍 PageOffline，ownership 未交 buddy；
 * 注意事项：调用者持 hotplug 写锁；isolate_pageblock 决定初始隔离，短暂先扩 span 后初始化是安全窗口。
 */
void move_pfn_range_to_zone(struct zone *zone, unsigned long start_pfn,
				  unsigned long nr_pages,
				  struct vmem_altmap *altmap, int migratetype,
				  bool isolate_pageblock)
{
	struct pglist_data *pgdat = zone->zone_pgdat;
	int nid = pgdat->node_id;

	clear_zone_contiguous(zone);
	/* span 与 memmap 初始化期间连续性结论暂时无效，完成后再重建。 */

	if (zone_is_empty(zone))
		init_currently_empty_zone(zone, start_pfn, nr_pages);
	resize_zone_range(zone, start_pfn, nr_pages);
	resize_pgdat_range(pgdat, start_pfn, nr_pages);

	/*
	 * Subsection population requires care in pfn_to_online_page().
	 * Set the taint to enable the slow path detection of
	 * ZONE_DEVICE pages in an otherwise  ZONE_{NORMAL,MOVABLE}
	 * section.
	 * 中文：首尾非 section 对齐的设备区间会污染所在 section，强制在线查询慢路。
	 */
	if (zone_is_zone_device(zone)) {
		if (!IS_ALIGNED(start_pfn, PAGES_PER_SECTION))
			section_taint_zone_device(start_pfn);
		if (!IS_ALIGNED(start_pfn + nr_pages, PAGES_PER_SECTION))
			section_taint_zone_device(start_pfn + nr_pages);
	}

	/*
	 * TODO now we have a visible range of pages which are not associated
	 * with their zone properly. Not nice but set_pfnblock_migratetype()
	 * expects the zone spans the pfn range. All the pages in the range
	 * are reserved so nobody should be touching them so we should be safe
	 * 中文：span 必须先覆盖 PFN 才能设置 pageblock 类型；短暂可见的 struct page
	 * 尚为 Reserved/Offline，普通分配者无法取得，因此不会观察错误 zone 归属。
	 */
	memmap_init_range(nr_pages, nid, zone_idx(zone), start_pfn, 0,
			 MEMINIT_HOTPLUG, altmap, migratetype,
			 isolate_pageblock);
	/* altmap 只提供元数据存储，页的最终 zone 已由 zone_idx 写入。 */

	set_zone_contiguous(zone);
}

struct auto_movable_stats {
	/* 页数；启动即存在的 kernel-zone 页，hotplug 写锁期聚合，是比例分母。 */
	unsigned long kernel_early_pages;
	/* 页数；ZONE_MOVABLE 与等价 CMA 页之和，仅在一次策略判断的栈对象内有效。 */
	unsigned long movable_pages;
};

/*
 * 业务背景：auto-movable 比例判断要把单个 zone 归入 MOVABLE 分子或 early-kernel 分母；
 * 参数：stats 是调用者栈上零初始化的输入/输出累加器；zone 是锁期借用且统计稳定；
 * 返回：void；累加页数，CMA 从 kernel 扣除并加入 MOVABLE；
 * 注意事项：调用者持 hotplug 串行；字段单位均为页，不读取动态 managed_pages。
 */
static void auto_movable_stats_account_zone(struct auto_movable_stats *stats,
					    struct zone *zone)
{
	if (zone_idx(zone) == ZONE_MOVABLE) {
		/* present 而非 managed 能避免 balloon 等运行期变化扭曲容量上限。 */
		stats->movable_pages += zone->present_pages;
	} else {
		stats->kernel_early_pages += zone->present_early_pages;
#ifdef CONFIG_CMA
		/*
		 * CMA pages (never on hotplugged memory) behave like
		 * ZONE_MOVABLE.
		 * 中文：CMA 虽位于 kernel zone，却只能承载可迁移分配，比例上视同 MOVABLE。
		 */
		stats->movable_pages += zone->cma_pages;
		stats->kernel_early_pages -= zone->cma_pages;
#endif /* CONFIG_CMA */
	}
}
struct auto_movable_group_stats {
	/* 页数；被遍历动态组已有的 present_movable_pages 总和。 */
	unsigned long movable_pages;
	/* 页数；这些组维持配置比例仍需独占的 early-kernel 预算总和。 */
	unsigned long req_kernel_early_pages;
};

/*
 * 业务背景：当前动态组申请 MOVABLE 前，要预留其他组已有分子及维持比例所需分母；
 * 参数：group 是 walker 借用项；arg 指向栈上 auto_movable_group_stats 输出累加器；
 * 返回：恒 0 继续遍历；ratio 为 0 时跳过，其他情况累计页数；
 * 注意事项：调用者排除当前组并持 hotplug 串行；READ_ONCE 只防编译器重复读取，不支持并发改策略。
 */
static int auto_movable_stats_account_group(struct memory_group *group,
					   void *arg)
{
	const int ratio = READ_ONCE(auto_movable_ratio);
	struct auto_movable_group_stats *stats = arg;
	long pages;

	/*
	 * We don't support modifying the config while the auto-movable online
	 * policy is already enabled. Just avoid the division by zero below.
	 * 中文：运行期把比例改成 0 不受支持；这里只防止除零并忽略该组。
	 */
	if (!ratio)
		return 0;

	/*
	 * Calculate how many early kernel pages this group requires to
	 * satisfy the configured zone ratio.
	 * 中文：用组内 MOVABLE 反推所需分母，再扣除组自身已有 kernel 页。
	 */
	pages = group->present_movable_pages * 100 / ratio;
	pages -= group->present_kernel_pages;

	if (pages > 0)
		/* 只有缺口需要占用全局 early-kernel 预算，富余不跨组转让。 */
		stats->req_kernel_early_pages += pages;
	stats->movable_pages += group->present_movable_pages;
	return 0;
}

/*
 * 业务背景：auto_movable_zone_for_pfn() 必须在提交前预测候选页是否会突破 MOVABLE:KERNEL_EARLY；
 * 参数：nid 为 NUMA_NO_NODE 时查全局、否则查单节点；group 可空借用；nr_pages 是候选页数；
 * 返回：加入候选后仍不超过比例返回 true，否则 false；不修改 zone/group；
 * 注意事项：调用者持 hotplug 串行；动态组只可使用同组 kernel 贡献，其他组预算先行扣除。
 */
static bool auto_movable_can_online_movable(int nid, struct memory_group *group,
					    unsigned long nr_pages)
{
	unsigned long kernel_early_pages, movable_pages;
	struct auto_movable_group_stats group_stats = {};
	struct auto_movable_stats stats = {};
	struct zone *zone;
	int i;

	/* Walk all relevant zones and collect MOVABLE vs. KERNEL stats. */
	/* 中文：NUMA_NO_NODE 表示全局检查，否则只聚合目标节点的 zone。 */
	if (nid == NUMA_NO_NODE) {
		/* TODO: cache values */
		/* 中文：当前直接遍历所有 zone，结果受 hotplug 串行保证为一致快照。 */
		for_each_populated_zone(zone)
			auto_movable_stats_account_zone(&stats, zone);
	} else {
		for (i = 0; i < MAX_NR_ZONES; i++) {
			/* NODE_DATA 在节点初始化后长寿；循环只读取各 zone 的 present 快照。 */
			pg_data_t *pgdat = NODE_DATA(nid);

			zone = pgdat->node_zones + i;
			if (populated_zone(zone))
				auto_movable_stats_account_zone(&stats, zone);
		}
	}

	kernel_early_pages = stats.kernel_early_pages;
	/* 下面先扣除其他动态组的已承诺额度，再判断当前请求。 */
	movable_pages = stats.movable_pages;

	/*
	 * Kernel memory inside dynamic memory group allows for more MOVABLE
	 * memory within the same group. Remove the effect of all but the
	 * current group from the stats.
	 * 中文：动态组内部的 kernel 页能为同一设备的 MOVABLE 页背书；其他组必须先
	 * 保留各自所需 early-kernel 份额，防止跨设备热拔破坏比例。
	 */
	walk_dynamic_memory_groups(nid, auto_movable_stats_account_group,
				   group, &group_stats);
	if (kernel_early_pages <= group_stats.req_kernel_early_pages)
		/* 连既有组的最低承诺都无法覆盖，新 MOVABLE 请求必然拒绝。 */
		return false;
	kernel_early_pages -= group_stats.req_kernel_early_pages;
	movable_pages -= group_stats.movable_pages;

	if (group && group->is_dynamic)
		kernel_early_pages += group->present_kernel_pages;

	/*
	 * Test if we could online the given number of pages to ZONE_MOVABLE
	 * and still stay in the configured ratio.
	 * 中文：把候选页先加入分子，以整数算术检查配置百分比上界。
	 */
	movable_pages += nr_pages;
	return movable_pages <= (auto_movable_ratio * kernel_early_pages) / 100;
}

/*
 * Returns a default kernel memory zone for the given pfn range.
 * If no kernel zone covers this pfn range it will automatically go
 * to the ZONE_NORMAL.
 * 中文：显式/回退 kernel 上线优先继承与范围相交的 DMA 类 zone，否则使用 NORMAL。
 */
/*
 * 业务背景：显式 ONLINE_KERNEL 或自动策略回退时，需要选择能保持 kernel zone 连续的目标；
 * 参数：nid 指定已初始化 pgdat；start_pfn/nr_pages 是候选页范围；
 * 返回：与范围相交的首个 NORMAL 以下 zone，否则返回 ZONE_NORMAL；均为长寿借用指针；
 * 注意事项：调用者持 hotplug 串行且随后才扩 span；函数不验证范围是否跨越多个既有 zone。
 */
static struct zone *default_kernel_zone_for_pfn(int nid, unsigned long start_pfn,
		unsigned long nr_pages)
{
	struct pglist_data *pgdat = NODE_DATA(nid);
	int zid;

	for (zid = 0; zid < ZONE_NORMAL; zid++) {
		/* 只查 NORMAL 以下的架构 kernel zone，NORMAL 自身是最终兜底。 */
		struct zone *zone = &pgdat->node_zones[zid];

		if (zone_intersects(zone, start_pfn, nr_pages))
			return zone;
	}

	return &pgdat->node_zones[ZONE_NORMAL];
}

/*
 * Determine to which zone to online memory dynamically based on user
 * configuration and system stats. We care about the following ratio:
 *
 *   MOVABLE : KERNEL
 *
 * Whereby MOVABLE is memory in ZONE_MOVABLE and KERNEL is memory in
 * one of the kernel zones. CMA pages inside one of the kernel zones really
 * behaves like ZONE_MOVABLE, so we treat them accordingly.
 *
 * We don't allow for hotplugged memory in a KERNEL zone to increase the
 * amount of MOVABLE memory we can have, so we end up with:
 *
 *   MOVABLE : KERNEL_EARLY
 *
 * Whereby KERNEL_EARLY is memory in one of the kernel zones, available since
 * boot. We base our calculation on KERNEL_EARLY internally, because:
 *
 * a) Hotplugged memory in one of the kernel zones can sometimes still get
 *    hotunplugged, especially when hot(un)plugging individual memory blocks.
 *    There is no coordination across memory devices, therefore "automatic"
 *    hotunplugging, as implemented in hypervisors, could result in zone
 *    imbalances.
 * b) Early/boot memory in one of the kernel zones can usually not get
 *    hotunplugged again (e.g., no firmware interface to unplug, fragmented
 *    with unmovable allocations). While there are corner cases where it might
 *    still work, it is barely relevant in practice.
 *
 * Exceptions are dynamic memory groups, which allow for more MOVABLE
 * memory within the same memory group -- because in that case, there is
 * coordination within the single memory device managed by a single driver.
 *
 * We rely on "present pages" instead of "managed pages", as the latter is
 * highly unreliable and dynamic in virtualized environments, and does not
 * consider boot time allocations. For example, memory ballooning adjusts the
 * managed pages when inflating/deflating the balloon, and balloon page
 * migration can even migrate inflated pages between zones.
 *
 * Using "present pages" is better but some things to keep in mind are:
 *
 * a) Some memblock allocations, such as for the crashkernel area, are
 *    effectively unused by the kernel, yet they account to "present pages".
 *    Fortunately, these allocations are comparatively small in relevant setups
 *    (e.g., fraction of system memory).
 * b) Some hotplugged memory blocks in virtualized environments, especially
 *    hotplugged by virtio-mem, look like they are completely present, however,
 *    only parts of the memory block are actually currently usable.
 *    "present pages" is an upper limit that can get reached at runtime. As
 *    we base our calculations on KERNEL_EARLY, this is not an issue.
 *
 * 中文：自动策略以 MOVABLE/KERNEL_EARLY 为核心不变量。热添加的 kernel 页未来
 * 仍可能独立拔出，不能永久扩大其他设备的 MOVABLE 配额；动态组因驱动能协调同组
 * 单元而例外。使用 present 而非 managed 可避开 balloon 和启动保留的动态噪声，
 * CMA 按实际可迁移性质计入 MOVABLE。该策略保守地允许少计可用容量，但不能超配。
 */
/*
 * 业务背景：MMOP_ONLINE+auto-movable 要在设备协调单元一致性和系统比例之间选择 zone；
 * 参数：nid 为目标节点；group 可空借用；pfn/nr_pages 是当前 block 页范围；
 * 返回：两级预算及组/单元一致性均允许时返回借用 ZONE_MOVABLE，否则返回借用 kernel zone；
 * 注意事项：调用者持 hotplug 串行；按整个未上线单元预算，函数会对齐并改写局部 pfn 但不改实参对象。
 */
static struct zone *auto_movable_zone_for_pfn(int nid,
					      struct memory_group *group,
					      unsigned long pfn,
					      unsigned long nr_pages)
{
	unsigned long online_pages = 0, max_pages, end_pfn;
	struct page *page;

	if (!auto_movable_ratio)
		/* 比例 0 明确禁用自动 MOVABLE，直接走 kernel 回退。 */
		goto kernel_zone;

	if (group && !group->is_dynamic) {
		max_pages = group->s.max_pages;
		online_pages = group->present_movable_pages;

		/* If anything is !MOVABLE online the rest !MOVABLE. */
		/* 中文：静态组必须保持单一类别；已有 kernel 页时剩余页也选 kernel。 */
		if (group->present_kernel_pages)
			goto kernel_zone;
	} else if (!group || group->d.unit_pages == nr_pages) {
		max_pages = nr_pages;
	} else {
		max_pages = group->d.unit_pages;
		/*
		 * Take a look at all online sections in the current unit.
		 * We can safely assume that all pages within a section belong
		 * to the same zone, because dynamic memory groups only deal
		 * with hotplugged memory.
		 * 中文：动态组按 unit 保持同一 zone；检查已在线 section 决定未上线余量。
		 */
		pfn = ALIGN_DOWN(pfn, group->d.unit_pages);
		end_pfn = pfn + group->d.unit_pages;
		for (; pfn < end_pfn; pfn += PAGES_PER_SECTION) {
			page = pfn_to_online_page(pfn);
			if (!page)
				continue;
			/* If anything is !MOVABLE online the rest !MOVABLE. */
			/* 中文：单元内发现任一非 MOVABLE section，整单元回退 kernel。 */
			if (!is_zone_movable_page(page))
				goto kernel_zone;
			online_pages += PAGES_PER_SECTION;
		}
	}

	/*
	 * Online MOVABLE if we could *currently* online all remaining parts
	 * MOVABLE. We expect to (add+) online them immediately next, so if
	 * nobody interferes, all will be MOVABLE if possible.
	 * 中文：按尚未上线的整个单元做预算，避免逐块均通过却最终超过比例。
	 */
	nr_pages = max_pages - online_pages;
	if (!auto_movable_can_online_movable(NUMA_NO_NODE, group, nr_pages))
		goto kernel_zone;

#ifdef CONFIG_NUMA
	/* NUMA-aware 模式要求全局和本节点两道预算同时通过。 */
	if (auto_movable_numa_aware &&
	    !auto_movable_can_online_movable(nid, group, nr_pages))
		goto kernel_zone;
#endif /* CONFIG_NUMA */

	return &NODE_DATA(nid)->node_zones[ZONE_MOVABLE];
kernel_zone:
	/* 所有保守拒绝都统一继承现有 kernel zone 或 NORMAL。 */
	return default_kernel_zone_for_pfn(nid, pfn, nr_pages);
}

/*
 * 业务背景：contig-zones 策略要优先继承范围内唯一既有 zone，避免无意制造 zone 交叠；
 * 参数：nid 指向已初始化节点；start_pfn/nr_pages 是候选页范围；
 * 返回：恰有一个相交 zone 时返回它，否则按 movable_node 返回 MOVABLE 或 kernel 借用指针；
 * 注意事项：调用者持 hotplug 串行；本函数只决策不扩 span，歧义情况下默认 kernel 更保守。
 */
static inline struct zone *default_zone_for_pfn(int nid, unsigned long start_pfn,
		unsigned long nr_pages)
{
	struct zone *kernel_zone = default_kernel_zone_for_pfn(nid, start_pfn,
			nr_pages);
	struct zone *movable_zone = &NODE_DATA(nid)->node_zones[ZONE_MOVABLE];
	bool in_kernel = zone_intersects(kernel_zone, start_pfn, nr_pages);
	bool in_movable = zone_intersects(movable_zone, start_pfn, nr_pages);

	/*
	 * We inherit the existing zone in a simple case where zones do not
	 * overlap in the given range
	 * 中文：异或表示恰好一个 zone 相交，此时延续它可避免制造 zone 交叠。
	 */
	if (in_kernel ^ in_movable)
		return (in_kernel) ? kernel_zone : movable_zone;

	/*
	 * If the range doesn't belong to any zone or two zones overlap in the
	 * given range then we use movable zone only if movable_node is
	 * enabled because we always online to a kernel zone by default.
	 * 中文：无唯一归属时默认 kernel；movable_node 启动参数才允许优先 MOVABLE。
	 */
	return movable_node_enabled ? movable_zone : kernel_zone;
}

/*
 * 业务背景：drivers/base/memory.c 在 block 上线前需要把用户 mmop 与全局策略归一为目标 zone；
 * 参数：online_type 为合法 MMOP_*；nid 为节点；group 可空借用；start_pfn/nr_pages 为页范围；
 * 返回：pgdat 内长寿 zone 借用指针；显式类型优先，再走 auto-movable 或 contig 策略；
 * 注意事项：调用者随后持 hotplug 写锁执行 online；本函数不修改 zone，非法 mmop 会落入策略路径。
 */
struct zone *zone_for_pfn_range(enum mmop online_type, int nid,
		struct memory_group *group, unsigned long start_pfn,
		unsigned long nr_pages)
{
	if (online_type == MMOP_ONLINE_KERNEL)
		return default_kernel_zone_for_pfn(nid, start_pfn, nr_pages);

	if (online_type == MMOP_ONLINE_MOVABLE)
		/* 显式用户选择绕过比例政策，由后续上线契约承担结果。 */
		return &NODE_DATA(nid)->node_zones[ZONE_MOVABLE];

	if (online_policy == ONLINE_POLICY_AUTO_MOVABLE)
		return auto_movable_zone_for_pfn(nid, group, start_pfn, nr_pages);

	return default_zone_for_pfn(nid, start_pfn, nr_pages);
}

/*
 * This function should only be called by memory_block_{online,offline},
 * and {online,offline}_pages.
 * 中文：只允许完整 memory block 的上线/下线入口调用，以保持 early、zone、node
 * 和 memory_group 四套 present 计数同步；nr_pages 为带符号增量。
 */
/*
 * 业务背景：memory_block online/offline 要同步维护 zone、node、early 与 group 四套 present 统计；
 * 参数：page 是范围首页借用指针；group 可空借用；nr_pages 是可正可负的页数增量；
 * 返回：void；按 page 所属 zone 原子事务式更新相关计数，不改变 managed_pages；
 * 注意事项：调用者持 hotplug 写锁且范围为完整 block；错误符号或错误 group 会造成永久统计漂移。
 */
void adjust_present_page_count(struct page *page, struct memory_group *group,
			       long nr_pages)
{
	struct zone *zone = page_zone(page);
	const bool movable = zone_idx(zone) == ZONE_MOVABLE;

	/*
	 * We only support onlining/offlining/adding/removing of complete
	 * memory blocks; therefore, either all is either early or hotplugged.
	 * 中文：完整 block 不会混合 early/hotplug 属性，因此只检查首个 section 即可。
	 */
	if (early_section(__pfn_to_section(page_to_pfn(page))))
		zone->present_early_pages += nr_pages;
	zone->present_pages += nr_pages;
	/* zone 与 pgdat 的增减必须在同一提交阶段发生，避免聚合统计漂移。 */
	zone->zone_pgdat->node_present_pages += nr_pages;

	if (group && movable)
		group->present_movable_pages += nr_pages;
	else if (group && !movable)
		group->present_kernel_pages += nr_pages;
}

/*
 * 业务背景：self-hosted memory_block 上线前，头部 vmemmap 页要先成为自身不可迁移的元数据；
 * 参数：pfn/nr_pages 是保留区页范围；zone 是普通数据页将使用的借用目标；
 * 返回：0 表示 shadow/zone/page 标志均初始化；KASAN 建 shadow 失败返回 errno 且无 zone 副作用；
 * 注意事项：调用者持 hotplug 写锁；页设 MIGRATE_UNMOVABLE/VmemmapSelfHosted，不交 buddy。
 */
int mhp_init_memmap_on_memory(unsigned long pfn, unsigned long nr_pages,
			      struct zone *zone)
{
	unsigned long end_pfn = pfn + nr_pages;
	int ret, i;

	ret = kasan_add_zero_shadow(__va(PFN_PHYS(pfn)), PFN_PHYS(nr_pages));
	if (ret)
		return ret;

	move_pfn_range_to_zone(zone, pfn, nr_pages, NULL, MIGRATE_UNMOVABLE,
			       /* 元数据页本身不能在拔出所承载内存时迁移走。 */
			       false);

	for (i = 0; i < nr_pages; i++) {
		/* 每个 struct page 从临时 Offline 状态转为自承载 vmemmap 专用页。 */
		struct page *page = pfn_to_page(pfn + i);

		__ClearPageOffline(page);
		SetPageVmemmapSelfHosted(page);
	}

	/*
	 * It might be that the vmemmap_pages fully span sections. If that is
	 * the case, mark those sections online here as otherwise they will be
	 * left offline.
	 * 中文：保留区覆盖完整 section 时主数据区不会替它发布在线位，需在此补做。
	 */
	if (nr_pages >= PAGES_PER_SECTION)
	        online_mem_sections(pfn, ALIGN_DOWN(end_pfn, PAGES_PER_SECTION));

	return ret;
}

/*
 * 业务背景：self-hosted block 的普通页已成功下线后，要撤销头部元数据页的在线状态；
 * 参数：pfn/nr_pages 是元数据页范围，page→zone 仍有效且对象不转移；
 * 返回：void；撤完整 section 在线位、poison/收缩 zone 并移除 KASAN zero shadow；
 * 注意事项：调用者持 hotplug 写锁且保证承载内存已离线；顺序不能早于普通页下线。
 */
void mhp_deinit_memmap_on_memory(unsigned long pfn, unsigned long nr_pages)
{
	unsigned long end_pfn = pfn + nr_pages;

	/*
	 * It might be that the vmemmap_pages fully span sections. If that is
	 * the case, mark those sections offline here as otherwise they will be
	 * left online.
	 * 中文：先撤发布位，阻止新 pfn_to_online_page 查询取得即将拆除的元数据页。
	 */
	if (nr_pages >= PAGES_PER_SECTION)
		offline_mem_sections(pfn, ALIGN_DOWN(end_pfn, PAGES_PER_SECTION));

        /*
	 * The pages associated with this vmemmap have been offlined, so
	 * we can reset its state here.
	 * 中文：承载的普通内存已经下线，现可安全毒化这些 struct page 并收缩 zone。
	 */
	remove_pfn_range_from_zone(page_zone(pfn_to_page(pfn)), pfn, nr_pages);
	kasan_remove_zero_shadow(__va(PFN_PHYS(pfn)), PFN_PHYS(nr_pages));
}

/*
 * Must be called with mem_hotplug_lock in write mode.
 * 业务背景：memory_block 设备上线的核心提交事务，要把离线页变成可分配容量并通知所有消费者；
 * 中文：memory block 核心的上线提交事务；调用者独占 hotplug 锁并已选定 zone。
 * 成功后页进入 buddy、统计和节点状态已发布并发出 MEM_ONLINE；通知器拒绝时撤销
 * zone 绑定并返回 errno，页保持离线。
 * 参数：pfn/nr_pages 是待上线页范围；zone/group 是 device 层已选好的借用归属，group 可空；
 * 返回：0 表示可分配且通知完成；-EINVAL 表示粒度错，或传播 node/memory notifier errno；
 * 注意事项：入口必须持 hotplug 写锁且可睡眠；undo_isolate 是可分配提交点，失败路径只存在于此前。
 */
int online_pages(unsigned long pfn, unsigned long nr_pages,
		       struct zone *zone, struct memory_group *group)
{
	struct memory_notify mem_arg = {
		.start_pfn = pfn,
		.nr_pages = nr_pages,
	};
	struct node_notify node_arg = {
		.nid = NUMA_NO_NODE,
	};
	/* NUMA_NO_NODE 是“本次不是首段内存”的哨兵，也控制成功/取消节点通知。 */
	const int nid = zone_to_nid(zone);
	int need_zonelists_rebuild = 0;
	unsigned long flags;
	int ret;

	/*
	 * {on,off}lining is constrained to full memory sections (or more
	 * precisely to memory blocks from the user space POV).
	 * memmap_on_memory is an exception because it reserves initial part
	 * of the physical memory space for vmemmaps. That space is pageblock
	 * aligned.
	 * 中文：普通请求必须覆盖完整 section；self-hosted vmemmap 允许起点只按
	 * pageblock 对齐，但结束仍落在 section 边界。
	 */
	if (WARN_ON_ONCE(!nr_pages || !pageblock_aligned(pfn) ||
			 !IS_ALIGNED(pfn + nr_pages, PAGES_PER_SECTION)))
		return -EINVAL;


	/* associate pfn range with the zone */
	/* 中文：先建立 PageOffline 的 zone 归属，通知器可据此检查但页尚不可分配。 */
	move_pfn_range_to_zone(zone, pfn, nr_pages, NULL, MIGRATE_MOVABLE,
			       true);

	if (!node_state(nid, N_MEMORY)) {
		/* Adding memory to the node for the first time */
		/* 中文：首段内存有独立节点通知；拒绝会与普通上线拒绝汇入同一回滚。 */
		node_arg.nid = nid;
		ret = node_notify(NODE_ADDING_FIRST_MEMORY, &node_arg);
		ret = notifier_to_errno(ret);
		if (ret)
			goto failed_addition;
	}

	ret = memory_notify(MEM_GOING_ONLINE, &mem_arg);
	/* blocking notifier 可否决事务；转成 errno 后尚未发布 section 或 buddy 页。 */
	ret = notifier_to_errno(ret);
	if (ret)
		goto failed_addition;

	/*
	 * Fixup the number of isolated pageblocks before marking the sections
	 * onlining, such that undo_isolate_page_range() works correctly.
	 * 中文：move 阶段把 pageblock 标为 isolate，此处先补统计，稍后 undo 才能对称扣减。
	 */
	spin_lock_irqsave(&zone->lock, flags);
	zone->nr_isolate_pageblock += nr_pages / pageblock_nr_pages;
	spin_unlock_irqrestore(&zone->lock, flags);

	/*
	 * If this zone is not populated, then it is not in zonelist.
	 * This means the page allocator ignores this zone.
	 * So, zonelist must be updated after online.
	 * 中文：空 zone 首次获得 present 页时先准备 pageset，发布后再重建分配回退链。
	 */
	if (!populated_zone(zone)) {
		need_zonelists_rebuild = 1;
		setup_zone_pageset(zone);
	}

	online_pages_range(pfn, nr_pages);
	/* section 在线和 buddy 暴露完成后，提交 zone/node/group 的 present 统计。 */
	adjust_present_page_count(pfn_to_page(pfn), group, nr_pages);

	if (node_arg.nid >= 0)
		node_set_state(nid, N_MEMORY);
	/*
	 * Check whether we are adding normal memory to the node for the first
	 * time.
	 * 中文：NORMAL 及以下任一 zone 首次出现时发布 N_NORMAL_MEMORY。
	 */
	if (!node_state(nid, N_NORMAL_MEMORY) && zone_idx(zone) <= ZONE_NORMAL)
		node_set_state(nid, N_NORMAL_MEMORY);

	if (need_zonelists_rebuild)
		build_all_zonelists(NULL);

	/* Basic onlining is complete, allow allocation of onlined pages. */
	/* 中文：解除 pageblock 隔离是普通分配真正可取得新页的提交点。 */
	undo_isolate_page_range(pfn, pfn + nr_pages);

	/*
	 * Freshly onlined pages aren't shuffled (e.g., all pages are placed to
	 * the tail of the freelist when undoing isolation). Shuffle the whole
	 * zone to make sure the just onlined pages are properly distributed
	 * across the whole freelist - to create an initial shuffle.
	 * 中文：刚解除隔离的页集中在 freelist 尾部，整 zone 洗牌恢复随机化分布。
	 */
	shuffle_zone(zone);

	/* reinitialise watermarks and update pcp limits */
	/* 中文：容量改变后重算水位和 per-CPU 缓存上限，再启动回收/规整线程。 */
	init_per_zone_wmark_min();

	kswapd_run(nid);
	kcompactd_run(nid);

	if (node_arg.nid >= 0)
		/* First memory added successfully. Notify consumers. */
		/* 中文：只有所有核心状态提交后，才发送“首段已添加”的完成通知。 */
		node_notify(NODE_ADDED_FIRST_MEMORY, &node_arg);

	writeback_set_ratelimit();

	memory_notify(MEM_ONLINE, &mem_arg);
	return 0;

failed_addition:
	/* 失败点均早于 buddy 暴露，只需取消通知并撤 zone span/memmap 初始化。 */
	pr_debug("online_pages [mem %#010llx-%#010llx] failed\n",
		 (unsigned long long) pfn << PAGE_SHIFT,
		 (((unsigned long long) pfn + nr_pages) << PAGE_SHIFT) - 1);
	memory_notify(MEM_CANCEL_ONLINE, &mem_arg);
	if (node_arg.nid != NUMA_NO_NODE)
		node_notify(NODE_CANCEL_ADDING_FIRST_MEMORY, &node_arg);
	remove_pfn_range_from_zone(zone, pfn, nr_pages);
	return ret;
}

/* we are OK calling __meminit stuff here - we have CONFIG_MEMORY_HOTPLUG */
/*
 * 业务背景：首次向离线节点添加内存时，预分配 pgdat 的内部 zone/pageset/zonelist 尚未初始化；
 * 参数：nid 是 node_possible_map 中的节点号，不携带对象 ownership；
 * 返回：成功返回 NODE_DATA 的长寿借用指针，底层分配失败可返回 NULL；
 * 注意事项：调用者持 hotplug 写锁且可睡眠；只初始化空节点，不发布 node online 或 present 页。
 */
static pg_data_t *hotadd_init_pgdat(int nid)
{
	struct pglist_data *pgdat;

	/*
	 * NODE_DATA is preallocated (free_area_init) but its internal
	 * state is not allocated completely. Add missing pieces.
	 * Completely offline nodes stay around and they just need
	 * reinitialization.
	 * 中文：pgdat 外壳由启动期预分配；这里补齐内部 zone/pageset 状态，离线节点则重置。
	 */
	pgdat = NODE_DATA(nid);

	/* init node's zones as empty zones, we don't have any present pages.*/
	/* 中文：此刻只初始化空 zone，真正 present 计数由后续 online_pages 提交。 */
	free_area_init_core_hotplug(pgdat);

	/*
	 * The node we allocated has no zone fallback lists. For avoiding
	 * to access not-initialized zonelist, build here.
	 * 中文：即使尚无页也先建立 fallback list，避免节点上线后的早期访问空指针。
	 */
	build_all_zonelists(pgdat);

	return pgdat;
}

/*
 * __try_online_node - online a node if offlined
 * @nid: the node ID
 * @set_node_online: Whether we want to online the node
 * called by cpu_up() to online a node without onlined memory.
 *
 * Returns:
 * 1 -> a new node has been allocated
 * 0 -> the node is already online
 * -ENOMEM -> the node could not be allocated
 * 中文：保证 pgdat 可用，并按 set_node_online 决定是否发布/注册节点；1 表示本次
 * 完成初始化，0 表示原已在线，负值表示初始化失败。
 * 业务背景：CPU online 与 memory add 都需确保离线 node 的 pgdat 可用，但发布时机不同；
 * 参数：nid 是可能离线的节点号；set_node_online 决定本函数是否立刻设置 online 并注册 node；
 * 返回：0 已在线，1 本次初始化成功，-ENOMEM 初始化失败；register_node 失败视为 BUG；
 * 注意事项：调用者持 hotplug 写锁；false 路径由 add_memory_resource 稍后发布并负责错误回滚。
 */
static int __try_online_node(int nid, bool set_node_online)
{
	pg_data_t *pgdat;
	int ret = 1;

	if (node_online(nid))
		/* 已发布节点无需重建 pgdat，也不重复注册 sysfs。 */
		return 0;

	pgdat = hotadd_init_pgdat(nid);
	if (!pgdat) {
		pr_err("Cannot online node %d due to NULL pgdat\n", nid);
		ret = -ENOMEM;
		goto out;
	}

	if (set_node_online) {
		/* CPU 热上线等调用者需要立即发布节点；内存添加路径会延迟到回滚点明确后。 */
		node_set_online(nid);
		ret = register_node(nid);
		BUG_ON(ret);
	}
out:
	return ret;
}

/*
 * Users of this function always want to online/register the node
 * 业务背景：CPU 热上线等调用者需要一个自行加锁且立即 online/register 节点的公共入口；
 * 参数：nid 是要确保在线的节点号，必须属于 possible map；
 * 返回：继承 __try_online_node：0 已在线、1 新初始化、-ENOMEM 失败；
 * 注意事项：可睡眠并自行 begin/done；与 add 的延迟发布路径不同，成功后 node 已对外可见。
 */
int try_online_node(int nid)
{
	int ret;

	mem_hotplug_begin();
	ret =  __try_online_node(nid, true);
	mem_hotplug_done();
	return ret;
}

/*
 * 业务背景：用户可见 memory_block 设备要求 add/remove 范围能一一切成完整 block；
 * 参数：start 是物理字节起点，size 是字节长度，均按值传递；
 * 返回：非零且两者按 block 对齐返回 0，否则打印诊断并返回 -EINVAL；
 * 注意事项：纯校验、无需锁且无副作用；不负责检查地址上限、回绕或 iomem 冲突。
 */
static int check_hotplug_memory_range(u64 start, u64 size)
{
	/* memory range must be block size aligned */
	/* 中文：sysfs memory device 以 block 为单位，非对齐范围无法建立一一对应设备。 */
	if (!size || !IS_ALIGNED(start, memory_block_size_bytes()) ||
	    !IS_ALIGNED(size, memory_block_size_bytes())) {
		pr_err("Block size [%#lx] unaligned hotplug range: start %#llx, size %#llx",
		       memory_block_size_bytes(), start, size);
		return -EINVAL;
	}

	return 0;
}

/*
 * 业务背景：add_memory_resource() 成功建完一批 block 后，按默认策略逐块触发自动上线；
 * 参数：mem 是 walker 借用的 memory_block；arg 未使用且可为 NULL；
 * 返回：直接返回 device_online() 结果，负 errno 可终止 walker，非负表示已处理/已在线；
 * 注意事项：外层已释放 hotplug 写锁但持 device 锁；device_online 内部会重新获取写锁。
 */
static int online_memory_block(struct memory_block *mem, void *arg)
{
	mem->online_type = mhp_get_default_online_type();
	return device_online(&mem->dev);
}

#ifndef arch_supports_memmap_on_memory
/*
 * 业务背景：架构未提供专用 hook 时，self-host vmemmap 只有整 PMD 才能避免映射相邻 block；
 * 参数：vmemmap_size 是单 block 元数据字节数；
 * 返回：PMD_SIZE 对齐返回 true，否则 false；
 * 注意事项：纯函数、无锁/ownership；仅为缺省条件编译实现，架构宏可替换更精确规则。
 */
static inline bool arch_supports_memmap_on_memory(unsigned long vmemmap_size)
{
	/*
	 * As default, we want the vmemmap to span a complete PMD such that we
	 * can map the vmemmap using a single PMD if supported by the
	 * architecture.
	 * 中文：完整 PMD 可避免 altmap 填充相邻 memory block 的 vmemmap 部分。
	 */
	return IS_ALIGNED(vmemmap_size, PMD_SIZE);
}
#endif

/*
 * 业务背景：收到 MHP_MEMMAP_ON_MEMORY 只是提示，core 仍须证明该布局能安全上线/下线；
 * 参数：无，读取静态 block 大小、memmap_mode 与架构能力；
 * 返回：所有 page/PMD/pageblock/剩余容量条件成立才 true，否则 false 并走普通布局；
 * 注意事项：纯能力检查、无需锁且无副作用；FORCE 只能修复 pageblock 对齐，不能绕过其他门禁。
 */
bool mhp_supports_memmap_on_memory(void)
{
	unsigned long vmemmap_size = memory_block_memmap_size();
	unsigned long memmap_pages = memory_block_memmap_on_memory_pages();

	/*
	 * Besides having arch support and the feature enabled at runtime, we
	 * need a few more assumptions to hold true:
	 *
	 * a) The vmemmap pages span complete PMDs: We don't want vmemmap code
	 *    to populate memory from the altmap for unrelated parts (i.e.,
	 *    other memory blocks)
	 *
	 * b) The vmemmap pages (and thereby the pages that will be exposed to
	 *    the buddy) have to cover full pageblocks: memory onlining/offlining
	 *    code requires applicable ranges to be page-aligned, for example, to
	 *    set the migratetypes properly.
	 *
	 * TODO: Although we have a check here to make sure that vmemmap pages
	 *       fully populate a PMD, it is not the right place to check for
	 *       this. A much better solution involves improving vmemmap code
	 *       to fallback to base pages when trying to populate vmemmap using
	 *       altmap as an alternative source of memory, and we do not exactly
	 *       populate a single PMD.
	 * 中文：除架构支持外，元数据必须完全落入 altmap，保留区和可用区边界必须满足
	 * pageblock 隔离契约；当前 PMD 限制是 vmemmap 大页实现的保守约束。
	 */
	if (!mhp_memmap_on_memory())
		return false;

	/*
	 * Make sure the vmemmap allocation is fully contained
	 * so that we always allocate vmemmap memory from altmap area.
	 * 中文：非整页元数据会跨到 altmap 外，无法保证 self-hosted 所有权边界。
	 */
	if (!IS_ALIGNED(vmemmap_size, PAGE_SIZE))
		return false;

	/*
	 * start pfn should be pageblock_nr_pages aligned for correctly
	 * setting migrate types
	 * 中文：保留页数必须对齐 pageblock，剩余上线范围才可正确设置 migratetype。
	 */
	if (!pageblock_aligned(memmap_pages))
		return false;

	if (memmap_pages == PHYS_PFN(memory_block_size_bytes()))
		/* No effective hotplugged memory doesn't make sense. */
		/* 中文：整块都被元数据吃掉时没有可上线容量，直接拒绝。 */
		return false;

	return arch_supports_memmap_on_memory(vmemmap_size);
}
EXPORT_SYMBOL_GPL(mhp_supports_memmap_on_memory);

/*
 * 业务背景：self-hosted block 的 arch 映射拆除后，需要最终释放其堆 altmap 描述符；
 * 参数：altmap 必须非空且 ownership 已从 memory_block 转给当前移除路径；
 * 返回：void；检查 alloc 后释放对象，返回后指针失效；
 * 注意事项：arch_remove_memory 应先把 alloc 归零，非零只告警仍继续 free，禁止重复调用。
 */
static void altmap_free(struct vmem_altmap *altmap)
{
	WARN_ONCE(altmap->alloc, "Altmap not fully unmapped");
	kfree(altmap);
}

/*
 * 业务背景：每个 self-hosted block 拥有独立 altmap，不能像普通范围一样一次拆除；
 * 参数：start/size 是按 memory-block 对齐的物理字节范围，所有 block 已 offline；
 * 返回：void；逐块转移 altmap、删设备和 arch 映射并释放描述符；
 * 注意事项：持 device 与 hotplug 锁且可睡眠；get 失败只 WARN/跳过，混合 altmap 已由上层拒绝。
 */
static void remove_memory_blocks_and_altmaps(u64 start, u64 size)
{
	unsigned long memblock_size = memory_block_size_bytes();
	u64 cur_start;

	/*
	 * For memmap_on_memory, the altmaps were added on a per-memblock
	 * basis; we have to process each individual memory block.
	 * 中文：每块拥有独立 altmap，不能像普通范围一样一次传 NULL 批量拆除。
	 */
	for (cur_start = start; cur_start < start + size;
	     cur_start += memblock_size) {
		/* 每轮取得当前 memory_block 引用并转移其 altmap；此前块已完整拆除。 */
		struct vmem_altmap *altmap = NULL;
		struct memory_block *mem;

		mem = memory_block_get(phys_to_block_id(cur_start));
		if (WARN_ON_ONCE(!mem))
			continue;

		altmap = mem->altmap;
		/* 在释放 device 前转移 altmap 所有权，清字段防止后续重复释放。 */
		mem->altmap = NULL;
		memory_block_put(mem);

		remove_memory_block_devices(cur_start, memblock_size);
		arch_remove_memory(cur_start, memblock_size, altmap, NULL);
		altmap_free(altmap);
	}
}

/*
 * 业务背景：self-host 请求必须逐 block 建独立 altmap、架构映射和 memory_block 设备；
 * 参数：nid 为节点；group 可空借用；start/size 是对齐物理字节范围；
 * 返回：0 表示全范围发布；-ENOMEM 或 arch/device errno，并已回滚当前块与成功前缀；
 * 注意事项：调用者持两类 hotplug 锁且可睡眠；成功后每个堆 altmap ownership 转给对应 block。
 */
static int create_altmaps_and_memory_blocks(int nid, struct memory_group *group,
					    u64 start, u64 size)
{
	unsigned long memblock_size = memory_block_size_bytes();
	u64 cur_start;
	int ret;

	for (cur_start = start; cur_start < start + size;
	     cur_start += memblock_size) {
		/* params 传递当前块的页表保护和 altmap，离开本轮后不再借用栈对象。 */
		struct mhp_params params = { .pgprot =
						     pgprot_mhp(PAGE_KERNEL) };
		struct vmem_altmap mhp_altmap = {
			.base_pfn = PHYS_PFN(cur_start),
			.end_pfn = PHYS_PFN(cur_start + memblock_size - 1),
		};

		mhp_altmap.free = memory_block_memmap_on_memory_pages();
		/* 栈上模板复制到堆；arch 与 memory_block 在成功路径共享这个长寿对象。 */
		params.altmap = kmemdup(&mhp_altmap, sizeof(struct vmem_altmap),
					GFP_KERNEL);
		if (!params.altmap) {
			ret = -ENOMEM;
			goto out;
		}

		/* call arch's memory hotadd */
		/* 中文：先建立 sparse/vmemmap 映射，之后 sysfs 设备才可引用其 section。 */
		ret = arch_add_memory(nid, cur_start, memblock_size, &params);
		if (ret < 0) {
			altmap_free(params.altmap);
			goto out;
		}

		/* create memory block devices after memory was added */
		/* 中文：设备创建失败需先撤架构映射，再释放尚未转交的 altmap。 */
		ret = create_memory_block_devices(cur_start, memblock_size, nid,
						  params.altmap, group);
		if (ret) {
			arch_remove_memory(cur_start, memblock_size, params.altmap, NULL);
			altmap_free(params.altmap);
			goto out;
		}
	}

	return 0;
out:
	/* cur_start 之前的块已完整发布，由配对 helper 连设备与 altmap 一并撤销。 */
	if (ret && cur_start != start)
		remove_memory_blocks_and_altmaps(start, cur_start - start);
	return ret;
}

/*
 * NOTE: The caller must call lock_device_hotplug() to serialize hotplug
 * and online/offline operations (triggered e.g. by sysfs).
 *
 * we are OK calling __meminit stuff here - we have CONFIG_MEMORY_HOTPLUG
 * 业务背景：已声明 iomem resource 后，核心要原子完成 node、arch 映射、block 设备及可选自动上线；
 * 中文：调用者持 device_hotplug_lock，本函数再独占 mem_hotplug_lock；成功建立
 * node、架构映射和 memory_block，最后解锁后按默认策略上线。失败按相反顺序回滚。
 * 参数：nid 是节点或 MHP_NID_IS_MGID 下的组号；res 已在 iomem 树；mhp_flags 控制组/self-host/merge；
 * 返回：0 成功，或范围、组、node、memblock、arch/device 的 errno；失败保留 res 给外层释放；
 * 注意事项：入口必须持 device_hotplug_lock；merge 后 res 可能失效，device_online 必须在释放写锁后调用。
 */
int add_memory_resource(int nid, struct resource *res, mhp_t mhp_flags)
{
	struct mhp_params params = { .pgprot = pgprot_mhp(PAGE_KERNEL) };
	enum memblock_flags memblock_flags = MEMBLOCK_NONE;
	struct memory_group *group = NULL;
	u64 start, size;
	bool new_node = false;
	int ret;

	start = res->start;
	size = resource_size(res);
	/* res 已在 iomem 树中，失败由外层 __add_memory 决定是否释放。 */

	ret = check_hotplug_memory_range(start, size);
	if (ret)
		return ret;

	if (mhp_flags & MHP_NID_IS_MGID) {
		/* 此时 nid 实为 group id；group 为注册表借用指针，由 hotplug 串行稳定。 */
		group = memory_group_find_by_id(nid);
		if (!group)
			return -EINVAL;
		nid = group->nid;
	}

	if (!node_possible(nid)) {
		WARN(1, "node %d was absent from the node_possible_map\n", nid);
		return -EINVAL;
	}

	mem_hotplug_begin();
	/* 从此 CPU/内存拓扑固定，直到全部资源可见或错误路径完成清理。 */

	if (IS_ENABLED(CONFIG_ARCH_KEEP_MEMBLOCK)) {
		/* 保留 memblock 的架构需同步记录新范围，驱动管理属性也随之传播。 */
		if (res->flags & IORESOURCE_SYSRAM_DRIVER_MANAGED)
			memblock_flags = MEMBLOCK_DRIVER_MANAGED;
		ret = memblock_add_node(start, size, nid, memblock_flags);
		if (ret)
			goto error_mem_hotplug_end;
	}

	ret = __try_online_node(nid, false);
	if (ret < 0)
		goto error_memblock_remove;
	if (ret) {
		/* pgdat 已初始化但节点尚未发布；本层完成上线/注册以便后续挂设备。 */
		node_set_online(nid);
		ret = register_node(nid);
		if (WARN_ON(ret)) {
			node_set_offline(nid);
			goto error_memblock_remove;
		}
		new_node = true;
	}

	/*
	 * Self hosted memmap array
	 * 中文：请求且平台满足约束时逐块 self-host，否则走普通整范围 arch_add。
	 */
	if ((mhp_flags & MHP_MEMMAP_ON_MEMORY) &&
	    mhp_supports_memmap_on_memory()) {
		ret = create_altmaps_and_memory_blocks(nid, group, start, size);
		if (ret)
			goto error;
	} else {
		ret = arch_add_memory(nid, start, size, &params);
		if (ret < 0)
			goto error;

		/* create memory block devices after memory was added */
		/* 中文：普通模式的设备同样必须晚于架构映射，失败则整范围撤销。 */
		ret = create_memory_block_devices(start, size, nid, NULL, group);
		if (ret) {
			arch_remove_memory(start, size, params.altmap, NULL);
			goto error;
		}
	}

	/* 把新设备链接到 node sysfs；此时 node 和全部 block 都已存在。 */
	register_memory_blocks_under_node_hotplug(nid, PFN_DOWN(start),
					  PFN_UP(start + size - 1));

	/* create new memmap entry */
	/* 中文：只有真正 System RAM 才加入固件映射，驱动管理资源不冒充固件 RAM。 */
	if (!strcmp(res->name, "System RAM"))
		firmware_map_add_hotplug(start, start + size, "System RAM");

	/* device_online() will take the lock when calling online_pages() */
	/* 中文：必须先释放写锁；device_online 的回调会重新取得它，持锁调用会死锁。 */
	mem_hotplug_done();

	/*
	 * In case we're allowed to merge the resource, flag it and trigger
	 * merging now that adding succeeded.
	 * 中文：资源合并不可在失败可回滚阶段进行，否则原 res 边界/所有权难以恢复。
	 */
	if (mhp_flags & MHP_MERGE_RESOURCE)
		merge_system_ram_resource(res);

	/* online pages if requested */
	/* 中文：默认 OFFLINE 仅创建设备；其他类型逐块经设备状态机上线。 */
	if (mhp_get_default_online_type() != MMOP_OFFLINE)
		walk_memory_blocks(start, size, NULL, online_memory_block);

	return ret;
error:
	/* 新节点只在本事务创建；错误时撤注册，既有节点绝不能被下线。 */
	if (new_node) {
		node_set_offline(nid);
		unregister_node(nid);
	}
error_memblock_remove:
	/* 仅配置要求保留 memblock 时，才存在需要对称撤销的记录。 */
	if (IS_ENABLED(CONFIG_ARCH_KEEP_MEMBLOCK))
		memblock_remove(start, size);
error_mem_hotplug_end:
	mem_hotplug_done();
	return ret;
}

/* requires device_hotplug_lock, see add_memory_resource() */
/*
 * 业务背景：普通 System RAM 内部入口要把 iomem 声明和 add_memory_resource 组成同一可回滚事务；
 * 参数：nid、物理字节 start/size、mhp_flags 按值传入；
 * 返回：成功 0；resource 声明或 add 阶段返回 errno，失败时本层释放未合并 resource；
 * 注意事项：调用者必须持 device_hotplug_lock；成功后 resource 归资源树且不能由调用者 free。
 */
int __add_memory(int nid, u64 start, u64 size, mhp_t mhp_flags)
{
	struct resource *res;
	int ret;

	res = register_memory_resource(start, size, "System RAM");
	/* ERR_PTR 同时编码范围越界、启动 mem= 限制或 iomem 冲突。 */
	if (IS_ERR(res))
		return PTR_ERR(res);

	ret = add_memory_resource(nid, res, mhp_flags);
	if (ret < 0)
		release_memory_resource(res);
	return ret;
}

/*
 * 业务背景：内存设备驱动通过导出 API 添加普通 System RAM，并需与 sysfs online/offline 串行；
 * 参数：nid、物理字节 start/size、mhp_flags 按值传递，范围由下层验证；
 * 返回：成功 0，失败返回 iomem/add errno，已取得资源由下层回滚；
 * 注意事项：可睡眠并自行持 device_hotplug_lock；不得在已持同锁时调用，应改用 __add_memory。
 */
int add_memory(int nid, u64 start, u64 size, mhp_t mhp_flags)
{
	int rc;

	lock_device_hotplug();
	/* 锁覆盖 resource 注册与完整 add，避免同范围设备状态并发变化。 */
	rc = __add_memory(nid, start, size, mhp_flags);
	unlock_device_hotplug();

	return rc;
}
EXPORT_SYMBOL_GPL(add_memory);

/*
 * Add special, driver-managed memory to the system as system RAM. Such
 * memory is not exposed via the raw firmware-provided memmap as system
 * RAM, instead, it is detected and added by a driver - during cold boot,
 * after a reboot, and after kexec.
 *
 * Reasons why this memory should not be used for the initial memmap of a
 * kexec kernel or for placing kexec images:
 * - The booting kernel is in charge of determining how this memory will be
 *   used (e.g., use persistent memory as system RAM)
 * - Coordination with a hypervisor is required before this memory
 *   can be used (e.g., inaccessible parts).
 *
 * For this memory, no entries in /sys/firmware/memmap ("raw firmware-provided
 * memory map") are created. Also, the created memory resource is flagged
 * with IORESOURCE_SYSRAM_DRIVER_MANAGED, so in-kernel users can special-case
 * this memory as well (esp., not place kexec images onto it).
 *
 * The resource_name (visible via /proc/iomem) has to have the format
 * "System RAM ($DRIVER)".
 * 中文：驱动管理 RAM 不来自固件原始内存图，重启/kexec 前仍需驱动或 hypervisor
 * 协调，故资源带 DRIVER_MANAGED 标志且不写 /sys/firmware/memmap，避免 kexec 把
 * 镜像放入尚不可访问的范围；名称必须显式携带驱动身份。
 */
/*
 * 业务背景：驱动管理 RAM 不能伪装成固件 RAM，但仍要复用完整 add 事务；
 * 参数：nid/start/size/flags 同 add_memory；resource_name 借用且必须为 "System RAM ($DRIVER)"；
 * 返回：0 成功；名称非法 -EINVAL，或传播 resource/add errno并释放失败资源；
 * 注意事项：可睡眠并自行持 device 锁；成功资源带 DRIVER_MANAGED 且不写 firmware memmap。
 */
int add_memory_driver_managed(int nid, u64 start, u64 size,
			      const char *resource_name, mhp_t mhp_flags)
{
	struct resource *res;
	int rc;

	/* 严格格式使内核消费者和 /proc/iomem 用户能识别这类特殊 System RAM。 */
	if (!resource_name ||
	    strstr(resource_name, "System RAM (") != resource_name ||
	    resource_name[strlen(resource_name) - 1] != ')')
		return -EINVAL;

	lock_device_hotplug();

	res = register_memory_resource(start, size, resource_name);
	/* register 会自动设置 DRIVER_MANAGED；其余事务复用普通 add 路径。 */
	if (IS_ERR(res)) {
		rc = PTR_ERR(res);
		goto out_unlock;
	}

	rc = add_memory_resource(nid, res, mhp_flags);
	/* 成功时 res 留在 iomem 树；失败时尚未合并，可按原边界释放。 */
	if (rc < 0)
		release_memory_resource(res);

out_unlock:
	unlock_device_hotplug();
	return rc;
}
EXPORT_SYMBOL_GPL(add_memory_driver_managed);

/*
 * Platforms should define arch_get_mappable_range() that provides
 * maximum possible addressable physical memory range for which the
 * linear mapping could be created. The platform returned address
 * range must adhere to these following semantics.
 *
 * - range.start <= range.end
 * - Range includes both end points [range.start..range.end]
 *
 * There is also a fallback definition provided here, allowing the
 * entire possible physical address range in case any platform does
 * not define arch_get_mappable_range().
 * 中文：架构 hook 返回线性映射可覆盖的闭区间；弱缺省允许整个物理地址空间，
 * 通用层稍后还会与 DIRECT_MAP_PHYSMEM_END 取交集。
 */
/*
 * 业务背景：平台可覆盖此弱 hook，声明能建立线性映射的最大物理地址闭区间；
 * 参数：无；返回对象按值构造，不涉及 ownership；
 * 返回：缺省 [0,U64_MAX]，通用层随后与 DIRECT_MAP_PHYSMEM_END 取交集；
 * 注意事项：架构实现必须保证 start<=end 且两端包含；本弱实现无需锁、无副作用。
 */
struct range __weak arch_get_mappable_range(void)
{
	struct range mhp_range = {
		.start = 0UL,
		.end = -1ULL,
	};
	return mhp_range;
}

/*
 * 业务背景：RAM add 需要 direct map，某些 DEVICE add 不需要，但两者都需统一地址上限；
 * 参数：need_mapping=true 请求架构可映射范围，false 仅使用通用物理上限；
 * 返回：按值返回包含式闭区间，不转移对象；无交集时返回空哨兵 [0,0]；
 * 注意事项：纯查询、无需锁；消费者必须用 end-1 把半开请求与闭区间比较。
 */
struct range mhp_get_pluggable_range(bool need_mapping)
{
	const u64 max_phys = DIRECT_MAP_PHYSMEM_END;
	struct range mhp_range;

	if (need_mapping) {
		mhp_range = arch_get_mappable_range();
		if (mhp_range.start > max_phys) {
			/* 架构范围与 direct map 无交集时返回空哨兵 [0,0]。 */
			mhp_range.start = 0;
			mhp_range.end = 0;
		}
		mhp_range.end = min_t(u64, mhp_range.end, max_phys);
	} else {
		mhp_range.start = 0;
		/* ZONE_DEVICE 等无需 direct map 的调用者仍受架构物理地址宽度上限约束。 */
		mhp_range.end = max_phys;
	}
	return mhp_range;
}
EXPORT_SYMBOL_GPL(mhp_get_pluggable_range);

/*
 * 业务背景：resource 注册和 sparse add 在修改任何状态前必须拒绝不可寻址或回绕范围；
 * 参数：start/size 是物理字节半开范围，need_mapping 指明是否要求线性映射；
 * 返回：非空、无回绕且完全落入允许闭区间返回 true，否则告警并返回 false；
 * 注意事项：纯检查、无需锁且无副作用；size=0 也因 start==end 被拒绝。
 */
bool mhp_range_allowed(u64 start, u64 size, bool need_mapping)
{
	struct range mhp_range = mhp_get_pluggable_range(need_mapping);
	u64 end = start + size;

	if (start < end && start >= mhp_range.start && (end - 1) <= mhp_range.end)
		/* end-1 把半开请求转换为与 arch hook 一致的闭区间终点。 */
		return true;

	pr_warn("Hotplug memory [%#llx-%#llx] exceeds maximum addressable range [%#llx-%#llx]\n",
		start, end, mhp_range.start, mhp_range.end);
	return false;
}

#ifdef CONFIG_MEMORY_HOTREMOVE
/*
 * Scan pfn range [start,end) to find movable/migratable pages (LRU and
 * hugetlb folio, movable_ops pages). Will skip over most unmovable
 * pages (esp., pages that can be skipped when offlining), but bail out on
 * definitely unmovable pages.
 *
 * Returns:
 *	0 in case a movable page is found and movable_pfn was updated.
 *	-ENOENT in case no movable page was found.
 *	-EBUSY in case a definitely unmovable page was found.
 * 中文：扫描隔离范围，找到 LRU、movable_ops 或可迁 hugetlb 时写回 PFN 并返回 0；
 * 没有候选返回 -ENOENT，确认存在带引用的不可移动 Offline 页则返回 -EBUSY。
 * 业务背景：offline 迁移循环需要区分“还有可迁页”“已扫完”和“确定不可迁”三种结果；
 * 参数：start/end 是页单位半开隔离范围；movable_pfn 非空输出且仅在返回 0 时有效；
 * 返回：0 并写回候选 PFN、-ENOENT 表示无候选、-EBUSY 表示发现确定不可移动页；
 * 注意事项：不持 folio 引用，hugetlb 判定允许竞态误报；外层迁移和最终隔离检查负责重验。
 */
static int scan_movable_pages(unsigned long start, unsigned long end,
			      unsigned long *movable_pfn)
{
	unsigned long pfn;

	for (pfn = start; pfn < end; pfn++) {
		unsigned long nr_pages;
		struct page *page;
		struct folio *folio;

		page = pfn_to_page(pfn);
		/* LRU 与 movable_ops 页交给迁移器进一步加锁验证。 */
		if (PageLRU(page) || page_has_movable_ops(page))
			goto found;

		/*
		 * PageOffline() pages that do not have movable_ops and
		 * have a reference count > 0 (after MEM_GOING_OFFLINE) are
		 * definitely unmovable. If their reference count would be 0,
		 * they could at least be skipped when offlining memory.
		 * 中文：无 movable_ops 的 Offline 页若仍有引用，既不能迁移也不能当空洞跳过。
		 */
		if (PageOffline(page) && page_count(page))
			return -EBUSY;

		folio = page_folio(page);
		if (!folio_test_hugetlb(folio))
			continue;
		/*
		 * This test is racy as we hold no reference or lock.  The
		 * hugetlb page could have been free'ed and head is no longer
		 * a hugetlb page before the following check.  In such unlikely
		 * cases false positives and negatives are possible.  Calling
		 * code must deal with these scenarios.
		 * 中文：此处不持 folio 引用，hugetlb 判定允许竞态误报；外层迁移和最终
		 * test_pages_isolated 会重新验证，因此不会错误提交下线。
		 */
		if (folio_test_hugetlb_migratable(folio))
			goto found;
		nr_pages = folio_nr_pages(folio);
		if (unlikely(nr_pages < 1 || nr_pages > MAX_FOLIO_NR_PAGES ||
			     !is_power_of_2(nr_pages)))
			continue;
		pfn |= nr_pages - 1;
		/* 不可迁 huge folio 一次跳到尾页，避免重复检查每个 tail page。 */
	}
	return -ENOENT;
found:
	/* 输出参数仅在成功时有效，调用者从该 PFN 开始迁移余下范围。 */
	*movable_pfn = pfn;
	return 0;
}

/*
 * 业务背景：offline_pages 找到可移动页后，要尽力把范围内 folio 批量迁出而不中断整个循环；
 * 参数：start_pfn/end_pfn 是单 zone 的页单位半开范围，不接收输出对象；
 * 返回：void；成功项迁移，失败隔离/迁移项诊断并放回，外层通过重扫观察结果；
 * 注意事项：可睡眠；folio_try_get/LRU 隔离/folio 锁稳定生命周期，目标优先排除源节点。
 */
static void do_migrate_range(unsigned long start_pfn, unsigned long end_pfn)
{
	struct folio *folio;
	unsigned long pfn;
	LIST_HEAD(source);
	static DEFINE_RATELIMIT_STATE(migrate_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	for (pfn = start_pfn; pfn < end_pfn; pfn++) {
		struct page *page;
		/* page/folio 仅在本轮持引用期间稳定，循环末统一 folio_put。 */

		page = pfn_to_page(pfn);
		folio = page_folio(page);

		if (!folio_try_get(folio))
			/* 正在释放的 folio 留给下一轮扫描重新观察。 */
			continue;

		if (unlikely(page_folio(page) != folio))
			/* compound 关系在取引用窗口改变，丢引用而不使用陈旧 folio。 */
			goto put_folio;

		if (folio_test_large(folio))
			/* 直接跨过 compound folio 的 tail 页，避免重复引用同一 folio。 */
			pfn = folio_pfn(folio) + folio_nr_pages(folio) - 1;

		if (folio_contain_hwpoisoned_page(folio)) {
			/*
			 * unmap_poisoned_folio() cannot handle large folios
			 * in all cases yet.
			 * 中文：非 hugetlb 大 folio 的中毒解除映射尚不完整，保守跳过。
			 */
			if (folio_test_large(folio) && !folio_test_hugetlb(folio))
				goto put_folio;
			if (folio_test_lru(folio) && !folio_isolate_lru(folio))
				goto put_folio;
			if (folio_mapped(folio)) {
				/* folio 锁串行页表解除映射；中毒页本身不加入迁移 source。 */
				folio_lock(folio);
				unmap_poisoned_folio(folio, pfn, false);
				folio_unlock(folio);
			}

			goto put_folio;
		}

		if (!isolate_folio_to_list(folio, &source)) {
			/* isolate 成功把迁移引用/链表所有权交给 source，失败仅限速诊断。 */
			if (__ratelimit(&migrate_rs)) {
				pr_warn("failed to isolate pfn %lx\n",
					page_to_pfn(page));
				dump_page(page, "isolation failed");
			}
		}
put_folio:
		folio_put(folio);
	}
	if (!list_empty(&source)) {
		/* source 非空时选择当前所有 N_MEMORY 节点作为候选目标。 */
		nodemask_t nmask = node_states[N_MEMORY];
		struct migration_target_control mtc = {
			.nmask = &nmask,
			.gfp_mask = GFP_KERNEL | __GFP_MOVABLE | __GFP_RETRY_MAYFAIL,
			.reason = MR_MEMORY_HOTPLUG,
		};
		int ret;

		/*
		 * We have checked that migration range is on a single zone so
		 * we can use the nid of the first page to all the others.
		 * 中文：范围已由上层验证为单 zone，首 folio 的 nid 可代表整个 source。
		 */
		mtc.nid = folio_nid(list_first_entry(&source, struct folio, lru));

		/*
		 * try to allocate from a different node but reuse this node
		 * if there are no other online nodes to be used (e.g. we are
		 * offlining a part of the only existing node)
		 * 中文：优先排除源节点；若它是唯一内存节点则重新加入，允许同节点范围外迁移。
		 */
		node_clear(mtc.nid, nmask);
		if (nodes_empty(nmask))
			node_set(mtc.nid, nmask);
		/* 同步模式在返回前迁移或留下失败项，便于本函数立即 putback。 */
		ret = migrate_pages(&source, alloc_migration_target, NULL,
			(unsigned long)&mtc, MIGRATE_SYNC, MR_MEMORY_HOTPLUG, NULL);
		if (ret) {
			/* migrate_pages 留下未迁移项；逐项诊断后统一放回原有可移动集合。 */
			list_for_each_entry(folio, &source, lru) {
				/* 限速避免大量顽固 folio 淹没日志，dump 保留定位所需页状态。 */
				if (__ratelimit(&migrate_rs)) {
					pr_warn("migrating pfn %lx failed ret:%d\n",
						folio_pfn(folio), ret);
					dump_page(&folio->page,
						  "migration failure");
				}
			}
			/* 无论迁移多少，source 中残留项都必须回到原链表并释放隔离状态。 */
			putback_movable_pages(&source);
		}
	}
}

/*
 * 业务背景：movable_node early 参数允许 contig 策略在歧义范围优先选择 ZONE_MOVABLE；
 * 参数：p 是解析器借用的可选文本，本标志无取值所以不读取也不保存；
 * 返回：0，发布全局 movable_node_enabled=true；
 * 注意事项：单线程 early boot 无锁；只改变后续 zone 选择，不立即移动或上线内存。
 */
static int __init cmdline_parse_movable_node(char *p)
{
	movable_node_enabled = true;
	return 0;
}
early_param("movable_node", cmdline_parse_movable_node);

/*
 * 业务背景：offline 前必须证明目标范围没有非-System-RAM 空洞，才能安全逐 PFN 访问；
 * 参数：start_pfn 未使用；nr_pages 是本段页数；data 指向调用者栈上页计数输出；
 * 返回：恒 0 继续 walker，并把 nr_pages 累加到输出；
 * 注意事项：同步回调不保存 data，无 ownership；调用者在 walker 完成后比较完整范围页数。
 */
static int count_system_ram_pages_cb(unsigned long start_pfn,
				     unsigned long nr_pages, void *data)
{
	unsigned long *nr_system_ram_pages = data;

	*nr_system_ram_pages += nr_pages;
	return 0;
}

/*
 * Must be called with mem_hotplug_lock in write mode.
 * 业务背景：memory_block 设备下线的核心事务，要在不留下分配/引用的前提下撤出系统容量；
 * 中文：完整下线事务在写锁下隔离 pageblock、通知消费者、迁移在用页、从 buddy
 * 摘除并提交统计/节点状态；任何提交前失败都解除隔离、发送 CANCEL 并恢复缓存。
 * 参数：start_pfn/nr_pages 是页范围；zone 是唯一借用归属；group 可空借用并参与 present 记账；
 * 返回：0 表示页/统计/节点状态均已下线；返回粒度、洞、多 zone、通知、信号、迁移/hugetlb errno；
 * 注意事项：入口必须持 hotplug 写锁且可睡眠；__offline_isolated_pages 是不可逆提交点，只在其前回滚。
 */
int offline_pages(unsigned long start_pfn, unsigned long nr_pages,
			struct zone *zone, struct memory_group *group)
{
	unsigned long pfn, managed_pages, system_ram_pages = 0;
	const unsigned long end_pfn = start_pfn + nr_pages;
	struct pglist_data *pgdat = zone->zone_pgdat;
	/* zone 决定唯一 pgdat/nid；单 zone 门禁稍后再次验证首尾页。 */
	const int node = zone_to_nid(zone);
	struct memory_notify mem_arg = {
		.start_pfn = start_pfn,
		.nr_pages = nr_pages,
	};
	/* 通知参数是栈上只读快照，仅在同步 blocking notifier 调用期间借用。 */
	struct node_notify node_arg = {
		.nid = NUMA_NO_NODE,
	};
	unsigned long flags;
	char *reason;
	int ret;
	unsigned long normal_pages = 0;
	enum zone_type zt;
	/* reason 在每条失败跳转前赋值，仅用于最终诊断，不跨函数保存。 */

	/*
	 * {on,off}lining is constrained to full memory sections (or more
	 * precisely to memory blocks from the user space POV).
	 * memmap_on_memory is an exception because it reserves initial part
	 * of the physical memory space for vmemmaps. That space is pageblock
	 * aligned.
	 * 中文：起点至少 pageblock 对齐且结束为 section 边界，以兼容 self-hosted vmemmap。
	 */
	if (WARN_ON_ONCE(!nr_pages || !pageblock_aligned(start_pfn) ||
			 !IS_ALIGNED(start_pfn + nr_pages, PAGES_PER_SECTION)))
		return -EINVAL;

	/*
	 * Don't allow to offline memory blocks that contain holes.
	 * Consequently, memory blocks with holes can never get onlined
	 * via the hotplug path - online_pages() - as hotplugged memory has
	 * no holes. This way, we don't have to worry about memory holes,
	 * don't need pfn_valid() checks, and can avoid using
	 * walk_system_ram_range() later.
	 * 中文：拒绝含洞范围后，后续逐 PFN 可直接访问 struct page，无需反复 pfn_valid。
	 */
	walk_system_ram_range(start_pfn, nr_pages, &system_ram_pages,
			      count_system_ram_pages_cb);
	if (system_ram_pages != nr_pages) {
		/* 尚未关闭 PCP 或隔离页，直接走最外层诊断返回。 */
		ret = -EINVAL;
		reason = "memory holes";
		goto failed_removal;
	}

	/*
	 * We only support offlining of memory blocks managed by a single zone,
	 * checked by calling code. This is just a sanity check that we might
	 * want to remove in the future.
	 * 中文：完整范围必须属于调用者给定的单一 zone，否则计数与迁移目标不成立。
	 */
	if (WARN_ON_ONCE(page_zone(pfn_to_page(start_pfn)) != zone ||
			 page_zone(pfn_to_page(end_pfn - 1)) != zone)) {
		ret = -EINVAL;
		reason = "multizone range";
		goto failed_removal;
	}

	/*
	 * Disable pcplists so that page isolation cannot race with freeing
	 * in a way that pages from isolated pageblock are left on pcplists.
	 * 中文：先排空/禁用 PCP 与 LRU 缓存，防止隔离后仍有并发释放藏在 per-CPU 队列。
	 */
	zone_pcp_disable(zone);
	lru_cache_disable();

	/* set above range as isolated */
	/* 中文：把 pageblock 迁移类型改为 isolate，阻止新的分配进入目标范围。 */
	ret = start_isolate_page_range(start_pfn, end_pfn,
				       PB_ISOLATE_MODE_MEM_OFFLINE);
	if (ret) {
		reason = "failure to isolate range";
		goto failed_removal_pcplists_disabled;
	}

	/*
	 * Check whether the node will have no present pages after we offline
	 * 'nr_pages' more. If so, we know that the node will become empty, and
	 * so we will clear N_MEMORY for it.
	 * 中文：可能移除最后内存时先让节点消费者否决，成功提交后才清 N_MEMORY。
	 */
	if (nr_pages >= pgdat->node_present_pages) {
		node_arg.nid = node;
		ret = node_notify(NODE_REMOVING_LAST_MEMORY, &node_arg);
		ret = notifier_to_errno(ret);
		if (ret) {
			reason = "node notifier failure";
			goto failed_removal_isolated;
		}
	}

	ret = memory_notify(MEM_GOING_OFFLINE, &mem_arg);
	/* 内存消费者在页迁移前获得准备/否决机会。 */
	ret = notifier_to_errno(ret);
	if (ret) {
		reason = "notifier failure";
		goto failed_removal_isolated;
	}

	do {
		pfn = start_pfn;
		do {
			/*
			 * Historically we always checked for any signal and
			 * can't limit it to fatal signals without eventually
			 * breaking user space.
			 * 中文：历史 ABI 允许任意 pending signal 中断长下线循环，不能收紧为 fatal。
			 */
			if (signal_pending(current)) {
				ret = -EINTR;
				reason = "signal backoff";
				goto failed_removal_isolated;
			}

			cond_resched();

			ret = scan_movable_pages(pfn, end_pfn, &pfn);
			if (!ret) {
				/*
				 * TODO: fatal migration failures should bail
				 * out
				 * 中文：迁移器无返回状态，下一轮扫描会发现仍不可移动的页并最终退出。
				 */
				do_migrate_range(pfn, end_pfn);
			}
		} while (!ret);

		if (ret != -ENOENT) {
			reason = "unmovable page";
			goto failed_removal_isolated;
		}

		/*
		 * Dissolve free hugetlb folios in the memory block before doing
		 * offlining actually in order to make hugetlbfs's object
		 * counting consistent.
		 * 中文：在 buddy 摘页前溶解空闲 hugetlb folio，使 hugetlbfs 对象计数同步减少。
		 */
		ret = dissolve_free_hugetlb_folios(start_pfn, end_pfn);
		if (ret) {
			reason = "failure to dissolve huge pages";
			goto failed_removal_isolated;
		}

		ret = test_pages_isolated(start_pfn, end_pfn,
					  PB_ISOLATE_MODE_MEM_OFFLINE);

	} while (ret);

	/* Mark all sections offline and remove free pages from the buddy. */
	/* 中文：这是不可逆提交点：section 先离线，隔离的空闲页再从 buddy 永久摘除。 */
	managed_pages = __offline_isolated_pages(start_pfn, end_pfn);
	pr_debug("Offlined Pages %ld\n", nr_pages);

	/*
	 * The memory sections are marked offline, and the pageblock flags
	 * effectively stale; nobody should be touching them. Fixup the number
	 * of isolated pageblocks, memory onlining will properly revert this.
	 * 中文：离线后 pageblock flags 无人读取，直接修正 isolate 计数而不再逐块 undo。
	 */
	spin_lock_irqsave(&zone->lock, flags);
	zone->nr_isolate_pageblock -= nr_pages / pageblock_nr_pages;
	spin_unlock_irqrestore(&zone->lock, flags);

	lru_cache_enable();
	zone_pcp_enable(zone);

	/* removal success */
	/* 中文：managed 与 present 分别按实际 buddy 页数和完整物理页数扣减。 */
	adjust_managed_page_count(pfn_to_page(start_pfn), -managed_pages);
	adjust_present_page_count(pfn_to_page(start_pfn), group, -nr_pages);

	/* reinitialise watermarks and update pcp limits */
	/* 中文：容量缩小后立即重算水位，避免旧阈值驱动错误回收决策。 */
	init_per_zone_wmark_min();

	/*
	 * Check whether this operation removes the last normal memory from
	 * the node. We do this before clearing N_MEMORY to avoid the possible
	 * transient "!N_MEMORY && N_NORMAL_MEMORY" state.
	 * 中文：先清更具体的 NORMAL 状态，再清总 N_MEMORY，禁止观察到矛盾组合。
	 */
	if (zone_idx(zone) <= ZONE_NORMAL) {
		for (zt = 0; zt <= ZONE_NORMAL; zt++)
			normal_pages += pgdat->node_zones[zt].present_pages;
		if (!normal_pages)
			node_clear_state(node, N_NORMAL_MEMORY);
	}
	/*
	 * Make sure to mark the node as memory-less before rebuilding the zone
	 * list. Otherwise this node would still appear in the fallback lists.
	 * 中文：重建 zonelist 前发布 memoryless，分配回退链就不会重新包含空节点。
	 */
	if (node_arg.nid >= 0)
		node_clear_state(node, N_MEMORY);
	if (!populated_zone(zone)) {
		zone_pcp_reset(zone);
		build_all_zonelists(NULL);
	}

	if (node_arg.nid >= 0) {
		kcompactd_stop(node);
		kswapd_stop(node);
		/* Node went memoryless. Notify consumers */
		/* 中文：停止后台线程后再发送完成通知，消费者看到的是稳定空节点。 */
		node_notify(NODE_REMOVED_LAST_MEMORY, &node_arg);
	}

	writeback_set_ratelimit();

	memory_notify(MEM_OFFLINE, &mem_arg);
	remove_pfn_range_from_zone(zone, start_pfn, nr_pages);
	return 0;

failed_removal_isolated:
	/* pushback to free area */
	/* 中文：提交前失败可解除隔离，把页重新开放给 buddy，并对称发送取消通知。 */
	undo_isolate_page_range(start_pfn, end_pfn);
	memory_notify(MEM_CANCEL_OFFLINE, &mem_arg);
	if (node_arg.nid != NUMA_NO_NODE)
		node_notify(NODE_CANCEL_REMOVING_LAST_MEMORY, &node_arg);
failed_removal_pcplists_disabled:
	/* 隔离建立前后的失败最终都必须恢复 LRU 与 per-CPU 页缓存。 */
	lru_cache_enable();
	zone_pcp_enable(zone);
failed_removal:
	pr_debug("memory offlining [mem %#010llx-%#010llx] failed due to %s\n",
		 (unsigned long long) start_pfn << PAGE_SHIFT,
		 ((unsigned long long) end_pfn << PAGE_SHIFT) - 1,
		 reason);
	return ret;
}

/*
 * 业务背景：try_remove_memory() 拆设备前要证明范围内每个 memory_block 都已 offline；
 * 参数：mem 是 walker 借用项；arg 指向栈上 nid 输出，每轮覆盖为当前 block 节点；
 * 返回：OFFLINE 返回 0 继续，否则打印物理范围并返回 -EBUSY 停止；
 * 注意事项：持 device 锁保证 state 稳定；混合节点只保留最后 nid，用于尽力注销而非安全判断。
 */
static int check_memblock_offlined_cb(struct memory_block *mem, void *arg)
{
	int *nid = arg;

	*nid = mem->nid;
	/* 混合节点时最终保存最后访问的 nid，仅用于尽力尝试下线节点。 */
	if (unlikely(mem->state != MEM_OFFLINE)) {
		phys_addr_t beginpa, endpa;

		beginpa = PFN_PHYS(section_nr_to_pfn(mem->start_section_nr));
		endpa = beginpa + memory_block_size_bytes() - 1;
		pr_warn("removing memory fails, because memory [%pa-%pa] is onlined\n",
			&beginpa, &endpa);

		return -EBUSY;
	}
	return 0;
}

/*
 * 业务背景：remove 要选择整范围普通拆除或逐 block self-host 拆除，必须先统计 altmap；
 * 参数：mem 是 device 锁期借用项；arg 指向栈上 u64 输入/输出计数；
 * 返回：恒 0 继续；altmap 非空时计数加一，不转移其 ownership；
 * 注意事项：同步 walker 不保存指针；真正转移发生在 remove_memory_blocks_and_altmaps()。
 */
static int count_memory_range_altmaps_cb(struct memory_block *mem, void *arg)
{
	u64 *num_altmaps = (u64 *)arg;

	if (mem->altmap)
		/* altmap 指针只作存在性检查，其 ownership 仍属于 memory_block。 */
		*num_altmaps += 1;

	return 0;
}

/*
 * 业务背景：node 注销前不仅要无内存，还必须没有仍归属该节点的 present CPU；
 * 参数：nid 是候选节点号，CPU 对象只读借用；
 * 返回：未发现 CPU 返回 0，发现任一 present CPU 返回 -EBUSY；
 * 注意事项：调用者持 device hotplug 串行并由 CPU hotplug 协议稳定映射；不执行 CPU offline。
 */
static int check_cpu_on_node(int nid)
{
	int cpu;

	for_each_present_cpu(cpu) {
		if (cpu_to_node(cpu) == nid)
			/*
			 * the cpu on this node isn't removed, and we can't
			 * offline this node.
			 * 中文：节点仍承载 present CPU 时不能注销，CPU 热拔必须先完成。
			 */
			return -EBUSY;
	}

	return 0;
}

/*
 * 业务背景：离线 block 不计入 node span，却仍链接 sysfs 且未来可上线，因此也阻止 node 注销；
 * 参数：mem 是 walker 借用项；arg 指向只读目标 nid；
 * 返回：mem->nid 匹配返回 -EEXIST 停止，否则 0 继续；
 * 注意事项：持 device 锁；跨节点 block 的 nid 不可靠，但这类 block 始终在线并已被 span 检查排除。
 */
static int check_no_memblock_for_node_cb(struct memory_block *mem, void *arg)
{
	int nid = *(int *)arg;

	/*
	 * If a memory block belongs to multiple nodes, the stored nid is not
	 * reliable. However, such blocks are always online (e.g., cannot get
	 * offlined) and, therefore, are still spanned by the node.
	 * 中文：跨节点 block 的 nid 字段不精确，但它不能下线且会保持 node span，
	 * 因此前面的 span 检查已排除；这里的相等判断足以处理普通离线 block。
	 */
	return mem->nid == nid ? -EEXIST : 0;
}

/**
 * try_offline_node
 * @nid: the node ID
 *
 * Offline a node if all memory sections and cpus of the node are removed.
 *
 * NOTE: The caller must call lock_device_hotplug() to serialize hotplug
 * and online/offline operations before this call.
 * 业务背景：remove 完成后若节点不再承载页、block 或 CPU，应注销空 node 及其 sysfs 表示；
 * 中文：仅当节点无 span、无可再次上线的 memory_block 且无 present CPU 时，
 * 才清 online 位并注销 node；所有早退均表示节点仍需保留，不是错误返回。
 * 参数：nid 是候选节点号；函数只借用 node/block/CPU 状态；
 * 返回：void；任一依赖存在时静默保留，否则设置 offline 并 unregister_node；
 * 注意事项：调用者必须持 device_hotplug_lock；span 检查须早于离线 block/CPU 检查以覆盖 DEVICE/跨节点块。
 */
void try_offline_node(int nid)
{
	int rc;

	/*
	 * If the node still spans pages (especially ZONE_DEVICE), don't
	 * offline it. A node spans memory after move_pfn_range_to_zone(),
	 * e.g., after the memory block was onlined.
	 * 中文：任何普通或 ZONE_DEVICE span 都证明节点仍承载页，立即保留。
	 */
	if (node_spanned_pages(nid))
		return;

	/*
	 * Especially offline memory blocks might not be spanned by the
	 * node. They will get spanned by the node once they get onlined.
	 * However, they link to the node in sysfs and can get onlined later.
	 * 中文：离线 block 不计入 span，却仍通过 sysfs 依附节点且未来可上线，故也阻止注销。
	 */
	rc = for_each_memory_block(&nid, check_no_memblock_for_node_cb);
	if (rc)
		return;

	if (check_cpu_on_node(nid))
		return;

	/*
	 * all memory/cpu of this node are removed, we can offline this
	 * node now.
	 * 中文：三类依赖均消失后，先发布 offline 再注销设备模型节点。
	 */
	node_set_offline(nid);
	unregister_node(nid);
}
EXPORT_SYMBOL(try_offline_node);

/*
 * 业务背景：try_remove_memory() 必须为整范围选择普通或 self-hosted 拆除算法；
 * 参数：start/size 是 block 对齐物理字节范围，block 与 altmap 只读借用；
 * 返回：0 表示全无 altmap，1 表示每块都有，-EINVAL 表示混合布局；
 * 注意事项：调用者持 device 锁；全局功能关闭时快速返回 0，不转移任何 altmap ownership。
 */
static int memory_blocks_have_altmaps(u64 start, u64 size)
{
	u64 num_memblocks = size / memory_block_size_bytes();
	u64 num_altmaps = 0;

	if (!mhp_memmap_on_memory())
		/* 全局未启用时无需遍历 block 的 altmap 字段。 */
		return 0;

	walk_memory_blocks(start, size, &num_altmaps,
			   count_memory_range_altmaps_cb);

	if (num_altmaps == 0)
		return 0;

	if (WARN_ON_ONCE(num_memblocks != num_altmaps))
		/* 部分有/部分无不能选择单一安全拆除路径，拒绝继续。 */
		return -EINVAL;

	return 1;
}

/*
 * 业务背景：公共/组合 remove 在 device 锁下共用实际事务，先验证离线再永久拆物理映射；
 * 参数：start/size 是合法且 block 对齐的物理字节范围；
 * 返回：0 表示 device/arch/memblock/iomem 均拆除；在线 block 或混合 altmap 返回 errno；
 * 注意事项：入口持 device_hotplug_lock，内部再持 hotplug 写锁；firmware_map_remove 早于 altmap 异常检查。
 */
static int try_remove_memory(u64 start, u64 size)
{
	int rc, nid = NUMA_NO_NODE;

	BUG_ON(check_hotplug_memory_range(start, size));

	/*
	 * All memory blocks must be offlined before removing memory.  Check
	 * whether all memory blocks in question are offline and return error
	 * if this is not the case.
	 *
	 * While at it, determine the nid. Note that if we'd have mixed nodes,
	 * we'd only try to offline the last determined one -- which is good
	 * enough for the cases we care about.
	 * 中文：先在设备锁保护下验证每块 OFFLINE，同时取得候选 nid；失败时尚未修改资源。
	 */
	rc = walk_memory_blocks(start, size, &nid, check_memblock_offlined_cb);
	if (rc)
		return rc;

	/* remove memmap entry */
	/* 中文：固件热插拔映射先撤销；driver-managed 范围没有该条目，删除为空操作。 */
	firmware_map_remove(start, start + size, "System RAM");

	mem_hotplug_begin();
	/* 从此架构映射与节点拓扑在拆除完成前不会并发变化。 */

	rc = memory_blocks_have_altmaps(start, size);
	if (rc < 0) {
		mem_hotplug_done();
		return rc;
	} else if (!rc) {
		/*
		 * Memory block device removal under the device_hotplug_lock is
		 * a barrier against racing online attempts.
		 * No altmaps present, do the removal directly
		 * 中文：device_hotplug_lock 阻止已验证离线的设备被并发重新上线；普通布局
		 * 可整范围先删设备，再让架构拆映射。
		 */
		remove_memory_block_devices(start, size);
		arch_remove_memory(start, size, NULL, NULL);
	} else {
		/* all memblocks in the range have altmaps */
		/* 中文：每块都 self-hosted，逐块转移并释放各自 altmap。 */
		remove_memory_blocks_and_altmaps(start, size);
	}

	if (IS_ENABLED(CONFIG_ARCH_KEEP_MEMBLOCK))
		/* 添加时写入的 memblock 记录在架构映射消失后对称撤销。 */
		memblock_remove(start, size);

	release_mem_region_adjustable(start, size);
	/* 最后释放 iomem 所有权，使未来设备可重新声明该物理区间。 */

	if (nid != NUMA_NO_NODE)
		try_offline_node(nid);

	mem_hotplug_done();
	return 0;
}

/**
 * __remove_memory - Remove memory if every memory block is offline
 * @start: physical address of the region to remove
 * @size: size of the region to remove
 *
 * NOTE: The caller must call lock_device_hotplug() to serialize hotplug
 * and online/offline operations before this call, as required by
 * try_offline_node().
 * 业务背景：某些内部调用链已把“范围必须可移除”作为不可恢复契约，需要 void 强包装；
 * 中文：内部/架构调用者使用的强契约包装；任何 block 未离线或移除失败均视为 BUG。
 * 参数：start/size 是 block 对齐物理字节范围，调用者已证明所有 block offline；
 * 返回：void；成功后资源和映射消失，任何 try_remove_memory errno 都触发 BUG；
 * 注意事项：入口必须已持 device_hotplug_lock；只适合无法合理恢复的强契约调用链。
 */
void __remove_memory(u64 start, u64 size)
{

	/*
	 * trigger BUG() if some memory is not offlined prior to calling this
	 * function
	 * 中文：void API 无法传回错误，因此调用前必须已证明全范围离线。
	 */
	if (try_remove_memory(start, size))
		BUG();
}

/*
 * Remove memory if every memory block is offline, otherwise return -EBUSY is
 * some memory is not offline
 * 业务背景：导出 API 必须允许驱动尝试移除并正常处理 block 仍在线等可恢复失败；
 * 中文：可失败公共入口自行取得设备热插拔锁；成功后资源与架构映射均已删除。
 * 参数：start/size 是待移除的物理字节范围，必须非零且 block 对齐；
 * 返回：0 成功，在线 block 返回 -EBUSY，或传播布局检查 errno；
 * 注意事项：可睡眠并自行持 device_hotplug_lock；与 __remove_memory 不同，失败不会 BUG。
 */
int remove_memory(u64 start, u64 size)
{
	int rc;

	lock_device_hotplug();
	rc = try_remove_memory(start, size);
	unlock_device_hotplug();

	return rc;
}
EXPORT_SYMBOL_GPL(remove_memory);

/*
 * 业务背景：组合 API 要逐块下线，并保存成功块原来的 KERNEL/MOVABLE 类型以便失败回滚；
 * 参数：mem 是 walker 借用项；arg 是 uint8_t **游标，指向当前 block 的回滚槽位；
 * 返回：负 device_offline errno 停止；已离线的正返回归一为 0；每次都推进游标；
 * 注意事项：持 device 锁；槽位仅在真正成功下线时从 OFFLINE 哨兵改写，单 block 多 zone 由下层拒绝。
 */
static int try_offline_memory_block(struct memory_block *mem, void *arg)
{
	enum mmop online_type = MMOP_ONLINE_KERNEL;
	uint8_t **online_types = arg;
	struct page *page;
	int rc;

	/*
	 * Sense the online_type via the zone of the memory block. Offlining
	 * with multiple zones within one memory block will be rejected
	 * by offlining code ... so we don't care about that.
	 * 中文：单 block 多 zone 会由下层拒绝；这里只需用首页判断 KERNEL/MOVABLE。
	 */
	page = pfn_to_online_page(section_nr_to_pfn(mem->start_section_nr));
	if (page && page_zonenum(page) == ZONE_MOVABLE)
		online_type = MMOP_ONLINE_MOVABLE;

	rc = device_offline(&mem->dev);
	/* 设备状态机内部取得 mem_hotplug_lock 并执行完整 offline_pages。 */
	/*
	 * Default is MMOP_OFFLINE - change it only if offlining succeeded,
	 * so try_reonline_memory_block() can do the right thing.
	 * 中文：初值 OFFLINE 同时表示“原本离线”或“尚未处理”，两者回滚都应跳过。
	 */
	if (!rc)
		**online_types = online_type;

	(*online_types)++;
	/* Ignore if already offline. */
	/* 中文：device_offline 的正返回表示已离线，不是失败；只传播负 errno。 */
	return rc < 0 ? rc : 0;
}

/*
 * 业务背景：某块下线或最终 remove 失败后，要按保存类型尽力恢复此前成功下线的 blocks；
 * 参数：mem 是 walker 借用项；arg 是 uint8_t **游标，OFFLINE 槽位表示跳过；
 * 返回：恒 0 以处理完所有块并推进游标；device_online 失败只告警；
 * 注意事项：持 device 锁；恢复可能被 notifier 否决，故只能尽力，显式类型避免策略改变原 zone。
 */
static int try_reonline_memory_block(struct memory_block *mem, void *arg)
{
	uint8_t **online_types = arg;
	int rc;

	if (**online_types != MMOP_OFFLINE) {
		/* 恢复显式类型，避免自动策略变化把原 MOVABLE block 上线到 kernel zone。 */
		mem->online_type = (enum mmop)**online_types;
		rc = device_online(&mem->dev);
		if (rc < 0)
			pr_warn("%s: Failed to re-online memory: %d",
				__func__, rc);
	}

	/* Continue processing all remaining memory blocks. */
	/* 中文：重上线失败只告警，回调仍返回 0 以继续恢复其他 block。 */
	(*online_types)++;
	return 0;
}

/*
 * Try to offline and remove memory. Might take a long time to finish in case
 * memory is still in use. Primarily useful for memory devices that logically
 * unplugged all memory (so it's no longer in use) and want to offline + remove
 * that memory.
 * 业务背景：逻辑拔除设备希望用单一 API 先下线所有 blocks 再永久移除，并在中途失败时恢复；
 * 中文：为逻辑上已拔除的设备提供“逐块下线后整段移除”；可能因迁移在用页耗时。
 * 任一块下线或最终移除失败时，按保存的 KERNEL/MOVABLE 类型尽力重上线已处理前缀。
 * 参数：start/size 是非零、block 对齐的物理字节范围；函数分配每块一个回滚槽位；
 * 返回：0 表示全部下线并移除；-EINVAL/-ENOMEM、下线或 remove errno 表示失败并已尝试回滚；
 * 注意事项：可长时间睡眠并自行持 device 锁；回滚重上线仍可能失败，数组只在所有回调结束后释放。
 */
int offline_and_remove_memory(u64 start, u64 size)
{
	const unsigned long mb_count = size / memory_block_size_bytes();
	uint8_t *online_types, *tmp;
	int rc;

	/* 在分配回滚数组前拒绝空范围和非 block 对齐参数。 */
	if (!IS_ALIGNED(start, memory_block_size_bytes()) ||
	    !IS_ALIGNED(size, memory_block_size_bytes()) || !size)
		return -EINVAL;

	/*
	 * We'll remember the old online type of each memory block, so we can
	 * try to revert whatever we did when offlining one memory block fails
	 * after offlining some others succeeded.
	 * 中文：每块一个字节足以保存 MMOP_*；数组生命周期覆盖设备锁临界区。
	 */
	online_types = kmalloc_array(mb_count, sizeof(*online_types),
				     GFP_KERNEL);
	if (!online_types)
		return -ENOMEM;
	/*
	 * Initialize all states to MMOP_OFFLINE, so when we abort processing in
	 * try_offline_memory_block(), we'll skip all unprocessed blocks in
	 * try_reonline_memory_block().
	 * 中文：统一 OFFLINE 哨兵让未访问槽位和原已离线块在回滚时自然跳过。
	 */
	memset(online_types, MMOP_OFFLINE, mb_count);

	lock_device_hotplug();
	/* 锁住后 block 集合、顺序和设备状态在两遍遍历间保持稳定。 */

	tmp = online_types;
	rc = walk_memory_blocks(start, size, &tmp, try_offline_memory_block);

	/*
	 * In case we succeeded to offline all memory, remove it.
	 * This cannot fail as it cannot get onlined in the meantime.
	 * 中文：全部下线后仍持设备锁，理论上 try_remove 不会再因并发上线失败。
	 */
	if (!rc) {
		rc = try_remove_memory(start, size);
		if (rc)
			pr_err("%s: Failed to remove memory: %d", __func__, rc);
	}

	/*
	 * Rollback what we did. While memory onlining might theoretically fail
	 * (nacked by a notifier), it barely ever happens.
	 * 中文：回滚重新上线仍可能被通知器否决，因此是尽力恢复并逐项告警。
	 */
	if (rc) {
		tmp = online_types;
		walk_memory_blocks(start, size, &tmp,
				   try_reonline_memory_block);
	}
	unlock_device_hotplug();

	kfree(online_types);
	/* 设备锁释放后数组不再被回调引用，可安全释放并返回首个事务错误。 */
	return rc;
}
EXPORT_SYMBOL_GPL(offline_and_remove_memory);
#endif /* CONFIG_MEMORY_HOTREMOVE */
