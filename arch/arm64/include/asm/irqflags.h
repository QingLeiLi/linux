/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_IRQFLAGS_H
#define __ASM_IRQFLAGS_H

#include <asm/barrier.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>

/*
 * Aarch64 has flags for masking: Debug, Asynchronous (serror), Interrupts and
 * FIQ exceptions, in the 'daif' register. We mask and unmask them in 'daif'
 * order:
 * Masking debug exceptions causes all other exceptions to be masked too/
 * Masking SError masks IRQ/FIQ, but not debug exceptions. IRQ and FIQ are
 * always masked and unmasked together, and have no side effects for other
 * flags. Keeping to this order makes it easier for entry.S to know which
 * exceptions should be unmasked.
 */

/*
 * __daif_local_irq_enable - 通过 DAIF 寄存器开启中断（传统路径）
 *
 * DAIF 寄存器中 I(bit1) 和 F(bit0) 分别控制 IRQ 和 FIQ 的屏蔽，
 * 置 1 表示屏蔽，清 0 表示允许。daifclr 是只写寄存器，写入哪些位
 * 就清零哪些位，不影响其余位（D、A 保持不变）。
 */
static __always_inline void __daif_local_irq_enable(void)
{
	/* 编译器屏障：禁止编译器将临界区内的内存访问重排到开中断指令之后，
	 * 确保临界区的所有操作对后续中断处理程序可见。 */
	barrier();
	/* daifclr, #3 = 0b0011：同时清零 I(bit1) 和 F(bit0)，
	 * 一次指令开启 IRQ 和 FIQ，MSR 对当前 CPU 立即生效。 */
	asm volatile("msr daifclr, #3");
	/* 编译器屏障：禁止编译器将开中断之后的代码提前到屏蔽窗口内执行。 */
	barrier();
}

/*
 * __pmr_local_irq_enable - 通过 GIC PMR 寄存器开启中断（伪 NMI 路径）
 *
 * CONFIG_ARM64_PSEUDO_NMI 启用时，系统用高优先级 FIQ 模拟 NMI，
 * 不能用 daifclr 直接开 FIQ（否则伪 NMI 的特权就消失了）。
 * 改用 GICv3 的优先级屏蔽寄存器 ICC_PMR_EL1 控制普通中断的开关：
 *   GIC_PRIO_IRQON  = 优先级阈值低  → 普通中断可送达 CPU（开中断状态）
 *   GIC_PRIO_IRQOFF = 优先级阈值高  → 普通中断被 GIC 拦截（关中断状态）
 * 伪 NMI 使用的 FIQ 优先级始终高于 IRQOFF 阈值，因此无论 PMR 如何设置
 * 都能穿透到达 CPU，实现"不可屏蔽"语义。
 */
static __always_inline void __pmr_local_irq_enable(void)
{
	/* 调试模式下读出当前 PMR 值，断言它只能是 IRQON 或 IRQOFF 两种合法状态。
	 * PMR 出现中间值意味着某处代码破坏了状态机，应当尽早暴露而非静默继续。
	 * IS_ENABLED() 在编译期求值，不启用该 config 时此块被完全消除。 */
	if (IS_ENABLED(CONFIG_ARM64_DEBUG_PRIORITY_MASKING)) {
		u32 pmr = read_sysreg_s(SYS_ICC_PMR_EL1);
		WARN_ON_ONCE(pmr != GIC_PRIO_IRQON && pmr != GIC_PRIO_IRQOFF);
	}

	/* 编译器屏障：确保临界区内的所有内存访问不被重排到开中断之后。 */
	barrier();
	/* 将 PMR 阈值降低到 IRQON，GIC 随即开始向 CPU 投递普通中断。 */
	write_sysreg_s(GIC_PRIO_IRQON, SYS_ICC_PMR_EL1);
	/* pmr_sync()：向 GIC 发出同步指令，确保上面的 PMR 写入已被 GIC 硬件感知。
	 * 开中断时必须同步：若不等 GIC 确认，CPU 可能在 PMR 尚未生效时就继续
	 * 执行，导致本应触发的中断被漏掉（GIC 仍按旧阈值过滤）。
	 * 关中断路径（__pmr_local_irq_disable）无需此调用，因为收紧阈值即使
	 * 有短暂窗口也不会导致额外中断提前到达。 */
	pmr_sync();
	/* 编译器屏障：禁止编译器将开中断之后的代码提前到屏蔽窗口内。 */
	barrier();
}

