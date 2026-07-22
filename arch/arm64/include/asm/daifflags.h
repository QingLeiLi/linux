/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2017 ARM Ltd.
 */
#ifndef __ASM_DAIFFLAGS_H
#define __ASM_DAIFFLAGS_H

#include <linux/irqflags.h>

#include <asm/arch_gicv3.h>
#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/ptrace.h>

/*
 * PSTATE.DAIF 是当前 CPU 的异常屏蔽位：D=调试异常、A=SError、I=IRQ、
 * F=FIQ；某位为 1 表示对应异常被屏蔽。下面四个值不是任意位图，而是
 * arm64 入口代码约定的三类执行上下文和完整掩码。
 */
/* 普通进程上下文：四类异常均可接收。 */
#define DAIF_PROCCTX		0
/* 进程上下文但关闭普通中断：IRQ/FIQ 被屏蔽。 */
#define DAIF_PROCCTX_NOIRQ	(PSR_I_BIT | PSR_F_BIT)
/* 错误处理上下文：额外屏蔽 SError，调试异常仍由具体入口决定。 */
#define DAIF_ERRCTX		(PSR_A_BIT | PSR_I_BIT | PSR_F_BIT)
/* 用于从完整 PSTATE 中只提取 D/A/I/F 四位。 */
#define DAIF_MASK		(PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT)


/* mask/save/unmask/restore all exceptions, including interrupts. */
static __always_inline void local_daif_mask(void)
{
	/*
	 * 调试模式检查一个不一致状态：PMR 已经表示“关 IRQ”，而 DAIF.I/F
	 * 尚未同时置位。local_daif_mask() 要建立的是最强屏蔽状态，发现
	 * 调用前状态违背优先级屏蔽协议时告警，但仍继续修复状态。
	 */
	WARN_ON(system_has_prio_mask_debugging() &&
		(read_sysreg_s(SYS_ICC_PMR_EL1) == (GIC_PRIO_IRQOFF |
						    GIC_PRIO_PSR_I_SET)));

	asm volatile(
		"msr	daifset, #0xf		// local_daif_mask\n"
		:
		:
		: "memory");
	/*
	 * daifset #0xf 原子置 D/A/I/F，并用 memory clobber 阻止编译器把
	 * 临界区内存访问移到屏蔽操作之前。这里操作的是本 CPU 寄存器，
	 * 不替代保护共享数据所需的自旋锁。
	 */

	/* Don't really care for a dsb here, we don't intend to enable IRQs */
	if (system_uses_irq_prio_masking())
		gic_write_pmr(GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET);
	/*
	 * 使用 GIC PMR 屏蔽时，DAIF.I 已经为 1，所以把 PMR 留在 IRQON
	 * 阈值并附加软件状态标志；恢复路径据此避免把两套屏蔽状态混淆。
	 */

	/* 更新 lockdep/irq tracing 的软件视图；它不是实际关中断的指令。 */
	trace_hardirqs_off();
}

/*
 * 读取并规范化“可交给 local_daif_restore() 的状态快照”。
 *
 * 仅读 DAIF 不够：启用 GIC 优先级屏蔽后，Linux 可能保持 DAIF.I=0、
 * 却通过 ICC_PMR_EL1 拒绝普通 IRQ。因此当 PMR 不是 IRQON 时，人为在
 * 返回值中置 I/F，让调用者看到统一的“中断已关闭”语义。返回值是
 * 当前 CPU 的瞬时状态，只能在同一保存/恢复临界区内使用。
 */
static __always_inline unsigned long local_daif_save_flags(void)
{
	/* flags 保存规范化后的 DAIF 快照，最终作为 restore 的输入。 */
	unsigned long flags;

	flags = read_sysreg(daif);

	if (system_uses_irq_prio_masking()) {
		/* If IRQs are masked with PMR, reflect it in the flags */
		if (read_sysreg_s(SYS_ICC_PMR_EL1) != GIC_PRIO_IRQON)
			flags |= PSR_I_BIT | PSR_F_BIT;
	}

	return flags;
}

static __always_inline unsigned long local_daif_save(void)
{
	/* 必须先保存旧值再屏蔽，否则恢复时只能得到全屏蔽状态。 */
	unsigned long flags;

	flags = local_daif_save_flags();

	local_daif_mask();

	return flags;
}

