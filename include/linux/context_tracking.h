/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Context Tracking 公共接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：
 * 本头文件是体系结构入口代码、KVM、调度器、RCU 和告警设施访问
 * Context Tracking 的轻量门面。它不保存状态，也不实现完整状态机；
 * 每 CPU 的状态布局在 context_tracking_state.h，真正的用户态、来宾态和
 * idle 状态转换在 kernel/context_tracking.c。
 *
 * 主要调用链：
 *   返回用户态：exit_to_user_mode()
 *                -> user_enter_irqoff()
 *                -> __ct_user_enter(CT_STATE_USER)
 *   进入内核：  enter_from_user_mode()
 *                -> user_exit_irqoff()
 *                -> __ct_user_exit(CT_STATE_USER)
 *   KVM：       guest_context_enter/exit_irqoff()
 *                -> context_tracking_guest_enter/exit()
 *   异常/调度： exception_enter() -> 内核工作 -> exception_exit()
 *   idle/RCU：  ct_idle_enter/exit() 改变 RCU watching 计数；
 *                warn_rcu_enter/exit() 为诊断路径临时建立可用的 RCU 环境。
 *
 * 核心状态与并发：
 * context_tracking 是每 CPU 对象，调用这些接口不会取得对象引用，也不存在
 * ownership 转移。state 原子量的低位记录 USER/GUEST/KERNEL 等上下文，高位
 * 记录 RCU watching 序列；大多数转换依赖调用者已经禁止中断，确保同一 CPU
 * 上的入口/出口按严格顺序配对。全局 static key 先消除未启用机器上
 * 的热路径
 * 开销，每 CPU active 位再区分当前 CPU 是否真正参与 full dynticks 跟踪。
 *
 * 方案权衡：
 * 内联门面和条件编译 stub 让未启用配置几乎没有运行时成本，也允许
 * 底层入口在不可插桩区域完成状态切换；代价是调用上下文约束严格，
 * 错配一次 enter/exit
 * 就可能让 RCU 把仍会访问临界区的 CPU 误认为处于扩展静止态。
 */
#ifndef _LINUX_CONTEXT_TRACKING_H
#define _LINUX_CONTEXT_TRACKING_H

/*
 * sched/vtime 提供 current 与虚拟 CPU 时间记账；状态头提供每 CPU 状态、
 * static key 和状态编码；instrumentation/ptrace 则服务于不可插桩入口及
 * 体系结构寄存器边界。本文件只组合这些契约，不拥有其中任何对象。
 */
#include <linux/sched.h>
#include <linux/vtime.h>
#include <linux/context_tracking_state.h>
#include <linux/instrumentation.h>

#include <asm/ptrace.h>


#ifdef CONFIG_CONTEXT_TRACKING_USER
/*
 * ct_cpu_track_user() - 在启动期让指定 CPU 参与用户上下文跟踪。
 *
 * 调用关系：NO_HZ/full-dynticks 初始化或强制跟踪初始化调用它；实现会设置
 * 目标 CPU 的 active 位，并增加全局 static key 的启用计数。
 * @cpu: 纯输入的逻辑 CPU 编号，必须对应一个可访问的 per-CPU 区域；
 *       不传递引用或所有权。
 * 上下文：__init 启动阶段调用，不是运行期热插拔同步接口；可能修改
 * init_task 的 TIF_NOHZ 标志。返回：无直接返回值；重复激活同一 CPU
 * 不会重复增加 key。
 */
extern void ct_cpu_track_user(int cpu);

/* Called with interrupts disabled.  */
/*
 * 调用这两个底层状态转换函数时，中断必须已经关闭。这样从检查当前
 * 每 CPU
 * 状态到发布新状态之间不会被同 CPU 的 IRQ 入口打断；实现位于
 * kernel/context_tracking.c，并处在 noinstr 路径。
 *
 * __ct_user_enter()/__ct_user_exit() - 切换用户态或来宾态跟踪状态。
 * @state: 纯输入状态，只应为 CT_STATE_USER 或 CT_STATE_GUEST；enter 表示即将
 *         离开内核，exit 表示刚回到内核，无对象 ownership 变化。
 * 返回：均无直接返回值。active CPU 上会同步 vtime（仅 USER）并进入/退出
 * RCU 扩展静止态；非 active CPU 仍维护状态，以便任务迁移后的异常返回恢复
 * 正确上下文。递归调用由实现的每 CPU recursion 字段抑制。
 */
extern void __ct_user_enter(enum ctx_state state);
extern void __ct_user_exit(enum ctx_state state);

