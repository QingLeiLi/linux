// SPDX-License-Identifier: GPL-2.0-only
/*
 * Context Tracking 状态机实现学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件在 kernel/user/guest/idle/IRQ/NMI 的高层边界维护每 CPU 状态，
 * 让 RCU 知道某 CPU 何时不可能执行普通读侧临界区，从而在 full dynticks
 * 场景停止为该 CPU 保留周期 tick；同时为 USER 状态完成 vtime 记账。
 *
 * 主状态机：
 *   内核/IRQ watching
 *      -> ct_kernel_exit() -> IDLE/USER/GUEST EQS
 *      -> ct_nmi_enter() 或 ct_kernel_enter() -> watching
 * USER/GUEST 外层还通过 recursion 防止 tracing/记账重入。state 原子量一次
 * 更新上下文低位与 watching 序列，高层 nesting 字段判断当前转换是最外层
 * 真实边界还是只需记账的嵌套边界。
 *
 * 所有对象都是长期 per-CPU 状态，无动态分配和 ownership 转移。正确性依赖
 * IRQ-off/noinstr 调用约束、WRITE_ONCE 防撕裂、原子 RMW 的全序，以及 NMI
 * 嵌套编码；错误配对可能导致 RCU 停滞或过早结束宽限期。
 */
/*
 * Context tracking: Probe on high level context boundaries such as kernel,
 * userspace, guest or idle.
 *
 * This is used by RCU to remove its dependency on the timer tick while a CPU
 * runs in idle, userspace or guest mode.
 *
 * User/guest tracking started by Frederic Weisbecker:
 *
 * Copyright (C) 2012 Red Hat, Inc., Frederic Weisbecker
 *
 * Many thanks to Gilad Ben-Yossef, Paul McKenney, Ingo Molnar, Andrew Morton,
 * Steven Rostedt, Peter Zijlstra for suggestions and improvements.
 *
 * RCU extended quiescent state bits imported from kernel/rcu/tree.c
 * where the relevant authorship may be found.
 */
/*
 * Context Tracking 在 kernel、userspace、guest、idle 等高层
 * 上下文边界设置探针。RCU 借此在 CPU 运行于 idle、用户态或来宾态时摆脱
 * 对 timer tick 的依赖。用户/来宾跟踪由 Frederic Weisbecker 发起，RCU
 * 扩展静止态位则从 kernel/rcu/tree.c 迁入；版权与致谢保持原样。
 */

#include <linux/context_tracking.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/hardirq.h>
#include <linux/export.h>
#include <linux/kprobes.h>
#include <trace/events/rcu.h>


/*
 * context_tracking - 每 CPU 状态对象的唯一存储定义。
 *
 * CPU 初始处于内核态且 RCU watching；任务 nesting 从 1 开始表示尚未进入
 * idle，nmi_nesting 用大偏移表示 IRQ/NMI 来自非 idle 区域。对象静态存在
 * 到关机，所有调用者只借用其地址。导出符号允许 RCU 等内核组件采样。
 */
DEFINE_PER_CPU(struct context_tracking, context_tracking) = {
#ifdef CONFIG_CONTEXT_TRACKING_IDLE
	.nesting = 1,
	.nmi_nesting = CT_NESTING_IRQ_NONIDLE,
#endif
	.state = ATOMIC_INIT(CT_RCU_WATCHING),
};
EXPORT_SYMBOL_GPL(context_tracking);

#ifdef CONFIG_CONTEXT_TRACKING_IDLE
/*
 * TPS() 把固定字符串登记为 tracepoint string，供状态转换 trace 使用；
 * @x 必须是可长期存在的字符串字面量，不产生资源 ownership。
 */
#define TPS(x)  tracepoint_string(x)

/* Record the current task on exiting RCU-tasks (dyntick-idle entry). */
/*
 * rcu_task_exit() - 进入 dyntick-idle/EQS 时记录当前任务所在 CPU。
 *
 * 原文意为“退出 RCU-tasks 活跃态时记录 current”。仅 TASKS_RCU 与 NO_HZ_FULL
 * 同时启用才写 current->rcu_tasks_idle_cpu；WRITE_ONCE 避免编译器合并。
 * 入参、返回均无，不取得 task 引用，调用者已固定 current/CPU。
 */
static __always_inline void rcu_task_exit(void)
{
#if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL)
	WRITE_ONCE(current->rcu_tasks_idle_cpu, smp_processor_id());
#endif /* #if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL) */
/* 上述写入只服务同时启用 TASKS_RCU 与 NO_HZ_FULL 的配置组合。 */
}

/* Record no current task on entering RCU-tasks (dyntick-idle exit). */
/*
 * rcu_task_enter() - 离开 dyntick-idle 时清除 current 的 idle CPU 记录。
 *
 * 原文意为“重新进入 RCU-tasks 活跃态时记录没有 idle current”。写入 -1
 * 表示任务不再作为该 CPU 的 EQS 见证；无入参、无返回和 ownership 变化。
 */
static __always_inline void rcu_task_enter(void)
{
#if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL)
	WRITE_ONCE(current->rcu_tasks_idle_cpu, -1);
#endif /* #if defined(CONFIG_TASKS_RCU) && defined(CONFIG_NO_HZ_FULL) */
/* 未同时启用 TASKS_RCU/NO_HZ_FULL 时该 helper 编译为空操作。 */
}

/*
 * Record entry into an extended quiescent state.  This is only to be
 * called when not already in an extended quiescent state, that is,
 * RCU is watching prior to the call to this function and is no longer
 * watching upon return.
 */
/*
 * ct_kernel_exit_state() - 原子发布“从 watching 进入 EQS”的最底层边界。
 *
 * 原文要求入口尚未处于 EQS：调用前 RCU 正在观察，返回后不再观察。
 * @offset: 纯输入组合增量，包含 CT_RCU_WATCHING 和目标 ctx_state。
 * 调用者必须关中断并已结束本次 busy period 的 RCU 读侧工作。
 * 返回无直接结果；有序 RMW 是远端 RCU 的发布边界，无 ownership 变化。
 */
