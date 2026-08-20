// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/vmstat.c
 *
 *  Manages VM statistics
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  zoned VM statistics
 *  Copyright (C) 2006 Silicon Graphics, Inc.,
 *		Christoph Lameter <cl@gentwo.org>
 *  Copyright (C) 2008-2014 Christoph Lameter
 */
/*
 * ============================================================================
 * 【vmstat - 虚拟内存统计（Virtual Memory Statistics）】
 *
 * 【文件功能】
 * 管理和导出 Linux 内核的虚拟内存统计信息，提供内存使用情况的实时监控。
 *
 * 【核心功能】
 * 1. 统计收集：收集各种内存相关的统计数据
 *    - 页面分配/释放计数
 *    - 页面回收统计
 *    - 内存区域（zone）统计
 *    - NUMA 节点统计
 *    - 页面迁移统计
 *
 * 2. Per-CPU 计数器：使用 per-CPU 变量减少锁竞争
 *    - 每个 CPU 独立维护计数器
 *    - 定期合并到全局计数器
 *    - 提供差分阈值机制
 *
 * 3. 统计导出：通过多种接口导出统计信息
 *    - /proc/vmstat: 全局虚拟内存统计
 *    - /proc/zoneinfo: 各内存区域详细信息
 *    - /proc/pagetypeinfo: 按页面类型统计
 *    - /sys/devices/system/node/nodeN/vmstat: NUMA 节点统计
 *
 * 【统计类型】
 *
 * 1. 全局计数器（vm_event_states）
 *    - pgalloc_*: 页面分配次数
 *    - pgfree: 页面释放次数
 *    - pgfault: 页面错误次数
 *    - pgmajfault: 主页面错误（需要磁盘 I/O）
 *
 * 2. Zone 计数器（zone->vm_stat）
 *    - nr_free_pages: 空闲页面数
 *    - nr_inactive_anon: 非活动匿名页面数
 *    - nr_active_file: 活动文件页面数
 *
 * 3. Node 计数器（pglist_data->vm_stat）
 *    - nr_writeback: 正在回写的页面数
 *    - nr_slab_reclaimable: 可回收的 slab 页面数
 *
 * 4. NUMA 统计（numa_event）
 *    - numa_hit: NUMA 本地分配成功
 *    - numa_miss: NUMA 远程分配
 *    - numa_foreign: 其他节点的远程访问
 *
 * 【Per-CPU 差分机制】
 * 为了性能，统计更新使用 per-CPU 差分：
 * 1. 每次更新只修改本地 CPU 的差分值（无锁）
 * 2. 差分累积到阈值时，批量合并到全局计数器（需锁）
 * 3. 阈值动态调整（基于内存区域大小和 CPU 数量）
 *
 * 【刷新机制】
 * 1. 主动刷新：读取统计信息时触发（如读 /proc/vmstat）
 * 2. 被动刷新：差分达到阈值时自动刷新
 * 3. 定期刷新：vmstat 工作队列定期合并计数器
 *
 * 【使用场景】
 * - 系统监控：通过 vmstat、sar 等工具监控内存
 * - 性能分析：分析页面分配/回收模式
 * - 故障诊断：查看内存压力、页面错误等
 * - 容量规划：了解内存使用趋势
 *
 * 【相关工具】
 * - vmstat: 报告虚拟内存统计
 * - sar: 系统活动报告
 * - /proc/meminfo: 内存信息概览
 * - atop/htop: 实时系统监控
 *
 * 【性能考虑】
 * - Per-CPU 计数器：避免缓存行乒乓
 * - 差分阈值：减少全局更新频率
 * - 延迟合并：批量处理减少开销
 * - 静态键优化：NUMA 统计可动态开关
 * ============================================================================
 */
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/vmstat.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/sched.h>
#include <linux/math64.h>
#include <linux/writeback.h>
#include <linux/compaction.h>
#include <linux/mm_inline.h>
#include <linux/page_owner.h>
#include <linux/sched/isolation.h>

#include "internal.h"

#ifdef CONFIG_PROC_FS
#ifdef CONFIG_NUMA
/*
 * ============================================================================
 * 【NUMA 统计管理】
 *
 * NUMA（Non-Uniform Memory Access）统计用于监控跨节点的内存访问模式。
 * ============================================================================
 */

#define ENABLE_NUMA_STAT 1
static int sysctl_vm_numa_stat = ENABLE_NUMA_STAT;
/* NUMA 统计开关
 * - 1: 启用 NUMA 统计（默认）
 * - 0: 禁用 NUMA 统计（降低开销）
 *
 * 【性能影响】
 * NUMA 统计有一定开销，在不需要时可以禁用。
 *
 * 【控制接口】
 * /proc/sys/vm/numa_stat
 */

/* zero numa counters within a zone */
/*
 * 【函数】zero_zone_numa_counters - 清零单个 zone 的 NUMA 计数器
 * @zone: 要清零的内存区域
 *
 * 【工作内容】
 * 1. 清零 zone 的全局 NUMA 计数器
 * 2. 清零所有 CPU 的 per-CPU NUMA 计数器
 */
static void zero_zone_numa_counters(struct zone *zone)
{
	int item, cpu;

	for (item = 0; item < NR_VM_NUMA_EVENT_ITEMS; item++) {
		atomic_long_set(&zone->vm_numa_event[item], 0);
		/* 清零全局计数器
		 * vm_numa_event 包含：
		 * - NUMA_HIT: 本地节点分配成功
		 * - NUMA_MISS: 远程节点分配
		 * - NUMA_FOREIGN: 其他节点对本节点的访问
		 * - NUMA_INTERLEAVE_HIT: 交错分配命中
		 * - NUMA_LOCAL: 本地 CPU 本地节点分配
		 * - NUMA_OTHER: 本地 CPU 远程节点分配
		 */

		for_each_online_cpu(cpu) {
			per_cpu_ptr(zone->per_cpu_zonestats, cpu)->vm_numa_event[item]
						= 0;
		}
		/* 清零每个 CPU 的差分计数器 */
	}
}

/* zero numa counters of all the populated zones */
/*
 * 【函数】zero_zones_numa_counters - 清零所有已填充 zone 的 NUMA 计数器
 *
 * 【使用场景】
 * 禁用 NUMA 统计时，清理旧数据。
 */
static void zero_zones_numa_counters(void)
{
	struct zone *zone;

	for_each_populated_zone(zone)
		zero_zone_numa_counters(zone);
	/* 遍历所有已填充的内存区域并清零
	 * populated zone: 包含实际物理内存的 zone
	 */
}

/* zero global numa counters */
/*
 * 【函数】zero_global_numa_counters - 清零全局 NUMA 计数器
 *
 * 清零系统级别的 NUMA 统计（跨所有 node 和 zone）。
 */
static void zero_global_numa_counters(void)
{
	int item;

	for (item = 0; item < NR_VM_NUMA_EVENT_ITEMS; item++)
		atomic_long_set(&vm_numa_event[item], 0);
	/* vm_numa_event: 全局 NUMA 事件计数器数组 */
}

/*
 * 【函数】invalid_numa_statistics - 使 NUMA 统计失效（清零）
 *
 * 【调用时机】
 * 禁用 NUMA 统计时调用，确保旧数据不会误导用户。
 */
static void invalid_numa_statistics(void)
{
	zero_zones_numa_counters();
	zero_global_numa_counters();
}

static DEFINE_MUTEX(vm_numa_stat_lock);
/* NUMA 统计配置锁
 * 保护 sysctl_vm_numa_stat 的并发修改
 */

/*
 * 【函数】sysctl_vm_numa_stat_handler - NUMA 统计 sysctl 处理函数
 * @table:  sysctl 表项
 * @write:  是否是写操作
 * @buffer: 用户缓冲区
 * @length: 缓冲区长度
 * @ppos:   文件位置
 *
 * 返回值：0=成功，负值=失败
 *
 * 【功能】
 * 处理 /proc/sys/vm/numa_stat 的读写：
 * - 读：返回当前值（0 或 1）
 * - 写：启用或禁用 NUMA 统计
 *
 * 【工作流程】
 * 1. 加锁保护
 * 2. 保存旧值（如果是写操作）
 * 3. 调用标准处理函数（proc_dointvec_minmax）
 * 4. 如果值改变：
 *    - 启用：打开静态键，开始收集统计
 *    - 禁用：关闭静态键，清零计数器
 * 5. 解锁
 *
 * 【静态键优化】
 * 使用静态键（static key）实现零开销的条件编译：
 * - 启用时：NUMA 统计代码正常执行
 * - 禁用时：NUMA 统计代码被编译为 nop（无操作）
 */
static int sysctl_vm_numa_stat_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *length, loff_t *ppos)
{
	int ret, oldval;

	mutex_lock(&vm_numa_stat_lock);
	if (write)
		oldval = sysctl_vm_numa_stat;
	/* 保存旧值，用于检测是否改变 */

	ret = proc_dointvec_minmax(table, write, buffer, length, ppos);
	/* 调用标准 sysctl 处理函数
	 * proc_dointvec_minmax: 处理有范围限制的整数
	 */

	if (ret || !write)
		goto out;
	/* 如果失败或是读操作，直接返回 */

	if (oldval == sysctl_vm_numa_stat)
		goto out;
	/* 值未改变，无需操作 */

	else if (sysctl_vm_numa_stat == ENABLE_NUMA_STAT) {
		static_branch_enable(&vm_numa_stat_key);
		pr_info("enable numa statistics\n");
		/* 启用 NUMA 统计
		 * static_branch_enable: 将静态键设为"真"，
		 * NUMA 统计代码开始执行
		 */
	} else {
		static_branch_disable(&vm_numa_stat_key);
		invalid_numa_statistics();
		pr_info("disable numa statistics, and clear numa counters\n");
		/* 禁用 NUMA 统计
		 * 1. 关闭静态键（NUMA 代码变为 nop）
		 * 2. 清零所有 NUMA 计数器（避免显示陈旧数据）
		 * 3. 打印日志通知管理员
		 */
	}

out:
	mutex_unlock(&vm_numa_stat_lock);
	return ret;
}
#endif
#endif /* CONFIG_PROC_FS */

#ifdef CONFIG_VM_EVENT_COUNTERS
/*
 * ============================================================================
 * 【VM 事件计数器（VM Event Counters）】
 *
 * VM 事件计数器跟踪内存管理子系统中的各种事件。
 * 使用 per-CPU 变量以提高性能。
 * ============================================================================
 */

DEFINE_PER_CPU(struct vm_event_state, vm_event_states) = {{0}};
EXPORT_PER_CPU_SYMBOL(vm_event_states);
/* Per-CPU VM 事件状态
 *
 * 【结构】
 * struct vm_event_state {
 *     unsigned long event[NR_VM_EVENT_ITEMS];
 * };
 *
 * 【事件类型】
 * - pgalloc_*: 页面分配事件（按 zone 类型）
 * - pgfree: 页面释放事件
 * - pgfault: 页面错误（缺页中断）
 * - pgmajfault: 主页面错误（需要磁盘 I/O）
 * - pgrefill: 页面重新填充到 LRU
 * - pgscan_*: 页面扫描（用于回收）
 * - pgsteal_*: 页面窃取（回收成功）
 * - pswpin/pswpout: 页面交换进/出
 * - 等等...
 *
 * 【Per-CPU 的好处】
 * - 无锁更新：每个 CPU 只修改自己的计数器
 * - 避免缓存行乒乓：不同 CPU 访问不同的缓存行
 * - 高性能：内存管理路径上的开销最小化
 */

/*
 * 【函数】sum_vm_events - 汇总所有 CPU 的 VM 事件计数器
 * @ret: 输出数组，存储汇总后的事件计数
 *
 * 【工作内容】
 * 1. 清零输出数组
 * 2. 遍历所有在线 CPU
 * 3. 累加每个事件的计数
 *
 * 【注意】
 * 结果是近似值，可能在执行过程中变化（其他 CPU 继续更新）。
 */
static void sum_vm_events(unsigned long *ret)
{
	int cpu;
	int i;

	memset(ret, 0, NR_VM_EVENT_ITEMS * sizeof(unsigned long));
	/* 清零输出数组 */

	for_each_online_cpu(cpu) {
		struct vm_event_state *this = &per_cpu(vm_event_states, cpu);
		/* 获取该 CPU 的事件状态 */

		for (i = 0; i < NR_VM_EVENT_ITEMS; i++)
			ret[i] += this->event[i];
		/* 累加所有事件类型 */
	}
}

/*
 * Accumulate the vm event counters across all CPUs.
 * The result is unavoidably approximate - it can change
 * during and after execution of this function.
*/
/*
 * 【函数】all_vm_events - 获取所有 CPU 的 VM 事件累计值
 * @ret: 输出数组，存储累计的事件计数
 *
 * 【功能】
 * 跨所有 CPU 累计 VM 事件计数器。
 *
 * 【注意】
 * 结果不可避免地是近似值——在函数执行期间和之后可能会变化。
 *
 * 【CPU hotplug 保护】
 * 使用 cpus_read_lock/unlock 防止 CPU 在统计过程中上线/下线。
 *
 * 【调用者】
 * - /proc/vmstat 读取时
 * - 内核内部需要查看全局统计时
 */
void all_vm_events(unsigned long *ret)
{
	cpus_read_lock();
	/* 获取 CPU hotplug 读锁
	 * 防止在统计过程中 CPU 上线或下线
	 */

	sum_vm_events(ret);
	/* 汇总所有 CPU 的事件计数 */

	cpus_read_unlock();
	/* 释放 CPU hotplug 读锁 */
}
EXPORT_SYMBOL_GPL(all_vm_events);
/* 导出符号，供模块使用（GPL 许可） */

/*
 * Fold the foreign cpu events into our own.
 *
 * This is adding to the events on one processor
 * but keeps the global counts constant.
 */
/*
 * 【函数】vm_events_fold_cpu - 将外部 CPU 的事件合并到当前 CPU
 * @cpu: 要合并的 CPU 编号
 *
 * 【功能】
 * 将指定 CPU 的事件计数器折叠（fold）到全局计数器。
 *
 * 【使用场景】
 * - CPU 下线前：将其事件计数转移到全局，避免丢失
 * - 周期性合并：定期将 per-CPU 计数合并到全局
 *
 * 【工作原理】
 * 1. 读取指定 CPU 的事件计数
 * 2. 将计数添加到全局计数器（通过 count_vm_events）
 * 3. 清零该 CPU 的计数
 *
 * 【保持不变性】
 * 这是将事件从一个处理器转移到全局，
 * 全局总计数保持不变。
 */
void vm_events_fold_cpu(int cpu)
{
	struct vm_event_state *fold_state = &per_cpu(vm_event_states, cpu);
	/* 获取要折叠的 CPU 的事件状态 */

	int i;

	for (i = 0; i < NR_VM_EVENT_ITEMS; i++) {
		count_vm_events(i, fold_state->event[i]);
		/* 将该 CPU 的计数添加到全局
		 * count_vm_events: 更新当前 CPU 的事件计数
		 * （这会将计数分散到当前活动的 CPU 上）
		 */

		fold_state->event[i] = 0;
		/* 清零原 CPU 的计数（已转移） */
	}
}

#endif /* CONFIG_VM_EVENT_COUNTERS */

/*
 * Manage combined zone based / global counters
 *
 * vm_stat contains the global counters
 */
/*
 * ============================================================================
 * 【Zone 和 Node 全局计数器】
 *
 * 这些计数器跟踪内存区域（zone）和 NUMA 节点（node）级别的统计信息。
 * ============================================================================
 */

atomic_long_t vm_zone_stat[NR_VM_ZONE_STAT_ITEMS] __cacheline_aligned_in_smp;
/* Zone 级别的全局统计数组
 *
 * 【包含的统计项】
 * - NR_FREE_PAGES: 空闲页面数
 * - NR_ZONE_INACTIVE_ANON: 非活动匿名页面
 * - NR_ZONE_ACTIVE_ANON: 活动匿名页面
 * - NR_ZONE_INACTIVE_FILE: 非活动文件页面
 * - NR_ZONE_ACTIVE_FILE: 活动文件页面
 * - NR_ZONE_UNEVICTABLE: 不可回收页面
 * - NR_ZONE_WRITE_PENDING: 待写回页面
 * 等等...
 *
 * 【缓存行对齐】
 * __cacheline_aligned_in_smp: 在 SMP 系统中对齐到缓存行边界，
 * 避免伪共享（false sharing）提高性能。
 *
 * 【原子操作】
 * 使用 atomic_long_t 支持无锁的原子更新。
 */

