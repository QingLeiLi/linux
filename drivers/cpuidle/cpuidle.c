/*
 * cpuidle.c - core cpuidle infrastructure
 *
 * (C) 2006-2007 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *               Shaohua Li <shaohua.li@intel.com>
 *               Adam Belay <abelay@novell.com>
 *
 * This code is licenced under the GPL.
 */

#include "linux/percpu-defs.h"
#include <linux/clockchips.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/idle.h>
#include <linux/notifier.h>
#include <linux/pm_qos.h>
#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/tick.h>
#include <linux/mmu_context.h>
#include <linux/context_tracking.h>
#include <trace/events/power.h>

#include "cpuidle.h"

/*
 * cpuidle 核心层总览：
 *
 * 驱动提供每个空闲状态的进入回调与时延/驻留时间属性，governor 根据预测选择状态，
 * 本文件则把每 CPU 设备、驱动、governor、tick/RCU/context tracking 与统计更新串成
 * 一条受控执行链。普通运行时的快路径不取 cpuidle_lock；注册、启停和解除注册路径
 * 先撤下全局 idle handler，再用互斥锁修改发布给各 CPU 的对象，最后重新安装。
 */

/* 每 CPU 发布指针供本地 idle loop 快速取得设备；实体存储供通用注册助手使用。 */
DEFINE_PER_CPU(struct cpuidle_device *, cpuidle_devices);
DEFINE_PER_CPU(struct cpuidle_device, cpuidle_dev);

/* cpuidle_lock 串行化控制面变更；detected_devices 记录已注册设备但不拥有其内存。 */
DEFINE_MUTEX(cpuidle_lock);
LIST_HEAD(cpuidle_detected_devices);

/* enabled_devices 决定是否安装处理器；off 是只读启动开关；initialized 发布可用状态。 */
static int enabled_devices;
static int off __read_mostly;
static int initialized __read_mostly;

/* 返回启动参数或架构路径是否永久关闭 cpuidle；该只读快路径不取得控制面锁。 */
int cpuidle_disabled(void)
{
	return off;
}

/* 将框架永久标记为关闭；应在框架开始向 idle loop 发布对象之前调用。 */
void disable_cpuidle(void)
{
	off = 1;
}

/*
 * 汇总快路径的可用条件：全局未关闭、handler 已发布、驱动和设备存在且设备已启用。
 * 调用方据此退回架构默认 idle；返回 false 也只保证当前观察点可用。
 */
bool cpuidle_not_available(struct cpuidle_driver *drv,
			   struct cpuidle_device *dev)
{
	return off || !initialized || !drv || !dev || !dev->enabled;
}

/**
 * cpuidle_play_dead - cpu off-lining
 *
 * Returns in case of an error or no driver
 *
 * CPU 下线时从最深状态向最浅状态尝试 enter_dead()。成功回调不会返回；若函数返回，
 * 就表示没有驱动、没有可用回调或所有回调均失败，统一以 -ENODEV 告知热拔路径。
 */
int cpuidle_play_dead(void)
{
	struct cpuidle_device *dev = __this_cpu_read(cpuidle_devices);
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	int i;

	if (!drv)
		return -ENODEV;

	/* 深状态通常最接近断电目标，因此按索引逆序尝试，不在此解释驱动私有失败原因。 */
	for (i = drv->state_count - 1; i >= 0; i--) {
		if (drv->states[i].enter_dead)
			drv->states[i].enter_dead(dev, i);
	}

	/*
	 * If :enter_dead() is successful, it will never return, so reaching
	 * here means that all of them failed above or were not present.
	 * enter_dead() 成功后 CPU 不会回到此处；能执行到这里即代表上述候选均未成功。
	 */
	return -ENODEV;
}

/*
 * 在索引 1..state_count-1 中选择满足约束且退出时延最大的状态。索引 0 是轮询/兜底
 * 状态，ret 初始为 0，确保没有更深候选时仍可返回安全基线。禁用状态、超过时延预算、
 * 含禁止标志或缺少 s2idle 回调的状态都会被过滤；相同/更小时延不替换当前候选。
 * 调用期间 drv/dev 的注册生命周期必须稳定，通常由本 CPU idle 上下文或控制面暂停保证。
 */
static int find_deepest_state(struct cpuidle_driver *drv,
			      struct cpuidle_device *dev,
			      u64 max_latency_ns,
			      unsigned int forbidden_flags,
			      bool s2idle)
{
	u64 latency_req = 0;
	int i, ret = 0;

	/* 以退出时延作为“更深”的可比较代理，并保留最后一个严格更大的合法候选。 */
	for (i = 1; i < drv->state_count; i++) {
		struct cpuidle_state *s = &drv->states[i];

		if (dev->states_usage[i].disable ||
		    s->exit_latency_ns <= latency_req ||
		    s->exit_latency_ns > max_latency_ns ||
		    (s->flags & forbidden_flags) ||
		    (s2idle && !s->enter_s2idle))
			continue;

		latency_req = s->exit_latency_ns;
		ret = i;
	}
	return ret;
}

