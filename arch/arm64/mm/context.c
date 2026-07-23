// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 ASID 分配、generation rollover 与 TTBR 上下文切换学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * TLB 用 ASID 区分进程，切换 mm 时无需全量失效。context.id 编成“高位
 * generation + 低位硬件 ASID”；编号用尽时 generation 递增，保存每 CPU
 * 仍可能缓存的 reserved ASID，并让各 CPU 在下次切换时延迟本地 flush。
 * active_asids 上的 atomic xchg/cmpxchg 与 cpu_asid_lock 共同仲裁 rollover。
 * KPTI 成对分配 kernel/user ASID；pinned ASID 用引用和独立 bitmap 保证
 * 外部用户跨调度、跨 generation 仍持有稳定低位编号。
 */
/*
 * Based on arch/arm/mm/context.c
 *
 * Copyright (C) 2002-2003 Deep Blue Solutions Ltd, all rights reserved.
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>

#include <asm/cpufeature.h>
#include <asm/mmu_context.h>
#include <asm/smp.h>
#include <asm/tlbflush.h>

/* boot CPU 决定的统一硬件 ASID 位数；热插 CPU 不得更小。 */
static u32 asid_bits;
/* 保护 generation、bitmap、reserved/pinned 状态及慢路径游标。 */
static DEFINE_RAW_SPINLOCK(cpu_asid_lock);

/* 当前代数及其低位 ASID 占用图。 */
static atomic64_t asid_generation;
static unsigned long *asid_map;

/* active 是 CPU 当前 id；reserved 是 rollover 后仍可能残留于该 CPU 的 id。 */
static DEFINE_PER_CPU(atomic64_t, active_asids);
static DEFINE_PER_CPU(u64, reserved_asids);
static cpumask_t tlb_flush_pending;

/* pinned 容量、当前数量与独立占用图；map=NULL 表示功能不可用。 */
static unsigned long max_pinned_asids;
static unsigned long nr_pinned_asids;
static unsigned long *pinned_asid_map;

#define ASID_MASK		(~GENMASK(asid_bits - 1, 0))
#define ASID_FIRST_VERSION	(1UL << 16)

#define NUM_USER_ASIDS		(1UL << asid_bits)
#define ctxid2asid(asid)	((asid) & ~ASID_MASK)
#define asid2ctxid(asid, genid)	((asid) | (genid))
/* 低 asid_bits 是硬件编号，高位是软件 generation；代步长固定 1<<16。 */

/* Get the ASIDBits supported by the current CPU */
/* 读取 ID_AA64MMFR0_EL1，返回 8/16；未知编码告警并保守采用 8。 */
static u32 get_cpu_asid_bits(void)
{
	u32 asid;
	int fld = cpuid_feature_extract_unsigned_field(read_cpuid(ID_AA64MMFR0_EL1),
						ID_AA64MMFR0_EL1_ASIDBITS_SHIFT);

	switch (fld) {
	default:
		pr_warn("CPU%d: Unknown ASID size (%d); assuming 8-bit\n",
					smp_processor_id(),  fld);
		fallthrough;
	case ID_AA64MMFR0_EL1_ASIDBITS_8:
		asid = 8;
		break;
	case ID_AA64MMFR0_EL1_ASIDBITS_16:
		asid = 16;
	}

	return asid;
}

/* Check if the current cpu's ASIDBits is compatible with asid_bits */
/* 热插校验：更小位宽无法表示已分配编号，不能在线缩容，只能 CPU panic。 */
void verify_cpu_asid_bits(void)
{
	u32 asid = get_cpu_asid_bits();

	if (asid < asid_bits) {
		/*
		 * We cannot decrease the ASID size at runtime, so panic if we support
		 * fewer ASID bits than the boot CPU.
		 */
		/* 已运行任务可能含大编号 ASID；让较窄 CPU 上线会截断并碰撞，无法靠 flush 修复。 */
		pr_crit("CPU%d: smaller ASID size(%u) than boot CPU (%u)\n",
				smp_processor_id(), asid, asid_bits);
		cpu_panic_kernel();
	}
}

