/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Based on arch/arm/include/asm/mmu_context.h
 *
 * Copyright (C) 1996 Russell King.
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef __ASM_MMU_CONTEXT_H
#define __ASM_MMU_CONTEXT_H

#ifndef __ASSEMBLER__

#include <linux/compiler.h>
#include <linux/sched.h>
#include <linux/sched/hotplug.h>
#include <linux/mm_types.h>
#include <linux/pgtable.h>
#include <linux/pkeys.h>

#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/daifflags.h>
#include <asm/gcs.h>
#include <asm/proc-fns.h>
#include <asm/cputype.h>
#include <asm/sysreg.h>
#include <asm/tlbflush.h>

extern bool rodata_full;
/*
 * rodata_full 表示内核线性映射是否也严格执行只读属性。它影响页表属性
 * 更新与别名处理；变量定义在 arm64 内存初始化代码中，此处仅声明，
 * 不属于某个 mm_struct，也不是进程切换时修改的状态。
 */

/*
 * 在任务切换时把 next 的 PID 写入 CONTEXTIDR_EL1，供硬件 trace、调试器
 * 和性能分析工具把采样归属到进程。该寄存器不参与页表翻译或 ASID
 * 隔离；关闭 CONFIG_PID_IN_CONTEXTIDR 时直接消除全部开销。
 * next 是即将运行的任务，task_pid_nr() 选择其当前 PID 命名空间视图。
 */
/*
 * 【对上段 PID 视图的修正】task_pid_nr() 返回全局 PID（init PID
 * namespace 中的编号），不是 current 所在 PID namespace 的虚拟编号；
 * 后者应由 task_pid_vnr() 获得。CONTEXTIDR 因而在全系统范围可稳定
 * 标识任务，不会随着观察者所在 PID namespace 改变。
 */
static inline void contextidr_thread_switch(struct task_struct *next)
{
	if (!IS_ENABLED(CONFIG_PID_IN_CONTEXTIDR))
		return;

	write_sysreg(task_pid_nr(next), contextidr_el1);
	/* 保证后续取指/调试事件看到新 Context ID，而非流水线中的旧值。 */
	isb();
}

/*
 * Set TTBR0 to reserved_pg_dir. No translations will be possible via TTBR0.
 */
static inline void cpu_set_reserved_ttbr0_nosync(void)
{
	/*
	 * ttbr 是 reserved_pg_dir 的物理页表基址，经过 phys_to_ttbr() 转成
	 * TTBR 编码。reserved_pg_dir 不提供用户映射，用它可立即封死 TTBR0
	 * 半区；nosync 版本只写寄存器，由调用者负责随后 ISB/更强同步。
	 */
	unsigned long ttbr = phys_to_ttbr(__pa_symbol(reserved_pg_dir));

	write_sysreg(ttbr, ttbr0_el1);
}

static inline void cpu_set_reserved_ttbr0(void)
{
	cpu_set_reserved_ttbr0_nosync();
	/* TTBR 写后必须刷新指令流水线，才可假设后续访问使用新翻译基址。 */
	isb();
}

/*
 * 底层汇编/CPU 实现：把 pgd_phys 与 mm 的 ASID/CNP 等属性组合后写入
 * TTBR0_EL1，并执行处理器需要的切换序列。调用前应已解决 ASID 分配。
 */
void cpu_do_switch_mm(phys_addr_t pgd_phys, struct mm_struct *mm);

/*
 * 安装用户地址空间 mm->pgd。pgd 是内核虚拟地址，先转为物理地址；
 * swapper_pg_dir 属于 TTBR1 内核地址空间，装到 TTBR0 是严重调用错误。
 * 此函数不增加 mm 引用，调度/MM 生命周期规则保证调用期间 mm 有效。
 */
static inline void cpu_switch_mm(pgd_t *pgd, struct mm_struct *mm)
{
	BUG_ON(pgd == swapper_pg_dir);
	cpu_do_switch_mm(virt_to_phys(pgd),mm);
}

/*
 * Ensure TCR.T0SZ is set to the provided value.
 */
static inline void __cpu_set_tcr_t0sz(unsigned long t0sz)
{
	/* tcr 保存 TCR_EL1 全值，只替换控制 TTBR0 虚拟地址宽度的 T0SZ 域。 */
	unsigned long tcr = read_sysreg(tcr_el1);

	if ((tcr & TCR_EL1_T0SZ_MASK) == t0sz)
		return;

	tcr &= ~TCR_EL1_T0SZ_MASK;
	tcr |= t0sz;
	write_sysreg(tcr, tcr_el1);
	isb();
}