/**
 * cpuidle_use_deepest_state - Set/unset governor override mode.
 * @latency_limit_ns: Idle state exit latency limit (or no override if 0).
 *
 * If @latency_limit_ns is nonzero, set the current CPU to use the deepest idle
 * state with exit latency within @latency_limit_ns (override governors going
 * forward), or do not override governors if it is zero.
 *
 * 为当前 CPU 设置 governor 覆盖预算。非零值要求后续 idle 直接选择预算内最深状态，
 * 0 恢复 governor 自主选择。禁抢占区保证读取的 per-CPU 设备指针与写入发生在同一 CPU；
 * 本函数不接管设备生命周期，设备不存在时静默忽略。
 */
void cpuidle_use_deepest_state(u64 latency_limit_ns)
{
	struct cpuidle_device *dev;

	preempt_disable();
	/* cpuidle_get_device() 是每 CPU 访问，必须防止任务在读写之间迁移。 */
	dev = cpuidle_get_device();
	if (dev)
		dev->forced_idle_latency_limit_ns = latency_limit_ns;
	preempt_enable();
}

/**
 * cpuidle_find_deepest_state - Find the deepest available idle state.
 * @drv: cpuidle driver for the given CPU.
 * @dev: cpuidle device for the given CPU.
 * @latency_limit_ns: Idle state exit latency limit
 *
 * Return: the index of the deepest available idle state.
 *
 * 公共查询不附加禁止标志，也不要求 enter_s2idle；返回值始终是状态表中的索引，
 * 没有合适的深状态时返回 0。调用方负责保证 @latency_limit_ns 的单位为纳秒。
 */
int cpuidle_find_deepest_state(struct cpuidle_driver *drv,
			       struct cpuidle_device *dev,
			       u64 latency_limit_ns)
{
	return find_deepest_state(drv, dev, latency_limit_ns, 0, false);
}

#ifdef CONFIG_SUSPEND
/*
 * 真正执行 suspend-to-idle 的 noinstr 区域。调用者已选择支持 enter_s2idle 的状态并
 * 关闭本地 IRQ；本函数冻结 tick、切换 context tracking、调用驱动并累计独立的
 * s2idle 统计。驱动必须带着 IRQ 关闭返回，违规时强制恢复以保护后续低级路径。
 */
static noinstr void enter_s2idle_proper(struct cpuidle_driver *drv,
					 struct cpuidle_device *dev, int index)
{
	struct cpuidle_state *target_state = &drv->states[index];
	ktime_t time_start, time_end;

	instrumentation_begin();

	/* local_clock_noinstr() 可在 noinstr 路径安全取时间；统计口径包住完整冻结区间。 */
	time_start = ns_to_ktime(local_clock_noinstr());

	tick_freeze();
	/*
	 * The state used here cannot be a "coupled" one, because the "coupled"
	 * cpuidle mechanism enables interrupts and doing that with timekeeping
	 * suspended is generally unsafe.
	 * 此处状态不能是 coupled 状态：coupled 协调机制会打开中断，而时间保持已暂停时
	 * 打开中断通常不安全；候选及驱动必须遵守这一跨层约束。
	 */
	stop_critical_timings();
	if (!(target_state->flags & CPUIDLE_FLAG_RCU_IDLE)) {
		/* 核心代替未自行处理 RCU idle 的驱动进入扩展静止状态。 */
		ct_cpuidle_enter();
		/* Annotate away the indirect call */
		/* 显式开放下面的间接回调，避免 noinstr 校验把合法驱动调用视为违规插桩。 */
		instrumentation_begin();
	}
	/* 回调负责硬件进入/退出，但不拥有 dev、drv 或状态表。 */
	target_state->enter_s2idle(dev, drv, index);
	if (WARN_ON_ONCE(!irqs_disabled()))
		raw_local_irq_disable();
	if (!(target_state->flags & CPUIDLE_FLAG_RCU_IDLE)) {
		/* 与进入顺序逆序收束插桩窗口和 context tracking 状态。 */
		instrumentation_end();
		ct_cpuidle_exit();
	}
	tick_unfreeze();
	start_critical_timings();

	time_end = ns_to_ktime(local_clock_noinstr());

	/* s2idle 使用独立计数，避免与普通 idle 驻留统计混淆。 */
	dev->states_usage[index].s2idle_time += ktime_us_delta(time_end, time_start);
	dev->states_usage[index].s2idle_usage++;
	instrumentation_end();
}

/**
 * cpuidle_enter_s2idle - Enter an idle state suitable for suspend-to-idle.
 * @drv: cpuidle driver for the given CPU.
 * @dev: cpuidle device for the given CPU.
 * @latency_limit_ns: Idle state exit latency limit
 *
 * If there are states with the ->enter_s2idle callback, find the deepest of
 * them and enter it with frozen tick.
 *
 * 在退出时延预算内查找最深且实现 ->enter_s2idle 的状态。只有索引大于 0 才进入，
 * 因为 0 是普通兜底状态；执行函数按契约带 IRQ 关闭返回，本包装层在离开前重新开 IRQ。
 * 返回索引让上层知道实际是否进入了 s2idle 专用状态。
 */
