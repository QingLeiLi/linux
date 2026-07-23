// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 内存异常解码、页错误处理与信号/oops 分发学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 异常入口传入 FAR_EL1（fault address）、ESR_EL1（原因）和 pt_regs。
 * do_mem_abort 用 FSC 索引 fault_info：translation/access/permission 进入
 * do_page_fault，SEA/MTE/alignment 走专用处理，未知类型返回上层发信号或
 * oops。用户 fault 优先尝试 per-VMA RCU 锁快路径，失败退 mmap_lock；
 * 内核 fault 先查 exception table、pKVM/KFENCE/BPF/EFI 修复，再决定致命。
 *
 * 页表修改由 VMA lock/PTL/MM 核心同步；异常路径本身不能假设可睡眠，
 * interrupt/no-mm 直接 no_context。FAR tag 对内核诊断有用，但暴露用户前
 * 按架构 UNKNOWN 规则去 tag/sanitize ESR，避免泄漏 kernel-only 映射。
 */
/*
 * Based on arch/arm/mm/fault.c
 *
 * Copyright (C) 1995  Linus Torvalds
 * Copyright (C) 1995-2004 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/bitfield.h>
#include <linux/bpf_defs.h>
#include <linux/extable.h>
#include <linux/kfence.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <linux/hardirq.h>
#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/page-flags.h>
#include <linux/sched/signal.h>
#include <linux/sched/debug.h>
#include <linux/highmem.h>
#include <linux/perf_event.h>
#include <linux/pkeys.h>
#include <linux/preempt.h>
#include <linux/hugetlb.h>

#include <asm/acpi.h>
#include <asm/bug.h>
#include <asm/cmpxchg.h>
#include <asm/cpufeature.h>
#include <asm/efi.h>
#include <asm/exception.h>
#include <asm/daifflags.h>
#include <asm/debug-monitors.h>
#include <asm/esr.h>
#include <asm/kprobes.h>
#include <asm/mte.h>
#include <asm/processor.h>
#include <asm/sysreg.h>
#include <asm/system_misc.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/virt.h>

/* FSC 分发表项：handler 返回 0 表示已处理，非零让 do_mem_abort 执行默认终结。 */
/*
 * fn 接收原始 FAR/ESR/寄存器现场并决定是否已消费异常；sig/code 是无法恢复时
 * 对用户报告的默认信号及 si_code；name 是 oops/signal 的稳定诊断文本。表项
 * 全部静态只读，不持有任务或 VMA 引用，索引只由 ESR_ELx_FSC 的 6 位产生。
 */
struct fault_info {
	int	(*fn)(unsigned long far, unsigned long esr,
		      struct pt_regs *regs);
	int	sig;
	int	code;
	const char *name;
};

static const struct fault_info fault_info[];

/* 取 ESR.FSC 低位作为 64 项表索引，返回只读静态项。 */
static inline const struct fault_info *esr_to_fault_info(unsigned long esr)
{
	return fault_info + (esr & ESR_ELx_FSC);
}

/* 解码 data-abort ISS/ISS2 的访问宽度、方向、GCS/MTE/overlay 等诊断字段。 */
static void data_abort_decode(unsigned long esr)
{
	unsigned long iss2 = ESR_ELx_ISS2(esr);

	pr_alert("Data abort info:\n");

	if (esr & ESR_ELx_ISV) {
		pr_alert("  Access size = %u byte(s)\n",
			 1U << ((esr & ESR_ELx_SAS) >> ESR_ELx_SAS_SHIFT));
		pr_alert("  SSE = %lu, SRT = %lu\n",
			 (esr & ESR_ELx_SSE) >> ESR_ELx_SSE_SHIFT,
			 (esr & ESR_ELx_SRT_MASK) >> ESR_ELx_SRT_SHIFT);
		pr_alert("  SF = %lu, AR = %lu\n",
			 (esr & ESR_ELx_SF) >> ESR_ELx_SF_SHIFT,
			 (esr & ESR_ELx_AR) >> ESR_ELx_AR_SHIFT);
	} else {
		pr_alert("  ISV = 0, ISS = 0x%08lx, ISS2 = 0x%08lx\n",
			 esr & ESR_ELx_ISS_MASK, iss2);
	}

	pr_alert("  CM = %lu, WnR = %lu, TnD = %lu, TagAccess = %lu\n",
		 (esr & ESR_ELx_CM) >> ESR_ELx_CM_SHIFT,
		 (esr & ESR_ELx_WNR) >> ESR_ELx_WNR_SHIFT,
		 (iss2 & ESR_ELx_TnD) >> ESR_ELx_TnD_SHIFT,
		 (iss2 & ESR_ELx_TagAccess) >> ESR_ELx_TagAccess_SHIFT);

	pr_alert("  GCS = %ld, Overlay = %lu, DirtyBit = %lu, Xs = %llu\n",
		 (iss2 & ESR_ELx_GCS) >> ESR_ELx_GCS_SHIFT,
		 (iss2 & ESR_ELx_Overlay) >> ESR_ELx_Overlay_SHIFT,
		 (iss2 & ESR_ELx_DirtyBit) >> ESR_ELx_DirtyBit_SHIFT,
		 (iss2 & ESR_ELx_Xs_MASK) >> ESR_ELx_Xs_SHIFT);
}

/* 打印通用 memory-abort EC/FSC/EA/S1PTW，数据异常再下钻 ISS 解码。 */
static void mem_abort_decode(unsigned long esr)
{
	pr_alert("Mem abort info:\n");

	pr_alert("  ESR = 0x%016lx\n", esr);
	pr_alert("  EC = 0x%02lx: %s, IL = %u bits\n",
		 ESR_ELx_EC(esr), esr_get_class_string(esr),
		 (esr & ESR_ELx_IL) ? 32 : 16);
	pr_alert("  SET = %lu, FnV = %lu\n",
		 (esr & ESR_ELx_SET_MASK) >> ESR_ELx_SET_SHIFT,
		 (esr & ESR_ELx_FnV) >> ESR_ELx_FnV_SHIFT);
	pr_alert("  EA = %lu, S1PTW = %lu\n",
		 (esr & ESR_ELx_EA) >> ESR_ELx_EA_SHIFT,
		 (esr & ESR_ELx_S1PTW) >> ESR_ELx_S1PTW_SHIFT);
	pr_alert("  FSC = 0x%02lx: %s\n", (esr & ESR_ELx_FSC),
		 esr_to_fault_info(esr)->name);

	if (esr_is_data_abort(esr))
		data_abort_decode(esr);
}