atomic_long_t vm_node_stat[NR_VM_NODE_STAT_ITEMS] __cacheline_aligned_in_smp;
/* Node 级别的全局统计数组
 *
 * 【包含的统计项】
 * - NR_INACTIVE_ANON: 节点级非活动匿名页面
 * - NR_ACTIVE_ANON: 节点级活动匿名页面
 * - NR_INACTIVE_FILE: 节点级非活动文件页面
 * - NR_ACTIVE_FILE: 节点级活动文件页面
 * - NR_UNEVICTABLE: 节点级不可回收页面
 * - NR_SLAB_RECLAIMABLE: 可回收的 slab 页面
 * - NR_SLAB_UNRECLAIMABLE: 不可回收的 slab 页面
 * - NR_ISOLATED_ANON: 隔离的匿名页面
 * - NR_ISOLATED_FILE: 隔离的文件页面
 * - NR_PAGES_SCANNED: 扫描的页面数
 * - WORKINGSET_*: 工作集相关统计
 * 等等...
 *
 * 【Zone vs Node】
 * - Zone 统计：特定内存区域的统计（DMA, DMA32, Normal, Movable）
 * - Node 统计：NUMA 节点级别的统计（跨该节点的所有 zone）
 */

atomic_long_t vm_numa_event[NR_VM_NUMA_EVENT_ITEMS] __cacheline_aligned_in_smp;
/* 全局 NUMA 事件计数器数组
 *
 * 【包含的事件】
 * - NUMA_HIT: 本地节点分配成功
 * - NUMA_MISS: 请求本地但实际分配在远程
 * - NUMA_FOREIGN: 其他节点请求本节点
 * - NUMA_INTERLEAVE_HIT: 交错策略分配成功
 * - NUMA_LOCAL: 本地 CPU 本地节点分配
 * - NUMA_OTHER: 本地 CPU 远程节点分配
 */

EXPORT_SYMBOL(vm_zone_stat);
EXPORT_SYMBOL(vm_node_stat);
/* 导出符号，供模块访问统计数据 */

#ifdef CONFIG_NUMA
/*
 * 【函数】fold_vm_zone_numa_events - 折叠指定 zone 的 NUMA 事件
 * @zone: 要折叠的内存区域
 *
 * 【功能】
 * 将该 zone 在所有 CPU 上的 per-CPU NUMA 事件计数器合并到全局。
 *
 * 【工作流程】
 * 1. 创建临时数组累计所有 CPU 的 NUMA 事件
 * 2. 遍历所有在线 CPU
 * 3. 使用 xchg 原子交换读取并清零每个 CPU 的计数
 * 4. 将累计值添加到全局 NUMA 事件计数器
 *
 * 【xchg 的作用】
 * - 原子操作：读取并清零在一个操作中完成
 * - 避免竞态：保证不会丢失计数
 * - 性能考虑：避免锁的开销
 */
static void fold_vm_zone_numa_events(struct zone *zone)
{
	unsigned long zone_numa_events[NR_VM_NUMA_EVENT_ITEMS] = { 0, };
	/* 临时数组，用于累计该 zone 的 NUMA 事件 */

	int cpu;
	enum numa_stat_item item;

	for_each_online_cpu(cpu) {
		struct per_cpu_zonestat *pzstats;
		/* per-CPU zone 统计结构 */

		pzstats = per_cpu_ptr(zone->per_cpu_zonestats, cpu);
		/* 获取该 CPU 在此 zone 的统计数据指针 */

		for (item = 0; item < NR_VM_NUMA_EVENT_ITEMS; item++)
			zone_numa_events[item] += xchg(&pzstats->vm_numa_event[item], 0);
		/* 原子交换：读取当前值并设为 0
		 * 累加到临时数组中
		 */
	}

	for (item = 0; item < NR_VM_NUMA_EVENT_ITEMS; item++)
		zone_numa_event_add(zone_numa_events[item], zone, item);
	/* 将累计的事件添加到全局 NUMA 事件计数器 */
}

/*
 * 【函数】fold_vm_numa_events - 折叠所有 zone 的 NUMA 事件
 *
 * 【功能】
 * 遍历所有已填充（populated）的 zone，将其 NUMA 事件合并到全局。
 *
 * 【调用时机】
 * - 周期性统计刷新
 * - /proc/vmstat 读取前
 * - 需要准确 NUMA 统计信息时
 *
 * 【为什么只处理 populated zone】
 * 空的 zone 没有内存页面，不会产生 NUMA 事件。
 */
void fold_vm_numa_events(void)
{
	struct zone *zone;

	for_each_populated_zone(zone)
		fold_vm_zone_numa_events(zone);
	/* 遍历每个已填充的 zone 并折叠其 NUMA 事件 */
}
#endif

#ifdef CONFIG_SMP

/*
 * ============================================================================
 * 【阈值计算（Threshold Calculation）】
 *
 * vmstat 使用阈值机制来平衡准确性和性能：
 * - per-CPU 计数器累积变化
 * - 当变化超过阈值时，才同步到全局计数器
 * - 阈值根据系统配置动态计算
 * ============================================================================
 */

/*
 * 【函数】calculate_pressure_threshold - 计算压力阈值
 * @zone: 内存区域
 * @return: 压力阈值（页面数）
 *
 * 【功能】
 * 计算用于内存压力检测的阈值。
 *
 * 【设计考虑】
 * vmstat 不是实时更新的，估计值和实际值之间存在漂移（drift）。
 * 对于高阈值和大量 CPU 的系统，即使估计值看起来正常，
 * 最小水位线（min watermark）也可能被突破。
 *
 * 【压力阈值的作用】
 * 这是一个减小的阈值，即使最大漂移量也不会意外突破最小水位线。
 *
 * 【计算公式】
 * threshold = (low_wmark - min_wmark) / num_online_cpus()
 * - 基于水位线距离（watermark distance）
 * - 除以 CPU 数量：更多 CPU = 更大的潜在漂移
 * - 至少为 1，最多为 125
 *
 * 【为什么需要压力阈值】
 * 当内存接近最小水位线时，需要更保守的阈值，
 * 以便及时触发内存回收，防止分配失败。
 */
int calculate_pressure_threshold(struct zone *zone)
{
	int threshold;
	int watermark_distance;

	/*
	 * As vmstats are not up to date, there is drift between the estimated
	 * and real values. For high thresholds and a high number of CPUs, it
	 * is possible for the min watermark to be breached while the estimated
	 * value looks fine. The pressure threshold is a reduced value such
	 * that even the maximum amount of drift will not accidentally breach
	 * the min watermark
	 */
	/*
	 * 由于 vmstat 不是实时的，估计值和实际值之间存在漂移。
	 * 对于高阈值和大量 CPU，当估计值看起来正常时，
	 * 最小水位线可能已被突破。压力阈值是一个减小的值，
	 * 即使最大漂移量也不会意外突破最小水位线。
	 */

	watermark_distance = low_wmark_pages(zone) - min_wmark_pages(zone);
	/* 计算低水位和最小水位之间的距离（页面数）
	 * 这是内存压力的"缓冲区"
	 */

	threshold = max(1, (int)(watermark_distance / num_online_cpus()));
	/* 阈值 = 水位距离 / CPU 数量
	 * 每个 CPU 可能独立地导致漂移，所以要除以 CPU 数
	 * 至少为 1 页，避免阈值为 0
	 */

	/*
	 * Maximum threshold is 125
	 */
	/*
	 * 最大阈值是 125
	 * （经验值，平衡性能和响应性）
	 */
	threshold = min(125, threshold);

	return threshold;
}

/*
 * 【函数】calculate_normal_threshold - 计算正常阈值
 * @zone: 内存区域
 * @return: 正常阈值（页面数）
 *
 * 【功能】
 * 计算 per-CPU 计数器的正常同步阈值。
 *
 * 【阈值缩放规则】
 * 阈值根据处理器数量和每个 zone 的内存量进行缩放：
 * - 更多内存：可以延迟更新更长时间
 * - 更多处理器：可能导致更多竞争
 *
 * 【fls() 函数】
 * fls (find last set) 用于廉价的对数缩放。
 * fls(x) 返回 x 中最高位 1 的位置（从 1 开始）。
 * 例如：fls(1)=1, fls(2)=2, fls(4)=3, fls(8)=4
 *
 * 【计算公式】
 * threshold = 2 * fls(num_online_cpus) * (1 + fls(mem))
 * 其中 mem = zone_managed_pages >> (27 - PAGE_SHIFT)
 *       （内存大小，以 128MB 为单位）
 *
 * 【阈值示例】（见下表）
 * 展示了不同 CPU 数量和内存大小下的阈值：
 * - 少量内存 + 少量 CPU：小阈值（如 4-10）
 * - 大量内存 + 大量 CPU：大阈值（如 108-125）
 * - 最大阈值限制为 125
 *
 * 【为什么需要动态阈值】
 * 1. 小系统：低阈值保证统计准确性
 * 2. 大系统：高阈值减少全局计数器竞争，提高性能
 * 3. 对数缩放：避免阈值增长过快
 */
int calculate_normal_threshold(struct zone *zone)
{
	int threshold;
	int mem;	/* memory in 128 MB units */
	            /* 内存大小，以 128MB 为单位 */

	/*
	 * The threshold scales with the number of processors and the amount
	 * of memory per zone. More memory means that we can defer updates for
	 * longer, more processors could lead to more contention.
 	 * fls() is used to have a cheap way of logarithmic scaling.
	 *
	 * Some sample thresholds:
	 *
	 * Threshold	Processors	(fls)	Zonesize	fls(mem)+1
	 * ------------------------------------------------------------------
	 * 8		1		1	0.9-1 GB	4
	 * 16		2		2	0.9-1 GB	4
	 * 20 		2		2	1-2 GB		5
	 * 24		2		2	2-4 GB		6
	 * 28		2		2	4-8 GB		7
	 * 32		2		2	8-16 GB		8
	 * 4		2		2	<128M		1
	 * 30		4		3	2-4 GB		5
	 * 48		4		3	8-16 GB		8
	 * 32		8		4	1-2 GB		4
	 * 32		8		4	0.9-1GB		4
	 * 10		16		5	<128M		1
	 * 40		16		5	900M		4
	 * 70		64		7	2-4 GB		5
	 * 84		64		7	4-8 GB		6
	 * 108		512		9	4-8 GB		6
	 * 125		1024		10	8-16 GB		8
	 * 125		1024		10	16-32 GB	9
	 */
	/*
	 * 阈值根据处理器数量和每个 zone 的内存量进行缩放。
	 * 更多内存意味着我们可以延迟更新更长时间，
	 * 更多处理器可能导致更多竞争。
	 * fls() 用于廉价的对数缩放。
	 *
	 * 阈值示例表格（见上）：
	 * 列说明：
	 * - Threshold: 计算出的阈值
	 * - Processors: CPU 数量
	 * - (fls): fls(num_online_cpus) 的值
	 * - Zonesize: zone 的内存大小
	 * - fls(mem)+1: fls(mem)+1 的值
	 */

	mem = zone_managed_pages(zone) >> (27 - PAGE_SHIFT);
	/* 将 zone 管理的页面数转换为 128MB 单位
	 * 27 = log2(128MB) = log2(128*1024*1024) = log2(2^27)
	 * >> (27 - PAGE_SHIFT): 右移得到以 128MB 为单位的内存大小
	 *
	 * 例如（PAGE_SHIFT = 12，即 4KB 页面）：
	 * - 1GB = 262144 页
	 * - >> (27-12) = >> 15 = 8（即 8 个 128MB 单位）
	 */

	threshold = 2 * fls(num_online_cpus()) * (1 + fls(mem));
	/* 阈值计算公式：
	 * 2 * fls(CPU数) * (1 + fls(内存/128MB))
	 *
	 * 示例计算：
	 * - 2 个 CPU, 1GB 内存:
	 *   fls(2)=2, mem=8, fls(8)=4
	 *   threshold = 2 * 2 * (1+4) = 20
	 * - 64 个 CPU, 4GB 内存:
	 *   fls(64)=7, mem=32, fls(32)=6
	 *   threshold = 2 * 7 * (1+6) = 98
	 */

	/*
	 * Maximum threshold is 125
	 */
	/*
	 * 最大阈值是 125
	 * 限制阈值上限，避免过大的漂移
	 */
	threshold = min(125, threshold);

	return threshold;
}

/*
 * Refresh the thresholds for each zone.
 */
/*
 * 【函数】refresh_zone_stat_thresholds - 刷新每个 zone 的阈值
 *
 * 【功能】
 * 重新计算并设置所有 zone 和 node 的统计阈值。
 *
 * 【调用时机】
 * - 系统初始化时
 * - CPU 热插拔事件（上线/下线）
 * - 内存热插拔事件
 * - 系统配置变化时
 *
 * 【工作流程】
 * 1. 清零所有 node 的阈值
 * 2. 遍历所有已填充的 zone
 * 3. 为每个 zone 计算正常阈值
 * 4. 设置每个 CPU 的 zone 和 node 阈值
 * 5. 计算并设置 percpu_drift_mark（漂移标记）
 *
 * 【percpu_drift_mark 的含义】
 * 这是一个危险水位线，当空闲页面数低于此值时，
 * 表示 per-CPU 计数器的漂移可能导致最小水位线被突破。
 */
void refresh_zone_stat_thresholds(void)
{
	struct pglist_data *pgdat;
	struct zone *zone;
	int cpu;
	int threshold;

	/* Zero current pgdat thresholds */
	/* 清零当前 pgdat（node）的阈值 */
	for_each_online_pgdat(pgdat) {
		/* 遍历所有在线的 NUMA 节点 */

		for_each_online_cpu(cpu) {
			per_cpu_ptr(pgdat->per_cpu_nodestats, cpu)->stat_threshold = 0;
			/* 将该节点在每个 CPU 上的统计阈值设为 0 */
		}
	}

	for_each_populated_zone(zone) {
		/* 遍历所有已填充的内存区域 */

		struct pglist_data *pgdat = zone->zone_pgdat;
		/* 获取该 zone 所属的 NUMA 节点 */

		unsigned long max_drift, tolerate_drift;

		threshold = calculate_normal_threshold(zone);
		/* 计算该 zone 的正常阈值 */

		for_each_online_cpu(cpu) {
			int pgdat_threshold;

			per_cpu_ptr(zone->per_cpu_zonestats, cpu)->stat_threshold
							= threshold;
			/* 设置该 zone 在每个 CPU 上的统计阈值 */

			/* Base nodestat threshold on the largest populated zone. */
			/* 节点统计阈值基于最大的已填充 zone */
			pgdat_threshold = per_cpu_ptr(pgdat->per_cpu_nodestats, cpu)->stat_threshold;
			/* 获取当前节点阈值 */

			per_cpu_ptr(pgdat->per_cpu_nodestats, cpu)->stat_threshold
				= max(threshold, pgdat_threshold);
			/* 节点阈值取所有 zone 阈值的最大值
			 * 原因：节点级统计是所有 zone 的聚合，
			 * 需要使用最大 zone 的阈值来保证准确性
			 */
		}

		/*
		 * Only set percpu_drift_mark if there is a danger that
		 * NR_FREE_PAGES reports the low watermark is ok when in fact
		 * the min watermark could be breached by an allocation
		 */
		/*
		 * 仅当存在危险时才设置 percpu_drift_mark：
		 * 即 NR_FREE_PAGES 报告低水位线正常，
		 * 但实际上最小水位线可能被分配突破。
		 */
		tolerate_drift = low_wmark_pages(zone) - min_wmark_pages(zone);
		/* 可容忍的漂移量 = 低水位 - 最小水位
		 * 这是"安全缓冲区"的大小
		 */

		max_drift = num_online_cpus() * threshold;
		/* 最大漂移量 = CPU数 × 阈值
		 * 每个 CPU 的 per-CPU 计数器最多可能漂移 threshold 页
		 * 所有 CPU 的总漂移 = CPU数 × threshold
		 */

		if (max_drift > tolerate_drift)
			zone->percpu_drift_mark = high_wmark_pages(zone) +
					max_drift;
		/* 如果最大漂移超过可容忍范围：
		 * 设置漂移标记 = 高水位 + 最大漂移
		 *
		 * 【解释】
		 * 当空闲页面降到 percpu_drift_mark 以下时，
		 * 即使考虑最坏情况的漂移（所有 per-CPU 计数器都未同步），
		 * 实际空闲页面也可能低于最小水位线。
		 * 此时需要强制同步 per-CPU 计数器。
		 *
		 * 【为什么加上 high_wmark】
		 * 在高水位线以上时系统是安全的，
		 * 加上 max_drift 确保即使有最大漂移也不会破坏最小水位线。
		 */
	}
}