/* 以 0xaa 位模式预占 KPTI pair 的 kernel 一侧，仅留下 user 一侧可分配。 */
static void set_kpti_asid_bits(unsigned long *map)
{
	unsigned int len = BITS_TO_LONGS(NUM_USER_ASIDS) * sizeof(unsigned long);
	/*
	 * In case of KPTI kernel/user ASIDs are allocated in
	 * pairs, the bottom bit distinguishes the two: if it
	 * is set, then the ASID will map only userspace. Thus
	 * mark even as reserved for kernel.
	 */
	/* pair 的最低位区分同一 mm 的 kernel/user TLB 命名空间。 */
	memset(map, 0xaa, len);
}

/* rollover 重建 bitmap 基线：优先 pinned，其次 KPTI 保留位，否则全清。 */
static void set_reserved_asid_bits(void)
{
	if (pinned_asid_map)
		bitmap_copy(asid_map, pinned_asid_map, NUM_USER_ASIDS);
	else if (arm64_kernel_unmapped_at_el0())
		set_kpti_asid_bits(asid_map);
	else
		bitmap_clear(asid_map, 0, NUM_USER_ASIDS);
}

#define asid_gen_match(asid) \
	(!(((asid) ^ atomic64_read(&asid_generation)) >> asid_bits))
/* 忽略低位编号，只比较 id 是否属于当前 generation。 */

/*
 * 在 cpu_asid_lock 下完成 rollover 记账。以 pinned/KPTI 重置 bitmap，atomic
 * xchg 每 CPU active 为 0，把旧 active/reserved 低位重新保留，最后给所有
 * CPU 设置 pending。实际 TLB flush 延迟到各 CPU 下次慢路径切换，避免锁内 IPI。
 */
static void flush_context(void)
{
	int i;
	u64 asid;

	/* Update the list of reserved ASIDs and the ASID bitmap. */
	/* 先铺好 pinned/KPTI 基线，后续循环再把各 CPU 仍可能缓存的编号叠加进去。 */
	set_reserved_asid_bits();

	for_each_possible_cpu(i) {
		asid = atomic64_xchg_relaxed(&per_cpu(active_asids, i), 0);
		/*
		 * If this CPU has already been through a
		 * rollover, but hasn't run another task in
		 * the meantime, we must preserve its reserved
		 * ASID, as this is the only trace we have of
		 * the process it is still running.
		 */
		/* active==0 不代表 CPU 无上下文，而可能表示它已被前一次 rollover 摘走。 */
		if (asid == 0)
			/* 已被此前 rollover 清零且未运行新任务，只能沿用 reserved 线索。 */
			asid = per_cpu(reserved_asids, i);
		__set_bit(ctxid2asid(asid), asid_map);
		per_cpu(reserved_asids, i) = asid;
	}

	/*
	 * Queue a TLB invalidation for each CPU to perform on next
	 * context-switch
	 */
	/* pending mask 把昂贵的全 CPU shootdown 改成各 CPU 下一次切换时本地清理。 */
	cpumask_setall(&tlb_flush_pending);
}

/* 扫描所有 CPU，把匹配旧 id 的 reserved 槽更新为同低位的新代 id。 */
static bool check_update_reserved_asid(u64 asid, u64 newasid)
{
	int cpu;
	bool hit = false;

	/*
	 * Iterate over the set of reserved ASIDs looking for a match.
	 * If we find one, then we can update our mm to use newasid
	 * (i.e. the same ASID in the current generation) but we can't
	 * exit the loop early, since we need to ensure that all copies
	 * of the old ASID are updated to reflect the mm. Failure to do
	 * so could result in us missing the reserved ASID in a future
	 * generation.
	 */
	/* 一个多线程 mm 的旧 id 可在多个 CPU reserved 槽出现，不能提前退出。 */
	for_each_possible_cpu(cpu) {
		if (per_cpu(reserved_asids, cpu) == asid) {
			hit = true;
			per_cpu(reserved_asids, cpu) = newasid;
		}
	}

	return hit;
}

/*
 * 在 cpu_asid_lock 下为 mm 取得当前代 id。依次尝试 reserved、pinned、旧低位
 * 复用和 next-fit 空槽；耗尽时递增 generation 并 flush_context。返回完整
 * context id，调用者负责写 mm->context.id。
 */
