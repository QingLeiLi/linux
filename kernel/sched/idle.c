// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic entry points for the idle threads and
 * implementation of the idle task scheduling class.
 *
 * (NOTE: these are not related to SCHED_IDLE batch scheduled
 *        tasks which are handled in sched/fair.c )
 */
/*
 * 本文件同时承载两条相连但不同的路径：
 *
 *   每 CPU idle 线程：do_idle() → poll / cpuidle / arch_cpu_idle()
 *   调度类兜底：      idle_sched_class → 始终选择 rq->idle
 *
 * 前者在 runqueue 没有普通 runnable task 时让处理器轮询或进入低功耗状态；后者
 * 保证调度器永远有一个可选择的 task。英文特别提醒：用户指定 SCHED_IDLE 的
 * 批处理任务仍由 fair class 管理，并不是这里的每 CPU idle task。
 *
 * 核心并发协议是 TIF_POLLING_NRFLAG 与 TIF_NEED_RESCHED：idle task 轮询时，
 * waker 只需设置 need_resched 而可省去 IPI；idle 即将睡眠时先清 polling、做
 * 配对屏障并复查 need_resched，确保 waker 要么被轮询看见，要么发送 IPI，
 * 不会把工作留给已经睡着且收不到通知的 CPU。
 */
#include <linux/cpuidle.h>
#include <linux/suspend.h>
#include <linux/livepatch.h>
#include "sched.h"
#include "smp.h"

/* Linker adds these: start and end of __cpuidle functions */
/*
 * 链接脚本生成这两个只读边界符号，分别标记 __cpuidle text section 的起止地址。
 * 它们不拥有存储；cpu_in_idle() 借用其地址判断采样 PC 是否落在 idle 代码中。
 */
extern char __cpuidle_text_start[], __cpuidle_text_end[];

/**
 * sched_idle_set_state - Record idle state for the current CPU.
 * @idle_state: State to record.
 */
/*
 * sched_idle_set_state() - 向调度器发布当前 CPU 正在进入的 cpuidle state。
 *
 * cpuidle 核心在进出具体状态时调用；idle_set_state(this_rq(), ...) 把借用指针
 * 记录到本 CPU rq，调度/观测路径可在 RCU 保护下读取。@idle_state 为输入借用，
 * 可为 NULL（表示离开/没有状态），本函数不持有引用、不转移 ownership。
 * 调用发生在本 CPU idle 低层路径，不能睡眠；返回无直接值。CONFIG_CPU_IDLE=n
 * 时 helper 为空实现，接口仍保留，外界无可观察状态变化。
 */
void sched_idle_set_state(struct cpuidle_state *idle_state)
{
	idle_set_state(this_rq(), idle_state);
}

/*
 * cpu_idle_force_poll 是全局嵌套请求计数：大于 0 时所有 CPU 的 idle 路径以轮询
 * 代替深度睡眠，0 时允许正常 cpuidle。__read_mostly 把高频读取、低频写入对象
 * 放到适合只读共享的区域；调用者必须成对 enable/disable，写侧本身不加锁，
 * 依赖控制路径串行化。启动参数还会直接把它设为 0/1。
 */
static int __read_mostly cpu_idle_force_poll;

/*
 * cpu_idle_poll_ctrl() - 成对增加或减少强制 idle 轮询请求。
 *
 * @enable 为输入：true 取得一份轮询请求，false 归还一份。函数不睡眠、无返回；
 * 正计数使 do_idle() 选择 cpu_idle_poll()，归零后恢复正常选择。它没有 token
 * 句柄，调用者必须保证配对和写侧串行；下溢触发 WARN，但不会自动修正负值。
 */
void cpu_idle_poll_ctrl(bool enable)
{
	/* 多个控制者可嵌套请求，只有最后一个 disable 才恢复低功耗 idle。 */
	if (enable) {
		cpu_idle_force_poll++;
	} else {
		cpu_idle_force_poll--;
		WARN_ON_ONCE(cpu_idle_force_poll < 0);
	}
}

#ifdef CONFIG_GENERIC_IDLE_POLL_SETUP
/*
 * cpu_idle_poll_setup() - 处理启动参数 nohlt，强制使用 idle 轮询。
 *
 * @__unused 是启动参数尾部的借用字符串，本实现无需解析且不保存。仅在 early
 * boot 的命令行解析上下文执行，不睡眠；把全局计数设为 1，返回 1 表示参数已
 * 被消费。后续 do_idle() 因此不会进入普通 halt/cpuidle 分支。
 */
static int __init cpu_idle_poll_setup(char *__unused)
{
	cpu_idle_force_poll = 1;

	return 1;
}
__setup("nohlt", cpu_idle_poll_setup);

/*
 * cpu_idle_nopoll_setup() - 处理启动参数 hlt，撤销启动期强制轮询。
 *
 * @__unused 为未使用且不保存的借用字符串。函数仅在 early boot 执行，不睡眠；
 * 写 cpu_idle_force_poll=0，返回 1 表示已处理。命令行顺序决定 hlt/nohlt 最终值。
 */
static int __init cpu_idle_nopoll_setup(char *__unused)
{
	cpu_idle_force_poll = 0;

	return 1;
}
__setup("hlt", cpu_idle_nopoll_setup);
#endif /* CONFIG_GENERIC_IDLE_POLL_SETUP */
/* 上述条件块只在架构采用通用 idle-poll 启动参数时编译；否则由架构自行决定。 */

/*
 * cpu_idle_poll() - 在中断开启时忙等，直到出现重调度或已到期广播 tick。
 *
 * 调用位置：do_idle() 的强制轮询/广播已到期分支 → 本函数 → 返回 do_idle()。
 * 入参：无。入口本 CPU 中断关闭、current 是 per-CPU idle task、polling bit 已置位，
 * 且处于 tick nohz idle 区间。函数不会调度睡眠，但会临时开中断并执行 CPU relax；
 * 返回 1 仅表示执行过 poll，不代表空闲时长。退出时中断最终开启，ownership 无。
 * __cpuidle/noinline 使 PC 可归类到 idle section，并避免内联破坏观测边界。
 */
