// SPDX-License-Identifier: GPL-2.0
/*
 * Library implementing the most common irq chip callback functions
 *
 * Copyright (C) 2011, Thomas Gleixner
 */
/*
 * 原文说明：本文件是一组可复用的通用 irq_chip 实现，面向最多 32 条线路的简单寄存器
 * 控制器。驱动通过 irq_chip_generic 保存共享寄存器基址、锁、mask/wake 缓存和一个或多个
 * irq_chip_type；每个 type 可为 level/edge 等 flow 使用不同回调、寄存器偏移和 handler。
 *
 * 两种建立模式：非 domain 驱动先 alloc/init，再 irq_setup_generic_chip() 绑定一段 Linux
 * IRQ；irqdomain 驱动一次为 revmap 空间分配若干 gc，由 map/unmap 按 hwirq 选择实例。
 * 所有已建立 gc 还进入 gc_list，syscore suspend/resume/shutdown 逐芯片调用 PM 回调。
 * gc->lock 保护寄存器与缓存；gc_lock 只保护全局实例链，不能混作硬件寄存器锁。
 */
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/syscore_ops.h>

#include "internals.h"

/* 所有已 setup 的 generic chip 全局链，供 syscore PM 阶段遍历；节点嵌在 gc 中。 */
static LIST_HEAD(gc_list);
/* 保护 gc_list 的加入/移除；PM 遍历依赖 syscore/设备生命周期已停止并发修改。 */
static DEFINE_RAW_SPINLOCK(gc_lock);

/**
 * irq_gc_noop - NOOP function
 * @d: irq_data
 */
/*
 * 空操作 irqchip 回调。@d 为未消费的 irq_data 借用指针；无返回值、无寄存器
 * 访问，供硬件不需要某阶段操作但接口要求函数指针的场景使用。
 */
void irq_gc_noop(struct irq_data *d)
{
}
EXPORT_SYMBOL_GPL(irq_gc_noop);

/**
 * irq_gc_mask_disable_reg - Mask chip via disable register
 * @d: irq_data
 *
 * Chip has separate enable/disable registers instead of a single mask
 * register.
 */
/*
 * 控制器使用独立 enable/disable 写寄存器时，mask @d 对应线路。由 irq_data
 * 取得所属 gc/type 和单线位 mask，在 gc->lock 下把该位写入 disable 偏移，并从 type 当前
 * mask_cache 清位，使缓存镜像这种硬件的“enabled 位集合”。无返回值，可在 hardirq 下调用。
 */
void irq_gc_mask_disable_reg(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	u32 mask = d->mask;

	guard(raw_spinlock)(&gc->lock);
	irq_reg_writel(gc, mask, ct->regs.disable);
	*ct->mask_cache &= ~mask;
}
EXPORT_SYMBOL_GPL(irq_gc_mask_disable_reg);

/**
 * irq_gc_mask_set_bit - Mask chip via setting bit in mask register
 * @d: irq_data
 *
 * Chip has a single mask register. Values of this register are cached
 * and protected by gc->lock
 */
/*
 * 单一 mask 寄存器采用“置 1 屏蔽”语义。锁内把 @d 位 OR 进缓存，再把完整
 * 缓存写回 type->regs.mask，避免并发线路更新互相覆盖。缓存指针可由所有 type 共享，也可
 * 按 IRQ_GC_MASK_CACHE_PER_TYPE 私有。无返回值。
 */
void irq_gc_mask_set_bit(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	u32 mask = d->mask;

	guard(raw_spinlock)(&gc->lock);
	*ct->mask_cache |= mask;
	irq_reg_writel(gc, *ct->mask_cache, ct->regs.mask);
}
EXPORT_SYMBOL_GPL(irq_gc_mask_set_bit);

/**
 * irq_gc_mask_clr_bit - Mask chip via clearing bit in mask register
 * @d: irq_data
 *
 * Chip has a single mask register. Values of this register are cached
 * and protected by gc->lock
 */
/*
 * 单一 mask 寄存器采用“清 0 屏蔽”语义。锁内从缓存清除 @d 位，再整值写回
 * mask 寄存器。函数名描述对寄存器位的操作，不代表 cache 在所有控制器上具有统一极性。
 */
void irq_gc_mask_clr_bit(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	u32 mask = d->mask;

	guard(raw_spinlock)(&gc->lock);
	*ct->mask_cache &= ~mask;
	irq_reg_writel(gc, *ct->mask_cache, ct->regs.mask);
}
EXPORT_SYMBOL_GPL(irq_gc_mask_clr_bit);

/**
 * irq_gc_unmask_enable_reg - Unmask chip via enable register
 * @d: irq_data
 *
 * Chip has separate enable/disable registers instead of a single mask
 * register.
 */
/*
 * 使用独立 enable/disable 寄存器时 unmask 线路。锁内把 @d 位写入 enable
 * 寄存器，并在缓存中置位，缓存因而表示 enabled 位集合；与 irq_gc_mask_disable_reg()
 * 对称。无返回值。
 */
