// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple CPU accounting cgroup controller
 */
/*
 * 本文件把“CPU 上流逝的时间”归入 task、thread group、cgroup 与每 CPU
 * cpustat。入口分成三类：周期 tick 的抽样计费、IRQ/虚拟化提供的精确
 * 计费，以及 NO_HZ CPU 在读取时补算尚未落账的区间。
 *
 * 核心不变量是同一段时间只进入一种基础类别；guest 同时是 task 的
 * user time，但另以 GUEST/GUEST_NICE 记录其子集。当前 CPU 的写路径通常
 * 已关中断或持 rq 锁；跨 CPU 读取只承诺一致或可重试的快照，并不冻结
 * 正在运行的任务。seqcount 保护 vtime/idle 快照，prev_cputime 自旋锁则
 * 保证导出的 user/system 值单调且二者之和等于调度器运行时间。
 */
#include <linux/sched/clock.h>
#include <linux/sched/cputime.h>
#include <linux/tsacct_kern.h>
#include "sched.h"

#ifdef CONFIG_VIRT_CPU_ACCOUNTING_NATIVE
 #include <asm/cputime.h>
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING

DEFINE_STATIC_KEY_FALSE(sched_clock_irqtime);

/*
 * There are no locks covering percpu hardirq/softirq time.
 * They are only modified in vtime_account, on corresponding CPU
 * with interrupts disabled. So, writes are safe.
 * They are read and saved off onto struct rq in update_rq_clock().
 * This may result in other CPU reading this CPU's IRQ time and can
 * race with irq/vtime_account on this CPU. We would either get old
 * or new value with a side effect of accounting a slice of IRQ time to wrong
 * task when IRQ is in progress while we read rq->clock. That is a worthy
 * compromise in place of having locks on each IRQ in account_system_time.
 */
DEFINE_PER_CPU(struct irqtime, cpu_irqtime);

/* 开启 IRQ 精确计费静态分支；无入参和返回值，切换会影响所有 CPU 的后续入口。 */
/*
 * 业务背景：平台具备 sched_clock IRQ 计时能力后，需要全局开启 IRQ 精确计费热路径。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；启用 sched_clock_irqtime static key。
 * 注意事项：属于全局控制路径且可能修改跳转标签，不能从普通计费热路径或原子上下文随意调用。
 */

void enable_sched_clock_irqtime(void)
{
	static_branch_enable(&sched_clock_irqtime);
}

/* 关闭已启用的 IRQ 精确计费；无入参和返回值，未启用时保持不变。 */
/*
 * 业务背景：IRQ 精确计费能力撤销时，要让后续入口退回 tick/vtime 分类而不重复计费。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；若 static key 已启用则将其关闭，否则状态不变。
 * 注意事项：全局 static-key 更新可同步且有调用上下文限制；不会清除已经累计的 per-CPU IRQ 时间。
 */
void disable_sched_clock_irqtime(void)
{
	if (irqtime_enabled())
		static_branch_disable(&sched_clock_irqtime);
}

/*
 * 把本 CPU 自上次 IRQ 边界以来的 @delta 纳秒提交到 @idx，并同步累计
 * irqtime 总量/tick 待扣量；调用者在本 CPU 关中断上下文中借用 @irqtime。
 */
/*
 * 业务背景：每次 IRQ 边界测得的时间片要同时进入 cpustat 和供后续 tick 扣除的 IRQ 累计。
 * 入参：irqtime 是不可空、本 CPU 借用的输入/输出状态；delta 是新增纳秒；idx 是 IRQ 或 SOFTIRQ cpustat 类别。
 * 出参/返回：无直接返回值、无输出参数；累加目标字段和 total，非 dyntick idle 时也累加 tick_delta。
 * 注意事项：调用者必须在对应 CPU 关中断；u64_stats 只保证读者快照，不能代替两个本地写者的串行。
 */
static void irqtime_account_delta(struct irqtime *irqtime, u64 delta,
				  enum cpu_usage_stat idx)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;

	/* 同一统计序列同时发布分类值、总值和待扣 tick 值，读者不会拼接半次更新。 */
	u64_stats_update_begin(&irqtime->sync);
	cpustat[idx] += delta;
	irqtime->total += delta;
	if (!kcpustat_idle_dyntick())
		irqtime->tick_delta += delta;
	u64_stats_update_end(&irqtime->sync);
}

/*
 * Called after incrementing preempt_count on {soft,}irq_enter
 * and before decrementing preempt_count on {soft,}irq_exit.
 */
/*
 * 必须在 irq_enter 已增加、irq_exit 尚未减少 preempt_count 的窗口调用，
 * @offset 用来扣掉调用点自身层级；据此区分 hardirq、softirq 与 ksoftirqd。
 */
/*
 * 业务背景：IRQ enter/exit 边界需把 sched_clock 增量归为 hardirq 或非 ksoftirqd softirq 时间。
 * 入参：curr 是不可空、本 CPU 当前 task 的借用输入；offset 是需从 irq_count 扣除的嵌套位偏移。
 * 出参/返回：无直接返回值、无输出参数；可能推进 irq_start_time 并累计 IRQ/SOFTIRQ 纳秒。
 * 注意事项：必须在 preempt_count 已加/未减且本地中断关闭的窗口；禁用 irqtime 时无副作用返回。
 */
void irqtime_account_irq(struct task_struct *curr, unsigned int offset)
{
	struct irqtime *irqtime = this_cpu_ptr(&cpu_irqtime);
	unsigned int pc;
	s64 delta;
	int cpu;

	if (!irqtime_enabled())
		return;

	/* 先推进 IRQ 边界时间和嵌套快照，再用同一 delta 做唯一分类。 */
	cpu = smp_processor_id();
	delta = sched_clock_cpu(cpu) - irqtime->irq_start_time;
	irqtime->irq_start_time += delta;
	pc = irq_count() - offset;

	/*
	 * We do not account for softirq time from ksoftirqd here.
	 * We want to continue accounting softirq time to ksoftirqd thread
	 * in that case, so as not to confuse scheduler with a special task
	 * that do not consume any time, but still wants to run.
	 */
	/* ksoftirqd 是普通可调度线程，其 softirq 工作必须保留在线程 system 时间中。 */
	if (pc & HARDIRQ_MASK)
		irqtime_account_delta(irqtime, delta, CPUTIME_IRQ);
	else if ((pc & SOFTIRQ_OFFSET) && curr != this_cpu_ksoftirqd())
		irqtime_account_delta(irqtime, delta, CPUTIME_SOFTIRQ);
}

/* 从 tick_delta 最多领取 @maxtime 纳秒，返回本 tick 不得再归给 task 的时间。 */
/*
 * 业务背景：tick 分类前必须扣掉已由精确 IRQ 路径记账的重叠区间，防止同一时间双计。
 * 入参：maxtime 是本次最多可领取的纳秒上限，允许为 U64 范围内任意值。
 * 出参/返回：返回不超过 maxtime 的已领取纳秒，并从本 CPU tick_delta 中扣除；无输出参数。
 * 注意事项：调用者负责本 CPU 串行和关中断；返回值只代表待扣量，IRQ cpustat 已在早先写入。
 */
static u64 irqtime_tick_accounted(u64 maxtime)
{
	struct irqtime *irqtime = this_cpu_ptr(&cpu_irqtime);
	u64 delta;

	delta = min(irqtime->tick_delta, maxtime);
	irqtime->tick_delta -= delta;

	return delta;
}

#else /* !CONFIG_IRQ_TIME_ACCOUNTING: */

/* 未配置 IRQ_TIME 时没有可领取的独立 IRQ 时间，恒返回 0。 */
/*
 * 业务背景：关闭 CONFIG_IRQ_TIME_ACCOUNTING 时保留统一调用接口，使 tick 路径无需条件分支。
 * 入参：dummy 是未使用的纳秒上限输入，对结果无影响。
 * 出参/返回：恒返回 0；无输出参数或状态副作用。
 * 注意事项：这是配置替代实现，不提供精确 IRQ 扣除，IRQ 时间由普通 tick 分类承担。
 */
static u64 irqtime_tick_accounted(u64 dummy)
{
	return 0;
}

#endif /* !CONFIG_IRQ_TIME_ACCOUNTING */

/*
 * 把 @tmp 纳秒写入本 CPU 根 cpustat，再沿 @p 的 cgroup 层级计入 @index；
 * @p 只是借用，调用点负责保证本 CPU 计费上下文稳定。
 */
/*
 * 业务背景：每段 task CPU 时间既要进入根 per-CPU 统计，也要沿任务所属 cgroup 层级传播。
 * 入参：p 是不可空、当前计费 task 的借用输入；index 是有效 cpu_usage_stat 字段；tmp 是新增纳秒。
 * 出参/返回：无直接返回值、无输出参数；累加根 kernel_cpustat 与 p 所在各级 cgroup 字段。
 * 注意事项：只适用于本 CPU 计费上下文，函数不取得 task/cgroup 引用；错误 index 会越界写统计数组。
 */
static inline void task_group_account_field(struct task_struct *p, int index,
					    u64 tmp)
{
	/*
	 * Since all updates are sure to touch the root cgroup, we
	 * get ourselves ahead and touch it first. If the root cgroup
	 * is the only cgroup, then nothing else should be necessary.
	 *
	 */
	__this_cpu_add(kernel_cpustat.cpustat[index], tmp);

	cgroup_account_cputime_field(p, index, tmp);
}

/*
 * Account user CPU time to a process.
 * @p: the process that the CPU time gets accounted to
 * @cputime: the CPU time spent in user space since the last update
 */
/* @p 为当前运行 task，@cputime 为本次新增纳秒；同时更新 task、线程组、cgroup 和进程记账。 */
/*
 * 业务背景：用户态执行区间要同步计入 task、线程组、nice 分类、cgroup 和进程会计。
 * 入参：p 是不可空、当前 task 的借用输入/输出对象；cputime 是本次新增的纳秒数。
 * 出参/返回：无直接返回值、无输出参数；累加 utime 并按 nice 选择 USER/NICE 字段。
 * 注意事项：调用者保证 p 的本地计费串行与生命周期；本函数不检查溢出，也不把普通 user 计为 guest。
 */
void account_user_time(struct task_struct *p, u64 cputime)
{
	int index;

	/* Add user time to process. */
	p->utime += cputime;
	account_group_user_time(p, cputime);

	index = (task_nice(p) > 0) ? CPUTIME_NICE : CPUTIME_USER;

	/* Add user time to cpustat. */
	task_group_account_field(p, index, cputime);

	/* Account for user time used */
	acct_account_cputime(p);
}

/*
 * Account guest CPU time to a process.
 * @p: the process that the CPU time gets accounted to
 * @cputime: the CPU time spent in virtual machine since the last update
 */
/* guest 时间既累计进 user 总量，也按 nice 状态写入独立 guest 子类别；无直接返回值。 */
/*
 * 业务背景：VCPU 客体执行在 task 视角属于 user 时间，同时 cpustat 还需保留 guest 子集。
 * 入参：p 是不可空、当前 VCPU task 的借用输入/输出对象；cputime 是新增 guest 纳秒。
 * 出参/返回：无直接返回值、无输出参数；累加 utime/gtime，并按 nice 写 USER/NICE 与 GUEST 子字段。
 * 注意事项：调用者必须只传真实 guest 区间并保证本地串行，否则会与 system/user 路径重复记账。
 */
