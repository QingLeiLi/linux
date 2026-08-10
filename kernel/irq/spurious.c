// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2004 Linus Torvalds, Ingo Molnar
 *
 * This file contains spurious interrupt handling.
 */
/*
 * 原文说明：本文件实现杂散/无人处理 IRQ 的检测、诊断与恢复。
 *
 * 正常硬中断结束时 note_interrupt() 汇总整条共享 action 链的返回值；它以 100000 次窗口
 * 判断线路是否近乎持续无人处理，必要时增加 disable depth、关闭 IRQ，并启动 10Hz 定时
 * 轮询。irqfixup/irqpoll 模式还会把疑似误路由事件交给其他共享 handler 试处理。轮询只
 * 是兼容和自愈机制，不能替代驱动正确 ack 设备及配置中断路由。
 *
 * 并发要点：硬中断在同一 desc 上不可重入，desc->lock 保护 action/istate；全局原子量
 * 只允许一个 CPU 扫描全部 IRQ。线程 handler 的结果通过 atomic threads_handled 计数延迟
 * 到下一次 hardirq 统一判断，避免在线程上下文破坏共享线路的复合判定。
 */

#include <linux/jiffies.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/moduleparam.h>
#include <linux/timer.h>

#include "internals.h"

/*
 * irqfixup 控制误路由恢复强度：0 关闭；1 仅在 IRQ_NONE 时扫描其他共享 IRQ；2（irqpoll）
 * 还对 IRQ0 或标记 IRQF_IRQPOLL 的已处理事件扫描。启动后很少写、热路径频繁读。
 */
static int irqfixup __read_mostly;

/* 被杂散检测关闭的线路每 HZ/10（约 100ms）轮询一次，给设备恢复和重新识别的机会。 */
#define POLL_SPURIOUS_IRQ_INTERVAL (HZ/10)
/* 定时器回调前置声明；timer 对象在静态初始化时引用它。 */
static void poll_spurious_irqs(struct timer_list *unused);
/* 全局恢复定时器按需启动，之后由回调持续重新排期。 */
static DEFINE_TIMER(poll_spurious_irq_timer, poll_spurious_irqs);
/* 当前全局 IRQ 扫描所在 CPU；chip flow 用它识别轮询导致的递归进入。 */
int irq_poll_cpu;
/* 全局扫描互斥计数：只有 atomic 增加后观察到 1 的 CPU 执行扫描。 */
static atomic_t irq_poll_active;

/*
 * Recovery handler for misrouted interrupts.
 */
/*
 * 尝试让一个候选 IRQ 的共享 handlers 处理可能误路由的事件。
 * @desc: 扫描器借用的存活描述符。
 * @force: false 跳过 disabled IRQ；true 允许定时恢复器轮询因 spurious 被禁用的线路。
 *
 * 函数取得 desc->lock，排除 per-CPU、nested-thread、显式 POLLED、非共享、timer 以及无
 * action 的线路。若该 IRQ 已在其他 CPU 执行，只置 PENDING 请对方循环再查。否则设置
 * POLL_INPROGRESS，反复调用 handle_irq_event() 直到没有 PENDING 或 action 被移除，最后
 * 清状态。任一次返回 IRQ_HANDLED 就返回 true；无适合处理者返回 false。调用者处于本地
 * IRQ 关闭的原子上下文，handler 必须满足 hardirq 约束。
 */
