// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2004 Linus Torvalds, Ingo Molnar
 *
 * This file contains the /proc/irq/ handling code.
 */

#include <linux/irq.h>
#include <linux/gfp.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/string.h>

#include "internals.h"

/*
 * 本文件维护两类用户接口：/proc/irq/<N>/ 下针对单个描述符的 affinity、NUMA、
 * spurious 与 handler 目录，以及 /proc/interrupts 的全局 seq_file 快照。前者依赖
 * procfs 删除等待正在执行的 file op，后者用 RCU 查 Maple Tree、为选中 desc 取得
 * 引用，再在 show/next/stop 中按协议归还。
 *
 * 单 IRQ 可变字段由 desc->lock 保护；affinity 写入通过核心 setter 与 CPU 在线集合
 * 验证。/proc/interrupts 的格式宽度由全局 raw lock 单调扩展，每次 seq 遍历复制
 * 快照，避免显示过程中列宽变化。注册/注销必须先撤销 proc 可见性再释放描述符。
 */

/*
 * Access rules:
 *
 * procfs protects read/write of /proc/irq/N/ files against a
 * concurrent free of the interrupt descriptor. remove_proc_entry()
 * immediately prevents new read/writes to happen and waits for
 * already running read/write functions to complete.
 *
 * We remove the proc entries first and then delete the interrupt
 * descriptor from the radix tree and free it. So it is guaranteed
 * that irq_to_desc(N) is valid as long as the read/writes are
 * permitted by procfs.
 *
 * The read from /proc/interrupts is a different problem because there
 * is no protection. So the lookup and the access to irqdesc
 * information must be protected by sparse_irq_lock.
 */
/*
 * 原文描述的访问规则是：procfs 在删除 /proc/irq/N 文件时立即拒绝新读写，并等待
 * 已进入的 file op 完成；IRQ 销毁先移除这些入口，再从描述符索引摘除对象，因此
 * 单 IRQ 回调期间 irq_to_desc(N) 有效。
 *
 * 原文后半的 radix tree/sparse_irq_lock 已与当前实现不一致：描述符索引现为
 * Maple Tree，/proc/interrupts 的 seq 迭代在 RCU 下查找，并用 irq_desc_get_ref()
 * 把对象寿命延长到 show/next/stop；不能再把这段旧说明当作当前锁实现。
 */
/* /proc/irq 根目录句柄；创建失败时所有按 IRQ 管理入口保持关闭。 */
static struct proc_dir_entry *root_irq_dir;

#ifdef CONFIG_SMP

/* show/write 包装器传入的内部数据源与 ABI 格式选择值，不暴露给用户空间。 */
enum {
	AFFINITY,
	AFFINITY_LIST,
	EFFECTIVE,
	EFFECTIVE_LIST,
};

/*
 * 输出单个 IRQ 的配置 affinity、pending affinity 或硬件 effective affinity。
 * @type 同时选择数据源和 bitmap/list 格式；m->private 保存 IRQ 号。procfs 保证 desc
 * 生命周期，函数持 desc raw lock 选择稳定 mask：配置 affinity 若有延迟 move 则显示
 * pending mask，effective 类型仅在对应配置启用时有效。成功写一行并返回 0，未知/
 * 不支持类型返回 -EINVAL。
 */
static int show_irq_affinity(int type, struct seq_file *m)
{
	struct irq_desc *desc = irq_to_desc((long)m->private);
	const struct cpumask *mask;

	guard(raw_spinlock_irq)(&desc->lock);

	/* 第一阶段在锁内选择与当前 move 状态一致的掩码。 */
	switch (type) {
	case AFFINITY:
	case AFFINITY_LIST:
		mask = desc->irq_common_data.affinity;
		if (irq_move_pending(&desc->irq_data))
			mask = irq_desc_get_pending_mask(desc);
		break;
	case EFFECTIVE:
	case EFFECTIVE_LIST:
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
		mask = irq_data_get_effective_affinity_mask(&desc->irq_data);
		break;
#endif
	default:
		return -EINVAL;
	}

	/* 第二阶段按 cpulist 或十六进制 bitmap ABI 格式输出。 */
	switch (type) {
	case AFFINITY_LIST:
	case EFFECTIVE_LIST:
		seq_printf(m, "%*pbl\n", cpumask_pr_args(mask));
		break;
	case AFFINITY:
	case EFFECTIVE:
		seq_printf(m, "%*pb\n", cpumask_pr_args(mask));
		break;
	}
	return 0;
}

