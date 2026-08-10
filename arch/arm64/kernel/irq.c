// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 IRQ 架构层学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件位于 ARM64 异常入口与通用 IRQ 子系统之间，负责三类架构工作：
 *   1. 为每个 possible CPU 建立独立的硬中断栈，以及启用 SCS 时配套的
 *      IRQ Shadow Call Stack；
 *   2. 提供 handle_arch_irq/handle_arch_fiq 顶层函数指针，让具体
 *      irqchip 在启动期把汇编异常入口接到 GIC、AIC 等根控制器；
 *   3. 在 irqchip 初始化完成后，让 DAIF 屏蔽位与 GIC PMR 优先级阈值
 *      进入一致状态，为 ARM64 pseudo-NMI 模式开放安全的高优先级入口。
 *
 * 主要调用链：
 *   start_kernel()
 *     -> early_irq_init()              建立通用 irq_desc
 *     -> init_IRQ()                    本文件的架构入口
 *        -> init_irq_stacks()/init_irq_scs()
 *        -> irqchip_init()
 *           -> 根 irqchip 初始化
 *              -> set_handle_irq()/set_handle_fiq()
 *
 * 运行期硬中断链：
 *   ARM64 vector -> entry-common.c -> el0/el1_interrupt()
 *     -> call_on_irq_stack(regs, handle_arch_irq)
 *     -> GIC/AIC 顶层 handler -> irq_domain 映射 -> 通用 irq_desc 流控。
 *
 * 核心不变量：
 *   - 任一 CPU 可能接收 IRQ 前，其 irq_stack_ptr 已指向完整分配的栈；
 *   - 启用 SCS 时，切换 IRQ 栈必须同步切换该 CPU 的 IRQ SCS；
 *   - 顶层 IRQ/FIQ handler 在启动期最多注册一次，之后由 __ro_after_init
 *     阻止意外改写；
 *   - IRQ 栈只隔离中断嵌套造成的内核线程栈消耗，不替代通用 IRQ 层的
 *     irq_enter/irq_exit、锁、RCU 或 irq_desc 生命周期协议。
 *
 * 方案权衡：每 CPU 专用栈增加固定的虚拟地址和物理内存开销，却把
 * 不可预测的中断嵌套从任务栈隔离；间接顶层 handler 支持多种根控制器
 * 共用异常入口，代价是启动期必须在开放中断前完成一次且仅一次的
 * 安全发布。
 */
/*
 * Based on arch/arm/kernel/irq.c
 *
 * Copyright (C) 1992 Linus Torvalds
 * Modifications for ARM processor Copyright (C) 1995-2000 Russell King.
 * Support for Dynamic Tick Timer Copyright (C) 2004-2005 Nokia Corporation.
 * Dynamic Tick Timer written by Tony Lindgren <tony@atomide.com> and
 * Tuukka Tikkanen <tuukka.tikkanen@elektrobit.com>.
 * Copyright (C) 2012 ARM Ltd.
 */
/*
 * 原文说明：本实现源自 32 位 ARM 的 arch/arm/kernel/irq.c，并保留了
 * 相应作者与动态 tick 支持的版权记录。ARM64 版本在共同的 IRQ 框架之外，
 * 额外处理每 CPU vmap IRQ 栈、Shadow Call Stack、FIQ 和 pseudo-NMI。
 */

#include <linux/errno.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/kprobes.h>
#include <linux/memory.h>
#include <linux/scs.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <linux/vmalloc.h>
#include <asm/daifflags.h>
#include <asm/exception.h>
#include <asm/numa.h>
#include <asm/softirq_stack.h>
#include <asm/stacktrace.h>
#include <asm/vmap_stack.h>

/* Only access this in an NMI enter/exit */
/*
 * nmi_contexts 只能由 NMI 进入/退出协议访问。每 CPU 的 nmi_ctx
 * 保存嵌套计数 cnt 和最外层入口前的 HCR_EL2；
 * arch_nmi_enter/exit() 用编译器屏障约束两字段顺序，使嵌套 NMI
 * 不会覆盖最外层恢复现场。普通 IRQ 代码不得把它当作通用计数器，
 * 否则会破坏 HCR_EL2 的成对恢复。
 */
DEFINE_PER_CPU(struct nmi_ctx, nmi_contexts);

/*
 * irq_stack_ptr 是每 CPU IRQ 栈的低地址基址。init_irq_stacks() 在启动 CPU
 * 尚未开放正常中断时为所有 possible CPU 发布它；entry.S 的
 * call_on_irq_stack() 读取本 CPU 指针，加 IRQ_STACK_SIZE 得到向下增长栈
 * 的初始 SP。对象在系统运行期永久保留，不发生热插拔释放。
 */
