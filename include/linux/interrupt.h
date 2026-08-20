/* SPDX-License-Identifier: GPL-2.0 */
/* interrupt.h */
/* interrupt.h - 中断处理接口 */
#ifndef _LINUX_INTERRUPT_H
#define _LINUX_INTERRUPT_H

/**
 * interrupt.h - Linux 中断子系统核心接口
 *
 * 【什么是中断】
 * 中断是硬件通知CPU发生重要事件的机制，允许CPU异步响应外部事件。
 * 常见中断源：
 * - 硬件设备：网卡、磁盘、键盘、定时器等
 * - 软件中断（softirq）：延迟处理的中断工作
 * - 进程间中断（IPI）：多核CPU间的通信
 *
 * 【中断处理流程】
 * 1. 硬件产生中断信号
 * 2. CPU保存当前上下文，跳转到中断处理程序
 * 3. 执行中断处理程序（IRQ handler）
 * 4. 恢复上下文，返回被中断的代码
 *
 * 【中断上下文的特点】
 * - 不能睡眠（不能调用可能阻塞的函数）
 * - 不能访问用户空间
 * - 执行时间要短，避免影响系统响应
 * - 可以被更高优先级中断抢占
 *
 * 【上半部与下半部】
 * - 上半部（Top Half）：中断处理程序，快速响应硬件，禁止中断
 * - 下半部（Bottom Half）：softirq/tasklet/workqueue，延迟处理，允许中断
 */

#include <linux/kernel.h>          /* 内核基础设施 */
#include <linux/bitops.h>          /* 位操作 */
#include <linux/cleanup.h>         /* 清理机制 */
#include <linux/irqreturn.h>       /* IRQ返回值定义 */
#include <linux/irqnr.h>           /* IRQ编号 */
#include <linux/hardirq.h>         /* 硬中断上下文检测 */
#include <linux/irqflags.h>        /* IRQ标志操作 */
#include <linux/hrtimer.h>         /* 高精度定时器 */
#include <linux/kref.h>            /* 引用计数 */
#include <linux/cpumask_types.h>   /* CPU掩码类型 */
#include <linux/workqueue.h>       /* 工作队列 */
#include <linux/jump_label.h>      /* 静态分支优化 */

#include <linux/atomic.h>          /* 原子操作 */
#include <asm/ptrace.h>            /* 寄存器上下文 */
#include <asm/irq.h>               /* 架构相关IRQ定义 */
#include <asm/sections.h>          /* 内核段定义 */

/*
 * These correspond to the IORESOURCE_IRQ_* defines in
 * linux/ioport.h to select the interrupt line behaviour.  When
 * requesting an interrupt without specifying a IRQF_TRIGGER, the
 * setting should be assumed to be "as already configured", which
 * may be as per machine or firmware initialisation.
 */
/**
 * 【中断触发模式标志】
 *
 * 这些标志对应 linux/ioport.h 中的 IORESOURCE_IRQ_* 定义，
 * 用于选择中断线的触发行为。
 *
 * 如果请求中断时未指定 IRQF_TRIGGER，应假定设置为"已配置的状态"，
 * 即按照机器或固件初始化时的配置。
 *
 * 【触发模式说明】
 * - 边沿触发：在信号跳变时触发（上升沿/下降沿）
 * - 电平触发：在信号保持某电平时持续触发（高电平/低电平）
 */
#define IRQF_TRIGGER_NONE	0x00000000  /* 无特定触发模式，使用默认配置 */
#define IRQF_TRIGGER_RISING	0x00000001  /* 上升沿触发（低电平→高电平） */
#define IRQF_TRIGGER_FALLING	0x00000002  /* 下降沿触发（高电平→低电平） */
#define IRQF_TRIGGER_HIGH	0x00000004  /* 高电平触发（持续高电平时触发） */
#define IRQF_TRIGGER_LOW	0x00000008  /* 低电平触发（持续低电平时触发） */
#define IRQF_TRIGGER_MASK	(IRQF_TRIGGER_HIGH | IRQF_TRIGGER_LOW | \
				 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)  /* 触发模式掩码 */
#define IRQF_TRIGGER_PROBE	0x00000010  /* 探测触发模式（自动检测） */

/*
 * These flags used only by the kernel as part of the
 * irq handling routines.
 *
 * IRQF_SHARED - allow sharing the irq among several devices
 * IRQF_PROBE_SHARED - set by callers when they expect sharing mismatches to occur
 * IRQF_TIMER - Flag to mark this interrupt as timer interrupt
 * IRQF_PERCPU - Interrupt is per cpu
 * IRQF_NOBALANCING - Flag to exclude this interrupt from irq balancing
 * IRQF_IRQPOLL - Interrupt is used for polling (only the interrupt that is
 *                registered first in a shared interrupt is considered for
 *                performance reasons)
 * IRQF_ONESHOT - Interrupt is not reenabled after the hardirq handler finished.
 *                Used by threaded interrupts which need to keep the
 *                irq line disabled until the threaded handler has been run.
 * IRQF_NO_SUSPEND - Do not disable this IRQ during suspend.  Does not guarantee
 *                   that this interrupt will wake the system from a suspended
 *                   state.  See Documentation/power/suspend-and-interrupts.rst
 * IRQF_FORCE_RESUME - Force enable it on resume even if IRQF_NO_SUSPEND is set
 * IRQF_NO_THREAD - Interrupt cannot be threaded
 * IRQF_EARLY_RESUME - Resume IRQ early during syscore instead of at device
 *                resume time.
 * IRQF_COND_SUSPEND - If the IRQ is shared with a NO_SUSPEND user, execute this
 *                interrupt handler after suspending interrupts. For system
 *                wakeup devices users need to implement wakeup detection in
 *                their interrupt handlers.
 * IRQF_NO_AUTOEN - Don't enable IRQ or NMI automatically when users request it.
 *                Users will enable it explicitly by enable_irq() or enable_nmi()
 *                later.
 * IRQF_NO_DEBUG - Exclude from runnaway detection for IPI and similar handlers,
 *		   depends on IRQF_PERCPU.
 * IRQF_COND_ONESHOT - Agree to do IRQF_ONESHOT if already set for a shared
 *                 interrupt.
 */
/**
 * 【中断处理标志】
 *
 * 这些标志仅供内核在中断处理例程中使用。
 *
 * IRQF_SHARED - 允许多个设备共享同一中断线
 *   【共享中断】多个设备可以注册同一个IRQ，中断时依次调用所有处理程序，
 *   每个处理程序检查是否是自己的设备触发的中断
 *
 * IRQF_PROBE_SHARED - 调用者预期会发生共享不匹配
 *   【探测阶段】用于驱动探测阶段，预期可能失败
 *
 * IRQF_TIMER - 标记这是定时器中断
 *   【特殊处理】定时器中断有特殊的调度和统计处理
 *
 * IRQF_PERCPU - 每CPU中断
 *   【每CPU独立】每个CPU有独立的中断处理，不在CPU间迁移
 *   例如：本地定时器中断、IPI（处理器间中断）
 *
 * IRQF_NOBALANCING - 排除此中断的IRQ负载均衡
 *   【固定CPU】不允许IRQ均衡器将此中断迁移到其他CPU
 *
 * IRQF_IRQPOLL - 中断用于轮询
 *   【性能优化】共享中断中只有第一个注册的中断用于轮询，
 *   避免性能损失
 *
 * IRQF_ONESHOT - 硬中断处理程序完成后不重新启用中断
 *   【线程化中断】用于线程化中断，需要保持中断线禁用，
 *   直到线程化处理程序运行完成。防止在线程处理时硬件再次触发。
 *
 * IRQF_NO_SUSPEND - 休眠期间不禁用此IRQ
 *   【保持唤醒】不保证能从休眠状态唤醒系统。
 *   参见 Documentation/power/suspend-and-interrupts.rst
 *
 * IRQF_FORCE_RESUME - 即使设置了IRQF_NO_SUSPEND，恢复时也强制启用
 *   【强制恢复】用于某些特殊硬件
 *
 * IRQF_NO_THREAD - 中断不能被线程化
 *   【必须硬中断】必须在硬中断上下文中执行，不能推迟到线程
 *
 * IRQF_EARLY_RESUME - 在syscore期间提前恢复IRQ
 *   【早期恢复】在设备恢复之前就恢复IRQ，而不是在设备恢复时
 *
 * IRQF_COND_SUSPEND - 如果IRQ与NO_SUSPEND用户共享，在挂起中断后执行
 *   【条件挂起】用于系统唤醒设备，需要在中断处理程序中实现唤醒检测
 *
 * IRQF_NO_AUTOEN - 请求时不自动启用IRQ或NMI
 *   【手动启用】用户稍后通过enable_irq()或enable_nmi()显式启用
 *
 * IRQF_NO_DEBUG - 从失控检测中排除
 *   【调试排除】用于IPI和类似处理程序的失控检测排除，
 *   依赖于IRQF_PERCPU
 *
 * IRQF_COND_ONESHOT - 如果共享中断已设置IRQF_ONESHOT，则同意使用
 *   【条件单次】允许与ONESHOT中断共享
 */
#define IRQF_SHARED		0x00000080
#define IRQF_PROBE_SHARED	0x00000100
#define __IRQF_TIMER		0x00000200
#define IRQF_PERCPU		0x00000400
#define IRQF_NOBALANCING	0x00000800
#define IRQF_IRQPOLL		0x00001000
#define IRQF_ONESHOT		0x00002000
#define IRQF_NO_SUSPEND		0x00004000
#define IRQF_FORCE_RESUME	0x00008000
#define IRQF_NO_THREAD		0x00010000
#define IRQF_EARLY_RESUME	0x00020000
#define IRQF_COND_SUSPEND	0x00040000
#define IRQF_NO_AUTOEN		0x00080000
#define IRQF_NO_DEBUG		0x00100000
#define IRQF_COND_ONESHOT	0x00200000

/**
 * IRQF_TIMER - 定时器中断的组合标志
 *
 * 组合了三个标志：
 * - __IRQF_TIMER: 标记为定时器
 * - IRQF_NO_SUSPEND: 休眠时不禁用（定时器需要持续运行）
 * - IRQF_NO_THREAD: 不能线程化（定时器需要精确的硬中断处理）
 */
