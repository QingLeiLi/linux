// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the core interrupt handling code. Detailed
 * information is available in Documentation/core-api/genericirq.rst
 *
 */
/*
 * 本文件实现通用 IRQ 的核心“事件分派”层，更完整的接口与流控背景
 * 见 Documentation/core-api/genericirq.rst。这里不负责分配 irq_desc，也不直接
 * 决定 edge/level 的 ack、mask、eoi 次序；chip.c 中的流处理器把锁和硬件状态准备好
 * 后，在本文件顺序调用 irqaction 的 primary handler，并按返回值唤醒线程 handler。
 *
 * 学习主线：
 *   1. handle_irq_event() 在 desc->lock 下发布 INPROGRESS，然后临时解锁；
 *   2. __handle_irq_event_percpu() 逐个执行 action->handler，并合并 irqreturn_t；
 *   3. IRQ_WAKE_THREAD 经 __irq_wake_thread() 转换为 RUNTHREAD、oneshot 与活动计数；
 *   4. handle_irq_event_percpu() 最后采集随机性并把结果交给 spurious 检测；
 *   5. 根入口 generic_handle_arch_irq() 则建立整次硬中断的 accounting/regs 边界。
 *
 * 关键并发约束：普通 IRQ 的 action 链由 IRQD_IRQ_INPROGRESS、desc->lock 和释放路径的
 * synchronize_irq() 共同稳定；执行设备 handler 时不能持 desc->lock，否则 handler
 * 调用 IRQ API 很容易自锁。per-CPU 流处理器可直接调用 per-CPU 版本，依赖每 CPU
 * 独立执行和注册生命周期，而不是全局 desc 锁串行化。
 */

#include <linux/irq.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>

#include <asm/irq_regs.h>

#include <trace/events/irq.h>

#include "internals.h"

#ifdef CONFIG_GENERIC_IRQ_MULTI_HANDLER
/*
 * handle_arch_irq 是低级异常入口到具体根中断控制器分派器的只读后初始化钩子。
 * 控制器启动代码通过 set_handle_irq() 只设置一次；启动完成后 __ro_after_init
 * 阻止普通写入。低级入口调用前必须确保它非 NULL，函数指针不携带对象所有权。
 */
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init;
#endif

/**
 * handle_bad_irq - handle spurious and unhandled irqs
 * @desc:      description of the interrupt
 *
 * Handles spurious and unhandled IRQ's. It also prints a debugmessage.
 */
/*
 * 处理虚假或无人接管的 IRQ，并输出调试信息。@desc 是调用期间存活的
 * 非 NULL 描述符借用指针，通常处于 hardirq 且本地中断关闭。函数先打印完整描述符，
 * 再增加当前 CPU 统计，最后让架构 ack_bad_irq() 处置该逻辑号；无返回值、不睡眠。
 *
 * 该函数是尚未安装正确流处理器时的安全占位入口，不会遍历 action，也不会尝试
 * 自动修复控制器配置。统计仍递增，使 /proc/interrupts 能暴露持续到达的坏中断。
 */
void handle_bad_irq(struct irq_desc *desc)
{
	/* irq 是从内嵌 irq_data 取得的值副本，后续架构 ack 使用同一逻辑号。 */
	unsigned int irq = irq_desc_get_irq(desc);

	print_irq_desc(irq, desc);
	kstat_incr_irqs_this_cpu(desc);
	ack_bad_irq(irq);
}
EXPORT_SYMBOL_GPL(handle_bad_irq);

/*
 * Special, empty irq handler:
 */
/*
 * 原文意为“特殊的空 IRQ handler”。@cpl 是未消费的 IRQ 号，@dev_id 是未消费的
 * 设备标识借用指针；函数在 hardirq 中可调用，不睡眠、无副作用，恒返回 IRQ_NONE。
 * 它可作为占位 action，但 IRQ_NONE 会参与共享中断和 spurious 统计，不能伪装成处理成功。
 */
irqreturn_t no_action(int cpl, void *dev_id)
{
	return IRQ_NONE;
}
EXPORT_SYMBOL_GPL(no_action);

/*
 * warn_no_thread() - 对“请求唤醒但没有线程函数”的 action 只告警一次
 *
 * @irq: 当前逻辑 IRQ 号，仅用于诊断文本。
 * @action: 输入输出的非 NULL action 借用指针，注册协议保证调用期间存活。
 *
 * test_and_set_bit() 原子设置 IRQTF_WARNED：首次调用打印警告，后续 CPU 即使并发命中
 * 也直接返回。无返回值，不创建线程、不改变本次 IRQ_WAKE_THREAD 的合并结果；printk
 * 可用于硬中断上下文，但本函数不能睡眠。
 */
