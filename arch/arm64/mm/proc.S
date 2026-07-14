/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/mm/proc.S
 *
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

/*
 * 【文件整体说明】
 *
 * 本文件（arch/arm64/mm/proc.S）负责 ARM64 处理器级别的内存管理初始化，
 * 以及系统休眠/唤醒时的 CPU 寄存器上下文保存与恢复。
 *
 * 核心函数及其在启动链中的位置：
 *
 *   arch/arm64/kernel/head.S
 *       └─ primary_entry()
 *           └─ __primary_switch()
 *               ├─ __cpu_setup()          ← 本文件实现，MMU 开启前的处理器初始化
 *               │     配置 MAIR_EL1（内存属性）和 TCR_EL1（地址翻译控制），
 *               │     返回 SCTLR_EL1 的目标值
 *               └─ enable_mmu()           ← 使用 __cpu_setup 返回值开启 MMU
 *
 * 其他函数：
 *   cpu_do_suspend()  - 系统进入休眠前保存 CPU 寄存器
 *   cpu_do_resume()   - 系统从休眠唤醒后恢复 CPU 寄存器
 *   idmap_cpu_replace_ttbr1()       - 低级页表切换辅助函数
 *   idmap_kpti_install_ng_mappings()- KPTI（内核页表隔离）安装非全局映射
 */

#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/pgtable.h>
#include <linux/cfi_types.h>
#include <asm/assembler.h>
#include <asm/asm-offsets.h>
#include <asm/asm_pointer_auth.h>
#include <asm/hwcap.h>
#include <asm/kernel-pgtable.h>
#include <asm/pgtable-hwdef.h>
#include <asm/cpufeature.h>
#include <asm/alternative.h>
#include <asm/smp.h>
#include <asm/sysreg.h>

/*
 * TCR_TG_FLAGS：TCR_EL1 中的页表粒度（Granule Size）标志位
 *
 * ARM64 支持三种页粒度：4KB、16KB、64KB，由编译时配置决定。
 * TCR_EL1 中有两组粒度位：
 *   TG0（位[15:14]）控制 TTBR0（用户空间，低地址）使用的页大小
 *   TG1（位[31:30]）控制 TTBR1（内核空间，高地址）使用的页大小
 *
 * 页粒度的选择影响页表层级数量和每个页的大小：
 *   4KB  粒度：通常需要 4 级页表，每页 4096 字节
 *   16KB 粒度：通常需要 4 级页表，每页 16384 字节
 *   64KB 粒度：通常只需 3 级页表，每页 65536 字节（减少 TLB 压力）
 */
#ifdef CONFIG_ARM64_64K_PAGES
#define TCR_TG_FLAGS	((TCR_EL1_TG0_64K << TCR_EL1_TG0_SHIFT) |\
			 (TCR_EL1_TG1_64K << TCR_EL1_TG1_SHIFT))
#elif defined(CONFIG_ARM64_16K_PAGES)
#define TCR_TG_FLAGS	((TCR_EL1_TG0_16K << TCR_EL1_TG0_SHIFT) |\
			 (TCR_EL1_TG1_16K << TCR_EL1_TG1_SHIFT))
#else /* CONFIG_ARM64_4K_PAGES */
#define TCR_TG_FLAGS	((TCR_EL1_TG0_4K << TCR_EL1_TG0_SHIFT) |\
			 (TCR_EL1_TG1_4K << TCR_EL1_TG1_SHIFT))
#endif

/*
 * TCR_KASLR_FLAGS：内核地址空间随机化（KASLR）相关标志位
 *
 * 当启用 CONFIG_RANDOMIZE_BASE（KASLR）时，内核被加载到随机的物理/虚拟地址，
 * 以增强安全性（抵御基于固定地址的漏洞利用）。
 *
 * TCR_EL1_NFD1（No Fault on Translation stage 1 for TTBR1，位[54]）：
 *   设置此位后，通过 TTBR1 发生的地址翻译错误不会立即触发异常，
 *   而是推迟到实际内存访问时处理。这允许内核在随机化地址尚未完全
 *   映射时安全地进行地址探测，是 KASLR 实现的必要条件。
 */
#ifdef CONFIG_RANDOMIZE_BASE
#define TCR_KASLR_FLAGS	TCR_EL1_NFD1
#else
#define TCR_KASLR_FLAGS	0
#endif

/*
 * TCR_CACHE_FLAGS：页表遍历（Page Table Walk，PTW）的缓存策略
 *
 * 页表遍历是 MMU 硬件自动执行的过程：当 TLB 未命中时，MMU 自动读取
 * 内存中的多级页表以完成地址翻译。这些页表读取同样需要经过 cache，
 * 因此需要在 TCR_EL1 中单独配置页表遍历的缓存属性。
 *
 * WBWA = Write-Back Write-Allocate（回写 + 写分配）：
 *   Write-Back：写操作只更新 cache，不立即写回内存（性能最好）
 *   Write-Allocate：写 miss 时先把该行加载到 cache 再写（减少后续 miss）
 *   这是性能最优的缓存策略，适合频繁访问的页表数据。
 *
 * IRGN（Inner Region）：内部共享域（同一 cluster 内的 cache）的缓存策略
 * ORGN（Outer Region）：外部共享域（跨 cluster 的 cache，如 LLC）的缓存策略
 * 两者都设为 WBWA，确保页表遍历在所有级别的 cache 上都有最优性能。
 */
/* PTWs cacheable, inner/outer WBWA */
#define TCR_CACHE_FLAGS	TCR_IRGN_WBWA | TCR_ORGN_WBWA

/*
 * TCR_KASAN_SW_FLAGS：内核地址消毒器软件标签（KASAN Software Tags）相关标志
 *
 * KASAN（Kernel Address SANitizer）是一种内核内存错误检测工具，
 * SW_TAGS 模式利用 ARM64 指针的高位字节存储内存标签来检测 use-after-free
 * 和 out-of-bounds 等内存安全问题。
 *
 * TCR_EL1_TBI1（Top Byte Ignore for TTBR1，位[38]）：
 *   忽略 TTBR1 地址（内核虚拟地址）的最高字节（第 63~56 位）进行地址翻译。
 *   这允许内核指针在高位字节中携带标签信息，而不影响实际的内存访问。
 *
 * TCR_EL1_TBID1（Top Byte Ignore for Data accesses，位[52]）：
 *   仅对数据访问忽略高字节，指令取指仍使用完整地址（防止跳转到带标签的地址）。
 *
 * 两者配合使 KASAN 软件标签机制能够在内核空间中正常工作。
 */
#ifdef CONFIG_KASAN_SW_TAGS
#define TCR_KASAN_SW_FLAGS TCR_EL1_TBI1 | TCR_EL1_TBID1
#else
#define TCR_KASAN_SW_FLAGS 0
#endif

/*
 * TCR_MTE_FLAGS：内存标签扩展（Memory Tagging Extension，MTE）相关标志
 *
 * MTE 是 ARMv8.5-A 引入的硬件内存安全特性，为每 16 字节内存分配一个 4 位标签，
 * 并在指针高位字节存储对应标签，访问时自动比较两者是否匹配，不匹配则触发异常。
 * 这比 KASAN 软件实现有更低的性能开销。
 *
 * TCR_EL1_TCMA1（Tag Check Mask All for TTBR1，位[58]）：
 *   使用"匹配所有"标签（0xF）进行的内核空间访问视为"标签未检查"，
 *   不触发标签不匹配异常。这是因为 mte_zero_clear_page_tags() 使用
 *   DC GZVA 指令清零标签，该指令需要 TBI 开启，并用 0xF 标签绕过检查。
 *
 * TCR_EL1_TBI1（同上，位[38]）：
 *   使内核地址翻译时忽略最高字节，允许指针高位携带 MTE 标签。
 *
 * TCR_EL1_TBID1（同上，位[52]）：
 *   仅数据访问忽略高字节标签，指令取指不忽略（安全考量）。
 *
 * 注意：CONFIG_KASAN_SW_TAGS 和 CONFIG_ARM64_MTE 定义了相同的 TBI1/TBID1 位，
 * 但 MTE 额外增加了 TCMA1，两者不会同时启用。
 */
