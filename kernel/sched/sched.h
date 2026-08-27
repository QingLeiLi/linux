/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Scheduler internal types and methods:
 *
 * 本头文件是调度器各实现文件共享的内部 ABI：struct rq 是每 CPU 状态与主锁域，
 * cfs_rq/rt_rq/dl_rq/scx_rq 分别由对应调度类在 rq 锁下维护；root_domain 把
 * 独占 cpuset 中的 CPU 连接为迁移和准入岛，拓扑替换通过 RCU 发布。无锁 helper
 * 只提供瞬时提示，真正迁移或状态改变必须重新取得 rq 锁并复验。多 rq 加锁统一按
 * 地址排序，timer、RCU 和引用计数分别承担延迟执行、读侧生命周期和对象所有权。
 */
#ifndef _KERNEL_SCHED_SCHED_H
#define _KERNEL_SCHED_SCHED_H

/*
 * 依赖按职责分成调度公共类型、通用内核设施、tracepoint、体系结构屏障和本目录私有索引。
 * 这些 include 让本内部头可以直接定义 rq、各调度类队列及其内联同步 helper；它不是面向
 * 驱动或用户 ABI 的公共入口，目录外代码应优先使用 include/linux/sched*.h 暴露的接口。
 */
#include <linux/prandom.h>
#include <linux/sched/affinity.h>
#include <linux/sched/autogroup.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/deadline.h>
#include <linux/sched.h>
/* task 核心模型之后补入 loadavg、mm、rseq、signal、SMT 与统计子协议。 */
#include <linux/sched/loadavg.h>
#include <linux/sched/mm.h>
#include <linux/sched/rseq_api.h>
#include <linux/sched/signal.h>
#include <linux/sched/smt.h>
#include <linux/sched/stat.h>
/* 调度策略、sysctl、task 标志与拓扑接口定义 task/rq 协议的公共输入。 */
#include <linux/sched/sysctl.h>
#include <linux/sched/task_flags.h>
#include <linux/sched/task.h>
#include <linux/sched/topology.h>
#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/bug.h>
/* cgroup/cpuset 与能力检查约束任务组生命周期、权限和 CPU 允许集合。 */
#include <linux/capability.h>
#include <linux/cgroup_api.h>
#include <linux/cgroup.h>
#include <linux/context_tracking.h>
/* CPU 频率、mask 与 cpuset 把调度放置结果连接到容量和管理约束。 */
#include <linux/cpufreq.h>
#include <linux/cpumask_api.h>
#include <linux/cpuset.h>
#include <linux/ctype.h>
#include <linux/file.h>
#include <linux/fs_api.h>
#include <linux/hrtimer_api.h>
/* IRQ、irq_work 与时间换算支撑跨 CPU 唤醒、带宽 timer 和高精度 tick。 */
#include <linux/interrupt.h>
#include <linux/irq_work.h>
#include <linux/jiffies.h>
#include <linux/kref_api.h>
#include <linux/kthread.h>
#include <linux/ktime_api.h>
#include <linux/lockdep_api.h>
#include <linux/lockdep.h>
/* 内存、memcg 与边界 helper 支撑调度对象分配、压力统计和安全算术。 */
#include <linux/memblock.h>
#include <linux/memcontrol.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex_api.h>
#include <linux/plist.h>
#include <linux/poll.h>
/* proc/seq/profile/PSI 提供调度状态导出与压力观测，不承担热路径同步。 */
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/psi.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <linux/seqlock.h>
#include <linux/softirq.h>
/* 自旋锁、静态键和 stop_machine 分别承担 rq 排他、零成本特性开关和 CPU 停机迁移。 */
#include <linux/spinlock_api.h>
#include <linux/static_key.h>
#include <linux/stop_machine.h>
#include <linux/syscalls_api.h>
#include <linux/syscalls.h>
#include <linux/tick.h>
#include <linux/topology.h>
#include <linux/types.h>
/* u64 快照、用户访问与 vmstat 连接 32 位统计一致性及用户/内存压力接口。 */
#include <linux/u64_stats_sync_api.h>
#include <linux/uaccess.h>
#include <linux/vmstat.h>
#include <linux/wait_api.h>
#include <linux/wait_bit.h>
/* workqueue、delayacct 与 mmu_context 支撑异步均衡、I/O 等待记账和地址空间切换。 */
#include <linux/workqueue_api.h>
#include <linux/delayacct.h>
#include <linux/mmu_context.h>

/* power/sched tracepoint 在热路径内联函数中直接使用，声明必须先于下方 helper。 */
#include <trace/events/power.h>
#include <trace/events/sched.h>

/* 调度器与 workqueue 的内部协作接口不属于稳定子系统 ABI。 */
#include "../workqueue_internal.h"

/*
 * 前置声明切断 rq、各类子队列、拓扑组和 idle 状态之间的类型环；这里只借用指针，
 * 对象的创建、锁定与销毁契约分别由后续完整定义及实现文件负责。
 */
struct rq;
struct cfs_rq;
struct rt_rq;
struct sched_group;
struct cpuidle_state;

#if defined(CONFIG_PARAVIRT) && !defined(CONFIG_HAVE_PV_STEAL_CLOCK_GEN)
/* 缺少通用 PV steal-clock 生成器时，调度时钟需使用体系结构 paravirt 接口。 */
# include <asm/paravirt.h>
#endif

/* 下方 rq 时钟、迁移和 membarrier helper 依赖体系结构内存屏障原语。 */
#include <asm/barrier.h>

/* RT/DL SMP 推拉分别使用 CPU 优先级索引与最早截止期索引。 */
#include "cpupri.h"
#include "cpudeadline.h"

/* task_struct::on_rq states: */
/*
 * task_struct::on_rq 的状态：QUEUED 表示实体受某个 rq 管理，MIGRATING 表示迁移
 * 路径已将它从旧 rq 摘除、但尚未在新 rq 完成发布；后者禁止普通唤醒路径按稳定归属处理。
 */
#define TASK_ON_RQ_QUEUED	1
#define TASK_ON_RQ_MIGRATING	2

extern __read_mostly int scheduler_running;

/*
 * 全局 loadavg 协议：calc_load_update 给出下一折叠时刻，calc_load_tasks 聚合各 rq 的
 * active 增量；tick 路径写入，周期更新路径读取。原子量只保护跨 CPU 增量，不冻结 rq 状态。
 */
extern unsigned long calc_load_update;
extern atomic_long_t calc_load_tasks;

extern void calc_global_load_tick(struct rq *this_rq);
extern long calc_load_fold_active(struct rq *this_rq, long adjust);

extern void call_trace_sched_update_nr_running(struct rq *rq, int count);

/* RT 周期/runtime 与 RR 时间片均由 sysctl 初始化并按各自单位在使用点转换。 */
extern int sysctl_sched_rt_period;
extern int sysctl_sched_rt_runtime;
extern int sched_rr_timeslice;

/*
 * Asymmetric CPU capacity bits
 */
/*
 * 非对称 CPU 容量索引节点：capacity 是该组 CPU 的原始算力等级，cpus 柔性位图列出
 * 同等级 CPU。拓扑代码创建并挂入 asym_cap_list，读侧借助 RCU 遍历，rcu 字段承接延迟释放。
 */
struct asym_cap_data {
	struct list_head link;
	struct rcu_head rcu;
	unsigned long capacity;
	unsigned long cpus[];
};

extern struct list_head asym_cap_list;

#define cpu_capacity_span(asym_data) to_cpumask((asym_data)->cpus)

/*
 * Helpers for converting nanosecond timing to jiffy resolution
 */
/*
 * 把纳秒时长降精度为 jiffy 数；整数除法向下取整，因此不足一个 tick 的余数会丢失，
 * 只适合允许 tick 粒度的调度超时，不能代替高分辨率定时器换算。
 */
#define NS_TO_JIFFIES(time)	((unsigned long)(time) / (NSEC_PER_SEC/HZ))

/*
 * Increase resolution of nice-level calculations for 64-bit architectures.
 * The extra resolution improves shares distribution and load balancing of
 * low-weight task groups (eg. nice +19 on an autogroup), deeper task-group
 * hierarchies, especially on larger systems. This is not a user-visible change
 * and does not change the user-interface for setting shares/weights.
 *
 * We increase resolution only if we have enough bits to allow this increased
 * resolution (i.e. 64-bit). The costs for increasing resolution when 32-bit
 * are pretty high and the returns do not justify the increased costs.
 *
 * Really only required when CONFIG_FAIR_GROUP_SCHED=y is also set, but to
 * increase coverage and consistency always enable it on 64-bit platforms.
 */
/*
 * 64 位构建为内部 load 多保留一组定点小数位，以改善低权重、深层 task_group 的份额
 * 分配和负载均衡；用户可见 weight 的范围不变。32 位若照做会显著增加溢出与宽算术成本，
 * 因而保持原分辨率。scale_load_down() 对非零极小值钳到 2，避免反向换算把活动负载抹成零。
 */
#ifdef CONFIG_64BIT
/* 64 位有足够中间位宽：左移提高精度，反向换算对非零小权重保留最小有效贡献。 */
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT + SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		((w) << SCHED_FIXEDPOINT_SHIFT)
# define scale_load_down(w)					\
({								\
	unsigned long __w = (w);				\
								\
	if (__w)						\
		__w = max(2UL, __w >> SCHED_FIXEDPOINT_SHIFT);	\
	__w;							\
})
/* scale_load_down() 的语句表达式返回 __w；非零权重被钳到至少 2，避免层级贡献消失。 */
#else
/* 32 位保持用户权重原尺度，避免宽乘除和溢出成本进入调度热路径。 */
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		(w)
# define scale_load_down(w)	(w)
#endif

/*
 * Task weight (visible to users) and its load (invisible to users) have
 * independent resolution, but they should be well calibrated. We use
 * scale_load() and scale_load_down(w) to convert between them. The
 * following must be true:
 *
 *  scale_load(sched_prio_to_weight[NICE_TO_PRIO(0)-MAX_RT_PRIO]) == NICE_0_LOAD
 *
 */
/*
 * 用户权重与内部 load 使用不同精度，二者必须通过 scale_load()/scale_load_down()
 * 成对换算；等式规定 nice 0 权重必须精确映射为 NICE_0_LOAD，是负载表和调度份额的基准不变量。
 */
#define NICE_0_LOAD		(1L << NICE_0_LOAD_SHIFT)

/*
 * Single value that decides SCHED_DEADLINE internal math precision.
 * 10 -> just above 1us
 * 9  -> just above 0.5us
 */
/* DL 内部时间定点精度：10 约对应略高于 1 微秒，9 约对应略高于 0.5 微秒。 */
#define DL_SCALE		10

/*
 * Single value that denotes runtime == period, ie unlimited time.
 */
/* runtime 等于 period、即不设运行时上限时使用的 u64 哨兵值。 */
#define RUNTIME_INF		((u64)~0ULL)

/* policy 分类 helper 只解释策略号，不证明 task 当前仍保持该策略。 */
/*
 * 业务背景：idle_policy() 判断策略号是否选择最低优先级的 SCHED_IDLE 类。
 * 入参：policy 是按值传入的用户/内核调度策略号，无引用与 ownership 变化。
 * 出参/返回：匹配返回 1，否则返回 0；无其他副作用。
 * 注意事项：这是瞬时分类，不锁定任何 task，也不证明并发任务仍保持该策略；可在任意上下文调用且不睡眠。
 */
static inline int idle_policy(int policy)
{
	return policy == SCHED_IDLE;
}

/*
 * 业务背景：normal_policy() 识别普通分时策略，并在 sched_ext 编译启用时把 SCHED_EXT 纳入普通策略族。
 * 入参：policy 为策略号，纯输入值；无指针、引用或 ownership。
 * 出参/返回：SCHED_NORMAL（以及配置允许时的 SCHED_EXT）返回真，否则返回假；无副作用。
 * 注意事项：条件编译会改变分类集合；函数不加锁、不睡眠，调用者仍需在 rq/task 锁下验证任务策略稳定性。
 */
static inline int normal_policy(int policy)
{
#ifdef CONFIG_SCHED_CLASS_EXT
	if (policy == SCHED_EXT)
		return true;
#endif
	return policy == SCHED_NORMAL;
}

/*
 * 业务背景：fair_policy() 把 normal_policy() 与批处理策略合并，供 CFS 路径选择调度类。
 * 入参：policy 为纯输入策略号，范围应是 SCHED_* 值；无 ownership。
 * 出参/返回：属于 CFS/扩展普通族或 SCHED_BATCH 时返回真，否则返回假；无副作用。
 * 注意事项：只组合分类结果，不读取 task；可在原子上下文调用且不睡眠。
 */
static inline int fair_policy(int policy)
{
	return normal_policy(policy) || policy == SCHED_BATCH;
}

/*
 * 业务背景：rt_policy() 识别固定优先级 FIFO/RR 策略，供权限、优先级和 RT 带宽路径分流。
 * 入参：policy 为纯输入策略号；无指针和 ownership。
 * 出参/返回：SCHED_FIFO 或 SCHED_RR 返回真，其余返回假；无副作用。
 * 注意事项：不验证 rt_priority 合法性，也不稳定任务状态；函数不加锁且不睡眠。
 */
static inline int rt_policy(int policy)
{
	return policy == SCHED_FIFO || policy == SCHED_RR;
}

/*
 * 业务背景：dl_policy() 识别 EDF/CBS 管理的 SCHED_DEADLINE 策略。
 * 入参：policy 为纯输入策略号；无引用或 ownership。
 * 出参/返回：仅 SCHED_DEADLINE 返回真；无输出参数和其他副作用。
 * 注意事项：不检查 runtime/deadline/period 参数或准入额度；可在任意上下文调用且不睡眠。
 */
static inline int dl_policy(int policy)
{
	return policy == SCHED_DEADLINE;
}

/* 仅接受已编译进内核且归属现有调度类的用户策略。 */
/*
 * 业务背景：valid_policy() 汇总所有可接受的用户调度策略分类，供属性设置入口拒绝未知编号。
 * 入参：policy 为纯输入策略号；无 ownership。
 * 出参/返回：命中 idle/fair/rt/dl 任一分类返回真，否则返回假；无副作用。
 * 注意事项：它只验证策略类别，不验证权限、优先级或带宽；SCHED_EXT 是否有效受配置控制，且函数不睡眠。
 */
static inline bool valid_policy(int policy)
{
	return idle_policy(policy) || fair_policy(policy) ||
		rt_policy(policy) || dl_policy(policy);
}

/*
 * 业务背景：task_has_idle_policy() 从 task 当前 policy 快照判断是否处于 SCHED_IDLE。
 * 入参：p 是调用者借用的非空 task 指针，函数不取得引用、不转移 ownership。
 * 出参/返回：读取到 SCHED_IDLE 返回真，否则返回假；不修改 task。
 * 注意事项：无锁读取仅是提示；需要稳定结论的调用者须持有相应 task/rq 锁，函数本身不睡眠。
 */
static inline int task_has_idle_policy(struct task_struct *p)
{
	return idle_policy(p->policy);
}

/*
 * 业务背景：task_has_rt_policy() 为唤醒、迁移和属性变更路径快速识别 FIFO/RR 任务。
 * 入参：p 是借用且必须有效的 task 指针；调用前后引用与 ownership 不变。
 * 出参/返回：当前 policy 属于 RT 类返回真，否则返回假；无副作用。
 * 注意事项：不稳定并发策略切换，也不检查有效优先级；函数不加锁且不睡眠。
 */
static inline int task_has_rt_policy(struct task_struct *p)
{
	return rt_policy(p->policy);
}

/*
 * 业务背景：task_has_dl_policy() 为 DL 准入、迁移与抢占逻辑识别 deadline 任务。
 * 入参：p 是调用者借用的非空 task 指针，不增加引用。
 * 出参/返回：当前 policy 为 SCHED_DEADLINE 返回真，否则返回假；无状态修改。
 * 注意事项：需要与策略更新串行化时由调用者持锁；本 helper 不睡眠，也不验证 DL 参数。
 */
static inline int task_has_dl_policy(struct task_struct *p)
{
	return dl_policy(p->policy);
}

#define cap_scale(v, s)		((v)*(s) >> SCHED_CAPACITY_SHIFT)

/* 以 1/8 步长更新低成本指数平均；调用者负责并发串行化。 */
/*
 * 业务背景：update_avg() 用固定 1/8 增益平滑短期样本，避免统计值随单次抖动剧烈变化。
 * 入参：avg 是调用者拥有的非空输入输出累计值；sample 是同单位的新样本，均不转移 ownership。
 * 出参/返回：void；把 *avg 推近 sample 的八分之一距离，除此之外无副作用。
 * 注意事项：差值按有符号量计算以支持下降；函数不做同步、不睡眠，调用者必须串行化并保证单位一致。
 */
static inline void update_avg(u64 *avg, u64 sample)
{
	s64 diff = sample - *avg;

	*avg += diff / 8;
}

/*
 * Shifting a value by an exponent greater *or equal* to the size of said value
 * is UB; cap at size-1.
 */
/* 右移量达到或超过值类型位宽在 C 中是未定义行为；该宏把 shift 钳到位宽减一后再执行。 */
#define shr_bound(val, shift)							\
	(val >> min_t(typeof(shift), shift, BITS_PER_TYPE(typeof(val)) - 1))

/*
 * cgroup weight knobs should use the common MIN, DFL and MAX values which are
 * 1, 100 and 10000 respectively. While it loses a bit of range on both ends, it
 * maps pretty well onto the shares value used by scheduler and the round-trip
 * conversions preserve the original value over the entire range.
 */
/* 在 cgroup [1,10000] 权重与调度器 1024 基准间做可逆近似映射。 */
/*
 * cgroup 权重旋钮统一使用 1/100/10000 的最小、默认、最大值；虽略微缩小两端范围，
 * 却能良好映射到调度器 shares，并保证整个范围内往返换算恢复原值。
 *
 * 业务背景：sched_weight_from_cgroup() 把用户 cgroup 权重映射为以 1024 为默认值的内部 shares。
 * 入参：cgrp_weight 是纯输入权重，合法业务范围为 CGROUP_WEIGHT_MIN..MAX；无 ownership。
 * 出参/返回：返回四舍五入后的内部权重；无输出参数和副作用。
 * 注意事项：调用者应先保证范围合法；函数仅做整数换算，不加锁且不睡眠。
 */
static inline unsigned long sched_weight_from_cgroup(unsigned long cgrp_weight)
{
	return DIV_ROUND_CLOSEST_ULL(cgrp_weight * 1024, CGROUP_WEIGHT_DFL);
}

/*
 * 业务背景：sched_weight_to_cgroup() 把内部 shares 反向映射为用户可见 cgroup 权重。
 * 入参：weight 是以 1024 为默认基准的纯输入内部权重；无 ownership。
 * 出参/返回：返回四舍五入并钳在 CGROUP_WEIGHT_MIN..MAX 的权重；无副作用。
 * 注意事项：钳位保证用户 ABI 范围，极端内部值会饱和；函数不加锁且不睡眠。
 */
static inline unsigned long sched_weight_to_cgroup(unsigned long weight)
{
	return clamp_t(unsigned long,
		       DIV_ROUND_CLOSEST_ULL(weight * CGROUP_WEIGHT_DFL, 1024),
		       CGROUP_WEIGHT_MIN, CGROUP_WEIGHT_MAX);
}

/*
 * !! For sched_setattr_nocheck() (kernel) only !!
 *
 * This is actually gross. :(
 *
 * It is used to make schedutil kworker(s) higher priority than SCHED_DEADLINE
 * tasks, but still be able to sleep. We need this on platforms that cannot
 * atomically change clock frequency. Remove once fast switching will be
 * available on such platforms.
 *
 * SUGOV stands for SchedUtil GOVernor.
 */
/*
 * 该仅供内核 sched_setattr_nocheck() 使用的标志让 schedutil kworker 排在普通
 * SCHED_DEADLINE 任务之前，同时仍可睡眠，以适配不能原子切换频率的平台；这是慢速切频
 * 的临时折衷，快速切频普及时应移除。SUGOV 即 SchedUtil governor，用户态不得设置该位。
 */
#define SCHED_FLAG_SUGOV	0x10000000

#define SCHED_DL_FLAGS		(SCHED_FLAG_RECLAIM | SCHED_FLAG_DL_OVERRUN | SCHED_FLAG_SUGOV)

/* schedutil 特殊实体不占普通 DL reservation，且在 EDF 比较中始终优先。 */
/*
 * 业务背景：dl_entity_is_special() 识别为慢速 schedutil 切频保留的特殊 DL 实体。
 * 入参：dl_se 是调用者借用的非空 deadline 实体，仅只读 flags，不取得引用。
 * 出参/返回：配置启用且带 SCHED_FLAG_SUGOV 时返回真，否则返回假；无副作用。
 * 注意事项：unlikely 标注该分支罕见；关闭 governor 配置时恒假，函数不加锁且不睡眠。
 */
static inline bool dl_entity_is_special(const struct sched_dl_entity *dl_se)
{
#ifdef CONFIG_CPU_FREQ_GOV_SCHEDUTIL
	return unlikely(dl_se->flags & SCHED_FLAG_SUGOV);
#else
	return false;
#endif
}

/*
 * Tells if entity @a should preempt entity @b.
 */
/*
 * 判断实体 @a 是否应抢占实体 @b：特殊 schedutil 实体无条件优先，否则按绝对截止期早者优先。
 *
 * 业务背景：dl_entity_preempt() 封装 DL 类的核心 EDF 抢占比较，供入队和唤醒路径决策。
 * 入参：a、b 均为调用者借用的非空只读实体，生命周期和锁由调用者保证，ownership 不变。
 * 出参/返回：a 更应先运行返回真，否则返回假；不修改实体。
 * 注意事项：dl_time_before() 处理时间回绕；调用者须在稳定 deadline 的 rq 锁域内使用，函数不睡眠。
 */
static inline bool dl_entity_preempt(const struct sched_dl_entity *a,
				     const struct sched_dl_entity *b)
{
	return dl_entity_is_special(a) ||
	       dl_time_before(a->deadline, b->deadline);
}

/*
 * This is the priority-queue data structure of the RT scheduling class:
 */
/* bitmap 找最高优先级，queue 保持同优先级 FIFO/RR 顺序；末位是哨兵。 */
/*
 * RT 调度类的优先队列：bitmap 快速定位最高非空优先级，queue[] 保存每个优先级的
 * FIFO/RR 链表；额外的一位作为扫描终止哨兵，避免无任务时越界。结构由所属 rt_rq 在 rq 锁下维护。
 */
struct rt_prio_array {
	DECLARE_BITMAP(bitmap, MAX_RT_PRIO+1); /* include 1 bit for delimiter */
	/* 上述额外一位包含扫描分隔符，不代表可运行的用户 RT 优先级。 */
	struct list_head queue[MAX_RT_PRIO];
};

/* RT 配额及补充 timer；runtime_lock 嵌套在 rq 锁内，反序会死锁。 */
/*
 * RT 带宽对象由调度域/任务组持有：period 与 runtime 定义周期预算，period_timer 负责补充，
 * period_active 表示定时器协议已启动。runtime_lock 嵌套在 rq 锁内，反序获取会形成 AB-BA 死锁。
 */
struct rt_bandwidth {
	/* nests inside the rq lock: */
	/* 该锁嵌套在 rq 锁内，保护本带宽对象的预算与补充定时器状态。 */
	raw_spinlock_t		rt_runtime_lock;
	ktime_t			rt_period;
	u64			rt_runtime;
	struct hrtimer		rt_period_timer;
	unsigned int		rt_period_active;
};

/*
 * 业务背景：dl_bandwidth_enabled() 复用全局 RT runtime 旋钮判断 DL 带宽准入是否启用。
 * 入参：无。
 * 出参/返回：sysctl_sched_rt_runtime 非负时返回真，负值表示关闭带宽限制；无副作用。
 * 注意事项：读取的是全局瞬时配置，不锁定后续准入过程；函数不加锁且不睡眠。
 */
static inline int dl_bandwidth_enabled(void)
{
	return sysctl_sched_rt_runtime >= 0;
}

/*
 * To keep the bandwidth of -deadline tasks under control
 * we need some place where:
 *  - store the maximum -deadline bandwidth of each cpu;
 *  - cache the fraction of bandwidth that is currently allocated in
 *    each root domain;
 *
 * This is all done in the data structure below. It is similar to the
 * one used for RT-throttling (rt_bandwidth), with the main difference
 * that, since here we are only interested in admission control, we
 * do not decrease any runtime while the group "executes", neither we
 * need a timer to replenish it.
 *
 * With respect to SMP, bandwidth is given on a per root domain basis,
 * meaning that:
 *  - bw (< 100%) is the deadline bandwidth of each CPU;
 *  - total_bw is the currently allocated bandwidth in each root domain;
 */
/* root_domain 级 DL 准入账本，所有 total_bw 替换都在 lock 下完成。 */
/*
 * 为约束 deadline 任务带宽，需要同时保存每 CPU 最大 DL 带宽，以及每个 root_domain
 * 已分配带宽比例。它类似 RT throttling 的 rt_bandwidth，但这里只做准入，不随执行扣减
 * runtime，也不需要周期补充 timer。SMP 下 bw 是每 CPU 上限（小于 100%），total_bw 是
 * 整个 root_domain 已承诺总量。lock 串行化检查与记账，防止两个并发准入共同超额。
 */
struct dl_bw {
	/* lock 把“检查剩余容量”和“提交 total_bw”合成一次原子准入事务。 */
	raw_spinlock_t		lock;
	/* bw 是单 CPU 可分配比例，total_bw 是整个 root_domain 已承诺比例，均用 DL 定点尺度。 */
	u64			bw;
	u64			total_bw;
};

/*
 * DL 准入/参数接口组：调用者借用 task、attr、cpumask 或 rq；可能更新 task 的 DL 参数、
 * root_domain 带宽账本或全局配置，具体返回 0/errno、锁与回滚边界由 deadline.c 实现。
 * 声明本身不取得引用，持久使用对象前仍需遵守 task_rq_lock、cpuset 与 DL 带宽锁协议。
 */
/* 初始化/全局校验处理账本，参数读写与 overflow 检查处理单任务策略，末组接口处理 CPU/执行扣费。 */
extern void init_dl_bw(struct dl_bw *dl_b);
extern int  sched_dl_global_validate(void);
extern void sched_dl_do_global(void);
extern int  sched_dl_overflow(struct task_struct *p, int policy, const struct sched_attr *attr);
/* set/get/check/changed 形成 sched_attr 校验与提交链，输出 attr 仍由调用者拥有。 */
extern void __setparam_dl(struct task_struct *p, const struct sched_attr *attr);
extern void __getparam_dl(struct task_struct *p, struct sched_attr *attr, unsigned int flags);
extern bool __checkparam_dl(const struct sched_attr *attr);
extern bool dl_param_changed(struct task_struct *p, const struct sched_attr *attr);
extern int  dl_cpuset_cpumask_can_shrink(const struct cpumask *cur, const struct cpumask *trial);
extern int  dl_bw_deactivate(int cpu);
extern s64 dl_scaled_delta_exec(struct rq *rq, struct sched_dl_entity *dl_se, s64 delta_exec);
/*
 * SCHED_DEADLINE supports servers (nested scheduling) with the following
 * interface:
 *
 *   dl_se::rq -- runqueue we belong to.
 *
 *   dl_se::server_pick() -- nested pick_next_task(); we yield the period if this
 *                           returns NULL.
 *
 *   dl_server_update() -- called from update_curr_common(), propagates runtime
 *                         to the server.
 *
 *   dl_server_start() -- start the server when it has tasks; it will stop
 *			  automatically when there are no more tasks, per
 *			  dl_se::server_pick() returning NULL.
 *
 *   dl_server_stop() -- (force) stop the server; use when updating
 *                       parameters.
 *
 *   dl_server_init() -- initializes the server.
 *
 * When started the dl_server will (per dl_defer) schedule a timer for its
 * zero-laxity point -- that is, unlike regular EDF tasks which run ASAP, a
 * server will run at the very end of its period.
 *
 * This is done such that any runtime from the target class can be accounted
 * against the server -- through dl_server_update() above -- such that when it
 * becomes time to run, it might already be out of runtime and get deferred
 * until the next period. In this case dl_server_timer() will alternate
 * between defer and replenish but never actually enqueue the server.
 *
 * Only when the target class does not manage to exhaust the server's runtime
 * (there's actualy starvation in the given period), will the dl_server get on
 * the runqueue. Once queued it will pick tasks from the target class and run
 * them until either its runtime is exhaused, at which point its back to
 * dl_server_timer, or until there are no more tasks to run, at which point
 * the dl_server stops itself.
 *
 * By stopping at this point the dl_server retains bandwidth, which, if a new
 * task wakes up imminently (starting the server again), can be used --
 * subject to CBS wakeup rules -- without having to wait for the next period.
 *
 * Additionally, because of the dl_defer behaviour the start/stop behaviour is
 * naturally thottled to once per period, avoiding high context switch
 * workloads from spamming the hrtimer program/cancel paths.
 */
/*
 * DL server 为“调度类之上的嵌套调度”提供 CBS 预算：dl_se::rq 指明归属 rq，server_pick()
 * 从目标类选任务，返回 NULL 表示让出本周期；update 传播目标任务消耗，start/stop 管理活动期，
 * init 建立实体。defer 模式把 timer 放在零松弛点，使 server 尽量在周期末运行；目标类若已耗尽
 * 预算，timer 只在延后与补充间切换而不入队。只有目标类发生实际饥饿时 server 才入队代它运行，
 * 直至预算耗尽或无任务。停止时保留带宽，近期再次唤醒可按 CBS 规则复用；每周期最多启停一次，
 * 避免高频上下文切换反复编程/取消 hrtimer。所有调用者必须在各接口要求的 rq/带宽锁域中维护状态。
 */
extern void dl_server_update_idle(struct sched_dl_entity *dl_se, s64 delta_exec);
extern void dl_server_update(struct sched_dl_entity *dl_se, s64 delta_exec);
extern void dl_server_start(struct sched_dl_entity *dl_se);
extern void dl_server_stop(struct sched_dl_entity *dl_se);
extern void dl_server_init(struct sched_dl_entity *dl_se, struct rq *rq,
		    dl_server_pick_f pick_task);
extern void sched_init_dl_servers(void);

/*
 * fair/ext server 初始化及带宽附着接口把嵌套 DL 实体连接到 root_domain 准入账本。
 * attach/apply/swap 可能以负 errno 失败；成功后 detach 承担对称撤销，调用者不能只改实体参数
 * 而跳过账本迁移，否则已承诺带宽与实际 server 会失配。
 */
extern void fair_server_init(struct rq *rq);
extern void ext_server_init(struct rq *rq);
extern void __dl_server_attach_root(struct sched_dl_entity *dl_se, struct rq *rq);
extern int dl_server_apply_params(struct sched_dl_entity *dl_se,
		    u64 runtime, u64 period, bool init);
extern int dl_server_attach_bw(struct sched_dl_entity *dl_se);
extern void dl_server_detach_bw(struct sched_dl_entity *dl_se);
extern int dl_server_swap_bw(struct sched_dl_entity *detach_se,
			     struct sched_dl_entity *attach_se);

/*
 * 业务背景：dl_server_active() 为调度类快速读取嵌套 DL server 是否已启动。
 * 入参：dl_se 是调用者借用的非空实体；函数不取得引用、不转移 ownership。
 * 出参/返回：返回 dl_server_active 状态快照；无输出参数和副作用。
 * 注意事项：无锁读取不提供跨 CPU 稳定性，决策路径须由 rq 锁或所属协议串行化；函数不睡眠。
 */
static inline bool dl_server_active(struct sched_dl_entity *dl_se)
{
	return dl_se->dl_server_active;
}

#ifdef CONFIG_CGROUP_SCHED

extern struct list_head task_groups;

#ifdef CONFIG_GROUP_SCHED_BANDWIDTH
extern const u64 max_bw_quota_period_us;

/*
 * default period for group bandwidth.
 * default: 0.1s, units: microseconds
 */
/*
 * 任务组带宽的默认周期为 0.1 秒，返回单位是微秒；该常量同时约束 quota 的解释单位。
 *
 * 业务背景：default_bw_period_us() 为新建 CFS/RT 任务组提供统一的带宽周期默认值。
 * 入参：无。
 * 出参/返回：返回 100000 微秒；无副作用。
 * 注意事项：仅在 GROUP_SCHED_BANDWIDTH 配置下存在，是常量 helper，不加锁且不睡眠。
 */
static inline u64 default_bw_period_us(void)
{
	return 100000ULL;
}
#endif /* CONFIG_GROUP_SCHED_BANDWIDTH */

/* task_group 的 CFS 配额池；throttled 列表和 timer 均由本结构 lock 串行化。 */
/*
 * task_group 的 CFS 配额池：period/quota/runtime/burst 使用时间预算，hierarchical_quota
 * 缓存层级限制；两个 hrtimer 分别补充周期预算与回收 slack。throttled_cfs_rq 保存因欠费
 * 被摘除的每 CPU 队列，统计字段累计节流与突发结果。lock 串行化预算、列表和 timer 状态。
 */
struct cfs_bandwidth {
#ifdef CONFIG_CFS_BANDWIDTH
	/* 预算、timer 状态与 throttled 队列作为同一事务在此锁下变化。 */
	raw_spinlock_t		lock;
	/* period/quota/runtime/burst 为时间预算；runtime_snap 支持一致统计，hierarchical_quota 合并父限额。 */
	ktime_t			period;
	u64			quota;
	u64			runtime;
	u64			burst;
	u64			runtime_snap;
	s64			hierarchical_quota;

	/* 三个状态位分别表示无债务空闲、周期 timer 活动和 slack timer 已启动。 */
	u8			idle;
	u8			period_active;
	u8			slack_started;
	struct hrtimer		period_timer;
	struct hrtimer		slack_timer;
	/* 欠费 cfs_rq 挂入该链，补充预算后由解节流路径逐个重新发布。 */
	struct list_head	throttled_cfs_rq;

	/* Statistics: */
	/* 以下计数只做观测：周期数、节流次数、burst 次数及对应累计时间，不参与所有权。 */
	int			nr_periods;
	int			nr_throttled;
	int			nr_burst;
	u64			throttled_time;
	u64			burst_time;
#endif /* CONFIG_CFS_BANDWIDTH */
/* 至此结束 CFS 带宽字段；配置关闭时结构保留类型但不携带预算状态。 */
};

/* Task group related information */
/* 下面定义任务组相关的共享调度状态。 */
/*
 * cgroup 调度节点：每 CPU 子 rq 由该组拥有，父子/兄弟链经 RCU 遍历；销毁时
 * 必须先从层级摘除并等待读者，再释放 per-CPU 实体与带宽状态。
 */
