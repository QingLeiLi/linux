// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMP initialisation and IPI support
 * Based on arch/arm/kernel/smp.c
 *
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/arm_sdei.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/spinlock.h>
#include <linux/sched/mm.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/task_stack.h>
#include <linux/interrupt.h>
#include <linux/cache.h>
#include <linux/profile.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <linux/seq_file.h>
#include <linux/irq.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/percpu.h>
#include <linux/clockchips.h>
#include <linux/completion.h>
#include <linux/of.h>
#include <linux/irq_work.h>
#include <linux/kernel_stat.h>
#include <linux/kexec.h>
#include <linux/kgdb.h>
#include <linux/kvm_host.h>
#include <linux/nmi.h>

#include <asm/alternative.h>
#include <asm/atomic.h>
#include <asm/cacheflush.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/cpu_ops.h>
#include <asm/daifflags.h>
#include <asm/kvm_mmu.h>
#include <asm/mmu_context.h>
#include <asm/numa.h>
#include <asm/processor.h>
#include <asm/smp_plat.h>
#include <asm/sections.h>
#include <asm/tlbflush.h>
#include <asm/ptrace.h>
#include <asm/virt.h>

#include <trace/events/ipi.h>

/*
 * as from 2.5, kernels no longer have an init_tasks structure
 * so we need some other way of telling a new secondary core
 * where to place its SVC stack
 */
struct secondary_data secondary_data;
/* Number of CPUs which aren't online, but looping in kernel text. */
static int cpus_stuck_in_kernel;

static int ipi_irq_base __ro_after_init;
static int nr_ipi __ro_after_init = NR_IPI;

struct ipi_descs {
	struct irq_desc *descs[MAX_IPI];
};

static DEFINE_PER_CPU_READ_MOSTLY(struct ipi_descs, pcpu_ipi_desc);

#define get_ipi_desc(__cpu, __ipi) (per_cpu_ptr(&pcpu_ipi_desc, __cpu)->descs[__ipi])

static bool percpu_ipi_descs __ro_after_init;

static bool crash_stop;

static void ipi_setup(int cpu);

#ifdef CONFIG_HOTPLUG_CPU
static void ipi_teardown(int cpu);
static int op_cpu_kill(unsigned int cpu);
#else
static inline int op_cpu_kill(unsigned int cpu)
{
	return -ENOSYS;
}
#endif


/*
 * Boot a secondary CPU, and assign it the specified idle task.
 * This also gives us the initial stack to use for this CPU.
 */
static int boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	if (ops->cpu_boot)
		return ops->cpu_boot(cpu);

	return -EOPNOTSUPP;
}

static DECLARE_COMPLETION(cpu_running);

int __cpu_up(unsigned int cpu, struct task_struct *idle)
{
	int ret;
	long status;

	/*
	 * We need to tell the secondary core where to find its stack and the
	 * page tables.
	 */
	secondary_data.task = idle;
	update_cpu_boot_status(CPU_MMU_OFF);

	/* Now bring the CPU into our world */
	ret = boot_secondary(cpu, idle);
	if (ret) {
		if (ret != -EPERM)
			pr_err("CPU%u: failed to boot: %d\n", cpu, ret);
		return ret;
	}

	/*
	 * CPU was successfully started, wait for it to come online or
	 * time out.
	 */
	wait_for_completion_timeout(&cpu_running,
				    msecs_to_jiffies(5000));
	if (cpu_online(cpu))
		return 0;

	pr_crit("CPU%u: failed to come online\n", cpu);
	secondary_data.task = NULL;
	status = READ_ONCE(secondary_data.status);
	if (status == CPU_MMU_OFF)
		status = READ_ONCE(__early_cpu_boot_status);

	switch (status & CPU_BOOT_STATUS_MASK) {
	default:
		pr_err("CPU%u: failed in unknown state : 0x%lx\n",
		       cpu, status);
		cpus_stuck_in_kernel++;
		break;
	case CPU_KILL_ME:
		if (!op_cpu_kill(cpu)) {
			pr_crit("CPU%u: died during early boot\n", cpu);
			break;
		}
		pr_crit("CPU%u: may not have shut down cleanly\n", cpu);
		fallthrough;
	case CPU_STUCK_IN_KERNEL:
		pr_crit("CPU%u: is stuck in kernel\n", cpu);
		if (status & CPU_STUCK_REASON_52_BIT_VA)
			pr_crit("CPU%u: does not support 52-bit VAs\n", cpu);
		if (status & CPU_STUCK_REASON_NO_GRAN) {
			pr_crit("CPU%u: does not support %luK granule\n",
				cpu, PAGE_SIZE / SZ_1K);
		}
		cpus_stuck_in_kernel++;
		break;
	case CPU_PANIC_KERNEL:
		panic("CPU%u detected unsupported configuration\n", cpu);
	}

	return -EIO;
}

/*
 * init_gic_priority_masking - 为当前 CPU 初始化 GIC PMR 优先级屏蔽模式
 *
 * 在启用 CONFIG_ARM64_PSEUDO_NMI 时，内核用 GIC PMR 寄存器（ICC_PMR_EL1）
 * 代替 DAIF.I 位来控制中断开关，从而保留高优先级 FIQ 作为伪 NMI 穿透。
 * 本函数对当前 CPU 完成该模式的硬件初始化，在 boot CPU 和每个 secondary
 * CPU 启动时各调用一次。
 *
 * boot CPU 路径：smp_prepare_boot_cpu() → init_gic_priority_masking()
 * secondary CPU 路径：secondary_start_kernel() → init_gic_priority_masking()
 */