static noinstr void ct_kernel_exit_state(int offset)
{
	/*
	 * CPUs seeing atomic_add_return() must see prior RCU read-side
	 * critical sections, and we also must force ordering with the
	 * next idle sojourn.
	 */
	/*
	 * 观察到原子更新的 CPU 必须同时看到此前的
	 * RCU 读侧临界区，
	 * 且本次更新还要与下一段 idle 停留排序，避免远端过早判定静止。
	 */
	// RCU is still watching.  Better not be in extended quiescent state!
	/* 此处 RCU 仍应 watching；调试配置下拒绝嵌套进入 EQS。 */
	WARN_ON_ONCE(IS_ENABLED(CONFIG_RCU_EQS_DEBUG) && !rcu_is_watching_curr_cpu());
	/* 唯一发布点：原子推进 watching 序列并写入目标上下文。 */
	(void)ct_state_inc(offset);
	// RCU is no longer watching.
	/* 从这里起协议禁止普通 RCU 读侧工作。 */
}

/*
 * Record exit from an extended quiescent state.  This is only to be
 * called from an extended quiescent state, that is, RCU is not watching
 * prior to the call to this function and is watching upon return.
 */
/*
 * ct_kernel_enter_state() - 原子发布“从 EQS 恢复 watching”的最底层边界。
 *
 * 原文要求入口已在 EQS，返回时 RCU 已重新观察 CPU。
 * @offset: 纯输入组合增量，通常为 CT_RCU_WATCHING 减旧 ctx_state。
 * 返回无直接结果；有序 RMW 后调用者才可进入 RCU 读侧临界区。
 */
static noinstr void ct_kernel_enter_state(int offset)
{
	/* seq 是更新后的完整 state，只用于验证 watching 奇偶位。 */
	int seq;

	/*
	 * CPUs seeing atomic_add_return() must see prior idle sojourns,
	 * and we also must force ordering with the next RCU read-side
	 * critical section.
	 */
	/*
	 * 远端看到本次更新时必须看到此前 idle 停留；
	 * 本次更新也必须先于下一段 RCU 读侧临界区，
	 * 防止新读者在“仍 idle”的快照下运行。
	 */
	seq = ct_state_inc(offset);
	// RCU is now watching.  Better not be in an extended quiescent state!
	/* 更新后 watching 位必须为真，否则增量或嵌套协议已经错配。 */
	WARN_ON_ONCE(IS_ENABLED(CONFIG_RCU_EQS_DEBUG) && !(seq & CT_RCU_WATCHING));
}

/*
 * Enter an RCU extended quiescent state, which can be either the
 * idle loop or adaptive-tickless usermode execution.
 *
 * We crowbar the ->nmi_nesting field to zero to allow for
 * the possibility of usermode upcalls having messed up our count
 * of interrupt nesting level during the prior busy period.
 */
/*
 * ct_kernel_exit() - 处理任务嵌套并在最外层真正进入 RCU EQS。
 *
 *  EQS 可来自 idle loop 或 adaptive-tickless 用户态；进入前把
 * nmi_nesting 强制归零，以容忍上一 busy period 的用户 upcall 扰乱计数。
 * @user: true 表示 USER/GUEST，false 表示 idle，仅影响调试约束。
 * @offset: 纯输入 state 增量，编码 watching 翻转与目标上下文。
 * 前置条件：本地中断关闭、当前 CPU 固定、不可睡眠。返回无直接结果；
 * nesting 从 1 到 0 才发布 EQS，其他层只做嵌套记账。
 */
static void noinstr ct_kernel_exit(bool user, int offset)
{
	/* ct 是当前 CPU 长期状态的借用指针。 */
	struct context_tracking *ct = this_cpu_ptr(&context_tracking);

	/* 阶段 1：规范两套 nesting，并判断这是否是最外层退出。 */
	WARN_ON_ONCE(ct_nmi_nesting() != CT_NESTING_IRQ_NONIDLE);
	WRITE_ONCE(ct->nmi_nesting, 0);
	WARN_ON_ONCE(IS_ENABLED(CONFIG_RCU_EQS_DEBUG) &&
		     ct_nesting() == 0);
	if (ct_nesting() != 1) {
		// RCU will still be watching, so just do accounting and leave.
		/* 原文：非最外层时 RCU 仍 watching，只递减 nesting 后返回。 */
		ct->nesting--;
		return;
	}

	/*
	 * 阶段 2：趁 RCU 仍 watching，完成 trace、调试检查和延迟 QS 报告。
	 * instrumentation 区间只包围允许插桩的操作，核心翻转保持 noinstr。
	 */
	instrumentation_begin();
	lockdep_assert_irqs_disabled();
	trace_rcu_watching(TPS("End"), ct_nesting(), 0, ct_rcu_watching());
	WARN_ON_ONCE(IS_ENABLED(CONFIG_RCU_EQS_DEBUG) && !user && !is_idle_task(current));
	rcu_preempt_deferred_qs(current);

	// instrumentation for the noinstr ct_kernel_exit_state()
	/* 向插桩工具描述随后不可插桩 helper 对 state 的原子写。 */
	instrument_atomic_write(&ct->state, sizeof(ct->state));

	instrumentation_end();
	WRITE_ONCE(ct->nesting, 0); /* Avoid irq-access tearing. */
	/* 原行尾说明：WRITE_ONCE 防止 IRQ 读取撕裂的 nesting 存储。 */
	// RCU is watching here ...
	/* 原文：原子 helper 调用前 RCU 仍在观察。 */
	ct_kernel_exit_state(offset);
	// ... but is no longer watching here.
	/* 原文：发布后已进入 EQS，只可执行协议允许的尾部记录。 */
	rcu_task_exit();
}

/*
 * Exit an RCU extended quiescent state, which can be either the
 * idle loop or adaptive-tickless usermode execution.
 *
 * We crowbar the ->nmi_nesting field to CT_NESTING_IRQ_NONIDLE to
 * allow for the possibility of usermode upcalls messing up our count of
 * interrupt nesting level during the busy period that is just now starting.
 */
/*
 * ct_kernel_enter() - 处理嵌套并在最外层从 EQS 恢复 RCU watching。
 *
 * 退出 EQS 时把 nmi_nesting 重置为大偏移，以容忍此前用户 upcall
 * 造成的计数偏差。@user/@offset 与 ct_kernel_exit() 对称；入口应关中断
 * 且尚不可使用 RCU，最外层返回后才可执行读侧代码。
 * 无 ownership 或错误码。
 */
