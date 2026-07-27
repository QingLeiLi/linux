// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * 内核异常表入口与可执行文本地址判定学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5.4（2026-07-27）。
 *
 * 本文件承担两组彼此关联的公共职责：
 *
 *   1. 启动时整理内建 __ex_table，并按
 *      “内建内核 -> 模块 -> BPF JIT”顺序查找与故障指令地址匹配的
 *      异常表项。体系结构异常处理代码随后
 *      解释表项并改写 pt_regs，真正完成跳转 fixup、返回 -EFAULT 等动作；
 *      本文件只负责找到表项，不解释架构专有编码。
 *   2. 判断一个地址是否属于仍然有效的内核代码，包括内建 .text、
 *      尚未释放的 .init.text、模块文本、ftrace/kprobe/BPF 动态代码。
 *      栈回溯、动态打补丁和调试校验据此区分代码地址与普通数据。
 *
 * 主调用链：
 *
 *   start_kernel()
 *     -> sort_main_extable()
 *          必要时按故障指令地址排序内建异常表
 *
 *   体系结构 fault/trap handler
 *     -> search_exception_tables(fault_ip)
 *          -> 内建表 -> 模块表 -> BPF JIT 表
 *     -> arch fixup_exception()
 *          找到：改写异常现场并恢复；找不到：继续普通 fault/oops 路径
 *
 *   unwinder / ftrace / kprobe / notifier debug
 *     -> core_kernel_text() / kernel_text_address()
 *          静态链接区间快速判断 -> 受 RCU 保护的动态代码判断
 *
 * 核心对象与生命周期：
 *
 *   __start___ex_table..__stop___ex_table 是链接器收集的内建表，
 *   内核整个生命周期只读使用；启动时可能原地排序一次。模块和 BPF 表
 *   随其拥有者发布、摘除，查询返回的都是借用指针，调用者不能取得其
 *   所有权，也不能脱离相应模块/BPF 执行期或 RCU 生命周期长期保存。
 *
 * 并发模型：
 *
 *   内建异常表在 SMP 和正常异常处理开始前排序，之后无需锁即可并发
 *   二分查找。模块、BPF、动态 trampoline 和 kprobe slot 的索引可能并发
 *   变化，查询 helper 使用 RCU；kernel_text_address() 还要处理 idle、
 *   CPU hotplug 等 RCU 暂停观察的特殊栈回溯上下文。text_mutex 则串行化
 *   代码字节修改，它不保护异常表查找，也不替代 RCU 生命周期保证。
 *
 * 方案权衡：
 *
 *   链接期表项加启动期排序让正常故障可用二分查找，查询成本低且
 *   不需要运行期分配；代价是动态代码必须维护各自的独立表和生命周期
 *   协议。地址判定先走静态区间快速路径，只在必要时进入动态索引，
 *   以兼顾常见栈回溯性能与模块卸载、JIT 回收期间的安全性。
 */
/* Rewritten by Rusty Russell, on the backs of many others...
   Copyright (C) 2001 Rusty Russell, 2002 Rusty Russell IBM.

*/
/*
 * 本文件由 Rusty Russell 在多位前人实现基础上重写；原版权信息如上。
 */
#include <linux/elf.h>
#include <linux/ftrace.h>
#include <linux/memory.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/filter.h>

#include <asm/sections.h>
#include <linux/uaccess.h>

/*
 * mutex protecting text section modification (dynamic code patching).
 * some users need to sleep (allocating memory...) while they hold this lock.
 *
 * Note: Also protects SMP-alternatives modification on x86.
 *
 * NOT exported to modules - patching kernel text is a really delicate matter.
 */
/*
 * text_mutex 串行化内核文本的运行期修改，例如 kprobe、ftrace、
 * static call、jump label 和部分架构 alternatives/BPF patching。
 *
 * 它是可睡眠 mutex，因为某些修改路径持锁期间需要分配内存；因此不能在
 * NMI、硬中断或其他原子上下文获取。x86 的 SMP alternatives 也遵守
 * 同一串行边界。锁本身只在内建内核内可见，刻意不导出给模块：
 * 任意模块直接改写内核代码会绕过架构指令同步、只读映射和其他
 * patching 协议。
 *
 * 该锁保护“修改操作之间不交错”，并不让正在执行的 CPU 自动看到
 * 新指令；具体 patch helper 仍负责 stop_machine、临时映射和
 * I-cache 同步等架构要求。锁为静态全生命周期对象，不涉及引用所有权。
 */