/*
 * 输出 IRQ 的 affinity hint 位图。
 * 先分配全零临时 cpumask，再在 desc 锁内复制可选 affinity_hint，锁外写 seq_file；
 * 这样不会持 raw 锁执行可能扩展缓冲区的输出。无 hint 时显示全零掩码。分配失败
 * -ENOMEM，成功释放临时对象并返回 0。
 */
static int irq_affinity_hint_proc_show(struct seq_file *m, void *v)
{
	struct irq_desc *desc = irq_to_desc((long)m->private);
	cpumask_var_t mask;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	scoped_guard(raw_spinlock_irq, &desc->lock) {
		if (desc->affinity_hint)
			cpumask_copy(mask, desc->affinity_hint);
	}

	seq_printf(m, "%*pb\n", cpumask_pr_args(mask));
	free_cpumask_var(mask);
	return 0;
}

/* 非零时全局禁止用户通过 /proc 改写 IRQ affinity，由启动参数/架构策略维护。 */
int no_irq_affinity;
/* 以十六进制 bitmap 格式显示配置或 pending affinity。 */
static int irq_affinity_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(AFFINITY, m);
}

/* 以 CPU 列表格式显示配置或 pending affinity。 */
static int irq_affinity_list_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(AFFINITY_LIST, m);
}

#ifndef CONFIG_AUTO_IRQ_AFFINITY
/*
 * 未启用架构自动选择时，用户提交不含在线 CPU 的 mask 一律无效。
 * 原文解释了两种时序：IRQ 已启动时已有在线目标，无需随机迁移；尚未启动时，
 * irq_setup_affinity() 会在启动阶段重新选在线 CPU，所以此刻写空集合也没有意义。
 */
static inline int irq_select_affinity_usr(unsigned int irq)
{
	/*
	 * If the interrupt is started up already then this fails. The
	 * interrupt is assigned to an online CPU already. There is no
	 * point to move it around randomly. Tell user space that the
	 * selected mask is bogus.
	 *
	 * If not then any change to the affinity is pointless because the
	 * startup code invokes irq_setup_affinity() which will select
	 * a online CPU anyway.
	 */
	return -EINVAL;
}
#else
/* ALPHA magic affinity auto selector. Keep it for historical reasons. */
/*
 * 这是为 ALPHA 历史 ABI 保留的自动亲和性选择入口；启用该配置时，把
 * “没有在线 CPU 的用户 mask”交给架构选择器，成功后由其完成默认目标选择。
 */
static inline int irq_select_affinity_usr(unsigned int irq)
{
	return irq_select_affinity(irq);
}
#endif

/*
 * 处理 smp_affinity(type=0，十六进制 bitmap) 与 smp_affinity_list(type!=0，
 * CPU 列表) 的共同写路径。先拒绝不可由用户迁移的 IRQ 和全局禁用状态，再解析到
 * 临时 cpumask。正常 mask 至少命中一个在线 CPU，并通过 irq_set_affinity() 进入
 * 核心迁移/延迟迁移协议；完全不命中在线集合时只允许架构自动选择分支。
 *
 * 解析、权限、setter 或架构选择失败时返回负 errno；成功必须返回原始 count，
 * 使 procfs 将整次写入视为已消费。所有出口都释放临时 mask。
 */
static ssize_t write_irq_affinity(int type, struct file *file,
		const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned int irq = (int)(long)pde_data(file_inode(file));
	cpumask_var_t new_value;
	int err;

	if (!irq_can_set_affinity_usr(irq) || no_irq_affinity)
		return -EPERM;

	if (!zalloc_cpumask_var(&new_value, GFP_KERNEL))
		return -ENOMEM;

	if (type)
		err = cpumask_parselist_user(buffer, count, new_value);
	else
		err = cpumask_parse_user(buffer, count, new_value);
	if (err)
		goto free_cpumask;

	/*
	 * Do not allow disabling IRQs completely - it's a too easy
	 * way to make the system unusable accidentally :-) At least
	 * one online CPU still has to be targeted.
	 */
	/* 原文强调必须保留至少一个在线目标，避免一次误写令 IRQ 完全不可投递。 */
	if (!cpumask_intersects(new_value, cpu_online_mask)) {
		/*
		 * Special case for empty set - allow the architecture code
		 * to set default SMP affinity.
		 */
		/* 原文特例：空/离线集合可交给支持该能力的架构恢复默认 affinity。 */
		err = irq_select_affinity_usr(irq) ? -EINVAL : count;
	} else {
		err = irq_set_affinity(irq, new_value);
		if (!err)
			err = count;
	}

free_cpumask:
	free_cpumask_var(new_value);
	return err;
}