static void noinstr ct_kernel_enter(bool user, int offset)
{
	/* ct 为本 CPU 借用状态；oldval 保存入口 nesting 快照。 */
	struct context_tracking *ct = this_cpu_ptr(&context_tracking);
	long oldval;

	WARN_ON_ONCE(IS_ENABLED(CONFIG_RCU_EQS_DEBUG) && !raw_irqs_disabled());
	oldval = ct_nesting();
	WARN_ON_ONCE(IS_ENABLED(CONFIG_RCU_EQS_DEBUG) && oldval < 0);
	if (oldval) {
		// RCU was already watching, so just do accounting and leave.
		/*
		 * 原文：嵌套进入时已 watching，只递增 nesting，
		 * 不重复翻转 state。
		 */
		ct->nesting++;
		return;
	}
	/* 最外层退出 EQS：先恢复 RCU-tasks 标记，再发布 watching。 */
	rcu_task_enter();
	// RCU is not watching here ...
	/* 原文：原子 helper 之前仍处于 EQS。 */
	ct_kernel_enter_state(offset);
	// ... but is watching here.
	/* 原文：从这里起 RCU 已 watching，可进入受控插桩阶段。 */
	instrumentation_begin();

	// instrumentation for the noinstr ct_kernel_enter_state()
	/* 向插桩工具补记刚才不可插桩 helper 的 state 原子写。 */
	instrument_atomic_write(&ct->state, sizeof(ct->state));

	/* 阶段 2：发布 trace，并恢复 busy-period 的两套 nesting 基准值。 */
	trace_rcu_watching(TPS("Start"), ct_nesting(), 1, ct_rcu_watching());
	WARN_ON_ONCE(IS_ENABLED(CONFIG_RCU_EQS_DEBUG) && !user && !is_idle_task(current));
	WRITE_ONCE(ct->nesting, 1);
	WARN_ON_ONCE(ct_nmi_nesting());
	WRITE_ONCE(ct->nmi_nesting, CT_NESTING_IRQ_NONIDLE);
	instrumentation_end();
}

/**
 * ct_nmi_exit - inform RCU of exit from NMI context
 *
 * If we are returning from the outermost NMI handler that interrupted an
 * RCU-idle period, update ct->state and ct->nmi_nesting
 * to let the RCU grace-period handling know that the CPU is back to
 * being RCU-idle.
 *
 * If you add or remove a call to ct_nmi_exit(), be sure to test
 * with CONFIG_RCU_EQS_DEBUG=y.
 */
/*
 * ct_nmi_exit() - 在退出 NMI/IRQ 最外层时恢复被打断 CPU 的 RCU-idle。
 *
 * 若正在退出的是打断 RCU-idle 的最外层 NMI，就更新 state 与
 * nmi_nesting，通知宽限期逻辑 CPU 再次 idle；增删调用点必须用
 * CONFIG_RCU_EQS_DEBUG=y 测试。
 * 入参：无。入口位于 noinstr、CPU 固定的 NMI/IRQ 尾部，不可睡眠。
 * 返回无直接结果；嵌套层只减计数，只有值 1 才翻转回 EQS。
 */
void noinstr ct_nmi_exit(void)
{
	/* ct 是本 CPU 长期状态的借用指针。 */
	struct context_tracking *ct = this_cpu_ptr(&context_tracking);

	/* 阶段 1：在可插桩窗口验证 NMI 深度和 watching 前置条件。 */
	instrumentation_begin();
	/*
	 * Check for ->nmi_nesting underflow and bad CT state.
	 * (We are exiting an NMI handler, so RCU better be paying attention
	 * to us!)
	 */
	/*
	 * 原文要求检查 nmi_nesting 下溢和错误状态：既然仍在 NMI handler，
	 * RCU 必须正在观察当前 CPU，否则 handler 的读侧访问可能不受保护。
	 */
	WARN_ON_ONCE(ct_nmi_nesting() <= 0);
	WARN_ON_ONCE(!rcu_is_watching_curr_cpu());

	/*
	 * If the nesting level is not 1, the CPU wasn't RCU-idle, so
	 * leave it in non-RCU-idle state.
	 */
	/*
	 * 值不等于 1 表示该 NMI 没有打断 RCU-idle，
	 * 或仍有外层嵌套；
	 * 因此只减去本层编码 2，保持 CPU 为非 idle/watching。
	 */
	if (ct_nmi_nesting() != 1) {
		trace_rcu_watching(TPS("--="), ct_nmi_nesting(), ct_nmi_nesting() - 2,
				  ct_rcu_watching());
		WRITE_ONCE(ct->nmi_nesting, /* No store tearing. */
			   ct_nmi_nesting() - 2);
		/* 原行尾说明 WRITE_ONCE 防止并发观察到撕裂存储。 */
		instrumentation_end();
		return;
	}

	/* This NMI interrupted an RCU-idle CPU, restore RCU-idleness. */
	/*
	 * 值 1 精确标识“最外层 NMI 打断了 RCU-idle”，现在把嵌套
	 * 归零并在插桩窗口结束后重新发布 EQS。
	 */
	trace_rcu_watching(TPS("Endirq"), ct_nmi_nesting(), 0, ct_rcu_watching());
	WRITE_ONCE(ct->nmi_nesting, 0); /* Avoid store tearing. */
	/* 原行尾说明归零也必须防止 IRQ 侧观察到撕裂。 */

	// instrumentation for the noinstr ct_kernel_exit_state()
	/* 向工具声明随后不可插桩的 state 原子写。 */
	instrument_atomic_write(&ct->state, sizeof(ct->state));
	instrumentation_end();

	// RCU is watching here ...
	/* 原文：翻转前仍 watching。 */
	ct_kernel_exit_state(CT_RCU_WATCHING);
	// ... but is no longer watching here.
	/* 原文：翻转后恢复 EQS。 */

	/*
	 * 普通 IRQ 复用 NMI 记账时还需恢复 RCU-tasks 标记；真正嵌套 NMI 不改变
	 * task 运行身份，因此跳过。
	 */
	if (!in_nmi())
		rcu_task_exit();
}

/**
 * ct_nmi_enter - inform RCU of entry to NMI context
 *
 * If the CPU was idle from RCU's viewpoint, update ct->state and
 * ct->nmi_nesting to let the RCU grace-period handling know
 * that the CPU is active.  This implementation permits nested NMIs, as
 * long as the nesting level does not overflow an int.  (You will probably
 * run out of stack space first.)
 *
 * If you add or remove a call to ct_nmi_enter(), be sure to test
 * with CONFIG_RCU_EQS_DEBUG=y.
 */