static noinline int __cpuidle cpu_idle_poll(void)
{
	/* 阶段 1：允许 trace/context-tracking 观测轮询 idle，而不把插桩放进 noinstr 区。 */
	instrumentation_begin();
	trace_cpu_idle(0, smp_processor_id());
	stop_critical_timings();
	ct_cpuidle_enter();

	/*
	 * 阶段 2：开中断后轮询 need_resched。waker 看到 polling bit 时可不发 IPI，
	 * 只设置该位；循环会自行退出。广播 tick 已过期时也不再继续空转。
	 */
	raw_local_irq_enable();
	while (!tif_need_resched() &&
	       (cpu_idle_force_poll || tick_check_broadcast_expired()))
		cpu_relax();
	/* 关中断后成对退出 context tracking，避免状态切换间被普通插桩打断。 */
	raw_local_irq_disable();

	ct_cpuidle_exit();
	start_critical_timings();
	trace_cpu_idle(PWR_EVENT_EXIT, smp_processor_id());
	/* 按 idle 调用契约在返回前开启本地中断。 */
	local_irq_enable();
	instrumentation_end();

	return 1;
}

/* Weak implementations for optional arch specific functions */
/*
 * 以下弱符号是架构 idle 生命周期钩子的默认实现；架构可用同名强符号覆盖。
 * prepare 在 CPU 进入永久 idle 循环前执行，enter/exit 包围每次低功耗尝试，
 * dead 用于 offline CPU 的不返回终点，arch_cpu_idle 是无 cpuidle 时的实际指令。
 * 英文所说“optional”意味着通用代码始终可调用它们，而不是每处加条件编译。
 */
/* 无参数、无返回、副作用为空；架构覆盖版仍必须遵守 idle 上下文且不能睡眠。 */
void __weak arch_cpu_idle_prepare(void) { }
/* 每轮 idle 前通知架构；默认无动作、无返回，入口中断状态由 do_idle() 管理。 */
void __weak arch_cpu_idle_enter(void) { }
/* 每轮 idle 后通知架构；默认无动作、无返回，不拥有任何对象。 */
void __weak arch_cpu_idle_exit(void) { }
/* offline CPU 的最终入口；默认永久自旋且绝不返回，架构通常替换为停机指令。 */
void __weak __noreturn arch_cpu_idle_dead(void) { while (1); }
/*
 * arch_cpu_idle() - 无 cpuidle 框架时执行架构默认 idle。
 *
 * 入参和直接返回值均无；应在本 CPU idle task、中断关闭且不可睡眠的上下文调用，
 * 架构覆盖版通常执行 halt/wfi。默认实现无法低功耗等待，故设置 force_poll=1，
 * 让后续循环退化到可被 need_resched 观察打断的忙轮询。
 */
void __weak arch_cpu_idle(void)
{
	cpu_idle_force_poll = 1;
}

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST_IDLE
/*
 * arch_needs_tick_broadcast 是运行期静态键：默认关闭时 cond_* 热路径接近 NOP；
 * 需要由广播时钟事件设备接管本地 tick 的架构启用后，所有 CPU 在默认 idle
 * 前后登记/撤销广播状态。它表达架构能力，不拥有具体 cpuidle state。
 */
DEFINE_STATIC_KEY_FALSE(arch_needs_tick_broadcast);

/*
 * cond_tick_broadcast_enter() - 必要时通知 tick 层本 CPU 将停止本地时钟事件。
 *
 * 入参、返回值均无；仅由 default_idle_call() 在中断关闭的 idle 路径调用，不
 * 睡眠。静态键关闭无副作用，开启则 tick_broadcast_enter() 更新本 CPU 广播
 * 掩码，使其他时钟设备可在截止时间唤醒它；必须与 exit 成对。
 */
static inline void cond_tick_broadcast_enter(void)
{
	if (static_branch_unlikely(&arch_needs_tick_broadcast))
		tick_broadcast_enter();
}

/*
 * cond_tick_broadcast_exit() - 必要时撤销本 CPU 的 tick 广播 idle 登记。
 *
 * 无参数、无返回且不睡眠；在 arch_cpu_idle() 返回后调用。静态键开启时恢复本
 * CPU 时钟事件归属，与 enter 成对；关闭时无副作用。
 */
static inline void cond_tick_broadcast_exit(void)
{
	if (static_branch_unlikely(&arch_needs_tick_broadcast))
		tick_broadcast_exit();
}
#else /* !CONFIG_GENERIC_CLOCKEVENTS_BROADCAST_IDLE: */
/* 未启用通用广播 idle 时，架构不需要通用层登记；两个钩子编译为空操作。 */
/* 无参数、无返回、不睡眠，保持 default_idle_call() 的跨配置调用结构一致。 */
static inline void cond_tick_broadcast_enter(void) { }
/* 无参数、无返回、不睡眠；没有 enter 状态需要撤销。 */
static inline void cond_tick_broadcast_exit(void) { }
#endif /* !CONFIG_GENERIC_CLOCKEVENTS_BROADCAST_IDLE */
/* 条件块结束：两种配置都向下游提供同名、同上下文契约的成对 helper。 */

/**
 * default_idle_call - Default CPU idle routine.
 *
 * To use when the cpuidle framework cannot be used.
 */
/*
 * default_idle_call() - cpuidle 不可用时执行架构默认 idle 尝试。
 *
 * 调用位置：cpuidle_idle_call() fallback → 本函数 → arch_cpu_idle()。入参无；
 * 入口 current 为本 CPU idle task、本地中断关闭、polling bit 已置位且无需持锁。
 * 函数不能睡眠，但架构指令可让 CPU 低功耗停顿直到中断。返回无直接值，保证
 * 本地中断开启。若清 polling 后发现 need_resched 已置位，跳过低功耗入口，
 * 让外层尽快调度；否则成对维护 tick broadcast、context tracking 和 trace。
 */
void __cpuidle default_idle_call(void)
{
	/* instrumentation 区包住 trace/tick/arch hook，保持 __cpuidle 代码可观测。 */
	instrumentation_begin();
	/*
	 * 清 polling 并通过配对屏障复查 need_resched：false 才说明当前既未观察到
	 * 工作，又已让未来 waker 知道必须发 IPI，可以安全走向睡眠指令。
	 */
	if (!current_clr_polling_and_test()) {
		cond_tick_broadcast_enter();
		trace_cpu_idle(1, smp_processor_id());
		stop_critical_timings();

		/* 阶段 2：context tracking 进入 idle，再由架构执行实际低功耗指令。 */
		ct_cpuidle_enter();
		arch_cpu_idle();
		ct_cpuidle_exit();

		start_critical_timings();
		trace_cpu_idle(PWR_EVENT_EXIT, smp_processor_id());
		cond_tick_broadcast_exit();
	}
	/* 无论是否真正进入 arch idle，统一履行返回时 IRQ 开启的契约。 */
	local_irq_enable();
	instrumentation_end();
}