DEFINE_PER_CPU(unsigned long *, irq_stack_ptr);

/*
 * 汇编入口无论配置如何都需要看到 irq_shadow_call_stack_ptr 的声明；
 * 只有 CONFIG_SHADOW_CALL_STACK=y 时才实际定义存储并生成相关访问指令。
 * 每个元素是该 CPU 专用 IRQ SCS 的基址，所有权由架构启动代码永久持有。
 */
DECLARE_PER_CPU(unsigned long *, irq_shadow_call_stack_ptr);

#ifdef CONFIG_SHADOW_CALL_STACK
DEFINE_PER_CPU(unsigned long *, irq_shadow_call_stack_ptr);
#endif

/*
 * init_irq_scs() - 为所有可能上线的 CPU 分配 IRQ 专用 Shadow Call Stack。
 *
 * 调用位置：init_IRQ() 在 irqchip_init() 之前调用。入参：无；启动期串行
 * 上下文，允许 vmalloc/GFP_KERNEL 路径睡眠。scs_is_enabled() 为 false
 * 时返回 0 且无副作用；启用时按 CPU 的早期 NUMA 节点逐个分配，全部成功
 * 返回 0，任一失败返回 -ENOMEM。
 *
 * 成功的 SCS 指针写入对应 per-CPU 槽，供 call_on_irq_stack() 切换
 * scs_sp。函数不提供回滚：失败会由 init_IRQ() 立即 panic，内核不会
 * 继续运行，也就没有需要恢复到“可启动”状态的调用者。已分配对象
 * 存活到关机。
 */
static int __init init_irq_scs(void)
{
	/*
	 * 变量地图：
	 *   cpu 当前 possible CPU 编号；包括尚未 online、以后可能热插拔
	 *       上线者。
	 *   s   当前新分配 SCS 的持有指针；发布到 per-CPU 槽后，所有权不再
	 *       由局部变量承担。
	 */
	int cpu;
	void *s;

	/*
	 * 快速路径：编译期未启用或动态 SCS 最终关闭时，不建立无用的
	 * IRQ SCS。
	 */
	if (!scs_is_enabled())
		return 0;

	/*
	 * 构造阶段：为所有 possible CPU 预先分配，而不是等 CPU online 再分配，
	 * 确保任何 CPU 第一次允许中断前，汇编栈切换所需指针已经存在。
	 */
	for_each_possible_cpu(cpu) {
		s = scs_alloc(early_cpu_to_node(cpu));
		if (!s)
			return -ENOMEM;
		/*
		 * 这是对该 CPU 异常入口的发布点。此时中断控制器尚未
		 * 初始化，没有运行期读者与写入竞争；后续该槽只读。
		 */
		per_cpu(irq_shadow_call_stack_ptr, cpu) = s;
	}

	return 0;
}

/*
 * init_irq_stacks() - 为所有可能上线的 CPU 分配 ARM64 硬中断栈。
 *
 * 调用位置和上下文与 init_irq_scs() 相同。入参：无。每个栈大小为
 * IRQ_STACK_SIZE，按 early_cpu_to_node(cpu) 尽量从本地 NUMA 节点分配；
 * 全部成功返回 0，失败返回 -ENOMEM。arch_alloc_vmap_stack() 按
 * THREAD_ALIGN 建立 vmap 区域，使 ARM64 的栈边界/溢出检测约定成立。
 *
 * 已发布栈由对应 CPU 的异常入口永久借用，不转移给任务，也不随 CPU
 * offline 释放。部分失败不回滚，原因同上：init_IRQ() 将把它升级为
 * 不可恢复的启动失败，而不是在缺失 IRQ 栈的条件下冒险开放中断。
 */
static int __init init_irq_stacks(void)
{
	/*
	 * 变量地图：
	 *   cpu 当前预配置 CPU；
	 *   p   当前 IRQ 栈低地址基址，写入 per-CPU 槽后生命周期持续到关机。
	 */
	int cpu;
	unsigned long *p;

	/*
	 * 与 CPU 热插拔的关键不变量：possible 集合中的每个 CPU 都提前有栈。
	 */
	for_each_possible_cpu(cpu) {
		p = arch_alloc_vmap_stack(IRQ_STACK_SIZE, early_cpu_to_node(cpu));
		if (!p)
			return -ENOMEM;
		/*
		 * irqchip 尚未发布 handler，因此这里的单次写入不需要
		 * 并发同步。
		 */
		per_cpu(irq_stack_ptr, cpu) = p;
	}

	return 0;
}

