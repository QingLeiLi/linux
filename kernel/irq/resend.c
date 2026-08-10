// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner
 *
 * This file contains the IRQ-resend code
 *
 * If the interrupt is waiting to be processed, we try to re-run it.
 * We can't directly run it from here since the caller might be in an
 * interrupt-protected region. Not all irq controller chips can
 * retrigger interrupts at the hardware level, so in those cases
 * we allow the resending of IRQs via a tasklet.
 */
/*
 * 原文说明：本文件实现 IRQ resend。若事件已 pending 但尚未处理，核心尝试重新触发它；
 * 调用者可能正处于中断保护区，不能直接在当前栈执行 flow handler。优先使用 irqchip 的
 * 硬件 retrigger；控制器不支持时，在 HARDIRQS_SW_RESEND 配置下把 desc 排入全局 hlist，
 * 由 tasklet 在 softirq 上下文重新调用 handle_irq。
 *
 * 状态链：IRQS_PENDING 表示需要补发，IRQS_REPLAY 防止同一事件重复排队；真正重新进入
 * flow handler 后由相应处理路径清 REPLAY。resend_node 嵌在 irq_desc 内，描述符释放前必须
 * clear_irq_resend() 摘链，irq_resend_lock 保护全局链及节点成员关系。
 */

#include <linux/irq.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/interrupt.h>

#include "internals.h"

#ifdef CONFIG_HARDIRQS_SW_RESEND

/* hlist_head to handle software resend of interrupts: */
/* 全局 hlist 保存等待软件重放的 irq_desc；节点由 desc 自身拥有，不另分配。 */
static HLIST_HEAD(irq_resend_list);
/* 同时串行排队、去重、摘除和 tasklet 消费；调用时不可睡眠。 */
static DEFINE_RAW_SPINLOCK(irq_resend_lock);

/*
 * Run software resends of IRQ's
 */
/*
 * 执行所有已排队的 IRQ 软件重发。
 * @unused: tasklet 接口参数，未消费。
 * 函数以 irq-disable raw lock 保护链；每次摘下一个 desc 并初始化为空节点，然后仅释放
 * raw lock（本地 IRQ 仍关闭）调用 desc->handle_irq(desc)，以模拟硬中断 flow 的上下文。
 * 回来后重新加锁继续，避免 handler 期间阻塞其他 CPU 排队/清理。无返回值、不睡眠；
 * 节点摘除后允许同一 desc 在处理期间再次排队，后续循环会再消费。
 */
static void resend_irqs(struct tasklet_struct *unused)
{
	guard(raw_spinlock_irq)(&irq_resend_lock);
	while (!hlist_empty(&irq_resend_list)) {
		struct irq_desc *desc;

		desc = hlist_entry(irq_resend_list.first, struct irq_desc,  resend_node);
		hlist_del_init(&desc->resend_node);

		raw_spin_unlock(&irq_resend_lock);
		desc->handle_irq(desc);
		raw_spin_lock(&irq_resend_lock);
	}
}

/* Tasklet to handle resend: */
/* 静态 tasklet 是软件重发的异步执行载体，多次 schedule 可合并但链保存每个 desc。 */
static DECLARE_TASKLET(resend_tasklet, resend_irqs);

/*
 * irq_sw_resend() - 把无法硬件 retrigger 的描述符排队到软件重发 tasklet
 *
 * @desc: 调用时存活、desc 锁内的目标；成功后其 resend_node 可能异步被消费。
 * 要求 flow 可安全从非真实硬件入口注入；IRQD_HANDLE_ENFORCE_IRQCTX 时返回 -EINVAL。
 * nested-thread IRQ 不能直接触发子 flow，必须有有效 parent_irq，并改为重发父描述符。
 * 在全局锁下仅当节点尚未入链时加入以去重，随后 schedule tasklet；成功返回 0。调用者
 * 通过 REPLAY 位防止逻辑重复，描述符生命周期由 clear/synchronize 协议保护。
 */