int cpuidle_enter_s2idle(struct cpuidle_driver *drv, struct cpuidle_device *dev,
			 u64 latency_limit_ns)
{
	int index;

	/*
	 * Find the deepest state with ->enter_s2idle present that meets the
	 * specified latency limit, which guarantees that interrupts won't be
	 * enabled when it exits and allows the tick to be frozen safely.
	 * 仅选择实现 enter_s2idle 且满足时延上限的最深状态；该回调契约保证退出时仍关闭
	 * 中断，因而冻结 tick 的临界窗口不会被普通中断路径打破。
	 */
	index = find_deepest_state(drv, dev, latency_limit_ns, 0, true);
	if (index > 0) {
		enter_s2idle_proper(drv, dev, index);
		local_irq_enable();
	}
	return index;
}
#endif /* CONFIG_SUSPEND */

/**
 * cpuidle_enter_state - enter the state and update stats
 * @dev: cpuidle device for this cpu
 * @drv: cpuidle driver for this cpu
 * @index: index into the states table in @drv of the state to enter
 *
 * 普通 idle 状态进入的核心事务：必要时切换广播 tick，发布调度器可见的计划状态，
 * 建立 RCU/context-tracking 边界，调用驱动，恢复 tick/IRQ，再按实际进入状态更新统计。
 * @index 是 governor 的选择，但广播定时器不可用时可降级；驱动返回的 entered_state
 * 才是实际状态。调用者必须在本 CPU、IRQ 已关闭且 drv/dev 生命周期稳定时进入。
 */
