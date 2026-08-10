// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016 Thomas Gleixner.
 * Copyright (C) 2016-2017 Christoph Hellwig.
 */
/*
 * 本文件为多队列 MSI/MSI-X 等 IRQ 构造自动 affinity 描述。向量空间分成三段：开头
 * pre_vectors、需要在 CPU 间分摊的中段、结尾 post_vectors。中段还能由驱动 calc_sets()
 * 划成最多 IRQ_AFFINITY_MAX_SETS 个独立集合，例如分别表示读/写/轮询队列。
 *
 * 输出数组每项持有 cpumask 值副本；中段标为 managed，由 IRQ 核心管理目标，用户空间
 * 不能随意覆盖。group_cpus_evenly() 优先考虑 present CPU、NUMA/cluster 局部性，同时覆盖
 * possible CPU；无法形成请求数量的有效 mask 时，剩余向量回退 irq_default_affinity。
 */
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/group_cpus.h>

/*
 * default_calc_sets() - 为未提供驱动策略的调用建立一个通用 affinity 集合
 *
 * @affd: 输入输出的需求描述，由调用者拥有。
 * @affvecs: 可分配 affinity 的中段向量数。
 * 设置 nr_sets=1、set_size[0]=affvecs；无返回值。即使 affvecs 为 0 也执行，使调用者看到
 * 与实际可用向量一致的集合状态。
 */
static void default_calc_sets(struct irq_affinity *affd, unsigned int affvecs)
{
	affd->nr_sets = 1;
	affd->set_size[0] = affvecs;
}

/**
 * irq_create_affinity_masks - Create affinity masks for multiqueue spreading
 * @nvecs:	The total number of vectors
 * @affd:	Description of the affinity requirements
 *
 * Returns the irq_affinity_desc pointer or NULL if allocation failed.
 */
/*
 * 为总计 @nvecs 个向量创建用于多队列分摊的 affinity 描述数组。
 * @affd 提供首尾保留数以及可选 calc_sets；函数会安装默认回调并调用它，因此 @affd 的
 * nr_sets/set_size/calc_sets 可能被修改。若首尾已耗尽向量，仍调用 calc_sets(0) 让驱动
 * 调整，随后返回 NULL；集合数越界或任何内存分配失败也返回 NULL。
 * calc_sets 回调必须保证每个已声明集合的 set_size 非零，且期望总量不越过 affvecs；本层
 * 只校验 nr_sets 上限，不替错误回调做数组边界修正。
 *
 * 成功时返回含 @nvecs 项的堆数组，调用者负责 kfree。pre/post 及未被有效分组覆盖的项
 * 复制 irq_default_affinity；每个集合分别调用 group_cpus_evenly()，只复制实际初始化的
 * nr_masks。最终整个 `[pre_vectors, nvecs-post_vectors)` 中段均标 is_managed，包括回退
 * 默认 mask 的中段项。函数在可睡眠上下文分配内存，不持 CPU hotplug 锁。
 */