/* init_mm pgd 是镜像符号需 __pa_symbol，普通 mm->pgd 位于 linear map 用 virt_to_phys。 */
static inline unsigned long mm_to_pgd_phys(struct mm_struct *mm)
{
	/* Either init_pg_dir or swapper_pg_dir */
	/* init_mm 的根可能是启动/正式内核页表符号，均不保证能按普通 linear VA 换算。 */
	if (mm == &init_mm)
		return __pa_symbol(mm->pgd);

	return (unsigned long)virt_to_phys(mm->pgd);
}

/*
 * Dump out the page tables associated with 'addr' in the currently active mm.
 */
/*
 * 按 addr 属于 TTBR0/TTBR1 选择 current->active_mm 或 init_mm，逐级 READ_ONCE
 * 打印页表项直到 none/bad/leaf。只做 oops 诊断快照，不持页表锁；并发变化
 * 允许输出跨时刻组合，但每个单项不撕裂。用户地址配 init_mm 时明确报告。
 */
static void show_pte(unsigned long addr)
{
	struct mm_struct *mm;
	pgd_t *pgdp;
	pgd_t pgd;

	if (is_ttbr0_addr(addr)) {
		/* TTBR0 */
		/* 低半地址由当前任务 active_mm 翻译；内核线程借用前一用户 mm。 */
		mm = current->active_mm;
		if (mm == &init_mm) {
			pr_alert("[%016lx] user address but active_mm is swapper\n",
				 addr);
			return;
		}
	} else if (is_ttbr1_addr(addr)) {
		/* TTBR1 */
		/* 高半地址始终使用全局 init_mm/swapper 页表诊断。 */
		mm = &init_mm;
	} else {
		pr_alert("[%016lx] address between user and kernel address ranges\n",
			 addr);
		return;
	}

	pr_alert("%s pgtable: %luk pages, %llu-bit VAs, pgdp=%016lx\n",
		 mm == &init_mm ? "swapper" : "user", PAGE_SIZE / SZ_1K,
		 vabits_actual, mm_to_pgd_phys(mm));
	pgdp = pgd_offset(mm, addr);
	pgd = READ_ONCE(*pgdp);
	pr_alert("[%016lx] pgd=%016llx", addr, pgd_val(pgd));

	do {
		p4d_t *p4dp, p4d;
		pud_t *pudp, pud;
		pmd_t *pmdp, pmd;
		pte_t *ptep, pte;

		if (pgd_none(pgd) || pgd_bad(pgd))
			break;

		p4dp = p4d_offset(pgdp, addr);
		p4d = READ_ONCE(*p4dp);
		pr_cont(", p4d=%016llx", p4d_val(p4d));
		if (p4d_none(p4d) || p4d_bad(p4d))
			break;

		pudp = pud_offset(p4dp, addr);
		pud = READ_ONCE(*pudp);
		pr_cont(", pud=%016llx", pud_val(pud));
		if (pud_none(pud) || pud_bad(pud))
			break;

		pmdp = pmd_offset(pudp, addr);
		pmd = READ_ONCE(*pmdp);
		pr_cont(", pmd=%016llx", pmd_val(pmd));
		if (pmd_none(pmd) || pmd_bad(pmd))
			break;

		ptep = pte_offset_map(pmdp, addr);
		if (!ptep)
			break;

		pte = __ptep_get(ptep);
		pr_cont(", pte=%016llx", pte_val(pte));
		pte_unmap(ptep);
	} while(0);

	pr_cont("\n");
}

/*
 * This function sets the access flags (dirty, accessed), as well as write
 * permission, and only to a more permissive setting.
 *
 * It needs to cope with hardware update of the accessed/dirty state by other
 * agents in the system and can safely skip the __sync_icache_dcache() call as,
 * like __set_ptes(), the PTE is never changed from no-exec to exec here.
 *
 * Returns whether or not the PTE actually changed.
 */
/*
 * 原子放宽 AF/dirty/write 权限，支持 PAGE/PMD/PUD 粒度。entry 只贡献这四位，
 * cmpxchg 循环保留硬件并发更新；PTE_RDONLY 反相后用 OR 合并以选择更宽松
 * 权限。dirty 请求时只本地 TLBI，远端旧 RO 项最多产生可修复 spurious fault。
 * 返回 1 表示写入，0 表示原值已相同；调用者持相应页表/VMA 约束。
 */
int __ptep_set_access_flags_anysz(struct vm_area_struct *vma,
				  unsigned long address, pte_t *ptep,
				  pte_t entry, int dirty, unsigned long pgsize)
{
	pteval_t old_pteval, pteval;
	pte_t pte = __ptep_get(ptep);
	int level;

	if (pte_same(pte, entry))
		return 0;

	/* only preserve the access flags and write permission */
	/* 调用者的新 entry 只能贡献 AF/dirty/write；PFN、内存类型等必须保持原值。 */
	pte_val(entry) &= PTE_RDONLY | PTE_AF | PTE_WRITE | PTE_DIRTY;

	/*
	 * Setting the flags must be done atomically to avoid racing with the
	 * hardware update of the access/dirty state. The PTE_RDONLY bit must
	 * be set to the most permissive (lowest value) of *ptep and entry
	 * (calculated as: a & b == ~(~a | ~b)).
	 */
	/* relaxed cmpxchg 负责原子仲裁，TLBI/页表锁协议提供所需硬件可见顺序。 */
	pte_val(entry) ^= PTE_RDONLY;
	pteval = pte_val(pte);
	do {
		old_pteval = pteval;
		pteval ^= PTE_RDONLY;
		pteval |= pte_val(entry);
		pteval ^= PTE_RDONLY;
		pteval = cmpxchg_relaxed(&pte_val(*ptep), old_pteval, pteval);
	} while (pteval != old_pteval);

	/*
	 * Invalidate the local stale read-only entry.  Remote stale entries
	 * may still cause page faults and be invalidated via
	 * flush_tlb_fix_spurious_fault().
	 */
	/* NOBROADCAST 只清本 PE；其他 PE 若命中旧只读项会进 fault 再自修复。 */
	if (dirty) {
		switch (pgsize) {
		case PAGE_SIZE:
			level = 3;
			break;
		case PMD_SIZE:
			level = 2;
			break;
#ifndef __PAGETABLE_PMD_FOLDED
		case PUD_SIZE:
			level = 1;
			break;
#endif
		default:
			level = TLBI_TTL_UNKNOWN;
			WARN_ON(1);
		}

		__flush_tlb_range(vma, address, address + pgsize, pgsize, level,
				  TLBF_NOWALKCACHE | TLBF_NOBROADCAST);
	}
	return 1;
}

