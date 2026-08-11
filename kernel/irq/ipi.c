// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Imagination Technologies Ltd
 * Author: Qais Yousef <qais.yousef@imgtec.com>
 *
 * This file contains driver APIs to the IPI subsystem.
 */

#define pr_fmt(fmt) "genirq/ipi: " fmt

#include <linux/irqdomain.h>
#include <linux/irq.h>

/*
 * 通用 IPI API 把“可向哪些 CPU 发送”固化在一段 irq_desc/irq_data 的 affinity 中。
 * IPI domain 有两种硬件模型：single 模型用一个 Linux/HW IRQ 配合目标掩码发送；
 * per-CPU 模型为连续 CPU 集合分配一段连续 virq，每个 CPU 对应独立 irq_data/hwirq。
 * common->ipi_offset 记录 virq 基址对应的首 CPU，用于发送、查询和销毁时换算。
 *
 * reserve/destroy 在可睡眠上下文管理描述符和 irqdomain 资源；send 快路径不加锁，
 * 依赖已保留 IPI 在整个调用期间存活。面向驱动的公开发送入口始终验证目标，架构/
 * 核心使用的双下划线入口只在 DEBUG 配置验证，以减少跨 CPU 热路径开销。
 */

/**
 * irq_reserve_ipi() - Setup an IPI to destination cpumask
 * @domain:	IPI domain
 * @dest:	cpumask of CPUs which can receive the IPI
 *
 * Allocate a virq that can be used to send IPI to any CPU in dest mask.
 *
 * Return: Linux IRQ number on success or error code on failure
 */
/*
 * 在 IPI @domain 上为可接收 CPU 集合 @dest 预留发送资源。
 * domain 必须是 IPI 类型，dest 必须是 cpu_possible_mask 的非空子集。single 模型只
 * 分配一个 virq，允许目标掩码有洞；per-CPU 模型按每个目标 CPU 分配一个 virq，
 * 当前实现要求 dest 连续，并把首 CPU 保存为 offset。
 *
 * 阶段一分配连续 irq_desc，阶段二以 dest 作为 domain alloc 参数建立硬件映射；
 * 成功后给每项复制完整目标 affinity、记录相同 offset，并设置 NO_BALANCING，最后
 * 返回起始 virq。描述符分配失败返回 -ENOMEM，硬件分配失败对外返回 -EBUSY。
 *
 * 当前失败标签使用已经被 __irq_domain_alloc_irqs() 返回值覆盖的 virq；失败时它为
 * 负数，外层 irq_free_descs() 因无符号越界而不做事。不过 irqdomain 内层失败路径
 * 已用原正基址释放 desc，所以当前不会泄漏；外层这次调用只是无效的重复清理。
 */