/* 十六进制 bitmap ABI 的轻量写包装。 */
static ssize_t irq_affinity_proc_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *pos)
{
	return write_irq_affinity(0, file, buffer, count, pos);
}

/* CPU 列表 ABI 的轻量写包装。 */
static ssize_t irq_affinity_list_proc_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *pos)
{
	return write_irq_affinity(1, file, buffer, count, pos);
}

/* 建立 smp_affinity 的 single_open 上下文，并把 proc 私有 IRQ 号传给 show。 */
static int irq_affinity_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, irq_affinity_proc_show, pde_data(inode));
}

/* 建立 smp_affinity_list 的 single_open 上下文。 */
static int irq_affinity_list_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, irq_affinity_list_proc_show, pde_data(inode));
}

/* smp_affinity 的 single_open/seq 读写协议表。 */
static const struct proc_ops irq_affinity_proc_ops = {
	.proc_open	= irq_affinity_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= irq_affinity_proc_write,
};

/* smp_affinity_list 与 bitmap 文件共享状态，只替换显示/解析格式。 */
static const struct proc_ops irq_affinity_list_proc_ops = {
	.proc_open	= irq_affinity_list_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= irq_affinity_list_proc_write,
};

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
/* 以十六进制 bitmap 输出 irqchip 已实际采用的 effective affinity。 */
static int irq_effective_aff_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(EFFECTIVE, m);
}

/* 以 CPU 列表输出 effective affinity。 */
static int irq_effective_aff_list_proc_show(struct seq_file *m, void *v)
{
	return show_irq_affinity(EFFECTIVE_LIST, m);
}
#endif

/* 输出新 IRQ 在没有专用策略时使用的全局默认亲和性 bitmap。 */
static int default_affinity_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%*pb\n", cpumask_pr_args(irq_default_affinity));
	return 0;
}

/*
 * 更新 irq_default_affinity。输入采用十六进制 bitmap，必须至少包含一个当前在线
 * CPU，避免将默认投递集合清空；解析成功后一次 cpumask_copy 发布新策略，只影响
 * 后续读取该默认值的分配/初始化流程，不逐个迁移已有 IRQ。成功返回 count，失败
 * 返回 -ENOMEM、解析错误或 -EINVAL，并始终释放临时 mask。
 */
static ssize_t default_affinity_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *ppos)
{
	cpumask_var_t new_value;
	int err;

	if (!zalloc_cpumask_var(&new_value, GFP_KERNEL))
		return -ENOMEM;

	err = cpumask_parse_user(buffer, count, new_value);
	if (err)
		goto out;

	/*
	 * Do not allow disabling IRQs completely - it's a too easy
	 * way to make the system unusable accidentally :-) At least
	 * one online CPU still has to be targeted.
	 */
	/* 原文同样要求至少保留一个在线 CPU，防止默认策略令新 IRQ 无处投递。 */
	if (!cpumask_intersects(new_value, cpu_online_mask)) {
		err = -EINVAL;
		goto out;
	}

	cpumask_copy(irq_default_affinity, new_value);
	err = count;

out:
	free_cpumask_var(new_value);
	return err;
}

/* 建立 default_smp_affinity 的 single_open 上下文。 */
static int default_affinity_open(struct inode *inode, struct file *file)
{
	return single_open(file, default_affinity_show, pde_data(inode));
}

/* 全局默认 affinity 文件的 seq 读写操作表。 */
static const struct proc_ops default_affinity_proc_ops = {
	.proc_open	= default_affinity_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= default_affinity_write,
};

/* 输出该 IRQ 描述符关联的 NUMA node；负值表示没有指定节点。 */
static int irq_node_proc_show(struct seq_file *m, void *v)
{
	struct irq_desc *desc = irq_to_desc((long) m->private);

	seq_printf(m, "%d\n", irq_desc_get_node(desc));
	return 0;
}
#endif

/*
 * 输出 spurious 检测的累计 IRQ 次数、未处理次数和最近未处理时间。
 * procfs 只保证 desc 寿命；这些计数由中断路径并发更新，此处提供诊断快照而非同一
 * 锁点上的事务视图。jiffies 时间戳转换成用户 ABI 使用的毫秒。
 */
