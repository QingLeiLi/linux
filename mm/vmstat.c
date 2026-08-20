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
		/* 遍历所有在线节点
		 * pgdat: Page Data，NUMA 节点的核心数据结构
		 */

		struct per_cpu_nodestat *p;

		p = per_cpu_ptr(pgdat->per_cpu_nodestats, cpu);
		/* 获取下线 CPU 在该节点的统计数据
		 * per_cpu_ptr: 获取指定 CPU 的 per-CPU 变量指针
		 */

		for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++)
			/* 遍历所有节点统计项 */

			if (p->vm_node_stat_diff[i]) {
				/* 如果该项有差分值（非零） */

				int v;

				v = p->vm_node_stat_diff[i];
				/* 读取差分值 */

				p->vm_node_stat_diff[i] = 0;
				/* 清零差分（已读取） */

				atomic_long_add(v, &pgdat->vm_stat[i]);
				/* 原子地添加到节点的全局统计
				 * 虽然 CPU 已下线，但其他 CPU 可能读取 pgdat->vm_stat
				 */

				global_node_diff[i] += v;
				/* 累加到全局差分数组
				 * 用于后续折叠到系统级全局计数器
				 */
			}
	}

	fold_diff(global_zone_diff, global_node_diff);
	/* 折叠全局差分
	 * 将累积的 zone 和 node 差分添加到系统级全局计数器
	 */
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
 *
 * 【工作内容】
 * 1. 将 zone 统计差分排空到全局
 * 2. 将 NUMA 事件排空到全局
 */
void drain_zonestat(struct zone *zone, struct per_cpu_zonestat *pzstats)
{
	unsigned long v;
	/* 临时变量，存储差分值 */

	int i;

	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++) {
		/* 遍历所有 zone 统计项 */

		if (pzstats->vm_stat_diff[i]) {
			/* 如果该项有差分值 */

			v = pzstats->vm_stat_diff[i];
			/* 读取差分值 */

			pzstats->vm_stat_diff[i] = 0;
			/* 清零（安全，因为 zone 不再被填充） */

			zone_page_state_add(v, zone, i);
			/* 添加到 zone 的全局统计
			 * 这会更新 zone->vm_stat[i]
			 */
		}
	}