int irq_reserve_ipi(struct irq_domain *domain,
			     const struct cpumask *dest)
{
	unsigned int nr_irqs, offset;
	struct irq_data *data;
	int virq, i;

	/* 先验证 domain 身份和目标集合，避免分配后才发现调用协议错误。 */
	if (!domain ||!irq_domain_is_ipi(domain)) {
		pr_warn("Reservation on a non IPI domain\n");
		return -EINVAL;
	}

	if (!cpumask_subset(dest, cpu_possible_mask)) {
		pr_warn("Reservation is not in possible_cpu_mask\n");
		return -EINVAL;
	}

	nr_irqs = cpumask_weight(dest);
	if (!nr_irqs) {
		pr_warn("Reservation for empty destination mask\n");
		return -EINVAL;
	}

	if (irq_domain_is_ipi_single(domain)) {
		/*
		 * If the underlying implementation uses a single HW irq on
		 * all cpus then we only need a single Linux irq number for
		 * it. We have no restrictions vs. the destination mask. The
		 * underlying implementation can deal with holes nicely.
		 */
		/* single HW IRQ 由发送回调解释 CPU 掩码，因此只需一个 virq 且允许有洞。 */
		nr_irqs = 1;
		offset = 0;
	} else {
		unsigned int next;

		/*
		 * The IPI requires a separate HW irq on each CPU. We require
		 * that the destination mask is consecutive. If an
		 * implementation needs to support holes, it can reserve
		 * several IPI ranges.
		 */
		/* per-CPU HW IRQ 要求每个目标一项；一个预留区间当前只能表示连续 CPU。 */
		offset = cpumask_first(dest);
		/*
		 * Find a hole and if found look for another set bit after the
		 * hole. For now we don't support this scenario.
		 */
		/* 找到首个洞后再找后续置位；若存在，就拒绝这个非连续掩码。 */
		next = cpumask_next_zero(offset, dest);
		if (next < nr_cpu_ids)
			next = cpumask_next(next, dest);
		if (next < nr_cpu_ids) {
			pr_warn("Destination mask has holes\n");
			return -EINVAL;
		}
	}

	/* 阶段一：预留 Linux 描述符编号，尚未建立 domain 硬件资源。 */
	virq = irq_domain_alloc_descs(-1, nr_irqs, 0, NUMA_NO_NODE, NULL);
	if (virq <= 0) {
		pr_warn("Can't reserve IPI, failed to alloc descs\n");
		return -ENOMEM;
	}

	/* 阶段二：realloc=true 表示复用刚分配的 desc，并让 domain 按 dest 配置。 */
	virq = __irq_domain_alloc_irqs(domain, virq, nr_irqs, NUMA_NO_NODE,
				       (void *) dest, true, NULL);

	if (virq <= 0) {
		pr_warn("Can't reserve IPI, failed to alloc hw irqs\n");
		goto free_descs;
	}

	/* 成功发布公共目标集合、CPU 偏移，并禁止通用 IRQ balancer 改写 IPI affinity。 */
	for (i = 0; i < nr_irqs; i++) {
		data = irq_get_irq_data(virq + i);
		cpumask_copy(data->common->affinity, dest);
		data->common->ipi_offset = offset;
		irq_set_status_flags(virq + i, IRQ_NO_BALANCING);
	}
	return virq;

free_descs:
	/* virq 此时为负；实际 desc 回收已由 irqdomain 内层用原基址完成。 */
	irq_free_descs(virq, nr_irqs);
	return -EBUSY;
}

/**
 * irq_destroy_ipi() - unreserve an IPI that was previously allocated
 * @irq:	Linux IRQ number to be destroyed
 * @dest:	cpumask of CPUs which should have the IPI removed
 *
 * The IPIs allocated with irq_reserve_ipi() are returned to the system
 * destroying all virqs associated with them.
 *
 * Return: %0 on success or error code on failure.
 */
/*
 * 释放先前由 irq_reserve_ipi() 建立的 IPI 资源。
 * @irq 必须指向有效 IPI irq_data，@dest 必须是预留 affinity 的子集。single 模型
 * 始终释放一个 virq；per-CPU 模型用首目标 CPU 与 ipi_offset 把传入 irq 换算到
 * 子区间起点，再按 cpumask_weight(dest) 释放连续 irqdomain/descriptor 范围。
 *
 * 因此 per-CPU 调用者实际必须提供连续子集；代码只检查 subset，带洞 dest 会按
 * “首 CPU + 个数”释放不同的连续区间。成功返回 0，身份、指针或子集错误返回
 * -EINVAL。irq_domain_free_irqs() 负责撤销硬件映射和描述符。
 */