/*
 * 【函数】set_pgdat_percpu_threshold - 设置 node 的 per-CPU 阈值
 * @pgdat: NUMA 节点
 * @calculate_pressure: 压力阈值计算函数指针
 *
 * 【功能】
 * 为指定节点的所有 zone 设置基于压力的 per-CPU 阈值。
 *
 * 【使用场景】
 * 当系统内存压力增大时，需要降低阈值以提高统计准确性，
 * 从而更及时地触发内存回收。
 *
 * 【calculate_pressure 函数】
 * 通常是 calculate_pressure_threshold，
 * 计算比正常阈值更小的压力阈值。
 */
void set_pgdat_percpu_threshold(pg_data_t *pgdat,
				int (*calculate_pressure)(struct zone *))
{
	struct zone *zone;
	int cpu;
	int threshold;
	int i;

	for (i = 0; i < pgdat->nr_zones; i++) {
		/* 遍历该节点的所有 zone */

		zone = &pgdat->node_zones[i];
		if (!zone->percpu_drift_mark)
			continue;
		/* 跳过没有设置漂移标记的 zone
		 * （说明该 zone 的漂移不是问题）
		 */

		threshold = (*calculate_pressure)(zone);
		/* 使用压力计算函数计算该 zone 的阈值 */

		for_each_online_cpu(cpu)
			per_cpu_ptr(zone->per_cpu_zonestats, cpu)->stat_threshold
							= threshold;
		/* 为每个在线 CPU 设置该 zone 的阈值 */
	}
}

/*
 * ============================================================================
 * 【Per-CPU 统计更新函数】
 *
 * 这些函数用于更新 per-CPU 统计计数器。
 * 它们实现了差分（differential）机制：
 * - 变化累积在 per-CPU 计数器中
 * - 当累积值超过阈值时，同步到全局计数器
 * ============================================================================
 */

/*
 * For use when we know that interrupts are disabled,
 * or when we know that preemption is disabled and that
 * particular counter cannot be updated from interrupt context.
 */
/*
 * 【函数】__mod_zone_page_state - 修改 zone 页面状态计数器
 * @zone: 内存区域
 * @item: 统计项
 * @delta: 变化量（可正可负）
 *
 * 【使用场景】
 * 当我们知道中断已禁用，或者我们知道抢占已禁用且
 * 特定计数器不能从中断上下文更新时使用。
 *
 * 【差分机制】
 * 1. 将 delta 加到 per-CPU 差分计数器
 * 2. 如果差分的绝对值超过阈值，同步到全局
 * 3. 否则保留在 per-CPU 计数器中
 *
 * 【为什么需要 RMW（Read-Modify-Write）】
 * 准确的 vmstat 更新需要原子的读-改-写操作。
 *
 * 【不同内核的原子性保证】
 * - 非 PREEMPT_RT 内核：通过禁用 IRQ 提供原子性
 *   （显式禁用或通过 local_lock_irq）
 * - PREEMPT_RT 内核：local_lock_irq 只禁用 CPU 迁移，
 *   抢占可能破坏计数器，因此需要禁用抢占。
 */
void __mod_zone_page_state(struct zone *zone, enum zone_stat_item item,
			   long delta)
{
	struct per_cpu_zonestat __percpu *pcp = zone->per_cpu_zonestats;
	/* 获取该 zone 的 per-CPU 统计结构 */

	s8 __percpu *p = pcp->vm_stat_diff + item;
	/* 获取该统计项的差分计数器指针
	 * s8: 8 位有符号整数，范围 -128 到 +127
	 * 足够存储阈值范围内的差分值
	 */

	long x;
	long t;

	/*
	 * Accurate vmstat updates require a RMW. On !PREEMPT_RT kernels,
	 * atomicity is provided by IRQs being disabled -- either explicitly
	 * or via local_lock_irq. On PREEMPT_RT, local_lock_irq only disables
	 * CPU migrations and preemption potentially corrupts a counter so
	 * disable preemption.
	 */
	/*
	 * 准确的 vmstat 更新需要 RMW（读-改-写）。
	 * 在非 PREEMPT_RT 内核上，原子性由禁用 IRQ 提供
	 * （显式禁用或通过 local_lock_irq）。
	 * 在 PREEMPT_RT 上，local_lock_irq 只禁用 CPU 迁移，
	 * 抢占可能破坏计数器，因此需要禁用抢占。
	 */
	preempt_disable_nested();
	/* 禁用抢占（嵌套版本，支持多层禁用）
	 * 确保在操作期间不会被抢占到其他 CPU
	 */

	x = delta + __this_cpu_read(*p);
	/* 读取当前 CPU 的差分计数器并加上新的变化量
	 * __this_cpu_read: 快速访问当前 CPU 的 per-CPU 变量
	 * 不需要禁用抢占（我们已经禁用了）
	 */

	t = __this_cpu_read(pcp->stat_threshold);
	/* 读取当前 CPU 的阈值 */

	if (unlikely(abs(x) > t)) {
		/* 如果差分的绝对值超过阈值 */

		zone_page_state_add(x, zone, item);
		/* 将累积的差分值添加到全局计数器
		 * 这是一个原子操作，更新 zone 的全局统计
		 */

		x = 0;
		/* 重置差分计数器为 0（已同步到全局） */
	}
	__this_cpu_write(*p, x);
	/* 写回差分计数器
	 * 如果未超过阈值，x 保留累积值
	 * 如果已同步，x 为 0
	 */

	preempt_enable_nested();
	/* 恢复抢占 */
}
EXPORT_SYMBOL(__mod_zone_page_state);
/* 导出符号，供内核其他部分使用 */

/*
 * 【函数】__mod_node_page_state - 修改 node 页面状态计数器
 * @pgdat: NUMA 节点
 * @item: 统计项
 * @delta: 变化量
 *
 * 【功能】
 * 类似 __mod_zone_page_state，但操作节点级别的统计。
 *
 * 【特殊处理：字节单位的统计项】
 * 某些统计项以字节为单位（如 cgroup 的子页面统计）。
 * 在全局级别，这些项仍以整页为单位变化。
 * 内部以页面为单位存储，保持 per-CPU 计数器紧凑。
 */
void __mod_node_page_state(struct pglist_data *pgdat, enum node_stat_item item,
				long delta)
{
	struct per_cpu_nodestat __percpu *pcp = pgdat->per_cpu_nodestats;
	/* 获取该节点的 per-CPU 统计结构 */

	s8 __percpu *p = pcp->vm_node_stat_diff + item;
	/* 获取该统计项的差分计数器指针 */

	long x;
	long t;

	if (vmstat_item_in_bytes(item)) {
		/* 如果该统计项以字节为单位 */

		/*
		 * Only cgroups use subpage accounting right now; at
		 * the global level, these items still change in
		 * multiples of whole pages. Store them as pages
		 * internally to keep the per-cpu counters compact.
		 */
		/*
		 * 目前只有 cgroup 使用子页面统计；
		 * 在全局级别，这些项仍以整页的倍数变化。
		 * 内部以页面为单位存储，保持 per-CPU 计数器紧凑。
		 */
		VM_WARN_ON_ONCE(delta & (PAGE_SIZE - 1));
		/* 警告：delta 应该是 PAGE_SIZE 的倍数
		 * (delta & (PAGE_SIZE - 1)) 检查是否有余数
		 */

		delta >>= PAGE_SHIFT;
		/* 转换为页面数：delta / PAGE_SIZE */
	}

	/* See __mod_zone_page_state() */
	/* 参见 __mod_zone_page_state() */
	preempt_disable_nested();

	x = delta + __this_cpu_read(*p);

	t = __this_cpu_read(pcp->stat_threshold);

	if (unlikely(abs(x) > t)) {
		node_page_state_add(x, pgdat, item);
		/* 同步到节点的全局计数器 */
		x = 0;
	}
	__this_cpu_write(*p, x);

	preempt_enable_nested();
}
EXPORT_SYMBOL(__mod_node_page_state);

/*
 * Optimized increment and decrement functions.
 *
 * These are only for a single page and therefore can take a struct page *
 * argument instead of struct zone *. This allows the inclusion of the code
 * generated for page_zone(page) into the optimized functions.
 *
 * No overflow check is necessary and therefore the differential can be
 * incremented or decremented in place which may allow the compilers to
 * generate better code.
 * The increment or decrement is known and therefore one boundary check can
 * be omitted.
 *
 * NOTE: These functions are very performance sensitive. Change only
 * with care.
 *
 * Some processors have inc/dec instructions that are atomic vs an interrupt.
 * However, the code must first determine the differential location in a zone
 * based on the processor number and then inc/dec the counter. There is no
 * guarantee without disabling preemption that the processor will not change
 * in between and therefore the atomicity vs. interrupt cannot be exploited
 * in a useful way here.
 */
/*
 * ============================================================================
 * 【优化的递增/递减函数】
 *
 * 这些函数针对单页操作进行了优化。
 *
 * 【优化点】
 * 1. 只处理单页：可以接受 struct page * 参数而不是 struct zone *
 *    允许编译器内联 page_zone(page) 的代码
 * 2. 无需溢出检查：递增/递减量已知（±1），
 *    可以省略一个边界检查
 * 3. 允许编译器生成更好的代码
 *
 * 【性能敏感】
 * 注意：这些函数对性能非常敏感。只能小心修改。
 *
 * 【为什么不能利用原子 inc/dec 指令】
 * 某些处理器有对中断原子的 inc/dec 指令。
 * 然而，代码必须首先根据处理器编号确定 zone 中的差分位置，
 * 然后再 inc/dec 计数器。
 * 如果不禁用抢占，无法保证处理器在此期间不会改变，
 * 因此无法有效利用对中断的原子性。
 * ============================================================================
 */

/*
 * 【函数】__inc_zone_state - 递增 zone 状态计数器
 * @zone: 内存区域
 * @item: 统计项
 *
 * 【优化】
 * 专门针对递增 1 页的情况优化。
 *
 * 【overstep（过冲）机制】
 * 当差分值刚超过阈值时，不是清零，而是设为负的 overstep。
 * 这样下次更新时可以直接累加，减少同步频率。
 *
 * 【为什么 overstep = t >> 1】
 * 过冲值为阈值的一半，这样：
 * - 实际同步的值 = v + overstep（略多于阈值）
 * - 差分计数器 = -overstep（负值）
 * - 下次更新时从负值开始，需要更多递增才会再次超过阈值
 * 这减少了同步频率，提高性能。
 */
void __inc_zone_state(struct zone *zone, enum zone_stat_item item)
{
	struct per_cpu_zonestat __percpu *pcp = zone->per_cpu_zonestats;
	s8 __percpu *p = pcp->vm_stat_diff + item;
	s8 v, t;
	/* s8: 8 位有符号整数，适合存储差分值和阈值 */

	/* See __mod_zone_page_state() */
	/* 参见 __mod_zone_page_state() */
	preempt_disable_nested();

	v = __this_cpu_inc_return(*p);
	/* 原子递增并返回新值
	 * 相当于 ++(*p) 但针对 per-CPU 变量优化
	 */

	t = __this_cpu_read(pcp->stat_threshold);
	/* 读取阈值 */

	if (unlikely(v > t)) {
		/* 如果新值超过阈值（注意：只检查正向超过） */

		s8 overstep = t >> 1;
		/* 过冲值 = 阈值 / 2 */

		zone_page_state_add(v + overstep, zone, item);
		/* 同步到全局：实际值 + 过冲值
		 * 这样会多同步一些，为下次更新留出空间
		 */

		__this_cpu_write(*p, -overstep);
		/* 差分计数器设为负的过冲值
		 * 下次递增时会从负值开始累加
		 * 需要更多次递增才会再次超过阈值
		 */
	}

	preempt_enable_nested();
}

/*
 * 【函数】__inc_node_state - 递增 node 状态计数器
 * @pgdat: NUMA 节点
 * @item: 统计项
 *
 * 【功能】
 * 类似 __inc_zone_state，但操作节点级别的统计。
 *
 * 【限制】
 * 不支持以字节为单位的统计项（会发出警告）。
 */
void __inc_node_state(struct pglist_data *pgdat, enum node_stat_item item)
{
	struct per_cpu_nodestat __percpu *pcp = pgdat->per_cpu_nodestats;
	s8 __percpu *p = pcp->vm_node_stat_diff + item;
	s8 v, t;

	VM_WARN_ON_ONCE(vmstat_item_in_bytes(item));
	/* 警告：此函数不应用于字节单位的统计项
	 * 因为递增 1 对于字节单位的项没有意义
	 */

	/* See __mod_zone_page_state() */
	/* 参见 __mod_zone_page_state() */
	preempt_disable_nested();

	v = __this_cpu_inc_return(*p);
	t = __this_cpu_read(pcp->stat_threshold);
	if (unlikely(v > t)) {
		s8 overstep = t >> 1;

		node_page_state_add(v + overstep, pgdat, item);
		__this_cpu_write(*p, -overstep);
	}

	preempt_enable_nested();
}

/*
 * 【函数】__inc_zone_page_state - 递增页面所属 zone 的状态计数器
 * @page: 页面
 * @item: 统计项
 *
 * 【功能】
 * 便利函数，根据页面自动确定所属 zone 并递增计数器。
 */
void __inc_zone_page_state(struct page *page, enum zone_stat_item item)
{
	__inc_zone_state(page_zone(page), item);
	/* page_zone(page): 获取页面所属的 zone */
}
EXPORT_SYMBOL(__inc_zone_page_state);

/*
 * 【函数】__inc_node_page_state - 递增页面所属 node 的状态计数器
 * @page: 页面
 * @item: 统计项
 *
 * 【功能】
 * 便利函数，根据页面自动确定所属 node 并递增计数器。
 */
void __inc_node_page_state(struct page *page, enum node_stat_item item)
{
	__inc_node_state(page_pgdat(page), item);
	/* page_pgdat(page): 获取页面所属的 NUMA 节点 */
}
EXPORT_SYMBOL(__inc_node_page_state);

/*
 * 【函数】__dec_zone_state - 递减 zone 状态计数器
 * @zone: 内存区域
 * @item: 统计项
 *
 * 【功能】
 * 专门针对递减 1 页的情况优化。
 *
 * 【负向 overstep】
 * 当差分值变得太负（< -t）时：
 * - 同步到全局：v - overstep（更负的值）
 * - 差分计数器设为正的 overstep
 * - 下次递减时从正值开始，需要更多递减才会再次超过阈值
 */
void __dec_zone_state(struct zone *zone, enum zone_stat_item item)
{
	struct per_cpu_zonestat __percpu *pcp = zone->per_cpu_zonestats;
	s8 __percpu *p = pcp->vm_stat_diff + item;
	s8 v, t;

	/* See __mod_zone_page_state() */
	/* 参见 __mod_zone_page_state() */
	preempt_disable_nested();

	v = __this_cpu_dec_return(*p);
	/* 原子递减并返回新值 */

	t = __this_cpu_read(pcp->stat_threshold);

	if (unlikely(v < - t)) {
		/* 如果新值小于负阈值（太负了） */

		s8 overstep = t >> 1;
		/* 过冲值 = 阈值 / 2 */

		zone_page_state_add(v - overstep, zone, item);
		/* 同步到全局：v - overstep（更负）
		 * 多同步一些负值
		 */

		__this_cpu_write(*p, overstep);
		/* 差分计数器设为正的过冲值
		 * 下次递减时从正值开始
		 */
	}

	preempt_enable_nested();
}