static void init_gic_priority_masking(void)
{
	u32 cpuflags;

	/* gic_enable_sre()：使能 GICv3 系统寄存器接口（SRE，System Register Enable）。
	 * GICv3 的 CPU 接口可以通过内存映射或系统寄存器两种方式访问，
	 * SRE 必须开启后才能用 MSR/MRS 指令直接读写 ICC_PMR_EL1 等寄存器。
	 * 返回 false 说明 GIC 不支持 SRE，PMR 路径无法使用，发出警告并退出。 */
	if (WARN_ON(!gic_enable_sre()))
		return;

	/* 读出当前 DAIF 值，断言 IRQ（PSR_I_BIT）和 FIQ（PSR_F_BIT）均已屏蔽。
	 * 初始化 PMR 时必须处于关中断状态，防止在 PMR 尚未就绪时就有中断投递进来。
	 * 若这两个断言触发，说明调用时序有误。 */
	cpuflags = read_sysreg(daif);
	WARN_ON(!(cpuflags & PSR_I_BIT));
	WARN_ON(!(cpuflags & PSR_F_BIT));

	/* 将 ICC_PMR_EL1 初始化为 GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET：
	 *   GIC_PRIO_IRQON：将 PMR 阈值设置为"开中断"状态的值，
	 *                   表示 GIC 侧允许投递普通中断；
	 *   GIC_PRIO_PSR_I_SET：同时设置一个特殊标志位，告知内核此时
	 *                        DAIF.I 仍然置位（中断实际被 DAIF 屏蔽），
	 *                        保证 PMR 和 DAIF 的状态视图一致。
	 * 之后内核通过写 PMR（而非 daifset/daifclr）来开关中断，
	 * 中断是否实际到达 CPU 由 PMR 阈值决定，DAIF.F 保持清零以允许
	 * 高优先级 FIQ（伪 NMI）穿透。 */
	gic_write_pmr(GIC_PRIO_IRQON | GIC_PRIO_PSR_I_SET);
}

/*
 * This is the secondary CPU boot entry.  We're using this CPUs
 * idle thread stack, but a set of temporary page tables.
 */
asmlinkage notrace void secondary_start_kernel(void)
{
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	struct mm_struct *mm = &init_mm;
	const struct cpu_operations *ops;
	unsigned int cpu = smp_processor_id();

	/*
	 * All kernel threads share the same mm context; grab a
	 * reference and switch to it.
	 */
	mmgrab(mm);
	current->active_mm = mm;

	/*
	 * TTBR0 is only used for the identity mapping at this stage. Make it
	 * point to zero page to avoid speculatively fetching new entries.
	 */
	cpu_uninstall_idmap();

	if (system_uses_irq_prio_masking())
		init_gic_priority_masking();

	rcutree_report_cpu_starting(cpu);
	trace_hardirqs_off();

	/*
	 * If the system has established the capabilities, make sure
	 * this CPU ticks all of those. If it doesn't, the CPU will
	 * fail to come online.
	 */
	check_local_cpu_capabilities();

	ops = get_cpu_ops(cpu);
	if (ops->cpu_postboot)
		ops->cpu_postboot();

	/*
	 * Log the CPU info before it is marked online and might get read.
	 */
	cpuinfo_store_cpu();
	store_cpu_topology(cpu);

	/*
	 * Enable GIC and timers.
	 */
	notify_cpu_starting(cpu);

	ipi_setup(cpu);

	numa_add_cpu(cpu);

	/*
	 * OK, now it's safe to let the boot CPU continue.  Wait for
	 * the CPU migration code to notice that the CPU is online
	 * before we continue.
	 */
	pr_info("CPU%u: Booted secondary processor 0x%010lx [0x%08x]\n",
					 cpu, (unsigned long)mpidr,
					 read_cpuid_id());
	update_cpu_boot_status(CPU_BOOT_SUCCESS);
	set_cpu_online(cpu, true);
	complete(&cpu_running);

	/*
	 * Secondary CPUs enter the kernel with all DAIF exceptions masked.
	 *
	 * As with setup_arch() we must unmask Debug and SError exceptions, and
	 * as the root irqchip has already been detected and initialized we can
	 * unmask IRQ and FIQ at the same time.
	 */
	local_daif_restore(DAIF_PROCCTX);

	/*
	 * OK, it's off to the idle thread for us
	 */
	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}

#ifdef CONFIG_HOTPLUG_CPU
static int op_cpu_disable(unsigned int cpu)
{
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	/*
	 * If we don't have a cpu_die method, abort before we reach the point
	 * of no return. CPU0 may not have an cpu_ops, so test for it.
	 */
	if (!ops || !ops->cpu_die)
		return -EOPNOTSUPP;

	/*
	 * We may need to abort a hot unplug for some other mechanism-specific
	 * reason.
	 */
	if (ops->cpu_disable)
		return ops->cpu_disable(cpu);

	return 0;
}

/*
 * __cpu_disable runs on the processor to be shutdown.
 */
int __cpu_disable(void)
{
	unsigned int cpu = smp_processor_id();
	int ret;

	ret = op_cpu_disable(cpu);
	if (ret)
		return ret;

	remove_cpu_topology(cpu);
	numa_remove_cpu(cpu);

	/*
	 * Take this CPU offline.  Once we clear this, we can't return,
	 * and we must not schedule until we're ready to give up the cpu.
	 */
	set_cpu_online(cpu, false);
	ipi_teardown(cpu);

	/*
	 * OK - migrate IRQs away from this CPU
	 */
	irq_migrate_all_off_this_cpu();

	return 0;
}

static int op_cpu_kill(unsigned int cpu)
{
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	/*
	 * If we have no means of synchronising with the dying CPU, then assume
	 * that it is really dead. We can only wait for an arbitrary length of
	 * time and hope that it's dead, so let's skip the wait and just hope.
	 */
	if (!ops->cpu_kill)
		return 0;

	return ops->cpu_kill(cpu);
}

/*
 * Called on the thread which is asking for a CPU to be shutdown after the
 * shutdown completed.
 */
void arch_cpuhp_cleanup_dead_cpu(unsigned int cpu)
{
	int err;

	pr_debug("CPU%u: shutdown\n", cpu);

	/*
	 * Now that the dying CPU is beyond the point of no return w.r.t.
	 * in-kernel synchronisation, try to get the firmware to help us to
	 * verify that it has really left the kernel before we consider
	 * clobbering anything it might still be using.
	 */
	err = op_cpu_kill(cpu);
	if (err)
		pr_warn("CPU%d may not have shut down cleanly: %d\n", cpu, err);
}

