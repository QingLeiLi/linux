// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2004 Linus Torvalds, Ingo Molnar
 *
 * This file contains the interrupt probing code and driver APIs.
 */

#include <linux/irq.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/async.h>

#include "internals.h"

/*
 * Autodetection depends on the fact that any interrupt that
 * comes in on to an unassigned handler will get stuck with
 * "IRQS_WAITING" cleared and the interrupt disabled.
 */
/*
 * 自动探测依赖无 action 线路的 flow handler：探测事件到来会清
 * IRQS_WAITING，并因没有可执行 action 而把事件记为 pending/保持线路关闭。于是
 * “WAITING 已清”可作为该 IRQ 在观察窗口内触发过的证据。
 *
 * 整个协议分三阶段：probe_irq_on() 清旧事件并武装候选，驱动主动触发设备，随后
 * probe_irq_off()/probe_irq_mask() 收集结果并关闭全部残留候选。它是旧式全局探测
 * 接口，睡眠且扫描整个 IRQ 空间，不适合热插拔或现代可枚举设备。
 */
/*
 * 跨 API 持有的全局事务锁：probe_irq_on() 获取后故意不释放，由同一任务调用
 * probe_irq_off()/probe_irq_mask() 结束事务并解锁。它串行化所有探测者；漏掉结束
 * 调用会永久阻塞后来者，换任务结束也违反 mutex ownership。
 */
static DEFINE_MUTEX(probing_active);

/**
 *	probe_irq_on	- begin an interrupt autodetect
 *
 *	Commence probing for an interrupt. The interrupts are scanned
 *	and a mask of potential interrupt lines is returned.
 *
 */
/*
 * 原文契约：开始一次 IRQ 自动探测，扫描未分配且允许 probe 的描述符，并返回低 32
 * 个 quiet 候选组成的位图。函数先等待全局 async 工作并取得跨调用 mutex；第一轮
 * 启动候选、等待 20ms 排掉长期悬挂事件，第二轮置 AUTODETECT|WAITING 并重新启动，
 * 再等待 100ms 让杂散源暴露。已清 WAITING 的线路判为杂散并立即 shutdown/deactivate；
 * 仍 WAITING 的线路保持武装，等待调用者触发设备后由结束 API 收集。
 *
 * 本函数可睡眠，必须与一个结束 API 在同一任务严格配对；返回 0 也仍持有 mutex，
 * 调用者不能据此跳过 cleanup。chip/type/startup 结果是 best-effort，函数无 errno 通道。
 */
unsigned long probe_irq_on(void)
{
	struct irq_desc *desc;
	unsigned long mask = 0;
	int i;

	/*
	 * quiesce the kernel, or at least the asynchronous portion
	 */
	/* 原文意为先静止内核中至少 async 框架这一部分，减少后台工作制造的误判。 */
	async_synchronize_full();
	mutex_lock(&probing_active);
	/*
	 * something may have generated an irq long ago and we want to
	 * flush such a longstanding irq before considering it as spurious.
	 */
	/* 第一轮先冲掉早已悬挂的 IRQ，避免把历史事件误判为本次杂散源。 */
	for_each_irq_desc_reverse(i, desc) {
		guard(raw_spinlock_irq)(&desc->lock);
		if (!desc->action && irq_settings_can_probe(desc)) {
			/*
			 * Some chips need to know about probing in
			 * progress:
			 */
			/* 某些 irqchip 需要通过 IRQ_TYPE_PROBE 得知正在进行自动探测。 */
			if (desc->irq_data.chip->irq_set_type)
				desc->irq_data.chip->irq_set_type(&desc->irq_data, IRQ_TYPE_PROBE);
			irq_activate_and_startup(desc, IRQ_NORESEND);
		}
	}

	/* Wait for longstanding interrupts to trigger. */
	/* 原文要求留出 20ms，让第一轮启动后积压的旧事件实际到达并被清理。 */
	msleep(20);

	/*
	 * enable any unassigned irqs
	 * (we must startup again here because if a longstanding irq
	 * happened in the previous stage, it may have masked itself)
	 */
	/*
	 * 第二轮必须重新 startup 所有未分配候选，因为旧事件在第一阶段到达时
	 * 可能已令线路自我 mask；此轮才正式发布 AUTODETECT|WAITING。
	 */
	for_each_irq_desc_reverse(i, desc) {
		guard(raw_spinlock_irq)(&desc->lock);
		if (!desc->action && irq_settings_can_probe(desc)) {
			desc->istate |= IRQS_AUTODETECT | IRQS_WAITING;
			if (irq_activate_and_startup(desc, IRQ_NORESEND))
				desc->istate |= IRQS_PENDING;
		}
	}

	/*
	 * Wait for spurious interrupts to trigger
	 */
	/* 原文要求再观察 100ms，使不受驱动控制的活跃/杂散线路清除 WAITING。 */
	msleep(100);

	/*
	 * Now filter out any obviously spurious interrupts
	 */
	/* 此轮过滤已经明显触发的杂散线路，只把仍安静者交给调用者测试。 */
	for_each_irq_desc(i, desc) {
		guard(raw_spinlock_irq)(&desc->lock);
		if (desc->istate & IRQS_AUTODETECT) {
			/* It triggered already - consider it spurious. */
			/* 原文判定：WAITING 已清表示它在驱动测试前就触发，应视为杂散并退出探测。 */
			if (!(desc->istate & IRQS_WAITING)) {
				desc->istate &= ~IRQS_AUTODETECT;
				irq_shutdown_and_deactivate(desc);
			} else if (i < 32) {
				mask |= 1 << i;
			}
		}
	}

	return mask;
}
EXPORT_SYMBOL(probe_irq_on);