/*
 * 【函数】__dec_node_state - 递减 node 状态计数器
 * @pgdat: NUMA 节点
 * @item: 统计项
 *
 * 【功能】
 * 类似 __dec_zone_state，但操作节点级别的统计。
 */
void __dec_node_state(struct pglist_data *pgdat, enum node_stat_item item)
{
	struct per_cpu_nodestat __percpu *pcp = pgdat->per_cpu_nodestats;
	s8 __percpu *p = pcp->vm_node_stat_diff + item;
	s8 v, t;

	VM_WARN_ON_ONCE(vmstat_item_in_bytes(item));
	/* 不支持字节单位的统计项 */

	/* See __mod_zone_page_state() */
	preempt_disable_nested();

	v = __this_cpu_dec_return(*p);
	t = __this_cpu_read(pcp->stat_threshold);
	if (unlikely(v < - t)) {
		s8 overstep = t >> 1;

		node_page_state_add(v - overstep, pgdat, item);
		__this_cpu_write(*p, overstep);
	}

	preempt_enable_nested();
}

/*
 * 【函数】__dec_zone_page_state - 递减页面所属 zone 的状态计数器
 * @page: 页面
 * @item: 统计项
 */
void __dec_zone_page_state(struct page *page, enum zone_stat_item item)
{
	__dec_zone_state(page_zone(page), item);
}
EXPORT_SYMBOL(__dec_zone_page_state);

/*
 * 【函数】__dec_node_page_state - 递减页面所属 node 的状态计数器
 * @page: 页面
 * @item: 统计项
 */
void __dec_node_page_state(struct page *page, enum node_stat_item item)
{
	__dec_node_state(page_pgdat(page), item);
}
EXPORT_SYMBOL(__dec_node_page_state);

#ifdef CONFIG_HAVE_CMPXCHG_LOCAL
/*
 * ============================================================================
 * 【基于 CMPXCHG 的优化版本】
 *
 * 如果硬件支持 cmpxchg_local，可以避免 local_irq_save/restore 的开销。
 * 使用 this_cpu_try_cmpxchg() 实现无锁的统计更新。
 * ============================================================================
 */

/*
 * If we have cmpxchg_local support then we do not need to incur the overhead
 * that comes with local_irq_save/restore if we use this_cpu_try_cmpxchg().
 *
 * mod_state() modifies the zone counter state through atomic per cpu
 * operations.
 *
 * Overstep mode specifies how overstep should handled:
 *     0       No overstepping
 *     1       Overstepping half of threshold
 *     -1      Overstepping minus half of threshold
*/
/*
 * 如果我们有 cmpxchg_local 支持，
 * 使用 this_cpu_try_cmpxchg() 就不需要承担
 * local_irq_save/restore 的开销。
 *
 * mod_state() 通过原子的 per-CPU 操作修改 zone 计数器状态。
 *
 * Overstep 模式指定如何处理过冲：
 *     0       无过冲
 *     1       过冲阈值的一半
 *     -1      过冲负的阈值一半
 *
 * 【函数】mod_zone_state - 修改 zone 状态（CMPXCHG 版本）
 * @zone: 内存区域
 * @item: 统计项
 * @delta: 变化量
 * @overstep_mode: 过冲模式（0/1/-1）
 *
 * 【CMPXCHG 优势】
 * - 无需禁用中断
 * - 使用硬件原子指令（Compare-And-Swap）
 * - 更低的开销
 *
 * 【工作原理】
 * 1. 读取当前差分值 o
 * 2. 计算新值 n = delta + o
 * 3. 如果超过阈值，计算需要同步到全局的值 z
 * 4. 使用 cmpxchg 原子地更新差分值（如果 o 未变）
 * 5. 如果 cmpxchg 失败（o 已被其他操作改变），重试
 * 6. 如果有溢出（z != 0），同步到全局
 */
static inline void mod_zone_state(struct zone *zone,
       enum zone_stat_item item, long delta, int overstep_mode)
{
	struct per_cpu_zonestat __percpu *pcp = zone->per_cpu_zonestats;
	s8 __percpu *p = pcp->vm_stat_diff + item;
	long n, t, z;
	s8 o;

	o = this_cpu_read(*p);
	/* 读取当前差分值（旧值） */

	do {
		z = 0;  /* overflow to zone counters */
		        /* 溢出到 zone 计数器（默认无溢出） */

		/*
		 * The fetching of the stat_threshold is racy. We may apply
		 * a counter threshold to the wrong the cpu if we get
		 * rescheduled while executing here. However, the next
		 * counter update will apply the threshold again and
		 * therefore bring the counter under the threshold again.
		 *
		 * Most of the time the thresholds are the same anyways
		 * for all cpus in a zone.
		 */
		/*
		 * 获取 stat_threshold 是有竞争的。
		 * 如果我们在这里执行时被重新调度，
		 * 可能会将计数器阈值应用到错误的 CPU。
		 * 然而，下次计数器更新会再次应用阈值，
		 * 从而再次将计数器带回阈值以下。
		 *
		 * 大多数时候，zone 中所有 CPU 的阈值都是相同的。
		 */
		t = this_cpu_read(pcp->stat_threshold);
		/* 读取阈值（可能在重调度后读到不同 CPU 的阈值，但影响不大） */

		n = delta + (long)o;
		/* 计算新的差分值 */

		if (abs(n) > t) {
			/* 如果新值的绝对值超过阈值 */

			int os = overstep_mode * (t >> 1) ;
			/* 计算过冲量：
			 * overstep_mode = 0: os = 0（无过冲）
			 * overstep_mode = 1: os = t/2（正过冲）
			 * overstep_mode = -1: os = -t/2（负过冲）
			 */

			/* Overflow must be added to zone counters */
			/* 溢出必须添加到 zone 计数器 */
			z = n + os;
			/* 要同步到全局的值 = 新值 + 过冲量 */

			n = -os;
			/* 差分计数器设为负的过冲量
			 * 这样下次更新时需要更多变化才会再次超过阈值
			 */
		}
	} while (!this_cpu_try_cmpxchg(*p, &o, n));
	/* 尝试原子地将 *p 从 o 更新为 n
	 * 如果成功（*p 仍然等于 o）：返回 true，循环结束
	 * 如果失败（*p 已被改变）：o 被更新为 *p 的当前值，重试
	 *
	 * 【为什么需要循环】
	 * 在读取 o 和 cmpxchg 之间，其他操作可能已经修改了 *p。
	 * cmpxchg 检测到这种情况并失败，我们需要用新的 o 值重新计算。
	 */

	if (z)
		zone_page_state_add(z, zone, item);
	/* 如果有溢出，同步到全局计数器 */
}

/*
 * 【函数】mod_zone_page_state - 修改 zone 页面状态（CMPXCHG 版本）
 * @zone: 内存区域
 * @item: 统计项
 * @delta: 变化量
 *
 * 【功能】
 * 无过冲模式（overstep_mode = 0）。
 */
void mod_zone_page_state(struct zone *zone, enum zone_stat_item item,
			 long delta)
{
	mod_zone_state(zone, item, delta, 0);
	/* 调用底层函数，overstep_mode = 0（无过冲） */
}
EXPORT_SYMBOL(mod_zone_page_state);

/*
 * 【函数】inc_zone_page_state - 递增 zone 页面状态（CMPXCHG 版本）
 */
void inc_zone_page_state(struct page *page, enum zone_stat_item item)
{
	mod_zone_state(page_zone(page), item, 1, 1);
	/* delta = 1, overstep_mode = 1（正过冲） */
}
EXPORT_SYMBOL(inc_zone_page_state);

/*
 * 【函数】dec_zone_page_state - 递减 zone 页面状态（CMPXCHG 版本）
 */
void dec_zone_page_state(struct page *page, enum zone_stat_item item)
{
	mod_zone_state(page_zone(page), item, -1, -1);
	/* delta = -1, overstep_mode = -1（负过冲） */
}
EXPORT_SYMBOL(dec_zone_page_state);

/*
 * 【函数】mod_node_state - 修改 node 状态（CMPXCHG 版本）
 * @pgdat: NUMA 节点
 * @item: 统计项
 * @delta: 变化量
 * @overstep_mode: 过冲模式
 */
static inline void mod_node_state(struct pglist_data *pgdat,
       enum node_stat_item item, int delta, int overstep_mode)
{
	struct per_cpu_nodestat __percpu *pcp = pgdat->per_cpu_nodestats;
	s8 __percpu *p = pcp->vm_node_stat_diff + item;
	long n, t, z;
	s8 o;

	if (vmstat_item_in_bytes(item)) {
		/* 如果是字节单位的统计项 */

		/*
		 * Only cgroups use subpage accounting right now; at
		 * the global level, these items still change in
		 * multiples of whole pages. Store them as pages
		 * internally to keep the per-cpu counters compact.
		 */
		/*
		 * 目前只有 cgroup 使用子页面统计；
		 * 在全局级别，这些项仍以整页的倍数变化。
		 * 内部以页面为单位存储，保持 per-CPU 计数器紧凑。
		 */
		VM_WARN_ON_ONCE(delta & (PAGE_SIZE - 1));
		delta >>= PAGE_SHIFT;
	}

	o = this_cpu_read(*p);
	do {
		z = 0;  /* overflow to node counters */
		        /* 溢出到 node 计数器 */

		/*
		 * The fetching of the stat_threshold is racy. We may apply
		 * a counter threshold to the wrong the cpu if we get
		 * rescheduled while executing here. However, the next
		 * counter update will apply the threshold again and
		 * therefore bring the counter under the threshold again.
		 *
		 * Most of the time the thresholds are the same anyways
		 * for all cpus in a node.
		 */
		/*
		 * 获取 stat_threshold 是有竞争的。
		 * 如果在此期间被重新调度，可能应用到错误的 CPU。
		 * 但下次更新会再次应用阈值，将计数器带回阈值以下。
		 *
		 * 大多数时候，节点中所有 CPU 的阈值都是相同的。
		 */
		t = this_cpu_read(pcp->stat_threshold);

		n = delta + (long)o;

		if (abs(n) > t) {
			int os = overstep_mode * (t >> 1) ;

			/* Overflow must be added to node counters */
			/* 溢出必须添加到 node 计数器 */
			z = n + os;
			n = -os;
		}
	} while (!this_cpu_try_cmpxchg(*p, &o, n));

	if (z)
		node_page_state_add(z, pgdat, item);
}

/*
 * 【函数】mod_node_page_state - 修改 node 页面状态（CMPXCHG 版本）
 */
void mod_node_page_state(struct pglist_data *pgdat, enum node_stat_item item,
					long delta)
{
	mod_node_state(pgdat, item, delta, 0);
}
EXPORT_SYMBOL(mod_node_page_state);

/*
 * 【函数】inc_node_page_state - 递增 node 页面状态（CMPXCHG 版本）
 */
void inc_node_page_state(struct page *page, enum node_stat_item item)
{
	mod_node_state(page_pgdat(page), item, 1, 1);
}
EXPORT_SYMBOL(inc_node_page_state);

/*
 * 【函数】dec_node_page_state - 递减 node 页面状态（CMPXCHG 版本）
 */
void dec_node_page_state(struct page *page, enum node_stat_item item)
{
	mod_node_state(page_pgdat(page), item, -1, -1);
}
EXPORT_SYMBOL(dec_node_page_state);
#else
/*
 * ============================================================================
 * 【基于中断禁用的版本】
 *
 * 如果没有 CMPXCHG 支持，使用中断禁用来序列化计数器更新。
 * ============================================================================
 */

/*
 * Use interrupt disable to serialize counter updates
 */
/*
 * 使用禁用中断来序列化计数器更新
 */

/*
 * 【函数】mod_zone_page_state - 修改 zone 页面状态（中断禁用版本）
 */
void mod_zone_page_state(struct zone *zone, enum zone_stat_item item,
			 long delta)
{
	unsigned long flags;

	local_irq_save(flags);
	/* 保存中断标志并禁用中断 */

	__mod_zone_page_state(zone, item, delta);
	/* 调用底层函数（假设中断已禁用） */

	local_irq_restore(flags);
	/* 恢复中断标志 */
}
EXPORT_SYMBOL(mod_zone_page_state);

/*
 * 【函数】inc_zone_page_state - 递增 zone 页面状态（中断禁用版本）
 */
