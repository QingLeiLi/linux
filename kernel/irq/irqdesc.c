// SPDX-License-Identifier: GPL-2.0
/*
 * irqdesc.c 学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件负责通用 IRQ 层中 struct irq_desc 的“容器管理”：为 Linux 逻辑
 * 中断号创建、初始化、索引、查找和回收描述符，并提供从逻辑 IRQ 或
 * irq_domain 中的硬件号进入流控处理函数的通用入口。中断控制器的寄存器
 * 操作由 irq_chip 完成，边沿/电平流控由 handle_edge_irq()、
 * handle_level_irq() 等完成，request_irq()/free_irq() 的 action 生命周期
 * 则主要位于 manage.c；本文件把这些参与者连接到一个稳定的 irq_desc。
 *
 * 主要路径：
 *   启动：early_irq_init() -> init_desc() -> desc_set_defaults()
 *   动态分配：__irq_alloc_descs() -> alloc_descs() -> alloc_desc()
 *             -> irq_insert_desc()
 *   中断分派：硬件号 -> irq_resolve_mapping() -> handle_irq_desc()
 *             -> desc->handle_irq(desc) -> irqaction 链
 *   动态释放：irq_free_descs() -> free_desc() -> 从 Maple Tree 摘除
 *             -> rcuref 归零 -> RCU 宽限期 -> kobject release -> 释放内存
 *
 * 核心不变量：
 *   1. irq_desc 在进入全局索引前已经完成锁、统计、亲和性和默认流控初始化；
 *   2. sparse_irq_lock 串行化逻辑 IRQ 号区间的分配/释放及 sysfs 的批量建立；
 *   3. desc->lock 保护单个描述符的 action、irq_data 与运行状态，但不代替
 *      描述符的生命周期引用；
 *   4. CONFIG_SPARSE_IRQ 下，Maple Tree 提供按号查找，RCU 与 rcuref 保证
 *      已经开始的 procfs、统计和级联中断读侧不会遇到已释放内存；
 *   5. CONFIG_SPARSE_IRQ 关闭时，描述符来自永久静态数组，“释放”只重置
 *      状态和统计，不回收对象存储。
 *
 * 方案权衡：稀疏模式只为实际使用的 IRQ 分配内存并允许扩展号码空间，但
 * 引入 Maple Tree、引用计数、RCU 和 kobject 的回收协议；静态数组路径
 * 简单且查找为常数地址计算，却固定占用 NR_IRQS 个描述符的空间。
 */
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the interrupt descriptor management code. Detailed
 * information is available in Documentation/core-api/genericirq.rst
 *
 */
/*
 * 原文说明：本文件包含中断描述符管理代码，更完整的通用 IRQ 设计说明
 * 位于 Documentation/core-api/genericirq.rst。这里的“管理”特指
 * irq_desc 的编号空间和生命周期，而不是具体 irq_chip 驱动。
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
/*
 * lockdep 将默认的 desc->lock 都归入同一锁类：不同 IRQ 虽有不同锁实例，
 * 但锁的使用规则相同。这样 lockdep 会按“任意 irq_desc 锁之间”的嵌套
 * 关系检查潜在死锁；需要合法嵌套的特殊 IRQ 可通过
 * __irq_set_lockdep_class() 改用独立锁类。
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
/*
 * @str 是 __setup 借用的 CPU 列表字符串，不可为 NULL，解析结果写入全局
 * irq_default_affinity；函数仅在单线程启动期运行，可能使用 bootmem，
 * 不需要锁。返回 1 表示参数已识别并消费，无 errno 失败类别；副作用持续
 * 到系统结束，后续 init_irq_default_affinity() 负责空集合兜底。
 */