#ifdef CONFIG_ARM64_MTE
/*
 * The mte_zero_clear_page_tags() implementation uses DC GZVA, which relies on
 * TBI being enabled at EL1.  TCMA1 is needed to treat accesses with the
 * match-all tag (0xF) as Tag Unchecked, irrespective of the SCTLR_EL1.TCF
 * setting.
 */
#define TCR_MTE_FLAGS TCR_EL1_TCMA1 | TCR_EL1_TBI1 | TCR_EL1_TBID1
#else
#define TCR_MTE_FLAGS 0
#endif

/*
 * TCR_IRGN_WBWA：内部缓存（Inner Cache）策略 = Write-Back Write-Allocate
 *
 * ARM64 的 cache 层次结构分为内部（Inner）和外部（Outer）两个共享域：
 *   内部共享域（Inner Shareable）：同一处理器集群（cluster）内的所有 CPU
 *   外部共享域（Outer Shareable）：多个集群之间共享的更大范围
 *
 * TCR_EL1_IRGN0/IRGN1：分别控制 TTBR0/TTBR1 页表遍历的内部缓存策略
 *   _WBWA = 0b01 = Write-Back, Write-Allocate（回写 + 写分配，最优性能）
 *   其他选项：0b00=Non-cacheable, 0b10=Write-Through, 0b11=Write-Back no WA
 */
#define TCR_IRGN_WBWA	((TCR_EL1_IRGN0_WBWA << TCR_EL1_IRGN0_SHIFT) |\
			 (TCR_EL1_IRGN1_WBWA << TCR_EL1_IRGN1_SHIFT))

/*
 * TCR_ORGN_WBWA：外部缓存（Outer Cache）策略 = Write-Back Write-Allocate
 *
 * TCR_EL1_ORGN0/ORGN1：分别控制 TTBR0/TTBR1 页表遍历的外部缓存策略
 * 编码含义与 IRGN 相同，同样设为 WBWA 以获得最优的页表遍历性能。
 * 外部 cache 通常指末级缓存（LLC，Last Level Cache），也采用 WBWA。
 */
#define TCR_ORGN_WBWA	((TCR_EL1_ORGN0_WBWA << TCR_EL1_ORGN0_SHIFT) |\
			 (TCR_EL1_ORGN1_WBWA << TCR_EL1_ORGN1_SHIFT))

/*
 * TCR_SHARED：页表遍历的共享属性 = Inner Shareable（内部共享）
 *
 * TCR_EL1_SH0/SH1：分别控制 TTBR0/TTBR1 页表遍历访问的共享域属性：
 *   0b00 = Non-shareable（不共享）
 *   0b10 = Outer Shareable（外部共享）
 *   0b11 = Inner Shareable（内部共享）← 此处选择
 *
 * 设为 Inner Shareable 意味着页表数据在同一集群内的所有 CPU 之间保持一致，
 * 这对 SMP（对称多处理）系统至关重要：当一个 CPU 修改页表后，同一集群内
 * 的其他 CPU 通过 cache 一致性协议能看到最新的页表数据，避免使用过期映射。
 */
#define TCR_SHARED	((TCR_EL1_SH0_INNER << TCR_EL1_SH0_SHIFT) |\
			 (TCR_EL1_SH1_INNER << TCR_EL1_SH1_SHIFT))

/*
 * MAIR_EL1_SET：内存属性间接寄存器（Memory Attribute Indirection Register）的默认值
 *
 * MAIR_EL1 是一个 64 位寄存器，被划分为 8 个 8 位槽（索引 0～7），
 * 每个槽定义一种内存类型的缓存属性。页表项通过 3 位的 AttrIdx 字段
 * 索引到 MAIR_EL1 中的对应槽，从而指定该内存页的访问属性。
 *
 * Linux 内核定义的内存类型索引（MT_* 宏，见 asm/pgtable-hwdef.h）：
 *
 *   MT_DEVICE_nGnRnE（索引 0）= MAIR_ATTR_DEVICE_nGnRnE（0x00）
 *     设备内存，non-Gathering（不聚合），non-Reordering（不重排），
 *     non-Early Write Acknowledgement（无提前写确认）。
 *     最严格的设备内存，用于强顺序 MMIO 寄存器（如中断控制器）。
 *     所有访问必须严格按顺序执行，不允许任何优化。
 *
 *   MT_DEVICE_nGnRE（索引 1）= MAIR_ATTR_DEVICE_nGnRE（0x04）
 *     设备内存，non-Gathering，non-Reordering，Early Write Acknowledgement 允许。
 *     用于普通 MMIO 设备，写操作可以提前被系统互联确认（不必等到设备实际接收）。
 *
 *   MT_NORMAL_NC（索引 2）= MAIR_ATTR_NORMAL_NC（0x44）
 *     普通内存，非缓存（Non-Cacheable）。
 *     Inner/Outer 均不经过 cache，每次访问直接到达内存。
 *     用于 DMA 缓冲区等需要 CPU 与设备共享且不需要 cache 一致性的场景。
 *
 *   MT_NORMAL（索引 3）= MAIR_ATTR_NORMAL（0xff）
 *     普通内存，Inner/Outer 均为 Write-Back Read-Allocate Write-Allocate。
 *     这是性能最优的内存类型，所有普通内核数据和代码都使用此类型。
 *
 *   MT_NORMAL_TAGGED（索引 4）= MAIR_ATTR_NORMAL（0xff，初始值与 MT_NORMAL 相同）
 *     为 MTE（内存标签扩展）预留的槽位，初始设为与普通内存相同的属性。
 *     如果硬件支持 MTE，在 mte_cpu_setup() 中会将此槽改为带标签的普通内存属性，
 *     以启用硬件内存标签检查功能。
 *
 * Default MAIR_EL1. MT_NORMAL_TAGGED is initially mapped as Normal memory and
 * changed during mte_cpu_setup to Normal Tagged if the system supports MTE.
 */
#define MAIR_EL1_SET							\
	(MAIR_ATTRIDX(MAIR_ATTR_DEVICE_nGnRnE, MT_DEVICE_nGnRnE) |	\
	 MAIR_ATTRIDX(MAIR_ATTR_DEVICE_nGnRE, MT_DEVICE_nGnRE) |	\
	 MAIR_ATTRIDX(MAIR_ATTR_NORMAL_NC, MT_NORMAL_NC) |		\
	 MAIR_ATTRIDX(MAIR_ATTR_NORMAL, MT_NORMAL) |			\
	 MAIR_ATTRIDX(MAIR_ATTR_NORMAL, MT_NORMAL_TAGGED))

