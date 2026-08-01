/* SPDX-License-Identifier: GPL-2.0 */
/*
 * arm64 启动阶段栈完整性保护学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 文件职责：为 arm64 实现通用 <linux/stackprotector.h> 所要求的
 * boot_init_stack_canary()。这个入口在 boot CPU 的 start_kernel() 中、随机数
 * 子系统完成早期初始化后调用，一次性完成两组“从此可以安全使用”的启动状态：
 *
 *   start_kernel()
 *     -> boot_init_stack_canary()
 *          -> 为 init_task 写入栈 canary
 *          -> 必要时发布全局 __stack_chk_guard
 *          -> 为 init_task 生成并装入内核指针认证密钥
 *          -> 在硬件支持时使能 EL1 指针认证
 *     -> 继续内核初始化，最终永久进入 boot idle
 *
 * 职责边界：这里只初始化启动任务。后续新任务的 stack_canary 由
 * dup_task_struct() 生成，内核 PAC key 由 copy_thread() 生成；任务切换时，
 * arm64 的 cpu_switch_to() 会同时切换 sp_el0/current 和相应的内核 PAC key。
 * 本文件不负责决定哪些函数由编译器插入栈保护，也不负责检测失败后的 panic
 * 路径；这些分别属于编译选项和编译器生成的 __stack_chk_fail() 调用。
 *
 * 并发与生命周期：调用发生在单个 boot task、尚未开启常规调度并发的阶段，
 * 因而不需要锁或引用计数。写入 task_struct 的值随任务存活；全局回退值在启动
 * 后只读；PAC key 则在每次任务切换时装入 CPU 系统寄存器。这里没有资源获取、
 * 失败回滚或可恢复错误。
 *
 * 方案权衡：每任务 canary 能限制一次信息泄露影响所有任务，但要求编译器支持
 * 从 sp_el0 所指 task_struct 的固定偏移取 guard。工具链不支持时使用全局 guard，
 * 兼容性更好但所有 CPU/任务共享同一个秘密。PAC 保护返回地址等控制流数据，
 * 与检测栈上局部对象越界的 canary 互补，不能互相替代。
 */
/*
 * GCC stack protector support.
 *
 * Stack protector works by putting predefined pattern at the start of
 * the stack frame and verifying that it hasn't been overwritten when
 * returning from the function.  The pattern is called stack canary
 * and gcc expects it to be defined by a global variable called
 * "__stack_chk_guard" on ARM.  This unfortunately means that on SMP
 * we cannot have a different canary value per task.
 */
/*
 * GCC 栈保护器会在受保护函数的栈帧中保存一个预设模式，并在函数返回前确认
 * 它没有被覆盖；该模式称为 stack canary。传统 ARM ABI 让 GCC 从名为
 * __stack_chk_guard 的全局变量取得 canary，因此所有 CPU 无法直接为每个任务
 * 使用不同值：若某处泄露全局值，其他任务的同类保护也会随之削弱。
 *
 * 修正说明：上面的限制只描述全局 guard 模式。当前 arm64 在
 * CONFIG_STACKPROTECTOR_PER_TASK=y 时，由 arch/arm64/Makefile 传入编译器的
 * sysreg guard 参数，使函数从 sp_el0（内核态的 current 指针）加上
 * task_struct::stack_canary 的生成偏移取值，从而真正使用每任务 canary；仅在
 * 工具链缺少该能力时才回退到全局 __stack_chk_guard。
 */

#ifndef __ASM_STACKPROTECTOR_H
#define __ASM_STACKPROTECTOR_H

/*
 * 头文件保护宏保证本定义在同一翻译单元中只展开一次；它只解决重复声明/定义，
 * 不参与运行时的栈保护状态。宏名带双下划线是体系结构内部头文件的命名约定。
 */

#include <asm/pointer_auth.h>

/*
 * pointer_auth.h 提供以下 PAC 初始化、密钥装载和硬件使能接口。即使关闭
 * CONFIG_STACKPROTECTOR，只要启用 ARM64 指针认证，通用栈保护头仍会包含本文件，
 * 因而 boot_init_stack_canary() 仍需完成启动任务的 PAC 初始化。
 */

extern unsigned long __stack_chk_guard;

/*
 * __stack_chk_guard 是编译器传统全局 guard ABI 所读取的机器字。
 * CONFIG_STACKPROTECTOR=y 且未启用 STACKPROTECTOR_PER_TASK 时，它在
 * arch/arm64/kernel/process.c 中定义、导出并标记为 __ro_after_init：本函数在
 * 启动期写一次，之后页属性转为只读。启用每任务模式时编译器不引用这个符号，
 * task_struct::stack_canary 才是实际数据源；这里的 extern 不持有任何所有权。
 */

