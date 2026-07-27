/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IRQ flags 通用接口学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 职责边界：
 * 本头文件位于体系结构 IRQ 屏蔽原语与通用内核调用者之间。它把
 * asm/irqflags.h 提供的 arch_local_irq_*() 包装成 raw_local_irq_*()，
 * 再按 CONFIG_TRACE_IRQFLAGS 决定 local_irq_*() 是否同步更新 lockdep 和
 * irqsoff/preempt tracer 的逻辑状态。它不负责 IRQ controller 的路由、
 * 中断入口保存现场、softirq 调度，也不定义各架构 flags 的位布局。
 *
 * 主调用链：
 *   普通临界区
 *     local_irq_save(flags)
 *       -> arch_local_irq_save()
 *       -> 必要时 trace_hardirqs_off()
 *     临界区结束
 *       -> 必要时 trace_hardirqs_on()
 *       -> arch_local_irq_restore(flags)
 *
 *   中断入口/退出
 *     entry code -> trace_hardirqs_off_finish()/on_prepare()
 *                -> lockdep 状态转换和 tracepoint
 *
 * 核心状态：
 * - 硬件状态由当前 CPU 的架构寄存器保存；在 arm64 上可能是 DAIF，也
 *   可能在 pseudo-NMI 配置下由 GIC PMR 表达。unsigned long flags 因而是
 *   只能交回同一套 arch helper 的不透明快照。
 * - hardirqs_enabled/hardirq_context 是 per-CPU 的 lockdep 逻辑状态；
 *   softirq_context、softirqs_enabled、irq_config 等位于 current task。
 *   逻辑状态用于验证锁依赖，不能代替硬件寄存器查询。
 *
 * 并发与上下文：
 * local/raw_local 操作只影响当前 CPU，不阻止其他 CPU，也不等价于
 * spin_lock。调用者必须在同一 CPU 上成对 save/restore；屏蔽本地 IRQ
 * 通常也使当前 CPU 的普通中断处理程序不能重入临界区，但 NMI、pseudo
 * NMI 和其他 CPU 仍可能并发。raw 版本绕过 trace/lockdep，主要供 tracing、
 * entry/noinstr 等不能递归进入跟踪设施的底层路径使用。
 *
 * 方案权衡：
 * 分层包装让关闭 TRACE_IRQFLAGS 的构建退化为直接架构调用，热路径没有
 * 跟踪成本；启用后则能发现“硬件已开中断但 lockdep 仍认为关闭”等状态
 * 失配，并统计关中断延迟。代价是开关顺序必须同时满足硬件安全和跟踪
 * 一致性，底层代码误用 raw API 会形成 lockdep 看不见的窗口。
 */
/*
 * include/linux/irqflags.h
 *
 * IRQ flags tracing: follow the state of the hardirq and softirq flags and
 * provide callbacks for transitions between ON and OFF states.
 *
 * This file gets included from lowlevel asm headers too, to provide
 * wrapped versions of the local_irq_*() APIs, based on the
 * raw_local_irq_*() macros from the lowlevel headers.
 */
/*
 * 中文对译：
 * 本文件跟踪 hardirq/softirq 标志的状态，并在 ON/OFF 转换时提供回调。
 * 它也会被低层汇编相关头文件包含，以体系结构头提供的
 * raw_local_irq_* 底层能力为基础，形成通用 local_irq_* 包装接口。
 *
 * 学习补充：这里的“跟踪状态”是 lockdep/tracer 的软件模型；真正是否
 * 接收 IRQ 仍由 arch_local_irq_* 操作的当前 CPU 硬件状态决定。两者必须
 * 按本文件规定的顺序同步更新。
 */
#ifndef _LINUX_TRACE_IRQFLAGS_H
#define _LINUX_TRACE_IRQFLAGS_H

#include <linux/irqflags_types.h>
#include <linux/typecheck.h>
#include <linux/cleanup.h>
#include <asm/irqflags.h>
#include <asm/percpu.h>

/*
 * 这里只需要在 CPU hotplug cleanup 声明中借用 idle task 指针；前向声明
 * 避免 irqflags 这个底层、广泛包含的头文件反向引入完整 sched.h。
 */
struct task_struct;

