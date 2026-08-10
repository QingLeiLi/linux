// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the core interrupt handling code, for irq-chip based
 * architectures. Detailed information is available in
 * Documentation/core-api/genericirq.rst
 */
/*
 * 原文说明：本文件实现以 irq_chip 为硬件抽象的架构通用 IRQ 核心，设计背景见
 * Documentation/core-api/genericirq.rst。irq_chip 提供 startup、mask、ack、eoi、
 * affinity 等控制器操作；本文件把这些回调与 irq_desc 软件状态、disable depth、
 * irq_domain 层级以及 edge/level/fasteoi 流处理器组合成一致协议。
 *
 * 学习时可按三层阅读：
 *   1. 配置层：irq_set_chip()/__irq_set_handler()/irq_modify_status() 发布控制器和策略；
 *   2. 状态层：irq_startup()/irq_shutdown()/mask_irq() 保持硬件动作与 IRQD_* 位同步；
 *   3. 分派层：handle_*_irq() 决定 ack、mask、action、unmask/eoi 的严格先后关系。
 *
 * 核心不变量是软件状态与硬件动作必须按既定顺序同步：MASKED/STARTED 通常在相应
 * chip 回调完成后发布；DISABLED 也可先表达软件禁用意图，再由 lazy-disable 选择是否
 * 立即 mask。大多数入口在 desc->lock 下运行且不可睡眠；带 buslock 的配置 API 可在
 * 取得 raw lock 前后调用慢总线同步回调。
 */

#include <linux/irq.h>
#include <linux/msi.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/irqdomain.h>
#include <linux/preempt.h>
#include <linux/random.h>

#include <trace/events/irq.h>

#include "internals.h"

/*
 * bad_chained_irq() - 捕获级联父 IRQ 错误进入普通 action 分派的情况
 *
 * @irq: 发生违约的父逻辑 IRQ 号。
 * @dev_id: 未消费的设备标识指针；级联哨兵 action 不绑定真实设备。
 * 返回恒为 IRQ_NONE，并通过 WARN_ONCE 全局只报告一次。函数可在 hardirq 中调用、
 * 不睡眠；出现它说明父 IRQ 的 flow handler 错误调用了 action 链，而非子域分派器。
 */
static irqreturn_t bad_chained_irq(int irq, void *dev_id)
{
	WARN_ONCE(1, "Chained irq %d should not call an action\n", irq);
	return IRQ_NONE;
}

/*
 * Chained handlers should never call action on their IRQ. This default
 * action will emit warning if such thing happens.
 */
/*
 * 级联 handler 绝不应在父 IRQ 上调用普通 action；这个默认 action 在违约
 * 时发出告警。chained_action 是全局只读语义的哨兵，irq_desc_is_chained() 以指针身份
 * 识别它；desc 只借用其永久存储，不负责释放，也不能把它当设备注册的 irqaction。
 */
struct irqaction chained_action = {
	.handler = bad_chained_irq,
};

/**
 * irq_set_chip - set the irq chip for an irq
 * @irq:	irq number
 * @chip:	pointer to irq chip description structure
 */
/*
 * 给逻辑 IRQ @irq 安装 @chip 控制器描述。@chip 可为 NULL，此时改装
 * no_irq_chip 安全占位；非 NULL 对象通常具有静态或驱动全生命周期，核心只借用并
 * 去掉 const 存入 irq_data，不复制、不持模块引用。
 *
 * 函数在可睡眠配置上下文通过 scoped irqdesc guard 查找并锁定描述符；不存在返回
 * -EINVAL，成功返回 0。锁内只发布 chip，出锁后让静态数组配置登记该号码，并更新
 * proc 展示。它不启动 IRQ、不调用资源/PM 回调；调用者仍须设置 handler/domain。
 */
int irq_set_chip(unsigned int irq, const struct irq_chip *chip)
{
	/* ret 在 guard 未进入时保持 -EINVAL，进入并完成指针发布后改为 0。 */
	int ret = -EINVAL;

	scoped_irqdesc_get_and_lock(irq, 0) {
		scoped_irqdesc->irq_data.chip = (struct irq_chip *)(chip ?: &no_irq_chip);
		ret = 0;
	}
	if (!ret) {
		/* For !CONFIG_SPARSE_IRQ make the irq show up in allocated_irqs. */
		/* 非稀疏配置下把该 IRQ 加入 allocated_irqs 索引以供遍历。 */
		irq_mark_irq(irq);
		irq_proc_update_chip(chip);
	}
	return ret;
}
EXPORT_SYMBOL(irq_set_chip);

/**
 * irq_set_irq_type - set the irq trigger type for an irq
 * @irq:	irq number
 * @type:	IRQ_TYPE_{LEVEL,EDGE}_* value - see include/linux/irq.h
 */
/*
 * 把逻辑 IRQ @irq 的触发类型设置为 @type，合法值见 irq.h 的
 * IRQ_TYPE_LEVEL/EDGE_*。函数取得可选慢总线锁和 desc->lock，拒绝 per-CPU devid
 * 类型误用；成功或控制器错误原样返回 __irq_set_trigger() 结果，描述符不存在返回
 * -EINVAL。调用可触发 irq_chip::irq_set_type 并同步 flow handler/状态，不宜在 hardirq。
 */
int irq_set_irq_type(unsigned int irq, unsigned int type)
{
	scoped_irqdesc_get_and_buslock(irq, IRQ_GET_DESC_CHECK_GLOBAL)
		return __irq_set_trigger(scoped_irqdesc, type);
	return -EINVAL;
}
EXPORT_SYMBOL(irq_set_irq_type);

/**
 * irq_set_handler_data - set irq handler data for an irq
 * @irq:	Interrupt number
 * @data:	Pointer to interrupt specific data
 *
 * Set the hardware irq controller data for an irq
 */
/*
 * 为 @irq 设置中断控制器相关的 handler data。@data 是可空、由调用者拥有
 * 的裸指针，核心只存入 irq_common_data.handler_data；其生命周期必须覆盖所有 flow/
 * chained handler 读取。成功返回 0，不存在返回 -EINVAL；desc 锁保证指针原子发布，
 * 函数不释放旧值，也不调用 irq_chip。该字段不同于 irq_data.chip_data。
 */
int irq_set_handler_data(unsigned int irq, void *data)
{
	scoped_irqdesc_get_and_lock(irq, 0) {
		scoped_irqdesc->irq_common_data.handler_data = data;
		return 0;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(irq_set_handler_data);

/**
 * irq_set_msi_desc_off - set MSI descriptor data for an irq at offset
 * @irq_base:	Interrupt number base
 * @irq_offset:	Interrupt number offset
 * @entry:		Pointer to MSI descriptor data
 *
 * Set the MSI descriptor entry for an irq at offset
 */
/*
 * 把 MSI 描述符 @entry 关联到 @irq_base + @irq_offset。entry 可为 NULL 以
 * 清除关联；核心借用其生命周期。函数锁定目标全局 IRQ，成功返回 0，不存在或类型
 * 不匹配返回 -EINVAL。仅 offset 为 0 且 entry 非 NULL 时把 entry->irq 写成区间基号，
 * 后续向量共享同一 MSI desc 时不会反复覆盖；不配置 MSI 消息或硬件。
 */
int irq_set_msi_desc_off(unsigned int irq_base, unsigned int irq_offset, struct msi_desc *entry)
{
	scoped_irqdesc_get_and_lock(irq_base + irq_offset, IRQ_GET_DESC_CHECK_GLOBAL) {
		scoped_irqdesc->irq_common_data.msi_desc = entry;
		if (entry && !irq_offset)
			entry->irq = irq_base;
		return 0;
	}
	return -EINVAL;
}

/**
 * irq_set_msi_desc - set MSI descriptor data for an irq
 * @irq:	Interrupt number
 * @entry:	Pointer to MSI descriptor data
 *
 * Set the MSI descriptor entry for an irq
 */
/*
 * 给单个逻辑 IRQ @irq 设置 MSI 描述符 @entry。它只是 offset=0 的薄包装，
 * 因而成功时也会写 entry->irq=@irq；返回值、锁、借用所有权与 irq_set_msi_desc_off()
 * 相同，不新增副作用。
 */
int irq_set_msi_desc(unsigned int irq, struct msi_desc *entry)
{
	return irq_set_msi_desc_off(irq, 0, entry);
}

/**
 * irq_set_chip_data - set irq chip data for an irq
 * @irq:	Interrupt number
 * @data:	Pointer to chip specific data
 *
 * Set the hardware irq chip data for an irq
 */
/*
 * 为 @irq 设置 irq_chip 私有数据 @data。指针可空，所有权和释放责任仍在
 * 控制器驱动；desc 锁内发布到 irq_data.chip_data，成功返回 0，找不到 IRQ 返回
 * -EINVAL。该值传给 chip 回调，不能与面向 flow handler 的 handler_data 混用。
 */
int irq_set_chip_data(unsigned int irq, void *data)
{
	scoped_irqdesc_get_and_lock(irq, 0) {
		scoped_irqdesc->irq_data.chip_data = data;
		return 0;
	}
	return -EINVAL;
}
EXPORT_SYMBOL(irq_set_chip_data);

/*
 * irq_get_irq_data() - 由逻辑 IRQ 号借出其内嵌 irq_data
 *
 * @irq: 要查找的 Linux 逻辑中断号。
 * 返回 desc 存在时的 &desc->irq_data，否则 NULL；不增加描述符/domain 引用、不加锁、
 * 不睡眠。返回指针与 desc 同寿命，动态删除可能并发时调用者必须已有 RCU、全局锁或
 * 注册生命周期保护；修改字段还需遵守具体 desc->lock/irqchip 协议。
 */
struct irq_data *irq_get_irq_data(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc ? &desc->irq_data : NULL;
}
EXPORT_SYMBOL_GPL(irq_get_irq_data);

/*
 * irq_state_clr_disabled() - 在已完成 enable/startup 后清软件 disabled 位
 *
 * @desc: 输入输出的非 NULL 描述符，调用者持 desc->lock 并已完成硬件动作。
 * 无返回值、不睡眠；只清 IRQD_IRQ_DISABLED，不改变 depth、masked 或 started。
 */
static void irq_state_clr_disabled(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_DISABLED);
}

/*
 * irq_state_clr_masked() - 在硬件已 unmask/enable 后清软件 masked 位
 *
 * @desc: 输入输出的非 NULL、锁内描述符借用指针。
 * 无返回值、不调用硬件；必须紧随成功的 chip 动作，不能单独用来解屏蔽线路。
 */
static void irq_state_clr_masked(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_MASKED);
}

/*
 * irq_state_clr_started() - 发布 startup 生命周期已经结束
 *
 * @desc: 输入输出的非 NULL、锁内描述符；shutdown/disable 硬件动作已完成。
 * 无返回值，只清 IRQD_IRQ_STARTED；depth 和 activated 状态由相邻层分别维护。
 */
static void irq_state_clr_started(struct irq_desc *desc)
{
	irqd_clear(&desc->irq_data, IRQD_IRQ_STARTED);
}

/*
 * irq_state_set_started() - 发布 startup 生命周期已经建立
 *
 * @desc: 输入输出的非 NULL、锁内描述符；chip startup/enable 已经返回。
 * 无返回值，只置 IRQD_IRQ_STARTED。回调返回非零时实现仍置位，返回码由上层报告。
 */
static void irq_state_set_started(struct irq_desc *desc)
{
	irqd_set(&desc->irq_data, IRQD_IRQ_STARTED);
}

/*
 * managed startup 内部分支结果：NORMAL 表示普通 IRQ；MANAGED 表示有在线目标并已完成
 * domain 激活；ABORT 表示暂时没有在线目标或激活失败，调用者恢复 depth=1 和
 * MANAGED_SHUTDOWN。它们是控制流枚举，不是导出的 errno 或 irq_chip 返回值。
 */
enum {
	/* 非 managed IRQ，按常规 affinity/startup 顺序执行。 */
	IRQ_STARTUP_NORMAL,
	/* managed IRQ 已可启动，必须先把保留 affinity 编程到硬件。 */
	IRQ_STARTUP_MANAGED,
	/* 本次启动被管理策略中止，等待 CPU hotplug 条件变化。 */
	IRQ_STARTUP_ABORT,
};