/*
 * ct_user_enter()/ct_user_exit() - 可在中断开启处使用的兼容包装。
 *
 * @state: 与底层版本相同的纯输入状态，无 ownership 转移。
 * 包装内部保存并关闭本地中断后调用 __ct_user_*()，在中断上下文中则直接
 * 返回，避免破坏 dynticks 嵌套计数。它们会恢复原 IRQ 状态、
 * 无直接返回值，
 * 但因 local_irq_restore() 可能触及 tracing/lockdep，当前实现已属过时接口；
 * 新的体系结构入口应优先使用 IRQ-off 版本。
 */
extern void ct_user_enter(enum ctx_state state);
extern void ct_user_exit(enum ctx_state state);

/*
 * user_enter_callable()/user_exit_callable() - 供无法在低层汇编中测试 static key
 * 的旧体系结构调用的无参数符号入口。
 *
 * 入参：无。返回：无直接返回值。它们最终走上述兼容包装，继承其
 * IRQ 保存、
 * 中断上下文短路和过时限制，不应作为新入口代码的首选接口。
 */
extern void user_enter_callable(void);
extern void user_exit_callable(void);

/*
 * user_enter() - 在中断状态不作承诺的旧入口上报告“即将返回用户态”。
 *
 * 入参：无。调用者必须处于当前 CPU 的返回用户态路径；
 * 函数不取得引用、
 * 不返回结果，也不睡眠。全局 static key 未启用时直接结束，否则把固定的
 * CT_STATE_USER 交给会自行保存 IRQ 的 ct_user_enter()。
 */
static inline void user_enter(void)
{
	/*
	 * 先走 static key 快速判断，使普通非 full-dynticks 配置不承担函数调用
	 * 和 IRQ 保存成本；这里判断的是全局是否有 CPU 启用，
	 * 具体 CPU 的 active
	 * 策略由底层转换函数处理。
	 */
	if (context_tracking_enabled())
		ct_user_enter(CT_STATE_USER);

}
/*
 * user_exit() - 在旧入口上报告“已经从用户态进入内核”。
 *
 * 入参：无。返回：无直接返回值。启用时恢复内核态/RCU watching
 * 状态并结束
 * USER vtime 记账；未启用时为空操作。与 user_enter() 一样，它不转移所有权，
 * 新代码应优先使用 user_exit_irqoff()。
 */
static inline void user_exit(void)
{
	if (context_tracking_enabled())
		ct_user_exit(CT_STATE_USER);
}

/* Called with interrupts disabled.  */
/*
 * 下列两个热路径包装要求调用者已经关闭中断。与上面的兼容包装相比，
 * 它们不保存/恢复 IRQ，因此可安全放在体系结构 noinstr 入口的最后
 * 或最前边界。
 */
/*
 * user_enter_irqoff() - 在返回用户态前发布 USER/RCU 扩展静止态。
 *
 * 入参：无。前置条件：本地中断关闭，之后直到真正进入用户态前
 * 不得再使用
 * RCU 读侧临界区。返回：无直接返回值；启用时修改当前 CPU 的 context
 * tracking 与 vtime 状态，未启用时无副作用，不会睡眠或取得引用。
 */
static __always_inline void user_enter_irqoff(void)
{
	if (context_tracking_enabled())
		__ct_user_enter(CT_STATE_USER);

}
/*
 * user_exit_irqoff() - 从用户态进入内核后重新建立可使用 RCU 的状态。
 *
 * 入参：无。前置条件：本地中断关闭，并且必须早于 syscall、
 * 异常处理等任何可能使用 RCU 的高层内核代码。返回：无直接返回值；
 * 启用时退出 USER 状态
 * 并恢复 RCU watching，未启用时为空操作。
 */
static __always_inline void user_exit_irqoff(void)
{
	if (context_tracking_enabled())
		__ct_user_exit(CT_STATE_USER);
}

/*
 * exception_enter() - 为“用户状态记账尚未退出时发生的内核工作”建立临时
 * 内核上下文。
 *
 * 入参：无。主要调用者包括调度、kprobe 和部分体系结构异常路径；
 * 调用者必须
 * 把返回值原样交给 exception_exit()。本函数不睡眠、不取得引用。
 * 返回：原上下文 CT_STATE_USER/CT_STATE_GUEST/CT_STATE_KERNEL；若架构已经
 * 在独立入口栈上完成跟踪，或全局跟踪关闭，则返回数值 0（即
 * CT_STATE_KERNEL）作为“不需要恢复”的哨兵。
 * 副作用：原状态不是 KERNEL 时先调用 ct_user_exit()，使异常处理期间 RCU
 * 可用；该兼容 helper 自行管理 IRQ，并可能在中断上下文中短路。
 */