/* 判断 ESR 是否来自当前 EL 的 instruction/data abort。 */
static bool is_el1_instruction_abort(unsigned long esr)
{
	return ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_CUR;
}

static bool is_el1_data_abort(unsigned long esr)
{
	return ESR_ELx_EC(esr) == ESR_ELx_EC_DABT_CUR;
}

/*
 * 识别 EL1 permission fault；软件 TTBR0 PAN 下，用户地址 translation fault
 * 且 PSTATE.PAN=1 也等价于权限违规，因为空 TTBR0 是 PAN 的实现手段。
 */
static inline bool is_el1_permission_fault(unsigned long addr, unsigned long esr,
					   struct pt_regs *regs)
{
	if (!is_el1_data_abort(esr) && !is_el1_instruction_abort(esr))
		return false;

	if (esr_fsc_is_permission_fault(esr))
		return true;

	if (is_ttbr0_addr(addr) && system_uses_ttbr0_pan())
		return esr_fsc_is_translation_fault(esr) &&
			(regs->pstate & PSR_PAN_BIT);

	return false;
}

/* pKVM 初始化后 ESR.S1PTW 是 hypervisor 注入 host stage-2 abort 的约定标志。 */
static bool is_pkvm_stage2_abort(unsigned int esr)
{
	/*
	 * S1PTW should only ever be set in ESR_EL1 if the pkvm hypervisor
	 * injected a stage-2 abort -- see host_inject_mem_abort().
	 */
	/* host 原生 EL1 abort 不应带 S1PTW；先确认 pKVM 已启动，避免误分类普通 fault。 */
	return is_pkvm_initialized() && (esr & ESR_ELx_S1PTW);
}

/*
 * 用 AT S1E1R 重走当前 EL1 translation 判断旧 fault 是否已被并发页表更新
 * 修复。临时关本地 IRQ保护 PAR_EL1 这个 CPU 共享诊断寄存器；AT 成功或
 * 返回不同 fault 类型均视为 spurious。pKVM 注入时还尝试强制回收 guest 页。
 */
static bool __kprobes is_spurious_el1_translation_fault(unsigned long addr,
							unsigned long esr,
							struct pt_regs *regs)
{
	unsigned long flags;
	u64 par, dfsc;

	if (!is_el1_data_abort(esr) || !esr_fsc_is_translation_fault(esr))
		return false;

	local_irq_save(flags);
	asm volatile("at s1e1r, %0" :: "r" (addr));
	isb();
	par = read_sysreg_par();
	local_irq_restore(flags);

	/*
	 * If we now have a valid translation, treat the translation fault as
	 * spurious.
	 */
	/* PAR.F=0 证明现在翻译有效，原异常无需再次执行 MM fault。 */
	if (!(par & SYS_PAR_EL1_F)) {
		if (is_pkvm_stage2_abort(esr)) {
			par &= SYS_PAR_EL1_PA;
			return pkvm_force_reclaim_guest_page(par);
		}

		return true;
	}

	/*
	 * If we got a different type of fault from the AT instruction,
	 * treat the translation fault as spurious.
	 */
	/* 当前重走结果已不再是 translation fault，说明原异常对应的页表状态已变化。 */
	dfsc = FIELD_GET(SYS_PAR_EL1_FST, par);
	return !esr_fsc_is_translation_fault(dfsc);
}

/*
 * 不可恢复内核 fault 终结器。打开 bust_spinlocks 允许 oops 控制台输出，
 * 打印 KASAN/ESR/页表/寄存器后 die，并杀死 current。函数不返回。
 */
static void die_kernel_fault(const char *msg, unsigned long addr,
			     unsigned long esr, struct pt_regs *regs)
{
	bust_spinlocks(1);

	pr_alert("Unable to handle kernel %s at virtual address %016lx\n", msg,
		 addr);

	kasan_non_canonical_hook(addr);

	mem_abort_decode(esr);

	show_pte(addr);
	die("Oops", regs, esr);
	bust_spinlocks(0);
	make_task_dead(SIGKILL);
}

#ifdef CONFIG_KASAN_HW_TAGS
/* HW_TAGS 下把同步 MTE fault 报给 KASAN；访问宽度未知所以 size=0。 */
static void report_tag_fault(unsigned long addr, unsigned long esr,
			     struct pt_regs *regs)
{
	/*
	 * SAS bits aren't set for all faults reported in EL1, so we can't
	 * find out access size.
	 */
	/* 因访问宽度不可靠，向 KASAN 传 size=0，仅保留地址和读写方向。 */
	bool is_write = !!(esr & ESR_ELx_WNR);
	kasan_report((void *)addr, 0, is_write, regs->pc);
}
#else
/* Tag faults aren't enabled without CONFIG_KASAN_HW_TAGS. */
/* 非 HW_TAGS 构建保留空接口，让恢复主路径无需条件编译。 */
static inline void report_tag_fault(unsigned long addr, unsigned long esr,
				    struct pt_regs *regs) { }
#endif

/*
 * 报告内核 MTE tag fault 后关闭本 CPU EL1 同步 tag check，并 ISB 提交。
 * 其他 CPU 延迟到各自 fault 时关闭，避免此异常上下文发跨 CPU 操作。
 */
static void do_tag_recovery(unsigned long addr, unsigned long esr,
			   struct pt_regs *regs)
{

	report_tag_fault(addr, esr, regs);

	/*
	 * Disable MTE Tag Checking on the local CPU for the current EL.
	 * It will be done lazily on the other CPUs when they will hit a
	 * tag fault.
	 */
	/* 这是 per-CPU SCTLR 状态；当前 CPU 立即停查，其他 CPU 无需 IPI，按 fault 自愈。 */
	sysreg_clear_set(sctlr_el1, SCTLR_EL1_TCF_MASK,
			 SYS_FIELD_PREP_ENUM(SCTLR_EL1, TCF, NONE));
	isb();
}