#ifdef CONFIG_NUMA
	for (i = 0; i < NR_VM_NUMA_EVENT_ITEMS; i++) {
		/* 遍历所有 NUMA 事件项 */

		if (pzstats->vm_numa_event[i]) {
			/* 如果该项有事件计数 */

			v = pzstats->vm_numa_event[i];
			/* 读取事件计数 */

			pzstats->vm_numa_event[i] = 0;
			/* 清零 */

			zone_numa_event_add(v, zone, i);
			/* 添加到 zone 的 NUMA 事件统计
			 * 同时会更新全局 NUMA 统计
			 */
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
/*
 * 【函数】sum_zone_node_page_state - 计算节点的统计项总值
 * @node: 节点 ID
 * @item: 统计项类型（zone_stat_item 枚举）
 * @return: 该节点所有 zone 的统计项总和
 *
 * 【功能】
 * 确定某个节点的统计项值。
 *
 * 【性能考虑】
 * 此函数在 NUMA 机器上频繁调用，因此尽可能节俭。
 * - 直接访问 zone 数组，避免额外的函数调用开销
 * - 简单的循环累加，无复杂计算
 *
 * 【使用场景】
 * - 查询节点级别的统计信息
 * - NUMA 感知的内存分配决策
 * - 节点间负载均衡
 */
unsigned long sum_zone_node_page_state(int node,
				 enum zone_stat_item item)
{
	struct zone *zones = NODE_DATA(node)->node_zones;
	/* 获取该节点的所有 zone
	 * NODE_DATA(node): 获取节点的 pg_data_t 结构
	 * node_zones: zone 数组（ZONE_DMA, ZONE_NORMAL 等）
	 */

	int i;
	unsigned long count = 0;
	/* 累加器 */

	for (i = 0; i < MAX_NR_ZONES; i++)
		/* 遍历所有可能的 zone 类型
		 * MAX_NR_ZONES: 系统支持的最大 zone 数量
		 */

		count += zone_page_state(zones + i, item);
		/* 累加每个 zone 的统计值
		 * zone_page_state: 获取 zone 的当前统计值（包括差分）
		 */

	return count;
	/* 返回总计值 */
}

/* Determine the per node value of a numa stat item. */
/*
 * 【函数】sum_zone_numa_event_state - 计算节点的 NUMA 事件总数
 * @node: 节点 ID
 * @item: NUMA 统计项类型（numa_stat_item 枚举）
 * @return: 该节点所有 zone 的 NUMA 事件总和
 *
 * 【功能】
 * 确定某个节点的 NUMA 统计项值。
 *
 * 【与 sum_zone_node_page_state 的区别】
 * - 该函数处理 NUMA 事件（如 NUMA_HIT、NUMA_MISS）
 * - sum_zone_node_page_state 处理一般的页面状态统计
 */
unsigned long sum_zone_numa_event_state(int node,
				 enum numa_stat_item item)
{
	struct zone *zones = NODE_DATA(node)->node_zones;
	/* 获取节点的 zone 数组 */

	unsigned long count = 0;
	int i;

	for (i = 0; i < MAX_NR_ZONES; i++)
		/* 遍历所有 zone */

		count += zone_numa_event_state(zones + i, item);
		/* 累加每个 zone 的 NUMA 事件计数
		 * zone_numa_event_state: 获取 zone 的 NUMA 事件统计
		 */

	return count;
}

/*
 * Determine the per node value of a stat item.
 */
/*
 * 【函数】node_page_state_pages - 获取节点统计项值（以页为单位）
 * @pgdat: 节点数据结构
 * @item: 节点统计项类型（node_stat_item 枚举）
 * @return: 统计项的值（页数）
 *
 * 【功能】
 * 确定节点的统计项值。
 *
 * 【负值处理】
 * 在 SMP 系统中，由于 per-CPU 差分的存在，
 * 全局计数器可能暂时为负（某些 CPU 的差分尚未折叠）。
 * 此函数将负值视为 0。
 *
 * 【使用场景】
 * - 内部使用，返回原始页数
 * - 不检查统计项单位是否为页
 */
unsigned long node_page_state_pages(struct pglist_data *pgdat,
				    enum node_stat_item item)
{
	long x = atomic_long_read(&pgdat->vm_stat[item]);
	/* 原子读取节点统计值
	 * 这是全局计数器，不包括尚未折叠的 per-CPU 差分
	 */

#ifdef CONFIG_SMP
	if (x < 0)
		x = 0;
	/* SMP 系统中，由于差分机制，值可能暂时为负
	 * 将负值归零，避免返回无意义的负页数
	 *
	 * 【为什么会出现负值】
	 * 1. CPU A 释放页面：本地差分 -= 1
	 * 2. CPU B 分配页面：本地差分 += 1
	 * 3. 如果 CPU B 的差分先折叠，全局计数器 += 1
	 * 4. CPU A 的差分尚未折叠，全局看起来多了 1 页
	 * 5. 反过来，如果 CPU A 先折叠，全局会暂时为负
	 */
#endif
	return x;
}

/*
 * 【函数】node_page_state - 获取节点统计项值（带单位检查）
 * @pgdat: 节点数据结构
 * @item: 节点统计项类型
 * @return: 统计项的值（页数）
 *
 * 【与 node_page_state_pages 的区别】
 * 此函数会检查统计项是否以页为单位。
 * 如果统计项以字节为单位，会触发警告。
 *
 * 【使用场景】
 * - 外部 API，确保调用者使用正确的单位
 * - 防止将字节单位的统计项误当作页数
 */
unsigned long node_page_state(struct pglist_data *pgdat,
			      enum node_stat_item item)
{
	VM_WARN_ON_ONCE(vmstat_item_in_bytes(item));
	/* 警告：如果该统计项以字节为单位
	 * vmstat_item_in_bytes: 检查统计项是否以字节计量
	 * VM_WARN_ON_ONCE: 只警告一次，避免日志泛滥
	 *
	 * 【哪些项以字节为单位】
	 * - NR_SLAB_RECLAIMABLE_B: 可回收 slab（字节）
	 * - NR_SLAB_UNRECLAIMABLE_B: 不可回收 slab（字节）
	 * 这些项应该用 node_page_state_pages() * PAGE_SIZE 访问
	 */

	return node_page_state_pages(pgdat, item);
	/* 返回页数 */
}
#endif

/*
 * Count number of pages "struct page" and "struct page_ext" consume.
 * nr_memmap_boot_pages: # of pages allocated by boot allocator
 * nr_memmap_pages: # of pages that were allocated by buddy allocator
 */
/*
 * 【内存映射页面计数】
 *
 * 统计 "struct page" 和 "struct page_ext" 消耗的页面数。
 *
 * 【两个计数器】
 * - nr_memmap_boot_pages: 由启动分配器分配的页面数
 * - nr_memmap_pages: 由伙伴分配器分配的页面数
 *
 * 【为什么需要两个计数器】
 * - 启动早期使用简单的启动分配器（bootmem/memblock）
 * - 启动后期使用伙伴分配器（buddy allocator）
 * - 分别统计有助于了解内存管理结构的开销
 *
 * 【struct page 的作用】
 * 每个物理页面都有一个 struct page 描述符。
 * 例如，64GB 内存需要约 1GB 来存储 struct page。
 *
 * 【struct page_ext 的作用】
 * 扩展的页面信息（如页面所有者跟踪、页面毒化）。
 * 只有在启用某些调试功能时才分配。
 */
static atomic_long_t nr_memmap_boot_pages = ATOMIC_LONG_INIT(0);
/* 启动时分配的内存映射页面计数
 * ATOMIC_LONG_INIT(0): 初始化为 0
 */

static atomic_long_t nr_memmap_pages = ATOMIC_LONG_INIT(0);
/* 运行时分配的内存映射页面计数 */

/*
 * 【函数】memmap_boot_pages_add - 增加启动内存映射页面计数
 * @delta: 增量（可为负，表示释放）
 *
 * 【调用时机】
 * 启动分配器分配/释放 struct page 或 struct page_ext 时。
 */
void memmap_boot_pages_add(long delta)
{
	atomic_long_add(delta, &nr_memmap_boot_pages);
	/* 原子地增加计数
	 * 虽然启动时通常是单线程，但使用原子操作保证安全
	 */
}

/*
 * 【函数】memmap_pages_add - 增加运行时内存映射页面计数
 * @delta: 增量（可为负，表示释放）
 *
 * 【调用时机】
 * 伙伴分配器分配/释放 struct page 或 struct page_ext 时。
 *
 * 【使用场景】
 * - 内存热插拔：添加新内存时分配 struct page
 * - 稀疏内存模型：按需分配 struct page
 */
void memmap_pages_add(long delta)
{
	atomic_long_add(delta, &nr_memmap_pages);
	/* 原子地增加计数
	 * 运行时可能有多个 CPU 同时操作，必须使用原子操作
	 */
}

#ifdef CONFIG_COMPACTION

/*
 * 【结构】contig_page_info - 连续页面信息
 *
 * 用于内存碎片分析，统计指定阶数的连续内存块信息。
 *
 * 【字段说明】
 * @free_pages: 总空闲页面数
 *              包括所有阶数的空闲页面总和
 *
 * @free_blocks_total: 总空闲块数
 *                     所有阶数的空闲块总数
 *
 * @free_blocks_suitable: 适合的空闲块数
 *                        阶数 >= 目标阶数的空闲块数
 *                        这些块可以满足目标阶数的分配
 *
 * 【使用场景】
 * - 碎片索引计算：评估内存碎片程度
 * - 压实决策：决定是否需要内存压实
 * - 调试分析：/sys/kernel/debug/extfrag/ 接口
 *
 * 【示例】
 * 假设要分配 order=2（4 个连续页）：
 * - free_pages = 1000 页
 * - free_blocks_total = 500 个块（各种阶数）
 * - free_blocks_suitable = 50 个块（order >= 2）
 *
 * 那么：
 * - 可用于分配的页数 = 50 * 4 = 200 页
 * - 不可用页数 = 1000 - 200 = 800 页（碎片化）
 * - 碎片指数 = 800 / 1000 = 0.8（80% 的空闲内存因碎片而不可用）
 */
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
/*
 * 【函数】fill_contig_page_info - 填充连续页面信息
 * @zone: 内存区域
 * @suitable_order: 目标阶数（期望的连续页面大小）
 * @info: 输出参数，存储统计信息
 *
 * 【功能】
 * 收集指定 zone 的连续页面分配能力信息，
 * 用于评估内存碎片程度。
 *
 * 【工作流程】
 * 1. 初始化统计信息为 0
 * 2. 遍历所有阶数的空闲列表
 * 3. 统计总空闲页数、总空闲块数、适合的空闲块数
 *
 * 【为什么使用 data_race】
 * nr_free 访问是无锁的，仅用于诊断目的。
 * data_race 用于避免 KCSAN（内核并发检测器）警告。
 *
 * 【适合的空闲块计算】
 * order >= suitable_order 的块才适合分配。
 * 例如：要分配 order=2 (4页)，order=3 (8页) 的块也适合，
 * 可以拆分为 2 个 order=2 的块。
 */
static void fill_contig_page_info(struct zone *zone,
				unsigned int suitable_order,
				struct contig_page_info *info)
{
	unsigned int order;

	info->free_pages = 0;
	/* 初始化：总空闲页数 */

	info->free_blocks_total = 0;
	/* 初始化：总空闲块数 */

	info->free_blocks_suitable = 0;
	/* 初始化：适合的空闲块数（order >= suitable_order） */

	for (order = 0; order < NR_PAGE_ORDERS; order++) {
		/* 遍历所有阶数（0 到 MAX_PAGE_ORDER）
		 * order=0: 单页
		 * order=1: 2 页连续
		 * order=2: 4 页连续
		 * ...
		 */

		unsigned long blocks;

		/*
		 * Count number of free blocks.
		 *
		 * Access to nr_free is lockless as nr_free is used only for
		 * diagnostic purposes. Use data_race to avoid KCSAN warning.
		 */
		/*
		 * 统计空闲块数量。
		 *
		 * 访问 nr_free 是无锁的，因为 nr_free 仅用于诊断目的。
		 * 使用 data_race 避免 KCSAN 警告。
		 */
		blocks = data_race(zone->free_area[order].nr_free);
		/* 读取该阶数的空闲块数
		 * free_area[order]: 该阶数的空闲列表
		 * nr_free: 该阶数的空闲块数量
		 *
		 * 【data_race 的作用】
		 * 告诉 KCSAN 这是故意的无锁访问，不是数据竞争 bug
		 */

		info->free_blocks_total += blocks;
		/* 累加空闲块总数 */

		/* Count free base pages */
		/* 统计空闲基础页面数 */
		info->free_pages += blocks << order;
		/* 累加空闲页总数
		 * blocks << order = blocks * (2^order)
		 *
		 * 【示例】
		 * 如果有 10 个 order=2 的块：
		 * free_pages += 10 << 2 = 10 * 4 = 40 页
		 */

		/* Count the suitable free blocks */
		/* 统计适合的空闲块数 */
		if (order >= suitable_order)
			/* 如果该阶数 >= 目标阶数，这些块适合分配 */

			info->free_blocks_suitable += blocks <<
						(order - suitable_order);
			/* 计算等效的目标阶数块数
			 *
			 * 【为什么要 << (order - suitable_order)】
			 * 一个 order=N 的块可以拆分为多个 order=M 的块（N > M）
			 * 拆分数量 = 2^(N-M)
			 *
			 * 【示例】
			 * 目标 suitable_order = 2 (4页)
			 * 如果有 1 个 order=4 的块 (16页)：
			 * 可拆分为 1 << (4-2) = 4 个 order=2 的块
			 * free_blocks_suitable += 1 << 2 = 4
			 */
	}
}

/*
 * A fragmentation index only makes sense if an allocation of a requested
 * size would fail. If that is true, the fragmentation index indicates
 * whether external fragmentation or a lack of memory was the problem.
 * The value can be used to determine if page reclaim or compaction
 * should be used
 */
/*
 * 【函数】__fragmentation_index - 计算碎片化索引（内部版本）
 * @order: 分配阶数
 * @info: 连续页面信息
 * @return: 碎片化索引（-1000 到 1000）
 *
 * 【功能】
 * 碎片化索引仅在请求大小的分配会失败时才有意义。
 * 如果确实会失败，碎片化索引指示问题是外部碎片还是内存不足。
 * 该值可用于决定应该使用页面回收还是内存压实。
 *
 * 【返回值含义】
 * - -1000: 分配会成功（有足够的连续块）
 * - 0: 分配失败是因为内存不足（真的没有足够内存）
 * - 1000: 分配失败是因为碎片化（有内存但不连续）
 * - 中间值: 部分是内存不足，部分是碎片化
 *
 * 【决策指导】
 * - 接近 0: 应该回收页面（增加空闲内存）
 * - 接近 1000: 应该压实内存（减少碎片）
 */
static int __fragmentation_index(unsigned int order, struct contig_page_info *info)
{
	unsigned long requested = 1UL << order;
	/* 请求的页面数 = 2^order
	 *
	 * 【示例】
	 * order=0: requested=1 页
	 * order=2: requested=4 页
	 * order=4: requested=16 页
	 */

	if (WARN_ON_ONCE(order > MAX_PAGE_ORDER))
		return 0;
	/* 检查阶数是否合法
	 * MAX_PAGE_ORDER: 系统支持的最大阶数（通常是 10 或 11）
	 * WARN_ON_ONCE: 只警告一次，避免日志泛滥
	 */

	if (!info->free_blocks_total)
		return 0;
	/* 没有空闲块，返回 0（内存不足）
	 * 分配失败完全是因为没有内存
	 */

	/* Fragmentation index only makes sense when a request would fail */
	/* 碎片化索引仅在请求会失败时才有意义 */
	if (info->free_blocks_suitable)
		return -1000;
	/* 如果有合适的空闲块，分配不会失败
	 * 返回 -1000 表示"不适用"
	 *
	 * 【为什么返回负值】
	 * 碎片索引是为失败的分配设计的。
	 * 如果分配会成功，索引没有意义。
	 * 负值表示这种"不适用"的情况。
	 */

	/*
	 * Index is between 0 and 1 so return within 3 decimal places
	 *
	 * 0 => allocation would fail due to lack of memory
	 * 1 => allocation would fail due to fragmentation
	 */
	/*
	 * 索引在 0 和 1 之间，返回值乘以 1000（保留 3 位小数）
	 *
	 * 0 => 分配失败是由于内存不足
	 * 1000 => 分配失败是由于碎片化
	 *
	 * 【计算公式推导】
	 * 理想情况下（无碎片）：所有空闲页都在一个大块中
	 * 实际情况：空闲页分散在多个小块中
	 *
	 * 碎片化程度 = 1 - (实际可用度 / 理想可用度)
	 * 实际可用度 = 0（因为没有 suitable 块）
	 * 理想可用度 = free_pages / requested（如果所有页都连续）
	 *
	 * 但我们需要考虑块的数量：
	 * fragmentation_index = 1 - [(free_pages/requested) / free_blocks_total]
	 *
	 * 【直觉理解】
	 * - 如果 free_pages 很少：分子小，索引接近 1000，但实际是内存不足
	 * - 如果 free_blocks_total 很大：分母大，索引接近 1000（碎片化严重）
	 * - 如果 free_pages 很多但 free_blocks_total 也很多：高度碎片化
	 *
	 * 【实现技巧】
	 * 为了避免浮点运算：
	 * 1000 - (1000 + (free_pages * 1000 / requested)) / free_blocks_total
	 */
	return 1000 - div_u64( (1000+(div_u64(info->free_pages * 1000ULL, requested))), info->free_blocks_total);
	/* div_u64: 64位除法
	 * 先计算 free_pages * 1000 / requested
	 * 加上 1000
	 * 再除以 free_blocks_total
	 * 最后用 1000 减去结果
	 */
}

/*
 * Calculates external fragmentation within a zone wrt the given order.
 * It is defined as the percentage of pages found in blocks of size
 * less than 1 << order. It returns values in range [0, 100].
 */
/*
 * 【函数】extfrag_for_order - 计算 zone 中针对给定阶数的外部碎片率
 * @zone: 内存区域
 * @order: 目标分配阶数
 * @return: 外部碎片百分比（0-100）
 *
 * 【定义】
 * 外部碎片率定义为：在小于 1 << order 大小的块中找到的页面百分比。
 *
 * 【计算公式】
 * extfrag = (free_pages - suitable_pages) / free_pages * 100
 * 其中 suitable_pages = free_blocks_suitable << order
 *
 * 【与 fragmentation_index 的区别】
 * - extfrag_for_order: 简单的百分比，表示有多少空闲内存因碎片而不可用
 * - fragmentation_index: 复杂的指标，区分"内存不足"和"碎片化"
 *
 * 【示例计算】
 * 假设 zone 有 1000 个空闲页，order=2（需要 4 页连续）：
 * - free_blocks_suitable = 50 个（order >= 2 的块）
 * - suitable_pages = 50 << 2 = 200 页
 * - extfrag = (1000 - 200) / 1000 * 100 = 80%
 *
 * 解释：80% 的空闲内存在太小的块中，无法满足 order=2 的分配。
 *
 * 【使用场景】
 * - 评估内存压实的必要性
 * - 监控系统碎片化趋势
 * - /sys/kernel/debug/extfrag/extfrag_index 接口
 */
unsigned int extfrag_for_order(struct zone *zone, unsigned int order)
{
	struct contig_page_info info;

	fill_contig_page_info(zone, order, &info);
	/* 收集 zone 的连续页面信息 */

	if (info.free_pages == 0)
		return 0;
	/* 没有空闲页，碎片率为 0（严格说应该是"不适用"） */

	return div_u64((info.free_pages -
			(info.free_blocks_suitable << order)) * 100,
			info.free_pages);
	/* 计算碎片率百分比
	 *
	 * 分子：不可用的空闲页数 = 总空闲页 - 适合的页数
	 * 分母：总空闲页数
	 *
	 * 【为什么 << order】
	 * free_blocks_suitable 是块数，需要转换为页数
	 * 一个 order=N 的块包含 2^N 页
	 */
}

/* Same as __fragmentation index but allocs contig_page_info on stack */
/*
 * 【函数】fragmentation_index - 计算碎片化索引（用户接口版本）
 * @zone: 内存区域
 * @order: 分配阶数
 * @return: 碎片化索引（-1000 到 1000）
 *
 * 【功能】
 * 与 __fragmentation_index 相同，但在栈上分配 contig_page_info。
 *
 * 【与 __fragmentation_index 的区别】
 * - __fragmentation_index: 接受已填充的 info 结构（内部使用）
 * - fragmentation_index: 自己填充 info 结构（外部接口）
 *
 * 【为什么在栈上分配】
 * contig_page_info 结构很小（3 个 unsigned long），
 * 在栈上分配避免了动态内存分配的开销。
 *
 * 【调用者】
 * - /sys/kernel/debug/extfrag/extfrag_index
 * - 内核内部的碎片分析代码
 */
int fragmentation_index(struct zone *zone, unsigned int order)
{
	struct contig_page_info info;
	/* 在栈上分配 info 结构
	 * 大小约为 24 字节（64 位系统）
	 */

	fill_contig_page_info(zone, order, &info);
	/* 填充连续页面信息 */

	return __fragmentation_index(order, &info);
	/* 调用内部函数计算索引 */
}
#endif

/*
 * ============================================================================
 * 【统计项名称文本】
 *
 * 定义所有 vmstat 统计项的文本名称，用于 /proc/vmstat 等接口。
 * ============================================================================
 */

#if defined(CONFIG_PROC_FS) || defined(CONFIG_SYSFS) || \
    defined(CONFIG_NUMA) || defined(CONFIG_MEMCG)
/*
 * 【条件编译】仅在需要文本名称时编译以下代码
 *
 * 这些文本名称用于：
 * - CONFIG_PROC_FS: /proc/vmstat、/proc/zoneinfo 等
 * - CONFIG_SYSFS: /sys/devices/system/node/nodeN/vmstat
 * - CONFIG_NUMA: NUMA 统计显示
 * - CONFIG_MEMCG: 内存控制组统计显示
 */

/*
 * 【宏定义】根据配置生成不同 zone 类型的文本
 *
 * 这些宏根据内核配置动态生成 zone 类型的文本名称。
 * 使用宏的好处：
 * 1. 避免在不支持的配置中浪费空间
 * 2. 自动适配不同的硬件架构
 * 3. 编译时决定，无运行时开销
 *
 * 【宏的使用方式】
 * TEXT_FOR_DMA(PGALLOC, "pgalloc")
 * 展开为：[PGALLOC_DMA] = "pgalloc_dma",
 *
 * 【xx 和 yy 参数】
 * - xx: 枚举名前缀（如 PGALLOC）
 * - yy: 文本名前缀（如 "pgalloc"）
 * - ##: 宏连接符，将 xx 和 _DMA 连接为 xx_DMA
 */

#ifdef CONFIG_ZONE_DMA
#define TEXT_FOR_DMA(xx, yy) [xx##_DMA] = yy "_dma",
/* DMA zone（Direct Memory Access）
 * 某些老旧设备只能访问低地址内存（通常前 16MB）
 *
 * 【为什么需要 DMA zone】
 * ISA 设备、软盘控制器等老旧硬件的地址总线位数有限，
 * 只能访问低 16MB 或 24MB 的物理内存。
 *
 * 【示例展开】
 * TEXT_FOR_DMA(PGALLOC, "pgalloc")
 * 展开为：[PGALLOC_DMA] = "pgalloc_dma",
 */
#else
#define TEXT_FOR_DMA(xx, yy)
/* 如果未配置 DMA zone，宏展开为空
 * 不生成相关的文本条目
 */
#endif

#ifdef CONFIG_ZONE_DMA32
#define TEXT_FOR_DMA32(xx, yy) [xx##_DMA32] = yy "_dma32",
/* DMA32 zone
 * 用于 64 位系统中只能访问 4GB 以下内存的设备
 *
 * 【为什么需要 DMA32 zone】
 * 某些 32 位 DMA 设备在 64 位系统中只能访问前 4GB 内存。
 * 例如：某些网卡、声卡的 DMA 控制器是 32 位的。
 *
 * 【x86-64 系统的 zone 布局】
 * - ZONE_DMA: 0-16MB（ISA 设备）
 * - ZONE_DMA32: 16MB-4GB（32 位 DMA 设备）
 * - ZONE_NORMAL: 4GB 以上（所有设备可访问）
 */
#else
#define TEXT_FOR_DMA32(xx, yy)
#endif

#ifdef CONFIG_HIGHMEM
#define TEXT_FOR_HIGHMEM(xx, yy) [xx##_HIGH] = yy "_high",
/* HIGHMEM zone（高端内存）
 * 用于 32 位系统中无法直接映射的内存
 *
 * 【为什么需要 HIGHMEM zone】
 * 32 位系统的虚拟地址空间只有 4GB（实际可用约 3GB）。
 * 如果物理内存超过 1GB，多余的内存无法永久映射到内核地址空间。
 * 这部分内存称为"高端内存"，需要时才临时映射。
 *
 * 【32 位 Linux 的内存布局（如 x86-32 with PAE）】
 * - ZONE_DMA: 0-16MB
 * - ZONE_NORMAL: 16MB-896MB（永久映射）
 * - ZONE_HIGHMEM: 896MB 以上（临时映射）
 *
 * 【64 位系统】
 * 没有 HIGHMEM zone，因为虚拟地址空间足够大。
 */
#else
#define TEXT_FOR_HIGHMEM(xx, yy)
#endif

#ifdef CONFIG_ZONE_DEVICE
#define TEXT_FOR_DEVICE(xx, yy) [xx##_DEVICE] = yy "_device",
/* DEVICE zone（设备内存）
 * 用于非易失性内存（NVDIMM）或 GPU 内存等特殊设备内存
 *
 * 【为什么需要 DEVICE zone】
 * 某些设备有自己的内存（如 GPU 显存、持久内存），
 * 内核需要管理这些内存但它们有特殊属性：
 * - 可能不是字节寻址的
 * - 访问速度可能不同
 * - 可能需要特殊的驱动程序访问
 *
 * 【使用场景】
 * - 持久内存（PMEM）
 * - GPU 显存映射
 * - HMM（Heterogeneous Memory Management）
 */
#else
#define TEXT_FOR_DEVICE(xx, yy)
#endif

/*
 * 【宏】TEXTS_FOR_ZONES - 生成所有 zone 类型的文本条目
 * @xx: 统计项枚举前缀
 * @yy: 统计项文本前缀
 *
 * 【功能】
 * 组合所有启用的 zone 类型，生成完整的统计项文本数组条目。
 *
 * 【展开示例】
 * TEXTS_FOR_ZONES(PGALLOC, "pgalloc")
 *
 * 可能展开为（取决于配置）：
 * [PGALLOC_DMA] = "pgalloc_dma",        // 如果有 CONFIG_ZONE_DMA
 * [PGALLOC_DMA32] = "pgalloc_dma32",    // 如果有 CONFIG_ZONE_DMA32
 * [PGALLOC_NORMAL] = "pgalloc_normal",  // 总是存在
 * [PGALLOC_HIGH] = "pgalloc_high",      // 如果有 CONFIG_HIGHMEM
 * [PGALLOC_MOVABLE] = "pgalloc_movable",// 总是存在
 * [PGALLOC_DEVICE] = "pgalloc_device",  // 如果有 CONFIG_ZONE_DEVICE
 *
 * 【ZONE_NORMAL】
 * 总是存在，是默认的内存 zone。
 * 所有可以被内核直接访问的"普通"内存。
 *
 * 【ZONE_MOVABLE】
 * 可移动的内存 zone，用于内存热插拔和大页分配。
 * 其中的页面可以被迁移，便于整理碎片或移除内存。
 */
#define TEXTS_FOR_ZONES(xx, yy)			\
	TEXT_FOR_DMA(xx, yy)			\
	TEXT_FOR_DMA32(xx, yy)			\
	[xx##_NORMAL] = yy "_normal",		\
	TEXT_FOR_HIGHMEM(xx, yy)		\
	[xx##_MOVABLE] = yy "_movable",		\
	TEXT_FOR_DEVICE(xx, yy)

/*
 * 【数组】vmstat_text - 统计项名称文本数组
 *
 * 包含所有 vmstat 统计项的文本名称，用于：
 * - /proc/vmstat: 显示虚拟内存统计
 * - /proc/zoneinfo: 显示 zone 信息
 * - /sys/devices/system/node/nodeX/vmstat: 显示 node 统计
 *
 * 【组织结构】
 * 1. enum zone_stat_item 计数器（zone 级别统计）
 * 2. enum numa_stat_item 计数器（NUMA 事件统计）
 * 3. enum node_stat_item 计数器（node 级别统计）
 * 4. enum vm_event_item 计数器（VM 事件统计）
 */
const char * const vmstat_text[] = {
	/* enum zone_stat_item counters */
	/* zone 级别统计项 */
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
	/* NUMA 事件统计项
	 *
	 * 【重新定义索引宏】
	 * 这里重新定义 I(x) 是因为 NUMA 统计项在数组中的位置
	 * 紧跟在 zone 统计项之后。
	 */
#define I(x) (NR_VM_ZONE_STAT_ITEMS + x)
/* 索引偏移 = zone 统计项数量 + x */

#ifdef CONFIG_NUMA
	[I(NUMA_HIT)]				= "numa_hit",
	/* 本地节点分配成功：请求的内存在期望的节点上成功分配 */

	[I(NUMA_MISS)]				= "numa_miss",
	/* 远程节点分配：请求本地节点但在远程节点分配 */

	[I(NUMA_FOREIGN)]			= "numa_foreign",
	/* 外来访问：其他节点请求分配到本节点 */

	[I(NUMA_INTERLEAVE_HIT)]		= "numa_interleave",
	/* 交错分配命中：使用交错策略成功分配 */

	[I(NUMA_LOCAL)]				= "numa_local",
	/* 本地 CPU 本地节点分配：最优情况 */

	[I(NUMA_OTHER)]				= "numa_other",
	/* 本地 CPU 远程节点分配：次优，可能影响性能 */
#endif
#undef I
/* 取消定义，为下一组统计项准备 */

	/* enum node_stat_item counters */
	/* node 级别统计项
	 *
	 * 【索引计算】
	 * node 统计项在数组中的位置 = zone 统计项数 + NUMA 事件项数 + x
	 */
#define I(x) (NR_VM_ZONE_STAT_ITEMS + NR_VM_NUMA_EVENT_ITEMS + x)

	/* ===== LRU 列表统计 ===== */
	[I(NR_INACTIVE_ANON)]			= "nr_inactive_anon",
	/* 非活动匿名页：不常访问的匿名页（如堆、栈），回收候选 */

	[I(NR_ACTIVE_ANON)]			= "nr_active_anon",
	/* 活动匿名页：频繁访问的匿名页，不易被回收 */

	[I(NR_INACTIVE_FILE)]			= "nr_inactive_file",
	/* 非活动文件页：不常访问的文件缓存，优先回收 */

	[I(NR_ACTIVE_FILE)]			= "nr_active_file",
	/* 活动文件页：频繁访问的文件缓存 */

	[I(NR_UNEVICTABLE)]			= "nr_unevictable",
	/* 不可回收页：被 mlock 锁定的页，不能交换或回收 */

	/* ===== Slab 分配器统计 ===== */
	[I(NR_SLAB_RECLAIMABLE_B)]		= "nr_slab_reclaimable",
	/* 可回收 slab（字节）：如 inode/dentry 缓存，内存压力时可回收 */

	[I(NR_SLAB_UNRECLAIMABLE_B)]		= "nr_slab_unreclaimable",
	/* 不可回收 slab（字节）：内核常驻数据结构，不能回收 */

	/* ===== 隔离页统计 ===== */
	[I(NR_ISOLATED_ANON)]			= "nr_isolated_anon",
	/* 隔离的匿名页：正在迁移或压实中的匿名页 */

	[I(NR_ISOLATED_FILE)]			= "nr_isolated_file",
	/* 隔离的文件页：正在迁移或压实中的文件页 */

	/* ===== 工作集（Working Set）统计 ===== */
	[I(WORKINGSET_NODES)]			= "workingset_nodes",
	/* 工作集节点数：用于检测页面缓存的颠簸 */

	[I(WORKINGSET_REFAULT_ANON)]		= "workingset_refault_anon",
	/* 匿名页重新错误：被回收的匿名页再次访问 */

	[I(WORKINGSET_REFAULT_FILE)]		= "workingset_refault_file",
	/* 文件页重新错误：被回收的文件页再次访问 */

	[I(WORKINGSET_ACTIVATE_ANON)]		= "workingset_activate_anon",
	/* 匿名页激活：重新错误的匿名页被激活 */

	[I(WORKINGSET_ACTIVATE_FILE)]		= "workingset_activate_file",
	/* 文件页激活：重新错误的文件页被激活 */

	[I(WORKINGSET_RESTORE_ANON)]		= "workingset_restore_anon",
	/* 匿名页恢复：工作集中的匿名页被恢复 */

	[I(WORKINGSET_RESTORE_FILE)]		= "workingset_restore_file",
	/* 文件页恢复：工作集中的文件页被恢复 */

	[I(WORKINGSET_NODERECLAIM)]		= "workingset_nodereclaim",
	/* 节点回收：工作集节点被回收 */

	/* ===== 页面映射统计 ===== */
	[I(NR_ANON_MAPPED)]			= "nr_anon_pages",
	/* 映射的匿名页：被映射到进程地址空间的匿名页 */

	[I(NR_FILE_MAPPED)]			= "nr_mapped",
	/* 映射的文件页：被映射到进程地址空间的文件页 */

	[I(NR_FILE_PAGES)]			= "nr_file_pages",
	/* 文件页总数：所有文件缓存页 */

	/* ===== 脏页和回写统计 ===== */
	[I(NR_FILE_DIRTY)]			= "nr_dirty",
	/* 脏页数：已修改但未写回磁盘的页 */

	[I(NR_WRITEBACK)]			= "nr_writeback",
	/* 正在回写的页：正在写回磁盘的页 */

	/* ===== 共享内存统计 ===== */
	[I(NR_SHMEM)]				= "nr_shmem",
	/* 共享内存页：tmpfs、shm 等共享内存 */

	[I(NR_SHMEM_THPS)]			= "nr_shmem_hugepages",
	/* 共享内存大页：shmem 中的透明大页 */

	[I(NR_SHMEM_PMDMAPPED)]			= "nr_shmem_pmdmapped",
	/* PMD 映射的共享内存：用 PMD 级别映射的 shmem */

	/* ===== 文件大页统计 ===== */
	[I(NR_FILE_THPS)]			= "nr_file_hugepages",
	/* 文件大页：文件缓存中的透明大页 */

	[I(NR_FILE_PMDMAPPED)]			= "nr_file_pmdmapped",
	/* PMD 映射的文件页：用 PMD 级别映射的文件页 */

	/* ===== 匿名大页统计 ===== */
	[I(NR_ANON_THPS)]			= "nr_anon_transparent_hugepages",
	/* 匿名透明大页：匿名内存中的透明大页 */

	/* ===== 页面扫描和回收统计 ===== */
	[I(NR_VMSCAN_WRITE)]			= "nr_vmscan_write",
	/* VM 扫描写：回收时写回的页数 */

	[I(NR_VMSCAN_IMMEDIATE)]		= "nr_vmscan_immediate_reclaim",
	/* 立即回收：紧急回收的页数 */

	[I(NR_DIRTIED)]				= "nr_dirtied",
	/* 已弄脏：累计被弄脏的页数 */

	[I(NR_WRITTEN)]				= "nr_written",
	/* 已写入：累计写回的页数 */

	[I(NR_THROTTLED_WRITTEN)]		= "nr_throttled_written",
	/* 限流写入：因限流而延迟写入的页数 */

	/* ===== 其他内核内存统计 ===== */
	[I(NR_KERNEL_MISC_RECLAIMABLE)]		= "nr_kernel_misc_reclaimable",
	/* 可回收的内核杂项内存 */

	[I(NR_FOLL_PIN_ACQUIRED)]		= "nr_foll_pin_acquired",
	/* 获取的 FOLL_PIN 引用：用于 DMA 等长期引用 */

	[I(NR_FOLL_PIN_RELEASED)]		= "nr_foll_pin_released",
	/* 释放的 FOLL_PIN 引用 */

	[I(NR_VMALLOC)]				= "nr_vmalloc",
	/* vmalloc 分配的页数 */

	[I(NR_KERNEL_STACK_KB)]			= "nr_kernel_stack",
	/* 内核栈占用（KB）：所有线程的内核栈 */

#if IS_ENABLED(CONFIG_SHADOW_CALL_STACK)
	[I(NR_KERNEL_SCS_KB)]			= "nr_shadow_call_stack",
	/* 影子调用栈（KB）：用于安全防护 */
#endif

	/* ===== 页表统计 ===== */
	[I(NR_PAGETABLE)]			= "nr_page_table_pages",
	/* 页表页数：用于进程页表的页数 */

	[I(NR_SECONDARY_PAGETABLE)]		= "nr_sec_page_table_pages",
	/* 二级页表页数：如 KVM EPT 页表 */

#ifdef CONFIG_IOMMU_SUPPORT
	[I(NR_IOMMU_PAGES)]			= "nr_iommu_pages",
	/* IOMMU 页数：IOMMU 页表占用 */
#endif

#ifdef CONFIG_SWAP
	[I(NR_SWAPCACHE)]			= "nr_swapcached",
	/* 交换缓存：同时在内存和交换区的页 */
#endif

	/* ===== NUMA 平衡和页面提升/降级 ===== */
#ifdef CONFIG_NUMA_BALANCING
	[I(PGPROMOTE_SUCCESS)]			= "pgpromote_success",
	/* 页面提升成功：从慢速层提升到快速层 */

	[I(PGPROMOTE_CANDIDATE)]		= "pgpromote_candidate",
	/* 提升候选：考虑提升的页数 */

	[I(PGPROMOTE_CANDIDATE_NRL)]		= "pgpromote_candidate_nrl",
	/* 非远程本地提升候选 */
#endif

	[I(PGDEMOTE_KSWAPD)]			= "pgdemote_kswapd",
	/* kswapd 降级：kswapd 将页面降级到慢速层 */

	[I(PGDEMOTE_DIRECT)]			= "pgdemote_direct",
	/* 直接降级：直接回收时的页面降级 */

	[I(PGDEMOTE_KHUGEPAGED)]		= "pgdemote_khugepaged",
	/* khugepaged 降级 */

	[I(PGDEMOTE_PROACTIVE)]			= "pgdemote_proactive",
	/* 主动降级 */

	/* ===== 页面窃取统计（回收成功）===== */
	[I(PGSTEAL_KSWAPD)]			= "pgsteal_kswapd",
	/* kswapd 窃取：kswapd 回收的页数 */

	[I(PGSTEAL_DIRECT)]			= "pgsteal_direct",
	/* 直接窃取：直接回收的页数 */

	[I(PGSTEAL_KHUGEPAGED)]			= "pgsteal_khugepaged",
	/* khugepaged 窃取 */

	[I(PGSTEAL_PROACTIVE)]			= "pgsteal_proactive",
	/* 主动窃取 */

	[I(PGSTEAL_ANON)]			= "pgsteal_anon",
	/* 匿名页窃取 */

	[I(PGSTEAL_FILE)]			= "pgsteal_file",
	/* 文件页窃取 */

	/* ===== 页面扫描统计（回收尝试）===== */
	[I(PGSCAN_KSWAPD)]			= "pgscan_kswapd",
	/* kswapd 扫描：kswapd 扫描的页数 */

	[I(PGSCAN_DIRECT)]			= "pgscan_direct",
	/* 直接扫描：直接回收扫描的页数 */

	[I(PGSCAN_KHUGEPAGED)]			= "pgscan_khugepaged",
	/* khugepaged 扫描 */

	[I(PGSCAN_PROACTIVE)]			= "pgscan_proactive",
	/* 主动扫描 */

	[I(PGSCAN_ANON)]			= "pgscan_anon",
	/* 匿名页扫描 */

	[I(PGSCAN_FILE)]			= "pgscan_file",
	/* 文件页扫描 */

	[I(PGREFILL)]				= "pgrefill",
	/* 页面重新填充：扫描后重新填充到 LRU */

	/* ===== 大页统计 ===== */
#ifdef CONFIG_HUGETLB_PAGE
	[I(NR_HUGETLB)]				= "nr_hugetlb",
	/* 大页数：预留的大页（不是透明大页）*/
#endif

	/* ===== 其他 ===== */
	[I(NR_BALLOON_PAGES)]			= "nr_balloon_pages",
	/* 气球页：虚拟化中的气球驱动占用的页 */

	[I(NR_KERNEL_FILE_PAGES)]		= "nr_kernel_file_pages",
	/* 内核文件页 */

	[I(NR_GPU_ACTIVE)]			= "nr_gpu_active",
	/* GPU 活动页 */

	[I(NR_GPU_RECLAIM)]			= "nr_gpu_reclaim",
	/* GPU 可回收页 */
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
	/* VM 事件统计项
	 *
	 * 【索引计算】
	 * VM 事件统计项在数组中的位置 =
	 *   zone 统计项数 + NUMA 事件项数 + node 统计项数 + system 统计项数 + x
	 *
	 * 【特点】
	 * VM 事件是全局计数器（非 zone/node 特定），记录各种内核事件。
	 * 这些计数器使用 per-CPU 变量，定期合并到全局。
	 */
#define I(x) (NR_VM_ZONE_STAT_ITEMS + NR_VM_NUMA_EVENT_ITEMS + \
	     NR_VM_NODE_STAT_ITEMS + NR_VM_STAT_ITEMS + x)

	/* ===== 页面 I/O 统计 ===== */
	[I(PGPGIN)]				= "pgpgin",
	/* 页面读入（页/秒）：从磁盘读入的页面数
	 * 高值表示大量页面 I/O 或内存不足（频繁换入）
	 */

	[I(PGPGOUT)]				= "pgpgout",
	/* 页面写出（页/秒）：写出到磁盘的页面数
	 * 包括脏页回写和交换写出
	 */

	[I(PSWPIN)]				= "pswpin",
	/* 交换读入（页/秒）：从交换区读入的匿名页
	 * 高值表示内存严重不足
	 */

	[I(PSWPOUT)]				= "pswpout",
	/* 交换写出（页/秒）：写出到交换区的匿名页
	 * 高值表示内存压力大
	 */

#define OFF (NR_VM_ZONE_STAT_ITEMS + NR_VM_NUMA_EVENT_ITEMS + \
	     NR_VM_NODE_STAT_ITEMS + NR_VM_STAT_ITEMS)
	/* ===== 页面分配统计（按 zone 类型）===== */
	TEXTS_FOR_ZONES(OFF+PGALLOC, "pgalloc")
	/* 各 zone 的页面分配次数
	 * pgalloc_dma / pgalloc_dma32 / pgalloc_normal / pgalloc_movable 等
	 */

	/* ===== 分配停滞统计（按 zone 类型）===== */
	TEXTS_FOR_ZONES(OFF+ALLOCSTALL, "allocstall")
	/* 各 zone 的分配停滞次数
	 * 分配失败并进入慢速路径（回收/等待）的次数
	 */

	/* ===== 跳过的页面扫描（按 zone 类型）===== */
	TEXTS_FOR_ZONES(OFF+PGSCAN_SKIP, "pgskip")
	/* 因 zone 未激活而跳过的页面扫描次数
	 * 避免在空 zone 上浪费时间
	 */
#undef OFF

	/* ===== 基本页面管理统计 ===== */
	[I(PGFREE)]				= "pgfree",
	/* 释放的页面数：总共释放回伙伴系统的页面 */

	[I(PGACTIVATE)]				= "pgactivate",
	/* 页面激活次数：从非活动 LRU 移到活动 LRU */

	[I(PGDEACTIVATE)]			= "pgdeactivate",
	/* 页面去活次数：从活动 LRU 移到非活动 LRU */

	[I(PGLAZYFREE)]				= "pglazyfree",
	/* 延迟释放次数：通过 madvise(MADV_FREE) 延迟释放 */

	/* ===== 页面错误统计 ===== */
	[I(PGFAULT)]				= "pgfault",
	/* 次要页面错误：不需要磁盘 I/O 的缺页中断（如 COW）*/

	[I(PGMAJFAULT)]				= "pgmajfault",
	/* 主要页面错误：需要磁盘 I/O 的缺页中断
	 * 高值表示工作集大于物理内存
	 */

	[I(PGLAZYFREED)]			= "pglazyfreed",
	/* 延迟释放完成次数：被标记为延迟释放的页面实际释放 */

	[I(PGREUSE)]				= "pgreuse",
	/* 页面重用次数：MADV_FREE 的页面被重新使用 */

	[I(PGSCAN_DIRECT_THROTTLE)]		= "pgscan_direct_throttle",
	/* 直接回收限流次数：直接回收过于频繁被限流 */

#ifdef CONFIG_NUMA
	/* ===== NUMA Zone 回收统计 ===== */
	[I(PGSCAN_ZONE_RECLAIM_SUCCESS)]	= "zone_reclaim_success",
	/* Zone 回收成功次数 */

	[I(PGSCAN_ZONE_RECLAIM_FAILED)]		= "zone_reclaim_failed",
	/* Zone 回收失败次数 */
#endif

	/* ===== Inode 回收统计 ===== */
	[I(PGINODESTEAL)]			= "pginodesteal",
	/* 直接回收 inode：直接回收路径中回收的 inode 数 */

	[I(SLABS_SCANNED)]			= "slabs_scanned",
	/* 扫描的 slab 数：回收期间扫描的 slab 数量 */

	[I(KSWAPD_INODESTEAL)]			= "kswapd_inodesteal",
	/* kswapd 回收 inode：kswapd 回收的 inode 数 */

	/* ===== kswapd 水位统计 ===== */
	[I(KSWAPD_LOW_WMARK_HIT_QUICKLY)]	= "kswapd_low_wmark_hit_quickly",
	/* 快速达到低水位：kswapd 快速将内存恢复到低水位以上 */

	[I(KSWAPD_HIGH_WMARK_HIT_QUICKLY)]	= "kswapd_high_wmark_hit_quickly",
	/* 快速达到高水位：kswapd 快速将内存恢复到高水位以上 */

	[I(PAGEOUTRUN)]				= "pageoutrun",
	/* 页面回收运行次数：kswapd 唤醒并开始回收的次数 */

	[I(PGROTATED)]				= "pgrotated",
	/* 页面旋转次数：回写等待时将页面移到 LRU 尾部 */

	/* ===== 内存释放操作统计 ===== */
	[I(DROP_PAGECACHE)]			= "drop_pagecache",
	/* 释放页面缓存次数：通过 /proc/sys/vm/drop_caches 手动释放 */

	[I(DROP_SLAB)]				= "drop_slab",
	/* 释放 slab 次数：通过 drop_caches 释放 slab */

	[I(OOM_KILL)]				= "oom_kill",
	/* OOM 杀死进程次数：内存不足时杀死进程的次数
	 * 高值表示系统内存极度不足
	 */

#ifdef CONFIG_NUMA_BALANCING
	/* ===== NUMA 均衡统计 ===== */
	[I(NUMA_PTE_UPDATES)]			= "numa_pte_updates",
	/* NUMA 页表更新：将 PTE 标记为 NUMA 提示错误
	 * 使下次访问时触发页面迁移决策
	 */

	[I(NUMA_HUGE_PTE_UPDATES)]		= "numa_huge_pte_updates",
	/* NUMA 大页 PTE 更新 */

	[I(NUMA_HINT_FAULTS)]			= "numa_hint_faults",
	/* NUMA 提示错误：由 NUMA PTE 标记触发的错误 */

	[I(NUMA_HINT_FAULTS_LOCAL)]		= "numa_hint_faults_local",
	/* 本地 NUMA 提示错误：发生在本地节点的提示错误 */

	[I(NUMA_PAGE_MIGRATE)]			= "numa_pages_migrated",
	/* NUMA 页面迁移：自动迁移的页面数 */
#endif

#ifdef CONFIG_MIGRATION
	/* ===== 页面迁移统计 ===== */
	[I(PGMIGRATE_SUCCESS)]			= "pgmigrate_success",
	/* 迁移成功：成功迁移的页面数 */

	[I(PGMIGRATE_FAIL)]			= "pgmigrate_fail",
	/* 迁移失败：迁移失败的页面数 */

	[I(THP_MIGRATION_SUCCESS)]		= "thp_migration_success",
	/* 透明大页迁移成功 */

	[I(THP_MIGRATION_FAIL)]			= "thp_migration_fail",
	/* 透明大页迁移失败 */

	[I(THP_MIGRATION_SPLIT)]		= "thp_migration_split",
	/* 透明大页迁移时被拆分 */
#endif

#ifdef CONFIG_COMPACTION
	/* ===== 内存压实统计 ===== */
	[I(COMPACTMIGRATE_SCANNED)]		= "compact_migrate_scanned",
	/* 压实时扫描的可移动页数 */

	[I(COMPACTFREE_SCANNED)]		= "compact_free_scanned",
	/* 压实时扫描的空闲页数 */

	[I(COMPACTISOLATED)]			= "compact_isolated",
	/* 压实时隔离的页数（准备迁移）*/

	[I(COMPACTSTALL)]			= "compact_stall",
	/* 等待内存压实的次数：直接分配路径触发压实 */

	[I(COMPACTFAIL)]			= "compact_fail",
	/* 压实失败次数：压实后分配仍然失败 */

	[I(COMPACTSUCCESS)]			= "compact_success",
	/* 压实成功次数：压实后分配成功 */

	[I(KCOMPACTD_WAKE)]			= "compact_daemon_wake",
	/* kcompactd 唤醒次数：后台压实守护进程被唤醒 */

	[I(KCOMPACTD_MIGRATE_SCANNED)]		= "compact_daemon_migrate_scanned",
	/* kcompactd 扫描的可移动页数 */

	[I(KCOMPACTD_FREE_SCANNED)]		= "compact_daemon_free_scanned",
	/* kcompactd 扫描的空闲页数 */
#endif

#ifdef CONFIG_HUGETLB_PAGE
	/* ===== 大页分配统计 ===== */
	[I(HTLB_BUDDY_PGALLOC)]			= "htlb_buddy_alloc_success",
	/* 大页伙伴系统分配成功 */

	[I(HTLB_BUDDY_PGALLOC_FAIL)]		= "htlb_buddy_alloc_fail",
	/* 大页伙伴系统分配失败 */
#endif

#ifdef CONFIG_CMA
	/* ===== CMA（连续内存分配器）统计 ===== */
	[I(CMA_ALLOC_SUCCESS)]			= "cma_alloc_success",
	/* CMA 分配成功次数 */

	[I(CMA_ALLOC_FAIL)]			= "cma_alloc_fail",
	/* CMA 分配失败次数 */
#endif

	/* ===== 不可回收页面统计 ===== */
	[I(UNEVICTABLE_PGCULLED)]		= "unevictable_pgs_culled",
	/* 被清除的不可回收页：从不可回收 LRU 移除并最终释放 */

	[I(UNEVICTABLE_PGSCANNED)]		= "unevictable_pgs_scanned",
	/* 扫描的不可回收页：内存压力时扫描不可回收 LRU */

	[I(UNEVICTABLE_PGRESCUED)]		= "unevictable_pgs_rescued",
	/* 解救的不可回收页：被移回活动/非活动 LRU */

	[I(UNEVICTABLE_PGMLOCKED)]		= "unevictable_pgs_mlocked",
	/* 被 mlock 锁定的页：通过 mlock() 系统调用锁定 */

	[I(UNEVICTABLE_PGMUNLOCKED)]		= "unevictable_pgs_munlocked",
	/* 被 munlock 解锁的页：通过 munlock() 系统调用解锁 */

	[I(UNEVICTABLE_PGCLEARED)]		= "unevictable_pgs_cleared",
	/* 清除的不可回收页：不可回收标志被清除 */

	[I(UNEVICTABLE_PGSTRANDED)]		= "unevictable_pgs_stranded",
	/* 滞留的不可回收页：无法回收也无法解锁的孤立页 */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	/* ===== 透明大页（THP）统计 ===== */
	[I(THP_FAULT_ALLOC)]			= "thp_fault_alloc",
	/* 缺页时成功分配 THP */

	[I(THP_FAULT_FALLBACK)]			= "thp_fault_fallback",
	/* 缺页时 THP 分配失败，回退到普通页 */

	[I(THP_FAULT_FALLBACK_CHARGE)]		= "thp_fault_fallback_charge",
	/* THP 失败并回退，但因为 memcg 充电失败 */

	[I(THP_COLLAPSE_ALLOC)]			= "thp_collapse_alloc",
	/* khugepaged 成功合并为 THP */

	[I(THP_COLLAPSE_ALLOC_FAILED)]		= "thp_collapse_alloc_failed",
	/* khugepaged 合并失败 */

	[I(THP_FILE_ALLOC)]			= "thp_file_alloc",
	/* 文件 THP 分配成功 */

	[I(THP_FILE_FALLBACK)]			= "thp_file_fallback",
	/* 文件 THP 分配失败，回退到普通页 */

	[I(THP_FILE_FALLBACK_CHARGE)]		= "thp_file_fallback_charge",
	/* 文件 THP 失败（因充电原因）*/

	[I(THP_FILE_MAPPED)]			= "thp_file_mapped",
	/* 映射的文件 THP */

	[I(THP_SPLIT_PAGE)]			= "thp_split_page",
	/* THP 拆分为普通页（成功）*/

	[I(THP_SPLIT_PAGE_FAILED)]		= "thp_split_page_failed",
	/* THP 拆分失败 */

	[I(THP_DEFERRED_SPLIT_PAGE)]		= "thp_deferred_split_page",
	/* 延迟拆分的 THP：加入延迟拆分队列 */

	[I(THP_UNDERUSED_SPLIT_PAGE)]		= "thp_underused_split_page",
	/* 欠用的 THP 被拆分：因部分页未使用而拆分 */

	[I(THP_SPLIT_PMD)]			= "thp_split_pmd",
	/* PMD 级 THP 拆分 */

	[I(THP_SCAN_EXCEED_NONE_PTE)]		= "thp_scan_exceed_none_pte",
	/* 扫描超出空 PTE 限制 */

	[I(THP_SCAN_EXCEED_SWAP_PTE)]		= "thp_scan_exceed_swap_pte",
	/* 扫描超出交换 PTE 限制 */

	[I(THP_SCAN_EXCEED_SHARED_PTE)]		= "thp_scan_exceed_share_pte",
	/* 扫描超出共享 PTE 限制 */
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
	[I(THP_SPLIT_PUD)]			= "thp_split_pud",
	/* PUD 级 THP 拆分 */
#endif
	[I(THP_ZERO_PAGE_ALLOC)]		= "thp_zero_page_alloc",
	/* 零页 THP 分配成功 */

	[I(THP_ZERO_PAGE_ALLOC_FAILED)]		= "thp_zero_page_alloc_failed",
	/* 零页 THP 分配失败 */

	[I(THP_SWPOUT)]				= "thp_swpout",
	/* THP 交换出：整个大页被交换出 */

	[I(THP_SWPOUT_FALLBACK)]		= "thp_swpout_fallback",
	/* THP 交换出失败，回退：大页被拆分再交换 */
#endif

#ifdef CONFIG_BALLOON
	/* ===== 气球驱动统计（虚拟化）===== */
	[I(BALLOON_INFLATE)]			= "balloon_inflate",
	/* 气球充气：虚拟机将内存归还给宿主机 */

	[I(BALLOON_DEFLATE)]			= "balloon_deflate",
	/* 气球放气：虚拟机从宿主机取回内存 */
#ifdef CONFIG_BALLOON_MIGRATION
	[I(BALLOON_MIGRATE)]			= "balloon_migrate",
	/* 气球页面迁移：气球驱动触发的页面迁移 */
#endif /* CONFIG_BALLOON_MIGRATION */
#endif /* CONFIG_BALLOON */

#ifdef CONFIG_DEBUG_TLBFLUSH
	/* ===== TLB 刷新统计（调试用）===== */
	[I(NR_TLB_REMOTE_FLUSH)]		= "nr_tlb_remote_flush",
	/* 远程 TLB 刷新次数：跨 CPU 的 TLB 无效化 */

	[I(NR_TLB_REMOTE_FLUSH_RECEIVED)]	= "nr_tlb_remote_flush_received",
	/* 接收到的远程 TLB 刷新请求次数 */

	[I(NR_TLB_LOCAL_FLUSH_ALL)]		= "nr_tlb_local_flush_all",
	/* 本地全部 TLB 刷新次数（flush 整个 TLB）*/

	[I(NR_TLB_LOCAL_FLUSH_ONE)]		= "nr_tlb_local_flush_one",
	/* 本地单条 TLB 刷新次数（flush 特定地址）*/
#endif /* CONFIG_DEBUG_TLBFLUSH */

#ifdef CONFIG_SWAP
	/* ===== 交换预读统计 ===== */
	[I(SWAP_RA)]				= "swap_ra",
	/* 交换预读次数：预先读入可能需要的交换页 */

	[I(SWAP_RA_HIT)]			= "swap_ra_hit",
	/* 交换预读命中：预读的页被实际访问 */

	[I(SWPIN_ZERO)]				= "swpin_zero",
	/* 零页交换读入：内容全为 0 的交换页（无需实际 I/O）*/

	[I(SWPOUT_ZERO)]			= "swpout_zero",
	/* 零页交换写出 */
#ifdef CONFIG_KSM
	[I(KSM_SWPIN_COPY)]			= "ksm_swpin_copy",
	/* KSM 交换读入复制：KSM 合并页读入时的复制 */
#endif
#endif

#ifdef CONFIG_KSM
	/* ===== KSM（内核同页合并）统计 ===== */
	[I(COW_KSM)]				= "cow_ksm",
	/* KSM 写时复制：写入 KSM 共享页时触发 COW */
#endif

#ifdef CONFIG_ZSWAP
	/* ===== zswap（压缩交换）统计 ===== */
	[I(ZSWPIN)]				= "zswpin",
	/* zswap 读入：从压缩交换缓存读取页面 */

	[I(ZSWPOUT)]				= "zswpout",
	/* zswap 写出：将页面压缩存入 zswap 缓存 */

	[I(ZSWPWB)]				= "zswpwb",
	/* zswap 回写：zswap 中的页面被写回交换设备 */
#endif

#ifdef CONFIG_X86
	/* ===== 直接映射（x86 特有）统计 ===== */
	[I(DIRECT_MAP_LEVEL2_SPLIT)]		= "direct_map_level2_splits",
	/* 直接映射二级（2MB 页）拆分次数 */

	[I(DIRECT_MAP_LEVEL3_SPLIT)]		= "direct_map_level3_splits",
	/* 直接映射三级（1GB 页）拆分次数 */

	[I(DIRECT_MAP_LEVEL2_COLLAPSE)]		= "direct_map_level2_collapses",
	/* 直接映射二级（2MB 页）合并次数 */

	[I(DIRECT_MAP_LEVEL3_COLLAPSE)]		= "direct_map_level3_collapses",
	/* 直接映射三级（1GB 页）合并次数 */
#endif

#ifdef CONFIG_PER_VMA_LOCK_STATS
	/* ===== VMA 锁统计 ===== */
	[I(VMA_LOCK_SUCCESS)]			= "vma_lock_success",
	/* VMA 锁获取成功 */

	[I(VMA_LOCK_ABORT)]			= "vma_lock_abort",
	/* VMA 锁获取中止 */

	[I(VMA_LOCK_RETRY)]			= "vma_lock_retry",
	/* VMA 锁重试 */

	[I(VMA_LOCK_MISS)]			= "vma_lock_miss",
	/* VMA 锁未命中 */
#endif

#ifdef CONFIG_DEBUG_STACK_USAGE
	/* ===== 内核栈使用统计（调试用）===== */
	/* 统计各大小范围的内核栈使用情况
	 * 帮助评估是否需要调整默认栈大小
	 */
	[I(KSTACK_1K)]				= "kstack_1k",
	/* 栈使用 <= 1KB 的线程数 */
#if THREAD_SIZE > 1024
	[I(KSTACK_2K)]				= "kstack_2k",
	/* 栈使用 1K~2KB 的线程数 */
#endif
#if THREAD_SIZE > 2048
	[I(KSTACK_4K)]				= "kstack_4k",
	/* 栈使用 2K~4KB 的线程数 */
#endif
#if THREAD_SIZE > 4096
	[I(KSTACK_8K)]				= "kstack_8k",
	/* 栈使用 4K~8KB 的线程数 */
#endif
#if THREAD_SIZE > 8192
	[I(KSTACK_16K)]				= "kstack_16k",
	/* 栈使用 8K~16KB 的线程数 */
#endif
#if THREAD_SIZE > 16384
	[I(KSTACK_32K)]				= "kstack_32k",
	/* 栈使用 16K~32KB 的线程数 */
#endif
#if THREAD_SIZE > 32768
	[I(KSTACK_64K)]				= "kstack_64k",
	/* 栈使用 32K~64KB 的线程数 */
#endif
#if THREAD_SIZE > 65536
	[I(KSTACK_REST)]			= "kstack_rest",
	/* 栈使用 > 64KB 的线程数（异常情况）*/
#endif
#endif
#undef I
/* 取消索引宏定义 */
#endif /* CONFIG_VM_EVENT_COUNTERS */
};
#endif /* CONFIG_PROC_FS || CONFIG_SYSFS || CONFIG_NUMA || CONFIG_MEMCG */

/*
 * ============================================================================
 * 【/proc 和 sysfs 接口实现】
 *
 * 提供用户空间访问 vmstat 信息的接口。
 * ============================================================================
 */

#if (defined(CONFIG_DEBUG_FS) && defined(CONFIG_COMPACTION)) || \
     defined(CONFIG_PROC_FS)

/*
 * 【seq_file 迭代器函数】
 *
 * 这些函数实现 seq_file 接口，用于遍历所有在线的 NUMA 节点。
 * seq_file 是 Linux 内核中用于实现 /proc 文件的框架。
 */

/*
 * 【函数】frag_start - seq_file 开始迭代
 * @m: seq_file 结构
 * @pos: 位置指针（节点索引）
 * @return: 当前节点的 pgdat，或 NULL
 *
 * 【功能】
 * 根据位置找到对应的在线 NUMA 节点。
 */
static void *frag_start(struct seq_file *m, loff_t *pos)
{
	pg_data_t *pgdat;
	loff_t node = *pos;

	for (pgdat = first_online_pgdat();
	     pgdat && node;
	     pgdat = next_online_pgdat(pgdat))
		--node;
	/* 遍历节点直到达到指定位置 */

	return pgdat;
}

/*
 * 【函数】frag_next - seq_file 下一个元素
 * @m: seq_file 结构
 * @arg: 当前节点的 pgdat
 * @pos: 位置指针（递增）
 * @return: 下一个节点的 pgdat，或 NULL
 */
static void *frag_next(struct seq_file *m, void *arg, loff_t *pos)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	(*pos)++;
	return next_online_pgdat(pgdat);
}

/*
 * 【函数】frag_stop - seq_file 结束迭代
 *
 * 【功能】
 * 清理资源（此处无需清理）。
 */
static void frag_stop(struct seq_file *m, void *arg)
{
}

/*
 * Walk zones in a node and print using a callback.
 * If @assert_populated is true, only use callback for zones that are populated.
 */
/*
 * 【函数】walk_zones_in_node - 遍历节点中的所有 zone 并打印
 * @m: seq_file 结构
 * @pgdat: NUMA 节点
 * @assert_populated: 如果为 true，只处理已填充的 zone
 * @nolock: 如果为 true，不加锁
 * @print: 打印回调函数
 *
 * 【功能】
 * 遍历节点中的所有 zone，并对每个 zone 调用打印回调。
 *
 * 【锁保护】
 * 如果 nolock 为 false，使用 zone->lock 保护 zone 数据。
 * 这防止在读取期间 zone 数据被修改。
 *
 * 【使用场景】
 * - /proc/buddyinfo: 显示伙伴系统空闲块信息
 * - /proc/pagetypeinfo: 显示页面类型信息
 * - debugfs 文件：显示内存碎片信息
 */
static void walk_zones_in_node(struct seq_file *m, pg_data_t *pgdat,
		bool assert_populated, bool nolock,
		void (*print)(struct seq_file *m, pg_data_t *, struct zone *))
{
	struct zone *zone;
	struct zone *node_zones = pgdat->node_zones;
	unsigned long flags;

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		/* 遍历节点的所有 zone */

		if (assert_populated && !populated_zone(zone))
			continue;
		/* 如果要求只处理已填充的 zone，跳过空 zone */

		if (!nolock)
			spin_lock_irqsave(&zone->lock, flags);
		/* 加锁保护 zone 数据（如果需要）
		 * spin_lock_irqsave: 自旋锁 + 禁用中断
		 */

		print(m, pgdat, zone);
		/* 调用打印回调函数 */

		if (!nolock)
			spin_unlock_irqrestore(&zone->lock, flags);
		/* 解锁并恢复中断 */
	}
}
#endif

