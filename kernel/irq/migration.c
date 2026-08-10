// SPDX-License-Identifier: GPL-2.0

#include <linux/irq.h>
#include <linux/interrupt.h>

#include "internals.h"

/*
 * 本文件实现 generic IRQ affinity 的延迟迁移执行与 CPU hotplug 收尾。manage.c 在暂时不能
 * 重编程向量时，把目标复制到 desc->pending_mask 并置 SETAFFINITY_PENDING；安全的 masked
 * 中断点再由本文件调用 irq_do_set_affinity()。CPU 下线还需判断 pending 目标是否仍含在线
 * CPU，并让支持的 irqchip 强制完成底层向量清理。
 *
 * pending_mask 是“尚待应用的请求值”，irq_common_data.affinity 是已接受的请求 affinity，
 * effective_affinity 才是 chip 实际编程结果；学习时不能把三者当作同一个状态。
 */
/**
 * irq_fixup_move_pending - Cleanup irq move pending from a dying CPU
 * @desc:		Interrupt descriptor to clean up
 * @force_clear:	If set clear the move pending bit unconditionally.
 *			If not set, clear it only when the dying CPU is the
 *			last one in the pending mask.
 *
 * Returns true if the pending bit was set and the pending mask contains an
 * online CPU other than the dying CPU.
 */
/*
 * CPU 正在下线时清理 @desc 的待迁移状态。@force_clear 为 true 时，只要原本有
 * pending 就无条件清位；false 时，仅在 dying CPU 已是 pending_mask 最后在线目标（即与
 * 当前 cpu_online_mask 无交集）时清位。若入口无 pending，或清理后没有在线目标，返回
 * false；pending_mask 仍含另一在线 CPU 时返回 true，表示仍可继续迁移。函数只调整
 * SETAFFINITY_PENDING，不清 pending_mask 内容、不调用 chip；调用者持 desc 锁。
 */
bool irq_fixup_move_pending(struct irq_desc *desc, bool force_clear)
{
	/* data 是 desc 内嵌顶层 irq_data 的借用指针。 */
	struct irq_data *data = irq_desc_get_irq_data(desc);

	if (!irqd_is_setaffinity_pending(data))
		return false;

	/*
	 * The outgoing CPU might be the last online target in a pending
	 * interrupt move. If that's the case clear the pending move bit.
	 */
	/*
	 * 下线 CPU 可能是 pending 目标中最后仍在线的一颗；若已无在线交集，这次
	 * 请求当前无法落实，清 pending 标志以免后续错误执行。
	 */
	if (!cpumask_intersects(desc->pending_mask, cpu_online_mask)) {
		irqd_clr_move_pending(data);
		return false;
	}
	if (force_clear)
		irqd_clr_move_pending(data);
	return true;
}

/*
 * irq_force_complete_move() - 请求层级中首个支持的 irqchip 强制完成底层迁移
 *
 * @desc: 正在 CPU hotplug/迁移收尾中的存活描述符。
 * 从顶层 irq_data 沿 parent_data 向硬件根遍历，找到首个 irq_force_complete_move 回调即
 * 调用并返回；无支持层为空操作。无错误返回，回调负责完成诸如旧向量清理；调用者保证
 * 所需锁和不可迁移/中断上下文条件。
 */
void irq_force_complete_move(struct irq_desc *desc)
{
	for (struct irq_data *d = irq_desc_get_irq_data(desc); d; d = irqd_get_parent_data(d)) {
		if (d->chip && d->chip->irq_force_complete_move) {
			d->chip->irq_force_complete_move(d);
			return;
		}
	}
}

/*
 * irq_move_masked_irq() - 在线路已 masked 的安全点应用 pending affinity
 *
 * @idata: 任意属于该 desc 的 irq_data；函数规范到 desc 顶层 data/chip。
 * 无 pending 直接返回；否则先清 pending 标志。per-CPU IRQ 属于调用错误，告警；空目标或
 * 无 set_affinity 能力放弃。本函数要求 desc->lock 已持有，并依赖调用者已 mask 线路，避免
 * edge 事件与路由表重编程并发。pending_mask 与在线 CPU 有交集时调用 irq_do_set_affinity；
 * 仅 -EBUSY 会重新置 pending 并保留 mask 下次重试，其他结果（含错误或无在线目标）均清
 * pending_mask。无返回值，成功会同步请求/effective affinity 并通知 IRQ 线程。
 */