/* 精确识别当前 EL data abort 且 FSC=MTE 的同步 tag-check fault。 */
static bool is_el1_mte_sync_tag_check_fault(unsigned long esr)
{
	unsigned long fsc = esr & ESR_ELx_FSC;

	if (!is_el1_data_abort(esr))
		return false;

	if (fsc == ESR_ELx_FSC_MTE)
		return true;

	return false;
}

/*
 * 内核 fault 修复/分类总入口。顺序很重要：先 exception table；再验证并发
 * translation/pKVM；再 MTE 降级；之后区分权限、NULL、hypervisor protection、
 * KFENCE/BPF paging，最后给 EFI runtime fixup 一次机会，否则 oops。
 */
static void __do_kernel_fault(unsigned long addr, unsigned long esr,
			      struct pt_regs *regs)
{
	const char *msg;

	/*
	 * Are we prepared to handle this kernel fault?
	 * We are almost certainly not prepared to handle instruction faults.
	 */
	/* exception table 只修 data/uaccess；instruction abort 不允许跳任意 fixup。 */
	if (!is_el1_instruction_abort(esr) && fixup_exception(regs, esr))
		return;

	if (is_spurious_el1_translation_fault(addr, esr, regs)) {
		WARN_RATELIMIT(!is_pkvm_stage2_abort(esr),
			"Ignoring spurious kernel translation fault at virtual address %016lx\n", addr);
		return;
	}

	if (is_el1_mte_sync_tag_check_fault(esr)) {
		do_tag_recovery(addr, esr, regs);

		return;
	}

	if (is_el1_permission_fault(addr, esr, regs)) {
		if (esr & ESR_ELx_WNR)
			msg = "write to read-only memory";
		else if (is_el1_instruction_abort(esr))
			msg = "execute from non-executable memory";
		else
			msg = "read from unreadable memory";
	} else if (addr < PAGE_SIZE) {
		msg = "NULL pointer dereference";
	} else if (is_pkvm_stage2_abort(esr)) {
		msg = "access to hypervisor-protected memory";
	} else {
		if (esr_fsc_is_translation_fault(esr)) {
			/* guard-page/arena 按自身元数据消化故意制造的 translation fault。 */
			if (kfence_handle_page_fault(addr, esr & ESR_ELx_WNR, regs))
				return;
			if (bpf_arena_handle_page_fault(addr, esr & ESR_ELx_WNR, regs->pc))
				return;
		}

		msg = "paging request";
	}

	if (efi_runtime_fixup_exception(regs, msg))
		return;

	die_kernel_fault(msg, addr, esr, regs);
}

/*
 * 保存供 sigcontext/ptrace 使用的 fault address/ESR。若地址不在 TTBR0，
 * 用户本不应知道 kernel mapping 的真实权限/层级，因此伪装为 level-0
 * translation fault，并清未来可能定义的 RES0 位。只修改 current->thread。
 */
static void set_thread_esr(unsigned long address, unsigned long esr)
{
	current->thread.fault_address = address;

	/*
	 * If the faulting address is in the kernel, we must sanitize the ESR.
	 * From userspace's point of view, kernel-only mappings don't exist
	 * at all, so we report them as level 0 translation faults.
	 * (This is not quite the way that "no mapping there at all" behaves:
	 * an alignment fault not caused by the memory type would take
	 * precedence over translation fault for a real access to empty
	 * space. Unfortunately we can't easily distinguish "alignment fault
	 * not caused by memory type" from "alignment fault caused by memory
	 * type", so we ignore this wrinkle and just return the translation
	 * fault.)
	 */
	/* sanitize 同时是信息隐藏与稳定用户 ABI，不能直接泄漏原 EL1 ESR。 */
	if (!is_ttbr0_addr(current->thread.fault_address)) {
		switch (ESR_ELx_EC(esr)) {
		case ESR_ELx_EC_DABT_LOW:
			/*
			 * These bits provide only information about the
			 * faulting instruction, which userspace knows already.
			 * We explicitly clear bits which are architecturally
			 * RES0 in case they are given meanings in future.
			 * We always report the ESR as if the fault was taken
			 * to EL1 and so ISV and the bits in ISS[23:14] are
			 * clear. (In fact it always will be a fault to EL1.)
			 */
			/* 只保留用户已知的方向/长度类信息，强制伪装成 L0 translation fault。 */
			esr &= ESR_ELx_EC_MASK | ESR_ELx_IL |
				ESR_ELx_CM | ESR_ELx_WNR;
			esr |= ESR_ELx_FSC_FAULT;
			break;
		case ESR_ELx_EC_IABT_LOW:
			/*
			 * Claim a level 0 translation fault.
			 * All other bits are architecturally RES0 for faults
			 * reported with that DFSC value, so we clear them.
			 */
			/* 指令 abort 同样只保留 EC/IL，避免把保留位或内核上下文泄给用户。 */
			esr &= ESR_ELx_EC_MASK | ESR_ELx_IL;
			esr |= ESR_ELx_FSC_FAULT;
			break;
		default:
			/*
			 * This should never happen (entry.S only brings us
			 * into this code for insn and data aborts from a lower
			 * exception level). Fail safe by not providing an ESR
			 * context record at all.
			 */
			/* 非预期异常类别时 ESR 清零，比构造可能错误的用户 signal context 更安全。 */
			WARN(1, "ESR 0x%lx is not DABT or IABT from EL0\n", esr);
			esr = 0;
			break;
		}
	}

	current->thread.fault_code = esr;
}

/*
 * 无法走正常 page fault 的地址处理。user mode 根据 fault_info 保存上下文并
 * 强制信号；kernel mode 交 __do_kernel_fault 尝试架构修复或 oops。FAR 在
 * 内部分类前去 tag，但信号 si_addr 保留适用的原 far。
 */
static void do_bad_area(unsigned long far, unsigned long esr,
			struct pt_regs *regs)
{
	unsigned long addr = untagged_addr(far);