static u64 new_context(struct mm_struct *mm)
{
	static u32 cur_idx = 1;
	u64 asid = atomic64_read(&mm->context.id);
	u64 generation = atomic64_read(&asid_generation);

	if (asid != 0) {
		u64 newasid = asid2ctxid(ctxid2asid(asid), generation);

		/*
		 * If our current ASID was active during a rollover, we
		 * can continue to use it and this was just a false alarm.
		 */
		/* reserved 命中说明低位编号仍被保留，只需把软件 generation 更新为当前代。 */
		if (check_update_reserved_asid(asid, newasid))
			return newasid;

		/*
		 * If it is pinned, we can keep using it. Note that reserved
		 * takes priority, because even if it is also pinned, we need to
		 * update the generation into the reserved_asids.
		 */
		/* reserved 优先于 pinned，以便同时更新所有 per-CPU generation。 */
		if (refcount_read(&mm->context.pinned))
			return newasid;

		/*
		 * We had a valid ASID in a previous life, so try to re-use
		 * it if possible.
		 */
		/* bitmap 原子置位前为 0 才可复用；已占用则必须进入通用分配路径。 */
		if (!__test_and_set_bit(ctxid2asid(asid), asid_map))
			return newasid;
	}

	/*
	 * Allocate a free ASID. If we can't find one, take a note of the
	 * currently active ASIDs and mark the TLBs as requiring flushes.  We
	 * always count from ASID #2 (index 1), as we use ASID #0 when setting
	 * a reserved TTBR0 for the init_mm and we allocate ASIDs in even/odd
	 * pairs.
	 */
	/* cur_idx 受同一锁保护，next-fit 避免每次从 bitmap 起点扫描。 */
	asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, cur_idx);
	if (asid != NUM_USER_ASIDS)
		goto set_asid;

	/* We're out of ASIDs, so increment the global generation count */
	/* 锁提供对象间顺序，relaxed atomic 只需原子更新供 fast path 读取。 */
	generation = atomic64_add_return_relaxed(ASID_FIRST_VERSION,
						 &asid_generation);
	flush_context();

	/* We have more ASIDs than CPUs, so this will always succeed */
	/* pinned 上限保证保留所有 CPU 状态后仍至少有一个空槽。 */
	asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, 1);

set_asid:
	__set_bit(asid, asid_map);
	cur_idx = asid;
	return asid2ctxid(asid, generation);
}

/*
 * 调度切换到 mm。fast path 用本 CPU active_asids cmpxchg 无锁发布当前代 id；
 * 与 rollover 竞争或代数过期则持 raw spinlock 分配新 id、消费本 CPU pending
 * flush 并发布 active。最后做分支预测硬化，非软件 PAN 模式立即写 TTBR。
 */
void check_and_switch_context(struct mm_struct *mm)
{
	unsigned long flags;
	unsigned int cpu;
	u64 asid, old_active_asid;

	if (system_supports_cnp())
		cpu_set_reserved_ttbr0();

	asid = atomic64_read(&mm->context.id);

	/*
	 * The memory ordering here is subtle.
	 * If our active_asids is non-zero and the ASID matches the current
	 * generation, then we update the active_asids entry with a relaxed
	 * cmpxchg. Racing with a concurrent rollover means that either:
	 *
	 * - We get a zero back from the cmpxchg and end up waiting on the
	 *   lock. Taking the lock synchronises with the rollover and so
	 *   we are forced to see the updated generation.
	 *
	 * - We get a valid ASID back from the cmpxchg, which means the
	 *   relaxed xchg in flush_context will treat us as reserved
	 *   because atomic RmWs are totally ordered for a given location.
	 */
	/* 正确性依赖 xchg/cmpxchg 在同一 per-CPU atomic 上的全序。 */
	old_active_asid = atomic64_read(this_cpu_ptr(&active_asids));
	if (old_active_asid && asid_gen_match(asid) &&
	    atomic64_cmpxchg_relaxed(this_cpu_ptr(&active_asids),
				     old_active_asid, asid))
		goto switch_mm_fastpath;

	raw_spin_lock_irqsave(&cpu_asid_lock, flags);
	/* Check that our ASID belongs to the current generation. */
	/* fast path 失败后在全局锁内复核，避免并发 rollover 后依据旧 generation 分配。 */
	asid = atomic64_read(&mm->context.id);
	if (!asid_gen_match(asid)) {
		asid = new_context(mm);
		atomic64_set(&mm->context.id, asid);
	}

	cpu = smp_processor_id();
	if (cpumask_test_and_clear_cpu(cpu, &tlb_flush_pending))
		/* 延迟清除本 CPU 的所有旧 generation TLB，不向其他 CPU 发 IPI。 */
		local_flush_tlb_all();

	atomic64_set(this_cpu_ptr(&active_asids), asid);
	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);