/*
 * arch_local_irq_enable - 开启当前 CPU 的中断（架构统一入口）
 *
 * 运行时根据 system_uses_irq_prio_masking() 选择路径：
 *   true  → PMR 路径（伪 NMI 系统，保留高优先级 FIQ 穿透能力）
 *   false → DAIF 路径（普通系统，daifclr #3 开启 IRQ+FIQ）
 *
 * system_uses_irq_prio_masking() 使用静态分支（alternative patching）
 * 实现，启动后 patch 一次，之后每次调用只执行一条指令，热路径零开销。
 */
static __always_inline void arch_local_irq_enable(void)
{
	if (system_uses_irq_prio_masking()) {
		__pmr_local_irq_enable();
	} else {
		__daif_local_irq_enable();
	}
}

/*
 * __daif_local_irq_disable - 通过 DAIF 寄存器屏蔽中断（传统路径）
 *
 * DAIF 是 AArch64 的异常屏蔽控制寄存器，每个 bit 控制一类异常：
 *   D(bit3) - Debug exception
 *   A(bit2) - SError（异步外部中止）
 *   I(bit1) - IRQ
 *   F(bit0) - FIQ
 * daifset 是只写寄存器，写入哪些位就将那些位置 1（屏蔽对应异常），
 * 不影响 D、A 位，符合文件头注释中描述的"按 DAIF 顺序操作"的原则。
 */
static __always_inline void __daif_local_irq_disable(void)
{
	/* 编译器屏障：禁止编译器将关中断之前的内存访问重排到屏蔽窗口之后，
	 * 确保进入临界区前所有"窗口外"的写操作已提交到内存模型中。 */
	/*
	指令重排
		编译器会做的重排举例：

		// 你写的代码
		*device_reg = 1;      // 触发设备操作
		local_irq_disable();  // 关中断
		int x = shared_var;   // 读共享变量

		编译器优化后可能变成：

		int x = shared_var;   // 提前读，因为"反正结果一样"
		*device_reg = 1;
		local_irq_disable();

		这就出问题了——在关中断之前读了共享变量，中断可能在这之间修改它。

		barrier 的作用不是"关注顺序"，而是切断优化的跨越能力：

		*device_reg = 1;
		barrier();            // 编译器不能把任何内存访问跨越这条线
		local_irq_disable();
		barrier();
		int x = shared_var;  // 必须在关中断之后才读

		编译器重排的三种来源：

		┌────────────────────┬──────────────────────────────────────────┐
		│      优化行为      │                   例子                   │
		├────────────────────┼──────────────────────────────────────────┤
		│ 读提升（hoisting） │ 把循环外能确定的读移到循环前             │
		├────────────────────┼──────────────────────────────────────────┤
		│ 写延迟（sinking）  │ 把写操作推迟到最后统一提交               │
		├────────────────────┼──────────────────────────────────────────┤
		│ 死代码消除         │ 认为某次写的值会被覆盖，直接删掉前一次写 │
		└────────────────────┴──────────────────────────────────────────┘

		所以 barrier 本质上是告诉编译器：

		▎ "你对内存状态的一切假设，到这里全部作废，重新来过。"

		编译器不是被迫"关注顺序"，而是被迫放弃它关于内存状态的所有缓存推断，从而不敢做跨越屏障的重排。
	*/
	barrier();
	/* daifset, #3 = 0b0011：同时将 I(bit1) 和 F(bit0) 置 1，
	 * 一次指令屏蔽 IRQ 和 FIQ。MSR 写 DAIF 对当前 CPU 立即生效，
	 * 无需额外的 ISB，因此不用硬件内存屏障。 */
	asm volatile("msr daifset, #3");
	/* 编译器屏障：禁止编译器将临界区内的操作提前到关中断指令之前执行。 */
	barrier();
}

/*
 * __pmr_local_irq_disable - 通过 GIC PMR 寄存器屏蔽中断（伪 NMI 路径）
 *
 * 启用 CONFIG_ARM64_PSEUDO_NMI 时，内核用高优先级 FIQ 模拟 NMI，
 * 用于 perf/watchdog 等需要抢占关中断临界区的场景。此时不能用
 * daifset 一刀切屏蔽 FIQ，改用 GIC 优先级屏蔽寄存器 ICC_PMR_EL1：
 *   GIC_PRIO_IRQOFF = 优先级阈值高，GIC 拦截普通 IRQ；
 *                     但伪 NMI 所用 FIQ 的优先级更高，仍可穿透阈值。
 *
 * 与 __pmr_local_irq_enable() 不同，此路径不需要 pmr_sync()：
 * 关中断是收紧阈值，即使 GIC 有短暂窗口尚未感知新阈值，也不会导致
 * 额外中断提前到达；开中断才需要等 GIC 确认，防止中断被漏送。
 */
