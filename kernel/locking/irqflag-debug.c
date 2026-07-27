// SPDX-License-Identifier: GPL-2.0-only
/*
 * IRQ flags restore 调试学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 本文件只实现 CONFIG_DEBUG_IRQFLAGS 下的错误报告慢路径。通用
 * raw_local_irq_restore() 在恢复快照前发现当前硬件 IRQ 已经开启时，
 * 调用这里产生一次警告；真正的架构 restore 随后仍会继续执行。
 *
 * 函数必须可从 noinstr/IRQ 底层路径进入，所以显式用
 * instrumentation_begin/end 划出允许执行 WARN_ONCE instrumentation 的
 * 小窗口。它不修复 lockdep 状态、不关闭 IRQ，也不拥有任何资源。
 */

#include <linux/bug.h>
#include <linux/export.h>
#include <linux/irqflags.h>

/*
 * warn_bogus_irq_restore() - 报告 save/restore 配对期间 IRQ 被提前打开。
 *
 * 调用者是 irqflags.h 的 raw_check_bogus_irq_restore()，入口硬件状态已
 * 被确认是 IRQ-on。noinstr 阻止编译器在函数普通区域插入 tracing/KASAN
 * 等 instrumentation；警告调用被严格包在 instrumentation_begin/end
 * 之间，避免递归破坏正在诊断的 IRQ 跟踪路径。
 *
 * WARN_ONCE 只输出首个同类警告以限制故障风暴，函数无返回值且不改变
 * IRQ flags；调用者在返回后仍负责执行 arch_local_irq_restore(flags)。
 */
noinstr void warn_bogus_irq_restore(void)
{
	/* 只在显式开放窗口内调用可能被插桩的告警设施。 */
	instrumentation_begin();
	WARN_ONCE(1, "raw_local_irq_restore() called with IRQs enabled\n");
	instrumentation_end();
}
/* 供 irqflags 通用包装以及可加载代码引用该调试入口。 */
EXPORT_SYMBOL(warn_bogus_irq_restore);
