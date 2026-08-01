// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 任务生命周期与处理器状态学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：实现 arm64 的机器停机/重启、异常寄存器展示、exec 状态清理、新任务
 * 体系结构上下文构造、任务切换前后的系统寄存器保存恢复，以及 tagged address、
 * MTE、PAC、GCS、TLS、BTI 和用户计时器访问等 ABI 钩子。通用调度策略仍位于
 * kernel/sched，实际通用寄存器切换在 entry.S::cpu_switch_to()。
 *
 * 主调用链：
 *   fork/clone -> arch_dup_task_struct() -> copy_thread() -> ret_from_fork
 *   schedule() -> __switch_to() -> cpu_switch_to(prev, next)
 *   exec -> flush_thread() -> arch_setup_new_exec() -> 新用户体系结构状态
 *   panic/debug -> show_regs() -> __show_regs() -> 栈回溯
 *   reboot/kexec -> machine_{shutdown,halt,power_off,restart}()
 *
 * 核心对象：task_struct::thread 保存任务不运行时的体系结构寄存器快照；pt_regs
 * 保存异常/系统调用边界的用户寄存器；CPU 系统寄存器保存当前运行任务的 live
 * 状态。任务构造阶段由创建路径独占，发布后切换路径依靠调度器禁止抢占和 IRQ
 * 状态协议，不以本文件私有锁保护。
 *
 * 方案权衡：惰性保存 FPSIMD/SVE/SME 可减少普通切换成本，却要求复制前先同步
 * live 状态；把多种扩展集中在 __switch_to() 可固定顺序和屏障边界，却使新增扩展
 * 必须明确与 DSB、SCTLR 和硬件寄存器的相对次序。
 */
/*
 * Based on arch/arm/kernel/process.c
 *
 * Original Copyright (C) 1995  Linus Torvalds
 * Copyright (C) 1996-2000 Russell King - Converted to ARM.
 * Copyright (C) 2012 ARM Ltd.
 */
/* 本实现源自 32 位 ARM process.c，随后由 ARM Ltd. 建立 AArch64 版本。 */
#include <linux/compat.h>
#include <linux/efi.h>
#include <linux/elf.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/kernel.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/nospec.h>
#include <linux/stddef.h>
#include <linux/sysctl.h>
#include <linux/unistd.h>
#include <linux/user.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/cpumask.h>
#include <linux/cpu.h>
#include <linux/elfcore.h>
#include <linux/pm.h>
#include <linux/tick.h>
#include <linux/utsname.h>
#include <linux/uaccess.h>
#include <linux/random.h>
#include <linux/hw_breakpoint.h>
#include <linux/personality.h>
#include <linux/notifier.h>
#include <trace/events/power.h>
#include <linux/percpu.h>
#include <linux/thread_info.h>
#include <linux/prctl.h>
#include <linux/stacktrace.h>

#include <asm/alternative.h>
#include <asm/arch_timer.h>
#include <asm/compat.h>
#include <asm/cpufeature.h>
#include <asm/cacheflush.h>
#include <asm/exec.h>
#include <asm/fpsimd.h>
#include <asm/gcs.h>
#include <asm/mmu_context.h>
#include <asm/mpam.h>
#include <asm/mte.h>
#include <asm/processor.h>
#include <asm/pointer_auth.h>
#include <asm/stacktrace.h>
#include <asm/switch_to.h>
#include <asm/system_misc.h>
/*
 * Linux 头提供任务、重启、ELF、sysctl 和内存管理框架；asm 头提供 arm64 系统
 * 寄存器、扩展状态与切换 helper。包含关系不获取运行时资源。
 */

#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_STACKPROTECTOR_PER_TASK)
/*
 * 工具链不支持 per-task guard 时提供传统全局 canary ABI。变量由
 * boot_init_stack_canary() 在启动期写一次，__ro_after_init 随后把存储页转只读；
 * 导出符号供同样由全局 ABI 插桩的模块使用。每任务模式不定义该共享秘密。
 */
#include <linux/stackprotector.h>
unsigned long __stack_chk_guard __ro_after_init;
EXPORT_SYMBOL(__stack_chk_guard);
#endif

/*
 * Function pointers to optional machine specific functions
 */
/*
 * 下方函数指针保存平台可选的最终断电回调；若固件/平台未注册则保持 NULL。
 * 注册者负责其实现生命周期，通用关机路径通过 do_kernel_power_off() 间接调用。
 */
void (*pm_power_off)(void);
EXPORT_SYMBOL_GPL(pm_power_off);

#ifdef CONFIG_HOTPLUG_CPU
/*
 * arch_cpu_idle_dead() - 让已下线 CPU 从 idle 路径永久进入 cpu_die()。
 *
 * 入参：无；调用时当前 CPU 已走完 hotplug teardown，不能再调度普通任务。
 * 返回：不返回。cpu_die() 完成架构停机；无错误回滚，配置关闭时函数不存在。
 */
void __noreturn arch_cpu_idle_dead(void)
{
       cpu_die();
}
#endif

/*
 * Called by kexec, immediately prior to machine_kexec().
 *
 * This must completely disable all secondary CPUs; simply causing those CPUs
 * to execute e.g. a RAM-based pin loop is not sufficient. This allows the
 * kexec'd kernel to use any and all RAM as it sees fit, without having to
 * avoid any code or data used by any SW CPU pin loop. The CPU hotplug
 * functionality embodied in smpt_shutdown_nonboot_cpus() to achieve this.
 */
/*
 * kexec 在 machine_kexec() 前调用本函数，必须真正关闭所有次级 CPU；仅让它们在
 * RAM 中自旋仍会占用新内核可能覆盖的代码/数据。CPU hotplug 基础设施完成摘除，
 * 使新内核可以把全部 RAM 当作无人执行旧代码的资源。
 *
 * 入参：无。返回：无直接返回值；成功后只有 reboot_cpu 保持运行。调用处已进入
 * 关机串行阶段，函数可能等待其他 CPU，但没有可恢复失败或 ownership 转移。
 */
void machine_shutdown(void)
{
	smp_shutdown_nonboot_cpus(reboot_cpu);
}

/*
 * Halting simply requires that the secondary CPUs stop performing any
 * activity (executing tasks, handling interrupts). smp_send_stop()
 * achieves this.
 */
/*
 * machine_halt() 只要求所有 CPU 停止执行任务和处理中断，不要求切断电源。
 * 当前 CPU 先永久关本地 IRQ，再通知其他 CPU 停止，最后原地自旋且不返回。
 */
void machine_halt(void)
{
	local_irq_disable();
	smp_send_stop();
	while (1);
}

/*
 * Power-off simply requires that the secondary CPUs stop performing any
 * activity (executing tasks, handling interrupts). smp_send_stop()
 * achieves this. When the system power is turned off, it will take all CPUs
 * with it.
 */