static bool try_one_irq(struct irq_desc *desc, bool force)
{
	/* action 在 desc 锁/handle_irq_event 协议下借用；ret 累积本轮是否有人处理。 */
	struct irqaction *action;
	bool ret = false;

	guard(raw_spinlock)(&desc->lock);

	/*
	 * PER_CPU, nested thread interrupts and interrupts explicitly
	 * marked polled are excluded from polling.
	 */
	/* per-CPU、嵌套线程和明确标成由其他机制轮询的 IRQ 不参与本扫描。 */
	if (irq_settings_is_per_cpu(desc) || irq_settings_is_nested_thread(desc) ||
	    irq_settings_is_polled(desc))
		return false;

	/*
	 * Do not poll disabled interrupts unless the spurious
	 * disabled poller asks explicitly.
	 */
	/* 普通误路由修复不碰 disabled IRQ；只有 spurious 恢复定时器可强制尝试。 */
	if (irqd_irq_disabled(&desc->irq_data) && !force)
		return false;

	/*
	 * All handlers must agree on IRQF_SHARED, so we test just the
	 * first.
	 */
	/* 共享兼容性由 setup 保证所有 action 一致，所以只检查链首 SHARED。 */
	action = desc->action;
	if (!action || !(action->flags & IRQF_SHARED) || (action->flags & __IRQF_TIMER))
		return false;

	/* Already running on another processor */
	/* 描述符正在另一 CPU 的 flow handler 中执行，不能并发直接调用 action。 */
	if (irqd_irq_inprogress(&desc->irq_data)) {
		/*
		 * Already running: If it is shared get the other
		 * CPU to go looking for our mystery interrupt too
		 */
		/*
		 * 共享 IRQ 已在执行时，设置 PENDING，让持有该 desc 的 CPU 在退出前也为
		 * 这次“来源不明”的事件再遍历一次 handler。
		 */
		desc->istate |= IRQS_PENDING;
		return false;
	}

	/* Mark it poll in progress */
	/* 标记人为轮询，防止 note_interrupt() 把轮询结果再次计入杂散统计。 */
	desc->istate |= IRQS_POLL_INPROGRESS;
	do {
		if (handle_irq_event(desc) == IRQ_HANDLED)
			ret = true;
		/* Make sure that there is still a valid action */
		/* handler 执行会暂时放锁，回来后重新读取 action，兼容并发注销。 */
		action = desc->action;
	} while ((desc->istate & IRQS_PENDING) && action);
	desc->istate &= ~IRQS_POLL_INPROGRESS;
	return ret;
}

/*
 * misrouted_irq() - 扫描其他 IRQ，寻找可能真正对应当前事件的共享 handler
 *
 * @irq: 已由正常 flow 尝试过的逻辑号，扫描时跳过。
 * irq_poll_active 保证全系统仅一名扫描者；竞争失败者不扫描并返回 0。获胜者记录当前
 * CPU，遍历除 IRQ0 和 @irq 外的描述符，以 force=false 调 try_one_irq()。任一候选处理
 * 成功返回 1，否则 0；所有出口配平原子计数。调用于 hardirq 路径，不睡眠，开销可达
 * 全 IRQ 空间，因此只有 irqfixup/irqpoll 显式启用时使用。
 */
static int misrouted_irq(int irq)
{
	struct irq_desc *desc;
	int i, ok = 0;

	if (atomic_inc_return(&irq_poll_active) != 1)
		goto out;

	irq_poll_cpu = smp_processor_id();

	for_each_irq_desc(i, desc) {
		if (!i)
			 continue;

		/* 当前 @irq 的正常 action 链已经尝试过，不在误路由扫描中重复执行。 */
		if (i == irq)	/* Already tried */
			continue;

		if (try_one_irq(desc, false))
			ok = 1;
	}
out:
	atomic_dec(&irq_poll_active);
	/* So the caller can adjust the irq error counts */
	/* 布尔式返回值让调用者在找到处理者时修正原 IRQ 的 unhandled 计数。 */
	return ok;
}

/*
 * poll_spurious_irqs() - 周期试运行被杂散检测关闭的共享 IRQ
 *
 * @unused: 定时器接口参数，未消费。
 * 与误路由扫描共用 irq_poll_active，已有扫描时跳过本轮。获胜者遍历 desc，使用无锁
 * READ_ONCE 快照筛选 SPURIOUS_DISABLED；命中后本地关 IRQ，以 force=true 调 try_one_irq。
 * 扫描结束配平活动计数，并无条件把定时器排到约 100ms 后。函数在 timer softirq 上下文
 * 运行，不睡眠；即使状态并发变化，try_one_irq 的锁内复核仍保证安全。
 */