/* Currently lockdep_softirqs_on/off is used only by lockdep */
/*
 * 当前 lockdep_softirqs_on/off 只服务于 lockdep。
 *
 * CONFIG_PROVE_LOCKING=y 时，下列接口把 hardirq/softirq 的开关位置和
 * CPU 上下文送入依赖验证器；调用参数 ip 是发生转换的指令地址借用值。
 * cleanup_dead_cpu() 在 CPU 下线时清理该 CPU 和 idle task 的残留状态。
 * 这些函数只维护调试模型，不直接修改硬件中断屏蔽位。
 *
 * CONFIG_PROVE_LOCKING=n 时用同签名 inline stub 消除调用，保持调用者无需
 * 条件编译。所有参数均只为接口兼容，不取得 task 引用，也无返回值。
 */
#ifdef CONFIG_PROVE_LOCKING
  extern void lockdep_softirqs_on(unsigned long ip);
  extern void lockdep_softirqs_off(unsigned long ip);
  extern void lockdep_hardirqs_on_prepare(void);
  extern void lockdep_hardirqs_on(unsigned long ip);
  extern void lockdep_hardirqs_off(unsigned long ip);
  extern void lockdep_cleanup_dead_cpu(unsigned int cpu,
				       struct task_struct *idle);
#else
  static inline void lockdep_softirqs_on(unsigned long ip) { }
  static inline void lockdep_softirqs_off(unsigned long ip) { }
  static inline void lockdep_hardirqs_on_prepare(void) { }
  static inline void lockdep_hardirqs_on(unsigned long ip) { }
  static inline void lockdep_hardirqs_off(unsigned long ip) { }
  static inline void lockdep_cleanup_dead_cpu(unsigned int cpu,
					      struct task_struct *idle) {}
#endif

#ifdef CONFIG_TRACE_IRQFLAGS

/*
 * hardirqs_enabled：lockdep 对“当前 CPU 是否逻辑允许 hardirq”的视图；
 * hardirq_context：当前 CPU 嵌套进入 hardirq 上下文的深度。
 *
 * 两者由中断 entry/exit 与下面的 trace/lockdep helper 更新。per-CPU
 * 布局避免普通中断热路径争用全局 cacheline；访问者仍须使用对应的
 * this_cpu/raw_cpu 原语，并满足禁止迁移或中断上下文等调用约束。
 */
DECLARE_PER_CPU(int, hardirqs_enabled);
DECLARE_PER_CPU(int, hardirq_context);

/*
 * trace_hardirqs_*() - 同步 hardirq 的跟踪与 lockdep 状态。
 *
 * on/off 是完整转换入口；on_prepare/off_finish 用于 entry code 已经在
 * 汇编或架构层改变硬件状态、但需要把 lockdep 与 tracepoint 分阶段排列
 * 的场景。无参数、无返回值，作用于当前 CPU/current；实现可能进入
 * lockdep 和 tracing，因此 noinstr/raw 路径不能随意调用完整包装。
 */
extern void trace_hardirqs_on_prepare(void);
extern void trace_hardirqs_off_finish(void);
extern void trace_hardirqs_on(void);
extern void trace_hardirqs_off(void);

/*
 * lockdep IRQ 状态查询组：
 * - hardirq_context 取当前 CPU 的嵌套深度；
 * - softirq_context/softirqs_enabled 取指定 task 的软件状态；
 * - hardirqs_enabled 取当前 CPU 的 lockdep 逻辑开关。
 *
 * raw_cpu_read() 适合查询“正在描述的 CPU 上下文深度”，不隐含抢占检查；
 * this_cpu_read() 则表达当前 CPU 热路径访问。返回值用于诊断和锁依赖分类，
 * 不能据此判断架构寄存器是否真的屏蔽 IRQ。
 */
# define lockdep_hardirq_context()	(raw_cpu_read(hardirq_context))
# define lockdep_softirq_context(p)	((p)->softirq_context)
# define lockdep_hardirqs_enabled()	(this_cpu_read(hardirqs_enabled))
# define lockdep_softirqs_enabled(p)	((p)->softirqs_enabled)

/*
 * lockdep_hardirq_enter/threaded/exit - 维护 hardirq 上下文分类。
 *
 * enter/exit 必须严格成对且运行在当前 CPU 不会迁移的中断路径；
 * __this_cpu_inc/dec 只改变本 CPU 深度。最外层 enter 把
 * current->hardirq_threaded 清零，随后若 IRQ 被强制线程化，
 * handle 路径用 threaded() 标记，使 lockdep 不把线程化 handler
 * 错判成普通 hardirq 锁上下文。宏无失败返回；失配会污染后续锁验证。
 */