void account_guest_time(struct task_struct *p, u64 cputime)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;

	/* Add guest time to process. */
	p->utime += cputime;
	account_group_user_time(p, cputime);
	p->gtime += cputime;

	/* guest 是 user 的子集：基础 USER/NICE 与额外 GUEST 字段必须同步增长。 */
	/* Add guest time to cpustat. */
	if (task_nice(p) > 0) {
		task_group_account_field(p, CPUTIME_NICE, cputime);
		cpustat[CPUTIME_GUEST_NICE] += cputime;
	} else {
		task_group_account_field(p, CPUTIME_USER, cputime);
		cpustat[CPUTIME_GUEST] += cputime;
	}
}

/*
 * Account system CPU time to a process and desired cpustat field
 * @p: the process that the CPU time gets accounted to
 * @cputime: the CPU time spent in kernel space since the last update
 * @index: pointer to cpustat field that has to be updated
 */
/* 把 @cputime 纳秒归给 @p 的 system 总量及调用者选择的 cpustat @index。 */
/*
 * 业务背景：内核执行时间的 task 总量相同，但 per-CPU/cgroup 还需区分 SYSTEM、IRQ 或 SOFTIRQ。
 * 入参：p 是不可空当前 task 的借用输入/输出；cputime 是新增纳秒；index 是目标 cpu_usage_stat 类别。
 * 出参/返回：无直接返回值、无输出参数；累加 task/线程组 system 时间及所选统计字段并触发进程会计。
 * 注意事项：index 必须有效且调用者负责互斥分类；本函数不判断 IRQ 上下文，也不取得 p 引用。
 */
void account_system_index_time(struct task_struct *p,
			       u64 cputime, enum cpu_usage_stat index)
{
	/* Add system time to process. */
	p->stime += cputime;
	account_group_system_time(p, cputime);

	/* Add system time to cpustat. */
	task_group_account_field(p, index, cputime);

	/* Account for system time used */
	acct_account_cputime(p);
}

/*
 * Account system CPU time to a process.
 * @p: the process that the CPU time gets accounted to
 * @hardirq_offset: the offset to subtract from hardirq_count()
 * @cputime: the CPU time spent in kernel space since the last update
 */
/*
 * 根据 PF_VCPU 与当前 IRQ 嵌套状态把内核态区间分流到 guest、hardirq、
 * softirq 或普通 system；@hardirq_offset 排除调用点人为增加的层级。
 */
/*
 * 业务背景：同为内核态采样，VCPU、hardirq、softirq 和普通 system 必须按互斥优先级分流。
 * 入参：p 是不可空当前 task 的借用输入/输出；hardirq_offset 是调用点需扣除的 IRQ 嵌套偏移；cputime 是新增纳秒。
 * 出参/返回：无直接返回值、无输出参数；恰好调用一个 guest 或 system-index 记账路径。
 * 注意事项：依赖当前 CPU 的 irq_count/preempt 状态且不可睡眠；错误 offset 会把时间归入错误类别。
 */
void account_system_time(struct task_struct *p, int hardirq_offset, u64 cputime)
{
	int index;

	if ((p->flags & PF_VCPU) && (irq_count() - hardirq_offset == 0)) {
		account_guest_time(p, cputime);
		return;
	}

	/* 非 guest 才按 IRQ 嵌套细分；hardirq 优先于 softirq，剩余为普通 system。 */
	if (hardirq_count() - hardirq_offset)
		index = CPUTIME_IRQ;
	else if (in_serving_softirq())
		index = CPUTIME_SOFTIRQ;
	else
		index = CPUTIME_SYSTEM;

	account_system_index_time(p, cputime, index);
}

/*
 * Account for involuntary wait time.
 * @cputime: the CPU time spent in involuntary wait
 */
/* 把 hypervisor 抢占 guest 的 @cputime 纳秒写入本 CPU STEAL 类别。 */
/*
 * 业务背景：虚拟 CPU 被宿主抢占的墙钟时间不属于 guest task 执行，需要单独进入 STEAL。
 * 入参：cputime 是本次新增的 steal 纳秒数。
 * 出参/返回：无直接返回值、无输出参数；累加本 CPU CPUTIME_STEAL。
 * 注意事项：调用者保证本 CPU 串行；函数不限制增量，重复提交会永久双计。
 */
void account_steal_time(u64 cputime)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;

	cpustat[CPUTIME_STEAL] += cputime;
}

/*
 * Account for idle time.
 * @cputime: the CPU time spent in idle wait
 */
/* idle 区间按采样时 nr_iowait 是否非零归入 IOWAIT 或 IDLE；该分类是近似快照。 */
/*
 * 业务背景：CPU 无可运行任务的时间需区分纯 idle 与存在 I/O 等待者时的 iowait 近似值。
 * 入参：cputime 是本次新增的空闲纳秒数。
 * 出参/返回：无直接返回值、无输出参数；按本 CPU rq->nr_iowait 快照累加 IDLE 或 IOWAIT。
 * 注意事项：nr_iowait 可被远端并发修改，分类允许近似甚至相邻读取抖动；调用者保证本 CPU 写串行。
 */
void account_idle_time(u64 cputime)
{
	u64 *cpustat = kcpustat_this_cpu->cpustat;
	struct rq *rq = this_rq();

	if (atomic_read(&rq->nr_iowait) > 0)
		cpustat[CPUTIME_IOWAIT] += cputime;
	else
		cpustat[CPUTIME_IDLE] += cputime;
}


#ifdef CONFIG_SCHED_CORE
/*
 * Account for forceidle time due to core scheduling.
 *
 * REQUIRES: schedstat is enabled.
 */
/* core scheduling 强制空转时，为受影响 task 的统计和 cgroup 增加 @delta 纳秒。 */
/*
 * 业务背景：core scheduling 为隔离 SMT sibling 被迫空转时，需要把损失归因到受影响 task/cgroup。
 * 入参：p 是不可空、受影响 task 的借用输入/输出对象；delta 是新增强制空转纳秒。
 * 出参/返回：无直接返回值、无输出参数；累加 schedstat core_forceidle_sum 和 FORCEIDLE cgroup 字段。
 * 注意事项：仅 CONFIG_SCHED_CORE 存在且要求 schedstat 已启用；调用者保证 task 生命周期和计费串行。
 */
void __account_forceidle_time(struct task_struct *p, u64 delta)
{
	__schedstat_add(p->stats.core_forceidle_sum, delta);

	task_group_account_field(p, CPUTIME_FORCEIDLE, delta);
}
#endif /* CONFIG_SCHED_CORE */

/*
 * When a guest is interrupted for a longer amount of time, missed clock
 * ticks are not redelivered later. Due to that, this function may on
 * occasion account more time than the calling functions think elapsed.
 */
#ifdef CONFIG_PARAVIRT
struct static_key paravirt_steal_enabled;

#ifdef CONFIG_HAVE_PV_STEAL_CLOCK_GEN
/* 原生后备实现没有 paravirt steal clock，@cpu 任意时均返回 0。 */
/*
 * 业务背景：支持可替换 PV steal clock 的架构需要一个未安装 paravirt 实现时的静态调用目标。
 * 入参：cpu 是未使用的 CPU 编号输入，后备实现不校验范围。
 * 出参/返回：恒返回 0 纳秒；无输出参数或状态副作用。
 * 注意事项：仅 HAVE_PV_STEAL_CLOCK_GEN 配置存在，后续 static_call 可替换该实现，调用者不拥有返回对象。
 */
static u64 native_steal_clock(int cpu)
{
	return 0;
}

DEFINE_STATIC_CALL(pv_steal_clock, native_steal_clock);
#endif
#endif

/*
 * 从平台累计 steal clock 领取不超过 @maxtime 的增量并推进 rq 游标；
 * 静态分支关闭或未配置 PARAVIRT 时返回 0。
 */
/*
 * 业务背景：tick/vtime 分类前要从平台累计 steal clock 中领取尚未记账且本轮可容纳的区间。
 * 入参：maxtime 是本轮最多可扣除的纳秒上限。
 * 出参/返回：返回 0..maxtime 的已记账 steal 纳秒；推进 rq->prev_steal_time 并累加 STEAL 字段。
 * 注意事项：读取本 CPU 平台时钟，调用者需保证本地 rq 计费串行；平台累计值异常可能影响无符号差值。
 */
static __always_inline u64 steal_account_process_time(u64 maxtime)
{
#ifdef CONFIG_PARAVIRT
	if (static_key_false(&paravirt_steal_enabled)) {
		u64 steal;

		/* 平台值是累计钟，先减已消费游标，再钳到本轮预算。 */
		steal = paravirt_steal_clock(smp_processor_id());
		steal -= this_rq()->prev_steal_time;
		steal = min(steal, maxtime);
		account_steal_time(steal);
		this_rq()->prev_steal_time += steal;

		return steal;
	}
#endif /* CONFIG_PARAVIRT */
	return 0;
}

/*
 * Account how much elapsed time was spent in steal, IRQ, or softirq time.
 */
/* 关中断调用；先扣 steal 再扣 IRQ，返回总领取量且绝不超过 @max。 */
/*
 * 业务背景：把一段墙钟归给 task/idle 前，应先扣除已独立记录的 steal 和 IRQ 重叠时间。
 * 入参：max 是本轮可扣除的最大纳秒数。
 * 出参/返回：返回 0..max 的 steal+IRQ 总纳秒；相关 per-CPU 游标随领取推进，无输出参数。
 * 注意事项：强制要求本地中断关闭；顺序固定为 steal 后 IRQ，防止两者合计超过 max。
 */
static inline u64 account_other_time(u64 max)
{
	u64 accounted;

	lockdep_assert_irqs_disabled();

	accounted = steal_account_process_time(max);

	if (accounted < max)
		accounted += irqtime_tick_accounted(max - accounted);

	return accounted;
}

#ifdef CONFIG_64BIT
/* 64 位平台可原子读取 task 的累计运行纳秒，返回借用快照。 */
/*
 * 业务背景：线程组汇总需要读取每个 task 的调度器累计运行时间，64 位平台可直接取得完整快照。
 * 入参：t 是不可空、由 RCU/调用者稳定的只读借用 task。
 * 出参/返回：返回 t->se.sum_exec_runtime 纳秒快照；无输出参数或 ownership 变化。
 * 注意事项：仅 CONFIG_64BIT 实现；快照可在返回后立即变旧，但不会发生 64 位撕裂。
 */
