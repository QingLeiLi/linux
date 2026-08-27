// SPDX-License-Identifier: GPL-2.0

#include <linux/error-injection.h>
#include <linux/kprobes.h>

void override_function_with_return(struct pt_regs *regs)
{
	/*
	 * 'regs' represents the state on entry of a predefined function in
	 * the kernel/module and which is captured on a kprobe.
	 *
	 * When kprobe returns back from exception it will override the end
	 * of probed function and directly return to the predefined
	 * function's caller.
	 */
	/*
	 * AArch64 的 x30/LR 保存“被探测函数返回后应继续执行的地址”。把异常
	 * 保存现场的 PC 直接改成 LR，异常返回就像函数立刻执行了 ret。这里只
	 * 改控制流；具体返回值须由错误注入框架按目标函数约定另行准备。
	 */
	instruction_pointer_set(regs, procedure_link_pointer(regs));
}
/* 防止 kprobe 再探测自身，造成递归异常。 */
NOKPROBE_SYMBOL(override_function_with_return);