#ifdef CONFIG_CPU_PM
/**
 * cpu_do_suspend - save CPU registers context
 *
 * x0: virtual address of context pointer
 *
 * This must be kept in sync with struct cpu_suspend_ctx in <asm/suspend.h>.
 *
 * 【函数说明】
 * cpu_do_suspend 在系统进入休眠（suspend to RAM / CPU 下电）时调用，
 * 负责将 CPU 的关键系统寄存器保存到内存中（x0 指向的 cpu_suspend_ctx 结构体）。
 * 当 CPU 重新上电后，cpu_do_resume 会从同一地址恢复这些寄存器。
 *
 * 需要保存的原因：CPU 下电后，所有寄存器值（包括系统寄存器）都会丢失，
 * 而 CPU 上电时处于复位状态，需要重新配置所有系统寄存器才能恢复到休眠前的状态。
 *
 * 保存的寄存器说明：
 *   tpidr_el0    - 线程 ID 寄存器，EL0 可读写，通常用于 TLS（线程本地存储）基地址
 *   tpidrro_el0  - 线程 ID 寄存器，EL0 只读，EL1 可写，用于用户态 TLS
 *   contextidr_el1 - 上下文 ID 寄存器，存储进程 ID（PROCID），用于 trace/debug
 *   osdlr_el1    - OS 双锁寄存器（OS Double Lock），控制调试锁定状态
 *   cpacr_el1    - 协处理器访问控制寄存器，控制 FP/SIMD/SVE 的 EL0/EL1 访问权限
 *   tcr_el1      - 地址翻译控制寄存器，定义虚拟地址空间大小、页粒度等
 *   vbar_el1     - 向量基地址寄存器，指向 EL1 异常向量表的基地址
 *   mdscr_el1    - 监控调试系统控制寄存器，控制硬件断点、观察点和调试功能
 *   oslsr_el1    - OS 锁状态寄存器（只读），反映当前调试 OS 锁状态
 *   sctlr_el1    - 系统控制寄存器，包含 MMU/cache/对齐等核心控制位
 *   per-cpu offset - 每 CPU 变量的基地址偏移（内核 per-cpu 机制）
 *   sp_el0       - EL0 栈指针（内核中用于保存 task_struct 指针）
 *   x18          - 平台寄存器，可能被用作影子调用栈（Shadow Call Stack）指针
 *   TCR2_EL1     - 扩展翻译控制寄存器（ARMv8.9/v9.4 新增，可选）
 */