/*
 * machine_power_off() 与 halt 同样先停止所有 CPU，再进入通用平台断电链；电源消失
 * 会同时终止全部 CPU，因此无需像 kexec 那样保留可供新内核接管的内存状态。
 * 入参：无；正常路径不返回，平台断电失败后的行为由通用关机层决定。
 */
void machine_power_off(void)
{
	local_irq_disable();
	smp_send_stop();
	do_kernel_power_off();
}

/*
 * Restart requires that the secondary CPUs stop performing any activity
 * while the primary CPU resets the system. Systems with multiple CPUs must
 * provide a HW restart implementation, to ensure that all CPUs reset at once.
 * This is required so that any code running after reset on the primary CPU
 * doesn't have to co-ordinate with other CPUs to ensure they aren't still
 * executing pre-reset code, and using RAM that the primary CPU's code wishes
 * to use. Implementing such co-ordination would be essentially impossible.
 */
/*
 * machine_restart() - 在单一主 CPU 上协调固件/平台整机复位。
 *
 * @cmd: 可空的借用字符串，携带用户或内核请求的重启命令；本函数不保存或释放。
 *
 * SMP 机器必须同时复位所有 CPU，否则主 CPU 复位后运行的新代码会与仍执行旧代码
 * 的次级 CPU 争用 RAM，几乎无法安全协调。函数先关 IRQ、停止次级 CPU，再优先
 * 走 EFI capsule 所要求的 ResetSystem 路径，最后调用体系结构重启处理链。
 * 正常路径不返回；若所有后端均失败则打印告警并永久自旋。
 */
void machine_restart(char *cmd)
{
	/* Disable interrupts first */
	/* 先禁止本地中断，避免停止其他 CPU 后当前 CPU 又进入异步设备路径。 */
	local_irq_disable();
	smp_send_stop();

	/*
	 * UpdateCapsule() depends on the system being reset via
	 * ResetSystem().
	 */
	/* EFI capsule 更新只有经 ResetSystem 复位才会提交，因此 EFI 路径必须优先。 */
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_reboot(reboot_mode, NULL);

	/* Now call the architecture specific reboot code. */
	/* 现在调用平台注册的架构重启实现；正常实现应在此完成复位而不返回。 */
	do_kernel_restart(cmd);

	/*
	 * Whoops - the architecture was unable to reboot.
	 */
	/* 返回到这里表示所有重启后端失败，只能保持 IRQ 关闭并停机，避免继续运行。 */
	printk("Reboot failed -- System halted\n");
	while (1);
}

#define bstr(suffix, str) [PSR_BTYPE_ ## suffix >> PSR_BTYPE_SHIFT] = str
/*
 * btypes[] 把 PSTATE.BTYPE 的两位编码映射为便于崩溃日志阅读的字符串；使用
 * 指定下标初始化，可直接以硬件编码索引，不依赖枚举声明顺序。
 */
static const char *const btypes[] = {
	bstr(NONE, "--"),
	bstr(  JC, "jc"),
	bstr(   C, "-c"),
	bstr(  J , "j-")
};
#undef bstr

static void print_pstate(struct pt_regs *regs)
{
	/*
	 * @regs 是调用者持有的只读异常现场，本函数只格式化其中的 PSTATE。
	 * compat 任务使用 AArch32 CPSR 位定义；原生任务则解码 AArch64 的中断屏蔽、
	 * PAN/UAO/MTE 等控制位。两套布局不可混用，所以必须先按执行模式分流。
	 */
	u64 pstate = regs->pstate;

	if (compat_user_mode(regs)) {
		printk("pstate: %08llx (%c%c%c%c %c %s %s %c%c%c %cDIT %cSSBS)\n",
			pstate,
			pstate & PSR_AA32_N_BIT ? 'N' : 'n',
			pstate & PSR_AA32_Z_BIT ? 'Z' : 'z',
			pstate & PSR_AA32_C_BIT ? 'C' : 'c',
			pstate & PSR_AA32_V_BIT ? 'V' : 'v',
			pstate & PSR_AA32_Q_BIT ? 'Q' : 'q',
			pstate & PSR_AA32_T_BIT ? "T32" : "A32",
			pstate & PSR_AA32_E_BIT ? "BE" : "LE",
			pstate & PSR_AA32_A_BIT ? 'A' : 'a',
			pstate & PSR_AA32_I_BIT ? 'I' : 'i',
			pstate & PSR_AA32_F_BIT ? 'F' : 'f',
			pstate & PSR_AA32_DIT_BIT ? '+' : '-',
			pstate & PSR_AA32_SSBS_BIT ? '+' : '-');
	} else {
		const char *btype_str = btypes[(pstate & PSR_BTYPE_MASK) >>
					       PSR_BTYPE_SHIFT];

		printk("pstate: %08llx (%c%c%c%c %c%c%c%c %cPAN %cUAO %cTCO %cDIT %cSSBS BTYPE=%s)\n",
			pstate,
			pstate & PSR_N_BIT ? 'N' : 'n',
			pstate & PSR_Z_BIT ? 'Z' : 'z',
			pstate & PSR_C_BIT ? 'C' : 'c',
			pstate & PSR_V_BIT ? 'V' : 'v',
			pstate & PSR_D_BIT ? 'D' : 'd',
			pstate & PSR_A_BIT ? 'A' : 'a',
			pstate & PSR_I_BIT ? 'I' : 'i',
			pstate & PSR_F_BIT ? 'F' : 'f',
			pstate & PSR_PAN_BIT ? '+' : '-',
			pstate & PSR_UAO_BIT ? '+' : '-',
			pstate & PSR_TCO_BIT ? '+' : '-',
			pstate & PSR_DIT_BIT ? '+' : '-',
			pstate & PSR_SSBS_BIT ? '+' : '-',
			btype_str);
	}
}