void inc_zone_page_state(struct page *page, enum zone_stat_item item)
{
	unsigned long flags;
	struct zone *zone;

	zone = page_zone(page);
	local_irq_save(flags);
	__inc_zone_state(zone, item);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(inc_zone_page_state);

/*
 * 【函数】dec_zone_page_state - 递减 zone 页面状态（中断禁用版本）
 */
void dec_zone_page_state(struct page *page, enum zone_stat_item item)
{
	unsigned long flags;

	local_irq_save(flags);
	__dec_zone_page_state(page, item);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(dec_zone_page_state);

/*
 * 【函数】mod_node_page_state - 修改 node 页面状态（中断禁用版本）
 */
void mod_node_page_state(struct pglist_data *pgdat, enum node_stat_item item,
					long delta)
{
	unsigned long flags;

	local_irq_save(flags);
	__mod_node_page_state(pgdat, item, delta);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(mod_node_page_state);

/*
 * 【函数】inc_node_page_state - 递增 node 页面状态（中断禁用版本）
 */
void inc_node_page_state(struct page *page, enum node_stat_item item)
{
	unsigned long flags;
	struct pglist_data *pgdat;

	pgdat = page_pgdat(page);
	local_irq_save(flags);
	__inc_node_state(pgdat, item);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(inc_node_page_state);

/*
 * 【函数】dec_node_page_state - 递减 node 页面状态（中断禁用版本）
 */
void dec_node_page_state(struct page *page, enum node_stat_item item)
{
	unsigned long flags;

	local_irq_save(flags);
	__dec_node_page_state(page, item);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(dec_node_page_state);
#endif

/*
 * ============================================================================
 * 【差分折叠（Differential Folding）】
 *
 * 将 per-CPU 差分计数器同步到全局计数器。
 * ============================================================================
 */

/*
 * Fold a differential into the global counters.
 * Returns whether counters were updated.
 */
/*
 * 【函数】fold_diff - 将差分折叠到全局计数器
 * @zone_diff: zone 差分数组
 * @node_diff: node 差分数组
 * @return: 计数器是否被更新（bool）
 *
 * 【功能】
 * 将 per-CPU 差分值累加到全局计数器。
 *
 * 【返回值】
 * - true: 至少有一个计数器被更新
 * - false: 所有差分都为 0，没有更新
 */
static int fold_diff(int *zone_diff, int *node_diff)
{
	int i;
	bool changed = false;

	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++) {
		if (zone_diff[i]) {
			atomic_long_add(zone_diff[i], &vm_zone_stat[i]);
			/* 原子地添加到全局 zone 统计 */
			changed = true;
		}
	}

	for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++) {
		if (node_diff[i]) {
			atomic_long_add(node_diff[i], &vm_node_stat[i]);
			/* 原子地添加到全局 node 统计 */
			changed = true;
		}
	}

	return changed;
}

/*
 * Update the zone counters for the current cpu.
 *
 * Note that refresh_cpu_vm_stats strives to only access
 * node local memory. The per cpu pagesets on remote zones are placed
 * in the memory local to the processor using that pageset. So the
 * loop over all zones will access a series of cachelines local to
 * the processor.
 *
 * The call to zone_page_state_add updates the cachelines with the
 * statistics in the remote zone struct as well as the global cachelines
 * with the global counters. These could cause remote node cache line
 * bouncing and will have to be only done when necessary.
 *
 * The function returns whether global counters were updated.
 */
/*
 * 【函数】refresh_cpu_vm_stats - 刷新当前 CPU 的 VM 统计
 * @do_pagesets: 是否处理 pageset 操作
 * @return: 全局计数器是否被更新
 *
 * 【功能】
 * 更新当前 CPU 的 zone 计数器。
 *
 * 【内存访问优化】
 * refresh_cpu_vm_stats 努力只访问节点本地内存。
 * 远程 zone 上的 per-CPU pageset 被放置在使用该 pageset 的处理器本地内存中。
 * 因此对所有 zone 的循环将访问处理器本地的一系列缓存行。
 *
 * 【缓存行抖动】
 * 对 zone_page_state_add 的调用会更新：
 * - 远程 zone 结构中的统计缓存行
 * - 全局计数器的全局缓存行
 * 这些可能导致远程节点缓存行抖动（cache line bouncing），
 * 因此必须仅在必要时执行。
 *
 * 【工作流程】
 * 1. 遍历所有已填充的 zone
 * 2. 使用 xchg 原子地读取并清零差分计数器
 * 3. 将差分添加到 zone 和全局计数器
 * 4. 如果 do_pagesets 为 true，处理 PCP（Per-CPU Pageset）：
 *    - 衰减 PCP 高水位
 *    - 处理远程 pageset 排空
 * 5. 遍历所有在线节点并折叠 node 统计
 * 6. 折叠全局差分
 */
static bool refresh_cpu_vm_stats(bool do_pagesets)
{
	struct pglist_data *pgdat;
	struct zone *zone;
	int i;
	int global_zone_diff[NR_VM_ZONE_STAT_ITEMS] = { 0, };
	/* 全局 zone 差分累加数组 */

	int global_node_diff[NR_VM_NODE_STAT_ITEMS] = { 0, };
	/* 全局 node 差分累加数组 */

	bool changed = false;

	for_each_populated_zone(zone) {
		/* 遍历所有已填充的 zone */

		struct per_cpu_zonestat __percpu *pzstats = zone->per_cpu_zonestats;
		struct per_cpu_pages __percpu *pcp = zone->per_cpu_pageset;

		for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++) {
			int v;

			v = this_cpu_xchg(pzstats->vm_stat_diff[i], 0);
			/* 原子交换：读取差分值并清零
			 * xchg: 原子地交换值（设为 0）并返回旧值
			 */

			if (v) {
				/* 如果差分不为 0 */

				atomic_long_add(v, &zone->vm_stat[i]);
				/* 添加到 zone 的统计计数器 */

				global_zone_diff[i] += v;
				/* 累加到全局差分数组（稍后一次性更新全局计数器） */

#ifdef CONFIG_NUMA
				/* 3 seconds idle till flush */
				/* 3 秒空闲后刷新 */
				__this_cpu_write(pcp->expire, 3);
				/* 重置过期计数器为 3
				 * 用于远程 pageset 的排空机制
				 */
#endif
			}
		}

		if (do_pagesets) {
			/* 如果需要处理 pageset */

			cond_resched();
			/* 条件性重新调度：如果需要调度则让出 CPU
			 * 避免长时间持有 CPU
			 */

			if (decay_pcp_high(zone, this_cpu_ptr(pcp)))
				changed = true;
			/* 衰减 PCP 高水位
			 * 如果高水位被调整，标记为有变化
			 *
			 * 【PCP 高水位衰减】
			 * PCP（Per-CPU Pageset）有一个动态的高水位，
			 * 定期衰减以释放不再需要的缓存页面。
			 */

#ifdef CONFIG_NUMA
			/*
			 * Deal with draining the remote pageset of this
			 * processor
			 *
			 * Check if there are pages remaining in this pageset
			 * if not then there is nothing to expire.
			 */
			/*
			 * 处理排空此处理器的远程 pageset
			 *
			 * 检查此 pageset 中是否还有剩余页面，
			 * 如果没有，则没有需要过期的内容。
			 */
			if (!__this_cpu_read(pcp->expire) ||
			       !__this_cpu_read(pcp->count))
				continue;
			/* 如果过期计数器为 0 或 pageset 为空，跳过
			 * expire: 过期倒计时（每次刷新递减）
			 * count: pageset 中的页面数
			 */

			/*
			 * We never drain zones local to this processor.
			 */
			/*
			 * 我们永不排空本处理器本地的 zone。
			 */
			if (zone_to_nid(zone) == numa_node_id()) {
				/* 如果 zone 属于本地 NUMA 节点 */

				__this_cpu_write(pcp->expire, 0);
				/* 重置过期计数器
				 * 本地 zone 不需要排空机制
				 */
				continue;
			}

			if (__this_cpu_dec_return(pcp->expire)) {
				/* 递减过期计数器并返回新值
				 * 如果返回值非零（还未到期）
				 */
				changed = true;
				continue;
				/* 继续下一个 zone */
			}

			if (__this_cpu_read(pcp->count)) {
				/* 如果 pageset 中有页面 */

				drain_zone_pages(zone, this_cpu_ptr(pcp));
				/* 排空该 zone 的页面
				 * 将 PCP 中的页面返回给伙伴系统
				 *
				 * 【为什么排空远程 pageset】
				 * 远程 zone 的页面缓存在本地 CPU 的 pageset 中，
				 * 长时间不使用会造成内存浪费。
				 * 定期排空可以将页面返回给伙伴系统，
				 * 供其他 CPU 或分配使用。
				 */

				changed = true;
			}
#endif
		}
	}

	for_each_online_pgdat(pgdat) {
		/* 遍历所有在线的 NUMA 节点 */

		struct per_cpu_nodestat __percpu *p = pgdat->per_cpu_nodestats;

		for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++) {
			int v;

			v = this_cpu_xchg(p->vm_node_stat_diff[i], 0);
			/* 原子交换：读取并清零 node 差分计数器 */

			if (v) {
				atomic_long_add(v, &pgdat->vm_stat[i]);
				/* 添加到节点的统计计数器 */

				global_node_diff[i] += v;
				/* 累加到全局差分数组 */
			}
		}
	}

	if (fold_diff(global_zone_diff, global_node_diff))
		changed = true;
	/* 将累积的全局差分折叠到全局计数器
	 * 如果有变化，标记 changed
	 */

	return changed;
	/* 返回是否有计数器被更新 */
}

/*
 * Fold the data for an offline cpu into the global array.
 * There cannot be any access by the offline cpu and therefore
 * synchronization is simplified.
 */
/*
 * 【函数】cpu_vm_stats_fold - 折叠下线 CPU 的数据到全局数组
 * @cpu: 下线的 CPU 编号
 *
 * 【功能】
 * 将下线 CPU 的统计数据折叠到全局数组。
 *
 * 【同步简化】
 * 下线的 CPU 不可能访问这些数据，因此同步被简化。
 * 不需要原子操作或锁来读取下线 CPU 的 per-CPU 数据。
 *
 * 【调用时机】
 * CPU 热插拔：CPU 下线前调用此函数保存其统计数据。
 *
 * 【工作流程】
 * 1. 遍历所有 zone，读取该 CPU 的差分计数器并清零
 * 2. 将差分添加到 zone 和全局计数器
 * 3. 处理 NUMA 事件计数器
 * 4. 遍历所有 node，处理 node 统计
 * 5. 折叠全局差分
 */
void cpu_vm_stats_fold(int cpu)
{
	struct pglist_data *pgdat;
	struct zone *zone;
	int i;
	int global_zone_diff[NR_VM_ZONE_STAT_ITEMS] = { 0, };
	int global_node_diff[NR_VM_NODE_STAT_ITEMS] = { 0, };

	for_each_populated_zone(zone) {
		struct per_cpu_zonestat *pzstats;

		pzstats = per_cpu_ptr(zone->per_cpu_zonestats, cpu);
		/* 获取下线 CPU 在该 zone 的统计数据
		 * 因为 CPU 已下线，可以安全地直接访问
		 */

		for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++) {
			if (pzstats->vm_stat_diff[i]) {
				int v;

				v = pzstats->vm_stat_diff[i];
				/* 读取差分值（不需要原子操作） */

				pzstats->vm_stat_diff[i] = 0;
				/* 清零 */

				atomic_long_add(v, &zone->vm_stat[i]);
				/* 添加到 zone 统计（需要原子操作，因为其他 CPU 可能访问） */

				global_zone_diff[i] += v;
			}
		}
#ifdef CONFIG_NUMA
		for (i = 0; i < NR_VM_NUMA_EVENT_ITEMS; i++) {
			if (pzstats->vm_numa_event[i]) {
				unsigned long v;

				v = pzstats->vm_numa_event[i];
				pzstats->vm_numa_event[i] = 0;
				zone_numa_event_add(v, zone, i);
				/* 添加 NUMA 事件到 zone */
			}
		}
#endif
	}

	for_each_online_pgdat(pgdat) {
		/* 遍历所有在线节点 */

		struct per_cpu_nodestat *p;

		p = per_cpu_ptr(pgdat->per_cpu_nodestats, cpu);
		/* 获取下线 CPU 在该节点的统计数据 */

		for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++)
			if (p->vm_node_stat_diff[i]) {
				int v;

				v = p->vm_node_stat_diff[i];
				p->vm_node_stat_diff[i] = 0;
				atomic_long_add(v, &pgdat->vm_stat[i]);
				global_node_diff[i] += v;
			}
	}

	fold_diff(global_zone_diff, global_node_diff);
	/* 折叠全局差分 */
}

/*
 * this is only called if !populated_zone(zone), which implies no other users of
 * pset->vm_stat_diff[] exist.
 */
/*
 * 【函数】drain_zonestat - 排空 zone 统计
 * @zone: 内存区域
 * @pzstats: per-CPU zone 统计结构
 *
 * 【调用条件】
 * 仅当 !populated_zone(zone) 时调用，
 * 这意味着 pset->vm_stat_diff[] 不存在其他用户。
 *
 * 【使用场景】
 * zone 即将变为非填充状态（如内存热插拔移除），
 * 需要将其统计数据完全排空到全局。
 *
 * 【安全性】
 * 因为没有其他用户，不需要同步机制。
 */
void drain_zonestat(struct zone *zone, struct per_cpu_zonestat *pzstats)
{
	unsigned long v;
	int i;

	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++) {
		if (pzstats->vm_stat_diff[i]) {
			v = pzstats->vm_stat_diff[i];
			pzstats->vm_stat_diff[i] = 0;
			zone_page_state_add(v, zone, i);
		}
	}

#ifdef CONFIG_NUMA
	for (i = 0; i < NR_VM_NUMA_EVENT_ITEMS; i++) {
		if (pzstats->vm_numa_event[i]) {
			v = pzstats->vm_numa_event[i];
			pzstats->vm_numa_event[i] = 0;
			zone_numa_event_add(v, zone, i);
		}
	}
#endif
}
#endif

#ifdef CONFIG_NUMA
/*
 * Determine the per node value of a stat item. This function
 * is called frequently in a NUMA machine, so try to be as
 * frugal as possible.
 */
unsigned long sum_zone_node_page_state(int node,
				 enum zone_stat_item item)
{
	struct zone *zones = NODE_DATA(node)->node_zones;
	int i;
	unsigned long count = 0;

	for (i = 0; i < MAX_NR_ZONES; i++)
		count += zone_page_state(zones + i, item);

	return count;
}

/* Determine the per node value of a numa stat item. */
unsigned long sum_zone_numa_event_state(int node,
				 enum numa_stat_item item)
{
	struct zone *zones = NODE_DATA(node)->node_zones;
	unsigned long count = 0;
	int i;

	for (i = 0; i < MAX_NR_ZONES; i++)
		count += zone_numa_event_state(zones + i, item);

	return count;
}

/*
 * Determine the per node value of a stat item.
 */
unsigned long node_page_state_pages(struct pglist_data *pgdat,
				    enum node_stat_item item)
{
	long x = atomic_long_read(&pgdat->vm_stat[item]);
#ifdef CONFIG_SMP
	if (x < 0)
		x = 0;
#endif
	return x;
}

unsigned long node_page_state(struct pglist_data *pgdat,
			      enum node_stat_item item)
{
	VM_WARN_ON_ONCE(vmstat_item_in_bytes(item));

	return node_page_state_pages(pgdat, item);
}
#endif

/*
 * Count number of pages "struct page" and "struct page_ext" consume.
 * nr_memmap_boot_pages: # of pages allocated by boot allocator
 * nr_memmap_pages: # of pages that were allocated by buddy allocator
 */
static atomic_long_t nr_memmap_boot_pages = ATOMIC_LONG_INIT(0);
static atomic_long_t nr_memmap_pages = ATOMIC_LONG_INIT(0);

void memmap_boot_pages_add(long delta)
{
	atomic_long_add(delta, &nr_memmap_boot_pages);
}

void memmap_pages_add(long delta)
{
	atomic_long_add(delta, &nr_memmap_pages);
}

#ifdef CONFIG_COMPACTION

struct contig_page_info {
	unsigned long free_pages;
	unsigned long free_blocks_total;
	unsigned long free_blocks_suitable;
};

/*
 * Calculate the number of free pages in a zone, how many contiguous
 * pages are free and how many are large enough to satisfy an allocation of
 * the target size. Note that this function makes no attempt to estimate
 * how many suitable free blocks there *might* be if MOVABLE pages were
 * migrated. Calculating that is possible, but expensive and can be
 * figured out from userspace
 */
static void fill_contig_page_info(struct zone *zone,
				unsigned int suitable_order,
				struct contig_page_info *info)
{
	unsigned int order;

	info->free_pages = 0;
	info->free_blocks_total = 0;
	info->free_blocks_suitable = 0;

	for (order = 0; order < NR_PAGE_ORDERS; order++) {
		unsigned long blocks;

		/*
		 * Count number of free blocks.
		 *
		 * Access to nr_free is lockless as nr_free is used only for
		 * diagnostic purposes. Use data_race to avoid KCSAN warning.
		 */
		blocks = data_race(zone->free_area[order].nr_free);
		info->free_blocks_total += blocks;

		/* Count free base pages */
		info->free_pages += blocks << order;

		/* Count the suitable free blocks */
		if (order >= suitable_order)
			info->free_blocks_suitable += blocks <<
						(order - suitable_order);
	}
}

/*
 * A fragmentation index only makes sense if an allocation of a requested
 * size would fail. If that is true, the fragmentation index indicates
 * whether external fragmentation or a lack of memory was the problem.
 * The value can be used to determine if page reclaim or compaction
 * should be used
 */
static int __fragmentation_index(unsigned int order, struct contig_page_info *info)
{
	unsigned long requested = 1UL << order;

	if (WARN_ON_ONCE(order > MAX_PAGE_ORDER))
		return 0;

	if (!info->free_blocks_total)
		return 0;

	/* Fragmentation index only makes sense when a request would fail */
	if (info->free_blocks_suitable)
		return -1000;

	/*
	 * Index is between 0 and 1 so return within 3 decimal places
	 *
	 * 0 => allocation would fail due to lack of memory
	 * 1 => allocation would fail due to fragmentation
	 */
	return 1000 - div_u64( (1000+(div_u64(info->free_pages * 1000ULL, requested))), info->free_blocks_total);
}

/*
 * Calculates external fragmentation within a zone wrt the given order.
 * It is defined as the percentage of pages found in blocks of size
 * less than 1 << order. It returns values in range [0, 100].
 */
unsigned int extfrag_for_order(struct zone *zone, unsigned int order)
{
	struct contig_page_info info;

	fill_contig_page_info(zone, order, &info);
	if (info.free_pages == 0)
		return 0;

	return div_u64((info.free_pages -
			(info.free_blocks_suitable << order)) * 100,
			info.free_pages);
}

/* Same as __fragmentation index but allocs contig_page_info on stack */
int fragmentation_index(struct zone *zone, unsigned int order)
{
	struct contig_page_info info;

	fill_contig_page_info(zone, order, &info);
	return __fragmentation_index(order, &info);
}
#endif

#if defined(CONFIG_PROC_FS) || defined(CONFIG_SYSFS) || \
    defined(CONFIG_NUMA) || defined(CONFIG_MEMCG)
#ifdef CONFIG_ZONE_DMA
#define TEXT_FOR_DMA(xx, yy) [xx##_DMA] = yy "_dma",
#else
#define TEXT_FOR_DMA(xx, yy)
#endif

#ifdef CONFIG_ZONE_DMA32
#define TEXT_FOR_DMA32(xx, yy) [xx##_DMA32] = yy "_dma32",
#else
#define TEXT_FOR_DMA32(xx, yy)
#endif

#ifdef CONFIG_HIGHMEM
#define TEXT_FOR_HIGHMEM(xx, yy) [xx##_HIGH] = yy "_high",
#else
#define TEXT_FOR_HIGHMEM(xx, yy)
#endif