static void poll_spurious_irqs(struct timer_list *unused)
{
	struct irq_desc *desc;
	int i;

	if (atomic_inc_return(&irq_poll_active) != 1)
		goto out;
	irq_poll_cpu = smp_processor_id();

	for_each_irq_desc(i, desc) {
		unsigned int state;

		if (!i)
			 continue;

		/* Racy but it doesn't matter */
		/* 这里只作廉价筛选，竞态最多导致多试/少试一轮，不影响锁内正确性。 */
		state = READ_ONCE(desc->istate);
		if (!(state & IRQS_SPURIOUS_DISABLED))
			continue;

		local_irq_disable();
		try_one_irq(desc, true);
		local_irq_enable();
	}
out:
	atomic_dec(&irq_poll_active);
	mod_timer(&poll_spurious_irq_timer, jiffies + POLL_SPURIOUS_IRQ_INTERVAL);
}

/*
 * bad_action_ret() - 校验 action 链汇总返回值是否属于 irqreturn_t 合法位组合
 *
 * @action_ret: handle_irq_event() 汇总结果。IRQ_NONE=0，合法非零位仅 HANDLED 与
 * WAKE_THREAD，因此无符号值不大于两者 OR 的最大值时返回 0，否则返回 1。该检查只识别
 * 越界/未知位，不改变结果，不睡眠。
 */
static inline int bad_action_ret(irqreturn_t action_ret)
{
	unsigned int r = action_ret;

	if (likely(r <= (IRQ_HANDLED | IRQ_WAKE_THREAD)))
		return 0;
	return 1;
}

/*
 * If 99,900 of the previous 100,000 interrupts have not been handled
 * then assume that the IRQ is stuck in some manner. Drop a diagnostic
 * and try to turn the IRQ off.
 *
 * (The other 100-of-100,000 interrupts may have been a correctly
 *  functioning device sharing an IRQ with the failing one)
 */
/*
 * 若最近 100000 次中有 99900 次无人处理，就认为 IRQ 可能卡死，打印诊断并
 * 尝试关闭；剩余约 100 次可能来自共享线路上仍正常的设备。
 *
 * __report_bad_irq() - 无频率限制地打印一次坏返回值/无人处理诊断
 * @desc: 正在诊断的存活描述符。
 * @action_ret: 本次 action 汇总返回值，用于区分 bogus value 与 nobody cared。
 * 函数打印栈和所有 action 的 primary/thread 函数。note_interrupt() 入口没有 desc 锁，
 * 因 action 可被 free 修改，遍历前取得 irqsave 锁；当前 flow 已标 INPROGRESS，与
 * synchronize_irq() 的释放协议共同保证节点存活。无返回值，可从 hardirq 上下文调用。
 */
static void __report_bad_irq(struct irq_desc *desc, irqreturn_t action_ret)
{
	unsigned int irq = irq_desc_get_irq(desc);
	struct irqaction *action;

	if (bad_action_ret(action_ret))
		pr_err("irq event %d: bogus return value %x\n", irq, action_ret);
	else
		pr_err("irq %d: nobody cared (try booting with the \"irqpoll\" option)\n", irq);
	dump_stack();
	pr_err("handlers:\n");

	/*
	 * We need to take desc->lock here. note_interrupt() is called
	 * w/o desc->lock held, but IRQ_PROGRESS set. We might race
	 * with something else removing an action. It's ok to take
	 * desc->lock here. See synchronize_irq().
	 */
	/*
	 * note_interrupt() 调用时未持 desc->lock，但 IRQ 已标 INPROGRESS；并发方
	 * 可能摘 action，所以这里允许/必须加锁读取链。free 会通过 synchronize_irq() 等待本
	 * 次 INPROGRESS 结束，故锁内借用 action 安全。
	 */
	guard(raw_spinlock_irqsave)(&desc->lock);
	for_each_action_of_desc(desc, action) {
		pr_err("[<%p>] %ps", action->handler, action->handler);
		if (action->thread_fn)
			pr_cont(" threaded [<%p>] %ps", action->thread_fn, action->thread_fn);
		pr_cont("\n");
	}
}