void irq_gc_unmask_enable_reg(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	u32 mask = d->mask;

	guard(raw_spinlock)(&gc->lock);
	irq_reg_writel(gc, mask, ct->regs.enable);
	*ct->mask_cache |= mask;
}
EXPORT_SYMBOL_GPL(irq_gc_unmask_enable_reg);

/**
 * irq_gc_ack_set_bit - Ack pending interrupt via setting bit
 * @d: irq_data
 */
/*
 * 对 write-1-to-ack 控制器确认 @d 的 pending 事件。锁内只把该线 mask 写到
 * type->regs.ack，不修改 mask_cache；无返回值，具体 ack 副作用由硬件定义。
 */
void irq_gc_ack_set_bit(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	u32 mask = d->mask;

	guard(raw_spinlock)(&gc->lock);
	irq_reg_writel(gc, mask, ct->regs.ack);
}
EXPORT_SYMBOL_GPL(irq_gc_ack_set_bit);

/**
 * irq_gc_ack_clr_bit - Ack pending interrupt via clearing bit
 * @d: irq_data
 */
/*
 * 对“清目标位完成 ack”的寄存器写入按位取反的 @d->mask，即目标位为 0、其他
 * 位为 1。gc->lock 串行寄存器访问；不修改缓存、无返回值。驱动只有在硬件确属该写入语义
 * 时才能选用此 helper。
 */
void irq_gc_ack_clr_bit(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	u32 mask = ~d->mask;

	guard(raw_spinlock)(&gc->lock);
	irq_reg_writel(gc, mask, ct->regs.ack);
}

/**
 * irq_gc_mask_disable_and_ack_set - Mask and ack pending interrupt
 * @d: irq_data
 *
 * This generic implementation of the irq_mask_ack method is for chips
 * with separate enable/disable registers instead of a single mask
 * register and where a pending interrupt is acknowledged by setting a
 * bit.
 *
 * Note: This is the only permutation currently used.  Similar generic
 * functions should be added here if other permutations are required.
 */
/*
 * 实现 irq_mask_ack 的一种通用组合，适用于独立 enable/disable 寄存器且 pending
 * 通过写 1 ack 的控制器；目前仅此排列被使用，其他寄存器极性组合应新增明确 helper。
 * 函数在一次 gc 锁临界区先写 disable、清“enabled”缓存位，再写 ack，避免 mask 与确认
 * 被其他线路寄存器更新穿插。@d 是存活 irq_data；无返回值。
 */
void irq_gc_mask_disable_and_ack_set(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	u32 mask = d->mask;

	guard(raw_spinlock)(&gc->lock);
	irq_reg_writel(gc, mask, ct->regs.disable);
	*ct->mask_cache &= ~mask;
	irq_reg_writel(gc, mask, ct->regs.ack);
}
EXPORT_SYMBOL_GPL(irq_gc_mask_disable_and_ack_set);

/**
 * irq_gc_eoi - EOI interrupt
 * @d: irq_data
 */
/*
 * 向当前 chip type 的 EOI 偏移写入 @d->mask，结束该线路的控制器服务状态。
 * gc->lock 保护共享寄存器访问；不修改缓存、无返回值。
 */
void irq_gc_eoi(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	u32 mask = d->mask;

	guard(raw_spinlock)(&gc->lock);
	irq_reg_writel(gc, mask, ct->regs.eoi);
}

/**
 * irq_gc_set_wake - Set/clr wake bit for an interrupt
 * @d:  irq_data
 * @on: Indicates whether the wake bit should be set or cleared
 *
 * For chips where the wake from suspend functionality is not
 * configured in a separate register and the wakeup active state is
 * just stored in a bitmask.
 */
/*
 * 用于没有独立 wake 配置寄存器、只需由软件位图记住 suspend 唤醒源的芯片。
 * @d 指定线路，@on 非零启用、零禁用。若该位不在 gc->wake_enabled 能力集合中返回
 * -EINVAL；否则锁内更新 wake_active 并返回 0。函数不访问硬件，后续 gc suspend/resume
 * 回调读取 wake_active 统一编程；wake_enabled 通常在发布前静态配置。
 */
int irq_gc_set_wake(struct irq_data *d, unsigned int on)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	u32 mask = d->mask;

	if (!(mask & gc->wake_enabled))
		return -EINVAL;

	guard(raw_spinlock)(&gc->lock);
	if (on)
		gc->wake_active |= mask;
	else
		gc->wake_active &= ~mask;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_gc_set_wake);

/*
 * irq_readl_be() - generic chip 的 32 位大端寄存器读取适配器
 *
 * @addr: 有效 __iomem 地址。返回 ioread32be() 转换后的 CPU 端 u32；供 gc->reg_readl 使用。
 */
static u32 irq_readl_be(void __iomem *addr)
{
	return ioread32be(addr);
}

/*
 * irq_writel_be() - generic chip 的 32 位大端寄存器写适配器
 *
 * @val: CPU 端数值；@addr: 有效 __iomem 地址。调用 iowrite32be()，无返回值。
 */
static void irq_writel_be(u32 val, void __iomem *addr)
{
	iowrite32be(val, addr);
}

