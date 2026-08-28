// SPDX-License-Identifier: GPL-2.0
/*
 * Deadline Scheduling Class (SCHED_DEADLINE)
 *
 * Earliest Deadline First (EDF) + Constant Bandwidth Server (CBS).
 *
 * Tasks that periodically executes their instances for less than their
 * runtime won't miss any of their deadlines.
 * Tasks that are not periodic or sporadic or that tries to execute more
 * than their reserved bandwidth will be slowed down (and may potentially
 * miss some of their deadlines), and won't affect any other task.
 *
 * Copyright (C) 2012 Dario Faggioli <raistlin@linux.it>,
 *                    Juri Lelli <juri.lelli@gmail.com>,
 *                    Michael Trimarchi <michael@amarulasolutions.com>,
 *                    Fabio Checconi <fchecconi@gmail.com>
 */
/*
 * Deadline 调度类以 EDF 选择绝对截止期最早的实体，并用 CBS 隔离每个实体的保留带宽。
 * 周期任务在每期 runtime 预算内执行即可获得截止期保障；超出预留的任务会被限速，
 * 可能错过自身截止期，但不应侵占其他任务的保留执行时间。作者与版权信息保持如上。
 */

#include <linux/cpuset.h>
#include <linux/sched/clock.h>
#include <linux/sched/deadline.h>
#include <uapi/linux/sched/types.h>
#include "sched.h"
#include "pelt.h"

/*
 * SCHED_DEADLINE 把每个实体建模为 CBS 服务器：runtime 是每个 period 可消费的
 * 预算，absolute deadline 同时是 EDF 红黑树的排序键。rq 锁保护实体的排队、
 * 预算和 timer 状态；root_domain 的 dl_bw 锁负责跨 CPU 的准入总量。任务用尽
 * 预算后被 throttle，由 replenish timer 推进 deadline 并恢复 runtime；SMP
 * push/pull 只把 cpupri/cpudl 候选当提示，迁移前必须双 rq 加锁并重新验证。
 * inactive timer 延迟归还仍可能被任务再次消费的带宽，避免睡眠/迁移窗口破坏
 * GRUB 记账；PI boost 则借用等待者的 deadline，但不能绕过带宽所有权。
 */

/*
 * 全局参数与局部对象的关系：用户态通过 sched_setattr() 提交 runtime/period/deadline，
 * 这些参数经过准入检查后折算为 dl_bw，分别计入 root_domain 的 total_bw、rq 的
 * this_bw 和当前可运行实体的 running_bw。下面的 helper 都在调度器内部使用，指针
 * 是借用引用；调用者负责 rq 锁、sched RCU 或 task 生命周期，helper 不分配对象，也
 * 不负责迁移任务。阅读这段代码时要区分“已保留但当前不运行”的 this_bw 与“当前
 * contending、会影响 CPU 需求”的 running_bw，二者不能互相替代。
 */

/*
 * Default limits for DL period; on the top end we guard against small util
 * tasks still getting ridiculously long effective runtimes, on the bottom end we
 * guard against timer DoS.
 */
/*
 * DL period 的默认边界：上端避免低利用率任务获得荒谬地长的有效运行时间，
 * 下端则防止通过极短周期制造 timer 拒绝服务。
 */
static unsigned int sysctl_sched_dl_period_max = 1 << 22; /* ~4 seconds */
static unsigned int sysctl_sched_dl_period_min = 100;     /* 100 us */
/* 默认上限约 4 秒、下限 100 微秒；这里的存储单位是微秒。 */

/*
 * 两个 period 参数是微秒单位的系统级边界：上限避免极小利用率任务得到过长的实际
 * 运行窗口，下限避免极短 timer 周期造成 timer DoS。它们是 sysctl 表的 backing
 * storage；修改通过 proc_douintvec_minmax() 受另一端边界约束，不直接改变已有任务
 * 的 CBS 状态。没有 CONFIG_SYSCTL 时数组和注册入口被裁剪，但默认值仍作为校验依据。
 */
#ifdef CONFIG_SYSCTL
static const struct ctl_table sched_dl_sysctls[] = {
	{
		/* 上界项把用户写入绑定到最小 period，防止 max 小于 min。 */
		.procname       = "sched_deadline_period_max_us",
		.data           = &sysctl_sched_dl_period_max,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = proc_douintvec_minmax,
		.extra1         = (void *)&sysctl_sched_dl_period_min,
	},
	{
		/* 下界项把用户写入绑定到最大 period，防止过短 timer 周期。 */
		.procname       = "sched_deadline_period_min_us",
		.data           = &sysctl_sched_dl_period_min,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		.proc_handler   = proc_douintvec_minmax,
		.extra2         = (void *)&sysctl_sched_dl_period_max,
	},
};

/* 注册 period 上下界；边界互相引用，用户写入不可能形成反向区间。 */
/*
 * sched_dl_sysctl_init() - 在 late_initcall 阶段注册 DL period sysctl
 *
 * 无入参；始终返回 0，注册副作用由 sysctl core 持有。调用时可睡眠且尚无并发注销；
 * CONFIG_SYSCTL 关闭时函数和表均不存在。注册后用户写入由 minmax handler 校验。
 */
static int __init sched_dl_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_dl_sysctls);
	return 0;
}
late_initcall(sched_dl_sysctl_init);
#endif /* CONFIG_SYSCTL */

/* dl_rq 内嵌于 rq，返回值仅借用，访问可变字段仍需持 rq 锁。 */
/*
 * rq_of_dl_rq() - 由内嵌 dl_rq 反解所属 rq
 *
 * 入参不可空且必须确为 rq->dl；返回借用 rq，不改引用。container_of 只做地址换算，
 * 调用者仍须以 rq 锁保护可变字段，错误传入非内嵌对象会得到无效地址。
 */
static inline struct rq *rq_of_dl_rq(struct dl_rq *dl_rq)
{
	return container_of(dl_rq, struct rq, dl);
}

/* server 记录所属 rq，普通任务则以 task_rq() 为准，覆盖迁移后的归属。 */
/*
 * rq_of_dl_se() - 按实体类型取得当前所属 rq
 *
 * 入参 dl_se 为存活借用实体；server 返回固定 dl_se->rq，普通 task 通过 task_rq()
 * 读取可迁移归属。返回 rq 仍为借用指针；调用者负责 task/rq 锁协议以稳定迁移竞态。
 */
static inline struct rq *rq_of_dl_se(struct sched_dl_entity *dl_se)
{
	struct rq *rq = dl_se->rq;

	if (!dl_server(dl_se))
		rq = task_rq(dl_task_of(dl_se));

	return rq;
}

/*
 * dl_rq_of_se() - 取得实体当前 rq 内嵌的 DL 队列
 *
 * 入参和迁移同步要求继承 rq_of_dl_se()；返回借用 dl_rq，无副作用、不增引用。
 */
static inline struct dl_rq *dl_rq_of_se(struct sched_dl_entity *dl_se)
{
	/* 实体的 rq 归属由 server/task 类型 helper 决定；这里只返回 rq 内嵌的 dl_rq。 */
	return &rq_of_dl_se(dl_se)->dl;
}

/*
 * on_dl_rq() - 判断实体是否已链接到本地 EDF 红黑树
 *
 * 业务背景：enqueue/dequeue、pick 和预算耗尽路径需要区分“实体在队列中”与“实体
 * 恰好正在运行”；红黑树节点是否为空只表达前者。入参：dl_se 是不可空的借用实体，
 * 不增加引用；出参/返回：返回非零表示 rb_node 已被初始化并链接，返回零表示未入队，
 * 不修改实体。注意事项：调用者必须用 rq 锁保护节点状态；当前任务可能在 rq 上运行但
 * 不在 EDF 树中，不能把返回值当作 running 状态或可迁移状态。
 */

/* RB 节点非空表示实体已链接 EDF 树，不等同于当前正在执行。 */
static inline int on_dl_rq(struct sched_dl_entity *dl_se)
{
	return !RB_EMPTY_NODE(&dl_se->rb_node);
}

#ifdef CONFIG_RT_MUTEXES

/*
 * pi_of() - 取得 deadline 实体当前用于 PI 的代理实体
 *
 * 业务背景：RT-mutex 优先级继承可能让 deadline 任务暂时借用等待者的调度实体，
 * 该 helper 统一返回 PI 链上的当前实体。入参：dl_se 是持有 rq/PI 协议的借用实体；
 * 出参/返回：返回 pi_se 指向的借用实体，不转移引用、不改变字段。注意事项：返回值
 * 只在对应 PI 状态和保护范围内有效，引用计数与字段并发由上层锁协议负责；关闭
 * CONFIG_RT_MUTEXES 时实现退化为返回自身，保持调用者的不变量不变。
 */
static inline struct sched_dl_entity *pi_of(struct sched_dl_entity *dl_se)
{
	return dl_se->pi_se;
}

/*
 * is_dl_boosted() - 判断实体是否正在使用 PI 借来的 deadline
 *
 * 业务背景：CBS 预算、EDF 排序和撤销 PI 时需要知道 deadline 是否来自原任务；
 * 调用者通过 pi_of() 比较代理与自身。入参：dl_se 是借用实体；出参/返回：RT-mutex
 * 开启时返回 pi_se != dl_se，关闭时恒为 false，无副作用。注意事项：这是状态快照，
 * 必须在 PI 状态保护下使用，不能仅凭该布尔值延长对象生命周期或跳过带宽记账。
 */
static inline bool is_dl_boosted(struct sched_dl_entity *dl_se)
{
	return pi_of(dl_se) != dl_se;
}
#else /* !CONFIG_RT_MUTEXES: */

/*
 * 无 RT-mutex 支持时，pi_of() 返回原实体，is_dl_boosted() 恒为 false；这不是删除
 * 调用者协议，而是让相同的 EDF/CBS 主路径在配置裁剪后仍能编译，并保留“没有代理
 * deadline”的不变量。两者的入参都是借用 dl_se，返回没有引用和状态副作用。
 */
static inline struct sched_dl_entity *pi_of(struct sched_dl_entity *dl_se)
{
	return dl_se;
}

/* 上述退化配置下没有 PI 代理，因此该查询对所有实体返回 false。 */
static inline bool is_dl_boosted(struct sched_dl_entity *dl_se)
{
	return false;
}
#endif /* !CONFIG_RT_MUTEXES */

/*
 * dl_get_type() - 把实体归类为普通 deadline 任务或 rq 内建 server
 *
 * 业务背景：server 与普通任务共享 deadline 调度类，但其带宽来源、计时和停止路径
 * 不同，timer/统计代码必须先识别类型。入参：dl_se 是借用实体，rq 是其当前所属 rq，
 * 两者不可空且由调用者稳定；出参/返回：返回 DL_TASK、DL_SERVER_FAIR、可选的
 * DL_SERVER_EXT 或 DL_OTHER，表示后续应采用的处理协议；不修改对象。注意事项：
 * CONFIG_SCHED_CLASS_EXT 只决定是否识别 ext_server，未匹配的 server 仍归 DL_OTHER；
 * 该值是分类结果，不是 EDF 优先级，也不授予额外 ownership。
 */
static inline u8 dl_get_type(struct sched_dl_entity *dl_se, struct rq *rq)
{
	/* 先区分普通 task，再识别 rq 内建 server；返回值决定后续 timer/统计分派。 */
	if (!dl_server(dl_se))
		return DL_TASK;
	if (dl_se == &rq->fair_server)
		return DL_SERVER_FAIR;
#ifdef CONFIG_SCHED_CLASS_EXT
	if (dl_se == &rq->ext_server)
		return DL_SERVER_EXT;
#endif
	return DL_OTHER;
}

/* root_domain 可由拓扑重建替换，调用者必须处于 sched RCU 读侧。 */
/*
 * dl_bw_of() - 取得 CPU 所属 root_domain 的 deadline 带宽状态
 *
 * 业务背景：准入控制必须在共享 root_domain 上累计跨 CPU 的保留带宽，而拓扑重建
 * 可能替换 rq->rd。入参：i 是有效 CPU 编号；出参/返回：返回 dl_bw 的借用指针，
 * 不增加引用、不修改状态。注意事项：调用者必须持有 sched RCU 读锁，RCU 只保证
 * root_domain 对象在本次读取期间不被释放，不冻结其中字段；需要修改 total_bw 的
 * 调用者还必须遵守 dl_bw 锁协议。
 */
static inline struct dl_bw *dl_bw_of(int i)
{
	/* RCU 读侧内读取 root_domain；返回的 dl_bw 只能在该生命周期保护下借用。 */
	RCU_LOCKDEP_WARN(!rcu_read_lock_sched_held(),
			 "sched RCU must be held");
	return &cpu_rq(i)->rd->dl_bw;
}

/*
 * dl_bw_cpus() - 统计 root_domain 中仍处于 active 状态的 CPU 数
 *
 * 业务背景：对称机器可以按 CPU 数估算 deadline 带宽容量。入参：i 是有效 CPU 编号，
 * 用来找到其 root_domain；出参/返回：返回 rd->span 与 cpu_active_mask 的交集大小，
 * 单位为 CPU 个数，不改变 mask。注意事项：rq->rd 受 sched RCU 保护，调用者必须在
 * RCU 读侧；CPU hotplug 改变 active mask 时结果只是当前快照，不能跨锁边界缓存。
 */
static inline int dl_bw_cpus(int i)
{
	struct root_domain *rd = cpu_rq(i)->rd;
	/* admission 分摊只使用 root span 与 active mask 的交集。 */

	RCU_LOCKDEP_WARN(!rcu_read_lock_sched_held(),
			 "sched RCU must be held");

	return cpumask_weight_and(rd->span, cpu_active_mask);
}

/*
 * __dl_bw_capacity() - 累加 mask 中 active CPU 的架构 capacity
 *
 * 业务背景：非对称 CPU 不能把每个 CPU 当作相同容量，准入分母必须反映大小核差异。
 * 入参：mask 是只读的借用 CPU 集合；出参/返回：返回 active CPU 的
 * arch_scale_cpu_capacity() 之和，单位为调度 capacity 刻度；无状态或 ownership 变化。
 * 注意事项：调用者负责保证 mask 生命周期和 CPU 拓扑读取协议，函数本身不持 rq 锁，
 * 也不睡眠；若 mask 为空返回 0，后续调用者必须避免用它进行无意义的除法。
 */
static inline unsigned long __dl_bw_capacity(const struct cpumask *mask)
{
	unsigned long cap = 0;
	int i;

	for_each_cpu_and(i, mask, cpu_active_mask)
		/* offline CPU 不应贡献当前可承载的 deadline capacity。 */
		cap += arch_scale_cpu_capacity(i);

	return cap;
}

/*
 * XXX Fix: If 'rq->rd == def_root_domain' perform AC against capacity
 * of the CPU the task is running on rather rd's \Sum CPU capacity.
 */
/* 对称机器用 CPU 数快算，否则累加实际 capacity 作为准入分母。 */
/*
 * dl_bw_capacity() - 选择对称机器快路径或非对称容量求和路径
 *
 * 业务背景：deadline admission control 需要一个 root_domain 可承载的总容量；对称
 * 机器按 CPU 数左移更快，非对称机器则调用 __dl_bw_capacity() 保留大小核差异。入参：
 * i 是有效 CPU 编号；出参/返回：返回与 dl_bw 同刻度的容量上限，不改变拓扑。注意事项：
 * 非对称分支和 dl_bw_cpus() 都要求 sched RCU 读锁；文件保留的英文 XXX 注释只描述
 * 一个尚未修复的 default root_domain 特殊情况，不能把当前实现误读成该问题已解决。
 */
static inline unsigned long dl_bw_capacity(int i)
{
	/* 对称且满 capacity 时走 CPU 数快路径，否则读取 root span 的实际容量总和。 */
	if (!sched_asym_cpucap_active() &&
	    arch_scale_cpu_capacity(i) == SCHED_CAPACITY_SCALE) {
		return dl_bw_cpus(i) << SCHED_CAPACITY_SHIFT;
	} else {
		RCU_LOCKDEP_WARN(!rcu_read_lock_sched_held(),
				 "sched RCU must be held");

		return __dl_bw_capacity(cpu_rq(i)->rd->span);
	}
}

/* 用 root_domain cookie 去重跨 CPU 遍历；调用方负责 cookie 的本轮唯一性。 */
/*
 * dl_bw_visited() - 用 root_domain cookie 抑制一次遍历中的重复处理
 *
 * 业务背景：跨 CPU 遍历可能从同一 root_domain 的多个 CPU 进入，重复处理会重复
 * 累加或迁移带宽。入参：cpu 是用来定位 root_domain 的有效 CPU，cookie 是本轮遍历
 * 的唯一标识；两者均为纯输入。出参/返回：若 rd->visit_cookie 已等于 cookie 返回
 * true，否则写入 cookie 并返回 false；写入的是本轮去重状态，不转移 ownership。注意：
 * 调用者必须保证 cookie 的本轮唯一性并遵守 root_domain 生命周期协议。
 */
bool dl_bw_visited(int cpu, u64 cookie)
{
	struct root_domain *rd = cpu_rq(cpu)->rd;

	/* 相同 cookie 表示该 root_domain 已被本轮跨 CPU 遍历处理。 */
	if (rd->visit_cookie == cookie)
		return true;

	rd->visit_cookie = cookie;
	return false;
}

/*
 * __dl_update() - 将 root_domain 带宽变化分摊到所有 active rq
 *
 * 业务背景：任务从 root_domain 的 total_bw 加入或移除后，每个 rq 都要获得相反方向
 * 的 extra_bw 调整，后续本地 reclaim 才能看到新的可回收余量。入参：dl_b 是内嵌于
 * root_domain 的借用 dl_bw，bw 是带符号的带宽增量；出参/返回：无直接返回值，更新
 * rd->span 内 active rq 的 dl.extra_bw。注意事项：必须持 sched RCU 读锁；调用者
 * 负责在更高层持有带宽修改锁，函数不睡眠，不能在 root_domain 释放后继续使用 rd。
 */
static inline
/* 在 sched RCU 下把 root_domain 带宽变化摊入各活动 rq 的可回收余量。 */
void __dl_update(struct dl_bw *dl_b, s64 bw)
{
	struct root_domain *rd = container_of(dl_b, struct root_domain, dl_bw);
	int i;

	RCU_LOCKDEP_WARN(!rcu_read_lock_sched_held(),
			 "sched RCU must be held");
	for_each_cpu_and(i, rd->span, cpu_active_mask) {
		struct rq *rq = cpu_rq(i);

		/* 把全局带宽变化映射到每个 active rq 的额外可回收量。 */
		rq->dl.extra_bw += bw;
	}
}

/*
 * __dl_sub() - 从 root_domain 总保留量扣除任务带宽
 *
 * 业务背景：任务撤销 deadline reservation 时，需要同时减少 total_bw，并把平均到
 * 各 CPU 的余量变化交给 __dl_update()。入参：dl_b 是借用带宽状态，tsk_bw 是要移除
 * 的定点带宽，cpus 是 active CPU 数且非零；出参/返回：无返回值，修改 total_bw 和
 * 各 rq extra_bw，不转移对象所有权。注意事项：调用者必须保证 tsk_bw 已属于该
 * root_domain、cpus 与当前拓扑匹配，并持有上层带宽锁。
 */
static inline
void __dl_sub(struct dl_bw *dl_b, u64 tsk_bw, int cpus)
{
	dl_b->total_bw -= tsk_bw;
	__dl_update(dl_b, (s32)tsk_bw / cpus);
}

/*
 * __dl_add() - 向 root_domain 总保留量加入任务带宽
 *
 * 业务背景：新任务通过 admission control 后，把其 reservation 计入共享总量；本地
 * rq 的 extra_bw 取反方向变化，以便把全局容量变化反映到各 CPU。入参：dl_b、tsk_bw、
 * cpus 分别是借用的 root_domain 状态、要加入的定点带宽和非零 active CPU 数；出参/
 * 返回：无直接返回值，更新 total_bw/extra_bw，不分配或释放对象。注意事项：必须在
 * 与 __dl_sub() 对称的带宽锁和 sched RCU 保护下调用，不能把该 helper 当作准入检查。
 */
static inline
void __dl_add(struct dl_bw *dl_b, u64 tsk_bw, int cpus)
{
	dl_b->total_bw += tsk_bw;
	__dl_update(dl_b, -((s32)tsk_bw / cpus));
}

/* 比较替换 old_bw 后的总量与容量上限；bw==-1 明确表示关闭准入限制。 */
/*
 * __dl_overflow() - 判断替换 reservation 后是否超过容量上限
 *
 * 业务背景：修改任务参数时要先用 old_bw 抵消旧 reservation，再加入 new_bw，不能
 * 只检查 new_bw。入参：dl_b 是借用 root_domain 带宽状态，cap 是容量刻度，old_bw/
 * new_bw 是同一固定点单位的旧/新带宽；出参/返回：true 表示替换后总量超出
 * cap_scale(dl_b->bw, cap)，false 表示未超出；不修改状态。注意事项：dl_b->bw == -1
 * 表示显式关闭准入限制，此时恒不报告 overflow；调用者仍须处理整数刻度和锁协议。
 */
static inline bool
__dl_overflow(struct dl_bw *dl_b, unsigned long cap, u64 old_bw, u64 new_bw)
{
	return dl_b->bw != -1 &&
	       cap_scale(dl_b->bw, cap) < dl_b->total_bw - old_bw + new_bw;
}

/* running_bw 只统计当前可运行实体，必须在 rq 锁下修改并通知 cpufreq。 */
static inline
void __add_running_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->running_bw;

	lockdep_assert_rq_held(rq_of_dl_rq(dl_rq));
	dl_rq->running_bw += dl_bw;
	/* 加法后 running_bw 不得回绕，也不能超过 this_bw 的 reservation ownership。 */
	WARN_ON_ONCE(dl_rq->running_bw < old); /* overflow */
	WARN_ON_ONCE(dl_rq->running_bw > dl_rq->this_bw);
	/* kick cpufreq (see the comment in kernel/sched/sched.h). */
	/* 这里主动通知 cpufreq，详细原因见 sched.h：DL active utilization 已发生变化。 */
	cpufreq_update_util(rq_of_dl_rq(dl_rq), 0);
}

/* 下溢时钳位为 0，避免损坏值继续传播到 reclaim/cpufreq。 */
static inline
void __sub_running_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->running_bw;

	lockdep_assert_rq_held(rq_of_dl_rq(dl_rq));
	dl_rq->running_bw -= dl_bw;
	/* 无符号减法回绕时结果变大，下面检测后归零，避免污染 reclaim/cpufreq。 */
	WARN_ON_ONCE(dl_rq->running_bw > old); /* underflow */
	if (dl_rq->running_bw > old)
		dl_rq->running_bw = 0;
	/* kick cpufreq (see the comment in kernel/sched/sched.h). */
	/* running_bw 减少同样会改变频率需求，因此对称通知 cpufreq。 */
	cpufreq_update_util(rq_of_dl_rq(dl_rq), 0);
}

/* this_bw 是该 rq 拥有的全部保留量，包括暂时不运行的实体。 */
static inline
void __add_rq_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->this_bw;

	lockdep_assert_rq_held(rq_of_dl_rq(dl_rq));
	dl_rq->this_bw += dl_bw;
	/* this_bw 是 rq 持有的全部 reservation，active 任务只是其中的子集。 */
	WARN_ON_ONCE(dl_rq->this_bw < old); /* overflow */
}

/* 归还 rq 保留量并校验 running_bw 不超过所有权总量。 */
static inline
void __sub_rq_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->this_bw;

	lockdep_assert_rq_held(rq_of_dl_rq(dl_rq));
	dl_rq->this_bw -= dl_bw;
	/* 先撤销保留量，再检查剩余 ownership 是否仍覆盖 running_bw。 */
	WARN_ON_ONCE(dl_rq->this_bw > old); /* underflow */
	if (dl_rq->this_bw > old)
		dl_rq->this_bw = 0;
	WARN_ON_ONCE(dl_rq->running_bw > dl_rq->this_bw);
}

/*
 * add_rq_bw()/sub_rq_bw() - 在实体属于普通 deadline 任务时维护 rq 保留带宽
 *
 * 业务背景：特殊 server 不应重复计入 task reservation；普通实体的 dl_bw 才需要进入
 * dl_rq->this_bw。入参：dl_se 是借用实体，dl_rq 是持 rq 锁的目标队列；两者不可空。
 * 出参/返回：无直接返回值，按实体是否 special 调用加/减 helper，不改变 ownership。
 * 注意事项：调用者必须保证实体当前属于该 rq，且在 enqueue/dequeue 或参数变更的对称
 * 路径中成对调用；漏掉任一边会让 admission 与 running 统计失去同一刻度。
 */
static inline
void add_rq_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	if (!dl_entity_is_special(dl_se))
		__add_rq_bw(dl_se->dl_bw, dl_rq);
}

static inline
void sub_rq_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	/* special server 不拥有普通 task reservation，普通实体才对称撤销 this_bw。 */
	if (!dl_entity_is_special(dl_se))
		__sub_rq_bw(dl_se->dl_bw, dl_rq);
}

/*
 * add_running_bw()/sub_running_bw() - 维护普通实体的 active utilization
 *
 * 入参为借用实体和持锁 dl_rq；无返回值。非 special 实体按 dl_bw 成对加减，special
 * server 的运行带宽由专用路径维护。下层 helper 会通知 cpufreq 并检查溢出/下溢。
 */
static inline
void add_running_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	if (!dl_entity_is_special(dl_se))
		__add_running_bw(dl_se->dl_bw, dl_rq);
}

/* 与 add_running_bw() 对称；停止 contending 后归还 active 份额，不改变 this_bw ownership。 */
static inline
void sub_running_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	if (!dl_entity_is_special(dl_se))
		__sub_running_bw(dl_se->dl_bw, dl_rq);
}

/* 修改 reservation 前取消 0-lag 延迟状态，并原子替换 rq 的 this_bw。 */
/*
 * dl_rq_change_utilization() - 在单 rq 内把实体 reservation 原子替换为 new_bw
 *
 * 入参 rq/dl_se 在 rq 锁下稳定，new_bw 与 dl_bw 同固定点单位；无返回值。若 inactive
 * timer 持有 non-contending 状态，先撤销 running_bw 并按取消结果释放 task 引用；随后
 * old→new 替换 this_bw。调用者另行更新实体字段和 root-domain total_bw。
 */
static void dl_rq_change_utilization(struct rq *rq, struct sched_dl_entity *dl_se, u64 new_bw)
{
	/* 参数替换先清掉旧 reservation，再写入新 reservation，保持 rq 统计成对。 */
	if (dl_se->dl_non_contending) {
		sub_running_bw(dl_se, &rq->dl);
		dl_se->dl_non_contending = 0;
		/* 清位先于取消 timer，运行中的回调看到清位后不会重复扣 active utilization。 */

		/*
		 * If the timer handler is currently running and the
		 * timer cannot be canceled, inactive_task_timer()
		 * will see that dl_not_contending is not set, and
		 * will not touch the rq's active utilization,
		 * so we are still safe.
		 */
		/*
		 * 如果 timer 回调正在执行而无法取消，回调会看到 dl_non_contending 已清零，
		 * 因而不会再修改 rq 的 active utilization；双方仍不会重复扣账。
		 */
		if (hrtimer_try_to_cancel(&dl_se->inactive_timer) == 1) {
			if (!dl_server(dl_se))
				put_task_struct(dl_task_of(dl_se));
		}
	}
	__sub_rq_bw(dl_se->dl_bw, &rq->dl);
	__add_rq_bw(new_bw, &rq->dl);
}

/* try_to_cancel 返回 1 才由本路径释放任务引用；回调运行中则由回调释放。 */
/*
 * cancel_dl_timer() - 取消携带 task 引用的 DL hrtimer
 *
 * 入参实体和其内嵌 timer 均为借用对象；无返回值。try_to_cancel()==1 表示本路径取得
 * 引用释放责任，回调运行中的 -1 则由回调 put；server 不持 task 引用。调用者须持使
 * timer/实体状态稳定的锁，函数不等待正在运行的回调。
 */
static __always_inline
void cancel_dl_timer(struct sched_dl_entity *dl_se, struct hrtimer *timer)
{
	/* 只有 try_to_cancel()==1 的路径负责释放 task 引用，-1 交给回调释放。 */
	/*
	 * If the timer callback was running (hrtimer_try_to_cancel == -1),
	 * it will eventually call put_task_struct().
	 */
	/* timer 回调若已在运行，最终会自行调用 put_task_struct()。 */
	if (hrtimer_try_to_cancel(timer) == 1 && !dl_server(dl_se))
		put_task_struct(dl_task_of(dl_se));
}

/* 取消 CBS replenish timer；输入/ownership/并发语义完全委托 cancel_dl_timer()。 */
static __always_inline
void cancel_replenish_timer(struct sched_dl_entity *dl_se)
{
	/* replenishment timer 与 task 生命周期的配对统一由 cancel_dl_timer() 实现。 */
	cancel_dl_timer(dl_se, &dl_se->dl_timer);
}

/* 取消 0-lag inactive timer；返回 void，引用释放责任由 cancel_dl_timer() 判定。 */
static __always_inline
void cancel_inactive_timer(struct sched_dl_entity *dl_se)
{
	/* inactive timer 同样不能由取消者和回调同时 put。 */
	cancel_dl_timer(dl_se, &dl_se->inactive_timer);
}

/* 仅离队任务可直接改带宽；在队任务由 dequeue/enqueue 路径维护一致性。 */
/*
 * dl_change_utilization() - 为当前未排队 task 替换 rq 层带宽
 *
 * 入参 p 为 rq 锁下借用 task，new_bw 为固定点 reservation；无返回值。排队 task
 * 直接返回，由 dequeue/enqueue 维护；SUGOV special 触发 WARN。实际 timer/ref 和
 * this_bw 交接由 dl_rq_change_utilization() 完成，不修改 root-domain 总量。
 */
static void dl_change_utilization(struct task_struct *p, u64 new_bw)
{
	/* 排队 task 由 dequeue/enqueue 维护带宽；这里仅修改离队 task 的 reservation。 */
	WARN_ON_ONCE(p->dl.flags & SCHED_FLAG_SUGOV);

	if (task_on_rq_queued(p))
		return;

	dl_rq_change_utilization(task_rq(p), &p->dl, new_bw);
}

static void __dl_clear_params(struct sched_dl_entity *dl_se);

/*
 * The utilization of a task cannot be immediately removed from
 * the rq active utilization (running_bw) when the task blocks.
 * Instead, we have to wait for the so called "0-lag time".
 *
 * If a task blocks before the "0-lag time", a timer (the inactive
 * timer) is armed, and running_bw is decreased when the timer
 * fires.
 *
 * If the task wakes up again before the inactive timer fires,
 * the timer is canceled, whereas if the task wakes up after the
 * inactive timer fired (and running_bw has been decreased) the
 * task's utilization has to be added to running_bw again.
 * A flag in the deadline scheduling entity (dl_non_contending)
 * is used to avoid race conditions between the inactive timer handler
 * and task wakeups.
 *
 * The following diagram shows how running_bw is updated. A task is
 * "ACTIVE" when its utilization contributes to running_bw; an
 * "ACTIVE contending" task is in the TASK_RUNNING state, while an
 * "ACTIVE non contending" task is a blocked task for which the "0-lag time"
 * has not passed yet. An "INACTIVE" task is a task for which the "0-lag"
 * time already passed, which does not contribute to running_bw anymore.
 *                              +------------------+
 *             wakeup           |    ACTIVE        |
 *          +------------------>+   contending     |
 *          | add_running_bw    |                  |
 *          |                   +----+------+------+
 *          |                        |      ^
 *          |                dequeue |      |
 * +--------+-------+                |      |
 * |                |   t >= 0-lag   |      | wakeup
 * |    INACTIVE    |<---------------+      |
 * |                | sub_running_bw |      |
 * +--------+-------+                |      |
 *          ^                        |      |
 *          |              t < 0-lag |      |
 *          |                        |      |
 *          |                        V      |
 *          |                   +----+------+------+
 *          | sub_running_bw    |    ACTIVE        |
 *          +-------------------+                  |
 *            inactive timer    |  non contending  |
 *            fired             +------------------+
 *
 * The task_non_contending() function is invoked when a task
 * blocks, and checks if the 0-lag time already passed or
 * not (in the first case, it directly updates running_bw;
 * in the second case, it arms the inactive timer).
 *
 * The task_contending() function is invoked when a task wakes
 * up, and checks if the task is still in the "ACTIVE non contending"
 * state or not (in the second case, it updates running_bw).
 */
