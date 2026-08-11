// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2009 Rafael J. Wysocki <rjw@sisk.pl>, Novell Inc.
 *
 * This file contains power management functions related to interrupts.
 */

#include <linux/irq.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#include "internals.h"

/*
 * 系统级 suspend 需要冻结普通设备 IRQ，同时保留能够唤醒系统的中断入口。本文件
 * 用 desc->istate 的 IRQS_SUSPENDED/PENDING 与 irq_data 的 WAKEUP_ARMED、
 * IRQ_ENABLED_ON_SUSPEND 协调这一过程，并用 desc->depth 保证临时 disable/enable
 * 与驱动原有禁用层数严格配对。
 *
 * suspend 路径在 desc 锁内发布状态，锁外 synchronize_irq() 等待旧 handler 退出；
 * 唤醒 IRQ 到达后由 flow-handler 入口消费 armed 状态、禁止该线并通知 PM 核心。
 * resume 分 syscore early 和常规设备阶段执行，嵌套线程 IRQ 由父中断统一管理。
 */

/*
 * 消费一次挂起期间到达的唤醒 IRQ，并把它转换成系统唤醒事件。
 * 调用者是持有 desc->lock 的 flow-handler 入口。先清 WAKEUP_ARMED，保证同一轮只
 * 消费一次；再设置 SUSPENDED|PENDING，并增加一层 disable depth 后关闭 irqchip，
 * 从而保留“醒来后仍需重放”的事实。最后把 Linux IRQ 号报告给 PM 核心。
 * 函数不返回错误；后续由 rearm_wake_irq() 或 resume 路径配对恢复该层 depth。
 */
void irq_pm_handle_wakeup(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_WAKEUP_ARMED);
	desc->istate |= IRQS_SUSPENDED | IRQS_PENDING;
	desc->depth++;
	irq_disable(desc);
	pm_system_irq_wakeup(irq_desc_get_irq(desc));
}

/*
 * Called from __setup_irq() with desc->lock held after @action has
 * been installed in the action chain.
 */
/* 在 @action 已插入 action 链且持有 desc->lock 后，由 __setup_irq() 调用。 */
/*
 * 把新 action 的 PM 属性计入描述符聚合计数。
 * nr_actions 先反映已发布链长度；FORCE_RESUME 采用 all-or-none 规则，若共享线只有
 * 部分 action 声明就告警。NO_SUSPEND 与 COND_SUSPEND 分别计数；一旦存在真正的
 * NO_SUSPEND action，其余 action 必须也为 NO_SUSPEND 或 COND_SUSPEND，否则共享线
 * 在 suspend 期间无法安全决定是否执行 handler，故告警。函数只更新锁内计数。
 */
void irq_pm_install_action(struct irq_desc *desc, struct irqaction *action)
{
	desc->nr_actions++;

	if (action->flags & IRQF_FORCE_RESUME)
		desc->force_resume_depth++;

	WARN_ON_ONCE(desc->force_resume_depth &&
		     desc->force_resume_depth != desc->nr_actions);

	if (action->flags & IRQF_NO_SUSPEND)
		desc->no_suspend_depth++;
	else if (action->flags & IRQF_COND_SUSPEND)
		desc->cond_suspend_depth++;

	WARN_ON_ONCE(desc->no_suspend_depth &&
		     (desc->no_suspend_depth + desc->cond_suspend_depth) != desc->nr_actions);
}

/*
 * Called from __free_irq() with desc->lock held after @action has
 * been removed from the action chain.
 */
/* 在 @action 已从 action 链摘除且持有 desc->lock 后，由 __free_irq() 调用。 */
/*
 * 从描述符 PM 聚合计数中撤销一个已摘链 action。
 * 更新顺序与安装路径对称：减少总 action 数，再按 FORCE_RESUME、NO_SUSPEND 或
 * COND_SUSPEND 标志减少对应深度。@action 在函数返回后仍归 free 路径所有；这里
 * 不释放对象、不操作硬件。安装时已验证共享规则，删除只收缩集合，无需重复告警。
 */