/*
 * Remove the idmap from TTBR0_EL1 and install the pgd of the active mm.
 *
 * The idmap lives in the same VA range as userspace, but uses global entries
 * and may use a different TCR_EL1.T0SZ. To avoid issues resulting from
 * speculative TLB fetches, we must temporarily install the reserved page
 * tables while we invalidate the TLBs and set up the correct TCR_EL1.T0SZ.
 *
 * If current is a not a user task, the mm covers the TTBR1_EL1 page tables,
 * which should not be installed in TTBR0_EL1. In this case we can leave the
 * reserved page tables in place.
 */
static inline void cpu_uninstall_idmap(void)
{
	/* active_mm 对用户线程是自己的 mm，对内核线程是借用的上一用户 mm。 */
	struct mm_struct *mm = current->active_mm;

	/* break-before-make：先断开旧 idmap，再清掉可能投机取得的旧 TLB。 */
	cpu_set_reserved_ttbr0();
	local_flush_tlb_all();
	/* 从 identity map 的地址宽度恢复正常用户 VA 宽度。 */
	__cpu_set_tcr_t0sz(TCR_T0SZ(vabits_actual));

	/*
	 * 硬件 PAN/无 TTBR0-PAN 时，用户任务需要立即装回自己的 pgd；
	 * init_mm 没有用户映射，而软件 TTBR0 PAN 会在异常返回时延迟恢复。
	 */
	if (mm != &init_mm && !system_uses_ttbr0_pan())
		cpu_switch_mm(mm->pgd, mm);
}

/*
 * 暂时把 TTBR0 切到恒等映射 idmap_pg_dir，供 kexec、休眠恢复及某些
 * MMU 过渡代码在 VA==PA 的环境运行。顺序必须是 reserved -> TLB 清空
 * -> 修改 T0SZ -> 安装 idmap，避免旧表与新表在同一 VA 范围重叠。
 */
static inline void cpu_install_idmap(void)
{
	cpu_set_reserved_ttbr0();
	local_flush_tlb_all();
	__cpu_set_tcr_t0sz(TCR_T0SZ(IDMAP_VA_BITS));

	/* lm_alias() 取得线性映射别名，避免当前映射属性/位置影响取物理址。 */
	cpu_switch_mm(lm_alias(idmap_pg_dir), &init_mm);
}

/*
 * Load our new page tables. A strict BBM approach requires that we ensure that
 * TLBs are free of any entries that may overlap with the global mappings we are
 * about to install.
 *
 * For a real hibernate/resume/kexec cycle TTBR0 currently points to a zero
 * page, but TLBs may contain stale ASID-tagged entries (e.g. for EFI runtime
 * services), while for a userspace-driven test_resume cycle it points to
 * userspace page tables (and we must point it at a zero page ourselves).
 *
 * We change T0SZ as part of installing the idmap. This is undone by
 * cpu_uninstall_idmap() in __cpu_suspend_exit().
 */
static inline void cpu_install_ttbr0(phys_addr_t ttbr0, unsigned long t0sz)
{
	/* ttbr0 已是调用者构造好的物理 TTBR 值；t0sz 描述该表覆盖的 VA 宽度。 */
	cpu_set_reserved_ttbr0();
	local_flush_tlb_all();
	__cpu_set_tcr_t0sz(t0sz);

	/* avoid cpu_switch_mm() and its SW-PAN and CNP interactions */
	write_sysreg(ttbr0, ttbr0_el1);
	isb();
}

/*
 * 在安全的 idmap/汇编过渡序列中替换内核 TTBR1。pgdp 是新顶级页表，
 * cnp 指示是否设置 Common-not-Private，使共享该表的 CPU 可共享 TLB 项。
 */
void __cpu_replace_ttbr1(pgd_t *pgdp, bool cnp);

/* CPU 能力确定后，为最终 swapper_pg_dir 开启 CNP。 */
static inline void cpu_enable_swapper_cnp(void)
{
	__cpu_replace_ttbr1(lm_alias(swapper_pg_dir), true);
}

static inline void cpu_replace_ttbr1(pgd_t *pgdp)
{
	/*
	 * Only for early TTBR1 replacement before cpucaps are finalized and
	 * before we've decided whether to use CNP.
	 */
	WARN_ON(system_capabilities_finalized());
	/* 早期阶段能力尚未统一，必须保守地关闭 CNP。 */
	__cpu_replace_ttbr1(pgdp, false);
}

/*
 * It would be nice to return ASIDs back to the allocator, but unfortunately
 * that introduces a race with a generation rollover where we could erroneously
 * free an ASID allocated in a future generation. We could workaround this by
 * freeing the ASID from the context of the dying mm (e.g. in arch_exit_mmap),
 * but we'd then need to make sure that we didn't dirty any TLBs afterwards.
 * Setting a reserved TTBR0 or EPD0 would work, but it all gets ugly when you
 * take CPU migration into account.
 */