static int irq_spurious_proc_show(struct seq_file *m, void *v)
{
	struct irq_desc *desc = irq_to_desc((long) m->private);

	seq_printf(m, "count %u\n" "unhandled %u\n" "last_unhandled %u ms\n",
		   desc->irq_count, desc->irqs_unhandled,
		   jiffies_to_msecs(desc->last_unhandled));
	return 0;
}

/* handler 目录名的栈缓冲上限；strscpy() 保证过长 action 名被安全截断。 */
#define MAX_NAMELEN 128

/*
 * 检查 @new_action 的名字在同一共享 IRQ 的 action 链上是否唯一。
 * desc raw lock 稳定 action 链；跳过对象自身，使重新检查已挂接 action 不会与自己
 * 冲突。发现另一个同名且非 NULL 的名字返回 false，否则返回 true。
 */
static bool name_unique(unsigned int irq, struct irqaction *new_action)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action;

	guard(raw_spinlock_irq)(&desc->lock);
	for_each_action_of_desc(desc, action) {
		if ((action != new_action) && action->name &&
		    !strcmp(new_action->name, action->name))
			return false;
	}
	return true;
}

/*
 * 为已注册的 handler 创建 /proc/irq/<irq>/<action-name>/ 目录。只有 IRQ 主目录
 * 存在、action 尚无目录、名字有效且在共享链上唯一时才创建；名字先复制到固定
 * 栈缓冲，避免把可变 action 字段直接交给 procfs。失败保持 action->dir 为 NULL，
 * 此诊断目录不影响 handler 本身的安装结果。
 */
void register_handler_proc(unsigned int irq, struct irqaction *action)
{
	char name[MAX_NAMELEN];
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc->dir || action->dir || !action->name || !name_unique(irq, action))
		return;

	strscpy(name, action->name);

	/* create /proc/irq/1234/handler/ */
	/* 此处建立形如 /proc/irq/1234/handler/ 的处理器目录。 */
	action->dir = proc_mkdir(name, desc->dir);
}

#undef MAX_NAMELEN

/* 32 位 unsigned IRQ 十进制文本最多 10 位，再留一个字符串终止字节。 */
#define MAX_NAMELEN 11

/*
 * 按需建立 /proc/irq/<irq>/ 及其属性文件。描述符创建时不会立即注册目录，而是等
 * handler 安装，因此 register_lock 串行化多个安装者的首次创建，并以 desc->dir
 * 作为已完成标志。无 proc 根、no_irq_chip、已创建或 mkdir 失败时直接返回。
 *
 * affinity 文件的写权限取决于 irq_can_set_affinity_usr()；各子文件创建失败不会
 * 回滚主目录，后续读取只能看到实际创建成功的条目。proc 私有数据保存 IRQ 号，
 * 生命周期由 unregister_irq_proc() 先删入口、等待在途 file op 的协议保证。
 */
void register_irq_proc(unsigned int irq, struct irq_desc *desc)
{
	static DEFINE_MUTEX(register_lock);
	void __maybe_unused *irqp = (void *)(unsigned long) irq;
	char name [MAX_NAMELEN];

	if (!root_irq_dir || (desc->irq_data.chip == &no_irq_chip))
		return;

	/*
	 * irq directories are registered only when a handler is
	 * added, not when the descriptor is created, so multiple
	 * tasks might try to register at the same time.
	 */
	/* 目录延迟到 handler 加入时创建，所以并发注册任务必须在此互斥。 */
	guard(mutex)(&register_lock);

	if (desc->dir)
		return;

	/* create /proc/irq/1234 */
	/* 先创建 IRQ 数字命名的主目录，再在其中发布属性文件。 */
	snprintf(name, MAX_NAMELEN, "%u", irq);
	desc->dir = proc_mkdir(name, root_irq_dir);
	if (!desc->dir)
		return;

#ifdef CONFIG_SMP
	umode_t umode = S_IRUGO;

	if (irq_can_set_affinity_usr(desc->irq_data.irq))
		umode |= S_IWUSR;

	/* create /proc/irq/<irq>/smp_affinity */
	/* 此条目提供 bitmap 格式的 smp_affinity ABI。 */
	proc_create_data("smp_affinity", umode, desc->dir, &irq_affinity_proc_ops, irqp);

	/* create /proc/irq/<irq>/affinity_hint */
	/* 此只读条目展示驱动给出的 affinity hint。 */
	proc_create_single_data("affinity_hint", 0444, desc->dir,
				irq_affinity_hint_proc_show, irqp);

	/* create /proc/irq/<irq>/smp_affinity_list */
	/* 此条目提供 CPU 列表格式的 smp_affinity ABI。 */
	proc_create_data("smp_affinity_list", umode, desc->dir,
			 &irq_affinity_list_proc_ops, irqp);

	proc_create_single_data("node", 0444, desc->dir, irq_node_proc_show, irqp);
# ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	proc_create_single_data("effective_affinity", 0444, desc->dir,
				irq_effective_aff_proc_show, irqp);
	proc_create_single_data("effective_affinity_list", 0444, desc->dir,
				irq_effective_aff_list_proc_show, irqp);