void irq_pm_remove_action(struct irq_desc *desc, struct irqaction *action)
{
	desc->nr_actions--;

	if (action->flags & IRQF_FORCE_RESUME)
		desc->force_resume_depth--;

	if (action->flags & IRQF_NO_SUSPEND)
		desc->no_suspend_depth--;
	else if (action->flags & IRQF_COND_SUSPEND)
		desc->cond_suspend_depth--;
}

/*
 * 在系统 suspend 阶段处理一个 IRQ，返回退出 desc 锁后是否必须 synchronize_irq()。
 * 调用者持有 desc->lock 且本地中断关闭。无 action、chained 或含 NO_SUSPEND action
 * 的中断保持运行并返回 false。
 *
 * 已配置为唤醒源的 IRQ 只设置 WAKEUP_ARMED；若 irqchip 要求挂起时必须启用唤醒线，
 * 且该 IRQ 原先被禁用，则临时 __enable_irq() 并记录 IRQ_ENABLED_ON_SUSPEND，供恢复
 * 阶段还原。普通 IRQ 设置 IRQS_SUSPENDED、增加禁用层；需要硬件级屏蔽的 chip 再
 * 显式 mask。两类已处理 IRQ 都返回 true，让外层等待旧 handler 退出，并保证 armed/
 * suspended 状态在 suspend_device_irqs() 返回前对并发中断处理可见。
 */
static bool suspend_device_irq(struct irq_desc *desc)
{
	unsigned long chipflags = irq_desc_get_chip(desc)->flags;
	struct irq_data *irqd = &desc->irq_data;

	if (!desc->action || irq_desc_is_chained(desc) ||
	    desc->no_suspend_depth)
		return false;

	/* 唤醒源保持可投递，只发布 armed 状态供 flow handler 消费。 */
	if (irqd_is_wakeup_set(irqd)) {
		irqd_set(irqd, IRQD_WAKEUP_ARMED);

		if ((chipflags & IRQCHIP_ENABLE_WAKEUP_ON_SUSPEND) &&
		     irqd_irq_disabled(irqd)) {
			/*
			 * Interrupt marked for wakeup is in disabled state.
			 * Enable interrupt here to unmask/enable in irqchip
			 * to be able to resume with such interrupts.
			 */
			/*
			 * 某些 chip 的唤醒机制要求中断线处于 enabled；若驱动原先禁用它，
			 * suspend 期间临时启用并留下专用标志，以便 resume 精确恢复原状态。
			 */
			__enable_irq(desc);
			irqd_set(irqd, IRQD_IRQ_ENABLED_ON_SUSPEND);
		}
		/*
		 * We return true here to force the caller to issue
		 * synchronize_irq(). We need to make sure that the
		 * IRQD_WAKEUP_ARMED is visible before we return from
		 * suspend_device_irqs().
		 */
		/* 返回 true 迫使外层同步，确保 WAKEUP_ARMED 在挂起入口完成前全局可见。 */
		return true;
	}

	/* 非唤醒 IRQ 进入 suspended 状态，并通过正常 depth 机制关闭。 */
	desc->istate |= IRQS_SUSPENDED;
	__disable_irq(desc);

	/*
	 * Hardware which has no wakeup source configuration facility
	 * requires that the non wakeup interrupts are masked at the
	 * chip level. The chip implementation indicates that with
	 * IRQCHIP_MASK_ON_SUSPEND.
	 */
	/* 无独立 wake 配置的硬件还要求在 chip 层显式 mask 普通 IRQ。 */
	if (chipflags & IRQCHIP_MASK_ON_SUSPEND)
		mask_irq(desc);
	return true;
}

/**
 * suspend_device_irqs - disable all currently enabled interrupt lines
 *
 * During system-wide suspend or hibernation device drivers need to be
 * prevented from receiving interrupts and this function is provided
 * for this purpose.
 *
 * So we disable all interrupts and mark them IRQS_SUSPENDED except
 * for those which are unused, those which are marked as not
 * suspendable via an interrupt request with the flag IRQF_NO_SUSPEND
 * set and those which are marked as active wakeup sources.
 *
 * The active wakeup sources are handled by the flow handler entry
 * code which checks for the IRQD_WAKEUP_ARMED flag, suspends the
 * interrupt and notifies the pm core about the wakeup.
 */