/*
 * Called from the idle thread for the CPU which has been shutdown.
 *
 */
void __noreturn cpu_die(void)
{
	unsigned int cpu = smp_processor_id();
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	idle_task_exit();

	local_daif_mask();

	/* Tell cpuhp_bp_sync_dead() that this CPU is now safe to dispose of */
	cpuhp_ap_report_dead();

	/*
	 * Actually shutdown the CPU. This must never fail. The specific hotplug
	 * mechanism must perform all required cache maintenance to ensure that
	 * no dirty lines are lost in the process of shutting down the CPU.
	 */
	ops->cpu_die(cpu);

	BUG();
}
#endif

static void __cpu_try_die(int cpu)
{
#ifdef CONFIG_HOTPLUG_CPU
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	if (ops && ops->cpu_die)
		ops->cpu_die(cpu);
#endif
}

/*
 * Kill the calling secondary CPU, early in bringup before it is turned
 * online.
 */
void __noreturn cpu_die_early(void)
{
	int cpu = smp_processor_id();

	pr_crit("CPU%d: will not boot\n", cpu);

	/* Mark this CPU absent */
	set_cpu_present(cpu, 0);
	rcutree_report_cpu_dead();

	if (IS_ENABLED(CONFIG_HOTPLUG_CPU)) {
		update_cpu_boot_status(CPU_KILL_ME);
		__cpu_try_die(cpu);
	}

	update_cpu_boot_status(CPU_STUCK_IN_KERNEL);

	cpu_park_loop();
}

static void __init hyp_mode_check(void)
{
	if (is_hyp_mode_available())
		pr_info("CPU: All CPU(s) started at EL2\n");
	else if (is_hyp_mode_mismatched())
		WARN_TAINT(1, TAINT_CPU_OUT_OF_SPEC,
			   "CPU: CPUs started in inconsistent modes");
	else
		pr_info("CPU: All CPU(s) started at EL1\n");
	if (IS_ENABLED(CONFIG_KVM) && !is_kernel_in_hyp_mode()) {
		kvm_compute_layout();
		kvm_apply_hyp_relocations();
	}
}

void __init smp_cpus_done(unsigned int max_cpus)
{
	pr_info("SMP: Total of %d processors activated.\n", num_online_cpus());
	hyp_mode_check();
	setup_system_features();
	setup_user_features();
	mark_linear_text_alias_ro();
}

/*
 * smp_prepare_boot_cpu - 完成 boot CPU（CPU 0）的 SMP 前置准备工作
 *
 * 调用时机：setup_per_cpu_areas() 之后，smp_init()（唤醒其他 CPU）之前。
 * 此时只有 boot CPU 在运行，其他 CPU 尚未启动。
 *
 * 本函数完成四项工作：
 *   1. 切换到运行时 per-cpu 区域
 *   2. 读取并存储 boot CPU 的硬件信息
 *   3. 检测 boot CPU 的硬件 capability
 *   4. 初始化中断屏蔽和内存安全检测机制
 */
