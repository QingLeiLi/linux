// SPDX-License-Identifier: GPL-2.0
// Copyright 2017 Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>

#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/uaccess.h>

#include "internals.h"

/*
 * 本文件为每个 IRQ 建立 /sys/kernel/debug/irq/irqs/<N> 诊断文件，集中展示
 * irq_desc、叶到根 irq_data/irq_chip 层级及 affinity，并提供仅用于调试的
 * "trigger" 软件注入入口。debugfs 不是稳定用户 ABI，创建失败不得影响 IRQ 核心。
 *
 * 文件数据是 irq_desc 借用指针；debugfs_create_file() 的安全代理使 remove 等待
 * 在途 file operation，free_desc() 先删除文件再释放描述符。show 在 desc raw lock
 * 下取得一致的核心状态，domain/chip debug_show 回调也在该锁区内执行。
 */

/* /sys/kernel/debug/irq/irqs 目录；初始化后供动态描述符注册路径长期借用。 */
static struct dentry *irq_dir;

/*
 * 把 @state 中命中的位按描述表 @sd 展开为名字行。@size 是表项数，@ind 控制前导
 * 缩进；每个与 mask 有交集的条目输出一次，未知位只保留在调用者的十六进制总值
 * 中。函数不加锁、不修改输入，调用者负责稳定状态和描述表生命周期。
 */
void irq_debug_show_bits(struct seq_file *m, int ind, unsigned int state,
			 const struct irq_bit_descr *sd, int size)
{
	int i;

	for (i = 0; i < size; i++, sd++) {
		if (state & sd->mask)
			seq_printf(m, "%*s%s\n", ind + 12, "", sd->name);
	}
}

#ifdef CONFIG_SMP
/*
 * 输出 desc 的配置 affinity，以及配置启用时的 effective/pending mask。
 * 调用者 irq_debug_show() 已持 desc->lock，故 irq_data 与可选 pending_mask 在本次
 * 展示中稳定；所有掩码均以 cpulist 格式输出。函数只借用 mask，不保存指针。
 */
static void irq_debug_show_masks(struct seq_file *m, struct irq_desc *desc)
{
	struct irq_data *data = irq_desc_get_irq_data(desc);
	const struct cpumask *msk;

	msk = irq_data_get_affinity_mask(data);
	seq_printf(m, "affinity: %*pbl\n", cpumask_pr_args(msk));
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	msk = irq_data_get_effective_affinity_mask(data);
	seq_printf(m, "effectiv: %*pbl\n", cpumask_pr_args(msk));
#endif
#ifdef CONFIG_GENERIC_PENDING_IRQ
	msk = desc->pending_mask;
	seq_printf(m, "pending:  %*pbl\n", cpumask_pr_args(msk));
#endif
}
#else
/* UP 构建没有 CPU affinity 诊断字段，保留同签名空实现供共同 show 路径调用。 */
static void irq_debug_show_masks(struct seq_file *m, struct irq_desc *desc) { }
#endif

/* irq_chip::flags 的可识别位到调试名称映射；未列出的新位仍显示在原始数值中。 */
static const struct irq_bit_descr irqchip_flags[] = {
	BIT_MASK_DESCR(IRQCHIP_SET_TYPE_MASKED),
	BIT_MASK_DESCR(IRQCHIP_EOI_IF_HANDLED),
	BIT_MASK_DESCR(IRQCHIP_MASK_ON_SUSPEND),
	BIT_MASK_DESCR(IRQCHIP_ONOFFLINE_ENABLED),
	BIT_MASK_DESCR(IRQCHIP_SKIP_SET_WAKE),
	BIT_MASK_DESCR(IRQCHIP_ONESHOT_SAFE),
	BIT_MASK_DESCR(IRQCHIP_EOI_THREADED),
	BIT_MASK_DESCR(IRQCHIP_SUPPORTS_LEVEL_MSI),
	BIT_MASK_DESCR(IRQCHIP_SUPPORTS_NMI),
	BIT_MASK_DESCR(IRQCHIP_ENABLE_WAKEUP_ON_SUSPEND),
	BIT_MASK_DESCR(IRQCHIP_IMMUTABLE),
	BIT_MASK_DESCR(IRQCHIP_MOVE_DEFERRED),
};