/*
 * irq_init_generic_chip() - 初始化调用者已分配的 generic chip 公共字段
 *
 * @gc: 至少带 @num_ct 个 chip_types 尾数组的零初始化/未发布对象。
 * @name: 各 irq_chip_type 的诊断名称借用字符串。
 * @num_ct: type 数量；必须大于 0 且不超过实际尾数组容量。
 * @irq_base: 非 domain 模式为 Linux IRQ 基号，domain 模式通常为该 gc 的 hwirq 基号。
 * @reg_base: 可空 MMIO 虚拟基址；驱动也可稍后在 init 回调填写。
 * @handler: 首要 type（索引 0）的默认 flow handler。
 * 初始化 gc raw lock、公共字段和所有 type 的 chip.name，仅设置 type0 handler；驱动须在
 * 发布前补齐寄存器偏移、回调及其他 type handler/type。无分配、无返回值、无需锁。
 */
void irq_init_generic_chip(struct irq_chip_generic *gc, const char *name,
			   int num_ct, unsigned int irq_base,
			   void __iomem *reg_base, irq_flow_handler_t handler)
{
	struct irq_chip_type *ct = gc->chip_types;
	int i;

	raw_spin_lock_init(&gc->lock);
	gc->num_ct = num_ct;
	gc->irq_base = irq_base;
	gc->reg_base = reg_base;
	for (i = 0; i < num_ct; i++)
		ct[i].chip.name = name;
	gc->chip_types->handler = handler;
}

/**
 * irq_alloc_generic_chip - Allocate a generic chip and initialize it
 * @name:	Name of the irq chip
 * @num_ct:	Number of irq_chip_type instances associated with this
 * @irq_base:	Interrupt base nr for this chip
 * @reg_base:	Register base address (virtual)
 * @handler:	Default flow handler associated with this chip
 *
 * Returns an initialized irq_chip_generic structure. The chip defaults
 * to the primary (index 0) irq_chip_type and @handler
 */
/*
 * 分配并初始化一个 generic chip，默认使用索引 0 的 irq_chip_type 与 @handler。
 * 参数语义同 irq_init_generic_chip()；内存由 kzalloc_flex 一次容纳 gc 和 @num_ct 个 type。
 * 成功返回由调用者拥有的零初始化对象，最终用 irq_free_generic_chip()/kfree 释放；失败
 * 返回 NULL。函数可睡眠，尚未把 chip 安装到 IRQ 或全局 PM 链。
 */
struct irq_chip_generic *
irq_alloc_generic_chip(const char *name, int num_ct, unsigned int irq_base,
		       void __iomem *reg_base, irq_flow_handler_t handler)
{
	struct irq_chip_generic *gc;

	gc = kzalloc_flex(*gc, chip_types, num_ct);
	if (gc) {
		irq_init_generic_chip(gc, name, num_ct, irq_base, reg_base,
				      handler);
	}
	return gc;
}
EXPORT_SYMBOL_GPL(irq_alloc_generic_chip);

/*
 * irq_gc_init_mask_cache() - 为每个 chip type 选择并可选初始化 mask 缓存
 *
 * @gc: 已填好 num_ct、type regs 和可访问寄存器基址的 generic chip。
 * @flags: IRQ_GC_MASK_CACHE_PER_TYPE 决定每个 type 指向自己的 mask_cache_priv，否则全部
 * 指向 gc->mask_cache；IRQ_GC_INIT_MASK_CACHE 再从相应 regs.mask 读取硬件初值。共享缓存
 * 模式下 mskreg 保持 type0 偏移，只读入同一值；调用者在尚未并发使用或持 gc 锁时调用。
 * 无返回值，读取失败无法报告，寄存器访问器/偏移必须由驱动预先保证有效。
 */
static void
irq_gc_init_mask_cache(struct irq_chip_generic *gc, enum irq_gc_flags flags)
{
	struct irq_chip_type *ct = gc->chip_types;
	u32 *mskptr = &gc->mask_cache, mskreg = ct->regs.mask;
	int i;

	for (i = 0; i < gc->num_ct; i++) {
		if (flags & IRQ_GC_MASK_CACHE_PER_TYPE) {
			mskptr = &ct[i].mask_cache_priv;
			mskreg = ct[i].regs.mask;
		}
		ct[i].mask_cache = mskptr;
		if (flags & IRQ_GC_INIT_MASK_CACHE)
			*mskptr = irq_reg_readl(gc, mskreg);
	}
}

/**
 * irq_domain_alloc_generic_chips - Allocate generic chips for an irq domain
 * @d:		irq domain for which to allocate chips
 * @info:	Generic chip information
 *
 * Return: 0 on success, negative error code on failure
 */