DEFINE_MUTEX(text_mutex);

/*
 * 链接器定义的内建异常表半开区间 [__start___ex_table,
 * __stop___ex_table)。数组元素由编译器/汇编宏写入
 * __ex_table section；本文件借用这两个边界，不拥有存储。启动排序后，
 * 异常处理路径把它当作有序、全生命周期有效的只读数组。
 */
extern struct exception_table_entry __start___ex_table[];
extern struct exception_table_entry __stop___ex_table[];

/* Cleared by build time tools if the table is already sorted. */
/*
 * 构建工具若已在最终 vmlinux 中排好异常表，会把此标志清零；否则
 * 保持 1，让启动代码补做排序。__visible 保证离线工具能够定位符号，
 * __initdata 表示判断完成后即可随初始化数据回收。启动阶段单线程
 * 读写，无需锁。
 */
u32 __initdata __visible main_extable_sort_needed = 1;

/* Sort the kernel's built-in exception table */
/*
 * 对内核内建异常表执行必要的启动期排序。
 *
 * 调用关系：start_kernel() 在正常异常表查询全面启用前调用；底层
 * sort_extable() 按每个表项对应的故障指令地址排序，建立二分查找前提。
 *
 * 入参：无。返回：无直接返回值。函数运行在早期单线程 __init 上下文，
 * 可以使用初始化期可写的 __ex_table，不持锁，也不转移任何 ownership。
 * 构建工具已排序或表为空时保持原状；否则原地重排表项并打印一次
 * 提示。完成后 search_extable() 可以安全依赖有序不变量，函数本身
 * 随 init 回收。
 */
void __init sort_main_extable(void)
{
	/*
	 * 两个条件分别避免重复排序以及对空表调用 sorter；边界相减前也先
	 * 确认 stop 位于 start 之后。
	 */
	if (main_extable_sort_needed &&
	    &__stop___ex_table > &__start___ex_table) {
		pr_notice("Sorting __ex_table...\n");
		/*
		 * sort_extable() 原地改变表项顺序；相对地址架构会在交换时同步
		 * 修正偏移，使排序后每个 insn/fixup 关系仍指向原对象。
		 */
		sort_extable(__start___ex_table, __stop___ex_table);
	}
}

/* Given an address, look for it in the kernel exception table */
/*
 * 在内建异常表中查找 addr 对应的故障指令记录。
 *
 * @addr 是待匹配的内核指令虚拟地址，按值输入、无范围预设。表已由构建
 * 工具或 sort_main_extable() 排序；函数可在 fault/NMI 等不可睡眠上下文
 * 并发调用，不取锁、不分配内存。
 *
 * 返回匹配表项的全生命周期借用 const 指针，找不到返回 NULL。函数只做
 * 二分查找，不执行 fixup、不修改异常现场；调用者通常把结果交给架构
 * fixup_exception() 解释。
 */
const
struct exception_table_entry *search_kernel_exception_table(unsigned long addr)
{
	/*
	 * stop-start 得到表项数量而非字节数；base 和数量共同描述链接器
	 * 生成的完整内建表。
	 */
	return search_extable(__start___ex_table,
			      __stop___ex_table - __start___ex_table, addr);
}

/* Given an address, look for it in the exception tables. */
/*
 * 按代码所有者优先级查找任意内核异常表。
 *
 * @addr 是发生异常的指令虚拟地址，纯输入。调用者通常正运行在该地址
 * 所属的内核、模块或 BPF 程序中，因此动态代码拥有者仍由当前执行关系
 * 稳定；返回值仅供当前修复/判定立即使用，不转移引用或存储所有权。
 *
 * 函数先查永久且最常见的内建表，再查模块，最后查 BPF JIT 表；配置关闭
 * 时后两者由头文件 stub 返回 NULL。返回首个匹配表项，全部未命中返回
 * NULL。各动态 helper 内部使用 RCU 且不会睡眠，本函数本身不修改状态。
 */