# endif
#endif
	proc_create_single_data("spurious", 0444, desc->dir,
				irq_spurious_proc_show, (void *)(long)irq);

}

/*
 * 撤销单 IRQ 的全部 proc 可见性。先逐项删除子文件，再删除数字目录；procfs 会阻止
 * 新 file op 并等待已开始操作退出，因此调用者随后才能安全地从全局索引移除并释放
 * desc。该函数用于描述符销毁路径，故不再把 desc->dir 清 NULL；根或目录不存在时
 * 无操作。
 */
void unregister_irq_proc(unsigned int irq, struct irq_desc *desc)
{
	char name [MAX_NAMELEN];

	if (!root_irq_dir || !desc->dir)
		return;
#ifdef CONFIG_SMP
	remove_proc_entry("smp_affinity", desc->dir);
	remove_proc_entry("affinity_hint", desc->dir);
	remove_proc_entry("smp_affinity_list", desc->dir);
	remove_proc_entry("node", desc->dir);
# ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	remove_proc_entry("effective_affinity", desc->dir);
	remove_proc_entry("effective_affinity_list", desc->dir);
# endif
#endif
	remove_proc_entry("spurious", desc->dir);

	snprintf(name, MAX_NAMELEN, "%u", irq);
	remove_proc_entry(name, root_irq_dir);
}

#undef MAX_NAMELEN

/*
 * 删除某 action 的 handler 目录。proc_remove(NULL) 可安全无操作；真正的 action
 * 释放必须发生在 procfs 撤销该目录之后，避免目录私有状态越过 action 生命周期。
 */
void unregister_handler_proc(unsigned int irq, struct irqaction *action)
{
	proc_remove(action->dir);
}

/*
 * 在 SMP 构建中创建全局 /proc/irq/default_smp_affinity；创建失败仅缺少该管理
 * 接口，不阻止 IRQ 子系统继续初始化。非 SMP 构建为空操作。
 */
static void register_default_affinity_proc(void)
{
#ifdef CONFIG_SMP
	proc_create("irq/default_smp_affinity", 0644, NULL,
		    &default_affinity_proc_ops);
#endif
}

/*
 * 初始化 /proc/irq 管理树。先建立根目录与全局默认 affinity 文件，再遍历当前已
 * 存在的描述符补注册单 IRQ 目录；运行期新装 handler 由 register_irq_proc()
 * 增量补齐。根目录创建失败时整个 proc IRQ 接口保持不可用。
 */
void init_irq_proc(void)
{
	unsigned int irq;
	struct irq_desc *desc;

	/* create /proc/irq */
	/* 此处创建所有单 IRQ 管理文件共享的 /proc/irq 根目录。 */
	root_irq_dir = proc_mkdir("irq", NULL);
	if (!root_irq_dir)
		return;

	register_default_affinity_proc();

	/*
	 * Create entries for all existing IRQs.
	 */
	/* 要为初始化前已经存在的描述符补建入口。 */
	for_each_irq_desc(irq, desc)
		register_irq_proc(irq, desc);
}

/*
 * 更新描述符能否出现在 /proc/interrupts 中的缓存设置位。隐藏 IRQ、chained
 * irqchip 内部级联入口以及尚无 action 的描述符均不可输出，其余置
 * _IRQ_PROC_VALID；调用者在 action 安装/移除或属性变化的同步上下文中负责发布。
 */
void irq_proc_update_valid(struct irq_desc *desc)
{
	u32 set = _IRQ_PROC_VALID;

	if (irq_settings_is_hidden(desc) || irq_desc_is_chained(desc) || !desc->action)
		set = 0;

	irq_settings_update_proc_valid(desc, set);
}

