// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the interrupt descriptor management code. Detailed
 * information is available in Documentation/core-api/genericirq.rst
 *
 */
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/maple_tree.h>
#include <linux/irqdomain.h>
#include <linux/sysfs.h>
#include <linux/string_choices.h>

#include "internals.h"

/*
 * lockdep: we want to handle all irq_desc locks as a single lock-class:
 */
static struct lock_class_key irq_desc_lock_class;

/*
 * irq_default_affinity - 中断默认 CPU 亲和性掩码
 *
 * 决定新注册的中断默认可以在哪些 CPU 上被投递和处理。
 * 可通过内核命令行参数 irqaffinity=<cpulist> 设置，
 * 例如 irqaffinity=0-3 表示所有中断默认只在 CPU 0~3 上处理。
 * 若未指定，默认为所有 CPU（cpumask_setall）。
 */
#if defined(CONFIG_SMP)
/*
 * irq_affinity_setup - 解析 irqaffinity= 命令行参数
 *
 * 在极早期（__setup 阶段）解析用户指定的 CPU 列表，写入 irq_default_affinity。
 * 同时强制将 boot CPU 加入掩码：防止用户指定了不含任何在线 CPU 的掩码，
 * 导致无法投递中断（至少保证 boot CPU 始终可以接收中断）。
 */
static int __init irq_affinity_setup(char *str)
{
	alloc_bootmem_cpumask_var(&irq_default_affinity);
	cpulist_parse(str, irq_default_affinity);
	/*
	 * Set at least the boot cpu. We don't want to end up with
	 * bugreports caused by random commandline masks
	 */
	cpumask_set_cpu(smp_processor_id(), irq_default_affinity);
	return 1;
}
__setup("irqaffinity=", irq_affinity_setup);

/*
 * init_irq_default_affinity - 初始化中断默认 CPU 亲和性掩码（SMP 版本）
 *
 * 若用户未通过 irqaffinity= 参数指定（irq_default_affinity 尚未分配），
 * 分配并初始化为全 CPU 掩码（所有 CPU 都可处理中断）。
 * 若已通过命令行分配但为空（解析结果为空集），同样重置为全 CPU 掩码，
 * 防止系统启动后没有任何 CPU 能接收中断。
 */
static void __init init_irq_default_affinity(void)
{
	/* irq_affinity_setup() 已在命令行阶段分配并填写，此处跳过。 */
	if (!cpumask_available(irq_default_affinity))
		zalloc_cpumask_var(&irq_default_affinity, GFP_NOWAIT);
	/* 兜底：若掩码为空，回退到允许所有 CPU。 */
	if (cpumask_empty(irq_default_affinity))
		cpumask_setall(irq_default_affinity);
}
#else
/* UP（单 CPU）系统不需要亲和性掩码，空实现。 */
static void __init init_irq_default_affinity(void)
{
}
#endif

#ifdef CONFIG_SMP
static int alloc_masks(struct irq_desc *desc, int node)
{
	if (!zalloc_cpumask_var_node(&desc->irq_common_data.affinity,
				     GFP_KERNEL, node))
		return -ENOMEM;

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	if (!zalloc_cpumask_var_node(&desc->irq_common_data.effective_affinity,
				     GFP_KERNEL, node)) {
		free_cpumask_var(desc->irq_common_data.affinity);
		return -ENOMEM;
	}
#endif

#ifdef CONFIG_GENERIC_PENDING_IRQ
	if (!zalloc_cpumask_var_node(&desc->pending_mask, GFP_KERNEL, node)) {
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
		free_cpumask_var(desc->irq_common_data.effective_affinity);
#endif
		free_cpumask_var(desc->irq_common_data.affinity);
		return -ENOMEM;
	}
#endif
	return 0;
}

static void irq_redirect_work(struct irq_work *work)
{
	handle_irq_desc(container_of(work, struct irq_desc, redirect.work));
}

static void desc_smp_init(struct irq_desc *desc, int node, const struct cpumask *affinity)
{
	if (!affinity)
		affinity = irq_default_affinity;
	cpumask_copy(desc->irq_common_data.affinity, affinity);

#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_clear(desc->pending_mask);
#endif
#ifdef CONFIG_NUMA
	desc->irq_common_data.node = node;
#endif
	desc->redirect.work = IRQ_WORK_INIT_HARD(irq_redirect_work);
}

static void free_masks(struct irq_desc *desc)
{
#ifdef CONFIG_GENERIC_PENDING_IRQ
	free_cpumask_var(desc->pending_mask);
#endif
	free_cpumask_var(desc->irq_common_data.affinity);
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	free_cpumask_var(desc->irq_common_data.effective_affinity);
#endif
}

#else
static inline int
alloc_masks(struct irq_desc *desc, int node) { return 0; }
static inline void
desc_smp_init(struct irq_desc *desc, int node, const struct cpumask *affinity) { }
static inline void free_masks(struct irq_desc *desc) { }
#endif