SYM_FUNC_START(cpu_do_suspend)
	// 读取各系统寄存器到通用寄存器（mrs = Move Register from System register）
	mrs	x2, tpidr_el0		// 用户线程 ID（TLS 指针）
	mrs	x3, tpidrro_el0		// 用户线程 ID（只读副本，通常存储 CPU 编号）
	mrs	x4, contextidr_el1	// 上下文 ID（进程 ID，用于调试 trace）
	mrs	x5, osdlr_el1		// OS 双锁寄存器（调试锁定控制）
	mrs	x6, cpacr_el1		// FP/SIMD 访问控制
	mrs	x7, tcr_el1		// 地址翻译控制
	mrs	x8, vbar_el1		// 异常向量表基地址
	mrs	x9, mdscr_el1		// 监控调试系统控制
	mrs	x10, oslsr_el1		// OS 锁状态（只读，恢复时通过 oslar_el1 间接设置）
	mrs	x11, sctlr_el1		// 系统控制寄存器（MMU/cache 开关等）
	get_this_cpu_offset x12		// 当前 CPU 的 per-cpu 偏移量（宏展开）
	mrs	x13, sp_el0		// EL0 栈指针（内核线程上下文中存 task_struct*）

	// 将寄存器值成对写入 x0 指向的 cpu_suspend_ctx 结构体
	// stp = Store Pair（一次存储两个寄存器，64 位对齐）
	stp	x2, x3, [x0]		// 偏移 +0:  tpidr_el0, tpidrro_el0
	stp	x4, x5, [x0, #16]	// 偏移 +16: contextidr_el1, osdlr_el1
	stp	x6, x7, [x0, #32]	// 偏移 +32: cpacr_el1, tcr_el1
	stp	x8, x9, [x0, #48]	// 偏移 +48: vbar_el1, mdscr_el1
	stp	x10, x11, [x0, #64]	// 偏移 +64: oslsr_el1, sctlr_el1
	stp	x12, x13, [x0, #80]	// 偏移 +80: per-cpu offset, sp_el0
	/*
	 * Save x18 as it may be used as a platform register, e.g. by shadow
	 * call stack.
	 *
	 * x18 是 ARM64 ABI 定义的平台寄存器，调用者不负责保存它（非 callee-saved），
	 * 但平台可以将其用于特殊目的（如 SCS 影子调用栈）。休眠时需要显式保存。
	 */
	str	x18, [x0, #96]		// 偏移 +96: 平台寄存器 x18

	// 如果 CPU 支持 TCR2_EL1（ARMv8.9 扩展翻译控制寄存器），则额外保存
	// alternative_if 是内核的运行时 CPU 特性替换机制：
	// 如果运行时检测到 ARM64_HAS_TCR2 特性，以下两条指令会被动态 patch 进来；
	// 否则替换为 NOP（由 alternative_else_nop_endif 负责填充）
alternative_if ARM64_HAS_TCR2
	mrs	x2, REG_TCR2_EL1	// 读取扩展翻译控制寄存器
	str	x2, [x0, #104]		// 偏移 +104: TCR2_EL1
alternative_else_nop_endif
	ret				// 返回调用者（lr 寄存器中的地址）
SYM_FUNC_END(cpu_do_suspend)

/**
 * cpu_do_resume - restore CPU register context
 *
 * x0: Address of context pointer
 *
 * 【函数说明】
 * cpu_do_resume 在 CPU 从休眠状态唤醒后调用，从 cpu_do_suspend 保存的
 * cpu_suspend_ctx 结构体中恢复所有系统寄存器，使 CPU 回到休眠前的状态。
 *
 * 注意事项：
 * 1. TCR_EL1 的 T0SZ 字段（控制 TTBR0 地址范围）不从保存值恢复，而是保留当前值。
 *    这是因为 resume 路径本身就在使用 TTBR0 恒等映射（idmap），
 *    如果修改 T0SZ 可能导致当前执行地址无法翻译，造成系统崩溃。
 * 2. oslsr_el1（OS 锁状态）是只读寄存器，需要通过写 oslar_el1 来间接控制。
 * 3. 恢复 sctlr_el1 会重新开启 MMU 和 cache，此后才能正常访问内核虚拟地址。
 */
SYM_FUNC_START(cpu_do_resume)
	// 从 cpu_suspend_ctx 结构体加载保存的寄存器值
	// ldp = Load Pair（一次加载两个寄存器，与 stp 对应）
	ldp	x2, x3, [x0]		// x2=tpidr_el0, x3=tpidrro_el0
	ldp	x4, x5, [x0, #16]	// x4=contextidr_el1, x5=osdlr_el1
	ldp	x6, x8, [x0, #32]	// x6=cpacr_el1, x8=tcr_el1（注意跳过 x7）
	ldp	x9, x10, [x0, #48]	// x9=vbar_el1, x10=mdscr_el1
	ldp	x11, x12, [x0, #64]	// x11=oslsr_el1, x12=sctlr_el1
	ldp	x13, x14, [x0, #80]	// x13=per-cpu offset, x14=sp_el0
	/*
	 * Restore x18, as it may be used as a platform register, and clear
	 * the buffer to minimize the risk of exposure when used for shadow
	 * call stack.
	 *
	 * 恢复 x18（平台/SCS 寄存器）后立即将保存槽清零，防止敏感指针（SCS 指针）
	 * 在内存中残留，降低信息泄露风险。
	 */
	ldr	x18, [x0, #96]		// 恢复平台寄存器 x18
	str	xzr, [x0, #96]		// 清零保存槽，消除敏感数据残留

	// 将通用寄存器写回系统寄存器（msr = Move to System Register）
	msr	tpidr_el0, x2		// 恢复用户线程 ID（TLS 指针）
	msr	tpidrro_el0, x3		// 恢复用户线程 ID（只读副本）
	msr	contextidr_el1, x4	// 恢复上下文 ID（进程 ID）
	msr	cpacr_el1, x6		// 恢复 FP/SIMD 访问控制

	/* Don't change t0sz here, mask those bits when restoring
	 *
	 * 不修改 TCR_EL1.T0SZ（TTBR0 地址范围大小）：
	 * resume 代码本身通过 TTBR0 的恒等映射（identity mapping）运行，
	 * T0SZ 影响 TTBR0 覆盖的地址范围，贸然恢复保存值会导致当前地址
	 * 翻译失败（PC 落在不再有效的地址范围内），系统将立即崩溃。
	 *
	 * 处理方式：读出当前 TCR_EL1 的 T0SZ 字段（x7），
	 * 用 bfi 将其回填到从内存恢复的 TCR 值（x8）中，
	 * 这样 x8 中除 T0SZ 外的其他字段都来自休眠前保存的值。
	 */
	mrs	x7, tcr_el1		// 读取当前（resume 时）的 TCR_EL1 值
	// bfi xd, xs, lsb, width：将 xs 的低 width 位插入 xd 的 [lsb+width-1:lsb]
	// 这里把当前 T0SZ 的值插入 x8（保存的 TCR）的 T0SZ 字段，保留其余位
	bfi	x8, x7, TCR_EL1_T0SZ_SHIFT, TCR_EL1_T0SZ_WIDTH

	msr	tcr_el1, x8		// 恢复 TCR_EL1（T0SZ 保持当前值，其余来自休眠前）
	msr	vbar_el1, x9		// 恢复异常向量表基地址
	msr	mdscr_el1, x10		// 恢复调试系统控制寄存器

	// 如果支持 TCR2_EL1，从保存区域恢复
alternative_if ARM64_HAS_TCR2
	ldr	x2, [x0, #104]		// 读取保存的 TCR2_EL1 值
	msr	REG_TCR2_EL1, x2	// 恢复扩展翻译控制寄存器
alternative_else_nop_endif

	msr	sctlr_el1, x12		// 恢复系统控制寄存器（重新开启 MMU/cache）
	set_this_cpu_offset x13		// 恢复 per-cpu 偏移量（宏展开）
	msr	sp_el0, x14		// 恢复 EL0 栈指针（task_struct 指针）

	/*
	 * Restore oslsr_el1 by writing oslar_el1
	 *
	 * oslsr_el1（OS 锁状态寄存器）是只读的，不能直接写入。
	 * 必须通过写 oslar_el1（OS 锁访问寄存器）来间接设置锁状态：
	 *   osdlr_el1 控制 OS 双锁（double lock）状态
	 *   oslar_el1 写入 1 设置 OS 锁，写入 0 清除 OS 锁
	 * oslsr_el1 位 [1] 是实际的锁位，ubfx 提取该位后写入 oslar_el1。
	 */
	msr	osdlr_el1, x5		// 恢复 OS 双锁寄存器
	ubfx	x11, x11, #1, #1	// ubfx: 无符号位域提取，提取 oslsr_el1 保存值的位[1]
					// 该位是 OS 锁标志（1=锁定，0=解锁）
	msr	oslar_el1, x11		// 通过写 oslar_el1 间接恢复 OS 锁状态

	// 重置用户态硬件性能计数器和活动监视器的访问控制（防止用户态滥用）
	reset_pmuserenr_el0 x0		// Disable PMU access from EL0
	reset_amuserenr_el0 x0		// Disable AMU access from EL0

	// 如果支持 RAS（Reliability/Availability/Serviceability）扩展，
	// 清零 DISR_EL1（延迟中断状态寄存器），避免残留的错误状态误触发
alternative_if ARM64_HAS_RAS_EXTN
	msr_s	SYS_DISR_EL1, xzr	// 清零延迟中断状态寄存器
alternative_else_nop_endif

	// 安装内核指针认证密钥（Pointer Authentication），nosync 表示不需要 ISB
	// x14 含 task_struct 指针，x1/x2/x3 为临时寄存器
	ptrauth_keys_install_kernel_nosync x14, x1, x2, x3
	isb				// 指令同步屏障：确保所有系统寄存器修改对后续指令可见
	ret				// 返回调用者
SYM_FUNC_END(cpu_do_resume)
#endif

	.pushsection ".idmap.text", "a"

.macro	__idmap_cpu_set_reserved_ttbr1, tmp1, tmp2
	adrp	\tmp1, reserved_pg_dir
	phys_to_ttbr \tmp2, \tmp1
	offset_ttbr1 \tmp2, \tmp1
	msr	ttbr1_el1, \tmp2
	isb
	tlbi	vmalle1
	dsb	nsh
	isb
.endm

/*
 * void idmap_cpu_replace_ttbr1(phys_addr_t ttbr1)
 *
 * This is the low-level counterpart to cpu_replace_ttbr1, and should not be
 * called by anything else. It can only be executed from a TTBR0 mapping.
 */
SYM_TYPED_FUNC_START(idmap_cpu_replace_ttbr1)
	__idmap_cpu_set_reserved_ttbr1 x1, x3

	offset_ttbr1 x0, x3
	msr	ttbr1_el1, x0
	isb

	ret
SYM_FUNC_END(idmap_cpu_replace_ttbr1)
SYM_FUNC_ALIAS(__pi_idmap_cpu_replace_ttbr1, idmap_cpu_replace_ttbr1)
	.popsection

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0

#define KPTI_NG_PTE_FLAGS	(PTE_ATTRINDX(MT_NORMAL) | PTE_TYPE_PAGE | \
				 PTE_AF | PTE_SHARED | PTE_UXN | PTE_WRITE)

	.pushsection ".idmap.text", "a"

	.macro	pte_to_phys, phys, pte
	and	\phys, \pte, #PTE_ADDR_LOW
#ifdef CONFIG_ARM64_PA_BITS_52
	and	\pte, \pte, #PTE_ADDR_HIGH
	orr	\phys, \phys, \pte, lsl #PTE_ADDR_HIGH_SHIFT
#endif
	.endm

	.macro	kpti_mk_tbl_ng, type, num_entries
	add	end_\type\()p, cur_\type\()p, #\num_entries * 8
.Ldo_\type:
	ldr	\type, [cur_\type\()p], #8	// Load the entry and advance
	tbz	\type, #0, .Lnext_\type		// Skip invalid and
	tbnz	\type, #11, .Lnext_\type	// non-global entries
	orr	\type, \type, #PTE_NG		// Same bit for blocks and pages
	str	\type, [cur_\type\()p, #-8]	// Update the entry
	.ifnc	\type, pte
	tbnz	\type, #1, .Lderef_\type
	.endif
.Lnext_\type:
	cmp	cur_\type\()p, end_\type\()p
	b.ne	.Ldo_\type
	.endm

	/*
	 * Dereference the current table entry and map it into the temporary
	 * fixmap slot associated with the current level.
	 */
	.macro	kpti_map_pgtbl, type, level
	str	xzr, [temp_pte, #8 * (\level + 2)]	// break before make
	dsb	nshst
	add	pte, temp_pte, #PAGE_SIZE * (\level + 2)
	lsr	pte, pte, #12
	tlbi	vaae1, pte
	dsb	nsh
	isb

	phys_to_pte pte, cur_\type\()p
	add	cur_\type\()p, temp_pte, #PAGE_SIZE * (\level + 2)
	orr	pte, pte, pte_flags
	str	pte, [temp_pte, #8 * (\level + 2)]
	dsb	nshst
	.endm

/*
 * void __kpti_install_ng_mappings(int cpu, int num_secondaries, phys_addr_t temp_pgd,
 *				   unsigned long temp_pte_va)
 *
 * Called exactly once from stop_machine context by each CPU found during boot.
 */
SYM_TYPED_FUNC_START(idmap_kpti_install_ng_mappings)
	cpu		.req	w0
	temp_pte	.req	x0
	num_cpus	.req	w1
	pte_flags	.req	x1
	temp_pgd_phys	.req	x2
	swapper_ttb	.req	x3
	flag_ptr	.req	x4
	cur_pgdp	.req	x5
	end_pgdp	.req	x6
	pgd		.req	x7
	cur_pudp	.req	x8
	end_pudp	.req	x9
	cur_pmdp	.req	x11
	end_pmdp	.req	x12
	cur_ptep	.req	x14
	end_ptep	.req	x15
	pte		.req	x16
	valid		.req	x17
	cur_p4dp	.req	x19
	end_p4dp	.req	x20

	mov	x5, x3				// preserve temp_pte arg
	mrs	swapper_ttb, ttbr1_el1
	adr_l	flag_ptr, idmap_kpti_bbml2_flag

	cbnz	cpu, __idmap_kpti_secondary

#if CONFIG_PGTABLE_LEVELS > 4
	stp	x29, x30, [sp, #-32]!
	mov	x29, sp
	stp	x19, x20, [sp, #16]
#endif

	/* We're the boot CPU. Wait for the others to catch up */
	sevl
1:	wfe
	ldaxr	w17, [flag_ptr]
	eor	w17, w17, num_cpus
	cbnz	w17, 1b

	/* Switch to the temporary page tables on this CPU only */
	__idmap_cpu_set_reserved_ttbr1 x8, x9
	offset_ttbr1 temp_pgd_phys, x8
	msr	ttbr1_el1, temp_pgd_phys
	isb

	mov	temp_pte, x5
	mov_q	pte_flags, KPTI_NG_PTE_FLAGS

	/* Everybody is enjoying the idmap, so we can rewrite swapper. */

#ifdef CONFIG_ARM64_LPA2
	/*
	 * If LPA2 support is configured, but 52-bit virtual addressing is not
	 * enabled at runtime, we will fall back to one level of paging less,
	 * and so we have to walk swapper_pg_dir as if we dereferenced its
	 * address from a PGD level entry, and terminate the PGD level loop
	 * right after.
	 */
	adrp	pgd, swapper_pg_dir	// walk &swapper_pg_dir at the next level
	mov	cur_pgdp, end_pgdp	// must be equal to terminate the PGD loop
alternative_if_not ARM64_HAS_VA52
	b	.Lderef_pgd		// skip to the next level
alternative_else_nop_endif
	/*
	 * LPA2 based 52-bit virtual addressing requires 52-bit physical
	 * addressing to be enabled as well. In this case, the shareability
	 * bits are repurposed as physical address bits, and should not be
	 * set in pte_flags.
	 */
	bic	pte_flags, pte_flags, #PTE_SHARED
#endif

	/* PGD */
	adrp		cur_pgdp, swapper_pg_dir
	kpti_map_pgtbl	pgd, -1
	kpti_mk_tbl_ng	pgd, PTRS_PER_PGD

	/* Ensure all the updated entries are visible to secondary CPUs */
	dsb	ishst

	/* We're done: fire up swapper_pg_dir again */
	__idmap_cpu_set_reserved_ttbr1 x8, x9
	msr	ttbr1_el1, swapper_ttb
	isb

	/* Set the flag to zero to indicate that we're all done */
	str	wzr, [flag_ptr]
#if CONFIG_PGTABLE_LEVELS > 4
	ldp	x19, x20, [sp, #16]
	ldp	x29, x30, [sp], #32
#endif
	ret

.Lderef_pgd:
	/* P4D */
	.if		CONFIG_PGTABLE_LEVELS > 4
	p4d		.req	x30
	pte_to_phys	cur_p4dp, pgd
	kpti_map_pgtbl	p4d, 0
	kpti_mk_tbl_ng	p4d, PTRS_PER_P4D
	b		.Lnext_pgd
	.else		/* CONFIG_PGTABLE_LEVELS <= 4 */
	p4d		.req	pgd
	.set		.Lnext_p4d, .Lnext_pgd
	.endif

.Lderef_p4d:
	/* PUD */
	.if		CONFIG_PGTABLE_LEVELS > 3
	pud		.req	x10
	pte_to_phys	cur_pudp, p4d
	kpti_map_pgtbl	pud, 1
	kpti_mk_tbl_ng	pud, PTRS_PER_PUD
	b		.Lnext_p4d
	.else		/* CONFIG_PGTABLE_LEVELS <= 3 */
	pud		.req	pgd
	.set		.Lnext_pud, .Lnext_pgd
	.endif

.Lderef_pud:
	/* PMD */
	.if		CONFIG_PGTABLE_LEVELS > 2
	pmd		.req	x13
	pte_to_phys	cur_pmdp, pud
	kpti_map_pgtbl	pmd, 2
	kpti_mk_tbl_ng	pmd, PTRS_PER_PMD
	b		.Lnext_pud
	.else		/* CONFIG_PGTABLE_LEVELS <= 2 */
	pmd		.req	pgd
	.set		.Lnext_pmd, .Lnext_pgd
	.endif

.Lderef_pmd:
	/* PTE */
	pte_to_phys	cur_ptep, pmd
	kpti_map_pgtbl	pte, 3
	kpti_mk_tbl_ng	pte, PTRS_PER_PTE
	b		.Lnext_pmd

	.unreq	cpu
	.unreq	temp_pte
	.unreq	num_cpus
	.unreq	pte_flags
	.unreq	temp_pgd_phys
	.unreq	cur_pgdp
	.unreq	end_pgdp
	.unreq	pgd
	.unreq	cur_pudp
	.unreq	end_pudp
	.unreq	pud
	.unreq	cur_pmdp
	.unreq	end_pmdp
	.unreq	pmd
	.unreq	cur_ptep
	.unreq	end_ptep
	.unreq	pte
	.unreq	valid
	.unreq	cur_p4dp
	.unreq	end_p4dp
	.unreq	p4d

	/* Secondary CPUs end up here */
__idmap_kpti_secondary:
	/* Uninstall swapper before surgery begins */
	__idmap_cpu_set_reserved_ttbr1 x16, x17
	b scondary_cpu_wait

	.unreq	swapper_ttb
	.unreq	flag_ptr
SYM_FUNC_END(idmap_kpti_install_ng_mappings)
	.popsection
#endif

	.pushsection ".idmap.text", "a"
SYM_TYPED_FUNC_START(wait_linear_map_split_to_ptes)
	/* Must be same registers as in idmap_kpti_install_ng_mappings */
	swapper_ttb	.req	x3
	flag_ptr	.req	x4

	mrs     swapper_ttb, ttbr1_el1
	adr_l   flag_ptr, idmap_kpti_bbml2_flag
	__idmap_cpu_set_reserved_ttbr1 x16, x17

scondary_cpu_wait:
	/* Increment the flag to let the boot CPU we're ready */
1:	ldxr	w16, [flag_ptr]
	add	w16, w16, #1
	stxr	w17, w16, [flag_ptr]
	cbnz	w17, 1b

	/* Wait for the boot CPU to finish messing around with swapper */
	sevl
1:	wfe
	ldxr	w16, [flag_ptr]
	cbnz	w16, 1b

	/* All done, act like nothing happened */
	msr	ttbr1_el1, swapper_ttb
	isb
	ret

	.unreq	swapper_ttb
	.unreq	flag_ptr
SYM_FUNC_END(wait_linear_map_split_to_ptes)
	.popsection

/*
 *	__cpu_setup
 *
 *	Initialise the processor for turning the MMU on.
 *
 * Output:
 *	Return in x0 the value of the SCTLR_EL1 register.
 *
 * 【函数整体说明】
 *
 * __cpu_setup 是 MMU 开启前最重要的处理器初始化函数，由 head.S 中的
 * __primary_switch（以及二级 CPU 的 secondary_startup）在开启 MMU 之前调用。
 *
 * 调用时机（启动链）：
 *   head.S: __primary_switch()
 *       bl __cpu_setup          // 配置处理器，返回 SCTLR_EL1 值到 x0
 *       bl __enable_mmu         // 使用 x0 写入 SCTLR_EL1，正式开启 MMU
 *
 * 函数完成的主要工作（按顺序）：
 *   1. 清空 TLB，防止历史映射干扰新的地址翻译
 *   2. 重置 cpacr_el1，禁止访问 FP/SIMD（内核初始化阶段不需要）
 *   3. 重置调试寄存器，禁止用户态访问硬件计数器
 *   4. 构造 MAIR_EL1 值（内存属性表）
 *   5. 构造 TCR_EL1 值（地址翻译控制，含地址空间大小、缓存策略、页粒度等）
 *   6. 根据 CPU 特性动态调整 TCR（物理地址位数、硬件访问标志、52 位 VA 等）
 *   7. 将 MAIR_EL1 和 TCR_EL1 写入系统寄存器
 *   8. 配置 PIE（Permission Indirection Extension）
 *   9. 返回 INIT_SCTLR_EL1_MMU_ON，供调用者写入 SCTLR_EL1 开启 MMU
 *
 * 函数放在 ".idmap.text" 段，该段同时映射到恒等映射（物理地址=虚拟地址）
 * 和内核高地址空间，确保在 MMU 开启前后代码都可执行。
 */
	.pushsection ".idmap.text", "a"
SYM_FUNC_START(__cpu_setup)
	/*
	 * 第一步：使所有本地 TLB 条目失效（TLB Invalidate）
	 *
	 * tlbi vmalle1：
	 *   tlbi = TLB Invalidation 指令
	 *   vmalle1 = Virtual Memory All EL1，使本 CPU 上 EL1（内核态）和
	 *             EL0（用户态）的所有 TLB 条目失效（仅影响本 CPU）
	 *
	 * 为什么需要先清空 TLB？
	 *   在 MMU 开启前，TLB 中可能残留来自 bootloader 或之前启动阶段的映射。
	 *   如果不清空，MMU 开启后可能使用错误的旧映射翻译地址，导致访问错误的
	 *   物理内存，造成数据损坏甚至系统崩溃。清空 TLB 确保 MMU 开启后使用的
	 *   是我们刚刚建立的页表进行翻译，没有任何历史干扰。
	 *
	 * dsb nsh（Data Synchronization Barrier, non-shareable）：
	 *   等待 tlbi 指令完成，确保 TLB 清空操作在后续 MMU 开启指令之前完成。
	 *   nsh = non-shareable domain，只需在本 CPU 范围内同步（不需要跨 CPU）。
	 */
	tlbi	vmalle1				// Invalidate local TLB
	dsb	nsh

	/*
	 * 第二步：重置 cpacr_el1，禁用浮点和 SIMD 访问
	 *
	 * CPACR_EL1（Coprocessor Access Control Register，协处理器访问控制寄存器）
	 * 控制 FP（浮点）、SIMD（高级 SIMD/NEON）和 SVE（可伸缩向量扩展）的访问权限。
	 *
	 * 写入 xzr（全零）意味着：
	 *   - FP/SIMD 访问（CPACR_EL1.FPEN 字段 = 0b00）：任何 EL 访问均产生异常
	 *   - SVE 访问（CPACR_EL1.ZEN 字段 = 0b00）：任何 EL 访问均产生异常
	 *
	 * 为什么要禁用？
	 *   内核启动早期不需要也不应该使用浮点/SIMD 寄存器。这些寄存器状态需要
	 *   按线程保存/恢复（上下文切换），在调度器和 FPU 上下文框架初始化完成
	 *   之前，任何 FP/SIMD 访问都是错误的。设为全零可以让误用时立即触发异常，
	 *   便于调试。正常情况下，内核在 fpsimd_thread_switch() 中按需启用。
	 */
	msr	cpacr_el1, xzr			// Reset cpacr_el1

	/*
	 * 第三步：配置 mdscr_el1（监控调试系统控制寄存器）
	 *
	 * MDSCR_EL1（Monitor Debug System Control Register）控制 ARM 硬件调试功能，
	 * 包括断点、观察点、单步执行和调试通信通道（DCC）等。
	 *
	 * MDSCR_EL1_TDCC（Trap DCC，位[12]）：
	 *   将该位设为 1 后，EL0（用户态）访问调试通信通道（DCC，即 DBGDTRxx 寄存器）
	 *   时会陷入（trap）到 EL1（内核态），防止用户程序直接读写调试通道。
	 *
	 * 注意：整个寄存器用 MDSCR_EL1_TDCC 的值（只设 TDCC 位）来初始化，
	 * 其他位清零意味着：禁用单步（SS=0）、禁用调试事件（MDE=0）等。
	 */
	mov	x1, MDSCR_EL1_TDCC		// Reset mdscr_el1 and disable
	msr	mdscr_el1, x1			// access to the DCC from EL0

	/*
	 * 第四步：重置用户态性能计数器访问控制
	 *
	 * reset_pmuserenr_el0：重置 PMUSERENR_EL0（性能监视器用户态使能寄存器）
	 *   PMU = Performance Monitoring Unit（硬件性能计数器单元）
	 *   将此寄存器清零后，EL0 访问 PMU 计数器和控制寄存器时会陷入 EL1，
	 *   防止用户程序在未授权的情况下读取硬件性能数据（可能泄露信息）。
	 *   内核会在用户空间 perf 工具请求时按需授权。
	 *
	 * reset_amuserenr_el0：重置 AMUSERENR_EL0（活动监视器用户态使能寄存器）
	 *   AMU = Activity Monitors Unit（活动监视器单元，ARMv8.4 新增）
	 *   类似 PMU，但专注于 CPU 活动统计（如指令周期、内存访问等）。
	 *   清零后同样禁止 EL0 直接访问，需内核明确授权。
	 */
	reset_pmuserenr_el0 x1			// Disable PMU access from EL0
	reset_amuserenr_el0 x1			// Disable AMU access from EL0

	/*
	 * 第五步：构造 MAIR_EL1 和 TCR_EL1 的默认值
	 *
	 * 这里使用寄存器别名（.req 伪指令）提高可读性：
	 *   mair（x17）：暂存 MAIR_EL1 的目标值
	 *   tcr（x16）：暂存 TCR_EL1 的目标值
	 *   tcr2（x15）：暂存 TCR2_EL1 的目标值（ARMv8.9 扩展）
	 *
	 * Default values for VMSA control registers. These will be adjusted
	 * below depending on detected CPU features.
	 */
	mair	.req	x17
	tcr	.req	x16
	tcr2	.req	x15

	/*
	 * 加载 MAIR_EL1 的默认值（内存属性表，见文件头 MAIR_EL1_SET 注释）
	 * mov_q 是加载 64 位立即数的宏（分多条指令执行，因为 ARM64 单条指令
	 * 最多只能编码 16 位立即数）
	 */
	mov_q	mair, MAIR_EL1_SET

	/*
	 * 构造 TCR_EL1 的完整初始值
	 *
	 * TCR_EL1（Translation Control Register，地址翻译控制寄存器）是控制
	 * MMU 行为最重要的寄存器，每个字段的含义如下：
	 *
	 * TCR_T0SZ(IDMAP_VA_BITS)：
	 *   T0SZ（Translation zero size）= 64 - IDMAP_VA_BITS
	 *   控制 TTBR0（低地址空间）能翻译的虚拟地址范围大小。
	 *   T0SZ=N 表示 TTBR0 管理高 (64-N) 位为 0 的地址（即低地址空间）。
	 *   IDMAP_VA_BITS 是恒等映射（identity map）需要覆盖的物理地址位数，
	 *   MMU 开启初期 TTBR0 指向 idmap 页表（物理地址=虚拟地址的映射），
	 *   该映射只需覆盖内核代码所在的物理地址范围，不需要完整 48 位地址空间。
	 *
	 * TCR_T1SZ(VA_BITS_MIN)：
	 *   T1SZ = 64 - VA_BITS_MIN
	 *   控制 TTBR1（高地址空间，内核空间）能翻译的虚拟地址范围大小。
	 *   T1SZ=N 表示 TTBR1 管理高 (64-N) 位为 1 的地址（即高地址内核空间）。
	 *   VA_BITS_MIN 是内核虚拟地址位数的最小值，用于确保所有配置下
	 *   内核地址空间都能被正确翻译。52 位 VA 时会在下面动态更新。
	 *
	 * TCR_CACHE_FLAGS（TCR_IRGN_WBWA | TCR_ORGN_WBWA）：
	 *   页表遍历的内部/外部缓存策略，均设为 Write-Back Write-Allocate（最优性能）
	 *
	 * TCR_SHARED：
	 *   页表遍历的共享域 = Inner Shareable，确保多核 SMP 系统缓存一致性
	 *
	 * TCR_TG_FLAGS：
	 *   TTBR0 和 TTBR1 的页粒度（4KB/16KB/64KB），由编译配置决定
	 *
	 * TCR_KASLR_FLAGS（TCR_EL1_NFD1 或 0）：
	 *   KASLR 支持：NFD1 使 TTBR1 的翻译错误延迟触发，允许地址探测
	 *
	 * TCR_EL1_AS（ASID Size，位[36]）：
	 *   0 = 8 位 ASID（256 个地址空间 ID）
	 *   1 = 16 位 ASID（65536 个地址空间 ID）← 此处选择 16 位
	 *   ASID（Address Space ID）区分不同进程的 TLB 条目，避免进程切换时
	 *   清空整个 TLB。16 位 ASID 支持更多进程并发运行时保留各自 TLB 条目。
	 *
	 * TCR_EL1_TBI0（Top Byte Ignore for TTBR0，位[37]）：
	 *   忽略 TTBR0 地址（用户态指针）的最高字节进行地址翻译。
	 *   这是 ARM64 的 Pointer Authentication 和内存标签（MTE）等特性的基础，
	 *   允许用户态指针在高字节携带标签/元数据而不影响内存访问。
	 *
	 * TCR_EL1_A1（ASID select from TTBR1，位[22]）：
	 *   1 = 使用 TTBR1 中的 ASID 字段作为当前 ASID。
	 *   内核将 ASID 存放在 TTBR1（内核页表基地址寄存器）的高位字段，
	 *   此位告诉 MMU 从 TTBR1 而不是 TTBR0 读取当前进程的 ASID。
	 *
	 * TCR_KASAN_SW_FLAGS / TCR_MTE_FLAGS：
	 *   根据配置启用 TBI1/TBID1/TCMA1，支持 KASAN 软件标签或 MTE 硬件标签
	 */
	mov_q	tcr, TCR_T0SZ(IDMAP_VA_BITS) | TCR_T1SZ(VA_BITS_MIN) | TCR_CACHE_FLAGS | \
		     TCR_SHARED | TCR_TG_FLAGS | TCR_KASLR_FLAGS | TCR_EL1_AS | \
		     TCR_EL1_TBI0 | TCR_EL1_A1 | TCR_KASAN_SW_FLAGS | TCR_MTE_FLAGS
	mov	tcr2, xzr			// TCR2_EL1 初始值为 0（后续按需设置）

	/*
	 * tcr_clear_errata_bits：清除 TCR_EL1 中某些 CPU 勘误（errata）需要
	 * 清零的位。不同 CPU 型号可能存在特定硬件 bug，需要通过特殊配置绕过。
	 * 这是内核 CPU 勘误处理机制（workaround）的一部分。
	 */
	tcr_clear_errata_bits tcr, x9, x5

	/*
	 * 如果配置了 52 位虚拟地址（CONFIG_ARM64_VA_BITS_52），
	 * 且运行时检测到 CPU 支持 52 位 VA（ARM64_HAS_VA52），
	 * 则将 T1SZ 更新为 64 - 52 = 12。
	 *
	 * 默认编译时用 VA_BITS_MIN（通常 48 位），运行时若 CPU 支持 52 位 VA，
	 * 则动态扩大内核虚拟地址空间到 52 位（2^52 = 4PB 地址范围）。
	 *
	 * CONFIG_ARM64_LPA2（Large Physical Address 2）：
	 *   LPA2 是 ARMv8.7 引入的扩展，支持 52 位物理地址和 52 位虚拟地址。
	 *   TCR_EL1_DS（位[32]）：Data Size，使能 LPA2 特性，
	 *   同时 SH0/SH1 字段被重新定义为物理地址扩展位。
	 */
#ifdef CONFIG_ARM64_VA_BITS_52
	mov		x9, #64 - VA_BITS	// 计算 T1SZ = 64 - 52 = 12
alternative_if ARM64_HAS_VA52
	tcr_set_t1sz	tcr, x9			// 将 T1SZ 字段更新为 52 位 VA 对应的值
#ifdef CONFIG_ARM64_LPA2
	orr		tcr, tcr, #TCR_EL1_DS	// 使能 LPA2（52 位物理地址扩展）
#endif
alternative_else_nop_endif
#endif

	/*
	 * 第六步：根据 CPU 支持的物理地址位数设置 TCR_EL1.IPS
	 *
	 * IPS（Intermediate Physical address Size，中间物理地址大小，位[34:32]）：
	 * 告诉 MMU 本系统支持的最大物理地址位数，MMU 会拒绝超出此范围的物理地址。
	 *
	 * tcr_compute_pa_size 宏：
	 *   读取 ID_AA64MMFR0_EL1.PARange 字段，该字段由 CPU 硬件报告支持的
	 *   最大物理地址位数（如 0b000=32位, 0b001=36位, 0b010=40位,
	 *   0b011=42位, 0b100=44位, 0b101=48位, 0b110=52位）。
	 *   然后将对应的编码写入 TCR_EL1 的 IPS 字段（TCR_EL1_IPS_SHIFT 指定位置）。
	 *
	 * 为什么不直接硬编码？
	 *   不同硬件支持的物理地址范围不同（从 32 位到 52 位），
	 *   运行时动态查询并配置可以适配各种 ARM64 硬件，同时让 MMU 能正确
	 *   检测和拒绝超出范围的物理地址，增强安全性。
	 *
	 * Set the IPS bits in TCR_EL1.
	 */
	tcr_compute_pa_size tcr, #TCR_EL1_IPS_SHIFT, x5, x6

	/*
	 * 第七步：启用硬件自动访问标志更新（Hardware Access Flag / Dirty Bit Management）
	 *
	 * ARM64 页表项中有两个重要标志位：
	 *   AF（Access Flag，访问标志）：页面首次被访问时 MMU 自动置 1。
	 *     软件通过检查 AF 可知某页是否最近被访问（用于页面回收算法）。
	 *   DBM（Dirty Bit Management）：页面被写入时 MMU 自动清除写保护标志。
	 *
	 * 传统方式（软件管理）：
	 *   AF=0 时触发异常，由缺页异常处理程序置 AF=1，性能较差。
	 *
	 * 硬件自动更新（CONFIG_ARM64_HW_AFDBM）：
	 *   如果 CPU 支持（由 ID_AA64MMFR1_EL1.HAFDBS 字段指示），
	 *   可以设置 TCR_EL1.HA（Hardware Access flag update）= 1，
	 *   让 MMU 在发生访问时自动置 AF 位，无需软件干预，显著减少缺页异常数量。
	 *
	 * ID_AA64MMFR1_EL1.HAFDBS 字段值含义：
	 *   0b0000 = 不支持硬件 AF/DBM
	 *   0b0001 = 支持硬件 AF 更新（HA），但不支持 DBM
	 *   0b0010 = 支持 HA 和硬件 DBM（HD）
	 *   >= HAFDBS_HAFT = 支持 HAFT（Hierarchical AF for Table entries）
	 *
	 * 注意：硬件 DBM（HD bit）此处暂不启用，而是后续通过 capabilities
	 * 机制（cpu_enable_hw_dbm）在确认所有 CPU 都支持后统一开启。
	 *
	 * Enable hardware update of the Access Flags bit.
	 * Hardware dirty bit management is enabled later,
	 * via capabilities.
	 */
#ifdef CONFIG_ARM64_HW_AFDBM
	mrs	x9, ID_AA64MMFR1_EL1		// 读取 AArch64 内存模型特性寄存器 1
	// ubfx：无符号位域提取（Unsigned Bit Field eXtract）
	// ubfx xd, xs, lsb, width：提取 xs 的 [lsb+width-1:lsb] 位到 xd
	ubfx	x9, x9, ID_AA64MMFR1_EL1_HAFDBS_SHIFT, #4  // 提取 HAFDBS 字段（4位）
	cbz	x9, 1f				// 若 HAFDBS=0（不支持），跳过
	orr	tcr, tcr, #TCR_EL1_HA		// 支持则设置 HA 位：启用硬件 AF 自动更新

	/*
	 * CONFIG_ARM64_HAFT：分层访问标志（Hierarchical Access Flags for Tables）
	 *
	 * HAFT（ARMv8.9 引入）是 HA 的增强版，不仅自动更新叶子页表项的 AF，
	 * 还可以自动更新中间层页表（块描述符、表描述符）的 AF 位。
	 * 这进一步减少了软件需要处理的缺页异常。
	 *
	 * 需要 HAFDBS >= HAFDBS_HAFT（即硬件明确报告支持 HAFT 功能）。
	 * 启用方式：设置 TCR2_EL1.HAFT 位。
	 */
#ifdef CONFIG_ARM64_HAFT
	cmp	x9, ID_AA64MMFR1_EL1_HAFDBS_HAFT  // 检查是否支持 HAFT 功能
	b.lt	1f				// 若低于 HAFT 门槛，跳过 HAFT 配置
	orr	tcr2, tcr2, TCR2_EL1_HAFT	// 在 TCR2 中启用 HAFT
#endif /* CONFIG_ARM64_HAFT */
1:
#endif	/* CONFIG_ARM64_HW_AFDBM */

	/*
	 * 第八步：将 MAIR_EL1 和 TCR_EL1 写入系统寄存器
	 *
	 * 此时 mair（x17）和 tcr（x16）已包含完整配置值，
	 * msr 指令将其写入对应的系统寄存器，配置生效。
	 *
	 * 注意：这两条指令执行后 MMU 仍未开启（SCTLR_EL1.M 仍为 0），
	 * 但寄存器已准备好，等待 __enable_mmu 开启 MMU 时立即生效。
	 */
	msr	mair_el1, mair			// 写入内存属性索引寄存器
	msr	tcr_el1, tcr			// 写入地址翻译控制寄存器

	/*
	 * 第九步：配置 PIE（Permission Indirection Extension，权限间接扩展）
	 *
	 * PIE 是 ARMv8.9 / ARMv9.4 引入的新特性，改变了页表权限的表达方式：
	 *
	 * 传统方式（直接权限）：
	 *   页表项中的 AP（Access Permission）字段直接编码 R/W/X 权限，
	 *   权限组合较为固定（只有少数几种组合）。
	 *
	 * PIE 方式（间接权限）：
	 *   页表项中存储的不再是权限，而是一个 4 位的权限"索引"（overlay index），
	 *   该索引指向 PIR_EL1（Permission Indirection Register）中的一个 4 位槽，
	 *   槽中才是实际的 R/W/X 权限位。
	 *   优点：通过修改一个 PIR 寄存器可以批量改变所有使用该索引的页面权限，
	 *   而无需修改页表（避免 TLB shootdown），性能更好。
	 *   也为 PKRU（保护键）风格的内存保护机制提供了基础。
	 *
	 * 检测 PIE 支持：
	 *   ID_AA64MMFR3_EL1.S1PIE 字段非零表示支持 EL1 第一阶段翻译的 PIE。
	 *
	 * PIRE0_EL1：EL0（用户态）的权限间接寄存器，定义用户态页面权限表
	 * PIR_EL1：EL1（内核态）的权限间接寄存器，定义内核态页面权限表
	 * TCR2_EL1.PIE（位[1]）：使能权限间接模式
	 */
	mrs_s	x1, SYS_ID_AA64MMFR3_EL1	// 读取 AArch64 内存模型特性寄存器 3
	// 提取 S1PIE 字段（Stage 1 Permission Indirection Extension 支持位）
	ubfx	x1, x1, #ID_AA64MMFR3_EL1_S1PIE_SHIFT, #4
	cbz	x1, .Lskip_indirection		// 若不支持 PIE，跳过配置

	// 支持 PIE，配置权限间接寄存器
	mov_q	x0, PIE_E0_ASM			// 加载 EL0 权限表的初始值
	msr	REG_PIRE0_EL1, x0		// 写入 EL0 权限间接寄存器（用户态权限表）
	mov_q	x0, PIE_E1_ASM			// 加载 EL1 权限表的初始值
	msr	REG_PIR_EL1, x0			// 写入 EL1 权限间接寄存器（内核态权限表）

	orr	tcr2, tcr2, TCR2_EL1_PIE	// 在 TCR2 中设置 PIE 使能位

.Lskip_indirection:

	/*
	 * 第十步：如果支持 TCR2_EL1，将 TCR2 值写入系统寄存器
	 *
	 * TCR2_EL1（Translation Control Register 2）是 ARMv8.9 引入的扩展寄存器，
	 * 包含 HAFT、PIE 等新特性的控制位。
	 *
	 * 检测方式：ID_AA64MMFR3_EL1.TCRX 字段非零表示支持 TCR2_EL1。
	 * 只有在支持时才写入，否则跳过（写不支持的寄存器会触发异常）。
	 */
	mrs_s	x1, SYS_ID_AA64MMFR3_EL1	// 再次读取特性寄存器（x1 可能被上面覆盖）
	ubfx	x1, x1, #ID_AA64MMFR3_EL1_TCRX_SHIFT, #4  // 提取 TCRX 字段
	cbz	x1, 1f				// 若不支持 TCR2，跳到 1f
	msr	REG_TCR2_EL1, tcr2		// 写入扩展翻译控制寄存器 2
1:

	/*
	 * 第十一步：准备 SCTLR_EL1 返回值，供调用者开启 MMU
	 *
	 * SCTLR_EL1（System Control Register，系统控制寄存器）包含许多系统级开关，
	 * 其中最关键的是：
	 *   M（位[0]）  = 1：使能 EL1 和 EL0 的 MMU
	 *   C（位[2]）  = 1：使能数据缓存（D-cache）
	 *   I（位[12]） = 1：使能指令缓存（I-cache）
	 *
	 * INIT_SCTLR_EL1_MMU_ON（定义于 asm/sysreg.h）是内核计算出的 SCTLR_EL1 值，
	 * 包含上述核心位以及其他安全/功能位（如 SPAN、nTWE、nTWI 等）。
	 *
	 * 为什么这里只返回值而不直接写入？
	 *   写入 SCTLR_EL1 开启 MMU 是个"点火"动作，之后程序计数器（PC）指向的
	 *   地址会立即被 MMU 翻译。这个操作需要在精心构造的上下文中执行
	 *   （代码必须同时在物理地址和虚拟地址都有效的映射下运行），
	 *   因此由 head.S 中的专用函数 __enable_mmu 在确保恒等映射正确后统一完成。
	 *
	 * Prepare SCTLR
	 */
	mov_q	x0, INIT_SCTLR_EL1_MMU_ON	// 将 SCTLR_EL1 目标值加载到 x0（返回值寄存器）
	ret					// return to head.S，head.S 用 x0 值写 SCTLR_EL1

	// 取消寄存器别名（汇编器作用域结束）
	.unreq	mair
	.unreq	tcr
	.unreq	tcr2
SYM_FUNC_END(__cpu_setup)