static void warn_no_thread(unsigned int irq, struct irqaction *action)
{
	if (test_and_set_bit(IRQTF_WARNED, &action->thread_flags))
		return;

	printk(KERN_WARNING "IRQ %d device %s returned IRQ_WAKE_THREAD "
	       "but no thread function available.", irq, action->name);
}

/*
 * __irq_wake_thread() - 把一次 primary handler 的唤醒请求提交给 IRQ 线程
 *
 * @desc: 输入输出的非 NULL 描述符借用指针；普通路径处于 IRQD_IRQ_INPROGRESS 窗口，
 *        外层已临时释放 desc->lock，显式 irq_wake_thread() 路径则持有该锁。
 * @action: 输入输出的非 NULL action；thread、thread_mask 与 thread_flags 已由
 *          request_threaded_irq() 安装完成，生命周期覆盖本次提交。
 *
 * 成功提交无直接返回值：原子置 RUNTHREAD，合并 oneshot 屏蔽位，增加 threads_active，
 * 再唤醒 action->thread。若线程正在退出或 RUNTHREAD 已置位则不重复计数/唤醒。
 * 函数运行于 hardirq 或 desc 锁保护的显式唤醒路径，不睡眠；RUNTHREAD 位把多次请求
 * 合并成至少一次线程执行，而不是为每个硬件边沿建立独立队列项。
 */