static inline u64 read_sum_exec_runtime(struct task_struct *t)
{
	return t->se.sum_exec_runtime;
}
#else /* !CONFIG_64BIT: */
/* 32 位平台用 task rq 锁防止 64 位字段撕裂，返回稳定的累计运行纳秒。 */
/*
 * 业务背景：32 位平台读取 64 位运行时间必须与调度器更新串行，避免高低半字来自不同代。
 * 入参：t 是不可空、由 RCU/调用者稳定的借用 task。
 * 出参/返回：返回持 task rq 锁读取的 sum_exec_runtime 纳秒快照；无输出参数或 ownership 变化。
 * 注意事项：仅非 64 位实现；会获取 rq 锁且可能重试迁移，调用者不得已持冲突 rq 锁。
 */
static u64 read_sum_exec_runtime(struct task_struct *t)
{
	u64 ns;
	struct rq_flags rf;
	struct rq *rq;

	/* task_rq_lock 同时稳定迁移归属和 64 位字段，解锁后只保留数值副本。 */
	rq = task_rq_lock(t, &rf);
	ns = t->se.sum_exec_runtime;
	task_rq_unlock(rq, t, &rf);

	return ns;
}
#endif /* !CONFIG_64BIT */

/*
 * Accumulate raw cputime values of dead tasks (sig->[us]time) and live
 * tasks (sum on group iteration) belonging to @tsk's group.
 */
/*
 * 把 @tsk 线程组中已死亡线程的 signal 累计值与存活线程快照合并写入
 * 输出参数 @times；RCU 稳定线程链，stats_lock seqcount 使死亡累计与遍历一致。
 */
/*
 * 业务背景：POSIX timer/统计读取需要合并线程组已退出线程累计值与所有存活线程的瞬时 CPU 时间。
 * 入参：tsk 是不可空、目标线程组成员的借用输入；times 是不可空、由调用者拥有的输出结构。
 * 出参/返回：无直接返回值；覆盖 times 的 user/system/sum_runtime 纳秒，times ownership 不变。
 * 注意事项：RCU 与 signal stats_lock 稳定遍历但不强刷其他 CPU 正运行线程，所得是可一致复读的近似快照。
 */
void thread_group_cputime(struct task_struct *tsk, struct task_cputime *times)
{
	struct signal_struct *sig = tsk->signal;
	struct task_struct *t;
	u64 utime, stime;

	/* 只刷新调用者同组的 current，避免为远端线程发送昂贵的同步请求。 */
	/*
	 * Update current task runtime to account pending time since last
	 * scheduler action or thread_group_cputime() call. This thread group
	 * might have other running tasks on different CPUs, but updating
	 * their runtime can affect syscall performance, so we skip account
	 * those pending times and rely only on values updated on tick or
	 * other scheduler action.
	 */
	if (same_thread_group(current, tsk))
		(void) task_sched_runtime(current);

	guard(rcu)();
	scoped_seqlock_read (&sig->stats_lock, ss_lock_irqsave) {
		times->utime = sig->utime;
		times->stime = sig->stime;
		times->sum_exec_runtime = sig->sum_sched_runtime;

		/* signal 基数覆盖已退出线程，循环只叠加仍在链表中的存活线程。 */
		__for_each_thread(sig, t) {
			task_cputime(t, &utime, &stime);
			times->utime += utime;
			times->stime += stime;
			times->sum_exec_runtime += read_sum_exec_runtime(t);
		}
	}
}

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
/*
 * Account a tick to a process and cpustat
 * @p: the process that the CPU time gets accounted to
 * @user_tick: is the tick from userspace
 * @rq: the pointer to rq
 *
 * Tick demultiplexing follows the order
 * - pending hardirq update
 * - pending softirq update
 * - user_time
 * - idle_time
 * - system time
 *   - check for guest_time
 *   - else account as system_time
 *
 * Check for hardirq is done both for system and user time as there is
 * no timer going off while we are on hardirq and hence we may never get an
 * opportunity to update it solely in system time.
 * p->stime and friends are only updated on system time and not on IRQ
 * softirq as those do not count in task exec_runtime any more.
 */
/*
 * 将 @ticks 个周期先扣除尚未落账的 steal/IRQ，再按 ksoftirqd、user、
 * idle、guest、system 的互斥顺序归类；@p 借用且必须是本 CPU 当前任务。
 */
/*
 * 业务背景：启用精确 IRQ 计费时，批量 tick 要先扣独立时间，再把剩余区间唯一归给当前执行类别。
 * 入参：p 是不可空、本 CPU 当前 task 的借用输入/输出；user_tick 是 0/非零用户态标志；ticks 是正 tick 数量。
 * 出参/返回：无直接返回值、无输出参数；最多记账 TICK_NSEC*ticks 的剩余纳秒到一个 task/cpustat 类别。
 * 注意事项：本 CPU tick 上下文不可睡眠；乘法范围由 tick 子系统约束，精确时间因舍入可吃掉全部批量 tick。
 */
static void irqtime_account_process_tick(struct task_struct *p, int user_tick,
					 int ticks)
{
	u64 other, cputime = TICK_NSEC * ticks;

	/* 批量 tick 仍先一次性领取全部待扣精确时间，舍入超额时本轮不再归 task。 */
	/*
	 * When returning from idle, many ticks can get accounted at
	 * once, including some ticks of steal, IRQ, and softirq time.
	 * Subtract those ticks from the amount of time accounted to
	 * idle, or potentially user or system time. Due to rounding,
	 * other time can exceed ticks occasionally.
	 */
	other = account_other_time(ULONG_MAX);
	if (other >= cputime)
		return;

	cputime -= other;

	/* 剩余时间按优先序进入唯一分支，避免 user/idle/guest/system 相互重叠。 */
	if (this_cpu_ksoftirqd() == p) {
		/*
		 * ksoftirqd time do not get accounted in cpu_softirq_time.
		 * So, we have to handle it separately here.
		 * Also, p->stime needs to be updated for ksoftirqd.
		 */
		account_system_index_time(p, cputime, CPUTIME_SOFTIRQ);
	/* 普通 task 再依次区分采样到的 user、真正 idle、VCPU guest 和 system。 */
	} else if (user_tick) {
		account_user_time(p, cputime);
	} else if (p == this_rq()->idle) {
		account_idle_time(cputime);
	} else if (p->flags & PF_VCPU) { /* System time or guest time */
		account_guest_time(p, cputime);
	} else {
		/* 以上身份均不满足时，剩余 tick 才是普通 task 的 system 时间。 */
		account_system_index_time(p, cputime, CPUTIME_SYSTEM);
	}
}

#else /* !CONFIG_IRQ_TIME_ACCOUNTING: */
/* 配置关闭时 tick 不需要做 IRQ 独立分流，调用者继续普通 tick 计费。 */
/*
 * 业务背景：未启用 IRQ_TIME 时提供同签名空实现，使上层条件代码保持可编译。
 * 入参：p 是未使用的 task 借用输入；user_tick 是未使用分类标志；nr_ticks 是未使用 tick 数。
 * 出参/返回：无直接返回值、无输出参数或状态副作用。
 * 注意事项：仅配置替代实现；调用它不会完成普通 tick 计费，调用者必须由配置路径保证后续分类。
 */
static inline void irqtime_account_process_tick(struct task_struct *p, int user_tick,
						int nr_ticks) { }
#endif /* !CONFIG_IRQ_TIME_ACCOUNTING */

#if defined(CONFIG_NO_HZ_COMMON) && !defined(CONFIG_HAVE_VIRT_CPU_ACCOUNTING_IDLE)
/*
 * 结束 @kc 从 idle_entrytime 到 @now 的动态 tick 空闲段；先消费上轮延迟的
 * steal，避免公开 idle 值倒退，再将本轮 steal 留给下一次扣除。
 */
/*
 * 业务背景：NO_HZ idle 结束或被 IRQ 打断时，要把开放区间提交到 IDLE/IOWAIT 并延迟扣除 steal。
 * 入参：kc 是不可空、本 CPU kernel_cpustat 的借用输入/输出；now 是当前单调纳秒时间点。
 * 出参/返回：无直接返回值、无输出参数；结算开放区间、更新 steal 延迟量并清 idle_elapse。
 * 注意事项：调用者保证本 CPU 串行；seqcount 供远端读者重试，now 需与 idle_entrytime 同一时钟域。
 */
static void kcpustat_idle_stop(struct kernel_cpustat *kc, u64 now)
{
	u64 *cpustat = kc->cpustat;
	u64 delta, steal, steal_delta;
	int iowait;

	if (!kc->idle_elapse)
		return;

	iowait = nr_iowait_cpu(smp_processor_id()) > 0;
	delta = now - kc->idle_entrytime;
	steal = steal_account_process_time(delta);

	/* 写段内先扣上轮欠账，当前新发现的 steal 留到下轮，保证公开累计不倒退。 */
	/*
	 * Record the idle time after substracting the steal time from
	 * previous update sequence. Don't substract the steal time from
	 * the current update sequence to avoid readers moving backward.
	 */
	write_seqcount_begin(&kc->idle_sleeptime_seq);
	steal_delta = min_t(u64, kc->idle_stealtime[iowait], delta);
	delta -= steal_delta;
	kc->idle_stealtime[iowait] -= steal_delta;

	if (iowait)
		cpustat[CPUTIME_IOWAIT] += delta;
	else
		cpustat[CPUTIME_IDLE] += delta;

	/* 保存本轮 steal 债务并关闭区间，读者要么看到完整旧代要么完整新代。 */
	kc->idle_stealtime[iowait] += steal;
	kc->idle_entrytime = now;
	kc->idle_elapse = false;
	write_seqcount_end(&kc->idle_sleeptime_seq);
}

/* 在 seqcount 下开启 @kc 的动态 tick 空闲区间；重复 start 是安全空操作。 */
/*
 * 业务背景：CPU 进入 NO_HZ idle 或 IRQ 返回 idle 时，要记录未提交空闲区间的起点。
 * 入参：kc 是不可空、本 CPU cpustat 的借用输入/输出；now 是当前单调纳秒时间点。
 * 出参/返回：无直接返回值、无输出参数；首次设置 idle_entrytime/idle_elapse，已开启时不变。
 * 注意事项：调用者保证本 CPU 串行；seqcount 只保护远端快照，重复 start 不重置起点以免丢时间。
 */
static void kcpustat_idle_start(struct kernel_cpustat *kc, u64 now)
{
	/* Irqtime accounting might have been enabled in the middle of the IRQ */
	if (kc->idle_elapse)
		return;

	write_seqcount_begin(&kc->idle_sleeptime_seq);
	kc->idle_entrytime = now;
	kc->idle_elapse = true;
	write_seqcount_end(&kc->idle_sleeptime_seq);
}

/* CPU 离开 dyntick idle 时结算空闲段并恢复普通 vtime；@now 为纳秒时间点。 */
/*
 * 业务背景：CPU 从动态 tick 空闲退出时，需要关闭 NO_HZ cpustat 区间并恢复常规 vtime 状态。
 * 入参：now 是退出时的单调纳秒时间点。
 * 出参/返回：无直接返回值、无输出参数；generic vtime 未接管时结算 idle 并清 idle_dyntick。
 * 注意事项：本 CPU idle 边界调用且不可睡眠；状态不匹配会 WARN，generic vtime 分支无副作用。
 */