void __init smp_prepare_boot_cpu(void)
{
	/*
	 * The runtime per-cpu areas have been allocated by
	 * setup_per_cpu_areas(), and CPU0's boot time per-cpu area will be
	 * freed shortly, so we must move over to the runtime per-cpu area.
	 *
	 * per-cpu 变量的访问依赖一个存储在寄存器中的"当前 CPU 的 per-cpu 区域基地址
	 * 偏移"。arm64 用 tpidr_el1（或虚拟化时 tpidr_el2）寄存器存放这个偏移。
	 *
	 * 内核启动极早期使用的是编译进镜像的"boot time per-cpu 区域"（静态分配），
	 * setup_per_cpu_areas() 已为每个 CPU 分配了正式的运行时区域并复制了初始值。
	 * 此处调用 set_my_cpu_offset() 将 tpidr_el1 更新为 boot CPU 的运行时区域
	 * 偏移，使后续所有 per-cpu 访问都指向运行时区域。
	 * 旧的 boot time 区域稍后会被释放，若不切换则访问已释放内存。
	 */
	set_my_cpu_offset(per_cpu_offset(smp_processor_id()));

	/* 读取 boot CPU 的 CPU ID 寄存器（MIDR、REVIDR、ID_AA64* 等系统寄存器），
	 * 存入 per_cpu(cpu_data, 0) 和全局 boot_cpu_data，
	 * 并调用 init_cpu_features() 根据这些寄存器值初始化 CPU feature 框架的
	 * 基准线（后续 secondary CPU 的特性只能是 boot CPU 特性的子集）。 */
	/*
		features（特性）CPU 硬件有什么

		CPU 硬件支持的功能，来自 CPU ID 寄存器（ID_AA64ISAR*、ID_AA64MMFR*、MIDR 等），直接反映硬件能力：

		CPU 的 features（部分）：
			MTE（内存标签扩展）
			SVE（可扩展向量扩展）
			BTI（分支目标识别）
			PAC（指针认证）
			LSE（大系统扩展，原子指令）
			...

		读出来是原始的寄存器字段值，描述"这颗 CPU 有什么硬件"。
		cpuinfo_store_boot_cpu() 读取并存储的就是这些原始 feature 寄存器值。
	*/
	cpuinfo_store_boot_cpu();

	/* 在 boot CPU 的硬件信息已存储的基础上：
	 *   1. init_cpucap_indirect_list()：初始化 CPU capability 间接指针数组，
	 *      为后续按 capability ID 快速查找做准备。
	 *   2. detect_system_supports_pseudo_nmi()：检测 GIC 是否支持优先级屏蔽，
	 *      决定是否可以启用伪 NMI，必须在 setup_boot_cpu_capabilities() 之前
	 *      调用，因为后者依赖此检测结果。
	 *   3. setup_boot_cpu_capabilities()：遍历所有 capability 定义，
	 *      检测 boot CPU 支持哪些特性，写入 system_cpucaps 位图，
	 *      并应用对应的 alternative patch（如将 NOP 替换为优化指令序列）。 */
	/*
		capability（能力）内核决定用什么

		内核对 feature 的加工和决策结果——综合考虑硬件支持、内核配置、安全策略等因素，最终判断"内核是否启用这个功能"：

		capability = feature + 内核决策

		例：
			硬件支持 MTE（feature 存在）
			+ CONFIG_KASAN_HW_TAGS=y（内核编译开启）
			+ 命令行没有 kasan.mode=off（运行时未禁用）
			——→ ARM64_MTE capability 成立
	*/
	/*
		features 和 capability 的关系

		feature（硬件原始值）
			↓ 经过 capability 定义中的 matches() 函数判断
		capability（内核启用决策）
			↓ 成立后
		alternative patch（替换机器码中的 NOP 为优化指令）
		system_uses_irq_prio_masking() 等查询函数返回 true

		一个 capability 可能依赖多个 feature，也可能依赖其他 capability：

		// ARM64_HAS_GIC_PRIO_MASKING 的成立条件：
		//   1. GIC 支持优先级屏蔽（硬件 feature）
		//   2. ARM64_HAS_GICV3_CPUIF capability 已成立
		//   3. CONFIG_ARM64_PSEUDO_NMI=y（编译配置）
		//   4. 命令行包含 irqchip.gicv3_pseudo_nmi=1
	*/
	setup_boot_cpu_features();

	/* Conditionally switch to GIC PMR for interrupt masking
	 *
	 * 若系统启用了 CONFIG_ARM64_PSEUDO_NMI 且 GIC 支持优先级屏蔽，
	 * 切换到用 ICC_PMR_EL1 控制中断开关的模式（而非 DAIF.I 位），
	 * 以支持高优先级 FIQ 作为伪 NMI 穿透关中断临界区。
	 * system_uses_irq_prio_masking() 使用静态分支，运行时零开销。 */
	if (system_uses_irq_prio_masking())
		init_gic_priority_masking();

	/* 初始化硬件内存标签（MTE，Memory Tagging Extension）用于 KASAN。
	 * MTE 是 ARMv8.5 引入的硬件特性，可在内存访问时自动检查地址标签，
	 * 用于检测堆溢出、use-after-free 等内存错误。
	 * 若硬件不支持 MTE 或 KASAN 被命令行禁用，此函数直接返回。 */
	kasan_init_hw_tags();

	/* Init percpu seeds for random tags after cpus are set up.
	 *
	 * 初始化软件内存标签（sw-tags KASAN）的每 CPU 随机数种子。
	 * sw-tags KASAN 在每次分配内存时生成随机标签写入指针高位，
	 * 并在访问时验证，用于不支持 MTE 硬件的系统上检测内存错误。
	 * 用 get_cycles()（CPU 时钟计数器）作为种子，确保各 CPU 种子不同。
	 * 必须在 per-cpu 区域切换完成后调用，因为种子存储在 per-cpu 变量中。 */
	kasan_init_sw_tags();
}

/*
 * Duplicate MPIDRs are a recipe for disaster. Scan all initialized
 * entries and check for duplicates. If any is found just ignore the
 * cpu. cpu_logical_map was initialized to INVALID_HWID to avoid
 * matching valid MPIDR values.
 */
static bool __init is_mpidr_duplicate(unsigned int cpu, u64 hwid)
{
	unsigned int i;

	for (i = 1; (i < cpu) && (i < NR_CPUS); i++)
		if (cpu_logical_map(i) == hwid)
			return true;
	return false;
}

/*
 * Initialize cpu operations for a logical cpu and
 * set it in the possible mask on success
 */
static int __init smp_cpu_setup(int cpu)
{
	const struct cpu_operations *ops;

	if (init_cpu_ops(cpu))
		return -ENODEV;

	ops = get_cpu_ops(cpu);
	if (ops->cpu_init(cpu))
		return -ENODEV;

	set_cpu_possible(cpu, true);

	return 0;
}

static bool bootcpu_valid __initdata;
static unsigned int cpu_count = 1;

int arch_register_cpu(int cpu)
{
	acpi_handle acpi_handle = acpi_get_processor_handle(cpu);
	struct cpu *c = &per_cpu(cpu_devices, cpu);

	if (!acpi_disabled && !acpi_handle &&
	    IS_ENABLED(CONFIG_ACPI_HOTPLUG_CPU))
		return -EPROBE_DEFER;

#ifdef CONFIG_ACPI_HOTPLUG_CPU
	/* For now block anything that looks like physical CPU Hotplug */
	if (invalid_logical_cpuid(cpu) || !cpu_present(cpu)) {
		pr_err_once("Changing CPU present bit is not supported\n");
		return -ENODEV;
	}
#endif

	/*
	 * Availability of the acpi handle is sufficient to establish
	 * that _STA has already been checked. No need to recheck here.
	 */
	c->hotpluggable = arch_cpu_is_hotpluggable(cpu);

	return register_cpu(c, cpu);
}

#ifdef CONFIG_ACPI_HOTPLUG_CPU
void arch_unregister_cpu(int cpu)
{
	acpi_handle acpi_handle = acpi_get_processor_handle(cpu);
	struct cpu *c = &per_cpu(cpu_devices, cpu);
	unsigned long long sta;
	acpi_status status;

	status = acpi_evaluate_integer(acpi_handle, "_STA", NULL, &sta);
	if (!ACPI_FAILURE(status) &&
	    cpu_present(cpu) && !(sta & ACPI_STA_DEVICE_PRESENT))
		pr_err_once("Changing CPU present bit is not supported\n");

	unregister_cpu(c);
}
#endif /* CONFIG_ACPI_HOTPLUG_CPU */

#ifdef CONFIG_ACPI
static struct acpi_madt_generic_interrupt cpu_madt_gicc[NR_CPUS];