static int __init irq_affinity_setup(char *str)
{
	alloc_bootmem_cpumask_var(&irq_default_affinity);
	cpulist_parse(str, irq_default_affinity);
	/*
	 * Set at least the boot cpu. We don't want to end up with
	 * bugreports caused by random commandline masks
	 */
	/*
	 * 至少加入启动 CPU，避免任意命令行掩码造成无接收 CPU 的
	 * 错误报告。即使解析结果为空或只含尚未在线的 CPU，早期中断仍有落点。
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
/*
 * 入参：无；返回：无直接返回值。仅在启动期调用，不存在并发读者。
 * GFP_NOWAIT 表示此处不能依赖可睡眠回收；代码按启动期必须取得该小块
 * 掩码存储的约束继续使用结果。成功出口保证默认掩码非空，可供后续
 * desc_smp_init() 复制。
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
/*
 * 入参：无；返回：无直接返回值且无副作用。UP 上中断天然只能在唯一 CPU
 * 处理，保留同名 stub 让 early_irq_init() 不需要条件编译调用点。
 */
static void __init init_irq_default_affinity(void)
{
}
#endif

#ifdef CONFIG_SMP
/*
 * alloc_masks() - 为一个动态 irq_desc 分配 SMP 相关的可变 cpumask。
 *
 * @desc 是尚未发布、由 init_desc() 独占的描述符；@node 是首选 NUMA
 * 节点。进程上下文、GFP_KERNEL，允许睡眠。依次分配期望亲和性、可选的
 * 生效亲和性和迁移 pending 掩码；成功返回 0，失败返回 -ENOMEM，并在
 * 本函数内逆序释放此前成功的掩码，因此调用者只需回收其他资源。
 */
static int alloc_masks(struct irq_desc *desc, int node)
{
	/* 第一项是用户/策略请求的目标集合，后续 irq_chip 设置亲和性时读取。 */
	if (!zalloc_cpumask_var_node(&desc->irq_common_data.affinity,
				     GFP_KERNEL, node))
		return -ENOMEM;

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	/*
	 * effective_affinity 记录控制器实际接受的目标集合；它可能比请求掩码窄，
	 * 因而必须是独立存储，不能与 affinity 共用。
	 */
	if (!zalloc_cpumask_var_node(&desc->irq_common_data.effective_affinity,
				     GFP_KERNEL, node)) {
		free_cpumask_var(desc->irq_common_data.affinity);
		return -ENOMEM;
	}
#endif

#ifdef CONFIG_GENERIC_PENDING_IRQ
	/*
	 * pending_mask 暂存 CPU 热插拔/重平衡期间尚未提交给硬件的目标集合。
	 * 此项失败时依配置逆序撤销 effective_affinity 和 affinity。
	 */
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

/*
 * irq_redirect_work() - 在目标 CPU 的 hard irq_work 上下文继续处理级联 IRQ。
 *
 * @work 嵌入在唯一的 irq_desc.redirect 中，container_of() 还原借用的
 * 描述符；排队前后描述符由 IRQ 注册/释放及 synchronize_irqwork() 保证
 * 存活。无直接返回值；副作用是调用该 desc 的流控处理函数。
 */
static void irq_redirect_work(struct irq_work *work)
{
	handle_irq_desc(container_of(work, struct irq_desc, redirect.work));
}

/*
 * desc_smp_init() - 建立描述符的初始亲和性与跨 CPU 重定向状态。
 *
 * @desc 已拥有 alloc_masks() 分配的存储且尚未发布；@node 为 NUMA 节点；
 * @affinity 是可空的只读借用掩码，NULL 表示复制全局默认值。函数不分配
 * 内存、不可失败；清空迁移 pending 状态并初始化可在硬中断中运行的
 * irq_work。调用者仍拥有 desc 及全部掩码。
 */
static void desc_smp_init(struct irq_desc *desc, int node, const struct cpumask *affinity)
{
	/* 必须复制位图，不能保存调用者指针；动态分配传入的临时数组随后可释放。 */
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

/*
 * free_masks() - 释放 alloc_masks() 成功建立的全部 SMP 掩码。
 *
 * @desc 必须已停止被并发使用；无返回值。它只释放子对象，不释放 desc。
 * 调用点分别是动态描述符的最终 kobject release 和静态数组初始化回滚。
 */
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
/*
 * UP 配置没有 CPU 亲和性、迁移和跨 CPU 重定向。这组 stub 保持初始化与
 * 回收调用图一致：@desc/@node/@affinity 均不被消费，分配始终返回成功，
 * 也没有任何副作用。
 */
/*
 * alloc_masks() - UP 配置下省略描述符亲和性掩码分配
 *
 * @desc: 未消费的描述符借用指针。
 * @node: 未消费的 NUMA 节点编号。
 * 返回恒为 0，表示没有需要分配的 SMP 子资源；不睡眠、无所有权变化。
 */
static inline int
alloc_masks(struct irq_desc *desc, int node) { return 0; }
/*
 * desc_smp_init() - UP 配置下省略描述符亲和性和重定向初始化
 *
 * @desc: 未消费的描述符借用指针。
 * @node: 未消费的 NUMA 节点编号。
 * @affinity: 未消费的可空亲和性掩码借用指针。
 * 返回：无直接返回值、不睡眠、无副作用；UP 上不存在需要初始化的 SMP 字段。
 */
static inline void
desc_smp_init(struct irq_desc *desc, int node, const struct cpumask *affinity) { }
/*
 * free_masks() - UP 配置下的亲和性掩码回收空实现
 *
 * @desc: 未消费的描述符借用指针。
 * 返回：无直接返回值、不睡眠、无副作用；它与成功但未分配资源的 alloc_masks()
 * 配对，使初始化回滚和最终析构无需增加条件编译分支。
 */
static inline void free_masks(struct irq_desc *desc) { }
#endif

/*
 * desc_set_defaults() - 把一个描述符恢复为“已存在但尚无可处理中断”状态。
 *
 * @irq 是 Linux 逻辑中断号；@desc 是调用者独占或已持有 desc->lock 的
 * 输入输出对象；@node/@affinity 供 SMP 初始化使用；@owner 是可空的模块
 * 借用指针，其存活期由分配该 IRQ 的上层协议约束。本函数不分配内存、
 * 不失败、不睡眠。
 *
 * 建立的出口不变量是：irq_data 指回本 desc 的 common 数据，chip 和流控
 * handler 都是安全的占位实现，中断处于 masked+disabled，depth=1 与
 * “一次 enable 才能启用”相匹配，统计和 action 元数据为空，生命周期
 * 引用为 1。动态新建时这是发布前初始化；静态模式 free_desc() 中则是在
 * 锁内把旧 IRQ 退回可再次分配的基线。
 */
static void desc_set_defaults(unsigned int irq, struct irq_desc *desc, int node,
			      const struct cpumask *affinity, struct module *owner)
{
	/* 阶段 1：断开控制器/设备私有载荷，建立 irq_data 的自包含回链。 */
	desc->irq_common_data.handler_data = NULL;
	desc->irq_common_data.msi_desc = NULL;

	desc->irq_data.common = &desc->irq_common_data;
	desc->irq_data.irq = irq;
	desc->irq_data.chip = &no_irq_chip;
	desc->irq_data.chip_data = NULL;
	/*
	 * 阶段 2：清除全部旧设置后只装入通用初值；先标记 disabled/masked，
	 * 再把 handle_irq 设为 handle_bad_irq，确保尚未安装控制器和 action
	 * 的描述符即使被误触发也不会解引用空回调。
	 */
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
	/*
	 * 新对象的树/kobject 所有权引用从 1 开始。静态数组配置下 rcuref 的
	 * put 是空操作；稀疏配置下最后一个 put 会进入 irq_desc_free_rcu()。
	 */
	rcuref_init(&desc->refcnt, 1);
	desc_smp_init(desc, node, affinity);
}

/*
 * total_nr_irqs 是当前允许查找/分配的逻辑 IRQ 号上界（数量而非最后一个
 * 编号），启动时取 NR_IRQS，架构探测或稀疏动态扩展可更新。read-mostly
 * 将它放入偏向读的缓存布局；写入只发生在受控的启动/分配阶段。
 */
unsigned int total_nr_irqs __read_mostly = NR_IRQS;

/**
 * irq_get_nr_irqs() - Number of interrupts supported by the system.
 */
/*
 * 原文意为“返回系统支持的中断数量”。入参：无；可在普通读取上下文调用，
 * 不睡眠、不取得对象引用。返回当前 total_nr_irqs 快照，调用者应把它
 * 当上界而不是已分配 IRQ 数量；稀疏模式下区间内仍可能存在空洞。
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
/*
 * 原文意为“把系统支持的中断数量设置为 @nr，并原样返回 @nr”。
 * @nr 是新的逻辑 IRQ 数量上界，必须由架构启动代码提供合法值；函数带
 * __init，只能在初始化期调用，不处理并发动态分配。副作用还包括重算
 * /proc/interrupts 的 IRQ 编号显示宽度。
 */
unsigned int __init irq_set_nr_irqs(unsigned int nr)
{
	total_nr_irqs = nr;
	irq_proc_calc_prec();
	return nr;
}

/*
 * sparse_irq_lock 串行化 IRQ 号区间的查找+占用、树中删除以及 sysfs
 * 启动补建，避免两个分配者选中同一空洞或释放与展示交错。Maple Tree
 * 声明为外部锁模式，因此树的写操作依赖调用路径已持有此 mutex；读侧
 * 可使用 RCU。
 */
static DEFINE_MUTEX(sparse_irq_lock);
/*
 * sparse_irqs 以 Linux IRQ 号为键、irq_desc 指针为值。ALLOC_RANGE 支持
 * 连续空洞搜索，LOCK_EXTERN 表示锁由 sparse_irq_lock 提供，USE_RCU
 * 允许 irq_to_desc()/迭代读侧与删除并发，但对象释放仍须等待 RCU。
 */
static struct maple_tree sparse_irqs = MTREE_INIT_EXT(sparse_irqs,
					MT_FLAGS_ALLOC_RANGE |
					MT_FLAGS_LOCK_EXTERN |
					MT_FLAGS_USE_RCU,
					sparse_irq_lock);

/*
 * irq_find_free_area() - 在逻辑 IRQ 空间寻找连续 @cnt 个空闲编号。
 *
 * @from 是包含式搜索起点，@cnt 必须非零；调用者持有 sparse_irq_lock，
 * 因而“找到空洞”到“插入描述符”之间不会被其他分配者抢占。成功返回
 * 首号，找不到返回 -ENOSPC；不修改树，也不取得任何对象所有权。
 */
static int irq_find_free_area(unsigned int from, unsigned int cnt)
{
	/* mas 的 index/last 由 mas_empty_area() 改写为找到的连续空洞范围。 */
	MA_STATE(mas, &sparse_irqs, 0, 0);

	if (mas_empty_area(&mas, from, MAX_SPARSE_IRQS, cnt))
		return -ENOSPC;
	return mas.index;
}

/*
 * irq_find_desc_at_or_after() - RCU 下寻找编号不小于 @offset 的首个 desc。
 *
 * @offset 是包含式起点。调用者必须位于 RCU 读侧，返回的是受该读侧窗口
 * 保护的借用指针，不增加 rcuref，不能在解锁后继续使用。无匹配时返回
 * NULL；搜索上界为 total_nr_irqs。
 */
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
	/*
	 * 调用者已持有 sparse_irq_lock，@desc 已完全初始化且由调用者持有
	 * 初始引用。存入树是对无锁/RCU 查找者的发布点；发布后初始化字段不得
	 * 再按“私有对象”无同步修改。这里 WARN 而不返回错误，因为分配路径已
	 * 预留号码，存储失败意味着内部不变量被破坏。
	 */
	MA_STATE(mas, &sparse_irqs, irq, irq);
	WARN_ON(mas_store_gfp(&mas, desc, GFP_KERNEL) != 0);
}

/*
 * delete_irq_desc() - 从全局索引摘除一个逻辑 IRQ。
 *
 * 调用者持有 sparse_irq_lock；@irq 必须对应当前待释放的条目。erase
 * 阻止新的查找者取得 desc，但已在 RCU 临界区或已持 rcuref 的读者仍可
 * 使用对象，因此本函数不释放内存，后续必须走 irq_desc_put_ref()。
 */
static void delete_irq_desc(unsigned int irq)
{
	MA_STATE(mas, &sparse_irqs, irq, irq);
	mas_erase(&mas);
}

#ifdef CONFIG_SPARSE_IRQ
/* kobject 类型在 sysfs 属性定义之前前置声明，最终 release 负责释放 desc。 */
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
/*
 *
 * 调用关系：动态路径由 alloc_desc() 调用，静态数组路径由
 * early_irq_init() 调用。@desc 是已分配但尚未对查找者发布的输入输出
 * 对象；@irq 是逻辑号；@node 是首选 NUMA 节点；@flags 是要写入
 * irq_data 的状态位；@affinity 为可空只读借用掩码；@owner 为可空模块
 * 借用指针。进程/启动上下文，未持 desc->lock，允许 GFP_KERNEL 睡眠。
 *
 * 成功返回 0，desc 的所有动态子对象、锁和默认状态均有效，调用者可将其
 * 插入索引；失败仅返回 -ENOMEM，函数已释放本次成功分配的所有子对象，
 * desc 外壳仍由调用者拥有。发布前不存在并发读者，所以初始化无需加锁。
 */
static int init_desc(struct irq_desc *desc, int irq, int node,
		     unsigned int flags,
		     const struct cpumask *affinity,
		     struct module *owner)
{
	/* 分配 per-cpu 中断统计计数器（irqstat），记录每个 CPU 上该中断的触发次数，
	 * 用于 /proc/interrupts 和 perf 统计。 */
	/*
	 * 阶段 1：取得第一个可失败资源。alloc_percpu() 返回由调用者持有的
	 * per-CPU 区域；零初始化保证 IRQ 尚未发生时所有计数为 0。
	 */
	desc->kstat_irqs = alloc_percpu(struct irqstat);
	if (!desc->kstat_irqs)
		return -ENOMEM;

	/* 分配 SMP 亲和性掩码（affinity、effective_affinity、pending_mask），
	 * 指定该中断可以/实际在哪些 CPU 上处理。UP 系统此函数为空。 */
	/*
	 * 阶段 2：若掩码分配失败，alloc_masks() 已清理自身的部分结果，这里只
	 * 需释放阶段 1 的 per-CPU 区域，保持严格逆序回滚。
	 */
	if (alloc_masks(desc, node)) {
		free_percpu(desc->kstat_irqs);
		return -ENOMEM;
	}

	/* 初始化 irq_desc 的主锁，所有对 desc 的修改操作都需持此锁。
	 * 所有 irq_desc 共用同一个 lockdep 锁类（irq_desc_lock_class），
	 * 使 lockdep 把它们视为同一种锁，避免误报死锁。 */
	/*
	 * 补充说明：共同锁类的主要作用是让 lockdep 检查同类锁的嵌套，而不是
	 * “避免所有误报”；确有硬件层级嵌套时，上层可显式设置不同锁类。
	 */
	raw_spin_lock_init(&desc->lock);
	lockdep_set_class(&desc->lock, &irq_desc_lock_class);

	/* 保护 request_irq/free_irq 不并发执行的互斥锁。 */
	/*
	 * 该 mutex 位于可能睡眠的
	 * 请求/释放外层；短临界区状态仍由 raw desc->lock 保护，二者不可互换。
	 */
	mutex_init(&desc->request_mutex);

	/* 等待中断线程（threaded IRQ）退出的等待队列，
	 * free_irq() 需要等待所有 IRQ 线程完成后才能释放 desc。 */
	/*
	 * 等待者以
	 * threads_active 等状态为条件，队列本身只负责睡眠/唤醒衔接。
	 */
	init_waitqueue_head(&desc->wait_for_threads);

	/* 设置默认处理函数（handle_bad_irq）、irq_data（硬件 irq 号、domain 等）
	 * 和 CPU 亲和性掩码（使用传入的 affinity 或 irq_default_affinity）。 */
	/*
	 * 阶段 3：完成后对象处于安全但禁用的基线状态，后续才叠加调用者传入
	 * 的策略 flags。
	 */
	desc_set_defaults(irq, desc, node, affinity, owner);

	/* 将初始标志位写入 irq_data.state_use_accessors，
	 * 例如 IRQ_NOAUTOEN（注册后不自动使能）等标志。 */
	/*
	 * 动态 managed IRQ
	 * 也在此携带“亲和性由框架管理、初始关闭”等策略。
	 */
	irqd_set(&desc->irq_data, flags);

	/* 初始化中断重发（resend）机制：某些中断控制器在边沿触发模式下，
	 * 若中断到来时被屏蔽，需要软件模拟重发以防止中断丢失。 */
	/*
	 * 此时仍未发布，
	 * 因而可安全建立 resend_node/状态而无需与重发队列竞争。
	 */
	irq_resend_init(desc);

#ifdef CONFIG_SPARSE_IRQ
	/* 初始化 sysfs kobject（对应 /sys/kernel/irq/N/ 目录）和 RCU head，
	 * RCU head 用于延迟释放 desc（free_irq 后通过 RCU 安全释放）。 */
	/*
	 * kobject_init() 建立独立 kobject 引用；最终 irq_kobj_release() 才释放
	 * desc，因此 RCU 回调只需 kobject_put()，不直接 kfree。
	 */
	kobject_init(&desc->kobj, &irq_kobj_type);
	init_rcu_head(&desc->rcu);
#endif

	return 0;
}

#ifdef CONFIG_SPARSE_IRQ

/* irq_kobj_type.release 的前置声明；所有动态 desc 内存统一由它最终释放。 */
static void irq_kobj_release(struct kobject *kobj);

#ifdef CONFIG_SYSFS
/*
 * /sys/kernel/irq 的父 kobject。postcore_initcall 建立前为 NULL，因此更早
 * 分配的 desc 只初始化自身 kobject，待 irq_sysfs_init() 批量补挂目录。
 */
static struct kobject *irq_kobj_base;

/*
 * 每个 IRQ 属性均为只读：show 回调通过内嵌 kobject 还原 irq_desc，
 * 输出到 sysfs 的一页缓冲区。宏只生成标准 kobj_attribute 声明，实际
 * 生命周期由 irq_kobj_type.default_groups 管理。
 */
#define IRQ_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

/*
 * per_cpu_count_show() - 输出该 IRQ 在每个 possible CPU 上的累计次数。
 *
 * @kobj 借用并保证 desc 在 show 期间存活；@attr 仅满足统一接口；@buf
 * 是 sysfs 提供的 PAGE_SIZE 输出缓冲区。函数不持 desc->lock：per-CPU
 * 计数可并发增长，输出是诊断快照而非事务一致视图。成功返回写入字节数，
 * 格式为逗号分隔的一行；无独立失败路径。
 */
static ssize_t per_cpu_count_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	/*
	 * 变量地图：desc 是由 kobject 生命周期保护的借用指针；ret 同时是已写
	 * 字节数和下一次偏移；p 在首项为空、后续为逗号；cpu 遍历 possible
	 * CPU，因而离线 CPU 的历史计数也会显示。
	 */
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	ssize_t ret = 0;
	char *p = "";
	int cpu;

	for_each_possible_cpu(cpu) {
		unsigned int c = irq_desc_kstat_cpu(desc, cpu);

		/* sysfs_emit_at() 负责边界安全；并发中断可使各列来自略不同时间点。 */
		ret += sysfs_emit_at(buf, ret, "%s%u", p, c);
		p = ",";
	}

	ret += sysfs_emit_at(buf, ret, "\n");
	return ret;
}
IRQ_ATTR_RO(per_cpu_count);

/*
 * chip_name_show() - 读取当前 irq_chip 的名称。
 *
 * kobject/@attr/@buf 契约同上。自动 raw_spinlock_irq guard 在作用域退出
 * 时恢复本地中断并解锁，防止控制器安装/替换与读取竞态；持锁期间不睡眠。
 * 有名称时返回包含换行的长度，否则返回 0（空属性）。
 */
static ssize_t chip_name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	if (desc->irq_data.chip && desc->irq_data.chip->name)
		return sysfs_emit(buf, "%s\n", desc->irq_data.chip->name);
	return 0;
}
IRQ_ATTR_RO(chip_name);