struct task_group {
	struct cgroup_subsys_state css;

#ifdef CONFIG_GROUP_SCHED_WEIGHT
	/* A positive value indicates that this is a SCHED_IDLE group. */
	/* 正值表示整个组按 SCHED_IDLE 权重语义参与层级竞争。 */
	int			idle;
#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* runqueue "owned" by this group on each CPU */
	/* 每 CPU cfs_rq 由本组拥有，组销毁前必须先停止调度引用并完成 RCU 同步。 */
	struct cfs_rq __percpu	*cfs_rq;
	unsigned long		shares;
	/*
	 * load_avg can be heavily contended at clock tick time, so put
	 * it in its own cache-line separated from the fields above which
	 * will also be accessed at each tick.
	 */
	/*
	 * load_avg 在 tick 上竞争很重，单独放入 cache line，避免与同样每 tick 访问的 shares/cfs_rq
	 * 发生伪共享；它是派生负载缓存，更新必须遵循 fair 调度的层级传播协议。
	 */
	atomic_long_t		load_avg ____cacheline_aligned;
#endif /* CONFIG_FAIR_GROUP_SCHED */
/* 至此结束公平组调度字段；后续成员与是否启用 FAIR_GROUP 无关。 */

#ifdef CONFIG_RT_GROUP_SCHED
	/* 每 CPU RT 实体/子队列共同表达本组在父层中的位置，生命周期随 task_group。 */
	struct sched_rt_entity	**rt_se;
	struct rt_rq		**rt_rq;

	/* 组级周期预算向每 CPU rt_rq 分发 runtime。 */
	struct rt_bandwidth	rt_bandwidth;
#endif

	/* sched_ext 为同一 cgroup 保存 BPF 调度器私有组状态。 */
	struct scx_task_group	scx;

	/* list 是全局组索引；rcu 在摘除后延迟最终释放，防止树遍历者悬空。 */
	struct rcu_head		rcu;
	struct list_head	list;

	/* parent/siblings/children 构成层级树，修改必须与 cgroup attach/destroy 协议串行化。 */
	struct task_group	*parent;
	struct list_head	siblings;
	struct list_head	children;

#ifdef CONFIG_SCHED_AUTOGROUP
	/* autogroup 为交互任务动态选择的上层对象，引用关系由 autogroup 子系统维护。 */
	struct autogroup	*autogroup;
#endif

	/* 每个组拥有自己的 CFS 配额池；未启用带宽配置时结构为空壳。 */
	struct cfs_bandwidth	cfs_bandwidth;

#ifdef CONFIG_UCLAMP_TASK_GROUP
	/* The two decimal precision [%] value requested from user-space */
	/* 用户请求的百分比，保留两位小数；它是配置源值而非调度热路径直接使用值。 */
	unsigned int		uclamp_pct[UCLAMP_CNT];
	/* Clamp values requested for a task group */
	/* 从用户请求归一化出的 task_group clamp，尚未叠加父组限制。 */
	struct uclamp_se	uclamp_req[UCLAMP_CNT];
	/* Effective clamp values used for a task group */
	/* 与父层级约束合并后的有效 clamp，任务入队时据此聚合到 rq。 */
	struct uclamp_se	uclamp[UCLAMP_CNT];
#endif

};

#ifdef CONFIG_GROUP_SCHED_WEIGHT
#define ROOT_TASK_GROUP_LOAD	NICE_0_LOAD

/*
 * A weight of 0 or 1 can cause arithmetics problems.
 * A weight of a cfs_rq is the sum of weights of which entities
 * are queued on this cfs_rq, so a weight of a entity should not be
 * too large, so as the shares value of a task group.
 * (The default weight is 1024 - so there's no practical
 *  limitation from this.)
 */
/*
 * 权重 0 或 1 会破坏部分除法/比例算术。cfs_rq 权重是队列实体权重之和，因此单实体及
 * task_group shares 也不能过大；默认值 1024 距边界很远，正常配置不会受实际限制。
 */
#define MIN_SHARES		(1UL <<  1)
#define MAX_SHARES		(1UL << 18)
#endif

typedef int (*tg_visitor)(struct task_group *, void *);

extern int walk_tg_tree_from(struct task_group *from,
			     tg_visitor down, tg_visitor up, void *data);

/*
 * Iterate the full tree, calling @down when first entering a node and @up when
 * leaving it for the final time.
 *
 * Caller must hold rcu_lock or sufficient equivalent.
 */
/* 深度优先遍历完整 task_group 树；调用者持 RCU 或等价生命周期保护。 */
/*
 * 完整 task_group 树按深度优先遍历：首次进入节点调用 @down，最终离开调用 @up；调用者必须
 * 持 RCU 读锁或提供等价生命周期保护，避免并发组销毁释放节点。
 *
 * 业务背景：walk_tg_tree() 从 root_task_group 启动通用上下行访问，供层级配置与统计传播。
 * 入参：down/up 是借用回调，可接收节点与 data；data 是透明借用上下文，三者 ownership 均不变。
 * 出参/返回：返回 walk_tg_tree_from() 的 0 或回调错误码；可能通过回调修改外部状态。
 * 注意事项：回调睡眠能力取决于调用者保护方式；持 RCU 时不得睡眠，错误会中止遍历并原样上返。
 */
static inline int walk_tg_tree(tg_visitor down, tg_visitor up, void *data)
{
	return walk_tg_tree_from(&root_task_group, down, up, data);
}

/*
 * 业务背景：css_tg() 把 cgroup 调度子系统的 css 嵌入成员还原为所属 task_group。
 * 入参：css 是借用指针，可为 NULL；函数不增加 css/task_group 引用。
 * 出参/返回：非空时返回 container_of 得到的 task_group，空时返回 NULL；无副作用。
 * 注意事项：返回指针只在调用者的 cgroup/RCU 生命周期保护内有效；helper 不加锁且不睡眠。
 */
static inline struct task_group *css_tg(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct task_group, css) : NULL;
}

extern int tg_nop(struct task_group *tg, void *data);

#ifdef CONFIG_FAIR_GROUP_SCHED
/* CFS 组对象按 alloc -> online -> unregister -> free 顺序经历构造、发布、摘除和释放。 */
extern void free_fair_sched_group(struct task_group *tg);
extern int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent);
extern void online_fair_sched_group(struct task_group *tg);
extern void unregister_fair_sched_group(struct task_group *tg);
#else /* !CONFIG_FAIR_GROUP_SCHED: */
/* 该分支对应未启用公平组调度的构建。 */
/*
 * 业务背景：以下 stub 在未编译 CFS 组调度时保持公共调用点可编译，实际没有 per-group CFS 资源。
 * 入参：tg、parent 均是借用参数，仅为接口兼容；不读取、不持有。
 * 出参/返回：free/online/unregister 为 void 无副作用，alloc 固定返回 1 表示无需分配亦可继续。
 * 注意事项：配置关闭分支不加锁、不睡眠，调用者不得据此假设存在组级 cfs_rq。
 */
static inline void free_fair_sched_group(struct task_group *tg) { }
/* alloc 的固定成功值只表示没有分配工作，不能据此解引用 tg->cfs_rq。 */
/*
 * 业务背景：alloc_fair_sched_group() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent)
{
       return 1;
}
/* online/unregister stub 不发布也不摘除任何 per-group CFS 对象。 */
/*
 * 业务背景：online_fair_sched_group() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void online_fair_sched_group(struct task_group *tg) { }
/*
 * 业务背景：unregister_fair_sched_group() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void unregister_fair_sched_group(struct task_group *tg) { }
#endif /* !CONFIG_FAIR_GROUP_SCHED */
/* 公平组调度条件定义到此结束。 */
/* 至此完成 FAIR_GROUP 的实现/stub 二选一。 */

extern void init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
			struct sched_entity *se, int cpu,
			struct sched_entity *parent);
/* init_cfs_bandwidth() 建立子组配额池并借用 parent 约束，须在组发布前完成。 */
extern void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b, struct cfs_bandwidth *parent);

/* CFS 带宽接口在 cfs_b 锁/rq 锁协议下补充预算、启动 timer 并重新发布被节流队列。 */
extern void __refill_cfs_bandwidth_runtime(struct cfs_bandwidth *cfs_b);
extern void start_cfs_bandwidth(struct cfs_bandwidth *cfs_b);
extern void unthrottle_cfs_rq(struct cfs_rq *cfs_rq);
extern bool cfs_task_bw_constrained(struct task_struct *p);

/* RT 组初始化和 sysctl/cgroup 设置接口按微秒配置组周期预算，并在 attach 前做准入校验。 */
extern void init_tg_rt_entry(struct task_group *tg, struct rt_rq *rt_rq,
		struct sched_rt_entity *rt_se, int cpu,
		struct sched_rt_entity *parent);
extern int sched_group_set_rt_runtime(struct task_group *tg, long rt_runtime_us);
extern int sched_group_set_rt_period(struct task_group *tg, u64 rt_period_us);
extern long sched_group_rt_runtime(struct task_group *tg);
extern long sched_group_rt_period(struct task_group *tg);
extern int sched_rt_can_attach(struct task_group *tg, struct task_struct *tsk);

/* task_group 生命周期接口把新组接入层级，销毁时先摘除可见性、后经 RCU/引用完成释放。 */
extern struct task_group *sched_create_group(struct task_group *parent);
extern void sched_online_group(struct task_group *tg,
			       struct task_group *parent);
extern void sched_destroy_group(struct task_group *tg);
extern void sched_release_group(struct task_group *tg);

/* sched_move_task() 在 pi_lock + rq 锁下提交任务的新组归属，for_autogroup 区分触发来源。 */
extern void sched_move_task(struct task_struct *tsk, bool for_autogroup);

#ifdef CONFIG_FAIR_GROUP_SCHED
extern int sched_group_set_shares(struct task_group *tg, unsigned long shares);

extern int sched_group_set_idle(struct task_group *tg, long idle);

extern void set_task_rq_fair(struct sched_entity *se,
			     struct cfs_rq *prev, struct cfs_rq *next);
#else /* !CONFIG_FAIR_GROUP_SCHED: */
/*
 * 业务背景：组权重支持关闭时，这两个设置接口退化为成功 no-op，以维持 cgroup 调用链。
 * 入参：tg 为借用组指针，shares/idle 是被忽略的请求值；无 ownership 变化。
 * 出参/返回：固定返回 0，表示无需应用；不修改任何状态。
 * 注意事项：只存在于配置关闭分支，不加锁、不睡眠，不能把返回 0 解释为已建立组级权重状态。
 */
static inline int sched_group_set_shares(struct task_group *tg, unsigned long shares) { return 0; }
/*
 * 业务背景：sched_group_set_idle() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int sched_group_set_idle(struct task_group *tg, long idle) { return 0; }
#endif /* !CONFIG_FAIR_GROUP_SCHED */
/* 至此完成组权重设置接口的实现/stub 二选一。 */

#else /* !CONFIG_CGROUP_SCHED: */
/* 以下分支在完全关闭 cgroup 调度时只保留最小兼容类型和查询 stub。 */

struct cfs_bandwidth { };

/*
 * 业务背景：cgroup 调度关闭时不存在任务组带宽，所有任务都视为不受组 CFS quota 约束。
 * 入参：p 是未使用的借用 task 指针；不取得引用。
 * 出参/返回：固定返回 false；无副作用。
 * 注意事项：这是编译期 stub，不代表启用 cgroup 的系统上瞬时未节流；函数不睡眠。
 */
static inline bool cfs_task_bw_constrained(struct task_struct *p) { return false; }

#endif /* !CONFIG_CGROUP_SCHED */
/* 至此结束 cgroup 调度配置分支。 */

extern void unregister_rt_sched_group(struct task_group *tg);
extern void free_rt_sched_group(struct task_group *tg);
extern int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent);

/*
 * u64_u32_load/u64_u32_store
 *
 * Use a copy of a u64 value to protect against data race. This is only
 * applicable for 32-bits architectures.
 */
/*
 * 32 位机器不能原子读取 u64，因而维护 var/copy 双份：写侧先写 var，经 wmb 后写 copy；
 * 读侧先取 copy，经 rmb 后取 var，二者不等便重试。匹配值证明读者未跨越一次撕裂写。
 * 64 位机器直接访问原值；这些宏只解决单个 u64 的数据竞争，不替代周边字段的一致性锁。
 */
#ifdef CONFIG_64BIT
# define u64_u32_load_copy(var, copy)		var
# define u64_u32_store_copy(var, copy, val)	(var = val)
#else
# define u64_u32_load_copy(var, copy)					\
({									\
	u64 __val, __val_copy;						\
	do {								\
		__val_copy = copy;					\
		/*							\
		 * paired with u64_u32_store_copy(), ordering access	\
		 * to var and copy.					\
		 */							\
		/* 与写宏配对排序两个副本，失配时重试撕裂快照。 */ \
		smp_rmb();						\
		__val = var;						\
	} while (__val != __val_copy);					\
	__val;								\
})
# define u64_u32_store_copy(var, copy, val)				\
do {									\
	typeof(val) __val = (val);					\
	var = __val;							\
	/*								\
	 * paired with u64_u32_load_copy(), ordering access to var and	\
	 * copy.							\
	 */								\
	/* 与读宏配对，确保 var 先于 copy 对其他 CPU 可见。 */ \
	smp_wmb();							\
	copy = __val;							\
} while (0)
#endif
# define u64_u32_load(var)		u64_u32_load_copy(var, var##_copy)
# define u64_u32_store(var, val)	u64_u32_store_copy(var, var##_copy, val)

/* rq 解锁前串行执行的延迟均衡回调；next 由 rq 锁保护。 */
struct balance_callback {
	struct balance_callback *next;
	void (*func)(struct rq *rq);
};

/* Fair scheduling SCHED_{NORMAL,BATCH,IDLE} related fields in a runqueue: */
/* 下面是运行队列中供 SCHED_NORMAL/BATCH/IDLE 公平调度使用的字段集合。 */
/*
 * CFS 的 per-CPU（或 per-group/per-CPU）队列：tasks_timeline 按虚拟时间排序，
 * curr 仍计入层级负载；removed 子结构另有锁，允许跨 CPU 移除 PELT 贡献。
 * 组调度下 rq/tg 是借用指针，task_group 生命周期覆盖该 cfs_rq。
 */
struct cfs_rq {
	/* load 是队列实体权重总和；三个 nr 字段分别统计直接、层级 runnable 与 idle 贡献。 */
	struct load_weight	load;
	unsigned int		nr_queued;
	unsigned int		h_nr_queued;		/* SCHED_{NORMAL,BATCH,IDLE} */
	/* h_nr_queued 是三种公平策略在整个层级中的已排队实体数。 */
	unsigned int		h_nr_runnable;		/* SCHED_{NORMAL,BATCH,IDLE} */
	/* h_nr_runnable 是三种公平策略中实际 runnable 的层级总数。 */
	unsigned int		h_nr_idle;		/* SCHED_IDLE */
	/* h_nr_idle 单独统计 SCHED_IDLE 实体，供组 idle 权重与均衡判断。 */

	/* EEVDF 聚合量用于维护平均虚拟运行时间与 eligibility；更新必须与树操作同在 rq 锁下。 */
	s64			sum_w_vruntime;
	u64			sum_weight;
	u64			zero_vruntime;
	unsigned int		sum_shift;

#ifdef CONFIG_SCHED_CORE
	/* force-idle 独立基线避免安全 cookie 导致的空转污染普通公平虚拟时间。 */
	unsigned int		forceidle_seq;
	u64			zero_vruntime_fi;
#endif

	struct rb_root_cached	tasks_timeline;

	/*
	 * 'curr' points to the currently running entity on this cfs_rq.
	 * It is set to NULL otherwise (i.e when none are currently running).
	 */
	/* curr 指向该 cfs_rq 当前正在运行的实体；没有运行实体时必须为 NULL，next 是选取提示而非所有权引用。 */
	struct sched_entity	*curr;
	struct sched_entity	*next;

	/*
	 * CFS load tracking
	 */
	/* CFS 的 PELT 负载跟踪；32 位副本用于避免 last_update_time 的撕裂读取。 */
	struct sched_avg	avg;
#ifndef CONFIG_64BIT
	u64			last_update_time_copy;
#endif
	struct {
		/* removed.lock 允许迁移方把已移除实体的 PELT 贡献暂存，拥有队列随后一次性折叠。 */
		raw_spinlock_t	lock ____cacheline_aligned;
		int		nr;
		unsigned long	load_avg;
		unsigned long	util_avg;
		unsigned long	runnable_avg;
	} removed;

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* 以下缓存把子组负载向父层传播；时间戳与贡献必须成组更新，不能孤立修正某一字段。 */
	u64			last_update_tg_load_avg;
	unsigned long		tg_load_avg_contrib;
	long			propagate;
	long			prop_runnable_sum;

	/*
	 *   h_load = weight * f(tg)
	 *
	 * Where f(tg) is the recursive weight fraction assigned to
	 * this group.
	 */
	/* h_load 是本组权重乘以沿父层级递归得到的份额比例，缓存需随层级负载传播保持一致。 */
	unsigned long		h_load;
	u64			last_h_load_update;
	struct sched_entity	*h_load_next;

	struct rq		*rq;	/* CPU runqueue to which this cfs_rq is attached */
	/* rq 是本 cfs_rq 附着的顶层 CPU 运行队列借用指针。 */

	/*
	 * leaf cfs_rqs are those that hold tasks (lowest schedulable entity in
	 * a hierarchy). Non-leaf lrqs hold other higher schedulable entities
	 * (like users, containers etc.)
	 *
	 * leaf_cfs_rq_list ties together list of leaf cfs_rq's in a CPU.
	 * This list is used during load balance.
	 */
	/*
	 * 叶 cfs_rq 直接容纳任务，非叶队列容纳用户/容器等更高层实体；每 CPU 的 leaf_cfs_rq_list
	 * 只串联叶队列，供负载均衡扫描。on_list 与链表成员必须在 rq 锁下同步改变。
	 */
	int			on_list;
	struct list_head	leaf_cfs_rq_list;
	struct task_group	*tg;	/* Group that "owns" this runqueue */
	/* tg 是拥有该子队列的任务组，生命周期覆盖本 cfs_rq。 */

	/* Locally cached copy of our task_group's idle value */
	/* 缓存 task_group 的 idle 值，避免热路径跨对象读取；配置更新必须同步刷新。 */
	int			idle;

# ifdef CONFIG_CFS_BANDWIDTH
	/* runtime_remaining 可为负表示欠费；throttled 状态决定本队列是否从父层可运行集合摘除。 */
	int			runtime_enabled;
	s64			runtime_remaining;

	u64			throttled_pelt_idle;
#  ifndef CONFIG_64BIT
	u64                     throttled_pelt_idle_copy;
#  endif
	/* throttled_clock* 分别冻结 rq、PELT 与自身时间轴，解节流时据差值补齐而不虚增利用率。 */
	u64			throttled_clock;
	u64			throttled_clock_pelt;
	u64			throttled_clock_pelt_time;
	u64			throttled_clock_self;
	u64			throttled_clock_self_time;
	/* throttled 是层级可运行状态，pelt_clock_throttled 防止节流区间继续衰减错误时间轴。 */
	bool			throttled:1;
	bool			pelt_clock_throttled:1;
	int			throttle_count;
	/* 三条链分别服务普通节流、跨 CPU 回调和层级 limbo 收尾，均不拥有 cfs_rq 内存。 */
	struct list_head	throttled_list;
	struct list_head	throttled_csd_list;
	struct list_head        throttled_limbo_list;
# endif /* CONFIG_CFS_BANDWIDTH */
/* 至此结束 cfs_rq 的带宽节流字段。 */
#endif /* CONFIG_FAIR_GROUP_SCHED */
/* 至此结束 cfs_rq 的组调度层级字段。 */
};

#ifdef CONFIG_SCHED_CLASS_EXT
/* scx_rq->flags, protected by the rq lock */
/* SCX rq 状态在 rq 锁下变化，wakeup/balance 位还限定 BPF helper 上下文。 */
/* scx_rq->flags 由 rq 锁保护；wakeup/balance 位还限定 BPF helper 的合法调用上下文。 */
enum scx_rq_flags {
	/*
	 * A hotplugged CPU starts scheduling before rq_online_scx(). Track
	 * ops.cpu_on/offline() state so that ops.enqueue/dispatch() are called
	 * only while the BPF scheduler considers the CPU to be online.
	 */
	/*
	 * 热插 CPU 在 rq_online_scx() 之前已可能开始调度；ONLINE 位跟踪 BPF ops.cpu_on/offline()
	 * 所见状态，确保仅在 BPF 调度器也认为 CPU 在线时调用 ops.enqueue/dispatch()。
	 */
	SCX_RQ_ONLINE		= 1 << 0,
	/* 当前 BPF 调度状态允许停 tick，NO_HZ 路径据此保留无周期模式。 */
	SCX_RQ_CAN_STOP_TICK	= 1 << 1,
	/* balance 已决定继续运行 current，避免 dispatch 再选另一个任务。 */
	SCX_RQ_BAL_KEEP		= 1 << 3, /* balance decided to keep current */
	/* clock 已由 scx_rq_clock_update() 以 release 语义发布，可供 BPF helper 读取。 */
	SCX_RQ_CLK_VALID	= 1 << 5, /* RQ clock is fresh and valid */
	/* dispatch 完成后必须排入 balance callback，把锁内请求延迟到规定边界执行。 */
	SCX_RQ_BAL_CB_PENDING	= 1 << 6, /* must queue a cb after dispatching */

	/* 两个上下文位限制只允许在 wakeup 或 balance 回调期间调用的 SCX 操作。 */
	SCX_RQ_IN_WAKEUP	= 1 << 16,
	SCX_RQ_IN_BALANCE	= 1 << 17,
};

/* SCX 的 per-CPU 队列和异步 kick/reenqueue 状态；本地 DSQ 所有权归该 rq。 */
struct scx_rq {
	/* local_dsq 拥有本 CPU 本地派发序列；runnable_list 仅索引当前 rq 上可运行 SCX task。 */
	struct scx_dispatch_q	local_dsq;
	struct list_head	runnable_list;		/* runnable tasks on this rq */
	/* runnable_list 串联本 rq 上可运行的 SCX 任务，不拥有 task 引用。 */
	struct list_head	ddsp_deferred_locals;	/* deferred ddsps from enq */
	/* ddsp_deferred_locals 保存 enqueue 阶段暂不能立即提交的本地 dispatch。 */
	unsigned long		ops_qseq;
	u64			extra_enq_flags;	/* see move_task_to_local_dsq() */
	/* extra_enq_flags 传给 move_task_to_local_dsq()，补充本轮入队语义。 */
	/* nr_running 与 cpuperf_target 是 BPF 可观察的队列负载和归一化性能请求。 */
	u32			nr_running;
	u32			cpuperf_target;		/* [0, SCHED_CAPACITY_SCALE] */
	/* cpuperf_target 取值 0 到 SCHED_CAPACITY_SCALE，表示归一化性能目标。 */
	bool			in_select_cpu;
	bool			cpu_released;
	/* flags 由 rq 锁保护；nr_immed 统计本地 DSQ 中立即派发实体，clock 仅在 VALID 时可读。 */
	u32			flags;
	u32			nr_immed;		/* ENQ_IMMED tasks on local_dsq */
	/* nr_immed 统计 local_dsq 中携带 ENQ_IMMED 的任务数。 */
	u64			clock;			/* current per-rq clock -- see scx_bpf_now() */
	/* clock 是 scx_bpf_now() 使用的当前 rq 时钟，仅在 VALID 位成立时新鲜。 */
	cpumask_var_t		cpus_to_kick;
	cpumask_var_t		cpus_to_kick_if_idle;
	/* kick/preempt/wait/sync mask 分组累积跨 CPU 动作，irq_work 批量发送以减少 IPI。 */
	cpumask_var_t		cpus_to_preempt;
	cpumask_var_t		cpus_to_wait;
	cpumask_var_t		cpus_to_sync;
	bool			kick_sync_pending;
	unsigned long		kick_sync;

	/* sub_dispatch_prev 记录嵌套 dispatch 的前一任务，只在对应 rq 临界阶段有效。 */
	struct task_struct	*sub_dispatch_prev;

	/* deferred_reenq_lock 保护锁外提交的重入队请求，irq_work 把它们带回安全执行上下文。 */
	raw_spinlock_t		deferred_reenq_lock;
	u64			deferred_reenq_locals_seq;
	struct list_head	deferred_reenq_locals;	/* scheds requesting reenq of local DSQ */
	/* 该链保存请求重入队本地 DSQ 的调度器实例。 */
	struct list_head	deferred_reenq_users;	/* user DSQs requesting reenq */
	/* 该链保存请求重入队的用户 DSQ。 */
	struct balance_callback	deferred_bal_cb;
	struct balance_callback	kick_sync_bal_cb;
	struct irq_work		deferred_irq_work;
	struct irq_work		kick_cpus_irq_work;
};
#endif /* CONFIG_SCHED_CLASS_EXT */


/*
 * 业务背景：rt_bandwidth_enabled() 判断 RT 周期预算机制是否启用，供运行时扣减与 timer 路径快判。
 * 入参：无。
 * 出参/返回：全局 rt_runtime 非负返回真，负数表示无限制；无副作用。
 * 注意事项：只是配置快照，不串行化后续扣费；函数不加锁且不睡眠。
 */
static inline int rt_bandwidth_enabled(void)
{
	return sysctl_sched_rt_runtime >= 0;
}

/* RT IPI pull logic requires IRQ_WORK */
/* RT 的 IPI 拉取逻辑依赖 IRQ_WORK；同时启用 SMP 才定义能力宏。 */
#if defined(CONFIG_IRQ_WORK) && defined(CONFIG_SMP)
# define HAVE_RT_PUSH_IPI
#endif

/* Real-Time classes' related field in a runqueue: */
/* 以下字段组成 RT 调度类在每 CPU/任务组运行队列中的状态。 */
/* RT per-CPU/组队列：active 与 pushable 分属执行顺序和 SMP 迁移候选。 */
struct rt_rq {
	/* active 按 RT 优先级保存 runnable 实体；计数区分全部 RT 与 RR 子集。 */
	struct rt_prio_array	active;
	unsigned int		rt_nr_running;
	unsigned int		rr_nr_running;
	/* curr/next 缓存当前最高与次高优先级，避免每次迁移判断重扫 bitmap。 */
	struct {
		int		curr; /* highest queued rt task prio */
		/* curr 是当前已排队 RT 任务中的最高优先级数值。 */
		int		next; /* next highest */
		/* next 是除当前候选外的次高优先级，用于迁移比较。 */
	} highest_prio;
	/* overloaded 与 pushable_tasks 是 SMP 迁移提示；锁定候选后必须复验亲和性和运行状态。 */
	bool			overloaded;
	struct plist_head	pushable_tasks;

	/* rt_queued 表示本层实体已向父 rq 发布；它与 rt_nr_running 不可单独解释。 */
	int			rt_queued;

#ifdef CONFIG_RT_GROUP_SCHED
	/* throttled 时该层队列虽有实体也不可向父层提供运行候选。 */
	int			rt_throttled;
	u64			rt_time; /* consumed RT time, goes up in update_curr_rt */
	/* rt_time 是 update_curr_rt() 累加的已消费 RT 时间。 */
	u64			rt_runtime; /* allotted RT time, "slice" from rt_bandwidth, RT sharing/balancing */
	/* rt_runtime 是从 rt_bandwidth 分得的可用时间片，参与组内共享与均衡。 */
	/* Nests inside the rq lock: */
	/* runtime_lock 嵌套于 rq 锁，保护本组已消费/获配的 RT runtime，禁止反序获取。 */
	raw_spinlock_t		rt_runtime_lock;

	unsigned int		rt_nr_boosted;

	/* rq 恒指向顶层 CPU rq；tg 指明拥有本 per-group rt_rq 的任务组。 */
	struct rq		*rq; /* this is always top-level rq, cache? */
	/* rq 始终指向顶层 CPU rq；原注释保留了它是否只是缓存的疑问语气。 */
#endif
#ifdef CONFIG_CGROUP_SCHED
	struct task_group	*tg; /* this tg has "this" rt_rq on given CPU for runnable entities */
	/* tg 拥有该 CPU 上供 runnable RT 实体使用的本 rt_rq。 */
#endif
};

/*
 * 业务背景：rt_rq_is_runnable() 同时检查队列已挂入层级且确有 RT 实体，避免仅凭一个缓存误判。
 * 入参：rt_rq 是调用者借用的非空队列，通常已由 rq 锁稳定；ownership 不变。
 * 出参/返回：rt_queued 与 rt_nr_running 均非零返回真，否则返回假；无副作用。
 * 注意事项：无锁调用只得到瞬时提示；函数不加锁、不睡眠，不能替代入队/节流状态复验。
 */
static inline bool rt_rq_is_runnable(struct rt_rq *rt_rq)
{
	return rt_rq->rt_queued && rt_rq->rt_nr_running;
}

/* Deadline class' related fields in a runqueue */
/* 以下字段组成 deadline 调度类的每 rq 状态。 */
/* DL per-CPU 队列：root 是 EDF 执行树，pushable 树只含可迁移非当前任务。 */
struct dl_rq {
	/* runqueue is an rbtree, ordered by deadline */
	/* 主运行树按绝对 deadline 排序并缓存最左节点，所有结构修改由 rq 锁保护。 */
	struct rb_root_cached	root;

	unsigned int		dl_nr_running;

	/*
	 * Deadline values of the currently executing and the
	 * earliest ready task on this rq. Caching these facilitates
	 * the decision whether or not a ready but not running task
	 * should migrate somewhere else.
	 */
	/* 缓存当前执行实体与最早就绪实体的 deadline，用于判断非当前任务是否值得向外迁移。 */
	struct {
		u64		curr;
		u64		next;
	} earliest_dl;

	bool			overloaded;

	struct sched_dl_entity	*curr;
	/*
	 * Tasks on this rq that can be pushed away. They are kept in
	 * an rb-tree, ordered by tasks' deadlines, with caching
	 * of the leftmost (earliest deadline) element.
	 */
	/* 可被推走的非当前任务按 deadline 建红黑树并缓存最早节点，迁移后必须同步摘除。 */
	struct rb_root_cached	pushable_dl_tasks_root;

	/*
	 * "Active utilization" for this runqueue: increased when a
	 * task wakes up (becomes TASK_RUNNING) and decreased when a
	 * task blocks
	 */
	/* 活跃利用率：任务唤醒成为 TASK_RUNNING 时增加，阻塞时减少，描述当前会竞争 CPU 的预算。 */
	u64			running_bw;

	/*
	 * Utilization of the tasks "assigned" to this runqueue (including
	 * the tasks that are in runqueue and the tasks that executed on this
	 * CPU and blocked). Increased when a task moves to this runqueue, and
	 * decreased when the task moves away (migrates, changes scheduling
	 * policy, or terminates).
	 * This is needed to compute the "inactive utilization" for the
	 * runqueue (inactive utilization = this_bw - running_bw).
	 */
	/*
	 * this_bw 统计分配给此 rq 的任务预算，包括队列内任务及曾在本 CPU 执行后阻塞的任务；
	 * 迁入时增加，迁出、改策略或退出时减少，因此 inactive utilization = this_bw - running_bw。
	 */
	u64			this_bw;
	u64			extra_bw;

	/*
	 * Maximum available bandwidth for reclaiming by SCHED_FLAG_RECLAIM
	 * tasks of this rq. Used in calculation of reclaimable bandwidth(GRUB).
	 */
	/* 本 rq 中带 RECLAIM 标志任务可回收的最大带宽，GRUB 用它限制跨任务借用。 */
	u64			max_bw;

	/*
	 * Inverse of the fraction of CPU utilization that can be reclaimed
	 * by the GRUB algorithm.
	 */
	/* GRUB 可回收 CPU 利用率比例的倒数缓存，须与 max_bw/准入记账同步更新。 */
	u64			bw_ratio;
};

#ifdef CONFIG_FAIR_GROUP_SCHED
/* Check whether a task group is root tg */
/* 判断任务组是否为全局根组；根组没有父级实体。 */
#define is_root_task_group(tg) ((tg) == &root_task_group)
/* An entity is a task if it doesn't "own" a runqueue */
/* 不拥有子 cfs_rq 的 sched_entity 才是叶子任务实体。 */
#define entity_is_task(se)	(!se->my_q)

/*
 * 业务背景：se_update_runnable() 把组实体的 runnable_weight 同步为其子 cfs_rq 的层级 runnable 数。
 * 入参：se 为借用的非空输入输出实体；调用者持 rq 锁，ownership 不变。
 * 出参/返回：void；组实体更新权重，任务实体保持不变。
 * 注意事项：仅组调度分支有效，不睡眠；漏更新会令父层负载传播使用陈旧值。
 */
static inline void se_update_runnable(struct sched_entity *se)
{
	if (!entity_is_task(se))
		se->runnable_weight = se->my_q->h_nr_runnable;
}

/*
 * 业务背景：se_runnable() 统一返回任务或组实体对父 cfs_rq 的 runnable 贡献。
 * 入参：se 为借用的非空只读实体，通常由 rq 锁稳定。
 * 出参/返回：延迟出队实体返回 0；任务返回 on_rq 布尔值，组返回 runnable_weight；无副作用。
 * 注意事项：返回单位因实体类型不同，只用于可运行性/层级计数；函数不睡眠。
 */
static inline long se_runnable(struct sched_entity *se)
{
	if (se->sched_delayed)
		/* 延迟出队实体仍可能留在树中，但对父层 runnable 贡献必须为零。 */
		return false;

	/* 叶任务使用 on_rq；组实体使用已从子队列同步的层级权重。 */
	if (entity_is_task(se))
		return !!se->on_rq;
	else
		return se->runnable_weight;
}

#else /* !CONFIG_FAIR_GROUP_SCHED: */
/* 此处切换到未启用公平组调度时的实体 helper。 */

#define entity_is_task(se)	1

/* 组调度关闭时无需同步子队列 runnable 权重；参数是借用占位，void 返回且无副作用。 */
/*
 * 业务背景：se_update_runnable() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void se_update_runnable(struct sched_entity *se) { }

/*
 * 业务背景：无组调度时 se_runnable() 只需判断任务实体是否已入 rq，延迟出队仍按不可运行处理。
 * 入参：se 为借用的非空实体；ownership 不变。
 * 出参/返回：sched_delayed 时返回 0，否则返回 on_rq 布尔值；无副作用。
 * 注意事项：调用者负责锁定状态，函数不加锁且不睡眠。
 */
static inline long se_runnable(struct sched_entity *se)
{
	if (se->sched_delayed)
		return false;

	return !!se->on_rq;
}

#endif /* !CONFIG_FAIR_GROUP_SCHED */
/* 实体 runnable helper 的公平组调度条件分支到此结束。 */

/*
 * XXX we want to get rid of these helpers and use the full load resolution.
 */
/* FIXME：期望删除这些降精度 helper，直接使用全精度 load；当前仍须保持既有数值尺度。 */
/*
 * 业务背景：se_weight() 把实体内部高分辨率 load.weight 降为普通计算尺度。
 * 入参：se 为借用的非空只读实体；无引用变化。
 * 出参/返回：返回 scale_load_down() 后的权重；无副作用。
 * 注意事项：低权重存在钳位语义，不能与原始 load.weight 混用；函数不睡眠。
 */
