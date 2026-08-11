// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2017 Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>

#include <linux/spinlock.h>
#include <linux/seq_file.h>
#include <linux/bitmap.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/irq.h>

/*
 * 单 CPU 位图及计数。alloc_map 表示已实际占用（普通与 managed 共用），managed_map
 * 表示预留给 managed IRQ 的槽；两者交集数量由 managed_allocated 记录。available
 * 只统计 alloc 区间内可供普通分配的槽，已排除 system、managed 预留和普通占用。
 * initialized 让 CPU 再上线时保留离线期间仍存在/释放的位图，online 决定其 available
 * 是否计入全局值。managed_map 指向 alloc_map 尾部的第二张等长位图。
 */
struct cpumap {
	unsigned int		available;
	unsigned int		allocated;
	unsigned int		managed;
	unsigned int		managed_allocated;
	bool			initialized;
	bool			online;
	unsigned long		*managed_map;
	unsigned long		alloc_map[];
};

/*
 * 整个跨 CPU 分配器。alloc_start/end 定义半开搜索区间，system_map 是所有 CPU 都
 * 不可分配的位，scratch_map 是所有操作共享的临时并集；maps 拥有每 CPU cpumap。
 * global_available 只汇总 online map 的 available，global_reserved 是尚未兑现到具体
 * CPU/bit 的普通预留额度，total_allocated 是诊断计数，online_maps 跟踪在线 map 数。
 *
 * 本文件不内置互斥；除明确只读的 debug 快照外，调用者必须用同一外部锁串行所有
 * 位图/计数操作。共享 scratch_map 尤其禁止并发分配或 managed remove。
 */
struct irq_matrix {
	unsigned int		matrix_bits;
	unsigned int		alloc_start;
	unsigned int		alloc_end;
	unsigned int		alloc_size;
	unsigned int		global_available;
	unsigned int		global_reserved;
	unsigned int		systembits_inalloc;
	unsigned int		total_allocated;
	unsigned int		online_maps;
	struct cpumap __percpu	*maps;
	unsigned long		*system_map;
	unsigned long		scratch_map[];
};

#define CREATE_TRACE_POINTS
#include <trace/events/irq_matrix.h>

/**
 * irq_alloc_matrix - Allocate a irq_matrix structure and initialize it
 * @matrix_bits:	Number of matrix bits
 * @alloc_start:	From which bit the allocation search starts
 * @alloc_end:		At which bit the allocation search ends, i.e first
 *			invalid bit
 */
/*
 * 原文契约：分配并初始化 matrix，@matrix_bits 是完整位图宽度，普通/managed 搜索
 * 只发生在 [@alloc_start,@alloc_end)。调用者必须保证 start<=end<=matrix_bits；本函数
 * 不验证范围。主体尾部同时容纳 scratch/system 两张位图，每 CPU 对象尾部容纳
 * alloc/managed 两张位图，并修正各自第二张图的指针。
 *
 * 主体或 per-CPU 分配失败返回 NULL，并只释放本函数已取得资源；成功返回永久拥有
 * 两层分配的 @m。接口没有 destroy 配对，面向早期 IRQ 控制器初始化并以 __init 调用。
 */
__init struct irq_matrix *irq_alloc_matrix(unsigned int matrix_bits,
					   unsigned int alloc_start,
					   unsigned int alloc_end)
{
	unsigned int cpu, matrix_size = BITS_TO_LONGS(matrix_bits);
	struct irq_matrix *m;

	m = kzalloc_flex(*m, scratch_map, matrix_size * 2);
	if (!m)
		return NULL;

	m->system_map = &m->scratch_map[matrix_size];

	m->matrix_bits = matrix_bits;
	m->alloc_start = alloc_start;
	m->alloc_end = alloc_end;
	m->alloc_size = alloc_end - alloc_start;
	m->maps = __alloc_percpu(struct_size(m->maps, alloc_map, matrix_size * 2),
				 __alignof__(*m->maps));
	if (!m->maps) {
		kfree(m);
		return NULL;
	}

	for_each_possible_cpu(cpu) {
		struct cpumap *cm = per_cpu_ptr(m->maps, cpu);

		cm->managed_map = &cm->alloc_map[matrix_size];
	}

	return m;
}