#define IRQF_TIMER		(__IRQF_TIMER | IRQF_NO_SUSPEND | IRQF_NO_THREAD)

/*
 * These values can be returned by request_any_context_irq() and
 * describe the context the interrupt will be run in.
 *
 * IRQC_IS_HARDIRQ - interrupt runs in hardirq context
 * IRQC_IS_NESTED - interrupt runs in a nested threaded context
 */
/**
 * 【中断上下文类型】
 *
 * request_any_context_irq() 可以返回这些值，描述中断运行的上下文。
 *
 * IRQC_IS_HARDIRQ - 中断在硬中断上下文中运行
 *   【硬中断】直接在中断处理程序中运行，禁止抢占，不能睡眠
 *
 * IRQC_IS_NESTED - 中断在嵌套的线程化上下文中运行
 *   【线程化】在内核线程中运行，允许睡眠和阻塞操作
 */
enum {
	IRQC_IS_HARDIRQ	= 0,
	IRQC_IS_NESTED,
};

/**
 * irq_handler_t - 中断处理函数类型
 * @参数1: 中断号（IRQ number）
 * @参数2: 设备标识符（dev_id），在注册中断时提供
 *
 * 返回值：irqreturn_t
 * - IRQ_NONE: 不是我的中断（共享中断时使用）
 * - IRQ_HANDLED: 中断已处理
 * - IRQ_WAKE_THREAD: 唤醒线程化处理程序
 */
typedef irqreturn_t (*irq_handler_t)(int, void *);

/**
 * struct irqaction - per interrupt action descriptor
 * @handler:	interrupt handler function
 * @name:	name of the device
 * @dev_id:	cookie to identify the device
 * @percpu_dev_id:	cookie to identify the device
 * @affinity:	CPUs this irqaction is allowed to run on
 * @next:	pointer to the next irqaction for shared interrupts
 * @irq:	interrupt number
 * @flags:	flags (see IRQF_* above)
 * @thread_fn:	interrupt handler function for threaded interrupts
 * @thread:	thread pointer for threaded interrupts
 * @secondary:	pointer to secondary irqaction (force threading)
 * @thread_flags:	flags related to @thread
 * @thread_mask:	bitmask for keeping track of @thread activity
 * @dir:	pointer to the proc/irq/NN/name entry
 */
/**
 * struct irqaction - 中断动作描述符（每个中断一个）
 *
 * 【作用】
 * 描述一个已注册的中断处理程序。当多个设备共享同一个IRQ时，
 * 通过next指针形成链表，中断发生时依次调用链表中的处理程序。
 *
 * @handler: 中断处理函数
 *   【签名】irqreturn_t handler(int irq, void *dev_id)
 *   【返回】IRQ_NONE/IRQ_HANDLED/IRQ_WAKE_THREAD
 *
 * @name: 设备名称
 *   【用途】在/proc/interrupts中显示，用于识别中断来源
 *   【示例】"eth0", "i8042"
 *
 * @dev_id: 设备标识符cookie
 *   【共享中断】用于区分共享同一IRQ的不同设备
 *   【传递】作为第二个参数传递给handler
 *   【注销】注销中断时需要提供相同的dev_id
 *
 * @percpu_dev_id: 每CPU设备标识符cookie
 *   【每CPU中断】对于IRQF_PERCPU中断，每个CPU有独立的dev_id
 *
 * @affinity: CPU亲和性
 *   【绑定CPU】此中断允许在哪些CPU上运行
 *
 * @next: 下一个irqaction指针
 *   【共享中断】指向共享同一IRQ的下一个处理程序，形成链表
 *
 * @irq: 中断号
 *   【IRQ编号】此处理程序对应的中断号
 *
 * @flags: 标志位
 *   【参见】上面的IRQF_*定义
 *
 * @thread_fn: 线程化中断处理函数
 *   【线程化】在内核线程中执行的处理函数，允许睡眠
 *   【流程】handler返回IRQ_WAKE_THREAD时，唤醒thread执行thread_fn
 *
 * @thread: 线程指针
 *   【线程化】指向运行thread_fn的内核线程（task_struct）
 *
 * @secondary: 次级irqaction指针
 *   【强制线程化】用于强制线程化时的次级处理
 *
 * @thread_flags: 线程相关标志
 *   【线程状态】跟踪线程的各种状态
 *
 * @thread_mask: 线程活动位掩码
 *   【活动跟踪】跟踪@thread的活动状态
 *
 * @dir: proc目录项指针
 *   【/proc】指向/proc/irq/NN/name条目
 */
struct irqaction {
	irq_handler_t		handler;
	union {
		void		*dev_id;
		void __percpu	*percpu_dev_id;
	};
	const struct cpumask	*affinity;
	struct irqaction	*next;
	irq_handler_t		thread_fn;
	struct task_struct	*thread;
	struct irqaction	*secondary;
	unsigned int		irq;
	unsigned int		flags;
	unsigned long		thread_flags;
	unsigned long		thread_mask;
	const char		*name;
	struct proc_dir_entry	*dir;
} ____cacheline_internodealigned_in_smp;  /* 缓存行对齐，在SMP系统中避免false sharing */

/**
 * no_action - 空的中断处理函数
 * @cpl: 中断号
 * @dev_id: 设备标识符
 *
 * 【作用】占位用的空处理函数，总是返回IRQ_NONE
 * 【用途】某些硬件需要注册中断但不需要实际处理
 */
extern irqreturn_t no_action(int cpl, void *dev_id);

/*
 * If a (PCI) device interrupt is not connected we set dev->irq to
 * IRQ_NOTCONNECTED. This causes request_irq() to fail with -ENOTCONN, so we
 * can distinguish that case from other error returns.
 *
 * 0x80000000 is guaranteed to be outside the available range of interrupts
 * and easy to distinguish from other possible incorrect values.
 */
/**
 * IRQ_NOTCONNECTED - 表示中断未连接
 *
 * 如果（PCI）设备中断未连接，我们将dev->irq设置为IRQ_NOTCONNECTED。
 * 这会导致request_irq()失败并返回-ENOTCONN，使我们能够区分这种情况
 * 与其他错误返回值。
 *
 * 0x80000000保证在可用中断范围之外，且易于与其他可能的错误值区分。
 */
#define IRQ_NOTCONNECTED	(1U << 31)

/**
 * request_threaded_irq - 分配一个中断线（支持线程化）
 * @irq: 要分配的中断号
 * @handler: 在中断时调用的硬中断处理函数（可以为NULL）
 * @thread_fn: 在线程化上下文中调用的函数（可以为NULL）
 * @flags: 中断类型标志（IRQF_*）
 * @name: 此中断所有者的ASCII名称
 * @dev: 传递回处理函数的cookie
 *
 * 【作用】
 * 注册一个中断处理程序，支持线程化处理。这是最灵活的中断注册函数。
 *
 * 【参数说明】
 * - handler和thread_fn至少要提供一个（不能都为NULL）
 * - handler: 硬中断上下文，快速响应，禁止睡眠
 * - thread_fn: 内核线程上下文，可以睡眠和阻塞
 * - dev: 用于区分共享中断，注销时需要提供相同的dev
 *
 * 【工作流程】
 * 1. 硬件触发中断
 * 2. 调用handler（如果提供）
 * 3. 如果handler返回IRQ_WAKE_THREAD，唤醒线程执行thread_fn
 * 4. 如果只提供thread_fn，自动生成一个返回IRQ_WAKE_THREAD的handler
 *
 * 【flags常用组合】
 * - IRQF_SHARED: 共享中断，多个设备可以使用同一IRQ
 * - IRQF_ONESHOT: 线程化处理完成前保持中断禁用
 * - IRQF_TRIGGER_*: 触发模式（上升沿/下降沿/高电平/低电平）
 *
 * 【返回值】
 * 成功返回0，失败返回负错误码：
 * - -EINVAL: 无效参数（irq无效，handler和thread_fn都为NULL）
 * - -EBUSY: 中断已被其他不可共享的处理程序占用
 * - -ENOTCONN: 中断未连接（IRQ_NOTCONNECTED）
 *
 * 【注意事项】
 * - 中断处理程序可能在返回前就开始运行
 * - 对于共享中断，dev不能为NULL（用于区分设备）
 * - 线程化中断通常与IRQF_ONESHOT一起使用
 */
extern int __must_check
request_threaded_irq(unsigned int irq, irq_handler_t handler,
		     irq_handler_t thread_fn,
		     unsigned long flags, const char *name, void *dev);

/**
 * request_irq - Add a handler for an interrupt line
 * @irq:	The interrupt line to allocate
 * @handler:	Function to be called when the IRQ occurs.
 *		Primary handler for threaded interrupts
 *		If NULL, the default primary handler is installed
 * @flags:	Handling flags
 * @name:	Name of the device generating this interrupt
 * @dev:	A cookie passed to the handler function
 *
 * This call allocates an interrupt and establishes a handler; see
 * the documentation for request_threaded_irq() for details.
 */
/**
 * request_irq - 为中断线添加处理程序（简化版）
 * @irq: 要分配的中断线
 * @handler: IRQ发生时调用的函数
 *           对于线程化中断，这是主处理程序
 *           如果为NULL，安装默认的主处理程序
 * @flags: 处理标志
 * @name: 生成此中断的设备名称
 * @dev: 传递给处理函数的cookie
 *
 * 此调用分配一个中断并建立处理程序；详细信息参见
 * request_threaded_irq() 的文档。
 *
 * 【简化版本】
 * 这是request_threaded_irq()的简化版本，用于只需要硬中断处理的场景。
 * 内部调用：request_threaded_irq(irq, handler, NULL, flags | IRQF_COND_ONESHOT, name, dev)
 * 自动添加IRQF_COND_ONESHOT，兼容共享的线程化中断。
 */
static inline int __must_check
request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
	    const char *name, void *dev)
{
	return request_threaded_irq(irq, handler, NULL, flags | IRQF_COND_ONESHOT, name, dev);
}

/**
 * request_any_context_irq - 分配中断线（自动选择上下文）
 * @irq: 要分配的中断号
 * @handler: 中断处理函数
 * @flags: 中断类型标志
 * @name: 设备名称
 * @dev_id: 设备标识符cookie
 *
 * 【自动适配】
 * 根据中断控制器的特性，自动选择硬中断或线程化上下文。
 * 返回IRQC_IS_HARDIRQ或IRQC_IS_NESTED，指示实际使用的上下文。
 *
 * 【使用场景】
 * 用于驱动程序不确定中断会在哪种上下文中运行的情况。
 */