void kcpustat_dyntick_stop(u64 now)
{
	struct kernel_cpustat *kc = kcpustat_this_cpu;

	if (!vtime_generic_enabled_this_cpu()) {
		WARN_ON_ONCE(!kc->idle_dyntick);
		kcpustat_idle_stop(kc, now);
		kc->idle_dyntick = false;
		vtime_dyntick_stop();
	}
}

/* CPU 进入 dyntick idle 时启动 vtime 与 cpustat 的空闲区间。 */
/*
 * 业务背景：CPU 准备停 tick 时要标记开放 idle 区间，供退出或远端读取时准确补算。
 * 入参：now 是进入时的单调纳秒时间点。
 * 出参/返回：无直接返回值、无输出参数；generic vtime 未接管时启动 dyntick 并设置 idle_dyntick。
 * 注意事项：只在本 CPU idle 边界调用且不可睡眠；generic vtime 自己计费时本函数保持状态不变。
 */
void kcpustat_dyntick_start(u64 now)
{
	struct kernel_cpustat *kc = kcpustat_this_cpu;

	if (!vtime_generic_enabled_this_cpu()) {
		vtime_dyntick_start();
		kc->idle_dyntick = true;
		kcpustat_idle_start(kc, now);
	}
}

/* IRQ 打断 idle 前暂停 NO_HZ 空闲累计，避免同一段时间同时记为 idle 和 IRQ。 */
/*
 * 业务背景：IRQ 进入会让 CPU 暂离 idle，精确 IRQ/架构 vtime 路径必须先截断开放空闲区间。
 * 入参：now 是 IRQ 进入边界的单调纳秒时间点。
 * 出参/返回：无直接返回值、无输出参数；满足配置时调用 idle_stop 结算当前区间。
 * 注意事项：IRQ 本 CPU 上下文不可睡眠；generic vtime 已独立处理时不操作，enter/exit 必须配对。
 */
void kcpustat_irq_enter(u64 now)
{
	struct kernel_cpustat *kc = kcpustat_this_cpu;

	if (!vtime_generic_enabled_this_cpu() &&
	    (irqtime_enabled() || vtime_accounting_enabled_this_cpu()))
		kcpustat_idle_stop(kc, now);
}

/* IRQ 返回 idle 后重新开始 NO_HZ 空闲累计；运行时禁用 irqtime 也必须配平 enter。 */
/*
 * 业务背景：IRQ 从 dyntick idle 返回后要重新建立空闲起点，避免 IRQ 后的 idle 时间漏计。
 * 入参：now 是 IRQ 退出边界的单调纳秒时间点。
 * 出参/返回：无直接返回值、无输出参数；满足配置时重新开启本 CPU idle 区间。
 * 注意事项：IRQ 本 CPU 上下文不可睡眠；使用编译期 IRQ_TIME 条件以覆盖 IRQ 中途运行时关闭的情况。
 */
void kcpustat_irq_exit(u64 now)
{
	struct kernel_cpustat *kc = kcpustat_this_cpu;

	/*
	 * Generic vtime already does its own idle accounting.
	 * But irqtime accounting or arch vtime which also accounts IRQs
	 * need to pause nohz accounting. Resume nohz accounting as long
	 * as the irqtime config is enabled to handle case where irqtime
	 * accounting got runtime disabled in the middle of an IRQ.
	 */
	if (!vtime_generic_enabled_this_cpu() &&
	    (IS_ENABLED(CONFIG_IRQ_TIME_ACCOUNTING) || vtime_accounting_enabled_this_cpu()))
		kcpustat_idle_start(kc, now);
}

/*
 * 读取 @cpu 的 @idx 累计值，并在请求时补算尚未 stop 的当前空闲段；
 * seqcount 失败则重试，因此不会组合 start/stop 两代状态。
 */
/*
 * 业务背景：远端读取 NO_HZ CPU 的 idle/iowait 时，要把尚未 stop 的开放区间临时加入已提交值。
 * 入参：cpu 是有效 CPU 编号；idx 是 IDLE 或 IOWAIT；compute_delta 决定是否补算；now 是读取纳秒时间点。
 * 出参/返回：返回目标字段的累计纳秒快照；无输出参数，可能包含未落账区间但不修改源状态。
 * 注意事项：seqcount 失败会重试；nr_iowait 分类由上层近似决定，now 必须与 entrytime 同时钟域。
 */
static u64 kcpustat_field_dyntick(int cpu, enum cpu_usage_stat idx,
				  bool compute_delta, u64 now)
{
	struct kernel_cpustat *kc = &kcpustat_cpu(cpu);
	int iowait = idx == CPUTIME_IOWAIT;
	u64 *cpustat = kc->cpustat;
	unsigned int seq;
	u64 idle;

	do {
		seq = read_seqcount_begin(&kc->idle_sleeptime_seq);

		/* 已提交值为基数，开放区间只在调用者允许且时间有效时临时补入。 */
		idle = cpustat[idx];

		if (kc->idle_elapse && compute_delta && now > kc->idle_entrytime) {
			u64 delta = now - kc->idle_entrytime;

			delta -= min_t(u64, kc->idle_stealtime[iowait], delta);
			idle += delta;
		}
	} while (read_seqcount_retry(&kc->idle_sleeptime_seq, seq));

	return idle;
}

/* 返回 @cpu 的累计 idle 纳秒；有 iowait 时不把当前未结算区间算入 idle。 */
/*
 * 业务背景：内核消费者需要读取指定 CPU 自启动以来的 idle 总量并补齐当前 NO_HZ 区间。
 * 入参：cpu 是有效且 cpustat 可访问的 CPU 编号。
 * 出参/返回：返回累计 idle 纳秒；无输出参数或 ownership 变化。
 * 注意事项：远端 nr_iowait 是无同步近似快照，结果可能短暂不单调；函数可在 seqcount 冲突时重试。
 */
u64 kcpustat_field_idle(int cpu)
{
	return kcpustat_field_dyntick(cpu, CPUTIME_IDLE,
				      !nr_iowait_cpu(cpu), ktime_get());
}
EXPORT_SYMBOL_GPL(kcpustat_field_idle);

/* 返回 @cpu 的累计 iowait 纳秒；仅在当前存在 iowait waiter 时补算开放区间。 */
/*
 * 业务背景：内核消费者需要读取指定 CPU 自启动以来的 iowait 近似累计值。
 * 入参：cpu 是有效且 cpustat 可访问的 CPU 编号。
 * 出参/返回：返回累计 iowait 纳秒；无输出参数或 ownership 变化。
 * 注意事项：nr_iowait 可远端变化，结果允许短暂倒退或误分类；可能因 seqcount 写者而重试。
 */
u64 kcpustat_field_iowait(int cpu)
{
	return kcpustat_field_dyntick(cpu, CPUTIME_IOWAIT,
				      nr_iowait_cpu(cpu), ktime_get());
}
EXPORT_SYMBOL_GPL(kcpustat_field_iowait);
#else
/* 无 NO_HZ 补算能力时直接返回已提交 cpustat 字段，其他参数不改变结果。 */
/*
 * 业务背景：缺少 NO_HZ idle 补算配置时保留统一字段读取接口，只能使用已提交统计。
 * 入参：cpu 是有效 CPU 编号；idx 是有效 cpustat 类别；compute_delta 与 now 是未使用输入。
 * 出参/返回：返回 kcpustat_cpu(cpu).cpustat[idx] 纳秒快照；无输出参数或状态副作用。
 * 注意事项：配置替代实现不会补开放区间；调用者必须保证 cpu/index 范围，快照可立即变旧。
 */
static u64 kcpustat_field_dyntick(int cpu, enum cpu_usage_stat idx,
				  bool compute_delta, ktime_t now)
{
	return kcpustat_cpu(cpu).cpustat[idx];
}
#endif /* CONFIG_NO_HZ_COMMON && !CONFIG_HAVE_VIRT_CPU_ACCOUNTING_IDLE */

/* 将指定 sleep 字段转换为微秒；可选输出 @last_update_time，不转移任何所有权。 */
/*
 * 业务背景：cpufreq 等用户需要以微秒读取 idle/iowait，并同时获得该快照的时间基线。
 * 入参：cpu 是有效 CPU 编号；idx 是 IDLE/IOWAIT；compute_delta 控制补算；last_update_time 是可空微秒输出指针。
 * 出参/返回：返回累计微秒；非空 last_update_time 被写为本次 ktime 微秒，指针 ownership 不变。
 * 注意事项：整数除法截断不足一微秒部分；generic vtime 与 dyntick 路径的同步保证不同，结果是快照。
 */
static u64 get_cpu_sleep_time_us(int cpu, enum cpu_usage_stat idx,
				 bool compute_delta, u64 *last_update_time)
{
	ktime_t now = ktime_get();
	u64 res;

	/* generic vtime 从当前 task 补算，其他配置从 NO_HZ idle 区间补算。 */
	if (vtime_generic_enabled_cpu(cpu))
		res = kcpustat_field(idx, cpu);
	else
		res = kcpustat_field_dyntick(cpu, idx, compute_delta, now);

	do_div(res, NSEC_PER_USEC);

	if (last_update_time)
		*last_update_time = ktime_to_us(now);

	return res;
}

/**
 * get_cpu_idle_time_us - get the total idle time of a CPU
 * @cpu: CPU number to query
 * @last_update_time: variable to store update time in. Do not update
 * counters if NULL.
 *
 * Return the cumulative idle time (since boot) for a given
 * CPU, in microseconds. Note that this is partially broken due to
 * the counter of iowait tasks that can be remotely updated without
 * any synchronization. Therefore it is possible to observe backward
 * values within two consecutive reads.
 *
 * This time is measured via accounting rather than sampling,
 * and is as accurate as ktime_get() is.
 *
 * Return: total idle time of the @cpu
 */
/* 返回 @cpu 自启动以来累计 idle 微秒；远端 iowait 竞态允许相邻读数暂时倒退。 */
/*
 * 业务背景：CPU idle 统计的公共导出接口需按调用者是否需要更新时间决定是否补开放区间。
 * 入参：cpu 是有效 CPU 编号；last_update_time 是可空输出，非空时接收当前微秒时间。
 * 出参/返回：返回该 CPU 累计 idle 微秒；可选输出被覆盖，ownership 不转移。
 * 注意事项：传 NULL 按 API 契约不更新开放 counter；远端 iowait 竞态使相邻结果可能暂时倒退。
 */
u64 get_cpu_idle_time_us(int cpu, u64 *last_update_time)
{
	return get_cpu_sleep_time_us(cpu, CPUTIME_IDLE,
				     !nr_iowait_cpu(cpu), last_update_time);
}
EXPORT_SYMBOL_GPL(get_cpu_idle_time_us);

/**
 * get_cpu_iowait_time_us - get the total iowait time of a CPU
 * @cpu: CPU number to query
 * @last_update_time: variable to store update time in. Do not update
 * counters if NULL.
 *
 * Return the cumulative iowait time (since boot) for a given
 * CPU, in microseconds. Note this is partially broken due to
 * the counter of iowait tasks that can be remotely updated without
 * any synchronization. Therefore it is possible to observe backward
 * values within two consecutive reads.
 *
 * This time is measured via accounting rather than sampling,
 * and is as accurate as ktime_get() is.
 *
 * Return: total iowait time of @cpu
 */