# define lockdep_hardirq_enter()			\
do {							\
	if (__this_cpu_inc_return(hardirq_context) == 1)\
		current->hardirq_threaded = 0;		\
} while (0)
# define lockdep_hardirq_threaded()		\
do {						\
	current->hardirq_threaded = 1;		\
} while (0)
# define lockdep_hardirq_exit()			\
do {						\
	__this_cpu_dec(hardirq_context);	\
} while (0)

/*
 * hrtimer 的回调既可能在 hardirq 中执行，也可能被配置为 soft 模式。
 * enter() 返回本次回调是否保持 hardirq 语义，调用者必须把该布尔值原样
 * 传给 exit()。soft hrtimer 期间设置 current->irq_config，告诉 lockdep
 * 这是显式配置的非普通 hardirq 回调；退出时恢复为零。这里不改变实际
 * IRQ 屏蔽状态，也不取得 hrtimer 所有权。
 */
# define lockdep_hrtimer_enter(__hrtimer)		\
({							\
	bool __expires_hardirq = true;			\
							\
	if (!__hrtimer->is_hard) {			\
		current->irq_config = 1;		\
		__expires_hardirq = false;		\
	}						\
	__expires_hardirq;				\
})

# define lockdep_hrtimer_exit(__expires_hardirq)	\
	do {						\
		if (!__expires_hardirq)			\
			current->irq_config = 0;	\
	} while (0)

/*
 * POSIX timer 回调使用同一 irq_config 例外标记。enter/exit 必须在同一
 * current 上成对，嵌套规则由调用路径保证；宏只影响 lockdep 分类。
 */
# define lockdep_posixtimer_enter()				\
	  do {							\
		  current->irq_config = 1;			\
	  } while (0)

# define lockdep_posixtimer_exit()				\
	  do {							\
		  current->irq_config = 0;			\
	  } while (0)

/*
 * irq_work 根据 IRQ_WORK_HARD_IRQ 标志区分真正 hardirq work 与在其他
 * IRQ 配置上下文执行的 work。非 hardirq 项在回调窗口把 irq_config
 * 置一，exit 按相同 flags 清零；_flags 是调用者借用的标志快照，宏不
 * 修改它。传入不同 flags 或漏掉 exit 会让 lockdep 上下文长期失真。
 */
# define lockdep_irq_work_enter(_flags)					\
	  do {								\
		  if (!((_flags) & IRQ_WORK_HARD_IRQ))			\
			current->irq_config = 1;			\
	  } while (0)
# define lockdep_irq_work_exit(_flags)					\
	  do {								\
		  if (!((_flags) & IRQ_WORK_HARD_IRQ))			\
			current->irq_config = 0;			\
	  } while (0)

#else
/*
 * CONFIG_TRACE_IRQFLAGS=n 时，跟踪转换、上下文查询和分类宏全部折叠为
 * 常量或空操作。hrtimer enter 固定返回 false，exit 显式消费参数以避免
 * 未使用告警；调用者的控制流和配置组合仍保持可编译。
 */
# define trace_hardirqs_on_prepare()		do { } while (0)
# define trace_hardirqs_off_finish()		do { } while (0)
# define trace_hardirqs_on()			do { } while (0)
# define trace_hardirqs_off()			do { } while (0)
# define lockdep_hardirq_context()		0
# define lockdep_softirq_context(p)		0
# define lockdep_hardirqs_enabled()		0
# define lockdep_softirqs_enabled(p)		0
# define lockdep_hardirq_enter()		do { } while (0)
# define lockdep_hardirq_threaded()		do { } while (0)
# define lockdep_hardirq_exit()			do { } while (0)
# define lockdep_softirq_enter()		do { } while (0)
# define lockdep_softirq_exit()			do { } while (0)
# define lockdep_hrtimer_enter(__hrtimer)	false
# define lockdep_hrtimer_exit(__context)	do { (void)(__context); } while (0)
# define lockdep_posixtimer_enter()		do { } while (0)
# define lockdep_posixtimer_exit()		do { } while (0)
# define lockdep_irq_work_enter(__work)		do { } while (0)
# define lockdep_irq_work_exit(__work)		do { } while (0)
#endif

#if defined(CONFIG_TRACE_IRQFLAGS) && !defined(CONFIG_PREEMPT_RT)
/*
 * 非 RT 内核中 softirq 具有独立的原子上下文语义，enter/exit 通过
 * current->softirq_context 维护可嵌套深度，供 lockdep 判断某把锁是否
 * 可能在 softirq 中取得。调用者必须在同一 task 上严格成对。
 *
 * PREEMPT_RT 会把多数 softirq 工作线程化，其锁与抢占语义不同，因此
 * 不能沿用普通内核的 softirq_context 分类；RT 或未启用跟踪时统一为空
 * 操作。这些宏只记账，不负责 local_bh_disable/enable 或执行 softirq。
 */