extern int __must_check
request_any_context_irq(unsigned int irq, irq_handler_t handler,
			unsigned long flags, const char *name, void *dev_id);

/**
 * request_percpu_irq_affinity - 分配每CPU中断（带亲和性）
 * @irq: 要分配的中断号
 * @handler: 中断处理函数
 * @devname: 设备名称
 * @affinity: CPU亲和性掩码
 * @percpu_dev_id: 每CPU设备标识符
 *
 * 【每CPU中断】
 * 每个CPU有独立的中断实例，常用于：
 * - 本地定时器中断
 * - IPI（处理器间中断）
 * - 每CPU性能监控中断
 */
extern int __must_check
request_percpu_irq_affinity(unsigned int irq, irq_handler_t handler, const char *devname,
			    const cpumask_t *affinity, void __percpu *percpu_dev_id);

/**
 * request_nmi - 分配NMI（不可屏蔽中断）
 * @irq: NMI号
 * @handler: NMI处理函数
 * @flags: 标志
 * @name: 处理程序名称
 * @dev: 设备标识符
 *
 * 【NMI特点】
 * - 不可被普通中断禁用指令屏蔽
 * - 优先级最高
 * - 用于严重错误（硬件故障、看门狗超时）
 * - 调试（NMI按钮、性能分析）
 */
extern int __must_check
request_nmi(unsigned int irq, irq_handler_t handler, unsigned long flags,
	    const char *name, void *dev);

/**
 * request_percpu_irq - 分配每CPU中断（无亲和性指定）
 *
 * 这是request_percpu_irq_affinity()的简化版本，亲和性设为NULL（所有CPU）。
 */
static inline int __must_check
request_percpu_irq(unsigned int irq, irq_handler_t handler,
		   const char *devname, void __percpu *percpu_dev_id)
{
	return request_percpu_irq_affinity(irq, handler, devname,
					   NULL, percpu_dev_id);
}

/**
 * request_percpu_nmi - 分配每CPU NMI
 * @irq: NMI号
 * @handler: NMI处理函数
 * @name: 处理程序名称
 * @affinity: CPU亲和性
 * @dev_id: 每CPU设备标识符
 *
 * 【每CPU NMI】
 * 结合了NMI和每CPU中断的特性。
 */
extern int __must_check
request_percpu_nmi(unsigned int irq, irq_handler_t handler, const char *name,
		   const struct cpumask *affinity, void __percpu *dev_id);

/**
 * free_irq - 释放中断
 * @中断号: 要释放的中断
 * @dev_id: 设备标识符（必须与request_irq时相同）
 *
 * 【注意】
 * - 移除中断处理程序
 * - 如果这是此IRQ的最后一个处理程序，则禁用中断线
 * - dev_id用于区分共享中断
 * - 返回被移除的dev_id指针
 */
extern const void *free_irq(unsigned int, void *);

/**
 * free_percpu_irq - 释放每CPU中断
 */
extern void free_percpu_irq(unsigned int, void __percpu *);

/**
 * free_nmi - 释放NMI
 */
extern const void *free_nmi(unsigned int irq, void *dev_id);

/**
 * free_percpu_nmi - 释放每CPU NMI
 */
extern void free_percpu_nmi(unsigned int irq, void __percpu *percpu_dev_id);

struct device;

/**
 * devm_request_threaded_irq - 设备托管的线程化中断请求
 * @dev: 设备指针
 * @irq: 中断号
 * @handler: 硬中断处理函数
 * @thread_fn: 线程化处理函数
 * @irqflags: 中断标志
 * @devname: 设备名称
 * @dev_id: 设备标识符
 *
 * 【设备托管】
 * 使用设备资源管理（devres）框架，设备注销时自动释放中断。
 * 驱动不需要显式调用free_irq()，减少资源泄漏风险。
 */
extern int __must_check
devm_request_threaded_irq(struct device *dev, unsigned int irq,
			  irq_handler_t handler, irq_handler_t thread_fn,
			  unsigned long irqflags, const char *devname,
			  void *dev_id);

/**
 * devm_request_irq - 设备托管的中断请求（简化版）
 *
 * devm_request_threaded_irq()的简化版本。
 */
static inline int __must_check
devm_request_irq(struct device *dev, unsigned int irq, irq_handler_t handler,
		 unsigned long irqflags, const char *devname, void *dev_id)
{
	return devm_request_threaded_irq(dev, irq, handler, NULL, irqflags | IRQF_COND_ONESHOT,
					 devname, dev_id);
}

/**
 * devm_request_any_context_irq - 设备托管的自动上下文中断请求
 */
extern int __must_check
devm_request_any_context_irq(struct device *dev, unsigned int irq,
		 irq_handler_t handler, unsigned long irqflags,
		 const char *devname, void *dev_id);

/**
 * devm_free_irq - 显式释放设备托管的中断
 * @dev: 设备指针
 * @irq: 中断号
 * @dev_id: 设备标识符
 *
 * 【手动释放】
 * 虽然devres会自动释放，但某些情况下需要提前手动释放。
 */
extern void devm_free_irq(struct device *dev, unsigned int irq, void *dev_id);

/**
 * irq_has_action - 检查IRQ是否有已注册的处理程序
 * @irq: 中断号
 *
 * 返回：true表示有处理程序，false表示无
 */
bool irq_has_action(unsigned int irq);

/**
 * disable_irq_nosync - 禁用中断（不等待）
 * @irq: 中断号
 *
 * 【不同步】
 * 立即返回，不等待当前正在运行的处理程序完成。
 * 【危险】如果处理程序正在运行，可能导致竞争条件。
 */
extern void disable_irq_nosync(unsigned int irq);

/**
 * disable_hardirq - 尝试禁用硬中断
 * @irq: 中断号
 *
 * 【非阻塞】
 * 如果中断正在运行，立即返回false；否则禁用并返回true。
 * 【用途】避免等待的禁用操作。
 */
extern bool disable_hardirq(unsigned int irq);

/**
 * disable_irq - 禁用中断（同步）
 * @irq: 中断号
 *
 * 【同步等待】
 * 禁用中断并等待当前运行的处理程序完成。
 * 【安全】确保返回时处理程序已完全退出。
 */
extern void disable_irq(unsigned int irq);

/**
 * disable_percpu_irq - 禁用每CPU中断
 * @irq: 中断号
 *
 * 【当前CPU】
 * 只禁用当前CPU上的中断实例。
 */
extern void disable_percpu_irq(unsigned int irq);

/**
 * enable_irq - 启用中断
 * @irq: 中断号
 *
 * 【引用计数】
 * 每次disable_irq增加计数，enable_irq减少计数。
 * 只有计数为0时中断才真正启用。
 */
extern void enable_irq(unsigned int irq);

/**
 * enable_percpu_irq - 启用每CPU中断
 * @irq: 中断号
 * @type: 触发类型
 */
extern void enable_percpu_irq(unsigned int irq, unsigned int type);

/**
 * irq_percpu_is_enabled - 检查每CPU中断是否启用
 * @irq: 中断号
 *
 * 返回：true表示启用，false表示禁用
 */
extern bool irq_percpu_is_enabled(unsigned int irq);

/**
 * irq_wake_thread - 唤醒线程化中断的线程
 * @irq: 中断号
 * @dev_id: 设备标识符
 *
 * 【手动唤醒】
 * 显式唤醒线程化处理程序，通常在特殊情况下使用。
 */
extern void irq_wake_thread(unsigned int irq, void *dev_id);

/**
 * DEFINE_LOCK_GUARD_1 - 定义中断禁用的锁保护宏
 *
 * 【RAII风格】
 * 自动在作用域内禁用中断，离开作用域时自动启用。
 * 使用：guard(disable_irq)(&irq_num);
 */
DEFINE_LOCK_GUARD_1(disable_irq, int,
		    disable_irq(*_T->lock), enable_irq(*_T->lock))

/**
 * disable_nmi_nosync - 禁用NMI（不同步）
 */
extern void disable_nmi_nosync(unsigned int irq);

/**
 * disable_percpu_nmi - 禁用每CPU NMI
 */
extern void disable_percpu_nmi(unsigned int irq);

/**
 * enable_nmi - 启用NMI
 */
extern void enable_nmi(unsigned int irq);

/**
 * enable_percpu_nmi - 启用每CPU NMI
 */
extern void enable_percpu_nmi(unsigned int irq, unsigned int type);

/**
 * prepare_percpu_nmi - 准备每CPU NMI
 * @irq: NMI号
 *
 * 【准备阶段】
 * 在实际启用NMI之前的准备工作。
 */
extern int prepare_percpu_nmi(unsigned int irq);

/**
 * teardown_percpu_nmi - 拆除每CPU NMI
 * @irq: NMI号
 *
 * 【清理阶段】
 * 清理NMI相关资源。
 */
extern void teardown_percpu_nmi(unsigned int irq);

/**
 * irq_inject_interrupt - 注入中断（调试用）
 * @irq: 中断号
 *
 * 【软件触发】
 * 软件触发一个中断，用于测试和调试。
 *
 * 返回：成功返回0，失败返回负错误码
 */
extern int irq_inject_interrupt(unsigned int irq);

/* The following three functions are for the core kernel use only. */
/**
 * 以下三个函数仅供内核核心使用。
 */

/**
 * suspend_device_irqs - 挂起设备中断
 *
 * 【电源管理】
 * 系统进入休眠状态前，挂起所有设备中断。
 */
extern void suspend_device_irqs(void);

/**
 * resume_device_irqs - 恢复设备中断
 *
 * 【电源管理】
 * 系统从休眠状态恢复后，重新启用设备中断。
 */
extern void resume_device_irqs(void);

/**
 * rearm_wake_irq - 重新装备唤醒中断
 * @irq: 中断号
 *
 * 【唤醒中断】
 * 重新配置唤醒中断，使其能够唤醒休眠的系统。
 */
extern void rearm_wake_irq(unsigned int irq);