	/*
	 * If we are in kernel mode at this point, we have no context to
	 * handle this fault with.
	 */
	/* 只有用户态异常拥有可投递信号的恢复上下文；内核态必须尝试 fixup 或终结。 */
	if (user_mode(regs)) {
		const struct fault_info *inf = esr_to_fault_info(esr);

		set_thread_esr(addr, esr);
		arm64_force_sig_fault(inf->sig, inf->code, far, inf->name);
	} else {
		__do_kernel_fault(addr, esr, regs);
	}
}

/*
 * 判断 fault 是否应报告 protection-key violation。不能只信 ESR Overlay：
 * POR_EL0 更新缺 ISB 可产生假 overlay，而无页 translation fault 也可能被
 * pkey 禁止。直接用当前 VMA pkey+访问类型重算；返回 bool。
 */
static bool fault_from_pkey(struct vm_area_struct *vma, unsigned int mm_flags)
{
	if (!system_supports_poe())
		return false;

	/*
	 * We do not check whether an Overlay fault has occurred because we
	 * cannot make a decision based solely on its value:
	 *
	 * - If Overlay is set, a fault did occur due to POE, but it may be
	 *   spurious in those cases where we update POR_EL0 without ISB (e.g.
	 *   on context-switch). We would then need to manually check POR_EL0
	 *   against vma_pkey(vma), which is exactly what
	 *   arch_vma_access_permitted() does.
	 *
	 * - If Overlay is not set, we may still need to report a pkey fault.
	 *   This is the case if an access was made within a mapping but with no
	 *   page mapped, and POR_EL0 forbids the access (according to
	 *   vma_pkey()). Such access will result in a SIGSEGV regardless
	 *   because core code checks arch_vma_access_permitted(), but in order
	 *   to report the correct error code - SEGV_PKUERR - we must handle
	 *   that case here.
	 */
	/* 因此统一让 arch_vma_access_permitted 以 VMA pkey 和本次访问类型重算结论。 */
	return !arch_vma_access_permitted(vma,
			mm_flags & FAULT_FLAG_WRITE,
			mm_flags & FAULT_FLAG_INSTRUCTION,
			false);
}

/* 从 data-abort ISS2.GCS 识别硬件 guarded-control-stack 访问。 */
static bool is_gcs_fault(unsigned long esr)
{
	if (!esr_is_data_abort(esr))
		return false;

	return ESR_ELx_ISS2(esr) & ESR_ELx_GCS;
}

/* 判断异常来自 EL0 instruction abort。 */
static bool is_el0_instruction_abort(unsigned long esr)
{
	return ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW;
}

/*
 * Note: not valid for EL1 DC IVAC, but we never use that such that it
 * should fault. EL0 cannot issue DC IVAC (undef).
 */
/* WnR=1 且非 cache-maintenance 才是写访问；EL1 DC IVAC 例外按调用约束排除。 */
static bool is_write_abort(unsigned long esr)
{
	return (esr & ESR_ELx_WNR) && !(esr & ESR_ELx_CM);
}

/*
 * 检查 GCS 操作与 VMA 类型匹配：GCS 指令只能访问 VM_SHADOW_STACK，普通
 * 写又不能写 shadow stack。无硬件直接 false；返回 true 代表 SEGV_ACCERR。
 */
static bool is_invalid_gcs_access(struct vm_area_struct *vma, u64 esr)
{
	if (!system_supports_gcs())
		return false;

	if (unlikely(is_gcs_fault(esr))) {
		/* GCS accesses must be performed on a GCS page */
		/* 专用 GCS 指令落到普通 VMA 说明页类型契约不匹配。 */
		if (!(vma->vm_flags & VM_SHADOW_STACK))
			return true;
	} else if (unlikely(vma->vm_flags & VM_SHADOW_STACK)) {
		/* Only GCS operations can write to a GCS page */
		/* 普通读取允许，普通写入必须拒绝；合法更新只能由 GCS 指令完成。 */
		return esr_is_data_abort(esr) && is_write_abort(esr);
	}

	return false;
}

/*
 * 处理可分页的 translation/access/permission fault。far/esr/regs 来自异常
 * 现场；返回 0 表示已处理（含已发信号/oops 路径）。先由 ESR 形成所需
 * VM_EXEC/WRITE/READ 与 FAULT_FLAG，再检查 PAN/uaccess、pKVM stage2。
 * 用户 fault 优先 lock_vma_under_rcu 快路径，RETRY 或非用户退 mmap_lock；
 * handle_mm_fault 可能释放锁、睡眠、OOM 或返回信号。所有标签都明确当前
 * 锁所有权，最终将错误翻译为 SIGSEGV/SIGBUS/MCE 或内核 no_context。
 */
static int __kprobes do_page_fault(unsigned long far, unsigned long esr,
				   struct pt_regs *regs)
{
	const struct fault_info *inf;
	struct mm_struct *mm = current->mm;
	vm_fault_t fault;
	vm_flags_t vm_flags;
	unsigned int mm_flags = FAULT_FLAG_DEFAULT;
	unsigned long addr = untagged_addr(far);
	struct vm_area_struct *vma;
	int si_code;
	int pkey = -1;

	if (kprobe_page_fault(regs, esr))
		/* kprobe 在故意探测指令上命中时已调整 regs，不能再进入 MM。 */
		return 0;

	/*
	 * If we're in an interrupt or have no user context, we must not take
	 * the fault.
	 */
	/* IRQ/禁 fault 区或内核线程没有可睡眠的用户 mm，只能内核修复/oops。 */
	if (faulthandler_disabled() || !mm)
		goto no_context;

	if (user_mode(regs))
		mm_flags |= FAULT_FLAG_USER;

