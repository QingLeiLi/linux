// SPDX-License-Identifier: GPL-2.0
/*
 * preemptoff/irqoff trace 实现学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 本文件把 IRQ/抢占状态转换同时送到两类消费者：
 * - tracepoint：生成 irq_enable/disable、preempt_enable/disable 事件；
 * - tracer：维护 irqsoff/preemptoff 延迟区间。
 *
 * include/linux/irqflags.h 的 local_irq_* 包装调用这里的
 * trace_hardirqs_on/off；低层 entry code 因 RCU watching 与 lockdep
 * 排序要求，改用 on_prepare/off_finish 分阶段接口。真正硬件 IRQ 状态
 * 已由调用者或架构代码改变，本文件只维护跟踪与 lockdep 软件视图。
 *
 * 并发模型以 current CPU 为单位：tracing_irq_cpu 抑制重复 ON/OFF，
 * current 和 CALLER_ADDR* 描述本次执行流。普通架构 fallback 在 idle
 * 中临时唤醒 context tracking/RCU 后调用 tracepoint；NMI 中直接跳过，
 * 因为该 fallback 不是 NMI-safe。
 *
 * 收益是统一记录 IRQ-off/preempt-off 延迟和调用位置；代价是 tracepoint
 * 可能递归进入内核基础设施，所以热路径先检查 enabled、用 per-CPU 状态
 * 去重，并以 NOKPROBE_SYMBOL 阻止 kprobe 再次探测这些转换函数。
 */
/*
 * preemptoff and irqoff tracepoints
 *
 * Copyright (C) Joel Fernandes (Google) <joel@joelfernandes.org>
 */
/*
 * 中文对译：
 * 本文件实现关闭抢占和关闭 IRQ 的 tracepoint。原作者版权信息如上。
 */

#include <linux/kallsyms.h>
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include <linux/hardirq.h>
#include "trace.h"

#define CREATE_TRACE_POINTS
#include <trace/events/preemptirq.h>

/*
 * Use regular trace points on architectures that implement noinstr
 * tooling: these calls will only happen with RCU enabled, which can
 * use a regular tracepoint.
 *
 * On older architectures, RCU may not be watching in idle. In that
 * case, wake up RCU to watch while calling the tracepoint. These
 * aren't NMI-safe - so exclude NMI contexts:
 */
/*
 * 中文对译与补充：
 * 实现 noinstr 工具链约束的架构只会在 RCU 正在观察时进入这些调用，
 * 因而可以直接使用普通 tracepoint。较旧架构的 idle 上下文中 RCU 可能
 * 暂停观察，此时调用 tracepoint 前要用 ct_irq_enter() 临时让 RCU
 * watching，结束后由 ct_irq_exit() 恢复。该兼容路径不是 NMI-safe，
 * 所以 NMI 上下文直接放弃事件。
 *
 * trace(point, args) 先用静态键式 enabled 检查避免未启用事件的成本。
 * exit_rcu 只记录本宏是否亲自进入 context tracking，防止错误退出调用者
 * 原本已经建立的 RCU watching 状态。宏不改变真实 IRQ/抢占状态。
 */
#ifdef CONFIG_ARCH_WANTS_NO_INSTR
#define trace(point, args)	trace_##point(args)
#else
#define trace(point, args)					\
	do {							\
		if (__trace_##point##_enabled()) {		\
			bool exit_rcu = false;			\
			if (in_nmi())				\
				break;				\
			if (!IS_ENABLED(CONFIG_TINY_RCU) &&	\
			    is_idle_task(current)) {		\
				ct_irq_enter();			\
				exit_rcu = true;		\
			}					\
			trace_##point(args);			\
			if (exit_rcu)				\
				ct_irq_exit();			\
		}						\
	} while (0)
#endif

#ifdef CONFIG_TRACE_IRQFLAGS
/* Per-cpu variable to prevent redundant calls when IRQs already off */
/*
 * 每 CPU 状态用于避免 IRQ 已关闭时重复产生 disable 事件：
 * 0 表示 tracer 认为 IRQ-on，1 表示 IRQ-off。更新发生在当前 CPU 且调用
 * 路径已具有 IRQ/entry 上下文约束，无需全局锁；它是 tracer 私有状态，
 * 与 lockdep 的 hardirqs_enabled、硬件 DAIF/PMR 是三个不同状态面。
 */
static DEFINE_PER_CPU(int, tracing_irq_cpu);

/*
 * Like trace_hardirqs_on() but without the lockdep invocation. This is
 * used in the low level entry code where the ordering vs. RCU is important
 * and lockdep uses a staged approach which splits the lockdep hardirq
 * tracking into a RCU on and a RCU off section.
 */
/*
 * 中文对译：
 * 本函数类似 trace_hardirqs_on()，但不调用 lockdep。低层 entry code
 * 需要精确安排 RCU 顺序，而 lockdep 又把 hardirq 跟踪拆成 RCU-on 与
 * RCU-off 两阶段，因此使用这个 prepare 版本。
 */
/*
 * trace_hardirqs_on_prepare() - 完成 IRQ-on 的 tracer 半部。
 *
 * 调用者已保证当前 CPU/RCU 状态适合 tracepoint。仅当私有状态原为 off
 * 时，记录两级调用地址、结束 irqsoff tracer 区间，最后写 0 提交 ON；
 * 重复 on 无副作用。无参数、返回值和资源 ownership 变化。
 */