/**
 * struct irq_affinity_notify - context for notification of IRQ affinity changes
 * @irq:		Interrupt to which notification applies
 * @kref:		Reference count, for internal use
 * @work:		Work item, for internal use
 * @notify:		Function to be called on change.  This will be
 *			called in process context.
 * @release:		Function to be called on release.  This will be
 *			called in process context.  Once registered, the
 *			structure must only be freed when this function is
 *			called or later.
 */
/**
 * struct irq_affinity_notify - IRQ亲和性变更通知上下文
 * @irq: 通知所应用的中断号
 * @kref: 引用计数，内部使用
 * @work: 工作项，内部使用
 * @notify: 变更时调用的函数
 *          【进程上下文】在进程上下文中调用
 * @release: 释放时调用的函数
 *           【进程上下文】在进程上下文中调用。注册后，
 *           此结构只能在调用此函数或之后才能释放。
 *
 * 【亲和性通知】
 * 当中断的CPU亲和性发生变化时，通过此结构通知感兴趣的代码。
 */
struct irq_affinity_notify {
	unsigned int irq;
	struct kref kref;
	struct work_struct work;
	void (*notify)(struct irq_affinity_notify *, const cpumask_t *mask);
	void (*release)(struct kref *ref);
};

/**
 * IRQ_AFFINITY_MAX_SETS - 最大中断集数量
 *
 * 【中断集】
 * 用于将中断分组，每组有独立的亲和性分配策略。
 */
#define	IRQ_AFFINITY_MAX_SETS  4

/**
 * struct irq_affinity - Description for automatic irq affinity assignments
 * @pre_vectors:	Don't apply affinity to @pre_vectors at beginning of
 *			the MSI(-X) vector space
 * @post_vectors:	Don't apply affinity to @post_vectors at end of
 *			the MSI(-X) vector space
 * @nr_sets:		The number of interrupt sets for which affinity
 *			spreading is required
 * @set_size:		Array holding the size of each interrupt set
 * @calc_sets:		Callback for calculating the number and size
 *			of interrupt sets
 * @priv:		Private data for usage by @calc_sets, usually a
 *			pointer to driver/device specific data.
 */
/**
 * struct irq_affinity - 自动IRQ亲和性分配的描述
 * @pre_vectors: MSI(-X)向量空间开头的@pre_vectors个不应用亲和性
 *               【保留向量】通常是管理向量（如MSI-X的配置向量）
 * @post_vectors: MSI(-X)向量空间末尾的@post_vectors个不应用亲和性
 *                【保留向量】用于特殊用途的向量
 * @nr_sets: 需要进行亲和性分散的中断集数量
 *           【分组】不同的中断集可以有不同的分配策略
 * @set_size: 数组，保存每个中断集的大小
 *            【每集大小】set_size[i]表示第i个集合的中断数量
 * @calc_sets: 计算中断集数量和大小的回调函数
 *             【动态计算】根据可用中断数动态调整集合划分
 * @priv: @calc_sets使用的私有数据
 *        【驱动数据】通常是指向驱动/设备特定数据的指针
 *
 * 【自动分配】
 * 用于MSI/MSI-X中断的自动CPU亲和性分配，将中断均匀分散到各CPU上，
 * 提高多核系统的并行处理能力。
 */
struct irq_affinity {
	unsigned int	pre_vectors;
	unsigned int	post_vectors;
	unsigned int	nr_sets;
	unsigned int	set_size[IRQ_AFFINITY_MAX_SETS];
	void		(*calc_sets)(struct irq_affinity *, unsigned int nvecs);
	void		*priv;
};

/**
 * struct irq_affinity_desc - Interrupt affinity descriptor
 * @mask:	cpumask to hold the affinity assignment
 * @is_managed: 1 if the interrupt is managed internally
 */
/**
 * struct irq_affinity_desc - 中断亲和性描述符
 * @mask: 保存亲和性分配的CPU掩码
 *        【CPU集合】此中断可以在哪些CPU上运行
 * @is_managed: 1表示中断由内部管理
 *              【托管中断】托管中断的亲和性由内核自动管理，
 *              用户不应手动修改
 */
struct irq_affinity_desc {
	struct cpumask	mask;
	unsigned int	is_managed : 1;  /* 位域，只占用1位 */
};

#if defined(CONFIG_SMP)

/**
 * irq_default_affinity - 默认IRQ亲和性掩码
 *
 * 【默认CPU集合】
 * 新分配的中断默认使用此CPU掩码。
 */
extern cpumask_var_t irq_default_affinity;

/**
 * irq_set_affinity - 设置中断亲和性
 * @irq: 中断号
 * @cpumask: 目标CPU掩码
 *
 * 【设置CPU】
 * 将中断绑定到指定的CPU集合。
 *
 * 返回：成功返回0，失败返回负错误码
 */
extern int irq_set_affinity(unsigned int irq, const struct cpumask *cpumask);

/**
 * irq_force_affinity - 强制设置中断亲和性
 * @irq: 中断号
 * @cpumask: 目标CPU掩码
 *
 * 【强制绑定】
 * 强制设置中断亲和性，即使对托管中断也生效。
 * 【危险】可能破坏内核的亲和性管理策略。
 *
 * 返回：成功返回0，失败返回负错误码
 */
extern int irq_force_affinity(unsigned int irq, const struct cpumask *cpumask);

/**
 * irq_can_set_affinity - 检查是否可以设置中断亲和性
 * @irq: 中断号
 *
 * 返回：非零表示可以设置，0表示不可以
 */
extern int irq_can_set_affinity(unsigned int irq);

/**
 * irq_select_affinity - 为中断选择亲和性
 * @irq: 中断号
 *
 * 【自动选择】
 * 让内核为中断自动选择合适的CPU亲和性。
 *
 * 返回：成功返回0，失败返回负错误码
 */
extern int irq_select_affinity(unsigned int irq);

/**
 * __irq_apply_affinity_hint - 应用亲和性提示（内部函数）
 * @irq: 中断号
 * @m: CPU掩码指针（NULL表示清除提示）
 * @setaffinity: 是否同时设置实际亲和性
 *
 * 【内部函数】
 * 供irq_update_affinity_hint()和irq_set_affinity_and_hint()使用。
 */
extern int __irq_apply_affinity_hint(unsigned int irq, const struct cpumask *m,
				     bool setaffinity);

/**
 * irq_update_affinity_hint - Update the affinity hint
 * @irq:	Interrupt to update
 * @m:		cpumask pointer (NULL to clear the hint)
 *
 * Updates the affinity hint, but does not change the affinity of the interrupt.
 */
/**
 * irq_update_affinity_hint - 更新亲和性提示
 * @irq: 要更新的中断
 * @m: CPU掩码指针（NULL表示清除提示）
 *
 * 更新亲和性提示，但不改变中断的实际亲和性。
 *
 * 【提示 vs 实际】
 * - 提示：建议的CPU集合，显示在/proc/irq/N/affinity_hint
 * - 实际：真正运行的CPU集合，在/proc/irq/N/smp_affinity
 *
 * 【用途】
 * 驱动可以提供建议的亲和性，供用户空间工具参考，
 * 但不强制修改实际的亲和性配置。
 */
static inline int
irq_update_affinity_hint(unsigned int irq, const struct cpumask *m)
{
	return __irq_apply_affinity_hint(irq, m, false);
}

/**
 * irq_set_affinity_and_hint - Update the affinity hint and apply the provided
 *			     cpumask to the interrupt
 * @irq:	Interrupt to update
 * @m:		cpumask pointer (NULL to clear the hint)
 *
 * Updates the affinity hint and if @m is not NULL it applies it as the
 * affinity of that interrupt.
 */
/**
 * irq_set_affinity_and_hint - 更新亲和性提示并应用到中断
 * @irq: 要更新的中断
 * @m: CPU掩码指针（NULL表示清除提示）
 *
 * 更新亲和性提示，并且如果@m不为NULL，将其应用为该中断的实际亲和性。
 *
 * 【同时设置】
 * 既设置提示（/proc/irq/N/affinity_hint），
 * 又设置实际亲和性（/proc/irq/N/smp_affinity）。
 */
static inline int
irq_set_affinity_and_hint(unsigned int irq, const struct cpumask *m)
{
	return __irq_apply_affinity_hint(irq, m, true);
}

/*
 * Deprecated. Use irq_update_affinity_hint() or irq_set_affinity_and_hint()
 * instead.
 */
/**
 * 已弃用。请使用 irq_update_affinity_hint() 或 irq_set_affinity_and_hint()
 * 代替。
 */
static inline int irq_set_affinity_hint(unsigned int irq, const struct cpumask *m)
{
	return irq_set_affinity_and_hint(irq, m);
}

/**
 * irq_update_affinity_desc - 更新中断亲和性描述符
 * @irq: 中断号
 * @affinity: 新的亲和性描述符
 *
 * 【托管中断】
 * 更新托管中断的亲和性描述符。
 *
 * 返回：成功返回0，失败返回负错误码
 */
extern int irq_update_affinity_desc(unsigned int irq,
				    struct irq_affinity_desc *affinity);

/**
 * irq_set_affinity_notifier - 设置亲和性变更通知器
 * @irq: 中断号
 * @notify: 通知结构指针
 *
 * 【注册回调】
 * 注册一个回调函数，当中断亲和性变更时被调用。
 *
 * 返回：成功返回0，失败返回负错误码
 */
extern int
irq_set_affinity_notifier(unsigned int irq, struct irq_affinity_notify *notify);

/**
 * irq_create_affinity_masks - 创建亲和性掩码
 * @nvec: 向量数量
 * @affd: 亲和性描述符
 *
 * 【自动分配】
 * 根据提供的亲和性描述符，为nvec个中断创建CPU亲和性掩码。
 *
 * 返回：亲和性描述符数组
 */
struct irq_affinity_desc *
irq_create_affinity_masks(unsigned int nvec, struct irq_affinity *affd);

/**
 * irq_calc_affinity_vectors - 计算亲和性向量数量
 * @minvec: 最小向量数
 * @maxvec: 最大向量数
 * @affd: 亲和性描述符
 *
 * 【向量计算】
 * 根据系统CPU数量和亲和性配置，计算最优的中断向量数量。
 *
 * 返回：建议的向量数量（在minvec和maxvec之间）
 */
unsigned int irq_calc_affinity_vectors(unsigned int minvec, unsigned int maxvec,
				       const struct irq_affinity *affd);