	/*
	 * vm_flags tells us what bits we must have in vma->vm_flags
	 * for the fault to be benign, __do_page_fault() would check
	 * vma->vm_flags & vm_flags and returns an error if the
	 * intersection is empty
	 */
	/* vm_flags 是 VMA 必须具备的权限集合，mm_flags 是传给 MM 的 fault 行为。 */
	if (is_el0_instruction_abort(esr)) {
		/* It was exec fault */
		/* 取指异常要求 VMA 具备 VM_EXEC，并告知 MM 这是 instruction fault。 */
		vm_flags = VM_EXEC;
		mm_flags |= FAULT_FLAG_INSTRUCTION;
	} else if (is_gcs_fault(esr)) {
		/*
		 * The GCS permission on a page implies both read and
		 * write so always handle any GCS fault as a write fault,
		 * we need to trigger CoW even for GCS reads.
		 */
		/* GCS 页的读也可能更新受保护栈状态，按写 fault 才会正确 COW 私有页。 */
		vm_flags = VM_WRITE;
		mm_flags |= FAULT_FLAG_WRITE;
	} else if (is_write_abort(esr)) {
		/* It was write fault */
		/* 写异常需要可写 VMA，并使 MM 执行 COW/dirty 等写路径。 */
		vm_flags = VM_WRITE;
		mm_flags |= FAULT_FLAG_WRITE;
	} else {
		/* It was read fault */
		/* arm64 权限蕴含关系要求同时接受可写 VMA，EPAN 缺失时还接受可执行 VMA。 */
		vm_flags = VM_READ;
		/* Write implies read */
		/* 架构无“只写不可读”普通映射，VM_WRITE 因而满足读访问。 */
		vm_flags |= VM_WRITE;
		/* If EPAN is absent then exec implies read */
		/* 无 Enhanced PAN 时 privileged data read 也可访问 execute-only 用户页。 */
		if (!alternative_has_cap_unlikely(ARM64_HAS_EPAN))
			vm_flags |= VM_EXEC;
	}

	if (is_ttbr0_addr(addr) && is_el1_permission_fault(addr, esr, regs)) {
		/* EL1 访问用户地址必须来自 exception-table 标记的 uaccess 指令。 */
		if (is_el1_instruction_abort(esr))
			die_kernel_fault("execution of user memory",
					 addr, esr, regs);

		if (!insn_may_access_user(regs->pc, esr))
			die_kernel_fault("access to user memory outside uaccess routines",
					 addr, esr, regs);
	}

	if (is_pkvm_stage2_abort(esr)) {
		/* 用户态注入可报告 ACCERR；内核 stage2 abort 需要更严格 no_context 分类。 */
		if (!user_mode(regs))
			goto no_context;
		arm64_force_sig_fault(SIGSEGV, SEGV_ACCERR, far, "stage-2 fault");
		return 0;
	}

	perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS, 1, regs, addr);

	if (!(mm_flags & FAULT_FLAG_USER))
		/* 内核 uaccess fault 不走 per-VMA RCU fast path，使用稳定 mmap_lock。 */
		goto lock_mmap;

	vma = lock_vma_under_rcu(mm, addr);
	/* fast path 获取单 VMA read lock；失败不代表无映射，必须退全局查找。 */
	if (!vma)
		goto lock_mmap;

	if (is_invalid_gcs_access(vma, esr)) {
		vma_end_read(vma);
		fault = 0;
		si_code = SEGV_ACCERR;
		goto bad_area;
	}

	if (!(vma->vm_flags & vm_flags)) {
		vma_end_read(vma);
		fault = 0;
		si_code = SEGV_ACCERR;
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);
		goto bad_area;
	}

	if (fault_from_pkey(vma, mm_flags)) {
		pkey = vma_pkey(vma);
		vma_end_read(vma);
		fault = 0;
		si_code = SEGV_PKUERR;
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);
		goto bad_area;
	}

	fault = handle_mm_fault(vma, addr, mm_flags | FAULT_FLAG_VMA_LOCK, regs);
	/* RETRY/COMPLETED 的锁释放语义由 MM core 定义，其他结果由本函数 vma_end_read。 */
	if (!(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))
		vma_end_read(vma);

	if (!(fault & VM_FAULT_RETRY)) {
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);
		goto done;
	}
	count_vm_vma_lock_event(VMA_LOCK_RETRY);
	if (fault & VM_FAULT_MAJOR)
		mm_flags |= FAULT_FLAG_TRIED;

	/* Quick path to respond to signals */
	/* MM core 已设置 pending signal 时不再解释其他 fault 位，内核态则走 no_context。 */
	/* pending signal 优先结束重试；内核 fault 仍不能直接返回用户式结果。 */
	if (fault_signal_pending(fault, regs)) {
		if (!user_mode(regs))
			goto no_context;
		return 0;
	}
lock_mmap:

retry:
	/* slow path 返回时持 mmap read lock；VMA grow-down 等查找也在 helper 内完成。 */
	vma = lock_mm_and_find_vma(mm, addr, regs);
	if (unlikely(!vma)) {
		fault = 0;
		si_code = SEGV_MAPERR;
		goto bad_area;
	}

	if (!(vma->vm_flags & vm_flags)) {
		mmap_read_unlock(mm);
		fault = 0;
		si_code = SEGV_ACCERR;
		goto bad_area;
	}

	if (fault_from_pkey(vma, mm_flags)) {
		pkey = vma_pkey(vma);
		mmap_read_unlock(mm);
		fault = 0;
		si_code = SEGV_PKUERR;
		goto bad_area;
	}

	fault = handle_mm_fault(vma, addr, mm_flags, regs);

	/* Quick path to respond to signals */
	/* 锁定慢路径也优先响应 MM core 设置的 signal；用户态直接返回，内核态需终结。 */
	if (fault_signal_pending(fault, regs)) {
		if (!user_mode(regs))
			goto no_context;
		return 0;
	}

	/* The fault is fully completed (including releasing mmap lock) */
	/* COMPLETED 明确表示 MM core 已释放 mmap_lock，本函数不得再次 unlock。 */
	if (fault & VM_FAULT_COMPLETED)
		return 0;

	if (fault & VM_FAULT_RETRY) {
		mm_flags |= FAULT_FLAG_TRIED;
		goto retry;
	}
	mmap_read_unlock(mm);

done:
	/* Handle the "normal" (no error) case first. */
	/* 成功包括 minor/major 统计位，只要无 VM_FAULT_ERROR 就可重试原指令。 */
	if (likely(!(fault & VM_FAULT_ERROR)))
		return 0;

	si_code = SEGV_MAPERR;