int irq_destroy_ipi(unsigned int irq, const struct cpumask *dest)
{
	struct irq_data *data = irq_get_irq_data(irq);
	const struct cpumask *ipimask;
	struct irq_domain *domain;
	unsigned int nr_irqs;

	if (!irq || !data)
		return -EINVAL;

	domain = data->domain;
	if (WARN_ON(domain == NULL))
		return -EINVAL;

	if (!irq_domain_is_ipi(domain)) {
		pr_warn("Trying to destroy a non IPI domain!\n");
		return -EINVAL;
	}

	ipimask = irq_data_get_affinity_mask(data);
	if (!ipimask || WARN_ON(!cpumask_subset(dest, ipimask)))
		/*
		 * Must be destroying a subset of CPUs to which this IPI
		 * was set up to target
		 */
		/* 销毁目标必须完全落在最初为该 IPI 预留的 CPU 集合内。 */
		return -EINVAL;

	/* per-CPU 模型把 CPU 号换算为连续 virq 子区间；single 模型只拥有一项。 */
	if (irq_domain_is_ipi_per_cpu(domain)) {
		irq = irq + cpumask_first(dest) - data->common->ipi_offset;
		nr_irqs = cpumask_weight(dest);
	} else {
		nr_irqs = 1;
	}

	irq_domain_free_irqs(irq, nr_irqs);
	return 0;
}

/**
 * ipi_get_hwirq - Get the hwirq associated with an IPI to a CPU
 * @irq:	Linux IRQ number
 * @cpu:	the target CPU
 *
 * When dealing with coprocessors IPI, we need to inform the coprocessor of
 * the hwirq it needs to use to receive and send IPIs.
 *
 * Return: hwirq value on success or INVALID_HWIRQ on failure.
 */
/*
 * 查询预留 IPI 对目标 @cpu 使用的硬件中断号。
 * 先验证 @irq 的 irq_data、CPU 范围及 CPU 属于预留 affinity。per-CPU domain 再用
 * cpu - ipi_offset 调整到该 CPU 对应的 virq/irq_data；single domain 直接复用基项，
 * 其发送机制自行编码目标 CPU。成功返回最终 irq_data::hwirq；任一状态缺失返回
 * INVALID_HWIRQ。返回值是数值快照，不增加任何对象引用。
 */
irq_hw_number_t ipi_get_hwirq(unsigned int irq, unsigned int cpu)
{
	struct irq_data *data = irq_get_irq_data(irq);
	const struct cpumask *ipimask;

	if (!data || cpu >= nr_cpu_ids)
		return INVALID_HWIRQ;

	ipimask = irq_data_get_affinity_mask(data);
	if (!ipimask || !cpumask_test_cpu(cpu, ipimask))
		return INVALID_HWIRQ;

	/*
	 * Get the real hardware irq number if the underlying implementation
	 * uses a separate irq per cpu. If the underlying implementation uses
	 * a single hardware irq for all cpus then the IPI send mechanism
	 * needs to take care of the cpu destinations.
	 */
	/* per-CPU 模型取目标 CPU 的真实项；single 模型的一个 hwirq 服务全部目标。 */
	if (irq_domain_is_ipi_per_cpu(data->domain))
		data = irq_get_irq_data(irq + cpu - data->common->ipi_offset);

	return data ? irqd_to_hwirq(data) : INVALID_HWIRQ;
}
EXPORT_SYMBOL_GPL(ipi_get_hwirq);

/*
 * 验证一次 IPI 发送请求的基础对象和目标授权。
 * chip/data 必须存在，chip 至少提供 single 或 mask 回调；@cpu 必须在 CPU 编号范围
 * 内，irq_data affinity 必须存在。@dest 非空时要求整个掩码是预留集合子集，否则
 * 验证单 CPU 属于预留集合。成功 0，任一条件失败 -EINVAL；函数只读状态。
 * 调用者必须保证 IPI 预留资源在检查和实际发送之间不被并发销毁。
 */