/*
 * report_bad_irq() - 对立即发生的坏 action 返回值做全局限流诊断
 *
 * @desc/@action_ret 原样交给 __report_bad_irq()。静态预算初值 100，只打印前 100 次，
 * 避免错误 handler 以中断频率淹没日志；计数仅在硬中断调用路径使用，精确并发计数不是
 * 正确性条件。无返回值，不负责禁用线路。
 */
static void report_bad_irq(struct irq_desc *desc, irqreturn_t action_ret)
{
	static int count = 100;

	if (count > 0) {
		count--;
		__report_bad_irq(desc, action_ret);
	}
}

/*
 * try_misrouted_irq() - 判断本次事件是否应触发全局误路由扫描
 *
 * @irq/@desc: 当前逻辑号及存活描述符。
 * @action_ret: 当前 action 链的汇总结果。
 * irqfixup=0 返回 false；任何 IRQ_NONE 在模式 1/2 下返回 true。模式 2 还允许传统 PC
 * timer IRQ0，或链首带 IRQF_IRQPOLL 的已处理 IRQ。函数不持 desc 锁，只用 READ_ONCE
 * 获取 action 快照，因此结果仅是扫描提示，不能解引用到调用外或充当生命周期证明。
 */
static inline bool try_misrouted_irq(unsigned int irq, struct irq_desc *desc,
				     irqreturn_t action_ret)
{
	struct irqaction *action;

	if (!irqfixup)
		return false;

	/* We didn't actually handle the IRQ - see if it was misrouted? */
	/* 当前线路没人处理时，尝试判断事件是否被路由到了错误 IRQ。 */
	if (action_ret == IRQ_NONE)
		return true;

	/*
	 * But for 'irqfixup == 2' we also do it for handled interrupts if
	 * they are marked as IRQF_IRQPOLL (or for irq zero, which is the
	 * traditional PC timer interrupt.. Legacy)
	 */
	/*
	 * irqfixup==2 时，即使已处理，也对 IRQF_IRQPOLL action 或传统 PC 定时器
	 * IRQ0 执行扫描，以持续轮询需要兼容的旧硬件。
	 */
	if (irqfixup < 2)
		return false;

	if (!irq)
		return true;

	/*
	 * Since we don't get the descriptor lock, "action" can
	 * change under us.
	 */
	/* 这里未持 desc 锁，action 指针可能并发变化，只能做一次 READ_ONCE 快照。 */
	action = READ_ONCE(desc->action);
	return action && (action->flags & IRQF_IRQPOLL);
}

/* threads_handled_last 的最高位用作“上一轮线程结果尚待判定”标志，低位保存处理计数快照。 */
#define SPURIOUS_DEFERRED	0x80000000

/*
 * note_interrupt() - 记录一次 IRQ action 汇总结果并执行杂散/误路由检测
 *
 * @desc: 当前 flow handler 正在处理且 INPROGRESS 的描述符。
 * @action_ret: 所有 primary handler 的按位汇总，可能含 HANDLED/WAKE_THREAD。
 * 人为 poll 或显式 POLLED IRQ 不统计；非法返回立即限流报告。含线程唤醒时，若没有 primary
 * 直接 HANDLED，就借 SPURIOUS_DEFERRED 和 atomic threads_handled 把“本次是否处理”延后
 * 到下一次 hardirq 判断。最终 IRQ_NONE 更新短时间连续 unhandled 计数，可选扫描误路由。
 * 每累计 100000 个需要观察的事件，若超过 99900 个 unhandled，就诊断、增加 depth、禁用
 * 线路并启动恢复定时器。函数在 hardirq/不可重入 desc 上运行，不睡眠；无返回值。
 */