# define lockdep_softirq_enter()		\
do {						\
	current->softirq_context++;		\
} while (0)
# define lockdep_softirq_exit()			\
do {						\
	current->softirq_context--;		\
} while (0)

#else
# define lockdep_softirq_enter()		do { } while (0)
# define lockdep_softirq_exit()			do { } while (0)
#endif

#if defined(CONFIG_IRQSOFF_TRACER) || \
	defined(CONFIG_PREEMPT_TRACER)
/*
 * critical timing 回调组：
 * stop_critical_timings() 暂停 irqsoff/preemptoff 临界区计时，
 * start_critical_timings() 恢复计时，供 tracer 自身或不可递归区域避免
 * 把跟踪开销计入被测窗口。它们不改变 IRQ/抢占硬件状态，必须由调用者
 * 按原状态配对。两个 tracer 都关闭时退化为空宏。
 */
 extern void stop_critical_timings(void);
 extern void start_critical_timings(void);
#else
# define stop_critical_timings() do { } while (0)
# define start_critical_timings() do { } while (0)
#endif

#ifdef CONFIG_DEBUG_IRQFLAGS
/*
 * raw_check_bogus_irq_restore() - 调试 save/restore 协议误用。
 *
 * raw_local_irq_restore(flags) 期望在当前 IRQ 已关闭的临界区尾部恢复旧
 * 快照；若 restore 前 arch_irqs_disabled() 为 false，通常表示调用者已
 * 提前开中断、flags 未按词法路径配对，或跨上下文传递了快照。调试构建
 * 触发一次警告但仍继续执行真正 restore；非调试构建无检查成本。
 *
 * 检查读取当前 CPU 的真实架构状态，而非 lockdep 的 hardirqs_enabled。
 */
extern void warn_bogus_irq_restore(void);
#define raw_check_bogus_irq_restore()			\
	do {						\
		if (unlikely(!arch_irqs_disabled()))	\
			warn_bogus_irq_restore();	\
	} while (0)
#else
#define raw_check_bogus_irq_restore() do { } while (0)
#endif

/*
 * Wrap the arch provided IRQ routines to provide appropriate checks.
 */
/*
 * 中文对译：
 * 对体系结构提供的 IRQ 原语做通用包装，并加入必要的类型和调试检查。
 *
 * 学习补充：
 * raw_local_irq_* 仍然只操作当前 CPU 的硬件 IRQ 状态，不更新
 * TRACE_IRQFLAGS 软件模型。raw 的含义不是“更强的关中断”，而是绕过
 * 通用跟踪层；只有底层 entry、tracing 或明确自行维护状态的路径才应
 * 选择它。普通代码应使用后面的 local_irq_*。
 */

/*
 * raw_local_irq_disable/enable - 直接关闭/开启当前 CPU IRQ。
 *
 * 无保存值、无失败返回，也不提供嵌套计数：enable 会无条件打开体系
 * 结构所定义的普通 IRQ。调用者若不知道入口状态，不应使用这对接口，
 * 而应使用 save/restore 保留外层临界区。
 */
#define raw_local_irq_disable()		arch_local_irq_disable()
#define raw_local_irq_enable()		arch_local_irq_enable()

/*
 * raw_local_irq_save(flags) - 保存入口 IRQ 快照并关闭当前 CPU IRQ。
 *
 * flags 是调用者提供的 unsigned long 左值输出；typecheck 只做编译期
 * 类型验证，不产生运行期转换。arch_local_irq_save() 返回的位布局由
 * 当前架构决定，值只能用于 disabled_flags 查询或配对 restore。
 */
#define raw_local_irq_save(flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		flags = arch_local_irq_save();		\
	} while (0)

/*
 * raw_local_irq_restore(flags) - 恢复 save 时的当前 CPU IRQ 状态。
 *
 * flags 是不透明输入值且不会被消费或改写。调试构建先检查当前 IRQ
 * 是否仍关闭，再交给架构恢复；它可能恢复为“仍关闭”，因此 restore
 * 不等价于 enable。快照不可跨 CPU、跨不匹配的嵌套层或随意合成。
 */