static inline long se_weight(struct sched_entity *se)
{
	return scale_load_down(se->load.weight);
}


/*
 * 业务背景：sched_asym_prefer() 在非对称 CPU 间选择体系结构优先级更高者。
 * 入参：a、b 是有效 CPU 编号，纯输入。
 * 出参/返回：a 的体系结构优先级严格高于 b 返回真；相等返回假，无副作用。
 * 注意事项：不验证在线性或容量适配，调用者需在拓扑保护下使用；函数不睡眠。
 */
static inline bool sched_asym_prefer(int a, int b)
{
	return arch_asym_cpu_priority(a) > arch_asym_cpu_priority(b);
}

/* 能耗模型 domain 链由 root_domain 持有并通过 RCU 延迟回收。 */
struct perf_domain {
	struct em_perf_domain *em_pd;
	struct perf_domain *next;
	struct rcu_head rcu;
};

/*
 * We add the notion of a root-domain which will be used to define per-domain
 * variables. Each exclusive cpuset essentially defines an island domain by
 * fully partitioning the member CPUs from any other cpuset. Whenever a new
 * exclusive cpuset is created, we also create and attach a new root-domain
 * object.
 *
 */
/*
 * root_domain 为每个独占 cpuset 建立调度岛，使成员 CPU 与其他岛完全分区；新建独占
 * cpuset 时创建并附着新对象。它承载岛内共享的 RT/DL 准入、过载提示和能耗域索引。
 */
/*
 * 独占 cpuset 的调度岛：span 是成员全集，online 是当前可调度子集；DL/RT
 * 过载 mask 是无锁搜索提示，迁移方锁住目标 rq 后仍须复验。refcount 管所有
 * rq 附着，归零后对象经 RCU 回收，避免 topology 读者看到悬空索引。
 */
struct root_domain {
	atomic_t		refcount;
	atomic_t		rto_count;
	struct rcu_head		rcu;
	cpumask_var_t		span;
	cpumask_var_t		online;

	/*
	 * Indicate pullable load on at least one CPU, e.g:
	 * - More than one runnable task
	 * - Running task is misfit
	 */
	/* 至少一个 CPU 存在可拉取负载，例如多于一个 runnable 任务或当前任务不适配本 CPU。 */
	bool			overloaded;

	/* Indicate one or more CPUs over-utilized (tipping point) */
	/* 指示一个或多个 CPU 已越过能耗感知调度的过度利用临界点。 */
	bool			overutilized;

	/*
	 * The bit corresponding to a CPU gets set here if such CPU has more
	 * than one runnable -deadline task (as it is below for RT tasks).
	 */
	/* CPU 有多个 runnable DL 任务便置位；这是搜索提示，迁移时仍须锁下复验。 */
	cpumask_var_t		dlo_mask;
	atomic_t		dlo_count;
	struct dl_bw		dl_bw;
	struct cpudl		cpudl;

	/*
	 * Indicate whether a root_domain's dl_bw has been checked or
	 * updated. It's monotonously increasing value.
	 *
	 * Also, some corner cases, like 'wrap around' is dangerous, but given
	 * that u64 is 'big enough'. So that shouldn't be a concern.
	 */
	/* dl_bw 每次检查/更新递增 cookie；理论回绕危险，但 u64 空间使现实运行期可忽略。 */
	u64 visit_cookie;

#ifdef HAVE_RT_PUSH_IPI
	/*
	 * For IPI pull requests, loop across the rto_mask.
	 */
	/* RT IPI 拉取请求沿 rto_mask 循环，rto_lock 保护游标，原子字段协调锁外启动。 */
	struct irq_work		rto_push_work;
	raw_spinlock_t		rto_lock;
	/* These are only updated and read within rto_lock */
	/* rto_loop/rto_cpu 只允许在 rto_lock 内读写。 */
	int			rto_loop;
	int			rto_cpu;
	/* These atomics are updated outside of a lock */
	/* next/start 可在锁外更新，使用原子类型避免并发请求丢失。 */
	atomic_t		rto_loop_next;
	atomic_t		rto_loop_start;
#endif /* HAVE_RT_PUSH_IPI */
	/* RT push IPI 专用字段到此结束。 */
	/*
	 * The "RT overload" flag: it gets set if a CPU has more than
	 * one runnable RT task.
	 */
	/* CPU 有多个 runnable RT 任务时设置过载位，供其他 CPU 发起拉取。 */
	cpumask_var_t		rto_mask;
	struct cpupri		cpupri;

	/*
	 * NULL-terminated list of performance domains intersecting with the
	 * CPUs of the rd. Protected by RCU.
	 */
	/* 与本 rd CPU 相交的 NULL 结尾能耗域链；发布、替换与回收受 RCU 保护。 */
	struct perf_domain __rcu *pd;
};

extern void init_defrootdomain(void);
extern int sched_init_domains(const struct cpumask *cpu_map);
extern void rq_attach_root(struct rq *rq, struct root_domain *rd);
extern void sched_get_rd(struct root_domain *rd);
extern void sched_put_rd(struct root_domain *rd);

/*
 * 业务背景：get_rd_overloaded() 无锁读取调度岛是否存在可拉取负载，供均衡快速跳过空岛。
 * 入参：rd 为借用的非空 root_domain，调用者保证生命周期。
 * 出参/返回：返回 overloaded 的 READ_ONCE 快照；无副作用。
 * 注意事项：提示可能立即过期，真正迁移必须在 rq 锁下复验；函数不睡眠。
 */
static inline int get_rd_overloaded(struct root_domain *rd)
{
	return READ_ONCE(rd->overloaded);
}

/*
 * 业务背景：set_rd_overloaded() 发布 root_domain 过载提示，并避免相同值的无谓写缓存线。
 * 入参：rd 为借用非空对象；status 是要发布的布尔状态，ownership 不变。
 * 出参/返回：void；必要时 WRITE_ONCE 更新 overloaded。
 * 注意事项：READ/WRITE_ONCE 不是锁；调用者维护计数与标志不变量，函数不睡眠。
 */
static inline void set_rd_overloaded(struct root_domain *rd, int status)
{
	if (get_rd_overloaded(rd) != status)
		WRITE_ONCE(rd->overloaded, status);
}

#ifdef HAVE_RT_PUSH_IPI
extern void rto_push_irq_work_func(struct irq_work *work);
#endif

#ifdef CONFIG_UCLAMP_TASK
/*
 * struct uclamp_bucket - Utilization clamp bucket
 * @value: utilization clamp value for tasks on this clamp bucket
 * @tasks: number of RUNNABLE tasks on this clamp bucket
 *
 * Keep track of how many tasks are RUNNABLE for a given utilization
 * clamp value.
 */
/* bucket 统计 runnable 引用，最后一个任务离开时才可撤销该 clamp 值。 */
struct uclamp_bucket {
	unsigned long value : bits_per(SCHED_CAPACITY_SCALE);
	unsigned long tasks : BITS_PER_LONG - bits_per(SCHED_CAPACITY_SCALE);
};

/*
 * struct uclamp_rq - rq's utilization clamp
 * @value: currently active clamp values for a rq
 * @bucket: utilization clamp buckets affecting a rq
 *
 * Keep track of RUNNABLE tasks on a rq to aggregate their clamp values.
 * A clamp value is affecting a rq when there is at least one task RUNNABLE
 * (or actually running) with that value.
 *
 * There are up to UCLAMP_CNT possible different clamp values, currently there
 * are only two: minimum utilization and maximum utilization.
 *
 * All utilization clamping values are MAX aggregated, since:
 * - for util_min: we want to run the CPU at least at the max of the minimum
 *   utilization required by its currently RUNNABLE tasks.
 * - for util_max: we want to allow the CPU to run up to the max of the
 *   maximum utilization allowed by its currently RUNNABLE tasks.
 *
 * Since on each system we expect only a limited number of different
 * utilization clamp values (UCLAMP_BUCKETS), use a simple array to track
 * the metrics required to compute all the per-rq utilization clamp values.
 */
/* rq 的有效 clamp 是非空 bucket 的聚合值，更新受 rq 锁保护。 */
struct uclamp_rq {
	unsigned int value;
	struct uclamp_bucket bucket[UCLAMP_BUCKETS];
};

DECLARE_STATIC_KEY_FALSE(sched_uclamp_used);
#endif /* CONFIG_UCLAMP_TASK */
/* rq 利用率钳制聚合定义到此结束。 */

/*
 * This is the main, per-CPU runqueue data structure.
 *
 * Locking rule: those places that want to lock multiple runqueues
 * (such as the load balancing or the thread migration code), lock
 * acquire operations must be ordered by ascending &runqueue.
 */
/*
 * 每 CPU 主运行队列。__lock 保护调度类队列、curr/donor 切换和大多数计数；
 * 标成 RCU 的指针允许受控无锁观察但不能据此直接修改任务。跨 rq 操作必须按
 * 地址顺序加锁，必要时在放锁后重新读取 donor/curr，CPU 热插拔则额外约束
 * online/active 与 root_domain 的附着时序。
 */
struct rq {
	/*
	 * The following members are loaded together, without holding the
	 * rq->lock, in an extremely hot loop in update_sg_lb_stats()
	 * (called from pick_next_task()). To reduce cache pollution from
	 * this operation, they are placed together on this dedicated cache
	 * line. Even though some of them are frequently modified, they are
	 * loaded much more frequently than they are stored.
	 */
	/*
	 * 该独立 cache line 是负载均衡的无锁采样面：nr_running/NUMA 计数、待处理唤醒和 capacity
	 * 都可能在采样后变化，调用者只能据此筛选候选，选中后须在目标 rq 锁下复验。
	 */
	unsigned int		nr_running;
#ifdef CONFIG_NUMA_BALANCING
	unsigned int		nr_numa_running;
	unsigned int		nr_preferred_running;
#endif
	unsigned int		ttwu_pending;
	unsigned long		cpu_capacity;
#ifdef CONFIG_SCHED_PROXY_EXEC
	/* donor 提供调度优先级，curr 实际占用 CPU；RCU 只延长读侧生命周期，不冻结二者关系。 */
	struct task_struct __rcu	*donor;  /* Scheduling context */
	/* donor 是决定优先级和调度策略的调度上下文。 */
	struct task_struct __rcu	*curr;   /* Execution context */
	/* curr 是当前真正占用处理器执行的上下文。 */
#else
	/* 未启用 proxy execution 时 donor 与 curr 共用存储，调度上下文就是执行上下文。 */
	union {
		struct task_struct __rcu *donor; /* Scheduler context */
		/* donor 保存调度上下文。 */
		struct task_struct __rcu *curr;  /* Execution context */
		/* curr 保存执行上下文。 */
	};
#endif
	struct task_struct	*idle;
	/* padding left here deliberately */
	/* 这里故意留 padding，使下一条 cache line 从高频 rq 锁和切换计数开始，减少无锁采样污染。 */

	/*
	 * The next cacheline holds the (hot) runqueue lock, as well as
	 * some other less performance-critical fields.
	 */
	/* 下一条 cache line 集中主 rq 锁及次热字段，降低前一无锁采样行的争用。 */
	u64			nr_switches	____cacheline_aligned;

	/* runqueue lock: */
	/* __lock 是本 rq 的主状态锁；core scheduling 下 rq_lockp() 可能改为 SMT core 的共享锁。 */
	raw_spinlock_t		__lock;

#ifdef CONFIG_NO_HZ_COMMON
	/* NO_HZ 字段记录 tick 停止、远程 kick 原因和阻塞负载更新时间；nohz_csd 承接跨 CPU 回调。 */
	unsigned int		nohz_tick_stopped;
	atomic_t		nohz_flags;
	unsigned int		has_blocked_load;
	unsigned long		last_blocked_load_update_tick;
	call_single_data_t	nohz_csd;
#endif /* CONFIG_NO_HZ_COMMON */
	/* 通用 NO_HZ 的 rq 状态字段到此结束。 */

#ifdef CONFIG_UCLAMP_TASK
	/* Utilization clamp values based on CPU's RUNNABLE tasks */
	/* runnable 任务的 clamp 以 bucket 聚合；IDLE 位保留最后任务离队后的特殊空闲语义。 */
	struct uclamp_rq	uclamp[UCLAMP_CNT] ____cacheline_aligned;
	unsigned int		uclamp_flags;
#define UCLAMP_FLAG_IDLE 0x01
#endif

	/* 每个调度类的顶层 per-CPU 队列均嵌入 rq，共享同一主锁与时钟。 */
	struct cfs_rq		cfs;
	struct rt_rq		rt;
	struct dl_rq		dl;
#ifdef CONFIG_SCHED_CLASS_EXT
	struct scx_rq		scx;
	struct sched_dl_entity	ext_server;
#endif
#ifdef CONFIG_SCHED_CACHE
	/* LLC 调度缓存的 epoch 锁和计数与普通 rq 热字段隔离，epoch_next 安排下一轮聚合。 */
	raw_spinlock_t		cpu_epoch_lock ____cacheline_aligned;
	u64			cpu_runtime;
	unsigned long		cpu_epoch;
	unsigned long		cpu_epoch_next;
#endif

	/* fair_server 让低优先级公平类借 DL server 获得饥饿保护预算。 */
	struct sched_dl_entity	fair_server;

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* list of leaf cfs_rq on this CPU: */
	/* leaf_cfs_rq_list 是均衡扫描入口；tmp_alone_branch 仅在层级链表重建阶段暂存分支。 */
	struct list_head	leaf_cfs_rq_list;
	struct list_head	*tmp_alone_branch;
#endif /* CONFIG_FAIR_GROUP_SCHED */
	/* 公平组调度的叶队列链字段到此结束。 */

#ifdef CONFIG_NUMA_BALANCING
	/* numa_migrate_on 标记当前 rq 正执行 NUMA 迁移协调，避免重入迁移。 */
	unsigned int		numa_migrate_on;
#endif

#ifdef CONFIG_SCHED_CACHE
	/* 两个计数区分偏好本 LLC 与全部 LLC runnable 任务，供 cache-aware 放置使用。 */
	unsigned int		nr_pref_llc_running;
	unsigned int		nr_llc_running;
#endif

	/*
	 * This is part of a global counter where only the total sum
	 * over all CPUs matters. A task can increase this counter on
	 * one CPU and if it got migrated afterwards it may decrease
	 * it on another CPU. Always updated under the runqueue lock:
	 */
	/*
	 * nr_uninterruptible 是可跨 CPU 加减的分布式总量，单 rq 值允许暂时为负；只有全 CPU 求和
	 * 具有意义。任务阻塞/唤醒各自在当时所属 rq 锁下记账，迁移无需搬运历史贡献。
	 */
	unsigned long		nr_uninterruptible;

	/* 当前 server、stop task、待扫描最高类、下一均衡时刻和延迟释放旧 mm 均由切换路径在 rq 锁下维护。 */
	struct sched_dl_entity	*dl_server;
	struct task_struct	*stop;
	const struct sched_class *next_class;
	unsigned long		next_balance;
	struct mm_struct	*prev_mm;

	/*
	 * The following fields of clock data are frequently referenced
	 * and updated together, and should go on their own cache line.
	 */
	/* clock 是墙上调度时钟，clock_task 排除 steal/IRQ，clock_pelt 服务衰减；flags 审计更新新鲜度。 */
	u64			clock_task ____cacheline_aligned;
	u64			clock_pelt;
	u64			clock;
	unsigned long		lost_idle_time;
	unsigned int		clock_update_flags;
	u64			clock_pelt_idle;
	u64			clock_idle;

#ifndef CONFIG_64BIT
	/* 32 位为两个无锁可读 u64 保存副本，配合 u64_u32_load/store 防止撕裂。 */
	u64			clock_pelt_idle_copy;
	u64			clock_idle_copy;
#endif

	/* resched 延迟监测保存首次观察时刻和连续未响应 tick 数，只用于告警。 */
	u64 last_seen_need_resched_ns;
	int ticks_without_resched;

#ifdef CONFIG_MEMBARRIER
	/* 当前执行 mm 的 membarrier_state 缓存，在 context switch 屏障包围下更新。 */
	int membarrier_state;
#endif

	/* rd 是引用保护的调度岛；sd 经 RCU 替换，读者不得越过其 RCU 临界区保存裸指针。 */
	struct root_domain		*rd;
	struct sched_domain __rcu	*sd;

	/* rq 解锁边界执行的单向回调链，节点由提交者持有到回调被摘除。 */
	struct balance_callback *balance_callback;

	/* nohz_idle_balance/idle_balance 协调空闲 CPU 是否承担全局/普通均衡工作。 */
	unsigned char		nohz_idle_balance;
	unsigned char		idle_balance;

	/* 非零表示当前任务容量需求超过本 CPU，供上层域优先迁移。 */
	unsigned long		misfit_task_load;

	/* For active balancing */
	/* active_balance 通过 stop-machine 风格 cpu_stop_work 把任务从繁忙源 rq 主动推出。 */
	int			active_balance;
	int			push_cpu;
	struct cpu_stop_work	active_balance_work;

	/* CPU of this runqueue: */
	/* cpu 是永久编号；online 是调度器可用状态，热插拔只改变后者。 */
	int			cpu;
	int			online;

	/* cfs_tasks 串联本 CPU 的公平任务，供负载统计/迁移遍历；修改受 rq 锁保护。 */
	struct list_head cfs_tasks;

	/* 各类 PELT 平均值和 idle 时间估计共同驱动容量、能耗与均衡决策。 */
	struct sched_avg	avg_rt;
	struct sched_avg	avg_dl;
#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	struct sched_avg	avg_irq;
#endif
#ifdef CONFIG_SCHED_HW_PRESSURE
	struct sched_avg	avg_hw;
#endif
	u64			idle_stamp;
	u64			avg_idle;

	/* This is used to determine avg_idle's max value */
	/* max_idle_balance_cost 限制空闲均衡预算，避免扫描成本超过预计空闲窗口。 */
	u64			max_idle_balance_cost;

#ifdef CONFIG_HOTPLUG_CPU
	/* CPU 下线等待调度状态收敛时睡在 hotplug_wait，由最后清理路径唤醒。 */
	struct rcuwait		hotplug_wait;
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	/* prev_irq_time 用于差分本 tick IRQ 时间，psi_irq_time 累计压力统计可归责部分。 */
	u64			prev_irq_time;
	u64			psi_irq_time;
#endif
#ifdef CONFIG_PARAVIRT
	/* steal-time 快照把宿主抢占时间从 guest 任务运行时间中扣除。 */
	u64			prev_steal_time;
#endif
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	u64			prev_steal_time_rq;
#endif

	/* calc_load related fields */
	/* 每 rq 的折叠时刻与 active 基线最终汇入全局 calc_load_tasks。 */
	unsigned long		calc_load_update;
	long			calc_load_active;

#ifdef CONFIG_SCHED_HRTICK
	/* hrtick timer 在精确片段边界触发；远程 CPU 通过 csd 请求重编程。 */
	call_single_data_t	hrtick_csd;
	struct hrtimer		hrtick_timer;
	ktime_t			hrtick_time;
	ktime_t			hrtick_delay;
	unsigned int		hrtick_sched;
#endif

#ifdef CONFIG_SCHEDSTATS
	/* latency stats */
	/* 以下字段只在 SCHEDSTATS 下累计观测数据，不参与调度正确性决定。 */
	struct sched_info	rq_sched_info;
	unsigned long long	rq_cpu_time;

	/* sys_sched_yield() stats */
	/* yld_count 统计显式 sched_yield 系统调用次数。 */
	unsigned int		yld_count;

	/* schedule() stats */
	/* sched_count 与 sched_goidle 分别统计调度次数和进入 idle 次数。 */
	unsigned int		sched_count;
	unsigned int		sched_goidle;

	/* try_to_wake_up() stats */
	/* ttwu_count 与 ttwu_local 分别统计全部唤醒尝试及本地唤醒。 */
	unsigned int		ttwu_count;
	unsigned int		ttwu_local;
#endif

#ifdef CONFIG_CPU_IDLE
	/* Must be inspected within a RCU lock section */
	/* idle_state 由 cpuidle 发布，调度器只可在 RCU 读侧检查其退出延迟等属性。 */
	struct cpuidle_state	*idle_state;
#endif

	/* pinned 任务限制 CPU 下线；push_busy 保证异步 push 工作单飞。 */
	unsigned int		nr_pinned;
	unsigned int		push_busy;
	struct cpu_stop_work	push_work;

#ifdef CONFIG_SCHED_CORE
	/* per rq */
	/* per-rq 字段保存本兄弟的选择结果；core 指向共享 cookie 调度主 rq。 */
	struct rq		*core;
	struct task_struct	*core_pick;
	struct sched_dl_entity	*core_dl_server;
	unsigned int		core_enabled;
	unsigned int		core_sched_seq;
	struct rb_root		core_tree;

	/* shared state -- careful with sched_core_cpu_deactivate() */
	/* 共享序列与 cookie 树协调所有 SMT 兄弟；CPU 下线时必须同步清除共同选择。 */
	unsigned int		core_task_seq;
	unsigned int		core_pick_seq;
	unsigned long		core_cookie;
	unsigned int		core_forceidle_count;
	unsigned int		core_forceidle_seq;
	unsigned int		core_forceidle_occupation;
	u64			core_forceidle_start;
#endif /* CONFIG_SCHED_CORE */
	/* core scheduling 的 per-rq 与共享状态字段到此结束。 */

	/* Scratch cpumask to be temporarily used under rq_lock */
	/* scratch_mask 只在持 rq 锁期间借用，任何指针都不得逃逸到放锁后。 */
	cpumask_var_t		scratch_mask;

#ifdef CONFIG_CFS_BANDWIDTH
	/* 解节流的跨 CPU 回调及其链表把配额恢复工作送回拥有 rq。 */
	call_single_data_t	cfsb_csd;
	struct list_head	cfsb_csd_list;
#endif

	/* nr_iowait 可由唤醒/阻塞并发更新，原子量保证计数但不保证任务列表快照一致。 */
	atomic_t		nr_iowait;
} __no_randomize_layout;

#ifdef CONFIG_FAIR_GROUP_SCHED

/* CPU runqueue to which this cfs_rq is attached */
/* 组调度 cfs_rq 显式记录所属 CPU rq；返回借用指针且不提供锁保护。 */
/*
 * 业务背景：rq_of() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline struct rq *rq_of(struct cfs_rq *cfs_rq)
{
	return cfs_rq->rq;
}

#else /* !CONFIG_FAIR_GROUP_SCHED: */

/*
 * 业务背景：无组调度时 cfs_rq 直接嵌在顶层 rq，rq_of() 用 container_of 还原拥有者。
 * 入参：cfs_rq 为调用者借用的非空顶层公平队列；ownership 不变。
 * 出参/返回：返回包含它的 rq 借用指针；无副作用。
 * 注意事项：只适用于该配置布局，稳定访问队列字段仍需 rq 锁；函数不睡眠。
 */
static inline struct rq *rq_of(struct cfs_rq *cfs_rq)
{
	return container_of(cfs_rq, struct rq, cfs);
}
#endif /* !CONFIG_FAIR_GROUP_SCHED */

/* rq 的 CPU 归属在初始化后稳定，热插拔不会改变编号。 */
/*
 * 业务背景：cpu_of() 把 rq 映射到初始化时绑定的永久 CPU 编号，供 per-CPU 索引和拓扑查找。
 * 入参：rq 为借用非空队列；不取得引用。
 * 出参/返回：返回 [0, nr_cpu_ids) 的 CPU 编号；无副作用。
 * 注意事项：热插拔只改变 online，不改变编号；函数不加锁且不睡眠。
 */
static inline int cpu_of(struct rq *rq)
{
	return rq->cpu;
}

#define MDF_PUSH		0x01

/*
 * 业务背景：is_migration_disabled() 快判任务是否位于 migrate_disable() 约束区。
 * 入参：p 为借用非空 task；ownership 不变。
 * 出参/返回：返回嵌套计数的布尔解释；无副作用。
 * 注意事项：无锁快照可能变化，真正迁移须在 task/rq 锁下复验；函数不睡眠。
 */
static inline bool is_migration_disabled(struct task_struct *p)
{
	return p->migration_disabled;
}

DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);
DECLARE_PER_CPU(struct rnd_state, sched_rnd_state);

/*
 * 业务背景：sched_rng() 从当前 CPU 私有 PRNG 状态生成调度随机数，避免共享状态争用。
 * 入参：无。
 * 出参/返回：返回 u32 伪随机值并推进本 CPU rnd_state；无引用变化。
 * 注意事项：调用者必须处于不可迁移上下文以保持 this_cpu 指针稳定；不提供安全随机性且不睡眠。
 */
static inline u32 sched_rng(void)
{
	return prandom_u32_state(this_cpu_ptr(&sched_rnd_state));
}

/*
 * 业务背景：__this_rq() 取得当前 CPU 的主运行队列，是 this_rq() 宏的类型安全实现。
 * 入参：无。
 * 出参/返回：返回当前 CPU rq 的永久借用指针；无副作用。
 * 注意事项：调用者须禁抢占/关 IRQ或已持相应本地约束，避免取指针后迁移；不睡眠。
 */
static __always_inline struct rq *__this_rq(void)
{
	return this_cpu_ptr(&runqueues);
}

#define cpu_rq(cpu)		(&per_cpu(runqueues, (cpu)))
#define this_rq()		__this_rq()
#define task_rq(p)		cpu_rq(task_cpu(p))
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)
#define raw_rq()		raw_cpu_ptr(&runqueues)

/* idle task、零 runnable 且无待处理远程唤醒三者同时满足才是真空闲。 */
/*
 * 业务背景：idle_rq() 同时排除正在执行普通任务、已入队任务和待处理远程唤醒，提供严格空闲提示。
 * 入参：rq 为借用非空队列；不取得引用。
 * 出参/返回：三项条件都满足返回真，否则返回假；无副作用。
 * 注意事项：无锁结果可立即过期，放置任务前必须遵循目标 rq 锁协议；函数不睡眠。
 */
static inline bool idle_rq(struct rq *rq)
{
	return rq->curr == rq->idle && !rq->nr_running && !rq->ttwu_pending;
}

/**
 * available_idle_cpu - is a given CPU idle for enqueuing work.
 * @cpu: the CPU in question.
 *
 * Return: 1 if the CPU is currently idle. 0 otherwise.
 */
/* 虚拟 CPU 被宿主抢占时即使 rq idle 也不可视为立即可用。 */
/*
 * 当前 CPU 空闲且虚拟 CPU 未被宿主抢占时才适合立即入队工作；前者排除本地 runnable/ttwu，
 * 后者避免把任务放到暂时得不到物理 CPU 的 vCPU。返回仅是无锁放置提示，最终入队仍需目标 rq 锁。
 */
/*
 * 业务背景：available_idle_cpu() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool available_idle_cpu(int cpu)
{
	if (!idle_rq(cpu_rq(cpu)))
		return 0;

	if (vcpu_is_preempted(cpu))
		return 0;

	return 1;
}

#ifdef CONFIG_SCHED_PROXY_EXEC
/*
 * 业务背景：rq_set_donor() 发布提供当前调度属性的 donor，使 proxy execution 可与 curr 分离。
 * 入参：rq 为已锁借用队列；t 为生命周期已由调度协议稳定的借用 task，可为空。
 * 出参/返回：void；以 RCU 发布 rq->donor，不增加 task 引用。
 * 注意事项：初始化必须先于发布，读者需 RCU/rq 锁；错误生命周期会产生悬空 donor，函数不睡眠。
 */
/*
 * 业务背景：rq_set_donor() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void rq_set_donor(struct rq *rq, struct task_struct *t)
{
	rcu_assign_pointer(rq->donor, t);
}
#else
/* proxy execution 关闭时 donor 与 curr 共用存储，无需另行发布；参数为借用占位且无副作用。 */
/*
 * 业务背景：rq_set_donor() 在未启用 proxy execution 时保持统一调用接口。
 * 入参：rq 与 t 均为借用参数，本分支不读取也不持有。
 * 出参/返回：void，无状态更新且不转移 ownership。
 * 注意事项：curr/donor 的共用存储由正常切换路径维护；本空桩不加锁、不睡眠。
 */
static inline void rq_set_donor(struct rq *rq, struct task_struct *t)
{
	/* Do nothing */
	/* 此配置下调用点保留统一流程，但真正 curr 更新已同时决定 donor。 */
}
#endif

#ifdef CONFIG_SCHED_CORE
static inline struct cpumask *sched_group_span(struct sched_group *sg);

DECLARE_STATIC_KEY_FALSE(__sched_core_enabled);

/*
 * 业务背景：sched_core_enabled() 同时检查全局静态键与本 rq 的 core 激活位，决定是否使用共享锁/选树。
 * 入参：rq 为借用非空队列。
 * 出参/返回：两项均开启返回真，否则返回假；无副作用。
 * 注意事项：无锁值是热路径提示，锁选择变化由 core 启停协议保护；函数不睡眠。
 */
static inline bool sched_core_enabled(struct rq *rq)
{
	return static_branch_unlikely(&__sched_core_enabled) && rq->core_enabled;
}

/*
 * 业务背景：sched_core_disabled() 只判断全局 core scheduling 静态键，供完全跳过 cookie 路径。
 * 入参：无。
 * 出参/返回：全局关闭返回真；无副作用。
 * 注意事项：不描述某个 rq 的 core_enabled 位，不能替代 sched_core_enabled(rq)；不睡眠。
 */
static inline bool sched_core_disabled(void)
{
	return !static_branch_unlikely(&__sched_core_enabled);
}

/*
 * Be careful with this function; not for general use. The return value isn't
 * stable unless you actually hold a relevant rq->__lock.
 */
/* core scheduling 可把 SMT 兄弟映射到共享锁；无锁读取结果不稳定。 */
/*
 * core scheduling 可把 SMT 兄弟映射到共享锁。rq 是借用输入，返回实际 raw lock 的借用指针，
 * 不取得锁或引用；除非已经持有相关 rq 锁，core 启停可令无锁结果失稳，故仅供统一锁包装使用。
 */
/*
 * 业务背景：rq_lockp() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline raw_spinlock_t *rq_lockp(struct rq *rq)
{
	if (sched_core_enabled(rq))
		return &rq->core->__lock;

	return &rq->__lock;
}

/*
 * 业务背景：__rq_lockp() 是 sparse 上下文标注版本，在已知 core_enabled 稳定时返回真实锁。
 * 入参：rq 为借用非空队列。
 * 出参/返回：返回 core 共享锁或本 rq 私锁的借用指针；无副作用。
 * 注意事项：调用者必须已处在令 core_enabled 稳定的协议内；不加锁、不睡眠。
 */
static inline raw_spinlock_t *__rq_lockp(struct rq *rq)
	__returns_ctx_lock(rq_lockp(rq)) /* alias them */
	/* sparse 将该返回锁视为 rq_lockp(rq) 的同一锁上下文别名。 */
{
	if (rq->core_enabled)
		return &rq->core->__lock;

	return &rq->__lock;
}

extern bool
cfs_prio_less(const struct task_struct *a, const struct task_struct *b, bool fi);

extern void task_vruntime_update(struct rq *rq, struct task_struct *p, bool in_fi);

/*
 * Helpers to check if the CPU's core cookie matches with the task's cookie
 * when core scheduling is enabled.
 * A special case is that the task's cookie always matches with CPU's core
 * cookie if the CPU is in an idle core.
 */
/*
 * sched_cpu_cookie_match() 做严格 cookie 等值检查；core 调度关闭时所有任务都兼容。
 * rq/p 均为借用输入，返回瞬时布尔值且无副作用；稳定选取必须持 core rq 锁，函数不睡眠。
 */
/*
 * 业务背景：sched_cpu_cookie_match() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_cpu_cookie_match(struct rq *rq, struct task_struct *p)
{
	/* Ignore cookie match if core scheduler is not enabled on the CPU. */
	/* 当前 CPU 未启用 core scheduler 时不施加 cookie 匹配约束。 */
	/* 未启用 core scheduling 时没有跨 SMT 隔离约束，直接接受。 */
	if (!sched_core_enabled(rq))
		return true;

	return rq->core->core_cookie == p->core_cookie;
}

/*
 * 业务背景：sched_core_cookie_match() 在严格 cookie 不同后检查整个 SMT core 是否完全空闲；空闲 core
 * 可安全接纳任意 cookie 并把它设为下一共同 cookie。rq/p 为借用输入，返回提示且无副作用。
 * 注意事项：扫描兄弟 CPU 的结果会过期，调用者必须在 core 锁下复验并提交；函数不睡眠。
 */
/*
 * 业务背景：sched_core_cookie_match() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_core_cookie_match(struct rq *rq, struct task_struct *p)
{
	bool idle_core = true;
	int cpu;

	/* Ignore cookie match if core scheduler is not enabled on the CPU. */
	/* core 调度关闭时组内任意允许 CPU 都满足 cookie 条件。 */
	if (!sched_core_enabled(rq))
		return true;

	if (rq->core->core_cookie == p->core_cookie)
		return true;

	for_each_cpu(cpu, cpu_smt_mask(cpu_of(rq))) {
		/* 任一兄弟并非可用空闲，就不能靠“空 core”例外绕过 cookie 不匹配。 */
		if (!available_idle_cpu(cpu)) {
			idle_core = false;
			break;
		}
	}

	/*
	 * A CPU in an idle core is always the best choice for tasks with
	 * cookies.
	 */
	/* 完全空闲 core 没有旁路执行者，因此接纳新 cookie 不会产生同核信息泄漏。 */
	return idle_core;
}

/*
 * 业务背景：sched_group_cookie_match() 判断调度组内是否至少有一个允许 p 安全共核运行的 CPU。
 * 入参：rq、p、group 均为调用者借用；p 的亲和性与 group span 只读，ownership 不变。
 * 出参/返回：core 调度关闭或找到 cookie 匹配 CPU 返回真，否则返回假；无副作用。
 * 注意事项：扫描结果是提示，入队前仍须在目标 core rq 锁下复验；函数不睡眠。
 */
static inline bool sched_group_cookie_match(struct rq *rq,
					    struct task_struct *p,
					    struct sched_group *group)
{
	int cpu;

	/* Ignore cookie match if core scheduler is not enabled on the CPU. */
	/* 当前 CPU 未启用 core scheduler 时无需检查任务 cookie。 */
	if (!sched_core_enabled(rq))
		return true;

	for_each_cpu_and(cpu, sched_group_span(group), p->cpus_ptr) {
		/* 同时受调度组 span 和任务 affinity 允许的 CPU 才进入 core 级复验。 */
		if (sched_core_cookie_match(cpu_rq(cpu), p))
			return true;
	}
	return false;
}

/*
 * 业务背景：sched_core_enqueued() 判断任务的 core_node 是否已挂入 cookie 选择树。
 * 入参：p 为借用的非空 task，调用者保证 core_node 生命周期。
 * 出参/返回：节点非空返回真，否则返回假；不修改树或引用。
 * 注意事项：稳定判断需要 core rq 锁；helper 不加锁且不睡眠。
 */
static inline bool sched_core_enqueued(struct task_struct *p)
{
	return !RB_EMPTY_NODE(&p->core_node);
}