void __irq_wake_thread(struct irq_desc *desc, struct irqaction *action)
{
	/*
	 * In case the thread crashed and was killed we just pretend that
	 * we handled the interrupt. The hardirq handler has disabled the
	 * device interrupt, so no irq storm is lurking.
	 */
	/*
	 * 若 IRQ 线程已经崩溃并进入退出流程，就把本次事件视作已提交而直接
	 * 返回。primary hardirq 已按 oneshot 协议禁用设备中断，因此不会因不再唤醒线程
	 * 形成中断风暴。这里不增加 threads_active，避免等待者永远等不到退出线程递减。
	 */
	if (action->thread->flags & PF_EXITING)
		return;

	/*
	 * Wake up the handler thread for this action. If the
	 * RUNTHREAD bit is already set, nothing to do.
	 */
	/*
	 * 为此 action 唤醒处理线程；若 RUNTHREAD 已置位则无需重复工作。
	 * test_and_set_bit() 同时是并发合并点，只有从 0 改为 1 的调用者继续维护
	 * threads_oneshot/threads_active，保证一次待执行状态只对应一份活动计数。
	 */
	if (test_and_set_bit(IRQTF_RUNTHREAD, &action->thread_flags))
		return;

	/*
	 * It's safe to OR the mask lockless here. We have only two
	 * places which write to threads_oneshot: This code and the
	 * irq thread.
	 *
	 * This code is the hard irq context and can never run on two
	 * cpus in parallel. If it ever does we have more serious
	 * problems than this bitmask.
	 *
	 * The irq threads of this irq which clear their "running" bit
	 * in threads_oneshot are serialized via desc->lock against
	 * each other and they are serialized against this code by
	 * IRQS_INPROGRESS.
	 *
	 * Hard irq handler:
	 *
	 *	spin_lock(desc->lock);
	 *	desc->state |= IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *	set_bit(IRQTF_RUNTHREAD, &action->thread_flags);
	 *	desc->threads_oneshot |= mask;
	 *	spin_lock(desc->lock);
	 *	desc->state &= ~IRQS_INPROGRESS;
	 *	spin_unlock(desc->lock);
	 *
	 * irq thread:
	 *
	 * again:
	 *	spin_lock(desc->lock);
	 *	if (desc->state & IRQS_INPROGRESS) {
	 *		spin_unlock(desc->lock);
	 *		while(desc->state & IRQS_INPROGRESS)
	 *			cpu_relax();
	 *		goto again;
	 *	}
	 *	if (!test_bit(IRQTF_RUNTHREAD, &action->thread_flags))
	 *		desc->threads_oneshot &= ~mask;
	 *	spin_unlock(desc->lock);
	 *
	 * So either the thread waits for us to clear IRQS_INPROGRESS
	 * or we are waiting in the flow handler for desc->lock to be
	 * released before we reach this point. The thread also checks
	 * IRQTF_RUNTHREAD under desc->lock. If set it leaves
	 * threads_oneshot untouched and runs the thread another time.
	 */
	/*
	 * 原文证明这里可无锁 OR threads_oneshot。该字段只有 hardirq 提交端置位、IRQ
	 * 线程清位；同一普通 IRQ 的 hardirq 由 IRQD_IRQ_INPROGRESS 保证不会在两颗 CPU
	 * 同时执行。线程清位时持 desc->lock，并先等待 INPROGRESS 消失；反过来，流处理器
	 * 在再次进入本段前也要经过 desc->lock。因此线程要么等 hardirq 完成发布，要么
	 * hardirq 等线程退出锁区，不会发生丢失位。线程还会在锁下复查 RUNTHREAD：若新
	 * 请求已到达，就保留 oneshot 位并再执行一轮。
	 *
	 * 这里使用按位 OR 而非赋值，因为共享 IRQ 的每个 threaded action 有不同
	 * thread_mask；只要任一线程仍待运行，流处理器就必须保持硬件线路 masked。
	 */
	desc->threads_oneshot |= action->thread_mask;

	/*
	 * We increment the threads_active counter in case we wake up
	 * the irq thread. The irq thread decrements the counter when
	 * it returns from the handler or in the exit path and wakes
	 * up waiters which are stuck in synchronize_irq() when the
	 * active count becomes zero. synchronize_irq() is serialized
	 * against this code (hard irq handler) via IRQS_INPROGRESS
	 * like the finalize_oneshot() code. See comment above.
	 */
	/*
	 * 只有实际把 IRQ 线程从“无待办”转为“有待办”时才增加活动计数。
	 * 线程执行完成或异常退出会递减；归零时唤醒卡在 synchronize_irq() 的等待者。
	 * INPROGRESS 与 finalize_oneshot() 相同的串行协议，保证释放路径不会在递增尚未
	 * 可见时误判为零。计数描述待执行/执行中的线程批次，不是 action 总数。
	 */
	atomic_inc(&desc->threads_active);

	/*
	 * This might be a premature wakeup before the thread reached the
	 * thread function and set the IRQTF_READY bit. It's waiting in
	 * kthread code with state UNINTERRUPTIBLE. Once it reaches the
	 * thread function it waits with INTERRUPTIBLE. The wakeup is not
	 * lost in that case because the thread is guaranteed to observe
	 * the RUN flag before it goes to sleep in wait_for_interrupt().
	 */
	/*
	 * 创建阶段可能在线程进入正式 thread_fn、设置 IRQTF_READY 之前过早
	 * 唤醒它。此时 kthread 核心使用 UNINTERRUPTIBLE，而本调用只唤醒
	 * TASK_INTERRUPTIBLE；即便没有立即唤醒，线程在 wait_for_interrupt() 入睡前必然
	 * 观察已发布的 RUNTHREAD，故事件不会丢失。已就绪线程则由该 wakeup 正常唤醒。
	 */
	wake_up_state(action->thread, TASK_INTERRUPTIBLE);
}

/*
 * irqhandler_duration_check_enabled 是默认关闭的 jump-label 开关。未提供启动参数时，
 * action 热路径只付出一次可被静态分支消除的判断，不读取时钟，也不改变 handler 时序。
 */
static DEFINE_STATIC_KEY_FALSE(irqhandler_duration_check_enabled);
/*
 * irqhandler_duration_threshold_ns 保存启动参数换算后的纳秒阈值。它只在 __init 解析器
 * 中写入，随后受 __ro_after_init 保护；启用静态键后所有 CPU 只读该值。
 */
static u64 irqhandler_duration_threshold_ns __ro_after_init;

/*
 * irqhandler_duration_check_setup() - 解析 primary IRQ handler 耗时告警阈值
 *
 * @arg: __setup 框架借用的非 NULL 数字字符串，单位为微秒，允许 kstrtoul() 接受的
 *       基数前缀；字符串只在启动参数解析期间有效。
 *
 * 合法且非零时，把阈值换算为纳秒、启用静态键并返回 1 表示参数已消费。解析失败或
 * 数值为零时打印错误并返回 0，不启用检测。函数仅在单线程启动期运行、不需锁；它
 * 未对 val * 1000 做饱和处理，命令行应提供能在 unsigned long 中安全换算的合理值。
 */