const struct exception_table_entry *search_exception_tables(unsigned long addr)
{
	/*
	 * e 始终是当前候选表项的借用指针；NULL 同时充当
	 * “继续查下一来源”和最终“不可修复”的哨兵。
	 */
	const struct exception_table_entry *e;

	/* 阶段 1：先走无需动态生命周期保护的内建表快速路径。 */
	e = search_kernel_exception_table(addr);
	/*
	 * 阶段 2：仅在未命中时进入模块的 RCU 索引，避免常见路径
	 * 额外开销。
	 */
	if (!e)
		e = search_module_extables(addr);
	/* 阶段 3：最后覆盖带异常表元数据的 BPF JIT 程序。 */
	if (!e)
		e = search_bpf_extables(addr);
	return e;
}

/*
 * core_kernel_text() - 判断 addr 是否属于当前仍有效的内建核心代码。
 *
 * @addr 是待检查的内核虚拟地址，纯输入、无 ownership。函数先接受常驻
 * .text（以及架构 gate area），在系统开始释放 initmem 之前也接受
 * .init.text；释放开始后必须拒绝后者，避免把已回收并可能复用的字节误认
 * 为可执行内核代码。
 *
 * 返回 1 表示属于有效核心文本，0 表示不属于。无输出参数和状态
 * 副作用，不取锁、不分配、不会睡眠，可供 tracing/异常等敏感上下文
 * 调用。
 * notrace 防止函数自身被跟踪后递归进入文本地址判定。模块、JIT 和动态
 * trampoline 不在本函数职责内，由 kernel_text_address() 继续判断。
 */
int notrace core_kernel_text(unsigned long addr)
{
	/* 常驻 .text 是绝大多数调用的无锁区间判断快速路径。 */
	if (is_kernel_text(addr))
		return 1;

	/*
	 * .init.text 只在释放动作开始前仍是活代码；system_state 的
	 * 顺序值让检查与启动生命周期直接对应，而不是仅凭地址范围猜测
	 * 存储有效性。
	 */
	if (system_state < SYSTEM_FREEING_INITMEM &&
	    is_kernel_inittext(addr))
		return 1;
	return 0;
}

/*
 * __kernel_text_address() - 为栈回溯识别当前或历史上的内核代码地址。
 *
 * @addr 是保存于栈中的候选虚拟地址，纯输入。先调用
 * kernel_text_address() 覆盖当前仍有效的内建与动态代码；若未命中，再
 * 接受链接期 .init.text 区间，即使 initmem 已被释放，也让早先保存的
 * init 调用栈仍有机会由 kallsyms 打印符号。
 *
 * 返回 1/0，无 ownership 或状态变化，不睡眠。动态代码查询可能使用
 * RCU，具体异常上下文兼容由 kernel_text_address() 处理。这里不是安全
 * 执行权限检查：对已释放 init 地址返回 1 只服务诊断，不能据此
 * 跳转执行。
 */
int __kernel_text_address(unsigned long addr)
{
	/* 先识别活跃代码，尤其要让模块索引优先处理可能的动态地址。 */
	if (kernel_text_address(addr))
		return 1;
	/*
	 * There might be init symbols in saved stacktraces.
	 * Give those symbols a chance to be printed in
	 * backtraces (such as lockdep traces).
	 *
	 * Since we are after the module-symbols check, there's
	 * no danger of address overlap:
	 */
	/*
	 * 已保存的栈可能包含 init 符号，因此即使对应存储已回收，也允许
	 * 符号化器把它打印出来。模块符号已在上面的动态代码检查中优先
	 * 判定，所以这里的历史 init 区间兜底不会遮蔽模块地址识别结果。
	 */
	if (is_kernel_inittext(addr))
		return 1;
	return 0;
}