/* 阻塞时到 0-lag 才移出 running_bw；此前由 inactive timer 延迟归还。 */
/*
 * task_non_contending() - 处理 deadline 实体从竞争状态进入阻塞状态
 *
 * 业务背景：任务阻塞后，已预留的带宽不能立刻从 rq 的 running_bw 删除；在 0-lag
 * 时刻之前删除会让 GRUB/利用率估计低估仍可能重新唤醒的任务。入参：dl_se 是 rq
 * 锁下稳定的借用实体，dl_task 表示它是否对应普通 task（server 传 false）；无
 * ownership 转移。出参/返回：无直接返回值；可能立即减少 running_bw，也可能设置
 * dl_non_contending、取得 task 引用并启动 inactive_timer，或者在 task 已死亡时
 * 撤销 reservation。注意事项：必须在 rq 锁下调用；timer 回调与唤醒路径通过
 * dl_non_contending 交接，不能把“timer 已启动”误解为带宽已经归还。
 */
static void task_non_contending(struct sched_dl_entity *dl_se, bool dl_task)
{
	struct hrtimer *timer = &dl_se->inactive_timer;
	struct rq *rq = rq_of_dl_se(dl_se);
	struct dl_rq *dl_rq = &rq->dl;
	s64 zerolag_time;

	/* runtime 为零的 PI 提升实体或 SUGOV special 实体没有普通 task 的 0-lag 账目。 */
	/*
	 * If this is a non-deadline task that has been boosted,
	 * do nothing
	 */
	/* 被 PI 提升进 DL 类但没有 DL runtime 的任务不拥有 CBS reservation，无需更新带宽。 */
	/* 被 PI 提升的非 DL task 没有自身 DL runtime，因此这里不做 active 带宽处理。 */
	if (dl_se->dl_runtime == 0)
		/* 被 PI 提升的非 deadline 任务没有自己的 CBS reservation。 */
		return;

	if (dl_entity_is_special(dl_se))
		return;

	WARN_ON(dl_se->dl_non_contending);

	zerolag_time = dl_se->deadline -
		 div64_long((dl_se->runtime * dl_se->dl_period),
			dl_se->dl_runtime);

	/*
	 * Using relative times instead of the absolute "0-lag time"
	 * allows to simplify the code
	 */
	zerolag_time -= rq_clock(rq);
	/* 后续 hrtimer 使用相对时间，因此把绝对 0-lag 时刻转换为相对 rq 时钟的剩余量。 */

	/*
	 * If the "0-lag time" already passed, decrease the active
	 * utilization now, instead of starting a timer
	 */
	if ((zerolag_time < 0) || hrtimer_active(&dl_se->inactive_timer)) {
		/* 已到 0-lag 或已有 timer：立即扣 active utilization，不再重复安装 timer。 */
		/* 已到 0-lag：立即归还 active utilization；死亡 task 还要撤销全局 reservation。 */
		if (dl_server(dl_se)) {
			/* server 没有 task reservation，直接扣其 running_bw。 */
			sub_running_bw(dl_se, dl_rq);
		} else {
			struct task_struct *p = dl_task_of(dl_se);

			if (dl_task)
				sub_running_bw(dl_se, dl_rq);

			if (!dl_task || READ_ONCE(p->__state) == TASK_DEAD) {
				/* 离队/死亡 task 不会再竞争，撤销 rq 与 root_domain 两级 reservation。 */
				struct dl_bw *dl_b = dl_bw_of(task_cpu(p));

				/* 死亡任务仍挂在原 rq 记账上；改策略任务的 rq 份额由切换路径处理。 */
				if (READ_ONCE(p->__state) == TASK_DEAD)
					sub_rq_bw(dl_se, &rq->dl);
				/* root-domain reservation 和静态参数在同一最终路径成对撤销。 */
				raw_spin_lock(&dl_b->lock);
				__dl_sub(dl_b, dl_se->dl_bw, dl_bw_cpus(task_cpu(p)));
				raw_spin_unlock(&dl_b->lock);
				__dl_clear_params(dl_se);
			}
		}

		return;
	}

	dl_se->dl_non_contending = 1;
	/* 设置交接位后持有引用，保证 inactive timer 回调期间 task_struct 不会释放。 */
	/* 未到 0-lag：先标记状态再持有 task 引用，避免 timer/唤醒竞态重复扣减。 */
	if (!dl_server(dl_se))
		get_task_struct(dl_task_of(dl_se));

	hrtimer_start(timer, ns_to_ktime(zerolag_time), HRTIMER_MODE_REL_HARD);
}

/* 唤醒与 timer 用 dl_non_contending 交接，保证 running_bw 只恢复一次。 */
/*
 * task_contending() - 处理实体重新进入可竞争状态
 *
 * 业务背景：阻塞任务可能在 inactive timer 到期前唤醒，也可能在带宽已经归还后
 * 才唤醒；两种情况对 running_bw 的操作相反。入参：dl_se 是目标借用实体，flags
 * 携带 enqueue 事件（尤其是 ENQUEUE_MIGRATED）；调用者负责 rq 锁和对象存活。出参/
 * 返回：无直接返回值；迁移时补入 this_bw，若仍处于 non-contending 则取消 timer，
 * 否则重新增加 running_bw。注意事项：先清除 dl_non_contending 再取消 timer，
 * 让正在执行的 inactive_task_timer 看到新状态后不再重复修改 active utilization。
 */
static void task_contending(struct sched_dl_entity *dl_se, int flags)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

	/* 被提升的非 DL task 没有独立 CBS budget，唤醒不改变 DL active 统计。 */
	/*
	 * If this is a non-deadline task that has been boosted,
	 * do nothing
	 */
	/* 被 PI 临时提升但没有 DL runtime 的非 DL task 不拥有 CBS 预算，此处不改 active 带宽。 */
	if (dl_se->dl_runtime == 0)
		return;

	if (flags & ENQUEUE_MIGRATED)
		/* 跨 rq 后先补回目标 rq 的保留带宽，再处理竞争状态。 */
		add_rq_bw(dl_se, dl_rq);

	if (dl_se->dl_non_contending) {
		/* timer 尚未归还：清位并取消 timer，保留原 running_bw。 */
		dl_se->dl_non_contending = 0;
		/*
		 * If the timer handler is currently running and the
		 * timer cannot be canceled, inactive_task_timer()
		 * will see that dl_not_contending is not set, and
		 * will not touch the rq's active utilization,
		 * so we are still safe.
		 */
		/* 回调已运行时会看到 non_contending 已清零，因而不会再扣 active utilization。 */
		cancel_inactive_timer(dl_se);
	} else {
		/* timer 已归还或没有延迟状态：唤醒必须重新加入 running_bw。 */
		/*
		 * Since "dl_non_contending" is not set, the
		 * task's utilization has already been removed from
		 * active utilization (either when the task blocked,
		 * when the "inactive timer" fired).
		 * So, add it back.
		 */
		/* 此位未设置说明利用率已被阻塞路径或 timer 移除，唤醒时必须重新加入。 */
		add_running_bw(dl_se, dl_rq);
	}
}

/*
 * is_leftmost() - 判断实体是否为本地 EDF 树当前最早节点
 *
 * 入参均为 rq 锁下借用对象；是树首返回非零，否则返回零，无副作用。空树或未链接
 * 实体自然返回零；该结果只在锁保护范围内有效，不能跨 dequeue/migration 缓存。
 */
static inline int is_leftmost(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	/* rb_first_cached() 读取树首；调用者用 rq 锁保证比较期间节点不变。 */
	return rb_first_cached(&dl_rq->root) == &dl_se->rb_node;
}

static void init_dl_rq_bw_ratio(struct dl_rq *dl_rq);

/* root_domain 的 DL 上限沿用全局 RT 配额；无限 RT runtime 关闭准入限制。 */
/*
 * init_dl_bw() - 初始化 root_domain 的 deadline 准入带宽状态
 *
 * 业务背景：每个 root_domain 维护共享的 deadline reservation 总量，必须先确定
 * global RT 配额对应的上限，任务参数变更才有统一分母。入参：dl_b 是内嵌于
 * root_domain 的输出对象，调用前尚未供并发路径使用；出参/返回：无直接返回值，
 * 初始化锁、bw 和 total_bw=0，不分配对象。注意事项：global_rt_runtime()==RUNTIME_INF
 * 时以 -1 表示关闭 admission control；完成初始化后对象才可发布给 CPU 路径。
 */
void init_dl_bw(struct dl_bw *dl_b)
{
	/* 先初始化锁，再根据全局 RT 配额确定准入上限，最后清空已注册总量。 */
	raw_spin_lock_init(&dl_b->lock);
	if (global_rt_runtime() == RUNTIME_INF)
		dl_b->bw = -1;
	else
		dl_b->bw = to_ratio(global_rt_period(), global_rt_runtime());
	dl_b->total_bw = 0;
}

/* 初始化 EDF 树、可迁移树和带宽计数；0 deadline 表示当前无实体。 */
/*
 * init_dl_rq() - 建立单个 rq 的 deadline 队列初始不变量
 *
 * 业务背景：EDF 主树负责选最早 deadline，pushable 树负责跨 CPU 迁移候选，三个
 * 带宽字段分别记录运行中和已保留的容量。入参：dl_rq 是 rq 内嵌、尚未被调度路径
 * 使用的输出对象；出参/返回：无直接返回值，清空树、过载状态、deadline 快照和
 * 带宽计数，并初始化比例；不转移 ownership。注意事项：必须在 rq 上线前完成，
 * 初始化后的空树约定是 `earliest_dl.curr/next == 0`。
 */
void init_dl_rq(struct dl_rq *dl_rq)
{
	/* 两棵 RB 树和 earliest 快照先置空，随后清零 running/保留带宽计数。 */
	dl_rq->root = RB_ROOT_CACHED;

	/* zero means no -deadline tasks */
	/* 数值 0 是“当前没有 deadline 实体”的哨兵，不是一个可比较的实际 deadline。 */
	dl_rq->earliest_dl.curr = dl_rq->earliest_dl.next = 0;

	dl_rq->overloaded = 0;
	dl_rq->pushable_dl_tasks_root = RB_ROOT_CACHED;

	dl_rq->running_bw = 0;
	dl_rq->this_bw = 0;
	init_dl_rq_bw_ratio(dl_rq);
}

/*
 * dl_overloaded() - 查询当前 root-domain 是否发布了任一 overloaded rq
 *
 * 入参 rq 用于定位 rd；返回 dlo_count 原子快照，非零仅提示可尝试 pull，无副作用。
 * mask 的可见性还依赖 dl_set_overload()/pull_dl_task() 的屏障配对。
 */
static inline int dl_overloaded(struct rq *rq)
{
	/* dlo_count 是 root_domain 的原子快照；非零只提示存在候选，不等于本 rq 有 task。 */
	return atomic_read(&rq->rd->dlo_count);
}

/* 先发布 mask 再增加计数，与 pull 侧读屏障配对，避免漏扫过载 rq。 */
/*
 * dl_set_overload() - 将 rq 发布为存在可迁移 deadline 任务
 *
 * 业务背景：push/pull 负载均衡通过 root_domain 的 mask 和计数寻找过载 CPU。入参：
 * rq 是持锁且可能包含 pushable task 的借用运行队列；出参/返回：在线 rq 先写入
 * dlo_mask，再经过 smp_wmb() 增加 dlo_count，无直接返回值。注意事项：写入顺序与
 * pull_dl_task() 的读屏障配对；若先增加计数，pull 侧可能看到“有过载”却看不到 mask，
 * 从而漏掉本应扫描的 CPU。离线 rq 不发布，避免把无效 CPU 放入候选集。
 */
static inline void dl_set_overload(struct rq *rq)
{
	/* offline rq 不参与迁移；在线 rq 先发布 mask，再发布计数。 */
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->dlo_mask);
	/*
	 * Must be visible before the overload count is
	 * set (as in sched_rt.c).
	 *
	 * Matched by the barrier in pull_dl_task().
	 */
	/* mask 位必须先于 overload 计数可见；pull_dl_task() 的读屏障与此配对。 */
	smp_wmb();
	atomic_inc(&rq->rd->dlo_count);
}

/*
 * dl_clear_overload() - 撤销在线 rq 的 root-domain 过载发布
 *
 * 入参 rq 由 rq 锁稳定；无返回值。在线时先递减 count 再清 mask，离线时为空操作。
 * 只能与一次成功 dl_set_overload() 成对调用，否则 count 会下溢或 mask/计数失配。
 */
static inline void dl_clear_overload(struct rq *rq)
{
	/* 清除顺序与 set 相反：先让计数不可见，再移除 mask 位。 */
	if (!rq->online)
		return;

	atomic_dec(&rq->rd->dlo_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->dlo_mask);
}

#define __node_2_pdl(node) \
	rb_entry((node), struct task_struct, pushable_dl_tasks)

/* pushable 树比较器：输入为 task 内嵌 rb_node，返回 a 的 DL 实体是否应先于 b；只读无引用变化。 */
static inline bool __pushable_less(struct rb_node *a, const struct rb_node *b)
{
	/* push 树复用 DL deadline 比较器，最左节点是最早可迁移 deadline。 */
	return dl_entity_preempt(&__node_2_pdl(a)->dl, &__node_2_pdl(b)->dl);
}

/* 查询 rq 的 pushable 树是否非空；返回布尔快照，无副作用，调用者以 rq 锁稳定树。 */
static inline int has_pushable_dl_tasks(struct rq *rq)
{
	/* 这里只查询树是否为空；候选有效性仍需 task_is_pushable() 再确认。 */
	return !RB_EMPTY_ROOT(&rq->dl.pushable_dl_tasks_root.rb_root);
}

/*
 * The list of pushable -deadline task is not a plist, like in
 * sched_rt.c, it is an rb-tree with tasks ordered by deadline.
 */
/* 按 deadline 将非当前且可迁移任务加入 push 树，并发布 rq 过载状态。 */
/*
 * enqueue_pushable_dl_task() - 把可迁移的 deadline task 插入 pushable EDF 树
 *
 * 业务背景：当前 rq 不能运行所有 deadline task 时，push 路径需要按 deadline 找到
 * 最紧迫的可迁移候选。入参：rq 是持 rq 锁的队列，p 是尚未在该树中的借用 task；
 * 出参/返回：无直接返回值，按 dl_entity_preempt() 顺序插入树，必要时更新
 * earliest_dl.next，并把 rq 发布为 overloaded；不转移 task 引用。注意事项：当前
 * 任务不应进入 pushable 树，重复插入由 WARN_ON_ONCE() 暴露；树非空与 overload
 * 标志必须保持一致，否则跨 CPU pull 会漏掉候选。
 */
static void enqueue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
	struct rb_node *leftmost;

	/* rb_add_cached() 按 deadline 插入；返回新树首时同步 next 提示。 */
	WARN_ON_ONCE(!RB_EMPTY_NODE(&p->pushable_dl_tasks));

	leftmost = rb_add_cached(&p->pushable_dl_tasks,
				 &rq->dl.pushable_dl_tasks_root,
				 __pushable_less);
	/* 只有成为新树首时才改变 earliest_dl.next；其余插入不影响迁移提示。 */
	if (leftmost)
		rq->dl.earliest_dl.next = p->dl.deadline;

	/* 第一项候选形成 rq 的 overloaded 0→1 发布边沿。 */
	if (!rq->dl.overloaded) {
		dl_set_overload(rq);
		rq->dl.overloaded = 1;
	}
}

/* 删除 push 候选并刷新 next deadline；最后一项离开时清除过载标记。 */
/*
 * dequeue_pushable_dl_task() - 从 pushable 树摘除 task 并维护过载发布状态
 *
 * 业务背景：task 离队、成为当前任务或迁移后不再是候选，必须先从树中移除，避免
 * push 路径取得失效的候选。入参：rq 是持锁队列，p 是借用 task；出参/返回：无直接
 * 返回值，更新 next deadline、清空节点哨兵；最后一个候选离开时递减计数并清 mask。
 * 注意事项：空节点是幂等快路径；rb_erase_cached() 返回新 leftmost，只有它存在时
 * 才需刷新 next，不能用旧 p 的 deadline 继续代表树首。
 */
static void dequeue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
	struct dl_rq *dl_rq = &rq->dl;
	struct rb_root_cached *root = &dl_rq->pushable_dl_tasks_root;
	struct rb_node *leftmost;

	/* 空节点是幂等快路径；已入树的 task 才能影响 next deadline 和 overload。 */
	if (RB_EMPTY_NODE(&p->pushable_dl_tasks))
		return;

	leftmost = rb_erase_cached(&p->pushable_dl_tasks, root);
	/* 删除树首时刷新 next；删除非树首时 cached leftmost 仍表示当前最早候选。 */
	if (leftmost)
		dl_rq->earliest_dl.next = __node_2_pdl(leftmost)->dl.deadline;

	RB_CLEAR_NODE(&p->pushable_dl_tasks);

	/* 最后一项离开形成 overloaded 1→0 边沿，并撤销 root-domain 提示。 */
	if (!has_pushable_dl_tasks(rq) && rq->dl.overloaded) {
		dl_clear_overload(rq);
		rq->dl.overloaded = 0;
	}
}

static int push_dl_task(struct rq *rq);

/*
 * need_pull_dl_task() - 判断 pick 前是否值得执行 DL pull
 *
 * 入参 rq/prev 为调度核心锁下借用对象；在线且前 donor 是 DL 时返回 true，否则 false。
 * 该结果只决定是否排/执行均衡，不保证远端存在候选。
 */
static inline bool need_pull_dl_task(struct rq *rq, struct task_struct *prev)
{
	/* 只有在线 rq 且当前 donor 属于 DL 类时，调度核心才需要 pull。 */
	return rq->online && dl_task(prev);
}

static DEFINE_PER_CPU(struct balance_callback, dl_push_head);
static DEFINE_PER_CPU(struct balance_callback, dl_pull_head);

static void push_dl_tasks(struct rq *);
static void pull_dl_task(struct rq *);

/*
 * deadline_queue_push_tasks() - 把本 rq 的 push 工作排入 balance callback
 *
 * 入参 rq 在 rq 锁下稳定；无返回值。无候选时快速返回，否则队列化 per-CPU callback；
 * 不在当前调用栈直接迁移，回调阶段会重新验证候选。
 */
static inline void deadline_queue_push_tasks(struct rq *rq)
{
	/* 没有候选无需排 callback；有候选则延迟到锁协议允许的 balance 阶段。 */
	if (!has_pushable_dl_tasks(rq))
		return;

	queue_balance_callback(rq, &per_cpu(dl_push_head, rq->cpu), push_dl_tasks);
}

/*
 * deadline_queue_pull_task() - 延迟请求本 rq 从过载 CPU 拉取 DL task
 *
 * 入参 rq 为借用队列；无返回值，queue_balance_callback() 合并/安排 per-CPU pull 工作。
 * 实际双 rq 锁、候选选择和失败处理均发生在 pull_dl_task()。
 */
static inline void deadline_queue_pull_task(struct rq *rq)
{
	/* pull callback 统一排到 rq balance 队列，由调度核心稍后执行。 */
	queue_balance_callback(rq, &per_cpu(dl_pull_head, rq->cpu), pull_dl_task);
}

static struct rq *find_lock_later_rq(struct task_struct *task, struct rq *rq);

/*
 * dl_task_offline_migration() - 在原 CPU 下线时把 deadline task 搬到可用 rq
 *
 * 业务背景：CPU hotplug 不能让仍有 reservation 的 task 留在即将离线的 rq；迁移还
 * 必须保持本地 this_bw/running_bw 与两个 root_domain 的 total_bw 一致。入参：rq 是
 * 当前旧队列，p 是已被调度锁协议稳定的借用 task；出参/返回：返回加锁后的目标 rq，
 * 成功后 p 的 CPU、rq 带宽和 root_domain 归属已改为目标；找不到目标时在 admission
 * 开启下发出警告，关闭 admission 时退化为任意 active CPU。注意事项：
 * find_lock_later_rq() 可能找不到可抢占目标，随后通过双 rq 锁建立迁移窗口；timer
 * 正在运行或任务被 throttle 时，要把 active/保留带宽先从旧 rq 摘下再在新 rq 恢复，
 * 否则 inactive timer 会对错误的 rq 重复记账。
 */
static struct rq *dl_task_offline_migration(struct rq *rq, struct task_struct *p)
{
	struct rq *later_rq = NULL;
	struct dl_bw *dl_b;

	later_rq = find_lock_later_rq(p, rq);
	/* 优先使用经过 deadline/迁移检查的目标；找不到时再从 active affinity 兜底。 */
	if (!later_rq) {
		int cpu;

		/*
		 * If we cannot preempt any rq, fall back to pick any
		 * online CPU:
		 */
		/* 若没有可抢占的 rq，则退化为从 task affinity 内选择任一 active CPU。 */
		cpu = cpumask_any_and(cpu_active_mask, p->cpus_ptr);
		if (cpu >= nr_cpu_ids) {
			/* affinity 无 active CPU 时，只有关闭 admission 才允许任意 active CPU 兜底。 */
			/*
			 * Failed to find any suitable CPU.
			 * The task will never come back!
			 */
			/* 找不到任何合法 CPU 意味着 task 将无法再次运行，这是应被告警的不变量破坏。 */
			WARN_ON_ONCE(dl_bandwidth_enabled());

			/*
			 * If admission control is disabled we
			 * try a little harder to let the task
			 * run.
			 */
			/* 只有 admission control 关闭时，才进一步尝试任意 active CPU 让 task 继续运行。 */
			cpu = cpumask_any(cpu_active_mask);
		}
		later_rq = cpu_rq(cpu);
		double_lock_balance(rq, later_rq);
	}

	if (p->dl.dl_non_contending || p->dl.dl_throttled) {
		/* timer/throttle 状态携带 active utilization，迁移时四项带宽必须整体搬迁。 */
		/*
		 * Inactive timer is armed (or callback is running, but
		 * waiting for us to release rq locks). In any case, when it
		 * will fire (or continue), it will see running_bw of this
		 * task migrated to later_rq (and correctly handle it).
		 */
		/* timer 回调会在取得 rq 锁后观察目标 rq，因此先整体搬迁 running_bw 与 this_bw 所有权。 */
		sub_running_bw(&p->dl, &rq->dl);
		sub_rq_bw(&p->dl, &rq->dl);

		add_rq_bw(&p->dl, &later_rq->dl);
		add_running_bw(&p->dl, &later_rq->dl);
	} else {
		sub_rq_bw(&p->dl, &rq->dl);
		add_rq_bw(&p->dl, &later_rq->dl);
	}

	/*
	 * And we finally need to fix up root_domain(s) bandwidth accounting,
	 * since p is still hanging out in the old (now moved to default) root
	 * domain.
	 */
	dl_b = &rq->rd->dl_bw;
	/* 先从旧 root_domain 扣除，再向新 root_domain 增加，锁域分别保护两个总量。 */
	raw_spin_lock(&dl_b->lock);
	__dl_sub(dl_b, p->dl.dl_bw, cpumask_weight(rq->rd->span));
	raw_spin_unlock(&dl_b->lock);

	dl_b = &later_rq->rd->dl_bw;
	raw_spin_lock(&dl_b->lock);
	__dl_add(dl_b, p->dl.dl_bw, cpumask_weight(later_rq->rd->span));
	raw_spin_unlock(&dl_b->lock);

	set_task_cpu(p, later_rq->cpu);
	/* 两个 rq 均解锁后，task 才正式拥有目标 CPU 的调度归属。 */
	double_unlock_balance(later_rq, rq);

	return later_rq;
}

static void
enqueue_dl_entity(struct sched_dl_entity *dl_se, int flags);
static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags);
static void dequeue_dl_entity(struct sched_dl_entity *dl_se, int flags);
static void wakeup_preempt_dl(struct rq *rq, struct task_struct *p, int flags);

/* 从当前 rq 时钟开始新实例，恢复完整预算；defer server 可先保持 throttle。 */
/*
 * replenish_dl_new_period() - 以当前 rq 时钟开启一个新的 CBS 实例
 *
 * 业务背景：每个 deadline 实体按 period 获得 runtime，并以绝对 deadline 参与 EDF；
 * 新实例必须从当前时间重新建立 deadline，避免沿用已经过期的时间点。入参：dl_se
 * 是 rq 锁下稳定的借用实体，rq 是其所属队列；出参/返回：无直接返回值，更新
 * deadline/runtime，defer server 在非 starvation 情况下保持 throttled，并发出
 * replenish tracepoint。注意事项：pi_of() 选择原始或 PI 代理参数；该函数只重置
 * 实例，不负责把实体重新入队，后续由 timer/enqueue 路径完成发布。
 */
static inline void replenish_dl_new_period(struct sched_dl_entity *dl_se,
					    struct rq *rq)
{
	/* for non-boosted task, pi_of(dl_se) == dl_se */
	/* 非 PI boost 实体的 pi_of(dl_se) 就是自身，因此下式直接使用本任务静态参数。 */
	dl_se->deadline = rq_clock(rq) + pi_of(dl_se)->dl_deadline;
	dl_se->runtime = pi_of(dl_se)->dl_runtime;

	/*
	 * If it is a deferred reservation, and the server
	 * is not handling an starvation case, defer it.
	 */
	/* 若 reservation 允许 defer 且当前不是 starvation 恢复，就继续延后发布 server。 */
	if (dl_se->dl_defer && !dl_se->dl_defer_running) {
		/* deferred server 暂不竞争 CPU；标志告诉 timer 后续继续延期。 */
		dl_se->dl_throttled = 1;
		dl_se->dl_defer_armed = 1;
	}
	trace_sched_dl_replenish_tp(dl_se, cpu_of(rq), dl_get_type(dl_se, rq));
}

/*
 * We are being explicitly informed that a new instance is starting,
 * and this means that:
 *  - the absolute deadline of the entity has to be placed at
 *    current time + relative deadline;
 *  - the runtime of the entity has to be set to the maximum value.
 *
 * The capability of specifying such event is useful whenever a -deadline
 * entity wants to (try to!) synchronize its behaviour with the scheduler's
 * one, and to (try to!) reconcile itself with its own scheduling
 * parameters.
 */
/* 显式开始新实例；若 replenish timer 正在接管则不与回调重复充值。 */
/*
 * setup_new_dl_entity() - 响应显式的“新实例开始”通知
 *
 * 业务背景：用户或上层 server 要求实体与调度器重新对齐时，不能在 deadline timer
 * 正在处理同一实例的窗口中重复充值。入参：dl_se 是 rq 锁下稳定的借用实体；无输出
 * 参数。出参/返回：无直接返回值；若未 throttle，则通过 replenish_dl_new_period()
 * 设置绝对 deadline 和满 runtime，若 timer 已接管则保持现状。注意事项：该入口要求
 * 非 PI boost 且当前 deadline 不在未来的状态，调用者必须持 rq 锁；普通 wall clock
 * 用于补偿 hardirq 等执行开销，函数不负责重新入队。
 */
static inline void setup_new_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

	/* 显式新实例只接受未 boost、旧 deadline 已到期的实体。 */
	WARN_ON(is_dl_boosted(dl_se));
	WARN_ON(dl_time_before(rq_clock(rq), dl_se->deadline));

	/*
	 * We are racing with the deadline timer. So, do nothing because
	 * the deadline timer handler will take care of properly recharging
	 * the runtime and postponing the deadline
	 */
	/* 与 deadline timer 竞争时保持不动，由 timer 回调负责正确充值并后移 deadline。 */
	if (dl_se->dl_throttled)
		/* timer 正在负责充值，当前路径不能重复写 runtime/deadline。 */
		return;

	/*
	 * We use the regular wall clock time to set deadlines in the
	 * future; in fact, we must consider execution overheads (time
	 * spent on hardirq context, etc.).
	 */
	/* 新 deadline 使用普通墙钟，以把 hardirq 等执行开销也计入时间推进。 */
	replenish_dl_new_period(dl_se, rq);
}

/* 后续 helper 相互递归引用，前置声明只建立类型契约，不产生状态。 */
static int start_dl_timer(struct sched_dl_entity *dl_se);
static bool dl_entity_overflow(struct sched_dl_entity *dl_se, u64 t);

/*
 * Pure Earliest Deadline First (EDF) scheduling does not deal with the
 * possibility of a entity lasting more than what it declared, and thus
 * exhausting its runtime.
 *
 * Here we are interested in making runtime overrun possible, but we do
 * not want a entity which is misbehaving to affect the scheduling of all
 * other entities.
 * Therefore, a budgeting strategy called Constant Bandwidth Server (CBS)
 * is used, in order to confine each entity within its own bandwidth.
 *
 * This function deals exactly with that, and ensures that when the runtime
 * of a entity is replenished, its deadline is also postponed. That ensures
 * the overrunning entity can't interfere with other entity in the system and
 * can't make them miss their deadlines. Reasons why this kind of overruns
 * could happen are, typically, a entity voluntarily trying to overcome its
 * runtime, or it just underestimated it during sched_setattr().
 */
/* CBS 逐 period 前推 deadline/runtime，直到预算为正且 deadline 不落后于时钟。 */
/*
 * replenish_dl_entity() - 用 CBS 规则恢复预算并推进绝对 deadline
 *
 * 业务背景：纯 EDF 允许过量执行，CBS 则把 runtime overrun 限制在 reservation 的
 * 带宽内，防止一个错误估算的任务拖累其他 deadline 任务。入参：dl_se 是持 rq 锁的
 * 借用实体，包含当前 runtime/deadline 及原始相对参数；无输出参数。出参/返回：无
 * 直接返回值；可能建立新 period、循环跳过多个耗尽 period、清除 yielded/throttled，
 * 或为 deferred server 重新设置 defer timer。注意事项：runtime<=0 时循环必须最终
 * 找到正预算；deadline 严重落后时打印一次告警并重置；defer_armed 分支会提前返回，
 * 不应被调用者误读为普通 task 已重新入队。
 */
static void replenish_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

	WARN_ON_ONCE(pi_of(dl_se)->dl_runtime <= 0);

	/*
	 * This could be the case for a !-dl task that is boosted.
	 * Just go with full inherited parameters.
	 *
	 * Or, it could be the case of a deferred reservation that
	 * was not able to consume its runtime in background and
	 * reached this point with current u > U.
	 *
	 * In both cases, set a new period.
	 */
	/* PI 提升的非 DL task 和超密度 defer reservation 都必须直接建立新 period。 */
	if (dl_se->dl_deadline == 0 ||
	    (dl_se->dl_defer_armed && dl_entity_overflow(dl_se, rq_clock(rq)))) {
		/* PI 或 deferred reservation 进入新实例：恢复代理参数，而非沿用过期预算。 */
		dl_se->deadline = rq_clock(rq) + pi_of(dl_se)->dl_deadline;
		dl_se->runtime = pi_of(dl_se)->dl_runtime;
	}

	if (dl_se->dl_yielded && dl_se->runtime > 0)
		dl_se->runtime = 0;

	/*
	 * We keep moving the deadline away until we get some
	 * available runtime for the entity. This ensures correct
	 * handling of situations where the runtime overrun is
	 * arbitrary large.
	 */
	while (dl_se->runtime <= 0) {
		/* 过量执行可能跨越多个 period；每轮只推进一个实例，直到获得可用预算。 */
		dl_se->deadline += pi_of(dl_se)->dl_period;
		dl_se->runtime += pi_of(dl_se)->dl_runtime;
	}

	/*
	 * At this point, the deadline really should be "in
	 * the future" with respect to rq->clock. If it's
	 * not, we are, for some reason, lagging too much!
	 * Anyway, after having warn userspace abut that,
	 * we still try to keep the things running by
	 * resetting the deadline and the budget of the
	 * entity.
	 */
	if (dl_time_before(dl_se->deadline, rq_clock(rq))) {
		/* 仍落后于 rq 时钟时说明延迟异常，重置到当前时间以恢复可运行性。 */
		printk_deferred_once("sched: DL replenish lagged too much\n");
		replenish_dl_new_period(dl_se, rq);
	}

	if (dl_se->dl_yielded)
		dl_se->dl_yielded = 0;
	if (dl_se->dl_throttled)
		dl_se->dl_throttled = 0;

	trace_sched_dl_replenish_tp(dl_se, cpu_of(rq), dl_get_type(dl_se, rq));

	/*
	 * If this is the replenishment of a deferred reservation,
	 * clear the flag and return.
	 */
	/* deferred reservation 的本次补充完成后只清 armed 标志并返回，不继续重新 arm。 */
	if (dl_se->dl_defer_armed) {
		/* deferred reservation 已完成一次补充；清除一次性 armed 标志后交还 timer 协议。 */
		dl_se->dl_defer_armed = 0;
		return;
	}

	/*
	 * A this point, if the deferred server is not armed, and the deadline
	 * is in the future, if it is not running already, throttle the server
	 * and arm the defer timer.
	 */
	/* defer server 未 armed、deadline 尚在未来且未运行时，应再次 throttle 并安装 defer timer。 */
	if (dl_se->dl_defer && !dl_se->dl_defer_running &&
	    dl_time_before(rq_clock(dl_se->rq), dl_se->deadline - dl_se->runtime)) {
		/* server 尚未运行且还有未来的可延期窗口：先 throttle，避免提前消耗预算。 */
		if (!is_dl_boosted(dl_se)) {
			/* deferred activation 先设置 armed/throttled，timer 才能识别本次语义。 */

			/*
			 * Set dl_se->dl_defer_armed and dl_throttled variables to
			 * inform the start_dl_timer() that this is a deferred
			 * activation.
			 */
			/* 设置 armed/throttled，明确告诉 start_dl_timer() 这是一次 deferred activation。 */
			dl_se->dl_defer_armed = 1;
			dl_se->dl_throttled = 1;
			if (!start_dl_timer(dl_se)) {
				/* 目标时刻已经过去：取消陈旧 timer 并回滚 defer 状态，允许立即入队。 */
				/*
				 * If for whatever reason (delays), a previous timer was
				 * queued but not serviced, cancel it and clean the
				 * deferrable server variables intended for start_dl_timer().
				 */
				/* 旧 timer 因延迟未服务时取消它，并回滚本次 defer 激活使用的状态位。 */
				hrtimer_try_to_cancel(&dl_se->dl_timer);
				dl_se->dl_defer_armed = 0;
				dl_se->dl_throttled = 0;
			}
		}
	}
}