#ifdef CONFIG_PROC_FS

/*
 * ============================================================================
 * 【/proc/buddyinfo 实现】
 *
 * 显示伙伴系统中每个阶数的空闲块数量。
 * ============================================================================
 */

/*
 * 【函数】frag_show_print - 打印单个 zone 的碎片信息
 * @m: seq_file 结构
 * @pgdat: NUMA 节点
 * @zone: 内存区域
 *
 * 【输出格式】
 * Node X, zone ZONE_NAME  count0  count1  count2  ...  countN
 *
 * 其中 countN 是阶数 N 的空闲块数量。
 *
 * 【示例】
 * Node 0, zone   Normal  1234  567  89  12  3  1  0  0  0  0  0
 * 表示：
 * - 阶数 0 (4KB): 1234 个块
 * - 阶数 1 (8KB): 567 个块
 * - 阶数 2 (16KB): 89 个块
 * - ...
 */
static void frag_show_print(struct seq_file *m, pg_data_t *pgdat,
						struct zone *zone)
{
	int order;

	seq_printf(m, "Node %d, zone %8s ", pgdat->node_id, zone->name);
	/* 打印节点 ID 和 zone 名称 */

	for (order = 0; order < NR_PAGE_ORDERS; ++order)
		/*
		 * Access to nr_free is lockless as nr_free is used only for
		 * printing purposes. Use data_race to avoid KCSAN warning.
		 */
		/*
		 * 对 nr_free 的访问是无锁的，因为 nr_free 仅用于打印目的。
		 * 使用 data_race 避免 KCSAN（Kernel Concurrency Sanitizer）警告。
		 *
		 * 【为什么可以无锁】
		 * 这些值仅用于诊断和监控，不需要绝对精确。
		 * 即使读到略微过时的值，也不会影响系统正确性。
		 */
		seq_printf(m, "%6lu ", data_race(zone->free_area[order].nr_free));
	/* 打印每个阶数的空闲块数量 */

