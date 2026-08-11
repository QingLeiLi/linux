// SPDX-License-Identifier: GPL-2.0
/*
 * Generic cpu hotunplug interrupt migration code copied from the
 * arch/arm implementation
 *
 * Copyright (C) Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/interrupt.h>
#include <linux/ratelimit.h>
#include <linux/irq.h>
#include <linux/sched/isolation.h>

#include "internals.h"

/*
 * CPU 热下线时，已经从 cpu_online_mask 移除的当前 CPU 不能继续作为普通 IRQ 的
 * 唯一有效目标。本文件在 irq_desc 锁内完成 pending move 收尾、选择仍在线目标、
 * 必要时临时屏蔽 irqchip，并区分普通 IRQ 的强制兜底迁移与 managed IRQ 的关停。
 * CPU 再上线时，只恢复原 affinity 包含该 CPU 的 managed IRQ，并结合 housekeeping
 * 隔离策略决定是否重新编程单目标中断。
 *
 * 这里不修改用户配置的 affinity 掩码：普通 IRQ 无可用目标时只把有效投递临时
 * 打破到 cpu_online_mask；managed IRQ 则保持 affinity 原样并进入 shutdown，等待
 * 合法 CPU 上线后恢复。上线恢复显式用 sparse IRQ 锁稳定描述符拓扑；两条路径都
 * 用 desc->lock 保护单个描述符的运行状态。
 */

/* For !GENERIC_IRQ_EFFECTIVE_AFF_MASK this looks at general affinity mask */
/* 未启用 EFFECTIVE_AFF_MASK 时，访问器退化为检查普通 affinity 掩码。 */
/*
 * 判断当前正在下线的 CPU 是否仍要求对此 IRQ 做迁移修正。
 * @d 属于调用者持锁的 irq_desc，函数只借用 affinity 掩码；当前 CPU 已先从
 * cpu_online_mask 删除。有效 affinity 非空时以硬件实际目标为准；配置虽启用但
 * irqchip 未维护有效掩码时退回用户 affinity。若掩码还有“非当前 CPU”却与在线
 * 集合完全不相交，说明此前漏做迁移，告警并强制返回 true。正常返回当前 CPU
 * 是否仍在选定掩码中；函数不改变状态。
 */
static inline bool irq_needs_fixup(struct irq_data *d)
{
	const struct cpumask *m = irq_data_get_effective_affinity_mask(d);
	unsigned int cpu = smp_processor_id();

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	/*
	 * The cpumask_empty() check is a workaround for interrupt chips,
	 * which do not implement effective affinity, but the architecture has
	 * enabled the config switch. Use the general affinity mask instead.
	 */
	/*
	 * 某些 irqchip 在架构启用该配置后仍不实现 effective affinity；空掩码是其
	 * 兼容信号，此时改查普通 affinity，避免把仍指向下线 CPU 的 IRQ 错判为安全。
	 */
	if (cpumask_empty(m))
		m = irq_data_get_affinity_mask(d);

	/*
	 * Sanity check. If the mask is not empty when excluding the outgoing
	 * CPU then it must contain at least one online CPU. The outgoing CPU
	 * has been removed from the online mask already.
	 */
	/*
	 * 健全性检查：排除当前 CPU 后掩码仍非空，却没有任何在线交集，说明剩余目标
	 * 全部已离线；当前 CPU 此时也已不在 online mask。
	 */
	if (cpumask_any_but(m, cpu) < nr_cpu_ids &&
	    !cpumask_intersects(m, cpu_online_mask)) {
		/*
		 * If this happens then there was a missed IRQ fixup at some
		 * point. Warn about it and enforce fixup.
		 */
		/* 这是此前某次下线遗漏的修正；记录异常并强制进入本轮迁移。 */
		pr_warn("Eff. affinity %*pbl of IRQ %u contains only offline CPUs after offlining CPU %u\n",
			cpumask_pr_args(m), d->irq, cpu);
		return true;
	}
#endif
	return cpumask_test_cpu(cpu, m);
}