void note_interrupt(struct irq_desc *desc, irqreturn_t action_ret)
{
	/* irq 在需要误路由或最终禁用时才从 desc 读取。 */
	unsigned int irq;

	if (desc->istate & IRQS_POLL_INPROGRESS || irq_settings_is_polled(desc))
		return;

	if (bad_action_ret(action_ret)) {
		report_bad_irq(desc, action_ret);
		return;
	}

	/*
	 * We cannot call note_interrupt from the threaded handler
	 * because we need to look at the compound of all handlers
	 * (primary and threaded). Aside of that in the threaded
	 * shared case we have no serialization against an incoming
	 * hardware interrupt while we are dealing with a threaded
	 * result.
	 *
	 * So in case a thread is woken, we just note the fact and
	 * defer the analysis to the next hardware interrupt.
	 *
	 * The threaded handlers store whether they successfully
	 * handled an interrupt and we check whether that number
	 * changed versus the last invocation.
	 *
	 * We could handle all interrupts with the delayed by one
	 * mechanism, but for the non forced threaded case we'd just
	 * add pointless overhead to the straight hardirq interrupts
	 * for the sake of a few lines less code.
	 */
	/*
	 * 不能从 threaded handler 直接调用本函数，因为判定必须观察共享线路上所有
	 * primary+thread 的复合结果；共享线程运行时也无法与下一次硬件 IRQ 串行。因此本次若
	 * 唤醒线程，只记录“待判定”，到下一次 hardirq 比较 threads_handled 是否变化。所有
	 * 事件都可统一延迟一轮，但非 forced-thread 的纯 hardirq 会为少量代码复用付出无意义
	 * 热路径开销，所以只对确有线程的情况延迟。
	 */
	if (action_ret & IRQ_WAKE_THREAD) {
		/*
		 * There is a thread woken. Check whether one of the
		 * shared primary handlers returned IRQ_HANDLED. If
		 * not we defer the spurious detection to the next
		 * interrupt.
		 */
		/*
		 * 已有线程被唤醒；先看共享 primary 中是否有人同时返回 HANDLED。若没有，
		 * 当前无法知道线程结果，把杂散判定延后到下一次硬件中断。
		 */
		if (action_ret == IRQ_WAKE_THREAD) {
			int handled;
			/*
			 * We use bit 31 of thread_handled_last to
			 * denote the deferred spurious detection
			 * active. No locking necessary as
			 * thread_handled_last is only accessed here
			 * and we have the guarantee that hard
			 * interrupts are not reentrant.
			 */
			/*
			 * threads_handled_last 的 bit31 表示 deferred 生效。该字段只在这里访问，
			 * 且同一 desc 的 hardirq 保证不重入，所以无需额外锁；首次遇到纯 WAKE_THREAD 只
			 * 置标志并返回，不把尚未完成的线程误算成 unhandled。
			 */
			if (!(desc->threads_handled_last & SPURIOUS_DEFERRED)) {
				desc->threads_handled_last |= SPURIOUS_DEFERRED;
				return;
			}
			/*
			 * Check whether one of the threaded handlers
			 * returned IRQ_HANDLED since the last
			 * interrupt happened.
			 *
			 * For simplicity we just set bit 31, as it is
			 * set in threads_handled_last as well. So we
			 * avoid extra masking. And we really do not
			 * care about the high bits of the handled
			 * count. We just care about the count being
			 * different than the one we saw before.
			 */
			/*
			 * 下一次 pure WAKE_THREAD 到来时，比较线程累计 HANDLED 次数与上次快照。
			 * 为简化比较，把当前计数也 OR bit31，无需单独 mask；高位计数值不重要，只关心是否
			 * 发生变化。threads_handled 由 IRQ 线程在返回 HANDLED 时原子递增。
			 */
			handled = atomic_read(&desc->threads_handled);
			handled |= SPURIOUS_DEFERRED;
			if (handled != desc->threads_handled_last) {
				action_ret = IRQ_HANDLED;
				/*
				 * Note: We keep the SPURIOUS_DEFERRED
				 * bit set. We are handling the
				 * previous invocation right now.
				 * Keep it for the current one, so the
				 * next hardware interrupt will
				 * account for it.
				 */
				/*
				 * 计数变化证明上一轮被线程处理，故把本次判定改为 HANDLED；仍保留 deferred
				 * 位，因为当前这一轮又唤醒了线程，要留给下一次硬件 IRQ 结算。
				 */
				desc->threads_handled_last = handled;
			} else {
				/*
				 * None of the threaded handlers felt
				 * responsible for the last interrupt
				 *
				 * We keep the SPURIOUS_DEFERRED bit
				 * set in threads_handled_last as we
				 * need to account for the current
				 * interrupt as well.
				 */
				/*
				 * 计数未变说明线程都未认领上一轮，将上一轮结算为 IRQ_NONE；但当前事件
				 * 同样刚唤醒线程，故 deferred 位继续保留。
				 */
				action_ret = IRQ_NONE;
			}
		} else {
			/*
			 * One of the primary handlers returned
			 * IRQ_HANDLED. So we don't care about the
			 * threaded handlers on the same line. Clear
			 * the deferred detection bit.
			 *
			 * In theory we could/should check whether the
			 * deferred bit is set and take the result of
			 * the previous run into account here as
			 * well. But it's really not worth the
			 * trouble. If every other interrupt is
			 * handled we never trigger the spurious
			 * detector. And if this is just the one out
			 * of 100k unhandled ones which is handled
			 * then we merily delay the spurious detection
			 * by one hard interrupt. Not a real problem.
			 */
			/*
			 * 至少一个 primary 已返回 HANDLED，就无需关心同线路 thread_fn 的结果，
			 * 清 deferred。理论上还可结算此前延迟的一轮，但收益很小：隔次被处理永远达不到
			 * 杂散阈值；若只是 100000 次中少数处理，也仅把禁用判断推迟一次 hardirq。
			 */
			desc->threads_handled_last &= ~SPURIOUS_DEFERRED;
		}
	}

	if (unlikely(action_ret == IRQ_NONE)) {
		/*
		 * If we are seeing only the odd spurious IRQ caused by
		 * bus asynchronicity then don't eventually trigger an error,
		 * otherwise the counter becomes a doomsday timer for otherwise
		 * working systems
		 */
		/*
		 * 总线异步偶发的孤立 spurious 不应永久累积成“末日计时器”。距上次无人
		 * 处理超过 HZ/10 时重置为 1；只有短时间连续发生才递增。
		 */
		if (time_after(jiffies, desc->last_unhandled + HZ/10))
			desc->irqs_unhandled = 1;
		else
			desc->irqs_unhandled++;
		desc->last_unhandled = jiffies;
	}

	irq = irq_desc_get_irq(desc);
	if (unlikely(try_misrouted_irq(irq, desc, action_ret))) {
		/* ok 表示其他 IRQ 的共享 handler 已认领；仅原结果为 NONE 时抵消一次 unhandled。 */
		int ok = misrouted_irq(irq);
		if (action_ret == IRQ_NONE)
			desc->irqs_unhandled -= ok;
	}

	if (likely(!desc->irqs_unhandled))
		return;

	/* Now getting into unhandled irq detection */
	/* 只有当前窗口中存在 unhandled，才增加用于 100000 次采样窗口的 irq_count。 */
	desc->irq_count++;
	if (likely(desc->irq_count < 100000))
		return;

	desc->irq_count = 0;
	if (unlikely(desc->irqs_unhandled > 99900)) {
		/*
		 * The interrupt is stuck
		 */
		/* 窗口内至少 99901 次无人处理，判定线路很可能卡死。 */
		__report_bad_irq(desc, action_ret);
		/*
		 * Now kill the IRQ
		 */
		/*
		 * 记录 SPURIOUS_DISABLED，增加一层 disable depth 并关闭硬件；该层以后可
		 * 由安装新共享 handler 的路径 __enable_irq() 撤销。恢复定时器仍会强制轮询 handler。
		 */
		pr_emerg("Disabling IRQ #%d\n", irq);
		desc->istate |= IRQS_SPURIOUS_DISABLED;
		desc->depth++;
		irq_disable(desc);

		mod_timer(&poll_spurious_irq_timer, jiffies + POLL_SPURIOUS_IRQ_INTERVAL);
	}
	desc->irqs_unhandled = 0;
}

