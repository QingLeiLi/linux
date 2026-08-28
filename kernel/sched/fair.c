// SPDX-License-Identifier: GPL-2.0
/*
 * Completely Fair Scheduling (CFS) Class (SCHED_NORMAL/SCHED_BATCH)
 *
 *  Copyright (C) 2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 *  Interactivity improvements by Mike Galbraith
 *  (C) 2007 Mike Galbraith <efault@gmx.de>
 *
 *  Various enhancements by Dmitry Adamushko.
 *  (C) 2007 Dmitry Adamushko <dmitry.adamushko@gmail.com>
 *
 *  Group scheduling enhancements by Srivatsa Vaddagiri
 *  Copyright IBM Corporation, 2007
 *  Author: Srivatsa Vaddagiri <vatsa@linux.vnet.ibm.com>
 *
 *  Scaled math optimizations by Thomas Gleixner
 *  Copyright (C) 2007, Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
 *
 *  Adaptive scheduling granularity, math enhancements by Peter Zijlstra
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra
 */
#include <linux/energy_model.h>
#include <linux/mmap_lock.h>
#include <linux/hugetlb_inline.h>
#include <linux/jiffies.h>
#include <linux/mm_api.h>
#include <linux/highmem.h>
#include <linux/spinlock_api.h>
#include <linux/cpumask_api.h>
#include <linux/lockdep_api.h>
/* 第一组依赖提供内存、时间、CPU mask、锁与调度器基础接口。 */
#include <linux/softirq.h>
#include <linux/refcount_api.h>
#include <linux/topology.h>
#include <linux/sched/clock.h>
#include <linux/sched/cond_resched.h>
#include <linux/sched/cputime.h>
#include <linux/sched/isolation.h>
#include <linux/sched/nohz.h>
#include <linux/sched/prio.h>
/* 第二组依赖补充 softirq、拓扑、调度时钟、隔离与 NOHZ 接口。 */

#include <linux/cpuidle.h>
#include <linux/interrupt.h>
#include <linux/memory-tiers.h>
#include <linux/mempolicy.h>
#include <linux/mutex_api.h>
#include <linux/profile.h>
#include <linux/psi.h>
#include <linux/ratelimit.h>
#include <linux/task_work.h>
/* 第三组依赖服务 idle、NUMA 策略、PSI、profile 与异步 task work。 */
#include <linux/rbtree_augmented.h>

#include <asm/switch_to.h>

#include <uapi/linux/sched/types.h>

#include "sched.h"
#include "stats.h"
#include "autogroup.h"

/*
 * CFS/EEVDF 用 vruntime 表示按权重归一化后的已获服务，并从所有实体的加权
 * vruntime 近似虚拟时间 V；只有 lag>=0 的实体有资格运行，再从中选择最早虚拟
 * deadline。tasks_timeline 是带 min_vruntime/min_slice 增广信息的红黑树，rq 锁
 * 保护排队、选择、PELT 与层级传播。组调度把每个 task_group 在每个 CPU 上表现为
 * 一层 cfs_rq 和父 sched_entity，enqueue/dequeue 必须沿祖先链维护 runnable 与负载。
 *
 * SMP 选核与负载均衡分两阶段：拓扑统计和无锁容量值只生成候选，真正 detach/attach
 * 时锁住源/目标 rq 并复验亲和性、热度和可运行状态。CFS bandwidth 从 task_group
 * 全局配额向 per-cfs_rq runtime 池切片，耗尽即 throttle 整个层级，period/slack timer
 * 补充后再逐层 unthrottle。NUMA balancing 以采样 hint fault 推断 task/mm 的内存局部性，
 * 其迁移选择同样不能越过 cpuset、容量和锁后状态复验。
 */

/*
 * The initial- and re-scaling of tunables is configurable
 *
 * Options are:
 *
 *   SCHED_TUNABLESCALING_NONE - unscaled, always *1
 *   SCHED_TUNABLESCALING_LOG - scaled logarithmically, *1+ilog(ncpus)
 *   SCHED_TUNABLESCALING_LINEAR - scaled linear, *ncpus
 *
 * (default SCHED_TUNABLESCALING_LOG = *(1+ilog(ncpus))
 */
/* tunable 可不缩放、按 CPU 数对数缩放或线性缩放；默认采用对数缩放。 */
unsigned int sysctl_sched_tunable_scaling = SCHED_TUNABLESCALING_LOG;

/*
 * Minimal preemption granularity for CPU-bound tasks:
 *
 * (default: 0.70 msec * (1 + ilog(ncpus)), units: nanoseconds)
 */
/* CPU 密集任务的最小抢占粒度默认是 0.70ms 乘以 CPU 数的对数缩放因子。 */
unsigned int sysctl_sched_base_slice			= 700000ULL;
static unsigned int normalized_sysctl_sched_base_slice	= 700000ULL;

__read_mostly unsigned int sysctl_sched_migration_cost	= 500000UL;

/*
 * setup_sched_thermal_decay_shift() - 吞掉已废弃启动参数并留下可诊断告警。
 *
 * 业务背景：启动参数解析链会把 sched_thermal_decay_shift= 交给此兼容入口；当前
 * 调度器不再使用该旋钮，保留入口是为了让旧引导配置不会悄悄改变行为。
 * 入参：@str 是 "=" 后的借用字符串；本函数不读取也不取得其所有权。
 * 出参/返回：返回 1，表示 __setup 处理器已经认领该参数；无输出对象。
 * 注意事项：只在早期启动阶段调用，不能睡眠；告警是唯一可观察副作用。
 */
static int __init setup_sched_thermal_decay_shift(char *str)
{
	pr_warn("Ignoring the deprecated sched_thermal_decay_shift= option\n");
	return 1;
}
__setup("sched_thermal_decay_shift=", setup_sched_thermal_decay_shift);

/*
 * For asym packing, by default the lower numbered CPU has higher priority.
 */
/* 不对称打包的通用默认规则是 CPU 编号越小，优先级越高。 */
/*
 * arch_asym_cpu_priority() - 为不对称 CPU 打包策略提供可被架构替换的默认排序。
 *
 * 业务背景：fair 选核路径需要比较性能不对称 CPU；弱符号使架构能按真实拓扑覆写
 * 默认策略，而通用代码不必依赖具体 CPU 编号约定。
 * 入参：@cpu 是已验证的逻辑 CPU 编号，没有单位；仅输入，不涉及对象所有权。
 * 出参/返回：返回整数优先级，数值越大表示越优先；默认用 -cpu 令小编号胜出。
 * 注意事项：会在调度热路径读取，必须无锁、不可睡眠；调用者只能比较结果，不能把
 * 该默认编号规则误认为所有架构的性能排序。
 */
int __weak arch_asym_cpu_priority(int cpu)
{
	return -cpu;
}

/*
 * The margin used when comparing utilization with CPU capacity.
 *
 * (default: ~20%)
 */
/* 利用率与 CPU 容量比较保留约 20% 余量，避免贴近上限时仍判定可容纳。 */
#define fits_capacity(cap, max)	((cap) * 1280 < (max) * 1024)

/*
 * The margin used when comparing CPU capacities.
 * is 'cap1' noticeably greater than 'cap2'
 *
 * (default: ~5%)
 */
/* 两个 CPU 容量相差约 5% 以上，才把 cap1 视为显著大于 cap2。 */
#define capacity_greater(cap1, cap2) ((cap1) * 1024 > (cap2) * 1078)

#ifdef CONFIG_CFS_BANDWIDTH
/*
 * Amount of runtime to allocate from global (tg) to local (per-cfs_rq) pool
 * each time a cfs_rq requests quota.
 *
 * Note: in the case that the slice exceeds the runtime remaining (either due
 * to consumption or the quota being specified to be smaller than the slice)
 * we will always only issue the remaining available time.
 *
 * (default: 5 msec, units: microseconds)
 */
/* 每次从任务组全局池向 cfs_rq 最多切 5ms；剩余配额更少时只发剩余量。 */
static unsigned int sysctl_sched_cfs_bandwidth_slice		= 5000UL;
#endif

#ifdef CONFIG_NUMA_BALANCING
/* Restrict the NUMA promotion throughput (MB/s) for each target node. */
	/* 每个目标 NUMA 节点的晋升吞吐按 MB/s 限流。 */
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
static unsigned int sysctl_numa_balancing_promote_rate_limit = 65536;
#endif

#ifdef CONFIG_SYSCTL
static const struct ctl_table sched_fair_sysctls[] = {
#ifdef CONFIG_CFS_BANDWIDTH
	{
		.procname       = "sched_cfs_bandwidth_slice_us",
		.data           = &sysctl_sched_cfs_bandwidth_slice,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		.proc_handler   = proc_dointvec_minmax,
		.extra1         = SYSCTL_ONE,
	},
#endif
#ifdef CONFIG_NUMA_BALANCING
	{
		.procname	= "numa_balancing_promote_rate_limit_MBps",
		.data		= &sysctl_numa_balancing_promote_rate_limit,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
	},
#endif /* CONFIG_NUMA_BALANCING */
};

/* 注册 fair/NUMA 调节项；参数边界由 ctl_table 在写入时统一校验。 */
/*
 * sched_fair_sysctl_init() - 把 fair 调度器的可调参数发布到 /proc/sys/kernel。
 *
 * 业务背景：启动完成后 sysctl 核心调用此 late_initcall；它把带范围校验的表交给
 * sysctl 注册层，供管理员调整 bandwidth/NUMA 策略。
 * 入参：无。
 * 出参/返回：返回 register_sysctl_init() 的结果；成功后表由 sysctl 核心持有和查找，
 * 本文件仍静态保存其存储期。
 * 注意事项：仅初始化期调用，可睡眠与否由注册层决定；配置项是否存在取决于对应
 * CONFIG，不能据此假定所有内核都暴露同一节点。
 */
static int __init sched_fair_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_fair_sysctls);
	return 0;
}
late_initcall(sched_fair_sysctl_init);
#endif /* CONFIG_SYSCTL */

/* 权重变化后清除倒数缓存，下一次比例计算再按新总权重生成。 */
/*
 * update_load_add() - 增加 load_weight 并使其倒数缓存失效。
 *
 * 业务背景：enqueue 等路径把实体权重计入队列；后续 vruntime 比例计算依赖与总权重
 * 一致的 inv_weight，所以更新不能只改 weight。
 * 入参：@lw 是调用者在相应 rq 锁保护下独占更新的 load_weight；@inc 是要加入的无
 * 单位权重，纯输入且不会转移所有权。
 * 出参/返回：无直接返回；weight 增加且 inv_weight 置零，下一次计算惰性重建缓存。
 * 注意事项：不做溢出检查，调用者必须维持权重不变量；本函数不可单独提供并发保护。
 */
static inline void update_load_add(struct load_weight *lw, unsigned long inc)
{
	lw->weight += inc;
	lw->inv_weight = 0;
}

/*
 * update_load_sub() - 从 load_weight 撤销一份权重并废弃倒数缓存。
 *
 * 业务背景：dequeue/重加权路径与 update_load_add() 配对，确保队列总权重和比例
 * 计算看到同一版本。
 * 入参：@lw 是借用的、已由调用者同步保护的权重对象；@dec 是先前已计入的权重。
 * 出参/返回：无直接返回；weight 减少、inv_weight 清零，不转移任何对象所有权。
 * 注意事项：@dec 大于现有值会破坏无符号计数；调用者必须在实体仍归本队列时调用，
 * 且不可把缓存清零误解为立即执行除法。
 */
static inline void update_load_sub(struct load_weight *lw, unsigned long dec)
{
	lw->weight -= dec;
	lw->inv_weight = 0;
}

/*
 * update_load_set() - 原子语义之外地替换一个受保护权重值。
 *
 * 业务背景：重加权需要先撤销旧贡献再安装新权重；清除 inv_weight 防止旧倒数把新的
 * vruntime 归一化为错误比例。
 * 入参：@lw 是调用者独占或持锁借用的对象；@w 是新的无单位权重，仅输入。
 * 出参/返回：无直接返回；写入 weight 并清零派生缓存，所有权不变。
 * 注意事项：不是原子 read-modify-write，和并发 PELT/排队更新竞争时必须由 rq 锁
 * 或上层协议串行化。
 */
static inline void update_load_set(struct load_weight *lw, unsigned long w)
{
	lw->weight = w;
	lw->inv_weight = 0;
}

/*
 * Increase the granularity value when there are more CPUs,
 * because with more CPUs the 'effective latency' as visible
 * to users decreases. But the relationship is not linear,
 * so pick a second-best guess by going with the log2 of the
 * number of CPUs.
 *
 * This idea comes from the SD scheduler of Con Kolivas:
 */
/* 在线 CPU 数最多按 8 计入缩放，避免大机器把交互粒度无限放大。 */
/*
 * get_update_sysctl_factor() - 把用户选择的缩放策略转换为当前在线 CPU 的倍率。
 *
 * 业务背景：sysctl 基准 slice 要随并行度调整以保持可感知延迟；此 helper 是
 * update_sysctl() 与 sched_update_scaling() 的共同策略点。
 * 入参：无显式参数；读取 sysctl_sched_tunable_scaling 和在线 CPU 快照，均为借用
 * 的全局状态。
 * 出参/返回：返回至少为 1 的无单位倍率；NONE、LINEAR、LOG 分别代表固定、线性和
 * 对数策略，未知值按 LOG 处理。
 * 注意事项：在线 CPU 数被截到 8，避免极大机器膨胀时间片；读取不建立热插拔稳定
 * 引用，倍率只是允许短暂过时的调优快照，不能用于对象生命周期判断。
 */
static unsigned int get_update_sysctl_factor(void)
{
	unsigned int cpus = min_t(unsigned int, num_online_cpus(), 8);
	unsigned int factor;

	switch (sysctl_sched_tunable_scaling) {
	case SCHED_TUNABLESCALING_NONE:
		factor = 1;
		break;
	case SCHED_TUNABLESCALING_LINEAR:
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		factor = cpus;
		break;
	case SCHED_TUNABLESCALING_LOG:
	default:
		factor = 1 + ilog2(cpus);
		break;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}

	return factor;
}

/*
 * update_sysctl() - 用当前缩放倍率重建实际生效的 fair 时间参数。
 *
 * 业务背景：启动和 CPU 拓扑变化后的调度粒度必须从“归一化基值”派生，避免多次缩放
 * 累积误差；调用链为 sched_init_granularity()/sched_update_scaling() → 本函数。
 * 入参：无；读取归一化 sysctl 基值及 get_update_sysctl_factor() 的瞬时倍率。
 * 出参/返回：无直接返回；更新 sysctl_sched_base_slice，所有全局存储仍由本文件持有。
 * 注意事项：SET_SYSCTL 是预处理宏，当前仅展开一个字段；更新协议由上层 sysctl/
 * 热插拔路径保证，读者不可把这里当成锁保护或逐字段事务提交。
 */
static void update_sysctl(void)
{
	unsigned int factor = get_update_sysctl_factor();

#define SET_SYSCTL(name) \
	(sysctl_##name = (factor) * normalized_sysctl_##name)
	SET_SYSCTL(sched_base_slice);
#undef SET_SYSCTL
}

/*
 * sched_init_granularity() - 在调度器初始化时安装与机器规模匹配的基础 slice。
 *
 * 业务背景：调度初始化链在运行队列开始服务普通任务前调用它；它把默认的归一化参数
 * 转为本机可用值，后续 EEVDF deadline 计算以此为基准。
 * 入参：无。
 * 出参/返回：无直接返回和输出参数；副作用是调用 update_sysctl() 更新全局实际参数。
 * 注意事项：__init 表示初始化后代码可被释放；只能在启动上下文使用，不可从运行时
 * 调度路径保存或调用其地址。
 */
void __init sched_init_granularity(void)
{
	update_sysctl();
}

#ifndef CONFIG_64BIT
#define WMULT_CONST	(~0U)
#define WMULT_SHIFT	32

/*
 * __update_inv_weight() - 惰性生成 weight 的定点倒数缓存。
 *
 * 业务背景：32 位路径用乘倒数替代昂贵且可能溢出的除法；__calc_delta() 在真正需要
 * 比例换算时才调用它，因此入队时不用为未使用的缓存付费。
 * 入参：@lw 是持有实际 weight 与 inv_weight 的借用对象，必须由调用者的 rq 锁或
 * 等价串行化保护；没有输出参数。
 * 出参/返回：无直接返回；若缓存为零则按缩放后权重写入倒数，特殊值 1/WMULT_CONST
 * 分别表示过大/零权重的饱和结果。
 * 注意事项：仅在 !CONFIG_64BIT 编译；likely 快速返回依赖更新权重的路径先清零缓存，
 * 否则旧倒数会使 vruntime 计算失真。
 */
static void __update_inv_weight(struct load_weight *lw)
{
	unsigned long w;

	if (likely(lw->inv_weight))
		return;

	w = scale_load_down(lw->weight);

	if (BITS_PER_LONG > 32 && unlikely(w >= WMULT_CONST))
		lw->inv_weight = 1;
	else if (unlikely(!w))
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		lw->inv_weight = WMULT_CONST;
	else
		lw->inv_weight = WMULT_CONST / w;
}

/*
 * delta_exec * weight / lw.weight
 *   OR
 * (delta_exec * (weight * lw->inv_weight)) >> WMULT_SHIFT
 *
 * Either weight := NICE_0_LOAD and lw \e sched_prio_to_wmult[], in which case
 * we're guaranteed shift stays positive because inv_weight is guaranteed to
 * fit 32 bits, and NICE_0_LOAD gives another 10 bits; therefore shift >= 22.
 *
 * Or, weight =< lw.weight (because lw.weight is the runqueue weight), thus
 * weight/lw.weight <= 1, and therefore our shift will also be positive.
 */
/* 用缓存倒数计算 delta_exec*weight/lw，分段移位避免中间乘法溢出。 */
/*
 * __calc_delta() - 将实际执行时间按实体/队列权重折算为虚拟时间增量。
 *
 * 业务背景：calc_delta_fair() 用此换算让低权重实体的 vruntime 增长更快，从而在
 * EEVDF 树中较早让出 CPU；64 位和 32 位实现保持相同数学契约。
 * 入参：@delta_exec 是非负纳秒执行时长；@weight 是分子权重；@lw 是分母权重及缓存
 * 的借用指针，不能为 NULL，所有权不变。
 * 出参/返回：返回截断后的 u64 虚拟时长；无额外对象副作用，32 位版本可填充缓存。
 * 注意事项：32 位实现按高位动态缩小乘数再右移，避免中间值溢出；调用者必须在同一
 * 权重快照内使用返回值，不能跨越并发 reweight 后把它与新权重混合。
 */
static u64 __calc_delta(u64 delta_exec, unsigned long weight, struct load_weight *lw)
{
	u64 fact = scale_load_down(weight);
	u32 fact_hi = (u32)(fact >> 32);
	int shift = WMULT_SHIFT;
	int fs;

	__update_inv_weight(lw);

	if (unlikely(fact_hi)) {
		fs = fls(fact_hi);
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		shift -= fs;
		fact >>= fs;
	}

	fact = mul_u32_u32(fact, lw->inv_weight);

	fact_hi = (u32)(fact >> 32);
	if (fact_hi) {
		fs = fls(fact_hi);
		shift -= fs;
		fact >>= fs;
	}

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return mul_u64_u32_shr(delta_exec, fact, shift);
}
#else
static u64 __calc_delta(u64 delta_exec, unsigned long weight, struct load_weight *lw)
{
	return (delta_exec * weight) / lw->weight;
}
#endif

/*
 * delta /= w
 */
/*
 * calc_delta_fair() - 把 wall-clock 增量转换为一个实体应累积的 vruntime。
 *
 * 业务背景：update_se()/update_curr() 通过它落实 nice 权重的公平份额；NICE_0_LOAD
 * 是基准权重，命中时直接返回以避免热路径除法。
 * 入参：@delta 是非负纳秒时长；@se 是当前仍有效的 sched_entity 借用指针，其 load
 * 由调用者持有 rq 锁保护。
 * 出参/返回：返回 u64 虚拟时长；不修改 @se，也不取得引用或所有权。
 * 注意事项：仅权重不等于 NICE_0_LOAD 才调用 __calc_delta()；传入已过期或已脱离
 * cfs_rq 的实体会把不一致权重带入 deadline/lag 计算。
 */
static inline u64 calc_delta_fair(u64 delta, struct sched_entity *se)
{
	if (unlikely(se->load.weight != NICE_0_LOAD))
		delta = __calc_delta(delta, NICE_0_LOAD, &se->load);

	return delta;
}

const struct sched_class fair_sched_class;

/**************************************************************
 * CFS operations on generic schedulable entities:
 */
/* 以下分隔段组织公平调度的基础实体辅助函数。 */

#ifdef CONFIG_FAIR_GROUP_SCHED

/* Walk up scheduling entities hierarchy */
#define for_each_sched_entity(se) \
		for (; se; se = se->parent)

/* 将有任务的叶 cfs_rq 按层级顺序接入 CPU 列表，返回是否仍需向父层传播。 */
/*
 * list_add_leaf_cfs_rq() - 将叶队列接到本 CPU 的层级遍历链。
 *
 * 业务背景：组调度的 enqueue 路径从叶向根传播；带任务的叶队列必须按子先父后
 * 的顺序出现在 rq->leaf_cfs_rq_list，供带宽和负载遍历使用。
 * 入参：@cfs_rq 是本 CPU 上、在 rq 锁保护下的借用队列；不得为 NULL。
 * 出参/返回：true 表示该分支已连到父树或根；false 表示父队列仍要继续入链。
 * 注意事项：list_add_rcu 只发布链表指针，不替代 rq 锁；on_list 与
 * tmp_alone_branch 必须成对维护，否则遍历会漏掉或错排子树。
 */
static inline bool list_add_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	int cpu = cpu_of(rq);

	if (cfs_rq->on_list)
		return rq->tmp_alone_branch == &rq->leaf_cfs_rq_list;

	cfs_rq->on_list = 1;

	/*
	 * Ensure we either appear before our parent (if already
	 * enqueued) or force our parent to appear after us when it is
	 * enqueued. The fact that we always enqueue bottom-up
	 * reduces this to two cases and a special case for the root
	 * cfs_rq. Furthermore, it also means that we will always reset
	 * tmp_alone_branch either when the branch is connected
	 * to a tree or when we reach the top of the tree
	 */
	/*
	 * 自底向上入链把排序问题限定为三种情形：父已存在、当前就是根、父尚未
	 * 存在。tmp_alone_branch 始终指向孤立分支的开头；分支接树或到根时复位。
	 */
	if (cfs_rq->tg->parent &&
	    tg_cfs_rq(cfs_rq->tg->parent, cpu)->on_list) {
		/*
		 * If parent is already on the list, we add the child
		 * just before. Thanks to circular linked property of
		 * the list, this means to put the child at the tail
		 * of the list that starts by parent.
		 */
		/* 父已入链时把 child 放在父节点前，环形链表语义使其成为该父分支尾部。 */
		list_add_tail_rcu(&cfs_rq->leaf_cfs_rq_list,
			&(tg_cfs_rq(cfs_rq->tg->parent, cpu)->leaf_cfs_rq_list));
		/*
		 * The branch is now connected to its tree so we can
		 * reset tmp_alone_branch to the beginning of the
		 * list.
		 */
		/* 子分支已与完整树连接；下次孤立分支从全局链表头重新开始。 */
		rq->tmp_alone_branch = &rq->leaf_cfs_rq_list;
		return true;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	if (!cfs_rq->tg->parent) {
		/*
		 * cfs rq without parent should be put
		 * at the tail of the list.
		 */
		/* 根 cfs_rq 没有父实体，直接挂在 CPU 链表尾部完成本树发布。 */
		list_add_tail_rcu(&cfs_rq->leaf_cfs_rq_list,
			&rq->leaf_cfs_rq_list);
		/*
		 * We have reach the top of a tree so we can reset
		 * tmp_alone_branch to the beginning of the list.
		 */
		/* 到达根同样结束孤立分支，避免后续 enqueue 复用陈旧锚点。 */
		rq->tmp_alone_branch = &rq->leaf_cfs_rq_list;
		return true;
	}

	/*
	 * The parent has not already been added so we want to
	 * make sure that it will be put after us.
	 * tmp_alone_branch points to the begin of the branch
	 * where we will add parent.
	 */
	/* 父尚未入链：先把 child 插在孤立分支开头，之后父入链时自然排在它之后。 */
	list_add_rcu(&cfs_rq->leaf_cfs_rq_list, rq->tmp_alone_branch);
	/*
	 * update tmp_alone_branch to points to the new begin
	 * of the branch
	 */
	/* 新 child 成为等待父节点连接的分支头，返回 false 驱动上层继续传播。 */
	rq->tmp_alone_branch = &cfs_rq->leaf_cfs_rq_list;
	return false;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}

/*
 * list_del_leaf_cfs_rq() - 从叶队列遍历链摘除一个已不活跃的 cfs_rq。
 *
 * 业务背景：dequeue、throttle 和组销毁会令叶队列失去可运行实体；必须先从
 * 遍历链摘除，避免后续统计将空队列当作可服务分支。
 * 入参：@cfs_rq 是 rq 锁保护的借用队列，on_list 指示是否拥有一个链表节点。
 * 出参/返回：无直接返回；成功时节点不可再被遍历且 on_list 清零，所有权不变。
 * 注意事项：若 tmp_alone_branch 指向该节点，必须先后移；否则后续 enqueue 会
 * 通过已经摘除的锚点插入，破坏层级顺序。
 */
static inline void list_del_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
	if (cfs_rq->on_list) {
		struct rq *rq = rq_of(cfs_rq);

		/*
		 * With cfs_rq being unthrottled/throttled during an enqueue,
		 * it can happen the tmp_alone_branch points to the leaf that
		 * we finally want to delete. In this case, tmp_alone_branch moves
		 * to the prev element but it will point to rq->leaf_cfs_rq_list
		 * at the end of the enqueue.
		 */
		/*
		 * throttle/unthrottle 可与传播交织；删除当前锚点前先回退到前驱，
		 * 入队完成后锚点仍会回到全局链表头。
		 */
		if (rq->tmp_alone_branch == &cfs_rq->leaf_cfs_rq_list)
			rq->tmp_alone_branch = cfs_rq->leaf_cfs_rq_list.prev;

		list_del_rcu(&cfs_rq->leaf_cfs_rq_list);
		cfs_rq->on_list = 0;
	}
}

/*
 * assert_list_leaf_cfs_rq() - 断言叶队列链不存在未连接的孤立分支。
 *
 * 业务背景：每轮层级 enqueue 完成后，tmp_alone_branch 必须回到链表头；这条
 * 调试断言捕获遗漏父传播或异常 throttle 交错。
 * 入参：@rq 是当前 CPU 的运行队列借用指针，调用者已处于其同步域。
 * 出参/返回：无；不变量破坏时 WARN_ON_ONCE 记录一次警告。
 * 注意事项：这是诊断而非修复；生产路径仍须保持 list_add/list_del 的协议正确。
 */
static inline void assert_list_leaf_cfs_rq(struct rq *rq)
{
	WARN_ON_ONCE(rq->tmp_alone_branch != &rq->leaf_cfs_rq_list);
}

/* Iterate through all leaf cfs_rq's on a runqueue */
/* 遍历一个 runqueue 上的全部叶 cfs_rq。 */
#define for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos)			\
	list_for_each_entry_safe(cfs_rq, pos, &rq->leaf_cfs_rq_list,	\
				 leaf_cfs_rq_list)

/* Do the two (enqueued) entities belong to the same group ? */
/* 两个已入队实体只有共享同一 cfs_rq 时才能直接进行同层 vruntime 比较。 */
/*
 * is_same_group() - 判断两个实体是否已投影到同一层 cfs_rq。
 *
 * 业务背景：抢占比较只在同级兄弟实体间有意义；不同 task_group 的实体需先上溯。
 * 入参：@se 与 @pse 是已入队实体的借用指针，调用者保证它们仍在层级树内。
 * 出参/返回：同组时返回共同 cfs_rq 借用指针，否则返回 NULL；不改变所有权。
 * 注意事项：返回指针只在 rq 锁和实体排队关系有效期间可用，不能跨越 dequeue 保存。
 */
static inline struct cfs_rq *
is_same_group(struct sched_entity *se, struct sched_entity *pse)
{
	if (se->cfs_rq == pse->cfs_rq)
		return se->cfs_rq;

	return NULL;
}

/*
 * parent_entity() - 返回组调度层级中的直接父实体。
 *
 * 业务背景：find_matching_se() 借此把不同层的 task/group 实体提升到同一比较层。
 * 入参：@se 是层级树中有效实体的借用指针；根实体的 parent 为 NULL。
 * 出参/返回：返回借用的父实体或 NULL，不获取引用也不改变层级。
 * 注意事项：调用者必须在层级关系受 rq 锁保护时使用返回值；退出/迁组后不可沿旧链。
 */
static inline struct sched_entity *parent_entity(const struct sched_entity *se)
{
	return se->parent;
}

/*
 * find_matching_se() - 原地上溯两个实体，使它们成为同一 cfs_rq 的兄弟。
 *
 * 业务背景：check_preempt_wakeup 等调用者传入的 task 可能位于不同组深度；只有
 * 对齐后才能比较 EEVDF deadline，避免跨层直接比较造成错误抢占。
 * 入参：@se、@pse 是输入输出双指针；初始指向已入队实体，返回时改为共同父下的
 * 两个借用实体，不转移 task/group 的所有权。
 * 出参/返回：无直接返回；两个指针被改写为可比较层级，后续由调用者继续比较。
 * 注意事项：要求两者在同一组树且调用期间持有对应 rq 锁；若迁组并发改变 parent，
 * 裸指针上溯可能越过失效关系，故不能脱离调度同步域调用。
 */
static void
find_matching_se(struct sched_entity **se, struct sched_entity **pse)
{
	int se_depth, pse_depth;

	/*
	 * preemption test can be made between sibling entities who are in the
	 * same cfs_rq i.e who have a common parent. Walk up the hierarchy of
	 * both tasks until we find their ancestors who are siblings of common
	 * parent.
	 */
	/* 抢占只比较同一 cfs_rq 的兄弟；先消除深度差，再同时上溯到共同父层。 */

	/* First walk up until both entities are at same depth */
	/* 第一阶段只提升较深一侧，避免两个指针错过其最近共同层。 */
	se_depth = (*se)->depth;
	pse_depth = (*pse)->depth;

	while (se_depth > pse_depth) {
		se_depth--;
		*se = parent_entity(*se);
	}

	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
	while (pse_depth > se_depth) {
		pse_depth--;
		*pse = parent_entity(*pse);
	}

	while (!is_same_group(*se, *pse)) {
		*se = parent_entity(*se);
		*pse = parent_entity(*pse);
	}
}

/*
 * tg_is_idle() - 读取 task_group 的 idle 策略标记。
 *
 * 业务背景：idle group 与普通组的负载/选择规则不同，此包装统一调用点。
 * 入参：@tg 是有效 task_group 的借用指针。
 * 出参/返回：返回非零表示该组启用 idle 策略；无副作用和所有权变化。
 * 注意事项：标记的并发读写约束由调用者所在的 task-group/rq 协议提供。
 */
static int tg_is_idle(struct task_group *tg)
{
	return tg->idle > 0;
}

/*
 * cfs_rq_is_idle() - 读取某 CPU 上组队列的 idle 状态。
 *
 * 业务背景：组实体入队后，选择器需要知道其底层 cfs_rq 是否属于 idle 组。
 * 入参：@cfs_rq 是当前 CPU 有效队列的借用指针。
 * 出参/返回：返回非零表示 idle；不修改队列，也不延长其生命周期。
 * 注意事项：调用者需在 rq 锁下把此快照用于当前决策，不能跨越迁组/下线复用。
 */
static int cfs_rq_is_idle(struct cfs_rq *cfs_rq)
{
	return cfs_rq->idle > 0;
}

/*
 * se_is_idle() - 将 task 实体或组实体统一映射为 idle 属性。
 *
 * 业务背景：fair 代码同时处理叶 task 与代表子组的 sched_entity；该分派避免
 * 调用者了解实体具体种类。
 * 入参：@se 是有效实体借用指针；task 类型访问其 policy，组类型访问子 cfs_rq。
 * 出参/返回：返回非零表示 idle；不取得 task/cfs_rq 引用，也不改变排队状态。
 * 注意事项：entity_is_task() 决定访问哪种嵌入对象；错误类型判断会把无效字段当作
 * 指针解引用，因此必须仅对受 rq 锁保护的已发布实体调用。
 */
static int se_is_idle(struct sched_entity *se)
{
	if (entity_is_task(se))
		return task_has_idle_policy(task_of(se));
	return cfs_rq_is_idle(group_cfs_rq(se));
}

#else /* !CONFIG_FAIR_GROUP_SCHED: */

/*
 * 关闭组调度时，层级遍历退化为只处理当前 task 实体一次；宏仍保留同一调用形式，
 * 使 enqueue/dequeue 主路径不必为配置差异复制控制流。
 */

#define for_each_sched_entity(se) \
		for (; se; se = NULL)

/*
 * list_add_leaf_cfs_rq() - 在无组调度配置下确认唯一根队列天然可遍历。
 *
 * 业务背景：调用者仍执行统一的叶队列发布协议，但每个 CPU 只有 rq->cfs，因而无需
 * 维护 leaf_cfs_rq_list。
 * 入参：@cfs_rq 是当前 CPU 根队列的借用指针；本 stub 不解引用它。
 * 出参/返回：恒为 true，表示无需继续向不存在的父层传播；无状态副作用。
 * 注意事项：仅在 !CONFIG_FAIR_GROUP_SCHED 编译，调用者仍须保持 rq 锁约束。
 */
static inline bool list_add_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
	return true;
}

/*
 * list_del_leaf_cfs_rq() - 无组调度配置下的叶队列摘除空操作。
 *
 * 业务背景：统一 dequeue 路径会调用此接口；唯一根 cfs_rq 从不加入额外叶链表。
 * 入参：@cfs_rq 是借用的根队列指针，本函数不读取也不改变其所有权。
 * 出参/返回：无直接返回，且没有可观察副作用。
 * 注意事项：仅为配置兼容 stub，不意味着调用者可省略其余出队记账。
 */
static inline void list_del_leaf_cfs_rq(struct cfs_rq *cfs_rq)
{
}

/*
 * assert_list_leaf_cfs_rq() - 无组调度配置下省略不存在的层级链断言。
 *
 * 业务背景：公共调用点在层级传播结束后执行断言；本配置没有孤立分支可检查。
 * 入参：@rq 是借用运行队列，本 stub 不解引用。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：只消除不适用的诊断，不替代 rq 自身的其他一致性检查。
 */
static inline void assert_list_leaf_cfs_rq(struct rq *rq)
{
}

#define for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos)	\
		for (cfs_rq = &rq->cfs, pos = NULL; cfs_rq; cfs_rq = pos)

/*
 * parent_entity() - 无组层级时声明任意 task 实体都没有调度父实体。
 *
 * 业务背景：公共层级代码通过此接口上溯；关闭组调度后 task 直接属于 rq->cfs。
 * 入参：@se 是借用实体，本 stub 不解引用。
 * 出参/返回：恒返回 NULL，不取得引用、不修改实体。
 * 注意事项：仅在 !CONFIG_FAIR_GROUP_SCHED 有效，NULL 是“已到根”而非查找失败。
 */
static inline struct sched_entity *parent_entity(struct sched_entity *se)
{
	return NULL;
}

/*
 * find_matching_se() - 无组调度时保留已处于同一根队列的两个实体。
 *
 * 业务背景：抢占比较使用统一接口对齐层级；本配置中所有 fair task 天然是兄弟。
 * 入参：@se、@pse 是输入输出双指针，均借用；函数不改写它们。
 * 出参/返回：无直接返回，无输出变化和所有权转移。
 * 注意事项：调用者仍须在 rq 锁下保证两个实体排队关系稳定。
 */
static inline void
find_matching_se(struct sched_entity **se, struct sched_entity **pse)
{
}

/*
 * tg_is_idle() - 无组调度时报告 task_group 不具备 idle 组策略。
 *
 * 业务背景：公共策略查询保留该入口以消除调用点条件编译。
 * 入参：@tg 是借用指针，本 stub 不读取。
 * 出参/返回：恒返回 0，无副作用。
 * 注意事项：task 自身的 SCHED_IDLE 仍由 se_is_idle() 判断，不能与 idle group 混淆。
 */
static inline int tg_is_idle(struct task_group *tg)
{
	return 0;
}

/*
 * cfs_rq_is_idle() - 无组调度时报告根 cfs_rq 不是 idle 组队列。
 *
 * 业务背景：根队列混合承载不同 task policy，不能把整个队列标成 idle。
 * 入参：@cfs_rq 是借用指针，本 stub 不解引用。
 * 出参/返回：恒返回 0，无状态或所有权变化。
 * 注意事项：实体级 idle 属性仍需通过 task policy 判断。
 */
static int cfs_rq_is_idle(struct cfs_rq *cfs_rq)
{
	return 0;
}

/*
 * se_is_idle() - 无组调度时直接读取叶 task 的 SCHED_IDLE 策略。
 *
 * 业务背景：此配置的每个 sched_entity 都嵌入 task_struct，无需分派到子 cfs_rq。
 * 入参：@se 是 rq 锁保护下的借用 task 实体，不能为 NULL。
 * 出参/返回：返回非零表示 task 使用 idle policy；不改变引用或排队状态。
 * 注意事项：仅对 task 实体成立；若把组实体传入 task_of() 会产生错误容器转换。
 */
static int se_is_idle(struct sched_entity *se)
{
	return task_has_idle_policy(task_of(se));
}

#endif /* !CONFIG_FAIR_GROUP_SCHED */

static __always_inline
bool account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec);

/**************************************************************
 * Scheduling class tree data structure manipulation methods:
 */
/* 以下区域维护 EEVDF 时间键与增强红黑树：树序决定 deadline，子树聚合值用于剪枝 eligibility 搜索。 */

/* 非法 vruntime 比较运算符会留下未定义符号，使构建在链接期失败而非静默采用错误语义。 */
extern void __BUILD_BUG_vruntime_cmp(void);

/* Use __builtin_strcmp() because of __HAVE_ARCH_STRCMP: */
/* 使用编译器内建字符串比较，避免体系结构自定义 strcmp 阻止常量折叠；CMP_STR 必须是受支持字面量。 */

#define vruntime_cmp(A, CMP_STR, B) ({				\
	int __res = 0;						\
								\
	if (!__builtin_strcmp(CMP_STR, "<")) {			\
		__res = ((s64)((A)-(B)) < 0);			\
	} else if (!__builtin_strcmp(CMP_STR, "<=")) {		\
		__res = ((s64)((A)-(B)) <= 0);			\
	} else if (!__builtin_strcmp(CMP_STR, ">")) {		\
		__res = ((s64)((A)-(B)) > 0);			\
	} else if (!__builtin_strcmp(CMP_STR, ">=")) {		\
		/* 这里计算大于等于的回绕安全结果。 */	\
		__res = ((s64)((A)-(B)) >= 0);			\
	} else {						\
		/* Unknown operator throws linker error: */	\
		/* 未知运算符借未定义符号触发链接失败。 */	\
		__BUILD_BUG_vruntime_cmp();			\
	}							\
								\
	__res;							\
})

/* 与比较宏相同，非法虚拟时间算术在链接期显式失败。 */
extern void __BUILD_BUG_vruntime_op(void);

/* vruntime_op() 以有符号差解释回绕 u64 时间戳；当前只允许减法并由编译期常量选择分支。 */
#define vruntime_op(A, OP_STR, B) ({				\
	s64 __res = 0;						\
								\
	if (!__builtin_strcmp(OP_STR, "-")) {			\
		__res = (s64)((A)-(B));				\
	} else {						\
		/* Unknown operator throws linker error: */	\
		/* 非减法字符串属于构建期 API 误用。 */		\
		__BUILD_BUG_vruntime_op();			\
	}							\
								\
	__res;						\
})

/*
 * max_vruntime() - 在允许 u64 回绕的时间域中选择较晚 vruntime。
 *
 * 业务背景：EEVDF 更新单调边界时不能用普通无符号比较，否则计数回绕会颠倒先后。
 * 入参：@max_vruntime 是当前候选最大值；@vruntime 是待合并值，单位均为虚拟纳秒。
 * 出参/返回：返回按有符号差判断的较晚值；无对象副作用。
 * 注意事项：两值距离必须小于有符号 64 位半区，调度时间差不变量保证该前提。
 */
static inline __maybe_unused u64 max_vruntime(u64 max_vruntime, u64 vruntime)
{
	if (vruntime_cmp(vruntime, ">", max_vruntime))
		max_vruntime = vruntime;

	return max_vruntime;
}

/*
 * min_vruntime() - 在回绕安全的虚拟时间域中选择较早值。
 *
 * 业务背景：运行队列推进最小 vruntime 时需保持时间单调且正确跨越 u64 回绕。
 * 入参：@min_vruntime 是当前下界；@vruntime 是新候选，单位均为虚拟纳秒。
 * 出参/返回：返回较早值，无输出参数或所有权变化。
 * 注意事项：同样依赖两值差不跨越 s64 半区；不可用于任意无界 u64 数据。
 */
static inline __maybe_unused u64 min_vruntime(u64 min_vruntime, u64 vruntime)
{
	if (vruntime_cmp(vruntime, "<", min_vruntime))
		min_vruntime = vruntime;

	return min_vruntime;
}

/*
 * entity_before() - 按 EEVDF virtual deadline 判断实体树序。
 *
 * 业务背景：增强红黑树以 deadline 为主键，插入和候选选择共享此比较关系。
 * 入参：@a、@b 是 rq 锁保护下的借用实体，deadline 单位为虚拟纳秒。
 * 出参/返回：@a deadline 更早时返回 true，否则 false；不修改实体。
 * 注意事项：deadline 相等不再以 vruntime 打破平局，树仍允许等键节点。
 */
static inline bool entity_before(const struct sched_entity *a,
				 const struct sched_entity *b)
{
	/*
	 * Tiebreak on vruntime seems unnecessary since it can
	 * hardly happen.
	 */
	/* deadline 平局极少发生，且红黑树不要求唯一键，因此省略 vruntime 次级排序。 */
	return vruntime_cmp(a->deadline, "<", b->deadline);
}

/*
 * Per avg_vruntime() below, cfs_rq::zero_vruntime is only slightly stale
 * and this value should be no more than two lag bounds. Which puts it in the
 * general order of:
 *
 *	(slice + TICK_NSEC) << NICE_0_LOAD_SHIFT
 *
 * which is around 44 bits in size (on 64bit); that is 20 for
 * NICE_0_LOAD_SHIFT, another 20 for NSEC_PER_MSEC and then a handful for
 * however many msec the actual slice+tick ends up begin.
 *
 * (disregarding the actual divide-by-weight part makes for the worst case
 * weight of 2, which nicely cancels vs the fuzz in zero_vruntime not actually
 * being the zero-lag point).
 */
/*
 * zero_vruntime 只略微滞后于零 lag 点，实体 key 的幅度约受两个 lag 边界限制；即使
 * 计入 slice、tick 和 NICE_0_LOAD_SHIFT，64 位下通常约 44 bit。忽略除权重会把
 * 最坏权重视为 2，正好覆盖 zero_vruntime 近似带来的余量。该界限使下面转为 s64
 * 的差值安全，并允许加权和使用有符号算术。
 */
/* 以 zero_vruntime 为近邻原点计算有符号 key，降低 u64 回绕和溢出风险。 */
/*
 * entity_key() - 计算实体相对运行队列零 lag 原点的有符号虚拟时间偏移。
 *
 * 业务背景：avg_vruntime() 对偏移做加权求和，避免直接乘巨大绝对 u64 vruntime 溢出。
 * 入参：@cfs_rq 提供借用的原点；@se 提供借用实体，两者由同一 rq 锁稳定。
 * 出参/返回：返回虚拟纳秒偏移，可正可负；不修改对象或取得引用。
 * 注意事项：结果只对当前 zero_vruntime 快照有效，不能跨越队列推进长期缓存。
 */
static inline s64 entity_key(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	return vruntime_op(se->vruntime, "-", cfs_rq->zero_vruntime);
}

#define __node_2_se(node) \
	rb_entry((node), struct sched_entity, run_node)
/* __node_2_se() 把嵌入 sched_entity 的红黑树节点还原为所属实体；不增加引用，生命周期由 rq 锁保证。 */

/*
 * Compute virtual time from the per-task service numbers:
 *
 * Fair schedulers conserve lag:
 *
 *   \Sum lag_i = 0
 *
 * Where lag_i is given by:
 *
 *   lag_i = S - s_i = w_i * (V - v_i)
 *
 * Where S is the ideal service time and V is it's virtual time counterpart.
 * Therefore:
 *
 *   \Sum lag_i = 0
 *   \Sum w_i * (V - v_i) = 0
 *   \Sum (w_i * V - w_i * v_i) = 0
 *
 * From which we can solve an expression for V in v_i (which we have in
 * se->vruntime):
 *
 *       \Sum v_i * w_i   \Sum v_i * w_i
 *   V = -------------- = --------------
 *          \Sum w_i            W
 *
 * Specifically, this is the weighted average of all entity virtual runtimes.
 *
 * [[ NOTE: this is only equal to the ideal scheduler under the condition
 *          that join/leave operations happen at lag_i = 0, otherwise the
 *          virtual time has non-contiguous motion equivalent to:
 *
 *	      V +-= lag_i / W
 *
 *	    Also see the comment in place_entity() that deals with this. ]]
 *
 * However, since v_i is u64, and the multiplication could easily overflow
 * transform it into a relative form that uses smaller quantities:
 *
 * Substitute: v_i == (v_i - v0) + v0
 *
 *     \Sum ((v_i - v0) + v0) * w_i   \Sum (v_i - v0) * w_i
 * V = ---------------------------- = --------------------- + v0
 *                  W                            W
 *
 * Which we track using:
 *
 *                    v0 := cfs_rq->zero_vruntime
 * \Sum (v_i - v0) * w_i := cfs_rq->sum_w_vruntime
 *              \Sum w_i := cfs_rq->sum_weight
 *
 * Since zero_vruntime closely tracks the per-task service, these
 * deltas: (v_i - v0), will be in the order of the maximal (virtual) lag
 * induced in the system due to quantisation.
 */
/*
 * fair 调度守恒总 lag：每个实体的 lag_i=w_i*(V-v_i)，全部实体之和为零，因此理想
 * 虚拟时间 V 是 vruntime 的权重平均。实体若以非零 lag 加入、离开或重加权，V 会按
 * lag_i/W 产生不连续移动，place_entity() 会补偿这一点。直接计算 v_i*w_i 容易溢出，
 * 所以用紧随服务进度的 zero_vruntime 作原点，只累计较小的相对 key 与总权重。
 */
/*
 * avg_vruntime_weight() - 按当前防溢出档位缩放参与平均值的实体权重。
 *
 * 业务背景：PARANOID_AVG 在 64 位乘加溢出时提高 sum_shift，并要求后续所有权重采用
 * 同一缩放尺度；普通路径也经此 helper 保持分子分母一致。
 * 入参：@cfs_rq 是借用队列，提供只读 sum_shift；@w 是原始无单位权重。
 * 出参/返回：返回至少为 2 的缩放权重，32 位配置原样返回；无对象副作用。
 * 注意事项：仅缩放平均值内部表示，不改变实体真实 load.weight 或调度份额。
 */
static inline unsigned long avg_vruntime_weight(struct cfs_rq *cfs_rq, unsigned long w)
{
#ifdef CONFIG_64BIT
	if (cfs_rq->sum_shift)
		w = max(2UL, w >> cfs_rq->sum_shift);
#endif
	return w;
}

/*
 * __sum_w_vruntime_add() - 将一个实体的相对 vruntime 加入队列加权和。
 *
 * 业务背景：实体进入 EEVDF 时间线时同步更新 V 的分子和分母，供 eligibility 判断。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是待计入的借用实体。
 * 出参/返回：无直接返回；增加 sum_w_vruntime 与 sum_weight，不改变实体所有权。
 * 注意事项：WARN 检测乘积符号扩展异常；该快速 helper 本身不处理算术溢出。
 */
static inline void
__sum_w_vruntime_add(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	unsigned long weight = avg_vruntime_weight(cfs_rq, se->load.weight);
	s64 w_vruntime, key = entity_key(cfs_rq, se);

	w_vruntime = key * weight;
	WARN_ON_ONCE((w_vruntime >> 63) != (w_vruntime >> 62));

	cfs_rq->sum_w_vruntime += w_vruntime;
	cfs_rq->sum_weight += weight;
}

/*
 * sum_w_vruntime_add_paranoid() - 以检测、降精度和全树重建方式安全加入加权和。
 *
 * 业务背景：启用 PARANOID_AVG 时宁可在罕见溢出上重扫时间线，也不让 V 静默损坏。
 * 入参：@cfs_rq 是持 rq 锁的输入输出队列；@se 是将加入统计的借用实体。
 * 出参/返回：无直接返回；成功累加，溢出时提高 sum_shift、重建已有树后重试。
 * 注意事项：最多缩放十档，超过说明不变量已无法恢复并触发 BUG；重扫不可睡眠。
 */
static void
sum_w_vruntime_add_paranoid(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	unsigned long weight;
	s64 key, tmp;

again:
	weight = avg_vruntime_weight(cfs_rq, se->load.weight);
	key = entity_key(cfs_rq, se);

	if (check_mul_overflow(key, weight, &key))
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		goto overflow;

	if (check_add_overflow(cfs_rq->sum_w_vruntime, key, &tmp))
		goto overflow;

	cfs_rq->sum_w_vruntime = tmp;
	cfs_rq->sum_weight += weight;
	return;

overflow:
	/*
	 * There's gotta be a limit -- if we're still failing at this point
	 * there's really nothing much to be done about things.
	 */
	/* 缩放必须有上限；十次后仍溢出表示加权和已超出设计可恢复范围，只能以 BUG 暴露。 */
	BUG_ON(cfs_rq->sum_shift >= 10);
	cfs_rq->sum_shift++;

	/*
	 * Note: \Sum (k_i * (w_i >> 1)) != (\Sum (k_i * w_i)) >> 1
	 */
	/* 逐项右移权重不等于总和后右移，因此必须清零并按新尺度遍历所有树节点重算。 */
	cfs_rq->sum_w_vruntime = 0;
	cfs_rq->sum_weight = 0;

	for (struct rb_node *node = cfs_rq->tasks_timeline.rb_leftmost;
	     node; node = rb_next(node))
		__sum_w_vruntime_add(cfs_rq, __node_2_se(node));

	goto again;
}

/*
 * sum_w_vruntime_add() - 按调度特性选择快速或带溢出恢复的加权加入。
 *
 * 业务背景：enqueue 时间线需要统一维护虚拟时间统计，调试特性可交换安全性与热路径成本。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是借用的待计入实体。
 * 出参/返回：无直接返回；更新队列加权和，不转移实体所有权。
 * 注意事项：PARANOID_AVG 可能重扫整棵树，关闭时则依赖正常数值边界不溢出。
 */
static void
sum_w_vruntime_add(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (sched_feat(PARANOID_AVG))
		return sum_w_vruntime_add_paranoid(cfs_rq, se);

	__sum_w_vruntime_add(cfs_rq, se);
}

/*
 * sum_w_vruntime_sub() - 从 V 的加权统计中撤销一个实体贡献。
 *
 * 业务背景：dequeue 必须与加入使用相同原点和权重尺度，否则剩余实体 eligibility 会漂移。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是仍按旧权重计入的借用实体。
 * 出参/返回：无直接返回；减少分子和分母，实体所有权与 vruntime 不变。
 * 注意事项：必须恰好与一次 add 配对，并在重加权或修改原点前使用一致快照。
 */
static void
sum_w_vruntime_sub(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	unsigned long weight = avg_vruntime_weight(cfs_rq, se->load.weight);
	s64 key = entity_key(cfs_rq, se);

	cfs_rq->sum_w_vruntime -= key * weight;
	cfs_rq->sum_weight -= weight;
}

/*
 * update_zero_vruntime() - 平移平均值原点并保持所有实体绝对加权和不变。
 *
 * 业务背景：avg_vruntime() 推进 zero_vruntime 后，相对 key 全部减少 delta；无需逐树更新，
 * 只需从分子减去 sum_weight*delta。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@delta 是有符号虚拟纳秒平移量。
 * 出参/返回：无直接返回；同步修改 zero_vruntime 和 sum_w_vruntime。
 * 注意事项：两字段必须作为同一代状态更新，遗漏任一项会改变计算出的绝对 V。
 */
static inline
void update_zero_vruntime(struct cfs_rq *cfs_rq, s64 delta)
{
	/*
	 * v' = v + d ==> sum_w_vruntime' = sum_w_vruntime - d*sum_weight
	 */
	/* 原点增加 d 时每个相对 key 都减少 d，聚合变化正好是 -d*总权重。 */
	cfs_rq->sum_w_vruntime -= cfs_rq->sum_weight * delta;
	cfs_rq->zero_vruntime += delta;
}

/*
 * Specifically: avg_vruntime() + 0 must result in entity_eligible() := true
 * For this to be so, the result of this function must have a left bias.
 *
 * Called in:
 *  - place_entity()      -- before enqueue
 *  - update_entity_lag() -- before dequeue
 *  - update_deadline()   -- slice expiration
 *
 * This means it is one entry 'behind' but that puts it close enough to where
 * the bound on entity_key() is at most two lag bounds.
 */
/*
 * avg_vruntime()+0 必须让实体被判定 eligible，因此除法需向左侧偏置。它在入队前、
 * 出队前和 slice 到期时调用，统计相对时间线会落后一项，但误差仍受两个 lag 上界约束。
 */
/* 返回所有排队实体加当前实体的加权平均 vruntime，左偏取整维持 eligibility。 */
/*
 * avg_vruntime() - 计算并发布当前 cfs_rq 的近似零 lag 虚拟时间 V。
 *
 * 业务背景：place、lag 更新和 deadline 续期需要同一公平原点；函数把树中实体与仍在
 * CPU 上运行的 curr 合并，随后平移 zero_vruntime。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列借用指针。
 * 出参/返回：返回新的绝对虚拟纳秒 V；副作用是同步重基准 zero_vruntime 与加权和。
 * 注意事项：运行但已不在 on_rq 的 curr 不计入；负分子除法向下取整以保持 eligibility。
 */
u64 avg_vruntime(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	long weight = cfs_rq->sum_weight;
	s64 delta = 0;

	if (curr && !curr->on_rq)
		curr = NULL;

	/* 队列有树节点时先取缓存分子，再把尚未放回树中的当前实体临时并入同一尺度。 */
	if (weight) {
		s64 runtime = cfs_rq->sum_w_vruntime;

		if (curr) {
			unsigned long w = avg_vruntime_weight(cfs_rq, curr->load.weight);

			runtime += entity_key(cfs_rq, curr) * w;
			weight += w;
		}

		/* sign flips effective floor / ceiling */
		/* C 的负数除法向零截断；先减 weight-1 把负结果改为向下取整，保证零 lag 左偏。 */
		if (runtime < 0)
			runtime -= (weight - 1);

		delta = div64_long(runtime, weight);
	} else if (curr) {
		/*
		 * When there is but one element, it is the average.
		 */
		/* 只有当前实体时，它自身就是平均值，无需执行除法。 */
		delta = curr->vruntime - cfs_rq->zero_vruntime;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}

	update_zero_vruntime(cfs_rq, delta);

	return cfs_rq->zero_vruntime;
}

static inline u64 cfs_rq_max_slice(struct cfs_rq *cfs_rq);

/*
 * lag_i = S - s_i = w_i * (V - v_i)
 *
 * However, since V is approximated by the weighted average of all entities it
 * is possible -- by addition/removal/reweight to the tree -- to move V around
 * and end up with a larger lag than we started with.
 *
 * Limit this to either double the slice length with a minimum of TICK_NSEC
 * since that is the timing granularity.
 *
 * EEVDF gives the following limit for a steady state system:
 *
 *   -r_max < lag < max(r_max, q)
 */
/*
 * V 是实体加权平均，实体加入、离开或重加权会移动 V，因而观测 lag 可能比初始值更大。
 * 这里用 max_slice+tick 形成可偿还边界；稳态 EEVDF 的理论范围为 -r_max 到
 * max(r_max,q)，钳位避免睡眠任务携带不可兑现的历史信用或债务。
 */
/* 计算应获与实获服务差，并按队列可在有限时间内偿还的范围钳位。 */
/*
 * entity_lag() - 计算并限制实体相对平均虚拟时间的服务差。
 *
 * 业务背景：dequeue 保存 lag、再次 place 时恢复公平位置；限制值防止队列组成变化放大债权。
 * 入参：@cfs_rq 是借用队列；@se 是借用实体；@avruntime 是同锁域计算的虚拟纳秒 V。
 * 出参/返回：返回钳位后的有符号虚拟 lag；不直接修改实体或队列。
 * 注意事项：调用者必须保证 V、slice 和 se->vruntime 属于同一 rq 锁快照。
 */
static s64 entity_lag(struct cfs_rq *cfs_rq, struct sched_entity *se, u64 avruntime)
{
	u64 max_slice = cfs_rq_max_slice(cfs_rq) + TICK_NSEC;
	s64 vlag, limit;

	vlag = avruntime - se->vruntime;
	limit = calc_delta_fair(max_slice, se);

	return clamp(vlag, -limit, limit);
}

/*
 * Delayed dequeue aims to reduce the negative lag of a dequeued task. While
 * updating the lag of an entity, check that negative lag didn't increase
 * during the delayed dequeue period which would be unfair.
 * Similarly, check that the entity didn't gain positive lag when DELAY_ZERO
 * is set.
 *
 * Return true if the vlag has been modified. Specifically:
 *
 *   se->vlag != avg_vruntime() - se->vruntime
 *
 * This can be due to clamping in entity_lag() or clamping due to
 * sched_delayed. Either way, when vlag is modified and the entity is
 * retained, the tree needs to be adjusted.
 */
/*
 * 延迟出队用于减少任务负 lag；更新时不得让这段延迟反而增加欠债，DELAY_ZERO 下也
 * 不允许任务凭延迟获得正 lag。返回 true 表示保存的 vlag 已偏离原始 V-vruntime，
 * 可能来自通用钳位或 delayed 钳位，若实体仍留树中就必须重调树增强信息。
 */
/*
 * update_entity_lag() - 在出队/续期边界保存受公平约束的实体 vlag。
 *
 * 业务背景：实体离开时间线后仍需携带服务债权，供未来 place_entity() 恢复位置。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是仍 on_rq 的借用实体。
 * 出参/返回：写入 se->vlag；若钳位改变了原始 V-vruntime 返回 true，否则 false。
 * 注意事项：sched_delayed 实体额外保持债务单调；违反 on_rq 前提会触发 WARN。
 */
static __always_inline
bool update_entity_lag(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	u64 avruntime = avg_vruntime(cfs_rq);
	s64 vlag = entity_lag(cfs_rq, se, avruntime);

	WARN_ON_ONCE(!se->on_rq);

	if (se->sched_delayed) {
		/* previous vlag < 0 otherwise se would not be delayed */
		/* 能被延迟出队说明旧 vlag 为负；只允许债务向零收敛，不能变得更负。 */
		vlag = max(vlag, se->vlag);
		if (sched_feat(DELAY_ZERO))
			vlag = min(vlag, 0);
	}
	se->vlag = vlag;

	return avruntime - vlag != se->vruntime;
}

/*
 * Entity is eligible once it received less service than it ought to have,
 * eg. lag >= 0.
 *
 * lag_i = S - s_i = w_i*(V - v_i)
 *
 * lag_i >= 0 -> V >= v_i
 *
 *     \Sum (v_i - v0)*w_i
 * V = ------------------- + v0
 *          \Sum w_i
 *
 * lag_i >= 0 -> \Sum (v_i - v0)*w_i >= (v_i - v0)*(\Sum w_i)
 *
 * Note: using 'avg_vruntime() > se->vruntime' is inaccurate due
 *       to the loss in precision caused by the division.
 */
/*
 * 实体获得的服务少于应得份额时 lag>=0，也就是 V>=v_i。直接调用 avg_vruntime() 再
 * 比较会因整数除法丢精度；把不等式交叉相乘，可在保留聚合分子精度的情况下判断。
 */
/*
 * vruntime_eligible() - 判断给定虚拟时间是否不晚于队列公平原点。
 *
 * 业务背景：pick_eevdf() 用它剪掉尚未还清超额服务的实体/子树。
 * 入参：@cfs_rq 是 rq 锁保护的借用队列；@vruntime 是候选虚拟纳秒时间。
 * 出参/返回：eligible 返回非零，否则零；不修改队列或取得引用。
 * 注意事项：当前实体若仍 on_rq 必须临时加入统计；64 位乘法需按架构能力防溢出。
 */
static int vruntime_eligible(struct cfs_rq *cfs_rq, u64 vruntime)
{
	struct sched_entity *curr = cfs_rq->curr;
	s64 key, avg = cfs_rq->sum_w_vruntime;
	long load = cfs_rq->sum_weight;

	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
	if (curr && curr->on_rq) {
		unsigned long weight = avg_vruntime_weight(cfs_rq, curr->load.weight);

		avg += entity_key(cfs_rq, curr) * weight;
		load += weight;
	}

	key = vruntime_op(vruntime, "-", cfs_rq->zero_vruntime);

	/*
	 * The worst case term for @key includes 'NSEC_TICK * NICE_0_LOAD'
	 * and @load obviously includes NICE_0_LOAD. NSEC_TICK is around 24
	 * bits, while NICE_0_LOAD is 20 on 64bit and 10 otherwise.
	 *
	 * This gives that on 64bit the product will be at least 64bit which
	 * overflows s64, while on 32bit it will only be 44bits and should fit
	 * comfortably.
	 */
	/*
	 * key 最坏含 tick 纳秒量级，load 至少含 NICE_0_LOAD；64 位乘积可能越过 s64，
	 * 32 位权重较小则约 44 bit。故 64 位必须使用 int128 或显式溢出处理。
	 */
#ifdef CONFIG_64BIT
#ifdef CONFIG_ARCH_SUPPORTS_INT128
	/* This often results in simpler code than __builtin_mul_overflow(). */
	/* 架构支持时直接提升到 128 位相乘，通常比生成溢出分支更简单。 */
	return avg >= (__int128)key * load;
#else
	s64 rhs;
	/*
	 * On overflow, the sign of key tells us the correct answer: a large
	 * positive key means vruntime >> V, so not eligible; a large negative
	 * key means vruntime << V, so eligible.
	 */
	/* 乘积溢出时 key 的符号已足够决定它远在 V 右侧或左侧，从而安全返回资格结果。 */
	if (check_mul_overflow(key, load, &rhs))
		return key <= 0;

	return avg >= rhs;
#endif
#else /* 32bit */
	return avg >= key * load;
#endif
}

/* lag 非负才有资格参与 EEVDF；判断使用同一 rq 锁下的 V 与实体 vruntime。 */
/*
 * entity_eligible() - 以实体当前 vruntime 查询其 EEVDF 资格。
 *
 * 业务背景：为选择器提供实体形式的薄包装，真正的精确比较由 vruntime_eligible() 完成。
 * 入参：@cfs_rq 与 @se 都是 rq 锁下借用对象，@se 必须属于对应调度层。
 * 出参/返回：返回非零表示 lag 非负、可以竞争 CPU；无副作用和所有权变化。
 * 注意事项：不能用不同队列的平均值判断实体，否则跨层 vruntime 不具可比性。
 */
int entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	return vruntime_eligible(cfs_rq, se->vruntime);
}

/*
 * cfs_rq_min_slice() - 汇总当前实体与时间线子树中的最短请求 slice。
 *
 * 业务背景：RUN_TO_PARITY 保护窗口以竞争者最短请求为上限，避免长请求长期阻挡短请求。
 * 入参：@cfs_rq 是 rq 锁保护的借用队列。
 * 出参/返回：返回最短纳秒 slice；空队列且无当前实体时返回 U64_MAX，无副作用。
 * 注意事项：root->min_slice 来自红黑树增强回调，修改 slice 后必须同步重算增强字段。
 */
static inline u64 cfs_rq_min_slice(struct cfs_rq *cfs_rq)
{
	struct sched_entity *root = __pick_root_entity(cfs_rq);
	struct sched_entity *curr = cfs_rq->curr;
	u64 min_slice = ~0ULL;

	if (curr && curr->on_rq)
		min_slice = curr->slice;

	if (root)
		min_slice = min(min_slice, root->min_slice);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return min_slice;
}

/*
 * cfs_rq_max_slice() - 汇总当前实体与时间线子树中的最长请求 slice。
 *
 * 业务背景：entity_lag() 用最大请求界定队列可能形成和偿还的服务差。
 * 入参：@cfs_rq 是 rq 锁保护的借用队列。
 * 出参/返回：返回最长纳秒 slice；完全为空时返回 0，无状态变化。
 * 注意事项：root->max_slice 是派生缓存，依赖所有插入、删除和重加权路径执行增强更新。
 */
static inline u64 cfs_rq_max_slice(struct cfs_rq *cfs_rq)
{
	struct sched_entity *root = __pick_root_entity(cfs_rq);
	struct sched_entity *curr = cfs_rq->curr;
	u64 max_slice = 0ULL;

	if (curr && curr->on_rq)
		max_slice = curr->slice;

	if (root)
		max_slice = max(max_slice, root->max_slice);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return max_slice;
}

/*
 * __entity_less() - 把红黑树节点比较转换为 sched_entity deadline 比较。
 *
 * 业务背景：rb_add_augmented_cached() 需要节点级回调，EEVDF 树序实际由 entity_before() 定义。
 * 入参：@a、@b 是嵌入有效实体的借用 rb_node，调用期间受 rq 锁保护。
 * 出参/返回：@a 应排在 @b 前时返回 true；不修改节点或引用。
 * 注意事项：container_of 转换要求节点确实来自 sched_entity::run_node。
 */
static inline bool __entity_less(struct rb_node *a, const struct rb_node *b)
{
	return entity_before(__node_2_se(a), __node_2_se(b));
}

/*
 * __min_vruntime_update() - 将一个子树的最小 vruntime 合并到父实体缓存。
 *
 * 业务背景：pick_eevdf() 依靠子树下界剪枝查找 eligible 实体。
 * 入参：@se 是待更新父实体；@node 可为 NULL，是借用的子树根节点。
 * 出参/返回：无直接返回；必要时降低 se->min_vruntime，所有权不变。
 * 注意事项：只做单子树合并，调用者必须先以 se 自身值初始化并覆盖左右子树。
 */
static inline void __min_vruntime_update(struct sched_entity *se, struct rb_node *node)
{
	if (node) {
		struct sched_entity *rse = __node_2_se(node);

		if (vruntime_cmp(se->min_vruntime, ">", rse->min_vruntime))
			se->min_vruntime = rse->min_vruntime;
	}
}

/*
 * __min_slice_update() - 将子树最短 slice 合并到父实体增强缓存。
 *
 * 业务背景：保护窗口需要 O(1) 获得整棵树最短请求，避免调度点线性扫描。
 * 入参：@se 是输入输出父实体；@node 可空，是借用子树根。
 * 出参/返回：无直接返回；必要时降低 se->min_slice。
 * 注意事项：单位为纳秒，结果只在树结构和各实体 slice 未变化时有效。
 */
static inline void __min_slice_update(struct sched_entity *se, struct rb_node *node)
{
	if (node) {
		struct sched_entity *rse = __node_2_se(node);
		if (rse->min_slice < se->min_slice)
			se->min_slice = rse->min_slice;
	}
}

/*
 * __max_slice_update() - 将子树最长 slice 合并到父实体增强缓存。
 *
 * 业务背景：lag 钳位需要队列最大请求，增强字段把查询成本留在树更新路径。
 * 入参：@se 是输入输出父实体；@node 可空，是借用子树根。
 * 出参/返回：无直接返回；必要时提高 se->max_slice。
 * 注意事项：与 min_slice 缓存必须在同一增强回调中维护，防止使用不同代快照。
 */
static inline void __max_slice_update(struct sched_entity *se, struct rb_node *node)
{
	if (node) {
		struct sched_entity *rse = __node_2_se(node);
		if (rse->max_slice > se->max_slice)
			se->max_slice = rse->max_slice;
	}
}

/*
 * se->min_vruntime = min(se->vruntime, {left,right}->min_vruntime)
 */
/* se->min_vruntime 取自身及左右子树最小值；同一回调还重建 min/max slice 聚合。 */
/*
 * min_vruntime_update() - 重算一个红黑树节点的全部 EEVDF 增强字段。
 *
 * 业务背景：树旋转、插入、删除或 slice 更新后，祖先缓存必须与新子树一致。
 * 入参：@se 是输入输出实体；@exit 由增强 API 传入但本实现无需区分退出阶段。
 * 出参/返回：字段与旧值全部相同返回 true，提示传播可提前停止；否则 false。
 * 注意事项：必须在 rq 锁下调用；返回语义是“未变化”而不是操作成功与否。
 */
static inline bool min_vruntime_update(struct sched_entity *se, bool exit)
{
	u64 old_min_vruntime = se->min_vruntime;
	u64 old_min_slice = se->min_slice;
	u64 old_max_slice = se->max_slice;
	struct rb_node *node = &se->run_node;

	se->min_vruntime = se->vruntime;
	__min_vruntime_update(se, node->rb_right);
	__min_vruntime_update(se, node->rb_left);

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	se->min_slice = se->slice;
	__min_slice_update(se, node->rb_right);
	__min_slice_update(se, node->rb_left);

	se->max_slice = se->slice;
	__max_slice_update(se, node->rb_right);
	__max_slice_update(se, node->rb_left);

	return se->min_vruntime == old_min_vruntime &&
	       se->min_slice == old_min_slice &&
	       se->max_slice == old_max_slice;
}

/* 宏生成红黑树 propagate/copy/rotate 回调，把 run_node 映射到上述三个增强字段的重算函数。 */
RB_DECLARE_CALLBACKS(static, min_vruntime_cb, struct sched_entity,
		     run_node, min_vruntime, min_vruntime_update);

/*
 * Enqueue an entity into the rb-tree:
 */
/* 将实体插入红黑树；插入前同步计入虚拟时间统计，树回调再维护 eligibility 与 slice 聚合。 */
/* 链接 deadline 有序红黑树；增广回调同步维护子树 eligibility 和 slice 边界。 */
/*
 * __enqueue_entity() - 发布实体到当前 cfs_rq 的 EEVDF 时间线。
 *
 * 业务背景：enqueue_entity() 完成记账和 placement 后在此执行真正可选择发布。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是尚未在树中的借用实体。
 * 出参/返回：无直接返回；实体贡献加入 V 统计，run_node 入树并初始化增强字段。
 * 注意事项：调用者负责 on_rq 等外围状态；重复插入同一节点会破坏红黑树。
 */
static void __enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	sum_w_vruntime_add(cfs_rq, se);
	se->min_vruntime = se->vruntime;
	se->min_slice = se->slice;
	rb_add_augmented_cached(&se->run_node, &cfs_rq->tasks_timeline,
				__entity_less, &min_vruntime_cb);
}

/*
 * __dequeue_entity() - 从 EEVDF 时间线摘除实体并撤销其虚拟时间贡献。
 *
 * 业务背景：实体离队或成为 curr 时，选择树不应再返回它，加权平均也不能重复计入。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是当前已在树中的借用实体。
 * 出参/返回：无直接返回；run_node 不再可查找，分子和权重减去该实体贡献。
 * 注意事项：先删树再减统计，整个操作由 rq 锁保持原子可见；必须与一次入树配对。
 */
static void __dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	rb_erase_augmented_cached(&se->run_node, &cfs_rq->tasks_timeline,
				  &min_vruntime_cb);
	sum_w_vruntime_sub(cfs_rq, se);
}

/*
 * __pick_root_entity() - 取得 deadline 红黑树的结构根实体。
 *
 * 业务背景：增强聚合值保存在根节点，min/max slice 查询由此 O(1) 读取整树范围。
 * 入参：@cfs_rq 是 rq 锁保护的借用队列。
 * 出参/返回：返回借用根实体；空树返回 NULL，不改变树或获取引用。
 * 注意事项：结构根不等于最早 deadline，也不保证 eligible，不能作为调度选择结果。
 */
struct sched_entity *__pick_root_entity(struct cfs_rq *cfs_rq)
{
	struct rb_node *root = cfs_rq->tasks_timeline.rb_root.rb_node;

	if (!root)
		return NULL;

	return __node_2_se(root);
}

/*
 * __pick_first_entity() - 取得 deadline 树最左侧即最早 deadline 实体。
 *
 * 业务背景：在不考虑 eligibility 时，cached leftmost 提供 O(1) 的最早截止期候选。
 * 入参：@cfs_rq 是 rq 锁保护的借用队列。
 * 出参/返回：返回借用实体，空树返回 NULL；不改变排队状态。
 * 注意事项：最早 deadline 可能尚不 eligible，完整选择必须交给 pick_eevdf()。
 */
struct sched_entity *__pick_first_entity(struct cfs_rq *cfs_rq)
{
	struct rb_node *left = rb_first_cached(&cfs_rq->tasks_timeline);

	if (!left)
		return NULL;

	return __node_2_se(left);
}

/*
 * Set the vruntime up to which an entity can run before looking
 * for another entity to pick.
 * In case of run to parity, we use the shortest slice of the enqueued
 * entities to set the protected period.
 * When run to parity is disabled, we give a minimum quantum to the running
 * entity to ensure progress.
 */
/*
 * 设置实体在重新选人前受保护的 vruntime 上界。RUN_TO_PARITY 打开时用排队实体中最短
 * slice 限制保护期；关闭时仍给当前实体至少一个基础量子，防止反复选择而无法推进。
 */
/*
 * set_protect_slice() - 为新选中的实体建立首次运行保护窗口。
 *
 * 业务背景：EEVDF 可在每个 tick 重选，但短暂保护可避免实体刚运行就被相近候选抖动抢占。
 * 入参：@cfs_rq 是 rq 锁下借用队列；@se 是即将运行的输入输出实体。
 * 出参/返回：无直接返回；写入 se->vprot，不改变 deadline、slice 或所有权。
 * 注意事项：vprot 不超过 deadline；配置决定公平对齐或最小进度语义。
 */
static inline void set_protect_slice(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	u64 slice = normalized_sysctl_sched_base_slice;
	u64 vprot = se->deadline;

	if (sched_feat(RUN_TO_PARITY))
		slice = cfs_rq_min_slice(cfs_rq);

	slice = min(slice, se->slice);
	if (slice != se->slice)
		vprot = min_vruntime(vprot, se->vruntime + calc_delta_fair(slice, se));

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	se->vprot = vprot;
}

/*
 * update_protect_slice() - 当竞争者集合变化时只收紧现有保护窗口。
 *
 * 业务背景：更短 slice 实体入队后，当前任务的旧保护期不能继续阻挡它。
 * 入参：@cfs_rq 是 rq 锁下借用队列；@se 是正在运行的输入输出实体。
 * 出参/返回：无直接返回；vprot 取旧值与新最短 slice 边界的较早者。
 * 注意事项：只允许缩短，不重新延长已消耗的保护；单位为虚拟纳秒。
 */
static inline void update_protect_slice(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	u64 slice = cfs_rq_min_slice(cfs_rq);

	se->vprot = min_vruntime(se->vprot, se->vruntime + calc_delta_fair(slice, se));
}

/*
 * protect_slice() - 判断实体是否仍处于不可被常规重选打断的保护区间。
 *
 * 业务背景：pick_eevdf() 在保护模式下优先保留当前实体直到 vruntime 到达 vprot。
 * 入参：@se 是 rq 锁下借用实体。
 * 出参/返回：vruntime 早于 vprot 返回 true，否则 false；无副作用。
 * 注意事项：使用回绕安全比较，结果只对当前 vruntime 快照有效。
 */
static inline bool protect_slice(struct sched_entity *se)
{
	return vruntime_cmp(se->vruntime, "<", se->vprot);
}

/*
 * cancel_protect_slice() - 提前终止尚未耗尽的实体保护期。
 *
 * 业务背景：dequeue、策略变化等边界不应让旧保护权泄漏到下一轮选择。
 * 入参：@se 是 rq 锁下输入输出实体。
 * 出参/返回：无直接返回；若仍受保护则把 vprot 收到当前 vruntime。
 * 注意事项：已自然过期时保持原值；不改变 deadline 或服务记账。
 */
static inline void cancel_protect_slice(struct sched_entity *se)
{
	if (protect_slice(se))
		se->vprot = se->vruntime;
}

/*
 * Earliest Eligible Virtual Deadline First
 *
 * In order to provide latency guarantees for different request sizes
 * EEVDF selects the best runnable task from two criteria:
 *
 *  1) the task must be eligible (must be owed service)
 *
 *  2) from those tasks that meet 1), we select the one
 *     with the earliest virtual deadline.
 *
 * We can do this in O(log n) time due to an augmented RB-tree. The
 * tree keeps the entries sorted on deadline, but also functions as a
 * heap based on the vruntime by keeping:
 *
 *  se->min_vruntime = min(se->vruntime, se->{left,right}->min_vruntime)
 *
 * Which allows tree pruning through eligibility.
 */
/*
 * EEVDF 先要求实体 lag 非负，再从合格集合选最早虚拟 deadline。红黑树按 deadline
 * 排序，同时以 min_vruntime 形成 eligibility 堆，因此可跳过整棵无资格子树并在
 * O(log n) 内找到候选。
 */
/* 借助增广最小 vruntime 剪枝，在 eligible 实体中以 O(log n) 找最早 deadline。 */
/*
 * pick_eevdf() - 从当前 cfs_rq 选择最早合格虚拟截止期实体。
 *
 * 业务背景：pick_next_entity() 在同一调度层调用它，兼顾服务债权、公平 deadline 与
 * 当前实体的短期保护，最终交付下一运行候选。
 * 入参：@cfs_rq 是 rq 锁保护的借用队列；@protect 表示是否应用 buddy/vprot 延迟保护。
 * 出参/返回：返回借用候选或队列无可运行实体时 NULL；不改变入队状态和所有权。
 * 注意事项：候选只在 rq 锁内稳定；PICK_BUDDY 改善延迟但不改变长期公平性。
 */
static struct sched_entity *pick_eevdf(struct cfs_rq *cfs_rq, bool protect)
{
	struct rb_node *node = cfs_rq->tasks_timeline.rb_root.rb_node;
	struct sched_entity *se = __pick_first_entity(cfs_rq);
	struct sched_entity *curr = cfs_rq->curr;
	struct sched_entity *best = NULL;

	/*
	 * We can safely skip eligibility check if there is only one entity
	 * in this cfs_rq, saving some cycles.
	 */
	/* 只有一个排队实体时不存在相对公平选择，直接在 curr 与树节点中返回唯一对象。 */
	if (cfs_rq->nr_queued == 1)
		return curr && curr->on_rq ? curr : se;

	/*
	 * Picking the ->next buddy will affect latency but not fairness.
	 */
	/* next buddy 只改变谁先获得短期运行机会；资格检查仍确保不会透支长期份额。 */
	if (sched_feat(PICK_BUDDY) && protect &&
	    cfs_rq->next && entity_eligible(cfs_rq, cfs_rq->next)) {
		/* ->next will never be delayed */
		/* delayed 实体不会被设置为 next；警告用于捕获 buddy 与延迟出队协议失配。 */
		WARN_ON_ONCE(cfs_rq->next->sched_delayed);
		return cfs_rq->next;
	}

	if (curr && (!curr->on_rq || !entity_eligible(cfs_rq, curr)))
		curr = NULL;

	if (curr && protect && protect_slice(curr))
		return curr;

	/* Pick the leftmost entity if it's eligible */
	/* 最左节点兼具全树最早 deadline，若已 eligible 就无需继续搜索。 */
	if (se && entity_eligible(cfs_rq, se)) {
		best = se;
		goto found;
	}

	/* Heap search for the EEVD entity */
	/* 最早节点无资格时，借助子树 min_vruntime 自顶向下寻找最早 eligible deadline。 */
	while (node) {
		struct rb_node *left = node->rb_left;

		/*
		 * Eligible entities in left subtree are always better
		 * choices, since they have earlier deadlines.
		 */
		/* 左子树若含 eligible 实体，其 deadline 必早于当前和右侧，优先下降不会漏掉更优解。 */
		if (left && vruntime_eligible(cfs_rq,
					__node_2_se(left)->min_vruntime)) {
			node = left;
			continue;
		}

		se = __node_2_se(node);

		/*
		 * The left subtree either is empty or has no eligible
		 * entity, so check the current node since it is the one
		 * with earliest deadline that might be eligible.
		 */
		/* 左侧已排除后检查当前节点；若仍不合格，只有右子树还可能提供候选。 */
		if (entity_eligible(cfs_rq, se)) {
			best = se;
			break;
		}

		node = node->rb_right;
	}
found:
	/* curr 不在树中；若它仍 eligible 且 deadline 更早，就与树搜索结果做最后合并。 */
	if (!best || (curr && entity_before(curr, best)))
		best = curr;

	return best;
}

/*
 * __pick_last_entity() - 返回 deadline 树中最晚截止期实体。
 *
 * 业务背景：调试和部分策略需要树序另一端，rb_last() 从结构中定位最大键。
 * 入参：@cfs_rq 是 rq 锁保护的借用队列。
 * 出参/返回：返回借用实体，空树返回 NULL；无副作用和引用变化。
 * 注意事项：最晚 deadline 不表达 eligibility，也不是普通调度候选。
 */
struct sched_entity *__pick_last_entity(struct cfs_rq *cfs_rq)
{
	struct rb_node *last = rb_last(&cfs_rq->tasks_timeline.rb_root);

	if (!last)
		return NULL;

	return __node_2_se(last);
}

/**************************************************************
 * Scheduling class statistics methods:
 */
/* 以下函数维护 fair 调度时间参数与 PELT 统计，使策略输入随 CPU 规模和任务生命周期更新。 */
/*
 * sched_update_scaling() - 将当前实际 sysctl 参数反推为归一化基值。
 *
 * 业务背景：管理员修改实际 base_slice 后，后续 CPU 数变化需从新基值重新缩放而非沿旧值累乘。
 * 入参：无；读取在线 CPU 缩放策略和 sysctl_sched_base_slice。
 * 出参/返回：恒返回 0；更新 normalized_sysctl_sched_base_slice 全局值。
 * 注意事项：调用上下文负责串行化 sysctl 与热插拔更新；整数除法会舍弃余数。
 */
int sched_update_scaling(void)
{
	unsigned int factor = get_update_sysctl_factor();

#define WRT_SYSCTL(name) \
	(normalized_sysctl_##name = sysctl_##name / (factor))
	WRT_SYSCTL(sched_base_slice);
#undef WRT_SYSCTL

	return 0;
}

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
static void clear_buddies(struct cfs_rq *cfs_rq, struct sched_entity *se);

/*
 * XXX: strictly: vd_i += N*r_i/w_i such that: vd_i > ve_i
 * this is probably good enough.
 */
/* 严格公式需选择 N 使 vd_i 超过 ve_i；当前按一次请求推进，在调度粒度下是足够近似。 */
/* slice 用尽后按权重推进虚拟 deadline；未到期时保留连续执行资格。 */
/*
 * update_deadline() - 在实体耗尽当前请求后建立下一 EEVDF 虚拟截止期。
 *
 * 业务背景：update_curr()/tick 在 vruntime 到达 deadline 时调用，续期后请求重新参与选择。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是当前输入输出实体。
 * 出参/返回：未到期返回 false且无变化；到期返回 true，更新 slice/deadline 和队列平均 V。
 * 注意事项：非自定义 slice 会跟随全局 sysctl；true 提示调用者应触发重新调度。
 */
static bool update_deadline(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (vruntime_cmp(se->vruntime, "<", se->deadline))
		return false;

	/*
	 * For EEVDF the virtual time slope is determined by w_i (iow.
	 * nice) while the request time r_i is determined by
	 * sysctl_sched_base_slice.
	 */
	/* 权重决定虚拟时间斜率，基础 slice 决定请求时长；自定义请求则保留实体自己的 slice。 */
	if (!se->custom_slice)
		se->slice = sysctl_sched_base_slice;

	/*
	 * EEVDF: vd_i = ve_i + r_i / w_i
	 */
	/* 从当前虚拟执行时间加上按权重折算的请求长度，形成下一 virtual deadline。 */
	se->deadline = se->vruntime + calc_delta_fair(se->slice, se);
	avg_vruntime(cfs_rq);

	/*
	 * The task has consumed its request, reschedule.
	 */
	/* 请求已经完整消费，返回 true 让上层设置 need_resched 并重新比较所有候选。 */
	return true;
}

#include "pelt.h"

static int select_idle_sibling(struct task_struct *p, int prev_cpu, int cpu);
static unsigned long task_h_load(struct task_struct *p);
static unsigned long capacity_of(int cpu);

/* Give new sched_entity start runnable values to heavy its load in infant time */
/* 新实体在统计尚未收敛的初生阶段先采用保守 runnable 初值，避免低估新任务负载。 */
/*
 * init_entity_runnable_average() - 初始化新调度实体的 PELT 历史与启动负载。
 *
 * 业务背景：fork/组实体创建时尚无运行样本，但选核和容量决策不能读取未初始化统计；
 * task 先按完整权重视为重负载，组实体则等待成员贡献。
 * 入参：@se 是尚未附着到 cfs_rq 的输入输出实体，调用者独占且不转移所有权。
 * 出参/返回：无直接返回；清零 se->avg，task 的 load_avg 设为缩放权重。
 * 注意事项：此时不更新队列聚合；后续 enqueue/attach 才把该值发布给 cfs_rq。
 */
void init_entity_runnable_average(struct sched_entity *se)
{
	struct sched_avg *sa = &se->avg;

	memset(sa, 0, sizeof(*sa));

	/*
	 * Tasks are initialized with full load to be seen as heavy tasks until
	 * they get a chance to stabilize to their real load level.
	 * Group entities are initialized with zero load to reflect the fact that
	 * nothing has been attached to the task group yet.
	 */
	/* task 在真实样本稳定前按满负载处理；组实体尚无成员附着，故保持零负载。 */
	if (entity_is_task(se))
		sa->load_avg = scale_load_down(se->load.weight);

	/* when this task is enqueued, it will contribute to its cfs_rq's load_avg */
	/* 当前只构造实体私有统计，入队时才把它累计进所属 cfs_rq 的 load_avg。 */
}

/*
 * With new tasks being created, their initial util_avgs are extrapolated
 * based on the cfs_rq's current util_avg:
 *
 *   util_avg = cfs_rq->avg.util_avg / (cfs_rq->avg.load_avg + 1)
 *		* se_weight(se)
 *
 * However, in many cases, the above util_avg does not give a desired
 * value. Moreover, the sum of the util_avgs may be divergent, such
 * as when the series is a harmonic series.
 *
 * To solve this problem, we also cap the util_avg of successive tasks to
 * only 1/2 of the left utilization budget:
 *
 *   util_avg_cap = (cpu_scale - cfs_rq->avg.util_avg) / 2^n
 *
 * where n denotes the nth task and cpu_scale the CPU capacity.
 *
 * For example, for a CPU with 1024 of capacity, a simplest series from
 * the beginning would be like:
 *
 *  task  util_avg: 512, 256, 128,  64,  32,   16,    8, ...
 * cfs_rq util_avg: 512, 768, 896, 960, 992, 1008, 1016, ...
 *
 * Finally, that extrapolated util_avg is clamped to the cap (util_avg_cap)
 * if util_avg > util_avg_cap.
 */
/*
 * 新任务的初始 util_avg 按当前队列 util/load 比例外推，但连续创建会形成可能发散的
 * 级数。因此第 n 个任务最多领取剩余 CPU 容量的 1/2^n：容量 1024 时依次为
 * 512、256、128……，既避免把新任务估成零，也不让预测总和越过 CPU 能力。
 */
/*
 * post_init_entity_util_avg() - 根据目标 cfs_rq 为新 task 建立有界利用率初值。
 *
 * 业务背景：任务刚创建或切换到 fair 前缺少 PELT 运行历史；此函数在附着队列前用
 * 队列现状估算 util，改善首次选核和频率决策。
 * 入参：@p 是调用者持有的输入输出 task；函数借用其 se、cfs_rq 和 CPU 容量。
 * 出参/返回：无直接返回；写入 se->avg util/runnable 或非 fair 情况的更新时间。
 * 注意事项：调用者需稳定任务所属 rq；估值只是启动先验，后续 PELT 样本会逐步替代。
 */
void post_init_entity_util_avg(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	struct sched_avg *sa = &se->avg;
	long cpu_scale = arch_scale_cpu_capacity(cpu_of(rq_of(cfs_rq)));
	long cap = (long)(cpu_scale - cfs_rq->avg.util_avg) / 2;

	if (p->sched_class != &fair_sched_class) {
		/*
		 * For !fair tasks do:
		 *
		update_cfs_rq_load_avg(now, cfs_rq);
		attach_entity_load_avg(cfs_rq, se);
		switched_from_fair(rq, p);
		 *
		 * such that the next switched_to_fair() has the
		 * expected state.
		 */
		/*
		 * 非 fair task 暂不附着统计，只把时间基准对齐 cfs_rq；未来 switched_to_fair()
		 * 会按“更新队列→附着实体→完成类切换”的顺序获得预期状态。
		 */
		se->avg.last_update_time = cfs_rq_clock_pelt(cfs_rq);
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
		return;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}

	if (cap > 0) {
		/* 队列已有样本时按 util/load 比例外推，再用剩余容量的一半封顶。 */
		if (cfs_rq->avg.util_avg != 0) {
			sa->util_avg  = cfs_rq->avg.util_avg * se_weight(se);
			sa->util_avg /= (cfs_rq->avg.load_avg + 1);

			if (sa->util_avg > cap)
				sa->util_avg = cap;
		} else {
			/* 队列尚无 load 样本时比例不可用，直接领取本轮 cap 作为保守初值。 */
			sa->util_avg = cap;
		}
	}

	sa->runnable_avg = sa->util_avg;
	/* 新任务尚未产生阻塞差异，初始 runnable 与 util 使用同一估值。 */
}

static inline void account_mm_sched(struct rq *rq, struct task_struct *p, s64 delta_exec);

/* 结算本次执行并推进 vruntime、deadline、统计与 cache 记账，返回实际执行量。 */
/*
 * update_se() - 结算当前 sched_entity 自 exec_start 以来的实际 CPU 执行时间。
 *
 * 业务背景：update_curr() 在 tick、切换和出队边界调用它，把时钟增量分配给真实
 * running task、proxy-exec donor、cgroup、mm cache 与 schedstat。
 * 入参：@rq 是锁定的运行队列；@se 是被记账的输入输出实体，二者均为借用对象。
 * 出参/返回：返回有符号纳秒增量；非正增量原样返回，否则推进各累计字段和 exec_start。
 * 注意事项：proxy-exec 下 @se 对应 donor 而 rq->curr 是实际执行者，两套统计不可混写。
 */
static s64 update_se(struct rq *rq, struct sched_entity *se)
{
	u64 now = rq_clock_task(rq);
	s64 delta_exec;

	delta_exec = now - se->exec_start;
	/* 时钟未前进或因校正倒退时不更新任何累计量，避免把负值转成巨大无符号时间。 */
	if (unlikely(delta_exec <= 0))
		return delta_exec;

	se->exec_start = now;
	if (entity_is_task(se)) {
		struct task_struct *donor = task_of(se);
		struct task_struct *running = rq->curr;
		/*
		 * If se is a task, we account the time against the running
		 * task, as w/ proxy-exec they may not be the same.
		 */
		/* proxy-exec 可能由 donor 决定调度、由 running 实际占 CPU；运行时记给后者。 */
		running->se.exec_start = now;
		running->se.sum_exec_runtime += delta_exec;

		trace_sched_stat_runtime(running, delta_exec);
		account_group_exec_runtime(running, delta_exec);
		account_mm_sched(rq, running, delta_exec);

		/* cgroup time is always accounted against the donor */
		/* cgroup 资源归属始终跟 donor，而非可能代其执行的 rq->curr。 */
		cgroup_account_cputime(donor, delta_exec);
	} else {
		/* If not task, account the time against donor se  */
		/* 组实体没有独立 task 容器，只累计本层 donor se 的层级运行时间。 */
		se->sum_exec_runtime += delta_exec;
	}

	if (schedstat_enabled()) {
		/* 可选 schedstat 记录单次最长执行段；静态键关闭时避免热路径额外写入。 */
		struct sched_statistics *stats;

		stats = __schedstats_from_se(se);
		__schedstat_set(stats->exec_max,
				max(delta_exec, stats->exec_max));
	}

	return delta_exec;
}

static void set_next_buddy(struct sched_entity *se);

#ifdef CONFIG_SCHED_CACHE

/*
 * XXX numbers come from a place the sun don't shine -- probably wants to be SD
 * tunable or so.
 */
/* 这些经验常量缺少严格模型，后续更适合成为 sched_domain 可调项；当前仅描述实现而不宣称最优。 */
#define EPOCH_PERIOD	(HZ / 100)	/* 10 ms */
#define EPOCH_LLC_AFFINITY_TIMEOUT	5	/* 50 ms */
/*
 * cache 聚合策略的只读热路径参数：epoch 以 tick/轮次计时，百分比控制容量容忍度、
 * 不平衡阈值与过度聚合判定；sysctl 写侧更新，调度路径通过 READ_ONCE 取快照。
 */
__read_mostly unsigned int llc_aggr_tolerance	= 1;
__read_mostly unsigned int llc_epoch_period	= EPOCH_PERIOD;
__read_mostly unsigned int llc_epoch_affinity_timeout = EPOCH_LLC_AFFINITY_TIMEOUT;
__read_mostly unsigned int llc_imb_pct		= 20;
__read_mostly unsigned int llc_overaggr_pct	= 50;

/*
 * llc_id() - 将逻辑 CPU 映射到调度域记录的 LLC 标识。
 *
 * 业务背景：cache 聚合以共享末级缓存为放置单元，需要把 CPU 选择转换为 LLC 身份。
 * 入参：@cpu 是逻辑 CPU 编号；负值表示没有有效 CPU。
 * 出参/返回：负输入返回 -1，否则返回该 CPU 的 per-CPU sd_llc_id；无副作用。
 * 注意事项：CPU 热插拔可重建拓扑，结果是瞬时快照，不能作为长期对象引用。
 */
static int llc_id(int cpu)
{
	if (cpu < 0)
		return -1;

	return per_cpu(sd_llc_id, cpu);
}

/*
 * get_sched_cache_scale() - 把 LLC 聚合容忍百分比转换为比较倍率。
 *
 * 业务背景：容量和线程数判定共享同一管理员策略，但各自以不同基础乘数缩放。
 * 入参：@mul 是调用场景的无单位基础倍率；读取全局 llc_aggr_tolerance 快照。
 * 出参/返回：容忍度 0 返回 0，>=100 返回 INT_MAX 表示无条件聚合，否则返回线性倍率。
 * 注意事项：READ_ONCE 只保证单次值不撕裂，不把一组 sysctl 更新变成事务。
 */
static inline int get_sched_cache_scale(int mul)
{
	unsigned int tol = READ_ONCE(llc_aggr_tolerance);

	if (!tol)
		return 0;

	if (tol >= 100)
		return INT_MAX;

	return (1 + (tol - 1) * mul);
}

/*
 * exceed_llc_capacity() - 判断 mm 工作集是否超过目标 CPU 所属 LLC 的容忍容量。
 *
 * 业务背景：cache-aware 放置只应聚合能受益于共享 LLC 的地址空间；过大 footprint 会
 * 互相驱逐缓存，选择器据此放弃强制聚合。
 * 入参：@mm 是借用地址空间；@cpu 是在线候选 CPU，函数借用其 sched_domain。
 * 出参/返回：无调度域时保守返回 true；超容返回 true，其余返回 false。
 * 注意事项：RCU guard 只保护 sd 生命周期；footprint 是近似快照，RDT 预留 ways 尚未扣除。
 */
static bool exceed_llc_capacity(struct mm_struct *mm, int cpu)
{
#ifdef CONFIG_NUMA_BALANCING
	unsigned long llc, footprint;
	struct sched_domain *sd;
	int scale;

	guard(rcu)();

	sd = rcu_dereference_sched_domain(cpu_rq(cpu)->sd);
	if (!sd)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return true;

	if (static_branch_likely(&sched_numa_balancing)) {
		/*
		 * TBD: RDT exclusive LLC ways reserved should be
		 * excluded.
		 */
		/* 当前容量模型尚未减去 RDT 独占的 LLC ways，可能高估可供普通任务使用的容量。 */
		llc = sd->llc_bytes;
		footprint = READ_ONCE(mm->sc_stat.footprint);

		/*
		 * Scale the LLC size by 256*llc_aggr_tolerance
		 * and compare it to the task's footprint.
		 *
		 * Suppose the L3 size is 32MB. If the
		 * llc_aggr_tolerance is 1:
		 * When the footprint is larger than 32MB, the
		 * process is regarded as exceeding the LLC
		 * capacity. If the llc_aggr_tolerance is 99:
		 * When the footprint is larger than 784GB, the
		 * process is regarded as exceeding the LLC
		 * capacity:
		 * 784GB = (1 + (99 - 1) * 256) * 32MB
		 * If the llc_aggr_tolerance is 100:
		 * ignore the footprint and do the aggregation
		 * anyway.
		 */
		/*
		 * LLC 字节数乘 256*容忍尺度后与 footprint 页数换算的字节量比较；容忍 100
		 * 直接忽略 footprint，而不是继续做可能溢出的巨大乘法。
		 */
		scale = get_sched_cache_scale(256);
		if (scale == INT_MAX)
			return false;

		return ((llc * (u64)scale) < (footprint * PAGE_SIZE));
	}
#endif
	return false;
}

/*
 * invalid_llc_nr() - 判断线程规模是否不值得或无法集中到目标 LLC。
 *
 * 业务背景：单线程没有跨 CPU cache 聚合收益；活跃线程超过容忍后的 LLC 核心容量时
 * 强行集中也会排队和过度竞争。
 * 入参：@mm 提供近似活跃线程统计；@p 提供线程组规模；@cpu 提供目标 LLC 拓扑容量。
 * 出参/返回：单线程或容量不匹配返回 true；容忍度 100 时恒 false；无副作用。
 * 注意事项：统计与热插拔均可短暂过时，只用于放置启发式而非正确性决策。
 */
static bool invalid_llc_nr(struct mm_struct *mm, struct task_struct *p,
			   int cpu)
{
	int scale;

	if (get_nr_threads(p) <= 1)
		return true;

	/*
	 * Scale the number of 'cores' in a LLC by llc_aggr_tolerance
	 * and compare it to the task's active threads.
	 */
	/* 按容忍度放大 LLC 核心容量，再与 mm 平均运行线程数换算后的 SMT 需求比较。 */
	scale = get_sched_cache_scale(1);
	if (scale == INT_MAX)
		return false;

	return !fits_capacity((mm->sc_stat.nr_running_avg * cpu_smt_num_threads),
			(scale * per_cpu(sd_llc_size, cpu)));
}

/*
 * account_llc_enqueue() - 将 task 入队事件计入运行队列和目标 LLC 的聚合统计。
 *
 * 业务背景：cache 放置器需要知道任务是否实际排在 preferred_llc，以及每个 LLC 有多少
 * 偏好者；enqueue 在排队状态发布时建立与 dequeue 配对的快照。
 * 入参：@rq 是 rq 锁保护的输入输出运行队列；@p 是正在入队的输入输出 task，均借用。
 * 出参/返回：无直接返回；增加 rq/域计数并保存 p->pref_llc_queued。
 * 注意事项：CPU 热插拔会改变 task_llc()，必须保存入队时结果而不能在出队时重新推导。
 */
static void account_llc_enqueue(struct rq *rq, struct task_struct *p)
{
	int pref_llc, pref_llc_queued;
	struct sched_domain *sd;

	pref_llc = p->preferred_llc;
	if (pref_llc < 0)
		return;

	pref_llc_queued = (pref_llc == task_llc(p));
	rq->nr_llc_running++;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	rq->nr_pref_llc_running += pref_llc_queued;

	/*
	 * Record whether p is enqueued on its preferred
	 * LLC, in order to pair with account_llc_dequeue()
	 * to maintain a consistent nr_pref_llc_running per
	 * runqueue.
	 * This is necessary because a race condition exists:
	 * after a task is enqueued on a runqueue, task_llc(p)
	 * may change due to CPU hotplug. Therefore, checking
	 * task_llc(p) to determine whether the task is being
	 * dequeued from its preferred LLC is unreliable and
	 * can cause inconsistent values - checking the
	 * p->pref_llc_queued in account_llc_dequeue() would
	 * be reliable.
	 */
	/*
	 * 保存入队瞬间是否命中 preferred LLC，供出队原样撤销。若出队时重算 task_llc(p)，
	 * CPU 热插拔可能已改写拓扑映射，导致 nr_pref_llc_running 永久失配。
	 */
	p->pref_llc_queued = pref_llc_queued;

	sd = rcu_dereference_all(rq->sd);
	if (sd && (unsigned int)pref_llc < sd->llc_max)
		sd->llc_counts[pref_llc]++;
}

/*
 * account_llc_dequeue() - 撤销 task 入队时记录的 LLC 运行统计。
 *
 * 业务背景：任务离开 rq 后不能继续影响 cache 聚合负载；本函数与 enqueue 快照严格配对。
 * 入参：@rq 是锁定的输入输出运行队列；@p 是正在出队的输入输出 task。
 * 出参/返回：无直接返回；减少 rq/域计数并清除 pref_llc_queued。
 * 注意事项：热插拔可能把新 sched_domain 计数重置为零，因此域计数下降前必须防下溢。
 */
static void account_llc_dequeue(struct rq *rq, struct task_struct *p)
{
	struct sched_domain *sd;
	int pref_llc;

	pref_llc = p->preferred_llc;
	if (pref_llc < 0)
		return;

	rq->nr_llc_running--;
	if (p->pref_llc_queued) {
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		rq->nr_pref_llc_running--;
		/*
		 * Update the status in case
		 * other logic might query
		 * this.
		 */
		/* 清除配对位，使任务离队后的其他查询不会误认为它仍命中 preferred LLC。 */
		p->pref_llc_queued = 0;
	}

	sd = rcu_dereference_all(rq->sd);
	if (sd && (unsigned int)pref_llc < sd->llc_max) {
		/*
		 * There is a race condition between dequeue
		 * and CPU hotplug. After a task has been enqueued
		 * on CPUx, a CPU hotplug event occurs, and all online
		 * CPUs (including CPUx) rebuild their sched_domains
		 * and reset statistics to zero(including sd->llc_counts).
		 * This can cause temporary undercount and we have to
		 * check for such underflow in sd->llc_counts.
		 *
		 * This undercount is temporary and accurate accounting
		 * will resume once the rq has a chance to be idle.
		 */
		/*
		 * dequeue 可与 CPU 热插拔重建 sched_domain 交错：新域已把 llc_counts 清零，
		 * 旧入队事件却仍来撤销。零值时跳过递减以防无符号下溢；短暂少计会在 rq 空闲
		 * 后通过新一轮记账恢复。
		 */
		if (sd->llc_counts[pref_llc])
			sd->llc_counts[pref_llc]--;
	}
}

/*
 * mm_init_sched() - 初始化地址空间的 per-CPU cache 调度时间与全局 epoch。
 *
 * 业务背景：新 mm 参与 sched-cache 前需从所有可能 CPU 的当前 epoch 建立基线，避免把
 * 创建前运行时间误算为该 mm 的样本。
 * 入参：@mm 是新建的输入输出地址空间；@_pcpu_sched 是其已分配 per-CPU 统计区，所有权不转移。
 * 出参/返回：无直接返回；清零每 CPU runtime、复制 rq epoch，并设置 mm 的聚合 epoch。
 * 注意事项：CPU epoch 允许轻微陈旧；遍历 possible CPU，要求 per-CPU 存储已完整分配。
 */
void mm_init_sched(struct mm_struct *mm,
		   struct sched_cache_time __percpu *_pcpu_sched)
{
	unsigned long epoch = 0;
	int i;

	for_each_possible_cpu(i) {
		/* 每个 CPU 的 runtime 从零开始，epoch 取该 rq 当前代次作为后续差分基准。 */
		struct sched_cache_time *pcpu_sched = per_cpu_ptr(_pcpu_sched, i);
		struct rq *rq = cpu_rq(i);

		pcpu_sched->runtime = 0;
		/* a slightly stale cpu epoch is acceptible */
		/* 无需锁定所有 rq 取得同一时刻快照；启发式允许 CPU epoch 略微陈旧。 */
		pcpu_sched->epoch = rq->cpu_epoch;
		epoch = rq->cpu_epoch;
	}

	raw_spin_lock_init(&mm->sc_stat.lock);
	mm->sc_stat.epoch = epoch;
	mm->sc_stat.cpu = -1;
	mm->sc_stat.next_scan = jiffies;
	mm->sc_stat.nr_running_avg = 0;
	mm->sc_stat.footprint = 0;
	/*
	 * The update to mm->sc_stat should not be reordered
	 * before initialization to mm's other fields, in case
	 * the readers may get invalid mm_sched_epoch, etc.
	 */
	/*
	 * release store 是 pcpu_sched 的发布点：读者一旦看到非 NULL 指针，也必须看到此前
	 * 完成的 mm 其他字段、锁和 epoch 初始化；普通赋值可能让指针先于内容对外可见。
	 */
	smp_store_release(&mm->sc_stat.pcpu_sched, _pcpu_sched);
}

/* because why would C be fully specified */
/* C 对移位量不小于类型宽度的结果不提供可移植语义，因此显式把 >=64 的衰减定义为零。 */
/*
 * __shr_u64() - 对 64 位累计量执行边界安全的指数衰减。
 *
 * 业务背景：cache epoch 跨过多期时 runtime 需右移 n 位；大停顿可能让 n>=64。
 * 入参：@val 是输入输出 u64 指针；@n 是无单位移位/衰减期数。
 * 出参/返回：无直接返回；n>=64 清零，否则原地右移，不转移所有权。
 * 注意事项：调用者负责锁保护累计量；此 helper 只消除未定义移位，不提供同步。
 */
static __always_inline void __shr_u64(u64 *val, unsigned int n)
{
	if (n >= 64) {
		*val = 0;
		return;
	}
	*val >>= n;
}

/*
 * __update_mm_sched() - 将 rq 与 mm 的 runtime 累计推进到当前 cache epoch。
 *
 * 业务背景：几何衰减让近期 CPU/地址空间共驻更重要；跨过 n 个周期就把历史右移 n 位。
 * 入参：@rq 是持 cpu_epoch_lock 的输入输出 rq；@pcpu_sched 是该 mm 在此 CPU 的统计。
 * 出参/返回：无直接返回；可能推进 epoch/next 并衰减两级 runtime。
 * 注意事项：lockdep 强制锁前提；period 至少为 1，避免 sysctl 为零导致除零。
 */
static inline void __update_mm_sched(struct rq *rq,
				     struct sched_cache_time *pcpu_sched)
{
	lockdep_assert_held(&rq->cpu_epoch_lock);

	unsigned int period = max(READ_ONCE(llc_epoch_period), 1U);
	unsigned long n, now = jiffies;
	long delta = now - rq->cpu_epoch_next;

	if (delta > 0) {
		/* 一次跳过多个周期时批量推进 epoch，并按跨期数衰减 CPU 总 runtime。 */
		n = (delta + period - 1) / period;
		rq->cpu_epoch += n;
		rq->cpu_epoch_next += n * period;
		__shr_u64(&rq->cpu_runtime, n);
	}

	n = rq->cpu_epoch - pcpu_sched->epoch;
	if (n) {
		/* mm 的 per-CPU 统计追赶同一 rq epoch，保持分子与分母时间窗口一致。 */
		pcpu_sched->epoch += n;
		__shr_u64(&pcpu_sched->runtime, n);
	}
}

/*
 * fraction_mm_sched() - 计算某 mm 在目标 CPU 近期 runtime 中的归一化份额。
 *
 * 业务背景：task_cache_work() 用该份额选择地址空间最常运行的 CPU/LLC。
 * 入参：@rq 是目标运行队列；@pcpu_sched 是 mm 对应的 per-CPU 统计，均借用。
 * 出参/返回：返回以 NICE_0_LOAD 为满刻度的无单位比例；无持久状态外副作用。
 * 注意事项：内部 irqsave 自旋锁使 epoch 更新与记账串行，不能在已持同锁时调用。
 */
static unsigned long fraction_mm_sched(struct rq *rq,
				       struct sched_cache_time *pcpu_sched)
{
	guard(raw_spinlock_irqsave)(&rq->cpu_epoch_lock);

	__update_mm_sched(rq, pcpu_sched);

	/*
	 * Runtime is a geometric series (r=0.5) and as such will sum to twice
	 * the accumulation period, this means the multiplcation here should
	 * not overflow.
	 */
	/* runtime 以 1/2 衰减形成和不超过两倍周期的几何级数，乘 NICE_0_LOAD 不会溢出。 */
	return div64_u64(NICE_0_LOAD * pcpu_sched->runtime, rq->cpu_runtime + 1);
}

/*
 * get_pref_llc() - 从 mm 的聚合 CPU 推导 task 当前可接受的 preferred LLC。
 *
 * 业务背景：同地址空间线程倾向共享 LLC，但不能违背 task 的 NUMA 首选节点。
 * 入参：@p 是借用 task；@mm 可为 NULL，是借用地址空间。
 * 出参/返回：返回 LLC id；无有效聚合或与 NUMA 冲突时返回 -1，无状态变化。
 * 注意事项：sc_stat.cpu 与 NUMA 偏好均是无锁快照，冲突会在后续频繁记账中短暂自愈。
 */
static int get_pref_llc(struct task_struct *p, struct mm_struct *mm)
{
	int mm_sched_llc = -1, mm_sched_cpu;

	if (!mm)
		return -1;

	mm_sched_cpu = READ_ONCE(mm->sc_stat.cpu);
	if (mm_sched_cpu != -1) {
		mm_sched_llc = llc_id(mm_sched_cpu);

#ifdef CONFIG_NUMA_BALANCING
		/*
		 * Don't assign preferred LLC if it
		 * conflicts with NUMA balancing.
		 * This can happen when sched_setnuma() gets
		 * called, however it is not much of an issue
		 * because we expect account_mm_sched() to get
		 * called fairly regularly -- at a higher rate
		 * than sched_setnuma() at least -- and thus the
		 * conflict only exists for a short period of time.
		 */
		/* sched_setnuma() 可先改变 task 节点偏好；若聚合 CPU 落在别节点，暂时取消 LLC 偏好。 */
		if (static_branch_likely(&sched_numa_balancing) &&
		    p->numa_preferred_nid >= 0 &&
		    cpu_to_node(mm_sched_cpu) != p->numa_preferred_nid)
			mm_sched_llc = -1;
#endif
	}

	return mm_sched_llc;
}

static unsigned int task_running_on_cpu(int cpu, struct task_struct *p);

/*
 * account_mm_sched() - 把 fair task 的本次执行量计入 mm cache 聚合统计并刷新 LLC 偏好。
 *
 * 业务背景：update_se() 在每段执行结算时调用，形成按 epoch 衰减的 CPU 驻留画像，供
 * cache-aware placement 选择共享 LLC。
 * 入参：@rq 是持 rq 锁的输入输出运行队列；@p 是实际运行 task；@delta_exec 为纳秒增量。
 * 出参/返回：无直接返回；更新 rq/mm runtime，必要时失效 mm 聚合并重记 task LLC 计数。
 * 注意事项：仅 sched-cache 开启、fair 用户 task 且 mm 已发布 per-CPU 存储时生效。
 */
static inline
void account_mm_sched(struct rq *rq, struct task_struct *p, s64 delta_exec)
{
	struct sched_cache_time *pcpu_sched;
	struct mm_struct *mm = p->mm;
	int mm_sched_llc = -1;
	unsigned long epoch;

	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
	if (!sched_cache_enabled())
		return;

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (p->sched_class != &fair_sched_class)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;
	/*
	 * init_task, kthreads and user thread created
	 * by user_mode_thread() don't have mm.
	 */
	/* init_task、内核线程和 user_mode_thread() 创建的特殊线程没有可聚合用户 mm。 */
	if (!mm || !mm->sc_stat.pcpu_sched)
		return;

	pcpu_sched = per_cpu_ptr(mm->sc_stat.pcpu_sched, cpu_of(rq));

	scoped_guard (raw_spinlock, &rq->cpu_epoch_lock) {
		/* 在同一 epoch 锁内先衰减旧窗口，再把本次运行同时加入 mm 分子与 CPU 分母。 */
		__update_mm_sched(rq, pcpu_sched);
		pcpu_sched->runtime += delta_exec;
		rq->cpu_runtime += delta_exec;
		epoch = rq->cpu_epoch;
	}

	/*
	 * If this process hasn't hit task_cache_work() for a while invalidate
	 * its preferred state.
	 */
	/* 长时间未扫描、线程规模不合适或工作集过大时，旧聚合 CPU 已无价值，先失效为 -1。 */
	if ((long)(epoch - READ_ONCE(mm->sc_stat.epoch)) > llc_epoch_affinity_timeout ||
	    invalid_llc_nr(mm, p, cpu_of(rq)) ||
	    exceed_llc_capacity(mm, cpu_of(rq))) {
		if (READ_ONCE(mm->sc_stat.cpu) != -1)
			WRITE_ONCE(mm->sc_stat.cpu, -1);
	}

	mm_sched_llc = get_pref_llc(p, mm);

	/* task not on rq accounted later in account_entity_enqueue() */
	/* 仅实际位于此 rq 的 task 立即换计数；未排队 task 会在下一次 enqueue 使用新偏好。 */
	if (task_running_on_cpu(rq->cpu, p) &&
	    READ_ONCE(p->preferred_llc) != mm_sched_llc) {
		account_llc_dequeue(rq, p);
		WRITE_ONCE(p->preferred_llc, mm_sched_llc);
		account_llc_enqueue(rq, p);
	}
}

/*
 * task_tick_cache() - 在 epoch 前进后为当前 task 安排一次用户返回前 cache 扫描工作。
 *
 * 业务背景：调度 tick 不能执行昂贵的进程线程扫描，故只把 task_work 挂到当前任务。
 * 入参：@rq 是当前锁定运行队列；@p 是当前 task，二者均借用。
 * 出参/返回：无直接返回；每个 epoch 至多入队一次 cache_work，并推进 mm 扫描 epoch。
 * 注意事项：内核线程、无 mm/统计存储或功能关闭时为空操作；实际工作可睡眠并延后执行。
 */
static void task_tick_cache(struct rq *rq, struct task_struct *p)
{
	struct callback_head *work = &p->cache_work;
	struct mm_struct *mm = p->mm;
	unsigned long epoch;
	/* 先完成开关与任务类型过滤，再比较 epoch，避免重复排入 task_work。 */

	if (!sched_cache_enabled())
		return;

	if (!mm || p->flags & PF_KTHREAD ||
	    !mm->sc_stat.pcpu_sched)
		return;

	epoch = rq->cpu_epoch;
	/* avoid moving backwards */
	/* mm 已处理同代或更新 epoch 时不重复挂工作，也不允许陈旧 rq 把全局代次倒退。 */
	if (time_after_eq(mm->sc_stat.epoch, epoch))
		return;

	guard(raw_spinlock)(&mm->sc_stat.lock);

	if (work->next == work) {
		/* callback_head 以 next==self 表示未排队；锁内确保多个 tick 只发布一次 task_work。 */
		task_work_add(p, work, TWA_RESUME);
		WRITE_ONCE(mm->sc_stat.epoch, epoch);
	}
}

/*
 * get_scan_cpumasks() - 构造 mm cache 聚合扫描允许检查的 CPU 集合。
 *
 * 业务背景：扫描优先限制在 task NUMA 节点，但进程级 preferred LLC 可能与各线程节点
 * 偏好来回摆动，因此还并入当前聚合 LLC 节点和正在运行节点。
 * 入参：@cpus 是调用者分配的输出 cpumask；@p 是借用 task 及其 mm。
 * 出参/返回：无直接返回；覆盖 @cpus，NUMA 不适用时填充全部在线 CPU。
 * 注意事项：CPU 在线与偏好均为瞬时快照；函数不取得 CPU 或 mm 生命周期引用。
 */
static void get_scan_cpumasks(cpumask_var_t cpus, struct task_struct *p)
{
#ifdef CONFIG_NUMA_BALANCING
	int cpu, curr_cpu, nid, pref_nid;

	if (!static_branch_likely(&sched_numa_balancing))
		goto out;

	cpu = READ_ONCE(p->mm->sc_stat.cpu);
	if (cpu != -1)
		nid = cpu_to_node(cpu);
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	curr_cpu = task_cpu(p);

	/*
	 * Scanning in the preferred NUMA node is ideal. However, the NUMA
	 * preferred node is per-task rather than per-process. It is possible
	 * for different threads of the process to have distinct preferred
	 * nodes; consequently, the process-wide preferred LLC may bounce
	 * between different nodes. As a workaround, maintain the scan
	 * CPU mask to also cover the process's current preferred LLC and the
	 * current running node to mitigate the bouncing risk.
	 * TBD: numa_group should be considered during task aggregation.
	 */
	/*
	 * task 的 NUMA 偏好是每线程属性，LLC 偏好却按进程 mm 聚合；不同线程偏好不同节点
	 * 时可能让进程级结果摆动。扫描集合同时覆盖任务偏好节点、当前聚合 LLC 节点和运行
	 * 节点以缓和该问题；未来还应把 numa_group 纳入聚合模型。
	 */
	pref_nid = p->numa_preferred_nid;
	/* honor the task's preferred node */
	/* 没有 NUMA 首选节点时退化为全在线 CPU 扫描。 */
	if (pref_nid == NUMA_NO_NODE)
		goto out;

	cpumask_or(cpus, cpus, cpumask_of_node(pref_nid));

	/* honor the task's preferred LLC CPU */
	/* 将进程当前聚合 CPU 所属节点并入，保留既有 LLC 局部性候选。 */
	if (cpu != -1 && !cpumask_test_cpu(cpu, cpus) && nid != NUMA_NO_NODE)
		cpumask_or(cpus, cpus, cpumask_of_node(nid));

	/* make sure the task's current running node is included */
	/* 当前运行节点始终可选，避免扫描结果把正在提供服务的本地 CPU 排除。 */
	if (!cpumask_test_cpu(curr_cpu, cpus))
		cpumask_or(cpus, cpus, cpumask_of_node(cpu_to_node(curr_cpu)));

	return;

out:
#endif
	cpumask_copy(cpus, cpu_online_mask);
}

/*
 * update_avg_scale() - 按 LLC 大小更新一个有界灵敏度的 EWMA 样本。
 *
 * 业务背景：小 LLC 域需要更快响应 nr_running 变化，大域则用最多 8 的平滑因子抑制抖动。
 * 入参：@avg 是输入输出平均值；@sample 是同单位新样本，二者均为 u64。
 * 出参/返回：无直接返回；以 diff/divisor 原地推进平均值。
 * 注意事项：使用 raw_smp_processor_id()，调用者必须处在 CPU 不迁移的调度上下文。
 */
static inline void update_avg_scale(u64 *avg, u64 sample)
{
	int factor = per_cpu(sd_llc_size, raw_smp_processor_id());
	s64 diff = sample - *avg;
	u32 divisor;

	/*
	 * Scale the divisor based on the number of CPUs contained
	 * in the LLC. This scaling ensures smaller LLC domains use
	 * a smaller divisor to achieve more precise sensitivity to
	 * changes in nr_running, while larger LLC domains are capped
	 * at a maximum divisor of 8 which is the default smoothing
	 * factor of EWMA in update_avg().
	 */
	/* LLC CPU 数越少除数越小、对新样本越敏感；大域封顶 8，与通用 EWMA 默认平滑一致。 */
	divisor = clamp_t(u32, (factor >> 2), 2, 8);
	*avg += div64_s64(diff, divisor);
}

/*
 * task_cache_work() - 扫描 mm 在各 LLC 的近期占用并选择稳定的聚合 CPU。
 *
 * 业务背景：task_tick_cache() 把昂贵扫描延后到用户返回路径；本函数以 per-CPU 衰减
 * runtime、当前线程分布和 NUMA 约束更新进程级 preferred LLC。
 * 入参：@work 必须是 current->cache_work，借用且由 task_work 框架交付。
 * 出参/返回：无直接返回；可能推进 next_scan、更新 sc_stat.cpu/nr_running_avg 并释放临时 cpumask。
 * 注意事项：进程上下文可睡眠；cmpxchg 保证同一 mm 每周期仅一个线程扫描，CPU/域由热插拔锁和 RCU 稳定。
 */
static void task_cache_work(struct callback_head *work)
{
	int cpu, m_a_cpu = -1, nr_running = 0, curr_cpu;
	unsigned long next_scan, now = jiffies;
	struct task_struct *p = current, *cur;
	unsigned long curr_m_a_occ = 0;
	struct mm_struct *mm = p->mm;
	unsigned long m_a_occ = 0;
	cpumask_var_t cpus;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	WARN_ON_ONCE(work != &p->cache_work);

	/* task_work 已被摘链，先恢复 self 哨兵，允许后续 tick 再次安排。 */
	work->next = work;

	if (p->flags & PF_EXITING)
		return;

	next_scan = READ_ONCE(mm->sc_stat.next_scan);
	if (time_before(now, next_scan))
		return;

	/* only 1 thread is allowed to scan */
	/* 原子认领下个扫描窗口；失败表示同 mm 另一线程已经取得本周期扫描权。 */
	if (!try_cmpxchg(&mm->sc_stat.next_scan, &next_scan,
			 now + max_t(unsigned long,
				     READ_ONCE(llc_epoch_period), 1)))
		return;

	curr_cpu = task_cpu(p);
	/* 不具多线程聚合价值或工作集超过 LLC 时失效旧偏好并终止本轮。 */
	if (invalid_llc_nr(mm, p, curr_cpu) ||
	    exceed_llc_capacity(mm, curr_cpu)) {
		if (READ_ONCE(mm->sc_stat.cpu) != -1)
			WRITE_ONCE(mm->sc_stat.cpu, -1);

		return;
	}

	if (!zalloc_cpumask_var(&cpus, GFP_KERNEL))
		return;

	/* cpus_read_lock 稳定在线拓扑，RCU 保证遍历中的 sched_domain/curr 对象暂不回收。 */
	scoped_guard (cpus_read_lock) {
		guard(rcu)();

		get_scan_cpumasks(cpus, p);

		for_each_cpu(cpu, cpus) {
			/* XXX sched_cluster_active */
			/* 未来可能需要过滤非活跃 cluster；当前按 LLC 调度域逐组扫描。 */
			struct sched_domain *sd = rcu_dereference_all(per_cpu(sd_llc, cpu));
			unsigned long occ, m_occ = 0, a_occ = 0;
			int m_cpu = -1, i;

			if (!sd)
				continue;

			for_each_cpu(i, sched_domain_span(sd)) {
				/* 累加 mm 在本 LLC 的衰减占用，并记录单 CPU 最大占用作为代表 CPU。 */
				occ = fraction_mm_sched(cpu_rq(i),
							per_cpu_ptr(mm->sc_stat.pcpu_sched, i));
				a_occ += occ;
				if (occ > m_occ) {
					m_occ = occ;
					m_cpu = i;
				}

				cur = rcu_dereference_all(cpu_rq(i)->curr);
				if (cur && !(cur->flags & (PF_EXITING | PF_KTHREAD)) &&
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
				    cur->mm == mm)
					nr_running++;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
			}

			/*
			 * Compare the accumulated occupancy of each LLC. The
			 * reason for using accumulated occupancy rather than average
			 * per CPU occupancy is that it works better in asymmetric LLC
			 * scenarios.
			 * For example, if there are 2 threads in a 4CPU LLC and 3
			 * threads in an 8CPU LLC, it might be better to choose the one
			 * with 3 threads. However, this would not be the case if the
			 * occupancy is divided by the number of CPUs in an LLC (i.e.,
			 * if average per CPU occupancy is used).
			 * Besides, NUMA balancing fault statistics behave similarly:
			 * the total number of faults per node is compared rather than
			 * the average number of faults per CPU. This strategy is also
			 * followed here.
			 */
			/*
			 * 以 LLC 总占用而非每 CPU 平均值比较，才能在非对称 LLC 上偏向实际承载更多
			 * 线程的一侧；这与 NUMA balancing 按节点比较总 fault 数的策略一致。
			 */
			if (a_occ > m_a_occ) {
				m_a_occ = a_occ;
				m_a_cpu = m_cpu;
			}

			if (llc_id(cpu) == llc_id(READ_ONCE(mm->sc_stat.cpu)))
				curr_m_a_occ = a_occ;

			/* 已处理整个 LLC span，将其从待扫 mask 删除，避免同一域被每个 CPU 重复统计。 */
			cpumask_andnot(cpus, cpus, sched_domain_span(sd));
		}
	}

	if (m_a_occ > (2 * curr_m_a_occ)) {
		/*
		 * Avoid switching sc_stat.cpu too fast.
		 * The reason to choose 2X is because:
		 * 1. It is better to keep the preferred LLC stable,
		 *    rather than changing it frequently and cause migrations
		 * 2. 2X means the new preferred LLC has at least 1 more
		 *    busy CPU than the old one(200% vs 100%, eg)
		 * 3. 2X is chosen based on test results, as it delivers
		 *    the optimal performance gain so far.
		 */
		/*
		 * 新 LLC 总占用必须超过当前两倍才切换：滞回减少迁移抖动，通常也意味着至少多
		 * 一个忙 CPU；2 倍阈值来自当前测试结果而非严格理论界限。
		 */
		WRITE_ONCE(mm->sc_stat.cpu, m_a_cpu);
	}

	/* 扫描成功后平滑本 mm 同时运行线程数，并释放本轮唯一临时资源。 */
	update_avg_scale(&mm->sc_stat.nr_running_avg, nr_running);
	free_cpumask_var(cpus);
}

/*
 * init_sched_mm() - 初始化 task 的 cache 扫描回调和无偏好哨兵。
 *
 * 业务背景：fork/任务初始化必须先构造 task_work，tick 才能安全延后 mm 扫描。
 * 入参：@p 是尚未参与 cache 记账的输入输出 task，调用者独占。
 * 出参/返回：无直接返回；初始化 cache_work 并把 preferred_llc 设为 -1。
 * 注意事项：next=self 同时表示回调未排队；旧偏好必须清除以免污染首次 enqueue 计数。
 */
void init_sched_mm(struct task_struct *p)
{
	struct callback_head *work = &p->cache_work;

	init_task_work(work, task_cache_work);
	work->next = work;
	/*
	 * Reset new task's preference to avoid
	 * polluting account_llc_enqueue().
	 */
	/* 新任务尚无有效驻留画像，以 -1 阻止首次入队错误增加某个 LLC 的偏好计数。 */
	p->preferred_llc = -1;
}

#else /* CONFIG_SCHED_CACHE */

/*
 * account_mm_sched() - sched-cache 关闭时省略 mm 运行画像记账。
 *
 * 业务背景：公共执行结算路径保留统一调用，配置关闭后不分配或维护 cache 统计。
 * 入参：@rq、@p 为借用对象，@delta_exec 为纳秒增量；本 stub 均不读取。
 * 出参/返回：无直接返回且无副作用。
 * 注意事项：只消除 cache 启发式，不影响基本执行时间、cgroup 或 PELT 记账。
 */
static inline void account_mm_sched(struct rq *rq, struct task_struct *p,
				    s64 delta_exec) { }

/*
 * init_sched_mm() - sched-cache 关闭时的任务初始化空操作。
 *
 * 业务背景：任务构造代码无需按配置复制调用路径。
 * 入参：@p 是借用 task，本 stub 不读取。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：该配置下 cache_work/preferred_llc 不参与任何决策。
 */
void init_sched_mm(struct task_struct *p) { }

/*
 * task_tick_cache() - sched-cache 关闭时跳过周期扫描安排。
 *
 * 业务背景：调度 tick 保持统一结构，但不创建 task_work。
 * 入参：@rq、@p 均为借用对象，本 stub 不读取。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：不影响普通 tick 的 vruntime、PELT 和抢占检查。
 */
static void task_tick_cache(struct rq *rq, struct task_struct *p) { }

/*
 * get_pref_llc() - sched-cache 关闭时报告不存在 LLC 偏好。
 *
 * 业务背景：选核调用点用 -1 统一表达“不施加 cache 聚合约束”。
 * 入参：@p、@mm 均为借用对象，本 stub 不读取。
 * 出参/返回：恒返回 -1，无副作用。
 * 注意事项：NUMA 与普通 cache affinity 仍可通过其他调度策略生效。
 */
static inline int get_pref_llc(struct task_struct *p,
			       struct mm_struct *mm)
{
	return -1;
}

/*
 * account_llc_enqueue() - sched-cache 关闭时不维护 LLC 入队计数。
 *
 * 业务背景：公共 enqueue 路径保留调用，配置裁剪全部聚合状态。
 * 入参：@rq、@p 均借用且不读取。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：基本 nr_running 等运行队列计数由其他路径照常维护。
 */
static void account_llc_enqueue(struct rq *rq, struct task_struct *p) {}

/*
 * account_llc_dequeue() - sched-cache 关闭时不维护 LLC 出队计数。
 *
 * 业务背景：与空的 enqueue stub 配对，保持配置无关调用结构。
 * 入参：@rq、@p 均借用且不读取。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：不得把该空操作推广到 CONFIG_SCHED_CACHE=y 的配对计数协议。
 */
static void account_llc_dequeue(struct rq *rq, struct task_struct *p) {}

#endif /* CONFIG_SCHED_CACHE */

/*
 * Used by other classes to account runtime.
 */
/* 其他调度类借此复用 fair/proxy-exec 的实际运行时间结算，以 rq->donor 实体为记账入口。 */
/*
 * update_curr_common() - 为非 fair 调用者结算当前 donor 实体的实际执行时间。
 *
 * 业务背景：类间公共运行时记账需要沿 proxy-exec donor 归属调用 update_se()。
 * 入参：@rq 是调用者已锁定的输入输出运行队列。
 * 出参/返回：返回纳秒执行增量，非正值表示时钟未推进；副作用由 update_se() 定义。
 * 注意事项：只结算通用执行统计，不推进 fair vruntime/deadline 或 bandwidth。
 */
s64 update_curr_common(struct rq *rq)
{
	return update_se(rq, &rq->donor->se);
}

/*
 * Update the current task's runtime statistics.
 */
/* 更新当前 fair 实体的运行时间统计，并把实际服务映射到 EEVDF 与 bandwidth 状态。 */
/* rq 锁下更新当前实体及祖先组实体，随后扣除本地 CFS bandwidth runtime。 */
/*
 * update_curr() - 结算当前 cfs_rq 实体并决定是否需要一次惰性重调度。
 *
 * 业务背景：tick、入出队和选人边界调用它，将 wall time 转为 vruntime，续期 deadline，
 * 同时扣减 fair-server/CFS bandwidth 配额。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列借用指针。
 * 出参/返回：无直接返回；可能更新 curr 统计、vruntime/deadline、配额并设置 lazy resched。
 * 注意事项：curr 是 donor 选择实体，proxy-exec 下可能不同于 rq->curr 实际执行 task。
 */
static void update_curr(struct cfs_rq *cfs_rq)
{
	/*
	 * Note: cfs_rq->curr corresponds to the task picked to
	 * run (ie: rq->donor.se) which due to proxy-exec may
	 * not necessarily be the actual task running
	 * (rq->curr.se). This is easy to confuse!
	 */
	/* cfs_rq->curr 对应被选择的 donor；proxy-exec 可让 rq->curr 成为另一实际运行 task。 */
	struct sched_entity *curr = cfs_rq->curr;
	struct rq *rq = rq_of(cfs_rq);
	s64 delta_exec;
	bool resched;

	if (unlikely(!curr))
		return;

	delta_exec = update_se(rq, curr);
	if (unlikely(delta_exec <= 0))
		return;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	curr->vruntime += calc_delta_fair(delta_exec, curr);
	/* 实际纳秒按权重折为虚拟服务；低权重实体增长更快并更早耗尽 deadline。 */
	resched = update_deadline(cfs_rq, curr);

	if (entity_is_task(curr)) {
		/*
		 * If the fair_server is active, we need to account for the
		 * fair_server time whether or not the task is running on
		 * behalf of fair_server or not:
		 *  - If the task is running on behalf of fair_server, we need
		 *    to limit its time based on the assigned runtime.
		 *  - Fair task that runs outside of fair_server should account
		 *    against fair_server such that it can account for this time
		 *    and possibly avoid running this period.
		 */
		/*
		 * fair_server 激活时，无论 task 是否代表 server 运行，都扣减 server 预算：代表
		 * server 时限制获准 runtime，独立 fair 运行时也要让 server 知道本周期已消费 CPU。
		 */
		dl_server_update(&rq->fair_server, delta_exec);
	}

	account_cfs_rq_runtime(cfs_rq, delta_exec);
	/* 每层 cfs_rq 都扣除实际服务量，带宽耗尽可在后续路径触发 throttle。 */

	if (cfs_rq->nr_queued == 1)
		return;

	/* 多候选时，请求到期或保护窗口结束才设置惰性重调度，并清理旧 buddy 偏好。 */
	if (resched || !protect_slice(curr)) {
		resched_curr_lazy(rq);
		clear_buddies(cfs_rq, curr);
	}
}

/*
 * update_curr_fair() - 从 rq donor 定位所属 cfs_rq 并执行 fair 当前实体结算。
 *
 * 业务背景：sched_class 回调使用 rq 形式入口，核心实现以 cfs_rq 为层级单位。
 * 入参：@rq 是已锁定的输入输出运行队列。
 * 出参/返回：无直接返回；副作用完全由 update_curr() 产生。
 * 注意事项：proxy-exec 下必须从 rq->donor 而非 rq->curr 获取 fair 实体。
 */
static void update_curr_fair(struct rq *rq)
{
	update_curr(cfs_rq_of(&rq->donor->se));
}

/*
 * update_stats_wait_start_fair() - 记录实体开始在 fair 运行队列等待 CPU 的时间点。
 *
 * 业务背景：enqueue 非当前实体时，schedstat 用等待区间计算调度延迟。
 * 入参：@cfs_rq 是借用队列；@se 是开始等待的借用实体。
 * 出参/返回：无直接返回；启用 schedstat 时更新实体统计及 task trace 上下文。
 * 注意事项：静态键关闭时立即返回；调用者持 rq 锁保证 wait_start 与排队状态一致。
 */
static inline void
update_stats_wait_start_fair(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct sched_statistics *stats;
	struct task_struct *p = NULL;

	if (!schedstat_enabled())
		return;

	stats = __schedstats_from_se(se);

	if (entity_is_task(se))
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		p = task_of(se);

	__update_stats_wait_start(rq_of(cfs_rq), p, stats);
}

/*
 * update_stats_wait_end_fair() - 在实体离开等待态时结算本次 runqueue 等待时长。
 *
 * 业务背景：dequeue 或被选为当前实体时，与 wait_start 配对生成延迟统计。
 * 入参：@cfs_rq 是借用队列；@se 是结束等待的借用实体。
 * 出参/返回：无直接返回；有效起点存在时更新 wait 累计/最大值。
 * 注意事项：运行时从关闭切到开启时已有实体的 wait_start 为零，必须跳过以免产生伪大差值。
 */
static inline void
update_stats_wait_end_fair(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct sched_statistics *stats;
	struct task_struct *p = NULL;

	if (!schedstat_enabled())
		return;

	stats = __schedstats_from_se(se);

	/*
	 * When the sched_schedstat changes from 0 to 1, some sched se
	 * maybe already in the runqueue, the se->statistics.wait_start
	 * will be 0.So it will let the delta wrong. We need to avoid this
	 * scenario.
	 */
	/* schedstat 动态开启时，已在队列的实体没有起点；零值表示不可结算而不是从时刻零等待。 */
	if (unlikely(!schedstat_val(stats->wait_start)))
		return;

	if (entity_is_task(se))
		p = task_of(se);

	__update_stats_wait_end(rq_of(cfs_rq), p, stats);
}

/*
 * update_stats_enqueue_sleeper_fair() - 记录睡眠任务唤醒后重新入队的统计事件。
 *
 * 业务背景：ENQUEUE_WAKEUP 路径需把睡眠/阻塞区间转换为 schedstat 可观测数据。
 * 入参：@cfs_rq 是借用队列；@se 是刚唤醒入队的借用实体。
 * 出参/返回：无直接返回；启用时更新 sleeper 统计，组实体以 NULL task 调用通用 helper。
 * 注意事项：只做观测记账，不改变任务状态、排队关系或所有权。
 */
static inline void
update_stats_enqueue_sleeper_fair(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct sched_statistics *stats;
	struct task_struct *tsk = NULL;

	if (!schedstat_enabled())
		return;

	stats = __schedstats_from_se(se);

	if (entity_is_task(se))
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		tsk = task_of(se);

	__update_stats_enqueue_sleeper(rq_of(cfs_rq), tsk, stats);
}

/*
 * Task is being enqueued - update stats:
 */
/* task 正在入队：为非当前实体开始等待计时，并在唤醒场景结算此前睡眠统计。 */
/*
 * update_stats_enqueue_fair() - 在 fair 实体入队边界分派等待与唤醒统计。
 *
 * 业务背景：enqueue_entity() 在改变 on_rq 前后调用该包装，统一 schedstat 事件顺序。
 * 入参：@cfs_rq 是借用队列；@se 是入队实体；@flags 是 ENQUEUE_* 事件位图。
 * 出参/返回：无直接返回；按条件写 sched_statistics，不改变调度状态。
 * 注意事项：curr 的 dequeue/enqueue 是记账 NOP，不应被误计为等待；功能关闭时零成本返回。
 */
static inline void
update_stats_enqueue_fair(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	if (!schedstat_enabled())
		return;

	/*
	 * Are we enqueueing a waiting task? (for current tasks
	 * a dequeue/enqueue event is a NOP)
	 */
	/* 只有非 curr 实体真正进入等待队列；当前实体的形式化重入队不开始新等待区间。 */
	if (se != cfs_rq->curr)
		update_stats_wait_start_fair(cfs_rq, se);

	if (flags & ENQUEUE_WAKEUP)
		update_stats_enqueue_sleeper_fair(cfs_rq, se);
}

/*
 * update_stats_dequeue_fair() - 在 fair 实体出队边界结束等待并标记睡眠/阻塞起点。
 *
 * 业务背景：dequeue_entity() 需要区分被选中、迁移与主动睡眠，保持 schedstat 区间成对。
 * 入参：@cfs_rq 是借用队列；@se 是出队实体；@flags 是 DEQUEUE_* 事件位图。
 * 出参/返回：无直接返回；可能更新 wait_end、sleep_start 或 block_start。
 * 注意事项：task state 与并发 TTWU 存在已知竞态，统计是观测近似而非状态同步依据。
 */
static inline void
update_stats_dequeue_fair(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{

	if (!schedstat_enabled())
		return;

	/*
	 * Mark the end of the wait period if dequeueing a
	 * waiting task:
	 */
	/* 非 curr 出队表示其等待区间结束；curr 的形式化出队没有等待起点可结算。 */
	if (se != cfs_rq->curr)
		update_stats_wait_end_fair(cfs_rq, se);

	if ((flags & DEQUEUE_SLEEP) && entity_is_task(se)) {
		struct task_struct *tsk = task_of(se);
		unsigned int state;

		/* XXX racy against TTWU */
		/* try_to_wake_up() 可并发改变 __state；READ_ONCE 防撕裂但不提供一致快照，故仅用于统计。 */
		state = READ_ONCE(tsk->__state);
		if (state & TASK_INTERRUPTIBLE)
			__schedstat_set(tsk->stats.sleep_start,
				      rq_clock(rq_of(cfs_rq)));
		if (state & TASK_UNINTERRUPTIBLE)
			__schedstat_set(tsk->stats.block_start,
				      rq_clock(rq_of(cfs_rq)));
	}
}

/*
 * We are picking a new current task - update its stats:
 */
/* 选出新当前实体时，以 rq task clock 建立下一运行段的 exec_start 起点。 */
/*
 * update_stats_curr_start() - 标记实体新一轮占用 CPU 的统计起点。
 *
 * 业务背景：set_next_entity() 在发布 curr 时调用，后续 update_se() 用该时间计算 delta_exec。
 * 入参：@cfs_rq 是借用队列；@se 是即将运行的输入输出实体。
 * 出参/返回：无直接返回；写入 se->exec_start 为当前 rq task clock。
 * 注意事项：必须在 rq 锁下与 curr 切换同序执行，否则下一段可能重复或漏记运行时间。
 */
static inline void
update_stats_curr_start(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	/*
	 * We are starting a new run period:
	 */
	/* 新运行区间从当前 task clock 开始，等待/阻塞时间不计入执行量。 */
	se->exec_start = rq_clock_task(rq_of(cfs_rq));
}

/* Check sched_smt_active before calling this to avoid overheads in fastpaths */
/* 调用者先检查 sched_smt_active 静态键，避免非 SMT 系统在调度热路径扫描 sibling mask。 */
/*
 * is_core_idle() - 判断指定 CPU 的所有 SMT 兄弟是否都空闲。
 *
 * 业务背景：NUMA/负载平衡评估物理 core 可用性时，当前 CPU 空闲还不足以代表共享 core 空闲。
 * 入参：@cpu 是有效逻辑 CPU 编号，仅输入。
 * 出参/返回：任一兄弟忙返回 false，否则 true；无状态变化。
 * 注意事项：无锁逐 CPU 快照可能立即过时，只用于启发式；调用前应确认 SMT 激活。
 */
static inline bool is_core_idle(int cpu)
{
	int sibling;

	for_each_cpu(sibling, cpu_smt_mask(cpu)) {
		if (cpu == sibling)
			continue;

		if (!idle_cpu(sibling))
			return false;
	}

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return true;
}

#ifdef CONFIG_NUMA
#define NUMA_IMBALANCE_MIN 2

/*
 * adjust_numa_imbalance() - 在轻载 NUMA 目标上容忍小幅任务数不均衡。
 *
 * 业务背景：严格均衡可能拆散通信任务并损害内存局部性；目标尚未拥挤时允许一小对任务留在本地。
 * 入参：@imbalance 是原需迁移任务数；@dst_running 是目标运行数；@imb_numa_nr 是拥挤阈值。
 * 出参/返回：目标忙时原样返回；轻载且不平衡<=2 时返回 0，否则返回原值。
 * 注意事项：running task 不等于 busy CPU，affinity 会造成近似误差；只调整策略不执行迁移。
 */
static inline long
adjust_numa_imbalance(int imbalance, int dst_running, int imb_numa_nr)
{
	/*
	 * Allow a NUMA imbalance if busy CPUs is less than the maximum
	 * threshold. Above this threshold, individual tasks may be contending
	 * for both memory bandwidth and any shared HT resources.  This is an
	 * approximation as the number of running tasks may not be related to
	 * the number of busy CPUs due to sched_setaffinity.
	 */
	/* 目标超过阈值时共享内存带宽/SMT 已可能竞争，不再为局部性容忍不均衡。 */
	if (dst_running > imb_numa_nr)
		return imbalance;

	/*
	 * Allow a small imbalance based on a simple pair of communicating
	 * tasks that remain local when the destination is lightly loaded.
	 */
	/* 轻载时最多容忍两个通信任务留在原节点，避免为了数字均衡破坏共享数据局部性。 */
	if (imbalance <= NUMA_IMBALANCE_MIN)
		return 0;

	return imbalance;
}
#endif /* CONFIG_NUMA */

#ifdef CONFIG_NUMA_BALANCING
/*
 * Approximate time to scan a full NUMA task in ms. The task scan period is
 * calculated based on the tasks virtual memory size and
 * numa_balancing_scan_size.
 */
/* 完整扫描任务常驻地址空间的目标毫秒范围；实际单窗周期按虚拟内存/RSS 与 scan_size 分摊。 */
unsigned int sysctl_numa_balancing_scan_period_min = 1000;
unsigned int sysctl_numa_balancing_scan_period_max = 60000;

/* Portion of address space to scan in MB */
/* 每次 PTE 扫描覆盖的地址空间窗口，单位 MB；窗口越大收敛更快但 fault/扫描开销更高。 */
unsigned int sysctl_numa_balancing_scan_size = 256;

/* Scan @scan_size MB every @scan_period after an initial @scan_delay in ms */
/* 初次延迟后每个 scan_period 扫描 scan_size MB；三个量共同控制 NUMA 采样速率。 */
unsigned int sysctl_numa_balancing_scan_delay = 1000;

/* The page with hint page fault latency < threshold in ms is considered hot */
/* hint fault 延迟低于该毫秒阈值的页视为热页，可更积极地向访问 CPU 所在节点迁移。 */
unsigned int sysctl_numa_balancing_hot_threshold = MSEC_PER_SEC;

/* 多任务共享地址空间/工作集的 NUMA 统计对象，以 refcount+RCU 管生命周期。 */
struct numa_group {
	/* 引用由加入组的 task 持有；归零后通过 rcu 延迟释放，避免无锁读者 UAF。 */
	refcount_t refcount;

	/* lock 串行维护成员数/成员关系和组聚合 fault 更新，不替代 RCU 生命周期保护。 */
	spinlock_t lock; /* nr_tasks, tasks */
	/* 当前加入组的 task 数；用于扫描周期缩放和最终释放判断。 */
	int nr_tasks;
	/* 稳定的组标识，通常源自创建/合并时选定的 task pid。 */
	pid_t gid;
	/* fault 足够集中的伪交错活跃 NUMA 节点数量。 */
	int active_nodes;

	/* 最后引用消失后挂入 RCU callback，grace period 后释放柔性数组对象。 */
	struct rcu_head rcu;
	/* 全节点、共享/私有类别的衰减 fault 总数，用于组权重归一化。 */
	unsigned long total_faults;
	/* 单 CPU fault 聚合的当前最大值，用于识别组的活跃节点集合。 */
	unsigned long max_faults_cpu;
	/*
	 * faults[] array is split into two regions: faults_mem and faults_cpu.
	 *
	 * Faults_cpu is used to decide whether memory should move
	 * towards the CPU. As a consequence, these stats are weighted
	 * more by CPU use than by memory faults.
	 */
	/*
	 * faults 柔性数组前半保存 memory fault，后半保存 CPU-weighted fault；后者决定内存
	 * 是否应向 CPU 迁移，因此比单纯缺页次数更强调实际 CPU 使用。索引由 task_faults_idx()
	 * 统一生成，分共享/私有、MEM/CPU 与节点维度。
	 */
	unsigned long faults[];
};

/*
 * For functions that can be called in multiple contexts that permit reading
 * ->numa_group (see struct task_struct for locking rules).
 */
/* 对可在多种合法上下文读取 task->numa_group 的调用者，按 task_struct 锁规则验证 RCU 解引用条件。 */
/*
 * deref_task_numa_group() - 在 current 或稳定的非运行 task 上读取 NUMA 组指针。
 *
 * 业务背景：fault/迁移 helper 既可能处理 current，也可能在目标 rq 锁下检查其他 task。
 * 入参：@p 是借用 task，调用者须满足表达式中的 current 或 rq 锁+!on_cpu 条件。
 * 出参/返回：返回借用 numa_group 或 NULL；不增加 refcount。
 * 注意事项：返回生命周期仅受当前 RCU/锁窗口保护，退出后长期保存必须另取引用。
 */
static struct numa_group *deref_task_numa_group(struct task_struct *p)
{
	return rcu_dereference_check(p->numa_group, p == current ||
		(lockdep_is_held(__rq_lockp(task_rq(p))) && !READ_ONCE(p->on_cpu)));
}

/*
 * deref_curr_numa_group() - 在仅允许 current 的保护条件下读取其 NUMA 组。
 *
 * 业务背景：task_scan_start/max 等当前任务路径无需额外 rq 锁，可利用 current 不会并发退出自身。
 * 入参：@p 必须等于 current，是借用 task。
 * 出参/返回：返回借用 numa_group 或 NULL，不修改引用计数。
 * 注意事项：rcu_dereference_protected 的条件是可验证契约，传入非 current 会违反生命周期保证。
 */
static struct numa_group *deref_curr_numa_group(struct task_struct *p)
{
	return rcu_dereference_protected(p->numa_group, p == current);
}

static inline unsigned long group_faults_priv(struct numa_group *ng);
static inline unsigned long group_faults_shared(struct numa_group *ng);

/*
 * task_nr_scan_windows() - 计算覆盖 task 常驻集需要多少个 NUMA 扫描窗口。
 *
 * 业务背景：scan_period_min/max 描述完整 RSS 扫描目标，需按每次 scan_size 拆成窗口周期。
 * 入参：@p 是持有效 mm 的借用 task。
 * 出参/返回：返回至少 1 的无单位窗口数；不修改 task/mm。
 * 注意事项：使用 RSS 而非虚拟大小，因为 PTE 扫描跳过空页/非驻留页，hint fault 也只来自驻留页。
 */
static unsigned int task_nr_scan_windows(struct task_struct *p)
{
	unsigned long rss = 0;
	unsigned long nr_scan_pages;

	/*
	 * Calculations based on RSS as non-present and empty pages are skipped
	 * by the PTE scanner and NUMA hinting faults should be trapped based
	 * on resident pages
	 */
	/* 以 RSS 估算实际可触发 hint fault 的页；零 RSS 仍按一个窗口返回，避免除零和零周期。 */
	nr_scan_pages = MB_TO_PAGES(sysctl_numa_balancing_scan_size);
	rss = get_mm_rss(p->mm);
	if (!rss)
		rss = nr_scan_pages;

	rss = round_up(rss, nr_scan_pages);
	return rss / nr_scan_pages;
}

/* For sanity's sake, never scan more PTEs than MAX_SCAN_WINDOW MB/sec. */
/* 为限制页表扫描成本，每秒最多检查 MAX_SCAN_WINDOW MB 对应的 PTE。 */
#define MAX_SCAN_WINDOW 2560

/*
 * task_scan_min() - 计算单个扫描窗口允许的最短间隔。
 *
 * 业务背景：既要满足完整 RSS 的最小周期，又必须把扫描吞吐限制在每秒 2560MB。
 * 入参：@p 是借用 task，读取其 mm RSS 和 NUMA sysctl 快照。
 * 出参/返回：返回毫秒窗口周期下界，取吞吐 floor 与完整扫描分摊值较大者。
 * 注意事项：scan_size 应为有效非零配置；sysctl 更新并非事务，结果允许短暂混合快照。
 */
static unsigned int task_scan_min(struct task_struct *p)
{
	unsigned int scan_size = READ_ONCE(sysctl_numa_balancing_scan_size);
	unsigned int scan, floor;
	unsigned int windows = 1;

	if (scan_size < MAX_SCAN_WINDOW)
		windows = MAX_SCAN_WINDOW / scan_size;
	floor = 1000 / windows;

	scan = sysctl_numa_balancing_scan_period_min / task_nr_scan_windows(p);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return max_t(unsigned int, floor, scan);
}

/*
 * task_scan_start() - 为新一轮 NUMA 自适应扫描选择初始窗口周期。
 *
 * 业务背景：共享 fault 多、组成员多时应放慢扫描，避免多个线程重复采样共享地址空间。
 * 入参：@p 必须是有 mm 的 current task，函数借用其 RCU numa_group。
 * 出参/返回：返回不小于 task_scan_min() 的毫秒周期；无状态副作用。
 * 注意事项：RCU 只稳定组内存，fault/refcount 快照仍可变化，结果用于启发式调速。
 */
static unsigned int task_scan_start(struct task_struct *p)
{
	unsigned long smin = task_scan_min(p);
	unsigned long period = smin;
	struct numa_group *ng;

	/* Scale the maximum scan period with the amount of shared memory. */
	/* 组引用数和共享 fault 比例共同拉长周期；private 占比高时保持更积极的单任务扫描。 */
	rcu_read_lock();
	ng = rcu_dereference_all(p->numa_group);
	if (ng) {
		unsigned long shared = group_faults_shared(ng);
		unsigned long private = group_faults_priv(ng);

		period *= refcount_read(&ng->refcount);
		period *= shared + 1;
		period /= private + shared + 1;
	}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	rcu_read_unlock();

	return max(smin, period);
}

/*
 * task_scan_max() - 计算 task 自适应 NUMA 扫描允许的最长窗口周期。
 *
 * 业务背景：慢速上界也按 RSS 窗口数分摊，并随 NUMA 组共享程度放大以控制总开销。
 * 入参：@p 必须是 current 的借用 task，持有有效 mm。
 * 出参/返回：返回毫秒上界且保证不低于 task_scan_min()；无对象副作用。
 * 注意事项：读取 current->numa_group 的保护契约由 deref_curr_numa_group() 强制。
 */
static unsigned int task_scan_max(struct task_struct *p)
{
	unsigned long smin = task_scan_min(p);
	unsigned long smax;
	struct numa_group *ng;

	/* Watch for min being lower than max due to floor calculations */
	/* 分窗与吞吐 floor 可能让名义 max 小于 min，出口用 max(smin,smax) 修正。 */
	smax = sysctl_numa_balancing_scan_period_max / task_nr_scan_windows(p);

	/* Scale the maximum scan period with the amount of shared memory. */
	/* 与起始周期相同，共享越强、组越大，允许的最慢扫描周期越长。 */
	ng = deref_curr_numa_group(p);
	if (ng) {
		unsigned long shared = group_faults_shared(ng);
		unsigned long private = group_faults_priv(ng);
		unsigned long period = smax;

		period *= refcount_read(&ng->refcount);
		period *= shared + 1;
		period /= private + shared + 1;

		smax = max(smax, period);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	return max(smin, smax);
}

/*
 * account_numa_enqueue() - 将 task 的 NUMA 偏好状态加入 rq 计数。
 *
 * 业务背景：负载平衡通过 nr_numa_running/nr_preferred_running 判断队列中有偏好及已本地化任务数。
 * 入参：@rq 是锁定的输入输出队列；@p 是正在入队的借用 task。
 * 出参/返回：无直接返回；按偏好有效性和当前节点匹配分别增加两个计数。
 * 注意事项：必须与同一偏好快照下的 dequeue 配对；改变 preferred_nid 时需先撤旧再加新。
 */
static void account_numa_enqueue(struct rq *rq, struct task_struct *p)
{
	rq->nr_numa_running += (p->numa_preferred_nid != NUMA_NO_NODE);
	rq->nr_preferred_running += (p->numa_preferred_nid == task_node(p));
}

/*
 * account_numa_dequeue() - 从 rq NUMA 计数撤销 task 当前偏好贡献。
 *
 * 业务背景：出队、迁移或偏好更新时保持 NUMA 队列统计与实际排队集合一致。
 * 入参：@rq 是锁定的输入输出队列；@p 是正在出队的借用 task。
 * 出参/返回：无直接返回；按条件减少两个计数，不改变 task 偏好。
 * 注意事项：未配对或偏好已先改变会导致下溢/漂移，调用顺序受 rq 锁协议约束。
 */
static void account_numa_dequeue(struct rq *rq, struct task_struct *p)
{
	rq->nr_numa_running -= (p->numa_preferred_nid != NUMA_NO_NODE);
	rq->nr_preferred_running -= (p->numa_preferred_nid == task_node(p));
}

/* Shared or private faults. */
/* 每个节点把 hint fault 分为共享(0)与私有(1)两类。 */
#define NR_NUMA_HINT_FAULT_TYPES 2

/* Memory and CPU locality */
/* 每类 fault 再分 NUMA_MEM 与 NUMA_CPU 两种统计视角。 */
#define NR_NUMA_HINT_FAULT_STATS (NR_NUMA_HINT_FAULT_TYPES * 2)

/* Averaged statistics, and temporary buffers. */
/* 数组前半为衰减平均，后半为当前采样窗临时计数。 */
#define NR_NUMA_HINT_FAULT_BUCKETS (NR_NUMA_HINT_FAULT_STATS * 2)

/*
 * task_numa_group_id() - 在 RCU 下读取 task 当前 NUMA 组标识。
 *
 * 业务背景：trace/proc 与迁移诊断需要稳定地报告组身份，而不持有组锁。
 * 入参：@p 是借用 task，调用期间必须保持 task 本身有效。
 * 出参/返回：有组时返回 gid，无组返回 0；不增加组引用或修改状态。
 * 注意事项：返回后组关系可立即变化，gid 只是读取时快照。
 */
pid_t task_numa_group_id(struct task_struct *p)
{
	struct numa_group *ng;
	pid_t gid = 0;

	rcu_read_lock();
	ng = rcu_dereference_all(p->numa_group);
	if (ng)
		gid = ng->gid;
	rcu_read_unlock();

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return gid;
}

/*
 * The averaged statistics, shared & private, memory & CPU,
 * occupy the first half of the array. The second half of the
 * array is for current counters, which are averaged into the
 * first set by task_numa_placement.
 */
/* fault 数组前半存共享/私有、memory/CPU 的平均值，后半存当前窗口，placement 周期再折入前半。 */
/*
 * task_faults_idx() - 把统计视角、节点与共享性映射到 numa_faults 线性索引。
 *
 * 业务背景：task 与 group 共享同一柔性数组布局，所有读写必须用统一索引公式。
 * 入参：@s 是 NUMA_MEM/NUMA_CPU；@nid 是节点编号；@priv 为 0 共享、1 私有。
 * 出参/返回：返回前半平均区中的元素索引；无副作用。
 * 注意事项：调用者若访问临时区需另加 NR_NUMA_HINT_FAULT_STATS*nr_node_ids 偏移。
 */
static inline int task_faults_idx(enum numa_faults_stats s, int nid, int priv)
{
	return NR_NUMA_HINT_FAULT_TYPES * (s * nr_node_ids + nid) + priv;
}

/*
 * task_faults() - 汇总 task 在指定节点的共享与私有 memory fault 平均值。
 *
 * 业务背景：节点权重比较通常关心总内存亲和证据，不区分页是共享还是私有。
 * 入参：@p 是借用 task；@nid 是有效 NUMA 节点编号。
 * 出参/返回：统计未分配时返回 0，否则返回两类 fault 之和；无状态变化。
 * 注意事项：无锁读取允许近似，只用于迁移启发式。
 */
static inline unsigned long task_faults(struct task_struct *p, int nid)
{
	if (!p->numa_faults)
		return 0;

	return p->numa_faults[task_faults_idx(NUMA_MEM, nid, 0)] +
		p->numa_faults[task_faults_idx(NUMA_MEM, nid, 1)];
}

/*
 * group_faults() - 汇总 task 所属 NUMA 组在指定节点的 memory fault。
 *
 * 业务背景：共享工作集按组放置时，用组证据替代单线程局部样本。
 * 入参：@p 是满足 deref_task_numa_group() 锁条件的借用 task；@nid 为节点编号。
 * 出参/返回：无组返回 0，否则返回共享+私有 fault；不获取组引用。
 * 注意事项：返回只在当前保护窗口内从借用组读取，数值可与写侧并发近似变化。
 */
static inline unsigned long group_faults(struct task_struct *p, int nid)
{
	struct numa_group *ng = deref_task_numa_group(p);

	if (!ng)
		return 0;

	return ng->faults[task_faults_idx(NUMA_MEM, nid, 0)] +
		ng->faults[task_faults_idx(NUMA_MEM, nid, 1)];
}

/*
 * group_faults_cpu() - 汇总 NUMA 组在指定节点的 CPU-weighted fault。
 *
 * 业务背景：该视角强调哪些节点真正消费 CPU，用于决定内存向计算位置靠拢。
 * 入参：@group 是受锁/RCU 稳定的借用组；@nid 是节点编号。
 * 出参/返回：返回共享与私有 CPU fault 之和；无副作用。
 * 注意事项：调用者负责 group 生命周期和节点范围合法性。
 */
static inline unsigned long group_faults_cpu(struct numa_group *group, int nid)
{
	return group->faults[task_faults_idx(NUMA_CPU, nid, 0)] +
		group->faults[task_faults_idx(NUMA_CPU, nid, 1)];
}

/*
 * group_faults_priv() - 汇总 NUMA 组全部在线节点的私有 memory fault。
 *
 * 业务背景：扫描周期用 private/shared 比例判断每线程独立采样的价值。
 * 入参：@ng 是调用者稳定的借用组。
 * 出参/返回：返回在线节点私有 fault 总数；不修改组。
 * 注意事项：在线节点集合与统计写入可变化，结果是启发式快照。
 */
static inline unsigned long group_faults_priv(struct numa_group *ng)
{
	unsigned long faults = 0;
	int node;

	for_each_online_node(node) {
		faults += ng->faults[task_faults_idx(NUMA_MEM, node, 1)];
	}

	return faults;
}

/*
 * group_faults_shared() - 汇总 NUMA 组全部在线节点的共享 memory fault。
 *
 * 业务背景：共享占比越高，多个线程重复扫描同一 mm 的收益越低，应放慢周期。
 * 入参：@ng 是调用者稳定的借用组。
 * 出参/返回：返回在线节点共享 fault 总数；无所有权或状态变化。
 * 注意事项：与 private helper 使用同一节点快照才适合计算比例。
 */
static inline unsigned long group_faults_shared(struct numa_group *ng)
{
	unsigned long faults = 0;
	int node;

	for_each_online_node(node) {
		faults += ng->faults[task_faults_idx(NUMA_MEM, node, 0)];
	}

	return faults;
}

/*
 * A node triggering more than 1/3 as many NUMA faults as the maximum is
 * considered part of a numa group's pseudo-interleaving set. Migrations
 * between these nodes are slowed down, to allow things to settle down.
 */
/* fault 超过组最大节点三分之一的节点属于伪交错活跃集；集合内迁移降速以等待采样稳定。 */
#define ACTIVE_NODE_FRACTION 3

/*
 * numa_is_active_node() - 判断节点是否属于组的伪交错活跃集合。
 *
 * 业务背景：多节点共享工作集不应在活跃节点间频繁迁移；以最大 CPU fault 的三分之一为门槛。
 * 入参：@nid 是节点编号；@ng 是受保护的借用 NUMA 组。
 * 出参/返回：节点 CPU fault 严格超过阈值返回 true；无副作用。
 * 注意事项：max_faults_cpu 与数组需来自同一组统计代次，结果仅用于迁移降速启发式。
 */
static bool numa_is_active_node(int nid, struct numa_group *ng)
{
	return group_faults_cpu(ng, nid) * ACTIVE_NODE_FRACTION > ng->max_faults_cpu;
}

/* Handle placement on systems where not all nodes are directly connected. */
/* 在并非所有节点直接互连的拓扑上，把邻近节点访问证据纳入目标节点评分。 */
/*
 * score_nearby_nodes() - 按 NUMA 距离和拓扑类型累计目标节点附近的 fault 分数。
 *
 * 业务背景：复杂 NUMA 上单节点 fault 不足以代表一组互联节点的局部性，放置需评价邻域。
 * 入参：@p 是借用 task；@nid 是候选节点；@lim_dist 是比较跳距；@task 选择 task 或 group 统计。
 * 出参/返回：返回邻近节点加权 fault 分数；直接互连拓扑返回 0，无状态变化。
 * 注意事项：O(N^2) 由外层逐节点调用形成，依赖实际节点数通常很小；拓扑全局值可并发更新。
 */
static unsigned long score_nearby_nodes(struct task_struct *p, int nid,
					int lim_dist, bool task)
{
	unsigned long score = 0;
	int node, max_dist;

	/*
	 * All nodes are directly connected, and the same distance
	 * from each other. No need for fancy placement algorithms.
	 */
	/* 全互连且等距时邻域评分对所有候选相同，直接返回零避免无意义扫描。 */
	if (sched_numa_topology_type == NUMA_DIRECT)
		return 0;

	/* sched_max_numa_distance may be changed in parallel. */
	/* READ_ONCE 取得一次一致快照，允许本轮评分使用稍旧最大距离。 */
	max_dist = READ_ONCE(sched_max_numa_distance);
	/*
	 * This code is called for each node, introducing N^2 complexity,
	 * which should be OK given the number of nodes rarely exceeds 8.
	 */
	/* 外层对每候选调用导致平方复杂度，但常见 NUMA 节点数不超过 8，成本可接受。 */
	for_each_online_node(node) {
		unsigned long faults;
		int dist = node_distance(nid, node);

		/*
		 * The furthest away nodes in the system are not interesting
		 * for placement; nid was already counted.
		 */
		/* 排除最远节点和已由主分数计入的 nid，避免重复或无关流量影响局部邻域。 */
		if (dist >= max_dist || node == nid)
			continue;

		/*
		 * On systems with a backplane NUMA topology, compare groups
		 * of nodes, and move tasks towards the group with the most
		 * memory accesses. When comparing two nodes at distance
		 * "hoplimit", only nodes closer by than "hoplimit" are part
		 * of each group. Skip other nodes.
		 */
		/* backplane 以 hoplimit 划分候选节点组，距离不小于当前比较界限的节点不属于本组。 */
		if (sched_numa_topology_type == NUMA_BACKPLANE && dist >= lim_dist)
			continue;

		/* Add up the faults from nearby nodes. */
		/* 按调用模式选择单 task 或整个 NUMA 组的访问证据。 */
		if (task)
			faults = task_faults(p, node);
		else
			faults = group_faults(p, node);

		/*
		 * On systems with a glueless mesh NUMA topology, there are
		 * no fixed "groups of nodes". Instead, nodes that are not
		 * directly connected bounce traffic through intermediate
		 * nodes; a numa_group can occupy any set of nodes.
		 * The further away a node is, the less the faults count.
		 * This seems to result in good task placement.
		 */
		/* 无胶合 mesh 没有固定节点组，按距离线性衰减 fault，使更近访问对候选贡献更大。 */
		if (sched_numa_topology_type == NUMA_GLUELESS_MESH) {
			faults *= (max_dist - dist);
			faults /= (max_dist - LOCAL_DISTANCE);
		}

		score += faults;
	}

	return score;
}

/*
 * These return the fraction of accesses done by a particular task, or
 * task group, on a particular numa node.  The group weight is given a
 * larger multiplier, in order to group tasks together that are almost
 * evenly spread out between numa nodes.
 */
/* 返回 task 或组在某节点邻域的千分比访问权重；组权重在上层使用更大倍率，以聚合近似均匀分布的线程。 */
/*
 * task_weight() - 计算单 task 对候选 NUMA 节点的归一化访问权重。
 *
 * 业务背景：NUMA 迁移比较源/目标局部性时，以本节点及邻域 fault 占 task 总 fault 的比例评分。
 * 入参：@p 是借用 task；@nid 是候选节点；@dist 是邻域跳距界限。
 * 出参/返回：返回 0..约1000 的千分比权重；无统计或总数为零时返回 0。
 * 注意事项：邻域可重叠且 mesh 加权，结果是比较分数而非严格概率。
 */
static inline unsigned long task_weight(struct task_struct *p, int nid,
					int dist)
{
	unsigned long faults, total_faults;

	if (!p->numa_faults)
		return 0;

	total_faults = p->total_numa_faults;

	if (!total_faults)
		return 0;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	faults = task_faults(p, nid);
	faults += score_nearby_nodes(p, nid, dist, true);

	return 1000 * faults / total_faults;
}

/*
 * group_weight() - 计算 task 所属 NUMA 组对候选节点的归一化访问权重。
 *
 * 业务背景：共享页和协作线程应整体靠近主要访问区域，组迁移决策以聚合 fault 评分。
 * 入参：@p 是满足组解引用条件的借用 task；@nid 为候选节点；@dist 为邻域界限。
 * 出参/返回：无组/总 fault 为零返回 0，否则返回千分比组权重。
 * 注意事项：借用组不增加 refcount，调用者必须保持 RCU/rq 锁保护窗口。
 */
static inline unsigned long group_weight(struct task_struct *p, int nid,
					 int dist)
{
	struct numa_group *ng = deref_task_numa_group(p);
	unsigned long faults, total_faults;

	if (!ng)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return 0;

	total_faults = ng->total_faults;

	if (!total_faults)
		return 0;

	faults = group_faults(p, nid);
	faults += score_nearby_nodes(p, nid, dist, false);

	return 1000 * faults / total_faults;
}

/*
 * If memory tiering mode is enabled, cpupid of slow memory page is
 * used to record scan time instead of CPU and PID.  When tiering mode
 * is disabled at run time, the scan time (in cpupid) will be
 * interpreted as CPU and PID.  So CPU needs to be checked to avoid to
 * access out of array bound.
 */
/*
 * 内存分层开启时，慢内存页的 cpupid 字段改存扫描时间；运行时关闭分层后同一位模式会
 * 被解释成 CPU/PID，因此必须先验证 CPU 部分，避免用时间值索引 per-CPU 数组越界。
 */
/*
 * cpupid_valid() - 验证页上编码值能否安全解释为 CPU/PID。
 *
 * 业务背景：NUMA balancing 与 memory tiering 复用页字段但编码语义不同。
 * 入参：@cpupid 是页上读取的编码整数，仅输入。
 * 出参/返回：CPU 部分小于 nr_cpu_ids 返回 true，否则 false；无副作用。
 * 注意事项：只验证 CPU 索引范围，不证明 PID 仍存在或页记录仍新鲜。
 */
static inline bool cpupid_valid(int cpupid)
{
	return cpupid_to_cpu(cpupid) < nr_cpu_ids;
}

/*
 * For memory tiering mode, if there are enough free pages (more than
 * enough watermark defined here) in fast memory node, to take full
 * advantage of fast memory capacity, all recently accessed slow
 * memory pages will be migrated to fast memory node without
 * considering hot threshold.
 */
/*
 * 分层内存中，若快速节点空闲页超过额外水位，就优先充分利用其容量，把近期访问的慢
 * 内存页全部提升，而不再要求低于 hot latency 阈值。
 */
/*
 * pgdat_free_space_enough() - 判断快速内存节点是否有足够余量放宽 promotion 热度门槛。
 *
 * 业务背景：空闲容量充足时容量利用优先于精细热度筛选，可加速慢页迁入。
 * 入参：@pgdat 是借用节点内存描述，调用者保持节点有效。
 * 出参/返回：任一 populated zone 超过 promotion 水位加额外余量返回 true，否则 false。
 * 注意事项：额外余量取 1GiB 页数与节点页数 1/16 较大者；只判断容量，不执行迁移。
 */
static bool pgdat_free_space_enough(struct pglist_data *pgdat)
{
	int z;
	unsigned long enough_wmark;

	enough_wmark = max(1UL * 1024 * 1024 * 1024 >> PAGE_SHIFT,
			   pgdat->node_present_pages >> 4);
	for (z = pgdat->nr_zones - 1; z >= 0; z--) {
		struct zone *zone = pgdat->node_zones + z;

		if (!populated_zone(zone))
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			continue;

		if (zone_watermark_ok(zone, 0,
				      promo_wmark_pages(zone) + enough_wmark,
				      ZONE_MOVABLE, 0))
			return true;
	}
	return false;
}

/*
 * For memory tiering mode, when page tables are scanned, the scan
 * time will be recorded in struct page in addition to make page
 * PROT_NONE for slow memory page.  So when the page is accessed, in
 * hint page fault handler, the hint page fault latency is calculated
 * via,
 *
 *	hint page fault latency = hint page fault time - scan time
 *
 * The smaller the hint page fault latency, the higher the possibility
 * for the page to be hot.
 */
/*
 * 分层模式扫描慢页时除设 PROT_NONE 外，还把扫描时刻写入 folio；hint fault 时以当前时刻
 * 减扫描时刻得到访问延迟，延迟越短表示页越热。
 */
/*
 * numa_hint_fault_latency() - 原子换取 folio 上次扫描/访问时刻并计算 hint fault 延迟。
 *
 * 业务背景：promotion 策略用毫秒延迟区分近期热页与偶发访问页。
 * 入参：@folio 是锁定/稳定的输入输出 folio 借用指针。
 * 出参/返回：返回按 PAGE_ACCESS_TIME_MASK 回绕的毫秒延迟；副作用是把访问时刻更新为 now。
 * 注意事项：位宽有限会回绕，结果用于阈值启发式而非精确时间测量。
 */
static int numa_hint_fault_latency(struct folio *folio)
{
	int last_time, time;

	time = jiffies_to_msecs(jiffies);
	last_time = folio_xchg_access_time(folio, time);

	return (time - last_time) & PAGE_ACCESS_TIME_MASK;
}

/*
 * For memory tiering mode, too high promotion/demotion throughput may
 * hurt application latency.  So we provide a mechanism to rate limit
 * the number of pages that are tried to be promoted.
 */
/* 分层内存 promotion/demotion 吞吐过高会增加复制与回收延迟，因此按节点限制候选页速率。 */
/*
 * numa_promotion_rate_limit() - 判断节点本秒 promotion 候选增量是否达到速率上限。
 *
 * 业务背景：hint fault 热页不能无限制提升到快速内存，否则迁移带宽反而伤害应用延迟。
 * 入参：@pgdat 是输入输出节点描述；@rate_limit 是每秒页数阈值；@nr 是本次候选页数。
 * 出参/返回：达到阈值返回 true 表示限流，否则 false；同时累计 PGPROMOTE_CANDIDATE。
 * 注意事项：cmpxchg 只允许一个并发 fault 切换秒窗口；计数差值允许近似竞争。
 */
static bool numa_promotion_rate_limit(struct pglist_data *pgdat,
				      unsigned long rate_limit, int nr)
{
	unsigned long nr_cand;
	unsigned int now, start;

	now = jiffies_to_msecs(jiffies);
	mod_node_page_state(pgdat, PGPROMOTE_CANDIDATE, nr);
	nr_cand = node_page_state(pgdat, PGPROMOTE_CANDIDATE);
	start = pgdat->nbp_rl_start;
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (now - start > MSEC_PER_SEC &&
	    cmpxchg(&pgdat->nbp_rl_start, start, now) == start)
		/* 赢得新时间窗的 CPU 保存累计基线，后续以全局候选数差值衡量本窗速率。 */
		pgdat->nbp_rl_nr_cand = nr_cand;
	if (nr_cand - pgdat->nbp_rl_nr_cand >= rate_limit)
		return true;
	return false;
}

#define NUMA_MIGRATION_ADJUST_STEPS	16

/*
 * numa_promotion_adjust_threshold() - 根据实际候选速率反馈调节热页 promotion 阈值。
 *
 * 业务背景：固定 latency 阈值难适配负载；控制器让候选量围绕 rate_limit，避免过载也不浪费带宽。
 * 入参：@pgdat 是输入输出节点；@rate_limit 为每秒速率目标；@ref_th 为基准毫秒阈值。
 * 出参/返回：无直接返回；每个最大扫描周期最多更新一次 nbp_threshold 与计数基线。
 * 注意事项：候选高于目标 110% 时降低阈值变严格，低于 90% 时升高阈值放宽，范围为一单位到 2*ref。
 */
static void numa_promotion_adjust_threshold(struct pglist_data *pgdat,
					    unsigned long rate_limit,
					    unsigned int ref_th)
{
	unsigned int now, start, th_period, unit_th, th;
	unsigned long nr_cand, ref_cand, diff_cand;

	now = jiffies_to_msecs(jiffies);
	th_period = sysctl_numa_balancing_scan_period_max;
	start = pgdat->nbp_th_start;
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (now - start > th_period &&
	    cmpxchg(&pgdat->nbp_th_start, start, now) == start) {
		/* 单个赢得周期切换的 CPU 读取候选增量并执行一次带 10% 死区的反馈调整。 */
		ref_cand = rate_limit *
			sysctl_numa_balancing_scan_period_max / MSEC_PER_SEC;
		nr_cand = node_page_state(pgdat, PGPROMOTE_CANDIDATE);
		diff_cand = nr_cand - pgdat->nbp_th_nr_cand;
		unit_th = ref_th * 2 / NUMA_MIGRATION_ADJUST_STEPS;
		th = pgdat->nbp_threshold ? : ref_th;
		if (diff_cand > ref_cand * 11 / 10)
			/* 候选过多：降低允许延迟阈值，仅更热页面可 promotion。 */
			th = max(th - unit_th, unit_th);
		else if (diff_cand < ref_cand * 9 / 10)
			/* 候选过少：提高阈值放宽热度要求，但不超过基准两倍。 */
			th = min(th + unit_th, ref_th * 2);
		pgdat->nbp_th_nr_cand = nr_cand;
		pgdat->nbp_threshold = th;
	}
}

/*
 * should_numa_migrate_memory() - 判断一次 NUMA hint fault 是否应把 folio 迁向访问 CPU。
 *
 * 业务背景：fault 路径在私有/共享工作集局部性、分层热度、节点容量和迁移带宽之间做决策；
 * 本函数是实际 page migration 前的策略门禁。
 * 入参：@p 必须是 current task；@folio 是稳定的 fault folio；@src_nid 为当前节点；@dst_cpu 为访问 CPU。
 * 出参/返回：允许迁移返回 true，否则 false；副作用包括更新 folio cpupid/访问时刻和 promotion 统计。
 * 注意事项：NUMA 组为 RCU 借用；分层模式与普通模式复用页字段，必须按当前编码语义分流。
 */
bool should_numa_migrate_memory(struct task_struct *p, struct folio *folio,
				int src_nid, int dst_cpu)
{
	struct numa_group *ng = deref_curr_numa_group(p);
	int dst_nid = cpu_to_node(dst_cpu);
	int last_cpupid, this_cpupid;

	/*
	 * Cannot migrate to memoryless nodes.
	 */
	/* 目标节点没有可分配内存时，即使 CPU 在该节点也无法完成页迁移。 */
	if (!node_state(dst_nid, N_MEMORY))
		return false;

	/*
	 * The pages in slow memory node should be migrated according
	 * to hot/cold instead of private/shared.
	 */
	/* 慢层页按扫描后访问延迟判冷热，不使用普通模式的 private/shared cpupid 关系。 */
	if (folio_use_access_time(folio)) {
		struct pglist_data *pgdat;
		unsigned long rate_limit;
		unsigned int latency, th, def_th;
		long nr = folio_nr_pages(folio);

		pgdat = NODE_DATA(dst_nid);
		if (pgdat_free_space_enough(pgdat)) {
			/* workload changed, reset hot threshold */
			/* 快速节点余量充足说明约束改变，清除自适应阈值并无条件把本 folio 作为候选。 */
			pgdat->nbp_threshold = 0;
			mod_node_page_state(pgdat, PGPROMOTE_CANDIDATE_NRL, nr);
			return true;
		}

		def_th = sysctl_numa_balancing_hot_threshold;
		rate_limit = MB_TO_PAGES(sysctl_numa_balancing_promote_rate_limit);
		numa_promotion_adjust_threshold(pgdat, rate_limit, def_th);

		th = pgdat->nbp_threshold ? : def_th;
		/* 先按延迟过滤冷页，再用每节点候选速率限制实际 promotion 压力。 */
		latency = numa_hint_fault_latency(folio);
		if (latency >= th)
			return false;

		return !numa_promotion_rate_limit(pgdat, rate_limit, nr);
	}

	this_cpupid = cpu_pid_to_cpupid(dst_cpu, current->pid);
	/* 普通模式原子发布本次访问者，并取回上一次 CPU/PID 样本构造两阶段关系。 */
	last_cpupid = folio_xchg_last_cpupid(folio, this_cpupid);

	if (!(sysctl_numa_balancing_mode & NUMA_BALANCING_MEMORY_TIERING) &&
	    !node_is_toptier(src_nid) && !cpupid_valid(last_cpupid))
		return false;

	/*
	 * Allow first faults or private faults to migrate immediately early in
	 * the lifetime of a task. The magic number 4 is based on waiting for
	 * two full passes of the "multi-stage node selection" test that is
	 * executed below.
	 */
	/* 初始四轮扫描内，首次 fault 或同 PID 私有 fault 直接迁移，加速尚未建立稳定样本的任务。 */
	if ((p->numa_preferred_nid == NUMA_NO_NODE || p->numa_scan_seq <= 4) &&
	    (cpupid_pid_unset(last_cpupid) || cpupid_match_pid(p, last_cpupid)))
		return true;

	/*
	 * Multi-stage node selection is used in conjunction with a periodic
	 * migration fault to build a temporal task<->page relation. By using
	 * a two-stage filter we remove short/unlikely relations.
	 *
	 * Using P(p) ~ n_p / n_t as per frequentist probability, we can equate
	 * a task's usage of a particular page (n_p) per total usage of this
	 * page (n_t) (in a given time-span) to a probability.
	 *
	 * Our periodic faults will sample this probability and getting the
	 * same result twice in a row, given these samples are fully
	 * independent, is then given by P(n)^2, provided our sample period
	 * is sufficiently short compared to the usage pattern.
	 *
	 * This quadric squishes small probabilities, making it less likely we
	 * act on an unlikely task<->page relation.
	 */
	/*
	 * 周期 hint fault 对 task-page 关系采样；连续两次落在同节点的概率近似 P²，会强烈
	 * 压低偶发小概率关系。上次样本来自别节点时本轮不迁移，等待下一样本确认时间局部性。
	 */
	if (!cpupid_pid_unset(last_cpupid) &&
				cpupid_to_nid(last_cpupid) != dst_nid)
		return false;

	/* Always allow migrate on private faults */
	/* 同 PID 再次访问证明私有关系，立即向当前 CPU 节点迁移。 */
	if (cpupid_match_pid(p, last_cpupid))
		return true;

	/* A shared fault, but p->numa_group has not been set up yet. */
	/* 共享 fault 尚无组统计可供比较时先允许迁移，后续采样会建立并修正组关系。 */
	if (!ng)
		return true;

	/*
	 * Destination node is much more heavily used than the source
	 * node? Allow migration.
	 */
	/* 组在目标的 CPU fault 超过源三倍，说明计算明显偏向目标，可直接迁移内存。 */
	if (group_faults_cpu(ng, dst_nid) > group_faults_cpu(ng, src_nid) *
					ACTIVE_NODE_FRACTION)
		return true;

	/*
	 * Distribute memory according to CPU & memory use on each node,
	 * with 3/4 hysteresis to avoid unnecessary memory migrations:
	 *
	 * faults_cpu(dst)   3   faults_cpu(src)
	 * --------------- * - > ---------------
	 * faults_mem(dst)   4   faults_mem(src)
	 */
	/* 其余共享页按 CPU 使用/内存分布交叉乘比较，并以 3/4 滞回避免边界附近反复迁移。 */
	return group_faults_cpu(ng, dst_nid) * group_faults(p, src_nid) * 3 >
	       group_faults_cpu(ng, src_nid) * group_faults(p, dst_nid) * 4;
}

/*
 * 'numa_type' describes the node at the moment of load balancing.
 */
/* numa_type 描述本次负载平衡快照中的节点容量状态，不是节点的永久属性。 */
enum numa_type {
	/* The node has spare capacity that can be used to run more tasks.  */
	/* 运行数或利用率低于容量阈值，可接收更多任务而不挤压现有份额。 */
	node_has_spare = 0,
	/*
	 * The node is fully used and the tasks don't compete for more CPU
	 * cycles. Nevertheless, some tasks might wait before running.
	 */
	/* CPU 基本满用但任务尚未争抢额外周期；排队延迟仍可能存在。 */
	node_fully_busy,
	/*
	 * The node is overloaded and can't provide expected CPU cycles to all
	 * tasks.
	 */
	/* 需求超过计算容量，无法向所有任务提供期望 CPU 周期。 */
	node_overloaded
};

/* Cached statistics for all CPUs within a node */
/* 本轮扫描汇总一个节点内全部 CPU 的负载、容量和空闲候选，离开决策后即失效。 */
struct numa_stats {
	/* 负载跟踪值、runnable 需求和 CFS util，均用于不同维度的容量归一化比较。 */
	unsigned long load;
	unsigned long runnable;
	unsigned long util;
	/* Total compute capacity of CPUs on a node */
	/* 节点所有 CPU 架构容量之和，是 load/util 比较的分母。 */
	unsigned long compute_capacity;
	/* 层级 CFS runnable 实体数和节点 CPU 数。 */
	unsigned int nr_running;
	unsigned int weight;
	/* 由 numa_classify() 派生的本轮容量类别。 */
	enum numa_type node_type;
	/* 可用于迁移的空闲 CPU，优先完整空闲 core；-1 表示无候选。 */
	int idle_cpu;
};

/* 一次 NUMA 放置搜索的输入、最佳候选和源/目标负载快照，不跨调用保存。 */
struct task_numa_env {
	/* 当前考虑迁移的 task，调用期间由迁移路径持有引用。 */
	struct task_struct *p;

	/* 源/当前候选目标 CPU 与节点编号。 */
	int src_cpu, src_nid;
	int dst_cpu, dst_nid;
	/* NUMA 轻载允许的不均衡阈值。 */
	int imb_numa_nr;

	/* 源与目标节点的本轮负载容量快照。 */
	struct numa_stats src_stats, dst_stats;

	/* 负载平衡容忍百分比与源目标节点间距离。 */
	int imbalance_pct;
	int dist;

	/* 当前最佳 swap 对端；持有 task 引用，替换/退出时必须 put。 */
	struct task_struct *best_task;
	/* 最佳 NUMA 改善分数和对应目标 CPU，-1 表示尚无可行方案。 */
	long best_imp;
	int best_cpu;
};

static unsigned long cpu_load(struct rq *rq);
static unsigned long cpu_runnable(struct rq *rq);

/*
 * numa_classify() - 按任务数、util、runnable 与容量把节点归入三种负载状态。
 *
 * 业务背景：task_numa_compare() 需遵守普通 load balancer 容量约束，不能只追求内存局部性。
 * 入参：@imbalance_pct 是容量容忍百分比；@ns 是本轮只读节点统计借用指针。
 * 出参/返回：返回 node_overloaded/node_has_spare/node_fully_busy；无副作用。
 * 注意事项：多指标必须同时满足对应阈值，边界状态落入 fully_busy。
 */
static inline enum
numa_type numa_classify(unsigned int imbalance_pct,
			 struct numa_stats *ns)
{
	if ((ns->nr_running > ns->weight) &&
	    (((ns->compute_capacity * 100) < (ns->util * imbalance_pct)) ||
	     ((ns->compute_capacity * imbalance_pct) < (ns->runnable * 100))))
		return node_overloaded;

	if ((ns->nr_running < ns->weight) ||
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	    (((ns->compute_capacity * 100) > (ns->util * imbalance_pct)) &&
	     ((ns->compute_capacity * imbalance_pct) > (ns->runnable * 100))))
		return node_has_spare;

	return node_fully_busy;
}

/* Forward declarations of select_idle_sibling helpers */
/* 前置声明复用 wakeup 选核的 idle-core 可用性提示，避免 NUMA 路径复制拓扑判断。 */
static inline bool test_idle_cores(int cpu);
/*
 * numa_idle_core() - 在空闲 CPU 候选中优先记住完整空闲的 SMT core。
 *
 * 业务背景：把任务塞到已有忙 sibling 会共享执行资源并很快触发再平衡，完整空闲 core 更优。
 * 入参：@idle_core 是已有候选或 -1；@cpu 是新空闲 CPU 候选。
 * 出参/返回：返回原候选，或在 SMT 激活且 core 全空闲时返回 @cpu；无状态变化。
 * 注意事项：test_idle_cores() 快速提示为 false 时跳过昂贵 sibling 扫描；结果可立即过时。
 */
static inline int numa_idle_core(int idle_core, int cpu)
{
	if (!sched_smt_active() ||
	    idle_core >= 0 || !test_idle_cores(cpu))
		return idle_core;

	/*
	 * Prefer cores instead of packing HT siblings
	 * and triggering future load balancing.
	 */
	/* 优先完整空闲 core，避免打包到 HT sibling 后因共享资源拥挤再次触发负载平衡。 */
	if (is_core_idle(cpu))
		idle_core = cpu;

	return idle_core;
}

/*
 * Gather all necessary information to make NUMA balancing placement
 * decisions that are compatible with standard load balancer. This
 * borrows code and logic from update_sg_lb_stats but sharing a
 * common implementation is impractical.
 */
/*
 * 收集兼容普通 load balancer 的 NUMA 放置输入；逻辑借鉴 update_sg_lb_stats，但对象和
 * 调用频率不同，强行共用实现会增加接口复杂度。
 */
/*
 * update_numa_stats() - 汇总节点 CPU 负载/容量并寻找可迁移的最佳空闲 CPU。
 *
 * 业务背景：每个目标节点比较前需形成同一口径快照，使 NUMA 局部性优化不破坏 CPU 均衡。
 * 入参：@env 提供 task/阈值；@ns 是输出统计；@nid 为节点；@find_idle 控制是否搜索空闲 CPU。
 * 出参/返回：无直接返回；完全覆盖 @ns，不转移 task 或 rq 所有权。
 * 注意事项：RCU 稳定 sched_domain，rq 字段为无锁近似；numa_migrate_on 原子位排除并发迁移占用。
 */
static void update_numa_stats(struct task_numa_env *env,
			      struct numa_stats *ns, int nid,
			      bool find_idle)
{
	int cpu, idle_core = -1;

	memset(ns, 0, sizeof(*ns));
	ns->idle_cpu = -1;

	rcu_read_lock();
	/* 遍历节点 CPU 累加三类需求与架构容量，同时筛选 affinity 允许且未被迁移占用的空闲 CPU。 */
	for_each_cpu(cpu, cpumask_of_node(nid)) {
		struct rq *rq = cpu_rq(cpu);

		ns->load += cpu_load(rq);
		ns->runnable += cpu_runnable(rq);
		ns->util += cpu_util_cfs(cpu);
		ns->nr_running += rq->cfs.h_nr_runnable;
		ns->compute_capacity += capacity_of(cpu);

		if (find_idle && idle_core < 0 && !rq->nr_running && idle_cpu(cpu)) {
			if (READ_ONCE(rq->numa_migrate_on) ||
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			    !cpumask_test_cpu(cpu, env->p->cpus_ptr))
				continue;

			if (ns->idle_cpu == -1)
				ns->idle_cpu = cpu;

			idle_core = numa_idle_core(idle_core, cpu);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		}
	}
	rcu_read_unlock();

	ns->weight = cpumask_weight(cpumask_of_node(nid));

	ns->node_type = numa_classify(env->imbalance_pct, ns);

	/* 若找到完整空闲 core，用它覆盖先遇到的任意空闲 sibling。 */
	if (idle_core >= 0)
		ns->idle_cpu = idle_core;
}

/*
 * task_numa_assign() - 把当前候选方案发布为环境中的最佳迁移或 swap 选择。
 *
 * 业务背景：比较多个 CPU 时需原子认领目标 rq，防止两个 NUMA 平衡实例同时向同一 CPU 迁入。
 * 入参：@env 是输入输出搜索环境；@p 是可空 swap 对端；@imp 是新方案改善分数。
 * 出参/返回：无直接返回；更新 best_*，对 best_task 进行 put/get 引用转移并设置目标 rq 标记。
 * 注意事项：找不到替代空闲 CPU 时保留旧最佳；最终调用者必须清除标记并释放 best_task 引用。
 */
static void task_numa_assign(struct task_numa_env *env,
			     struct task_struct *p, long imp)
{
	struct rq *rq = cpu_rq(env->dst_cpu);

	/* Check if run-queue part of active NUMA balance. */
	/* xchg 原子认领目标 rq；已被其他平衡占用时，在同节点继续寻找可用空闲 CPU。 */
	if (env->best_cpu != env->dst_cpu && xchg(&rq->numa_migrate_on, 1)) {
		int cpu;
		int start = env->dst_cpu;

		/* Find alternative idle CPU. */
		/* 环形遍历跳过当前最佳、非空闲或 affinity 禁止 CPU，并尝试认领其迁移标记。 */
		for_each_cpu_wrap(cpu, cpumask_of_node(env->dst_nid), start + 1) {
			if (cpu == env->best_cpu || !idle_cpu(cpu) ||
			    !cpumask_test_cpu(cpu, env->p->cpus_ptr)) {
				continue;
			}

			env->dst_cpu = cpu;
			rq = cpu_rq(env->dst_cpu);
			if (!xchg(&rq->numa_migrate_on, 1))
				goto assign;
		}

		/* Failed to find an alternative idle CPU */
		/* 本候选无法取得独占目标，直接返回且不破坏此前最佳方案。 */
		return;
	}

assign:
	/*
	 * Clear previous best_cpu/rq numa-migrate flag, since task now
	 * found a better CPU to move/swap.
	 */
	/* 新方案胜出后释放旧 best_cpu 的迁移占用位，避免 rq 永久被标记为迁移中。 */
	if (env->best_cpu != -1 && env->best_cpu != env->dst_cpu) {
		rq = cpu_rq(env->best_cpu);
		WRITE_ONCE(rq->numa_migrate_on, 0);
	}

	if (env->best_task)
		put_task_struct(env->best_task);
	if (p)
		get_task_struct(p);
	/* best_task 引用随候选替换转移，保证搜索离开 rq/RCU 后 swap 对端仍存活。 */

	env->best_task = p;
	env->best_imp = imp;
	env->best_cpu = env->dst_cpu;
}

/*
 * load_too_imbalanced() - 判断候选 move/swap 是否恶化源目标的容量归一化负载差。
 *
 * 业务背景：NUMA 改善不能以更严重 CPU 负载失衡为代价；比较迁移前后交叉乘差值。
 * 入参：@src_load、@dst_load 是候选后的负载；@env 提供原负载和两节点容量。
 * 出参/返回：新绝对不平衡大于旧值返回 true，否则 false；无副作用。
 * 注意事项：交叉乘避免整数除法精度损失，输入范围依赖负载统计不溢出 long。
 */
static bool load_too_imbalanced(long src_load, long dst_load,
				struct task_numa_env *env)
{
	long imb, old_imb;
	long orig_src_load, orig_dst_load;
	long src_capacity, dst_capacity;

	/*
	 * The load is corrected for the CPU capacity available on each node.
	 *
	 * src_load        dst_load
	 * ------------ vs ---------
	 * src_capacity    dst_capacity
	 */
	/* 用 load/capacity 而非绝对 load 比较异构节点；交叉乘后无需除法。 */
	src_capacity = env->src_stats.compute_capacity;
	dst_capacity = env->dst_stats.compute_capacity;

	imb = abs(dst_load * src_capacity - src_load * dst_capacity);

	orig_src_load = env->src_stats.load;
	orig_dst_load = env->dst_stats.load;

	old_imb = abs(orig_dst_load * src_capacity - orig_src_load * dst_capacity);

	/* Would this change make things worse? */
	/* 只拒绝严格变差；相等负载平衡允许由 NUMA 局部性收益决定。 */
	return (imb > old_imb);
}

/*
 * Maximum NUMA importance can be 1998 (2*999);
 * SMALLIMP @ 30 would be close to 1998/64.
 * Used to deter task migration.
 */
/* NUMA 改善最大约 1998；30 接近其 1/64，作为抑制微小收益迁移和 cache ping-pong 的门槛。 */
#define SMALLIMP	30

/*
 * This checks if the overall compute and NUMA accesses of the system would
 * be improved if the source tasks was migrated to the target dst_cpu taking
 * into account that it might be best if task running on the dst_cpu should
 * be exchanged with the source task
 */
/*
 * 比较把源 task 直接迁到 dst_cpu 与和 dst 当前 task 交换两种方案，只有 CPU 负载与
 * NUMA 访问总体改善时才更新最佳候选。
 */
/*
 * task_numa_compare() - 评价一个目标 CPU 的 move/swap 收益并可能更新最佳方案。
 *
 * 业务背景：task_numa_find_cpu() 逐 CPU 调用，在局部性收益、affinity、节点容量和负载均衡间筛选。
 * 入参：@env 是输入输出搜索环境；@taskimp/@groupimp 是源 task 的节点改善分；@maymove 允许空位直迁。
 * 出参/返回：找到足够好的终止候选返回 true，否则 false；可能认领目标 rq 并持有 best_task 引用。
 * 注意事项：RCU 只保护 dst curr/NUMA 组读取；preempt 开启时 current 可迁移，必须防自交换。
 */
static bool task_numa_compare(struct task_numa_env *env,
			      long taskimp, long groupimp, bool maymove)
{
	struct numa_group *cur_ng, *p_ng = deref_curr_numa_group(env->p);
	struct rq *dst_rq = cpu_rq(env->dst_cpu);
	long imp = p_ng ? groupimp : taskimp;
	struct task_struct *cur;
	long src_load, dst_load;
	int dist = env->dist;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	long moveimp = imp;
	long load;
	bool stopsearch = false;

	if (READ_ONCE(dst_rq->numa_migrate_on))
		return false;

	/* RCU 下读取目标 rq 当前 task；退出中、内核线程或无 mm 的对象不适合作为 NUMA swap 对端。 */
	rcu_read_lock();
	cur = rcu_dereference_all(dst_rq->curr);
	if (cur && ((cur->flags & (PF_EXITING | PF_KTHREAD)) ||
		    !cur->mm))
		cur = NULL;

	/*
	 * Because we have preemption enabled we can get migrated around and
	 * end try selecting ourselves (current == env->p) as a swap candidate.
	 */
	/* 搜索期间源 task 自身可能被抢占迁到目标 rq；遇到自对象时停止，避免与自己交换。 */
	if (cur == env->p) {
		stopsearch = true;
		goto unlock;
	}

	if (!cur) {
		/* 空 CPU 只需评价直迁；收益不差于当前最佳且 maymove 时进入统一 assign。 */
		if (maymove && moveimp >= env->best_imp)
			goto assign;
		else
			goto unlock;
	}

	/* Skip this swap candidate if cannot move to the source cpu. */
	/* swap 要求目标 task 的 affinity 也允许源 CPU，否则交换无法完成。 */
	if (!cpumask_test_cpu(env->src_cpu, cur->cpus_ptr))
		goto unlock;

	/*
	 * Skip this swap candidate if it is not moving to its preferred
	 * node and the best task is.
	 */
	/* 已有候选能让对端回到首选节点时，不用一个不能回首选节点的候选替换它。 */
	if (env->best_task &&
	    env->best_task->numa_preferred_nid == env->src_nid &&
	    cur->numa_preferred_nid != env->src_nid) {
		goto unlock;
	}

	/*
	 * "imp" is the fault differential for the source task between the
	 * source and destination node. Calculate the total differential for
	 * the source task and potential destination task. The more negative
	 * the value is, the more remote accesses that would be expected to
	 * be incurred if the tasks were swapped.
	 *
	 * If dst and source tasks are in the same NUMA group, or not
	 * in any group then look only at task weights.
	 */
	/* imp 合并源与目标 task 的 fault 差；越负表示交换后预计远程访问越多。同组或都无组时只比较个体。 */
	cur_ng = rcu_dereference_all(cur->numa_group);
	if (cur_ng == p_ng) {
		/*
		 * Do not swap within a group or between tasks that have
		 * no group if there is spare capacity. Swapping does
		 * not address the load imbalance and helps one task at
		 * the cost of punishing another.
		 */
		/* 目标有空余容量时，同组/无组交换只是一得一失且不改善负载，直接拒绝。 */
		if (env->dst_stats.node_type == node_has_spare)
			goto unlock;

		imp = taskimp + task_weight(cur, env->src_nid, dist) -
		      task_weight(cur, env->dst_nid, dist);
		/*
		 * Add some hysteresis to prevent swapping the
		 * tasks within a group over tiny differences.
		 */
		/* 同组 swap 对改善值打 1/16 折扣，防止微小统计差异引发成员来回交换。 */
		if (cur_ng)
			imp -= imp / 16;
	} else {
		/*
		 * Compare the group weights. If a task is all by itself
		 * (not part of a group), use the task weight instead.
		 */
		/* 双方都有不同组时用组权重；任一为独立 task 时改用其个体权重。 */
		if (cur_ng && p_ng)
			imp += group_weight(cur, env->src_nid, dist) -
			       group_weight(cur, env->dst_nid, dist);
		else
			imp += task_weight(cur, env->src_nid, dist) -
			       task_weight(cur, env->dst_nid, dist);
	}

	/* Discourage picking a task already on its preferred node */
	/* 对端已在首选节点时交换会破坏其局部性，给方案 1/16 惩罚。 */
	if (cur->numa_preferred_nid == env->dst_nid)
		imp -= imp / 16;

	/*
	 * Encourage picking a task that moves to its preferred node.
	 * This potentially makes imp larger than it's maximum of
	 * 1998 (see SMALLIMP and task_weight for why) but in this
	 * case, it does not matter.
	 */
	/* 对端交换后能到首选源节点时奖励 1/8；即便超过理论 1998 上界也只影响相对排序。 */
	if (cur->numa_preferred_nid == env->src_nid)
		imp += imp / 8;

	if (maymove && moveimp > imp && moveimp > env->best_imp) {
		/* 空位直迁同时优于 swap 和历史最佳时，清空 cur 表达“不交换”。 */
		imp = moveimp;
		cur = NULL;
		goto assign;
	}

	/*
	 * Prefer swapping with a task moving to its preferred node over a
	 * task that is not.
	 */
	/* 若新 swap 能让对端回首选而旧候选不能，即使分数接近也优先新方案。 */
	if (env->best_task && cur->numa_preferred_nid == env->src_nid &&
	    env->best_task->numa_preferred_nid != env->src_nid) {
		goto assign;
	}

	/*
	 * If the NUMA importance is less than SMALLIMP,
	 * task migration might only result in ping pong
	 * of tasks and also hurt performance due to cache
	 * misses.
	 */
	/* 收益低于绝对门槛或未显著超过旧最佳时拒绝，给采样噪声和迁移 cache 成本留滞回。 */
	if (imp < SMALLIMP || imp <= env->best_imp + SMALLIMP / 2)
		goto unlock;

	/*
	 * In the overloaded case, try and keep the load balanced.
	 */
	/* swap 两个层级 load 不同时，计算源目标新负载并拒绝任何更差的容量归一化不平衡。 */
	load = task_h_load(env->p) - task_h_load(cur);
	if (!load)
		goto assign;

	dst_load = env->dst_stats.load + load;
	src_load = env->src_stats.load - load;

	if (load_too_imbalanced(src_load, dst_load, env))
		goto unlock;

assign:
	/* Evaluate an idle CPU for a task numa move. */
	/* 直迁方案优先使用节点统计缓存的空闲 CPU；swap 则固定使用当前 dst_cpu。 */
	if (!cur) {
		int cpu = env->dst_stats.idle_cpu;

		/* Nothing cached so current CPU went idle since the search. */
		/* 没缓存空闲点时仍试当前候选，它可能在扫描后刚变空闲。 */
		if (cpu < 0)
			cpu = env->dst_cpu;

		/*
		 * If the CPU is no longer truly idle and the previous best CPU
		 * is, keep using it.
		 */
		/* 状态可在扫描后变化；新点已忙而旧最佳仍空闲时保留旧点，避免候选质量倒退。 */
		if (!idle_cpu(cpu) && env->best_cpu >= 0 &&
		    idle_cpu(env->best_cpu)) {
			cpu = env->best_cpu;
		}

		env->dst_cpu = cpu;
	}

	task_numa_assign(env, cur, imp);

	/*
	 * If a move to idle is allowed because there is capacity or load
	 * balance improves then stop the search. While a better swap
	 * candidate may exist, a search is not free.
	 */
	/* 找到允许直迁的真实空闲 CPU 后提前停止；继续找潜在更优 swap 的扫描成本不值得。 */
	if (maymove && !cur && env->best_cpu >= 0 && idle_cpu(env->best_cpu))
		stopsearch = true;

	/*
	 * If a swap candidate must be identified and the current best task
	 * moves its preferred node then stop the search.
	 */
	/* 不能直迁时，若 swap 已让对端回到首选节点，也认为质量足够并终止。 */
	if (!maymove && env->best_task &&
	    env->best_task->numa_preferred_nid == env->src_nid) {
		stopsearch = true;
	}
unlock:
	rcu_read_unlock();

	return stopsearch;
}

/*
 * task_numa_find_cpu() - 在目标节点允许 CPU 中搜索最佳直迁或交换位置。
 *
 * 业务背景：节点级 fault 改善已知后，还需逐 CPU 遵守 affinity、空闲容量和普通负载均衡约束。
 * 入参：@env 是输入输出搜索环境；@taskimp/@groupimp 是迁到当前 dst_nid 的改善分。
 * 出参/返回：无直接返回；可能通过 task_numa_assign() 更新并认领 env->best_*。
 * 注意事项：节点有余量时优先直迁空闲 CPU，满载时仅在不恶化容量平衡时允许 move。
 */
static void task_numa_find_cpu(struct task_numa_env *env,
				long taskimp, long groupimp)
{
	bool maymove = false;
	int cpu;

	/*
	 * If dst node has spare capacity, then check if there is an
	 * imbalance that would be overruled by the load balancer.
	 */
	/* 有余量仍需预测任务数差，避免 NUMA 迁移立刻被普通 load balancer 反向纠正。 */
	if (env->dst_stats.node_type == node_has_spare) {
		unsigned int imbalance;
		int src_running, dst_running;

		/*
		 * Would movement cause an imbalance? Note that if src has
		 * more running tasks that the imbalance is ignored as the
		 * move improves the imbalance from the perspective of the
		 * CPU load balancer.
		 * */
		/* 模拟源减一、目标加一；源本来更忙时目标-源为负并钳零，视为改善普通均衡。 */
		src_running = env->src_stats.nr_running - 1;
		dst_running = env->dst_stats.nr_running + 1;
		imbalance = max(0, dst_running - src_running);
		imbalance = adjust_numa_imbalance(imbalance, dst_running,
						  env->imb_numa_nr);

		/* Use idle CPU if there is no imbalance */
		/* 无任务数失衡且已有空闲 CPU 时可直接认领零额外分候选并结束节点扫描。 */
		if (!imbalance) {
			maymove = true;
			if (env->dst_stats.idle_cpu >= 0) {
				env->dst_cpu = env->dst_stats.idle_cpu;
				task_numa_assign(env, NULL, 0);
				return;
			}
		}
	} else {
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		long src_load, dst_load, load;
		/*
		 * If the improvement from just moving env->p direction is better
		 * than swapping tasks around, check if a move is possible.
		 */
		/* 非 spare 节点模拟源 task 负载直迁，只有不恶化归一化负载差才设置 maymove。 */
		load = task_h_load(env->p);
		dst_load = env->dst_stats.load + load;
		src_load = env->src_stats.load - load;
		maymove = !load_too_imbalanced(src_load, dst_load, env);
	}

	/* Skip CPUs if the source task cannot migrate */
	/* 仅遍历目标节点与 task affinity 的交集；compare 可在找到足够候选时提前终止。 */
	for_each_cpu_and(cpu, cpumask_of_node(env->dst_nid), env->p->cpus_ptr) {
		env->dst_cpu = cpu;
		if (task_numa_compare(env, taskimp, groupimp, maymove))
			break;
	}
}

/* 汇总节点/CPU 候选收益后执行一次锁后复验迁移；无改善或竞态失败保持原 CPU。 */
/*
 * task_numa_migrate() - 为 current task 搜索并执行一次 NUMA 直迁或任务交换。
 *
 * 业务背景：task_numa_placement() 更新首选节点后调用；它遍历首选及其他有益节点，
 * 结合 fault 权重、CPU 容量和 affinity 选择方案，最后交给迁移核心复验执行。
 * 入参：@p 必须是 current 且生命周期稳定的输入输出 task。
 * 出参/返回：成功返回 0；无域返回 -EINVAL；无更优点返回 -EAGAIN；迁移失败返回对应 errno。
 * 注意事项：best_task 持引用、best rq 持 numa_migrate_on 认领位，所有出口必须配对释放/清除。
 */
static int task_numa_migrate(struct task_struct *p)
{
	struct task_numa_env env = {
		.p = p,

		.src_cpu = task_cpu(p),
		.src_nid = task_node(p),

		.imbalance_pct = 112,

		.best_task = NULL,
		.best_imp = 0,
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		.best_cpu = -1,
	};
	unsigned long taskweight, groupweight;
	struct sched_domain *sd;
	long taskimp, groupimp;
	struct numa_group *ng;
	struct rq *best_rq;
	int nid, ret, dist;

	/*
	 * Pick the lowest SD_NUMA domain, as that would have the smallest
	 * imbalance and would be the first to start moving tasks about.
	 *
	 * And we want to avoid any moving of tasks about, as that would create
	 * random movement of tasks -- counter the numa conditions we're trying
	 * to satisfy here.
	 */
	/* 选择最低 NUMA 域以采用最小失衡阈值，并避免高层随机迁移抵消 NUMA 局部性目标。 */
	rcu_read_lock();
	sd = rcu_dereference_all(per_cpu(sd_numa, env.src_cpu));
	if (sd) {
		env.imbalance_pct = 100 + (sd->imbalance_pct - 100) / 2;
		env.imb_numa_nr = sd->imb_numa_nr;
	}
	rcu_read_unlock();

	/*
	 * Cpusets can break the scheduler domain tree into smaller
	 * balance domains, some of which do not cross NUMA boundaries.
	 * Tasks that are "trapped" in such domains cannot be migrated
	 * elsewhere, so there is no point in (re)trying.
	 */
	/* cpuset 困住 task 且无跨 NUMA 域时，重置偏好到本节点并停止无意义重试。 */
	if (unlikely(!sd)) {
		sched_setnuma(p, task_node(p));
		return -EINVAL;
	}

	env.dst_nid = p->numa_preferred_nid;
	/* 先建立源与首选节点的 task/group 权重、距离和容量快照。 */
	dist = env.dist = node_distance(env.src_nid, env.dst_nid);
	taskweight = task_weight(p, env.src_nid, dist);
	groupweight = group_weight(p, env.src_nid, dist);
	update_numa_stats(&env, &env.src_stats, env.src_nid, false);
	taskimp = task_weight(p, env.dst_nid, dist) - taskweight;
	groupimp = group_weight(p, env.dst_nid, dist) - groupweight;
	update_numa_stats(&env, &env.dst_stats, env.dst_nid, true);

	/* Try to find a spot on the preferred nid. */
	/* 首轮只搜索 preferred_nid，优先完成最直接的内存局部性目标。 */
	task_numa_find_cpu(&env, taskimp, groupimp);

	/*
	 * Look at other nodes in these cases:
	 * - there is no space available on the preferred_nid
	 * - the task is part of a numa_group that is interleaved across
	 *   multiple NUMA nodes; in order to better consolidate the group,
	 *   we need to check other locations.
	 */
	/* 首选节点无位置或组跨多个活跃节点时，再搜索其他 CPU 节点以整合工作集。 */
	ng = deref_curr_numa_group(p);
	if (env.best_cpu == -1 || (ng && ng->active_nodes > 1)) {
		for_each_node_state(nid, N_CPU) {
			if (nid == env.src_nid || nid == p->numa_preferred_nid)
				continue;

			dist = node_distance(env.src_nid, env.dst_nid);
			if (sched_numa_topology_type == NUMA_BACKPLANE &&
						dist != env.dist) {
				taskweight = task_weight(p, env.src_nid, dist);
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
				groupweight = group_weight(p, env.src_nid, dist);
			}

			/* Only consider nodes where both task and groups benefit */
			/* task 与 group 分数同时为负的节点无任何层级受益，直接跳过。 */
			taskimp = task_weight(p, nid, dist) - taskweight;
			groupimp = group_weight(p, nid, dist) - groupweight;
			if (taskimp < 0 && groupimp < 0)
				continue;

			env.dist = dist;
			env.dst_nid = nid;
			update_numa_stats(&env, &env.dst_stats, env.dst_nid, true);
			task_numa_find_cpu(&env, taskimp, groupimp);
		}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	/*
	 * If the task is part of a workload that spans multiple NUMA nodes,
	 * and is migrating into one of the workload's active nodes, remember
	 * this node as the task's preferred numa node, so the workload can
	 * settle down.
	 * A task that migrated to a second choice node will be better off
	 * trying for a better one later. Do not set the preferred node here.
	 */
	/* 迁入组活跃节点时更新 task 偏好帮助稳定；次优节点不固化，留待后续继续改进。 */
	if (ng) {
		if (env.best_cpu == -1)
			nid = env.src_nid;
		else
			nid = cpu_to_node(env.best_cpu);

		if (nid != p->numa_preferred_nid)
			sched_setnuma(p, nid);
	}

	/* No better CPU than the current one was found. */
	/* 未找到改善方案时记录 stick trace 并返回可重试错误，task 保持当前 CPU。 */
	if (env.best_cpu == -1) {
		trace_sched_stick_numa(p, env.src_cpu, NULL, -1);
		return -EAGAIN;
	}

	best_rq = cpu_rq(env.best_cpu);
	if (env.best_task == NULL) {
		/* 无 swap 对端时执行普通 task 迁移；无论成功失败都释放目标 rq 认领位。 */
		ret = migrate_task_to(p, env.best_cpu);
		WRITE_ONCE(best_rq->numa_migrate_on, 0);
		if (ret != 0)
			trace_sched_stick_numa(p, env.src_cpu, NULL, env.best_cpu);
		return ret;
	}

	/* 有 best_task 时由双 rq 锁迁移核心执行交换，随后清标记、trace 失败并释放候选引用。 */
	ret = migrate_swap(p, env.best_task, env.best_cpu, env.src_cpu);
	WRITE_ONCE(best_rq->numa_migrate_on, 0);

	if (ret != 0)
		trace_sched_stick_numa(p, env.src_cpu, env.best_task, env.best_cpu);
	put_task_struct(env.best_task);
	return ret;
}

/* Attempt to migrate a task to a CPU on the preferred node. */
/* 周期性尝试把 task 迁到首选节点 CPU；尚无统计、已在首选节点或未到重试点时不迁移。 */
/*
 * numa_migrate_preferred() - 按自适应退避周期重试 task 的首选 NUMA 节点迁移。
 *
 * 业务背景：一次迁移可能因负载/竞态失败，不能每 tick 重试；周期取最多 1 秒且不超过扫描周期 1/16。
 * 入参：@p 是当前锁规则下稳定的输入输出 task。
 * 出参/返回：无直接返回；推进 numa_migrate_retry，必要时调用 task_numa_migrate()。
 * 注意事项：无偏好或无 fault 统计时不调度重试；迁移 errno 由后续周期自然重试而不向上传播。
 */
static void numa_migrate_preferred(struct task_struct *p)
{
	unsigned long interval = HZ;

	/* This task has no NUMA fault statistics yet */
	/* 没有首选节点或 fault 数组时不存在可证实的迁移目标。 */
	if (unlikely(p->numa_preferred_nid == NUMA_NO_NODE || !p->numa_faults))
		return;

	/* Periodically retry migrating the task to the preferred node */
	/* 重试间隔与扫描周期联动，且上限一秒，既及时收敛又避免迁移风暴。 */
	interval = min(interval, msecs_to_jiffies(p->numa_scan_period) / 16);
	p->numa_migrate_retry = jiffies + interval;

	/* Success if task is already running on preferred CPU */
	/* 当前节点已匹配偏好时只保留新的重试期限，无需 CPU 内部迁移。 */
	if (task_node(p) == p->numa_preferred_nid)
		return;

	/* Otherwise, try migrate to a CPU on the preferred node */
	/* 进入完整搜索，允许选择直迁或与目标 CPU task 交换。 */
	task_numa_migrate(p);
}

/*
 * Find out how many nodes the workload is actively running on. Do this by
 * tracking the nodes from which NUMA hinting faults are triggered. This can
 * be different from the set of nodes where the workload's memory is currently
 * located.
 */
/* 活跃节点按触发 hint fault 的 CPU 位置统计，可能与工作集当前物理内存节点集合不同。 */
/*
 * numa_group_count_active_nodes() - 更新 NUMA 组伪交错活跃节点数与最大 CPU fault。
 *
 * 业务背景：跨多个显著访问节点的工作负载需允许组内多节点稳定，而非强压到单节点。
 * 入参：@numa_group 是持组锁或等价保护的输入输出组。
 * 出参/返回：无直接返回；写 max_faults_cpu 和 active_nodes。
 * 注意事项：分两遍先求最大值再计超过 1/3 阈值节点，避免遍历中动态阈值漏计早期节点。
 */
static void numa_group_count_active_nodes(struct numa_group *numa_group)
{
	unsigned long faults, max_faults = 0;
	int nid, active_nodes = 0;

	/* 遍历本层候选并逐项复验，循环结果汇入后续选择。 */
	for_each_node_state(nid, N_CPU) {
		faults = group_faults_cpu(numa_group, nid);
		if (faults > max_faults)
			max_faults = faults;
	}

	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
	for_each_node_state(nid, N_CPU) {
		faults = group_faults_cpu(numa_group, nid);
		if (faults * ACTIVE_NODE_FRACTION > max_faults)
			active_nodes++;
	}

	numa_group->max_faults_cpu = max_faults;
	numa_group->active_nodes = active_nodes;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}

/*
 * When adapting the scan rate, the period is divided into NUMA_PERIOD_SLOTS
 * increments. The more local the fault statistics are, the higher the scan
 * period will be for the next scan window. If local/(local+remote) ratio is
 * below NUMA_PERIOD_THRESHOLD (where range of ratio is 1..NUMA_PERIOD_SLOTS)
 * the scan period will decrease. Aim for 70% local accesses.
 */
/* 扫描周期分十档；local 或 private 比例以十分位表示，阈值 7 令目标约为 70% 本地访问。 */
#define NUMA_PERIOD_SLOTS 10
#define NUMA_PERIOD_THRESHOLD 7

/*
 * Increase the scan period (slow down scanning) if the majority of
 * our memory is already on our local node, or if the majority of
 * the page accesses are shared with other processes.
 * Otherwise, decrease the scan period.
 */
/* 本地或共享访问占多数时放慢扫描；私有远程访问占多数时加快，以推动页面迁到 task。 */
/*
 * update_task_scan_period() - 根据上一窗口 fault 局部性自适应调整 task NUMA 扫描周期。
 *
 * 业务背景：自动平衡在收敛后应降低 PTE/fault 开销，远程私有页较多时则需更快采样迁移。
 * 入参：@p 是输入输出 task；@shared/@private 是本窗口 fault 计数。
 * 出参/返回：无直接返回；更新 numa_scan_period/next_scan，并清零 locality 窗口统计。
 * 注意事项：周期被 task_scan_min/max 钳位；无样本或迁移失败采用指数退避且提前返回。
 */
static void update_task_scan_period(struct task_struct *p,
			unsigned long shared, unsigned long private)
{
	unsigned int period_slot;
	int lr_ratio, ps_ratio;
	int diff;

	unsigned long remote = p->numa_faults_locality[0];
	unsigned long local = p->numa_faults_locality[1];

	/*
	 * If there were no record hinting faults then either the task is
	 * completely idle or all activity is in areas that are not of interest
	 * to automatic numa balancing. Related to that, if there were failed
	 * migration then it implies we are migrating too quickly or the local
	 * node is overloaded. In either case, scan slower
	 */
	/* 无有效样本或存在迁移失败都表示更快扫描无益，周期翻倍并立即重排 mm 下次扫描。 */
	if (local + shared == 0 || p->numa_faults_locality[2]) {
		p->numa_scan_period = min(p->numa_scan_period_max,
			p->numa_scan_period << 1);

		p->mm->numa_next_scan = jiffies +
			msecs_to_jiffies(p->numa_scan_period);

		return;
	}

	/*
	 * Prepare to scale scan period relative to the current period.
	 *	 == NUMA_PERIOD_THRESHOLD scan period stays the same
	 *       <  NUMA_PERIOD_THRESHOLD scan period decreases (scan faster)
	 *	 >= NUMA_PERIOD_THRESHOLD scan period increases (scan slower)
	 */
	/* 将当前周期切成十档，用 local/remote 与 private/shared 两个十分比决定正负步长。 */
	period_slot = DIV_ROUND_UP(p->numa_scan_period, NUMA_PERIOD_SLOTS);
	lr_ratio = (local * NUMA_PERIOD_SLOTS) / (local + remote);
	ps_ratio = (private * NUMA_PERIOD_SLOTS) / (private + shared);

	if (ps_ratio >= NUMA_PERIOD_THRESHOLD) {
		/*
		 * Most memory accesses are local. There is no need to
		 * do fast NUMA scanning, since memory is already local.
		 */
		/* 私有访问比例高且（按现有变量口径）已具良好局部性，按超阈值档数放慢。 */
		int slot = ps_ratio - NUMA_PERIOD_THRESHOLD;
		if (!slot)
			slot = 1;
		diff = slot * period_slot;
	} else if (lr_ratio >= NUMA_PERIOD_THRESHOLD) {
		/*
		 * Most memory accesses are shared with other tasks.
		 * There is no point in continuing fast NUMA scanning,
		 * since other tasks may just move the memory elsewhere.
		 */
		/* 本地访问已达目标或共享任务可能反向迁页时，继续快速扫描收益低，放慢周期。 */
		int slot = lr_ratio - NUMA_PERIOD_THRESHOLD;
		if (!slot)
			slot = 1;
		diff = slot * period_slot;
	} else {
		/*
		 * Private memory faults exceed (SLOTS-THRESHOLD)/SLOTS,
		 * yet they are not on the local NUMA node. Speed up
		 * NUMA scanning to get the memory moved over.
		 */
		/* 私有远程页仍多，按离阈值的档数给负 diff，加快下一窗口扫描。 */
		int ratio = max(lr_ratio, ps_ratio);
		diff = -(NUMA_PERIOD_THRESHOLD - ratio) * period_slot;
	}

	p->numa_scan_period = clamp(p->numa_scan_period + diff,
			task_scan_min(p), task_scan_max(p));
	memset(p->numa_faults_locality, 0, sizeof(p->numa_faults_locality));
	/* 新周期从空窗口开始，避免旧 fault 在多轮中被重复用于反馈。 */
}

/*
 * Get the fraction of time the task has been running since the last
 * NUMA placement cycle. The scheduler keeps similar statistics, but
 * decays those on a 32ms period, which is orders of magnitude off
 * from the dozens-of-seconds NUMA balancing period. Use the scheduler
 * stats only if the task is so new there are no NUMA statistics yet.
 */
/*
 * 计算 task 自上次 NUMA placement 以来的运行时间占比。调度 PELT 32ms 衰减周期远短于
 * 数十秒 NUMA 周期，不能直接复用；只有新 task 尚无 NUMA 基线时才用 PELT load_sum。
 */
/*
 * numa_get_avg_runtime() - 取得本次 NUMA 周期 task 运行量并推进采样基线。
 *
 * 业务背景：placement 用 CPU 运行份额给 task fault 加权，避免睡眠线程的少量 fault 主导组决策。
 * 入参：@p 是输入输出 current task；@period 是输出参数，返回采样时间窗纳秒数。
 * 出参/返回：返回累计执行增量；首次返回 load_sum 近似，并写回 last_* 基线。
 * 注意事项：使用 exec_start 避免额外时钟读取；时间倒退时把 period 钳零防止后续除零/反向比例。
 */
static u64 numa_get_avg_runtime(struct task_struct *p, u64 *period)
{
	u64 runtime, delta, now;
	/* Use the start of this time slice to avoid calculations. */
	/* 复用当前时间片起点作为 now，避免在慢速 placement 中再次读取调度时钟。 */
	now = p->se.exec_start;
	runtime = p->se.sum_exec_runtime;

	if (p->last_task_numa_placement) {
		delta = runtime - p->last_sum_exec_runtime;
		*period = now - p->last_task_numa_placement;

		/* Avoid time going backwards, prevent potential divide error: */
		/* 时钟/迁移快照若导致有符号负周期，钳为零让调用者避开错误除法。 */
		if (unlikely((s64)*period < 0))
			*period = 0;
	} else {
		delta = p->se.avg.load_sum;
		*period = LOAD_AVG_MAX;
	}

	p->last_sum_exec_runtime = runtime;
	p->last_task_numa_placement = now;

	return delta;
}

/*
 * Determine the preferred nid for a task in a numa_group. This needs to
 * be done in a way that produces consistent results with group_weight,
 * otherwise workloads might not converge.
 */
/* 首选节点选择必须与 group_weight() 的邻域评分口径一致，否则迁移方向会在两套目标间振荡。 */
/*
 * preferred_group_nid() - 按 NUMA 拓扑与组 fault 选择一致收敛的首选节点。
 *
 * 业务背景：placement 更新组 task 偏好时，直接互连、无胶合 mesh 和 backplane 需要不同邻域模型。
 * 入参：@p 是 current 组成员借用 task；@nid 是单节点统计给出的初始候选。
 * 出参/返回：返回首选 CPU 节点编号；不执行迁移或改变组状态。
 * 注意事项：mesh 全局评分，backplane 逐距离递归缩小节点集合；拓扑与 fault 为近似快照。
 */
static int preferred_group_nid(struct task_struct *p, int nid)
{
	nodemask_t nodes;
	int dist;

	/* Direct connections between all NUMA nodes. */
	/* 全节点等距直连时没有更高层邻域，沿用已有最佳 nid。 */
	if (sched_numa_topology_type == NUMA_DIRECT)
		return nid;

	/*
	 * On a system with glueless mesh NUMA topology, group_weight
	 * scores nodes according to the number of NUMA hinting faults on
	 * both the node itself, and on nearby nodes.
	 */
	/* mesh 对每个 CPU 节点计算包含邻近距离衰减的 group_weight，选择全局最高分。 */
	if (sched_numa_topology_type == NUMA_GLUELESS_MESH) {
		unsigned long score, max_score = 0;
		int node, max_node = nid;

		dist = sched_max_numa_distance;

		for_each_node_state(node, N_CPU) {
			score = group_weight(p, node, dist);
			if (score > max_score) {
				max_score = score;
				max_node = node;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			}
		}
		return max_node;
	}

	/*
	 * Finding the preferred nid in a system with NUMA backplane
	 * interconnect topology is more involved. The goal is to locate
	 * tasks from numa_groups near each other in the system, and
	 * untangle workloads from different sides of the system. This requires
	 * searching down the hierarchy of node groups, recursively searching
	 * inside the highest scoring group of nodes. The nodemask tricks
	 * keep the complexity of the search down.
	 */
	/* backplane 从最大距离向内递归选择最高 fault 节点组，用 nodemask 逐层缩小搜索集合。 */
	nodes = node_states[N_CPU];
	for (dist = sched_max_numa_distance; dist > LOCAL_DISTANCE; dist--) {
		unsigned long max_faults = 0;
		nodemask_t max_group = NODE_MASK_NONE;
		int a, b;

		/* Are there nodes at this distance from each other? */
		/* 当前机器没有该距离层级时跳过，不构造空的节点组。 */
		if (!find_numa_distance(dist))
			continue;

		for_each_node_mask(a, nodes) {
			unsigned long faults = 0;
			nodemask_t this_group;
			nodes_clear(this_group);

			/* Sum group's NUMA faults; includes a==b case. */
			/* 把距离小于本层阈值的节点聚成一组并从待处理集合摘除，a 自身也计入。 */
			for_each_node_mask(b, nodes) {
				if (node_distance(a, b) < dist) {
					faults += group_faults(p, b);
					node_set(b, this_group);
					node_clear(b, nodes);
				}
			}

			/* Remember the top group. */
			/* 保存本层 fault 总量最高的节点组，下一距离层只在该组内部继续细分。 */
			if (faults > max_faults) {
				max_faults = faults;
				max_group = this_group;
				/*
				 * subtle: at the smallest distance there is
				 * just one node left in each "group", the
				 * winner is the preferred nid.
				 */
				/* 最小距离时每组只剩单节点，此层胜者 a 就是最终 preferred nid。 */
				nid = a;
			}
		}
		/* Next round, evaluate the nodes within max_group. */
		/* 没有 fault 证据时保留已有 nid；否则递归收窄到本层最高分组。 */
		if (!max_faults)
			break;
		nodes = max_group;
	}
	return nid;
}

/* 将本轮 hint faults 折入 task/group 统计并更新 preferred_nid，不直接承诺迁移。 */
/*
 * task_numa_placement() - 在新 mm 扫描序列到达时衰减 fault、更新组统计并选择首选节点。
 *
 * 业务背景：hint fault 热路径只写临时桶；本周期函数批量折入长期平均，按 task CPU 份额
 * 归一化组贡献，并驱动扫描周期与 preferred_nid 收敛。
 * 入参：@p 必须是 current 的输入输出 task，持有有效 mm 与 numa_faults。
 * 出参/返回：无直接返回；更新 task/group fault、mm footprint、scan period 和 NUMA 偏好。
 * 注意事项：组更新持 spin_lock_irq；函数标为 conditional-lock context unsafe，不能在任意原子上下文调用。
 */
static void task_numa_placement(struct task_struct *p)
	__context_unsafe(/* conditional locking */)
{
	int seq, nid, max_nid = NUMA_NO_NODE;
	unsigned long max_faults = 0;
	unsigned long fault_types[2] = { 0, 0 };
	unsigned long total_faults;
	u64 runtime, period;
	spinlock_t *group_lock = NULL;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	long __maybe_unused new_fp;
	struct numa_group *ng;

	/*
	 * The p->mm->numa_scan_seq field gets updated without
	 * exclusive access. Use READ_ONCE() here to ensure
	 * that the field is read in a single access:
	 */
	/* scan_seq 无排他锁更新，只需单次无撕裂读取；task 已处理同一序列时立即返回。 */
	seq = READ_ONCE(p->mm->numa_scan_seq);
	if (p->numa_scan_seq == seq)
		return;
	p->numa_scan_seq = seq;
	p->numa_scan_period_max = task_scan_max(p);

	total_faults = p->numa_faults_locality[0] +
		       p->numa_faults_locality[1];
	runtime = numa_get_avg_runtime(p, &period);

	/* If the task is part of a group prevent parallel updates to group stats */
	/* 组成员可并行 fault/placement，持组锁把柔性数组、total_faults 与 active_nodes 作为一代更新。 */
	ng = deref_curr_numa_group(p);
	if (ng) {
		group_lock = &ng->lock;
		spin_lock_irq(group_lock);
	}

	/* Find the node with the highest number of faults */
	/* 逐在线节点和共享/私有类别，把临时桶衰减折入长期 task/group 统计并寻找最大 fault 节点。 */
	for_each_online_node(nid) {
		/* Keep track of the offsets in numa_faults array */
		/* 四个索引分别定位 memory/CPU 的长期桶与本扫描窗临时桶。 */
		int mem_idx, membuf_idx, cpu_idx, cpubuf_idx;
		unsigned long faults = 0, group_faults = 0;
		int priv;

		for (priv = 0; priv < NR_NUMA_HINT_FAULT_TYPES; priv++) {
			long diff, f_diff, f_weight;

			mem_idx = task_faults_idx(NUMA_MEM, nid, priv);
			membuf_idx = task_faults_idx(NUMA_MEMBUF, nid, priv);
			cpu_idx = task_faults_idx(NUMA_CPU, nid, priv);
			cpubuf_idx = task_faults_idx(NUMA_CPUBUF, nid, priv);

			/* Decay existing window, copy faults since last scan */
			/* 旧平均减半，再加本窗新 fault；读取后清零临时 memory 桶。 */
			diff = p->numa_faults[membuf_idx] - p->numa_faults[mem_idx] / 2;
			fault_types[priv] += p->numa_faults[membuf_idx];
			p->numa_faults[membuf_idx] = 0;

			/*
			 * Normalize the faults_from, so all tasks in a group
			 * count according to CPU use, instead of by the raw
			 * number of faults. Tasks with little runtime have
			 * little over-all impact on throughput, and thus their
			 * faults are less important.
			 */
			/* 按 task 在 placement 周期内的 CPU 运行份额给 fault 加权，低 runtime 线程对组吞吐决策影响更小。 */
			f_weight = div64_u64(runtime << 16, period + 1);
			f_weight = (f_weight * p->numa_faults[cpubuf_idx]) /
				   (total_faults + 1);
			f_diff = f_weight - p->numa_faults[cpu_idx] / 2;
			p->numa_faults[cpubuf_idx] = 0;

			p->numa_faults[mem_idx] += diff;
			p->numa_faults[cpu_idx] += f_diff;
			faults += p->numa_faults[mem_idx];
			p->total_numa_faults += diff;
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
			if (ng) {
				/*
				 * safe because we can only change our own group
				 *
				 * mem_idx represents the offset for a given
				 * nid and priv in a specific region because it
				 * is at the beginning of the numa_faults array.
				 */
				/*
				 * 持组锁且 task 只能迁移自己的组贡献，因此可安全把本 task diff 加到组。
				 * mem_idx 位于数组长期区开头，能直接复用为对应 nid/priv 的组索引。
				 */
				ng->faults[mem_idx] += diff;
				ng->faults[cpu_idx] += f_diff;
				ng->total_faults += diff;
				group_faults += ng->faults[mem_idx];
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			}
#ifdef CONFIG_SCHED_CACHE
			/*
			 * Per task p->numa_faults[mem_idx] converges,
			 * so the accumulation of each task's faults
			 * converges too - Given the number of threads,
			 * it cannot overflow an unsigned long.
			 * Racy with concurrent updates from other threads
			 * sharing this mm. Acceptable since footprint is a
			 * heuristic and occasional lost updates are tolerable.
			 *
			 * If a task exits, its corresponding footprint must
			 * be subtracted from the mm->sc_stat.footprint, otherwise
			 * the mm->sc_stat.footprint will not converge:
			 * the exiting thread's footprint remains unchanged/undecayed
			 * in mm->sc_stat.footprint. See exit_mm().
			 *
			 * Lost updates and unsynchronized subtraction
			 * in exit_mm() can cause footprint + diff to
			 * go negative. Clamp to zero to prevent the
			 * unsigned footprint from wrapping.
			 */
			/*
			 * 每 task 衰减 fault 收敛，其线程总和也受线程数约束；footprint 与共享 mm 的
			 * 其他线程及 exit_mm() 无锁更新，允许偶发丢失。退出线程必须减去自己的贡献，
			 * 否则总量不再收敛；竞争可令有符号新值为负，写回前钳零防无符号回绕。
			 */
			new_fp = (long)READ_ONCE(p->mm->sc_stat.footprint) + diff;
			WRITE_ONCE(p->mm->sc_stat.footprint,
				   max(new_fp, 0L));
#endif
		}

		if (!ng) {
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
			if (faults > max_faults) {
				max_faults = faults;
				max_nid = nid;
			}
		} else if (group_faults > max_faults) {
			max_faults = group_faults;
			max_nid = nid;
		}
	}

	/* Cannot migrate task to CPU-less node */
	/* fault 最大节点若没有 CPU，就映射到最近有 CPU 节点，保证 task 迁移目标可执行。 */
	max_nid = numa_nearest_node(max_nid, N_CPU);

	if (ng) {
		/* 锁内完成 active_nodes 快照，解锁后再按拓扑选择组的一致 preferred nid。 */
		numa_group_count_active_nodes(ng);
		spin_unlock_irq(group_lock);
		max_nid = preferred_group_nid(p, max_nid);
	}

	if (max_faults) {
		/* Set the new preferred node */
		/* 只有本窗存在 fault 证据才发布新偏好，避免空样本覆盖已有稳定选择。 */
		if (max_nid != p->numa_preferred_nid)
			sched_setnuma(p, max_nid);
	}

	update_task_scan_period(p, fault_types[0], fault_types[1]);
}

/*
 * get_numa_group() - 尝试为仍存活的 NUMA 组取得一个引用。
 *
 * 业务背景：task_numa_group() 离开 RCU 窗口前需固定待加入组，防止最后成员并发释放。
 * 入参：@grp 是 RCU 下借用组。
 * 出参/返回：refcount 非零并成功递增返回 1，已归零返回 0；无其他副作用。
 * 注意事项：inc_not_zero 不能复活已进入 RCU 回收的对象。
 */
static inline int get_numa_group(struct numa_group *grp)
{
	return refcount_inc_not_zero(&grp->refcount);
}

/*
 * put_numa_group() - 释放一个 NUMA 组引用并在最后引用后安排 RCU 回收。
 *
 * 业务背景：组指针被 RCU 无锁读取，refcount 归零后不能立即 kfree。
 * 入参：@grp 是调用者持有一份引用的组，函数消费该引用。
 * 出参/返回：无直接返回；最后引用触发 kfree_rcu(grp, rcu)。
 * 注意事项：每次成功 get/初始成员引用必须恰好 put 一次，RCU 只延长内存而不保护字段修改。
 */
static inline void put_numa_group(struct numa_group *grp)
{
	if (refcount_dec_and_test(&grp->refcount))
		kfree_rcu(grp, rcu);
}

/*
 * task_numa_group() - 根据连续访问者关系创建或合并 task 的 NUMA 共享工作集组。
 *
 * 业务背景：共享 hint fault 表明两个 task 可能协作访问同一页；把它们的 fault 聚合后可
 * 按工作负载整体放置，同时用 PID/mm 过滤偶发碰撞和假共享。
 * 入参：@p 是 current 输入输出 task；@cpupid 是上次访问者编码；@flags 为 TNF_*；@priv 是输出共享性。
 * 出参/返回：无直接返回；可能分配/发布组、转移 p 的组成员关系并把 *@priv 改为 0/1。
 * 注意事项：先在 RCU 下选组并取引用，离开后双锁按地址序串行转移统计，旧组最终 RCU 回收。
 */
static void task_numa_group(struct task_struct *p, int cpupid, int flags,
			int *priv)
{
	struct numa_group *grp, *my_grp;
	struct task_struct *tsk;
	bool join = false;
	int cpu = cpupid_to_cpu(cpupid);
	int i;

	if (unlikely(!deref_curr_numa_group(p))) {
		/* task 首次需要分组时，分配含所有节点/统计维度柔性数组的私有单成员组。 */
		unsigned int size = sizeof(struct numa_group) +
				    NR_NUMA_HINT_FAULT_STATS *
				    nr_node_ids * sizeof(unsigned long);

		grp = kzalloc(size, GFP_KERNEL | __GFP_NOWARN);
		if (!grp)
			return;

		refcount_set(&grp->refcount, 1);
		grp->active_nodes = 1;
		grp->max_faults_cpu = 0;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		spin_lock_init(&grp->lock);
		grp->gid = p->pid;

		for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++)
			grp->faults[i] = p->numa_faults[i];

		grp->total_faults = p->total_numa_faults;

		grp->nr_tasks++;
		/* 初始化全部字段后用 RCU 发布；读者看到非 NULL 时可观察完整组状态。 */
		rcu_assign_pointer(p->numa_group, grp);
	}

	rcu_read_lock();
	/* 通过 cpupid 指向 CPU 的 current 反查上次访问者；PID 不匹配说明样本已陈旧。 */
	tsk = READ_ONCE(cpu_rq(cpu)->curr);

	if (!cpupid_match_pid(tsk, cpupid))
		goto no_join;

	grp = rcu_dereference_all(tsk->numa_group);
	if (!grp)
		goto no_join;

	my_grp = deref_curr_numa_group(p);
	if (grp == my_grp)
		goto no_join;

	/*
	 * Only join the other group if its bigger; if we're the bigger group,
	 * the other task will join us.
	 */
	/* 小组单向加入大组，避免双方同时互换；未来对方 fault 时也会遵循同一方向。 */
	if (my_grp->nr_tasks > grp->nr_tasks)
		goto no_join;

	/*
	 * Tie-break on the grp address.
	 */
	/* 组大小相等时按地址决定唯一方向，防止两个 CPU 各自认为对方应加入自己。 */
	if (my_grp->nr_tasks == grp->nr_tasks && my_grp > grp)
		goto no_join;

	/* Always join threads in the same process. */
	/* 共享 mm 的线程必然属于同一工作集，直接确认 join。 */
	if (tsk->mm == current->mm)
		join = true;

	/* Simple filter to avoid false positives due to PID collisions */
	/* 明确 TNF_SHARED 也允许加入；否则不同进程 PID 编码碰撞保持 private。 */
	if (flags & TNF_SHARED)
		join = true;

	/* Update priv based on whether false sharing was detected */
	/* 把是否确认共享反馈给 fault 统计分类；未 join 就继续记为 private。 */
	*priv = !join;

	if (join && !get_numa_group(grp))
		/* 目标组引用已归零时不能复活，回到 no_join 保持当前组。 */
		goto no_join;

	rcu_read_unlock();

	if (!join)
		return;

	WARN_ON_ONCE(irqs_disabled());
	/* double_lock_irq 按锁地址稳定排序并关闭中断，防止两组反向合并死锁。 */
	double_lock_irq(&my_grp->lock, &grp->lock);

	for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++) {
		/* 把 p 的全部长期 fault 从旧组原子搬到新组，保持两组总和守恒。 */
		my_grp->faults[i] -= p->numa_faults[i];
		grp->faults[i] += p->numa_faults[i];
	}
	my_grp->total_faults -= p->total_numa_faults;
	grp->total_faults += p->total_numa_faults;

	my_grp->nr_tasks--;
	grp->nr_tasks++;

	spin_unlock(&my_grp->lock);
	spin_unlock_irq(&grp->lock);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	rcu_assign_pointer(p->numa_group, grp);
	/* 统计搬移完成后发布新组指针，再释放 task 对旧组的成员引用。 */

	put_numa_group(my_grp);
	return;

no_join:
	rcu_read_unlock();
	return;
}

/*
 * Get rid of NUMA statistics associated with a task (either current or dead).
 * If @final is set, the task is dead and has reached refcount zero, so we can
 * safely free all relevant data structures. Otherwise, there might be
 * concurrent reads from places like load balancing and procfs, and we should
 * reset the data back to default state without freeing ->numa_faults.
 */
/*
 * 清理 task 的 NUMA 统计。final 表示 task 已死亡且引用归零，可释放数组；非 final 仍可能
 * 被 load balance/procfs 并发读取，只能把内容复位，不能释放 numa_faults 存储。
 */
/*
 * task_numa_free() - 从 NUMA 组撤销 task 贡献并按生命周期阶段复位或释放统计。
 *
 * 业务背景：exec/reset 与最终 task 回收共享清理入口，但对并发读者可见性的保证不同。
 * 入参：@p 是 current 或正由 current 最终释放的输入输出 task；@final 指示是否可销毁数组。
 * 出参/返回：无直接返回；摘除组关系并 put，final 时 kfree，否则清零长期统计。
 * 注意事项：组数组修改持 irqsave 锁；RCU_INIT_POINTER 仅因调用者保证没有需要发布排序的新对象。
 */
void task_numa_free(struct task_struct *p, bool final)
{
	/* safe: p either is current or is being freed by current */
	/* current 不会并发释放自身；dead task 已到最终回收，因此可 raw 读取组指针。 */
	struct numa_group *grp = rcu_dereference_raw(p->numa_group);
	unsigned long *numa_faults = p->numa_faults;
	unsigned long flags;
	int i;

	if (!numa_faults)
		return;

	if (grp) {
		/* 锁内从组撤销本 task 的每节点贡献和成员计数，再摘指针并释放成员引用。 */
		spin_lock_irqsave(&grp->lock, flags);
		for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++)
			grp->faults[i] -= p->numa_faults[i];
		grp->total_faults -= p->total_numa_faults;

		grp->nr_tasks--;
		spin_unlock_irqrestore(&grp->lock, flags);
		RCU_INIT_POINTER(p->numa_group, NULL);
		put_numa_group(grp);
	}

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (final) {
		/* 最终回收先清发布指针再释放数组，阻止后续误用陈旧地址。 */
		p->numa_faults = NULL;
		kfree(numa_faults);
	} else {
		/* 非最终复位保留数组地址，满足并发 procfs/load-balance 读者的生命周期要求。 */
		p->total_numa_faults = 0;
		for (i = 0; i < NR_NUMA_HINT_FAULT_STATS * nr_node_ids; i++)
			numa_faults[i] = 0;
	}
}

/*
 * Got a PROT_NONE fault for a page on @node.
 */
/* 在 @node 页上收到人为 PROT_NONE hint fault，用它采样访问 CPU、共享性与迁移结果。 */
/* hint fault 记录访问者与内存节点关系；分配失败只丢失优化信息，不影响页访问正确性。 */
/*
 * task_numa_fault() - 记录一次 NUMA hint fault 并周期触发 placement/迁移重试。
 *
 * 业务背景：页表扫描把页设为 PROT_NONE，真实访问进入此处形成 task-page 时间关系；
 * 临时 fault 桶随后由 task_numa_placement() 衰减聚合。
 * 入参：@last_cpupid 是页上次访问编码；@mem_node 是页节点；@pages 是 folio 页数；@flags 为 TNF_*。
 * 出参/返回：无直接返回；可能分配统计、创建/合并组、更新 fault 桶并尝试 task NUMA 迁移。
 * 注意事项：仅 current 用户 task；分配失败或慢内存分层过滤只损失优化采样，不影响 fault 正确完成。
 */
void task_numa_fault(int last_cpupid, int mem_node, int pages, int flags)
{
	struct task_struct *p = current;
	bool migrated = flags & TNF_MIGRATED;
	int cpu_node = task_node(current);
	int local = !!(flags & TNF_FAULT_LOCAL);
	struct numa_group *ng;
	int priv;

	if (!static_branch_likely(&sched_numa_balancing))
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;

	/* for example, ksmd faulting in a user's mm */
	/* 例如 ksmd 可能代表用户 mm fault，但当前内核线程无 mm，不能归属 task NUMA 统计。 */
	if (!p->mm)
		return;

	/*
	 * NUMA faults statistics are unnecessary for the slow memory
	 * node for memory tiering mode.
	 */
	/* 分层模式慢节点按访问时间处理，不需要普通 NUMA fault；无效 cpupid 也不能安全归类。 */
	if (!node_is_toptier(mem_node) &&
	    (sysctl_numa_balancing_mode & NUMA_BALANCING_MEMORY_TIERING ||
	     !cpupid_valid(last_cpupid)))
		return;

	/* Allocate buffer to track faults on a per-node basis */
	/* 首次有效 fault 才惰性分配全部长期/临时桶，失败时安全放弃本次优化统计。 */
	if (unlikely(!p->numa_faults)) {
		int size = sizeof(*p->numa_faults) *
			   NR_NUMA_HINT_FAULT_BUCKETS * nr_node_ids;

		p->numa_faults = kzalloc(size, GFP_KERNEL|__GFP_NOWARN);
		if (!p->numa_faults)
			return;

		p->total_numa_faults = 0;
		memset(p->numa_faults_locality, 0, sizeof(p->numa_faults_locality));
	}

	/*
	 * First accesses are treated as private, otherwise consider accesses
	 * to be private if the accessing pid has not changed
	 */
	/* 无上次访问者时先当 private；同 PID 仍 private，不同 PID 可尝试建立共享 NUMA 组。 */
	if (unlikely(last_cpupid == (-1 & LAST_CPUPID_MASK))) {
		priv = 1;
	} else {
		priv = cpupid_match_pid(p, last_cpupid);
		if (!priv && !(flags & TNF_NO_GROUP))
			task_numa_group(p, last_cpupid, flags, &priv);
	}

	/*
	 * If a workload spans multiple NUMA nodes, a shared fault that
	 * occurs wholly within the set of nodes that the workload is
	 * actively using should be counted as local. This allows the
	 * scan rate to slow down when a workload has settled down.
	 */
	/* 共享 fault 若发生在组的两个活跃节点之间，视为组内本地访问，使稳定多节点工作集能降低扫描频率。 */
	ng = deref_curr_numa_group(p);
	if (!priv && !local && ng && ng->active_nodes > 1 &&
				numa_is_active_node(cpu_node, ng) &&
				numa_is_active_node(mem_node, ng))
		local = 1;

	/*
	 * Retry to migrate task to preferred node periodically, in case it
	 * previously failed, or the scheduler moved us.
	 */
	/* 到重试时点先折入最新 fault 更新 placement，再尝试回首选节点，处理旧失败或普通调度迁离。 */
	if (time_after(jiffies, p->numa_migrate_retry)) {
		task_numa_placement(p);
		numa_migrate_preferred(p);
	}

	if (migrated)
		/* 迁移成功页数和失败标记分别进入可观测统计与扫描退避反馈。 */
		p->numa_pages_migrated += pages;
	if (flags & TNF_MIGRATE_FAIL)
		p->numa_faults_locality[2] += pages;

	p->numa_faults[task_faults_idx(NUMA_MEMBUF, mem_node, priv)] += pages;
	/* memory 临时桶按页所在节点记，CPU 临时桶按访问 task 节点记；locality 按最终归类记。 */
	p->numa_faults[task_faults_idx(NUMA_CPUBUF, cpu_node, priv)] += pages;
	p->numa_faults_locality[local] += pages;
}

/*
 * reset_ptenuma_scan() - 完成一轮 mm VMA 扫描并把游标复位到地址空间起点。
 *
 * 业务背景：task_numa_work() 到达 VMA 尾部后推进采样序列，placement 据此折入新窗口 fault。
 * 入参：@p 是 current 输入输出 task，持 mmap 读锁并借用其 mm。
 * 出参/返回：无直接返回；numa_scan_seq 加一、numa_scan_offset 清零。
 * 注意事项：读锁不排斥其他扫描者，序列递增非原子但只用于统计；READ/WRITE_ONCE 防编译器合并撕裂。
 */
static void reset_ptenuma_scan(struct task_struct *p)
{
	/*
	 * We only did a read acquisition of the mmap sem, so
	 * p->mm->numa_scan_seq is written to without exclusive access
	 * and the update is not guaranteed to be atomic. That's not
	 * much of an issue though, since this is just used for
	 * statistical sampling. Use READ_ONCE/WRITE_ONCE, which are not
	 * expensive, to avoid any form of compiler optimizations:
	 */
	/* mmap 读锁允许并发扫描写 seq；偶发丢增量可接受，单次访问原语避免更糟的编译器重排/拆分。 */
	WRITE_ONCE(p->mm->numa_scan_seq, READ_ONCE(p->mm->numa_scan_seq) + 1);
	p->mm->numa_scan_offset = 0;
}

/*
 * vma_is_accessed() - 判断当前 task 是否应在本轮扫描指定 VMA。
 *
 * 业务背景：按 PID 活跃位过滤无关 VMA 可降低 PTE 扫描成本，但必须保留首次覆盖与前向进度。
 * 入参：@mm 是 current 地址空间；@vma 是持 mmap 读锁的借用 VMA，numab_state 已初始化。
 * 出参/返回：近期访问、前两轮、扫描已进入 VMA 或长期无人协助时返回 true，否则 false。
 * 注意事项：PID 位是哈希近似会碰撞；返回 false 只延后采样，不改变 VMA 权限。
 */
static bool vma_is_accessed(struct mm_struct *mm, struct vm_area_struct *vma)
{
	unsigned long pids;
	/*
	 * Allow unconditional access first two times, so that all the (pages)
	 * of VMAs get prot_none fault introduced irrespective of accesses.
	 * This is also done to avoid any side effect of task scanning
	 * amplifying the unfairness of disjoint set of VMAs' access.
	 */
	/* 前两轮无条件扫描全部 VMA，建立初始 fault 样本，并避免扫描行为放大线程各自访问不同 VMA 的不公平。 */
	if ((READ_ONCE(current->mm->numa_scan_seq) - vma->numab_state->start_scan_seq) < 2)
		return true;

	pids = vma->numab_state->pids_active[0] | vma->numab_state->pids_active[1];
	if (test_bit(hash_32(current->pid, ilog2(BITS_PER_LONG)), &pids))
		return true;

	/*
	 * Complete a scan that has already started regardless of PID access, or
	 * some VMAs may never be scanned in multi-threaded applications:
	 */
	/* 游标已进入该 VMA 后必须完成它，否则多线程 PID 过滤可能让尾部区间永久得不到扫描。 */
	if (mm->numa_scan_offset > vma->vm_start) {
		trace_sched_skip_vma_numa(mm, vma, NUMAB_SKIP_IGNORE_PID);
		return true;
	}

	/*
	 * This vma has not been accessed for a while, and if the number
	 * the threads in the same process is low, which means no other
	 * threads can help scan this vma, force a vma scan.
	 */
	/* 超过线程数轮次仍无人访问时强制扫描，避免线程少、无人替代的 VMA 永久饥饿。 */
	if (READ_ONCE(mm->numa_scan_seq) >
	   (vma->numab_state->prev_scan_seq + get_nr_threads(current)))
		return true;

	return false;
}

#define VMA_PID_RESET_PERIOD (4 * sysctl_numa_balancing_scan_delay)
/* VMA PID 活跃位每四个初始扫描延迟轮换一次，兼顾近期性与避免频繁清零。 */

/*
 * The expensive part of numa migration is done from task_work context.
 * Triggered from task_tick_numa().
 */
/* NUMA 迁移昂贵部分放在可睡眠 task_work 上下文，由调度 tick 仅负责触发。 */
/* task_work 分批扫描 VMA 并设置 PROT_NONE 提示页，按自适应周期重新安排下一轮。 */
/*
 * task_numa_work() - 分窗扫描 current mm 的合适 VMA 并安装 NUMA hint PTE。
 *
 * 业务背景：运行时间达到周期后，tick 将工作延后至用户返回；此处持 mmap 读锁遍历 VMA，
 * 以受限 PTE/虚拟地址预算制造后续 PROT_NONE fault 样本。
 * 入参：@work 必须嵌入 current->numa_work，由 task_work 框架借用交付。
 * 出参/返回：无直接返回；推进 mm 扫描游标/序列、VMA numab_state、PTE 权限和 task node_stamp。
 * 注意事项：可睡眠并 cond_resched；cmpxchg 让共享 mm 每周期仅一个线程扫描，退出路径先检查 PF_EXITING。
 */
static void task_numa_work(struct callback_head *work)
{
	unsigned long migrate, next_scan, now = jiffies;
	struct task_struct *p = current;
	struct mm_struct *mm = p->mm;
	u64 runtime = p->se.sum_exec_runtime;
	struct vm_area_struct *vma;
	unsigned long start, end;
	unsigned long nr_pte_updates = 0;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	long pages, virtpages;
	struct vma_iterator vmi;
	bool vma_pids_skipped;
	bool vma_pids_forced = false;

	WARN_ON_ONCE(p != container_of(work, struct task_struct, numa_work));

	/* callback 已摘链，恢复 self 哨兵以允许下一运行周期再次排队。 */
	work->next = work;
	/*
	 * Who cares about NUMA placement when they're dying.
	 *
	 * NOTE: make sure not to dereference p->mm before this check,
	 * exit_task_work() happens _after_ exit_mm() so we could be called
	 * without p->mm even though we still had it when we enqueued this
	 * work.
	 */
	/* exit_task_work() 晚于 exit_mm()；必须先检查 PF_EXITING，不能在此之前解引用可能已清空的 p->mm。 */
	if (p->flags & PF_EXITING)
		return;

	/*
	 * Memory is pinned to only one NUMA node via cpuset.mems, naturally
	 * no page can be migrated.
	 */
	/* cpuset.mems 仅允许一个节点时没有合法迁移目标，扫描只会制造无收益 fault。 */
	if (cpusets_enabled() && nodes_weight(cpuset_current_mems_allowed) == 1) {
		trace_sched_skip_cpuset_numa(current, &cpuset_current_mems_allowed);
		return;
	}

	if (!mm->numa_next_scan) {
		/* 首次工作只建立全 mm 初始延迟，避免任务创建后立即扫描大量页表。 */
		mm->numa_next_scan = now +
			msecs_to_jiffies(sysctl_numa_balancing_scan_delay);
	}

	/*
	 * Enforce maximal scan/migration frequency..
	 */
	/* 未到共享 mm 的下次扫描时点时退出，所有线程共同服从这一最大频率。 */
	migrate = mm->numa_next_scan;
	if (time_before(now, migrate))
		return;

	if (p->numa_scan_period == 0) {
		/* task 尚无周期时按 RSS/组共享程度计算起始值和动态上界。 */
		p->numa_scan_period_max = task_scan_max(p);
		p->numa_scan_period = task_scan_start(p);
	}

	next_scan = now + msecs_to_jiffies(p->numa_scan_period);
	if (!try_cmpxchg(&mm->numa_next_scan, &migrate, next_scan))
		/* 同 mm 另一线程先认领本周期，当前线程不重复扫描。 */
		return;

	/*
	 * Delay this task enough that another task of this mm will likely win
	 * the next time around.
	 */
	/* 人为推迟当前 task 两 tick 的运行时门槛，让同 mm 其他线程更可能承担下一轮扫描。 */
	p->node_stamp += 2 * TICK_NSEC;

	pages = sysctl_numa_balancing_scan_size;
	pages <<= 20 - PAGE_SHIFT; /* MB in pages */
	virtpages = pages * 8;	   /* Scan up to this much virtual space */
	/* PTE 实际更新预算为 scan_size MB；虚拟遍历最多八倍，用于快速越过空洞或已是 NUMA PTE 的区域。 */
	if (!pages)
		return;


	if (!mmap_read_trylock(mm))
		/* 不阻塞争用 mmap 写者；本轮已认领的扫描机会允许丢弃，后续周期再试。 */
		return;

	/*
	 * VMAs are skipped if the current PID has not trapped a fault within
	 * the VMA recently. Allow scanning to be forced if there is no
	 * suitable VMA remaining.
	 */
	/* 先按 PID 活跃位过滤；若因此没有任何候选，再强制扫描一个 VMA 保证前向进度。 */
	vma_pids_skipped = false;

retry_pids:
	/* 从共享 mm 游标继续；到末尾时完成序列并从首个 VMA 重新开始。 */
	start = mm->numa_scan_offset;
	vma_iter_init(&vmi, mm, start);
	vma = vma_next(&vmi);
	if (!vma) {
		reset_ptenuma_scan(p);
		start = 0;
		vma_iter_set(&vmi, start);
		vma = vma_next(&vmi);
	}

	/* 遍历本层候选并逐项复验，循环结果汇入后续选择。 */
	for (; vma; vma = vma_next(&vmi)) {
		/* 只扫描可迁移、策略允许、非 hugetlb/混合映射的普通 VMA。 */
		if (!vma_migratable(vma) || !vma_policy_mof(vma) ||
			is_vm_hugetlb_page(vma) || (vma->vm_flags & VM_MIXEDMAP)) {
			trace_sched_skip_vma_numa(mm, vma, NUMAB_SKIP_UNSUITABLE);
			continue;
		}

		/*
		 * Shared library pages mapped by multiple processes are not
		 * migrated as it is expected they are cache replicated. Avoid
		 * hinting faults in read-only file-backed mappings or the vDSO
		 * as migrating the pages will be of marginal benefit.
		 */
		/* 只读文件页/vDSO 通常由各节点 page cache 复制，迁移收益很小且会影响其他映射者。 */
		if (!vma->vm_mm ||
		    (vma->vm_file && (vma->vm_flags & (VM_READ|VM_WRITE)) == (VM_READ))) {
			trace_sched_skip_vma_numa(mm, vma, NUMAB_SKIP_SHARED_RO);
			continue;
		}

		/*
		 * Skip inaccessible VMAs to avoid any confusion between
		 * PROT_NONE and NUMA hinting PTEs
		 */
		/* 原本不可访问 VMA 的 PROT_NONE 具有真实保护语义，不能与 NUMA hint 标记混淆。 */
		if (!vma_is_accessible(vma)) {
			trace_sched_skip_vma_numa(mm, vma, NUMAB_SKIP_INACCESSIBLE);
			continue;
		}

		/* Initialise new per-VMA NUMAB state. */
		/* 首次遇到 VMA 时无锁分配状态并 cmpxchg 发布，输掉竞争者释放自己的临时对象。 */
		if (!vma->numab_state) {
			struct vma_numab_state *ptr;

			ptr = kzalloc_obj(*ptr);
			if (!ptr)
				continue;

			if (cmpxchg(&vma->numab_state, NULL, ptr)) {
				kfree(ptr);
				continue;
			}

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			vma->numab_state->start_scan_seq = mm->numa_scan_seq;

			vma->numab_state->next_scan = now +
				msecs_to_jiffies(sysctl_numa_balancing_scan_delay);

			/* Reset happens after 4 times scan delay of scan start */
			/* PID 活跃位首轮复位时刻设为 next_scan 后四个扫描延迟。 */
			vma->numab_state->pids_active_reset =  vma->numab_state->next_scan +
				msecs_to_jiffies(VMA_PID_RESET_PERIOD);

			/*
			 * Ensure prev_scan_seq does not match numa_scan_seq,
			 * to prevent VMAs being skipped prematurely on the
			 * first scan:
			 */
			/* prev_seq 设为当前减一，保证新状态不会在第一轮被误判为已扫描。 */
			 vma->numab_state->prev_scan_seq = mm->numa_scan_seq - 1;
		}

		/*
		 * Scanning the VMAs of short lived tasks add more overhead. So
		 * delay the scan for new VMAs.
		 */
		/* 新 VMA 延迟到 next_scan，避免短命任务为不会收敛的 NUMA 样本支付页表扫描成本。 */
		if (mm->numa_scan_seq && time_before(jiffies,
						vma->numab_state->next_scan)) {
			trace_sched_skip_vma_numa(mm, vma, NUMAB_SKIP_SCAN_DELAY);
			continue;
		}

		/* RESET access PIDs regularly for old VMAs. */
		/* 双缓冲活跃位轮换：上一新桶变旧桶，清空新桶，保留一个周期历史。 */
		if (mm->numa_scan_seq &&
				time_after(jiffies, vma->numab_state->pids_active_reset)) {
			vma->numab_state->pids_active_reset = vma->numab_state->pids_active_reset +
				msecs_to_jiffies(VMA_PID_RESET_PERIOD);
			vma->numab_state->pids_active[0] = READ_ONCE(vma->numab_state->pids_active[1]);
			vma->numab_state->pids_active[1] = 0;
		}

		/* Do not rescan VMAs twice within the same sequence. */
		/* 已完成本序列的 VMA 推进全局游标到末尾，避免同轮重复制造 hint fault。 */
		if (vma->numab_state->prev_scan_seq == mm->numa_scan_seq) {
			mm->numa_scan_offset = vma->vm_end;
			trace_sched_skip_vma_numa(mm, vma, NUMAB_SKIP_SEQ_COMPLETED);
			continue;
		}

		/*
		 * Do not scan the VMA if task has not accessed it, unless no other
		 * VMA candidate exists.
		 */
		/* 当前 PID 不活跃时先跳过并记标志；若所有候选都被跳过，尾部会强制重试一次。 */
		if (!vma_pids_forced && !vma_is_accessed(mm, vma)) {
			vma_pids_skipped = true;
			trace_sched_skip_vma_numa(mm, vma, NUMAB_SKIP_PID_INACTIVE);
			continue;
		}

		do {
			/* 按 hugepage 边界构造本批区间，change_prot_numa() 真正把合适 PTE 改成 hint 状态。 */
			start = max(start, vma->vm_start);
			end = ALIGN(start + (pages << PAGE_SHIFT), HPAGE_SIZE);
			end = min(end, vma->vm_end);
			nr_pte_updates = change_prot_numa(vma, start, end);

			/*
			 * Try to scan sysctl_numa_balancing_size worth of
			 * hpages that have at least one present PTE that
			 * is not already PTE-numa. If the VMA contains
			 * areas that are unused or already full of prot_numa
			 * PTEs, scan up to virtpages, to skip through those
			 * areas faster.
			 */
			/* 只有实际更新 PTE 才扣 pages；无效/已标记区只扣八倍虚拟预算，快速穿越稀疏地址。 */
			if (nr_pte_updates)
				pages -= (end - start) >> PAGE_SHIFT;
			virtpages -= (end - start) >> PAGE_SHIFT;

			start = end;
			if (pages <= 0 || virtpages <= 0)
				goto out;

			cond_resched();
		} while (end != vma->vm_end);

		/* VMA scan is complete, do not scan until next sequence. */
		/* 完整走到 vm_end 后记录本序列完成，下一序列前跳过。 */
		vma->numab_state->prev_scan_seq = mm->numa_scan_seq;

		/*
		 * Only force scan within one VMA at a time, to limit the
		 * cost of scanning a potentially uninteresting VMA.
		 */
		/* 强制模式最多处理一个原本无活跃 PID 的 VMA，限制可能无收益的额外成本。 */
		if (vma_pids_forced)
			break;
	}

	/*
	 * If no VMAs are remaining and VMAs were skipped due to the PID
	 * not accessing the VMA previously, then force a scan to ensure
	 * forward progress:
	 */
	/* 首遍无剩余候选且只因 PID 不活跃跳过时，回到游标强制挑一个，避免扫描永久停滞。 */
	if (!vma && !vma_pids_forced && vma_pids_skipped) {
		vma_pids_forced = true;
		goto retry_pids;
	}

out:
	/*
	 * It is possible to reach the end of the VMA list but the last few
	 * VMAs are not guaranteed to the vma_migratable. If they are not, we
	 * would find the !migratable VMA on the next scan but not reset the
	 * scanner to the start so check it now.
	 */
	/* 预算耗尽时保存 start；真正到列表末尾（即使尾部全不可迁移）则显式完成并复位序列。 */
	if (vma)
		mm->numa_scan_offset = start;
	else
		reset_ptenuma_scan(p);
	mmap_read_unlock(mm);

	/*
	 * Make sure tasks use at least 32x as much time to run other code
	 * than they used here, to limit NUMA PTE scanning overhead to 3% max.
	 * Usually update_task_scan_period slows down scanning enough; on an
	 * overloaded system we need to limit overhead on a per task basis.
	 */
	/* 扫描自身消耗的 CPU 时间乘 32 加到 node_stamp，把每 task 最坏扫描开销限制约 3%。 */
	if (unlikely(p->se.sum_exec_runtime != runtime)) {
		u64 diff = p->se.sum_exec_runtime - runtime;
		p->node_stamp += 32 * diff;
	}
}

/*
 * init_numa_balancing() - 初始化新 task/mm 的自动 NUMA balancing 状态与 task_work。
 *
 * 业务背景：fork 新进程需建立全新扫描序列和无偏好状态；共享 mm 的新线程继承偏好但错开扫描。
 * 入参：@clone_flags 是 clone 共享位图；@p 是尚未运行的输入输出新 task。
 * 出参/返回：无直接返回；初始化扫描时钟、fault/组字段和 numa_work callback。
 * 注意事项：调用者独占新 task；CLONE_VM 分支依赖 arch_dup_task_struct 已复制 preferred_nid。
 */
void init_numa_balancing(u64 clone_flags, struct task_struct *p)
{
	int mm_users = 0;
	struct mm_struct *mm = p->mm;

	if (mm) {
		/* mm 第一个用户负责初始化共享扫描时点和序列；后续线程沿用同一进度。 */
		mm_users = atomic_read(&mm->mm_users);
		if (mm_users == 1) {
			mm->numa_next_scan = jiffies + msecs_to_jiffies(sysctl_numa_balancing_scan_delay);
			mm->numa_scan_seq = 0;
		}
	}
	p->node_stamp			= 0;
	p->numa_scan_seq		= mm ? mm->numa_scan_seq : 0;
	p->numa_scan_period		= sysctl_numa_balancing_scan_delay;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	p->numa_migrate_retry		= 0;
	/* Protect against double add, see task_tick_numa and task_numa_work */
	/* callback next=self 是未排队哨兵，tick 与 work 依靠它防重复 task_work_add。 */
	p->numa_work.next		= &p->numa_work;
	p->numa_faults			= NULL;
	p->numa_pages_migrated		= 0;
	p->total_numa_faults		= 0;
	RCU_INIT_POINTER(p->numa_group, NULL);
	p->last_task_numa_placement	= 0;
	p->last_sum_exec_runtime	= 0;

	init_task_work(&p->numa_work, task_numa_work);

	/* New address space, reset the preferred nid */
	/* 非 CLONE_VM 没有可继承的工作集证据，首选节点重置为未知并等待 fault 学习。 */
	if (!(clone_flags & CLONE_VM)) {
		p->numa_preferred_nid = NUMA_NO_NODE;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;
	}

	/*
	 * New thread, keep existing numa_preferred_nid which should be copied
	 * already by arch_dup_task_struct but stagger when scans start.
	 */
	/* 共享 mm 线程保留复制的偏好，但按线程数和当前周期增加 node_stamp，错开扫描触发。 */
	if (mm) {
		unsigned int delay;

		delay = min_t(unsigned int, task_scan_max(current),
			current->numa_scan_period * mm_users * NSEC_PER_MSEC);
		delay += 2 * TICK_NSEC;
		p->node_stamp = delay;
	}
}

/*
 * Drive the periodic memory faults..
 */
/* 以 task 实际运行时间驱动周期性 NUMA hint fault 扫描，而非按墙钟唤醒空闲线程。 */
/*
 * task_tick_numa() - 在当前 task 累计足够 CPU 时间后安排 NUMA 扫描 task_work。
 *
 * 业务背景：调度 tick 只做常量级门禁，把可睡眠 mmap/PTE 扫描推迟到用户返回路径。
 * 入参：@rq 是当前锁定 rq；@curr 是当前运行输入输出 task，均借用。
 * 出参/返回：无直接返回；可能初始化扫描周期、推进 node_stamp 并挂 numa_work。
 * 注意事项：无 mm、退出/内核线程或 work 已排队时返回；运行时间门槛偏向真正繁忙线程承担扫描。
 */
static void task_tick_numa(struct rq *rq, struct task_struct *curr)
{
	struct callback_head *work = &curr->numa_work;
	u64 period, now;

	/*
	 * We don't care about NUMA placement if we don't have memory.
	 */
	/* 无用户内存、正在退出、内核线程或 callback 已排队时无需再触发。 */
	if (!curr->mm || (curr->flags & (PF_EXITING | PF_KTHREAD)) || work->next != work)
		return;

	/*
	 * Using runtime rather than walltime has the dual advantage that
	 * we (mostly) drive the selection from busy threads and that the
	 * task needs to have done some actual work before we bother with
	 * NUMA placement.
	 */
	/* runtime 让繁忙线程主导选择，并要求 task 先做真实工作，避免睡眠任务按墙钟频繁扫描。 */
	now = curr->se.sum_exec_runtime;
	period = (u64)curr->numa_scan_period * NSEC_PER_MSEC;

	if (now > curr->node_stamp + period) {
		/* 首次触发计算自适应起始周期；之后每跨一周期推进门槛，并在 mm 到期时挂工作。 */
		if (!curr->node_stamp)
			curr->numa_scan_period = task_scan_start(curr);
		curr->node_stamp += period;

		if (!time_before(jiffies, curr->mm->numa_next_scan))
			task_work_add(curr, work, TWA_RESUME);
	}
}

/*
 * update_scan_period() - 在 task 跨 NUMA 节点迁移后重置其扫描周期以重新学习局部性。
 *
 * 业务背景：wakeup/load balance 可能在一轮扫描完成前拉走新 task，旧周期不再适合新节点。
 * 入参：@p 是输入输出 task；@new_cpu 是即将迁入的逻辑 CPU。
 * 出参/返回：无直接返回；符合条件时把 numa_scan_period 重置为 task_scan_start()。
 * 注意事项：迁向首选节点或源本就不在首选节点时不调节，避免正常收敛过程被反复重启。
 */
static void update_scan_period(struct task_struct *p, int new_cpu)
{
	int src_nid = cpu_to_node(task_cpu(p));
	int dst_nid = cpu_to_node(new_cpu);

	if (!static_branch_likely(&sched_numa_balancing))
		return;

	if (!p->mm || !p->numa_faults || (p->flags & PF_EXITING))
		return;

	if (src_nid == dst_nid)
		return;

	/*
	 * Allow resets if faults have been trapped before one scan
	 * has completed. This is most likely due to a new task that
	 * is pulled cross-node due to wakeups or load balancing.
	 */
	/* 已有 fault 但扫描尚早的 task 跨节点，多半由唤醒/负载平衡拉动，允许重置学习速度。 */
	if (p->numa_scan_seq) {
		/*
		 * Avoid scan adjustments if moving to the preferred
		 * node or if the task was not previously running on
		 * the preferred node.
		 */
		/* 向首选节点迁移是预期收敛；源不在首选节点则此次移动也不代表从稳定位置被打破。 */
		if (dst_nid == p->numa_preferred_nid ||
		    (p->numa_preferred_nid != NUMA_NO_NODE &&
			src_nid != p->numa_preferred_nid))
			return;
	}

	p->numa_scan_period = task_scan_start(p);
}

#else /* !CONFIG_NUMA_BALANCING: */

/*
 * task_tick_numa() - NUMA balancing 关闭时的 tick 空操作。
 *
 * 业务背景：fair tick 保持配置无关调用结构。
 * 入参：@rq、@curr 均为借用且不读取。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：不影响普通 tick 记账与抢占。
 */
static void task_tick_numa(struct rq *rq, struct task_struct *curr)
{
}

/*
 * account_numa_enqueue() - NUMA balancing 关闭时不维护入队偏好计数。
 *
 * 业务背景：公共实体入队路径无需条件编译。
 * 入参：@rq、@p 均借用且不读取。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：基础运行队列负载仍由 account_entity_enqueue() 维护。
 */
static inline void account_numa_enqueue(struct rq *rq, struct task_struct *p)
{
}

/*
 * account_numa_dequeue() - NUMA balancing 关闭时不维护出队偏好计数。
 *
 * 业务背景：与空 enqueue stub 配对。
 * 入参：@rq、@p 均借用且不读取。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：只裁剪 NUMA 启发式统计。
 */
static inline void account_numa_dequeue(struct rq *rq, struct task_struct *p)
{
}

/*
 * update_scan_period() - NUMA balancing 关闭时忽略跨 CPU 扫描周期调整。
 *
 * 业务背景：迁移路径保持统一调用，但该配置没有扫描状态。
 * 入参：@p 为借用 task；@new_cpu 为目标 CPU，均不读取。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：不改变 task 的其他迁移统计。
 */
static inline void update_scan_period(struct task_struct *p, int new_cpu)
{
}

#endif /* !CONFIG_NUMA_BALANCING */

/*
 * account_entity_enqueue() - 将实体加入 cfs_rq 的总权重、策略计数和 task 遍历链。
 *
 * 业务背景：真正插入 EEVDF 树前，运行队列必须先反映新实体的基础可运行集合。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是正在入队的借用实体。
 * 出参/返回：无直接返回；增加 load/nr_queued，task 还加入 NUMA/LLC 计数和 cfs_tasks 链。
 * 注意事项：组实体不进入 task 专属链；所有动作必须与 account_entity_dequeue() 严格配对。
 */
static void
account_entity_enqueue(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	update_load_add(&cfs_rq->load, se->load.weight);
	if (entity_is_task(se)) {
		/* task 才拥有 NUMA/LLC 偏好与 rq->cfs_tasks 节点；组实体仅维护层级权重。 */
		struct task_struct *p = task_of(se);
		struct rq *rq = rq_of(cfs_rq);

		account_numa_enqueue(rq, p);
		account_llc_enqueue(rq, p);
		list_add(&se->group_node, &rq->cfs_tasks);
	}
	cfs_rq->nr_queued++;
}

/*
 * account_entity_dequeue() - 从 cfs_rq 基础集合撤销实体权重和 task 策略状态。
 *
 * 业务背景：实体离开可运行集合时，所有与 enqueue 建立的总量和链表可见性都必须撤销。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是正在出队的借用实体。
 * 出参/返回：无直接返回；减少 load/nr_queued，task 还撤销 NUMA/LLC 并摘除 cfs_tasks。
 * 注意事项：list_del_init 让节点恢复未链接状态，防止重复出队或后续重入队误用旧指针。
 */
static void
account_entity_dequeue(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	update_load_sub(&cfs_rq->load, se->load.weight);
	if (entity_is_task(se)) {
		struct task_struct *p = task_of(se);
		struct rq *rq = rq_of(cfs_rq);

		account_numa_dequeue(rq, p);
		account_llc_dequeue(rq, p);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		list_del_init(&se->group_node);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}
	cfs_rq->nr_queued--;
}

/*
 * Signed add and clamp on underflow.
 *
 * Explicitly do a load-store to ensure the intermediate value never hits
 * memory. This allows lockless observations without ever seeing the negative
 * values.
 */
/*
 * 对共享计数执行有符号增量并在减法下溢时钳零。显式 load/store 保证无锁观察者永远
 * 不会看到中间负值；READ/WRITE_ONCE 只约束单次访问，不提供跨字段事务。
 */
#define add_positive(_ptr, _val) do {                           \
	typeof(_ptr) ptr = (_ptr);                              \
	__signed_scalar_typeof(*ptr) val = (_val);              \
	typeof(*ptr) res, var = READ_ONCE(*ptr);                \
	/* 先读旧值；下方饱和加法防止负增量回绕。 */		\
								\
	res = var + val;                                        \
								\
	if (val < 0 && res > var)                               \
		res = 0;                                        \
								\
	WRITE_ONCE(*ptr, res);                                  \
} while (0)

/*
 * Remove and clamp on negative, from a local variable.
 *
 * A variant of sub_positive(), which does not use explicit load-store
 * and is thus optimized for local variable updates.
 */
/* lsub_positive() 是仅用于局部变量的减法钳零版本，无需显式单次 load/store。 */
#define lsub_positive(_ptr, _val) do {				\
	typeof(_ptr) ptr = (_ptr);				\
	*ptr -= min_t(typeof(*ptr), *ptr, _val);		\
} while (0)


/*
 * Because of rounding, se->util_sum might ends up being +1 more than
 * cfs->util_sum. Although this is not a problem by itself, detaching
 * a lot of tasks with the rounding problem between 2 updates of
 * util_avg (~1ms) can make cfs->util_sum becoming null whereas
 * cfs_util_avg is not.
 *
 * Check that util_sum is still above its lower bound for the new
 * util_avg. Given that period_contrib might have moved since the last
 * sync, we are only sure that util_sum must be above or equal to
 *    util_avg * minimum possible divider
 */
/*
 * 舍入可令实体 util_sum 比队列多 1；短时间批量 detach 可能把队列 sum 减到零而 avg 仍
 * 非零。宏同时更新 avg/sum 并强制 sum 不低于 avg*PELT_MIN_DIVIDER，维持 PELT 下界。
 */
#define __update_sa(sa, name, delta_avg, delta_sum) do {	\
	add_positive(&(sa)->name##_avg, delta_avg);		\
	add_positive(&(sa)->name##_sum, delta_sum);		\
	(sa)->name##_sum = max_t(typeof((sa)->name##_sum),	\
			       (sa)->name##_sum,		\
			       (sa)->name##_avg * PELT_MIN_DIVIDER); \
} while (0)

/*
 * enqueue_load_avg() - 把实体 PELT load 平均与加权 sum 加入 cfs_rq 聚合。
 *
 * 业务背景：实体 attach/enqueue 后，选核和组传播需要队列立即包含其历史负载。
 * 入参：@cfs_rq 是输入输出队列；@se 是借用实体，调用者持 rq 锁。
 * 出参/返回：无直接返回；通过 __update_sa 增加 load_avg/load_sum 并维持下界。
 * 注意事项：sum 需乘当前实体权重，必须在 reweight 修改权重前后按正确顺序撤旧加新。
 */
static inline void
enqueue_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	__update_sa(&cfs_rq->avg, load, se->avg.load_avg,
		    se_weight(se) * se->avg.load_sum);
}

/*
 * dequeue_load_avg() - 从 cfs_rq 聚合撤销实体当前 PELT load 贡献。
 *
 * 业务背景：detach 或 reweight 前必须移除旧尺度统计，防止队列负载重复累计。
 * 入参：@cfs_rq 是输入输出队列；@se 是借用实体，调用者持 rq 锁。
 * 出参/返回：无直接返回；以负增量更新 load_avg/load_sum并钳零。
 * 注意事项：必须和使用同一权重/统计快照的 enqueue_load_avg() 配对。
 */
static inline void
dequeue_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	__update_sa(&cfs_rq->avg, load, -se->avg.load_avg,
		    se_weight(se) * -se->avg.load_sum);
}

/*
 * rescale_entity() - 在权重改变时缩放实体 lag、相对 deadline 与保护窗口。
 *
 * 业务背景：nice/reweight 改变虚拟时间斜率；若仅换 weight 会凭空改变历史服务债权和请求期限。
 * 入参：@se 是输入输出实体；@weight 是新无单位权重；@rel_vprot 指示 vprot 已转为相对 V。
 * 出参/返回：无直接返回；按 old/new 权重比原地缩放 vlag，必要时 deadline/vprot。
 * 注意事项：调用前这些字段必须处于相对 avg_vruntime 表示；weight 非零，函数本身不更新 load.weight。
 */
static void
rescale_entity(struct sched_entity *se, unsigned long weight, bool rel_vprot)
{
	unsigned long old_weight = se->load.weight;

	/*
	 * VRUNTIME
	 * --------
	 *
	 * COROLLARY #1: The virtual runtime of the entity needs to be
	 * adjusted if re-weight at !0-lag point.
	 *
	 * Proof: For contradiction assume this is not true, so we can
	 * re-weight without changing vruntime at !0-lag point.
	 *
	 *             Weight	VRuntime   Avg-VRuntime
	 *     before    w          v            V
	 *      after    w'         v'           V'
	 *
	 * Since lag needs to be preserved through re-weight:
	 *
	 *	lag = (V - v)*w = (V'- v')*w', where v = v'
	 *	==>	V' = (V - v)*w/w' + v		(1)
	 *
	 * Let W be the total weight of the entities before reweight,
	 * since V' is the new weighted average of entities:
	 *
	 *	V' = (WV + w'v - wv) / (W + w' - w)	(2)
	 *
	 * by using (1) & (2) we obtain:
	 *
	 *	(WV + w'v - wv) / (W + w' - w) = (V - v)*w/w' + v
	 *	==> (WV-Wv+Wv+w'v-wv)/(W+w'-w) = (V - v)*w/w' + v
	 *	==> (WV - Wv)/(W + w' - w) + v = (V - v)*w/w' + v
	 *	==>	(V - v)*W/(W + w' - w) = (V - v)*w/w' (3)
	 *
	 * Since we are doing at !0-lag point which means V != v, we
	 * can simplify (3):
	 *
	 *	==>	W / (W + w' - w) = w / w'
	 *	==>	Ww' = Ww + ww' - ww
	 *	==>	W * (w' - w) = w * (w' - w)
	 *	==>	W = w	(re-weight indicates w' != w)
	 *
	 * So the cfs_rq contains only one entity, hence vruntime of
	 * the entity @v should always equal to the cfs_rq's weighted
	 * average vruntime @V, which means we will always re-weight
	 * at 0-lag point, thus breach assumption. Proof completed.
	 *
	 *
	 * COROLLARY #2: Re-weight does NOT affect weighted average
	 * vruntime of all the entities.
	 *
	 * Proof: According to corollary #1, Eq. (1) should be:
	 *
	 *	(V - v)*w = (V' - v')*w'
	 *	==>    v' = V' - (V - v)*w/w'		(4)
	 *
	 * According to the weighted average formula, we have:
	 *
	 *	V' = (WV - wv + w'v') / (W - w + w')
	 *	   = (WV - wv + w'(V' - (V - v)w/w')) / (W - w + w')
	 *	   = (WV - wv + w'V' - Vw + wv) / (W - w + w')
	 *	   = (WV + w'V' - Vw) / (W - w + w')
	 *
	 *	==>  V'*(W - w + w') = WV + w'V' - Vw
	 *	==>	V' * (W - w) = (W - w) * V	(5)
	 *
	 * If the entity is the only one in the cfs_rq, then reweight
	 * always occurs at 0-lag point, so V won't change. Or else
	 * there are other entities, hence W != w, then Eq. (5) turns
	 * into V' = V. So V won't change in either case, proof done.
	 *
	 *
	 * So according to corollary #1 & #2, the effect of re-weight
	 * on vruntime should be:
	 *
	 *	v' = V' - (V - v) * w / w'		(4)
	 *	   = V  - (V - v) * w / w'
	 *	   = V  - vl * w / w'
	 *	   = V  - vl'
	 */
	/*
	 * 非零 lag 点重加权必须调整 vruntime 才能守恒 lag=(V-v)w；推导同时证明所有实体的
	 * 加权平均 V 在重加权前后不变，因此只需把 vlag 按 old_weight/new_weight 缩放，
	 * 新 vruntime 由调用者用 V-vlag 重建。
	 */
	se->vlag = div64_long(se->vlag * old_weight, weight);

	/*
	 * DEADLINE
	 * --------
	 *
	 * When the weight changes, the virtual time slope changes and
	 * we should adjust the relative virtual deadline accordingly.
	 *
	 *	d' = v' + (d - v)*w/w'
	 *	   = V' - (V - v)*w/w' + (d - v)*w/w'
	 *	   = V  - (V - v)*w/w' + (d - v)*w/w'
	 *	   = V  + (d - V)*w/w'
	 */
	/* virtual deadline 相对 V 的距离也按权重比缩放；只有已转为相对形式时才能直接计算。 */
	if (se->rel_deadline)
		se->deadline = div64_long(se->deadline * old_weight, weight);

	if (rel_vprot)
		se->vprot = div64_long(se->vprot * old_weight, weight);
}

/* rq 锁下先撤旧权重贡献、保留 lag，再以新权重重算 slice/deadline 并加回。 */
/*
 * reweight_entity() - 原子地更换实体权重并守恒 EEVDF lag、deadline 与 PELT 聚合。
 *
 * 业务背景：nice/cgroup shares 更新可发生在实体排队或运行时；必须先结算旧服务、从树和
 * 队列统计撤旧，再按新斜率重建相对时间，最后恢复可选择状态。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出队列；@se 是输入输出实体；@weight 是新权重。
 * 出参/返回：无直接返回；修改 load、vlag/vruntime/deadline/vprot、PELT load 与树增强状态。
 * 注意事项：curr 不在红黑树但计入 nr_queued；所有撤销/加回顺序必须对称，期间不得释放 rq 锁。
 */
static void reweight_entity(struct cfs_rq *cfs_rq, struct sched_entity *se,
			    unsigned long weight)
{
	bool curr = cfs_rq->curr == se;
	bool rel_vprot = false;
	u64 avruntime = 0;

	if (se->on_rq) {
		/* commit outstanding execution time */
		/* 先按旧权重结算未记执行，随后保存相对 V 的 lag/deadline，避免历史服务在换权时跳变。 */
		update_curr(cfs_rq);
		avruntime = avg_vruntime(cfs_rq);
		se->vlag = entity_lag(cfs_rq, se, avruntime);
		se->deadline -= avruntime;
		se->rel_deadline = 1;
		if (curr && protect_slice(se)) {
			/* 仅仍有效的 curr 保护窗口需转为相对 V；已过期 vprot 无需参与缩放。 */
			se->vprot -= avruntime;
			rel_vprot = true;
		}

		cfs_rq->nr_queued--;
		/* curr 不在树中；其他实体先摘树，再从 cfs_rq 总权重撤销旧贡献。 */
		if (!curr)
			__dequeue_entity(cfs_rq, se);
		update_load_sub(&cfs_rq->load, se->load.weight);
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}
	dequeue_load_avg(cfs_rq, se);

	/* 缩放相对时间后安装新 load.weight，并按同一 load_sum 重新计算 load_avg。 */
	rescale_entity(se, weight, rel_vprot);

	update_load_set(&se->load, weight);

	do {
		u32 divider = get_pelt_divider(&se->avg);
		se->avg.load_avg = div_u64(se_weight(se) * se->avg.load_sum, divider);
	} while (0);

	enqueue_load_avg(cfs_rq, se);
	if (se->on_rq) {
		/* 把相对字段加回绝对 V，以 V-vlag 重建 vruntime，再恢复权重、树和 nr_queued。 */
		if (rel_vprot)
			se->vprot += avruntime;
		se->deadline += avruntime;
		se->rel_deadline = 0;
		se->vruntime = avruntime - se->vlag;

		update_load_add(&cfs_rq->load, se->load.weight);
		if (!curr)
			__enqueue_entity(cfs_rq, se);
		cfs_rq->nr_queued++;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}
}

/*
 * reweight_task_fair() - 将 task 的 load_weight 更新为调度核心提供的新值。
 *
 * 业务背景：set_user_nice/策略更新经 sched_class 回调进入，核心实体重加权后还需同步倒数缓存。
 * 入参：@rq 是锁定运行队列；@p 是输入输出 fair task；@lw 是借用的新权重及 inv_weight。
 * 出参/返回：无直接返回；完成 EEVDF/PELT 重加权并安装 inv_weight 缓存。
 * 注意事项：@p 必须属于 @rq 当前锁域；reweight_entity() 已处理 weight，随后才复制匹配的倒数。
 */
static void reweight_task_fair(struct rq *rq, struct task_struct *p,
			       const struct load_weight *lw)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	struct load_weight *load = &se->load;

	reweight_entity(cfs_rq, se, lw->weight);
	load->inv_weight = lw->inv_weight;
}

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
static inline int throttled_hierarchy(struct cfs_rq *cfs_rq);

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * All this does is approximate the hierarchical proportion which includes that
 * global sum we all love to hate.
 *
 * That is, the weight of a group entity, is the proportional share of the
 * group weight based on the group runqueue weights. That is:
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = -----------------------------               (1)
 *                       \Sum grq->load.weight
 *
 * Now, because computing that sum is prohibitively expensive to compute (been
 * there, done that) we approximate it with this average stuff. The average
 * moves slower and therefore the approximation is cheaper and more stable.
 *
 * So instead of the above, we substitute:
 *
 *   grq->load.weight -> grq->avg.load_avg                         (2)
 *
 * which yields the following:
 *
 *                     tg->weight * grq->avg.load_avg
 *   ge->load.weight = ------------------------------              (3)
 *                             tg->load_avg
 *
 * Where: tg->load_avg ~= \Sum grq->avg.load_avg
 *
 * That is shares_avg, and it is right (given the approximation (2)).
 *
 * The problem with it is that because the average is slow -- it was designed
 * to be exactly that of course -- this leads to transients in boundary
 * conditions. In specific, the case where the group was idle and we start the
 * one task. It takes time for our CPU's grq->avg.load_avg to build up,
 * yielding bad latency etc..
 *
 * Now, in that special case (1) reduces to:
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = ----------------------------- = tg->weight   (4)
 *                         grp->load.weight
 *
 * That is, the sum collapses because all other CPUs are idle; the UP scenario.
 *
 * So what we do is modify our approximation (3) to approach (4) in the (near)
 * UP case, like:
 *
 *   ge->load.weight =
 *
 *              tg->weight * grq->load.weight
 *     ---------------------------------------------------         (5)
 *     tg->load_avg - grq->avg.load_avg + grq->load.weight
 *
 * But because grq->load.weight can drop to 0, resulting in a divide by zero,
 * we need to use grq->avg.load_avg as its lower bound, which then gives:
 *
 *
 *                     tg->weight * grq->load.weight
 *   ge->load.weight = -----------------------------		   (6)
 *                             tg_load_avg'
 *
 * Where:
 *
 *   tg_load_avg' = tg->load_avg - grq->avg.load_avg +
 *                  max(grq->load.weight, grq->avg.load_avg)
 *
 * And that is shares_weight and is icky. In the (near) UP case it approaches
 * (4) while in the normal case it approaches (3). It consistently
 * overestimates the ge->load.weight and therefore:
 *
 *   \Sum ge->load.weight >= tg->weight
 *
 * hence icky!
 */
/*
 * 组实体权重理想上等于 tg shares 乘本 CPU 组队列权重再除全 CPU 总权重，但实时求全局和
 * 代价过高。实现用慢变 PELT load_avg 近似；idle→单任务瞬态下又以当前 load.weight
 * 替换本地平均项，使结果靠近 UP 精确值。该近似稳定且便宜，但系统性高估权重总和。
 */
/*
 * calc_group_shares() - 估算本 CPU task_group 实体应获得的层级权重。
 *
 * 业务背景：组 shares 要按各 CPU cfs_rq 当前负载分摊，不能在热路径扫描所有 CPU。
 * 入参：@cfs_rq 是 rq 锁下借用的组队列，读取 tg 全局原子 load_avg 与本地贡献。
 * 出参/返回：返回 [MIN_SHARES,tg_shares] 内无单位权重；无持久副作用。
 * 注意事项：使用近似全局平均，允许短暂过估；MIN_SHARES 必须未缩放以支持小 shares 的 per-CPU 分割。
 */
static long calc_group_shares(struct cfs_rq *cfs_rq)
{
	long tg_weight, tg_shares, load, shares;
	struct task_group *tg = cfs_rq->tg;

	tg_shares = READ_ONCE(tg->shares);

	load = max(scale_load_down(cfs_rq->load.weight), cfs_rq->avg.load_avg);

	tg_weight = atomic_long_read(&tg->load_avg);

	/* Ensure tg_weight >= load */
	/* 从全局近似撤销本地旧贡献并换入本地较新 load，保证分母至少覆盖当前分子负载。 */
	tg_weight -= cfs_rq->tg_load_avg_contrib;
	tg_weight += load;

	shares = (tg_shares * load);
	if (tg_weight)
		shares /= tg_weight;

	/*
	 * MIN_SHARES has to be unscaled here to support per-CPU partitioning
	 * of a group with small tg->shares value. It is a floor value which is
	 * assigned as a minimum load.weight to the sched_entity representing
	 * the group on a CPU.
	 *
	 * E.g. on 64-bit for a group with tg->shares of scale_load(15)=15*1024
	 * on an 8-core system with 8 tasks each runnable on one CPU shares has
	 * to be 15*1024*1/8=1920 instead of scale_load(MIN_SHARES)=2*1024. In
	 * case no task is runnable on a CPU MIN_SHARES=2 should be returned
	 * instead of 0.
	 */
	/*
	 * MIN_SHARES 作为每 CPU 组实体最小 load.weight 必须保持未缩放值；例如总 shares=15*1024
	 * 分给 8 CPU 时每份 1920，小于 scale_load(MIN_SHARES)=2048，却仍是合法比例。空 CPU
	 * 则至少返回 2 而非零，使组实体保持可表示。
	 */
	return clamp_t(long, shares, MIN_SHARES, tg_shares);
}

/*
 * Recomputes the group entity based on the current state of its group
 * runqueue.
 */
/* 根据子组运行队列最新负载重新计算代表该组的父层 sched_entity 权重。 */
/*
 * update_cfs_group() - 必要时把组实体权重更新为 calc_group_shares() 结果。
 *
 * 业务背景：层级 enqueue/PELT 更新后，父层实体必须反映子队列在全组 shares 中的比例。
 * 入参：@se 是 rq 锁下输入输出组实体；group_cfs_rq() 可在无子队列时返回 NULL。
 * 出参/返回：无直接返回；权重变化时调用 reweight_entity()，否则无副作用。
 * 注意事项：空组保留旧权重以支持 DELAY_DEQUEUE，不能在暂时无 load 时清零。
 */
static void update_cfs_group(struct sched_entity *se)
{
	struct cfs_rq *gcfs_rq = group_cfs_rq(se);
	long shares;

	/*
	 * When a group becomes empty, preserve its weight. This matters for
	 * DELAY_DEQUEUE.
	 */
	/* 延迟出队期间组可暂时空但实体仍保留在层级，维持旧权重才能守恒其 EEVDF lag。 */
	if (!gcfs_rq || !gcfs_rq->load.weight)
		return;

	shares = calc_group_shares(gcfs_rq);
	if (unlikely(se->load.weight != shares))
		reweight_entity(cfs_rq_of(se), se, shares);
}

#else /* !CONFIG_FAIR_GROUP_SCHED: */
/*
 * update_cfs_group() - 无组调度配置下的权重传播空操作。
 *
 * 业务背景：公共层级路径保持统一调用。
 * 入参：@se 是借用实体，本 stub 不读取。
 * 出参/返回：无直接返回和副作用。
 * 注意事项：task 自身 reweight 仍由 reweight_task_fair() 处理。
 */
static inline void update_cfs_group(struct sched_entity *se)
{
}
#endif /* !CONFIG_FAIR_GROUP_SCHED */

/*
 * cfs_rq_util_change() - 在根 CFS 利用率变化后通知 CPUFreq 调度 governor。
 *
 * 业务背景：PELT util 是 schedutil 频率选择输入；只有根 cfs_rq 代表整 CPU fair 负载。
 * 入参：@cfs_rq 是刚更新的借用队列；@flags 是 cpufreq_update_util 事件位图。
 * 出参/返回：无直接返回；根队列时触发 cpufreq 回调，子组无副作用。
 * 注意事项：边界事件可能漏调且 idle/RT 不包含在此 util 中；频率不变性依赖架构 scale 实现。
 */
static inline void cfs_rq_util_change(struct cfs_rq *cfs_rq, int flags)
{
	struct rq *rq = rq_of(cfs_rq);

	if (&rq->cfs == cfs_rq) {
		/*
		 * There are a few boundary cases this might miss but it should
		 * get called often enough that that should (hopefully) not be
		 * a real problem.
		 *
		 * It will not get called when we go idle, because the idle
		 * thread is a different class (!fair), nor will the utilization
		 * number include things like RT tasks.
		 *
		 * As is, the util number is not freq-invariant (we'd have to
		 * implement arch_scale_freq_capacity() for that).
		 *
		 * See cpu_util_cfs().
		 */
		/*
		 * 少量边界事件可能漏掉，但常规更新足够频繁；进入 idle 属于另一调度类，RT 负载也不
		 * 在 CFS util 中。若架构未实现频率容量缩放，该数值本身不是 freq-invariant。
		 */
		cpufreq_update_util(rq, flags);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}
}

/*
 * load_avg_is_decayed() - 验证 sched_avg 的 load/util/runnable 历史是否完全衰减为零。
 *
 * 业务背景：离线/空组清理只有在三类 sum 都归零后才能摘除队列并停止传播。
 * 入参：@sa 是调用者稳定的借用 PELT 统计。
 * 出参/返回：任一 sum 非零返回 false；全部为零返回 true，并警告任何不一致 avg。
 * 注意事项：avg=sum/divider，sum 清零而 avg 非零只能来自舍入/传播不变量破坏。
 */
static inline bool load_avg_is_decayed(struct sched_avg *sa)
{
	if (sa->load_sum)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
		return false;

	if (sa->util_sum)
		return false;

	if (sa->runnable_sum)
		return false;

	/*
	 * _avg must be null when _sum are null because _avg = _sum / divider
	 * Make sure that rounding and/or propagation of PELT values never
	 * break this.
	 */
	/* sum 全零时 avg 理应同步为零；WARN 捕获舍入或跨层传播留下的幽灵负载。 */
	WARN_ON_ONCE(sa->load_avg ||
		      sa->util_avg ||
		      sa->runnable_avg);

	return true;
}

/*
 * cfs_rq_last_update_time() - 无撕裂读取 cfs_rq PELT 的 64 位最后更新时间。
 *
 * 业务背景：32 位架构不能原子读取普通 u64，迁移路径又只有 pi_lock 而无 rq 锁。
 * 入参：@cfs_rq 是借用队列，提供主值和 u32 copy 序列。
 * 出参/返回：返回一致的 PELT 时钟时间戳；不修改队列。
 * 注意事项：只保证字段读取一致，不冻结 PELT 状态；值可能在返回后立即推进。
 */
static inline u64 cfs_rq_last_update_time(struct cfs_rq *cfs_rq)
{
	return u64_u32_load_copy(cfs_rq->avg.last_update_time,
				 cfs_rq->last_update_time_copy);
}
#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * Because list_add_leaf_cfs_rq always places a child cfs_rq on the list
 * immediately before a parent cfs_rq, and cfs_rqs are removed from the list
 * bottom-up, we only have to test whether the cfs_rq before us on the list
 * is our child.
 * If cfs_rq is not on the list, test whether a child needs its to be added to
 * connect a branch to the tree  * (see list_add_leaf_cfs_rq() for details).
 */
/*
 * 叶队列链始终 child 紧邻并位于 parent 之前，且删除自底向上，因此只需检查前驱即可判断
 * 是否仍有 child。parent 尚未入链时则检查 tmp_alone_branch，识别待连接的孤立分支。
 */
/*
 * child_cfs_rq_on_list() - 判断当前组队列是否仍被一个已入链/待连接子队列依赖。
 *
 * 业务背景：cfs_rq_is_decayed() 不能在子分支仍需父节点连接时把父队列视为可清理。
 * 入参：@cfs_rq 是 rq 锁保护的借用组队列。
 * 出参/返回：前驱属于其直接子组返回 true，否则 false；无状态变化。
 * 注意事项：正确性依赖 list_add_leaf_cfs_rq() 的 child-before-parent 排序不变量。
 */
static inline bool child_cfs_rq_on_list(struct cfs_rq *cfs_rq)
{
	struct cfs_rq *prev_cfs_rq;
	struct list_head *prev;
	struct rq *rq = rq_of(cfs_rq);

	if (cfs_rq->on_list) {
		prev = cfs_rq->leaf_cfs_rq_list.prev;
	} else {
		prev = rq->tmp_alone_branch;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}

	if (prev == &rq->leaf_cfs_rq_list)
		return false;

	prev_cfs_rq = container_of(prev, struct cfs_rq, leaf_cfs_rq_list);

	return (prev_cfs_rq->tg->parent == cfs_rq->tg);
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
}

/*
 * cfs_rq_is_decayed() - 判断组队列是否已无负载、历史、子分支和全局贡献。
 *
 * 业务背景：blocked load 更新可在完全衰减后把空 cfs_rq 从叶链摘除，停止后续扫描。
 * 入参：@cfs_rq 是 rq 锁保护的借用输入队列。
 * 出参/返回：四项条件全满足返回 true，否则 false；仅可能由内部 WARN 诊断不变量。
 * 注意事项：当前 load 为零不够，PELT 历史、child 连接或 tg_load_avg_contrib 任一存在都需保留。
 */
static inline bool cfs_rq_is_decayed(struct cfs_rq *cfs_rq)
{
	if (cfs_rq->load.weight)
		return false;

	if (!load_avg_is_decayed(&cfs_rq->avg))
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
		return false;

	if (child_cfs_rq_on_list(cfs_rq))
		return false;

	if (cfs_rq->tg_load_avg_contrib)
		return false;

	return true;
}

/**
 * update_tg_load_avg - update the tg's load avg
 * @cfs_rq: the cfs_rq whose avg changed
 *
 * This function 'ensures': tg->load_avg := \Sum tg->cfs_rq[]->avg.load.
 * However, because tg->load_avg is a global value there are performance
 * considerations.
 *
 * In order to avoid having to look at the other cfs_rq's, we use a
 * differential update where we store the last value we propagated. This in
 * turn allows skipping updates if the differential is 'small'.
 *
 * Updating tg's load_avg is necessary before update_cfs_share().
 */
/*
 * update_tg_load_avg() - 以差分方式把本 CPU cfs_rq load_avg 传播到 task_group 全局和。
 *
 * 业务背景：calc_group_shares() 需要近似所有 CPU 子队列总负载；逐 CPU 扫描太贵，因此保存
 * 上次贡献，仅在变化显著且限速允许时原子更新全局值。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出组队列。
 * 出参/返回：无直接返回；可能更新 tg->load_avg、本地贡献和更新时间。
 * 注意事项：root 结果无人使用、离线 CPU 不贡献；迁移重负载下最多每毫秒更新一次且忽略<=1/64 小差值。
 */
static inline void update_tg_load_avg(struct cfs_rq *cfs_rq)
{
	long delta;
	u64 now;

	/*
	 * No need to update load_avg for root_task_group as it is not used.
	 */
	/* 根组没有代表它的父层实体，因而不需要全局 group share 分母。 */
	if (cfs_rq->tg == &root_task_group)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;

	/* rq has been offline and doesn't contribute to the share anymore: */
	/* 离线 CPU 的历史贡献由专门清理路径撤销，不能在普通传播中重新加回。 */
	if (!cpu_active(cpu_of(rq_of(cfs_rq))))
		return;

	/*
	 * For migration heavy workloads, access to tg->load_avg can be
	 * unbound. Limit the update rate to at most once per ms.
	 */
	/* 高频迁移会让共享原子 cacheline 无界争用；每 cfs_rq 至多每毫秒传播一次。 */
	now = rq_clock(rq_of(cfs_rq));
	if (now - cfs_rq->last_update_tg_load_avg < NSEC_PER_MSEC)
		return;

	delta = cfs_rq->avg.load_avg - cfs_rq->tg_load_avg_contrib;
	/* 仅相对旧贡献变化超过 1/64 时发布，过滤 PELT 小幅抖动。 */
	if (abs(delta) > cfs_rq->tg_load_avg_contrib / 64) {
		atomic_long_add(delta, &cfs_rq->tg->load_avg);
		cfs_rq->tg_load_avg_contrib = cfs_rq->avg.load_avg;
		cfs_rq->last_update_tg_load_avg = now;
	}
}

/*
 * clear_tg_load_avg() - 从 task_group 全局负载和中撤销本 cfs_rq 的最后传播贡献。
 *
 * 业务背景：CPU 下线或队列永久停止贡献时，旧 tg_load_avg_contrib 不能残留为幽灵负载。
 * 入参：@cfs_rq 是 rq 锁保护的输入输出组队列。
 * 出参/返回：无直接返回；原子减去旧贡献并清零本地缓存、更新时间。
 * 注意事项：root 无需维护；调用后离线队列不可再走普通 update_tg_load_avg() 加回。
 */
static inline void clear_tg_load_avg(struct cfs_rq *cfs_rq)
{
	long delta;
	u64 now;

	/*
	 * No need to update load_avg for root_task_group, as it is not used.
	 */
	/* 根组全局和没有父层消费者，无需撤销。 */
	if (cfs_rq->tg == &root_task_group)
		return;

	now = rq_clock(rq_of(cfs_rq));
	delta = 0 - cfs_rq->tg_load_avg_contrib;
	atomic_long_add(delta, &cfs_rq->tg->load_avg);
	cfs_rq->tg_load_avg_contrib = 0;
	cfs_rq->last_update_tg_load_avg = now;
}

/* CPU offline callback: */
/* CPU 下线回调：遍历所有 task_group，撤销该 rq 上每个 cfs_rq 的全局 load_avg 贡献。 */
/*
 * clear_tg_offline_cfs_rqs() - 在 CPU 下线时批量清理该 CPU 的组负载贡献。
 *
 * 业务背景：task_group 全局近似和跨 CPU 共享，rq 下线后其历史贡献必须全部移除。
 * 入参：@rq 是调用者已持锁的离线中运行队列借用指针。
 * 出参/返回：无直接返回；清零所有组在该 CPU 的 tg_load_avg_contrib。
 * 注意事项：RCU 稳定 task_groups 列表；loop clock 模式避免每组 helper 重复更新 rq 时钟。
 */
static void __maybe_unused clear_tg_offline_cfs_rqs(struct rq *rq)
{
	struct task_group *tg;

	lockdep_assert_rq_held(rq);

	/*
	 * The rq clock has already been updated in
	 * set_rq_offline(), so we should skip updating
	 * the rq clock again in unthrottle_cfs_rq().
	 */
	/* set_rq_offline() 已刷新 rq clock；开启 loop_update 让潜在 unthrottle 避免再次推进时钟。 */
	rq_clock_start_loop_update(rq);

	guard(rcu)();

	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct cfs_rq *cfs_rq = tg_cfs_rq(tg, cpu_of(rq));

		clear_tg_load_avg(cfs_rq);
	}

	rq_clock_stop_loop_update(rq);
}

/*
 * Called within set_task_rq() right before setting a task's CPU. The
 * caller only guarantees p->pi_lock is held; no other assumptions,
 * including the state of rq->lock, should be made.
 */
/*
 * set_task_rq_fair() - 在 task 改写 CPU 前把其 PELT 时间基准从旧 cfs_rq 映射到新队列。
 *
 * 业务背景：迁移时实体离开一个 PELT 时钟域再加入另一个；ATTACH_AGE_LOAD 要保留阻塞衰减年龄。
 * 入参：@se 是输入输出 task 实体；@prev/@next 是借用旧/新 cfs_rq。
 * 出参/返回：无直接返回；把 se 先衰减到旧队列时间，再替换 last_update_time 为新队列时间。
 * 注意事项：调用者只保证 p->pi_lock，不能假定任何 rq 锁；因此只能用无撕裂时间副本和 blocked helper。
 */
void set_task_rq_fair(struct sched_entity *se,
		      struct cfs_rq *prev, struct cfs_rq *next)
{
	u64 p_last_update_time;
	u64 n_last_update_time;

	if (!sched_feat(ATTACH_AGE_LOAD))
		return;

	/*
	 * We are supposed to update the task to "current" time, then its up to
	 * date and ready to go to new CPU/cfs_rq. But we have difficulty in
	 * getting what current time is, so simply throw away the out-of-date
	 * time. This will result in the wakee task is less decayed, but giving
	 * the wakee more load sounds not bad.
	 */
	/*
	 * 理想上应衰减到真实当前时刻，但此处无 rq 锁难取得统一 now；实现丢弃跨队列过期时间，
	 * 让 wakee 略少衰减、负载略高，这是偏保守且可接受的选核信号。
	 */
	if (!(se->avg.last_update_time && prev))
		return;

	p_last_update_time = cfs_rq_last_update_time(prev);
	n_last_update_time = cfs_rq_last_update_time(next);

	__update_load_avg_blocked_se(p_last_update_time, se);
	se->avg.last_update_time = n_last_update_time;
}

/*
 * When on migration a sched_entity joins/leaves the PELT hierarchy, we need to
 * propagate its contribution. The key to this propagation is the invariant
 * that for each group:
 *
 *   ge->avg == grq->avg						(1)
 *
 * _IFF_ we look at the pure running and runnable sums. Because they
 * represent the very same entity, just at different points in the hierarchy.
 *
 * Per the above update_tg_cfs_util() and update_tg_cfs_runnable() are trivial
 * and simply copies the running/runnable sum over (but still wrong, because
 * the group entity and group rq do not have their PELT windows aligned).
 *
 * However, update_tg_cfs_load() is more complex. So we have:
 *
 *   ge->avg.load_avg = ge->load.weight * ge->avg.runnable_avg		(2)
 *
 * And since, like util, the runnable part should be directly transferable,
 * the following would _appear_ to be the straight forward approach:
 *
 *   grq->avg.load_avg = grq->load.weight * grq->avg.runnable_avg	(3)
 *
 * And per (1) we have:
 *
 *   ge->avg.runnable_avg == grq->avg.runnable_avg
 *
 * Which gives:
 *
 *                      ge->load.weight * grq->avg.load_avg
 *   ge->avg.load_avg = -----------------------------------		(4)
 *                               grq->load.weight
 *
 * Except that is wrong!
 *
 * Because while for entities historical weight is not important and we
 * really only care about our future and therefore can consider a pure
 * runnable sum, runqueues can NOT do this.
 *
 * We specifically want runqueues to have a load_avg that includes
 * historical weights. Those represent the blocked load, the load we expect
 * to (shortly) return to us. This only works by keeping the weights as
 * integral part of the sum. We therefore cannot decompose as per (3).
 *
 * Another reason this doesn't work is that runnable isn't a 0-sum entity.
 * Imagine a rq with 2 tasks that each are runnable 2/3 of the time. Then the
 * rq itself is runnable anywhere between 2/3 and 1 depending on how the
 * runnable section of these tasks overlap (or not). If they were to perfectly
 * align the rq as a whole would be runnable 2/3 of the time. If however we
 * always have at least 1 runnable task, the rq as a whole is always runnable.
 *
 * So we'll have to approximate.. :/
 *
 * Given the constraint:
 *
 *   ge->avg.running_sum <= ge->avg.runnable_sum <= LOAD_AVG_MAX
 *
 * We can construct a rule that adds runnable to a rq by assuming minimal
 * overlap.
 *
 * On removal, we'll assume each task is equally runnable; which yields:
 *
 *   grq->avg.runnable_sum = grq->avg.load_sum / grq->load.weight
 *
 * XXX: only do this for the part of runnable > running ?
 *
 */
/*
 * 迁移令 sched_entity 在 PELT 层级加入/离开，组实体 ge 与其子队列 grq 对同一运行对象的
 * running/runnable 理应相等，但窗口可能不对齐。util/runnable 可直接复制后重建 sum；load
 * 必须保留历史权重形成的 blocked load，且多个 task runnable 区间可能重叠，不能简单用
 * grq load.weight 反推。实现因此在增加时假设最小重叠、删除时假设各 task 同等 runnable，
 * 同时维持 running_sum<=runnable_sum<=LOAD_AVG_MAX；这是有意的近似而非精确分解。
 */
/*
 * update_tg_cfs_util() - 把子组队列 util 平均同步到父层组实体及父 cfs_rq。
 *
 * 业务背景：组实体与子队列代表同一运行利用率，层级迁移/更新后需传播差值。
 * 入参：@cfs_rq 是父输入输出队列；@se 是输入输出组实体；@gcfs_rq 是借用子队列。
 * 出参/返回：无直接返回；同步 se util avg/sum，并把差值加入父聚合。
 * 注意事项：使用父队列 divider 对齐窗口；delta_avg 为零时不重建，调用者持 rq 锁。
 */
static inline void
update_tg_cfs_util(struct cfs_rq *cfs_rq, struct sched_entity *se, struct cfs_rq *gcfs_rq)
{
	long delta_sum, delta_avg = gcfs_rq->avg.util_avg - se->avg.util_avg;
	u32 new_sum, divider;

	/* Nothing to update */
	/* 平均值未变时父聚合无差值，避免无意义 sum 重写。 */
	if (!delta_avg)
		return;

	/*
	 * cfs_rq->avg.period_contrib can be used for both cfs_rq and se.
	 * See ___update_load_avg() for details.
	 */
	/* 组实体与父 cfs_rq 共享 period_contrib，可用同一 divider 把 avg 精确映射到本窗口 sum。 */
	divider = get_pelt_divider(&cfs_rq->avg);

	/* Set new sched_entity's utilization */
	/* 先写实体新 avg/sum，再以新旧差值更新父队列，保持层级聚合一致。 */
	se->avg.util_avg = gcfs_rq->avg.util_avg;
	new_sum = se->avg.util_avg * divider;
	delta_sum = (long)new_sum - (long)se->avg.util_sum;
	se->avg.util_sum = new_sum;

	/* Update parent cfs_rq utilization */
	/* __update_sa 同时钳制父 sum 下界，避免舍入导致 avg 非零而 sum 为零。 */
	__update_sa(&cfs_rq->avg, util, delta_avg, delta_sum);
}

/*
 * update_tg_cfs_runnable() - 把子组 runnable 平均同步到父组实体及父队列。
 *
 * 业务背景：父层调度需看到子组整体可运行程度，而非逐 task 扫描。
 * 入参：@cfs_rq 是父输入输出队列；@se 是组实体；@gcfs_rq 是借用子队列。
 * 出参/返回：无直接返回；同步 runnable avg/sum 并传播父聚合差值。
 * 注意事项：窗口 divider 取父队列；无变化快速返回，所有对象受同一 rq 锁保护。
 */
static inline void
update_tg_cfs_runnable(struct cfs_rq *cfs_rq, struct sched_entity *se, struct cfs_rq *gcfs_rq)
{
	long delta_sum, delta_avg = gcfs_rq->avg.runnable_avg - se->avg.runnable_avg;
	u32 new_sum, divider;

	/* Nothing to update */
	/* avg 无差值时无需传播。 */
	if (!delta_avg)
		return;

	/*
	 * cfs_rq->avg.period_contrib can be used for both cfs_rq and se.
	 * See ___update_load_avg() for details.
	 */
	/* 采用父窗口 divider 重建组实体 runnable_sum，消除子/父窗口相位差。 */
	divider = get_pelt_divider(&cfs_rq->avg);

	/* Set new sched_entity's runnable */
	/* 更新组实体镜像并计算 sum 差值。 */
	se->avg.runnable_avg = gcfs_rq->avg.runnable_avg;
	new_sum = se->avg.runnable_avg * divider;
	delta_sum = (long)new_sum - (long)se->avg.runnable_sum;
	se->avg.runnable_sum = new_sum;

	/* Update parent cfs_rq runnable */
	/* 将镜像变化计入父队列 runnable 聚合。 */
	__update_sa(&cfs_rq->avg, runnable, delta_avg, delta_sum);
}

/*
 * update_tg_cfs_load() - 用子队列待传播 runnable 变化近似重建组实体 load 并更新父队列。
 *
 * 业务背景：load_sum 含历史权重和 blocked load，不能像 util 一样直接复制；本函数按增加/
 * 删除方向采用不同重叠假设，保持层级 PELT 可用且有界。
 * 入参：@cfs_rq 是父输入输出队列；@se 是组实体；@gcfs_rq 是带 prop_runnable_sum 的子队列。
 * 出参/返回：无直接返回；消费子传播量，更新 se load avg/sum 与父队列聚合。
 * 注意事项：runnable 不得低于 running，增加封顶 divider；近似允许误差但不能破坏范围不变量。
 */
static inline void
update_tg_cfs_load(struct cfs_rq *cfs_rq, struct sched_entity *se, struct cfs_rq *gcfs_rq)
{
	long delta_avg, running_sum, runnable_sum = gcfs_rq->prop_runnable_sum;
	unsigned long load_avg;
	u64 load_sum = 0;
	s64 delta_sum;
	u32 divider;

	if (!runnable_sum)
		return;

	/* 传播量由本层独占消费，先清零防重复向父层累计。 */
	gcfs_rq->prop_runnable_sum = 0;

	/*
	 * cfs_rq->avg.period_contrib can be used for both cfs_rq and se.
	 * See ___update_load_avg() for details.
	 */
	/* 所有重建 sum 都使用父 cfs_rq 当前 divider，使组实体与父窗口相位一致。 */
	divider = get_pelt_divider(&cfs_rq->avg);

	if (runnable_sum >= 0) {
		/*
		 * Add runnable; clip at LOAD_AVG_MAX. Reflects that until
		 * the CPU is saturated running == runnable.
		 */
		/* 增加时假定与已有 runnable 最少重叠，累加后最多到满窗口 divider。 */
		runnable_sum += se->avg.load_sum;
		runnable_sum = min_t(long, runnable_sum, divider);
	} else {
		/*
		 * Estimate the new unweighted runnable_sum of the gcfs_rq by
		 * assuming all tasks are equally runnable.
		 */
		/* 删除时用子队列加权 load_sum/总权重估计无权 runnable_sum。 */
		if (scale_load_down(gcfs_rq->load.weight)) {
			load_sum = div_u64(gcfs_rq->avg.load_sum,
				scale_load_down(gcfs_rq->load.weight));
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
		}

		/* But make sure to not inflate se's runnable */
		/* 删除传播绝不能把组实体 runnable 提高，因此取旧 sum 与估值较小者。 */
		runnable_sum = min(se->avg.load_sum, load_sum);
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}

	/*
	 * runnable_sum can't be lower than running_sum
	 * Rescale running sum to be in the same range as runnable sum
	 * running_sum is in [0 : LOAD_AVG_MAX <<  SCHED_CAPACITY_SHIFT]
	 * runnable_sum is in [0 : LOAD_AVG_MAX]
	 */
	/* util_sum 带容量缩放，右移后与 runnable 同量纲；实际运行是可运行子集，故作为下界。 */
	running_sum = se->avg.util_sum >> SCHED_CAPACITY_SHIFT;
	runnable_sum = max(runnable_sum, running_sum);

	load_sum = se_weight(se) * runnable_sum;
	load_avg = div_u64(load_sum, divider);

	delta_avg = load_avg - se->avg.load_avg;
	if (!delta_avg)
		return;

	delta_sum = load_sum - (s64)se_weight(se) * se->avg.load_sum;

	se->avg.load_sum = runnable_sum;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	se->avg.load_avg = load_avg;
	__update_sa(&cfs_rq->avg, load, delta_avg, delta_sum);
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
}

/*
 * add_tg_cfs_propagate() - 向组队列累积一笔待上推的 runnable_sum 变化。
 *
 * 业务背景：attach/detach/removed load 先记录本层差值，下一次实体传播再逐层向根传递。
 * 入参：@cfs_rq 是输入输出组队列；@runnable_sum 是可正可负的窗口量变化。
 * 出参/返回：无直接返回；置 propagate 并累加 prop_runnable_sum。
 * 注意事项：调用者持 rq 锁；多笔可合并，消费方必须清零防重复传播。
 */
static inline void add_tg_cfs_propagate(struct cfs_rq *cfs_rq, long runnable_sum)
{
	cfs_rq->propagate = 1;
	cfs_rq->prop_runnable_sum += runnable_sum;
}

/* Update task and its cfs_rq load average */
/* 更新组实体及父 cfs_rq 的 PELT；task 叶实体没有子队列，直接返回。 */
/*
 * propagate_entity_load_avg() - 消费子组待传播状态并同步组实体的 util/runnable/load。
 *
 * 业务背景：层级 PELT 更新沿 sched_entity 向父层推进，使根队列包含所有子组变化。
 * 入参：@se 是 rq 锁下输入输出实体；task 或无 pending 时无操作。
 * 出参/返回：发生组传播返回 1，否则 0；可能给父 cfs_rq 再挂传播量并发 tracepoint。
 * 注意事项：先清子 propagate 再更新，避免递归/重复消费；窗口误差由各 update_tg helper 处理。
 */
static inline int propagate_entity_load_avg(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq, *gcfs_rq;

	if (entity_is_task(se))
		return 0;

	gcfs_rq = group_cfs_rq(se);
	if (!gcfs_rq->propagate)
		return 0;

	gcfs_rq->propagate = 0;

	/* 先把子队列累计变化继续挂到父队列，再同步三类组实体镜像。 */
	cfs_rq = cfs_rq_of(se);

	add_tg_cfs_propagate(cfs_rq, gcfs_rq->prop_runnable_sum);

	update_tg_cfs_util(cfs_rq, se, gcfs_rq);
	update_tg_cfs_runnable(cfs_rq, se, gcfs_rq);
	update_tg_cfs_load(cfs_rq, se, gcfs_rq);

	trace_pelt_cfs_tp(cfs_rq);
	trace_pelt_se_tp(se);

	return 1;
}

/*
 * Check if we need to update the load and the utilization of a blocked
 * group_entity:
 */
/* 判断 blocked 组实体是否既已完全衰减又无待传播，从而可跳过昂贵 PELT 更新。 */
/*
 * skip_blocked_update() - 判断阻塞组实体本轮是否无需继续衰减/传播。
 *
 * 业务背景：NOHZ blocked-load 扫描会遍历叶组，已归零对象应尽早跳过降低成本。
 * 入参：@se 是 rq 锁/RCU 路径稳定的借用组实体。
 * 出参/返回：load/util 均零且子队列无 propagate 返回 true，否则 false。
 * 注意事项：只适用于组实体；runnable/load pending 任一存在都必须返回 false 继续更新。
 */
static inline bool skip_blocked_update(struct sched_entity *se)
{
	struct cfs_rq *gcfs_rq = group_cfs_rq(se);

	/*
	 * If sched_entity still have not zero load or utilization, we have to
	 * decay it:
	 */
	/* 实体仍携带 blocked load 或 util 历史时必须推进 PELT 衰减。 */
	if (se->avg.load_avg || se->avg.util_avg)
		return false;

	/*
	 * If there is a pending propagation, we have to update the load and
	 * the utilization of the sched_entity:
	 */
	/* 子队列即使自身 avg 已零，也可能有 detach 差值等待同步到父实体。 */
	if (gcfs_rq->propagate)
		return false;

	/*
	 * Otherwise, the load and the utilization of the sched_entity is
	 * already zero and there is no pending propagation, so it will be a
	 * waste of time to try to decay it:
	 */
	/* 状态和待传播均为空，继续调用只会产生零差值，可安全跳过。 */
	return true;
}

#else /* !CONFIG_FAIR_GROUP_SCHED: */

/* 无组调度时不存在 task_group 全局负载和，传播入口为空操作。 */
static inline void update_tg_load_avg(struct cfs_rq *cfs_rq) {}

/* 无组调度时 CPU 下线无需遍历组 cfs_rq。 */
static inline void clear_tg_offline_cfs_rqs(struct rq *rq) {}

/*
 * propagate_entity_load_avg() - 无组调度时报告没有层级 PELT 传播。
 *
 * 业务背景：公共 attach/update 路径保持统一接口。
 * 入参：@se 是借用 task 实体，本 stub 不读取。
 * 出参/返回：恒返回 0，无副作用。
 * 注意事项：task 对根 cfs_rq 的直接 PELT 更新仍正常执行。
 */
static inline int propagate_entity_load_avg(struct sched_entity *se)
{
	return 0;
}

/* 无组调度时没有父层待传播队列，runnable_sum 变化无需保存。 */
static inline void add_tg_cfs_propagate(struct cfs_rq *cfs_rq, long runnable_sum) {}

#endif /* !CONFIG_FAIR_GROUP_SCHED */

#ifdef CONFIG_NO_HZ_COMMON
/*
 * migrate_se_pelt_lag() - 在 idle NOHZ 源 CPU 上迁移前估算并补齐实体遗漏的 PELT 衰减。
 *
 * 业务背景：停止 tick 的 idle rq 时钟可能滞后，直接迁移会把过旧 blocked load 带到目标 CPU。
 * 入参：@se 是即将迁移的输入输出实体，调用者稳定其 cfs_rq/rq 归属。
 * 出参/返回：无直接返回；仅源 CPU idle 且可估时推进 se->avg 到估算 now。
 * 注意事项：估算成本较高只用于最大陈旧风险场景；CFS throttle 停钟时放弃，屏障偏向低估而非高估。
 */
static inline void migrate_se_pelt_lag(struct sched_entity *se)
{
	u64 throttled = 0, now, lut;
	struct cfs_rq *cfs_rq;
	struct rq *rq;
	bool is_idle;

	if (load_avg_is_decayed(&se->avg))
		return;

	cfs_rq = cfs_rq_of(se);
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	rq = rq_of(cfs_rq);

	rcu_read_lock();
	is_idle = is_idle_task(rcu_dereference_all(rq->curr));
	rcu_read_unlock();

	/*
	 * The lag estimation comes with a cost we don't want to pay all the
	 * time. Hence, limiting to the case where the source CPU is idle and
	 * we know we are at the greatest risk to have an outdated clock.
	 */
	/* 只有源 CPU idle 才值得支付估算成本，因为无 tick 时其 PELT clock 最可能陈旧。 */
	if (!is_idle)
		return;

	/*
	 * Estimated "now" is: last_update_time + cfs_idle_lag + rq_idle_lag, where:
	 *
	 *   last_update_time (the cfs_rq's last_update_time)
	 *	= cfs_rq_clock_pelt()@cfs_rq_idle
	 *      = rq_clock_pelt()@cfs_rq_idle
	 *        - cfs->throttled_clock_pelt_time@cfs_rq_idle
	 *
	 *   cfs_idle_lag (delta between rq's update and cfs_rq's update)
	 *      = rq_clock_pelt()@rq_idle - rq_clock_pelt()@cfs_rq_idle
	 *
	 *   rq_idle_lag (delta between now and rq's update)
	 *      = sched_clock_cpu() - rq_clock()@rq_idle
	 *
	 * We can then write:
	 *
	 *    now = rq_clock_pelt()@rq_idle - cfs->throttled_clock_pelt_time +
	 *          sched_clock_cpu() - rq_clock()@rq_idle
	 * Where:
	 *      rq_clock_pelt()@rq_idle is rq->clock_pelt_idle
	 *      rq_clock()@rq_idle      is rq->clock_idle
	 *      cfs->throttled_clock_pelt_time@cfs_rq_idle
	 *                              is cfs_rq->throttled_pelt_idle
	 */
	/*
	 * 估算 now 由 rq idle 时保存的 PELT clock，加上从 rq idle 到当前的 sched_clock 差，再
	 * 减 cfs_rq throttle 停钟量组成；这样无需重新启动 tick 也能近似 blocked 衰减时间。
	 */

#ifdef CONFIG_CFS_BANDWIDTH
	throttled = u64_u32_load(cfs_rq->throttled_pelt_idle);
	/* The clock has been stopped for throttling */
	/* U64_MAX 表示 throttle 停钟基线不可用于估算，避免把暂停时间错误计入衰减。 */
	if (throttled == U64_MAX)
		return;
#endif
	now = u64_u32_load(rq->clock_pelt_idle);
	/*
	 * Paired with _update_idle_rq_clock_pelt(). It ensures at the worst case
	 * is observed the old clock_pelt_idle value and the new clock_idle,
	 * which lead to an underestimation. The opposite would lead to an
	 * overestimation.
	 */
	/*
	 * 与 idle rq clock 写侧配对：最坏读到旧 pelt_idle 与新 clock_idle，只会低估 elapsed；
	 * 相反组合会高估衰减并丢失负载，因此用读屏障禁止该危险观察顺序。
	 */
	smp_rmb();
	lut = cfs_rq_last_update_time(cfs_rq);

	now -= throttled;
	if (now < lut)
		/*
		 * cfs_rq->avg.last_update_time is more recent than our
		 * estimation, let's use it.
		 */
		/* 实体真实 last_update_time 已更新得更近时以它为下界，绝不让 PELT 时间倒退。 */
		now = lut;
	else
		now += sched_clock_cpu(cpu_of(rq)) - u64_u32_load(rq->clock_idle);

	__update_load_avg_blocked_se(now, se);
}
#else /* !CONFIG_NO_HZ_COMMON: */
/* NOHZ common 关闭时 rq PELT clock 持续更新，迁移无需额外 lag 估算。 */
static void migrate_se_pelt_lag(struct sched_entity *se) {}
#endif /* !CONFIG_NO_HZ_COMMON */

/**
 * update_cfs_rq_load_avg - update the cfs_rq's load/util averages
 * @now: current time, as per cfs_rq_clock_pelt()
 * @cfs_rq: cfs_rq to update
 *
 * The cfs_rq avg is the direct sum of all its entities (blocked and runnable)
 * avg. The immediate corollary is that all (fair) tasks must be attached.
 *
 * cfs_rq->avg is used for task_h_load() and update_cfs_share() for example.
 *
 * Return: true if the load decayed or we removed load.
 *
 * Since both these conditions indicate a changed cfs_rq->avg.load we should
 * call update_tg_load_avg() when this function returns true.
 */
/*
 * update_cfs_rq_load_avg() - 将 cfs_rq 的 PELT 聚合推进到 now 并消费异步 removed 贡献。
 *
 * 业务背景：队列 avg 是所有 runnable/blocked fair 实体之和；迁移摘除可能在无目标 rq 锁
 * 上下文先记入 removed，当前更新再批量撤销并衰减。
 * 入参：@now 是 cfs_rq PELT 时钟时间；@cfs_rq 是 rq 锁保护的输入输出队列。
 * 出参/返回：load 发生衰减或移除返回非零，提示调用者更新 tg 全局和；否则 0。
 * 注意事项：所有 fair task 必须 attach；removed.lock 仅保护交换缓冲，真正聚合更新仍在 rq 锁下。
 */
static inline int
update_cfs_rq_load_avg(u64 now, struct cfs_rq *cfs_rq)
{
	unsigned long removed_load = 0, removed_util = 0, removed_runnable = 0;
	struct sched_avg *sa = &cfs_rq->avg;
	int decayed = 0;

	if (cfs_rq->removed.nr) {
		/* 锁内一次性交换出异步移除累计并清零共享缓冲，缩短 removed.lock 临界区。 */
		unsigned long r;
		u32 divider = get_pelt_divider(&cfs_rq->avg);

		raw_spin_lock(&cfs_rq->removed.lock);
		swap(cfs_rq->removed.util_avg, removed_util);
		swap(cfs_rq->removed.load_avg, removed_load);
		swap(cfs_rq->removed.runnable_avg, removed_runnable);
		cfs_rq->removed.nr = 0;
		raw_spin_unlock(&cfs_rq->removed.lock);

		r = removed_load;
		/* 三类 avg/sum 使用同一当前 divider 撤销，__update_sa 负责下溢钳零。 */
		__update_sa(sa, load, -r, -r*divider);

		r = removed_util;
		__update_sa(sa, util, -r, -r*divider);

		r = removed_runnable;
		__update_sa(sa, runnable, -r, -r*divider);

		/*
		 * removed_runnable is the unweighted version of removed_load so we
		 * can use it to estimate removed_load_sum.
		 */
		/* 无权 runnable 可近似已移除 load_sum，取负后挂到组传播以同步父层。 */
		add_tg_cfs_propagate(cfs_rq,
			-(long)(removed_runnable * divider) >> SCHED_CAPACITY_SHIFT);

		decayed = 1;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}

	decayed |= __update_load_avg_cfs_rq(now, cfs_rq);
	/* 保存 64 位更新时间及 32 位副本，供无 rq 锁迁移路径无撕裂读取。 */
	u64_u32_store_copy(sa->last_update_time,
			   cfs_rq->last_update_time_copy,
			   sa->last_update_time);
	return decayed;
}

/**
 * attach_entity_load_avg - attach this entity to its cfs_rq load avg
 * @cfs_rq: cfs_rq to attach to
 * @se: sched_entity to attach
 *
 * Must call update_cfs_rq_load_avg() before this, since we rely on
 * cfs_rq->avg.last_update_time being current.
 */
/* 迁入实体先与目标 PELT 时间轴对齐，再把 load/util/runnable 加入聚合。 */
/*
 * attach_entity_load_avg() - 将实体 PELT 历史对齐并附着到目标 cfs_rq 聚合。
 *
 * 业务背景：迁移/首次入队的实体来自不同或未初始化时间窗，直接相加会破坏指数衰减相位。
 * 入参：@cfs_rq 是已更新到当前 PELT 时间的输入输出队列；@se 是待附着输入输出实体。
 * 出参/返回：无直接返回；重建 se sums，加入队列三类聚合、挂组传播并通知 cpufreq。
 * 注意事项：必须先 update_cfs_rq_load_avg()；重建 sum 有少量截断但实体此时在层级外可接受。
 */
static void attach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	/*
	 * cfs_rq->avg.period_contrib can be used for both cfs_rq and se.
	 * See ___update_load_avg() for details.
	 */
	/* 实体附着后共享目标队列 period_contrib，可用同一 divider 重建全部 sum。 */
	u32 divider = get_pelt_divider(&cfs_rq->avg);

	/*
	 * When we attach the @se to the @cfs_rq, we must align the decay
	 * window because without that, really weird and wonderful things can
	 * happen.
	 *
	 * XXX illustrate
	 */
	/* 先把 last_update_time/period_contrib 对齐到目标窗口，否则相同 avg 会在后续衰减出异常跳变。 */
	se->avg.last_update_time = cfs_rq->avg.last_update_time;
	se->avg.period_contrib = cfs_rq->avg.period_contrib;

	/*
	 * Hell(o) Nasty stuff.. we need to recompute _sum based on the new
	 * period_contrib. This isn't strictly correct, but since we're
	 * entirely outside of the PELT hierarchy, nobody cares if we truncate
	 * _sum a little.
	 */
	/* 从 avg 反推新窗口 sum 会截断部分低位，但层级外没有并发消费者，换取相位一致更重要。 */
	se->avg.util_sum = se->avg.util_avg * divider;

	se->avg.runnable_sum = se->avg.runnable_avg * divider;

	se->avg.load_sum = se->avg.load_avg * divider;
	if (se_weight(se) < se->avg.load_sum)
		se->avg.load_sum = div_u64(se->avg.load_sum, se_weight(se));
	else
		se->avg.load_sum = 1;

	enqueue_load_avg(cfs_rq, se);
	/* load 由加权 helper 维护，util/runnable 直接相加；随后向父组传播无权 load_sum。 */
	cfs_rq->avg.util_avg += se->avg.util_avg;
	cfs_rq->avg.util_sum += se->avg.util_sum;
	cfs_rq->avg.runnable_avg += se->avg.runnable_avg;
	cfs_rq->avg.runnable_sum += se->avg.runnable_sum;

	add_tg_cfs_propagate(cfs_rq, se->avg.load_sum);

	cfs_rq_util_change(cfs_rq, 0);

	trace_pelt_cfs_tp(cfs_rq);
}

/**
 * detach_entity_load_avg - detach this entity from its cfs_rq load avg
 * @cfs_rq: cfs_rq to detach from
 * @se: sched_entity to detach
 *
 * Must call update_cfs_rq_load_avg() before this, since we rely on
 * cfs_rq->avg.last_update_time being current.
 */
/* 从目标聚合扣除实体贡献并记入 removed 同步，避免无锁更新出现负值。 */
/*
 * detach_entity_load_avg() - 从 cfs_rq PELT 聚合撤销实体并向父层传播负贡献。
 *
 * 业务背景：迁出实体离开当前 PELT 层级前，目标队列必须不再包含其 blocked/runnable 历史。
 * 入参：@cfs_rq 是已更新到当前时间的输入输出队列；@se 是待分离借用实体。
 * 出参/返回：无直接返回；减三类聚合、挂负 load_sum、通知 cpufreq 并 trace。
 * 注意事项：__update_sa 对舍入下溢钳零；函数不清实体自身 avg，供迁入目标继续携带历史。
 */
static void detach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	dequeue_load_avg(cfs_rq, se);
	__update_sa(&cfs_rq->avg, util, -se->avg.util_avg, -se->avg.util_sum);
	__update_sa(&cfs_rq->avg, runnable, -se->avg.runnable_avg, -se->avg.runnable_sum);

	add_tg_cfs_propagate(cfs_rq, -se->avg.load_sum);

	cfs_rq_util_change(cfs_rq, 0);

	trace_pelt_cfs_tp(cfs_rq);
}

#define UTIL_EST_MARGIN (SCHED_CAPACITY_SCALE / 100)
/* util_est 的约 1% 容量误差带，过滤无意义小幅 EWMA 更新。 */

/*
 * util_est_update() - 在 task 一次 activation 完成出队时更新保守利用率 EWMA。
 *
 * 业务背景：PELT 对突发任务上升太慢；util_est 上升立即跟随、下降才平滑，为选核/频率提供保守需求。
 * 入参：@se 是输入输出 task 实体，调用者处在 rq 锁下。
 * 出参/返回：无直接返回；可能更新 util_est 并置 UTIL_AVG_UNCHANGED 哨兵。
 * 注意事项：runnable 明显高于实际 util 表示 task 未获足 CPU，此时不能把低 util 样本当成需求下降。
 */
static inline void util_est_update(struct sched_entity *se)
{
	unsigned int ewma, dequeued, last_ewma_diff;

	if (!sched_feat(UTIL_EST))
		return;

	/* Get current estimate of utilization */
	/* 单次读取同时取得 EWMA 与 unchanged 标志，避免并发观察撕裂。 */
	ewma = READ_ONCE(se->avg.util_est);

	/*
	 * If the PELT values haven't changed since enqueue time,
	 * skip the util_est update.
	 */
	/* activation 内 PELT 未变化时没有新样本，保留旧估值。 */
	if (ewma & UTIL_AVG_UNCHANGED)
		return;

	/* Get utilization at dequeue */
	/* dequeue 时 util_avg 代表本次 activation 结束样本。 */
	dequeued = READ_ONCE(se->avg.util_avg);

	/*
	 * Reset EWMA on utilization increases, the moving average is used only
	 * to smooth utilization decreases.
	 */
	/* 需求上升立即提升估值，避免低估；仅下降使用 EWMA 防短暂空闲过快降频。 */
	if (ewma <= dequeued) {
		ewma = dequeued;
		goto done;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}

	/*
	 * Skip update of task's estimated utilization when its members are
	 * already ~1% close to its last activation value.
	 */
	/* 新旧估值差小于 1% 容量时直接保留，减少热路径写与数值抖动。 */
	last_ewma_diff = ewma - dequeued;
	if (last_ewma_diff < UTIL_EST_MARGIN)
		goto done;

	/*
	 * To avoid underestimate of task utilization, skip updates of EWMA if
	 * we cannot grant that thread got all CPU time it wanted.
	 */
	/* runnable 比 util 高出误差带说明 task 受 CPU 竞争限制，低 util 不是低需求，不能下调估值。 */
	if ((dequeued + UTIL_EST_MARGIN) < READ_ONCE(se->avg.runnable_avg))
		goto done;

	/*
	 * Update Task's estimated utilization
	 *
	 * When *p completes an activation we can consolidate another sample
	 * of the task size. This is done by using this value to update the
	 * Exponential Weighted Moving Average (EWMA):
	 *
	 *  ewma(t) = w *  task_util(p) + (1-w) * ewma(t-1)
	 *          = w *  task_util(p) +         ewma(t-1)  - w * ewma(t-1)
	 *          = w * (task_util(p) -         ewma(t-1)) +     ewma(t-1)
	 *          = w * (      -last_ewma_diff           ) +     ewma(t-1)
	 *          = w * (-last_ewma_diff +  ewma(t-1) / w)
	 *
	 * Where 'w' is the weight of new samples, which is configured to be
	 * 0.25, thus making w=1/4 ( >>= UTIL_EST_WEIGHT_SHIFT)
	 */
	/* 每次 activation 以 1/4 新样本、3/4 旧值更新；移位形式避免乘除并保持无符号算术。 */
	ewma <<= UTIL_EST_WEIGHT_SHIFT;
	ewma  -= last_ewma_diff;
	ewma >>= UTIL_EST_WEIGHT_SHIFT;
done:
	/* 标记自本次 dequeue 起 util_avg 未再改变；下一 enqueue/update 路径会清除该位。 */
	ewma |= UTIL_AVG_UNCHANGED;
	WRITE_ONCE(se->avg.util_est, ewma);

	trace_sched_util_est_se_tp(se);
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
}

/*
 * Optional action to be done while updating the load average
 */
/* update_load_avg() 行为位：传播组、跳过 aging、附着、分离和更新 util_est，可按调用边界组合。 */
#define UPDATE_TG	0x01
#define SKIP_AGE_LOAD	0x02
#define DO_ATTACH	0x04
#define DO_DETACH	0x08
#define UPDATE_UTIL_EST	0x10

/* Update task and its cfs_rq load average */
/* 更新实体及所属 cfs_rq PELT，并按 flags 执行迁移 attach/detach、组传播或 util_est。 */
/*
 * update_load_avg() - 统一推进实体/队列 PELT 并处理层级与迁移边界动作。
 *
 * 业务背景：enqueue/dequeue/tick 等路径需要相同时间顺序，但附着、分离和全局组更新条件不同。
 * 入参：@cfs_rq 是 rq 锁保护输入输出队列；@se 是输入输出实体；@flags 为上述行为位图。
 * 出参/返回：无直接返回；可能衰减、传播、attach/detach、通知 cpufreq/tg 并更新 util_est。
 * 注意事项：DO_ATTACH 仅对迁移后 last_update_time=0 实体；DO_DETACH 仅在迁出且实体已离队时使用。
 */
static inline void update_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	u64 now = cfs_rq_clock_pelt(cfs_rq);
	int decayed;

	/*
	 * Track task load average for carrying it to new CPU after migrated, and
	 * track group sched_entity load average for task_h_load calculation in migration
	 */
	/* task avg 随迁移携带；组实体 avg 供 task_h_load 层级计算，除显式 SKIP 外先推进到 now。 */
	if (se->avg.last_update_time && !(flags & SKIP_AGE_LOAD))
		__update_load_avg_se(now, cfs_rq, se);

	decayed  = update_cfs_rq_load_avg(now, cfs_rq);
	decayed |= propagate_entity_load_avg(se);

	if (!se->avg.last_update_time && (flags & DO_ATTACH)) {

		/*
		 * DO_ATTACH means we're here from enqueue_entity().
		 * !last_update_time means we've passed through
		 * migrate_task_rq_fair() indicating we migrated.
		 *
		 * IOW we're enqueueing a task on a new CPU.
		 */
		/* DO_ATTACH 且时间为零标识 migrate_task_rq_fair() 已清旧时基，当前正入新 CPU。 */
		attach_entity_load_avg(cfs_rq, se);
		update_tg_load_avg(cfs_rq);

	} else if (flags & DO_DETACH) {
		/*
		 * DO_DETACH means we're here from dequeue_entity()
		 * and we are migrating task out of the CPU.
		 */
		/* 迁出 dequeue 在旧 CPU 撤销聚合，但保留实体历史供目标 attach。 */
		detach_entity_load_avg(cfs_rq, se);
		update_tg_load_avg(cfs_rq);
	} else if (decayed) {
		/* 普通衰减/传播改变 util 时通知频率；仅请求 UPDATE_TG 的调用点更新全局组和。 */
		cfs_rq_util_change(cfs_rq, 0);

		if (flags & UPDATE_TG)
			update_tg_load_avg(cfs_rq);
	}

	if (flags & UPDATE_UTIL_EST)
		util_est_update(se);
}

/*
 * Synchronize entity load avg of dequeued entity without locking
 * the previous rq.
 */
/* 不锁旧 rq，将已出队实体的 blocked PELT 衰减追赶到旧 cfs_rq 最近时间。 */
/*
 * sync_entity_load_avg() - 用 cfs_rq 无撕裂时间副本推进已出队实体的 blocked load。
 *
 * 业务背景：迁移/退出路径可能无法取得旧 rq 锁，但需先把实体统计更新到可撤销时点。
 * 入参：@se 是已离队输入输出实体，所属旧 cfs_rq 仍有效。
 * 出参/返回：无直接返回；调用 blocked_se helper 衰减 se->avg。
 * 注意事项：时间是近似快照且只推进实体，不更新队列；因此后续通过 removed 缓冲异步扣除。
 */
static void sync_entity_load_avg(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	u64 last_update_time;

	last_update_time = cfs_rq_last_update_time(cfs_rq);
	__update_load_avg_blocked_se(last_update_time, se);
}

/*
 * Task first catches up with cfs_rq, and then subtract
 * itself from the cfs_rq (task must be off the queue now).
 */
/* task 先追赶旧 cfs_rq 时间，再把自身 avg 记入 removed 缓冲；前提是已不在运行队列。 */
/*
 * remove_entity_load_avg() - 在无需旧 rq 锁的退出路径登记待撤销实体 PELT 贡献。
 *
 * 业务背景：task 最终离开调度层级时，直接改 cfs_rq avg 会与 rq 更新竞争；removed.lock 缓冲差值。
 * 入参：@se 是已 off-rq、将不再附着的输入输出 task 实体。
 * 出参/返回：无直接返回；推进实体 blocked avg，并累加 removed nr/util/load/runnable。
 * 注意事项：所有 task 在退出前都曾经 wake_up_new_task→enqueue，故可无条件登记一次移除。
 */
static void remove_entity_load_avg(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);
	unsigned long flags;

	/*
	 * tasks cannot exit without having gone through wake_up_new_task() ->
	 * enqueue_task_fair() which will have added things to the cfs_rq,
	 * so we can remove unconditionally.
	 */
	/* 进程退出前必经首次唤醒入队，队列一定曾包含其贡献；此处无需“是否 attach”分支。 */

	sync_entity_load_avg(se);

	raw_spin_lock_irqsave(&cfs_rq->removed.lock, flags);
	++cfs_rq->removed.nr;
	cfs_rq->removed.util_avg	+= se->avg.util_avg;
	cfs_rq->removed.load_avg	+= se->avg.load_avg;
	cfs_rq->removed.runnable_avg	+= se->avg.runnable_avg;
	raw_spin_unlock_irqrestore(&cfs_rq->removed.lock, flags);
}

/*
 * cfs_rq_runnable_avg() - 读取队列 PELT runnable 平均。
 *
 * 业务背景：容量/负载平衡调用点通过统一 helper 获取可运行需求。
 * 入参：@cfs_rq 是调用者稳定的借用队列。
 * 出参/返回：返回无单位容量刻度 runnable_avg；无副作用。
 * 注意事项：无锁调用得到近似快照，不提供生命周期保护。
 */
static inline unsigned long cfs_rq_runnable_avg(struct cfs_rq *cfs_rq)
{
	return cfs_rq->avg.runnable_avg;
}

/*
 * cfs_rq_load_avg() - 读取队列包含 blocked 历史的 PELT load 平均。
 *
 * 业务背景：层级 shares 和负载平衡需要预期将返回的睡眠负载，而非仅当前 runnable。
 * 入参：@cfs_rq 是借用队列。
 * 出参/返回：返回 load_avg 快照；无状态或所有权变化。
 * 注意事项：与 runnable/util 语义不同，不能互换作为 CPU 容量需求。
 */
static inline unsigned long cfs_rq_load_avg(struct cfs_rq *cfs_rq)
{
	return cfs_rq->avg.load_avg;
}

static int sched_balance_newidle(struct rq *this_rq, struct rq_flags *rf)
	__must_hold(__rq_lockp(this_rq));

/*
 * task_util() - 单次读取 task 当前 PELT util_avg。
 *
 * 业务背景：选核、misfit 和能耗模型共享实际衰减利用率输入。
 * 入参：@p 是借用 task。
 * 出参/返回：返回容量刻度 util 快照；无副作用。
 * 注意事项：READ_ONCE 防编译器重复读取，不保证与 runnable/util_est 同代。
 */
static inline unsigned long task_util(struct task_struct *p)
{
	return READ_ONCE(p->se.avg.util_avg);
}

/*
 * _task_util_est() - 读取并移除 util_est 内部 unchanged 标志后的纯估值。
 *
 * 业务背景：util_est 与控制位复用一个字段，容量计算只能使用数值部分。
 * 入参：@p 是借用 task。
 * 出参/返回：返回容量刻度估值；无副作用。
 * 注意事项：下划线版本不与 PELT 取最大值，仅供聚合增减和包装函数使用。
 */
static inline unsigned long _task_util_est(struct task_struct *p)
{
	return READ_ONCE(p->se.avg.util_est) & ~UTIL_AVG_UNCHANGED;
}

/* util_est 取 PELT 与突发估值较大者，为尚未收敛的新负载提供保守容量需求。 */
/*
 * task_util_est() - 返回 task 的保守有效利用率需求。
 *
 * 业务背景：PELT 捕获稳态、util_est 捕获最近 activation，选核不能低估二者任一。
 * 入参：@p 是借用 task。
 * 出参/返回：返回 task_util 与纯 util_est 较大值；无副作用。
 * 注意事项：结果可超过当前实际运行，属于刻意保守预测。
 */
static inline unsigned long task_util_est(struct task_struct *p)
{
	return max(task_util(p), _task_util_est(p));
}

/*
 * util_est_enqueue() - 将 task 估值加入根 cfs_rq 的快速利用率总量。
 *
 * 业务背景：schedutil/选核需 O(1) 获得所有排队 task 的突发需求和。
 * 入参：@cfs_rq 必须是 rq 锁下根队列；@p 是正在入队的借用 task。
 * 出参/返回：无直接返回；UTIL_EST 开启时增加 avg.util_est 并发 trace。
 * 注意事项：必须与 dequeue 恰好配对；字段是估值聚合，不等于 PELT util_avg。
 */
static inline void util_est_enqueue(struct cfs_rq *cfs_rq,
				    struct task_struct *p)
{
	unsigned int enqueued;

	if (!sched_feat(UTIL_EST))
		return;

	/* Update root cfs_rq's estimated utilization */
	/* rq 锁串行读改写；WRITE_ONCE 让锁外观察者得到单次完整值。 */
	enqueued  = cfs_rq->avg.util_est;
	enqueued += _task_util_est(p);
	WRITE_ONCE(cfs_rq->avg.util_est, enqueued);

	trace_sched_util_est_cfs_tp(cfs_rq);
}

/*
 * util_est_dequeue() - 从根 cfs_rq 估值总量撤销 task 贡献。
 *
 * 业务背景：task 离队后不应继续驱动 CPU 频率/过载判断。
 * 入参：@cfs_rq 是锁定根队列；@p 是正在出队的借用 task。
 * 出参/返回：无直接返回；减去纯 util_est 并钳到零、发 trace。
 * 注意事项：min_t 防配对误差或并发近似导致无符号下溢。
 */
static inline void util_est_dequeue(struct cfs_rq *cfs_rq,
				    struct task_struct *p)
{
	unsigned int enqueued;

	if (!sched_feat(UTIL_EST))
		return;

	/* Update root cfs_rq's estimated utilization */
	/* 撤销不超过当前总量，保持无锁读者永不看到回绕巨值。 */
	enqueued  = cfs_rq->avg.util_est;
	enqueued -= min_t(unsigned int, enqueued, _task_util_est(p));
	WRITE_ONCE(cfs_rq->avg.util_est, enqueued);

	trace_sched_util_est_cfs_tp(cfs_rq);
}

/*
 * get_actual_cpu_capacity() - 计算 uclamp_min 可实际获得的 CPU 原始容量。
 *
 * 业务背景：硬件与 cpufreq pressure 会直接降低可用 OPP，最小性能保证必须考虑它们。
 * 入参：@cpu 是有效逻辑 CPU。
 * 出参/返回：返回原始架构容量减去两类 pressure 较大值；无副作用。
 * 注意事项：pressure 是瞬时近似；调用者保证减法不会在合法容量范围下溢。
 */
static inline unsigned long get_actual_cpu_capacity(int cpu)
{
	unsigned long capacity = arch_scale_cpu_capacity(cpu);

	capacity -= max(hw_load_avg(cpu_rq(cpu)), cpufreq_get_pressure(cpu));

	return capacity;
}

/* 同时检查 capacity 与 uclamp min/max；返回值区分完全适配和仅 capacity 适配。 */
/*
 * util_fits_cpu() - 综合真实 util、CPU 容量压力与 uclamp 提示判断任务是否适配 CPU。
 *
 * 业务背景：异构选核既要避免真实需求过载，又要兑现 UCLAMP_MIN 性能下限和 UCLAMP_MAX 封顶意图。
 * 入参：@util/@uclamp_min/@uclamp_max 为容量刻度值；@cpu 是候选逻辑 CPU。
 * 出参/返回：1 完全适配，0 真实容量不适配，-1 表示真实 util 适配但最小性能提示无法兑现。
 * 注意事项：真实 util 用受 pressure 的 capacity；uclamp 比较多用原始容量，只有 min 考虑 HW/cpufreq pressure。
 */
static inline int util_fits_cpu(unsigned long util,
				unsigned long uclamp_min,
				unsigned long uclamp_max,
				int cpu)
{
	unsigned long capacity = capacity_of(cpu);
	unsigned long capacity_orig;
	bool fits, uclamp_max_fits;

	/*
	 * Check if the real util fits without any uclamp boost/cap applied.
	 */
	/* 第一层仅按实际可用 capacity 与调度余量检查未经 clamp 的真实需求。 */
	fits = fits_capacity(util, capacity);

	if (!uclamp_is_used())
		return fits;

	/*
	 * We must use arch_scale_cpu_capacity() for comparing against uclamp_min and
	 * uclamp_max. We only care about capacity pressure (by using
	 * capacity_of()) for comparing against the real util.
	 *
	 * If a task is boosted to 1024 for example, we don't want a tiny
	 * pressure to skew the check whether it fits a CPU or not.
	 *
	 * Similarly if a task is capped to arch_scale_cpu_capacity(little_cpu), it
	 * should fit a little cpu even if there's some pressure.
	 *
	 * Only exception is for HW or cpufreq pressure since it has a direct impact
	 * on available OPP of the system.
	 *
	 * We honour it for uclamp_min only as a drop in performance level
	 * could result in not getting the requested minimum performance level.
	 *
	 * For uclamp_max, we can tolerate a drop in performance level as the
	 * goal is to cap the task. So it's okay if it's getting less.
	 */
	/*
	 * clamp 表达性能提示而非当前拥塞：轻微运行压力不应让 boost/cap 改变 CPU 类别。硬件或
	 * cpufreq pressure 会降低实际 OPP，故 UCLAMP_MIN 必须考虑；UCLAMP_MAX 允许获得更少性能。
	 */
	capacity_orig = arch_scale_cpu_capacity(cpu);

	/*
	 * We want to force a task to fit a cpu as implied by uclamp_max.
	 * But we do have some corner cases to cater for..
	 *
	 *
	 *                                 C=z
	 *   |                             ___
	 *   |                  C=y       |   |
	 *   |_ _ _ _ _ _ _ _ _ ___ _ _ _ | _ | _ _ _ _ _  uclamp_max
	 *   |      C=x        |   |      |   |
	 *   |      ___        |   |      |   |
	 *   |     |   |       |   |      |   |    (util somewhere in this region)
	 *   |     |   |       |   |      |   |
	 *   |     |   |       |   |      |   |
	 *   +----------------------------------------
	 *         CPU0        CPU1       CPU2
	 *
	 *   In the above example if a task is capped to a specific performance
	 *   point, y, then when:
	 *
	 *   * util = 80% of x then it does not fit on CPU0 and should migrate
	 *     to CPU1
	 *   * util = 80% of y then it is forced to fit on CPU1 to honour
	 *     uclamp_max request.
	 *
	 *   which is what we're enforcing here. A task always fits if
	 *   uclamp_max <= capacity_orig. But when uclamp_max > capacity_orig,
	 *   the normal upmigration rules should withhold still.
	 *
	 *   Only exception is when we are on max capacity, then we need to be
	 *   careful not to block overutilized state. This is so because:
	 *
	 *     1. There's no concept of capping at max_capacity! We can't go
	 *        beyond this performance level anyway.
	 *     2. The system is being saturated when we're operating near
	 *        max capacity, it doesn't make sense to block overutilized.
	 */
	/*
	 * cap 不高于 CPU 原始容量时强制视为可容纳，兑现“无需为超过 cap 的 util 上迁”；但
	 * max CPU 上 clamp_max=系统最大不是真正封顶，仍须允许 overutilized 被识别。
	 */
	uclamp_max_fits = (capacity_orig == SCHED_CAPACITY_SCALE) && (uclamp_max == SCHED_CAPACITY_SCALE);
	uclamp_max_fits = !uclamp_max_fits && (uclamp_max <= capacity_orig);
	fits = fits || uclamp_max_fits;

	/*
	 *
	 *                                 C=z
	 *   |                             ___       (region a, capped, util >= uclamp_max)
	 *   |                  C=y       |   |
	 *   |_ _ _ _ _ _ _ _ _ ___ _ _ _ | _ | _ _ _ _ _ uclamp_max
	 *   |      C=x        |   |      |   |
	 *   |      ___        |   |      |   |      (region b, uclamp_min <= util <= uclamp_max)
	 *   |_ _ _|_ _|_ _ _ _| _ | _ _ _| _ | _ _ _ _ _ uclamp_min
	 *   |     |   |       |   |      |   |
	 *   |     |   |       |   |      |   |      (region c, boosted, util < uclamp_min)
	 *   +----------------------------------------
	 *         CPU0        CPU1       CPU2
	 *
	 * a) If util > uclamp_max, then we're capped, we don't care about
	 *    actual fitness value here. We only care if uclamp_max fits
	 *    capacity without taking margin/pressure into account.
	 *    See comment above.
	 *
	 * b) If uclamp_min <= util <= uclamp_max, then the normal
	 *    fits_capacity() rules apply. Except we need to ensure that we
	 *    enforce we remain within uclamp_max, see comment above.
	 *
	 * c) If util < uclamp_min, then we are boosted. Same as (b) but we
	 *    need to take into account the boosted value fits the CPU without
	 *    taking margin/pressure into account.
	 *
	 * Cases (a) and (b) are handled in the 'fits' variable already. We
	 * just need to consider an extra check for case (c) after ensuring we
	 * handle the case uclamp_min > uclamp_max.
	 */
	/*
	 * util 高于 max 或位于 min/max 间由 fits/uclamp_max_fits 处理；util 低于 min 属 boost，
	 * 还需确认考虑 HW/cpufreq pressure 后的实际容量能兑现 min。先把 min 钳到 max 处理冲突提示。
	 */
	uclamp_min = min(uclamp_min, uclamp_max);
	if (fits && (util < uclamp_min) &&
	    (uclamp_min > get_actual_cpu_capacity(cpu)))
		return -1;

	return fits;
}

/*
 * task_fits_cpu() - 用 task 有效 clamp 与保守 util_est 判断候选 CPU 是否完全适配。
 *
 * 业务背景：wakeup placement 和 misfit 只接受同时满足容量与性能提示的 CPU。
 * 入参：@p 是借用 task；@cpu 是候选 CPU。
 * 出参/返回：util_fits_cpu()>0 返回 true；0 或 -1 均返回 false。
 * 注意事项：有效 clamp 已合并 cgroup/task 约束，结果为瞬时容量快照。
 */
static inline int task_fits_cpu(struct task_struct *p, int cpu)
{
	unsigned long uclamp_min = uclamp_eff_value(p, UCLAMP_MIN);
	unsigned long uclamp_max = uclamp_eff_value(p, UCLAMP_MAX);
	unsigned long util = task_util_est(p);
	/*
	 * Return true only if the cpu fully fits the task requirements, which
	 * include the utilization but also the performance hints.
	 */
	/* 严格要求正返回；仅真实 util 合适但 boost 无法兑现的 -1 仍视为不适配。 */
	return (util_fits_cpu(util, uclamp_min, uclamp_max, cpu) > 0);
}

/*
 * update_misfit_status() - 更新 rq 上当前 task 是否需要向更高容量 CPU 迁移的负载标记。
 *
 * 业务背景：异构负载平衡通过 misfit_task_load 主动拉走放在小核且无法满足需求的 task。
 * 入参：@p 可为 NULL，是当前候选 task；@rq 是锁定输入输出运行队列。
 * 出参/返回：无直接返回；清零或写至少为 1 的 misfit_task_load。
 * 注意事项：单 CPU affinity、已在允许范围最大核或完全适配时不能标 misfit；只在 asym capacity 激活时更新。
 */
static inline void update_misfit_status(struct task_struct *p, struct rq *rq)
{
	int cpu = cpu_of(rq);

	if (!sched_asym_cpucap_active())
		return;

	/*
	 * Affinity allows us to go somewhere higher?  Or are we on biggest
	 * available CPU already? Or do we fit into this CPU ?
	 */
	/* 没有更大允许 CPU、已在最大允许容量或当前 CPU 能满足时，迁移无可行收益，清标记。 */
	if (!p || (p->nr_cpus_allowed == 1) ||
	    (arch_scale_cpu_capacity(cpu) == p->max_allowed_capacity) ||
	    task_fits_cpu(p, cpu)) {

		rq->misfit_task_load = 0;
		return;
	}

	/*
	 * Make sure that misfit_task_load will not be null even if
	 * task_h_load() returns 0.
	 */
	/* 层级负载可能因舍入为零，仍写 1 保证 load balancer 能观察到 misfit。 */
	rq->misfit_task_load = max_t(unsigned long, task_h_load(p), 1);
}

/*
 * __setparam_fair() - 将 sched_attr nice 和可选 runtime 请求安装到 fair 实体。
 *
 * 业务背景：调度参数系统调用先更新策略字段，后续 reweight/deadline 路径据此采用新优先级与 slice。
 * 入参：@p 是输入输出 task；@attr 是已校验的借用属性结构。
 * 出参/返回：无直接返回；更新 static_prio、custom_slice 和 slice。
 * 注意事项：自定义 runtime 被钳到 0.1ms..100ms，零值恢复全局 base_slice；本函数不直接重排队列。
 */
void __setparam_fair(struct task_struct *p, const struct sched_attr *attr)
{
	struct sched_entity *se = &p->se;

	p->static_prio = NICE_TO_PRIO(attr->sched_nice);
	if (attr->sched_runtime) {
		se->custom_slice = 1;
		se->slice = clamp_t(u64, attr->sched_runtime,
				      NSEC_PER_MSEC/10,   /* HZ=1000 * 10 */
				      NSEC_PER_MSEC*100); /* HZ=100  / 10 */
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	} else {
		se->custom_slice = 0;
		se->slice = sysctl_sched_base_slice;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}

/* 唤醒/初生实体按保存 lag 映射到当前 V，并计算新虚拟 deadline。 */
/*
 * place_entity() - 把新入队/迁入实体的保存 lag 映射到当前 EEVDF 时间线并设置 deadline。
 *
 * 业务背景：实体离队期间 V 会移动；重新加入时既要守恒服务债权，又不能让重实体拉动平均值导致溢出。
 * 入参：@cfs_rq 是 rq 锁保护输入输出队列；@se 是输入输出实体；@flags 是 ENQUEUE_* 场景位。
 * 出参/返回：无直接返回；更新 vruntime/deadline/rel_deadline，必要时平移 zero_vruntime。
 * 注意事项：PLACE_LAG/REL_DEADLINE/INITIAL 特性改变策略；所有权和 on_rq 状态由 enqueue_entity() 管理。
 */
static void
place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	u64 vslice, vruntime = avg_vruntime(cfs_rq);
	bool update_zero = false;
	s64 lag = 0;

	if (!se->custom_slice)
		se->slice = sysctl_sched_base_slice;
	vslice = calc_delta_fair(se->slice, se);

	/*
	 * Due to how V is constructed as the weighted average of entities,
	 * adding tasks with positive lag, or removing tasks with negative lag
	 * will move 'time' backwards, this can screw around with the lag of
	 * other tasks.
	 *
	 * EEVDF: placement strategy #1 / #2
	 */
	/* 正 lag 加入或负 lag 离开会让加权平均 V 后退并扰动他人；PLACE_LAG 用补偿后的虚拟 lag 放置。 */
	if (sched_feat(PLACE_LAG) && cfs_rq->nr_queued && se->vlag) {
		struct sched_entity *curr = cfs_rq->curr;
		long load, weight;

		lag = se->vlag;

		/*
		 * If we want to place a task and preserve lag, we have to
		 * consider the effect of the new entity on the weighted
		 * average and compensate for this, otherwise lag can quickly
		 * evaporate.
		 *
		 * Lag is defined as:
		 *
		 *   lag_i = S - s_i = w_i * (V - v_i)
		 *
		 * To avoid the 'w_i' term all over the place, we only track
		 * the virtual lag:
		 *
		 *   vl_i = V - v_i <=> v_i = V - vl_i
		 *
		 * And we take V to be the weighted average of all v:
		 *
		 *   V = (\Sum w_j*v_j) / W
		 *
		 * Where W is: \Sum w_j
		 *
		 * Then, the weighted average after adding an entity with lag
		 * vl_i is given by:
		 *
		 *   V' = (\Sum w_j*v_j + w_i*v_i) / (W + w_i)
		 *      = (W*V + w_i*(V - vl_i)) / (W + w_i)
		 *      = (W*V + w_i*V - w_i*vl_i) / (W + w_i)
		 *      = (V*(W + w_i) - w_i*vl_i) / (W + w_i)
		 *      = V - w_i*vl_i / (W + w_i)
		 *
		 * And the actual lag after adding an entity with vl_i is:
		 *
		 *   vl'_i = V' - v_i
		 *         = V - w_i*vl_i / (W + w_i) - (V - vl_i)
		 *         = vl_i - w_i*vl_i / (W + w_i)
		 *
		 * Which is strictly less than vl_i. So in order to preserve lag
		 * we should inflate the lag before placement such that the
		 * effective lag after placement comes out right.
		 *
		 * As such, invert the above relation for vl'_i to get the vl_i
		 * we need to use such that the lag after placement is the lag
		 * we computed before dequeue.
		 *
		 *   vl'_i = vl_i - w_i*vl_i / (W + w_i)
		 *         = ((W + w_i)*vl_i - w_i*vl_i) / (W + w_i)
		 *
		 *   (W + w_i)*vl'_i = (W + w_i)*vl_i - w_i*vl_i
		 *                   = W*vl_i
		 *
		 *   vl_i = (W + w_i)*vl'_i / W
		 */
		/*
		 * 新实体自身会把 V 拉向 v_i，使入队后的有效 lag 比保存值小；由加权平均公式反解，
		 * 入队前需把 lag 放大为 (W+w_i)/W，才能让入队后恢复原 vl'。
		 */
		load = cfs_rq->sum_weight;
		if (curr && curr->on_rq)
			load += avg_vruntime_weight(cfs_rq, curr->load.weight);

		weight = avg_vruntime_weight(cfs_rq, se->load.weight);
		lag *= load + weight;
		if (WARN_ON_ONCE(!load))
			load = 1;
		lag = div64_long(lag, load);

		/*
		 * A heavy entity (relative to the tree) will pull the
		 * avg_vruntime close to its vruntime position on enqueue. But
		 * the zero_vruntime point is only updated at the next
		 * update_deadline()/place_entity()/update_entity_lag().
		 *
		 * Specifically (see the comment near avg_vruntime_weight()):
		 *
		 *   sum_w_vruntime = \Sum (v_i - v0) * w_i
		 *
		 * Note that if v0 is near a light entity, both terms will be
		 * small for the light entity, while in that case both terms
		 * are large for the heavy entity, leading to risk of
		 * overflow.
		 *
		 * OTOH if v0 is near the heavy entity, then the difference is
		 * larger for the light entity, but the factor is small, while
		 * for the heavy entity the difference is small but the factor
		 * is large. Avoiding the multiplication overflow.
		 */
		/* 重实体权重大于现有总权重时，把 zero_vruntime 向其位置移动，让“大差值×大权重”变成小差值×大权重，降低溢出风险。 */
		if (weight > load)
			update_zero = true;
	}

	se->vruntime = vruntime - lag;

	if (update_zero)
		update_zero_vruntime(cfs_rq, -lag);

	if (sched_feat(PLACE_REL_DEADLINE) && se->rel_deadline) {
		/* 迁移保存的是相对 deadline，入新时间线时加 vruntime 还原绝对值并消费标志。 */
		se->deadline += se->vruntime;
		se->rel_deadline = 0;
		return;
	}

	/*
	 * When joining the competition; the existing tasks will be,
	 * on average, halfway through their slice, as such start tasks
	 * off with half a slice to ease into the competition.
	 */
	/* 初生实体假设竞争者平均已执行半个 slice，自己用半 slice deadline 温和加入而非抢占完整请求。 */
	if (sched_feat(PLACE_DEADLINE_INITIAL) && (flags & ENQUEUE_INITIAL))
		vslice /= 2;

	/*
	 * EEVDF: vd_i = ve_i + r_i/w_i
	 */
	/* 最终 virtual deadline 等于放置 vruntime 加权重折算后的请求时长。 */
	se->deadline = se->vruntime + vslice;
}

static void check_enqueue_throttle(struct cfs_rq *cfs_rq);
static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq);

static void
requeue_delayed_entity(struct sched_entity *se);

/* 依次更新时间、负载、lag/deadline 和树链接；throttle 祖先不向 CPU 发布 runnable。 */
/*
 * enqueue_entity() - 将一个 sched_entity 完整发布到 cfs_rq 的 EEVDF 可运行集合。
 *
 * 业务背景：task/group 入队需按顺序结算旧 curr、同步 PELT、更新组 shares、放置时间键、
 * 维护统计并最终设置 on_rq；首实体还连接叶队列和恢复 throttle 后 PELT 时钟。
 * 入参：@cfs_rq 是 rq 锁保护输入输出队列；@se 是待入队实体；@flags 为 ENQUEUE_* 位图。
 * 出参/返回：无直接返回；实体对树、load、PELT、层级链和策略计数变为可见。
 * 注意事项：on_rq 是发布结果；curr 不在树中需特殊处理，函数不能睡眠且必须与 dequeue 对称。
 */
static void
enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	bool curr = cfs_rq->curr == se;

	/*
	 * If we're the current task, we must renormalise before calling
	 * update_curr().
	 */
	/* curr 形式化重入队时先按当前 V 归一化，否则 update_curr 推进 V 后保存 lag 口径会错位。 */
	if (curr)
		place_entity(cfs_rq, se, flags);

	update_curr(cfs_rq);

	/*
	 * When enqueuing a sched_entity, we must:
	 *   - Update loads to have both entity and cfs_rq synced with now.
	 *   - For group_entity, update its runnable_weight to reflect the new
	 *     h_nr_runnable of its group cfs_rq.
	 *   - For group_entity, update its weight to reflect the new share of
	 *     its group cfs_rq
	 *   - Add its new weight to cfs_rq->load.weight
	 */
	/* 同步实体/队列到 now，刷新组 runnable/shares，再把最终权重加入队列总量。 */
	update_load_avg(cfs_rq, se, UPDATE_TG | DO_ATTACH);
	se_update_runnable(se);
	/*
	 * XXX update_load_avg() above will have attached us to the pelt sum;
	 * but update_cfs_group() here will re-adjust the weight and have to
	 * undo/redo all that. Seems wasteful.
	 */
	/* 当前顺序会先 attach 旧权重再因组 shares 重加权而撤销/加回，语义正确但存在可优化重复工作。 */
	update_cfs_group(se);

	/*
	 * XXX now that the entity has been re-weighted, and it's lag adjusted,
	 * we can place the entity.
	 */
	/* 权重最终确定后才计算 placement；curr 已在开头处理，普通实体在此放置。 */
	if (!curr)
		place_entity(cfs_rq, se, flags);

	account_entity_enqueue(cfs_rq, se);

	/* Entity has migrated, no longer consider this task hot */
	/* 迁移后清 exec_start，避免目标 CPU 把源 CPU 的旧时间戳当作连续热运行段。 */
	if (flags & ENQUEUE_MIGRATED)
		se->exec_start = 0;

	check_schedstat_required();
	update_stats_enqueue_fair(cfs_rq, se, flags);
	if (!curr)
		__enqueue_entity(cfs_rq, se);
	se->on_rq = 1;

	if (cfs_rq->nr_queued == 1) {
		/* 从空变非空是层级发布边界：检查带宽 throttle 并把叶队列连接到 CPU 遍历链。 */
		check_enqueue_throttle(cfs_rq);
		list_add_leaf_cfs_rq(cfs_rq);
#ifdef CONFIG_CFS_BANDWIDTH
		if (cfs_rq->pelt_clock_throttled) {
			/* 恢复此前因 throttle 暂停的 PELT clock，累计暂停时间后清停钟标志。 */
			struct rq *rq = rq_of(cfs_rq);

			cfs_rq->throttled_clock_pelt_time += rq_clock_pelt(rq) -
				cfs_rq->throttled_clock_pelt;
			cfs_rq->pelt_clock_throttled = 0;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		}
#endif
	}
}

/*
 * __clear_buddies_next() - 沿调度组祖先清除连续指向实体链的 next buddy。
 *
 * 业务背景：wakeup buddy 可跨层设置；实体离队/运行后旧偏好必须在每个对应 cfs_rq 撤销。
 * 入参：@se 是借用实体，调用者持 rq 锁并稳定父链。
 * 出参/返回：无直接返回；逐层把匹配的 cfs_rq->next 置 NULL，首个不匹配即停止。
 * 注意事项：只清与该实体祖先链精确配对的提示，不影响其他 buddy。
 */
static void __clear_buddies_next(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->next != se)
			break;

		cfs_rq->next = NULL;
	}
}

/*
 * clear_buddies() - 若当前层 next buddy 指向实体，则清除其整条祖先 buddy 链。
 *
 * 业务背景：enqueue/dequeue/set_next 共用轻量门禁，避免无匹配时遍历层级。
 * 入参：@cfs_rq 是当前层借用队列；@se 是借用实体。
 * 出参/返回：无直接返回；匹配时产生 __clear_buddies_next() 副作用。
 * 注意事项：buddy 只影响短期延迟，不改变 EEVDF 公平统计。
 */
static void clear_buddies(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (cfs_rq->next == se)
		__clear_buddies_next(se);
}

static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq);

/*
 * set_delayed() - 标记负 lag 睡眠实体为延迟出队，并调整 task 层级 runnable 计数。
 *
 * 业务背景：DELAY_DEQUEUE 让欠服务 task 暂留 EEVDF 树偿还债务，减少睡眠导致的 lag 丢失。
 * 入参：@se 是 rq 锁下输入输出实体。
 * 出参/返回：无直接返回；置 sched_delayed，task 实体沿祖先递减 h_nr_runnable。
 * 注意事项：组实体下已无真实 task，dequeue_entities() 会处理 blocked 计数，故不重复调整。
 */
static void set_delayed(struct sched_entity *se)
{
	se->sched_delayed = 1;

	/*
	 * Delayed se of cfs_rq have no tasks queued on them.
	 * Do not adjust h_nr_runnable since dequeue_entities()
	 * will account it for blocked tasks.
	 */
	/* 组实体代表的子队列已无 task，blocked 层级计数由外层 dequeue 统一撤销。 */
	if (!entity_is_task(se))
		return;

	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);

		cfs_rq->h_nr_runnable--;
	}
}

/*
 * clear_delayed() - 结束实体延迟出队状态并恢复 task 层级 runnable 计数。
 *
 * 业务背景：真正 delayed dequeue 或下层新 task 重新入队后，旧延迟标记不能继续影响选择。
 * 入参：@se 是 rq 锁下输入输出实体。
 * 出参/返回：无直接返回；清 sched_delayed，task 实体沿祖先增加 h_nr_runnable。
 * 注意事项：组实体计数已由真实 dequeue/enqueue 路径处理，避免双加。
 */
static void clear_delayed(struct sched_entity *se)
{
	se->sched_delayed = 0;

	/*
	 * Delayed se of cfs_rq have no tasks queued on them.
	 * Do not adjust h_nr_runnable since a dequeue has
	 * already accounted for it or an enqueue of a task
	 * below it will account for it in enqueue_task_fair().
	 */
	/* 组实体对应层级计数已在先前 dequeue 或下层 enqueue 中处理，本函数不重复恢复。 */
	if (!entity_is_task(se))
		return;

	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);

		cfs_rq->h_nr_runnable++;
	}
}

/* 睡眠可延迟 dequeue 以偿还负 lag；真正摘除时保存 lag 并沿层级撤销负载。 */
/*
 * dequeue_entity() - 从 cfs_rq 撤销实体，或把欠服务睡眠实体转换为 delayed 状态。
 *
 * 业务背景：睡眠、迁移、throttle 和选人共用出队入口；普通睡眠可暂留树中偿还负 lag，
 * 特殊状态则必须立即摘除，最终路径同步 PELT、保存相对时间并维护层级/带宽时钟。
 * 入参：@cfs_rq 是 rq 锁保护输入输出队列；@se 是输入输出实体；@flags 为 DEQUEUE_* 位图。
 * 出参/返回：真正摘除返回 true；转换为 delayed 返回 false；副作用覆盖树、统计、PELT 和叶链。
 * 注意事项：DEQUEUE_DELAYED 只用于已标记实体；迁移需 DO_DETACH，sleep 才在适当时更新 util_est。
 */
static bool
dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	bool sleep = flags & DEQUEUE_SLEEP;
	int action = 0;

	update_curr(cfs_rq);
	clear_buddies(cfs_rq, se);

	if (flags & DEQUEUE_DELAYED) {
		/* delayed 实体最终摘除必须已带标记；否则调用协议失配。 */
		WARN_ON_ONCE(!se->sched_delayed);
	} else {
		bool delay = sleep;
		/*
		 * DELAY_DEQUEUE relies on spurious wakeups, special task
		 * states must not suffer spurious wakeups, excempt them.
		 */
		/* 延迟出队依赖可容忍的伪唤醒；特殊状态和 throttle 不能被额外唤醒，强制立即摘除。 */
		if (flags & (DEQUEUE_SPECIAL | DEQUEUE_THROTTLE))
			delay = false;

		WARN_ON_ONCE(delay && se->sched_delayed);

		if (sched_feat(DELAY_DEQUEUE) && delay &&
		    !entity_eligible(cfs_rq, se)) {
			/* 欠服务且普通睡眠：先更新估值/PELT并保存 lag，再标 delayed 留树，返回 false。 */
			if (entity_is_task(se))
				action |= UPDATE_UTIL_EST;
			update_load_avg(cfs_rq, se, action);
			update_entity_lag(cfs_rq, se);
			set_delayed(se);
			return false;
		}
	}

	action = UPDATE_TG;
	/* 真正出队默认传播 task_group；迁移额外 detach，正常睡眠在非 delayed 最终出口更新 util_est。 */
	if (entity_is_task(se)) {
		if (task_on_rq_migrating(task_of(se)))
			action |= DO_DETACH;

		if (sleep && !(flags & DEQUEUE_DELAYED))
			action |= UPDATE_UTIL_EST;
	}

	/*
	 * When dequeuing a sched_entity, we must:
	 *   - Update loads to have both entity and cfs_rq synced with now.
	 *   - For group_entity, update its runnable_weight to reflect the new
	 *     h_nr_runnable of its group cfs_rq.
	 *   - Subtract its previous weight from cfs_rq->load.weight.
	 *   - For group entity, update its weight to reflect the new share
	 *     of its group cfs_rq.
	 */
	/* 同步实体/队列到 now，刷新组 runnable，撤旧权重并按子队列新状态重算组 shares。 */
	update_load_avg(cfs_rq, se, action);
	se_update_runnable(se);

	update_stats_dequeue_fair(cfs_rq, se, flags);

	update_entity_lag(cfs_rq, se);
	/* 摘树前保存受界 vlag；非睡眠迁移可把 deadline 转为相对 vruntime 以跨队列恢复。 */
	if (sched_feat(PLACE_REL_DEADLINE) && !sleep) {
		se->deadline -= se->vruntime;
		se->rel_deadline = 1;
	}

	if (se != cfs_rq->curr)
		__dequeue_entity(cfs_rq, se);
	se->on_rq = 0;
	account_entity_dequeue(cfs_rq, se);

	/* return excess runtime on last dequeue */
	/* 每次出队检查并归还不再需要的局部 CFS bandwidth 余额，最后实体尤其重要。 */
	return_cfs_rq_runtime(cfs_rq);

	update_cfs_group(se);

	if (flags & DEQUEUE_DELAYED)
		/* 真正完成 delayed 摘除后恢复层级 runnable 计数/清标志。 */
		clear_delayed(se);

	if (cfs_rq->nr_queued == 0) {
		/* 从非空变空时保存 idle PELT clock；若祖先 throttle，则摘叶链并开始停钟区间。 */
		update_idle_cfs_rq_clock_pelt(cfs_rq);
#ifdef CONFIG_CFS_BANDWIDTH
		if (throttled_hierarchy(cfs_rq)) {
			struct rq *rq = rq_of(cfs_rq);

			list_del_leaf_cfs_rq(cfs_rq);
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
			cfs_rq->throttled_clock_pelt = rq_clock_pelt(rq);
			cfs_rq->pelt_clock_throttled = 1;
		}
#endif
	}

	return true;
}

/* 选中实体从树取出成为 curr，但仍保留在 cfs_rq 的负载与 runnable 统计中。 */
/*
 * set_next_entity() - 将 EEVDF 候选从树中取出并发布为 cfs_rq 当前运行实体。
 *
 * 业务背景：调度选择完成后，curr 不再留在树中以免重复选择，但仍属于 on_rq 可运行集合。
 * 入参：@cfs_rq 是锁定输入输出队列；@se 是候选实体；@first 表示本轮首次选择并建立保护窗。
 * 出参/返回：无直接返回；结束等待统计、摘树、设置 exec_start/curr/vprot 和 slice_max 基线。
 * 注意事项：调用前 curr 必须为空；实体生命周期由 rq 锁稳定，函数不改变 on_rq 或队列总负载。
 */
static void
set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, bool first)
{
	clear_buddies(cfs_rq, se);

	/* 'current' is not kept within the tree. */
	/* 已排队候选结束等待、从 EEVDF 树摘除并刷新 PELT；on_rq 保持 1。 */
	if (se->on_rq) {
		/*
		 * Any task has to be enqueued before it get to execute on
		 * a CPU. So account for the time it spent waiting on the
		 * runqueue.
		 */
		/* 所有 task 必先入队才运行，因此这里总能结算从 enqueue 到选中的等待区间。 */
		update_stats_wait_end_fair(cfs_rq, se);
		__dequeue_entity(cfs_rq, se);
		update_load_avg(cfs_rq, se, UPDATE_TG);

		if (first)
			set_protect_slice(cfs_rq, se);
	}

	update_stats_curr_start(cfs_rq, se);
	WARN_ON_ONCE(cfs_rq->curr);
	cfs_rq->curr = se;

	/*
	 * Track our maximum slice length, if the CPU's load is at
	 * least twice that of our own weight (i.e. don't track it
	 * when there are only lesser-weight tasks around):
	 */
	/* 仅竞争总权重至少为自身两倍时记录 slice_max，避免把几乎独占 CPU 的长运行误作调度延迟。 */
	if (schedstat_enabled() &&
	    rq_of(cfs_rq)->cfs.load.weight >= 2*se->load.weight) {
		struct sched_statistics *stats;

		stats = __schedstats_from_se(se);
		__schedstat_set(stats->slice_max,
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
				max((u64)stats->slice_max,
				    se->sum_exec_runtime - se->prev_sum_exec_runtime));
	}

	se->prev_sum_exec_runtime = se->sum_exec_runtime;
}

static int dequeue_entities(struct rq *rq, struct sched_entity *se, int flags);

/*
 * Pick the next process, keeping these things in mind, in this order:
 * 1) keep things fair between processes/task groups
 * 2) pick the "next" process, since someone really wants that to run
 * 3) pick the "last" process, for cache locality
 * 4) do not run the "skip" process, if something else is available
 */
/* 选择优先保持组/进程公平，再考虑 next/last/skip 延迟与缓存提示；当前实现核心由 EEVDF 完成。 */
/* EEVDF 结果若是 delayed 实体则完成最终阻塞并重选，之后不可再引用该实体。 */
/*
 * pick_next_entity() - 选择本层下一 EEVDF 实体并处理 delayed dequeue 候选。
 *
 * 业务背景：pick_task_fair() 逐层调用；欠服务睡眠实体可能仍在树中，选到时才完成真正阻塞。
 * 入参：@rq/@cfs_rq 是锁定借用队列；@protect 控制保护窗口/buddy。
 * 出参/返回：返回借用可运行实体；处理 delayed 后返回 NULL 要求上层重新调度。
 * 注意事项：dequeue_entities() 可能使 @se 生命周期结束，调用后绝不可再次引用。
 */
static struct sched_entity *
pick_next_entity(struct rq *rq, struct cfs_rq *cfs_rq, bool protect)
{
	struct sched_entity *se;

	se = pick_eevdf(cfs_rq, protect);
	if (se->sched_delayed) {
		dequeue_entities(rq, se, DEQUEUE_SLEEP | DEQUEUE_DELAYED);
		/*
		 * Must not reference @se again, see __block_task().
		 */
		/* __block_task() 可发布阻塞并释放相关引用；返回 NULL 是唯一安全后续。 */
		return NULL;
	}
	return se;
}

/*
 * put_prev_entity() - 结算并把仍 runnable 的前一 curr 放回 EEVDF 树。
 *
 * 业务背景：上下文切换离开 prev 时，若未 deactivate，它仍竞争 CPU，需恢复等待统计与树可见性。
 * 入参：@cfs_rq 是锁定输入输出队列；@prev 必须等于当前 cfs_rq->curr。
 * 出参/返回：无直接返回；可能更新 runtime/PELT、重入树，最终清 cfs_rq->curr。
 * 注意事项：off-rq prev 已在 dequeue 更新，不得重复；函数不清 prev->on_rq。
 */
static void put_prev_entity(struct cfs_rq *cfs_rq, struct sched_entity *prev)
{
	/*
	 * If still on the runqueue then deactivate_task()
	 * was not called and update_curr() has to be done:
	 */
	/* on_rq 表明没有走 deactivate，先结算离开 CPU 前最后一段执行。 */
	if (prev->on_rq)
		update_curr(cfs_rq);

	if (prev->on_rq) {
		update_stats_wait_start_fair(cfs_rq, prev);
		/* Put 'current' back into the tree. */
		/* 从 curr 状态恢复为等待候选，重新插入 deadline 树。 */
		__enqueue_entity(cfs_rq, prev);
		/* in !on_rq case, update occurred at dequeue */
		/* off-rq 的负载已由 dequeue 更新；仅 runnable prev 需在此刷新。 */
		update_load_avg(cfs_rq, prev, 0);
	}
	WARN_ON_ONCE(cfs_rq->curr != prev);
	cfs_rq->curr = NULL;
}

/*
 * entity_tick() - 在调度 tick 更新当前实体执行/PELT/组权重并处理 hrtick 到期。
 *
 * 业务背景：周期 tick 保持 EEVDF 服务与容量统计前进；高精度排队 tick 直接对应 slice 边界。
 * 入参：@cfs_rq 是锁定输入输出队列；@curr 是当前实体；@queued 表示 hrtick 排队到期。
 * 出参/返回：无直接返回；推进统计/组 shares，queued 时设置立即 resched。
 * 注意事项：普通 tick 的 deadline/protect 判断已在 update_curr() 内完成；函数不可睡眠。
 */
static void
entity_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr, int queued)
{
	/*
	 * Update run-time statistics of the 'current'.
	 */
	/* 先结算执行，确保后续 PELT 和 deadline 使用同一 now。 */
	update_curr(cfs_rq);

	/*
	 * Ensure that runnable average is periodically updated.
	 */
	/* 即使没有入出队，tick 也推进 runnable 衰减并刷新组实体 shares。 */
	update_load_avg(cfs_rq, curr, UPDATE_TG);
	update_cfs_group(curr);

#ifdef CONFIG_SCHED_HRTICK
	/*
	 * queued ticks are scheduled to match the slice, so don't bother
	 * validating it and just reschedule.
	 */
	/* hrtick 已精确排到 slice 结束，无需再次比较，直接请求调度。 */
	if (queued) {
		resched_curr(rq_of(cfs_rq));
		return;
	}
#endif
}


/**************************************************
 * CFS bandwidth control machinery
 */
/* CFS 带宽控制把 task_group quota 周期性分片到各 CPU cfs_rq，耗尽时 throttle 整个层级。 */

#ifdef CONFIG_CFS_BANDWIDTH

#ifdef CONFIG_JUMP_LABEL
static struct static_key __cfs_bandwidth_used;

/*
 * cfs_bandwidth_used() - 通过静态键判断系统是否有启用 quota 的 CFS 组。
 *
 * 业务背景：绝大多数热路径无需带宽控制时应被 jump label 消除分支成本。
 * 入参：无。
 * 出参/返回：静态键启用返回 true，否则 false；无副作用。
 * 注意事项：增减只能在 CPU 集合锁约束下走慢路径修改代码跳转。
 */
static inline bool cfs_bandwidth_used(void)
{
	return static_key_false(&__cfs_bandwidth_used);
}

/* 增加启用 bandwidth 的组计数，首次使用时修补静态分支；调用者持 CPU 锁。 */
void cfs_bandwidth_usage_inc(void)
{
	static_key_slow_inc_cpuslocked(&__cfs_bandwidth_used);
}

/* 减少使用计数，最后一个 quota 组消失时关闭热路径静态分支。 */
void cfs_bandwidth_usage_dec(void)
{
	static_key_slow_dec_cpuslocked(&__cfs_bandwidth_used);
}
#else /* !CONFIG_JUMP_LABEL: */
/* 无 jump label 时无法动态消除分支，保守恒报 bandwidth 可能使用。 */
static bool cfs_bandwidth_used(void)
{
	return true;
}

/* 无 jump label 配置不需要维护静态键引用。 */
void cfs_bandwidth_usage_inc(void) {}
/* 无 jump label 配置不需要维护静态键引用。 */
void cfs_bandwidth_usage_dec(void) {}
#endif /* !CONFIG_JUMP_LABEL */

/*
 * sched_cfs_bandwidth_slice() - 把 sysctl 微秒配额切片转换为内部纳秒。
 *
 * 业务背景：全局 runtime 池按 slice 分配给 per-CPU cfs_rq，统一使用调度时钟纳秒单位。
 * 入参：无；读取 sysctl_sched_cfs_bandwidth_slice。
 * 出参/返回：返回纳秒切片；无副作用。
 * 注意事项：sysctl 范围由注册层校验，读取是瞬时调优快照。
 */
static inline u64 sched_cfs_bandwidth_slice(void)
{
	return (u64)sysctl_sched_cfs_bandwidth_slice * NSEC_PER_USEC;
}

/*
 * Replenish runtime according to assigned quota. We use sched_clock_cpu
 * directly instead of rq->clock to avoid adding additional synchronization
 * around rq->lock.
 *
 * requires cfs_b->lock
 */
/*
 * __refill_cfs_bandwidth_runtime() - 新周期把 quota 补入组全局 runtime 池并统计 burst 使用。
 *
 * 业务背景：hrtimer 周期边界调用；未用完/借用 burst 的余额需限制在 quota+burst 范围。
 * 入参：@cfs_b 是调用者持 lock 的输入输出带宽池。
 * 出参/返回：无直接返回；增加 runtime、更新 burst_time/nr_burst、runtime_snap 并封顶。
 * 注意事项：无限 quota 空操作；直接读 sched_clock 的设计避免额外 rq 锁同步。
 */
void __refill_cfs_bandwidth_runtime(struct cfs_bandwidth *cfs_b)
{
	s64 runtime;

	if (unlikely(cfs_b->quota == RUNTIME_INF))
		return;

	cfs_b->runtime += cfs_b->quota;
	/* runtime_snap 与补充后余额之差为上周期实际超出 quota、由 burst 覆盖的时间。 */
	runtime = cfs_b->runtime_snap - cfs_b->runtime;
	if (runtime > 0) {
		cfs_b->burst_time += runtime;
		cfs_b->nr_burst++;
	}

	cfs_b->runtime = min(cfs_b->runtime, cfs_b->quota + cfs_b->burst);
	/* 池余额最多积累 quota+burst，快照作为下一周期 burst 消耗基线。 */
	cfs_b->runtime_snap = cfs_b->runtime;
}

/* 返回 task_group 内嵌带宽池的借用指针，生命周期随组本身。 */
static inline struct cfs_bandwidth *tg_cfs_bandwidth(struct task_group *tg)
{
	return &tg->cfs_bandwidth;
}

/* returns 0 on failure to allocate runtime */
/* 无法给本地 cfs_rq 分配正 runtime 时返回 0。 */
/* 在 bandwidth 锁下从组全局池切片给本地 rq；只发放实际剩余量。 */
/*
 * __assign_cfs_rq_runtime() - 从 task_group 全局池向本 CPU cfs_rq 发放目标 runtime。
 *
 * 业务背景：本地余额耗尽时按切片补充，减少每次执行都争用全局 cfs_b->lock。
 * 入参：@cfs_b 是已锁全局池；@cfs_rq 是输入输出本地队列；@target_runtime 为期望正余额纳秒。
 * 出参/返回：分配后本地余额>0 返回 1，否则 0；同步扣全局池并清 idle。
 * 注意事项：runtime_remaining<=0 时 min_amount 为正；无限 quota 可直接满足而不启动 period timer。
 */
static int __assign_cfs_rq_runtime(struct cfs_bandwidth *cfs_b,
				   struct cfs_rq *cfs_rq, u64 target_runtime)
{
	u64 min_amount, amount = 0;

	lockdep_assert_held(&cfs_b->lock);

	/* note: this is a positive sum as runtime_remaining <= 0 */
	/* target 减非正余额得到必须补足的正量。 */
	min_amount = target_runtime - cfs_rq->runtime_remaining;

	if (cfs_b->quota == RUNTIME_INF)
		amount = min_amount;
	else {
		start_cfs_bandwidth(cfs_b);

		if (cfs_b->runtime > 0) {
			amount = min(cfs_b->runtime, min_amount);
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
			cfs_b->runtime -= amount;
			cfs_b->idle = 0;
		}
	}

	cfs_rq->runtime_remaining += amount;

	return cfs_rq->runtime_remaining > 0;
}

static bool throttle_cfs_rq(struct cfs_rq *cfs_rq);

/*
 * __account_cfs_rq_runtime() - 从本地 CFS 余额扣除执行量并在耗尽时尝试 throttle。
 *
 * 业务背景：update_curr() 每段执行结算调用，跨 period 的 delta 必须先完整记账再判断配额。
 * 入参：@cfs_rq 是 rq 锁保护输入输出队列；@delta_exec 是实际执行纳秒。
 * 出参/返回：仍有余额返回 false；已/新 throttle 返回 true。
 * 注意事项：throttle_cfs_rq() 会先尝试从全局池续配，只有失败才冻结层级。
 */
static bool __account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec)
{
	/* dock delta_exec before expiring quota (as it could span periods) */
	/* delta 可能跨越周期，必须先全部扣除，不能在零边界截断实际 CPU 使用。 */
	cfs_rq->runtime_remaining -= delta_exec;

	if (likely(cfs_rq->runtime_remaining > 0))
		return false;

	if (cfs_rq->throttled)
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
		return true;
	/*
	 * throttle_cfs_rq() will try to extend the runtime first
	 * before throttling the hierarchy.
	 */
	/* 到零后先向全局池申请续片，真正无可用配额才进入 throttle 状态。 */
	return throttle_cfs_rq(cfs_rq);
}

/*
 * account_cfs_rq_runtime() - 带静态键和 per-rq 开关的 CFS runtime 热路径包装。
 *
 * 业务背景：无 quota 的系统/队列应跳过所有扣费成本。
 * 入参：@cfs_rq 是输入输出队列；@delta_exec 是纳秒执行量。
 * 出参/返回：功能关闭返回 false，否则转发真实 throttle 结果。
 * 注意事项：调用者持 rq 锁，不能把 false 理解为余额一定充足，可能只是控制未启用。
 */
static __always_inline
bool account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec)
{
	if (!cfs_bandwidth_used() || !cfs_rq->runtime_enabled)
		return false;

	return __account_cfs_rq_runtime(cfs_rq, delta_exec);
}

/*
 * cfs_rq_throttled() - 判断单个 CFS 队列是否处于配额节流状态。
 *
 * 业务背景：调度热路径只有在全局启用带宽控制时才需要读取队列的 throttled 位。
 * 入参：@cfs_rq 是只读查询的队列。
 * 出参/返回：带宽控制已启用且本队列被节流时返回非零。
 * 注意事项：这里只看本层；判断祖先层级应使用 throttled_hierarchy()。
 */
static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq)
{
	return cfs_bandwidth_used() && cfs_rq->throttled;
}

/*
 * cfs_rq_pelt_clock_throttled() - 判断本队列的 PELT 时钟是否因节流暂停。
 *
 * 业务背景：节流期间不能让不可运行时间错误衰减或累积负载信号。
 * 入参：@cfs_rq 是只读查询的队列。
 * 出参/返回：静态键打开且 PELT 时钟暂停时返回 true。
 * 注意事项：该状态与任务是否位于 limbo 列表不是同一个概念。
 */
static inline bool cfs_rq_pelt_clock_throttled(struct cfs_rq *cfs_rq)
{
	return cfs_bandwidth_used() && cfs_rq->pelt_clock_throttled;
}

/* check whether cfs_rq, or any parent, is throttled */
/* 检查 cfs_rq 本身或任一父层是否被节流。 */
/*
 * throttled_hierarchy() - 查询队列所在调度组路径的节流嵌套计数。
 *
 * 业务背景：子组自身尚有配额，也不能穿过已经冻结的父组获得 CPU。
 * 入参：@cfs_rq 是待查询队列。
 * 出参/返回：@throttle_count 非零表示本层或祖先至少有一层被节流。
 * 注意事项：计数而非布尔值用于正确处理嵌套组的成对下压和恢复。
 */
static inline int throttled_hierarchy(struct cfs_rq *cfs_rq)
{
	return cfs_bandwidth_used() && cfs_rq->throttle_count;
}

/*
 * lb_throttled_hierarchy() - 在目标 CPU 上判断任务所属组是否被节流。
 *
 * 业务背景：负载均衡不能把任务迁往一个当前无法运行其组实体的层级。
 * 入参：@p 提供任务组；@dst_cpu 指定待考察的目标 CPU。
 * 出参/返回：目标 CPU 对应 cfs_rq 的层级节流结果。
 * 注意事项：查询的是目标 CPU 的 per-CPU 组队列，不是任务当前队列。
 */
static inline int lb_throttled_hierarchy(struct task_struct *p, int dst_cpu)
{
	return throttled_hierarchy(tg_cfs_rq(task_group(p), dst_cpu));
}

/*
 * task_is_throttled() - 判断任务是否已被移出正常 CFS 队列等待解限。
 *
 * 业务背景：延迟 task_work 会把当前任务放入 throttled_limbo_list。
 * 入参：@p 是只读查询的任务。
 * 出参/返回：带宽控制启用且任务的 @throttled 位已置位时返回 true。
 * 注意事项：任务位与 cfs_rq 层级位分别维护，不能相互替代。
 */
static inline bool task_is_throttled(struct task_struct *p)
{
	return cfs_bandwidth_used() && p->throttled;
}

static bool dequeue_task_fair(struct rq *rq, struct task_struct *p, int flags);
/*
 * throttle_cfs_rq_work() - 在当前任务返回用户态前完成实际出队和 limbo 挂接。
 *
 * 业务背景：节流可能发生在 update_curr() 的敏感上下文，先登记 task_work，随后在
 * 可获取 task_rq_lock 的安全点把任务从公平类运行队列撤下。
 * 入参：@work 内嵌于当前任务的 @sched_throttle_work。
 * 出参/返回：无；成功时任务从 CFS 出队、加入队列 limbo 并标记 throttled。
 * 注意事项：退出任务、公平类已变化、或竞态中层级已恢复都会直接取消本次处理。
 */
static void throttle_cfs_rq_work(struct callback_head *work)
{
	struct task_struct *p = container_of(work, struct task_struct, sched_throttle_work);
	struct sched_entity *se;
	struct cfs_rq *cfs_rq;
	struct rq *rq;

	WARN_ON_ONCE(p != current);
	p->sched_throttle_work.next = &p->sched_throttle_work;

	/*
	 * If task is exiting, then there won't be a return to userspace, so we
	 * don't have to bother with any of this.
	 */
	/* 退出路径不会再次返回用户态运行，无需建立等待恢复的 limbo 状态。 */
	if ((p->flags & PF_EXITING))
		return;

	scoped_guard(task_rq_lock, p) {
		se = &p->se;
		cfs_rq = cfs_rq_of(se);

		/* Raced, forget */
		/* 与调度类变更竞争失败：任务已不再由 CFS 管理，放弃旧工作。 */
		if (p->sched_class != &fair_sched_class)
			return;

		/*
		 * If not in limbo, then either replenish has happened or this
		 * task got migrated out of the throttled cfs_rq, move along.
		 */
		/* 计数归零说明配额已补充，或任务已迁出原节流层级，无需再出队。 */
		if (!cfs_rq->throttle_count)
			return;
		rq = scope.rq;
		update_rq_clock(rq);
		WARN_ON_ONCE(p->throttled || !list_empty(&p->throttle_node));
		dequeue_task_fair(rq, p, DEQUEUE_SLEEP | DEQUEUE_THROTTLE);
		list_add(&p->throttle_node, &cfs_rq->throttled_limbo_list);
		/*
		 * Must not set throttled before dequeue or dequeue will
		 * mistakenly regard this task as an already throttled one.
		 */
		/* 状态位必须最后发布，否则 dequeue 会误走“已节流任务”的特殊分支。 */
		p->throttled = true;
		resched_curr(rq);
	}
}

/*
 * init_cfs_throttle_work() - 初始化任务的延迟节流工作和 limbo 链表节点。
 *
 * 业务背景：任务创建时预置可复用 callback，避免运行时动态分配。
 * 入参：@p 是待初始化任务。
 * 出参/返回：无；初始化 callback、自指哨兵和空链表节点。
 * 注意事项：自指的 next 同时承担“当前没有待执行工作”的状态标记。
 */
void init_cfs_throttle_work(struct task_struct *p)
{
	init_task_work(&p->sched_throttle_work, throttle_cfs_rq_work);
	/* Protect against double add, see throttle_cfs_rq() and throttle_cfs_rq_work() */
	/* 用自指哨兵防止重复加入；对应 throttle_cfs_rq() 与回调中的状态协议。 */
	p->sched_throttle_work.next = &p->sched_throttle_work;
	INIT_LIST_HEAD(&p->throttle_node);
}

/*
 * Task is throttled and someone wants to dequeue it again:
 * it could be sched/core when core needs to do things like
 * task affinity change, task group change, task sched class
 * change etc. and in these cases, DEQUEUE_SLEEP is not set;
 * or the task is blocked after throttled due to freezer etc.
 * and in these cases, DEQUEUE_SLEEP is set.
 */
/*
 * 任务已节流后仍可能被 core 再次出队：亲和性、组或调度类变化不会带
 * DEQUEUE_SLEEP；freezer 等阻塞路径则会携带该标志。
 */
static void detach_task_cfs_rq(struct task_struct *p);
/*
 * dequeue_throttled_task() - 从 limbo 状态撤下任务并按原因清理负载归属。
 *
 * 业务背景：core 在任务等待配额期间仍可执行阻塞或迁移类操作。
 * 入参：@p 是已节流任务；@flags 描述本次出队原因。
 * 出参/返回：无；移除 limbo 节点，阻塞时清除标志，迁移时分离旧队列负载。
 * 注意事项：调用时实体不应在 rb 运行队列；非睡眠普通变更保留 throttled 位。
 */
static void dequeue_throttled_task(struct task_struct *p, int flags)
{
	WARN_ON_ONCE(p->se.on_rq);
	list_del_init(&p->throttle_node);

	/* task blocked after throttled */
	/* 节流后又真正阻塞：不再等待配额恢复，解除任务级节流状态。 */
	if (flags & DEQUEUE_SLEEP) {
		p->throttled = false;
		return;
	}

	/*
	 * task is migrating off its old cfs_rq, detach
	 * the task's load from its old cfs_rq.
	 */
	/* 迁移会改变负载所有者，必须从旧 cfs_rq 的聚合信号中显式分离。 */
	if (task_on_rq_migrating(p))
		detach_task_cfs_rq(p);
}

/*
 * enqueue_throttled_task() - 为已节流任务选择 limbo 快路或正常入队。
 *
 * 业务背景：任务属性或组变化后 core 会重新入队，但目标层级可能仍被节流。
 * 入参：@p 是即将重新入队的任务。
 * 出参/返回：直接挂入目标 limbo 返回 true；需要正常 enqueue 返回 false。
 * 注意事项：当前 donor 任务必须走正常路径，以规避与 sched_move_task() 的竞态。
 */
static bool enqueue_throttled_task(struct task_struct *p)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(&p->se);

	/* @p should have gone through dequeue_throttled_task() first */
	/* 调用协议要求旧 limbo 节点已由 dequeue_throttled_task() 摘除。 */
	WARN_ON_ONCE(!list_empty(&p->throttle_node));

	/*
	 * If the throttled task @p is enqueued to a throttled cfs_rq,
	 * take the fast path by directly putting the task on the
	 * target cfs_rq's limbo list.
	 *
	 * Do not do that when @p is current because the following race can
	 * cause @p's group_node to be incorectly re-insterted in its rq's
	 * cfs_tasks list, despite being throttled:
	 *
	 *     cpuX                       cpuY
	 *   p ret2user
	 *  throttle_cfs_rq_work()  sched_move_task(p)
	 *  LOCK task_rq_lock
	 *  dequeue_task_fair(p)
	 *  UNLOCK task_rq_lock
	 *                          LOCK task_rq_lock
	 *                          task_current_donor(p) == true
	 *                          task_on_rq_queued(p) == true
	 *                          dequeue_task(p)
	 *                          put_prev_task(p)
	 *                          sched_change_group()
	 *                          enqueue_task(p) -> p's new cfs_rq
	 *                                             is throttled, go
	 *                                             fast path and skip
	 *                                             actual enqueue
	 *                          set_next_task(p)
	 *                    list_move(&se->group_node, &rq->cfs_tasks); // bug
	 *  schedule()
	 *
	 * In the above race case, @p current cfs_rq is in the same rq as
	 * its previous cfs_rq because sched_move_task() only moves a task
	 * to a different group from the same rq, so we can use its current
	 * cfs_rq to derive rq and test if the task is current.
	 */
	/*
	 * 快路直接挂 limbo，避免无意义入队；但 current 与同一 rq 内换组并发时，
	 * set_next_task() 仍可能重挂 group_node，因此 donor 必须拒绝快路。
	 */
	if (throttled_hierarchy(cfs_rq) &&
	    !task_current_donor(rq_of(cfs_rq), p)) {
		list_add(&p->throttle_node, &cfs_rq->throttled_limbo_list);
		return true;
	}

	/* we can't take the fast path, do an actual enqueue*/
	/* 无法安全走快路：清除旧标志，让调用者执行完整的公平类入队。 */
	p->throttled = false;
	return false;
}

static void enqueue_task_fair(struct rq *rq, struct task_struct *p, int flags);
/*
 * tg_unthrottle_up() - 自目标组向子层遍历，逐层解除节流并重入队 limbo 任务。
 *
 * 业务背景：配额补充后 walk_tg_tree_from() 用该回调恢复每个 per-CPU cfs_rq。
 * 入参：@tg 是当前遍历组；@data 指向持锁的目标 rq。
 * 出参/返回：始终返回 0；计数仍非零时仅完成一次嵌套解引用。
 * 注意事项：先 update_curr() 对齐时钟；重入队可能再次耗尽祖先配额，需中途停止。
 */
static int tg_unthrottle_up(struct task_group *tg, void *data)
{
	struct rq *rq = data;
	struct cfs_rq *cfs_rq = tg_cfs_rq(tg, cpu_of(rq));
	struct task_struct *p, *tmp;
	LIST_HEAD(throttled_tasks);

	/*
	 * If cfs_rq->curr is set, the cfs_rq might not have caught up
	 * since the last clock update. Do it now before we begin
	 * queueing task onto it to save the need for unnecessarily
	 * unthrottle the hierarchy for this cfs_rq to be throttled
	 * right back again.
	 */
	/* 先结算正在运行实体，避免刚恢复并入队便因遗漏消耗而再次节流。 */
	update_curr(cfs_rq);

	if (--cfs_rq->throttle_count)
		return 0;

	if (cfs_rq->pelt_clock_throttled) {
		cfs_rq->throttled_clock_pelt_time += rq_clock_pelt(rq) -
					     cfs_rq->throttled_clock_pelt;
		cfs_rq->pelt_clock_throttled = 0;
	}

	if (cfs_rq->throttled_clock_self) {
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		u64 delta = rq_clock(rq) - cfs_rq->throttled_clock_self;

		cfs_rq->throttled_clock_self = 0;

		if (WARN_ON_ONCE((s64)delta < 0))
			delta = 0;

		cfs_rq->throttled_clock_self_time += delta;
	}

	/*
	 * Move the tasks to a local list since an update_curr() during
	 * enqueue_task_fair() can throttle a higher cfs_rq, and it can
	 * see the "throttled_limbo_list" being non-empty in
	 * tg_throttle_down() if throttle_count turned 0 above.
	 */
	/*
	 * 临时摘到局部链表，隔离 enqueue 内再次触发上层 throttle 对原 limbo
	 * 链表的观察；这是链表所有权的短暂移交。
	 */
	list_splice_init(&cfs_rq->throttled_limbo_list, &throttled_tasks);

	/* Re-enqueue the tasks that have been throttled at this level. */
	/* 逐个恢复本层冻结的任务；每次入队都可能让层级重新进入节流。 */
	list_for_each_entry_safe(p, tmp, &throttled_tasks, throttle_node) {
		/*
		 * Back to being throttled! Break out and put the remaining
		 * tasks back onto the limbo_list to prevent running them
		 * unnecessarily.
		 */
		/* 若祖先重新节流，剩余任务保持 limbo，避免做注定不能运行的入队。 */
		if (cfs_rq->throttle_count)
			break;

		list_del_init(&p->throttle_node);
		p->throttled = false;
		enqueue_task_fair(rq, p, ENQUEUE_WAKEUP);
	}

	list_splice(&throttled_tasks, &cfs_rq->throttled_limbo_list);

	/* Add cfs_rq with load or one or more already running entities to the list */
	/* 仍有有效负载或运行实体时，重新加入叶子队列链供均衡器遍历。 */
	if (!cfs_rq_is_decayed(cfs_rq))
		list_add_leaf_cfs_rq(cfs_rq);

	return 0;
}

/*
 * task_has_throttle_work() - 查询任务是否已有待执行的节流 task_work。
 *
 * 业务背景：同一任务可能被多个祖先节流事件重复触达，需要合并回调。
 * 入参：@p 是待查询任务。
 * 出参/返回：next 不再指向自身表示 callback 已进入 task_work 链。
 * 注意事项：该哨兵协议由初始化和回调共同维护。
 */
static inline bool task_has_throttle_work(struct task_struct *p)
{
	return p->sched_throttle_work.next != &p->sched_throttle_work;
}

/*
 * task_throttle_setup_work() - 为可返回用户态的任务登记一次延迟节流工作。
 *
 * 业务背景：无法立刻安全出队时，将操作推迟到 resume-to-user 边界。
 * 入参：@p 是待节流任务。
 * 出参/返回：无；满足条件时把预初始化 callback 加入任务工作队列。
 * 注意事项：重复工作、内核线程和退出任务均跳过，因为它们无用户态返回点。
 */
static inline void task_throttle_setup_work(struct task_struct *p)
{
	if (task_has_throttle_work(p))
		return;

	/*
	 * Kthreads and exiting tasks don't return to userspace, so adding the
	 * work is pointless
	 */
	/* 内核线程和退出任务不会经过用户态恢复点，登记 TWA_RESUME 没有消费者。 */
	if ((p->flags & (PF_EXITING | PF_KTHREAD)))
		return;

	task_work_add(p, &p->sched_throttle_work, TWA_RESUME);
}

/*
 * record_throttle_clock() - 记录层级节流开始时的 rq 与本层时钟快照。
 *
 * 业务背景：解限时需从可运行和 PELT 时间中扣除实际冻结区间。
 * 入参：@cfs_rq 是进入或处于节流路径的队列。
 * 出参/返回：无；按需设置全局节流起点和本层自节流起点。
 * 注意事项：仅首次写入，防止嵌套节流覆盖最早起点。
 */
static void record_throttle_clock(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);

	if (cfs_rq_throttled(cfs_rq) && !cfs_rq->throttled_clock)
		cfs_rq->throttled_clock = rq_clock(rq);

	if (!cfs_rq->throttled_clock_self)
		cfs_rq->throttled_clock_self = rq_clock(rq);
}

/*
 * tg_throttle_down() - 沿任务组子树下压一次节流引用并冻结首次受影响队列。
 *
 * 业务背景：父组耗尽配额时，其所有后代 per-CPU cfs_rq 都必须变为不可运行。
 * 入参：@tg 是当前遍历组；@data 指向持锁 rq。
 * 出参/返回：始终返回 0；已有引用时只增加嵌套计数。
 * 注意事项：无排队实体时立即摘叶并暂停 PELT；有实体时由后续 dequeue 完成。
 */
static int tg_throttle_down(struct task_group *tg, void *data)
{
	struct rq *rq = data;
	struct cfs_rq *cfs_rq = tg_cfs_rq(tg, cpu_of(rq));

	if (cfs_rq->throttle_count++)
		return 0;

	/*
	 * For cfs_rqs that still have entities enqueued, PELT clock
	 * stop happens at dequeue time when all entities are dequeued.
	 */
	/* 仍有实体的队列会在逐级 dequeue 至空时停 PELT；空队列可在此立即冻结。 */
	if (!cfs_rq->nr_queued) {
		list_del_leaf_cfs_rq(cfs_rq);
		cfs_rq->throttled_clock_pelt = rq_clock_pelt(rq);
		cfs_rq->pelt_clock_throttled = 1;
	}

	WARN_ON_ONCE(cfs_rq->throttled_clock_self);
	WARN_ON_ONCE(!list_empty(&cfs_rq->throttled_limbo_list));
	return 0;
}

/*
 * throttle_cfs_rq() - 在无法续配 runtime 时冻结整个 CFS 层级。
 *
 * 业务背景：本地余额耗尽后先从 task_group 全局池补片；池也为空才阻止该组运行。
 * 入参：@cfs_rq 是余额耗尽、由其 rq 锁保护的队列。
 * 出参/返回：成功获得新 runtime 返回 false；真正完成或已处于节流返回 true。
 * 注意事项：同时涉及 cfs_b->lock、rq 锁、RCU 节流链表和祖先 runnable 记账，
 * 必须保持现有锁序；当前任务仅登记延迟 work，不能在错误上下文直接移除。
 */
static bool throttle_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	struct sched_entity *curr = cfs_rq->curr;
	struct rq *rq = rq_of(cfs_rq);

	scoped_guard(raw_spinlock, &cfs_b->lock) {
		u64 target_runtime = 1;

		/*
		 * If cfs_rq->curr is still runnable, we are here from an
		 * update_curr(). Request sysctl_sched_cfs_bandwidth_slice
		 * worth of bandwidth to continue running.
		 *
		 * If the curr is not runnable, just request enough bandwidth
		 * to be runnable next time the pick selects this cfs_rq.
		 */
		/* 当前实体仍可运行则申请一个完整 slice；否则 1ns 足以恢复下次可选资格。 */
		if (curr && curr->on_rq)
			target_runtime = sched_cfs_bandwidth_slice();

		/*
		 * Check if We have raced with bandwidth becoming available. If
		 * we actually throttled the timer might not unthrottle us for
		 * an entire period. We additionally needed to make sure that
		 * any subsequent check_cfs_rq_runtime calls agree not to
		 * throttle us, as we may commit to do cfs put_prev+pick_next,
		 * so we ask for 1ns of runtime rather than just check cfs_b.
		 *
		 * This will start the period timer if necessary.
		 */
		/*
		 * 在最终冻结前持 cfs_b 锁重试，关闭“配额刚补充”的竞态窗口；申请
		 * 至少 1ns 还保证后续 put_prev/pick_next 对状态判断一致，并按需启动周期 timer。
		 */
		if (__assign_cfs_rq_runtime(cfs_b, cfs_rq, target_runtime))
			return false;

		/*
		 * No bandwidth available; Add ourselves on the list to be
		 * unthrottled later.
		 */
		/* 全局池确实为空，把队列发布到 RCU 节流链，等待 period/slack 分发。 */
		list_add_tail_rcu(&cfs_rq->throttled_list,
				  &cfs_b->throttled_cfs_rq);
	}

	/* freeze hierarchy runnable averages while throttled */
	/* 对当前组及后代下压引用，冻结层级 runnable/PELT 视图。 */
	scoped_guard(rcu)
		walk_tg_tree_from(cfs_rq->tg, tg_throttle_down, tg_nop, (void *)rq);

	/*
	 * Note: distribution will already see us throttled via the
	 * throttled-list.  rq->lock protects completion.
	 */
	/* 分发者已能从链表观察本队列；rq 锁保证本地状态切换完整。 */
	cfs_rq->throttled = 1;
	WARN_ON_ONCE(cfs_rq->throttled_clock);

	/*
	 * If current hierarchy was throttled, add throttle work to the
	 * current donor. In case of proxy-execution, the execution
	 * context cannot exit to the userspace while holding a mutex
	 * and the rule of throttle deferral to only throttle the
	 * throttled context at exit to userspace is still preserved.
	 */
	/*
	 * 当前层级正在执行时，把延迟节流工作交给 donor；代理执行中 donor 才是
	 * 真正持有执行权且最终能安全到达用户态边界的任务。
	 */
	if (curr && curr->on_rq)
		task_throttle_setup_work(rq->donor);

	return true;
}

/*
 * unthrottle_cfs_rq() - 在获得正 runtime 后恢复队列、层级时钟和可运行关系。
 *
 * 业务背景：周期或 slack 分发为节流队列补充余额后，需要在目标 rq 锁下解冻。
 * 入参：@cfs_rq 是待恢复队列。
 * 出参/返回：无；余额仍非正时保留节流，否则摘链、恢复子树并按需唤醒 CPU。
 * 注意事项：先 update_curr() 消化异步窗口中的执行量，避免无余额恢复后立即再节流。
 */
void unthrottle_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	struct sched_entity *se = cfs_rq_se(cfs_rq);

	/*
	 * It's possible we are called with runtime_remaining < 0 due to things
	 * like async unthrottled us with a positive runtime_remaining but other
	 * still running entities consumed those runtime before we reached here.
	 *
	 * We can't unthrottle this cfs_rq without any runtime remaining because
	 * any enqueue in tg_unthrottle_up() will immediately trigger a throttle,
	 * which is not supposed to happen on unthrottle path.
	 *
	 * Catch up on the remaining runtime since last clock update before
	 * checking runtime remaining.
	 */
	/*
	 * 异步补额到真正取得 rq 锁之间，其他实体可能已耗尽新增额度；先结算再检查，
	 * 否则 tg_unthrottle_up() 的入队会在解限路径内反向触发节流。
	 */
	update_curr(cfs_rq);
	if (cfs_rq->runtime_enabled && cfs_rq->runtime_remaining <= 0)
		return;

	cfs_rq->throttled = 0;

	scoped_guard(raw_spinlock, &cfs_b->lock) {
		list_del_rcu(&cfs_rq->throttled_list);

		if (!cfs_rq->throttled_clock)
			break;

		cfs_b->throttled_time += rq_clock(rq) - cfs_rq->throttled_clock;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		cfs_rq->throttled_clock = 0;
	}

	/* update hierarchical throttle state */
	/* 自本组向下成对减少 throttle_count，并恢复 PELT 与 limbo 任务。 */
	walk_tg_tree_from(cfs_rq->tg, tg_nop, tg_unthrottle_up, (void *)rq);

	if (!cfs_rq->load.weight) {
		if (!cfs_rq->on_list)
			return;
		/*
		 * Nothing to run but something to decay (on_list)?
		 * Complete the branch.
		 */
		/* 无可运行负载但仍在衰减链时，补齐祖先叶子链的结构关系。 */
		for_each_sched_entity(se) {
			if (list_add_leaf_cfs_rq(cfs_rq_of(se)))
				break;
		}
	}

	assert_list_leaf_cfs_rq(rq);

	/* Determine whether we need to wake up potentially idle CPU: */
	/* 解限产生可运行实体且 CPU 正空闲时，请求重新调度。 */
	if (rq->curr == rq->idle && rq->cfs.nr_queued)
		resched_curr(rq);
}

/*
 * __cfsb_csd_unthrottle() - 在目标 CPU 上批量冲刷异步解限 CSD 链表。
 *
 * 业务背景：远端配额分发不能直接持目标 rq 锁操作其 cfs_rq，借异步 IPI 转交。
 * 入参：@arg 指向目标 rq。
 * 出参/返回：无；在 rq 锁下逐项摘链并调用 unthrottle_cfs_rq()。
 * 注意事项：RCU 临界区与 sched_free_group_rcu() 配对，防止摘链后组对象被释放。
 */
static void __cfsb_csd_unthrottle(void *arg)
{
	struct cfs_rq *cursor, *tmp;
	struct rq *rq = arg;

	guard(rq_lock)(rq);

	/*
	 * Iterating over the list can trigger several call to
	 * update_rq_clock() in unthrottle_cfs_rq().
	 * Do it once and skip the potential next ones.
	 */
	/* 批次只更新一次 rq 时钟，loop-update 标志让各解限调用复用该快照。 */
	update_rq_clock(rq);
	rq_clock_start_loop_update(rq);

	/*
	 * Since we hold rq lock we're safe from concurrent manipulation of
	 * the CSD list. However, this RCU critical section annotates the
	 * fact that we pair with sched_free_group_rcu(), so that we cannot
	 * race with group being freed in the window between removing it
	 * from the list and advancing to the next entry in the list.
	 */
	/* rq 锁保护链表结构；RCU 额外保护 cfs_rq 所属组在迭代窗口内的生命周期。 */
	guard(rcu)();

	list_for_each_entry_safe(cursor, tmp, &rq->cfsb_csd_list,
				 throttled_csd_list) {
		list_del_init(&cursor->throttled_csd_list);

		if (cfs_rq_throttled(cursor))
			unthrottle_cfs_rq(cursor);
	}

	rq_clock_stop_loop_update(rq);
}

/*
 * __unthrottle_cfs_rq_async() - 在本地直接恢复或向远端 rq 的 CSD 链排队。
 *
 * 业务背景：runtime 分发可遍历任意 CPU 的队列，实际恢复必须由所属 rq 串行化。
 * 入参：@cfs_rq 已获得正余额且仍被节流。
 * 出参/返回：无；本 CPU 同步处理，远端首项触发一次异步 IPI。
 * 注意事项：throttled_csd_list 既是队列节点也用于检测重复排队。
 */
static inline void __unthrottle_cfs_rq_async(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	bool first;

	if (rq == this_rq()) {
		update_rq_clock(rq);
		unthrottle_cfs_rq(cfs_rq);
		return;
	}

	/* Already enqueued */
	/* 节点非空说明已有分发者负责，重复加入会破坏链表。 */
	if (WARN_ON_ONCE(!list_empty(&cfs_rq->throttled_csd_list)))
		return;

	first = list_empty(&rq->cfsb_csd_list);
	list_add_tail(&cfs_rq->throttled_csd_list, &rq->cfsb_csd_list);
	if (first)
		smp_call_function_single_async(cpu_of(rq), &rq->cfsb_csd);
}

/*
 * unthrottle_cfs_rq_async() - 校验解限前置状态后执行跨 CPU 转交。
 *
 * 业务背景：调用者已持所属 rq 锁，使用该包装捕获错误状态。
 * 入参：@cfs_rq 是待异步恢复队列。
 * 出参/返回：无；非节流或余额非正时告警并放弃。
 * 注意事项：lockdep 明确要求 rq 锁，防止状态与 CSD 节点并发变化。
 */
static void unthrottle_cfs_rq_async(struct cfs_rq *cfs_rq)
{
	lockdep_assert_rq_held(rq_of(cfs_rq));

	if (WARN_ON_ONCE(!cfs_rq_throttled(cfs_rq) ||
	    cfs_rq->runtime_remaining <= 0))
		return;

	__unthrottle_cfs_rq_async(cfs_rq);
}

/*
 * distribute_cfs_runtime() - 把全局配额池分给节流队列并安排逐 CPU 解限。
 *
 * 业务背景：period/slack timer 为 task_group 充值后需清偿各 CPU 队列的负余额。
 * 入参：@cfs_b 是持有全局 runtime 与 RCU 节流链的带宽池。
 * 出参/返回：仍存在未获正余额的节流队列返回 true，否则返回 false。
 * 注意事项：每项按 rq 锁串行，取额度时短暂持 cfs_b 锁；远端只排 CSD，不跨锁恢复。
 */
static bool distribute_cfs_runtime(struct cfs_bandwidth *cfs_b)
{
	bool throttled = false, unthrottle_local = false;
	int this_cpu = smp_processor_id();
	u64 runtime, remaining = 1;
	struct cfs_rq *cfs_rq;
	struct rq *rq;

	guard(rcu)();

	list_for_each_entry_rcu(cfs_rq, &cfs_b->throttled_cfs_rq,
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
				throttled_list) {
		rq = rq_of(cfs_rq);

		if (!remaining) {
			throttled = true;
			break;
		}

		guard(rq_lock_irqsave)(rq);

		if (!cfs_rq_throttled(cfs_rq))
			continue;

		/* Already queued for async unthrottle */
		/* 已在目标 CPU 的 CSD 链中，前一个分发者会完成恢复。 */
		if (!list_empty(&cfs_rq->throttled_csd_list))
			continue;

		if (cfs_rq->curr) {
			update_rq_clock(rq);
			update_curr(cfs_rq);
		}

		/* By the above checks, this should never be true */
		/* 节流且未排解限工作的队列按协议不应已有正余额。 */
		WARN_ON_ONCE(cfs_rq->runtime_remaining > 0);

		scoped_guard(raw_spinlock, &cfs_b->lock) {
			runtime = -cfs_rq->runtime_remaining + 1;
			if (runtime > cfs_b->runtime)
				runtime = cfs_b->runtime;
			cfs_b->runtime -= runtime;
			remaining = cfs_b->runtime;
		}

		cfs_rq->runtime_remaining += runtime;

		/*
		 * Ran out of bandwidth during distribution!
		 * Indicate throttled entities and break early.
		 */
		/* 分得额度仍不足以跨过零点：记录尚有节流实体并停止空转遍历。 */
		if (cfs_rq->runtime_remaining <= 0) {
			throttled = true;
			break;
		}

		/* we check whether we're throttled above */
		/* 前面已确认仍节流；远端 CPU 通过异步 CSD 获得正确锁上下文。 */
		if (cpu_of(rq) != this_cpu) {
			unthrottle_cfs_rq_async(cfs_rq);
			continue;
		}

		/*
		 * Allow a parallel async unthrottle to unthrottle
		 * this cfs_rq too via __cfsb_csd_unthrottle().
		 * If we are first, do it ourselves at the end and
		 * save on an IPI from remote CPUs.
		 */
		/*
		 * 本 CPU 也先进入统一 CSD 链，以便并行远端 IPI 一并冲刷；若本项是首个，
		 * 循环末尾本地处理，从而省去对自己的 IPI。
		 */
		unthrottle_local = list_empty(&rq->cfsb_csd_list);
		list_add_tail(&cfs_rq->throttled_csd_list, &rq->cfsb_csd_list);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	if (unthrottle_local) {
		/*
		 * Protect against an IPI that is also trying to flush
		 * the unthrottled cfs_rq(s) from this CPU's csd_list.
		 */
		/* 关中断与可能到达的 IPI 串行，确保本地 CSD 链只被一个冲刷者消费。 */
		scoped_guard(irqsave)
			__cfsb_csd_unthrottle(cpu_rq(this_cpu));
	}

	return throttled;
}

/*
 * Responsible for refilling a task_group's bandwidth and unthrottling its
 * cfs_rqs as appropriate. If there has been no activity within the last
 * period the timer is deactivated until scheduling resumes; cfs_b->idle is
 * used to track this state.
 */
/*
 * do_sched_cfs_period_timer() - 处理一个或多个逾期周期并分发新配额。
 *
 * 业务背景：period hrtimer 到期时给 task_group 充值，并解限能被新额度覆盖的队列。
 * 入参：@cfs_b 是带宽池；@overrun 是跨过周期数；@flags 保存调用者中断状态。
 * 出参/返回：可停用周期 timer 返回 1；仍需下周期回调返回 0。
 * 注意事项：入口持 cfs_b 锁；分发前必须解锁，因为分发会按相反层次获取 rq 锁。
 */
static int do_sched_cfs_period_timer(struct cfs_bandwidth *cfs_b, int overrun, unsigned long flags)
	__must_hold(&cfs_b->lock)
{
	int throttled;

	/* no need to continue the timer with no bandwidth constraint */
	/* 无限 quota 不存在节流需求，可直接停用 timer。 */
	if (cfs_b->quota == RUNTIME_INF)
		goto out_deactivate;

	throttled = !list_empty(&cfs_b->throttled_cfs_rq);
	cfs_b->nr_periods += overrun;

	/* Refill extra burst quota even if cfs_b->idle */
	/* 即使上一周期空闲也补充 burst，使后续唤醒看到正确可突发余额。 */
	__refill_cfs_bandwidth_runtime(cfs_b);

	/*
	 * idle depends on !throttled (for the case of a large deficit), and if
	 * we're going inactive then everything else can be deferred
	 */
	/* 已空闲且没有节流欠账时无需继续周期性唤醒。 */
	if (cfs_b->idle && !throttled)
		goto out_deactivate;

	if (!throttled) {
		/* mark as potentially idle for the upcoming period */
		/* 本周期无人等待，先标记候选空闲；新活动会重新启动 timer。 */
		cfs_b->idle = 1;
		return 0;
	}

	/* account preceding periods in which throttling occurred */
	/* overrun 的每个周期都存在未清节流，累计统计次数。 */
	cfs_b->nr_throttled += overrun;

	/*
	 * This check is repeated as we release cfs_b->lock while we unthrottle.
	 */
	/* 分发需要释放池锁，返回后重新检查余额和未解限状态以容纳并发变化。 */
	while (throttled && cfs_b->runtime > 0) {
		raw_spin_unlock_irqrestore(&cfs_b->lock, flags);
		/* we can't nest cfs_b->lock while distributing bandwidth */
		throttled = distribute_cfs_runtime(cfs_b);
		raw_spin_lock_irqsave(&cfs_b->lock, flags);
	}

	/*
	 * While we are ensured activity in the period following an
	 * unthrottle, this also covers the case in which the new bandwidth is
	 * insufficient to cover the existing bandwidth deficit.  (Forcing the
	 * timer to remain active while there are any throttled entities.)
	 */
	/* 只要尚有欠额队列就保持 timer 活跃，哪怕本轮新额度不足以全部清偿。 */
	cfs_b->idle = 0;

	return 0;

out_deactivate:
	return 1;
}

/* a cfs_rq won't donate quota below this amount */
/* 单个 cfs_rq 至少保留 1ms，避免频繁归还后立刻重新申请。 */
static const u64 min_cfs_rq_runtime = 1 * NSEC_PER_MSEC;
/* minimum remaining period time to redistribute slack quota */
/* 距周期刷新不足 2ms 时不再启动独立 slack 分发。 */
static const u64 min_bandwidth_expiration = 2 * NSEC_PER_MSEC;
/* how long we wait to gather additional slack before distributing */
/* 等待 5ms 聚合多个队列归还的零散额度，再批量分发。 */
static const u64 cfs_bandwidth_slack_period = 5 * NSEC_PER_MSEC;

/*
 * Are we near the end of the current quota period?
 *
 * Requires cfs_b->lock for hrtimer_expires_remaining to be safe against the
 * hrtimer base being cleared by hrtimer_start. In the case of
 * migrate_hrtimers, base is never cleared, so we are fine.
 */
/*
 * runtime_refresh_within() - 判断周期配额是否将在给定时间窗内刷新。
 *
 * 业务背景：临近 period 边界时无需另启 slack timer 做重复分发。
 * 入参：@cfs_b 提供 period timer；@min_expire 是纳秒时间窗。
 * 出参/返回：回调正在运行或剩余时间小于窗口返回 1，否则返回 0。
 * 注意事项：调用者持 cfs_b 锁，保证读取 hrtimer base/到期时间稳定。
 */
static int runtime_refresh_within(struct cfs_bandwidth *cfs_b, u64 min_expire)
{
	struct hrtimer *refresh_timer = &cfs_b->period_timer;
	s64 remaining;

	/* if the call-back is running a quota refresh is already occurring */
	/* 周期回调已在充值，slack 分发无需竞争执行。 */
	if (hrtimer_callback_running(refresh_timer))
		return 1;

	/* is a quota refresh about to occur? */
	/* 到期时间落入窗口则让即将发生的周期刷新统一处理。 */
	remaining = ktime_to_ns(hrtimer_expires_remaining(refresh_timer));
	if (remaining < (s64)min_expire)
		return 1;

	return 0;
}

/*
 * start_cfs_slack_bandwidth() - 在合适时机启动一次延迟 slack 分发 timer。
 *
 * 业务背景：空队列归还的余额可提前解限其他 CPU，但需聚合以控制 IPI/锁开销。
 * 入参：@cfs_b 是已收到归还额度的带宽池。
 * 出参/返回：无；临近刷新或已有 timer 时保持现状，否则启动相对 timer。
 * 注意事项：由 cfs_b 锁保护 @slack_started 和两类 timer 的协调。
 */
static void start_cfs_slack_bandwidth(struct cfs_bandwidth *cfs_b)
{
	u64 min_left = cfs_bandwidth_slack_period + min_bandwidth_expiration;

	/* if there's a quota refresh soon don't bother with slack */
	/* 周期刷新将很快发生时，额外 timer 只会制造重复工作。 */
	if (runtime_refresh_within(cfs_b, min_left))
		return;

	/* don't push forwards an existing deferred unthrottle */
	/* 已安排的批次保持原到期点，持续归还不能无限向后推迟解限。 */
	if (cfs_b->slack_started)
		return;
	cfs_b->slack_started = true;

	hrtimer_start(&cfs_b->slack_timer,
			ns_to_ktime(cfs_bandwidth_slack_period),
			HRTIMER_MODE_REL);
}

/* we know any runtime found here is valid as update_curr() precedes return */
/* update_curr() 已先结算，因此这里找到的剩余额度可以安全归还。 */
/*
 * __return_cfs_rq_runtime() - 把空闲队列超过保留量的 runtime 归还全局池。
 *
 * 业务背景：本地缓存的闲置配额可用于提前恢复同组其他 CPU 上的节流队列。
 * 入参：@cfs_rq 已完成执行结算且当前无须保留全部余额。
 * 出参/返回：无；最多保留 min_cfs_rq_runtime，其余加入 cfs_b->runtime。
 * 注意事项：持 rq 锁时不能直接跨 rq 解限，只能启动 slack timer 延后分发。
 */
static void __return_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	s64 slack_runtime = cfs_rq->runtime_remaining - min_cfs_rq_runtime;

	if (slack_runtime <= 0)
		return;

	guard(raw_spinlock)(&cfs_b->lock);

	if (cfs_b->quota != RUNTIME_INF) {
		cfs_b->runtime += slack_runtime;

		/* we are under rq->lock, defer unthrottling using a timer */
		/* rq 锁下不能进行多 rq 分发，用 timer 打破锁嵌套。 */
		if (cfs_b->runtime > sched_cfs_bandwidth_slice() &&
		    !list_empty(&cfs_b->throttled_cfs_rq))
			start_cfs_slack_bandwidth(cfs_b);
	}

	/* even if it's not valid for return we don't want to try again */
	/* 无限 quota 时虽不入全局池，也扣除本地 slack，避免每次空队列都重复尝试。 */
	cfs_rq->runtime_remaining -= slack_runtime;
}

/*
 * return_cfs_rq_runtime() - 仅在带宽控制队列变空时尝试归还余额。
 *
 * 业务背景：dequeue 后空队列不再近期消费本地 slice，可释放资源给同组队列。
 * 入参：@cfs_rq 是刚完成出队/结算的队列。
 * 出参/返回：无；功能关闭、runtime 未启用或仍有排队实体时跳过。
 * 注意事项：这是静态键热路径包装，实际数值处理在 __return_cfs_rq_runtime()。
 */
static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return;

	if (!cfs_rq->runtime_enabled || cfs_rq->nr_queued)
		return;

	__return_cfs_rq_runtime(cfs_rq);
}

/*
 * This is done with a timer (instead of inline with bandwidth return) since
 * it's necessary to juggle rq->locks to unthrottle their respective cfs_rqs.
 */
/* 归还发生在 rq 锁内，借 timer 延后到可安全切换各目标 rq 锁的上下文。 */
/*
 * do_sched_cfs_slack_timer() - 校验归还额度仍有效并触发一次批量分发。
 *
 * 业务背景：slack timer 聚合多个 cfs_rq 的归还额度，提前偿还节流欠账。
 * 入参：@cfs_b 是 timer 所属带宽池。
 * 出参/返回：无；临近周期刷新、无限 quota 或余额不足一个 slice 时不分发。
 * 注意事项：先在池锁下拍快照并清 slack_started，再释放锁调用多 rq 分发。
 */
static void do_sched_cfs_slack_timer(struct cfs_bandwidth *cfs_b)
{
	/* confirm we're still not at a refresh boundary */
	/* timer 到期时再次确认 period 边界，覆盖启动后时间推进的竞态。 */
	scoped_guard(raw_spinlock_irqsave, &cfs_b->lock) {
		u64 runtime = 0, slice = sched_cfs_bandwidth_slice();

		cfs_b->slack_started = false;

		if (runtime_refresh_within(cfs_b, min_bandwidth_expiration))
			return;

	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
		if (cfs_b->quota != RUNTIME_INF && cfs_b->runtime > slice)
			runtime = cfs_b->runtime;

		if (!runtime)
			return;
	}

	distribute_cfs_runtime(cfs_b);
}

/*
 * When a group wakes up we want to make sure that its quota is not already
 * expired/exceeded, otherwise it may be allowed to steal additional ticks of
 * runtime as update_curr() throttling can not trigger until it's on-rq.
 */
/*
 * check_enqueue_throttle() - 组从空闲唤醒时预先核对 quota 并按需节流。
 *
 * 业务背景：实体尚未 on-rq 时 update_curr() 不会触发扣费，直接入队可能偷跑数 tick。
 * 入参：@cfs_rq 是即将接收首个实体的队列。
 * 出参/返回：无；非活动且未节流队列以零增量执行一次余额检查。
 * 注意事项：已有 curr 的活动组由正常 update_curr() 路径负责，避免重复记账。
 */
static void check_enqueue_throttle(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return;

	/* an active group must be handled by the update_curr() path */
	/* 活动队列随执行结算检查，无需在入队路径重复处理。 */
	if (!cfs_rq->runtime_enabled || cfs_rq->curr)
		return;

	/* ensure the group is not already throttled */
	/* 已节流队列已经在恢复链上，不能重复插入。 */
	if (cfs_rq_throttled(cfs_rq))
		return;

	/* update runtime allocation */
	/* delta 为零仅检查/续配已有负余额，不虚构新的执行量。 */
	account_cfs_rq_runtime(cfs_rq, 0);
}

/*
 * sync_throttle() - 新建 per-CPU 组队列时继承父层节流与 PELT 冻结状态。
 *
 * 业务背景：离线/未排队层级可能错过先前的 throttle_down 遍历，首次使用前需同步。
 * 入参：@tg 是待同步任务组；@cpu 指定其 per-CPU cfs_rq。
 * 出参/返回：无；根组或功能关闭不处理，否则复制父计数并初始化时钟快照。
 * 注意事项：只要继承非零计数就强制冻结 PELT，首个 enqueue 或分发再负责恢复。
 */
static void sync_throttle(struct task_group *tg, int cpu)
{
	struct cfs_rq *pcfs_rq, *cfs_rq;

	if (!cfs_bandwidth_used())
		return;

	if (!tg->parent)
		return;

	cfs_rq = tg_cfs_rq(tg, cpu);
	pcfs_rq = tg_cfs_rq(tg->parent, cpu);

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	cfs_rq->throttle_count = pcfs_rq->throttle_count;
	cfs_rq->throttled_clock_pelt = rq_clock_pelt(cpu_rq(cpu));

	/*
	 * It is not enough to sync the "pelt_clock_throttled" indicator
	 * with the parent cfs_rq when the hierarchy is not queued.
	 * Always join a throttled hierarchy with PELT clock throttled
	 * and leaf it to the first enqueue, or distribution to
	 * unthrottle the PELT clock.
	 */
	/*
	 * 未排队子层不能只复制父层的布尔位；统一以冻结状态加入层级，避免离线窗口
	 * 中 PELT 偷走时间，之后由首次入队或配额分发成对解冻。
	 */
	if (cfs_rq->throttle_count)
		cfs_rq->pelt_clock_throttled = 1;
}

/*
 * sched_cfs_slack_timer() - slack hrtimer 回调包装。
 *
 * 业务背景：到期后把 timer 对象还原为带宽池并执行延迟分发。
 * 入参：@timer 是 cfs_b->slack_timer。
 * 出参/返回：处理一次后返回 HRTIMER_NORESTART。
 * 注意事项：下一次 slack 归还会按需重新显式启动。
 */
static enum hrtimer_restart sched_cfs_slack_timer(struct hrtimer *timer)
{
	struct cfs_bandwidth *cfs_b =
		container_of(timer, struct cfs_bandwidth, slack_timer);

	do_sched_cfs_slack_timer(cfs_b);

	return HRTIMER_NORESTART;
}

/*
 * sched_cfs_period_timer() - 推进 CFS 配额周期并驱动充值/解限。
 *
 * 业务背景：固定周期维持 quota 速率；过短周期导致连续 overrun 时需自适应放大。
 * 入参：@timer 是 cfs_b->period_timer。
 * 出参/返回：带宽池空闲返回 NORESTART，否则返回 RESTART。
 * 注意事项：持 cfs_b irqsave 锁调用周期处理；放大 period 时同比放大 quota/burst，
 * 保持带宽比率不变并避免整数精度让 __cfs_schedulable() 误判。
 */
static enum hrtimer_restart sched_cfs_period_timer(struct hrtimer *timer)
{
	struct cfs_bandwidth *cfs_b =
		container_of(timer, struct cfs_bandwidth, period_timer);
	int overrun;
	int idle = 0;
	int count = 0;

	CLASS(raw_spinlock_irqsave, cfsb_guard)(&cfs_b->lock);

	for (;;) {
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		overrun = hrtimer_forward_now(timer, cfs_b->period);
		if (!overrun)
			break;

		idle = do_sched_cfs_period_timer(cfs_b, overrun, cfsb_guard.flags);

		if (++count > 3) {
			u64 new, old = ktime_to_ns(cfs_b->period);

			/*
			 * Grow period by a factor of 2 to avoid losing precision.
			 * Precision loss in the quota/period ratio can cause __cfs_schedulable
			 * to fail.
			 */
			/* 连续追赶说明周期过短；三者同比翻倍保持 quota/period 比率并提升精度。 */
			new = old * 2;
			if (new < max_bw_quota_period_us * NSEC_PER_USEC) {
				cfs_b->period = ns_to_ktime(new);
				cfs_b->quota *= 2;
				cfs_b->burst *= 2;

				pr_warn_ratelimited(
	"cfs_period_timer[cpu%d]: period too short, scaling up (new cfs_period_us = %lld, cfs_quota_us = %lld)\n",
					smp_processor_id(),
					div_u64(new, NSEC_PER_USEC),
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
					div_u64(cfs_b->quota, NSEC_PER_USEC));
			} else {
				pr_warn_ratelimited(
	"cfs_period_timer[cpu%d]: period too short, but cannot scale up without losing precision (cfs_period_us = %lld, cfs_quota_us = %lld)\n",
					smp_processor_id(),
					div_u64(old, NSEC_PER_USEC),
					div_u64(cfs_b->quota, NSEC_PER_USEC));
			}

			/* reset count so we don't come right back in here */
			/* 清零重试计数，避免一次长延迟中连续倍增。 */
			count = 0;
		}
	}

	if (idle) {
		cfs_b->period_active = 0;
		return HRTIMER_NORESTART;
	}

	return HRTIMER_RESTART;
}

/*
 * init_cfs_bandwidth() - 初始化任务组的配额池、两类 timer 和节流链表。
 *
 * 业务背景：创建 task_group 时建立独立 quota/burst 状态及周期性补额机制。
 * 入参：@cfs_b 是待初始化池；@parent 提供父组层级 quota，根组可为 NULL。
 * 出参/返回：无；初始无限 quota、空余额和未激活 timer。
 * 注意事项：period timer 加随机初始偏移，使不同组回调错峰以降低锁峰值。
 */
void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b, struct cfs_bandwidth *parent)
{
	raw_spin_lock_init(&cfs_b->lock);
	cfs_b->runtime = 0;
	cfs_b->quota = RUNTIME_INF;
	cfs_b->period = us_to_ktime(default_bw_period_us());
	cfs_b->burst = 0;
	cfs_b->hierarchical_quota = parent ? parent->hierarchical_quota : RUNTIME_INF;

	INIT_LIST_HEAD(&cfs_b->throttled_cfs_rq);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	hrtimer_setup(&cfs_b->period_timer, sched_cfs_period_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_ABS_PINNED);

	/* Add a random offset so that timers interleave */
	/* 随机化首次到期，让大量组的周期 timer 在时间轴上交错。 */
	hrtimer_set_expires(&cfs_b->period_timer,
			    get_random_u32_below(cfs_b->period));
	hrtimer_setup(&cfs_b->slack_timer, sched_cfs_slack_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	cfs_b->slack_started = false;
}

/*
 * init_cfs_rq_runtime() - 初始化 per-CPU CFS 队列的带宽运行态节点。
 *
 * 业务背景：每个 task_group 在每个 CPU 上有独立本地余额和三类等待链节点。
 * 入参：@cfs_rq 是待初始化队列。
 * 出参/返回：无；默认禁用 runtime，并清空节流、CSD 与 limbo 链。
 * 注意事项：是否启用稍后依据组 quota 在 CPU online/config 路径同步。
 */
static void init_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	cfs_rq->runtime_enabled = 0;
	INIT_LIST_HEAD(&cfs_rq->throttled_list);
	INIT_LIST_HEAD(&cfs_rq->throttled_csd_list);
	INIT_LIST_HEAD(&cfs_rq->throttled_limbo_list);
}

/*
 * start_cfs_bandwidth() - 在首次需要配额时启动绝对周期 timer。
 *
 * 业务背景：无限 quota 或长期空闲时 timer 可停用，调度恢复时按需重启。
 * 入参：@cfs_b 是调用者已持锁的带宽池。
 * 出参/返回：无；已激活则幂等返回，否则推进到下一周期并启动。
 * 注意事项：必须持 cfs_b->lock，使 @period_active 与 timer 状态原子一致。
 */
void start_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	lockdep_assert_held(&cfs_b->lock);

	if (cfs_b->period_active)
		return;

	cfs_b->period_active = 1;
	hrtimer_forward_now(&cfs_b->period_timer, cfs_b->period);
	hrtimer_start_expires(&cfs_b->period_timer, HRTIMER_MODE_ABS_PINNED);
}

/*
 * destroy_cfs_bandwidth() - 销毁组配额 timer 并冲刷残留的跨 CPU 解限工作。
 *
 * 业务背景：task_group 释放前必须停止所有可能再访问其 cfs_rq 的异步来源。
 * 入参：@cfs_b 是待销毁带宽池。
 * 出参/返回：无；未初始化时跳过，否则取消 timer 并逐 CPU 清空相关 CSD 链。
 * 注意事项：此时已保证不会有该组的新 cfs_rq 入链；冲刷在 irq 禁用区串行执行。
 */
static void destroy_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	int __maybe_unused i;

	/* init_cfs_bandwidth() was not called */
	/* 链表头尚无 next 表明初始化从未发生，timer 对象也不可取消。 */
	if (!cfs_b->throttled_cfs_rq.next)
		return;

	hrtimer_cancel(&cfs_b->period_timer);
	hrtimer_cancel(&cfs_b->slack_timer);

	/*
	 * It is possible that we still have some cfs_rq's pending on a CSD
	 * list, though this race is very rare. In order for this to occur, we
	 * must have raced with the last task leaving the group while there
	 * exist throttled cfs_rq(s), and the period_timer must have queued the
	 * CSD item but the remote cpu has not yet processed it. To handle this,
	 * we can simply flush all pending CSD work inline here. We're
	 * guaranteed at this point that no additional cfs_rq of this group can
	 * join a CSD list.
	 */
	/*
	 * 极小窗口内 period timer 可能已把本组 cfs_rq 放入远端 CSD，而最后任务同时
	 * 离组；取消 timer 后逐 CPU 内联冲刷，利用“不再有新入链”保证生命周期收敛。
	 */
	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);

		if (list_empty(&rq->cfsb_csd_list))
			continue;

		scoped_guard(irqsave)
			__cfsb_csd_unthrottle(rq);
	}
}

/*
 * Both these CPU hotplug callbacks race against unregister_fair_sched_group()
 *
 * The race is harmless, since modifying bandwidth settings of unhooked group
 * bits doesn't do much.
 */
/* CPU hotplug 与组注销可并发；已摘除组上的配置位变化不再影响调度，竞态无害。 */

/* cpu online callback */
/* CPU 上线回调：依据各组当前 quota 刷新该 CPU 的 runtime_enabled。 */
/*
 * update_runtime_enabled() - 为新上线 CPU 同步所有任务组的带宽开关。
 *
 * 业务背景：离线期间 quota 可能变化，新 CPU 的 per-CPU 队列不能沿用旧状态。
 * 入参：@rq 是已持锁的上线运行队列。
 * 出参/返回：无；RCU 遍历任务组并在各自池锁下更新 runtime_enabled。
 * 注意事项：组列表生命周期由 RCU 保护，数值与 quota 的一致性由 cfs_b 锁保护。
 */
static void __maybe_unused update_runtime_enabled(struct rq *rq)
{
	struct task_group *tg;

	lockdep_assert_rq_held(rq);

	guard(rcu)();

	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;
		struct cfs_rq *cfs_rq = tg_cfs_rq(tg, cpu_of(rq));

		scoped_guard(raw_spinlock, &cfs_b->lock)
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			cfs_rq->runtime_enabled = cfs_b->quota != RUNTIME_INF;
	}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}

/* cpu offline callback */
/* CPU 下线回调：解除该 rq 上残留的 CFS 节流状态，避免离线状态泄漏。 */
/*
 * unthrottle_offline_cfs_rqs() - CPU 真正不活跃后恢复其所有受限组队列。
 *
 * 业务背景：离线 rq 不再等待远端 timer/CSD，残留节流链必须在热拔路径清理。
 * 入参：@rq 是已持锁的目标运行队列。
 * 出参/返回：无；CPU 仍 active 时拒绝执行，否则遍历各组解限。
 * 注意事项：set_rq_offline() 已更新 rq 时钟，后续解限使用 loop-update 抑制重复更新。
 */
static void __maybe_unused unthrottle_offline_cfs_rqs(struct rq *rq)
{
	struct task_group *tg;

	lockdep_assert_rq_held(rq);

	// Do not unthrottle for an active CPU
	/* 活跃 CPU 仍由正常配额机制管理，不能借 hotplug 路径绕过限制。 */
	if (cpumask_test_cpu(cpu_of(rq), cpu_active_mask))
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;

	/*
	 * The rq clock has already been updated in the
	 * set_rq_offline(), so we should skip updating
	 * the rq clock again in unthrottle_cfs_rq().
	 */
	/* rq 时钟已由 set_rq_offline() 推进，标记循环更新以避免各队列重复读取。 */
	rq_clock_start_loop_update(rq);

	guard(rcu)();

	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct cfs_rq *cfs_rq = tg_cfs_rq(tg, cpu_of(rq));

		if (!cfs_rq->runtime_enabled)
			continue;

		/*
		 * Offline rq is schedulable till CPU is completely disabled
		 * in take_cpu_down(), so we prevent new cfs throttling here.
		 */
		/* CPU 完全关闭前 rq 仍可能被选择；先关闭 runtime 检查，阻止产生新的节流。 */
		cfs_rq->runtime_enabled = 0;

		if (!cfs_rq_throttled(cfs_rq))
			continue;

		/*
		 * clock_task is not advancing so we just need to make sure
		 * there's some valid quota amount
		 */
		/* 离线后 clock_task 不再前进，写入最小正余额只为满足解限前置条件。 */
		cfs_rq->runtime_remaining = 1;
		unthrottle_cfs_rq(cfs_rq);
	}

	rq_clock_stop_loop_update(rq);
}

/*
 * cfs_task_bw_constrained() - 判断任务是否受本组或祖先 CFS 带宽约束。
 *
 * 业务背景：NO_HZ_FULL 等路径需要知道任务是否依赖周期 tick 做配额扣费。
 * 入参：@p 是待查询公平类任务。
 * 出参/返回：本队列启用 runtime 或层级 quota 有限时返回 true。
 * 注意事项：静态键关闭时快速返回；层级约束不能只看本队列开关。
 */
bool cfs_task_bw_constrained(struct task_struct *p)
{
	struct cfs_rq *cfs_rq = task_cfs_rq(p);

	if (!cfs_bandwidth_used())
		return false;

	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
	if (cfs_rq->runtime_enabled ||
	    tg_cfs_bandwidth(cfs_rq->tg)->hierarchical_quota != RUNTIME_INF)
		return true;

	return false;
}

#ifdef CONFIG_NO_HZ_FULL
/* called from pick_next_task_fair() */
/* 由 pick_next_task_fair() 在选定任务后调用。 */
/*
 * sched_fair_update_stop_tick() - 为受带宽约束的单任务 rq 保留调度 tick。
 *
 * 业务背景：NO_HZ_FULL 通常可在单任务运行时停 tick，但 CFS quota 仍需周期性记账。
 * 入参：@rq 是当前运行队列；@p 是刚选中的唯一可运行任务。
 * 出参/返回：无；仅在 full-nohz CPU 且恰有一个任务时设置调度 tick 依赖。
 * 注意事项：普通 enqueue 已处理可停 tick 条件，这里只补带宽控制这一约束。
 */
static void sched_fair_update_stop_tick(struct rq *rq, struct task_struct *p)
{
	int cpu = cpu_of(rq);

	if (!cfs_bandwidth_used())
		return;

	if (!tick_nohz_full_cpu(cpu))
		return;

	if (rq->nr_running != 1)
		return;

	/*
	 *  We know there is only one task runnable and we've just picked it. The
	 *  normal enqueue path will have cleared TICK_DEP_BIT_SCHED if we will
	 *  be otherwise able to stop the tick. Just need to check if we are using
	 *  bandwidth control.
	 */
	/* 唯一任务刚被选中，普通入队已清理其他依赖；这里只检查 quota 是否要求保留 tick。 */
	if (cfs_task_bw_constrained(p))
		tick_nohz_dep_set_cpu(cpu, TICK_DEP_BIT_SCHED);
}
#endif /* CONFIG_NO_HZ_FULL */

#else /* !CONFIG_CFS_BANDWIDTH: */

/*
 * account_cfs_rq_runtime() - 未配置 CFS 带宽控制时的零成本占位实现。
 *
 * 业务背景：让公共调度路径无需条件编译即可调用 runtime 记账接口。
 * 入参：@cfs_rq 与 @delta_exec 在该配置下均不使用。
 * 出参/返回：始终返回 false，表示不会由配额触发节流。
 * 注意事项：这是编译期分支，不代表启用功能后的队列余额状态。
 */
static bool account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec) { return false; }
/*
 * check_enqueue_throttle() - 未配置带宽控制时的入队检查占位。
 *
 * 业务背景：保留统一调用点，编译器会消除空函数。
 * 入参：@cfs_rq 未使用。
 * 出参/返回：无。
 * 注意事项：不读取或改变任何队列状态。
 */
static void check_enqueue_throttle(struct cfs_rq *cfs_rq) {}
/* 无带宽控制配置：组层级没有节流状态需要同步。入参仅维持统一接口，无返回值。 */
static inline void sync_throttle(struct task_group *tg, int cpu) {}
/* 无带宽控制配置：空队列没有可归还的本地配额。入参不使用，无返回值。 */
static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq) {}
/* 无带宽控制配置：不会登记延迟节流工作。入参不使用，无返回值。 */
static void task_throttle_setup_work(struct task_struct *p) {}
/* 无带宽控制配置：任务永不因 CFS quota 被移入 limbo，固定返回 false。 */
static bool task_is_throttled(struct task_struct *p) { return false; }
/* 无带宽控制配置：没有 limbo 任务需要二次出队。入参不使用，无返回值。 */
static void dequeue_throttled_task(struct task_struct *p, int flags) {}
/* 无带宽控制配置：不存在 limbo 入队快路，固定要求调用者走正常入队。 */
static bool enqueue_throttled_task(struct task_struct *p) { return false; }
/* 无带宽控制配置：无需记录冻结时钟。入参不使用，无返回值。 */
static void record_throttle_clock(struct cfs_rq *cfs_rq) {}

/* 无带宽控制配置：单队列不会节流，固定返回 0。 */
static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq)
{
	return 0;
}

/* 无带宽控制配置：PELT 时钟不会因 quota 暂停，固定返回 false。 */
static inline bool cfs_rq_pelt_clock_throttled(struct cfs_rq *cfs_rq)
{
	return false;
}

/* 无带宽控制配置：层级没有节流引用，固定返回 0。 */
static inline int throttled_hierarchy(struct cfs_rq *cfs_rq)
{
	return 0;
}

/* 无带宽控制配置：目标 CPU 上的任务组也不会节流，固定返回 0。 */
static inline int lb_throttled_hierarchy(struct task_struct *p, int dst_cpu)
{
	return 0;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/* 无带宽控制配置：保留组创建接口，不初始化 quota 状态；入参不使用。 */
void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b, struct cfs_bandwidth *parent) {}
/* 无带宽控制配置：per-CPU 组队列无需 runtime 节点初始化。 */
static void init_cfs_rq_runtime(struct cfs_rq *cfs_rq) {}
#endif

/* 无带宽控制配置：任务组没有对应配额池，固定返回 NULL。 */
static inline struct cfs_bandwidth *tg_cfs_bandwidth(struct task_group *tg)
{
	return NULL;
}
/* 无带宽控制配置：不存在 timer/CSD 生命周期需要销毁。 */
static inline void destroy_cfs_bandwidth(struct cfs_bandwidth *cfs_b) {}
/* 无带宽控制配置：CPU 上线无需同步 runtime_enabled。 */
static inline void update_runtime_enabled(struct rq *rq) {}
/* 无带宽控制配置：CPU 下线没有节流队列需要恢复。 */
static inline void unthrottle_offline_cfs_rqs(struct rq *rq) {}
#ifdef CONFIG_CGROUP_SCHED
/* 无带宽控制配置：任何任务都不受 CFS quota 约束，固定返回 false。 */
bool cfs_task_bw_constrained(struct task_struct *p)
{
	return false;
}
#endif
#endif /* !CONFIG_CFS_BANDWIDTH */

#if !defined(CONFIG_CFS_BANDWIDTH) || !defined(CONFIG_NO_HZ_FULL)
/* 缺少带宽控制或 NO_HZ_FULL 时，不需要额外保留调度 tick。 */
static inline void sched_fair_update_stop_tick(struct rq *rq, struct task_struct *p) {}
#endif

/**************************************************
 * CFS operations on tasks:
 */

#ifdef CONFIG_SCHED_HRTICK
/*
 * hrtick_start_fair() - 按当前实体距虚拟截止期的时间启动高精度抢占 tick。
 *
 * 业务背景：多个公平实体竞争时，用 hrtick 提高 EEVDF 截止期切换精度。
 * 入参：@rq 是持锁运行队列；@p 是当前 donor 公平任务。
 * 出参/返回：无；不足两个实体或截止期已过时不启动，必要时直接重调度。
 * 注意事项：实际墙钟时长按实体权重换算，并补偿 IRQ 等其他类别瞬时占用。
 */
static void hrtick_start_fair(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	unsigned long scale = 1024;
	unsigned long util = 0;
	u64 vdelta;
	u64 delta;

	WARN_ON_ONCE(task_rq(p) != rq);

	if (rq->cfs.h_nr_queued <= 1)
		return;

	/*
	 * Compute time until virtual deadline
	 */
	/* 先求截止期相对 vruntime 的虚拟距离，再按权重还原为实际运行时间。 */
	vdelta = se->deadline - se->vruntime;
	if ((s64)vdelta < 0) {
		if (task_current_donor(rq, p))
			resched_curr(rq);
		return;
	}
	delta = (se->load.weight * vdelta) / NICE_0_LOAD;

	/*
	 * Correct for instantaneous load of other classes.
	 */
	/* IRQ 等非公平类占用会稀释 CPU 供给，按剩余容量放大定时长度。 */
	util += cpu_util_irq(rq);
	if (util && util < 1024) {
		scale *= 1024;
		scale /= (1024 - util);
	}

	hrtick_start(rq, (scale * delta) / 1024);
}

/*
 * Called on enqueue to start the hrtick when h_nr_queued becomes more than 1.
 */
/* 入队使层级实体数超过 1 时调用，以便为当前 donor 建立精确截止点。 */
/*
 * hrtick_update() - 在需要且尚无 hrtick 时为公平类 donor 启动定时器。
 *
 * 业务背景：竞争者新入队后，原单任务运行的粗粒度 tick 已不足以精确切换。
 * 入参：@rq 是当前运行队列。
 * 出参/返回：无；功能关闭、donor 非公平类或已有 timer 时幂等返回。
 * 注意事项：真正的时长计算委托 hrtick_start_fair()。
 */
static void hrtick_update(struct rq *rq)
{
	struct task_struct *donor = rq->donor;

	if (!hrtick_enabled_fair(rq) || donor->sched_class != &fair_sched_class)
		return;

	if (hrtick_active(rq))
		return;

	hrtick_start_fair(rq, donor);
}
#else /* !CONFIG_SCHED_HRTICK: */
/* 未配置高精度调度 tick：保留统一调用接口，所有入参不使用。 */
static inline void
hrtick_start_fair(struct rq *rq, struct task_struct *p)
{
}

/* 未配置高精度调度 tick：入队更新不执行任何操作。 */
static inline void hrtick_update(struct rq *rq)
{
}
#endif /* !CONFIG_SCHED_HRTICK */

/*
 * cpu_overutilized() - 判断 CPU 的 CFS 利用率是否超出其受 clamp 限制的容量。
 *
 * 业务背景：EAS 仅在根域未过载时按能耗模型放置任务，过载后转向容量均衡。
 * 入参：@cpu 是待评估 CPU。
 * 出参/返回：EAS 关闭返回 false；util 不适配有效容量时返回 true。
 * 注意事项：UCLAMP_MAX 会降低可用上限，判断复用 util_fits_cpu() 的容量语义。
 */
static inline bool cpu_overutilized(int cpu)
{
	unsigned long rq_util_max;

	if (!sched_energy_enabled())
		return false;

	rq_util_max = uclamp_rq_get(cpu_rq(cpu), UCLAMP_MAX);

	/* Return true only if the utilization doesn't fit CPU's capacity */
	/* 只有 CFS util 在 clamp 后容量上确实放不下，才认为 CPU 过载。 */
	return !util_fits_cpu(cpu_util_cfs(cpu), 0, rq_util_max, cpu);
}

/*
 * overutilized value make sense only if EAS is enabled
 */
/* overutilized 只在 EAS 启用时具有实际决策含义。 */
/*
 * is_rd_overutilized() - 读取根域是否应退出 EAS 快路。
 *
 * 业务背景：非 EAS 系统应始终采用传统负载均衡，等价视作已过载。
 * 入参：@rd 是待查询 root_domain。
 * 出参/返回：EAS 关闭或 @overutilized 已置位时返回 true。
 * 注意事项：READ_ONCE 与无锁写入协议配对，允许热路径近似读取。
 */
static inline bool is_rd_overutilized(struct root_domain *rd)
{
	return !sched_energy_enabled() || READ_ONCE(rd->overutilized);
}

/*
 * set_rd_overutilized() - 发布根域过载状态并发出调度 tracepoint。
 *
 * 业务背景：CPU 新增负载跨过容量线时，根域后续放置应停止依赖能耗最优假设。
 * 入参：@rd 是目标根域；@flag 是新状态。
 * 出参/返回：无；EAS 关闭时不写状态。
 * 注意事项：WRITE_ONCE 保证无锁观察者看到单次完整更新。
 */
static inline void set_rd_overutilized(struct root_domain *rd, bool flag)
{
	if (!sched_energy_enabled())
		return;

	WRITE_ONCE(rd->overutilized, flag);
	trace_sched_overutilized_tp(rd, flag);
}

/*
 * check_update_overutilized_status() - 入队后按需把根域提升为过载状态。
 *
 * 业务背景：根域由未过载转为过载后，均衡策略需立即切换；清除由周期均衡负责。
 * 入参：@rq 是刚更新负载的运行队列。
 * 出参/返回：无；仅在当前根域未过载且本 CPU 放不下 util 时置位。
 * 注意事项：这里只做单向 false->true，减少热路径同步成本。
 */
static inline void check_update_overutilized_status(struct rq *rq)
{
	/*
	 * overutilized field is used for load balancing decisions only
	 * if energy aware scheduler is being used
	 */
	/* 该根域标记只服务 EAS/传统均衡切换，非 EAS 时查询函数直接视为过载。 */

	if (!is_rd_overutilized(rq->rd) && cpu_overutilized(rq->cpu))
		set_rd_overutilized(rq->rd, 1);
}

/* Runqueue only has SCHED_IDLE tasks enqueued */
/* 运行队列非空且全部是 SCHED_IDLE 策略任务时，视为可被普通任务抢占的“空闲”。 */
/*
 * sched_idle_rq() - 判断 rq 是否只包含 SCHED_IDLE 公平任务。
 *
 * 业务背景：唤醒放置可优先选择这类 CPU，而不必等待其真正运行 idle 线程。
 * 入参：@rq 是待查询运行队列。
 * 出参/返回：nr_running 非零且等于公平层级 idle 实体数时返回非零。
 * 注意事项：真正空 rq 返回 0，由 available_idle_cpu() 单独识别。
 */
static int sched_idle_rq(struct rq *rq)
{
	return unlikely(rq->nr_running == rq->cfs.h_nr_idle &&
			rq->nr_running);
}

/*
 * choose_sched_idle_rq() - 判断任务是否适合占用仅有 SCHED_IDLE 任务的 rq。
 *
 * 业务背景：普通任务可抢占 SCHED_IDLE 工作，但 idle-policy 任务不应借此偏置。
 * 入参：@rq 是候选队列；@p 是待放置任务。
 * 出参/返回：队列仅含 idle-policy 任务且 @p 不是同策略时返回非零。
 * 注意事项：用于 CPU 选择启发式，不直接迁移或抢占任务。
 */
static int choose_sched_idle_rq(struct rq *rq, struct task_struct *p)
{
	return sched_idle_rq(rq) && !task_has_idle_policy(p);
}

/*
 * choose_idle_cpu() - 合并硬空闲与可抢占 SCHED_IDLE 两种候选判定。
 *
 * 业务背景：唤醒任务寻找低延迟 CPU 时，两类位置都无需挤压普通工作负载。
 * 入参：@cpu 是候选 CPU；@p 是待放置任务。
 * 出参/返回：CPU 可用空闲，或符合 sched-idle 抢占条件时返回非零。
 * 注意事项：不检查亲和性，调用者必须先限定候选 mask。
 */
static int choose_idle_cpu(int cpu, struct task_struct *p)
{
	return available_idle_cpu(cpu) ||
	       choose_sched_idle_rq(cpu_rq(cpu), p);
}

/*
 * requeue_delayed_entity() - 重新评估延迟出队实体并清除 delayed 状态。
 *
 * 业务背景：睡眠实体可能因负 lag 暂留 EEVDF 树，重新入队时需先收敛旧状态。
 * 入参：@se 是仍 on-rq 且标记 sched_delayed 的实体。
 * 出参/返回：无；必要时摘树、重新 place 后插回，并更新 PELT 与计数。
 * 注意事项：curr 不在 rb 树中，因此只调整计数和位置，不对其执行树操作。
 */
static void
requeue_delayed_entity(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	/*
	 * se->sched_delayed should imply: se->on_rq == 1.
	 * Because a delayed entity is one that is still on
	 * the runqueue competing until elegibility.
	 */
	/* delayed 表示实体仍以 on_rq 身份竞争到重新 eligible，而不是已经完全离队。 */
	WARN_ON_ONCE(!se->sched_delayed);
	WARN_ON_ONCE(!se->on_rq);

	if (update_entity_lag(cfs_rq, se)) {
		cfs_rq->nr_queued--;
		if (se != cfs_rq->curr)
			__dequeue_entity(cfs_rq, se);
		place_entity(cfs_rq, se, 0);
		if (se != cfs_rq->curr)
			__enqueue_entity(cfs_rq, se);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		cfs_rq->nr_queued++;
	}

	update_load_avg(cfs_rq, se, 0);
	clear_delayed(se);
}

/*
 * The enqueue_task method is called before nr_running is
 * increased. Here we update the fair scheduling stats and
 * then put the task into the rbtree:
 */
/* enqueue_task 在 core 增加 nr_running 前调用；先更新公平统计，再把实体加入红黑树。 */
/*
 * enqueue_task_fair() - 将任务及必要的组实体沿层级加入 CFS，并传播聚合统计。
 *
 * 业务背景：任务唤醒、新建或 delayed 重入队时，把叶实体的可运行性传到根 rq。
 * 入参：@rq 是持锁目标队列；@p 是任务；@flags 描述 wakeup/migrate/delayed 等原因。
 * 出参/返回：无；节流任务可能只进入 limbo，普通任务逐层入队并更新 rq 计数。
 * 注意事项：第一轮只到首个已 on-rq 祖先，第二轮继续传播 PELT/层级计数；
 * schedutil 观察前必须先加入 util_est，避免频率决策落后于新任务。
 */
static void
enqueue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;
	int h_nr_idle = task_has_idle_policy(p);
	int h_nr_runnable = 1;
	int task_new = !(flags & ENQUEUE_WAKEUP);
	int rq_h_nr_queued = rq->cfs.h_nr_queued;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	u64 slice = 0;

	if (task_is_throttled(p) && enqueue_throttled_task(p))
		return;

	/*
	 * The code below (indirectly) updates schedutil which looks at
	 * the cfs_rq utilization to select a frequency.
	 * Let's add the task's estimated utilization to the cfs_rq's
	 * estimated utilization, before we update schedutil.
	 */
	/* schedutil 会间接读取 cfs_rq，先合入任务估算 util，保证本次频率更新看到它。 */
	if (!p->se.sched_delayed || (flags & ENQUEUE_DELAYED))
		util_est_enqueue(&rq->cfs, p);

	if (flags & ENQUEUE_DELAYED) {
		requeue_delayed_entity(se);
		return;
	}

	/*
	 * If in_iowait is set, the code below may not trigger any cpufreq
	 * utilization updates, so do it here explicitly with the IOWAIT flag
	 * passed.
	 */
	/* I/O 唤醒可能没有其他 PELT 变化触发 cpufreq，显式发送 IOWAIT boost 提示。 */
	if (p->in_iowait)
		cpufreq_update_util(rq, SCHED_CPUFREQ_IOWAIT);

	if (task_new && se->sched_delayed)
		h_nr_runnable = 0;

	for_each_sched_entity(se) {
		if (se->on_rq) {
			if (se->sched_delayed)
				requeue_delayed_entity(se);
			break;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		}
		cfs_rq = cfs_rq_of(se);

		/*
		 * Basically set the slice of group entries to the min_slice of
		 * their respective cfs_rq. This ensures the group can service
		 * its entities in the desired time-frame.
		 */
		/* 子组实体继承其队列最小 slice，使父层为该组保留满足内部时限的服务窗口。 */
		if (slice) {
			se->slice = slice;
			se->custom_slice = 1;
		}
		enqueue_entity(cfs_rq, se, flags);
		slice = cfs_rq_min_slice(cfs_rq);

		cfs_rq->h_nr_runnable += h_nr_runnable;
		cfs_rq->h_nr_queued++;
		cfs_rq->h_nr_idle += h_nr_idle;

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (cfs_rq_is_idle(cfs_rq))
			h_nr_idle = 1;

		flags = ENQUEUE_WAKEUP;
	}

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		update_load_avg(cfs_rq, se, UPDATE_TG);
		se_update_runnable(se);
		update_cfs_group(se);

		se->slice = slice;
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (se != cfs_rq->curr)
			min_vruntime_cb_propagate(&se->run_node, NULL);
		slice = cfs_rq_min_slice(cfs_rq);

		cfs_rq->h_nr_runnable += h_nr_runnable;
		cfs_rq->h_nr_queued++;
		cfs_rq->h_nr_idle += h_nr_idle;

		if (cfs_rq_is_idle(cfs_rq))
			h_nr_idle = 1;
	}

	if (!rq_h_nr_queued && rq->cfs.h_nr_queued)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		dl_server_start(&rq->fair_server);

	/* At this point se is NULL and we are at root level*/
	/* 两轮均已越过最后祖先到达根层，此时才发布 rq 总任务数增加。 */
	add_nr_running(rq, 1);

	/*
	 * Since new tasks are assigned an initial util_avg equal to
	 * half of the spare capacity of their CPU, tiny tasks have the
	 * ability to cross the overutilized threshold, which will
	 * result in the load balancer ruining all the task placement
	 * done by EAS. As a way to mitigate that effect, do not account
	 * for the first enqueue operation of new tasks during the
	 * overutilized flag detection.
	 *
	 * A better way of solving this problem would be to wait for
	 * the PELT signals of tasks to converge before taking them
	 * into account, but that is not straightforward to implement,
	 * and the following generally works well enough in practice.
	 */
	/*
	 * 新任务首个 util_avg 按 CPU 空余容量的一半初始化，可能瞬时把根域误置过载并
	 * 破坏 EAS 放置；首入队跳过检测，等待后续真实 PELT 样本再判断。
	 */
	if (!task_new)
		check_update_overutilized_status(rq);

	assert_list_leaf_cfs_rq(rq);

	hrtick_update(rq);
}

/*
 * Basically dequeue_task_fair(), except it can deal with dequeue_entity()
 * failing half-way through and resume the dequeue later.
 *
 * Returns:
 * -1 - dequeue delayed
 *  0 - dequeue throttled
 *  1 - dequeue complete
 */
/*
 * dequeue_entities() - 逐层撤下实体，并允许 delayed 或 throttle 路径中途收敛。
 *
 * 业务背景：任务睡眠、迁移或节流时需撤销叶到根的可运行性，但负 lag 实体可能
 * 暂缓真正出树，父组有其他负载时也应停止向上撤除。
 * 入参：@rq 是持锁运行队列；@se 是起始实体；@flags 描述出队原因。
 * 出参/返回：-1 表示 delayed 暂留；0 预留节流中止语义；1 表示完整处理。
 * 注意事项：DEQUEUE_DELAYED 最终 __block_task() 可能释放 @p，之后严禁再引用。
 */
static int dequeue_entities(struct rq *rq, struct sched_entity *se, int flags)
{
	bool was_sched_idle = sched_idle_rq(rq);
	bool task_sleep = flags & DEQUEUE_SLEEP;
	bool task_delayed = flags & DEQUEUE_DELAYED;
	bool task_throttled = flags & DEQUEUE_THROTTLE;
	struct task_struct *p = NULL;
	int h_nr_idle = 0;
	int h_nr_queued = 0;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	int h_nr_runnable = 0;
	struct cfs_rq *cfs_rq;
	u64 slice = 0;

	if (entity_is_task(se)) {
		p = task_of(se);
		h_nr_queued = 1;
		h_nr_idle = task_has_idle_policy(p);
		if (task_sleep || task_delayed || !se->sched_delayed)
			h_nr_runnable = 1;
	}

	/* 遍历本层候选并逐项复验，循环结果汇入后续选择。 */
	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		if (!dequeue_entity(cfs_rq, se, flags)) {
			if (p && &p->se == se)
				return -1;

			slice = cfs_rq_min_slice(cfs_rq);
			break;
		}

		cfs_rq->h_nr_runnable -= h_nr_runnable;
		cfs_rq->h_nr_queued -= h_nr_queued;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		cfs_rq->h_nr_idle -= h_nr_idle;

		if (cfs_rq_is_idle(cfs_rq))
			h_nr_idle = h_nr_queued;

		if (throttled_hierarchy(cfs_rq) && task_throttled)
			record_throttle_clock(cfs_rq);

		/* Don't dequeue parent if it has other entities besides us */
		/* 当前 cfs_rq 仍有其他实体，组实体必须留在父队列，停止结构性向上出队。 */
		if (cfs_rq->load.weight) {
			slice = cfs_rq_min_slice(cfs_rq);

			/* Avoid re-evaluating load for this entity: */
			/* 父实体仍 on-rq，只需从这里开始传播统计，不应再次执行其 dequeue_entity。 */
			se = parent_entity(se);
			/*
			 * Bias pick_next to pick a task from this cfs_rq, as
			 * p is sleeping when it is within its sched_slice.
			 */
			/* 任务在 slice 内主动睡眠时偏置回该组，鼓励其醒来后完成未用服务窗口。 */
			if (task_sleep && se)
				set_next_buddy(se);
			break;
		}
		flags |= DEQUEUE_SLEEP;
		flags &= ~(DEQUEUE_DELAYED | DEQUEUE_SPECIAL);
	}

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		update_load_avg(cfs_rq, se, UPDATE_TG);
		se_update_runnable(se);
		update_cfs_group(se);

		se->slice = slice;
		if (se != cfs_rq->curr)
			min_vruntime_cb_propagate(&se->run_node, NULL);
		slice = cfs_rq_min_slice(cfs_rq);

		cfs_rq->h_nr_runnable -= h_nr_runnable;
		cfs_rq->h_nr_queued -= h_nr_queued;
		cfs_rq->h_nr_idle -= h_nr_idle;

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (cfs_rq_is_idle(cfs_rq))
			h_nr_idle = h_nr_queued;

		if (throttled_hierarchy(cfs_rq) && task_throttled)
			record_throttle_clock(cfs_rq);
	}

	sub_nr_running(rq, h_nr_queued);

	/* balance early to pull high priority tasks */
	/* rq 刚退化为仅有 SCHED_IDLE 任务，提前均衡以尽快拉取普通优先级工作。 */
	if (unlikely(!was_sched_idle && sched_idle_rq(rq)))
		rq->next_balance = jiffies;

	if (p && task_delayed) {
		WARN_ON_ONCE(!task_sleep);
		WARN_ON_ONCE(p->on_rq != 1);

		/*
		 * Fix-up what block_task() skipped.
		 *
		 * Must be last, @p might not be valid after this.
		 */
		/* 补做 block_task() 被 delayed 协议跳过的收尾；调用后任务对象可能失效。 */
		__block_task(rq, p);
	}

	return 1;
}

/*
 * The dequeue_task method is called before nr_running is
 * decreased. We remove the task from the rbtree and
 * update the fair scheduling stats:
 */
/* dequeue_task 在 core 减少 nr_running 前调用；从树移除并同步公平统计。 */
/*
 * dequeue_task_fair() - 处理任务级节流特殊态并委托层级实体出队。
 *
 * 业务背景：任务阻塞、迁移或策略变化时，统一撤销 util_est、EEVDF 与组统计。
 * 入参：@rq 是持锁旧队列；@p 是任务；@flags 描述 sleep/delayed/throttle 等原因。
 * 出参/返回：delayed 实体尚未真正出队返回 false，其余路径返回 true。
 * 注意事项：DEQUEUE_DELAYED 后 dequeue_entities() 可能使 @p 无效，返回前不得再读取。
 */
static bool dequeue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	if (task_is_throttled(p)) {
		dequeue_throttled_task(p, flags);
		return true;
	}

	if (!p->se.sched_delayed)
		util_est_dequeue(&rq->cfs, p);

	if (dequeue_entities(rq, &p->se, flags) < 0)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return false;

	/*
	 * Must not reference @p after dequeue_entities(DEQUEUE_DELAYED).
	 */
	/* delayed 收尾可能释放任务，函数剩余路径只返回常量。 */
	return true;
}

/*
 * cfs_h_nr_delayed() - 计算根 CFS 层级中仍排队但已不算 runnable 的实体数。
 *
 * 业务背景：EEVDF delayed dequeue 保持实体在队列结构中直到 lag 收敛。
 * 入参：@rq 是待查询运行队列。
 * 出参/返回：层级 queued 与 runnable 计数之差。
 * 注意事项：依赖两计数在 rq 锁下的一致快照。
 */
static inline unsigned int cfs_h_nr_delayed(struct rq *rq)
{
	return (rq->cfs.h_nr_queued - rq->cfs.h_nr_runnable);
}

/* Working cpumask for: sched_balance_rq(), sched_balance_newidle(). */
/* 该工作 mask 由周期均衡和新空闲均衡共同复用。 */
static DEFINE_PER_CPU(cpumask_var_t, load_balance_mask);
static DEFINE_PER_CPU(cpumask_var_t, select_rq_mask);
static DEFINE_PER_CPU(cpumask_var_t, should_we_balance_tmpmask);

#ifdef CONFIG_NO_HZ_COMMON

static struct {
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	cpumask_var_t idle_cpus_mask;
	int has_blocked_load;		/* Idle CPUS has blocked load */
	int needs_update;		/* Newly idle CPUs need their next_balance collated */
	unsigned long next_balance;     /* in jiffy units */
	unsigned long next_blocked;	/* Next update of blocked load in jiffies */
/* next_blocked 以 jiffies 表示下一次 blocked load 更新时间。 */
} nohz ____cacheline_aligned;

#endif /* CONFIG_NO_HZ_COMMON */

/*
 * cpu_load() - 读取 CPU 根 CFS 队列的 PELT load_avg。
 *
 * 业务背景：负载均衡以权重化历史负载比较源/目标 CPU。
 * 入参：@rq 是待查询运行队列。
 * 出参/返回：根 cfs_rq 的 load_avg。
 * 注意事项：这是近似信号，是否需要先刷新 PELT 由调用路径保证。
 */
static unsigned long cpu_load(struct rq *rq)
{
	return cfs_rq_load_avg(&rq->cfs);
}

/*
 * cpu_load_without - compute CPU load without any contributions from *p
 * @cpu: the CPU which load is requested
 * @p: the task which load should be discounted
 *
 * The load of a CPU is defined by the load of tasks currently enqueued on that
 * CPU as well as tasks which are currently sleeping after an execution on that
 * CPU.
 *
 * This method returns the load of the specified CPU by discounting the load of
 * the specified task, whenever the task is currently contributing to the CPU
 * load.
 */
/*
 * cpu_load_without() - 估算移除指定任务后的 CPU PELT 负载。
 *
 * 业务背景：唤醒放置/迁移比较边际效果时，任务不能同时计入源 CPU 和候选 CPU。
 * 入参：@rq 是待估算 CPU；@p 是需要扣除贡献的任务。
 * 出参/返回：若任务当前贡献该 CPU，返回扣除 task_h_load 后的非负 load；否则原值。
 * 注意事项：睡眠任务仍可能通过 PELT 衰减贡献负载，last_update_time 用于识别新任务。
 */
static unsigned long cpu_load_without(struct rq *rq, struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	unsigned int load;

	/* Task has no contribution or is new */
	/* 任务不属于该 CPU，或尚无 PELT 样本时，没有可扣除贡献。 */
	if (cpu_of(rq) != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_load(rq);

	cfs_rq = &rq->cfs;
	load = READ_ONCE(cfs_rq->avg.load_avg);

	/* Discount task's util from CPU's util */
	/* task_h_load 包含组层级折算；饱和减法防止并发近似造成下溢。 */
	lsub_positive(&load, task_h_load(p));

	return load;
}

/*
 * cpu_runnable() - 读取 CPU 根 CFS 队列的 runnable_avg。
 *
 * 业务背景：runnable 信号刻画等待/运行需求，与实际 running util 分工。
 * 入参：@rq 是待查询运行队列。
 * 出参/返回：根 cfs_rq 的 runnable_avg。
 * 注意事项：值按 PELT 衰减，属于负载均衡近似输入。
 */
static unsigned long cpu_runnable(struct rq *rq)
{
	return cfs_rq_runnable_avg(&rq->cfs);
}

/*
 * cpu_runnable_without() - 估算扣除指定任务后的 CPU runnable 信号。
 *
 * 业务背景：比较任务迁移前后排队压力时避免重复计算任务自身。
 * 入参：@rq 是候选 CPU；@p 是待扣除任务。
 * 出参/返回：任务有贡献时做非负扣减，否则返回原 runnable_avg。
 * 注意事项：只在任务 CPU 与 last_update_time 表明其确有贡献时扣除。
 */
static unsigned long cpu_runnable_without(struct rq *rq, struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	unsigned int runnable;

	/* Task has no contribution or is new */
	/* 任务不属于该 CPU 或尚无 PELT 样本时，直接保留原信号。 */
	if (cpu_of(rq) != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_runnable(rq);

	cfs_rq = &rq->cfs;
	runnable = READ_ONCE(cfs_rq->avg.runnable_avg);

	/* Discount task's runnable from CPU's runnable */
	/* 用饱和减法扣除任务 runnable_avg，容忍并发采样造成的轻微不一致。 */
	lsub_positive(&runnable, p->se.avg.runnable_avg);

	return runnable;
}

/*
 * capacity_of() - 读取 CPU 当前经架构/热约束修正后的调度容量。
 *
 * 业务背景：跨 CPU 比较负载时必须按异构容量归一化。
 * 入参：@cpu 是目标 CPU。
 * 出参/返回：rq->cpu_capacity。
 * 注意事项：这是调度器维护的动态容量，不等同于硬件最大频率。
 */
static unsigned long capacity_of(int cpu)
{
	return cpu_rq(cpu)->cpu_capacity;
}

/*
 * record_wakee() - 记录当前唤醒者切换被唤醒伙伴的频率。
 *
 * 业务背景：wake_wide() 用伙伴切换次数识别 dispatcher-to-workers 等 M:N 模式。
 * 入参：@p 是本次被 current 唤醒的任务。
 * 出参/返回：无；每秒衰减历史计数，伙伴变化时累计一次 flip。
 * 注意事项：只记录身份切换而非总唤醒数，以区分稳定的一对一关系。
 */
static void record_wakee(struct task_struct *p)
{
	/*
	 * Only decay a single time; tasks that have less then 1 wakeup per
	 * jiffy will not have built up many flips.
	 */
	/* 每个衰减窗口最多右移一次；低于每 jiffy 一次的任务本就难以积累大量 flip。 */
	if (time_after(jiffies, current->wakee_flip_decay_ts + HZ)) {
		current->wakee_flips >>= 1;
		current->wakee_flip_decay_ts = jiffies;
	}

	if (current->last_wakee != p) {
		current->last_wakee = p;
		current->wakee_flips++;
	}
}

/*
 * Detect M:N waker/wakee relationships via a switching-frequency heuristic.
 *
 * A waker of many should wake a different task than the one last awakened
 * at a frequency roughly N times higher than one of its wakees.
 *
 * In order to determine whether we should let the load spread vs consolidating
 * to shared cache, we look for a minimum 'flip' frequency of llc_size in one
 * partner, and a factor of lls_size higher frequency in the other.
 *
 * With both conditions met, we can be relatively sure that the relationship is
 * non-monogamous, with partner count exceeding socket size.
 *
 * Waker/wakee being client/server, worker/dispatcher, interrupt source or
 * whatever is irrelevant, spread criteria is apparent partner count exceeds
 * socket size.
 */
/*
 * wake_wide() - 用唤醒伙伴切换频率判断是否应跨 LLC 扩散负载。
 *
 * 业务背景：一对一唤醒倾向共享缓存；一个调度者唤醒大量 worker 时过度聚合会拥塞。
 * 入参：@p 是 wakee，current 是隐式 waker。
 * 出参/返回：双方 flip 满足 LLC 大小门槛及倍数关系时返回 1，否则返回 0。
 * 注意事项：这是与业务角色无关的启发式，只近似推断伙伴数是否超过 socket 容量。
 */
static int wake_wide(struct task_struct *p)
{
	unsigned int master = current->wakee_flips;
	unsigned int slave = p->wakee_flips;
	int factor = __this_cpu_read(sd_llc_size);

	if (master < slave)
		swap(master, slave);
	if (slave < factor || master < slave * factor)
		return 0;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return 1;
}

/*
 * The purpose of wake_affine() is to quickly determine on which CPU we can run
 * soonest. For the purpose of speed we only consider the waking and previous
 * CPU.
 *
 * wake_affine_idle() - only considers 'now', it check if the waking CPU is
 *			cache-affine and is (or	will be) idle.
 *
 * wake_affine_weight() - considers the weight to reflect the average
 *			  scheduling latency of the CPUs. This seems to work
 *			  for the overloaded case.
 */
/* wake_affine 只比较唤醒 CPU 与任务旧 CPU：idle 分支看立即可运行性，weight 分支看平均等待。 */
/*
 * wake_affine_idle() - 在 this_cpu 与 prev_cpu 间按空闲和同步唤醒选择。
 *
 * 业务背景：唤醒热路径需要低成本决定是否保持缓存亲和或就近运行。
 * 入参：@this_cpu 是 waker CPU；@prev_cpu 是 wakee 旧 CPU；@sync 表示同步唤醒。
 * 出参/返回：选中的 CPU；无明确选择返回 nr_cpumask_bits 哨兵。
 * 注意事项：中断上下文只在共享缓存时迁到 this_cpu，避免 IRQ 拓扑把任务挤到单节点。
 */
static int
wake_affine_idle(int this_cpu, int prev_cpu, int sync)
{
	/*
	 * If this_cpu is idle, it implies the wakeup is from interrupt
	 * context. Only allow the move if cache is shared. Otherwise an
	 * interrupt intensive workload could force all tasks onto one
	 * node depending on the IO topology or IRQ affinity settings.
	 *
	 * If the prev_cpu is idle and cache affine then avoid a migration.
	 * There is no guarantee that the cache hot data from an interrupt
	 * is more important than cache hot data on the prev_cpu and from
	 * a cpufreq perspective, it's better to have higher utilisation
	 * on one CPU.
	 */
	/*
	 * this_cpu 空闲常源自中断唤醒，仅共享缓存才迁移；旧 CPU 也空闲则优先保留。
	 * 这同时保护旧缓存热度，并倾向集中利用率以利于 cpufreq。
	 */
	if (available_idle_cpu(this_cpu) && cpus_share_cache(this_cpu, prev_cpu))
		return available_idle_cpu(prev_cpu) ? prev_cpu : this_cpu;

	if (sync) {
		struct rq *rq = cpu_rq(this_cpu);

		if ((rq->nr_running - cfs_h_nr_delayed(rq)) == 1)
			return this_cpu;
	}

	if (available_idle_cpu(prev_cpu))
		return prev_cpu;

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return nr_cpumask_bits;
}

/*
 * wake_affine_weight() - 用容量归一化有效负载比较唤醒 CPU 与旧 CPU。
 *
 * 业务背景：两端都忙时，idle 启发式失效，需要选择预期调度延迟更低的一端。
 * 入参：@sd 提供失衡阈值；@p 是 wakee；两个 CPU 是候选；@sync 表示 waker 将睡眠。
 * 出参/返回：this_cpu 更优时返回它，否则返回 nr_cpumask_bits 让上层保留 prev_cpu。
 * 注意事项：同步唤醒从 this_cpu 扣除 current 负载；WA_BIAS 为迁移设置迟滞。
 */
static int
wake_affine_weight(struct sched_domain *sd, struct task_struct *p,
		   int this_cpu, int prev_cpu, int sync)
{
	s64 this_eff_load, prev_eff_load;
	unsigned long task_load;

	this_eff_load = cpu_load(cpu_rq(this_cpu));

	if (sync) {
		unsigned long current_load = task_h_load(current);

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (current_load > this_eff_load)
			return this_cpu;

		this_eff_load -= current_load;
	}

	task_load = task_h_load(p);

	this_eff_load += task_load;
	if (sched_feat(WA_BIAS))
		this_eff_load *= 100;
	this_eff_load *= capacity_of(prev_cpu);

	prev_eff_load = cpu_load(cpu_rq(prev_cpu));
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	prev_eff_load -= task_load;
	if (sched_feat(WA_BIAS))
		prev_eff_load *= 100 + (sd->imbalance_pct - 100) / 2;
	prev_eff_load *= capacity_of(this_cpu);

	/*
	 * If sync, adjust the weight of prev_eff_load such that if
	 * prev_eff == this_eff that select_idle_sibling() will consider
	 * stacking the wakee on top of the waker if no other CPU is
	 * idle.
	 */
	/* 同步唤醒平局时轻微偏向叠放 wakee/waker，便于无其他空闲 CPU 时保持亲和。 */
	if (sync)
		prev_eff_load += 1;

	return this_eff_load < prev_eff_load ? this_cpu : nr_cpumask_bits;
}

/*
 * wake_affine() - 组合 idle 与 weight 启发式并记录唤醒亲和统计。
 *
 * 业务背景：在深入调度域搜索前，快速尝试将 wakee 放在 waker 或旧 CPU。
 * 入参：@sd 是共享域；@p 是 wakee；@this_cpu/@prev_cpu 是两候选；@sync 是同步提示。
 * 出参/返回：仅启发式明确选择 this_cpu 时迁移，否则返回 prev_cpu。
 * 注意事项：特性开关决定启用的子策略；失败哨兵不会泄漏给调用者。
 */
static int wake_affine(struct sched_domain *sd, struct task_struct *p,
		       int this_cpu, int prev_cpu, int sync)
{
	int target = nr_cpumask_bits;

	if (sched_feat(WA_IDLE))
		target = wake_affine_idle(this_cpu, prev_cpu, sync);

	if (sched_feat(WA_WEIGHT) && target == nr_cpumask_bits)
		target = wake_affine_weight(sd, p, this_cpu, prev_cpu, sync);

	schedstat_inc(p->stats.nr_wakeups_affine_attempts);
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (target != this_cpu)
		return prev_cpu;

	schedstat_inc(sd->ttwu_move_affine);
	schedstat_inc(p->stats.nr_wakeups_affine);
	return target;
}

static struct sched_group *
sched_balance_find_dst_group(struct sched_domain *sd, struct task_struct *p, int this_cpu);

/*
 * sched_balance_find_dst_group_cpu - find the idlest CPU among the CPUs in the group.
 */
/* 在给定调度组与任务亲和性交集中寻找最空闲 CPU。 */
/*
 * sched_balance_find_dst_group_cpu() - 在已选目标组中确定具体 CPU。
 *
 * 业务背景：组级均衡先选调度组，再在组内优先低唤醒延迟/热缓存的空闲 CPU。
 * 入参：@group 是目标组；@p 是待放置任务；@this_cpu 是默认回退 CPU。
 * 出参/返回：优先最浅 idle，其次最近 idle，最后最小 load 的允许 CPU。
 * 注意事项：跳过 core-cookie 不兼容 CPU，并始终受 @p->cpus_ptr 约束。
 */
static int
sched_balance_find_dst_group_cpu(struct sched_group *group, struct task_struct *p, int this_cpu)
{
	unsigned long load, min_load = ULONG_MAX;
	unsigned int min_exit_latency = UINT_MAX;
	u64 latest_idle_timestamp = 0;
	int least_loaded_cpu = this_cpu;
	int shallowest_idle_cpu = -1;
	int i;

	/* Check if we have any choice: */
	/* 单 CPU 组没有内部选择空间，直接返回其唯一成员。 */
	if (group->group_weight == 1)
		return cpumask_first(sched_group_span(group));

	/* Traverse only the allowed CPUs */
	/* 同时遍历组 span 与任务亲和性，禁止越界放置。 */
	for_each_cpu_and(i, sched_group_span(group), p->cpus_ptr) {
		struct rq *rq = cpu_rq(i);

		if (!sched_core_cookie_match(rq, p))
			continue;

		if (choose_sched_idle_rq(rq, p))
			return i;

		if (available_idle_cpu(i)) {
			struct cpuidle_state *idle = idle_get_state(rq);
			if (idle && idle->exit_latency < min_exit_latency) {
				/*
				 * We give priority to a CPU whose idle state
				 * has the smallest exit latency irrespective
				 * of any idle timestamp.
				 */
				/* 更浅 idle 状态退出更快，其优先级高于缓存热度时间戳。 */
				min_exit_latency = idle->exit_latency;
				latest_idle_timestamp = rq->idle_stamp;
				shallowest_idle_cpu = i;
			} else if ((!idle || idle->exit_latency == min_exit_latency) &&
				   rq->idle_stamp > latest_idle_timestamp) {
				/*
				 * If equal or no active idle state, then
				 * the most recently idled CPU might have
				 * a warmer cache.
				 */
				/* 退出延迟相同或未知时，最近进入 idle 的 CPU 更可能保留热缓存。 */
				latest_idle_timestamp = rq->idle_stamp;
				shallowest_idle_cpu = i;
			}
		} else if (shallowest_idle_cpu == -1) {
			load = cpu_load(cpu_rq(i));
			if (load < min_load) {
				min_load = load;
				least_loaded_cpu = i;
			}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	return shallowest_idle_cpu != -1 ? shallowest_idle_cpu : least_loaded_cpu;
}

/*
 * sched_balance_find_dst_cpu() - 沿调度域层级为唤醒/新建任务细化目标 CPU。
 *
 * 业务背景：select_task_rq_fair() 已给出起始 CPU 与起始调度域后，本函数在每一级先选
 * 最合适的 sched_group，再在组内选具体 CPU；一旦跨到新 CPU，搜索会转到该 CPU 的较低层
 * 调度域继续细化，避免拿旧 CPU 的拓扑层级解释新目标。
 * 入参：@sd 是借用的起始调度域，必须处于调用者的 RCU 读侧保护内；@p 是待放置任务的
 * 借用指针，其亲和性由 @p->cpus_ptr 给出；@cpu 是当前候选 CPU；@prev_cpu 是无可用域时
 * 保持亲和性的回退 CPU；@sd_flag 指明本次是 wake/fork/exec 等哪类均衡请求。
 * 出参/返回：返回最终候选 CPU；若起始域与任务允许掩码无交集则返回 @prev_cpu。函数不入队
 * 任务、不转移引用，只可能同步 @p->se 的 PELT 视图。
 * 注意事项：调用者必须保证调度域拓扑在 RCU 读侧窗口内有效；本函数不睡眠。域搜索只经过
 * 同时声明支持 @sd_flag 的层级，返回值仍受任务亲和性和调度组选择结果约束。
 */
static inline int sched_balance_find_dst_cpu(struct sched_domain *sd, struct task_struct *p,
				  int cpu, int prev_cpu, int sd_flag)
{
	/* new_cpu 保存最近一次成功选择；尚未改善时与入口 @cpu 相同。 */
	int new_cpu = cpu;

	/* 起始域完全不含允许 CPU 时，继续搜索没有意义，并保持任务原驻留位置。 */
	if (!cpumask_intersects(sched_domain_span(sd), p->cpus_ptr))
		return prev_cpu;

	/*
	 * We need task's util for cpu_util_without, sync it up to
	 * prev_cpu's last_update_time.
	 */
	/*
	 * 后续组选择会用 cpu_util_without() 比较“拿走 p 后”的负载，因此先把 p 的 PELT
	 * 平均值推进到 prev_cpu 的时间基准。fork 任务尚无可同步的历史负载，跳过该步骤。
	 */
	if (!(sd_flag & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	/*
	 * 从较大的域向 child 域收敛：每轮先完成组级选择，再把结果落实为 CPU；若目标跨域，
	 * 便以新 CPU 的拓扑重新寻找小于当前 span 的下一层，保证层级关系始终属于当前候选。
	 */
	while (sd) {
		/* group/tmp 是借用的拓扑节点；weight 固定本轮域跨度，作为下降搜索的上界。 */
		struct sched_group *group;
		struct sched_domain *tmp;
		int weight;

		/* 不支持本次均衡类型的域不能参与决策，但其 child 仍可能支持。 */
		if (!(sd->flags & sd_flag)) {
			sd = sd->child;
			continue;
		}

		/* 组级策略没有给出更佳组时，缩小到当前 CPU 的下一层拓扑继续尝试。 */
		group = sched_balance_find_dst_group(sd, p, cpu);
		if (!group) {
			sd = sd->child;
			continue;
		}

		new_cpu = sched_balance_find_dst_group_cpu(group, p, cpu);
		if (new_cpu == cpu) {
			/* Now try balancing at a lower domain level of 'cpu': */
			/* 目标未改变；在同一 CPU 的更小域内继续寻找更精细的放置机会。 */
			sd = sd->child;
			continue;
		}

		/* Now try balancing at a lower domain level of 'new_cpu': */
		/*
		 * 目标已经跨到 new_cpu；从它自己的域链重新定位比当前域更小且支持 sd_flag 的层级。
		 * 将 sd 先清空也构成自然终止条件：若找不到这种子层，当前 new_cpu 就是最终结果。
		 */
		cpu = new_cpu;
		weight = sd->span_weight;
		sd = NULL;
		for_each_domain(cpu, tmp) {
			if (weight <= tmp->span_weight)
				break;
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
			if (tmp->flags & sd_flag)
				sd = tmp;
		}
	}

	return new_cpu;
}

/*
 * __select_idle_cpu() - 验证单个 CPU 是否同时可接收任务并满足 core-scheduling cookie。
 *
 * 业务背景：LLC 空闲 CPU 扫描把单点判断收敛到这里，避免只看到 idle 却把不同安全域的任务
 * 放到同一物理核。它是 select_idle_cpu() 的叶子判定，不负责遍历拓扑。
 * 入参：@cpu 是待验证的逻辑 CPU；@p 是只读借用的待唤醒任务，必须在调用期间保持有效。
 * 出参/返回：两项条件均满足时返回 @cpu，否则返回 -1；不修改运行队列或任务状态。
 * 注意事项：该函数不加锁、不睡眠；返回成功只是候选快照，真正 enqueue 前仍由上层锁协议
 * 处理并发变化。cookie 检查用于 core scheduling，不能由普通 idle 判断替代。
 */
static inline int __select_idle_cpu(int cpu, struct task_struct *p)
{
	/* choose_idle_cpu() 处理可用/调度空闲语义，cookie 检查补上 SMT 同核隔离约束。 */
	if (choose_idle_cpu(cpu, p) && sched_cpu_cookie_match(cpu_rq(cpu), p))
		return cpu;

	return -1;
}

/*
 * sched_smt_present 是 SMT 拓扑存在性的静态分支键：拓扑代码在出现 SMT 时启用它，调度热路径
 * 据此跳过非 SMT 机器上的 sibling/core 扫描成本。导出符号允许其他调度代码复用同一全局判断；
 * static key 的修改由拓扑更新路径串行化，普通读侧只执行低开销分支判断。
 */
DEFINE_STATIC_KEY_FALSE(sched_smt_present);
EXPORT_SYMBOL_GPL(sched_smt_present);

/*
 * set_idle_cores() - 发布某 LLC 调度域内是否仍可能存在整颗空闲 SMT 核的提示。
 *
 * 业务背景：空闲 CPU 搜索用共享提示决定是否值得执行更昂贵的整核扫描；该值是可失真的
 * 性能提示而非正确性状态，后续 update 路径可重新置位。
 * 入参：@cpu 用于定位其共享调度域对象；@val 为要发布的布尔语义值，0 表示当前扫描未找到
 * 空闲整核，非 0 表示可能存在。两者均为纯输入。
 * 出参/返回：无直接返回值；若共享对象存在，以 WRITE_ONCE 更新 has_idle_cores，否则无副作用。
 * 注意事项：调用者须保证 sd_balance_shared 指针的生命周期，例如处于 RCU 保护或拓扑稳定期；
 * 本函数不睡眠。WRITE_ONCE 防止编译器拆分/合并访问，但不把该提示升级为锁保护的一致快照。
 */
static inline void set_idle_cores(int cpu, int val)
{
	/* sds 是 per-CPU 槽中共享域对象的借用指针，不取得长期引用。 */
	struct sched_domain_shared *sds;

	sds = rcu_dereference_all(per_cpu(sd_balance_shared, cpu));
	/* 拓扑尚未建立或正在重建时允许对象为空；此时宁可丢失提示，也不能解引用空指针。 */
	if (sds)
		WRITE_ONCE(sds->has_idle_cores, val);
}

/*
 * test_idle_cores() - 读取某 LLC 域的“可能存在整颗空闲核”提示。
 *
 * 业务背景：select_idle_cpu()/select_idle_capacity() 用它决定按物理核优先扫描还是直接按线程
 * 搜索；false 只用于削减扫描，不影响任务可运行性的正确性。
 * 入参：@cpu 是用于查找共享域对象的逻辑 CPU，纯输入。
 * 出参/返回：共享对象存在时返回 has_idle_cores 的单次快照；对象不存在时返回 false；无副作用。
 * 注意事项：返回值可能在读取后立即过期，调用者不得把它当锁或生命周期保证；调用环境仍须
 * 保证 sds 可安全解引用，本函数不睡眠、不取得引用。
 */
static inline bool test_idle_cores(int cpu)
{
	/* sds 仅在当前拓扑保护窗口内借用。 */
	struct sched_domain_shared *sds;

	sds = rcu_dereference_all(per_cpu(sd_balance_shared, cpu));
	/* READ_ONCE 与写侧 WRITE_ONCE 配对保证一次标量访问，但不保证随后各 CPU 状态保持不变。 */
	if (sds)
		return READ_ONCE(sds->has_idle_cores);

	return false;
}

/*
 * Scans the local SMT mask to see if the entire core is idle, and records this
 * information in sd_balance_shared->has_idle_cores.
 *
 * Since SMT siblings share all cache levels, inspecting this limited remote
 * state should be fairly cheap.
 */
/*
 * 扫描当前 CPU 的 SMT sibling，确认整颗物理核都空闲后，把结果写入共享域的
 * has_idle_cores 提示。SMT 线程共享缓存层级，因此这里只读取同核少量远端运行状态，成本通常
 * 低于后续遍历整个 LLC 域。
 *
 * 业务背景：update_idle_core() 在运行队列进入空闲状态时调用本函数，重新打开此前因扫描失败
 * 而关闭的“优先找空闲整核”优化，使唤醒任务有机会独占执行资源而不是与 sibling 争用。
 * 入参：@rq 是刚进入相关空闲更新路径的运行队列借用指针，不可为空；函数从它取得逻辑 CPU。
 * 出参/返回：无直接返回值；仅当所有 SMT sibling 都可用且空闲时，把共享提示置 1，否则保持
 * 原值。没有引用或 ownership 转移。
 * 注意事项：调用者的 rq 锁契约由 update_idle_core() 保证；函数自身进入 RCU 读侧保护拓扑对象，
 * 不睡眠。CPU 状态是逐个观察的快照，因此该提示只减少搜索成本，不承诺返回时整核仍然空闲。
 */
void __update_idle_core(struct rq *rq)
{
	/* core 是本 rq 的逻辑 CPU，也是定位其 SMT mask/共享域的锚点；cpu 为 sibling 游标。 */
	int core = cpu_of(rq);
	int cpu;

	/* RCU 保证扫描期间 SMT mask 与 sd_balance_shared 对象不会被拓扑重建路径释放。 */
	rcu_read_lock();
	/* 已经有“可能存在空闲核”的提示时无需重复扫描。 */
	if (test_idle_cores(core))
		goto unlock;

	/* 任一其他硬件线程仍忙，就不能把这颗物理核作为完整空闲核发布。 */
	for_each_cpu(cpu, cpu_smt_mask(core)) {
		if (cpu == core)
			continue;

		if (!available_idle_cpu(cpu))
			goto unlock;
	}

	/* 所有 sibling 均通过快照检查，发布正向提示供 LLC 扫描路径消费。 */
	set_idle_cores(core, 1);
unlock:
	/* 无论快速跳过还是扫描失败，都在同一出口结束拓扑生命周期保护。 */
	rcu_read_unlock();
}

/*
 * Scan the entire LLC domain for idle cores; this dynamically switches off if
 * there are no idle cores left in the system; tracked through
 * sd_balance_shared->has_idle_cores and enabled through update_idle_core()
 * above.
 */
/*
 * 在 LLC 域扫描某个 SMT 核的全部硬件线程；若整核空闲则立即返回可用 CPU。扫描若发现该核
 * 并非全空闲，会把整颗核从临时候选掩码删除，避免外层循环对同一 sibling 集合重复工作。
 * has_idle_cores 为 false 时外层会关闭这种整核扫描；CPU 再次进入空闲后由 update_idle_core()
 * 重新置位提示。
 *
 * 业务背景：select_idle_cpu() 在共享提示表明 LLC 内可能有完整空闲核时调用本函数，优先减少
 * SMT 资源争用；即使整核不空闲，也可通过 @idle_cpu 保留一个调度意义上可用的 idle sibling。
 * 入参：@p 是待放置任务的只读借用指针；@core 是待检查物理核中的一个逻辑 CPU；@cpus 是
 * 输入输出的本 CPU 临时掩码，初始为域 span 与任务亲和性的交集；@idle_cpu 是输入输出结果槽，
 * -1 表示尚无回退候选，非 -1 时保留先前候选。所有指针均不发生 ownership 转移。
 * 出参/返回：整核空闲时返回 @core；否则返回 -1，并可能写入 *@idle_cpu，同时从 @cpus 清除
 * 该核的全部 sibling。函数不入队任务。
 * 注意事项：调用者处于调度域 RCU 读侧窗口且使用 per-CPU 临时 cpumask；函数不睡眠。逐 CPU
 * idle 状态可能并发变化，返回值只是放置候选，最终入队仍需运行队列锁下重新建立状态。
 */
static int select_idle_core(struct task_struct *p, int core, struct cpumask *cpus, int *idle_cpu)
{
	/* idle 汇总“截至当前是否全核空闲”；cpu 遍历 core 的 SMT sibling。 */
	bool idle = true;
	int cpu;

	/*
	 * 阶段 1：逐 sibling 检查整核条件，并在第一次遇到忙 CPU 时尽量保存一个仍可调度的
	 * SCHED_IDLE/空闲回退线程；已有回退候选后无需继续细查这个非空闲核。
	 */
	for_each_cpu(cpu, cpu_smt_mask(core)) {
		if (!available_idle_cpu(cpu)) {
			idle = false;
			if (*idle_cpu == -1) {
				if (choose_sched_idle_rq(cpu_rq(cpu), p) &&
				    cpumask_test_cpu(cpu, cpus)) {
					*idle_cpu = cpu;
					break;
				}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
				continue;
			}
			break;
		}
		if (*idle_cpu == -1 && cpumask_test_cpu(cpu, cpus))
			*idle_cpu = cpu;
	}

	/* 阶段 2：全核空闲时立即成功，外层可停止 LLC 扫描并优先使用这颗核。 */
	if (idle)
		return core;

	/* 非全空闲核已完整处理，从工作集删去所有 sibling，防止外层再次选它们作为核首项。 */
	cpumask_andnot(cpus, cpus, cpu_smt_mask(core));
	return -1;
}

/*
 * Scan the local SMT mask for idle CPUs.
 */
/* 扫描目标 CPU 的本地 SMT sibling，寻找任务亲和性与 LLC 域都允许的空闲硬件线程。 */
/*
 * select_idle_smt() - 在目标物理核内为 wakee 寻找空闲 sibling。
 *
 * 业务背景：select_idle_sibling() 的 target/prev 快速路径未命中后，先做局部 SMT 搜索通常比
 * 扫描整个 LLC 便宜，并能保持缓存亲和性。
 * 入参：@p 是待唤醒任务的只读借用指针；@sd 是 @target 所在 LLC 调度域的借用指针；
 * @target 是作为 SMT mask 锚点且已检查过的逻辑 CPU。参数均不转移引用。
 * 出参/返回：返回第一个同时属于任务亲和性、@sd 域且 choose_idle_cpu() 接受的 sibling；
 * 无候选时返回 -1；无状态副作用。
 * 注意事项：调用者必须在 RCU 读侧保护下保持 @sd 有效；函数不睡眠。idle 结果可能并发失效，
 * 且该局部 helper 不做 core cookie 检查，后续路径仍受真正入队时的调度约束。
 */
static int select_idle_smt(struct task_struct *p, struct sched_domain *sd, int target)
{
	/* cpu 是 p 允许 CPU 与 target SMT mask 交集上的遍历游标。 */
	int cpu;

	for_each_cpu_and(cpu, cpu_smt_mask(target), p->cpus_ptr) {
		/* target 已被上层检查且未命中，本轮只考察其余硬件线程。 */
		if (cpu == target)
			continue;
		/*
		 * Check if the CPU is in the LLC scheduling domain of @target.
		 * Due to isolcpus, there is no guarantee that all the siblings are in the domain.
		 */
		/*
		 * 必须再次确认 sibling 属于 target 的 LLC 调度域；isolcpus 可能把同一物理核的线程
		 * 排除在该域之外，不能仅凭 cpu_smt_mask() 就跨隔离边界放置任务。
		 */
		if (!cpumask_test_cpu(cpu, sched_domain_span(sd)))
			continue;
		/* 第一个满足 idle 语义的 sibling 即完成局部快速路径，不再扩大扫描范围。 */
		if (choose_idle_cpu(cpu, p))
			return cpu;
	}

	return -1;
}

/*
 * Scan the LLC domain for idle CPUs; this is dynamically regulated by
 * comparing the average scan cost (tracked in sd->avg_scan_cost) against the
 * average idle time for this rq (as found in rq->avg_idle).
 */
/*
 * 扫描目标 CPU 的 LLC 域寻找空闲 CPU。扫描规模不是固定全遍历：调度器比较历史平均扫描成本
 * sd->avg_scan_cost 与当前运行队列平均空闲时长 rq->avg_idle，折算为 sd->shared->nr_idle_scan，
 * 在唤醒延迟收益与搜索开销之间动态取舍。
 *
 * 业务背景：select_idle_sibling() 的近邻快速路径未命中后调用这里；若共享提示认为存在整颗
 * 空闲 SMT 核，则按核搜索以避免 sibling 争用，否则在预算内寻找第一个可接受的空闲线程。
 * 入参：@p 是待唤醒任务的只读借用指针；@sd 是 @target 所在 LLC 域的借用指针；
 * @has_idle_core 表示是否值得优先扫描完整 SMT 核；@target 是循环起点和共享提示定位 CPU。
 * 出参/返回：找到候选时返回其 CPU 编号；预算耗尽或无候选时返回 -1，但整核模式可能返回
 * @idle_cpu 保存的部分空闲回退线程。函数不迁移、不入队任务，也不转移引用。
 * 注意事项：调用者必须在 RCU 读侧保护 @sd 及其 shared 拓扑，并保证本 CPU 临时 cpumask 不被
 * 同路径递归复用；函数不睡眠。idle/cookie 结果是无 rq 锁快照，最终 enqueue 仍需上层同步。
 */
static int select_idle_cpu(struct task_struct *p, struct sched_domain *sd, bool has_idle_core, int target)
{
	/*
	 * cpus 是当前 CPU 专用的可破坏工作掩码；idle_cpu 保存整核扫描期间发现的线程级回退。
	 * nr 为 SIS_UTIL 扫描预算，INT_MAX 表示该特性未限制本轮扫描。
	 */
	struct cpumask *cpus = this_cpu_cpumask_var_ptr(select_rq_mask);
	int i, cpu, idle_cpu = -1, nr = INT_MAX;

	/* 阶段 1：读取共享扫描预算，过载域用 0 预算直接跳过昂贵的远端探测。 */
	if (sched_feat(SIS_UTIL) && sd->shared) {
		/*
		 * Increment because !--nr is the condition to stop scan.
		 *
		 * Since "sd" is "sd_llc" for target CPU dereferenced in the
		 * caller, it is safe to directly dereference "sd->shared".
		 * Topology bits always ensure it assigned for "sd_llc" abd it
		 * cannot disappear as long as we have a RCU protected
		 * reference to one the associated "sd" here.
		 */
		/*
		 * 预算加一是因为循环在探测前用 !--nr 判断停止；这样共享值 N 恰好允许检查 N 个
		 * 候选。这里的 sd 是调用者在 RCU 读侧取得的 target->sd_llc，拓扑构造保证 LLC 域
		 * 必有 shared 对象，且在该 RCU 引用存续期间不会消失，因此可以直接读取。
		 */
		nr = READ_ONCE(sd->shared->nr_idle_scan) + 1;
		/* overloaded LLC is unlikely to have idle cpu/core */
		/* 共享值为 0 表示 LLC 过载、几乎不可能有空闲 CPU/整核，避免无收益扫描。 */
		if (nr == 1)
			return -1;
	}

	/* 将拓扑范围收窄为任务亲和性允许集合；空交集意味着本域绝无合法目标。 */
	if (!cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr))
		return -1;

	/*
	 * 阶段 2：启用 cluster 拓扑时先扫描 target 所在 cluster。该层通常共享更近缓存，优先命中
	 * 可减少跨 cluster 数据搬移；处理完后从工作集删去整个组，防止 LLC 阶段重复扫描。
	 */
	if (static_branch_unlikely(&sched_cluster_active)) {
		/* sg 是当前 LLC 域首个调度组的借用指针，拓扑保护与 @sd 相同。 */
		struct sched_group *sg = sd->groups;

		if (sg->flags & SD_CLUSTER) {
			for_each_cpu_wrap(cpu, sched_group_span(sg), target + 1) {
				/* cluster span 仍可能包含被任务亲和性过滤掉的 CPU。 */
				if (!cpumask_test_cpu(cpu, cpus))
					continue;

				if (has_idle_core) {
					/* 整核模式由 helper 同时更新 cpus 与线程级回退候选。 */
					i = select_idle_core(p, cpu, cpus, &idle_cpu);
					if ((unsigned int)i < nr_cpumask_bits)
						return i;
				} else {
					/* 线程模式每探测一个合法候选消耗一个预算单位。 */
					if (--nr <= 0)
						return -1;
					idle_cpu = __select_idle_cpu(cpu, p);
					if ((unsigned int)idle_cpu < nr_cpumask_bits)
						return idle_cpu;
				}
			}
			/* cluster 内未命中，下一阶段仅处理 LLC 中尚未检查的 CPU。 */
			cpumask_andnot(cpus, cpus, sched_group_span(sg));
		}
	}

	/* 阶段 3：从 target 后一位环绕扫描剩余 LLC 工作集，保持起点公平且复用相同两种模式。 */
	for_each_cpu_wrap(cpu, cpus, target + 1) {
		if (has_idle_core) {
			i = select_idle_core(p, cpu, cpus, &idle_cpu);
			if ((unsigned int)i < nr_cpumask_bits)
				return i;

		} else {
			if (--nr <= 0)
				return -1;
			idle_cpu = __select_idle_cpu(cpu, p);
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
			if ((unsigned int)idle_cpu < nr_cpumask_bits)
				break;
		}
	}

	/*
	 * 声称“可能存在空闲整核”却扫描完整工作集仍未找到时，清除共享提示，令后续唤醒跳过
	 * 整核搜索；某 CPU 真正变空闲时 __update_idle_core() 会重新置位，不会永久关闭优化。
	 */
	if (has_idle_core)
		set_idle_cores(target, false);

	/* 整核模式保留找到的最佳线程级 idle 回退；普通模式未命中时仍为 -1。 */
	return idle_cpu;
}

/*
 * Idle-capacity scan converts util_fits_cpu() outcomes into preference ranks,
 * where lower values indicate a better fit - see select_idle_capacity().
 *
 * A CPU that both fits the task and sits on a fully-idle SMT core is returned
 * immediately and is never assigned one of these ranks. On !SMT every CPU is
 * its own "core", so the early return covers all fits-and-idle cases and the
 * core-tier ranks below become unreachable.
 *
 *   Rank                            Val  Tier    Meaning
 *   ------------------------------  ---  ------  ---------------------------
 *   ASYM_IDLE_UCLAMP_MISFIT         -4   core    Idle core; capacity fits
 *                                                util but uclamp_min misses.
 *   ASYM_IDLE_COMPLETE_MISFIT       -3   core    Idle core; capacity does
 *                                                not fit. Still beats every
 *                                                thread-tier rank: a busy
 *                                                sibling cuts effective
 *                                                capacity more than a
 *                                                misfit hurts a quiet core.
 *   ASYM_IDLE_THREAD_FITS           -2   thread  Busy SMT sibling; capacity
 *                                                fits util + uclamp.
 *   ASYM_IDLE_THREAD_UCLAMP_MISFIT  -1   thread  Busy SMT sibling; capacity
 *                                                fits but uclamp_min misses
 *                                                (native util_fits_cpu()
 *                                                return value).
 *   ASYM_IDLE_THREAD_MISFIT          0   thread  Busy SMT sibling; capacity
 *                                                does not fit.
 *
 * ASYM_IDLE_CORE_BIAS (-3) is an offset, not a state. On an idle core,
 * fits += ASYM_IDLE_CORE_BIAS rebases thread-tier ranks into the core tier:
 *
 *   ASYM_IDLE_THREAD_UCLAMP_MISFIT (-1) + BIAS -> ASYM_IDLE_UCLAMP_MISFIT   (-4)
 *   ASYM_IDLE_THREAD_MISFIT         (0) + BIAS -> ASYM_IDLE_COMPLETE_MISFIT (-3)
 *
 * ASYM_IDLE_THREAD_FITS (-2) is never rebased because a fully-fitting idle-core
 * candidate early-returns from select_idle_capacity().
 */
/*
 * 非对称容量的空闲扫描把 util_fits_cpu() 结果编码成“数值越小越优先”的等级，供
 * select_idle_capacity() 比较。任务需求完全适配且位于全空闲 SMT 核的 CPU 会立即返回，不会
 * 进入这些等级；非 SMT 机器上每个 CPU 自成一核，所以适配且空闲的候选也都直接返回。
 *
 * 等级从优到劣为：-4 表示空闲整核容量能容纳 util、但达不到 uclamp_min；-3 表示空闲整核连
 * util 也不完全适配，但仍优于所有“有忙 sibling”的线程级候选；-2 表示有忙 sibling 但容量和
 * uclamp 均适配；-1 表示线程容量适配 util、仅 uclamp_min 不适配；0 表示线程容量不适配。
 *
 * ASYM_IDLE_CORE_BIAS=-3 只是偏移量而非状态。空闲整核上的 -1/0 分别映射到 -4/-3，使整核
 * 候选压过部分繁忙的核；-2 不会映射，因为完全适配的空闲整核早已返回。这些值只由
 * select_idle_capacity() 在单次扫描中生成和消费，不是持久任务或 CPU 状态。
 */
enum asym_fits_state {
	/* 空闲整核：util 容量适配，仅 uclamp_min 提示无法满足。 */
	ASYM_IDLE_UCLAMP_MISFIT = -4,
	/* 空闲整核：容量也不适配；无理想 CPU 时仍优先于承受 SMT 争用的候选。 */
	ASYM_IDLE_COMPLETE_MISFIT,
	/* 有忙 sibling 的线程：util 与 uclamp 均适配，但整核资源并不独占。 */
	ASYM_IDLE_THREAD_FITS,
	/* 有忙 sibling 的线程：只缺少 uclamp_min 要求的性能余量。 */
	ASYM_IDLE_THREAD_UCLAMP_MISFIT,
	/* 有忙 sibling 的线程：任务估算利用率超过 CPU 可用容量。 */
	ASYM_IDLE_THREAD_MISFIT,

	/* util_fits_cpu() bias for idle core */
	/* 把空闲整核的非正适配结果平移到 core 优先等级，不代表独立候选状态。 */
	ASYM_IDLE_CORE_BIAS = -3,
};

/*
 * Scan the asym_capacity domain for idle CPUs; pick the first idle one on which
 * the task fits. If no CPU is big enough, but there are idle ones, try to
 * maximize capacity.
 */
/*
 * 扫描非对称容量域中的空闲 CPU：优先返回首个能完整承载任务且满足性能钳制的候选；若没有
 * 足够大的 CPU，则按“整核优先、适配等级优先、同等级容量最大”保留退化候选。
 *
 * 业务背景：select_idle_sibling() 在异构系统存在 sd_asym_cpucapacity 域时调用这里，避免
 * 高利用率或高 uclamp_min 任务落到容量不足的小核，并在 SMT 系统上优先避开忙 sibling。
 * 入参：@p 是待唤醒任务的借用指针；@sd 是非对称容量调度域的借用指针；@target 是环绕扫描
 * 起点和共享 idle-core 提示的定位 CPU。参数均为纯输入且不转移 ownership。
 * 出参/返回：返回最佳空闲 CPU 编号；无候选时返回 -1。函数只读取快照并可能清除过期的
 * has_idle_cores 性能提示，不执行迁移或入队。
 * 注意事项：调用者在 RCU 读侧窗口内保持 @sd/shared 有效，并保证 per-CPU 临时掩码可独占使用；
 * 函数不睡眠。容量、uclamp 与 idle 状态都可能并发变化，最终正确性由后续入队锁协议保证。
 */
static int
select_idle_capacity(struct task_struct *p, struct sched_domain *sd, int target)
{
	/*
	 * On !SMT systems, has_idle_core is always false and preferred_core
	 * is always true (CPU == core), so the SMT preference logic below
	 * collapses to the plain capacity scan.
	 */
	/*
	 * 非 SMT 系统中 has_idle_core 恒为 false，且每个 CPU 就是自己的 core，所以
	 * preferred_core 恒真；下面的分层自然退化为普通容量扫描。
	 */
	bool has_idle_core = sched_smt_active() && test_idle_cores(target);
	/* task_util/uclamp 是任务需求；best_cap/best_fits/best_cpu 共同保存当前最佳退化候选。 */
	unsigned long task_util, util_min, util_max, best_cap = 0;
	int fits, best_fits = ASYM_IDLE_THREAD_MISFIT;
	int cpu, best_cpu = -1;
	/* cpus 是本 CPU 可破坏工作掩码；nr 是非整核模式下的剩余扫描预算。 */
	struct cpumask *cpus;
	int nr = INT_MAX;

	/* 阶段 1：构造“当前容量域且任务允许”的候选集合，并冻结本轮任务需求快照。 */
	cpus = this_cpu_cpumask_var_ptr(select_rq_mask);
	cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr);

	task_util = task_util_est(p);
	util_min = uclamp_eff_value(p, UCLAMP_MIN);
	util_max = uclamp_eff_value(p, UCLAMP_MAX);

	if (sched_feat(SIS_UTIL) && sd->shared) {
		/*
		 * Same nr_idle_scan hint as select_idle_cpu(), nr only limits
		 * the scan when not preferring an idle core.
		 */
		/*
		 * 与 select_idle_cpu() 复用 nr_idle_scan 提示；只有不优先找完整空闲核时才消耗预算，
		 * 整核提示为真时则扫描到足以证实或推翻提示。
		 */
		nr = READ_ONCE(sd->shared->nr_idle_scan) + 1;
		/* overloaded domain is unlikely to have idle cpu/core */
		/* 0 预算代表域已过载，继续扫描大概率只有成本而没有空闲命中。 */
		if (nr == 1)
			return -1;
	}

	/* 阶段 2：从 target 环绕遍历候选，为每个真正 idle 的 CPU 计算适配等级。 */
	for_each_cpu_wrap(cpu, cpus, target) {
		/* 无需寻找整核或当前 CPU 所在核全空闲时，它属于 preferred_core 层级。 */
		bool preferred_core = !has_idle_core || is_core_idle(cpu);
		/* 默认用标称容量；仅 uclamp_min 不适配时会改用考虑运行压力后的实际容量。 */
		unsigned long cpu_cap = capacity_of(cpu);

		/*
		 * Stop when the nr_idle_scan is exhausted (mirrors
		 * select_idle_cpu() logic).
		 */
		/* 非整核模式按共享预算停止，同时保留并返回已经找到的最佳候选。 */
		if (!has_idle_core && --nr <= 0)
			return best_cpu;

		/* 忙 CPU 不参与本函数的空闲候选排名。 */
		if (!choose_idle_cpu(cpu, p))
			continue;

		/* 返回值同时编码 util 容量与 uclamp 性能提示是否满足。 */
		fits = util_fits_cpu(task_util, util_min, util_max, cpu);

		/*
		 * Perfect fit: capacity satisfies util + uclamp and the CPU
		 * sits on a fully-idle SMT core, this is a !SMT system, or
		 * there is no idle core to find.
		 * Short-circuit the rank-based selection and return
		 * immediately.
		 */
		/* 完全适配且位于首选整核时已经达到最优目标，立即返回以缩短唤醒延迟。 */
		if (fits > 0 && preferred_core)
			return cpu;
		/*
		 * Only the min performance hint (i.e. uclamp_min) doesn't fit.
		 * Look for the CPU with best capacity.
		 */
		/* 仅最低性能提示不满足时，用扣除运行压力后的实际容量区分退化候选。 */
		else if (fits < 0)
			cpu_cap = get_actual_cpu_capacity(cpu);
		/*
		 * fits > 0 implies we are not on a preferred core, but the util
		 * fits CPU capacity. Set fits to ASYM_IDLE_THREAD_FITS
		 * so the effective range becomes
		 * [ASYM_IDLE_THREAD_FITS, ASYM_IDLE_THREAD_MISFIT], where:
		 *    ASYM_IDLE_THREAD_MISFIT - does not fit
		 *    ASYM_IDLE_THREAD_UCLAMP_MISFIT - fits with the exception of UCLAMP_MIN
		 *    ASYM_IDLE_THREAD_FITS - fits with the exception of preferred_core
		 */
		/*
		 * fits>0 但不在 preferred_core，说明任务需求适配、只是 sibling 正忙；改写为 -2 后，
		 * 与“仅 uclamp_min 不适配”的 -1 和“容量不适配”的 0 组成线程级有序区间。
		 */
		else if (fits > 0)
			fits = ASYM_IDLE_THREAD_FITS;

		/*
		 * If we are on a preferred core, translate the range of fits
		 * of [ASYM_IDLE_THREAD_UCLAMP_MISFIT, ASYM_IDLE_THREAD_MISFIT] to
		 * [ASYM_IDLE_UCLAMP_MISFIT, ASYM_IDLE_COMPLETE_MISFIT].
		 * This ensures that an idle core is always given priority over
		 * (partially) busy core.
		 *
		 * A fully fitting idle core would have returned early and hence
		 * fits > 0 for preferred_core need not be dealt with.
		 */
		/*
		 * preferred_core 上只会剩 -1 或 0；加 -3 后映射成 -4/-3，使任何全空闲核都优先于
		 * 部分繁忙核。完全适配的空闲核已在上方返回，无需处理正值。
		 */
		if (preferred_core)
			fits += ASYM_IDLE_CORE_BIAS;

		/*
		 * First, select CPU which fits better (lower is more preferred).
		 * Then, select the one with best capacity at same level.
		 */
		/* 主键是更小的适配等级；同等级再选容量更大者，为退化放置保留最大余量。 */
		if ((fits < best_fits) ||
		    ((fits == best_fits) && (cpu_cap > best_cap))) {
			best_cap = cpu_cap;
			best_cpu = cpu;
			best_fits = fits;
		}
	}

	/*
	 * A value in the [ASYM_IDLE_UCLAMP_MISFIT, ASYM_IDLE_COMPLETE_MISFIT]
	 * range means the chosen CPU is in a fully idle SMT core. Values above
	 * ASYM_IDLE_COMPLETE_MISFIT mean we never ranked such a CPU best.
	 *
	 * The asym-capacity wakeup path returns from select_idle_sibling()
	 * after this function and never runs select_idle_cpu(), so the usual
	 * select_idle_cpu() tail that clears idle cores must live here when the
	 * idle-core preference did not win.
	 */
	/*
	 * -4/-3 表示最佳项来自全空闲 SMT 核；更大的等级说明整轮未让空闲核胜出。异构路径会
	 * 直接从 select_idle_sibling() 返回，不经过 select_idle_cpu() 的清理尾部，所以在这里
	 * 清除被扫描证伪的 has_idle_cores，等待 idle 更新重新置位。
	 */
	if (has_idle_core && best_fits > ASYM_IDLE_COMPLETE_MISFIT)
		set_idle_cores(target, false);

	/* -1 表示没有任何空闲候选；否则返回按等级与容量排序后的最佳 CPU。 */
	return best_cpu;
}

/*
 * asym_fits_cpu() - 判断快速路径候选是否满足非对称容量与 SMT 独占要求。
 *
 * 业务背景：select_idle_sibling() 尝试 target/prev/recent_used_cpu 等近邻候选时调用它，避免
 * 快速命中绕过异构容量检查；对称容量系统无需额外过滤。
 * 入参：@util 是任务估算利用率；@util_min/@util_max 是有效 uclamp 边界，三者均使用调度容量
 * 标度；@cpu 是待验证逻辑 CPU。全部为纯输入。
 * 出参/返回：异构容量启用时，仅 CPU 完整满足 util/uclamp，且 SMT 启用时整核无忙 sibling
 * 才返回 true；未启用异构容量时恒 true。无副作用。
 * 注意事项：函数不睡眠、不加 rq 锁，结果只是唤醒选择快照；ASYM+SMT 下的整核门禁也作用于
 * 三个提前返回候选，而普通对称容量路径不会施加这一额外限制。
 */
static inline bool asym_fits_cpu(unsigned long util,
				 unsigned long util_min,
				 unsigned long util_max,
				 int cpu)
{
	if (sched_asym_cpucap_active()) {
		/*
		 * Return true only if the cpu fully fits the task requirements
		 * which include the utilization and the performance hints.
		 *
		 * When SMT is active, also require that the core has no busy
		 * siblings.
		 *
		 * Note: gating on is_core_idle() also makes the early-bailout
		 * candidates in select_idle_sibling() (target, prev,
		 * recent_used_cpu) idle-core-aware on ASYM+SMT, which the
		 * NO_ASYM path does not do.
		 */
		/*
		 * 只有任务利用率与性能钳制都完整适配才允许快速返回；SMT 开启时还要求没有忙 sibling。
		 * 因而 target、prev、recent_used_cpu 在 ASYM+SMT 上都会感知整核状态，对称容量路径则不会。
		 */
		return (!sched_smt_active() || is_core_idle(cpu)) &&
		       (util_fits_cpu(util, util_min, util_max, cpu) > 0);
	}

	return true;
}

/*
 * Try and locate an idle core/thread in the LLC cache domain.
 */
/* 按 target、共享缓存、idle core/CPU/SMT 次序搜索；结果仍需唤醒路径复验。 */
/*
 * select_idle_sibling() - 在唤醒快速路径中寻找兼顾缓存亲和性与容量适配的空闲 CPU/SMT 核。
 *
 * 业务背景：select_task_rq_fair() 完成 wake-affine/EAS 决策后调用这里。函数先尝试 target、
 * prev、per-CPU kthread 所在 CPU 和 recent_used_cpu 等低成本近邻，再按异构容量域或 LLC 域
 * 扩大搜索；目标是避免通用负载均衡的高成本，同时尽量保持热缓存并减少 SMT 争用。
 * 入参：@p 是正在选择唤醒目标的任务借用指针，调用者持有使亲和性稳定的 @p->pi_lock；
 * @prev 是任务上次运行 CPU；@target 是 wake-affine 等上游策略给出的首选 CPU。三者为纯输入，
 * CPU 编号必须有效，函数不取得或转移任务引用。
 * 出参/返回：始终返回一个 CPU 编号；优先返回通过 idle、亲和性、容量及拓扑约束的候选，全部
 * 搜索失败时回退 @target。可观察副作用是把 @p->recent_used_cpu 更新为 @prev，并可能清除过期的
 * idle-core 扫描提示；函数不把任务入队。
 * 注意事项：调用者必须关闭本地中断，以独占本 CPU 的 select_rq_mask 临时掩码，并处在调度域
 * 拓扑可安全解引用的窗口；函数不睡眠。所有 idle/容量判断都是无目标 rq 锁快照，返回后仍需
 * 唤醒入队路径处理并发变化。
 */
static int select_idle_sibling(struct task_struct *p, int prev, int target)
{
	/*
	 * has_idle_core 决定 LLC 扫描按整核还是线程工作；sd 是 RCU 借用的搜索域；task_util 与
	 * uclamp 只在异构容量启用时有效；prev_aff/recent_used_cpu 保存 cluster 约束下的回退候选。
	 */
	bool has_idle_core = false;
	struct sched_domain *sd;
	unsigned long task_util, util_min, util_max;
	int i, recent_used_cpu, prev_aff = -1;

	/*
	 * On asymmetric system, update task utilization because we will check
	 * that the task fits with CPU's capacity.
	 */
	/*
	 * 异构系统必须比较任务需求与大小核容量，因此先把 sched_entity 的 PELT 值同步到当前时间，
	 * 再取得估算利用率和有效 uclamp 边界；对称系统的 asym_fits_cpu() 恒真，不消费这些值。
	 */
	if (sched_asym_cpucap_active()) {
		sync_entity_load_avg(&p->se);
		task_util = task_util_est(p);
		util_min = uclamp_eff_value(p, UCLAMP_MIN);
		util_max = uclamp_eff_value(p, UCLAMP_MAX);
	}

	/*
	 * per-cpu select_rq_mask usage
	 */
	/* 后续 helper 会改写本 CPU 的 select_rq_mask；关中断保证同 CPU 中断路径不会重入并覆盖它。 */
	lockdep_assert_irqs_disabled();

	/* 阶段 1：首选 target 已空闲且在异构系统上容量适配时，直接接受上游策略结果。 */
	if (choose_idle_cpu(target, p) &&
	    asym_fits_cpu(task_util, util_min, util_max, target))
		return target;

	/*
	 * If the previous CPU is cache affine and idle, don't be stupid:
	 */
	/*
	 * 若 prev 与 target 共享缓存且 prev 空闲、容量适配，优先复用任务原 CPU，避免无意义迁移与
	 * cache 冷启动。cluster 激活时还要求共享更低层资源；否则先记为 prev_aff，待域扫描失败回退。
	 */
	if (prev != target && cpus_share_cache(prev, target) &&
	    choose_idle_cpu(prev, p) &&
	    asym_fits_cpu(task_util, util_min, util_max, prev)) {

		if (!static_branch_unlikely(&sched_cluster_active) ||
		    cpus_share_resources(prev, target))
			return prev;

		prev_aff = prev;
	}

	/*
	 * Allow a per-cpu kthread to stack with the wakee if the
	 * kworker thread and the tasks previous CPUs are the same.
	 * The assumption is that the wakee queued work for the
	 * per-cpu kthread that is now complete and the wakeup is
	 * essentially a sync wakeup. An obvious example of this
	 * pattern is IO completions.
	 */
	/*
	 * 当前若是 per-CPU kthread，且它运行在 wakee 的 prev CPU、当前 rq 除它外基本无任务，允许
	 * wakee 与其叠放。典型场景是 wakee 提交 I/O 后由本 CPU kworker 完成工作再将其唤醒，这在
	 * 数据依赖上近似同步唤醒；复用 prev 能保留生产者/消费者缓存。容量门禁仍不可省略。
	 */
	if (is_per_cpu_kthread(current) &&
	    in_task() &&
	    prev == smp_processor_id() &&
	    this_rq()->nr_running <= 1 &&
	    asym_fits_cpu(task_util, util_min, util_max, prev)) {
		return prev;
	}

	/* Check a recently used CPU as a potential idle candidate: */
	/*
	 * 阶段 2：把上一次记录的 recent CPU 取出作为缓存近邻候选，同时无条件把本轮 prev 写回，
	 * 让下一次唤醒能记住这次来源。候选必须不同于 prev/target、共享 LLC、空闲、在亲和性内且
	 * 容量适配；cluster 资源不匹配时保留到全域扫描失败后再回退。
	 */
	recent_used_cpu = p->recent_used_cpu;
	p->recent_used_cpu = prev;
	if (recent_used_cpu != prev &&
	    recent_used_cpu != target &&
	    cpus_share_cache(recent_used_cpu, target) &&
	    choose_idle_cpu(recent_used_cpu, p) &&
	    cpumask_test_cpu(recent_used_cpu, p->cpus_ptr) &&
	    asym_fits_cpu(task_util, util_min, util_max, recent_used_cpu)) {

		if (!static_branch_unlikely(&sched_cluster_active) ||
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		    cpus_share_resources(recent_used_cpu, target))
			return recent_used_cpu;

	} else {
		recent_used_cpu = -1;
	}

	/*
	 * For asymmetric CPU capacity systems, our domain of interest is
	 * sd_asym_cpucapacity rather than sd_llc.
	 */
	/* 异构容量系统应先在覆盖不同原始容量等级的域中比较候选，而不是只看共享 LLC。 */
	if (sched_asym_cpucap_active()) {
		/* sd 借用自 target 的 per-CPU 拓扑槽，其生命周期由调用上下文的拓扑保护保证。 */
		sd = rcu_dereference_all(per_cpu(sd_asym_cpucapacity, target));
		/*
		 * On an asymmetric CPU capacity system where an exclusive
		 * cpuset defines a symmetric island (i.e. one unique
		 * capacity_orig value through the cpuset), the key will be set
		 * but the CPUs within that cpuset will not have a domain with
		 * SD_ASYM_CPUCAPACITY. These should follow the usual symmetric
		 * capacity path.
		 */
		/*
		 * 全局异构 static key 可能已开启，但 exclusive cpuset 可形成 capacity_orig 唯一的对称
		 * 小岛，此时岛内 CPU 没有 SD_ASYM_CPUCAPACITY 域，应继续走普通 LLC 对称容量路径。
		 */
		if (sd) {
			/* 异构扫描自行完成容量与 SMT 排名；无空闲候选时保持上游 target。 */
			i = select_idle_capacity(p, sd, target);
			return ((unsigned)i < nr_cpumask_bits) ? i : target;
		}
	}

	/* 阶段 3：对称容量路径取得 target 的 LLC 域，域缺失时无法安全扩大搜索。 */
	sd = rcu_dereference_all(per_cpu(sd_llc, target));
	if (!sd)
		return target;

	/*
	 * SMT 活跃时读取“可能存在空闲整核”的共享提示。提示为 false 且 prev 仍在同一 LLC 时，
	 * 先只扫 prev 的 sibling；这一局部尝试比遍历整个 LLC 更便宜。
	 */
	if (sched_smt_active()) {
		has_idle_core = test_idle_cores(target);

		if (!has_idle_core && cpus_share_cache(prev, target)) {
			i = select_idle_smt(p, sd, prev);
			if ((unsigned int)i < nr_cpumask_bits)
				return i;
		}
	}

	/* 阶段 4：近邻均未命中，按共享预算和整核提示扫描完整 LLC 候选集。 */
	i = select_idle_cpu(p, sd, has_idle_core, target);
	if ((unsigned)i < nr_cpumask_bits)
		return i;

	/*
	 * For cluster machines which have lower sharing cache like L2 or
	 * LLC Tag, we tend to find an idle CPU in the target's cluster
	 * first. But prev_cpu or recent_used_cpu may also be a good candidate,
	 * use them if possible when no idle CPU found in select_idle_cpu().
	 */
	/*
	 * cluster 机器优先在 target 的 L2/LLC-tag 等更低层共享资源内找 idle CPU；若全扫描没有命中，
	 * 先前因不共享 cluster 而暂存的 prev/recent 仍可能优于忙碌的 target，按该顺序回退。
	 */
	if ((unsigned int)prev_aff < nr_cpumask_bits)
		return prev_aff;
	if ((unsigned int)recent_used_cpu < nr_cpumask_bits)
		return recent_used_cpu;

	/* 所有空闲与亲和候选都失败时维持 target，由后续 enqueue 接受其可能忙碌的现实。 */
	return target;
}

/**
 * cpu_util() - Estimates the amount of CPU capacity used by CFS tasks.
 * @cpu: the CPU to get the utilization for
 * @p: task for which the CPU utilization should be predicted or NULL
 * @dst_cpu: CPU @p migrates to, -1 if @p moves from @cpu or @p == NULL
 * @boost: 1 to enable boosting, otherwise 0
 *
 * The unit of the return value must be the same as the one of CPU capacity
 * so that CPU utilization can be compared with CPU capacity.
 *
 * CPU utilization is the sum of running time of runnable tasks plus the
 * recent utilization of currently non-runnable tasks on that CPU.
 * It represents the amount of CPU capacity currently used by CFS tasks in
 * the range [0..max CPU capacity] with max CPU capacity being the CPU
 * capacity at f_max.
 *
 * The estimated CPU utilization is defined as the maximum between CPU
 * utilization and sum of the estimated utilization of the currently
 * runnable tasks on that CPU. It preserves a utilization "snapshot" of
 * previously-executed tasks, which helps better deduce how busy a CPU will
 * be when a long-sleeping task wakes up. The contribution to CPU utilization
 * of such a task would be significantly decayed at this point of time.
 *
 * Boosted CPU utilization is defined as max(CPU runnable, CPU utilization).
 * CPU contention for CFS tasks can be detected by CPU runnable > CPU
 * utilization. Boosting is implemented in cpu_util() so that internal
 * users (e.g. EAS) can use it next to external users (e.g. schedutil),
 * latter via cpu_util_cfs_boost().
 *
 * CPU utilization can be higher than the current CPU capacity
 * (f_curr/f_max * max CPU capacity) or even the max CPU capacity because
 * of rounding errors as well as task migrations or wakeups of new tasks.
 * CPU utilization has to be capped to fit into the [0..max CPU capacity]
 * range. Otherwise a group of CPUs (CPU0 util = 121% + CPU1 util = 80%)
 * could be seen as over-utilized even though CPU1 has 20% of spare CPU
 * capacity. CPU utilization is allowed to overshoot current CPU capacity
 * though since this is useful for predicting the CPU capacity required
 * after task migrations (scheduler-driven DVFS).
 *
 * Return: (Boosted) (estimated) utilization for the specified CPU.
 */
/*
 * cpu_util() - 估算指定 CPU 上 CFS 任务占用的容量，并可模拟一次任务迁移。
 *
 * 原注释中的 @cpu 是待估算 CPU；@p 可为空，非空时表示要从快照中加上或扣除的任务；
 * @dst_cpu 是 p 的预测目标，-1 表示从当前 CPU 移出或没有 p；@boost 非 0 时用 runnable_avg
 * 抬高结果。返回单位与 CPU capacity 相同，才能直接比较。
 *
 * CFS util_avg 包含可运行任务的运行时间和当前睡眠任务最近留下的衰减贡献，范围最终规范为
 * [0, f_max 下最大容量]。UTIL_EST 再取当前 util 与可运行任务估算和的较大值，保留长睡任务
 * 醒来前已经衰减掉的历史忙碌快照。boost 则取 runnable 与 util 的较大值：runnable>util
 * 表示 CFS 竞争，EAS 可直接请求该视图，schedutil 经 cpu_util_cfs_boost() 使用它。
 *
 * 迁移、刚唤醒任务及舍入可令原始利用率超过当前频率容量甚至最大容量；函数只截断到最大
 * CPU capacity，而允许高于当前频率容量，以便调度器驱动 DVFS 预测迁移后的频率需求。否则例如
 * 121% 与 80% 的两个 CPU 会被错误判为整个组过载，忽略后者仍有的 20% 空闲容量。
 *
 * 业务背景：EAS、空闲选核和 schedutil 需要同一套 CFS 容量视图，并经 @p/@dst_cpu 比较
 * “迁移前”和“迁移后”场景，而不能真的移动任务再测量。
 * 入参：@cpu 为有效逻辑 CPU；@p 为可空借用任务指针；@dst_cpu 为预测落点或 -1；@boost
 * 控制是否纳入 runnable 压力。函数不取得引用，调用者负责在读取期间稳定 @p。
 * 出参/返回：返回截断到 arch_scale_cpu_capacity(@cpu) 的普通/boosted、PELT/util_est 最大利用率；
 * 不修改任务或运行队列。
 * 注意事项：该函数通过 READ_ONCE 获取近似并发快照，不持有目标 rq 锁且不睡眠；各字段并非
 * 原子事务快照，因此适合放置/频率启发式，不提供记账精确性保证。
 */
static unsigned long
cpu_util(int cpu, struct task_struct *p, int dst_cpu, int boost)
{
	/* add/sub_task 编码本次预测对 @cpu 的方向；cfs_rq 为借用队列，util 是可调整快照。 */
	bool add_task = p && task_cpu(p) != cpu && dst_cpu == cpu;
	bool sub_task = p && task_cpu(p) == cpu && dst_cpu != cpu;
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	unsigned long util = READ_ONCE(cfs_rq->avg.util_avg);
	unsigned long runnable;

	/*
	 * If @dst_cpu is -1 or @p migrates from @cpu to @dst_cpu remove its
	 * contribution. If @p migrates from another CPU to @cpu add its
	 * contribution. In all the other cases @cpu is not impacted by the
	 * migration so its util_avg is already correct.
	 */
	/*
	 * dst_cpu=-1 或 p 从 cpu 迁出时扣除 p；p 从其他 CPU 迁入 cpu 时增加 p；其他组合不改变
	 * cpu 的预测状态，当前 util_avg 已可直接使用。lsub_positive() 饱和到 0，避免快照时差下溢。
	 */
	if (add_task)
		util += task_util(p);
	else if (sub_task)
		lsub_positive(&util, task_util(p));

	/* boost 阶段把 runnable 压力按同一迁移方向调整，再与 util 取最大值暴露排队竞争。 */
	if (boost) {
		runnable = READ_ONCE(cfs_rq->avg.runnable_avg);
		if (add_task)
			runnable += READ_ONCE(p->se.avg.runnable_avg);
		else if (sub_task)
			lsub_positive(&runnable,
				      READ_ONCE(p->se.avg.runnable_avg));
		util = max(util, runnable);
	}

	/* UTIL_EST 阶段用更敏捷的任务估算弥补 PELT 对新唤醒/长睡任务反应较慢的问题。 */
	if (sched_feat(UTIL_EST)) {
		/* util_est 保存调整前的 rq 估算和，随后模拟 p 入队或移除。 */
		unsigned long util_est;

		util_est = READ_ONCE(cfs_rq->avg.util_est);

		/*
		 * During wake-up @p isn't enqueued yet and doesn't contribute
		 * to any cpu_rq(cpu)->cfs.avg.util_est.
		 * If @dst_cpu == @cpu add it to "simulate" cpu_util after @p
		 * has been enqueued.
		 *
		 * During exec (@dst_cpu = -1) @p is enqueued and does
		 * contribute to cpu_rq(cpu)->cfs.util_est.
		 * Remove it to "simulate" cpu_util without @p's contribution.
		 *
		 * Despite the task_on_rq_queued(@p) check there is still a
		 * small window for a possible race when an exec
		 * select_task_rq_fair() races with LB's detach_task().
		 *
		 *   detach_task()
		 *     deactivate_task()
		 *       p->on_rq = TASK_ON_RQ_MIGRATING;
		 *       -------------------------------- A
		 *       dequeue_task()                    \
		 *         dequeue_task_fair()              + Race Time
		 *           util_est_dequeue()            /
		 *       -------------------------------- B
		 *
		 * The additional check "current == p" is required to further
		 * reduce the race window.
		 */
		/*
		 * 唤醒阶段 p 尚未入队、未计入任何 rq 的 util_est；若 dst_cpu==cpu，显式加上 p 来模拟
		 * 入队后状态。exec 使用 dst_cpu=-1，此时 p 通常仍已入队，应扣除它以模拟“无 p”。
		 *
		 * task_on_rq_queued() 仍无法完全关闭 exec 选核与负载均衡 detach_task() 的竞态：A 点先把
		 * on_rq 改成 TASK_ON_RQ_MIGRATING，直到 B 点 dequeue_task_fair()->util_est_dequeue()
		 * 才真正从聚合值删除，期间标志和记账短暂不一致。current==p 的补充检查进一步缩小遗漏
		 * 扣除的窗口，但这里仍是允许轻微误差的预测快照。
		 */
		if (dst_cpu == cpu)
			util_est += _task_util_est(p);
		else if (p && unlikely(task_on_rq_queued(p) || current == p))
			lsub_positive(&util_est, _task_util_est(p));

		util = max(util, util_est);
	}

	/* 仅限制到 f_max 的架构最大容量；保留高于当前频率容量的压力以支持 DVFS 预测。 */
	return min(util, arch_scale_cpu_capacity(cpu));
}

/*
 * cpu_util_cfs() - 获取 CPU 当前未 boost 的 CFS 利用率。
 *
 * 业务背景：不需要迁移模拟的内部/外部调用者复用 cpu_util() 的普通视图。
 * 入参：@cpu 为有效逻辑 CPU，纯输入。
 * 出参/返回：返回不加减任务且不使用 runnable boost 的容量标度利用率；无副作用。
 * 注意事项：继承 cpu_util() 的近似快照、不加 rq 锁且不睡眠的约束。
 */
unsigned long cpu_util_cfs(int cpu)
{
	return cpu_util(cpu, NULL, -1, 0);
}

/*
 * cpu_util_cfs_boost() - 获取考虑 CFS runnable 竞争的 boosted 利用率。
 *
 * 业务背景：schedutil 等调用者需要在 runnable_avg 高于 util_avg 时提前提高容量/频率需求。
 * 入参：@cpu 为有效逻辑 CPU，纯输入。
 * 出参/返回：返回 max(util, runnable) 再结合 UTIL_EST 并截断后的容量标度结果；无副作用。
 * 注意事项：这是压力启发式而非精确运行时间，继承 cpu_util() 的无锁快照约束且不睡眠。
 */
unsigned long cpu_util_cfs_boost(int cpu)
{
	return cpu_util(cpu, NULL, -1, 1);
}

/*
 * cpu_util_without: compute cpu utilization without any contributions from *p
 * @cpu: the CPU which utilization is requested
 * @p: the task which utilization should be discounted
 *
 * The utilization of a CPU is defined by the utilization of tasks currently
 * enqueued on that CPU as well as tasks which are currently sleeping after an
 * execution on that CPU.
 *
 * This method returns the utilization of the specified CPU by discounting the
 * utilization of the specified task, whenever the task is currently
 * contributing to the CPU utilization.
 */
/*
 * 计算去掉 p 贡献后的 CPU 利用率。CPU 利用率既包含当前入队任务，也包含曾在该 CPU 运行、
 * 现已睡眠但 PELT 尚未衰减完的任务；只要 p 仍对该 CPU 的聚合值有贡献，就应在预测中扣除。
 *
 * 业务背景：唤醒选核和 EAS 比较“把 p 从源 CPU 拿走”后的剩余负载，避免把 p 同时算在源与目标。
 * 入参：@cpu 是待估算逻辑 CPU；@p 是必须非空且由调用者稳定的借用任务指针。
 * 出参/返回：p 属于该 CPU 且已有 PELT 历史时返回扣除 p 的非 boost 利用率，否则返回 CPU 当前
 * 利用率；不修改 p 或 rq，也不转移 ownership。
 * 注意事项：last_update_time==0 表示新实体尚无可扣贡献；函数不睡眠，结果继承 cpu_util() 的
 * 并发近似性和最大容量截断。
 */
static unsigned long cpu_util_without(int cpu, struct task_struct *p)
{
	/* Task has no contribution or is new */
	/* p 不属于 cpu，或从未进入 PELT 时间线时没有可扣贡献，以 NULL 关闭迁移模拟。 */
	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		p = NULL;

	return cpu_util(cpu, p, -1, 0);
}

/*
 * This function computes an effective utilization for the given CPU, to be
 * used for frequency selection given the linear relation: f = u * f_max.
 *
 * The scheduler tracks the following metrics:
 *
 *   cpu_util_{cfs,rt,dl,irq}()
 *   cpu_bw_dl()
 *
 * Where the cfs,rt and dl util numbers are tracked with the same metric and
 * synchronized windows and are thus directly comparable.
 *
 * The cfs,rt,dl utilization are the running times measured with rq->clock_task
 * which excludes things like IRQ and steal-time. These latter are then accrued
 * in the IRQ utilization.
 *
 * The DL bandwidth number OTOH is not a measured metric but a value computed
 * based on the task model parameters and gives the minimal utilization
 * required to meet deadlines.
 */
/*
 * effective_cpu_util() - 合并各调度类与 IRQ 压力，生成频率选择所需的有效利用率及钳制提示。
 *
 * 该计算面向线性近似 f = u * f_max。调度器维护 cpu_util_{cfs,rt,dl,irq}() 与 cpu_bw_dl()：
 * CFS/RT/DL 的运行时间都以排除 IRQ 和 steal time 的 rq->clock_task、同步 PELT 窗口记账，因而
 * 可直接相加；缺失的 IRQ/steal 压力另由 irq util 补回。DL bandwidth 则不是测量值，而是从
 * deadline 参数推导出的、按期完成任务所需的最低利用率。
 *
 * 业务背景：schedutil 与 EAS 不能只看 CFS 负载；它们既要知道 CPU 实际总忙碌度，也要把 DL
 * 保证、RT 默认满频要求和 uclamp min/max 作为频率边界交给容量/能耗模型。
 * 入参：@cpu 为有效逻辑 CPU；@util_cfs 是已按容量标度计算的 CFS 利用率；@min/@max 是可空
 * 输出指针，分别接收最低性能需求和软上限。指针由调用者所有，本函数只写值、不保存引用。
 * 出参/返回：返回加入 RT/DL、按 IRQ 不可见时间缩放并截断到架构最大容量的有效利用率；若输出
 * 指针非空则同步写入 clamp/DL/RT 推导的边界，不修改 rq 记账。
 * 注意事项：函数不持 rq 锁、不睡眠，读取的是允许近似误差的调度快照；@max 是软带宽提示，
 * 可以低于实际利用率，不能用来截断本函数返回的真实需求。IRQ 饱和时三个结果均直接取满容量。
 */
unsigned long effective_cpu_util(int cpu, unsigned long util_cfs,
				 unsigned long *min,
				 unsigned long *max)
{
	unsigned long util, irq, scale;
	struct rq *rq = cpu_rq(cpu);

	/* scale 是 f_max 下的 CPU 容量上界，所有输入、输出都归一到同一标度。 */
	scale = arch_scale_cpu_capacity(cpu);

	/*
	 * Early check to see if IRQ/steal time saturates the CPU, can be
	 * because of inaccuracies in how we track these -- see
	 * update_irq_load_avg().
	 */
	/*
	 * 阶段 1：IRQ/steal 估算若已达到或超过容量，可能来自 update_irq_load_avg() 的近似误差，
	 * 但无论原因都已没有可分配给任务的余量；提前把返回值与可选上下界统一饱和到 scale。
	 */
	irq = cpu_util_irq(rq);
	if (unlikely(irq >= scale)) {
		if (min)
			*min = scale;
		if (max)
			*max = scale;
		return scale;
	}

	/* 阶段 2：按调用者需要计算最低频率/容量保证。 */
	if (min) {
		/*
		 * The minimum utilization returns the highest level between:
		 * - the computed DL bandwidth needed with the IRQ pressure which
		 *   steals time to the deadline task.
		 * - The minimum performance requirement for CFS and/or RT.
		 */
		/*
		 * 下界取两类约束的较大者：DL 模型带宽加上会偷走 deadline 执行时间的 IRQ 压力；以及
		 * CFS/RT 汇总后的 uclamp_min。这样频率既不破坏 deadline 保证，也不低于显式性能提示。
		 */
		*min = max(irq + cpu_bw_dl(rq), uclamp_rq_get(rq, UCLAMP_MIN));

		/*
		 * When an RT task is runnable and uclamp is not used, we must
		 * ensure that the task will run at maximum compute capacity.
		 */
		/* 未启用 uclamp 时沿用 RT runnable 默认请求最大算力的语义，把 min 提升到 scale。 */
		if (!uclamp_is_used() && rt_rq_is_runnable(&rq->rt))
			*min = max(*min, scale);
	}

	/*
	 * Because the time spend on RT/DL tasks is visible as 'lost' time to
	 * CFS tasks and we use the same metric to track the effective
	 * utilization (PELT windows are synchronized) we can directly add them
	 * to obtain the CPU's actual utilization.
	 */
	/*
	 * 阶段 3：RT/DL 对 CFS 表现为 rq->clock_task 中丢失的执行时间，但三者 PELT 窗口同步，
	 * 因此可把 RT、DL util 直接加到调用者提供的 CFS util，恢复任务侧总利用率。
	 */
	util = util_cfs + cpu_util_rt(rq);
	util += cpu_util_dl(rq);

	/*
	 * The maximum hint is a soft bandwidth requirement, which can be lower
	 * than the actual utilization because of uclamp_max requirements.
	 */
	/* uclamp_max 是期望的软带宽上限，可低于当前真实 util；只经输出参数报告，不截断 util。 */
	if (max)
		*max = min(scale, uclamp_rq_get(rq, UCLAMP_MAX));

	/* 任务侧利用率已满时无需再做 IRQ 比例换算，直接饱和返回。 */
	if (util >= scale)
		return scale;

	/*
	 * There is still idle time; further improve the number by using the
	 * IRQ metric. Because IRQ/steal time is hidden from the task clock we
	 * need to scale the task numbers:
	 *
	 *              max - irq
	 *   U' = irq + --------- * U
	 *                 max
	 */
	/*
	 * 阶段 4：尚有空闲容量时补回 task clock 看不到的 IRQ/steal 时间。先把任务 util 按
	 * (scale-irq)/scale 压缩到剩余时间轴，再加 irq，即 U' = irq + (scale-irq)/scale * U；
	 * 最后再次截断，吸收并发快照与整数舍入造成的轻微超界。
	 */
	util = scale_irq_capacity(util, irq, scale);
	util += irq;

	return min(scale, util);
}

/*
 * sched_cpu_util() - 获取供调度器通用消费者使用的 CPU 总有效利用率。
 *
 * 业务背景：只关心合并后忙碌度、不需要 min/max 钳制输出的路径通过该薄包装统一调用方式。
 * 入参：@cpu 为有效逻辑 CPU，纯输入。
 * 出参/返回：返回 cpu_util_cfs() 与 RT/DL/IRQ 合并后的最大容量标度利用率；无输出参数和副作用。
 * 注意事项：不睡眠，继承 cpu_util_cfs()/effective_cpu_util() 的无锁近似快照语义。
 */
unsigned long sched_cpu_util(int cpu)
{
	return effective_cpu_util(cpu, cpu_util_cfs(cpu), NULL, NULL);
}

/*
 * energy_env - Utilization landscape for energy estimation.
 * @task_busy_time: Utilization contribution by the task for which we test the
 *                  placement. Given by eenv_task_busy_time().
 * @pd_busy_time:   Utilization of the whole perf domain without the task
 *                  contribution. Given by eenv_pd_busy_time().
 * @cpu_cap:        Maximum CPU capacity for the perf domain.
 * @pd_cap:         Entire perf domain capacity. (pd->nr_cpus * cpu_cap).
 */
/* EAS 对单个 perf_domain 的 busy time、容量和 task 增量计算工作区。 */
/*
 * energy_env 表示 EAS 评估某个 performance domain 时的利用率景观，由
 * find_energy_efficient_cpu() 在一次候选比较中构造，交给 compute_energy() 消费，生命周期仅限
 * 当前栈帧，不发布给并发读者。
 *
 * task_busy_time 是待放置任务经 IRQ 时间轴修正后的独立贡献，由 eenv_task_busy_time() 写入；
 * pd_busy_time 是从域中移除该任务后的总有效利用率，由 eenv_pd_busy_time() 写入。二者分离保证
 * 把同一任务放到域内任意 CPU 时基线一致。cpu_cap 是域内单 CPU 最大容量，pd_cap 是整个域容量
 * 上限（CPU 数量乘 cpu_cap）；字段均使用调度容量标度，由当前 EAS 计算独占读写，无锁需求。
 */
struct energy_env {
	/* 待评估任务的独立 busy-time 增量，加入候选域时最多计一次。 */
	unsigned long task_busy_time;
	/* 排除待评估任务后的 performance domain 总 busy time。 */
	unsigned long pd_busy_time;
	/* 该 performance domain 内单个 CPU 在 f_max 下的容量上界。 */
	unsigned long cpu_cap;
	/* 整个 performance domain 的聚合容量上界，用于总 busy time 饱和。 */
	unsigned long pd_cap;
};

/*
 * Compute the task busy time for compute_energy(). This time cannot be
 * injected directly into effective_cpu_util() because of the IRQ scaling.
 * The latter only makes sense with the most recent CPUs where the task has
 * run.
 */
/*
 * 为 compute_energy() 计算待放置任务自身的 busy time。不能把该贡献直接注入任意目标 CPU 的
 * effective_cpu_util()，因为其中 IRQ 缩放只对任务最近实际运行的 CPU 时间轴有意义；在陌生 CPU
 * 上使用其 IRQ 压力缩放任务历史会让不同候选得到不一致的任务贡献。
 *
 * 业务背景：find_energy_efficient_cpu() 先以 @prev_cpu 的最近执行环境固定任务贡献，之后各 PD
 * 候选都复用同一增量，保证能耗差只来自放置位置而非估算口径变化。
 * 入参：@eenv 是调用者拥有的输入输出工作区借用指针；@p 是待放置任务的只读借用指针；
 * @prev_cpu 是 p 最近运行的有效 CPU。所有权均不转移。
 * 出参/返回：无直接返回值；写入 @eenv->task_busy_time，范围不超过 prev_cpu 最大容量。
 * 注意事项：函数不睡眠、不持 rq 锁，IRQ 与任务 util 是近似快照；调用者须保证 p/eenv 有效。
 */
static inline void eenv_task_busy_time(struct energy_env *eenv,
				       struct task_struct *p, int prev_cpu)
{
	/* max_cap 固定任务最近 CPU 的容量标度，irq 是该时间轴中 task clock 不可见的压力。 */
	unsigned long busy_time, max_cap = arch_scale_cpu_capacity(prev_cpu);
	unsigned long irq = cpu_util_irq(cpu_rq(prev_cpu));

	/* IRQ 已饱和时没有可可靠缩放的任务时间，保守按满容量；否则只做 IRQ 时间轴缩放。 */
	if (unlikely(irq >= max_cap))
		busy_time = max_cap;
	else
		busy_time = scale_irq_capacity(task_util_est(p), irq, max_cap);

	/* 发布到当前栈工作区，供随后每个候选 placement 的 compute_energy() 复用。 */
	eenv->task_busy_time = busy_time;
}

/*
 * Compute the perf_domain (PD) busy time for compute_energy(). Based on the
 * utilization for each @pd_cpus, it however doesn't take into account
 * clamping since the ratio (utilization / cpu_capacity) is already enough to
 * scale the EM reported power consumption at the (eventually clamped)
 * cpu_capacity.
 *
 * The contribution of the task @p for which we want to estimate the
 * energy cost is removed (by cpu_util()) and must be calculated
 * separately (see eenv_task_busy_time). This ensures:
 *
 *   - A stable PD utilization, no matter which CPU of that PD we want to place
 *     the task on.
 *
 *   - A fair comparison between CPUs as the task contribution (task_util())
 *     will always be the same no matter which CPU utilization we rely on
 *     (util_avg or util_est).
 *
 * Set @eenv busy time for the PD that spans @pd_cpus. This busy time can't
 * exceed @eenv->pd_cap.
 */
/*
 * 计算 @pd_cpus 覆盖的 performance domain 基线 busy time。这里不应用 uclamp：利用率/CPU 容量
 * 比值已经足以在最终钳制后的容量点上缩放 Energy Model 功耗。
 *
 * cpu_util(cpu, p, -1, 0) 会从每个 CPU 的视图扣除待评估任务 p，任务贡献则由
 * eenv_task_busy_time() 单独计算。这带来两个不变量：无论准备把 p 放到域内哪个 CPU，PD 基线都
 * 稳定；无论某 CPU 最终采用 util_avg 还是 util_est，参与比较的 p 自身贡献始终相同。
 *
 * 业务背景：compute_energy() 需要“无 p 的域基线 + 固定 p 增量”才能公平比较多个目标 CPU。
 * 入参：@eenv 是输入输出工作区，入口已设置 pd_cap；@pd_cpus 是只读借用的域 CPU 掩码；
 * @p 是要从基线扣除的任务借用指针。均不转移 ownership。
 * 出参/返回：无直接返回值；写入不超过 @eenv->pd_cap 的 pd_busy_time，其他字段不变。
 * 注意事项：遍历期间不持各 rq 锁且不睡眠，结果是跨 CPU 近似快照；饱和上界防止并发/舍入误差
 * 让 Energy Model 接收到超过整个域容量的 busy time。
 */
static inline void eenv_pd_busy_time(struct energy_env *eenv,
				     struct cpumask *pd_cpus,
				     struct task_struct *p)
{
	/* busy_time 聚合每个 CPU 排除 p 后的有效利用率；cpu 是域掩码游标。 */
	unsigned long busy_time = 0;
	int cpu;

	/* 阶段 1：逐 CPU 扣除 p 后，再合并 RT/DL/IRQ，得到可相加的同标度有效利用率。 */
	for_each_cpu(cpu, pd_cpus) {
		unsigned long util = cpu_util(cpu, p, -1, 0);

		busy_time += effective_cpu_util(cpu, util, NULL, NULL);
	}

	/* 阶段 2：把聚合结果饱和到整个域容量并写回工作区，建立 compute_energy() 的稳定基线。 */
	eenv->pd_busy_time = min(eenv->pd_cap, busy_time);
}

/*
 * Compute the maximum utilization for compute_energy() when the task @p
 * is placed on the cpu @dst_cpu.
 *
 * Returns the maximum utilization among @eenv->cpus. This utilization can't
 * exceed @eenv->cpu_cap.
 */
/*
 * eenv_pd_max_util() - 模拟把 p 放到 dst_cpu 后，求 performance domain 的最高有效利用率。
 *
 * 业务背景：一个 PD 内 CPU 共享频率，Energy Model 以其中需求最高的 CPU 决定 OPP；因此 EAS
 * 不只需要域总 busy time，还要知道候选放置后的峰值频率需求。
 * 入参：@eenv 是已初始化容量上界的借用工作区；@pd_cpus 是域 CPU 掩码；@p 是待放置任务；
 * @dst_cpu 是预测目标，负值表示不加入任务。所有指针仅借用。
 * 出参/返回：返回各 CPU 经 CFS/RT/DL/IRQ、uclamp 与 schedutil 映射后的最大性能需求，并截断到
 * @eenv->cpu_cap；不修改 rq 或任务。
 * 注意事项：函数无锁、不睡眠，逐 CPU 读取近似快照；min/max 是每轮局部输出，只有 dst_cpu
 * 对应 CPU 才叠加任务自身 uclamp，避免把任务约束误施加到整个域的每个 CPU。
 */
static inline unsigned long
eenv_pd_max_util(struct energy_env *eenv, struct cpumask *pd_cpus,
		 struct task_struct *p, int dst_cpu)
{
	unsigned long max_util = 0;
	int cpu;

	/* 遍历域内每个 CPU，模拟 p 只在 dst_cpu 上入队，并求共享频率所需的峰值。 */
	for_each_cpu(cpu, pd_cpus) {
		struct task_struct *tsk = (cpu == dst_cpu) ? p : NULL;
		unsigned long util = cpu_util(cpu, p, dst_cpu, 1);
		unsigned long eff_util, min, max;

		/*
		 * Performance domain frequency: utilization clamping
		 * must be considered since it affects the selection
		 * of the performance domain frequency.
		 * NOTE: in case RT tasks are running, by default the min
		 * utilization can be max OPP.
		 */
		/*
		 * PD 频率必须考虑 utilization clamp；effective_cpu_util() 同时给出 min/max，RT 可运行且
		 * 未启用 uclamp 时 min 默认达到最高 OPP。
		 */
		eff_util = effective_cpu_util(cpu, util, &min, &max);

		/* Task's uclamp can modify min and max value */
		/* 仅预测目标 CPU 额外合并 p 的有效 clamp，因为 p 尚未进入该 rq 聚合值。 */
		if (tsk && uclamp_is_used()) {
			min = max(min, uclamp_eff_value(p, UCLAMP_MIN));

			/*
			 * If there is no active max uclamp constraint,
			 * directly use task's one, otherwise keep max.
			 */
			/* rq 无活动 max clamp 时直接采用 p 的上限；否则保留 rq 与 p 中更宽松的较大需求。 */
			if (uclamp_rq_is_idle(cpu_rq(cpu)))
				max = uclamp_eff_value(p, UCLAMP_MAX);
			else
				max = max(max, uclamp_eff_value(p, UCLAMP_MAX));
		}

		/* 把有效利用率和钳制边界转换为 schedutil 实际请求的性能点，再更新域峰值。 */
		eff_util = sugov_effective_cpu_perf(cpu, eff_util, min, max);
		max_util = max(max_util, eff_util);
	}

	/* 并发快照或舍入可超界，最终限制到单 CPU 最大容量，满足 Energy Model 输入契约。 */
	return min(max_util, eenv->cpu_cap);
}

/*
 * compute_energy(): Use the Energy Model to estimate the energy that @pd would
 * consume for a given utilization landscape @eenv. When @dst_cpu < 0, the task
 * contribution is ignored.
 */
/*
 * compute_energy() - 用 Energy Model 估算某 PD 在指定任务放置方案下的能耗。
 *
 * 业务背景：find_energy_efficient_cpu() 以“无 p”基线和若干候选 dst_cpu 重复调用本函数，比较
 * 增量能耗而不真正迁移任务。
 * 入参：@eenv 是包含域基线、任务增量与容量上界的借用工作区；@pd 是借用的 performance domain；
 * @pd_cpus 是该域在线 CPU 掩码；@p 是待评估任务；@dst_cpu>=0 表示把 p 加到该 CPU，负值表示
 * 忽略任务贡献。所有权不转移。
 * 出参/返回：返回 EM 对峰值利用率、总 busy time 和容量计算出的能耗标度值，并发出 tracepoint；
 * 不修改调度状态。
 * 注意事项：调用者须保证 EM/PD 与掩码生命周期，函数不睡眠；结果是模型估计而非硬件能量计量。
 */
static inline unsigned long
compute_energy(struct energy_env *eenv, struct perf_domain *pd,
	       struct cpumask *pd_cpus, struct task_struct *p, int dst_cpu)
{
	unsigned long max_util = eenv_pd_max_util(eenv, pd_cpus, p, dst_cpu);
	unsigned long busy_time = eenv->pd_busy_time;
	unsigned long energy;

	/* 有具体放置时只把固定任务增量加入一次，并对域总容量饱和；基线调用保持无 p。 */
	if (dst_cpu >= 0)
		busy_time = min(eenv->pd_cap, busy_time + eenv->task_busy_time);

	/* EM 用共享频率所需的 max_util 和域活动量 busy_time 查表/插值得到能耗估计。 */
	energy = em_cpu_energy(pd->em_pd, max_util, busy_time, eenv->cpu_cap);

	/* tracepoint 记录同一组模型输入与结果，便于验证 EAS 决策而不改变选择。 */
	trace_sched_compute_energy_tp(p, dst_cpu, energy, max_util, busy_time);

	return energy;
}

/*
 * find_energy_efficient_cpu(): Find most energy-efficient target CPU for the
 * waking task. find_energy_efficient_cpu() looks for the CPU with maximum
 * spare capacity in each performance domain and uses it as a potential
 * candidate to execute the task. Then, it uses the Energy Model to figure
 * out which of the CPU candidates is the most energy-efficient.
 *
 * The rationale for this heuristic is as follows. In a performance domain,
 * all the most energy efficient CPU candidates (according to the Energy
 * Model) are those for which we'll request a low frequency. When there are
 * several CPUs for which the frequency request will be the same, we don't
 * have enough data to break the tie between them, because the Energy Model
 * only includes active power costs. With this model, if we assume that
 * frequency requests follow utilization (e.g. using schedutil), the CPU with
 * the maximum spare capacity in a performance domain is guaranteed to be among
 * the best candidates of the performance domain.
 *
 * In practice, it could be preferable from an energy standpoint to pack
 * small tasks on a CPU in order to let other CPUs go in deeper idle states,
 * but that could also hurt our chances to go cluster idle, and we have no
 * ways to tell with the current Energy Model if this is actually a good
 * idea or not. So, find_energy_efficient_cpu() basically favors
 * cluster-packing, and spreading inside a cluster. That should at least be
 * a good thing for latency, and this is consistent with the idea that most
 * of the energy savings of EAS come from the asymmetry of the system, and
 * not so much from breaking the tie between identical CPUs. That's also the
 * reason why EAS is enabled in the topology code only for systems where
 * SD_ASYM_CPUCAPACITY is set.
 *
 * NOTE: Forkees are not accepted in the energy-aware wake-up path because
 * they don't have any useful utilization data yet and it's not possible to
 * forecast their impact on energy consumption. Consequently, they will be
 * placed by sched_balance_find_dst_cpu() on the least loaded CPU, which might turn out
 * to be energy-inefficient in some use-cases. The alternative would be to
 * bias new tasks towards specific types of CPUs first, or to try to infer
 * their util_avg from the parent task, but those heuristics could hurt
 * other use-cases too. So, until someone finds a better way to solve this,
 * let's keep things simple by re-using the existing slow path.
 */
/* 在 perf_domain 中比较放置前后能耗，容量/uclamp 不适配的 CPU 不参与候选。 */
/*
 * find_energy_efficient_cpu() - 为唤醒任务选择增量能耗最低且容量适配的 CPU。
 *
 * 每个 performance domain 只选“剩余容量最大”的 CPU 作为代表，再用 Energy Model 比较这些代表。
 * 原因是 PD 内共享频率；请求同一频率的多个 CPU 在只含 active power 的 EM 中无法进一步区分，
 * 而假设 schedutil 让频率随利用率变化时，最大余量 CPU 保证属于该 PD 的最优候选集合。
 *
 * 当前模型无法判断把小任务堆到一个 CPU、让其他 CPU 进入深 idle 是否真的省电；堆叠也可能妨碍
 * cluster idle。因此策略总体是 cluster 间聚合、cluster 内分散，兼顾延迟，并把主要节能收益归于
 * 异构容量而非同构 CPU 间破平局，所以拓扑仅在 SD_ASYM_CPUCAPACITY 系统启用 EAS。
 * fork 新任务没有可靠 util 数据，不能预测能耗，故不进入本唤醒路径，而由普通最轻负载慢路径放置；
 * 继承父任务或偏置某类 CPU 都可能伤害其他工作负载，当前保持这一明确退化边界。
 *
 * 业务背景：select_task_rq_fair() 在 root_domain 未过载的 TTWU 路径调用本函数。
 * 入参：@p 是待唤醒任务的借用指针，亲和性在 pi_lock 下稳定；@prev_cpu 是上次运行 CPU。
 * 出参/返回：返回选中 CPU；无可用 EM/异构域时返回 -1 让调用者继续普通路径；任务无有效需求时
 * 返回 prev_cpu。只读取/同步 PELT 与模型快照，不入队任务。
 * 注意事项：调用者关闭中断并保持 RCU/拓扑对象有效；本函数复用 per-CPU cpumask，不睡眠。
 * 能耗值是近似模型，若并发快照导致候选能耗小于基线则放弃 EAS 并回退 prev_cpu。
 */
static int find_energy_efficient_cpu(struct task_struct *p, int prev_cpu)
{
	/* 变量地图：delta 为加入 p 的增量能耗；fits 为 util/uclamp 适配等级；eenv 为每域工作区。 */
	struct cpumask *cpus = this_cpu_cpumask_var_ptr(select_rq_mask);
	unsigned long prev_delta = ULONG_MAX, best_delta = ULONG_MAX;
	unsigned long p_util_min = uclamp_is_used() ? uclamp_eff_value(p, UCLAMP_MIN) : 0;
	unsigned long p_util_max = uclamp_is_used() ? uclamp_eff_value(p, UCLAMP_MAX) : 1024;
	struct root_domain *rd = this_rq()->rd;
	int cpu, best_energy_cpu, target = -1;
	int prev_fits = -1, best_fits = -1;
	unsigned long best_actual_cap = 0;
	unsigned long prev_actual_cap = 0;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	struct sched_domain *sd;
	struct perf_domain *pd;
	struct energy_env eenv;

	/* 阶段 1：取得 root_domain 的 EM 域链；没有模型就明确返回 -1 交给普通选核。 */
	pd = rcu_dereference_all(rd->pd);
	if (!pd)
		return target;

	/*
	 * Energy-aware wake-up happens on the lowest sched_domain starting
	 * from sd_asym_cpucapacity spanning over this_cpu and prev_cpu.
	 */
	/* 从覆盖当前 CPU 与 prev_cpu 的最低异构域开始，限制 EAS 搜索的拓扑范围。 */
	sd = rcu_dereference_all(*this_cpu_ptr(&sd_asym_cpucapacity));
	while (sd && !cpumask_test_cpu(prev_cpu, sched_domain_span(sd)))
		sd = sd->parent;
	if (!sd)
		return target;

	/* 从这里起失败回退保持 prev_cpu，避免模型不确定时制造迁移。 */
	target = prev_cpu;

	/* 同步任务需求；零 util 且无 uclamp_min 的任务没有可区分能耗信号。 */
	sync_entity_load_avg(&p->se);
	if (!task_util_est(p) && p_util_min == 0)
		return target;

	/* 固定任务自身的 IRQ 修正贡献，后续所有域/CPU 使用同一数值比较。 */
	eenv_task_busy_time(&eenv, p, prev_cpu);

	/* 阶段 2：逐 performance domain 建立在线 CPU 集合并选出 prev 与最大余量代表候选。 */
	for (; pd; pd = pd->next) {
		unsigned long util_min = p_util_min, util_max = p_util_max;
		unsigned long cpu_cap, cpu_actual_cap, util;
		long prev_spare_cap = -1, max_spare_cap = -1;
		unsigned long rq_util_min, rq_util_max;
		unsigned long cur_delta, base_energy;
		int max_spare_cap_cpu = -1;
		int fits, max_fits = -1;

		/* 离线或空域不能参与能耗评估。 */
		if (!cpumask_and(cpus, perf_domain_span(pd), cpu_online_mask))
			continue;

		/* Account external pressure for the energy estimation */
		/* 同一 PD 的 CPU 共享容量等级；实际容量扣除了 thermal/IRQ 等外部压力。 */
		cpu = cpumask_first(cpus);
		cpu_actual_cap = get_actual_cpu_capacity(cpu);

		eenv.cpu_cap = cpu_actual_cap;
		eenv.pd_cap = 0;

		/* 扫描域内 CPU：累加总容量，并只在 sd 与任务亲和性交集中筛选放置候选。 */
		for_each_cpu(cpu, cpus) {
			struct rq *rq = cpu_rq(cpu);

			eenv.pd_cap += cpu_actual_cap;

			if (!cpumask_test_cpu(cpu, sched_domain_span(sd)))
				continue;

			if (!cpumask_test_cpu(cpu, p->cpus_ptr))
				continue;

			util = cpu_util(cpu, p, cpu, 0);
			cpu_cap = capacity_of(cpu);

			/*
			 * Skip CPUs that cannot satisfy the capacity request.
			 * IOW, placing the task there would make the CPU
			 * overutilized. Take uclamp into account to see how
			 * much capacity we can get out of the CPU; this is
			 * aligned with sched_cpu_util().
			 */
			/*
			 * 模拟加入 p 后若容量请求完全不适配则跳过，否则该 CPU 会过载。uclamp 必须与
			 * sched_cpu_util() 口径一致地影响“可得到多少容量”。
			 */
			if (uclamp_is_used() && !uclamp_rq_is_idle(rq)) {
				/*
				 * Open code uclamp_rq_util_with() except for
				 * the clamp() part. I.e.: apply max aggregation
				 * only. util_fits_cpu() logic requires to
				 * operate on non clamped util but must use the
				 * max-aggregated uclamp_{min, max}.
				 */
				/*
				 * 这里展开 uclamp_rq_util_with() 的 max 聚合但故意不 clamp util：util_fits_cpu()
				 * 必须看到原始 util，同时使用 rq 与 p 聚合后的 min/max 才能区分适配类别。
				 */
				rq_util_min = uclamp_rq_get(rq, UCLAMP_MIN);
				rq_util_max = uclamp_rq_get(rq, UCLAMP_MAX);

				util_min = max(rq_util_min, p_util_min);
				util_max = max(rq_util_max, p_util_max);
			}

			fits = util_fits_cpu(util, util_min, util_max, cpu);
			if (!fits)
				continue;

			/* 饱和减法得到放置后的剩余容量，防止并发估算误差下溢。 */
			lsub_positive(&cpu_cap, util);

			if (cpu == prev_cpu) {
				/* Always use prev_cpu as a candidate. */
				/* prev_cpu 永远单独保留，用于比较“维持位置”与迁移的增量收益。 */
				prev_spare_cap = cpu_cap;
				prev_fits = fits;
			} else if ((fits > max_fits) ||
				   ((fits == max_fits) && ((long)cpu_cap > max_spare_cap))) {
				/*
				 * Find the CPU with the maximum spare capacity
				 * among the remaining CPUs in the performance
				 * domain.
				 */
				/* 其余 CPU 先按适配等级、再按剩余容量选该 PD 唯一代表，压缩 EM 计算次数。 */
				max_spare_cap = cpu_cap;
				max_spare_cap_cpu = cpu;
				max_fits = fits;
			}
		}

		/* 本域连 prev 或代表候选都没有时，无需建立能耗基线。 */
		if (max_spare_cap_cpu < 0 && prev_spare_cap < 0)
			continue;

		/* 阶段 3：构造排除 p 的稳定 PD 基线，再计算未放置任务时的能耗。 */
		eenv_pd_busy_time(&eenv, cpus, p);
		/* Compute the 'base' energy of the pd, without @p */
		/* base_energy 是无 p 场景，后续候选都减去它得到可跨域比较的增量。 */
		base_energy = compute_energy(&eenv, pd, cpus, p, -1);

		/* Evaluate the energy impact of using prev_cpu. */
		/* prev_cpu 属于本域且适配时，记录维持原 CPU 的能耗增量与实际容量。 */
		if (prev_spare_cap > -1) {
			prev_delta = compute_energy(&eenv, pd, cpus, p,
						    prev_cpu);
			/* CPU utilization has changed */
			/* 候选能耗小于基线说明并发快照改变，增量无效；立即回退而不比较无符号差。 */
			if (prev_delta < base_energy)
				return target;
			prev_delta -= base_energy;
			prev_actual_cap = cpu_actual_cap;
			best_delta = min(best_delta, prev_delta);
		}

		/* Evaluate the energy impact of using max_spare_cap_cpu. */
		/* 阶段 4：代表候选必须比 prev 余量大，并满足跨域最佳适配/容量/能耗的层级规则。 */
		if (max_spare_cap_cpu >= 0 && max_spare_cap > prev_spare_cap) {
			/* Current best energy cpu fits better */
			/* 已有全局候选适配等级更高时，本域较差候选不能仅靠能耗胜出。 */
			if (max_fits < best_fits)
				continue;

			/*
			 * Both don't fit performance hint (i.e. uclamp_min)
			 * but best energy cpu has better capacity.
			 */
			/* 两者都不满足 uclamp_min 时优先实际容量更大的域，减少性能违约程度。 */
			if ((max_fits < 0) &&
			    (cpu_actual_cap <= best_actual_cap))
				continue;

			cur_delta = compute_energy(&eenv, pd, cpus, p,
						   max_spare_cap_cpu);
			/* CPU utilization has changed */
			/* 与基线倒挂同样表示快照不可比较，回退 prev_cpu。 */
			if (cur_delta < base_energy)
				return target;
			cur_delta -= base_energy;

			/*
			 * Both fit for the task but best energy cpu has lower
			 * energy impact.
			 */
			/* 两者都完整适配时，只有增量能耗更低才替换当前全局最佳。 */
			if ((max_fits > 0) && (best_fits > 0) &&
			    (cur_delta >= best_delta))
				continue;

			best_delta = cur_delta;
			best_energy_cpu = max_spare_cap_cpu;
			best_fits = max_fits;
			best_actual_cap = cpu_actual_cap;
		}
	}

	/*
	 * 阶段 5：最终只在适配更好、都适配但能耗更低，或都欠 uclamp_min 但容量更高时迁移；
	 * 其他情况保持 prev_cpu，避免为没有明确收益的模型平局付出迁移成本。
	 */
	if ((best_fits > prev_fits) ||
	    ((best_fits > 0) && (best_delta < prev_delta)) ||
	    ((best_fits < 0) && (best_actual_cap > prev_actual_cap)))
		target = best_energy_cpu;

	return target;
}

/*
 * select_task_rq_fair: Select target runqueue for the waking task in domains
 * that have the relevant SD flag set. In practice, this is SD_BALANCE_WAKE,
 * SD_BALANCE_FORK, or SD_BALANCE_EXEC.
 *
 * Balances load by selecting the idlest CPU in the idlest group, or under
 * certain conditions an idle sibling CPU if the domain has SD_WAKE_AFFINE set.
 *
 * Returns the target CPU number.
 */
/* 唤醒选核组合 wake-affine、EAS、idle 搜索和 domain idlest；失败保留 prev_cpu。 */
/*
 * select_task_rq_fair() - 为 CFS 的 wake/fork/exec 事件选择目标运行队列。
 *
 * 业务背景：调度类入口把 EAS、wake-affine、调度域最闲组慢路径和 idle sibling 快路径按成本排序。
 * 入参：@p 是 pi_lock 保护下的借用任务；@prev_cpu 是原 CPU；@wake_flags 描述 TTWU/FORK/EXEC、
 * 同步唤醒及当前 CPU 偏好。均为纯输入。
 * 出参/返回：返回亲和性允许的目标 CPU；无策略改善时保持 prev_cpu，不入队任务。
 * 注意事项：必须持 p->pi_lock 以稳定 cpus_ptr，并处于调度域拓扑保护/关中断上下文；不睡眠。
 */
static int
select_task_rq_fair(struct task_struct *p, int prev_cpu, int wake_flags)
{
	/* sync 排除正在退出的 waker；sd 保存最外层支持本事件的慢路径域。 */
	int sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	struct sched_domain *tmp, *sd = NULL;
	int cpu = smp_processor_id();
	int new_cpu = prev_cpu;
	int want_affine = 0;
	/* SD_flags and WF_flags share the first nibble */
	/* WF 与 SD 的低四位按协议对齐，可直接映射本次所需的均衡域标志。 */
	int sd_flag = wake_flags & 0xF;

	/*
	 * required for stable ->cpus_allowed
	 */
	/* pi_lock 防止并发 set_cpus_allowed 改写亲和性，使整个选核过程基于稳定允许集合。 */
	lockdep_assert_held(&p->pi_lock);
	/* 阶段 1：真正唤醒先记录 waker/wakee 关系，再尝试强制当前 CPU 与 EAS。 */
	if (wake_flags & WF_TTWU) {
		record_wakee(p);

		if ((wake_flags & WF_CURRENT_CPU) &&
		    cpumask_test_cpu(cpu, p->cpus_ptr))
			return cpu;

		if (!is_rd_overutilized(this_rq()->rd)) {
			new_cpu = find_energy_efficient_cpu(p, prev_cpu);
			if (new_cpu >= 0)
				return new_cpu;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			new_cpu = prev_cpu;
		}

		want_affine = !wake_wide(p) && cpumask_test_cpu(cpu, p->cpus_ptr);
	}

	/* 阶段 2：沿当前 CPU 域链优先寻找 wake-affine；否则记录支持事件标志的慢路径域。 */
	for_each_domain(cpu, tmp) {
		/*
		 * If both 'cpu' and 'prev_cpu' are part of this domain,
		 * cpu is a valid SD_WAKE_AFFINE target.
		 */
		/* current 与 prev 同属该域时，当前 CPU 才是合法的亲和候选。 */
		if (want_affine && (tmp->flags & SD_WAKE_AFFINE) &&
		    cpumask_test_cpu(prev_cpu, sched_domain_span(tmp))) {
			if (cpu != prev_cpu)
				new_cpu = wake_affine(tmp, p, cpu, prev_cpu, sync);

			sd = NULL; /* Prefer wake_affine over balance flags */
			/* wake-affine 已做低成本二选一，不再执行更昂贵的最闲组慢路径。 */
			break;
		}

		/*
		 * Usually only true for WF_EXEC and WF_FORK, as sched_domains
		 * usually do not have SD_BALANCE_WAKE set. That means wakeup
		 * will usually go to the fast path.
		 */
		/* 通常仅 fork/exec 域带 balance 标志；普通 wake 多数直接进入 idle sibling 快路径。 */
		if (tmp->flags & sd_flag)
			sd = tmp;
		else if (!want_affine)
			break;
	}

	/* Slow path */
	/* 支持事件的域存在时逐层选择最闲组/CPU，主要服务 fork/exec。 */
	if (unlikely(sd))
		return sched_balance_find_dst_cpu(sd, p, cpu, prev_cpu, sd_flag);

	/* Fast path */
	/* TTWU 用 target/prev/LLC/SMT 的空闲搜索缩短唤醒延迟。 */
	if (wake_flags & WF_TTWU)
		return select_idle_sibling(p, prev_cpu, new_cpu);

	return new_cpu;
}

/*
 * Called immediately before a task is migrated to a new CPU; task_cpu(p) and
 * cfs_rq_of(p) references at time of call are still valid and identify the
 * previous CPU. The caller guarantees p->pi_lock or task_rq(p)->lock is held.
 */
/* TASK_WAKING 迁移时把实体 PELT lag 映射到目标时钟，避免携带旧 rq 的时间基准。 */
/*
 * migrate_task_rq_fair() - 在 task_cpu 更新前把 CFS 实体的负载历史转换为可迁移状态。
 *
 * 业务背景：set_task_cpu() 前调用，旧 task_cpu/cfs_rq 仍有效；迁移后目标 rq 必须重新对齐 PELT 时钟。
 * 入参：@p 是待迁移任务借用指针；@new_cpu 是有效目标 CPU，纯输入。
 * 出参/返回：无直接返回值；从旧聚合负载移除实体、修正 PELT lag、清零 last_update_time，并更新
 * NUMA 扫描周期。引用所有权不变。
 * 注意事项：调用者持 p->pi_lock 或 task_rq(p)->lock；不睡眠。正在 rq migration 的实体已由
 * dequeue 路径处理聚合负载，不能重复 remove。
 */
static void migrate_task_rq_fair(struct task_struct *p, int new_cpu)
{
	struct sched_entity *se = &p->se;

	/* 非显式 rq-migrating 路径需先从旧 cfs_rq 的可衰减聚合中摘除。 */
	if (!task_on_rq_migrating(p)) {
		remove_entity_load_avg(se);

		/*
		 * Here, the task's PELT values have been updated according to
		 * the current rq's clock. But if that clock hasn't been
		 * updated in a while, a substantial idle time will be missed,
		 * leading to an inflation after wake-up on the new rq.
		 *
		 * Estimate the missing time from the cfs_rq last_update_time
		 * and update sched_avg to improve the PELT continuity after
		 * migration.
		 */
		/*
		 * p 的 PELT 已按旧 rq 时钟更新；旧 rq 长期未更新会漏掉大量 idle 时间，若原样搬到新 rq，
		 * 唤醒时会虚增利用率。用 cfs_rq last_update_time 估算缺失时间，保持迁移前后衰减连续。
		 */
		migrate_se_pelt_lag(se);
	}

	/* Tell new CPU we are migrated */
	/* 0 是“尚未接入目标 PELT 时间线”的哨兵，目标 enqueue 会以新 rq 时钟重新附着。 */
	se->avg.last_update_time = 0;

	update_scan_period(p, new_cpu);
}

/*
 * task_dead_fair() - 在 CFS 任务最终死亡时解除 delayed 队列与负载跟踪。
 *
 * 业务背景：退出路径在 task 不再运行后调用；sched_delayed 实体即使逻辑睡眠仍可能留在 EEVDF 树。
 * 入参：@p 是即将销毁的任务借用指针。
 * 出参/返回：无直接返回值；必要时在 rq 锁下真正 dequeue，并移除实体 load_avg；无引用转移。
 * 注意事项：锁后必须复查 sched_delayed，因为锁前观察可能与调度路径竞争；函数可能自旋等待锁，
 * 但不执行可睡眠分配。
 */
static void task_dead_fair(struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	/* delayed 实体仍占 EEVDF 队列结构，必须先在所属 rq 锁下摘除。 */
	if (se->sched_delayed) {
		struct rq_flags rf;
		struct rq *rq;

		rq = task_rq_lock(p, &rf);
		/* 取得锁后重检，避免并发路径已完成 delayed dequeue 而重复摘除。 */
		if (se->sched_delayed) {
			update_rq_clock(rq);
			dequeue_entities(rq, se, DEQUEUE_SLEEP | DEQUEUE_DELAYED);
		}
		task_rq_unlock(rq, p, &rf);
	}

	/* 队列关系闭合后解除 PELT 聚合/延迟移除状态，完成实体统计生命周期。 */
	remove_entity_load_avg(se);
}

/*
 * Set the max capacity the task is allowed to run at for misfit detection.
 */
/*
 * set_task_max_allowed_capacity() - 缓存任务亲和性集合可达到的最高原始 CPU 容量。
 *
 * 业务背景：misfit detection 需区分“任务在当前小核不适配但允许迁往大核”和“亲和性只允许小核”。
 * 入参：@p 是 cpus_ptr 已稳定的任务借用指针。
 * 出参/返回：无直接返回值；异构系统把 p->max_allowed_capacity 更新为首个与亲和性交集的容量层；
 * 对称系统无副作用。
 * 注意事项：RCU 保护 asym_cap_list/entry 生命周期；列表按容量策略排序，不取得 entry 引用且不睡眠。
 */
static void set_task_max_allowed_capacity(struct task_struct *p)
{
	struct asym_cap_data *entry;

	if (!sched_asym_cpucap_active())
		return;

	/* 遍历 RCU 发布的容量层，找到任务允许集合能够触达的最高匹配层。 */
	rcu_read_lock();
	list_for_each_entry_rcu(entry, &asym_cap_list, link) {
		cpumask_t *cpumask;

		cpumask = cpu_capacity_span(entry);
		if (!cpumask_intersects(p->cpus_ptr, cpumask))
			continue;

		p->max_allowed_capacity = entry->capacity;
		break;
	}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	rcu_read_unlock();
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}

/*
 * set_cpus_allowed_fair() - 应用新亲和性并同步 CFS 的最大可达容量缓存。
 *
 * 业务背景：通用亲和性字段更新后，misfit 依赖的派生值必须同批刷新。
 * 入参：@p 是待更新任务；@ctx 是借用的亲和性上下文，所有权均不转移。
 * 出参/返回：无直接返回值；修改 p 的允许 CPU 与 max_allowed_capacity。
 * 注意事项：继承调度类 set_cpus_allowed 回调锁契约，不睡眠；顺序不可交换。
 */
static void set_cpus_allowed_fair(struct task_struct *p, struct affinity_context *ctx)
{
	set_cpus_allowed_common(p, ctx);
	set_task_max_allowed_capacity(p);
}

/*
 * set_next_buddy() - 沿任务组层级把实体标记为下一次 EEVDF 选择偏好。
 *
 * 业务背景：唤醒抢占或 yield_to 希望某任务尽快运行，但仍通过每层 cfs_rq->next 提示而非强制入选。
 * 入参：@se 是在队实体借用指针；父链均应仍在相应 rq 上。
 * 出参/返回：无直接返回值；为非 idle 的各层实体写 next buddy；遇到不在队或 idle 层提前停止。
 * 注意事项：调用者持 rq 锁；不睡眠。WARN 暴露把已摘除实体提名为 buddy 的协议错误。
 */
static void set_next_buddy(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		if (WARN_ON_ONCE(!se->on_rq))
			return;
		if (se_is_idle(se))
			return;
		cfs_rq_of(se)->next = se;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}
}

/* 唤醒抢占状态由 wakeup_preempt_fair() 设置并在 switch 中消费，数值表达逐步增强的动作。 */
enum preempt_wakeup_action {
	PREEMPT_WAKEUP_NONE,	/* No preemption. */
	/* 不抢占；例如同步提示尚未超过阈值，保留当前任务。 */
	PREEMPT_WAKEUP_SHORT,	/* Ignore slice protection. */
	/* 忽略当前 slice 保护再让 EEVDF 选择，适用于更短 slice 或非 idle 抢占 idle。 */
	PREEMPT_WAKEUP_PICK,	/* Let __pick_eevdf() decide. */
	/* 保持常规保护，由 __pick_eevdf() 判断 wakee 是否成为最 eligible 实体。 */
	PREEMPT_WAKEUP_RESCHED,	/* Force reschedule. */
	/* 直接请求重调度；同步 wakee 已满足 deadline/运行时阈值时使用。 */
};

/*
 * set_preempt_buddy() - 在不违背更早 deadline 的前提下提名 wakee 为 next buddy。
 * 入参：@cfs_rq 是共同层队列；@wake_flags 当前未消费，仅保持调用接口；@pse 是 wakee 对齐实体；
 * @se 是当前实体，均为借用指针。
 * 出参/返回：成功更新返回 true；已有 next deadline 更早时返回 false。副作用由 set_next_buddy()
 * 沿 pse 父链写提示。
 * 注意事项：调用者持 rq 锁，不睡眠；buddy 是偏好，不绕过 eligibility/deadline。
 */
static inline bool
set_preempt_buddy(struct cfs_rq *cfs_rq, int wake_flags,
		  struct sched_entity *pse, struct sched_entity *se)
{
	/*
	 * Keep existing buddy if the deadline is sooner than pse.
	 * The older buddy may be cache cold and completely unrelated
	 * to the current wakeup but that is unpredictable where as
	 * obeying the deadline is more in line with EEVDF objectives.
	 */
	/* 旧 buddy 即使可能缓存冷，也不能被更晚 deadline 的新 wakee 覆盖，EEVDF 时限优先。 */
	if (cfs_rq->next && entity_before(cfs_rq->next, pse))
		return false;

	set_next_buddy(pse);
	return true;
}

/*
 * WF_SYNC|WF_TTWU indicates the waker expects to sleep but it is not
 * strictly enforced because the hint is either misunderstood or
 * multiple tasks must be woken up.
 */
/*
 * WF_SYNC|WF_TTWU 表示 waker 预计马上睡眠，希望 wakee 接力，但这只是提示：调用者可能误用，
 * 或一次唤醒多个任务，不能无条件强制抢占。
 *
 * preempt_sync() - 根据当前运行时与 deadline 判断是否兑现同步唤醒抢占。
 * 入参：@rq 是锁定运行队列；@wake_flags 含同步/TTWU/选核信息；@pse 是 wakee；@se 是当前实体。
 * 出参/返回：达到条件返回 RESCHED，否则 NONE；不直接设置 need_resched。
 * 注意事项：调用者持 rq 锁，不睡眠；WF_SYNC 缺 TTWU 会告警但继续按提示计算。
 */
static inline enum preempt_wakeup_action
preempt_sync(struct rq *rq, int wake_flags,
	     struct sched_entity *pse, struct sched_entity *se)
{
	u64 threshold, delta;

	/*
	 * WF_SYNC without WF_TTWU is not expected so warn if it happens even
	 * though it is likely harmless.
	 */
	/* 协议期望同步提示只来自真正唤醒；异常组合大多无害但值得暴露调用错误。 */
	WARN_ON_ONCE(!(wake_flags & WF_TTWU));

	/* delta 是当前实体自 exec_start 的运行时；时钟异常回退时饱和为 0。 */
	threshold = sysctl_sched_migration_cost;
	delta = rq_clock_task(rq) - se->exec_start;
	if ((s64)delta < 0)
		delta = 0;

	/*
	 * WF_RQ_SELECTED implies the tasks are stacking on a CPU when they
	 * could run on other CPUs. Reduce the threshold before preemption is
	 * allowed to an arbitrary lower value as it is more likely (but not
	 * guaranteed) the waker requires the wakee to finish.
	 */
	/* 已特意叠放到同 CPU 时更可能存在接力依赖，把抢占等待阈值降为四分之一。 */
	if (wake_flags & WF_RQ_SELECTED)
		threshold >>= 2;

	/*
	 * As WF_SYNC is not strictly obeyed, allow some runtime for batch
	 * wakeups to be issued.
	 */
	/* 仍给 waker 一段批量发出 wakeup 的时间；之后若 wakee deadline 更早才强制重调度。 */
	if (entity_before(pse, se) && delta >= threshold)
		return PREEMPT_WAKEUP_RESCHED;

	return PREEMPT_WAKEUP_NONE;
}

/*
 * Preempt the current task with a newly woken task if needed:
 */
/* 只在唤醒实体更 eligible/更早 deadline 时请求重调度，并维护 next buddy 提示。 */
/*
 * wakeup_preempt_fair() - 在 rq 锁下判断新唤醒 CFS 任务是否应抢占当前 donor。
 *
 * 业务背景：enqueue 后调用；函数把 idle policy、EEVDF eligibility/deadline、slice protection、
 * fork/delayed 限制和 buddy 提示合并为一次抢占裁决。
 * 入参：@rq 是已锁运行队列；@p 是已入队 wakee 借用指针；@wake_flags 描述唤醒语义。
 * 出参/返回：无直接返回值；必要时取消当前 slice 保护、维护 buddy 并 lazy 设置重调度标志。
 * 注意事项：不睡眠；p 可能因组带宽 throttle 而已不可运行，必须先排除。函数只请求调度，实际
 * 上下文切换发生在后续调度点。
 */
static void wakeup_preempt_fair(struct rq *rq, struct task_struct *p, int wake_flags)
{
	enum preempt_wakeup_action preempt_action = PREEMPT_WAKEUP_PICK;
	struct task_struct *donor = rq->donor;
	struct sched_entity *nse, *se = &donor->se, *pse = &p->se;
	struct cfs_rq *cfs_rq = task_cfs_rq(donor);
	int cse_is_idle, pse_is_idle;

	/*
	 * XXX Getting preempted by higher class, try and find idle CPU?
	 */
	/* 非 fair wakee 由更高调度类自己的抢占规则处理，本函数不跨类裁决。 */
	if (p->sched_class != &fair_sched_class)
		return;

	if (unlikely(se == pse))
		return;

	/*
	 * This is possible from callers such as attach_tasks(), in which we
	 * unconditionally wakeup_preempt() after an enqueue (which may have
	 * lead to a throttle).  This both saves work and prevents false
	 * next-buddy nomination below.
	 */
	/* attach_tasks 等会无条件调用；若 enqueue 导致 throttle，早退既省工作也避免错误 buddy。 */
	if (task_is_throttled(p))
		return;

	/*
	 * We can come here with TIF_NEED_RESCHED already set from new task
	 * wake up path.
	 *
	 * Note: this also catches the edge-case of curr being in a throttled
	 * group (e.g. via set_curr_task), since update_curr() (in the
	 * enqueue of curr) will have resulted in resched being set.  This
	 * prevents us from potentially nominating it as a false LAST_BUDDY
	 * below.
	 */
	/* need_resched 已置位时不重复裁决；这也覆盖 curr 所在组刚被 throttle 的边界。 */
	if (test_tsk_need_resched(rq->curr))
		return;

	if (!sched_feat(WAKEUP_PREEMPTION))
		return;

	/* 把当前与 wakee 提升到共同 task-group 层级，确保 deadline 比较属于同一 cfs_rq。 */
	find_matching_se(&se, &pse);
	WARN_ON_ONCE(!pse);

	cse_is_idle = se_is_idle(se);
	pse_is_idle = se_is_idle(pse);

	/*
	 * Preempt an idle entity in favor of a non-idle entity (and don't preempt
	 * in the inverse case).
	 */
	/* 非 idle 实体优先于 idle；反向则禁止抢占，二者同类才继续 EEVDF 比较。 */
	if (cse_is_idle && !pse_is_idle) {
		/*
		 * When non-idle entity preempt an idle entity,
		 * don't give idle entity slice protection.
		 */
		/* idle 当前实体不应以 slice protection 阻挡正常任务，转入 SHORT 选择。 */
		preempt_action = PREEMPT_WAKEUP_SHORT;
		goto preempt;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	if (cse_is_idle != pse_is_idle)
		return;

	/*
	 * BATCH and IDLE tasks do not preempt others.
	 */
	/* 非 normal policy 的 wakee 不主动抢占，维持 batch/idle 的低干扰语义。 */
	if (unlikely(!normal_policy(p->policy)))
		return;

	cfs_rq = cfs_rq_of(se);
	/* 先结算当前运行时间/deadline，后续比较必须基于最新 EEVDF 状态。 */
	update_curr(cfs_rq);
	/*
	 * If @p has a shorter slice than current and @p is eligible, override
	 * current's slice protection in order to allow preemption.
	 */
	/* eligible 的短 slice wakee 可忽略当前保护，让延迟敏感短任务尽快接受 EEVDF 选择。 */
	if (sched_feat(PREEMPT_SHORT) && (pse->slice < se->slice)) {
		preempt_action = PREEMPT_WAKEUP_SHORT;
		goto pick;
	}

	/*
	 * Ignore wakee preemption on WF_FORK as it is less likely that
	 * there is shared data as exec often follow fork. Do not
	 * preempt for tasks that are sched_delayed as it would violate
	 * EEVDF to forcibly queue an ineligible task.
	 */
	/* fork 通常紧接 exec、共享数据弱；delayed 实体又尚不 eligible，二者都不强制抢占。 */
	if ((wake_flags & WF_FORK) || pse->sched_delayed)
		return;

	/* Prefer picking wakee soon if appropriate. */
	if (sched_feat(NEXT_BUDDY) &&
	    set_preempt_buddy(cfs_rq, wake_flags, pse, se)) {

		/*
		 * Decide whether to obey WF_SYNC hint for a new buddy. Old
		 * buddies are ignored as they may not be relevant to the
		 * waker and less likely to be cache hot.
		 */
		/* 只有本次成功建立的新 buddy 才解释 WF_SYNC；旧 buddy 可能与当前 waker 完全无关。 */
		if (wake_flags & WF_SYNC)
			preempt_action = preempt_sync(rq, wake_flags, pse, se);
	}

	/* 将策略状态落实为早退、强制 resched 或进入 EEVDF pick；SHORT 与 PICK 共用选择阶段。 */
	switch (preempt_action) {
	case PREEMPT_WAKEUP_NONE:
		return;
	case PREEMPT_WAKEUP_RESCHED:
		goto preempt;
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
	case PREEMPT_WAKEUP_SHORT:
		fallthrough;
	case PREEMPT_WAKEUP_PICK:
		break;
	}

pick:
	/* 重新选择最 eligible 实体；SHORT 参数关闭当前 slice 保护。 */
	nse = pick_next_entity(rq, cfs_rq, preempt_action != PREEMPT_WAKEUP_SHORT);
	/* If @p has become the most eligible task, force preemption */
	if (nse == pse)
		goto preempt;

	/*
	 * Because p is enqueued, nse being null can only mean that we
	 * dequeued a delayed task. If there are still entities queued in
	 * cfs, check if the next one will be p.
	 */
	/* nse 为空但仍有 queued，说明选择过程只摘除了 delayed 实体；重试直到得到实体或队列耗尽。 */
	if (!nse && cfs_rq->nr_queued)
		goto pick;

	/* 未抢占时按特性更新当前实体的 parity/slice 保护，维持 EEVDF 防抖。 */
	if (sched_feat(RUN_TO_PARITY))
		update_protect_slice(cfs_rq, se);

	return;

preempt:
	/* SHORT 抢占必须撤销旧保护和 buddy，防止下一轮又被陈旧提示拉回。 */
	if (preempt_action == PREEMPT_WAKEUP_SHORT) {
		cancel_protect_slice(se);
		clear_buddies(cfs_rq, se);
	}

	resched_curr_lazy(rq);
}

/* 从根 cfs_rq 逐层下降到 task；途中 delayed 实体阻塞后从根重新选择。 */
/*
 * pick_task_fair() - 在 rq 锁下从 CFS 层级选择下一个可运行 task，空队列时尝试 newidle 拉取。
 * 入参：@rq 是已锁运行队列；@rf 是可供 newidle balance 临时解锁/重锁的 flags 工作区。
 * 出参/返回：返回借用 task；需重试整个 class 选择时返回 RETRY_TASK；确实无任务返回 NULL。
 * 注意事项：必须持 rq 锁，不睡眠；组层下降中 delayed dequeue 可改变树，需从根重启。
 */
struct task_struct *pick_task_fair(struct rq *rq, struct rq_flags *rf)
	__must_hold(__rq_lockp(rq))
{
	struct sched_entity *se;
	struct cfs_rq *cfs_rq;
	struct task_struct *p;
	bool throttled;
	int new_tasks;

again:
	/* 每次重试都从根 cfs_rq 开始，避免沿用被 delayed/throttle 改变的子树。 */
	cfs_rq = &rq->cfs;
	if (!cfs_rq->nr_queued)
		goto idle;

	throttled = false;

	/* 逐层选择 EEVDF 实体，group entity 指向下一层 cfs_rq，叶实体最终映射为 task。 */
	do {
		/* Might not have done put_prev_entity() */
		/* class 快速选择可能尚未 put_prev；curr 仍在队时先结算其运行时。 */
		if (cfs_rq->curr && cfs_rq->curr->on_rq)
			update_curr(cfs_rq);

		se = pick_next_entity(rq, cfs_rq, true);
		if (!se)
			goto again;
		cfs_rq = group_cfs_rq(se);
	} while (cfs_rq);

	/* 到达叶层后 se 的 ownership 仍属 rq，返回 task 借用指针。 */
	p = task_of(se);
	if (unlikely(throttled))
		task_throttle_setup_work(p);
	return p;

idle:
	/* core scheduling 自行协调 sibling，不能在此解锁做 newidle balance。 */
	if (sched_core_enabled(rq))
		return NULL;

	/* newidle balance 可能拉入任务、要求全类重试，或确认仍空闲。 */
	new_tasks = sched_balance_newidle(rq, rf);
	if (new_tasks < 0)
		return RETRY_TASK;
	if (new_tasks > 0)
		goto again;
	return NULL;
}

/*
 * fair_server_pick_task() - deadline fair-server 回调，复用普通 CFS 选取逻辑。
 * 入参：@dl_se 是绑定 rq 的 server 实体；@rf 为 rq flags 工作区。
 * 出参/返回：透传 pick_task_fair() 的 task/NULL/RETRY_TASK；无额外副作用。
 * 注意事项：调用者持 dl_se->rq 锁，不睡眠。
 */
static struct task_struct *
fair_server_pick_task(struct sched_dl_entity *dl_se, struct rq_flags *rf)
	__must_hold(__rq_lockp(dl_se->rq))
{
	return pick_task_fair(dl_se->rq, rf);
}

/*
 * fair_server_init() - 初始化 rq 的 CFS deadline server 并绑定取任务回调。
 * 入参：@rq 是构造期运行队列借用指针。
 * 出参/返回：无直接返回值；初始化 rq->fair_server 并关联 rq/callback。
 * 注意事项：仅初始化阶段调用，发布前完成，不睡眠且不转移 rq ownership。
 */
void fair_server_init(struct rq *rq)
{
	struct sched_dl_entity *dl_se = &rq->fair_server;

	init_dl_entity(dl_se);

	dl_server_init(dl_se, rq, fair_server_pick_task);
}

/*
 * Account for a descheduled task:
 */
/* 从 task 向根逐层结算并把仍 runnable 的 curr 放回 EEVDF 树。 */
/*
 * put_prev_task_fair() - 结算离开 CPU 的 fair task，并沿组层级恢复可选择状态。
 * 入参：@rq 是已锁队列；@prev 是被换出的 donor；@next 可空，若同为 fair 用于跳过公共层重复工作。
 * 出参/返回：无直接返回值；更新运行时、deadline 与 EEVDF 树位置，不转移引用。
 * 注意事项：持 rq 锁、不睡眠；CONFIG_FAIR_GROUP_SCHED 下只处理 prev/next 分叉前的必要层级。
 */
static void put_prev_task_fair(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	struct sched_entity *se = &prev->se;
	struct cfs_rq *cfs_rq;
	struct sched_entity *nse = NULL;

#ifdef CONFIG_FAIR_GROUP_SCHED
	if (next && next->sched_class == &fair_sched_class)
		nse = &next->se;
#endif

	/* 从叶向根结算；若 next 共享同组，公共祖先可留给后续 set_next 避免无谓出入树。 */
	while (se) {
		cfs_rq = cfs_rq_of(se);
		if (!nse || cfs_rq->curr)
			put_prev_entity(cfs_rq, se);
#ifdef CONFIG_FAIR_GROUP_SCHED
		if (nse) {
			if (is_same_group(se, nse))
				/* 两条实体链已汇合，该层以上无需重复 put。 */
				break;

			int d = nse->depth - se->depth;
			if (d >= 0) {
				/* nse has equal or greater depth, ascend */
				/* 先提升更深或同深的 next 链，对齐层级后再比较共同组。 */
				nse = parent_entity(nse);
				/* if nse is the deeper, do not ascend se */
				if (d > 0)
					continue;
			}
		}
#endif
		se = parent_entity(se);
	}
}

/*
 * sched_yield() is very simple
 */
/* yield 放弃 slice 保护并推进 deadline，再重排当前实体；不保证其他任务立即运行。 */
/*
 * yield_task_fair() - 实现 SCHED_OTHER/BATCH 的主动让出提示。
 * 入参：@rq 是已锁当前运行队列。
 * 出参/返回：无直接返回值；清 buddy、结算 curr，并在 eligible 时放弃剩余虚拟 slice。
 * 注意事项：单任务时无效；core scheduling 可能选择 ineligible task，故不能无条件推进 vruntime。
 */
static void yield_task_fair(struct rq *rq)
{
	struct task_struct *curr = rq->donor;
	struct cfs_rq *cfs_rq = task_cfs_rq(curr);
	struct sched_entity *se = &curr->se;

	/*
	 * Are we the only task in the tree?
	 */
	/* 没有竞争者时让出不会改变选择，避免破坏自身 deadline。 */
	if (unlikely(rq->nr_running == 1))
		return;

	clear_buddies(cfs_rq, se);

	update_rq_clock(rq);
	/*
	 * Update run-time statistics of the 'current'.
	 */
	/* 先把让出前已执行时间计入 EEVDF/PELT，后续 deadline 更新才基于真实状态。 */
	update_curr(cfs_rq);
	/*
	 * Tell update_rq_clock() that we've just updated,
	 * so we don't do microscopic update in schedule()
	 * and double the fastpath cost.
	 */
	/* 标记 rq clock 已更新，避免 schedule() 做微小重复更新而扩大热路径成本。 */
	rq_clock_skip_update(rq);

	/*
	 * Forfeit the remaining vruntime, only if the entity is eligible. This
	 * condition is necessary because in core scheduling we prefer to run
	 * ineligible tasks rather than force idling. If this happens we may
	 * end up in a loop where the core scheduler picks the yielding task,
	 * which yields immediately again; without the condition the vruntime
	 * ends up quickly running away.
	 */
	/*
	 * 只有 eligible 实体才把 vruntime 推到 deadline 并申请新 slice。core scheduling 可能为避免
	 * 强制 idle 而运行 ineligible task；若它每次 yield 都推进，会被反复选中并令 vruntime 失控。
	 */
	if (entity_eligible(cfs_rq, se)) {
		se->vruntime = se->deadline;
		update_deadline(cfs_rq, se);
	}
}

/*
 * yield_to_task_fair() - 让当前任务让出并提名指定 fair task 下一次运行。
 * 入参：@rq 是已锁队列；@p 是目标任务借用指针。
 * 出参/返回：p 不在队（含 throttle）返回 false；成功设置 buddy 并执行当前任务 yield 后返回 true。
 * 注意事项：buddy 仍是提示，EEVDF eligibility 可阻止 p 立即运行；不睡眠。
 */
static bool yield_to_task_fair(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	/* !se->on_rq also covers throttled task */
/* 实体不在 rq 上的情况也包含层级被节流。 */
	if (!se->on_rq)
		return false;

	/* Tell the scheduler that we'd really like se to run next. */
/* 向调度器提示优先运行 se，但不绕过资格和节流规则。 */
	set_next_buddy(se);

	yield_task_fair(rq);

	return true;
}

/**************************************************
 * Fair scheduling class load-balancing methods.
 *
 * BASICS
 *
 * The purpose of load-balancing is to achieve the same basic fairness the
 * per-CPU scheduler provides, namely provide a proportional amount of compute
 * time to each task. This is expressed in the following equation:
 *
 *   W_i,n/P_i == W_j,n/P_j for all i,j                               (1)
 *
 * Where W_i,n is the n-th weight average for CPU i. The instantaneous weight
 * W_i,0 is defined as:
 *
 *   W_i,0 = \Sum_j w_i,j                                             (2)
 *
 * Where w_i,j is the weight of the j-th runnable task on CPU i. This weight
 * is derived from the nice value as per sched_prio_to_weight[].
 *
 * The weight average is an exponential decay average of the instantaneous
 * weight:
 *
 *   W'_i,n = (2^n - 1) / 2^n * W_i,n + 1 / 2^n * W_i,0               (3)
 *
 * C_i is the compute capacity of CPU i, typically it is the
 * fraction of 'recent' time available for SCHED_OTHER task execution. But it
 * can also include other factors [XXX].
 *
 * To achieve this balance we define a measure of imbalance which follows
 * directly from (1):
 *
 *   imb_i,j = max{ avg(W/C), W_i/C_i } - min{ avg(W/C), W_j/C_j }    (4)
 *
 * We them move tasks around to minimize the imbalance. In the continuous
 * function space it is obvious this converges, in the discrete case we get
 * a few fun cases generally called infeasible weight scenarios.
 *
 * [XXX expand on:
 *     - infeasible weights;
 *     - local vs global optima in the discrete case. ]
 *
 *
 * SCHED DOMAINS
 *
 * In order to solve the imbalance equation (4), and avoid the obvious O(n^2)
 * for all i,j solution, we create a tree of CPUs that follows the hardware
 * topology where each level pairs two lower groups (or better). This results
 * in O(log n) layers. Furthermore we reduce the number of CPUs going up the
 * tree to only the first of the previous level and we decrease the frequency
 * of load-balance at each level inversely proportional to the number of CPUs in
 * the groups.
 *
 * This yields:
 *
 *     log_2 n     1     n
 *   \Sum       { --- * --- * 2^i } = O(n)                            (5)
 *     i = 0      2^i   2^i
 *                               `- size of each group
 *         |         |     `- number of CPUs doing load-balance
 *         |         `- freq
 *         `- sum over all levels
 *
 * Coupled with a limit on how many tasks we can migrate every balance pass,
 * this makes (5) the runtime complexity of the balancer.
 *
 * An important property here is that each CPU is still (indirectly) connected
 * to every other CPU in at most O(log n) steps:
 *
 * The adjacency matrix of the resulting graph is given by:
 *
 *             log_2 n
 *   A_i,j = \Union     (i % 2^k == 0) && i / 2^(k+1) == j / 2^(k+1)  (6)
 *             k = 0
 *
 * And you'll find that:
 *
 *   A^(log_2 n)_i,j != 0  for all i,j                                (7)
 *
 * Showing there's indeed a path between every CPU in at most O(log n) steps.
 * The task movement gives a factor of O(m), giving a convergence complexity
 * of:
 *
 *   O(nm log n),  n := nr_cpus, m := nr_tasks                        (8)
 *
 *
 * WORK CONSERVING
 *
 * In order to avoid CPUs going idle while there's still work to do, new idle
 * balancing is more aggressive and has the newly idle CPU iterate up the domain
 * tree itself instead of relying on other CPUs to bring it work.
 *
 * This adds some complexity to both (5) and (8) but it reduces the total idle
 * time.
 *
 * [XXX more?]
 *
 *
 * CGROUPS
 *
 * Cgroups make a horror show out of (2), instead of a simple sum we get:
 *
 *                                s_k,i
 *   W_i,0 = \Sum_j \Prod_k w_k * -----                               (9)
 *                                 S_k
 *
 * Where
 *
 *   s_k,i = \Sum_j w_i,j,k  and  S_k = \Sum_i s_k,i                 (10)
 *
 * w_i,j,k is the weight of the j-th runnable task in the k-th cgroup on CPU i.
 *
 * The big problem is S_k, its a global sum needed to compute a local (W_i)
 * property.
 *
 * [XXX write more on how we solve this.. _after_ merging pjt's patches that
 *      rewrite all of this once again.]
 */
/*
 * CFS 跨 CPU 负载均衡要恢复单 CPU 调度器的比例公平：任意 CPU i、j 的衰减权重 W 与处理能力 P
 * 之比应相等（式 1）。瞬时 W_i,0 是该 CPU 所有 runnable task 权重 w_i,j 之和（式 2），权重由
 * nice 经 sched_prio_to_weight[] 得到；长期 W_i,n 用式 3 的指数衰减平均平滑。C_i 通常表示近期
 * 可供 SCHED_OTHER 的时间比例，也可能合并其他容量因素。式 4 以平均 W/C 与两端 W_i/C_i 的
 * 最大/最小差定义 imbalance，迁移任务使其减小；连续空间会收敛，离散任务权重会产生 infeasible
 * weight、局部/全局最优等尚待原文扩展的问题。
 *
 * 为避免所有 CPU 两两比较的 O(n^2)，sched_domain 按硬件拓扑组成 O(log n) 层树；每层只让下层
 * 首个 CPU 向上均衡，并按组规模反比降低频率。式 5 把层数、执行 CPU 数、频率和组大小相乘后
 * 得到每轮 O(n)，再限制每轮迁移任务数。式 6/7 的邻接关系保证任意 CPU 最多 O(log n) 跳可达，
 * 加上 m 个任务移动后总体收敛复杂度为 O(n*m*log n)（式 8）。
 *
 * 为保持 work-conserving，新空闲 CPU 不等待别的 CPU 推送任务，而是主动沿域树向上拉取；这会
 * 增加式 5/8 的常数和边界复杂度，却减少仍有 runnable work 时的空闲。cgroup 又把简单权重和改成
 * 式 9：每个任务权重乘各层组在本 CPU 的份额 s_k,i/S_k；式 10 中 S_k 是跨 CPU 全局和，却要用于
 * 本地 W_i，构成组调度负载传播的核心难题。原文关于 work-conserving 细节、infeasible weights
 * 以及合并后续重写补充 cgroup 解法的 XXX 均仍是未完成 TODO，不能把当前说明视为最终证明。
 */

/* 最大均衡周期的只读热点全局值；拓扑/CPU 数变化路径更新，普通 balance 路径频繁读取。 */
static unsigned long __read_mostly max_load_balance_interval = HZ/10;

/*
 * fbq_type 控制 find-busiest-queue 的任务范围：regular 只看常规本地任务，remote 聚焦远端 LLC
 * 偏好任务，all 不作类别过滤；load-balance 入口设置，rq 分类 helper 消费。
 */
enum fbq_type { regular, remote, all };

/*
 * 'group_type' describes the group of CPUs at the moment of load balancing.
 *
 * The enum is ordered by pulling priority, with the group with lowest priority
 * first so the group_type can simply be compared when selecting the busiest
 * group. See update_sd_pick_busiest().
 */
/* 枚举从低到高按“应被拉取”的优先级排序，update_sd_pick_busiest() 可直接比较数值选最忙组。 */
enum group_type {
	/* The group has spare capacity that can be used to run more tasks.  */
	/* 尚有容量，可接收更多任务，是最低拉取优先级。 */
	group_has_spare = 0,
	/*
	 * The group is fully used and the tasks don't compete for more CPU
	 * cycles. Nevertheless, some tasks might wait before running.
	 */
	/* 容量已充分利用但任务不争抢额外周期，仍可能因离散排队短暂等待。 */
	group_fully_busy,
	/*
	 * One task doesn't fit with CPU's capacity and must be migrated to a
	 * more powerful CPU.
	 */
	/* 至少一个 misfit task 需迁往更强 CPU，容量修复优先于普通余量。 */
	group_misfit_task,
	/*
	 * Balance SMT group that's fully busy. Can benefit from migration
	 * a task on SMT with busy sibling to another CPU on idle core.
	 */
	/* SMT 组满忙；把忙 sibling 上任务迁到空闲整核可减少共享执行资源争用。 */
	group_smt_balance,
	/*
	 * SD_ASYM_PACKING only: One local CPU with higher capacity is available,
	 * and the task should be migrated to it instead of running on the
	 * current CPU.
	 */
	/* 仅 ASYM_PACKING：本地有更高优先/容量 CPU，应把任务拉回而非留在当前 CPU。 */
	group_asym_packing,
	/*
	 * The tasks' affinity constraints previously prevented the scheduler
	 * from balancing the load across the system.
	 */
	/* 历史亲和性约束阻止过均衡，需要更积极路径修复残留不平衡。 */
	group_imbalanced,
	/*
	 * There are tasks running on non-preferred LLC, possible to move
	 * them to their preferred LLC without creating too much imbalance.
	 * The priority of group_llc_balance is lower than that of
	 * group_overloaded and higher than that of all other group types.
	 * This is because group_llc_balance may exacerbate load imbalance.
	 * If the LLC balancing attempt fails, the nr_balance_failed
	 * mechanism will trigger other group types to rebalance the load.
	 */
	/* LLC 偏好迁移可能加剧负载不均，故仅低于 overloaded、高于其他类型；失败计数会触发后续修复。 */
	group_llc_balance,
	/*
	 * The CPU is overloaded and can't provide expected CPU cycles to all
	 * tasks.
	 */
	/* 任务总需求超过 CPU 能提供的周期，是最高普通拉取优先级。 */
	group_overloaded
};

/* imbalance 的单位/对象协议：load 权重、util 容量、任务数、单个 misfit、或单个 LLC 偏好任务。 */
enum migration_type {
	migrate_load = 0,
	migrate_util,
	migrate_task,
	migrate_misfit,
	migrate_llc_task
};

/* lb_env.flags：全部任务被亲和性钉住、需中断扫描、目标被钉住、部分钉住、主动均衡、LLC 钉住。 */
#define LBF_ALL_PINNED	0x01
#define LBF_NEED_BREAK	0x02
#define LBF_DST_PINNED  0x04
#define LBF_SOME_PINNED	0x08
#define LBF_ACTIVE_LB	0x10
#define LBF_LLC_PINNED	0x20

/* 一轮 domain balance 的锁外统计与锁内迁移上下文；src_rq 可在循环中变化。 */
/*
 * lb_env 由 sched_balance_rq()/newidle 路径在栈上创建，贯穿选最忙组、选源 rq、detach/attach。
 * @sd 是当前域；src_rq/src_cpu 是本轮拉取源，dst_rq/dst_cpu 是目标；dst_core_idle 记录目标整核
 * 是否空闲。dst_grpmask 限定目标组，new_dst_cpu 用于亲和性迫使的目标重定向，idle 描述目标忙闲
 * 场景。imbalance 的单位由 migration_type 决定。cpus 是仍可检查集合；flags 记录 pinned/break/
 * active 状态；loop/loop_break/loop_max 限制锁内扫描成本；fbq_type 过滤队列类别；tasks 是已 detach
 * 但尚未 attach 的临时所有权链。环境只在一次均衡调用有效，rq 锁保护队列变更，列表最终必须清空。
 */
struct lb_env {
	/* 当前调度域及可变源运行队列。 */
	struct sched_domain	*sd;

	struct rq		*src_rq;
	int			src_cpu;

	/* 固定目标 CPU/rq 与其物理核空闲快照。 */
	int			dst_cpu;
	struct rq		*dst_rq;
	bool			dst_core_idle;

	/* 目标组掩码、亲和性重定向目标、目标 idle 类型和剩余迁移量。 */
	struct cpumask		*dst_grpmask;
	int			new_dst_cpu;
	enum cpu_idle_type	idle;
	long			imbalance;
	/* The set of CPUs under consideration for load-balancing */
	/* 本轮仍允许从中选择源队列的 CPU 集合，可能因 pinned/失败逐步清除。 */
	struct cpumask		*cpus;

	unsigned int		flags;

	/* 锁内任务扫描计数、主动 break 阈值和硬上限，防止长时间占用 src rq 锁。 */
	unsigned int		loop;
	unsigned int		loop_break;
	unsigned int		loop_max;

	/* 队列过滤类别、imbalance 单位协议，以及 detach 后由目标接管前的任务链。 */
	enum fbq_type		fbq_type;
	enum migration_type	migration_type;
	struct list_head	tasks;
};

/*
 * Is this task likely cache-hot:
 */
/*
 * task_hot() - 判断迁移 p 是否可能损失热缓存或违反 core cookie。
 * 入参：@p 是 src_rq 上候选任务；@env 是当前均衡环境借用指针。
 * 出参/返回：1 表示应视为 hot、暂缓普通迁移；0 表示可迁移。无状态副作用。
 * 注意事项：必须持 env->src_rq 锁，不睡眠；migration_cost=-1/0 分别强制全 hot/全 cold，cookie
 * 不匹配也借用 hot 返回阻止非法目标放置。
 */
static int task_hot(struct task_struct *p, struct lb_env *env)
{
	s64 delta;

	lockdep_assert_rq_held(env->src_rq);

	/* 非 fair、idle policy 或同 SMT 共享缓存均无需按普通 CFS cache-hot 阻止。 */
	if (p->sched_class != &fair_sched_class)
		return 0;

	if (unlikely(task_has_idle_policy(p)))
		return 0;

	/* SMT siblings share cache */
/* SMT 兄弟共享缓存，应作为同一缓存局部性范围处理。 */
	if (env->sd->flags & SD_SHARE_CPUCAPACITY)
		return 0;

	/*
	 * Buddy candidates are cache hot:
	 */
	/* next buddy 预期很快运行且可能保有工作集，目标 rq 有负载时保留它。 */
	if (sched_feat(CACHE_HOT_BUDDY) && env->dst_rq->nr_running &&
	    (&p->se == cfs_rq_of(&p->se)->next))
		return 1;

	if (sysctl_sched_migration_cost == -1)
		return 1;

	/*
	 * Don't migrate task if the task's cookie does not match
	 * with the destination CPU's core cookie.
	 */
	/* core scheduling cookie 不匹配属于隔离约束，复用 hot 结果拒绝迁移。 */
	if (!sched_core_cookie_match(cpu_rq(env->dst_cpu), p))
		return 1;

	if (sysctl_sched_migration_cost == 0)
		return 0;

	/* 最近执行距离小于 migration_cost 时认为缓存尚热。 */
	delta = rq_clock_task(env->src_rq) - p->se.exec_start;

	return delta < (s64)sysctl_sched_migration_cost;
}

#ifdef CONFIG_NUMA_BALANCING
/*
 * Returns a positive value, if task migration degrades locality.
 * Returns 0, if task migration is not affected by locality.
 * Returns a negative value, if task migration improves locality i.e migration preferred.
 */
/*
 * migrate_degrades_locality() - 评估 src→dst 迁移对 NUMA 内存局部性的方向与强度。
 * 入参：@p 是候选任务；@env 提供源/目标 CPU、域和 idle 场景，均借用。
 * 出参/返回：正值表示恶化、0 表示中性、负值表示改善；不修改 NUMA 状态。
 * 注意事项：在 RCU 可安全读取 numa_group 的上下文调用，不睡眠；CPU_IDLE 为避免空转可覆盖
 * 轻微局部性损失。
 */
static long migrate_degrades_locality(struct task_struct *p, struct lb_env *env)
{
	struct numa_group *numa_group = rcu_dereference_all(p->numa_group);
	unsigned long src_weight, dst_weight;
	int src_nid, dst_nid, dist;

	/* 未启用 NUMA、无 fault 样本、域非 NUMA 或节点相同，都没有可靠差异。 */
	if (!static_branch_likely(&sched_numa_balancing))
		return 0;

	if (!p->numa_faults || !(env->sd->flags & SD_NUMA))
		return 0;

	src_nid = cpu_to_node(env->src_cpu);
	dst_nid = cpu_to_node(env->dst_cpu);

	if (src_nid == dst_nid)
		return 0;

	/* Migrating away from the preferred node is always bad. */
	/* 离开 preferred node 通常恶化；源 rq 尚有非 preferred 任务时更应保留 p。 */
	if (src_nid == p->numa_preferred_nid) {
		if (env->src_rq->nr_running > env->src_rq->nr_preferred_running)
			return 1;
		else
			return 0;
	}

	/* Encourage migration to the preferred node. */
	/* 迁入 preferred node 明确改善，负值让迁移优先。 */
	if (dst_nid == p->numa_preferred_nid)
		return -1;

	/* Leaving a core idle is often worse than degrading locality. */
	/* 新空闲 CPU 拉取时 work-conserving 优先，避免为局部性让整核空转。 */
	if (env->idle == CPU_IDLE)
		return 0;

	dist = node_distance(src_nid, dst_nid);
	if (numa_group) {
		src_weight = group_weight(p, src_nid, dist);
		dst_weight = group_weight(p, dst_nid, dist);
	} else {
		src_weight = task_weight(p, src_nid, dist);
		dst_weight = task_weight(p, dst_nid, dist);
	}

	/* fault 权重越高表示更亲近；src-dst 的正负直接编码恶化/改善。 */
	return src_weight - dst_weight;
}

#else /* !CONFIG_NUMA_BALANCING: */
static inline long migrate_degrades_locality(struct task_struct *p,
					     struct lb_env *env)
{
	/* CONFIG_NUMA_BALANCING 关闭时所有迁移在此维度均为中性。 */
	return 0;
}
#endif /* !CONFIG_NUMA_BALANCING */

/*
 * Check whether the task is ineligible on the destination cpu
 *
 * When the PLACE_LAG scheduling feature is enabled and
 * dst_cfs_rq->nr_queued is greater than 1, if the task
 * is ineligible, it will also be ineligible when
 * it is migrated to the destination cpu.
 */
/*
 * task_is_ineligible_on_dst_cpu() - 预测迁移后 p 是否仍无法进入目标 EEVDF eligible 集合。
 * 入参：@p 是候选任务；@dest_cpu 是目标 CPU。出参/返回：PLACE_LAG 开启、目标已有排队实体且
 * p 当前不 eligible 时返回 1，否则 0；无副作用。
 * 注意事项：组调度下使用 p 所属 task_group 的目标 cfs_rq；无锁快照只用于迁移软限制。
 */
static inline int task_is_ineligible_on_dst_cpu(struct task_struct *p, int dest_cpu)
{
	struct cfs_rq *dst_cfs_rq;

#ifdef CONFIG_FAIR_GROUP_SCHED
	dst_cfs_rq = tg_cfs_rq(task_group(p), dest_cpu);
#else
	dst_cfs_rq = &cpu_rq(dest_cpu)->cfs;
#endif
	if (sched_feat(PLACE_LAG) && dst_cfs_rq->nr_queued &&
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	    !entity_eligible(task_cfs_rq(p), &p->se))
		return 1;

	return 0;
}

#ifdef CONFIG_SCHED_CACHE
/*
 * The margin used when comparing LLC utilization with CPU capacity.
 * It determines the LLC load level where active LLC aggregation is
 * done.
 * Derived from fits_capacity().
 *
 * (default: ~50%, tunable via debugfs)
 */
/*
 * fits_llc_capacity() - 判断 LLC 利用率是否低于主动聚合阈值。
 * 入参：@util 与 @max 使用同一容量标度。出参/返回：低于 debugfs 可调百分比返回 true。
 * 注意事项：单硬件线程核心把阈值提高 1.5 倍以容纳更多任务；不睡眠、无副作用。
 */
static bool fits_llc_capacity(unsigned long util, unsigned long max)
{
	u32 aggr_pct = llc_overaggr_pct;

	/*
	 * For single core systems, raise the aggregation
	 * threshold to accommodate more tasks.
	 */
	/* 无 SMT sibling 争用时可更积极聚合，故提高容量阈值。 */
	if (cpu_smt_num_threads == 1)
		aggr_pct = (aggr_pct * 3 / 2);

	return util * 100 < max * aggr_pct;
}

/*
 * The margin used when comparing utilization.
 * is 'util1' noticeably greater than 'util2'
 * Derived from capacity_greater().
 * Bias is in perentage.
 */
/* Allows dst util to be bigger than src util by up to bias percent */
/* 允许 dst 比 src 高出 llc_imb_pct 百分比仍不判为显著更忙，为 LLC 往返迁移提供滞回。 */
#define util_greater(util1, util2) \
	((util1) * 100 > (util2) * (100 + llc_imb_pct))

/*
 * get_llc_stats() - 读取 cpu 所在 LLC 的共享利用率/容量快照。
 * 入参：@cpu 定位共享域；@util/@cap 是非空输出指针。返回：对象存在为 true，否则 false；
 * 成功时写输出。注意事项：调用者处于拓扑保护窗口，READ_ONCE 只保证单字段快照，不睡眠。
 */
static __maybe_unused bool get_llc_stats(int cpu, unsigned long *util,
					 unsigned long *cap)
{
	struct sched_domain_shared *sd_share;

	sd_share = rcu_dereference_all(per_cpu(sd_llc_shared, cpu));
	if (!sd_share)
		return false;

	*util = READ_ONCE(sd_share->util_avg);
	*cap = READ_ONCE(sd_share->capacity);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return true;
}

/*
 * Decision matrix according to the LLC utilization. To
 * decide whether we can do task aggregation across LLC.
 *
 * By default, 50% is the threshold for treating the LLC
 * as busy. The reason for choosing 50% is to avoid saturation
 * of SMT-2, and it is also a safe cutoff for other SMT-n
 * platforms. SMT-1 has higher threshold because it is
 * supposed to accommodate more tasks, see fits_llc_capacity().
 *
 * 20% is the utilization imbalance percentage to decide
 * if the preferred LLC is busier than the non-preferred LLC.
 * 20 is a little higher than the LLC domain's imbalance_pct
 * 17. The hysteresis is used to avoid task bouncing between the
 * preferred LLC and the non-preferred LLC, and it will
 * be turned into tunable debugfs.
 *
 * 1. moving towards the preferred LLC, dst is the preferred
 *    LLC, src is not.
 *
 * src \ dst      30%  40%  50%  60%
 * 30%            Y    Y    Y    N
 * 40%            Y    Y    Y    Y
 * 50%            Y    Y    G    G
 * 60%            Y    Y    G    G
 *
 * 2. moving out of the preferred LLC, src is the preferred
 *    LLC, dst is not:
 *
 * src \ dst      30%  40%  50%  60%
 * 30%            N    N    N    N
 * 40%            N    N    N    N
 * 50%            N    N    G    G
 * 60%            Y    N    G    G
 *
 * src :      src_util
 * dst :      dst_util
 * Y :        Yes, migrate
 * N :        No, do not migrate
 * G :        let the Generic load balance to even the load.
 *
 * The intention is that if both LLCs are quite busy, cache aware
 * load balance should not be performed, and generic load balance
 * should take effect. However, if one is busy and the other is not,
 * the preferred LLC capacity(50%) and imbalance criteria(20%) should
 * be considered to determine whether LLC aggregation should be
 * performed to bias the load towards the preferred LLC.
 */
/*
 * cache-aware LLC 迁移矩阵使用约 50% busy 阈值避免 SMT-2 饱和，SMT-1 阈值更高；另用 20%
 * imbalance（略高于域默认 17%）形成滞回，防止任务在 preferred/non-preferred LLC 间反复弹跳。
 * 向 preferred LLC 迁移时，目标不过载或源足够忙通常允许；迁出 preferred 时更保守。Y 表示执行
 * LLC 偏好迁移，N 表示禁止，G 表示两边都忙等场景交回通用负载均衡。只有一边忙时才综合 50%
 * 容量阈值与 20% 差异决定是否聚合；这些百分比计划转为 debugfs 可调参数。
 */

/* migration decision, 3 states are orthogonal. */
/* 三态互斥：forbid 强制保留偏好，llc 执行偏好迁移，unrestricted 放行通用均衡。 */
enum llc_mig {
	mig_forbid = 0,		/* N: Don't migrate task, respect LLC preference */
	mig_llc,		/* Y: Do LLC preference based migration */
	mig_unrestricted	/* G: Don't restrict generic load balance migration */
};

/*
 * Check if task can be moved from the source LLC to the
 * destination LLC without breaking cache aware preferrence.
 * src_cpu and dst_cpu are arbitrary CPUs within the source
 * and destination LLCs, respectively.
 */
/* 迁移须同时复验源/目标 LLC 负载、任务偏好和通用亲和性约束。 */
static enum llc_mig can_migrate_llc(int src_cpu, int dst_cpu,
				    unsigned long tsk_util,
				    bool to_pref)
{
	/* 先读取两端 LLC 快照并模拟 tsk_util 从源扣除、向目标加入；缺统计时不限制通用均衡。 */
	unsigned long src_util, dst_util, src_cap, dst_cap;

	if (!get_llc_stats(src_cpu, &src_util, &src_cap) ||
	    !get_llc_stats(dst_cpu, &dst_util, &dst_cap))
		return mig_unrestricted;

	src_util = src_util < tsk_util ? 0 : src_util - tsk_util;
	dst_util = dst_util + tsk_util;

	if (!fits_llc_capacity(dst_util, dst_cap) &&
	    !fits_llc_capacity(src_util, src_cap))
		return mig_unrestricted;

	/* 两端都过 busy 阈值时交给 generic；否则按迁入/迁出 preferred 的非对称矩阵裁决。 */
	if (to_pref) {
		/*
		 * Don't migrate if we will get preferred LLC too
		 * heavily loaded and if the dest is much busier
		 * than the src, in which case migration will
		 * increase the imbalance too much.
		 */
		if (!fits_llc_capacity(dst_util, dst_cap) &&
		    util_greater(dst_util, src_util))
			return mig_forbid;
	} else {
		/*
		 * Don't migrate if we will leave preferred LLC
		 * too idle, or if this migration leads to the
		 * non-preferred LLC falls within sysctl_aggr_imb percent
		 * of preferred LLC, leading to migration again
		 * back to preferred LLC.
		 */
/* 迁移须同时复验源/目标 LLC 负载、任务偏好和通用亲和性约束。 */
		if (fits_llc_capacity(src_util, src_cap) ||
		    !util_greater(src_util, dst_util))
			return mig_forbid;
	}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return mig_llc;
}

/*
 * Check if task p can migrate from source LLC to
 * destination LLC in terms of cache aware load balance.
 */
static enum llc_mig can_migrate_llc_task(int src_cpu, int dst_cpu,
					 struct task_struct *p)
{
	/* mm->sc_stat.cpu 标记地址空间 preferred LLC 锚点；内核线程无 mm 时不施加缓存偏好。 */
	struct mm_struct *mm;
	bool to_pref;
	int cpu;

	mm = p->mm;
	if (!mm)
		return mig_unrestricted;

	cpu = READ_ONCE(mm->sc_stat.cpu);
	if (cpu < 0 || cpus_share_cache(src_cpu, dst_cpu))
		return mig_unrestricted;

	/* skip cache aware load balance for too many threads */
	/* 线程过多或目标 LLC 容量已超阈值时禁用该 mm 的偏好，避免错误聚合。 */
	if (invalid_llc_nr(mm, p, dst_cpu) ||
	    exceed_llc_capacity(mm, dst_cpu)) {
		if (READ_ONCE(mm->sc_stat.cpu) != -1)
			WRITE_ONCE(mm->sc_stat.cpu, -1);
		return mig_unrestricted;
	}

	if (cpus_share_cache(dst_cpu, cpu))
		to_pref = true;
	else if (cpus_share_cache(src_cpu, cpu))
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		to_pref = false;
	else
		return mig_unrestricted;

	return can_migrate_llc(src_cpu, dst_cpu,
			       task_util(p), to_pref);
}

/*
 * Check if active load balance breaks LLC locality in
 * terms of cache aware load balance. The load level and
 * imbalance do not warrant breaking LLC preference per
 * the can_migrate_llc() policy. Here, the benefit of
 * LLC locality outweighs the power efficiency gained from
 * migrating the only runnable task away.
 */
/* 迁移须同时复验源/目标 LLC 负载、任务偏好和通用亲和性约束。 */
static inline bool
alb_break_llc(struct lb_env *env)
{
	if (!sched_cache_enabled())
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return false;

	if (cpus_share_cache(env->src_cpu, env->dst_cpu))
		return false;
	/*
	 * All tasks prefer to stay on their current CPU.
	 * Do not pull a task from its preferred CPU if:
	 * 1. It is the only task running and does not exceed
	 *    imbalance allowance; OR
	 * 2. Migrating it away from its preferred LLC would violate
	 *    the cache-aware scheduling policy.
	 */
/* 迁移须同时复验源/目标 LLC 负载、任务偏好和通用亲和性约束。 */
	if (env->src_rq->nr_pref_llc_running &&
	    env->src_rq->nr_pref_llc_running == env->src_rq->cfs.h_nr_runnable) {
		unsigned long util = 0;
		struct task_struct *cur;

		if (env->src_rq->nr_running <= 1)
			return true;

		cur = rcu_dereference_all(env->src_rq->curr);
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (cur && cur->sched_class == &fair_sched_class)
			util = task_util(cur);

		if (can_migrate_llc(env->src_cpu, env->dst_cpu,
				    util, false) == mig_forbid)
			return true;
	}

	return false;
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
}

/*
 * Check if migrating task p from env->src_cpu to
 * env->dst_cpu breaks LLC localiy.
 */
static bool migrate_degrades_llc(struct task_struct *p, struct lb_env *env)
{
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (!sched_cache_enabled())
		return false;

	if (task_has_sched_core(p))
		return false;
	/*
	 * Skip over tasks that would degrade LLC locality;
	 * only when nr_balanced_failed is sufficiently high do we
	 * ignore this constraint.
	 *
	 * Threshold of cache_nice_tries is set to 1 higher
	 * than nr_balance_failed to avoid excessive task
	 * migration at the same time.
	 */
/* 迁移须同时复验源/目标 LLC 负载、任务偏好和通用亲和性约束。 */
	if (env->sd->nr_balance_failed >= env->sd->cache_nice_tries + 1)
		return false;

	/*
	 * We know the env->src_cpu has some tasks prefer to
	 * run on env->dst_cpu, skip the tasks do not prefer
	 * env->dst_cpu, and find the one that prefers.
	 */
/* 迁移须同时复验源/目标 LLC 负载、任务偏好和通用亲和性约束。 */
	if (env->migration_type == migrate_llc_task &&
	    READ_ONCE(p->preferred_llc) != llc_id(env->dst_cpu))
		return true;

	if (can_migrate_llc_task(env->src_cpu,
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
				 env->dst_cpu, p) != mig_forbid)
		return false;

	return true;
}

#else
static inline bool get_llc_stats(int cpu, unsigned long *util,
				 unsigned long *cap)
{
	return false;
}

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
static inline bool
alb_break_llc(struct lb_env *env)
{
	return false;
}

static inline bool
migrate_degrades_llc(struct task_struct *p, struct lb_env *env)
{
	/* 无 LLC 感知配置时不存在局部性降级，统一返回 false。 */
	return false;
}
#endif
/*
 * can_migrate_task - may task p from runqueue rq be migrated to this_cpu?
 */
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
static
/* 锁住源 rq 后检查亲和性、running、热度、throttle 与 dst capacity，失败记录原因。 */
/*
 * can_migrate_task() - 在源 rq 锁下验证 p 是否可迁往 env->dst_cpu。
 * 入参：@p 是源队列候选；@env 是本轮均衡环境。返回 1 允许 detach，0 拒绝；拒绝时可能更新
 * pinned/break 统计和重定向 CPU，允许热任务时标记 forced。注意事项：不睡眠，必须持 src rq 锁；
 * 依次执行 delayed/throttle/eligibility、亲和性、running、NUMA/LLC、cache-hot 与失败重试策略。
 */
int can_migrate_task(struct task_struct *p, struct lb_env *env)
{
	long degrades, hot;

	lockdep_assert_rq_held(env->src_rq);
	if (p->sched_task_hot)
		p->sched_task_hot = 0;

	/*
	 * We do not migrate tasks that are:
	 * 1) delayed dequeued unless we migrate load, or
	 * 2) target cfs_rq is in throttled hierarchy, or
	 * 3) cannot be migrated to this CPU due to cpus_ptr, or
	 * 4) running (obviously), or
	 * 5) are cache-hot on their current CPU, or
	 * 6) are blocked on mutexes (if SCHED_PROXY_EXEC is enabled)
	 */
	/* delayed 非 load 单位、目标 throttle、per-CPU kthread、proxy blocked 等均不可迁移。 */
	if ((p->se.sched_delayed) && (env->migration_type != migrate_load))
		return 0;

	if (lb_throttled_hierarchy(p, env->dst_cpu))
		return 0;

	/*
	 * We want to prioritize the migration of eligible tasks.
	 * For ineligible tasks we soft-limit them and only allow
	 * them to migrate when nr_balance_failed is non-zero to
	 * avoid load-balancing trying very hard to balance the load.
	 */
	/* 首次尝试软拒绝迁后仍 ineligible 的任务；已有失败时放宽，避免均衡器无限执着。 */
	if (!env->sd->nr_balance_failed &&
	    task_is_ineligible_on_dst_cpu(p, env->dst_cpu))
		return 0;

	/* Disregard percpu kthreads; they are where they need to be. */
/* per-CPU 内核线程已在其固定 CPU 上，均衡器应跳过。 */
	if (kthread_is_per_cpu(p))
		return 0;

	if (task_is_blocked(p))
		return 0;

	if (!cpumask_test_cpu(env->dst_cpu, p->cpus_ptr)) {
		int cpu;

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		schedstat_inc(p->stats.nr_failed_migrations_affine);

		env->flags |= LBF_SOME_PINNED;

		/*
		 * Remember if this task can be migrated to any other CPU in
		 * our sched_group. We may want to revisit it if we couldn't
		 * meet load balance goals by pulling other tasks on src_cpu.
		 *
		 * Avoid computing new_dst_cpu
		 * - for NEWLY_IDLE
		 * - if we have already computed one in current iteration
		 * - if it's an active balance
		 */
		/*
		 * 亲和性不含当前 dst 时记录 SOME_PINNED；普通非 newidle/active 场景可在目标组中寻找另一个
		 * p 允许的 new_dst_cpu，供外层重试，且从 cpus 集排除原 dst 防止原地重选。
		 */
		if (env->idle == CPU_NEWLY_IDLE ||
		    env->flags & (LBF_DST_PINNED | LBF_ACTIVE_LB))
			return 0;

		/* Prevent to re-select dst_cpu via env's CPUs: */
/* 从候选 mask 排除已固定目标，防止再次选中 dst_cpu。 */
		cpu = cpumask_first_and_and(env->dst_grpmask, env->cpus, p->cpus_ptr);

		if (cpu < nr_cpu_ids) {
			env->flags |= LBF_DST_PINNED;
			env->new_dst_cpu = cpu;
		}

		return 0;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	/* Record that we found at least one task that could run on dst_cpu */
	/* 至少有一个亲和性允许的任务，清除“全部 pinned”假设。 */
	env->flags &= ~LBF_ALL_PINNED;

	if (task_on_cpu(env->src_rq, p) ||
	    task_current_donor(env->src_rq, p)) {
		schedstat_inc(p->stats.nr_failed_migrations_running);
		return 0;
	}

	/*
	 * Aggressive migration if:
	 * 1) active balance
	 * 2) destination numa is preferred
	 * 3) task is cache cold, or
	 * 4) too many balance attempts have failed.
	 */
	/* active balance 直接允许；否则结合 NUMA/LLC 局部性与 cache hot，失败次数高时逐步放宽。 */
	if (env->flags & LBF_ACTIVE_LB)
		return 1;

	degrades = migrate_degrades_locality(p, env);
	if (!degrades) {
		/*
		 * If the NUMA locality is not broken,
		 * further check if migration would hurt
		 * LLC locality.
		 */
		/* NUMA 中性时再检查 LLC，避免两个局部性层同时重复惩罚。 */
		if (migrate_degrades_llc(p, env)) {
			/*
			 * If regular load balancing fails to pull a task
			 * due to LLC locality, this is expected behavior
			 * and we set LBF_LLC_PINNED so we don't increase
			 * nr_balance_failed unecessarily.
			 */
			/* 普通均衡因预期 LLC 偏好拒绝时标 LLC_PINNED，不把它误计为异常失败。 */
			if (env->migration_type != migrate_llc_task)
				env->flags |= LBF_LLC_PINNED;

			return 0;
		}

		hot = task_hot(p, env);
	} else {
		hot = degrades > 0;
	}

	if (!hot || env->sd->nr_balance_failed > env->sd->cache_nice_tries) {
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (hot)
			p->sched_task_hot = 1;
		return 1;
	}

	schedstat_inc(p->stats.nr_failed_migrations_hot);
	return 0;
}

/*
 * detach_task() -- detach the task for the migration specified in env
 */
/*
 * detach_task() - 从锁定源 rq 摘除 p 并把 task_cpu 切到 env->dst_cpu。
 * 入参：@p 是已通过 can_migrate_task 的任务；@env 提供源/目标。返回：无；任务从源队列失活，
 * CPU 归属更新但尚未附着目标 rq。注意事项：必须持 src rq 锁、不睡眠；此处形成迁移中间态，
 * 调用者必须随后 attach，且当前/donor 任务绝不能进入本函数。
 */
static void detach_task(struct task_struct *p, struct lb_env *env)
{
	lockdep_assert_rq_held(env->src_rq);

	if (p->sched_task_hot) {
		p->sched_task_hot = 0;
		schedstat_inc(env->sd->lb_hot_gained[env->idle]);
		schedstat_inc(p->stats.nr_forced_migrations);
	}

	WARN_ON(task_current(env->src_rq, p));
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	WARN_ON(task_current_donor(env->src_rq, p));

	/* 源时钟已由批量路径更新，NOCLOCK 摘除后 set_task_cpu 建立目标归属，ownership 交给迁移列表。 */
	deactivate_task(env->src_rq, p, DEQUEUE_NOCLOCK);
	set_task_cpu(p, env->dst_cpu);
}

/*
 * detach_one_task() -- tries to dequeue exactly one task from env->src_rq, as
 * part of active balancing operations within "domain".
 *
 * Returns a task if successful and NULL otherwise.
 */
/*
 * detach_one_task() - 主动均衡时从源 cfs_tasks 逆序找到并摘除恰好一个可迁任务。
 * 入参：@env 为持锁源环境。返回：成功返回迁移中任务借用指针，失败 NULL；成功者尚未 attach。
 * 注意事项：必须持 src rq 锁，不睡眠；统计在此更新，因为与 detach_tasks() 是仅有两个 gain 点。
 */
static struct task_struct *detach_one_task(struct lb_env *env)
{
	struct task_struct *p;

	lockdep_assert_rq_held(env->src_rq);

	list_for_each_entry_reverse(p,
			&env->src_rq->cfs_tasks, se.group_node) {
		if (!can_migrate_task(p, env))
			continue;

		detach_task(p, env);

		/*
		 * Right now, this is only the second place where
		 * lb_gained[env->idle] is updated (other is detach_tasks)
		 * so we can safely collect stats here rather than
		 * inside detach_tasks().
		 */
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		schedstat_inc(env->sd->lb_gained[env->idle]);
		return p;
	}
	return NULL;
}

/*
 * detach_tasks() -- tries to detach up to imbalance load/util/tasks from
 * busiest_rq, as part of a balancing operation within domain "sd".
 *
 * Returns number of detached tasks if successful and 0 otherwise.
 */
/* 源 rq 锁下按 imbalance 批量摘任务；每项经 can_migrate_task() 重新验证。 */
/*
 * detach_tasks() - 按 migration_type 从最忙 rq 摘取不超过 imbalance 的任务批次。
 * 入参：@env 是持有 src_rq 锁的输入输出环境。返回摘除数；成功任务进入 env->tasks，task_cpu 已
 * 指向 dst 但尚未入队。注意事项：不睡眠；扫描和迁移量均有上限，调用者必须随后 attach_tasks()
 * 闭合迁移中 ownership。
 */
static int detach_tasks(struct lb_env *env)
{
	struct list_head *tasks = &env->src_rq->cfs_tasks;
	unsigned long util, load;
	struct task_struct *p;
	int detached = 0;

	lockdep_assert_rq_held(env->src_rq);

	/*
	 * Source run queue has been emptied by another CPU, clear
	 * LBF_ALL_PINNED flag as we will not test any task.
	 */
	/* 源已被并发抽到至多一个任务时未测试亲和性，不能保留 ALL_PINNED 结论。 */
	if (env->src_rq->nr_running <= 1) {
		env->flags &= ~LBF_ALL_PINNED;
		return 0;
	}

	if (env->imbalance <= 0)
		return 0;

	/* 逆序轮转任务链；拒绝项移到另一端，避免本轮反复检查。 */
	while (!list_empty(tasks)) {
		/*
		 * We don't want to steal all, otherwise we may be treated likewise,
		 * which could at worst lead to a livelock crash.
		 */
		/* 目标 idle 时至少给源留一个任务，避免两端互相抽空形成活锁。 */
		if (env->idle && env->src_rq->nr_running <= 1)
			break;

		env->loop++;
		/* We've more or less seen every task there is, call it quits */
		/* loop_max 保证最多近似检查一遍候选集。 */
		if (env->loop > env->loop_max)
			break;

		/* take a breather every nr_migrate tasks */
		/* 分段长临界区，置 NEED_BREAK 让外层释放源 rq 锁后再继续。 */
		if (env->loop > env->loop_break) {
			env->loop_break += SCHED_NR_MIGRATE_BREAK;
			env->flags |= LBF_NEED_BREAK;
			break;
		}

		p = list_last_entry(tasks, struct task_struct, se.group_node);

		if (!can_migrate_task(p, env))
			goto next;

		switch (env->migration_type) {
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		case migrate_load:
			/* 按层级 load 权重搬运；task_h_load 为 0 时强制 1，保证 imbalance 单调下降。 */
			/*
			 * Depending of the number of CPUs and tasks and the
			 * cgroup hierarchy, task_h_load() can return a null
			 * value. Make sure that env->imbalance decreases
			 * otherwise detach_tasks() will stop only after
			 * detaching up to loop_max tasks.
			 */
			load = max_t(unsigned long, task_h_load(p), 1);

			if (sched_feat(LB_MIN) &&
			    load < 16 && !env->sd->nr_balance_failed)
				goto next;

			/*
			 * Make sure that we don't migrate too much load.
			 * Nevertheless, let relax the constraint if
			 * scheduler fails to find a good waiting task to
			 * migrate.
			 */
			/* 初期禁止过量搬运；连续失败会通过 shr_bound 逐步放宽粒度约束。 */
			if (shr_bound(load, env->sd->nr_balance_failed) > env->imbalance)
				goto next;

			env->imbalance -= load;
			break;

		case migrate_util:
			/* 按 util_est 容量需求扣减，使用相同的失败放宽策略。 */
			util = task_util_est(p);

			if (shr_bound(util, env->sd->nr_balance_failed) > env->imbalance)
				goto next;

			env->imbalance -= util;
			break;

		case migrate_task:
			/* 按任务个数扣减。 */
			env->imbalance--;
			break;

		case migrate_misfit:
			/* This is not a misfit task */
			/* 只摘源 CPU 容量不适配的任务，命中一个即完成本轮目标。 */
			if (task_fits_cpu(p, env->src_cpu))
				goto next;

			env->imbalance = 0;
			break;

		case migrate_llc_task:
			/* 按单个 preferred-LLC 任务计数，资格已由 can_migrate_task 过滤。 */
			env->imbalance--;
			break;
		}

		/* detach 后 ownership 转给 env 临时链，等待目标 rq attach。 */
		detach_task(p, env);
		list_add(&p->se.group_node, &env->tasks);

		detached++;

#ifdef CONFIG_PREEMPTION
		/*
		 * NEWIDLE balancing is a source of latency, so preemptible
		 * kernels will stop after the first task is detached to minimize
		 * the critical section.
		 */
		/* 可抢占内核 NEWIDLE 只搬一个，限制唤醒延迟。 */
		if (env->idle == CPU_NEWLY_IDLE)
			break;
#endif

		/*
		 * We only want to steal up to the prescribed amount of
		 * load/util/tasks.
		 */
		/* 达到规定迁移量立即结束，避免越过计算出的平衡点。 */
		if (env->imbalance <= 0)
			break;

		continue;
next:
		if (p->sched_task_hot)
			schedstat_inc(p->stats.nr_failed_migrations_hot);

		list_move(&p->se.group_node, tasks);
	}

	/*
	 * Right now, this is one of only two places we collect this stat
	 * so we can safely collect detach_one_task() stats here rather
	 * than inside detach_one_task().
	 */
	schedstat_add(env->sd->lb_gained[env->idle], detached);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return detached;
}

/*
 * attach_tasks() -- attaches all tasks detached by detach_tasks() to their
 * new rq.
 */
/* 目标 rq 锁下逐项 set_task_cpu+activate，完成后清空临时迁移链。 */
/*
 * attach_tasks() - 把 env->tasks 的迁移中任务批量附着到目标 rq。
 * 入参：@env 提供 dst_rq 和临时任务链。返回：无；成功后链为空、任务重新 active。
 * 注意事项：自行锁目标 rq 并更新时间，不睡眠；与 detach_tasks() 构成必须闭合的迁移事务。
 */
static void attach_tasks(struct lb_env *env)
{
	struct list_head *tasks = &env->tasks;
	struct task_struct *p;
	struct rq_flags rf;

	/* 一次锁住并更新目标时钟后批量 attach，减少每任务锁开销。 */
	rq_lock(env->dst_rq, &rf);
	update_rq_clock(env->dst_rq);

	/* list_del_init 先解除临时 ownership，再由 attach_task 发布到目标 rq。 */
	while (!list_empty(tasks)) {
		p = list_first_entry(tasks, struct task_struct, se.group_node);
		list_del_init(&p->se.group_node);

		attach_task(env->dst_rq, p);
	}

	rq_unlock(env->dst_rq, &rf);
}

#ifdef CONFIG_NO_HZ_COMMON
/* cfs_rq_has_blocked_load() 读取借用 cfs_rq 的 PELT load/util；任一非零表示仍需周期衰减。 */
static inline bool cfs_rq_has_blocked_load(struct cfs_rq *cfs_rq)
{
	if (cfs_rq->avg.load_avg)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return true;

	if (cfs_rq->avg.util_avg)
		return true;

	return false;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}

/* others_have_blocked() 汇总 RT/DL、硬件压力和 IRQ 的残留信号；非零意味着 nohz 仍不能停止更新。 */
static inline bool others_have_blocked(struct rq *rq)
{
	if (cpu_util_rt(rq))
		return true;

	if (cpu_util_dl(rq))
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
		return true;

	if (hw_load_avg(rq))
		return true;

	if (cpu_util_irq(rq))
		return true;

	return false;
}

/* update_blocked_load_tick() 以 WRITE_ONCE 发布本 rq 最近衰减 jiffies，供无锁 nohz 协调读取。 */
static inline void update_blocked_load_tick(struct rq *rq)
{
	WRITE_ONCE(rq->last_blocked_load_update_tick, jiffies);
}

/* 仅在确认所有信号归零时清 has_blocked_load；true 保留既有提示，避免与并发置位竞争丢更新。 */
static inline void update_has_blocked_load_status(struct rq *rq, bool has_blocked_load)
{
	if (!has_blocked_load)
		rq->has_blocked_load = 0;
}
#else /* !CONFIG_NO_HZ_COMMON: */
/* 无 NO_HZ_COMMON 时 blocked-load 协调不存在，四个同签名 stub 返回中性值且无副作用。 */
static inline bool cfs_rq_has_blocked_load(struct cfs_rq *cfs_rq) { return false; }
static inline bool others_have_blocked(struct rq *rq) { return false; }
static inline void update_blocked_load_tick(struct rq *rq) {}
static inline void update_has_blocked_load_status(struct rq *rq, bool has_blocked_load) {}
#endif /* !CONFIG_NO_HZ_COMMON */

/*
 * __update_blocked_others() - 先衰减 RT/DL/IRQ 等非 CFS 信号并更新完成标志。
 * 入参：@rq 为锁定队列；@done 是输入输出布尔槽。返回是否有信号变化；若仍残留则写 false。
 * 注意事项：不睡眠；必须早于 CFS，因为后者可能触发 cpufreq_update_util() 并需看到最新其他类。
 */
static bool __update_blocked_others(struct rq *rq, bool *done)
{
	bool updated;

	/*
	 * update_load_avg() can call cpufreq_update_util(). Make sure that RT,
	 * DL and IRQ signals have been updated before updating CFS.
	 */
	/* update_load_avg(CFS) 可通知 cpufreq，故先让 RT/DL/IRQ 到同一时刻，避免频率看到混合窗口。 */
	updated = update_other_load_avgs(rq);

	if (others_have_blocked(rq))
		*done = false;

	return updated;
}

#ifdef CONFIG_FAIR_GROUP_SCHED

/*
 * __update_blocked_fair() - 自底向上衰减 rq 上所有 task-group cfs_rq 并向父层传播。
 * 入参：@rq 为锁定队列；@done 输入输出完成标志。返回根 cfs_rq 是否发生衰减。
 * 注意事项：安全迭代允许删除 fully-decayed 叶队列；不睡眠，保持父子 PELT 顺序。
 */
static bool __update_blocked_fair(struct rq *rq, bool *done)
{
	struct cfs_rq *cfs_rq, *pos;
	bool decayed = false;

	/*
	 * Iterates the task_group tree in a bottom up fashion, see
	 * list_add_leaf_cfs_rq() for details.
	 */
	/* 叶到根顺序确保子组变化先聚合到父实体。 */
	for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos) {
		struct sched_entity *se;

		if (update_cfs_rq_load_avg(cfs_rq_clock_pelt(cfs_rq), cfs_rq)) {
			update_tg_load_avg(cfs_rq);

			if (cfs_rq->nr_queued == 0)
				update_idle_cfs_rq_clock_pelt(cfs_rq);

			if (cfs_rq == &rq->cfs)
				decayed = true;
		}

		/* Propagate pending load changes to the parent, if any: */
		/* 子 cfs_rq 对应 group entity 时，把待传播变化提交到父队列。 */
		se = cfs_rq_se(cfs_rq);
		if (se && !skip_blocked_update(se))
			update_load_avg(cfs_rq_of(se), se, UPDATE_TG);

		/*
		 * There can be a lot of idle CPU cgroups.  Don't let fully
		 * decayed cfs_rqs linger on the list.
		 */
		/* 完全衰减的空闲 cgroup 从叶链摘除，避免 nohz 每轮扫描大量零对象。 */
		if (cfs_rq_is_decayed(cfs_rq))
			list_del_leaf_cfs_rq(cfs_rq);

		/* Don't need periodic decay once load/util_avg are null */
		if (cfs_rq_has_blocked_load(cfs_rq))
			*done = false;
	}

	return decayed;
}

/*
 * Compute the hierarchical load factor for cfs_rq and all its ascendants.
 * This needs to be done in a top-down fashion because the load of a child
 * group is a fraction of its parents load.
 */
/*
 * update_cfs_rq_h_load() - 自顶向下计算 cfs_rq 及祖先约束后的层级负载份额。
 * 入参：@cfs_rq 是目标组队列借用指针。返回：无；更新路径各层 h_load 与本 jiffy 时间戳。
 * 注意事项：同一 jiffy 命中缓存即返回；h_load_next 临时串起向下路径，READ/WRITE_ONCE 防止编译器
 * 破坏无锁可见性，不提供事务快照；调用者处于调度统计安全上下文且不睡眠。
 */
static void update_cfs_rq_h_load(struct cfs_rq *cfs_rq)
{
	struct sched_entity *se = cfs_rq_se(cfs_rq);
	unsigned long now = jiffies;
	unsigned long load;

	if (cfs_rq->last_h_load_update == now)
		return;

	/* 阶段 1：从目标向上记录反向路径，遇到本 jiffy 已算祖先或根停止。 */
	WRITE_ONCE(cfs_rq->h_load_next, NULL);
	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		WRITE_ONCE(cfs_rq->h_load_next, se);
		if (cfs_rq->last_h_load_update == now)
			break;
	}

	/* 到根时以根 load_avg 建立自顶向下传播的基值。 */
	if (!se) {
		cfs_rq->h_load = cfs_rq_load_avg(cfs_rq);
		cfs_rq->last_h_load_update = now;
	}

	/* 阶段 2：沿记录路径向下按子实体 load/父 rq load 比例分摊层级负载。分母 +1 防零除。 */
	while ((se = READ_ONCE(cfs_rq->h_load_next)) != NULL) {
		load = cfs_rq->h_load;
		load = div64_ul(load * se->avg.load_avg,
			cfs_rq_load_avg(cfs_rq) + 1);
		cfs_rq = group_cfs_rq(se);
		cfs_rq->h_load = load;
		cfs_rq->last_h_load_update = now;
	}
}

/*
 * task_h_load() - 将 task 自身 load_avg 乘以所属 cgroup 的层级份额。
 * 入参：@p 为借用任务。返回 task 在跨 CPU 均衡中的层级 load 权重；无 ownership 变化。
 * 注意事项：会按需刷新祖先 h_load，不睡眠；分母 +1 处理空队列并提供保守饱和。
 */
static unsigned long task_h_load(struct task_struct *p)
{
	struct cfs_rq *cfs_rq = task_cfs_rq(p);

	update_cfs_rq_h_load(cfs_rq);
	return div64_ul(p->se.avg.load_avg * cfs_rq->h_load,
			cfs_rq_load_avg(cfs_rq) + 1);
}
#else /* !CONFIG_FAIR_GROUP_SCHED: */
/* 无组调度时只衰减根 cfs_rq；done=false 表示仍需后续周期更新。 */
static bool __update_blocked_fair(struct rq *rq, bool *done)
{
	struct cfs_rq *cfs_rq = &rq->cfs;
	bool decayed;

	decayed = update_cfs_rq_load_avg(cfs_rq_clock_pelt(cfs_rq), cfs_rq);
	if (cfs_rq_has_blocked_load(cfs_rq))
		*done = false;

	return decayed;
}

/* 无组层级时 task 的均衡权重就是实体自身 load_avg。 */
static unsigned long task_h_load(struct task_struct *p)
{
	return p->se.avg.load_avg;
}
#endif /* !CONFIG_FAIR_GROUP_SCHED */

/*
 * __sched_balance_update_blocked_averages() - 在锁定 rq 上统一推进所有 blocked PELT 信号。
 * 入参：@rq 为锁定队列。返回：无；更新 nohz 时间/提示，任一信号衰减时通知 cpufreq。
 * 注意事项：other 必须早于 fair；不睡眠，调用者负责 rq clock 已更新。
 */
static void __sched_balance_update_blocked_averages(struct rq *rq)
{
	bool decayed = false, done = true;

	update_blocked_load_tick(rq);

	decayed |= __update_blocked_others(rq, &done);
	decayed |= __update_blocked_fair(rq, &done);

	update_has_blocked_load_status(rq, !done);
	if (decayed)
		cpufreq_update_util(rq, 0);
}

/*
 * sched_balance_update_blocked_averages() - 为指定 CPU 加 irqsave rq 锁后执行 blocked 衰减。
 * 入参：@cpu 为有效 CPU。返回：无；scope guard 离开函数自动解锁恢复中断。
 * 注意事项：不睡眠；先 update_rq_clock 再调用锁内核心，保证统一时间基准。
 */
static void sched_balance_update_blocked_averages(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	guard(rq_lock_irqsave)(rq);
	update_rq_clock(rq);
	__sched_balance_update_blocked_averages(rq);
}

/********** Helpers for sched_balance_find_src_group ************************/
/* 以下是源组选择的统计与候选比较辅助函数。 */

/*
 * sg_lb_stats - stats of a sched_group required for load-balancing:
 */
/* 单 sched_group 的瞬时负载、容量、idle 和 misfit 汇总。 */
/*
 * sg_lb_stats 由 update_sg_lb_stats() 每轮重建：avg_load 是 group_load/group_capacity；group_load、
 * group_util、group_runnable 分别为层级权重、PELT util、runnable 总量；capacity 是聚合可用容量。
 * sum_nr_running 含全部类，sum_h_nr_running 为层级 CFS 数，idle_cpus 与 group_weight 是空闲/CPU 数。
 * group_type 是最终拉取优先级；asym/smt/llc 三字段标记专项迁移；misfit_load 保存最大不适配任务，
 * overutilized 表示至少一 CPU 过载。NUMA 字段区分 NUMA/首选节点任务数，SCHED_CACHE 字段计数偏好
 * 目标 LLC 的任务。对象是调用栈快照，无独立锁/生命周期。
 */
struct sg_lb_stats {
	unsigned long avg_load;			/* Avg load            over the CPUs of the group */
	unsigned long group_load;		/* Total load          over the CPUs of the group */
	unsigned long group_capacity;		/* Capacity            over the CPUs of the group */
/* 该字段汇总组内全部 CPU 容量。 */
	unsigned long group_util;		/* Total utilization   over the CPUs of the group */
/* 该字段汇总组内全部 CPU 利用率。 */
	unsigned long group_runnable;		/* Total runnable time over the CPUs of the group */
	unsigned int sum_nr_running;		/* Nr of all tasks running in the group */
	unsigned int sum_h_nr_running;		/* Nr of CFS tasks running in the group */
	unsigned int idle_cpus;                 /* Nr of idle CPUs         in the group */
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	unsigned int group_weight;
	enum group_type group_type;
	unsigned int group_asym_packing;	/* Tasks should be moved to preferred CPU */
	unsigned int group_smt_balance;		/* Task on busy SMT be moved */
	unsigned int group_llc_balance;		/* Tasks should be moved to preferred LLC */
/* 该标志表示任务应迁往其偏好 LLC。 */
	unsigned long group_misfit_task_load;	/* A CPU has a task too big for its capacity */
	unsigned int group_overutilized;	/* At least one CPU is overutilized in the group */
#ifdef CONFIG_NUMA_BALANCING
	unsigned int nr_numa_running;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	unsigned int nr_preferred_running;
#endif
#ifdef CONFIG_SCHED_CACHE
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	unsigned int nr_pref_dst_llc;
#endif
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
};

/*
 * sd_lb_stats - stats of a sched_domain required for load-balancing:
 */
/* domain 级本地组与 busiest 组比较结果，供 imbalance 类型和迁移量决策。 */
/*
 * sd_lb_stats 汇总一个 sched_domain：busiest/local 指向拓扑组借用对象；total_load/capacity 与 avg_load
 * 描述全域，prefer_sibling 决定先填充低层 sibling；busiest_stat/local_stat 是两个组快照。由一次
 * update_sd_lb_stats() 创建和消费，不跨均衡轮次发布。
 */
struct sd_lb_stats {
	struct sched_group *busiest;		/* Busiest group in this sd */
	struct sched_group *local;		/* Local group in this sd */
	unsigned long total_load;		/* Total load of all groups in sd */
/* 该字段汇总调度域所有组的负载。 */
	unsigned long total_capacity;		/* Total capacity of all groups in sd */
/* 该字段汇总调度域所有组的容量。 */
	unsigned long avg_load;			/* Average load across all groups in sd */
/* 该字段保存调度域按容量归一化的平均负载。 */
	unsigned int prefer_sibling;		/* Tasks should go to sibling first */
/* 该标志要求优先填充本地兄弟组。 */

	struct sg_lb_stats busiest_stat;	/* Statistics of the busiest group */
/* 该字段保存当前最忙候选组的统计快照。 */
	struct sg_lb_stats local_stat;		/* Statistics of the local group */
};

/*
 * init_sd_lb_stats() - 以最小写入初始化域统计哨兵。
 * 入参：@sds 为调用者拥有的输出对象。返回：无；清核心累计值并把 busiest 初值设为最低 group_type、
 * 最大 idle_cpus。注意事项：local_stat 随后会被完整赋值，故不重复清零；不睡眠。
 */
static inline void init_sd_lb_stats(struct sd_lb_stats *sds)
{
	/*
	 * Skimp on the clearing to avoid duplicate work. We can avoid clearing
	 * local_stat because update_sg_lb_stats() does a full clear/assignment.
	 * We must however set busiest_stat::group_type and
	 * busiest_stat::idle_cpus to the worst busiest group because
	 * update_sd_pick_busiest() reads these before assignment.
	 */
	/* busiest 比较在首次赋值前会读类型/idle 数，必须给“最差候选”哨兵；local_stat 将被覆盖。 */
	*sds = (struct sd_lb_stats){
		.busiest = NULL,
		.local = NULL,
		.total_load = 0UL,
		.total_capacity = 0UL,
		.busiest_stat = {
			.idle_cpus = UINT_MAX,
			.group_type = group_has_spare,
		},
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	};
}

/*
 * scale_rt_capacity() - 从 CPU 实际容量扣除 RT/DL 与 IRQ 压力，得到 CFS 可用容量。
 * 入参：@cpu 有效 CPU。返回至少 1 的近似空闲容量标度；无副作用。
 * 注意事项：无锁快照、不睡眠；饱和压力返回 1 而非 0，避免后续除零。
 */
static unsigned long scale_rt_capacity(int cpu)
{
	unsigned long max = get_actual_cpu_capacity(cpu);
	struct rq *rq = cpu_rq(cpu);
	unsigned long used, free;
	unsigned long irq;

	irq = cpu_util_irq(rq);

	if (unlikely(irq >= max))
		return 1;

	/*
	 * avg_rt.util_avg and avg_dl.util_avg track binary signals
	 * (running and not running) with weights 0 and 1024 respectively.
	 */
	/* RT/DL avg 是 0/1024 二值运行信号的 PELT，窗口同步后可直接相加扣除。 */
	used = cpu_util_rt(rq);
	used += cpu_util_dl(rq);

	if (unlikely(used >= max))
		return 1;

	free = max - used;

	return scale_irq_capacity(free, irq, max);
}

/*
 * update_cpu_capacity() - 更新叶调度组单 CPU 的 CFS 可用容量。
 * 入参：@sd 是该 CPU 叶域；@cpu 是目标 CPU。返回：无；发布 rq capacity、tracepoint 与组 min/max。
 * 注意事项：由均衡容量更新路径串行调用，不睡眠；容量强制至少 1。
 */
static void update_cpu_capacity(struct sched_domain *sd, int cpu)
{
	unsigned long capacity = scale_rt_capacity(cpu);
	struct sched_group *sdg = sd->groups;

	if (!capacity)
		capacity = 1;

	cpu_rq(cpu)->cpu_capacity = capacity;
	trace_sched_cpu_capacity_tp(cpu_rq(cpu));

	sdg->sgc->capacity = capacity;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	sdg->sgc->min_capacity = capacity;
	sdg->sgc->max_capacity = capacity;
}

/*
 * update_group_capacity() - 自底向上汇总本地组总/最小/最大 CFS 容量并安排下次更新。
 * 入参：@sd 为当前域；@cpu 为本地 CPU。返回：无；写 sgc 容量与 next_update。
 * 注意事项：叶域直接更新 CPU；NUMA 域逐 CPU 求和，其他域复用 child group 聚合值；不睡眠。
 */
void update_group_capacity(struct sched_domain *sd, int cpu)
{
	struct sched_domain *child = sd->child;
	struct sched_group *group, *sdg = sd->groups;
	unsigned long capacity, min_capacity, max_capacity;
	unsigned long interval;

	/* 周期限制在 1 jiffy 与全局最大均衡间隔之间。 */
	interval = msecs_to_jiffies(sd->balance_interval);
	interval = clamp(interval, 1UL, max_load_balance_interval);
	sdg->sgc->next_update = jiffies + interval;

	if (!child) {
		update_cpu_capacity(sd, cpu);
		return;
	}

	capacity = 0;
	min_capacity = ULONG_MAX;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	max_capacity = 0;

	if (child->flags & SD_NUMA) {
		/*
		 * SD_NUMA domains cannot assume that child groups
		 * span the current group.
		 */
		/* NUMA child 边界可与当前组错位，只能逐 CPU 汇总。 */

		for_each_cpu(cpu, sched_group_span(sdg)) {
			unsigned long cpu_cap = capacity_of(cpu);

			capacity += cpu_cap;
			min_capacity = min(cpu_cap, min_capacity);
			max_capacity = max(cpu_cap, max_capacity);
		}
	} else  {
		/*
		 * !SD_NUMA domains can assume that child groups
		 * span the current group.
		 */
		/* 非 NUMA child groups 完整分割当前组，可直接聚合缓存容量。 */

		group = child->groups;
		do {
			struct sched_group_capacity *sgc = group->sgc;

			capacity += sgc->capacity;
			min_capacity = min(sgc->min_capacity, min_capacity);
			max_capacity = max(sgc->max_capacity, max_capacity);
			group = group->next;
		} while (group != child->groups);
	}

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	sdg->sgc->capacity = capacity;
	sdg->sgc->min_capacity = min_capacity;
	sdg->sgc->max_capacity = max_capacity;
}

/*
 * Check whether the capacity of the rq has been noticeably reduced by side
 * activity. The imbalance_pct is used for the threshold.
 * Return true is the capacity is reduced
 */
/* 本段定义组统计口径、候选优先级及允许不平衡的计算边界。 */
static inline int
check_cpu_capacity(struct rq *rq, struct sched_domain *sd)
{
	return ((rq->cpu_capacity * sd->imbalance_pct) <
				(arch_scale_cpu_capacity(cpu_of(rq)) * 100));
}

/* Check if the rq has a misfit task */
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
static inline bool check_misfit_status(struct rq *rq)
{
	return rq->misfit_task_load;
}

/*
 * Group imbalance indicates (and tries to solve) the problem where balancing
 * groups is inadequate due to ->cpus_ptr constraints.
 *
 * Imagine a situation of two groups of 4 CPUs each and 4 tasks each with a
 * cpumask covering 1 CPU of the first group and 3 CPUs of the second group.
 * Something like:
 *
 *	{ 0 1 2 3 } { 4 5 6 7 }
 *	        *     * * *
 *
 * If we were to balance group-wise we'd place two tasks in the first group and
 * two tasks in the second group. Clearly this is undesired as it will overload
 * cpu 3 and leave one of the CPUs in the second group unused.
 *
 * The current solution to this issue is detecting the skew in the first group
 * by noticing the lower domain failed to reach balance and had difficulty
 * moving tasks due to affinity constraints.
 *
 * When this is so detected; this group becomes a candidate for busiest; see
 * update_sd_pick_busiest(). And calculate_imbalance() and
 * sched_balance_find_src_group() avoid some of the usual balance conditions to allow it
 * to create an effective group imbalance.
 *
 * This is a somewhat tricky proposition since the next run might not find the
 * group imbalance and decide the groups need to be balanced again. A most
 * subtle and fragile situation.
 */
/*
 * cpus_ptr 可使组级平均失真：两个四 CPU 组各四任务，但任务只允许第一组一个 CPU、第二组三个，
 * 简单 2:2 会压满 CPU3 并闲置另组 CPU。低层因亲和性搬移失败时置 imbalance，上层把该组提升为
 * busiest 并放宽条件；下一轮标志可能消失又追求组平均，因此这是依赖失败状态的脆弱修复。
 */

/* sg_imbalanced() 读取组容量对象的亲和性失衡提示；无副作用。 */
static inline int sg_imbalanced(struct sched_group *group)
{
	return group->sgc->imbalance;
}

/*
 * group_has_capacity returns true if the group has spare capacity that could
 * be used by some tasks.
 * We consider that a group has spare capacity if the number of task is
 * smaller than the number of CPUs or if the utilization is lower than the
 * available capacity for CFS tasks.
 * For the latter, we use a threshold to stabilize the state, to take into
 * account the variance of the tasks' load and to return true if the available
 * capacity in meaningful for the load balancer.
 * As an example, an available capacity of 1% can appear but it doesn't make
 * any benefit for the load balance.
 */
/*
 * group_has_capacity() - 判断组是否有有意义的接收余量。任务数少于 CPU 数直接为真；否则用
 * imbalance_pct 滞回比较 runnable/util 与 capacity，过滤微小无效余量。只读快照、无副作用。
 */
static inline bool
group_has_capacity(unsigned int imbalance_pct, struct sg_lb_stats *sgs)
{
	if (sgs->sum_nr_running < sgs->group_weight)
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
		return true;

	if ((sgs->group_capacity * imbalance_pct) <
			(sgs->group_runnable * 100))
		return false;

	if ((sgs->group_capacity * 100) >
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			(sgs->group_util * imbalance_pct))
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return true;

	return false;
}

/*
 *  group_is_overloaded returns true if the group has more tasks than it can
 *  handle.
 *  group_is_overloaded is not equals to !group_has_capacity because a group
 *  with the exact right number of tasks, has no more spare capacity but is not
 *  overloaded so both group_has_capacity and group_is_overloaded return
 *  false.
 */
/*
 * group_is_overloaded() - 判断任务竞争是否超过组容量。它不是 !group_has_capacity：恰好满载组
 * 既无余量也不过载。EAS/uclamp 下还要求至少一 CPU overutilized；只读统计、无副作用。
 */
static inline bool
group_is_overloaded(unsigned int imbalance_pct, struct sg_lb_stats *sgs)
{
	/*
	 * With EAS and uclamp, 1 CPU in the group must be overutilized to
	 * consider the group overloaded.
	 */
	/* EAS 下需 CPU 级过载证据，避免 clamp 后聚合值误报。 */
	if (sched_energy_enabled() && !sgs->group_overutilized)
		return false;

	if (sgs->sum_nr_running <= sgs->group_weight)
		return false;

	if ((sgs->group_capacity * 100) <
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
			(sgs->group_util * imbalance_pct))
		return true;

	if ((sgs->group_capacity * imbalance_pct) <
			(sgs->group_runnable * 100))
		return true;

	return false;
}

/*
 * group_classify() - 按拉取优先级把组统计映射为唯一类型：overloaded、LLC、亲和性失衡、ASYM、
 * SMT、misfit、fully-busy、spare。返回值可直接比较，无状态副作用。
 */
static inline enum
group_type group_classify(unsigned int imbalance_pct,
			  struct sched_group *group,
			  struct sg_lb_stats *sgs)
{
	if (group_is_overloaded(imbalance_pct, sgs))
		return group_overloaded;

	if (sgs->group_llc_balance)
		return group_llc_balance;

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (sg_imbalanced(group))
		return group_imbalanced;

	if (sgs->group_asym_packing)
		return group_asym_packing;

	if (sgs->group_smt_balance)
		return group_smt_balance;

	if (sgs->group_misfit_task_load)
		return group_misfit_task;

	if (!group_has_capacity(imbalance_pct, sgs))
		return group_fully_busy;

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return group_has_spare;
}

/**
 * sched_use_asym_prio - Check whether asym_packing priority must be used
 * @sd:		The scheduling domain of the load balancing
 * @cpu:	A CPU
 *
 * Always use CPU priority when balancing load between SMT siblings. When
 * balancing load between cores, it is not sufficient that @cpu is idle. Only
 * use CPU priority if the whole core is idle.
 *
 * Returns: True if the priority of @cpu must be followed. False otherwise.
 */
/*
 * sched_use_asym_prio() - 判断均衡是否应服从 cpu 的 ASYM_PACKING 优先级。
 * 同 SMT sibling 总按优先级；跨物理核仅整核空闲时使用，避免把单线程 idle 误当可独占高优先核。
 * 入参借用 sd/cpu，返回布尔值，无副作用、不睡眠。
 */
static bool sched_use_asym_prio(struct sched_domain *sd, int cpu)
{
	if (!(sd->flags & SD_ASYM_PACKING))
		return false;

	if (!sched_smt_active())
		return true;

	return sd->flags & SD_SHARE_CPUCAPACITY || is_core_idle(cpu);
}

/* sched_asym() 仅当 dst 可执行 asym packing 且优先级高于 src 时允许拉取；无副作用。 */
static inline bool sched_asym(struct sched_domain *sd, int dst_cpu, int src_cpu)
{
	/*
	 * First check if @dst_cpu can do asym_packing load balance. Only do it
	 * if it has higher priority than @src_cpu.
	 */
	/* 先验证 dst 的域/SMT 前置条件，再比较架构优先级。 */
	return sched_use_asym_prio(sd, dst_cpu) &&
		sched_asym_prefer(dst_cpu, src_cpu);
}

/**
 * sched_group_asym - Check if the destination CPU can do asym_packing balance
 * @env:	The load balancing environment
 * @sgs:	Load-balancing statistics of the candidate busiest group
 * @group:	The candidate busiest group
 *
 * @env::dst_cpu can do asym_packing if it has higher priority than the
 * preferred CPU of @group.
 *
 * Return: true if @env::dst_cpu can do with asym_packing load balance. False
 * otherwise.
 */
/*
 * sched_group_asym() - 判断 dst_cpu 能否从候选组执行 asym-packing 拉取。
 * @env/@sgs/@group 均为借用统计；返回布尔值。SMT 组若超过一个忙 sibling，CPU 优先级不再能
 *代表整核可用性，故拒绝；否则比较 dst 与组 preferred CPU。
 */
static inline bool
sched_group_asym(struct lb_env *env, struct sg_lb_stats *sgs, struct sched_group *group)
{
	/*
	 * CPU priorities do not make sense for SMT cores with more than one
	 * busy sibling.
	 */
	/* 多个忙 sibling 时单 CPU 优先级没有可操作意义。 */
	if ((group->flags & SD_SHARE_CPUCAPACITY) &&
	    (sgs->group_weight - sgs->idle_cpus != 1))
		return false;

	return sched_asym(env->sd, env->dst_cpu, READ_ONCE(group->asym_prefer_cpu));
}

/* One group has more than one SMT CPU while the other group does not */
/* smt_vs_nonsmt_groups() 检测两组是否分属 SMT 共享容量与非 SMT 类型；空组指针返回 false。 */
static inline bool smt_vs_nonsmt_groups(struct sched_group *sg1,
				    struct sched_group *sg2)
{
	if (!sg1 || !sg2)
		return false;

	return (sg1->flags & SD_SHARE_CPUCAPACITY) !=
		(sg2->flags & SD_SHARE_CPUCAPACITY);
}

/*
 * smt_balance() - idle 目标存在时判断是否值得从满载 SMT 组迁出任务以独占 CPU 容量。
 * 返回布尔提示；单 sibling 组不会带 SD_SHARE_CPUCAPACITY，非 idle 场景不触发。
 */
static inline bool smt_balance(struct lb_env *env, struct sg_lb_stats *sgs,
			       struct sched_group *group)
{
	if (!env->idle)
		return false;

	/*
	 * For SMT source group, it is better to move a task
	 * to a CPU that doesn't have multiple tasks sharing its CPU capacity.
	 * Note that if a group has a single SMT, SD_SHARE_CPUCAPACITY
	 * will not be on.
	 */
	/* 源为共享容量 SMT 且有多个层级 CFS runnable 时，迁往非争用 CPU 有收益。 */
	if (group->flags & SD_SHARE_CPUCAPACITY &&
	    sgs->sum_h_nr_running > 1)
		return true;

	return false;
}

/*
 * sibling_imbalance() - 以每核 nr_running 比例计算 SMT/非 SMT sibling 组应迁任务数。
 * 入参为当前 env 与两组统计；返回非负任务数。等核数直接求差，异核数交叉相乘并四舍五入；
 * 本地空组且源有多个任务时最少返回 2，以真正利用空组资源。
 */
static inline long sibling_imbalance(struct lb_env *env,
				    struct sd_lb_stats *sds,
				    struct sg_lb_stats *busiest,
				    struct sg_lb_stats *local)
{
	int ncores_busiest, ncores_local;
	long imbalance;

	if (!env->idle || !busiest->sum_nr_running)
		return 0;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	ncores_busiest = sds->busiest->cores;
	ncores_local = sds->local->cores;

	if (ncores_busiest == ncores_local) {
		imbalance = busiest->sum_nr_running;
		lsub_positive(&imbalance, local->sum_nr_running);
		return imbalance;
	}

	/* Balance such that nr_running/ncores ratio are same on both groups */
	/* 交叉乘法避免先除法丢精度，再按总核数归一化。 */
	imbalance = ncores_local * busiest->sum_nr_running;
	lsub_positive(&imbalance, ncores_busiest * local->sum_nr_running);
	/* Normalize imbalance and do rounding on normalization */
	imbalance = 2 * imbalance + ncores_local + ncores_busiest;
	imbalance /= ncores_local + ncores_busiest;

	/* Take advantage of resource in an empty sched group */
	/* 计算仅 1 但本地全空时提升为 2，避免 SMT 源继续堆叠而空核闲置。 */
	if (imbalance <= 1 && local->sum_nr_running == 0 &&
	    busiest->sum_nr_running > 1)
		imbalance = 2;

	return imbalance;
}

/* sched_reduced_capacity() 仅在 rq 恰有一个 CFS runnable 时检查侧向压力减容；多任务已归 overloaded。 */
static inline bool
sched_reduced_capacity(struct rq *rq, struct sched_domain *sd)
{
	/*
	 * When there is more than 1 task, the group_overloaded case already
	 * takes care of cpu with reduced capacity
	 */
	if (rq->cfs.h_nr_runnable != 1)
		return false;

	return check_cpu_capacity(rq, sd);
}

#ifdef CONFIG_SCHED_CACHE
/*
 * Record the statistics for this scheduler group for later
 * use. These values guide load balancing on aggregating tasks
 * to a LLC.
 */
/*
 * record_sg_llc_stats() - 把跨 LLC 组的 util/capacity 快照发布到对应共享 LLC 对象。
 * 入参：@env/@sgs/@group 均借用。返回：无；仅 cache-aware 已启用、非 NEWIDLE 且当前域正跨多个
 * LLC 时 WRITE_ONCE 更新共享统计。注意事项：用组首 CPU 定位其 shared，不能用本地 child->shared。
 */
static void record_sg_llc_stats(struct lb_env *env,
				struct sg_lb_stats *sgs,
				struct sched_group *group)
{
	struct sched_domain_shared *sd_share;
	int cpu;

	if (!sched_cache_enabled() || env->idle == CPU_NEWLY_IDLE)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;

	/* Only care about sched domain spanning multiple LLCs */
	if (env->sd->child != rcu_dereference_all(per_cpu(sd_llc, env->dst_cpu)))
		return;

	/*
	 * At this point we know this group spans a LLC domain.
	 * Record the statistic of this group in its corresponding
	 * shared LLC domain.
	 * Note: sd_share cannot be obtained via sd->child->shared,
	 * because the latter refers to the domain that covers the
	 * local group. Instead, sd_share should be located using
	 * the first CPU of the LLC group.
	 */
	/* child->shared 指向本地组覆盖域，远端组必须通过该 LLC 首 CPU 的 per-CPU 槽定位。 */
	cpu = cpumask_first(sched_group_span(group));
	sd_share = rcu_dereference_all(per_cpu(sd_llc_shared, cpu));
	if (!sd_share)
		return;

	if (READ_ONCE(sd_share->util_avg) != sgs->group_util)
		WRITE_ONCE(sd_share->util_avg, sgs->group_util);

	if (unlikely(READ_ONCE(sd_share->capacity) != sgs->group_capacity))
		WRITE_ONCE(sd_share->capacity, sgs->group_capacity);
}

/*
 * Do LLC balance on sched group that contains LLC, and have tasks preferring
 * to run on LLC in idle dst_cpu.
 */
/*
 * llc_balance() - 判断候选组是否含可迁往 idle dst preferred LLC 的任务。
 * 仅跨 LLC、失败次数未越过 cache_nice_tries 且 nr_pref_dst_llc 非零，并经 can_migrate_llc() 允许时
 * 返回 true；无副作用。
 */
static inline bool llc_balance(struct lb_env *env, struct sg_lb_stats *sgs,
			       struct sched_group *group)
{
	if (!sched_cache_enabled())
		return false;

	if (env->sd->flags & SD_SHARE_LLC)
		return false;

	/*
	 * Skip cache aware tagging if nr_balanced_failed is sufficiently high.
	 * Threshold of cache_nice_tries is set to 1 higher than nr_balance_failed
	 * to avoid excessive task migration at the same time.
	 */
	/* 连续失败足够多时让通用均衡接管，避免 cache-aware 与强制迁移同时过度搬运。 */
	if (env->sd->nr_balance_failed >= env->sd->cache_nice_tries + 1)
		return false;

	if (sgs->nr_pref_dst_llc &&
	    can_migrate_llc(cpumask_first(sched_group_span(group)),
			    env->dst_cpu, 0, true) == mig_llc)
		return true;

	return false;
}

/* update_llc_busiest() 选择包含更多“偏好 dst LLC”任务的组；只读比较，无副作用。 */
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
static bool update_llc_busiest(struct lb_env *env,
			       struct sg_lb_stats *busiest,
			       struct sg_lb_stats *sgs)
{
	/*
	 * There are more tasks that want to run on dst_cpu's LLC.
	 */
	return sgs->nr_pref_dst_llc > busiest->nr_pref_dst_llc;
}
#else
/* CONFIG_SCHED_CACHE 关闭时记录与 LLC 专项选择均为中性 stub。 */
static inline void record_sg_llc_stats(struct lb_env *env, struct sg_lb_stats *sgs,
				       struct sched_group *group)
{
}

static inline bool llc_balance(struct lb_env *env, struct sg_lb_stats *sgs,
			       struct sched_group *group)
{
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
	return false;
}

static bool update_llc_busiest(struct lb_env *env,
			       struct sg_lb_stats *busiest,
			       struct sg_lb_stats *sgs)
{
	return false;
}
#endif

/**
 * update_sg_lb_stats - Update sched_group's statistics for load balancing.
 * @env: The load balancing environment.
 * @sds: Load-balancing data with statistics of the local group.
 * @group: sched_group whose statistics are to be updated.
 * @sgs: variable to hold the statistics for this group.
 * @sg_overloaded: sched_group is overloaded
 */
/*
 * update_sg_lb_stats() - 扫描 group 内可用 CPU，重建负载、容量、任务数及专项分类输入。
 * 入参：@env 是均衡环境；@sds 含 local 组；@group 为目标拓扑组；@sgs 是完整输出；
 * @sg_overloaded 是 root-domain 过载输出。返回：无。
 * 注意事项：RCU/拓扑保护下无锁读取各 rq 快照，不睡眠；local 组不生成作为源的 misfit/asym/SMT/LLC
 * 标志，最终 group_type 在扫描后统一分类。
 */
static inline void update_sg_lb_stats(struct lb_env *env,
				      struct sd_lb_stats *sds,
				      struct sched_group *group,
				      struct sg_lb_stats *sgs,
				      bool *sg_overloaded)
{
	int i, nr_running, local_group, sd_flags = env->sd->flags;
	bool balancing_at_rd = !env->sd->parent;

	/* 每组完整清零，建立本轮独立快照。 */
	memset(sgs, 0, sizeof(*sgs));

	local_group = group == sds->local;

	/* 阶段 1：聚合交集 CPU 的 load/util/runnable、任务数、过载和可选 NUMA/LLC 统计。 */
	for_each_cpu_and(i, sched_group_span(group), env->cpus) {
		struct rq *rq = cpu_rq(i);
		unsigned long load = cpu_load(rq);

		sgs->group_load += load;
		sgs->group_util += cpu_util_cfs(i);
		sgs->group_runnable += cpu_runnable(rq);
		sgs->sum_h_nr_running += rq->cfs.h_nr_runnable;

		nr_running = rq->nr_running;
		sgs->sum_nr_running += nr_running;

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (cpu_overutilized(i))
			sgs->group_overutilized = 1;

#ifdef CONFIG_SCHED_CACHE
		if (sched_cache_enabled()) {
			struct sched_domain *sd_tmp;
			int dst_llc;

			dst_llc = llc_id(env->dst_cpu);
			if (llc_id(i) != dst_llc) {
				sd_tmp = rcu_dereference_all(rq->sd);
				if (sd_tmp && (unsigned int)dst_llc < sd_tmp->llc_max)
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
					sgs->nr_pref_dst_llc += sd_tmp->llc_counts[dst_llc];
			}
		}
#endif

		/*
		 * No need to call idle_cpu() if nr_running is not 0
		 */
		/* nr_running 非零已知不 idle，避免昂贵状态检查；idle CPU 也不可能有 misfit。 */
		if (!nr_running && idle_cpu(i)) {
			sgs->idle_cpus++;
			/* Idle cpu can't have misfit task */
			continue;
		}

		/* Overload indicator is only updated at root domain */
		/* root-domain 级全局 overutilized 状态只在最顶层收集。 */
		if (balancing_at_rd && nr_running > 1)
			*sg_overloaded = 1;

#ifdef CONFIG_NUMA_BALANCING
		/* Only fbq_classify_group() uses this to classify NUMA groups */
/* 只有 NUMA 组分类路径使用此值。 */
		if (sd_flags & SD_NUMA) {
			sgs->nr_numa_running += rq->nr_numa_running;
			sgs->nr_preferred_running += rq->nr_preferred_running;
		}
#endif
		/* 本地组只作目标基线，不参与“从该组拉取”的专项源标记。 */
		if (local_group)
			continue;

		if (sd_flags & SD_ASYM_CPUCAPACITY) {
			/* Check for a misfit task on the cpu */
/* 检查该 CPU 是否有容量不匹配任务。 */
			if (sgs->group_misfit_task_load < rq->misfit_task_load) {
				sgs->group_misfit_task_load = rq->misfit_task_load;
				*sg_overloaded = 1;
			}
		} else if (env->idle && sched_reduced_capacity(rq, env->sd)) {
			/* Check for a task running on a CPU with reduced capacity */
			if (sgs->group_misfit_task_load < load)
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
				sgs->group_misfit_task_load = load;
		}
	}

	/* 阶段 2：补充拓扑容量/权重，并仅对远端组生成专项拉取原因。 */
	sgs->group_capacity = group->sgc->capacity;

	sgs->group_weight = group->group_weight;

	if (!local_group) {
		/* Check if dst CPU is idle and preferred to this group */
/* 检查目标 CPU 是否空闲且在拓扑排序上更优。 */
		if (env->idle && sgs->sum_h_nr_running &&
		    sched_group_asym(env, sgs, group))
			sgs->group_asym_packing = 1;

		/* Check for loaded SMT group to be balanced to dst CPU */
/* 检查是否应把繁忙 SMT 组任务分散到目标 CPU。 */
		if (smt_balance(env, sgs, group))
			sgs->group_smt_balance = 1;

		/* Check for tasks in this group can be moved to their preferred LLC */
		if (llc_balance(env, sgs, group))
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			sgs->group_llc_balance = 1;
	}

	sgs->group_type = group_classify(env->sd->imbalance_pct, group, sgs);

	record_sg_llc_stats(env, sgs, group);
	/* Computing avg_load makes sense only when group is overloaded */
/* 只有过载组的 avg_load 才具有比较意义。 */
	if (sgs->group_type == group_overloaded)
		sgs->avg_load = (sgs->group_load * SCHED_CAPACITY_SCALE) /
				sgs->group_capacity;
}

/**
 * update_sd_pick_busiest - return 1 on busiest group
 * @env: The load balancing environment.
 * @sds: sched_domain statistics
 * @sg: sched_group candidate to be checked for being the busiest
 * @sgs: sched_group statistics
 *
 * Determine if @sg is a busier group than the previously selected
 * busiest group.
 *
 * Return: %true if @sg is a busier group than the previously selected
 * busiest group. %false otherwise.
 */
/*
 * update_sd_pick_busiest() - 判断候选 sg 是否应替换当前 busiest。
 * 先按 group_type 优先级，再按类型专属规则破平局；misfit 还要求 idle 整核、目标容量更强且本地有
 * 余量。入参均为借用统计，返回布尔值，无副作用；在拓扑保护下不睡眠。
 */
static bool update_sd_pick_busiest(struct lb_env *env,
				   struct sd_lb_stats *sds,
				   struct sched_group *sg,
				   struct sg_lb_stats *sgs)
{
	struct sg_lb_stats *busiest = &sds->busiest_stat;

	/* Make sure that there is at least one task to pull */
	/* 没有层级 CFS runnable 的组不可能提供迁移任务。 */
	if (!sgs->sum_h_nr_running)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return false;

	/*
	 * Don't try to pull misfit tasks we can't help.
	 * We can use max_capacity here as reduction in capacity on some
	 * CPUs in the group should either be possible to resolve
	 * internally or be covered by avg_load imbalance (eventually).
	 *
	 * When SMT is active, only pull a misfit to dst_cpu if it is on a
	 * fully idle core; otherwise the effective capacity of the core is
	 * reduced and we may not actually provide more capacity than the
	 * source.
	 */
	/* SMT 下目标必须整核空闲且容量显著更高，否则 misfit 迁移未必增加有效容量。 */
	if ((env->sd->flags & SD_ASYM_CPUCAPACITY) &&
	    (sgs->group_type == group_misfit_task) &&
	    (!env->dst_core_idle ||
	     !capacity_greater(capacity_of(env->dst_cpu), sg->sgc->max_capacity) ||
	     sds->local_stat.group_type != group_has_spare))
		return false;

	if (sgs->group_type > busiest->group_type)
		return true;

	if (sgs->group_type < busiest->group_type)
		return false;

	/*
	 * The candidate and the current busiest group are the same type of
	 * group. Let check which one is the busiest according to the type.
	 */
	/* 同类型进入专属破平局规则；每个 case 决定可观察状态与后续返回。 */

	switch (sgs->group_type) {
	case group_overloaded:
		/* Select the overloaded group with highest avg_load. */
		/* 过载组选择归一化 avg_load 最大者。 */
		return sgs->avg_load > busiest->avg_load;

	case group_llc_balance:
		/* Select the group with most tasks preferring dst LLC */
		/* LLC 类型选择偏好目标 LLC 的任务更多者。 */
		return update_llc_busiest(env, busiest, sgs);

	case group_imbalanced:
		/*
		 * Select the 1st imbalanced group as we don't have any way to
		 * choose one more than another.
		 */
		/* 亲和性失衡无法量化强弱，保留首次候选避免抖动。 */
		return false;

	case group_asym_packing:
		/* Prefer to move from lowest priority CPU's work */
		/* 从优先级更低的 CPU 所在组拉取。 */
		return sched_asym_prefer(READ_ONCE(sds->busiest->asym_prefer_cpu),
					 READ_ONCE(sg->asym_prefer_cpu));

	case group_misfit_task:
		/*
		 * If we have more than one misfit sg go with the biggest
		 * misfit.
		 */
		/* 多个 misfit 组选择最大不适配负载。 */
		return sgs->group_misfit_task_load > busiest->group_misfit_task_load;

	case group_smt_balance:
		/*
		 * Check if we have spare CPUs on either SMT group to
		 * choose has spare or fully busy handling.
		 */
		/* SMT 任一组有 idle CPU 时转入 spare 规则，否则按 fully-busy 比较。 */
		if (sgs->idle_cpus != 0 || busiest->idle_cpus != 0)
			goto has_spare;

		fallthrough;

	case group_fully_busy:
		/*
		 * Select the fully busy group with highest avg_load. In
		 * theory, there is no need to pull task from such kind of
		 * group because tasks have all compute capacity that they need
		 * but we can still improve the overall throughput by reducing
		 * contention when accessing shared HW resources.
		 *
		 * XXX for now avg_load is not computed and always 0 so we
		 * select the 1st one, except if @sg is composed of SMT
		 * siblings.
		 */
		/* fully-busy 理论无需拉取，但可能减少共享硬件争用；avg_load 尚恒 0 的 TODO 使其多保留首组。 */

		if (sgs->avg_load < busiest->avg_load)
			return false;

		if (sgs->avg_load == busiest->avg_load) {
			/*
			 * SMT sched groups need more help than non-SMT groups.
			 * If @sg happens to also be SMT, either choice is good.
			 */
			/* SMT 组比非 SMT 更需帮助；当前 busiest 已是 SMT 时不替换。 */
			if (sds->busiest->flags & SD_SHARE_CPUCAPACITY)
				return false;
		}

		break;

	case group_has_spare:
		/*
		 * Do not pick sg with SMT CPUs over sg with pure CPUs,
		 * as we do not want to pull task off SMT core with one task
		 * and make the core idle.
		 */
		/* spare 平局不从仅一任务的 SMT 核拉走任务致整核空闲，纯 CPU 组优先。 */
		if (smt_vs_nonsmt_groups(sds->busiest, sg)) {
			if (sg->flags & SD_SHARE_CPUCAPACITY && sgs->sum_h_nr_running <= 1)
				return false;
			else
				return true;
		}
has_spare:

		/*
		 * Select not overloaded group with lowest number of idle CPUs
		 * and highest number of running tasks. We could also compare
		 * the spare capacity which is more stable but it can end up
		 * that the group has less spare capacity but finally more idle
		 * CPUs which means less opportunity to pull tasks.
		 */
		/* 先选 idle CPU 更少、再选 running 更多的组，最大化可拉任务机会。 */
		if (sgs->idle_cpus > busiest->idle_cpus)
			return false;
		else if ((sgs->idle_cpus == busiest->idle_cpus) &&
			 (sgs->sum_nr_running <= busiest->sum_nr_running))
			return false;

		break;
	}

	/*
	 * Candidate sg has no more than one task per CPU and has higher
	 * per-CPU capacity. Migrating tasks to less capable CPUs may harm
	 * throughput. Maximize throughput, power/energy consequences are not
	 * considered.
	 */
	/* 候选每 CPU 至多一任务且最小容量高于 dst 时拒绝向弱 CPU 拉取，吞吐优先于能耗。 */
	if ((env->sd->flags & SD_ASYM_CPUCAPACITY) &&
	    (sgs->group_type <= group_fully_busy) &&
	    (capacity_greater(sg->sgc->min_capacity, capacity_of(env->dst_cpu))))
		return false;

	return true;
}

#ifdef CONFIG_NUMA_BALANCING
/* fbq_classify_group()：含非 NUMA task 为 regular；全 NUMA 但有非 preferred 为 remote；否则 all。 */
static inline enum fbq_type fbq_classify_group(struct sg_lb_stats *sgs)
{
	if (sgs->sum_h_nr_running > sgs->nr_numa_running)
		return regular;
	if (sgs->sum_h_nr_running > sgs->nr_preferred_running)
		return remote;
	return all;
}

/* rq 版本使用运行任务计数执行同一 NUMA 队列分类。 */
static inline enum fbq_type fbq_classify_rq(struct rq *rq)
{
	if (rq->nr_running > rq->nr_numa_running)
		return regular;
	if (rq->nr_running > rq->nr_preferred_running)
		return remote;
	return all;
}
#else /* !CONFIG_NUMA_BALANCING: */
/* 无 NUMA 时组扫描不需过滤（all），单 rq 普通分类为 regular。 */
static inline enum fbq_type fbq_classify_group(struct sg_lb_stats *sgs)
{
	return all;
}

static inline enum fbq_type fbq_classify_rq(struct rq *rq)
{
	return regular;
}
#endif /* !CONFIG_NUMA_BALANCING */


	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
struct sg_lb_stats;

/*
 * task_running_on_cpu - return 1 if @p is running on @cpu.
 */
/* task_running_on_cpu() 预测从统计中扣 p：仅 p 属于 cpu、有 PELT 历史且已排队时返回 1。 */
static unsigned int task_running_on_cpu(int cpu, struct task_struct *p)
{
	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return 0;

	if (task_on_rq_queued(p))
		return 1;

	return 0;
}

/**
 * idle_cpu_without - would a given CPU be idle without p ?
 * @cpu: the processor on which idleness is tested.
 * @p: task which should be ignored.
 *
 * Return: 1 if the CPU would be idle. 0 otherwise.
 */
/*
 * idle_cpu_without() - 在调用者已扣除 p 的 nr_running 后判断 cpu 是否会 idle。
 * curr 若为其他任务或存在 ttwu_pending 返回 0，否则 1。入参借用、无副作用；不能直接使用原始
 * rq->nr_running，调用前必须计算无 p 版本。
 */
static int idle_cpu_without(int cpu, struct task_struct *p)
{
	struct rq *rq = cpu_rq(cpu);

	if (rq->curr != rq->idle && rq->curr != p)
		return 0;

	/*
	 * rq->nr_running can't be used but an updated version without the
	 * impact of p on cpu must be used instead. The updated nr_running
	 * be computed and tested before calling idle_cpu_without().
	 */
	/* nr_running 的 p 贡献必须由上层先扣除，本函数只检查 curr 与待处理唤醒。 */

	if (rq->ttwu_pending)
		return 0;

	return 1;
}

/*
 * update_sg_wakeup_stats - Update sched_group's statistics for wakeup.
 * @sd: The sched_domain level to look for idlest group.
 * @group: sched_group whose statistics are to be updated.
 * @sgs: variable to hold the statistics for this group.
 * @p: The task for which we look for the idlest group/CPU.
 */
/*
 * update_sg_wakeup_stats() - 重建“忽略 p 后”的组统计供慢速选目标组。
 * @sd/@group/@p 为借用输入，@sgs 完整输出；逐允许 CPU 扣 p 对负载和任务数的贡献，统计 idle、
 * capacity、misfit 并分类。返回无；无锁近似快照、不睡眠。
 */
static inline void update_sg_wakeup_stats(struct sched_domain *sd,
					  struct sched_group *group,
					  struct sg_lb_stats *sgs,
					  struct task_struct *p)
{
	int i, nr_running;

	/* 异构域先假定无 CPU 能容纳 p，扫描到任一适配 CPU 再清 misfit。 */
	memset(sgs, 0, sizeof(*sgs));

	/* Assume that task can't fit any CPU of the group */
	if (sd->flags & SD_ASYM_CPUCAPACITY)
		sgs->group_misfit_task_load = 1;

	for_each_cpu_and(i, sched_group_span(group), p->cpus_ptr) {
		struct rq *rq = cpu_rq(i);
		unsigned int local;

		sgs->group_load += cpu_load_without(rq, p);
		sgs->group_util += cpu_util_without(i, p);
		sgs->group_runnable += cpu_runnable_without(rq, p);
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		local = task_running_on_cpu(i, p);
		sgs->sum_h_nr_running += rq->cfs.h_nr_runnable - local;

		nr_running = rq->nr_running - local;
		sgs->sum_nr_running += nr_running;

		/*
		 * No need to call idle_cpu_without() if nr_running is not 0
		 */
		/* 扣 p 后仍有任务必不 idle，为零才检查 curr/ttwu_pending。 */
		if (!nr_running && idle_cpu_without(i, p))
			sgs->idle_cpus++;

		/* Check if task fits in the CPU */
		if (sd->flags & SD_ASYM_CPUCAPACITY &&
		    sgs->group_misfit_task_load &&
		    task_fits_cpu(p, i))
			sgs->group_misfit_task_load = 0;

	}

	sgs->group_capacity = group->sgc->capacity;

	sgs->group_weight = group->group_weight;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	sgs->group_type = group_classify(sd->imbalance_pct, group, sgs);

	/*
	 * Computing avg_load makes sense only when group is fully busy or
	 * overloaded
	 */
	/* avg_load 只服务 fully-busy/overloaded 的同类破平局。 */
	if (sgs->group_type == group_fully_busy ||
		sgs->group_type == group_overloaded)
		sgs->avg_load = (sgs->group_load * SCHED_CAPACITY_SCALE) /
				sgs->group_capacity;
}

/*
 * update_pick_idlest() - 判断 group 是否比当前 idlest 更适合接收 p。
 * 先选更低 group_type；同类按 avg_load、最大容量或 idle CPU/总 util 破平局。LLC/imbalanced/asym/SMT
 * 不用于慢速 wakeup。只读统计、无副作用。
 */
static bool update_pick_idlest(struct sched_group *idlest,
			       struct sg_lb_stats *idlest_sgs,
			       struct sched_group *group,
			       struct sg_lb_stats *sgs)
{
	if (sgs->group_type < idlest_sgs->group_type)
		return true;

	if (sgs->group_type > idlest_sgs->group_type)
		return false;

	/*
	 * The candidate and the current idlest group are the same type of
	 * group. Let check which one is the idlest according to the type.
	 */
	/* switch 为每类定义目标组偏好，未胜出则保留已有 idlest 以减少抖动。 */

	switch (sgs->group_type) {
	case group_overloaded:
	case group_fully_busy:
		/* Select the group with lowest avg_load. */
/* 选择平均负载最低的组。 */
		if (idlest_sgs->avg_load <= sgs->avg_load)
			return false;
		break;

	case group_llc_balance:
	case group_imbalanced:
	case group_asym_packing:
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	case group_smt_balance:
		/* Those types are not used in the slow wakeup path */
		return false;

	case group_misfit_task:
		/* Select group with the highest max capacity */
/* 选择最大 CPU 容量最高的组。 */
		if (idlest->sgc->max_capacity >= group->sgc->max_capacity)
			return false;
		break;

	case group_has_spare:
		/* Select group with most idle CPUs */
/* 选择空闲 CPU 数最多的组。 */
		if (idlest_sgs->idle_cpus > sgs->idle_cpus)
			return false;

		/* Select group with lowest group_util */
		if (idlest_sgs->idle_cpus == sgs->idle_cpus &&
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			idlest_sgs->group_util <= sgs->group_util)
			return false;

		break;
	}

	return true;
}

/*
 * sched_balance_find_dst_group() finds and returns the least busy CPU group within the
 * domain.
 *
 * Assumes p is allowed on at least one CPU in sd.
 */
/*
 * sched_balance_find_dst_group() - 在 sd 中选择比 this_cpu 本地组更适合接收 p 的最闲组。
 * @sd 为受保护域，@p 为借用任务且至少允许域内一 CPU，@this_cpu 定义 local 组。返回借用组，
 * 本地不劣或无合法组时 NULL；无副作用、不睡眠。
 */
static struct sched_group *
sched_balance_find_dst_group(struct sched_domain *sd, struct task_struct *p, int this_cpu)
{
	struct sched_group *idlest = NULL, *local = NULL, *group = sd->groups;
	struct sg_lb_stats local_sgs, tmp_sgs;
	struct sg_lb_stats *sgs;
	unsigned long imbalance;
	struct sg_lb_stats idlest_sgs = {
			.avg_load = UINT_MAX,
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			.group_type = group_overloaded,
	};

	/* 环形遍历，跳过亲和性/cookie 无交集组，分别保存 local 与最佳远端快照。 */
	do {
		int local_group;

		/* Skip over this group if it has no CPUs allowed */
		if (!cpumask_intersects(sched_group_span(group),
					p->cpus_ptr))
			continue;

		/* Skip over this group if no cookie matched */
/* 没有 core cookie 匹配 CPU 时跳过该组。 */
		if (!sched_group_cookie_match(cpu_rq(this_cpu), p, group))
			continue;

		local_group = cpumask_test_cpu(this_cpu,
					       sched_group_span(group));

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (local_group) {
			sgs = &local_sgs;
			local = group;
		} else {
			sgs = &tmp_sgs;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		}

		update_sg_wakeup_stats(sd, group, sgs, p);

		if (!local_group && update_pick_idlest(idlest, &idlest_sgs, group, sgs)) {
			idlest = group;
			idlest_sgs = *sgs;
		}

	} while (group = group->next, group != sd->groups);


	/* There is no idlest group to push tasks to */
	/* 无合法远端候选则保持本地。 */
	if (!idlest)
		return NULL;

	/* The local group has been skipped because of CPU affinity */
	/* p 不允许本地组时直接采用远端。 */
	if (!local)
		return idlest;

	/*
	 * If the local group is idler than the selected idlest group
	 * don't try and push the task.
	 */
	/* 类型数值更低代表更闲；本地更闲则不迁。 */
	if (local_sgs.group_type < idlest_sgs.group_type)
		return NULL;

	/*
	 * If the local group is busier than the selected idlest group
	 * try and push the task.
	 */
	/* 本地类型更忙则直接迁，无需同类型细比。 */
	if (local_sgs.group_type > idlest_sgs.group_type)
		return idlest;

	switch (local_sgs.group_type) {
	case group_overloaded:
	case group_fully_busy:

		/* Calculate allowed imbalance based on load */
/* 根据负载计算可容忍的不平衡量。 */
		imbalance = scale_load_down(NICE_0_LOAD) *
				(sd->imbalance_pct-100) / 100;

		/*
		 * When comparing groups across NUMA domains, it's possible for
		 * the local domain to be very lightly loaded relative to the
		 * remote domains but "imbalance" skews the comparison making
		 * remote CPUs look much more favourable. When considering
		 * cross-domain, add imbalance to the load on the remote node
		 * and consider staying local.
		 */
		/* NUMA 跨节点给远端加滞回，避免固定 imbalance 让轻载本地错误外迁。 */

		if ((sd->flags & SD_NUMA) &&
		    ((idlest_sgs.avg_load + imbalance) >= local_sgs.avg_load))
			return NULL;

		/*
		 * If the local group is less loaded than the selected
		 * idlest group don't try and push any tasks.
		 */
/* 本段定义组统计口径、候选优先级及允许不平衡的计算边界。 */
		if (idlest_sgs.avg_load >= (local_sgs.avg_load + imbalance))
			return NULL;

		if (100 * local_sgs.avg_load <= sd->imbalance_pct * idlest_sgs.avg_load)
			return NULL;
		break;

	case group_llc_balance:
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	case group_imbalanced:
	case group_asym_packing:
	case group_smt_balance:
		/* Those type are not used in the slow wakeup path */
		return NULL;

	case group_misfit_task:
		/* Select group with the highest max capacity */
		/* misfit 仅在远端最大容量更高时值得迁移。 */
		if (local->sgc->max_capacity >= idlest->sgc->max_capacity)
			return NULL;
		break;

	case group_has_spare:
#ifdef CONFIG_NUMA
		if (sd->flags & SD_NUMA) {
			int imb_numa_nr = sd->imb_numa_nr;
#ifdef CONFIG_NUMA_BALANCING
			int idlest_cpu;
			/*
			 * If there is spare capacity at NUMA, try to select
			 * the preferred node
			 */
			/* 有 spare 时先尊重 NUMA preferred node。 */
			if (cpu_to_node(this_cpu) == p->numa_preferred_nid)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
				return NULL;

			idlest_cpu = cpumask_first(sched_group_span(idlest));
			if (cpu_to_node(idlest_cpu) == p->numa_preferred_nid)
				return idlest;
#endif /* CONFIG_NUMA_BALANCING */
			/*
			 * Otherwise, keep the task close to the wakeup source
			 * and improve locality if the number of running tasks
			 * would remain below threshold where an imbalance is
			 * allowed while accounting for the possibility the
			 * task is pinned to a subset of CPUs. If there is a
			 * real need of migration, periodic load balance will
			 * take care of it.
			 */
			/* 允许阈值内不平衡以保持 wake-source 局部性，真实失衡由周期均衡修复。 */
			if (p->nr_cpus_allowed != NR_CPUS) {
				unsigned int w = cpumask_weight_and(p->cpus_ptr,
								sched_group_span(local));
				imb_numa_nr = min(w, sd->imb_numa_nr);
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
			}

			imbalance = abs(local_sgs.idle_cpus - idlest_sgs.idle_cpus);
			if (!adjust_numa_imbalance(imbalance,
						   local_sgs.sum_nr_running + 1,
						   imb_numa_nr)) {
				return NULL;
			}
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
		}
#endif /* CONFIG_NUMA */

		/*
		 * Select group with highest number of idle CPUs. We could also
		 * compare the utilization which is more stable but it can end
		 * up that the group has less spare capacity but finally more
		 * idle CPUs which means more opportunity to run task.
		 */
		/* spare 类型按 idle CPU 数优先真实立即运行机会。 */
		if (local_sgs.idle_cpus >= idlest_sgs.idle_cpus)
			return NULL;
		break;
	}

	return idlest;
}

/*
 * update_idle_cpu_scan() - 根据 LLC 总利用率更新 select_idle_cpu() 的动态扫描预算。
 * @env 提供 LLC 域，@sum_util 为域总 util；返回无，周期均衡时写 shared->nr_idle_scan。
 * 共享 cacheline 写昂贵，NEWIDLE 不更新；SIS_UTIL 关闭或无 shared 时无副作用、不睡眠。
 */
static void update_idle_cpu_scan(struct lb_env *env,
				 unsigned long sum_util)
{
	struct sched_domain_shared *sd_share;
	struct sched_domain *sd = env->sd;
	int llc_weight, pct;
	u64 x, y, tmp;
	/*
	 * Update the number of CPUs to scan in LLC domain, which could
	 * be used as a hint in select_idle_cpu(). The update of sd_share
	 * could be expensive because it is within a shared cache line.
	 * So the write of this hint only occurs during periodic load
	 * balancing, rather than CPU_NEWLY_IDLE, because the latter
	 * can fire way more frequently than the former.
	 */
	/* 只在低频周期均衡写提示，避免高频 NEWIDLE 反复争用共享 cacheline。 */
	if (!sched_feat(SIS_UTIL) || env->idle == CPU_NEWLY_IDLE)
		return;

	sd_share = sd->shared;
	if (!sd_share)
		return;

	/*
	 * The number of CPUs to search drops as sum_util increases, when
	 * sum_util hits 85% or above, the scan stops.
	 * The reason to choose 85% as the threshold is because this is the
	 * imbalance_pct(117) when a LLC sched group is overloaded.
	 *
	 * let y = SCHED_CAPACITY_SCALE - p * x^2                       [1]
	 * and y'= y / SCHED_CAPACITY_SCALE
	 *
	 * x is the ratio of sum_util compared to the CPU capacity:
	 * x = sum_util / (llc_weight * SCHED_CAPACITY_SCALE)
	 * y' is the ratio of CPUs to be scanned in the LLC domain,
	 * and the number of CPUs to scan is calculated by:
	 *
	 * nr_scan = llc_weight * y'                                    [2]
	 *
	 * When x hits the threshold of overloaded, AKA, when
	 * x = 100 / pct, y drops to 0. According to [1],
	 * p should be SCHED_CAPACITY_SCALE * pct^2 / 10000
	 *
	 * Scale x by SCHED_CAPACITY_SCALE:
	 * x' = sum_util / llc_weight;                                  [3]
	 *
	 * and finally [1] becomes:
	 * y = SCHED_CAPACITY_SCALE -
	 *     x'^2 * pct^2 / (10000 * SCHED_CAPACITY_SCALE)            [4]
	 *
	 */
	/*
	 * 扫描比例 y' 随利用率比 x 二次下降，达到约 85%（imbalance_pct=117 对应阈值）归零。
	 * 由 y=scale-p*x²、nr_scan=llc_weight*y' 推导，使用 x'=sum_util/llc_weight 的定点式 4，
	 * 最终得到 0..llc_weight 的预算；高负载停止无望扫描，低负载扩大搜索。
	 */
	/* equation [3] */
	x = sum_util;
	llc_weight = sd->span_weight;
	do_div(x, llc_weight);

	/* equation [4] */
/* 应用公式四把任务数差异换算成负载差异。 */
	pct = sd->imbalance_pct;
	tmp = x * x * pct * pct;
	do_div(tmp, 10000 * SCHED_CAPACITY_SCALE);
	tmp = min_t(long, tmp, SCHED_CAPACITY_SCALE);
	y = SCHED_CAPACITY_SCALE - tmp;

	/* equation [2] */
	y *= llc_weight;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	do_div(y, SCHED_CAPACITY_SCALE);
	if ((int)y != sd_share->nr_idle_scan)
		WRITE_ONCE(sd_share->nr_idle_scan, (int)y);
}

/**
 * update_sd_lb_stats - Update sched_domain's statistics for load balancing.
 * @env: The load balancing environment.
 * @sds: variable to hold the statistics for this sched_domain.
 */
/*
 * update_sd_lb_stats() - 遍历 sd 全部组，填充 local/busiest 与全域累计值并发布 root-domain 状态。
 * @env 为输入输出均衡环境，@sds 为完整输出。返回无；可能更新组容量、fbq_type、rd overload/
 * overutilized 和 idle 扫描预算。注意事项：拓扑保护下无锁读取 rq 快照，不睡眠。
 */
static inline void update_sd_lb_stats(struct lb_env *env, struct sd_lb_stats *sds)
{
	struct sched_group *sg = env->sd->groups;
	struct sg_lb_stats *local = &sds->local_stat;
	struct sg_lb_stats tmp_sgs;
	unsigned long sum_util = 0;
	bool sg_overloaded = 0, sg_overutilized = 0;

	/* misfit 拉取必须知道 dst 是否整核空闲。 */
	env->dst_core_idle = !sched_smt_active() || is_core_idle(env->dst_cpu);

	/* 环形扫描每组；本地组按周期刷新容量，远端组参加 busiest 比较。 */
	do {
		struct sg_lb_stats *sgs = &tmp_sgs;
		int local_group;

		local_group = cpumask_test_cpu(env->dst_cpu, sched_group_span(sg));
		if (local_group) {
			sds->local = sg;
			sgs = local;

			if (env->idle != CPU_NEWLY_IDLE ||
			    time_after_eq(jiffies, sg->sgc->next_update))
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
				update_group_capacity(env->sd, env->dst_cpu);
		}

		update_sg_lb_stats(env, sds, sg, sgs, &sg_overloaded);

		if (!local_group && update_sd_pick_busiest(env, sds, sg, sgs)) {
			sds->busiest = sg;
			sds->busiest_stat = *sgs;
		}

		sg_overutilized |= sgs->group_overutilized;

		/* Now, start updating sd_lb_stats */
		sds->total_load += sgs->group_load;
		sds->total_capacity += sgs->group_capacity;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		sum_util += sgs->group_util;
		sg = sg->next;
	} while (sg != env->sd->groups);

	/*
	 * Indicate that the child domain of the busiest group prefers tasks
	 * go to a child's sibling domains first. NB the flags of a sched group
	 * are those of the child domain.
	 */
	/* busiest 的 flags 来自 child domain；PREFER_SIBLING 要求先向其 sibling 分散任务。 */
	if (sds->busiest)
		sds->prefer_sibling = !!(sds->busiest->flags & SD_PREFER_SIBLING);


	if (env->sd->flags & SD_NUMA)
		env->fbq_type = fbq_classify_group(&sds->busiest_stat);

	if (!env->sd->parent) {
		/* update overload indicator if we are at root domain */
		/* 只有根域拥有完整任务竞争视图，可同时更新 overloaded。 */
		set_rd_overloaded(env->dst_rq->rd, sg_overloaded);

		/* Update over-utilization (tipping point, U >= 0) indicator */
		/* overutilized 可由任意层观察后向 root 置位，清除则仅根域完整扫描负责。 */
		set_rd_overutilized(env->dst_rq->rd, sg_overutilized);
	} else if (sg_overutilized) {
		set_rd_overutilized(env->dst_rq->rd, sg_overutilized);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	update_idle_cpu_scan(env, sum_util);
}

/**
 * calculate_imbalance - Calculate the amount of imbalance present within the
 *			 groups of a given sched_domain during load balance.
 * @env: load balance environment
 * @sds: statistics of the sched_domain whose imbalance is to be calculated.
 */
/*
 * calculate_imbalance() - 把 local/busiest 分类转换为迁移单位和数量。
 * @env 输出 migration_type/imbalance，@sds 为只读统计；返回无。专项类型先处理，spare 本地按 util
 * 或任务数填充，两边过载则按 avg_load 求不跨过域平均值的最小 load；不睡眠。
 */
static inline void calculate_imbalance(struct lb_env *env, struct sd_lb_stats *sds)
{
	struct sg_lb_stats *local, *busiest;

	local = &sds->local_stat;
	busiest = &sds->busiest_stat;

	/* misfit：异构域搬一个任务；同构减容场景按 misfit load 搬。 */
	if (busiest->group_type == group_misfit_task) {
		if (env->sd->flags & SD_ASYM_CPUCAPACITY) {
			/* Set imbalance to allow misfit tasks to be balanced. */
			env->migration_type = migrate_misfit;
			env->imbalance = 1;
		} else {
			/*
			 * Set load imbalance to allow moving task from cpu
			 * with reduced capacity.
			 */
			env->migration_type = migrate_load;
			env->imbalance = busiest->group_misfit_task_load;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		}
		return;
	}

	/* asym packing 尝试搬走该组全部层级 runnable。 */
	if (busiest->group_type == group_asym_packing) {
		/*
		 * In case of asym capacity, we will try to migrate all load to
		 * the preferred CPU.
		 */
		env->migration_type = migrate_task;
		env->imbalance = busiest->sum_h_nr_running;
		return;
	}

	/* SMT 搬一个即可减少 sibling 共享。 */
	if (busiest->group_type == group_smt_balance) {
		/* Reduce number of tasks sharing CPU capacity */
		env->migration_type = migrate_task;
		env->imbalance = 1;
		return;
	}

#ifdef CONFIG_SCHED_CACHE
	/* LLC 专项每轮搬一个偏好本地 LLC 的任务。 */
	if (busiest->group_type == group_llc_balance) {
		/* Move a task that prefer local LLC */
		env->migration_type = migrate_llc_task;
		env->imbalance = 1;
		return;
	}
#endif

	/* 亲和性失衡不能信任组平均，先搬一个，下一轮再修复余下偏差。 */
	if (busiest->group_type == group_imbalanced) {
		/*
		 * In the group_imb case we cannot rely on group-wide averages
		 * to ensure CPU-load equilibrium, try to move any task to fix
		 * the imbalance. The next load balance will take care of
		 * balancing back the system.
		 */
		env->migration_type = migrate_task;
		env->imbalance = 1;
		return;
	}

	/*
	 * Try to use spare capacity of local group without overloading it or
	 * emptying busiest.
	 */
	/* 本地有 spare 时填充但不使其过载，也不抽空源。 */
	if (local->group_type == group_has_spare) {
		if ((busiest->group_type > group_fully_busy) &&
		    !(env->sd->flags & SD_SHARE_LLC)) {
			/*
			 * If busiest is overloaded, try to fill spare
			 * capacity. This might end up creating spare capacity
			 * in busiest or busiest still being overloaded but
			 * there is no simple way to directly compute the
			 * amount of load to migrate in order to balance the
			 * system.
			 */
			/* 非共享 LLC 的过载源以 util 为单位填满本地容量余量。 */
			env->migration_type = migrate_util;
			env->imbalance = max(local->group_capacity, local->group_util) -
					 local->group_util;

			/*
			 * In some cases, the group's utilization is max or even
			 * higher than capacity because of migrations but the
			 * local CPU is (newly) idle. There is at least one
			 * waiting task in this overloaded busiest group. Let's
			 * try to pull it.
			 */
			if (env->idle && env->imbalance == 0) {
				/* util 快照虽满但 CPU 实际 idle 时至少拉一个，维持 work-conserving。 */
				env->migration_type = migrate_task;
				env->imbalance = 1;
			}

			return;
		}

		if (busiest->group_weight == 1 || sds->prefer_sibling) {
			/*
			 * When prefer sibling, evenly spread running tasks on
			 * groups.
			 */
			/* 单 CPU 组或 prefer_sibling 按每核任务比例均分。 */
			env->migration_type = migrate_task;
			env->imbalance = sibling_imbalance(env, sds, busiest, local);
		} else {

			/*
			 * If there is no overload, we just want to even the number of
			 * idle CPUs.
			 */
			/* 无过载时只均衡 idle CPU 个数。 */
			env->migration_type = migrate_task;
			env->imbalance = max_t(long, 0,
					       (local->idle_cpus - busiest->idle_cpus));
		}

	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
#ifdef CONFIG_NUMA
		/* Consider allowing a small imbalance between NUMA groups */
		if (env->sd->flags & SD_NUMA) {
			env->imbalance = adjust_numa_imbalance(env->imbalance,
							       local->sum_nr_running + 1,
							       env->sd->imb_numa_nr);
		}
#endif

		/* Number of tasks to move to restore balance */
		/* 一次迁移同时缩小两侧差值，任务数除以 2。 */
		env->imbalance >>= 1;

		return;
	}

	/*
	 * Local is fully busy but has to take more load to relieve the
	 * busiest group
	 */
	/* 本地 fully-busy 仍需接收以缓解源，以下用 avg_load 精确约束。 */
	if (local->group_type < group_overloaded) {
		/*
		 * Local will become overloaded so the avg_load metrics are
		 * finally needed.
		 */

		local->avg_load = (local->group_load * SCHED_CAPACITY_SCALE) /
				  local->group_capacity;

		/*
		 * If the local group is more loaded than the selected
		 * busiest group don't try to pull any tasks.
		 */
/* 本段定义组统计口径、候选优先级及允许不平衡的计算边界。 */
		if (local->avg_load >= busiest->avg_load) {
			env->imbalance = 0;
			return;
		}

		sds->avg_load = (sds->total_load * SCHED_CAPACITY_SCALE) /
				sds->total_capacity;

		/*
		 * If the local group is more loaded than the average system
		 * load, don't try to pull any tasks.
		 */
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (local->avg_load >= sds->avg_load) {
			env->imbalance = 0;
			return;
		}

	}

	/*
	 * Both group are or will become overloaded and we're trying to get all
	 * the CPUs to the average_load, so we don't want to push ourselves
	 * above the average load, nor do we wish to reduce the max loaded CPU
	 * below the average load. At the same time, we also don't want to
	 * reduce the group load below the group capacity. Thus we look for
	 * the minimum possible imbalance.
	 */
	/* 两侧均过载：取“源降到平均”与“目标升到平均”可移动 load 的较小值。 */
	env->migration_type = migrate_load;
	env->imbalance = min(
		(busiest->avg_load - sds->avg_load) * busiest->group_capacity,
		(sds->avg_load - local->avg_load) * local->group_capacity
	) / SCHED_CAPACITY_SCALE;
}

/******* sched_balance_find_src_group() helpers end here *********************/
/* 源组选择辅助函数到此结束。 */

/*
 * Decision matrix according to the local and busiest group type:
 *
 * busiest \ local has_spare fully_busy misfit asym imbalanced overloaded
 * has_spare        nr_idle   balanced   N/A    N/A  balanced   balanced
 * fully_busy       nr_idle   nr_idle    N/A    N/A  balanced   balanced
 * misfit_task      force     N/A        N/A    N/A  N/A        N/A
 * asym_packing     force     force      N/A    N/A  force      force
 * imbalanced       force     force      N/A    N/A  force      force
 * overloaded       force     force      N/A    N/A  force      avg_load
 *
 * N/A :      Not Applicable because already filtered while updating
 *            statistics.
 * balanced : The system is balanced for these 2 groups.
 * force :    Calculate the imbalance as load migration is probably needed.
 * avg_load : Only if imbalance is significant enough.
 * nr_idle :  dst_cpu is not busy and the number of idle CPUs is quite
 *            different in groups.
 */
/*
 * 决策矩阵把 local/busiest 类型组合映射为：N/A（统计阶段已过滤）、balanced（无需迁移）、force
 *（专项失衡直接计算）、avg_load（仅显著过载差异）、nr_idle（目标不忙且 idle CPU 数差异显著）。
 * spare/fully-busy 多按 idle 数；misfit/asym/imbalanced 强制；两边 overloaded 才进入 avg_load。
 */

/**
 * sched_balance_find_src_group - Returns the busiest group within the sched_domain
 * if there is an imbalance.
 * @env: The load balancing environment.
 *
 * Also calculates the amount of runnable load which should be moved
 * to restore balance.
 *
 * Return:	- The busiest group if imbalance exists.
 */
/* 根据组类型与 imbalance 指标选择源组；统计快照只是本轮均衡输入。 */
/*
 * sched_balance_find_src_group() - 在 env->sd 找到确有可迁 imbalance 的 busiest 源组。
 * 入参：@env 为输入输出环境。返回借用组或 NULL；成功同时设置 env->migration_type/imbalance。
 * 注意事项：拓扑保护下不睡眠；EAS root 未过载时退出，专项 misfit/asym/affinity 可绕过普通公平检查。
 */
static struct sched_group *sched_balance_find_src_group(struct lb_env *env)
{
	struct sg_lb_stats *local, *busiest;
	struct sd_lb_stats sds;

	init_sd_lb_stats(&sds);

	/*
	 * Compute the various statistics relevant for load balancing at
	 * this level.
	 */
	/* 阶段 1：收集完整域统计、本地/最忙组和 root-domain 状态。 */
	update_sd_lb_stats(env, &sds);

	/* There is no busy sibling group to pull tasks from */
	/* 无源候选即平衡。 */
	if (!sds.busiest)
		goto out_balanced;

	busiest = &sds.busiest_stat;

	/* Misfit tasks should be dealt with regardless of the avg load */
	/* misfit 容量问题不受平均 load 掩盖。 */
	if (busiest->group_type == group_misfit_task)
		goto force_balance;

	/* EAS 可用且 root 未过载时避免通用负载迁移破坏能效放置。 */
	if (!is_rd_overutilized(env->dst_rq->rd) &&
	    rcu_dereference_all(env->dst_rq->rd->pd))
		goto out_balanced;

	/* ASYM feature bypasses nice load balance check */
	/* asym packing 是拓扑优先级目标，绕过 nice 平均检查。 */
	if (busiest->group_type == group_asym_packing)
		goto force_balance;

	/*
	 * If the busiest group is imbalanced the below checks don't
	 * work because they assume all things are equal, which typically
	 * isn't true due to cpus_ptr constraints and the like.
	 */
	/* affinity 失衡破坏“组内同质”假设，直接强制计算。 */
	if (busiest->group_type == group_imbalanced)
		goto force_balance;

	local = &sds.local_stat;
	/*
	 * If the local group is busier than the selected busiest group
	 * don't try and pull any tasks.
	 */
/* 本段说明候选 CPU、亲和性固定、主动迁移与退避计数的处理顺序。 */
	if (local->group_type > busiest->group_type)
		goto out_balanced;

	/*
	 * When groups are overloaded, use the avg_load to ensure fairness
	 * between tasks.
	 */
	/* 两组过载时用 avg_load 保证任务比例公平并应用滞回。 */
	if (local->group_type == group_overloaded) {
		/*
		 * If the local group is more loaded than the selected
		 * busiest group don't try to pull any tasks.
		 */
		if (local->avg_load >= busiest->avg_load)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			goto out_balanced;

		/* XXX broken for overlapping NUMA groups */
		sds.avg_load = (sds.total_load * SCHED_CAPACITY_SCALE) /
				sds.total_capacity;

		/*
		 * Don't pull any tasks if this group is already above the
		 * domain average load.
		 */
/* 本段说明候选 CPU、亲和性固定、主动迁移与退避计数的处理顺序。 */
		if (local->avg_load >= sds.avg_load)
			goto out_balanced;

		/*
		 * If the busiest group is more loaded, use imbalance_pct to be
		 * conservative.
		 */
/* 本段说明候选 CPU、亲和性固定、主动迁移与退避计数的处理顺序。 */
		if (100 * busiest->avg_load <=
				env->sd->imbalance_pct * local->avg_load)
			goto out_balanced;
	}

	/*
	 * Try to move all excess tasks to a sibling domain of the busiest
	 * group's child domain.
	 */
	/* prefer_sibling 希望把多余任务先分散到 child sibling。 */
	if (sds.prefer_sibling && local->group_type == group_has_spare &&
	    (busiest->group_type == group_llc_balance ||
	    sibling_imbalance(env, &sds, busiest, local) > 1))
		goto force_balance;

	if (busiest->group_type != group_overloaded) {
		if (!env->idle) {
			/*
			 * If the busiest group is not overloaded (and as a
			 * result the local one too) but this CPU is already
			 * busy, let another idle CPU try to pull task.
			 */
			/* 两组都未过载且本 CPU 已忙，让真正 idle CPU 执行拉取。 */
			goto out_balanced;
		}

		if (busiest->group_type == group_smt_balance &&
		    smt_vs_nonsmt_groups(sds.local, sds.busiest)) {
			/* Let non SMT CPU pull from SMT CPU sharing with sibling */
/* 允许非 SMT CPU 从有忙兄弟的 SMT CPU 拉取任务。 */
			goto force_balance;
		}

		if (busiest->group_weight > 1 &&
		    local->idle_cpus <= (busiest->idle_cpus + 1)) {
			/*
			 * If the busiest group is not overloaded
			 * and there is no imbalance between this and busiest
			 * group wrt idle CPUs, it is balanced. The imbalance
			 * becomes significant if the diff is greater than 1
			 * otherwise we might end up to just move the imbalance
			 * on another group. Of course this applies only if
			 * there is more than 1 CPU per group.
			 */
			/* 多 CPU 组 idle 差不超过 1 不迁，避免只把不平衡搬到另一组。 */
			goto out_balanced;
		}

		if (busiest->sum_h_nr_running == 1) {
			/*
			 * busiest doesn't have any tasks waiting to run
			 */
			/* 源只有一个 runnable，无等待任务可安全拉取。 */
			goto out_balanced;
		}
	}

force_balance:
	/* Looks like there is an imbalance. Compute it */
	/* 专项或显著不平衡进入单位/数量计算；结果为 0 仍返回 NULL。 */
	calculate_imbalance(env, &sds);
	return env->imbalance ? sds.busiest : NULL;

out_balanced:
	env->imbalance = 0;
	return NULL;
}

/*
 * sched_balance_find_src_rq - find the busiest runqueue among the CPUs in the group.
 */
/* 在选定组内找最合适源 rq，并过滤 capacity/亲和性上不可能迁移的队列。 */
/*
 * sched_balance_find_src_rq() - 在已选 busiest group 内按 migration_type 选择具体源 rq。
 * @env 为均衡环境，@group 为借用拓扑组；返回借用 rq 或 NULL。按 load/capacity 比、boost util、
 * nr_running、最大 misfit、偏好 dst LLC 数分别选最大者；无副作用、不睡眠。
 */
static struct rq *sched_balance_find_src_rq(struct lb_env *env,
				     struct sched_group *group)
{
	struct rq *busiest = NULL, *rq;
	unsigned long busiest_util = 0, busiest_load = 0, busiest_capacity = 1;
	unsigned int __maybe_unused busiest_pref_llc = 0;
	struct sched_domain __maybe_unused *sd_tmp;
	unsigned int busiest_nr = 0;
	int __maybe_unused dst_llc;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	int i;

	/* 扫描仍允许的 CPU，并先用 NUMA fbq 分类、任务数和容量/ASYM 条件过滤不可能源。 */
	for_each_cpu_and(i, sched_group_span(group), env->cpus) {
		unsigned long capacity, load, util;
		unsigned int nr_running;
		enum fbq_type rt;

		rq = cpu_rq(i);
		rt = fbq_classify_rq(rq);

		/*
		 * We classify groups/runqueues into three groups:
		 *  - regular: there are !numa tasks
		 *  - remote:  there are numa tasks that run on the 'wrong' node
		 *  - all:     there is no distinction
		 *
		 * In order to avoid migrating ideally placed numa tasks,
		 * ignore those when there's better options.
		 *
		 * If we ignore the actual busiest queue to migrate another
		 * task, the next balance pass can still reduce the busiest
		 * queue by moving tasks around inside the node.
		 *
		 * If we cannot move enough load due to this classification
		 * the next pass will adjust the group classification and
		 * allow migration of more tasks.
		 *
		 * Both cases only affect the total convergence complexity.
		 */
		/*
		 * regular/remote/all 逐步放宽 NUMA 任务范围，优先不移动已在 preferred node 的任务；若因此
		 * 略过真实最忙 rq，后续节点内/下一轮仍会收敛，只增加时间复杂度。
		 */
		if (rt > env->fbq_type)
			continue;

		nr_running = rq->cfs.h_nr_runnable;
		if (!nr_running)
			continue;

		capacity = capacity_of(i);

		/*
		 * For ASYM_CPUCAPACITY domains, don't pick a CPU that could
		 * eventually lead to active_balancing high->low capacity.
		 * Higher per-CPU capacity is considered better than balancing
		 * average load.
		 */
		/* 异构域不从仅一任务且容量不低于 dst 的 CPU 拉取，避免随后 high→low 主动迁移。 */
		if (env->sd->flags & SD_ASYM_CPUCAPACITY &&
		    !capacity_greater(capacity_of(env->dst_cpu), capacity) &&
		    nr_running == 1)
			continue;

		/*
		 * Make sure we only pull tasks from a CPU of lower priority
		 * when balancing between SMT siblings.
		 *
		 * If balancing between cores, let lower priority CPUs help
		 * SMT cores with more than one busy sibling.
		 */
		/* SMT sibling 间只从较低优先级拉；跨核时允许低优先 dst 帮助多 sibling 繁忙核。 */
		if (sched_asym(env->sd, i, env->dst_cpu) && nr_running == 1)
			continue;

		switch (env->migration_type) {
		case migrate_load:
			/* load 单位先用未按容量缩放的值检查单任务是否会过量，再按 load/capacity 比选源。 */
			/*
			 * When comparing with load imbalance, use cpu_load()
			 * which is not scaled with the CPU capacity.
			 */
			load = cpu_load(rq);

			if (nr_running == 1 && load > env->imbalance &&
			    !check_cpu_capacity(rq, env->sd))
				break;

			/*
			 * For the load comparisons with the other CPUs,
			 * consider the cpu_load() scaled with the CPU
			 * capacity, so that the load can be moved away
			 * from the CPU that is potentially running at a
			 * lower capacity.
			 *
			 * Thus we're looking for max(load_i / capacity_i),
			 * crosswise multiplication to rid ourselves of the
			 * division works out to:
			 * load_i * capacity_j > load_j * capacity_i;
			 * where j is our previous maximum.
			 */
			/* 交叉相乘比较 load_i/cap_i，避免除法精度损失，偏向从减容 CPU 拉走负载。 */
			if (load * busiest_capacity > busiest_load * capacity) {
				busiest_load = load;
				busiest_capacity = capacity;
				busiest = rq;
			}
			break;

		case migrate_util:
			/* util 模式选 boosted CFS util 最大且至少两任务的 rq，否则 detach 必然失败。 */
			util = cpu_util_cfs_boost(i);

			/*
			 * Don't try to pull utilization from a CPU with one
			 * running task. Whatever its utilization, we will fail
			 * detach the task.
			 */
			if (nr_running <= 1)
				continue;

			if (busiest_util < util) {
				busiest_util = util;
				busiest = rq;
			}
			break;

		case migrate_task:
			/* task 模式选层级 runnable 最多者。 */
			if (busiest_nr < nr_running) {
				busiest_nr = nr_running;
				busiest = rq;
			}
			break;

		case migrate_misfit:
			/*
			 * For ASYM_CPUCAPACITY domains with misfit tasks we
			 * simply seek the "biggest" misfit task.
			 */
			/* misfit 模式选 rq->misfit_task_load 最大者。 */
			if (rq->misfit_task_load > busiest_load) {
				busiest_load = rq->misfit_task_load;
				busiest = rq;
			}

			break;

		case migrate_llc_task:
			/* LLC 模式选偏好 dst LLC 的任务计数最多者。 */
#ifdef CONFIG_SCHED_CACHE
			sd_tmp = rcu_dereference_all(rq->sd);
			dst_llc = llc_id(env->dst_cpu);

			if (sd_tmp && (unsigned)dst_llc < sd_tmp->llc_max) {
				unsigned int this_pref_llc =
					sd_tmp->llc_counts[dst_llc];

				if (busiest_pref_llc < this_pref_llc) {
					busiest_pref_llc = this_pref_llc;
					busiest = rq;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
				}
			}
#endif
			break;

		}
	}

	return busiest;
}

/*
 * Max backoff if we encounter pinned tasks. Pretty arbitrary value, but
 * so long as it is large enough.
 */
#define MAX_PINNED_INTERVAL	512

/*
 * asym_active_balance() - 判断是否需 stop-machine 风格主动迁移实现 ASYM_PACKING。
 * idle dst 必须可用优先级；dst 更优，或 src 因忙 SMT sibling 不应坚持优先级时返回 true。
 * 入参借用 env，返回布尔值，无副作用。
 */
static inline bool
asym_active_balance(struct lb_env *env)
{
	/*
	 * ASYM_PACKING needs to force migrate tasks from busy but lower
	 * priority CPUs in order to pack all tasks in the highest priority
	 * CPUs. When done between cores, do it only if the whole core if the
	 * whole core is idle.
	 *
	 * If @env::src_cpu is an SMT core with busy siblings, let
	 * the lower priority @env::dst_cpu help it. Do not follow
	 * CPU priority.
	 */
	/* 跨核只在 dst 整核空闲强制；忙 sibling 的 src 可由较低优先 dst 帮助。 */
	return env->idle && sched_use_asym_prio(env->sd, env->dst_cpu) &&
	       (sched_asym_prefer(env->dst_cpu, env->src_cpu) ||
		!sched_use_asym_prio(env->sd, env->src_cpu));
}

/*
 * imbalanced_active_balance() - 连续普通迁移失败后强制处理任务数/亲和性失衡。
 * migrate_task 且失败次数超过 cache_nice_tries+2 返回 true；无副作用。
 */
static inline bool
imbalanced_active_balance(struct lb_env *env)
{
	struct sched_domain *sd = env->sd;

	/*
	 * The imbalanced case includes the case of pinned tasks preventing a fair
	 * distribution of the load on the system but also the even distribution of the
	 * threads on a system with spare capacity
	 */
	/* 同时覆盖 pinned 导致不公平和有 spare 时线程分布不均。 */
	if ((env->migration_type == migrate_task) &&
	    (sd->nr_balance_failed > sd->cache_nice_tries+2))
		return 1;

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	return 0;
}

/*
 * need_active_balance() - 决定普通 detach 失败后是否安排 CPU stopper 强制迁移。
 * LLC 局部性可否决；ASYM/亲和性失衡、源单任务但减容、misfit/LLC 专项可触发。只读 env，返回布尔。
 */
static int need_active_balance(struct lb_env *env)
{
	struct sched_domain *sd = env->sd;

	if (alb_break_llc(env))
		return 0;

	if (asym_active_balance(env))
		return 1;

	if (imbalanced_active_balance(env))
		return 1;

	/*
	 * The dst_cpu is idle and the src_cpu CPU has only 1 CFS task.
	 * It's worth migrating the task if the src_cpu's capacity is reduced
	 * because of other sched_class or IRQs if more capacity stays
	 * available on dst_cpu.
	 */
	/* 源唯一 CFS 任务受 RT/DL/IRQ 减容且 dst 明显更强时，主动搬走仍有收益。 */
	if (env->idle &&
	    (env->src_rq->cfs.h_nr_runnable == 1)) {
		if ((check_cpu_capacity(env->src_rq, sd)) &&
		    (capacity_of(env->src_cpu)*sd->imbalance_pct < capacity_of(env->dst_cpu)*100))
			return 1;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}

	if (env->migration_type == migrate_misfit ||
	    env->migration_type == migrate_llc_task)
		return 1;

	return 0;
}

static int active_load_balance_cpu_stop(void *data);

/* 只有 group balance CPU 或合适 idle 替代者执行本层均衡，避免重复扫描。 */
/*
 * should_we_balance() - 选出本组唯一应执行当前层均衡的 CPU。
 * NEWIDLE 允许每个刚空闲 CPU 自拉任务，但已有任务/待唤醒则取消；周期均衡优先首个空闲整核，
 * 再首个忙核 idle SMT，最后 group_balance_cpu。返回 dst 是否当选；复用 per-CPU mask，不睡眠。
 */
static int should_we_balance(struct lb_env *env)
{
	struct cpumask *swb_cpus = this_cpu_cpumask_var_ptr(should_we_balance_tmpmask);
	struct sched_group *sg = env->sd->groups;
	int cpu, idle_smt = -1;

	/*
	 * Ensure the balancing environment is consistent; can happen
	 * when the softirq triggers 'during' hotplug.
	 */
	/* 热插拔可使 dst 已离开 active mask，此时环境失效。 */
	if (!cpumask_test_cpu(env->dst_cpu, env->cpus))
		return 0;

	/*
	 * In the newly idle case, we will allow all the CPUs
	 * to do the newly idle load balance.
	 *
	 * However, we bail out if we already have tasks or a wakeup pending,
	 * to optimize wakeup latency.
	 */
	/* NEWIDLE 主动拉取，但本地已有工作/待唤醒时优先恢复执行、降低唤醒延迟。 */
	if (env->idle == CPU_NEWLY_IDLE) {
		if (env->dst_rq->nr_running > 0 || env->dst_rq->ttwu_pending)
			return 0;
		return 1;
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	}

	cpumask_copy(swb_cpus, group_balance_mask(sg));
	/* Try to find first idle CPU */
	for_each_cpu_and(cpu, swb_cpus, env->cpus) {
		if (!idle_cpu(cpu))
			continue;

		/*
		 * Don't balance to idle SMT in busy core right away when
		 * balancing cores, but remember the first idle SMT CPU for
		 * later consideration.  Find CPU on an idle core first.
		 */
		/* 跨核均衡先找整核 idle；忙核的 idle sibling 仅作回退，并跳过同核其余 sibling。 */
		if (sched_smt_active() &&
		    !(env->sd->flags & SD_SHARE_CPUCAPACITY) &&
		    !is_core_idle(cpu)) {
			if (idle_smt == -1)
				idle_smt = cpu;
			/*
			 * If the core is not idle, and first SMT sibling which is
			 * idle has been found, then its not needed to check other
			 * SMT siblings for idleness:
			 */
/* 本段说明候选 CPU、亲和性固定、主动迁移与退避计数的处理顺序。 */
			cpumask_andnot(swb_cpus, swb_cpus, cpu_smt_mask(cpu));
			continue;
		}

		/*
		 * Are we the first idle core in a non-SMT domain or higher,
		 * or the first idle CPU in a SMT domain?
		 */
		/* 第一个合格 idle CPU/core 唯一执行，其他 CPU 返回 false。 */
		return cpu == env->dst_cpu;
	}

	/* Are we the first idle CPU with busy siblings? */
	if (idle_smt != -1)
		return idle_smt == env->dst_cpu;

	/* Are we the first CPU of this group ? */
/* 检查本 CPU 是否为该组第一个均衡 CPU。 */
	return group_balance_cpu(sg) == env->dst_cpu;
}

/*
 * update_lb_imbalance_stat() - 按 migration_type 把本轮剩余 imbalance 记入对应 schedstat。
 * @env/@sd 借用，@idle 选择统计槽；返回无。统计关闭时无副作用，LLC 专项当前不计该组指标。
 */
static void update_lb_imbalance_stat(struct lb_env *env, struct sched_domain *sd,
				     enum cpu_idle_type idle)
{
	if (!schedstat_enabled())
		return;

	switch (env->migration_type) {
	case migrate_load:
		__schedstat_add(sd->lb_imbalance_load[idle], env->imbalance);
		break;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	case migrate_util:
		__schedstat_add(sd->lb_imbalance_util[idle], env->imbalance);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		break;
	case migrate_task:
		__schedstat_add(sd->lb_imbalance_task[idle], env->imbalance);
		break;
	case migrate_misfit:
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
		__schedstat_add(sd->lb_imbalance_misfit[idle], env->imbalance);
		break;
	case migrate_llc_task:
		break;
	}
}

/*
 * This flag serializes load-balancing passes over large domains
 * (above the NODE topology level) - only one load-balancing instance
 * may run at a time, to reduce overhead on very large systems with
 * lots of CPUs and large NUMA distances.
 *
 * - Note that load-balancing passes triggered while another one
 *   is executing are skipped and not re-tried.
 *
 * - Also note that this does not serialize rebalance_domains()
 *   execution, as non-SD_SERIALIZE domains will still be
 *   load-balanced in parallel.
 */
/*
 * sched_balance_running 只串行化 NODE 以上带 SD_SERIALIZE 的大域均衡，降低大 CPU/远 NUMA 系统开销。
 * 已有实例运行时新触发直接跳过且不重试；非 SERIALIZE 域仍可并行，所以它不是全局 rebalance 锁。
 * acquire cmpxchg 获得、完成路径释放，生命周期为全局静态原子标志。
 */
static atomic_t sched_balance_running = ATOMIC_INIT(0);

/*
 * Check this_cpu to ensure it is balanced within domain. Attempt to move
 * tasks if there is an imbalance.
 */
/* 逐组定位 busiest rq 并成批 detach/attach；每次迁移前在双 rq 锁下复验。 */
/*
 * sched_balance_rq() - 在单个 sched_domain 上执行一次完整拉取均衡。
 * @this_cpu/@this_rq 是目标，@sd 当前域，@idle 场景，@continue_balancing 为输出控制。返回迁移任务数；
 * 可能更新失败退避、亲和性失衡、主动 stopper 和域统计。
 * 注意事项：softirq/newidle 上下文不睡眠；env.tasks 必须在返回前为空。源任务 detach 后处于
 * TASK_ON_RQ_MIGRATING，可释放源锁再锁目标 attach；SD_SERIALIZE acquire 必须在所有出口释放。
 */
static int sched_balance_rq(int this_cpu, struct rq *this_rq,
			struct sched_domain *sd, enum cpu_idle_type idle,
			int *continue_balancing)
{
	int ld_moved, cur_ld_moved, active_balance = 0;
	struct sched_domain *sd_parent = sd->parent;
	struct sched_group *group;
	struct rq *busiest;
	struct rq_flags rf;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	struct cpumask *cpus = this_cpu_cpumask_var_ptr(load_balance_mask);
	struct lb_env env = {
		.sd		= sd,
		.dst_cpu	= this_cpu,
		.dst_rq		= this_rq,
		.dst_grpmask    = group_balance_mask(sd->groups),
		.idle		= idle,
		.loop_break	= SCHED_NR_MIGRATE_BREAK,
		.cpus		= cpus,
		.fbq_type	= all,
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
		.tasks		= LIST_HEAD_INIT(env.tasks),
	};
	bool need_unlock = false;

	/* 初始化本轮可选 active CPU 集；热插拔后的非 active CPU 不参与。 */
	cpumask_and(cpus, sched_domain_span(sd), cpu_active_mask);

	schedstat_inc(sd->lb_count[idle]);

redo:
	/* 阶段 1：确认本 CPU 是唯一执行者；大域尝试获得全局串行令牌，失败直接跳过。 */
	if (!should_we_balance(&env)) {
		*continue_balancing = 0;
		goto out_balanced;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	if (!need_unlock && (sd->flags & SD_SERIALIZE)) {
		int zero = 0;
		if (!atomic_try_cmpxchg_acquire(&sched_balance_running, &zero, 1))
			goto out_balanced;

		need_unlock = true;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	/* 阶段 2：选择确有 imbalance 的源组和具体 busiest rq。 */
	group = sched_balance_find_src_group(&env);
	if (!group) {
		schedstat_inc(sd->lb_nobusyg[idle]);
		goto out_balanced;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	}

	busiest = sched_balance_find_src_rq(&env, group);
	if (!busiest) {
		schedstat_inc(sd->lb_nobusyq[idle]);
		goto out_balanced;
	}

	WARN_ON_ONCE(busiest == env.dst_rq);

	update_lb_imbalance_stat(&env, sd, idle);

	env.src_cpu = busiest->cpu;
	env.src_rq = busiest;

	/* 阶段 3：锁源 rq 分批 detach，解锁后锁目标 attach；ALL_PINNED 先作保守假设。 */
	ld_moved = 0;
	/* Clear this flag as soon as we find a pullable task */
	env.flags |= LBF_ALL_PINNED;
	if (busiest->nr_running > 1) {
		/*
		 * Attempt to move tasks. If sched_balance_find_src_group has found
		 * an imbalance but busiest->nr_running <= 1, the group is
		 * still unbalanced. ld_moved simply stays zero, so it is
		 * correctly treated as an imbalance.
		 */
		/* busiest 只有一个任务时仍可能组失衡，但普通批量 detach 不运行，后续失败/active 路径处理。 */
		env.loop_max  = min(sysctl_sched_nr_migrate, busiest->nr_running);

more_balance:
		rq_lock_irqsave(busiest, &rf);
		update_rq_clock(busiest);

		/*
		 * cur_ld_moved - load moved in current iteration
		 * ld_moved     - cumulative load moved across iterations
		 */
/* 本段说明候选 CPU、亲和性固定、主动迁移与退避计数的处理顺序。 */
		cur_ld_moved = detach_tasks(&env);

		/*
		 * We've detached some tasks from busiest_rq. Every
		 * task is masked "TASK_ON_RQ_MIGRATING", so we can safely
		 * unlock busiest->lock, and we are able to be sure
		 * that nobody can manipulate the tasks in parallel.
		 * See task_rq_lock() family for the details.
		 */
		/* MIGRATING 状态阻止其他 task_rq_lock 操作，故可安全跨源/目标两个非同时持有的 rq 锁。 */

		rq_unlock(busiest, &rf);

		if (cur_ld_moved) {
			attach_tasks(&env);
			ld_moved += cur_ld_moved;
		}

		local_irq_restore(rf.flags);

		if (env.flags & LBF_NEED_BREAK) {
			/* 已主动释放长源锁，重置 break 标志后继续同一源。 */
			env.flags &= ~LBF_NEED_BREAK;
			goto more_balance;
		}

		/*
		 * Revisit (affine) tasks on src_cpu that couldn't be moved to
		 * us and move them to an alternate dst_cpu in our sched_group
		 * where they can run. The upper limit on how many times we
		 * iterate on same src_cpu is dependent on number of CPUs in our
		 * sched_group.
		 *
		 * This changes load balance semantics a bit on who can move
		 * load to a given_cpu. In addition to the given_cpu itself
		 * (or a ilb_cpu acting on its behalf where given_cpu is
		 * nohz-idle), we now have balance_cpu in a position to move
		 * load to given_cpu. In rare situations, this may cause
		 * conflicts (balance_cpu and given_cpu/ilb_cpu deciding
		 * _independently_ and at _same_ time to move some load to
		 * given_cpu) causing excess load to be moved to given_cpu.
		 * This however should not happen so much in practice and
		 * moreover subsequent load balance cycles should correct the
		 * excess load moved.
		 */
		/*
		 * 某任务不允许原 dst 但允许同目标组另一 CPU 时，改 dst 后继续同一 src。给定 CPU 与 balance
		 * CPU 可能并发拉入而短暂过量，后续均衡会修正；先从 cpus 清旧 dst 防止重选。
		 */
		if ((env.flags & LBF_DST_PINNED) && env.imbalance > 0) {

			/* Prevent to re-select dst_cpu via env's CPUs */
			__cpumask_clear_cpu(env.dst_cpu, env.cpus);

			env.dst_rq	 = cpu_rq(env.new_dst_cpu);
			env.dst_cpu	 = env.new_dst_cpu;
			env.flags	&= ~LBF_DST_PINNED;
			env.loop	 = 0;
			env.loop_break	 = SCHED_NR_MIGRATE_BREAK;

			/*
			 * Go back to "more_balance" rather than "redo" since we
			 * need to continue with same src_cpu.
			 */
/* 本段说明候选 CPU、亲和性固定、主动迁移与退避计数的处理顺序。 */
			goto more_balance;
		}

		/*
		 * We failed to reach balance because of affinity.
		 */
		/* 部分 pinned 且仍有 imbalance 时向父域发布 group imbalance，允许更高层放宽组平均。 */
		if (sd_parent) {
			int *group_imbalance = &sd_parent->groups->sgc->imbalance;

			if ((env.flags & LBF_SOME_PINNED) && env.imbalance > 0)
				*group_imbalance = 1;
		}

		/* All tasks on this runqueue were pinned by CPU affinity */
		/* 全 pinned 时移除该源 CPU；若目标组之外仍有候选源则 redo，否则结束 pinned。 */
		if (unlikely(env.flags & LBF_ALL_PINNED)) {
			__cpumask_clear_cpu(cpu_of(busiest), cpus);
			/*
			 * Attempting to continue load balancing at the current
			 * sched_domain level only makes sense if there are
			 * active CPUs remaining as possible busiest CPUs to
			 * pull load from which are not contained within the
			 * destination group that is receiving any migrated
			 * load.
			 */
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
			if (!cpumask_subset(cpus, env.dst_grpmask)) {
				env.loop = 0;
				env.loop_break = SCHED_NR_MIGRATE_BREAK;
				goto redo;
			}
			goto out_all_pinned;
		}
	}

	if (!ld_moved) {
		/* 被动拉取失败时仅对真正代表周期均衡失败的场景累计退避依据。 */
		schedstat_inc(sd->lb_failed[idle]);
		/*
		 * Increment the failure counter only on periodic balance.
		 * We do not want newidle balance, which can be very
		 * frequent, pollute the failure counter causing
		 * excessive cache_hot migrations and active balances.
		 *
		 * Similarly for migration_misfit which is not related to
		 * load/util migration, don't pollute nr_balance_failed.
		 *
		 * The same for cache aware scheduling's allowance for
		 * load imbalance. If regular load balance does not
		 * migrate task due to LLC locality, it is a expected
		 * behavior and don't pollute nr_balance_failed.
		 * See can_migrate_task().
		 */
		if (idle != CPU_NEWLY_IDLE &&
		    env.migration_type != migrate_misfit &&
		    !(env.flags & LBF_LLC_PINNED))
			sd->nr_balance_failed++;

		if (need_active_balance(&env)) {
			/* 多次拉取失败或 misfit 等条件满足后，改由源 CPU stopper 主动推出。 */
			unsigned long flags;

			raw_spin_rq_lock_irqsave(busiest, flags);

			/*
			 * Don't kick the active_load_balance_cpu_stop,
			 * if the curr task on busiest CPU can't be
			 * moved to this_cpu:
			 */
/* 本段说明候选 CPU、亲和性固定、主动迁移与退避计数的处理顺序。 */
			if (!cpumask_test_cpu(this_cpu, busiest->curr->cpus_ptr)) {
				raw_spin_rq_unlock_irqrestore(busiest, flags);
				goto out_one_pinned;
			}

			/* Record that we found at least one task that could run on this_cpu */
/* 已发现可在目标 CPU 运行的任务，清除全固定标志。 */
			env.flags &= ~LBF_ALL_PINNED;

			/*
			 * ->active_balance synchronizes accesses to
			 * ->active_balance_work.  Once set, it's cleared
			 * only after active load balance is finished.
			 */
			if (!busiest->active_balance) {
				busiest->active_balance = 1;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
				busiest->push_cpu = this_cpu;
				active_balance = 1;
			}

			preempt_disable();
			raw_spin_rq_unlock_irqrestore(busiest, flags);
			if (active_balance) {
				stop_one_cpu_nowait(cpu_of(busiest),
					active_load_balance_cpu_stop, busiest,
					&busiest->active_balance_work);
			}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			preempt_enable();
		}
	} else {
		/* 一旦被动迁移成功，清零连续失败计数，避免误触发后续主动均衡。 */
		sd->nr_balance_failed = 0;
	}

	if (likely(!active_balance) || need_active_balance(&env)) {
		/* We were unbalanced, so reset the balancing interval */
/* 检测到不平衡后把均衡周期重置为最小值。 */
		sd->balance_interval = sd->min_interval;
	}

	goto out;

out_balanced:
	/* 已达到本层平衡；只有并非全亲和性固定时才可清父层 imbalance。 */
	/*
	 * We reach balance although we may have faced some affinity
	 * constraints. Clear the imbalance flag only if other tasks got
	 * a chance to move and fix the imbalance.
	 */
	if (sd_parent && !(env.flags & LBF_ALL_PINNED)) {
		int *group_imbalance = &sd_parent->groups->sgc->imbalance;

		if (*group_imbalance)
			*group_imbalance = 0;
	}

out_all_pinned:
	/* 本层无可迁任务也视为本次结束，但保留父层继续尝试的机会。 */
	/*
	 * We reach balance because all tasks are pinned at this level so
	 * we can't migrate them. Let the imbalance flag set so parent level
	 * can try to migrate them.
	 */
	schedstat_inc(sd->lb_balanced[idle]);

	sd->nr_balance_failed = 0;

out_one_pinned:
	/* 单个/全部固定的失败路径不报告迁移成功，并按周期场景指数退避。 */
	ld_moved = 0;

	/*
	 * sched_balance_newidle() disregards balance intervals, so we could
	 * repeatedly reach this code, which would lead to balance_interval
	 * skyrocketing in a short amount of time. Skip the balance_interval
	 * increase logic to avoid that.
	 *
	 * Similarly misfit migration which is not necessarily an indication of
	 * the system being busy and requires lb to backoff to let it settle
	 * down.
	 */
	if (env.idle == CPU_NEWLY_IDLE ||
	    env.migration_type == migrate_misfit)
		goto out;

	/* tune up the balancing interval */
/* 平衡或不可迁移时增大均衡间隔以退避。 */
	if ((env.flags & LBF_ALL_PINNED &&
	     sd->balance_interval < MAX_PINNED_INTERVAL) ||
	    sd->balance_interval < sd->max_interval)
		sd->balance_interval *= 2;
out:
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (need_unlock)
		atomic_set_release(&sched_balance_running, 0);

	return ld_moved;
}

static inline unsigned long
/*
 * 把 domain 的毫秒均衡周期换算为当前忙闲状态下的 jiffies 周期。
 * 输入 sd 为目标层、cpu_busy 表示忙 CPU；返回值被限制在 1 到全局上限。
 */
get_sd_balance_interval(struct sched_domain *sd, int cpu_busy)
{
	unsigned long interval = sd->balance_interval;

	if (cpu_busy)
		interval *= sd->busy_factor;

	/* scale ms to jiffies */
/* 把毫秒均衡周期换算为 jiffies。 */
	interval = msecs_to_jiffies(interval);

	/*
	 * Reduce likelihood of busy balancing at higher domains racing with
	 * balancing at lower domains by preventing their balancing periods
	 * from being multiples of each other.
	 */
/* 本段说明候选 CPU、亲和性固定、主动迁移与退避计数的处理顺序。 */
	if (cpu_busy)
		interval -= 1;

	interval = clamp(interval, 1UL, max_load_balance_interval);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return interval;
}

static inline void
/* 根据该 domain 下一截止点收紧调用者维护的最早均衡时间。 */
update_next_balance(struct sched_domain *sd, unsigned long *next_balance)
{
	unsigned long interval, next;

	/* used by idle balance, so cpu_busy = 0 */
/* idle 均衡按 cpu_busy=0 计算周期。 */
	interval = get_sd_balance_interval(sd, 0);
	next = sd->last_balance + interval;

	if (time_after(*next_balance, next))
		*next_balance = next;
}

/*
 * active_load_balance_cpu_stop is run by the CPU stopper. It pushes
 * running tasks off the busiest CPU onto idle CPUs. It requires at
 * least 1 task to be running on each physical CPU where possible, and
 * avoids physical / logical imbalances.
 */
/*
 * stopper 在源 CPU 上锁后强制迁走一项运行任务到预选空闲 CPU。
 * data 是源 rq；始终返回 0，任务通过 detach/attach 转移，条件失效则只清
 * active_balance。源 rq 锁保护选择与摘除，attach 在解锁后取得目标 rq 锁。
 */
static int active_load_balance_cpu_stop(void *data)
{
	struct rq *busiest_rq = data;
	int busiest_cpu = cpu_of(busiest_rq);
	int target_cpu = busiest_rq->push_cpu;
	struct rq *target_rq = cpu_rq(target_cpu);
	struct sched_domain *sd;
	struct task_struct *p = NULL;
	struct rq_flags rf;

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	rq_lock_irq(busiest_rq, &rf);
	/*
	 * Between queueing the stop-work and running it is a hole in which
	 * CPUs can become inactive. We should not move tasks from or to
	 * inactive CPUs.
	 */
	if (!cpu_active(busiest_cpu) || !cpu_active(target_cpu))
		goto out_unlock;

	/* Make sure the requested CPU hasn't gone down in the meantime: */
/* 执行 stopper 前复验请求 CPU 尚未下线。 */
	if (unlikely(busiest_cpu != smp_processor_id() ||
		     !busiest_rq->active_balance))
		goto out_unlock;

	/* Is there any task to move? */
/* 确认源 rq 仍有多于一个任务可供迁移。 */
	if (busiest_rq->nr_running <= 1)
		goto out_unlock;

	/*
	 * This condition is "impossible", if it occurs
	 * we need to fix it. Originally reported by
	 * Bjorn Helgaas on a 128-CPU setup.
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	WARN_ON_ONCE(busiest_rq == target_rq);

	/* Search for an sd spanning us and the target CPU. */
	rcu_read_lock();
	/* 遍历本层候选并逐项复验，循环结果汇入后续选择。 */
	for_each_domain(target_cpu, sd) {
		if (cpumask_test_cpu(busiest_cpu, sched_domain_span(sd)))
			break;
	}

	if (likely(sd)) {
		struct lb_env env = {
			.sd		= sd,
			.dst_cpu	= target_cpu,
			.dst_rq		= target_rq,
			.src_cpu	= busiest_rq->cpu,
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			.src_rq		= busiest_rq,
			.idle		= CPU_IDLE,
			.flags		= LBF_ACTIVE_LB,
		};

		schedstat_inc(sd->alb_count);
		update_rq_clock(busiest_rq);

		p = detach_one_task(&env);
		if (p) {
			schedstat_inc(sd->alb_pushed);
			/* Active balancing done, reset the failure counter. */
			sd->nr_balance_failed = 0;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		} else {
			schedstat_inc(sd->alb_failed);
		}
	}
	rcu_read_unlock();
out_unlock:
	busiest_rq->active_balance = 0;
	rq_unlock(busiest_rq, &rf);

	if (p)
		attach_one_task(target_rq, p);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	local_irq_enable();

	return 0;
}

/*
 * Scale the max sched_balance_rq interval with the number of CPUs in the system.
 * This trades load-balance latency on larger machines for less cross talk.
 */
void update_max_interval(void)
{
	/* CPU 越多，允许周期均衡间隔越大，以交换更少的跨 CPU 干扰。 */
	max_load_balance_interval = HZ*num_online_cpus()/10;
}

/* 累计新空闲均衡命中率，并每 1024 次折半历史样本防止旧数据支配。 */
static inline void update_newidle_stats(struct sched_domain *sd, unsigned int success)
{
	sd->newidle_call++;
	sd->newidle_success += success;

	if (sd->newidle_call >= 1024) {
		u64 now = sched_clock();
		s64 delta = now - sd->newidle_stamp;
		sd->newidle_stamp = now;
		int ratio = 0;

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (delta < 0)
			delta = 0;

		if (sched_feat(NI_RATE)) {
			/*
			 * ratio  delta   freq
			 *
			 * 1024 -  4  s -  128 Hz
			 *  512 -  2  s -  256 Hz
			 *  256 -  1  s -  512 Hz
			 *  128 - .5  s - 1024 Hz
			 *   64 - .25 s - 2048 Hz
			 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
			ratio = delta >> 22;
		}

		ratio += sd->newidle_success;

		sd->newidle_ratio = min(1024, ratio);
		sd->newidle_call /= 2;
		sd->newidle_success /= 2;
	}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}

static inline bool
/*
 * 更新新空闲均衡的最大成本并按秒衰减；cost 非零时也更新成功率。
 * 返回 true 表示本次发生衰减，调用者据此重算 rq 级成本上界。
 */
update_newidle_cost(struct sched_domain *sd, u64 cost, unsigned int success)
{
	unsigned long next_decay = sd->last_decay_max_lb_cost + HZ;
	unsigned long now = jiffies;

	if (cost)
		update_newidle_stats(sd, success);

	if (cost > sd->max_newidle_lb_cost) {
		/*
		 * Track max cost of a domain to make sure to not delay the
		 * next wakeup on the CPU.
		 */
		sd->max_newidle_lb_cost = cost;
		sd->last_decay_max_lb_cost = now;

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	} else if (time_after(now, next_decay)) {
		/*
		 * Decay the newidle max times by ~1% per second to ensure that
		 * it is not outdated and the current max cost is actually
		 * shorter.
		 */
		sd->max_newidle_lb_cost = (sd->max_newidle_lb_cost * 253) / 256;
		sd->last_decay_max_lb_cost = now;
		return true;
	}

	return false;
}

/*
 * It checks each scheduling domain to see if it is due to be balanced,
 * and initiates a balancing operation if so.
 *
 * Balancing parameters are set up in init_sched_domains.
 */
/*
 * 按当前 rq 所在 CPU 的 domain 链执行到期的周期均衡。
 * idle 描述进入时忙闲状态；无返回值，副作用是迁移任务、更新各层统计及
 * rq->next_balance。domain 链在 RCU 读侧保护下遍历。
 */
static void sched_balance_domains(struct rq *rq, enum cpu_idle_type idle)
{
	int continue_balancing = 1;
	int cpu = rq->cpu;
	int busy = idle != CPU_IDLE && !sched_idle_rq(rq);
	unsigned long interval;
	struct sched_domain *sd;
	/* Earliest time when we have to do rebalance again */
	unsigned long next_balance = jiffies + 60*HZ;
	int update_next_balance = 0;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	int need_decay = 0;
	u64 max_cost = 0;

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		/*
		 * Decay the newidle max times here because this is a regular
		 * visit to all the domains.
		 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
		need_decay = update_newidle_cost(sd, 0, 0);
		max_cost += sd->max_newidle_lb_cost;

		/*
		 * Stop the load balance at this level. There is another
		 * CPU in our sched group which is doing load balancing more
		 * actively.
		 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
		if (!continue_balancing) {
			if (need_decay)
				continue;
			break;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		}

		interval = get_sd_balance_interval(sd, busy);
		if (time_after_eq(jiffies, sd->last_balance + interval)) {
			if (sched_balance_rq(cpu, rq, sd, idle, &continue_balancing)) {
				/*
				 * The LBF_DST_PINNED logic could have changed
				 * env->dst_cpu, so we can't know our idle
				 * state even if we migrated tasks. Update it.
				 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
				idle = idle_cpu(cpu);
				busy = !idle && !sched_idle_rq(rq);
			}
			sd->last_balance = jiffies;
			interval = get_sd_balance_interval(sd, busy);
		}
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (time_after(next_balance, sd->last_balance + interval)) {
			next_balance = sd->last_balance + interval;
			update_next_balance = 1;
		}
	}
	if (need_decay) {
		/*
		 * Ensure the rq-wide value also decays but keep it at a
		 * reasonable floor to avoid funnies with rq->avg_idle.
		 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
		rq->max_idle_balance_cost =
			max((u64)sysctl_sched_migration_cost, max_cost);
	}
	rcu_read_unlock();

	/*
	 * next_balance will be updated only when there is a need.
	 * When the cpu is attached to null domain for ex, it will not be
	 * updated.
	 */
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (likely(update_next_balance))
		rq->next_balance = next_balance;

}

static inline int on_null_domain(struct rq *rq)
{
	/* 热插拔/隔离期间 rq 可能暂时没有调度域，此时不能做 domain 均衡。 */
	return unlikely(!rcu_dereference_sched(rq->sd));
}

#ifdef CONFIG_NO_HZ_COMMON
/*
 * NOHZ idle load balancing (ILB) details:
 *
 * - When one of the busy CPUs notices that there may be an idle rebalancing
 *   needed, they will kick the idle load balancer, which then does idle
 *   load balancing for all the idle CPUs.
 */
/* 从空闲且允许 kernel-noise housekeeping 的 CPU 中选择一个 ILB，失败返回 nr_cpu_ids。 */
static inline int find_new_ilb(void)
{
	int this_cpu = smp_processor_id();
	const struct cpumask *hk_mask;
	int ilb_cpu;

	hk_mask = housekeeping_cpumask(HK_TYPE_KERNEL_NOISE);

	for_each_cpu_and(ilb_cpu, nohz.idle_cpus_mask, hk_mask) {
		if (ilb_cpu == this_cpu)
			continue;

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (idle_cpu(ilb_cpu))
			return ilb_cpu;
	}

	return -1;
}

/*
 * Kick a CPU to do the NOHZ balancing, if it is time for it, via a cross-CPU
 * SMP function call (IPI).
 *
 * We pick the first idle CPU in the HK_TYPE_KERNEL_NOISE housekeeping set
 * (if there is one).
 */
/*
 * 将 flags 原子合并到选中的 idle load balancer，并在首次取得 CSD 所有权时发 IPI。
 * 找不到空闲 housekeeping CPU 或同类工作已挂起时静默返回。
 */
static void kick_ilb(unsigned int flags)
{
	int ilb_cpu;

	/*
	 * Increase nohz.next_balance only when if full ilb is triggered but
	 * not if we only update stats.
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	if (flags & NOHZ_BALANCE_KICK)
		nohz.next_balance = jiffies+1;

	ilb_cpu = find_new_ilb();
	if (ilb_cpu < 0)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;

	/*
	 * Don't bother if no new NOHZ balance work items for ilb_cpu,
	 * i.e. all bits in flags are already set in ilb_cpu.
	 */
	if ((atomic_read(nohz_flags(ilb_cpu)) & flags) == flags)
		return;

	/*
	 * Access to rq::nohz_csd is serialized by NOHZ_KICK_MASK; he who sets
	 * the first flag owns it; cleared by nohz_csd_func().
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	flags = atomic_fetch_or(flags, nohz_flags(ilb_cpu));
	if (flags & NOHZ_KICK_MASK)
		return;

	/*
	 * This way we generate an IPI on the target CPU which
	 * is idle, and the softirq performing NOHZ idle load balancing
	 * will be run before returning from the IPI.
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	smp_call_function_single_async(ilb_cpu, &cpu_rq(ilb_cpu)->nohz_csd);
}

/*
 * Current decision point for kicking the idle load balancer in the presence
 * of idle CPUs in the system.
 */
/*
 * 忙 rq 根据过载、容量不对称、LLC 与 blocked load 状态决定 NOHZ 工作类型。
 * 无返回值；只聚合 flags，最后最多发送一次 ILB kick，domain 指针按 RCU 读取。
 */
static void nohz_balancer_kick(struct rq *rq)
{
	unsigned long now = jiffies;
	struct sched_domain_shared *sds;
	struct sched_domain *sd;
	int nr_busy, i, cpu = rq->cpu;
	unsigned int flags = 0;

	if (unlikely(rq->idle_balance))
		return;

	/*
	 * We may be recently in ticked or tickless idle mode. At the first
	 * busy tick after returning from idle, we will update the busy stats.
	 */
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	nohz_balance_exit_idle(rq);

	if (READ_ONCE(nohz.has_blocked_load) &&
	    time_after(now, READ_ONCE(nohz.next_blocked)))
		flags = NOHZ_STATS_KICK;

	/*
	 * Most of the time system is not 100% busy. i.e nohz.nr_cpus > 0
	 * Skip the read if time is not due.
	 *
	 * If none are in tickless mode, there maybe a narrow window
	 * (28 jiffies, HZ=1000) where flags maybe set and kick_ilb called.
	 * But idle load balancing is not done as find_new_ilb fails.
	 * That's very rare. So read nohz.nr_cpus only if time is due.
	 */
	if (time_before(now, nohz.next_balance))
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		goto out;

	/*
	 * None are in tickless mode and hence no need for NOHZ idle load
	 * balancing
	 */
	if (unlikely(cpumask_empty(nohz.idle_cpus_mask)))
		return;

	if (rq->nr_running >= 2) {
		flags = NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;
		goto out;
	}

	sd = rcu_dereference_all(rq->sd);
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (sd) {
		/*
		 * If there's a runnable CFS task and the current CPU has reduced
		 * capacity, kick the ILB to see if there's a better CPU to run on:
		 */
		if (rq->cfs.h_nr_runnable >= 1 && check_cpu_capacity(rq, sd)) {
			flags |= NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;
			goto out;
		}
	}

	sd = rcu_dereference_all(per_cpu(sd_asym_packing, cpu));
	if (sd) {
		/*
		 * When ASYM_PACKING; see if there's a more preferred CPU
		 * currently idle; in which case, kick the ILB to move tasks
		 * around.
		 *
		 * When balancing between cores, all the SMT siblings of the
		 * preferred CPU must be idle.
		 */
		for_each_cpu_and(i, sched_domain_span(sd), nohz.idle_cpus_mask) {
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
			if (sched_asym(sd, i, cpu)) {
				flags |= NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;
				goto out;
			}
		}
	}

	sd = rcu_dereference_all(per_cpu(sd_asym_cpucapacity, cpu));
	if (sd) {
		/*
		 * When ASYM_CPUCAPACITY; see if there's a higher capacity CPU
		 * to run the misfit task on.
		 */
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (check_misfit_status(rq))
			flags |= NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;

		/*
		 * For asymmetric systems, we do not want to nicely balance
		 * cache use, instead we want to embrace asymmetry and only
		 * ensure tasks have enough CPU capacity.
		 *
		 * Skip the LLC logic because it's not relevant in that case.
		 */
		goto out;
	}

	sds = rcu_dereference_all(per_cpu(sd_balance_shared, cpu));
	if (sds) {
		/*
		 * If there is an imbalance between LLC domains (IOW we could
		 * increase the overall cache utilization), we need a less-loaded LLC
		 * domain to pull some load from. Likewise, we may need to spread
		 * load within the current LLC domain (e.g. packed SMT cores but
		 * other CPUs are idle). We can't really know from here how busy
		 * the others are - so just get a NOHZ balance going if it looks
		 * like this LLC domain has tasks we could move.
		 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
		nr_busy = atomic_read(&sds->nr_busy_cpus);
		if (nr_busy > 1)
			flags |= NOHZ_STATS_KICK | NOHZ_BALANCE_KICK;
	}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
out:
	if (READ_ONCE(nohz.needs_update))
		flags |= NOHZ_NEXT_KICK;

	if (flags)
		kick_ilb(flags);
}

/* 将 CPU 从 LLC 共享域的 NOHZ-idle 记账切回 busy；重复调用无副作用。 */
static void set_cpu_sd_state_busy(int cpu)
{
	struct sched_domain *sd;
	sd = rcu_dereference_all(per_cpu(sd_llc, cpu));

	/*
	 * sd->nohz_idle only pairs with nr_busy_cpus on sd->shared; if this
	 * domain has no shared object there is nothing to clear or account.
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	if (!sd || !sd->shared || !sd->nohz_idle)
		return;
	sd->nohz_idle = 0;

	atomic_inc(&sd->shared->nr_busy_cpus);
}

/* 当前 rq 恢复 tick/busy 时退出 NOHZ mask，并恢复共享域 busy CPU 计数。 */
void nohz_balance_exit_idle(struct rq *rq)
{
	WARN_ON_ONCE(rq != this_rq());

	if (likely(!rq->nohz_tick_stopped))
		return;

	rq->nohz_tick_stopped = 0;
	cpumask_clear_cpu(rq->cpu, nohz.idle_cpus_mask);

	set_cpu_sd_state_busy(rq->cpu);
}

/* 将 CPU 记为 LLC 共享域的 NOHZ-idle，并把 nr_busy_cpus 减一。 */
static void set_cpu_sd_state_idle(int cpu)
{
	struct sched_domain *sd;
	sd = rcu_dereference_all(per_cpu(sd_llc, cpu));

	/* See set_cpu_sd_state_busy(): nohz_idle is only used with sd->shared. */
/* nohz_idle 只与共享域的 nr_busy_cpus 计数配对。 */
	if (!sd || !sd->shared || sd->nohz_idle)
		return;
	sd->nohz_idle = 1;

	atomic_dec(&sd->shared->nr_busy_cpus);
}

/*
 * This routine will record that the CPU is going idle with tick stopped.
 * This info will be used in performing idle load balancing in the future.
 */
/*
 * 当前 CPU 停 tick 前登记到 NOHZ idle 集合，并发布 blocked-load 更新请求。
 * cpu 必须是本 CPU；原子 mask 后的屏障保证代理均衡不会同时漏看 mask 和标志。
 */
void nohz_balance_enter_idle(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	WARN_ON_ONCE(cpu != smp_processor_id());

	/* If this CPU is going down, then nothing needs to be done: */
/* CPU 正在下线时无需登记 NOHZ idle。 */
	if (!cpu_active(cpu))
		return;

	/*
	 * Can be set safely without rq->lock held
	 * If a clear happens, it will have evaluated last additions because
	 * rq->lock is held during the check and the clear
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	rq->has_blocked_load = 1;

	/*
	 * The tick is still stopped but load could have been added in the
	 * meantime. We set the nohz.has_blocked_load flag to trig a check of the
	 * *_avg. The CPU is already part of nohz.idle_cpus_mask so the clear
	 * of nohz.has_blocked_load can only happen after checking the new load
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	if (rq->nohz_tick_stopped)
		goto out;

	/* If we're a completely isolated CPU, we don't play: */
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (on_null_domain(rq))
		return;

	rq->nohz_tick_stopped = 1;

	cpumask_set_cpu(cpu, nohz.idle_cpus_mask);

	/*
	 * Ensures that if nohz_idle_balance() fails to observe our
	 * @idle_cpus_mask store, it must observe the @has_blocked_load
	 * and @needs_update stores.
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	smp_mb__after_atomic();

	set_cpu_sd_state_idle(cpu);

	WRITE_ONCE(nohz.needs_update, 1);
out:
	/*
	 * Each time a cpu enter idle, we assume that it has blocked load and
	 * enable the periodic update of the load of idle CPUs
	 */
	WRITE_ONCE(nohz.has_blocked_load, 1);
}

/* 若 rq 仍是 NOHZ idle 且更新到期，刷新其 blocked PELT；返回是否仍有 blocked load。 */
static bool update_nohz_stats(struct rq *rq)
{
	unsigned int cpu = rq->cpu;

	if (!rq->has_blocked_load)
		return false;

	if (!cpumask_test_cpu(cpu, nohz.idle_cpus_mask))
		return false;

	if (!time_after(jiffies, READ_ONCE(rq->last_blocked_load_update_tick)))
		return true;

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	sched_balance_update_blocked_averages(cpu);

	return rq->has_blocked_load;
}

/*
 * Internal function that runs load balance for all idle CPUs. The load balance
 * can be a simple update of blocked load or a complete load balance with
 * tasks movement depending of flags.
 */
/*
 * 当前 idle rq 代理所有停 tick CPU 更新 blocked PELT，flags 可要求完整 domain 均衡。
 * 无返回值；遍历中本 CPU 获得任务会中止并重新发布未完成标志，屏障防止漏记新 idle CPU。
 */
static void _nohz_idle_balance(struct rq *this_rq, unsigned int flags)
{
	/* Earliest time when we have to do rebalance again */
	unsigned long now = jiffies;
	unsigned long next_balance = now + 60*HZ;
	bool has_blocked_load = false;
	int update_next_balance = 0;
	int this_cpu = this_rq->cpu;
	int balance_cpu;
	struct rq *rq;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	WARN_ON_ONCE((flags & NOHZ_KICK_MASK) == NOHZ_BALANCE_KICK);

	/*
	 * We assume there will be no idle load after this update and clear
	 * the has_blocked_load flag. If a cpu enters idle in the mean time, it will
	 * set the has_blocked_load flag and trigger another update of idle load.
	 * Because a cpu that becomes idle, is added to idle_cpus_mask before
	 * setting the flag, we are sure to not clear the state and not
	 * check the load of an idle cpu.
	 *
	 * Same applies to idle_cpus_mask vs needs_update.
	 */
	if (flags & NOHZ_STATS_KICK)
		WRITE_ONCE(nohz.has_blocked_load, 0);
	if (flags & NOHZ_NEXT_KICK)
		WRITE_ONCE(nohz.needs_update, 0);

	/*
	 * Ensures that if we miss the CPU, we must see the has_blocked_load
	 * store from nohz_balance_enter_idle().
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	smp_mb();

	/*
	 * Start with the next CPU after this_cpu so we will end with this_cpu and let a
	 * chance for other idle cpu to pull load.
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	for_each_cpu_wrap(balance_cpu,  nohz.idle_cpus_mask, this_cpu+1) {
		if (!idle_cpu(balance_cpu))
			continue;

		/*
		 * If this CPU gets work to do, stop the load balancing
		 * work being done for other CPUs. Next load
		 * balancing owner will pick it up.
		 */
		if (!idle_cpu(this_cpu) && need_resched()) {
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
			if (flags & NOHZ_STATS_KICK)
				has_blocked_load = true;
			if (flags & NOHZ_NEXT_KICK)
				WRITE_ONCE(nohz.needs_update, 1);
			goto abort;
		}

		rq = cpu_rq(balance_cpu);

		if (flags & NOHZ_STATS_KICK)
			has_blocked_load |= update_nohz_stats(rq);

		/*
		 * If time for next balance is due,
		 * do the balance.
		 */
		if (time_after_eq(jiffies, rq->next_balance)) {
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			struct rq_flags rf;

			rq_lock_irqsave(rq, &rf);
			update_rq_clock(rq);
			rq_unlock_irqrestore(rq, &rf);

			if (flags & NOHZ_BALANCE_KICK)
				sched_balance_domains(rq, CPU_IDLE);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		}

		if (time_after(next_balance, rq->next_balance)) {
			next_balance = rq->next_balance;
			update_next_balance = 1;
		}
	}

	/*
	 * next_balance will be updated only when there is a need.
	 * When the CPU is attached to null domain for ex, it will not be
	 * updated.
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	if (likely(update_next_balance))
		nohz.next_balance = next_balance;

	if (flags & NOHZ_STATS_KICK)
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
		WRITE_ONCE(nohz.next_blocked,
			   now + msecs_to_jiffies(LOAD_AVG_PERIOD));

abort:
	/* There is still blocked load, enable periodic update */
	if (has_blocked_load)
		WRITE_ONCE(nohz.has_blocked_load, 1);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
}

/*
 * In CONFIG_NO_HZ_COMMON case, the idle balance kickee will do the
 * rebalancing for all the CPUs for whom scheduler ticks are stopped.
 */
/* 消费本 rq 的 NOHZ kick；仅仍为 CPU_IDLE 时执行，返回是否完成了代理均衡。 */
static bool nohz_idle_balance(struct rq *this_rq, enum cpu_idle_type idle)
{
	unsigned int flags = this_rq->nohz_idle_balance;

	if (!flags)
		return false;

	this_rq->nohz_idle_balance = 0;

	if (idle != CPU_IDLE)
		return false;

	_nohz_idle_balance(this_rq, flags);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return true;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
}

/*
 * Check if we need to directly run the ILB for updating blocked load before
 * entering idle state. Here we run ILB directly without issuing IPIs.
 *
 * Note that when this function is called, the tick may not yet be stopped on
 * this CPU yet. nohz.idle_cpus_mask is updated only when tick is stopped and
 * cleared on the next busy tick. In other words, nohz.idle_cpus_mask updates
 * don't align with CPUs enter/exit idle to avoid bottlenecks due to high idle
 * entry/exit rate (usec). So it is possible that _nohz_idle_balance() is
 * called from this function on (this) CPU that's not yet in the mask. That's
 * OK because the goal of nohz_run_idle_balance() is to run ILB only for
 * updating the blocked load of already idle CPUs without waking up one of
 * those idle CPUs and outside the preempt disable / IRQ off phase of the local
 * cpu about to enter idle, because it can take a long time.
 */
/* CPU 真正入 idle 前直接消费 NEWILB 请求，仅更新其他 idle CPU 的 blocked load。 */
void nohz_run_idle_balance(int cpu)
{
	unsigned int flags;

	flags = atomic_fetch_andnot(NOHZ_NEWILB_KICK, nohz_flags(cpu));

	/*
	 * Update the blocked load only if no SCHED_SOFTIRQ is about to happen
	 * (i.e. NOHZ_STATS_KICK set) and will do the same.
	 */
/* 本段说明 NOHZ idle balancer 的原子发布、blocked load 刷新和中止恢复规则。 */
	if ((flags == NOHZ_NEWILB_KICK) && !need_resched())
		_nohz_idle_balance(cpu_rq(cpu), NOHZ_STATS_KICK);
}

/* 新空闲路径成本足够且 blocked 更新到期时，原子发布延后执行的 NEWILB 请求。 */
static void nohz_newidle_balance(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu;

	/* Will wake up very soon. No time for doing anything else*/
/* 预计很快唤醒时，不支付新空闲均衡成本。 */
	if (this_rq->avg_idle < sysctl_sched_migration_cost)
		return;

	/* Don't need to update blocked load of idle CPUs*/
	if (!READ_ONCE(nohz.has_blocked_load) ||
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
	    time_before(jiffies, READ_ONCE(nohz.next_blocked)))
		return;

	/*
	 * Set the need to trigger ILB in order to update blocked load
	 * before entering idle state.
	 */
	atomic_or(NOHZ_NEWILB_KICK, nohz_flags(this_cpu));
}

#else /* !CONFIG_NO_HZ_COMMON: */
/* 非 NOHZ 配置下无需选择或唤醒 idle balancer。 */
static inline void nohz_balancer_kick(struct rq *rq) { }

static inline bool nohz_idle_balance(struct rq *this_rq, enum cpu_idle_type idle)
{
	/* 非 NOHZ 配置永远没有代理均衡工作。 */
	return false;
}

/* 非 NOHZ 配置无需发布新空闲 blocked-load 更新。 */
static inline void nohz_newidle_balance(struct rq *this_rq) { }
#endif /* !CONFIG_NO_HZ_COMMON */

/*
 * sched_balance_newidle is called by schedule() if this_cpu is about to become
 * idle. Attempts to pull tasks from other CPUs.
 *
 * Returns:
 *   < 0 - we released the lock and there are !fair tasks present
 *     0 - failed, no new tasks
 *   > 0 - success, new (fair) tasks present
 */
/*
 * schedule() 即将让本 rq idle 时，按 avg_idle 成本预算从各 domain 拉取公平任务。
 * 输入 rq 及其锁状态 rf；返回负值表示放锁期间更高调度类变化、0 表示未拉到、
 * 正值表示已有公平任务。函数临时 unpin/释放 rq 锁，返回前恢复原锁语义。
 */
static int sched_balance_newidle(struct rq *this_rq, struct rq_flags *rf)
	__must_hold(__rq_lockp(this_rq))
{
	unsigned long next_balance = jiffies + HZ;
	int this_cpu = this_rq->cpu;
	int continue_balancing = 1;
	u64 t0, t1, curr_cost = 0;
	struct sched_domain *sd;
	int pulled_task = 0;

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	update_misfit_status(NULL, this_rq);

	/*
	 * There is a task waiting to run. No need to search for one.
	 * Return 0; the task will be enqueued when switching to idle.
	 */
	if (this_rq->ttwu_pending)
		return 0;

	/*
	 * We must set idle_stamp _before_ calling sched_balance_rq()
	 * for CPU_NEWLY_IDLE, such that we measure the this duration
	 * as idle time.
	 */
/* 本段说明新空闲均衡的放锁、成本预算、返回值及截止时间更新。 */
	this_rq->idle_stamp = rq_clock(this_rq);

	/*
	 * Do not pull tasks towards !active CPUs...
	 */
/* 本段说明新空闲均衡的放锁、成本预算、返回值及截止时间更新。 */
	if (!cpu_active(this_cpu))
		return 0;

	/*
	 * This is OK, because current is on_cpu, which avoids it being picked
	 * for load-balance and preemption/IRQs are still disabled avoiding
	 * further scheduler activity on it and we're being very careful to
	 * re-start the picking loop.
	 */
/* 本段说明新空闲均衡的放锁、成本预算、返回值及截止时间更新。 */
	rq_unpin_lock(this_rq, rf);

	sd = rcu_dereference_sched_domain(this_rq->sd);
	if (!sd)
		goto out;

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (!get_rd_overloaded(this_rq->rd) ||
	    this_rq->avg_idle < sd->max_newidle_lb_cost) {

		update_next_balance(sd, &next_balance);
		goto out;
	}

	/*
	 * Include sched_balance_update_blocked_averages() in the cost
	 * calculation because it can be quite costly -- this ensures we skip
	 * it when avg_idle gets to be very low.
	 */
/* 本段说明新空闲均衡的放锁、成本预算、返回值及截止时间更新。 */
	t0 = sched_clock_cpu(this_cpu);
	__sched_balance_update_blocked_averages(this_rq);

	rq_modified_begin(this_rq, &fair_sched_class);
	raw_spin_rq_unlock(this_rq);

	for_each_domain(this_cpu, sd) {
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		u64 domain_cost;

		update_next_balance(sd, &next_balance);

		if (this_rq->avg_idle < curr_cost + sd->max_newidle_lb_cost)
			break;

		if (sd->flags & SD_BALANCE_NEWIDLE) {
			unsigned int weight = 1;

			if (sched_feat(NI_RANDOM) && sd->newidle_ratio < 1024) {
				/*
				 * Throw a 1k sided dice; and only run
				 * newidle_balance according to the success
				 * rate.
				 */
				u32 d1k = sched_rng() % 1024;
				weight = 1 + sd->newidle_ratio;
				if (d1k > weight) {
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
					update_newidle_stats(sd, 0);
					continue;
				}
				weight = (1024 + weight/2) / weight;
			}

			pulled_task = sched_balance_rq(this_cpu, this_rq,
						   sd, CPU_NEWLY_IDLE,
						   &continue_balancing);

			t1 = sched_clock_cpu(this_cpu);
			domain_cost = t1 - t0;
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			curr_cost += domain_cost;
			t0 = t1;

			/*
			 * Track max cost of a domain to make sure to not delay the
			 * next wakeup on the CPU.
			 */
			update_newidle_cost(sd, domain_cost, weight * !!pulled_task);
		}

		/*
		 * Stop searching for tasks to pull if there are
		 * now runnable tasks on this rq.
		 */
/* 本段说明新空闲均衡的放锁、成本预算、返回值及截止时间更新。 */
		if (pulled_task || !continue_balancing)
			break;
	}

	raw_spin_rq_lock(this_rq);

	if (curr_cost > this_rq->max_idle_balance_cost)
		this_rq->max_idle_balance_cost = curr_cost;

	/*
	 * While browsing the domains, we released the rq lock, a task could
	 * have been enqueued in the meantime. Since we're not going idle,
	 * pretend we pulled a task.
	 */
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (this_rq->cfs.h_nr_queued && !pulled_task)
		pulled_task = 1;

	/* If a higher prio class was modified, restart the pick */
	if (rq_modified_above(this_rq, &fair_sched_class))
		pulled_task = -1;

out:
	/* Move the next balance forward */
/* 用各层最早截止时间收紧 rq 的 next_balance。 */
	if (time_after(this_rq->next_balance, next_balance))
		this_rq->next_balance = next_balance;

	if (pulled_task)
		this_rq->idle_stamp = 0;
	else
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		nohz_newidle_balance(this_rq);

	rq_repin_lock(this_rq, rf);

	return pulled_task;
}

/*
 * This softirq handler is triggered via SCHED_SOFTIRQ from two places:
 *
 * - directly from the local sched_tick() for periodic load balancing
 *
 * - indirectly from a remote sched_tick() for NOHZ idle balancing
 *   through the SMP cross-call nohz_csd_func()
 */
/* SCHED_SOFTIRQ 入口：先完成 NOHZ 代理工作，否则更新本 CPU PELT 并做周期均衡。 */
static __latent_entropy void sched_balance_softirq(void)
{
	struct rq *this_rq = this_rq();
	enum cpu_idle_type idle = this_rq->idle_balance;
	/*
	 * If this CPU has a pending NOHZ_BALANCE_KICK, then do the
	 * balancing on behalf of the other idle CPUs whose ticks are
	 * stopped. Do nohz_idle_balance *before* sched_balance_domains to
	 * give the idle CPUs a chance to load balance. Else we may
	 * load balance only within the local sched_domain hierarchy
	 * and abort nohz_idle_balance altogether if we pull some load.
	 */
/* 本段说明新空闲均衡的放锁、成本预算、返回值及截止时间更新。 */
	if (nohz_idle_balance(this_rq, idle))
		return;

	/* normal load balance */
/* 没有 NOHZ 代理工作时执行普通周期负载均衡。 */
	sched_balance_update_blocked_averages(this_rq->cpu);
	sched_balance_domains(this_rq, idle);
}

/*
 * Trigger the SCHED_SOFTIRQ if it is time to do periodic load balancing.
 */
/* tick 侧在 rq 到达 next_balance 时触发软中断，并评估是否需要唤醒 NOHZ ILB。 */
void sched_balance_trigger(struct rq *rq)
{
	/*
	 * Don't need to rebalance while attached to NULL domain or
	 * runqueue CPU is not active
	 */
	if (unlikely(on_null_domain(rq) || !cpu_active(cpu_of(rq))))
		return;

	if (time_after_eq(jiffies, rq->next_balance))
		raise_softirq(SCHED_SOFTIRQ);

	nohz_balancer_kick(rq);
}

/* 公平类 CPU 上线回调：重算 tunable 并恢复该 rq 的带宽运行时。 */
static void rq_online_fair(struct rq *rq)
{
	update_sysctl();

	update_runtime_enabled(rq);
}

/* 公平类 CPU 下线回调：解除节流并移除离线 rq 对任务组份额的贡献。 */
static void rq_offline_fair(struct rq *rq)
{
	update_sysctl();

	/* Ensure any throttled groups are reachable by pick_next_task */
/* CPU 下线前解除组节流，使其中任务仍可被选择。 */
	unthrottle_offline_cfs_rqs(rq);

	/* Ensure that we remove rq contribution to group share: */
/* CPU 下线时移除该 rq 对任务组份额的贡献。 */
	clear_tg_offline_cfs_rqs(rq);
}

#ifdef CONFIG_SCHED_CORE
static inline bool
/* 判断实体在本次运行中是否已消耗超过按 min_nr_tasks 缩放后的 slice。 */
__entity_slice_used(struct sched_entity *se, int min_nr_tasks)
{
	u64 rtime = se->sum_exec_runtime - se->prev_sum_exec_runtime;
	u64 slice = se->slice;

	return (rtime * min_nr_tasks > slice);
}

#define MIN_NR_TASKS_DURING_FORCEIDLE	2
/* core scheduling 强制 sibling idle 时，用保守任务数判断是否应让当前任务重调度。 */
static inline void task_tick_core(struct rq *rq, struct task_struct *curr)
{
	if (!sched_core_enabled(rq))
		return;

	/*
	 * If runqueue has only one task which used up its slice and
	 * if the sibling is forced idle, then trigger schedule to
	 * give forced idle task a chance.
	 *
	 * sched_slice() considers only this active rq and it gets the
	 * whole slice. But during force idle, we have siblings acting
	 * like a single runqueue and hence we need to consider runnable
	 * tasks on this CPU and the forced idle CPU. Ideally, we should
	 * go through the forced idle rq, but that would be a perf hit.
	 * We can assume that the forced idle CPU has at least
	 * MIN_NR_TASKS_DURING_FORCEIDLE - 1 tasks and use that to check
	 * if we need to give up the CPU.
	 */
/* 只有当前任务耗尽 slice 且兄弟被强制 idle 时才触发重调度。 */
	if (rq->core->core_forceidle_count && rq->cfs.nr_queued == 1 &&
	    __entity_slice_used(&curr->se, MIN_NR_TASKS_DURING_FORCEIDLE))
		resched_curr(rq);
}

/*
 * Consider any infeasible weight scenario. Take for instance two tasks,
 * each bound to their respective sibling, one with weight 1 and one with
 * weight 2. Then the lower weight task will run ahead of the higher weight
 * task without bound.
 *
 * This utterly destroys the concept of a shared time base.
 *
 * Remember; all this is about a proportionally fair scheduling, where each
 * tasks receives:
 *
 *              w_i
 *   dt_i = ---------- dt                                     (1)
 *          \Sum_j w_j
 *
 * which we do by tracking a virtual time, s_i:
 *
 *          1
 *   s_i = --- d[t]_i                                         (2)
 *         w_i
 *
 * Where d[t] is a delta of discrete time, while dt is an infinitesimal.
 * The immediate corollary is that the ideal schedule S, where (2) to use
 * an infinitesimal delta, is:
 *
 *           1
 *   S = ---------- dt                                        (3)
 *       \Sum_i w_i
 *
 * From which we can define the lag, or deviation from the ideal, as:
 *
 *   lag(i) = S - s_i                                         (4)
 *
 * And since the one and only purpose is to approximate S, we get that:
 *
 *   \Sum_i w_i lag(i) := 0                                   (5)
 *
 * If this were not so, we no longer converge to S, and we can no longer
 * claim our scheduler has any of the properties we derive from S. This is
 * exactly what you did above, you broke it!
 *
 *
 * Let's continue for a while though; to see if there is anything useful to
 * be learned. We can combine (1)-(3) or (4)-(5) and express S in s_i:
 *
 *       \Sum_i w_i s_i
 *   S = --------------                                       (6)
 *         \Sum_i w_i
 *
 * Which gives us a way to compute S, given our s_i. Now, if you've read
 * our code, you know that we do not in fact do this, the reason for this
 * is two-fold. Firstly, computing S in that way requires a 64bit division
 * for every time we'd use it (see 12), and secondly, this only describes
 * the steady-state, it doesn't handle dynamics.
 *
 * Anyway, in (6):  s_i -> x + (s_i - x), to get:
 *
 *           \Sum_i w_i (s_i - x)
 *   S - x = --------------------                             (7)
 *              \Sum_i w_i
 *
 * Which shows that S and s_i transform alike (which makes perfect sense
 * given that S is basically the (weighted) average of s_i).
 *
 * So the thing to remember is that the above is strictly UP. It is
 * possible to generalize to multiple runqueues -- however it gets really
 * yuck when you have to add affinity support, as illustrated by our very
 * first counter-example.
 *
 * Luckily I think we can avoid needing a full multi-queue variant for
 * core-scheduling (or load-balancing). The crucial observation is that we
 * only actually need this comparison in the presence of forced-idle; only
 * then do we need to tell if the stalled rq has higher priority over the
 * other.
 *
 * [XXX assumes SMT2; better consider the more general case, I suspect
 * it'll work out because our comparison is always between 2 rqs and the
 * answer is only interesting if one of them is forced-idle]
 *
 * And (under assumption of SMT2) when there is forced-idle, there is only
 * a single queue, so everything works like normal.
 *
 * Let, for our runqueue 'k':
 *
 *   T_k = \Sum_i w_i s_i
 *   W_k = \Sum_i w_i      ; for all i of k                  (8)
 *
 * Then we can write (6) like:
 *
 *         T_k
 *   S_k = ---                                               (9)
 *         W_k
 *
 * From which immediately follows that:
 *
 *           T_k + T_l
 *   S_k+l = ---------                                       (10)
 *           W_k + W_l
 *
 * On which we can define a combined lag:
 *
 *   lag_k+l(i) := S_k+l - s_i                               (11)
 *
 * And that gives us the tools to compare tasks across a combined runqueue.
 *
 *
 * Combined this gives the following:
 *
 *  a) when a runqueue enters force-idle, sync it against it's sibling rq(s)
 *     using (7); this only requires storing single 'time'-stamps.
 *
 *  b) when comparing tasks between 2 runqueues of which one is forced-idle,
 *     compare the combined lag, per (11).
 *
 * Now, of course cgroups (I so hate them) make this more interesting in
 * that a) seems to suggest we need to iterate all cgroup on a CPU at such
 * boundaries, but I think we can avoid that. The force-idle is for the
 * whole CPU, all it's rqs. So we can mark it in the root and lazily
 * propagate downward on demand.
 */
/* 比例公平依赖共享虚拟时间与加权 lag 守恒；force-idle 时用合并队列 lag 跨 rq 比较。 */

/*
 * So this sync is basically a relative reset of S to 0.
 *
 * So with 2 queues, when one goes idle, we drop them both to 0 and one
 * then increases due to not being idle, and the idle one builds up lag to
 * get re-elected. So far so simple, right?
 *
 * When there's 3, we can have the situation where 2 run and one is idle,
 * we sync to 0 and let the idle one build up lag to get re-election. Now
 * suppose another one also drops idle. At this point dropping all to 0
 * again would destroy the built-up lag from the queue that was already
 * idle, not good.
 *
 * So instead of syncing everything, we can:
 *
 *   less := !((s64)(s_a - s_b) <= 0)
 *
 *   (v_a - S_a) - (v_b - S_b) == v_a - v_b - S_a + S_b
 *                             == v_a - (v_b - S_a + S_b)
 *
 * IOW, we can recast the (lag) comparison to a one-sided difference.
 * So if then, instead of syncing the whole queue, sync the idle queue
 * against the active queue with S_a + S_b at the point where we sync.
 *
 * (XXX consider the implication of living in a cyclic group: N / 2^n N)
 *
 * This gives us means of syncing single queues against the active queue,
 * and for already idle queues to preserve their build-up lag.
 *
 * Of course, then we get the situation where there's 2 active and one
 * going idle, who do we pick to sync against? Theory would have us sync
 * against the combined S, but as we've already demonstrated, there is no
 * such thing in infeasible weight scenarios.
 *
 * One thing I've considered; and this is where that core_active rudiment
 * came from, is having active queues sync up between themselves after
 * every tick. This limits the observed divergence due to the work
 * conservancy.
 *
 * On top of that, we can improve upon things by employing (10) here.
 */
/* 不能反复归零全部队列；只同步新 idle 队列，以保留其他 idle 队列已积累的 lag。 */

/*
 * se_fi_update - Update the cfs_rq->zero_vruntime_fi in a CFS hierarchy if needed.
 */
static void se_fi_update(const struct sched_entity *se, unsigned int fi_seq,
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			 bool forceidle)
{
	/* 沿实体层级惰性同步 force-idle 序列，把比较基准快照到 zero_vruntime_fi。 */
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);

		if (forceidle) {
			if (cfs_rq->forceidle_seq == fi_seq)
				break;
			cfs_rq->forceidle_seq = fi_seq;
		}

		cfs_rq->zero_vruntime_fi = cfs_rq->zero_vruntime;
	}
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}

/* core 调度比较前更新公平任务各层 force-idle 虚拟时间基准；非公平任务忽略。 */
void task_vruntime_update(struct rq *rq, struct task_struct *p, bool in_fi)
{
	struct sched_entity *se = &p->se;

	if (p->sched_class != &fair_sched_class)
		return;

	se_fi_update(se, rq->core->core_forceidle_seq, in_fi);
}

/*
 * 在同一 core 的两个公平任务间比较 force-idle 归一化 vruntime。
 * 返回 true 表示 a 的归一化 vruntime 更大、优先级更低；组调度时先提升到兄弟实体。
 */
bool cfs_prio_less(const struct task_struct *a, const struct task_struct *b,
			bool in_fi)
{
	struct rq *rq = task_rq(a);
	const struct sched_entity *sea = &a->se;
	const struct sched_entity *seb = &b->se;
	struct cfs_rq *cfs_rqa;
	struct cfs_rq *cfs_rqb;
	s64 delta;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	WARN_ON_ONCE(task_rq(b)->core != rq->core);

#ifdef CONFIG_FAIR_GROUP_SCHED
	/*
	 * Find an se in the hierarchy for tasks a and b, such that the se's
	 * are immediate siblings.
	 */
	while (sea->cfs_rq->tg != seb->cfs_rq->tg) {
		int sea_depth = sea->depth;
		int seb_depth = seb->depth;

		if (sea_depth >= seb_depth)
			sea = parent_entity(sea);
		if (sea_depth <= seb_depth)
			seb = parent_entity(seb);
	}

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	se_fi_update(sea, rq->core->core_forceidle_seq, in_fi);
	se_fi_update(seb, rq->core->core_forceidle_seq, in_fi);

	cfs_rqa = sea->cfs_rq;
	cfs_rqb = seb->cfs_rq;
#else /* !CONFIG_FAIR_GROUP_SCHED: */
	cfs_rqa = &task_rq(a)->cfs;
	cfs_rqb = &task_rq(b)->cfs;
#endif /* !CONFIG_FAIR_GROUP_SCHED */

	/*
	 * Find delta after normalizing se's vruntime with its cfs_rq's
	 * zero_vruntime_fi, which would have been updated in prior calls
	 * to se_fi_update().
	 */
	delta = vruntime_op(sea->vruntime, "-", seb->vruntime) +
		vruntime_op(cfs_rqb->zero_vruntime_fi, "-", cfs_rqa->zero_vruntime_fi);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return delta > 0;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}

/* 查询任务放到 cpu 后所在 CFS 层级是否被带宽节流，返回非零表示不可运行。 */
static int task_is_throttled_fair(struct task_struct *p, int cpu)
{
	struct cfs_rq *cfs_rq;

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* 前半段结果已就绪；以下继续完成本分支的复验、传播或返回值整理。 */
	cfs_rq = tg_cfs_rq(task_group(p), cpu);
#else
	cfs_rq = &cpu_rq(cpu)->cfs;
#endif
	return throttled_hierarchy(cfs_rq);
}
#else /* !CONFIG_SCHED_CORE: */
/* 非 core-scheduling 配置无需处理 force-idle tick。 */
static inline void task_tick_core(struct rq *rq, struct task_struct *curr) {}
#endif /* !CONFIG_SCHED_CORE */

/*
 * scheduler tick hitting a task of our scheduling class.
 *
 * NOTE: This function can be called remotely by the tick offload that
 * goes along full dynticks. Therefore no local assumption can be made
 * and everything must be accessed through the @rq and @curr passed in
 * parameters.
 */
/*
 * 公平任务 tick 回调：沿实体祖先链结算 EEVDF/PELT，再维护 NUMA、cache、
 * misfit、overutilized 与 core 状态。queued 表示远程/排队 tick，届时不做后半段。
 */
static void task_tick_fair(struct rq *rq, struct task_struct *curr, int queued)
{
	struct sched_entity *se = &curr->se;
	struct cfs_rq *cfs_rq;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		entity_tick(cfs_rq, se, queued);
	}

	if (queued)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;

	if (static_branch_unlikely(&sched_numa_balancing))
		task_tick_numa(rq, curr);

	task_tick_cache(rq, curr);

	update_misfit_status(curr, rq);
	check_update_overutilized_status(task_rq(curr));

	task_tick_core(rq, curr);
}

/*
 * called on fork with the child task as argument from the parent's context
 *  - child not yet on the tasklist
 *  - preemption disabled
 */
/* fork 阶段为尚未入 tasklist 的子任务缓存其亲和性集合最大 CPU 容量。 */
static void task_fork_fair(struct task_struct *p)
{
	set_task_max_allowed_capacity(p);
}

/*
 * Priority of the task has changed. Check to see if we preempt
 * the current task.
 */
static void
/* 公平任务有效优先级改变后，按当前/等待状态决定是否重调度或做唤醒抢占。 */
prio_changed_fair(struct rq *rq, struct task_struct *p, u64 oldprio)
{
	if (!task_on_rq_queued(p))
		return;

	if (p->prio == oldprio)
		return;

	if (rq->cfs.nr_queued == 1)
		return;

	/*
	 * Reschedule if we are currently running on this runqueue and
	 * our priority decreased, or if we are not currently running on
	 * this runqueue and our priority is higher than the current's
	 */
	if (task_current_donor(rq, p)) {
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (p->prio > oldprio)
			resched_curr(rq);
	} else {
		wakeup_preempt(rq, p, 0);
	}
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * Propagate the changes of the sched_entity across the tg tree to make it
 * visible to the root
 */
/* 将实体负载变化逐层传播到根 cfs_rq，并保证需衰减的叶队列仍在 leaf 列表。 */
static void propagate_entity_cfs_rq(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	/*
	 * If a task gets attached to this cfs_rq and before being queued,
	 * it gets migrated to another CPU due to reasons like affinity
	 * change, make sure this cfs_rq stays on leaf cfs_rq list to have
	 * that removed load decayed or it can cause faireness problem.
	 */
/* 迁组与 PELT 接入、分离、传播必须在正确 rq 锁下按父子顺序完成。 */
	if (!cfs_rq_pelt_clock_throttled(cfs_rq))
		list_add_leaf_cfs_rq(cfs_rq);

	/* Start to propagate at parent */
/* 从父实体开始把负载变化传播到根。 */
	se = se->parent;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		update_load_avg(cfs_rq, se, UPDATE_TG);

	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (!cfs_rq_pelt_clock_throttled(cfs_rq))
			list_add_leaf_cfs_rq(cfs_rq);
	}

	assert_list_leaf_cfs_rq(rq_of(cfs_rq));
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}
#else /* !CONFIG_FAIR_GROUP_SCHED: */
/* 无组调度时没有父实体负载需要传播。 */
static void propagate_entity_cfs_rq(struct sched_entity *se) { }
#endif /* !CONFIG_FAIR_GROUP_SCHED */

/* 实体离开 cfs_rq 前结算 PELT、移除贡献并向任务组祖先传播。 */
static void detach_entity_cfs_rq(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	/*
	 * In case the task sched_avg hasn't been attached:
	 * - A forked task which hasn't been woken up by wake_up_new_task().
	 * - A task which has been woken up by try_to_wake_up() but is
	 *   waiting for actually being woken up by sched_ttwu_pending().
	 */
/* 迁组与 PELT 接入、分离、传播必须在正确 rq 锁下按父子顺序完成。 */
	if (!se->avg.last_update_time)
		return;

	/* Catch up with the cfs_rq and remove our load when we leave */
/* 分离前先追平 PELT 时钟，再移除实体负载。 */
	update_load_avg(cfs_rq, se, 0);
	detach_entity_load_avg(cfs_rq, se);
	update_tg_load_avg(cfs_rq);
	propagate_entity_cfs_rq(se);
}

/* 实体进入新 cfs_rq 时同步 PELT 时钟、加入贡献并向任务组祖先传播。 */
static void attach_entity_cfs_rq(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	/* Synchronize entity with its cfs_rq */
/* 接入前先同步实体与目标 cfs_rq 的 PELT 时钟。 */
	update_load_avg(cfs_rq, se, sched_feat(ATTACH_AGE_LOAD) ? 0 : SKIP_AGE_LOAD);
	attach_entity_load_avg(cfs_rq, se);
	update_tg_load_avg(cfs_rq);
	propagate_entity_cfs_rq(se);
}

/* 任务级包装：从当前 cfs_rq 分离其调度实体负载。 */
static void detach_task_cfs_rq(struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	detach_entity_cfs_rq(se);
}

/* 任务级包装：把调度实体负载接入当前 cfs_rq。 */
static void attach_task_cfs_rq(struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	attach_entity_cfs_rq(se);
}

/* 离开公平类前先兑现 delayed dequeue，避免延迟实体残留在 EEVDF 树。 */
static void switching_from_fair(struct rq *rq, struct task_struct *p)
{
	if (p->se.sched_delayed)
		dequeue_task(rq, p, DEQUEUE_SLEEP | DEQUEUE_DELAYED | DEQUEUE_NOCLOCK);
}

/* 已离开公平类后移除其 PELT 负载贡献。 */
static void switched_from_fair(struct rq *rq, struct task_struct *p)
{
	detach_task_cfs_rq(p);
}

/* 切入公平类后接入 PELT、刷新容量，并在已排队时重新检查抢占。 */
static void switched_to_fair(struct rq *rq, struct task_struct *p)
{
	WARN_ON_ONCE(p->se.sched_delayed);

	attach_task_cfs_rq(p);

	set_task_max_allowed_capacity(p);

	if (task_on_rq_queued(p)) {
		/*
		 * We were most likely switched from sched_rt, so
		 * kick off the schedule if running, otherwise just see
		 * if we can still preempt the current task.
		 */
		if (task_current_donor(rq, p))
			resched_curr(rq);
		else
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
			wakeup_preempt(rq, p, 0);
	}
}

/*
 * Account for a task changing its policy or group.
 *
 * This routine is mostly called to set cfs_rq->curr field when a task
 * migrates between groups/classes.
 */
/* 沿组层级设置 curr 并确保各层配额；首次切入再启动 hrtick 与 misfit 检查。 */
static void set_next_task_fair(struct rq *rq, struct task_struct *p, bool first)
{
	struct sched_entity *se = &p->se;
	bool throttled = false;

	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);

		if (IS_ENABLED(CONFIG_FAIR_GROUP_SCHED) &&
		    first && cfs_rq->curr)
			break;

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		set_next_entity(cfs_rq, se, first);
		/* ensure bandwidth has been allocated on our new cfs_rq */
		throttled |= account_cfs_rq_runtime(cfs_rq, 0);
	}

	if (throttled)
		task_throttle_setup_work(p);

	se = &p->se;

	if (task_on_rq_queued(p)) {
		/*
		 * Move the next running task to the front of the list, so our
		 * cfs_tasks list becomes MRU one.
		 */
		list_move(&se->group_node, &rq->cfs_tasks);
	}
	if (!first)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;

	WARN_ON_ONCE(se->sched_delayed);

	if (hrtick_enabled_fair(rq))
		hrtick_start_fair(rq, p);

	update_misfit_status(p, rq);
	sched_fair_update_stop_tick(rq, p);
}

/* 建立空 EEVDF 树、回绕安全的初始虚拟时间和 removed PELT 锁。 */
void init_cfs_rq(struct cfs_rq *cfs_rq)
{
	cfs_rq->tasks_timeline = RB_ROOT_CACHED;
	cfs_rq->zero_vruntime = (u64)(-(1LL << 20));
	raw_spin_lock_init(&cfs_rq->removed.lock);
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/* 非 TASK_NEW 任务换组时从旧 cfs_rq 分离，重绑 task_rq 后接入新层级。 */
static void task_change_group_fair(struct task_struct *p)
{
	/*
	 * We couldn't detach or attach a forked task which
	 * hasn't been woken up by wake_up_new_task().
	 */
	if (READ_ONCE(p->__state) == TASK_NEW)
		return;

	detach_task_cfs_rq(p);

	/* Tell se's cfs_rq has been changed -- migrated */
/* 清零更新时间表示实体已迁到新的 cfs_rq。 */
	p->se.avg.last_update_time = 0;
	set_task_rq(p, task_cpu(p));
	attach_task_cfs_rq(p);
}

/* 释放任务组按 CPU 分配的 CFS 状态；调用者已保证组离线且无人引用。 */
void free_fair_sched_group(struct task_group *tg)
{
	free_percpu(tg->cfs_rq);
}

/* 为任务组分配并初始化每 CPU cfs_rq/实体；成功返回 1，分配失败返回 0。 */
int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct cfs_tg_state __percpu *state;
	struct sched_entity *se;
	struct cfs_rq *cfs_rq;
	int i;

	state = alloc_percpu_gfp(struct cfs_tg_state, GFP_KERNEL);
	if (!state)
		goto err;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	tg->cfs_rq = &state->cfs_rq;
	tg->shares = NICE_0_LOAD;

	init_cfs_bandwidth(tg_cfs_bandwidth(tg), tg_cfs_bandwidth(parent));

	for_each_possible_cpu(i) {
		cfs_rq = tg_cfs_rq(tg, i);
		if (!cfs_rq)
			goto err;

		se = tg_se(tg, i);
		init_cfs_rq(cfs_rq);
		init_tg_cfs_entry(tg, cfs_rq, se, i, tg_se(parent, i));
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		init_entity_runnable_average(se);
	}

	return 1;

err:
	return 0;
}

/* 逐 CPU 加锁把新任务组实体接入 PELT 层级，并同步带宽节流状态。 */
void online_fair_sched_group(struct task_group *tg)
{
	struct sched_entity *se;
	struct rq_flags rf;
	struct rq *rq;
	int i;

	for_each_possible_cpu(i) {
		rq = cpu_rq(i);
		se = tg_se(tg, i);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		rq_lock_irq(rq, &rf);
		update_rq_clock(rq);
		attach_entity_cfs_rq(se);
		sync_throttle(tg, i);
		rq_unlock_irq(rq, &rf);
	}
}

/* 销毁带宽状态并从每 CPU 叶列表/PELT 中移除空任务组，处理 delayed 实体。 */
void unregister_fair_sched_group(struct task_group *tg)
{
	int cpu;

	destroy_cfs_bandwidth(tg_cfs_bandwidth(tg));

	for_each_possible_cpu(cpu) {
		struct cfs_rq *cfs_rq = tg_cfs_rq(tg, cpu);
		struct sched_entity *se = tg_se(tg, cpu);
		struct rq *rq = cpu_rq(cpu);

		if (se) {
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
			if (se->sched_delayed) {
				guard(rq_lock_irqsave)(rq);
				if (se->sched_delayed) {
					update_rq_clock(rq);
					dequeue_entities(rq, se, DEQUEUE_SLEEP | DEQUEUE_DELAYED);
				}
				list_del_leaf_cfs_rq(cfs_rq);
			}
			remove_entity_load_avg(se);
		}

		/*
		 * Only empty task groups can be destroyed; so we can speculatively
		 * check on_list without danger of it being re-added.
		 */
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
		if (cfs_rq->on_list) {
			guard(rq_lock_irqsave)(rq);
			list_del_leaf_cfs_rq(cfs_rq);
		}
	}
}

/* 初始化任务组在 cpu 上的 cfs_rq 与组实体父子关系；根组允许 se 为 NULL。 */
void init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
			struct sched_entity *se, int cpu,
			struct sched_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	cfs_rq->tg = tg;
	cfs_rq->rq = rq;
	init_cfs_rq_runtime(cfs_rq);

	/* se could be NULL for root_task_group */
	if (!se)
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		return;

	if (!parent) {
		se->cfs_rq = &rq->cfs;
		se->depth = 0;
	} else {
		se->cfs_rq = parent->my_q;
		se->depth = parent->depth + 1;
	}

	se->my_q = cfs_rq;
	/* guarantee group entities always have weight */
	update_load_set(&se->load, NICE_0_LOAD);
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	se->parent = parent;
}

static DEFINE_MUTEX(shares_mutex);

/* shares_mutex 下设置非根组权重并逐 CPU 向祖先传播；返回 0 或 -EINVAL。 */
static int __sched_group_set_shares(struct task_group *tg, unsigned long shares)
{
	int i;

	lockdep_assert_held(&shares_mutex);

	/*
	 * We can't change the weight of the root cgroup.
	 */
/* 迁组与 PELT 接入、分离、传播必须在正确 rq 锁下按父子顺序完成。 */
	if (is_root_task_group(tg))
		return -EINVAL;

	shares = clamp(shares, scale_load(MIN_SHARES), scale_load(MAX_SHARES));

	if (tg->shares == shares)
		return 0;

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	tg->shares = shares;
	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);
		struct sched_entity *se = tg_se(tg, i);
		struct rq_flags rf;

		/* Propagate contribution to hierarchy */
/* 把新的任务组贡献逐层传播到祖先。 */
		rq_lock_irqsave(rq, &rf);
		update_rq_clock(rq);
		for_each_sched_entity(se) {
			update_load_avg(cfs_rq_of(se), se, UPDATE_TG);
			update_cfs_group(se);
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		}
		rq_unlock_irqrestore(rq, &rf);
	}

	return 0;
}

/* 公共权重设置入口：串行化修改，idle 组拒绝显式 shares。 */
int sched_group_set_shares(struct task_group *tg, unsigned long shares)
{
	int ret;

	mutex_lock(&shares_mutex);
	if (tg_is_idle(tg))
		ret = -EINVAL;
	else
		ret = __sched_group_set_shares(tg, shares);
	mutex_unlock(&shares_mutex);

	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
	return ret;
}

/*
 * 将非根任务组切换为 idle/normal，逐 CPU 修正层级 idle 计数并同步最小/正常权重。
 * idle 仅接受 0/1；成功返回 0，非法目标返回 -EINVAL。
 */
int sched_group_set_idle(struct task_group *tg, long idle)
{
	int i;

	if (tg == &root_task_group)
		return -EINVAL;

	if (idle < 0 || idle > 1)
		return -EINVAL;

	mutex_lock(&shares_mutex);

	if (tg->idle == idle) {
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		mutex_unlock(&shares_mutex);
		return 0;
	}

	tg->idle = idle;

	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);
		struct sched_entity *se = tg_se(tg, i);
		struct cfs_rq *grp_cfs_rq = tg_cfs_rq(tg, i);
		bool was_idle = cfs_rq_is_idle(grp_cfs_rq);
		long idle_task_delta;
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		struct rq_flags rf;

		rq_lock_irqsave(rq, &rf);

		grp_cfs_rq->idle = idle;
		if (WARN_ON_ONCE(was_idle == cfs_rq_is_idle(grp_cfs_rq)))
			goto next_cpu;

		idle_task_delta = grp_cfs_rq->h_nr_queued -
				  grp_cfs_rq->h_nr_idle;
		if (!cfs_rq_is_idle(grp_cfs_rq))
			idle_task_delta *= -1;

		for_each_sched_entity(se) {
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			struct cfs_rq *cfs_rq = cfs_rq_of(se);

			if (!se->on_rq)
				break;

			cfs_rq->h_nr_idle += idle_task_delta;

			/* Already accounted at parent level and above. */
/* 父层及以上已经记账时停止重复传播。 */
			if (cfs_rq_is_idle(cfs_rq))
				break;
		}

next_cpu:
		rq_unlock_irqrestore(rq, &rf);
	}

	/* Idle groups have minimum weight. */
	/* 检查当前边界条件，失败分支在继续修改调度状态前退出或改走备用路径。 */
	if (tg_is_idle(tg))
		__sched_group_set_shares(tg, scale_load(WEIGHT_IDLEPRIO));
	else
		__sched_group_set_shares(tg, NICE_0_LOAD);

	mutex_unlock(&shares_mutex);
	return 0;
}

#endif /* CONFIG_FAIR_GROUP_SCHED */


/* 将公平实体当前 slice 换算为 RR 查询接口的 jiffies；空负载 rq 返回 0。 */
static unsigned int get_rr_interval_fair(struct rq *rq, struct task_struct *task)
{
	struct sched_entity *se = &task->se;
	unsigned int rr_interval = 0;

	/*
	 * Time slice is 0 for SCHED_OTHER tasks that are on an otherwise
	 * idle runqueue:
	 */
/* 迁组与 PELT 接入、分离、传播必须在正确 rq 锁下按父子顺序完成。 */
	if (rq->cfs.load.weight)
		rr_interval = NS_TO_JIFFIES(se->slice);

	return rr_interval;
}

/*
 * All the scheduling class methods:
 */
/* 公平调度类操作表：把核心调度器生命周期回调连接到本文件实现。 */
DEFINE_SCHED_CLASS(fair) = {
	.enqueue_task		= enqueue_task_fair,
	.dequeue_task		= dequeue_task_fair,
	.yield_task		= yield_task_fair,
	.yield_to_task		= yield_to_task_fair,

	.wakeup_preempt		= wakeup_preempt_fair,

	.pick_task		= pick_task_fair,
	.put_prev_task		= put_prev_task_fair,
	.set_next_task          = set_next_task_fair,

	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	.select_task_rq		= select_task_rq_fair,
	.migrate_task_rq	= migrate_task_rq_fair,

	.rq_online		= rq_online_fair,
	.rq_offline		= rq_offline_fair,

	.task_dead		= task_dead_fair,
	.set_cpus_allowed	= set_cpus_allowed_fair,

	.task_tick		= task_tick_fair,
	.task_fork		= task_fork_fair,

	.reweight_task		= reweight_task_fair,
	.prio_changed		= prio_changed_fair,
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	.switching_from		= switching_from_fair,
	.switched_from		= switched_from_fair,
	.switched_to		= switched_to_fair,

	.get_rr_interval	= get_rr_interval_fair,

	.update_curr		= update_curr_fair,

#ifdef CONFIG_FAIR_GROUP_SCHED
	.task_change_group	= task_change_group_fair,
#endif

#ifdef CONFIG_SCHED_CORE
	/* 到此已形成一组完整中间结果，下一段在此基础上继续筛选或传播。 */
	.task_is_throttled	= task_is_throttled_fair,
#endif

#ifdef CONFIG_UCLAMP_TASK
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
	.uclamp_enabled		= 1,
#endif
};

/* RCU 下遍历 cpu 的叶 cfs_rq 并输出调试统计到 seq_file。 */
void print_cfs_stats(struct seq_file *m, int cpu)
{
	struct cfs_rq *cfs_rq, *pos;

	rcu_read_lock();
	for_each_leaf_cfs_rq_safe(cpu_rq(cpu), cfs_rq, pos)
		print_cfs_rq(m, cpu, cfs_rq);
	rcu_read_unlock();
}

#ifdef CONFIG_NUMA_BALANCING
/* RCU 下按在线节点输出任务及其 NUMA 组的私有/共享 fault 计数。 */
void show_numa_stats(struct task_struct *p, struct seq_file *m)
{
	int node;
	unsigned long tsf = 0, tpf = 0, gsf = 0, gpf = 0;
	struct numa_group *ng;

	rcu_read_lock();
	ng = rcu_dereference_all(p->numa_group);
	for_each_online_node(node) {
		if (p->numa_faults) {
	/* 更新本阶段局部量，后续比较与记账都读取这份结果。 */
			tsf = p->numa_faults[task_faults_idx(NUMA_MEM, node, 0)];
			tpf = p->numa_faults[task_faults_idx(NUMA_MEM, node, 1)];
		}
		if (ng) {
			gsf = ng->faults[task_faults_idx(NUMA_MEM, node, 0)];
			gpf = ng->faults[task_faults_idx(NUMA_MEM, node, 1)];
		}
		print_numa_stats(m, node, tsf, tpf, gsf, gpf);
	}
	rcu_read_unlock();
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}
#endif /* CONFIG_NUMA_BALANCING */

/*
 * 启动期初始化每 CPU 均衡临时 mask、CFS 带宽 CSD、均衡 softirq 与 NOHZ 状态。
 * 无返回值；分配使用启动期容错语义，后续调度路径依赖这些对象已完成初始化。
 */
__init void init_sched_fair_class(void)
{
	int i;

	for_each_possible_cpu(i) {
		zalloc_cpumask_var_node(&per_cpu(load_balance_mask, i), GFP_KERNEL, cpu_to_node(i));
		zalloc_cpumask_var_node(&per_cpu(select_rq_mask,    i), GFP_KERNEL, cpu_to_node(i));
		zalloc_cpumask_var_node(&per_cpu(should_we_balance_tmpmask, i),
					GFP_KERNEL, cpu_to_node(i));

#ifdef CONFIG_CFS_BANDWIDTH
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
		INIT_CSD(&cpu_rq(i)->cfsb_csd, __cfsb_csd_unthrottle, cpu_rq(i));
		INIT_LIST_HEAD(&cpu_rq(i)->cfsb_csd_list);
#endif
	}

	open_softirq(SCHED_SOFTIRQ, sched_balance_softirq);

#ifdef CONFIG_NO_HZ_COMMON
	nohz.next_balance = jiffies;
	nohz.next_blocked = jiffies;
	zalloc_cpumask_var(&nohz.idle_cpus_mask, GFP_NOWAIT);
#endif
	/* 执行本阶段辅助操作，并把结果交给紧邻的判断或提交路径。 */
}