static inline enum ctx_state exception_enter(void)
{
	/*
	 * prev_ctx 是按值保存的状态令牌，只在配对的 exception_exit()
	 * 前有效。
	 */
	enum ctx_state prev_ctx;

	/*
	 * OFFSTACK 架构由低层入口负责切换，重复操作会错配状态；
	 * 全局 static key 关闭时也没有状态需要保存。返回 KERNEL
	 * 让配对出口自然成为空操作。
	 */
	if (IS_ENABLED(CONFIG_HAVE_CONTEXT_TRACKING_USER_OFFSTACK) ||
	    !context_tracking_enabled())
		return 0;

	/*
	 * 读取当前 CPU 的状态后，仅在确实来自用户/来宾上下文时
	 * 切回内核态；
	 * 已是 KERNEL 表示嵌套异常，无需重复改变 RCU watching 计数。
	 */
	prev_ctx = __ct_state();
	if (prev_ctx != CT_STATE_KERNEL)
		ct_user_exit(prev_ctx);

	return prev_ctx;
}

/*
 * exception_exit() - 根据 exception_enter() 保存的令牌恢复异常前上下文。
 *
 * @prev_ctx: 纯输入状态值，必须来自同一次配对的 exception_enter()；不包含
 *            指针或 ownership。调用者应确保配对期间的迁移/调度语义符合其
 *            路径约束，底层仍会以当前 CPU 的状态完成恢复。
 * 返回：无直接返回值。仅在非 OFFSTACK 架构、全局跟踪仍启用且原状态不是
 * KERNEL 时恢复 USER/GUEST；否则不产生副作用。它必须位于异常内核工作完成
 * 之后，避免过早告诉 RCU 当前 CPU 已进入扩展静止态。
 */
static inline void exception_exit(enum ctx_state prev_ctx)
{
	if (!IS_ENABLED(CONFIG_HAVE_CONTEXT_TRACKING_USER_OFFSTACK) &&
	    context_tracking_enabled()) {
		if (prev_ctx != CT_STATE_KERNEL)
			ct_user_enter(prev_ctx);
	}
}

/*
 * context_tracking_guest_enter() - 在 KVM 运行 vCPU 前进入 GUEST 跟踪状态。
 *
 * 入参：无。调用者必须已经关闭本地中断，并在调用后到
 * guest exit 之前避免
 * RCU、tracing 和 lockdep 等可插桩代码。返回 true 表示当前 CPU 的 active
 * 位已启用，Context Tracking 已承担 RCU 扩展静止态转换；返回 false 表示
 * KVM 还需调用 rcu_virt_note_context_switch() 兼容路径。
 * 即使当前 CPU 非 active，只要全局 static key 开启，底层仍记录 GUEST 状态
 * 以保证跨 CPU 恢复一致；函数不睡眠、不取得引用。
 */
static __always_inline bool context_tracking_guest_enter(void)
{
	if (context_tracking_enabled())
		__ct_user_enter(CT_STATE_GUEST);

	/*
	 * 返回的是“本 CPU 是否 active”，不是刚才是否执行过
	 * 全局状态记录。
	 */
	return context_tracking_enabled_this_cpu();
}

/*
 * context_tracking_guest_exit() - vCPU 退出后从 GUEST 状态恢复内核上下文。
 *
 * 入参：无。应与 guest enter 在 IRQ-off、不可插桩区间配对，并早于任何 RCU
 * 使用。返回值与 enter 相同：true 表示当前 CPU active，false 要求 KVM 执行
 * 兼容的 RCU 虚拟切换通知。启用时修改每 CPU 状态，无 ownership 转移。
 */
static __always_inline bool context_tracking_guest_exit(void)
{
	if (context_tracking_enabled())
		__ct_user_exit(CT_STATE_GUEST);

	return context_tracking_enabled_this_cpu();
}

/*
 * CT_WARN_ON() 只在 Context Tracking 全局启用时检查协议断言，避免关闭配置
 * 对热入口增加无意义告警。@cond 仅在启用分支求值，
 * 调用者不得依赖其副作用；
 * 结果沿用 WARN_ON() 的告警和布尔返回语义。
 */
#define CT_WARN_ON(cond) WARN_ON(context_tracking_enabled() && (cond))