/*
 * Here we check if --at time t-- an entity (which is probably being
 * [re]activated or, in general, enqueued) can use its remaining runtime
 * and its current deadline _without_ exceeding the bandwidth it is
 * assigned (function returns true if it can't). We are in fact applying
 * one of the CBS rules: when a task wakes up, if the residual runtime
 * over residual deadline fits within the allocated bandwidth, then we
 * can keep the current (absolute) deadline and residual budget without
 * disrupting the schedulability of the system. Otherwise, we should
 * refill the runtime and set the deadline a period in the future,
 * because keeping the current (absolute) deadline of the task would
 * result in breaking guarantees promised to other tasks (refer to
 * Documentation/scheduler/sched-deadline.rst for more information).
 *
 * This function returns true if:
 *
 *   runtime / (deadline - t) > dl_runtime / dl_deadline ,
 *
 * IOW we can't recycle current parameters.
 *
 * Notice that the bandwidth check is done against the deadline. For
 * task with deadline equal to period this is the same of using
 * dl_period instead of dl_deadline in the equation above.
 */
/* 交叉相乘判断剩余预算密度是否超 reservation，避免除法与精度损失。 */
/*
 * dl_entity_overflow() - 判断当前剩余 runtime/deadline 是否超出 reservation 密度
 *
 * 业务背景：实体唤醒时若保留旧绝对 deadline，会在剩余窗口内以过高密度运行并破坏
 * admission 保证。入参：dl_se 是稳定借用实体，t 是与 deadline 同刻度的 rq 时间点；
 * 出参/返回：true 表示 runtime/(deadline-t) 大于 dl_runtime/dl_deadline，调用者应
 * 重新充值/延期；false 表示可复用当前参数；无副作用。注意事项：两边都先右移
 * DL_SCALE 再相乘，把精度降到约微秒以避免 u64 溢出；这是布尔检查，不是精确预算结算。
 */
static bool dl_entity_overflow(struct sched_dl_entity *dl_se, u64 t)
{
	u64 left, right;

	/*
	 * left and right are the two sides of the equation above,
	 * after a bit of shuffling to use multiplications instead
	 * of divisions.
	 *
	 * Note that none of the time values involved in the two
	 * multiplications are absolute: dl_deadline and dl_runtime
	 * are the relative deadline and the maximum runtime of each
	 * instance, runtime is the runtime left for the last instance
	 * and (deadline - t), since t is rq->clock, is the time left
	 * to the (absolute) deadline. Even if overflowing the u64 type
	 * is very unlikely to occur in both cases, here we scale down
	 * as we want to avoid that risk at all. Scaling down by 10
	 * means that we reduce granularity to 1us. We are fine with it,
	 * since this is only a true/false check and, anyway, thinking
	 * of anything below microseconds resolution is actually fiction
	 * (but still we want to give the user that illusion >;).
	 */
	/*
	 * left/right 是上式交叉相乘后的两边，以乘法替代除法。参与乘法的量都不是两个
	 * 绝对时间：静态 deadline/runtime、剩余 runtime，以及距绝对 deadline 的剩余时间。
	 * 两边右移 DL_SCALE 把粒度降到约 1us，以规避 u64 乘法溢出；该函数只做真假判断，
	 * 因此这种精度损失可接受。
	 */
	left = (pi_of(dl_se)->dl_deadline >> DL_SCALE) * (dl_se->runtime >> DL_SCALE);
	right = ((dl_se->deadline - t) >> DL_SCALE) *
		(pi_of(dl_se)->dl_runtime >> DL_SCALE);

	return dl_time_before(right, left);
}

/*
 * Revised wakeup rule [1]: For self-suspending tasks, rather then
 * re-initializing task's runtime and deadline, the revised wakeup
 * rule adjusts the task's runtime to avoid the task to overrun its
 * density.
 *
 * Reasoning: a task may overrun the density if:
 *    runtime / (deadline - t) > dl_runtime / dl_deadline
 *
 * Therefore, runtime can be adjusted to:
 *     runtime = (dl_runtime / dl_deadline) * (deadline - t)
 *
 * In such way that runtime will be equal to the maximum density
 * the task can use without breaking any rule.
 *
 * [1] Luca Abeni, Giuseppe Lipari, and Juri Lelli. 2015. Constant
 * bandwidth server revisited. SIGBED Rev. 11, 4 (January 2015), 19-24.
 */
/*
 * update_dl_revised_wakeup() - 按剩余 laxity 限制受限 deadline 实体的 runtime
 *
 * 业务背景：对于 deadline < period 的 constrained-deadline 任务，原始 CBS 唤醒规则
 * 直接满额充值会让任务在一个 period 内获得 runtime/deadline 的过高密度。入参：
 * dl_se 是 rq 锁下的借用实体，rq 是其所属运行队列；出参/返回：无直接返回值，按
 * dl_density * (absolute deadline - rq_clock) 更新剩余 runtime。注意事项：调用前
 * deadline 应仍不早于 rq 时钟，函数只调整预算，不移动红黑树节点或启动 timer。
 */
static void
update_dl_revised_wakeup(struct sched_dl_entity *dl_se, struct rq *rq)
{
	/* 受限 deadline 任务在 deadline 前唤醒时，按剩余 laxity 重算预算而不直接满额充值。 */
	u64 laxity = dl_se->deadline - rq_clock(rq);

	/*
	 * If the task has deadline < period, and the deadline is in the past,
	 * it should already be throttled before this check.
	 *
	 * See update_dl_entity() comments for further details.
	 */
	/* constrained task 的 deadline 若已过去，本应在到达此检查前就被 throttle。 */
	WARN_ON(dl_time_before(dl_se->deadline, rq_clock(rq)));

	dl_se->runtime = (dl_se->dl_density * laxity) >> BW_SHIFT;
}

/*
 * When a deadline entity is placed in the runqueue, its runtime and deadline
 * might need to be updated. This is done by a CBS wake up rule. There are two
 * different rules: 1) the original CBS; and 2) the Revisited CBS.
 *
 * When the task is starting a new period, the Original CBS is used. In this
 * case, the runtime is replenished and a new absolute deadline is set.
 *
 * When a task is queued before the begin of the next period, using the
 * remaining runtime and deadline could make the entity to overflow, see
 * dl_entity_overflow() to find more about runtime overflow. When such case
 * is detected, the runtime and deadline need to be updated.
 *
 * If the task has an implicit deadline, i.e., deadline == period, the Original
 * CBS is applied. The runtime is replenished and a new absolute deadline is
 * set, as in the previous cases.
 *
 * However, the Original CBS does not work properly for tasks with
 * deadline < period, which are said to have a constrained deadline. By
 * applying the Original CBS, a constrained deadline task would be able to run
 * runtime/deadline in a period. With deadline < period, the task would
 * overrun the runtime/period allowed bandwidth, breaking the admission test.
 *
 * In order to prevent this misbehave, the Revisited CBS is used for
 * constrained deadline tasks when a runtime overflow is detected. In the
 * Revisited CBS, rather than replenishing & setting a new absolute deadline,
 * the remaining runtime of the task is reduced to avoid runtime overflow.
 * Please refer to the comments update_dl_revised_wakeup() function to find
 * more about the Revised CBS rule.
 */
/*
 * update_dl_entity() - 在入队前选择原始 CBS 或 revised CBS 唤醒规则
 *
 * 业务背景：实体重新入队时必须决定能否复用旧的 runtime/deadline；隐式 deadline
 * 通常可直接新建 period，而 constrained deadline 在密度溢出时只能缩减剩余预算。
 * 入参：dl_se 是 rq 锁下稳定的借用实体；出参/返回：无直接返回值，可能更新
 * runtime/deadline、清除 defer_running，或为 deferred server 设置 throttle 标志。
 * 注意事项：dl_entity_overflow() 只做密度判定；被 PI boost 的实体不能套用普通
 * revised 规则。调用者随后仍需完成 enqueue，不能把本函数当作发布点。
 */
static void update_dl_entity(struct sched_dl_entity *dl_se)
{
	struct rq *rq = rq_of_dl_se(dl_se);

	/* deadline 已过或剩余密度溢出时，必须在复用旧参数与新 period 之间做选择。 */
	if (dl_time_before(dl_se->deadline, rq_clock(rq)) ||
	    dl_entity_overflow(dl_se, rq_clock(rq))) {

		if (unlikely((!dl_is_implicit(dl_se) || dl_se->dl_defer) &&
			     !dl_time_before(dl_se->deadline, rq_clock(rq)) &&
			     !is_dl_boosted(dl_se))) {
			/* constrained/defer 且 deadline 仍在未来：缩减 runtime，避免提高长期带宽。 */
			update_dl_revised_wakeup(dl_se, rq);
			return;
		}

		/*
		 * When [4] D->A is followed by [1] A->B, dl_defer_running
		 * needs to be cleared, otherwise it will fail to properly
		 * start the zero-laxity timer.
		 */
		/* D→A 后紧接 A→B 时必须清 running，否则 zero-laxity timer 无法正确启动。 */
		dl_se->dl_defer_running = 0;
		/* 其余过期/溢出场景进入新 period，重建绝对 deadline 和满预算。 */
		replenish_dl_new_period(dl_se, rq);
	} else if (dl_server(dl_se) && dl_se->dl_defer) {
		/* 旧 deadline 仍可复用；未 running 的 defer server 只等待 zero-laxity timer。 */
		/*
		 * The server can still use its previous deadline, so check if
		 * it left the dl_defer_running state.
		 */
		/* server 仍可复用旧 deadline；若已离开 running 状态，就进入 armed/throttled 等待。 */
		if (!dl_se->dl_defer_running) {
			dl_se->dl_defer_armed = 1;
			dl_se->dl_throttled = 1;
		}
	}
}

/*
 * dl_next_period() - 计算当前实例之后的下一个 period 起点
 *
 * 入参 dl_se 为稳定借用实体；返回与 rq clock 同域的绝对时间，不修改状态。公式先由
 * absolute deadline 减 relative deadline 还原本期起点，再加 period；调用者处理回绕。
 */
static inline u64 dl_next_period(struct sched_dl_entity *dl_se)
{
	/* 由当前绝对 deadline 反推出下一实例的起点；仅计算时间，不改变实体状态。 */
	return dl_se->deadline - dl_se->dl_deadline + dl_se->dl_period;
}

/*
 * If the entity depleted all its runtime, and if we want it to sleep
 * while waiting for some new execution time to become available, we
 * set the bandwidth replenishment timer to the replenishment instant
 * and try to activate it.
 *
 * Notice that it is important for the caller to know if the timer
 * actually started or not (i.e., the replenishment instant is in
 * the future or in the past).
 */
/*
 * 实体预算耗尽且需要等待新执行时间时，把带宽 timer 安装到 replenishment 时刻。
 * 调用者必须知道 timer 是否真的启动：目标时刻在未来才成功，在过去则需立即处理。
 */
/*
 * start_dl_timer() - 在下一 replenishment/defer 时刻安装 deadline hrtimer
 *
 * 业务背景：预算耗尽后实体必须等待合法的补充时刻，不能忙等或在过去的时间点重复
 * 启动 timer。入参：dl_se 是 rq 锁下稳定的借用实体；出参/返回：返回 1 表示 timer
 * 已在队列中或成功启动，返回 0 表示目标时刻已过去、未启动；成功时普通 task 获得
 * 一个供回调释放的 task 引用。注意事项：rq clock 与 hrtimer 使用不同时间基准，
 * 必须用 ktime_get() 的 delta 校正；持 rq 锁保证检查 enqueued 与 timer 回调之间
 * 的 task 引用配对安全。
 */
static int start_dl_timer(struct sched_dl_entity *dl_se)
{
	struct hrtimer *timer = &dl_se->dl_timer;
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);
	ktime_t now, act;
	s64 delta;

	lockdep_assert_rq_held(rq);
	/* rq 锁同时稳定实体状态和 timer 引用的 get/put 决策。 */

	/*
	 * We want the timer to fire at the deadline, but considering
	 * that it is actually coming from rq->clock and not from
	 * hrtimer's time base reading.
	 *
	 * The deferred reservation will have its timer set to
	 * (deadline - runtime). At that point, the CBS rule will decide
	 * if the current deadline can be used, or if a replenishment is
	 * required to avoid add too much pressure on the system
	 * (current u > U).
	 */
	if (dl_se->dl_defer_armed) {
		/* deferred server 在零松弛时刻唤醒；普通 CBS 在下一 period 起点唤醒。 */
		WARN_ON_ONCE(!dl_se->dl_throttled);
		act = ns_to_ktime(dl_se->deadline - dl_se->runtime);
	} else {
		/* act = deadline - rel-deadline + period */
		/* 下一 period 起点 = 当前绝对 deadline - 相对 deadline + period。 */
		act = ns_to_ktime(dl_next_period(dl_se));
	}

	now = ktime_get();
	/* 把 rq clock 计算出的绝对时刻转换到 hrtimer 的单调时钟基准。 */
	delta = ktime_to_ns(now) - rq_clock(rq);
	act = ktime_add_ns(act, delta);

	/*
	 * If the expiry time already passed, e.g., because the value
	 * chosen as the deadline is too small, don't even try to
	 * start the timer in the past!
	 */
	/* expiry 已经过期时不启动“过去的 timer”，返回失败让调用者走立即恢复路径。 */
	if (ktime_us_delta(act, now) < 0)
		/* 过去的 expiry 无法提供等待语义，交给调用者立即处理下一状态。 */
		return 0;

	/*
	 * !enqueued will guarantee another callback; even if one is already in
	 * progress. This ensures a balanced {get,put}_task_struct().
	 *
	 * The race against __run_timer() clearing the enqueued state is
	 * harmless because we're holding task_rq()->lock, therefore the timer
	 * expiring after we've done the check will wait on its task_rq_lock()
	 * and observe our state.
	 */
	/*
	 * timer 尚未排队才会保证产生一个新回调，从而与 get/put_task_struct 成对。rq 锁使
	 * __run_timer() 清 enqueued 的竞态无害：过期回调会等待同一 rq 锁并看到这里的状态。
	 */
	if (!hrtimer_is_queued(timer)) {
		/* 只有首次入队才 get；已有回调或 timer 会负责同一引用的 put。 */
		if (!dl_server(dl_se))
			get_task_struct(dl_task_of(dl_se));
		hrtimer_start(timer, act, HRTIMER_MODE_ABS_HARD);
	}

	return 1;
}

/*
 * __push_dl_task() - 在本地重新入队后释放 rq 锁并触发跨 CPU push
 *
 * 业务背景：enqueue 可能令本 rq 同时拥有多个可迁移 deadline task，需要把最不合适
 * 的任务移走。入参：rq 是当前队列，rf 保存可重新 pin 的 rq 锁状态；两者均由调度
 * 主路径稳定。出参/返回：无直接返回值；若存在候选，暂时 unpin rq 锁调用 push，
 * 然后恢复锁 pin。注意事项：push_dl_task() 不依赖锁释放后的旧 rq 锁状态，因此只能
 * 在确认其调用契约后 unpin；返回后继续使用 rf 前必须 repin。
 */
static void __push_dl_task(struct rq *rq, struct rq_flags *rf)
{
	/*
	 * Queueing this task back might have overloaded rq, check if we need
	 * to kick someone away.
	 */
	/* task 重新入队可能使 rq 过载，因此检查是否需要把一个候选推走。 */
	if (has_pushable_dl_tasks(rq)) {
		/*
		 * Nothing relies on rq->lock after this, so its safe to drop
		 * rq->lock.
		 */
		/* 后续逻辑不依赖持续持有 rq 锁，所以可临时 unpin，push 后再恢复。 */
		rq_unpin_lock(rq, rf);
		push_dl_task(rq);
		rq_repin_lock(rq, rf);
	}
}

/* a defer timer will not be reset if the runtime consumed was < dl_server_min_res */
/* 若已消费 runtime 小于该最小粒度，就不再重置 defer timer，避免微小延期风暴。 */
static const u64 dl_server_min_res = 1 * NSEC_PER_MSEC;

/*
 * dl_server_timer() - 处理 fair/ext server 的 defer timer
 *
 * 业务背景：server 代表被保护的客户端预算，defer 模式允许它在 idle/短余量窗口
 * 延后竞争，避免把零碎预算变成无意义的频繁唤醒。入参：timer 是嵌入 dl_se 的借用
 * hrtimer，dl_se 是其稳定 server；出参/返回：返回 HRTIMER_RESTART 表示仅前移 timer，
 * 返回 HRTIMER_NORESTART 表示已补充并入队或停止 server。注意事项：scoped_guard
 * 自动持有/释放 rq 锁；先让 donor sched_class 更新 pending runtime，再决定 stop、
 * forward 或 enqueue，避免 server 预算落后于实际执行。
 */
static enum hrtimer_restart dl_server_timer(struct hrtimer *timer, struct sched_dl_entity *dl_se)
{
	struct rq *rq = rq_of_dl_se(dl_se);
	u64 fw;

	scoped_guard (rq_lock, rq) {
		struct rq_flags *rf = &scope.rf;

		/* timer 过期后状态可能已被 stop/参数更新清理，先做幂等性复核。 */
		if (!dl_se->dl_throttled || !dl_se->dl_runtime)
			return HRTIMER_NORESTART;

		sched_clock_tick();
		update_rq_clock(rq);

		/*
		 * Make sure current has propagated its pending runtime into
		 * any relevant server through calling dl_server_update() and
		 * friends.
		 */
		rq->donor->sched_class->update_curr(rq);
		/* donor 的 pending 执行时间已传播，下面读取 server runtime 才是最新值。 */

		if (dl_se->dl_defer_idle) {
			/* idle-wait 到期表示无需保护客户端，停止 server 回到 init 状态。 */
			dl_server_stop(dl_se);
			return HRTIMER_NORESTART;
		}

		if (dl_se->dl_defer_armed) {
			/* zero-laxity 前仍有大于最小粒度的后台窗口时，只前移 timer，不发布 server。 */
			/*
			 * First check if the server could consume runtime in background.
			 * If so, it is possible to push the defer timer for this amount
			 * of time. The dl_server_min_res serves as a limit to avoid
			 * forwarding the timer for a too small amount of time.
			 */
			/* server 可在后台消费预算时可前移 defer timer，但最小粒度防止过短反复延期。 */
			if (dl_time_before(rq_clock(dl_se->rq),
					   (dl_se->deadline - dl_se->runtime - dl_server_min_res))) {

				/* reset the defer timer */
				/* 用 deadline、当前 rq 时钟和剩余 runtime 重新计算 defer 延期量。 */
				fw = dl_se->deadline - rq_clock(dl_se->rq) - dl_se->runtime;

				hrtimer_forward_now(timer, ns_to_ktime(fw));
				return HRTIMER_RESTART;
			}

			dl_se->dl_defer_running = 1;
			/* 不能再延期时进入 running 状态，随后 replenish 并加入 EDF 树。 */
		}

		enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH);
		/* enqueue 是 server 重新对调度器可见的发布点；之后检查抢占与跨 CPU push。 */

		if (!dl_task(dl_se->rq->curr) || dl_entity_preempt(dl_se, &dl_se->rq->curr->dl))
			resched_curr(rq);

		__push_dl_task(rq, rf);
	}

	return HRTIMER_NORESTART;
}

/*
 * This is the bandwidth enforcement timer callback. If here, we know
 * a task is not on its dl_rq, since the fact that the timer was running
 * means the task is throttled and needs a runtime replenishment.
 *
 * However, what we actually do depends on the fact the task is active,
 * (it is on its rq) or has been removed from there by a call to
 * dequeue_task_dl(). In the former case we must issue the runtime
 * replenishment and add the task back to the dl_rq; in the latter, we just
 * do nothing but clearing dl_throttled, so that runtime and deadline
 * updating (and the queueing back to dl_rq) will be done by the
 * next call to enqueue_task_dl().
 */
/*
 * dl_task_timer() - 处理普通 deadline task 的预算补充回调
 *
 * 业务背景：CBS timer 到期后，仍在 rq 上的 task 要恢复预算并重新进入 EDF 树；已
 * dequeue 或已改策略的 task 只清理/释放 timer 关联状态，不能凭回调强行入队。入参：
 * timer 是嵌入 dl_se 的借用 hrtimer；出参/返回：server 转发其专用回调，普通 task
 * 返回 HRTIMER_NORESTART，并在必要时迁移、replenish、enqueue 和触发 reschedule。
 * 注意事项：先 task_rq_lock 稳定 task 与 rq；所有早退路径都必须解锁，最终 put_task_struct
 * 后不得再访问 p 或其 timer，因为最后一个引用可能释放整个 task_struct。
 */
static enum hrtimer_restart dl_task_timer(struct hrtimer *timer)
{
	struct sched_dl_entity *dl_se = container_of(timer,
						     struct sched_dl_entity,
						     dl_timer);
	struct task_struct *p;
	struct rq_flags rf;
	struct rq *rq;

	if (dl_server(dl_se))
		/* server 的 defer 语义不同，统一交给 server 回调处理。 */
		return dl_server_timer(timer, dl_se);

	p = dl_task_of(dl_se);
	rq = task_rq_lock(p, &rf);
	/* task_rq_lock 稳定策略、rq 归属与 throttle 状态，随后才能判定回调是否仍有效。 */

	/*
	 * The task might have changed its scheduling policy to something
	 * different than SCHED_DEADLINE (through switched_from_dl()).
	 */
	if (!dl_task(p))
		/* switched_from_dl 已撤销本类 ownership，回调只负责释放引用。 */
		goto unlock;

	/*
	 * The task might have been boosted by someone else and might be in the
	 * boosting/deboosting path, its not throttled.
	 */
	if (is_dl_boosted(dl_se))
		/* PI boost 路径拥有当前调度状态，普通 replenishment timer 不得干预。 */
		goto unlock;

	/*
	 * Spurious timer due to start_dl_timer() race; or we already received
	 * a replenishment from rt_mutex_setprio().
	 */
	if (!dl_se->dl_throttled)
		/* throttle 已由其他路径清除，说明本回调过时，无需重复充值。 */
		goto unlock;

	sched_clock_tick();
	update_rq_clock(rq);

	/*
	 * If the throttle happened during sched-out; like:
	 *
	 *   schedule()
	 *     deactivate_task()
	 *       dequeue_task_dl()
	 *         update_curr_dl()
	 *           start_dl_timer()
	 *         __dequeue_task_dl()
	 *     prev->on_rq = 0;
	 *
	 * We can be both throttled and !queued. Replenish the counter
	 * but do not enqueue -- wait for our wakeup to do that.
	 */
	if (!task_on_rq_queued(p)) {
		/* task 已 sched-out：只补充预算，等下一次 wakeup 按正常入队协议发布。 */
		replenish_dl_entity(dl_se);
		goto unlock;
	}

	if (unlikely(!rq->online)) {
		/* 原 rq 已下线：先迁移带宽与 CPU 归属，再在新 rq 继续补充入队。 */
		/*
		 * If the runqueue is no longer available, migrate the
		 * task elsewhere. This necessarily changes rq.
		 */
		/* 原 rq 不可用时迁移到其他 CPU；该操作必然替换后续使用的 rq。 */
		lockdep_unpin_lock(__rq_lockp(rq), rf.cookie);
		rq = dl_task_offline_migration(rq, p);
		rf.cookie = lockdep_pin_lock(__rq_lockp(rq));
		update_rq_clock(rq);

		/*
		 * Now that the task has been migrated to the new RQ and we
		 * have that locked, proceed as normal and enqueue the task
		 * there.
		 */
		/* task 已迁到并锁住新 rq，随后按正常路径在该 rq 重新入队。 */
	}

	enqueue_task_dl(rq, p, ENQUEUE_REPLENISH);
	/* 发布回 EDF 队列后重新比较当前任务，并把剩余 push 工作排入 balance callback。 */
	if (dl_task(rq->donor))
		wakeup_preempt_dl(rq, p, 0);
	else
		resched_curr(rq);

	__push_dl_task(rq, &rf);

unlock:
	task_rq_unlock(rq, p, &rf);

	/*
	 * This can free the task_struct, including this hrtimer, do not touch
	 * anything related to that after this.
	 */
	/* put 可能释放 task_struct 及其中的 hrtimer；此行之后绝不能再访问相关对象。 */
	put_task_struct(p);

	return HRTIMER_NORESTART;
}

/*
 * init_dl_task_timer() - 初始化实体的 CBS replenishment hrtimer
 *
 * 业务背景：每个 deadline 实体需要一个硬实时模式的单调时钟回调来恢复 runtime。
 * 入参：dl_se 是尚未发布或正在初始化的借用实体；出参/返回：无直接返回值，把 timer
 * 绑定到 dl_task_timer()，不取得 task 引用。注意事项：timer 的引用由 start/cancel
 * 路径管理；本函数只建立 timer 对象，不能替代后续的参数初始化和 rq 发布。
 */