/*
 * 输出一层 irq_data 对应的 irq_chip。无 chip 时打印 None；否则优先调用 chip 自有
 * irq_print_chip()，退化为名字，再输出 flags 原值与已知位。调用者持 desc->lock，
 * @data/@chip 均为借用对象；provider 回调必须遵守该不可睡眠锁上下文。
 */
static void
irq_debug_show_chip(struct seq_file *m, struct irq_data *data, int ind)
{
	struct irq_chip *chip = data->chip;

	if (!chip) {
		seq_printf(m, "chip: None\n");
		return;
	}
	seq_printf(m, "%*schip:    ", ind, "");
	if (chip->irq_print_chip)
		chip->irq_print_chip(data, m);
	else
		seq_printf(m, "%s", chip->name);
	seq_printf(m, "\n%*sflags:   0x%lx\n", ind + 1, "", chip->flags);
	irq_debug_show_bits(m, ind, chip->flags, irqchip_flags,
			    ARRAY_SIZE(irqchip_flags));
}

/*
 * 从 @data 开始输出 domain、hwirq、chip 与 domain 专属 debug 信息；层级 IRQ 构建
 * 中沿 parent_data 递归到根层，每层增加缩进。函数不取得 domain/chip 引用，依赖
 * 调用者持 desc->lock 稳定整条 irq_data 链；无 parent 时结束，非层级构建只输出叶层。
 */
static void
irq_debug_show_data(struct seq_file *m, struct irq_data *data, int ind)
{
	seq_printf(m, "%*sdomain:  %s\n", ind, "",
		   data->domain ? data->domain->name : "");
	seq_printf(m, "%*shwirq:   0x%lx\n", ind + 1, "", data->hwirq);
	irq_debug_show_chip(m, data, ind + 1);
	if (data->domain && data->domain->ops && data->domain->ops->debug_show)
		data->domain->ops->debug_show(m, NULL, data, ind + 1);
#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
	if (!data->parent_data)
		return;
	seq_printf(m, "%*sparent:\n", ind + 1, "");
	irq_debug_show_data(m, data->parent_data, ind + 4);
#endif
}

/* irq_data::common->state_use_accessors 中已知触发类型、运行态和 affinity 策略位。 */
static const struct irq_bit_descr irqdata_states[] = {
	BIT_MASK_DESCR(IRQ_TYPE_EDGE_RISING),
	BIT_MASK_DESCR(IRQ_TYPE_EDGE_FALLING),
	BIT_MASK_DESCR(IRQ_TYPE_LEVEL_HIGH),
	BIT_MASK_DESCR(IRQ_TYPE_LEVEL_LOW),
	BIT_MASK_DESCR(IRQD_LEVEL),

	BIT_MASK_DESCR(IRQD_ACTIVATED),
	BIT_MASK_DESCR(IRQD_IRQ_STARTED),
	BIT_MASK_DESCR(IRQD_IRQ_DISABLED),
	BIT_MASK_DESCR(IRQD_IRQ_MASKED),
	BIT_MASK_DESCR(IRQD_IRQ_INPROGRESS),

	BIT_MASK_DESCR(IRQD_PER_CPU),
	BIT_MASK_DESCR(IRQD_NO_BALANCING),

	BIT_MASK_DESCR(IRQD_SINGLE_TARGET),
	BIT_MASK_DESCR(IRQD_AFFINITY_SET),
	BIT_MASK_DESCR(IRQD_SETAFFINITY_PENDING),
	BIT_MASK_DESCR(IRQD_AFFINITY_MANAGED),
	BIT_MASK_DESCR(IRQD_AFFINITY_ON_ACTIVATE),
	BIT_MASK_DESCR(IRQD_MANAGED_SHUTDOWN),
	BIT_MASK_DESCR(IRQD_CAN_RESERVE),

	BIT_MASK_DESCR(IRQD_FORWARDED_TO_VCPU),

	BIT_MASK_DESCR(IRQD_WAKEUP_STATE),
	BIT_MASK_DESCR(IRQD_WAKEUP_ARMED),

	BIT_MASK_DESCR(IRQD_DEFAULT_TRIGGER_SET),

	BIT_MASK_DESCR(IRQD_HANDLE_ENFORCE_IRQCTX),

	BIT_MASK_DESCR(IRQD_IRQ_ENABLED_ON_SUSPEND),

	BIT_MASK_DESCR(IRQD_RESEND_WHEN_IN_PROGRESS),
};