/*
 * hwirq_show() - 输出 irq_domain 中的硬件中断号。
 *
 * 只有存在 domain 时 hwirq 才有可解释的映射语义；desc->lock 将 domain
 * 与 hwirq 作为一组稳定读取。返回输出长度，未映射时返回 0。
 */
static ssize_t hwirq_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	if (desc->irq_data.domain)
		return sysfs_emit(buf, "%lu\n", desc->irq_data.hwirq);
	return 0;
}
IRQ_ATTR_RO(hwirq);

/*
 * type_show() - 输出当前流控触发类型为 level 或 edge。
 *
 * 在 desc->lock 下读取 irq_data 状态，避免与 irq_set_irq_type() 更新交错。
 * 返回一行文本长度；该二分展示面向诊断，不细分高/低电平或上/下降沿。
 */
static ssize_t type_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	return sysfs_emit(buf, "%s\n", irqd_is_level_type(&desc->irq_data) ? "level" : "edge");

}
IRQ_ATTR_RO(type);

/*
 * wakeup_show() - 输出该 IRQ 是否配置为系统唤醒源。
 *
 * wake 状态与并发 irq_set_irq_wake() 受 desc->lock 协调。返回
 * "enabled" 或 "disabled" 及换行，不改变 wake_depth 或硬件状态。
 */
static ssize_t wakeup_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	return sysfs_emit(buf, "%s\n", str_enabled_disabled(irqd_is_wakeup_set(&desc->irq_data)));
}
IRQ_ATTR_RO(wakeup);

/*
 * name_show() - 输出流控 handler 的可选描述名称。
 *
 * desc->name 是借用字符串，读取必须与设置者通过 desc->lock 串行；存在时
 * 返回一行文本长度，不存在时返回空属性。它不同于每个 irqaction 的设备名。
 */
static ssize_t name_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	guard(raw_spinlock_irq)(&desc->lock);
	if (desc->name)
		return sysfs_emit(buf, "%s\n", desc->name);
	return 0;
}
IRQ_ATTR_RO(name);

