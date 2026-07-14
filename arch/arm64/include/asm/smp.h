/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_SMP_H
#define __ASM_SMP_H

#include <linux/const.h>

/* Values for secondary_data.status */
#define CPU_STUCK_REASON_SHIFT		(8)
#define CPU_BOOT_STATUS_MASK		((UL(1) << CPU_STUCK_REASON_SHIFT) - 1)

#define CPU_MMU_OFF			(-1)
#define CPU_BOOT_SUCCESS		(0)
/* The cpu invoked ops->cpu_die, synchronise it with cpu_kill */
#define CPU_KILL_ME			(1)
/* The cpu couldn't die gracefully and is looping in the kernel */
#define CPU_STUCK_IN_KERNEL		(2)
/* Fatal system error detected by secondary CPU, crash the system */
#define CPU_PANIC_KERNEL		(3)

#define CPU_STUCK_REASON_52_BIT_VA	(UL(1) << CPU_STUCK_REASON_SHIFT)
#define CPU_STUCK_REASON_NO_GRAN	(UL(2) << CPU_STUCK_REASON_SHIFT)

#ifndef __ASSEMBLER__

#include <linux/threads.h>
#include <linux/cpumask.h>
#include <linux/thread_info.h>

#define raw_smp_processor_id() (current_thread_info()->cpu)

/*
 * Logical CPU mapping.
 */
/*
__cpu_logical_map 是一张逻辑 CPU 编号 → 物理硬件 ID（MPIDR）的映射表。

背景：两套编号并存

- 逻辑编号：内核内部使用的 0, 1, 2, 3...，连续且稳定，smp_processor_id() 返回的就是这个
- MPIDR（物理硬件 ID）：硬件寄存器 MPIDR_EL1 的值，编码了 CPU 的 Aff0/Aff1/Aff2/Aff3（cluster 层级），不一定连续

为什么需要这张表

硬件操作（如唤醒 secondary CPU、GIC 中断路由、PSCI 调用）必须用 MPIDR，而内核调度、CPU mask 等逻辑用逻辑编号。__cpu_logical_map 就是两者的桥梁：

// 典型用法：用逻辑编号查出 MPIDR，再去操作硬件
u64 mpidr = cpu_logical_map(cpu);
psci_cpu_on(mpidr, entry_point);   // 唤醒某个 CPU

生命周期

┌────────────────────────┬──────────────────────────────────────────────────────────┐
│          时机          │                           动作                           │
├────────────────────────┼──────────────────────────────────────────────────────────┤
│ 初始化为 INVALID_HWID  │ arch/arm64/kernel/setup.c:441                            │
├────────────────────────┼──────────────────────────────────────────────────────────┤
│ boot CPU（逻辑 0）写入 │ smp_setup_processor_id() 读 MPIDR_EL1 后写入 [0]         │
├────────────────────────┼──────────────────────────────────────────────────────────┤
│ 其余 CPU 写入          │ smp_init_cpus() 解析设备树 cpu 节点的 reg 属性，逐个填充 │
└────────────────────────┴──────────────────────────────────────────────────────────┘
*/
extern u64 __cpu_logical_map[NR_CPUS];
extern u64 cpu_logical_map(unsigned int cpu);

static inline void set_cpu_logical_map(unsigned int cpu, u64 hwid)
{
	__cpu_logical_map[cpu] = hwid;
}

struct seq_file;

/*
 * Discover the set of possible CPUs and determine their
 * SMP operations.
 */
extern void smp_init_cpus(void);

enum ipi_msg_type {
	IPI_RESCHEDULE,
	IPI_CALL_FUNC,
	IPI_CPU_STOP,
	IPI_CPU_STOP_NMI,
	IPI_TIMER,
	IPI_IRQ_WORK,
	NR_IPI,
	/*
	 * Any enum >= NR_IPI and < MAX_IPI is special and not tracable
	 * with trace_ipi_*
	 */
	IPI_CPU_BACKTRACE = NR_IPI,
	IPI_KGDB_ROUNDUP,
	MAX_IPI
};

/*
 * Register IPI interrupts with the arch SMP code
 */
extern void set_smp_ipi_range_percpu(int ipi_base, int nr_ipi, int ncpus);

static inline void set_smp_ipi_range(int ipi_base, int n)
{
	set_smp_ipi_range_percpu(ipi_base, n, 0);
}

/*
 * Called from the secondary holding pen, this is the secondary CPU entry point.
 */
asmlinkage void secondary_start_kernel(void);

/*
 * Initial data for bringing up a secondary CPU.
 * @status - Result passed back from the secondary CPU to
 *           indicate failure.
 */
struct secondary_data {
	struct task_struct *task;
	long status;
};

extern struct secondary_data secondary_data;
extern long __early_cpu_boot_status;
extern void secondary_entry(void);

extern void arch_send_call_function_single_ipi(int cpu);
extern void arch_send_call_function_ipi_mask(const struct cpumask *mask);

#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
extern void arch_send_wakeup_ipi(unsigned int cpu);
#else
static inline void arch_send_wakeup_ipi(unsigned int cpu)
{
	BUILD_BUG();
}
#endif

extern int __cpu_disable(void);

static inline void __cpu_die(unsigned int cpu) { }
extern void __noreturn cpu_die(void);
extern void __noreturn cpu_die_early(void);

static inline void __noreturn cpu_park_loop(void)
{
	for (;;) {
		wfe();
		wfi();
	}
}

static inline void update_cpu_boot_status(int val)
{
	WRITE_ONCE(secondary_data.status, val);
	/* Ensure the visibility of the status update */
	dsb(ishst);
}

/*
 * The calling secondary CPU has detected serious configuration mismatch,
 * which calls for a kernel panic. Update the boot status and park the calling
 * CPU.
 */
static inline void __noreturn cpu_panic_kernel(void)
{
	update_cpu_boot_status(CPU_PANIC_KERNEL);
	cpu_park_loop();
}

/*
 * If a secondary CPU enters the kernel but fails to come online,
 * (e.g. due to mismatched features), and cannot exit the kernel,
 * we increment cpus_stuck_in_kernel and leave the CPU in a
 * quiesecent loop within the kernel text. The memory containing
 * this loop must not be re-used for anything else as the 'stuck'
 * core is executing it.
 *
 * This function is used to inhibit features like kexec and hibernate.
 */
bool cpus_are_stuck_in_kernel(void);

extern void crash_smp_send_stop(void);
extern bool smp_crash_stop_failed(void);

#endif /* ifndef __ASSEMBLER__ */

#endif /* ifndef __ASM_SMP_H */