static __always_inline void __pmr_local_irq_disable(void)
{
	/* 调试模式下读出当前 PMR 值，断言只能是 IRQON 或 IRQOFF 两种合法状态。
	 * 若出现中间值说明状态机紊乱，WARN_ON_ONCE 使问题尽早暴露。
	 * IS_ENABLED() 编译期求值，不启用该 config 时此块完全消除，无运行时开销。 */
	if (IS_ENABLED(CONFIG_ARM64_DEBUG_PRIORITY_MASKING)) {
		u32 pmr = read_sysreg_s(SYS_ICC_PMR_EL1);
		WARN_ON_ONCE(pmr != GIC_PRIO_IRQON && pmr != GIC_PRIO_IRQOFF);
	}

	/* 编译器屏障：禁止关中断之前的内存访问被重排到屏蔽窗口之后。 */
	barrier();
	/* 将 ICC_PMR_EL1 写入 GIC_PRIO_IRQOFF，提高 GIC 优先级阈值，
	 * 使 GIC 停止向该 CPU 投递普通优先级中断。 */
	write_sysreg_s(GIC_PRIO_IRQOFF, SYS_ICC_PMR_EL1);
	/* 编译器屏障：禁止临界区内的操作提前到关中断指令之前执行。 */
	barrier();
}

/*
 * arch_local_irq_disable - 关闭当前 CPU 的中断（架构统一入口）
 *
 * 运行时根据 system_uses_irq_prio_masking() 分派到两条路径：
 *   true  → __pmr_local_irq_disable()
 *           伪 NMI 系统，写 ICC_PMR_EL1 = IRQOFF，保留高优先级 FIQ 穿透。
 *   false → __daif_local_irq_disable()
 *           普通系统，写 daifset #3，IRQ 和 FIQ 一并屏蔽，开销最小。
 *
 * __always_inline 确保调用处内联展开，避免函数调用开销（关中断是极热路径）。
 * system_uses_irq_prio_masking() 内部使用静态分支，运行时开销接近零。
 */
static __always_inline void arch_local_irq_disable(void)
{
	/* 静态分支：启动时 patch 一次，之后此处执行单条指令判断路径。 */
	if (system_uses_irq_prio_masking()) {
		__pmr_local_irq_disable();
	} else {
		__daif_local_irq_disable();
	}
}

/*
 * __daif_local_save_flags - 读取当前 DAIF 寄存器值作为中断状态快照
 *
 * 返回值中 PSR_I_BIT(bit7) 置 1 表示 IRQ 当前被屏蔽，
 * 配合 __daif_local_irq_restore() 可恢复到保存时的状态。
 */
static __always_inline unsigned long __daif_local_save_flags(void)
{
	/* read_sysreg(daif) 展开为 "mrs <reg>, daif"，读出完整的 DAIF 值。
	 * 返回值直接用于后续的 irqs_disabled_flags() 判断和 irq_restore()。 */
	return read_sysreg(daif);
}

/*
 * __pmr_local_save_flags - 读取当前 ICC_PMR_EL1 值作为中断状态快照
 *
 * PMR 路径下，中断状态由 ICC_PMR_EL1 的值决定（IRQON 或 IRQOFF），
 * 保存该值即可在 restore 时精确还原中断使能状态。
 */
static __always_inline unsigned long __pmr_local_save_flags(void)
{
	/* 直接读 ICC_PMR_EL1 系统寄存器，返回当前优先级屏蔽阈值。 */
	return read_sysreg_s(SYS_ICC_PMR_EL1);
}

/*
 * arch_local_save_flags - 保存当前中断状态（架构统一入口）
 *
 * 返回值是不透明的"flags"，只应传给 arch_local_irq_restore() 或
 * arch_irqs_disabled_flags()，不应直接解读其数值含义（两条路径
 * 返回的是不同寄存器的值，语义不同）。
 */
static __always_inline unsigned long arch_local_save_flags(void)
{
	if (system_uses_irq_prio_masking()) {
		/* PMR 路径：返回 ICC_PMR_EL1 当前值（IRQON 或 IRQOFF）。 */
		return __pmr_local_save_flags();
	} else {
		/* DAIF 路径：返回 DAIF 寄存器当前值，PSR_I_BIT 表示中断屏蔽状态。 */
		return __daif_local_save_flags();
	}
}

/*
 * __daif_irqs_disabled_flags - 判断 DAIF flags 快照中 IRQ 是否被屏蔽
 *
 * PSR_I_BIT 对应 DAIF 寄存器中的 I 位（IRQ 屏蔽位），
 * 该位为 1 表示 IRQ 屏蔽中，为 0 表示 IRQ 开启。
 */