#ifdef CONFIG_SOFTIRQ_ON_OWN_STACK
/*
 * ____do_softirq() - 适配 call_on_irq_stack() 函数签名的 softirq 包装器。
 *
 * @regs 由统一栈切换接口传入；softirq 没有新的异常 pt_regs，调用者实际
 * 传 NULL，本函数不读取也不持有它。当前已经位于本 CPU IRQ 栈（启用 SCS
 * 时也已切换 IRQ SCS），不可睡眠；无直接返回值，副作用由 __do_softirq()
 * 完成，包括运行本 CPU pending softirq 并更新相应 softirq 状态。
 */
static void ____do_softirq(struct pt_regs *regs)
{
	__do_softirq();
}

/*
 * do_softirq_own_stack() - 在本 CPU 专用 IRQ 栈上执行待处理 softirq。
 *
 * 入参：无；返回：无直接返回值。由通用 softirq 核心在
 * CONFIG_SOFTIRQ_ON_OWN_STACK 下调用，入口已处于不允许睡眠的中断处理
 * 语境。call_on_irq_stack() 保存任务栈/返回地址和异常屏蔽状态，切到
 * irq_stack_ptr（及 IRQ SCS），调用包装器后完整恢复。
 *
 * NULL 明确表示没有可供 softirq 使用的新异常现场；真正输入是本 CPU 的
 * pending 位图。独立栈避免大量 softirq 回调继续消耗被中断任务的内核栈。
 */
void do_softirq_own_stack(void)
{
	call_on_irq_stack(NULL, ____do_softirq);
}
#endif

/*
 * default_handle_irq() - 根 IRQ handler 尚未注册时的故障哨兵。
 *
 * @regs 是异常入口借用的寄存器现场，仅用于满足顶层 handler 契约；函数
 * 不返回而 panic。正常启动必须由根 irqchip 在开放 IRQ 前调用
 * set_handle_irq() 替换它，因此到达这里说明中断发布顺序被破坏，与其在
 * 未知控制器状态下返回并反复触发 IRQ，不如立即停止系统。
 */
static void default_handle_irq(struct pt_regs *regs)
{
	panic("IRQ taken without a root IRQ handler\n");
}

/*
 * default_handle_fiq() - 根 FIQ handler 尚未注册时的故障哨兵。
 *
 * @regs 与 default_handle_irq() 相同，是借用现场。函数不返回而 panic。
 * 未使用独立 FIQ 路径的平台通常不会收到 FIQ；支持 FIQ（例如 Apple AIC）
 * 的根控制器必须在启动期通过 set_handle_fiq() 完成替换。
 */
static void default_handle_fiq(struct pt_regs *regs)
{
	panic("FIQ taken without a root FIQ handler\n");
}

/*
 * handle_arch_irq/handle_arch_fiq 是异常入口与根中断控制器之间的单一分派
 * 槽。初值是 panic 哨兵；irqchip 初始化成功后分别指向 GIC/AIC 等实现。
 * __ro_after_init 使初始化结束后的页映射只读，防止内存破坏或错误代码在
 * 运行期劫持所有 IRQ/FIQ。调用方只借用 pt_regs，不取得对象所有权。
 */
void (*handle_arch_irq)(struct pt_regs *) __ro_after_init = default_handle_irq;
void (*handle_arch_fiq)(struct pt_regs *) __ro_after_init = default_handle_fiq;

/*
 * set_handle_irq() - 一次性注册 ARM64 根 IRQ 分派函数。
 *
 * @handle_irq 是不可为 NULL、必须在内核运行期持续有效的函数指针，输入
 * 现场为异常入口借用的 struct pt_regs。主要调用者是 irqchip_init()
 * 探测到的根控制器驱动，例如 GICv3 的 gic_handle_irq。
 *
 * 启动期串行执行，不需要锁、允许 pr_info 输出；当前槽仍为默认哨兵时
 * 发布新指针并返回 0，已被任何控制器注册则返回 -EBUSY 且保持原
 * handler。该检查禁止“后探测者覆盖先探测者”，从而让系统只有一个
 * 根 IRQ 入口。
 */
int __init set_handle_irq(void (*handle_irq)(struct pt_regs *))
{
	/* default_handle_irq 同时充当“尚未注册”的唯一状态标记。 */
	if (handle_arch_irq != default_handle_irq)
		return -EBUSY;

	/*
	 * 发布发生在 IRQ 仍被屏蔽的启动阶段，故无需运行期 RCU/锁；随后
	 * 日志用 %ps 输出符号名，便于确认实际选中的根控制器实现。
	 */
	handle_arch_irq = handle_irq;
	pr_info("Root IRQ handler: %ps\n", handle_irq);
	return 0;
}