#else
/*
 * CONFIG_CONTEXT_TRACKING_USER=n 的完整编译期替身。
 *
 * user_enter/user_exit 及 IRQ-off 版本：无参数、无返回值、无副作用。
 * exception_enter：无参数，返回 0/KERNEL 哨兵；exception_exit 的 @prev_ctx
 * 为纯输入但被忽略。ct_state/__ct_state：无参数，返回 -1 表示状态未知。
 * guest enter/exit：无参数并返回 false，促使 KVM 使用兼容 RCU 通知。
 * 所有函数均不睡眠、不访问 per-CPU 状态、无引用或 ownership 变化。
 */
static inline void user_enter(void) { }
static inline void user_exit(void) { }
static inline void user_enter_irqoff(void) { }
static inline void user_exit_irqoff(void) { }
static inline int exception_enter(void) { return 0; }
static inline void exception_exit(enum ctx_state prev_ctx) { }
static inline int ct_state(void) { return -1; }
static inline int __ct_state(void) { return -1; }
static __always_inline bool context_tracking_guest_enter(void) { return false; }
static __always_inline bool context_tracking_guest_exit(void) { return false; }
/*
 * 关闭用户上下文跟踪时连条件表达式也不求值；因此 @cond
 * 同样不能包含调用者
 * 期望必须发生的副作用。do/while(0) 保持它作为单条语句使用的接口形状。
 */
#define CT_WARN_ON(cond) do { } while (0)
#endif /* !CONFIG_CONTEXT_TRACKING_USER */
/* 上述分支在未配置用户上下文跟踪时提供零成本替身。 */

#ifdef CONFIG_CONTEXT_TRACKING_USER_FORCE
/*
 * context_tracking_init() - 启动时强制激活所有 possible CPU 的用户跟踪。
 *
 * 入参：无。仅 CONFIG_CONTEXT_TRACKING_USER_FORCE 下存在真实实现，逐 CPU 调用
 * ct_cpu_track_user() 并更新 static key。返回：无直接返回值；副作用持续到
 * 系统运行期，不分配由调用者管理的资源。
 */
extern void context_tracking_init(void);
#else
/*
 * 未强制全 CPU 跟踪时的启动期空实现。
 * 入参：无；返回：无直接返回值；无状态、引用或 ownership 副作用。
 */
static inline void context_tracking_init(void) { }
#endif /* CONFIG_CONTEXT_TRACKING_USER_FORCE */
/*
 * 上述条件只决定是否强制激活所有 CPU，不等同于 USER 跟踪功能
 * 本身是否编译。
 */

#ifdef CONFIG_CONTEXT_TRACKING_IDLE
/*
 * ct_idle_enter()/ct_idle_exit() - idle 循环与 RCU 扩展静止态的边界通知。
 *
 * 入参：均无，不取得引用。enter 要求本地中断已关闭，
 * 声明当前 CPU 不再执行普通 RCU 读侧临界区；exit 在实现内部保存
 * 并关闭中断，重新建立 RCU watching。
 * 返回：均无直接返回值。它们更新每 CPU nesting/state，配对错误会造成 RCU
 * 停滞或过早结束宽限期；实现为 noinstr 且不会睡眠。
 */
extern void ct_idle_enter(void);
extern void ct_idle_exit(void);

/*
 * Is RCU watching the current CPU (IOW, it is not in an extended quiescent state)?
 *
 * Note that this returns the actual boolean data (watching / not watching),
 * whereas ct_rcu_watching() returns the RCU_WATCHING subvariable of
 * context_tracking.state.
 *
 * No ordering, as we are sampling CPU-local information.
 */
/*
 * 判断 RCU 当前是否正在观察本 CPU，也就是本 CPU 是否不在扩展静止态。
 *
 * 原文强调两个细节：返回值是 watching 位的实际真假，而 ct_rcu_watching()
 * 返回的是 state 中可能多位宽的 RCU_WATCHING 子变量；这里仅采样 CPU 本地
 * 信息，因此不提供跨 CPU 的内存顺序保证。
 *
 * 入参：无。调用者必须确保读取期间不会迁移到另一 CPU，
 * 通常已禁止抢占或处于
 * IRQ/noinstr 路径。返回 true 表示可执行 RCU 读侧工作，false 表示 CPU 正处
 * EQS；函数只读借用的 per-CPU 原子量，不取得引用，也不睡眠。
 */
static __always_inline bool rcu_is_watching_curr_cpu(void)
{
	/*
	 * raw_atomic_read 避免插桩并只做本地快照；掩码结果转换为
	 * bool，不能把
	 * 这次无序读取当作观察其他 CPU 数据发布的 acquire 屏障。
	 */
	return raw_atomic_read(this_cpu_ptr(&context_tracking.state)) & CT_RCU_WATCHING;
}