static __always_inline bool __daif_irqs_disabled_flags(unsigned long flags)
{
	/* 与 PSR_I_BIT 做位与：非零则说明保存时 IRQ 处于屏蔽状态。 */
	return flags & PSR_I_BIT;
}

/*
 * __pmr_irqs_disabled_flags - 判断 PMR flags 快照中中断是否被屏蔽
 *
 * PMR 路径只有两种合法状态：
 *   GIC_PRIO_IRQON  → 中断开启
 *   GIC_PRIO_IRQOFF → 中断屏蔽（或任何非 IRQON 的值）
 * 因此"不等于 IRQON"即视为中断被屏蔽，涵盖所有关中断状态。
 */
static __always_inline bool __pmr_irqs_disabled_flags(unsigned long flags)
{
	/* flags != GIC_PRIO_IRQON 即表示中断未开启（屏蔽或中间状态）。 */
	return flags != GIC_PRIO_IRQON;
}

/*
 * arch_irqs_disabled_flags - 判断保存的 flags 中中断是否被屏蔽（架构统一入口）
 *
 * 通常与 arch_local_save_flags() 配合使用：
 *   flags = arch_local_save_flags();
 *   if (arch_irqs_disabled_flags(flags)) { ... }
 */
static __always_inline bool arch_irqs_disabled_flags(unsigned long flags)
{
	if (system_uses_irq_prio_masking()) {
		/* PMR 路径：flags 是 ICC_PMR_EL1 的值，非 IRQON 即为屏蔽。 */
		return __pmr_irqs_disabled_flags(flags);
	} else {
		/* DAIF 路径：flags 是 DAIF 寄存器值，检查 PSR_I_BIT。 */
		return __daif_irqs_disabled_flags(flags);
	}
}

/*
 * __daif_irqs_disabled - 直接读 DAIF 寄存器判断当前 IRQ 是否被屏蔽
 *
 * 先调用 __daif_local_save_flags() 读出 DAIF 当前值，
 * 再用 __daif_irqs_disabled_flags() 检查 PSR_I_BIT，合二为一。
 */
static __always_inline bool __daif_irqs_disabled(void)
{
	return __daif_irqs_disabled_flags(__daif_local_save_flags());
}

/*
 * __pmr_irqs_disabled - 直接读 ICC_PMR_EL1 判断当前中断是否被屏蔽
 *
 * 先调用 __pmr_local_save_flags() 读出 PMR 当前值，
 * 再用 __pmr_irqs_disabled_flags() 判断是否为非 IRQON 状态。
 */
static __always_inline bool __pmr_irqs_disabled(void)
{
	return __pmr_irqs_disabled_flags(__pmr_local_save_flags());
}

/*
 * arch_irqs_disabled - 查询当前 CPU 中断是否被屏蔽（架构统一入口）
 *
 * 无需传入 flags，直接读取当前寄存器状态并返回布尔值。
 * 常用于断言或调试代码（如 WARN_ON(!irqs_disabled())）。
 */
static __always_inline bool arch_irqs_disabled(void)
{
	if (system_uses_irq_prio_masking()) {
		/* PMR 路径：读 ICC_PMR_EL1，判断是否处于 IRQON 状态。 */
		return __pmr_irqs_disabled();
	} else {
		/* DAIF 路径：读 DAIF 寄存器，检查 I 位是否置 1。 */
		return __daif_irqs_disabled();
	}
}

/*
 * __daif_local_irq_save - 保存 DAIF 中断状态并关闭中断（原子操作，传统路径）
 *
 * 先读后写，确保调用者拿到关中断之前的状态快照，
 * 之后可用 __daif_local_irq_restore(flags) 精确还原。
 * 注意：读取和写入之间不是原子的，但在单 CPU 上下文中这不是问题，
 * 因为中断尚未关闭时不会有其他路径修改当前 CPU 的 DAIF。
 */
static __always_inline unsigned long __daif_local_irq_save(void)
{
	/* 先保存当前 DAIF 值，此时中断可能是开启的。 */
	unsigned long flags = __daif_local_save_flags();

	/* 再关闭中断（写 daifset #3），进入临界区。 */
	__daif_local_irq_disable();

	/* 返回关中断之前的 DAIF 快照，供 restore 时使用。 */
	return flags;
}