void __show_regs(struct pt_regs *regs)
{
	/*
	 * 输出一个 pt_regs 快照的核心寄存器信息，但不主动回溯栈。
	 * @regs 的生命周期和同步由调用者保证；这里不修改现场，也不取得引用。
	 * AArch32 现场只有 r0-r12、compat LR/SP，AArch64 则输出 x0-x29、x30/SP。
	 * 内核态 PC/LR 用符号形式打印，且先剥离内核指针认证码，避免 PAC 妨碍
	 * 符号解析；用户态地址保持原始数值，便于与进程映像精确对应。
	 */
	int i, top_reg;
	u64 lr, sp;

	if (compat_user_mode(regs)) {
		/* pt_regs 为 compat 现场提供独立的 LR/SP 访问布局。 */
		lr = regs->compat_lr;
		sp = regs->compat_sp;
		top_reg = 12;
	} else {
		lr = regs->regs[30];
		sp = regs->sp;
		top_reg = 29;
	}

	show_regs_print_info(KERN_DEFAULT);
	print_pstate(regs);

	if (!user_mode(regs)) {
		/* 内核返回地址可能带 PAC，符号化前必须恢复为规范指令地址。 */
		printk("pc : %pS\n", (void *)regs->pc);
		printk("lr : %pS\n", (void *)ptrauth_strip_kernel_insn_pac(lr));
	} else {
		printk("pc : %016llx\n", regs->pc);
		printk("lr : %016llx\n", lr);
	}

	printk("sp : %016llx\n", sp);

	if (system_uses_irq_prio_masking())
		/* 启用优先级屏蔽时，PMR 也是重建异常屏蔽状态所需的一部分。 */
		printk("pmr: %08x\n", regs->pmr);

	i = top_reg;

	while (i >= 0) {
		/* 每行逆序打印三个通用寄存器，便于紧凑查看且不遗漏 x0。 */
		printk("x%-2d: %016llx", i, regs->regs[i]);

		while (i-- % 3)
			pr_cont(" x%-2d: %016llx", i, regs->regs[i]);

		pr_cont("\n");
	}
}

void show_regs(struct pt_regs *regs)
{
	/* 在核心寄存器快照之后追加调用链，形成完整的诊断入口。 */
	__show_regs(regs);
	dump_backtrace(regs, NULL, KERN_DEFAULT);
}

static void tls_thread_flush(void)
{
	/*
	 * exec 时清空当前线程所有用户 TLS 寄存器及 compat 影子副本，防止旧映像
	 * 的线程指针泄漏到新程序。调用者正在刷新 current，无并发线程可替它写
	 * 这些 CPU 本地系统寄存器；该函数不失败、不分配资源。
	 */
	write_sysreg(0, tpidr_el0);
	if (system_supports_tpidr2())
		write_sysreg_s(0, SYS_TPIDR2_EL0);

	if (is_compat_task()) {
		current->thread.uw.tp_value = 0;

		/*
		 * We need to ensure ordering between the shadow state and the
		 * hardware state, so that we don't corrupt the hardware state
		 * with a stale shadow state during context switch.
		 */
		/*
		 * 必须约束影子状态与硬件状态的编译器可见顺序，否则上下文切换可能把
		 * 尚未清零的旧影子值重新写回 TPIDRRO_EL0，破坏刚完成的清理。
		 */
		barrier();
		write_sysreg(0, tpidrro_el0);
	}
}

static void flush_tagged_addr_state(void)
{
	/* exec 后新地址空间默认不继承 tagged-address ABI 的线程选择。 */
	if (IS_ENABLED(CONFIG_ARM64_TAGGED_ADDR_ABI))
		clear_thread_flag(TIF_TAGGED_ADDR);
}

static void flush_poe(void)
{
	/* 若 CPU 支持权限覆盖扩展，则把用户 POR_EL0 恢复为体系结构初始权限。 */
	if (!system_supports_poe())
		return;

	write_sysreg_s(POR_EL0_INIT, SYS_POR_EL0);
}

#ifdef CONFIG_ARM64_GCS

static void flush_gcs(void)
{
	/*
	 * 清除 current 的 Guarded Control Stack 软件元数据和硬件指针。新 exec 映像
	 * 必须重新申请并启用 GCS；旧映射的基址、大小、模式与锁定位均不可继承。
	 */
	if (!system_supports_gcs())
		return;

	current->thread.gcspr_el0 = 0;
	current->thread.gcs_base = 0;
	current->thread.gcs_size = 0;
	current->thread.gcs_el0_mode = 0;
	current->thread.gcs_el0_locked = 0;
	write_sysreg_s(GCSCRE0_EL1_nTR, SYS_GCSCRE0_EL1);
	write_sysreg_s(0, SYS_GCSPR_EL0);
}

static int copy_thread_gcs(struct task_struct *p,
			   const struct kernel_clone_args *args)
{
	/*
	 * 为新任务准备 GCS 状态。@p 已由 fork 路径私有持有，@args 仅在调用期间
	 * 借用。模式和锁策略继承 current，但栈映射由 gcs_alloc_thread_stack()
	 * 根据 clone 语义重新选择；成功返回 0，分配失败原样返回负 errno。
	 */
	unsigned long gcs;

	if (!system_supports_gcs())
		return 0;

	p->thread.gcs_base = 0;
	p->thread.gcs_size = 0;

	p->thread.gcs_el0_mode = current->thread.gcs_el0_mode;
	p->thread.gcs_el0_locked = current->thread.gcs_el0_locked;

	gcs = gcs_alloc_thread_stack(p, args);
	if (IS_ERR_VALUE(gcs))
		return PTR_ERR((void *)gcs);

	return 0;
}

#else

/* 未配置 GCS 时保留同一调用接口，让 exec/fork 主流程无需条件编译。 */
static void flush_gcs(void) { }
static int copy_thread_gcs(struct task_struct *p,
			   const struct kernel_clone_args *args)
{
	return 0;
}

#endif

void flush_thread(void)
{
	/*
	 * exec 提交新程序映像时的 arm64 线程状态总清理入口。依次丢弃浮点/SVE、
	 * TLS、调试断点、tagged-address、POE 与 GCS 状态，保证新映像只看到 ABI
	 * 规定的初始值。操作对象固定为 current，各子步骤均不把资源所有权交出。
	 */
	fpsimd_flush_thread();
	tls_thread_flush();
	flush_ptrace_hw_breakpoint(current);
	flush_tagged_addr_state();
	flush_poe();
	flush_gcs();
}

void arch_release_task_struct(struct task_struct *tsk)
{
	/* task_struct 最终释放前归还独立于结构体本体管理的 FPSIMD/SVE 状态。 */
	fpsimd_release_task(tsk);
}