/**
 * irq_matrix_online - Bring the local CPU matrix online
 * @m:		Matrix pointer
 */
/*
 * 原文契约：把当前 CPU 的 map 纳入全局分配。首次上线按 alloc_size 减去 managed
 * 预留和分配区内 system 位计算 available；再次上线保留离线期位图/计数。随后把本地
 * available 加入 global_available，置 online 并增加 online_maps。重复上线是 BUG。
 * 调用者在目标 CPU 上、持矩阵外部锁进入，无返回值。
 */
void irq_matrix_online(struct irq_matrix *m)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);

	BUG_ON(cm->online);

	if (!cm->initialized) {
		cm->available = m->alloc_size;
		cm->available -= cm->managed + m->systembits_inalloc;
		cm->initialized = true;
	}
	m->global_available += cm->available;
	cm->online = true;
	m->online_maps++;
	trace_irq_matrix_online(m);
}

/**
 * irq_matrix_offline - Bring the local CPU matrix offline
 * @m:		Matrix pointer
 */
/*
 * 原文契约：把当前 CPU map 从全局候选移除。先从 global_available 扣除本地可用量，
 * 再清 online、减少 online_maps；位图与本地计数全部保留，支持 dormant 分配稍后释放
 * 或 CPU 再上线。调用者必须保证 map 当前在线并持外部锁，本函数没有防御检查。
 */
void irq_matrix_offline(struct irq_matrix *m)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);

	/* Update the global available size */
	/* 离线首先从全局可用总量移除此 CPU 的贡献。 */
	m->global_available -= cm->available;
	cm->online = false;
	m->online_maps--;
	trace_irq_matrix_offline(m);
}

/*
 * 在一个 CPU map 的普通分配区查找 @num 个连续空位。先把 managed、system、alloc
 * 三图 OR 到共享 scratch，找到空区后按 @managed 写入 managed_map 或 alloc_map。
 * 成功返回起始 bit，失败返回 >=alloc_end；只改位图，不更新任何计数。调用者持全局
 * 矩阵锁，并负责在失败/成功后维护与位图一致的计数。
 */
static unsigned int matrix_alloc_area(struct irq_matrix *m, struct cpumap *cm,
				      unsigned int num, bool managed)
{
	unsigned int area, start = m->alloc_start;
	unsigned int end = m->alloc_end;

	bitmap_or(m->scratch_map, cm->managed_map, m->system_map, end);
	bitmap_or(m->scratch_map, m->scratch_map, cm->alloc_map, end);
	area = bitmap_find_next_zero_area(m->scratch_map, end, start, num, 0);
	if (area >= end)
		return area;
	if (managed)
		bitmap_set(cm->managed_map, area, num);
	else
		bitmap_set(cm->alloc_map, area, num);
	return area;
}

/* Find the best CPU which has the lowest vector allocation count */
/*
 * 原文意为选择向量占用最少的 CPU；实现以 available 最大作为等价负载指标，只考虑
 * @msk 中 online 且至少有一个可用槽的 map，同值保留先遇到者。没有候选返回 UINT_MAX。
 * 它不预占位，调用者必须继续用 matrix_alloc_area() 验证实际空洞。
 */
static unsigned int matrix_find_best_cpu(struct irq_matrix *m,
					const struct cpumask *msk)
{
	unsigned int cpu, best_cpu, maxavl = 0;
	struct cpumap *cm;

	best_cpu = UINT_MAX;

	for_each_cpu(cpu, msk) {
		cm = per_cpu_ptr(m->maps, cpu);

		if (!cm->online || cm->available <= maxavl)
			continue;

		best_cpu = cpu;
		maxavl = cm->available;
	}
	return best_cpu;
}