#ifdef CONFIG_SMP
/*
 * __irq_startup_managed() - 判定并准备 affinity-managed IRQ 的首次启动
 *
 * @desc: 输入输出的非 NULL、锁内描述符借用指针。
 * @aff: 只读非 NULL 目标 CPU 掩码，通常指向 desc 内部 affinity。
 * @force: true 表示 enable/autoprobe/chained 等无条件请求，用于诊断非法 managed 用法。
 *
 * 普通 IRQ 返回 IRQ_STARTUP_NORMAL。managed IRQ 先清旧 shutdown 标志；目标含在线 CPU
 * 时激活 domain 并返回 MANAGED，目标全离线或激活失败返回 ABORT。函数不启动 chip、
 * 不改 depth；调用者根据枚举继续设置 affinity/startup 或恢复 managed shutdown。
 */
static int
__irq_startup_managed(struct irq_desc *desc, const struct cpumask *aff,
		      bool force)
{
	/* d 是 desc 内嵌 irq_data 的借用指针，整个调用都由 desc 生命周期和锁保护。 */
	struct irq_data *d = irq_desc_get_irq_data(desc);

	if (!irqd_affinity_is_managed(d))
		return IRQ_STARTUP_NORMAL;

	irqd_clr_managed_shutdown(d);

	if (!cpumask_intersects(aff, cpu_online_mask)) {
		/*
		 * Catch code which fiddles with enable_irq() on a managed
		 * and potentially shutdown IRQ. Chained interrupt
		 * installment or irq auto probing should not happen on
		 * managed irqs either.
		 */
		/*
		 * 捕获对可能处于 shutdown 的 managed IRQ 调用 enable_irq() 等
		 * 强制启动代码；级联 handler 安装和自动探测同样不应作用于 managed IRQ。
		 * force 只决定是否 WARN，不绕过“没有在线目标就不能启动”的安全条件。
		 */
		if (WARN_ON_ONCE(force))
			return IRQ_STARTUP_ABORT;
		/*
		 * The interrupt was requested, but there is no online CPU
		 * in it's affinity mask. Put it into managed shutdown
		 * state and let the cpu hotplug mechanism start it up once
		 * a CPU in the mask becomes available.
		 */
		/*
		 * IRQ 已被请求，但 affinity 中没有在线 CPU，于是让调用者重新置
		 * MANAGED_SHUTDOWN；CPU hotplug 在目标上线后调用 irq_startup_managed()。
		 * 返回 ABORT 不表示永久失败，也不丢失保留的 domain/affinity 资源。
		 */
		return IRQ_STARTUP_ABORT;
	}
	/*
	 * Managed interrupts have reserved resources, so this should not
	 * happen.
	 */
	/*
	 * managed IRQ 已预留资源，domain 激活失败按设计不应发生；WARN 后仍
	 * 中止本次 startup。激活成功后资源状态由 domain 持有，后续 deactivate 负责配对。
	 */
	if (WARN_ON(irq_domain_activate_irq(d, false)))
		return IRQ_STARTUP_ABORT;
	return IRQ_STARTUP_MANAGED;
}

/*
 * irq_startup_managed() - CPU 重新上线时恢复此前自动关闭的 managed IRQ
 *
 * @desc: 输入输出的非 NULL managed 描述符；CPU hotplug 路径持 desc->lock，并确认新
 *        在线 CPU 与 affinity 相交。函数无返回值、不睡眠、不转移所有权。
 *
 * 清 MANAGED_SHUTDOWN 后只撤销 hot-unplug 增加的一层 depth；仅当 depth 从 1 变 0 才
 * 条件启动并请求 resend。这样用户先 disable 后经历下线/上线时，剩余用户 disable
 * 深度仍阻止硬件被意外打开。
 */
void irq_startup_managed(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);

	/*
	 * Clear managed-shutdown flag, so we don't repeat managed-startup for
	 * multiple hotplugs, and cause imbalanced disable depth.
	 */
	/* 先清该标志，避免多次 hotplug 重复恢复并造成 disable depth 不平衡。 */
	irqd_clr_managed_shutdown(d);

	/*
	 * Only start it up when the disable depth is 1, so that a disable,
	 * hotunplug, hotplug sequence does not end up enabling it during
	 * hotplug unconditionally.
	 */
	/*
	 * 仅当当前 depth 为 1 时，本次递减才归零并启动；若用户已有额外
	 * disable 层，热插拔往返不能无条件启用 IRQ。调用协议保证 depth 至少为 1。
	 */
	desc->depth--;
	if (!desc->depth)
		irq_startup(desc, IRQ_RESEND, IRQ_START_COND);
}

#else
/*
 * __irq_startup_managed() - UP 配置下把所有 IRQ 视为普通启动
 *
 * @desc: 未消费的描述符借用指针。
 * @aff: 未消费的可空 affinity 借用指针。
 * @force: 未消费的强制启动标志。
 * 返回恒为 IRQ_STARTUP_NORMAL，不睡眠、无状态变化；UP 没有 CPU affinity hotplug 管理。
 */
static __always_inline int
__irq_startup_managed(struct irq_desc *desc, const struct cpumask *aff,
		      bool force)
{
	return IRQ_STARTUP_NORMAL;
}
#endif

/*
 * irq_enable() - 让已 started 的 IRQ 从 disabled/masked 状态恢复可接收
 *
 * @desc: 输入输出的非 NULL、锁内描述符；chip 和回调组合已在注册阶段校验。
 * 无返回值、不睡眠。软件未 disabled 时只尝试 unmask；否则先清 disabled，再优先调用
 * chip->irq_enable 并清 masked，缺少 enable 时退化为 unmask_irq()。它不改 depth、
 * STARTED 或 affinity；chip 回调没有错误返回，核心按完成处理。
 */
static void irq_enable(struct irq_desc *desc)
{
	if (!irqd_irq_disabled(&desc->irq_data)) {
		unmask_irq(desc);
	} else {
		irq_state_clr_disabled(desc);
		if (desc->irq_data.chip->irq_enable) {
			desc->irq_data.chip->irq_enable(&desc->irq_data);
			irq_state_clr_masked(desc);
		} else {
			unmask_irq(desc);
		}
	}
}

/*
 * __irq_startup() - 执行控制器首次 startup 并发布 started 状态
 *
 * @desc: 输入输出的非 NULL、锁内且已完成 domain 激活的描述符。
 * 返回 chip->irq_startup 的 unsigned 结果转换为 int；缺少 startup 时返回 0 并用
 * irq_enable() 回退。显式 startup 后清 disabled/masked；无论回调返回值如何都置
 * IRQD_IRQ_STARTED，因此非零返回用于报告控制器结果，不是自动回滚事务。
 */
static int __irq_startup(struct irq_desc *desc)
{
	/* d 是当前层 irq_data 借用指针；ret 只承载可选 startup 回调结果。 */
	struct irq_data *d = irq_desc_get_irq_data(desc);
	int ret = 0;

	/* Warn if this interrupt is not activated but try nevertheless */
	/* 若 IRQ 尚未激活则告警，但仍继续尝试 startup 以暴露调用顺序错误。 */
	WARN_ON_ONCE(!irqd_is_activated(d));

	if (d->chip->irq_startup) {
		ret = d->chip->irq_startup(d);
		irq_state_clr_disabled(desc);
		irq_state_clr_masked(desc);
	} else {
		irq_enable(desc);
	}
	irq_state_set_started(desc);
	return ret;
}

/*
 * irq_startup() - 按 managed/affinity 策略启动或重新启用 IRQ
 *
 * @desc: 输入输出的非 NULL、锁内描述符，domain/chip/action 配置已稳定。
 * @resend: true 时启动后检查并补发此前记录的 pending 事件。
 * @force: true 表示无条件调用意图；对无在线目标的 managed IRQ 只增加 WARN，仍中止。
 *
 * 函数先把 depth 置 0。已 STARTED 时仅 irq_enable()；首次启动则根据 managed 分类：
 * 普通 IRQ 按 IRQCHIP_AFFINITY_PRE_STARTUP 决定 affinity 在 startup 前还是后设置；
 * managed IRQ 先编程预留 affinity，再启动；ABORT 恢复 depth=1/MANAGED_SHUTDOWN 并
 * 返回 0。成功路径返回 chip startup 结果，可选执行 resend。affinity 设置返回值在这里
 * 不传播，因此 STARTED/硬件状态而非单一返回码才是判断实际启动结果的依据。
 */
int irq_startup(struct irq_desc *desc, bool resend, bool force)
{
	/* d/aff 都是 desc 内部借用；ret 传递首次 startup 结果，重复 enable 默认为 0。 */
	struct irq_data *d = irq_desc_get_irq_data(desc);
	const struct cpumask *aff = irq_data_get_affinity_mask(d);
	int ret = 0;

	desc->depth = 0;

	if (irqd_is_started(d)) {
		irq_enable(desc);
	} else {
		switch (__irq_startup_managed(desc, aff, force)) {
		case IRQ_STARTUP_NORMAL:
			if (d->chip->flags & IRQCHIP_AFFINITY_PRE_STARTUP)
				irq_setup_affinity(desc);
			ret = __irq_startup(desc);
			if (!(d->chip->flags & IRQCHIP_AFFINITY_PRE_STARTUP))
				irq_setup_affinity(desc);
			break;
		case IRQ_STARTUP_MANAGED:
			irq_do_set_affinity(d, aff, false);
			ret = __irq_startup(desc);
			break;
		case IRQ_STARTUP_ABORT:
			desc->depth = 1;
			irqd_set_managed_shutdown(d);
			return 0;
		}
	}
	if (resend)
		check_irq_resend(desc, false);

	return ret;
}

/*
 * irq_activate() - 在 startup 前激活非 managed IRQ 的 domain 层级
 *
 * @desc: 输入输出的非 NULL、已串行化描述符借用指针。
 * 普通 IRQ 返回 irq_domain_activate_irq() 的 0/负错误；managed IRQ 返回 0 并把实际
 * 激活延迟到存在在线目标的 __irq_startup_managed()。不改 depth、不启动 chip。
 */
int irq_activate(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);

	if (!irqd_affinity_is_managed(d))
		return irq_domain_activate_irq(d, false);
	return 0;
}

/*
 * irq_activate_and_startup() - 激活后以 FORCE 语义尝试启动 IRQ
 *
 * @desc: 输入输出的非 NULL、锁内描述符。
 * @resend: 是否在 startup 后补发 pending 事件。
 * 激活失败会 WARN、跳过 startup 并返回 0；否则返回 irq_startup() 结果。managed IRQ
 * 没有在线目标时也可能返回 0 且保持 shutdown，调用者须结合状态判断。用于 autoprobe
 * 和 chained handler 建立路径，不转移 desc 所有权。
 */
int irq_activate_and_startup(struct irq_desc *desc, bool resend)
{
	if (WARN_ON(irq_activate(desc)))
		return 0;
	return irq_startup(desc, resend, IRQ_START_FORCE);
}

/* __irq_disable() 前置声明：@mask 决定缺少 chip->irq_disable 时是否立即硬件 mask。 */
static void __irq_disable(struct irq_desc *desc, bool mask);

/*
 * irq_shutdown() - 结束一个已 started IRQ 的控制器生命周期
 *
 * @desc: 输入输出的非 NULL、锁内描述符。无返回值、不睡眠。
 * 未 started 时为空操作；否则清软件 resend、增加 depth，优先调用 chip shutdown 并
 * 发布 disabled/masked，缺少回调则强制 __irq_disable(mask=true)，最后清 STARTED。
 * 本函数不 deactivate domain，调用者可用 irq_shutdown_and_deactivate() 完成整套回收。
 */
void irq_shutdown(struct irq_desc *desc)
{
	if (irqd_is_started(&desc->irq_data)) {
		clear_irq_resend(desc);
		/*
		 * Increment disable depth, so that a managed shutdown on
		 * CPU hotunplug preserves the actual disabled state when the
		 * CPU comes back online. See irq_startup_managed().
		 */
		/*
		 * 增加 disable depth，使 CPU 下线触发的 managed shutdown 能保存
		 * 原先禁用层数；目标 CPU 回来后 irq_startup_managed() 只撤销这一层。
		 */
		desc->depth++;

		if (desc->irq_data.chip->irq_shutdown) {
			desc->irq_data.chip->irq_shutdown(&desc->irq_data);
			irq_state_set_disabled(desc);
			irq_state_set_masked(desc);
		} else {
			__irq_disable(desc, true);
		}
		irq_state_clr_started(desc);
	}
}