/*
 * noirqdebug 是全局杂散锁死检测开关；true 时 IRQ 建立路径给描述符设置 NO_DEBUG，使正常
 * flow 不调用上述检测。启动参数和可写 module parameter 可设置，热路径多读少写。
 */
bool noirqdebug __read_mostly;

/*
 * noirqdebug_setup() - 解析 noirqdebug 启动参数
 *
 * @str: __setup 提供但本选项不消费的参数串。
 * 设置全局开关、打印提示并返回 1 表示参数已处理。启动期调用，不需锁；注意运行期通过
 * module parameter 改全局值并不会回溯清除/设置所有既有 desc 的 NO_DEBUG 状态。
 */
int noirqdebug_setup(char *str)
{
	noirqdebug = 1;
	pr_info("IRQ lockup detection disabled\n");
	return 1;
}
/* 注册早期命令行开关 noirqdebug。 */
__setup("noirqdebug", noirqdebug_setup);
/* 同一变量作为权限 0644 的 bool 模块参数暴露。 */
module_param(noirqdebug, bool, 0644);
/* 用户可见参数说明：true 时关闭 IRQ lockup detection。 */
MODULE_PARM_DESC(noirqdebug, "Disable irq lockup detection when true");

/*
 * irqfixup_setup() - 启用仅针对未处理事件的误路由修复模式
 *
 * @str: 未消费的启动参数值。PREEMPT_RT 不支持此全 IRQ hard-handler 扫描，打印警告但
 * 返回 1 消费参数且不启用；其他配置将 irqfixup 设为 1，并警告性能影响。启动期调用。
 */