/* Find the best CPU which has the lowest number of managed IRQs allocated */
/*
 *  managed 分配按 managed_allocated 最少选 CPU。只考虑 @msk 中 online map；
 * 比较使用 “>” 因而相同负载时后遇到者胜出。选择器不验证尚有未兑现 managed 位，
 * 后续位图扫描仍可能返回 -ENOSPC；无 online 候选返回 UINT_MAX。
 */
static unsigned int matrix_find_best_cpu_managed(struct irq_matrix *m,
						const struct cpumask *msk)
{
	unsigned int cpu, best_cpu, allocated = UINT_MAX;
	struct cpumap *cm;

	best_cpu = UINT_MAX;

	for_each_cpu(cpu, msk) {
		cm = per_cpu_ptr(m->maps, cpu);

		if (!cm->online || cm->managed_allocated > allocated)
			continue;

		best_cpu = cpu;
		allocated = cm->managed_allocated;
	}
	return best_cpu;
}

/**
 * irq_matrix_assign_system - Assign system wide entry in the matrix
 * @m:		Matrix pointer
 * @bit:	Which bit to reserve
 * @replace:	Replace an already allocated vector with a system
 *		vector at the same bit position.
 *
 * The BUG_ON()s below are on purpose. If this goes wrong in the
 * early boot process, then the chance to survive is about zero.
 * If this happens when the system is life, it's not much better.
 */
/*
 * 原文契约：把 @bit 设为全系统保留位；@replace=true 表示它原先是当前 CPU 上已分配
 * 的普通向量，需要先从 alloc_map/allocated/total_allocated 撤账。刻意使用 BUG_ON：
 * 该操作若在早期启动出错几乎无法继续，运行期出错同样致命。最多允许一个 online map，
 * 且已有 online map 时必须 replace。
 *
 * @bit 的真实前提是 bit<matrix_bits 且未重复设为 system；现有检查只拒绝 “>”，不会
 * 拒绝等于 matrix_bits，调用者不能依赖该不完整防线。若位于分配区，增加
 * systembits_inalloc；replace 的槽仍被 system 占据，所以不增加 available。
 */
void irq_matrix_assign_system(struct irq_matrix *m, unsigned int bit,
			      bool replace)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);

	BUG_ON(bit > m->matrix_bits);
	BUG_ON(m->online_maps > 1 || (m->online_maps && !replace));

	set_bit(bit, m->system_map);
	if (replace) {
		BUG_ON(!test_and_clear_bit(bit, cm->alloc_map));
		cm->allocated--;
		m->total_allocated--;
	}
	if (bit >= m->alloc_start && bit < m->alloc_end)
		m->systembits_inalloc++;

	trace_irq_matrix_assign_system(bit, m);
}

/**
 * irq_matrix_reserve_managed - Reserve a managed interrupt in a CPU map
 * @m:		Matrix pointer
 * @msk:	On which CPUs the bits should be reserved.
 *
 * Can be called for offline CPUs. Note, this will only reserve one bit
 * on all CPUs in @msk, but it's not guaranteed that the bits are at the
 * same offset on all CPUs
 */
/*
 * 原文契约：在 @msk 每个 CPU map 各预留一个 managed bit，可针对 offline CPU；各 CPU
 * 只保证一位，不保证 offset 相同。每次成功置 managed_map 并增加 managed；online map
 * 还要减少本地/全局 available，offline map 留待首次 online 初始化时统一扣除。
 *
 * 任一 CPU 无空位则按掩码顺序撤销此前成功项并返回 -ENOSPC，失败 CPU 未改位图；全部
 * 成功返回 0。调用者持外部锁，@msk 生命周期只覆盖调用。
 */