/*
 * kernel_text_address() - 判断 addr 是否属于当前有效的内核可执行代码。
 *
 * @addr 是候选虚拟地址，纯输入，既可能来自栈回溯，也可能来自动态 patch
 * 或调试校验。返回 1 表示属于常驻核心文本、仍存活的 init text、模块、
 * ftrace trampoline、kprobe 指令槽或 BPF JIT 文本；全部不匹配返回 0。
 * 函数不返回对象引用，也不保证地址对应代码在返回后可长期存活，
 * 因此结果适合即时分类，不能替代模块/BPF 的长期引用协议。
 *
 * 核心区间走无锁快速路径。动态索引依赖 RCU，但调用可能来自 idle 退出、
 * CPU online/offline、WARN 或 tracing 递归等 RCU 暂未观察当前 CPU 的
 * 上下文；此时临时按 NMI 进入/退出协议恢复 RCU watching。函数不睡眠，
 * 不要求调用者预持锁，并必须通过统一 out 出口精确配对上下文恢复。
 */
int kernel_text_address(unsigned long addr)
{
	/*
	 * no_rcu 记录本函数是否亲自切换了 context tracking；ret 预置 1，
	 * 让任一动态命中都经 goto out 共享同一个恢复出口。
	 */
	bool no_rcu;
	int ret = 1;

	/* 永久/尚有效的静态链接区不需要触碰 RCU，是常见快速路径。 */
	if (core_kernel_text(addr))
		return 1;

	/*
	 * If a stack dump happens while RCU is not watching, then
	 * RCU needs to be notified that it requires to start
	 * watching again. This can happen either by tracing that
	 * triggers a stack trace, or a WARN() that happens during
	 * coming back from idle, or cpu on or offlining.
	 *
	 * is_module_text_address() as well as the kprobe slots,
	 * is_bpf_text_address() and is_bpf_image_address require
	 * RCU to be watching.
	 */
	/*
	 * 栈转储可能发生在 RCU 停止观察当前 CPU 的窗口，例如 tracing 引发
	 * 递归栈、idle 返回途中的 WARN，或 CPU 上下线。模块、kprobe slot
	 * 以及 BPF 动态代码索引都依赖 RCU 生命周期；在这种状态直接
	 * 进入读侧会违反 RCU 上下文约束。
	 *
	 * 原文提到 is_bpf_image_address()，当前实现实际调用
	 * is_bpf_text_address()；两者都属于动态 BPF 地址判定这一生命周期
	 * 类别，核心要求仍是查询期间 RCU 必须 watching。
	 */
	no_rcu = !rcu_is_watching();

	/* Treat this like an NMI as it can happen anywhere */
	/*
	 * 把特殊调用按 NMI 嵌套处理：若 CPU 原处于 RCU extended quiescent
	 * state，ct_nmi_enter() 临时标记为活跃；若本来就在观察，只记录普通
	 * 嵌套。只在 no_rcu=true 时调用，避免破坏调用者已有的跟踪层级。
	 */
	if (no_rcu)
		ct_nmi_enter();

	/*
	 * 动态来源依次覆盖模块、ftrace trampoline、kprobe 优化/普通指令槽
	 * 和 BPF JIT。各 helper 只给出当前查询窗口内的存在性，不转移引用；
	 * 一旦命中便跳到统一出口，避免漏掉 ct_nmi_exit()。
	 */
	if (is_module_text_address(addr))
		goto out;
	if (is_ftrace_trampoline(addr))
		goto out;
	if (is_kprobe_optinsn_slot(addr) || is_kprobe_insn_slot(addr))
		goto out;
	if (is_bpf_text_address(addr))
		goto out;
	/* 所有活跃文本来源均未命中，覆盖 ret 的默认成功值。 */
	ret = 0;
out:
	/*
	 * 只有本函数曾临时唤醒 RCU 时才恢复进入前的 quiescent 状态；
	 * 配对顺序不能提前到动态查询之前，也不能在命中分支直接 return。
	 */
	if (no_rcu)
		ct_nmi_exit();

	return ret;
}

/*
 * On some architectures (PPC64, IA64, PARISC) function pointers
 * are actually only tokens to some data that then holds the
 * real function address. As a result, to find if a function
 * pointer is part of the kernel text, we need to do some
 * special dereferencing first.
 */