static int __init irqfixup_setup(char *str)
{
	if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
		pr_warn("irqfixup boot option not supported with PREEMPT_RT\n");
		return 1;
	}
	irqfixup = 1;
	pr_warn("Misrouted IRQ fixup support enabled.\n");
	pr_warn("This may impact system performance.\n");
	return 1;
}
/* 注册 irqfixup 内核命令行选项。 */
__setup("irqfixup", irqfixup_setup);
/* 允许以权限 0644 观察/修改扫描模式；运行期修改应理解其全局性能影响。 */
module_param(irqfixup, int, 0644);

/*
 * irqpoll_setup() - 启用增强误路由修复与 IRQPOLL 兼容扫描模式
 *
 * @str: 未消费的启动参数值。PREEMPT_RT 下仅警告并消费；其他配置把 irqfixup 设为 2，
 * 此后除 IRQ_NONE 外，IRQ0 和 IRQF_IRQPOLL action 也触发全描述符扫描，故明确警告可能
 * 显著降低性能。返回 1 表示命令行参数已处理。
 */
static int __init irqpoll_setup(char *str)
{
	if (IS_ENABLED(CONFIG_PREEMPT_RT)) {
		pr_warn("irqpoll boot option not supported with PREEMPT_RT\n");
		return 1;
	}
	irqfixup = 2;
	pr_warn("Misrouted IRQ fixup and polling support enabled\n");
	pr_warn("This may significantly impact system performance\n");
	return 1;
}
/* 注册 irqpoll 启动选项；它与 irqfixup 共用同一模式变量，后解析者决定最终值。 */
__setup("irqpoll", irqpoll_setup);