/*
 * Increment the current CPU's context_tracking structure's ->state field
 * with ordering.  Return the new value.
 */
/*
 * 原文说明：按指定增量、以有序方式增加当前 CPU 的 context_tracking.state，
 * 并返回更新后的值。这里的“有序”来自原子 read-modify-write，而不是普通
 * 本地计数器赋值。
 *
 * @incby: 纯输入增量，不是任意计数；正常调用用 CT_RCU_WATCHING 加减上下文
 *         状态，使 watching 序列奇偶翻转并同步低位 ctx_state。告警修复路径
 *         只加 CT_RCU_WATCHING。无单位、无 ownership 变化。
 * 返回：更新后的无符号状态快照。raw_atomic_add_return() 提供有序的原子
 * read-modify-write，供 RCU 把进入/退出 EQS 与前后临界区排序；函数不睡眠。
 */
static __always_inline unsigned long ct_state_inc(int incby)
{
	return raw_atomic_add_return(incby, this_cpu_ptr(&context_tracking.state));
}

/*
 * warn_rcu_enter() - 为可能在 RCU 未 watching 时运行的告警报告临时搭桥。
 *
 * 入参：无。panic/WARN/lockdep 等诊断路径调用它，因为报告过程自身可能依赖
 * RCU。函数用 notrace 方式禁止抢占，把 enter/exit 固定在同一 CPU；若发现
 * 当前 CPU 位于 EQS，就原子推进 watching 序列使 RCU 暂时可用。
 * 返回 true 表示本函数确实修复了状态，调用者必须把该值传给
 * warn_rcu_exit；false 表示原本已 watching。无对象引用、不会睡眠，
 * 但返回前保持抢占关闭。
 */
static __always_inline bool warn_rcu_enter(void)
{
	/*
	 * ret 记录本次是否改变了 RCU 状态，也是配对出口的
	 * 恢复责任令牌。
	 */
	bool ret = false;

	/*
	 * Horrible hack to shut up recursive RCU isn't watching fail since
	 * lots of the actual reporting also relies on RCU.
	 */
	/*
	 * 这是用于压住递归“RCU isn't watching”失败的权宜措施，因为打印和
	 * 诊断错误的许多真实路径本身又依赖 RCU。
	 * 它不修复原始协议错误，只保证
	 * 报告设施有机会输出诊断，并必须由 warn_rcu_exit() 成对撤销。
	 */
	preempt_disable_notrace();
	if (!rcu_is_watching_curr_cpu()) {
		/*
		 * 只在原本未 watching 时取得恢复责任。增加而非写死该位，
		 * 保留 RCU 序列计数的单调推进语义，并通过奇偶位
		 * 把当前快照变为 watching。
		 */
		ret = true;
		ct_state_inc(CT_RCU_WATCHING);
	}

	return ret;
}

/*
 * warn_rcu_exit() - 撤销 warn_rcu_enter() 的临时 RCU 环境并恢复抢占。
 *
 * @rcu: 纯输入的恢复令牌，必须是配对 enter 的返回值；
 *       true 表示需要再次推进
 *       watching 序列回到未 watching，false 表示不得改变原状态。
 * 返回：无直接返回值。无论 @rcu 为何都释放 enter 保持的 preempt-disable；
 * 不取得引用，也不主动等待资源；最后的 preempt_enable_notrace() 恢复抢占
 * 时可以触发一次调度。错配会泄漏抢占禁用或破坏 RCU watching 奇偶性。
 */
static __always_inline void warn_rcu_exit(bool rcu)
{
	/*
	 * 这里再次“加”同一位而不是减去：RCU watching
	 * 是带序列信息的奇偶计数，
	 * 第二次推进既恢复未 watching，也保留发生过边界转换这一历史。
	 */
	if (rcu)
		ct_state_inc(CT_RCU_WATCHING);
	preempt_enable_notrace();
}

#else
/*
 * CONFIG_CONTEXT_TRACKING_IDLE=n 时，idle 通知没有状态可维护：
 * ct_idle_enter/exit 无参数、无返回值和副作用；warn_rcu_enter 无参数并返回
 * false，warn_rcu_exit 的 @rcu 被忽略。它们均不改变抢占状态或 ownership。
 */
static inline void ct_idle_enter(void) { }
static inline void ct_idle_exit(void) { }

static __always_inline bool warn_rcu_enter(void) { return false; }
static __always_inline void warn_rcu_exit(bool rcu) { }
#endif /* !CONFIG_CONTEXT_TRACKING_IDLE */
/* 上述分支在未配置 idle Context Tracking 时提供零成本替身。 */

#endif