int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src)
{
	/*
	 * 复制 task_struct 的架构私有状态。@src 为借用的父任务，@dst 尚未发布且由
	 * fork 路径独占；本函数先固化父任务可能仍驻留在 CPU 的有效浮点现场，再做
	 * 浅拷贝，最后清除不能共享所有权的 SVE/SME 指针和瞬时故障标志。
	 * 当前实现不分配内存，成功固定返回 0；需要继承的 ZA 稍后单独深拷贝。
	 */
	/*
	 * The current/src task's FPSIMD state may or may not be live, and may
	 * have been altered by ptrace after entry to the kernel. Save the
	 * effective FPSIMD state so that this will be copied into dst.
	 */
	/*
	 * current/src 的 FPSIMD 状态可能仍只存在于硬件中，也可能在进入内核后被
	 * ptrace 修改；先保存并同步“有效状态”，随后浅拷贝才不会复制过期快照。
	 */
	fpsimd_save_and_flush_current_state();
	fpsimd_sync_from_effective_state(src);

	*dst = *src;

	/*
	 * Drop stale reference to src's sve_state and convert dst to
	 * non-streaming FPSIMD mode.
	 */
	/* 丢弃指向父任务 sve_state 的陈旧引用，并让子任务从普通 FPSIMD 模式开始。 */
	dst->thread.fp_type = FP_STATE_FPSIMD;
	dst->thread.sve_state = NULL;
	clear_tsk_thread_flag(dst, TIF_SVE);
	task_smstop_sm(dst);

	/*
	 * Drop stale reference to src's sme_state and ensure dst has ZA
	 * disabled.
	 *
	 * When necessary, ZA will be inherited later in copy_thread_za().
	 */
	/*
	 * sme_state 同样不能因结构体浅拷贝而共享所有权；先禁用子任务 ZA，若 clone
	 * 语义允许继承，copy_thread_za() 会在后续分配并复制独立缓冲区。
	 */
	dst->thread.sme_state = NULL;
	clear_tsk_thread_flag(dst, TIF_SME);
	dst->thread.svcr &= ~SVCR_ZA_MASK;

	/* clear any pending asynchronous tag fault raised by the parent */
	/* 异步 MTE tag fault 属于父任务的待处理事件，不能传播给尚未运行的子任务。 */
	clear_tsk_thread_flag(dst, TIF_MTE_ASYNC_FAULT);

	return 0;
}

static int copy_thread_za(struct task_struct *dst, struct task_struct *src)
{
	/*
	 * 当父任务启用 SME ZA 时，为 @dst 深拷贝 SVE/SME 两块状态。@dst 仍未发布，
	 * 因而无需与调度器并发；成功后子任务独占新缓冲区。第二次分配失败会回滚
	 * 第一次分配并返回 -ENOMEM，未启用 ZA 则无需工作并返回 0。
	 */
	if (!thread_za_enabled(&src->thread))
		return 0;

	dst->thread.sve_state = kzalloc(sve_state_size(src),
					GFP_KERNEL);
	if (!dst->thread.sve_state)
		return -ENOMEM;

	dst->thread.sme_state = kmemdup(src->thread.sme_state,
					sme_state_size(src),
					GFP_KERNEL);
	if (!dst->thread.sme_state) {
		kfree(dst->thread.sve_state);
		dst->thread.sve_state = NULL;
		return -ENOMEM;
	}

	set_tsk_thread_flag(dst, TIF_SME);
	dst->thread.svcr |= SVCR_ZA_MASK;

	return 0;
}

asmlinkage void ret_from_fork(void) asm("ret_from_fork");

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	/*
	 * 构造新任务第一次被调度时所需的用户异常现场和内核 cpu_context。
	 * @p 是尚未发布、由 fork 核心独占的新任务；@args 为借用的创建参数。
	 * args->fn 为空表示复制用户任务，否则创建内核线程。用户分支复制当前
	 * pt_regs、修正子返回值/栈/TLS，并按地址空间共享关系处理 SME ZA、TPIDR2
	 * 和 GCS；内核线程分支则把入口函数与参数放入 x19/x20。两条路径最终都
	 * 从 ret_from_fork 开始，并建立展开器可识别的终止帧。资源准备失败返回
	 * 负 errno，由上层销毁未发布任务；成功返回 0。
	 */
	u64 clone_flags = args->flags;
	unsigned long stack_start = args->stack;
	unsigned long tls = args->tls;
	struct pt_regs *childregs = task_pt_regs(p);
	int ret;

	memset(&p->thread.cpu_context, 0, sizeof(struct cpu_context));

	/*
	 * In case p was allocated the same task_struct pointer as some
	 * other recently-exited task, make sure p is disassociated from
	 * any cpu that may have run that now-exited task recently.
	 * Otherwise we could erroneously skip reloading the FPSIMD
	 * registers for p.
	 */
	/*
	 * slab 可能复用刚退出任务的同一地址；清除各 CPU 对旧地址的 FPSIMD 归属
	 * 记忆，避免以后误判“状态已在本 CPU”而跳过给新任务装载寄存器。
	 */
	fpsimd_flush_task_state(p);

	ptrauth_thread_init_kernel(p);

	if (likely(!args->fn)) {
		/* 用户子任务看到 clone/fork 返回值 0，其余寄存器从父任务现场继承。 */
		*childregs = *current_pt_regs();
		childregs->regs[0] = 0;

		/*
		 * Read the current TLS pointer from tpidr_el0 as it may be
		 * out-of-sync with the saved value.
		 */
		/* 当前硬件 TLS 可能比 thread_struct 影子值更新，必须直接读取 TPIDR_EL0。 */
		*task_user_tls(p) = read_sysreg(tpidr_el0);

		if (system_supports_poe())
			p->thread.por_el0 = read_sysreg_s(SYS_POR_EL0);

		if (stack_start) {
			if (is_compat_thread(task_thread_info(p)))
				childregs->compat_sp = stack_start;
			else
				childregs->sp = stack_start;
		}

		/*
		 * Due to the AAPCS64 "ZA lazy saving scheme", PSTATE.ZA and
		 * TPIDR2 need to be manipulated as a pair, and either both
		 * need to be inherited or both need to be reset.
		 *
		 * Within a process, child threads must not inherit their
		 * parent's TPIDR2 value or they may clobber their parent's
		 * stack at some later point.
		 *
		 * When a process is fork()'d, the child must inherit ZA and
		 * TPIDR2 from its parent in case there was dormant ZA state.
		 *
		 * Use CLONE_VM to determine when the child will share the
		 * address space with the parent, and cannot safely inherit the
		 * state.
		 */
		/*
		 * AAPCS64 的 ZA 延迟保存协议把 PSTATE.ZA 与 TPIDR2 视为一对：必须同时
		 * 继承或同时复位。同一进程内以 CLONE_VM 创建的线程共享地址空间，若继承
		 * 父线程 TPIDR2，日后可能错误覆盖父线程栈，因此清零且不继承 ZA；真正
		 * fork 出独立地址空间时则必须同时复制二者，以保留可能休眠的 ZA 状态。
		 */
		if (system_supports_sme()) {
			if (!(clone_flags & CLONE_VM)) {
				p->thread.tpidr2_el0 = read_sysreg_s(SYS_TPIDR2_EL0);
				ret = copy_thread_za(p, current);
				if (ret)
					return ret;
			} else {
				p->thread.tpidr2_el0 = 0;
				WARN_ON_ONCE(p->thread.svcr & SVCR_ZA_MASK);
			}
		}

		/*
		 * If a TLS pointer was passed to clone, use it for the new
		 * thread.
		 */
		/* CLONE_SETTLS 显式参数优先于刚从父任务硬件寄存器取得的默认 TLS。 */
		if (clone_flags & CLONE_SETTLS)
			p->thread.uw.tp_value = tls;

		ret = copy_thread_gcs(p, args);
		if (ret != 0)
			return ret;
	} else {
		/*
		 * A kthread has no context to ERET to, so ensure any buggy
		 * ERET is treated as an illegal exception return.
		 *
		 * When a user task is created from a kthread, childregs will
		 * be initialized by start_thread() or start_compat_thread().
		 */
		/*
		 * 内核线程没有可供 ERET 返回的用户现场，故把伪现场设为 EL1h|IL；意外
		 * ERET 会成为非法异常返回。若以后由内核线程创建用户任务，start_thread()
		 * 或 start_compat_thread() 会重新初始化这块现场。
		 */
		memset(childregs, 0, sizeof(struct pt_regs));
		childregs->pstate = PSR_MODE_EL1h | PSR_IL_BIT;
		childregs->stackframe.type = FRAME_META_TYPE_FINAL;

		p->thread.cpu_context.x19 = (unsigned long)args->fn;
		p->thread.cpu_context.x20 = (unsigned long)args->fn_arg;

		if (system_supports_poe())
			p->thread.por_el0 = POR_EL0_INIT;
	}
	p->thread.cpu_context.pc = (unsigned long)ret_from_fork;
	p->thread.cpu_context.sp = (unsigned long)childregs;
	/*
	 * For the benefit of the unwinder, set up childregs->stackframe
	 * as the final frame for the new task.
	 */
	/* 把伪栈帧标为新任务调用链终点，避免展开器越过尚不存在的父调用帧。 */
	p->thread.cpu_context.fp = (unsigned long)&childregs->stackframe;

	ptrace_hw_copy_thread(p);

	return 0;
}