extern void sched_core_enqueue(struct rq *rq, struct task_struct *p);
extern void sched_core_dequeue(struct rq *rq, struct task_struct *p, int flags);

extern void sched_core_get(void);
extern void sched_core_put(void);

/*
 * 业务背景：task_has_sched_core() 快判任务是否携带非零 core cookie 并参与同核隔离。
 * 入参：p 为借用的非空 task；不增加引用。
 * 出参/返回：全局关闭时返回假，否则返回 core_cookie 是否非零；无副作用。
 * 注意事项：cookie 可由属性更新路径改变，需要稳定值时调用者持相应锁；不睡眠。
 */
static inline bool task_has_sched_core(struct task_struct *p)
{
	if (sched_core_disabled())
		return false;

	return !!p->core_cookie;
}

#else /* !CONFIG_SCHED_CORE: */

/*
 * 业务背景：以下 stub 保持关闭 core scheduling 时公共锁与 cookie 调用点可编译。
 * 入参：rq、p、group 均为借用占位，ownership 不变。
 * 出参/返回：enabled/task_has 返回假，disabled 与 cookie_match 返回真，rq_lockp 返回 rq 私锁。
 * 注意事项：所有 stub 不睡眠且无 core 共享状态；锁 helper 仍要求调用者遵守普通 rq 锁协议。
 */
static inline bool sched_core_enabled(struct rq *rq)
{
	return false;
}

/* 全局 disabled 在此配置恒真，调用者可编译掉所有 core 专用分支。 */
/*
 * 业务背景：sched_core_disabled() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_core_disabled(void)
{
	return true;
}

/* 锁映射退化为每 CPU 私有 __lock，不存在 SMT 兄弟共享锁。 */
/*
 * 业务背景：rq_lockp() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline raw_spinlock_t *rq_lockp(struct rq *rq)
{
	return &rq->__lock;
}

/*
 * 业务背景：__rq_lockp() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline raw_spinlock_t *__rq_lockp(struct rq *rq)
	__returns_ctx_lock(rq_lockp(rq)) /* alias them */
	/* 无 core scheduling 时该标注仍把返回值映射为 rq_lockp(rq) 上下文。 */
{
	return &rq->__lock;
}

/* 未编译 core scheduling 时没有 cookie 隔离约束，CPU/task/group 三种匹配查询都恒真。 */
/*
 * 业务背景：sched_cpu_cookie_match() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_cpu_cookie_match(struct rq *rq, struct task_struct *p)
{
	return true;
}

/* core 级查询同样恒真，因为没有需要隔离的并发 SMT cookie。 */
/*
 * 业务背景：sched_core_cookie_match() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_core_cookie_match(struct rq *rq, struct task_struct *p)
{
	return true;
}

/*
 * 业务背景：sched_group_cookie_match() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_group_cookie_match(struct rq *rq,
					    struct task_struct *p,
					    struct sched_group *group)
{
	return true;
}

/* 没有 core_cookie 状态，任何 task 都不参与 core scheduling。 */
/*
 * 业务背景：task_has_sched_core() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool task_has_sched_core(struct task_struct *p)
{
	return false;
}

#endif /* !CONFIG_SCHED_CORE */
/* core scheduling 的实现与退化接口到此结束。 */

#ifdef CONFIG_RT_GROUP_SCHED
# ifdef CONFIG_RT_GROUP_SCHED_DEFAULT_DISABLED
DECLARE_STATIC_KEY_FALSE(rt_group_sched);
/*
 * 业务背景：rt_group_sched_enabled() 在默认关闭构建中通过静态键查询 RT 组调度是否运行时启用。
 * 入参：无。
 * 出参/返回：静态键开启返回真，否则返回假；无副作用。
 * 注意事项：static_branch 优化默认假热路径，不提供配置更新串行化；不睡眠。
 */
static inline bool rt_group_sched_enabled(void)
{
	return static_branch_unlikely(&rt_group_sched);
}
# else /* !CONFIG_RT_GROUP_SCHED_DEFAULT_DISABLED: */
/* 此分支对应 RT 组调度默认开启。 */
DECLARE_STATIC_KEY_TRUE(rt_group_sched);
/*
 * 业务背景：rt_group_sched_enabled() 在默认开启构建中快速查询 RT 层级调度状态。
 * 入参：无。
 * 出参/返回：静态键开启返回真，否则返回假；无副作用。
 * 注意事项：static_branch 优化默认真热路径，调用者不应缓存跨配置切换结果；不睡眠。
 */
static inline bool rt_group_sched_enabled(void)
{
	return static_branch_likely(&rt_group_sched);
}
# endif /* !CONFIG_RT_GROUP_SCHED_DEFAULT_DISABLED */
/* RT 组调度默认值的条件分支到此结束。 */
#else /* !CONFIG_RT_GROUP_SCHED: */
/* 此分支对应未编译 RT 组调度。 */
# define rt_group_sched_enabled()	false
#endif /* !CONFIG_RT_GROUP_SCHED */

/* 断言的是动态 rq_lockp()，因此同时覆盖普通 rq 锁和 core 共享锁。 */
/*
 * 业务背景：lockdep_assert_rq_held() 让 helper 声明其依赖的实际 rq/core 锁契约。
 * 入参：rq 为借用的非空运行队列；无 ownership 变化。
 * 出参/返回：void；仅 lockdep 构建中验证锁持有状态，不改运行时调度状态。
 * 注意事项：失败表示调用协议错误；函数不获取锁、不睡眠，也不能代替真实同步。
 */
static inline void lockdep_assert_rq_held(struct rq *rq)
	__assumes_ctx_lock(__rq_lockp(rq))
{
	lockdep_assert_held(__rq_lockp(rq));
}

extern void raw_spin_rq_lock_nested(struct rq *rq, int subclass)
	__acquires(__rq_lockp(rq));

extern bool raw_spin_rq_trylock(struct rq *rq)
	__cond_acquires(true, __rq_lockp(rq));

/* 所有 rq 加锁包装都经 rq_lockp()，避免 core 模式锁错每 CPU 私锁。 */
/*
 * 业务背景：raw_spin_rq_lock() 获取 rq 对应的普通或 core 共享原始自旋锁。
 * 入参：rq 为借用的非空队列；函数不改变其 ownership。
 * 出参/返回：void；成功后调用者持有 __rq_lockp(rq)，必须配对 unlock。
 * 注意事项：不处理 IRQ 状态、不可睡眠；锁选择与 nested 实现共同应对 core 模式变化。
 */
static inline void raw_spin_rq_lock(struct rq *rq)
	__acquires(__rq_lockp(rq))
{
	raw_spin_rq_lock_nested(rq, 0);
}

/*
 * 业务背景：raw_spin_rq_unlock() 释放 raw_spin_rq_lock() 取得的实际 rq/core 锁。
 * 入参：rq 为借用队列且调用者必须已持有对应锁。
 * 出参/返回：void；释放锁，不恢复 IRQ，也不转移对象 ownership。
 * 注意事项：错误配对会破坏排他性或触发 lockdep；函数不可睡眠。
 */
static inline void raw_spin_rq_unlock(struct rq *rq)
	__releases(__rq_lockp(rq))
{
	raw_spin_unlock(rq_lockp(rq));
}

/*
 * 业务背景：raw_spin_rq_lock_irq() 先关本地 IRQ 再锁 rq，防止本 CPU 中断路径递归取得该锁。
 * 入参：rq 为借用非空队列。
 * 出参/返回：void；返回时 IRQ 关闭且 rq/core 锁已持有。
 * 注意事项：必须与 raw_spin_rq_unlock_irq() 配对；临界区不可睡眠。
 */
static inline void raw_spin_rq_lock_irq(struct rq *rq)
	__acquires(__rq_lockp(rq))
{
	local_irq_disable();
	raw_spin_rq_lock(rq);
}

/*
 * 业务背景：raw_spin_rq_unlock_irq() 先放 rq 锁再开本地 IRQ，结束无保存型锁区。
 * 入参：rq 为借用队列，调用者已持锁且确认入口 IRQ 原本开启。
 * 出参/返回：void；释放锁并启用 IRQ。
 * 注意事项：若入口 IRQ 状态未知应使用 irqsave/restore 版本；不可睡眠。
 */
static inline void raw_spin_rq_unlock_irq(struct rq *rq)
	__releases(__rq_lockp(rq))
{
	raw_spin_rq_unlock(rq);
	local_irq_enable();
}

/*
 * 业务背景：_raw_spin_rq_lock_irqsave() 保存本地 IRQ 状态后获取 rq/core 锁。
 * 入参：rq 为借用非空队列。
 * 出参/返回：返回 opaque IRQ flags；副作用是关闭 IRQ 并持锁。
 * 注意事项：flags 必须原样交给 irqrestore，临界区不可睡眠。
 */
static inline unsigned long _raw_spin_rq_lock_irqsave(struct rq *rq)
	__acquires(__rq_lockp(rq))
{
	unsigned long flags;

	local_irq_save(flags);
	raw_spin_rq_lock(rq);

	return flags;
}

/*
 * 业务背景：raw_spin_rq_unlock_irqrestore() 释放 rq/core 锁并恢复调用前 IRQ 状态。
 * 入参：rq 为已锁借用队列；flags 必须来自配对 irqsave 调用。
 * 出参/返回：void；锁与 IRQ 状态恢复，不改变对象 ownership。
 * 注意事项：错误 flags 或非配对锁会破坏中断/锁状态；不可睡眠。
 */
static inline void raw_spin_rq_unlock_irqrestore(struct rq *rq, unsigned long flags)
	__releases(__rq_lockp(rq))
{
	raw_spin_rq_unlock(rq);
	local_irq_restore(flags);
}

#define raw_spin_rq_lock_irqsave(rq, flags)	\
do {						\
	flags = _raw_spin_rq_lock_irqsave(rq);	\
} while (0)

/* __update_idle_core() 由 core 调度实现更新 SMT 共享空闲状态，调用者需满足其 rq 锁契约。 */
extern void __update_idle_core(struct rq *rq);

/*
 * 业务背景：update_idle_core() 仅在 SMT 活跃时更新 core 是否完全空闲的共享提示。
 * 入参：rq 为借用的非空队列，调用者遵守 __update_idle_core() 的锁约束。
 * 出参/返回：void；SMT 活跃时可能更新 core 状态，否则无副作用。
 * 注意事项：静态分支避免非 SMT 成本；helper 本身不睡眠。
 */
static inline void update_idle_core(struct rq *rq)
{
	if (sched_smt_active())
		__update_idle_core(rq);
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * 业务背景：task_of() 从叶 sched_entity 还原其所属 task_struct，供 CFS 从通用实体进入任务路径。
 * 入参：se 为借用非空实体，必须是 task 而非组实体；ownership 不变。
 * 出参/返回：返回嵌入 se 的 task 借用指针；无副作用，错误类型会 WARN。
 * 注意事项：返回生命周期由调用者的 task/rq 保护保证；函数不睡眠。
 */
static inline struct task_struct *task_of(struct sched_entity *se)
{
	WARN_ON_ONCE(!entity_is_task(se));
	return container_of(se, struct task_struct, se);
}

/*
 * 业务背景：task_cfs_rq() 返回组调度下任务实体当前归属的 cfs_rq。
 * 入参：p 为借用非空 task；不增加引用。
 * 出参/返回：返回 p->se.cfs_rq 借用指针；无副作用。
 * 注意事项：迁移可改变归属，稳定使用需持 task/rq 锁；函数不睡眠。
 */
static inline struct cfs_rq *task_cfs_rq(struct task_struct *p)
{
	return p->se.cfs_rq;
}

/* runqueue on which this entity is (to be) queued */
/* 实体已经或将要入队的 cfs_rq；返回借用指针，迁移期间必须由 rq 锁稳定。 */
/*
 * 业务背景：cfs_rq_of() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline struct cfs_rq *cfs_rq_of(const struct sched_entity *se)
{
	return se->cfs_rq;
}

/* runqueue "owned" by this group */
/* 组实体拥有的子 cfs_rq；任务实体没有 my_q，调用者须先确认实体类型。 */
/*
 * 业务背景：group_cfs_rq() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline struct cfs_rq *group_cfs_rq(struct sched_entity *grp)
{
	return grp->my_q;
}

#else /* !CONFIG_FAIR_GROUP_SCHED: */
/* 此分支提供无公平组调度时的顶层队列退化实现。 */

#define task_of(_se)		container_of(_se, struct task_struct, se)

/*
 * 业务背景：无组调度时 task_cfs_rq() 直接返回任务所在 CPU 的顶层 CFS 队列。
 * 入参：p 为借用非空 task；ownership 不变。
 * 出参/返回：返回 task_rq(p)->cfs 借用指针；无副作用。
 * 注意事项：task CPU 可并发迁移，稳定使用须持 pi/rq 锁；函数不睡眠。
 */
static inline struct cfs_rq *task_cfs_rq(const struct task_struct *p)
{
	return &task_rq(p)->cfs;
}

/*
 * 业务背景：无组调度时 cfs_rq_of() 经实体所属 task 与 CPU 找到唯一顶层 cfs_rq。
 * 入参：se 为借用的任务实体；不增加引用。
 * 出参/返回：返回当前 CPU rq 的 cfs 成员借用指针；无副作用。
 * 注意事项：传入组实体无效，迁移稳定性由调用者锁保证；函数不睡眠。
 */
static inline struct cfs_rq *cfs_rq_of(const struct sched_entity *se)
{
	const struct task_struct *p = task_of(se);
	struct rq *rq = task_rq(p);

	return &rq->cfs;
}

/* runqueue "owned" by this group */
/* 无组调度时不存在组实体拥有的子 cfs_rq，因此固定返回 NULL 且无副作用。 */
/*
 * 业务背景：group_cfs_rq() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline struct cfs_rq *group_cfs_rq(struct sched_entity *grp)
{
	return NULL;
}

#endif /* !CONFIG_FAIR_GROUP_SCHED */
/* 公平组调度实体到队列映射接口到此结束。 */

extern void update_rq_avg_idle(struct rq *rq);
extern void update_rq_clock(struct rq *rq);

/*
 * rq::clock_update_flags bits
 *
 * %RQCF_REQ_SKIP - will request skipping of clock update on the next
 *  call to __schedule(). This is an optimisation to avoid
 *  neighbouring rq clock updates.
 *
 * %RQCF_ACT_SKIP - is set from inside of __schedule() when skipping is
 *  in effect and calls to update_rq_clock() are being ignored.
 *
 * %RQCF_UPDATED - is a debug flag that indicates whether a call has been
 *  made to update_rq_clock() since the last time rq::lock was pinned.
 *
 * If inside of __schedule(), clock_update_flags will have been
 * shifted left (a left shift is a cheap operation for the fast path
 * to promote %RQCF_REQ_SKIP to %RQCF_ACT_SKIP), so you must use,
 *
 *	if (rq-clock_update_flags >= RQCF_UPDATED)
 *
 * to check if %RQCF_UPDATED is set. It'll never be shifted more than
 * one position though, because the next rq_unpin_lock() will shift it
 * back.
 */
/*
 * clock_update_flags 协议：REQ_SKIP 请求下一次 __schedule() 跳过相邻重复更新，进入调度后左移成
 * ACT_SKIP；UPDATED 记录本次 pin 后已更新时钟。由于调度内会左移一次，应使用 >= UPDATED 判断。
 * rq_unpin_lock() 会移回并结束此状态，所有位都在 rq 锁与 pin 协议下维护。
 */
#define RQCF_REQ_SKIP		0x01
#define RQCF_ACT_SKIP		0x02
#define RQCF_UPDATED		0x04

/*
 * 业务背景：assert_clock_updated() 检查本次 rq pin 后是否更新时钟，或正合法跳过更新。
 * 入参：rq 为借用且已锁队列。
 * 出参/返回：void；违反协议时 WARN，不修改调度状态。
 * 注意事项：仅是调试断言，不产生同步；调用者仍须持 rq 锁，函数不睡眠。
 */
static inline void assert_clock_updated(struct rq *rq)
{
	/*
	 * The only reason for not seeing a clock update since the
	 * last rq_pin_lock() is if we're currently skipping updates.
	 */
	/* 若 pin 后尚未更新时钟，唯一允许的原因是当前处在合法跳过更新状态。 */
	WARN_ON_ONCE(rq->clock_update_flags < RQCF_ACT_SKIP);
}

/* rq 时钟只可在锁内且本次 pin 已更新后读取。 */
/*
 * 业务背景：rq_clock() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline u64 rq_clock(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	assert_clock_updated(rq);

	return rq->clock;
}

/* task clock 排除不可归责时间；锁和 UPDATED 约束与 rq_clock 相同。 */
/*
 * 业务背景：rq_clock_task() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline u64 rq_clock_task(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	assert_clock_updated(rq);

	return rq->clock_task;
}

/*
 * 业务背景：rq_clock_skip_update() 请求下一次 schedule 跳过重复 rq 时钟更新。
 * 入参：rq 为借用且已锁队列。
 * 出参/返回：void；设置 RQCF_REQ_SKIP，不改变 ownership。
 * 注意事项：只用于已知相邻更新场景，滥用会让时间记账陈旧；不睡眠。
 */
static inline void rq_clock_skip_update(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	rq->clock_update_flags |= RQCF_REQ_SKIP;
}

/*
 * See rt task throttling, which is the only time a skip
 * request is canceled.
 */
/* RT 节流是唯一取消 skip 请求的路径，因为预算状态转换需要重新取得新鲜时钟。 */
/*
 * 业务背景：rq_clock_cancel_skipupdate() 撤销尚未生效的时钟跳过请求。
 * 入参：rq 为借用且已锁队列。
 * 出参/返回：void；清除 REQ_SKIP，无其他副作用。
 * 注意事项：不能清除已生效 ACT_SKIP；函数不睡眠。
 */
static inline void rq_clock_cancel_skipupdate(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	rq->clock_update_flags &= ~RQCF_REQ_SKIP;
}

/*
 * During cpu offlining and rq wide unthrottling, we can trigger
 * an update_rq_clock() for several cfs and rt runqueues (Typically
 * when using list_for_each_entry_*)
 * rq_clock_start_loop_update() can be called after updating the clock
 * once and before iterating over the list to prevent multiple update.
 * After the iterative traversal, we need to call rq_clock_stop_loop_update()
 * to clear RQCF_ACT_SKIP of rq->clock_update_flags.
 */
/*
 * CPU 下线或全 rq 解节流会在链表循环中触发多个子队列更新；先更新一次时钟，再用 start 设置
 * ACT_SKIP 屏蔽循环内重复更新，遍历后必须调用 stop 清位，否则后续记账会长期使用旧时间。
 */
/*
 * 业务背景：rq_clock_start_loop_update() 开启批量遍历的时钟更新抑制区。
 * 入参：rq 为借用且已锁队列。
 * 出参/返回：void；验证未嵌套后设置 ACT_SKIP。
 * 注意事项：必须与 stop 配对，函数不睡眠。
 */
static inline void rq_clock_start_loop_update(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	WARN_ON_ONCE(rq->clock_update_flags & RQCF_ACT_SKIP);
	rq->clock_update_flags |= RQCF_ACT_SKIP;
}

/*
 * 业务背景：rq_clock_stop_loop_update() 结束批量更新抑制，使后续 update_rq_clock() 恢复生效。
 * 入参：rq 为借用且已锁队列。
 * 出参/返回：void；清除 ACT_SKIP，无 ownership 变化。
 * 注意事项：只应结束配对 start 建立的区间；函数不睡眠。
 */
static inline void rq_clock_stop_loop_update(struct rq *rq)
{
	lockdep_assert_rq_held(rq);
	rq->clock_update_flags &= ~RQCF_ACT_SKIP;
}

/* 保存 IRQ 状态、lockdep pin cookie 和放锁重锁间的时钟更新证据。 */
struct rq_flags {
	/* flags 保存调用前 IRQ 状态，cookie 表示当前 lockdep pin，二者必须与同一 rq 配对。 */
	unsigned long flags;
	struct pin_cookie cookie;
	/*
	 * A copy of (rq::clock_update_flags & RQCF_UPDATED) for the
	 * current pin context is stashed here in case it needs to be
	 * restored in rq_repin_lock().
	 */
	/* 暂放锁只继承 UPDATED 证据；REQ/ACT_SKIP 仍保存在 rq 自身。 */
	unsigned int clock_update_flags;
};

extern struct balance_callback balance_push_callback;

#ifdef CONFIG_SCHED_CLASS_EXT
extern const struct sched_class ext_sched_class;

DECLARE_STATIC_KEY_FALSE(__scx_enabled);	/* SCX BPF scheduler loaded */
/* __scx_enabled 表示 SCX BPF 调度器已经装载。 */
DECLARE_STATIC_KEY_FALSE(__scx_switched_all);	/* all fair class tasks on SCX */
/* __scx_switched_all 表示全部 fair 类任务都已切换到 SCX。 */

#define scx_enabled()		static_branch_unlikely(&__scx_enabled)
#define scx_switched_all()	static_branch_unlikely(&__scx_switched_all)

/*
 * 业务背景：scx_rq_clock_update() 向 BPF sched_ext 发布当前 rq 时钟并标记快照有效。
 * 入参：rq 为借用且由 rq 锁稳定；clock 是同 rq 时间轴的新值，单位纳秒。
 * 出参/返回：void；SCX 开启时写 clock，再以 release 发布 VALID 位。
 * 注意事项：写值必须先于有效位被读者观察；关闭时快速返回，不睡眠。
 */
static inline void scx_rq_clock_update(struct rq *rq, u64 clock)
{
	if (!scx_enabled())
		return;
	WRITE_ONCE(rq->scx.clock, clock);
	smp_store_release(&rq->scx.flags, rq->scx.flags | SCX_RQ_CLK_VALID);
}

/*
 * 业务背景：scx_rq_clock_invalidate() 在放锁/时钟失稳边界撤销 SCX 时钟有效标记。
 * 入参：rq 为借用非空队列；ownership 不变。
 * 出参/返回：void；SCX 开启时清除 VALID 位，不改 clock 数值。
 * 注意事项：读者必须先检查有效位；函数不睡眠。
 */
static inline void scx_rq_clock_invalidate(struct rq *rq)
{
	if (!scx_enabled())
		return;
	WRITE_ONCE(rq->scx.flags, rq->scx.flags & ~SCX_RQ_CLK_VALID);
}

#else /* !CONFIG_SCHED_CLASS_EXT: */
/* 此分支提供未编译 sched_ext 时的退化接口。 */
#define scx_enabled()		false
#define scx_switched_all()	false

/* SCX 未编译时两个时钟 helper 是无副作用 no-op；参数仅为接口兼容，函数不睡眠。 */
/*
 * 业务背景：scx_rq_clock_update() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void scx_rq_clock_update(struct rq *rq, u64 clock) {}
/*
 * 业务背景：scx_rq_clock_invalidate() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void scx_rq_clock_invalidate(struct rq *rq) {}
#endif /* !CONFIG_SCHED_CLASS_EXT */

/*
 * 业务背景：assert_balance_callbacks_empty() 防止离开 rq 临界区时遗留普通 balance callback。
 * 入参：rq 为借用且应已锁队列。
 * 出参/返回：void；PROVE_LOCKING 下发现非 push 回调则 WARN，无状态修改。
 * 注意事项：balance_push_callback 是热插拔允许的例外；断言不代替执行回调，不睡眠。
 */
static inline void assert_balance_callbacks_empty(struct rq *rq)
{
	WARN_ON_ONCE(IS_ENABLED(CONFIG_PROVE_LOCKING) &&
		     rq->balance_callback &&
		     rq->balance_callback != &balance_push_callback);
}

/*
 * Lockdep annotation that avoids accidental unlocks; it's like a
 * sticky/continuous lockdep_assert_held().
 *
 * This avoids code that has access to 'struct rq *rq' (basically everything in
 * the scheduler) from accidentally unlocking the rq if they do not also have a
 * copy of the (on-stack) 'struct rq_flags rf'.
 *
 * Also see Documentation/locking/lockdep-design.rst.
 */
/* pin 防止深层 helper 意外解锁，并为本次临界区重置时钟审计状态。 */
/*
 * 业务背景：rq_pin_lock() 把已获取的 rq 锁绑定到栈上 rf，建立连续 lockdep 与时钟审计上下文。
 * 入参：rq 为已锁借用队列；rf 为调用者拥有的非空输出上下文。
 * 出参/返回：void；写入 pin cookie，保留 skip 位并清除本次 UPDATED 证据。
 * 注意事项：必须在真实持锁后调用并与 unpin 配对；不获取锁、不睡眠。
 */
static inline void rq_pin_lock(struct rq *rq, struct rq_flags *rf)
{
	rf->cookie = lockdep_pin_lock(__rq_lockp(rq));

	rq->clock_update_flags &= (RQCF_REQ_SKIP|RQCF_ACT_SKIP);
	rf->clock_update_flags = 0;
	assert_balance_callbacks_empty(rq);
}

/* 暂时放锁前保存 UPDATED 并使 SCX 时钟失效，重锁后不得沿用旧快照。 */
/*
 * 业务背景：rq_unpin_lock() 在释放或暂放 rq 锁前结束 pin，并保存可恢复的时钟更新证据。
 * 入参：rq 为已锁借用队列；rf 是配对 pin 的输入输出上下文。
 * 出参/返回：void；可能保存 UPDATED、使 SCX 时钟失效并解除 lockdep pin。
 * 注意事项：只处理审计状态，不释放真实锁；必须使用原 cookie，函数不睡眠。
 */
static inline void rq_unpin_lock(struct rq *rq, struct rq_flags *rf)
{
	if (rq->clock_update_flags > RQCF_ACT_SKIP)
		rf->clock_update_flags = RQCF_UPDATED;

	scx_rq_clock_invalidate(rq);
	lockdep_unpin_lock(__rq_lockp(rq), rf->cookie);
}

/*
 * 业务背景：rq_repin_lock() 在暂放后重新持锁时恢复同一 rf 的 lockdep pin 与 UPDATED 证据。
 * 入参：rq 为重新锁住的借用队列；rf 来自此前 unpin，ownership 不变。
 * 出参/返回：void；恢复 pin 并合并保存的 clock_update_flags。
 * 注意事项：不得跨不同 rq 或复用失效 rf；函数不获取真实锁、不睡眠。
 */
static inline void rq_repin_lock(struct rq *rq, struct rq_flags *rf)
{
	lockdep_repin_lock(__rq_lockp(rq), rf->cookie);

	/*
	 * Restore the value we stashed in @rf for this pin context.
	 */
	/* 恢复该 pin 上下文暂存的值，使锁内时钟读取继续满足 UPDATED 断言。 */
	rq->clock_update_flags |= rf->clock_update_flags;
}

#define __task_rq_lock(...) __acquire_ret(___task_rq_lock(__VA_ARGS__), __rq_lockp(__ret))
extern struct rq *___task_rq_lock(struct task_struct *p, struct rq_flags *rf) __acquires_ret;

#define task_rq_lock(...) __acquire_ret(_task_rq_lock(__VA_ARGS__), __rq_lockp(__ret))
extern struct rq *_task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(&p->pi_lock) __acquires_ret;

/* 只释放 rq 锁；调用者若同时持 pi_lock 必须用 task_rq_unlock()。 */
/*
 * 业务背景：__task_rq_unlock() 结束只持 rq 的 task 队列临界区，保留可能由上层持有的 pi_lock。
 * 入参：rq/p 为借用对象，rf 为配对锁上下文；p 仅表达接口与 lockdep 关系。
 * 出参/返回：void；unpin 后释放实际 rq/core 锁，不恢复 IRQ。
 * 注意事项：若同时持 pi_lock 不得在此后遗忘释放；函数不可睡眠。
 */
static inline void
__task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
	__releases(__rq_lockp(rq))
{
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock(rq);
}

/*
 * 业务背景：task_rq_unlock() 完整结束 task_rq_lock() 建立的 rq + p->pi_lock 双锁协议。
 * 入参：rq/p/rf 均来自配对加锁，借用且不可混用其他任务上下文。
 * 出参/返回：void；先释放 rq，再用 rf->flags 释放 pi_lock 并恢复 IRQ。
 * 注意事项：顺序避免持 rq 恢复 IRQ；调用后裸 rq 归属可能因迁移改变，不可睡眠于锁内。
 */
static inline void
task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
	__releases(__rq_lockp(rq), &p->pi_lock)
{
	__task_rq_unlock(rq, p, rf);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);
}

/* 自动 guard 把 task_rq_lock() 的 rq、rf 和 pi_lock 配对释放绑定到词法作用域。 */
DEFINE_LOCK_GUARD_1(task_rq_lock, struct task_struct,
		    _T->rq = task_rq_lock(_T->lock, &_T->rf),
		    task_rq_unlock(_T->rq, _T->lock, &_T->rf),
		    struct rq *rq; struct rq_flags rf)
DECLARE_LOCK_GUARD_1_ATTRS(task_rq_lock, __acquires(_T->pi_lock), __releases((*(struct task_struct **)_T)->pi_lock))
#define class_task_rq_lock_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(task_rq_lock, _T)

/* __task_rq_lock guard 只自动管理 rq 锁；外层若另持 pi_lock，仍由外层负责其生命周期。 */
DEFINE_LOCK_GUARD_1(__task_rq_lock, struct task_struct,
		    _T->rq = __task_rq_lock(_T->lock, &_T->rf),
		    __task_rq_unlock(_T->rq, _T->lock, &_T->rf),
		    struct rq *rq; struct rq_flags rf)

/*
 * 业务背景：rq_lock_irqsave() 保存 IRQ、取得 rq/core 锁并建立 pin 审计上下文。
 * 入参：rq 为借用队列；rf 为调用者拥有的输出上下文。
 * 出参/返回：void；返回时 IRQ 关闭、锁已持有且 rf 完整，须配对 unlock_irqrestore。
 * 注意事项：临界区不可睡眠，rf 不得跨 rq 使用。
 */
static inline void rq_lock_irqsave(struct rq *rq, struct rq_flags *rf)
	__acquires(__rq_lockp(rq))
{
	raw_spin_rq_lock_irqsave(rq, rf->flags);
	rq_pin_lock(rq, rf);
}

/*
 * 业务背景：rq_lock_irq() 关闭本地 IRQ、获取 rq/core 锁并 pin，适合入口 IRQ 已开启路径。
 * 入参：rq 为借用队列；rf 为输出上下文。
 * 出参/返回：void；返回时 IRQ 关闭且锁已持有，须配对 rq_unlock_irq()。
 * 注意事项：未知入口 IRQ 状态应使用 irqsave；临界区不可睡眠。
 */
static inline void rq_lock_irq(struct rq *rq, struct rq_flags *rf)
	__acquires(__rq_lockp(rq))
{
	raw_spin_rq_lock_irq(rq);
	rq_pin_lock(rq, rf);
}

/*
 * 业务背景：rq_lock() 在保持当前 IRQ 状态的同时获取 rq/core 锁并建立 pin。
 * 入参：rq 为借用队列；rf 为输出上下文。
 * 出参/返回：void；锁已持有，IRQ 状态不变，须配对 rq_unlock()。
 * 注意事项：调用者负责防止本地中断递归及不可睡眠约束。
 */
static inline void rq_lock(struct rq *rq, struct rq_flags *rf)
	__acquires(__rq_lockp(rq))
{
	raw_spin_rq_lock(rq);
	rq_pin_lock(rq, rf);
}

/*
 * 业务背景：rq_unlock_irqrestore() 结束 irqsave 型 rq 临界区。
 * 入参：rq/rf 必须来自配对 rq_lock_irqsave()。
 * 出参/返回：void；unpin、释放锁并恢复原 IRQ 状态。
 * 注意事项：释放后 rq 状态可并发变化，rf 不可再复用；函数不睡眠。
 */
static inline void rq_unlock_irqrestore(struct rq *rq, struct rq_flags *rf)
	__releases(__rq_lockp(rq))
{
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock_irqrestore(rq, rf->flags);
}

/*
 * 业务背景：rq_unlock_irq() 结束入口 IRQ 开启的 rq_lock_irq() 临界区。
 * 入参：rq/rf 来自配对加锁。
 * 出参/返回：void；unpin、放锁并启用本地 IRQ。
 * 注意事项：不得用于入口 IRQ 原已关闭路径；函数不睡眠。
 */
static inline void rq_unlock_irq(struct rq *rq, struct rq_flags *rf)
	__releases(__rq_lockp(rq))
{
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock_irq(rq);
}

/*
 * 业务背景：rq_unlock() 结束不改变 IRQ 状态的 rq_lock() 临界区。
 * 入参：rq/rf 来自配对加锁。
 * 出参/返回：void；unpin 并释放实际 rq/core 锁，IRQ 状态保持不变。
 * 注意事项：放锁后所有无引用裸状态都须重新验证；函数不睡眠。
 */
static inline void rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(__rq_lockp(rq))
{
	rq_unpin_lock(rq, rf);
	raw_spin_rq_unlock(rq);
}

/* 三组自动 guard 分别保持 IRQ、不保存地关闭 IRQ、保存并恢复 IRQ；都同时管理 rq pin。 */
DEFINE_LOCK_GUARD_1(rq_lock, struct rq,
		    rq_lock(_T->lock, &_T->rf),
		    rq_unlock(_T->lock, &_T->rf),
		    struct rq_flags rf)

DECLARE_LOCK_GUARD_1_ATTRS(rq_lock, __acquires(__rq_lockp(_T)), __releases(__rq_lockp(*(struct rq **)_T)));
#define class_rq_lock_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rq_lock, _T)

/* irq guard 要求入口 IRQ 开启，析构时固定启用 IRQ；未知状态必须选 irqsave guard。 */
DEFINE_LOCK_GUARD_1(rq_lock_irq, struct rq,
		    rq_lock_irq(_T->lock, &_T->rf),
		    rq_unlock_irq(_T->lock, &_T->rf),
		    struct rq_flags rf)

DECLARE_LOCK_GUARD_1_ATTRS(rq_lock_irq, __acquires(__rq_lockp(_T)), __releases(__rq_lockp(*(struct rq **)_T)));
#define class_rq_lock_irq_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rq_lock_irq, _T)

/* irqsave guard 把原 IRQ flags 保存在自动对象中，离开作用域时精确恢复。 */
DEFINE_LOCK_GUARD_1(rq_lock_irqsave, struct rq,
		    rq_lock_irqsave(_T->lock, &_T->rf),
		    rq_unlock_irqrestore(_T->lock, &_T->rf),
		    struct rq_flags rf)

DECLARE_LOCK_GUARD_1_ATTRS(rq_lock_irqsave, __acquires(__rq_lockp(_T)), __releases(__rq_lockp(*(struct rq **)_T)));
#define class_rq_lock_irqsave_constructor(_T) WITH_LOCK_GUARD_1_ATTRS(rq_lock_irqsave, _T)