/*
 * 按 @d->revmap_size 为 irqdomain 一次分配并建立全部 generic chip 容器。
 * @info 描述每 chip 线路数、type 数、名称、handler、
 * status 修改位、I/O 字节序以及可选 per-chip init/exit。domain 已有 d->gc 返回 -EBUSY；
 * 计算不到 chip 返回 -EINVAL；分配失败 -ENOMEM；init 失败原样返回。
 * 本函数没有防御除零和零长度 type，调用者必须保证 irqs_per_chip 为 1..32、num_ct>0，
 * 且尺寸加法/乘法在 size_t 范围内；这些是 generic-chip API 的建立期前置契约。
 *
 * 内存布局是一个 dgc flexible pointer array 后紧跟 numchips 个等长 gc+chip_types 块，整个
 * 生命周期只需最终 kfree(dgc)。函数先发布 d->gc，逐块 init：irq_base 使用该块 hwirq
 * 基号，关联 domain，大端标志安装访问器，成功 init 后加入 gc_list。中途失败只对已经
 * 完成 init/入链的前序 chip 逆序调用 exit+remove，清 d->gc 并释放整块；失败的当前 chip
 * init 若已取得部分私有资源，须由 init 自身在返回错误前回滚。
 */
int irq_domain_alloc_generic_chips(struct irq_domain *d,
				   const struct irq_domain_chip_generic_info *info)
{
	struct irq_domain_chip_generic *dgc;
	struct irq_chip_generic *gc;
	int numchips, i;
	size_t dgc_sz;
	size_t gc_sz;
	size_t sz;
	void *tmp;
	int ret;

	if (d->gc)
		return -EBUSY;

	numchips = DIV_ROUND_UP(d->revmap_size, info->irqs_per_chip);
	if (!numchips)
		return -EINVAL;

	/* Allocate a pointer, generic chip and chiptypes for each chip */
	/* 精确计算 dgc 指针尾数组与每个 gc type 尾数组大小，再做一次连续分配。 */
	gc_sz = struct_size(gc, chip_types, info->num_ct);
	dgc_sz = struct_size(dgc, gc, numchips);
	sz = dgc_sz + numchips * gc_sz;

	tmp = dgc = kzalloc(sz, GFP_KERNEL);
	if (!dgc)
		return -ENOMEM;
	dgc->irqs_per_chip = info->irqs_per_chip;
	dgc->num_chips = numchips;
	dgc->irq_flags_to_set = info->irq_flags_to_set;
	dgc->irq_flags_to_clear = info->irq_flags_to_clear;
	dgc->gc_flags = info->gc_flags;
	dgc->exit = info->exit;
	d->gc = dgc;

	/* Calc pointer to the first generic chip */
	/* 越过 dgc 头及 gc 指针数组，定位连续区域中的第一个 generic chip。 */
	tmp += dgc_sz;
	for (i = 0; i < numchips; i++) {
		/* Store the pointer to the generic chip */
		/* 把当前连续块地址写入 dgc->gc[i]，再原地初始化对象。 */
		dgc->gc[i] = gc = tmp;
		irq_init_generic_chip(gc, info->name, info->num_ct,
				      i * dgc->irqs_per_chip, NULL,
				      info->handler);

		gc->domain = d;
		if (dgc->gc_flags & IRQ_GC_BE_IO) {
			gc->reg_readl = &irq_readl_be;
			gc->reg_writel = &irq_writel_be;
		}

		if (info->init) {
			ret = info->init(gc);
			if (ret)
				goto err;
		}

		scoped_guard (raw_spinlock_irqsave, &gc_lock)
			list_add_tail(&gc->list, &gc_list);
		/* Calc pointer to the next generic chip */
		/* 按 gc+num_ct 尾数组的固定大小推进到下一对象。 */
		tmp += gc_sz;
	}
	return 0;

err:
	while (i--) {
		if (dgc->exit)
			dgc->exit(dgc->gc[i]);
		irq_remove_generic_chip(dgc->gc[i], ~0U, 0, 0);
	}
	d->gc = NULL;
	kfree(dgc);
	return ret;
}
EXPORT_SYMBOL_GPL(irq_domain_alloc_generic_chips);

/**
 * irq_domain_remove_generic_chips - Remove generic chips from an irq domain
 * @d: irq domain for which generic chips are to be removed
 */
/*
 * 移除 @d 中由 irq_domain_alloc_generic_chips() 建立的全部 generic chip。
 * d->gc 为空时无操作；否则按创建顺序对每个实例先调用可选 exit，再以全位 mask 调
 * irq_remove_generic_chip()，从 gc_list 摘除并解除已映射 IRQ 的 handler/chip/data。最后
 * 清 d->gc 并一次释放连续内存。调用者必须已阻止新 map、IRQ 执行和 syscore PM 遍历；
 * 无返回值，exit 必须自行完成私有资源清理。
 */
void irq_domain_remove_generic_chips(struct irq_domain *d)
{
	struct irq_domain_chip_generic *dgc = d->gc;
	unsigned int i;

	if (!dgc)
		return;

	for (i = 0; i < dgc->num_chips; i++) {
		if (dgc->exit)
			dgc->exit(dgc->gc[i]);
		irq_remove_generic_chip(dgc->gc[i], ~0U, 0, 0);
	}
	d->gc = NULL;
	kfree(dgc);
}
EXPORT_SYMBOL_GPL(irq_domain_remove_generic_chips);