static int __init irqhandler_duration_check_setup(char *arg)
{
	/* val 接收解析后的微秒数；ret 只传递 kstrtoul() 的 0/负 errno 结果。 */
	unsigned long val;
	int ret;

	ret = kstrtoul(arg, 0, &val);
	if (ret) {
		pr_err("Unable to parse irqhandler.duration_warn_us setting: ret=%d\n", ret);
		return 0;
	}

	if (!val) {
		pr_err("Invalid irqhandler.duration_warn_us setting, must be > 0\n");
		return 0;
	}

	irqhandler_duration_threshold_ns = val * 1000;
	static_branch_enable(&irqhandler_duration_check_enabled);

	return 1;
}
/* 将 irqhandler.duration_warn_us=<微秒> 注册为早期内核命令行设置项。 */
__setup("irqhandler.duration_warn_us=", irqhandler_duration_check_setup);

/*
 * irqhandler_duration_check() - 检查一个 primary handler 是否超过配置阈值
 *
 * @ts_start: 调用 handler 前取得的 local_clock() 纳秒时间戳。
 * @irq: 当前逻辑 IRQ 号，用于告警定位。
 * @action: 只读非 NULL action 借用指针；handler 函数指针用于符号化输出。
 *
 * 调用者仅在静态键启用时进入。函数在 hardirq、本地中断关闭状态运行，不睡眠；
 * 计算本 CPU 本地时钟差，超过阈值时按速率限制打印耗时微秒数。无返回值，也不会
 * 禁用慢 handler 或改变 IRQ 状态；它是诊断机制，额外的时钟读取本身会增加少量开销。
 */
static inline void irqhandler_duration_check(u64 ts_start, unsigned int irq,
					     const struct irqaction *action)
{
	/* 无符号差适配正常的 u64 时钟回绕；同一 CPU hardirq 窗口避免跨 CPU 时钟比较。 */
	u64 delta_ns = local_clock() - ts_start;

	if (unlikely(delta_ns > irqhandler_duration_threshold_ns)) {
		pr_warn_ratelimited("[CPU%u] long duration of IRQ[%u:%ps], took: %llu us\n",
				    smp_processor_id(), irq, action->handler,
				    div_u64(delta_ns, NSEC_PER_USEC));
	}
}

/*
 * __handle_irq_event_percpu() - 顺序运行描述符上的全部 primary irqaction
 *
 * @desc: 输入输出的非 NULL 描述符借用指针。普通 edge/level/fasteoi 路径已设置
 *        IRQD_IRQ_INPROGRESS 并释放 desc->lock；per-CPU 路径允许不同 CPU 并发进入，
 *        但依靠固定注册生命周期稳定只读 action 链，而不依赖全局 INPROGRESS 串行。
 *
 * 函数在 hardirq 且本地中断关闭的上下文运行，绝不能睡眠。它按链表顺序调用每个
 * action->handler(irq, dev_id)，发出 tracepoint，检查 handler 是否错误打开本地中断，
 * 并把 IRQ_WAKE_THREAD 转成线程提交。返回所有 handler irqreturn_t 的按位 OR：
 * IRQ_NONE 表示无人声称处理，IRQ_HANDLED/IRQ_WAKE_THREAD 任一出现都会保留相应位。
 *
 * 本层不采集随机性、不做 spurious 判定，也不 ack/eoi 控制器；这些分别由外层
 * handle_irq_event_percpu() 和具体流处理器完成。handler 可操作设备寄存器并唤醒线程，
 * 但不得释放当前 action；free_irq() 会等待 INPROGRESS/IRQ 线程协议完成。
 */