/* 返回 @cpu 自启动以来累计 iowait 微秒；@last_update_time 为可空输出参数。 */
/*
 * 业务背景：CPU iowait 统计的公共导出接口为监控和调频提供自启动累计微秒值。
 * 入参：cpu 是有效 CPU 编号；last_update_time 是可空输出，非空时接收当前微秒时间。
 * 出参/返回：返回该 CPU 累计 iowait 微秒；可选输出被覆盖，ownership 不转移。
 * 注意事项：nr_iowait 无同步远端更新令结果仅为近似且可能倒退；NULL 时不补开放区间。
 */
u64 get_cpu_iowait_time_us(int cpu, u64 *last_update_time)
{
	return get_cpu_sleep_time_us(cpu, CPUTIME_IOWAIT,
				     nr_iowait_cpu(cpu), last_update_time);
}
EXPORT_SYMBOL_GPL(get_cpu_iowait_time_us);

/*
 * Use precise platform statistics if available:
 */
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_NATIVE

/* 原生 vtime 在 IRQ 边界按嵌套状态结算 hardirq、softirq、idle 或 kernel 时间。 */
/*
 * 业务背景：原生精确 vtime 架构在 IRQ 边界要按实际嵌套和 idle 状态提交离开区间。
 * 入参：tsk 是不可空、本 CPU 当前 task 的借用输入/输出；offset 是从 irq_count 扣除的调用层级。
 * 出参/返回：无直接返回值、无输出参数；调用恰当 hardirq/softirq/idle/kernel 计费或重置 vtime。
 * 注意事项：仅 NATIVE 配置且运行在 IRQ 边界不可睡眠；错误 offset 或 idle 状态会造成错误分类。
 */
void vtime_account_irq(struct task_struct *tsk, unsigned int offset)
{
	unsigned int pc = irq_count() - offset;

	/* IRQ 嵌套优先；非 IRQ 时再区分非 dyntick idle 的 idle/kernel 与纯 idle 重置。 */
	if (pc & HARDIRQ_OFFSET) {
		vtime_account_hardirq(tsk);
	} else if (pc & SOFTIRQ_OFFSET) {
		vtime_account_softirq(tsk);
	/* 未处于 IRQ 嵌套时，dyntick 标记决定应结算 task 还是只重置边界。 */
	} else if (!kcpustat_idle_dyntick()) {
		if (!IS_ENABLED(CONFIG_HAVE_VIRT_CPU_ACCOUNTING_IDLE) &&
		    is_idle_task(tsk)) {
			vtime_account_idle(tsk);
		} else {
			/* 架构自行处理 idle 或当前非 idle 时，按普通 kernel 区间结算。 */
			vtime_account_kernel(tsk);
		}
	} else {
		vtime_reset();
	}
}

/* 原生精确计费无需缩放；把 @curr 的 user/system 纳秒原样写到输出 @ut/@st。 */
/*
 * 业务背景：原生 vtime 已精确累计 user/system，不需要 tick 路径按调度运行时间做比例校正。
 * 入参：curr 是不可空只读原始统计；prev 是未使用的兼容参数；ut/st 是不可空、调用者拥有的输出指针。
 * 出参/返回：无直接返回值；分别覆盖 *ut/*st 为 curr 的纳秒值，所有指针 ownership 不变。
 * 注意事项：仅 NATIVE 配置实现；调用者保证 curr 快照一致，输出指针不可别名到会破坏输入的位置。
 */
void cputime_adjust(struct task_cputime *curr, struct prev_cputime *prev,
		    u64 *ut, u64 *st)
{
	*ut = curr->utime;
	*st = curr->stime;
}

/* 返回单 task 已精确累计的 user/system 纳秒；输出指针必须非空。 */
/*
 * 业务背景：原生 vtime 下导出单 task CPU 时间可直接读取已精确维护的两个累计字段。
 * 入参：p 是不可空且生命周期稳定的只读借用 task；ut/st 是不可空、调用者拥有的输出指针。
 * 出参/返回：无直接返回值；*ut/*st 分别得到 p 的 user/system 纳秒快照，ownership 不变。
 * 注意事项：仅 NATIVE 配置；不刷新正在运行的开放区间，调用者需接受瞬时快照语义。
 */
void task_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	*ut = p->utime;
	*st = p->stime;
}
EXPORT_SYMBOL_GPL(task_cputime_adjusted);

/* 汇总 @p 整个线程组并输出精确 user/system 纳秒；不取得 task 引用。 */
/*
 * 业务背景：原生 vtime 消费者还需要线程组维度的精确 user/system 总量。
 * 入参：p 是不可空、目标线程组成员的借用输入；ut/st 是不可空、调用者拥有的输出指针。
 * 出参/返回：无直接返回值；覆盖 *ut/*st 为线程组累计纳秒，ownership 不变。
 * 注意事项：内部遍历语义继承 thread_group_cputime，其他 CPU 的开放区间可能未强制刷新。
 */
void thread_group_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	struct task_cputime cputime;

	thread_group_cputime(p, &cputime);

	*ut = cputime.utime;
	*st = cputime.stime;
}

#else /* !CONFIG_VIRT_CPU_ACCOUNTING_NATIVE: */

/*
 * Account a single tick of CPU time.
 * @p: the process that the CPU time gets accounted to
 * @user_tick: indicates if the tick is a user or a system tick
 */
/*
 * 普通 tick 计费入口；vtime/NO_HZ idle 已接管时立即返回，否则依次扣 steal、
 * 选择 IRQ 精确路径或把一个 TICK_NSEC 归入 user/system/idle。
 */
/*
 * 业务背景：没有原生 vtime 时，周期 tick 是普通 task/user/system/idle CPU 时间的主要采样入口。
 * 入参：p 是不可空、本 CPU 当前 task 的借用输入/输出；user_tick 是 0/非零的用户态采样标志。
 * 出参/返回：无直接返回值、无输出参数；可能扣 steal/IRQ 后把至多一个 tick 归入唯一类别。
 * 注意事项：tick/本 CPU 原子上下文不可睡眠；vtime 或 dyntick idle 接管时必须返回避免双计。
 */
void account_process_tick(struct task_struct *p, int user_tick)
{
	u64 cputime, steal;

	/* 精确 vtime 或 NO_HZ idle 已拥有该区间，tick 路径必须完全退出。 */
	if (vtime_accounting_enabled_this_cpu())
		return;

	if (kcpustat_idle_dyntick())
		return;

	if (irqtime_enabled()) {
		irqtime_account_process_tick(p, user_tick, 1);
		return;
	}

	/* 普通路径先领取 steal；它覆盖整个 tick 时不再给当前 task 分配时间。 */
	cputime = TICK_NSEC;
	steal = steal_account_process_time(ULONG_MAX);

	if (steal >= cputime)
		return;

	cputime -= steal;

	/* 最终按采样态和 idle/IRQ 身份把剩余纳秒投入且只投入一个类别。 */
	if (user_tick)
		account_user_time(p, cputime);
	else if ((p != this_rq()->idle) || (irq_count() != HARDIRQ_OFFSET))
		account_system_time(p, HARDIRQ_OFFSET, cputime);
	else
		account_idle_time(cputime);
}

/*
 * Adjust tick based cputime random precision against scheduler runtime
 * accounting.
 *
 * Tick based cputime accounting depend on random scheduling timeslices of a
 * task to be interrupted or not by the timer.  Depending on these
 * circumstances, the number of these interrupts may be over or
 * under-optimistic, matching the real user and system cputime with a variable
 * precision.
 *
 * Fix this by scaling these tick based values against the total runtime
 * accounted by the CFS scheduler.
 *
 * This code provides the following guarantees:
 *
 *   stime + utime == rtime
 *   stime_i+1 >= stime_i, utime_i+1 >= utime_i
 *
 * Assuming that rtime_i+1 >= rtime_i.
 */
/*
 * 用调度器精确的 @curr->sum_exec_runtime 缩放 tick 抽样的 user/system 比例；
 * @prev 的锁串行并发读者，输出保证 ut+st=rtime 且两者各自不倒退。
 */
/*
 * 业务背景：tick 抽样的 user/system 比例有随机误差，需用精确 scheduler runtime 缩放并保持导出单调。
 * 入参：curr 是不可空只读当前统计；prev 是不可空持久校正状态；ut/st 是不可空调用者输出指针。
 * 出参/返回：无直接返回值；更新 prev 并输出纳秒，保证 ut+st=sum_runtime 且两者不倒退。
 * 注意事项：会 irqsave 获取 prev->lock；所有同一实体读者必须共享 prev，否则单调保证失效。
 */
void cputime_adjust(struct task_cputime *curr, struct prev_cputime *prev,
		    u64 *ut, u64 *st)
{
	u64 rtime, stime, utime;
	unsigned long flags;

	/* Serialize concurrent callers such that we can honour our guarantees */
	raw_spin_lock_irqsave(&prev->lock, flags);
	rtime = curr->sum_exec_runtime;

	/* 已导出总和覆盖当前 runtime 时沿用旧值，过滤倒退输入或锁竞争导致的旧快照。 */
	/*
	 * This is possible under two circumstances:
	 *  - rtime isn't monotonic after all (a bug);
	 *  - we got reordered by the lock.
	 *
	 * In both cases this acts as a filter such that the rest of the code
	 * can assume it is monotonic regardless of anything else.
	 */
	if (prev->stime + prev->utime >= rtime)
		goto out;

	stime = curr->stime;
	utime = curr->utime;

	/* 尚未采到某一类别时把全部 runtime 暂归另一侧，之后随新 tick 比例逐步收敛。 */
	/*
	 * If either stime or utime are 0, assume all runtime is userspace.
	 * Once a task gets some ticks, the monotonicity code at 'update:'
	 * will ensure things converge to the observed ratio.
	 */
	if (stime == 0) {
		utime = rtime;
		goto update;
	}

	if (utime == 0) {
		stime = rtime;
		goto update;
	}

	stime = mul_u64_u64_div_u64(stime, rtime, stime + utime);

update:
	/* 先固定 system 下界，再用精确总量求 user，因而始终保持两者之和等于 rtime。 */
	/*
	 * Make sure stime doesn't go backwards; this preserves monotonicity
	 * for utime because rtime is monotonic.
	 *
	 *  utime_i+1 = rtime_i+1 - stime_i
	 *            = rtime_i+1 - (rtime_i - utime_i)
	 *            = (rtime_i+1 - rtime_i) + utime_i
	 *            >= utime_i
	 */
	if (stime < prev->stime)
		stime = prev->stime;
	utime = rtime - stime;

	/* 若反算 user 仍倒退，则固定 user 下界并把差额重新归回 system。 */
	/*
	 * Make sure utime doesn't go backwards; this still preserves
	 * monotonicity for stime, analogous argument to above.
	 */
	if (utime < prev->utime) {
		utime = prev->utime;
		stime = rtime - utime;
	}

	prev->stime = stime;
	prev->utime = utime;
out:
	/* 所有出口都从持久 prev 输出，锁内完成发布以串行并发读者。 */
	*ut = prev->utime;
	*st = prev->stime;
	raw_spin_unlock_irqrestore(&prev->lock, flags);
}