/*
 * 在当前 CPU 下线过程中迁移一个 IRQ。
 * @desc 的 raw lock 必须由调用者持有；本函数借用 irq_data/irq_chip，不取得引用。
 * 已拆除 chip、无 set_affinity、per-CPU、未启动或不再涉及当前 CPU 的 IRQ无需迁移，
 * 但仍会按需要终止以死亡 CPU 为唯一目标的 pending move。
 *
 * 对需迁移项，先完成旧的延迟 move cleanup，再优先复用仍含在线 CPU 的 pending
 * affinity，防止丢失最后一次用户设置。不能在进程上下文安全移动且当前未屏蔽时，
 * 临时 mask 芯片。目标无在线 CPU 时，managed IRQ 保留配置并 shutdown/deactivate；
 * 普通 IRQ 暂时打破 affinity 到 cpu_online_mask。首次 set_affinity 因向量耗尽返回
 * -ENOSPC 时，普通 IRQ 还可用全部在线 CPU 重试。
 *
 * 返回 true 仅表示“普通 IRQ 的 affinity 被打破且兜底迁移成功”，外层据此安排
 * affinity 通知；所有失败都告警并返回 false。非 shutdown 路径会精确恢复本函数
 * 临时施加的 mask，managed shutdown 则由关闭流程接管硬件状态。
 */
static bool migrate_one_irq(struct irq_desc *desc)
{
	struct irq_data *d = irq_desc_get_irq_data(desc);
	struct irq_chip *chip = irq_data_get_irq_chip(d);
	bool maskchip = !irq_can_move_pcntxt(d) && !irqd_irq_masked(d);
	const struct cpumask *affinity;
	bool brokeaff = false;
	int err;

	/*
	 * IRQ chip might be already torn down, but the irq descriptor is
	 * still in the radix tree. Also if the chip has no affinity setter,
	 * nothing can be done here.
	 */
	/* 描述符可能仍在稀疏树中而 chip 已拆除；无 setter 时也没有可执行动作。 */
	if (!chip || !chip->irq_set_affinity) {
		pr_debug("IRQ %u: Unable to migrate away\n", d->irq);
		return false;
	}

	/*
	 * Complete an eventually pending irq move cleanup. If this
	 * interrupt was moved in hard irq context, then the vectors need
	 * to be cleaned up. It can't wait until this interrupt actually
	 * happens and this CPU was involved.
	 */
	/* 当前 CPU 即将消失，不能再等待未来中断到来完成旧向量清理。 */
	irq_force_complete_move(desc);

	/*
	 * No move required, if:
	 * - Interrupt is per cpu
	 * - Interrupt is not started
	 * - Affinity mask does not include this CPU.
	 *
	 * Note: Do not check desc->action as this might be a chained
	 * interrupt.
	 */
	/*
	 * per-CPU、未启动或有效目标不含当前 CPU 时无需改硬件；不能用 action 判断，
	 * 因为 chained interrupt 也必须参与热插拔迁移。
	 */
	if (irqd_is_per_cpu(d) || !irqd_is_started(d) || !irq_needs_fixup(d)) {
		/*
		 * If an irq move is pending, abort it if the dying CPU is
		 * the sole target.
		 */
		/* 若 pending 请求只剩死亡 CPU，则在退出前终止该请求。 */
		irq_fixup_move_pending(desc, false);
		return false;
	}

	/*
	 * If there is a setaffinity pending, then try to reuse the pending
	 * mask, so the last change of the affinity does not get lost. If
	 * there is no move pending or the pending mask does not contain
	 * any online CPU, use the current affinity mask.
	 */
	/* 优先领取仍可执行的 pending affinity；否则从当前用户 affinity 重新选择。 */
	if (irq_fixup_move_pending(desc, true))
		affinity = irq_desc_get_pending_mask(desc);
	else
		affinity = irq_data_get_affinity_mask(d);

	/* Mask the chip for interrupts which cannot move in process context */
	/* 不能在进程上下文裸移的 chip，需要在重编程窗口临时屏蔽。 */
	if (maskchip && chip->irq_mask)
		chip->irq_mask(d);

	if (!cpumask_intersects(affinity, cpu_online_mask)) {
		/*
		 * If the interrupt is managed, then shut it down and leave
		 * the affinity untouched.
		 */
		/* managed IRQ 不得擅自扩大目标集合；保留配置并等待合法 CPU 回来。 */
		if (irqd_affinity_is_managed(d)) {
			irqd_set_managed_shutdown(d);
			irq_shutdown_and_deactivate(desc);
			return false;
		}
		affinity = cpu_online_mask;
		brokeaff = true;
	}
	/*
	 * Do not set the force argument of irq_do_set_affinity() as this
	 * disables the masking of offline CPUs from the supplied affinity
	 * mask and therefore might keep/reassign the irq to the outgoing
	 * CPU.
	 */
	/* force=false 让核心过滤离线 CPU，避免又把 IRQ 指回正在退出的当前 CPU。 */
	err = irq_do_set_affinity(d, affinity, false);

	/*
	 * If there are online CPUs in the affinity mask, but they have no
	 * vectors left to make the migration work, try to break the
	 * affinity by migrating to any online CPU.
	 */
	/* 原目标仍在线但无可用向量时，只有非 managed IRQ 可以扩大到全部在线 CPU。 */
	if (err == -ENOSPC && !irqd_affinity_is_managed(d) && affinity != cpu_online_mask) {
		pr_debug("IRQ%u: set affinity failed for %*pbl, re-try with online CPUs\n",
			 d->irq, cpumask_pr_args(affinity));

		affinity = cpu_online_mask;
		brokeaff = true;

		err = irq_do_set_affinity(d, affinity, false);
	}

	if (err) {
		pr_warn_ratelimited("IRQ%u: set affinity failed(%d).\n",
				    d->irq, err);
		brokeaff = false;
	}

	/* 只撤销本函数为安全重编程临时施加的 mask。 */
	if (maskchip && chip->irq_unmask)
		chip->irq_unmask(d);

	return brokeaff;
}