noinstr int cpuidle_enter_state(struct cpuidle_device *dev,
				 struct cpuidle_driver *drv,
				 int index)
{
	int entered_state;

	struct cpuidle_state *target_state = &drv->states[index];
	bool broadcast = !!(target_state->flags & CPUIDLE_FLAG_TIMER_STOP);
	ktime_t time_start, time_end;

	instrumentation_begin();

	/*
	 * Tell the time framework to switch to a broadcast timer because our
	 * local timer will be shut down.  If a local timer is used from another
	 * CPU as a broadcast timer, this call may fail if it is not available.
	 * 若目标状态停止本地定时器，先请求 tick 框架切到广播设备。如果广播设备正作为
	 * 其他 CPU 的本地设备而不可用，请求会失败，此时降级到不停止定时器的最深状态。
	 */
	if (broadcast && tick_broadcast_enter()) {
		index = find_deepest_state(drv, dev, target_state->exit_latency_ns,
					   CPUIDLE_FLAG_TIMER_STOP, false);

		target_state = &drv->states[index];
		/* 降级候选已显式排除 TIMER_STOP，因此退出时无需调用 broadcast_exit。 */
		broadcast = false;
	}

	if (target_state->flags & CPUIDLE_FLAG_TLB_FLUSHED)
		/* 目标状态会丢失 TLB，上层先离开当前 mm，避免返回后沿用失效地址空间。 */
		leave_mm();

	/* Take note of the planned idle state. */
	/* 向调度器发布计划状态；指针只在本次进入窗口有效，退出前必须清空。 */
	sched_idle_set_state(target_state);

	/* trace 与时间戳围住硬件驻留窗口，作为统计和可观测性的共同边界。 */
	trace_cpu_idle(index, dev->cpu);
	time_start = ns_to_ktime(local_clock_noinstr());

	stop_critical_timings();
	if (!(target_state->flags & CPUIDLE_FLAG_RCU_IDLE)) {
		/* 未声明自行管理 RCU idle 的驱动，由核心层完成 context tracking 切换。 */
		ct_cpuidle_enter();
		/* Annotate away the indirect call */
		/* 允许这个已知的驱动间接调用位于受控的 noinstr 区域内。 */
		instrumentation_begin();
	}

	/*
	 * NOTE!!
	 *
	 * For cpuidle_state::enter() methods that do *NOT* set
	 * CPUIDLE_FLAG_RCU_IDLE RCU will be disabled here and these functions
	 * must be marked either noinstr or __cpuidle.
	 *
	 * For cpuidle_state::enter() methods that *DO* set
	 * CPUIDLE_FLAG_RCU_IDLE this isn't required, but they must mark the
	 * function calling ct_cpuidle_enter() as noinstr/__cpuidle and all
	 * functions called within the RCU-idle region.
	 * 注意：未设置 CPUIDLE_FLAG_RCU_IDLE 的 enter() 在这里由核心关闭 RCU watching，
	 * 回调必须标记 noinstr 或 __cpuidle。设置该标志则代表驱动自行调用 ct_cpuidle_enter()，
	 * 它及 RCU-idle 区域内所有下游函数同样必须满足 noinstr/__cpuidle 约束。
	 */
	/* 返回值可能与请求 index 不同，后续统计必须使用 entered_state。 */
	entered_state = target_state->enter(dev, drv, index);

	if (WARN_ONCE(!irqs_disabled(), "%ps leaked IRQ state", target_state->enter))
		/* 驱动泄漏 IRQ 开启状态会破坏退出序列；告警后强制关回。 */
		raw_local_irq_disable();

	if (!(target_state->flags & CPUIDLE_FLAG_RCU_IDLE)) {
		instrumentation_end();
		ct_cpuidle_exit();
	}
	start_critical_timings();

	sched_clock_idle_wakeup_event();
	/* 驻留终点在 context tracking 恢复之后记录，与进入侧形成统一统计口径。 */
	time_end = ns_to_ktime(local_clock_noinstr());
	trace_cpu_idle(PWR_EVENT_EXIT, dev->cpu);

	/* The cpu is no longer idle or about to enter idle. */
	/* CPU 已不再处于或准备进入 idle，撤销调度器看到的临时状态指针。 */
	sched_idle_set_state(NULL);

	if (broadcast)
		/* 只有成功进入广播模式的原始目标才需要在这里恢复本地 tick。 */
		tick_broadcast_exit();

	if (!cpuidle_state_is_coupled(drv, index))
		/* coupled 框架自行管理 IRQ；非 coupled 路径由核心在所有低级恢复完成后开启。 */
		local_irq_enable();

	if (entered_state >= 0) {
		s64 diff, delay = drv->states[entered_state].exit_latency_ns;
		int i;

		/*
		 * Update cpuidle counters
		 * This can be moved to within driver enter routine,
		 * but that results in multiple copies of same code.
		 * 统一更新 cpuidle 计数。虽可下沉到各驱动 enter 回调，但会复制相同逻辑并使
		 * 统计口径分裂，所以由核心按实际进入状态集中处理。
		 */
		diff = ktime_sub(time_end, time_start);

		dev->last_residency_ns = diff;
		dev->states_usage[entered_state].time_ns += diff;
		dev->states_usage[entered_state].usage++;

		/* 驻留短于目标值：若存在可用浅状态，本次选择被统计为“过深”。 */
		if (diff < drv->states[entered_state].target_residency_ns) {
			for (i = entered_state - 1; i >= 0; i--) {
				if (dev->states_usage[i].disable)
					continue;

				/* Shallower states are enabled, so update. */
				/* 找到启用的浅状态即可证明可选，因此只累计一次 above 并结束扫描。 */
				dev->states_usage[entered_state].above++;
				trace_cpu_idle_miss(dev->cpu, entered_state, false);
				break;
			}
		} else if (diff > delay) {
			/* 扣除当前状态退出时延后，判断是否足以覆盖下一可用深状态的目标驻留时间。 */
			for (i = entered_state + 1; i < drv->state_count; i++) {
				if (dev->states_usage[i].disable)
					continue;

				/*
				 * Update if a deeper state would have been a
				 * better match for the observed idle duration.
				 * 若观测驻留时间在扣除当前退出时延后仍达到更深状态门槛，则更深状态
				 * 可能更匹配，本次选择累计为 below；只比较第一个启用的更深候选。
				 */
				if (diff - delay >= drv->states[i].target_residency_ns) {
					dev->states_usage[entered_state].below++;
					trace_cpu_idle_miss(dev->cpu, entered_state, true);
				}

				break;
			}
		}
	} else {
		/* 负返回表示驱动拒绝进入；没有有效驻留时间，并记到最终请求 index。 */
		dev->last_residency_ns = 0;
		dev->states_usage[index].rejected++;
	}

	instrumentation_end();

	return entered_state;
}

/**
 * cpuidle_select - ask the cpuidle framework to choose an idle state
 *
 * @drv: the cpuidle driver
 * @dev: the cpuidle device
 * @stop_tick: indication on whether or not to stop the tick
 *
 * Returns the index of the idle state.  The return value must not be negative.
 *
 * The memory location pointed to by @stop_tick is expected to be written the
 * 'false' boolean value if the scheduler tick should not be stopped before
 * entering the returned state.
 *
 * 把状态选择委托给当前 governor。governor 必须返回非负索引，并在所选状态要求保留
 * 调度 tick 时把 *@stop_tick 写为 false；核心只转发，不校验索引，因此注册阶段必须
 * 保证 governor 与驱动状态表匹配且其生命周期覆盖 idle 快路径。
 */
int cpuidle_select(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		   bool *stop_tick)
{
	return cpuidle_curr_governor->select(drv, dev, stop_tick);
}

/**
 * cpuidle_enter - enter into the specified idle state
 *
 * @drv:   the cpuidle driver tied with the cpu
 * @dev:   the cpuidle device
 * @index: the index in the idle state table
 *
 * Returns the index in the idle state, < 0 in case of error.
 * The error code depends on the backend driver
 *
 * 记录下一高精度定时器期限后，根据状态是否 coupled 分派到协调入口或普通入口。
 * next_hrtimer 通过 WRITE_ONCE 发布给 cpuidle 外部观察者，退出时无条件清零；返回实际
 * 状态索引或驱动错误码，错误语义由后端定义。
 */
