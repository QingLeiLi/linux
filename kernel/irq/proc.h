/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_IRQ_PROC_H
#define _KERNEL_IRQ_PROC_H

/*
 * /proc/interrupts 全局格式约束的内部配置裁剪接口。只有 procfs 与通用 IRQ 展示
 * 同时启用时才维护 IRQ 编号列宽和 irq_chip 名称列宽；其他构建保留同签名空实现，
 * 使描述符扩容和安装 irq_chip 的主路径无需条件编译。
 */
#if defined(CONFIG_PROC_FS) && defined(CONFIG_GENERIC_IRQ_SHOW)
/* 根据当前 total_nr_irqs 单调扩大 IRQ 十进制编号列宽；不分配对象、无返回值。 */
void irq_proc_calc_prec(void);
/*
 * 把 @chip 的名称长度单调并入全局展示宽度。chip/名称可为 NULL，指针只在调用期间
 * 借用且不会被保存；入口允许来自已关中断上下文，真实实现使用 irqsave raw lock。
 */
void irq_proc_update_chip(const struct irq_chip *chip);
#else
/* 展示功能未构建时无格式状态需要更新。 */
static inline void irq_proc_calc_prec(void) { }
/* 空实现仍对 chip 类型做编译期检查；实参会求值，但函数不读取或保存该对象。 */
static inline void irq_proc_update_chip(const struct irq_chip *chip) { }
#endif

#endif