static void desc_set_defaults(unsigned int irq, struct irq_desc *desc, int node,
			      const struct cpumask *affinity, struct module *owner)
{
	desc->irq_common_data.handler_data = NULL;
	desc->irq_common_data.msi_desc = NULL;

	desc->irq_data.common = &desc->irq_common_data;
	desc->irq_data.irq = irq;
	desc->irq_data.chip = &no_irq_chip;
	desc->irq_data.chip_data = NULL;
	irq_settings_clr_and_set(desc, ~0, _IRQ_DEFAULT_INIT_FLAGS);
	irqd_set(&desc->irq_data, IRQD_IRQ_DISABLED);
	irqd_set(&desc->irq_data, IRQD_IRQ_MASKED);
	desc->handle_irq = handle_bad_irq;
	desc->depth = 1;
	desc->irq_count = 0;
	desc->irqs_unhandled = 0;
	desc->tot_count = 0;
	desc->name = NULL;
	desc->owner = owner;
	rcuref_init(&desc->refcnt, 1);
	desc_smp_init(desc, node, affinity);
}

unsigned int total_nr_irqs __read_mostly = NR_IRQS;

/**
 * irq_get_nr_irqs() - Number of interrupts supported by the system.
 */
unsigned int irq_get_nr_irqs(void)
{
	return total_nr_irqs;
}
EXPORT_SYMBOL_GPL(irq_get_nr_irqs);

/**
 * irq_set_nr_irqs() - Set the number of interrupts supported by the system.
 * @nr: New number of interrupts.
 *
 * Return: @nr.
 */
unsigned int __init irq_set_nr_irqs(unsigned int nr)
{
	total_nr_irqs = nr;
	irq_proc_calc_prec();
	return nr;
}

static DEFINE_MUTEX(sparse_irq_lock);
static struct maple_tree sparse_irqs = MTREE_INIT_EXT(sparse_irqs,
					MT_FLAGS_ALLOC_RANGE |
					MT_FLAGS_LOCK_EXTERN |
					MT_FLAGS_USE_RCU,
					sparse_irq_lock);

static int irq_find_free_area(unsigned int from, unsigned int cnt)
{
	MA_STATE(mas, &sparse_irqs, 0, 0);

	if (mas_empty_area(&mas, from, MAX_SPARSE_IRQS, cnt))
		return -ENOSPC;
	return mas.index;
}

struct irq_desc *irq_find_desc_at_or_after(unsigned int offset)
{
	unsigned long index = offset;

	lockdep_assert_in_rcu_read_lock();
	return mt_find(&sparse_irqs, &index, total_nr_irqs);
}

/*
 * irq_insert_desc - 将 irq_desc 插入全局 sparse_irqs maple tree
 * @irq:  中断号（作为 maple tree 的键）
 * @desc: 要插入的 irq_desc 指针
 *
 * sparse_irqs 是一棵以中断号为键的 maple tree，用于在中断号空间稀疏
 * （不连续）时高效存取 irq_desc，避免为所有可能的中断号分配连续数组。
 * MA_STATE 宏创建 maple tree 操作状态，指定在 irq 这个键的位置存储 desc。
 */
static void irq_insert_desc(unsigned int irq, struct irq_desc *desc)
{
	MA_STATE(mas, &sparse_irqs, irq, irq);
	WARN_ON(mas_store_gfp(&mas, desc, GFP_KERNEL) != 0);
}

static void delete_irq_desc(unsigned int irq)
{
	MA_STATE(mas, &sparse_irqs, irq, irq);
	mas_erase(&mas);
}

#ifdef CONFIG_SPARSE_IRQ
static const struct kobj_type irq_kobj_type;
#endif

/*
 * init_desc - 初始化一个 irq_desc 结构体的所有字段
 * @desc:     已分配但未初始化的 irq_desc 指针
 * @irq:      中断号
 * @node:     NUMA 节点（用于内存分配亲和性）
 * @flags:    初始 irq_data 标志位
 * @affinity: 初始 CPU 亲和性掩码，NULL 表示使用 irq_default_affinity
 * @owner:    拥有该中断的内核模块（通常为 NULL）
 *
 * 分配 per-cpu 统计结构、CPU 亲和性掩码，初始化锁、互斥量、等待队列，
 * 并通过 desc_set_defaults() 设置默认处理函数和 irq_data 字段。
 */
static int init_desc(struct irq_desc *desc, int irq, int node,
		     unsigned int flags,
		     const struct cpumask *affinity,
		     struct module *owner)
{
	/* 分配 per-cpu 中断统计计数器（irqstat），记录每个 CPU 上该中断的触发次数，
	 * 用于 /proc/interrupts 和 perf 统计。 */
	desc->kstat_irqs = alloc_percpu(struct irqstat);
	if (!desc->kstat_irqs)
		return -ENOMEM;

	/* 分配 SMP 亲和性掩码（affinity、effective_affinity、pending_mask），
	 * 指定该中断可以/实际在哪些 CPU 上处理。UP 系统此函数为空。 */
	if (alloc_masks(desc, node)) {
		free_percpu(desc->kstat_irqs);
		return -ENOMEM;
	}

	/* 初始化 irq_desc 的主锁，所有对 desc 的修改操作都需持此锁。
	 * 所有 irq_desc 共用同一个 lockdep 锁类（irq_desc_lock_class），
	 * 使 lockdep 把它们视为同一种锁，避免误报死锁。 */
	raw_spin_lock_init(&desc->lock);
	lockdep_set_class(&desc->lock, &irq_desc_lock_class);