#define this_rq_lock_irq(...) __acquire_ret(_this_rq_lock_irq(__VA_ARGS__), __rq_lockp(__ret))
/*
 * 业务背景：_this_rq_lock_irq() 原子化取得当前 CPU rq：先关 IRQ 防迁移/重入，再取 this_rq 并加锁。
 * 入参：rf 为调用者拥有的非空输出锁上下文。
 * 出参/返回：返回已锁的当前 rq 借用指针；IRQ 关闭，须由 rq_unlock_irq() 释放。
 * 注意事项：返回 ownership 不转移，临界区不可睡眠；关 IRQ 必须早于读取 per-CPU rq。
 */
static inline struct rq *_this_rq_lock_irq(struct rq_flags *rf) __acquires_ret
{
	struct rq *rq;

	local_irq_disable();
	rq = this_rq();
	rq_lock(rq, rf);

	return rq;
}

#ifdef CONFIG_NUMA

enum numa_topology_type {
	/* CPU 到内存节点存在直接连接，距离层级较简单，调度域可按直连关系构建。 */
	NUMA_DIRECT,
	/* 无胶合 mesh：节点间经规则网格互连，距离反映多跳拓扑。 */
	NUMA_GLUELESS_MESH,
	/* 背板型拓扑：远端通信汇聚到共享互连，均衡需按较宽层级处理。 */
	NUMA_BACKPLANE,
};

extern enum numa_topology_type sched_numa_topology_type;
extern int sched_max_numa_distance;
/* NUMA 拓扑接口构建距离层级并随 CPU online/offline 更新 per-CPU domain mask。 */
extern bool find_numa_distance(int distance);
extern void sched_init_numa(int offline_node);
extern void sched_update_numa(int cpu, bool online);
extern void sched_domains_numa_masks_set(unsigned int cpu);
extern void sched_domains_numa_masks_clear(unsigned int cpu);
extern int sched_numa_find_closest(const struct cpumask *cpus, int cpu);

#else /* !CONFIG_NUMA: */

/* NUMA 未编译时初始化、热插与 mask 更新接口均为无副作用 no-op，参数仅保持调用 ABI。 */
/*
 * 业务背景：sched_init_numa() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_init_numa(int offline_node) { }
/*
 * 业务背景：sched_update_numa() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_update_numa(int cpu, bool online) { }
/*
 * 业务背景：sched_domains_numa_masks_set() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_domains_numa_masks_set(unsigned int cpu) { }
/*
 * 业务背景：sched_domains_numa_masks_clear() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_domains_numa_masks_clear(unsigned int cpu) { }

/*
 * 业务背景：无 NUMA 配置时 sched_numa_find_closest() 表示不存在可选 NUMA 目标。
 * 入参：cpus 为借用 mask，cpu 为源编号；两者均不读取。
 * 出参/返回：固定返回 nr_cpu_ids 哨兵；无副作用。
 * 注意事项：调用者必须把该值当“未找到”而非有效 CPU；函数不睡眠。
 */
static inline int sched_numa_find_closest(const struct cpumask *cpus, int cpu)
{
	return nr_cpu_ids;
}

#endif /* !CONFIG_NUMA */
/* NUMA 拓扑查询接口的配置分支到此结束。 */

#ifdef CONFIG_NUMA_BALANCING

/* The regions in numa_faults array from task_struct */
/* task_struct::numa_faults 数组的四个分区分别保存内存/CPU 当前统计及其缓冲累计。 */
enum numa_faults_stats {
	/* 已归档的按内存节点访问故障。 */
	NUMA_MEM = 0,
	/* 已归档的按执行 CPU/节点故障。 */
	NUMA_CPU,
	/* 本轮尚未结算的内存节点故障缓冲。 */
	NUMA_MEMBUF,
	/* 本轮尚未结算的执行 CPU/节点故障缓冲。 */
	NUMA_CPUBUF
};

extern void sched_setnuma(struct task_struct *p, int node);
extern int migrate_task_to(struct task_struct *p, int cpu);
extern int migrate_swap(struct task_struct *p, struct task_struct *t,
			int cpu, int scpu);
extern void init_numa_balancing(u64 clone_flags, struct task_struct *p);

#else /* !CONFIG_NUMA_BALANCING: */

/*
 * 业务背景：NUMA 自动均衡关闭时 init_numa_balancing() 不为新任务建立故障统计状态。
 * 入参：clone_flags 与 p 为未使用的纯输入/借用参数；ownership 不变。
 * 出参/返回：void，无副作用。
 * 注意事项：这是编译期 no-op，不影响显式 cpuset/亲和性；函数不睡眠。
 */
static inline void
init_numa_balancing(u64 clone_flags, struct task_struct *p)
{
}

#endif /* !CONFIG_NUMA_BALANCING */
/* NUMA 自动均衡初始化接口的配置分支到此结束。 */

int task_llc(const struct task_struct *p);

/*
 * 业务背景：queue_balance_callback() 把一次 rq 解锁前执行的均衡工作按头插方式排入回调链。
 * 入参：rq 为已锁借用队列；head 由调用者长期拥有且未在其他链上；func 是不可睡眠回调。
 * 出参/返回：void；成功时写 head 并发布到 rq，重复排队或 push 活跃时无副作用。
 * 注意事项：调用者持 rq 锁；head 在执行/摘除前必须存活，回调运行时仍处调度锁协议内。
 */
static inline void
queue_balance_callback(struct rq *rq,
		       struct balance_callback *head,
		       void (*func)(struct rq *rq))
{
	lockdep_assert_rq_held(rq);

	/*
	 * Don't (re)queue an already queued item; nor queue anything when
	 * balance_push() is active, see the comment with
	 * balance_push_callback.
	 */
	/*
	 * 同一 head 已排队时不得重复挂链；CPU 热插 balance_push() 活跃时也拒绝其他回调，
	 * 避免 push 专用回调被覆盖或链表形成环。成功时在 rq 锁下头插，ownership 仍由调用者管理。
	 */
	if (unlikely(head->next || rq->balance_callback == &balance_push_callback))
		return;

	head->func = func;
	head->next = rq->balance_callback;
	rq->balance_callback = head;
}

#define rcu_dereference_sched_domain(p) \
	rcu_dereference_all_check((p), lockdep_is_held(&sched_domains_mutex))

/*
 * The domain tree (rq->sd) is protected by RCU's quiescent state transition.
 * See destroy_sched_domains: call_rcu for details.
 *
 * The domain tree of any CPU may only be accessed from within
 * preempt-disabled sections.
 */
/*
 * sched_domain 树由 RCU 静止状态保护，destroy_sched_domains() 通过 call_rcu 延迟回收；访问任意
 * CPU 的树都必须在禁抢占区，既稳定当前 CPU/per-CPU 根指针，也覆盖 RCU 读侧生命周期。
 */
#define for_each_domain(cpu, __sd) \
	for (__sd = rcu_dereference_sched_domain(cpu_rq(cpu)->sd); \
			__sd; __sd = __sd->parent)

/* A mask of all the SD flags that have the SDF_SHARED_CHILD metaflag */
/* 汇总所有带 SDF_SHARED_CHILD 元标志的 SD flag，用于判断向上搜索能否安全提前停止。 */
#define SD_FLAG(name, mflags) (name * !!((mflags) & SDF_SHARED_CHILD)) |
static const unsigned int SD_SHARED_CHILD_MASK =
#include <linux/sched/sd_flags.h>
0;
#undef SD_FLAG

/**
 * highest_flag_domain - Return highest sched_domain containing flag.
 * @cpu:	The CPU whose highest level of sched domain is to
 *		be returned.
 * @flag:	The flag to check for the highest sched_domain
 *		for the given CPU.
 *
 * Returns the highest sched_domain of a CPU which contains @flag. If @flag has
 * the SDF_SHARED_CHILD metaflag, all the children domains also have @flag.
 */
/* 在禁抢占/RCU 保护下找最高匹配层；shared-child 标志允许提前终止。 */
/*
 * 业务背景：highest_flag_domain() 沿 CPU 的 domain 父链寻找包含 flag 的最高层级。
 * 入参：cpu 是有效 CPU；flag 是单个 SD 标志，均为纯输入。
 * 出参/返回：返回最高匹配 domain 借用指针，找不到返回 NULL；无副作用。
 * 注意事项：调用者必须禁抢占/处于拓扑 RCU 读侧，返回指针不可越界使用；不睡眠。
 */
static inline struct sched_domain *highest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd, *hsd = NULL;

	for_each_domain(cpu, sd) {
		if (sd->flags & flag) {
			hsd = sd;
			continue;
		}

		/*
		 * Stop the search if @flag is known to be shared at lower
		 * levels. It will not be found further up.
		 */
		/* 若该 flag 按定义由子层共享，当前层缺失便说明更高层也不会重新出现，可终止搜索。 */
		if (flag & SD_SHARED_CHILD_MASK)
			break;
	}

	return hsd;
}

/* 返回首个匹配层或 NULL，返回指针不能越过调用者的拓扑读侧区间。 */
/*
 * 业务背景：lowest_flag_domain() 返回 CPU 父链上首个带 flag 的最内层 domain。
 * 入参：cpu 为有效 CPU；flag 为待匹配标志，纯输入。
 * 出参/返回：返回最低匹配 domain 借用指针或 NULL；无副作用。
 * 注意事项：调用者必须禁抢占/持拓扑读侧保护；函数不睡眠。
 */
static inline struct sched_domain *lowest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd;

	for_each_domain(cpu, sd) {
		if (sd->flags & flag)
			break;
	}

	return sd;
}

/*
 * 这些 per-CPU 指针缓存 LLC、NUMA、asym packing/capacity 等关键 domain 层；指针通过 RCU
 * 随拓扑重建替换，配套 id/size 只是派生快照，读者必须在同一 RCU/禁抢占窗口内使用。
 */
DECLARE_PER_CPU(struct sched_domain __rcu *, sd_llc);
DECLARE_PER_CPU(int, sd_llc_size);
DECLARE_PER_CPU(int, sd_llc_id);
DECLARE_PER_CPU(int, sd_share_id);
DECLARE_PER_CPU(struct sched_domain_shared __rcu *, sd_llc_shared);
DECLARE_PER_CPU(struct sched_domain_shared __rcu *, sd_balance_shared);
/* NUMA 与两类非对称缓存分别定位纵向父链上的首个相关 domain。 */
DECLARE_PER_CPU(struct sched_domain __rcu *, sd_numa);
DECLARE_PER_CPU(struct sched_domain __rcu *, sd_asym_packing);
DECLARE_PER_CPU(struct sched_domain __rcu *, sd_asym_cpucapacity);

extern struct static_key_false sched_asym_cpucapacity;
extern struct static_key_false sched_cluster_active;

/*
 * 业务背景：sched_asym_cpucap_active() 快判系统是否存在非对称 CPU capacity 调度域。
 * 入参：无。
 * 出参/返回：静态键开启返回真，否则返回假；无副作用。
 * 注意事项：只表示特性存在，不保证特定 CPU 位于该域；函数不睡眠。
 */
static __always_inline bool sched_asym_cpucap_active(void)
{
	return static_branch_unlikely(&sched_asym_cpucapacity);
}

/* 相同 group 共享的容量快照，以 ref 管理，balance_mask 决定唯一执行 CPU。 */
struct sched_group_capacity {
	atomic_t		ref;
	/*
	 * CPU capacity of this group, SCHED_CAPACITY_SCALE being max capacity
	 * for a single CPU.
	 */
	/* 组总 capacity，以单 CPU 最大值 SCHED_CAPACITY_SCALE 为尺度，可超过单 CPU 上限。 */
	unsigned long		capacity;
	unsigned long		min_capacity;		/* Min per-CPU capacity in group */
	/* min_capacity 是组内单 CPU 最小 capacity，用于识别异构跨度。 */
	unsigned long		max_capacity;		/* Max per-CPU capacity in group */
	/* max_capacity 是组内单 CPU 最大 capacity，与 min 共同描述不对称程度。 */
	unsigned long		next_update;
	/* next_update 限制容量重算频率；imbalance 是共享均衡状态但沿用同一对象生命周期。 */
	int			imbalance;		/* XXX unrelated to capacity but shared group state */
	/* FIXME：imbalance 与 capacity 无直接关系，只因同组共享生命周期暂存于此。 */

	int			id;

	unsigned long		cpumask[];		/* Balance mask */
	/* 柔性 balance mask 指定该组中唯一/允许承担均衡工作的 CPU 集合。 */
};

/* domain 的横向组环；next 必须闭环，span 与 ref 在拓扑发布后只读。 */
struct sched_group {
	struct sched_group	*next;			/* Must be a circular list */
	/* next 必须形成闭环；遍历以回到起点终止，断链会导致越界或漏组。 */
	atomic_t		ref;

	unsigned int		group_weight;
	unsigned int		cores;
	struct sched_group_capacity *sgc;
	int			asym_prefer_cpu;	/* CPU of highest priority in group */
	/* 非对称 packing 下组内体系结构优先级最高的 CPU 编号。 */
	int			flags;

	/*
	 * The CPUs this group covers.
	 *
	 * NOTE: this field is variable length. (Allocated dynamically
	 * by attaching extra space to the end of the structure,
	 * depending on how many CPUs the kernel has booted up with)
	 */
	/*
	 * cpumask 是动态尾随字段，长度随启动 CPU 数分配；对象发布后只读，生命周期由 ref/拓扑 RCU 保证。
	 */
	unsigned long		cpumask[];
};

/*
 * 业务背景：sched_group_span() 把 sched_group 的尾随位图解释为覆盖 CPU mask。
 * 入参：sg 为借用非空组，调用者保证拓扑生命周期。
 * 出参/返回：返回嵌入 mask 的借用指针；无副作用和引用变化。
 * 注意事项：发布后应只读，返回指针不可越过 RCU/拓扑保护；不睡眠。
 */
static inline struct cpumask *sched_group_span(struct sched_group *sg)
{
	return to_cpumask(sg->cpumask);
}

/*
 * See build_balance_mask().
 */
/* balance mask 由 build_balance_mask() 构建，用于从组内选择承担均衡的 CPU。 */
/*
 * 业务背景：group_balance_mask() 返回 sched_group_capacity 中的均衡 CPU 集合。
 * 入参：sg 为借用非空组，sgc 必须已初始化。
 * 出参/返回：返回 sgc 尾随 mask 借用指针；无副作用。
 * 注意事项：生命周期随拓扑对象，调用者需持 RCU/重建锁；不睡眠。
 */
static inline struct cpumask *group_balance_mask(struct sched_group *sg)
{
	return to_cpumask(sg->sgc->cpumask);
}

extern int group_balance_cpu(struct sched_group *sg);

extern void update_sched_domain_debugfs(void);
extern void dirty_sched_domain_sysctl(int cpu);

extern int sched_update_scaling(void);

/*
 * 业务背景：task_user_cpus() 取得用户亲和性约束；未单独保存时退回全部 possible CPU。
 * 入参：p 为借用非空 task；不增加 user mask 引用。
 * 出参/返回：返回 user_cpus_ptr 或 cpu_possible_mask 借用常量指针；无副作用。
 * 注意事项：任务亲和性更新可替换指针，稳定使用需持相应锁；函数不睡眠。
 */
static inline const struct cpumask *task_user_cpus(struct task_struct *p)
{
	if (!p->user_cpus_ptr)
		return cpu_possible_mask; /* &init_task.cpus_mask */
		/* 未保存用户 mask 时借用 init_task.cpus_mask 所代表的 possible 集合。 */
	return p->user_cpus_ptr;
}

#ifdef CONFIG_CGROUP_SCHED

/*
 * Return the group to which this tasks belongs.
 *
 * We cannot use task_css() and friends because the cgroup subsystem
 * changes that value before the cgroup_subsys::attach() method is called,
 * therefore we cannot pin it and might observe the wrong value.
 *
 * The same is true for autogroup's p->signal->autogroup->tg, the autogroup
 * core changes this before calling sched_move_task().
 *
 * Instead we use a 'copy' which is updated from sched_move_task() while
 * holding both task_struct::pi_lock and rq::lock.
 */
/*
 * 返回任务真实参与调度的 task_group。不能读取 task_css()：cgroup core 会在 attach 回调前先改 CSS，
 * 此时无法固定且可能观察到“新 CSS、旧调度归属”。autogroup 也会提前更新 signal 中的 tg。
 * 因而 sched_move_task() 在同时持 pi_lock 与 rq 锁时更新专用副本 sched_task_group，读者据此得到一致归属。
 *
 * 业务背景：task_group() 为入队、迁移和权重传播读取可锁定的调度组归属。
 * 入参：p 为借用非空 task；不增加组引用。
 * 出参/返回：返回 sched_task_group 借用指针；无副作用。
 * 注意事项：稳定读取需遵循 pi_lock/rq 锁协议，返回生命周期由 cgroup 调度层保证；不睡眠。
 */