irqreturn_t __handle_irq_event_percpu(struct irq_desc *desc)
{
	/* retval 是整条共享 action 链的位集合；irq 是循环期间稳定的逻辑号值副本。 */
	irqreturn_t retval = IRQ_NONE;
	unsigned int irq = desc->irq_data.irq;
	/* action 是当前链节点的借用指针，只能在 IRQ 注册生命周期保护范围内使用。 */
	struct irqaction *action;

	for_each_action_of_desc(desc, action) {
		/* res 只描述当前 primary handler 的结果，循环末尾再并入总结果。 */
		irqreturn_t res;

		/*
		 * If this IRQ would be threaded under force_irqthreads, mark it so.
		 */
		/*
		 * 若启用强制 IRQ 线程化后此 action 会被线程化，就通知 lockdep
		 * 当前 hardirq 逻辑应按 threaded handler 建模。NO_THREAD、PERCPU、ONESHOT
		 * action 不走这一标记；该调用只影响锁依赖验证，不改变实际 action 标志。
		 */
		if (irq_settings_can_thread(desc) &&
		    !(action->flags & (IRQF_NO_THREAD | IRQF_PERCPU | IRQF_ONESHOT)))
			lockdep_hardirq_threaded();

		/* entry/exit tracepoint 包围驱动回调，观察者可关联 action 与返回值。 */
		trace_irq_handler_entry(irq, action);

		if (static_branch_unlikely(&irqhandler_duration_check_enabled)) {
			/* 只有显式启用诊断时才读取两次 local_clock()，保持默认热路径精简。 */
			u64 ts_start = local_clock();

			res = action->handler(irq, action->dev_id);
			irqhandler_duration_check(ts_start, irq, action);
		} else {
			res = action->handler(irq, action->dev_id);
		}

		trace_irq_handler_exit(irq, action, res);

		/*
		 * primary handler 契约要求返回时本地硬中断仍关闭。违约会破坏流处理器的
		 * 锁/ack 时序；这里告警后强制恢复关闭状态，让后续 action 和退出路径仍在
		 * 预期上下文运行。WARN_ONCE 是全调用点一次，不是每个 action 一次。
		 */
		if (WARN_ONCE(!irqs_disabled(),"irq %u handler %pS enabled interrupts\n",
			      irq, action->handler))
			local_irq_disable();

		switch (res) {
		case IRQ_WAKE_THREAD:
			/*
			 * Catch drivers which return WAKE_THREAD but
			 * did not set up a thread function
			 */
			/*
			 * 捕获返回 WAKE_THREAD 却没有注册 thread_fn 的驱动。该错误
			 * 只告警且跳过唤醒；res 仍并入 retval，便于 spurious 逻辑看到驱动的
			 * 原始声明，而不是在核心层伪造 IRQ_NONE 或 IRQ_HANDLED。
			 */
			if (unlikely(!action->thread_fn)) {
				warn_no_thread(irq, action);
				break;
			}

			__irq_wake_thread(desc, action);
			break;

		default:
			/* 普通收尾中，IRQ_NONE/IRQ_HANDLED 无线程动作，异常位组合由 note_interrupt() 诊断。 */
			break;
		}

		/* 共享 IRQ 不能让后一个 action 覆盖前一个 action 已声明的处理状态。 */
		retval |= res;
	}

	return retval;
}

/*
 * handle_irq_event_percpu() - 在 action 分派后完成通用统计与异常检测
 *
 * @desc: 与 __handle_irq_event_percpu() 相同的非 NULL、存活描述符借用指针；调用者
 *        已建立 hardirq/本地中断关闭条件，但此函数本身不要求持 desc->lock。
 *
 * 返回共享 action 链的合并 irqreturn_t。无论 handler 是否声称处理，都会把该 IRQ
 * 事件交给随机子系统采样；若描述符未设置 IRQ_NO_DEBUG 跳过策略，
 * note_interrupt() 再更新未处理计数、检查非法返回值，并可能触发 spurious IRQ 防护。
 * 普通路径调用时 IRQD_IRQ_INPROGRESS 仍保持置位，保护这段无 desc 锁的分析；
 * per-CPU IRQ 可在不同 CPU 并行进入，若属于 IPI 等不适合 runaway 检测的线路，
 * 注册者应按接口约定同时提供 IRQF_NO_DEBUG，使这里跳过共享诊断状态。
 * 函数不可睡眠；随机性接口自行决定可记账的熵，不等于每次中断都增加固定熵信用。
 */
irqreturn_t handle_irq_event_percpu(struct irq_desc *desc)
{
	/* retval 在 action 执行、随机性采样和 spurious 记账之间保持原始合并结果。 */
	irqreturn_t retval;

	retval = __handle_irq_event_percpu(desc);

	/* 采样事件时序；该调用不表示设备数据本身被直接加入随机池。 */
	add_interrupt_randomness(desc->irq_data.irq);

	/* NO_DEBUG IRQ 跳过虚假中断启发式，适合由调用者另行保证语义的特殊线路。 */
	if (!irq_settings_no_debug(desc))
		note_interrupt(desc, retval);
	return retval;
}