/* 获取 @p 的原始快照，必要时刷新运行中 task，再经 prev_cputime 输出单调结果。 */
/*
 * 业务背景：非原生计费下，单 task 导出值要合并开放 vtime 并用 scheduler runtime 校正 tick 误差。
 * 入参：p 是不可空且生命周期稳定的借用输入/输出 task；ut/st 是不可空调用者输出指针。
 * 出参/返回：无直接返回值；输出单调 user/system 纳秒，必要时刷新 p 的 runtime 快照。
 * 注意事项：可能获取 task rq 锁和 prev_cputime 锁；输出 ownership 不转移，不能在冲突锁上下文调用。
 */
void task_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	struct task_cputime cputime = {
		.sum_exec_runtime = p->se.sum_exec_runtime,
	};

	if (task_cputime(p, &cputime.utime, &cputime.stime))
		cputime.sum_exec_runtime = task_sched_runtime(p);
	cputime_adjust(&cputime, &p->prev_cputime, ut, st);
}
EXPORT_SYMBOL_GPL(task_cputime_adjusted);

/* 汇总线程组后用 signal 共享的 prev_cputime 输出线程组级单调 user/system 时间。 */
/*
 * 业务背景：线程组 CPU 时间导出也要校正 tick 抽样比例，并在多个并发读者间保持单调。
 * 入参：p 是不可空目标线程组成员的借用输入；ut/st 是不可空调用者拥有的输出指针。
 * 出参/返回：无直接返回值；输出线程组校正后的 user/system 纳秒并更新 signal 共享 prev 状态。
 * 注意事项：内部执行 RCU/seqcount 汇总和 raw spinlock 校正；其他 CPU 开放时间仍可能滞后。
 */
void thread_group_cputime_adjusted(struct task_struct *p, u64 *ut, u64 *st)
{
	struct task_cputime cputime;

	thread_group_cputime(p, &cputime);
	cputime_adjust(&cputime, &p->signal->prev_cputime, ut, st);
}
#endif /* !CONFIG_VIRT_CPU_ACCOUNTING_NATIVE */

#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
/* 返回从 @vtime->starttime 到当前 sched_clock 的非负纳秒差；时钟倒退时返回 0。 */
/*
 * 业务背景：generic vtime 需要按 sched_clock 计算当前开放执行区间的墙钟增量。
 * 入参：vtime 是不可空、由调用者稳定的只读借用状态，starttime 使用 sched_clock 纳秒域。
 * 出参/返回：返回当前时钟减 starttime 的非负纳秒；时钟倒退返回 0，无输出参数。
 * 注意事项：不更新 starttime且快照可立即变旧；调用者负责 seqcount/本地串行，不能混用其他时钟域。
 */
static u64 vtime_delta(struct vtime *vtime)
{
	unsigned long long clock;

	clock = sched_clock();
	if (clock < vtime->starttime)
		return 0;

	return clock - vtime->starttime;
}

/* 领取当前 vtime 区间并扣除 steal/IRQ；推进 starttime，返回可归 task 的纳秒。 */
/*
 * 业务背景：提交一个 generic-vtime 区间前，要先剔除已独立记账的 steal/IRQ 时间。
 * 入参：vtime 是不可空、本 CPU 当前 task 的借用输入/输出状态。
 * 出参/返回：返回可归 task/idle 的非负纳秒；推进 starttime，并可能推进 steal/IRQ 游标。
 * 注意事项：要求 vtime 非 INACTIVE 且本地计费串行；account_other_time 需关中断，舍入由 delta 上限约束。
 */
static u64 get_vtime_delta(struct vtime *vtime)
{
	u64 delta = vtime_delta(vtime);
	u64 other;

	/*
	 * Unlike tick based timing, vtime based timing never has lost
	 * ticks, and no need for steal time accounting to make up for
	 * lost ticks. Vtime accounts a rounded version of actual
	 * elapsed time. Limit account_other_time to prevent rounding
	 * errors from causing elapsed vtime to go negative.
	 */
	other = account_other_time(delta);
	WARN_ON_ONCE(vtime->state == VTIME_INACTIVE);
	vtime->starttime += delta;

	return delta - other;
}

/* 累积 system 残量，达到一个 tick 后批量提交给 task/cpustat 并清空残量。 */
/*
 * 业务背景：generic vtime 用残量聚合小区间，达到 tick 粒度后再更新较重的 task/cgroup 统计。
 * 入参：tsk 是不可空当前 task 的借用输入/输出；vtime 是其不可空、已由写序列稳定的输入/输出状态。
 * 出参/返回：无直接返回值、无输出参数；累加 stime，达阈值时提交全部残量并清零。
 * 注意事项：调用者持 vtime seqcount 写段且本地中断状态满足计费要求；未达阈值时间仅存在 vtime 中。
 */
static void vtime_account_system(struct task_struct *tsk,
				 struct vtime *vtime)
{
	vtime->stime += get_vtime_delta(vtime);
	if (vtime->stime >= TICK_NSEC) {
		account_system_time(tsk, irq_count(), vtime->stime);
		vtime->stime = 0;
	}
}

/* 累积 guest 残量，达到一个 tick 后批量提交并清空；@tsk/@vtime 均为借用。 */
/*
 * 业务背景：guest generic-vtime 同样批量提交，以减少每个用户/客体边界的统计更新成本。
 * 入参：tsk 是不可空 VCPU task 的借用输入/输出；vtime 是其不可空、写序列内输入/输出状态。
 * 出参/返回：无直接返回值、无输出参数；累加 gtime，达一个 tick 后提交并清零残量。
 * 注意事项：只应在 VTIME_GUEST 相关路径使用；调用者保证 seqcount 写串行，错误调用会误增 user/guest。
 */
static void vtime_account_guest(struct task_struct *tsk,
				struct vtime *vtime)
{
	vtime->gtime += get_vtime_delta(vtime);
	if (vtime->gtime >= TICK_NSEC) {
		account_guest_time(tsk, vtime->gtime);
		vtime->gtime = 0;
	}
}

/* 根据旧状态结算 guest 或普通 kernel 区间，用于切换/内核边界的共同内部入口。 */
/*
 * 业务背景：调度切出和内核边界都需根据被结算 task 的旧 vtime 状态选择 guest 或 system 账户。
 * 入参：tsk 是不可空当前/切出 task 的借用输入/输出；vtime 是其不可空输入/输出计费状态。
 * 出参/返回：无直接返回值、无输出参数；把开放区间累入 guest 或 system 残量/统计。
 * 注意事项：调用者必须持 vtime seqcount 写段；除 GUEST 外均按 system 处理，INACTIVE 传入属于误用。
 */
static void __vtime_account_kernel(struct task_struct *tsk,
				   struct vtime *vtime)
{
	/* We might have scheduled out from guest path */
	if (vtime->state == VTIME_GUEST)
		vtime_account_guest(tsk, vtime);
	else
		vtime_account_system(tsk, vtime);
}

/* 在 seqcount 写段内结算运行 task 的 kernel/guest 时间；零增量走快速返回。 */
/*
 * 业务背景：generic vtime 的内核计费入口需提交 task 自上次边界以来的 kernel 或 guest 区间。
 * 入参：tsk 是不可空、本 CPU 当前 task 的借用输入/输出对象。
 * 出参/返回：无直接返回值、无输出参数；非零区间被累加，零区间不触碰 seqcount。
 * 注意事项：本 CPU 原子计费上下文不可睡眠；seqcount 让远端读者重试，但 task 生命周期由调用者保证。
 */
void vtime_account_kernel(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;

	if (!vtime_delta(vtime))
		return;

	write_seqcount_begin(&vtime->seqcount);
	__vtime_account_kernel(tsk, vtime);
	write_seqcount_end(&vtime->seqcount);
}

/* 进入用户态前结算 system 区间并发布 VTIME_USER，新区间从更新后的 starttime 起算。 */
/*
 * 业务背景：系统调用/中断返回用户态时，必须封口内核区间并把后续时间标记为 USER。
 * 入参：tsk 是不可空、本 CPU 当前 task 的借用输入/输出对象。
 * 出参/返回：无直接返回值、无输出参数；提交 system 增量并在同一写序列设置 VTIME_USER。
 * 注意事项：只能在本 task 用户边界调用且不可睡眠；状态和 starttime 发布必须保持同一 seqcount 事务。
 */
void vtime_user_enter(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;

	write_seqcount_begin(&vtime->seqcount);
	vtime_account_system(tsk, vtime);
	vtime->state = VTIME_USER;
	write_seqcount_end(&vtime->seqcount);
}

/* 离开用户态时结算 user 残量并切回 VTIME_SYS；seqcount 让远端读取整体重试。 */
/*
 * 业务背景：陷入内核前要把开放用户区间纳入残量/统计，并标记后续时间属于 system。
 * 入参：tsk 是不可空、本 CPU 当前 task 的借用输入/输出对象。
 * 出参/返回：无直接返回值、无输出参数；累加 utime，达阈值时提交，并设置 VTIME_SYS。
 * 注意事项：本 CPU 边界上下文不可睡眠；必须与 user_enter 成对，错误状态会造成区间错分。
 */
void vtime_user_exit(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;

	write_seqcount_begin(&vtime->seqcount);
	vtime->utime += get_vtime_delta(vtime);
	/* 只在累计达到 tick 粒度时下沉到 task/cpustat，余数留待下次边界。 */
	if (vtime->utime >= TICK_NSEC) {
		account_user_time(tsk, vtime->utime);
		vtime->utime = 0;
	}
	vtime->state = VTIME_SYS;
	write_seqcount_end(&vtime->seqcount);
}

/*
 * 进入 guest 前在同一 seqcount 写段中结算 system、设置 PF_VCPU 与
 * VTIME_GUEST；该顺序使 task_gtime() 不会漏掉无 tick 的运行区间。
 */
/*
 * 业务背景：KVM 等进入 guest 前要封口 system 区间，并原子发布 PF_VCPU/VTIME_GUEST 供无 tick 读者识别。
 * 入参：tsk 是不可空、本 CPU 当前 VCPU task 的借用输入/输出对象。
 * 出参/返回：无直接返回值、无输出参数；提交 system 时间并设置 PF_VCPU 与 GUEST 状态。
 * 注意事项：seqcount 写序不可拆分；只能在本 task guest-enter 边界调用，重复进入会破坏分类。
 */
void vtime_guest_enter(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;
	/*
	 * The flags must be updated under the lock with
	 * the vtime_starttime flush and update.
	 * That enforces a right ordering and update sequence
	 * synchronization against the reader (task_gtime())
	 * that can thus safely catch up with a tickless delta.
	 */
	write_seqcount_begin(&vtime->seqcount);
	vtime_account_system(tsk, vtime);
	tsk->flags |= PF_VCPU;
	vtime->state = VTIME_GUEST;
	write_seqcount_end(&vtime->seqcount);
}
EXPORT_SYMBOL_GPL(vtime_guest_enter);