bad_area:
	/*
	 * If we are in kernel mode at this point, we have no context to
	 * handle this fault with.
	 */
	/* bad_area 到此已不持 VMA/mmap 锁；kernel mode 统一转 no_context。 */
	if (!user_mode(regs))
		goto no_context;

	if (fault & VM_FAULT_OOM) {
		/*
		 * We ran out of memory, call the OOM killer, and return to
		 * userspace (which will retry the fault, or kill us if we got
		 * oom-killed).
		 */
		/* OOM killer 决定杀谁；当前任务若存活，返回 EL0 后会重新 fault。 */
		pagefault_out_of_memory();
		return 0;
	}

	inf = esr_to_fault_info(esr);
	set_thread_esr(addr, esr);
	if (fault & VM_FAULT_SIGBUS) {
		/*
		 * We had some memory, but were unable to successfully fix up
		 * this page fault.
		 */
		/* SIGBUS 表示地址存在但后端无法提供页，区别于无映射 SIGSEGV。 */
		arm64_force_sig_fault(SIGBUS, BUS_ADRERR, far, inf->name);
	} else if (fault & (VM_FAULT_HWPOISON_LARGE | VM_FAULT_HWPOISON)) {
		unsigned int lsb;

		lsb = PAGE_SHIFT;
		if (fault & VM_FAULT_HWPOISON_LARGE)
			lsb = hstate_index_to_shift(VM_FAULT_GET_HINDEX(fault));

		arm64_force_sig_mceerr(BUS_MCEERR_AR, far, lsb, inf->name);
	} else {
		/*
		 * The pkey value that we return to userspace can be different
		 * from the pkey that caused the fault.
		 *
		 * 1. T1   : mprotect_key(foo, PAGE_SIZE, pkey=4);
		 * 2. T1   : set POR_EL0 to deny access to pkey=4, touches, page
		 * 3. T1   : faults...
		 * 4.    T2: mprotect_key(foo, PAGE_SIZE, pkey=5);
		 * 5. T1   : enters fault handler, takes mmap_lock, etc...
		 * 6. T1   : reaches here, sees vma_pkey(vma)=5, when we really
		 *	     faulted on a pte with its pkey=4.
		 */
		/* pkey 只能报告加锁检查时 VMA 的当前值，竞态下不保证等于 fault 瞬间 PTE。 */
		/* Something tried to access memory that out of memory map */
		/* bad_area 表示地址不在合法映射或权限不符，按 pkey/普通 SEGV 分别编码。 */
		if (si_code == SEGV_PKUERR)
			arm64_force_sig_fault_pkey(far, inf->name, pkey);
		else
			arm64_force_sig_fault(SIGSEGV, si_code, far, inf->name);
	}

	return 0;

no_context:
	/* 所有无法安全睡眠/无用户上下文/内核错误汇聚到架构修复或 oops。 */
	__do_kernel_fault(addr, esr, regs);
	return 0;
}

/* TTBR0 translation fault 可由用户 MM 补页；TTBR1/空洞地址直接 bad_area。 */
static int __kprobes do_translation_fault(unsigned long far,
					  unsigned long esr,
					  struct pt_regs *regs)
{
	unsigned long addr = untagged_addr(far);

	if (is_ttbr0_addr(addr))
		return do_page_fault(far, esr, regs);

	do_bad_area(far, esr, regs);
	return 0;
}

/* compat 用户可尝试软件未对齐修复；其他情况按 BUS_ADRALN/bad kernel fault。 */
static int do_alignment_fault(unsigned long far, unsigned long esr,
			      struct pt_regs *regs)
{
	if (IS_ENABLED(CONFIG_COMPAT_ALIGNMENT_FIXUPS) &&
	    compat_user_mode(regs))
		return do_compat_alignment_fixup(far, regs);
	do_bad_area(far, esr, regs);
	return 0;
}

/* fault_info 中无专用恢复的占位 handler，返回 1 请求 do_mem_abort 默认终结。 */
static int do_bad(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
	/* 返回 1 由统一分发器视为未处理；字符串是 fault_info 的默认诊断类别。 */
	return 1; /* "fault" */
}

/*
 * 同步外部异常/内存 ECC 处理。用户态先让 APEI firmware-first 认领并延迟
 * task_work；否则按 FnV 决定 si_addr 是否可信，去除 UNKNOWN tag，taint
 * MACHINE_CHECK 后通知 SIGBUS/架构 die。返回 0 表示已完成分发。
 */
static int do_sea(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
	const struct fault_info *inf;
	unsigned long siaddr;

	inf = esr_to_fault_info(esr);

	if (user_mode(regs) && apei_claim_sea(regs) == 0) {
		/*
		 * APEI claimed this as a firmware-first notification.
		 * Some processing deferred to task_work before ret_to_user().
		 */
		/* APEI=0 表示已认领而非失败，不能再重复发信号。 */
		return 0;
	}

	if (esr & ESR_ELx_FnV) {
		siaddr = 0;
	} else {
		/*
		 * The architecture specifies that the tag bits of FAR_EL1 are
		 * UNKNOWN for synchronous external aborts. Mask them out now
		 * so that userspace doesn't see them.
		 */
		/* FnV=1 时整个 FAR 无效，向用户报告 0 而非猜测地址。 */
		siaddr  = untagged_addr(far);
	}
	add_taint(TAINT_MACHINE_CHECK, LOCKDEP_STILL_OK);
	arm64_notify_die(inf->name, regs, inf->sig, inf->code, siaddr, esr);

	return 0;
}

/*
 * 修正同步 MTE fault FAR 高 tag 位的架构 UNKNOWN 语义。无 MTE_FAR 能力时
 * 仅保留定义的逻辑 tag、其余取 untagged 地址；随后走 bad_area 发 MTESERR。
 */
static int do_tag_check_fault(unsigned long far, unsigned long esr,
			      struct pt_regs *regs)
{
	/*
	 * The architecture specifies that bits 63:60 of FAR_EL1 are UNKNOWN
	 * for tag check faults. Set them to corresponding bits in the untagged
	 * address if ARM64_MTE_FAR isn't supported.
	 * Otherwise, bits 63:60 of FAR_EL1 are not UNKNOWN.
	 */
	/* 旧 CPU 用 untagged 地址补齐 UNKNOWN 高 nibble，同时保留真正的 MTE logical tag。 */
	if (!cpus_have_cap(ARM64_MTE_FAR))
		far = (__untagged_addr(far) & ~MTE_TAG_MASK) | (far & MTE_TAG_MASK);

	do_bad_area(far, esr, regs);
	return 0;
}