/*
 * 某些架构（PPC64、IA64、PARISC）的函数指针不是代码地址，而是指向
 * function descriptor 的 token；descriptor 再保存真实入口地址。因此
 * 对函数指针做文本区间判断前，必须先按架构 ABI 安全取出实际地址。
 *
 * 以下实现仅在 CONFIG_HAVE_FUNCTION_DESCRIPTORS=y 时编译；其他架构由
 * asm-generic/sections.h 的恒等宏直接返回原指针，不产生额外读取。
 */
#ifdef CONFIG_HAVE_FUNCTION_DESCRIPTORS
/*
 * dereference_function_descriptor() - 尝试从函数描述符取得真实代码地址。
 *
 * @ptr 是调用者借用的候选 descriptor 指针，也可能不是可读 descriptor；
 * 所有权不转移。get_kernel_nofault() 在禁止普通 fault 修复的敏感路径中
 * 安全读取 desc->addr：读取成功返回其中的代码指针，读取失败则原样返回
 * ptr，避免因诊断一个坏函数指针再次触发内核故障。
 *
 * 函数无状态副作用、不分配、不会睡眠；返回值仍是无引用的借用地址。
 * 它不验证结果是否属于文本，调用者须再做 core/module 等范围判断。
 */
void *dereference_function_descriptor(void *ptr)
{
	/*
	 * desc 只是对输入 token 的解释视图；p 仅在 nofault 读取成功后成为
	 * 候选真实入口，二者都不获得所指对象的引用。
	 */
	func_desc_t *desc = ptr;
	void *p;

	/* 只有安全读取成功（返回 0）才用 descriptor 内容替换输入。 */
	if (!get_kernel_nofault(p, (void *)&desc->addr))
		ptr = p;
	return ptr;
}
EXPORT_SYMBOL_GPL(dereference_function_descriptor);

/*
 * dereference_kernel_function_descriptor() - 仅解引用内建内核 .opd token。
 *
 * @ptr 是借用的候选函数指针。链接器区间 [__start_opd, __end_opd)
 * 表示内建 kernel 的 descriptor table；区间外指针原样返回，避免把普通
 * 代码地址或模块地址误当作 descriptor 读取。区间内委托通用 nofault
 * helper，返回真实入口或读取失败时的原 token。
 *
 * 函数不睡眠、不修改状态、不取得引用。模块 descriptor 需要模块感知的
 * dereference_module_function_descriptor() 路径，不由本函数处理。
 */
void *dereference_kernel_function_descriptor(void *ptr)
{
	/* 半开区间检查同时排除边界 __end_opd，它不是有效表项。 */
	if (ptr < (void *)__start_opd || ptr >= (void *)__end_opd)
		return ptr;

	return dereference_function_descriptor(ptr);
}
#endif

/*
 * func_ptr_is_kernel_text() - 校验函数指针最终是否落在核心或模块文本。
 *
 * @ptr 是待校验的借用函数指针；descriptor 架构先解引用，普通架构该
 * helper 是恒等转换。返回 1 表示真实入口属于当前 core text，或属于已
 * 发布模块文本；否则返回 0。BPF、ftrace trampoline 和 kprobe slot 不在
 * 本接口的“普通函数指针”契约内。
 *
 * 核心路径是无锁区间判断；模块路径内部使用 RCU。因此可能传入模块函数
 * 的调用者必须处在允许 RCU watching 的上下文。函数不睡眠、不取得模块
 * 引用，结果只适合立即校验；CONFIG_DEBUG_NOTIFIERS 用它在回调前发现
 * 已损坏或指向非代码区域的 notifier 函数指针。
 */
int func_ptr_is_kernel_text(void *ptr)
{
	/* addr 是 descriptor 解析后的候选代码虚拟地址，只在本函数内有效。 */
	unsigned long addr;

	addr = (unsigned long) dereference_function_descriptor(ptr);
	/* 内建 core 是无需进入模块 RCU 索引的快速成功路径。 */
	if (core_kernel_text(addr))
		return 1;
	/*
	 * 模块 helper 完成动态生命周期内的存在性判断；返回值直接
	 * 向上传递。
	 */
	return is_module_text_address(addr);
}