#else /* CONFIG_SMP */

/**
 * 以下是CONFIG_SMP未定义时的存根实现（单处理器系统）
 */

static inline int irq_set_affinity(unsigned int irq, const struct cpumask *m)
{
	return -EINVAL;  /* 单CPU系统不支持亲和性设置 */
}

static inline int irq_force_affinity(unsigned int irq, const struct cpumask *cpumask)
{
	return 0;
}

static inline int irq_can_set_affinity(unsigned int irq)
{
	return 0;  /* 单CPU系统不能设置亲和性 */
}

static inline int irq_select_affinity(unsigned int irq)  { return 0; }

static inline int irq_update_affinity_hint(unsigned int irq,
					   const struct cpumask *m)
{
	return -EINVAL;
}

static inline int irq_set_affinity_and_hint(unsigned int irq,
					    const struct cpumask *m)
{
	return -EINVAL;
}

static inline int irq_set_affinity_hint(unsigned int irq,
					const struct cpumask *m)
{
	return -EINVAL;
}

static inline int irq_update_affinity_desc(unsigned int irq,
					   struct irq_affinity_desc *affinity)
{
	return -EINVAL;
}

static inline int
irq_set_affinity_notifier(unsigned int irq, struct irq_affinity_notify *notify)
{
	return 0;
}

static inline struct irq_affinity_desc *
irq_create_affinity_masks(unsigned int nvec, struct irq_affinity *affd)
{
	return NULL;
}

static inline unsigned int
irq_calc_affinity_vectors(unsigned int minvec, unsigned int maxvec,
			  const struct irq_affinity *affd)
{
	return maxvec;  /* 单CPU系统返回最大值 */
}

#endif /* CONFIG_SMP */

/*
 * Special lockdep variants of irq disabling/enabling.
 * These should be used for locking constructs that
 * know that a particular irq context which is disabled,
 * and which is the only irq-context user of a lock,
 * that it's safe to take the lock in the irq-disabled
 * section without disabling hardirqs.
 *
 * On !CONFIG_LOCKDEP they are equivalent to the normal
 * irq disable/enable methods.
 */
/**
 * 特殊的lockdep变体的中断禁用/启用。
 * 这些应该用于以下锁构造：
 * 知道某个被禁用的特定IRQ上下文，
 * 并且它是锁的唯一IRQ上下文用户，
 * 在IRQ禁用的区域内获取锁是安全的，无需禁用硬中断。
 *
 * 在!CONFIG_LOCKDEP上，它们等价于普通的
 * IRQ禁用/启用方法。
 *
 * 【lockdep调试】
 * 这些函数帮助lockdep正确追踪锁的依赖关系，
 * 避免误报死锁警告。
 */
static inline void disable_irq_nosync_lockdep(unsigned int irq)
{
	disable_irq_nosync(irq);
#if defined(CONFIG_LOCKDEP) && !defined(CONFIG_PREEMPT_RT)
	local_irq_disable();  /* lockdep需要同时禁用本地中断 */
#endif
}

/**
 * disable_irq_nosync_lockdep_irqsave - 禁用中断并保存标志（lockdep版本）
 * @irq: 中断号
 * @flags: 保存中断状态的标志
 *
 * 【保存状态】
 * lockdep版本，同时保存本地中断状态。
 */
static inline void disable_irq_nosync_lockdep_irqsave(unsigned int irq, unsigned long *flags)
{
	disable_irq_nosync(irq);
#if defined(CONFIG_LOCKDEP) && !defined(CONFIG_PREEMPT_RT)
	local_irq_save(*flags);
#endif
}

/**
 * enable_irq_lockdep - 启用中断（lockdep版本）
 * @irq: 中断号
 *
 * 【lockdep版本】
 * 先启用本地中断，再启用指定的中断。
 */
static inline void enable_irq_lockdep(unsigned int irq)
{
#if defined(CONFIG_LOCKDEP) && !defined(CONFIG_PREEMPT_RT)
	local_irq_enable();
#endif
	enable_irq(irq);
}

/**
 * enable_irq_lockdep_irqrestore - 启用中断并恢复标志（lockdep版本）
 * @irq: 中断号
 * @flags: 之前保存的中断状态标志
 *
 * 【恢复状态】
 * lockdep版本，恢复之前保存的本地中断状态。
 */
static inline void enable_irq_lockdep_irqrestore(unsigned int irq, unsigned long *flags)
{
#if defined(CONFIG_LOCKDEP) && !defined(CONFIG_PREEMPT_RT)
	local_irq_restore(*flags);
#endif
	enable_irq(irq);
}

/* IRQ wakeup (PM) control: */
/**
 * IRQ唤醒（电源管理）控制：
 */

/**
 * irq_set_irq_wake - 设置中断唤醒能力
 * @irq: 中断号
 * @on: 1=启用唤醒，0=禁用唤醒
 *
 * 【系统唤醒】
 * 配置中断是否可以将系统从休眠状态唤醒。
 *
 * 返回：成功返回0，失败返回负错误码
 */
extern int irq_set_irq_wake(unsigned int irq, unsigned int on);

/**
 * enable_irq_wake - 启用中断唤醒
 * @irq: 中断号
 *
 * 【唤醒源】
 * 将此中断标记为唤醒源，可以唤醒休眠的系统。
 */
static inline int enable_irq_wake(unsigned int irq)
{
	return irq_set_irq_wake(irq, 1);
}

/**
 * disable_irq_wake - 禁用中断唤醒
 * @irq: 中断号
 *
 * 【取消唤醒】
 * 取消此中断作为唤醒源。
 */
static inline int disable_irq_wake(unsigned int irq)
{
	return irq_set_irq_wake(irq, 0);
}

/*
 * irq_get_irqchip_state/irq_set_irqchip_state specific flags
 */
/**
 * irq_get_irqchip_state/irq_set_irqchip_state 特定标志
 *
 * 【中断芯片状态】
 * 用于查询和设置中断控制器内部状态。
 */
enum irqchip_irq_state {
	IRQCHIP_STATE_PENDING,		/* Is interrupt pending? */
	                                /* 中断是否挂起？等待处理 */
	IRQCHIP_STATE_ACTIVE,		/* Is interrupt in progress? */
	                                /* 中断是否正在处理？已确认但未完成 */
	IRQCHIP_STATE_MASKED,		/* Is interrupt masked? */
	                                /* 中断是否被屏蔽？被阻止触发 */
	IRQCHIP_STATE_LINE_LEVEL,	/* Is IRQ line high? */
	                                /* IRQ线是否为高电平？硬件信号状态 */
};

/**
 * irq_get_irqchip_state - 获取中断芯片状态
 * @irq: 中断号
 * @which: 要查询的状态类型（PENDING/ACTIVE/MASKED/LINE_LEVEL）
 * @state: 输出参数，返回状态值（true/false）
 *
 * 【查询状态】
 * 读取中断控制器的内部状态。
 *
 * 返回：成功返回0，失败返回负错误码
 */
extern int irq_get_irqchip_state(unsigned int irq, enum irqchip_irq_state which,
				 bool *state);

/**
 * irq_set_irqchip_state - 设置中断芯片状态
 * @irq: 中断号
 * @which: 要设置的状态类型
 * @state: 新的状态值（true/false）
 *
 * 【修改状态】
 * 直接修改中断控制器的内部状态，用于特殊情况。
 *
 * 返回：成功返回0，失败返回负错误码
 */
extern int irq_set_irqchip_state(unsigned int irq, enum irqchip_irq_state which,
				 bool state);

#ifdef CONFIG_IRQ_FORCED_THREADING
# ifdef CONFIG_PREEMPT_RT
/**
 * force_irqthreads - 检查是否强制线程化中断
 *
 * 【实时内核】
 * PREEMPT_RT内核强制所有中断线程化，返回true。
 */
#  define force_irqthreads()	(true)
# else
/**
 * force_irqthreads_key - 强制线程化中断的静态键
 *
 * 【运行时控制】
 * 通过内核参数threadirqs可以在启动时强制线程化。
 */
DECLARE_STATIC_KEY_FALSE(force_irqthreads_key);
/**
 * force_irqthreads - 检查是否强制线程化中断
 *
 * 【动态检查】
 * 使用静态键优化，避免运行时开销。
 */
#  define force_irqthreads()	(static_branch_unlikely(&force_irqthreads_key))
# endif
#else
/**
 * force_irqthreads - 不支持强制线程化
 */
#define force_irqthreads()	(false)
#endif

#ifndef local_softirq_pending

#ifndef local_softirq_pending_ref
/**
 * local_softirq_pending_ref - 本地softirq挂起位图引用
 *
 * 【每CPU变量】
 * 指向irq_stat结构中的__softirq_pending字段。
 */
#define local_softirq_pending_ref irq_stat.__softirq_pending
#endif

/**
 * local_softirq_pending - 读取本地softirq挂起位
 *
 * 【查询挂起】
 * 返回当前CPU上挂起的softirq位图。
 */
#define local_softirq_pending()	(__this_cpu_read(local_softirq_pending_ref))

/**
 * set_softirq_pending - 设置softirq挂起位
 * @x: 新的挂起位图
 *
 * 【覆盖设置】
 * 直接设置挂起位图，覆盖旧值。
 */
#define set_softirq_pending(x)	(__this_cpu_write(local_softirq_pending_ref, (x)))

/**
 * or_softirq_pending - 添加softirq挂起位
 * @x: 要添加的位
 *
 * 【按位或】
 * 将新的挂起位与现有位图进行OR操作，不清除已有位。
 */
#define or_softirq_pending(x)	(__this_cpu_or(local_softirq_pending_ref, (x)))

#endif /* local_softirq_pending */

/* Some architectures might implement lazy enabling/disabling of
 * interrupts. In some cases, such as stop_machine, we might want
 * to ensure that after a local_irq_disable(), interrupts have
 * really been disabled in hardware. Such architectures need to
 * implement the following hook.
 */
/**
 * 某些架构可能实现延迟启用/禁用中断。
 * 在某些情况下，如stop_machine，我们希望
 * 确保在local_irq_disable()之后，中断真的
 * 在硬件上被禁用了。这样的架构需要
 * 实现以下钩子。
 *
 * 【延迟禁用】
 * 某些架构为了性能，local_irq_disable()只设置软件标志，
 * 不立即操作硬件。hard_irq_disable()强制同步到硬件。
 */