/*
 * actions_show() - 输出挂在该描述符上的全部 irqaction 名称。
 *
 * action 链可由 request_irq/free_irq 修改，故在 desc->lock 下完整遍历；
 * scoped_guard 只覆盖遍历，换行写入在出锁后完成，以缩短硬中断关闭时间。
 * 返回逗号分隔文本的字节数；无 action 时返回 0。
 */
static ssize_t actions_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	/*
	 * action 是锁内借用的当前链节点，不能逃逸临界区；ret/p 与
	 * per_cpu_count_show() 相同，负责无前导逗号的增量输出。
	 */
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

/*
 * irq_attrs 是每个动态 IRQ kobject 默认公开的只读属性表。数组元素借用上述
 * 静态 attribute 对象，以 NULL 结尾；kobject 核心只消费表结构，不取得由
 * 本文件单独释放的所有权。各 show 回调的锁决定字段一致性，属性表本身只读。
 */
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
/* 由 irq_attrs 生成 irq_groups，供 irq_kobj_type.default_groups 在 add 时批量建文件。 */
ATTRIBUTE_GROUPS(irq);

/*
 * irq_kobj_type 把属性组和最终析构绑定到每个动态 desc。release 是 kobject
 * 的所有引用归零后唯一允许释放包含对象的位置，避免 sysfs 文件操作仍持有
 * kobject 时提前 kfree。
 */
static const struct kobj_type irq_kobj_type = {
	.release	= irq_kobj_release,
	.sysfs_ops	= &kobj_sysfs_ops,
	.default_groups = irq_groups,
};

/*
 * irq_sysfs_add() - 尝试把一个已初始化 desc 发布为 /sys/kernel/irq/@irq。
 *
 * @irq 是目录名；@desc 由树/分配路径持有。调用者在动态分配时持
 * sparse_irq_lock，启动补建时同样持锁，故与删除串行。返回：无；sysfs
 * 尚未初始化时静默延后，kobject_add() 失败只告警，不回滚 IRQ 分配。
 * 成功以 IRQS_SYSFS 记录“确实 add 过”，供删除路径精确配对。
 */
static void irq_sysfs_add(int irq, struct irq_desc *desc)
{
	if (irq_kobj_base) {
		/*
		 * Continue even in case of failure as this is nothing
		 * crucial and failures in the late irq_sysfs_init()
		 * cannot be rolled back.
		 */
		/*
		 * 即使失败也继续，因为 sysfs 不是 IRQ 正确工作的必要
		 * 条件，而且晚期 irq_sysfs_init() 中已创建的目录无法整体回滚。
		 * 因此这里的事务边界只覆盖单个 kobject，不影响描述符发布。
		 */
		if (kobject_add(&desc->kobj, irq_kobj_base, "%d", irq))
			pr_warn("Failed to add kobject for irq %d\n", irq);
		else
			desc->istate |= IRQS_SYSFS;
	}
}

/*
 * irq_sysfs_del() - 若 desc 曾发布到 sysfs，则将其目录从命名空间摘除。
 *
 * @desc 是待释放的输入对象，调用者持 sparse_irq_lock。无返回值；
 * kobject_del() 只取消可见性，不丢初始引用，真正释放仍由后续
 * irq_desc_put_ref() -> RCU -> kobject_put() 驱动。
 */
static void irq_sysfs_del(struct irq_desc *desc)
{
	/*
	 * Only invoke kobject_del() when kobject_add() was successfully
	 * invoked for the descriptor. This covers both early boot, where
	 * sysfs is not initialized yet, and the case of a failed
	 * kobject_add() invocation.
	 */
	/*
	 * 仅在该描述符的 kobject_add() 成功执行过时才调用
	 * kobject_del()；这样同时覆盖 sysfs 尚未初始化的早期启动，以及
	 * kobject_add() 自身失败两种情况，避免删除一个从未加入的对象。
	 */
	if (desc->istate & IRQS_SYSFS)
		kobject_del(&desc->kobj);
}

/*
 * irq_sysfs_init() - 建立 /sys/kernel/irq 并补挂启动早期已有的 desc。
 *
 * 入参：无；postcore_initcall 的可睡眠启动上下文。持 sparse_irq_lock
 * 使父目录建立和全树遍历成为一个序列化阶段，动态分配/释放不能看到半完成
 * 的 irq_kobj_base。成功返回 0；父 kobject 分配失败返回 -ENOMEM，已有
 * desc 和 IRQ 功能保持不变。
 */
static int __init irq_sysfs_init(void)
{
	struct irq_desc *desc;
	int irq;

	/* Prevent concurrent irq alloc/free */
	/*
	 * 原文意为“阻止并发 IRQ 分配/释放”。mutex guard 离开函数时自动解锁；
	 * 这里允许 kobject 操作睡眠，不能使用 desc 的 raw spinlock 代替。
	 */
	guard(mutex)(&sparse_irq_lock);
	irq_kobj_base = kobject_create_and_add("irq", kernel_kobj);
	if (!irq_kobj_base)
		return -ENOMEM;

	/* Add the already allocated interrupts */
	/*
	 * 原文意为“加入已经分配的中断”。遍历时树写者被 mutex 排除，每个失败
	 * 仅产生告警，循环继续为其余 IRQ 建立诊断目录。
	 */
	for_each_irq_desc(irq, desc)
		irq_sysfs_add(irq, desc);
	return 0;
}
/* postcore 阶段在普通 IRQ 动态分配开始后补建 sysfs，因此函数会遍历早期条目。 */
postcore_initcall(irq_sysfs_init);

#else /* !CONFIG_SYSFS */

/*
 * 无 SYSFS 时仍需 kobj_type.release：动态 desc 的 kobject 被用作最终
 * 生命周期容器，即使没有目录和属性。add/del stub 不消费参数且无副作用。
 */
static const struct kobj_type irq_kobj_type = {
	.release	= irq_kobj_release,
};

/*
 * irq_sysfs_add() - SYSFS 关闭时保留动态描述符发布调用点
 *
 * @irq: 未消费的逻辑 IRQ 号。
 * @desc: 未消费的描述符借用指针。
 * 返回：无直接返回值、不睡眠、无副作用；kobject 仍承担最终内存析构职责。
 */
static void irq_sysfs_add(int irq, struct irq_desc *desc) {}
/*
 * irq_sysfs_del() - SYSFS 关闭时保留动态描述符撤销调用点
 *
 * @desc: 未消费的描述符借用指针。
 * 返回：无直接返回值、不睡眠、无副作用；随后 rcuref/RCU/kobject_put() 仍须配对。
 */
static void irq_sysfs_del(struct irq_desc *desc) {}

#endif /* CONFIG_SYSFS */

/*
 * irq_to_desc() - 由 Linux 逻辑 IRQ 号查找动态描述符。
 *
 * @irq 是查找键。返回 Maple Tree 中的借用指针或 NULL，不增加 rcuref、
 * 不锁定字段，也不保证离开调用者已有的锁/RCU 窗口后仍存活。短暂的硬
 * 中断分派依靠 IRQ 映射/注册生命周期；可能与动态删除并发的遍历者必须
 * 另用 sparse_irq_lock、RCU 或 irq_desc_get_ref()。
 */
struct irq_desc *irq_to_desc(unsigned int irq)
{
	return mtree_load(&sparse_irqs, irq);
}
#ifdef CONFIG_KVM_BOOK3S_64_HV_MODULE
EXPORT_SYMBOL_GPL(irq_to_desc);
#endif

/*
 * irq_lock_sparse()/irq_unlock_sparse() - 暴露描述符全局拓扑锁的配对接口。
 *
 * 入参、返回值：无。只能在可睡眠上下文获取，调用者必须严格配对，持锁
 * 期间可稳定遍历/修改 sparse_irqs 及相关 proc/sysfs 拓扑。该锁不保护
 * 单个 desc 的运行字段，后者仍需 desc->lock。
 */
void irq_lock_sparse(void)
{
	mutex_lock(&sparse_irq_lock);
}

/*
 * irq_unlock_sparse() - 释放当前任务持有的描述符全局拓扑锁
 *
 * 入参、返回值：无。必须与同一任务先前成功的 irq_lock_sparse() 配对；释放后
 * Maple Tree、proc/sysfs 拓扑可立即被其他任务修改，旧借用指针不能仅凭此锁使用。
 */
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
/*
 *
 * 调用者处于可睡眠上下文并持 sparse_irq_lock；@affinity/@owner 均为
 * 借用输入。成功返回尚未发布、由调用者持有初始 rcuref 的 desc；失败
 * 返回 NULL 且无资源残留。真正的发布由随后的 irq_insert_desc() 完成。
 */