void irq_move_masked_irq(struct irq_data *idata)
{
	struct irq_desc *desc = irq_data_to_desc(idata);
	struct irq_data *data = &desc->irq_data;
	struct irq_chip *chip = data->chip;

	if (likely(!irqd_is_setaffinity_pending(data)))
		return;

	irqd_clr_move_pending(data);

	/*
	 * Paranoia: cpu-local interrupts shouldn't be calling in here anyway.
	 */
	/* CPU-local IRQ 根本不应走通用迁移路径；防御性告警并退出。 */
	if (irqd_is_per_cpu(data)) {
		WARN_ON(1);
		return;
	}

	if (unlikely(cpumask_empty(desc->pending_mask)))
		return;

	if (!chip->irq_set_affinity)
		return;

	assert_raw_spin_locked(&desc->lock);

	/*
	 * If there was a valid mask to work with, please
	 * do the disable, re-program, enable sequence.
	 * This is *not* particularly important for level triggered
	 * but in a edge trigger case, we might be setting rte
	 * when an active trigger is coming in. This could
	 * cause some ioapics to mal-function.
	 * Being paranoid i guess!
	 *
	 * For correct operation this depends on the caller
	 * masking the irqs.
	 */
	/*
	 * 存在有效目标时，应在 disable/mask、重编程、enable/unmask 序列内迁移。
	 * level 源影响较小，但 edge 到达时同时改 RTE 可能让某些 IO-APIC 异常，因此宁可保守。
	 * 正确性依赖调用者已把 IRQ mask；本函数只断言 desc 锁，不自行验证硬件 mask。
	 */
	if (cpumask_intersects(desc->pending_mask, cpu_online_mask)) {
		int ret;

		ret = irq_do_set_affinity(data, desc->pending_mask, false);
		/*
		 * If the there is a cleanup pending in the underlying
		 * vector management, reschedule the move for the next
		 * interrupt. Leave desc->pending_mask intact.
		 */
		/*
		 * 底层向量管理仍有 cleanup pending 而返回 -EBUSY 时，重新设置 move pending，
		 * 保留 pending_mask，等待下一次中断安全点重试。
		 */
		if (ret == -EBUSY) {
			irqd_set_move_pending(data);
			return;
		}
	}
	cpumask_clear(desc->pending_mask);
}

/*
 * __irq_move_irq() - 必要时临时 mask 线路并执行一次 pending IRQ 迁移
 *
 * @idata: 该逻辑 IRQ 层级内任意 irq_data；先规范为 desc 顶层 data。
 * disabled IRQ 不迁移。记录入口 masked 状态，未 mask 时调用 chip->irq_mask，执行
 * irq_move_masked_irq() 后仅在本函数临时 mask 的情况下 unmask，精确恢复入口状态。
 * 调用者持 desc->lock，chip 必须提供相应 mask/unmask；无返回值。对 ONESHOT threaded IRQ
 * 尤其不能误解既有 MASKED 并提前 unmask，否则设备电平未清可能形成中断风暴。
 */
void __irq_move_irq(struct irq_data *idata)
{
	bool masked;

	/*
	 * Get top level irq_data when CONFIG_IRQ_DOMAIN_HIERARCHY is enabled,
	 * and it should be optimized away when CONFIG_IRQ_DOMAIN_HIERARCHY is
	 * disabled. So we avoid an "#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY" here.
	 */
	/*
	 * 统一取 desc 顶层 irq_data；层级配置关闭时编译器会消除多余转换，从而无需
	 * 在热路径显式使用 CONFIG_IRQ_DOMAIN_HIERARCHY 条件编译。
	 */
	idata = irq_desc_get_irq_data(irq_data_to_desc(idata));

	if (unlikely(irqd_irq_disabled(idata)))
		return;

	/*
	 * Be careful vs. already masked interrupts. If this is a
	 * threaded interrupt with ONESHOT set, we can end up with an
	 * interrupt storm.
	 */
	/*
	 * 必须尊重入口已 masked 状态，尤其 ONESHOT threaded IRQ 正靠 mask 等线程
	 * 完成；若迁移后无条件 unmask，设备仍 active 时可能造成中断风暴。
	 */
	masked = irqd_irq_masked(idata);
	if (!masked)
		idata->chip->irq_mask(idata);
	irq_move_masked_irq(idata);
	if (!masked)
		idata->chip->irq_unmask(idata);
}

/*
 * irq_can_move_in_process_context() - 查询该 IRQ 是否可在进程上下文直接迁移
 *
 * @data: 层级内任意、配置稳定的 irq_data 借用指针。
 * 规范为 desc 顶层 data 后返回 irq_can_move_pcntxt() 的能力判定；层级关闭时转换会被优化。
 * 返回 bool，无锁、无副作用，只是即时能力快照，不能代替实际设置路径的锁与忙状态检查。
 */
bool irq_can_move_in_process_context(struct irq_data *data)
{
	/*
	 * Get the top level irq_data in the hierarchy, which is optimized
	 * away when CONFIG_IRQ_DOMAIN_HIERARCHY is disabled.
	 */
	/* 始终以顶层 data 判断；无层级构建中该规范化操作会被编译器消除。 */
	data = irq_desc_get_irq_data(irq_data_to_desc(data));
	return irq_can_move_pcntxt(data);
}