#ifndef hard_irq_disable
/**
 * hard_irq_disable - 硬件级别禁用中断
 *
 * 【默认空实现】
 * 大多数架构不需要此钩子。
 */
#define hard_irq_disable()	do { } while(0)
#endif

/* PLEASE, avoid to allocate new softirqs, if you need not _really_ high
   frequency threaded job scheduling. For almost all the purposes
   tasklets are more than enough. F.e. all serial device BHs et
   al. should be converted to tasklets, not to softirqs.
 */
/**
 * 请注意，避免分配新的softirq，除非你_真的_需要高
 * 频率的线程作业调度。对于几乎所有目的，
 * tasklet已经足够了。例如，所有串行设备的BH等
 * 都应该转换为tasklet，而不是softirq。
 *
 * 【设计建议】
 * - softirq数量有限（最多NR_SOFTIRQS个），是全局资源
 * - softirq运行在所有CPU上，适合高频率、对延迟敏感的任务
 * - tasklet基于softirq实现，更灵活，适合大多数驱动
 * - 新代码应优先使用tasklet或工作队列
 */

enum
{
	HI_SOFTIRQ=0,        /* 高优先级tasklet */
	TIMER_SOFTIRQ,       /* 定时器软中断 */
	NET_TX_SOFTIRQ,      /* 网络发送 */
	NET_RX_SOFTIRQ,      /* 网络接收 */
	BLOCK_SOFTIRQ,       /* 块设备 */
	IRQ_POLL_SOFTIRQ,    /* IRQ轮询（块设备） */
	TASKLET_SOFTIRQ,     /* 普通tasklet */
	SCHED_SOFTIRQ,       /* 调度器 */
	HRTIMER_SOFTIRQ,     /* 高精度定时器 */
	RCU_SOFTIRQ,    /* Preferable RCU should always be the last softirq */
	                /* RCU软中断，最好总是最后一个 */

	NR_SOFTIRQS     /* softirq总数，当前为10 */
};

/*
 * The following vectors can be safely ignored after ksoftirqd is parked:
 *
 * _ RCU:
 * 	1) rcutree_migrate_callbacks() migrates the queue.
 * 	2) rcutree_report_cpu_dead() reports the final quiescent states.
 *
 * _ IRQ_POLL: irq_poll_cpu_dead() migrates the queue
 *
 * _ (HR)TIMER_SOFTIRQ: (hr)timers_dead_cpu() migrates the queue
 */
/**
 * ksoftirqd停止后可以安全忽略的向量：
 *
 * _ RCU:
 * 	1) rcutree_migrate_callbacks() 迁移队列。
 * 	2) rcutree_report_cpu_dead() 报告最终的静止状态。
 *
 * _ IRQ_POLL: irq_poll_cpu_dead() 迁移队列
 *
 * _ (HR)TIMER_SOFTIRQ: (hr)timers_dead_cpu() 迁移队列
 *
 * 【CPU热插拔】
 * 这些softirq在CPU下线时会被迁移到其他CPU，
 * 因此在ksoftirqd停止后可以安全忽略。
 */
#define SOFTIRQ_HOTPLUG_SAFE_MASK (BIT(TIMER_SOFTIRQ) | BIT(IRQ_POLL_SOFTIRQ) |\
				   BIT(HRTIMER_SOFTIRQ) | BIT(RCU_SOFTIRQ))


/* map softirq index to softirq name. update 'softirq_to_name' in
 * kernel/softirq.c when adding a new softirq.
 */
/**
 * softirq_to_name - softirq索引到名称的映射
 *
 * 【调试用】
 * 将softirq索引映射到名称字符串。添加新的softirq时，
 * 需要在kernel/softirq.c中更新'softirq_to_name'。
 */
extern const char * const softirq_to_name[NR_SOFTIRQS];

/* softirq mask and active fields moved to irq_cpustat_t in
 * asm/hardirq.h to get better cache usage.  KAO
 */
/**
 * softirq mask和active字段已移至asm/hardirq.h中的irq_cpustat_t，
 * 以获得更好的缓存使用。KAO
 *
 * 【性能优化】
 * 将每CPU的softirq状态放在irq_cpustat_t中，提高缓存局部性。
 */

/**
 * struct softirq_action - softirq动作描述符
 * @action: 要执行的函数指针
 *
 * 【回调函数】
 * 每个softirq类型对应一个action函数。
 * 注意：没有参数传递，函数必须知道如何找到自己的数据。
 */
struct softirq_action
{
	void	(*action)(void);
};

/**
 * do_softirq - 处理挂起的softirq
 *
 * 【主入口】
 * 检查并处理当前CPU上挂起的softirq。
 * 通常在中断返回路径上调用。
 */
asmlinkage void do_softirq(void);

/**
 * __do_softirq - 实际执行softirq处理（内部函数）
 *
 * 【核心实现】
 * do_softirq()的底层实现，执行挂起的softirq回调。
 */
asmlinkage void __do_softirq(void);

#ifdef CONFIG_PREEMPT_RT
/**
 * do_softirq_post_smp_call_flush - SMP调用后刷新softirq
 * @was_pending: 之前挂起的softirq位图
 *
 * 【实时内核】
 * PREEMPT_RT特殊处理，在SMP函数调用后处理softirq。
 */
extern void do_softirq_post_smp_call_flush(unsigned int was_pending);
#else
/**
 * do_softirq_post_smp_call_flush - 非实时内核的存根实现
 *
 * 【标准内核】
 * 直接调用do_softirq()。
 */
static inline void do_softirq_post_smp_call_flush(unsigned int unused)
{
	do_softirq();
}
#endif

/**
 * open_softirq - 注册softirq处理函数
 * @nr: softirq编号（HI_SOFTIRQ, TIMER_SOFTIRQ等）
 * @action: 处理函数
 *
 * 【注册回调】
 * 将处理函数注册到指定的softirq槽位。
 * 通常在内核初始化时调用。
 */
extern void open_softirq(int nr, void (*action)(void));

/**
 * softirq_init - 初始化softirq子系统
 *
 * 【启动初始化】
 * 在内核启动时初始化softirq机制。
 */
extern void softirq_init(void);

/**
 * __raise_softirq_irqoff - 触发softirq（中断已禁用）
 * @nr: softirq编号
 *
 * 【低级接口】
 * 直接设置softirq挂起位，不检查中断状态。
 * 调用者必须确保中断已禁用。
 */
extern void __raise_softirq_irqoff(unsigned int nr);

/**
 * raise_softirq_irqoff - 触发softirq（中断已禁用）
 * @nr: softirq编号
 *
 * 【常用接口】
 * 设置softirq挂起位并唤醒ksoftirqd（如果需要）。
 * 调用者必须确保中断已禁用。
 */
extern void raise_softirq_irqoff(unsigned int nr);

/**
 * raise_softirq - 触发softirq（安全版本）
 * @nr: softirq编号
 *
 * 【安全接口】
 * 自动禁用中断，设置softirq挂起位，然后恢复中断。
 * 可以在任何上下文中调用。
 */
extern void raise_softirq(unsigned int nr);

/*
 * With forced-threaded interrupts enabled a raised softirq is deferred to
 * ksoftirqd unless it can be handled within the threaded interrupt. This
 * affects timer_list timers and hrtimers which are explicitly marked with
 * HRTIMER_MODE_SOFT.
 * With PREEMPT_RT enabled more hrtimers are moved to softirq for processing
 * which includes all timers which are not explicitly marked HRTIMER_MODE_HARD.
 * Userspace controlled timers (like the clock_nanosleep() interface) is divided
 * into two categories: Tasks with elevated scheduling policy including
 * SCHED_{FIFO|RR|DL} and the remaining scheduling policy. The tasks with the
 * elevated scheduling policy are woken up directly from the HARDIRQ while all
 * other wake ups are delayed to softirq and so to ksoftirqd.
 *
 * The ksoftirqd runs at SCHED_OTHER policy at which it should remain since it
 * handles the softirq in an overloaded situation (not handled everything
 * within its last run).
 * If the timers are handled at SCHED_OTHER priority then they competes with all
 * other SCHED_OTHER tasks for CPU resources are possibly delayed.
 * Moving timers softirqs to a low priority SCHED_FIFO thread instead ensures
 * that timer are performed before scheduling any SCHED_OTHER thread.
 */
/**
 * 启用强制线程化中断时，触发的softirq会延迟到ksoftirqd，
 * 除非它可以在线程化中断中处理。这会影响timer_list定时器
 * 和显式标记为HRTIMER_MODE_SOFT的hrtimer。
 *
 * 启用PREEMPT_RT时，更多hrtimer被移到softirq处理，
 * 包括所有未显式标记为HRTIMER_MODE_HARD的定时器。
 *
 * 用户空间控制的定时器（如clock_nanosleep()接口）分为
 * 两类：具有提升调度策略的任务（包括SCHED_{FIFO|RR|DL}）
 * 和其余调度策略。具有提升调度策略的任务直接从HARDIRQ
 * 唤醒，而所有其他唤醒都延迟到softirq，因此到ksoftirqd。
 *
 * ksoftirqd以SCHED_OTHER策略运行，它应该保持这样，因为它
 * 在过载情况下处理softirq（在上次运行中未处理完所有内容）。
 *
 * 如果定时器以SCHED_OTHER优先级处理，那么它们会与所有
 * 其他SCHED_OTHER任务竞争CPU资源，可能会延迟。
 *
 * 将定时器softirq移到低优先级的SCHED_FIFO线程可以确保
 * 定时器在调度任何SCHED_OTHER线程之前执行。
 *
 * 【ktimerd线程】
 * PREEMPT_RT引入的专门处理定时器的线程，优先级高于ksoftirqd。
 */
DECLARE_PER_CPU(struct task_struct *, ktimerd);
DECLARE_PER_CPU(unsigned long, pending_timer_softirq);

/**
 * raise_ktimers_thread - 唤醒ktimerd线程
 * @nr: softirq编号
 *
 * 【实时内核】
 * 唤醒ktimerd线程处理定时器softirq。
 */
void raise_ktimers_thread(unsigned int nr);