static struct irq_desc *alloc_desc(int irq, int node, unsigned int flags,
				   const struct cpumask *affinity,
				   struct module *owner)
{
	/* desc 是外壳所有权，ret 只在初始化阶段传递 0/-ENOMEM。 */
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

/*
 * irq_kobj_release() - 动态 irq_desc 的最终析构函数。
 *
 * @kobj 是最后一个 kobject 引用刚归零的内嵌对象；container_of() 取回
 * 唯一包含 desc。调用前该 desc 已从树/sysfs 摘除、rcuref 已耗尽且经过
 * RCU 宽限期，因此不再有合法读者。无返回值；按子对象到外壳的顺序释放
 * SMP 掩码、per-CPU 统计和 desc 本身。
 *
 * 此处只回收 init_desc() 建立的通用子资源；irq_set_percpu_devid() 后另行
 * 建立的 percpu_enabled 不在本析构函数的释放清单内，因此该专用模式依赖
 * “描述符长期存在、只设置一次”的调用协议，不能把本路径理解为回收它。
 */
static void irq_kobj_release(struct kobject *kobj)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	free_masks(desc);
	free_percpu(desc->kstat_irqs);
	kfree(desc);
}

/*
 * delayed_free_desc() - RCU 宽限期结束后交还 desc 的 kobject 初始引用。
 *
 * @rhp 嵌在待回收 desc 中。此回调不能睡眠；kobject_put() 若成为最后引用
 * 会同步调用 irq_kobj_release()。若 sysfs 文件仍持引用，实际析构继续
 * 延后，但已不会有 Maple Tree RCU 读者访问该对象。
 */
static void delayed_free_desc(struct rcu_head *rhp)
{
	struct irq_desc *desc = container_of(rhp, struct irq_desc, rcu);

	kobject_put(&desc->kobj);
}

/*
 * irq_desc_free_rcu() - 安排动态描述符在 RCU 宽限期后进入最终释放。
 *
 * @desc 的最后一个 rcuref 已由 irq_desc_put_ref() 释放，调用者不再拥有
 * 对象；函数无直接返回值且不等待宽限期。call_rcu() 之后，旧 RCU 读者
 * 可完成，最终由 delayed_free_desc() 丢弃 kobject 引用。
 */
void irq_desc_free_rcu(struct irq_desc *desc)
{
	/*
	 * We free the descriptor, masks and stat fields via RCU. That
	 * allows demultiplex interrupts to do rcu based management of
	 * the child interrupts.
	 * This also allows us to use rcu in kstat_irqs_usr().
	 */
	/*
	 * 描述符、亲和性掩码和统计字段统一经 RCU 释放，使级联/
	 * 解复用中断可以用 RCU 管理子中断，也让 kstat_irqs_usr() 能在读侧
	 * 临界区无锁统计。RCU 只延长存储期，不冻结 desc 内字段。
	 */
	call_rcu(&desc->rcu, delayed_free_desc);
}

/*
 * free_desc() - 摘除一个稀疏 IRQ 描述符并丢弃索引所有权引用。
 *
 * @irq 必须是已分配编号；调用者持 sparse_irq_lock，且上层已先释放
 * irqaction/硬件资源。函数依次移除 debugfs、procfs、sysfs 和 Maple Tree
 * 可见性，最后 put 初始 rcuref；无直接返回值。对象可能因 procfs 引用或
 * RCU 读者继续存活，但新查找已不可获得它。
 */
static void free_desc(unsigned int irq)
{
	/* desc 在全局锁保护下由树查得，是直到最后 put 前有效的借用指针。 */
	struct irq_desc *desc = irq_to_desc(irq);

	/* 阶段 1：先撤销用户可见的辅助入口，阻止新的 debug/proc 操作。 */
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
	/*
	 * sparse_irq_lock 同时保护 show_interrupts() 和
	 * kstat_irq_usr()；从稀疏树删除后，后续 proc 查找会失败。sysfs 删除
	 * 也必须与并发 irq_sysfs_init() 串行。补充：已开始的引用型读者仍由
	 * rcuref/RCU 保护，故“可以释放”表示可以启动延迟释放而非立即 kfree。
	 */
	irq_sysfs_del(desc);
	delete_irq_desc(irq);
	irq_desc_put_ref(desc);
}

/*
 * alloc_descs() - 构造并发布一段连续的动态 irq_desc。
 *
 * @start/@cnt 描述逻辑号区间；@node 是默认 NUMA 节点；@affinity 是可空、
 * 长度至少为 @cnt 的逐 IRQ 策略数组；@owner 为可空模块借用指针。调用者
 * 持 sparse_irq_lock，允许睡眠。
 *
 * 成功返回 @start，区间内每个 desc 已进入 Maple Tree、sysfs/debugfs；
 * affinity 元素为空返回 -EINVAL 且不分配；任一内存分配失败返回 -ENOMEM，
 * 并逆序 free_desc() 已发布前缀，使区间恢复为空。发布后所有权由树中的
 * 初始 rcuref 承担。
 *
 * 精确地说，成功只保证每项已进入主 Maple Tree；sysfs 尚未初始化或辅助入口
 * 创建失败时，IRQ 分配仍成功，debugfs/sysfs 仅是已经尝试建立的诊断界面。
 */
static int alloc_descs(unsigned int start, unsigned int cnt, int node,
		       const struct irq_affinity_desc *affinity,
		       struct module *owner)
{
	/* desc 是当前新对象；i 是已成功发布的前缀长度/当前偏移。 */
	struct irq_desc *desc;
	int i;

	/* Validate affinity mask(s) */
	/*
	 * 原文意为“校验亲和性掩码”。必须在首次分配前完整预检，避免中途才因
	 * 空掩码失败而需要混合 -EINVAL 与资源回滚。
	 */
	if (affinity) {
		for (i = 0; i < cnt; i++) {
			if (cpumask_empty(&affinity[i].mask))
				return -EINVAL;
		}
	}