/*
 * handle_irq_event() - 在普通 IRQ 的描述符锁协议内运行 action 链
 *
 * @desc: 输入输出的非 NULL 描述符借用指针。调用者必须持有 desc->lock，且通常位于
 *        hardirq、本地中断关闭状态；函数返回时再次持有同一把锁，不改变调用者的
 *        本地 IRQ 保存值。调用者不能把中间解锁窗口误认为描述符配置可被任意释放。
 *
 * 进入时清除旧 IRQS_PENDING 并置 IRQD_IRQ_INPROGRESS，然后释放 raw lock，使设备
 * primary handler 可调用不会睡眠的 IRQ/设备操作而不自锁。执行和收尾完成后重新加锁、
 * 清 INPROGRESS 并返回合并 irqreturn_t。期间到达的同一 IRQ 可由具体流处理器重新置
 * PENDING，供 edge/fasteoi 循环或 resend 路径在本函数返回后处理。
 */
irqreturn_t handle_irq_event(struct irq_desc *desc)
{
	/* ret 跨越无锁 handler 窗口，最终在恢复 desc->lock 后交给流处理器。 */
	irqreturn_t ret;

	/* 旧 pending 已由本轮消费；新到达事件会在 INPROGRESS 可见期间重新发布 pending。 */
	desc->istate &= ~IRQS_PENDING;
	irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	/* 只释放 raw lock，不打开本地中断；返回前必须在同一 CPU/上下文重新取得。 */
	raw_spin_unlock(&desc->lock);

	ret = handle_irq_event_percpu(desc);

	/* 与 action 释放者及并发流处理路径重新汇合，再撤销“handler 正在执行”状态。 */
	raw_spin_lock(&desc->lock);
	irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	return ret;
}

#ifdef CONFIG_GENERIC_IRQ_MULTI_HANDLER
/*
 * set_handle_irq() - 一次性安装架构根 IRQ 分派函数
 *
 * @handle_irq: 非 NULL 函数指针，接收低级入口提供的 pt_regs；代码存活期覆盖内核运行。
 *
 * 根 irqchip 在 __init 阶段调用。首次安装写入 handle_arch_irq 并返回 0；已有非 NULL
 * handler 时返回 -EBUSY 且保留原值。无锁设计依赖启动期串行和发布后只读；本函数不
 * 验证 NULL，因此调用者必须满足非 NULL 前置条件，否则后续根入口会解引用空指针。
 */
int __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
	/* 非 NULL 旧值表示根控制器分派所有权已经被先到的 irqchip 占用。 */
	if (handle_arch_irq)
		return -EBUSY;

	handle_arch_irq = handle_irq;
	return 0;
}

/**
 * generic_handle_arch_irq - root irq handler for architectures which do no
 *                           entry accounting themselves
 * @regs:	Register file coming from the low-level handling code
 */
/*
 * 给未自行完成中断入口记账的架构提供根 IRQ handler；@regs 是低级异常
 * 代码构造、仅在本次入口有效的寄存器帧借用指针。调用者已关闭本地硬中断并保证
 * handle_arch_irq 已安装。函数无直接返回值，不睡眠。
 *
 * irq_enter()/irq_exit() 建立并撤销 hardirq 计数、RCU/软中断等通用边界；
 * set_irq_regs() 把当前 CPU 的寄存器指针临时切换为 @regs，并在控制器分派返回后恢复
 * old_regs，使嵌套或外层诊断仍看到正确帧。handle_arch_irq() 在这个窗口解析根控制器
 * pending 状态并进入 domain/desc 流处理。noinstr 防止入口本身被常规编译器插桩递归。
 */
asmlinkage void noinstr generic_handle_arch_irq(struct pt_regs *regs)
{
	/* old_regs 是当前 CPU 先前 irq-regs 借用值，只用于本函数末尾精确恢复。 */
	struct pt_regs *old_regs;

	/* 顺序不可交换：先声明进入 hardirq，再发布本次寄存器帧并调用架构分派器。 */
	irq_enter();
	old_regs = set_irq_regs(regs);
	handle_arch_irq(regs);
	/* 在 irq_exit() 可能处理待执行 softirq 前，恢复外层寄存器观察上下文。 */
	set_irq_regs(old_regs);
	irq_exit();
}
#endif
/* 关闭 CONFIG_GENERIC_IRQ_MULTI_HANDLER 时，架构必须提供自己的根入口/分派机制。 */