/*
 * irq_shutdown_and_deactivate() - 关闭控制器并无条件释放 domain 激活状态
 *
 * @desc: 输入输出的非 NULL、锁内描述符。无返回值；先按需 shutdown，再调用 domain
 * deactivate。domain 回调的资源释放/上下文约束由层级实现负责，本函数不改 action。
 */
void irq_shutdown_and_deactivate(struct irq_desc *desc)
{
	irq_shutdown(desc);
	/*
	 * This must be called even if the interrupt was never started up,
	 * because the activation can happen before the interrupt is
	 * available for request/startup. It has it's own state tracking so
	 * it's safe to call it unconditionally.
	 */
	/*
	 * 即使 IRQ 从未 startup 也必须 deactivate，因为 activation 可能早于
	 * request/startup；domain 自带状态追踪，所以无条件调用可安全配对而不会重复释放。
	 */
	irq_domain_deactivate_irq(&desc->irq_data);
}

/*
 * __irq_disable() - 发布 disabled，并按策略执行 chip disable 或 mask
 *
 * @desc: 输入输出的非 NULL、锁内描述符。
 * @mask: 缺少 irq_disable 时是否仍须立即 mask；false 实现 lazy disable。
 * 已 disabled 时仅在 @mask 为真补做 mask；首次禁用先置 disabled，有 irq_disable 就调用
 * 并置 masked，否则按 @mask 调 mask_irq()。无返回值，不改 depth/STARTED。
 */
static void __irq_disable(struct irq_desc *desc, bool mask)
{
	if (irqd_irq_disabled(&desc->irq_data)) {
		if (mask)
			mask_irq(desc);
	} else {
		irq_state_set_disabled(desc);
		if (desc->irq_data.chip->irq_disable) {
			desc->irq_data.chip->irq_disable(&desc->irq_data);
			irq_state_set_masked(desc);
		} else if (mask) {
			mask_irq(desc);
		}
	}
}

/**
 * irq_disable - Mark interrupt disabled
 * @desc:	irq descriptor which should be disabled
 *
 * If the chip does not implement the irq_disable callback, we
 * use a lazy disable approach. That means we mark the interrupt
 * disabled, but leave the hardware unmasked. That's an
 * optimization because we avoid the hardware access for the
 * common case where no interrupt happens after we marked it
 * disabled. If an interrupt happens, then the interrupt flow
 * handler masks the line at the hardware level and marks it
 * pending.
 *
 * If the interrupt chip does not implement the irq_disable callback,
 * a driver can disable the lazy approach for a particular irq line by
 * calling 'irq_set_status_flags(irq, IRQ_DISABLE_UNLAZY)'. This can
 * be used for devices which cannot disable the interrupt at the
 * device level under certain circumstances and have to use
 * disable_irq[_nosync] instead.
 */
/*
 * 把 @desc 标为禁用。若 chip 没有 irq_disable，默认采用 lazy disable：
 * 只置软件 disabled 而暂不访问硬件；若之后仍到达中断，flow handler 再 mask 并记录
 * pending。这避免“disable 后没有事件”的常见路径付出寄存器访问。
 *
 * 驱动可用 IRQ_DISABLE_UNLAZY 禁止这种优化，适用于设备侧无法可靠关中断、必须依赖
 * disable_irq[_nosync] 立即屏蔽控制器的情形。调用者持 desc->lock；函数无返回值、
 * 不调整嵌套 depth，外层 enable/disable API 负责计数。
 */
void irq_disable(struct irq_desc *desc)
{
	__irq_disable(desc, irq_settings_disable_unlazy(desc));
}

/*
 * irq_percpu_enable() - 在指定 CPU 上启用 per-CPU IRQ 并发布启用位
 *
 * @desc: 输入输出的非 NULL per-CPU devid 描述符，percpu_enabled 已分配。
 * @cpu: 要置位的有效 CPU 编号，调用者处于该 CPU 或已串行化其状态。
 * 无返回值、不睡眠；优先 chip enable，缺少时必须有 unmask，硬件动作后才设置 cpumask
 * 位。它不改全局 depth/disabled，因为每 CPU 启用状态由该掩码单独表达。
 */
void irq_percpu_enable(struct irq_desc *desc, unsigned int cpu)
{
	if (desc->irq_data.chip->irq_enable)
		desc->irq_data.chip->irq_enable(&desc->irq_data);
	else
		desc->irq_data.chip->irq_unmask(&desc->irq_data);
	cpumask_set_cpu(cpu, desc->percpu_enabled);
}

/*
 * irq_percpu_disable() - 在指定 CPU 上禁用 per-CPU IRQ 并清启用位
 *
 * @desc: 输入输出的非 NULL per-CPU devid 描述符。
 * @cpu: 要清位的有效 CPU 编号。
 * 无返回值；优先 chip disable，缺少时必须有 mask，完成硬件动作后清 percpu_enabled。
 * 调用者提供不可迁移/锁保护，函数不修改其他 CPU 的状态。
 */
void irq_percpu_disable(struct irq_desc *desc, unsigned int cpu)
{
	if (desc->irq_data.chip->irq_disable)
		desc->irq_data.chip->irq_disable(&desc->irq_data);
	else
		desc->irq_data.chip->irq_mask(&desc->irq_data);
	cpumask_clear_cpu(cpu, desc->percpu_enabled);
}

/*
 * mask_ack_irq() - 按 irq_chip 能力以正确次序完成 mask 与 acknowledge
 *
 * @desc: 输入输出的非 NULL、锁内描述符。无返回值、不睡眠。
 * 有原子 irq_mask_ack 时一次调用并置 masked；否则先经 mask_irq() 尝试屏蔽，再调用可选
 * irq_ack。先 mask 后 ack 防止电平源在确认窗口重复进入；ack 本身没有软件状态位。
 */
static inline void mask_ack_irq(struct irq_desc *desc)
{
	if (desc->irq_data.chip->irq_mask_ack) {
		desc->irq_data.chip->irq_mask_ack(&desc->irq_data);
		irq_state_set_masked(desc);
	} else {
		mask_irq(desc);
		if (desc->irq_data.chip->irq_ack)
			desc->irq_data.chip->irq_ack(&desc->irq_data);
	}
}

/*
 * mask_irq() - 若尚未屏蔽则调用 chip mask 并同步软件状态
 *
 * @desc: 输入输出的非 NULL、锁内描述符。已 masked 直接返回；chip 提供 irq_mask 时
 * 调用后置 IRQD_IRQ_MASKED。缺少回调则不改变软件位，因为核心不能声称硬件已屏蔽。
 * 无返回值、不调整 disabled/depth。
 */
void mask_irq(struct irq_desc *desc)
{
	if (irqd_irq_masked(&desc->irq_data))
		return;

	if (desc->irq_data.chip->irq_mask) {
		desc->irq_data.chip->irq_mask(&desc->irq_data);
		irq_state_set_masked(desc);
	}
}

/*
 * unmask_irq() - 若软件记录为 masked 则调用 chip unmask 并清状态
 *
 * @desc: 输入输出的非 NULL、锁内描述符。未 masked 或缺少 irq_unmask 时为空操作；
 * 回调完成后清 IRQD_IRQ_MASKED。无返回值，不等价于清 disabled 或修改 depth。
 */
void unmask_irq(struct irq_desc *desc)
{
	if (!irqd_irq_masked(&desc->irq_data))
		return;

	if (desc->irq_data.chip->irq_unmask) {
		desc->irq_data.chip->irq_unmask(&desc->irq_data);
		irq_state_clr_masked(desc);
	}
}

/*
 * unmask_threaded_irq() - 在线程化 oneshot 收尾时完成必要 EOI 并解屏蔽
 *
 * @desc: 输入输出的非 NULL、锁内描述符。若 chip 声明 IRQCHIP_EOI_THREADED，先在
 * 线程完成点发 EOI，再调用 unmask_irq()；否则 EOI 已由 hardirq 流处理器完成。
 * 无返回值、不睡眠，调用者保证所有相关 oneshot thread_mask 已清。
 */
void unmask_threaded_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = desc->irq_data.chip;

	if (chip->flags & IRQCHIP_EOI_THREADED)
		chip->irq_eoi(&desc->irq_data);

	unmask_irq(desc);
}

/* Busy wait until INPROGRESS is cleared */
/*
 * 原文意为“忙等直到 INPROGRESS 被清除”。
 * irq_wait_on_inprogress() - 在保留调用者锁契约的同时等待另一 CPU 完成本 IRQ
 *
 * @desc: 输入输出的非 NULL 描述符；进入和返回时都持 desc->lock，本地中断状态不变。
 * SMP 上循环解锁、cpu_relax() 等待，再重新加锁复核，以封闭“刚观察清零又被置位”的
 * 窗口；最终返回 IRQ 仍未 disabled 且仍有 action。等待期间其他任务可关闭/摘 action，
 * 因而返回 false。UP 不可能有另一 CPU 执行同一普通 IRQ，恒返回 false。不可睡眠。
 */
static bool irq_wait_on_inprogress(struct irq_desc *desc)
{
	if (IS_ENABLED(CONFIG_SMP)) {
		do {
			raw_spin_unlock(&desc->lock);
			while (irqd_irq_inprogress(&desc->irq_data))
				cpu_relax();
			raw_spin_lock(&desc->lock);
		} while (irqd_irq_inprogress(&desc->irq_data));

		/* Might have been disabled in meantime */
		/* 等待解锁期间 IRQ 可能已被禁用，因此必须在锁内重新检查。 */
		return !irqd_irq_disabled(&desc->irq_data) && desc->action;
	}
	return false;
}

/*
 * irq_can_handle_pm() - 处理 PM、轮询和 affinity 竞态后判断本 CPU 能否继续分派
 *
 * @desc: 输入输出的非 NULL、已持 desc->lock 的描述符。
 * 返回 true 表示可继续检查 action；false 表示本次事件已转为 wake/pending、应立即退出。
 * 函数不可睡眠，但在受限的 SMP edge 竞态中会暂释锁并忙等另一 CPU 的 INPROGRESS。
 */
static bool irq_can_handle_pm(struct irq_desc *desc)
{
	/* irqd 是内嵌状态借用；aff 仅在单目标 edge 迁移分支指向有效 affinity 掩码。 */
	struct irq_data *irqd = &desc->irq_data;
	const struct cpumask *aff;

	/*
	 * If the interrupt is not in progress and is not an armed
	 * wakeup interrupt, proceed.
	 */
	/* 既没有 handler 正在运行，也未 armed 为唤醒源时可直接继续。 */
	if (!irqd_has_set(irqd, IRQD_IRQ_INPROGRESS | IRQD_WAKEUP_ARMED))
		return true;

	/*
	 * If the interrupt is an armed wakeup source, mark it pending
	 * and suspended, disable it and notify the pm core about the
	 * event.
	 */
	/*
	 * armed 唤醒 IRQ 到达时，将它记为 pending/suspended、禁用线路并通知
	 * PM 核心；irq_pm_handle_wakeup() 维护 depth/state，本次不能再执行设备 action。
	 */
	if (unlikely(irqd_has_set(irqd, IRQD_WAKEUP_ARMED))) {
		irq_pm_handle_wakeup(desc);
		return false;
	}

	/* Check whether the interrupt is polled on another CPU */
	/* 检查该 IRQ 是否正在另一 CPU 的 spurious 轮询路径中执行。 */
	if (unlikely(desc->istate & IRQS_POLL_INPROGRESS)) {
		if (WARN_ONCE(irq_poll_cpu == smp_processor_id(),
			      "irq poll in progress on cpu %d for irq %d\n",
			      smp_processor_id(), desc->irq_data.irq))
			return false;
		return irq_wait_on_inprogress(desc);
	}

	/* The below works only for single target interrupts */
	/* 以下迁移竞态修复仅对具有有效 affinity 的单目标 edge IRQ 成立。 */
	if (!IS_ENABLED(CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK) ||
	    !irqd_is_single_target(irqd) || desc->handle_irq != handle_edge_irq)
		return false;

	/*
	 * If the interrupt affinity was moved to this CPU and the
	 * interrupt is currently handled on the previous target CPU, then
	 * busy wait for INPROGRESS to be cleared. Otherwise for edge type
	 * interrupts the handler might get stuck on the previous target:
	 *
	 * CPU 0			CPU 1 (new target)
	 * handle_edge_irq()
	 * repeat:
	 *	handle_event()		handle_edge_irq()
	 *			        if (INPROGESS) {
	 *				  set(PENDING);
	 *				  mask();
	 *				  return;
	 *				}
	 *	if (PENDING) {
	 *	  clear(PENDING);
	 *	  unmask();
	 *	  goto repeat;
	 *	}
	 *
	 * This happens when the device raises interrupts with a high rate
	 * and always before handle_event() completes and the CPU0 handler
	 * can clear INPROGRESS. This has been observed in virtual machines.
	 */
	/*
	 * 原文描述单目标 edge IRQ 迁移竞态：旧 CPU 尚在 action 窗口时，新目标 CPU 已收到
	 * 下一边沿；新 CPU 看到 INPROGRESS 后置 PENDING、mask 并返回。旧 CPU 随后可能因
	 * 高频事件不断在清 pending/unmask 与新事件之间循环，长期无法清 INPROGRESS，导致
	 * handler 卡在旧目标。这在虚拟机中被观察到。
	 *
	 * 只有当前 CPU 正是 effective affinity 的新目标时才等待旧 CPU 完成；否则返回
	 * false，避免任意 CPU 参与忙等。irq_wait_on_inprogress() 返回时还复核 action/disable。
	 */
	aff = irq_data_get_effective_affinity_mask(irqd);
	if (cpumask_first(aff) != smp_processor_id())
		return false;
	return irq_wait_on_inprogress(desc);
}