#ifdef CONFIG_GENERIC_IRQ_SHOW

/* seq 迭代器在所有普通 desc 之后返回的哨兵，触发架构附加行而不持 desc 引用。 */
#define ARCH_PROC_IRQDESC ((void *)0x00001111)

/* 架构可覆盖此弱钩子，在通用 IRQ 行之后输出 IPI 等架构专属中断统计。 */
int __weak arch_show_interrupts(struct seq_file *p, int prec)
{
	return 0;
}

/* 保护全局显示约束的扩展；读者为每次 seq 遍历取得一个稳定快照。 */
static DEFINE_RAW_SPINLOCK(irq_proc_constraints_lock);

/*
 * /proc/interrupts 的共享格式上限：print_header 只在每个打开实例的私有副本中使用，
 * num_prec 容纳 IRQ 十进制编号，chip_width 容纳最长 irq_chip 名。后两者在运行期
 * 只增不减，避免已有输出因新 IRQ/irqchip 注册而缩窄。
 */
static struct irq_proc_constraints {
	bool		print_header;
	unsigned int	num_prec;
	unsigned int	chip_width;
} irq_proc_constraints __read_mostly = {
	.num_prec	= 4,
	.chip_width	= 8,
};

#ifndef ACTUAL_NR_IRQS
/* 架构未提供实际 IRQ 上限时，以动态 total_nr_irqs 作为通用上限。 */
# define ACTUAL_NR_IRQS total_nr_irqs
#endif

/*
 * 根据 total_nr_irqs 计算 IRQ 编号列宽，最少 4 位、最多 10 位；在 irqsave raw
 * lock 下仅在新值更大时更新全局约束。IRQ 描述符空间扩展/初始化路径调用它，因而
 * 并发打开的旧 seq 快照可保持旧宽度，之后的新打开会采用更宽格式。
 */
void irq_proc_calc_prec(void)
{
	unsigned int prec, n;

	for (prec = 4, n = 10000; prec < 10 && n <= total_nr_irqs; ++prec)
		n *= 10;

	guard(raw_spinlock_irqsave)(&irq_proc_constraints_lock);
	if (prec > irq_proc_constraints.num_prec)
		WRITE_ONCE(irq_proc_constraints.num_prec, prec);
}

/*
 * 把有效 irq_chip 名字长度并入全局列宽上限。无 chip/名字或长度未超过当前快照时
 * 快速返回；否则在 irqsave raw lock 下复查并单调更新。调用点可能已关中断，因此
 * 必须使用 irqsave 形式，不能假定普通进程上下文。
 */
void irq_proc_update_chip(const struct irq_chip *chip)
{
	unsigned int len = chip && chip->name ? strlen(chip->name) : 0;

	if (!len || len <= READ_ONCE(irq_proc_constraints.chip_width))
		return;

	/* Can be invoked from interrupt disabled contexts */
	/* 原文提示本函数可能从已禁中断上下文进入，所以锁操作必须保存/恢复 flags。 */
	guard(raw_spinlock_irqsave)(&irq_proc_constraints_lock);
	if (len > irq_proc_constraints.chip_width)
		WRITE_ONCE(irq_proc_constraints.chip_width, len);
}

/* Same as seq_put_decimal_ull_width(p, " ", cnt, 10) */
/*
 * 每个片段等价于输出一个宽度 10、前导空格的十进制零。预构造 1/16/256
 * 个片段，让连续零 CPU 列能用少量 seq_write() 批量写出，保持既有文本 ABI。
 */
#define ZSTR1 "          0"
#define ZSTR1_LEN	(sizeof(ZSTR1) - 1)
#define ZSTR16		ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 \
			ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1 ZSTR1
#define ZSTR256		ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 \
			ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16 ZSTR16

/*
 * 输出 @zeros 个固定宽度零列。每轮最多取 256 个预构造片段，最后一轮按实际字节数
 * 截断；零个直接返回。该函数只压缩格式化工作，不改变 /proc/interrupts 文本。
 */
static inline void irq_proc_emit_zero_counts(struct seq_file *p, unsigned int zeros)
{
	if (!zeros)
		return;

	for (unsigned int n = min(zeros, 256); n; zeros -= n, n = min(zeros, 256))
		seq_write(p, ZSTR256, n * ZSTR1_LEN);
}

/*
 * 把一个 CPU 计数并入延迟零游程：当前值为零时仅递增 @zeros；遇到非零值时先刷出
 * 累积零列，再以宽度 10 输出当前计数并返回 0。返回值供下一 CPU 继续携带零游程。
 */