/*
 * __pmr_local_irq_save - 保存 PMR 中断状态并关闭中断（原子操作，伪 NMI 路径）
 *
 * PMR 路径的 save 与 DAIF 路径略有不同：若中断已经处于某种屏蔽状态
 * （包括中间状态），则不再重复写 IRQOFF，直接返回当前快照。
 * 这是因为 PMR 可能处于多种"已关中断"状态（如嵌套关中断），
 * 重复写 IRQOFF 会覆盖原有的更精确状态，导致 restore 时无法正确还原。
 */
static __always_inline unsigned long __pmr_local_irq_save(void)
{
	/* 先读出当前 ICC_PMR_EL1 值作为快照。 */
	unsigned long flags = __pmr_local_save_flags();

	/*
	 * There are too many states with IRQs disabled, just keep the current
	 * state if interrupts are already disabled/masked.
	 *
	 * 只有当前处于开中断状态（flags == IRQON）时，才需要主动关中断；
	 * 若已经处于某种屏蔽状态，保持现状并直接返回快照，
	 * 避免用 IRQOFF 覆盖可能更精确的中间屏蔽值。
	 */
	if (!__pmr_irqs_disabled_flags(flags))
		__pmr_local_irq_disable();

	return flags;
}

/*
 * arch_local_irq_save - 保存中断状态并关闭中断（架构统一入口）
 *
 * 等价于：flags = save(); disable(); return flags;
 * 返回的 flags 只应传给 arch_local_irq_restore()，不应直接比较数值。
 * 典型用法：
 *   unsigned long flags = arch_local_irq_save();
 *   // 临界区
 *   arch_local_irq_restore(flags);
 */
static __always_inline unsigned long arch_local_irq_save(void)
{
	if (system_uses_irq_prio_masking()) {
		return __pmr_local_irq_save();
	} else {
		return __daif_local_irq_save();
	}
}

/*
 * __daif_local_irq_restore - 将 DAIF 寄存器恢复到保存时的状态（传统路径）
 *
 * 直接将 flags 写回 DAIF 寄存器，精确还原所有 4 个屏蔽位（D/A/I/F）。
 * 与 daifset/daifclr 不同，write_sysreg(flags, daif) 是全量写入，
 * 可以一次性还原整个 DAIF 状态，无需分别操作各位。
 */
static __always_inline void __daif_local_irq_restore(unsigned long flags)
{
	/* 编译器屏障：确保临界区内的所有内存操作在 restore 之前完成。 */
	barrier();
	/* 将保存时的 DAIF 值全量写回，恢复关中断/开中断状态。 */
	write_sysreg(flags, daif);
	/* 编译器屏障：防止 restore 之后的代码被提前到临界区内执行。 */
	barrier();
}

/*
 * __pmr_local_irq_restore - 将 ICC_PMR_EL1 恢复到保存时的状态（伪 NMI 路径）
 *
 * 将 __pmr_local_irq_save() 返回的 PMR 快照写回 ICC_PMR_EL1，
 * 精确还原 GIC 优先级屏蔽阈值（可能是 IRQON 或 IRQOFF）。
 */
static __always_inline void __pmr_local_irq_restore(unsigned long flags)
{
	/* 编译器屏障：确保临界区操作不被重排到 restore 之后。 */
	barrier();
	/* 将保存时的 PMR 值写回 ICC_PMR_EL1，还原 GIC 中断屏蔽阈值。 */
	write_sysreg_s(flags, SYS_ICC_PMR_EL1);
	/* pmr_sync()：等待 GIC 确认 PMR 写入已生效。
	 * restore 可能是从 IRQOFF 恢复到 IRQON（即开中断），
	 * 此时必须同步，确保 GIC 重新开始投递中断后 CPU 不会漏掉已排队的中断。
	 * 若 restore 的目标是 IRQOFF（嵌套关中断场景），sync 虽多余但无害。 */
	pmr_sync();
	/* 编译器屏障：防止 restore 之后的代码提前到临界区内。 */
	barrier();
}

/*
 * arch_local_irq_restore - 恢复中断状态到 arch_local_irq_save() 保存时的值
 *
 * flags 必须来自同一 CPU 上的 arch_local_irq_save() 或 arch_local_save_flags()，
 * 不能跨 CPU 传递（每个 CPU 有独立的 DAIF/PMR 寄存器）。
 */
static __always_inline void arch_local_irq_restore(unsigned long flags)
{
	if (system_uses_irq_prio_masking()) {
		/* PMR 路径：写 ICC_PMR_EL1 并调用 pmr_sync() 等待 GIC 确认。 */
		__pmr_local_irq_restore(flags);
	} else {
		/* DAIF 路径：全量写回 DAIF 寄存器，立即生效。 */
		__daif_local_irq_restore(flags);
	}
}

#endif /* __ASM_IRQFLAGS_H */