struct acpi_madt_generic_interrupt *acpi_cpu_get_madt_gicc(int cpu)
{
	return &cpu_madt_gicc[cpu];
}
EXPORT_SYMBOL_GPL(acpi_cpu_get_madt_gicc);

static bool acpi_cpu_is_present(int cpu)
{
	return acpi_cpu_get_madt_gicc(cpu)->flags & ACPI_MADT_ENABLED;
}

/*
 * acpi_map_gic_cpu_interface - parse processor MADT entry
 *
 * Carry out sanity checks on MADT processor entry and initialize
 * cpu_logical_map on success
 */
static void __init
acpi_map_gic_cpu_interface(struct acpi_madt_generic_interrupt *processor)
{
	u64 hwid = processor->arm_mpidr;

	if (!(processor->flags &
	      (ACPI_MADT_ENABLED | ACPI_MADT_GICC_ONLINE_CAPABLE))) {
		pr_debug("skipping disabled CPU entry with 0x%llx MPIDR\n", hwid);
		return;
	}

	if (hwid & ~MPIDR_HWID_BITMASK || hwid == INVALID_HWID) {
		pr_err("skipping CPU entry with invalid MPIDR 0x%llx\n", hwid);
		return;
	}

	if (is_mpidr_duplicate(cpu_count, hwid)) {
		pr_err("duplicate CPU MPIDR 0x%llx in MADT\n", hwid);
		return;
	}

	/* Check if GICC structure of boot CPU is available in the MADT */
	if (cpu_logical_map(0) == hwid) {
		if (bootcpu_valid) {
			pr_err("duplicate boot CPU MPIDR: 0x%llx in MADT\n",
			       hwid);
			return;
		}
		bootcpu_valid = true;
		cpu_madt_gicc[0] = *processor;
		return;
	}

	if (cpu_count >= NR_CPUS)
		return;

	/* map the logical cpu id to cpu MPIDR */
	set_cpu_logical_map(cpu_count, hwid);

	cpu_madt_gicc[cpu_count] = *processor;

	/*
	 * Set-up the ACPI parking protocol cpu entries
	 * while initializing the cpu_logical_map to
	 * avoid parsing MADT entries multiple times for
	 * nothing (ie a valid cpu_logical_map entry should
	 * contain a valid parking protocol data set to
	 * initialize the cpu if the parking protocol is
	 * the only available enable method).
	 */
	acpi_set_mailbox_entry(cpu_count, processor);

	cpu_count++;
}

static int __init
acpi_parse_gic_cpu_interface(union acpi_subtable_headers *header,
			     const unsigned long end)
{
	struct acpi_madt_generic_interrupt *processor;

	processor = (struct acpi_madt_generic_interrupt *)header;
	if (BAD_MADT_GICC_ENTRY(processor, end))
		return -EINVAL;

	acpi_table_print_madt_entry(&header->common);

	acpi_map_gic_cpu_interface(processor);

	return 0;
}

static void __init acpi_parse_and_init_cpus(void)
{
	int i;

	/*
	 * do a walk of MADT to determine how many CPUs
	 * we have including disabled CPUs, and get information
	 * we need for SMP init.
	 */
	acpi_table_parse_madt(ACPI_MADT_TYPE_GENERIC_INTERRUPT,
				      acpi_parse_gic_cpu_interface, 0);

	/*
	 * In ACPI, SMP and CPU NUMA information is provided in separate
	 * static tables, namely the MADT and the SRAT.
	 *
	 * Thus, it is simpler to first create the cpu logical map through
	 * an MADT walk and then map the logical cpus to their node ids
	 * as separate steps.
	 */
	acpi_map_cpus_to_nodes();

	for (i = 0; i < nr_cpu_ids; i++)
		early_map_cpu_to_node(i, acpi_numa_get_nid(i));
}
#else
static bool acpi_cpu_is_present(int cpu)
{
	return false;
}
#define acpi_parse_and_init_cpus(...)	do { } while (0)
#endif

/*
 * Enumerate the possible CPU set from the device tree and build the
 * cpu logical map array containing MPIDR values related to logical
 * cpus. Assumes that cpu_logical_map(0) has already been initialized.
 */
static void __init of_parse_and_init_cpus(void)
{
	struct device_node *dn;

	for_each_of_cpu_node(dn) {
		u64 hwid = of_get_cpu_hwid(dn, 0);

		if (hwid & ~MPIDR_HWID_BITMASK)
			goto next;

		if (is_mpidr_duplicate(cpu_count, hwid)) {
			pr_err("%pOF: duplicate cpu reg properties in the DT\n",
				dn);
			goto next;
		}

		/*
		 * The numbering scheme requires that the boot CPU
		 * must be assigned logical id 0. Record it so that
		 * the logical map built from DT is validated and can
		 * be used.
		 */
		if (hwid == cpu_logical_map(0)) {
			if (bootcpu_valid) {
				pr_err("%pOF: duplicate boot cpu reg property in DT\n",
					dn);
				goto next;
			}

			bootcpu_valid = true;
			early_map_cpu_to_node(0, of_node_to_nid(dn));

			/*
			 * cpu_logical_map has already been
			 * initialized and the boot cpu doesn't need
			 * the enable-method so continue without
			 * incrementing cpu.
			 */
			continue;
		}

		if (cpu_count >= NR_CPUS)
			goto next;

		pr_debug("cpu logical map 0x%llx\n", hwid);
		set_cpu_logical_map(cpu_count, hwid);

		early_map_cpu_to_node(cpu_count, of_node_to_nid(dn));
next:
		cpu_count++;
	}
}

/*
 * Enumerate the possible CPU set from the device tree or ACPI and build the
 * cpu logical map array containing MPIDR values related to logical
 * cpus. Assumes that cpu_logical_map(0) has already been initialized.
 */