static inline unsigned int irq_proc_emit_count(struct seq_file *p, unsigned int cnt,
					       unsigned int zeros)
{
	if (!cnt)
		return zeros + 1;

	irq_proc_emit_zero_counts(p, zeros);
	seq_put_decimal_ull_width(p, " ", cnt, 10);
	return 0;
}

/*
 * 按在线 CPU 顺序输出 per-CPU IRQ 计数。连续零值经 irq_proc_emit_count() 延迟，
 * 最后显式刷出尾部零游程；调用期间 CPU 在线集合可能变化，因此这是诊断性遍历，
 * 列头与数据依赖同一次读取附近的在线集合而非热插拔事务快照。
 */
void irq_proc_emit_counts(struct seq_file *p, unsigned int __percpu *cnts)
{
	unsigned int cpu, zeros = 0;

	for_each_online_cpu(cpu)
		zeros = irq_proc_emit_count(p, per_cpu(*cnts, cpu), zeros);
	irq_proc_emit_zero_counts(p, zeros);
}

/*
 * 输出一次 seq 元素。每次打开首次调用先打印在线 CPU 列头；架构哨兵转交
 * arch_show_interrupts()。普通 desc 行依次输出逻辑 IRQ、每 CPU 计数、irqchip、
 * hwirq、可选触发类型/desc 名和共享 action 名链。
 *
 * per-CPU IRQ 始终读取计数；设备 IRQ 用无锁 tot_count 快照跳过从未发生过的项，
 * 该 data_race 仅用于选择“逐 CPU 读”还是“等价零列”的显示优化。后半段在 desc
 * raw lock 下稳定 chip/domain/name/action 拓扑；seq 迭代器已持 desc rcuref，保证
 * 整次 show 即使与描述符摘除并发也不会访问已释放对象。成功返回 0，架构钩子的
 * 返回值原样上传。
 */
static int irq_seq_show(struct seq_file *p, void *v)
{
	struct irq_proc_constraints *constr = p->private;
	struct irq_desc *desc = v;
	struct irqaction *action;

	/* Print header for the first interrupt? */
	/* 每次遍历仅在第一个元素前输出一次 CPU 表头。 */
	if (constr->print_header) {
		unsigned int cpu;

		seq_printf(p, "%*s", constr->num_prec + 8, "");
		for_each_online_cpu(cpu)
			seq_printf(p, "CPU%-8d", cpu);
		seq_putc(p, '\n');
		constr->print_header = false;
	}

	if (desc == ARCH_PROC_IRQDESC)
		return arch_show_interrupts(p, constr->num_prec);

	seq_put_decimal_ull_width(p, "", irq_desc_get_irq(desc), constr->num_prec);
	seq_putc(p, ':');

	/*
	 * Always output per CPU interrupts. Output device interrupts only when
	 * desc::tot_count is not zero.
	 */
	/*
	 * 原文要求 per-CPU IRQ 始终输出真实计数；普通设备 IRQ 仅在 tot_count 非零时
	 * 读取各 CPU 计数，否则直接生成全零列以减少冷 IRQ 开销。
	 */
	if (irq_settings_is_per_cpu(desc) || irq_settings_is_per_cpu_devid(desc) ||
	    data_race(desc->tot_count))
		irq_proc_emit_counts(p, &desc->kstat_irqs->cnt);
	else
		irq_proc_emit_zero_counts(p, num_online_cpus());

	/* Enforce a visual gap */
	/* 原文要求在计数区与 irqchip 信息间固定留出两个空格。 */
	seq_write(p, "  ", 2);

	guard(raw_spinlock_irq)(&desc->lock);
	if (desc->irq_data.chip) {
		if (desc->irq_data.chip->irq_print_chip)
			desc->irq_data.chip->irq_print_chip(&desc->irq_data, p);
		else if (desc->irq_data.chip->name)
			seq_printf(p, "%-*s", constr->chip_width, desc->irq_data.chip->name);
		else
			seq_printf(p, "%-*s", constr->chip_width, "-");
	} else {
		seq_printf(p, "%-*s", constr->chip_width, "None");
	}

	seq_putc(p, ' ');
	if (desc->irq_data.domain)
		seq_put_decimal_ull_width(p, "", desc->irq_data.hwirq, constr->num_prec);
	else
		seq_printf(p, " %*s", constr->num_prec, "");

	if (IS_ENABLED(CONFIG_GENERIC_IRQ_SHOW_LEVEL))
		seq_printf(p, " %-8s", irqd_is_level_type(&desc->irq_data) ? "Level" : "Edge");

	if (desc->name)
		seq_printf(p, "-%-8s", desc->name);

	action = desc->action;
	if (action) {
		seq_printf(p, "  %s", action->name);
		while ((action = action->next) != NULL)
			seq_printf(p, ", %s", action->name);
	}

	seq_putc(p, '\n');
	return 0;
}