	seq_putc(m, '\n');
}

/*
 * This walks the free areas for each zone.
 */
/*
 * 【函数】frag_show - /proc/buddyinfo 的 show 函数
 * @m: seq_file 结构
 * @arg: 当前节点的 pgdat
 * @return: 0
 *
 * 【功能】
 * 遍历节点中的所有 zone，显示空闲区域信息。
 *
 * 【对应文件】
 * /proc/buddyinfo
 *
 * 【用途】
 * 查看伙伴系统的碎片化情况。
 * 如果高阶数的空闲块很少，说明内存碎片化严重。
 */
static int frag_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;
	walk_zones_in_node(m, pgdat, true, false, frag_show_print);
	/* assert_populated=true: 只显示已填充的 zone
	 * nolock=false: 需要加锁
	 */
	return 0;
}

/*
 * ============================================================================
 * 【/proc/pagetypeinfo 实现】
 *
 * 显示不同迁移类型的页面块统计信息。
 * ============================================================================
 */

/*
 * 【函数】pagetypeinfo_showfree_print - 打印页面类型的空闲信息
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
/*
 * 【函数】pagetypeinfo_showfree - 显示每个迁移类型在每个阶数的空闲页面
 * @m: seq_file 结构
 * @arg: 当前节点的 pgdat
 *
 * 【输出格式】
 * 标题行：Free pages count per migrate type at order   0    1    2  ...
 * 数据行：Node X, zone ZONE_NAME  Unmovable Movable Reclaimable ...
 *
 * 【用途】
 * 分析不同迁移类型的页面分布，帮助诊断碎片化问题。
 */
