/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_IRQ_DEBUGFS_H
#define _KERNEL_IRQ_DEBUGFS_H

/*
 * IRQ 核心 debugfs 的内部接口与配置裁剪层。启用 GENERIC_IRQ_DEBUGFS 时声明真实的
 * 展示/建档入口，并把 irq_desc 的 debugfs 资源释放内联到描述符销毁路径；关闭时以
 * 类型安全的空函数保持调用方无需散布条件编译。此头文件不拥有传入对象。
 */
#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
#include <linux/debugfs.h>

/* 单个可解释状态位：mask 用于匹配，name 借用静态字符串并只供 seq_file 输出。 */
struct irq_bit_descr {
	unsigned int	mask;
	char		*name;
};

/* 从宏参数同时生成位值和同名静态字符串，构造只读的 irq_bit_descr 表项。 */
#define BIT_MASK_DESCR(m)	{ .mask = m, .name = #m }

/*
 * 把 state 中命中的描述表项写入 @m。@m 与 @sd 必须在调用期间有效，@size 是 sd
 * 的表项数，@ind 是缩进基数；函数不保存指针、不修改状态，未知位不会单独输出。
 */
void irq_debug_show_bits(struct seq_file *m, int ind, unsigned int state,
			 const struct irq_bit_descr *sd, int size);

/* 为已发布且存活的 @desc 尽力创建单 IRQ 文件；失败不影响 IRQ 主功能。 */
void irq_add_debugfs_entry(unsigned int irq, struct irq_desc *desc);
/*
 * 销毁 @desc 的 debugfs 附属资源。调用者保证 desc 非 NULL、不会重复释放，并已阻止
 * 新的描述符用户；必须先 debugfs_remove() 等待安全文件操作退出，再释放 show 路径
 * 可能读取的 dev_name。两字段随 desc 生命周期终止，无需在这里清空。
 */
static inline void irq_remove_debugfs_entry(struct irq_desc *desc)
{
	debugfs_remove(desc->debugfs_file);
	kfree(desc->dev_name);
}
/* 为 IRQ 复制 @dev 的当前名称；只在描述符存活期调用，分配失败时名称为 NULL。 */
void irq_debugfs_copy_devname(int irq, struct device *dev);
# ifdef CONFIG_IRQ_DOMAIN
/* 在 @root 下建立 domain 视图，并补建初始化前已发布的 domain；仅初始化期调用。 */
void irq_domain_debugfs_init(struct dentry *root);
# else
/* 未构建 IRQ domain 时保留同名初始化接口；root 会求值但不被保存或访问。 */
static inline void irq_domain_debugfs_init(struct dentry *root)
{
}
# endif
#else /* CONFIG_GENERIC_IRQ_DEBUGFS */
/*
 * debugfs 整体关闭时，这些 inline stub 仍提供编译期类型检查；实参按普通 C 函数调用
 * 规则求值，但函数不读取对象、不取得 ownership，也不产生文件或分配。
 */
/* 空建档入口：接受任意 IRQ/desc 值但不解引用，调用后没有可释放的 debugfs 状态。 */
static inline void irq_add_debugfs_entry(unsigned int irq, struct irq_desc *d)
{
}
/* 空撤档入口：配置关闭时 desc 从未由本接口附加 debugfs 资源，因此无需清理。 */
static inline void irq_remove_debugfs_entry(struct irq_desc *d)
{
}
/* 空名称复制入口：不读取 dev、不查找 irq_desc，也不会进行内存分配。 */
static inline void irq_debugfs_copy_devname(int irq, struct device *dev)
{
}
#endif /* CONFIG_GENERIC_IRQ_DEBUGFS */

#endif