int cpuidle_enter(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		  int index)
{
	int ret = 0;

	/*
	 * Store the next hrtimer, which becomes either next tick or the next
	 * timer event, whatever expires first. Additionally, to make this data
	 * useful for consumers outside cpuidle, we rely on that the governor's
	 * ->select() callback have decided, whether to stop the tick or not.
	 * 保存下一 hrtimer（下一 tick 与下一定时事件中较早者）。该值要供 cpuidle 外部
	 * 消费者使用，因此依赖 governor 的 select 已经决定是否停止 tick。
	 */
	WRITE_ONCE(dev->next_hrtimer, tick_nohz_get_next_hrtimer());

	if (cpuidle_state_is_coupled(drv, index))
		/* coupled 状态要求一组 CPU 协同进入，交给专用状态机管理屏障与 IRQ。 */
		ret = cpuidle_enter_state_coupled(dev, drv, index);
	else
		ret = cpuidle_enter_state(dev, drv, index);

	WRITE_ONCE(dev->next_hrtimer, 0);
	/* 清零表示本次 idle 事务结束，避免观察者把旧期限误认为下一次预测。 */
	return ret;
}

/**
 * cpuidle_reflect - tell the underlying governor what was the state
 * we were in
 *
 * @dev  : the cpuidle device
 * @index: the index in the idle state table
 *
 * 将实际进入结果反馈给 governor，以便其更新预测历史。负索引代表进入失败，不反射；
 * reflect 是可选回调，调用期间当前 governor 与设备必须仍保持启用。
 */
void cpuidle_reflect(struct cpuidle_device *dev, int index)
{
	if (cpuidle_curr_governor->reflect && index >= 0)
		cpuidle_curr_governor->reflect(dev, index);
}

/*
 * Min polling interval of 10usec is a guess. It is assuming that
 * for most users, the time for a single ping-pong workload like
 * perf bench pipe would generally complete within 10usec but
 * this is hardware dependent. Actual time can be estimated with
 *
 * perf bench sched pipe -l 10000
 *
 * Run multiple times to avoid cpufreq effects.
 *
 * 最小轮询区间 10 微秒是经验值：假定常见 ping-pong 工作负载可在此时间内完成，
 * 但结果依赖硬件。可用 `perf bench sched pipe -l 10000` 多次测量，以减弱 cpufreq
 * 波动影响。最大值取一个 tick 的 1/16，避免纯轮询长期占用 CPU。
 */
#define CPUIDLE_POLL_MIN 10000
#define CPUIDLE_POLL_MAX (TICK_NSEC / 16)

/**
 * cpuidle_poll_time - return amount of time to poll for,
 * governors can override dev->poll_limit_ns if necessary
 *
 * @drv:   the cpuidle driver tied with the cpu
 * @dev:   the cpuidle device
 *
 * 计算并缓存轮询状态的时间上限。governor 可预先写 poll_limit_ns 覆盖自动值；否则
 * 从第一个启用且目标驻留不小于最小轮询值的非零状态取得门槛，并限制到最大值。
 * 状态启停变化时控制面需要使缓存失效，函数本身只在本 CPU idle 上下文更新设备字段。
 */
__cpuidle u64 cpuidle_poll_time(struct cpuidle_driver *drv,
		      struct cpuidle_device *dev)
{
	int i;
	u64 limit_ns;

	BUILD_BUG_ON(CPUIDLE_POLL_MIN > CPUIDLE_POLL_MAX);

	if (dev->poll_limit_ns)
		/* 非零既可能是 governor 覆盖，也可能是本函数先前计算的缓存。 */
		return dev->poll_limit_ns;

	limit_ns = CPUIDLE_POLL_MAX;
	for (i = 1; i < drv->state_count; i++) {
		u64 state_limit;

		if (dev->states_usage[i].disable)
			continue;

		state_limit = drv->states[i].target_residency_ns;
		if (state_limit < CPUIDLE_POLL_MIN)
			continue;

		limit_ns = min_t(u64, state_limit, CPUIDLE_POLL_MAX);
		/* 状态表按深度排列，首个合格状态就是轮询与睡眠之间的切换边界。 */
		break;
	}

	dev->poll_limit_ns = limit_ns;

	return dev->poll_limit_ns;
}

/**
 * cpuidle_install_idle_handler - installs the cpuidle idle loop handler
 *
 * 至少一个设备启用时，用写屏障确保设备、驱动和 governor 的初始化先于 initialized
 * 可见。idle loop 观察到 initialized=1 后即可无锁读取这些对象。
 */
void cpuidle_install_idle_handler(void)
{
	if (enabled_devices) {
		/* Make sure all changes finished before we switch to new idle */
		/* 在切换到新 idle 路径前，保证所有控制面修改已完成并对其他 CPU 可见。 */
		smp_wmb();
		initialized = 1;
	}
}