static int irq_sw_resend(struct irq_desc *desc)
{
	/*
	 * Validate whether this interrupt can be safely injected from
	 * non interrupt context
	 */
	/* 先验证该 flow 是否允许从软件构造而非真实 interrupt entry 注入。 */
	if (irqd_is_handle_enforce_irqctx(&desc->irq_data))
		return -EINVAL;

	/*
	 * If the interrupt is running in the thread context of the parent
	 * irq we need to be careful, because we cannot trigger it
	 * directly.
	 */
	/*
	 * nested IRQ 的处理发生在父 IRQ 线程上下文，直接调用子 handle_irq 会破坏
	 * 上下文/层级约束，因此需要改为重新触发父 IRQ。
	 */
	if (irq_settings_is_nested_thread(desc)) {
		/*
		 * If the parent_irq is valid, we retrigger the parent,
		 * otherwise we do nothing.
		 */
		/* 只有记录了有效 parent_irq 才能重发父级，否则明确失败且不排队。 */
		if (!desc->parent_irq)
			return -EINVAL;

		desc = irq_to_desc(desc->parent_irq);
		if (!desc)
			return -EINVAL;
	}

	/* Add to resend_list and activate the softirq: */
	/* 锁内去重加入全局链，锁外调度 tasklet/softirq 执行。 */
	scoped_guard(raw_spinlock, &irq_resend_lock) {
		if (hlist_unhashed(&desc->resend_node))
			hlist_add_head(&desc->resend_node, &irq_resend_list);
	}
	tasklet_schedule(&resend_tasklet);
	return 0;
}

/*
 * clear_irq_resend() - 从软件重发链同步摘除一个描述符
 *
 * @desc: 正在 shutdown/free 的存活描述符。全局锁下 hlist_del_init() 同时兼容已排队和空
 * 节点，返回后节点不在链中；无返回值。它只阻止尚未取出的任务，调用者仍须与可能已经
 * 摘节点并执行 handle_irq 的 tasklet 做相应同步。
 */
void clear_irq_resend(struct irq_desc *desc)
{
	guard(raw_spinlock)(&irq_resend_lock);
	hlist_del_init(&desc->resend_node);
}

/*
 * irq_resend_init() - 初始化新描述符内嵌的软件重发节点
 *
 * @desc: 尚未对外发布/尚未入 resend 链的描述符。将 resend_node 标成 unhashed；无锁、
 * 无返回值，只能在描述符初始化或已确认摘链后调用。
 */
void irq_resend_init(struct irq_desc *desc)
{
	INIT_HLIST_NODE(&desc->resend_node);
}
#else
/* 未启用软件重发时，描述符无需维护 resend_node，清理与初始化为空操作。 */
void clear_irq_resend(struct irq_desc *desc) {}
void irq_resend_init(struct irq_desc *desc) {}

/*
 * irq_sw_resend() - 配置关闭时的软件重发失败桩
 *
 * @desc: 未消费的借用描述符。始终返回 -EINVAL，促使上层报告该 IRQ 无可用重发机制。
 */
static int irq_sw_resend(struct irq_desc *desc)
{
	return -EINVAL;
}
#endif

/*
 * try_retrigger() - 尝试由 irqchip/domain 在硬件层重新触发描述符
 *
 * @desc: 非 NULL、desc 锁内且 chip 已建立的描述符。
 * 当前 chip 有 irq_retrigger 时直接调用；层级 domain 配置下否则沿父层寻找能力；非层级
 * 且无回调返回 0。返回遵循 retrigger 契约：非零表示已接受/成功，0 表示不可重触发，
 * 上层随后尝试软件重发。函数不修改 PENDING/REPLAY，不睡眠。
 */
static int try_retrigger(struct irq_desc *desc)
{
	if (desc->irq_data.chip->irq_retrigger)
		return desc->irq_data.chip->irq_retrigger(&desc->irq_data);

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	return irq_chip_retrigger_hierarchy(&desc->irq_data);
#else
	return 0;
#endif
}

/*
 * IRQ resend
 *
 * Is called with interrupts disabled and desc->lock held.
 */