/*
 * call_cpuidle_s2idle() - 在最终重调度复查后进入 suspend-to-idle 专用状态。
 *
 * @drv: 借用的本 CPU cpuidle driver，不可为 NULL，调用期间保持注册。
 * @dev: 借用的本 CPU cpuidle device，不可为 NULL，由框架持有。
 * @max_latency_ns: 输入的最大允许退出延迟，单位纳秒；用于筛选最深状态。
 *
 * 入口为 idle task、中断关闭、polling bit 已置位，不持可睡眠锁；函数自身不按
 * 调度睡眠，但 CPU 可停在低功耗状态。若清 polling 后发现 need_resched，返回
 * -EBUSY 且不进入硬件；否则返回 cpuidle_enter_s2idle() 的状态索引/结果，该
 * helper 负责选择支持 enter_s2idle 的最深合规状态及恢复 IRQ。ownership 不变。
 */
static int call_cpuidle_s2idle(struct cpuidle_driver *drv,
			       struct cpuidle_device *dev,
			       u64 max_latency_ns)
{
	if (current_clr_polling_and_test())
		return -EBUSY;

	return cpuidle_enter_s2idle(drv, dev, max_latency_ns);
}

/*
 * call_cpuidle() - 复查唤醒竞态后进入 governor/调用者选定的 cpuidle state。
 *
 * @drv、@dev 是本 CPU 已注册对象的借用指针，不可为 NULL；@next_state 是 drv
 * states[] 的有效索引，由 governor 或 deepest-state 查询产生。入口中断关闭、
 * polling bit 已置位且 current 是 idle task。函数不做调度睡眠；返回实际进入
 * 状态索引或后端负错误码。-EBUSY 表示 need_resched 在最终窗口出现，此时把
 * last_residency_ns 清零并主动开 IRQ；正常路径由 cpuidle_enter() 负责 IRQ 恢复。
 */
static int call_cpuidle(struct cpuidle_driver *drv, struct cpuidle_device *dev,
		      int next_state)
{
	/*
	 * The idle task must be scheduled, it is pointless to go to idle, just
	 * update no idle residency and return.
	 */
	/*
	 * 英文所述竞态：选好 state 后普通任务可能已经被唤醒。清 polling 并复查
	 * need_resched 把这个窗口闭合；有工作时继续进 idle 既增加延迟也可能漏 IPI。
	 */
	if (current_clr_polling_and_test()) {
		dev->last_residency_ns = 0;
		local_irq_enable();
		return -EBUSY;
	}

	/*
	 * Enter the idle state previously returned by the governor decision.
	 * This function will block until an interrupt occurs and will take
	 * care of re-enabling the local interrupts
	 */
	/*
	 * cpuidle 核心更新统计、按需要切换广播 timer 并调用 driver ->enter；返回值
	 * 可能与 next_state 不同，表示后端实际进入的状态，且返回时 IRQ 已恢复。
	 */
	return cpuidle_enter(drv, dev, next_state);
}

/*
 * idle_call_stop_or_retain_tick() - 落实本轮 idle 是否停止调度 tick 的决策。
 *
 * @stop_tick 为 governor/调用者输入；true 要停 tick。若 tick 已经停止，即使本轮
 * 建议保留也继续调用 stop helper 维护 nohz 状态；否则 retain helper 明确保持。
 * 只能由本 CPU idle 路径调用，不睡眠、无返回、无 ownership 变化。
 */
static void idle_call_stop_or_retain_tick(bool stop_tick)
{
	if (stop_tick || tick_nohz_tick_stopped())
		tick_nohz_idle_stop_tick();
	else
		tick_nohz_idle_retain_tick();
}

/**
 * cpuidle_idle_call - the main idle function
 *
 * NOTE: no locks or semaphores should be used here
 *
 * On architectures that support TIF_POLLING_NRFLAG, is called with polling
 * set, and it returns with polling set.  If it ever stops polling, it
 * must clear the polling bit.
 */
/*
 * cpuidle_idle_call() - 为当前 CPU 选择并执行一次 idle 状态。
 *
 * 调用位置：do_idle() 每轮 → 本函数 → default_idle_call() 或 cpuidle
 * governor/driver → 返回 do_idle()。@stop_tick 是输入建议，表示 nohz 是否可停
 * 调度 tick；governor 可通过输出参数把它改为 false。入口 current 为本 CPU
 * idle task、本地中断关闭、polling bit 已置位；不得持锁或信号量，英文强调这一
 * 点是因为底层可能让 CPU 长时间停顿，持锁会阻塞其他 CPU 取得进展。
 *
 * 函数不发生调度意义的睡眠，但会进入硬件低功耗。返回无直接值；保证重新设置
 * polling bit 且本地中断开启。@drv/@dev 是 cpuidle 框架持有的借用对象；状态
 * 选择与统计由框架负责。快速路径处理 need_resched，fallback 使用架构 idle，
 * s2idle 绕过 governor，普通路径则 select → tick 决策 → enter → reflect。
 */