#ifdef CONFIG_ZONE_DEVICE
#define TEXT_FOR_DEVICE(xx, yy) [xx##_DEVICE] = yy "_device",
#else
#define TEXT_FOR_DEVICE(xx, yy)
#endif

#define TEXTS_FOR_ZONES(xx, yy)			\
	TEXT_FOR_DMA(xx, yy)			\
	TEXT_FOR_DMA32(xx, yy)			\
	[xx##_NORMAL] = yy "_normal",		\
	TEXT_FOR_HIGHMEM(xx, yy)		\
	[xx##_MOVABLE] = yy "_movable",		\
	TEXT_FOR_DEVICE(xx, yy)

const char * const vmstat_text[] = {
	/* enum zone_stat_item counters */
#define I(x) (x)
	[I(NR_FREE_PAGES)]			= "nr_free_pages",
	[I(NR_FREE_PAGES_BLOCKS)]		= "nr_free_pages_blocks",
	[I(NR_ZONE_INACTIVE_ANON)]		= "nr_zone_inactive_anon",
	[I(NR_ZONE_ACTIVE_ANON)]		= "nr_zone_active_anon",
	[I(NR_ZONE_INACTIVE_FILE)]		= "nr_zone_inactive_file",
	[I(NR_ZONE_ACTIVE_FILE)]		= "nr_zone_active_file",
	[I(NR_ZONE_UNEVICTABLE)]		= "nr_zone_unevictable",
	[I(NR_ZONE_WRITE_PENDING)]		= "nr_zone_write_pending",
	[I(NR_MLOCK)]				= "nr_mlock",
#if IS_ENABLED(CONFIG_ZSMALLOC)
	[I(NR_ZSPAGES)]				= "nr_zspages",
#endif
	[I(NR_FREE_CMA_PAGES)]			= "nr_free_cma",
#ifdef CONFIG_UNACCEPTED_MEMORY
	[I(NR_UNACCEPTED)]			= "nr_unaccepted",
#endif
#undef I

	/* enum numa_stat_item counters */
#define I(x) (NR_VM_ZONE_STAT_ITEMS + x)
#ifdef CONFIG_NUMA
	[I(NUMA_HIT)]				= "numa_hit",
	[I(NUMA_MISS)]				= "numa_miss",
	[I(NUMA_FOREIGN)]			= "numa_foreign",
	[I(NUMA_INTERLEAVE_HIT)]		= "numa_interleave",
	[I(NUMA_LOCAL)]				= "numa_local",
	[I(NUMA_OTHER)]				= "numa_other",
#endif
#undef I

	/* enum node_stat_item counters */
#define I(x) (NR_VM_ZONE_STAT_ITEMS + NR_VM_NUMA_EVENT_ITEMS + x)
	[I(NR_INACTIVE_ANON)]			= "nr_inactive_anon",
	[I(NR_ACTIVE_ANON)]			= "nr_active_anon",
	[I(NR_INACTIVE_FILE)]			= "nr_inactive_file",
	[I(NR_ACTIVE_FILE)]			= "nr_active_file",
	[I(NR_UNEVICTABLE)]			= "nr_unevictable",
	[I(NR_SLAB_RECLAIMABLE_B)]		= "nr_slab_reclaimable",
	[I(NR_SLAB_UNRECLAIMABLE_B)]		= "nr_slab_unreclaimable",
	[I(NR_ISOLATED_ANON)]			= "nr_isolated_anon",
	[I(NR_ISOLATED_FILE)]			= "nr_isolated_file",
	[I(WORKINGSET_NODES)]			= "workingset_nodes",
	[I(WORKINGSET_REFAULT_ANON)]		= "workingset_refault_anon",
	[I(WORKINGSET_REFAULT_FILE)]		= "workingset_refault_file",
	[I(WORKINGSET_ACTIVATE_ANON)]		= "workingset_activate_anon",
	[I(WORKINGSET_ACTIVATE_FILE)]		= "workingset_activate_file",
	[I(WORKINGSET_RESTORE_ANON)]		= "workingset_restore_anon",
	[I(WORKINGSET_RESTORE_FILE)]		= "workingset_restore_file",
	[I(WORKINGSET_NODERECLAIM)]		= "workingset_nodereclaim",
	[I(NR_ANON_MAPPED)]			= "nr_anon_pages",
	[I(NR_FILE_MAPPED)]			= "nr_mapped",
	[I(NR_FILE_PAGES)]			= "nr_file_pages",
	[I(NR_FILE_DIRTY)]			= "nr_dirty",
	[I(NR_WRITEBACK)]			= "nr_writeback",
	[I(NR_SHMEM)]				= "nr_shmem",
	[I(NR_SHMEM_THPS)]			= "nr_shmem_hugepages",
	[I(NR_SHMEM_PMDMAPPED)]			= "nr_shmem_pmdmapped",
	[I(NR_FILE_THPS)]			= "nr_file_hugepages",
	[I(NR_FILE_PMDMAPPED)]			= "nr_file_pmdmapped",
	[I(NR_ANON_THPS)]			= "nr_anon_transparent_hugepages",
	[I(NR_VMSCAN_WRITE)]			= "nr_vmscan_write",
	[I(NR_VMSCAN_IMMEDIATE)]		= "nr_vmscan_immediate_reclaim",
	[I(NR_DIRTIED)]				= "nr_dirtied",
	[I(NR_WRITTEN)]				= "nr_written",
	[I(NR_THROTTLED_WRITTEN)]		= "nr_throttled_written",
	[I(NR_KERNEL_MISC_RECLAIMABLE)]		= "nr_kernel_misc_reclaimable",
	[I(NR_FOLL_PIN_ACQUIRED)]		= "nr_foll_pin_acquired",
	[I(NR_FOLL_PIN_RELEASED)]		= "nr_foll_pin_released",
	[I(NR_VMALLOC)]				= "nr_vmalloc",
	[I(NR_KERNEL_STACK_KB)]			= "nr_kernel_stack",
#if IS_ENABLED(CONFIG_SHADOW_CALL_STACK)
	[I(NR_KERNEL_SCS_KB)]			= "nr_shadow_call_stack",
#endif
	[I(NR_PAGETABLE)]			= "nr_page_table_pages",
	[I(NR_SECONDARY_PAGETABLE)]		= "nr_sec_page_table_pages",
#ifdef CONFIG_IOMMU_SUPPORT
	[I(NR_IOMMU_PAGES)]			= "nr_iommu_pages",
#endif
#ifdef CONFIG_SWAP
	[I(NR_SWAPCACHE)]			= "nr_swapcached",
#endif
#ifdef CONFIG_NUMA_BALANCING
	[I(PGPROMOTE_SUCCESS)]			= "pgpromote_success",
	[I(PGPROMOTE_CANDIDATE)]		= "pgpromote_candidate",
	[I(PGPROMOTE_CANDIDATE_NRL)]		= "pgpromote_candidate_nrl",
#endif
	[I(PGDEMOTE_KSWAPD)]			= "pgdemote_kswapd",
	[I(PGDEMOTE_DIRECT)]			= "pgdemote_direct",
	[I(PGDEMOTE_KHUGEPAGED)]		= "pgdemote_khugepaged",
	[I(PGDEMOTE_PROACTIVE)]			= "pgdemote_proactive",
	[I(PGSTEAL_KSWAPD)]			= "pgsteal_kswapd",
	[I(PGSTEAL_DIRECT)]			= "pgsteal_direct",
	[I(PGSTEAL_KHUGEPAGED)]			= "pgsteal_khugepaged",
	[I(PGSTEAL_PROACTIVE)]			= "pgsteal_proactive",
	[I(PGSTEAL_ANON)]			= "pgsteal_anon",
	[I(PGSTEAL_FILE)]			= "pgsteal_file",
	[I(PGSCAN_KSWAPD)]			= "pgscan_kswapd",
	[I(PGSCAN_DIRECT)]			= "pgscan_direct",
	[I(PGSCAN_KHUGEPAGED)]			= "pgscan_khugepaged",
	[I(PGSCAN_PROACTIVE)]			= "pgscan_proactive",
	[I(PGSCAN_ANON)]			= "pgscan_anon",
	[I(PGSCAN_FILE)]			= "pgscan_file",
	[I(PGREFILL)]				= "pgrefill",
#ifdef CONFIG_HUGETLB_PAGE
	[I(NR_HUGETLB)]				= "nr_hugetlb",
#endif
	[I(NR_BALLOON_PAGES)]			= "nr_balloon_pages",
	[I(NR_KERNEL_FILE_PAGES)]		= "nr_kernel_file_pages",
	[I(NR_GPU_ACTIVE)]			= "nr_gpu_active",
	[I(NR_GPU_RECLAIM)]			= "nr_gpu_reclaim",
#undef I

	/* system-wide enum vm_stat_item counters */
#define I(x) (NR_VM_ZONE_STAT_ITEMS + NR_VM_NUMA_EVENT_ITEMS + \
	     NR_VM_NODE_STAT_ITEMS + x)
	[I(NR_DIRTY_THRESHOLD)]			= "nr_dirty_threshold",
	[I(NR_DIRTY_BG_THRESHOLD)]		= "nr_dirty_background_threshold",
	[I(NR_MEMMAP_PAGES)]			= "nr_memmap_pages",
	[I(NR_MEMMAP_BOOT_PAGES)]		= "nr_memmap_boot_pages",
#undef I

#if defined(CONFIG_VM_EVENT_COUNTERS)
	/* enum vm_event_item counters */
#define I(x) (NR_VM_ZONE_STAT_ITEMS + NR_VM_NUMA_EVENT_ITEMS + \
	     NR_VM_NODE_STAT_ITEMS + NR_VM_STAT_ITEMS + x)

	[I(PGPGIN)]				= "pgpgin",
	[I(PGPGOUT)]				= "pgpgout",
	[I(PSWPIN)]				= "pswpin",
	[I(PSWPOUT)]				= "pswpout",

#define OFF (NR_VM_ZONE_STAT_ITEMS + NR_VM_NUMA_EVENT_ITEMS + \
	     NR_VM_NODE_STAT_ITEMS + NR_VM_STAT_ITEMS)
	TEXTS_FOR_ZONES(OFF+PGALLOC, "pgalloc")
	TEXTS_FOR_ZONES(OFF+ALLOCSTALL, "allocstall")
	TEXTS_FOR_ZONES(OFF+PGSCAN_SKIP, "pgskip")
#undef OFF

	[I(PGFREE)]				= "pgfree",
	[I(PGACTIVATE)]				= "pgactivate",
	[I(PGDEACTIVATE)]			= "pgdeactivate",
	[I(PGLAZYFREE)]				= "pglazyfree",

	[I(PGFAULT)]				= "pgfault",
	[I(PGMAJFAULT)]				= "pgmajfault",
	[I(PGLAZYFREED)]			= "pglazyfreed",

	[I(PGREUSE)]				= "pgreuse",
	[I(PGSCAN_DIRECT_THROTTLE)]		= "pgscan_direct_throttle",

#ifdef CONFIG_NUMA
	[I(PGSCAN_ZONE_RECLAIM_SUCCESS)]	= "zone_reclaim_success",
	[I(PGSCAN_ZONE_RECLAIM_FAILED)]		= "zone_reclaim_failed",
#endif
	[I(PGINODESTEAL)]			= "pginodesteal",
	[I(SLABS_SCANNED)]			= "slabs_scanned",
	[I(KSWAPD_INODESTEAL)]			= "kswapd_inodesteal",
	[I(KSWAPD_LOW_WMARK_HIT_QUICKLY)]	= "kswapd_low_wmark_hit_quickly",
	[I(KSWAPD_HIGH_WMARK_HIT_QUICKLY)]	= "kswapd_high_wmark_hit_quickly",
	[I(PAGEOUTRUN)]				= "pageoutrun",

	[I(PGROTATED)]				= "pgrotated",

	[I(DROP_PAGECACHE)]			= "drop_pagecache",
	[I(DROP_SLAB)]				= "drop_slab",
	[I(OOM_KILL)]				= "oom_kill",

#ifdef CONFIG_NUMA_BALANCING
	[I(NUMA_PTE_UPDATES)]			= "numa_pte_updates",
	[I(NUMA_HUGE_PTE_UPDATES)]		= "numa_huge_pte_updates",
	[I(NUMA_HINT_FAULTS)]			= "numa_hint_faults",
	[I(NUMA_HINT_FAULTS_LOCAL)]		= "numa_hint_faults_local",
	[I(NUMA_PAGE_MIGRATE)]			= "numa_pages_migrated",
#endif
#ifdef CONFIG_MIGRATION
	[I(PGMIGRATE_SUCCESS)]			= "pgmigrate_success",
	[I(PGMIGRATE_FAIL)]			= "pgmigrate_fail",
	[I(THP_MIGRATION_SUCCESS)]		= "thp_migration_success",
	[I(THP_MIGRATION_FAIL)]			= "thp_migration_fail",
	[I(THP_MIGRATION_SPLIT)]		= "thp_migration_split",
#endif
#ifdef CONFIG_COMPACTION
	[I(COMPACTMIGRATE_SCANNED)]		= "compact_migrate_scanned",
	[I(COMPACTFREE_SCANNED)]		= "compact_free_scanned",
	[I(COMPACTISOLATED)]			= "compact_isolated",
	[I(COMPACTSTALL)]			= "compact_stall",
	[I(COMPACTFAIL)]			= "compact_fail",
	[I(COMPACTSUCCESS)]			= "compact_success",
	[I(KCOMPACTD_WAKE)]			= "compact_daemon_wake",
	[I(KCOMPACTD_MIGRATE_SCANNED)]		= "compact_daemon_migrate_scanned",
	[I(KCOMPACTD_FREE_SCANNED)]		= "compact_daemon_free_scanned",
#endif

#ifdef CONFIG_HUGETLB_PAGE
	[I(HTLB_BUDDY_PGALLOC)]			= "htlb_buddy_alloc_success",
	[I(HTLB_BUDDY_PGALLOC_FAIL)]		= "htlb_buddy_alloc_fail",
#endif
#ifdef CONFIG_CMA
	[I(CMA_ALLOC_SUCCESS)]			= "cma_alloc_success",
	[I(CMA_ALLOC_FAIL)]			= "cma_alloc_fail",
#endif
	[I(UNEVICTABLE_PGCULLED)]		= "unevictable_pgs_culled",
	[I(UNEVICTABLE_PGSCANNED)]		= "unevictable_pgs_scanned",
	[I(UNEVICTABLE_PGRESCUED)]		= "unevictable_pgs_rescued",
	[I(UNEVICTABLE_PGMLOCKED)]		= "unevictable_pgs_mlocked",
	[I(UNEVICTABLE_PGMUNLOCKED)]		= "unevictable_pgs_munlocked",
	[I(UNEVICTABLE_PGCLEARED)]		= "unevictable_pgs_cleared",
	[I(UNEVICTABLE_PGSTRANDED)]		= "unevictable_pgs_stranded",

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	[I(THP_FAULT_ALLOC)]			= "thp_fault_alloc",
	[I(THP_FAULT_FALLBACK)]			= "thp_fault_fallback",
	[I(THP_FAULT_FALLBACK_CHARGE)]		= "thp_fault_fallback_charge",
	[I(THP_COLLAPSE_ALLOC)]			= "thp_collapse_alloc",
	[I(THP_COLLAPSE_ALLOC_FAILED)]		= "thp_collapse_alloc_failed",
	[I(THP_FILE_ALLOC)]			= "thp_file_alloc",
	[I(THP_FILE_FALLBACK)]			= "thp_file_fallback",
	[I(THP_FILE_FALLBACK_CHARGE)]		= "thp_file_fallback_charge",
	[I(THP_FILE_MAPPED)]			= "thp_file_mapped",
	[I(THP_SPLIT_PAGE)]			= "thp_split_page",
	[I(THP_SPLIT_PAGE_FAILED)]		= "thp_split_page_failed",
	[I(THP_DEFERRED_SPLIT_PAGE)]		= "thp_deferred_split_page",
	[I(THP_UNDERUSED_SPLIT_PAGE)]		= "thp_underused_split_page",
	[I(THP_SPLIT_PMD)]			= "thp_split_pmd",
	[I(THP_SCAN_EXCEED_NONE_PTE)]		= "thp_scan_exceed_none_pte",
	[I(THP_SCAN_EXCEED_SWAP_PTE)]		= "thp_scan_exceed_swap_pte",
	[I(THP_SCAN_EXCEED_SHARED_PTE)]		= "thp_scan_exceed_share_pte",
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
	[I(THP_SPLIT_PUD)]			= "thp_split_pud",