switch_mm_fastpath:

	arm64_apply_bp_hardening();

	/*
	 * Defer TTBR0_EL1 setting for user threads to uaccess_enable() when
	 * emulating PAN.
	 */
	/* 软件 PAN 内核态保持 reserved TTBR0，uaccess/异常返回路径再恢复用户表。 */
	if (!system_uses_ttbr0_pan())
		cpu_switch_mm(mm->pgd, mm);
}

/*
 * 固定 mm 的低位 ASID，供 KVM/SMMU 等长期用户使用。返回 0 表示不可固定，
 * 否则返回外部应使用的 ASID（KPTI 时带 user bit）。成功增加 pinned 引用，
 * 首次固定在锁内确保当前代 id并置 bitmap，必须以 context_put 配对。
 */
unsigned long arm64_mm_context_get(struct mm_struct *mm)
{
	unsigned long flags;
	u64 asid;

	if (!pinned_asid_map)
		return 0;

	raw_spin_lock_irqsave(&cpu_asid_lock, flags);

	asid = atomic64_read(&mm->context.id);

	if (refcount_inc_not_zero(&mm->context.pinned))
		/* 已固定时只增引用，不改变低位编号。 */
		goto out_unlock;

	if (nr_pinned_asids >= max_pinned_asids) {
		asid = 0;
		goto out_unlock;
	}

	if (!asid_gen_match(asid)) {
		/*
		 * We went through one or more rollover since that ASID was
		 * used. Ensure that it is still valid, or generate a new one.
		 */
		/* pinned 请求也不能直接沿用旧软件 id；new_context 会保住其固定低位 ASID。 */
		asid = new_context(mm);
		atomic64_set(&mm->context.id, asid);
	}

	nr_pinned_asids++;
	__set_bit(ctxid2asid(asid), pinned_asid_map);
	refcount_set(&mm->context.pinned, 1);

out_unlock:
	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);

	asid = ctxid2asid(asid);

	/* Set the equivalent of USER_ASID_BIT */
	/* 外部访问用户页表应使用 KPTI pair 的 user 编号。 */
	if (asid && arm64_kernel_unmapped_at_el0())
		asid |= 1;

	return asid;
}
EXPORT_SYMBOL_GPL(arm64_mm_context_get);

/*
 * 释放一次 pinned 引用。最后引用清 pinned bitmap 和全局计数；当前代普通
 * asid_map 不立即清，交给 rollover 回收。mm 生命周期由调用者保证。
 */
void arm64_mm_context_put(struct mm_struct *mm)
{
	unsigned long flags;
	u64 asid = atomic64_read(&mm->context.id);

	if (!pinned_asid_map)
		return;

	raw_spin_lock_irqsave(&cpu_asid_lock, flags);

	if (refcount_dec_and_test(&mm->context.pinned)) {
		__clear_bit(ctxid2asid(asid), pinned_asid_map);
		nr_pinned_asids--;
	}

	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);
}
EXPORT_SYMBOL_GPL(arm64_mm_context_put);

/* Errata workaround post TTBRx_EL1 update. */
/* 仅受 Cavium 27456 影响的 CPU 把 alternative nop 替换为 IC IALLU+屏障。 */
asmlinkage void post_ttbr_update_workaround(void)
{
	if (!IS_ENABLED(CONFIG_CAVIUM_ERRATUM_27456))
		return;

	asm(ALTERNATIVE("nop; nop; nop",
			"ic iallu; dsb nsh; isb",
			ARM64_WORKAROUND_CAVIUM_27456));
}

/*
 * 安装 pgd_phys/mm 到当前 CPU TTBR0/TTBR1。TCR.A1 使 ASID 主字段位于 TTBR1；
 * 软件 PAN 还在 TTBR0 保存副本。顺序先 reserved TTBR0，再写新 TTBR1/0、
 * ISB 和 erratum workaround，避免过渡期间使用错误用户翻译。
 */