	/* 保护 request_irq/free_irq 不并发执行的互斥锁。 */
	mutex_init(&desc->request_mutex);

	/* 等待中断线程（threaded IRQ）退出的等待队列，
	 * free_irq() 需要等待所有 IRQ 线程完成后才能释放 desc。 */
	init_waitqueue_head(&desc->wait_for_threads);

	/* 设置默认处理函数（handle_bad_irq）、irq_data（硬件 irq 号、domain 等）
	 * 和 CPU 亲和性掩码（使用传入的 affinity 或 irq_default_affinity）。 */
	desc_set_defaults(irq, desc, node, affinity, owner);

	/* 将初始标志位写入 irq_data.state_use_accessors，
	 * 例如 IRQ_NOAUTOEN（注册后不自动使能）等标志。 */
	irqd_set(&desc->irq_data, flags);

	/* 初始化中断重发（resend）机制：某些中断控制器在边沿触发模式下，
	 * 若中断到来时被屏蔽，需要软件模拟重发以防止中断丢失。 */
	irq_resend_init(desc);

#ifdef CONFIG_SPARSE_IRQ
	/* 初始化 sysfs kobject（对应 /sys/kernel/irq/N/ 目录）和 RCU head，
	 * RCU head 用于延迟释放 desc（free_irq 后通过 RCU 安全释放）。 */
	kobject_init(&desc->kobj, &irq_kobj_type);
	init_rcu_head(&desc->rcu);
#endif

	return 0;
}

#ifdef CONFIG_SPARSE_IRQ

static void irq_kobj_release(struct kobject *kobj);

#ifdef CONFIG_SYSFS
static struct kobject *irq_kobj_base;

#define IRQ_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static ssize_t per_cpu_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	ssize_t ret = 0;
	char *p = "";
	int cpu;

	for_each_possible_cpu(cpu) {
		unsigned int c = irq_desc_kstat_cpu(desc, cpu);

		ret += sysfs_emit_at(buf, ret, "%s%u", p, c);
		p = ",";
	}

	ret += sysfs_emit_at(buf, ret, "\n");
	return ret;
}
IRQ_ATTR_RO(per_cpu_count);

static ssize_t chip_name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	if (desc->irq_data.chip && desc->irq_data.chip->name)
		return sysfs_emit(buf, "%s\n", desc->irq_data.chip->name);
	return 0;
}
IRQ_ATTR_RO(chip_name);

static ssize_t hwirq_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	if (desc->irq_data.domain)
		return sysfs_emit(buf, "%lu\n", desc->irq_data.hwirq);
	return 0;
}
IRQ_ATTR_RO(hwirq);

static ssize_t type_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	return sysfs_emit(buf, "%s\n", irqd_is_level_type(&desc->irq_data) ? "level" : "edge");

}
IRQ_ATTR_RO(type);

static ssize_t wakeup_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	return sysfs_emit(buf, "%s\n", str_enabled_disabled(irqd_is_wakeup_set(&desc->irq_data)));
}
IRQ_ATTR_RO(wakeup);

static ssize_t name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	if (desc->name)
		return sysfs_emit(buf, "%s\n", desc->name);
	return 0;
}
IRQ_ATTR_RO(name);

static ssize_t actions_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	struct irqaction *action;
	ssize_t ret = 0;
	char *p = "";

	scoped_guard(raw_spinlock_irq, &desc->lock) {
		for_each_action_of_desc(desc, action) {
			ret += sysfs_emit_at(buf, ret, "%s%s", p, action->name);
			p = ",";
		}
	}

	if (ret)
		ret += sysfs_emit_at(buf, ret, "\n");
	return ret;
}
IRQ_ATTR_RO(actions);