#endif
	[I(THP_ZERO_PAGE_ALLOC)]		= "thp_zero_page_alloc",
	[I(THP_ZERO_PAGE_ALLOC_FAILED)]		= "thp_zero_page_alloc_failed",
	[I(THP_SWPOUT)]				= "thp_swpout",
	[I(THP_SWPOUT_FALLBACK)]		= "thp_swpout_fallback",
#endif
#ifdef CONFIG_BALLOON
	[I(BALLOON_INFLATE)]			= "balloon_inflate",
	[I(BALLOON_DEFLATE)]			= "balloon_deflate",
#ifdef CONFIG_BALLOON_MIGRATION
	[I(BALLOON_MIGRATE)]			= "balloon_migrate",
#endif /* CONFIG_BALLOON_MIGRATION */
#endif /* CONFIG_BALLOON */
#ifdef CONFIG_DEBUG_TLBFLUSH
	[I(NR_TLB_REMOTE_FLUSH)]		= "nr_tlb_remote_flush",
	[I(NR_TLB_REMOTE_FLUSH_RECEIVED)]	= "nr_tlb_remote_flush_received",
	[I(NR_TLB_LOCAL_FLUSH_ALL)]		= "nr_tlb_local_flush_all",
	[I(NR_TLB_LOCAL_FLUSH_ONE)]		= "nr_tlb_local_flush_one",
#endif /* CONFIG_DEBUG_TLBFLUSH */

#ifdef CONFIG_SWAP
	[I(SWAP_RA)]				= "swap_ra",
	[I(SWAP_RA_HIT)]			= "swap_ra_hit",
	[I(SWPIN_ZERO)]				= "swpin_zero",
	[I(SWPOUT_ZERO)]			= "swpout_zero",
#ifdef CONFIG_KSM
	[I(KSM_SWPIN_COPY)]			= "ksm_swpin_copy",
#endif
#endif
#ifdef CONFIG_KSM
	[I(COW_KSM)]				= "cow_ksm",
#endif
#ifdef CONFIG_ZSWAP
	[I(ZSWPIN)]				= "zswpin",
	[I(ZSWPOUT)]				= "zswpout",
	[I(ZSWPWB)]				= "zswpwb",
#endif
#ifdef CONFIG_X86
	[I(DIRECT_MAP_LEVEL2_SPLIT)]		= "direct_map_level2_splits",
	[I(DIRECT_MAP_LEVEL3_SPLIT)]		= "direct_map_level3_splits",
	[I(DIRECT_MAP_LEVEL2_COLLAPSE)]		= "direct_map_level2_collapses",
	[I(DIRECT_MAP_LEVEL3_COLLAPSE)]		= "direct_map_level3_collapses",
#endif
#ifdef CONFIG_PER_VMA_LOCK_STATS
	[I(VMA_LOCK_SUCCESS)]			= "vma_lock_success",
	[I(VMA_LOCK_ABORT)]			= "vma_lock_abort",
	[I(VMA_LOCK_RETRY)]			= "vma_lock_retry",
	[I(VMA_LOCK_MISS)]			= "vma_lock_miss",
#endif
#ifdef CONFIG_DEBUG_STACK_USAGE
	[I(KSTACK_1K)]				= "kstack_1k",
#if THREAD_SIZE > 1024
	[I(KSTACK_2K)]				= "kstack_2k",
#endif
#if THREAD_SIZE > 2048
	[I(KSTACK_4K)]				= "kstack_4k",
#endif
#if THREAD_SIZE > 4096
	[I(KSTACK_8K)]				= "kstack_8k",
#endif
#if THREAD_SIZE > 8192
	[I(KSTACK_16K)]				= "kstack_16k",
#endif
#if THREAD_SIZE > 16384
	[I(KSTACK_32K)]				= "kstack_32k",
#endif
#if THREAD_SIZE > 32768
	[I(KSTACK_64K)]				= "kstack_64k",
#endif
#if THREAD_SIZE > 65536
	[I(KSTACK_REST)]			= "kstack_rest",
#endif
#endif
#undef I
#endif /* CONFIG_VM_EVENT_COUNTERS */
};
#endif /* CONFIG_PROC_FS || CONFIG_SYSFS || CONFIG_NUMA || CONFIG_MEMCG */

#if (defined(CONFIG_DEBUG_FS) && defined(CONFIG_COMPACTION)) || \
     defined(CONFIG_PROC_FS)
static void *frag_start(struct seq_file *m, loff_t *pos)
{
	pg_data_t *pgdat;
	loff_t node = *pos;

	for (pgdat = first_online_pgdat();
	     pgdat && node;
	     pgdat = next_online_pgdat(pgdat))
		--node;

	return pgdat;
}

static void *frag_next(struct seq_file *m, void *arg, loff_t *pos)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	(*pos)++;
	return next_online_pgdat(pgdat);
}

static void frag_stop(struct seq_file *m, void *arg)
{
}

/*
 * Walk zones in a node and print using a callback.
 * If @assert_populated is true, only use callback for zones that are populated.
 */
static void walk_zones_in_node(struct seq_file *m, pg_data_t *pgdat,
		bool assert_populated, bool nolock,
		void (*print)(struct seq_file *m, pg_data_t *, struct zone *))
{
	struct zone *zone;
	struct zone *node_zones = pgdat->node_zones;
	unsigned long flags;

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		if (assert_populated && !populated_zone(zone))
			continue;

		if (!nolock)
			spin_lock_irqsave(&zone->lock, flags);
		print(m, pgdat, zone);
		if (!nolock)
			spin_unlock_irqrestore(&zone->lock, flags);
	}
}
#endif

#ifdef CONFIG_PROC_FS
static void frag_show_print(struct seq_file *m, pg_data_t *pgdat,
						struct zone *zone)
{
	int order;

	seq_printf(m, "Node %d, zone %8s ", pgdat->node_id, zone->name);
	for (order = 0; order < NR_PAGE_ORDERS; ++order)
		/*
		 * Access to nr_free is lockless as nr_free is used only for
		 * printing purposes. Use data_race to avoid KCSAN warning.
		 */
		seq_printf(m, "%6lu ", data_race(zone->free_area[order].nr_free));
	seq_putc(m, '\n');
}

/*
 * This walks the free areas for each zone.
 */
static int frag_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;
	walk_zones_in_node(m, pgdat, true, false, frag_show_print);
	return 0;
}

static void pagetypeinfo_showfree_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	int order, mtype;

	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++) {
		seq_printf(m, "Node %4d, zone %8s, type %12s ",
					pgdat->node_id,
					zone->name,
					migratetype_names[mtype]);
		for (order = 0; order < NR_PAGE_ORDERS; ++order) {
			unsigned long freecount = 0;
			struct free_area *area;
			struct list_head *curr;
			bool overflow = false;

			area = &(zone->free_area[order]);

			list_for_each(curr, &area->free_list[mtype]) {
				/*
				 * Cap the free_list iteration because it might
				 * be really large and we are under a spinlock
				 * so a long time spent here could trigger a
				 * hard lockup detector. Anyway this is a
				 * debugging tool so knowing there is a handful
				 * of pages of this order should be more than
				 * sufficient.
				 */
				if (++freecount >= 100000) {
					overflow = true;
					break;
				}
			}
			seq_printf(m, "%s%6lu ", overflow ? ">" : "", freecount);
			spin_unlock_irq(&zone->lock);
			cond_resched();
			spin_lock_irq(&zone->lock);
		}
		seq_putc(m, '\n');
	}
}

/* Print out the free pages at each order for each migratetype */
static void pagetypeinfo_showfree(struct seq_file *m, void *arg)
{
	int order;
	pg_data_t *pgdat = (pg_data_t *)arg;

	/* Print header */
	seq_printf(m, "%-43s ", "Free pages count per migrate type at order");
	for (order = 0; order < NR_PAGE_ORDERS; ++order)
		seq_printf(m, "%6d ", order);
	seq_putc(m, '\n');

	walk_zones_in_node(m, pgdat, true, false, pagetypeinfo_showfree_print);
}

static void pagetypeinfo_showblockcount_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	int mtype;
	unsigned long pfn;
	unsigned long start_pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count[MIGRATE_TYPES] = { 0, };

	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		struct page *page;

		page = pfn_to_online_page(pfn);
		if (!page)
			continue;

		if (page_zone(page) != zone)
			continue;

		mtype = get_pageblock_migratetype(page);

		if (mtype < MIGRATE_TYPES)
			count[mtype]++;
	}

	/* Print counts */
	seq_printf(m, "Node %d, zone %8s ", pgdat->node_id, zone->name);
	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++)
		seq_printf(m, "%12lu ", count[mtype]);
	seq_putc(m, '\n');
}

/* Print out the number of pageblocks for each migratetype */
static void pagetypeinfo_showblockcount(struct seq_file *m, void *arg)
{
	int mtype;
	pg_data_t *pgdat = (pg_data_t *)arg;

	seq_printf(m, "\n%-23s", "Number of blocks type ");
	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++)
		seq_printf(m, "%12s ", migratetype_names[mtype]);
	seq_putc(m, '\n');
	walk_zones_in_node(m, pgdat, true, false,
		pagetypeinfo_showblockcount_print);
}

/*
 * Print out the number of pageblocks for each migratetype that contain pages
 * of other types. This gives an indication of how well fallbacks are being
 * contained by rmqueue_fallback(). It requires information from PAGE_OWNER
 * to determine what is going on
 */
static void pagetypeinfo_showmixedcount(struct seq_file *m, pg_data_t *pgdat)
{
#ifdef CONFIG_PAGE_OWNER
	int mtype;

	if (!static_branch_unlikely(&page_owner_inited))
		return;

	drain_all_pages(NULL);

	seq_printf(m, "\n%-23s", "Number of mixed blocks ");
	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++)
		seq_printf(m, "%12s ", migratetype_names[mtype]);
	seq_putc(m, '\n');

	walk_zones_in_node(m, pgdat, true, true,
		pagetypeinfo_showmixedcount_print);
#endif /* CONFIG_PAGE_OWNER */
}

/*
 * This prints out statistics in relation to grouping pages by mobility.
 * It is expensive to collect so do not constantly read the file.
 */
static int pagetypeinfo_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	/* check memoryless node */
	if (!node_state(pgdat->node_id, N_MEMORY))
		return 0;

	seq_printf(m, "Page block order: %d\n", pageblock_order);
	seq_printf(m, "Pages per block:  %lu\n", pageblock_nr_pages);
	seq_putc(m, '\n');
	pagetypeinfo_showfree(m, pgdat);
	pagetypeinfo_showblockcount(m, pgdat);
	pagetypeinfo_showmixedcount(m, pgdat);

	return 0;
}

static const struct seq_operations fragmentation_op = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= frag_show,
};

static const struct seq_operations pagetypeinfo_op = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= pagetypeinfo_show,
};

static bool is_zone_first_populated(pg_data_t *pgdat, struct zone *zone)
{
	int zid;

	for (zid = 0; zid < MAX_NR_ZONES; zid++) {
		struct zone *compare = &pgdat->node_zones[zid];

		if (populated_zone(compare))
			return zone == compare;
	}

	return false;
}

static void zoneinfo_show_print(struct seq_file *m, pg_data_t *pgdat,
							struct zone *zone)
{
	int i;
	seq_printf(m, "Node %d, zone %8s", pgdat->node_id, zone->name);
	if (is_zone_first_populated(pgdat, zone)) {
		seq_printf(m, "\n  per-node stats");
		for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++) {
			unsigned long pages = node_page_state_pages(pgdat, i);

			if (vmstat_item_print_in_thp(i))
				pages /= HPAGE_PMD_NR;
			seq_printf(m, "\n      %-12s %lu", node_stat_name(i),
				   pages);
		}
	}
	seq_printf(m,
		   "\n  pages free     %lu"
		   "\n        boost    %lu"
		   "\n        min      %lu"
		   "\n        low      %lu"
		   "\n        high     %lu"
		   "\n        promo    %lu"
		   "\n        spanned  %lu"
		   "\n        present  %lu"
		   "\n        managed  %lu"
		   "\n        cma      %lu",
		   zone_page_state(zone, NR_FREE_PAGES),
		   zone->watermark_boost,
		   min_wmark_pages(zone),
		   low_wmark_pages(zone),
		   high_wmark_pages(zone),
		   promo_wmark_pages(zone),
		   zone->spanned_pages,
		   zone->present_pages,
		   zone_managed_pages(zone),
		   zone_cma_pages(zone));

	seq_printf(m,
		   "\n        protection: (%ld",
		   zone->lowmem_reserve[0]);
	for (i = 1; i < ARRAY_SIZE(zone->lowmem_reserve); i++)
		seq_printf(m, ", %ld", zone->lowmem_reserve[i]);
	seq_putc(m, ')');

	/* If unpopulated, no other information is useful */
	if (!populated_zone(zone)) {
		seq_putc(m, '\n');
		return;
	}

	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++)
		seq_printf(m, "\n      %-12s %lu", zone_stat_name(i),
			   zone_page_state(zone, i));

#ifdef CONFIG_NUMA
	fold_vm_zone_numa_events(zone);
	for (i = 0; i < NR_VM_NUMA_EVENT_ITEMS; i++)
		seq_printf(m, "\n      %-12s %lu", numa_stat_name(i),
			   zone_numa_event_state(zone, i));
#endif

	seq_printf(m, "\n  pagesets");
	for_each_online_cpu(i) {
		struct per_cpu_pages *pcp;
		struct per_cpu_zonestat __maybe_unused *pzstats;

		pcp = per_cpu_ptr(zone->per_cpu_pageset, i);
		seq_printf(m,
			   "\n    cpu: %i"
			   "\n              count:    %i"
			   "\n              high:     %i"
			   "\n              batch:    %i"
			   "\n              high_min: %i"
			   "\n              high_max: %i",
			   i,
			   pcp->count,
			   pcp->high,
			   pcp->batch,
			   pcp->high_min,
			   pcp->high_max);
#ifdef CONFIG_SMP
		pzstats = per_cpu_ptr(zone->per_cpu_zonestats, i);
		seq_printf(m, "\n  vm stats threshold: %d",
				pzstats->stat_threshold);
#endif
	}
	seq_printf(m,
		   "\n  node_unreclaimable:  %u"
		   "\n  start_pfn:           %lu"
		   "\n  reserved_highatomic: %lu"
		   "\n  free_highatomic:     %lu",
		   kswapd_test_hopeless(pgdat),
		   zone->zone_start_pfn,
		   zone->nr_reserved_highatomic,
		   zone->nr_free_highatomic);
	seq_putc(m, '\n');
}

/*
 * Output information about zones in @pgdat.  All zones are printed regardless
 * of whether they are populated or not: lowmem_reserve_ratio operates on the
 * set of all zones and userspace would not be aware of such zones if they are
 * suppressed here (zoneinfo displays the effect of lowmem_reserve_ratio).
 */
static int zoneinfo_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;
	walk_zones_in_node(m, pgdat, false, false, zoneinfo_show_print);
	return 0;
}

static const struct seq_operations zoneinfo_op = {
	.start	= frag_start, /* iterate over all zones. The same as in
			       * fragmentation. */
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= zoneinfo_show,
};

#define NR_VMSTAT_ITEMS (NR_VM_ZONE_STAT_ITEMS + \
			 NR_VM_NUMA_EVENT_ITEMS + \
			 NR_VM_NODE_STAT_ITEMS + \
			 NR_VM_STAT_ITEMS + \
			 (IS_ENABLED(CONFIG_VM_EVENT_COUNTERS) ? \
			  NR_VM_EVENT_ITEMS : 0))