/* irq_desc::status_use_accessors 中面向核心策略的已知设置位。 */
static const struct irq_bit_descr irqdesc_states[] = {
	BIT_MASK_DESCR(_IRQ_NOPROBE),
	BIT_MASK_DESCR(_IRQ_NOREQUEST),
	BIT_MASK_DESCR(_IRQ_NOTHREAD),
	BIT_MASK_DESCR(_IRQ_NOAUTOEN),
	BIT_MASK_DESCR(_IRQ_NESTED_THREAD),
	BIT_MASK_DESCR(_IRQ_PER_CPU_DEVID),
	BIT_MASK_DESCR(_IRQ_IS_POLLED),
	BIT_MASK_DESCR(_IRQ_DISABLE_UNLAZY),
	BIT_MASK_DESCR(_IRQ_HIDDEN),
};

/* irq_desc::istate 中由流处理、spurious、PM、NMI 等路径维护的内部状态位。 */
static const struct irq_bit_descr irqdesc_istates[] = {
	BIT_MASK_DESCR(IRQS_AUTODETECT),
	BIT_MASK_DESCR(IRQS_SPURIOUS_DISABLED),
	BIT_MASK_DESCR(IRQS_POLL_INPROGRESS),
	BIT_MASK_DESCR(IRQS_ONESHOT),
	BIT_MASK_DESCR(IRQS_REPLAY),
	BIT_MASK_DESCR(IRQS_WAITING),
	BIT_MASK_DESCR(IRQS_PENDING),
	BIT_MASK_DESCR(IRQS_SUSPENDED),
	BIT_MASK_DESCR(IRQS_NMI),
};


/*
 * 生成单个 IRQ debugfs 文件的完整快照。m->private 是创建文件时传入的 irq_desc；
 * 安全 debugfs 代理保证本次 read 不与删除后释放重叠。函数持 desc raw lock，依次
 * 输出 flow handler、设备名、desc 设置/内部状态、disable/wake depth、irq_data
 * 状态、NUMA、affinity 和完整父层级。dev_name 由 MSI 建立期无锁地补入，因而该字段
 * 允许短暂显示旧值/NULL；desc 锁主要稳定中断运行态和层级拓扑。
 *
 * 原始十六进制值保证未知新 bit 仍可观察，描述表只追加可读展开。成功返回 0；底层
 * seq 写入按 seq_file 协议记录溢出，helper 和 provider debug_show 无独立错误返回。
 */
static int irq_debug_show(struct seq_file *m, void *p)
{
	struct irq_desc *desc = m->private;
	struct irq_data *data;

	guard(raw_spinlock_irq)(&desc->lock);
	data = irq_desc_get_irq_data(desc);
	seq_printf(m, "handler:  %ps\n", desc->handle_irq);
	seq_printf(m, "device:   %s\n", desc->dev_name);
	seq_printf(m, "status:   0x%08x\n", desc->status_use_accessors);
	irq_debug_show_bits(m, 0, desc->status_use_accessors, irqdesc_states,
			    ARRAY_SIZE(irqdesc_states));
	seq_printf(m, "istate:   0x%08x\n", desc->istate);
	irq_debug_show_bits(m, 0, desc->istate, irqdesc_istates,
			    ARRAY_SIZE(irqdesc_istates));
	seq_printf(m, "ddepth:   %u\n", desc->depth);
	seq_printf(m, "wdepth:   %u\n", desc->wake_depth);
	seq_printf(m, "dstate:   0x%08x\n", irqd_get(data));
	irq_debug_show_bits(m, 0, irqd_get(data), irqdata_states,
			    ARRAY_SIZE(irqdata_states));
	seq_printf(m, "node:     %d\n", irq_data_get_node(data));
	irq_debug_show_masks(m, desc);
	irq_debug_show_data(m, data, 0);
	return 0;
}

/*
 * 为一次只读打开建立 single_open 上下文，并把 inode 私有 irq_desc 传给 show。
 * debugfs full-fops 代理保护 open；后续 read 也先取得 debugfs active-user 引用，文件
 * 已删除时会在进入真实回调前失败，因此 single_open 中缓存的借用指针不会越过释放。
 */
static int irq_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, irq_debug_show, inode->i_private);
}