int irq_matrix_reserve_managed(struct irq_matrix *m, const struct cpumask *msk)
{
	unsigned int cpu, failed_cpu;

	for_each_cpu(cpu, msk) {
		struct cpumap *cm = per_cpu_ptr(m->maps, cpu);
		unsigned int bit;

		bit = matrix_alloc_area(m, cm, 1, true);
		if (bit >= m->alloc_end)
			goto cleanup;
		cm->managed++;
		if (cm->online) {
			cm->available--;
			m->global_available--;
		}
		trace_irq_matrix_reserve_managed(bit, cpu, m, cm);
	}
	return 0;
cleanup:
	failed_cpu = cpu;
	for_each_cpu(cpu, msk) {
		if (cpu == failed_cpu)
			break;
		irq_matrix_remove_managed(m, cpumask_of(cpu));
	}
	return -ENOSPC;
}

/**
 * irq_matrix_remove_managed - Remove managed interrupts in a CPU map
 * @m:		Matrix pointer
 * @msk:	On which CPUs the bits should be removed
 *
 * Can be called for offline CPUs
 *
 * This removes not allocated managed interrupts from the map. It does
 * not matter which one because the managed interrupts free their
 * allocation when they shut down. If not, the accounting is screwed,
 * but all what can be done at this point is warn about it.
 */
/*
 * 原文契约：从 @msk 每个 CPU 移除一个尚未实际分配的 managed 预留，可处理 offline
 * map。具体删哪一位不重要，因为 managed IRQ shutdown 时应先释放 alloc_map 交集；实现
 * 计算 managed_map & ~alloc_map 并清首位。成功减少 managed，online 时归还 available。
 *
 * managed 计数为零或所有 managed 位仍处于 allocated 状态时只能 WARN 并跳过，不能
 * 猜测性清正在使用的槽；此时账目保持原状。函数也是 reserve_managed() 的部分失败回滚器。
 */
void irq_matrix_remove_managed(struct irq_matrix *m, const struct cpumask *msk)
{
	unsigned int cpu;

	for_each_cpu(cpu, msk) {
		struct cpumap *cm = per_cpu_ptr(m->maps, cpu);
		unsigned int bit, end = m->alloc_end;

		if (WARN_ON_ONCE(!cm->managed))
			continue;

		/* Get managed bit which are not allocated */
		/* 只从 managed 预留中挑选尚未出现在 alloc_map 的位。 */
		bitmap_andnot(m->scratch_map, cm->managed_map, cm->alloc_map, end);

		bit = find_first_bit(m->scratch_map, end);
		if (WARN_ON_ONCE(bit >= end))
			continue;

		clear_bit(bit, cm->managed_map);

		cm->managed--;
		if (cm->online) {
			cm->available++;
			m->global_available++;
		}
		trace_irq_matrix_remove_managed(bit, cpu, m, cm);
	}
}

/**
 * irq_matrix_alloc_managed - Allocate a managed interrupt in a CPU map
 * @m:		Matrix pointer
 * @msk:	Which CPUs to search in
 * @mapped_cpu:	Pointer to store the CPU for which the irq was allocated
 */
/*
 * 原文契约：在 @msk 中选择 managed_allocated 最少的 online CPU，再从该 CPU 已预留但
 * 未分配的 managed 位中取首位。空 mask 返回 -EINVAL，无 online 候选或所选 map 没有
 * 空闲 managed 位返回 -ENOSPC；成功把位加入 alloc_map，增加 allocated、
 * managed_allocated、total_allocated，写回 @mapped_cpu 并返回 bit。
 *
 * available 不变：槽在 reserve_managed() 时已经从普通容量中扣除。输出参数仅在成功
 * 后写入，所有状态由调用者外部锁串行。
 */
