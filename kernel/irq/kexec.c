// SPDX-License-Identifier: GPL-2.0

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqnr.h>

#include "internals.h"

/*
 * kexec/crash-kexec 跳入新内核前，旧内核必须停止所有已启动 IRQ，并尽量清除控制器
 * 中仍处于 active 的状态，避免旧中断在新内核尚未接管时继续投递。该阶段通常已
 * 关闭本地中断并停止其他 CPU，不再依赖普通驱动 teardown 顺序；描述符和 irqchip
 * 只在最终切换窗口内借用，不转移所有权。
 */

/*
 * 在机器切换到 kexec 内核前停用全部已启动中断。
 * 遍历所有 irq_desc，跳过没有 chip 或尚未 startup 的条目。启用
 * GENERIC_IRQ_KEXEC_CLEAR_VM_FORWARD 时，先请求 irqchip 清除 ACTIVE 状态：成功
 * 表示转发给 VM 的活动中断已撤销，无需再发 EOI；不支持或失败时，若 IRQ 正在
 * in-progress 且 chip 提供 irq_eoi()，则用 EOI 结束旧控制器状态。最后无论上述
 * 清理结果如何都调用 irq_shutdown()，阻止线路继续投递。
 *
 * 函数无返回值，单个 chip 状态清理失败不能中止全局关停。调用者必须处于架构
 * kexec/crash shutdown 的最终静止上下文；本函数自身不取得 desc 锁，也不等待
 * handler，依赖其他 CPU 已停止且本地 IRQ 已关闭的外部生命周期保证。
 */
void machine_kexec_mask_interrupts(void)
{
	struct irq_desc *desc;
	unsigned int i;

	/* 全描述符扫描也覆盖没有 action、但已经由层级/级联路径启动的 IRQ。 */
	for_each_irq_desc(i, desc) {
		struct irq_chip *chip;
		int check_eoi = 1;

		chip = irq_desc_get_chip(desc);
		/* 未绑定 chip 或从未启动的描述符没有遗留硬件投递状态。 */
		if (!chip || !irqd_is_started(&desc->irq_data))
			continue;

		if (IS_ENABLED(CONFIG_GENERIC_IRQ_KEXEC_CLEAR_VM_FORWARD)) {
			/*
			 * First try to remove the active state from an interrupt which is forwarded
			 * to a VM. If the interrupt is not forwarded, try to EOI the interrupt.
			 */
			/*
			 * 优先清除可能转发给 VM 的 ACTIVE 状态；返回 0 表示已处理，非 0
			 * 表示未转发或不支持，随后仍需考虑常规 EOI。
			 */
			check_eoi = irq_set_irqchip_state(i, IRQCHIP_STATE_ACTIVE, false);
		}

		/* 只有仍在处理且 ACTIVE 清理未成功时，才用 provider EOI 收尾。 */
		if (check_eoi && chip->irq_eoi && irqd_irq_inprogress(&desc->irq_data))
			chip->irq_eoi(&desc->irq_data);

		/* 最终关闭线路；这一步不因前面的可选状态清理结果而跳过。 */
		irq_shutdown(desc);
	}
}