static void init_dl_task_timer(struct sched_dl_entity *dl_se)
{
	struct hrtimer *timer = &dl_se->dl_timer;

	hrtimer_setup(timer, dl_task_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
}

/*
 * During the activation, CBS checks if it can reuse the current task's
 * runtime and period. If the deadline of the task is in the past, CBS
 * cannot use the runtime, and so it replenishes the task. This rule
 * works fine for implicit deadline tasks (deadline == period), and the
 * CBS was designed for implicit deadline tasks. However, a task with
 * constrained deadline (deadline < period) might be awakened after the
 * deadline, but before the next period. In this case, replenishing the
 * task would allow it to run for runtime / deadline. As in this case
 * deadline < period, CBS enables a task to run for more than the
 * runtime / period. In a very loaded system, this can cause a domino
 * effect, making other tasks miss their deadlines.
 *
 * To avoid this problem, in the activation of a constrained deadline
 * task after the deadline but before the next period, throttle the
 * task and set the replenishing timer to the begin of the next period,
 * unless it is boosted.
 */
/*
 * dl_check_constrained_dl() - 约束 deadline 任务在 deadline 与下一 period 之间限流
 *
 * 业务背景：constrained deadline 任务若在 deadline 后、下一 period 前直接获得满额
 * runtime，会暂时以 runtime/deadline 超过 runtime/period 的密度运行。入参：dl_se
 * 是 rq 锁下借用实体；出参/返回：无直接返回值，必要时启动下一 period timer、发
 * throttle tracepoint、置 dl_throttled 并清零 runtime。注意事项：PI boost 时不适用
 * 该限制；timer 无法启动时保持原状态，由调用者继续处理，不能把 throttle 标志提前
 * 置上造成无 timer 可恢复。
 */
static inline void dl_check_constrained_dl(struct sched_dl_entity *dl_se)
{
	struct rq *rq = rq_of_dl_se(dl_se);

	/* 只处理旧 deadline 已过、下一 period 尚未开始的受限窗口。 */
	if (dl_time_before(dl_se->deadline, rq_clock(rq)) &&
	    dl_time_before(rq_clock(rq), dl_next_period(dl_se))) {
		if (unlikely(is_dl_boosted(dl_se) || !start_dl_timer(dl_se)))
			/* boost 或 timer 时刻已过时保持可运行，由上层 PI/CBS 路径继续处理。 */
			return;
		/* timer 已可靠安装后才发布 throttle，保证每次限流都有恢复者。 */
		trace_sched_dl_throttle_tp(dl_se, cpu_of(rq), dl_get_type(dl_se, rq));
		dl_se->dl_throttled = 1;
		if (dl_se->runtime > 0)
			dl_se->runtime = 0;
	}
}

static
/* 输入为借用实体；runtime<=0 返回非零，否则零，无副作用，是统一 throttle 判定。 */
int dl_runtime_exceeded(struct sched_dl_entity *dl_se)
{
	return (dl_se->runtime <= 0);
}

/*
 * This function implements the GRUB accounting rule. According to the
 * GRUB reclaiming algorithm, the runtime is not decreased as "dq = -dt",
 * but as "dq = -(max{u, (Umax - Uinact - Uextra)} / Umax) dt",
 * where u is the utilization of the task, Umax is the maximum reclaimable
 * utilization, Uinact is the (per-runqueue) inactive utilization, computed
 * as the difference between the "total runqueue utilization" and the
 * "runqueue active utilization", and Uextra is the (per runqueue) extra
 * reclaimable utilization.
 * Since rq->dl.running_bw and rq->dl.this_bw contain utilizations multiplied
 * by 2^BW_SHIFT, the result has to be shifted right by BW_SHIFT.
 * Since rq->dl.bw_ratio contains 1 / Umax multiplied by 2^RATIO_SHIFT, dl_bw
 * is multiplied by rq->dl.bw_ratio and shifted right by RATIO_SHIFT.
 * Since delta is a 64 bit variable, to have an overflow its value should be
 * larger than 2^(64 - 20 - 8), which is more than 64 seconds. So, overflow is
 * not an issue here.
 */
/*
 * grub_reclaim() - 按 GRUB 规则把执行时间折算为应扣除的 runtime
 *
 * 业务背景：启用 reclaim 的任务可以消费其他任务暂时未使用的 spare bandwidth，
 * 但扣账速度必须受 rq 当前 active/inactive/extra utilization 约束。入参：delta 是
 * 本次执行的时间增量，rq 是持锁运行队列，dl_se 是当前借用实体；时间和带宽均使用
 * 调度器定点刻度。出参/返回：返回经 GRUB 利用率比例折算后的 runtime 消耗，无对象
 * 或 ownership 副作用。注意事项：this_bw、running_bw、extra_bw 由 rq 锁保护；先
 * 比较而不直接计算可能为负的 u_max-u_inact-u_extra，避免无符号下溢。
 */
static u64 grub_reclaim(u64 delta, struct rq *rq, struct sched_dl_entity *dl_se)
{
	u64 u_act;
	u64 u_inact = rq->dl.this_bw - rq->dl.running_bw; /* Utot - Uact */
	/* u_inact 是已保留但当前不竞争的利用率，rq 锁保证两个源字段来自同一快照。 */

	/*
	 * Instead of computing max{u, (u_max - u_inact - u_extra)}, we
	 * compare u_inact + u_extra with u_max - u, because u_inact + u_extra
	 * can be larger than u_max. So, u_max - u_inact - u_extra would be
	 * negative leading to wrong results.
	 */
	if (u_inact + rq->dl.extra_bw > rq->dl.max_bw - dl_se->dl_bw)
		/* spare 不足时至少按实体自身利用率扣账，不能回收不存在的容量。 */
		u_act = dl_se->dl_bw;
	else
		u_act = rq->dl.max_bw - u_inact - rq->dl.extra_bw;

	u_act = (u_act * rq->dl.bw_ratio) >> RATIO_SHIFT;
	/* 先归一化到 Umax 比例，再把时间 delta 缩放为实际 runtime 消耗。 */
	return (delta * u_act) >> BW_SHIFT;
}

/* reclaim 实体用 GRUB-PA，普通实体按频率和 CPU capacity 缩放预算消耗。 */
/*
 * dl_scaled_delta_exec() - 将 wall-clock 执行时间换算为 deadline 预算消耗
 *
 * 业务背景：CPU 频率和异构 capacity 会改变同一 wall-clock 时间的实际计算量；
 * GRUB-PA 任务还应把可回收 spare bandwidth 用于频率缩放。入参：rq 是持锁运行队列，
 * dl_se 是当前借用实体，delta_exec 是本次执行的有符号时间增量；出参/返回：返回
 * 按 GRUB 或频率/capacity 定点比例缩放后的时间，不修改实体。注意事项：reclaim
 * 分支依赖 rq 的带宽统计，普通分支依赖当前 CPU 的 freq/cpu capacity；调用者随后
 * 才会用结果扣减 runtime，不能在此函数内假定已发生 throttle。
 */
s64 dl_scaled_delta_exec(struct rq *rq, struct sched_dl_entity *dl_se, s64 delta_exec)
{
	s64 scaled_delta_exec;

	/*
	 * For tasks that participate in GRUB, we implement GRUB-PA: the
	 * spare reclaimed bandwidth is used to clock down frequency.
	 *
	 * For the others, we still need to scale reservation parameters
	 * according to current frequency and CPU maximum capacity.
	 */
	if (unlikely(dl_se->flags & SCHED_FLAG_RECLAIM)) {
		/* reclaim task 使用 rq 当前 spare bandwidth 的 GRUB 比例。 */
		scaled_delta_exec = grub_reclaim(delta_exec, rq, dl_se);
	} else {
		/* 普通 reservation 只按当前频率和 CPU 最大 capacity 修正执行量。 */
		int cpu = cpu_of(rq);
		unsigned long scale_freq = arch_scale_freq_capacity(cpu);
		unsigned long scale_cpu = arch_scale_cpu_capacity(cpu);

		scaled_delta_exec = cap_scale(delta_exec, scale_freq);
		scaled_delta_exec = cap_scale(scaled_delta_exec, scale_cpu);
	}

	return scaled_delta_exec;
}

static inline void
update_stats_dequeue_dl(struct dl_rq *dl_rq, struct sched_dl_entity *dl_se, int flags);

/* 扣减当前实体预算；超限时离队并启动补充 timer，yield 也走同一 throttle 出口。 */
/*
 * update_curr_dl_se() - 把当前执行时间扣入 deadline 实体的 CBS 预算
 *
 * 业务背景：调度类在 tick、切换和 server 记账时必须把 wall-clock 执行时间转换为
 * runtime；预算耗尽或主动 yield 后，实体必须离开 EDF 队列并等待合法 replenishment。
 * 入参：rq 是持锁当前运行队列，dl_se 是当前执行的借用实体，delta_exec 是本次执行
 * 的 ns 增量；出参/返回：无直接返回值，可能减少 runtime、更新 server 状态、置
 * throttled、dequeue、启动 timer、重新 enqueue 或触发 reschedule。注意事项：
 * special entity/server 与普通 task 的记账规则不同；普通 task 被 throttle 后最后的
 * task 引用由 timer 回调释放，函数不能在 put 后继续访问 task。CONFIG_RT_GROUP_SCHED
 * 下还需在共享 RT 配额锁下同步 rt_time。
 */
static void update_curr_dl_se(struct rq *rq, struct sched_dl_entity *dl_se, s64 delta_exec)
{
	bool idle = idle_rq(rq);
	s64 scaled_delta_exec;

	if (unlikely(delta_exec <= 0)) {
		/* 无正执行时间通常直接返回；yielded 仍必须走 throttle 出口以清理队列。 */
		if (unlikely(dl_se->dl_yielded))
			goto throttle;
		return;
	}

	if (dl_server(dl_se) && dl_se->dl_throttled && !dl_se->dl_defer)
		/* 非 defer server 被 throttle 后不能继续消费 CBS 预算。 */
		return;

	if (dl_entity_is_special(dl_se))
		/* special entity 的 budget 由其专用协议维护，跳过普通 CBS 扣账。 */
		return;

	scaled_delta_exec = delta_exec;
	if (!dl_server(dl_se))
		scaled_delta_exec = dl_scaled_delta_exec(rq, dl_se, delta_exec);

	dl_se->runtime -= scaled_delta_exec;
	/* 从这里开始 runtime 已反映本次执行；后续分支只决定 server 生命周期或 throttle。 */

	if (dl_se->dl_defer_idle && !idle)
		/* client 恢复执行后退出 idle-wait，避免 timer 把活跃 server 停止。 */
		dl_se->dl_defer_idle = 0;

	/*
	 * The DL server can consume its runtime while throttled (not
	 * queued / running as regular CFS).
	 *
	 * If the server consumes its entire runtime in this state. The server
	 * is not required for the current period. Thus, reset the server by
	 * starting a new period, pushing the activation.
	 */
	/* throttle 中的 DL server 仍可由常规 CFS 工作消耗预算；耗尽说明本期不再需要它，应前推激活。 */
	if (dl_se->dl_defer && dl_se->dl_throttled && dl_runtime_exceeded(dl_se)) {
		/* defer server 可在 throttle 状态由 client-class 消费；耗尽后重置实例并重新设 timer。 */
		/*
		 * Non-servers would never get time accounted while throttled.
		 */
		/* 普通实体 throttle 时不会被执行记账；到达这里的对象必须是 server。 */
		WARN_ON_ONCE(!dl_server(dl_se));

		/*
		 * While the server is marked idle, do not push out the
		 * activation further, instead wait for the period timer
		 * to lapse and stop the server.
		 */
		/* server 标记 idle 时不再推迟 activation，而等待当前 period timer 到期并停止。 */
		if (dl_se->dl_defer_idle && idle) {
			/* idle-wait 不再前推 activation，只把负 runtime 钳为零等待当前 timer。 */
			/*
			 * The timer is at the zero-laxity point, this means
			 * dl_server_stop() / dl_server_start() can happen
			 * while now < deadline. This means update_dl_entity()
			 * will not replenish. Additionally start_dl_timer()
			 * will be set for 'deadline - runtime'. Negative
			 * runtime will not do.
			 */
			/* timer 位于 zero-laxity；负 runtime 会使下一 deadline-runtime 失效，故先钳零。 */
			dl_se->runtime = 0;
			return;
		}

		/*
		 * If the server was previously activated - the starving condition
		 * took place, it this point it went away because the fair scheduler
		 * was able to get runtime in background. So return to the initial
		 * state.
		 */
		/* starvation 曾激活 server，但后台已获得执行时间，现应返回初始 defer 状态。 */
		dl_se->dl_defer_running = 0;
		/* starvation 已消失，取消旧 timer 并用新 period 回到 zero-laxity wait。 */

		hrtimer_try_to_cancel(&dl_se->dl_timer);

		replenish_dl_new_period(dl_se, dl_se->rq);

		if (idle)
			dl_se->dl_defer_idle = 1;

		/*
		 * Not being able to start the timer seems problematic. If it could not
		 * be started for whatever reason, we need to "unthrottle" the DL server
		 * and queue right away. Otherwise nothing might queue it. That's similar
		 * to what enqueue_dl_entity() does on start_dl_timer==0. For now, just warn.
		 */
		/* timer 启动失败会让 server 永久无人入队；当前实现先 WARN，语义与 enqueue 失败处理一致。 */
		WARN_ON_ONCE(!start_dl_timer(dl_se));

		return;
	}

throttle:
	if (dl_runtime_exceeded(dl_se) || dl_se->dl_yielded) {
		/* 普通耗尽/yield 的发布边界：先记录 throttle，再摘出 EDF 树，最后安排恢复。 */
		trace_sched_dl_throttle_tp(dl_se, cpu_of(rq), dl_get_type(dl_se, rq));
		dl_se->dl_throttled = 1;

		/* If requested, inform the user about runtime overruns. */
		/* 用户设置 SCHED_FLAG_DL_OVERRUN 时，耗尽预算需锁存可观察的 overrun 通知。 */
		if (dl_runtime_exceeded(dl_se) &&
		    (dl_se->flags & SCHED_FLAG_DL_OVERRUN))
			/* 用户请求 overrun 通知时锁存标志，由用户接口读取/清理。 */
			dl_se->dl_overrun = 1;

		dequeue_dl_entity(dl_se, 0);
		/* dequeue 后实体不再由 EDF 选择；普通 task 的统计和 push 候选也同步撤销。 */
		if (!dl_server(dl_se)) {
			update_stats_dequeue_dl(&rq->dl, dl_se, 0);
			dequeue_pushable_dl_task(rq, dl_task_of(dl_se));
		}

		if (unlikely(is_dl_boosted(dl_se) || !start_dl_timer(dl_se))) {
			/* PI 或过去的 timer 时刻不能等待，立即补充并重新发布实体。 */
			if (dl_server(dl_se)) {
				/* defer server 进入新 period 后仍由 timer 驱动；普通 server 可直接回树。 */
				if (dl_se->dl_defer) {
					replenish_dl_new_period(dl_se, rq);
					start_dl_timer(dl_se);
				} else {
					enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH);
				}
			} else {
				/* task wrapper 同步恢复统计与 pushable 状态，不能只插 entity。 */
				enqueue_task_dl(rq, dl_task_of(dl_se), ENQUEUE_REPLENISH);
			}
		}

		/* 当前耗尽实体已不再最早时强制重新选择 donor。 */
		if (!is_leftmost(dl_se, &rq->dl))
			resched_curr(rq);
	} else {
		trace_sched_dl_update_tp(dl_se, cpu_of(rq), dl_get_type(dl_se, rq));
	}

	/*
	 * The dl_server does not account for real-time workload because it
	 * is running fair work.
	 */
	if (dl_se->dl_server)
		/* server 承载 fair 工作，不应再次消耗共享 RT task 配额。 */
		return;

#ifdef CONFIG_RT_GROUP_SCHED
	/*
	 * Because -- for now -- we share the rt bandwidth, we need to
	 * account our runtime there too, otherwise actual rt tasks
	 * would be able to exceed the shared quota.
	 *
	 * Account to the root rt group for now.
	 *
	 * The solution we're working towards is having the RT groups scheduled
	 * using deadline servers -- however there's a few nasties to figure
	 * out before that can happen.
	 */
	if (rt_bandwidth_enabled()) {
		/* 与 RT 类共享全局配额时，在 rt_runtime_lock 下同步累计原始执行时间。 */
		struct rt_rq *rt_rq = &rq->rt;

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * We'll let actual RT tasks worry about the overflow here, we
		 * have our own CBS to keep us inline; only account when RT
		 * bandwidth is relevant.
		 */
		/* 真正的 RT task 负责共享配额溢出；DL 有 CBS 自限，只在 RT 带宽启用时累计时间。 */
		if (sched_rt_bandwidth_account(rt_rq))
			rt_rq->rt_time += delta_exec;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
#endif /* CONFIG_RT_GROUP_SCHED */
}

/*
 * In the non-defer mode, the idle time is not accounted, as the
 * server provides a guarantee.
 *
 * If the dl_server is in defer mode, the idle time is also considered as
 * time available for the dl_server, avoiding a penalty for the rt
 * scheduler that did not consumed that time.
 */
/* defer server 把 idle 时间视为可用窗口，避免无工作时错误消耗实时保证。 */
/*
 * dl_server_update_idle() - 在 defer server 的 idle 窗口中推进其预算
 *
 * 业务背景：defer server 代表 fair/其他客户端，server 没有普通 task 的直接竞争者；
 * idle 时间仍可用于推进 server 的 CBS 记账，否则会把未消费的保证错误地当成损失。
 * 入参：dl_se 是 server 的借用实体，delta_exec 是 idle 时间增量；出参/返回：无直接
 * 返回值，仅在 server active、runtime 非零且 defer 开启时转发到 update_curr_dl_se()。
 * 注意事项：调用者负责 rq 锁和时间增量有效性；非 defer 或未 active 时必须保持预算
 * 不变，避免误扣普通 server 的实时保证。
 */
void dl_server_update_idle(struct sched_dl_entity *dl_se, s64 delta_exec)
{
	if (dl_se->dl_server_active && dl_se->dl_runtime && dl_se->dl_defer)
		update_curr_dl_se(dl_se->rq, dl_se, delta_exec);
}

/* client class 运行时同步消耗其保护 server 的 CBS 预算。 */
/*
 * dl_server_update() - 用客户端类实际执行时间消耗保护 server 的预算
 *
 * 业务背景：server 的 runtime 是 fair/ext 客户端获得实时服务的上限，客户端执行时
 * 必须同步扣减，否则 server 会虚假地拥有未使用预算。入参：dl_se 是 active server
 * 的借用实体，delta_exec 是客户端执行增量；出参/返回：无直接返回值，runtime 非零
 * 时调用统一记账路径，可能触发 replenish/throttle。注意事项：`dl_server_active` 和
 * `dl_runtime` 是门控状态；注释中的 0 runtime 表示 fair server 被禁用，不是无限预算。
 */
void dl_server_update(struct sched_dl_entity *dl_se, s64 delta_exec)
{
	/* 0 runtime = fair server disabled */
	/* runtime 为零表示 fair server 被禁用，因此 active 标志本身不足以触发扣账。 */
	if (dl_se->dl_server_active && dl_se->dl_runtime)
		update_curr_dl_se(dl_se->rq, dl_se, delta_exec);
}

/*
 * dl_server && dl_defer:
 *
 *                                        6
 *                            +--------------------+
 *                            v                    |
 *     +-------------+  4   +-----------+  5     +------------------+
 * +-> |   A:init    | <--- | D:running | -----> | E:replenish-wait |
 * |   +-------------+      +-----------+        +------------------+
 * |     |         |    1     ^    ^               |
 * |     | 1       +----------+    | 3             |
 * |     v                         |               |
 * |   +--------------------------------+   2      |
 * |   |                                | ----+    |
 * | 8 |       B:zero_laxity-wait       |     |    |
 * |   |                                | <---+    |
 * |   +--------------------------------+          |
 * |     |              ^         ^       2        |
 * |     | 7            | 2, 1    +----------------+
 * |     v              |
 * |   +-------------+  |
 * +-- | C:idle-wait | -+
 *     +-------------+
 *       ^ 7       |
 *       +---------+
 *
 *
 * [A] - init
 *   dl_server_active = 0
 *   dl_throttled = 0
 *   dl_defer_armed = 0
 *   dl_defer_running = 0/1
 *   dl_defer_idle = 0
 *
 * [B] - zero_laxity-wait
 *   dl_server_active = 1
 *   dl_throttled = 1
 *   dl_defer_armed = 1
 *   dl_defer_running = 0
 *   dl_defer_idle = 0
 *
 * [C] - idle-wait
 *   dl_server_active = 1
 *   dl_throttled = 1
 *   dl_defer_armed = 1
 *   dl_defer_running = 0
 *   dl_defer_idle = 1
 *
 * [D] - running
 *   dl_server_active = 1
 *   dl_throttled = 0
 *   dl_defer_armed = 0
 *   dl_defer_running = 1
 *   dl_defer_idle = 0
 *
 * [E] - replenish-wait
 *   dl_server_active = 1
 *   dl_throttled = 1
 *   dl_defer_armed = 0
 *   dl_defer_running = 1
 *   dl_defer_idle = 0
 *
 *
 * [1] A->B, A->D, C->B
 * dl_server_start()
 *   dl_defer_idle = 0;
 *   if (dl_server_active)
 *     return; // [B]
 *   dl_server_active = 1;
 *   enqueue_dl_entity()
 *     update_dl_entity(WAKEUP)
 *       if (dl_time_before() || dl_entity_overflow())
 *         dl_defer_running = 0;
 *         replenish_dl_new_period();
 *           // fwd period
 *           dl_throttled = 1;
 *           dl_defer_armed = 1;
 *       if (!dl_defer_running)
 *         dl_defer_armed = 1;
 *         dl_throttled = 1;
 *     if (dl_throttled && start_dl_timer())
 *       return; // [B]
 *     __enqueue_dl_entity();
 *     // [D]
 *
 * // deplete server runtime from client-class
 * [2] B->B, C->B, E->B
 * dl_server_update()
 *   update_curr_dl_se() // idle = false
 *     if (dl_defer_idle)
 *       dl_defer_idle = 0;
 *     if (dl_defer && dl_throttled && dl_runtime_exceeded())
 *       dl_defer_running = 0;
 *       hrtimer_try_to_cancel();   // stop timer
 *       replenish_dl_new_period()
 *         // fwd period
 *         dl_throttled = 1;
 *         dl_defer_armed = 1;
 *       start_dl_timer();        // restart timer
 *       // [B]
 *
 * // timer actually fires means we have runtime
 * [3] B->D
 * dl_server_timer()
 *   if (dl_defer_armed)
 *     dl_defer_running = 1;
 *   enqueue_dl_entity(REPLENISH)
 *     replenish_dl_entity()
 *       // fwd period
 *       if (dl_throttled)
 *         dl_throttled = 0;
 *       if (dl_defer_armed)
 *         dl_defer_armed = 0;
 *     __enqueue_dl_entity();
 *     // [D]
 *
 * // schedule server
 * [4] D->A
 * pick_task_dl()
 *   p = server_pick_task();
 *   if (!p)
 *     dl_server_stop()
 *       dequeue_dl_entity();
 *       hrtimer_try_to_cancel();
 *       dl_defer_armed = 0;
 *       dl_throttled = 0;
 *       dl_server_active = 0;
 *       // [A]
 *   return p;
 *
 * // server running
 * [5] D->E
 * update_curr_dl_se()
 *   if (dl_runtime_exceeded())
 *     dl_throttled = 1;
 *     dequeue_dl_entity();
 *     start_dl_timer();
 *     // [E]
 *
 * // server replenished
 * [6] E->D
 * dl_server_timer()
 *   enqueue_dl_entity(REPLENISH)
 *     replenish_dl_entity()
 *       fwd-period
 *       if (dl_throttled)
 *         dl_throttled = 0;
 *     __enqueue_dl_entity();
 *     // [D]
 *
 * // deplete server runtime from idle
 * [7] B->C, C->C
 * dl_server_update_idle()
 *   update_curr_dl_se() // idle = true
 *     if (dl_defer && dl_throttled && dl_runtime_exceeded())
 *       if (dl_defer_idle)
 *         return;
 *       dl_defer_running = 0;
 *       hrtimer_try_to_cancel();
 *       replenish_dl_new_period()
 *         // fwd period
 *         dl_throttled = 1;
 *         dl_defer_armed = 1;
 *       dl_defer_idle = 1;
 *       start_dl_timer();        // restart timer
 *       // [C]
 *
 * // stop idle server
 * [8] C->A
 * dl_server_timer()
 *   if (dl_defer_idle)
 *     dl_server_stop();
 *     // [A]
 *
 *
 * digraph dl_server {
 *   "A:init" -> "B:zero_laxity-wait"             [label="1:dl_server_start"]
 *   "A:init" -> "D:running"                      [label="1:dl_server_start"]
 *   "B:zero_laxity-wait" -> "B:zero_laxity-wait" [label="2:dl_server_update"]
 *   "B:zero_laxity-wait" -> "C:idle-wait"        [label="7:dl_server_update_idle"]
 *   "B:zero_laxity-wait" -> "D:running"          [label="3:dl_server_timer"]
 *   "C:idle-wait" -> "A:init"                    [label="8:dl_server_timer"]
 *   "C:idle-wait" -> "B:zero_laxity-wait"        [label="1:dl_server_start"]
 *   "C:idle-wait" -> "B:zero_laxity-wait"        [label="2:dl_server_update"]
 *   "C:idle-wait" -> "C:idle-wait"               [label="7:dl_server_update_idle"]
 *   "D:running" -> "A:init"                      [label="4:pick_task_dl"]
 *   "D:running" -> "E:replenish-wait"            [label="5:update_curr_dl_se"]
 *   "E:replenish-wait" -> "B:zero_laxity-wait"   [label="2:dl_server_update"]
 *   "E:replenish-wait" -> "D:running"            [label="6:dl_server_timer"]
 * }
 *
 *
 * Notes:
 *
 *  - When there are fair tasks running the most likely loop is [2]->[2].
 *    the dl_server never actually runs, the timer never fires.
 *
 *  - When there is actual fair starvation; the timer fires and starts the
 *    dl_server. This will then throttle and replenish like a normal DL
 *    task. Notably it will not 'defer' again.
 *
 *  - When idle it will push the actication forward once, and then wait
 *    for the timer to hit or a non-idle update to restart things.
 */
/* 在 rq 锁下激活已附带宽的 server；必要时抢占较晚 deadline 的当前任务。 */
/*
 * dl_server_start() - 激活一个已完成带宽附着的 deadline server
 *
 * 业务背景：fair/ext server 只有在客户端出现时才应进入 EDF 队列；启动时要先把 donor
 * 的时间记到当前，再由 enqueue_dl_entity() 建立 deadline 实例。入参：dl_se 是其
 * rq 锁下稳定的借用 server；出参/返回：无直接返回值，成功后设置 active、入队并在
 * server 优先于当前 donor 时请求 reschedule；不满足 server/runtime/bw_attached 或
 * CPU 离线条件时安全返回。注意事项：重复 start 必须幂等，dl_server_start() 不负责
 * 附着 reservation；rq->donor 更新与后续 EDF 比较必须在同一 rq 协议内完成。
 */
void dl_server_start(struct sched_dl_entity *dl_se)
{
	struct rq *rq = dl_se->rq;

	/*
	 * 每次显式启动都退出 idle 延后状态；随后用四个状态门保证该操作幂等，
	 * 也避免尚未配置或尚未取得 reservation 的 server 进入 EDF 树。
	 */
	dl_se->dl_defer_idle = 0;
	if (!dl_server(dl_se) || dl_se->dl_server_active || !dl_se->dl_runtime ||
	    !dl_se->dl_bw_attached)
		return;

	/*
	 * Update the current task to 'now'.
	 */
	/* 先结清当前 donor 到此刻的执行时间，避免新 server 抢占时漏记旧任务预算。 */
	rq->donor->sched_class->update_curr(rq);

	if (WARN_ON_ONCE(!cpu_online(cpu_of(rq))))
		return;

	trace_sched_dl_server_start_tp(dl_se, cpu_of(rq), dl_get_type(dl_se, rq));
	/* active 必须先于入队发布；入队会据此建立/恢复 server 的 CBS 实例。 */
	dl_se->dl_server_active = 1;
	enqueue_dl_entity(dl_se, ENQUEUE_WAKEUP);
	if (!dl_task(dl_se->rq->curr) || dl_entity_preempt(dl_se, &rq->curr->dl))
		resched_curr(dl_se->rq);
}

/* 停止 server、取消 timer 并清空 defer 状态，但保留配置与带宽附着。 */
/*
 * dl_server_stop() - 从 EDF 队列停止 active server
 *
 * 业务背景：客户端变为空或 server 进入 idle-wait 时，要撤销调度可见性而保留已配置
 * 的 runtime/period 与 root-domain reservation。入参：dl_se 是 rq 锁下借用 server；
 * 出参/返回：无直接返回值，dequeue、取消 timer 并清除 defer/throttle/active 状态。
 * 注意事项：只对真正 active 的 server 生效；dequeue_dl_entity() 负责队列和统计，
 * timer 取消与状态清理必须在同一生命周期内完成，否则回调可能重新发布已停止 server。
 */
void dl_server_stop(struct sched_dl_entity *dl_se)
{
	/* 非 server 或已停止对象无需重复撤销 timer、队列和统计。 */
	if (!dl_server(dl_se) || !dl_server_active(dl_se))
		return;

	trace_sched_dl_server_stop_tp(dl_se, cpu_of(dl_se->rq),
				      dl_get_type(dl_se, dl_se->rq));
	/* 先从调度可见状态撤下，再取消异步唤醒源，最后统一封闭状态机。 */
	dequeue_dl_entity(dl_se, DEQUEUE_SLEEP);
	hrtimer_try_to_cancel(&dl_se->dl_timer);
	dl_se->dl_defer_armed = 0;
	dl_se->dl_throttled = 0;
	dl_se->dl_defer_idle = 0;
	dl_se->dl_server_active = 0;
}

/*
 * dl_server_init() - 绑定 server 与其所属 rq 及客户端选择回调
 *
 * 业务背景：server 本身没有普通 task 的 task_struct，需要通过回调从保护的客户端类
 * 选择实际工作。入参：dl_se 是待初始化的输出实体，rq 是其借用运行队列，pick_task
 * 是调用者提供的选择函数指针；出参/返回：无直接返回值，写入 rq 和回调，不分配对象。
 * 注意事项：调用者必须保证回调生命周期覆盖 server active 期间，并在发布前完成其余
 * deadline 参数初始化；本函数不持锁、不启动 server。
 */
void dl_server_init(struct sched_dl_entity *dl_se, struct rq *rq,
		    dl_server_pick_f pick_task)
{
	dl_se->rq = rq;
	dl_se->server_pick_task = pick_task;
}

/*
 * sched_init_dl_servers() - 为每个在线 CPU 建立 fair/ext 默认 server
 *
 * 业务背景：deadline server 为 fair 或 sched_ext 工作提供可控的 runtime 窗口；启动时
 * 每 CPU 使用 50ms/1000ms 的默认 reservation，并在 ext 未加载时撤销无用 reservation。
 * 入参：无；出参/返回：无直接返回值，初始化在线 rq 的 server、timer 参数和 defer 状态。
 * 注意事项：每个 CPU 迭代持 rq 锁并推进 rq 时钟；CONFIG_SCHED_CLASS_EXT 分支只在
 * 支持 ext 时建立 ext_server，BPF scheduler 尚未加载时必须 detach 其带宽。
 */
void sched_init_dl_servers(void)
{
	int cpu;
	struct rq *rq;
	struct sched_dl_entity *dl_se;

	/* 启动阶段逐 rq 初始化；rq 锁使时钟、参数和实体发布对该 CPU 原子可见。 */
	for_each_online_cpu(cpu) {
		u64 runtime =  50 * NSEC_PER_MSEC;
		u64 period = 1000 * NSEC_PER_MSEC;

		rq = cpu_rq(cpu);

		guard(rq_lock_irq)(rq);
		update_rq_clock(rq);

		dl_se = &rq->fair_server;

		/* fair server 永久在线配置，首次 apply 同时占用 rq/root-domain 带宽。 */
		WARN_ON(dl_server(dl_se));

		dl_server_apply_params(dl_se, runtime, period, 1);

		dl_se->dl_server = 1;
		dl_se->dl_defer = 1;
		setup_new_dl_entity(dl_se);

#ifdef CONFIG_SCHED_CLASS_EXT
		/* ext server 使用同一默认预算，但是否保留 reservation 取决于 BPF scheduler。 */
		dl_se = &rq->ext_server;

		WARN_ON(dl_server(dl_se));

		dl_server_apply_params(dl_se, runtime, period, 1);

		dl_se->dl_server = 1;
		dl_se->dl_defer = 1;
		setup_new_dl_entity(dl_se);

		/*
		 * No BPF scheduler is loaded at boot, so the ext_server has no
		 * tasks to protect. Detach its bandwidth reservation, it will
		 * be attached when a BPF scheduler is loaded.
		 */
		/* 启动时尚无 BPF scheduler/client，先撤销 ext reservation，加载后再动态附着。 */
		dl_server_detach_bw(dl_se);
#endif
	}
}

/*
 * __dl_server_attach_root() - 在 root-domain 重建后重新发布 server reservation
 *
 * 业务背景：拓扑或 CPU hotplug 改变 root_domain 后，server 需要把已附着的 dl_bw
 * 重新计入新域。入参：dl_se 是带宽已附着的借用 server，rq 是其目标 rq；出参/返回：
 * 无直接返回值，在 dl_bw 锁和 sched RCU 保护下增加 root total_bw。注意事项：
 * rq->this_bw 的恢复由调用方重建流程负责；这里仅处理 root-domain 层，active CPU
 * 数为零时必须跳过，否则会出现除零或错误容量记账。
 */
void __dl_server_attach_root(struct sched_dl_entity *dl_se, struct rq *rq)
{
	u64 new_bw = dl_se->dl_bw;
	int cpu = cpu_of(rq);
	struct dl_bw *dl_b;

	/* 只有逻辑上仍 attached 的 reservation 才需要向重建后的根域重新发布。 */
	if (!dl_se->dl_bw_attached)
		return;

	dl_b = dl_bw_of(cpu_of(rq));
	guard(raw_spinlock)(&dl_b->lock);

	/* 无 active CPU 的过渡根域不承担可运行容量，也不能接收归一化带宽。 */
	if (!dl_bw_cpus(cpu))
		return;

	__dl_add(dl_b, new_bw, dl_bw_cpus(cpu));
}

/* 在 dl_bw 锁下先做准入，再同时替换 root_domain、rq 和实体参数。 */
/*
 * dl_server_apply_params() - 原子地校验并应用 server 的 runtime/period 参数
 *
 * 业务背景：server 参数变化同时影响利用率 reservation、rq 带宽和下一 CBS 实例；
 * 必须在同一 dl_bw 锁域先做 old_bw→new_bw 的准入检查，避免中途暴露不一致状态。
 * 入参：dl_se 是 rq 锁/初始化协议下的借用实体，runtime/period 是 ns 单位的正参数，
 * init 表示首次建立；出参/返回：成功返回 0 并重置实例参数，超容量返回 -EBUSY 且
 * 保持配置不变。注意事项：非首次修改会先替换 rq/root 带宽；调用者须保证 rq 所属
 * root_domain 稳定，返回后需重新 setup/replenish，不能使用旧 deadline/runtime 快照。
 */
int dl_server_apply_params(struct sched_dl_entity *dl_se, u64 runtime, u64 period, bool init)
{
	u64 old_bw = (init || !dl_se->dl_bw_attached) ? 0 :
		     to_ratio(dl_se->dl_period, dl_se->dl_runtime);
	u64 new_bw = to_ratio(period, runtime);
	struct rq *rq = dl_se->rq;
	int cpu = cpu_of(rq);
	struct dl_bw *dl_b;
	unsigned long cap;
	int cpus;

	/* old_bw 仅代表已发布份额；detached server 改参不会从根域扣除不存在的旧值。 */
	dl_b = dl_bw_of(cpu);
	/* dl_b 锁串行化准入与 old→new 记账，失败路径因此可以保持原状态。 */
	guard(raw_spinlock)(&dl_b->lock);

	/* cpus/cap 均取自锁内稳定的当前 root-domain，用于同一份归一化检查。 */
	cpus = dl_bw_cpus(cpu);
	cap = dl_bw_capacity(cpu);

	if (__dl_overflow(dl_b, cap, old_bw, new_bw))
		return -EBUSY;

	/* 首次建立同时发布两级 reservation；修改则只替换已 attached 的份额。 */
	if (init) {
		__add_rq_bw(new_bw, &rq->dl);
		__dl_add(dl_b, new_bw, cpus);
		dl_se->dl_bw_attached = 1;
	} else if (dl_se->dl_bw_attached) {
		__dl_sub(dl_b, dl_se->dl_bw, cpus);
		__dl_add(dl_b, new_bw, cpus);

		dl_rq_change_utilization(rq, dl_se, new_bw);
	}

	/* 准入成功后再提交配置，并清空旧 CBS 实例，强制下一次从新参数建账。 */
	dl_se->dl_runtime = runtime;
	dl_se->dl_deadline = period;
	dl_se->dl_period = period;

	dl_se->runtime = 0;
	dl_se->deadline = 0;

	dl_se->dl_bw = to_ratio(dl_se->dl_period, dl_se->dl_runtime);
	dl_se->dl_density = to_ratio(dl_se->dl_deadline, dl_se->dl_runtime);

	return 0;
}

/*
 * Add @dl_se's bw to the root-domain accounting.
 *
 * Return -EBUSY if attaching would overflow root domain capacity.
 */
/*
 * __dl_server_attach_bw_locked() - 在已持有 dl_bw 锁时附着 server reservation
 *
 * 业务背景：动态加载 server 时，reservation 必须同时进入 rq->this_bw 和 active
 * root_domain->total_bw。入参：dl_se 是待附着 server，dl_b 是其 root-domain 带宽状态，
 * cpus 是当前 active CPU 数；调用者已持 dl_b->lock。出参/返回：成功返回 0 并设置
 * dl_bw_attached，溢出返回 -EBUSY 且不发布 reservation；rq 层即使 CPU inactive 也
 * 更新，root 层只对 active CPU 发布。注意事项：该函数不启动 server，外层成功后需
 * 补发可能错过的 0→nr_running 边沿。
 */
static int __dl_server_attach_bw_locked(struct sched_dl_entity *dl_se,
					struct dl_bw *dl_b, int cpus)
{
	struct rq *rq = dl_se->rq;
	unsigned long cap;

	/*
	 * Always update @rq->dl.this_bw, but only update @dl_b->total_bw
	 * (and run the overflow check it gates) while this CPU is active.
	 *
	 * This mirrors dl_server_add_bw() during root-domain rebuilds, which
	 * only publishes bandwidth from active CPUs into @dl_b.
	 */
	if (cpu_active(cpu_of(rq))) {
		/* active CPU 的份额必须先通过根域容量检查，才能对外发布。 */
		cap = dl_bw_capacity(cpu_of(rq));
		if (__dl_overflow(dl_b, cap, 0, dl_se->dl_bw))
			return -EBUSY;
		__dl_add(dl_b, dl_se->dl_bw, cpus);
	}
	/* rq 本地份额不依赖 active 状态，供该 rq 恢复上线时重建根域统计。 */
	__add_rq_bw(dl_se->dl_bw, &rq->dl);
	dl_se->dl_bw_attached = 1;

	return 0;
}

/*
 * Drain @dl_se and remove its bw from the root-domain accounting.
 */
/*
 * __dl_server_detach_bw_locked() - 在已持有 dl_bw 锁时撤销 server reservation
 *
 * 业务背景：停止或卸载 server 时要逆向撤销 active/rq/root 两级带宽，同时处理仍在
 * 队列或 inactive timer 中的实体。入参：dl_se 是待撤销 server，dl_b 是 root-domain
 * 状态，cpus 是 active CPU 数；调用者已持锁。出参/返回：无直接返回值，停止 active
 * server、归还 rq 带宽、按 active 状态扣减 root total_bw 并清除 attached。注意事项：
 * inactive CPU 的 root 带宽可能从未发布，不能无条件扣减；timer/0-lag cleanup 必须
 * 先完成，避免回调在 reservation 已清除后继续改动错误统计。
 */
static void __dl_server_detach_bw_locked(struct sched_dl_entity *dl_se,
					 struct dl_bw *dl_b, int cpus)
{
	struct rq *rq = dl_se->rq;

	/*
	 * If the server is still active (on_rq), dequeue it via
	 * dl_server_stop(); task_non_contending() will either subtract
	 * @dl_bw from running_bw immediately (0-lag passed) or set
	 * dl_non_contending and arm the inactive_timer.
	 */
	/* active server 先经 stop 离队；0-lag 已过则立即扣 running_bw，否则进入 timer 延迟归还。 */
	if (dl_se->dl_server_active)
		dl_server_stop(dl_se);

	/*
	 * Drop @dl_se's contribution from this rq's bandwidth accounting,
	 * mirroring the __add_rq_bw() done at attach time.
	 */
	/* 从本 rq 撤销该 server 的 reservation，与 attach 时的本地加账严格对称。 */
	dl_rq_change_utilization(rq, dl_se, 0);

	/*
	 * Update @dl_b only while this CPU is active, matching
	 * dl_server_add_bw() during root-domain rebuilds.
	 *
	 * If this CPU is inactive, its bandwidth is not currently accounted in
	 * @dl_b->total_bw: either attach skipped adding it, or a rebuild
	 * already dropped it while re-publishing active CPUs only.
	 *
	 * In that case there is nothing to subtract from @dl_b. Just clear
	 * @dl_se->dl_bw_attached; if the CPU becomes active again, the next
	 * rebuild will re-publish its bandwidth.
	 */
	/*
	 * 只有 CPU active 时才修改根域总量，与重建时仅发布 active CPU 一致。inactive CPU
	 * 的份额当前不在 total_bw 中，无需扣除；只清 attached，未来上线重建时再重新发布。
	 */
	if (cpu_active(cpu_of(rq)))
		__dl_sub(dl_b, dl_se->dl_bw, cpus);
	/* 清 attached 是 ownership 交还的提交点，后续 start 会被状态门拒绝。 */
	dl_se->dl_bw_attached = 0;
}

/*
 * Attach @dl_se's bandwidth to the root domain's total_bw accounting.
 *
 * Use to dynamically register a dl_server's bandwidth reservation while
 * preserving its configured @dl_runtime / @dl_period. No-op if @dl_se is
 * already attached.
 *
 * Returns -EBUSY if attaching would overflow the root domain capacity.
 */
/* 动态附着 reservation；溢出返回 -EBUSY，成功后补偿可能错过的启动边沿。 */
/*
 * dl_server_attach_bw() - 为 server 动态注册 root-domain reservation
 *
 * 业务背景：sched_ext 等模块可能在运行中启用 server，必须以锁保护的准入检查加入
 * reservation，并补发 server start。入参：dl_se 是待附着的借用 server；出参/返回：
 * 已附着时返回 0，成功附着返回 0，容量不足返回 -EBUSY；成功还可能启动 server，
 * 不转移对象 ownership。注意事项：dl_bw 锁只覆盖带宽原子操作，之后的 start 由
 * 在线 CPU 条件保护；离线 CPU 等待重新上线自然启动。
 */
int dl_server_attach_bw(struct sched_dl_entity *dl_se)
{
	struct rq *rq = dl_se->rq;
	int cpu = cpu_of(rq);
	struct dl_bw *dl_b;
	int cpus, ret;

	/* attached 是 reservation ownership 标志，也是重复注册的幂等门。 */
	if (dl_se->dl_bw_attached)
		return 0;

	/* 锁内完成检查与两级记账；任何失败都不留下半附着状态。 */
	scoped_guard (raw_spinlock, &dl_bw_of(cpu)->lock) {
		dl_b = dl_bw_of(cpu);
		cpus = dl_bw_cpus(cpu);
		ret = __dl_server_attach_bw_locked(dl_se, dl_b, cpus);
	}
	/* 锁内 helper 失败时 attached 仍为零，外层不执行补偿启动。 */
	if (ret)
		return ret;

	/*
	 * The natural 0->nr_running transition that triggers dl_server_start()
	 * may have happened while @dl_se was still detached (e.g., between
	 * scx_bypass(false) and the scx_enable() re-balance loop), so kick a
	 * start here.
	 *
	 * dl_server_start() bails out cleanly if there's nothing to schedule or
	 * it's already active. Skip if @cpu is offline; the server will be
	 * started naturally on the first enqueue once @cpu comes back.
	 */
	/* attach 期间可能错过 nr_running 的 0→非零边沿，故在线时补 start；离线则等首次 enqueue。 */
	if (cpu_online(cpu))
		dl_server_start(dl_se);

	return 0;
}

/*
 * Detach @dl_se's bandwidth from the root domain's total_bw accounting.
 *
 * Use to dynamically unregister a dl_server's bandwidth reservation while
 * preserving its configured @dl_runtime / @dl_period. No-op if @dl_se is
 * not currently attached.
 */
/* 在同一 dl_bw 锁域停止 server 并归还 rq/root_domain 两级带宽。 */
/*
 * dl_server_detach_bw() - 动态注销 server 的 reservation
 *
 * 业务背景：server 被卸载或暂时不再保护客户端时，需要撤销 root/rq 记账但保留
 * runtime/period 配置供未来重新附着。入参：dl_se 是借用 server；出参/返回：无直接
 * 返回值，已撤销时幂等返回，否则持 dl_bw 锁完成逆向清理。注意事项：调用者须确保
 * server 的 rq/root_domain 生命周期有效；本函数不释放实体本身，attached 标志才是
 * 后续 start/attach 是否可用的状态门。
 */
void dl_server_detach_bw(struct sched_dl_entity *dl_se)
{
	int cpu = cpu_of(dl_se->rq);
	struct dl_bw *dl_b;
	int cpus;

	/* 未持有 reservation 时既不应停 server，也不能从共享总量重复扣减。 */
	if (!dl_se->dl_bw_attached)
		return;

	/* detach helper 在同一根域锁内封闭 timer、rq 与 total_bw 的状态变化。 */
	dl_b = dl_bw_of(cpu);
	guard(raw_spinlock)(&dl_b->lock);
	cpus = dl_bw_cpus(cpu);
	__dl_server_detach_bw_locked(dl_se, dl_b, cpus);
}

/*
 * Atomically detach @detach_se and attach @attach_se on the same rq, holding
 * @dl_b->lock across both operations so a concurrent sched_setattr() cannot
 * steal the bandwidth freed by the detach before the attach can claim it.
 *
 * Both entities must live on the same rq (same root domain). Returns the
 * result of the attach: -EBUSY if attaching @attach_se would overflow root
 * domain capacity (in which case both servers end up detached).
 */
/* 同锁先 detach 后 attach，防止并发 sched_setattr 抢走刚释放的容量。 */
/*
 * dl_server_swap_bw() - 在同一 root-domain 原子替换两个 server 的 reservation
 *
 * 业务背景：切换 sched_ext server 时，detach 与 attach 之间不能让并发参数更新抢走
 * 刚释放的容量。入参：detach_se 和 attach_se 是同一 rq 的借用 server；出参/返回：
 * 成功返回 0 并完成 detach→attach，失败返回 -EBUSY 且两个 server 都保持 detached。
 * 注意事项：dl_bw 锁跨越两次操作，rq 不同会触发 WARN；成功后在线 CPU 再启动新 server，
 * 因此函数同时是带宽 ownership 的原子交换边界。
 */
int dl_server_swap_bw(struct sched_dl_entity *detach_se,
		      struct sched_dl_entity *attach_se)
{
	struct rq *rq = detach_se->rq;
	int cpu = cpu_of(rq);
	struct dl_bw *dl_b;
	int cpus, ret;

	/* 两个实体不共享 rq 时，下面的一把 dl_b 锁无法提供原子交换语义。 */
	WARN_ON_ONCE(attach_se->rq != rq);

	/* 先释放旧份额再申请新份额；锁不释放，容量不会被第三方插队占用。 */
	scoped_guard (raw_spinlock, &dl_bw_of(cpu)->lock) {
		dl_b = dl_bw_of(cpu);
		cpus = dl_bw_cpus(cpu);

		if (detach_se->dl_bw_attached)
			__dl_server_detach_bw_locked(detach_se, dl_b, cpus);

		if (attach_se->dl_bw_attached)
			ret = 0;
		else
			ret = __dl_server_attach_bw_locked(attach_se, dl_b, cpus);
	}
	/* attach 失败不会回滚旧 server：接口契约明确让二者都保持 detached。 */
	if (ret)
		return ret;

	/* 带宽 ownership 已提交后，补偿此前因 detached 而错过的启动边沿。 */
	if (cpu_online(cpu))
		dl_server_start(attach_se);

	return 0;
}

/*
 * Update the current task's runtime statistics (provided it is still
 * a -deadline task and has not been removed from the dl_rq).
 */
/* 仅当前 donor 仍为排队 DL 实体时扣预算；IRQ 时间不算执行但 deadline 走墙钟。 */
/*
 * update_curr_dl() - 更新 rq 当前 donor 的 deadline 执行统计
 *
 * 业务背景：调度核心只把仍是 deadline 且仍在 EDF rq 中的 donor 交给本类记账；
 * 被移除或切换策略的 task 不得继续扣旧 reservation。入参：rq 是持锁运行队列；
 * 无显式输出。出参/返回：无直接返回值，update_curr_common() 提供可计费执行时间，
 * 再由 update_curr_dl_se() 扣 runtime。注意事项：预算排除 hardirq 时间，而绝对
 * deadline 使用 wall time；这是两种时钟刻度的有意分离，不能合并为同一个 delta。
 */
static void update_curr_dl(struct rq *rq)
{
	struct task_struct *donor = rq->donor;
	struct sched_dl_entity *dl_se = &donor->dl;
	s64 delta_exec;

	if (!dl_task(donor) || !on_dl_rq(dl_se))
		return;

	/*
	 * Consumed budget is computed considering the time as
	 * observed by schedulable tasks (excluding time spent
	 * in hardirq context, etc.). Deadlines are instead
	 * computed using hard walltime. This seems to be the more
	 * natural solution, but the full ramifications of this
	 * approach need further study.
	 */
	/* 预算按可调度任务观察到的执行时间扣除（排除 hardirq 等），deadline 则按墙钟推进。 */
	delta_exec = update_curr_common(rq);
	update_curr_dl_se(rq, dl_se, delta_exec);
}

/* 0-lag 到期后在 rq 锁下移出 active utilization，并处理退出/改策略的最终归还。 */
/*
 * inactive_task_timer() - 完成 non-contending task 的 0-lag 带宽归还
 *
 * 业务背景：任务阻塞后 timer 到 0-lag 才能安全从 running_bw 移除；若期间退出或
 * 改变调度策略，还必须撤销 root-domain reservation 和参数。入参：timer 是嵌入
 * sched_dl_entity 的借用 timer；出参/返回：返回 HRTIMER_NORESTART，按 server/普通
 * task 分别获取 rq 锁，清除 dl_non_contending，并在必要时扣 running_bw、this_bw、
 * total_bw。注意事项：普通 task 先 task_rq_lock 再在最终 put_task_struct；server
 * 只持 rq 锁。`dl_non_contending` 是 timer 与 wakeup 的一次性交接位，清零必须在扣账
 * 后完成，避免并发唤醒重复增加 active utilization。
 */
static enum hrtimer_restart inactive_task_timer(struct hrtimer *timer)
{
	struct sched_dl_entity *dl_se = container_of(timer,
						     struct sched_dl_entity,
						     inactive_timer);
	struct task_struct *p = NULL;
	struct rq_flags rf;
	struct rq *rq;

	/* 普通任务需要同时稳定 task->cpu 与 rq；server 没有 task，只锁固定的所属 rq。 */
	if (!dl_server(dl_se)) {
		p = dl_task_of(dl_se);
		rq = task_rq_lock(p, &rf);
	} else {
		rq = dl_se->rq;
		rq_lock(rq, &rf);
	}

	/* timer 可在 tick 之外运行，先推进时钟再判断 0-lag 状态和执行扣账。 */
	sched_clock_tick();
	update_rq_clock(rq);

	if (dl_server(dl_se))
		goto no_task;

	/* 退出或离开 DL 策略的任务在这里完成 reservation 与参数的最终销毁。 */
	if (!dl_task(p) || READ_ONCE(p->__state) == TASK_DEAD) {
		struct dl_bw *dl_b = dl_bw_of(task_cpu(p));

		if (READ_ONCE(p->__state) == TASK_DEAD && dl_se->dl_non_contending) {
			sub_running_bw(&p->dl, dl_rq_of_se(&p->dl));
			sub_rq_bw(&p->dl, dl_rq_of_se(&p->dl));
			dl_se->dl_non_contending = 0;
		}

		/* root-domain 总量受 dl_b 锁保护；rq 层已由上面的 task_rq_lock 串行化。 */
		raw_spin_lock(&dl_b->lock);
		__dl_sub(dl_b, p->dl.dl_bw, dl_bw_cpus(task_cpu(p)));
		raw_spin_unlock(&dl_b->lock);
		__dl_clear_params(dl_se);

		goto unlock;
	}

no_task:
	/* 唤醒路径若已抢先清除此位，timer 只负责解锁，不能再次扣 running_bw。 */
	if (dl_se->dl_non_contending == 0)
		goto unlock;

	sub_running_bw(dl_se, &rq->dl);
	dl_se->dl_non_contending = 0;
unlock:

	/* 与入口的两种锁/引用协议严格配对；仅普通任务持有额外 task 引用。 */
	if (!dl_server(dl_se)) {
		task_rq_unlock(rq, p, &rf);
		put_task_struct(p);
	} else {
		rq_unlock(rq, &rf);
	}

	return HRTIMER_NORESTART;
}

/*
 * init_dl_inactive_task_timer() - 初始化 0-lag inactive timer
 *
 * 业务背景：阻塞实体在 0-lag 到期前仍占用 running_bw，需要独立 timer 完成延迟归还。
 * 入参：dl_se 是待初始化的借用实体；出参/返回：无直接返回值，把 timer 绑定到
 * inactive_task_timer()，不取得 task 引用。注意事项：引用由 task_non_contending()
 * 在真正启动 timer 时取得，取消/回调负责释放；本函数不能提前修改带宽状态。
 */
static void init_dl_inactive_task_timer(struct sched_dl_entity *dl_se)
{
	struct hrtimer *timer = &dl_se->inactive_timer;

	hrtimer_setup(timer, inactive_task_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
}

#define __node_2_dle(node) \
	rb_entry((node), struct sched_dl_entity, rb_node)

/* 新最早 deadline 同步发布到 cpudl，并把 cpupri 提升到 DL 档。 */
/*
 * inc_dl_deadline() - 在 rq 出现更早 deadline 时更新跨 CPU 提示
 *
 * 业务背景：本地 EDF 树决定 rq 内顺序，而 cpupri/cpudl 让 push/pull 快速发现具有
 * 紧迫 deadline 的 CPU。入参：dl_rq 是持 rq 锁的借用队列，deadline 是新实体绝对
 * deadline；出参/返回：无直接返回值，仅在新值更早或队列原为空时更新 earliest_dl.curr、
 * cpudl 和 cpupri。注意事项：这些是迁移候选提示，不替代本地红黑树；发布前必须保证
 * 实体已完成其 deadline 初始化。
 */
static void inc_dl_deadline(struct dl_rq *dl_rq, u64 deadline)
{
	struct rq *rq = rq_of_dl_rq(dl_rq);

	if (dl_rq->earliest_dl.curr == 0 ||
	    dl_time_before(deadline, dl_rq->earliest_dl.curr)) {
		/* 空→非空边沿还要覆盖 RT cpupri 提示，后续更早插入只更新 deadline。 */
		if (dl_rq->earliest_dl.curr == 0)
			cpupri_set(&rq->rd->cpupri, rq->cpu, CPUPRI_HIGHER);
		dl_rq->earliest_dl.curr = deadline;
		cpudl_set(&rq->rd->cpudl, rq->cpu, deadline);
	}
}

/* 删除后从 EDF 树重算最早值；空队列同时清除 cpudl 并恢复 RT 优先级。 */
/*
 * dec_dl_deadline() - 在实体离队后重建 rq 的最早 deadline 提示
 *
 * 业务背景：删除树首后旧 earliest 值失效，跨 CPU 调度器必须看到新树首或空队列。
 * 入参：dl_rq 是持锁队列，deadline 是被删除实体的旧值；出参/返回：无直接返回值，
 * 空队列清零 curr/next、清 cpudl 并恢复 RT 优先级，非空则从 rb_first_cached() 发布
 * 新树首。注意事项：即使传入 deadline 不是树首也要依据 dl_nr_running 重算，不能
 * 只做数值比较；调用者必须先完成红黑树删除和计数更新。
 */
static void dec_dl_deadline(struct dl_rq *dl_rq, u64 deadline)
{
	struct rq *rq = rq_of_dl_rq(dl_rq);

	/*
	 * Since we may have removed our earliest (and/or next earliest)
	 * task we must recompute them.
	 */
	if (!dl_rq->dl_nr_running) {
		/* 空树同时撤销本地两个快照和 root-domain 的 cpudl/cpupri 发布。 */
		dl_rq->earliest_dl.curr = 0;
		dl_rq->earliest_dl.next = 0;
		cpudl_clear(&rq->rd->cpudl, rq->cpu, rq->online);
		cpupri_set(&rq->rd->cpupri, rq->cpu, rq->rt.highest_prio.curr);
	} else {
		struct rb_node *leftmost = rb_first_cached(&dl_rq->root);
		struct sched_dl_entity *entry = __node_2_dle(leftmost);

		/* 非空树直接从 cached leftmost 重建，不能依赖已删除 deadline 参数。 */
		dl_rq->earliest_dl.curr = entry->deadline;
		cpudl_set(&rq->rd->cpudl, rq->cpu, entry->deadline);
	}
}

static inline
/*
 * inc_dl_tasks() - 发布一个 EDF 实体后的 runnable 计数与跨 CPU 索引更新
 *
 * 入参 dl_se/dl_rq 均为 rq 锁下借用对象；无返回值。普通任务同时计入 rq 的
 * nr_running，server 只计入 dl_nr_running，因为它代表借用调度上下文而非新任务。
 * 最后必须用实体绝对 deadline 更新 cpupri/cpudl，顺序与 rb 插入协议配套。
 */
void inc_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	u64 deadline = dl_se->deadline;

	dl_rq->dl_nr_running++;

	if (!dl_server(dl_se))
		add_nr_running(rq_of_dl_rq(dl_rq), 1);

	inc_dl_deadline(dl_rq, deadline);
}

static inline
/*
 * dec_dl_tasks() - 撤销一个 EDF 实体的 runnable 计数并重建最早 deadline
 *
 * 入参是已从 rb 树删除的实体和所属 dl_rq；无返回值。WARN 捕获计数下溢，普通
 * task 才减少全局 nr_running；server 不代表独立 runnable task。调用结尾重算
 * cpudl/cpupri，调用者不能在删除前执行本函数。
 */
void dec_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	WARN_ON(!dl_rq->dl_nr_running);
	dl_rq->dl_nr_running--;

	if (!dl_server(dl_se))
		sub_nr_running(rq_of_dl_rq(dl_rq), 1);

	dec_dl_deadline(dl_rq, dl_se->deadline);
}

