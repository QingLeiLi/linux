/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Debugging printout:
 *
 * 中断描述符的调试输出辅助代码。
 */

/*
 * ___P() 检查由 settings.h 封装的持久策略位，___PS() 检查 desc->istate 中的运行时
 * 内部状态位；只有置位项才以宏参数的名字输出。两者展开后依赖调用者作用域中的 desc，
 * 因而只供下面的 print_irq_desc() 使用，文件末尾会立即取消定义以免污染其他源码。
 */
#define ___P(f) if (desc->status_use_accessors & f) printk("%14s set\n", #f)
#define ___PS(f) if (desc->istate & f) printk("%14s set\n", #f)
/* FIXME */
/* 这些状态已移出 desc->istate，当前调试输出尚未改为从 irq_data 读取。 */
/*
 * 参数 f 故意不出现在替换文本中，所以 IRQS_INPROGRESS/IRQS_DISABLED/IRQS_MASKED
 * 即使已不再定义也不会被展开或求值；三个调用目前只保留待修复位置，不产生输出。
 */
#define ___PD(f) do { } while (0)

/*
 * 打印一次 IRQ 描述符快照。irq 是用于日志标识的逻辑号，desc 必须为调用期间存活的
 * 非 NULL 借用指针；函数不取得引用、不修改描述符，也不睡眠，可从坏中断的 hardirq
 * 路径调用。调用者是否持有 desc->lock 并不统一，因此各字段只是诊断快照，不能据此
 * 推导跨行一致状态。
 *
 * 该 static inline 在每个包含它的编译单元中各有一份静态 ratelimit；同一编译单元内
 * 所有 IRQ 共享“每 5 秒最多 5 次完整报告”的额度，超限时直接返回，避免坏中断风暴
 * 持续淹没日志。%p 输出遵循内核指针限制，%pS 尝试解析符号；action 为空时不会解引用
 * handler。
 */
static inline void print_irq_desc(unsigned int irq, struct irq_desc *desc)
{
	/* 本编译单元共享的速率状态先于任何 desc 字段读取，受限调用没有部分日志。 */
	static DEFINE_RATELIMIT_STATE(ratelimit, 5 * HZ, 5);

	if (!__ratelimit(&ratelimit))
		return;

	printk("irq %d, desc: %p, depth: %d, count: %d, unhandled: %d\n",
		irq, desc, desc->depth, desc->irq_count, desc->irqs_unhandled);
	printk("->handle_irq():  %p, %pS\n",
		desc->handle_irq, desc->handle_irq);
	printk("->irq_data.chip(): %p, %pS\n",
		desc->irq_data.chip, desc->irq_data.chip);
	printk("->action(): %p\n", desc->action);
	if (desc->action) {
		printk("->action->handler(): %p, %pS\n",
			desc->action->handler, desc->action->handler);
	}

	/* 输出影响流处理、申请、探测、线程化和自动启动策略的持久设置位。 */
	___P(IRQ_LEVEL);
	___P(IRQ_PER_CPU);
	___P(IRQ_NOPROBE);
	___P(IRQ_NOREQUEST);
	___P(IRQ_NOTHREAD);
	___P(IRQ_NOAUTOEN);

	/* 输出仍存放在 istate 中的自动探测、重放、等待和 pending 状态。 */
	___PS(IRQS_AUTODETECT);
	___PS(IRQS_REPLAY);
	___PS(IRQS_WAITING);
	___PS(IRQS_PENDING);

	/* 旧状态名只标记缺失的 irq_data 调试项；___PD() 当前全部吞掉。 */
	___PD(IRQS_INPROGRESS);
	___PD(IRQS_DISABLED);
	___PD(IRQS_MASKED);
}

/* 三个宏只服务于本头文件函数体，避免短名字泄漏到其余 IRQ 核心实现。 */
#undef ___P
#undef ___PS
#undef ___PD