static void cpuidle_idle_call(bool stop_tick)
{
	/* dev/driver 在本 CPU idle 调用期由 cpuidle 注册生命周期保证，均为借用指针。 */
	struct cpuidle_device *dev = cpuidle_get_device();
	struct cpuidle_driver *drv = cpuidle_get_cpu_driver(dev);
	/* next_state 是候选索引；entered_state 是后端实际结果，可能为负错误码。 */
	int next_state, entered_state;

	/*
	 * Check if the idle task must be rescheduled. If it is the
	 * case, exit the function after re-enabling the local IRQ.
	 */
	/*
	 * 英文说明：进入复杂选择前先检查当前 idle task 是否已被请求重调度。
	 * 有工作时不清 polling，直接开 IRQ 返回，由 do_idle() 离开循环并调度。
	 */
	if (need_resched()) {
		local_irq_enable();
		return;
	}

	if (cpuidle_not_available(drv, dev)) {
		/*
		 * fallback：没有 driver/device 或框架被禁用。仍先落实 tick 决策，
		 * 然后用 default_idle_call() 执行架构级 idle；其返回时 IRQ 已开启。
		 */
		idle_call_stop_or_retain_tick(stop_tick);

		default_idle_call();
		goto exit_idle;
	}

	/*
	 * Suspend-to-idle ("s2idle") is a system state in which all user space
	 * has been frozen, all I/O devices have been suspended and the only
	 * activity happens here and in interrupts (if any). In that case bypass
	 * the cpuidle governor and go straight for the deepest idle state
	 * available.  Possibly also suspend the local tick and the entire
	 * timekeeping to prevent timer interrupts from kicking us out of idle
	 * until a proper wakeup interrupt happens.
	 */
	/*
	 * suspend-to-idle 冻结用户态和设备，活动只来自本路径及唤醒中断；因此绕过
	 * 面向运行态预测的 governor，优先挑选满足延迟约束的最深状态，并允许冻结
	 * 本地 tick/时间维护，避免普通定时器把系统无谓唤醒。
	 */

	if (idle_should_enter_s2idle() || dev->forced_idle_latency_limit_ns) {
		/* max_latency_ns 统一承载系统 QoS 或注入者给出的纳秒级退出延迟上限。 */
		u64 max_latency_ns;

		if (idle_should_enter_s2idle()) {
			/* QoS 接口返回微秒；乘 NSEC_PER_USEC 后交给 s2idle 状态筛选。 */
			max_latency_ns = cpu_wakeup_latency_qos_limit() *
					 NSEC_PER_USEC;

			entered_state = call_cpuidle_s2idle(drv, dev,
							    max_latency_ns);
			/* 正索引表示 s2idle 专用入口已完成，本轮无需通用 deepest fallback。 */
			if (entered_state > 0)
				goto exit_idle;
		} else {
			/* 强制 idle 注入直接使用设备上暂存的调用者延迟要求。 */
			max_latency_ns = dev->forced_idle_latency_limit_ns;
		}

		/* deepest-state 通用入口要求 tick 已停止，随后按延迟上限选择候选。 */
		tick_nohz_idle_stop_tick();

		next_state = cpuidle_find_deepest_state(drv, dev, max_latency_ns);
		call_cpuidle(drv, dev, next_state);
	} else if (drv->state_count > 1) {
		/*
		 * stop_tick is expected to be true by default by cpuidle
		 * governors, which allows them to select idle states with
		 * target residency above the tick period length.
		 */
		/*
		 * 普通多状态系统默认允许停 tick，governor 才能选择目标驻留时间超过
		 * 一个 tick 周期的深状态；它仍可把 stop_tick 改回 false。
		 */
		stop_tick = true;

		/*
		 * Ask the cpuidle framework to choose a convenient idle state.
		 */
		/* governor 根据预计 idle 时长、退出延迟和历史统计返回非负状态索引。 */
		next_state = cpuidle_select(drv, dev, &stop_tick);

		idle_call_stop_or_retain_tick(stop_tick);

		entered_state = call_cpuidle(drv, dev, next_state);
		/*
		 * Give the governor an opportunity to reflect on the outcome
		 */
		/*
		 * 实际进入索引/错误反馈给 governor，供下一轮修正预测；这里不把失败
		 * 向 do_idle() 传播，因为外层只需继续循环或响应 need_resched。
		 */
		cpuidle_reflect(dev, entered_state);
	} else {
		idle_call_stop_or_retain_tick(stop_tick);

		/*
		 * If there is only a single idle state (or none), there is
		 * nothing meaningful for the governor to choose.  Skip the
		 * governor and always use state 0.
		 */
		/* 单状态系统没有选择空间，跳过 governor 可减少热路径开销。 */
		call_cpuidle(drv, dev, 0);
	}

exit_idle:
	/*
	 * 所有执行分支在此重新宣告 idle task 会轮询 need_resched；这让后续 waker
	 * 可以按 polling 协议决定省略 IPI。即使 call_cpuidle 因 -EBUSY 返回也一致。
	 */
	__current_set_polling();

	/*
	 * It is up to the idle functions to re-enable local interrupts
	 */
	/*
	 * 英文说明：底层 idle helper 契约要求自行恢复 IRQ。WARN 捕获违约，并在
	 * 调试告警后兜底开中断，确保 do_idle() 下一轮不在错误状态运行。
	 */
	if (WARN_ON_ONCE(irqs_disabled()))
		local_irq_enable();
}

/*
 * Generic idle loop implementation
 *
 * Called with polling cleared.
 */
/*
 * do_idle() - 运行当前 CPU 的通用 idle 循环，直到需要调度普通任务。
 *
 * 上述英文说明：入口 polling bit 已清。调用位置是 cpu_startup_entry() 的永久
 * 循环以及 play_idle_precise() 的受控注入循环。入参无；current 必须是当前 CPU
 * 的 idle 语义任务、抢占受调用路径约束，入口不持 rq 锁。函数在硬件层可停顿，
 * 但不会以普通可阻塞任务身份睡眠。返回无直接值；online CPU 路径最终执行一次
 * schedule_idle()，可能切换到 runnable task，再回到调用者；offline CPU 路径
 * 报告死亡后进入不返回的 arch_cpu_idle_dead()。
 *
 * 阶段：处理 hotplug → nohz balance → 发布 polling/nohz idle → 在 IRQ-off 窗口
 * 选择 poll 或 cpuidle → 发现 need_resched 后清 polling 并配对屏障 → flush 远端
 * 调用 → schedule_idle() → 更新 livepatch 状态。polling 与 need_resched 的屏障
 * 是本函数最关键的不变量，删除会造成 waker 省略 IPI 而 idle CPU 已经睡下。
 */