/* EDF 红黑树比较器：仅按环绕安全的绝对 deadline 排序，返回 a 是否更早。 */
static inline bool __dl_less(struct rb_node *a, const struct rb_node *b)
{
	return dl_time_before(__node_2_dle(a)->deadline, __node_2_dle(b)->deadline);
}

static __always_inline struct sched_statistics *
/*
 * __schedstats_from_dl_se() - 取得普通 DL task 的可选统计对象
 *
 * schedstat 关闭或实体是 server 时返回 NULL；否则返回 task 内嵌 stats 的借用指针。
 * server 没有可安全反解的 task_struct，所有调用者都必须先接受 NULL 结果。
 */
__schedstats_from_dl_se(struct sched_dl_entity *dl_se)
{
	if (!schedstat_enabled())
		return NULL;

	if (dl_server(dl_se))
		return NULL;

	return &dl_task_of(dl_se)->stats;
}

static inline void
/*
 * update_stats_wait_start_dl() - 开启普通 DL task 的等待统计区间
 *
 * 入参 dl_rq/dl_se 在 rq 锁下借用；无返回值。schedstat 关闭或 server 时为空操作，
 * 否则把 rq/task/stats 交给通用 helper；不改变调度 ownership。
 */
update_stats_wait_start_dl(struct dl_rq *dl_rq, struct sched_dl_entity *dl_se)
{
	struct sched_statistics *stats = __schedstats_from_dl_se(dl_se);
	if (stats)
		__update_stats_wait_start(rq_of_dl_rq(dl_rq), dl_task_of(dl_se), stats);
}

static inline void
/* 与 wait_start 配对结束等待区间；输入/锁语义相同，server 或关闭统计时无副作用。 */
update_stats_wait_end_dl(struct dl_rq *dl_rq, struct sched_dl_entity *dl_se)
{
	struct sched_statistics *stats = __schedstats_from_dl_se(dl_se);
	if (stats)
		__update_stats_wait_end(rq_of_dl_rq(dl_rq), dl_task_of(dl_se), stats);
}

static inline void
/* 睡眠唤醒入队时累计 sleeper 延迟；输入为锁下借用对象，无返回值且只处理普通 task。 */
update_stats_enqueue_sleeper_dl(struct dl_rq *dl_rq, struct sched_dl_entity *dl_se)
{
	struct sched_statistics *stats = __schedstats_from_dl_se(dl_se);
	if (stats)
		__update_stats_enqueue_sleeper(rq_of_dl_rq(dl_rq), dl_task_of(dl_se), stats);
}

static inline void
/* 入队统计分发器：flags 为事件位；仅启用统计的 ENQUEUE_WAKEUP 形成 sleeper 边沿，无返回值。 */
update_stats_enqueue_dl(struct dl_rq *dl_rq, struct sched_dl_entity *dl_se,
			int flags)
{
	if (!schedstat_enabled())
		return;

	if (flags & ENQUEUE_WAKEUP)
		update_stats_enqueue_sleeper_dl(dl_rq, dl_se);
}

static inline void
/*
 * update_stats_dequeue_dl() - 结束等待并按睡眠类型记录 block 起点
 *
 * 入参为 rq 锁下队列、普通 task 实体和 dequeue flags；无返回值。当前任务没有
 * 排队等待区间，因此不结束 wait；DEQUEUE_SLEEP 再按 task state 区分可中断睡眠
 * 与不可中断阻塞。该 helper 不能用于 server，因为入口会反解 dl_task_of()。
 */
update_stats_dequeue_dl(struct dl_rq *dl_rq, struct sched_dl_entity *dl_se,
			int flags)
{
	struct task_struct *p = dl_task_of(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

	if (!schedstat_enabled())
		return;

	/* 只有非当前 task 曾在 EDF 树中等待 CPU，才能闭合 wait 统计区间。 */
	if (p != rq->curr)
		update_stats_wait_end_dl(dl_rq, dl_se);

	/* 睡眠 dequeue 记录下一次唤醒所需的起始时间，迁移/策略切换不记录。 */
	if ((flags & DEQUEUE_SLEEP)) {
		unsigned int state;

		state = READ_ONCE(p->__state);
		/* 两种睡眠状态分别驱动 sleep_max 与 block_max，可能的其他状态不建区间。 */
		if (state & TASK_INTERRUPTIBLE)
			__schedstat_set(p->stats.sleep_start,
					rq_clock(rq_of_dl_rq(dl_rq)));

		if (state & TASK_UNINTERRUPTIBLE)
			__schedstat_set(p->stats.block_start,
					rq_clock(rq_of_dl_rq(dl_rq)));
	}
}

/* rq 锁下链接 EDF 缓存红黑树并更新 runnable/deadline 索引。 */
/*
 * __enqueue_dl_entity() - 把状态已准备好的实体插入本地 EDF 树
 *
 * 入参 dl_se 是 rq 锁下、尚未链接的借用实体；无返回值。rb_add_cached() 维护最左
 * 缓存，随后 inc_dl_tasks() 发布计数和跨 CPU 最早 deadline。WARN 防止同一 rb_node
 * 重复 ownership；CBS、timer 与带宽状态必须由外层在调用前完成。
 */
static void __enqueue_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

	WARN_ON_ONCE(!RB_EMPTY_NODE(&dl_se->rb_node));

	rb_add_cached(&dl_se->rb_node, &dl_rq->root, __dl_less);

	inc_dl_tasks(dl_se, dl_rq);
}

/* 幂等删除 EDF 节点并清空节点标记，随后修正 rq 的最早 deadline。 */
/*
 * __dequeue_dl_entity() - 从本地 EDF 树撤销实体的 runnable 发布
 *
 * 入参 dl_se 是 rq 锁下借用实体；无返回值，未链接时幂等退出。删除后立即清空
 * rb_node ownership，再由 dec_dl_tasks() 修正计数与 cpudl/cpupri。函数不处理 CBS、
 * timer 或 active utilization，这些属于外层事件语义。
 */
static void __dequeue_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

	if (RB_EMPTY_NODE(&dl_se->rb_node))
		return;

	rb_erase_cached(&dl_se->rb_node, &dl_rq->root);

	RB_CLEAR_NODE(&dl_se->rb_node);

	dec_dl_tasks(dl_se, dl_rq);
}

/* 按唤醒、补充或迁移语义先更新 CBS；仍 throttle 时只保留带宽记账。 */
/*
 * enqueue_dl_entity() - 按事件类型更新 CBS 并把实体发布到 EDF 队列
 *
 * 业务背景：wakeup、replenish、migration 和 restore 进入队列时的预算语义不同；
 * 该函数是状态更新与真正 rb 插入之间的协议边界。入参：dl_se 是 rq 锁下借用实体，
 * flags 描述事件及 ownership 变化；出参/返回：无直接返回值，可能更新带宽、调用
 * task_contending/update_dl_entity/replenish、启动 timer，最终通过 __enqueue_dl_entity
 * 发布到 EDF 树。注意事项：throttled 且非 replenish 时不能入树但仍可能占 active
 * utilization；timer 启动失败才清 throttle 并立即发布，顺序改变会造成永不唤醒或
 * 重复记账。
 */
static void
enqueue_dl_entity(struct sched_dl_entity *dl_se, int flags)
{
	WARN_ON_ONCE(on_dl_rq(dl_se));

	/* 统计先记录事件；后面的 constrained/throttle 判断可能不真正插入 rb 树。 */
	update_stats_enqueue_dl(dl_rq_of_se(dl_se), dl_se, flags);

	/*
	 * Check if a constrained deadline task was activated
	 * after the deadline but before the next period.
	 * If that is the case, the task will be throttled and
	 * the replenishment timer will be set to the next period.
	 */
	/* constrained task 若在 deadline 后、下一 period 前激活，就 throttle 到下一 period。 */
	if (!dl_se->dl_throttled && !dl_is_implicit(dl_se))
		dl_check_constrained_dl(dl_se);

	/* restore/migrating 表示 reservation ownership 曾离开 rq，必须先恢复两级本地统计。 */
	if (flags & (ENQUEUE_RESTORE|ENQUEUE_MIGRATING)) {
		struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

		add_rq_bw(dl_se, dl_rq);
		add_running_bw(dl_se, dl_rq);
	}

	/*
	 * If p is throttled, we do not enqueue it. In fact, if it exhausted
	 * its budget it needs a replenishment and, since it now is on
	 * its rq, the bandwidth timer callback (which clearly has not
	 * run yet) will take care of this.
	 * However, the active utilization does not depend on the fact
	 * that the task is on the runqueue or not (but depends on the
	 * task's state - in GRUB parlance, "inactive" vs "active contending").
	 * In other words, even if a task is throttled its utilization must
	 * be counted in the active utilization; hence, we need to call
	 * add_running_bw().
	 */
	/* throttle task 不入 EDF 树，但在 GRUB 中仍属 active utilization，wakeup 需完成状态交接。 */
	if (!dl_se->dl_defer && dl_se->dl_throttled && !(flags & ENQUEUE_REPLENISH)) {
		/* 普通 throttle task 等待自己的 timer；wakeup 只修正 non-contending 状态。 */
		if (flags & ENQUEUE_WAKEUP)
			task_contending(dl_se, flags);

		return;
	}

	/*
	 * If this is a wakeup or a new instance, the scheduling
	 * parameters of the task might need updating. Otherwise,
	 * we want a replenishment of its runtime.
	 */
	/* wakeup 更新 CBS 参数，REPLENISH 恢复预算，MOVE 且 deadline 过期则创建新实例。 */
	if (flags & ENQUEUE_WAKEUP) {
		/* 唤醒先恢复 active 状态，再选择原始或 revised CBS 参数。 */
		task_contending(dl_se, flags);
		update_dl_entity(dl_se);
	} else if (flags & ENQUEUE_REPLENISH) {
		replenish_dl_entity(dl_se);
	} else if ((flags & ENQUEUE_MOVE) &&
		   !is_dl_boosted(dl_se) &&
		   dl_time_before(dl_se->deadline, rq_clock(rq_of_dl_se(dl_se)))) {
		setup_new_dl_entity(dl_se);
	}

	/*
	 * If the reservation is still throttled, e.g., it got replenished but is a
	 * deferred task and still got to wait, don't enqueue.
	 */
	/* 补充后仍 throttle 的 defer 实体若成功安装未来 timer，就继续等待而不入树。 */
	if (dl_se->dl_throttled && start_dl_timer(dl_se))
		/* deferred/throttled 实体仍有未来 timer 时不进入 EDF 树。 */
		return;

	/*
	 * We're about to enqueue, make sure we're not ->dl_throttled!
	 * In case the timer was not started, say because the defer time
	 * has passed, mark as not throttled and mark unarmed.
	 * Also cancel earlier timers, since letting those run is pointless.
	 */
	/* timer 未启动时清 throttle/armed 并取消旧 timer，确保即将入树的实体不再被陈旧回调处理。 */
	if (dl_se->dl_throttled) {
		hrtimer_try_to_cancel(&dl_se->dl_timer);
		dl_se->dl_defer_armed = 0;
		dl_se->dl_throttled = 0;
	}

	__enqueue_dl_entity(dl_se);
	/* rb 插入是外界可观察的 runnable 发布点，之前所有预算和 timer 状态已稳定。 */
}

/* 离队时分别处理迁移所有权和阻塞 0-lag，二者不能混为立即归还。 */
/*
 * dequeue_dl_entity() - 按离队原因撤销 EDF 发布并交接带宽状态
 *
 * 入参 dl_se 在所属 rq 锁下借用，flags 表示 save/migrate/sleep；无返回值。先删除 rb
 * 节点；迁移/保存立即搬走 running_bw/this_bw，睡眠则进入 0-lag non-contending 协议。
 * 调用者另行处理 task 统计和 pushable 树，不能把迁移误作睡眠延迟归还。
 */
static void dequeue_dl_entity(struct sched_dl_entity *dl_se, int flags)
{
	/* 先摘除 EDF 可见性，再按迁移或睡眠事件分别处理带宽 ownership。 */
	__dequeue_dl_entity(dl_se);

	if (flags & (DEQUEUE_SAVE|DEQUEUE_MIGRATING)) {
		struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

		sub_running_bw(dl_se, dl_rq);
		sub_rq_bw(dl_se, dl_rq);
	}

	/*
	 * This check allows to start the inactive timer (or to immediately
	 * decrease the active utilization, if needed) in two cases:
	 * when the task blocks and when it is terminating
	 * (p->state == TASK_DEAD). We can handle the two cases in the same
	 * way, because from GRUB's point of view the same thing is happening
	 * (the task moves from "active contending" to "active non contending"
	 * or "inactive")
	 */
	/* 阻塞和终止对 GRUB 都是从 active contending 转为 non-contending/inactive，可共用 0-lag。 */
	if (flags & DEQUEUE_SLEEP)
		task_non_contending(dl_se, true);
}