static inline struct task_group *task_group(struct task_struct *p)
{
	return p->sched_task_group;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * Defined here to be available before stats.h is included, since
 * stats.h has dependencies on things defined later in this file.
 */
/* cfs_tg_state 必须在包含 stats.h 前定义，因为 stats.h 又依赖本文件后续实体，借此打破声明顺序环。 */
/* 每 CPU 任务组状态把 cfs_rq、其父层代表实体与统计放在同一分配对象，container_of 可相互还原。 */
struct cfs_tg_state {
	struct cfs_rq		cfs_rq;
	struct sched_entity	se;
	struct sched_statistics	stats;
} __no_randomize_layout;

/* Access a specific CPU's cfs_rq from a task group */
/*
 * 业务背景：tg_cfs_rq() 定位任务组在指定 CPU 上拥有的 per-CPU CFS 队列。
 * 入参：tg 为借用非空组；cpu 为有效 CPU 编号。
 * 出参/返回：返回 per-CPU cfs_rq 借用指针；无副作用。
 * 注意事项：组销毁与 CPU 热插需由调用者保护，返回指针不带引用；不睡眠。
 */
static inline struct cfs_rq *tg_cfs_rq(struct task_group *tg, int cpu)
{
	return per_cpu_ptr(tg->cfs_rq, cpu);
}

/*
 * 业务背景：tg_se() 取得任务组在父 cfs_rq 中代表该组的 per-CPU 调度实体。
 * 入参：tg 为借用组；cpu 为有效编号，ownership 不变。
 * 出参/返回：根组无父实体返回 NULL；其他组返回同一 cfs_tg_state 中 se 的借用指针。
 * 注意事项：container_of 依赖共同分配布局；调用者保护组生命周期，函数不睡眠。
 */
static inline struct sched_entity *tg_se(struct task_group *tg, int cpu)
{
	struct cfs_tg_state *state;

	if (is_root_task_group(tg))
		return NULL;

	state = container_of(tg_cfs_rq(tg, cpu), struct cfs_tg_state, cfs_rq);
	return &state->se;
}

/*
 * 业务背景：cfs_rq_se() 从组的子 cfs_rq 反查其在父层排队的 sched_entity。
 * 入参：cfs_rq 为借用非空组队列。
 * 出参/返回：根组返回 NULL，否则返回同一 cfs_tg_state 的 se 借用指针；无副作用。
 * 注意事项：只适用于组调度共同布局，生命周期由 task_group/rq 锁保护；不睡眠。
 */
static inline struct sched_entity *cfs_rq_se(struct cfs_rq *cfs_rq)
{
	struct cfs_tg_state *state;

	if (is_root_task_group(cfs_rq->tg))
		return NULL;

	state = container_of(cfs_rq, struct cfs_tg_state, cfs_rq);
	return &state->se;
}
#endif

/* Change a task's cfs_rq and parent entity if it moves across CPUs/groups */
/*
 * 任务跨 CPU/组时更新 CFS 队列和父实体；RT 组调度同时切换 rt_rq/parent。CFS helper 先结算旧新
 * 队列状态，再替换指针与层级 depth。RT 实体初始 rt_rq 可为 NULL，关闭运行时组调度时强制根组。
 *
 * 业务背景：set_task_rq() 在真正发布新 task CPU 前重建各调度类的层级归属。
 * 入参：p 为调用者借用且已从可见队列稳定的 task；cpu 为有效目标 CPU。
 * 出参/返回：void；修改 p 的 CFS/RT 归属缓存，不转移 task ownership。
 * 注意事项：调用者持任务/rq 迁移锁且不得睡眠；顺序必须早于 __set_task_cpu() 发布新 CPU。
 */
static inline void set_task_rq(struct task_struct *p, unsigned int cpu)
{
#if defined(CONFIG_FAIR_GROUP_SCHED) || defined(CONFIG_RT_GROUP_SCHED)
	struct task_group *tg = task_group(p);
#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* 阶段 1：先结算旧队列并把实体指向目标 CPU 的组队列，再重建父链深度。 */
	set_task_rq_fair(&p->se, p->se.cfs_rq, tg_cfs_rq(tg, cpu));
	p->se.cfs_rq = tg_cfs_rq(tg, cpu);
	p->se.parent = tg_se(tg, cpu);
	p->se.depth = p->se.parent ? p->se.parent->depth + 1 : 0;
#endif

#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * p->rt.rt_rq is NULL initially and it is easier to assign
	 * root_task_group's rt_rq than switching in rt_rq_of_se()
	 * Clobbers tg(!)
	 */
	/*
	 * p->rt.rt_rq 初始为 NULL；直接赋根组队列比让 rt_rq_of_se() 处理 NULL 更简单。
	 * 若运行时关闭 RT 组调度会把局部 tg 改为 root_task_group，随后同时更新 rt_rq 与 parent。
	 */
	if (!rt_group_sched_enabled())
		tg = &root_task_group;
	p->rt.rt_rq  = tg->rt_rq[cpu];
	p->rt.parent = tg->rt_se[cpu];
#endif /* CONFIG_RT_GROUP_SCHED */
	/* RT 组调度的实体父链与 per-CPU rt_rq 绑定到此结束。 */
}

#else /* !CONFIG_CGROUP_SCHED: */

/* cgroup 调度未编译时没有层级归属需要切换；参数为借用占位，void 返回且无副作用。 */
/*
 * 业务背景：set_task_rq() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void set_task_rq(struct task_struct *p, unsigned int cpu) { }

/*
 * 业务背景：无 cgroup 调度时 task_group() 明确表示任务没有可查询的组对象。
 * 入参：p 为未使用借用参数。
 * 出参/返回：固定返回 NULL；无副作用。
 * 注意事项：调用者应走顶层 rq 逻辑，函数不睡眠。
 */
static inline struct task_group *task_group(struct task_struct *p)
{
	return NULL;
}

#endif /* !CONFIG_CGROUP_SCHED */

/*
 * 业务背景：__set_task_cpu() 完成任务迁移的归属发布：先更新类层级数据，再发布 thread_info CPU。
 * 入参：p 为调用者借用且已按迁移协议锁定的 task；cpu 为有效目标 CPU。
 * 出参/返回：void；更新组 rq、task CPU、wake_cpu 和 rseq 迁移标记，ownership 不变。
 * 注意事项：wmb 保证其他 CPU 经 task_rq_lock() 看到新 CPU 时也看到此前 per-task 更新；不可睡眠。
 */
static inline void __set_task_cpu(struct task_struct *p, unsigned int cpu)
{
	set_task_rq(p, cpu);
#ifdef CONFIG_SMP
	/*
	 * After ->cpu is set up to a new value, task_rq_lock(p, ...) can be
	 * successfully executed on another CPU. We must ensure that updates of
	 * per-task data have been completed by this moment.
	 */
	/*
	 * 一旦 ->cpu 发布为新值，另一 CPU 就可能据此成功锁定新 rq；wmb 保证它不会看到新 CPU
	 * 却遗漏此前层级/任务字段更新。WRITE_ONCE 发布编号，随后更新唤醒提示与 rseq 迁移事件。
	 */
	smp_wmb();
	WRITE_ONCE(task_thread_info(p)->cpu, cpu);
	p->wake_cpu = cpu;
	rseq_sched_set_ids_changed(p);
#endif /* CONFIG_SMP */
	/* SMP 下的 CPU 编号发布与迁移事件更新到此结束。 */
}

/*
 * Tunables:
 */
/* 以下为运行时调度特性开关；枚举顺序、静态键数组与 features.h 展开必须严格一致。 */

#define SCHED_FEAT(name, enabled)	\
	__SCHED_FEAT_##name ,

enum {
#include "features.h"
	__SCHED_FEAT_NR,
};

#undef SCHED_FEAT

/*
 * To support run-time toggling of sched features, all the translation units
 * (but core.c) reference the sysctl_sched_features defined in core.c.
 */
/* 为支持运行时切换，除定义者 core.c 外的所有翻译单元都引用同一 sysctl_sched_features 位图。 */
extern __read_mostly unsigned int sysctl_sched_features;

#ifdef CONFIG_JUMP_LABEL

/* features.h 被再次展开为逐特性静态键查询函数，enabled 参数选择 likely/unlikely 默认方向。 */
/*
 * 业务背景：SCHED_FEAT() 为每个特性生成对应的 static_branch 查询函数。
 * 入参：name/enabled 是预处理期标识，生成函数的 key 是借用静态键指针。
 * 出参/返回：生成函数返回静态键当前布尔状态，不修改 ownership。
 * 注意事项：宏体每行续接符不可删除，枚举、键数组和 features.h 展开顺序必须一致。
 */
#define SCHED_FEAT(name, enabled)					\
static __always_inline bool static_branch_##name(struct static_key *key) \
{									\
	return static_key_##enabled(key);				\
}

#include "features.h"
#undef SCHED_FEAT

extern struct static_key sched_feat_keys[__SCHED_FEAT_NR];
/* sched_feat(x) 以枚举下标找到对应静态键，热路径可在关闭时被补丁成无分支代码。 */
#define sched_feat(x) (static_branch_##x(&sched_feat_keys[__SCHED_FEAT_##x]))

#else /* !CONFIG_JUMP_LABEL: */
/* 此分支在无 jump label 时用普通位图实现特性查询。 */

/* 无 jump label 时退化为普通位图读取，功能相同但每次查询保留分支/位运算成本。 */
#define sched_feat(x) (sysctl_sched_features & (1UL << __SCHED_FEAT_##x))

#endif /* !CONFIG_JUMP_LABEL */
/* 调度特性查询的 jump-label 与位图实现选择到此结束。 */

extern struct static_key_false sched_numa_balancing;
/* 两个静态键分别控制 NUMA 自动均衡与 schedstats 热路径插桩。 */
extern struct static_key_false sched_schedstats;

/*
 * 业务背景：global_rt_period() 把全局 RT 配额周期从 sysctl 微秒转换为内部纳秒。
 * 入参：无。
 * 出参/返回：返回 u64 纳秒周期；无副作用。
 * 注意事项：读取瞬时全局配置，不串行化后续预算操作；不睡眠。
 */
static inline u64 global_rt_period(void)
{
	return (u64)sysctl_sched_rt_period * NSEC_PER_USEC;
}

/*
 * 业务背景：global_rt_runtime() 把全局 RT runtime 配置转换为内部纳秒或无限哨兵。
 * 入参：无。
 * 出参/返回：负配置返回 RUNTIME_INF，否则返回微秒换算的纳秒值；无副作用。
 * 注意事项：仅为配置快照，调用者在带宽锁下应用；函数不睡眠。
 */
static inline u64 global_rt_runtime(void)
{
	if (sysctl_sched_rt_runtime < 0)
		return RUNTIME_INF;

	return (u64)sysctl_sched_rt_runtime * NSEC_PER_USEC;
}

/*
 * Is p the current execution context?
 */
/*
 * 判断 p 是否为 rq 当前真正占用 CPU 的执行上下文；proxy execution 下它可不同于 donor。
 * 入参 rq/p 均为借用非空对象；返回相等布尔值，无副作用。稳定判断须持 rq 锁，函数不睡眠。
 */
/*
 * 业务背景：task_current() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int task_current(struct rq *rq, struct task_struct *p)
{
	return rq->curr == p;
}

/*
 * Is p the current scheduling context?
 *
 * Note that it might be the current execution context at the same time if
 * rq->curr == rq->donor == p.
 */
/*
 * 判断 p 是否提供 rq 当前调度属性/优先级的 donor 上下文；它也可能同时等于执行上下文 curr。
 * rq/p 均为借用，返回相等布尔值且无副作用；稳定读取需持 rq 锁，函数不睡眠。
 */
/*
 * 业务背景：task_current_donor() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int task_current_donor(struct rq *rq, struct task_struct *p)
{
	return rq->donor == p;
}

/*
 * 业务背景：task_is_blocked() 在 proxy execution 启用时判断任务是否阻塞在另一任务/锁上。
 * 入参：p 为借用非空 task；不增加 blocked_on 引用。
 * 出参/返回：特性关闭恒假，否则返回 blocked_on 是否非空；无副作用。
 * 注意事项：指针是状态提示，稳定遍历依赖 PI/调度锁；函数不睡眠。
 */
static inline bool task_is_blocked(struct task_struct *p)
{
	if (!sched_proxy_exec())
		return false;

	return !!p->blocked_on;
}

/*
 * 业务背景：task_on_cpu() 查询任务是否处于某 CPU 执行/切换协议的 on_cpu 状态。
 * 入参：rq 为接口上下文借用参数，p 为借用 task。
 * 出参/返回：返回 p->on_cpu 快照；无副作用。
 * 注意事项：稳定性由 rq/task 锁和架构切换屏障保证；函数不睡眠。
 */
static inline int task_on_cpu(struct rq *rq, struct task_struct *p)
{
	return p->on_cpu;
}

/*
 * 业务背景：task_on_rq_queued() 区分任务已稳定入队与迁移中状态。
 * 入参：p 为借用非空 task。
 * 出参/返回：READ_ONCE(on_rq) 等于 QUEUED 返回真；无副作用。
 * 注意事项：无锁结果可能过期，状态转换仍需 pi/rq 锁；不睡眠。
 */
static inline int task_on_rq_queued(struct task_struct *p)
{
	return READ_ONCE(p->on_rq) == TASK_ON_RQ_QUEUED;
}

/*
 * 业务背景：task_on_rq_migrating() 检测任务已从旧 rq 摘除但尚未稳定发布到新 rq 的窗口。
 * 入参：p 为借用非空 task。
 * 出参/返回：READ_ONCE(on_rq) 等于 MIGRATING 返回真；无副作用。
 * 注意事项：观察者须按迁移协议等待/重试，不能把它当普通离队；不睡眠。
 */
static inline int task_on_rq_migrating(struct task_struct *p)
{
	return READ_ONCE(p->on_rq) == TASK_ON_RQ_MIGRATING;
}

/* Wake flags. The first three directly map to some SD flag value */
/* 唤醒标志：前三项直接映射 sched_domain balance 标志，static_assert 保证数值 ABI 一致。 */
#define WF_EXEC			0x02 /* Wakeup after exec; maps to SD_BALANCE_EXEC */
/* exec 后唤醒，对应 SD_BALANCE_EXEC。 */
#define WF_FORK			0x04 /* Wakeup after fork; maps to SD_BALANCE_FORK */
/* fork 后首次唤醒，对应 SD_BALANCE_FORK。 */
#define WF_TTWU			0x08 /* Wakeup;            maps to SD_BALANCE_WAKE */
/* 普通 try_to_wake_up 唤醒，对应 SD_BALANCE_WAKE。 */

#define WF_SYNC			0x10 /* Waker goes to sleep after wakeup */
/* 唤醒者随后将睡眠，放置策略可利用同步交接局部性。 */
#define WF_MIGRATED		0x20 /* Internal use, task got migrated */
/* 内部标志：wakee 在本次唤醒选择中发生迁移。 */
#define WF_CURRENT_CPU		0x40 /* Prefer to move the wakee to the current CPU. */
/* 倾向把 wakee 放到当前 CPU，但仍受亲和性和容量约束。 */
#define WF_RQ_SELECTED		0x80 /* ->select_task_rq() was called */
/* 已调用 select_task_rq()，后续不得把未选择路径的假设套用。 */

static_assert(WF_EXEC == SD_BALANCE_EXEC);
static_assert(WF_FORK == SD_BALANCE_FORK);
static_assert(WF_TTWU == SD_BALANCE_WAKE);

/*
 * To aid in avoiding the subversion of "niceness" due to uneven distribution
 * of tasks with abnormal "nice" values across CPUs the contribution that
 * each task makes to its run queue's load is weighted according to its
 * scheduling class and "nice" value. For SCHED_NORMAL tasks this is just a
 * scaled version of the new time slice allocation that they receive on time
 * slice expiry etc.
 */
/*
 * 为防异常 nice 任务在 CPU 间分布不均而颠覆 nice 语义，每个任务对 rq load 的贡献按调度类和
 * nice 加权；SCHED_NORMAL 的权重对应时间片到期后可获得份额的缩放表示。
 */

#define WEIGHT_IDLEPRIO		3
#define WMULT_IDLEPRIO		1431655765

extern const int		sched_prio_to_weight[40];
extern const u32		sched_prio_to_wmult[40];

/*
 * {de,en}queue flags:
 *
 * SLEEP/WAKEUP - task is no-longer/just-became runnable
 *
 * SAVE/RESTORE - an otherwise spurious dequeue/enqueue, done to ensure tasks
 *                are in a known state which allows modification. Such pairs
 *                should preserve as much state as possible.
 *
 * MOVE - paired with SAVE/RESTORE, explicitly does not preserve the location
 *        in the runqueue. IOW the priority is allowed to change. Callers
 *        must expect to deal with balance callbacks.
 *
 * NOCLOCK - skip the update_rq_clock() (avoids double updates)
 *
 * MIGRATION - p->on_rq == TASK_ON_RQ_MIGRATING (used for DEADLINE)
 *
 * DELAYED - de/re-queue a sched_delayed task
 *
 * CLASS - going to update p->sched_class; makes sched_change call the
 *         various switch methods.
 *
 * ENQUEUE_HEAD      - place at front of runqueue (tail if not specified)
 * ENQUEUE_REPLENISH - CBS (replenish runtime and postpone deadline)
 * ENQUEUE_MIGRATED  - the task was migrated during wakeup
 * ENQUEUE_RQ_SELECTED - ->select_task_rq() was called
 *
 * XXX SAVE/RESTORE in combination with CLASS doesn't really make sense, but
 * SCHED_DEADLINE seems to rely on this for now.
 */
/*
 * 入/出队 flags 描述状态转换契约：SLEEP/WAKEUP 改变 runnable；SAVE/RESTORE 是为安全改属性的
 * 临时摘除并尽量保留位置；MOVE 允许改变队列位置并可能产生 balance callback；NOCLOCK 避免重复
 * 更新时钟；MIGRATING 对应 on_rq 迁移态；DELAYED 处理延迟实体；CLASS 通知 sched_change 调用类
 * 切换回调。HEAD/REPLENISH/MIGRATED/RQ_SELECTED 分别控制头插、CBS 补充、唤醒迁移和已选 rq。
 * FIXME：SAVE/RESTORE 与 CLASS 组合概念上不自然，但当前 SCHED_DEADLINE 仍依赖它。
 */

#define DEQUEUE_SLEEP		0x0001 /* Matches ENQUEUE_WAKEUP */
/* DEQUEUE_SLEEP 与 ENQUEUE_WAKEUP 描述睡眠后再次唤醒的一对转换。 */
#define DEQUEUE_SAVE		0x0002 /* Matches ENQUEUE_RESTORE */
/* DEQUEUE_SAVE 与 ENQUEUE_RESTORE 描述保留位置的临时摘除与恢复。 */
#define DEQUEUE_MOVE		0x0004 /* Matches ENQUEUE_MOVE */
/* DEQUEUE_MOVE 与 ENQUEUE_MOVE 描述允许改变队列位置的摘除与重入。 */
#define DEQUEUE_NOCLOCK		0x0008 /* Matches ENQUEUE_NOCLOCK */

/* 迁移、延迟出队和换类是独立状态维度，可与基础 SAVE/MOVE 语义组合。 */
#define DEQUEUE_MIGRATING	0x0010 /* Matches ENQUEUE_MIGRATING */
/* DEQUEUE_MIGRATING 与 ENQUEUE_MIGRATING 配对表示跨 rq 迁移。 */
#define DEQUEUE_DELAYED		0x0020 /* Matches ENQUEUE_DELAYED */
/* DEQUEUE_DELAYED 与 ENQUEUE_DELAYED 配对表示延迟出队实体的状态转换。 */
#define DEQUEUE_CLASS		0x0040 /* Matches ENQUEUE_CLASS */
/* DEQUEUE_CLASS 与 ENQUEUE_CLASS 配对表示调度类切换。 */

#define DEQUEUE_SPECIAL		0x00010000
#define DEQUEUE_THROTTLE	0x00020000

/* 入队低位与出队低位成对，保证临时摘除后能按同一种原因恢复。 */
#define ENQUEUE_WAKEUP		0x0001
#define ENQUEUE_RESTORE		0x0002
#define ENQUEUE_MOVE		0x0004
#define ENQUEUE_NOCLOCK		0x0008

#define ENQUEUE_MIGRATING	0x0010
#define ENQUEUE_DELAYED		0x0020
#define ENQUEUE_CLASS		0x0040

/* 高位只改变具体插入/补充策略，不替代 WAKEUP、RESTORE 等基本生命周期原因。 */
#define ENQUEUE_HEAD		0x00010000
#define ENQUEUE_REPLENISH	0x00020000
#define ENQUEUE_MIGRATED	0x00040000
#define ENQUEUE_INITIAL		0x00080000
#define ENQUEUE_RQ_SELECTED	0x00100000

#define RETRY_TASK		((void *)-1UL)

/*
 * 亲和性更新事务上下文：new_mask 是本次生效的借用输入，user_mask 可承接用户请求副本，
 * flags 区分检查、migrate_disable/enable 与用户调用。分配的 user_mask 由亲和性提交/回滚路径接管。
 */
struct affinity_context {
	const struct cpumask	*new_mask;
	struct cpumask		*user_mask;
	unsigned int		flags;
};

extern s64 update_curr_common(struct rq *rq);

/*
 * 调度类虚表。每个回调上方列出的锁是 ABI 的一部分；实现不能假定比调用点
 * 更多的保护。链接脚本按优先级排列实例，遍历顺序就是跨类抢占顺序。
 */
struct sched_class {

#ifdef CONFIG_UCLAMP_TASK
	int uclamp_enabled;
#endif

	/*
	 * move_queued_task/activate_task/enqueue_task: rq->lock
	 * ttwu_do_activate/activate_task/enqueue_task: rq->lock
	 * wake_up_new_task/activate_task/enqueue_task: task_rq_lock
	 * ttwu_runnable/enqueue_task: task_rq_lock
	 * proxy_task_current: rq->lock
	 * sched_change_end
	 */
	/* enqueue/dequeue 在 rq 或 task_rq 锁下改变类队列可见性，flags 说明唤醒、迁移、换类等原因。 */
	void (*enqueue_task) (struct rq *rq, struct task_struct *p, int flags);
	/*
	 * move_queued_task/deactivate_task/dequeue_task: rq->lock
	 * __schedule/block_task/dequeue_task: rq->lock
	 * proxy_task_current: rq->lock
	 * wait_task_inactive: task_rq_lock
	 * sched_change_begin
	 */
	/* dequeue 返回是否真正移除实体；延迟出队类可保留逻辑状态并由返回值通知上层。 */
	bool (*dequeue_task) (struct rq *rq, struct task_struct *p, int flags);

	/*
	 * do_sched_yield: rq->lock
	 */
	/* yield_task 让当前类内任务主动让出，yield_to_task 尝试向指定任务定向交接。 */
	void (*yield_task)   (struct rq *rq);
	/*
	 * yield_to: rq->lock (double)
	 */
	/* yield_to_task 需要同时满足本 rq 与目标任务 rq 的双锁协议。 */
	bool (*yield_to_task)(struct rq *rq, struct task_struct *p);

	/*
	 * move_queued_task: rq->lock
	 * __migrate_swap_task: rq->lock
	 * ttwu_do_activate: rq->lock
	 * ttwu_runnable: task_rq_lock
	 * wake_up_new_task: task_rq_lock
	 */
	/* 新任务入队后比较 current/donor 与 wakee，必要时设置 resched 或执行类特定抢占处理。 */
	void (*wakeup_preempt)(struct rq *rq, struct task_struct *p, int flags);

	/*
	 * schedule/pick_next_task/prev_balance: rq->lock
	 */
	/* balance 可暂放/重取 rq 锁补充候选；返回值告诉核心层该类是否准备好继续选择。 */
	int (*balance)(struct rq *rq, struct rq_flags *rf);

	/*
	 * schedule/pick_next_task: rq->lock
	 */
	/* pick_task 返回借用候选或 NULL，不自行完成 curr 发布，后续由 set_next_task 接管。 */
	struct task_struct *(*pick_task)(struct rq *rq, struct rq_flags *rf);

	/*
	 * sched_change:
	 * __schedule: rq->lock
	 */
	/* put_prev 结算离开实体，set_next 发布新实体；两者在同一 rq 锁下构成类切换提交。 */
	void (*put_prev_task)(struct rq *rq, struct task_struct *p, struct task_struct *next);
	void (*set_next_task)(struct rq *rq, struct task_struct *p, bool first);

	/*
	 * select_task_rq: p->pi_lock
	 * sched_exec: p->pi_lock
	 */
	/* select_task_rq 在亲和性/拓扑约束内返回目标 CPU，结果在真正迁移前仍需复验。 */
	int  (*select_task_rq)(struct task_struct *p, int task_cpu, int flags);

	/*
	 * set_task_cpu: p->pi_lock || rq->lock (ttwu like)
	 */
	/* migrate_task_rq 在 task CPU 发布前搬移该类的 per-rq 派生状态，不能自行入队。 */
	void (*migrate_task_rq)(struct task_struct *p, int new_cpu);

	/*
	 * ttwu_do_activate: rq->lock
	 * wake_up_new_task: task_rq_lock
	 */
	/* task_woken 在完成入队后执行类特定后处理，输入 task 已由 this_rq 锁稳定。 */
	void (*task_woken)(struct rq *this_rq, struct task_struct *task);

	/*
	 * do_set_cpus_allowed: task_rq_lock + sched_change
	 */
	/* set_cpus_allowed 把 affinity_context 提交到类私有状态，失败校验由更上层事务处理。 */
	void (*set_cpus_allowed)(struct task_struct *p, struct affinity_context *ctx);

	/*
	 * sched_set_rq_{on,off}line: rq->lock
	 */
	/* CPU 热插拔在 rq 锁下通知类加入/退出可调度集合，类须同步维护全局推拉索引。 */
	void (*rq_online)(struct rq *rq);
	void (*rq_offline)(struct rq *rq);

	/*
	 * push_cpu_stop: p->pi_lock && rq->lock
	 */
	/* find_lock_rq 尝试找到并锁住迁移目标，失败返回 NULL，成功返回值携带目标 rq 锁。 */
	struct rq *(*find_lock_rq)(struct task_struct *p, struct rq *rq);

	/*
	 * hrtick: rq->lock
	 * sched_tick: rq->lock
	 * sched_tick_remote: rq->lock
	 */
	/* task_tick 消费周期/高精 timer 事件并更新片额、runtime 与抢占需求。 */
	void (*task_tick)(struct rq *rq, struct task_struct *p, int queued);
	/*
	 * sched_cgroup_fork: p->pi_lock
	 */
	/* task_fork 初始化新任务的类私有状态；此时任务尚未发布到运行队列。 */
	void (*task_fork)(struct task_struct *p);
	/*
	 * finish_task_switch: no locks
	 */
	/* task_dead 在任务已退出类队列后做最终类清理，不能再依赖 rq 锁保护其可运行状态。 */
	void (*task_dead)(struct task_struct *p);

	/*
	 * sched_change
	 */
	/* switching_* 在换类事务两侧准备/提交状态，get_prio/prio_changed 维护类间优先级可见性。 */
	void (*switching_from)(struct rq *this_rq, struct task_struct *task);
	void (*switched_from) (struct rq *this_rq, struct task_struct *task);
	void (*switching_to)  (struct rq *this_rq, struct task_struct *task);
	void (*switched_to)   (struct rq *this_rq, struct task_struct *task);
	u64  (*get_prio)     (struct rq *this_rq, struct task_struct *task);
	void (*prio_changed) (struct rq *this_rq, struct task_struct *task,
			      u64 oldprio);

	/*
	 * set_load_weight: task_rq_lock + sched_change
	 * __setscheduler_parms: task_rq_lock + sched_change
	 */
	/* reweight_task 在任务不可被类误观察的换类事务内替换 load，并修正队列聚合量。 */
	void (*reweight_task)(struct rq *this_rq, struct task_struct *task,
			      const struct load_weight *lw);

	/*
	 * sched_rr_get_interval: task_rq_lock
	 */
	/* 返回任务当前类的 RR 时间片长度；零可表示该策略没有固定 RR 间隔。 */
	unsigned int (*get_rr_interval)(struct rq *rq,
					struct task_struct *task);

	/*
	 * task_sched_runtime: task_rq_lock
	 */
	/* update_curr 把 rq 时钟增量结算给当前类实体，调用前必须已有新鲜 rq clock。 */
	void (*update_curr)(struct rq *rq);

#ifdef CONFIG_FAIR_GROUP_SCHED
	/*
	 * sched_change_group: task_rq_lock + sched_change
	 */
	/* task_change_group 在任务组切换事务中重建公平层级归属与负载传播状态。 */
	void (*task_change_group)(struct task_struct *p);
#endif

#ifdef CONFIG_SCHED_CORE
	/*
	 * pick_next_task: rq->lock
	 * try_steal_cookie: rq->lock (double)
	 */
	/* core 调度询问任务在指定 CPU 是否因类带宽而不可运行，决定 cookie 偷取是否安全。 */
	int (*task_is_throttled)(struct task_struct *p, int cpu);
#endif
};

/*
 * 业务背景：put_prev_task() 结算当前 donor 离开 CPU，并把工作分派给其调度类。
 * 入参：rq 为已锁借用队列；prev 为当前 donor 借用任务。
 * 出参/返回：void；类回调更新运行时间和队列状态，不转移 task ownership。
 * 注意事项：donor 不一致会 WARN；调用者持 rq 锁且已有新鲜时钟，函数不可睡眠。
 */
static inline void put_prev_task(struct rq *rq, struct task_struct *prev)
{
	WARN_ON_ONCE(rq->donor != prev);
	prev->sched_class->put_prev_task(rq, prev, NULL);
}

/*
 * 业务背景：set_next_task() 通知目标类把 next 建为继续运行的实体，非完整跨任务切换。
 * 入参：rq 为已锁借用队列；next 为已选中的借用任务。
 * 出参/返回：void；类回调提交 current 状态，无引用转移。
 * 注意事项：first=false 表示不是 put_prev 后的首次切换提交；函数不可睡眠。
 */
static inline void set_next_task(struct rq *rq, struct task_struct *next)
{
	next->sched_class->set_next_task(rq, next, false);
}

/*
 * 业务背景：__put_prev_set_next_dl_server() 把 rq 暂存的 DL server 上下文从 prev 转交 next。
 * 入参：rq/prev/next 均为 rq 锁下借用对象。
 * 出参/返回：void；清空 prev，写入 next，并消费 rq->dl_server。
 * 注意事项：必须恰好执行一次，否则 server 会丢失或重复归属；函数不可睡眠。
 */
static inline void
__put_prev_set_next_dl_server(struct rq *rq,
			      struct task_struct *prev,
			      struct task_struct *next)
{
	prev->dl_server = NULL;
	next->dl_server = rq->dl_server;
	rq->dl_server = NULL;
}

/* 同 rq 锁内完成前后类交接；next==prev 只转移 DL server 上下文。 */
/*
 * 业务背景：put_prev_set_next_task() 在同一 rq 锁事务内结算 prev 并发布 next。
 * 入参：rq 为已锁队列；prev 是当前 donor，next 是已选借用任务，二者可相同。
 * 出参/返回：void；总是转移 DL server；不同任务时依序调用两个类回调，无引用变化。
 * 注意事项：next==prev 只需 server 交接；回调顺序不能交换，函数不可睡眠。
 */
static inline void put_prev_set_next_task(struct rq *rq,
					  struct task_struct *prev,
					  struct task_struct *next)
{
	WARN_ON_ONCE(rq->donor != prev);

	__put_prev_set_next_dl_server(rq, prev, next);

	/* 同一任务继续运行时类私有 current 状态未离开，只需完成 server 上下文转移。 */
	if (next == prev)
		return;

	prev->sched_class->put_prev_task(rq, prev, next);
	/* prev 完全结算后才发布 next，防止两个类同时认为自己占有 CPU。 */
	next->sched_class->set_next_task(rq, next, true);
}

/*
 * Helper to define a sched_class instance; each one is placed in a separate
 * section which is ordered by the linker script:
 *
 *   include/asm-generic/vmlinux.lds.h
 *
 * *CAREFUL* they are laid out in *REVERSE* order!!!
 *
 * Also enforce alignment on the instance, not the type, to guarantee layout.
 */
/*
 * DEFINE_SCHED_CLASS() 把每个类实例放入链接脚本按优先级反向排列的独立 section，并在实例上
 * 强制对齐；地址比较和顺序遍历因此等价于类优先级。宏只定义对象，不创建运行期 ownership。
 */
#define DEFINE_SCHED_CLASS(name) \
const struct sched_class name##_sched_class \
	__aligned(__alignof__(struct sched_class)) \
	__section("__" #name "_sched_class")

/* Defined in include/asm-generic/vmlinux.lds.h */
/* 两个链接器边界围住全部 sched_class 实例，highest 地址低于 lowest，范围为半开区间。 */
extern struct sched_class __sched_class_highest[];
extern struct sched_class __sched_class_lowest[];

extern const struct sched_class stop_sched_class;
extern const struct sched_class dl_sched_class;
/* 固定类顺序为 stop -> DL -> RT -> fair/SCX -> idle，SCX 的活动性由静态键动态筛选。 */
extern const struct sched_class rt_sched_class;
extern const struct sched_class fair_sched_class;
extern const struct sched_class idle_sched_class;

/*
 * Iterate only active classes. SCX can take over all fair tasks or be
 * completely disabled. If the former, skip fair. If the latter, skip SCX.
 */
/* 根据 SCX 静态键跳过被接管的 fair 或未启用的 ext 类。 */
/*
 * 业务背景：next_active_class() 沿链接顺序返回下一个当前可参与选取的类。
 * 入参：class 为链接器区间内的借用指针。
 * 出参/返回：返回下一个活动类或区间终点；无引用和队列副作用。
 * 注意事项：SCX 静态键决定跳过 fair/ext，调用者必须以 lowest 为终止哨兵；函数不睡眠。
 */
static inline const struct sched_class *next_active_class(const struct sched_class *class)
{
	class++;
#ifdef CONFIG_SCHED_CLASS_EXT
	/* SCX 接管全部 fair 任务时跳过 fair；SCX 未加载时反向跳过 ext。 */
	if (scx_switched_all() && class == &fair_sched_class)
		class++;
	if (!scx_enabled() && class == &ext_sched_class)
		class++;
#endif
	return class;
}

#define for_class_range(class, _from, _to) \
	for (class = (_from); class < (_to); class++)

/* for_each_class() 覆盖链接器区间内所有类，包括运行期可能被静态键跳过的实例。 */
#define for_each_class(class) \
	for_class_range(class, __sched_class_highest, __sched_class_lowest)

/* active 版本通过 next_active_class() 跨过运行期无候选的 fair/ext 实例。 */
#define for_active_class_range(class, _from, _to)				\
	for (class = (_from); class != (_to); class = next_active_class(class))

#define for_each_active_class(class)						\
	for_active_class_range(class, __sched_class_highest, __sched_class_lowest)

#define sched_class_above(_a, _b)	((_a) < (_b))

/*
 * 业务背景：rq_modified_begin() 记录本轮队列修改可能影响到的最高优先级类，缩小后续重选范围。
 * 入参：rq 为已锁输入输出队列；class 为本次被修改类的静态借用指针。
 * 出参/返回：void；必要时向高优先级方向更新 next_class。
 * 注意事项：依赖链接地址顺序且必须在 rq 锁下调用；不睡眠。
 */
static inline void rq_modified_begin(struct rq *rq, const struct sched_class *class)
{
	if (sched_class_above(rq->next_class, class))
		rq->next_class = class;
}

/* rq_modified_above() 只读判断已有修改是否高于 class；rq/class 均借用，返回布尔值且要求 rq 锁稳定。 */
/*
 * 业务背景：rq_modified_above() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool rq_modified_above(struct rq *rq, const struct sched_class *class)
{
	return sched_class_above(rq->next_class, class);
}

/* stop 类仅在 stop task 存在且稳定入队时 runnable；rq 为锁下借用，返回快照且无副作用。 */
/*
 * 业务背景：sched_stop_runnable() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_stop_runnable(struct rq *rq)
{
	return rq->stop && task_on_rq_queued(rq->stop);
}

/* DL runnable 由 rq 锁保护的 dl_nr_running 计数决定；返回布尔值，不取得实体引用。 */
/*
 * 业务背景：sched_dl_runnable() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_dl_runnable(struct rq *rq)
{
	return rq->dl.dl_nr_running > 0;
}

/* RT 使用已向父层发布的 rt_queued，而非仅看叶任务数；调用者持 rq 锁，函数无副作用。 */
/*
 * 业务背景：sched_rt_runnable() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_rt_runnable(struct rq *rq)
{
	return rq->rt.rt_queued > 0;
}

/* fair 使用直接队列实体数 nr_queued 判断；延迟实体语义已由 CFS 维护在该计数中。 */
/*
 * 业务背景：sched_fair_runnable() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_fair_runnable(struct rq *rq)
{
	return rq->cfs.nr_queued > 0;
}

extern struct task_struct *pick_task_fair(struct rq *rq, struct rq_flags *rf);
extern struct task_struct *pick_task_idle(struct rq *rq, struct rq_flags *rf);

/* SCA flags 区分仅检查、进入/退出 migrate_disable 与用户亲和性请求，可组合传入 affinity 事务。 */
#define SCA_CHECK		0x01
#define SCA_MIGRATE_DISABLE	0x02
#define SCA_MIGRATE_ENABLE	0x04
#define SCA_USER		0x08

extern void update_group_capacity(struct sched_domain *sd, int cpu);

extern void sched_balance_trigger(struct rq *rq);

/* 亲和性提交接口在 task_rq_lock 下验证 mask、迁移任务并维护 user mask ownership。 */
extern int __set_cpus_allowed_ptr(struct task_struct *p, struct affinity_context *ctx);
extern void set_cpus_allowed_common(struct task_struct *p, struct affinity_context *ctx);

/* 同时检查动态亲和性和用户任务的 possible mask；结果仍受热插拔竞态影响。 */
/*
 * 业务背景：task_allowed_on_cpu() 过滤不在动态亲和性内或不能承载用户线程的 CPU。
 * 入参：p 为借用非空 task；cpu 是候选有效编号。
 * 出参/返回：两项约束均满足返回真，否则返回假；无副作用。
 * 注意事项：结果受亲和性与热插拔竞态影响，真正迁移须锁下复验；函数不睡眠。
 */
static inline bool task_allowed_on_cpu(struct task_struct *p, int cpu)
{
	/* When not in the task's cpumask, no point in looking further. */
	/* 不在 cpus_ptr 时后续任何容量/拓扑检查都无意义，立即拒绝。 */
	if (!cpumask_test_cpu(cpu, p->cpus_ptr))
		return false;

	/* Can @cpu run a user thread? */
	/* 内核线程可使用仅内核 CPU；用户线程还必须满足 task_cpu_possible()。 */
	if (!(p->flags & PF_KTHREAD) && !task_cpu_possible(cpu, p))
		return false;

	return true;
}

/*
 * 业务背景：alloc_user_cpus_ptr() 为可由 RCU 延迟释放的用户 affinity mask 分配存储。
 * 入参：node 是 NUMA 分配节点，可为允许回退的节点编号。
 * 出参/返回：成功返回调用者拥有的 cpumask 缓冲区，失败返回 NULL。
 * 注意事项：空间至少容纳 rcu_head 以复用释放布局；GFP_KERNEL 可睡眠，调用者负责释放/转移所有权。
 */
static inline cpumask_t *alloc_user_cpus_ptr(int node)
{
	/*
	 * See set_cpus_allowed_force() above for the rcu_head usage.
	 */
	/* 分配尺寸沿用上方 set_cpus_allowed_force() 对 rcu_head 复用空间的约定。 */
	int size = max_t(int, cpumask_size(), sizeof(struct rcu_head));

	return kmalloc_node(size, GFP_KERNEL, node);
}

/* rq 锁下取得当前 donor 引用并设置单飞标志；失败表示不可迁移或已有 push。 */
/*
 * 业务背景：get_push_task() 为 CPU stop 推送路径领取当前 donor，并用 push_busy 保证单飞。
 * 入参：rq 为已锁借用队列。
 * 出参/返回：不可推返回 NULL；成功返回持有引用的 task，调用者必须 put_task_struct()。
 * 注意事项：单 CPU affinity 或 migrate_disable 均禁止推送；成功会设置 push_busy，完成路径必须清除。
 */
static inline struct task_struct *get_push_task(struct rq *rq)
{
	struct task_struct *p = rq->donor;

	lockdep_assert_rq_held(rq);

	/* 已有 stop push 在途时拒绝第二个领取者，避免两个 worker 同迁一个 donor。 */
	if (rq->push_busy)
		return NULL;

	if (p->nr_cpus_allowed == 1)
		return NULL;

	/* migrate_disable 区间要求任务留在当前 CPU，即使静态 affinity 允许多个 CPU。 */
	if (p->migration_disabled)
		return NULL;

	rq->push_busy = true;
	/* 引用跨越 rq 放锁与异步 cpu_stop 工作，完成路径负责 put 并清单飞位。 */
	return get_task_struct(p);
}

extern int push_cpu_stop(void *arg);

#ifdef CONFIG_CPU_IDLE

/* rq 锁/idle 协议下发布当前 cpuidle_state 借用指针；void 返回，不取得 state 引用。 */
/*
 * 业务背景：idle_set_state() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
	rq->idle_state = idle_state;
}

/* RCU 读侧取得 idle_state 借用快照；返回可为 NULL，离开 RCU 后不得继续解引用。 */
/*
 * 业务背景：idle_get_state() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	lockdep_assert(rcu_read_lock_any_held());

	return rq->idle_state;
}

#else /* !CONFIG_CPU_IDLE: */
/* 此分支提供未编译 CPU idle 时的空实现。 */

/* CPU_IDLE 未编译时 setter 是无副作用 stub，rq/state 参数仅保持公共调用点。 */
/*
 * 业务背景：idle_set_state() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
}

/* CPU_IDLE 未编译时没有状态对象，getter 固定返回 NULL 且不睡眠。 */
/*
 * 业务背景：idle_get_state() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	return NULL;
}

#endif /* !CONFIG_CPU_IDLE */
/* cpuidle 状态访问 helper 的配置分支到此结束。 */

extern void schedule_idle(void);
asmlinkage void schedule_user(void);

/* 调试/粒度接口输出 rq 状态并初始化或重算调度时间尺度。 */
extern void sysrq_sched_debug_show(void);
extern void sched_init_granularity(void);
extern void update_max_interval(void);

extern void init_sched_dl_class(void);
extern void init_sched_rt_class(void);
extern void init_sched_fair_class(void);

/* resched 接口分别请求当前 rq 立即/延迟抢占，或向指定 CPU 发送重调度通知。 */
extern void resched_curr(struct rq *rq);
extern void resched_curr_lazy(struct rq *rq);
extern void resched_cpu(int cpu);

extern void init_rt_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime);
extern bool sched_rt_bandwidth_account(struct rt_rq *rt_rq);

/* DL 实体和 CFS 节流 work 的初始化必须在对象发布/任务可运行之前完成。 */
extern void init_dl_entity(struct sched_dl_entity *dl_se);

extern void init_cfs_throttle_work(struct task_struct *p);

#define BW_SHIFT		20
#define BW_UNIT			(1 << BW_SHIFT)
#define RATIO_SHIFT		8
#define MAX_BW_BITS		(64 - BW_SHIFT)
#define MAX_BW			((1ULL << MAX_BW_BITS) - 1)

/* to_ratio() 把 runtime/period 转为 BW_SHIFT 定点比例，调用者须避免非法零周期。 */
extern u64 to_ratio(u64 period, u64 runtime);

/* PELT 初始化建立零基线，post_init 再用已有队列/任务状态校正初始 util。 */
extern void init_entity_runnable_average(struct sched_entity *se);
extern void post_init_entity_util_avg(struct task_struct *p);

#ifdef CONFIG_NO_HZ_FULL
extern bool sched_can_stop_tick(struct rq *rq);
extern int __init sched_tick_offload_init(void);

/*
 * Tick may be needed by tasks in the runqueue depending on their policy and
 * requirements. If tick is needed, lets send the target an IPI to kick it out of
 * nohz mode if necessary.
 */
/*
 * 任务策略仍需要 tick 时为目标 CPU 设置 SCHED dependency 并用 IPI 退出 nohz_full；若所有类都允许
 * 停 tick 则清除依赖。rq 是借用输入，函数修改 tick 子系统状态，不转移 ownership 且不可睡眠。
 */
/*
 * 业务背景：sched_update_tick_dependency() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_update_tick_dependency(struct rq *rq)
{
	int cpu = cpu_of(rq);

	if (!tick_nohz_full_cpu(cpu))
		return;

	/* 只有 full-nohz CPU 维护该 dependency；普通 tick CPU 不需要额外 IPI 状态。 */
	if (sched_can_stop_tick(rq))
		tick_nohz_dep_clear_cpu(cpu, TICK_DEP_BIT_SCHED);
	else
		tick_nohz_dep_set_cpu(cpu, TICK_DEP_BIT_SCHED);
}
#else /* !CONFIG_NO_HZ_FULL: */
/* 未启用 full dynticks 时无 offload 工作：初始化成功，依赖更新为 no-op。 */
/*
 * 业务背景：sched_tick_offload_init() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int sched_tick_offload_init(void) { return 0; }
/*
 * 业务背景：sched_update_tick_dependency() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_update_tick_dependency(struct rq *rq) { }
#endif /* !CONFIG_NO_HZ_FULL */

/* rq 锁下增加总 runnable；跨过 2 的阈值发布 root_domain 过载提示。 */
/*
 * 业务背景：add_nr_running() 在 rq 锁下增加 runnable 总量，并同步 trace、过载提示和 tick 依赖。
 * 入参：rq 为已锁输入输出队列；count 为正向增加的任务数。
 * 出参/返回：void；更新计数，跨过 2 时发布 rd overloaded，无引用变化。
 * 注意事项：调用者保证不溢出且与 dequeue 配对；函数不可睡眠。
 */
static inline void add_nr_running(struct rq *rq, unsigned count)
{
	unsigned prev_nr = rq->nr_running;

	rq->nr_running = prev_nr + count;
	/* tracepoint 用相同增量记录状态变化，但不参与计数正确性。 */
	if (trace_sched_update_nr_running_tp_enabled()) {
		call_trace_sched_update_nr_running(rq, count);
	}

	if (prev_nr < 2 && rq->nr_running >= 2)
		/* 首次形成可拉取任务时发布 root_domain 提示；迁移方仍会锁下复验。 */
		set_rd_overloaded(rq->rd, 1);

	sched_update_tick_dependency(rq);
}

/* 减少 runnable 并重算 nohz tick 依赖；过载清理由均衡路径精确处理。 */
/*
 * 业务背景：sub_nr_running() 在 rq 锁下撤销 runnable 贡献，并重算 trace/nohz tick 需求。
 * 入参：rq 为已锁队列；count 不得大于当前 nr_running。
 * 出参/返回：void；减少计数，不在此清 rd overloaded。
 * 注意事项：过载清理由拥有完整均衡状态的路径完成；下溢会破坏全局负载，函数不可睡眠。
 */
static inline void sub_nr_running(struct rq *rq, unsigned count)
{
	rq->nr_running -= count;
	if (trace_sched_update_nr_running_tp_enabled()) {
		call_trace_sched_update_nr_running(rq, -count);
	}

	/* Check if we still need preemption */
	/* runnable 减少后可能允许停 tick，必须重新发布 dependency。 */
	sched_update_tick_dependency(rq);
}

/* release 清零 on_rq 后任务所有权可立即转给唤醒方，调用者不得再解引用 p。 */
/*
 * 业务背景：__block_task() 完成 schedule 阻塞提交，记账后以 release 清除 on_rq，把任务交给唤醒方。
 * 入参：rq 为已锁队列；p 为即将阻塞的当前借用任务。
 * 出参/返回：void；可能增加 uninterruptible/iowait 统计，最终失去对 p 的独占访问权。
 * 注意事项：release 与 ttwu 的 acquire 配对；写 on_rq 后调用者绝不能再解引用 p，函数不可睡眠。
 */
static inline void __block_task(struct rq *rq, struct task_struct *p)
{
	if (p->sched_contributes_to_load)
		/* 不可中断阻塞任务增加分布式 loadavg 贡献；它可能在另一 rq 上被唤醒扣除。 */
		rq->nr_uninterruptible++;

	/* I/O wait 同时进入 rq 原子计数和 delayacct 起点，供 /proc/PSI 等观测。 */
	if (p->in_iowait) {
		atomic_inc(&rq->nr_iowait);
		delayacct_blkio_start();
	}

	ASSERT_EXCLUSIVE_WRITER(p->on_rq);
	/* 此断言证明当前路径是 on_rq 的唯一写者，但真正跨 CPU 排序由下方 release/acquire 提供。 */

	/*
	 * The moment this write goes through, ttwu() can swoop in and migrate
	 * this task, rendering our rq->__lock ineffective.
	 *
	 * __schedule()				try_to_wake_up()
	 *   LOCK rq->__lock			  LOCK p->pi_lock
	 *   pick_next_task()
	 *     pick_next_task_fair()
	 *       pick_next_entity()
	 *         dequeue_entities()
	 *           __block_task()
	 *             RELEASE p->on_rq = 0	  if (p->on_rq && ...)
	 *					    break;
	 *
	 *					  ACQUIRE (after ctrl-dep)
	 *
	 *					  cpu = select_task_rq();
	 *					  set_task_cpu(p, cpu);
	 *					  ttwu_queue()
	 *					    ttwu_do_activate()
	 *					      LOCK rq->__lock
	 *					      activate_task()
	 *					        STORE p->on_rq = 1
	 *   UNLOCK rq->__lock
	 *
	 * Callers must ensure to not reference @p after this -- we no longer
	 * own it.
	 */
	/* release 发布前述记账与离队结果；ttwu 控制依赖后的 acquire 观察零值并取得任务处理权。 */
	smp_store_release(&p->on_rq, 0);
}

extern void activate_task(struct rq *rq, struct task_struct *p, int flags);
extern void deactivate_task(struct rq *rq, struct task_struct *p, int flags);

/* wakeup_preempt() 在任务已入队且 rq 锁仍持有时比较候选并发布 resched 请求。 */
extern void wakeup_preempt(struct rq *rq, struct task_struct *p, int flags);

/*
 * attach_task() -- attach the task detached by detach_task() to its new rq.
 */
/* 目标 rq 锁下激活已完成 set_task_cpu() 的任务，并立即检查抢占。 */
/*
 * 业务背景：attach_task() 把 detach_task() 已迁移归属的任务发布到目标 rq 并立即检查抢占。
 * 入参：rq 为已锁目标队列；p 为借用且 task_rq(p) 已指向 rq 的离队任务。
 * 出参/返回：void；以 NOCLOCK 激活 p 并可能设置 resched，不转移 task 引用。
 * 注意事项：调用前须更新 rq clock；归属不一致会 WARN，函数不可睡眠。
 */
static inline void attach_task(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);

	WARN_ON_ONCE(task_rq(p) != rq);
	activate_task(rq, p, ENQUEUE_NOCLOCK);
	wakeup_preempt(rq, p, 0);
}

/*
 * attach_one_task() -- attaches the task returned from detach_one_task() to
 * its new rq.
 */
/*
 * 业务背景：attach_one_task() 为单任务迁移自行获取目标 rq 锁、更新时钟并调用 attach_task()。
 * 入参：rq 为目标队列借用指针；p 为已 detach 且归属已改写的借用任务。
 * 出参/返回：void；作用域 guard 自动解锁，任务成功发布到目标队列。
 * 注意事项：不可在已持同一 rq 锁时调用；原始自旋临界区不睡眠。
 */
static inline void attach_one_task(struct rq *rq, struct task_struct *p)
{
	guard(rq_lock)(rq);
	update_rq_clock(rq);
	attach_task(rq, p);
}

#ifdef CONFIG_PREEMPT_RT
/* PREEMPT_RT 缩短单次迁移批量，限制持锁/关抢占延迟；普通内核以更大批量换吞吐。 */
# define SCHED_NR_MIGRATE_BREAK 8
#else
# define SCHED_NR_MIGRATE_BREAK 32
#endif

extern __read_mostly unsigned int sysctl_sched_nr_migrate;
extern __read_mostly unsigned int sysctl_sched_migration_cost;

/* base_slice 与缩放模式共同确定公平调度基础时间尺度。 */
extern unsigned int sysctl_sched_base_slice;

/* resched latency 旋钮控制告警阈值及是否只报告一次。 */
extern int sysctl_resched_latency_warn_ms;
extern int sysctl_resched_latency_warn_once;

extern unsigned int sysctl_sched_tunable_scaling;

/* NUMA balancing 扫描延迟、周期、窗口和热页阈值共同控制自动页/任务迁移成本。 */
extern unsigned int sysctl_numa_balancing_scan_delay;
extern unsigned int sysctl_numa_balancing_scan_period_min;
extern unsigned int sysctl_numa_balancing_scan_period_max;
extern unsigned int sysctl_numa_balancing_scan_size;
extern unsigned int sysctl_numa_balancing_hot_threshold;

#ifdef CONFIG_SCHED_HRTICK

/*
 * Use hrtick when:
 *  - enabled by features
 *  - hrtimer is actually high res
 */
/*
 * 业务背景：hrtick_enabled() 要求 CPU active 且 hrtimer 处于高分辨率模式，才可精确切片。
 * 入参：rq 为借用非空队列。
 * 出参/返回：两条件满足返回真；无副作用。
 * 注意事项：只是能力快照，启动 timer 仍须 rq 锁；函数不睡眠。
 */