/**
 * cpuidle_uninstall_idle_handler - uninstalls the cpuidle idle loop handler
 *
 * 先撤销 initialized 并唤醒所有 idle CPU，使其离开可能仍引用状态表的快路径；随后
 * synchronize_rcu() 等待调度器等外部 RCU 观察者完成。调用方通常持 cpuidle_lock，
 * 从而在整个静默窗口内独占控制面修改权。
 */
void cpuidle_uninstall_idle_handler(void)
{
	if (enabled_devices) {
		initialized = 0;
		wake_up_all_idle_cpus();
	}

	/*
	 * Make sure external observers (such as the scheduler)
	 * are done looking at pointed idle states.
	 * 等待调度器等外部观察者停止查看已发布的 idle state 指针，之后才可释放或改写对象。
	 */
	synchronize_rcu();
}

/**
 * cpuidle_pause_and_lock - temporarily disables CPUIDLE
 *
 * 获取全局控制面锁并撤下 idle handler；成功返回后调用方持锁，可安全成组修改设备。
 */
void cpuidle_pause_and_lock(void)
{
	mutex_lock(&cpuidle_lock);
	cpuidle_uninstall_idle_handler();
}

EXPORT_SYMBOL_GPL(cpuidle_pause_and_lock);

/**
 * cpuidle_resume_and_unlock - resumes CPUIDLE operation
 *
 * 在仍持锁时重新发布 handler，再释放控制面锁；必须与 pause_and_lock 成对调用。
 */
void cpuidle_resume_and_unlock(void)
{
	cpuidle_install_idle_handler();
	mutex_unlock(&cpuidle_lock);
}

EXPORT_SYMBOL_GPL(cpuidle_resume_and_unlock);

/* Currently used in suspend/resume path to suspend cpuidle */
/* 当前供系统 suspend/resume 使用：在锁内短暂撤下 handler，但不把锁交给调用方。 */
void cpuidle_pause(void)
{
	mutex_lock(&cpuidle_lock);
	cpuidle_uninstall_idle_handler();
	mutex_unlock(&cpuidle_lock);
}

/* Currently used in suspend/resume path to resume cpuidle */
/* suspend/resume 恢复阶段在锁内重新发布 handler，与 cpuidle_pause() 配对。 */
void cpuidle_resume(void)
{
	mutex_lock(&cpuidle_lock);
	cpuidle_install_idle_handler();
	mutex_unlock(&cpuidle_lock);
}

/**
 * cpuidle_enable_device - enables idle PM for a CPU
 * @dev: the CPU
 *
 * This function must be called between cpuidle_pause_and_lock and
 * cpuidle_resume_and_unlock when used externally.
 *
 * 把一个已注册设备接入 sysfs 和当前 governor，最后以写屏障发布 enabled=1 并增加
 * 全局启用计数。外部调用者必须位于 pause_and_lock/resume_and_unlock 之间，避免
 * idle CPU 与半初始化状态并发。重复启用幂等返回 0；任一步失败都会回滚已创建的 sysfs。
 */
int cpuidle_enable_device(struct cpuidle_device *dev)
{
	int ret;
	struct cpuidle_driver *drv;

	if (!dev)
		return -EINVAL;

	/* 已启用设备不重复执行 governor 回调或增加全局计数。 */
	if (dev->enabled)
		return 0;

	if (!cpuidle_curr_governor)
		return -EIO;

	drv = cpuidle_get_cpu_driver(dev);

	if (!drv)
		return -EIO;

	if (!dev->registered)
		return -EINVAL;

	ret = cpuidle_add_device_sysfs(dev);
	if (ret)
		return ret;

	if (cpuidle_curr_governor->enable) {
		/* governor 可为每 CPU 分配私有状态；失败时只需撤销此前建立的设备 sysfs。 */
		ret = cpuidle_curr_governor->enable(drv, dev);
		if (ret)
			goto fail_sysfs;
	}

	smp_wmb();

	/* enabled 是快路径发布点：此前 sysfs/governor 初始化必须先对观察者可见。 */
	dev->enabled = 1;

	enabled_devices++;
	return 0;

fail_sysfs:
	/* governor 未成功启用，所以回滚范围止于本函数创建的设备级 sysfs。 */
	cpuidle_remove_device_sysfs(dev);

	return ret;
}

EXPORT_SYMBOL_GPL(cpuidle_enable_device);

/**
 * cpuidle_disable_device - disables idle PM for a CPU
 * @dev: the CPU
 *
 * This function must be called between cpuidle_pause_and_lock and
 * cpuidle_resume_and_unlock when used externally.
 *
 * 撤销 enable 的逆序操作：先清 enabled 阻止新快路径使用，再通知 governor 释放每 CPU
 * 状态、移除设备 sysfs 并减少全局计数。设备仍保持 registered，可在之后重新启用。
 */
void cpuidle_disable_device(struct cpuidle_device *dev)
{
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);

	if (!dev || !dev->enabled)
		return;

	if (!drv || !cpuidle_curr_governor)
		/* 注册约束异常时不尝试用空回调拆除，保持现状供控制面诊断。 */
		return;

	/* handler 已由外层暂停；先清发布位，使随后恢复的 idle loop 不再选择此设备。 */
	dev->enabled = 0;

	if (cpuidle_curr_governor->disable)
		cpuidle_curr_governor->disable(drv, dev);

	cpuidle_remove_device_sysfs(dev);
	enabled_devices--;
}