/*
 * irq_can_handle_actions() - 消费等待状态并检查设备 action 是否可执行
 *
 * @desc: 输入输出的非 NULL、锁内描述符。先清 REPLAY/WAITING，表示本次硬件入口已
 * 接管先前等待；若无 action 或 IRQ disabled，则置 PENDING 并返回 false，否则 true。
 * 无睡眠、不调用硬件。PENDING 让后续 enable/resend 或 edge 循环保留事件语义。
 */
static inline bool irq_can_handle_actions(struct irq_desc *desc)
{
	desc->istate &= ~(IRQS_REPLAY | IRQS_WAITING);

	if (unlikely(!desc->action || irqd_irq_disabled(&desc->irq_data))) {
		desc->istate |= IRQS_PENDING;
		return false;
	}
	return true;
}

/*
 * irq_can_handle() - 组合 PM/并发判定与 action 可执行判定
 *
 * @desc: 输入输出的非 NULL、锁内描述符。
 * 先调用 irq_can_handle_pm()；只有其返回 true 才检查 action/disabled。返回值直接指示
 * flow handler 是否应统计并进入 action 分派，pending/wakeup 副作用由两个子步骤完成。
 */
static inline bool irq_can_handle(struct irq_desc *desc)
{
	if (!irq_can_handle_pm(desc))
		return false;

	return irq_can_handle_actions(desc);
}

/**
 * handle_nested_irq - Handle a nested irq from a irq thread
 * @irq:	the interrupt number
 *
 * Handle interrupts which are nested into a threaded interrupt
 * handler. The handler function is called inside the calling threads
 * context.
 */
/*
 * 在线程化父 IRQ 的线程上下文中处理逻辑号 @irq 所代表的嵌套子 IRQ；
 * 子 action->thread_fn 直接在当前调用线程运行，而不是再唤醒一个 IRQ kthread。
 *
 * 调用者保证 @irq 有效且配置为 nested-thread。函数允许睡眠：先在 desc 锁下检查 action、
 * 增加统计和 threads_active，并借出稳定 action 链；解锁后顺序调用所有 thread_fn，按位
 * 合并返回值，再做可选 spurious 分析，最后递减活动计数并唤醒 synchronize_irq()。
 * 无直接返回值；设备回调的副作用归各 action 所有，函数不做 chip ack/eoi。
 */
void handle_nested_irq(unsigned int irq)
{
	/* desc/action 是注册生命周期内借用；action_ret 合并全部嵌套线程 handler 结果。 */
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action;
	irqreturn_t action_ret;

	/* 明确允许 thread_fn 睡眠，并让原子上下文误用在调试配置下尽早暴露。 */
	might_sleep();

	scoped_guard(raw_spinlock_irq, &desc->lock) {
		if (!irq_can_handle_actions(desc))
			return;

		action = desc->action;
		kstat_incr_irqs_this_cpu(desc);
		atomic_inc(&desc->threads_active);
	}

	/* threads_active 已发布后才离锁执行，释放路径会等待整个 action 遍历结束。 */
	action_ret = IRQ_NONE;
	for_each_action_of_desc(desc, action)
		action_ret |= action->thread_fn(action->irq, action->dev_id);

	if (!irq_settings_no_debug(desc))
		note_interrupt(desc, action_ret);

	/* 与锁内 atomic_inc() 配对；减到零时通知同步/释放等待者。 */
	wake_threads_waitq(desc);
}
EXPORT_SYMBOL_GPL(handle_nested_irq);

/**
 * handle_simple_irq - Simple and software-decoded IRQs.
 * @desc:	the interrupt description structure for this irq
 *
 * Simple interrupts are either sent from a demultiplexing interrupt
 * handler or come from hardware, where no interrupt hardware control is
 * necessary.
 *
 * Note: The caller is expected to handle the ack, clear, mask and unmask
 * issues if necessary.
 */
/*
 * 简单 IRQ 来自软件解复用或无需通用核心控制硬件流转的中断源；调用者须
 * 自行完成必要的 ack/clear/mask/unmask。@desc 是存活的非 NULL 描述符借用指针，
 * 函数在 hardirq 中取得 desc->lock，不睡眠、无返回值。
 *
 * PM/并发不允许处理时，只有声明“in-progress 期间需 resend”的控制器才置 PENDING；
 * action 不可执行时由 helper 自行置 pending。可执行时增加普通统计，并调用
 * handle_irq_event() 在暂时解锁窗口运行 action，返回时 guard 仍负责最终解锁。
 */
void handle_simple_irq(struct irq_desc *desc)
{
	guard(raw_spinlock)(&desc->lock);

	if (!irq_can_handle_pm(desc)) {
		if (irqd_needs_resend_when_in_progress(&desc->irq_data))
			desc->istate |= IRQS_PENDING;
		return;
	}

	if (!irq_can_handle_actions(desc))
		return;

	kstat_incr_irqs_this_cpu(desc);
	handle_irq_event(desc);
}
EXPORT_SYMBOL_GPL(handle_simple_irq);

/**
 * handle_untracked_irq - Simple and software-decoded IRQs.
 * @desc:	the interrupt description structure for this irq
 *
 * Untracked interrupts are sent from a demultiplexing interrupt handler
 * when the demultiplexer does not know which device it its multiplexed irq
 * domain generated the interrupt. IRQ's handled through here are not
 * subjected to stats tracking, randomness, or spurious interrupt
 * detection.
 *
 * Note: Like handle_simple_irq, the caller is expected to handle the ack,
 * clear, mask and unmask issues if necessary.
 */
/*
 * 当解复用器不能确定其复合 domain 中究竟哪个设备产生事件时，可经此入口
 * 调用简单 action；该事件不计 IRQ 统计、不采集随机性、也不做 spurious 检测。与
 * handle_simple_irq() 一样，调用者承担全部硬件 ack/clear/mask/unmask。
 *
 * @desc 是存活的非 NULL 借用对象。函数在 hardirq 中先锁定并检查可处理性，清旧
 * PENDING、置 INPROGRESS 后出锁，只调用最底层 __handle_irq_event_percpu()；最后重新
 * 加锁清 INPROGRESS。无返回值、不睡眠，action 返回值仅用于线程唤醒，不向外传播。
 */
void handle_untracked_irq(struct irq_desc *desc)
{
	scoped_guard(raw_spinlock, &desc->lock) {
		if (!irq_can_handle(desc))
			return;

		desc->istate &= ~IRQS_PENDING;
		irqd_set(&desc->irq_data, IRQD_IRQ_INPROGRESS);
	}

	__handle_irq_event_percpu(desc);

	scoped_guard(raw_spinlock, &desc->lock)
		irqd_clear(&desc->irq_data, IRQD_IRQ_INPROGRESS);
}
EXPORT_SYMBOL_GPL(handle_untracked_irq);

/*
 * Called unconditionally from handle_level_irq() and only for oneshot
 * interrupts from handle_fasteoi_irq()
 */
/*
 * handle_level_irq() 无条件调用本 helper，并称 fasteoi oneshot 也会使用。
 * 但当前实现的 handle_fasteoi_irq() 实际走独立的 cond_unmask_eoi_irq()；本 helper
 * 在本文件现有调用图中只由 handle_level_irq() 调用。
 * cond_unmask_irq() - 在线路仍可用且没有 oneshot 线程占用时解除屏蔽
 *
 * @desc: 输入输出的非 NULL、锁内描述符。IRQ 未 disabled、当前确实 masked 且
 * threads_oneshot 为零时调用 unmask_irq()；否则保留屏蔽。无返回值、不睡眠。
 */
static void cond_unmask_irq(struct irq_desc *desc)
{
	/*
	 * We need to unmask in the following cases:
	 * - Standard level irq (IRQF_ONESHOT is not set)
	 * - Oneshot irq which did not wake the thread (caused by a
	 *   spurious interrupt or a primary handler handling it
	 *   completely).
	 */
	/*
	 * 原文列出两种解屏蔽情况：普通 level IRQ 没有 ONESHOT；或 oneshot IRQ 本次没有
	 * 唤醒线程（虚假事件或 primary handler 已完整处理）。统一判据就是没有活跃
	 * threads_oneshot，且线路未被用户禁用。
	 */
	if (!irqd_irq_disabled(&desc->irq_data) &&
	    irqd_irq_masked(&desc->irq_data) && !desc->threads_oneshot)
		unmask_irq(desc);
}

/**
 * handle_level_irq - Level type irq handler
 * @desc:	the interrupt description structure for this irq
 *
 * Level type interrupts are active as long as the hardware line has the
 * active level. This may require to mask the interrupt and unmask it after
 * the associated handler has acknowledged the device, so the interrupt
 * line is back to inactive.
 */
/*
 * 电平 IRQ 在硬件线路维持有效电平期间会持续触发，所以进入时通常必须
 * mask+ack，待设备 handler 清除源、线路回到非活动电平后再 unmask。
 *
 * @desc 是非 NULL、存活描述符；函数在 hardirq 中持 desc->lock，先 mask_ack，无论
 * action 后续是否可运行都阻断重复电平。可处理时增加统计，handle_irq_event() 暂释锁
 * 执行 action；返回后只有未 disabled 且无 oneshot 线程时才解屏蔽。无返回值、不睡眠。
 */
void handle_level_irq(struct irq_desc *desc)
{
	guard(raw_spinlock)(&desc->lock);
	mask_ack_irq(desc);

	if (!irq_can_handle(desc))
		return;

	kstat_incr_irqs_this_cpu(desc);
	handle_irq_event(desc);

	cond_unmask_irq(desc);
}
EXPORT_SYMBOL_GPL(handle_level_irq);

/*
 * cond_unmask_eoi_irq() - 完成 fasteoi IRQ 的 EOI 与 oneshot 解屏蔽协议
 *
 * @desc: 输入输出的非 NULL、锁内描述符。
 * @chip: 只读借用的非 NULL 当前 irq_chip，必须提供 irq_eoi。
 * 非 oneshot 直接 EOI。oneshot 在未 disabled、已 masked 且没有活跃线程时先 EOI 再
 * unmask；仍需线程时，若 chip 不要求线程点 EOI 则只 EOI，否则把 EOI 延迟到
 * unmask_threaded_irq()。无返回值、不睡眠，顺序防止 EOI 后线路过早重入。
 */
static void cond_unmask_eoi_irq(struct irq_desc *desc, struct irq_chip *chip)
{
	if (!(desc->istate & IRQS_ONESHOT)) {
		chip->irq_eoi(&desc->irq_data);
		return;
	}
	/*
	 * We need to unmask in the following cases:
	 * - Oneshot irq which did not wake the thread (caused by a
	 *   spurious interrupt or a primary handler handling it
	 *   completely).
	 */
	/*
	 * oneshot 只有在本次未唤醒线程（虚假 IRQ 或 primary 已完全处理）时
	 * 才能立即解屏蔽；否则必须保持 mask，直至最后一个线程完成。
	 */
	if (!irqd_irq_disabled(&desc->irq_data) &&
	    irqd_irq_masked(&desc->irq_data) && !desc->threads_oneshot) {
		chip->irq_eoi(&desc->irq_data);
		unmask_irq(desc);
	} else if (!(chip->flags & IRQCHIP_EOI_THREADED)) {
		chip->irq_eoi(&desc->irq_data);
	}
}