/* 离开 guest 时先提交 guest 区间，再清 PF_VCPU 并发布 VTIME_SYS。 */
/*
 * 业务背景：退出 guest 回到宿主内核时，要把开放客体区间提交并恢复普通 system 状态。
 * 入参：tsk 是不可空、本 CPU 当前 VCPU task 的借用输入/输出对象。
 * 出参/返回：无直接返回值、无输出参数；提交 guest 时间、清 PF_VCPU 并设置 VTIME_SYS。
 * 注意事项：必须与 guest_enter 配对且在本 task 边界调用；三个状态变化需处于同一 seqcount 写段。
 */
void vtime_guest_exit(struct task_struct *tsk)
{
	struct vtime *vtime = &tsk->vtime;

	write_seqcount_begin(&vtime->seqcount);
	vtime_account_guest(tsk, vtime);
	tsk->flags &= ~PF_VCPU;
	vtime->state = VTIME_SYS;
	write_seqcount_end(&vtime->seqcount);
}
EXPORT_SYMBOL_GPL(vtime_guest_exit);

/* 将当前 idle vtime 区间一次性归入 idle/iowait，调用者持有 vtime 写序列。 */
/*
 * 业务背景：generic-vtime idle task 切出时，要把开放区间按当前 iowait 快照提交到 cpustat。
 * 入参：vtime 是不可空、当前 idle task 的借用输入/输出状态。
 * 出参/返回：无直接返回值、无输出参数；领取区间并累加本 CPU IDLE 或 IOWAIT。
 * 注意事项：调用者持 seqcount 写段且状态为 VTIME_IDLE；nr_iowait 分类是近似值，函数不可睡眠。
 */
static void __vtime_account_idle(struct vtime *vtime)
{
	account_idle_time(get_vtime_delta(vtime));
}

/*
 * context switch 提交 @prev 的最后区间并标成 INACTIVE，再按 next 身份建立
 * 新状态/starttime/cpu；两次独立 seqcount 写段供远端 cpustat 读者检测竞态。
 */
/*
 * 业务背景：context switch 是 generic vtime ownership 从 prev 转给 current 的提交点，需同时封口旧区间和开启新区间。
 * 入参：prev 是不可空、正在切出的 task 借用输入/输出；隐式 current 是正在切入的 task。
 * 出参/返回：无直接返回值、无输出参数；prev 变 INACTIVE/cpu=-1，current 获得新状态、起点和本 CPU 编号。
 * 注意事项：调用者处于调度切换受保护上下文；两个 seqcount 写段间的窗口由读者 -EAGAIN 重试处理。
 */
void vtime_task_switch_generic(struct task_struct *prev)
{
	struct vtime *vtime = &prev->vtime;

	/* 第一写段封口 prev 的最后区间，并撤销它与该 CPU 的活动关联。 */
	write_seqcount_begin(&vtime->seqcount);
	if (vtime->state == VTIME_IDLE)
		__vtime_account_idle(vtime);
	else
		__vtime_account_kernel(prev, vtime);
	vtime->state = VTIME_INACTIVE;
	vtime->cpu = -1;
	write_seqcount_end(&vtime->seqcount);

	vtime = &current->vtime;

	/* 第二写段按 current 身份建立新区间；此时远端读者已能识别切换窗口。 */
	write_seqcount_begin(&vtime->seqcount);
	if (is_idle_task(current))
		vtime->state = VTIME_IDLE;
	else if (current->flags & PF_VCPU)
		vtime->state = VTIME_GUEST;
	else
		vtime->state = VTIME_SYS;
	/* 起点与 cpu 归属最后一起发布，后续 delta 才有明确的时钟和账户。 */
	vtime->starttime = sched_clock();
	vtime->cpu = smp_processor_id();
	write_seqcount_end(&vtime->seqcount);
}

/* 初始化 @cpu 的 idle task vtime 基线；本地关中断防止初始化被 IRQ 计费打断。 */
/*
 * 业务背景：CPU idle task 建立时要预置 generic-vtime 状态，后续首次切换才能正确累计 idle。
 * 入参：t 是不可空、目标 CPU idle task 的借用输入/输出；cpu 是与该 task 对应的有效 CPU 编号。
 * 出参/返回：无直接返回值、无输出参数；设置 IDLE 状态、sched_clock 起点和 cpu 字段。
 * 注意事项：函数本地 irqsave 并写 seqcount；调用者保证 t 未并发运行且 cpu/task 配对正确。
 */
void vtime_init_idle(struct task_struct *t, int cpu)
{
	struct vtime *vtime = &t->vtime;
	unsigned long flags;

	/* 关中断覆盖整段初始化，防止 IRQ 路径观察到只写一半的 idle 状态。 */
	local_irq_save(flags);
	write_seqcount_begin(&vtime->seqcount);
	vtime->state = VTIME_IDLE;
	vtime->starttime = sched_clock();
	vtime->cpu = cpu;
	write_seqcount_end(&vtime->seqcount);
	local_irq_restore(flags);
}

/* 返回 @t 已提交与当前未提交 guest 时间之和；seqcount 失败则重新获取完整快照。 */
/*
 * 业务背景：读取 task guest 时间时，NO_HZ guest 可能仍有未达到 tick 的开放区间需要补入。
 * 入参：t 是不可空且生命周期稳定的只读借用 task。
 * 出参/返回：返回累计 guest 纳秒；vtime 关闭时仅返回已提交 gtime，无输出参数。
 * 注意事项：seqcount 冲突会重试；结果是瞬时快照且不提交残量，调用者不获得 task 引用。
 */
u64 task_gtime(struct task_struct *t)
{
	struct vtime *vtime = &t->vtime;
	unsigned int seq;
	u64 gtime;

	if (!vtime_accounting_enabled())
		return t->gtime;

	/* 已提交 gtime 为基数，只有活动 GUEST 状态才叠加残量和开放 delta。 */
	do {
		seq = read_seqcount_begin(&vtime->seqcount);

		gtime = t->gtime;
		if (vtime->state == VTIME_GUEST)
			gtime += vtime->gtime + vtime_delta(vtime);

		/* 任一字段在读取期间改变都丢弃整次组合，防止跨代拼接。 */
	} while (read_seqcount_retry(&vtime->seqcount, seq));

	return gtime;
}

/*
 * Fetch cputime raw values from fields of task_struct and
 * add up the pending nohz execution time since the last
 * cputime snapshot.
 */
/*
 * 输出 @t 的原始 user/system 纳秒；vtime 活跃时把开放区间加入对应输出并
 * 返回 true，睡眠/未启用时返回 false。输出仅是瞬时快照，不取得引用。
 */
/*
 * 业务背景：task CPU 时间读取需在不打断 NO_HZ CPU 的情况下，把当前开放 user/system 区间加入快照。
 * 入参：t 是不可空且稳定的只读借用 task；utime/stime 是不可空、调用者拥有的纳秒输出指针。
 * 出参/返回：活动 vtime 被补算返回 true，未启用或 task 睡眠/idle 返回 false；两输出始终被覆盖。
 * 注意事项：seqcount 冲突重试且不修改源计费；true 只表示存在开放运行区间，不代表获取了引用。
 */
bool task_cputime(struct task_struct *t, u64 *utime, u64 *stime)
{
	struct vtime *vtime = &t->vtime;
	unsigned int seq;
	u64 delta;
	int ret;

	/* vtime 全局关闭时两输出仍明确初始化，但没有开放区间需要报告。 */
	if (!vtime_accounting_enabled()) {
		*utime = t->utime;
		*stime = t->stime;
		return false;
	}

	/* 每轮先复制已提交基数；重试时不能在上轮输出上继续叠加。 */
	do {
		ret = false;
		seq = read_seqcount_begin(&vtime->seqcount);

		*utime = t->utime;
		*stime = t->stime;

		/* Task is sleeping or idle, nothing to add */
		if (vtime->state < VTIME_SYS)
			continue;

		/* 活动 task 的开放区间按状态只加到 system 或 user 一侧。 */
		ret = true;
		delta = vtime_delta(vtime);

		/*
		 * Task runs either in user (including guest) or kernel space,
		 * add pending nohz time to the right place.
		 */
		if (vtime->state == VTIME_SYS)
			*stime += vtime->stime + delta;
		else
			*utime += vtime->utime + delta;
		/* 状态或残量并发变化时，连同 ret 和两个输出一起重新计算。 */
	} while (read_seqcount_retry(&vtime->seqcount, seq));

	return ret;
}

/*
 * 验证 @vtime 是否仍属于 @cpu 且已越过切入的 INACTIVE 窗口；成功返回
 * VTIME_* 状态，竞态返回 -EAGAIN 让上层重新读取 rq->curr。
 */
/*
 * 业务背景：远端 cpustat 补算必须确认所读 vtime 仍属于目标 CPU 且已跨过 context-switch 中间态。
 * 入参：vtime 是不可空、seqcount 读段内的只读借用状态；cpu 是期望归属的有效 CPU 编号。
 * 出参/返回：安全时返回非负 VTIME_* 状态；迁移或 INACTIVE 窗口返回 -EAGAIN，无输出参数。
 * 注意事项：单次 READ_ONCE 不锁定任务；调用者必须在 -EAGAIN 后重新获取 rq->curr 而非原地沿用指针。
 */
static int vtime_state_fetch(struct vtime *vtime, int cpu)
{
	int state = READ_ONCE(vtime->state);

	/*
	 * We raced against a context switch, fetch the
	 * kcpustat task again.
	 */
	if (vtime->cpu != cpu && vtime->cpu != -1)
		return -EAGAIN;

	/*
	 * Two possible things here:
	 * 1) We are seeing the scheduling out task (prev) or any past one.
	 * 2) We are seeing the scheduling in task (next) but it hasn't
	 *    passed though vtime_task_switch() yet so the pending
	 *    cputime of the prev task may not be flushed yet.
	 *
	 * Case 1) is ok but 2) is not. So wait for a safe VTIME state.
	 */
	if (state == VTIME_INACTIVE)
		return -EAGAIN;

	return state;
}

/* 按 USER/GUEST 状态返回尚未提交的用户侧纳秒，其他状态返回 0。 */
/*
 * 业务背景：USER/NICE 字段补算共享同一逻辑，需要按当前状态选择 user 或 guest 残量。
 * 入参：vtime 是不可空、由 seqcount 读段稳定的只读借用状态。
 * 出参/返回：USER 返回 utime+delta，GUEST 返回 gtime+delta，其他状态返回 0；无输出参数。
 * 注意事项：不验证 CPU 归属且不提交状态，必须在 vtime_state_fetch 成功后的同一读事务使用。
 */
static u64 kcpustat_user_vtime(struct vtime *vtime)
{
	if (vtime->state == VTIME_USER)
		return vtime->utime + vtime_delta(vtime);
	else if (vtime->state == VTIME_GUEST)
		return vtime->gtime + vtime_delta(vtime);
	return 0;
}