struct irq_affinity_desc *
irq_create_affinity_masks(unsigned int nvecs, struct irq_affinity *affd)
{
	/* affvecs 是中段容量；curvec 为下一写入下标；usedvecs 累计有效分组 mask 数。 */
	unsigned int affvecs, curvec, usedvecs, i;
	/* masks 在成功时转交调用者；任一中途失败由本函数释放。 */
	struct irq_affinity_desc *masks = NULL;

	/*
	 * Determine the number of vectors which need interrupt affinities
	 * assigned. If the pre/post request exhausts the available vectors
	 * then nothing to do here except for invoking the calc_sets()
	 * callback so the device driver can adjust to the situation.
	 */
	/*
	 * 先扣除不参与分摊的 pre/post 向量；若保留数已耗尽总数，中段为 0，但仍
	 * 必须调用 calc_sets()，让驱动知道退化后的实际容量并调整内部队列集合。
	 */
	if (nvecs > affd->pre_vectors + affd->post_vectors)
		affvecs = nvecs - affd->pre_vectors - affd->post_vectors;
	else
		affvecs = 0;

	/*
	 * Simple invocations do not provide a calc_sets() callback. Install
	 * the generic one.
	 */
	/* 简单调用者未提供集合划分回调时，使用单集合默认策略。 */
	if (!affd->calc_sets)
		affd->calc_sets = default_calc_sets;

	/* Recalculate the sets */
	/* 以本次实际中段容量重新计算集合数量及每组期望向量数。 */
	affd->calc_sets(affd, affvecs);

	if (WARN_ON_ONCE(affd->nr_sets > IRQ_AFFINITY_MAX_SETS))
		return NULL;

	/* Nothing to assign? */
	/* 没有 affinity 中段时不分配描述数组，NULL 表示无需自动分摊。 */
	if (!affvecs)
		return NULL;

	masks = kzalloc_objs(*masks, nvecs);
	if (!masks)
		return NULL;

	/* Fill out vectors at the beginning that don't need affinity */
	/* 开头保留向量不做专用分摊，使用全局默认 affinity 且保持非 managed。 */
	for (curvec = 0; curvec < affd->pre_vectors; curvec++)
		cpumask_copy(&masks[curvec].mask, irq_default_affinity);

	/*
	 * Spread on present CPUs starting from affd->pre_vectors. If we
	 * have multiple sets, build each sets affinity mask separately.
	 */
	/*
	 * 从 pre_vectors 后开始在 present/possible CPU 上均匀分摊；多个集合分别
	 * 计算，避免不同用途的队列共享一套连续分配状态。result 是临时 cpumask 数组，复制后
	 * 立即释放；nr_masks 可能小于 this_vecs。
	 */
	for (i = 0, usedvecs = 0; i < affd->nr_sets; i++) {
		unsigned int nr_masks, this_vecs = affd->set_size[i];
		struct cpumask *result = group_cpus_evenly(this_vecs, &nr_masks);

		if (!result) {
			kfree(masks);
			return NULL;
		}

		for (int j = 0; j < nr_masks; j++)
			cpumask_copy(&masks[curvec + j].mask, &result[j]);
		kfree(result);

		curvec += nr_masks;
		usedvecs += nr_masks;
	}

	/* Fill out vectors at the end that don't need affinity */
	/*
	 * 从“中段末尾”或“实际已使用 mask 末尾”中较合适的位置开始，把所有剩余
	 * 项（包括 post 向量和无法形成独立 mask 的中段向量）填为 irq_default_affinity。
	 */
	if (usedvecs >= affvecs)
		curvec = affd->pre_vectors + affvecs;
	else
		curvec = affd->pre_vectors + usedvecs;
	for (; curvec < nvecs; curvec++)
		cpumask_copy(&masks[curvec].mask, irq_default_affinity);

	/* Mark the managed interrupts */
	/* 仅首尾保留段之外的全部向量交由内核 managed-affinity 机制管理。 */
	for (i = affd->pre_vectors; i < nvecs - affd->post_vectors; i++)
		masks[i].is_managed = 1;

	return masks;
}

/**
 * irq_calc_affinity_vectors - Calculate the optimal number of vectors
 * @minvec:	The minimum number of vectors available
 * @maxvec:	The maximum number of vectors available
 * @affd:	Description of the affinity requirements
 */
/*
 * 在硬件/调用者允许的 [@minvec, @maxvec] 范围内计算适合自动 affinity 的向量
 * 总数。@affd 只读，resv 为 pre+post 保留量。resv 大于 minvec 时返回 0，让上层按不足
 * minvec 处理；有自定义 calc_sets 时允许使用 maxvec-resv 个中段向量，由回调之后细分；
 * 无回调时通用策略把中段上限限制为 possible CPU 数，避免创建多于 CPU 的专用向量。
 * 返回 resv + min(set_vecs, maxvec-resv)，不分配内存、不修改 @affd；调用者应保证
 * maxvec >= minvec 且保留数不会导致无符号下溢。
 */
unsigned int irq_calc_affinity_vectors(unsigned int minvec, unsigned int maxvec,
				       const struct irq_affinity *affd)
{
	/* resv 不参与 CPU 分摊；set_vecs 是中段的策略上限。 */
	unsigned int resv = affd->pre_vectors + affd->post_vectors;
	unsigned int set_vecs;

	if (resv > minvec)
		return 0;

	if (affd->calc_sets)
		set_vecs = maxvec - resv;
	else
		set_vecs = cpumask_weight(cpu_possible_mask);

	return resv + min(set_vecs, maxvec - resv);
}