/**
 * __irq_alloc_domain_generic_chips - Allocate generic chips for an irq domain
 * @d:			irq domain for which to allocate chips
 * @irqs_per_chip:	Number of interrupts each chip handles (max 32)
 * @num_ct:		Number of irq_chip_type instances associated with this
 * @name:		Name of the irq chip
 * @handler:		Default flow handler associated with these chips
 * @clr:		IRQ_* bits to clear in the mapping function
 * @set:		IRQ_* bits to set in the mapping function
 * @gcflags:		Generic chip specific setup flags
 */
/*
 * 兼容旧接口，把显式参数封装成 irq_domain_chip_generic_info，再调用新的 domain
 * 批量分配函数。@irqs_per_chip 是每个 gc 的 hwirq 数（调用者必须保证 1..32），@num_ct
 * 是 type 数；@clr/@set 在 map 时传给 irq_modify_status；@gcflags 控制缓存、嵌套锁和 I/O。
 * 返回底层 0 或负 errno，不提供 per-chip init/exit。
 */
int __irq_alloc_domain_generic_chips(struct irq_domain *d, int irqs_per_chip,
				     int num_ct, const char *name,
				     irq_flow_handler_t handler,
				     unsigned int clr, unsigned int set,
				     enum irq_gc_flags gcflags)
{
	struct irq_domain_chip_generic_info info = {
		.irqs_per_chip		= irqs_per_chip,
		.num_ct			= num_ct,
		.name			= name,
		.handler		= handler,
		.irq_flags_to_clear	= clr,
		.irq_flags_to_set	= set,
		.gc_flags		= gcflags,
	};

	return irq_domain_alloc_generic_chips(d, &info);
}
EXPORT_SYMBOL_GPL(__irq_alloc_domain_generic_chips);

/*
 * __irq_get_domain_generic_chip() - 按 domain hwirq 查找所属 generic chip
 *
 * @d: 目标 irqdomain；@hw_irq: domain 局部硬件号。
 * d->gc 不存在返回 ERR_PTR(-ENODEV)；用 hw_irq/irqs_per_chip 得到块索引，越界返回
 * ERR_PTR(-EINVAL)；成功返回连续分配内的 gc 借用指针。函数不验证该具体线是否 unused/
 * installed，不加锁，调用者保证 domain/gc 生命周期稳定。
 */
static struct irq_chip_generic *
__irq_get_domain_generic_chip(struct irq_domain *d, unsigned int hw_irq)
{
	struct irq_domain_chip_generic *dgc = d->gc;
	int idx;

	if (!dgc)
		return ERR_PTR(-ENODEV);
	idx = hw_irq / dgc->irqs_per_chip;
	if (idx >= dgc->num_chips)
		return ERR_PTR(-EINVAL);
	return dgc->gc[idx];
}

/**
 * irq_get_domain_generic_chip - Get a pointer to the generic chip of a hw_irq
 * @d:			irq domain pointer
 * @hw_irq:		Hardware interrupt number
 */
/*
 * 公开查询 @hw_irq 所属 generic chip。内部错误指针统一转换为 NULL，成功返回
 * 由 domain 拥有的借用指针；调用者不得释放，且必须在 domain remove 前使用完。
 */
struct irq_chip_generic *
irq_get_domain_generic_chip(struct irq_domain *d, unsigned int hw_irq)
{
	struct irq_chip_generic *gc = __irq_get_domain_generic_chip(d, hw_irq);

	return !IS_ERR(gc) ? gc : NULL;
}
EXPORT_SYMBOL_GPL(irq_get_domain_generic_chip);

/*
 * Separate lockdep classes for interrupt chip which can nest irq_desc
 * lock and request mutex.
 */
/*
 * 为需要在子 irq_desc 锁内调用父 IRQ 管理接口的控制器准备独立 lockdep class。
 * 常见于 GPIO irqchip 的 irq_set_wake 向父级传播；分别标记 desc raw lock 和 request_mutex，
 * 让 lockdep 理解合法嵌套，而不是关闭依赖检查。key 仅作静态身份标记。
 */
static struct lock_class_key irq_nested_lock_class;
static struct lock_class_key irq_nested_request_class;

/*
 * irq_map_generic_chip - Map a generic chip for an irq domain
 */
/*
 * 把 @d 中的 @hw_irq 映射到已分配 Linux @virq，并安装对应 generic chip。
 * 先按除法定位 gc，再以余数 idx 作为 installed/unused 位及默认 data->mask。unused 返回
 * -ENOTSUPP，重复 installed 返回 -EBUSY。该 gc 第一次映射时在 gc 锁内选择/读取 mask
 * cache；随后置 installed，按标志设置嵌套 lockdep class，调用 type0 irq_calc_mask 或默认
 * `1 << idx`，最后 irq_domain_set_info() 发布 chip/gc/handler，并修改 desc status。
 * 成功返回 0；调用者/domain core 串行 map/unmap。默认位移要求 irqs_per_chip<=32。
 */