static void pagetypeinfo_showfree(struct seq_file *m, void *arg)
{
	int order;
	pg_data_t *pgdat = (pg_data_t *)arg;

	/* Print header */
	/* 打印标题 */
	seq_printf(m, "%-43s ", "Free pages count per migrate type at order");
	for (order = 0; order < NR_PAGE_ORDERS; ++order)
		seq_printf(m, "%6d ", order);
	seq_putc(m, '\n');

	walk_zones_in_node(m, pgdat, true, false, pagetypeinfo_showfree_print);
}

/*
 * 【函数】pagetypeinfo_showblockcount_print - 打印页面块计数
 * @m: seq_file 结构
 * @pgdat: NUMA 节点
 * @zone: 内存区域
 *
 * 【功能】
 * 统计每个迁移类型有多少个页面块（pageblock）。
 *
 * 【页面块（pageblock）】
 * 页面块是内存管理的基本单位，大小通常为 2MB（x86-64）。
 * 每个页面块有一个迁移类型：
 * - MIGRATE_UNMOVABLE: 不可移动
 * - MIGRATE_MOVABLE: 可移动
 * - MIGRATE_RECLAIMABLE: 可回收
 * - 等等...
 */
static void pagetypeinfo_showblockcount_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	int mtype;
	unsigned long pfn;
	unsigned long start_pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count[MIGRATE_TYPES] = { 0, };

	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		/* 以页面块为单位遍历 zone */

		struct page *page;

		page = pfn_to_online_page(pfn);
		if (!page)
			continue;
		/* 跳过不在线的页面 */

		if (page_zone(page) != zone)
			continue;
		/* 跳过不属于此 zone 的页面
		 * （可能由于内存热插拔等原因）
		 */

		mtype = get_pageblock_migratetype(page);
		/* 获取该页面块的迁移类型 */

		if (mtype < MIGRATE_TYPES)
			count[mtype]++;
		/* 统计每种迁移类型的页面块数 */
	}

	/* Print counts */
	/* 打印统计结果 */
	seq_printf(m, "Node %d, zone %8s ", pgdat->node_id, zone->name);
	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++)
		seq_printf(m, "%12lu ", count[mtype]);
	seq_putc(m, '\n');
}

/* Print out the number of pageblocks for each migratetype */
/*
 * 【函数】pagetypeinfo_showblockcount - 显示每个迁移类型的页面块数量
 * @m: seq_file 结构
 * @arg: 当前节点的 pgdat
 */
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
/*
 * 【函数】pagetypeinfo_showmixedcount - 显示混合页面块的数量
 * @m: seq_file 结构
 * @pgdat: NUMA 节点
 *
 * 【功能】
 * 打印每个迁移类型中包含其他类型页面的页面块数量。
 * 这可以指示 rmqueue_fallback() 对回退的控制效果如何。
 *
 * 【混合页面块】
 * 当某种迁移类型的页面不足时，会从其他类型借用（fallback）。
 * 这会导致页面块包含多种迁移类型的页面，称为"混合"。
 * 混合会导致碎片化增加。
 *
 * 【依赖】
 * 需要 CONFIG_PAGE_OWNER 来跟踪页面所有者信息。
 */
static void pagetypeinfo_showmixedcount(struct seq_file *m, pg_data_t *pgdat)
{
#ifdef CONFIG_PAGE_OWNER
	int mtype;

	if (!static_branch_unlikely(&page_owner_inited))
		return;
	/* 如果 page_owner 未初始化，返回 */

	drain_all_pages(NULL);
	/* 排空所有 per-CPU 页面缓存
	 * 确保统计数据准确
	 */

	seq_printf(m, "\n%-23s", "Number of mixed blocks ");
	for (mtype = 0; mtype < MIGRATE_TYPES; mtype++)
		seq_printf(m, "%12s ", migratetype_names[mtype]);
	seq_putc(m, '\n');

	walk_zones_in_node(m, pgdat, true, true,
		pagetypeinfo_showmixedcount_print);
	/* nolock=true: 不加锁（因为已经 drain_all_pages） */
#endif /* CONFIG_PAGE_OWNER */
}

/*
 * This prints out statistics in relation to grouping pages by mobility.
 * It is expensive to collect so do not constantly read the file.
 */
/*
 * 【函数】pagetypeinfo_show - /proc/pagetypeinfo 的 show 函数
 * @m: seq_file 结构
 * @arg: 当前节点的 pgdat
 * @return: 0
 *
 * 【功能】
 * 打印与按移动性分组页面相关的统计信息。
 *
 * 【性能警告】
 * 收集这些信息很昂贵，所以不要频繁读取此文件。
 * 特别是 showmixedcount 需要遍历所有页面块。
 *
 * 【对应文件】
 * /proc/pagetypeinfo
 *
 * 【输出内容】
 * 1. 页面块阶数和大小
 * 2. 每个迁移类型在每个阶数的空闲页面数
 * 3. 每个迁移类型的页面块数量
 * 4. 每个迁移类型的混合页面块数量（需要 PAGE_OWNER）
 */
static int pagetypeinfo_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	/* check memoryless node */
	/* 检查无内存节点 */
	if (!node_state(pgdat->node_id, N_MEMORY))
		return 0;
	/* 某些 NUMA 节点可能没有内存（只有 CPU） */

	seq_printf(m, "Page block order: %d\n", pageblock_order);
	seq_printf(m, "Pages per block:  %lu\n", pageblock_nr_pages);
	seq_putc(m, '\n');
	pagetypeinfo_showfree(m, pgdat);
	pagetypeinfo_showblockcount(m, pgdat);
	pagetypeinfo_showmixedcount(m, pgdat);

	return 0;
}

/*
 * 【结构】seq_operations - seq_file 操作集
 *
 * 定义 /proc 文件的迭代器操作。
 */

static const struct seq_operations fragmentation_op = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= frag_show,
};
/* /proc/buddyinfo 的操作集 */

static const struct seq_operations pagetypeinfo_op = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= pagetypeinfo_show,
};
/* /proc/pagetypeinfo 的操作集 */

/*
 * ============================================================================
 * 【/proc/zoneinfo 实现】
 *
 * 显示详细的 zone 信息，包括统计、水位线、per-CPU 页面集等。
 * ============================================================================
 */

/*
 * 【函数】is_zone_first_populated - 检查是否为节点的第一个已填充 zone
 * @pgdat: NUMA 节点
 * @zone: 要检查的 zone
 * @return: 如果是第一个已填充的 zone 返回 true
 *
 * 【用途】
 * 避免重复打印 per-node 统计信息。
 * 只在第一个 zone 时打印节点级别的统计。
 */
static bool is_zone_first_populated(pg_data_t *pgdat, struct zone *zone)
{
	int zid;

	for (zid = 0; zid < MAX_NR_ZONES; zid++) {
		struct zone *compare = &pgdat->node_zones[zid];

		if (populated_zone(compare))
			return zone == compare;
		/* 返回第一个已填充的 zone 是否是当前 zone */
	}

	return false;
}

/*
 * 【函数】zoneinfo_show_print - 打印单个 zone 的详细信息
 * @m: seq_file 结构
 * @pgdat: NUMA 节点
 * @zone: 内存区域
 *
 * 【输出内容】
 * 1. 节点 ID 和 zone 名称
 * 2. Per-node 统计（仅第一个 zone）
 * 3. 页面统计：free, boost, min, low, high, promo
 * 4. 内存范围：spanned, present, managed, cma
 * 5. lowmem_reserve 保护值
 * 6. Zone 统计项
 * 7. NUMA 事件统计
 * 8. Per-CPU pageset 信息
 * 9. 其他杂项信息
 */
static void zoneinfo_show_print(struct seq_file *m, pg_data_t *pgdat,
							struct zone *zone)
{
	int i;
	seq_printf(m, "Node %d, zone %8s", pgdat->node_id, zone->name);

	if (is_zone_first_populated(pgdat, zone)) {
		/* 如果是第一个已填充的 zone，打印 per-node 统计 */

		seq_printf(m, "\n  per-node stats");
		for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++) {
			unsigned long pages = node_page_state_pages(pgdat, i);

			if (vmstat_item_print_in_thp(i))
				pages /= HPAGE_PMD_NR;
			/* 对于透明大页统计，转换为大页数量 */

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
	/*
	 * 【水位线说明】
	 * - min: 最小水位线，低于此值触发直接回收
	 * - low: 低水位线，低于此值唤醒 kswapd
	 * - high: 高水位线，高于此值 kswapd 停止
	 * - boost: 临时提升值，用于快速恢复
	 * - promo: 提升水位线（用于内存分层）
	 *
	 * 【内存范围说明】
	 * - spanned: zone 覆盖的总页面数（包括空洞）
	 * - present: 实际存在的页面数
	 * - managed: 伙伴系统管理的页面数
	 * - cma: CMA（Contiguous Memory Allocator）页面数
	 */

	seq_printf(m,
		   "\n        protection: (%ld",
		   zone->lowmem_reserve[0]);
	for (i = 1; i < ARRAY_SIZE(zone->lowmem_reserve); i++)
		seq_printf(m, ", %ld", zone->lowmem_reserve[i]);
	seq_putc(m, ')');
	/*
	 * 【lowmem_reserve】
	 * 低内存保护：保留一定页面供更高优先级的 zone 使用。
	 * 防止高 zone（如 NORMAL）的分配耗尽低 zone（如 DMA）的内存。
	 */

	/* If unpopulated, no other information is useful */
	/* 如果 zone 未填充，其他信息无用 */
	if (!populated_zone(zone)) {
		seq_putc(m, '\n');
		return;
	}

	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++)
		seq_printf(m, "\n      %-12s %lu", zone_stat_name(i),
			   zone_page_state(zone, i));
	/* 打印所有 zone 统计项 */

#ifdef CONFIG_NUMA
	fold_vm_zone_numa_events(zone);
	/* 折叠 NUMA 事件到全局（确保最新） */

	for (i = 0; i < NR_VM_NUMA_EVENT_ITEMS; i++)
		seq_printf(m, "\n      %-12s %lu", numa_stat_name(i),
			   zone_numa_event_state(zone, i));
#endif

	seq_printf(m, "\n  pagesets");
	for_each_online_cpu(i) {
		/* 遍历所有在线 CPU，打印其 per-CPU pageset */

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
		/*
		 * 【Per-CPU Pageset】
		 * - count: 当前缓存的页面数
		 * - high: 高水位，超过此值将页面返回伙伴系统
		 * - batch: 批量分配/释放的页面数
		 * - high_min/high_max: 高水位的动态范围
		 */

#ifdef CONFIG_SMP
		pzstats = per_cpu_ptr(zone->per_cpu_zonestats, i);
		seq_printf(m, "\n  vm stats threshold: %d",
				pzstats->stat_threshold);
		/* 打印该 CPU 的统计阈值 */
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
	/*
	 * 【其他信息】
	 * - node_unreclaimable: 节点是否无法回收
	 * - start_pfn: zone 起始页帧号
	 * - reserved_highatomic: 为高优先级原子分配保留的页面
	 * - free_highatomic: 高优先级原子分配的空闲页面
	 */

	seq_putc(m, '\n');
}

/*
 * Output information about zones in @pgdat.  All zones are printed regardless
 * of whether they are populated or not: lowmem_reserve_ratio operates on the
 * set of all zones and userspace would not be aware of such zones if they are
 * suppressed here (zoneinfo displays the effect of lowmem_reserve_ratio).
 */
/*
 * 【函数】zoneinfo_show - /proc/zoneinfo 的 show 函数
 * @m: seq_file 结构
 * @arg: 当前节点的 pgdat
 * @return: 0
 *
 * 【功能】
 * 输出 @pgdat 中所有 zone 的信息。
 * 无论 zone 是否已填充，都会打印所有 zone：
 * lowmem_reserve_ratio 对所有 zone 集合操作，
 * 如果在此处抑制某些 zone，用户空间将不知道这些 zone
 * （zoneinfo 显示 lowmem_reserve_ratio 的效果）。
 *
 * 【对应文件】
 * /proc/zoneinfo
 */
static int zoneinfo_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;
	walk_zones_in_node(m, pgdat, false, false, zoneinfo_show_print);
	/* assert_populated=false: 显示所有 zone */
	return 0;
}

static const struct seq_operations zoneinfo_op = {
	.start	= frag_start, /* iterate over all zones. The same as in
			       * fragmentation. */
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= zoneinfo_show,
};
/* /proc/zoneinfo 的操作集 */

/*
 * ============================================================================
 * 【/proc/vmstat 实现】
 *
 * 显示全局虚拟内存统计信息。
 * ============================================================================
 */

/*
 * 【宏】NR_VMSTAT_ITEMS - 所有 vmstat 统计项的总数
 *
 * 包括：
 * - Zone 统计项
 * - NUMA 事件统计项
 * - Node 统计项
 * - VM 统计项（如 dirty limits）
 * - VM 事件计数器（如果启用）
 */
#define NR_VMSTAT_ITEMS (NR_VM_ZONE_STAT_ITEMS + \
			 NR_VM_NUMA_EVENT_ITEMS + \
			 NR_VM_NODE_STAT_ITEMS + \
			 NR_VM_STAT_ITEMS + \
			 (IS_ENABLED(CONFIG_VM_EVENT_COUNTERS) ? \
			  NR_VM_EVENT_ITEMS : 0))

/*
 * 【函数】vmstat_start - /proc/vmstat 的 start 函数
 * @m: seq_file 结构
 * @pos: 位置指针
 * @return: 统计数组指针，或 NULL/ERR_PTR
 *
 * 【功能】
 * 收集所有全局 vmstat 统计信息到一个数组中。
 *
 * 【工作流程】
 * 1. 分配数组存储所有统计项
 * 2. 收集 zone 统计
 * 3. 收集 NUMA 事件统计
 * 4. 收集 node 统计
 * 5. 收集 VM 统计（dirty limits, memmap pages）
 * 6. 收集 VM 事件计数器
 *
 * 【性能考虑】
 * 此函数会遍历所有 CPU 和 zone 收集统计，
 * 在大型系统上可能较慢。
 */