/*
 * cond_eoi_irq() - 按 IRQCHIP_EOI_IF_HANDLED 策略决定异常退出是否 EOI
 *
 * @chip: 只读借用的非 NULL irq_chip。
 * @data: 输入输出的非 NULL irq_data，传给 chip->irq_eoi。
 * chip 未要求“仅已处理才 EOI”时立即调用；设置该 flag 时跳过，让未处理状态留给硬件/
 * 后续路径。调用者保证 irq_eoi 存在；无返回值、不睡眠。
 */
static inline void cond_eoi_irq(struct irq_chip *chip, struct irq_data *data)
{
	if (!(chip->flags & IRQCHIP_EOI_IF_HANDLED))
		chip->irq_eoi(data);
}

/**
 * handle_fasteoi_irq - irq handler for transparent controllers
 * @desc:	the interrupt description structure for this irq
 *
 * Only a single callback will be issued to the chip: an ->eoi() call when
 * the interrupt has been serviced. This enables support for modern forms
 * of interrupt handlers, which handle the flow details in hardware,
 * transparently.
 */
/*
 * 透明控制器的 IRQ 流只需在服务结束时调用一次 ->irq_eoi()，现代控制器
 * 自己处理其余流控细节。@desc 是非 NULL、存活描述符；hardirq 中持 desc->lock，
 * 不睡眠、无返回值，chip 必须提供符合 fasteoi 契约的 irq_eoi。
 *
 * PM/迁移竞态不能处理时按 chip 策略 EOI，必要时记录 PENDING；无 action/disabled 时
 * mask 后条件 EOI。正常路径计数，oneshot 先 mask，暂释锁运行 action，再依据线程状态
 * EOI/unmask；若迁移竞态留下 PENDING，最后请求软件 resend。
 */
void handle_fasteoi_irq(struct irq_desc *desc)
{
	/* chip 在 desc 锁和 IRQ 注册生命周期内稳定，整个函数只借用。 */
	struct irq_chip *chip = desc->irq_data.chip;

	guard(raw_spinlock)(&desc->lock);

	/*
	 * When an affinity change races with IRQ handling, the next interrupt
	 * can arrive on the new CPU before the original CPU has completed
	 * handling the previous one - it may need to be resent.
	 */
	/*
	 * affinity 改变与处理中断竞态时，新 CPU 可在旧 CPU 完成前收到下一次
	 * 中断；若当前不能分派，这个事件可能需要记为 pending 并在稍后重发。
	 */
	if (!irq_can_handle_pm(desc)) {
		if (irqd_needs_resend_when_in_progress(&desc->irq_data))
			desc->istate |= IRQS_PENDING;
		cond_eoi_irq(chip, &desc->irq_data);
		return;
	}

	if (!irq_can_handle_actions(desc)) {
		mask_irq(desc);
		cond_eoi_irq(chip, &desc->irq_data);
		return;
	}

	kstat_incr_irqs_this_cpu(desc);
	if (desc->istate & IRQS_ONESHOT)
		mask_irq(desc);

	handle_irq_event(desc);

	cond_unmask_eoi_irq(desc, chip);

	/*
	 * When the race described above happens this will resend the interrupt.
	 */
	/* 若上述迁移竞态留下 PENDING，就在完成 EOI/unmask 后尝试重发。 */
	if (unlikely(desc->istate & IRQS_PENDING))
		check_irq_resend(desc, false);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_irq);

/**
 *	handle_fasteoi_nmi - irq handler for NMI interrupt lines
 *	@desc:	the interrupt description structure for this irq
 *
 *	A simple NMI-safe handler, considering the restrictions
 *	from request_nmi.
 *
 *	Only a single callback will be issued to the chip: an ->eoi()
 *	call when the interrupt has been serviced. This enables support
 *	for modern forms of interrupt handlers, which handle the flow
 *	details in hardware, transparently.
 */
/*
 * 这是满足 request_nmi() 限制的简单 NMI-safe fasteoi handler；控制器透明
 * 管理流细节，服务完成后至多调用一次 EOI。@desc 必须配置为 NMI、只有一个 action，
 * action/chip 均在 NMI 注册生命周期内稳定。
 *
 * 函数不取 desc->lock、不触碰普通 tot_count、随机性或 spurious 状态；只增加当前 CPU
 * per-CPU 统计，发 tracepoint，直接调用唯一 primary handler，再调用可选 irq_eoi。
 * 运行于 NMI，不可睡眠，所有回调和追踪点必须 NMI-safe；handler 返回值仅供 trace。
 */
void handle_fasteoi_nmi(struct irq_desc *desc)
{
	/* chip/action 是 NMI 生命周期稳定的借用指针；irq/res 是本次调用的值状态。 */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct irqaction *action = desc->action;
	unsigned int irq = irq_desc_get_irq(desc);
	irqreturn_t res;

	__kstat_incr_irqs_this_cpu(desc);

	trace_irq_handler_entry(irq, action);
	/*
	 * NMIs cannot be shared, there is only one action.
	 */
	/* NMI 禁止共享，因此无需遍历 action 链，desc->action 就是唯一处理器。 */
	res = action->handler(irq, action->dev_id);
	trace_irq_handler_exit(irq, action, res);

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_nmi);

/**
 * handle_edge_irq - edge type IRQ handler
 * @desc:	the interrupt description structure for this irq
 *
 * Interrupt occurs on the falling and/or rising edge of a hardware
 * signal. The occurrence is latched into the irq controller hardware and
 * must be acked in order to be reenabled. After the ack another interrupt
 * can happen on the same source even before the first one is handled by
 * the associated event handler. If this happens it might be necessary to
 * disable (mask) the interrupt depending on the controller hardware. This
 * requires to reenable the interrupt inside of the loop which handles the
 * interrupts which have arrived while the handler was running. If all
 * pending interrupts are handled, the loop is left.
 */
/*
 * edge IRQ 把上/下降沿锁存在控制器中，必须 ack 才能接受下一沿；第一轮
 * action 尚未完成时又可到达新沿，控制器可能因此 mask。flow handler 要在循环中消费
 * PENDING、按需 unmask，直到没有新沿或 IRQ 被禁用。
 *
 * @desc 是非 NULL、存活描述符；hardirq 中持 desc->lock，不睡眠、无返回值。不能处理
 * 时置 PENDING 并 mask+ack；正常时只为硬件入口计数一次并先 ack。循环每轮确认 action
 * 仍存在，恢复竞态产生的 mask，调用 handle_irq_event()；其入口会清 PENDING，执行期间
 * 新事件可再次置位。用户禁用后立即停止循环并保留状态。
 */
void handle_edge_irq(struct irq_desc *desc)
{
	guard(raw_spinlock)(&desc->lock);

	if (!irq_can_handle(desc)) {
		desc->istate |= IRQS_PENDING;
		mask_ack_irq(desc);
		return;
	}

	kstat_incr_irqs_this_cpu(desc);

	/* Start handling the irq */
	/* 原文意为“开始处理 IRQ”：先确认当前锁存边沿，让控制器可观察下一事件。 */
	desc->irq_data.chip->irq_ack(&desc->irq_data);

	do {
		if (unlikely(!desc->action)) {
			mask_irq(desc);
			return;
		}

		/*
		 * When another irq arrived while we were handling
		 * one, we could have masked the irq.
		 * Reenable it, if it was not disabled in meantime.
		 */
		/*
		 * 本轮 action 执行期间若又到达 IRQ，竞态路径可能已 mask；只在没有
		 * 同时被用户禁用时重新 unmask，使循环可以继续消费已记录边沿。
		 */
		if (unlikely(desc->istate & IRQS_PENDING)) {
			if (!irqd_irq_disabled(&desc->irq_data) &&
			    irqd_irq_masked(&desc->irq_data))
				unmask_irq(desc);
		}

		handle_irq_event(desc);

	} while ((desc->istate & IRQS_PENDING) && !irqd_irq_disabled(&desc->irq_data));
}
EXPORT_SYMBOL(handle_edge_irq);

/**
 *	handle_percpu_irq - Per CPU local irq handler
 *	@desc:	the interrupt description structure for this irq
 *
 *	Per CPU interrupts on SMP machines without locking requirements
 */
/*
 * 处理 SMP 上无需全局锁串行的 per-CPU 本地 IRQ。@desc 是注册生命周期内
 * 稳定的共享描述符，但各 CPU 可同时进入；因此不取 desc->lock，也不修改共享
 * tot_count。函数在 hardirq 中不可睡眠：增加当前 CPU 统计，调用可选 ack，使用
 * handle_irq_event_percpu() 分派固定 action 链并做通用收尾，最后调用可选 eoi。
 */
void handle_percpu_irq(struct irq_desc *desc)
{
	/* chip 具有 IRQ 注册期借用寿命，回调必须支持每 CPU 并发。 */
	struct irq_chip *chip = irq_desc_get_chip(desc);

	/*
	 * PER CPU interrupts are not serialized. Do not touch
	 * desc->tot_count.
	 */
	/* per-CPU IRQ 不相互串行，故只能更新当前 CPU 计数，不能竞争 tot_count。 */
	__kstat_incr_irqs_this_cpu(desc);

	if (chip->irq_ack)
		chip->irq_ack(&desc->irq_data);

	handle_irq_event_percpu(desc);

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}

/**
 * handle_percpu_devid_irq - Per CPU local irq handler with per cpu dev ids
 * @desc:	the interrupt description structure for this irq
 *
 * Per CPU interrupts on SMP machines without locking requirements. Same as
 * handle_percpu_irq() above but with the following extras:
 *
 * action->percpu_dev_id is a pointer to percpu variables which
 * contain the real device id for the cpu on which this handler is
 * called.
 *
 * May be used for NMI interrupt lines, and so may be called in IRQ or NMI
 * context.
 */
/*
 * 这是带 per-CPU dev_id 的本地 IRQ 入口，仍允许多 CPU 无锁并发；它从
 * action->percpu_dev_id 取当前 CPU 实例传给 handler。该入口也可用于 NMI，因此只做
 * NMI-safe 操作，并在 NMI 中跳过随机性采样。
 *
 * @desc 是非 NULL、长期稳定的 per-CPU devid 描述符。函数增加当前 CPU 统计并可选 ack，
 * 从 action 链选择 affinity 包含当前 CPU 的项；找到后发 trace 并调用 primary handler。
 * 找不到时，若该 CPU 仍标记 enabled 就立即硬件 disable 并清位，随后只告警一次，防止
 * 无 action 的本地 IRQ 风暴。最后在普通 hardirq 中采样随机性，并可选 eoi。无返回值。
 */
void handle_percpu_devid_irq(struct irq_desc *desc)
{
	/* chip/irq/cpu 是本次入口稳定快照；action/res 只在选择并调用的分支有效。 */
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int irq = irq_desc_get_irq(desc);
	unsigned int cpu = smp_processor_id();
	struct irqaction *action;
	irqreturn_t res;

	/*
	 * PER CPU interrupts are not serialized. Do not touch
	 * desc->tot_count.
	 */
	/* 各 CPU 可并发处理，不能修改共享 tot_count，只更新本 CPU 统计。 */
	__kstat_incr_irqs_this_cpu(desc);

	if (chip->irq_ack)
		chip->irq_ack(&desc->irq_data);

	for (action = desc->action; action; action = action->next)
		if (cpumask_test_cpu(cpu, action->affinity))
			break;

	if (likely(action)) {
		trace_irq_handler_entry(irq, action);
		res = action->handler(irq, raw_cpu_ptr(action->percpu_dev_id));
		trace_irq_handler_exit(irq, action, res);
	} else {
		/* enabled 快照决定是否需先关闭硬件，并影响告警中“and unmasked”诊断。 */
		bool enabled = cpumask_test_cpu(cpu, desc->percpu_enabled);

		if (enabled)
			irq_percpu_disable(desc, cpu);

		pr_err_once("Spurious%s percpu IRQ%u on CPU%u\n",
			    enabled ? " and unmasked" : "", irq, cpu);
	}

	/* 随机子系统入口不是 NMI-safe 契约，NMI 只保留统计和设备处理。 */
	if (!in_nmi())
		add_interrupt_randomness(irq);

	if (chip->irq_eoi)
		chip->irq_eoi(&desc->irq_data);
}