/* PI boost 可覆盖 throttle；普通可迁移任务在入 EDF 树后再进入 push 树。 */
/*
 * enqueue_task_dl() - 把 task 级 deadline 实体接入 rq 的调度路径
 *
 * 业务背景：调度核心以 task 为单位 enqueue，但 EDF 树以 sched_dl_entity 为节点；
 * 本 wrapper 处理 PI boost/deboost、统计、迁移标志和 push 候选。入参：rq 是持锁
 * 目标队列，p 是稳定 task，flags 描述唤醒/迁移/补充事件；出参/返回：无直接返回值，
 * 普通 task 最终可能进入 EDF 与 pushable 两棵树，server 或 blocked task 在相应边界
 * 返回。注意事项：PI boost 可覆盖 throttle，deboost 若缺少 REPLENISH 会留下无法
 * 清理的状态；必须先 update_stats_wait_start，再按 task 当前状态决定是否可迁移。
 */
static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_dl_entity *dl_se = &p->dl;
	struct dl_rq *dl_rq = &rq->dl;

	if (is_dl_boosted(dl_se)) {
		/* boost 与 throttle 同时存在时，PI 优先保证互斥锁持有者继续推进。 */
		/*
		 * Because of delays in the detection of the overrun of a
		 * thread's runtime, it might be the case that a thread
		 * goes to sleep in a rt mutex with negative runtime. As
		 * a consequence, the thread will be throttled.
		 *
		 * While waiting for the mutex, this thread can also be
		 * boosted via PI, resulting in a thread that is throttled
		 * and boosted at the same time.
		 *
		 * In this case, the boost overrides the throttle.
		 */
		/* overrun 检测延迟可让 rt-mutex 睡眠者同时 throttle 和 PI boost；此时 boost 优先。 */
		if (dl_se->dl_throttled) {
			/*
			 * The replenish timer needs to be canceled. No
			 * problem if it fires concurrently: boosted threads
			 * are ignored in dl_task_timer().
			 */
			/* 必须取消 replenish timer；并发回调会忽略 boosted task，因此竞态安全。 */
			cancel_replenish_timer(dl_se);
			dl_se->dl_throttled = 0;
		}
	} else if (!dl_prio(p->normal_prio)) {
		/* deboost 后即将离开 DL 类：清掉 throttle，避免未来重新 boost 时无恢复 timer。 */
		/*
		 * Special case in which we have a !SCHED_DEADLINE task that is going
		 * to be deboosted, but exceeds its runtime while doing so. No point in
		 * replenishing it, as it's going to return back to its original
		 * scheduling class after this. If it has been throttled, we need to
		 * clear the flag, otherwise the task may wake up as throttled after
		 * being boosted again with no means to replenish the runtime and clear
		 * the throttle.
		 */
		/* 非 DL task deboost 时不再需要充值；必须清 throttle，避免未来再次 boost 后永久限流。 */
		dl_se->dl_throttled = 0;
		/* deboost 异常缺失 REPLENISH 时仅告警；继续入队会把非 DL task 错发到 EDF。 */
		if (!(flags & ENQUEUE_REPLENISH))
			printk_deferred_once("sched: DL de-boosted task PID %d: REPLENISH flag missing\n",
					     task_pid_nr(p));

		return;
	}

	check_schedstat_required();
	/* 从 task wrapper 开启 wait 区间，再把迁移状态编码进 entity flags。 */
	update_stats_wait_start_dl(dl_rq, dl_se);

	if (task_on_rq_migrating(p))
		flags |= ENQUEUE_MIGRATING;

	enqueue_dl_entity(dl_se, flags);
	/* 从这里开始 entity 级 CBS/EDF 状态已处理，下面只决定 task 是否进入迁移候选。 */

	if (dl_server(dl_se))
		return;

	if (task_is_blocked(p))
		return;

	if (dl_rq->curr == dl_se)
		return;

	if (!task_current(rq, p) && !dl_se->dl_throttled && p->nr_cpus_allowed > 1)
		enqueue_pushable_dl_task(rq, p);
}

/* 先结算当前执行，再同步移出 EDF 与 push 两棵树。 */
/*
 * dequeue_task_dl() - 从 DL 调度类撤下 task 并完成执行结算
 *
 * 业务背景：阻塞、迁移、策略切换和调度切换都必须先结算 donor 的 runtime，再撤销
 * EDF/push 可见性。入参：rq 是持锁队列，p 是稳定 task，flags 描述离队原因；出参/
 * 返回：返回 true 表示 DL 类离队处理完成，可能启动 0-lag timer 或保留 throttle 状态。
 * 注意事项：migrating 需要把 this_bw/running_bw ownership 一起移出旧 rq；普通离队
 * 后若仍未 throttle 才清 push 候选，不能在 update_curr_dl() 前删除节点而丢失执行时间。
 */
static bool dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	update_curr_dl(rq);

	if (task_on_rq_migrating(p))
		flags |= DEQUEUE_MIGRATING;

	dequeue_dl_entity(&p->dl, flags);
	if (!p->dl.dl_throttled && !dl_server(&p->dl))
		dequeue_pushable_dl_task(rq, p);

	return true;
}

/*
 * Yield task semantic for -deadline tasks is:
 *
 *   get off from the CPU until our next instance, with
 *   a new runtime. This is of little use now, since we
 *   don't have a bandwidth reclaiming mechanism. Anyway,
 *   bandwidth reclaiming is planned for the future, and
 *   yield_task_dl will indicate that some spare budget
 *   is available for other task instances to use it.
 */
/* yield 把本实例预算置零并等待下一次 replenish，而不是同 deadline 内轮转。 */
/*
 * yield_task_dl() - 将主动让出解释为本实例耗尽并等待下一 period
 *
 * 业务背景：deadline 任务 yield 不是普通时间片轮转；它声明当前实例不再消费，
 * 由 replenish timer 在下一合法时刻提供新预算。入参：rq 是持锁当前队列；出参/返回：
 * 无直接返回值，设置 donor->dl_yielded、立即更新当前预算并标记 rq clock 已同步。
 * 注意事项：runtime 置零由 update_curr_dl() 统一触发 dequeue/timer；clock_skip_update
 * 防止 schedule() 再做一次微小更新，改变顺序会造成重复记账。
 */
static void yield_task_dl(struct rq *rq)
{
	/*
	 * We make the task go to sleep until its current deadline by
	 * forcing its runtime to zero. This way, update_curr_dl() stops
	 * it and the bandwidth timer will wake it up and will give it
	 * new scheduling parameters (thanks to dl_yielded=1).
	 */
	/* 置 yielded 后 update_curr_dl() 会把预算视为耗尽，timer 到下一实例再恢复新参数。 */
	rq->donor->dl.dl_yielded = 1;

	update_rq_clock(rq);
	update_curr_dl(rq);
	/*
	 * Tell update_rq_clock() that we've just updated,
	 * so we don't do microscopic update in schedule()
	 * and double the fastpath cost.
	 */
	/* 已手动推进 rq 时钟，标记 skip 避免 schedule() 再做微小更新并重复快路径成本。 */
	rq_clock_skip_update(rq);
}

/*
 * dl_task_is_earliest_deadline() - 判断 task 在目标 rq 是否可成为严格 EDF 树首
 *
 * 入参 p/rq 为迁移搜索中的借用快照；目标空或 p deadline 严格更早时返回 true，
 * 相等也返回 false。函数无副作用；未持目标 rq 锁时调用者必须在双锁后再次验证。
 */
static inline bool dl_task_is_earliest_deadline(struct task_struct *p,
						 struct rq *rq)
{
	/* 空目标 rq 总可接受；非空时只有严格更早的 deadline 才改善 EDF 顺序。 */
	return (!rq->dl.dl_nr_running ||
		dl_time_before(p->dl.deadline,
			       rq->dl.earliest_dl.curr));
}

static int find_later_rq(struct task_struct *task);

/* 唤醒时优先保留更紧迫任务；当前不可迁移或容量不足才查询较晚 deadline rq。 */
/*
 * select_task_rq_dl() - 为唤醒的 deadline task 选择目标 CPU
 *
 * 业务背景：更早 deadline 的任务应尽量留在当前 CPU 抢占，但若当前 donor 不可移动、
 * 新 task 可移动或异构 CPU 容量不足，就应寻找较晚 deadline 的可用 rq。入参：p 是
 * 被唤醒 task，cpu 是初始 CPU，flags 含 WF_TTWU 时才执行选择；出参/返回：返回
 * 选定 CPU 编号，任务/CPU ownership 仍由唤醒路径持有。注意事项：curr/donor 使用
 * RCU 下 READ_ONCE 快照，find_later_rq() 的候选需再次验证最早 deadline；RCU 只保护
 * 指针生命周期，不冻结 rq 字段。
 */
static int
select_task_rq_dl(struct task_struct *p, int cpu, int flags)
{
	struct task_struct *curr, *donor;
	bool select_rq;
	struct rq *rq;

	if (!(flags & WF_TTWU))
		return cpu;

	/* 非加锁快照只用于决定是否搜索，最终迁移仍由唤醒核心在锁下提交。 */
	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr); /* unlocked access */
	/* 这是 RCU 读侧的未加 rq 锁快照，只能用于候选决策，不能承担状态提交。 */
	donor = READ_ONCE(rq->donor);

	/*
	 * If we are dealing with a -deadline task, we must
	 * decide where to wake it up.
	 * If it has a later deadline and the current task
	 * on this rq can't move (provided the waking task
	 * can!) we prefer to send it somewhere else. On the
	 * other hand, if it has a shorter deadline, we
	 * try to make it stay here, it might be important.
	 */
	/* donor 较紧迫/不可迁移而唤醒 task 可迁移时搜索别处；唤醒者更紧迫则优先留本 CPU。 */
	select_rq = unlikely(dl_task(donor)) &&
		    (curr->nr_cpus_allowed < 2 ||
		     !dl_entity_preempt(&p->dl, &donor->dl)) &&
		    p->nr_cpus_allowed > 1;

	/*
	 * Take the capacity of the CPU into account to
	 * ensure it fits the requirement of the task.
	 */
	/* 异构系统上，即使 deadline 顺序允许留下，容量不匹配也要触发搜索。 */
	if (sched_asym_cpucap_active())
		select_rq |= !dl_task_fits_capacity(p, cpu);

	/* find_later_rq 返回提示候选；再次比较目标树首后才接受。 */
	if (select_rq) {
		int target = find_later_rq(p);

		if (target != -1 &&
		    dl_task_is_earliest_deadline(p, cpu_rq(target)))
			cpu = target;
	}
	rcu_read_unlock();

	return cpu;
}

/* TASK_WAKING 迁移时旧 rq 锁下取消 inactive 状态并撤销旧 rq 带宽。 */
/*
 * migrate_task_rq_dl() - 在 TASK_WAKING 迁移边界清理旧 rq 记账
 *
 * 业务背景：try_to_wake_up() 已调用 set_task_cpu() 并持有 pi_lock，但尚未持旧 rq 锁；
 * deadline 的 non-contending 状态和 this_bw 必须在旧 rq 上先摘除。入参：p 是 TASK_WAKING
 * 的稳定 task，new_cpu 是目标 CPU（本实现不直接使用）；出参/返回：无直接返回值，
 * 取消 inactive timer、清除 active utilization 并撤销旧 rq reservation。注意事项：
 * 只有 TASK_WAKING 才执行；rq 锁是补足 set_task_cpu() 后的保护，timer 回调竞态依赖
 * 先清 dl_non_contending 再 cancel。
 */
static void migrate_task_rq_dl(struct task_struct *p, int new_cpu __maybe_unused)
{
	struct rq_flags rf;
	struct rq *rq;

	if (READ_ONCE(p->__state) != TASK_WAKING)
		return;

	/* 此时 task_cpu 仍指向需要清账的旧 rq；入口状态门避免误扣普通迁移。 */
	rq = task_rq(p);
	/*
	 * Since p->state == TASK_WAKING, set_task_cpu() has been called
	 * from try_to_wake_up(). Hence, p->pi_lock is locked, but
	 * rq->lock is not... So, lock it
	 */
	/* TASK_WAKING 已持 pi_lock 且 set_task_cpu 完成，但 rq 锁未持有，需显式补锁。 */
	rq_lock(rq, &rf);
	/* non-contending ownership 若尚未由 timer 消费，则由迁移路径抢先归还。 */
	if (p->dl.dl_non_contending) {
		update_rq_clock(rq);
		sub_running_bw(&p->dl, &rq->dl);
		p->dl.dl_non_contending = 0;
		/*
		 * If the timer handler is currently running and the
		 * timer cannot be canceled, inactive_task_timer()
		 * will see that dl_not_contending is not set, and
		 * will not touch the rq's active utilization,
		 * so we are still safe.
		 */
		/* 回调已运行时会看到 non_contending 清零，不会再次修改旧 rq 的 active utilization。 */
		cancel_inactive_timer(&p->dl);
	}
	/* this_bw 总是属于旧 rq，迁移前无条件撤销，目标 enqueue 再补入。 */
	sub_rq_bw(&p->dl, &rq->dl);
	rq_unlock(rq, &rf);
}

/*
 * check_preempt_equal_dl() - 处理相同 deadline 下的迁移与重调度决策
 *
 * 业务背景：两个实体 deadline 相同，直接抢占不能改善 EDF 顺序；若当前可迁移且
 * 唤醒 task 不适合留下，调度器更应尝试 push/pull。入参：rq 是持锁队列，p 是已
 * 唤醒借用 task；出参/返回：无直接返回值，必要时设置 resched。注意事项：当前 task
 * 只有一个允许 CPU 时不能迁移；cpudl_find() 同时判断候选可迁移性，不能只看 deadline。
 */
static void check_preempt_equal_dl(struct rq *rq, struct task_struct *p)
{
	/*
	 * Current can't be migrated, useless to reschedule,
	 * let's hope p can move out.
	 */
	/* current 无迁移目标时重调度无益，只能期待唤醒 task 被 push 到其他 CPU。 */
	if (rq->curr->nr_cpus_allowed == 1 ||
	    !cpudl_find(&rq->rd->cpudl, rq->donor, NULL))
		return;

	/*
	 * p is migratable, so let's not schedule it and
	 * see if it is pushed or pulled somewhere else.
	 */
	/* p 自身可迁移且 cpudl 有目标时先不抢占，让 push/pull 解决相同 deadline 冲突。 */
	if (p->nr_cpus_allowed != 1 &&
	    cpudl_find(&rq->rd->cpudl, p, NULL))
		return;

	resched_curr(rq);
}

/* pick 前可暂时放开 rq 锁执行 pull，重新加锁后必须以当前 donor 重新判断。 */
/*
 * balance_dl() - 在选择下一个 task 前拉取更合适的 deadline task
 *
 * 业务背景：当前 donor 不在 DL EDF 树而 rq 仍在线时，本地可能有可拉取的 DL 工作。
 * 入参：rq 是当前持锁队列，rf 是可重新 pin 的锁状态；出参/返回：返回 stop 或 DL
 * 是否仍 runnable，供调度核心决定下一选择。注意事项：pull 期间 rq 锁暂时 unpin，
 * donor 可能改变，所以不能跨锁释放复用旧 p；on_cpu 且中断/抢占约束保证当前 task
 * 不会被同时选走。
 */
static int balance_dl(struct rq *rq, struct rq_flags *rf)
{
	/*
	 * Note, rq->donor may change during rq lock drops,
	 * so don't re-use prev across lock drops
	 */
	/* rq 锁释放期间 donor 可变，因此旧 prev 快照不能跨 pull 复用。 */
	struct task_struct *p = rq->donor;

	if (!on_dl_rq(&p->dl) && need_pull_dl_task(rq, p)) {
		/*
		 * This is OK, because current is on_cpu, which avoids it being
		 * picked for load-balance and preemption/IRQs are still
		 * disabled avoiding further scheduler activity on it and we've
		 * not yet started the picking loop.
		 */
		/* current 仍 on_cpu 且中断/抢占关闭，在 pick loop 前临时 unpin 不会被并发选走。 */
		rq_unpin_lock(rq, rf);
		pull_dl_task(rq);
		rq_repin_lock(rq, rf);
	}

	return sched_stop_runnable(rq) || sched_dl_runnable(rq);
}

/*
 * Only called when both the current and waking task are -deadline
 * tasks.
 */
/* 该回调只在当前和唤醒 task 都属于 DL 类时进入。 */
/* 更早 absolute deadline 立即抢占；相同 deadline 尝试通过迁移避免无效切换。 */
/*
 * wakeup_preempt_dl() - 比较唤醒 task 与当前 donor 的 EDF 优先级
 *
 * 业务背景：DL 类只应被更早 absolute deadline 的 DL task 抢占；相同 deadline 则交给
 * 迁移策略避免无收益切换。入参：rq 是持锁队列，p 是唤醒 task，flags 是抢占事件
 * 标志；出参/返回：无直接返回值，必要时设置 resched。注意事项：非 DL sched_class
 * 的 task 只会被 stop class 抢占，不能在这里 push；比较使用 rq->donor 而非盲信 curr，
 * 因为 server 可能代表实际当前工作。
 */
static void wakeup_preempt_dl(struct rq *rq, struct task_struct *p, int flags)
{
	/*
	 * Can only get preempted by stop-class, and those should be
	 * few and short lived, doesn't really make sense to push
	 * anything away for that.
	 */
	/* 非 DL sched_class 的唤醒者只涉及更高 stop-class 情况，无需为短暂执行做 push。 */
	if (p->sched_class != &dl_sched_class)
		return;

	/* 严格更早立即请求重调度；比较 donor 可覆盖 server/proxy 的实际预算拥有者。 */
	if (dl_entity_preempt(&p->dl, &rq->donor->dl)) {
		resched_curr(rq);
		return;
	}

	/*
	 * In the unlikely case current and p have the same deadline
	 * let us try to decide what's the best thing to do...
	 */
	/* deadline 相等且尚未请求重调度时，进一步用迁移可行性决定是否切换。 */
	if ((p->dl.deadline == rq->donor->dl.deadline) &&
	    !test_tsk_need_resched(rq->curr))
		check_preempt_equal_dl(rq, p);
}

#ifdef CONFIG_SCHED_HRTICK
/* hrtick 以剩余 runtime 为相对期限，精确触发预算结算；rq 锁由调用者持有。 */
/* 入参 rq/dl_se 均为锁下借用对象；无返回值，不取得 timer 或实体 ownership。 */
static void start_hrtick_dl(struct rq *rq, struct sched_dl_entity *dl_se)
{
	hrtick_start(rq, dl_se->runtime);
}
#else /* !CONFIG_SCHED_HRTICK: */
/* 未配置 hrtick 时保留同一调用接口，周期 tick 负责后续预算检查。 */
/* 入参仅用于维持接口一致；无返回值、无副作用，预算耗尽由普通 tick 路径发现。 */
static void start_hrtick_dl(struct rq *rq, struct sched_dl_entity *dl_se)
{
}
#endif /* !CONFIG_SCHED_HRTICK */

/*
 * DL keeps current in tree, because ->deadline is not typically changed while
 * a task is runnable.
 */
/* 当前任务仍留在 EDF 树，但必须从 push 树移除；首次切入再安排均衡/hrtick。 */
/*
 * set_next_task_dl() - 把选中的 DL task 发布为 rq 当前 donor
 *
 * 业务背景：DL 当前实体保留在 EDF 树以维护排序，但运行中的 task 不能作为自己的
 * push 候选。入参：rq 是持锁队列，p 是被选 task，first 表示本次是否首次切入；出参/
 * 返回：无直接返回值，写入 dl_rq->curr、exec_start，首次切入时更新 PELT、排队 push
 * callback 并按剩余 runtime 启动 hrtick。注意事项：先清 pushable 再设置 curr，发布后
 * 其他路径会认为 p 不可迁移；`dl_rq->curr` 与 rq->donor 的一致性由调度核心共同维护。
 */
static void set_next_task_dl(struct rq *rq, struct task_struct *p, bool first)
{
	struct sched_dl_entity *dl_se = &p->dl;
	struct dl_rq *dl_rq = &rq->dl;

	p->se.exec_start = rq_clock_task(rq);
	/* 若实体仍在树中，切入 CPU 结束其 wait 区间；throttled 实体不会进入此分支。 */
	if (on_dl_rq(&p->dl))
		update_stats_wait_end_dl(dl_rq, dl_se);

	/* You can't push away the running task */
	/* 正在运行的 task 不能同时作为迁移候选，先从 pushable 树撤下。 */
	dequeue_pushable_dl_task(rq, p);

	/* 清除迁移候选后再发布 curr，避免同一 task 同时被运行和 push。 */
	WARN_ON_ONCE(dl_rq->curr);
	dl_rq->curr = dl_se;

	/* 非首次调用仅修复当前指针；PELT、balance callback 和 hrtick 已由切入路径安排。 */
	if (!first)
		return;

	if (rq->donor->sched_class != &dl_sched_class)
		update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 0);

	deadline_queue_push_tasks(rq);

	if (hrtick_enabled_dl(rq))
		start_hrtick_dl(rq, &p->dl);
}

/* 缓存红黑树最左节点即 EDF 选择；空树返回 NULL。 */
/*
 * pick_next_dl_entity() - 取得 EDF 树最左实体
 *
 * 业务背景：absolute deadline 是红黑树排序键，最左节点就是本 rq 的 earliest entity。
 * 入参：dl_rq 是持锁借用队列；出参/返回：返回借用 sched_dl_entity 指针，空树返回
 * NULL，不增加引用、不修改树。注意事项：调用者必须在 rq 锁下使用返回指针；它可能
 * 是普通 task，也可能是 server，后续必须按 dl_server() 分派。
 */
static struct sched_dl_entity *pick_next_dl_entity(struct dl_rq *dl_rq)
{
	struct rb_node *left = rb_first_cached(&dl_rq->root);

	if (!left)
		return NULL;

	return __node_2_dle(left);
}

/*
 * __pick_next_task_dl - Helper to pick the next -deadline task to run.
 * @rq: The runqueue to pick the next task from.
 */
/* server 无 client 时先停 server 再重选，保证不会返回虚假的 runnable 实体。 */
/*
 * __pick_task_dl()/pick_task_dl() - 从 EDF 树选择实际要运行的 task
 *
 * 业务背景：树节点既可能是普通 task，也可能是需要调用 client 选择回调的 server；
 * server 没有 client 时必须 stop 后重新扫描，不能把空 server 当成 runnable。入参：
 * rq 是持锁队列，rf 是可传给 server 回调的锁状态；出参/返回：返回普通 task 或
 * server_pick_task() 选择的 task，队列为空返回 NULL，并把 rq->dl_server 记录为当前
 * server。注意事项：`again` 重新读取树首以跳过停止的 server；返回的是借用 task，
 * 生命周期由 rq 锁和调度核心保证。
 */
static struct task_struct *__pick_task_dl(struct rq *rq, struct rq_flags *rf)
{
	struct sched_dl_entity *dl_se;
	struct dl_rq *dl_rq = &rq->dl;
	struct task_struct *p;

again:
	/* 停掉无 client server 后回到此处重新读取树，避免使用已删除的 leftmost。 */
	if (!sched_dl_runnable(rq))
		return NULL;

	dl_se = pick_next_dl_entity(dl_rq);
	WARN_ON_ONCE(!dl_se);

	/* server 节点通过回调借出真实 task；普通节点可直接 container_of。 */
	if (dl_server(dl_se)) {
		p = dl_se->server_pick_task(dl_se, rf);
		if (!p) {
			/* client 集合为空时 server 失去 runnable 含义，stop 后必须重新扫描。 */
			dl_server_stop(dl_se);
			goto again;
		}
		/* 记录本次 donor 背后的 server，执行记账和后续 stop 可定位正确 reservation。 */
		rq->dl_server = dl_se;
	} else {
		p = dl_task_of(dl_se);
	}

	return p;
}

/*
 * pick_task_dl() - 调度类对 DL 选择器的公开包装
 *
 * 入参 rq 为持锁运行队列，rf 携带可由 server 回调使用的锁状态；返回下一 task 的借用
 * 指针，或在无可运行实体时返回 NULL。所有重试、server stop 和 donor 选择由内部实现完成。
 */
static struct task_struct *pick_task_dl(struct rq *rq, struct rq_flags *rf)
{
	/* 调度类接口薄封装，返回值和锁/引用语义完全继承 __pick_task_dl()。 */
	return __pick_task_dl(rq, rf);
}

/* 结算预算并清空 curr；仍可运行且可迁移的前任务重新加入 push 树。 */
/*
 * put_prev_task_dl() - 结束 DL task 的当前执行身份并恢复等待/迁移状态
 *
 * 入参 rq 为持锁队列，p 为前 donor，next 由通用接口传入但本实现不使用；无返回值。
 * 先开启新的 wait 区间、结算 runtime 和 PELT，再清 dl_rq->curr；仍排队且可迁移的
 * task 重回 pushable 树。blocked task 已不具备迁移资格，必须提前返回。
 */
static void put_prev_task_dl(struct rq *rq, struct task_struct *p, struct task_struct *next)
{
	struct sched_dl_entity *dl_se = &p->dl;
	struct dl_rq *dl_rq = &rq->dl;

	if (on_dl_rq(dl_se))
		update_stats_wait_start_dl(dl_rq, dl_se);

	/* 执行结算必须发生在 curr ownership 清除之前，update_curr_dl 依赖该身份。 */
	update_curr_dl(rq);

	update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 1);

	WARN_ON_ONCE(dl_rq->curr != dl_se);
	dl_rq->curr = NULL;

	/* proxy/block 状态不进入 push 树；其后续唤醒由 enqueue 路径重新发布。 */
	if (task_is_blocked(p))
		return;

	if (on_dl_rq(dl_se) && p->nr_cpus_allowed > 1)
		enqueue_pushable_dl_task(rq, p);
}

/*
 * scheduler tick hitting a task of our scheduling class.
 *
 * NOTE: This function can be called remotely by the tick offload that
 * goes along full dynticks. Therefore no local assumption can be made
 * and everything must be accessed through the @rq and @curr passed in
 * parameters.
 */
/* tick offload 可远程调用本函数，因此只能使用传入 rq/p，不能读取本地 CPU 隐式状态。 */
/* tick 可能远程执行，只依赖传入 rq/p；扣预算后仅给仍最早实体续 hrtick。 */
/*
 * task_tick_dl() - 周期性结算当前 DL runtime 并维护精确预算 tick
 *
 * 入参 rq/p 来自调度核心，queued 表示 task 仍排队；无返回值。远程 tick 场景禁止
 * 使用本地 CPU 假设。update_curr_dl() 可能 throttle 或改变树首，只有仍有预算、仍
 * queued 且仍最左时才能重启 hrtick，否则 NEED_RESCHED 路径接管。
 */
/*
 * task_tick_dl() - 在周期 tick 边沿结算 DL donor 并维护精确预算定时
 *
 * 入参 rq/p 在 rq 锁下借用，queued 表示 p 是否仍在运行队列；无返回值。先结算 runtime
 * 与 PELT；仅当 hrtick 可用、p 仍排队且仍是 EDF 树首时，才按剩余预算重启 hrtick。
 * update_curr_dl() 可能已触发限流或重调度，因此不能在结算前缓存这些条件。
 */
static void task_tick_dl(struct rq *rq, struct task_struct *p, int queued)
{
	update_curr_dl(rq);

	update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 1);
	/*
	 * Even when we have runtime, update_curr_dl() might have resulted in us
	 * not being the leftmost task anymore. In that case NEED_RESCHED will
	 * be set and schedule() will start a new hrtick for the next task.
	 */
	/* update_curr 后若 p 不再是树首，NEED_RESCHED 会让 schedule() 为新 task 重建 hrtick。 */
	if (hrtick_enabled_dl(rq) && queued && p->dl.runtime > 0 &&
	    is_leftmost(&p->dl, &rq->dl))
		start_hrtick_dl(rq, &p->dl);
}

/*
 * task_fork_dl() - 保持调度类统一 fork 回调接口
 *
 * 入参 p 是 sched_fork() 路径借用的 task；无返回值且无副作用。SCHED_DEADLINE fork 已在
 * 更早的通用路径拒绝，因此这里不能创建或复制 CBS/timer/bandwidth ownership。
 */
static void task_fork_dl(struct task_struct *p)
{
	/* DL task 在 sched_fork() 已被禁止 fork；该类回调仅满足统一接口，不产生子状态。 */
	/*
	 * SCHED_DEADLINE tasks cannot fork and this is achieved through
	 * sched_fork()
	 */
	/* SCHED_DEADLINE 的 fork 禁止由 sched_fork() 实现，本回调不需额外处理。 */
}

/* Only try algorithms three times */
/* 迁移目标在并发下最多重试三轮，限制双 rq 锁竞争和无界扫描。 */
#define DL_MAX_TRIES 3

/*
 * Return the earliest pushable rq's task, which is suitable to be executed
 * on the CPU, NULL otherwise:
 */
/* 遍历源 rq 的 deadline 有序候选，返回首个亲和性和容量均允许的任务。 */
/*
 * pick_earliest_pushable_dl_task() - 从源 rq 找到可在目标 CPU 执行的最早候选
 *
 * 业务背景：push/pull 不能只取树首，因为 affinity、migration disabled 和异构容量
 * 可能淘汰它。入参：rq 是持锁源队列，cpu 是目标 CPU；出参/返回：按 deadline 顺序
 * 返回第一个 task_on_rq 且 task_is_pushable() 的借用 task，找不到返回 NULL，不修改树。
 * 注意事项：最多由调用者控制重试次数；返回 task 仍需在双 rq 锁下重新验证，不能把
 * 扫描快照当作迁移 ownership。
 */
static struct task_struct *pick_earliest_pushable_dl_task(struct rq *rq, int cpu)
{
	struct task_struct *p = NULL;
	struct rb_node *next_node;

	if (!has_pushable_dl_tasks(rq))
		return NULL;

	/* 树按 deadline 升序，逐项跳过亲和性、容量或 migration-disabled 不合格项。 */
	next_node = rb_first_cached(&rq->dl.pushable_dl_tasks_root);
	while (next_node) {
		p = __node_2_pdl(next_node);

		if (task_is_pushable(rq, p, cpu))
			return p;

		next_node = rb_next(next_node);
	}

	return NULL;
}

/* Access rule: must be called on local CPU with preemption disabled */
/* 访问该 per-CPU 临时 mask 必须在本 CPU 且禁止抢占，防止同 CPU 调用者并发复用。 */
static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask_dl);

/*
 * find_later_rq() - 为可迁移 deadline task 搜索更适合的目标 CPU
 *
 * 业务背景：迁移应让 task 离开一个当前 donor deadline 更早/相同的 rq，并尽量保持
 * cache affinity；cpudl 先按拓扑和 deadline 筛选，再用 sched_domain 选择。入参：
 * task 是稳定借用 task，调用要求本地 CPU 且禁止抢占；出参/返回：返回候选 CPU 编号，
 * 无合适 CPU 返回 -1，不锁目标 rq。注意事项：per-CPU mask 和 sched_domain 在 RCU
 * 下读取；返回后 affinity、online 状态和 earliest deadline 都必须重新确认。
 */