void trace_hardirqs_on_prepare(void)
{
	if (this_cpu_read(tracing_irq_cpu)) {
		/* 先把转换交给事件与延迟 tracer，完成后才提交 per-CPU ON。 */
		trace(irq_enable, TP_ARGS(CALLER_ADDR0, CALLER_ADDR1));
		tracer_hardirqs_on(CALLER_ADDR0, CALLER_ADDR1);
		this_cpu_write(tracing_irq_cpu, 0);
	}
}
/*
 * 以下 IRQ 转换入口均导出给通用/架构低层代码，并标成 NOKPROBE，避免
 * kprobe 在 IRQ 状态转换函数内递归触发 tracing。
 */
EXPORT_SYMBOL(trace_hardirqs_on_prepare);
NOKPROBE_SYMBOL(trace_hardirqs_on_prepare);

/*
 * trace_hardirqs_on() - 完整提交 hardirq ON 软件状态。
 *
 * 普通 local_irq_enable/restore 在真正打开硬件 IRQ 前调用。函数先按需
 * 完成 tracer ON，再执行 lockdep prepare/final 两阶段：prepare 检查
 * held-lock usage 和 RCU 可见部分，final 发布 hardirqs_enabled 及事件
 * 位置。重复 tracer ON 会去重，lockdep 自己处理重复统计。无失败返回。
 */
void trace_hardirqs_on(void)
{
	if (this_cpu_read(tracing_irq_cpu)) {
		trace(irq_enable, TP_ARGS(CALLER_ADDR0, CALLER_ADDR1));
		tracer_hardirqs_on(CALLER_ADDR0, CALLER_ADDR1);
		this_cpu_write(tracing_irq_cpu, 0);
	}

	lockdep_hardirqs_on_prepare();
	lockdep_hardirqs_on(CALLER_ADDR0);
}
EXPORT_SYMBOL(trace_hardirqs_on);
NOKPROBE_SYMBOL(trace_hardirqs_on);

/*
 * Like trace_hardirqs_off() but without the lockdep invocation. This is
 * used in the low level entry code where the ordering vs. RCU is important
 * and lockdep uses a staged approach which splits the lockdep hardirq
 * tracking into a RCU on and a RCU off section.
 */
/*
 * 中文对译：
 * 本函数类似 trace_hardirqs_off()，但不调用 lockdep。低层 entry code
 * 为满足 RCU 排序，把 lockdep hardirq 跟踪拆开，因此使用 finish 版本。
 */
/*
 * trace_hardirqs_off_finish() - 完成 IRQ-off 的 tracer 半部。
 *
 * 调用者已在此前关闭硬件 IRQ并完成对应 lockdep 阶段。仅从 ON 转 OFF
 * 时先写 tracing_irq_cpu=1，随后启动 irqsoff tracer 并发出 disable
 * tracepoint；先提交 OFF 可阻止跟踪回调递归时再次记录同一转换。
 */
void trace_hardirqs_off_finish(void)
{
	if (!this_cpu_read(tracing_irq_cpu)) {
		this_cpu_write(tracing_irq_cpu, 1);
		tracer_hardirqs_off(CALLER_ADDR0, CALLER_ADDR1);
		trace(irq_disable, TP_ARGS(CALLER_ADDR0, CALLER_ADDR1));
	}

}
EXPORT_SYMBOL(trace_hardirqs_off_finish);
NOKPROBE_SYMBOL(trace_hardirqs_off_finish);

/*
 * trace_hardirqs_off() - 完整提交 hardirq OFF 软件状态。
 *
 * 普通 local_irq_disable/save 在硬件已经关闭后调用。先让 lockdep 记录
 * ON->OFF 和调用位置，再按需提交 tracer OFF。这个顺序保证 tracer 回调
 * 中发生锁操作时 lockdep 已经知道当前处于 IRQ-off。重复 off 只增加
 * lockdep 的冗余统计，不重复开启 irqsoff 区间。
 */
void trace_hardirqs_off(void)
{
	lockdep_hardirqs_off(CALLER_ADDR0);

	if (!this_cpu_read(tracing_irq_cpu)) {
		this_cpu_write(tracing_irq_cpu, 1);
		tracer_hardirqs_off(CALLER_ADDR0, CALLER_ADDR1);
		trace(irq_disable, TP_ARGS(CALLER_ADDR0, CALLER_ADDR1));
	}
}
EXPORT_SYMBOL(trace_hardirqs_off);
NOKPROBE_SYMBOL(trace_hardirqs_off);
#endif /* CONFIG_TRACE_IRQFLAGS */
/* CONFIG_TRACE_IRQFLAGS=n 时不生成上述 hardirq 跟踪实现。 */

#ifdef CONFIG_TRACE_PREEMPT_TOGGLE

/*
 * trace_preempt_on/off() - 发布抢占开关 trace 与延迟 tracer 转换。
 *
 * a0/a1 是调用者及其父调用者的指令地址快照，不取得代码引用。on 结束
 * preemptoff 区间，off 开始区间；调用者负责在真实抢占状态转换的正确
 * 一侧调用。这里不修改 preempt_count、IRQ 或 lockdep hardirq 状态，
 * 无失败返回。
 */
void trace_preempt_on(unsigned long a0, unsigned long a1)
{
	trace(preempt_enable, TP_ARGS(a0, a1));
	tracer_preempt_on(a0, a1);
}

void trace_preempt_off(unsigned long a0, unsigned long a1)
{
	trace(preempt_disable, TP_ARGS(a0, a1));
	tracer_preempt_off(a0, a1);
}
#endif