void cpu_do_switch_mm(phys_addr_t pgd_phys, struct mm_struct *mm)
{
	unsigned long ttbr1 = read_sysreg(ttbr1_el1);
	unsigned long asid = ASID(mm);
	unsigned long ttbr0 = phys_to_ttbr(pgd_phys);

	/* Skip CNP for the reserved ASID */
	/* ASID0 空表不是正常共享地址空间，不能设置 Common-not-Private。 */
	if (system_supports_cnp() && asid)
		ttbr0 |= TTBRx_EL1_CnP;

	/* SW PAN needs a copy of the ASID in TTBR0 for entry */
	/* 软件 PAN 的异常入口需从保留 TTBR0 中恢复同一 ASID，故在两侧各保存一份。 */
	if (IS_ENABLED(CONFIG_ARM64_SW_TTBR0_PAN))
		ttbr0 |= FIELD_PREP(TTBRx_EL1_ASID_MASK, asid);

	/* Set ASID in TTBR1 since TCR.A1 is set */
	/* TCR.A1 选择 TTBR1.ASID 作为硬件当前 ASID，必须在写 TTBR0 前保持一致。 */
	ttbr1 &= ~TTBRx_EL1_ASID_MASK;
	ttbr1 |= FIELD_PREP(TTBRx_EL1_ASID_MASK, asid);

	cpu_set_reserved_ttbr0_nosync();
	write_sysreg(ttbr1, ttbr1_el1);
	write_sysreg(ttbr0, ttbr0_el1);
	isb();
	post_ttbr_update_workaround();
}

/*
 * capability 最终确定后计算真实可用容量和 pinned 上限。KPTI 容量减半；
 * 除 ASID0 与 possible CPU reserved 槽外还必须留一个 rollover 空槽。
 */
static int asids_update_limit(void)
{
	unsigned long num_available_asids = NUM_USER_ASIDS;

	if (arm64_kernel_unmapped_at_el0()) {
		num_available_asids /= 2;
		if (pinned_asid_map)
			set_kpti_asid_bits(pinned_asid_map);
	}
	/*
	 * Expect allocation after rollover to fail if we don't have at least
	 * one more ASID than CPUs. ASID #0 is reserved for init_mm.
	 */
	/* 每 CPU 最坏各占一个 reserved，再额外需要一个可分配编号才能保证前进。 */
	WARN_ON(num_available_asids - 1 <= num_possible_cpus());
	pr_info("ASID allocator initialised with %lu entries\n",
		num_available_asids);

	/*
	 * There must always be an ASID available after rollover. Ensure that,
	 * even if all CPUs have a reserved ASID and the maximum number of ASIDs
	 * are pinned, there still is at least one empty slot in the ASID map.
	 */
	/* -2 分别代表保留 ASID0 和保证新代分配成功的空槽。 */
	max_pinned_asids = num_available_asids - num_possible_cpus() - 2;
	return 0;
}
arch_initcall(asids_update_limit);

/*
 * early_initcall 初始化 ASID allocator。主 bitmap 分配失败 panic；pinned map
 * 失败允许禁用固定功能。CPU caps 未 final，构建支持 KPTI 时先保守预留 pair，
 * 之后 rollover 再按最终能力重建。
 */
static int asids_init(void)
{
	asid_bits = get_cpu_asid_bits();
	atomic64_set(&asid_generation, ASID_FIRST_VERSION);
	asid_map = bitmap_zalloc(NUM_USER_ASIDS, GFP_KERNEL);
	if (!asid_map)
		panic("Failed to allocate bitmap for %lu ASIDs\n",
		      NUM_USER_ASIDS);

	pinned_asid_map = bitmap_zalloc(NUM_USER_ASIDS, GFP_KERNEL);
	nr_pinned_asids = 0;

	/*
	 * We cannot call set_reserved_asid_bits() here because CPU
	 * caps are not finalized yet, so it is safer to assume KPTI
	 * and reserve kernel ASID's from beginning.
	 */
	/* 过度预留只损失短期容量；低估预留会造成 kernel/user TLB 编号冲突。 */
	if (IS_ENABLED(CONFIG_UNMAP_KERNEL_AT_EL0))
		set_kpti_asid_bits(asid_map);
	return 0;
}
early_initcall(asids_init);