static void do_idle(void)
{
	/* cpu 是本轮固定的逻辑 CPU；idle task 不应迁移。got_tick 记录上一轮 tick 事实。 */
	int cpu = smp_processor_id();
	bool got_tick = false;

	/* 阶段 1：CPU offline 后不再参与调度，必须从 idle 上下文完成死亡握手。 */
	if (cpu_is_offline(cpu)) {
		local_irq_disable();
		/* All per-CPU kernel threads should be done by now. */
		/*
		 * 英文说明：此时所有 per-CPU kthread 理应已经迁走/停止，所以不该再有
		 * 重调度请求；告警只诊断热插拔协议违约，随后仍报告 idle-dead。
		 */
		WARN_ON_ONCE(need_resched());
		cpuhp_report_idle_dead();
		arch_cpu_idle_dead();
	}

	/*
	 * Check if we need to update blocked load
	 */
	/*
	 * 英文说明：CPU 刚空闲时尝试执行 NOHZ blocked-load/balance 更新，避免该 CPU
	 * 停 tick 后负载衰减长期无人处理；helper 自行判断是否真的需要工作。
	 */
	nohz_run_idle_balance(cpu);

	/*
	 * If the arch has a polling bit, we maintain an invariant:
	 *
	 * Our polling bit is clear if we're not scheduled (i.e. if rq->curr !=
	 * rq->idle). This means that, if rq->idle has the polling bit set,
	 * then setting need_resched is guaranteed to cause the CPU to
	 * reschedule.
	 */
	/*
	 * 英文给出 polling 不变量：rq->curr 不是 rq->idle 时 polling 必须清；反之
	 * idle task 置位后会主动检查 need_resched。于是 waker 观察到 polling=1 时
	 * 可以只设重调度位而省 IPI，仍保证本 CPU 自行走到 schedule_idle()。
	 */

	__current_set_polling();
	/* 通知 tick/nohz 层本 CPU 进入 idle 记账区，退出前必须成对调用 exit。 */
	tick_nohz_idle_enter();

	/* 阶段 2：只要没有重调度请求，就反复进行一次 poll/低功耗 idle 尝试。 */
	while (!need_resched()) {

		/*
		 * Interrupts shouldn't be re-enabled from that point on until
		 * the CPU sleeping instruction is reached. Otherwise an interrupt
		 * may fire and queue a timer that would be ignored until the CPU
		 * wakes from the sleeping instruction. And testing need_resched()
		 * doesn't tell about pending needed timer reprogram.
		 *
		 * Several cases to consider:
		 *
		 * - SLEEP-UNTIL-PENDING-INTERRUPT based instructions such as
		 *   "wfi" or "mwait" are fine because they can be entered with
		 *   interrupt disabled.
		 *
		 * - sti;mwait() couple is fine because the interrupts are
		 *   re-enabled only upon the execution of mwait, leaving no gap
		 *   in-between.
		 *
		 * - ROLLBACK based idle handlers with the sleeping instruction
		 *   called with interrupts enabled are NOT fine. In this scheme
		 *   when the interrupt detects it has interrupted an idle handler,
		 *   it rolls back to its beginning which performs the
		 *   need_resched() check before re-executing the sleeping
		 *   instruction. This can leak a pending needed timer reprogram.
		 *   If such a scheme is really mandatory due to the lack of an
		 *   appropriate CPU sleeping instruction, then a FAST-FORWARD
		 *   must instead be applied: when the interrupt detects it has
		 *   interrupted an idle handler, it must resume to the end of
		 *   this idle handler so that the generic idle loop is iterated
		 *   again to reprogram the tick.
		 */
		/*
		 * 英文分析的是“检查工作与执行睡眠指令之间”的 IRQ 窗口。这里先关 IRQ，
		 * 之后直到真正进入 wfi/mwait 等指令前都不能任意重开；否则中断可能在缝隙
		 * 中排入需要重编程的 timer，CPU 随后仍执行睡眠，而 need_resched 并不能
		 * 表达该 timer 事件，造成延迟。可在 IRQ-off 下进入的指令安全；sti;mwait
		 * 把开中断与睡眠做成无缝组合也安全。若架构只能 rollback 到 handler 开头，
		 * 仍可能漏 timer 重编程，必须 fast-forward 到 handler 末尾，让通用循环重跑。
		 */
		local_irq_disable();

		/* 架构钩子和 nocb deferred wakeup flush 均位于每次硬件 idle 尝试之前。 */
		arch_cpu_idle_enter();
		rcu_nocb_flush_deferred_wakeup();

		/*
		 * In poll mode we re-enable interrupts and spin. Also if we
		 * detected in the wakeup from idle path that the tick
		 * broadcast device expired for us, we don't want to go deep
		 * idle as we know that the IPI is going to arrive right away.
		 */
		/*
		 * 英文说明两条浅路径：显式 force_poll，或广播设备已替本 CPU 到期。后者
		 * 意味着 IPI 即将到达，进入深 idle 只有额外开销；先重启 tick 再开 IRQ 轮询。
		 * 其余情况交给 cpuidle_idle_call()，该函数负责状态选择和 IRQ 恢复。
		 */
		if (cpu_idle_force_poll || tick_check_broadcast_expired()) {
			tick_nohz_idle_restart_tick();
			cpu_idle_poll();
		} else {
			cpuidle_idle_call(got_tick);
		}
		/* 记录本轮 idle 是否实际收到 tick，传给下一轮的 stop/retain 决策。 */
		got_tick = tick_nohz_idle_got_tick();
		arch_cpu_idle_exit();
	}

	/*
	 * Since we fell out of the loop above, we know TIF_NEED_RESCHED must
	 * be set, propagate it into PREEMPT_NEED_RESCHED.
	 *
	 * This is required because for polling idle loops we will not have had
	 * an IPI to fold the state for us.
	 */
	/*
	 * 英文说明：循环因 TIF_NEED_RESCHED 退出。轮询 CPU 可能没有收到调度 IPI，
	 * 所以缺少 IPI handler 把 thread flag 折叠到抢占计数的步骤；这里显式补做，
	 * 保证后续抢占检查同样看到请求。
	 */
	preempt_set_need_resched();
	/* 阶段 3：结束 nohz idle，并在 current 即将不再主动轮询前清 polling。 */
	tick_nohz_idle_exit();
	__current_clr_polling();

	/*
	 * We promise to call sched_ttwu_pending() and reschedule if
	 * need_resched() is set while polling is set. That means that clearing
	 * polling needs to be visible before doing these things.
	 */
	/*
	 * 英文承诺：polling=1 时到来的唤醒即使省 IPI，本 CPU 也会处理 ttwu pending
	 * 并重调度。先清 polling，再用该原子后屏障让清位对 waker 可见；此后新的
	 * waker 会发送 IPI。屏障另一侧是 resched_curr() 的 set_nr_and_not_polling
	 * 原子协议，避免双方都以为对方会负责。
	 */
	smp_mb__after_atomic();

	/*
	 * RCU relies on this call to be done outside of an RCU read-side
	 * critical section.
	 */
	/*
	 * 英文说明：RCU 要求此 flush 不在 RCU 读侧临界区内，否则回调/IPI 工作可能
	 * 违反 quiescent-state 与等待规则。flush 同时处理排队的 smp call function。
	 */
	flush_smp_call_function_queue();
	/* 真正进入调度器：选择 runnable task；若仍无工作，idle task 之后再次运行。 */
	schedule_idle();

	/* idle task 到达安全点后接纳等待中的 livepatch 一致性状态。 */
	if (unlikely(klp_patch_pending(current)))
		klp_update_patch_state(current);
}