/**
 * local_timers_pending_force_th - 检查挂起的定时器softirq
 *
 * 【强制线程化】
 * 返回当前CPU上挂起的定时器softirq位图。
 */
static inline unsigned int local_timers_pending_force_th(void)
{
	return __this_cpu_read(pending_timer_softirq);
}

/**
 * raise_timer_softirq - 触发定时器softirq
 * @nr: softirq编号
 *
 * 【智能分发】
 * 根据force_irqthreads()的值决定：
 * - 强制线程化：唤醒ktimerd线程
 * - 正常模式：直接触发softirq
 *
 * 必须在中断上下文中调用（lockdep检查）。
 */
static inline void raise_timer_softirq(unsigned int nr)
{
	lockdep_assert_in_irq();
	if (force_irqthreads())
		raise_ktimers_thread(nr);
	else
		__raise_softirq_irqoff(nr);
}

/**
 * local_timers_pending - 检查挂起的定时器
 *
 * 【统一接口】
 * 根据force_irqthreads()的值，返回挂起的定时器位图。
 */
static inline unsigned int local_timers_pending(void)
{
	if (force_irqthreads())
		return local_timers_pending_force_th();
	else
		return local_softirq_pending();
}

/**
 * ksoftirqd - 每CPU的softirq守护线程
 *
 * 【背景线程】
 * 当softirq过载时（处理时间过长或频率过高），
 * 将剩余的softirq工作交给ksoftirqd线程处理。
 * 避免长时间禁用中断。
 */
DECLARE_PER_CPU(struct task_struct *, ksoftirqd);

/**
 * this_cpu_ksoftirqd - 获取当前CPU的ksoftirqd线程
 *
 * 返回：当前CPU的ksoftirqd任务指针
 */
static inline struct task_struct *this_cpu_ksoftirqd(void)
{
	return this_cpu_read(ksoftirqd);
}

/* Tasklets --- multithreaded analogue of BHs.

   This API is deprecated. Please consider using threaded IRQs instead:
   https://lore.kernel.org/lkml/20200716081538.2sivhkj4hcyrusem@linutronix.de

   Main feature differing them of generic softirqs: tasklet
   is running only on one CPU simultaneously.

   Main feature differing them of BHs: different tasklets
   may be run simultaneously on different CPUs.

   Properties:
   * If tasklet_schedule() is called, then tasklet is guaranteed
     to be executed on some cpu at least once after this.
   * If the tasklet is already scheduled, but its execution is still not
     started, it will be executed only once.
   * If this tasklet is already running on another CPU (or schedule is called
     from tasklet itself), it is rescheduled for later.
   * Tasklet is strictly serialized wrt itself, but not
     wrt another tasklets. If client needs some intertask synchronization,
     he makes it with spinlocks.
 */
/**
 * Tasklet --- BH的多线程类比。
 *
 * 此API已弃用。请考虑改用线程化IRQ：
 * https://lore.kernel.org/lkml/20200716081538.2sivhkj4hcyrusem@linutronix.de
 *
 * 与通用softirq的主要区别：tasklet
 * 同时只在一个CPU上运行。
 *
 * 与BH的主要区别：不同的tasklet
 * 可以在不同的CPU上同时运行。
 *
 * 属性：
 * * 如果调用tasklet_schedule()，则保证tasklet
 *   在此之后至少在某个CPU上执行一次。
 * * 如果tasklet已经被调度，但其执行尚未
 *   开始，它将只执行一次。
 * * 如果此tasklet已经在另一个CPU上运行（或从tasklet本身调用schedule），
 *   它会被重新调度以便稍后执行。
 * * Tasklet相对于自身是严格串行化的，但不
 *   相对于其他tasklet。如果客户端需要任务间同步，
 *   他用自旋锁来实现。
 *
 * 【使用建议】
 * 新代码应使用线程化中断（threaded IRQ）代替tasklet。
 * Tasklet是遗留API，仅为兼容性保留。
 */

/**
 * struct tasklet_struct - tasklet描述符
 * @next: 链表指针，用于tasklet队列
 * @state: tasklet状态（TASKLET_STATE_SCHED/RUN）
 * @count: 引用计数，0表示启用，非0表示禁用
 * @use_callback: true表示使用callback，false表示使用func
 * @func: 旧式回调函数（带unsigned long参数）
 * @callback: 新式回调函数（带tasklet_struct指针）
 * @data: 传递给func的参数（callback不使用）
 *
 * 【两种回调】
 * - 旧式：void (*func)(unsigned long data)
 * - 新式：void (*callback)(struct tasklet_struct *t)
 * 新代码应使用callback，可以通过container_of访问上下文。
 */
struct tasklet_struct
{
	struct tasklet_struct *next;
	unsigned long state;
	atomic_t count;
	bool use_callback;
	union {
		void (*func)(unsigned long data);
		void (*callback)(struct tasklet_struct *t);
	};
	unsigned long data;
};

/**
 * DECLARE_TASKLET - 声明并初始化一个tasklet（启用状态）
 * @name: tasklet变量名
 * @_callback: 回调函数
 *
 * 【静态初始化】
 * 创建一个启用的tasklet，使用新式callback接口。
 *
 * 示例：
 * void my_callback(struct tasklet_struct *t) { ... }
 * DECLARE_TASKLET(my_tasklet, my_callback);
 */
#define DECLARE_TASKLET(name, _callback)		\
struct tasklet_struct name = {				\
	.count = ATOMIC_INIT(0),			\
	.callback = _callback,				\
	.use_callback = true,				\
}

/**
 * DECLARE_TASKLET_DISABLED - 声明并初始化一个tasklet（禁用状态）
 * @name: tasklet变量名
 * @_callback: 回调函数
 *
 * 【禁用初始化】
 * 创建一个禁用的tasklet（count=1），需要调用tasklet_enable()启用。
 */
#define DECLARE_TASKLET_DISABLED(name, _callback)	\
struct tasklet_struct name = {				\
	.count = ATOMIC_INIT(1),			\
	.callback = _callback,				\
	.use_callback = true,				\
}

/**
 * from_tasklet - 从tasklet指针获取包含它的结构
 * @var: 指向包含结构的指针（类型推断用）
 * @callback_tasklet: 传递给回调的tasklet指针
 * @tasklet_fieldname: tasklet在包含结构中的字段名
 *
 * 【容器获取】
 * 类似container_of，从tasklet_struct指针反向获取包含它的结构。
 *
 * 示例：
 * struct my_data {
 *     struct tasklet_struct task;
 *     int value;
 * };
 * void my_callback(struct tasklet_struct *t) {
 *     struct my_data *data = from_tasklet(data, t, task);
 *     // 现在可以访问data->value
 * }
 */
#define from_tasklet(var, callback_tasklet, tasklet_fieldname)	\
	container_of(callback_tasklet, typeof(*var), tasklet_fieldname)

/**
 * DECLARE_TASKLET_OLD - 声明旧式tasklet（已弃用）
 * @name: tasklet变量名
 * @_func: 旧式回调函数
 *
 * 【遗留接口】
 * 仅为兼容旧代码，新代码应使用DECLARE_TASKLET。
 */
#define DECLARE_TASKLET_OLD(name, _func)		\
struct tasklet_struct name = {				\
	.count = ATOMIC_INIT(0),			\
	.func = _func,					\
}

/**
 * DECLARE_TASKLET_DISABLED_OLD - 声明禁用的旧式tasklet（已弃用）
 * @name: tasklet变量名
 * @_func: 旧式回调函数
 *
 * 【遗留接口】
 * 仅为兼容旧代码，新代码应使用DECLARE_TASKLET_DISABLED。
 */
#define DECLARE_TASKLET_DISABLED_OLD(name, _func)	\
struct tasklet_struct name = {				\
	.count = ATOMIC_INIT(1),			\
	.func = _func,					\
}

/**
 * tasklet状态位
 */
enum
{
	TASKLET_STATE_SCHED,	/* Tasklet is scheduled for execution */
	                        /* tasklet已被调度等待执行 */
	TASKLET_STATE_RUN	/* Tasklet is running (SMP only) */
	                        /* tasklet正在运行（仅SMP） */
};

#if defined(CONFIG_SMP) || defined(CONFIG_PREEMPT_RT)
/**
 * tasklet_trylock - 尝试获取tasklet锁
 * @t: tasklet指针
 *
 * 【并发控制】
 * 尝试设置TASKLET_STATE_RUN位，确保tasklet同时只在一个CPU上运行。
 *
 * 返回：成功返回1（获取锁），失败返回0（已在其他CPU运行）
 */
static inline int tasklet_trylock(struct tasklet_struct *t)
{
	return !test_and_set_bit(TASKLET_STATE_RUN, &(t)->state);
}

/**
 * tasklet_unlock - 释放tasklet锁
 * @t: tasklet指针
 *
 * 【解锁】
 * 清除TASKLET_STATE_RUN位。
 */
void tasklet_unlock(struct tasklet_struct *t);

/**
 * tasklet_unlock_wait - 等待tasklet解锁
 * @t: tasklet指针
 *
 * 【可睡眠等待】
 * 阻塞等待直到tasklet不再运行。可能睡眠。
 */
void tasklet_unlock_wait(struct tasklet_struct *t);

/**
 * tasklet_unlock_spin_wait - 自旋等待tasklet解锁
 * @t: tasklet指针
 *
 * 【忙等待】
 * 自旋等待直到tasklet不再运行。不能睡眠，用于原子上下文。
 */
void tasklet_unlock_spin_wait(struct tasklet_struct *t);

#else
/**
 * 单处理器或非PREEMPT_RT的存根实现
 * 单CPU系统不需要锁机制
 */
static inline int tasklet_trylock(struct tasklet_struct *t) { return 1; }
static inline void tasklet_unlock(struct tasklet_struct *t) { }
static inline void tasklet_unlock_wait(struct tasklet_struct *t) { }
static inline void tasklet_unlock_spin_wait(struct tasklet_struct *t) { }
#endif

/**
 * __tasklet_schedule - 调度tasklet（内部函数）
 * @t: tasklet指针
 *
 * 【低级接口】
 * 将tasklet添加到当前CPU的tasklet队列。
 */
extern void __tasklet_schedule(struct tasklet_struct *t);