static struct attribute *irq_attrs[] = {
	&per_cpu_count_attr.attr,
	&chip_name_attr.attr,
	&hwirq_attr.attr,
	&type_attr.attr,
	&wakeup_attr.attr,
	&name_attr.attr,
	&actions_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(irq);

static const struct kobj_type irq_kobj_type = {
	.release	= irq_kobj_release,
	.sysfs_ops	= &kobj_sysfs_ops,
	.default_groups = irq_groups,
};

static void irq_sysfs_add(int irq, struct irq_desc *desc)
{
	if (irq_kobj_base) {
		/*
		 * Continue even in case of failure as this is nothing
		 * crucial and failures in the late irq_sysfs_init()
		 * cannot be rolled back.
		 */
		if (kobject_add(&desc->kobj, irq_kobj_base, "%d", irq))
			pr_warn("Failed to add kobject for irq %d\n", irq);
		else
			desc->istate |= IRQS_SYSFS;
	}
}

static void irq_sysfs_del(struct irq_desc *desc)
{
	/*
	 * Only invoke kobject_del() when kobject_add() was successfully
	 * invoked for the descriptor. This covers both early boot, where
	 * sysfs is not initialized yet, and the case of a failed
	 * kobject_add() invocation.
	 */
	if (desc->istate & IRQS_SYSFS)
		kobject_del(&desc->kobj);
}

static int __init irq_sysfs_init(void)
{
	struct irq_desc *desc;
	int irq;

	/* Prevent concurrent irq alloc/free */
	guard(mutex)(&sparse_irq_lock);
	irq_kobj_base = kobject_create_and_add("irq", kernel_kobj);
	if (!irq_kobj_base)
		return -ENOMEM;

	/* Add the already allocated interrupts */
	for_each_irq_desc(irq, desc)
		irq_sysfs_add(irq, desc);
	return 0;
}
postcore_initcall(irq_sysfs_init);

#else /* !CONFIG_SYSFS */

static const struct kobj_type irq_kobj_type = {
	.release	= irq_kobj_release,
};

static void irq_sysfs_add(int irq, struct irq_desc *desc) {}
static void irq_sysfs_del(struct irq_desc *desc) {}

#endif /* CONFIG_SYSFS */

struct irq_desc *irq_to_desc(unsigned int irq)
{
	return mtree_load(&sparse_irqs, irq);
}
#ifdef CONFIG_KVM_BOOK3S_64_HV_MODULE
EXPORT_SYMBOL_GPL(irq_to_desc);
#endif

void irq_lock_sparse(void)
{
	mutex_lock(&sparse_irq_lock);
}

void irq_unlock_sparse(void)
{
	mutex_unlock(&sparse_irq_lock);
}

/*
 * alloc_desc - 分配并初始化一个 irq_desc 结构体
 * @irq:      中断号
 * @node:     NUMA 节点，内存从该节点分配（减少跨节点访问延迟）
 * @flags:    初始 irq_data 标志位（如 IRQ_NOAUTOEN）
 * @affinity: 初始 CPU 亲和性掩码，NULL 表示使用默认掩码
 * @owner:    拥有该中断的内核模块
 *
 * kzalloc_node 在指定 NUMA 节点分配并清零内存，然后调用 init_desc 完成
 * 所有字段的初始化。分配失败或初始化失败时返回 NULL。
 */
static struct irq_desc *alloc_desc(int irq, int node, unsigned int flags,
				   const struct cpumask *affinity,
				   struct module *owner)
{
	struct irq_desc *desc;
	int ret;

	/* 在 NUMA 节点 node 上分配 irq_desc，并清零所有字段。
	 * NUMA 亲和分配使中断处理路径上的数据访问尽量在本地内存节点完成。 */
	desc = kzalloc_node(sizeof(*desc), GFP_KERNEL, node);
	if (!desc)
		return NULL;

	/* 初始化所有子字段（per-cpu 统计、亲和性掩码、锁、默认处理函数等），
	 * 失败时释放已分配的 desc 内存并返回 NULL。 */
	ret = init_desc(desc, irq, node, flags, affinity, owner);
	if (unlikely(ret)) {
		kfree(desc);
		return NULL;
	}

	return desc;
}

static void irq_kobj_release(struct kobject *kobj)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	free_masks(desc);
	free_percpu(desc->kstat_irqs);
	kfree(desc);
}

static void delayed_free_desc(struct rcu_head *rhp)
{
	struct irq_desc *desc = container_of(rhp, struct irq_desc, rcu);

	kobject_put(&desc->kobj);
}

void irq_desc_free_rcu(struct irq_desc *desc)
{
	/*
	 * We free the descriptor, masks and stat fields via RCU. That
	 * allows demultiplex interrupts to do rcu based management of
	 * the child interrupts.
	 * This also allows us to use rcu in kstat_irqs_usr().
	 */
	call_rcu(&desc->rcu, delayed_free_desc);
}

static void free_desc(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	irq_remove_debugfs_entry(desc);
	unregister_irq_proc(irq, desc);

	/*
	 * sparse_irq_lock protects also show_interrupts() and
	 * kstat_irq_usr(). Once we deleted the descriptor from the
	 * sparse tree we can free it. Access in proc will fail to
	 * lookup the descriptor.
	 *
	 * The sysfs entry must be serialized against a concurrent
	 * irq_sysfs_init() as well.
	 */
	irq_sysfs_del(desc);
	delete_irq_desc(irq);
	irq_desc_put_ref(desc);
}

static int alloc_descs(unsigned int start, unsigned int cnt, int node,
		       const struct irq_affinity_desc *affinity,
		       struct module *owner)
{
	struct irq_desc *desc;
	int i;

	/* Validate affinity mask(s) */
	if (affinity) {
		for (i = 0; i < cnt; i++) {
			if (cpumask_empty(&affinity[i].mask))
				return -EINVAL;
		}
	}

	for (i = 0; i < cnt; i++) {
		const struct cpumask *mask = NULL;
		unsigned int flags = 0;

		if (affinity) {
			if (affinity->is_managed) {
				flags = IRQD_AFFINITY_MANAGED |
					IRQD_MANAGED_SHUTDOWN;
			}
			flags |= IRQD_AFFINITY_SET;
			mask = &affinity->mask;
			node = cpu_to_node(cpumask_first(mask));
			affinity++;
		}

		desc = alloc_desc(start + i, node, flags, mask, owner);
		if (!desc)
			goto err;
		irq_insert_desc(start + i, desc);
		irq_sysfs_add(start + i, desc);
		irq_add_debugfs_entry(start + i, desc);
	}
	return start;

err:
	for (i--; i >= 0; i--)
		free_desc(start + i);
	return -ENOMEM;
}

static bool irq_expand_nr_irqs(unsigned int nr)
{
	if (nr > MAX_SPARSE_IRQS)
		return false;
	total_nr_irqs = nr;
	irq_proc_calc_prec();
	return true;
}