#define raw_local_irq_restore(flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		raw_check_bogus_irq_restore();		\
		arch_local_irq_restore(flags);		\
	} while (0)

/*
 * raw_local_save_flags(flags) 只读取当前硬件状态，不改变 IRQ；
 * raw_irqs_disabled_flags(flags) 解释此前由同架构保存的快照；
 * raw_irqs_disabled() 直接查询当前 CPU；raw_safe_halt() 进入架构定义的
 * 安全 idle/halt。查询结果是硬件视图，与 lockdep 逻辑状态相互独立。
 */
#define raw_local_save_flags(flags)			\
	do {						\
		typecheck(unsigned long, flags);	\
		flags = arch_local_save_flags();	\
	} while (0)
#define raw_irqs_disabled_flags(flags)			\
	({						\
		typecheck(unsigned long, flags);	\
		arch_irqs_disabled_flags(flags);	\
	})
#define raw_irqs_disabled()		(arch_irqs_disabled())
#define raw_safe_halt()			arch_safe_halt()

/*
 * The local_irq_*() APIs are equal to the raw_local_irq*()
 * if !TRACE_IRQFLAGS.
 */
/*
 * 中文对译：
 * 未启用 TRACE_IRQFLAGS 时，local_irq_* 与 raw_local_irq_* 等价。
 *
 * 学习补充：
 * 启用跟踪后，local 层必须让软件模型与硬件转换保持无竞态顺序：
 * 关中断时先关闭硬件，再报告 OFF；开中断时先报告 ON，再打开硬件。
 * 否则硬件允许 IRQ 的短窗口内，真正的中断可能进入，而 lockdep 仍把
 * CPU 归类为 IRQ-off，形成错误的锁依赖记录。
 */
#ifdef CONFIG_TRACE_IRQFLAGS

/*
 * local_irq_enable() - 无条件打开当前 CPU IRQ 并报告 ON。
 *
 * trace 必须先于 raw enable：raw enable 后 IRQ 可立即打断当前指令流，
 * 因此在硬件开放前 lockdep/tracer 就必须完成状态发布。无返回值，不
 * 保留外层状态，只适合入口状态确定为关闭的路径。
 */
#define local_irq_enable()				\
	do {						\
		trace_hardirqs_on();			\
		raw_local_irq_enable();			\
	} while (0)

/*
 * local_irq_disable() - 关闭当前 CPU IRQ，必要时报告一次 OFF 转换。
 *
 * 先读取真实硬件状态，再 raw disable，关闭“中断可进入”的竞态窗口；
 * 只有入口确实开启时才 trace OFF，避免嵌套 disable 重复推进 lockdep
 * 事件序列。即使入口已关闭，最终硬件状态仍保持关闭。
 */
#define local_irq_disable()				\
	do {						\
		bool was_disabled = raw_irqs_disabled();\
		raw_local_irq_disable();		\
		if (!was_disabled)			\
			trace_hardirqs_off();		\
	} while (0)

/*
 * local_irq_save(flags) - 保存入口状态并关闭 IRQ。
 *
 * raw save 同时完成取快照和硬件关闭；只有快照表明入口开启时才报告
 * OFF。flags 是 unsigned long 输出，并携带“外层原本是否已关闭”的
 * 嵌套信息，调用者必须在所有退出路径交给 local_irq_restore()。
 */
#define local_irq_save(flags)				\
	do {						\
		raw_local_irq_save(flags);		\
		if (!raw_irqs_disabled_flags(flags))	\
			trace_hardirqs_off();		\
	} while (0)

/*
 * local_irq_restore(flags) - 恢复 save 的硬件与跟踪状态。
 *
 * 若目标快照是 IRQ-on，先 trace ON 再执行 raw restore，原因与 enable
 * 相同；若目标仍 IRQ-off，则不产生 ON 事件。restore 不保证最终开中断，
 * 而是精确恢复外层状态，所以支持嵌套 save/restore。
 */
#define local_irq_restore(flags)			\
	do {						\
		if (!raw_irqs_disabled_flags(flags))	\
			trace_hardirqs_on();		\
		raw_local_irq_restore(flags);		\
	} while (0)

/*
 * safe_halt() - 在跟踪模型切换到 IRQ-on 后进入架构安全 halt。
 *
 * arch_safe_halt() 通常以避免“检查后睡眠”丢中断的架构协议开启 IRQ 并
 * 等待事件；因此调用前必须让 lockdep/tracer 先看到 ON。函数无返回值
 * 契约，CPU 被事件唤醒后从架构原语之后继续执行。
 */