int irq_map_generic_chip(struct irq_domain *d, unsigned int virq,
			 irq_hw_number_t hw_irq)
{
	struct irq_data *data = irq_domain_get_irq_data(d, virq);
	struct irq_domain_chip_generic *dgc = d->gc;
	struct irq_chip_generic *gc;
	struct irq_chip_type *ct;
	struct irq_chip *chip;
	int idx;

	gc = __irq_get_domain_generic_chip(d, hw_irq);
	if (IS_ERR(gc))
		return PTR_ERR(gc);

	idx = hw_irq % dgc->irqs_per_chip;

	if (test_bit(idx, &gc->unused))
		return -ENOTSUPP;

	if (test_bit(idx, &gc->installed))
		return -EBUSY;

	ct = gc->chip_types;
	chip = &ct->chip;

	/* We only init the cache for the first mapping of a generic chip */
	/* installed 位图仍为 0 时才初始化共享/type 私有缓存，保留后续运行时状态。 */
	if (!gc->installed) {
		guard(raw_spinlock_irqsave)(&gc->lock);
		irq_gc_init_mask_cache(gc, dgc->gc_flags);
	}

	/* Mark the interrupt as installed */
	/* 发布该相对线路已占用；后续同 hwirq map 会被拒绝。 */
	set_bit(idx, &gc->installed);

	if (dgc->gc_flags & IRQ_GC_INIT_NESTED_LOCK)
		irq_set_lockdep_class(virq, &irq_nested_lock_class,
				      &irq_nested_request_class);

	if (chip->irq_calc_mask)
		chip->irq_calc_mask(data);
	else
		data->mask = 1 << idx;

	irq_domain_set_info(d, virq, hw_irq, chip, gc, ct->handler, NULL, NULL);
	irq_modify_status(virq, dgc->irq_flags_to_clear, dgc->irq_flags_to_set);
	return 0;
}

/*
 * irq_unmap_generic_chip() - 撤销一个 domain virq 的 generic-chip 绑定
 *
 * @d/@virq: 已映射的 domain 与 Linux IRQ。读取当前 irq_data->hwirq，定位 gc；找不到时
 * 防御性返回。清除相对 installed 位，再用 no_irq_chip 和空 handler/data 重置 domain
 * info。无返回值；不重置 mask_cache、不调用 gc exit，也不释放 gc，后续可重新 map。
 * 调用者由 irqdomain core 保证无并发 handler 使用正在解绑的信息。
 */
void irq_unmap_generic_chip(struct irq_domain *d, unsigned int virq)
{
	struct irq_data *data = irq_domain_get_irq_data(d, virq);
	struct irq_domain_chip_generic *dgc = d->gc;
	unsigned int hw_irq = data->hwirq;
	struct irq_chip_generic *gc;
	int irq_idx;

	gc = irq_get_domain_generic_chip(d, hw_irq);
	if (!gc)
		return;

	irq_idx = hw_irq % dgc->irqs_per_chip;

	clear_bit(irq_idx, &gc->installed);
	irq_domain_set_info(d, virq, hw_irq, &no_irq_chip, NULL, NULL, NULL,
			    NULL);

}

/*
 * irq_generic_chip_ops 是可直接交给 linear/tree domain 的标准操作表：map/unmap 使用上述
 * generic-chip 生命周期，xlate 接受 firmware one-cell/two-cell 中断描述。对象静态只读。
 */
const struct irq_domain_ops irq_generic_chip_ops = {
	.map	= irq_map_generic_chip,
	.unmap  = irq_unmap_generic_chip,
	.xlate	= irq_domain_xlate_onetwocell,
};
EXPORT_SYMBOL_GPL(irq_generic_chip_ops);

/**
 * irq_setup_generic_chip - Setup a range of interrupts with a generic chip
 * @gc:		Generic irq chip holding all data
 * @msk:	Bitmask holding the irqs to initialize relative to gc->irq_base
 * @flags:	Flags for initialization
 * @clr:	IRQ_* bits to clear
 * @set:	IRQ_* bits to set
 *
 * Set up max. 32 interrupts starting from gc->irq_base. Note, this
 * initializes all interrupts to the primary irq_chip_type and its
 * associated handler.
 */
/*
 * 从 gc->irq_base 起，按 @msk 最多建立 32 个非 domain Linux IRQ；所有选中线
 * 初始绑定 type0 chip 与 handler。@flags 控制嵌套 lockdep、mask cache 初始化及是否跳过
 * data->mask；@clr/@set 修改每条 desc 的 IRQ_* status。
 *
 * 函数先把 gc 加入 syscore 全局链并初始化 cache，再逐个置位：可选设 lockdep class，调用
 * irq_calc_mask 或按相对位计算 mask，发布 chip/handler 和 chip_data，最后记录扫描到的
 * irq_cnt。无错误返回/回滚，调用前必须确保 IRQ 号有效且未被占用，gc/type/寄存器已配置；
 * 生命周期结束用同范围 irq_remove_generic_chip()。调用可触及 MMIO，配置阶段执行。
 */