static void *vmstat_start(struct seq_file *m, loff_t *pos)
{
	unsigned long *v;
	int i;

	if (*pos >= NR_VMSTAT_ITEMS)
		return NULL;

	BUILD_BUG_ON(ARRAY_SIZE(vmstat_text) != NR_VMSTAT_ITEMS);
	/* 编译时检查：vmstat_text 数组大小必须匹配 */

	fold_vm_numa_events();
	/* 折叠所有 NUMA 事件到全局 */

	v = kmalloc_array(NR_VMSTAT_ITEMS, sizeof(unsigned long), GFP_KERNEL);
	m->private = v;
	if (!v)
		return ERR_PTR(-ENOMEM);

	/* 收集 zone 统计 */
	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++)
		v[i] = global_zone_page_state(i);
	v += NR_VM_ZONE_STAT_ITEMS;

#ifdef CONFIG_NUMA
	/* 收集 NUMA 事件统计 */
	for (i = 0; i < NR_VM_NUMA_EVENT_ITEMS; i++)
		v[i] = global_numa_event_state(i);
	v += NR_VM_NUMA_EVENT_ITEMS;
#endif

	/* 收集 node 统计 */
	for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++) {
		v[i] = global_node_page_state_pages(i);
		if (vmstat_item_print_in_thp(i))
			v[i] /= HPAGE_PMD_NR;
		/* 透明大页项转换为大页数 */
	}
	v += NR_VM_NODE_STAT_ITEMS;

	/* 收集 VM 统计 */
	global_dirty_limits(v + NR_DIRTY_BG_THRESHOLD,
			    v + NR_DIRTY_THRESHOLD);
	/* 计算脏页阈值 */

	v[NR_MEMMAP_PAGES] = atomic_long_read(&nr_memmap_pages);
	v[NR_MEMMAP_BOOT_PAGES] = atomic_long_read(&nr_memmap_boot_pages);
	/* 内存映射页面统计 */

	v += NR_VM_STAT_ITEMS;

#ifdef CONFIG_VM_EVENT_COUNTERS
	/* 收集 VM 事件计数器 */
	all_vm_events(v);
	v[PGPGIN] /= 2;		/* sectors -> kbytes */
	v[PGPGOUT] /= 2;
	/* 将扇区转换为 KB */
#endif
	return (unsigned long *)m->private + *pos;
}

/*
 * 【函数】vmstat_next - /proc/vmstat 的 next 函数
 */
static void *vmstat_next(struct seq_file *m, void *arg, loff_t *pos)
{
	(*pos)++;
	if (*pos >= NR_VMSTAT_ITEMS)
		return NULL;
	return (unsigned long *)m->private + *pos;
}

/*
 * 【函数】vmstat_show - /proc/vmstat 的 show 函数
 * @m: seq_file 结构
 * @arg: 当前统计项指针
 * @return: 0
 *
 * 【功能】
 * 打印单个统计项的名称和值。
 *
 * 【输出格式】
 * stat_name value
 *
 * 【示例】
 * nr_free_pages 123456
 * pgfault 987654
 */
static int vmstat_show(struct seq_file *m, void *arg)
{
	unsigned long *l = arg;
	unsigned long off = l - (unsigned long *)m->private;

	seq_puts(m, vmstat_text[off]);
	seq_put_decimal_ull(m, " ", *l);
	seq_putc(m, '\n');
	/* 打印：统计项名称 空格 值 换行 */

	if (off == NR_VMSTAT_ITEMS - 1) {
		/*
		 * We've come to the end - add any deprecated counters to avoid
		 * breaking userspace which might depend on them being present.
		 */
		/*
		 * 我们已到达末尾 - 添加任何已废弃的计数器，
		 * 以避免破坏可能依赖它们存在的用户空间程序。
		 *
		 * 【向后兼容性】
		 * nr_unstable 是一个已废弃的计数器，但为了兼容性保留。
		 */
		seq_puts(m, "nr_unstable 0\n");
	}
	return 0;
}

/*
 * 【函数】vmstat_stop - /proc/vmstat 的 stop 函数
 * @m: seq_file 结构
 * @arg: 参数（未使用）
 *
 * 【功能】
 * 释放在 vmstat_start 中分配的内存。
 */
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
/* /proc/vmstat 的操作集 */

#endif /* CONFIG_PROC_FS */

/*
 * ============================================================================
 * 【定期统计刷新机制】
 *
 * 使用延迟工作队列定期刷新 vmstat 统计信息。
 * ============================================================================
 */

#ifdef CONFIG_SMP
static DEFINE_PER_CPU(struct delayed_work, vmstat_work);
/* 每个 CPU 的延迟工作，用于定期刷新统计 */

static int sysctl_stat_interval __read_mostly = HZ;
/* 统计刷新间隔，默认 1 秒（HZ 是每秒的时钟滴答数）
 * 可通过 /proc/sys/vm/stat_interval 调整
 */

static int vmstat_late_init_done;
/* 标记 vmstat 后期初始化是否完成 */

#ifdef CONFIG_PROC_FS
/*
 * 【函数】refresh_vm_stats - 工作队列回调函数
 * @work: 工作结构
 *
 * 【功能】
 * 定期刷新 VM 统计信息。
 */
static void refresh_vm_stats(struct work_struct *work)
{
	refresh_cpu_vm_stats(true);
	/* 刷新当前 CPU 的 VM 统计
	 * do_pagesets=true: 同时处理 pageset 操作
	 */
}

/*
 * 【函数】vmstat_refresh - /proc/sys/vm/stat_refresh 的处理函数
 * @table: sysctl 表项
 * @write: 是否为写操作
 * @buffer: 用户缓冲区
 * @lenp: 长度指针
 * @ppos: 位置指针
 * @return: 0 表示成功，负数表示错误
 *
 * 【功能】
 * 立即刷新所有 CPU 的 VM 统计，并检查统计项是否有负值。
 *
 * 【使用场景】
 * 在读取统计信息之前更新它们，特别是在测试后立即检查时。
 * 可以通过以下方式触发：
 * - echo 1 > /proc/sys/vm/stat_refresh
 * - cat /proc/sys/vm/stat_refresh
 *
 * 【为什么需要这个】
 * 常规更新（每 sysctl_stat_interval）可能晚于预期，
 * 在 per-CPU 桶中留下大量未同步的值。
 * 这在运行测试后立即检查大量页面（如 HUGE pages）时特别具有误导性。
 *
 * 【负值检测】
 * global_zone_page_state() 等函数会隐藏瞬时的负值，
 * 但如果有统计项为负，这里会报告错误，
 * 以便我们知道需要查找不平衡问题。
 */
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
	/*
	 * 常规更新（每 sysctl_stat_interval）可能晚于预期：
	 * 在 per-CPU 桶中留下大量值。
	 * 这在运行测试后立即检查大量页面时特别具有误导性。
	 * /proc/sys/vm/stat_refresh（root 可以 echo 或 cat）
	 * 可用于在读取统计之前更新它们。
	 *
	 * 由于 global_zone_page_state() 等函数非常小心地隐藏
	 * 瞬时负值，如果任何统计项为负，这里报告错误，
	 * 以便我们知道需要查找不平衡。
	 */

	err = schedule_on_each_cpu(refresh_vm_stats);
	/* 在每个 CPU 上调度刷新工作
	 * 这会立即在所有 CPU 上同步统计
	 */

	if (err)
		return err;

	for (i = 0; i < NR_VM_ZONE_STAT_ITEMS; i++) {
		/* 检查所有 zone 统计项 */

		/*
		 * Skip checking stats known to go negative occasionally.
		 */
		/*
		 * 跳过已知偶尔会变负的统计项。
		 */
		switch (i) {
		case NR_ZONE_WRITE_PENDING:
		case NR_FREE_CMA_PAGES:
			continue;
		/* 这些统计项已知可能暂时为负，跳过检查 */
		}

		val = atomic_long_read(&vm_zone_stat[i]);
		if (val < 0) {
			pr_warn("%s: %s %ld\n",
				__func__, zone_stat_name(i), val);
			/* 警告：发现负值，可能存在统计不平衡 */
		}
	}

	for (i = 0; i < NR_VM_NODE_STAT_ITEMS; i++) {
		/* 检查所有 node 统计项 */

		/*
		 * Skip checking stats known to go negative occasionally.
		 */
		/*
		 * 跳过已知偶尔会变负的统计项。
		 */
		switch (i) {
		case NR_WRITEBACK:
			continue;
		/* NR_WRITEBACK 可能暂时为负 */
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
	/* 处理 sysctl 读写操作的位置更新 */

	return 0;
}
#endif /* CONFIG_PROC_FS */

/*
 * 【函数】vmstat_update - 定期更新工作函数
 * @w: 工作结构
 *
 * 【功能】
 * 定期刷新 VM 统计，如果有更新则重新调度自己。
 *
 * 【自适应调度】
 * 如果 refresh_cpu_vm_stats() 返回 true（有计数器更新），
 * 说明系统活跃，继续调度更新工作。
 * 如果返回 false（无更新），不再调度，节省 CPU 资源。
 *
 * 【round_jiffies_relative】
 * 将超时时间舍入到最近的整秒边界，
 * 允许多个定时器在同一时刻唤醒，提高电源效率。
 */
static void vmstat_update(struct work_struct *w)
{
	if (refresh_cpu_vm_stats(true)) {
		/* 如果有计数器被更新 */

		/*
		 * Counters were updated so we expect more updates
		 * to occur in the future. Keep on running the
		 * update worker thread.
		 */
		/*
		 * 计数器已更新，我们预期未来会有更多更新。
		 * 继续运行更新工作线程。
		 */
		queue_delayed_work_on(smp_processor_id(), mm_percpu_wq,
				this_cpu_ptr(&vmstat_work),
				round_jiffies_relative(sysctl_stat_interval));
		/* 在当前 CPU 上重新调度延迟工作
		 * mm_percpu_wq: 内存管理的 per-CPU 工作队列
		 * sysctl_stat_interval: 刷新间隔（默认 1 秒）
		 */
	}
	/* 如果无更新，不再调度，让工作队列停止 */
}

/*
 * Check if the diffs for a certain cpu indicate that
 * an update is needed.
 */
/*
 * 【函数】need_update - 检查指定 CPU 是否需要更新统计
 * @cpu: CPU 编号
 * @return: true 表示需要更新，false 表示不需要
 *
 * 【功能】
 * 检查 CPU 的差分计数器是否有非零值。
 *
 * 【快速检查】
 * 使用 memchr_inv 快速检查整个数组是否全为 0。
 * 比逐个检查每个统计项更高效。
 *
 * 【优化】
 * 对于同一 NUMA 节点的多个 zone，只检查第一个 zone 的 node 统计。
 */
static bool need_update(int cpu)
{
	pg_data_t *last_pgdat = NULL;
	struct zone *zone;

	for_each_populated_zone(zone) {
		/* 遍历所有已填充的 zone */

		struct per_cpu_zonestat *pzstats = per_cpu_ptr(zone->per_cpu_zonestats, cpu);
		struct per_cpu_nodestat *n;

		/*
		 * The fast way of checking if there are any vmstat diffs.
		 */
		/*
		 * 快速检查是否有任何 vmstat 差分。
		 */
		if (memchr_inv(pzstats->vm_stat_diff, 0, sizeof(pzstats->vm_stat_diff)))
			return true;
		/* memchr_inv: 检查内存区域是否包含非指定值的字节
		 * 如果差分数组中有任何非零字节，返回 true
		 */

		if (last_pgdat == zone->zone_pgdat)
			continue;
		/* 如果已经检查过这个节点，跳过
		 * （避免重复检查同一节点的 node 统计）
		 */

		last_pgdat = zone->zone_pgdat;
		n = per_cpu_ptr(zone->zone_pgdat->per_cpu_nodestats, cpu);
		if (memchr_inv(n->vm_node_stat_diff, 0, sizeof(n->vm_node_stat_diff)))
			return true;
		/* 检查节点的差分计数器 */
	}
	return false;
}

/*
 * Switch off vmstat processing and then fold all the remaining differentials
 * until the diffs stay at zero. The function is used by NOHZ and can only be
 * invoked when tick processing is not active.
 */
/*
 * 【函数】quiet_vmstat - 关闭 vmstat 处理并折叠剩余差分
 *
 * 【功能】
 * 关闭 vmstat 处理，然后折叠所有剩余的差分，直到差分保持为零。
 *
 * 【使用场景】
 * 此函数由 NOHZ（NO_HZ，无时钟滴答）使用，
 * 只能在时钟滴答处理不活动时调用。
 *
 * 【NOHZ 模式】
 * 在空闲 CPU 上停止时钟滴答以节省电源。
 * 在进入 NOHZ 模式前，需要清理所有待处理的统计更新。
 *
 * 【为什么不取消延迟工作】
 * 只刷新计数器，不关心待处理的延迟 vmstat_update。
 * 它不会频繁触发，从此路径取消它会太昂贵。
 * vmstat_shepherd 会为我们处理这个。
 */
void quiet_vmstat(void)
{
	if (system_state != SYSTEM_RUNNING)
		return;
	/* 系统未运行，无需处理 */

	if (!delayed_work_pending(this_cpu_ptr(&vmstat_work)))
		return;
	/* 如果没有待处理的延迟工作，无需处理 */

	if (!need_update(smp_processor_id()))
		return;
	/* 如果不需要更新，无需处理 */

	/*
	 * Just refresh counters and do not care about the pending delayed
	 * vmstat_update. It doesn't fire that often to matter and canceling
	 * it would be too expensive from this path.
	 * vmstat_shepherd will take care about that for us.
	 */
	/*
	 * 只刷新计数器，不关心待处理的延迟 vmstat_update。
	 * 它不会频繁触发，从此路径取消它会太昂贵。
	 * vmstat_shepherd 会为我们处理这个。
	 */
	refresh_cpu_vm_stats(false);
	/* do_pagesets=false: 不处理 pageset 操作（更快） */
}

/*
 * Shepherd worker thread that checks the
 * differentials of processors that have their worker
 * threads for vm statistics updates disabled because of
 * inactivity.
 */
/*
 * 【函数声明】vmstat_shepherd - 牧羊人工作线程
 *
 * 检查那些因不活跃而禁用了 VM 统计更新工作线程的处理器的差分。
 */
static void vmstat_shepherd(struct work_struct *w);

static DECLARE_DEFERRABLE_WORK(shepherd, vmstat_shepherd);
/* 声明可延迟的工作：牧羊人工作
 * DEFERRABLE: 可以延迟到 CPU 唤醒时执行，节省电源
 */

/*
 * vmstat_flush_workqueue() - 排空承载 per-CPU vmstat 更新的专用 workqueue。
 *
 * 入参：无；返回：无直接返回值。flush_workqueue() 可能睡眠，返回时调用前排入
 * mm_percpu_wq 的 work 已完成，但 deferrable shepherd 或返回后新入队工作不因
 * 此永久停止。housekeeping_update() 用它划清旧 unbound affinity 工作与新
 * DOMAIN 掩码传播的边界，避免 vmstat worker 在 pool 重配期间仍依赖旧目标。
 */
/*
 * 【函数】vmstat_flush_workqueue - 刷新 vmstat 工作队列
 *
 * 【功能】
 * 排空承载 per-CPU vmstat 更新的专用工作队列。
 *
 * 【行为】
 * - flush_workqueue() 可能睡眠
 * - 返回时，调用前排入 mm_percpu_wq 的工作已完成
 * - 但可延迟的 shepherd 或返回后新入队的工作不会因此永久停止
 *
 * 【使用场景】
 * housekeeping_update() 使用它来划清旧的 unbound affinity 工作
 * 与新 DOMAIN 掩码传播的边界，避免 vmstat worker 在 pool 重配期间
 * 仍依赖旧目标。
 *
 * 【注意事项】
 * - 这是一个同步操作，会等待所有待处理的工作完成
 * - 不影响可延迟的 shepherd 工作
 * - 不会阻止新工作在返回后入队
 */
void vmstat_flush_workqueue(void)
{
	flush_workqueue(mm_percpu_wq);
	/* 刷新内存管理的 per-CPU 工作队列
	 * 等待所有待处理的工作完成
	 */
}

/*
 * 【函数】vmstat_shepherd - 牧羊人工作线程
 * @w: 工作结构
 *
 * 【功能】
 * 定期检查所有 CPU，唤醒因不活跃而停止的 vmstat 工作线程。
 *
 * 【"牧羊人"的含义】
 * 像牧羊人照看羊群一样，这个线程照看所有 CPU 的 vmstat 工作。
 * 如果发现某个 CPU 有待处理的更新但工作线程已停止，就唤醒它。
 *
 * 【隔离 CPU 的特殊处理】
 * 跳过隔离的 CPU，避免干扰隔离的工作负载。
 * 隔离 CPU 通常用于实时任务或高性能计算。
 *
 * 【工作流程】
 * 1. 遍历所有在线 CPU
 * 2. 跳过隔离的 CPU
 * 3. 如果工作不忙且需要更新，立即调度工作
 * 4. 重新调度自己在下一个间隔运行
 *
 * 【设计理由】
 * vmstat_update() 在没有更新时会停止自己，节省 CPU。
 * 但如果之后又有更新产生，需要有人唤醒它。
 * vmstat_shepherd 就是这个"唤醒者"。
 */
static void vmstat_shepherd(struct work_struct *w)
{
	int cpu;

	cpus_read_lock();
	/* 获取 CPU 热插拔读锁，防止 CPU 在遍历期间热插拔 */

	/* Check processors whose vmstat worker threads have been disabled */
	/* 检查 vmstat 工作线程已被禁用的处理器 */
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
		/*
		 * 内核中 vmstat 计数器的使用者要么需要精确值
		 * （使用 zone_page_state_snapshot 接口），
		 * 要么可以容忍不精确（因为常规刷新可能在任意时间发生，
		 * 累积误差可能增长，见 calculate_normal_threshold）。
		 *
		 * 从这个角度看，对于已从内核干扰中隔离的 CPU，
		 * 常规刷新可以推迟，而关键基础设施永远不会注意到。
		 * 在 vmstat_shepherd 中跳过所有隔离 CPU 的常规刷新，
		 * 以避免干扰隔离的工作负载。
		 *
		 * 【隔离 CPU 的特点】
		 * - 通常用于实时任务或高性能计算
		 * - 需要最小化内核干扰
		 * - 统计精度要求低于性能要求
		 */
		scoped_guard(rcu) {
			/* RCU 作用域保护，用于安全访问 CPU 隔离状态 */

			if (cpu_is_isolated(cpu))
				continue;
			/* 跳过隔离的 CPU，不唤醒它们的 vmstat 工作 */

			if (!work_busy(&dw->work) && need_update(cpu))
				queue_delayed_work_on(cpu, mm_percpu_wq, dw, 0);
			/* 如果工作不忙且需要更新：
			 * - work_busy: 检查工作是否正在运行或待处理
			 * - need_update: 检查是否有待处理的统计更新
			 * - queue_delayed_work_on: 立即在指定 CPU 上调度工作（延迟 0）
			 *
			 * 【为什么同时检查两个条件】
			 * - work_busy: 避免重复调度已在运行的工作
			 * - need_update: 避免调度无意义的工作（没有更新）
			 */
		}

		cond_resched();
		/* 条件性重新调度，避免长时间占用 CPU
		 * 在多 CPU 系统中遍历可能需要较长时间
		 */
	}
	cpus_read_unlock();
	/* 释放 CPU 热插拔读锁 */

	schedule_delayed_work(&shepherd,
		round_jiffies_relative(sysctl_stat_interval));
	/* 重新调度牧羊人工作在下一个间隔运行
	 * round_jiffies_relative: 将唤醒时间对齐到整秒，减少唤醒次数
	 * sysctl_stat_interval: 统计刷新间隔（默认 HZ，即 1 秒）
	 */
}