static int ipi_send_verify(struct irq_chip *chip, struct irq_data *data,
			   const struct cpumask *dest, unsigned int cpu)
{
	const struct cpumask *ipimask;

	if (!chip || !data)
		return -EINVAL;

	if (!chip->ipi_send_single && !chip->ipi_send_mask)
		return -EINVAL;

	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	ipimask = irq_data_get_affinity_mask(data);
	if (!ipimask)
		return -EINVAL;

	if (dest) {
		if (!cpumask_subset(dest, ipimask))
			return -EINVAL;
	} else {
		if (!cpumask_test_cpu(cpu, ipimask))
			return -EINVAL;
	}
	return 0;
}

/**
 * __ipi_send_single - send an IPI to a target Linux SMP CPU
 * @desc:	pointer to irq_desc of the IRQ
 * @cpu:	destination CPU, must in the destination mask passed to
 *		irq_reserve_ipi()
 *
 * This function is for architecture or core code to speed up IPI sending. Not
 * usable from driver code.
 *
 * Return: %0 on success or negative error number on failure.
 */
/*
 * 架构/IRQ 核心向单个 Linux SMP CPU @cpu 发送 IPI 的低开销入口。
 * @desc 必须来自 irq_reserve_ipi() 且生命周期稳定。仅 DEBUG 构建调用
 * ipi_send_verify()，生产构建信任核心调用者。chip 没有 single 回调时退化为
 * ipi_send_mask(cpumask_of(cpu))；否则 per-CPU domain 先按 ipi_offset 换到目标
 * irq_data，再调用 ipi_send_single()。成功返回 0，DEBUG 验证失败返回 -EINVAL。
 */
int __ipi_send_single(struct irq_desc *desc, unsigned int cpu)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);
	struct irq_chip *chip = irq_data_get_irq_chip(data);

#ifdef DEBUG
	/*
	 * Minimise the overhead by omitting the checks for Linux SMP IPIs.
	 * Since the callers should be arch or core code which is generally
	 * trusted, only check for errors when debugging.
	 */
	/* Linux SMP IPI 是高频核心路径，非 DEBUG 构建省略可信调用者的重复检查。 */
	if (WARN_ON_ONCE(ipi_send_verify(chip, data, NULL, cpu)))
		return -EINVAL;
#endif
	/* provider 只有 mask 发送能力时，用单 CPU 掩码实现相同语义。 */
	if (!chip->ipi_send_single) {
		chip->ipi_send_mask(data, cpumask_of(cpu));
		return 0;
	}

	/* FIXME: Store this information in irqdata flags */
	/* 待把 domain 模型缓存进 irq_data flags；当前每次发送都查询 domain。 */
	if (irq_domain_is_ipi_per_cpu(data->domain) &&
	    cpu != data->common->ipi_offset) {
		/* use the correct data for that cpu */
		/* per-CPU 模型必须换到 @cpu 对应的 virq/irq_data。 */
		unsigned irq = data->irq + cpu - data->common->ipi_offset;

		data = irq_get_irq_data(irq);
	}
	chip->ipi_send_single(data, cpu);
	return 0;
}

/**
 * __ipi_send_mask - send an IPI to target Linux SMP CPU(s)
 * @desc:	pointer to irq_desc of the IRQ
 * @dest:	dest CPU(s), must be a subset of the mask passed to
 *		irq_reserve_ipi()
 *
 * This function is for architecture or core code to speed up IPI sending. Not
 * usable from driver code.
 *
 * Return: %0 on success or negative error number on failure.
 */
/*
 * 架构/IRQ 核心向 @dest 中多个 Linux SMP CPU 发送 IPI 的低开销入口。
 * @desc 和 dest 授权前提同 reserve，仅 DEBUG 构建验证。provider 有 mask 回调时一次
 * 提交完整掩码；否则逐 CPU 调用 single。per-CPU domain 的循环每次按
 * cpu - ipi_offset 选择独立 irq_data，single domain 则对所有 CPU 复用同一 data。
 * 成功返回 0，DEBUG 验证失败返回 -EINVAL；chip 回调本身没有可传播的错误返回。
 */