void irq_setup_generic_chip(struct irq_chip_generic *gc, u32 msk,
			    enum irq_gc_flags flags, unsigned int clr,
			    unsigned int set)
{
	struct irq_chip_type *ct = gc->chip_types;
	struct irq_chip *chip = &ct->chip;
	unsigned int i;

	scoped_guard (raw_spinlock, &gc_lock)
		list_add_tail(&gc->list, &gc_list);

	irq_gc_init_mask_cache(gc, flags);

	for (i = gc->irq_base; msk; msk >>= 1, i++) {
		if (!(msk & 0x01))
			continue;

		if (flags & IRQ_GC_INIT_NESTED_LOCK)
			irq_set_lockdep_class(i, &irq_nested_lock_class,
					      &irq_nested_request_class);

		if (!(flags & IRQ_GC_NO_MASK)) {
			struct irq_data *d = irq_get_irq_data(i);

			if (chip->irq_calc_mask)
				chip->irq_calc_mask(d);
			else
				d->mask = 1 << (i - gc->irq_base);
		}
		irq_set_chip_and_handler(i, chip, ct->handler);
		irq_set_chip_data(i, gc);
		irq_modify_status(i, clr, set);
	}
	gc->irq_cnt = i - gc->irq_base;
}
EXPORT_SYMBOL_GPL(irq_setup_generic_chip);

/**
 * irq_setup_alt_chip - Switch to alternative chip
 * @d:		irq_data for this interrupt
 * @type:	Flow type to be initialized
 *
 * Only to be called from chip->irq_set_type() callbacks.
 */
/*
 * 仅供 chip->irq_set_type() 回调在触发 flow 改变时切换同一 gc 的备选 chip type。
 * @d: 当前线路 irq_data；@type: 请求的 IRQ_TYPE_* 位。
 * 顺序扫描 chip_types，首个 ct->type 与 @type 有交集时，同时把 d->chip 改为该 type 的
 * irq_chip，并把所属 desc->handle_irq 改为其 flow handler，返回 0；无匹配返回 -EINVAL，
 * 状态不变。调用者持 desc 锁并负责实际硬件 type 寄存器编程；gc/type 生命周期稳定。
 */
int irq_setup_alt_chip(struct irq_data *d, unsigned int type)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = gc->chip_types;
	unsigned int i;

	for (i = 0; i < gc->num_ct; i++, ct++) {
		if (ct->type & type) {
			d->chip = &ct->chip;
			irq_data_to_desc(d)->handle_irq = ct->handler;
			return 0;
		}
	}
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(irq_setup_alt_chip);

/**
 * irq_remove_generic_chip - Remove a chip
 * @gc:		Generic irq chip holding all data
 * @msk:	Bitmask holding the irqs to initialize relative to gc->irq_base
 * @clr:	IRQ_* bits to clear
 * @set:	IRQ_* bits to set
 *
 * Remove up to 32 interrupts starting from gc->irq_base.
 */
/*
 * 解除 @gc 从 irq_base 起由 @msk 选中的最多 32 条线路，并从 syscore gc_list
 * 摘除整个 chip。非 domain 模式 irq_base 是 Linux IRQ 基号；domain 模式它是 hwirq 基号，
 * 每位先 irq_find_mapping() 转成 virq，未映射项跳过。对命中项先 irq_set_handler(NULL)
 * （该核心路径会 mask 线路），再换成 no_irq_chip、清 chip_data，并按 @clr/@set 修改状态。
 *
 * 无返回值，不释放 gc 内存、不调用 domain unmap，也不清 installed 位；调用者必须在停止
 * IRQ/映射及 PM 遍历后使用，并负责随后的 exit/内存释放。@msk 应与 setup 范围一致。
 */
void irq_remove_generic_chip(struct irq_chip_generic *gc, u32 msk,
			     unsigned int clr, unsigned int set)
{
	unsigned int i, virq;

	scoped_guard (raw_spinlock, &gc_lock)
		list_del(&gc->list);

	for (i = 0; msk; msk >>= 1, i++) {
		if (!(msk & 0x01))
			continue;

		/*
		 * Interrupt domain based chips store the base hardware
		 * interrupt number in gc::irq_base. Otherwise gc::irq_base
		 * contains the base Linux interrupt number.
		 */
		/*
		 * domain gc 的 irq_base 保存硬件号基值，需查反向映射；传统 gc 则直接保存
		 * Linux IRQ 基值，可简单相加得到 virq。
		 */
		if (gc->domain) {
			virq = irq_find_mapping(gc->domain, gc->irq_base + i);
			if (!virq)
				continue;
		} else {
			virq = gc->irq_base + i;
		}

		/* Remove handler first. That will mask the irq line */
		/* 先移除 flow handler，核心会在换出过程中 mask 线路，再清 chip/data。 */
		irq_set_handler(virq, NULL);
		irq_set_chip(virq, &no_irq_chip);
		irq_set_chip_data(virq, NULL);
		irq_modify_status(virq, clr, set);
	}
}
EXPORT_SYMBOL_GPL(irq_remove_generic_chip);