/*
 * cpu_in_idle() - 判断一条程序计数器地址是否位于 __cpuidle text section。
 *
 * @pc 是输入的内核虚拟指令地址，常由采样/异常路径提供；不要求它对应当前 CPU。
 * 函数只读取链接器边界，不加锁、不睡眠、无副作用。返回 true 表示半开区间
 * [__cpuidle_text_start, __cpuidle_text_end) 内，false 表示区间外；它只能证明
 * PC 落在标注代码，不能单独证明 CPU 已进入具体硬件 C-state。
 */
bool cpu_in_idle(unsigned long pc)
{
	return pc >= (unsigned long)__cpuidle_text_start &&
		pc < (unsigned long)__cpuidle_text_end;
}

/*
 * struct idle_timer - 精确 idle 注入的一次栈上计时会话。
 *
 * 由 play_idle_precise() 在栈上创建并销毁；硬 hrtimer 回调在同一对象内设置完成
 * 标志。@timer 由 hrtimer 核心持有到回调结束；@done 是调用者轮询的 0/1 状态，
 * 用 READ_ONCE/WRITE_ONCE 在回调与 idle 循环间传递。对象没有独立引用计数，
 * 因此函数必须等回调完成后才能离开栈帧。
 */
struct idle_timer {
	/* 固定在当前 CPU、硬中断上下文触发的相对 CLOCK_MONOTONIC 定时器。 */
	struct hrtimer timer;
	/* 0 表示继续注入 idle，1 表示期限到达；只作同步标志，不计次数。 */
	int done;
};

/*
 * idle_inject_timer_fn() - 结束一次精确 idle 注入并请求当前 CPU 重调度。
 *
 * @timer 是输入借用指针，必须嵌在仍存活的 struct idle_timer 中；container_of
 * 恢复拥有它的栈对象，不取得引用。回调运行在 pinned hard hrtimer 中断上下文，
 * 不得睡眠。先 WRITE_ONCE(done=1)，再设置 current 的 need_resched，使 do_idle()
 * 离开；返回 HRTIMER_NORESTART 表示一次性 timer 不重启。current 在 pinned timer
 * 上就是执行注入的本 CPU kthread，前提由 play_idle_precise() 的 CPU 绑定保证。
 */
static enum hrtimer_restart idle_inject_timer_fn(struct hrtimer *timer)
{
	struct idle_timer *it = container_of(timer, struct idle_timer, timer);

	WRITE_ONCE(it->done, 1);
	set_tsk_need_resched(current);

	return HRTIMER_NORESTART;
}

/*
 * play_idle_precise() - 让受约束的 per-CPU kthread 注入一段可控 idle 时间。
 *
 * 主要调用者是 idle injection/testing 路径；本函数临时把 current 标为 PF_IDLE，
 * 反复调用 do_idle()，由 pinned hard hrtimer 在 @duration_ns 后结束。
 * @duration_ns: 输入持续时间，单位纳秒，必须非 0。
 * @latency_ns: 输入最大退出延迟，单位纳秒；0 解除强制 deepest-state 限制，非零
 *     通过 cpuidle_use_deepest_state() 限制可选状态。
 *
 * 入口必须是绑定单 CPU、不可改亲和性、无用户 mm 的 SCHED_FIFO kthread；调用者
 * 不持 RCU 读锁。函数会让 CPU 低功耗停顿并执行 schedule_idle()，但不是普通
 * 阻塞等待。返回无直接值；恢复 deepest-state 限制和 PF_IDLE，折叠重调度状态并
 * 重新启用抢占。局部 timer 只在栈帧内有效，hard 回调完成后循环才会退出。
 */
void play_idle_precise(u64 duration_ns, u64 latency_ns)
{
	/* it 是本次调用独占的栈对象；timer 与 done 的生命周期完全包含于函数内。 */
	struct idle_timer it;

	/*
	 * Only FIFO tasks can disable the tick since they don't need the forced
	 * preemption.
	 */
	/*
	 * 英文说明：只有 FIFO 任务不依赖周期 tick 的强制时间片抢占，才允许本路径
	 * 停 tick。其余 WARN 共同验证 pinned per-CPU kernel-thread 身份和有效时长；
	 * 告警不替代调用契约，违约调用仍可能产生不正确调度行为。
	 */
	WARN_ON_ONCE(current->policy != SCHED_FIFO);
	WARN_ON_ONCE(current->nr_cpus_allowed != 1);
	WARN_ON_ONCE(!(current->flags & PF_KTHREAD));
	WARN_ON_ONCE(!(current->flags & PF_NO_SETAFFINITY));
	WARN_ON_ONCE(!duration_ns);
	WARN_ON_ONCE(current->mm);

	/* 阶段 1：确认不在 RCU 读侧，随后禁止迁移/抢占并临时获得 idle 任务语义。 */
	rcu_sleep_check();
	preempt_disable();
	current->flags |= PF_IDLE;
	cpuidle_use_deepest_state(latency_ns);

	/* 阶段 2：构造并启动当前 CPU pinned 的一次性 hard timer。 */
	it.done = 0;
	hrtimer_setup_on_stack(&it.timer, idle_inject_timer_fn, CLOCK_MONOTONIC,
			       HRTIMER_MODE_REL_HARD);
	hrtimer_start(&it.timer, ns_to_ktime(duration_ns),
		      HRTIMER_MODE_REL_PINNED_HARD);

	/* 每次 do_idle() 可能因其他事件短暂返回；只有期限回调置 done 才结束注入。 */
	while (!READ_ONCE(it.done))
		do_idle();

	/* 阶段 3：回调已经发生，撤销设备限制和 PF_IDLE，再恢复调用者抢占状态。 */
	cpuidle_use_deepest_state(0);
	current->flags &= ~PF_IDLE;

	preempt_fold_need_resched();
	preempt_enable();
}
EXPORT_SYMBOL_GPL(play_idle_precise);