#define safe_halt()				\
	do {					\
		trace_hardirqs_on();		\
		raw_safe_halt();		\
	} while (0)


#else /* !CONFIG_TRACE_IRQFLAGS */

/*
 * 无 TRACE_IRQFLAGS 构建中，local API 是 raw API 的薄包装。保留 do/while
 * 形式以维持语句宏语义；硬件效果、flags 所有权和嵌套约束完全相同，
 * 只是没有 lockdep IRQ 状态与 irqsoff trace 事件。
 */
#define local_irq_enable()	do { raw_local_irq_enable(); } while (0)
#define local_irq_disable()	do { raw_local_irq_disable(); } while (0)
#define local_irq_save(flags)	do { raw_local_irq_save(flags); } while (0)
#define local_irq_restore(flags) do { raw_local_irq_restore(flags); } while (0)
#define safe_halt()		do { raw_safe_halt(); } while (0)

#endif /* CONFIG_TRACE_IRQFLAGS */

/*
 * local_save_flags(flags) 只取得当前 CPU 的不透明 IRQ 快照，不关闭 IRQ。
 * 它不触发 trace 转换，因为硬件状态没有改变；flags 的类型和后续解释
 * 规则同 raw_local_save_flags()。
 */
#define local_save_flags(flags)	raw_local_save_flags(flags)

/*
 * Some architectures don't define arch_irqs_disabled(), so even if either
 * definition would be fine we need to use different ones for the time being
 * to avoid build issues.
 */
/*
 * 中文对译：
 * 某些体系结构没有定义 arch_irqs_disabled()。因此尽管两种实现的结果
 * 都可以满足接口，当前仍需按配置选择不同形式，以避免构建失败。
 *
 * 学习补充：
 * TRACE_IRQFLAGS_SUPPORT 表示架构具有适合通用跟踪包装的 flags
 * save/interpret 能力。支持时，irqs_disabled() 通过“保存快照再解释”
 * 统一查询，不要求额外的 arch_irqs_disabled()；其他构建直接使用架构
 * 提供的 raw 查询。两条路径都只读当前 CPU 硬件状态，不查询 lockdep。
 */
#ifdef CONFIG_TRACE_IRQFLAGS_SUPPORT
/*
 * irqs_disabled() - 查询当前 CPU 的真实 IRQ 屏蔽状态。
 *
 * _flags 是宏内部的 unsigned long 瞬时快照；保存操作不改变 IRQ，
 * raw_irqs_disabled_flags() 按同一架构解释后返回布尔值。调用期间若调用
 * 环境允许中断改变状态，结果只代表取样时刻，不是后续代码的稳定
 * 保证。
 */
#define irqs_disabled()					\
	({						\
		unsigned long _flags;			\
		raw_local_save_flags(_flags);		\
		raw_irqs_disabled_flags(_flags);	\
	})
#else /* !CONFIG_TRACE_IRQFLAGS_SUPPORT */
/* 架构不支持通用 flags 路径时，退回直接读取架构 IRQ 状态。 */
#define irqs_disabled()	raw_irqs_disabled()
#endif /* CONFIG_TRACE_IRQFLAGS_SUPPORT */

/*
 * irqs_disabled_flags(flags) 解释调用者已有的 flags 快照。它不读取当前
 * CPU，也不改变 IRQ；flags 必须来自匹配的 local/raw save 接口。
 */
#define irqs_disabled_flags(flags) raw_irqs_disabled_flags(flags)

/*
 * scope-based IRQ guard：
 *
 * guard(irq)() 进入作用域时无条件 local_irq_disable()，离开作用域的
 * 正常返回或提前 return 路径自动 local_irq_enable()。它不保存入口状态，
 * 只适合调用契约明确要求入口 IRQ-on、退出恢复 IRQ-on 的区域。
 *
 * guard(irqsave)() 在 guard 对象中保存 unsigned long flags，并在所有
 * 词法作用域出口自动 local_irq_restore(flags)，可正确嵌套在未知入口
 * 状态中。cleanup 属性只保证 C 作用域退出时配对，不会形成跨 CPU 锁，
 * 也不能跨 goto 跳过初始化或把 guard 生命周期延伸到异步回调。
 */
DEFINE_LOCK_GUARD_0(irq, local_irq_disable(), local_irq_enable())
DEFINE_LOCK_GUARD_0(irqsave,
		    local_irq_save(_T->flags),
		    local_irq_restore(_T->flags),
		    unsigned long flags)

#endif