/**
 *	probe_irq_mask - scan a bitmap of interrupt lines
 *	@val:	mask of interrupts to consider
 *
 *	Scan the interrupt lines and return a bitmap of active
 *	autodetect interrupts. The interrupt probe logic state
 *	is then returned to its previous value.
 *
 *	Note: we need to scan all the irq's even though we will
 *	only return autodetect irq numbers - just so that we reset
 *	them all to a known state.
 */
/*
 * 原文契约：结束探测并返回 @val 所关心的已触发位图。函数必须扫描所有 desc，而非
 * 只看 @val：每个 AUTODETECT 项都要清标志并 shutdown/deactivate，恢复统一已知状态；
 * 只有 IRQ<16 且 WAITING 已清的项进入结果，最后再与 @val 相交。
 *
 * 调用者必须是取得 probing_active 的 probe_irq_on() 同一任务；函数完成全量清理后
 * 解锁并返回，无 errno 通道。即使 @val 为 0 也不能跳过扫描，因为清理与返回筛选是
 * 两项独立责任。
 */
unsigned int probe_irq_mask(unsigned long val)
{
	unsigned int mask = 0;
	struct irq_desc *desc;
	int i;

	for_each_irq_desc(i, desc) {
		guard(raw_spinlock_irq)(&desc->lock);
		if (desc->istate & IRQS_AUTODETECT) {
			if (i < 16 && !(desc->istate & IRQS_WAITING))
				mask |= 1 << i;

			desc->istate &= ~IRQS_AUTODETECT;
			irq_shutdown_and_deactivate(desc);
		}
	}
	mutex_unlock(&probing_active);

	return mask & val;
}
EXPORT_SYMBOL(probe_irq_mask);

/**
 *	probe_irq_off	- end an interrupt autodetect
 *	@val: mask of potential interrupts (unused)
 *
 *	Scans the unused interrupt lines and returns the line which
 *	appears to have triggered the interrupt. If no interrupt was
 *	found then zero is returned. If more than one interrupt is
 *	found then minus the first candidate is returned to indicate
 *	their is doubt.
 *
 *	The interrupt probe logic state is returned to its previous
 *	value.
 *
 *	BUGS: When used in a module (which arguably shouldn't happen)
 *	nothing prevents two IRQ probe callers from overlapping. The
 *	results of this are non-optimal.
 */
/*
 * 原文契约：结束探测，扫描所有未分配候选；WAITING 已清者是调用者触发阶段观察到的
 * IRQ。没有候选返回 0，恰好一个返回其逻辑号，多个则返回第一个候选的负值表示歧义；
 * @val 是保留但当前未使用的历史参数。无论结果如何，全部 AUTODETECT 项都会清标志并
 * shutdown/deactivate，随后释放跨调用 mutex。
 * 因为 on 阶段会武装全部可探测 IRQ、却只把低 32 位放进返回 mask，off 又不按 @val
 * 过滤，所以更高编号的触发项仍可能影响这里的唯一/多候选结果；这是当前旧 ABI 实现。
 *
 * 原 BUGS 声称模块中的两个探测者可重叠，已与当前 probing_active 实现不一致：当前
 * 第二个调用者会在 probe_irq_on() 阻塞，而不会并行改状态。真实剩余约束是 on/off 必须
 * 同任务、严格配对；持锁期间睡眠或遗漏结束调用会阻塞全局探测。
 */
int probe_irq_off(unsigned long val)
{
	int i, irq_found = 0, nr_of_irqs = 0;
	struct irq_desc *desc;

	for_each_irq_desc(i, desc) {
		guard(raw_spinlock_irq)(&desc->lock);
		if (desc->istate & IRQS_AUTODETECT) {
			if (!(desc->istate & IRQS_WAITING)) {
				if (!nr_of_irqs)
					irq_found = i;
				nr_of_irqs++;
			}
			desc->istate &= ~IRQS_AUTODETECT;
			irq_shutdown_and_deactivate(desc);
		}
	}
	mutex_unlock(&probing_active);

	if (nr_of_irqs > 1)
		irq_found = -irq_found;

	return irq_found;
}
EXPORT_SYMBOL(probe_irq_off);

/* 三个 legacy 探测入口供内核驱动使用；状态事务仍由 on 与一个结束接口配对拥有。 */