/*
 * ct_nmi_enter() - NMI/IRQ 进入时确保 RCU watching 并编码嵌套来源。
 *
 * 若 CPU 从 RCU 视角 idle，则更新 state/nmi_nesting 使宽限期逻辑
 * 知道它已活跃；支持嵌套 NMI，实际先耗尽栈才可能溢出 long。
 * 增删调用点要用
 * CONFIG_RCU_EQS_DEBUG 验证。
 * 入参：无。noinstr、固定 CPU、不可睡眠。返回无直接结果；从 EQS 进入时
 * nesting 加 1，原本 watching 或嵌套时加 2，由 exit 据此恢复正确状态。
 */
void noinstr ct_nmi_enter(void)
{
	/*
	 * incby 是本次嵌套编码：默认 2 表示原本非 idle；
	 * ct 是 per-CPU 借用状态。
	 */
	long incby = 2;
	struct context_tracking *ct = this_cpu_ptr(&context_tracking);

	/* Complain about underflow. */
	/* 原文：若入口前嵌套值已小于零，立即报告下溢协议错误。 */
	WARN_ON_ONCE(ct_nmi_nesting() < 0);

	/*
	 * If idle from RCU viewpoint, atomically increment CT state
	 * to mark non-idle and increment ->nmi_nesting by one.
	 * Otherwise, increment ->nmi_nesting by two.  This means
	 * if ->nmi_nesting is equal to one, we are guaranteed
	 * to be in the outermost NMI handler that interrupted an RCU-idle
	 * period (observation due to Andy Lutomirski).
	 */
	/*
	 * 编码不变量：从 RCU-idle 进入时原子改为 watching，
	 * 且深度加 1；其他入口深度加 2。因此值恰好为 1
	 * 只可能是打断 idle 的最外层 handler，
	 * ct_nmi_exit() 可据此决定是否恢复 EQS，而不需要额外布尔字段。
	 */
	if (!rcu_is_watching_curr_cpu()) {

		/*
		 * 普通 IRQ 离开 task-idle 语义；
		 * 真正 NMI 不改 current 的 tasks-RCU 标记。
		 */
		if (!in_nmi())
			rcu_task_enter();

		// RCU is not watching here ...
		/* 原文：原子转换前仍处于 EQS。 */
		ct_kernel_enter_state(CT_RCU_WATCHING);
		// ... but is watching here.
		/* 原文：转换后 handler 可合法使用 RCU。 */

		instrumentation_begin();
		// instrumentation for the noinstr rcu_is_watching_curr_cpu()
		/* 补记不可插桩的 state 原子读取。 */
		instrument_atomic_read(&ct->state, sizeof(ct->state));
		// instrumentation for the noinstr ct_kernel_enter_state()
		/* 补记不可插桩的 state 原子写入。 */
		instrument_atomic_write(&ct->state, sizeof(ct->state));

		/* 记为 1，让退出路径知道需要恢复 RCU-idle。 */
		incby = 1;
	} else if (!in_nmi()) {
		/* watching 状态下的普通 IRQ 还需让 RCU 检查是否要恢复 tick。 */
		instrumentation_begin();
		rcu_irq_enter_check_tick();
	} else  {
		instrumentation_begin();
	}

	trace_rcu_watching(incby == 1 ? TPS("Startirq") : TPS("++="),
			  ct_nmi_nesting(),
			  ct_nmi_nesting() + incby, ct_rcu_watching());
	instrumentation_end();
	WRITE_ONCE(ct->nmi_nesting, /* Prevent store tearing. */
		   ct_nmi_nesting() + incby);
	/* 原行尾说明：嵌套值发布必须防止存储撕裂。 */
	/*
	 * 编译器屏障阻止后续 handler 代码被移动到 nesting 发布之前；
	 * 它不产生
	 * 跨 CPU 硬件屏障，跨 CPU 可见性由 state 的原子转换承担。
	 */
	barrier();
}

/**
 * ct_idle_enter - inform RCU that current CPU is entering idle
 *
 * Enter idle mode, in other words, -leave- the mode in which RCU
 * read-side critical sections can occur.  (Though RCU read-side
 * critical sections can occur in irq handlers in idle, a possibility
 * handled by irq_enter() and irq_exit().)
 *
 * If you add or remove a call to ct_idle_enter(), be sure to test with
 * CONFIG_RCU_EQS_DEBUG=y.
 */
/*
 * ct_idle_enter() - idle loop 进入时发布 RCU 扩展静止态。
 *
 * 原文把 idle 描述为“离开可能发生普通 RCU 读侧临界区的模式”；idle 中的
 * IRQ 仍可通过 irq_enter/exit 暂时恢复 watching。增删调用点必须打开 EQS
 * 调试。入参、返回均无；要求 IRQ 已关闭，不睡眠，最终把 nesting 从 1
 * 降到 0，并以 IDLE 状态翻转 watching。
 */
void noinstr ct_idle_enter(void)
{
	WARN_ON_ONCE(IS_ENABLED(CONFIG_RCU_EQS_DEBUG) && !raw_irqs_disabled());
	ct_kernel_exit(false, CT_RCU_WATCHING + CT_STATE_IDLE);
}
EXPORT_SYMBOL_GPL(ct_idle_enter);

/**
 * ct_idle_exit - inform RCU that current CPU is leaving idle
 *
 * Exit idle mode, in other words, -enter- the mode in which RCU
 * read-side critical sections can occur.
 *
 * If you add or remove a call to ct_idle_exit(), be sure to test with
 * CONFIG_RCU_EQS_DEBUG=y.
 */
/*
 * ct_idle_exit() - 离开 idle 时恢复可执行 RCU 读侧代码的内核状态。
 *
 * 这是重新进入可发生 RCU 读侧临界区的模式，增删调用点要用 EQS
 * 调试测试。入参、返回均无；实现自行保存并关闭本地 IRQ，调用
 * ct_kernel_enter() 后恢复原 IRQ 状态，不转移 ownership。
 */