/*
 * 恢复由 local_daif_save[_flags]() 得到的逻辑异常状态。
 *
 * 常见过程：flags = local_daif_save() 关闭本 CPU 异常；代码在临界区
 * 更新仅由本 CPU 中断访问的数据；最后 local_daif_restore(flags)。
 * 若进入前 IRQ 开启，恢复 DAIF/PMR 并通知 tracing；若进入前关闭，
 * 继续维持关闭。该接口可嵌套，因为每层恢复自己的快照，但不能把
 * flags 跨 CPU、跨任务长期保存后再恢复。
 */
static __always_inline void local_daif_restore(unsigned long flags)
{
	/* Linux 以 I 位作为逻辑 IRQ 状态；save_flags 已把 PMR 状态折叠进来。 */
	bool irq_disabled = flags & PSR_I_BIT;

	WARN_ON(system_has_prio_mask_debugging() &&
		(read_sysreg(daif) & (PSR_I_BIT | PSR_F_BIT)) != (PSR_I_BIT | PSR_F_BIT));

	if (!irq_disabled) {
		/* 先更新软件追踪状态，使随后真正开放的 IRQ 被视为 IRQ-on。 */
		trace_hardirqs_on();

		if (system_uses_irq_prio_masking()) {
			/* 放宽 PMR 阈值，并确保效果在后续指令/异常观察前生效。 */
			gic_write_pmr(GIC_PRIO_IRQON);
			pmr_sync();
		}
	} else if (system_uses_irq_prio_masking()) {
		/* pmr 是本次要写入 ICC_PMR_EL1 的物理优先级阈值。 */
		u64 pmr;

		if (!(flags & PSR_A_BIT)) {
			/*
			 * If interrupts are disabled but we can take
			 * asynchronous errors, we can take NMIs
			 */
			flags &= ~(PSR_I_BIT | PSR_F_BIT);
			pmr = GIC_PRIO_IRQOFF;
			/*
			 * 这里看似把 DAIF.I/F 清零，实际普通 IRQ 仍被较严格 PMR
			 * 拦截；更高优先级的 NMI 类中断却可以穿透。这样同时满足
			 * “普通 IRQ 关闭”和“SError/NMI 仍可达”两种要求。
			 */
		} else {
			/* A/I/F 都屏蔽时不需要 NMI 窗口，DAIF 保持强屏蔽。 */
			pmr = GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET;
		}

		/*
		 * There has been concern that the write to daif
		 * might be reordered before this write to PMR.
		 * From the ARM ARM DDI 0487D.a, section D1.7.1
		 * "Accessing PSTATE fields":
		 *   Writes to the PSTATE fields have side-effects on
		 *   various aspects of the PE operation. All of these
		 *   side-effects are guaranteed:
		 *     - Not to be visible to earlier instructions in
		 *       the execution stream.
		 *     - To be visible to later instructions in the
		 *       execution stream
		 *
		 * Also, writes to PMR are self-synchronizing, so no
		 * interrupts with a lower priority than PMR is signaled
		 * to the PE after the write.
		 *
		 * So we don't need additional synchronization here.
		 */
		gic_write_pmr(pmr);
	}

	/* 最后提交规范化后的硬件 DAIF 状态。 */
	write_sysreg(flags, daif);

	if (irq_disabled)
		trace_hardirqs_off();
}

/*
 * Called by synchronous exception handlers to restore the DAIF bits that were
 * modified by taking an exception.
 */
static __always_inline void local_daif_inherit(struct pt_regs *regs)
{
	/*
	 * regs 是同步异常入口保存的现场；pstate 给出异常发生前的 DAIF，
	 * pmr 给出当时的 GIC 优先级阈值。这里只继承四个屏蔽位，不能把
	 * pt_regs 中其他 PSTATE 控制位意外写回当前 EL1。
	 */
	unsigned long flags = regs->pstate & DAIF_MASK;

	if (!regs_irqs_disabled(regs))
		trace_hardirqs_on();

	if (system_uses_irq_prio_masking())
		/* PMR 与 DAIF 是一个逻辑状态的两半，必须从同一份现场恢复。 */
		gic_write_pmr(regs->pmr);

	/*
	 * We can't use local_daif_restore(regs->pstate) here as
	 * system_has_prio_mask_debugging() won't restore the I bit if it can
	 * use the pmr instead.
	 */
	write_sysreg(flags, daif);
}
#endif