void check_and_switch_context(struct mm_struct *mm);
/*
 * 核心上下文切换器：确保 mm 拥有当前 ASID generation 的有效 ASID，
 * 必要时处理 generation rollover/TLB 刷新，然后把其 pgd 装入 TTBR0。
 * 慢路径的分配与并发协议位于 arch/arm64/mm/context.c；此头文件只把
 * 它接入 scheduler 的 switch_mm() 热路径。
 */

#define init_new_context(tsk, mm) init_new_context(tsk, mm)
/*
 * 初始化新 mm 的 arm64 私有上下文。tsk 由通用 MM 接口传入，但 arm64
 * 不需要读取它；mm 此时尚未运行，因此 ASID id 置 0，首次调度时延迟
 * 分配。pinned 是 ASID 固定引用计数，pkey bitmap 则预留默认键 0。
 */
static inline int
init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	atomic64_set(&mm->context.id, 0);
	refcount_set(&mm->context.pinned, 0);

	/* pkey 0 is the default, so always reserve it. */
	mm->context.pkey_allocation_map = BIT(0);

	return 0;
}

static inline void arch_dup_pkeys(struct mm_struct *oldmm,
				  struct mm_struct *mm)
{
	/* fork 时只复制“哪些保护键已分配”，实际页的 pkey 来自复制的 PTE。 */
	/* Duplicate the oldmm pkey state in mm: */
	mm->context.pkey_allocation_map = oldmm->context.pkey_allocation_map;
}

/* fork/dup_mmap 的体系结构钩子；当前 arm64 只需继承 protection-key 状态。 */
static inline int arch_dup_mmap(struct mm_struct *oldmm, struct mm_struct *mm)
{
	arch_dup_pkeys(oldmm, mm);

	return 0;
}

/*
 * 通用 MM 在销毁地址空间时调用的架构钩子。arm64 刻意不在这里归还
 * 普通 ASID，原因见上方 generation rollover 竞态说明，故为空。
 */
static inline void arch_exit_mmap(struct mm_struct *mm)
{
}

/* 当前 arm64 无需在一段 VMA 被撤销后维护额外的每-mm 架构状态。 */
static inline void arch_unmap(struct mm_struct *mm,
			unsigned long start, unsigned long end)
{
}

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
/*
 * 为软件 PAN 保存任务“返回用户态时应恢复”的 TTBR0 值。
 *
 * 软件 TTBR0 PAN 的核心思想是：内核态让硬件 TTBR0 指向空表，异常返回
 * 用户态时再从 thread_info->ttbr0 恢复用户页表。tsk 是要被调度的任务，
 * mm 是它将使用的地址空间；同一进程多线程共享 mm，但各任务分别保存
 * 返回路径所需快照。WRITE_ONCE 防止编译器拆分 64 位状态访问。
 */
static inline void update_saved_ttbr0(struct task_struct *tsk,
				      struct mm_struct *mm)
{
	/* ttbr 同时编码页表物理基址和本 generation 的 ASID。 */
	u64 ttbr;

	if (!system_uses_ttbr0_pan())
		return;

	if (mm == &init_mm)
		/* 内核线程没有用户地址空间，保存空 reserved 页表。 */
		ttbr = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
	else
		ttbr = phys_to_ttbr(virt_to_phys(mm->pgd)) |
		       FIELD_PREP(TTBRx_EL1_ASID_MASK, ASID(mm));

	WRITE_ONCE(task_thread_info(tsk)->ttbr0, ttbr);
}
#else
/* 未编译软件 PAN 时保留空钩子，调度热路径无需条件编译。 */
static inline void update_saved_ttbr0(struct task_struct *tsk,
				      struct mm_struct *mm)
{
}
#endif

#define enter_lazy_tlb enter_lazy_tlb
/*
 * 内核线程进入 lazy-TLB 模式：它可借用 active_mm 维持内核映射/引用，
 * 却绝不需要访问该 mm 的用户地址，因此软件 PAN 快照改为空 TTBR0。
 * mm 参数是通用接口的一部分，在 arm64 本实现中不读取。
 */
static inline void
enter_lazy_tlb(struct mm_struct *mm, struct task_struct *tsk)
{
	/*
	 * We don't actually care about the ttbr0 mapping, so point it at the
	 * zero page.
	 */
	update_saved_ttbr0(tsk, &init_mm);
}