int __ipi_send_mask(struct irq_desc *desc, const struct cpumask *dest)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);
	struct irq_chip *chip = irq_data_get_irq_chip(data);
	unsigned int cpu;

#ifdef DEBUG
	/*
	 * Minimise the overhead by omitting the checks for Linux SMP IPIs.
	 * Since the callers should be arch or core code which is generally
	 * trusted, only check for errors when debugging.
	 */
	/* 与单目标快路径相同，非 DEBUG 构建信任架构/核心已满足预留契约。 */
	if (WARN_ON_ONCE(ipi_send_verify(chip, data, dest, 0)))
		return -EINVAL;
#endif
	/* 原生 mask 回调可由硬件/provider 自行合并广播。 */
	if (chip->ipi_send_mask) {
		chip->ipi_send_mask(data, dest);
		return 0;
	}

	/* 没有 mask 回调时，按 domain 模型选择逐 CPU 使用独立还是共享 irq_data。 */
	if (irq_domain_is_ipi_per_cpu(data->domain)) {
		unsigned int base = data->irq;

		for_each_cpu(cpu, dest) {
			unsigned irq = base + cpu - data->common->ipi_offset;

			data = irq_get_irq_data(irq);
			chip->ipi_send_single(data, cpu);
		}
	} else {
		for_each_cpu(cpu, dest)
			chip->ipi_send_single(data, cpu);
	}
	return 0;
}

/**
 * ipi_send_single - Send an IPI to a single CPU
 * @virq:	Linux IRQ number from irq_reserve_ipi()
 * @cpu:	destination CPU, must in the destination mask passed to
 *		irq_reserve_ipi()
 *
 * Return: %0 on success or negative error number on failure.
 */
/*
 * 面向普通调用者的单 CPU IPI 发送入口。
 * 用 @virq 查找 desc/data/chip，并始终调用 ipi_send_verify() 验证对象、回调、CPU
 * 范围和预留 affinity；失败告警一次并返回 -EINVAL，成功交给低开销内部入口。
 * 调用者仍须与 irq_destroy_ipi() 串行化，查找不取得长期引用。
 */
int ipi_send_single(unsigned int virq, unsigned int cpu)
{
	struct irq_desc *desc = irq_to_desc(virq);
	struct irq_data *data = desc ? irq_desc_get_irq_data(desc) : NULL;
	struct irq_chip *chip = data ? irq_data_get_irq_chip(data) : NULL;

	if (WARN_ON_ONCE(ipi_send_verify(chip, data, NULL, cpu)))
		return -EINVAL;

	return __ipi_send_single(desc, cpu);
}
EXPORT_SYMBOL_GPL(ipi_send_single);

/**
 * ipi_send_mask - Send an IPI to target CPU(s)
 * @virq:	Linux IRQ number from irq_reserve_ipi()
 * @dest:	dest CPU(s), must be a subset of the mask passed to
 *		irq_reserve_ipi()
 *
 * Return: %0 on success or negative error number on failure.
 */
/*
 * 面向普通调用者的多 CPU IPI 发送入口。
 * 从 @virq 取得 desc/data/chip，验证 @dest 完全属于预留 affinity 且 provider 至少
 * 有一种发送回调；失败告警一次并返回 -EINVAL，成功调用 __ipi_send_mask()。
 * dest 可有洞的能力取决于 reserve/domain 模型，发送本身按掩码逐项或原生广播。
 */
int ipi_send_mask(unsigned int virq, const struct cpumask *dest)
{
	struct irq_desc *desc = irq_to_desc(virq);
	struct irq_data *data = desc ? irq_desc_get_irq_data(desc) : NULL;
	struct irq_chip *chip = data ? irq_data_get_irq_chip(data) : NULL;

	if (WARN_ON_ONCE(ipi_send_verify(chip, data, dest, 0)))
		return -EINVAL;

	return __ipi_send_mask(desc, dest);
}
EXPORT_SYMBOL_GPL(ipi_send_mask);
