// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 SMP 启动与 IPI 学习导读
 *
 * 中文学习注释生成模型：OpenAI GPT-5 Codex（2026-07-27）。
 * 源码分析基线：doc/lql 分支，commit f8f7ac7435bf。
 * 宏观学习入口：doc/00 ANDROID_BOOT_GUIDE.md 的“SMP 多核启动”，以及
 * doc/02 arm64-interrupt-model.md 的 GIC/伪 NMI 说明。
 *
 * 本文件位于通用 SMP/CPU hotplug 框架与 arm64 硬件启动协议之间，负责：
 *
 *   - 从设备树或 ACPI 枚举 CPU，建立 Linux 逻辑 CPU 号到 MPIDR 的映射；
 *   - 通过 cpu_operations 调用 PSCI、spin-table 或 parking-protocol，
 *     启动、验证和关闭 CPU；
 *   - 在次级 CPU 的早期入口建立 MM、特性、中断、NUMA 和拓扑状态，并与
 *     启动 CPU 完成上线握手；
 *   - 建立 arm64 IPI IRQ，把调度、跨 CPU 函数调用、时钟、回溯、停止等
 *     逻辑消息分派给通用内核子系统；
 *   - 在 panic、kexec 和 CPU hotplug 路径中停止或最终停放 CPU。
 *
 * 它不实现具体固件调用，也不实现 GIC 的底层发中断操作：前者由各
 * cpu_operations 后端完成，后者由 irqchip 提供的 IRQ descriptor 和
 * __ipi_send_*() 接口完成。
 *
 * CPU 启动的核心状态链为：
 *
 *   固件表枚举 -> possible -> cpu_prepare() -> present
 *       -> __cpu_up()/cpu_boot() -> secondary_start_kernel()
 *       -> online -> complete(cpu_running)
 *
 * secondary_data 是启动 CPU 与“当前正在启动的次级 CPU”共享的临时交接区，
 * cpu_running completion 是反向上线确认。CPU hotplug 框架串行化正常
 * bring-up/teardown；IPI 描述符按 CPU 保存，启停时只操作目标 CPU 的中断。
 * panic 停机路径不能依赖 hotplug 锁，因而使用原子门闩、在线掩码快照和
 * 有界轮询，并接受无法绝对阻止并发 CPU 上线这一退化条件。
 *
 * 这种分层让同一套 SMP 状态机复用多种固件协议，代价是启动失败需要
 * 结合 cpu_operations 返回值、共享 boot status 和在线握手状态共同诊断。
 */
/*
 * SMP initialisation and IPI support
 * Based on arch/arm/kernel/smp.c
 *
 * Copyright (C) 2012 ARM Ltd.
 */
/*
 * 本文件负责 SMP 初始化和处理器间中断（IPI）支持，其实现以 32 位 ARM
 * 的 arch/arm/kernel/smp.c 为基础演进而来。
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
/*
 * 从 Linux 2.5 起内核不再提供 init_tasks 数组，因此不能再按 CPU 从该数组
 * 找初始任务；启动 CPU 必须通过另一个共享位置告诉新核心把 SVC/内核栈
 * 放在哪里。secondary_data 正是这个替代交接机制。
 *
 * 启动 CPU 在调用具体 cpu_boot() 前填写这个单槽交接区；早期汇编和
 * secondary_start_kernel() 用它找到目标 CPU 的 idle task、初始栈及启动
 * 状态。正常 CPU 上线由 CPU hotplug 核心串行进行，所以同一时刻只有一个
 * 次级 CPU 消费该对象；字段发布和早期状态读取需配合架构启动协议中的
 * cache/屏障以及 READ_ONCE()/WRITE_ONCE()。
 */
struct secondary_data secondary_data;
/* Number of CPUs which aren't online, but looping in kernel text. */
/*
 * 已进入内核代码、却无法上线或退出的 CPU 数量。它只在串行启动失败
 * 路径增加，最终供 cpus_are_stuck_in_kernel() 判断 kexec 等操作能否
 * 安全覆盖这些 CPU 可能仍在执行的内核内存。
 */
static int cpus_stuck_in_kernel;

/*
 * IPI IRQ 编号空间由 irqchip 初始化代码一次性发布，随后成为只读状态：
 * SGI 模式下所有 CPU 共用 [ipi_irq_base, ipi_irq_base + nr_ipi)；
 * per-CPU LPI 模式下每个 CPU 拥有自己的一组 nr_ipi 个 IRQ。
 */
static int ipi_irq_base __ro_after_init;
static int nr_ipi __ro_after_init = NR_IPI;

/* 保存某个逻辑 CPU 的每一种 IPI 消息所对应的 IRQ descriptor。 */
struct ipi_descs {
	struct irq_desc *descs[MAX_IPI];
};

/*
 * IRQ descriptor 是 IRQ 子系统拥有的长期对象，这里只缓存借用指针，不获取
 * 或释放 descriptor。READ_MOSTLY 反映它们初始化后主要在发 IPI 路径读取。
 */
static DEFINE_PER_CPU_READ_MOSTLY(struct ipi_descs, pcpu_ipi_desc);

/*
 * 将逻辑 CPU 和 IPI 类型映射为对应的 descriptor 槽位。__cpu 是逻辑编号，
 * __ipi 是 [0, MAX_IPI) 消息槽；per_cpu_ptr() 选择目标 CPU 的 ipi_descs，
 * 最终表达式是可读写左值，因此初始化路径可赋值、发送路径可取得
 * 借用指针。
 * 宏不取得 irq_desc 引用，调用者必须依赖 IRQ 子系统的静态注册生命周期。
 */
#define get_ipi_desc(__cpu, __ipi) (per_cpu_ptr(&pcpu_ipi_desc, __cpu)->descs[__ipi])

/*
 * false 表示 GIC SGI 的共享 per-CPU IRQ descriptor 模型；true 表示为各 CPU
 * 分配独立 LPI descriptor。该选择在 set_smp_ipi_range_percpu() 后只读。
 */
static bool percpu_ipi_descs __ro_after_init;

/*
 * panic/kexec 停机模式标志。设置后 STOP IPI 的接收者会保存 crash CPU 状态，
 * 而不是仅退出在线集合并停放。panic 路径按一次性、尽力而为的协议
 * 访问它。
 */
static bool crash_stop;

/* IPI 生命周期 helper：CPU 上线时 enable，热下线时对称 disable。 */
static void ipi_setup(int cpu);

#ifdef CONFIG_HOTPLUG_CPU
static void ipi_teardown(int cpu);
static int op_cpu_kill(unsigned int cpu);
#else
/*
 * op_cpu_kill - 无 CPU hotplug 构建中的固件死亡确认桩。
 *
 * 调用者：__cpu_up() 的早期启动失败诊断。
 * 入参：cpu 是待确认的 Linux 逻辑 CPU 号，无引用或所有权转移。
 * 上下文：启动控制线程，可睡眠，但本桩不睡眠、不访问硬件。
 * 返回：固定 -ENOSYS，表示本配置没有死亡确认能力；无其他副作用。
 */
static inline int op_cpu_kill(unsigned int cpu)
{
	/* 未编译热插拔支持时，没有可调用的固件“确认已死亡”操作。 */
	return -ENOSYS;
}
#endif


/*
 * Boot a secondary CPU, and assign it the specified idle task.
 * This also gives us the initial stack to use for this CPU.
 */
/*
 * 这里是固件启动机制的薄分派层。idle 已由上层写入 secondary_data，
 * 具体后端只需令目标 CPU 开始执行；返回 0 仅代表启动请求已被接受，
 * 真正上线仍由 __cpu_up() 等待 cpu_running。当前实现不直接使用 idle，
 * 参数保留了通用启动接口中“已为该 CPU 准备 idle 线程”的契约。
 *
 * 调用者：__cpu_up()。cpu 是 possible/present 的 Linux 逻辑编号；idle 是
 * 借用的非 NULL 目标 CPU idle task，调用前已存入 secondary_data，
 * 函数不取得引用。
 * 上下文：CPU hotplug 控制线程，未持有本文件私锁；后端可能进入固件
 * 并失败。
 * 返回：0、后端 errno，或缺少 cpu_boot 时的 -EOPNOTSUPP；无输出参数。
 */
static int boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	/*
	 * ops 在枚举阶段按 enable-method 绑定，生命周期覆盖整个系统
	 * 运行期。
	 */
	if (ops->cpu_boot)
		return ops->cpu_boot(cpu);

	/* 后端没有启动回调时，CPU 不能从 possible 状态推进到 online。 */
	return -EOPNOTSUPP;
}

/*
 * 启动 CPU 等待、次级 CPU完成的单次 bring-up 握手对象。CPU hotplug 核心
 * 串行启动请求；completion 被完成后可由后续一次等待重新消费，无需
 * 为每个 CPU 分配独立对象。
 */
static DECLARE_COMPLETION(cpu_running);

/*
 * __cpu_up - 启动一个已准备好的 possible/present CPU，并等待其上线。
 *
 * cpu 是 Linux 逻辑编号；idle 是该 CPU 专属且已初始化的 idle task，调用者
 * 在本函数返回前保持其有效。CPU hotplug 核心在可睡眠、串行上下文调用。
 *
 * 本函数先发布 secondary_data 和初始 CPU_MMU_OFF 状态，再调用固件后端；
 * 次级 CPU 成功完成初始化后设置 online 并 complete(cpu_running)。等待最多
 * 五秒。固件调用失败直接返回其错误；超时则根据早期 boot status 区分固件
 * 关闭、内核中卡死、地址空间不兼容或致命配置错误。
 *
 * 返回：0 保证 cpu_online(cpu) 已成立；固件错误原样返回；
 * -EIO 表示请求已发出但五秒内未上线；CPU_PANIC_KERNEL 不返回而触发 panic。
 * 副作用：更新 secondary_data、可能启动并发 CPU、更新 stuck 计数。
 * 下一步：成功后通用 CPU hotplug 继续完成 ONLINE 状态；失败由其回滚。
 */