/*
 * early_irq_init - 中断子系统最早期初始化（CONFIG_SPARSE_IRQ 版本）
 *
 * 调用时机：start_kernel() 极早期，内存分配器就绪后，中断控制器驱动初始化前。
 * 此时只需建立 irq_desc 的基础框架，实际硬件中断号由后续 irqchip_init() 确定。
 *
 * SPARSE_IRQ 模式：中断号空间可能很稀疏（不连续，如 0~1023 中只有少数几个使用），
 * 用 maple tree（sparse_irqs）动态存储 irq_desc，按需分配，避免为所有可能的
 * 中断号预分配大量 irq_desc 浪费内存。
 *
 * 初始化完成后 IRQ 框架可接受中断注册（request_irq），但中断控制器尚未启动，
 * 实际中断还不会触发。
 */
int __init early_irq_init(void)
{
	int i, initcnt, node = first_online_node;
	struct irq_desc *desc;

	/* 初始化中断默认 CPU 亲和性掩码，确保新注册的中断有合法的投递目标 CPU。 */
	init_irq_default_affinity();

	/* Let arch update nr_irqs and return the nr of preallocated irqs
	 *
	 * 询问架构需要预分配多少个 irq_desc：
	 *   - x86：根据 IOAPIC 数量和 MSI 配置计算，需要提前确定
	 *   - arm64：返回 NR_IRQS_LEGACY（通常 16），真正的 GIC 中断号
	 *     在 irqchip_init() → gic_init() 时才从设备树动态发现
	 *
	 * total_nr_irqs 是运行时总中断号上限，NR_IRQS 是编译时上限。 */
	initcnt = arch_probe_nr_irqs();
	printk(KERN_INFO "NR_IRQS: %d, nr_irqs: %d, preallocated irqs: %d\n",
	       NR_IRQS, total_nr_irqs, initcnt);

	/* 防御性检查：总数和预分配数均不得超过 sparse irq 的硬上限。 */
	if (WARN_ON(total_nr_irqs > MAX_SPARSE_IRQS))
		total_nr_irqs = MAX_SPARSE_IRQS;

	if (WARN_ON(initcnt > MAX_SPARSE_IRQS))
		initcnt = MAX_SPARSE_IRQS;

	/* 若预分配数超过当前总数，扩大总数上限以容纳预分配的条目。 */
	if (initcnt > total_nr_irqs)
		total_nr_irqs = initcnt;

	/* 预分配 initcnt 个 irq_desc 并插入 sparse_irqs maple tree。
	 * 这些是"保留"中断号（如旧式 ISA 中断 0~15），后续驱动无需再动态分配。
	 * 在 first_online_node 上分配，减少跨 NUMA 节点的内存访问延迟。 */
	for (i = 0; i < initcnt; i++) {
		desc = alloc_desc(i, node, 0, NULL, NULL);
		irq_insert_desc(i, desc);
	}

	/* 根据 total_nr_irqs 计算 /proc/interrupts 输出的数字列宽（对齐宽度），
	 * 例如 total_nr_irqs=1000 时列宽为 4（"%4d"）。 */
	irq_proc_calc_prec();

	/* 调用架构钩子做架构相关的早期中断初始化。
	 * arm64 使用弱符号默认实现（return 0），什么都不做；
	 * x86 等架构可能在此设置 APIC 相关的早期状态。 */
	/*
		为什么 arm64 不需要覆盖这两个函数

		arm64 使用 GICv3/v4 中断控制器，中断号是动态发现的（从设备树或 ACPI MADT 解析），不需要在启动极早期就确定总数。
		真正的 GIC 初始化发生在后续的 irqchip_init() 里，通过设备树驱动框架探测 GIC 并调用 gic_init()，那时才确定支持多少个硬件中断号。
		相比之下，x86 的 arch_probe_nr_irqs 需要覆盖，因为它要根据 IOAPIC 数量和 MSI 配置提前计算中断号总量，在极早期就确定 irq_desc 数组大小。
	*/
	return arch_early_irq_init();
}

#else /* !CONFIG_SPARSE_IRQ */

/*
 * 非 SPARSE_IRQ 模式（CONFIG_SPARSE_IRQ 未开启）：
 * 中断号空间连续且数量固定（NR_IRQS），使用静态数组 irq_desc[] 存储。
 * 编译时已分配好内存（__cacheline_aligned_in_smp 保证 cache line 对齐），
 * 适合中断号数量确定、不需要动态扩展的嵌入式/小型系统。
 */
struct irq_desc irq_desc[NR_IRQS] __cacheline_aligned_in_smp = {
	[0 ... NR_IRQS-1] = {
		.handle_irq	= handle_bad_irq,  /* 默认处理函数：打印警告，中断未注册 */
		.depth		= 1,               /* depth=1 表示初始处于屏蔽状态 */
		.lock		= __RAW_SPIN_LOCK_UNLOCKED(irq_desc->lock),
	}
};

/*
 * early_irq_init - 中断子系统最早期初始化（非 SPARSE_IRQ 版本）
 *
 * 静态数组已在编译时分配，此处只需对每个 irq_desc 调用 init_desc()
 * 完成动态字段的初始化（per-cpu 统计、亲和性掩码、锁等）。
 * 任何一个 init_desc 失败则回滚已初始化的条目并返回错误。
 */