/*
 * cpu_startup_entry() - 把刚启动/online 的 CPU 永久带入 idle 调度循环。
 *
 * @state 是输入的 CPU hotplug 起始状态，传给 cpuhp_online_idle() 完成 idle-context
 * 上线阶段。入口 current 是该 CPU 的 boot idle task；函数不持普通锁且绝不返回。
 * 它设置 PF_IDLE，调用一次可由架构覆盖的 prepare，再完成 hotplug 状态并永久
 * 重复 do_idle()。do_idle() 内部在有 runnable task 时 schedule_idle()，任务结束
 * 后控制权回到 idle task 的下一轮。无返回值和 ownership 转移。
 */
void cpu_startup_entry(enum cpuhp_state state)
{
	/* PF_IDLE 必须在任何调度/RCU idle 判断前发布。 */
	current->flags |= PF_IDLE;
	arch_cpu_idle_prepare();
	cpuhp_online_idle(state);
	while (1)
		do_idle();
}

/*
 * idle-task scheduling class.
 */
/*
 * 以下回调把每 CPU idle task 接入调度类框架。idle class 是优先级最低的兜底类：
 * 它没有普通 enqueue/yield 生命周期，也不允许迁移或阻塞；rq 没有更高类任务时
 * pick_task_idle() 直接返回 rq->idle。回调入口通常已持目标 rq 锁且 IRQ 状态由
 * 调度核心控制，任何可能睡眠的操作都非法。
 */

/*
 * select_task_rq_idle() - 保持 idle task 固定在其当前 CPU。
 *
 * @p 是借用的 idle task；@cpu 是通用调度器给出的候选 CPU；@flags 是唤醒/迁移
 * 原因位图。本类不迁移 idle task，故忽略后两者并返回 task_cpu(p)。入口在调度
 * 热路径，不睡眠、不改状态、不取得引用；返回值是逻辑 CPU 编号。
 */
static int
select_task_rq_idle(struct task_struct *p, int cpu, int flags)
{
	return task_cpu(p); /* IDLE tasks as never migrated */
	/* 上述行尾英文说明：idle task 从不迁移，其身份与 per-CPU rq 永久绑定。 */
}

/*
 * balance_idle() - 拒绝对 idle class 执行普通 balance 回调。
 *
 * @rq 是已锁的借用 runqueue，@rf 是对应锁状态的借用记录；本函数不消费两者。
 * 调度核心正常情况下不应为兜底 idle class 请求 balance，因此无条件 WARN 并
 * 返回非零诊断值。不可睡眠，无其他副作用；告警用于暴露调用链不变量被破坏。
 */
static int
balance_idle(struct rq *rq, struct rq_flags *rf)
{
	return WARN_ON_ONCE(1);
}

/*
 * Idle tasks are unconditionally rescheduled:
 */
/*
 * 英文说明：任何真实任务唤醒都应无条件抢占 idle task；无需比较优先级、deadline
 * 或 vruntime，因为 idle 只在系统无其他 runnable 工作时合法运行。
 */
/*
 * wakeup_preempt_idle() - 为正在运行 idle 的 rq 发布立即重调度请求。
 *
 * @rq 是已锁借用 runqueue；@p 是刚唤醒任务的借用指针；@flags 为唤醒提示。
 * 本实现只需 resched_curr(rq)，不读取 @p/@flags。函数不可睡眠、无返回、不取得
 * ownership；本地 rq 设置 need_resched，远端 rq 按 polling 协议决定是否发 IPI。
 */
static void wakeup_preempt_idle(struct rq *rq, struct task_struct *p, int flags)
{
	resched_curr(rq);
}

/* 前向声明：定义见下方；@rq 已锁，更新 idle 运行时间及 deadline server 记账。 */
static void update_curr_idle(struct rq *rq);

/*
 * put_prev_task_idle() - idle task 被换下 CPU 前完成本轮记账和 idle 状态发布。
 *
 * @rq 是已锁借用 runqueue；@prev 是将离开的 idle task；@next 是接替任务，二者
 * 均由调度核心持有且不可为空。函数不睡眠、无返回、不改变引用 ownership。
 * 先把 rq 时钟差额计入 idle/deadline server，再通知 sched_ext 本 CPU 非 idle，
 * 最后更新 rq 平均 idle 统计；返回后调度核心继续 context switch。
 */
static void put_prev_task_idle(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	update_curr_idle(rq);
	scx_update_idle(rq, false, true);
	update_rq_avg_idle(rq);
}

/*
 * set_next_task_idle() - idle task 被选为 next 时发布 runqueue idle 状态并开始计时。
 *
 * @rq 是已锁借用 runqueue；@next 必须是 rq->idle；@first 表示调度核心的首次设置
 * 情形，本实现无需区分。不可睡眠、无返回、不取得引用。它更新 idle-core 和
 * sched_ext 可见状态、go-idle 统计，把 next->se.exec_start 设为当前 rq task clock，
 * 并同步 PELT 丢失 idle 时间，为 update_curr_idle() 建立计时起点。
 */
static void set_next_task_idle(struct rq *rq, struct task_struct *next, bool first)
{
	update_idle_core(rq);
	scx_update_idle(rq, true, true);
	schedstat_inc(rq->sched_goidle);
	next->se.exec_start = rq_clock_task(rq);

	/*
	 * rq is about to be idle, check if we need to update the
	 * lost_idle_time of clock_pelt
	 */
	/*
	 * 英文说明：rq 即将 idle，PELT 时钟可能因 tick/nohz 停止而丢失时间；此处在
	 * 发布边界更新 lost_idle_time，避免后续负载衰减把停 tick 时段解释错误。
	 */
	update_idle_rq_clock_pelt(rq);
}

/*
 * pick_task_idle() - 返回目标 runqueue 永久拥有的兜底 idle task。
 *
 * @rq 是已锁借用 runqueue；@rf 是通用选择协议的借用锁状态，本实现不修改。
 * 调度器只在更高优先级 class 均无可运行任务时调用。函数不可睡眠；先向 sched_ext
 * 发布 CPU idle（非切换最终态），再返回借用的 rq->idle，不增加 task 引用。
 */