	for (i = 0; i < cnt; i++) {
		/*
		 * 每轮的 mask 是传给 desc_smp_init() 的只读借用；flags 从当前
		 * affinity 描述生成，只对这个 IRQ 有效。
		 */
		const struct cpumask *mask = NULL;
		unsigned int flags = 0;

		if (affinity) {
			/*
			 * managed IRQ 的目标由内核管理，初始保持 shutdown；无论是否
			 * managed，提供了 mask 就设置 AFFINITY_SET，并按首个目标 CPU
			 * 选择本地 NUMA 节点。affinity++ 使下一轮消费下一项。
			 */
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
		/*
		 * 发布次序：先入主索引，再建立可选诊断入口。sysfs/debugfs 失败
		 * 不撤销主功能；从插入起 RCU 查找者即可看到完整初始化的 desc。
		 */
		irq_insert_desc(start + i, desc);
		irq_sysfs_add(start + i, desc);
		irq_add_debugfs_entry(start + i, desc);
	}
	return start;

err:
	/*
	 * 到达时 [start, start+i) 已发布，当前 i 尚未分配成功。逆序摘除不是
	 * 因资源间有父子依赖，而是保持标准回滚栈并让最后发布者先消失。
	 */
	for (i--; i >= 0; i--)
		free_desc(start + i);
	return -ENOMEM;
}

/*
 * irq_expand_nr_irqs() - 扩大稀疏逻辑 IRQ 的运行时上界。
 *
 * @nr 是所需数量（开区间上界），调用者持 sparse_irq_lock。超过
 * MAX_SPARSE_IRQS 返回 false 且不改变状态；否则写 total_nr_irqs、更新
 * proc 显示宽度并返回 true。只允许扩大方向的调用，函数自身不分配 desc。
 */
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
/*
 *
 * 入参：无；启动期、单线程、允许内存分配。返回 arch_early_irq_init()
 * 的结果。成功后预分配 desc 已进入 Maple Tree，但硬件 domain/action
 * 尚待后续 irqchip 与驱动初始化。架构返回的预分配数量必须与启动期可用
 * 内存相匹配，本函数的预分配循环按内核启动不可恢复约束处理分配结果。
 */
int __init early_irq_init(void)
{
	/*
	 * 变量地图：initcnt 是架构要求立即创建的低号 IRQ 数；i 为逻辑号；
	 * node 是预分配内存的首选在线 NUMA 节点；desc 是当前尚待发布的新对象。
	 */
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
	/*
	 * 原文第一行意为“允许架构更新 nr_irqs，并返回要预分配的 IRQ 数量”。
	 * 架构钩子既可调整全局容量，又决定必须在控制器探测前存在的低号区间。
	 */
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
		/* 完整初始化先于树发布，RCU 查找者不会观察到半初始化对象。 */
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
/*
 *
 * 入参：无；启动期单线程并允许分配。成功返回架构钩子的结果，全部
 * NR_IRQS 个静态对象已初始化；失败返回 init_desc() 的 -ENOMEM，并释放
 * 已初始化前缀的动态子资源。数组外壳具有静态存储期，永不释放。
 */
int __init early_irq_init(void)
{
	/* count 是数组固定容量；i 是当前/已初始化前缀长度；ret 传递失败码。 */
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

/*
 * irq_to_desc() - 在静态数组配置下把逻辑号转换为描述符地址。
 *
 * @irq 必须小于 NR_IRQS 才有效。成功返回具有永久存储期的借用指针，越界
 * 返回 NULL；无需 RCU/引用保护对象寿命，但字段并发访问仍须遵循 desc 锁。
 */
struct irq_desc *irq_to_desc(unsigned int irq)
{
	return (irq < NR_IRQS) ? irq_desc + irq : NULL;
}
EXPORT_SYMBOL(irq_to_desc);

/*
 * free_desc() - 将静态描述符重置为可再次分配的空闲基线。
 *
 * @irq 必须有效；调用者持 sparse_irq_lock。函数在 desc->lock 且本地
 * 中断关闭时清除旧配置、恢复 disabled/masked 默认值，再清零所有 possible
 * CPU 统计，最后更新空闲索引。无内存释放、无返回值，对象地址始终有效。
 */
static void free_desc(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	int cpu;

	/* 阶段 1：与中断处理/配置串行，原子地撤销旧 desc 运行状态。 */
	scoped_guard(raw_spinlock_irqsave, &desc->lock)
		desc_set_defaults(irq, desc, irq_desc_get_node(desc), NULL, NULL);

	for_each_possible_cpu(cpu)
		*per_cpu_ptr(desc->kstat_irqs, cpu) = (struct irqstat) { };

	/* 阶段 3：把号码从“已分配集合”摘除，数组存储本身仍保留。 */
	delete_irq_desc(irq);
}

/*
 * alloc_descs() - 在静态数组中标记连续 IRQ 区间为已分配。
 *
 * @start/@cnt 必须落在 NR_IRQS 内；@node/@affinity 在此配置中无效；
 * @owner 记录到每个永久 desc。调用者持 sparse_irq_lock。成功总是返回
 * @start，无内存分配和失败路径；发布点是 irq_insert_desc()。
 */
static inline int alloc_descs(unsigned int start, unsigned int cnt, int node,
			      const struct irq_affinity_desc *affinity,
			      struct module *owner)
{
	u32 i;

	for (i = 0; i < cnt; i++) {
		struct irq_desc *desc = irq_to_desc(start + i);

		/* owner 是上层保证存活的借用模块指针，随后号码进入已分配索引。 */
		desc->owner = owner;
		irq_insert_desc(start + i, desc);
	}
	return start;
}

/*
 * 静态数组容量不可在运行时扩大；@nr 不被消费，始终返回 false，促使
 * __irq_alloc_descs() 把超出 total_nr_irqs 的请求报告为 -ENOMEM。
 */
static inline bool irq_expand_nr_irqs(unsigned int nr)
{
	return false;
}

/*
 * irq_mark_irq() - 将一个既有静态数组槽标为已分配。
 *
 * @irq 必须有效。函数可睡眠获取全局 mutex，将永久 desc 发布到已分配
 * 索引；无返回值、不改变 desc 内容，通常供静态架构路径登记保留 IRQ。
 */
void irq_mark_irq(unsigned int irq)
{
	guard(mutex)(&sparse_irq_lock);
	irq_insert_desc(irq, irq_desc + irq);
}

#endif /* !CONFIG_SPARSE_IRQ */

/*
 * handle_irq_desc() - 校验上下文后调用一个描述符的高层流控 handler。
 *
 * @desc 是由逻辑号或 domain 映射得到的借用指针；调用者必须保证其在调用
 * 期间存活。函数本身不取 desc->lock，因为 generic_handle_irq_desc()
 * 进入的 edge/level/fasteoi handler 按各自协议管理锁、ack/mask/eoi。
 *
 * NULL 返回 -EINVAL；若该 IRQ 标记为强制硬中断上下文而当前不在 hardirq，
 * WARN 一次并返回 -EPERM；否则调用 desc->handle_irq(desc) 并返回 0。
 * 0 表示已完成分派调用，不等价于设备 action 必然声称处理了中断。函数
 * 通常运行于 hardirq、不可睡眠。
 */
int handle_irq_desc(struct irq_desc *desc)
{
	/* data 是 desc 内嵌 irq_data 的借用指针，与 desc 具有相同有效期。 */
	struct irq_data *data;

	if (!desc)
		return -EINVAL;

	data = irq_desc_get_irq_data(desc);
	if (WARN_ON_ONCE(!in_hardirq() && irqd_is_handle_enforce_irqctx(data)))
		return -EPERM;

	/* 真正状态转换点是流控函数指针，它会按触发类型驱动 chip 和 action 链。 */
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
/*
 * 为指定 Linux IRQ 调用处理函数；@irq 是逻辑中断号。成功返回
 * 0，号码无法转换为 desc 时返回 -EINVAL。调用者必须已在 IRQ 上下文并
 * 初始化 irq regs；函数只做 irq_to_desc() 转换并把契约交给
 * handle_irq_desc()，不取得长期引用。
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
/*
 * 从任意上下文为指定逻辑 IRQ 调用处理函数。@irq 是逻辑号；
 * 返回 0 或 handle_irq_desc() 的负错误。函数保存并关闭本地硬中断，使
 * 流控路径不会在当前 CPU 被普通硬中断嵌套；退出时精确恢复原状态。
 * 若 IRQ 带“必须真正来自 hardirq”标记，仅 local_irq_disable() 并不会
 * 伪造 hardirq 上下文，仍返回 -EPERM。
 */
int generic_handle_irq_safe(unsigned int irq)
{
	/* flags 保存调用前本地 IRQ 状态；ret 在恢复状态后原样返回。 */
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
/*
 * 在 @domain 中把硬件号 @hwirq 映射为 Linux IRQ 并调用其
 * handler；成功返回 0，映射不存在时经 NULL desc 返回 -EINVAL。domain
 * 和映射是调用期间的借用对象，调用者必须处于已初始化 irq regs 的 IRQ
 * 上下文。irq_resolve_mapping() 只完成映射查找，流控仍由 desc 决定。
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
/*
 * 原文标题把此函数误写为 generic_handle_irq_safe；当前实现实际是
 * generic_handle_domain_irq_safe()。其语义是在任意上下文解析
 * @domain/@hwirq，保存并关闭本地中断后分派，再恢复原状态；返回 0 或
 * 负错误。若映射 IRQ 强制 hardirq 上下文，调用者仍必须本来就在 hardirq。
 */
int generic_handle_domain_irq_safe(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	/* flags/ret 分别承载调用前本地 IRQ 状态和完整分派结果。 */
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
/*
 * 解析 @domain 中的 NMI 硬件号 @hwirq 并调用对应 handler；
 * 成功返回 0，映射失败返回 -EINVAL。调用者必须处于 NMI 上下文且 irq
 * regs 已初始化。WARN 只诊断契约违例，不阻止继续分派；NMI 路径必须使用
 * 已配置为 NMI 安全的 chip/handler，函数不会替调用者建立 NMI 语义。
 */
int generic_handle_domain_nmi(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	WARN_ON_ONCE(!in_nmi());
	return handle_irq_desc(irq_resolve_mapping(domain, hwirq));
}

#ifdef CONFIG_SMP
/*
 * demux_redirect_remote() - 必要时把解复用出的子 IRQ 转投亲和 CPU。
 *
 * @desc 是已解析且由调用者生命周期保护的借用对象；当前位于硬中断上下文。
 * 函数在 desc->lock 下读取 action、有效亲和性和 redirect.target_cpu，
 * 必要时调用 chip 的预重定向钩子并排 irq_work。当前 CPU 已在有效掩码中
 * 返回 false，表示调用者应本地处理；否则返回 true，表示本次本地入口已
 * 被重定向（即使 action 已在并发释放中而没有实际排 work，也不可本地调用）。
 * 不睡眠，锁由 guard 自动释放。
 */
static bool demux_redirect_remote(struct irq_desc *desc)
{
	/*
	 * m 在 desc->lock 范围内借用 chip 维护的有效掩码；target_cpu 用
	 * READ_ONCE() 与 CPU 热插拔迁移写者形成单次一致读取，防止编译器重复
	 * 取值。锁主要保护 desc 状态，READ_ONCE 明确该字段的并发访问属性。
	 */
	guard(raw_spinlock)(&desc->lock);
	const struct cpumask *m = irq_data_get_effective_affinity_mask(&desc->irq_data);
	unsigned int target_cpu = READ_ONCE(desc->redirect.target_cpu);

	if (desc->irq_data.chip->irq_pre_redirect)
		/*
		 * 某些级联控制器需要在离开当前 CPU 前先 ack/清底层状态；回调在
		 * desc 锁和 hardirq 上下文执行，必须不可睡眠。
		 */
		desc->irq_data.chip->irq_pre_redirect(&desc->irq_data);

	/*
	 * If the interrupt handler is already running on a CPU that's included
	 * in the interrupt's affinity mask, redirection is not necessary.
	 */
	/*
	 * 若 handler 已运行在有效亲和掩码包含的 CPU 上，就无需
	 * 重定向。本地直接处理可避免额外 IPI/irq_work 延迟。
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
	/*
	 * 原文先说明 action 检查与 __free_irq() 在同一 desc->lock 下配对，
	 * 因而不会把 work 排给已关闭的 IRQ。随后证明与 CPU 下线安全：
	 * multi_cpu_stop() 先让所有活动 CPU 关闭硬中断，再由 dying CPU 执行
	 * take_cpu_down()；本函数身处 hardirq，所以只会完整发生在下线动作
	 * 之前或之后。之前排入 dying CPU 的 work 会在下线回调链中 flush；
	 * 之后 IRQ 迁移已更新 target_cpu，不会再指向 dying CPU。由此
	 * irq_work_queue_on() 的在线 CPU 前置条件始终成立。
	 */
	if (desc->action)
		irq_work_queue_on(&desc->redirect.work, target_cpu);

	return true;
}
#else /* CONFIG_SMP */
/*
 * UP 没有远端 CPU；@desc 不被使用，始终返回 false，让调用者在本 CPU
 * 继续处理，且无锁、无排队副作用。
 */
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
/*
 * 处理解复用 domain 中的硬件中断 @hwirq。@domain 为借用映射
 * 域；查找失败返回 false。成功找到 desc 后，若当前 CPU 不符合有效亲和性
 * 则排到目标 CPU 并返回 true；否则本地调用 handle_irq_desc()，其 0
 * 转换为 true、负错误转换为 false。这里的 true 表示“已接受本次分派”，
 * 不表示具体设备 action 的 IRQ_HANDLED 结果。
 *
 * 调用者通常是级联控制器的 hardirq handler。desc 借用期由 domain/IRQ
 * 注册协议及动态 desc 的 RCU 回收保证；函数本身不睡眠、不持长期引用。
 */
bool generic_handle_demux_domain_irq(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	/* desc 是映射返回的借用对象，NULL 是唯一查找失败类别。 */
	struct irq_desc *desc = irq_resolve_mapping(domain, hwirq);

	if (unlikely(!desc))
		return false;

	if (demux_redirect_remote(desc))
		return true;

	/* handle_irq_desc() 以 0 表示成功，逻辑取反适配此接口的 bool 契约。 */
	return !handle_irq_desc(desc);
}
EXPORT_SYMBOL_GPL(generic_handle_demux_domain_irq);

#endif

/* Dynamic interrupt handling */
/*
 * 原文意为“动态中断处理”。以下接口管理的是 Linux 逻辑 IRQ 描述符号码
 * 空间，不直接申请设备中断 action，也不配置 irq_chip 硬件。
 */

/**
 * irq_free_descs - free irq descriptors
 * @from:	Start of descriptor range
 * @cnt:	Number of consecutive irqs to free
 */
/*
 * 释放从 @from 开始的 @cnt 个连续 IRQ 描述符。参数单位均为
 * 逻辑 IRQ 个数；越界请求静默返回，不释放部分区间。函数在可睡眠上下文
 * 获取 sparse_irq_lock，逐个调用配置相关的 free_desc()；无返回值。
 *
 * 调用者必须已撤销该范围的 domain 映射、action 和硬件使用，且区间内每项
 * 确实已分配。本函数负责描述符层的摘除/重置；稀疏配置中的物理释放会因
 * rcuref、RCU 或 kobject 引用延迟。
 */
void irq_free_descs(unsigned int from, unsigned int cnt)
{
	/* i 是区间内偏移；全局 mutex 使整个范围对其他分配/释放者原子占用。 */
	int i;

	/*
	 * 先做整体边界检查，避免只释放合法前缀。无符号加法理论上可能溢出，
	 * 调用协议要求 cnt 是实际连续申请的合理长度。
	 */
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
/*
 * 分配并初始化一段 IRQ 描述符。
 *
 * @irq >= 0 时要求精确分配该逻辑号，负值表示由框架选择；@from 是包含式
 * 搜索下界；@cnt 是非零连续数量；@node 是默认 NUMA 节点；@owner 是
 * 可空模块借用指针；@affinity 是可空、至少 @cnt 项的亲和策略数组，既
 * 指示本地内存节点又给出默认 affinity。函数可睡眠，内部获取全局 mutex。
 *
 * 成功返回首个逻辑 IRQ 号，所有 desc 已初始化并发布。失败可返回
 * -EINVAL（数量/精确号约束或空 affinity）、-EEXIST（指定号不可完整获得）、
 * -ENOSPC（无连续空洞）或 -ENOMEM（描述符/掩码分配或容量扩展失败）；
 * alloc_descs() 保证部分构造被回滚。__ref 允许启动期代码与常驻调用者
 * 共享此入口，而不把函数本身永久限制在 __init section。
 *
 * 实现细节：irq_find_free_area() 内部虽用 -ENOSPC 表示搜索失败，但自由分配的
 * 正常调用会在随后的容量检查中把无法扩展统一报告为 -ENOMEM；精确分配若未找到
 * 指定首号则报告 -EEXIST。调用者不应依赖从本入口直接观察内部 -ENOSPC。
 */
int __ref __irq_alloc_descs(int irq, unsigned int from, unsigned int cnt, int node,
			    struct module *owner, const struct irq_affinity_desc *affinity)
{
	/* start 同时承载找到的首号或 irq_find_free_area() 的负错误。 */
	int start;

	if (!cnt)
		return -EINVAL;

	if (irq >= 0) {
		/*
		 * 精确模式把搜索起点钉到 irq；若调用者给出的 from 已越过目标，
		 * 请求自相矛盾，尚未取得任何资源即返回。
		 */
		if (from > irq)
			return -EINVAL;
		from = irq;
	} else {
		/*
		 * For interrupts which are freely allocated the
		 * architecture can force a lower bound to the @from
		 * argument. x86 uses this to exclude the GSI space.
		 */
		/*
		 * 自由分配时架构可抬高 @from 下界；x86 用它跳过为
		 * GSI 保留的号码空间。该调整发生在加锁前且只改变搜索策略。
		 */
		from = arch_dynirq_lower_bound(from);
	}

	/* 从“寻找空洞”到“发布整个区间”始终持锁，防止并发分配抢占同一区间。 */
	guard(mutex)(&sparse_irq_lock);

	start = irq_find_free_area(from, cnt);
	if (irq >=0 && start != irq)
		return -EEXIST;

	/*
	 * 稀疏配置可把可用上界扩大到覆盖新范围；静态配置拒绝扩容。扩上界先于
	 * desc 发布，故迭代者不会因上界仍旧过小而漏掉新对象。
	 */
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
/*
 * 从 @offset 开始寻找下一个已分配 IRQ；实现实际上是包含
 * @offset 的“at or after”，找到时返回其逻辑号，没有则返回
 * total_nr_irqs 哨兵。函数用自动 RCU guard 保护 Maple Tree 借用指针，
 * 在读侧退出前只提取稳定的 irq 数值，不把 desc 返回给调用者；不睡眠。
 */
unsigned int irq_get_next_irq(unsigned int offset)
{
	struct irq_desc *desc;

	guard(rcu)();
	desc = irq_find_desc_at_or_after(offset);
	return desc ? irq_desc_get_irq(desc) : total_nr_irqs;
}

/*
 * __irq_get_desc_lock() - 查找、校验并锁定一个 IRQ 描述符。
 *
 * @irq 是逻辑号；@flags 是必非空输出参数，保存调用前本地 IRQ 状态；
 * @bus 决定是否先取得 irq_chip 的慢速总线锁；@check 是
 * _IRQ_DESC_CHECK/_IRQ_DESC_PERCPU 组合，用于拒绝普通与 per-CPU devid
 * API 的类型误用。调用者必须用 __irq_put_desc_unlock() 精确配对。
 *
 * 找不到或类型不匹配返回 NULL，未取得任何锁且 @flags 内容不可使用；
 * 成功返回锁内借用 desc，本地中断已关闭，desc->lock 已持有，若 @bus
 * 为真还持有 chip bus lock。bus lock 可能睡眠，故相应调用点须在合适
 * 上下文；raw lock 临界区不可睡眠。先总线锁后 desc 锁规定了全局锁序。
 */
struct irq_desc *__irq_get_desc_lock(unsigned int irq, unsigned long *flags, bool bus,
				     unsigned int check)
{
	/* desc 在成功返回后由锁保护字段一致性；对象生命周期由 IRQ 注册协议保证。 */
	struct irq_desc *desc;

	desc = irq_to_desc(irq);
	if (!desc)
		return NULL;

	if (check & _IRQ_DESC_CHECK) {
		/*
		 * PERCPU 位表达“调用者要求 per-CPU devid 描述符”；两项检查同时
		 * 排除把 per-CPU API 用于普通 IRQ，或把普通 API 用于 per-CPU IRQ。
		 */
		if ((check & _IRQ_DESC_PERCPU) && !irq_settings_is_per_cpu_devid(desc))
			return NULL;

		if (!(check & _IRQ_DESC_PERCPU) && irq_settings_is_per_cpu_devid(desc))
			return NULL;
	}

	if (bus)
		chip_bus_lock(desc);
	/* irqsave 既防本 CPU 中断重入同一 desc，也和其他 CPU 的配置/处理串行。 */
	raw_spin_lock_irqsave(&desc->lock, *flags);

	return desc;
}

/*
 * __irq_put_desc_unlock() - 释放 __irq_get_desc_lock() 建立的锁状态。
 *
 * @desc 是成功 get 返回的锁内借用对象；@flags 必须是对应调用的保存值；
 * @bus 必须与获取时一致。无返回值。先释放 desc raw lock 并恢复本地 IRQ，
 * 再调用可能完成/同步总线操作的 chip_bus_sync_unlock()，与获取时的锁序
 * 逆序配对；返回后不得再假设 desc 字段稳定。
 */
void __irq_put_desc_unlock(struct irq_desc *desc, unsigned long flags, bool bus)
	__releases(&desc->lock)
{
	raw_spin_unlock_irqrestore(&desc->lock, flags);
	if (bus)
		chip_bus_sync_unlock(desc);
}

/*
 * irq_set_percpu_devid() - 把 IRQ 准备为“每 CPU 独立设备实例”模式。
 *
 * @irq 是已分配逻辑号。调用者处于可睡眠配置上下文，并保证尚无并发
 * request/free 或重复设置；函数本身不取 desc->lock。无 desc 或已经建立
 * percpu_enabled 返回 -EINVAL；分配 per-CPU 启用位图失败返回 -ENOMEM；
 * 成功返回 0，并设置 per-CPU devid 相关 flags。
 *
 * percpu_enabled 的每 CPU 单元记录该 CPU 上此 IRQ 是否启用，所有权转给
 * desc，后续 IRQ 生命周期负责释放。先分配再发布 flags，避免读者看到
 * per-CPU 模式却没有配套存储。
 *
 * 这里的存储是一个普通动态分配的 struct cpumask，每个 CPU 对应其中一位；
 * 它不是 __percpu 区域。enable_percpu_irq()/disable_percpu_irq() 在描述符锁下
 * 设置或清除当前 CPU 位，free_percpu_irq() 据此拒绝释放仍在使用的 action。
 * 当前 free_desc()/irq_kobj_release() 不显式 kfree 该指针，所以接口面向创建后
 * 长期存在的 per-CPU IRQ；“所有权转给 desc”不等于任意动态释放路径都会回收它。
 */
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

/*
 * kstat_incr_irq_this_cpu() - 增加 @irq 在当前 CPU 上的中断统计。
 *
 * 通常在 hardirq 流控路径调用，不睡眠、无返回值。irq_to_desc() 返回借用
 * desc，注册协议保证有效；底层 helper 更新当前 CPU 的 cnt，并按普通
 * 非 per-CPU IRQ 的策略维护 tot_count，避免跨 CPU 原子操作热路径开销。
 */
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
/*
 * 返回逻辑 IRQ @irq 自启动以来在 CPU @cpu 上的累计次数；调用
 * 者必须保证描述符不会并发删除。@cpu 应是合法 possible CPU 编号。找到
 * desc 且统计区存在时返回该 per-CPU cnt 的无锁快照，否则返回 0。
 *
 * 该接口不取 RCU/引用也不锁计数，适用于已有 IRQ 生命周期保护的内核调用
 * 路径；计数可在读取同时增加，因此结果用于统计而非同步判定。
 */
unsigned int kstat_irqs_cpu(unsigned int irq, int cpu)
{
	/* desc 是调用者生命周期保证下的借用指针，不得跨越 IRQ 删除边界。 */
	struct irq_desc *desc = irq_to_desc(irq);

	return desc && desc->kstat_irqs ? per_cpu(desc->kstat_irqs->cnt, cpu) : 0;
}

/*
 * kstat_irqs_desc() - 按 IRQ 类型汇总指定 CPU 集合内的累计次数。
 *
 * @desc 是存活的借用对象；@cpumask 是只读借用集合。普通、非 NMI、非
 * per-CPU IRQ 使用热路径维护的 desc->tot_count 快速返回，避免遍历；
 * per-CPU devid、per-CPU IRQ 和 NMI 则累加每 CPU cnt。返回 unsigned
 * 汇总快照，无错误码、不睡眠。
 *
 * data_race() 明确这些诊断计数允许与中断侧无锁更新竞争：单次读取不用于
 * 控制正确性，也不要求各 CPU 来自同一时刻。它抑制 KCSAN 报告，但不是
 * 锁、原子操作或内存屏障。
 */
static unsigned int kstat_irqs_desc(struct irq_desc *desc, const struct cpumask *cpumask)
{
	/* sum 是所选 CPU 的累计值；cpu 是当前掩码成员。 */
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

/*
 * kstat_irqs() - 返回 @irq 在所有 possible CPU 上的内部统计快照。
 *
 * @irq 是逻辑号。无 desc/统计区返回 0，否则把存活期契约传给
 * kstat_irqs_desc()。返回值无错误区分，因此“IRQ 不存在”和“尚未触发”
 * 都表现为 0；调用者负责用锁、RCU 或固定 IRQ 生命周期保护 desc。
 */
static unsigned int kstat_irqs(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !desc->kstat_irqs)
		return 0;
	return kstat_irqs_desc(desc, cpu_possible_mask);
}

#ifdef CONFIG_GENERIC_IRQ_STAT_SNAPSHOT

/*
 * kstat_snapshot_irqs() - 为当前 CPU 保存所有已分配 IRQ 的计数基线。
 *
 * 入参、返回值：无。调用者保证当前 CPU 上下文稳定，并保证遍历期间描述符
 * 集合不会失效；函数遍历已分配 desc，把当前 CPU 的 cnt 复制到同 CPU 的
 * ref。只写本 CPU per-CPU 存储，无需跨 CPU 锁；其他 CPU 的基线不变。
 * softlockup watchdog 在怀疑硬中断风暴时调用它，随后以
 * kstat_get_irq_since_snapshot() 选出该采样区间内最频繁的 IRQ。
 */
void kstat_snapshot_irqs(void)
{
	/* irq 是迭代键；desc 是每轮借用对象，仅在当前遍历保护范围内有效。 */
	struct irq_desc *desc;
	unsigned int irq;

	for_each_irq_desc(irq, desc) {
		if (!desc->kstat_irqs)
			continue;
		/*
		 * 当前 CPU 上 ref=cnt 建立“此后增量为 0”的基线；中断可能在两次
		 * this_cpu 操作之间发生，因本地执行上下文契约决定快照精度。
		 */
		this_cpu_write(desc->kstat_irqs->ref, this_cpu_read(desc->kstat_irqs->cnt));
	}
}

/*
 * kstat_get_irq_since_snapshot() - 计算当前 CPU 上 @irq 自基线后的增量。
 *
 * 无 desc/统计区返回 0；否则返回当前 CPU cnt-ref 的无符号差。函数不重置
 * 基线、不汇总其他 CPU，调用者须保证 IRQ 不并发删除。计数回绕按无符号
 * 算术工作，适合统计间隔而非严格事件确认。
 */
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
/*
 * 从线程上下文返回 @irq 在全部 CPU 上自启动以来的累计次数。
 * 函数用 RCU 保护访问，因为并发删除描述符会先等待一个 RCU 宽限期，再经
 * delayed_free_desc()/irq_kobj_release() 释放。@irq 是逻辑号；不存在
 * 或尚无统计都返回 0，无错误码。
 *
 * RCU 只保证 desc 与 kstat_irqs 存储在临界区内不被释放；计数仍可并发
 * 更新，所以结果是近似一致快照。函数不睡眠，也不取得可跨临界区的引用。
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
/*
 * __irq_set_lockdep_class() - 为特定 IRQ 覆盖默认的两把锁的 lockdep 类。
 *
 * @irq 是逻辑号；@lock_class 用于 desc->lock；@request_class 用于可睡眠
 * 的 request_mutex，二者都是静态存活的 key 借用指针。通常在 IRQ 建立且
 * 尚无并发使用时调用，不改变运行时锁状态、无返回值；IRQ 不存在则静默
 * 忽略。用途是准确描述级联 IRQ 等合法嵌套，既避免误报也保留真实死锁检查。
 */
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