int irq_matrix_alloc_managed(struct irq_matrix *m, const struct cpumask *msk,
			     unsigned int *mapped_cpu)
{
	unsigned int bit, cpu, end;
	struct cpumap *cm;

	if (cpumask_empty(msk))
		return -EINVAL;

	cpu = matrix_find_best_cpu_managed(m, msk);
	if (cpu == UINT_MAX)
		return -ENOSPC;

	cm = per_cpu_ptr(m->maps, cpu);
	end = m->alloc_end;
	/* Get managed bit which are not allocated */
	/* 候选集合是 managed_map 中尚未进入 alloc_map 的预留位。 */
	bitmap_andnot(m->scratch_map, cm->managed_map, cm->alloc_map, end);
	bit = find_first_bit(m->scratch_map, end);
	if (bit >= end)
		return -ENOSPC;
	set_bit(bit, cm->alloc_map);
	cm->allocated++;
	cm->managed_allocated++;
	m->total_allocated++;
	*mapped_cpu = cpu;
	trace_irq_matrix_alloc_managed(bit, cpu, m, cm);
	return bit;
}

/**
 * irq_matrix_assign - Assign a preallocated interrupt in the local CPU map
 * @m:		Matrix pointer
 * @bit:	Which bit to mark
 *
 * This should only be used to mark preallocated vectors
 */
/*
 * 原文契约：把当前 CPU 上已由平台预分配的普通向量 @bit 纳入矩阵账目，只用于
 * preallocated vector。bit 必须在分配区、尚未在 alloc_map；违规 WARN 并不改状态。
 * 成功置位，增加本地 allocated/全局 total_allocated，并减少本地/全局 available。
 *
 * 调用者必须在 online 当前 CPU 上持外部锁，并保证该位不是 system/managed 预留；
 * 本函数只检查 alloc_map，错误类别会造成位图与计数冲突。
 */
void irq_matrix_assign(struct irq_matrix *m, unsigned int bit)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);

	if (WARN_ON_ONCE(bit < m->alloc_start || bit >= m->alloc_end))
		return;
	if (WARN_ON_ONCE(test_and_set_bit(bit, cm->alloc_map)))
		return;
	cm->allocated++;
	m->total_allocated++;
	cm->available--;
	m->global_available--;
	trace_irq_matrix_assign(bit, smp_processor_id(), m, cm);
}

/**
 * irq_matrix_reserve - Reserve interrupts
 * @m:		Matrix pointer
 *
 * This is merely a book keeping call. It increments the number of globally
 * reserved interrupt bits w/o actually allocating them. This allows to
 * setup interrupt descriptors w/o assigning low level resources to it.
 * The actual allocation happens when the interrupt gets activated.
 */
/*
 * 原文契约：只做全局 bookkeeping，为尚未激活、尚未绑定 CPU/bit 的普通 IRQ 增加一份
 * reservation，使上层可先建 irq_desc，激活时再兑现硬件资源。该调用不改任何位图或
 * available；若 reserved 已等于 available，仍会告警后继续增加，接口无失败返回。
 * 调用者必须保证最终由 reserved=true 的 irq_matrix_alloc() 消费，或显式 remove。
 */
void irq_matrix_reserve(struct irq_matrix *m)
{
	if (m->global_reserved == m->global_available)
		pr_warn("Interrupt reservation exceeds available resources\n");

	m->global_reserved++;
	trace_irq_matrix_reserve(m);
}

/**
 * irq_matrix_remove_reserved - Remove interrupt reservation
 * @m:		Matrix pointer
 *
 * This is merely a book keeping call. It decrements the number of globally
 * reserved interrupt bits. This is used to undo irq_matrix_reserve() when the
 * interrupt was never in use and a real vector allocated, which undid the
 * reservation.
 */
/*
 * 原文契约：撤销一份从未兑现到真实 vector 的全局 reservation；若已经通过
 * irq_matrix_alloc(reserved=true) 分配，分配路径本身已减计数，不应再调用本函数。
 * 实现直接递减且无下溢检查，因此必须与尚存的 irq_matrix_reserve() 严格一一配对。
 */