int __init early_irq_init(void)
{
	int count, i, node = first_online_node;
	int ret;

	/* 初始化中断默认 CPU 亲和性掩码。 */
	init_irq_default_affinity();

	pr_info("NR_IRQS: %d\n", NR_IRQS);

	count = ARRAY_SIZE(irq_desc);

	/* 逐个初始化静态数组中的每个 irq_desc。
	 * 主要工作：分配 per-cpu 统计计数器、亲和性掩码，初始化锁和等待队列。 */
	for (i = 0; i < count; i++) {
		ret = init_desc(irq_desc + i, i, node, 0, NULL, NULL);
		if (unlikely(ret))
			goto __free_desc_res;
	}

	/* 计算 /proc/interrupts 输出列宽。 */
	irq_proc_calc_prec();
	return arch_early_irq_init();

__free_desc_res:
	/* 回滚：释放已初始化的 desc 的动态分配资源（亲和性掩码和 per-cpu 统计）。
	 * 静态数组本身不需要 kfree，只需释放其中动态分配的字段。 */
	while (--i >= 0) {
		free_masks(irq_desc + i);
		free_percpu(irq_desc[i].kstat_irqs);
	}

	return ret;
}

struct irq_desc *irq_to_desc(unsigned int irq)
{
	return (irq < NR_IRQS) ? irq_desc + irq : NULL;
}
EXPORT_SYMBOL(irq_to_desc);

static void free_desc(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	int cpu;

	scoped_guard(raw_spinlock_irqsave, &desc->lock)
		desc_set_defaults(irq, desc, irq_desc_get_node(desc), NULL, NULL);

	for_each_possible_cpu(cpu)
		*per_cpu_ptr(desc->kstat_irqs, cpu) = (struct irqstat) { };

	delete_irq_desc(irq);
}

static inline int alloc_descs(unsigned int start, unsigned int cnt, int node,
			      const struct irq_affinity_desc *affinity,
			      struct module *owner)
{
	u32 i;

	for (i = 0; i < cnt; i++) {
		struct irq_desc *desc = irq_to_desc(start + i);

		desc->owner = owner;
		irq_insert_desc(start + i, desc);
	}
	return start;
}

static inline bool irq_expand_nr_irqs(unsigned int nr)
{
	return false;
}

void irq_mark_irq(unsigned int irq)
{
	guard(mutex)(&sparse_irq_lock);
	irq_insert_desc(irq, irq_desc + irq);
}

#endif /* !CONFIG_SPARSE_IRQ */

int handle_irq_desc(struct irq_desc *desc)
{
	struct irq_data *data;

	if (!desc)
		return -EINVAL;

	data = irq_desc_get_irq_data(desc);
	if (WARN_ON_ONCE(!in_hardirq() && irqd_is_handle_enforce_irqctx(data)))
		return -EPERM;

	generic_handle_irq_desc(desc);
	return 0;
}

/**
 * generic_handle_irq - Invoke the handler for a particular irq
 * @irq:	The irq number to handle
 *
 * Returns:	0 on success, or -EINVAL if conversion has failed
 *
 * 		This function must be called from an IRQ context with irq regs
 * 		initialized.
  */
int generic_handle_irq(unsigned int irq)
{
	return handle_irq_desc(irq_to_desc(irq));
}
EXPORT_SYMBOL_GPL(generic_handle_irq);

/**
 * generic_handle_irq_safe - Invoke the handler for a particular irq from any
 *			     context.
 * @irq:	The irq number to handle
 *
 * Returns:	0 on success, a negative value on error.
 *
 * This function can be called from any context (IRQ or process context). It
 * will report an error if not invoked from IRQ context and the irq has been
 * marked to enforce IRQ-context only.
 */
int generic_handle_irq_safe(unsigned int irq)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = handle_irq_desc(irq_to_desc(irq));
	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL_GPL(generic_handle_irq_safe);

#ifdef CONFIG_IRQ_DOMAIN
/**
 * generic_handle_domain_irq - Invoke the handler for a HW irq belonging
 *                             to a domain.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The HW irq number to convert to a logical one
 *
 * Returns:	0 on success, or -EINVAL if conversion has failed
 *
 * 		This function must be called from an IRQ context with irq regs
 * 		initialized.
 */
int generic_handle_domain_irq(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	return handle_irq_desc(irq_resolve_mapping(domain, hwirq));
}
EXPORT_SYMBOL_GPL(generic_handle_domain_irq);

 /**
 * generic_handle_irq_safe - Invoke the handler for a HW irq belonging
 *			     to a domain from any context.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The HW irq number to convert to a logical one
 *
 * Returns:	0 on success, a negative value on error.
 *
 * This function can be called from any context (IRQ or process
 * context). If the interrupt is marked as 'enforce IRQ-context only' then
 * the function must be invoked from hard interrupt context.
 */