static void *vmstat_start(struct seq_file *m, loff_t *pos)
{
	unsigned long *v;
	int i;

	if (*pos >= NR_VMSTAT_ITEMS)
		return NULL;

	BUILD_BUG_ON(ARRAY_SIZE(vmstat_text) != NR_VMSTAT_ITEMS);
	fold_vm_numa_events();
	v = kmalloc_array(NR_VMSTAT_ITEMS, sizeof(unsigned long), GFP_KERNEL);
	m->private = v;
	if (!v)
		return ERR_PTR(-ENOMEM);
	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++)
		v[i] = global_zone_page_state(i);
	v += NR_VM_ZONE_STAT_ITEMS;

#ifdef CONFIG_NUMA
	for (i = 0; i < NR_VM_NUMA_EVENT_ITEMS; i++)
		v[i] = global_numa_event_state(i);
	v += NR_VM_NUMA_EVENT_ITEMS;
#endif

	for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++) {
		v[i] = global_node_page_state_pages(i);
		if (vmstat_item_print_in_thp(i))
			v[i] /= HPAGE_PMD_NR;
	}
	v += NR_VM_NODE_STAT_ITEMS;

	global_dirty_limits(v + NR_DIRTY_BG_THRESHOLD,
			    v + NR_DIRTY_THRESHOLD);
	v[NR_MEMMAP_PAGES] = atomic_long_read(&nr_memmap_pages);
	v[NR_MEMMAP_BOOT_PAGES] = atomic_long_read(&nr_memmap_boot_pages);
	v += NR_VM_STAT_ITEMS;

#ifdef CONFIG_VM_EVENT_COUNTERS
	all_vm_events(v);
	v[PGPGIN] /= 2;		/* sectors -> kbytes */
	v[PGPGOUT] /= 2;
#endif
	return (unsigned long *)m->private + *pos;
}

static void *vmstat_next(struct seq_file *m, void *arg, loff_t *pos)
{
	(*pos)++;
	if (*pos >= NR_VMSTAT_ITEMS)
		return NULL;
	return (unsigned long *)m->private + *pos;
}

static int vmstat_show(struct seq_file *m, void *arg)
{
	unsigned long *l = arg;
	unsigned long off = l - (unsigned long *)m->private;

	seq_puts(m, vmstat_text[off]);
	seq_put_decimal_ull(m, " ", *l);
	seq_putc(m, '\n');

	if (off == NR_VMSTAT_ITEMS - 1) {
		/*
		 * We've come to the end - add any deprecated counters to avoid
		 * breaking userspace which might depend on them being present.
		 */
		seq_puts(m, "nr_unstable 0\n");
	}
	return 0;
}

static void vmstat_stop(struct seq_file *m, void *arg)
{
	kfree(m->private);
	m->private = NULL;
}

static const struct seq_operations vmstat_op = {
	.start	= vmstat_start,
	.next	= vmstat_next,
	.stop	= vmstat_stop,
	.show	= vmstat_show,
};
#endif /* CONFIG_PROC_FS */

#ifdef CONFIG_SMP
static DEFINE_PER_CPU(struct delayed_work, vmstat_work);
static int sysctl_stat_interval __read_mostly = HZ;
static int vmstat_late_init_done;

#ifdef CONFIG_PROC_FS
static void refresh_vm_stats(struct work_struct *work)
{
	refresh_cpu_vm_stats(true);
}

static int vmstat_refresh(const struct ctl_table *table, int write,
		   void *buffer, size_t *lenp, loff_t *ppos)
{
	long val;
	int err;
	int i;

	/*
	 * The regular update, every sysctl_stat_interval, may come later
	 * than expected: leaving a significant amount in per_cpu buckets.
	 * This is particularly misleading when checking a quantity of HUGE
	 * pages, immediately after running a test.  /proc/sys/vm/stat_refresh,
	 * which can equally be echo'ed to or cat'ted from (by root),
	 * can be used to update the stats just before reading them.
	 *
	 * Oh, and since global_zone_page_state() etc. are so careful to hide
	 * transiently negative values, report an error here if any of
	 * the stats is negative, so we know to go looking for imbalance.
	 */
	err = schedule_on_each_cpu(refresh_vm_stats);
	if (err)
		return err;
	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++) {
		/*
		 * Skip checking stats known to go negative occasionally.
		 */
		switch (i) {
		case NR_ZONE_WRITE_PENDING:
		case NR_FREE_CMA_PAGES:
			continue;
		}
		val = atomic_long_read(&vm_zone_stat[i]);
		if (val < 0) {
			pr_warn("%s: %s %ld\n",
				__func__, zone_stat_name(i), val);
		}
	}
	for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++) {
		/*
		 * Skip checking stats known to go negative occasionally.
		 */
		switch (i) {
		case NR_WRITEBACK:
			continue;
		}
		val = atomic_long_read(&vm_node_stat[i]);
		if (val < 0) {
			pr_warn("%s: %s %ld\n",
				__func__, node_stat_name(i), val);
		}
	}
	if (write)
		*ppos += *lenp;
	else
		*lenp = 0;
	return 0;
}
#endif /* CONFIG_PROC_FS */

static void vmstat_update(struct work_struct *w)
{
	if (refresh_cpu_vm_stats(true)) {
		/*
		 * Counters were updated so we expect more updates
		 * to occur in the future. Keep on running the
		 * update worker thread.
		 */
		queue_delayed_work_on(smp_processor_id(), mm_percpu_wq,
				this_cpu_ptr(&vmstat_work),
				round_jiffies_relative(sysctl_stat_interval));
	}
}

/*
 * Check if the diffs for a certain cpu indicate that
 * an update is needed.
 */
static bool need_update(int cpu)
{
	pg_data_t *last_pgdat = NULL;
	struct zone *zone;

	for_each_populated_zone(zone) {
		struct per_cpu_zonestat *pzstats = per_cpu_ptr(zone->per_cpu_zonestats, cpu);
		struct per_cpu_nodestat *n;

		/*
		 * The fast way of checking if there are any vmstat diffs.
		 */
		if (memchr_inv(pzstats->vm_stat_diff, 0, sizeof(pzstats->vm_stat_diff)))
			return true;

		if (last_pgdat == zone->zone_pgdat)
			continue;
		last_pgdat = zone->zone_pgdat;
		n = per_cpu_ptr(zone->zone_pgdat->per_cpu_nodestats, cpu);
		if (memchr_inv(n->vm_node_stat_diff, 0, sizeof(n->vm_node_stat_diff)))
			return true;
	}
	return false;
}

/*
 * Switch off vmstat processing and then fold all the remaining differentials
 * until the diffs stay at zero. The function is used by NOHZ and can only be
 * invoked when tick processing is not active.
 */
void quiet_vmstat(void)
{
	if (system_state != SYSTEM_RUNNING)
		return;

	if (!delayed_work_pending(this_cpu_ptr(&vmstat_work)))
		return;

	if (!need_update(smp_processor_id()))
		return;

	/*
	 * Just refresh counters and do not care about the pending delayed
	 * vmstat_update. It doesn't fire that often to matter and canceling
	 * it would be too expensive from this path.
	 * vmstat_shepherd will take care about that for us.
	 */
	refresh_cpu_vm_stats(false);
}

/*
 * Shepherd worker thread that checks the
 * differentials of processors that have their worker
 * threads for vm statistics updates disabled because of
 * inactivity.
 */
static void vmstat_shepherd(struct work_struct *w);

static DECLARE_DEFERRABLE_WORK(shepherd, vmstat_shepherd);

/*
 * vmstat_flush_workqueue() - 排空承载 per-CPU vmstat 更新的专用 workqueue。
 *
 * 入参：无；返回：无直接返回值。flush_workqueue() 可能睡眠，返回时调用前排入
 * mm_percpu_wq 的 work 已完成，但 deferrable shepherd 或返回后新入队工作不因
 * 此永久停止。housekeeping_update() 用它划清旧 unbound affinity 工作与新
 * DOMAIN 掩码传播的边界，避免 vmstat worker 在 pool 重配期间仍依赖旧目标。
 */
void vmstat_flush_workqueue(void)
{
	flush_workqueue(mm_percpu_wq);
}

static void vmstat_shepherd(struct work_struct *w)
{
	int cpu;

	cpus_read_lock();
	/* Check processors whose vmstat worker threads have been disabled */
	for_each_online_cpu(cpu) {
		struct delayed_work *dw = &per_cpu(vmstat_work, cpu);

		/*
		 * In kernel users of vmstat counters either require the precise value and
		 * they are using zone_page_state_snapshot interface or they can live with
		 * an imprecision as the regular flushing can happen at arbitrary time and
		 * cumulative error can grow (see calculate_normal_threshold).
		 *
		 * From that POV the regular flushing can be postponed for CPUs that have
		 * been isolated from the kernel interference without critical
		 * infrastructure ever noticing. Skip regular flushing from vmstat_shepherd
		 * for all isolated CPUs to avoid interference with the isolated workload.
		 */
		scoped_guard(rcu) {
			if (cpu_is_isolated(cpu))
				continue;

			if (!work_busy(&dw->work) && need_update(cpu))
				queue_delayed_work_on(cpu, mm_percpu_wq, dw, 0);
		}

		cond_resched();
	}
	cpus_read_unlock();

	schedule_delayed_work(&shepherd,
		round_jiffies_relative(sysctl_stat_interval));
}

static void __init start_shepherd_timer(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		INIT_DEFERRABLE_WORK(per_cpu_ptr(&vmstat_work, cpu),
			vmstat_update);

		/*
		 * For secondary CPUs during CPU hotplug scenarios,
		 * vmstat_cpu_online() will enable the work.
		 * mm/vmstat:online enables and disables vmstat_work
		 * symmetrically during CPU hotplug events.
		 */
		if (!cpu_online(cpu))
			disable_delayed_work_sync(&per_cpu(vmstat_work, cpu));
	}

	schedule_delayed_work(&shepherd,
		round_jiffies_relative(sysctl_stat_interval));
}

static void __init init_cpu_node_state(void)
{
	int node;

	for_each_online_node(node) {
		if (!cpumask_empty(cpumask_of_node(node)))
			node_set_state(node, N_CPU);
	}
}

static int vmstat_cpu_online(unsigned int cpu)
{
	if (vmstat_late_init_done)
		refresh_zone_stat_thresholds();

	if (!node_state(cpu_to_node(cpu), N_CPU)) {
		node_set_state(cpu_to_node(cpu), N_CPU);
	}
	enable_delayed_work(&per_cpu(vmstat_work, cpu));

	return 0;
}

static int vmstat_cpu_down_prep(unsigned int cpu)
{
	disable_delayed_work_sync(&per_cpu(vmstat_work, cpu));
	return 0;
}

static int vmstat_cpu_dead(unsigned int cpu)
{
	const struct cpumask *node_cpus;
	int node;

	node = cpu_to_node(cpu);

	refresh_zone_stat_thresholds();
	node_cpus = cpumask_of_node(node);
	if (!cpumask_empty(node_cpus))
		return 0;

	node_clear_state(node, N_CPU);

	return 0;
}

static int __init vmstat_late_init(void)
{
	refresh_zone_stat_thresholds();
	vmstat_late_init_done = 1;

	return 0;
}
late_initcall(vmstat_late_init);
#endif

#ifdef CONFIG_PROC_FS
static const struct ctl_table vmstat_table[] = {
#ifdef CONFIG_SMP
	{
		.procname	= "stat_interval",
		.data		= &sysctl_stat_interval,
		.maxlen		= sizeof(sysctl_stat_interval),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_jiffies,
	},
	{
		.procname	= "stat_refresh",
		.data		= NULL,
		.maxlen		= 0,
		.mode		= 0600,
		.proc_handler	= vmstat_refresh,
	},
#endif
#ifdef CONFIG_NUMA
	{
		.procname	= "numa_stat",
		.data		= &sysctl_vm_numa_stat,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sysctl_vm_numa_stat_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
#endif
};
#endif

struct workqueue_struct *mm_percpu_wq;

void __init init_mm_internals(void)
{
	int ret __maybe_unused;

	mm_percpu_wq = alloc_workqueue("mm_percpu_wq",
				       WQ_MEM_RECLAIM | WQ_PERCPU, 0);

#ifdef CONFIG_SMP
	ret = cpuhp_setup_state_nocalls(CPUHP_MM_VMSTAT_DEAD, "mm/vmstat:dead",
					NULL, vmstat_cpu_dead);
	if (ret < 0)
		pr_err("vmstat: failed to register 'dead' hotplug state\n");

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "mm/vmstat:online",
					vmstat_cpu_online,
					vmstat_cpu_down_prep);
	if (ret < 0)
		pr_err("vmstat: failed to register 'online' hotplug state\n");

	cpus_read_lock();
	init_cpu_node_state();
	cpus_read_unlock();

	start_shepherd_timer();
#endif
#ifdef CONFIG_PROC_FS
	proc_create_seq("buddyinfo", 0444, NULL, &fragmentation_op);
	proc_create_seq("pagetypeinfo", 0400, NULL, &pagetypeinfo_op);
	proc_create_seq("vmstat", 0444, NULL, &vmstat_op);
	proc_create_seq("zoneinfo", 0444, NULL, &zoneinfo_op);
	register_sysctl_init("vm", vmstat_table);
#endif
}

#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_COMPACTION)

/*
 * Return an index indicating how much of the available free memory is
 * unusable for an allocation of the requested size.
 */
static int unusable_free_index(unsigned int order,
				struct contig_page_info *info)
{
	/* No free memory is interpreted as all free memory is unusable */
	if (info->free_pages == 0)
		return 1000;

	/*
	 * Index should be a value between 0 and 1. Return a value to 3
	 * decimal places.
	 *
	 * 0 => no fragmentation
	 * 1 => high fragmentation
	 */
	return div_u64((info->free_pages - (info->free_blocks_suitable << order)) * 1000ULL, info->free_pages);

}

static void unusable_show_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	unsigned int order;
	int index;
	struct contig_page_info info;

	seq_printf(m, "Node %d, zone %8s ",
				pgdat->node_id,
				zone->name);
	for (order = 0; order < NR_PAGE_ORDERS; ++order) {
		fill_contig_page_info(zone, order, &info);
		index = unusable_free_index(order, &info);
		seq_printf(m, "%d.%03d ", index / 1000, index % 1000);
	}

	seq_putc(m, '\n');
}

/*
 * Display unusable free space index
 *
 * The unusable free space index measures how much of the available free
 * memory cannot be used to satisfy an allocation of a given size and is a
 * value between 0 and 1. The higher the value, the more of free memory is
 * unusable and by implication, the worse the external fragmentation is. This
 * can be expressed as a percentage by multiplying by 100.
 */
static int unusable_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	/* check memoryless node */
	if (!node_state(pgdat->node_id, N_MEMORY))
		return 0;

	walk_zones_in_node(m, pgdat, true, false, unusable_show_print);

	return 0;
}

static const struct seq_operations unusable_sops = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= unusable_show,
};

DEFINE_SEQ_ATTRIBUTE(unusable);

static void extfrag_show_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	unsigned int order;
	int index;

	/* Alloc on stack as interrupts are disabled for zone walk */
	struct contig_page_info info;

	seq_printf(m, "Node %d, zone %8s ",
				pgdat->node_id,
				zone->name);
	for (order = 0; order < NR_PAGE_ORDERS; ++order) {
		fill_contig_page_info(zone, order, &info);
		index = __fragmentation_index(order, &info);
		seq_printf(m, "%2d.%03d ", index / 1000, index % 1000);
	}

	seq_putc(m, '\n');
}

/*
 * Display fragmentation index for orders that allocations would fail for
 */
static int extfrag_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	walk_zones_in_node(m, pgdat, true, false, extfrag_show_print);

	return 0;
}

static const struct seq_operations extfrag_sops = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= extfrag_show,
};

DEFINE_SEQ_ATTRIBUTE(extfrag);

static int __init extfrag_debug_init(void)
{
	struct dentry *extfrag_debug_root;

	extfrag_debug_root = debugfs_create_dir("extfrag", NULL);

	debugfs_create_file("unusable_index", 0444, extfrag_debug_root, NULL,
			    &unusable_fops);

	debugfs_create_file("extfrag_index", 0444, extfrag_debug_root, NULL,
			    &extfrag_fops);

	return 0;
}

module_init(extfrag_debug_init);

#endif