/*
 * set_handle_fiq() - 一次性注册 ARM64 根 FIQ 分派函数。
 *
 * @handle_fiq 的存活期、pt_regs 借用和启动期上下文与 set_handle_irq()
 * 相同。槽仍为 default_handle_fiq 时写入并返回 0，已注册则返回 -EBUSY。
 * 多数 GIC 平台只注册 IRQ；需要独立 FIQ 顶层路径的控制器才调用本函数。
 * 成功副作用是随后所有 FIQ 异常改由该回调处理。
 */
int __init set_handle_fiq(void (*handle_fiq)(struct pt_regs *))
{
	if (handle_arch_fiq != default_handle_fiq)
		return -EBUSY;

	/*
	 * 与 IRQ 槽相同，在开放异常前完成单次发布，并记录最终函数符号。
	 */
	handle_arch_fiq = handle_fiq;
	pr_info("Root FIQ handler: %ps\n", handle_fiq);
	return 0;
}

/*
 * init_IRQ() - 建立 ARM64 可安全接收硬中断所需的全部架构资源。
 *
 * 入参：无；返回：无直接返回值。由 start_kernel() 在 early_irq_init()
 * 之后、tick_init() 之前调用，处于单 CPU 启动期且普通 IRQ/FIQ 仍屏蔽，
 * 可执行 vmap 分配和 irqchip 探测。函数分三阶段：
 *   1. 为所有 possible CPU 准备 IRQ 栈和可选 SCS；
 *   2. 通过 DT/ACPI irqchip 框架初始化根控制器和 irq_domain，并发布
 *      handle_arch_irq/fiq；
 *   3. 若启用 GIC 优先级屏蔽，统一 DAIF 与 PMR 的逻辑 IRQ-off 状态。
 *
 * 栈资源失败没有可安全降级方案，函数直接 panic；irqchip_init() 的具体
 * 探测失败也由相应驱动/后续默认 handler 暴露。成功出口仍保持普通 IRQ
 * 关闭，start_kernel() 会在更晚阶段统一开放中断。
 */
void __init init_IRQ(void)
{
	/*
	 * 阶段 1：先分配普通 IRQ 栈，再分配与其配套的 SCS。逻辑或短路
	 * 意味着 IRQ 栈失败时不会继续分配 SCS；任一步失败都终止启动，
	 * 避免某个 CPU 在栈指针为空时进入异常。
	 */
	if (init_irq_stacks() || init_irq_scs())
		panic("Failed to allocate IRQ stack resources\n");

	/*
	 * 阶段 2：irqchip_init() 遍历固件声明的控制器，建立父子 domain
	 * 和根控制器硬件状态；根驱动在此调用 set_handle_irq/fiq。此调用
	 * 必须晚于栈准备，因为控制器初始化后可能逐步具备异常投递
	 * 能力。
	 */
	irqchip_init();

	/*
	 * 阶段 3 只存在于 ARM64_HAS_GIC_PRIO_MASKING 系统。该能力允许 Linux
	 * 用 ICC_PMR_EL1 屏蔽普通 IRQ，同时让更高优先级 pseudo-NMI 穿透。
	 */
	if (system_uses_irq_prio_masking()) {
		/*
		 * Now that we have a stack for our IRQ handler, set
		 * the PMR/PSR pair to a consistent state.
		 */
		/*
		 * 既然 IRQ handler 已经有安全栈，现在可以把 PMR
		 * 与 PSTATE/DAIF 这两半中断屏蔽状态调整为一致。之所以必须等
		 * 栈就绪，是因为恢复过程中可能允许高优先级 pseudo-NMI 到达。
		 */
		/*
		 * 入口预期异步错误 A 位未屏蔽；WARN 发现异常启动状态但
		 * 仍继续修复。DAIF_PROCCTX_NOIRQ 表示普通 IRQ/FIQ 逻辑上关闭、
		 * SError 可接收。local_daif_restore() 在 PMR 模式下会用 PMR
		 * 保持普通 IRQ 屏蔽，同时按协议清理 DAIF.I/F，使 pseudo-NMI
		 * 具备穿透条件。
		 */
		WARN_ON(read_sysreg(daif) & PSR_A_BIT);
		local_daif_restore(DAIF_PROCCTX_NOIRQ);
	}
}