static int find_later_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *later_mask = this_cpu_cpumask_var_ptr(local_cpu_mask_dl);
	int this_cpu = smp_processor_id();
	int cpu = task_cpu(task);

	/* Make sure the mask is initialized first */
	/* 初始化失败时 per-CPU mask 可为 NULL，此时没有安全的搜索缓冲区。 */
	if (unlikely(!later_mask))
		return -1;

	/* 单 CPU affinity 没有迁移自由度，不进行 cpudl 或 topology 搜索。 */
	if (task->nr_cpus_allowed == 1)
		return -1;

	/*
	 * We have to consider system topology and task affinity
	 * first, then we can look for a suitable CPU.
	 */
	/* cpudl 同时应用 task affinity，并把 deadline 更晚的 CPU 写入 per-CPU 临时 mask。 */
	if (!cpudl_find(&task_rq(task)->rd->cpudl, task, later_mask))
		return -1;

	/*
	 * If we are here, some targets have been found, including
	 * the most suitable which is, among the runqueues where the
	 * current tasks have later deadlines than the task's one, the
	 * rq with the latest possible one.
	 *
	 * Now we check how well this matches with task's
	 * affinity and system topology.
	 *
	 * The last CPU where the task run is our first
	 * guess, since it is most likely cache-hot there.
	 */
	/* 最近运行 CPU 优先，尽可能保留热缓存而不牺牲 deadline 条件。 */
	if (cpumask_test_cpu(cpu, later_mask))
		return cpu;
	/*
	 * Check if this_cpu is to be skipped (i.e., it is
	 * not in the mask) or not.
	 */
	/* 若当前 CPU 不在候选掩码中，就把它标记为不可用，后续不能再走本地优先分支。 */
	if (!cpumask_test_cpu(this_cpu, later_mask))
		this_cpu = -1;

	/* sched_domain 拓扑由 RCU 保护；先尝试本 CPU，再在最近 WAKE_AFFINE 域分布候选。 */
	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/* 先检查当前 CPU 是否落在这个亲和域，命中可避免一次迁移。 */
			/*
			 * If possible, preempting this_cpu is
			 * cheaper than migrating.
			 */
			/* 若当前 CPU 同属该亲和域，直接本地抢占通常比迁移 task 的缓存与锁成本更低。 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			/* 否则在当前域与 later_mask 的交集中分布选择，避免集中到固定 CPU。 */
			best_cpu = cpumask_any_and_distribute(later_mask,
							      sched_domain_span(sd));
			/*
			 * Last chance: if a CPU being in both later_mask
			 * and current sd span is valid, that becomes our
			 * choice. Of course, the latest possible CPU is
			 * already under consideration through later_mask.
			 */
			/* 最后在本调度域与合法候选的交集中选一个 CPU；later_mask 已保证 deadline 条件。 */
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * At this point, all our guesses failed, we just return
	 * 'something', and let the caller sort the things out.
	 */
	/* 拓扑偏好均失败后退化到任一合法候选，正确性仍由 later_mask 保证。 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any_distribute(later_mask);
	if (cpu < nr_cpu_ids)
		return cpu;

	return -1;
}

/*
 * pick_next_pushable_dl_task() - 取得当前 rq 不在 CPU 上的首个 push 候选
 *
 * 业务背景：proxy-exec 等场景可能让树中 task 暂时仍在 CPU 上，push 只能选择非 on_cpu
 * 的普通 DL task。入参：rq 是持锁队列；出参/返回：返回借用 task 或 NULL，不修改树。
 * 注意事项：返回前的 WARN 检查记录 rq/CPU/队列/策略不变量；调用者若释放 rq 锁，必须
 * 先取得 task 引用并在迁移时再次校验。
 */
static struct task_struct *pick_next_pushable_dl_task(struct rq *rq)
{
	struct task_struct *i, *p = NULL;
	struct rb_node *next_node;

	if (!has_pushable_dl_tasks(rq))
		return NULL;

	/* proxy-exec 可能让 push 树节点仍 on_cpu，逐项跳过直到找到可移动对象。 */
	next_node = rb_first_cached(&rq->dl.pushable_dl_tasks_root);
	while (next_node) {
		i = __node_2_pdl(next_node);
		/* make sure task isn't on_cpu (possible with proxy-exec) */
		/* proxy-exec 下树节点可能仍在 CPU 上，此处必须跳过。 */
		/* on_cpu 节点仍保留树 ownership，但此刻不能由迁移代码接管。 */
		if (!task_on_cpu(rq, i)) {
			p = i;
			break;
		}

		next_node = rb_next(next_node);
	}

	if (!p)
		return NULL;

	/* rq 锁下验证 push 树不变量，任何失败都表示候选维护路径已损坏。 */
	WARN_ON_ONCE(rq->cpu != task_cpu(p));
	WARN_ON_ONCE(task_current(rq, p));
	WARN_ON_ONCE(p->nr_cpus_allowed <= 1);

	WARN_ON_ONCE(!task_on_rq_queued(p));
	WARN_ON_ONCE(!dl_task(p));

	return p;
}

/* Locks the rq it finds */
/* 成功返回时目标 rq 已与源 rq 双锁；失败返回 NULL，锁 ownership 不增加。 */
/*
 * find_lock_later_rq() - 搜索并锁住可接收 task 的目标 rq
 *
 * 业务背景：CPU 迁移存在锁释放和并发变化，候选 CPU 不能只由一次 cpudl 查询决定。
 * 入参：task 是待迁移借用 task，rq 是当前持锁源队列；出参/返回：成功返回已与 rq
 * 成对加锁的目标 rq，失败返回 NULL。注意事项：最多 DL_MAX_TRIES 次；双锁过程中
 * 必须重新验证 migration_disabled、affinity、task 是否仍在源 rq、是否 on_cpu、是否
 * 仍是 DL 及 push 树首，否则解锁重试；返回的锁 ownership 交给调用者释放。
 */
static struct rq *find_lock_later_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *later_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < DL_MAX_TRIES; tries++) {
		/* 第一阶段无锁查 cpudl 候选；当前 CPU 或无候选可立即终止。 */
		cpu = find_later_rq(task);

		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		later_rq = cpu_rq(cpu);

		/* 加双锁前的快速淘汰减少无意义锁竞争，但不能替代锁后验证。 */
		if (!dl_task_is_earliest_deadline(task, later_rq)) {
			/*
			 * Target rq has tasks of equal or earlier deadline,
			 * retrying does not release any lock and is unlikely
			 * to yield a different result.
			 */
			/* 目标 rq 已有同等或更早任务，且此处未释放锁，立即重试不会得到新的候选状态。 */
			later_rq = NULL;
			break;
		}

		/* Retry if something changed. */
		/* 返回非零表示源锁曾释放，task/rq/affinity 的所有快照都可能过期。 */
		if (double_lock_balance(rq, later_rq)) {
			/*
			 * double_lock_balance had to release rq->lock, in the
			 * meantime, task may no longer be fit to be migrated.
			 * Check the following to ensure that the task is
			 * still suitable for migration:
			 * 1. It is possible the task was scheduled,
			 *    migrate_disabled was set and then got preempted,
			 *    so we must check the task migration disable
			 *    flag.
			 * 2. The CPU picked is in the task's affinity.
			 * 3. For throttled task (dl_task_offline_migration),
			 *    check the following:
			 *    - the task is not on the rq anymore (it was
			 *      migrated)
			 *    - the task is not on CPU anymore
			 *    - the task is still a dl task
			 *    - the task is not queued on the rq anymore
			 * 4. For the non-throttled task (push_dl_task), the
			 *    check to ensure that this task is still at the
			 *    head of the pushable tasks list is enough.
			 */
			/* throttle 离线迁移和普通 push 使用不同的存活性/树首验证条件。 */
			if (unlikely(is_migration_disabled(task) ||
				     !cpumask_test_cpu(later_rq->cpu, &task->cpus_mask) ||
				     (task->dl.dl_throttled &&
				      (task_rq(task) != rq ||
				       task_on_cpu(rq, task) ||
				       !dl_task(task) ||
				       !task_on_rq_queued(task))) ||
				     (!task->dl.dl_throttled &&
				      task != pick_next_pushable_dl_task(rq)))) {

				/* 任一不变量失效都撤销双锁 ownership，本轮不再尝试陈旧候选。 */
				double_unlock_balance(rq, later_rq);
				later_rq = NULL;
				break;
			}
		}

		/*
		 * If the rq we found has no -deadline task, or
		 * its earliest one has a later deadline than our
		 * task, the rq is a good one.
		 */
		/* 双锁稳定后最终确认 task 在目标 rq 能成为严格最早实体。 */
		if (dl_task_is_earliest_deadline(task, later_rq))
			break;

		/* Otherwise we try again. */
		/* 最终验证失败就解双锁并清候选，下一轮重新查询 cpudl。 */
		double_unlock_balance(rq, later_rq);
		later_rq = NULL;
	}

	return later_rq;
}

/*
 * See if the non running -deadline tasks on this rq
 * can be sent to some other CPU where they can preempt
 * and start executing.
 */
/*
 * push_dl_task() - 将一个可迁移 deadline task 推送到更合适的 CPU
 *
 * 业务背景：本 rq 的非当前 DL task 可能阻塞其他 CPU 上更紧迫的工作；push 通过双 rq
 * 锁移动任务并请求目标 reschedule。入参：rq 是持锁源队列；出参/返回：移动成功返回
 * 1，否则返回 0。注意事项：先取得 next_task 引用以跨越 find_lock_later_rq 的锁释放；
 * 目标不存在时重新检查树首，若 task 仍在源树则交给其他 CPU pull，所有出口都要 put。
 */
static int push_dl_task(struct rq *rq)
{
	struct task_struct *next_task;
	struct rq *later_rq;
	int ret = 0;

	next_task = pick_next_pushable_dl_task(rq);
	if (!next_task)
		return 0;

retry:
	/* 每轮都基于当前候选重做 donor 与 migration-disabled 检查。 */
	/*
	 * If next_task preempts rq->curr, and rq->curr
	 * can move away, it makes sense to just reschedule
	 * without going further in pushing next_task.
	 */
	/* 若候选本应抢占当前任务且当前任务可迁移，先请求本地重调度，比把候选推出去更合理。 */
	if (dl_task(rq->donor) &&
	    dl_time_before(next_task->dl.deadline, rq->donor->dl.deadline) &&
	    rq->curr->nr_cpus_allowed > 1) {
		resched_curr(rq);
		return 0;
	}

	/* migration-disabled 或意外成为 current 的候选都不能由普通 push 移走。 */
	if (is_migration_disabled(next_task))
		return 0;

	if (WARN_ON(next_task == rq->curr))
		return 0;

	/* We might release rq lock */
	/* 后续 helper 可能释放源 rq 锁。 */
	/* 查找目标可能短暂释放源 rq 锁，引用保证 next_task 不被并发释放。 */
	get_task_struct(next_task);

	/* Will lock the rq it'll find */
	/* 成功时 helper 把找到的目标 rq 一并锁住。 */
	later_rq = find_lock_later_rq(next_task, rq);
	/* 目标失败后重新读取 push 树：原候选可能已迁移，不能沿用旧节点。 */
	if (!later_rq) {
		struct task_struct *task;

		/*
		 * We must check all this again, since
		 * find_lock_later_rq releases rq->lock and it is
		 * then possible that next_task has migrated.
		 */
		task = pick_next_pushable_dl_task(rq);
		/* 同一候选仍在树首说明当前无目的地，等待远端 pull 比本地忙重试更合适。 */
		if (task == next_task) {
			/*
			 * The task is still there. We don't try
			 * again, some other CPU will pull it when ready.
			 */
			/* 同一任务仍是树首，说明暂时没有合法目标；停止自旋，等待其他 CPU 主动 pull。 */
			goto out;
		}

		if (!task)
			/* No more tasks */
			/* pushable 树已空，结束本轮。 */
			goto out;

		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	/* 双 rq 锁下转移队列与带宽 ownership，再请求目标 CPU 尽快重选。 */
	move_queued_task_locked(rq, later_rq, next_task);
	ret = 1;

	resched_curr(later_rq);

	double_unlock_balance(rq, later_rq);

out:
	put_task_struct(next_task);

	return ret;
}

/*
 * push_dl_tasks() - 持续排空当前 rq 的可推送 deadline 候选
 *
 * 业务背景：一次 balance callback 可能需要迁移多个 task，直到 push_dl_task() 无法
 * 再移动为止。入参：rq 是调度回调提供的借用队列；出参/返回：无直接返回值。注意：
 * 循环依赖 push_dl_task() 的重新验证和返回值终止，不能直接遍历旧树节点。
 */
static void push_dl_tasks(struct rq *rq)
{
	/* push_dl_task() will return true if it moved a -deadline task */
	/* 每次成功迁移返回 true，循环继续；首次无法移动即终止。 */
	while (push_dl_task(rq))
		;
}

/*
 * pull_dl_task() - 从其他 overloaded rq 拉取最早且适合本 CPU 的 DL task
 *
 * 业务背景：本 CPU 空闲或 donor 非 DL 时，通过 dlo_mask 找源 rq，补齐本地 EDF 工作。
 * 入参：this_rq 是持锁目标队列；出参/返回：无直接返回值，可能双锁移动 task、设置
 * resched，或为 migration-disabled task 投递 stop work。注意事项：smp_rmb() 与
 * dl_set_overload() 的 smp_wmb() 配对，保证看到计数时也能看到 mask；mask/树首是
 * 允许短暂竞态的提示，双锁后必须重新取候选并检查 deadline/队列状态。
 */
static void pull_dl_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, cpu;
	struct task_struct *p, *push_task;
	bool resched = false;
	struct rq *src_rq;
	u64 dmin = LONG_MAX;

	if (likely(!dl_overloaded(this_rq)))
		return;

	/* 与发布端写屏障配对后，dlo_mask 中至少包含导致 count 非零的 rq。 */
	/*
	 * Match the barrier from dl_set_overloaded; this guarantees that if we
	 * see overloaded we must also see the dlo_mask bit.
	 */
	smp_rmb();

	/* dlo_mask 只是候选目录；每个源 rq 都必须在双锁下重新检查。 */
	for_each_cpu(cpu, this_rq->rd->dlo_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * It looks racy, and it is! However, as in sched_rt.c,
		 * we are fine with this.
		 */
		/* 本地最早 deadline 已早于源 rq 的次早提示时，源端不可能提供改善项。 */
		if (this_rq->dl.dl_nr_running &&
		    dl_time_before(this_rq->dl.earliest_dl.curr,
				   src_rq->dl.earliest_dl.next))
			continue;

		/* Might drop this_rq->lock */
		/* double_lock_balance 可能短暂释放目标 rq 锁。 */
		push_task = NULL;
		/* 从这里起源/目标队列稳定，可以安全检查树和提交 move。 */
		double_lock_balance(this_rq, src_rq);

		/*
		 * If there are no more pullable tasks on the
		 * rq, we're done with it.
		 */
		/* 源 rq 只剩正在执行的一个 DL 实体时，没有可从 pushable 树拉走的等待者。 */
		if (src_rq->dl.dl_nr_running <= 1)
			goto skip;

		/* 在锁内按目标 CPU affinity/capacity 重新取得最早可拉候选。 */
		p = pick_earliest_pushable_dl_task(src_rq, this_cpu);

		/*
		 * We found a task to be pulled if:
		 *  - it preempts our current (if there's one),
		 *  - it will preempt the last one we pulled (if any).
		 */
		/* 候选既要早于本轮已拉入任务的 deadline，也必须能成为目标 rq 的最早任务。 */
		if (p && dl_time_before(p->dl.deadline, dmin) &&
		    dl_task_is_earliest_deadline(p, this_rq)) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!task_on_rq_queued(p));

			/*
			 * Then we pull iff p has actually an earlier
			 * deadline than the current task of its runqueue.
			 */
			/* 若 p 比源 rq 的 donor 更早，它应留在源端抢占；仅在不妨碍源端 EDF 时才能拉走。 */
			if (dl_time_before(p->dl.deadline,
					   src_rq->donor->dl.deadline))
				goto skip;

			/* migration-disabled 任务交给源 CPU stop work；其余可在双锁下直接移动。 */
			if (is_migration_disabled(p)) {
				push_task = get_push_task(src_rq);
			} else {
				move_queued_task_locked(src_rq, this_rq, p);
				dmin = p->dl.deadline;
				resched = true;
			}

			/* Is there any other task even earlier? */
			/* 本次迁移后继续扫描其他源 rq，寻找 deadline 还早于当前 dmin 的任务。 */
		}
skip:
		double_unlock_balance(this_rq, src_rq);

		/* stop work 不能在持有目标 rq 锁时排队，故临时解锁并禁抢占保护本 CPU。 */
		if (push_task) {
			preempt_disable();
			raw_spin_rq_unlock(this_rq);
			stop_one_cpu_nowait(src_rq->cpu, push_cpu_stop,
					    push_task, &src_rq->push_work);
			preempt_enable();
			raw_spin_rq_lock(this_rq);
		}
	}

	/* 至少拉入一个更早实体后，统一触发一次本地 reschedule。 */
	if (resched)
		resched_curr(this_rq);
}

/*
 * Since the task is not running and a reschedule is not going to happen
 * anytime soon on its runqueue, we try pushing it away now.
 */
/*
 * task_woken_dl() - 在唤醒后没有即刻 reschedule 时主动触发 push
 *
 * 业务背景：wakeup task 可能已排队但当前 CPU 很快不会再次调度，不能让它长期压住
 * 更适合远端的工作。入参：rq 是持锁队列，p 是刚唤醒借用 task；出参/返回：无直接
 * 返回值，满足 donor/deadline/affinity 条件时执行 push_dl_tasks()。注意事项：当前
 * task 已在 CPU 或已有 NEED_RESCHED 时交给正常调度路径，避免重复迁移。
 */
static void task_woken_dl(struct rq *rq, struct task_struct *p)
{
	/* 只有不会由当前 schedule 自然处理、且存在迁移收益的唤醒 task 才主动 push。 */
	if (!task_on_cpu(rq, p) &&
	    !test_tsk_need_resched(rq->curr) &&
	    p->nr_cpus_allowed > 1 &&
	    dl_task(rq->donor) &&
	    (rq->curr->nr_cpus_allowed < 2 ||
	     !dl_entity_preempt(&p->dl, &rq->donor->dl))) {
		push_dl_tasks(rq);
	}
}

/* 跨 root_domain 改亲和性时目的端已预留，函数只归还源端后再发布 mask。 */
/*
 * set_cpus_allowed_dl() - 调整 DL task 亲和性并迁移 root-domain 带宽
 *
 * 业务背景：跨 exclusive cpuset/root_domain 的 affinity 变化会改变 admission accounting，
 * 必须先释放源域 reservation，再让通用路径发布新 mask；目的域容量已由 cpuset attach
 * 预留。入参：p 是稳定 DL task，ctx 描述新 affinity；出参/返回：无直接返回值，必要时
 * 在源 dl_bw 锁下扣减 reservation，随后调用 set_cpus_allowed_common()。注意事项：
 * 调用者持有 p->pi_lock/相关 rq 协议；只在新 mask 与旧 root span 无交集时移动带宽，
 * 否则不能重复扣减。
 */
static void set_cpus_allowed_dl(struct task_struct *p,
				struct affinity_context *ctx)
{
	struct rq *rq;

	WARN_ON_ONCE(!dl_task(p));

	rq = task_rq(p);
	/*
	 * Migrating a SCHED_DEADLINE task between exclusive
	 * cpusets (different root_domains) entails a bandwidth
	 * update. We already made space for us in the destination
	 * domain (see cpuset_can_attach()).
	 */
	/* 只有跨根域时源 total_bw ownership 才需在发布新 affinity 前归还。 */
	if (dl_task_needs_bw_move(p, ctx->new_mask)) {
		struct dl_bw *src_dl_b;

		src_dl_b = dl_bw_of(cpu_of(rq));
		/*
		 * We now free resources of the root_domain we are migrating
		 * off. In the worst case, sched_setattr() may temporary fail
		 * until we complete the update.
		 */
		/* 目的域已预留成功，此处归还源域份额；交接窗口内 sched_setattr 可能暂时因账面容量失败。 */
		raw_spin_lock(&src_dl_b->lock);
		__dl_sub(src_dl_b, p->dl.dl_bw, dl_bw_cpus(task_cpu(p)));
		raw_spin_unlock(&src_dl_b->lock);
	}

	/* 源域扣账完成后才允许通用代码切换 mask/task_cpu，维持可定位的 ownership。 */
	set_cpus_allowed_common(p, ctx);
}

/* 新 mask 与当前 root_domain 无交集才需要迁移 reservation 所有权。 */
/*
 * dl_task_needs_bw_move() - 判断 affinity 是否跨出当前 root_domain
 *
 * 业务背景：root-domain total_bw 只覆盖其 span，任务仍在同一 span 内改 mask 不需要
 * 重新记账。入参：p 是借用 task，new_mask 是只读新 CPU 集合；出参/返回：非 DL task
 * 返回 false，否则返回旧 root span 与新 mask 不相交的结果，无副作用。注意事项：
 * task_rq(p)->rd 需由调用者稳定，结果只是迁移决策快照，不能替代后续锁下检查。
 */
bool dl_task_needs_bw_move(struct task_struct *p,
			   const struct cpumask *new_mask)
{
	if (!dl_task(p))
		return false;

	return !cpumask_intersects(task_rq(p)->rd->span, new_mask);
}

/* Assumes rq->lock is held */
/* 在线 rq 重新发布 overload/cpudl 提示；rq 锁保护本地状态，root-domain 生命周期由外层保证。 */
/*
 * rq_online_dl() - 恢复上线 rq 的 DL 跨 CPU 提示
 *
 * 入参 rq 由调用者持锁；无返回值。按本地 overloaded/EDF 树状态重发 dlo_mask 与 cpudl，
 * 不迁移 task、不改 reservation。root-domain 生命周期由 hotplug 外层稳定。
 */
static void rq_online_dl(struct rq *rq)
{
	/* 重新上线只恢复全局提示；本地树与计数在离线迁移流程中已保持一致。 */
	if (rq->dl.overloaded)
		dl_set_overload(rq);

	if (rq->dl.dl_nr_running > 0)
		cpudl_set(&rq->rd->cpudl, rq->cpu, rq->dl.earliest_dl.curr);
	else
		cpudl_clear(&rq->rd->cpudl, rq->cpu, true);
}

/* Assumes rq->lock is held */
/* 下线 rq 撤销 overload/cpudl 发布，但不在此函数迁移 task；迁移由 offline 路径负责。 */
/*
 * rq_offline_dl() - 撤销下线 rq 的 DL 跨 CPU 提示
 *
 * 入参 rq 在 rq 锁下稳定；无返回值。清 overload 和 cpudl 可见性，但 task/带宽迁移
 * 由更高层 hotplug 路径完成，不能把本函数当作完整下线事务。
 */
static void rq_offline_dl(struct rq *rq)
{
	/* 先撤销过载目录，避免其他 CPU 继续把该 rq 当作 pull 源。 */
	if (rq->dl.overloaded)
		dl_clear_overload(rq);

	cpudl_clear(&rq->rd->cpudl, rq->cpu, false);
}

/* 为每 CPU 分配迁移搜索临时 mask；节点亲和分配减少热路径远端访问。 */
/*
 * init_sched_dl_class() - 分配每 CPU 的 deadline 迁移临时 mask
 *
 * 业务背景：find_later_rq() 需要可复用的 per-CPU 候选 mask，避免热路径动态分配。
 * 入参：无；出参/返回：无直接返回值，为每个 possible CPU 按 NUMA node 分配并清零
 * mask。注意事项：GFP_KERNEL 允许初始化阶段睡眠；分配失败由 helper 语义处理，后续
 * 搜索必须把 NULL 当作无候选而不是解引用。
 */
void __init init_sched_dl_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i)
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask_dl, i),
					GFP_KERNEL, cpu_to_node(i));
}

/*
 * This function always returns a non-empty bitmap in @cpus. This is because
 * if a root domain has reserved bandwidth for DL tasks, the DL bandwidth
 * check will prevent CPU hotplug from deactivating all CPUs in that domain.
 */
/* 在 cpuset_mutex 下取得任务所属有效分区，isolcpus domain 使用隔离集合。 */
/*
 * dl_get_task_effective_cpus() - 计算 DL task 可用于 root-domain 选择的 CPU 集合
 *
 * 业务背景：DL reservation 要求所属 root_domain 仍有 active CPU；housekeeping domain
 * 隔离和 cpuset 分区会改变普通 affinity 的有效范围。入参：p 是 cpuset 稳定的借用 task，
 * cpus 是调用者提供的输出 mask；出参/返回：无直接返回值，填充非空有效 CPU 集合，
 * 不转移 mask ownership。注意事项：调用者必须持 cpuset_mutex；isolcpus=domain 的
 * task 走 def_root_domain 的反选路径，不能简单使用 p->cpus_ptr。
 */
static void dl_get_task_effective_cpus(struct task_struct *p, struct cpumask *cpus)
{
	const struct cpumask *hk_msk;

	hk_msk = housekeeping_cpumask(HK_TYPE_DOMAIN);
	/* 完全位于 domain-isolated 集合的 task 属于默认根域，用 active isolated CPU 表示。 */
	if (housekeeping_enabled(HK_TYPE_DOMAIN)) {
		if (!cpumask_intersects(p->cpus_ptr, hk_msk)) {
			/*
			 * CPUs isolated by isolcpu="domain" always belong to
			 * def_root_domain.
			 */
			/* domain 隔离 CPU 不进入普通调度域分区，统一归属于 def_root_domain。 */
			cpumask_andnot(cpus, cpu_active_mask, hk_msk);
			return;
		}
	}

	/*
	 * If a root domain holds a DL task, it must have active CPUs. So
	 * active CPUs can always be found by walking up the task's cpuset
	 * hierarchy up to the partition root.
	 */
	/* 普通分区沿 cpuset 层级向上寻找有效 partition root，结果由 cpuset_mutex 稳定。 */
	cpuset_cpus_allowed_locked(p, cpus);
}

/* The caller should hold cpuset_mutex */
/* cpuset 重建后在 pi_lock 与 dl_bw 锁下把普通 DL reservation 加入新 root_domain。 */
/*
 * dl_add_task_root_domain() - 在 cpuset/root-domain 重建后重新发布 task reservation
 *
 * 业务背景：cpuset 重新分区会让 DL task 需要从新的有效 root_domain 获得带宽归属。
 * 入参：p 是调用者已稳定的 task；出参/返回：无直接返回值，普通非 special DL task
 * 在 pi_lock 和目标 dl_bw 锁下重新加入 total_bw。注意事项：调用者持 cpuset_mutex，
 * 函数内部用临时 mask 找 active CPU；非 DL 或 special task 直接返回，CPU 集合为空
 * 属于违反 root-domain reservation 不变量的 BUG。
 */
void dl_add_task_root_domain(struct task_struct *p)
{
	struct rq_flags rf;
	struct rq *rq;
	struct dl_bw *dl_b;
	unsigned int cpu;
	struct cpumask *msk;

	raw_spin_lock_irqsave(&p->pi_lock, rf.flags);
	/* pi_lock 稳定策略和参数；special 实体不参与普通 task admission accounting。 */
	if (!dl_task(p) || dl_entity_is_special(&p->dl)) {
		raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);
		return;
	}

	/* 先求新分区中的 active CPU，再借它定位新 root_domain。 */
	msk = this_cpu_cpumask_var_ptr(local_cpu_mask_dl);
	dl_get_task_effective_cpus(p, msk);
	cpu = cpumask_first_and(cpu_active_mask, msk);
	BUG_ON(cpu >= nr_cpu_ids);
	rq = cpu_rq(cpu);
	dl_b = &rq->rd->dl_bw;

	/* 目标根域在 cpuset 变更前已校验容量，这里只在锁下重新发布 reservation。 */
	raw_spin_lock(&dl_b->lock);
	__dl_add(dl_b, p->dl.dl_bw, cpumask_weight(rq->rd->span));
	raw_spin_unlock(&dl_b->lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);
}

/*
 * dl_server_add_bw() - 重建 root_domain 时重新计入指定 CPU 的 server 带宽
 *
 * 入参 rd/cpu 由持 rd->dl_bw.lock 的重建路径提供；无返回值。仅 attached 且 CPU active
 * 的 fair/ext server 进入 total_bw，避免离线 CPU 或未注册 server 贡献虚假容量占用。
 */
static void dl_server_add_bw(struct root_domain *rd, int cpu)
{
	struct sched_dl_entity *dl_se;

	dl_se = &cpu_rq(cpu)->fair_server;
	/* rebuild 按 CPU 重放 server；active 门避免把即将离线 CPU 的份额放入新总量。 */
	if (dl_server(dl_se) && dl_se->dl_bw_attached && cpu_active(cpu))
		__dl_add(&rd->dl_bw, dl_se->dl_bw, dl_bw_cpus(cpu));

#ifdef CONFIG_SCHED_CLASS_EXT
	dl_se = &cpu_rq(cpu)->ext_server;
	if (dl_server(dl_se) && dl_se->dl_bw_attached && cpu_active(cpu))
		__dl_add(&rd->dl_bw, dl_se->dl_bw, dl_bw_cpus(cpu));
#endif
}

/*
 * dl_server_read_bw() - 汇总指定 CPU 当前已附着的 server reservation
 *
 * 返回 fair 与可选 ext server 的 dl_bw 之和；仅检查 server/attached 状态，不修改对象。
 * 调用者用相应热插拔和 dl_bw 锁协议稳定快照，结果用于 CPU 下线容量预演。
 */
static u64 dl_server_read_bw(int cpu)
{
	u64 dl_bw = 0;

	/* fair 与 ext 分别判断，任一未初始化或 detached 都不计入可折扣份额。 */
	if (cpu_rq(cpu)->fair_server.dl_server &&
	    cpu_rq(cpu)->fair_server.dl_bw_attached)
		dl_bw += cpu_rq(cpu)->fair_server.dl_bw;

#ifdef CONFIG_SCHED_CLASS_EXT
	if (cpu_rq(cpu)->ext_server.dl_server &&
	    cpu_rq(cpu)->ext_server.dl_bw_attached)
		dl_bw += cpu_rq(cpu)->ext_server.dl_bw;
#endif

	return dl_bw;
}

/* 重建前清零任务带宽基线，并显式重新计入不属于 task 的 DL servers。 */
/*
 * dl_clear_root_domain() - 为 root-domain 带宽重建建立空基线
 *
 * 入参 rd 是待重建借用根域；无返回值。持 dl_bw 锁清 total_bw，并把每 rq 的 extra_bw
 * 恢复为 max_bw；随后显式重新加入 active server。普通 task 由后续遍历加入，因此此处
 * 不能保留旧 task 总量，也不能遗漏非 task 的 server reservation。
 */
void dl_clear_root_domain(struct root_domain *rd)
{
	int i;

	guard(raw_spinlock_irqsave)(&rd->dl_bw.lock);

	/* 第一阶段清任务总量并恢复每 rq 可回收上限，形成确定的空基线。 */
	/*
	 * Reset total_bw to zero and extra_bw to max_bw so that next
	 * loop will add dl-servers contributions back properly,
	 */
	/* 先清普通 task 总量并把可回收余量复位，随后才能只按现存 DL server 重新记账。 */
	rd->dl_bw.total_bw = 0;
	/* 第二阶段只重放 server；普通 task 由 cpuset 遍历另行逐项发布。 */
	for_each_cpu(i, rd->span)
		cpu_rq(i)->dl.extra_bw = cpu_rq(i)->dl.max_bw;

	/*
	 * dl_servers are not tasks. Since dl_add_task_root_domain ignores
	 * them, we need to account for them here explicitly.
	 */
	/* DL server 不参与 task 遍历，必须逐 CPU 显式补回其 root-domain 带宽。 */
	for_each_cpu(i, rd->span)
		dl_server_add_bw(rd, i);
}

/* 按 CPU 定位并重建其 root-domain DL 基线；无返回值，锁和副作用继承 dl_clear_root_domain()。 */
void dl_clear_root_domain_cpu(int cpu)
{
	/* CPU 包装接口借 cpu_rq 定位根域，锁与重建语义由 dl_clear_root_domain 统一处理。 */
	dl_clear_root_domain(cpu_rq(cpu)->rd);
}

/* 离开 DL 时按 0-lag 处理 reservation，并防止随后以其他策略迁移造成错账。 */
/*
 * switched_from_dl() - 完成 task 离开 DL 类后的带宽与均衡交接
 *
 * 入参 rq/p 由策略切换路径在 rq 锁下提供；无返回值。仍排队的实例先按 0-lag 转为
 * non-contending，cpuset 计数随即减少；不排队 task 立即从 rq 层清账。由于 task 后续
 * 可按其他类迁移，不能让 inactive timer 再访问旧 rq，故清交接位并按需触发 pull。
 */