/*
 * IRQ 重发入口；调用时本地 IRQ 已关闭且持有 desc->lock。
 * @desc: 输入输出的目标描述符。
 * @inject: false 仅补发已有 IRQS_PENDING；true 为测试注入，即使没有 pending 也触发。
 *
 * level IRQ 依靠仍有效的电平由硬件自然重送，核心清 PENDING 后返回 -EINVAL。已有 REPLAY
 * 返回 -EBUSY；无 pending 且非 inject 返回 0。其余先清 PENDING，优先硬件 retrigger，
 * 返回 0 时回退 irq_sw_resend()。任一机制成功（err==0）置 IRQS_REPLAY。返回 0、-EBUSY、
 * -EINVAL 或软件重发错误；REPLAY 的清理由后续 flow handler 完成。
 */
int check_irq_resend(struct irq_desc *desc, bool inject)
{
	int err = 0;

	/*
	 * We do not resend level type interrupts. Level type interrupts
	 * are resent by hardware when they are still active. Clear the
	 * pending bit so suspend/resume does not get confused.
	 */
	/*
	 * level 源若电平仍 active，硬件会自然再次送达，无需人工 retrigger；仍要清
	 * 软件 PENDING，避免 suspend/resume 把它误认为尚需补发。
	 */
	if (irq_settings_is_level(desc)) {
		desc->istate &= ~IRQS_PENDING;
		return -EINVAL;
	}

	if (desc->istate & IRQS_REPLAY)
		return -EBUSY;

	if (!(desc->istate & IRQS_PENDING) && !inject)
		return 0;

	desc->istate &= ~IRQS_PENDING;

	if (!try_retrigger(desc))
		err = irq_sw_resend(desc);

	/* If the retrigger was successful, mark it with the REPLAY bit */
	/* 硬件或软件机制成功接受重发后置 REPLAY，禁止完成前重复安排。 */
	if (!err)
		desc->istate |= IRQS_REPLAY;
	return err;
}

#ifdef CONFIG_GENERIC_IRQ_INJECTION
/**
 * irq_inject_interrupt - Inject an interrupt for testing/error injection
 * @irq:	The interrupt number
 *
 * This function must only be used for debug and testing purposes!
 *
 * Especially on x86 this can cause a premature completion of an interrupt
 * affinity change causing the interrupt line to become stale. Very
 * unlikely, but possible.
 *
 * The injection can fail for various reasons:
 * - Interrupt is not activated
 * - Interrupt is NMI type or currently replaying
 * - Interrupt is level type
 * - Interrupt does not support hardware retrigger and software resend is
 *   either not enabled or not possible for the interrupt.
 */
/*
 * 仅供调试/错误注入，把 @irq 人工置为 pending。尤其在 x86，注入可能让一次
 * affinity change 过早完成并留下 stale 线路，概率虽低但不能用于生产控制流。
 *
 * 首先调用 irq_set_irqchip_state(PENDING=true)，成功即返回 0。失败后取得 desc bus lock
 * 与 desc 锁，只有非 NMI 且 domain 已 activated 才以 inject=true 走 resend。level、正在
 * replay、未激活、无硬件 retrigger 且无可用软件重发等均返回负 errno。函数可访问慢总线，
 * 必须在可满足 chip bus-lock 约束的测试上下文调用。
 */
int irq_inject_interrupt(unsigned int irq)
{
	int err = -EINVAL;

	/* Try the state injection hardware interface first */
	/* 优先使用 irqchip 明确定义的 PENDING 状态接口，最接近真实硬件注入。 */
	if (!irq_set_irqchip_state(irq, IRQCHIP_STATE_PENDING, true))
		return 0;

	/* That failed, try via the resend mechanism */
	/* 状态接口失败后，再尝试通用硬件 retrigger/软件 tasklet resend。 */
	scoped_irqdesc_get_and_buslock(irq, 0) {
		struct irq_desc *desc = scoped_irqdesc;

		/*
		 * Only try to inject when the interrupt is:
		 *  - not NMI type
		 *  - activated
		 */
		/* NMI 不允许此普通注入；未 activated 的 IRQ 尚无可安全操作的硬件资源。 */
		if (!irq_is_nmi(desc) && irqd_is_activated(&desc->irq_data))
			err = check_irq_resend(desc, true);
	}
	return err;
}
EXPORT_SYMBOL_GPL(irq_inject_interrupt);
#endif