/*
 * Initialize the stackprotector canary value.
 *
 * NOTE: this must only be called from functions that never return,
 * and it must always be inlined.
 */
/*
 * 初始化启动任务的栈 canary 和内核指针认证状态。
 *
 * 调用关系：唯一直接调用者是 start_kernel()。调用前 current 指向静态 init_task，
 * random_init() 已执行，常规调度尚未开始且中断仍关闭；调用者未持有本函数要求的
 * 锁。路径只读取随机数状态、写当前任务字段和系统寄存器，不分配内存、不睡眠。
 *
 * 入参：无。返回：无直接返回值，也没有错误码或输出参数。可观察副作用是：
 *   1. CONFIG_STACKPROTECTOR 下初始化 current->stack_canary；
 *   2. 全局 guard 模式下同步初始化 __stack_chk_guard；
 *   3. CONFIG_ARM64_PTR_AUTH_KERNEL 下生成、保存并装载 init_task 的 APIAKey；
 *   4. CPU 支持地址认证时设置 SCTLR_EL1 使能位，后续内核 PAC 指令开始生效。
 * 关闭相应配置或硬件不支持时，相关 helper 编译为空操作，不构成失败。
 *
 * 原英文警告要求只能从“永不返回”的函数调用，并且必须内联。原因不只是减少
 * 调用开销：本函数会在仍存活的调用链中更换 canary 和 PAC key。若形成独立栈帧
 * 后再返回，该帧入口保存的旧 canary，或用旧 APIAKey 签名的返回地址，可能无法
 * 通过用新状态执行的尾声检查。内联把状态切换并入 start_kernel()；后者自身禁用
 * 栈保护并最终进入 idle 而不返回，从控制流上消除了跨越切换点返回旧栈帧的问题。
 */
static __always_inline void boot_init_stack_canary(void)
{
#if defined(CONFIG_STACKPROTECTOR)
	/*
	 * get_random_canary() 从内核随机数接口取得一个 unsigned long，并按
	 * CANARY_MASK 把内存中的首字节清零。保留 NUL 字节可使某些未终止字符串越界
	 * 在到达 canary 时停止；arm64 仍保留其余 56 位随机性。canary 是本阶段的
	 * 临时值，没有引用或释放责任。
	 */
	unsigned long canary = get_random_canary();

	/*
	 * 无论编译器最终采用哪种取值方式，都先把启动任务自己的字段初始化完整。
	 * 在每任务模式下，编译器通过 sp_el0/current 直接读取此字段；在全局模式下，
	 * 它还作为下面全局值的唯一初始化来源，使两处在启动任务上保持相同。
	 */
	current->stack_canary = canary;
	/*
	 * IS_ENABLED() 让配置关闭分支保持 C 语法可检查，同时被编译器折叠为常量。
	 * 每任务模式不写全局符号，避免制造一个看似有效却不会被函数序言读取的共享
	 * guard；回退模式则必须在任何受保护函数执行前完成这次启动期发布。
	 */
	if (!IS_ENABLED(CONFIG_STACKPROTECTOR_PER_TASK))
		__stack_chk_guard = current->stack_canary;
#endif
	/*
	 * 栈保护配置分支到此结束；下面的 PAC 初始化有自己的配置桩，因此即使只启用
	 * CONFIG_ARM64_PTR_AUTH、完全关闭 STACKPROTECTOR，也必须继续执行。
	 */

	/*
	 * 阶段 2：建立启动任务的内核 PAC 身份。
	 *
	 * init helper 在 CPU 支持地址认证时为 current->thread.keys_kernel.apia 填入
	 * 128 位随机密钥；switch helper 随即把它写入 APIAKeyLo/Hi_EL1，并用 ISB
	 * 保证后续指令在新密钥可见后执行。密钥归 init_task 的 thread 上下文所有，
	 * cpu_switch_to() 会为后续任务装入各自密钥。
	 */
	ptrauth_thread_init_kernel(current);
	ptrauth_thread_switch_kernel(current);
	/*
	 * 最后设置 SCTLR_EL1 的地址认证使能位并执行 ISB。先生成和装载密钥、再允许
	 * PAC 指令使用它，可避免硬件在未建立任务密钥时进入启用状态；不支持该特性
	 * 的 CPU 会在 helper 内直接返回，配置关闭时三个调用都编译为空操作。
	 */
	ptrauth_enable();
}

#endif	/* _ASM_STACKPROTECTOR_H */
/*
 * 上一行结束本文件的 include guard；原注释中的名称是该保护宏的简写写法，
 * 实际与文件开头的 #ifndef/#define 配对，重复包含时会跳过全部声明和内联定义。
 */