/*
 * irq_gc_get_irq_data() - 为一次 per-chip PM 回调选择代表性 irq_data
 *
 * @gc: 已建立且生命周期稳定的 generic chip。
 * 非 domain 模式直接取 irq_base 对应 data。domain 模式不知道哪些 hwirq 真正安装，若
 * installed 为空返回 NULL；否则找最低置位对应的 hwirq 映射，再返回其 irq_data，映射
 * 缺失也返回 NULL。返回值只是借用，回调应把它作为进入 chip 的代表，不得假定它代表
 * 所有线路的独立状态；函数不加锁，PM 阶段要求映射稳定。
 */
static struct irq_data *irq_gc_get_irq_data(struct irq_chip_generic *gc)
{
	unsigned int virq;

	if (!gc->domain)
		return irq_get_irq_data(gc->irq_base);

	/*
	 * We don't know which of the irqs has been actually
	 * installed. Use the first one.
	 */
	/* 无法预知 domain 中哪些线路已 map，选择 installed 位图中的第一条作代表。 */
	if (!gc->installed)
		return NULL;

	virq = irq_find_mapping(gc->domain, gc->irq_base + __ffs(gc->installed));
	return virq ? irq_get_irq_data(virq) : NULL;
}

#ifdef CONFIG_PM
/*
 * irq_gc_suspend() - syscore suspend 阶段挂起所有 generic chip
 *
 * @data: syscore 接口私有参数，未消费。按 gc_list 顺序遍历：若 type0 irq_chip 提供
 * irq_suspend，则取得代表 irq_data 后调用；随后无论是否安装 IRQ，均调用 gc->suspend，
 * 适合保存整芯片状态。返回 0，单个回调无错误通道；PM 阶段列表/映射必须冻结。顺序使
 * per-IRQ 风格回调先执行，per-gc 回调后执行。
 */
static int irq_gc_suspend(void *data)
{
	struct irq_chip_generic *gc;

	list_for_each_entry(gc, &gc_list, list) {
		struct irq_chip_type *ct = gc->chip_types;

		if (ct->chip.irq_suspend) {
			struct irq_data *data = irq_gc_get_irq_data(gc);

			if (data)
				ct->chip.irq_suspend(data);
		}

		if (gc->suspend)
			gc->suspend(gc);
	}
	return 0;
}

/*
 * irq_gc_resume() - syscore resume 阶段恢复所有 generic chip
 *
 * @data 未消费。按 gc_list 顺序先调用 gc->resume 恢复整芯片寄存器/缓存，再在存在代表
 * irq_data 时调用 type0 irq_chip->irq_resume，顺序与 suspend 对称反转。无返回值，回调
 * 不能向 syscore 传播失败。
 */
static void irq_gc_resume(void *data)
{
	struct irq_chip_generic *gc;

	list_for_each_entry(gc, &gc_list, list) {
		struct irq_chip_type *ct = gc->chip_types;

		if (gc->resume)
			gc->resume(gc);

		if (ct->chip.irq_resume) {
			struct irq_data *data = irq_gc_get_irq_data(gc);

			if (data)
				ct->chip.irq_resume(data);
		}
	}
}
#else
/* 未启用 CONFIG_PM 时，syscore ops 中 suspend/resume 回调编译为空指针。 */
#define irq_gc_suspend NULL
#define irq_gc_resume NULL
#endif

/*
 * irq_gc_shutdown() - syscore 关机阶段通知所有 generic chip
 *
 * @data 未消费。遍历 gc_list；type0 chip 提供 irq_pm_shutdown 且能取得代表 irq_data 时
 * 调用。无 gc 级独立 shutdown 回调、无返回值；用于在 reboot/poweroff 前把控制器置于
 * 安全状态，列表和 mapping 此时必须稳定。
 */
static void irq_gc_shutdown(void *data)
{
	struct irq_chip_generic *gc;

	list_for_each_entry(gc, &gc_list, list) {
		struct irq_chip_type *ct = gc->chip_types;

		if (ct->chip.irq_pm_shutdown) {
			struct irq_data *data = irq_gc_get_irq_data(gc);

			if (data)
				ct->chip.irq_pm_shutdown(data);
		}
	}
}

/* generic-chip 的系统级 PM 操作表；CONFIG_PM 关闭时仅 shutdown 非 NULL。 */
static const struct syscore_ops irq_gc_syscore_ops = {
	.suspend = irq_gc_suspend,
	.resume = irq_gc_resume,
	.shutdown = irq_gc_shutdown,
};

/* syscore 注册对象持有上述静态 ops，生命周期覆盖系统运行期。 */
static struct syscore irq_gc_syscore = {
	.ops = &irq_gc_syscore_ops,
};

/*
 * irq_gc_init_ops() - 在 device initcall 阶段注册 generic-chip syscore 回调
 *
 * 无参数。register_syscore() 发布静态 irq_gc_syscore，返回 0；没有注销路径，因为对象及
 * gc 基础设施与内核同寿命。此后加入 gc_list 的 chip 会参与系统 suspend/resume/shutdown。
 */
static int __init irq_gc_init_ops(void)
{
	register_syscore(&irq_gc_syscore);
	return 0;
}
/* 设备初始化阶段完成 syscore 注册，晚于基础 IRQ 初始化。 */
device_initcall(irq_gc_init_ops);