/*
 * 以 6-bit FSC 为索引的完整 dispatch table。每项绑定 handler、默认 signal、
 * si_code 与诊断名；数组位置就是硬件 ABI，unknown/reserved 项也必须占位。
 */
static const struct fault_info fault_info[] = {
	{ do_bad,		SIGKILL, SI_KERNEL,	"ttbr address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level 1 address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level 2 address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level 3 address size fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 0 translation fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 1 translation fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 2 translation fault"	},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level 3 translation fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 0 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 1 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 2 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 3 access flag fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 0 permission fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 1 permission fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 2 permission fault"	},
	{ do_page_fault,	SIGSEGV, SEGV_ACCERR,	"level 3 permission fault"	},
	{ do_sea,		SIGBUS,  BUS_OBJERR,	"synchronous external abort"	},
	{ do_tag_check_fault,	SIGSEGV, SEGV_MTESERR,	"synchronous tag check fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 18"			},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level -1 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 0 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 1 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 2 (translation table walk)"	},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 3 (translation table walk)"	},
	{ do_sea,		SIGBUS,  BUS_OBJERR,	"synchronous parity or ECC error" },	// Reserved when RAS is implemented
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 25"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 26"			},
	{ do_sea,		SIGKILL, SI_KERNEL,	"level -1 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 0 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 1 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 2 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_sea,		SIGKILL, SI_KERNEL,	"level 3 synchronous parity error (translation table walk)"	},	// Reserved when RAS is implemented
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 32"			},
	{ do_alignment_fault,	SIGBUS,  BUS_ADRALN,	"alignment fault"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 34"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 35"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 36"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 37"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 38"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 39"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 40"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"level -1 address size fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 42"			},
	{ do_translation_fault,	SIGSEGV, SEGV_MAPERR,	"level -1 translation fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 44"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 45"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 46"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 47"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"TLB conflict abort"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"Unsupported atomic hardware update fault"	},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 50"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 51"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"implementation fault (lockdown abort)" },
	{ do_bad,		SIGBUS,  BUS_OBJERR,	"implementation fault (unsupported exclusive)" },
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 54"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 55"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 56"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 57"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 58" 			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 59"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 60"			},
	{ do_bad,		SIGKILL, SI_KERNEL,	"section domain fault"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"page domain fault"		},
	{ do_bad,		SIGKILL, SI_KERNEL,	"unknown 63"			},
};

/*
 * memory abort 公共入口。按 FSC 调 handler；返回 0 即恢复/信号已处理。非零
 * 时 kernel 直接 oops，user 使用表中 signal/code 并只暴露 untagged FAR。
 * NOKPROBE 防止 kprobe 递归插桩异常总入口。
 */
void do_mem_abort(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
	const struct fault_info *inf = esr_to_fault_info(esr);
	unsigned long addr = untagged_addr(far);

	if (!inf->fn(far, esr, regs))
		return;

	if (!user_mode(regs))
		die_kernel_fault(inf->name, addr, esr, regs);

	/*
	 * At this point we have an unrecognized fault type whose tag bits may
	 * have been defined as UNKNOWN. Therefore we only expose the untagged
	 * address to the signal handler.
	 */
	/* 未识别 FSC 的 FAR tag 位可能 UNKNOWN，默认通知必须使用 addr。 */
	arm64_notify_die(inf->name, regs, inf->sig, inf->code, addr, esr);
}
NOKPROBE_SYMBOL(do_mem_abort);

/* SP/PC 对齐异常固定映射为 SIGBUS/BUS_ADRALN；同样禁止 kprobe。 */
void do_sp_pc_abort(unsigned long addr, unsigned long esr, struct pt_regs *regs)
{
	arm64_notify_die("SP/PC alignment exception", regs, SIGBUS, BUS_ADRALN,
			 addr, esr);
}
NOKPROBE_SYMBOL(do_sp_pc_abort);

/*
 * Used during anonymous page fault handling.
 */
/*
 * 为匿名 fault 分配 order-0 可移动零 folio。VMA 有 VM_MTE 时附加 ZEROTAGS，
 * 让分配器一次完成数据清零和 tag 初始化，避免 DC ZVA+STGM 两遍。返回 folio
 * 或 NULL，所有权按 vma_alloc_folio 契约交 fault 路径。
 */
struct folio *vma_alloc_zeroed_movable_folio(struct vm_area_struct *vma,
						unsigned long vaddr)
{
	gfp_t flags = GFP_HIGHUSER_MOVABLE | __GFP_ZERO;

	/*
	 * If the page is mapped with PROT_MTE, initialise the tags at the
	 * point of allocation and page zeroing as this is usually faster than
	 * separate DC ZVA and STGM.
	 */
	/* 合并数据与 tag 初始化减少一次遍历；分配器返回前已建立“零数据+有效 tag”状态。 */
	if (vma->vm_flags & VM_MTE)
		flags |= __GFP_ZEROTAGS;

	return vma_alloc_folio(flags, 0, vma, vaddr);
}

/*
 * 初始化连续 numpages 页的 MTE tags，可选同时清数据。无 MTE 返回 clear_pages，
 * 告诉调用者是否仍需普通 clear_highpage；有 MTE 则逐页取得 tagging 状态，
 * 执行 tag-only 或 data+tag 清理并置 page flag，最终 false 表示无需再清。
 */
bool tag_clear_highpages(struct page *page, int numpages, bool clear_pages)
{
	/*
	 * Check if MTE is supported and fall back to clear_highpage().
	 * get_huge_zero_folio() unconditionally passes __GFP_ZEROTAGS and
	 * post_alloc_hook() will invoke tag_clear_highpages().
	 */
	/* huge zero folio 即使无 MTE 也请求 ZEROTAGS，因此必须明确退回数据清零责任。 */
	if (!system_supports_mte())
		return clear_pages;

	/* Newly allocated pages, shouldn't have been tagged yet */
	/* try_page_mte_tagging 取得每页首次 tag 初始化权；命中旧状态说明生命周期异常。 */
	for (int i = 0; i < numpages; i++, page++) {
		WARN_ON_ONCE(!try_page_mte_tagging(page));
		if (clear_pages)
			mte_zero_clear_page_tags(page_address(page));
		else
			mte_clear_page_tags(page_address(page));
		set_page_mte_tagged(page);
	}
	return false;
}