static inline bool hrtick_enabled(struct rq *rq)
{
	return cpu_active(cpu_of(rq)) && hrtimer_highres_enabled();
}

/* fair 类还需 HRTICK feature 开启；返回布尔能力快照，不修改 rq。 */
/*
 * 业务背景：hrtick_enabled_fair() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool hrtick_enabled_fair(struct rq *rq)
{
	return sched_feat(HRTICK) && hrtick_enabled(rq);
}

/* DL 类由独立 HRTICK_DL feature 控制，避免强迫其他类承担 timer 成本。 */
/*
 * 业务背景：hrtick_enabled_dl() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool hrtick_enabled_dl(struct rq *rq)
{
	return sched_feat(HRTICK_DL) && hrtick_enabled(rq);
}

extern void hrtick_start(struct rq *rq, u64 delay);
/* hrtick_active() 查询 rq timer 是否已排队；rq 为借用输入，返回瞬时状态且不睡眠。 */
/*
 * 业务背景：hrtick_active() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool hrtick_active(struct rq *rq)
{
	return hrtimer_active(&rq->hrtick_timer);
}

#else /* !CONFIG_SCHED_HRTICK: */
/* 未编译 hrtick 时所有能力查询恒假，参数仅保持公共调用接口。 */
/*
 * 业务背景：hrtick_enabled_fair() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool hrtick_enabled_fair(struct rq *rq) { return false; }
/*
 * 业务背景：hrtick_enabled_dl() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool hrtick_enabled_dl(struct rq *rq) { return false; }
/*
 * 业务背景：hrtick_enabled() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool hrtick_enabled(struct rq *rq) { return false; }
#endif /* !CONFIG_SCHED_HRTICK */
/* 高精度调度 tick 的实现与退化查询到此结束。 */

#ifndef arch_scale_freq_tick
/* 架构未提供频率尺度 tick hook 时使用无副作用 stub。 */
/*
 * 业务背景：arch_scale_freq_tick() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline void arch_scale_freq_tick(void) { }
#endif

#ifndef arch_scale_freq_capacity
/**
 * arch_scale_freq_capacity - get the frequency scale factor of a given CPU.
 * @cpu: the CPU in question.
 *
 * Return: the frequency scale factor normalized against SCHED_CAPACITY_SCALE, i.e.
 *
 *     f_curr
 *     ------ * SCHED_CAPACITY_SCALE
 *     f_max
 */
/*
 * 架构未实现频率容量缩放时假定当前频率等于最大频率，返回 SCHED_CAPACITY_SCALE。
 * cpu 为纯输入且在 fallback 中不读取；无副作用、不睡眠，但异构 DVFS 精度将依赖其他架构 hook。
 */
/*
 * 业务背景：arch_scale_freq_capacity() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline
unsigned long arch_scale_freq_capacity(int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

/*
 * In double_lock_balance()/double_rq_lock(), we use raw_spin_rq_lock() to
 * acquire rq lock instead of rq_lock(). So at the end of these two functions
 * we need to call double_rq_clock_clear_update() to clear RQCF_UPDATED of
 * rq->clock_update_flags to avoid the WARN_DOUBLE_CLOCK warning.
 */
/*
 * 业务背景：double_rq_clock_clear_update() 为未走 rq_pin_lock() 的双锁路径清除旧 UPDATED 证据。
 * 入参：rq1/rq2 均为已锁借用队列，可映射同一 core 锁。
 * 出参/返回：void；仅保留 skip 位，不更新时钟。
 * 注意事项：遗漏会触发重复时钟更新告警；函数不可睡眠。
 */
static inline void double_rq_clock_clear_update(struct rq *rq1, struct rq *rq2)
{
	rq1->clock_update_flags &= (RQCF_REQ_SKIP|RQCF_ACT_SKIP);
	rq2->clock_update_flags &= (RQCF_REQ_SKIP|RQCF_ACT_SKIP);
}

/* DEFINE_LOCK_GUARD_2 生成保存两指针的类、构造器和统一析构调用。 */
/*
 * 业务背景：DEFINE_LOCK_GUARD_2() 生成同时管理两把锁的构造函数与自动清理类型。
 * 入参：两把 lock 指针均由调用者借用，附加参数原样进入 guard 状态。
 * 出参/返回：生成构造器返回持有两锁语义的 guard 值，离开作用域时执行配对 unlock。
 * 注意事项：_lock/_unlock 必须严格配对，宏续行与 sparse 上下文标注不可拆散。
 */
#define DEFINE_LOCK_GUARD_2(name, type, _lock, _unlock, ...)				\
__DEFINE_UNLOCK_GUARD(name, type, _unlock, type *lock2; __VA_ARGS__)			\
static inline class_##name##_t class_##name##_constructor(type *lock, type *lock2)	\
	__no_context_analysis								\
{ class_##name##_t _t = { .lock = lock, .lock2 = lock2 }, *_T = &_t;			\
  _lock; return _t; }
/* 两锁 guard 的声明宏生成构造器原型及两个 cleanup 上下文，分别配平 sparse 的双重 ownership。 */
/*
 * 业务背景：DECLARE_LOCK_GUARD_2_ATTRS() 声明双锁构造器和两份 sparse cleanup helper。
 * 入参：_name 选择 guard 类型，_lock/_unlock1/_unlock2 提供上下文属性。
 * 出参/返回：生成构造器声明及两个 void cleanup 函数，不转移锁对象 ownership。
 * 注意事项：两个 cleanup 只表达静态分析语义，真实解锁仍由 guard 析构协议负责。
 */
#define DECLARE_LOCK_GUARD_2_ATTRS(_name, _lock, _unlock1, _unlock2)			\
static inline class_##_name##_t class_##_name##_constructor(lock_##_name##_t *_T1,	\
							    lock_##_name##_t *_T2) _lock; \
static __always_inline void __class_##_name##_cleanup_ctx1(class_##_name##_t **_T1)	\
	__no_context_analysis _unlock1 { }						\
static __always_inline void __class_##_name##_cleanup_ctx2(class_##_name##_t **_T2)	\
	__no_context_analysis _unlock2 { }
/* WITH 形式同时构造 guard 并布置两份 cleanup 上下文，处理 sparse 的每把锁标注。 */
#define WITH_LOCK_GUARD_2_ATTRS(_name, _T1, _T2)					\
	class_##_name##_constructor(_T1, _T2),						\
	*__UNIQUE_ID(unlock1) __cleanup(__class_##_name##_cleanup_ctx1) = (void *)(_T1),\
	*__UNIQUE_ID(unlock2) __cleanup(__class_##_name##_cleanup_ctx2) = (void *)(_T2)

/* 上述宏为两把锁生成自动清理对象及 sparse 获取/释放标注，使任一作用域出口都按配对析构。 */

/* 双 rq 锁的全序：core 模式先按共享 core、再按 CPU，防止 AB-BA。 */
/*
 * 业务背景：rq_order_less() 为双 rq 加锁建立全局严格顺序，阻止两个 CPU 形成 AB-BA。
 * 入参：rq1/rq2 是待比较借用队列。
 * 出参/返回：rq1 应先加锁返回真，否则假；无副作用。
 * 注意事项：core 模式先比较共享 core，再比较 CPU；调用者不得另创冲突顺序，函数不睡眠。
 */
static inline bool rq_order_less(struct rq *rq1, struct rq *rq2)
{
#ifdef CONFIG_SCHED_CORE
	/*
	 * In order to not have {0,2},{1,3} turn into into an AB-BA,
	 * order by core-id first and cpu-id second.
	 *
	 * Notably:
	 *
	 *	double_rq_lock(0,3); will take core-0, core-1 lock
	 *	double_rq_lock(1,2); will take core-1, core-0 lock
	 *
	 * when only cpu-id is considered.
	 */
	/* 示例说明仅按 CPU id 会让两条路径以相反 core 顺序取锁，因此必须先比较 core id。 */
	if (rq1->core->cpu < rq2->core->cpu)
		return true;
	if (rq1->core->cpu > rq2->core->cpu)
		return false;

	/*
	 * __sched_core_flip() relies on SMT having cpu-id lock order.
	 */
	/* 同一 core 内仍按 CPU id 排序，满足 __sched_core_flip() 的兄弟锁序假设。 */
#endif /* CONFIG_SCHED_CORE */
	/* core scheduling 专用的 core/CPU 复合锁序比较到此结束。 */
	return rq1->cpu < rq2->cpu;
}

extern void double_rq_lock(struct rq *rq1, struct rq *rq2)
	__acquires(__rq_lockp(rq1), __rq_lockp(rq2));

#ifdef CONFIG_PREEMPTION

/*
 * fair double_lock_balance: Safely acquires both rq->locks in a fair
 * way at the expense of forcing extra atomic operations in all
 * invocations.  This assures that the double_lock is acquired using the
 * same underlying policy as the spinlock_t on this architecture, which
 * reduces latency compared to the unfair variant below.  However, it
 * also adds more overhead and therefore may reduce throughput.
 */
/*
 * PREEMPTION 构建选择公平版本：无条件暂放 this_rq，再由 double_rq_lock() 按体系结构锁策略重取
 * 两锁。入参队列均借用，入口持 this_rq，出口持两锁并返回 1 表示曾放锁；期间状态需由调用者复验。
 */
/*
 * 业务背景：_double_lock_balance() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int _double_lock_balance(struct rq *this_rq, struct rq *busiest)
	__must_hold(__rq_lockp(this_rq))
	__acquires(__rq_lockp(busiest))
{
	raw_spin_rq_unlock(this_rq);
	double_rq_lock(this_rq, busiest);

	return 1;
}

#else /* !CONFIG_PREEMPTION: */
/* 此分支在非抢占构建中选择偏吞吐的双 rq 加锁策略。 */
/*
 * Unfair double_lock_balance: Optimizes throughput at the expense of
 * latency by eliminating extra atomic operations when the locks are
 * already in proper order on entry.  This favors lower CPU-ids and will
 * grant the double lock to lower CPUs over higher ids under contention,
 * regardless of entry order into the function.
 */
/*
 * 非抢占构建选择吞吐版本：共享锁或 trylock 成功时不放 this_rq；锁序允许则嵌套等待，反序时才
 * 释放并按全序重取。返回 0 表示始终持有 this_rq，1 表示出现放锁窗口，所有路径出口均持两锁。
 */
/*
 * 业务背景：_double_lock_balance() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int _double_lock_balance(struct rq *this_rq, struct rq *busiest)
	__must_hold(__rq_lockp(this_rq))
	__acquires(__rq_lockp(busiest))
{
	if (__rq_lockp(this_rq) == __rq_lockp(busiest)) {
		/* SMT 兄弟可能共用一把 core 锁，只补 lockdep 获取计数，不能真实重复加锁。 */
		__acquire(__rq_lockp(busiest)); /* already held */
		/* 真实 core 锁已经持有，这里只补一份 lockdep 获取记录。 */
		double_rq_clock_clear_update(this_rq, busiest);
		return 0;
	}

	if (likely(raw_spin_rq_trylock(busiest))) {
		/* 无竞争快速路径保持 this_rq 连续持有，随后清除两个 rq 的旧时钟更新证据。 */
		double_rq_clock_clear_update(this_rq, busiest);
		return 0;
	}

	if (rq_order_less(this_rq, busiest)) {
		/* 当前持锁顺序正确，可带嵌套 subclass 等待 busiest 而不形成环。 */
		raw_spin_rq_lock_nested(busiest, SINGLE_DEPTH_NESTING);
		double_rq_clock_clear_update(this_rq, busiest);
		return 0;
	}

	raw_spin_rq_unlock(this_rq);
	/* 当前顺序反向，只能放锁后让统一 helper 按全序取得两锁；调用者必须复验队列状态。 */
	double_rq_lock(this_rq, busiest);

	return 1;
}

#endif /* !CONFIG_PREEMPTION */
/* 抢占与非抢占的双 rq 加锁策略选择到此结束。 */

/*
 * double_lock_balance - lock the busiest runqueue, this_rq is locked already.
 */
/* IRQ 已关闭且 this_rq 已锁；返回值说明过程中是否曾释放 this_rq。 */
/*
 * 业务背景：double_lock_balance() 在均衡路径已持 this_rq 时安全取得 busiest 锁。
 * 入参：两 rq 均为借用，入口 IRQ 关闭且 this_rq 已锁。
 * 出参/返回：出口持两锁；1 表示中途放过 this_rq，0 表示连续持有。
 * 注意事项：返回 1 后候选和计数都须复验；与 double_unlock_balance() 配对且不可睡眠。
 */
static inline int double_lock_balance(struct rq *this_rq, struct rq *busiest)
	__must_hold(__rq_lockp(this_rq))
	__acquires(__rq_lockp(busiest))
{
	lockdep_assert_irqs_disabled();

	return _double_lock_balance(this_rq, busiest);
}

/* 共享 core 锁只做一次真实 unlock，另一份 lockdep 获取用伪释放配平。 */
/*
 * 业务背景：double_unlock_balance() 释放额外取得的 busiest 锁并恢复 this_rq 的普通 lockdep subclass。
 * 入参：两 rq 均已锁且为借用对象。
 * 出参/返回：void；保留 this_rq 锁，只释放 busiest 的真实或伪获取。
 * 注意事项：共享 core 锁只能伪释放一次计数；不恢复 IRQ且不可睡眠。
 */
static inline void double_unlock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(__rq_lockp(busiest))
{
	if (__rq_lockp(this_rq) != __rq_lockp(busiest))
		raw_spin_rq_unlock(busiest);
	else
		__release(__rq_lockp(busiest)); /* fake release */
		/* 共享锁没有第二次真实解锁，此处仅配平 lockdep 记录。 */
	lock_set_subclass(&__rq_lockp(this_rq)->dep_map, 0, _RET_IP_);
}

/* 普通 spinlock 双锁按地址排序，出口持两锁；l1/l2 借用且不可相同，函数不可睡眠。 */
/*
 * 业务背景：double_lock() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void double_lock(spinlock_t *l1, spinlock_t *l2)
	__acquires(l1, l2)
{
	if (l1 > l2)
		swap(l1, l2);

	spin_lock(l1);
	spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

/* 与 double_lock 相同但首锁同时关闭 IRQ；调用者负责在释放后按协议恢复 IRQ。 */
/*
 * 业务背景：double_lock_irq() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void double_lock_irq(spinlock_t *l1, spinlock_t *l2)
	__acquires(l1, l2)
{
	if (l1 > l2)
		swap(l1, l2);

	spin_lock_irq(l1);
	spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

/* raw spinlock 双锁按地址全序获取，供不能使用普通 spinlock 语义的底层 rq/带宽路径。 */
/*
 * 业务背景：double_raw_lock() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void double_raw_lock(raw_spinlock_t *l1, raw_spinlock_t *l2)
	__acquires(l1, l2)
{
	if (l1 > l2)
		swap(l1, l2);

	raw_spin_lock(l1);
	raw_spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

/* 释放由 double_raw_lock() 取得的两锁；参数仍按调用者传入顺序，二者都必须已持有。 */
/*
 * 业务背景：double_raw_unlock() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void double_raw_unlock(raw_spinlock_t *l1, raw_spinlock_t *l2)
	__releases(l1, l2)
{
	raw_spin_unlock(l1);
	raw_spin_unlock(l2);
}

DEFINE_LOCK_GUARD_2(double_raw_spinlock, raw_spinlock_t,
		    double_raw_lock(_T->lock, _T->lock2),
		    double_raw_unlock(_T->lock, _T->lock2))

/* double_raw_spinlock guard 把两把 raw lock 的获取/释放和 sparse 上下文绑定到同一自动对象。 */
DECLARE_LOCK_GUARD_2_ATTRS(double_raw_spinlock,
			   __acquires(_T1, _T2),
			   __releases(*(raw_spinlock_t **)_T1),
			   __releases(*(raw_spinlock_t **)_T2));
#define class_double_raw_spinlock_constructor(_T1, _T2) \
	WITH_LOCK_GUARD_2_ATTRS(double_raw_spinlock, _T1, _T2)

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
/* 两 rq 可能映射到同一 core 锁，必须避免重复解锁；本 helper 不恢复 IRQ。 */
/*
 * 业务背景：double_rq_unlock() 结束 double_rq_lock() 建立的双队列临界区。
 * 入参：rq1/rq2 为已锁借用队列。
 * 出参/返回：void；释放两份 lockdep ownership，共享 core 锁只真实释放一次。
 * 注意事项：不恢复 IRQ，调用者必须自行恢复；放锁后队列状态立即可能变化，函数不睡眠。
 */
static inline void double_rq_unlock(struct rq *rq1, struct rq *rq2)
	__releases(__rq_lockp(rq1), __rq_lockp(rq2))
{
	if (__rq_lockp(rq1) != __rq_lockp(rq2))
		raw_spin_rq_unlock(rq2);
	else
		__release(__rq_lockp(rq2)); /* fake release */
	/* rq2 的额外 ownership 处理完后，最后释放 rq1 对应的真实底层锁。 */
	raw_spin_rq_unlock(rq1);
}

/* online/offline 在 rq 锁下通知所有调度类并维护 root_domain online mask。 */
extern void set_rq_online (struct rq *rq);
extern void set_rq_offline(struct rq *rq);

extern bool sched_smp_initialized;

/* double_rq_lock guard 在离开作用域时调用 double_rq_unlock()，但仍不负责 IRQ 恢复。 */
DEFINE_LOCK_GUARD_2(double_rq_lock, struct rq,
		    double_rq_lock(_T->lock, _T->lock2),
		    double_rq_unlock(_T->lock, _T->lock2))

extern struct sched_entity *__pick_root_entity(struct cfs_rq *cfs_rq);
extern struct sched_entity *__pick_first_entity(struct cfs_rq *cfs_rq);
extern struct sched_entity *__pick_last_entity(struct cfs_rq *cfs_rq);

/* 调试输出接口借用 seq_file 和各 rq，在相应锁/快照协议下格式化类统计，不取得对象 ownership。 */
extern bool sched_debug_verbose;

extern void print_cfs_stats(struct seq_file *m, int cpu);
extern void print_rt_stats(struct seq_file *m, int cpu);
extern void print_dl_stats(struct seq_file *m, int cpu);
extern void print_cfs_rq(struct seq_file *m, int cpu, struct cfs_rq *cfs_rq);
extern void print_rt_rq(struct seq_file *m, int cpu, struct rt_rq *rt_rq);
extern void print_dl_rq(struct seq_file *m, int cpu, struct dl_rq *dl_rq);

extern void resched_latency_warn(int cpu, u64 latency);

#ifdef CONFIG_NUMA_BALANCING
/* NUMA 统计输出借用 task/seq_file，tsf/tpf/gsf/gpf 分别是任务/组的共享与私有 fault 计数。 */
extern void show_numa_stats(struct task_struct *p, struct seq_file *m);
extern void
print_numa_stats(struct seq_file *m, int node, unsigned long tsf,
		 unsigned long tpf, unsigned long gsf, unsigned long gpf);
#endif /* CONFIG_NUMA_BALANCING */

/* 各类 rq 初始化在 CPU rq 发布前建立空树、计数、锁和哨兵状态。 */
extern void init_cfs_rq(struct cfs_rq *cfs_rq);
extern void init_rt_rq(struct rt_rq *rt_rq);
extern void init_dl_rq(struct dl_rq *dl_rq);

extern void cfs_bandwidth_usage_inc(void);
extern void cfs_bandwidth_usage_dec(void);

#ifdef CONFIG_NO_HZ_COMMON

/* 四个 kick bit 分别请求域均衡、阻塞负载更新、idle 入场更新和 next_balance 刷新。 */
#define NOHZ_BALANCE_KICK_BIT	0
#define NOHZ_STATS_KICK_BIT	1
#define NOHZ_NEWILB_KICK_BIT	2
#define NOHZ_NEXT_KICK_BIT	3

/* Run sched_balance_domains() */
/* 执行 sched_balance_domains()，把空闲 CPU 用作全局均衡代理。 */
#define NOHZ_BALANCE_KICK	BIT(NOHZ_BALANCE_KICK_BIT)
/* Update blocked load */
/* 更新已阻塞实体的衰减负载，避免停 tick CPU 的 PELT 长期陈旧。 */
#define NOHZ_STATS_KICK		BIT(NOHZ_STATS_KICK_BIT)
/* Update blocked load when entering idle */
/* CPU 刚进入 idle 时立即完成一次阻塞负载更新。 */
#define NOHZ_NEWILB_KICK	BIT(NOHZ_NEWILB_KICK_BIT)
/* Update nohz.next_balance */
/* 只刷新下一均衡截止时刻，不强制执行完整域扫描。 */
#define NOHZ_NEXT_KICK		BIT(NOHZ_NEXT_KICK_BIT)

#define NOHZ_KICK_MASK		(NOHZ_BALANCE_KICK | NOHZ_STATS_KICK | NOHZ_NEXT_KICK)

#define nohz_flags(cpu)		(&cpu_rq(cpu)->nohz_flags)

/* 离开 idle 时清理 rq 的 NO_HZ idle 状态，rq 为借用输入且调用点遵守本地 rq 协议。 */
extern void nohz_balance_exit_idle(struct rq *rq);
#else /* !CONFIG_NO_HZ_COMMON: */
/* 未启用通用 NO_HZ 时没有 idle balance 状态需要清理。 */
/*
 * 业务背景：nohz_balance_exit_idle() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void nohz_balance_exit_idle(struct rq *rq) { }
#endif /* !CONFIG_NO_HZ_COMMON */
/* NO_HZ idle 退出接口的配置分支到此结束。 */

#ifdef CONFIG_NO_HZ_COMMON
/* 在指定 idle CPU 上消费远程均衡 kick；cpu 是纯输入，内部负责对应 rq 同步。 */
extern void nohz_run_idle_balance(int cpu);
#else
/* 未启用 NO_HZ 时远程 idle balance 调用退化为空操作。 */
/*
 * 业务背景：nohz_run_idle_balance() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void nohz_run_idle_balance(int cpu) { }
#endif

#include "stats.h"

#if defined(CONFIG_SCHED_CORE) && defined(CONFIG_SCHEDSTATS)

extern void __sched_core_account_forceidle(struct rq *rq);

/* core force-idle 统计仅在 schedstats 运行时开启时进入慢路径；rq 借用且无引用变化。 */
/*
 * 业务背景：sched_core_account_forceidle() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_core_account_forceidle(struct rq *rq)
{
	if (schedstat_enabled())
		__sched_core_account_forceidle(rq);
}

extern void __sched_core_tick(struct rq *rq);

/* 同时启用本 rq core scheduling 与 schedstats 时才累计 core tick 统计。 */
/*
 * 业务背景：sched_core_tick() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_core_tick(struct rq *rq)
{
	if (sched_core_enabled(rq) && schedstat_enabled())
		__sched_core_tick(rq);
}

#else /* !(CONFIG_SCHED_CORE && CONFIG_SCHEDSTATS): */

/* 任一所需配置关闭时两个统计 hook 都是无副作用 stub。 */
/*
 * 业务背景：sched_core_account_forceidle() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_core_account_forceidle(struct rq *rq) { }

/*
 * 业务背景：sched_core_tick() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_core_tick(struct rq *rq) { }

#endif /* !(CONFIG_SCHED_CORE && CONFIG_SCHEDSTATS) */
/* core force-idle 统计 hook 的实现与空桩到此结束。 */

#ifdef CONFIG_IRQ_TIME_ACCOUNTING

/* 32 位读者通过 u64_stats_sync 获取一致的 IRQ 累计时间快照。 */
struct irqtime {
	/* total 是累计硬中断时间，tick_delta 是本 tick 增量，irq_start_time 标记当前 IRQ 入口时刻。 */
	u64			total;
	u64			tick_delta;
	u64			irq_start_time;
	struct u64_stats_sync	sync;
};

DECLARE_PER_CPU(struct irqtime, cpu_irqtime);
DECLARE_STATIC_KEY_FALSE(sched_clock_irqtime);

/* irqtime_enabled() 读取静态键，返回是否启用 IRQ 时间记账；无副作用且不睡眠。 */
/*
 * 业务背景：irqtime_enabled() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int irqtime_enabled(void)
{
	return static_branch_likely(&sched_clock_irqtime);
}

/*
 * Returns the irqtime minus the softirq time computed by ksoftirqd.
 * Otherwise ksoftirqd's sum_exec_runtime is subtracted its own runtime
 * and never move forward.
 */
/*
 * irq_time_read() 返回扣除 ksoftirqd 自身计算部分后的 CPU IRQ 累计时间，避免其 runtime 被重复扣除。
 * cpu 为有效纯输入；返回 u64 时间快照。u64_stats_sync 循环保证 32 位读者不观察撕裂值，函数不睡眠。
 */
/*
 * 业务背景：irq_time_read() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline u64 irq_time_read(int cpu)
{
	struct irqtime *irqtime = &per_cpu(cpu_irqtime, cpu);
	unsigned int seq;
	u64 total;

	do {
		/* writer 更新期间 seq 会变化，读者丢弃本轮 total 并重新获取完整 u64。 */
		seq = __u64_stats_fetch_begin(&irqtime->sync);
		total = irqtime->total;
	} while (__u64_stats_fetch_retry(&irqtime->sync, seq));

	return total;
}

#else /* !CONFIG_IRQ_TIME_ACCOUNTING: */

/* 未编译 IRQ 时间记账时能力查询固定为 0，无副作用。 */
/*
 * 业务背景：irqtime_enabled() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int irqtime_enabled(void)
{
	return 0;
}

#endif /* !CONFIG_IRQ_TIME_ACCOUNTING */
/* IRQ 时间记账读取接口的配置分支到此结束。 */

#ifdef CONFIG_CPU_FREQ

DECLARE_PER_CPU(struct update_util_data __rcu *, cpufreq_update_util_data);

/**
 * cpufreq_update_util - Take a note about CPU utilization changes.
 * @rq: Runqueue to carry out the update for.
 * @flags: Update reason flags.
 *
 * This function is called by the scheduler on the CPU whose utilization is
 * being updated.
 *
 * It can only be called from RCU-sched read-side critical sections.
 *
 * The way cpufreq is currently arranged requires it to evaluate the CPU
 * performance state (frequency/voltage) on a regular basis to prevent it from
 * being stuck in a completely inadequate performance level for too long.
 * That is not guaranteed to happen if the updates are only triggered from CFS
 * and DL, though, because they may not be coming in if only RT tasks are
 * active all the time (or there are RT tasks only).
 *
 * As a workaround for that issue, this function is called periodically by the
 * RT sched class to trigger extra cpufreq updates to prevent it from stalling,
 * but that really is a band-aid.  Going forward it should be replaced with
 * solutions targeted more specifically at RT tasks.
 */
/* sched RCU 下读取 governor 回调；注册方负责 RCU 替换与回收。 */
/*
 * 业务背景：cpufreq_update_util() 把本 CPU 最新利用率变化通知 governor，避免性能状态长期不匹配。
 * 入参：rq 为当前 CPU 已锁借用队列；flags 是更新原因位图。
 * 出参/返回：void；若注册回调则传入新鲜 rq 时钟执行，不转移 data ownership。
 * 注意事项：只能在 RCU-sched 读侧调用；RT 周期触发只是临时补救方案，回调不可破坏 rq 锁约束。
 */
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags)
{
	struct update_util_data *data;

	data = rcu_dereference_sched(*per_cpu_ptr(&cpufreq_update_util_data,
						  cpu_of(rq)));
	if (data)
		data->func(data, rq_clock(rq), flags);
}
#else /* !CONFIG_CPU_FREQ: */
/* CPU_FREQ 未编译时更新 hook 为无副作用 stub，rq/flags 仅保持热路径调用统一。 */
/*
 * 业务背景：cpufreq_update_util() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags) { }
#endif /* !CONFIG_CPU_FREQ */
/* CPU 频率 governor 通知接口的配置分支到此结束。 */

#ifdef arch_scale_freq_capacity
# ifndef arch_scale_freq_invariant
/* 架构提供频率 capacity 却未另声明时，调度器按频率不变性可用处理。 */
#  define arch_scale_freq_invariant()	true
# endif
#else
/* 缺少频率 capacity hook 时不能声称利用率已按频率归一化。 */
# define arch_scale_freq_invariant()	false
#endif

unsigned long effective_cpu_util(int cpu, unsigned long util_cfs,
				 unsigned long *min,
				 unsigned long *max);

unsigned long sugov_effective_cpu_perf(int cpu, unsigned long actual,
				 unsigned long min,
				 unsigned long max);


/*
 * Verify the fitness of task @p to run on @cpu taking into account the
 * CPU original capacity and the runtime/deadline ratio of the task.
 *
 * The function will return true if the original capacity of @cpu is
 * greater than or equal to task's deadline density right shifted by
 * (BW_SHIFT - SCHED_CAPACITY_SHIFT) and false otherwise.
 */
/* 比较原始 CPU capacity 与任务 deadline density，不承诺目标 CPU 当前在线。 */
/*
 * 业务背景：dl_task_fits_capacity() 检查 CPU 原始容量能否承载任务的 runtime/deadline 密度。
 * 入参：p 为借用非空 DL task；cpu 是候选编号。
 * 出参/返回：capacity 大于等于换算后 density 返回真，否则假；无副作用。
 * 注意事项：不检查 online/affinity，放置路径须另行验证；读取属性需由任务锁稳定，函数不睡眠。
 */
static inline bool dl_task_fits_capacity(struct task_struct *p, int cpu)
{
	unsigned long cap = arch_scale_cpu_capacity(cpu);

	return cap >= p->dl.dl_density >> (BW_SHIFT - SCHED_CAPACITY_SHIFT);
}

/* cpu_bw_dl() 把 rq 的 running_bw 从 BW_SHIFT 定点值换算到 capacity 尺度；rq 借用且无副作用。 */
/*
 * 业务背景：cpu_bw_dl() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned long cpu_bw_dl(struct rq *rq)
{
	return (rq->dl.running_bw * SCHED_CAPACITY_SCALE) >> BW_SHIFT;
}

/* cpu_util_dl() 用 READ_ONCE 读取 DL PELT util 快照；返回可过期但不会撕裂，函数不睡眠。 */
/*
 * 业务背景：cpu_util_dl() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned long cpu_util_dl(struct rq *rq)
{
	return READ_ONCE(rq->avg_dl.util_avg);
}


extern unsigned long cpu_util_cfs(int cpu);
extern unsigned long cpu_util_cfs_boost(int cpu);

/* cpu_util_rt() 返回 rq 的 RT PELT util 瞬时快照；rq 为借用输入，无 ownership 变化。 */
/*
 * 业务背景：cpu_util_rt() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned long cpu_util_rt(struct rq *rq)
{
	return READ_ONCE(rq->avg_rt.util_avg);
}

#ifdef CONFIG_UCLAMP_TASK

unsigned long uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id);

/*
 * When uclamp is compiled in, the aggregation at rq level is 'turned off'
 * by default in the fast path and only gets turned on once userspace performs
 * an operation that requires it.
 *
 * Returns true if userspace opted-in to use uclamp and aggregation at rq level
 * hence is active.
 */
/* 静态键一旦启用不再关闭，避免未使用系统承担 rq 聚合开销。 */
/*
 * 业务背景：uclamp_is_used() 快判用户是否曾启用 rq 级 clamp 聚合，未使用系统保持零热路径成本。
 * 入参：无。
 * 出参/返回：静态键开启返回真；无副作用。
 * 注意事项：键一旦开启不再关闭，因此只表示机制需维护，不表示当前 rq 一定受限；不睡眠。
 */
static inline bool uclamp_is_used(void)
{
	return static_branch_likely(&sched_uclamp_used);
}

/*
 * Enabling static branches would get the cpus_read_lock(),
 * check whether uclamp_is_used before enable it to avoid always
 * calling cpus_read_lock(). Because we never disable this
 * static key once enable it.
 */
/*
 * sched_uclamp_enable() 先快判再启用永久静态键，避免每次请求都取得 cpus_read_lock()。
 * 无入参、void 返回；首次调用会更新所有 CPU 的跳转标签并可能睡眠，调用者须处于可睡眠上下文。
 */
/*
 * 业务背景：sched_uclamp_enable() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_uclamp_enable(void)
{
	if (!uclamp_is_used())
		static_branch_enable(&sched_uclamp_used);
}

/* uclamp_rq_get() 原子读取指定 MIN/MAX 聚合值；rq 借用，clamp_id 须小于 UCLAMP_CNT。 */
/*
 * 业务背景：uclamp_rq_get() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned long uclamp_rq_get(struct rq *rq,
					  enum uclamp_id clamp_id)
{
	return READ_ONCE(rq->uclamp[clamp_id].value);
}

/* uclamp_rq_set() 以 WRITE_ONCE 发布指定聚合值；调用者持 rq 锁并保证 value 在 capacity 范围。 */
/*
 * 业务背景：uclamp_rq_set() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void uclamp_rq_set(struct rq *rq, enum uclamp_id clamp_id,
				 unsigned int value)
{
	WRITE_ONCE(rq->uclamp[clamp_id].value, value);
}

/* uclamp_rq_is_idle() 返回 rq 是否处于 clamp idle 保留状态；稳定判断依赖 rq 锁。 */
/*
 * 业务背景：uclamp_rq_is_idle() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool uclamp_rq_is_idle(struct rq *rq)
{
	return rq->uclamp_flags & UCLAMP_FLAG_IDLE;
}

/* Is the rq being capped/throttled by uclamp_max? */
/* CFS+RT 利用率达到有效 max clamp 时视为受限；关闭 uclamp 快速返回。 */
/*
 * 业务背景：uclamp_rq_is_capped() 判断 CFS+RT 利用率是否已达到有效 UCLAMP_MAX 上限。
 * 入参：rq 为借用非空队列。
 * 出参/返回：未启用或 max 为满尺度返回假；util >= max 返回真，无副作用。
 * 注意事项：利用率与 clamp 是无锁快照，仅供频率/能耗决策提示；函数不睡眠。
 */
static inline bool uclamp_rq_is_capped(struct rq *rq)
{
	unsigned long rq_util;
	unsigned long max_util;

	if (!uclamp_is_used())
		/* 从未启用聚合时 rq->uclamp 无需维护，不能据其初始内容判定 capped。 */
		return false;

	rq_util = cpu_util_cfs(cpu_of(rq)) + cpu_util_rt(rq);
	/* max clamp 与两类 util 都是 capacity 尺度；满尺度代表没有上限。 */
	max_util = READ_ONCE(rq->uclamp[UCLAMP_MAX].value);

	return max_util != SCHED_CAPACITY_SCALE && rq_util >= max_util;
}

#define for_each_clamp_id(clamp_id) \
	for ((clamp_id) = 0; (clamp_id) < UCLAMP_CNT; (clamp_id)++)

extern unsigned int sysctl_sched_uclamp_util_min_rt_default;