void tls_preserve_current_state(void)
{
	/*
	 * 把 current 仍驻留于硬件的用户 TLS 保存到 thread_struct，供迁移/恢复使用。
	 * compat 任务不使用 AArch64 TPIDR2 ABI，因此只保存通用 TPIDR_EL0。
	 */
	*task_user_tls(current) = read_sysreg(tpidr_el0);
	if (system_supports_tpidr2() && !is_compat_task())
		current->thread.tpidr2_el0 = read_sysreg_s(SYS_TPIDR2_EL0);
}

static void tls_thread_switch(struct task_struct *next)
{
	/*
	 * 先保存 current，再把 @next 的 TLS 影子状态写入本 CPU。compat 任务还通过
	 * 只读 TPIDRRO_EL0 暴露线程指针；原生任务必须清零该兼容寄存器，避免泄漏。
	 */
	tls_preserve_current_state();

	if (is_compat_thread(task_thread_info(next)))
		write_sysreg(next->thread.uw.tp_value, tpidrro_el0);
	else
		write_sysreg(0, tpidrro_el0);

	write_sysreg(*task_user_tls(next), tpidr_el0);
	if (system_supports_tpidr2())
		write_sysreg_s(next->thread.tpidr2_el0, SYS_TPIDR2_EL0);
}

/*
 * Force SSBS state on context-switch, since it may be lost after migrating
 * from a CPU which treats the bit as RES0 in a heterogeneous system.
 */
/*
 * 异构系统中某些 CPU 可能把 SSBS 位视为 RES0，任务迁移后不能假定该位仍保持；
 * 上下文切换必须按 next 的 Spectre-v4 策略重新施加缓解状态。
 */
static void ssbs_thread_switch(struct task_struct *next)
{
	/*
	 * Nothing to do for kernel threads, but 'regs' may be junk
	 * (e.g. idle task) so check the flags and bail early.
	 */
	/* 内核线程无需用户 SSBS 状态，且 idle 等任务的 pt_regs 可能无效，应提前退出。 */
	if (unlikely(next->flags & PF_KTHREAD))
		return;

	/*
	 * If all CPUs implement the SSBS extension, then we just need to
	 * context-switch the PSTATE field.
	 */
	/* 所有 CPU 都实现 SSBS 时，通用 PSTATE 保存/恢复已足够，无需软件补偿。 */
	if (alternative_has_cap_unlikely(ARM64_SSBS))
		return;

	spectre_v4_enable_task_mitigation(next);
}

/*
 * We store our current task in sp_el0, which is clobbered by userspace. Keep a
 * shadow copy so that we can restore this upon entry from userspace.
 *
 * This is *only* for exception entry from EL0, and is not valid until we
 * __switch_to() a user task.
 */
/*
 * 内核平时把 current 放在 SP_EL0，但用户态可覆盖 SP_EL0；因此每 CPU 保存一个
 * 入口影子指针，EL0 异常入口先由它恢复 current。该值只服务 EL0 入口，并且仅在
 * __switch_to() 已安装用户任务后有效，不能当作任意时刻的通用 current 副本。
 */
DEFINE_PER_CPU(struct task_struct *, __entry_task);

static void entry_task_switch(struct task_struct *next)
{
	/* 调度器已禁止抢占，在当前 CPU 上发布即将运行任务的入口影子指针。 */
	__this_cpu_write(__entry_task, next);
}

#ifdef CONFIG_ARM64_GCS

void gcs_preserve_current_state(void)
{
	/* GCSPR_EL0 始终可读，将 current 的硬件 GCS 栈指针保存回软件上下文。 */
	current->thread.gcspr_el0 = read_sysreg_s(SYS_GCSPR_EL0);
}

static void gcs_thread_switch(struct task_struct *next)
{
	/*
	 * 保存 current 并恢复 @next 的 Guarded Control Stack 指针/模式；任务状态由
	 * 调度器串行切换，不发生同一任务同时运行。仅在任一任务启用 GCS 时发出
	 * 同步屏障，以满足迁移到其他 PE 后的跨核可见性要求。
	 */
	if (!system_supports_gcs())
		return;

	/* GCSPR_EL0 is always readable */
	/* GCSPR_EL0 无论当前是否启用 GCS 都可读取，因此无需先判断任务模式。 */
	gcs_preserve_current_state();
	write_sysreg_s(next->thread.gcspr_el0, SYS_GCSPR_EL0);

	if (current->thread.gcs_el0_mode != next->thread.gcs_el0_mode)
		gcs_set_el0_mode(next);

	/*
	 * Ensure that GCS memory effects of the 'prev' thread are
	 * ordered before other memory accesses with release semantics
	 * (or preceded by a DMB) on the current PE. In addition, any
	 * memory accesses with acquire semantics (or succeeded by a
	 * DMB) are ordered before GCS memory effects of the 'next'
	 * thread. This will ensure that the GCS memory effects are
	 * visible to other PEs in case of migration.
	 */
	/*
	 * 把 prev 的 GCS 内存效果排在本 PE 后续 release/DMB 访问之前，并把 next
	 * 开始产生的 GCS 效果排在先前 acquire/DMB 访问之后；任务跨 PE 迁移时，
	 * 其他处理器由此能观察到符合锁同步顺序的控制栈更新。
	 */
	if (task_gcs_el0_enabled(current) || task_gcs_el0_enabled(next))
		gcsb_dsync();
}