void irq_matrix_remove_reserved(struct irq_matrix *m)
{
	m->global_reserved--;
	trace_irq_matrix_remove_reserved(m);
}

/**
 * irq_matrix_alloc - Allocate a regular interrupt in a CPU map
 * @m:		Matrix pointer
 * @msk:	Which CPUs to search in
 * @reserved:	Allocate previously reserved interrupts
 * @mapped_cpu: Pointer to store the CPU for which the irq was allocated
 */
/*
 * 原文契约：在 @msk 的 online map 中选择 available 最大者，为普通 IRQ 分配一个空
 * bit。空 mask 返回 -EINVAL（即使 UP 的 for_each_cpu 会忽略 mask 也必须拦截）；无候选
 * 或无实际空洞返回 -ENOSPC。成功更新 alloc_map、allocated、available 及对应全局计数，
 * @reserved=true 时再消费一份 global_reserved，写回 CPU 并返回 bit。
 *
 * @reserved 必须确有未消费预留，否则无检查递减会下溢；@mapped_cpu 只在成功后写入。
 * 所有选 CPU、scratch 查位和提交步骤依赖同一调用者外部锁保持原子。
 */
int irq_matrix_alloc(struct irq_matrix *m, const struct cpumask *msk,
		     bool reserved, unsigned int *mapped_cpu)
{
	unsigned int cpu, bit;
	struct cpumap *cm;

	/*
	 * Not required in theory, but matrix_find_best_cpu() uses
	 * for_each_cpu() which ignores the cpumask on UP .
	 */
	/*  UP 的 for_each_cpu() 会忽略传入 mask，因此仍需显式拒绝空集合。 */
	if (cpumask_empty(msk))
		return -EINVAL;

	cpu = matrix_find_best_cpu(m, msk);
	if (cpu == UINT_MAX)
		return -ENOSPC;

	cm = per_cpu_ptr(m->maps, cpu);
	bit = matrix_alloc_area(m, cm, 1, false);
	if (bit >= m->alloc_end)
		return -ENOSPC;
	cm->allocated++;
	cm->available--;
	m->total_allocated++;
	m->global_available--;
	if (reserved)
		m->global_reserved--;
	*mapped_cpu = cpu;
	trace_irq_matrix_alloc(bit, cpu, m, cm);
	return bit;

}

/**
 * irq_matrix_free - Free allocated interrupt in the matrix
 * @m:		Matrix pointer
 * @cpu:	Which CPU map needs be updated
 * @bit:	The bit to remove
 * @managed:	If true, the interrupt is managed and not accounted
 *		as available.
 */
/*
 * 原文契约：从指定 @cpu 的 alloc_map 释放 @bit；@managed 必须与该占用的真实类别
 * 一致。越界或位未占用时 WARN 并保持计数不变。成功减少 allocated，managed 时另减
 * managed_allocated 且不归还 available（managed 预留仍在）；普通位增加本地 available，
 * map 在线时也增加 global_available。
 *
 * offline map 仍会清位和修正本地计数，以便再上线时恢复正确容量，但只有 online 时
 * 才减少 total_allocated。由于 offline 本身也不改该字段，离线 dormant 位的后续释放
 * 不会从 total_allocated 扣除；该字段是 trace/debug 账目，不能在此场景当作位图精确权重。
 */
void irq_matrix_free(struct irq_matrix *m, unsigned int cpu,
		     unsigned int bit, bool managed)
{
	struct cpumap *cm = per_cpu_ptr(m->maps, cpu);

	if (WARN_ON_ONCE(bit < m->alloc_start || bit >= m->alloc_end))
		return;

	if (WARN_ON_ONCE(!test_and_clear_bit(bit, cm->alloc_map)))
		return;

	cm->allocated--;
	if(managed)
		cm->managed_allocated--;

	if (cm->online)
		m->total_allocated--;

	if (!managed) {
		cm->available++;
		if (cm->online)
			m->global_available++;
	}
	trace_irq_matrix_free(bit, cpu, m, cm);
}