void __init smp_init_cpus(void)
{
	int i;

	if (acpi_disabled)
		of_parse_and_init_cpus();
	else
		acpi_parse_and_init_cpus();

	if (!bootcpu_valid) {
		pr_err("missing boot CPU MPIDR, not enabling secondaries\n");
		return;
	}

	/*
	 * For the nosmp/maxcpus=0 case, do not mark the secondary CPUs
	 * possible.
	 */
	if (!setup_max_cpus)
		return;

	if (cpu_count > nr_cpu_ids)
		pr_warn("Number of cores (%d) exceeds configured maximum of %u - clipping\n",
			cpu_count, nr_cpu_ids);
	/*
	 * We need to set the cpu_logical_map entries before enabling
	 * the cpus so that cpu processor description entries (DT cpu nodes
	 * and ACPI MADT entries) can be retrieved by matching the cpu hwid
	 * with entries in cpu_logical_map while initializing the cpus.
	 * If the cpu set-up fails, invalidate the cpu_logical_map entry.
	 */
	for (i = 1; i < nr_cpu_ids; i++) {
		if (cpu_logical_map(i) != INVALID_HWID) {
			if (smp_cpu_setup(i))
				set_cpu_logical_map(i, INVALID_HWID);
		}
	}
}

void __init smp_prepare_cpus(unsigned int max_cpus)
{
	const struct cpu_operations *ops;
	int err;
	unsigned int cpu;
	unsigned int this_cpu;

	init_cpu_topology();

	this_cpu = smp_processor_id();
	store_cpu_topology(this_cpu);
	numa_store_cpu_info(this_cpu);
	numa_add_cpu(this_cpu);

	/*
	 * If UP is mandated by "nosmp" (which implies "maxcpus=0"), don't set
	 * secondary CPUs present.
	 */
	if (max_cpus == 0)
		return;

	/*
	 * Initialise the present map (which describes the set of CPUs
	 * actually populated at the present time) and release the
	 * secondaries from the bootloader.
	 */
	for_each_possible_cpu(cpu) {

		if (cpu == smp_processor_id())
			continue;

		ops = get_cpu_ops(cpu);
		if (!ops)
			continue;

		err = ops->cpu_prepare(cpu);
		if (err)
			continue;

		if (acpi_disabled || acpi_cpu_is_present(cpu))
			set_cpu_present(cpu, true);
		numa_store_cpu_info(cpu);
	}
}

static const char *ipi_types[MAX_IPI] __tracepoint_string = {
	[IPI_RESCHEDULE]	= "Rescheduling interrupts",
	[IPI_CALL_FUNC]		= "Function call interrupts",
	[IPI_CPU_STOP]		= "CPU stop interrupts",
	[IPI_CPU_STOP_NMI]	= "CPU stop NMIs",
	[IPI_TIMER]		= "Timer broadcast interrupts",
	[IPI_IRQ_WORK]		= "IRQ work interrupts",
	[IPI_CPU_BACKTRACE]	= "CPU backtrace interrupts",
	[IPI_KGDB_ROUNDUP]	= "KGDB roundup interrupts",
};

static void smp_cross_call(const struct cpumask *target, unsigned int ipinr);

unsigned long irq_err_count;

int arch_show_interrupts(struct seq_file *p, int prec)
{
	unsigned int cpu, i;

	for (i = 0; i < MAX_IPI; i++) {
		seq_printf(p, "%*s%u: ", prec - 1, "IPI", i);
		for_each_online_cpu(cpu)
			seq_printf(p, "%10u ", irq_desc_kstat_cpu(get_ipi_desc(cpu, i), cpu));
		seq_printf(p, " %s\n", ipi_types[i]);
	}

	seq_printf(p, "%*s: %10lu\n", prec, "Err", irq_err_count);
	return 0;
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	smp_cross_call(mask, IPI_CALL_FUNC);
}

void arch_send_call_function_single_ipi(int cpu)
{
	smp_cross_call(cpumask_of(cpu), IPI_CALL_FUNC);
}

#ifdef CONFIG_IRQ_WORK
void arch_irq_work_raise(void)
{
	smp_cross_call(cpumask_of(smp_processor_id()), IPI_IRQ_WORK);
}
#endif

static void __noreturn local_cpu_stop(unsigned int cpu)
{
	set_cpu_online(cpu, false);

	local_daif_mask();
	sdei_mask_local_cpu();
	cpu_park_loop();
}

/*
 * We need to implement panic_smp_self_stop() for parallel panic() calls, so
 * that cpu_online_mask gets correctly updated and smp_send_stop() can skip
 * CPUs that have already stopped themselves.
 */
void __noreturn panic_smp_self_stop(void)
{
	local_cpu_stop(smp_processor_id());
}

static void __noreturn ipi_cpu_crash_stop(unsigned int cpu, struct pt_regs *regs)
{
#ifdef CONFIG_KEXEC_CORE
	/*
	 * Use local_daif_mask() instead of local_irq_disable() to make sure
	 * that pseudo-NMIs are disabled. The "crash stop" code starts with
	 * an IRQ and falls back to NMI (which might be pseudo). If the IRQ
	 * finally goes through right as we're timing out then the NMI could
	 * interrupt us. It's better to prevent the NMI and let the IRQ
	 * finish since the pt_regs will be better.
	 */
	local_daif_mask();

	crash_save_cpu(regs, cpu);

	set_cpu_online(cpu, false);

	sdei_mask_local_cpu();

	if (IS_ENABLED(CONFIG_HOTPLUG_CPU))
		__cpu_try_die(cpu);

	/* just in case */
	cpu_park_loop();
#else
	BUG();
#endif
}

static void arm64_send_ipi(const cpumask_t *mask, unsigned int nr)
{
	unsigned int cpu;

	if (!percpu_ipi_descs)
		__ipi_send_mask(get_ipi_desc(0, nr), mask);
	else
		for_each_cpu(cpu, mask)
			__ipi_send_single(get_ipi_desc(cpu, nr), cpu);
}

static void arm64_backtrace_ipi(cpumask_t *mask)
{
	arm64_send_ipi(mask, IPI_CPU_BACKTRACE);
}