/*
 * __irq_do_set_handler() - 在锁内安装、替换或卸载描述符的高层 flow handler
 *
 * @desc: 输入输出的非 NULL 描述符；调用者持 desc->lock，通常也持 chip 慢总线锁。
 * @handle: 可空 flow handler；NULL 归一化为 handle_bad_irq，表示卸载有效处理器。
 * @is_chained: 非零表示级联父 IRQ，安装后必须立即激活/startup 且不允许 request/thread。
 * @name: 可空、长期存活的只读名称借用指针，供 proc/debug 输出。
 *
 * 无返回值；chip 层级校验失败会 WARN 并在发布前返回，后续 PM/startup 不变量失败则
 * 不回滚已经安装的配置。安装普通 handler 只发布函数和名称；卸载会 mask/ack、标
 * disabled、恢复 depth=1，并为旧 chained 配置撤 action/PM 引用。安装
 * chained handler 还同步触发类型、设置策略位、挂全局哨兵 action、尝试取得 PM 引用并
 * 立即 activate/startup。PM get 失败只 WARN，startup 结果也不传播：这些都被视为级联
 * 控制器建立阶段不应失败的不变量。函数不可睡眠于 raw 锁区，相关回调须满足该约束。
 */
static void
__irq_do_set_handler(struct irq_desc *desc, irq_flow_handler_t handle,
		     int is_chained, const char *name)
{
	if (!handle) {
		/* NULL 是公开 API 的卸载表示，内部统一使用可安全诊断的 bad handler。 */
		handle = handle_bad_irq;
	} else {
		/* irq_data 沿层级向 parent 借用遍历，只用于确认至少一层已安装真实 chip。 */
		struct irq_data *irq_data = &desc->irq_data;
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
		/*
		 * With hierarchical domains we might run into a
		 * situation where the outermost chip is not yet set
		 * up, but the inner chips are there.  Instead of
		 * bailing we install the handler, but obviously we
		 * cannot enable/startup the interrupt at this point.
		 */
		/*
		 * 层级 domain 可能出现最外层 chip 尚未建立、内层 chip 已存在的阶段；
		 * 普通 handler 可以先安装，但此时显然不能启用/startup。代码沿 parent 寻找首个
		 * 非 no_irq_chip，以区分“顶层占位”与“整个层级都未配置”。
		 */
		while (irq_data) {
			if (irq_data->chip != &no_irq_chip)
				break;
			/*
			 * Bail out if the outer chip is not set up
			 * and the interrupt supposed to be started
			 * right away.
			 */
			/*
			 * 若最外层 chip 未就绪而本次是必须立即启动的 chained IRQ，就告警
			 * 并退出；级联父不能只安装 handler 后等待，因为子中断入口依赖它立刻工作。
			 */
			if (WARN_ON(is_chained))
				return;
			/* Try the parent */
			/* 当前层仍是占位 chip，继续检查直接 parent。 */
			irq_data = irq_data->parent_data;
		}
#endif
		if (WARN_ON(!irq_data || irq_data->chip == &no_irq_chip))
			return;
	}

	/* Uninstall? */
	/* 原文意为“是否卸载”：handle_bad_irq 是内部统一的无有效 flow handler 标记。 */
	if (handle == handle_bad_irq) {
		if (desc->irq_data.chip != &no_irq_chip)
			mask_ack_irq(desc);
		irq_state_set_disabled(desc);
		if (is_chained) {
			desc->action = NULL;
			irq_chip_pm_put(irq_desc_get_irq_data(desc));
		}
		desc->depth = 1;
	}
	/* 发布阶段：handler 与诊断名称作为一组在 desc 锁下对后续流处理可见。 */
	desc->handle_irq = handle;
	desc->name = name;

	if (handle != handle_bad_irq && is_chained) {
		/* type 是已记录触发类型的值副本，用来在立即启动前重新编程控制器。 */
		unsigned int type = irqd_get_trigger_type(&desc->irq_data);

		/*
		 * We're about to start this interrupt immediately,
		 * hence the need to set the trigger configuration.
		 * But the .set_type callback may have overridden the
		 * flow handler, ignoring that we're dealing with a
		 * chained interrupt. Reset it immediately because we
		 * do know better.
		 */
		/*
		 * 级联 IRQ 即将立即启动，所以必须先应用已有触发配置；但 chip 的
		 * set_type 回调可能根据 edge/level 擅自替换 flow handler，忽略这是 chained
		 * 父 IRQ。核心掌握更完整语义，因此回调后立刻把 handle 恢复为调用者指定值。
		 */
		if (type != IRQ_TYPE_NONE) {
			__irq_set_trigger(desc, type);
			desc->handle_irq = handle;
		}

		/* 级联父由内核内部独占：禁止 probe、request 和线程化，并挂身份哨兵 action。 */
		irq_settings_set_noprobe(desc);
		irq_settings_set_norequest(desc);
		irq_settings_set_nothread(desc);
		desc->action = &chained_action;
		WARN_ON(irq_chip_pm_get(irq_desc_get_irq_data(desc)));
		irq_activate_and_startup(desc, IRQ_RESEND);
	}
	irq_proc_update_valid(desc);
}

/*
 * __irq_set_handler() - 按逻辑号锁定描述符并配置 flow handler
 *
 * @irq: 目标逻辑 IRQ。
 * @handle: 可空 handler，NULL 表示恢复 handle_bad_irq。
 * @is_chained: 是否按级联父 IRQ 协议安装。
 * @name: 可空、长期存活的 handler 名称借用指针。
 * 无返回值；找不到 IRQ 时 guard 不进入而静默结束。成功路径持 bus lock/desc lock 调用
 * __irq_do_set_handler()，可触发 chip/PM/startup 副作用，调用者应处于可睡眠配置上下文。
 */
void __irq_set_handler(unsigned int irq, irq_flow_handler_t handle, int is_chained,
		       const char *name)
{
	scoped_irqdesc_get_and_buslock(irq, 0)
		__irq_do_set_handler(scoped_irqdesc, handle, is_chained, name);
}
EXPORT_SYMBOL_GPL(__irq_set_handler);

/*
 * irq_set_chained_handler_and_data() - 原子发布级联 handler data 并安装父处理器
 *
 * @irq: 父逻辑 IRQ。
 * @handle: 可空级联 flow handler；NULL 表示卸载。
 * @data: 可空、由调用者拥有且寿命覆盖 handler 的私有数据借用指针。
 * 无返回值；在同一 bus/desc 锁区先写 handler_data，再按 chained 协议配置 handler，
 * 避免新 handler 看见旧 data。若后续配置因 WARN 条件提前返回，data 写入不会回滚。
 */
void irq_set_chained_handler_and_data(unsigned int irq, irq_flow_handler_t handle,
				      void *data)
{
	scoped_irqdesc_get_and_buslock(irq, 0) {
		struct irq_desc *desc = scoped_irqdesc;

		desc->irq_common_data.handler_data = data;
		__irq_do_set_handler(desc, handle, 1, NULL);
	}
}
EXPORT_SYMBOL_GPL(irq_set_chained_handler_and_data);

/*
 * irq_set_chip_and_handler_name() - 依次安装 irq_chip 与普通 flow handler
 *
 * @irq: 目标逻辑 IRQ。
 * @chip: 可空、长期存活的控制器描述借用指针。
 * @handle: 可空普通 flow handler。
 * @name: 可空、长期存活的 flow handler 名称。
 * 无返回值；这是两个公开设置器的便利包装，二者分别加锁，且忽略 irq_set_chip() 的
 * -EINVAL。因此调用者须保证 IRQ 有效；函数不提供跨两步的原子发布或失败回滚。
 */
void
irq_set_chip_and_handler_name(unsigned int irq, const struct irq_chip *chip,
			      irq_flow_handler_t handle, const char *name)
{
	irq_set_chip(irq, chip);
	__irq_set_handler(irq, handle, 0, name);
}
EXPORT_SYMBOL_GPL(irq_set_chip_and_handler_name);

/*
 * irq_modify_status() - 更新描述符策略位并重建 irq_data 镜像状态
 *
 * @irq: 目标逻辑 IRQ；不存在时静默结束。
 * @clr: 要从 status_use_accessors 清除的位集合。
 * @set: 要设置的位集合；与 @clr 同时包含某位时，因为先清后置，最终该位为 1。
 *
 * 函数持 desc->lock，不调用硬件、不睡眠、无返回值；不在 _IRQF_MODIFY_MASK 中的
 * clr/set 位由 settings helper 忽略。先更新设置位，再从中重建
 * IRQD_NO_BALANCING/PER_CPU/LEVEL 和触发类型；若新设置未给触发掩码则保留旧 irq_data
 * trigger。最后更新 proc 可见资格。已 active IRQ 再设置 NOAUTOEN 会 WARN，因为它只
 * 影响未来自动启动，不能倒退关闭当前线路。
 */
void irq_modify_status(unsigned int irq, unsigned long clr, unsigned long set)
{
	scoped_irqdesc_get_and_lock(irq, 0) {
		struct irq_desc *desc = scoped_irqdesc;
		/* trigger 保存旧/新最终触发值；tmp 只承载策略字段中新提供的触发掩码。 */
		unsigned long trigger, tmp;
		/*
		 * Warn when a driver sets the no autoenable flag on an already
		 * active interrupt.
		 */
		/* 驱动若在 IRQ 已启用（depth=0）后才设置 NOAUTOEN，就发出告警。 */
		WARN_ON_ONCE(!desc->depth && (set & _IRQ_NOAUTOEN));

		irq_settings_clr_and_set(desc, clr, set);

		trigger = irqd_get_trigger_type(&desc->irq_data);

		irqd_clear(&desc->irq_data, IRQD_NO_BALANCING | IRQD_PER_CPU |
			   IRQD_TRIGGER_MASK | IRQD_LEVEL);
		if (irq_settings_has_no_balance_set(desc))
			irqd_set(&desc->irq_data, IRQD_NO_BALANCING);
		if (irq_settings_is_per_cpu(desc))
			irqd_set(&desc->irq_data, IRQD_PER_CPU);
		if (irq_settings_is_level(desc))
			irqd_set(&desc->irq_data, IRQD_LEVEL);

		tmp = irq_settings_get_trigger_mask(desc);
		if (tmp != IRQ_TYPE_NONE)
			trigger = tmp;

		irqd_set(&desc->irq_data, trigger);
		irq_proc_update_valid(desc);
	}
}
EXPORT_SYMBOL_GPL(irq_modify_status);

#ifdef CONFIG_DEPRECATED_IRQ_CPU_ONOFFLINE
/**
 *	irq_cpu_online - Invoke all irq_cpu_online functions.
 *
 *	Iterate through all irqs and invoke the chip.irq_cpu_online()
 *	for each.
 */
/*
 * 遍历所有 active IRQ，并逐一调用可用的 chip->irq_cpu_online()。
 * 入参、返回值：无。CPU hotplug 旧式兼容路径调用；每项重新查 desc，在 irqsave 锁内
 * 借出 chip。默认无论 IRQ enabled 与否都调用；chip 设置 IRQCHIP_ONOFFLINE_ENABLED
 * 时只对未 disabled IRQ 调用。回调不可睡眠，描述符在 active 遍历协议内存活。
 */
void irq_cpu_online(void)
{
	/* irq 是 active 索引值；desc/chip 是每轮在锁与注册生命周期内的借用指针。 */
	unsigned int irq;

	for_each_active_irq(irq) {
		struct irq_desc *desc = irq_to_desc(irq);
		struct irq_chip *chip;

		if (!desc)
			continue;

		guard(raw_spinlock_irqsave)(&desc->lock);
		chip = irq_data_get_irq_chip(&desc->irq_data);
		if (chip && chip->irq_cpu_online &&
		    (!(chip->flags & IRQCHIP_ONOFFLINE_ENABLED) ||
		     !irqd_irq_disabled(&desc->irq_data)))
			chip->irq_cpu_online(&desc->irq_data);
	}
}

/**
 *	irq_cpu_offline - Invoke all irq_cpu_offline functions.
 *
 *	Iterate through all irqs and invoke the chip.irq_cpu_offline()
 *	for each.
 */
/*
 * 遍历所有 active IRQ，并调用可用的 chip->irq_cpu_offline()。
 * 锁、过滤条件、无返回值及回调上下文与 irq_cpu_online() 相同；它只通知控制器旧式
 * CPU 下线事件，不替代通用 affinity migration，也不删除描述符。
 */