/*
 * 基于稳定的 @tsk vtime 快照补算单个 @usage 字段到输出 @val；若 task
 * 已迁移或尚未完成切换则返回 -EAGAIN，成功返回 0。
 */
/*
 * 业务背景：读取单个 cpustat 字段时，要把目标 CPU 当前 task 尚未落账的 vtime 加到已提交基数。
 * 入参：cpustat 是不可空只读数组；tsk 是不可空 RCU 借用 task；usage 是目标类别；cpu 是目标 CPU；val 是不可空输出。
 * 出参/返回：成功返回 0 并覆盖 *val；切换/迁移竞态返回 -EAGAIN，val 内容不保证可用；ownership 不变。
 * 注意事项：调用者持 RCU 并在 seqcount 失败时重试；nice/nr_iowait 可并发变化，因此类别补算是近似快照。
 */
static int kcpustat_field_vtime(u64 *cpustat,
				struct task_struct *tsk,
				enum cpu_usage_stat usage,
				int cpu, u64 *val)
{
	struct vtime *vtime = &tsk->vtime;
	struct rq *rq = cpu_rq(cpu);
	unsigned int seq;

	/* 每次重试都重新验证 CPU 归属，并从已提交字段重新初始化输出。 */
	do {
		int state;

		seq = read_seqcount_begin(&vtime->seqcount);

		state = vtime_state_fetch(vtime, cpu);
		if (state < 0)
			return state;

		*val = cpustat[usage];

		/* nice 改变不触发 nohz_full CPU，只能按读取时快照选择 USER/NICE 子类。 */
		/*
		 * Nice VS unnice cputime accounting may be inaccurate if
		 * the nice value has changed since the last vtime update.
		 * But proper fix would involve interrupting target on nice
		 * updates which is a no go on nohz_full (although the scheduler
		 * may still interrupt the target if rescheduling is needed...)
		 */
		switch (usage) {
		case CPUTIME_SYSTEM:
			/* SYSTEM 只补 VTIME_SYS 的 system 残量与本轮开放 delta。 */
			if (state == VTIME_SYS)
				*val += vtime->stime + vtime_delta(vtime);
			break;
		/* USER/NICE 共用用户侧补算，再按 task 当前 nice 快照二选一。 */
		case CPUTIME_USER:
			if (task_nice(tsk) <= 0)
				*val += kcpustat_user_vtime(vtime);
			break;
		case CPUTIME_NICE:
			if (task_nice(tsk) > 0)
				*val += kcpustat_user_vtime(vtime);
			break;
		/* GUEST/GUEST_NICE 只在客体状态补同一 gtime 区间，并按 nice 拆分。 */
		case CPUTIME_GUEST:
			if (state == VTIME_GUEST && task_nice(tsk) <= 0)
				*val += vtime->gtime + vtime_delta(vtime);
			break;
		case CPUTIME_GUEST_NICE:
			if (state == VTIME_GUEST && task_nice(tsk) > 0)
				*val += vtime->gtime + vtime_delta(vtime);
			break;
		/* idle 与 iowait 共享 VTIME_IDLE delta，并按 rq 当前 waiter 快照二选一。 */
		case CPUTIME_IDLE:
			if (state == VTIME_IDLE && !atomic_read(&rq->nr_iowait))
				*val += vtime_delta(vtime);
			break;
		/* IOWAIT 与 IDLE 条件互斥，非空 waiter 快照才接收开放 idle delta。 */
		case CPUTIME_IOWAIT:
			if (state == VTIME_IDLE && atomic_read(&rq->nr_iowait) > 0)
				*val += vtime_delta(vtime);
			break;
		default:
			break;
		}
		/* seqcount 失败时 *val 是混合代快照，必须整体丢弃并重新分类。 */
	} while (read_seqcount_retry(&vtime->seqcount, seq));

	return 0;
}

/*
 * 返回 @cpu 的指定 cpustat 字段；generic vtime 下以 RCU 稳定 rq->curr，
 * 遇到切换窗口循环重试，把当前未落账区间加入已提交基数。
 */
/*
 * 业务背景：公共字段读取在 generic-vtime CPU 上必须无 IPI 地补入当前 task 的开放执行区间。
 * 入参：usage 是有效 cpu_usage_stat 类别；cpu 是有效且 rq/cpustat 可访问的 CPU 编号。
 * 出参/返回：返回该字段累计纳秒快照；无输出参数或 ownership 变化，异常 curr 时退回已提交值。
 * 注意事项：RCU 稳定 curr，迁移窗口可无限短暂重试；错误 usage/cpu 会越界，结果不冻结后续更新。
 */
u64 kcpustat_field(enum cpu_usage_stat usage, int cpu)
{
	u64 *cpustat = kcpustat_cpu(cpu).cpustat;
	u64 val = cpustat[usage];
	struct rq *rq;
	int err;

	if (!vtime_generic_enabled_cpu(cpu))
		return kcpustat_field_default(usage, cpu);

	/* generic 路径需要同时稳定 rq->curr 指针和该 task 的 vtime 状态。 */
	rq = cpu_rq(cpu);

	for (;;) {
		struct task_struct *curr;

		rcu_read_lock();
		curr = rcu_dereference(rq->curr);
		/* NULL curr 是内部异常，保守返回不含开放区间的已提交基数。 */
		if (WARN_ON_ONCE(!curr)) {
			rcu_read_unlock();
			return cpustat[usage];
		}

		err = kcpustat_field_vtime(cpustat, curr, usage, cpu, &val);
		rcu_read_unlock();

		if (!err)
			return val;

		/* -EAGAIN 表示恰逢切换，释放 RCU 后短暂让步再重新抓取 curr。 */
		cpu_relax();
	}
}
EXPORT_SYMBOL_GPL(kcpustat_field);

/* 将整个 @src 快照复制到 @dst 并补算 @tsk 当前区间；竞态返回 -EAGAIN。 */
/*
 * 业务背景：批量读取全部 cpustat 字段时，需先复制已提交数组，再把当前 task 的开放 vtime 归入一个类别。
 * 入参：dst 是不可空调用者输出；src 是不可空只读源；tsk 是不可空 RCU 借用 task；cpu 是目标 CPU。
 * 出参/返回：成功返回 0 并完整覆盖 *dst；迁移/切换竞态返回 -EAGAIN，dst 可能部分更新且须丢弃。
 * 注意事项：调用者持 RCU并重试 -EAGAIN；seqcount 保护 vtime，不同步 nice/nr_iowait 的近似分类。
 */
static int kcpustat_cpu_fetch_vtime(struct kernel_cpustat *dst,
				    const struct kernel_cpustat *src,
				    struct task_struct *tsk, int cpu)
{
	struct vtime *vtime = &tsk->vtime;
	unsigned int seq;

	/* 每轮先验证 task 仍代表该 CPU，再复制完整已提交基数到调用者缓冲区。 */
	do {
		u64 *cpustat;
		u64 delta;
		int state;

		seq = read_seqcount_begin(&vtime->seqcount);

		state = vtime_state_fetch(vtime, cpu);
		if (state < 0)
			return state;

		*dst = *src;
		cpustat = dst->cpustat;

		/* 睡眠/死亡 task 没有开放区间，复制后的基数已经是完整结果。 */
		/* Task is sleeping or dead, nothing to add */
		if (state < VTIME_IDLE)
			continue;

		delta = vtime_delta(vtime);

		/* 活动状态只允许把 delta 与对应残量加入一个基础类别。 */
		/*
		 * Task runs either in user (including guest) or kernel space,
		 * add pending nohz time to the right place.
		 */
		switch (state) {
		case VTIME_SYS:
			cpustat[CPUTIME_SYSTEM] += vtime->stime + delta;
			break;
		case VTIME_USER:
			/* nice 快照决定 USER 或 NICE，二者不会同时增加。 */
			if (task_nice(tsk) > 0)
				cpustat[CPUTIME_NICE] += vtime->utime + delta;
			else
				cpustat[CPUTIME_USER] += vtime->utime + delta;
			break;
		case VTIME_GUEST:
			/* guest 同时是 user/nice 的子集，因此成对更新基础字段和 guest 字段。 */
			if (task_nice(tsk) > 0) {
				cpustat[CPUTIME_GUEST_NICE] += vtime->gtime + delta;
				cpustat[CPUTIME_NICE] += vtime->gtime + delta;
			} else {
				cpustat[CPUTIME_GUEST] += vtime->gtime + delta;
				cpustat[CPUTIME_USER] += vtime->gtime + delta;
			}
			break;
		/* idle 时间仍按读取时 waiter 快照在 IOWAIT/IDLE 中二选一。 */
		case VTIME_IDLE:
			if (atomic_read(&cpu_rq(cpu)->nr_iowait) > 0)
				cpustat[CPUTIME_IOWAIT] += delta;
			else
				cpustat[CPUTIME_IDLE] += delta;
			break;
		default:
			WARN_ON_ONCE(1);
		}
		/* 写者并发改变任一状态时，丢弃整个 dst 并从 src 重新复制。 */
	} while (read_seqcount_retry(&vtime->seqcount, seq));

	return 0;
}

/*
 * 获取 @cpu 的完整 cpustat 快照到调用者拥有的 @dst；generic vtime 下
 * RCU 稳定当前 task 并在迁移窗口重试，异常时退回已提交数据。
 */
/*
 * 业务背景：公共批量接口要为监控者生成指定 CPU 的完整 cpustat，并在 generic-vtime 下补开放区间。
 * 入参：dst 是不可空、由调用者拥有的输出结构；cpu 是有效且 rq/cpustat 可访问的 CPU 编号。
 * 出参/返回：无直接返回值；完整覆盖 *dst，generic-vtime 异常时复制默认已提交快照；ownership 不变。
 * 注意事项：RCU 与 seqcount 竞态可重试；不会冻结 CPU 或强制提交，返回内容是近似瞬时快照。
 */
void kcpustat_cpu_fetch(struct kernel_cpustat *dst, int cpu)
{
	const struct kernel_cpustat *src = &kcpustat_cpu(cpu);
	struct rq *rq;
	int err;

	if (!vtime_generic_enabled_cpu(cpu)) {
		kcpustat_cpu_fetch_default(dst, cpu);
		return;
	}

	/* generic 路径从 rq 找当前 task，RCU 只稳定对象，不冻结 context switch。 */
	rq = cpu_rq(cpu);

	for (;;) {
		struct task_struct *curr;

		rcu_read_lock();
		curr = rcu_dereference(rq->curr);
		/* rq->curr 异常为空时发出一次告警，并退回无需 task 的默认快照。 */
		if (WARN_ON_ONCE(!curr)) {
			rcu_read_unlock();
			kcpustat_cpu_fetch_default(dst, cpu);
			return;
		}

		err = kcpustat_cpu_fetch_vtime(dst, src, curr, cpu);
		rcu_read_unlock();

		if (!err)
			return;

		/* 切换窗口返回 -EAGAIN，释放 RCU 后重新读取 curr，不能复用旧 task。 */
		cpu_relax();
	}
}
EXPORT_SYMBOL_GPL(kcpustat_cpu_fetch);

#endif /* CONFIG_VIRT_CPU_ACCOUNTING_GEN */