/**
 * irq_migrate_all_off_this_cpu - Migrate irqs away from offline cpu
 *
 * The current CPU has been marked offline.  Migrate IRQs off this CPU.
 * If the affinity settings do not allow other CPUs, force them onto any
 * available CPU.
 *
 * Note: we must iterate over all IRQs, whether they have an attached
 * action structure or not, as we need to get chained interrupts too.
 */
/*
 * 把所有需要修正的 IRQ 从已经标记离线的当前 CPU 迁走。
 * 即使用户 affinity 没有其他在线目标，普通 IRQ 也会被强制投递到任一在线 CPU；
 * managed IRQ 则由单项 helper 保持配置并关停。遍历不能按 desc->action 筛选，
 * 因为 chained interrupt 可能没有 action，却同样依赖当前 CPU 的硬件目标。
 *
 * 对每个 active IRQ 取得 desc raw lock 后调用 migrate_one_irq()。只有成功打破普通
 * affinity 时才在锁内安排异步通知，并在锁外输出限速调试信息。函数由 CPU
 * hot-unplug 时序调用，当前 CPU 已不在 online mask；无返回值，单项失败只记录。
 */
void irq_migrate_all_off_this_cpu(void)
{
	struct irq_desc *desc;
	unsigned int irq;

	/* active 集合同时覆盖普通 action IRQ 与 chained IRQ。 */
	for_each_active_irq(irq) {
		bool affinity_broken;

		desc = irq_to_desc(irq);
		/* 单项状态判断、硬件迁移与通知条件都在同一 desc 锁快照内完成。 */
		scoped_guard(raw_spinlock, &desc->lock) {
			affinity_broken = migrate_one_irq(desc);
			if (affinity_broken && desc->affinity_notify)
				irq_affinity_schedule_notify_work(desc);
		}
		/* 日志放在 desc 锁外，缩短 raw 临界区。 */
		if (affinity_broken) {
			pr_debug_ratelimited("IRQ %u: no longer affine to CPU%u\n",
					    irq, smp_processor_id());
		}
	}
}