void irq_cpu_offline(void)
{
	/* irq 是 active 索引值；desc/chip 仅在当前迭代与锁区有效。 */
	unsigned int irq;

	for_each_active_irq(irq) {
		struct irq_desc *desc = irq_to_desc(irq);
		struct irq_chip *chip;

		if (!desc)
			continue;

		guard(raw_spinlock_irqsave)(&desc->lock);
		chip = irq_data_get_irq_chip(&desc->irq_data);
		if (chip && chip->irq_cpu_offline &&
		    (!(chip->flags & IRQCHIP_ONOFFLINE_ENABLED) ||
		     !irqd_irq_disabled(&desc->irq_data)))
			chip->irq_cpu_offline(&desc->irq_data);
	}
}
#endif
/* 关闭 CONFIG_DEPRECATED_IRQ_CPU_ONOFFLINE 时，CPU 热插拔改由非旧式通用机制处理。 */

#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY

#ifdef CONFIG_IRQ_FASTEOI_HIERARCHY_HANDLERS
/**
 * handle_fasteoi_ack_irq - irq handler for edge hierarchy stacked on
 *			    transparent controllers
 *
 * @desc:	the interrupt description structure for this irq
 *
 * Like handle_fasteoi_irq(), but for use with hierarchy where the irq_chip
 * also needs to have its ->irq_ack() function called.
 */
/*
 * 该入口类似 handle_fasteoi_irq()，用于“edge 子层叠在透明父控制器”场景，
 * 额外要求当前 irq_chip 调用 ->irq_ack()。@desc 是非 NULL、存活描述符；hardirq 中
 * 持锁，不睡眠、无返回值。
 *
 * PM 不允许处理时按条件 EOI；action 不可执行时 mask 并条件 EOI。正常路径计数，
 * oneshot 先 mask，再 ack 当前边沿、运行 action，最后按线程状态 EOI/unmask。chip
 * 必须提供 irq_ack/irq_eoi；层级中更下方/上方的转发由该 chip 回调自行完成。
 */
void handle_fasteoi_ack_irq(struct irq_desc *desc)
{
	/* chip 是锁内稳定的当前层控制器借用指针。 */
	struct irq_chip *chip = desc->irq_data.chip;

	guard(raw_spinlock)(&desc->lock);

	if (!irq_can_handle_pm(desc)) {
		cond_eoi_irq(chip, &desc->irq_data);
		return;
	}

	if (unlikely(!irq_can_handle_actions(desc))) {
		mask_irq(desc);
		cond_eoi_irq(chip, &desc->irq_data);
		return;
	}

	kstat_incr_irqs_this_cpu(desc);
	if (desc->istate & IRQS_ONESHOT)
		mask_irq(desc);

	desc->irq_data.chip->irq_ack(&desc->irq_data);

	handle_irq_event(desc);

	cond_unmask_eoi_irq(desc, chip);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_ack_irq);

/**
 * handle_fasteoi_mask_irq - irq handler for level hierarchy stacked on
 *			     transparent controllers
 *
 * @desc:	the interrupt description structure for this irq
 *
 * Like handle_fasteoi_irq(), but for use with hierarchy where the irq_chip
 * also needs to have its ->irq_mask_ack() function called.
 */
/*
 * 该入口类似 handle_fasteoi_irq()，用于“level 子层叠在透明父控制器”场景，
 * 还必须调用 ->irq_mask_ack()。@desc 在 hardirq 中由本函数持锁；无返回值、不睡眠。
 *
 * 一进入就 mask_ack，保证持续电平不会重入。PM/action 不允许时条件 EOI 并保持屏蔽；
 * 正常时计数、运行 action，再依据 disabled/oneshot 线程状态完成 EOI/unmask。
 */
void handle_fasteoi_mask_irq(struct irq_desc *desc)
{
	/* chip 是当前层透明控制器借用指针，用于 EOI 策略。 */
	struct irq_chip *chip = desc->irq_data.chip;

	guard(raw_spinlock)(&desc->lock);
	mask_ack_irq(desc);

	if (!irq_can_handle(desc)) {
		cond_eoi_irq(chip, &desc->irq_data);
		return;
	}

	kstat_incr_irqs_this_cpu(desc);

	handle_irq_event(desc);

	cond_unmask_eoi_irq(desc, chip);
}
EXPORT_SYMBOL_GPL(handle_fasteoi_mask_irq);

#endif /* CONFIG_IRQ_FASTEOI_HIERARCHY_HANDLERS */
/* 关闭该配置时，不构建需要额外 ack/mask_ack 的两种层级 fasteoi flow handler。 */

#ifdef CONFIG_SMP
/*
 * irq_chip_pre_redirect_parent() - 把重定向前通知转发给直接 parent chip
 *
 * @data: 当前层非 NULL irq_data；必须存在 parent_data，且 parent chip 提供
 *        irq_pre_redirect。函数把借用指针上移一层后直接调用回调；无返回值、不睡眠，
 * 通常位于 desc->lock 和 hardirq 上下文，不遍历更高祖先。
 */
void irq_chip_pre_redirect_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_pre_redirect(data);
}
EXPORT_SYMBOL_GPL(irq_chip_pre_redirect_parent);
#endif
/* UP 配置没有跨 CPU irq_work 重定向，因此不构建 parent pre-redirect 转发器。 */

/**
 * irq_chip_set_parent_state - set the state of a parent interrupt.
 *
 * @data: Pointer to interrupt specific data
 * @which: State to be restored (one of IRQCHIP_STATE_*)
 * @val: Value corresponding to @which
 *
 * Conditional success, if the underlying irqchip does not implement it.
 */
/*
 * 在直接 parent IRQ 上设置状态。@data 是当前层非 NULL 借用指针；@which
 * 指定 IRQCHIP_STATE_*，@val 是目标布尔值。若无 parent 或 parent 未实现 state setter，
 * 按“可选功能成功”返回 0；否则原样返回回调的 0/负错误。不改变当前层软件状态，
 * 不遍历祖先；调用上下文和锁由发起该 chip 回调的上层保证。
 */
int irq_chip_set_parent_state(struct irq_data *data,
			      enum irqchip_irq_state which,
			      bool val)
{
	data = data->parent_data;

	if (!data || !data->chip->irq_set_irqchip_state)
		return 0;

	return data->chip->irq_set_irqchip_state(data, which, val);
}
EXPORT_SYMBOL_GPL(irq_chip_set_parent_state);

/**
 * irq_chip_get_parent_state - get the state of a parent interrupt.
 *
 * @data: Pointer to interrupt specific data
 * @which: one of IRQCHIP_STATE_* the caller wants to know
 * @state: a pointer to a boolean where the state is to be stored
 *
 * Conditional success, if the underlying irqchip does not implement it.
 */
/*
 * 查询直接 parent IRQ 的 @which 状态，并把结果写入非 NULL 输出 @state。
 * @data/@state 均由调用者拥有。无 parent 或回调时返回 0，但不会初始化 @state，调用者
 * 必须把“功能缺失”纳入协议；有回调时原样返回结果。不遍历更高层、不取得引用。
 */
int irq_chip_get_parent_state(struct irq_data *data,
			      enum irqchip_irq_state which,
			      bool *state)
{
	data = data->parent_data;

	if (!data || !data->chip->irq_get_irqchip_state)
		return 0;

	return data->chip->irq_get_irqchip_state(data, which, state);
}
EXPORT_SYMBOL_GPL(irq_chip_get_parent_state);

/**
 * irq_chip_shutdown_parent - Shutdown the parent interrupt
 * @data:	Pointer to interrupt specific data
 *
 * Invokes the irq_shutdown() callback of the parent if available or falls
 * back to irq_chip_disable_parent().
 */
/*
 * 关闭直接 parent interrupt；有 irq_shutdown 就调用，否则退化为
 * irq_chip_disable_parent()。@data 是当前层非 NULL 借用对象，parent/chip/至少一个
 * disable 或 mask 回调必须有效。无返回值，只驱动 parent 硬件，不更新 parent desc 的
 * depth/IRQD_STARTED；供组合 irq_chip 在自己的回调内转发，不能替代核心 irq_shutdown()。
 */
void irq_chip_shutdown_parent(struct irq_data *data)
{
	/* parent 是直接上层 irq_data 借用指针，仅在当前 domain 层级生命周期内有效。 */
	struct irq_data *parent = data->parent_data;

	if (parent->chip->irq_shutdown)
		parent->chip->irq_shutdown(parent);
	else
		irq_chip_disable_parent(data);
}
EXPORT_SYMBOL_GPL(irq_chip_shutdown_parent);

/**
 * irq_chip_startup_parent - Startup the parent interrupt
 * @data:	Pointer to interrupt specific data
 *
 * Invokes the irq_startup() callback of the parent if available or falls
 * back to irq_chip_enable_parent().
 */
/*
 * 启动直接 parent；优先 parent->irq_startup 并返回其 unsigned 结果，缺少时
 * 调 irq_chip_enable_parent() 并返回 0。@data/parent 均为借用，回调不可睡眠；函数只
 * 操作 parent chip，不同步 parent desc 的 depth/STARTED 软件状态。
 */
unsigned int irq_chip_startup_parent(struct irq_data *data)
{
	/* parent 必须存在，其 chip 至少提供 startup 或 enable/unmask 组合。 */
	struct irq_data *parent = data->parent_data;

	if (parent->chip->irq_startup)
		return parent->chip->irq_startup(parent);

	irq_chip_enable_parent(data);
	return 0;
}
EXPORT_SYMBOL_GPL(irq_chip_startup_parent);

/**
 * irq_chip_enable_parent - Enable the parent interrupt (defaults to unmask if
 * NULL)
 * @data:	Pointer to interrupt specific data
 */
/*
 * 启用直接 parent IRQ；parent 没有 irq_enable 时默认调用 irq_unmask。
 * @data 是当前层非 NULL 借用指针，层级和回调必须完整。无返回值、不修改通用软件位；
 * 只供子 chip 实现回调时转发，调用上下文继承自原始 IRQ 操作。
 */
void irq_chip_enable_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_enable)
		data->chip->irq_enable(data);
	else
		data->chip->irq_unmask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_enable_parent);

/**
 * irq_chip_disable_parent - Disable the parent interrupt (defaults to mask if
 * NULL)
 * @data:	Pointer to interrupt specific data
 */
/*
 * 禁用直接 parent IRQ；缺少 irq_disable 时默认 irq_mask。@data 为当前层
 * 借用指针，parent 和所需回调必须存在。无返回值、不更新 depth/IRQD 状态。
 */
void irq_chip_disable_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_disable)
		data->chip->irq_disable(data);
	else
		data->chip->irq_mask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_disable_parent);

/**
 * irq_chip_ack_parent - Acknowledge the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * 确认直接 parent interrupt。@data 是当前层非 NULL 借用指针；函数上移
 * 一层并无条件调用 parent->irq_ack，故组合 chip 必须在注册时保证回调存在。
 * 无返回值、不睡眠、不触碰当前层状态。
 */
void irq_chip_ack_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_ack(data);
}
EXPORT_SYMBOL_GPL(irq_chip_ack_parent);

/**
 * irq_chip_mask_parent - Mask the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * 屏蔽直接 parent interrupt。@data/parent 为借用，parent->irq_mask 必须
 * 存在；无返回值，只执行硬件转发，不置当前或 parent desc 的 IRQD_MASKED。
 */
void irq_chip_mask_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_mask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_mask_parent);

/**
 * irq_chip_mask_ack_parent - Mask and acknowledge the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * 以单个 parent->irq_mask_ack 回调同时屏蔽并确认直接 parent。回调必须
 * 存在；函数不提供 mask+ack 分离回退、不更新软件状态、无返回值。
 */
void irq_chip_mask_ack_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_mask_ack(data);
}
EXPORT_SYMBOL_GPL(irq_chip_mask_ack_parent);

/**
 * irq_chip_unmask_parent - Unmask the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * 解除直接 parent interrupt 的屏蔽。parent->irq_unmask 必须存在；函数
 * 直接转发、无返回值，不清任何 desc 软件 masked 位。
 */
void irq_chip_unmask_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_unmask(data);
}
EXPORT_SYMBOL_GPL(irq_chip_unmask_parent);