/**
 * tasklet_schedule - 调度tasklet执行
 * @t: tasklet指针
 *
 * 【标准调度】
 * 将tasklet加入TASKLET_SOFTIRQ队列，稍后在softirq上下文执行。
 * 如果tasklet已调度，不会重复添加。
 */
static inline void tasklet_schedule(struct tasklet_struct *t)
{
	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
		__tasklet_schedule(t);
}

/**
 * __tasklet_hi_schedule - 调度高优先级tasklet（内部函数）
 * @t: tasklet指针
 *
 * 【低级接口】
 * 将tasklet添加到高优先级队列。
 */
extern void __tasklet_hi_schedule(struct tasklet_struct *t);

/**
 * tasklet_hi_schedule - 调度高优先级tasklet
 * @t: tasklet指针
 *
 * 【高优先级】
 * 将tasklet加入HI_SOFTIRQ队列，比TASKLET_SOFTIRQ先执行。
 * 用于时延敏感的任务。
 */
static inline void tasklet_hi_schedule(struct tasklet_struct *t)
{
	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
		__tasklet_hi_schedule(t);
}

/**
 * tasklet_disable_nosync - 禁用tasklet（不等待）
 * @t: tasklet指针
 *
 * 【异步禁用】
 * 增加count计数，阻止tasklet执行，但不等待正在运行的实例完成。
 * 适合中断上下文。
 */
static inline void tasklet_disable_nosync(struct tasklet_struct *t)
{
	atomic_inc(&t->count);
	smp_mb__after_atomic();
}

/*
 * Do not use in new code. Disabling tasklets from atomic contexts is
 * error prone and should be avoided.
 */
/**
 * 不要在新代码中使用。在原子上下文中禁用tasklet
 * 容易出错，应该避免。
 */
/**
 * tasklet_disable_in_atomic - 在原子上下文禁用tasklet（已弃用）
 * @t: tasklet指针
 *
 * 【已弃用】
 * 禁用tasklet并自旋等待完成。仅用于遗留代码，新代码禁止使用。
 */
static inline void tasklet_disable_in_atomic(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_spin_wait(t);
	smp_mb();
}

/**
 * tasklet_disable - 禁用tasklet并等待完成
 * @t: tasklet指针
 *
 * 【同步禁用】
 * 禁用tasklet并等待正在运行的实例完成。可能睡眠。
 * 必须在进程上下文调用。
 */
static inline void tasklet_disable(struct tasklet_struct *t)
{
	tasklet_disable_nosync(t);
	tasklet_unlock_wait(t);
	smp_mb();
}

/**
 * tasklet_enable - 启用tasklet
 * @t: tasklet指针
 *
 * 【启用】
 * 减少count计数。如果count降到0，tasklet变为可运行。
 * 必须与tasklet_disable成对使用。
 */
static inline void tasklet_enable(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	atomic_dec(&t->count);
}

/**
 * tasklet_kill - 终止tasklet
 * @t: tasklet指针
 *
 * 【安全清理】
 * 等待tasklet完成并确保不会再次调度。必须在释放tasklet前调用。
 * 可能睡眠，必须在进程上下文调用。
 */
extern void tasklet_kill(struct tasklet_struct *t);

/**
 * tasklet_init - 初始化旧式tasklet（已弃用）
 * @t: tasklet指针
 * @func: 旧式回调函数
 * @data: 传递给回调的data参数
 *
 * 【遗留接口】
 * 初始化使用旧式回调的tasklet。新代码应使用tasklet_setup。
 */
extern void tasklet_init(struct tasklet_struct *t,
			 void (*func)(unsigned long), unsigned long data);

/**
 * tasklet_setup - 初始化新式tasklet
 * @t: tasklet指针
 * @callback: 新式回调函数
 *
 * 【推荐接口】
 * 初始化使用新式回调的tasklet。回调接收tasklet指针，
 * 可以通过from_tasklet获取包含结构。
 */
extern void tasklet_setup(struct tasklet_struct *t,
			  void (*callback)(struct tasklet_struct *));

/*
 * Autoprobing for irqs:
 *
 * probe_irq_on() and probe_irq_off() provide robust primitives
 * for accurate IRQ probing during kernel initialization.  They are
 * reasonably simple to use, are not "fooled" by spurious interrupts,
 * and, unlike other attempts at IRQ probing, they do not get hung on
 * stuck interrupts (such as unused PS2 mouse interfaces on ASUS boards).
 *
 * For reasonably foolproof probing, use them as follows:
 *
 * 1. clear and/or mask the device's internal interrupt.
 * 2. sti();
 * 3. irqs = probe_irq_on();      // "take over" all unassigned idle IRQs
 * 4. enable the device and cause it to trigger an interrupt.
 * 5. wait for the device to interrupt, using non-intrusive polling or a delay.
 * 6. irq = probe_irq_off(irqs);  // get IRQ number, 0=none, negative=multiple
 * 7. service the device to clear its pending interrupt.
 * 8. loop again if paranoia is required.
 *
 * probe_irq_on() returns a mask of allocated irq's.
 *
 * probe_irq_off() takes the mask as a parameter,
 * and returns the irq number which occurred,
 * or zero if none occurred, or a negative irq number
 * if more than one irq occurred.
 */
/**
 * 【中断自动探测】
 *
 * probe_irq_on() 和 probe_irq_off() 提供健壮的原语，
 * 用于内核初始化期间准确的IRQ探测。它们：
 * - 使用简单合理
 * - 不会被伪中断"欺骗"
 * - 与其他IRQ探测尝试不同，不会挂在卡死的中断上
 *   （例如华硕主板上未使用的PS2鼠标接口）
 *
 * 【可靠探测的使用方法】
 *
 * 1. 清除和/或屏蔽设备的内部中断
 * 2. sti();  // 启用中断
 * 3. irqs = probe_irq_on();      // "接管"所有未分配的空闲IRQ
 * 4. 启用设备并使其触发中断
 * 5. 等待设备中断，使用非侵入式轮询或延迟
 * 6. irq = probe_irq_off(irqs);  // 获取IRQ号，0=无，负数=多个
 * 7. 服务设备以清除其挂起的中断
 * 8. 如果需要更加谨慎，再次循环
 *
 * 【返回值】
 * probe_irq_on(): 返回已分配irq的掩码
 * probe_irq_off(mask):
 *   - 正数：发生的irq号
 *   - 0：未发生中断
 *   - 负数：发生多个中断
 */

#if !defined(CONFIG_GENERIC_IRQ_PROBE)
/**
 * 未启用通用IRQ探测时的存根实现
 */
static inline unsigned long probe_irq_on(void)
{
	return 0;
}
static inline int probe_irq_off(unsigned long val)
{
	return 0;
}
static inline unsigned int probe_irq_mask(unsigned long val)
{
	return 0;
}
#else
/**
 * probe_irq_on - 开始IRQ探测
 *
 * 返回值：已分配irq的掩码，失败返回0
 *
 * 【功能】
 * "接管"所有未分配的空闲IRQ，为探测做准备
 */
extern unsigned long probe_irq_on(void);	/* returns 0 on failure */

/**
 * probe_irq_off - 结束IRQ探测
 * @val: probe_irq_on返回的掩码
 *
 * 返回值：
 *   - 正数：检测到的IRQ号
 *   - 0：未检测到中断
 *   - 负数：检测到多个中断
 */
extern int probe_irq_off(unsigned long);	/* returns 0 or negative on failure */

/**
 * probe_irq_mask - 获取ISA中断掩码
 * @val: probe_irq_on返回的掩码
 *
 * 返回值：ISA中断的掩码
 */
extern unsigned int probe_irq_mask(unsigned long);	/* returns mask of ISA interrupts */
#endif

#ifdef CONFIG_PROC_FS
/* Initialize /proc/irq/ */
/**
 * init_irq_proc - 初始化/proc/irq/目录
 *
 * 创建/proc/irq/下的各种文件，用于查看和配置中断信息
 */
extern void init_irq_proc(void);
#else
static inline void init_irq_proc(void)
{
}
#endif

struct seq_file;
/**
 * show_interrupts - 显示中断统计信息
 * @p: seq_file指针
 * @v: 迭代器值
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【用途】
 * 用于/proc/interrupts文件，显示每个CPU上各中断的计数
 */
int show_interrupts(struct seq_file *p, void *v);

/**
 * arch_show_interrupts - 显示架构特定的中断信息
 * @p: seq_file指针
 * @prec: 精度（列宽）
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【架构相关】
 * 各CPU架构可以实现此函数显示特定的中断信息
 */
int arch_show_interrupts(struct seq_file *p, int prec);

/**
 * irq_proc_emit_counts - 输出per-CPU中断计数
 * @p: seq_file指针
 * @cnts: per-CPU计数器数组
 *
 * 【辅助函数】
 * 格式化输出每个CPU的中断计数
 */
void irq_proc_emit_counts(struct seq_file *p, unsigned int __percpu *cnts);

/**
 * early_irq_init - 早期中断初始化
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【启动阶段】
 * 在内核启动早期初始化中断子系统的基本数据结构
 */
extern int early_irq_init(void);

/**
 * arch_probe_nr_irqs - 探测架构支持的IRQ数量
 *
 * 返回值：IRQ数量
 *
 * 【架构相关】
 * 各架构实现此函数返回其支持的IRQ数量
 */
extern int arch_probe_nr_irqs(void);

/**
 * arch_early_irq_init - 架构特定的早期中断初始化
 *
 * 返回值：0=成功，负数=错误码
 *
 * 【架构相关】
 * 各架构可以实现此函数进行特定的早期中断设置
 */
extern int arch_early_irq_init(void);

/*
 * We want to know which function is an entrypoint of a hardirq or a softirq.
 */
/**
 * 我们想知道哪个函数是硬中断或软中断的入口点
 *
 * 【代码段标记】
 * 这些宏用于将中断入口函数放置到特定的代码段，
 * 方便性能分析工具（如perf）识别中断处理代码
 */
#ifndef __irq_entry
/**
 * __irq_entry - 标记硬中断入口函数
 *
 * 将函数放入.irqentry.text段
 */
# define __irq_entry	 __section(".irqentry.text")
#endif

/**
 * __softirq_entry - 标记软中断入口函数
 *
 * 将函数放入.softirqentry.text段
 */
#define __softirq_entry  __section(".softirqentry.text")

#endif