/*
 * 【函数】start_shepherd_timer - 启动牧羊人定时器
 *
 * 【功能】
 * 初始化所有 CPU 的 vmstat 工作，并启动牧羊人工作。
 *
 * 【初始化步骤】
 * 1. 为每个可能的 CPU 初始化可延迟工作
 * 2. 对于离线的 CPU，禁用其工作（等待上线时启用）
 * 3. 调度牧羊人工作开始监控
 *
 * 【CPU 热插拔对称性】
 * vmstat_cpu_online() 和 vmstat_cpu_down_prep()
 * 在 CPU 热插拔事件期间对称地启用和禁用 vmstat_work。
 *
 * 【为什么对所有可能的 CPU 初始化】
 * 包括当前离线的 CPU，因为它们可能在运行时上线。
 * 提前初始化避免热插拔时的额外开销。
 */
static void __init start_shepherd_timer(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		/* 遍历所有可能的 CPU（包括离线的）
		 * possible: 系统支持的所有 CPU（可能未上线）
		 * present: 物理存在的 CPU
		 * online: 当前在线的 CPU
		 */

		INIT_DEFERRABLE_WORK(per_cpu_ptr(&vmstat_work, cpu),
			vmstat_update);
		/* 初始化可延迟工作
		 * DEFERRABLE: 可以延迟到 CPU 唤醒时执行，节省电源
		 * vmstat_update: 工作处理函数
		 */

		/*
		 * For secondary CPUs during CPU hotplug scenarios,
		 * vmstat_cpu_online() will enable the work.
		 * mm/vmstat:online enables and disables vmstat_work
		 * symmetrically during CPU hotplug events.
		 */
		/*
		 * 对于 CPU 热插拔场景中的次要 CPU，
		 * vmstat_cpu_online() 将启用工作。
		 * mm/vmstat:online 在 CPU 热插拔事件期间
		 * 对称地启用和禁用 vmstat_work。
		 */
		if (!cpu_online(cpu))
			disable_delayed_work_sync(&per_cpu(vmstat_work, cpu));
		/* 如果 CPU 离线，禁用其工作
		 * disable_delayed_work_sync: 同步禁用，等待当前工作完成
		 *
		 * 【为什么要禁用】
		 * - 离线 CPU 不需要统计更新
		 * - 避免调度到不存在的 CPU 上
		 * - 上线时会重新启用
		 */
	}

	schedule_delayed_work(&shepherd,
		round_jiffies_relative(sysctl_stat_interval));
	/* 启动牧羊人工作
	 * 开始定期检查所有 CPU 的统计更新需求
	 */
}

/*
 * 【函数】init_cpu_node_state - 初始化 CPU 节点状态
 *
 * 【功能】
 * 标记有 CPU 的 NUMA 节点为 N_CPU 状态。
 *
 * 【N_CPU 状态】
 * 表示该节点至少有一个 CPU。
 * 某些 NUMA 节点可能只有内存而没有 CPU（纯内存节点）。
 *
 * 【为什么需要这个函数】
 * - 区分有 CPU 的节点和纯内存节点
 * - 某些操作只需要在有 CPU 的节点上执行
 * - 优化内存分配策略（优先在有 CPU 的节点上分配）
 *
 * 【使用场景示例】
 * - 确定哪些节点需要初始化 per-CPU 数据
 * - 优化跨节点内存访问
 * - NUMA 感知的调度和内存分配
 */
static void __init init_cpu_node_state(void)
{
	int node;

	for_each_online_node(node) {
		/* 遍历所有在线节点
		 * online_node: 当前可用的 NUMA 节点
		 */

		if (!cpumask_empty(cpumask_of_node(node)))
			node_set_state(node, N_CPU);
		/* 如果节点有 CPU，设置 N_CPU 状态
		 * cpumask_of_node: 获取节点的 CPU 掩码
		 * cpumask_empty: 检查掩码是否为空（无 CPU）
		 * node_set_state: 设置节点状态标志
		 */
	}
}

/*
 * 【函数】vmstat_cpu_online - CPU 上线回调
 * @cpu: 上线的 CPU 编号
 * @return: 0 表示成功
 *
 * 【功能】
 * 在 CPU 上线时执行的操作：
 * 1. 刷新 zone 统计阈值（因为 CPU 数量变化）
 * 2. 更新节点的 N_CPU 状态
 * 3. 启用该 CPU 的 vmstat 工作
 *
 * 【为什么要刷新阈值】
 * 阈值计算依赖于 CPU 数量（见 calculate_normal_threshold）。
 * CPU 上线后，总 CPU 数增加，阈值需要重新计算。
 *
 * 【与 start_shepherd_timer 的对称性】
 * start_shepherd_timer 为离线 CPU 禁用工作，
 * vmstat_cpu_online 为上线 CPU 启用工作。
 *
 * 【调用时机】
 * CPU 热插拔框架在 CPU 上线时调用此回调。
 */
static int vmstat_cpu_online(unsigned int cpu)
{
	if (vmstat_late_init_done)
		refresh_zone_stat_thresholds();
	/* 如果后期初始化已完成，刷新阈值
	 * vmstat_late_init_done: 标记 vmstat 系统是否完全初始化
	 * 只有在系统初始化完成后才刷新阈值
	 *
	 * 【为什么检查 vmstat_late_init_done】
	 * - 启动早期 CPU 上线不需要刷新（尚未完全初始化）
	 * - 只有运行时热插拔才需要刷新
	 */

	if (!node_state(cpu_to_node(cpu), N_CPU)) {
		node_set_state(cpu_to_node(cpu), N_CPU);
	}
	/* 如果该 CPU 所在节点尚未标记为有 CPU，标记它
	 * cpu_to_node: 获取 CPU 所属的 NUMA 节点
	 *
	 * 【使用场景】
	 * - 节点首次上线 CPU
	 * - 纯内存节点首次获得 CPU
	 */

	enable_delayed_work(&per_cpu(vmstat_work, cpu));
	/* 启用该 CPU 的 vmstat 工作
	 * 与 start_shepherd_timer 中的 disable_delayed_work_sync 对称
	 *
	 * 【效果】
	 * - vmstat_shepherd 可以调度这个 CPU 的工作
	 * - CPU 可以开始定期刷新统计
	 */

	return 0;
	/* 返回 0 表示成功
	 * CPU 热插拔框架要求的返回值
	 */
}

/*
 * 【函数】vmstat_cpu_down_prep - CPU 下线准备回调
 * @cpu: 即将下线的 CPU 编号
 * @return: 0 表示成功
 *
 * 【功能】
 * 在 CPU 下线前执行的准备操作：禁用该 CPU 的 vmstat 工作。
 *
 * 【为什么使用同步禁用】
 * disable_delayed_work_sync 确保：
 * - 当前正在运行的工作完成
 * - 不会有新的工作被调度
 * - CPU 下线时没有待处理的 vmstat 工作
 *
 * 【与 vmstat_cpu_online 的对称性】
 * vmstat_cpu_online 启用工作，vmstat_cpu_down_prep 禁用工作。
 * 确保 CPU 热插拔过程的完整性。
 *
 * 【调用时机】
 * CPU 热插拔框架在 CPU 下线前调用此回调。
 */
static int vmstat_cpu_down_prep(unsigned int cpu)
{
	disable_delayed_work_sync(&per_cpu(vmstat_work, cpu));
	/* 同步禁用 vmstat 工作
	 * sync: 等待当前工作完成后才返回
	 *
	 * 【必须同步的原因】
	 * - 避免工作在 CPU 下线后仍尝试执行
	 * - 确保统计数据已刷新到全局计数器
	 * - 防止访问已下线 CPU 的 per-CPU 数据
	 */

	return 0;
	/* 返回 0 表示成功 */
}

/*
 * 【函数】vmstat_cpu_dead - CPU 死亡回调
 * @cpu: 已死亡的 CPU 编号
 * @return: 0 表示成功
 *
 * 【功能】
 * 在 CPU 完全下线后执行的清理操作：
 * 1. 刷新 zone 统计阈值（因为 CPU 数量减少）
 * 2. 如果节点没有 CPU 了，清除 N_CPU 状态
 *
 * 【与 vmstat_cpu_online 的对称性】
 * vmstat_cpu_online 设置 N_CPU 状态，
 * vmstat_cpu_dead 清除 N_CPU 状态（如果节点无 CPU）。
 *
 * 【为什么要刷新阈值】
 * CPU 数量减少，阈值需要重新计算（见 calculate_normal_threshold）。
 * 阈值过大会导致统计不准确。
 *
 * 【调用时机】
 * CPU 热插拔框架在 CPU 完全下线后调用此回调。
 */
static int vmstat_cpu_dead(unsigned int cpu)
{
	const struct cpumask *node_cpus;
	int node;

	node = cpu_to_node(cpu);
	/* 获取 CPU 所属的 NUMA 节点 */

	refresh_zone_stat_thresholds();
	/* 刷新阈值，因为 CPU 数量减少
	 * 必须在检查节点状态前执行，确保阈值正确
	 */

	node_cpus = cpumask_of_node(node);
	/* 获取节点的 CPU 掩码 */

	if (!cpumask_empty(node_cpus))
		return 0;
	/* 如果节点还有其他 CPU，不清除 N_CPU 状态
	 * 直接返回，保持节点的 N_CPU 状态
	 */

	node_clear_state(node, N_CPU);
	/* 如果节点没有 CPU 了，清除 N_CPU 状态
	 * 将节点标记为纯内存节点
	 *
	 * 【使用场景】
	 * - 节点的最后一个 CPU 下线
	 * - 节点变为纯内存节点
	 */

	return 0;
	/* 返回 0 表示成功 */
}

/*
 * 【函数】vmstat_late_init - vmstat 后期初始化
 * @return: 0 表示成功
 *
 * 【功能】
 * 在系统启动后期执行的 vmstat 初始化：
 * 1. 刷新 zone 统计阈值
 * 2. 设置后期初始化完成标志
 *
 * 【为什么需要后期初始化】
 * - 启动早期某些子系统尚未就绪
 * - 所有 CPU 上线后才能准确计算阈值
 * - 作为热插拔回调行为的分界点
 *
 * 【late_initcall 的含义】
 * late_initcall 是 Linux 内核初始化顺序中的一个阶段，
 * 在大部分子系统初始化完成后执行。
 *
 * 【vmstat_late_init_done 的作用】
 * 作为标志位，影响 vmstat_cpu_online 的行为：
 * - 为 0：启动早期，不刷新阈值
 * - 为 1：运行时，热插拔时需要刷新阈值
 */
static int __init vmstat_late_init(void)
{
	refresh_zone_stat_thresholds();
	/* 刷新所有 zone 的统计阈值
	 * 此时所有 CPU 应该已经上线，可以准确计算阈值
	 */

	vmstat_late_init_done = 1;
	/* 设置后期初始化完成标志
	 * 之后的 CPU 热插拔事件将触发阈值刷新
	 */

	return 0;
	/* 返回 0 表示成功 */
}
late_initcall(vmstat_late_init);
/* 注册为后期初始化调用
 * 在内核初始化的后期阶段自动调用此函数
 */
#endif

#ifdef CONFIG_PROC_FS
/*
 * 【sysctl 表】vmstat_table - vmstat 的 sysctl 接口
 *
 * 提供通过 /proc/sys/vm/ 访问的 vmstat 配置项。
 */
static const struct ctl_table vmstat_table[] = {
#ifdef CONFIG_SMP
	{
		.procname	= "stat_interval",
		/* /proc/sys/vm/stat_interval
		 * 统计刷新间隔（以 jiffies 为单位）
		 */

		.data		= &sysctl_stat_interval,
		/* 指向 sysctl_stat_interval 全局变量 */

		.maxlen		= sizeof(sysctl_stat_interval),
		/* 数据大小 */

		.mode		= 0644,
		/* 权限：所有者可读写，其他人只读 */

		.proc_handler	= proc_dointvec_jiffies,
		/* 处理函数：整数转换为 jiffies
		 * 用户写入秒数，内核转换为 jiffies
		 */
	},
	{
		.procname	= "stat_refresh",
		/* /proc/sys/vm/stat_refresh
		 * 立即刷新统计的触发器
		 */

		.data		= NULL,
		/* 无关联数据，纯触发器 */

		.maxlen		= 0,
		/* 数据大小为 0 */

		.mode		= 0600,
		/* 权限：仅所有者可读写（通常是 root）
		 * 安全考虑：只有特权用户可以触发刷新
		 */

		.proc_handler	= vmstat_refresh,
		/* 处理函数：立即刷新所有统计
		 * 读取时触发刷新并检测负值
		 */
	},
#endif
#ifdef CONFIG_NUMA
	{
		.procname	= "numa_stat",
		/* /proc/sys/vm/numa_stat
		 * 控制是否启用 NUMA 统计
		 */

		.data		= &sysctl_vm_numa_stat,
		/* 指向 sysctl_vm_numa_stat 全局变量 */

		.maxlen		= sizeof(int),
		/* 数据大小：一个整数 */

		.mode		= 0644,
		/* 权限：所有者可读写，其他人只读 */

		.proc_handler	= sysctl_vm_numa_stat_handler,
		/* 处理函数：处理 NUMA 统计开关
		 * 启用/禁用时需要特殊处理
		 */

		.extra1		= SYSCTL_ZERO,
		/* 最小值：0（禁用） */

		.extra2		= SYSCTL_ONE,
		/* 最大值：1（启用） */
	},
#endif
};
#endif

struct workqueue_struct *mm_percpu_wq;
/* 内存管理 per-CPU 工作队列
 * 用于处理 vmstat 等内存管理相关的 per-CPU 工作
 */

/*
 * 【函数】init_mm_internals - 初始化内存管理内部机制
 *
 * 【功能】
 * 这是内存管理子系统的主初始化函数，设置：
 * 1. per-CPU 工作队列
 * 2. CPU 热插拔回调
 * 3. CPU 节点状态
 * 4. vmstat 牧羊人定时器
 * 5. /proc 文件系统接口
 * 6. sysctl 接口
 *
 * 【调用时机】
 * 在内核启动早期调用（__init 标记）。
 *
 * 【初始化顺序的重要性】
 * 必须先设置工作队列和热插拔回调，再启动牧羊人定时器。
 */