/*
 * 当前 CPU 实际切换到 next。init_mm 特判避免把只属于 TTBR1 的内核
 * pgd 错装进 TTBR0；普通用户 mm 交给 ASID 管理器完成检查和安装。
 */
static inline void __switch_mm(struct mm_struct *next)
{
	/*
	 * init_mm.pgd does not contain any user mappings and it is always
	 * active for kernel addresses in TTBR1. Just set the reserved TTBR0.
	 */
	if (next == &init_mm) {
		cpu_set_reserved_ttbr0();
		return;
	}

	check_and_switch_context(next);
}

static inline void
switch_mm(struct mm_struct *prev, struct mm_struct *next,
	  struct task_struct *tsk)
{
	/* 同一进程线程切换时页表相同，无需重写硬件 TTBR0。 */
	if (prev != next)
		__switch_mm(next);

	/*
	 * Update the saved TTBR0_EL1 of the scheduled-in task as the previous
	 * value may have not been initialised yet (activate_mm caller) or the
	 * ASID has changed since the last run (following the context switch
	 * of another thread of the same process).
	 */
	update_saved_ttbr0(tsk, next);
}

/*
 * 计算任务真正可运行的 CPU 集合。异构系统可能只有部分 CPU 支持 AArch32
 * EL0：能力一致或 p 为原生 64 位线程时沿用调用者 mask；compat 线程则
 * 收窄为 system_32bit_el0_cpumask()，避免迁移后执行能力缺失。
 */
static inline const struct cpumask *
__task_cpu_possible_mask(struct task_struct *p, const struct cpumask *mask)
{
	if (!static_branch_unlikely(&arm64_mismatched_32bit_el0))
		return mask;

	if (!is_compat_thread(task_thread_info(p)))
		return mask;

	return system_32bit_el0_cpumask();
}

static inline const struct cpumask *
task_cpu_possible_mask(struct task_struct *p)
{
	/* 默认从系统可能上线 CPU 集合开始，再应用上面的 compat 限制。 */
	return __task_cpu_possible_mask(p, cpu_possible_mask);
}
#define task_cpu_possible_mask	task_cpu_possible_mask

const struct cpumask *task_cpu_fallback_mask(struct task_struct *p);
/* affinity 无法满足时寻找仍可运行该任务的后备集合，定义在架构调度代码。 */

void verify_cpu_asid_bits(void);
/* 校验各 CPU 实现的 ASID 位数是否满足系统统一上下文分配要求。 */
void post_ttbr_update_workaround(void);
/* TTBR 更新后执行命中 CPU erratum 时所需的体系结构规避序列。 */

unsigned long arm64_mm_context_get(struct mm_struct *mm);
/* 固定并取得 mm 的有效 ASID；返回值供长期使用者保存，失败语义见实现。 */
void arm64_mm_context_put(struct mm_struct *mm);
/* 释放与 arm64_mm_context_get() 配对的 pinned 引用，而非立即回收普通 ASID。 */

#define mm_untag_mask mm_untag_mask
static inline unsigned long mm_untag_mask(struct mm_struct *mm)
{
	/*
	 * arm64 用户指针的高 8 位可承载 TBI/MTE tag；返回低 56 位为 1 的
	 * 掩码供通用代码去标签。当前所有 mm 相同，参数仅为通用接口一致性。
	 */
	return -1UL >> 8;
}

/*
 * Only enforce protection keys on the current process, because there is no
 * user context to access POR_EL0 for another address space.
 */
static inline bool arch_vma_access_permitted(struct vm_area_struct *vma,
		bool write, bool execute, bool foreign)
{
	/*
	 * vma 是待访问映射；write/execute 描述操作类型；foreign 表示检查者
	 * 正在观察别的进程地址空间。返回 true 仅表示架构 protection key
	 * 允许，普通 PTE/VMA 权限仍由上层检查。
	 */
	if (!system_supports_poe())
		return true;

	/* allow access if the VMA is not one from this process */
	if (foreign || vma_is_foreign(vma))
		return true;

	/* 当前进程可读取 POR_EL0，按 VMA 的 pkey 判定读/写/执行权限。 */
	return por_el0_allows_pkey(vma_pkey(vma), write, execute);
}

#define deactivate_mm deactivate_mm
static inline void deactivate_mm(struct task_struct *tsk,
			struct mm_struct *mm)
{
	/*
	 * 任务停止使用该 mm 时释放其 Guarded Control Stack 线程资源。
	 * GCS 属于 task 而非 mm，所以使用 tsk；mm 是通用钩子参数，此处不用。
	 */
	gcs_free(tsk);
}


#include <asm-generic/mmu_context.h>

#endif /* !__ASSEMBLER__ */

#endif /* !__ASM_MMU_CONTEXT_H */