#else

/* 未配置 GCS 时保持无副作用的切换钩子。 */
static void gcs_thread_switch(struct task_struct *next)
{
}

#endif

/*
 * Handle sysreg updates for ARM erratum 1418040 which affects the 32bit view of
 * CNTVCT, various other errata which require trapping all CNTVCT{,_EL0}
 * accesses and prctl(PR_SET_TSC). Ensure access is disabled iff a workaround is
 * required or PR_TSC_SIGSEGV is set.
 */
/*
 * 综合 PR_SET_TSC 策略及若干 CNTVCT 硬件缺陷，更新本 CPU 的 CNTKCTL_EL1。
 * 需要陷入内核处理时清除用户虚拟计数器访问许可，否则开放直接访问；调用者
 * 必须保证正在为当前 CPU 上即将运行的任务配置寄存器。
 */
static void update_cntkctl_el1(struct task_struct *next)
{
	struct thread_info *ti = task_thread_info(next);

	if (test_ti_thread_flag(ti, TIF_TSC_SIGSEGV) ||
	    has_erratum_handler(read_cntvct_el0) ||
	    (IS_ENABLED(CONFIG_ARM64_ERRATUM_1418040) &&
	     this_cpu_has_cap(ARM64_WORKAROUND_1418040) &&
	     is_compat_thread(ti)))
		sysreg_clear_set(cntkctl_el1, ARCH_TIMER_USR_VCT_ACCESS_EN, 0);
	else
		sysreg_clear_set(cntkctl_el1, 0, ARCH_TIMER_USR_VCT_ACCESS_EN);
}

static void cntkctl_thread_switch(struct task_struct *prev,
				  struct task_struct *next)
{
	/* 只有影响策略的 32 位/TSC 标志变化时才触碰代价较高的系统寄存器。 */
	if ((read_ti_thread_flags(task_thread_info(prev)) &
	     (_TIF_32BIT | _TIF_TSC_SIGSEGV)) !=
	    (read_ti_thread_flags(task_thread_info(next)) &
	     (_TIF_32BIT | _TIF_TSC_SIGSEGV)))
		update_cntkctl_el1(next);
}

static int do_set_tsc_mode(unsigned int val)
{
	/*
	 * 设置 current 是否令用户计数器访问触发 SIGSEGV。只接受 PR_TSC_ENABLE 与
	 * PR_TSC_SIGSEGV；在禁止抢占区同时更新线程标志和本 CPU CNTKCTL_EL1，避免
	 * 两者中间迁移造成软件状态与硬件状态落在不同 CPU。
	 */
	bool tsc_sigsegv;

	if (val == PR_TSC_SIGSEGV)
		tsc_sigsegv = true;
	else if (val == PR_TSC_ENABLE)
		tsc_sigsegv = false;
	else
		return -EINVAL;

	preempt_disable();
	update_thread_flag(TIF_TSC_SIGSEGV, tsc_sigsegv);
	update_cntkctl_el1(current);
	preempt_enable();

	return 0;
}

static void permission_overlay_switch(struct task_struct *next)
{
	/* 保存 current 的 POR_EL0，并在值变化时恢复 next 的权限覆盖配置。 */
	if (!system_supports_poe())
		return;

	current->thread.por_el0 = read_sysreg_s(SYS_POR_EL0);
	if (current->thread.por_el0 != next->thread.por_el0) {
		write_sysreg_s(next->thread.por_el0, SYS_POR_EL0);
		/*
		 * No ISB required as we can tolerate spurious Overlay faults -
		 * the fault handler will check again based on the new value
		 * of POR_EL0.
		 */
		/*
		 * 此处无需 ISB；旧配置造成的偶发 Overlay fault 是可恢复的，故障处理器
		 * 会依据新的 POR_EL0 再检查，而不会把瞬时不一致误当作最终权限结论。
		 */
	}
}

/*
 * __switch_to() checks current->thread.sctlr_user as an optimisation. Therefore
 * this function must be called with preemption disabled and the update to
 * sctlr_user must be made in the same preemption disabled block so that
 * __switch_to() does not see the variable update before the SCTLR_EL1 one.
 */
/*
 * __switch_to() 以 thread.sctlr_user 快照决定是否跳过寄存器写入。因此调用者
 * 必须在同一禁止抢占区先更新 SCTLR_EL1、再同步软件快照，防止任务迁移或切换
 * 观察到“快照已新、硬件仍旧”的状态。
 */
void update_sctlr_el1(u64 sctlr)
{
	/*
	 * EnIA must not be cleared while in the kernel as this is necessary for
	 * in-kernel PAC. It will be cleared on kernel exit if needed.
	 */
	/* 内核执行依赖 EnIA 验证 PAC，EL1 阶段不得清除；若用户禁用则在退出内核时处理。 */
	sysreg_clear_set(sctlr_el1, SCTLR_USER_MASK & ~SCTLR_ELx_ENIA, sctlr);

	/* ISB required for the kernel uaccess routines when setting TCF0. */
	/* 修改 TCF0 后以 ISB 让后续内核 uaccess 按新 tag-check 模式取指执行。 */
	isb();
}

static inline void debug_switch_state(void)
{
	/*
	 * 调试断言：验证调度切换入口的 IRQ 屏蔽表示符合 arm64 约定。启用 GIC
	 * 优先级屏蔽时检查 DAIF 与 PMR 的组合，否则检查传统 DAIF 值。
	 */
	if (system_uses_irq_prio_masking()) {
		unsigned long daif_expected = 0;
		unsigned long daif_actual = read_sysreg(daif);
		unsigned long pmr_expected = GIC_PRIO_IRQOFF;
		unsigned long pmr_actual = read_sysreg_s(SYS_ICC_PMR_EL1);

		WARN_ONCE(daif_actual != daif_expected ||
			  pmr_actual != pmr_expected,
			  "Unexpected DAIF + PMR: 0x%lx + 0x%lx (expected 0x%lx + 0x%lx)\n",
			  daif_actual, pmr_actual,
			  daif_expected, pmr_expected);
	} else {
		unsigned long daif_expected = DAIF_PROCCTX_NOIRQ;
		unsigned long daif_actual = read_sysreg(daif);

		WARN_ONCE(daif_actual != daif_expected,
			  "Unexpected DAIF value: 0x%lx (expected 0x%lx)\n",
			  daif_actual, daif_expected);
	}
}