void __init init_mm_internals(void)
{
	int ret __maybe_unused;
	/* __maybe_unused: 某些配置下 ret 可能未使用，抑制警告 */

	mm_percpu_wq = alloc_workqueue("mm_percpu_wq",
				       WQ_MEM_RECLAIM | WQ_PERCPU, 0);
	/* 分配内存管理 per-CPU 工作队列
	 *
	 * 【标志说明】
	 * WQ_MEM_RECLAIM: 内存回收路径可以使用此队列
	 *                 必须保留一个工作线程以避免死锁
	 *                 （内存分配失败时仍需处理统计刷新）
	 * WQ_PERCPU: 每个 CPU 有独立的工作池
	 *            避免跨 CPU 调度，提高缓存局部性
	 * 0: max_active，0 表示使用默认值
	 *
	 * 【为什么需要专用工作队列】
	 * - 隔离 vmstat 工作与其他系统工作
	 * - 保证内存回收路径的可靠性
	 * - per-CPU 特性提高性能
	 */

#ifdef CONFIG_SMP
	ret = cpuhp_setup_state_nocalls(CPUHP_MM_VMSTAT_DEAD, "mm/vmstat:dead",
					NULL, vmstat_cpu_dead);
	/* 注册 CPU 死亡状态的热插拔回调
	 *
	 * CPUHP_MM_VMSTAT_DEAD: 热插拔状态标识符
	 * "mm/vmstat:dead": 状态名称（用于调试和日志）
	 * NULL: 无启动回调（CPU 死亡时不需要）
	 * vmstat_cpu_dead: 拆除回调（CPU 死亡时调用）
	 * nocalls: 不立即调用回调，只注册
	 */
	if (ret < 0)
		pr_err("vmstat: failed to register 'dead' hotplug state\n");
	/* 注册失败时打印错误，但继续执行
	 * 不是致命错误，系统仍可运行
	 */

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "mm/vmstat:online",
					vmstat_cpu_online,
					vmstat_cpu_down_prep);
	/* 注册 CPU 在线/下线准备的热插拔回调
	 *
	 * CPUHP_AP_ONLINE_DYN: 动态分配在线状态标识符
	 *                      在应用处理器（AP）启动路径中
	 * "mm/vmstat:online": 状态名称
	 * vmstat_cpu_online: CPU 上线时的启动回调
	 * vmstat_cpu_down_prep: CPU 下线前的拆除回调
	 */
	if (ret < 0)
		pr_err("vmstat: failed to register 'online' hotplug state\n");

	cpus_read_lock();
	/* 获取 CPU 热插拔读锁，保护 CPU 状态 */

	init_cpu_node_state();
	/* 初始化 CPU 节点状态（设置 N_CPU 标志）*/

	cpus_read_unlock();
	/* 释放 CPU 热插拔读锁 */

	start_shepherd_timer();
	/* 启动 vmstat 牧羊人定时器
	 * 必须在热插拔回调注册后执行
	 */
#endif
#ifdef CONFIG_PROC_FS
	proc_create_seq("buddyinfo", 0444, NULL, &fragmentation_op);
	/* 创建 /proc/buddyinfo
	 * 0444: 所有用户只读
	 * 显示伙伴系统的碎片信息
	 */

	proc_create_seq("pagetypeinfo", 0400, NULL, &pagetypeinfo_op);
	/* 创建 /proc/pagetypeinfo
	 * 0400: 仅所有者只读（root）
	 * 显示按迁移类型分类的页面信息
	 * 权限更严格因为信息更敏感
	 */

	proc_create_seq("vmstat", 0444, NULL, &vmstat_op);
	/* 创建 /proc/vmstat
	 * 0444: 所有用户只读
	 * 显示虚拟内存统计信息
	 */

	proc_create_seq("zoneinfo", 0444, NULL, &zoneinfo_op);
	/* 创建 /proc/zoneinfo
	 * 0444: 所有用户只读
	 * 显示内存 zone 的详细信息
	 */

	register_sysctl_init("vm", vmstat_table);
	/* 注册 sysctl 表到 /proc/sys/vm/
	 * vmstat_table: 包含 stat_interval、stat_refresh、numa_stat 等
	 */
#endif
}

#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_COMPACTION)

/*
 * Return an index indicating how much of the available free memory is
 * unusable for an allocation of the requested size.
 */
/*
 * 【函数】unusable_free_index - 计算不可用空闲内存索引
 * @order: 请求的分配阶数
 * @info: 连续页面信息
 * @return: 不可用索引（0-1000），表示碎片程度
 *
 * 【功能】
 * 返回一个索引，指示有多少可用空闲内存对于请求大小的分配是不可用的。
 *
 * 【索引含义】
 * - 0: 无碎片，所有空闲内存都可用于此分配
 * - 1000: 高度碎片化，所有空闲内存都不可用于此分配
 * - 中间值: 部分碎片化
 *
 * 【计算公式】
 * index = (free_pages - suitable_pages) * 1000 / free_pages
 * 其中 suitable_pages = free_blocks_suitable << order
 *
 * 【使用场景】
 * debugfs 接口，用于调试内存碎片问题。
 */
static int unusable_free_index(unsigned int order,
				struct contig_page_info *info)
{
	/* No free memory is interpreted as all free memory is unusable */
	/* 没有空闲内存被解释为所有空闲内存都不可用 */
	if (info->free_pages == 0)
		return 1000;
	/* 特殊情况：没有空闲页面
	 * 返回 1000（最大不可用度）
	 */

	/*
	 * Index should be a value between 0 and 1. Return a value to 3
	 * decimal places.
	 *
	 * 0 => no fragmentation
	 * 1 => high fragmentation
	 */
	/*
	 * 索引应该是 0 和 1 之间的值。返回一个保留 3 位小数的值。
	 *
	 * 0 => 无碎片
	 * 1 => 高碎片
	 */
	return div_u64((info->free_pages - (info->free_blocks_suitable << order)) * 1000ULL, info->free_pages);
	/* 计算不可用索引
	 * (总空闲页 - 可用于此阶数的页) * 1000 / 总空闲页
	 *
	 * 【示例】
	 * 假设 order=2（需要 4 个连续页）：
	 * - free_pages = 1000 页
	 * - free_blocks_suitable = 50 个块（每块 4 页）
	 * - suitable_pages = 50 << 2 = 200 页
	 * - index = (1000 - 200) * 1000 / 1000 = 800
	 * - 表示 80% 的空闲内存因碎片而不可用
	 *
	 * 【为什么乘以 1000】
	 * - 保留 3 位小数精度
	 * - 避免浮点运算
	 * - 1000 表示 100.0%
	 */
}

/*
 * 【函数】unusable_show_print - 打印不可用空闲内存索引
 * @m: seq_file 结构
 * @pgdat: 节点数据
 * @zone: 内存 zone
 *
 * 【功能】
 * 为指定 zone 打印所有阶数的不可用空闲内存索引。
 *
 * 【输出格式】
 * Node X, zone ZONE_NAME 0.000 0.123 0.456 ...
 * 每个数字对应一个阶数的碎片指数。
 */
static void unusable_show_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	unsigned int order;
	int index;
	struct contig_page_info info;

	seq_printf(m, "Node %d, zone %8s ",
				pgdat->node_id,
				zone->name);
	/* 打印节点 ID 和 zone 名称 */

	for (order = 0; order < NR_PAGE_ORDERS; ++order) {
		/* 遍历所有阶数 */

		fill_contig_page_info(zone, order, &info);
		/* 填充连续页面信息 */

		index = unusable_free_index(order, &info);
		/* 计算不可用索引 */

		seq_printf(m, "%d.%03d ", index / 1000, index % 1000);
		/* 打印索引，格式为 X.XXX（3 位小数）
		 * 例如：index = 800 输出 "0.800"
		 */
	}

	seq_putc(m, '\n');
	/* 换行 */
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
/*
 * 【函数】unusable_show - 显示不可用空闲空间索引
 * @m: seq_file 结构
 * @arg: 节点数据（pg_data_t 指针）
 * @return: 0 表示成功
 *
 * 显示不可用空闲空间索引
 *
 * 不可用空闲空间索引测量有多少可用空闲内存无法用于满足给定大小的分配，
 * 是一个 0 到 1 之间的值。值越高，空闲内存越不可用，
 * 隐含地说明外部碎片越严重。这可以通过乘以 100 表示为百分比。
 *
 * 【使用场景】
 * - 调试内存碎片问题
 * - 评估内存压实的必要性
 * - 性能分析
 *
 * 【如何读取】
 * cat /sys/kernel/debug/extfrag/unusable_index
 */
static int unusable_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	/* check memoryless node */
	/* 检查无内存节点 */
	if (!node_state(pgdat->node_id, N_MEMORY))
		return 0;
	/* 如果节点没有内存，跳过
	 * 某些 NUMA 节点可能只有 CPU 而无内存
	 */

	walk_zones_in_node(m, pgdat, true, false, unusable_show_print);
	/* 遍历节点中的所有 zone 并打印
	 * true: 包括空 zone
	 * false: 不详细输出
	 */

	return 0;
}

static const struct seq_operations unusable_sops = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= unusable_show,
};
/* seq_file 操作集
 * 复用 fragmentation 的迭代函数
 */

DEFINE_SEQ_ATTRIBUTE(unusable);
/* 定义 seq_file 属性
 * 生成 unusable_fops 等相关结构
 */

/*
 * 【函数】extfrag_show_print - 打印外部碎片索引
 * @m: seq_file 结构
 * @pgdat: 节点数据
 * @zone: 内存 zone
 *
 * 【功能】
 * 为指定 zone 打印所有阶数的外部碎片索引。
 *
 * 【与 unusable_show_print 的区别】
 * - unusable_index: 测量有多少空闲内存因碎片而不可用
 * - extfrag_index: 测量碎片化的原因（内存不足 vs 碎片）
 */
static void extfrag_show_print(struct seq_file *m,
					pg_data_t *pgdat, struct zone *zone)
{
	unsigned int order;
	int index;

	/* Alloc on stack as interrupts are disabled for zone walk */
	/* 在栈上分配，因为 zone 遍历期间中断被禁用 */
	struct contig_page_info info;

	seq_printf(m, "Node %d, zone %8s ",
				pgdat->node_id,
				zone->name);
	/* 打印节点 ID 和 zone 名称 */

	for (order = 0; order < NR_PAGE_ORDERS; ++order) {
		/* 遍历所有阶数 */

		fill_contig_page_info(zone, order, &info);
		/* 填充连续页面信息 */

		index = __fragmentation_index(order, &info);
		/* 计算碎片索引
		 * 区分内存不足和碎片问题
		 */

		seq_printf(m, "%2d.%03d ", index / 1000, index % 1000);
		/* 打印索引，格式为 XX.XXX（3 位小数）
		 * %2d: 整数部分至少 2 位（可能是负数）
		 */
	}

	seq_putc(m, '\n');
	/* 换行 */
}

/*
 * Display fragmentation index for orders that allocations would fail for
 */
/*
 * 【函数】extfrag_show - 显示会导致分配失败的阶数的碎片索引
 * @m: seq_file 结构
 * @arg: 节点数据（pg_data_t 指针）
 * @return: 0 表示成功
 *
 * 显示会导致分配失败的阶数的碎片索引
 *
 * 【功能】
 * 帮助诊断分配失败的原因：
 * - 接近 0: 失败是因为内存真的不足
 * - 接近 1: 失败是因为碎片（有足够的空闲页，但不连续）
 *
 * 【使用场景】
 * - 调试大块内存分配失败
 * - 决定是否需要内存压实
 * - 评估内存碎片对性能的影响
 *
 * 【如何读取】
 * cat /sys/kernel/debug/extfrag/extfrag_index
 */
static int extfrag_show(struct seq_file *m, void *arg)
{
	pg_data_t *pgdat = (pg_data_t *)arg;

	walk_zones_in_node(m, pgdat, true, false, extfrag_show_print);
	/* 遍历节点中的所有 zone 并打印 */

	return 0;
}

static const struct seq_operations extfrag_sops = {
	.start	= frag_start,
	.next	= frag_next,
	.stop	= frag_stop,
	.show	= extfrag_show,
};
/* seq_file 操作集
 * 复用 fragmentation 的迭代函数
 */

DEFINE_SEQ_ATTRIBUTE(extfrag);
/* 定义 seq_file 属性
 * 生成 extfrag_fops 等相关结构
 */

/*
 * 【函数】extfrag_debug_init - 初始化外部碎片 debugfs 接口
 * @return: 0 表示成功
 *
 * 【功能】
 * 在 debugfs 中创建外部碎片相关的调试接口：
 * - /sys/kernel/debug/extfrag/unusable_index
 * - /sys/kernel/debug/extfrag/extfrag_index
 *
 * 【两个文件的用途】
 * unusable_index: 显示有多少空闲内存因碎片而不可用
 * extfrag_index: 显示碎片化的原因（内存不足 vs 碎片）
 *
 * 【初始化时机】
 * device_initcall 在设备初始化阶段调用。
 */
static int __init extfrag_debug_init(void)
{
	struct dentry *extfrag_debug_root;

	extfrag_debug_root = debugfs_create_dir("extfrag", NULL);
	/* 创建 /sys/kernel/debug/extfrag/ 目录
	 * NULL: 在 debugfs 根目录下创建
	 */

	debugfs_create_file("unusable_index", 0444, extfrag_debug_root, NULL,
			    &unusable_fops);
	/* 创建 unusable_index 文件
	 * 0444: 所有用户只读
	 * unusable_fops: 由 DEFINE_SEQ_ATTRIBUTE(unusable) 生成
	 */

	debugfs_create_file("extfrag_index", 0444, extfrag_debug_root, NULL,
			    &extfrag_fops);
	/* 创建 extfrag_index 文件
	 * 0444: 所有用户只读
	 * extfrag_fops: 由 DEFINE_SEQ_ATTRIBUTE(extfrag) 生成
	 */

	return 0;
	/* 返回 0 表示成功
	 * debugfs_create_* 函数失败时返回 ERR_PTR，但我们不检查
	 * 因为 debugfs 失败不应该影响系统运行
	 */
}

module_init(extfrag_debug_init);
/* 注册为模块初始化函数
 * 在内核模块加载时调用（或编译进内核时在启动时调用）
 */

#endif
/* CONFIG_DEBUG_FS && CONFIG_COMPACTION */

/*
 * ============================================================================
 * 【文件结束】
 * ============================================================================
 *
 * 本文件实现了 Linux 内核的虚拟内存统计（vmstat）系统。
 *
 * 【核心机制总结】
 *
 * 1. **差分计数器机制**
 *    - 每个 CPU 维护本地差分计数器
 *    - 累积到阈值时同步到全局计数器
 *    - 减少原子操作，提高性能
 *
 * 2. **阈值动态调整**
 *    - 基于 CPU 数量和内存大小计算
 *    - 平衡统计精度和性能
 *    - CPU 热插拔时自动调整
 *
 * 3. **定期刷新机制**
 *    - vmstat_update: 自适应调度，无更新时停止
 *    - vmstat_shepherd: 定期唤醒停止的工作线程
 *    - 与 NOHZ 模式集成，节省电源
 *
 * 4. **统计类型**
 *    - Zone 统计：zone_stat_item（按 zone 统计）
 *    - Node 统计：node_stat_item（按 NUMA 节点统计）
 *    - NUMA 事件：numa_stat_item（NUMA 特定事件）
 *    - VM 事件：vm_event_states（全局事件计数）
 *
 * 5. **用户空间接口**
 *    - /proc/vmstat: 汇总的虚拟内存统计
 *    - /proc/zoneinfo: 详细的 zone 信息
 *    - /proc/buddyinfo: 伙伴系统碎片信息
 *    - /proc/pagetypeinfo: 按迁移类型分类的页面信息
 *    - /proc/sys/vm/stat_interval: 刷新间隔配置
 *    - /proc/sys/vm/stat_refresh: 立即刷新触发器
 *    - /proc/sys/vm/numa_stat: NUMA 统计开关
 *    - /sys/kernel/debug/extfrag/: 碎片调试接口
 *
 * 6. **性能优化**
 *    - Per-CPU 数据避免缓存行竞争
 *    - CMPXCHG 优化减少锁开销
 *    - Overstep 机制减少同步频率
 *    - 可延迟工作节省电源
 *    - 隔离 CPU 特殊处理
 *
 * 7. **可靠性保障**
 *    - CPU 热插拔完整支持
 *    - 内存回收路径专用工作队列
 *    - 负值检测机制
 *    - 节点状态同步
 *
 * 【设计理念】
 * - 性能优先：最小化对关键路径的影响
 * - 可扩展性：支持大规模 SMP 和 NUMA 系统
 * - 灵活性：支持运行时配置和调试
 * - 可靠性：确保统计的一致性和准确性
 */