/* uclamp_none() 返回 MIN 的无下限值 0 或 MAX 的无限制值 SCHED_CAPACITY_SCALE。 */
/*
 * 业务背景：uclamp_none() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned int uclamp_none(enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;
	return SCHED_CAPACITY_SCALE;
}

/* Integer rounded range for each bucket */
/* 每个 bucket 覆盖四舍五入后的 capacity 区间，最后一桶由 id helper 显式钳制。 */
#define UCLAMP_BUCKET_DELTA DIV_ROUND_CLOSEST(SCHED_CAPACITY_SCALE, UCLAMP_BUCKETS)

/* clamp 值映射到有限 bucket 下标，输入应位于 capacity 尺度，返回永不越界。 */
/*
 * 业务背景：uclamp_bucket_id() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned int uclamp_bucket_id(unsigned int clamp_value)
{
	return min_t(unsigned int, clamp_value / UCLAMP_BUCKET_DELTA, UCLAMP_BUCKETS - 1);
}

/* uclamp_se_set() 同步写入值、派生 bucket_id 和用户定义标志；uc_se 由调用者拥有并锁定。 */
/*
 * 业务背景：uclamp_se_set() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void
uclamp_se_set(struct uclamp_se *uc_se, unsigned int value, bool user_defined)
{
	uc_se->value = value;
	uc_se->bucket_id = uclamp_bucket_id(value);
	uc_se->user_defined = user_defined;
}

#else /* !CONFIG_UCLAMP_TASK: */

/* 未编译 uclamp 时有效值退化为 MIN=0、MAX=capacity 满尺度；p 仅为接口占位。 */
/*
 * 业务背景：uclamp_eff_value() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned long
uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;

	return SCHED_CAPACITY_SCALE;
}

/*
 * 业务背景：uclamp_rq_is_capped() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool uclamp_rq_is_capped(struct rq *rq) { return false; }

/* 配置关闭分支不存在聚合状态，used 恒假、enable 无操作。 */
/*
 * 业务背景：uclamp_is_used() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool uclamp_is_used(void)
{
	return false;
}

/*
 * 业务背景：sched_uclamp_enable() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void sched_uclamp_enable(void) {}

/* 配置关闭时 rq getter 同样返回无约束边界，不读取 rq。 */
/*
 * 业务背景：uclamp_rq_get() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned long
uclamp_rq_get(struct rq *rq, enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;

	return SCHED_CAPACITY_SCALE;
}

/* 配置关闭时 setter 丢弃请求且无副作用；调用者仍可保留统一更新流程。 */
/*
 * 业务背景：uclamp_rq_set() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void
uclamp_rq_set(struct rq *rq, enum uclamp_id clamp_id, unsigned int value)
{
}

/* 没有 uclamp_flags，rq 永不处于 clamp idle 特殊状态。 */
/*
 * 业务背景：uclamp_rq_is_idle() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool uclamp_rq_is_idle(struct rq *rq)
{
	return false;
}

#endif /* !CONFIG_UCLAMP_TASK */
/* 利用率钳制 rq helper 的实现与空桩到此结束。 */

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ

/* cpu_util_irq() 返回 rq 的 IRQ PELT util 快照；rq 借用且值可随中断记账并发变化。 */
/*
 * 业务背景：cpu_util_irq() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned long cpu_util_irq(struct rq *rq)
{
	return READ_ONCE(rq->avg_irq.util_avg);
}

/*
 * scale_irq_capacity() 从可用 max 中扣除 irq 压力，再按剩余比例缩放任务 util。
 * util/irq/max 均为同一 capacity 尺度的纯输入，返回缩放值；调用者保证 irq <= max 且 max 非零。
 */
/*
 * 业务背景：scale_irq_capacity() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline
unsigned long scale_irq_capacity(unsigned long util, unsigned long irq, unsigned long max)
{
	util *= (max - irq);
	util /= max;

	return util;

}

#else /* !CONFIG_HAVE_SCHED_AVG_IRQ: */

/* 没有 IRQ PELT 支持时报告零 IRQ 利用率，不读取 rq。 */
/*
 * 业务背景：cpu_util_irq() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline unsigned long cpu_util_irq(struct rq *rq)
{
	return 0;
}

/* 无 IRQ 压力模型时保持 util 原值；irq/max 参数仅维持公共签名。 */
/*
 * 业务背景：scale_irq_capacity() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline
unsigned long scale_irq_capacity(unsigned long util, unsigned long irq, unsigned long max)
{
	return util;
}

#endif /* !CONFIG_HAVE_SCHED_AVG_IRQ */
/* IRQ PELT 利用率缩放接口的配置分支到此结束。 */

extern void __setparam_fair(struct task_struct *p, const struct sched_attr *attr);

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)

/* perf_domain_span() 借用能耗模型 domain 的尾随 CPU mask，生命周期由 root_domain RCU 保护。 */
#define perf_domain_span(pd) (to_cpumask(((pd)->em_pd->cpus)))

DECLARE_STATIC_KEY_FALSE(sched_energy_present);

/* sched_energy_enabled() 快判能耗模型已发布且可供 EAS 使用；无入参、副作用或睡眠。 */
/*
 * 业务背景：sched_energy_enabled() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_energy_enabled(void)
{
	return static_branch_unlikely(&sched_energy_present);
}

#else /* !(CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL): */

/* 缺少 EM 或 schedutil 时没有可遍历 perf_domain span。 */
#define perf_domain_span(pd) NULL

/* EAS 依赖不完整时能力查询恒假。 */
/*
 * 业务背景：sched_energy_enabled() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_energy_enabled(void) { return false; }

#endif /* !(CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL) */
/* EAS 能耗域接口的实现与退化查询到此结束。 */

#ifdef CONFIG_MEMBARRIER

/*
 * The scheduler provides memory barriers required by membarrier between:
 * - prior user-space memory accesses and store to rq->membarrier_state,
 * - store to rq->membarrier_state and following user-space memory accesses.
 * In the same way it provides those guarantees around store to rq->curr.
 */
/* context switch 时缓存 next mm 状态；外围屏障保证用户内存访问的规定顺序。 */
/*
 * 业务背景：membarrier_switch_mm() 在地址空间切换时把 next_mm 请求状态缓存到 rq，供调度屏障协议观察。
 * 入参：rq 为已锁输入输出队列；prev_mm/next_mm 为切换期间稳定的借用 mm，可相同。
 * 出参/返回：void；mm 不变或缓存相同走快速路径，否则更新 rq->membarrier_state。
 * 注意事项：本 helper 自身的 READ/WRITE_ONCE 不是完整屏障，正确顺序由外围 context-switch 屏障提供。
 */
/*
 * 业务背景：membarrier_switch_mm() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void membarrier_switch_mm(struct rq *rq,
					struct mm_struct *prev_mm,
					struct mm_struct *next_mm)
{
	int membarrier_state;

	if (prev_mm == next_mm)
		/* 同一地址空间没有新的 membarrier 请求域需要切换。 */
		return;

	membarrier_state = atomic_read(&next_mm->membarrier_state);
	if (READ_ONCE(rq->membarrier_state) == membarrier_state)
		/* 缓存已同步时避免写 rq 热缓存线。 */
		return;

	WRITE_ONCE(rq->membarrier_state, membarrier_state);
}

#else /* !CONFIG_MEMBARRIER: */

/* 未编译 membarrier 时切换 hook 无状态可同步，三个参数均为借用占位。 */
/*
 * 业务背景：membarrier_switch_mm() 在未编译 membarrier 时保留上下文切换调用点。
 * 入参：rq、prev_mm、next_mm 均为未使用的借用参数。
 * 出参/返回：void，无副作用且不转移任何引用或 ownership。
 * 注意事项：这是编译期空桩，不提供内存屏障；调用者仍遵守普通 context-switch 锁协议。
 */
static inline void membarrier_switch_mm(struct rq *rq,
					struct mm_struct *prev_mm,
					struct mm_struct *next_mm)
{
}

#endif /* !CONFIG_MEMBARRIER */

/*
 * 业务背景：is_per_cpu_kthread() 识别绑定到唯一 CPU 的内核线程，供热插拔/亲和性路径特殊处理。
 * 入参：p 为借用非空 task。
 * 出参/返回：PF_KTHREAD 且 nr_cpus_allowed==1 返回真；无副作用。
 * 注意事项：亲和性可并发变化，稳定决策须持 task/rq 锁；不睡眠。
 */
static inline bool is_per_cpu_kthread(struct task_struct *p)
{
	if (!(p->flags & PF_KTHREAD))
		return false;

	if (p->nr_cpus_allowed != 1)
		return false;

	return true;
}

extern void swake_up_all_locked(struct swait_queue_head *q);
extern void __prepare_to_swait(struct swait_queue_head *q, struct swait_queue *wait);

/* try_to_wake_up() 尝试把匹配 state 的 task 发布为 runnable，返回是否完成状态转换。 */
extern int try_to_wake_up(struct task_struct *tsk, unsigned int state, int wake_flags);

#ifdef CONFIG_PREEMPT_DYNAMIC
/* 动态抢占接口解析模式名并切换静态调用点；更新过程由实现负责全局串行化。 */
extern int preempt_dynamic_mode;
extern int sched_dynamic_mode(const char *str);
extern void sched_dynamic_update(int mode);
#endif
extern const char *preempt_modes[];

#ifdef CONFIG_SCHED_MM_CID

/* CID 位 helper 只分类编码值：ONCPU 表示 per-CPU 槽拥有，返回布尔值且无副作用。 */
/*
 * 业务背景：cid_on_cpu() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline bool cid_on_cpu(unsigned int cid)
{
	return cid & MM_CID_ONCPU;
}

/* TRANSIT 表示 ownership 模式切换中的临时 CID，调出时必须收尾或释放。 */
/*
 * 业务背景：cid_in_transit() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline bool cid_in_transit(unsigned int cid)
{
	return cid & MM_CID_TRANSIT;
}

/* cpu_cid_to_cid() 清 ONCPU 位，保留数值 CID 与其他模式位。 */
/*
 * 业务背景：cpu_cid_to_cid() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline unsigned int cpu_cid_to_cid(unsigned int cid)
{
	return cid & ~MM_CID_ONCPU;
}

/* cid_to_cpu_cid() 设置 ONCPU 位，把同一数值 CID 编码为 per-CPU 槽所有。 */
/*
 * 业务背景：cid_to_cpu_cid() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline unsigned int cid_to_cpu_cid(unsigned int cid)
{
	return cid | MM_CID_ONCPU;
}

/* cid_to_transit_cid() 标记模式转换中的临时 ownership，数值 CID 不变。 */
/*
 * 业务背景：cid_to_transit_cid() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline unsigned int cid_to_transit_cid(unsigned int cid)
{
	return cid | MM_CID_TRANSIT;
}

/* cid_from_transit_cid() 只清 TRANSIT 位，供调出收尾恢复普通编码。 */
/*
 * 业务背景：cid_from_transit_cid() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline unsigned int cid_from_transit_cid(unsigned int cid)
{
	return cid & ~MM_CID_TRANSIT;
}

/* task-owned CID 不含 ONCPU/TRANSIT/UNSET 高位；无锁分类只解释传入快照。 */
/*
 * 业务背景：cid_on_task() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline bool cid_on_task(unsigned int cid)
{
	/* True if none of the MM_CID_ONCPU, MM_CID_TRANSIT, MM_CID_UNSET bits is set */
	/* 数值小于 TRANSIT 阈值等价于三个模式位均未设置。 */
	return cid < MM_CID_TRANSIT;
}

/* mm_drop_cid() 清除 mm 位图中的数值 CID，调用者必须拥有该 CID 且与分配并发协议串行化。 */
/*
 * 业务背景：mm_drop_cid() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline void mm_drop_cid(struct mm_struct *mm, unsigned int cid)
{
	clear_bit(cid, mm_cidmask(mm));
}

/*
 * mm_unset_cid_on_task() 先把 task 槽设为 UNSET，再仅在原值确由 task 拥有时归还 mm 位图。
 * t/mm 均借用，void 返回；顺序避免并发观察者把已归还 CID 仍视为 task 所有。
 */
/*
 * 业务背景：mm_unset_cid_on_task() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline void mm_unset_cid_on_task(struct task_struct *t)
{
	unsigned int cid = t->mm_cid.cid;

	t->mm_cid.cid = MM_CID_UNSET;
	if (cid_on_task(cid))
		mm_drop_cid(t->mm, cid);
}

/*
 * mm_drop_cid_on_cpu() 撤销 per-CPU 槽的 ONCPU ownership 并归还位图，但保留普通数值编码而非 UNSET，
 * 便于模式交接继续读取。mm/pcp 均借用，调用者处于本 CPU CID 切换协议且不可睡眠。
 */
/*
 * 业务背景：mm_drop_cid_on_cpu() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline void mm_drop_cid_on_cpu(struct mm_struct *mm, struct mm_cid_pcpu *pcp)
{
	/* Clear the ONCPU bit, but do not set UNSET in the per CPU storage */
	/* 只有槽当前真正拥有 CID 才能清位图，普通缓存值不得重复释放。 */
	if (cid_on_cpu(pcp->cid)) {
		pcp->cid = cpu_cid_to_cid(pcp->cid);
		mm_drop_cid(mm, pcp->cid);
	}
}

/* 原子占用首个空闲 CID；并发抢占同一位时返回 UNSET 让上层重试。 */
/*
 * 业务背景：__mm_get_cid() 在指定收敛区间内寻找并原子占用首个空闲 CID。
 * 入参：mm 为借用地址空间；max_cids 是搜索上界且不超过位图容量。
 * 出参/返回：成功返回调用者新拥有的数值 CID；耗尽或竞争失败返回 MM_CID_UNSET。
 * 注意事项：find 后仍可能被并发者抢占，test_and_set_bit 是提交点；函数不睡眠。
 */
static inline unsigned int __mm_get_cid(struct mm_struct *mm, unsigned int max_cids)
{
	unsigned int cid = find_first_zero_bit(mm_cidmask(mm), max_cids);

	if (cid >= max_cids)
		return MM_CID_UNSET;
	if (test_and_set_bit(cid, mm_cidmask(mm)))
		return MM_CID_UNSET;
	return cid;
}

/* 先尝试收敛区间，耗尽时扩到 possible CPU 数并自旋等待可用位。 */
/*
 * 业务背景：mm_get_cid() 优先在当前 max_cids 收敛区间分配，耗尽后扩大到 possible CPU 上限自旋。
 * 入参：mm 为借用地址空间。
 * 出参/返回：返回已在 mm 位图中占用、由调用者接管的有效 CID，不返回 UNSET。
 * 注意事项：可能忙等，必须在不可睡眠且系统保证最终有空位的调度切换上下文调用。
 */
static inline unsigned int mm_get_cid(struct mm_struct *mm)
{
	unsigned int cid = __mm_get_cid(mm, READ_ONCE(mm->mm_cid.max_cids));

	while (cid == MM_CID_UNSET) {
		/* 并发释放者归还位后下一轮可成功；cpu_relax 降低自旋互连压力。 */
		cpu_relax();
		cid = __mm_get_cid(mm, num_possible_cpus());
	}
	return cid;
}

/* 尽量把旧 CID 换入新上限，失败则保留原所有权并维持 ONCPU 模式。 */
/*
 * 业务背景：mm_cid_converge() 在 max_cids 缩小时把区间外 CID 尽量交换到新最优区间。
 * 入参：mm 借用；orig_cid 由 task/CPU 当前拥有；max_cids 为新收敛上界。
 * 出参/返回：成功返回新拥有 CID 并释放旧位；无空位返回原 CID，ONCPU 模式位保持不变。
 * 注意事项：只有取得新位后才释放旧位，避免失败丢失 ownership；函数不睡眠。
 */
static inline unsigned int mm_cid_converge(struct mm_struct *mm, unsigned int orig_cid,
					   unsigned int max_cids)
{
	unsigned int new_cid, cid = cpu_cid_to_cid(orig_cid);

	/* Is it in the optimal CID space? */
	/* 已在目标区间是无写快速路径，保留完整模式编码。 */
	if (likely(cid < max_cids))
		return orig_cid;

	/* Try to find one in the optimal space. Otherwise keep the provided. */
	/* 先占新位再清旧位，使任何时刻 task 都至少拥有一个有效 CID。 */
	new_cid = __mm_get_cid(mm, max_cids);
	if (new_cid != MM_CID_UNSET) {
		mm_drop_cid(mm, cid);
		/* Preserve the ONCPU mode of the original CID */
		/* 数值换成 new_cid，但 ownership 仍属于原来的 CPU 或 task 一侧。 */
		return new_cid | (orig_cid & MM_CID_ONCPU);
	}
	return orig_cid;
}

/* 更新 task CID 时同步通知 rseq 用户态缓存失效；相同值快速返回，t 为当前切换任务借用指针。 */
/*
 * 业务背景：mm_cid_update_task_cid() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline void mm_cid_update_task_cid(struct task_struct *t, unsigned int cid)
{
	if (t->mm_cid.cid != cid) {
		t->mm_cid.cid = cid;
		rseq_sched_set_ids_changed(t);
	}
}

/* 写当前 CPU 的 mm CID 槽；调用者保证不可迁移且 mm->pcpu 已分配。 */
/*
 * 业务背景：mm_cid_update_pcpu_cid() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static __always_inline void mm_cid_update_pcpu_cid(struct mm_struct *mm, unsigned int cid)
{
	__this_cpu_write(mm->mm_cid.pcpu->cid, cid);
}

/*
 * 业务背景：mm_cid_from_cpu() 以 per-CPU 槽为主要 owner，把可用 CID 交给调入 task 并处理模式转换。
 * 入参：t 为当前调入借用任务；cpu_cid 是本 CPU 槽快照；mode 是 mm ownership 模式快照。
 * 出参/返回：void；更新 per-CPU 与 task 槽，必要时分配/释放 CID 并通知 rseq。
 * 注意事项：调用者不可迁移且保证 t->mm 生命周期；所有分支最终让两槽形成一致可复用状态。
 */
static __always_inline void mm_cid_from_cpu(struct task_struct *t, unsigned int cpu_cid,
					    unsigned int mode)
{
	unsigned int max_cids, tcid = t->mm_cid.cid;
	struct mm_struct *mm = t->mm;

	max_cids = READ_ONCE(mm->mm_cid.max_cids);
	/* Optimize for the common case where both have the ONCPU bit set */
	/* 快速路径：两份编码都承认 CPU ownership，仅需检查数值是否仍位于收敛区间。 */
	if (likely(cid_on_cpu(cpu_cid & tcid))) {
		if (likely(cpu_cid_to_cid(cpu_cid) < max_cids)) {
			mm_cid_update_task_cid(t, cpu_cid);
			return;
		}
		/* Try to converge into the optimal CID space */
		/* 上限缩小后尝试换号；失败仍保留原 CPU-owned CID。 */
		cpu_cid = mm_cid_converge(mm, cpu_cid, max_cids);
	} else {
		/* Hand over or drop the task owned CID */
		/* 慢路径先解决 task 与 CPU 两侧可能同时/均不拥有的冲突，再确保 CPU 侧得到唯一 CID。 */
		if (cid_on_task(tcid)) {
			if (cid_on_cpu(cpu_cid))
				mm_unset_cid_on_task(t);
			else
				cpu_cid = cid_to_cpu_cid(tcid);
		}
		/* Still nothing, allocate a new one */
		/* 两侧都没有 ownership 时分配新位，并编码为 CPU-owned。 */
		if (!cid_on_cpu(cpu_cid))
			cpu_cid = cid_to_cpu_cid(mm_get_cid(mm));

		/* Handle the transition mode flag if required */
		/* 模式转换期间清 ONCPU 并设 TRANSIT，调出路径会完成最终 ownership 选择。 */
		if (mode & MM_CID_TRANSIT)
			cpu_cid = cpu_cid_to_cid(cpu_cid) | MM_CID_TRANSIT;
	}
	mm_cid_update_pcpu_cid(mm, cpu_cid);
	/* 提交阶段同时更新 CPU 与 task 快照，使下一次调入命中共同快速路径。 */
	mm_cid_update_task_cid(t, cpu_cid);
}

/*
 * 业务背景：mm_cid_from_task() 以 task 槽为主要 owner，把其 CID 安装到当前 CPU 并处理旧 CPU-owned 值。
 * 入参：t 为调入借用任务；cpu_cid 为本 CPU 槽快照；mode 为 mm 模式位。
 * 出参/返回：void；可能转移、分配或释放 CID，最终同步 task/per-CPU 槽并通知 rseq。
 * 注意事项：调用者不可迁移且处于 context switch；位图 ownership 在所有分支保持唯一。
 */
static __always_inline void mm_cid_from_task(struct task_struct *t, unsigned int cpu_cid,
					     unsigned int mode)
{
	unsigned int max_cids, tcid = t->mm_cid.cid;
	struct mm_struct *mm = t->mm;

	max_cids = READ_ONCE(mm->mm_cid.max_cids);
	/* Optimize for the common case, where both have the ONCPU bit clear */
	/* 快速路径：task 和 CPU 快照都不是 CPU-owned，优先复用 task 的数值 CID。 */
	if (likely(cid_on_task(tcid | cpu_cid))) {
		if (likely(tcid < max_cids)) {
			mm_cid_update_pcpu_cid(mm, tcid);
			return;
		}
		/* Try to converge into the optimal CID space */
		/* task CID 超出新上限时尝试换到收敛区间，失败则继续保留旧位。 */
		tcid = mm_cid_converge(mm, tcid, max_cids);
	} else {
		/* Hand over or drop the CPU owned CID */
		/* 慢路径先消解 CPU-owned 值：task 已有值则释放 CPU 位，否则把 CPU 值交给 task。 */
		if (cid_on_cpu(cpu_cid)) {
			if (cid_on_task(tcid))
				mm_drop_cid_on_cpu(mm, this_cpu_ptr(mm->mm_cid.pcpu));
			else
				tcid = cpu_cid_to_cid(cpu_cid);
		}
		/* Still nothing, allocate a new one */
		/* 两侧都无普通 task-owned CID 时分配新位。 */
		if (!cid_on_task(tcid))
			tcid = mm_get_cid(mm);
		/* Set the transition mode flag if required */
		/* TRANSIT 位把临时 ownership 延迟到 schedout 按最终 mode 收尾。 */
		tcid |= mode & MM_CID_TRANSIT;
	}
	mm_cid_update_pcpu_cid(mm, tcid);
	/* 两槽提交相同编码，保证下一次切换可从共同快照继续。 */
	mm_cid_update_task_cid(t, tcid);
}

/* 调入时按 mode 在 task 与 per-CPU 槽之间交接 CID，非 active 任务跳过。 */
/*
 * 业务背景：mm_cid_schedin() 在 context switch 调入时按 mm mode 选择 task-owned 或 CPU-owned 交接算法。
 * 入参：next 为即将运行的借用任务，必须拥有有效 mm；无引用变化。
 * 出参/返回：void；inactive 快速返回，active 时同步 task/per-CPU CID并可能更新 rseq。
 * 注意事项：调用者不可迁移，读取 this_cpu 槽与写入必须发生在同一 CPU；函数不可睡眠。
 */
static __always_inline void mm_cid_schedin(struct task_struct *next)
{
	struct mm_struct *mm = next->mm;
	unsigned int cpu_cid, mode;

	if (!next->mm_cid.active)
		/* 未启用 CID 的任务不参与位图 ownership，避免无意义读写 per-CPU 槽。 */
		return;

	cpu_cid = __this_cpu_read(mm->mm_cid.pcpu->cid);
	mode = READ_ONCE(mm->mm_cid.mode);
	/* mode 的 ONCPU 位决定以哪一侧作为权威 owner，转换位由被调 helper 继续传播。 */
	if (likely(!cid_on_cpu(mode)))
		mm_cid_from_task(next, cpu_cid, mode);
	else
		mm_cid_from_cpu(next, cpu_cid, mode);
}

/* 仅 transition CID 需要调出收尾：可收敛则转移，否则清位并设 UNSET。 */
/*
 * 业务背景：mm_cid_schedout() 在任务调出时只处理 TRANSIT CID，把临时 ownership 收敛到最终 mode。
 * 入参：prev 为刚停止运行的借用任务，mm 生命周期由切换路径稳定。
 * 出参/返回：void；普通 CID 无操作；可收敛时同步两槽，否则释放位并把 task 设 UNSET。
 * 注意事项：必须先判断最终 mode/上限再决定转移，防止保留越界或双重 ownership；不可睡眠。
 */
static __always_inline void mm_cid_schedout(struct task_struct *prev)
{
	struct mm_struct *mm = prev->mm;
	unsigned int mode, cid;

	/* During mode transitions CIDs are temporary and need to be dropped */
	/* 非 TRANSIT 值已经由正常 owner 管理，无需调出清理。 */
	if (likely(!cid_in_transit(prev->mm_cid.cid)))
		return;

	mode = READ_ONCE(mm->mm_cid.mode);
	cid = cid_from_transit_cid(prev->mm_cid.cid);

	/*
	 * If transition mode is done, transfer ownership when the CID is
	 * within the convergence range to optimize the next schedule in.
	 */
	/* 转换结束且 CID 仍在上限内时保留它，避免下一次 schedin 重新扫描位图。 */
	if (!cid_in_transit(mode) && cid < READ_ONCE(mm->mm_cid.max_cids)) {
		if (cid_on_cpu(mode))
			cid = cid_to_cpu_cid(cid);

		/* Update both so that the next schedule in goes into the fast path */
		/* 同时提交 per-CPU 与 task 槽，下一次调入看到一致编码。 */
		mm_cid_update_pcpu_cid(mm, cid);
		prev->mm_cid.cid = cid;
	} else {
		/* 转换仍进行或 CID 已越界时归还位图，task 留 UNSET 让下一次重新分配。 */
		mm_drop_cid(mm, cid);
		prev->mm_cid.cid = MM_CID_UNSET;
	}
}

/*
 * 业务背景：mm_cid_switch_to() 按“先收尾 prev、再为 next 取得 CID”的顺序连接一次任务切换。
 * 入参：prev/next 均为切换路径稳定的借用任务。
 * 出参/返回：void；可能改变两任务及当前 CPU CID 槽，无引用变化。
 * 注意事项：顺序不可交换，否则 next 分配可能看不到 prev 刚释放的位；函数不可睡眠。
 */
static inline void mm_cid_switch_to(struct task_struct *prev, struct task_struct *next)
{
	mm_cid_schedout(prev);
	mm_cid_schedin(next);
}

#else /* !CONFIG_SCHED_MM_CID: */
/* MM CID 未编译时切换 hook 为无副作用 stub，prev/next 参数仅保持统一调用链。 */
/*
 * 业务背景：mm_cid_switch_to() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline void mm_cid_switch_to(struct task_struct *prev, struct task_struct *next) { }
#endif /* !CONFIG_SCHED_MM_CID */
/* MM CID 调度切换接口的配置分支到此结束。 */

#ifdef CONFIG_SCHED_CACHE
/* present 表示硬件/拓扑支持，active 表示运行时已启用；sysctl 与 LLC 阈值控制 epoch 聚合策略。 */
DECLARE_STATIC_KEY_FALSE(sched_cache_present);
DECLARE_STATIC_KEY_FALSE(sched_cache_active);
extern int sysctl_sched_cache_user;
extern unsigned int llc_aggr_tolerance;
extern unsigned int llc_epoch_period;
extern unsigned int llc_epoch_affinity_timeout;
extern unsigned int llc_imb_pct;
extern unsigned int llc_overaggr_pct;

/* sched_cache_enabled() 查询运行时 active 静态键；无入参、副作用或睡眠。 */
/*
 * 业务背景：sched_cache_enabled() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline bool sched_cache_enabled(void)
{
	return static_branch_unlikely(&sched_cache_active);
}

extern void sched_cache_active_set(void);

#endif

void sched_domains_free_llc_id(int cpu);

/* init_sched_mm() 初始化新任务地址空间相关的调度状态，包括启用配置下的 MM CID。 */
extern void init_sched_mm(struct task_struct *p);

/* EEVDF helper 计算 cfs_rq 平均虚拟运行时间并判断实体当前是否 eligible。 */
extern u64 avg_vruntime(struct cfs_rq *cfs_rq);
extern int entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se);
/* 源、目的 rq 均已锁时完成离队、改 CPU、入队和抢占检查。 */
/*
 * 业务背景：move_queued_task_locked() 在已持源/目的 rq 双锁时原子迁移一个已入队任务。
 * 入参：src_rq/dst_rq 为已锁借用队列；task 为源队列上借用任务。
 * 出参/返回：void；依次离队、发布新 CPU、入队并检查抢占，不转移 task 引用。
 * 注意事项：两锁须按统一全序取得；set_task_cpu 是发布边界，步骤不可交换且函数不可睡眠。
 */
static inline
void move_queued_task_locked(struct rq *src_rq, struct rq *dst_rq, struct task_struct *task)
{
	lockdep_assert_rq_held(src_rq);
	lockdep_assert_rq_held(dst_rq);

	/* 构造阶段先从源队列撤销所有类计数，再改变 task CPU 归属。 */
	deactivate_task(src_rq, task, 0);
	set_task_cpu(task, dst_rq->cpu);
	/* 发布阶段把任务加入目标类队列，并让目标 current 感知可能需要抢占。 */
	activate_task(dst_rq, task, 0);
	wakeup_preempt(dst_rq, task, 0);
}

/* push 候选必须不在执行且目标在亲和性内；调用者锁住 rq 后使用。 */
/*
 * 业务背景：task_is_pushable() 检查任务是否既未在执行、又允许迁往给定 CPU。
 * 入参：rq 为已锁源队列；p 为借用任务；cpu 为候选目标编号。
 * 出参/返回：两条件满足返回真，否则假；无副作用。
 * 注意事项：只检查 cpus_mask，不检查 online/capacity；真正迁移仍需目标锁下复验，函数不睡眠。
 */
static inline
bool task_is_pushable(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_on_cpu(rq, p) &&
	    cpumask_test_cpu(cpu, &p->cpus_mask))
		return true;

	return false;
}

#ifdef CONFIG_RT_MUTEXES

/*
 * __rt_effective_prio() 合并基础 prio 与 PI 顶层捐赠者优先级；pi_task 可空且为借用，返回数值最小值。
 * helper 不锁定捐赠链，调用者必须在 rt_mutex/PI 协议下提供稳定快照；无副作用且不睡眠。
 */
/*
 * 业务背景：__rt_effective_prio() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int __rt_effective_prio(struct task_struct *pi_task, int prio)
{
	if (pi_task)
		prio = min(prio, pi_task->prio);

	return prio;
}

/*
 * rt_effective_prio() 从 p 的 rt_mutex 顶层 waiter 取得借用快照，再计算有效优先级。
 * p 为借用 task，prio 为基础值；返回考虑 PI 后的值，无 ownership 转移，调用者负责所需 PI 同步。
 */
/*
 * 业务背景：rt_effective_prio() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int rt_effective_prio(struct task_struct *p, int prio)
{
	struct task_struct *pi_task = rt_mutex_get_top_task(p);

	return __rt_effective_prio(pi_task, prio);
}

#else /* !CONFIG_RT_MUTEXES: */

/* 未编译 RT mutex 时没有 PI 捐赠者，effective prio 恒等于传入基础 prio，p 不读取。 */
/*
 * 业务背景：rt_effective_prio() 完成本配置路径下名称所示的轻量调度辅助操作。
 * 入参：参数均按声明借用或按值传入，调用者保持对象生命周期与必要锁。
 * 出参/返回：按函数体返回查询值或提交局部状态更新，不转移 ownership。
 * 注意事项：函数不额外建立同步；结果需要稳定时由调用者按相邻协议复验，内联路径不可睡眠。
 */
static inline int rt_effective_prio(struct task_struct *p, int prio)
{
	return prio;
}

#endif /* !CONFIG_RT_MUTEXES */
/* RT mutex PI 有效优先级接口的配置分支到此结束。 */

extern int __sched_setscheduler(struct task_struct *p, const struct sched_attr *attr, bool user, bool pi);
extern int __sched_setaffinity(struct task_struct *p, struct affinity_context *ctx);
/* 策略更改接口选择类、重算 weight，并通过统一 enqueue/dequeue wrapper 提交队列状态。 */
extern const struct sched_class *__setscheduler_class(int policy, int prio);
extern void set_load_weight(struct task_struct *p, bool update_load);
extern void enqueue_task(struct rq *rq, struct task_struct *p, int flags);
extern bool dequeue_task(struct rq *rq, struct task_struct *p, int flags);

extern struct balance_callback *splice_balance_callbacks(struct rq *rq);

/* balance callback 先在 rq 锁下摘链，再于规定的锁边界逐个执行；head 节点 ownership 由提交者维持。 */
extern void __balance_callbacks(struct rq *rq, struct rq_flags *rf);
extern void balance_callbacks(struct rq *rq, struct balance_callback *head);

/*
 * The 'sched_change' pattern is the safe, easy and slow way of changing a
 * task's scheduling properties. It dequeues a task, such that the scheduler
 * is fully unaware of it; at which point its properties can be modified;
 * after which it is enqueued again.
 *
 * Typically this must be called while holding task_rq_lock, since most/all
 * properties are serialized under those locks. There is currently one
 * exception to this rule in sched/ext which only holds rq->lock.
 */
/*
 * sched_change 采用“临时离队 -> 修改属性 -> 重新入队”的慢速事务，使各调度类不会观察半更新状态。
 * 通常必须持 task_rq_lock；sched/ext 的已知例外只持 rq 锁。begin 成功后的上下文必须交给 end 收尾。
 */

/*
 * This structure is a temporary, used to preserve/convey the queueing state
 * of the task between sched_change_begin() and sched_change_end(). Ensuring
 * the task's queueing state is idempotent across the operation.
 */
/*
 * 临时上下文保存原优先级、task/类借用指针、操作 flags 以及入口时 queued/running 状态。
 * begin 创建并可能离队，end 按这些快照恢复可见性；对象只在词法事务期间有效，不拥有 task 引用。
 */
struct sched_change_ctx {
	/* prio/class 是修改前快照，p 是目标借用任务，flags 解释本次属性变化类型。 */
	u64			prio;
	struct task_struct	*p;
	const struct sched_class *class;
	int			flags;
	/* queued/running 保证 end 只恢复入口实际存在的队列/执行状态，使事务幂等。 */
	bool			queued;
	bool			running;
};

struct sched_change_ctx *sched_change_begin(struct task_struct *p, unsigned int flags);
void sched_change_end(struct sched_change_ctx *ctx);

/* 自动 cleanup class 保证所有作用域出口都调用 sched_change_end()，避免任务永久留在离队状态。 */
DEFINE_CLASS(sched_change, struct sched_change_ctx *,
	     sched_change_end(_T),
	     sched_change_begin(p, flags),
	     struct task_struct *p, unsigned int flags)

DEFINE_CLASS_IS_UNCONDITIONAL(sched_change)

/* sched_ext 依赖上述 rq、锁和调度类 ABI，故在全部基础定义完成后再包含其内部接口。 */
#include "ext/ext.h"

#endif /* _KERNEL_SCHED_SCHED_H */
/* 本调度器内部头文件的保护范围到此结束。 */