void arch_trigger_cpumask_backtrace(const cpumask_t *mask, int exclude_cpu)
{
	/*
	 * NOTE: though nmi_trigger_cpumask_backtrace() has "nmi_" in the name,
	 * nothing about it truly needs to be implemented using an NMI, it's
	 * just that it's _allowed_ to work with NMIs. If ipi_should_be_nmi()
	 * returned false our backtrace attempt will just use a regular IPI.
	 */
	nmi_trigger_cpumask_backtrace(mask, exclude_cpu, arm64_backtrace_ipi);
}

#ifdef CONFIG_KGDB
void kgdb_roundup_cpus(void)
{
	int this_cpu = raw_smp_processor_id();
	int cpu;

	for_each_online_cpu(cpu) {
		/* No need to roundup ourselves */
		if (cpu == this_cpu)
			continue;

		__ipi_send_single(get_ipi_desc(cpu, IPI_KGDB_ROUNDUP), cpu);
	}
}
#endif

/*
 * Main handler for inter-processor interrupts
 */
static void do_handle_IPI(int ipinr)
{
	unsigned int cpu = smp_processor_id();

	if ((unsigned)ipinr < NR_IPI)
		trace_ipi_entry(ipi_types[ipinr]);

	switch (ipinr) {
	case IPI_RESCHEDULE:
		scheduler_ipi();
		break;

	case IPI_CALL_FUNC:
		generic_smp_call_function_interrupt();
		break;

	case IPI_CPU_STOP:
	case IPI_CPU_STOP_NMI:
		if (IS_ENABLED(CONFIG_KEXEC_CORE) && crash_stop) {
			ipi_cpu_crash_stop(cpu, get_irq_regs());
			unreachable();
		} else {
			local_cpu_stop(cpu);
		}
		break;

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
	case IPI_TIMER:
		tick_receive_broadcast();
		break;
#endif

#ifdef CONFIG_IRQ_WORK
	case IPI_IRQ_WORK:
		irq_work_run();
		break;
#endif

	case IPI_CPU_BACKTRACE:
		/*
		 * NOTE: in some cases this _won't_ be NMI context. See the
		 * comment in arch_trigger_cpumask_backtrace().
		 */
		nmi_cpu_backtrace(get_irq_regs());
		break;

	case IPI_KGDB_ROUNDUP:
		kgdb_nmicallback(cpu, get_irq_regs());
		break;

	default:
		pr_crit("CPU%u: Unknown IPI message 0x%x\n", cpu, ipinr);
		break;
	}

	if ((unsigned)ipinr < NR_IPI)
		trace_ipi_exit(ipi_types[ipinr]);
}

static irqreturn_t ipi_handler(int irq, void *data)
{
	unsigned int ipi = (irq - ipi_irq_base) % nr_ipi;

	do_handle_IPI(ipi);
	return IRQ_HANDLED;
}

static void smp_cross_call(const struct cpumask *target, unsigned int ipinr)
{
	trace_ipi_raise(target, ipi_types[ipinr]);
	arm64_send_ipi(target, ipinr);
}

static bool ipi_should_be_nmi(enum ipi_msg_type ipi)
{
	if (!system_uses_irq_prio_masking())
		return false;

	switch (ipi) {
	case IPI_CPU_STOP_NMI:
	case IPI_CPU_BACKTRACE:
	case IPI_KGDB_ROUNDUP:
		return true;
	default:
		return false;
	}
}

static void ipi_setup(int cpu)
{
	int i;

	if (WARN_ON_ONCE(!ipi_irq_base))
		return;

	for (i = 0; i < nr_ipi; i++) {
		if (!percpu_ipi_descs) {
			if (ipi_should_be_nmi(i)) {
				prepare_percpu_nmi(ipi_irq_base + i);
				enable_percpu_nmi(ipi_irq_base + i, 0);
			} else {
				enable_percpu_irq(ipi_irq_base + i, 0);
			}
		} else {
			enable_irq(irq_desc_get_irq(get_ipi_desc(cpu, i)));
		}
	}
}

#ifdef CONFIG_HOTPLUG_CPU
static void ipi_teardown(int cpu)
{
	int i;

	if (WARN_ON_ONCE(!ipi_irq_base))
		return;

	for (i = 0; i < nr_ipi; i++) {
		if (!percpu_ipi_descs) {
			if (ipi_should_be_nmi(i)) {
				disable_percpu_nmi(ipi_irq_base + i);
				teardown_percpu_nmi(ipi_irq_base + i);
			} else {
				disable_percpu_irq(ipi_irq_base + i);
			}
		} else {
			disable_irq(irq_desc_get_irq(get_ipi_desc(cpu, i)));
		}
	}
}
#endif

static void ipi_setup_sgi(int ipi)
{
	int err, irq, cpu;

	irq = ipi_irq_base + ipi;

	if (ipi_should_be_nmi(ipi)) {
		err = request_percpu_nmi(irq, ipi_handler, "IPI", NULL, &irq_stat);
		WARN(err, "Could not request IRQ %d as NMI, err=%d\n", irq, err);
	} else {
		err = request_percpu_irq(irq, ipi_handler, "IPI", &irq_stat);
		WARN(err, "Could not request IRQ %d as IRQ, err=%d\n", irq, err);
	}

	for_each_possible_cpu(cpu)
		get_ipi_desc(cpu, ipi) = irq_to_desc(irq);

	irq_set_status_flags(irq, IRQ_HIDDEN);
}

static void ipi_setup_lpi(int ipi, int ncpus)
{
	for (int cpu = 0; cpu < ncpus; cpu++) {
		int err, irq;

		irq = ipi_irq_base + (cpu * nr_ipi) + ipi;

		err = irq_force_affinity(irq, cpumask_of(cpu));
		WARN(err, "Could not force affinity IRQ %d, err=%d\n", irq, err);

		err = request_irq(irq, ipi_handler, IRQF_NO_AUTOEN, "IPI",
				  NULL);
		WARN(err, "Could not request IRQ %d, err=%d\n", irq, err);

		irq_set_status_flags(irq, (IRQ_HIDDEN | IRQ_NO_BALANCING_MASK));

		get_ipi_desc(cpu, ipi) = irq_to_desc(irq);
	}
}

