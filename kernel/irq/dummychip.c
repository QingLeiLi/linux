// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the dummy interrupt chip implementation
 *
 * 本文件提供两种不访问真实硬件的 irq_chip：no_irq_chip 是“尚无有效控制器”的
 * 哨兵，dummy_irq_chip 则是可供简单中断源使用的最小合法控制器。二者回调看似
 * 相近，但前者会让中断申请路径拒绝该描述符，后者可以被正常安装和使用。
 */
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/export.h>

#include "internals.h"

/*
 * What should we do if we get a hw irq event on an illegal vector?
 * Each architecture has to answer this themselves.
 *
 * 如果非法向量上出现硬件中断事件，应当怎样处理？每个体系结构都必须自行给出答案。
 */
/*
 * 非法向量确认入口：data 必须属于一个有效 irq_desc；先打印描述符状态以保留诊断
 * 现场，再把 Linux IRQ 号交给体系结构的 ack_bad_irq() 清除或隔离异常来源。此路径
 * 不把该中断变成可用中断，也不取得描述符锁；它只用于 no_irq_chip 意外收到中断时
 * 尽力止住硬件并暴露配置错误。
 */
static void ack_bad(struct irq_data *data)
{
	struct irq_desc *desc = irq_data_to_desc(data);

	print_irq_desc(data->irq, desc);
	ack_bad_irq(data->irq);
}

/*
 * NOP functions
 *
 * 空操作回调。
 */
/*
 * 满足不需要硬件动作的 void irq_chip 回调槽；data 只用于统一函数签名，没有 ownership
 * 转移，也不会改变 irq_data、irq_desc 或控制器状态。
 */
static void noop(struct irq_data *data) { }

/*
 * 满足 irq_startup() 回调槽并返回成功；调用者会据此继续启用流程，但这里不触碰硬件。
 * 只有确实无需启动动作的芯片才可复用它，不能用它掩盖本应失败的硬件初始化。
 */
static unsigned int noop_ret(struct irq_data *data)
{
	return 0;
}

/*
 * Generic no controller implementation
 *
 * 通用的“无控制器”实现。
 */
/*
 * no_irq_chip 是 irq_desc 尚未绑定有效控制器时的静态哨兵。irq_set_chip(..., NULL)
 * 也会恢复到它，而 __setup_irq() 会以 -ENOSYS 拒绝在该哨兵上注册处理程序。因此这些
 * NOP 回调不是可工作的虚拟硬件接口；唯一有意保留的 irq_ack 是 ack_bad()，用于异常
 * 中断诊断。IRQCHIP_SKIP_SET_WAKE 表示核心无需尝试不存在的唤醒控制。
 */
struct irq_chip no_irq_chip = {
	.name		= "none",
	.irq_startup	= noop_ret,
	.irq_shutdown	= noop,
	.irq_enable	= noop,
	.irq_disable	= noop,
	.irq_ack	= ack_bad,
	.flags		= IRQCHIP_SKIP_SET_WAKE,
};

/*
 * Generic dummy implementation which can be used for
 * real dumb interrupt sources
 *
 * 通用虚拟实现，可用于真实但无需任何控制操作的简单中断源。
 */
/*
 * dummy_irq_chip 与 no_irq_chip 的关键区别是它代表一个有效、可申请的控制器。startup、
 * ack、mask 和 unmask 等槽位均成功但不访问硬件，使核心和流处理代码仍可沿正常 irq_chip
 * 接口运行；调用方必须保证中断源确实无需屏蔽、确认或唤醒编程。对象为静态全局实例，
 * 使用方只保存其地址，不负责释放。
 */
struct irq_chip dummy_irq_chip = {
	.name		= "dummy",
	.irq_startup	= noop_ret,
	.irq_shutdown	= noop,
	.irq_enable	= noop,
	.irq_disable	= noop,
	.irq_ack	= noop,
	.irq_mask	= noop,
	.irq_unmask	= noop,
	.flags		= IRQCHIP_SKIP_SET_WAKE,
};
/* 导出最小合法 irq_chip，供内核其他子系统和可加载模块复用。 */
EXPORT_SYMBOL_GPL(dummy_irq_chip);