struct task_struct *pick_task_idle(struct rq *rq, struct rq_flags *rf)
{
	scx_update_idle(rq, true, false);
	return rq->idle;
}

/*
 * It is not legal to sleep in the idle task - print a warning
 * message if some code attempts to do it:
 */
/*
 * 英文说明：idle task 代表“无工作”，不允许像普通任务一样阻塞并从 rq dequeue；
 * 若代码尝试这样做，说明在 idle 上下文调用了会睡眠的路径，可能让 CPU 没有兜底
 * task。这里用显式告警和栈回溯暴露错误，而不是静默维护非法队列状态。
 */
/*
 * dequeue_task_idle() - 诊断非法的 idle-task dequeue 请求。
 *
 * @rq 为入口已锁且 IRQ 关闭的借用 runqueue；@p 是被错误 dequeue 的 idle task；
 * @flags 为通用 dequeue 原因位图。为避免持 rq raw lock 打印造成锁问题，本函数
 * 暂时解锁、输出错误和栈，再原样加锁。不可睡眠语义仍由调度上下文约束；返回
 * true 让调用框架继续其诊断路径，但不真正摘除 idle task，ownership 不变。
 */
static bool
dequeue_task_idle(struct rq *rq, struct task_struct *p, int flags)
{
	raw_spin_rq_unlock_irq(rq);
	printk(KERN_ERR "bad: scheduling from the idle thread!\n");
	dump_stack();
	raw_spin_rq_lock_irq(rq);
	return true;
}

/*
 * scheduler tick hitting a task of our scheduling class.
 *
 * NOTE: This function can be called remotely by the tick offload that
 * goes along full dynticks. Therefore no local assumption can be made
 * and everything must be accessed through the @rq and @curr passed in
 * parameters.
 */
/*
 * 英文说明：full-dynticks 的 tick offload 可从远端 CPU 调用本回调，所以不能用
 * current、this_rq() 或本地 CPU 状态推断被记账对象；所有状态必须经传入的
 * @rq/@curr 访问。这是远端调用正确性的边界，不只是编码风格。
 */
/*
 * task_tick_idle() - 处理 idle class 的一次调度 tick 记账。
 *
 * @rq 是目标 CPU 的已锁借用 runqueue；@curr 是该 rq 当前 idle task；@queued
 * 是通用 tick 队列提示，本类无需使用。函数可能由本地或远端 tick-offload
 * 上下文调用，不睡眠、无返回；只经 update_curr_idle() 结算运行时间。
 */
static void task_tick_idle(struct rq *rq, struct task_struct *curr, int queued)
{
	update_curr_idle(rq);
}

/*
 * switching_to_idle() - 拒绝把普通 task 动态切换到 idle scheduling class。
 *
 * @rq 是已锁借用 runqueue，@p 是被请求切换的借用 task。per-CPU idle task 在
 * 启动期专门创建，用户/普通内核任务不得进入此类；任何调用都是内核错误，故
 * BUG() 不返回。函数不能睡眠，也没有成功出口或 ownership 转移。
 */
static void switching_to_idle(struct rq *rq, struct task_struct *p)
{
	BUG();
}

/*
 * prio_changed_idle() - 验证 idle task 的优先级从未发生实质变化。
 *
 * @rq 为已锁借用 runqueue；@p 为 idle task；@oldprio 是修改前的调度优先级值。
 * 相等时无副作用返回；不同表示破坏了专用 idle task 不变量，BUG() 不返回。
 * 调度热路径不可睡眠，函数不取得任何引用。
 */
static void
prio_changed_idle(struct rq *rq, struct task_struct *p, u64 oldprio)
{
	if (p->prio == oldprio)
		return;

	BUG();
}

/*
 * update_curr_idle() - 结算 rq->idle 自上次 exec_start 以来的运行时间。
 *
 * @rq 是入口已锁的借用 runqueue；函数读取 rq task clock，更新 idle sched_entity
 * 的 exec_start，并把正的纳秒级 delta_exec 交给 fair/ext deadline server 的
 * idle 更新。不可睡眠、无返回、不改变 task ownership。时钟未前进或倒退时直接
 * 返回，避免把非正差额写入带宽/运行时记账。
 */
static void update_curr_idle(struct rq *rq)
{
	/* se 借用 rq 永久 idle task 的调度实体；now/delta_exec 单位均为纳秒。 */
	struct sched_entity *se = &rq->idle->se;
	u64 now = rq_clock_task(rq);
	s64 delta_exec;

	/* 阶段 1：只结算严格为正的 rq-clock 区间。 */
	delta_exec = now - se->exec_start;
	if (unlikely(delta_exec <= 0))
		return;

	/* 阶段 2：先推进时间基线，再把本段 idle 时间告知启用的 deadline servers。 */
	se->exec_start = now;

	dl_server_update_idle(&rq->fair_server, delta_exec);
#ifdef CONFIG_SCHED_CLASS_EXT
	dl_server_update_idle(&rq->ext_server, delta_exec);
#endif
}

/*
 * Simple, special scheduling class for the per-CPU idle tasks:
 */
/*
 * 英文说明：该调度类只服务每 CPU 专用 idle task，是所有 class 之后的简单兜底。
 * 静态操作表由调度核心长期借用，无动态销毁；缺失 enqueue/yield 回调本身就是
 * “idle task 永不进入普通队列”的契约。各回调均在 rq 锁/调度器上下文中运行。
 */
DEFINE_SCHED_CLASS(idle) = {
	/* no enqueue/yield_task for idle tasks */
	/* idle task 永久属于 rq，不通过普通 enqueue 或主动 yield 改变成员关系。 */

	/* dequeue is not valid, we print a debug message there: */
	/* 非法 dequeue 仍接入诊断回调，以便输出错误而不是空指针调用。 */
	.dequeue_task		= dequeue_task_idle,

	.wakeup_preempt		= wakeup_preempt_idle,

	.pick_task		= pick_task_idle,
	.put_prev_task		= put_prev_task_idle,
	.set_next_task          = set_next_task_idle,

	.balance		= balance_idle,
	.select_task_rq		= select_task_rq_idle,
	.set_cpus_allowed	= set_cpus_allowed_common,

	.task_tick		= task_tick_idle,

	.prio_changed		= prio_changed_idle,
	.switching_to		= switching_to_idle,
	.update_curr		= update_curr_idle,
};