void __init set_smp_ipi_range_percpu(int ipi_base, int n, int ncpus)
{
	int i;

	WARN_ON(n < MAX_IPI);
	nr_ipi = min(n, MAX_IPI);

	percpu_ipi_descs = !!ncpus;
	ipi_irq_base = ipi_base;

	for (i = 0; i < nr_ipi; i++) {
		if (!percpu_ipi_descs)
			ipi_setup_sgi(i);
		else
			ipi_setup_lpi(i, ncpus);
	}

	/* Setup the boot CPU immediately */
	ipi_setup(smp_processor_id());
}

void arch_smp_send_reschedule(int cpu)
{
	smp_cross_call(cpumask_of(cpu), IPI_RESCHEDULE);
}

#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
void arch_send_wakeup_ipi(unsigned int cpu)
{
	/*
	 * We use a scheduler IPI to wake the CPU as this avoids the need for a
	 * dedicated IPI and we can safely handle spurious scheduler IPIs.
	 */
	smp_send_reschedule(cpu);
}
#endif

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
void tick_broadcast(const struct cpumask *mask)
{
	smp_cross_call(mask, IPI_TIMER);
}
#endif

/*
 * The number of CPUs online, not counting this CPU (which may not be
 * fully online and so not counted in num_online_cpus()).
 */
static inline unsigned int num_other_online_cpus(void)
{
	unsigned int this_cpu_online = cpu_online(smp_processor_id());

	return num_online_cpus() - this_cpu_online;
}

void smp_send_stop(void)
{
	static unsigned long stop_in_progress;
	static cpumask_t mask;
	unsigned long timeout;

	/*
	 * If this cpu is the only one alive at this point in time, online or
	 * not, there are no stop messages to be sent around, so just back out.
	 */
	if (num_other_online_cpus() == 0)
		goto skip_ipi;

	/* Only proceed if this is the first CPU to reach this code */
	if (test_and_set_bit(0, &stop_in_progress))
		return;

	/*
	 * Send an IPI to all currently online CPUs except the CPU running
	 * this code.
	 *
	 * NOTE: we don't do anything here to prevent other CPUs from coming
	 * online after we snapshot `cpu_online_mask`. Ideally, the calling code
	 * should do something to prevent other CPUs from coming up. This code
	 * can be called in the panic path and thus it doesn't seem wise to
	 * grab the CPU hotplug mutex ourselves. Worst case:
	 * - If a CPU comes online as we're running, we'll likely notice it
	 *   during the 1 second wait below and then we'll catch it when we try
	 *   with an NMI (assuming NMIs are enabled) since we re-snapshot the
	 *   mask before sending an NMI.
	 * - If we leave the function and see that CPUs are still online we'll
	 *   at least print a warning. Especially without NMIs this function
	 *   isn't foolproof anyway so calling code will just have to accept
	 *   the fact that there could be cases where a CPU can't be stopped.
	 */
	cpumask_copy(&mask, cpu_online_mask);
	cpumask_clear_cpu(smp_processor_id(), &mask);

	if (system_state <= SYSTEM_RUNNING)
		pr_crit("SMP: stopping secondary CPUs\n");

	/*
	 * Start with a normal IPI and wait up to one second for other CPUs to
	 * stop. We do this first because it gives other processors a chance
	 * to exit critical sections / drop locks and makes the rest of the
	 * stop process (especially console flush) more robust.
	 */
	smp_cross_call(&mask, IPI_CPU_STOP);
	timeout = USEC_PER_SEC;
	while (num_other_online_cpus() && timeout--)
		udelay(1);

	/*
	 * If CPUs are still online, try an NMI. There's no excuse for this to
	 * be slow, so we only give them an extra 10 ms to respond.
	 */
	if (num_other_online_cpus() && ipi_should_be_nmi(IPI_CPU_STOP_NMI)) {
		smp_rmb();
		cpumask_copy(&mask, cpu_online_mask);
		cpumask_clear_cpu(smp_processor_id(), &mask);

		pr_info("SMP: retry stop with NMI for CPUs %*pbl\n",
			cpumask_pr_args(&mask));

		smp_cross_call(&mask, IPI_CPU_STOP_NMI);
		timeout = USEC_PER_MSEC * 10;
		while (num_other_online_cpus() && timeout--)
			udelay(1);
	}

	if (num_other_online_cpus()) {
		smp_rmb();
		cpumask_copy(&mask, cpu_online_mask);
		cpumask_clear_cpu(smp_processor_id(), &mask);

		pr_warn("SMP: failed to stop secondary CPUs %*pbl\n",
			cpumask_pr_args(&mask));
	}

skip_ipi:
	sdei_mask_local_cpu();
}

#ifdef CONFIG_KEXEC_CORE
void crash_smp_send_stop(void)
{
	/*
	 * This function can be called twice in panic path, but obviously
	 * we execute this only once.
	 *
	 * We use this same boolean to tell whether the IPI we send was a
	 * stop or a "crash stop".
	 */
	if (crash_stop)
		return;
	crash_stop = 1;

	smp_send_stop();

	sdei_handler_abort();
}

bool smp_crash_stop_failed(void)
{
	return num_other_online_cpus() != 0;
}
#endif

static bool have_cpu_die(void)
{
#ifdef CONFIG_HOTPLUG_CPU
	int any_cpu = raw_smp_processor_id();
	const struct cpu_operations *ops = get_cpu_ops(any_cpu);

	if (ops && ops->cpu_die)
		return true;
#endif
	return false;
}

bool cpus_are_stuck_in_kernel(void)
{
	bool smp_spin_tables = (num_possible_cpus() > 1 && !have_cpu_die());

	return !!cpus_stuck_in_kernel || smp_spin_tables ||
		is_protected_kvm_enabled();
}