/*
 * Thread switching.
 */
/*
 * 线程切换的体系结构总入口。调度核心已禁止抢占并建立所需锁语义；@prev 是
 * 当前任务，@next 是即将运行任务。函数先切换仍绑定于 CPU 的扩展状态，再用
 * DSB 收束 TLB/cache/页表与 membarrier 顺序，随后处理依赖该屏障的 MTE/MPAM，
 * 最后由 cpu_switch_to() 更换 callee-saved 寄存器、SP、current、内核 PAC 与
 * shadow call stack。返回值是本任务再次恢复执行时由汇编返回的 last 任务。
 */
__notrace_funcgraph __sched
struct task_struct *__switch_to(struct task_struct *prev,
				struct task_struct *next)
{
	struct task_struct *last;

	debug_switch_state();

	fpsimd_thread_switch(next);
	tls_thread_switch(next);
	hw_breakpoint_thread_switch(next);
	contextidr_thread_switch(next);
	entry_task_switch(next);
	ssbs_thread_switch(next);
	cntkctl_thread_switch(prev, next);
	ptrauth_thread_switch_user(next);
	permission_overlay_switch(next);
	gcs_thread_switch(next);

	/*
	 * Complete any pending TLB or cache maintenance on this CPU in case the
	 * thread migrates to a different CPU. This full barrier is also
	 * required by the membarrier system call. Additionally it makes any
	 * in-progress pgtable writes visible to the table walker; See
	 * emit_pte_barriers().
	 */
	/*
	 * 在任务可能迁移前完成本 CPU 待处理的 TLB/cache 维护；完整 DSB 同时满足
	 * membarrier，并保证进行中的页表写入已对硬件 table walker 可见。
	 */
	dsb(ish);

	/*
	 * MTE thread switching must happen after the DSB above to ensure that
	 * any asynchronous tag check faults have been logged in the TFSR*_EL1
	 * registers.
	 */
	/* DSB 之后读取 MTE 异步错误，才能确保故障已记入 TFSR*_EL1。 */
	mte_thread_switch(next);
	/* avoid expensive SCTLR_EL1 accesses if no change */
	/* 软件快照相同便跳过昂贵的 SCTLR_EL1 访问。 */
	if (prev->thread.sctlr_user != next->thread.sctlr_user)
		update_sctlr_el1(next->thread.sctlr_user);

	/*
	 * MPAM thread switch happens after the DSB to ensure prev's accesses
	 * use prev's MPAM settings.
	 */
	/* 先完成 prev 的内存访问，再切换 MPAM，保证其访问都按 prev 的资源分区计账。 */
	mpam_thread_switch(next);

	/* the actual thread switch */
	/* 真正切换内核寄存器现场；本调用会在当前任务将来再次获调度时返回。 */
	last = cpu_switch_to(prev, next);

	return last;
}

struct wchan_info {
	/* 栈回溯回调的私有累加器：首个非调度器 PC，以及已跳过的调度帧数。 */
	unsigned long	pc;
	int		count;
};

static bool get_wchan_cb(void *arg, unsigned long pc)
{
	/*
	 * 跳过调度器内部帧，遇到第一个外部调用点便记录并终止；最多继续 16 个
	 * 调度帧，防止异常栈或深层内部调用导致无界遍历。@arg 由 __get_wchan()
	 * 在栈上创建，在同步 arch_stack_walk() 返回前始终有效。
	 */
	struct wchan_info *wchan_info = arg;

	if (!in_sched_functions(pc)) {
		wchan_info->pc = pc;
		return false;
	}
	return wchan_info->count++ < 16;
}

unsigned long __get_wchan(struct task_struct *p)
{
	/*
	 * 查询睡眠任务 @p 的等待位置。先取得任务栈引用，避免回溯期间栈被释放；
	 * 无法取引用或没有找到有效帧时返回 0，否则返回首个非调度器 PC。函数只
	 * 借用 task_struct，并在所有成功取栈路径上成对 put_task_stack()。
	 */
	struct wchan_info wchan_info = {
		.pc = 0,
		.count = 0,
	};

	if (!try_get_task_stack(p))
		return 0;

	arch_stack_walk(get_wchan_cb, &wchan_info, p, NULL);

	put_task_stack(p);

	return wchan_info.pc;
}

unsigned long arch_align_stack(unsigned long sp)
{
	/*
	 * exec 时先按地址随机化策略在一页内扰动用户 SP，再向下对齐到 AAPCS64
	 * 要求的 16 字节边界。ADDR_NO_RANDOMIZE 或全局关闭 ASLR 时只做对齐。
	 */
	if (!(current->personality & ADDR_NO_RANDOMIZE) && randomize_va_space)
		sp -= get_random_u32_below(PAGE_SIZE);
	return sp & ~0xf;
}

#ifdef CONFIG_COMPAT
int compat_elf_check_arch(const struct elf32_hdr *hdr)
{
	/*
	 * 验证 32 位 ELF 可否在本机和 current 的调度约束下执行：必须存在 AArch32
	 * EL0、机器类型为 ARM、声明 EABI；异构系统上的 deadline 任务还必须能把
	 * affinity 限制到支持 32 位的 CPU。@hdr 仅借用，返回布尔式结果。
	 */
	if (!system_supports_32bit_el0())
		return false;

	if ((hdr)->e_machine != EM_ARM)
		return false;

	if (!((hdr)->e_flags & EF_ARM_EABI_MASK))
		return false;

	/*
	 * Prevent execve() of a 32-bit program from a deadline task
	 * if the restricted affinity mask would be inadmissible on an
	 * asymmetric system.
	 */
	/*
	 * 若 deadline 任务受限 affinity 在非对称机器上无法只落到 32 位 CPU，
	 * 禁止 execve 32 位程序，以免实时调度约束与可运行 CPU 集合互相冲突。
	 */
	return !static_branch_unlikely(&arm64_mismatched_32bit_el0) ||
	       !dl_task_check_affinity(current, system_32bit_el0_cpumask());
}
#endif

/*
 * Called from setup_new_exec() after (COMPAT_)SET_PERSONALITY.
 */
/*
 * setup_new_exec() 在设定 personality 后调用本函数，为 current 的新映像提交
 * arm64 ABI 状态：设置 mm 执行模式、调整 32 位 CPU affinity，并初始化 PAC、
 * MTE、用户计数器访问及 SSBS 策略。此时 exec 已串行持有 current/mm 的更新权。
 */