void noinstr ct_idle_exit(void)
{
	/* flags 保存调用前 IRQ 状态，只在本函数配对恢复。 */
	unsigned long flags;

	raw_local_irq_save(flags);
	ct_kernel_enter(false, CT_RCU_WATCHING - CT_STATE_IDLE);
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(ct_idle_exit);

/**
 * ct_irq_enter - inform RCU that current CPU is entering irq away from idle
 *
 * Enter an interrupt handler, which might possibly result in exiting
 * idle mode, in other words, entering the mode in which read-side critical
 * sections can occur.  The caller must have disabled interrupts.
 *
 * Note that the Linux kernel is fully capable of entering an interrupt
 * handler that it never exits, for example when doing upcalls to user mode!
 * This code assumes that the idle loop never does upcalls to user mode.
 * If your architecture's idle loop does do upcalls to user mode (or does
 * anything else that results in unbalanced calls to the irq_enter() and
 * irq_exit() functions), RCU will give you what you deserve, good and hard.
 * But very infrequently and irreproducibly.
 *
 * Use things like work queues to work around this limitation.
 *
 * You have been warned.
 *
 * If you add or remove a call to ct_irq_enter(), be sure to test with
 * CONFIG_RCU_EQS_DEBUG=y.
 */
/*
 * ct_irq_enter() - IRQ 从 idle/EQS 打断 CPU 时恢复 RCU watching。
 *
 *  handler 可能使 CPU 离开 idle，因此进入可执行读侧临界区的模式；
 * 调用者必须关中断。原文还警告 idle loop 不能通过用户 upcall 等方式造成
 * irq_enter/exit 不配对，否则会产生罕见且难复现的 RCU 错误，
 * 应改用工作队列。
 * 入参、返回均无，实际复用 ct_nmi_enter() 的嵌套编码，不睡眠。
 */
noinstr void ct_irq_enter(void)
{
	lockdep_assert_irqs_disabled();
	ct_nmi_enter();
}

/**
 * ct_irq_exit - inform RCU that current CPU is exiting irq towards idle
 *
 * Exit from an interrupt handler, which might possibly result in entering
 * idle mode, in other words, leaving the mode in which read-side critical
 * sections can occur.  The caller must have disabled interrupts.
 *
 * This code assumes that the idle loop never does anything that might
 * result in unbalanced calls to irq_enter() and irq_exit().  If your
 * architecture's idle loop violates this assumption, RCU will give you what
 * you deserve, good and hard.  But very infrequently and irreproducibly.
 *
 * Use things like work queues to work around this limitation.
 *
 * You have been warned.
 *
 * If you add or remove a call to ct_irq_exit(), be sure to test with
 * CONFIG_RCU_EQS_DEBUG=y.
 */
/*
 * ct_irq_exit() - IRQ 返回 idle 时按嵌套来源恢复 EQS。
 *
 *  handler 退出可能重新进入 idle，即离开普通 RCU 读侧模式；调用者
 * 必须关中断。idle loop 若制造不配对入口/出口会得到
 * 罕见难复现故障，应把
 * 异步工作移交 workqueue。入参、返回均无，委托 ct_nmi_exit() 恢复状态。
 */
noinstr void ct_irq_exit(void)
{
	lockdep_assert_irqs_disabled();
	ct_nmi_exit();
}

/*
 * Wrapper for ct_irq_enter() where interrupts are enabled.
 *
 * If you add or remove a call to ct_irq_enter_irqson(), be sure to test
 * with CONFIG_RCU_EQS_DEBUG=y.
 */
/*
 * ct_irq_enter_irqson() - 为 IRQ 开启的调用点保存 IRQ 状态后进入 CT IRQ。
 *
 * 它只是 ct_irq_enter() 的 IRQ-on 包装，增删调用点需 EQS 调试。
 * 入参、返回均无；flags 只保存本地 IRQ 状态，无 ownership 或失败路径。
 */
void ct_irq_enter_irqson(void)
{
	/* flags 在 local_irq_save/restore 之间有效。 */
	unsigned long flags;

	local_irq_save(flags);
	ct_irq_enter();
	local_irq_restore(flags);
}

/*
 * Wrapper for ct_irq_exit() where interrupts are enabled.
 *
 * If you add or remove a call to ct_irq_exit_irqson(), be sure to test
 * with CONFIG_RCU_EQS_DEBUG=y.
 */
/*
 * ct_irq_exit_irqson() - 为 IRQ 开启的调用点提供配对退出包装。
 *
 * 它包装 ct_irq_exit()，并要求改动调用点后进行 EQS 调试。
 * 入参、返回均无；临时关闭 IRQ 保证 per-CPU 嵌套更新不被同 CPU 打断。
 */
void ct_irq_exit_irqson(void)
{
	/* flags 保存并恢复入口 IRQ 状态。 */
	unsigned long flags;

	local_irq_save(flags);
	ct_irq_exit();
	local_irq_restore(flags);
}
#else
/*
 * CONFIG_CONTEXT_TRACKING_IDLE=n 时 USER 代码仍可调用这两个内部 helper，
 * 但它们无状态、副作用和返回值；@user/@offset 均被忽略。
 */
static __always_inline void ct_kernel_exit(bool user, int offset) { }
static __always_inline void ct_kernel_enter(bool user, int offset) { }
#endif /* #ifdef CONFIG_CONTEXT_TRACKING_IDLE */
/* 上述真实状态机仅在 idle Context Tracking 编译时存在。 */

#ifdef CONFIG_CONTEXT_TRACKING_USER

/*
 * 本翻译单元负责实例化 context_tracking tracepoint；宏必须在 trace 头首次
 * 包含前定义，生成的探针供 USER enter/exit 路径记录边界。
 */
#define CREATE_TRACE_POINTS
#include <trace/events/context_tracking.h>

/*
 * context_tracking_key - 全局 USER 跟踪 jump-label 门控。
 *
 * 初始为 false；ct_cpu_track_user() 每首次激活一个 CPU 就增加引用计数。
 * 热路径只读，不拥有该对象；RO 变体限制任意写入，状态持续到关机。
 */
DEFINE_STATIC_KEY_FALSE_RO(context_tracking_key);
EXPORT_SYMBOL_GPL(context_tracking_key);

/*
 * context_tracking_recursion_enter() - 领取本 CPU 状态转换的最外层执行权。
 *
 * 入参：无。调用点已固定 CPU；返回 true 仅表示 recursion 从 0 变 1，
 * 当前调用负责真正转换。嵌套调用回滚自己的增量并返回 false，
 * 避免 trace、
 * vtime 或告警递归再次进入状态机。无 ownership、不可睡眠。
 */
static noinstr bool context_tracking_recursion_enter(void)
{
	/* recursion 是递增后的本 CPU 深度快照。 */
	int recursion;

	/* __this_cpu 操作依赖调用者已固定 CPU，不提供跨 CPU 同步。 */
	recursion = __this_cpu_inc_return(context_tracking.recursion);
	if (recursion == 1)
		return true;

	WARN_ONCE((recursion < 1), "Invalid context tracking recursion value %d\n", recursion);
	__this_cpu_dec(context_tracking.recursion);

	return false;
}

/*
 * context_tracking_recursion_exit() - 归还最外层状态转换执行权。
 *
 * 入参、返回均无；必须与成功返回 true 的 recursion_enter() 配对，
 * 只递减本 CPU 深度，不睡眠。遗漏会让后续所有转换被误判为递归。
 */
static __always_inline void context_tracking_recursion_exit(void)
{
	__this_cpu_dec(context_tracking.recursion);
}

/**
 * __ct_user_enter - Inform the context tracking that the CPU is going
 *		     to enter user or guest space mode.
 *
 * @state: userspace context-tracking state to enter.
 *
 * This function must be called right before we switch from the kernel
 * to user or guest space, when it's guaranteed the remaining kernel
 * instructions to execute won't use any RCU read side critical section
 * because this function sets RCU in extended quiescent state.
 */
/*
 * __ct_user_enter() - 在最后一段内核出口把 CPU 发布为 USER 或 GUEST。
 *
 * 原文要求它紧邻从内核切换到用户/来宾空间的位置调用；
 * 剩余指令必须保证
 * 不再使用 RCU 读侧临界区，因为本函数会把 RCU 置于扩展静止态。
 * @state: 纯输入，只能是 CT_STATE_USER 或 CT_STATE_GUEST，无 ownership。
 * 前置条件：本地 IRQ 关闭、CPU 固定、noinstr、current 为用户任务。
 * 返回无直接结果；active CPU 完成 vtime/RCU 转换，inactive CPU 至少保存
 * 上下文状态；递归或已经处于目标状态时保持现状。
 */
void noinstr __ct_user_enter(enum ctx_state state)
{
	/* ct 是当前 CPU 长期状态的借用指针。 */
	struct context_tracking *ct = this_cpu_ptr(&context_tracking);
	lockdep_assert_irqs_disabled();

	/* Kernel threads aren't supposed to go to userspace */
	/*
	 * 内核线程不应返回用户态；
	 * current->mm 为空即报告协议错误。
	 */
	WARN_ON_ONCE(!current->mm);

	/* 递归调用不参与转换，最外层退出时才归还 recursion 令牌。 */
	if (!context_tracking_recursion_enter())
		return;

	/* 状态相同表示重复通知，只需正常释放 recursion 深度。 */
	if (__ct_state() != state) {
		if (ct->active) {
			/*
			 * At this stage, only low level arch entry code remains and
			 * then we'll run in userspace. We can assume there won't be
			 * any RCU read-side critical section until the next call to
			 * user_exit() or ct_irq_enter(). Let's remove RCU's dependency
			 * on the tick.
			 */
			/*
			 * 此时只剩低层体系结构出口，直到 user_exit() 或
			 * ct_irq_enter() 前不会再有 RCU 读侧临界区，因此可移除
			 * RCU 对 tick 的依赖。USER 还需在可插桩窗口切换 vtime。
			 */
			if (state == CT_STATE_USER) {
				instrumentation_begin();
				trace_user_enter(0);
				vtime_user_enter(current);
				instrumentation_end();
			}
			/*
			 * Other than generic entry implementation, we may be past the last
			 * rescheduling opportunity in the entry code. Trigger a self IPI
			 * that will fire and reschedule once we resume in user/guest mode.
			 */
			/*
			 * 某些非通用入口可能已越过最后调度点；
			 * 若此时需要 resched，就排一个 self-IPI，
			 * 待恢复 USER/GUEST 后再触发调度。
			 */
			rcu_irq_work_resched();

			/*
			 * Enter RCU idle mode right before resuming userspace.  No use of RCU
			 * is permitted between this call and rcu_eqs_exit(). This way the
			 * CPU doesn't need to maintain the tick for RCU maintenance purposes
			 * when the CPU runs in userspace.
			 */
			/*
			 * 原文要求紧邻恢复用户态进入 RCU idle；
			 * 从该调用到下一次
			 * rcu_eqs_exit() 之间禁止使用 RCU。收益是用户态运行时 RCU
			 * 不再需要维持 tick。
			 */
			ct_kernel_exit(true, CT_RCU_WATCHING + state);

			/*
			 * Special case if we only track user <-> kernel transitions for tickless
			 * cputime accounting but we don't support RCU extended quiescent state.
			 * In this we case we don't care about any concurrency/ordering.
			 */
			/*
			 * 仅做 tickless cputime、未支持 RCU EQS 的特殊配置
			 * 不关心并发/顺序，直接把低位状态设为目标值。
			 */
			if (!IS_ENABLED(CONFIG_CONTEXT_TRACKING_IDLE))
				raw_atomic_set(&ct->state, state);
		} else {
			/*
			 * Even if context tracking is disabled on this CPU, because it's outside
			 * the full dynticks mask for example, we still have to keep track of the
			 * context transitions and states to prevent inconsistency on those of
			 * other CPUs.
			 * If a task triggers an exception in userspace, sleep on the exception
			 * handler and then migrate to another CPU, that new CPU must know where
			 * the exception returns by the time we call exception_exit().
			 * This information can only be provided by the previous CPU when it called
			 * exception_enter().
			 * OTOH we can spare the calls to vtime and RCU when context_tracking.active
			 * is false because we know that CPU is not tickless.
			 */
			/*
			 * 即使本 CPU 不 active
			 * （例如不在 full-dynticks mask），仍须记录上下文，
			 * 防止任务在异常中睡眠并迁移后，另一 CPU 的
			 * exception_exit() 不知道应返回哪里。
			 * inactive CPU 不是 tickless，
			 * 因此可省略 vtime 与 RCU EQS 的昂贵调用。
			 */
			if (!IS_ENABLED(CONFIG_CONTEXT_TRACKING_IDLE)) {
				/* Tracking for vtime only, no concurrent RCU EQS accounting */
				/* 原文：只跟踪 vtime，不存在并发 RCU EQS 记账。 */
				raw_atomic_set(&ct->state, state);
			} else {
				/*
				 * Tracking for vtime and RCU EQS. Make sure we don't race
				 * with NMIs. OTOH we don't care about ordering here since
				 * RCU only requires CT_RCU_WATCHING increments to be fully
				 * ordered.
				 */
				/*
				 * 原文：vtime 与 RCU EQS 都编译时用原子加避免和 NMI
				 * 竞争；这里只改上下文低位，不要求排序，
				 * RCU 只要求
				 * CT_RCU_WATCHING 的增量是全序的。
				 */
				raw_atomic_add(state, &ct->state);
			}
		}
	}
	/* 所有非递归出口在此归还本 CPU recursion 令牌。 */
	context_tracking_recursion_exit();
}
EXPORT_SYMBOL_GPL(__ct_user_enter);

/*
 * OBSOLETE:
 * This function should be noinstr but the below local_irq_restore() is
 * unsafe because it involves illegal RCU uses through tracing and lockdep.
 * This is unlikely to be fixed as this function is obsolete. The preferred
 * way is to call __context_tracking_enter() through user_enter_irqoff()
 * or context_tracking_guest_enter(). It should be the arch entry code
 * responsibility to call into context tracking with IRQs disabled.
 */
/*
 * ct_user_enter() - 为旧调用点保存 IRQ 后进入 USER/GUEST 状态。
 *
 * 原文明确标为过时：理想上应为 noinstr，但 local_irq_restore() 会经 tracing
 * 与 lockdep 产生非法 RCU 使用，且不准备修复。新体系结构应在 IRQ 已关闭时
 * 调 user_enter_irqoff() 或 guest helper。
 * @state: 纯输入 USER/GUEST 状态。返回无直接结果；中断上下文直接跳过，
 * 普通路径保存/恢复 IRQ 并调用底层状态机，无 ownership。
 */
void ct_user_enter(enum ctx_state state)
{
	/* flags 保存入口 IRQ 状态。 */
	unsigned long flags;

	/*
	 * Some contexts may involve an exception occuring in an irq,
	 * leading to that nesting:
	 * ct_irq_enter() rcu_eqs_exit(true) rcu_eqs_enter(true) ct_irq_exit()
	 * This would mess up the dyntick_nesting count though. And rcu_irq_*()
	 * helpers are enough to protect RCU uses inside the exception. So
	 * just return immediately if we detect we are in an IRQ.
	 */
	/*
	 * 异常可能发生在 IRQ 中，形成 IRQ enter、临时 EQS exit/enter、
	 * IRQ exit 的嵌套；再做用户转换会破坏 dyntick_nesting，而 rcu_irq_*
	 * 已足够保护异常内 RCU，所以检测到 IRQ 时直接返回。
	 */
	if (in_interrupt())
		return;

	/* 关闭本地 IRQ 满足 __ct_user_enter() 的每 CPU 原子转换前置条件。 */
	local_irq_save(flags);
	__ct_user_enter(state);
	local_irq_restore(flags);
}
NOKPROBE_SYMBOL(ct_user_enter);
EXPORT_SYMBOL_GPL(ct_user_enter);

/**
 * user_enter_callable() - Unfortunate ASM callable version of user_enter() for
 *			   archs that didn't manage to check the context tracking
 *			   static key from low level code.
 *
 * This OBSOLETE function should be noinstr but it unsafely calls
 * local_irq_restore(), involving illegal RCU uses through tracing and lockdep.
 * This is unlikely to be fixed as this function is obsolete. The preferred
 * way is to call user_enter_irqoff(). It should be the arch entry code
 * responsibility to call into context tracking with IRQs disabled.
 */
/*
 * user_enter_callable() - 供无法在低层汇编测试 static key 的旧架构符号入口。
 *
 * 原文称该 ASM-callable 版本“不幸且过时”：它最终执行不安全的
 * local_irq_restore()，新代码应直接在 IRQ-off 入口调用 user_enter_irqoff()。
 * 入参、返回均无，只转发 user_enter()，不取得引用。
 */
void user_enter_callable(void)
{
	user_enter();
}
NOKPROBE_SYMBOL(user_enter_callable);

/**
 * __ct_user_exit - Inform the context tracking that the CPU is
 *		    exiting user or guest mode and entering the kernel.
 *
 * @state: userspace context-tracking state being exited from.
 *
 * This function must be called after we entered the kernel from user or
 * guest space before any use of RCU read side critical section. This
 * potentially include any high level kernel code like syscalls, exceptions,
 * signal handling, etc...
 *
 * This call supports re-entrancy. This way it can be called from any exception
 * handler without needing to know if we came from userspace or not.
 */
/*
 * __ct_user_exit() - 从 USER/GUEST 进入内核时恢复 RCU watching。
 *
 * 原文要求它在刚进入内核后、任何 syscall/异常/信号等高层代码使用 RCU
 * 之前调用；接口可重入，所以异常 handler 无需先判断来源上下文。
 * @state: 纯输入的待退出 USER/GUEST 状态。调用者固定 CPU，通常 IRQ-off，
 * noinstr 且不可睡眠。返回无直接结果；
 * 只有当前状态匹配时才转换为 KERNEL，
 * USER 路径还结束 vtime；无对象 ownership。
 */
void noinstr __ct_user_exit(enum ctx_state state)
{
	/* ct 是当前 CPU 长期状态的借用指针。 */
	struct context_tracking *ct = this_cpu_ptr(&context_tracking);

	/* 递归调用不改变状态，最外层负责完整转换。 */
	if (!context_tracking_recursion_enter())
		return;

	/* 状态不匹配说明已在 KERNEL 或由其他嵌套完成，只释放 recursion。 */
	if (__ct_state() == state) {
		if (ct->active) {
			/*
			 * Exit RCU idle mode while entering the kernel because it can
			 * run a RCU read side critical section anytime.
			 */
			/*
			 * 进入内核后随时可能执行 RCU 读侧临界区，
			 * 因此首先退出 RCU idle；
			 * 这是真正允许高层内核代码运行的发布边界。
			 */
			ct_kernel_enter(true, CT_RCU_WATCHING - state);
			if (state == CT_STATE_USER) {
				/*
				 * USER 才有对应 vtime/trace，
				 * GUEST 时间由 KVM 外层记账。
				 */
				instrumentation_begin();
				vtime_user_exit(current);
				trace_user_exit(0);
				instrumentation_end();
			}

			/*
			 * Special case if we only track user <-> kernel transitions for tickless
			 * cputime accounting but we don't support RCU extended quiescent state.
			 * In this we case we don't care about any concurrency/ordering.
			 */
			/*
			 * 仅做 tickless cputime、未支持 RCU EQS 时不需要
			 * 并发/排序，直接把状态恢复为 KERNEL。
			 */
			if (!IS_ENABLED(CONFIG_CONTEXT_TRACKING_IDLE))
				raw_atomic_set(&ct->state, CT_STATE_KERNEL);

		} else {
			if (!IS_ENABLED(CONFIG_CONTEXT_TRACKING_IDLE)) {
				/* Tracking for vtime only, no concurrent RCU EQS accounting */
				/* 原文：仅跟踪 vtime，不做并发 RCU EQS 记账。 */
				raw_atomic_set(&ct->state, CT_STATE_KERNEL);
			} else {
				/*
				 * Tracking for vtime and RCU EQS. Make sure we don't race
				 * with NMIs. OTOH we don't care about ordering here since
				 * RCU only requires CT_RCU_WATCHING increments to be fully
				 * ordered.
				 */
				/*
				 * 原文：同时编译 vtime/RCU EQS 时以原子减
				 * 避免 NMI 竞争；上下文低位不要求顺序，
				 * watching 增量才必须全序。
				 */
				raw_atomic_sub(state, &ct->state);
			}
		}
	}
	/* 与最外层 recursion_enter() 配对，恢复下一次转换资格。 */
	context_tracking_recursion_exit();
}
EXPORT_SYMBOL_GPL(__ct_user_exit);

/*
 * OBSOLETE:
 * This function should be noinstr but the below local_irq_save() is
 * unsafe because it involves illegal RCU uses through tracing and lockdep.
 * This is unlikely to be fixed as this function is obsolete. The preferred
 * way is to call __context_tracking_exit() through user_exit_irqoff()
 * or context_tracking_guest_exit(). It should be the arch entry code
 * responsibility to call into context tracking with IRQs disabled.
 */
/*
 * ct_user_exit() - 为旧入口保存 IRQ 后退出 USER/GUEST 状态。
 *
 * 原文同 enter 包装一样标为过时：local_irq_save/restore 可能经 tracing 与
 * lockdep 非法使用 RCU；新架构应直接调用 user_exit_irqoff() 或 guest helper。
 * @state: 纯输入待退出状态。返回无直接结果；
 * IRQ 上下文短路，普通路径关 IRQ
 * 后调用 __ct_user_exit()，无 ownership。
 */
void ct_user_exit(enum ctx_state state)
{
	/* flags 保存入口 IRQ 状态。 */
	unsigned long flags;

	/* IRQ 自身已有 rcu_irq_* 保护，重复用户退出会破坏嵌套记账。 */
	if (in_interrupt())
		return;

	local_irq_save(flags);
	__ct_user_exit(state);
	local_irq_restore(flags);
}
NOKPROBE_SYMBOL(ct_user_exit);
EXPORT_SYMBOL_GPL(ct_user_exit);

/**
 * user_exit_callable() - Unfortunate ASM callable version of user_exit() for
 *			  archs that didn't manage to check the context tracking
 *			  static key from low level code.
 *
 * This OBSOLETE function should be noinstr but it unsafely calls local_irq_save(),
 * involving illegal RCU uses through tracing and lockdep. This is unlikely
 * to be fixed as this function is obsolete. The preferred way is to call
 * user_exit_irqoff(). It should be the arch entry code responsibility to
 * call into context tracking with IRQs disabled.
 */
/*
 * user_exit_callable() - 旧架构从汇编可调用的 user_exit() 符号入口。
 *
 * 它因无法在低层检查 static key 而存在，但已经过时且不安全；
 * 新代码应在 IRQ-off 入口直接调用 user_exit_irqoff()。无入参、无返回，
 * 只转发兼容包装，不改变 ownership。
 */
void user_exit_callable(void)
{
	user_exit();
}
NOKPROBE_SYMBOL(user_exit_callable);

/*
 * ct_cpu_track_user() - 启动期激活指定 CPU 的 USER Context Tracking。
 *
 * @cpu: 纯输入 possible CPU 编号，无对象 ownership。
 * 首次激活该 CPU 时设置 active 并增加全局 static key 计数；全系统首次调用
 * 还把 TIF_NOHZ 播种到 init_task，后续 fork 继承。__init 上下文不可在运行期
 * 调用；返回无直接结果，重复 CPU 激活幂等。
 */
void __init ct_cpu_track_user(int cpu)
{
	/*
	 * initialized 只记录“一次性全局初始化”是否完成，位于 __initdata，
	 * 启动后随 init 段回收；它不替代每 CPU active。
	 */
	static __initdata bool initialized = false;

	/* 每个 CPU 只在 false->true 时增加 key，保持 static-key 引用计数平衡。 */
	if (!per_cpu(context_tracking.active, cpu)) {
		per_cpu(context_tracking.active, cpu) = true;
		static_branch_inc(&context_tracking_key);
	}

	if (initialized)
		return;

#ifdef CONFIG_HAVE_TIF_NOHZ
	/*
	 * Set TIF_NOHZ to init/0 and let it propagate to all tasks through fork
	 * This assumes that init is the only task at this early boot stage.
	 */
	/*
	 * 把 TIF_NOHZ 设置到 init/0，随后由 fork 传播给所有任务；
	 * 前提是此启动阶段 init 仍是唯一任务。
	 * 下面的 tasklist 检查验证该假设。
	 */
	set_tsk_thread_flag(&init_task, TIF_NOHZ);
#endif
	/*
	 * 若已有其他任务，TIF 继承模型可能漏标，
	 * 故以 WARN 暴露初始化过晚。
	 */
	WARN_ON_ONCE(!tasklist_empty());

	/* 只在完成全局播种与前置条件检查后发布 initialized。 */
	initialized = true;
}

#ifdef CONFIG_CONTEXT_TRACKING_USER_FORCE
/*
 * context_tracking_init() - 强制为所有 possible CPU 启用 USER 跟踪。
 *
 * 入参、返回均无。启动期遍历 CPU 编号并调用 ct_cpu_track_user()；
 * 它不启动 CPU、不分配 per-CPU 对象，只修改既有 active/static-key 状态。
 */
void __init context_tracking_init(void)
{
	/* cpu 是 for_each_possible_cpu() 产生的逻辑 CPU 编号，仅循环内有效。 */
	int cpu;

	for_each_possible_cpu(cpu)
		ct_cpu_track_user(cpu);
}
#endif

#endif /* #ifdef CONFIG_CONTEXT_TRACKING_USER */
/* USER/GUEST 状态机及启动初始化仅在配置 USER 跟踪时编译。 */