/**
 * irq_chip_eoi_parent - Invoke EOI on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * 在直接 parent interrupt 上发送 EOI。parent->irq_eoi 必须存在；函数
 * 只转发当前事件完成通知，不遍历祖先、不修改状态、无返回值。
 */
void irq_chip_eoi_parent(struct irq_data *data)
{
	data = data->parent_data;
	data->chip->irq_eoi(data);
}
EXPORT_SYMBOL_GPL(irq_chip_eoi_parent);

/**
 * irq_chip_set_affinity_parent - Set affinity on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @dest:	The affinity mask to set
 * @force:	Flag to enforce setting (disable online checks)
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
/*
 * 在直接 parent 设置 CPU affinity。@data 是当前层借用指针；@dest 是调用期
 * 有效的非 NULL cpumask；@force 为 true 时要求跳过在线性检查。parent 实现回调则
 * 原样返回 IRQ_SET_MASK_* 或负错误，缺失返回 -ENOSYS。函数不更新当前层 effective
 * affinity，是否向更高层转发由 parent chip 决定。
 */
int irq_chip_set_affinity_parent(struct irq_data *data,
				 const struct cpumask *dest, bool force)
{
	data = data->parent_data;
	if (data->chip->irq_set_affinity)
		return data->chip->irq_set_affinity(data, dest, force);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_affinity_parent);

/**
 * irq_chip_set_type_parent - Set IRQ type on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @type:	IRQ_TYPE_{LEVEL,EDGE}_* value - see include/linux/irq.h
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
/*
 * 把 IRQ_TYPE_LEVEL/EDGE_* @type 设置到直接 parent。@data 为当前层借用；
 * 有 irq_set_type 时原样返回 0/负错误，缺失返回 -ENOSYS。不更新当前层 trigger/flow
 * handler，子 chip 在自身 set_type 回调中决定如何同步软件状态。
 */
int irq_chip_set_type_parent(struct irq_data *data, unsigned int type)
{
	data = data->parent_data;

	if (data->chip->irq_set_type)
		return data->chip->irq_set_type(data, type);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_type_parent);

/**
 * irq_chip_retrigger_hierarchy - Retrigger an interrupt in hardware
 * @data:	Pointer to interrupt specific data
 *
 * Iterate through the domain hierarchy of the interrupt and check
 * whether a hw retrigger function exists. If yes, invoke it.
 */
/*
 * 从直接 parent 开始沿 domain 层级向上寻找首个 irq_retrigger 硬件回调，
 * 找到后调用并立即返回结果。@data 和整条 parent 链都是调用期借用；没有实现时返回
 * 0，表示无需/无法硬件重触发而非 -ENOSYS。函数不尝试当前层，也不做软件 resend。
 */
int irq_chip_retrigger_hierarchy(struct irq_data *data)
{
	for (data = data->parent_data; data; data = data->parent_data)
		if (data->chip && data->chip->irq_retrigger)
			return data->chip->irq_retrigger(data);

	return 0;
}
EXPORT_SYMBOL_GPL(irq_chip_retrigger_hierarchy);

/**
 * irq_chip_set_vcpu_affinity_parent - Set vcpu affinity on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @vcpu_info:	The vcpu affinity information
 */
/*
 * 把虚拟 CPU 目标信息 @vcpu_info 转发给直接 parent。该指针格式和所有权由
 * 具体 irqchip 定义，核心不解析、不保留；有回调时原样返回结果，缺失返回 -ENOSYS。
 * @data 必须有 parent，不更新通用 CPU affinity。
 */
int irq_chip_set_vcpu_affinity_parent(struct irq_data *data, void *vcpu_info)
{
	data = data->parent_data;
	if (data->chip->irq_set_vcpu_affinity)
		return data->chip->irq_set_vcpu_affinity(data, vcpu_info);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_vcpu_affinity_parent);
/**
 * irq_chip_set_wake_parent - Set/reset wake-up on the parent interrupt
 * @data:	Pointer to interrupt specific data
 * @on:		Whether to set or reset the wake-up capability of this irq
 *
 * Conditional, as the underlying parent chip might not implement it.
 */
/*
 * 在直接 parent 设置或清除 wake 能力；@on 非零启用、零禁用。parent 声明
 * IRQCHIP_SKIP_SET_WAKE 时直接返回 0；否则有 irq_set_wake 就转发并返回结果，缺失
 * 返回 -ENOSYS。函数不修改当前层 IRQD_WAKEUP_STATE，嵌套 wake_depth 由外层管理。
 */
int irq_chip_set_wake_parent(struct irq_data *data, unsigned int on)
{
	data = data->parent_data;

	if (data->chip->flags & IRQCHIP_SKIP_SET_WAKE)
		return 0;

	if (data->chip->irq_set_wake)
		return data->chip->irq_set_wake(data, on);

	return -ENOSYS;
}
EXPORT_SYMBOL_GPL(irq_chip_set_wake_parent);

/**
 * irq_chip_request_resources_parent - Request resources on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * 请求直接 parent interrupt 的可选硬件资源。@data 为当前层借用；有
 * irq_request_resources 时原样返回 0/负错误，缺失按可选操作成功返回 0。成功取得的
 * parent 资源由同一层生命周期在 release helper 中配对，本函数不记录引用计数。
 */
int irq_chip_request_resources_parent(struct irq_data *data)
{
	data = data->parent_data;

	if (data->chip->irq_request_resources)
		return data->chip->irq_request_resources(data);

	/* no error on missing optional irq_chip::irq_request_resources */
	/* 可选 request_resources 缺失不是错误，组合 chip 可继续注册。 */
	return 0;
}
EXPORT_SYMBOL_GPL(irq_chip_request_resources_parent);

/**
 * irq_chip_release_resources_parent - Release resources on the parent interrupt
 * @data:	Pointer to interrupt specific data
 */
/*
 * 释放先前向直接 parent 请求的资源。若 parent 没有 release 回调则为空操作；
 * 有回调时直接调用。@data/parent 均为借用，无返回值；调用者保证与成功 request 配对，
 * 且 action/hardware 已停止使用相关资源。
 */
void irq_chip_release_resources_parent(struct irq_data *data)
{
	data = data->parent_data;
	if (data->chip->irq_release_resources)
		data->chip->irq_release_resources(data);
}
EXPORT_SYMBOL_GPL(irq_chip_release_resources_parent);
#endif /* CONFIG_IRQ_DOMAIN_HIERARCHY */
/* 非层级 irq_domain 不构建 parent 转发 helper，chip 必须直接实现本层硬件操作。 */

#ifdef CONFIG_SMP
/*
 * irq_chip_redirect_set_affinity() - 为软件 irq_work 重定向记录目标 CPU
 *
 * @data: 输入输出的非 NULL irq_data，其 desc 含 redirect 状态。
 * @dest: 只读非 NULL、非空目标掩码；函数选择 cpumask_first() 作为单一重定向 CPU。
 * @force: 未消费的通用 affinity 强制标志，软件重定向不做在线性复核。
 *
 * WRITE_ONCE 发布 target_cpu，与 hardirq 侧 READ_ONCE 配对；同时调用 effective-affinity
 * helper：启用 CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK 时复制整个 @dest，关闭时为空操作，
 * 查询接口回退到请求 affinity。返回 IRQ_SET_MASK_OK_DONE，表示本 chip 已完成处理；
 * 无硬件寄存器动作、不持有 @dest 指针。调用者负责目标有效及 CPU hotplug 串行。
 */
int irq_chip_redirect_set_affinity(struct irq_data *data, const struct cpumask *dest, bool force)
{
	struct irq_redirect *redir = &irq_data_to_desc(data)->redirect;

	WRITE_ONCE(redir->target_cpu, cpumask_first(dest));
	irq_data_update_effective_affinity(data, dest);

	return IRQ_SET_MASK_OK_DONE;
}
EXPORT_SYMBOL_GPL(irq_chip_redirect_set_affinity);
#endif
/* UP 配置无远端 CPU，不构建基于 irq_work 的 affinity 重定向设置器。 */

/**
 * irq_chip_compose_msi_msg - Compose msi message for a irq chip
 * @data:	Pointer to interrupt specific data
 * @msg:	Pointer to the MSI message
 *
 * For hierarchical domains we find the first chip in the hierarchy
 * which implements the irq_compose_msi_msg callback. For non
 * hierarchical we use the top level chip.
 */
/*
 * 为 IRQ 组合 MSI message。层级 domain 从当前 @data 向 parent 查找首个实现
 * irq_compose_msi_msg 的 chip；非层级配置 irqd_get_parent_data() 返回 NULL，因此只检查
 * 当前顶层。@msg 是非 NULL 输出对象，回调负责完整填写；核心不保存其指针。
 *
 * 找不到回调返回 -ENOSYS 且不保证 @msg 内容；找到后调用 void 回调并返回 0。函数不写
 * 设备 MSI 寄存器（那是 irq_write_msi_msg 等路径），调用上下文由 MSI 配置流程保证。
 */
int irq_chip_compose_msi_msg(struct irq_data *data, struct msi_msg *msg)
{
	/* pos 最终借用首个有 compose 回调的层；循环变量 data 沿 parent 单向上移。 */
	struct irq_data *pos;

	for (pos = NULL; !pos && data; data = irqd_get_parent_data(data)) {
		if (data->chip && data->chip->irq_compose_msi_msg)
			pos = data;
	}

	if (!pos)
		return -ENOSYS;

	pos->chip->irq_compose_msi_msg(pos, msg);
	return 0;
}

/*
 * irq_get_pm_device() - 查询当前 irq_data 所属 domain 的 runtime-PM 设备
 *
 * @data: 只读非 NULL irq_data 借用指针。
 * 有 domain 时返回其可空 pm_dev 借用指针，否则 NULL；不增加 device/PM 引用、不沿
 * parent 层级搜索、不睡眠。调用者必须在 domain 生命周期内立即使用返回值。
 */
static struct device *irq_get_pm_device(struct irq_data *data)
{
	if (data->domain)
		return data->domain->pm_dev;

	return NULL;
}

/**
 * irq_chip_pm_get - Enable power for an IRQ chip
 * @data:	Pointer to interrupt specific data
 *
 * Enable the power to the IRQ chip referenced by the interrupt data
 * structure.
 */
/*
 * 为 @data 引用的 IRQ chip 打开电源。函数从当前 domain 借出 pm_dev；只有
 * CONFIG_PM 启用且设备存在时调用 pm_runtime_resume_and_get()。成功返回 0，失败返回
 * 负 errno；无 PM 设备也返回 0，但不会增加 runtime-PM usage 引用。
 *
 * 每次成功取得的 runtime-PM usage 引用必须由 irq_chip_pm_put() 配对。调用上下文须满足
 * 该 irq-domain PM 设备的 runtime-PM/irq-safe 约束；函数本身不修改 IRQD/depth。
 */
int irq_chip_pm_get(struct irq_data *data)
{
	/* dev 是 domain 生命周期内借用；retval 传递 PM resume/get 的结果。 */
	struct device *dev = irq_get_pm_device(data);
	int retval = 0;

	if (IS_ENABLED(CONFIG_PM) && dev)
		retval = pm_runtime_resume_and_get(dev);

	return retval;
}

/**
 * irq_chip_pm_put - Drop a PM reference on an IRQ chip
 * @data:	Pointer to interrupt specific data
 *
 * Drop a power management reference, acquired via irq_chip_pm_get(), on the IRQ
 * chip represented by the interrupt data structure.
 *
 * Note that this will not disable power to the IRQ chip until this function
 * has been called for all IRQs that have called irq_chip_pm_get() and it may
 * not disable power at all (if user space prevents that, for example).
 */
/*
 * 释放 irq_chip_pm_get() 为 @data 对应 IRQ chip 取得的一份 PM 引用。只有
 * 所有 IRQ 各自取得的引用都配对释放后，设备才可能断电；用户空间策略等也可能让它
 * 始终保持上电。
 *
 * @data 是非 NULL 借用；有 pm_dev 时调用 pm_runtime_put()，否则为空操作。无返回值，
 * 调用者只能在先前 get 成功后配对，避免 usage_count 失衡；实际 suspend 可被延后。
 */
void irq_chip_pm_put(struct irq_data *data)
{
	/* dev 只是用于本次 put 的 domain-owned 借用指针，不由本函数 device_put()。 */
	struct device *dev = irq_get_pm_device(data);

	if (dev)
		pm_runtime_put(dev);
}