/*
 * 处理调试注入写入。最多复制前 7 字节到零初始化栈缓冲；只要用户输入是 "trigger"
 * 的长度受限前缀（或更长输入的前 7 字节等于它），就调用 irq_inject_interrupt()。
 * 注入失败返回其负 errno，成功返回原 count；其他文本也被静默消费并返回 count。
 *
 * 该宽松前缀比较是当前调试接口的真实语义，包含 size==0 时 strncmp(..., 0) 匹配的
 * 边界；注释不把它误述为必须完整命令。copy_from_user() 失败返回 -EFAULT。
 */
static ssize_t irq_debug_write(struct file *file, const char __user *user_buf,
			       size_t count, loff_t *ppos)
{
	struct irq_desc *desc = file_inode(file)->i_private;
	char buf[8] = { 0, };
	size_t size;

	size = min(sizeof(buf) - 1, count);
	if (copy_from_user(buf, user_buf, size))
		return -EFAULT;

	if (!strncmp(buf, "trigger", size)) {
		int err = irq_inject_interrupt(irq_desc_get_irq(desc));

		return err ? err : count;
	}

	return count;
}

/* 单 IRQ 的 full-fops；debugfs 为 open/read/write/llseek 访问提供删除并发保护。 */
static const struct file_operations dfs_irq_ops = {
	.open		= irq_debug_open,
	.write		= irq_debug_write,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/*
 * 为 MSI 等设备 IRQ 复制设备名到描述符，使 debugfs 不借用可能先失效的 device 名字。
 * @irq 必须对应存活 desc，@dev 必须有效；kstrdup() 失败时保持 NULL。成功分配的字符串
 * 由 irq_remove_debugfs_entry() 在文件删除并等待在途操作后释放。
 *
 * 当前函数不持 desc 锁且会直接覆盖字段，调用协议要求每个 desc 在建立期至多调用一次；
 * 与极早 debugfs 读取并发时只提供 best-effort 名称，重复调用还会丢失旧分配的所有权。
 */
void irq_debugfs_copy_devname(int irq, struct device *dev)
{
	struct irq_desc *desc = irq_to_desc(irq);
	const char *name = dev_name(dev);

	if (name)
		desc->dev_name = kstrdup(name, GFP_KERNEL);
}

/*
 * 为 @desc 按十进制 IRQ 号创建可读写调试文件。debugfs 根未初始化、desc 为空或已有
 * dentry/错误哨兵时无操作；创建结果（包括 ERR_PTR）写入 desc->debugfs_file，后续
 * free_desc() 交给 debugfs_remove()，后者可安全接受错误值。
 *
 * 文件私有数据借用 desc，不增加 rcuref；调用者在描述符发布/稀疏索引锁协议内保证
 * 单次创建，debugfs full-fops 的 active-user 屏障保证最终删除后才可释放对象。
 */
void irq_add_debugfs_entry(unsigned int irq, struct irq_desc *desc)
{
	char name [12];

	if (!irq_dir || !desc || desc->debugfs_file)
		return;

	sprintf(name, "%u", irq);
	desc->debugfs_file = debugfs_create_file(name, 0644, irq_dir, desc,
						 &dfs_irq_ops);
}

/*
 * 建立 debugfs IRQ 树：/irq 作为 domain 与单 IRQ 诊断的共同根，/irq/irqs 保存
 * 数字文件。先发布 irq_dir，再持 sparse IRQ 锁遍历当前 active 描述符补建条目；
 * 之后新分配 desc 由 irq_add_debugfs_entry() 增量创建。
 *
 * debugfs/domain/文件创建均为 best-effort，错误指针由 debugfs API 继续安全传播，
 * 本 initcall 始终返回 0，绝不因诊断接口缺失阻止 IRQ 子系统或系统启动。
 */
static int __init irq_debugfs_init(void)
{
	struct dentry *root_dir;
	int irq;

	root_dir = debugfs_create_dir("irq", NULL);

	irq_domain_debugfs_init(root_dir);

	irq_dir = debugfs_create_dir("irqs", root_dir);

	irq_lock_sparse();
	for_each_active_irq(irq)
		irq_add_debugfs_entry(irq, irq_to_desc(irq));
	irq_unlock_sparse();

	return 0;
}
__initcall(irq_debugfs_init);