EXPORT_SYMBOL_GPL(cpuidle_disable_device);

/*
 * 已持 cpuidle_lock 的底层解除注册：从全局链表和 per-CPU 发布槽移除设备，释放驱动
 * 模块引用并清 registered。调用前设备必须已禁用；本函数不处理 coupled 或 sysfs。
 */
static void __cpuidle_unregister_device(struct cpuidle_device *dev)
{
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);

	list_del(&dev->device_list);
	per_cpu(cpuidle_devices, dev->cpu) = NULL;
	module_put(drv->owner);

	dev->registered = 0;
}

/* 重置一次注册周期的运行统计与观察字段；不改 cpu、coupled_cpus 等配置身份。 */
static void __cpuidle_device_init(struct cpuidle_device *dev)
{
	memset(dev->states_usage, 0, sizeof(dev->states_usage));
	dev->last_residency_ns = 0;
	dev->next_hrtimer = 0;
}

/**
 * __cpuidle_register_device - internal register function called before register
 * and enable routines
 * @dev: the cpu
 *
 * cpuidle_lock mutex must be held before this is called
 *
 * 在控制面锁下建立设备的核心注册关系：验证每 CPU 槽唯一性，取得驱动模块引用，
 * 把驱动状态标志折算为设备级禁用位，发布 per-CPU 指针和全局链表节点，再登记 coupled
 * 协调。coupled 登记失败时回滚所有核心关系；成功后才设置 registered。
 */
static int __cpuidle_register_device(struct cpuidle_device *dev)
{
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	unsigned int cpu = dev->cpu;
	int i, ret;

	if (per_cpu(cpuidle_devices, cpu)) {
		pr_info("CPU%d: cpuidle device already registered\n", cpu);
		return -EEXIST;
	}

	if (!try_module_get(drv->owner))
		/* 模块引用保证设备已发布期间驱动回调和状态表不会被卸载。 */
		return -EINVAL;

	/* 将驱动的永久不可用/默认关闭属性投影到每设备统计槽，保留禁用原因位。 */
	for (i = 0; i < drv->state_count; i++) {
		if (drv->states[i].flags & CPUIDLE_FLAG_UNUSABLE)
			dev->states_usage[i].disable |= CPUIDLE_STATE_DISABLED_BY_DRIVER;

		if (drv->states[i].flags & CPUIDLE_FLAG_OFF)
			dev->states_usage[i].disable |= CPUIDLE_STATE_DISABLED_BY_USER;
	}

	per_cpu(cpuidle_devices, cpu) = dev;
	/* 锁内发布，读侧只有在 handler 安装后才会进入无锁快路径。 */
	list_add(&dev->device_list, &cpuidle_detected_devices);

	ret = cpuidle_coupled_register_device(dev);
	if (ret)
		/* 回滚 per-CPU 槽、链表和模块引用；registered 尚未置位。 */
		__cpuidle_unregister_device(dev);
	else
		dev->registered = 1;

	return ret;
}

/**
 * cpuidle_register_device - registers a CPU's idle PM feature
 * @dev: the cpu
 *
 * 完整注册事务：锁内初始化统计、建立核心关系、创建顶层 sysfs、启用设备，最后安装
 * idle handler。失败标签严格按已完成阶段逆序回滚。guard(mutex) 保证所有返回路径自动
 * 解锁；设备存储由调用方拥有，必须持续到 unregister 完成。
 */
int cpuidle_register_device(struct cpuidle_device *dev)
{
	int ret = -EBUSY;

	if (!dev)
		return -EINVAL;

	guard(mutex)(&cpuidle_lock);

	if (dev->registered)
		return ret;

	__cpuidle_device_init(dev);

	ret = __cpuidle_register_device(dev);
	if (ret)
		return ret;

	ret = cpuidle_add_sysfs(dev);
	if (ret)
		goto out_unregister;

	ret = cpuidle_enable_device(dev);
	if (ret)
		goto out_sysfs;

	cpuidle_install_idle_handler();
	/* enabled_devices 已增加，因此这里把初始化结果发布给 idle loop。 */

	return ret;

out_sysfs:
	/* enable 失败时设备级 sysfs 已自行回滚，此处移除注册级 sysfs。 */
	cpuidle_remove_sysfs(dev);
out_unregister:
	/* 最后撤销核心发布关系与驱动模块引用。 */
	__cpuidle_unregister_device(dev);

	return ret;
}

EXPORT_SYMBOL_GPL(cpuidle_register_device);

/*
 * 已持 cpuidle_lock 且 handler 已暂停的解除注册主体。按“禁用 -> sysfs -> 核心发布 ->
 * coupled 协调”顺序拆除；空指针或未注册设备幂等返回。调用后调用方仍拥有 dev 存储。
 */