/*
 * 从 *@pos 起查找下一个可输出描述符。函数在 RCU 读侧遍历 Maple Tree；找到对象后
 * 先把位置修正为真实 IRQ 号，只对 _IRQ_PROC_VALID 对象尝试 irq_desc_get_ref()。
 * 取得 rcuref 后即可在退出 RCU 临界区后安全交给 seq show；无效或正在销毁的对象
 * 跳到下一编号继续。
 *
 * 描述符耗尽时把位置设为 total_nr_irqs 并返回架构哨兵，使架构附加统计恰好出现
 * 一次；位置已经越过该上限则返回 NULL 结束遍历。
 */
static void *irq_seq_next_desc(loff_t *pos)
{
	if (*pos > total_nr_irqs)
		return NULL;

	guard(rcu)();
	for (;;) {
		struct irq_desc *desc = irq_find_desc_at_or_after((unsigned int) *pos);

		if (desc) {
			*pos = irq_desc_get_irq(desc);
			/*
			 * If valid for output then try to acquire a reference
			 * count on the descriptor so that it can't be freed
			 * after dropping RCU read lock on return.
			 */
			/*
			 * 原文要求仅对可输出对象取得描述符引用，使函数返回并退出 RCU 后，
			 * desc 仍不能被释放；获取失败表示对象已进入销毁阶段，应跳过。
			 */
			if (irq_settings_proc_valid(desc) && irq_desc_get_ref(desc))
				return desc;
			(*pos)++;
		} else {
			*pos = total_nr_irqs;
			return ARCH_PROC_IRQDESC;
		}
	}
}

/*
 * 开始或重启一次 seq 遍历。位置为 0 时把全局单调格式约束复制到此 open 的私有
 * 对象并请求打印表头，随后统一查找首个 desc；非零 seek 则保留已有实例状态，
 * 直接从指定位置继续。
 */
static void *irq_seq_start(struct seq_file *f, loff_t *pos)
{
	if (!*pos) {
		struct irq_proc_constraints *constr = f->private;

		constr->num_prec = READ_ONCE(irq_proc_constraints.num_prec);
		constr->chip_width = READ_ONCE(irq_proc_constraints.chip_width);
		constr->print_header = true;
	}
	return irq_seq_next_desc(pos);
}

/*
 * 推进到下一 seq 元素。先归还当前普通 desc 的 rcuref，架构哨兵不持引用；再递增
 * 逻辑位置并调用共同查找器。这样每个成功 get 都由 next 或 stop 恰好 put 一次。
 */
static void *irq_seq_next(struct seq_file *f, void *v, loff_t *pos)
{
	if (v && v != ARCH_PROC_IRQDESC)
		irq_desc_put_ref(v);

	(*pos)++;
	return irq_seq_next_desc(pos);
}

/* 遍历正常结束、用户中断读取或 show 出错时，归还仍由 seq 持有的普通 desc 引用。 */
static void irq_seq_stop(struct seq_file *f, void *v)
{
	if (v && v != ARCH_PROC_IRQDESC)
		irq_desc_put_ref(v);
}

/* /proc/interrupts 的 seq 生命周期表；私有格式快照由创建入口分配。 */
static const struct seq_operations irq_seq_ops = {
	.start = irq_seq_start,
	.next  = irq_seq_next,
	.stop  = irq_seq_stop,
	.show  = irq_seq_show,
};

/*
 * 在 fs 初始化阶段创建 /proc/interrupts，并为每次 open 分配一个
 * irq_proc_constraints 私有副本。创建失败不会阻止启动，函数仍返回 0；缺失的 proc
 * 文件只是观测能力降级，不影响 IRQ 投递。
 */
static int __init irq_proc_init(void)
{
	proc_create_seq_private("interrupts", 0, NULL, &irq_seq_ops,
				sizeof(irq_proc_constraints), NULL);
	return 0;
}
fs_initcall(irq_proc_init);

#endif