/*
 * 系统 suspend/hibernate 前冻结当前设备 IRQ。
 * 普通已用中断会被标记 IRQS_SUSPENDED 并禁用；未使用、通过 IRQF_NO_SUSPEND
 * 声明必须运行、以及活动唤醒源的中断不会按普通路径关闭。唤醒源改由 flow-handler
 * 入口检查 WAKEUP_ARMED，到达时暂停该线并通知 PM 核心。
 *
 * 按描述符遍历时跳过 nested-thread IRQ，因为它们依赖父 IRQ 的硬件线。单项状态在
 * raw desc 锁内修改，锁外再 synchronize_irq()：既避免持 raw 锁等待 handler，又
 * 保证旧处理实例结束和新 PM 状态可见。函数无返回值，单项策略由 helper 决定。
 */
void suspend_device_irqs(void)
{
	struct irq_desc *desc;
	int irq;

	/* 遍历所有已分配描述符，不能只看 active action 的快速索引。 */
	for_each_irq_desc(irq, desc) {
		bool sync;

		if (irq_settings_is_nested_thread(desc))
			continue;
		scoped_guard(raw_spinlock_irqsave, &desc->lock)
			sync = suspend_device_irq(desc);

		/* 等待必须放在 desc raw 锁外；true 同时充当状态发布后的同步要求。 */
		if (sync)
			synchronize_irq(irq);
	}
}

/*
 * 在持有 desc->lock 的条件下恢复一个 IRQ 的 suspend 状态。
 * 先无条件撤销 WAKEUP_ARMED。若该唤醒 IRQ 是 suspend 为硬件要求临时启用的，
 * 先 __disable_irq() 并清专用标志，恢复驱动原先的禁用深度。
 *
 * 具有 IRQS_SUSPENDED 的 IRQ 清标志并用 __enable_irq() 撤销 suspend 增加的深度。
 * 未被 suspend 的 IRQ 通常直接返回；若任一 action 声明 FORCE_RESUME，则人为增加
 * 一层 depth 并设置 disabled/masked 状态，再走共同 enable 路径，强制 irqchip 执行
 * 恢复/重启。函数不转移对象、不报告错误，硬件操作遵循 chip 回调契约。
 */
static void resume_irq(struct irq_desc *desc)
{
	struct irq_data *irqd = &desc->irq_data;

	irqd_clear(irqd, IRQD_WAKEUP_ARMED);

	if (irqd_is_enabled_on_suspend(irqd)) {
		/*
		 * Interrupt marked for wakeup was enabled during suspend
		 * entry. Disable such interrupts to restore them back to
		 * original state.
		 */
		/* 挂起入口临时打开的唤醒线必须先恢复为驱动原有的 disabled 状态。 */
		__disable_irq(desc);
		irqd_clear(irqd, IRQD_IRQ_ENABLED_ON_SUSPEND);
	}

	if (desc->istate & IRQS_SUSPENDED)
		goto resume;

	/* Force resume the interrupt? */
	/* 没有 suspend 标志时，仅 FORCE_RESUME 仍要求执行一次硬件恢复。 */
	if (!desc->force_resume_depth)
		return;

	/* Pretend that it got disabled ! */
	/* 构造一层“已禁用”状态，让共同 __enable_irq() 确实调用 startup/enable。 */
	desc->depth++;
	irq_state_set_disabled(desc);
	irq_state_set_masked(desc);
resume:
	/* 先撤销软件 suspend 标志，再对称减少一层 depth 并恢复硬件。 */
	desc->istate &= ~IRQS_SUSPENDED;
	__enable_irq(desc);
}

/*
 * 扫描描述符并执行 early 或常规 IRQ 恢复阶段。
 * @want_early=true 时只处理首个 action 带 IRQF_EARLY_RESUME 的 IRQ；false 时遍历
 * 全部非 nested-thread IRQ，因此早期已处理项也会再次按当前状态判断。每个
 * resume_irq() 调用都在 raw_spinlock_irqsave 保护下，嵌套线程 IRQ 由父线恢复。
 * 函数无返回值，不持跨描述符锁。
 */