void cpuidle_unregister_device_no_lock(struct cpuidle_device *dev)
{
	if (!dev || dev->registered == 0)
		return;

	lockdep_assert_held(&cpuidle_lock);

	cpuidle_disable_device(dev);

	cpuidle_remove_sysfs(dev);

	__cpuidle_unregister_device(dev);

	cpuidle_coupled_unregister_device(dev);
}
EXPORT_SYMBOL_GPL(cpuidle_unregister_device_no_lock);

/**
 * cpuidle_unregister_device - unregisters a CPU's idle PM feature
 * @dev: the cpu
 *
 * 对外包装先暂停所有 idle 快路径并取得控制面锁，再运行无锁版本，最后按剩余设备数
 * 决定是否重新安装 handler。返回后设备已不再被框架引用，可由其所有者释放或复用。
 */
void cpuidle_unregister_device(struct cpuidle_device *dev)
{
	if (!dev || dev->registered == 0)
		return;

	cpuidle_pause_and_lock();
	cpuidle_unregister_device_no_lock(dev);
	cpuidle_resume_and_unlock();
}
EXPORT_SYMBOL_GPL(cpuidle_unregister_device);

/**
 * cpuidle_unregister: unregister a driver and the devices. This function
 * can be used only if the driver has been previously registered through
 * the cpuidle_register function.
 *
 * @drv: a valid pointer to a struct cpuidle_driver
 *
 * 仅供通过 cpuidle_register() 走通用批量路径的驱动使用。遍历驱动 cpumask 中的每个
 * per-CPU 通用设备并完整解除注册，最后注销驱动本身；顺序保证不再有设备引用状态表
 * 后才撤销驱动。驱动和 cpumask 的生命周期由调用者维持到函数返回。
 */
void cpuidle_unregister(struct cpuidle_driver *drv)
{
	int cpu;
	struct cpuidle_device *device;

	for_each_cpu(cpu, drv->cpumask) {
		/* cpuidle_register() 使用本文件的 per-CPU 实体，因此可按同一地址成对拆除。 */
		device = &per_cpu(cpuidle_dev, cpu);
		cpuidle_unregister_device(device);
	}

	cpuidle_unregister_driver(drv);
}
EXPORT_SYMBOL_GPL(cpuidle_unregister);

/**
 * cpuidle_register: registers the driver and the cpu devices with the
 * coupled_cpus passed as parameter. This function is used for all common
 * initialization pattern there are in the arch specific drivers. The
 * devices is globally defined in this file.
 *
 * @drv         : a valid pointer to a struct cpuidle_driver
 * @coupled_cpus: a cpumask for the coupled states
 *
 * Returns 0 on success, < 0 otherwise
 *
 * 通用批量注册助手先注册驱动，再为 cpumask 中每个 CPU 初始化本文件持有的设备实体。
 * 可选 coupled_cpus 按值复制进每个设备；任一 CPU 注册失败就调用 cpuidle_unregister()
 * 回滚此前全部设备和驱动，因而成功返回时是全有，失败返回时是全无。
 */
int cpuidle_register(struct cpuidle_driver *drv,
		     const struct cpumask *const coupled_cpus)
{
	int ret, cpu;
	struct cpuidle_device *device;

	ret = cpuidle_register_driver(drv);
	if (ret) {
		pr_err("failed to register cpuidle driver\n");
		return ret;
	}

	for_each_cpu(cpu, drv->cpumask) {
		device = &per_cpu(cpuidle_dev, cpu);
		device->cpu = cpu;

#ifdef CONFIG_ARCH_NEEDS_CPU_IDLE_COUPLED
		/*
		 * On multiplatform for ARM, the coupled idle states could be
		 * enabled in the kernel even if the cpuidle driver does not
		 * use it. Note, coupled_cpus is a struct copy.
		 * ARM multiplatform 内核可能编译 coupled 支持，即便具体驱动不用它；仅在调用方
		 * 提供掩码时按值复制，设备不借用外部 cpumask 指针及其生命周期。
		 */
		if (coupled_cpus)
			device->coupled_cpus = *coupled_cpus;
#endif
		ret = cpuidle_register_device(device);
		if (!ret)
			continue;

		pr_err("Failed to register cpuidle device for cpu%d\n", cpu);

		/* 事务式回滚包含当前失败前已成功的 CPU，并最终撤销驱动。 */
		cpuidle_unregister(drv);
		break;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(cpuidle_register);

/**
 * cpuidle_init - core initializer
 *
 * core_initcall 阶段若启动参数已关闭框架则返回 -ENODEV；否则只创建用户空间接口。
 * 驱动和设备仍由后续架构/平台初始化注册，initialized 要等首个设备启用后才发布。
 */
static int __init cpuidle_init(void)
{
	if (cpuidle_disabled())
		return -ENODEV;

	return cpuidle_add_interface();
}

/* off/governor 均为启动后只读参数；前者关闭框架，后者指定初始 governor 名称。 */
module_param(off, int, 0444);
module_param_string(governor, param_governor, CPUIDLE_NAME_LEN, 0444);
core_initcall(cpuidle_init);