static void switched_from_dl(struct rq *rq, struct task_struct *p)
{
	/*
	 * task_non_contending() can start the "inactive timer" (if the 0-lag
	 * time is in the future). If the task switches back to dl before
	 * the "inactive timer" fires, it can continue to consume its current
	 * runtime using its current deadline. If it stays outside of
	 * SCHED_DEADLINE until the 0-lag time passes, inactive_task_timer()
	 * will reset the task parameters.
	 */
	/* 离开 DL 后保留到 0-lag：期间切回可续用当前预算，超过 0-lag 则由 timer 重置参数。 */
	if (task_on_rq_queued(p) && p->dl.dl_runtime)
		task_non_contending(&p->dl, false);

	/*
	 * In case a task is setscheduled out from SCHED_DEADLINE we need to
	 * keep track of that on its cpuset (for correct bandwidth tracking).
	 */
	/* cpuset 计数反映策略 membership，与实体是否仍在 rq 上是两条独立状态线。 */
	dec_dl_tasks_cs(p);

	/* 未排队 task 没有后续 dequeue 边沿，由切换路径立即撤销 rq 两类带宽。 */
	if (!task_on_rq_queued(p)) {
		/*
		 * Inactive timer is armed. However, p is leaving DEADLINE and
		 * might migrate away from this rq while continuing to run on
		 * some other class. We need to remove its contribution from
		 * this rq running_bw now, or sub_rq_bw (below) will complain.
		 */
		/* task 可能以其他策略迁移，必须现在从旧 rq 撤销 running_bw，不能留给旧 timer 处理。 */
		if (p->dl.dl_non_contending)
			sub_running_bw(&p->dl, &rq->dl);
		sub_rq_bw(&p->dl, &rq->dl);
	}

	/*
	 * We cannot use inactive_task_timer() to invoke sub_running_bw()
	 * at the 0-lag time, because the task could have been migrated
	 * while SCHED_OTHER in the meanwhile.
	 */
	/* 封闭 timer ownership：离开 DL 后即使 timer 回调运行，也不得再触碰旧 rq。 */
	if (p->dl.dl_non_contending)
		p->dl.dl_non_contending = 0;

	/*
	 * Since this might be the only -deadline task on the rq,
	 * this is the right place to try to pull some other one
	 * from an overloaded CPU, if any.
	 */
	/* 若本 rq 因该任务离开而不再有 DL 工作，应尝试从过载 CPU 拉取可迁移任务。 */
	if (!task_on_rq_queued(p) || rq->dl.dl_nr_running)
		return;

	deadline_queue_pull_task(rq);
}

/*
 * When switching to -deadline, we may overload the rq, then
 * we try to push someone off, if possible.
 */
/* 取消旧 inactive timer，恢复 rq 带宽并按是否排队触发 push/抢占。 */
/*
 * switched_to_dl() - 发布 task 新取得的 DL 策略并触发必要均衡
 *
 * 入参 rq/p 在 rq 锁下稳定；无返回值。先取消旧 0-lag timer、增加 cpuset DL 计数；
 * 未排队 task 仅恢复 this_bw，首次唤醒再建 CBS。已排队 task 根据是否为 donor，安排
 * push、EDF 抢占或 PELT 更新。函数不重复 enqueue 已在 rq 的实体。
 */
static void switched_to_dl(struct rq *rq, struct task_struct *p)
{
	cancel_inactive_timer(&p->dl);

	/*
	 * In case a task is setscheduled to SCHED_DEADLINE we need to keep
	 * track of that on its cpuset (for correct bandwidth tracking).
	 */
	/* 策略进入 DL 时同步增加 cpuset 计数，供跨分区带宽重建判断使用。 */
	inc_dl_tasks_cs(p);

	/* If p is not queued we will update its parameters at next wakeup. */
	/* p 尚未入队时只恢复 reservation，CBS 参数留到下一次唤醒入队时更新。 */
	if (!task_on_rq_queued(p)) {
		add_rq_bw(&p->dl, &rq->dl);

		return;
	}

	/* 非 donor 需让更紧迫实体获得 CPU；当前 donor 只需刷新本类 PELT 可见性。 */
	if (rq->donor != p) {
		if (p->nr_cpus_allowed > 1 && rq->dl.overloaded)
			deadline_queue_push_tasks(rq);
		/* donor 属于 DL 时做 EDF 比较，否则新 DL task 必然高于较低调度类。 */
		if (dl_task(rq->donor))
			wakeup_preempt_dl(rq, p, 0);
		else
			resched_curr(rq);
	} else {
		update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 0);
	}
}

/*
 * get_prio_dl() - 返回用于调度核心比较的当前绝对 deadline
 *
 * 入参 rq/p 在调度锁协议下稳定；返回 p->dl.deadline。若 p 正是 donor，先结算执行，
 * 防止预算耗尽/补充尚未反映到比较值；返回值是数值快照，不转移任何 ownership。
 */
static u64 get_prio_dl(struct rq *rq, struct task_struct *p)
{
	/*
	 * Make sure to update current so we don't return a stale value.
	 */
	/* 若 p 正在消耗 CPU，先结算本轮运行时间，避免返回耗尽或补充前的旧 deadline。 */
	if (task_current_donor(rq, p))
		update_curr_dl(rq);

	return p->dl.deadline;
}

/*
 * If the scheduling parameters of a -deadline task changed,
 * a push or pull operation might be needed.
 */
/* absolute deadline 改变后分别触发 pull、当前重调度或唤醒抢占。 */
/*
 * prio_changed_dl() - 在绝对 deadline 改变后修复本地/跨 CPU 调度决策
 *
 * 入参 rq/p 在 rq 锁下稳定，old_deadline 是修改前快照；无返回值。未排队或值未变时
 * 无动作；deadline 变晚可触发 pull。若 p 是 donor，则在树中出现更早项时重调度；
 * 否则在非 DL 当前任务或 p 更早时重调度。函数不直接迁移 task。
 */
static void prio_changed_dl(struct rq *rq, struct task_struct *p, u64 old_deadline)
{
	if (!task_on_rq_queued(p))
		return;

	if (p->dl.deadline == old_deadline)
		return;

	if (dl_time_before(old_deadline, p->dl.deadline))
		deadline_queue_pull_task(rq);

	/* donor 与等待者的 deadline 改变分别影响“让出 CPU”和“抢占 CPU”两种方向。 */
	if (task_current_donor(rq, p)) {
		/*
		 * If we now have a earlier deadline task than p,
		 * then reschedule, provided p is still on this
		 * runqueue.
		 */
		/* p 作为 donor 变晚后，若等待树已有更早任务，就立即重调度让 EDF 顺序生效。 */
		if (dl_time_before(rq->dl.earliest_dl.curr, p->dl.deadline))
			resched_curr(rq);
	} else {
		/*
		 * Current may not be deadline in case p was throttled but we
		 * have just replenished it (e.g. rt_mutex_setprio()).
		 *
		 * Otherwise, if p was given an earlier deadline, reschedule.
		 */
		/* p 可能刚从限流状态补充，或获得更早 deadline；两种情况都可能要求抢占当前任务。 */
		if (!dl_task(rq->curr) ||
		    dl_time_before(p->dl.deadline, rq->curr->dl.deadline))
			resched_curr(rq);
	}
}

#ifdef CONFIG_SCHED_CORE
/* core-sched 查询接口：p 为借用 task，返回 throttle 位；cpu 未使用，无状态或 ownership 变化。 */
static int task_is_throttled_dl(struct task_struct *p, int cpu)
{
	return p->dl.dl_throttled;
}
#endif

/* 将上述实现发布给调度核心；各回调共享 rq 锁、task 生命周期和类切换协议。 */
DEFINE_SCHED_CLASS(dl) = {
	/* task 生命周期与抢占入口。 */
	.enqueue_task		= enqueue_task_dl,
	.dequeue_task		= dequeue_task_dl,
	.yield_task		= yield_task_dl,

	.wakeup_preempt		= wakeup_preempt_dl,

	/* EDF 选择和上下文切换记账。 */
	.pick_task		= pick_task_dl,
	.put_prev_task		= put_prev_task_dl,
	.set_next_task		= set_next_task_dl,

	/* SMP 选择、迁移、热插拔和负载均衡入口。 */
	.balance		= balance_dl,
	.select_task_rq		= select_task_rq_dl,
	.migrate_task_rq	= migrate_task_rq_dl,
	.set_cpus_allowed       = set_cpus_allowed_dl,
	.rq_online              = rq_online_dl,
	.rq_offline             = rq_offline_dl,
	.task_woken		= task_woken_dl,
	.find_lock_rq		= find_lock_later_rq,

	/* 周期事件以及策略参数变化入口。 */
	.task_tick		= task_tick_dl,
	.task_fork              = task_fork_dl,

	.get_prio		= get_prio_dl,
	.prio_changed           = prio_changed_dl,
	.switched_from		= switched_from_dl,
	.switched_to		= switched_to_dl,

	/* 通用调度核心对当前 donor 的执行结算入口。 */
	.update_curr		= update_curr_dl,
#ifdef CONFIG_SCHED_CORE
	.task_is_throttled	= task_is_throttled_dl,
#endif
};

/*
 * Used for dl_bw check and update, used under sched_rt_handler()::mutex and
 * sched_domains_mutex.
 */
u64 dl_cookie;

/* 用 cookie 每个 root_domain 只验一次，拒绝低于既有 reservation 的全局上限。 */
/*
 * sched_dl_global_validate() - 预检新的全局 RT/DL 配额能否容纳现有 reservation
 *
 * 无入参；成功返回 0，任一 root_domain 的 total_bw 超出新上限返回 -EBUSY。函数在
 * 外层 mutex 保护下生成 cookie，遍历在线 CPU 并借 sched RCU 稳定 rd；同一根域仅检查
 * 一次，dl_b 锁保护 total_bw。这里只验证，不提交新的 bw 配置。
 */
int sched_dl_global_validate(void)
{
	u64 runtime = global_rt_runtime();
	u64 period = global_rt_period();
	u64 new_bw = to_ratio(period, runtime);
	u64 cookie = ++dl_cookie;
	struct dl_bw *dl_b;
	int cpu, cpus, ret = 0;
	unsigned long flags;

	/*
	 * Here we want to check the bandwidth not being set to some
	 * value smaller than the currently allocated bandwidth in
	 * any of the root_domains.
	 */
	/* CPU 可能共享 root_domain，cookie 避免按 CPU 重复锁定和计算同一总量。 */
	for_each_online_cpu(cpu) {
		rcu_read_lock_sched();

		if (dl_bw_visited(cpu, cookie))
			goto next;

		dl_b = dl_bw_of(cpu);
		cpus = dl_bw_cpus(cpu);

		/* 比较 new_bw * active_cpus 与当前已分配总量，失败后结束后续扫描。 */
		raw_spin_lock_irqsave(&dl_b->lock, flags);
		if (new_bw * cpus < dl_b->total_bw)
			ret = -EBUSY;
		raw_spin_unlock_irqrestore(&dl_b->lock, flags);

next:
		/* sched RCU 的持有范围覆盖 rd 定位、visited 标记和 dl_b 访问。 */
		rcu_read_unlock_sched();

		if (ret)
			break;
	}

	return ret;
}

/*
 * init_dl_rq_bw_ratio() - 从全局配额派生单 rq 的 GRUB 缩放参数
 *
 * 入参 dl_rq 为待更新借用队列；无返回值。无限配额使用 1.0 比例和满带宽，否则把
 * runtime/period 转为固定点 bw_ratio、max_bw 与 extra_bw。调用者负责避免与热路径并发。
 */
static void init_dl_rq_bw_ratio(struct dl_rq *dl_rq)
{
	/* 无限模式不缩放；有限模式把全局配额转换为执行扣减和 reclaim 固定点比例。 */
	if (global_rt_runtime() == RUNTIME_INF) {
		dl_rq->bw_ratio = 1 << RATIO_SHIFT;
		dl_rq->max_bw = dl_rq->extra_bw = 1 << BW_SHIFT;
	} else {
		dl_rq->bw_ratio = to_ratio(global_rt_runtime(),
			  global_rt_period()) >> (BW_SHIFT - RATIO_SHIFT);
		dl_rq->max_bw = dl_rq->extra_bw =
			to_ratio(global_rt_period(), global_rt_runtime());
	}
}

/* 全局 RT 配额通过校验后更新所有 rq 比例和各 root_domain 的准入上限。 */
/*
 * sched_dl_do_global() - 提交已验证的全局 DL 带宽配置
 *
 * 无入参/返回值；外层 mutex 已串行化配置。先更新每个 possible rq 的 reclaim 比例，
 * 再以 cookie 遍历各 root_domain，在 sched RCU 和 dl_b 锁下写入新 bw 上限。无限 runtime
 * 用 -1 表示关闭准入限制；本函数假定 validate 已成功，不再提供失败回滚。
 */
void sched_dl_do_global(void)
{
	u64 new_bw = -1;
	u64 cookie = ++dl_cookie;
	struct dl_bw *dl_b;
	int cpu;
	unsigned long flags;

	if (global_rt_runtime() != RUNTIME_INF)
		new_bw = to_ratio(global_rt_period(), global_rt_runtime());

	/* rq 参数是 per-CPU 状态，即使 CPU 当前离线也要为未来上线准备一致配置。 */
	for_each_possible_cpu(cpu)
		init_dl_rq_bw_ratio(&cpu_rq(cpu)->dl);

	/* root-domain 上限是共享状态，使用 cookie 对每个域只提交一次。 */
	for_each_possible_cpu(cpu) {
		rcu_read_lock_sched();

		if (dl_bw_visited(cpu, cookie)) {
			rcu_read_unlock_sched();
			continue;
		}

		/* 首次遇到该根域时在锁下提交上限，其余 CPU 由 cookie 跳过。 */
		dl_b = dl_bw_of(cpu);

		raw_spin_lock_irqsave(&dl_b->lock, flags);
		dl_b->bw = new_bw;
		raw_spin_unlock_irqrestore(&dl_b->lock, flags);

		rcu_read_unlock_sched();
	}
}

/*
 * We must be sure that accepting a new task (or allowing changing the
 * parameters of an existing one) is consistent with the bandwidth
 * constraints. If yes, this function also accordingly updates the currently
 * allocated bandwidth to reflect the new situation.
 *
 * This function is called while holding p's rq->lock.
 */
/* rq 锁下做任务级准入和总量替换；退出 DL 的归还延迟到正确的 0-lag。 */
/*
 * sched_dl_overflow() - 校验并预提交 task 策略/参数变化的 root-domain reservation
 *
 * 入参 p 在 rq 锁下借用，policy 是目标策略，attr 是只读候选参数；成功返回 0，准入
 * 失败返回负值（当前为 -1）。进入或留在 DL 时锁下替换 total_bw；离开 DL 仅批准，
 * 实际归还延迟到 switched_from_dl()/0-lag。SUGOV special 与相同参数走无副作用快路。
 */
int sched_dl_overflow(struct task_struct *p, int policy,
		      const struct sched_attr *attr)
{
	u64 period = attr->sched_period ?: attr->sched_deadline;
	u64 runtime = attr->sched_runtime;
	u64 new_bw = dl_policy(policy) ? to_ratio(period, runtime) : 0;
	int cpus, err = -1, cpu = task_cpu(p);
	struct dl_bw *dl_b = dl_bw_of(cpu);
	unsigned long cap;

	/* sugov special task 不占普通 DL admission reservation。 */
	if (attr->sched_flags & SCHED_FLAG_SUGOV)
		return 0;

	/* !deadline task may carry old deadline bandwidth */
	/* 非 DL task 也可能暂存旧 dl_bw；仅当策略和份额都未变化时才可直接返回。 */
	if (new_bw == p->dl.dl_bw && task_has_dl_policy(p))
		return 0;

	/*
	 * Either if a task, enters, leave, or stays -deadline but changes
	 * its parameters, we may need to update accordingly the total
	 * allocated bandwidth of the container.
	 */
	/* dl_b 锁覆盖 old→new 总量替换，使失败分支不暴露部分更新。 */
	raw_spin_lock(&dl_b->lock);
	cpus = dl_bw_cpus(cpu);
	cap = dl_bw_capacity(cpu);

	/* 进入、留在和离开 DL 三类转换具有不同的 reservation 交接时机。 */
	if (dl_policy(policy) && !task_has_dl_policy(p) &&
	    !__dl_overflow(dl_b, cap, 0, new_bw)) {
		/* 旧 inactive timer 若仍持有历史 reservation，先扣旧份额再加入新策略。 */
		if (hrtimer_active(&p->dl.inactive_timer))
			__dl_sub(dl_b, p->dl.dl_bw, cpus);
		__dl_add(dl_b, new_bw, cpus);
		err = 0;
	} else if (dl_policy(policy) && task_has_dl_policy(p) &&
		   !__dl_overflow(dl_b, cap, p->dl.dl_bw, new_bw)) {
		/*
		 * XXX this is slightly incorrect: when the task
		 * utilization decreases, we should delay the total
		 * utilization change until the task's 0-lag point.
		 * But this would require to set the task's "inactive
		 * timer" when the task is not inactive.
		 */
		/* 参数修改在同锁内替换总量，并同步 rq active/this_bw 的利用率值。 */
		__dl_sub(dl_b, p->dl.dl_bw, cpus);
		__dl_add(dl_b, new_bw, cpus);
		dl_change_utilization(p, new_bw);
		err = 0;
	} else if (!dl_policy(policy) && task_has_dl_policy(p)) {
		/*
		 * Do not decrease the total deadline utilization here,
		 * switched_from_dl() will take care to do it at the correct
		 * (0-lag) time.
		 */
		err = 0;
	}
	/* err 只有在准入成功或离开 DL 可延迟归还时置零。 */
	raw_spin_unlock(&dl_b->lock);

	return err;
}

/*
 * This function initializes the sched_dl_entity of a newly becoming
 * SCHED_DEADLINE task.
 *
 * Only the static values are considered here, the actual runtime and the
 * absolute deadline will be properly calculated when the task is enqueued
 * for the first time with its new policy.
 */
/* 只复制静态 reservation；absolute deadline/runtime 在首次 enqueue 时生成。 */
/*
 * __setparam_dl() - 把用户 sched_attr 转换为实体静态 DL 参数
 *
 * 入参 p 为待配置 task，attr 已经通过参数/准入校验；无返回值。period 为零时按 deadline
 * 解释，并计算固定点 bandwidth/density。函数不建立当前 CBS runtime/absolute deadline，
 * 也不操作 timer/rb 树；首次 enqueue 才发布动态实例。
 */
void __setparam_dl(struct task_struct *p, const struct sched_attr *attr)
{
	struct sched_dl_entity *dl_se = &p->dl;

	dl_se->dl_runtime = attr->sched_runtime;
	dl_se->dl_deadline = attr->sched_deadline;
	dl_se->dl_period = attr->sched_period ?: dl_se->dl_deadline;
	dl_se->flags = attr->sched_flags & SCHED_DL_FLAGS;
	dl_se->dl_bw = to_ratio(dl_se->dl_period, dl_se->dl_runtime);
	dl_se->dl_density = to_ratio(dl_se->dl_deadline, dl_se->dl_runtime);
}

/*
 * __getparam_dl() - 导出 task 的静态参数或当前动态 CBS 快照
 *
 * 入参 p 是目标 task，attr 是调用者输出缓冲区，flags 选择 DL_DYNAMIC；无返回值。
 * 动态读取在 rq 锁下先结算当前 donor，并把 rq 时钟域的 absolute deadline 平移到
 * ktime_get_ns() 时钟域。静态模式返回配置值；两者都保留非 DL flags 并更新 DL flags。
 */
void __getparam_dl(struct task_struct *p, struct sched_attr *attr, unsigned int flags)
{
	struct sched_dl_entity *dl_se = &p->dl;
	struct rq *rq = task_rq(p);
	u64 adj_deadline;

	attr->sched_priority = p->rt_priority;
	/* 动态状态会被运行/补充路径并发修改，必须在所属 rq 锁下取得一致快照。 */
	if (flags & SCHED_GETATTR_FLAG_DL_DYNAMIC) {
		guard(raw_spinlock_irq)(&rq->__lock);
		update_rq_clock(rq);
		/* 当前 task 的 runtime 先结算到查询时刻，导出值才不是过期预算。 */
		if (task_current(rq, p))
			update_curr_dl(rq);

		attr->sched_runtime = dl_se->runtime;
		adj_deadline = dl_se->deadline - rq_clock(rq) + ktime_get_ns();
		attr->sched_deadline = adj_deadline;
	} else {
		attr->sched_runtime = dl_se->dl_runtime;
		attr->sched_deadline = dl_se->dl_deadline;
	}
	/* period 和配置 flags 不随动态实例变化，统一从静态字段导出。 */
	attr->sched_period = dl_se->dl_period;
	attr->sched_flags &= ~SCHED_DL_FLAGS;
	attr->sched_flags |= dl_se->flags;
}

/*
 * This function validates the new parameters of a -deadline task.
 * We ask for the deadline not being zero, and greater or equal
 * than the runtime, as well as the period of being zero or
 * greater than deadline. Furthermore, we have to be sure that
 * user parameters are above the internal resolution of 1us (we
 * check sched_runtime only since it is always the smaller one) and
 * below 2^63 ns (we have to check both sched_deadline and
 * sched_period, as the latter can be zero).
 */
/* 验证 runtime <= deadline <= period、内部精度、符号位和 sysctl 周期范围。 */
/*
 * __checkparam_dl() - 验证用户提供的 DL 时间参数是否可被内核安全表示
 *
 * 入参 attr 为只读候选配置；合法返回 true，否则 false，无副作用。SUGOV special task
 * 跳过普通参数。普通配置要求 deadline 非零、runtime 不低于固定点精度，时间值不占
 * 符号位，满足 runtime<=deadline<=period，并位于 sysctl period 范围。
 */
bool __checkparam_dl(const struct sched_attr *attr)
{
	u64 period, max, min;

	/* special dl tasks don't actually use any parameter */
	/* special sugov 实体由内核构造，其调度语义不依赖用户三元组。 */
	if (attr->sched_flags & SCHED_FLAG_SUGOV)
		return true;

	/* deadline != 0 */
	/* relative deadline 是 CBS/EDF 时间基准，零值无法形成合法周期参数。 */
	if (attr->sched_deadline == 0)
		return false;

	/*
	 * Since we truncate DL_SCALE bits, make sure we're at least
	 * that big.
	 */
	/* 带宽固定点换算会截断 DL_SCALE 位，runtime 太小将被量化为零，必须拒绝。 */
	if (attr->sched_runtime < (1ULL << DL_SCALE))
		return false;

	/* 绝对时间比较借有符号差值处理回绕，因此用户值必须保留最高位。 */
	/*
	 * Since we use the MSB for wrap-around and sign issues, make
	 * sure it's not set (mind that period can be equal to zero).
	 */
	/* deadline 比较把最高位用于回绕后的有符号判断，因此用户时间值不能占用该位。 */
	if (attr->sched_deadline & (1ULL << 63) ||
	    attr->sched_period & (1ULL << 63))
		return false;

	/* 用户 period=0 表示使用 relative deadline 作为 period。 */
	period = attr->sched_period;
	if (!period)
		period = attr->sched_deadline;

	/* runtime <= deadline <= period (if period != 0) */
	/* CBS 合法三元组要求预算不超过相对 deadline，deadline 也不能超过有效 period。 */
	if (period < attr->sched_deadline ||
	    attr->sched_deadline < attr->sched_runtime)
		return false;

	/* READ_ONCE 接受并发 sysctl 快照；任一完整快照之外的值都按非法拒绝。 */
	max = (u64)READ_ONCE(sysctl_sched_dl_period_max) * NSEC_PER_USEC;
	min = (u64)READ_ONCE(sysctl_sched_dl_period_min) * NSEC_PER_USEC;

	if (period < min || period > max)
		return false;

	return true;
}

/*
 * This function clears the sched_dl_entity static params.
 */
/* 清空静态及运行状态并恢复 PI 自指；timer/RB 生命周期由调用路径先处理。 */
/*
 * __dl_clear_params() - 把 sched_dl_entity 恢复为无 reservation 的静态基线
 *
 * 入参 dl_se 为已从队列、timer 和带宽 ownership 中安全撤下的实体；无返回值。清除
 * 配置值与所有 CBS/server 状态，并在 RT_MUTEXES 下恢复 pi_se 自指。函数不取消 timer、
 * 不删除 rb_node，也不归还 total_bw，调用者必须先完成这些外部生命周期动作。
 */
static void __dl_clear_params(struct sched_dl_entity *dl_se)
{
	dl_se->dl_runtime		= 0;
	dl_se->dl_deadline		= 0;
	dl_se->dl_period		= 0;
	dl_se->flags			= 0;
	dl_se->dl_bw			= 0;
	dl_se->dl_density		= 0;

	/* 动态 CBS、通知和 server 状态与静态配置一并回到未发布状态。 */
	dl_se->dl_throttled		= 0;
	dl_se->dl_yielded		= 0;
	dl_se->dl_non_contending	= 0;
	dl_se->dl_overrun		= 0;
	dl_se->dl_server		= 0;
	dl_se->dl_defer			= 0;
	dl_se->dl_defer_running		= 0;
	dl_se->dl_defer_armed		= 0;

#ifdef CONFIG_RT_MUTEXES
	/* 清除可能的 PI donor 关联，重新以自身作为有效调度实体。 */
	dl_se->pi_se			= dl_se;
#endif
}

/* 初始化两类 timer 和空 RB 节点后建立无 reservation 的基线状态。 */
/*
 * init_dl_entity() - 初始化新实体的队列节点、timer 与空参数状态
 *
 * 入参 dl_se 为尚未发布的输出对象；无返回值。初始化顺序先建立空 rb ownership，再绑定
 * replenish/inactive timer 回调，最后清状态。调用后实体仍不可运行，必须配置并准入后入队。
 */
void init_dl_entity(struct sched_dl_entity *dl_se)
{
	RB_CLEAR_NODE(&dl_se->rb_node);
	init_dl_task_timer(dl_se);
	init_dl_inactive_task_timer(dl_se);
	__dl_clear_params(dl_se);
}

/*
 * dl_param_changed() - 比较 task 的静态 DL 配置与候选 sched_attr
 *
 * 任一 runtime/deadline/period/DL flag 不同返回 true，全相同返回 false；只读无副作用。
 * attr->sched_period 必须已按调用层约定规范化，否则用户的零 period 可能表现为值变化。
 */
bool dl_param_changed(struct task_struct *p, const struct sched_attr *attr)
{
	struct sched_dl_entity *dl_se = &p->dl;

	if (dl_se->dl_runtime != attr->sched_runtime ||
	    dl_se->dl_deadline != attr->sched_deadline ||
	    dl_se->dl_period != attr->sched_period ||
	    dl_se->flags != (attr->sched_flags & SCHED_DL_FLAGS))
		return true;

	return false;
}

/* 以 trial 的实际 capacity 预演缩容，已有 total_bw 放不下则拒绝。 */
/*
 * dl_cpuset_cpumask_can_shrink() - 检查 cpuset 缩容后现有 DL reservation 是否仍可容纳
 *
 * 入参 cur 是当前 CPU 集合，trial 是候选集合；可缩返回 1，否则 0。sched RCU 稳定
 * 当前 root_domain，dl_b 锁保护 total_bw；capacity 使用 trial 的异构实际容量而非 CPU 数。
 */
int dl_cpuset_cpumask_can_shrink(const struct cpumask *cur,
				 const struct cpumask *trial)
{
	unsigned long flags, cap;
	struct dl_bw *cur_dl_b;
	int ret = 1;

	/* cur 中任一 CPU 都可定位共享 dl_bw；trial 则只提供变更后的容量分母。 */
	rcu_read_lock_sched();
	cur_dl_b = dl_bw_of(cpumask_any(cur));
	cap = __dl_bw_capacity(trial);
	/* 用现有 total_bw 对新 capacity 做零增量 overflow 检查，不修改 reservation。 */
	raw_spin_lock_irqsave(&cur_dl_b->lock, flags);
	if (__dl_overflow(cur_dl_b, cap, 0, 0))
		ret = 0;
	raw_spin_unlock_irqrestore(&cur_dl_b->lock, flags);
	rcu_read_unlock_sched();

	return ret;
}

/* dl_bw_manage 的封闭操作集合：预演下线、预留份额、归还份额。 */
enum dl_bw_request {
	dl_bw_req_deactivate = 0,
	dl_bw_req_alloc,
	dl_bw_req_free
};

/* 在 sched RCU 与 dl_bw 锁下统一处理预留、归还和 CPU 下线容量预演。 */
/*
 * dl_bw_manage() - 统一执行 root-domain 带宽申请、释放与 CPU 下线预检
 *
 * 入参 req 指定操作，cpu 定位根域，dl_bw 是 alloc/free 份额；成功返回 0，申请或下线
 * 将超量时返回 -EBUSY。sched RCU 稳定 rd，dl_b 锁使总量原子变化。deactivate 只预演，
 * 会扣除该 CPU server 份额和 capacity，不提交 CPU 状态或普通 task 迁移。
 */
static int dl_bw_manage(enum dl_bw_request req, int cpu, u64 dl_bw)
{
	unsigned long flags, cap;
	struct dl_bw *dl_b;
	bool overflow = 0;
	u64 dl_server_bw = 0;

	rcu_read_lock_sched();
	dl_b = dl_bw_of(cpu);
	raw_spin_lock_irqsave(&dl_b->lock, flags);

	/* 三种请求共享同一锁内 capacity/total_bw 快照。 */
	cap = dl_bw_capacity(cpu);
	switch (req) {
	case dl_bw_req_free:
		/* free 由已持有 reservation 的调用者保证不下溢。 */
		__dl_sub(dl_b, dl_bw, dl_bw_cpus(cpu));
		break;
	case dl_bw_req_alloc:
		/* alloc 先准入，成功即预留目的域容量，使后续迁移阶段不再失败。 */
		overflow = __dl_overflow(dl_b, cap, 0, dl_bw);

		if (!overflow) {
			/*
			 * We reserve space in the destination
			 * root_domain, as we can't fail after this point.
			 * We will free resources in the source root_domain
			 * later on (see set_cpus_allowed_dl()).
			 */
			/* 先在目的 root_domain 预留，保证迁移提交后不失败；源域份额在 affinity 切换时归还。 */
			__dl_add(dl_b, dl_bw, dl_bw_cpus(cpu));
		}
		break;
	case dl_bw_req_deactivate:
		/*
		 * cpu is not off yet, but we need to do the math by
		 * considering it off already (i.e., what would happen if we
		 * turn cpu off?).
		 */
		/* CPU 尚在线，但准入必须预演下线后的容量，否则真正移除时可能留下超卖账目。 */
		cap -= arch_scale_cpu_capacity(cpu);

		/*
		 * cpu is going offline and NORMAL and EXT tasks will be
		 * moved away from it. We can thus discount dl_server
		 * bandwidth contribution as it won't need to be servicing
		 * tasks after the cpu is off.
		 */
		/* 普通与 EXT 工作会迁走，负责服务它们的本 CPU DL server 带宽可从下线后需求中扣除。 */
		/* fair/ext 工作会随 CPU 离开，可从预演后的现有占用中折扣。 */
		dl_server_bw = dl_server_read_bw(cpu);

		/*
		 * Not much to check if no DEADLINE bandwidth is present.
		 * dl_servers we can discount, as tasks will be moved out the
		 * offlined CPUs anyway.
		 */
		/* 扣除可消失的 server 后若无普通 DL 带宽，就不存在下线导致的 DL 容量溢出。 */
		/* 若仍有普通 DL reservation，则必须至少保留一个 CPU 且新容量不得溢出。 */
		if (dl_b->total_bw - dl_server_bw > 0) {
			/*
			 * Leaving at least one CPU for DEADLINE tasks seems a
			 * wise thing to do. As said above, cpu is not offline
			 * yet, so account for that.
			 */
			/* 仍有 DL reservation 时至少保留一个 CPU；当前 CPU 尚在线，所以计数需显式减一。 */
			/* 仍有其他 active CPU 才做容量检查；最后一个 CPU 必须直接拒绝下线。 */
			if (dl_bw_cpus(cpu) - 1)
				overflow = __dl_overflow(dl_b, cap, dl_server_bw, 0);
			else
				overflow = 1;
		}

		break;
	}

	/* deactivate 不改 total_bw；alloc/free 的提交则在释放锁时对并发路径可见。 */
	raw_spin_unlock_irqrestore(&dl_b->lock, flags);
	rcu_read_unlock_sched();

	return overflow ? -EBUSY : 0;
}

/* CPU hotplug 预检包装：cpu 定位根域；返回 0/-EBUSY，不改变状态，语义委托 dl_bw_manage。 */
int dl_bw_deactivate(int cpu)
{
	return dl_bw_manage(dl_bw_req_deactivate, cpu, 0);
}

/* 为跨根域迁移预留 dl_bw 固定点份额；返回 0/-EBUSY，成功后调用者取得目的域 reservation。 */
int dl_bw_alloc(int cpu, u64 dl_bw)
{
	return dl_bw_manage(dl_bw_req_alloc, cpu, dl_bw);
}

/* 归还 cpu 所属根域的 dl_bw 份额；无返回值，调用者保证此前成功持有相同 reservation。 */
void dl_bw_free(int cpu, u64 dl_bw)
{
	dl_bw_manage(dl_bw_req_free, cpu, dl_bw);
}

/* 将 cpu 的 dl_rq 调试统计写入借用 seq_file；无返回值，格式与读取同步由 print_dl_rq() 负责。 */
void print_dl_stats(struct seq_file *m, int cpu)
{
	print_dl_rq(m, cpu, &cpu_rq(cpu)->dl);
}