static void resume_irqs(bool want_early)
{
	struct irq_desc *desc;
	int irq;

	for_each_irq_desc(irq, desc) {
		bool is_early = desc->action &&	desc->action->flags & IRQF_EARLY_RESUME;

		if (!is_early && want_early)
			continue;
		if (irq_settings_is_nested_thread(desc))
			continue;

		/* guard 覆盖本轮余下作用域，保证 resume_irq() 的 depth/state 原子变化。 */
		guard(raw_spinlock_irqsave)(&desc->lock);
		resume_irq(desc);
	}
}

/**
 * rearm_wake_irq - rearm a wakeup interrupt line after signaling wakeup
 * @irq: Interrupt to rearm
 */
/*
 * 唤醒事件已上报但系统仍需继续等待时，重新武装 Linux IRQ @irq。
 * scoped_irqdesc_get_and_buslock() 同时取得稳定 desc、chip bus lock 和 desc 锁；
 * 只有该线确实因唤醒而处于 IRQS_SUSPENDED，且仍配置为 wakeup source 时才处理。
 * 清 suspended、重新设置 WAKEUP_ARMED，再用 __enable_irq() 撤销唤醒处理增加的
 * depth。无效/状态已变化时幂等返回，scope 自动按正确顺序解锁。
 */
void rearm_wake_irq(unsigned int irq)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL) {
		struct irq_desc *desc = scoped_irqdesc;

		/* 只重新武装仍处于“已消费唤醒、等待恢复”的有效 wake IRQ。 */
		if (!(desc->istate & IRQS_SUSPENDED) || !irqd_is_wakeup_set(&desc->irq_data))
			return;

		desc->istate &= ~IRQS_SUSPENDED;
		irqd_set(&desc->irq_data, IRQD_WAKEUP_ARMED);
		__enable_irq(desc);
	}
}

/**
 * irq_pm_syscore_resume - enable interrupt lines early
 * @data: syscore context
 *
 * Enable all interrupt lines with %IRQF_EARLY_RESUME set.
 */
/*
 * syscore 恢复阶段提前启用所有带 IRQF_EARLY_RESUME 的中断线。
 * @data 仅满足 syscore 回调签名，未使用；实际工作委托 resume_irqs(true)，无返回值。
 */
static void irq_pm_syscore_resume(void *data)
{
	resume_irqs(true);
}

/* 把 IRQ early-resume 回调挂入 syscore 生命周期；对象静态存活且只读发布。 */
static const struct syscore_ops irq_pm_syscore_ops = {
	.resume		= irq_pm_syscore_resume,
};

/* IRQ PM 的 syscore 注册对象，长期借用静态 ops 表。 */
static struct syscore irq_pm_syscore = {
	.ops = &irq_pm_syscore_ops,
};

/*
 * 设备初始化阶段注册 IRQ PM syscore 对象。
 * register_syscore() 把静态节点加入全局 syscore 链，之后由系统恢复时序调用；
 * 当前注册接口无错误返回，因此 initcall 始终返回 0。
 */
static int __init irq_pm_init_ops(void)
{
	register_syscore(&irq_pm_syscore);
	return 0;
}

device_initcall(irq_pm_init_ops);

/**
 * resume_device_irqs - enable interrupt lines disabled by suspend_device_irqs()
 *
 * Enable all non-%IRQF_EARLY_RESUME interrupt lines previously
 * disabled by suspend_device_irqs() that have the IRQS_SUSPENDED flag
 * set as well as those with %IRQF_FORCE_RESUME.
 */
/*
 * 常规设备恢复阶段启用 suspend_device_irqs() 关闭的中断，并处理 FORCE_RESUME。
 * 调用 resume_irqs(false) 扫描全部非 nested-thread 描述符；真正是否恢复由每项的
 * IRQS_SUSPENDED、临时 enabled-on-suspend 和 force_resume_depth 状态决定。
 */
void resume_device_irqs(void)
{
	resume_irqs(false);
}