/**
 * irq_matrix_available - Get the number of globally available irqs
 * @m:		Pointer to the matrix to query
 * @cpudown:	If true, the local CPU is about to go down, adjust
 *		the number of available irqs accordingly
 */
/*
 * 原文契约：查询全局普通可用槽数。@cpudown=false 直接返回 global_available；true
 * 表示当前 CPU 即将离线，预先扣除其 cm->available，供热插拔判断其余 CPU 是否足以
 * 接纳待迁移 IRQ。cpudown 模式要求当前 map 仍在线，否则可能错误扣减/下溢。
 * 返回是调用者外部锁下的数值快照，不预留容量。
 */
unsigned int irq_matrix_available(struct irq_matrix *m, bool cpudown)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);

	if (!cpudown)
		return m->global_available;
	return m->global_available - cm->available;
}

/**
 * irq_matrix_reserved - Get the number of globally reserved irqs
 * @m:		Pointer to the matrix to query
 */
/* 返回尚未兑现到具体 CPU/bit 的全局普通预留数；只读快照，不消费 reservation。 */
unsigned int irq_matrix_reserved(struct irq_matrix *m)
{
	return m->global_reserved;
}

/**
 * irq_matrix_allocated - Get the number of allocated non-managed irqs on the local CPU
 * @m:		Pointer to the matrix to search
 *
 * This returns number of allocated non-managed interrupts.
 */
/*
 * 原文契约：返回当前 CPU alloc_map 计数中排除 managed_allocated 后的普通占用数，供
 * CPU 下线容量检查估算需要迁移的 vector。调用者须在目标 CPU、矩阵外部锁下读取；
 * 返回计数而非位图，也不包含单纯 managed 预留。
 */
unsigned int irq_matrix_allocated(struct irq_matrix *m)
{
	struct cpumap *cm = this_cpu_ptr(m->maps);

	return cm->allocated - cm->managed_allocated;
}

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
/**
 * irq_matrix_debug_show - Show detailed allocation information
 * @sf:		Pointer to the seq_file to print to
 * @m:		Pointer to the matrix allocator
 * @ind:	Indentation for the print format
 *
 * Note, this is a lockless snapshot.
 */
/*
 * 原文强调这是无矩阵锁快照。函数输出 online map 数、全局 available/reserved、诊断
 * total_allocated、system 位图及每个 online CPU 的 available/managed/
 * managed_allocated/allocated/alloc_map。cpus_read_lock() 只稳定 CPU online 遍历，不能
 * 阻止分配器计数和位图并发变化，因此同一行各字段也不保证事务一致，仅供调试观察。
 */
void irq_matrix_debug_show(struct seq_file *sf, struct irq_matrix *m, int ind)
{
	unsigned int nsys = bitmap_weight(m->system_map, m->matrix_bits);
	int cpu;

	seq_printf(sf, "Online bitmaps:   %6u\n", m->online_maps);
	seq_printf(sf, "Global available: %6u\n", m->global_available);
	seq_printf(sf, "Global reserved:  %6u\n", m->global_reserved);
	seq_printf(sf, "Total allocated:  %6u\n", m->total_allocated);
	seq_printf(sf, "System: %u: %*pbl\n", nsys, m->matrix_bits,
		   m->system_map);
	seq_printf(sf, "%*s| CPU | avl | man | mac | act | vectors\n", ind, " ");
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct cpumap *cm = per_cpu_ptr(m->maps, cpu);

		seq_printf(sf, "%*s %4d  %4u  %4u  %4u %4u  %*pbl\n", ind, " ",
			   cpu, cm->available, cm->managed,
			   cm->managed_allocated, cm->allocated,
			   m->matrix_bits, cm->alloc_map);
	}
	cpus_read_unlock();
}
#endif