void arch_setup_new_exec(void)
{
	unsigned long mmflags = 0;

	if (is_compat_task()) {
		mmflags = MMCF_AARCH32;

		/*
		 * Restrict the CPU affinity mask for a 32-bit task so that
		 * it contains only 32-bit-capable CPUs.
		 *
		 * From the perspective of the task, this looks similar to
		 * what would happen if the 64-bit-only CPUs were hot-unplugged
		 * at the point of execve(), although we try a bit harder to
		 * honour the cpuset hierarchy.
		 */
		/*
		 * 32 位任务只能在支持 AArch32 EL0 的 CPU 上运行；在能力不对称机器上收紧
		 * affinity。任务观察到的效果近似 64 位专用 CPU 在 exec 时被热拔除，同时
		 * force_compatible_cpus_allowed_ptr() 会尽力遵守 cpuset 层级约束。
		 */
		if (static_branch_unlikely(&arm64_mismatched_32bit_el0))
			force_compatible_cpus_allowed_ptr(current);
	} else if (static_branch_unlikely(&arm64_mismatched_32bit_el0)) {
		relax_compatible_cpus_allowed_ptr(current);
	}

	current->mm->context.flags = mmflags;
	ptrauth_thread_init_user();
	mte_thread_init_user();
	do_set_tsc_mode(PR_TSC_ENABLE);

	if (task_spec_ssb_noexec(current)) {
		arch_prctl_spec_ctrl_set(current, PR_SPEC_STORE_BYPASS,
					 PR_SPEC_ENABLE);
	}
}

#ifdef CONFIG_ARM64_TAGGED_ADDR_ABI
/*
 * Control the relaxed ABI allowing tagged user addresses into the kernel.
 */
/* 以下接口控制“允许带 tag 的用户指针进入内核”的宽松 ABI，并联动 MTE 策略。 */
static unsigned int tagged_addr_disabled;

long set_tagged_addr_ctrl(struct task_struct *task, unsigned long arg)
{
	/*
	 * 为 @task 设置 tagged-address/MTE prctl 位。compat ABI 不支持；参数只能含
	 * 当前硬件支持的位，且全局 sysctl 可阻止新的 tagged-address opt-in。
	 * set_mte_ctrl() 先验证/提交 MTE 状态，成功后再原子更新 thread_info 标志；
	 * 返回 0 或 -EINVAL，不取得 task 的所有权。
	 */
	unsigned long valid_mask = PR_TAGGED_ADDR_ENABLE;
	struct thread_info *ti = task_thread_info(task);

	if (is_compat_thread(ti))
		return -EINVAL;

	if (system_supports_mte()) {
		valid_mask |= PR_MTE_TCF_SYNC | PR_MTE_TCF_ASYNC \
			| PR_MTE_TAG_MASK;

		if (cpus_have_cap(ARM64_MTE_STORE_ONLY))
			valid_mask |= PR_MTE_STORE_ONLY;
	}

	if (arg & ~valid_mask)
		return -EINVAL;

	/*
	 * Do not allow the enabling of the tagged address ABI if globally
	 * disabled via sysctl abi.tagged_addr_disabled.
	 */
	/* 全局禁用只阻止新的 prctl 开启请求，避免绕过管理员策略。 */
	if (arg & PR_TAGGED_ADDR_ENABLE && tagged_addr_disabled)
		return -EINVAL;

	if (set_mte_ctrl(task, arg) != 0)
		return -EINVAL;

	update_ti_thread_flag(ti, TIF_TAGGED_ADDR, arg & PR_TAGGED_ADDR_ENABLE);

	return 0;
}

long get_tagged_addr_ctrl(struct task_struct *task)
{
	/* 汇总 @task 的 tagged-address 选择和 MTE 控制位；compat 任务返回 -EINVAL。 */
	long ret = 0;
	struct thread_info *ti = task_thread_info(task);

	if (is_compat_thread(ti))
		return -EINVAL;

	if (test_ti_thread_flag(ti, TIF_TAGGED_ADDR))
		ret = PR_TAGGED_ADDR_ENABLE;

	ret |= get_mte_ctrl(task);

	return ret;
}

/*
 * Global sysctl to disable the tagged user addresses support. This control
 * only prevents the tagged address ABI enabling via prctl() and does not
 * disable it for tasks that already opted in to the relaxed ABI.
 */
/*
 * abi.tagged_addr_disabled 是只禁止后续开启请求的全局闸门；已选择宽松 ABI 的
 * 任务不会被异步撤销，因而不会在运行中突然改变用户/内核指针传递契约。
 */

static const struct ctl_table tagged_addr_sysctl_table[] = {
	/* 只接受 0/1，数据直接保存在 tagged_addr_disabled。 */
	{
		.procname	= "tagged_addr_disabled",
		.mode		= 0644,
		.data		= &tagged_addr_disabled,
		.maxlen		= sizeof(int),
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
};

static int __init tagged_addr_init(void)
{
	/* 在 core initcall 阶段注册 abi/ sysctl；注册失败向启动日志链返回 -EINVAL。 */
	if (!register_sysctl("abi", tagged_addr_sysctl_table))
		return -EINVAL;
	return 0;
}

core_initcall(tagged_addr_init);
#endif	/* CONFIG_ARM64_TAGGED_ADDR_ABI */

#ifdef CONFIG_BINFMT_ELF
int arch_elf_adjust_prot(int prot, const struct arch_elf_state *state,
			 bool has_interp, bool is_interp)
{
	/*
	 * 根据 ELF BTI 属性为可执行映射补 PROT_BTI。动态链接程序由解释器负责标记
	 * 主程序/DSO，内核只处理解释器自身；静态程序则由内核处理其映射。
	 */
	/*
	 * For dynamically linked executables the interpreter is
	 * responsible for setting PROT_BTI on everything except
	 * itself.
	 */
	/* 动态链接时解释器负责除自身外的映射，避免内核与加载器重复或冲突设置。 */
	if (is_interp != has_interp)
		return prot;

	if (!(state->flags & ARM64_ELF_BTI))
		return prot;

	if (prot & PROT_EXEC)
		prot |= PROT_BTI;

	return prot;
}
#endif

int get_tsc_mode(unsigned long adr)
{
	/*
	 * 把 current 的 PR_SET_TSC 模式写到用户地址 @adr。AArch32 ABI 不提供此接口；
	 * put_user() 负责用户指针检查，可能返回 -EFAULT，成功返回 0。
	 */
	unsigned int val;

	if (is_compat_task())
		return -EINVAL;

	if (test_thread_flag(TIF_TSC_SIGSEGV))
		val = PR_TSC_SIGSEGV;
	else
		val = PR_TSC_ENABLE;

	return put_user(val, (unsigned int __user *)adr);
}

int set_tsc_mode(unsigned int val)
{
	/* AArch64 prctl 包装层：拒绝 compat 任务，具体校验和硬件更新交给 do_set_tsc_mode()。 */
	if (is_compat_task())
		return -EINVAL;

	return do_set_tsc_mode(val);
}