int generic_handle_domain_irq_safe(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = handle_irq_desc(irq_resolve_mapping(domain, hwirq));
	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL_GPL(generic_handle_domain_irq_safe);

/**
 * generic_handle_domain_nmi - Invoke the handler for a HW nmi belonging
 *                             to a domain.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The HW irq number to convert to a logical one
 *
 * Returns:	0 on success, or -EINVAL if conversion has failed
 *
 * 		This function must be called from an NMI context with irq regs
 * 		initialized.
 **/
int generic_handle_domain_nmi(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	WARN_ON_ONCE(!in_nmi());
	return handle_irq_desc(irq_resolve_mapping(domain, hwirq));
}

#ifdef CONFIG_SMP
static bool demux_redirect_remote(struct irq_desc *desc)
{
	guard(raw_spinlock)(&desc->lock);
	const struct cpumask *m = irq_data_get_effective_affinity_mask(&desc->irq_data);
	unsigned int target_cpu = READ_ONCE(desc->redirect.target_cpu);

	if (desc->irq_data.chip->irq_pre_redirect)
		desc->irq_data.chip->irq_pre_redirect(&desc->irq_data);

	/*
	 * If the interrupt handler is already running on a CPU that's included
	 * in the interrupt's affinity mask, redirection is not necessary.
	 */
	if (cpumask_test_cpu(smp_processor_id(), m))
		return false;

	/*
	 * The desc->action check protects against IRQ shutdown: __free_irq() sets
	 * desc->action to NULL while holding desc->lock, which we also hold.
	 *
	 * Calling irq_work_queue_on() here is safe w.r.t. CPU unplugging:
	 *   - takedown_cpu() schedules multi_cpu_stop() on all active CPUs,
	 *     including the one that's taken down.
	 *   - multi_cpu_stop() acts like a barrier, which means all active
	 *     CPUs go through MULTI_STOP_DISABLE_IRQ and disable hard IRQs
	 *     *before* the dying CPU runs take_cpu_down() in MULTI_STOP_RUN.
	 *   - Hard IRQs are re-enabled at the end of multi_cpu_stop(), *after*
	 *     the dying CPU has run take_cpu_down() in MULTI_STOP_RUN.
	 *   - Since we run in hard IRQ context, we run either before or after
	 *     take_cpu_down() but never concurrently.
	 *   - If we run before take_cpu_down(), the dying CPU hasn't been marked
	 *     offline yet (it's marked via take_cpu_down() -> __cpu_disable()),
	 *     so the WARN in irq_work_queue_on() can't occur.
	 *   - Furthermore, the work item we queue will be flushed later via
	 *     take_cpu_down() -> cpuhp_invoke_callback_range_nofail() ->
	 *     smpcfd_dying_cpu() -> irq_work_run().
	 *   - If we run after take_cpu_down(), target_cpu has been already
	 *     updated via take_cpu_down() -> __cpu_disable(), which eventually
	 *     calls irq_do_set_affinity() during IRQ migration. So, target_cpu
	 *     no longer points to the dying CPU in this case.
	 */
	if (desc->action)
		irq_work_queue_on(&desc->redirect.work, target_cpu);

	return true;
}
#else /* CONFIG_SMP */
static bool demux_redirect_remote(struct irq_desc *desc)
{
	return false;
}
#endif

/**
 * generic_handle_demux_domain_irq - Invoke the handler for a hardware interrupt
 *				     of a demultiplexing domain.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The hardware interrupt number to convert to a logical one
 *
 * Returns:	True on success, or false if lookup has failed
 */
bool generic_handle_demux_domain_irq(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	struct irq_desc *desc = irq_resolve_mapping(domain, hwirq);

	if (unlikely(!desc))
		return false;

	if (demux_redirect_remote(desc))
		return true;

	return !handle_irq_desc(desc);
}
EXPORT_SYMBOL_GPL(generic_handle_demux_domain_irq);

#endif

/* Dynamic interrupt handling */

/**
 * irq_free_descs - free irq descriptors
 * @from:	Start of descriptor range
 * @cnt:	Number of consecutive irqs to free
 */
void irq_free_descs(unsigned int from, unsigned int cnt)
{
	int i;

	if (from >= total_nr_irqs || (from + cnt) > total_nr_irqs)
		return;

	guard(mutex)(&sparse_irq_lock);
	for (i = 0; i < cnt; i++)
		free_desc(from + i);
}
EXPORT_SYMBOL_GPL(irq_free_descs);

/**
 * __irq_alloc_descs - allocate and initialize a range of irq descriptors
 * @irq:	Allocate for specific irq number if irq >= 0
 * @from:	Start the search from this irq number
 * @cnt:	Number of consecutive irqs to allocate.
 * @node:	Preferred node on which the irq descriptor should be allocated
 * @owner:	Owning module (can be NULL)
 * @affinity:	Optional pointer to an affinity mask array of size @cnt which
 *		hints where the irq descriptors should be allocated and which
 *		default affinities to use
 *
 * Returns the first irq number or error code
 */
int __ref __irq_alloc_descs(int irq, unsigned int from, unsigned int cnt, int node,
			    struct module *owner, const struct irq_affinity_desc *affinity)
{
	int start;

	if (!cnt)
		return -EINVAL;

	if (irq >= 0) {
		if (from > irq)
			return -EINVAL;
		from = irq;
	} else {
		/*
		 * For interrupts which are freely allocated the
		 * architecture can force a lower bound to the @from
		 * argument. x86 uses this to exclude the GSI space.
		 */
		from = arch_dynirq_lower_bound(from);
	}

	guard(mutex)(&sparse_irq_lock);

	start = irq_find_free_area(from, cnt);
	if (irq >=0 && start != irq)
		return -EEXIST;

	if (start + cnt > total_nr_irqs) {
		if (!irq_expand_nr_irqs(start + cnt))
			return -ENOMEM;
	}
	return alloc_descs(start, cnt, node, affinity, owner);
}
EXPORT_SYMBOL_GPL(__irq_alloc_descs);

/**
 * irq_get_next_irq - get next allocated irq number
 * @offset:	where to start the search
 *
 * Returns next irq number after offset or total_nr_irqs if none is found.
 */
unsigned int irq_get_next_irq(unsigned int offset)
{
	struct irq_desc *desc;

	guard(rcu)();
	desc = irq_find_desc_at_or_after(offset);
	return desc ? irq_desc_get_irq(desc) : total_nr_irqs;
}

struct irq_desc *__irq_get_desc_lock(unsigned int irq, unsigned long *flags, bool bus,
				     unsigned int check)
{
	struct irq_desc *desc;

	desc = irq_to_desc(irq);
	if (!desc)
		return NULL;

	if (check & _IRQ_DESC_CHECK) {
		if ((check & _IRQ_DESC_PERCPU) && !irq_settings_is_per_cpu_devid(desc))
			return NULL;

		if (!(check & _IRQ_DESC_PERCPU) && irq_settings_is_per_cpu_devid(desc))
			return NULL;
	}

	if (bus)
		chip_bus_lock(desc);
	raw_spin_lock_irqsave(&desc->lock, *flags);

	return desc;
}

void __irq_put_desc_unlock(struct irq_desc *desc, unsigned long flags, bool bus)
	__releases(&desc->lock)
{
	raw_spin_unlock_irqrestore(&desc->lock, flags);
	if (bus)
		chip_bus_sync_unlock(desc);
}

int irq_set_percpu_devid(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || desc->percpu_enabled)
		return -EINVAL;

	desc->percpu_enabled = kzalloc_obj(*desc->percpu_enabled);

	if (!desc->percpu_enabled)
		return -ENOMEM;

	irq_set_percpu_devid_flags(irq);
	return 0;
}