int __cpu_up(unsigned int cpu, struct task_struct *idle)
{
	int ret;
	long status;

	/*
	 * We need to tell the secondary core where to find its stack and the
	 * page tables.
	 */
	/*
	 * 完整含义：次级核心尚不能通过普通调度状态寻找自己的执行
	 * 环境，
	 * 启动端必须预先告诉它初始栈和页表位于何处。
	 *
	 * secondary_data 是共享单槽，先写 task 再发布启动状态。idle task 同时
	 * 提供次级 CPU 的初始内核栈；update_cpu_boot_status() 负责按早期启动
	 * 契约更新可被 MMU 外代码观察的状态。
	 */
	secondary_data.task = idle;
	update_cpu_boot_status(CPU_MMU_OFF);

	/* Now bring the CPU into our world */
	/* 此调用跨越固件/硬件边界；成功后目标 CPU 可能立即并发执行。 */
	ret = boot_secondary(cpu, idle);
	if (ret) {
		/*
		 * -EPERM 常用于固件策略性拒绝，避免重复打印通用错误；
		 * 无论何种失败，次级 CPU 都未完成 online 握手，错误原样
		 * 交给 hotplug 核心。
		 */
		if (ret != -EPERM)
			pr_err("CPU%u: failed to boot: %d\n", cpu, ret);
		return ret;
	}

	/*
	 * CPU was successfully started, wait for it to come online or
	 * time out.
	 */
	/*
	 * completion 由目标 CPU 在所有关键本地初始化完成、设置 online 之后
	 * 唤醒。超时返回值无需保留，因为 cpu_online() 才是最终状态判据：
	 * 即使五秒边界附近发生完成，也不会误报已经 online 的 CPU。
	 */
	wait_for_completion_timeout(&cpu_running,
				    msecs_to_jiffies(5000));
	if (cpu_online(cpu))
		return 0;

	pr_crit("CPU%u: failed to come online\n", cpu);
	/*
	 * 清除共享 task，避免失败 CPU 或后续诊断把旧 idle task 当作新
	 * 启动参数。若目标 CPU 仍在内核中执行，下面只做状态分类，不回收
	 * 它正在使用的内存。
	 */
	secondary_data.task = NULL;
	/*
	 * status 可由 MMU 开启后的 C 路径更新；若仍是初值，则故障发生在
	 * 更早的
	 * MMU-off 阶段，需要读取专供早期汇编发布的 __early_cpu_boot_status。
	 * READ_ONCE 防止编译器合并跨异步 CPU 更新的读取。
	 */
	status = READ_ONCE(secondary_data.status);
	if (status == CPU_MMU_OFF)
		status = READ_ONCE(__early_cpu_boot_status);

	/* 低位是主状态，高位保留“为什么卡住”等原因位。 */
	switch (status & CPU_BOOT_STATUS_MASK) {
	default:
		/*
		 * 未识别状态意味着无法证明 CPU 已退出内核，必须计入
		 * stuck。
		 */
		pr_err("CPU%u: failed in unknown state : 0x%lx\n",
		       cpu, status);
		cpus_stuck_in_kernel++;
		break;
	case CPU_KILL_ME:
		/*
		 * 目标 CPU 已请求由控制 CPU/固件完成关闭。cpu_kill() 返回 0
		 * 表示已确认死亡；失败则按仍可能执行内核代码处理。
		 */
		if (!op_cpu_kill(cpu)) {
			pr_crit("CPU%u: died during early boot\n", cpu);
			break;
		}
		pr_crit("CPU%u: may not have shut down cleanly\n", cpu);
		fallthrough;
	case CPU_STUCK_IN_KERNEL:
		/*
		 * CPU 已进入内核但无法继续或关闭，记录可诊断的体系结构
		 * 原因。
		 */
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
		/* 目标 CPU 发现系统级不兼容，继续运行其他 CPU 已不安全。 */
		panic("CPU%u detected unsupported configuration\n", cpu);
	}

	/*
	 * 所有非 panic 的“请求已发出但未上线”情形对调用者统一表现为
	 * I/O 失败。
	 */
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
/*
 * 补充说明：入参：无。上下文：当前 CPU 的 DAIF.I/F 均已屏蔽，
 * 不可睡眠。返回：无直接返回值；成功时本地 ICC_PMR_EL1 建立 PMR
 * 屏蔽初值，SRE 不可用时仅 WARN 并保持原状态。只修改当前 CPU 的
 * GIC 系统寄存器。
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
/*
 * secondary_start_kernel - 次级 CPU 从早期汇编进入通用内核的首个 C 入口。
 *
 * 此时当前任务已是启动 CPU 为本 CPU 准备的 idle task，内核栈可用，但仍
 * 使用临时页表，所有 DAIF 异常均被屏蔽。函数依次接管 init_mm、撤销恒等
 * 映射、校验系统能力、完成固件 postboot、发布 CPU 信息，最后建立中断、
 * NUMA、online 状态并进入 idle 循环。函数不返回。
 *
 * online 是对其他内核子系统的正式发布点；在此之前失败会进入
 * cpu_die_early()，不能让调度器或普通跨 CPU 操作看到半初始化 CPU。
 *
 * 调用者：secondary_entry/secondary_startup 的汇编尾端。
 * 入参和返回：无入参，永不返回。上下文：目标 CPU、临时页表、DAIF
 * 全屏蔽，不持有普通内核锁；进入 idle 前的初始化可能调用 CPUHP
 * starting 回调。
 * 副作用：取得 init_mm 引用，发布 cpuinfo/topology/NUMA/online 状态并完成
 * cpu_running；成功后的唯一去向是 cpu_startup_entry()。
 */
asmlinkage notrace void secondary_start_kernel(void)
{
	/*
	 * 变量地图：
	 *   mpidr  当前硬件 CPU 的亲和 ID，仅在上线日志阶段使用；
	 *   mm     指向全局 init_mm；本函数取得引用并交给 idle task 的 active_mm；
	 *   ops    当前 CPU 的静态启动操作表借用指针，用于 postboot；
	 *   cpu    当前 Linux 逻辑编号，索引全部 per-CPU 状态。
	 */
	/* mpidr 仅用于上线日志；cpu 是早期映射已经确定的 Linux 逻辑编号。 */
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	struct mm_struct *mm = &init_mm;
	const struct cpu_operations *ops;
	unsigned int cpu = smp_processor_id();

	/*
	 * All kernel threads share the same mm context; grab a
	 * reference and switch to it.
	 */
	/*
	 * idle 是内核线程，没有用户 mm。为 init_mm 增加 mm_count 引用并记为
	 * active_mm，使上下文切换和 idle_task_exit() 遵循借用 active_mm 的
	 * 通用内核线程生命周期。
	 */
	mmgrab(mm);
	current->active_mm = mm;

	/*
	 * TTBR0 is only used for the identity mapping at this stage. Make it
	 * point to zero page to avoid speculatively fetching new entries.
	 */
	/*
	 * 早期恒等映射的任务已经完成。把 TTBR0 指向零页可阻止处理器
	 * 继续投机
	 * 遍历旧 idmap；之后只依赖正式内核页表。
	 */
	cpu_uninstall_idmap();

	/* 每个 CPU 都有独立 GIC CPU interface，必须在本地初始化 PMR。 */
	if (system_uses_irq_prio_masking())
		init_gic_priority_masking();

	/*
	 * 把当前 CPU 加入 RCU 的启动状态机；硬中断跟踪仍标记为关闭，直到
	 * local_daif_restore() 真正开放异常。
	 */
	rcutree_report_cpu_starting(cpu);
	trace_hardirqs_off();

	/*
	 * If the system has established the capabilities, make sure
	 * this CPU ticks all of those. If it doesn't, the CPU will
	 * fail to come online.
	 */
	/*
	 * 系统能力已按 boot CPU/已知 CPU 的安全交集确定。当前 CPU 若缺少内核
	 * 已经启用且无法撤销的能力，检查路径会阻止其上线，而不是运行
	 * 异构的
	 * 不安全指令集合。
	 */
	check_local_cpu_capabilities();

	/*
	 * 后端可在通用发布前验证固件交接，例如 parking protocol
	 * 清零信箱。
	 */
	ops = get_cpu_ops(cpu);
	if (ops->cpu_postboot)
		ops->cpu_postboot();

	/*
	 * Log the CPU info before it is marked online and might get read.
	 */
	/* 先填充 per-CPU 信息和拓扑，再通过 online 位允许并发读者发现。 */
	cpuinfo_store_cpu();
	store_cpu_topology(cpu);

	/*
	 * Enable GIC and timers.
	 */
	/*
	 * 执行 CPUHP_AP_IRQ_GIC_STARTING 等 starting 回调，使本地 GIC、timer
	 * 等依赖 CPU 的硬件在 online 发布前可用。
	 */
	notify_cpu_starting(cpu);

	/* 开启本 CPU 的全部 IPI IRQ；此后可接收调度和跨 CPU 请求。 */
	ipi_setup(cpu);

	/* 将 CPU 加入其 NUMA 节点的活动集合，供调度和内存策略使用。 */
	numa_add_cpu(cpu);

	/*
	 * OK, now it's safe to let the boot CPU continue.  Wait for
	 * the CPU migration code to notice that the CPU is online
	 * before we continue.
	 */
	/*
	 * 完整含义：本地初始化已足以让 boot CPU 继续；当前次级 CPU 在随后
	 * 进入 CPUHP idle 状态机，由 CPU migration/hotplug 代码确认它 online
	 * 后再继续。
	 *
	 * 发布顺序很重要：先报告启动成功，再设置 cpu_online_mask，最后
	 * complete。启动 CPU 被唤醒后看到 online=true，即可确认前述本地状态
	 * 均已建立。
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
	/*
	 * 完整含义：次级 CPU 带着 Debug、SError、IRQ、FIQ 全部屏蔽进入内核；
	 * 与 setup_arch() 一样必须开放 Debug 和 SError。根 irqchip 已完成检测
	 * 和初始化，所以这里还可同时开放 IRQ 与 FIQ。
	 *
	 * DAIF_PROCCTX 恢复普通进程上下文允许的异常集合。必须晚于 GIC、
	 * timer、IPI 和 online 发布，否则中断处理可能观察到未完成初始化的
	 * CPU。
	 */
	local_daif_restore(DAIF_PROCCTX);

	/*
	 * OK, it's off to the idle thread for us
	 */
	/* 将控制权交给 CPU hotplug/调度器的 idle 状态机，此调用不返回。 */
	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * 在目标 CPU 跨过不可返回点前，向具体启动后端询问能否下线。
 *
 * cpu_die 是最终停机能力的最低要求；可选 cpu_disable 用于 PSCI 等后端的
 * 额外策略检查。任何错误都发生在 online 位清除之前，hotplug 核心仍可
 * 安全中止并让 CPU 继续工作。
 *
 * cpu 是当前待下线 CPU 的逻辑编号，无 ownership 转移。调用者为
 * __cpu_disable()，运行在目标 CPU 的 hotplug 线程，可由后端返回 errno。
 * 返回 0 表示允许跨越下线边界，-EOPNOTSUPP 或后端错误保证状态尚未摘除。
 */
static int op_cpu_disable(unsigned int cpu)
{
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	/*
	 * If we don't have a cpu_die method, abort before we reach the point
	 * of no return. CPU0 may not have an cpu_ops, so test for it.
	 */
	/*
	 * 完整含义：缺少 cpu_die 就必须在不可回滚点之前中止；CPU0 可能没有
	 * cpu_ops，因此也必须先检查 ops 本身，不能直接解引用。
	 */
	if (!ops || !ops->cpu_die)
		return -EOPNOTSUPP;

	/*
	 * We may need to abort a hot unplug for some other mechanism-specific
	 * reason.
	 */
	/*
	 * 即使能够最终关机，具体机制仍可能因固件策略或平台状态拒绝
	 * 本次热下线；可选 cpu_disable 在任何架构状态摘除之前完成检查。
	 */
	if (ops->cpu_disable)
		return ops->cpu_disable(cpu);

	return 0;
}

/*
 * __cpu_disable runs on the processor to be shutdown.
 */
/*
 * __cpu_disable - 在待下线 CPU 自身执行架构不可返回点之前的清理。
 *
 * 成功返回 0 后，该 CPU 已从 topology、NUMA 和 online 集合摘除，IPI 已
 * 禁用且 IRQ 已迁走；调用者只能继续进入 cpu_die()，不能恢复普通调度。
 * 后端拒绝时不改变任何可见状态，错误交给 CPU hotplug 核心回滚。
 *
 * 入参：无，目标由 smp_processor_id() 取得。上下文：待下线 CPU 的
 * CPU hotplug 线程，可执行 IRQ 迁移；入口仍 online。返回 0 或后端 errno，
 * 无输出参数；成功后的下一步是 cpuhp teardown 与 cpu_die()。
 */
int __cpu_disable(void)
{
	unsigned int cpu = smp_processor_id();
	int ret;

	/* 所有仍可失败的后端检查必须位于状态摘除之前。 */
	ret = op_cpu_disable(cpu);
	if (ret)
		return ret;

	/*
	 * 先撤销调度器使用的拓扑/NUMA 描述，避免后续放置任务到
	 * 待死 CPU。
	 */
	remove_cpu_topology(cpu);
	numa_remove_cpu(cpu);

	/*
	 * Take this CPU offline.  Once we clear this, we can't return,
	 * and we must not schedule until we're ready to give up the cpu.
	 */
	/* online=false 是架构路径的不可返回点，向跨 CPU 发送者隐藏该 CPU。 */
	set_cpu_online(cpu, false);
	/* 对称关闭上线时启用的本地 IPI，阻止新的普通跨 CPU 工作到达。 */
	ipi_teardown(cpu);

	/*
	 * OK - migrate IRQs away from this CPU
	 */
	/*
	 * 把普通设备 IRQ 的亲和性迁往其他在线 CPU，函数可能涉及
	 * irqdesc 锁。
	 */
	irq_migrate_all_off_this_cpu();

	return 0;
}

/*
 * 请求后端确认目标 CPU 已经离开内核执行环境。
 *
 * 该函数在控制 CPU 上调用。没有 cpu_kill 回调时只能乐观假设目标已死亡；
 * 有回调时返回值采用后端约定，0 表示确认成功，非零表示无法确认。
 * cpu 是待确认的逻辑编号，无引用转移。正常调用上下文为 hotplug
 * 控制线程，后端可能轮询固件状态；返回值不改变目标 CPU 的 online 状态。
 */
static int op_cpu_kill(unsigned int cpu)
{
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	/*
	 * If we have no means of synchronising with the dying CPU, then assume
	 * that it is really dead. We can only wait for an arbitrary length of
	 * time and hope that it's dead, so let's skip the wait and just hope.
	 */
	/*
	 * 完整含义：没有 cpu_kill 就没有与待死 CPU 同步的可靠手段；
	 * 任意固定等待时间仍只能“希望”它已停止，因此当前实现干脆
	 * 不等待并按成功处理。
	 * 这是无法证明物理死亡时的兼容性退化，不是硬件确认。
	 */
	if (!ops->cpu_kill)
		return 0;

	return ops->cpu_kill(cpu);
}

/*
 * Called on the thread which is asking for a CPU to be shutdown after the
 * shutdown completed.
 */
/*
 * 此时目标 CPU 已通过 cpuhp_ap_report_dead() 完成内核侧死亡握手。控制 CPU
 * 再请求固件确认物理执行已经停止，之后上层才可安全复用其资源。
 * 确认失败只能告警：内核状态已经跨过不可回滚点。
 *
 * cpu 是已死亡的逻辑编号。调用者为 CPU hotplug boot-processor 侧清理，
 * 可睡眠且不持有目标 CPU 的资源所有权。返回：无直接返回值；
 * 唯一副作用是固件确认及失败告警，后续由 hotplug 核心释放通用资源。
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
	/*
	 * 目标 CPU 已越过内核同步的不可返回点；在覆盖它可能仍使用的
	 * 任何资源之前，请固件协助验证它确实已经离开内核执行环境。
	 */
	err = op_cpu_kill(cpu);
	if (err)
		pr_warn("CPU%d may not have shut down cleanly: %d\n", cpu, err);
}

/*
 * Called from the idle thread for the CPU which has been shutdown.
 *
 */
/*
 * cpu_die - 待下线 CPU 的最终、不可返回路径。
 *
 * 释放 idle 线程借用的 active_mm，屏蔽全部异常，向 hotplug 控制端报告
 * “可以处置”，然后调用固件后端关闭当前 CPU。cpu_die 回调按契约不得
 * 返回；若返回说明硬件可能继续执行已被上层视为死亡的 CPU，因此以
 * BUG 终止。
 *
 * 入参和返回：无入参，永不返回。上下文：待死 CPU 的 idle 线程，调度已
 * 禁止；函数自行屏蔽 DAIF。副作用是释放 active_mm 借用、报告 AP dead，
 * 并把物理关闭责任移交 cpu_operations::cpu_die。
 */
void __noreturn cpu_die(void)
{
	unsigned int cpu = smp_processor_id();
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	/* 与 secondary_start_kernel() 中 mmgrab(init_mm) 的 active_mm 生命周期配对。 */
	idle_task_exit();

	/* 死亡握手之后不允许再处理 IRQ、FIQ、SError 或 Debug 异常。 */
	local_daif_mask();

	/* Tell cpuhp_bp_sync_dead() that this CPU is now safe to dispose of */
	/*
	 * 这是与控制 CPU 的同步点；报告之后当前 CPU 不得再访问可能由
	 * 控制端
	 * 回收的 per-CPU/hotplug 资源。
	 */
	cpuhp_ap_report_dead();

	/*
	 * Actually shutdown the CPU. This must never fail. The specific hotplug
	 * mechanism must perform all required cache maintenance to ensure that
	 * no dirty lines are lost in the process of shutting down the CPU.
	 */
	/* 后端负责 PSCI CPU_OFF 等硬件动作及其要求的最后缓存维护。 */
	ops->cpu_die(cpu);

	BUG();
}
#endif

/*
 * 在正常热下线或早期启动失败中，尽力调用当前 CPU 的后端停机入口。
 * 未启用 HOTPLUG_CPU 或后端不支持 cpu_die 时为空操作，调用者必须准备
 * 退化为在内核代码中的永久 park loop。
 *
 * cpu 是当前执行 CPU 的逻辑编号。调用者为 crash/早期失败路径；不可睡眠。
 * 返回：仅在没有可用回调时返回，无状态保证；回调存在时按契约
 * 永不返回。
 */
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
/*
 * cpu_die_early - 次级 CPU 在 online 发布前发现不兼容时的终止路径。
 *
 * CPU 从 present 集合移除并向 RCU 报告死亡；若有 hotplug 后端，则先发布
 * CPU_KILL_ME 让启动 CPU 可协助确认关闭。后端不能关机时改发
 * CPU_STUCK_IN_KERNEL，并永久停在 cpu_park_loop()，使控制 CPU 不会误判
 * 它已经离开内核。
 *
 * 入参和返回：无入参，永不返回。调用者是本地 capability/启动失败路径；
 * 当前 CPU 尚未 online、DAIF 仍受早期启动约束，不可睡眠。
 */
void __noreturn cpu_die_early(void)
{
	int cpu = smp_processor_id();

	pr_crit("CPU%d: will not boot\n", cpu);

	/* Mark this CPU absent */
	/* 尚未 online，直接撤销 present 即可阻止后续再次尝试使用该 CPU。 */
	set_cpu_present(cpu, 0);
	rcutree_report_cpu_dead();

	if (IS_ENABLED(CONFIG_HOTPLUG_CPU)) {
		/* 先发布状态，再跨入可能不返回的固件 CPU_OFF。 */
		update_cpu_boot_status(CPU_KILL_ME);
		__cpu_try_die(cpu);
	}

	/*
	 * 走到这里说明无法物理关闭，明确告诉启动 CPU 该 CPU 仍在
	 * 内核地址中。
	 */
	update_cpu_boot_status(CPU_STUCK_IN_KERNEL);

	cpu_park_loop();
}

/*
 * 汇总所有 CPU 的启动异常级别，并在 SMP 枚举完成后建立 KVM hyp 布局。
 * CPU 必须一致地从 EL1 或 EL2 启动；不一致会污染系统可信度。若 KVM 已
 * 编译但宿主内核不驻留 EL2，则计算并重定位独立的 hypervisor 地址空间。
 *
 * 调用者：smp_cpus_done()。入参：无。__init 启动 CPU 上下文，可调用
 * KVM 初始化 helper；返回：无直接返回值，副作用为日志、taint 或 hyp 布局。
 */
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

/*
 * smp_cpus_done - 所有允许启动的 CPU 尝试完成后的系统级收尾。
 *
 * 此时 CPU 特性集合不再扩大，可以建立 system/user feature 最终视图，并
 * 把内核线性别名中的 text 标为只读。__init 表明初始化后代码可被回收。
 * max_cpus 是通用 SMP 初始化回调传入的启动上限；arm64 在更早阶段已经
 * 应用该限制，本收尾函数无需再次读取它。
 * 上下文：启动 CPU 的 __init 阶段，可睡眠；返回：无直接返回值。
 * 副作用是最终发布 system/user feature，并收紧 text 线性别名权限。
 */
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
/*
 * 补充说明：入参：无。调用者为启动主线，运行在 CPU0、尚无次级 CPU
 * 并发的 __init 上下文，可执行初始化 helper。返回：无直接返回值；
 * 副作用包括切换 per-CPU 基址、发布 boot CPU 特性并初始化本地
 * GIC/KASAN 状态。
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
/*
 * 在把新硬件 ID 发布到 cpu_logical_map 前检查唯一性。MPIDR 是固件启动、
 * IPI 路由和 per-CPU 身份的共同键；重复映射会让两个逻辑 CPU 指向同一
 * 处理器。CPU0 已预先建立映射，因此从逻辑 CPU1 扫描到当前候选项之前。
 *
 * cpu 是候选逻辑槽上界，hwid 是已掩码的 MPIDR 值；均为纯输入。
 * __init 单 CPU 枚举上下文，无锁且不睡眠。返回 true 表示已有相同 MPIDR，
 * false 表示当前已初始化范围内唯一；不修改映射。
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
/*
 * smp_cpu_setup - 为一个已建立 MPIDR 映射的逻辑 CPU 绑定启动后端。
 *
 * init_cpu_ops() 根据 DT enable-method 或 ACPI 启动方法选择长期 ops；
 * cpu_init() 再解析后端的逐 CPU 数据。两个阶段都成功后才设置 possible，
 * 表示内核具备管理该 CPU 的完整软件资源和启动方法。失败不做部分
 * 发布，调用者随后会把 cpu_logical_map 恢复为 INVALID_HWID。
 *
 * cpu 是已分配 MPIDR 的逻辑编号。调用者为 smp_init_cpus()，处于 boot CPU
 * __init 上下文。返回 0 并发布 possible，或 -ENODEV 且不发布 possible；
 * ops 指针为静态借用，无资源转移。
 */
static int __init smp_cpu_setup(int cpu)
{
	const struct cpu_operations *ops;

	/* 绑定失败时没有可安全调用的后端，统一转换为 CPU 不可用。 */
	if (init_cpu_ops(cpu))
		return -ENODEV;

	ops = get_cpu_ops(cpu);
	/* 后端初始化可能解析 release 地址或建立固件协议的逐 CPU 状态。 */
	if (ops->cpu_init(cpu))
		return -ENODEV;

	/*
	 * possible 是成功初始化的发布点，不代表 CPU 已物理存在或
	 * 已经在线。
	 */
	set_cpu_possible(cpu, true);

	return 0;
}

/*
 * 枚举期全局状态，只在 boot CPU 的 __init 路径读写：
 *
 * bootcpu_valid 表示固件描述中恰好找到了运行当前内核的 MPIDR；
 * cpu_count 是下一个候选逻辑编号/已遍历 CPU 数的计数器，初值 1 为 CPU0
 * 预留。初始化结束后这两个值不参与运行时并发。
 */
static bool bootcpu_valid __initdata;
static unsigned int cpu_count = 1;

/*
 * arch_register_cpu - 把架构 CPU 对象注册到通用 CPU 设备模型。
 *
 * ACPI 热插拔配置下，processor handle 可能尚未建立，返回 -EPROBE_DEFER
 * 让设备注册稍后重试。成功时 register_cpu() 发布 sysfs CPU 设备；对象是
 * 静态 per-CPU cpu_devices，不在这里分配或释放。
 *
 * cpu 是待注册逻辑编号。设备核心的注册线程上下文，可睡眠；
 * 无输入指针。
 * 返回 0、-EPROBE_DEFER、-ENODEV 或 register_cpu() errno。失败时静态
 * cpu_devices 仍归架构层且未发布，调用者决定是否重试。
 */
int arch_register_cpu(int cpu)
{
	/*
	 * acpi_handle 是 ACPI namespace 中 processor 对象的借用句柄；
	 * c 指向静态 per-CPU cpu_devices 槽，由设备核心注册但不转移
	 * 存储所有权。
	 */
	acpi_handle acpi_handle = acpi_get_processor_handle(cpu);
	struct cpu *c = &per_cpu(cpu_devices, cpu);

	if (!acpi_disabled && !acpi_handle &&
	    IS_ENABLED(CONFIG_ACPI_HOTPLUG_CPU))
		return -EPROBE_DEFER;

#ifdef CONFIG_ACPI_HOTPLUG_CPU
	/* For now block anything that looks like physical CPU Hotplug */
	/*
	 * arm64 当前只支持已枚举 CPU 的逻辑 online/offline，不支持通过 _STA
	 * 改变物理 present 集合；非法编号或 absent CPU 因而不能注册。
	 */
	if (invalid_logical_cpuid(cpu) || !cpu_present(cpu)) {
		pr_err_once("Changing CPU present bit is not supported\n");
		return -ENODEV;
	}
#endif

	/*
	 * Availability of the acpi handle is sufficient to establish
	 * that _STA has already been checked. No need to recheck here.
	 */
	/*
	 * 完整含义：能够取得 ACPI processor handle 本身就证明此前已检查 _STA，
	 * 因此这里无需重复求值固件方法。避免重复固件调用也保持注册时
	 * 看到的 present 判断与前序枚举一致。
	 */
	/* hotpluggable 只描述能否逻辑下线，最终能力仍由 cpu_operations 决定。 */
	c->hotpluggable = arch_cpu_is_hotpluggable(cpu);

	return register_cpu(c, cpu);
}

#ifdef CONFIG_ACPI_HOTPLUG_CPU
/*
 * 从通用 CPU 设备模型撤销 sysfs 设备。先读取 _STA 只为检测固件试图改变
 * present 位的未支持情形；无论该诊断是否成功，已注册设备都由
 * unregister_cpu() 摘除。
 *
 * cpu 是已注册逻辑编号。调用者为 ACPI CPU 设备移除路径，可睡眠。
 * 返回：无直接返回值；副作用是注销静态 cpu_devices 对应的设备，
 * ACPI handle 是借用值，不在这里释放。
 */
void arch_unregister_cpu(int cpu)
{
	/*
	 * acpi_handle/c 均为借用；sta 接收 _STA 位图，status 仅描述本次 AML
	 * 求值是否成功，求值失败不阻止注销既有 CPU 设备。
	 */
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
/*
 * 每个逻辑 CPU 对应的 MADT GICC 描述副本。ACPI 表本体可能位于初始化期
 * 映射中，因此枚举时复制完整结构；数组静态存活，可安全借给后续
 * ACPI、NUMA 和 parking protocol 查询者。
 */
static struct acpi_madt_generic_interrupt cpu_madt_gicc[NR_CPUS];

/*
 * acpi_cpu_get_madt_gicc - 取得一个逻辑 CPU 的长期 MADT GICC 副本。
 *
 * cpu 必须位于已分配 per-CPU 范围。任意允许查询 ACPI CPU 描述的上下文
 * 均可调用，不睡眠。返回非 NULL 借用指针，无引用增加、无副作用；
 * 调用者不得释放或越过数组边界。
 */
struct acpi_madt_generic_interrupt *acpi_cpu_get_madt_gicc(int cpu)
{
	return &cpu_madt_gicc[cpu];
}
EXPORT_SYMBOL_GPL(acpi_cpu_get_madt_gicc);

/*
 * acpi_cpu_is_present - 查询 MADT 是否把 CPU 标为当前存在。
 *
 * cpu 是逻辑编号；函数只读取静态 GICC 副本，不睡眠、不转移所有权。
 * 返回 ENABLED 位的布尔值，无副作用；由 smp_prepare_cpus() 决定是否发布
 * present mask。
 */
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
/*
 * acpi_map_gic_cpu_interface - 校验并接纳一条 MADT GICC CPU 描述。
 *
 * processor 是 ACPI 解析器借出的表项，本函数复制需要长期保存的数据。
 * 禁用且不可 online、MPIDR 越界/保留、MPIDR 重复的条目均被忽略。boot CPU
 * 只验证并保存到槽 0；其他 CPU 依次分配逻辑编号，同时准备 parking
 * protocol 数据。函数无错误返回，拒绝原因通过日志和未增长的
 * cpu_count 表示。
 *
 * processor 是 ACPI walker 借出的非 NULL 纯输入指针，仅在调用期间有效。
 * __init boot CPU 上下文，无锁；返回：无直接返回值。成功副作用是更新
 * logical map、GICC/parking 表和 cpu_count，拒绝时除日志外不发布状态。
 */
static void __init
acpi_map_gic_cpu_interface(struct acpi_madt_generic_interrupt *processor)
{
	/* hwid 是从借用 GICC 表项复制出的 MPIDR，在本函数调用期间有效。 */
	u64 hwid = processor->arm_mpidr;

	/*
	 * ONLINE_CAPABLE 允许当前 disabled、但固件声明可由操作系统上线的 CPU
	 * 参与枚举；两位都没有的条目没有可用生命周期。
	 */
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
	/*
	 * 逻辑 CPU0 的 MPIDR 已由早期启动代码取得，MADT 必须包含且只能包含
	 * 一条匹配记录；不能按遍历顺序重新编号 boot CPU。
	 */
	if (cpu_logical_map(0) == hwid) {
		if (bootcpu_valid) {
			pr_err("duplicate boot CPU MPIDR: 0x%llx in MADT\n",
			       hwid);
			return;
		}
		bootcpu_valid = true;
		/* 结构体值复制后不依赖 ACPI 原表映射的生命周期。 */
		cpu_madt_gicc[0] = *processor;
		return;
	}

	/* 超出编译期 NR_CPUS 容量的固件条目无法建立 per-CPU 状态。 */
	if (cpu_count >= NR_CPUS)
		return;

	/* map the logical cpu id to cpu MPIDR */
	/* 先发布硬件映射，后续 cpu_ops 初始化会用它反查本 GICC 表项。 */
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
	/*
	 * 同次 MADT 遍历复制 parking 信箱元数据，使一个有效逻辑映射与它的
	 * enable-method 数据保持同槽对应，避免后续重新扫描 ACPI 表。
	 */
	acpi_set_mailbox_entry(cpu_count, processor);

	cpu_count++;
}

/*
 * MADT 子表解析适配器。end 用于验证变长 ACPI 表项没有越过表尾；
 * 格式错误返回 -EINVAL 给 ACPI walker，合法表项交给上面的接纳逻辑。
 * header 是 walker 借用的非 NULL 输入表项，end 是表映射末地址而非长度。
 * __init 同步解析上下文；返回 0 或 -EINVAL，无 ownership 转移。
 */
static int __init
acpi_parse_gic_cpu_interface(union acpi_subtable_headers *header,
			     const unsigned long end)
{
	/*
	 * processor 是对已通过 end 边界校验的同一 ACPI 表内存的
	 * 类型化借用。
	 */
	struct acpi_madt_generic_interrupt *processor;

	processor = (struct acpi_madt_generic_interrupt *)header;
	/* 在强制转换后、读取字段前完成长度和边界检查。 */
	if (BAD_MADT_GICC_ENTRY(processor, end))
		return -EINVAL;

	acpi_table_print_madt_entry(&header->common);

	acpi_map_gic_cpu_interface(processor);

	return 0;
}

/*
 * 完成 ACPI CPU 枚举和 NUMA 早期映射。
 *
 * MADT 提供 MPIDR/逻辑 CPU，SRAT 提供 proximity domain；必须先建立逻辑
 * 映射，才能把 SRAT 结果写入对应 CPU 的 early node 表。
 *
 * 入参：无。调用者为 smp_init_cpus()，boot CPU 的 __init 上下文，ACPI
 * table walker 可能执行映射操作。返回：无直接返回值；副作用是建立逻辑
 * CPU、GICC、parking mailbox 和 early NUMA 映射。
 */
static void __init acpi_parse_and_init_cpus(void)
{
	/* i 遍历最终 nr_cpu_ids 范围，把 ACPI NUMA 结果复制到 early map。 */
	int i;

	/*
	 * do a walk of MADT to determine how many CPUs
	 * we have including disabled CPUs, and get information
	 * we need for SMP init.
	 */
	/*
	 * 完整含义：遍历 MADT 统计包括 disabled 条目在内的 CPU，并取得 SMP
	 * 初始化所需信息。解析器同步调用回调，回调只复制数据，不持有
	 * header 指针。
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
	/*
	 * 完整含义：ACPI 把 SMP CPU 身份放在 MADT、NUMA 归属放在 SRAT 两张
	 * 独立静态表中；因此先遍历 MADT 建立逻辑 CPU 映射，再单独把
	 * 逻辑 CPU 映射到 node ID，比在两张表之间交叉解析更简单。
	 */
	acpi_map_cpus_to_nodes();

	/* nr_cpu_ids 限定运行内核实际支持的逻辑编号范围。 */
	for (i = 0; i < nr_cpu_ids; i++)
		early_map_cpu_to_node(i, acpi_numa_get_nid(i));
}
#else
/*
 * acpi_cpu_is_present - 无 ACPI 构建的查询桩。
 *
 * cpu 参数不被使用且无所有权。任意上下文均不睡眠；固定返回 false，
 * 无副作用，使调用点保持同一控制流而由设备树路径决定 present。
 */
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
/*
 * of_parse_and_init_cpus - 从设备树 CPU 节点建立 MPIDR 和早期 NUMA 映射。
 *
 * for_each_of_cpu_node() 借用遍历节点，循环宏管理引用。每个有效非 boot
 * CPU 按固件遍历顺序获得逻辑编号；无效/重复节点不建立映射，但仍消耗
 * cpu_count 的枚举位置，这是现有编号方案的一部分。
 *
 * 入参：无。调用者为 smp_init_cpus()，boot CPU 的 __init 上下文。
 * device_node 均为遍历期间借用；返回：无直接返回值，副作用是建立逻辑
 * MPIDR、early NUMA 映射并更新 bootcpu_valid/cpu_count。
 */
static void __init of_parse_and_init_cpus(void)
{
	/*
	 * dn 是 for_each_of_cpu_node() 在每轮借出的 CPU 节点，引用由遍历宏管理；
	 * 每轮 hwid 是从 dn 解析出的 MPIDR 值，离开该轮后无需保留。
	 */
	struct device_node *dn;

	for_each_of_cpu_node(dn) {
		/*
		 * reg/架构定义被规范化为 MPIDR 硬件 ID；无效属性通常得到
		 * 异常值。
		 */
		u64 hwid = of_get_cpu_hwid(dn, 0);

		/* 拒绝 MPIDR 允许掩码之外的保留位。 */
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
		/*
		 * boot CPU 必须占逻辑 0；这里只验证固件节点并补充
		 * NUMA 映射。
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
			/*
			 * CPU0 已由早期汇编启动，不依赖本节点的 enable-method；
			 * 同时 cpu_count 的初值已经为它预留槽位，因此直接
			 * 继续。
			 */
			continue;
		}

		/*
		 * 没有静态 per-CPU 容量时跳过映射，但继续统计固件
		 * 节点数量。
		 */
		if (cpu_count >= NR_CPUS)
			goto next;

		pr_debug("cpu logical map 0x%llx\n", hwid);
		/* MPIDR 映射必须先于后续 init_cpu_ops() 对 CPU 节点的反查。 */
		set_cpu_logical_map(cpu_count, hwid);

		/* NUMA 节点号同逻辑 CPU 槽同步发布。 */
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
/*
 * smp_init_cpus - 选择固件来源，形成 possible CPU 集合。
 *
 * 入口时 CPU0 的 MPIDR 已知，其他 cpu_logical_map 槽为 INVALID_HWID。
 * 枚举先建立全部硬件映射，再逐 CPU 绑定 cpu_operations；这种两阶段顺序
 * 允许后端通过 MPIDR 反查 DT/ACPI 描述。失败槽被重新置为 INVALID_HWID，
 * 不会以半初始化状态留在 possible mask。
 *
 * 入参：无。启动 CPU 的 __init 上下文，可由固件解析 helper 分配/映射
 * 临时资源。返回：无直接返回值；失败通过日志及未设置 possible 表达，
 * 后续通常进入 smp_prepare_cpus()。
 */
void __init smp_init_cpus(void)
{
	/* i 在固件枚举全部完成后遍历可配置逻辑槽，逐一绑定启动后端。 */
	int i;

	if (acpi_disabled)
		of_parse_and_init_cpus();
	else
		acpi_parse_and_init_cpus();

	if (!bootcpu_valid) {
		/*
		 * 固件拓扑无法与正在执行的 CPU 对齐，启动任何次级 CPU
		 * 都不安全。
		 */
		pr_err("missing boot CPU MPIDR, not enabling secondaries\n");
		return;
	}

	/*
	 * For the nosmp/maxcpus=0 case, do not mark the secondary CPUs
	 * possible.
	 */
	/* nosmp/maxcpus=0 明确要求保持单核，只保留已经运行的 boot CPU。 */
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
	/*
	 * CPU0 已在线且不需要通过 enable-method 启动，因此从槽 1 开始。
	 * possible 的发布发生在 smp_cpu_setup() 内；失败时撤销硬件身份映射，
	 * 使后续查找不会把该槽误认为有效 CPU。
	 */
	for (i = 1; i < nr_cpu_ids; i++) {
		if (cpu_logical_map(i) != INVALID_HWID) {
			if (smp_cpu_setup(i))
				set_cpu_logical_map(i, INVALID_HWID);
		}
	}
}

/*
 * smp_prepare_cpus - 为 possible CPU 执行启动前协议并形成 present 集合。
 *
 * max_cpus 来自启动参数限制。先建立 boot CPU 的 topology/NUMA 状态，再对
 * 每个次级 CPU 调用后端 cpu_prepare()：PSCI 通常为空操作，spin-table 会
 * 发布 holding-pen 入口。只有准备成功且固件报告物理存在的 CPU 才设置
 * present；prepare 失败保持 possible 但 absent，本轮启动不会选择它。
 *
 * max_cpus 是本次允许上线的最大 CPU 数，0 表示强制单核。启动 CPU 的
 * __init 上下文，后端 prepare 可能映射固件信箱。返回：无直接返回值；
 * 副作用是建立 topology/NUMA 并发布 present mask，后续由通用 SMP 启动。
 */
void __init smp_prepare_cpus(unsigned int max_cpus)
{
	/*
	 * 变量地图：
	 *   ops      当前候选 CPU 的静态操作表借用指针；
	 *   err      cpu_prepare() 的临时 errno，不跨循环迭代保留；
	 *   cpu      possible mask 的遍历编号；
	 *   this_cpu 正在执行初始化的 boot CPU 编号，用于跳过自身。
	 */
	const struct cpu_operations *ops;
	int err;
	unsigned int cpu;
	unsigned int this_cpu;

	/*
	 * 初始化全局拓扑容器，再发布 boot CPU 的本地拓扑与 NUMA
	 * 成员关系。
	 */
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
	/*
	 * 完整含义：初始化描述“当前真正装配 CPU”的 present map，并通过
	 * cpu_prepare() 把次级 CPU 从 bootloader 等待位置释放到内核等待入口。
	 * possible 只表示内核有管理能力；准备完成后才发布 present，形成通用
	 * SMP 启动线程随后可选择的物理 CPU 集合。
	 */
	for_each_possible_cpu(cpu) {

		/* boot CPU 已在运行，不需要也不应再次调用固件准备回调。 */
		if (cpu == smp_processor_id())
			continue;

		ops = get_cpu_ops(cpu);
		/* possible CPU 正常应有 ops；防御性跳过不完整槽。 */
		if (!ops)
			continue;

		err = ops->cpu_prepare(cpu);
		/* 后端准备失败没有通用回滚资源，CPU 保持 absent。 */
		if (err)
			continue;

		if (acpi_disabled || acpi_cpu_is_present(cpu))
			set_cpu_present(cpu, true);
		/*
		 * 即使 ACPI CPU 暂不 present，也缓存其 NUMA 信息供
		 * 设备模型使用。
		 */
		numa_store_cpu_info(cpu);
	}
}

/*
 * IPI 类型编号到 /proc/interrupts 和 tracepoint 可读名称的稳定映射。
 * __tracepoint_string 让追踪基础设施把这些长期字符串作为可识别常量处理。
 * 数组槽必须与 enum ipi_msg_type 保持一致。
 */
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

/* 所有逻辑 IPI 发送接口最终汇聚到这个带 trace 的分派器。 */
static void smp_cross_call(const struct cpumask *target, unsigned int ipinr);

/* IRQ 核心记录的架构级错误中断计数，展示在 /proc/interrupts 的 Err 行。 */
unsigned long irq_err_count;

/*
 * arch_show_interrupts - 向 /proc/interrupts 追加 arm64 IPI 和错误统计。
 *
 * p 是 seq_file 框架拥有的非 NULL 借用指针；prec 是 CPU 列表计算出的
 * 标签宽度。函数只读取
 * online CPU 的 per-CPU irq descriptor 统计，不改变 IRQ 状态。
 * 调用者为通用 /proc/interrupts 展示路径，进程上下文可睡眠。
 * 返回固定 0；写入结果进入 p，p 的 ownership 不变。
 */
int arch_show_interrupts(struct seq_file *p, int prec)
{
	/* cpu 遍历 online 列，i 遍历稳定的 IPI 消息类型槽。 */
	unsigned int cpu, i;

	for (i = 0; i < MAX_IPI; i++) {
		seq_printf(p, "%*s%u: ", prec - 1, "IPI", i);
		/* descriptor 统计按目标 CPU 记账，离线 CPU 不展示。 */
		for_each_online_cpu(cpu)
			seq_printf(p, "%10u ", irq_desc_kstat_cpu(get_ipi_desc(cpu, i), cpu));
		seq_printf(p, " %s\n", ipi_types[i]);
	}

	seq_printf(p, "%*s: %10lu\n", prec, "Err", irq_err_count);
	return 0;
}

/*
 * 通用 smp_call_function_many() 的架构发送钩子。mask 是调用期间借用的
 * 非 NULL 只读指针，
 * 消息到达后由 generic_smp_call_function_interrupt() 消费通用队列。
 * 可在不能睡眠的上下文调用；返回：无直接返回值。副作用是向 mask 的
 * CPU 投递 IPI，mask 和队列对象的 ownership 均不转移。
 */
void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
	smp_cross_call(mask, IPI_CALL_FUNC);
}

/*
 * arch_send_call_function_single_ipi - 投递单 CPU call-function 通知。
 *
 * cpu 是在线目标逻辑编号。调用者为通用 SMP call-function 层，不可假定
 * 可睡眠。返回：无直接返回值；复用静态 cpumask_of()，不转移掩码所有权。
 */
void arch_send_call_function_single_ipi(int cpu)
{
	smp_cross_call(cpumask_of(cpu), IPI_CALL_FUNC);
}

#ifdef CONFIG_IRQ_WORK
/*
 * 通知当前 CPU 执行其 irq_work 队列。即便目标是自身，也通过 IPI 异步进入
 * 中断上下文，满足 irq_work 对执行时机的要求。
 * 入参：无；可从原子上下文调用，不睡眠。返回：无直接返回值，
 * 副作用是投递本地 IPI，队列项仍由 irq_work 子系统管理。
 */
void arch_irq_work_raise(void)
{
	smp_cross_call(cpumask_of(smp_processor_id()), IPI_IRQ_WORK);
}
#endif

/*
 * local_cpu_stop - 把当前 CPU 从在线集合摘除并永久停放。
 *
 * 用于普通 STOP IPI 和并发 panic 的自停路径。online 位先清除，让发送者
 * 的有界等待能够观察进度；随后屏蔽全部异常和本地 SDEI，保证 park loop
 * 不再执行普通内核工作。函数不参与固件 CPU_OFF，CPU 仍可能物理运行。
 * cpu 必须是当前逻辑 CPU。IPI/panic 原子上下文，不可睡眠；永不返回。
 */
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
/*
 * 多个 CPU 同时进入 panic 时，每个非主导 CPU 可走这里主动清除 online 位。
 * 这样主导 CPU 的 smp_send_stop() 不会向已经永久停放的 CPU 发送并等待 IPI。
 * 入参：无。panic 原子上下文，不可睡眠；返回：永不返回，副作用同
 * local_cpu_stop()。
 */
void __noreturn panic_smp_self_stop(void)
{
	local_cpu_stop(smp_processor_id());
}

/*
 * ipi_cpu_crash_stop - 在 crash/kexec 模式停止当前 CPU 并保存寄存器现场。
 *
 * regs 是中断/NMI 入口保存的非 NULL 借用指针，仅在 crash_save_cpu()
 * 调用期间使用。
 * 保存完成后清除 online、屏蔽 SDEI，并尽力通过 hotplug 后端物理关闭；
 * 无法关闭则永久 park。CONFIG_KEXEC_CORE 关闭时到达这里属于逻辑错误。
 * cpu 必须是当前逻辑编号。IRQ/NMI crash 上下文，不可睡眠；永不返回。
 */
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
	/*
	 * DAIF 全屏蔽同时挡住普通 IRQ 和伪 NMI，避免 IRQ 与 NMI 两条 stop 路径
	 * 嵌套保存同一 CPU；已经先到达的一条路径可以完整保留更可信的
	 * pt_regs。
	 */
	local_daif_mask();

	/* 把寄存器现场复制到 crash note，供崩溃内核或 vmcore 使用。 */
	crash_save_cpu(regs, cpu);

	set_cpu_online(cpu, false);

	sdei_mask_local_cpu();

	if (IS_ENABLED(CONFIG_HOTPLUG_CPU))
		__cpu_try_die(cpu);

	/* just in case */
	/* cpu_die 意外返回或没有后端时，绝不能回到被中断的旧内核。 */
	cpu_park_loop();
#else
	BUG();
#endif
}

/*
 * arm64_send_ipi - 把一种逻辑 IPI 投递到目标 CPU 掩码。
 *
 * SGI 模式共享一个 per-CPU IRQ descriptor，可一次按 mask 广播；
 * per-CPU LPI 模式中每个目标有独立 descriptor，必须逐 CPU 单发。
 * mask 是仅在调用期间读取的非 NULL 借用指针，descriptor 由初始化阶段
 * 长期持有。
 * nr 是 [0, nr_ipi) 的逻辑消息号。可在原子上下文调用，不睡眠。
 * 返回：无直接返回值；硬件投递结果不通过返回值确认。
 */
static void arm64_send_ipi(const cpumask_t *mask, unsigned int nr)
{
	/* cpu 仅在 per-CPU LPI 布局下遍历 mask 的目标逻辑编号。 */
	unsigned int cpu;

	if (!percpu_ipi_descs)
		__ipi_send_mask(get_ipi_desc(0, nr), mask);
	else
		for_each_cpu(cpu, mask)
			__ipi_send_single(get_ipi_desc(cpu, nr), cpu);
}

/*
 * arm64_backtrace_ipi - 为 NMI backtrace 框架投递回溯 IPI。
 *
 * mask 是框架提供的非 NULL 输入输出 CPU 集合借用指针，本函数不修改
 * 或持有它。
 * 原子上下文、不睡眠；返回：无，投递类型固定为 IPI_CPU_BACKTRACE。
 */
static void arm64_backtrace_ipi(cpumask_t *mask)
{
	arm64_send_ipi(mask, IPI_CPU_BACKTRACE);
}

/*
 * 请求 mask 中的 CPU 捕获内核栈，exclude_cpu 可排除发起者。NMI 通用框架
 * 负责串行化并跟踪响应；底层是否真用 NMI 由 IPI 配置和伪 NMI 能力决定。
 * mask 是非 NULL 只读借用，exclude_cpu 为逻辑编号或框架约定的
 * “不排除”值。
 * 可从诊断/原子上下文调用，不睡眠；返回：无，输出通过日志或
 * 回溯状态产生。
 */
void arch_trigger_cpumask_backtrace(const cpumask_t *mask, int exclude_cpu)
{
	/*
	 * NOTE: though nmi_trigger_cpumask_backtrace() has "nmi_" in the name,
	 * nothing about it truly needs to be implemented using an NMI, it's
	 * just that it's _allowed_ to work with NMIs. If ipi_should_be_nmi()
	 * returned false our backtrace attempt will just use a regular IPI.
	 */
	/* 回调只负责投递，响应 CPU 在 do_handle_IPI() 中调用 nmi_cpu_backtrace。 */
	nmi_trigger_cpumask_backtrace(mask, exclude_cpu, arm64_backtrace_ipi);
}

#ifdef CONFIG_KGDB
/*
 * KGDB roundup 把除当前 CPU 外的所有在线 CPU 拉入调试回调。这里绕过通用
 * trace 包装直接按 descriptor 单发，因为 KGDB 需要精确控制每个目标。
 * 入参：无。调试异常上下文，不可睡眠；返回：无直接返回值，目标 CPU
 * 异步进入 kgdb_nmicallback()。
 */
void kgdb_roundup_cpus(void)
{
	int this_cpu = raw_smp_processor_id();
	int cpu;

	for_each_online_cpu(cpu) {
		/* No need to roundup ourselves */
		/*
		 * 当前 CPU 已在 KGDB 控制路径中，无需再向自己发送
		 * roundup IPI。
		 */
		if (cpu == this_cpu)
			continue;

		__ipi_send_single(get_ipi_desc(cpu, IPI_KGDB_ROUNDUP), cpu);
	}
}
#endif

/*
 * Main handler for inter-processor interrupts
 */
/*
 * do_handle_IPI - 在目标 CPU 的 IRQ/NMI 上下文分派逻辑 IPI。
 *
 * ipinr 是架构内部消息编号，不是 Linux IRQ 号。有效的标准 IPI 在处理前后
 * 产生 trace 事件；STOP 分支不返回，其余分支必须使用中断上下文安全的
 * helper。未知编号只记录错误，避免把不可识别中断错误分派给其他
 * 子系统。
 * 调用者为 ipi_handler()；IRQ 或伪 NMI 上下文，不可睡眠。
 * 返回：普通消息处理后返回；STOP 消息永不返回。副作用由消息类型
 * 决定。
 */
static void do_handle_IPI(int ipinr)
{
	/* cpu 是本中断的接收 CPU，用于 stop、KGDB、日志和 per-CPU 处理。 */
	unsigned int cpu = smp_processor_id();

	if ((unsigned)ipinr < NR_IPI)
		trace_ipi_entry(ipi_types[ipinr]);

	switch (ipinr) {
	case IPI_RESCHEDULE:
		/* 让调度器检查远端唤醒、负载均衡和重新调度请求。 */
		scheduler_ipi();
		break;

	case IPI_CALL_FUNC:
		/*
		 * 消费通用 SMP call-function 队列，执行发送者预先发布的
		 * 回调。
		 */
		generic_smp_call_function_interrupt();
		break;

	case IPI_CPU_STOP:
	case IPI_CPU_STOP_NMI:
		if (IS_ENABLED(CONFIG_KEXEC_CORE) && crash_stop) {
			/*
			 * crash 模式还要保存当前异常现场；此调用永久停止
			 * CPU。
			 */
			ipi_cpu_crash_stop(cpu, get_irq_regs());
			unreachable();
		} else {
			/* 普通停机只撤销 online 并停放，不保存 crash note。 */
			local_cpu_stop(cpu);
		}
		break;

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
	case IPI_TIMER:
		/* 本地 timer 停止时，由广播设备通过 IPI 注入一次 tick。 */
		tick_receive_broadcast();
		break;
#endif

#ifdef CONFIG_IRQ_WORK
	case IPI_IRQ_WORK:
		/* 在硬中断上下文排空本 CPU 已发布的 irq_work。 */
		irq_work_run();
		break;
#endif

	case IPI_CPU_BACKTRACE:
		/*
		 * NOTE: in some cases this _won't_ be NMI context. See the
		 * comment in arch_trigger_cpumask_backtrace().
		 */
		/*
		 * 完整含义：这里在某些配置下并不处于 NMI 上下文，原因见
		 * arch_trigger_cpumask_backtrace()；处理 helper 能兼容 IRQ/NMI。
		 * 通用 NMI backtrace 核心负责响应计数和栈打印。
		 */
		nmi_cpu_backtrace(get_irq_regs());
		break;

	case IPI_KGDB_ROUNDUP:
		/* 让目标 CPU 保存调试状态并加入 KGDB 的全 CPU 停止协议。 */
		kgdb_nmicallback(cpu, get_irq_regs());
		break;

	default:
		pr_crit("CPU%u: Unknown IPI message 0x%x\n", cpu, ipinr);
		break;
	}

	if ((unsigned)ipinr < NR_IPI)
		trace_ipi_exit(ipi_types[ipinr]);
}

/*
 * Linux IRQ handler 到逻辑 IPI 的适配层。
 *
 * SGI 的 IRQ 号为 base+ipi；LPI 为 base+cpu*nr_ipi+ipi，因此对 nr_ipi
 * 取模可在两种布局下恢复消息类型。IRQ 已由 setup 阶段保证落在
 * 合法范围。data 是 request_*irq() API 的 dev_id，本实现不需要设备私有
 * 状态，因此注册和处理时都使用 NULL。
 * IRQ/NMI 上下文，不可睡眠；返回固定 IRQ_HANDLED，无 ownership 转移。
 */
static irqreturn_t ipi_handler(int irq, void *data)
{
	/* ipi 是从 SGI/LPI IRQ 布局反算出的逻辑消息槽。 */
	unsigned int ipi = (irq - ipi_irq_base) % nr_ipi;

	do_handle_IPI(ipi);
	return IRQ_HANDLED;
}

/*
 * 统一记录一次 IPI raise 事件，再交给布局相关发送器。target 是借用的稳定
 * 非 NULL 快照；调用者负责只选择可接收该类消息的 CPU。
 * ipinr 必须位于 ipi_types 有效范围。可在原子上下文调用，不睡眠；
 * 返回：无直接返回值，副作用是 trace 与硬件 IPI 投递。
 */
static void smp_cross_call(const struct cpumask *target, unsigned int ipinr)
{
	trace_ipi_raise(target, ipi_types[ipinr]);
	arm64_send_ipi(target, ipinr);
}

/*
 * 判断指定 IPI 是否应注册为伪 NMI。只有系统已采用 GIC PMR 屏蔽模型时，
 * stop、backtrace 和 KGDB 这类必须穿透普通 IRQ 屏蔽的消息才升级为 NMI；
 * 其他消息保持普通 IRQ 语义。
 * ipi 是逻辑消息枚举值。任意上下文均不睡眠；返回布尔选择结果，
 * 无副作用。
 */
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

/*
 * ipi_setup - 在一个正在上线的 CPU 上启用全部 IPI。
 *
 * SGI 使用共享 per-CPU IRQ/NMI API，在目标 CPU 本地建立 NMI 上下文或开启
 * per-CPU IRQ；LPI 使用已经绑定到该 CPU 的独立普通 IRQ descriptor。
 * ipi_irq_base 尚未发布表示 irqchip 初始化顺序错误，只告警并避免无效访问。
 * cpu 是正在上线的逻辑编号。CPUHP starting/早期启动上下文，不应睡眠。
 * 返回：无；成功后该 CPU 可接收全部已注册 IPI，失败仅 WARN。
 */
static void ipi_setup(int cpu)
{
	/* i 在当前 CPU 上依次遍历 [0, nr_ipi) 的全部已注册消息。 */
	int i;

	if (WARN_ON_ONCE(!ipi_irq_base))
		return;

	for (i = 0; i < nr_ipi; i++) {
		if (!percpu_ipi_descs) {
			if (ipi_should_be_nmi(i)) {
				/* NMI 需先建立 per-CPU NMI 状态，再允许 GIC 投递。 */
				prepare_percpu_nmi(ipi_irq_base + i);
				enable_percpu_nmi(ipi_irq_base + i, 0);
			} else {
				enable_percpu_irq(ipi_irq_base + i, 0);
			}
		} else {
			/*
			 * 每 CPU LPI 在 request_irq 时带 NO_AUTOEN，直到
			 * 此处才启用。
			 */
			enable_irq(irq_desc_get_irq(get_ipi_desc(cpu, i)));
		}
	}
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * ipi_teardown - 在目标 CPU 下线不可返回点关闭全部 IPI。
 *
 * 与 ipi_setup() 严格对称。SGI NMI 先 disable 再 teardown 本地状态；
 * LPI 关闭目标 CPU 的独立 IRQ。descriptor 本身属于 IRQ 核心并继续保留，
 * 以便 CPU 后续重新上线时复用。
 * cpu 是当前待下线逻辑编号。CPUHP 原子阶段，不应睡眠；返回：无。
 */
static void ipi_teardown(int cpu)
{
	/* i 与 ipi_setup() 使用相同范围，保证每个 enable 都有对应 disable。 */
	int i;

	if (WARN_ON_ONCE(!ipi_irq_base))
		return;

	for (i = 0; i < nr_ipi; i++) {
		if (!percpu_ipi_descs) {
			if (ipi_should_be_nmi(i)) {
				/* 禁止新 NMI 后才能拆除 per-CPU NMI 上下文。 */
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

/*
 * ipi_setup_sgi - 为一种 IPI 消息注册共享的 GIC SGI。
 *
 * ipi 是逻辑消息编号，映射到 base+ipi。IRQ/NMI handler 和 irq_stat
 * per-CPU dev_id 在所有 possible CPU 间共享同一个 descriptor；循环只把
 * 该借用指针缓存进每 CPU 表。注册失败会告警，但初始化继续。
 * 调用者为 set_smp_ipi_range_percpu()；__init 上下文，可调用 IRQ 注册
 * API。返回：无；成功发布 descriptor，失败只 WARN 且没有本地回滚接口。
 */
static void ipi_setup_sgi(int ipi)
{
	/*
	 * err 保存 IRQ 注册 errno，仅用于 WARN；irq 是 base+ipi 的 Linux IRQ；
	 * cpu 遍历 possible 槽，把同一共享 descriptor 发布到每 CPU 缓存。
	 */
	int err, irq, cpu;

	irq = ipi_irq_base + ipi;

	/* 伪 NMI 与普通 per-CPU IRQ 使用不同注册和入口语义。 */
	if (ipi_should_be_nmi(ipi)) {
		err = request_percpu_nmi(irq, ipi_handler, "IPI", NULL, &irq_stat);
		WARN(err, "Could not request IRQ %d as NMI, err=%d\n", irq, err);
	} else {
		err = request_percpu_irq(irq, ipi_handler, "IPI", &irq_stat);
		WARN(err, "Could not request IRQ %d as IRQ, err=%d\n", irq, err);
	}

	for_each_possible_cpu(cpu)
		get_ipi_desc(cpu, ipi) = irq_to_desc(irq);

	/* IPI 是架构内部中断，不作为普通设备 IRQ 暴露给用户配置接口。 */
	irq_set_status_flags(irq, IRQ_HIDDEN);
}

/*
 * ipi_setup_lpi - 为一种 IPI 消息建立逐 CPU LPI descriptor。
 *
 * IRQ 布局为 base + cpu*nr_ipi + ipi。每个 IRQ 被强制亲和到唯一 CPU，
 * 禁止 irqbalance 移动，并以 NO_AUTOEN 注册；目标 CPU 真正上线时再由
 * ipi_setup() enable。LPI 路径按普通 IRQ 注册，不提供伪 NMI 语义。
 * ipi 是消息槽，ncpus 是 irqchip 分配了 LPI 组的 CPU 数。__init 上下文，
 * 可调用 IRQ 注册 API。返回：无；每 CPU descriptor 被缓存，错误仅 WARN。
 */
static void ipi_setup_lpi(int ipi, int ncpus)
{
	for (int cpu = 0; cpu < ncpus; cpu++) {
		/*
		 * cpu 是当前 LPI 组编号；irq 是该 CPU/消息的 Linux IRQ；
		 * err 逐步接收 affinity 和 request_irq 的诊断结果。
		 */
		int err, irq;

		irq = ipi_irq_base + (cpu * nr_ipi) + ipi;

		/* 强制亲和确保 descriptor 的 CPU 槽与硬件投递目标一致。 */
		err = irq_force_affinity(irq, cpumask_of(cpu));
		WARN(err, "Could not force affinity IRQ %d, err=%d\n", irq, err);

		err = request_irq(irq, ipi_handler, IRQF_NO_AUTOEN, "IPI",
				  NULL);
		WARN(err, "Could not request IRQ %d, err=%d\n", irq, err);

		irq_set_status_flags(irq, (IRQ_HIDDEN | IRQ_NO_BALANCING_MASK));

		/*
		 * 缓存 IRQ 核心拥有的 descriptor，供发送、统计和启停
		 * 路径复用。
		 */
		get_ipi_desc(cpu, ipi) = irq_to_desc(irq);
	}
}

/*
 * set_smp_ipi_range_percpu - irqchip 向 arm64 SMP 层交付 IPI IRQ 空间。
 *
 * ipi_base 是连续 IRQ 区间起点，n 是每组可用消息数，ncpus 为零选择共享
 * SGI 模式，非零选择逐 CPU LPI 模式并给出组数。函数在初始化期单次调用，
 * 把最多 MAX_IPI 种消息注册到 IRQ 核心，随后立即启用 boot CPU；次级 CPU
 * 在 secondary_start_kernel() 中各自启用。
 * 三个参数均为 irqchip 传入的纯数值，无 ownership。__init 上下文，可睡眠。
 * 返回：无；发布 __ro_after_init 布局及 descriptor，注册失败仅 WARN。
 */
void __init set_smp_ipi_range_percpu(int ipi_base, int n, int ncpus)
{
	int i;

	WARN_ON(n < MAX_IPI);
	/*
	 * 少于 MAX_IPI 会留下功能缺口，多余硬件槽不纳入本文件的
	 * 消息枚举。
	 */
	nr_ipi = min(n, MAX_IPI);

	/* ncpus 同时充当布局选择标志和 LPI 初始化的 CPU 数量。 */
	percpu_ipi_descs = !!ncpus;
	ipi_irq_base = ipi_base;

	for (i = 0; i < nr_ipi; i++) {
		if (!percpu_ipi_descs)
			ipi_setup_sgi(i);
		else
			ipi_setup_lpi(i, ncpus);
	}

	/* Setup the boot CPU immediately */
	/* boot CPU 不会经过 secondary_start_kernel()，必须在这里单独 enable。 */
	ipi_setup(smp_processor_id());
}

/*
 * arch_smp_send_reschedule - 调度器向指定 CPU 投递重新调度请求。
 *
 * cpu 是在线逻辑编号。调度器原子上下文，不睡眠；返回：无直接返回值，
 * 副作用是投递 IPI_RESCHEDULE，无对象所有权变化。
 */
void arch_smp_send_reschedule(int cpu)
{
	smp_cross_call(cpumask_of(cpu), IPI_RESCHEDULE);
}

#ifdef CONFIG_ARM64_ACPI_PARKING_PROTOCOL
/*
 * parking protocol 用调度 IPI 作为固件 parking loop 的唤醒事件。CPU 真正
 * 进入内核后若仍收到这次 IPI，scheduler_ipi() 可安全处理这个无额外工作的
 * 虚假通知，因此无需占用专用 IPI 类型。
 * cpu 是尚在固件 parking loop 的逻辑目标。启动控制上下文，不睡眠；
 * 返回：无，副作用是一次尽力而为的 IPI 投递。
 */
void arch_send_wakeup_ipi(unsigned int cpu)
{
	/*
	 * We use a scheduler IPI to wake the CPU as this avoids the need for a
	 * dedicated IPI and we can safely handle spurious scheduler IPIs.
	 */
	/*
	 * 固件只需要一次可到达目标 CPU 的唤醒事件；即使该事件晚到
	 * 内核阶段，调度 IPI 的接收路径也允许没有待处理调度工作的
	 * 空操作。
	 */
	smp_send_reschedule(cpu);
}
#endif

#ifdef CONFIG_GENERIC_CLOCKEVENTS_BROADCAST
/*
 * tick_broadcast - 向本地时钟停止的 CPU 广播 timer tick。
 *
 * mask 是 clockevents 核心借出的非 NULL 只读目标集合。时钟中断/原子上下文，
 * 不睡眠；返回：无，目标 CPU 将在 IPI_TIMER 中消费广播 tick。
 */
void tick_broadcast(const struct cpumask *mask)
{
	smp_cross_call(mask, IPI_TIMER);
}
#endif

/*
 * The number of CPUs online, not counting this CPU (which may not be
 * fully online and so not counted in num_online_cpus()).
 */
/*
 * 发起停机的 CPU 可能正在 panic 且尚未或不再位于 online mask 中，不能简单
 * 使用 num_online_cpus()-1。先读取自身 online 位再扣除，结果始终表示
 * “除当前执行者之外仍公开在线的 CPU 数”。
 * 入参：无。任意上下文均不睡眠；返回计数快照，单位为 CPU 个数，
 * 无副作用。
 */
static inline unsigned int num_other_online_cpus(void)
{
	/* 布尔值按 0/1 参与计数修正，单位仍为 CPU 个数。 */
	unsigned int this_cpu_online = cpu_online(smp_processor_id());

	return num_online_cpus() - this_cpu_online;
}

/*
 * smp_send_stop - 尽力停止除当前 CPU 外的所有在线 CPU。
 *
 * 可从 reboot/panic 等脆弱上下文调用，不能获取 CPU hotplug mutex，也不能
 * 睡眠。第一个调用者用 stop_in_progress 获得执行权，快照 online mask 后
 * 先发普通 STOP IPI 并忙等一秒；仍有 CPU 时再以伪 NMI 重拍快照并等待
 * 10ms。函数不保证全部 CPU 停止，最终通过 online mask 告警实际残留。
 *
 * 静态 mask 只由赢得原子门闩的调用者修改；其他并发调用立即返回。函数
 * 结束时无论是否发送 IPI，都屏蔽本 CPU 的 SDEI。
 *
 * 入参：无。reboot/panic 原子上下文，不可睡眠；返回：无直接错误码，
 * 调用者必须通过 online mask 或日志判断是否全部停止。副作用是远端 CPU
 * 清除 online 并永久停放、本地 SDEI 被屏蔽。
 */
void smp_send_stop(void)
{
	/*
	 * 变量地图：
	 *   stop_in_progress 永久一次性原子门闩，位 0 选出唯一发送者；
	 *   mask             仅由门闩获胜者复用的目标 CPU 快照；
	 *   timeout          微秒级忙等剩余次数，普通 IPI 与 NMI 阶段分别重置。
	 */
	static unsigned long stop_in_progress;
	static cpumask_t mask;
	unsigned long timeout;

	/*
	 * If this cpu is the only one alive at this point in time, online or
	 * not, there are no stop messages to be sent around, so just back out.
	 */
	/*
	 * 包含当前 CPU 是否 online 的修正，避免无目标时触碰共享
	 * 静态 mask。
	 */
	if (num_other_online_cpus() == 0)
		goto skip_ipi;

	/* Only proceed if this is the first CPU to reach this code */
	/*
	 * 原子 test-and-set 同时完成竞争判定和门闩发布；panic 中的其他 CPU
	 * 不等待持有者，避免持有者已损坏或永久停止时形成新的死锁。
	 */
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
	/*
	 * online mask 是瞬时快照，不持有 hotplug 锁。目标 CPU 响应 STOP 后会
	 * 自行清 online 位，轮询通过重新统计而不是等待每 CPU completion。
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
	/*
	 * 先用普通 IRQ 给远端退出临界区和释放锁的机会。udelay 忙等适用于
	 * panic/关机上下文，且一秒上限避免永久卡住控制 CPU。
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
		/*
		 * 与远端 set_cpu_online(false) 的发布配合，在重新读取全局 online
		 * mask 前执行读屏障，避免基于旧观察构造第二轮目标快照。
		 */
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
		/*
		 * 最后再次获取快照，只用于准确报告未响应 CPU，不再
		 * 无限重试。
		 */
		smp_rmb();
		cpumask_copy(&mask, cpu_online_mask);
		cpumask_clear_cpu(smp_processor_id(), &mask);

		pr_warn("SMP: failed to stop secondary CPUs %*pbl\n",
			cpumask_pr_args(&mask));
	}

skip_ipi:
	/* 当前控制 CPU 继续执行关机/panic，但不再接受本地 SDEI 事件。 */
	sdei_mask_local_cpu();
}

#ifdef CONFIG_KEXEC_CORE
/*
 * crash_smp_send_stop - crash dump 模式的一次性全 CPU 停止入口。
 *
 * crash_stop 既防止 panic 路径重复执行，也改变接收端 STOP IPI 的行为：
 * 对方会先保存 crash note，再尝试关机。smp_send_stop() 返回后终止当前 CPU
 * 的 SDEI handler 状态，避免旧固件事件跨入崩溃内核。
 * 入参：无。panic/kexec 原子上下文，不可睡眠；返回：无直接结果。
 * 重复调用无副作用，首次调用发布 crash_stop 并停止其他 CPU。
 */
void crash_smp_send_stop(void)
{
	/*
	 * This function can be called twice in panic path, but obviously
	 * we execute this only once.
	 *
	 * We use this same boolean to tell whether the IPI we send was a
	 * stop or a "crash stop".
	 */
	/*
	 * panic 已破坏正常并发环境，这里采用幂等布尔门闩而非可能
	 * 阻塞的锁。
	 */
	if (crash_stop)
		return;
	crash_stop = 1;

	smp_send_stop();

	sdei_handler_abort();
}

/*
 * smp_crash_stop_failed - 查询 crash 停机是否仍遗留在线 CPU。
 *
 * 入参：无。panic 上下文不睡眠；返回 true 表示失败、false 表示在线掩码
 * 已无其他 CPU。只读取状态，无 ownership 和其他副作用。
 */
bool smp_crash_stop_failed(void)
{
	return num_other_online_cpus() != 0;
}
#endif

/*
 * 检查当前任意 CPU 的启动后端是否具备 cpu_die。系统中的 CPU 通常共享同一
 * enable-method，因此取当前 CPU 作为代表；无 HOTPLUG_CPU 构建恒为 false。
 * 入参：无。任意上下文不睡眠；返回能力布尔值，仅借用静态 ops 指针。
 */
static bool have_cpu_die(void)
{
#ifdef CONFIG_HOTPLUG_CPU
	/*
	 * any_cpu 取当前逻辑编号作为系统后端代表；ops 是静态操作表
	 * 借用指针，
	 * 只在本函数内检查 cpu_die 槽。
	 */
	int any_cpu = raw_smp_processor_id();
	const struct cpu_operations *ops = get_cpu_ops(any_cpu);

	if (ops && ops->cpu_die)
		return true;
#endif
	return false;
}

/*
 * cpus_are_stuck_in_kernel - 判断是否存在无法证明已离开当前内核的 CPU。
 *
 * 显式启动失败计数、没有 cpu_die 的多核 spin-table 类系统，以及 protected
 * KVM 都意味着 kexec 覆盖内核内存时其他执行上下文可能仍可访问旧映像。
 * 返回 true 是安全侧保守判定，不等价于已经观察到 CPU 死锁。
 * 入参：无。kexec/能力查询上下文不睡眠；无副作用。
 */
bool cpus_are_stuck_in_kernel(void)
{
	bool smp_spin_tables = (num_possible_cpus() > 1 && !have_cpu_die());

	return !!cpus_stuck_in_kernel || smp_spin_tables ||
		is_protected_kvm_enabled();
}
