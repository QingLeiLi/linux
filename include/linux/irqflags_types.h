/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IRQ flags 跟踪类型学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 本头文件只定义可嵌入 task_struct 的 IRQ 跟踪数据，不执行中断开关、
 * lockdep 校验或 tracepoint 输出。include/linux/irqflags.h 声明状态转换
 * 接口，kernel/locking/lockdep.c 在 ON/OFF 转换时更新这里的字段。
 *
 * 数据属于 task 而非 CPU：它记录当前执行流最近一次 hardirq/softirq
 * 转换的位置和事件序号，便于 lockdep 把锁获取与 IRQ 状态历史关联。
 * 真正的硬件屏蔽状态以及 per-CPU hardirqs_enabled 不存放在这里。
 *
 * CONFIG_TRACE_IRQFLAGS=n 时结构体完全不出现，避免未使用的调试字段增加
 * 每个 task_struct 的常驻内存。代价是关闭配置后无法追溯转换位置。
 */
#ifndef _LINUX_IRQFLAGS_TYPES_H
#define _LINUX_IRQFLAGS_TYPES_H

#ifdef CONFIG_TRACE_IRQFLAGS

/* Per-task IRQ trace events information. */
/*
 * 每 task 的 IRQ 状态转换事件信息。
 *
 * irq_events 是 hardirq/softirq 共用的单调递增事件序号；每次有效转换
 * 先递增它，再把新值写入对应 enable/disable_event。比较事件号即可恢复
 * 各类转换在本 task 上的先后关系，而无需维护完整历史队列。
 *
 * *_ip 保存最近一次转换的调用指令地址，只用于诊断和 lockdep 报告，
 * 不持有代码对象或模块引用；读取者必须接受模块卸载或地址符号化
 * 受限。
 * 该结构由当前 task 的 lockdep 路径更新，不是跨 CPU 原子快照。
 */
struct irqtrace_events {
	/* hardirq 与 softirq 四类事件共享的 task-local 序列发生器。 */
	unsigned int	irq_events;

	/* 最近一次 hardirq 开启/关闭的位置及其对应事件序号。 */
	unsigned long	hardirq_enable_ip;
	unsigned long	hardirq_disable_ip;
	unsigned int	hardirq_enable_event;
	unsigned int	hardirq_disable_event;

	/* 最近一次 softirq 关闭/开启的位置及其对应事件序号。 */
	unsigned long	softirq_disable_ip;
	unsigned long	softirq_enable_ip;
	unsigned int	softirq_disable_event;
	unsigned int	softirq_enable_event;
};

#endif

#endif /* _LINUX_IRQFLAGS_TYPES_H */
/* 结束 _LINUX_IRQFLAGS_TYPES_H 头文件防重复包含范围。 */