void kstat_incr_irq_this_cpu(unsigned int irq)
{
	kstat_incr_irqs_this_cpu(irq_to_desc(irq));
}

/**
 * kstat_irqs_cpu - Get the statistics for an interrupt on a cpu
 * @irq:	The interrupt number
 * @cpu:	The cpu number
 *
 * Returns the sum of interrupt counts on @cpu since boot for
 * @irq. The caller must ensure that the interrupt is not removed
 * concurrently.
 */
unsigned int kstat_irqs_cpu(unsigned int irq, int cpu)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc && desc->kstat_irqs ? per_cpu(desc->kstat_irqs->cnt, cpu) : 0;
}

static unsigned int kstat_irqs_desc(struct irq_desc *desc, const struct cpumask *cpumask)
{
	unsigned int sum = 0;
	int cpu;

	if (!irq_settings_is_per_cpu_devid(desc) &&
	    !irq_settings_is_per_cpu(desc) &&
	    !irq_is_nmi(desc))
		return data_race(desc->tot_count);

	for_each_cpu(cpu, cpumask)
		sum += data_race(per_cpu(desc->kstat_irqs->cnt, cpu));
	return sum;
}

static unsigned int kstat_irqs(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !desc->kstat_irqs)
		return 0;
	return kstat_irqs_desc(desc, cpu_possible_mask);
}

#ifdef CONFIG_GENERIC_IRQ_STAT_SNAPSHOT

void kstat_snapshot_irqs(void)
{
	struct irq_desc *desc;
	unsigned int irq;

	for_each_irq_desc(irq, desc) {
		if (!desc->kstat_irqs)
			continue;
		this_cpu_write(desc->kstat_irqs->ref, this_cpu_read(desc->kstat_irqs->cnt));
	}
}

unsigned int kstat_get_irq_since_snapshot(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !desc->kstat_irqs)
		return 0;
	return this_cpu_read(desc->kstat_irqs->cnt) - this_cpu_read(desc->kstat_irqs->ref);
}

#endif

/**
 * kstat_irqs_usr - Get the statistics for an interrupt from thread context
 * @irq:	The interrupt number
 *
 * Returns the sum of interrupt counts on all cpus since boot for @irq.
 *
 * It uses rcu to protect the access since a concurrent removal of an
 * interrupt descriptor is observing an rcu grace period before
 * delayed_free_desc()/irq_kobj_release().
 */
unsigned int kstat_irqs_usr(unsigned int irq)
{
	unsigned int sum;

	rcu_read_lock();
	sum = kstat_irqs(irq);
	rcu_read_unlock();
	return sum;
}

#ifdef CONFIG_LOCKDEP
void __irq_set_lockdep_class(unsigned int irq, struct lock_class_key *lock_class,
			     struct lock_class_key *request_class)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc) {
		lockdep_set_class(&desc->lock, lock_class);
		lockdep_set_class(&desc->request_mutex, request_class);
	}
}
EXPORT_SYMBOL_GPL(__irq_set_lockdep_class);
#endif