/*
 * 判断 managed IRQ 是否应因新上线的 @cpu 是 housekeeping CPU 而重新隔离。
 * 未启用 HK_TYPE_MANAGED_IRQ 时直接 false；若当前有效 affinity 已完全位于
 * housekeeping 集合也无需移动。否则，仅当新 CPU 自身属于 housekeeping 集合时
 * 返回 true，促使调用者把可能仍落在隔离 CPU 上的单目标 IRQ 重新编程。
 * 函数只读取全局 housekeeping 配置和 irq_data 的有效掩码，不改变状态。
 */
static bool hk_should_isolate(struct irq_data *data, unsigned int cpu)
{
	const struct cpumask *hk_mask;

	if (!housekeeping_enabled(HK_TYPE_MANAGED_IRQ))
		return false;

	hk_mask = housekeeping_cpumask(HK_TYPE_MANAGED_IRQ);
	if (cpumask_subset(irq_data_get_effective_affinity_mask(data), hk_mask))
		return false;

	return cpumask_test_cpu(cpu, hk_mask);
}

/*
 * 在 CPU 上线时尝试恢复一个 managed IRQ 的可运行状态和硬件 affinity。
 * 调用者持有 desc->lock 且本地中断关闭。只有 managed、已安装 action、仍有 irq_chip，
 * 且原始 affinity 包含 @cpu 的 IRQ 才参与；其余对象保持不变。
 *
 * 若下线阶段因无合法目标进入 managed shutdown，先调用 irq_startup_managed()
 * 重新激活。多目标 IRQ 随后总按原 affinity 重新编程；单目标 IRQ 通常已有合法
 * 归属，只有 housekeeping 隔离要求借新 CPU 纠正目标时才重新设置。函数不报告
 * set_affinity 结果，错误处理遵循 irq_set_affinity_locked() 的内部契约。
 */
static void irq_restore_affinity_of_irq(struct irq_desc *desc, unsigned int cpu)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);
	const struct cpumask *affinity = irq_data_get_affinity_mask(data);

	if (!irqd_affinity_is_managed(data) || !desc->action ||
	    !irq_data_get_irq_chip(data) || !cpumask_test_cpu(cpu, affinity))
		return;

	/* 对称恢复下线阶段保留 affinity 后设置的 managed shutdown。 */
	if (irqd_is_managed_and_shutdown(data))
		irq_startup_managed(desc);

	/*
	 * If the interrupt can only be directed to a single target
	 * CPU then it is already assigned to a CPU in the affinity
	 * mask. No point in trying to move it around unless the
	 * isolation mechanism requests to move it to an upcoming
	 * housekeeping CPU.
	 */
	/*
	 * 单目标 IRQ 已落在原掩码中的某个 CPU，无需仅因新 CPU 上线而扰动；若隔离策略
	 * 要求迁入 housekeeping CPU，则例外地重新计算。多目标 IRQ 总允许核心恢复。
	 */
	if (!irqd_is_single_target(data) || hk_should_isolate(data, cpu))
		irq_set_affinity_locked(data, affinity, false);
}

/**
 * irq_affinity_online_cpu - Restore affinity for managed interrupts
 * @cpu:	Upcoming CPU for which interrupts should be restored
 */
/*
 * 为即将上线的 @cpu 扫描并恢复所有符合条件的 managed IRQ。
 * irq_lock_sparse() 稳定 active IRQ 集合和 irq_to_desc() 结果；每个描述符再使用
 * raw_spinlock_irq guard 串行化状态并关闭本地中断，满足恢复 helper 与 irqchip
 * 回调的锁前提。逐项处理完后释放 sparse 锁，当前实现始终返回 0；单项 helper
 * 无错误返回，不会因一个 IRQ 阻止 CPU 上线。
 */
int irq_affinity_online_cpu(unsigned int cpu)
{
	struct irq_desc *desc;
	unsigned int irq;

	/* 外层锁保证遍历期间 active 描述符不会从稀疏表消失。 */
	irq_lock_sparse();
	for_each_active_irq(irq) {
		desc = irq_to_desc(irq);
		/* 内层锁保护 managed 状态、action、chip 和 affinity 的一致快照。 */
		scoped_guard(raw_spinlock_irq, &desc->lock)
			irq_restore_affinity_of_irq(desc, cpu);
	}
	irq_unlock_sparse();

	return 0;
}
