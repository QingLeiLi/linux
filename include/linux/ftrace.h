/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Ftrace header.  For implementation details beyond the random comments
 * scattered below, see: Documentation/trace/ftrace-design.rst
 */
/*
 * Ftrace 头文件。关于实现细节，除了下面的注释外，
 * 请参阅：Documentation/trace/ftrace-design.rst
 *
 * =====================================================
 * Ftrace (Function Tracer) 整体设计说明:
 *
 * Ftrace 是 Linux 内核的函数跟踪框架，允许在运行时跟踪内核函数的调用。
 *
 * 核心机制:
 *   1. 编译器在每个函数入口处插入桩代码（mcount 或 fentry）
 *   2. 动态 ftrace: 启动时将桩替换为 nop，需要时再替换为跳转到跟踪代码
 *   3. 静态 ftrace: 桩始终调用 ftrace，通过检查标志决定是否执行跟踪
 *
 * 主要组件:
 *   - ftrace_ops: 描述一个跟踪操作（回调函数、过滤器等）
 *   - ftrace_regs: 函数调用时的寄存器快照（参数、返回值等）
 *   - dyn_ftrace: 动态 ftrace 的每个跟踪点元数据
 *
 * 与 kprobes 的区别:
 *   - ftrace 只能在函数入口/出口，kprobes 可在任意指令
 *   - ftrace 开销更低（专用桩），kprobes 更灵活（断点机制）
 *   - ftrace 用于性能分析，kprobes 用于调试/监控
 *
 * 典型用途:
 *   - 函数调用图（function graph tracer）
 *   - 性能分析（function profiling）
 *   - 延迟跟踪（latency tracer）
 *   - 动态事件跟踪（event tracing）
 * =====================================================
 */

#ifndef _LINUX_FTRACE_H
#define _LINUX_FTRACE_H

#include <linux/trace_recursion.h>
#include <linux/trace_clock.h>
#include <linux/jump_label.h>
#include <linux/kallsyms.h>
#include <linux/linkage.h>
#include <linux/bitops.h>
#include <linux/ptrace.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/fs.h>

#include <asm/ftrace.h>

/*
 * If the arch supports passing the variable contents of
 * function_trace_op as the third parameter back from the
 * mcount call, then the arch should define this as 1.
 */
/*
 * 如果架构支持将 function_trace_op 变量内容作为第三个参数
 * 从 mcount 调用传回，则架构应将此定义为 1。
 *
 * ARCH_SUPPORTS_FTRACE_OPS 说明:
 * - 为 1: 架构的 mcount/fentry 桩可以传递 ftrace_ops 指针给回调
 *   这样回调函数可以直接访问注册时提供的上下文（ftrace_ops）
 * - 为 0: 架构不支持，需要通过间接函数查找当前 ftrace_ops
 *
 * 支持此特性的架构性能更好，因为避免了额外的查找开销
 */
#ifndef ARCH_SUPPORTS_FTRACE_OPS
#define ARCH_SUPPORTS_FTRACE_OPS 0
#endif

#ifdef CONFIG_TRACER_SNAPSHOT
/* ftrace_boot_snapshot - 在启动时触发快照 */
extern void ftrace_boot_snapshot(void);
#else
static inline void ftrace_boot_snapshot(void) { }
#endif

/* 前向声明：这些结构在后面定义 */
struct ftrace_ops;       // ftrace 操作描述符
struct ftrace_regs;      // ftrace 寄存器快照
struct dyn_ftrace;       // 动态 ftrace 跟踪点

/*
 * arch_ftrace_match_adjust - 架构相关：调整符号匹配字符串
 * @str:    原始字符串
 * @search: 搜索字符串
 * 返回值: 调整后的字符串
 *
 * 某些架构（如 PowerPC）的函数符号可能有特殊前缀/后缀，
 * 此函数用于在匹配前进行调整
 */
char *arch_ftrace_match_adjust(char *str, const char *search);

#ifdef CONFIG_HAVE_FUNCTION_GRAPH_FREGS
/*
 * ftrace_return_to_handler - 函数图跟踪器的返回处理（带完整寄存器）
 * @fregs: ftrace 寄存器快照（含返回值）
 * 返回值: 真实的返回地址
 */
unsigned long ftrace_return_to_handler(struct ftrace_regs *fregs);
#else
/*
 * ftrace_return_to_handler - 函数图跟踪器的返回处理（只有帧指针）
 * @frame_pointer: 帧指针
 * 返回值: 真实的返回地址
 */
unsigned long ftrace_return_to_handler(unsigned long frame_pointer);
#endif

#ifdef CONFIG_FUNCTION_TRACER
/*
 * If the arch's mcount caller does not support all of ftrace's
 * features, then it must call an indirect function that
 * does. Or at least does enough to prevent any unwelcome side effects.
 *
 * Also define the function prototype that these architectures use
 * to call the ftrace_ops_list_func().
 */
#if !ARCH_SUPPORTS_FTRACE_OPS
# define FTRACE_FORCE_LIST_FUNC 1
void arch_ftrace_ops_list_func(unsigned long ip, unsigned long parent_ip);
#else
# define FTRACE_FORCE_LIST_FUNC 0
void arch_ftrace_ops_list_func(unsigned long ip, unsigned long parent_ip,
			       struct ftrace_ops *op, struct ftrace_regs *fregs);
#endif
extern const struct ftrace_ops ftrace_nop_ops;
extern const struct ftrace_ops ftrace_list_ops;
struct ftrace_ops *ftrace_find_unique_ops(struct dyn_ftrace *rec);
#endif /* CONFIG_FUNCTION_TRACER */

/* Main tracing buffer and events set up */
#ifdef CONFIG_TRACING
void trace_init(void);
void early_trace_init(void);
#else
static inline void trace_init(void) { }
static inline void early_trace_init(void) { }
#endif

struct module;
struct ftrace_hash;
struct ftrace_func_entry;

#if defined(CONFIG_FUNCTION_TRACER) && defined(CONFIG_MODULES) && \
	defined(CONFIG_DYNAMIC_FTRACE)
int
ftrace_mod_address_lookup(unsigned long addr, unsigned long *size,
			  unsigned long *off, char **modname,
			  const unsigned char **modbuildid, char *sym);
#else
static inline int
ftrace_mod_address_lookup(unsigned long addr, unsigned long *size,
			  unsigned long *off, char **modname,
			  const unsigned char **modbuildid, char *sym)
{
	return 0;
}
#endif

#if defined(CONFIG_FUNCTION_TRACER) && defined(CONFIG_DYNAMIC_FTRACE)
int ftrace_mod_get_kallsym(unsigned int symnum, unsigned long *value,
			   char *type, char *name,
			   char *module_name, int *exported);
#else
static inline int ftrace_mod_get_kallsym(unsigned int symnum, unsigned long *value,
					 char *type, char *name,
					 char *module_name, int *exported)
{
	return -1;
}
#endif

#ifdef CONFIG_FUNCTION_TRACER

#include <linux/ftrace_regs.h>

extern int ftrace_enabled;

/**
 * ftrace_regs - ftrace partial/optimal register set
 *
 * ftrace_regs represents a group of registers which is used at the
 * function entry and exit. There are three types of registers.
 *
 * - Registers for passing the parameters to callee, including the stack
 *   pointer. (e.g. rcx, rdx, rdi, rsi, r8, r9 and rsp on x86_64)
 * - Registers for passing the return values to caller.
 *   (e.g. rax and rdx on x86_64)
 * - Registers for hooking the function call and return including the
 *   frame pointer (the frame pointer is architecture/config dependent)
 *   (e.g. rip, rbp and rsp for x86_64)
 *
 * Also, architecture dependent fields can be used for internal process.
 * (e.g. orig_ax on x86_64)
 *
 * Basically, ftrace_regs stores the registers related to the context.
 * On function entry, registers for function parameters and hooking the
 * function call are stored, and on function exit, registers for function
 * return value and frame pointers are stored.
 *
 * And also, it dpends on the context that which registers are restored
 * from the ftrace_regs.
 * On the function entry, those registers will be restored except for
 * the stack pointer, so that user can change the function parameters
 * and instruction pointer (e.g. live patching.)
 * On the function exit, only registers which is used for return values
 * are restored.
 *
 * NOTE: user *must not* access regs directly, only do it via APIs, because
 * the member can be changed according to the architecture.
 * This is why the structure is empty here, so that nothing accesses
 * the ftrace_regs directly.
 */
/*
 * ftrace_regs - ftrace 寄存器快照（架构无关抽象层）
 *
 * 这是一个故意为空的结构体！真正的寄存器字段定义在
 * arch/*/include/asm/ftrace.h 的 __arch_ftrace_regs 中。
 *
 * 设计原因（为什么不直接暴露字段）:
 * - 不同架构的寄存器布局完全不同（x86 vs ARM vs RISC-V）
 * - 使用 API 访问而非直接读字段，可以让上层代码跨架构移植
 * - 编译器会报错阻止任何直接访问，强制使用安全的 accessor 函数
 *
 * 使用场景（函数入口 vs 函数出口）:
 * - 函数入口: 保存参数寄存器（可修改参数或 IP，用于 live patching）
 * - 函数出口: 保存返回值寄存器（可读取/修改返回值）
 *
 * 正确访问方式，使用下列 API:
 * - ftrace_regs_get_argument(fregs, n)   获取第 n 个参数
 * - ftrace_regs_get_return_value(fregs)  获取返回值
 * - ftrace_regs_get_instruction_pointer(fregs)  获取 IP
 * - ftrace_regs_set_instruction_pointer(fregs, ip)  修改 IP（live patch）
 * - ftrace_get_regs(fregs)  获取完整 pt_regs（若可用）
 */
struct ftrace_regs {
	/* Nothing to see here, use the accessor functions! */
	/* 没什么可看的，请使用 accessor 函数！ */
};

/* ftrace_regs_size - 获取真实的架构寄存器结构大小（字节） */
#define ftrace_regs_size()	sizeof(struct __arch_ftrace_regs)

#ifndef CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS
/*
 * Architectures that define HAVE_DYNAMIC_FTRACE_WITH_ARGS must define their own
 * arch_ftrace_get_regs() where it only returns pt_regs *if* it is fully
 * populated. It should return NULL otherwise.
 */
/*
 * arch_ftrace_get_regs - 获取完整的 pt_regs 指针（不支持 WITH_ARGS 的架构）
 * @fregs: ftrace 寄存器快照
 * 返回值: 指向 pt_regs 的指针（始终非 NULL，因为此版本总是完整保存了寄存器）
 *
 * 注意: 支持 HAVE_DYNAMIC_FTRACE_WITH_ARGS 的架构必须自定义此函数，
 * 因为在该模式下并非所有寄存器都被保存，需要判断后再返回
 */
static inline struct pt_regs *arch_ftrace_get_regs(struct ftrace_regs *fregs)
{
	return &arch_ftrace_regs(fregs)->regs;
}

/*
 * ftrace_regs_set_instruction_pointer() is to be defined by the architecture
 * if to allow setting of the instruction pointer from the ftrace_regs when
 * HAVE_DYNAMIC_FTRACE_WITH_ARGS is set and it supports live kernel patching.
 */
/*
 * ftrace_regs_set_instruction_pointer - 修改指令指针（用于 live patching）
 *
 * 对于不支持 DYNAMIC_FTRACE_WITH_ARGS 的架构，此操作为空（nop）。
 * 支持该特性的架构需要在 asm/ftrace.h 中提供真正的实现。
 *
 * Live patching 原理: 在函数入口修改 IP，使函数直接跳到新版本执行
 */
#define ftrace_regs_set_instruction_pointer(fregs, ip) do { } while (0)
#endif /* CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS */

#ifdef CONFIG_HAVE_FTRACE_REGS_HAVING_PT_REGS
/* 静态断言: 当 ftrace_regs 的内存布局与 pt_regs 完全一致时，大小必须相同 */
static_assert(sizeof(struct pt_regs) == ftrace_regs_size());

#endif /* CONFIG_HAVE_FTRACE_REGS_HAVING_PT_REGS */

/*
 * ftrace_get_regs - 安全地获取完整 pt_regs 指针
 * @fregs: ftrace 寄存器快照，允许为 NULL
 * 返回值: 完整的 pt_regs 指针；若 fregs 为 NULL 或寄存器不完整则返回 NULL
 *
 * 与 arch_ftrace_get_regs 的区别:
 * - 做了 NULL 检查，更安全
 * - 返回 NULL 意味着只有部分寄存器可用，不能当作完整 pt_regs 使用
 */
static __always_inline struct pt_regs *ftrace_get_regs(struct ftrace_regs *fregs)
{
	if (!fregs)
		return NULL;

	return arch_ftrace_get_regs(fregs);
}

#if !defined(CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS) || \
	defined(CONFIG_HAVE_FTRACE_REGS_HAVING_PT_REGS)

#ifndef arch_ftrace_partial_regs
/* 默认实现: 架构不需要额外处理，什么都不做 */
#define arch_ftrace_partial_regs(regs) do {} while (0)
#endif

/*
 * ftrace_partial_regs - 获取部分寄存器的 pt_regs 视图
 * @fregs: ftrace 寄存器快照
 * @regs:  调用者提供的 pt_regs 缓冲区（在某些情况下被填充）
 * 返回值: 指向寄存器数据的 pt_regs 指针
 *
 * 与 ftrace_get_regs 的区别:
 * - 即使寄存器不完整也返回非 NULL（"部分"之意）
 * - 用于 perf、stack trace 等可以接受部分寄存器的场景
 *
 * 注意: 当 FTRACE_REGS_HAVING_PT_REGS=y 时，ftrace_regs 内存布局
 * 与 pt_regs 完全重叠，直接返回其地址（不通过 get_regs 是因为后者可能返回 NULL）
 */
static __always_inline struct pt_regs *
ftrace_partial_regs(struct ftrace_regs *fregs, struct pt_regs *regs)
{
	/*
	 * If CONFIG_HAVE_FTRACE_REGS_HAVING_PT_REGS=y, ftrace_regs memory
	 * layout is including pt_regs. So always returns that address.
	 * Since arch_ftrace_get_regs() will check some members and may return
	 * NULL, we can not use it.
	 */
	regs = &arch_ftrace_regs(fregs)->regs;

	/* Allow arch specific updates to regs. */
	arch_ftrace_partial_regs(regs);
	return regs;
}

#endif /* !CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS || CONFIG_HAVE_FTRACE_REGS_HAVING_PT_REGS */

#ifdef CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS

/*
 * Please define arch dependent pt_regs which compatible to the
 * perf_arch_fetch_caller_regs() but based on ftrace_regs.
 * This requires
 *   - user_mode(_regs) returns false (always kernel mode).
 *   - able to use the _regs for stack trace.
 */
/*
 * arch_ftrace_fill_perf_regs - 将 ftrace_regs 填充为 perf 兼容的 pt_regs
 * @fregs:  ftrace 寄存器快照（源）
 * @_regs:  目标 pt_regs 缓冲区
 *
 * 要求:
 * - user_mode(_regs) 必须返回 false（始终内核模式）
 * - 填充后的 _regs 必须支持栈回溯
 *
 * 各架构按需重写此宏；默认实现为空（do nothing）
 */
#ifndef arch_ftrace_fill_perf_regs
/* As same as perf_arch_fetch_caller_regs(), do nothing by default */
#define arch_ftrace_fill_perf_regs(fregs, _regs) do {} while (0)
#endif

/*
 * ftrace_fill_perf_regs - 填充 perf 用的寄存器视图（WITH_ARGS 版本）
 * 调用架构特定的填充宏后返回 regs
 */
static __always_inline struct pt_regs *
ftrace_fill_perf_regs(struct ftrace_regs *fregs, struct pt_regs *regs)
{
	arch_ftrace_fill_perf_regs(fregs, regs);
	return regs;
}

#else /* !CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS */

/*
 * ftrace_fill_perf_regs - 填充 perf 用的寄存器视图（传统版本）
 * 直接返回 arch_ftrace_regs 中的 pt_regs（因为它是完整保存的）
 */
static __always_inline struct pt_regs *
ftrace_fill_perf_regs(struct ftrace_regs *fregs, struct pt_regs *regs)
{
	return &arch_ftrace_regs(fregs)->regs;
}

#endif

/*
 * When true, the ftrace_regs_{get,set}_*() functions may be used on fregs.
 * Note: this can be true even when ftrace_get_regs() cannot provide a pt_regs.
 */
/*
 * ftrace_regs_has_args - 判断 fregs 中的参数/寄存器 accessor 是否可用
 * @fregs: ftrace 寄存器快照
 * 返回值: true 表示可以调用 ftrace_regs_get_argument() 等函数
 *
 * 注意: 即使 ftrace_get_regs() 返回 NULL（寄存器不完整），
 * 此函数也可能返回 true——两者是独立的能力判断：
 * - ftrace_regs_has_args: 是否有参数寄存器（用于读取函数参数）
 * - ftrace_get_regs:      是否有完整的 pt_regs（用于调试器、stacktrace）
 */
static __always_inline bool ftrace_regs_has_args(struct ftrace_regs *fregs)
{
	if (IS_ENABLED(CONFIG_HAVE_DYNAMIC_FTRACE_WITH_ARGS))
		return true;

	return ftrace_get_regs(fregs) != NULL;
}

#ifdef CONFIG_HAVE_REGS_AND_STACK_ACCESS_API
/*
 * ftrace_regs_get_kernel_stack_nth - 读取内核栈上第 nth 个 unsigned long
 * @fregs: ftrace 寄存器快照
 * @nth:   栈偏移（0 = 栈顶）
 * 返回值: 栈上对应位置的值；若越界（跨越线程栈边界）则返回 0
 *
 * 安全性: 通过 THREAD_SIZE 掩码检查确保不会越界读取到相邻线程栈
 * 注意: 只有在 CONFIG_HAVE_REGS_AND_STACK_ACCESS_API 时才可用
 */
static __always_inline unsigned long
ftrace_regs_get_kernel_stack_nth(struct ftrace_regs *fregs, unsigned int nth)
{
	unsigned long *stackp;

	stackp = (unsigned long *)ftrace_regs_get_stack_pointer(fregs);
	if (((unsigned long)(stackp + nth) & ~(THREAD_SIZE - 1)) ==
	    ((unsigned long)stackp & ~(THREAD_SIZE - 1)))
		return *(stackp + nth);

	return 0;
}
#else /* !CONFIG_HAVE_REGS_AND_STACK_ACCESS_API */
#define ftrace_regs_get_kernel_stack_nth(fregs, nth)	(0L)
#endif /* CONFIG_HAVE_REGS_AND_STACK_ACCESS_API */

/*
 * ftrace_func_t - ftrace 回调函数类型
 * @ip:        被跟踪函数的地址（指令指针）
 * @parent_ip: 调用被跟踪函数的调用者地址（返回地址）
 * @op:        触发此回调的 ftrace_ops（包含用户注册的上下文）
 * @fregs:     函数调用时的寄存器快照（可为 NULL，取决于 ops 标志）
 *
 * 注意事项:
 * - 此回调在被跟踪函数执行前（或后，取决于 graph tracer）调用
 * - 若需要访问寄存器，ops 必须设置 FTRACE_OPS_FL_SAVE_REGS 标志
 * - 回调不能无条件睡眠；若设置了 FTRACE_OPS_FL_RCU，则在 RCU 保护下调用
 * - ip 和 parent_ip 结合可用于构建调用图（call graph）
 */
typedef void (*ftrace_func_t)(unsigned long ip, unsigned long parent_ip,
			      struct ftrace_ops *op, struct ftrace_regs *fregs);

/*
 * ftrace_ops_get_func - 获取 ftrace_ops 实际使用的回调函数
 * @ops: ftrace 操作描述符
 * 返回值: 实际注册到跟踪基础设施的函数指针
 *
 * 说明: 当 ops 需要 recursion protection 或其他包装时，
 * 实际使用的函数可能不是 ops->func，而是一个包装函数
 */
ftrace_func_t ftrace_ops_get_func(struct ftrace_ops *ops);

/*
 * FTRACE_OPS_FL_* bits denote the state of ftrace_ops struct and are
 * set in the flags member.
 * CONTROL, SAVE_REGS, SAVE_REGS_IF_SUPPORTED, RECURSION, STUB and
 * IPMODIFY are a kind of attribute flags which can be set only before
 * registering the ftrace_ops, and can not be modified while registered.
 * Changing those attribute flags after registering ftrace_ops will
 * cause unexpected results.
 *
 * ENABLED - set/unset when ftrace_ops is registered/unregistered
 * DYNAMIC - set when ftrace_ops is registered to denote dynamically
 *           allocated ftrace_ops which need special care
 * SAVE_REGS - The ftrace_ops wants regs saved at each function called
 *            and passed to the callback. If this flag is set, but the
 *            architecture does not support passing regs
 *            (CONFIG_DYNAMIC_FTRACE_WITH_REGS is not defined), then the
 *            ftrace_ops will fail to register, unless the next flag
 *            is set.
 * SAVE_REGS_IF_SUPPORTED - This is the same as SAVE_REGS, but if the
 *            handler can handle an arch that does not save regs
 *            (the handler tests if regs == NULL), then it can set
 *            this flag instead. It will not fail registering the ftrace_ops
 *            but, the regs field will be NULL if the arch does not support
 *            passing regs to the handler.
 *            Note, if this flag is set, the SAVE_REGS flag will automatically
 *            get set upon registering the ftrace_ops, if the arch supports it.
 * RECURSION - The ftrace_ops can set this to tell the ftrace infrastructure
 *            that the call back needs recursion protection. If it does
 *            not set this, then the ftrace infrastructure will assume
 *            that the callback can handle recursion on its own.
 * STUB   - The ftrace_ops is just a place holder.
 * INITIALIZED - The ftrace_ops has already been initialized (first use time
 *            register_ftrace_function() is called, it will initialized the ops)
 * DELETED - The ops are being deleted, do not let them be registered again.
 * ADDING  - The ops is in the process of being added.
 * REMOVING - The ops is in the process of being removed.
 * MODIFYING - The ops is in the process of changing its filter functions.
 * ALLOC_TRAMP - A dynamic trampoline was allocated by the core code.
 *            The arch specific code sets this flag when it allocated a
 *            trampoline. This lets the arch know that it can update the
 *            trampoline in case the callback function changes.
 *            The ftrace_ops trampoline can be set by the ftrace users, and
 *            in such cases the arch must not modify it. Only the arch ftrace
 *            core code should set this flag.
 * IPMODIFY - The ops can modify the IP register. This can only be set with
 *            SAVE_REGS. If another ops with this flag set is already registered
 *            for any of the functions that this ops will be registered for, then
 *            this ops will fail to register or set_filter_ip.
 * PID     - Is affected by set_ftrace_pid (allows filtering on those pids)
 * RCU     - Set when the ops can only be called when RCU is watching.
 * TRACE_ARRAY - The ops->private points to a trace_array descriptor.
 * PERMANENT - Set when the ops is permanent and should not be affected by
 *             ftrace_enabled.
 * DIRECT - Used by the direct ftrace_ops helper for direct functions
 *            (internal ftrace only, should not be used by others)
 * SUBOP  - Is controlled by another op in field managed.
 * GRAPH  - Is a component of the fgraph_ops structure
 */
/*
 * FTRACE_OPS_FL_* 标志位说明（用于 ftrace_ops.flags 字段）:
 *
 * 分为两类：
 * 1. 属性标志（注册前设置，注册后不可改）: SAVE_REGS, SAVE_REGS_IF_SUPPORTED,
 *    RECURSION, STUB, IPMODIFY
 * 2. 状态标志（由内核 ftrace 核心自动管理）: ENABLED, DYNAMIC, INITIALIZED,
 *    DELETED, ADDING, REMOVING, MODIFYING, ALLOC_TRAMP 等
 *
 * 常用标志详解:
 *
 * ENABLED (BIT 0)
 *   - 含义: ops 已注册并激活
 *   - 管理: register/unregister_ftrace_function 自动设置/清除
 *
 * DYNAMIC (BIT 1)
 *   - 含义: ops 是动态分配的（非 static），注销时需要等待 RCU 宽限期
 *   - 注意: 非 static ops 会自动被设置此标志
 *
 * SAVE_REGS (BIT 2)
 *   - 含义: 要求保存完整寄存器并传递给回调（fregs 非 NULL）
 *   - 前提: 需要 CONFIG_DYNAMIC_FTRACE_WITH_REGS，否则注册失败
 *   - 开销: 更高（需要保存/恢复更多寄存器）
 *   - 用途: live patching（需修改参数/IP）、参数读取
 *
 * SAVE_REGS_IF_SUPPORTED (BIT 3)
 *   - 含义: 希望保存寄存器，但可以接受不支持的架构（fregs 可能为 NULL）
 *   - 与 SAVE_REGS 区别: 不会因架构不支持而注册失败
 *   - 使用建议: 回调中需判断 fregs 是否为 NULL
 *
 * RECURSION (BIT 4)
 *   - 含义: 回调需要 ftrace 基础设施提供递归保护
 *   - 背景: ftrace 回调可能在被跟踪函数执行时再次触发，造成无限递归
 *   - 不设置时: 回调自己负责防递归（性能更好但实现复杂）
 *
 * STUB (BIT 5)
 *   - 含义: 占位符 ops，不实际执行跟踪
 *   - 用途: 保留 trampoline 地址等内部用途
 *
 * INITIALIZED (BIT 6)
 *   - 含义: ops 已完成初始化（第一次注册时初始化）
 *   - 管理: 由 register_ftrace_function 设置
 *
 * DELETED (BIT 7)
 *   - 含义: ops 正在被删除，阻止重复注册
 *
 * ADDING/REMOVING/MODIFYING (BIT 8/9/10)
 *   - 含义: ops 正处于过渡状态（并发安全用）
 *
 * ALLOC_TRAMP (BIT 11)
 *   - 含义: 架构核心为此 ops 动态分配了 trampoline
 *   - 注意: 用户设置的 trampoline 不能被架构代码修改；
 *     只有架构 ftrace 核心代码才能设置此标志
 *
 * IPMODIFY (BIT 12)
 *   - 含义: 回调可修改 IP（指令指针），实现执行流重定向
 *   - 前提: 必须同时设置 SAVE_REGS
 *   - 限制: 同一函数只能有一个带 IPMODIFY 的 ops，否则注册失败
 *   - 用途: live kernel patching（kpatch, livepatch）
 *
 * PID (BIT 13)
 *   - 含义: 受 set_ftrace_pid 影响，可按 PID 过滤跟踪
 *
 * RCU (BIT 14)
 *   - 含义: 只在 RCU 正在监视时才调用回调
 *   - 用途: 避免在 RCU 空闲期（如 idle/nohz）调用
 *
 * TRACE_ARRAY (BIT 15)
 *   - 含义: ops->private 指向 trace_array 描述符
 *
 * PERMANENT (BIT 16)
 *   - 含义: 永久 ops，不受 ftrace_enabled=0 影响
 *   - 用途: 某些必须始终活跃的跟踪（如 live patching）
 *
 * DIRECT (BIT 17)
 *   - 含义: 直接函数调用辅助用，仅内部使用
 *
 * SUBOP (BIT 18)
 *   - 含义: 受另一个 ops（managed 字段）控制的子操作
 *
 * GRAPH (BIT 19)
 *   - 含义: 此 ops 是 fgraph_ops 结构的组成部分（函数图跟踪）
 */
enum {
	FTRACE_OPS_FL_ENABLED			= BIT(0),  /* ops 已注册激活 */
	FTRACE_OPS_FL_DYNAMIC			= BIT(1),  /* 动态分配，注销需等 RCU */
	FTRACE_OPS_FL_SAVE_REGS			= BIT(2),  /* 要求保存完整寄存器 */
	FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED	= BIT(3),  /* 尽量保存寄存器，不支持则忽略 */
	FTRACE_OPS_FL_RECURSION			= BIT(4),  /* 需要 ftrace 提供递归保护 */
	FTRACE_OPS_FL_STUB			= BIT(5),  /* 占位符 ops */
	FTRACE_OPS_FL_INITIALIZED		= BIT(6),  /* 已完成初始化 */
	FTRACE_OPS_FL_DELETED			= BIT(7),  /* 正在删除，禁止重新注册 */
	FTRACE_OPS_FL_ADDING			= BIT(8),  /* 正在添加中 */
	FTRACE_OPS_FL_REMOVING			= BIT(9),  /* 正在移除中 */
	FTRACE_OPS_FL_MODIFYING			= BIT(10), /* 正在修改过滤器 */
	FTRACE_OPS_FL_ALLOC_TRAMP		= BIT(11), /* 架构核心分配了 trampoline */
	FTRACE_OPS_FL_IPMODIFY			= BIT(12), /* 可修改 IP（live patching 用） */
	FTRACE_OPS_FL_PID			= BIT(13), /* 受 set_ftrace_pid 过滤影响 */
	FTRACE_OPS_FL_RCU			= BIT(14), /* 只在 RCU 监视期调用 */
	FTRACE_OPS_FL_TRACE_ARRAY		= BIT(15), /* private 指向 trace_array */
	FTRACE_OPS_FL_PERMANENT                 = BIT(16), /* 永久有效，不受 ftrace_enabled 影响 */
	FTRACE_OPS_FL_DIRECT			= BIT(17), /* 直接函数调用（内部使用）*/
	FTRACE_OPS_FL_SUBOP			= BIT(18), /* 被另一 ops 管理的子操作 */
	FTRACE_OPS_FL_GRAPH			= BIT(19), /* fgraph_ops 组成部分 */
};

#ifndef CONFIG_DYNAMIC_FTRACE_WITH_ARGS
/*
 * FTRACE_OPS_FL_SAVE_ARGS - 兼容性宏：保存参数
 * 在不支持 DYNAMIC_FTRACE_WITH_ARGS 的架构上，
 * "保存参数" 等价于 "保存完整寄存器"
 */
#define FTRACE_OPS_FL_SAVE_ARGS                        FTRACE_OPS_FL_SAVE_REGS
#else
/*
 * 支持 DYNAMIC_FTRACE_WITH_ARGS 时不需要此标志
 * （参数总是可通过 ftrace_regs_get_argument 获取）
 */
#define FTRACE_OPS_FL_SAVE_ARGS                        0
#endif

/*
 * FTRACE_OPS_CMD_* commands allow the ftrace core logic to request changes
 * to a ftrace_ops. Note, the requests may fail.
 *
 * ENABLE_SHARE_IPMODIFY_SELF - enable a DIRECT ops to work on the same
 *                              function as an ops with IPMODIFY. Called
 *                              when the DIRECT ops is being registered.
 *                              This is called with both direct_mutex and
 *                              ftrace_lock are locked.
 *
 * ENABLE_SHARE_IPMODIFY_PEER - enable a DIRECT ops to work on the same
 *                              function as an ops with IPMODIFY. Called
 *                              when the other ops (the one with IPMODIFY)
 *                              is being registered.
 *                              This is called with direct_mutex locked.
 *
 * DISABLE_SHARE_IPMODIFY_PEER - disable a DIRECT ops to work on the same
 *                               function as an ops with IPMODIFY. Called
 *                               when the other ops (the one with IPMODIFY)
 *                               is being unregistered.
 *                               This is called with direct_mutex locked.
 */
/*
 * enum ftrace_ops_cmd - ftrace 操作命令
 *
 * 允许 ftrace 核心向 ftrace_ops 请求更改。请求可能失败。
 *
 * 背景: IPMODIFY 标志通常互斥（一个函数只能有一个 IPMODIFY ops），
 * 但某些情况下 DIRECT ops（直接调用）可与 IPMODIFY 共存，
 * 此时需通过 ops_func 回调协商权限。
 *
 * ENABLE_SHARE_IPMODIFY_SELF:
 *   - 场景: 正在注册带 DIRECT 标志的 ops，但目标函数已被带 IPMODIFY 的 ops 占用
 *   - 请求对象: 正在注册的 DIRECT ops 自己
 *   - 调用时机: DIRECT ops 注册时
 *   - 锁: 同时持有 direct_mutex 和 ftrace_lock
 *   - 返回 0: 允许共存；非 0: 拒绝，注册失败
 *
 * ENABLE_SHARE_IPMODIFY_PEER:
 *   - 场景: 正在注册带 IPMODIFY 的 ops，但目标函数已被 DIRECT ops 占用
 *   - 请求对象: 已存在的 DIRECT ops（peer，对端）
 *   - 调用时机: 带 IPMODIFY 的 ops 注册时
 *   - 锁: 持有 direct_mutex
 *   - 返回 0: 允许共存；非 0: 拒绝，注册失败
 *
 * DISABLE_SHARE_IPMODIFY_PEER:
 *   - 场景: 带 IPMODIFY 的 ops 正在注销
 *   - 请求对象: 与其共存的 DIRECT ops
 *   - 调用时机: 带 IPMODIFY 的 ops 注销时
 *   - 锁: 持有 direct_mutex
 *   - 作用: 通知 DIRECT ops 停止与 IPMODIFY ops 的交互
 */
enum ftrace_ops_cmd {
	FTRACE_OPS_CMD_ENABLE_SHARE_IPMODIFY_SELF,   /* 请求自己（DIRECT）与 IPMODIFY 共存 */
	FTRACE_OPS_CMD_ENABLE_SHARE_IPMODIFY_PEER,   /* 请求对端（DIRECT）与 IPMODIFY 共存 */
	FTRACE_OPS_CMD_DISABLE_SHARE_IPMODIFY_PEER,  /* 通知对端（DIRECT）停止共存 */
};

/*
 * For most ftrace_ops_cmd,
 * Returns:
 *        0 - Success.
 *        Negative on failure. The return value is dependent on the
 *        callback.
 */
/*
 * ftrace_ops_func_t - ftrace_ops 命令回调函数类型
 * @op:  正在被请求更改的 ftrace_ops
 * @ip:  相关函数地址（可用于判断是否接受请求）
 * @cmd: 命令类型（enum ftrace_ops_cmd）
 *
 * 返回值:
 *   0      - 成功接受请求
 *   负数   - 拒绝请求，具体含义由回调决定（通常是 -EBUSY, -EPERM 等）
 *
 * 注意: 此回调是可选的，只有需要响应命令的 ops 才需要设置
 * （主要用于 DIRECT ops 处理 IPMODIFY 共存协商）
 */
typedef int (*ftrace_ops_func_t)(struct ftrace_ops *op, unsigned long ip, enum ftrace_ops_cmd cmd);

#ifdef CONFIG_DYNAMIC_FTRACE

#define FTRACE_HASH_DEFAULT_BITS 10  /* 默认哈希表大小: 2^10 = 1024 个桶 */

/* alloc_ftrace_hash - 分配一个 ftrace 哈希表（用于存储过滤的函数地址集合） */
struct ftrace_hash *alloc_ftrace_hash(int size_bits);
/* free_ftrace_hash - 释放 ftrace 哈希表 */
void free_ftrace_hash(struct ftrace_hash *hash);
/* add_ftrace_hash_entry_direct - 向哈希表添加直接调用条目（同时记录直接跳转目标地址） */
struct ftrace_func_entry *add_ftrace_hash_entry_direct(struct ftrace_hash *hash,
						       unsigned long ip, unsigned long direct);
/* add_ftrace_hash_entry - 向哈希表添加普通条目（已分配好的 entry） */
void add_ftrace_hash_entry(struct ftrace_hash *hash, struct ftrace_func_entry *entry);
/* ftrace_hash_remove - 从哈希表中移除所有条目（清空但不释放哈希表本身） */
void ftrace_hash_remove(struct ftrace_hash *hash);

/*
 * struct ftrace_ops_hash - ftrace_ops 的过滤哈希表对
 *
 * 每个 ftrace_ops 有两个过滤哈希表：
 *   - filter_hash:  白名单，只跟踪此集合中的函数（为空则跟踪所有）
 *   - notrace_hash: 黑名单，不跟踪此集合中的函数（为空则不排除任何）
 *
 * 设计为独立结构的原因:
 * - 更新哈希表时需要 RCU 保护（__rcu 标记），旧版本不能立即释放
 * - 将过滤状态与主 ops 分开，便于原子替换整个过滤配置
 *
 * regex_lock: 保护哈希表更新的互斥锁（用户通过 debugfs 设置过滤时使用）
 */
/* The hash used to know what functions callbacks trace */
/* 哈希表：记录回调函数跟踪哪些函数 */
struct ftrace_ops_hash {
	struct ftrace_hash __rcu	*notrace_hash;  /* 黑名单：这些函数不跟踪 */
	struct ftrace_hash __rcu	*filter_hash;   /* 白名单：只跟踪这些函数（空=全部） */
	struct mutex			regex_lock;     /* 保护哈希表修改的互斥锁 */
};

/* ftrace_free_init_mem - 释放 init 段中的 ftrace 跟踪点元数据（内核启动后调用） */
void ftrace_free_init_mem(void);
/* ftrace_free_mem - 释放模块卸载或内存范围内的 ftrace 跟踪点元数据 */
void ftrace_free_mem(struct module *mod, void *start, void *end);
#else
static inline void ftrace_free_init_mem(void)
{
	ftrace_boot_snapshot();
}
static inline void ftrace_free_mem(struct module *mod, void *start, void *end) { }
#endif

/*
 * Note, ftrace_ops can be referenced outside of RCU protection, unless
 * the RCU flag is set. If ftrace_ops is allocated and not part of kernel
 * core data, the unregistering of it will perform a scheduling on all CPUs
 * to make sure that there are no more users. Depending on the load of the
 * system that may take a bit of time.
 *
 * Any private data added must also take care not to be freed and if private
 * data is added to a ftrace_ops that is in core code, the user of the
 * ftrace_ops must perform a schedule_on_each_cpu() before freeing it.
 */
/*
 * struct ftrace_ops - ftrace 操作描述符
 *
 * 注册给 ftrace 的"跟踪操作"，描述：回调函数是什么、跟踪哪些函数、
 * 以及各种行为控制标志。
 *
 * 生命周期:
 * 1. 初始化: 清零结构，设置 func、flags（可选 private）
 * 2. 注册: register_ftrace_function(ops) — ftrace 开始调用 ops->func
 * 3. 注销: unregister_ftrace_function(ops)
 *
 * 注意事项:
 * - ops 必须是静态变量（static），推荐加 __read_mostly
 * - 注销后 next 指针内部可能仍被使用，不要修改它
 * - 若 ops 是动态分配的，注销后需等待 RCU 宽限期再释放
 *   （内核会对所有 CPU 进行调度以确保没有使用者）
 * - private 中的数据在 ops 注销后不能立即释放；
 *   如在核心代码中，需先调用 schedule_on_each_cpu()
 *
 * 典型使用示例（跟踪所有函数入口）:
 *   static struct ftrace_ops my_ops = {
 *       .func = my_callback,
 *       .flags = FTRACE_OPS_FL_SAVE_REGS,
 *   };
 *   register_ftrace_function(&my_ops);
 */
struct ftrace_ops {
	ftrace_func_t			func;           /* 跟踪回调函数（每次命中时调用） */
	struct ftrace_ops __rcu		*next;          /* RCU 保护的链表，指向下一个 ops（内部使用） */
	unsigned long			flags;          /* FTRACE_OPS_FL_* 标志位组合 */
	void				*private;       /* 用户私有数据，可在回调中通过 op->private 访问 */
	ftrace_func_t			saved_func;     /* 保存的原始函数（切换时临时存储） */
#ifdef CONFIG_DYNAMIC_FTRACE
	struct ftrace_ops_hash		local_hash;     /* 本地哈希表（未激活时使用的暂存副本） */
	struct ftrace_ops_hash		*func_hash;     /* 当前激活的过滤哈希表指针（通常指向 local_hash） */
	struct ftrace_ops_hash		old_hash;       /* 更新过滤时保留旧哈希表（RCU 延迟释放） */
	unsigned long			trampoline;     /* 架构特定 trampoline 地址（0=使用默认） */
	unsigned long			trampoline_size;/* trampoline 代码大小（字节） */
	struct list_head		list;           /* 链接到全局 ftrace_ops 链表 */
	struct list_head		subop_list;     /* 子操作链表（SUBOP 特性使用） */
	ftrace_ops_func_t		ops_func;       /* 接收 ftrace_ops_cmd 命令的回调（可选） */
	struct ftrace_ops		*managed;       /* SUBOP: 指向管理此 ops 的父 ops */
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS
	unsigned long			direct_call;    /* 直接调用的目标地址（DIRECT 特性） */
#endif
#endif
};

extern struct ftrace_ops __rcu *ftrace_ops_list;  /* 全局 ftrace_ops 链表头（RCU 保护） */
extern struct ftrace_ops ftrace_list_end;          /* 链表哨兵节点，标记链表结束 */

/*
 * Traverse the ftrace_ops_list, invoking all entries.  The reason that we
 * can use rcu_dereference_raw_check() is that elements removed from this list
 * are simply leaked, so there is no need to interact with a grace-period
 * mechanism.  The rcu_dereference_raw_check() calls are needed to handle
 * concurrent insertions into the ftrace_ops_list.
 *
 * Silly Alpha and silly pointer-speculation compiler optimizations!
 */
/*
 * do_for_each_ftrace_op / while_for_each_ftrace_op - 遍历 ftrace_ops 链表宏
 *
 * 用法（成对使用，类似 do-while）:
 *   do_for_each_ftrace_op(op, ftrace_ops_list) {
 *       // 处理 op
 *   } while_for_each_ftrace_op(op);
 *
 * 设计说明:
 * - 使用 rcu_dereference_raw_check() 而非标准 rcu_dereference():
 *   因为从链表移除的 ops 直接"泄漏"（不立即释放），无需等待 RCU 宽限期，
 *   因此不需要与 grace-period 机制交互
 * - 需要 rcu_dereference 系列宏是为了处理并发插入（防止编译器/CPU 的
 *   指针推测优化，Alpha 架构尤其需要此保护）
 *
 * while_for_each_ftrace_op 优化:
 * - likely(op->next): 多数情况下链表有下一个元素（优化预测）
 * - unlikely(op != &ftrace_list_end): 链表结束是少数情况
 */
#define do_for_each_ftrace_op(op, list)			\
	op = rcu_dereference_raw_check(list);			\
	do

/*
 * Optimized for just a single item in the list (as that is the normal case).
 */
/* 针对单元素链表优化（最常见情况：只有一个 ftrace_ops 注册） */
#define while_for_each_ftrace_op(op)				\
	while (likely(op = rcu_dereference_raw_check((op)->next)) &&	\
	       unlikely((op) != &ftrace_list_end))

/*
 * Type of the current tracing.
 */
/*
 * enum ftrace_tracing_type_t - 当前跟踪类型
 *
 * FTRACE_TYPE_ENTER:  在函数入口处钩入（默认模式）
 *   - 可读取/修改函数参数
 *   - 可修改 IP 实现跳转（live patching）
 *
 * FTRACE_TYPE_RETURN: 在函数返回处钩入（函数图跟踪器使用）
 *   - 可读取/修改返回值
 *   - 用于测量函数执行时间
 */
enum ftrace_tracing_type_t {
	FTRACE_TYPE_ENTER = 0, /* Hook the call of the function */
	                       /* 在函数调用处钩入（函数入口） */
	FTRACE_TYPE_RETURN,    /* Hook the return of the function */
	                       /* 在函数返回处钩入 */
};

/* Current tracing type, default is FTRACE_TYPE_ENTER */
/* 当前跟踪类型，默认为 FTRACE_TYPE_ENTER（函数入口） */
extern enum ftrace_tracing_type_t ftrace_tracing_type;

/*
 * The ftrace_ops must be a static and should also
 * be read_mostly.  These functions do modify read_mostly variables
 * so use them sparely. Never free an ftrace_op or modify the
 * next pointer after it has been registered. Even after unregistering
 * it, the next pointer may still be used internally.
 */
/*
 * register_ftrace_function - 注册 ftrace 操作，开始跟踪
 * @ops: 已初始化的 ftrace_ops 指针（必须是 static 变量）
 * 返回值: 0 成功；负数 errno 失败
 *
 * 注意事项:
 * - ops 必须是 static，建议加 __read_mostly
 * - 注册后不要修改 ops->next 或释放 ops
 * - 这些函数会修改 read_mostly 变量，开销较大，不要频繁调用
 */
int register_ftrace_function(struct ftrace_ops *ops);

/*
 * unregister_ftrace_function - 注销 ftrace 操作，停止跟踪
 * @ops: 已注册的 ftrace_ops 指针
 * 返回值: 0 成功；负数 errno 失败
 *
 * 注意: 注销后 ops->next 指针内部可能仍被使用，不要修改它
 */
int unregister_ftrace_function(struct ftrace_ops *ops);

/* ftrace_stub - 默认空回调，不做任何操作（用作占位符） */
extern void ftrace_stub(unsigned long a0, unsigned long a1,
			struct ftrace_ops *op, struct ftrace_regs *fregs);

/*
 * ftrace_lookup_symbols - 批量查找符号地址
 * @sorted_syms: 按字母顺序排列的符号名数组（必须预先排序！）
 * @cnt:         数组元素个数
 * @addrs:       输出：对应符号的地址数组（调用者分配）
 * 返回值: 0 成功；负数 errno 失败（某个符号找不到）
 *
 * 注意: 输入数组必须按字母序排列，内部使用二分查找优化性能
 */
int ftrace_lookup_symbols(const char **sorted_syms, size_t cnt, unsigned long *addrs);
#else /* !CONFIG_FUNCTION_TRACER */
/*
 * (un)register_ftrace_function must be a macro since the ops parameter
 * must not be evaluated.
 */
/* 未启用 FUNCTION_TRACER 时，所有函数退化为空操作或错误返回 */
#define register_ftrace_function(ops) ({ 0; })
#define unregister_ftrace_function(ops) ({ 0; })
static inline void ftrace_kill(void) { }
static inline void ftrace_free_init_mem(void) { }
static inline void ftrace_free_mem(struct module *mod, void *start, void *end) { }
static inline int ftrace_lookup_symbols(const char **sorted_syms, size_t cnt, unsigned long *addrs)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_FUNCTION_TRACER */

/*
 * struct ftrace_func_entry - 哈希表中的单个函数条目
 * @hlist:  哈希链表节点（用于哈希桶冲突链）
 * @ip:     被跟踪函数的地址
 * @direct: 直接调用目标地址（仅在 DIRECT_CALLS 模式下使用）
 *
 * 用途: ftrace_hash 的元素，记录要跟踪（或排除）的函数地址
 */
struct ftrace_func_entry {
	struct hlist_node hlist;
	unsigned long ip;
	unsigned long direct; /* for direct lookup only */
	                      /* 仅用于直接调用查找 */
};

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS
/*
 * ftrace_find_rec_direct - 查找指定地址的直接调用记录
 * @ip: 函数地址
 * 返回值: 直接调用的目标地址；0 表示此地址没有直接调用
 *
 * 用途: 检查某个函数是否被 DIRECT 模式跟踪
 */
unsigned long ftrace_find_rec_direct(unsigned long ip);

/*
 * register_ftrace_direct - 注册直接调用的 ftrace_ops
 * @ops:  ftrace_ops，必须设置 FTRACE_OPS_FL_DIRECT
 * @addr: 直接调用的目标函数地址（被跟踪函数直接跳到此地址）
 * 返回值: 0 成功；负数 errno 失败
 *
 * DIRECT 模式说明:
 * - 正常 ftrace: 函数入口 → ftrace trampoline → 回调 → 原函数
 * - DIRECT 模式: 函数入口 → 直接跳到 addr（绕过 ftrace 基础设施）
 * - 优点: 开销极低，适合高频调用路径（如 BPF JIT 编译的跟踪器）
 * - 限制: 不能与其他 ftrace_ops 共存（除非协商 IPMODIFY）
 */
int register_ftrace_direct(struct ftrace_ops *ops, unsigned long addr);

/*
 * unregister_ftrace_direct - 注销直接调用
 * @ops:          已注册的 DIRECT ops
 * @addr:         之前注册时的目标地址
 * @free_filters: 是否释放过滤器（通常为 true）
 */
int unregister_ftrace_direct(struct ftrace_ops *ops, unsigned long addr,
			     bool free_filters);

/*
 * modify_ftrace_direct - 修改直接调用的目标地址（持锁版本）
 * @ops:  已注册的 DIRECT ops
 * @addr: 新的目标地址
 */
int modify_ftrace_direct(struct ftrace_ops *ops, unsigned long addr);

/*
 * modify_ftrace_direct_nolock - 修改直接调用目标地址（调用者已持锁）
 * 内部使用，避免重复加锁
 */
int modify_ftrace_direct_nolock(struct ftrace_ops *ops, unsigned long addr);

/* update_ftrace_direct_add/del/mod - 更新直接调用的过滤哈希表 */
int update_ftrace_direct_add(struct ftrace_ops *ops, struct ftrace_hash *hash);
int update_ftrace_direct_del(struct ftrace_ops *ops, struct ftrace_hash *hash);
int update_ftrace_direct_mod(struct ftrace_ops *ops, struct ftrace_hash *hash, bool do_direct_lock);

/* ftrace_stub_direct_tramp - 直接调用的桩 trampoline */
void ftrace_stub_direct_tramp(void);

/* ftrace_hash_count - 计算哈希表中的函数数量 */
unsigned long ftrace_hash_count(struct ftrace_hash *hash);

#else
struct ftrace_ops;
static inline unsigned long ftrace_find_rec_direct(unsigned long ip)
{
	return 0;
}
static inline int register_ftrace_direct(struct ftrace_ops *ops, unsigned long addr)
{
	return -ENODEV;
}
static inline int unregister_ftrace_direct(struct ftrace_ops *ops, unsigned long addr,
					   bool free_filters)
{
	return -ENODEV;
}
static inline int modify_ftrace_direct(struct ftrace_ops *ops, unsigned long addr)
{
	return -ENODEV;
}
static inline int modify_ftrace_direct_nolock(struct ftrace_ops *ops, unsigned long addr)
{
	return -ENODEV;
}

static inline int update_ftrace_direct_add(struct ftrace_ops *ops, struct ftrace_hash *hash)
{
	return -ENODEV;
}

static inline int update_ftrace_direct_del(struct ftrace_ops *ops, struct ftrace_hash *hash)
{
	return -ENODEV;
}

static inline int update_ftrace_direct_mod(struct ftrace_ops *ops, struct ftrace_hash *hash, bool do_direct_lock)
{
	return -ENODEV;
}

static inline unsigned long ftrace_hash_count(struct ftrace_hash *hash)
{
	return 0;
}

/*
 * This must be implemented by the architecture.
 * It is the way the ftrace direct_ops helper, when called
 * via ftrace (because there's other callbacks besides the
 * direct call), can inform the architecture's trampoline that this
 * routine has a direct caller, and what the caller is.
 *
 * For example, in x86, it returns the direct caller
 * callback function via the regs->orig_ax parameter.
 * Then in the ftrace trampoline, if this is set, it makes
 * the return from the trampoline jump to the direct caller
 * instead of going back to the function it just traced.
 */
/*
 * arch_ftrace_set_direct_caller - 通知架构 trampoline 有直接调用者
 * @fregs: ftrace 寄存器快照
 * @addr:  直接调用者地址
 *
 * 背景: 当一个函数既有 DIRECT ops 又有其他普通 ops 时，
 * ftrace 需要先调用普通 ops，再跳到 DIRECT 目标。
 *
 * 此函数将 DIRECT 目标地址传给架构 trampoline（例如 x86 通过 regs->orig_ax），
 * trampoline 返回时直接跳到 DIRECT 目标，而非回到被跟踪函数。
 */
static inline void arch_ftrace_set_direct_caller(struct ftrace_regs *fregs,
						 unsigned long addr) { }
#endif /* CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS */

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_JMP
/*
 * 某些架构使用地址的最低位标记"这是跳转而非调用"
 * （最低位=1: 跳转；=0: 调用）
 * 因为指令地址通常是对齐的，最低位可安全用作标志位
 */

/* ftrace_is_jmp - 判断地址是否标记为跳转 */
static inline bool ftrace_is_jmp(unsigned long addr)
{
	return addr & 1;
}

/* ftrace_jmp_set - 将地址标记为跳转 */
static inline unsigned long ftrace_jmp_set(unsigned long addr)
{
	return addr | 1UL;
}

/* ftrace_jmp_get - 清除跳转标记，获取真实地址 */
static inline unsigned long ftrace_jmp_get(unsigned long addr)
{
	return addr & ~1UL;
}
#else
static inline bool ftrace_is_jmp(unsigned long addr)
{
	return false;
}

static inline unsigned long ftrace_jmp_set(unsigned long addr)
{
	return addr;
}

static inline unsigned long ftrace_jmp_get(unsigned long addr)
{
	return addr;
}
#endif /* CONFIG_DYNAMIC_FTRACE_WITH_JMP */

#ifdef CONFIG_STACK_TRACER

/* stack_trace_sysctl - 栈跟踪器的 sysctl 处理函数 */
int stack_trace_sysctl(const struct ctl_table *table, int write, void *buffer,
		       size_t *lenp, loff_t *ppos);

/* DO NOT MODIFY THIS VARIABLE DIRECTLY! */
/* 不要直接修改此变量！使用下面的 helper 函数 */
DECLARE_PER_CPU(int, disable_stack_tracer);

/**
 * stack_tracer_disable - temporarily disable the stack tracer
 *
 * There's a few locations (namely in RCU) where stack tracing
 * cannot be executed. This function is used to disable stack
 * tracing during those critical sections.
 *
 * This function must be called with preemption or interrupts
 * disabled and stack_tracer_enable() must be called shortly after
 * while preemption or interrupts are still disabled.
 */
/*
 * stack_tracer_disable - 临时禁用栈跟踪器
 *
 * 某些位置（尤其是 RCU 内部）不能执行栈跟踪（可能递归或死锁）。
 * 此函数用于在这些关键区域禁用栈跟踪。
 *
 * 调用要求:
 * - 必须在关闭抢占或中断的情况下调用
 * - 必须尽快调用 stack_tracer_enable() 恢复
 *
 * 实现: 使用 per-CPU 计数器（支持嵌套禁用）
 */
static inline void stack_tracer_disable(void)
{
	/* Preemption or interrupts must be disabled */
	if (IS_ENABLED(CONFIG_DEBUG_PREEMPT))
		WARN_ON_ONCE(!preempt_count() || !irqs_disabled());
	this_cpu_inc(disable_stack_tracer);
}

/**
 * stack_tracer_enable - re-enable the stack tracer
 *
 * After stack_tracer_disable() is called, stack_tracer_enable()
 * must be called shortly afterward.
 */
/*
 * stack_tracer_enable - 重新启用栈跟踪器
 * 与 stack_tracer_disable() 成对使用
 */
static inline void stack_tracer_enable(void)
{
	if (IS_ENABLED(CONFIG_DEBUG_PREEMPT))
		WARN_ON_ONCE(!preempt_count() || !irqs_disabled());
	this_cpu_dec(disable_stack_tracer);
}
#else
static inline void stack_tracer_disable(void) { }
static inline void stack_tracer_enable(void) { }
#endif

/*
 * enum - ftrace 更新命令标志
 *
 * FTRACE_UPDATE_CALLS: 更新函数调用点（激活/停用跟踪）
 * FTRACE_DISABLE_CALLS: 禁用所有调用点
 * FTRACE_UPDATE_TRACE_FUNC: 更新跟踪函数指针
 * FTRACE_START_FUNC_RET: 启动函数返回跟踪（graph tracer）
 * FTRACE_STOP_FUNC_RET: 停止函数返回跟踪
 * FTRACE_MAY_SLEEP: 此更新操作可能睡眠（允许调度）
 */
enum {
	FTRACE_UPDATE_CALLS		= (1 << 0),
	FTRACE_DISABLE_CALLS		= (1 << 1),
	FTRACE_UPDATE_TRACE_FUNC	= (1 << 2),
	FTRACE_START_FUNC_RET		= (1 << 3),
	FTRACE_STOP_FUNC_RET		= (1 << 4),
	FTRACE_MAY_SLEEP		= (1 << 5),
};

/* Arches can override ftrace_get_symaddr() to convert fentry_ip to symaddr. */
/*
 * ftrace_get_symaddr - 从 fentry_ip 获取符号地址
 * @fentry_ip: ftrace 插桩位置的地址
 * 返回值: 符号地址（快速路径）；0 表示无快速路径，需使用 kallsyms API
 *
 * 某些架构的 ftrace 插桩地址与符号地址不同（例如函数入口+偏移），
 * 此函数提供快速转换。默认实现返回 0（无快速路径）。
 */
#ifndef ftrace_get_symaddr
#define ftrace_get_symaddr(fentry_ip) (0)
#endif

/* ftrace_sync_ipi - ftrace 同步 IPI 处理函数（用于代码修改后的 CPU 同步） */
void ftrace_sync_ipi(void *data);

#ifdef CONFIG_DYNAMIC_FTRACE

/* ftrace_arch_code_modify_prepare - 修改函数代码前的架构准备（如关闭写保护） */
void ftrace_arch_code_modify_prepare(void);
/* ftrace_arch_code_modify_post_process - 代码修改完成后的架构清理（如恢复写保护） */
void ftrace_arch_code_modify_post_process(void);

/*
 * enum ftrace_bug_type - ftrace 内部 bug 类型
 * 用于 ftrace_bug() 报告具体发生了什么错误
 *
 * UNKNOWN: 未知错误
 * INIT:    初始化阶段错误（期望看到原始 mcount 调用，但内容不符）
 * NOP:     期望是 NOP 指令，但实际不是
 * CALL:    期望是跟踪调用，但实际不是
 * UPDATE:  代码更新失败
 */
enum ftrace_bug_type {
	FTRACE_BUG_UNKNOWN,  /* 未知 bug */
	FTRACE_BUG_INIT,     /* 初始化时内容不符 */
	FTRACE_BUG_NOP,      /* 期望 NOP 但不是 */
	FTRACE_BUG_CALL,     /* 期望跟踪调用但不是 */
	FTRACE_BUG_UPDATE,   /* 代码更新失败 */
};
extern enum ftrace_bug_type ftrace_bug_type;

/*
 * ftrace_expected - 期望的调用点内容
 * 架构代码可将此指针指向期望的字节序列，
 * ftrace_bug() 报告时会打印出来用于诊断
 */
extern const void *ftrace_expected;

/*
 * ftrace_bug - 报告 ftrace 内部错误
 * @err: 错误码
 * @rec: 出错的 dyn_ftrace 跟踪点记录
 *
 * 打印诊断信息（包括函数地址、期望内容、实际内容），
 * 通常在代码修改时发现不一致时调用
 */
void ftrace_bug(int err, struct dyn_ftrace *rec);

struct seq_file;

/* ftrace_text_reserved - 检查地址范围是否被 ftrace 保留（不可随意修改） */
extern int ftrace_text_reserved(const void *start, const void *end);

/* ftrace_ops_trampoline - 查找负责某地址的 ftrace_ops trampoline */
struct ftrace_ops *ftrace_ops_trampoline(unsigned long addr);

/* is_ftrace_trampoline - 判断一个地址是否是 ftrace trampoline */
bool is_ftrace_trampoline(unsigned long addr);

/*
 * The dyn_ftrace record's flags field is split into two parts.
 * the first part which is '0-FTRACE_REF_MAX' is a counter of
 * the number of callbacks that have registered the function that
 * the dyn_ftrace descriptor represents.
 *
 * The second part is a mask:
 *  ENABLED - the function is being traced
 *  REGS    - the record wants the function to save regs
 *  REGS_EN - the function is set up to save regs.
 *  IPMODIFY - the record allows for the IP address to be changed.
 *  DISABLED - the record is not ready to be touched yet
 *  DIRECT   - there is a direct function to call
 *  CALL_OPS - the record can use callsite-specific ops
 *  CALL_OPS_EN - the function is set up to use callsite-specific ops
 *  TOUCHED  - A callback was added since boot up
 *  MODIFIED - The function had IPMODIFY or DIRECT attached to it
 *
 * When a new ftrace_ops is registered and wants a function to save
 * pt_regs, the rec->flags REGS is set. When the function has been
 * set up to save regs, the REG_EN flag is set. Once a function
 * starts saving regs it will do so until all ftrace_ops are removed
 * from tracing that function.
 */
/*
 * enum - dyn_ftrace 每个跟踪点的标志位（存在 flags 字段中）
 *
 * flags 字段分为两部分:
 *   - 低 19 位（bits 0-18）: 引用计数，表示有多少个 ftrace_ops 在跟踪此函数
 *     最大值为 FTRACE_REF_MAX（2^19 - 1 = 524287）
 *   - 高位（bits 19-31）: 状态标志位，含义如下
 *
 * 区分"想要"和"已启用"的标志对（如 REGS/REGS_EN, TRAMP/TRAMP_EN）:
 * - 第一个（REGS/TRAMP 等）: 某个 ops "希望"开启此功能
 * - 第二个（REGS_EN/TRAMP_EN 等）: 代码实际上已经被修改为该模式
 * - 两者都需要是因为代码修改是批量异步完成的（stop_machine），
 *   "想要"和"已启用"之间存在窗口期
 *
 * ENABLED (BIT 31): 此函数当前正在被跟踪
 *
 * REGS (BIT 30):    某 ops 要求此函数保存寄存器
 * REGS_EN (BIT 29): 函数已被修改为保存寄存器模式
 *
 * TRAMP (BIT 28):   某 ops 需要为此函数使用专属 trampoline
 * TRAMP_EN (BIT 27): 函数已被修改为使用专属 trampoline
 *
 * IPMODIFY (BIT 26): 某 ops 需要修改 IP（live patching）
 *
 * DISABLED (BIT 25): 此记录尚未准备好（init 阶段或模块加载中）
 *
 * DIRECT (BIT 24):    此函数有直接调用
 * DIRECT_EN (BIT 23): 直接调用已激活
 *
 * CALL_OPS (BIT 22):    此函数可使用调用点专属 ops
 * CALL_OPS_EN (BIT 21): 调用点专属 ops 已激活
 *
 * TOUCHED (BIT 20): 启动后有回调注册过此函数（用于优化决策）
 *
 * MODIFIED (BIT 19): 此函数曾被设置过 IPMODIFY 或 DIRECT
 *                    （影响内存回收决策）
 */
enum {
	FTRACE_FL_ENABLED	= (1UL << 31),   /* 正在被跟踪 */
	FTRACE_FL_REGS		= (1UL << 30),   /* 需要保存寄存器 */
	FTRACE_FL_REGS_EN	= (1UL << 29),   /* 已修改为保存寄存器 */
	FTRACE_FL_TRAMP		= (1UL << 28),   /* 需要专属 trampoline */
	FTRACE_FL_TRAMP_EN	= (1UL << 27),   /* 已使用专属 trampoline */
	FTRACE_FL_IPMODIFY	= (1UL << 26),   /* 需要可修改 IP */
	FTRACE_FL_DISABLED	= (1UL << 25),   /* 未就绪（初始化中） */
	FTRACE_FL_DIRECT	= (1UL << 24),   /* 有直接调用 */
	FTRACE_FL_DIRECT_EN	= (1UL << 23),   /* 直接调用已激活 */
	FTRACE_FL_CALL_OPS	= (1UL << 22),   /* 可用调用点专属 ops */
	FTRACE_FL_CALL_OPS_EN	= (1UL << 21),   /* 调用点专属 ops 已激活 */
	FTRACE_FL_TOUCHED	= (1UL << 20),   /* 启动后有回调注册 */
	FTRACE_FL_MODIFIED	= (1UL << 19),   /* 曾被 IPMODIFY/DIRECT 使用 */
};

#define FTRACE_REF_MAX_SHIFT	19
/* FTRACE_REF_MAX: 单个函数最多可被 2^19-1 个 ops 同时跟踪 */
#define FTRACE_REF_MAX		((1UL << FTRACE_REF_MAX_SHIFT) - 1)

/* ftrace_rec_count - 读取 dyn_ftrace 记录的引用计数（注册的 ops 数量） */
#define ftrace_rec_count(rec)	((rec)->flags & FTRACE_REF_MAX)

/*
 * struct dyn_ftrace - 动态 ftrace 跟踪点记录
 *
 * 每个内核函数入口的 mcount/fentry 调用点对应一个此结构，
 * 存储在内核的 __mcount_loc 段，由 ftrace 核心管理。
 *
 * @ip:    mcount 调用点的地址（即函数入口处的插桩位置）
 * @flags: 引用计数（低19位）+ 状态标志（高13位）
 * @arch:  架构特定的扩展数据（如 x86 保存原始字节）
 *
 * 工作原理:
 * - 启动时: ip 处是 call mcount（或 call __fentry__）
 * - ftrace 初始化后: 替换为 NOP（nop 指令，无开销）
 * - 有 ops 注册跟踪此函数时: 替换为 call ftrace_caller
 * - 使用专属 trampoline 时: 替换为 call <trampoline_addr>
 */
struct dyn_ftrace {
	unsigned long		ip;    /* address of mcount call-site */
	                               /* mcount 调用点地址（函数入口） */
	unsigned long		flags; /* 引用计数(低19位) | 状态标志(高13位) */
	struct dyn_arch_ftrace	arch; /* 架构特定数据 */
};

/*
 * ftrace_set_filter_ip - 向 ops 的过滤器添加/删除单个函数地址
 * @ops:    目标 ftrace_ops
 * @ip:     函数地址
 * @remove: 0=添加到白名单；1=从白名单删除
 * @reset:  1=先清空白名单再操作；0=增量操作
 * 返回值: 0 成功；负数 errno
 */
int ftrace_set_filter_ip(struct ftrace_ops *ops, unsigned long ip,
			 int remove, int reset);

/*
 * ftrace_set_filter_ips - 批量添加/删除多个函数地址到过滤器
 * @ops:    目标 ftrace_ops
 * @ips:    函数地址数组
 * @cnt:    数组元素个数
 * @remove: 0=添加；1=删除
 * @reset:  1=先清空
 */
int ftrace_set_filter_ips(struct ftrace_ops *ops, unsigned long *ips,
			  unsigned int cnt, int remove, int reset);

/*
 * ftrace_set_filter - 通过函数名模式设置白名单过滤器
 * @ops:   目标 ftrace_ops
 * @buf:   函数名或通配符模式（如 "schedule*"）
 * @len:   buf 长度
 * @reset: 1=先清空
 *
 * 只有匹配 buf 的函数才会被跟踪（白名单）
 */
int ftrace_set_filter(struct ftrace_ops *ops, unsigned char *buf,
		       int len, int reset);

/*
 * ftrace_set_notrace - 通过函数名模式设置黑名单
 * @ops:   目标 ftrace_ops
 * @buf:   函数名或通配符模式
 * @len:   buf 长度
 * @reset: 1=先清空
 *
 * 匹配 buf 的函数不会被跟踪（黑名单）
 */
int ftrace_set_notrace(struct ftrace_ops *ops, unsigned char *buf,
			int len, int reset);

/* ftrace_set_global_filter/notrace - 设置全局过滤器（影响所有 ops） */
void ftrace_set_global_filter(unsigned char *buf, int len, int reset);
void ftrace_set_global_notrace(unsigned char *buf, int len, int reset);

/* ftrace_free_filter - 释放 ops 的过滤器哈希表 */
void ftrace_free_filter(struct ftrace_ops *ops);

/* ftrace_ops_set_global_filter - 让 ops 使用全局过滤器（而非私有过滤器） */
void ftrace_ops_set_global_filter(struct ftrace_ops *ops);

/*
 * The FTRACE_UPDATE_* enum is used to pass information back
 * from the ftrace_update_record() and ftrace_test_record()
 * functions. These are called by the code update routines
 * to find out what is to be done for a given function.
 *
 *  IGNORE           - The function is already what we want it to be
 *  MAKE_CALL        - Start tracing the function
 *  MODIFY_CALL      - Stop saving regs for the function
 *  MAKE_NOP         - Stop tracing the function
 */
/*
 * enum - ftrace_update_record/ftrace_test_record 的返回值
 *
 * 告知代码更新例程对某个函数需要做什么操作:
 *
 * IGNORE:       无需操作，当前状态已经是期望的状态
 * MAKE_CALL:    需要将 NOP 替换为跟踪调用（开始跟踪）
 * MODIFY_CALL:  需要修改调用（例如添加/去掉寄存器保存，或切换 trampoline）
 * MAKE_NOP:     需要将跟踪调用替换为 NOP（停止跟踪）
 */
enum {
	FTRACE_UPDATE_IGNORE,        /* 无需操作 */
	FTRACE_UPDATE_MAKE_CALL,     /* NOP → 跟踪调用（开始跟踪） */
	FTRACE_UPDATE_MODIFY_CALL,   /* 修改现有调用（改变模式） */
	FTRACE_UPDATE_MAKE_NOP,      /* 跟踪调用 → NOP（停止跟踪） */
};

/*
 * enum - ftrace 迭代器标志位（用于 debugfs 遍历跟踪点）
 *
 * FILTER:    遍历过滤器（白名单）中的函数
 * NOTRACE:   遍历 notrace（黑名单）中的函数
 * PRINTALL:  打印所有函数（不只是过滤的）
 * DO_PROBES: 遍历探针
 * PROBE:     当前项是探针
 * MOD:       遍历模块中的函数
 * ENABLED:   只显示已启用跟踪的函数
 * TOUCHED:   只显示启动后被访问过的函数
 * ADDRS:     显示函数地址而非名称
 */
enum {
	FTRACE_ITER_FILTER	= (1 << 0),   /* 过滤器白名单 */
	FTRACE_ITER_NOTRACE	= (1 << 1),   /* 黑名单 */
	FTRACE_ITER_PRINTALL	= (1 << 2),   /* 打印全部 */
	FTRACE_ITER_DO_PROBES	= (1 << 3),   /* 处理探针 */
	FTRACE_ITER_PROBE	= (1 << 4),   /* 当前是探针 */
	FTRACE_ITER_MOD		= (1 << 5),   /* 模块中的函数 */
	FTRACE_ITER_ENABLED	= (1 << 6),   /* 已启用跟踪 */
	FTRACE_ITER_TOUCHED	= (1 << 7),   /* 被访问过 */
	FTRACE_ITER_ADDRS	= (1 << 8),   /* 显示地址 */
};

/*
 * arch_ftrace_update_code - 架构实现：批量更新函数代码（NOP↔CALL 替换）
 * @command: FTRACE_UPDATE_* 标志位组合
 *
 * 通常通过 stop_machine 在所有 CPU 停止时执行，确保代码修改安全
 */
void arch_ftrace_update_code(int command);

/*
 * arch_ftrace_update_trampoline - 更新 ops 的 trampoline 代码
 * @ops: 需要更新 trampoline 的 ftrace_ops
 */
void arch_ftrace_update_trampoline(struct ftrace_ops *ops);

/*
 * arch_ftrace_trampoline_func - 获取 trampoline 实际调用的函数
 * @ops: ftrace_ops
 * @rec: dyn_ftrace 记录
 * 返回值: trampoline 中实际调用的目标函数地址
 */
void *arch_ftrace_trampoline_func(struct ftrace_ops *ops, struct dyn_ftrace *rec);

/*
 * arch_ftrace_trampoline_free - 释放 ops 的 trampoline 内存
 * @ops: ftrace_ops
 */
void arch_ftrace_trampoline_free(struct ftrace_ops *ops);

struct ftrace_rec_iter;  /* 遍历 dyn_ftrace 记录的迭代器（不透明类型） */

/* ftrace_rec_iter_start - 开始遍历所有 dyn_ftrace 记录 */
struct ftrace_rec_iter *ftrace_rec_iter_start(void);
/* ftrace_rec_iter_next - 移动到下一个记录 */
struct ftrace_rec_iter *ftrace_rec_iter_next(struct ftrace_rec_iter *iter);
/* ftrace_rec_iter_record - 获取迭代器当前指向的 dyn_ftrace 记录 */
struct dyn_ftrace *ftrace_rec_iter_record(struct ftrace_rec_iter *iter);

/* for_ftrace_rec_iter - 遍历所有 dyn_ftrace 记录的宏 */
#define for_ftrace_rec_iter(iter)		\
	for (iter = ftrace_rec_iter_start();	\
	     iter;				\
	     iter = ftrace_rec_iter_next(iter))


/*
 * ftrace_update_record - 根据当前状态更新 dyn_ftrace 记录，返回所需操作
 * @rec:    要更新的记录
 * @enable: true=启用跟踪；false=禁用跟踪
 * 返回值: FTRACE_UPDATE_* 枚举值
 */
int ftrace_update_record(struct dyn_ftrace *rec, bool enable);

/*
 * ftrace_test_record - 测试 dyn_ftrace 记录需要什么操作（不实际更新）
 * @rec:    要测试的记录
 * @enable: true=测试启用；false=测试禁用
 * 返回值: FTRACE_UPDATE_* 枚举值
 */
int ftrace_test_record(struct dyn_ftrace *rec, bool enable);

/* ftrace_run_stop_machine - 在 stop_machine 上下文执行 ftrace 代码更新命令 */
void ftrace_run_stop_machine(int command);

/* ftrace_location - 查找 ip 处的跟踪点地址（若不是跟踪点返回 0） */
unsigned long ftrace_location(unsigned long ip);
/* ftrace_location_range - 在地址范围内查找第一个跟踪点 */
unsigned long ftrace_location_range(unsigned long start, unsigned long end);
/* ftrace_get_addr_new - 获取 dyn_ftrace 记录期望的新调用地址（下次更新后的目标） */
unsigned long ftrace_get_addr_new(struct dyn_ftrace *rec);
/* ftrace_get_addr_curr - 获取 dyn_ftrace 记录当前的调用地址 */
unsigned long ftrace_get_addr_curr(struct dyn_ftrace *rec);

extern ftrace_func_t ftrace_trace_function;

int ftrace_regex_open(struct ftrace_ops *ops, int flag,
		  struct inode *inode, struct file *file);
ssize_t ftrace_filter_write(struct file *file, const char __user *ubuf,
			    size_t cnt, loff_t *ppos);
ssize_t ftrace_notrace_write(struct file *file, const char __user *ubuf,
			     size_t cnt, loff_t *ppos);
int ftrace_regex_release(struct inode *inode, struct file *file);

void __init
ftrace_set_early_filter(struct ftrace_ops *ops, char *buf, int enable);

/* defined in arch */
extern int ftrace_dyn_arch_init(void);
extern void ftrace_replace_code(int enable);
extern int ftrace_update_ftrace_func(ftrace_func_t func);
extern void ftrace_caller(void);
extern void ftrace_regs_caller(void);
extern void ftrace_call(void);
extern void ftrace_regs_call(void);
extern void mcount_call(void);

void ftrace_modify_all_code(int command);

#ifndef FTRACE_ADDR
#define FTRACE_ADDR ((unsigned long)ftrace_caller)
#endif

#ifndef FTRACE_GRAPH_ADDR
#define FTRACE_GRAPH_ADDR ((unsigned long)ftrace_graph_caller)
#endif

#ifndef FTRACE_REGS_ADDR
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_REGS
# define FTRACE_REGS_ADDR ((unsigned long)ftrace_regs_caller)
#else
# define FTRACE_REGS_ADDR FTRACE_ADDR
#endif
#endif

/*
 * If an arch would like functions that are only traced
 * by the function graph tracer to jump directly to its own
 * trampoline, then they can define FTRACE_GRAPH_TRAMP_ADDR
 * to be that address to jump to.
 */
/*
 * FTRACE_GRAPH_TRAMP_ADDR - 函数图跟踪器的直接跳转地址
 *
 * 设计原因:
 * - 若架构希望仅被函数图跟踪器跟踪的函数直接跳转到自定义蹦床，
 *   可定义此宏指向该地址
 * - 优化: 跳过通用ftrace分发路径，直接进入graph tracer
 *
 * 默认为0表示不使用此优化
 */
#ifndef FTRACE_GRAPH_TRAMP_ADDR
#define FTRACE_GRAPH_TRAMP_ADDR ((unsigned long) 0)
#endif

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
/*
 * ftrace_graph_caller - 函数图跟踪器的入口蹦床（汇编实现）
 *
 * 在函数入口记录调用关系，在出口测量执行时间
 */
extern void ftrace_graph_caller(void);

/*
 * ftrace_enable_ftrace_graph_caller - 激活graph tracer的代码路径
 *
 * 返回值: 0=成功，负数=失败
 *
 * 修改代码使函数入口跳转到ftrace_graph_caller
 */
extern int ftrace_enable_ftrace_graph_caller(void);

/*
 * ftrace_disable_ftrace_graph_caller - 停用graph tracer的代码路径
 *
 * 将跳转目标改为nop或恢复原状
 */
extern int ftrace_disable_ftrace_graph_caller(void);
#else
/* CONFIG_FUNCTION_GRAPH_TRACER未启用时的空桩实现 */
static inline int ftrace_enable_ftrace_graph_caller(void) { return 0; }
static inline int ftrace_disable_ftrace_graph_caller(void) { return 0; }
#endif

/**
 * ftrace_make_nop - convert code into nop
 * @mod: module structure if called by module load initialization
 * @rec: the call site record (e.g. mcount/fentry)
 * @addr: the address that the call site should be calling
 *
 * This is a very sensitive operation and great care needs
 * to be taken by the arch.  The operation should carefully
 * read the location, check to see if what is read is indeed
 * what we expect it to be, and then on success of the compare,
 * it should write to the location.
 *
 * The code segment at @rec->ip should be a caller to @addr
 *
 * Return must be:
 *  0 on success
 *  -EFAULT on error reading the location
 *  -EINVAL on a failed compare of the contents
 *  -EPERM  on error writing to the location
 * Any other value will be considered a failure.
 */
/*
 * ftrace_make_nop - 将调用点代码转换为nop指令
 * @mod: 模块结构（若由模块加载初始化调用），NULL表示内核代码
 * @rec: 调用点记录（如mcount/fentry位置）
 * @addr: 调用点当前应该调用的地址（用于验证）
 *
 * 这是一个非常敏感的操作，架构实现必须极其小心：
 * 1. 仔细读取@rec->ip位置的指令
 * 2. 检查读取的内容是否确实是期望的（call @addr）
 * 3. 比对成功后才写入nop指令
 *
 * 设计原因:
 * - 读取-检查-写入序列防止意外修改非ftrace指令
 * - 模块代码可能与内核代码有不同的地址空间布局
 *
 * 前置条件: @rec->ip处的代码应该是对@addr的call指令
 *
 * 返回值:
 *  0        - 成功
 *  -EFAULT  - 读取位置出错（地址不可访问）
 *  -EINVAL  - 内容比对失败（指令不匹配期望）
 *  -EPERM   - 写入位置出错（代码页不可写）
 *  其他值   - 视为失败
 *
 * 注意事项:
 * - 必须原子地修改指令（使用text_poke等机制）
 * - 需要处理指令缓存（I-cache）一致性
 * - 可能在stop_machine上下文执行
 */
extern int ftrace_make_nop(struct module *mod,
			   struct dyn_ftrace *rec, unsigned long addr);

/**
 * ftrace_need_init_nop - return whether nop call sites should be initialized
 *
 * Normally the compiler's -mnop-mcount generates suitable nops, so we don't
 * need to call ftrace_init_nop() if the code is built with that flag.
 * Architectures where this is not always the case may define their own
 * condition.
 *
 * Return must be:
 *  0	    if ftrace_init_nop() should be called
 *  Nonzero if ftrace_init_nop() should not be called
 */
/*
 * ftrace_need_init_nop - 判断nop调用点是否需要初始化
 *
 * 设计原因:
 * - 编译器的-mnop-mcount选项会生成合适的nop，无需额外初始化
 * - 但某些架构/编译器组合生成的nop不符合要求，需要运行时修正
 *
 * 返回值:
 *  0    - 需要调用ftrace_init_nop()初始化
 *  非0  - 不需要调用ftrace_init_nop()
 *
 * 默认实现: 检查CC_USING_NOP_MCOUNT宏（编译器设置）
 * 架构可定义自己的条件覆盖此宏
 */
#ifndef ftrace_need_init_nop
#define ftrace_need_init_nop() (!__is_defined(CC_USING_NOP_MCOUNT))
#endif

/**
 * ftrace_init_nop - initialize a nop call site
 * @mod: module structure if called by module load initialization
 * @rec: the call site record (e.g. mcount/fentry)
 *
 * This is a very sensitive operation and great care needs
 * to be taken by the arch.  The operation should carefully
 * read the location, check to see if what is read is indeed
 * what we expect it to be, and then on success of the compare,
 * it should write to the location.
 *
 * The code segment at @rec->ip should contain the contents created by
 * the compiler
 *
 * Return must be:
 *  0 on success
 *  -EFAULT on error reading the location
 *  -EINVAL on a failed compare of the contents
 *  -EPERM  on error writing to the location
 * Any other value will be considered a failure.
 */
/*
 * ftrace_init_nop - 初始化nop调用点
 * @mod: 模块结构（若由模块加载初始化调用）
 * @rec: 调用点记录（如mcount/fentry位置）
 *
 * 与ftrace_make_nop的区别:
 * - ftrace_make_nop: 将活跃的call指令转为nop
 * - ftrace_init_nop: 修正编译器生成的nop为ftrace期望的格式
 *
 * 前置条件: @rec->ip处的代码应该包含编译器生成的内容（可能是不标准的nop）
 *
 * 返回值: 同ftrace_make_nop
 *
 * 默认实现: 调用ftrace_make_nop(mod, rec, MCOUNT_ADDR)
 * - 假设编译器生成的是对MCOUNT_ADDR的call
 * - 架构可覆盖此函数以处理特殊情况
 */
#ifndef ftrace_init_nop
static inline int ftrace_init_nop(struct module *mod, struct dyn_ftrace *rec)
{
	return ftrace_make_nop(mod, rec, MCOUNT_ADDR);
}
#endif

/**
 * ftrace_make_call - convert a nop call site into a call to addr
 * @rec: the call site record (e.g. mcount/fentry)
 * @addr: the address that the call site should call
 *
 * This is a very sensitive operation and great care needs
 * to be taken by the arch.  The operation should carefully
 * read the location, check to see if what is read is indeed
 * what we expect it to be, and then on success of the compare,
 * it should write to the location.
 *
 * The code segment at @rec->ip should be a nop
 *
 * Return must be:
 *  0 on success
 *  -EFAULT on error reading the location
 *  -EINVAL on a failed compare of the contents
 *  -EPERM  on error writing to the location
 * Any other value will be considered a failure.
 */
/*
 * ftrace_make_call - 将nop调用点转换为对addr的调用
 * @rec: 调用点记录（如mcount/fentry位置）
 * @addr: 调用点应该调用的目标地址
 *
 * ftrace_make_nop的反向操作，激活跟踪时使用
 *
 * 前置条件: @rec->ip处的代码应该是nop指令
 *
 * 返回值: 同ftrace_make_nop
 *
 * 设计原因:
 * - 动态ftrace的核心：运行时在nop和call之间切换
 * - 允许零开销的条件跟踪（未激活时无性能影响）
 *
 * 注意事项: 同ftrace_make_nop，需要原子修改和I-cache同步
 */
extern int ftrace_make_call(struct dyn_ftrace *rec, unsigned long addr);

#if defined(CONFIG_DYNAMIC_FTRACE_WITH_REGS) || \
	defined(CONFIG_DYNAMIC_FTRACE_WITH_CALL_OPS) || \
	defined(CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS)
/**
 * ftrace_modify_call - convert from one addr to another (no nop)
 * @rec: the call site record (e.g. mcount/fentry)
 * @old_addr: the address expected to be currently called to
 * @addr: the address to change to
 *
 * This is a very sensitive operation and great care needs
 * to be taken by the arch.  The operation should carefully
 * read the location, check to see if what is read is indeed
 * what we expect it to be, and then on success of the compare,
 * it should write to the location.
 *
 * When using call ops, this is called when the associated ops change, even
 * when (addr == old_addr).
 *
 * The code segment at @rec->ip should be a caller to @old_addr
 *
 * Return must be:
 *  0 on success
 *  -EFAULT on error reading the location
 *  -EINVAL on a failed compare of the contents
 *  -EPERM  on error writing to the location
 * Any other value will be considered a failure.
 */
/*
 * ftrace_modify_call - 将call从一个地址修改为另一个（不经过nop）
 * @rec: 调用点记录
 * @old_addr: 当前期望调用的地址（用于验证）
 * @addr: 要改为调用的新地址
 *
 * 与ftrace_make_call/nop的区别:
 * - make_call/nop: 在nop和call之间切换（启用/禁用跟踪）
 * - modify_call: 在两个call之间切换（更改跟踪目标）
 *
 * 前置条件: @rec->ip处应该是对@old_addr的call指令
 *
 * 典型用途:
 * - 切换蹦床（从ftrace_caller到ftrace_regs_caller）
 * - 更新DIRECT调用的目标
 * - 在不同ftrace_ops之间切换
 *
 * 特殊情况（call ops）:
 * - 即使addr == old_addr也会调用此函数
 * - 因为关联的ftrace_ops已更改，可能需要更新元数据
 *
 * 返回值: 同ftrace_make_nop
 */
extern int ftrace_modify_call(struct dyn_ftrace *rec, unsigned long old_addr,
			      unsigned long addr);
#else
/* Should never be called */
/* 不支持相关特性时的桩函数，调用会返回错误 */
static inline int ftrace_modify_call(struct dyn_ftrace *rec, unsigned long old_addr,
				     unsigned long addr)
{
	return -EINVAL;
}
#endif

/*
 * skip_trace - 判断指定地址是否应跳过跟踪
 * @ip: 指令指针地址
 *
 * 返回值: 1=跳过，0=不跳过
 *
 * 用途: 防止跟踪某些敏感函数（如ftrace自身的代码）
 */
extern int skip_trace(unsigned long ip);

/*
 * ftrace_module_init - 模块加载时的ftrace初始化
 * @mod: 被加载的模块
 *
 * 扫描模块中的mcount调用点，为每个创建dyn_ftrace记录
 */
extern void ftrace_module_init(struct module *mod);

/*
 * ftrace_module_enable - 模块加载完成后启用ftrace
 * @mod: 已加载的模块
 *
 * 根据当前ftrace_ops的filter/notrace设置激活模块中相应的跟踪点
 */
extern void ftrace_module_enable(struct module *mod);

/*
 * ftrace_release_mod - 模块卸载时释放ftrace资源
 * @mod: 被卸载的模块
 *
 * 移除模块的所有dyn_ftrace记录，更新filter哈希表
 */
extern void ftrace_release_mod(struct module *mod);
#else /* CONFIG_DYNAMIC_FTRACE */
/* CONFIG_DYNAMIC_FTRACE未启用时的空桩实现 */
static inline int skip_trace(unsigned long ip) { return 0; }
static inline void ftrace_module_init(struct module *mod) { }
static inline void ftrace_module_enable(struct module *mod) { }
static inline void ftrace_release_mod(struct module *mod) { }
static inline int ftrace_text_reserved(const void *start, const void *end)
{
	return 0;
}
static inline unsigned long ftrace_location(unsigned long ip)
{
	return 0;
}

/*
 * Again users of functions that have ftrace_ops may not
 * have them defined when ftrace is not enabled, but these
 * functions may still be called. Use a macro instead of inline.
 */
/*
 * 以下宏用于非动态ftrace配置，提供空操作实现
 * 使用宏而非inline函数，因为某些函数的参数类型在此配置下可能未定义
 */
#define ftrace_regex_open(ops, flag, inod, file) ({ -ENODEV; })
#define ftrace_set_early_filter(ops, buf, enable) do { } while (0)
#define ftrace_set_filter_ip(ops, ip, remove, reset) ({ -ENODEV; })
#define ftrace_set_filter_ips(ops, ips, cnt, remove, reset) ({ -ENODEV; })
#define ftrace_set_filter(ops, buf, len, reset) ({ -ENODEV; })
#define ftrace_set_notrace(ops, buf, len, reset) ({ -ENODEV; })
#define ftrace_free_filter(ops) do { } while (0)
#define ftrace_ops_set_global_filter(ops) do { } while (0)

static inline ssize_t ftrace_filter_write(struct file *file, const char __user *ubuf,
			    size_t cnt, loff_t *ppos) { return -ENODEV; }
static inline ssize_t ftrace_notrace_write(struct file *file, const char __user *ubuf,
			     size_t cnt, loff_t *ppos) { return -ENODEV; }
static inline int
ftrace_regex_release(struct inode *inode, struct file *file) { return -ENODEV; }

static inline bool is_ftrace_trampoline(unsigned long addr)
{
	return false;
}
#endif /* CONFIG_DYNAMIC_FTRACE */

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
#ifndef ftrace_graph_func
/*
 * ftrace_graph_func - 函数图跟踪器的回调函数
 *
 * 默认为ftrace_stub（空操作），架构可覆盖以提供优化实现
 */
# define ftrace_graph_func ftrace_stub
/*
 * FTRACE_OPS_GRAPH_STUB - 标记函数图ops为桩
 *
 * 当使用通用ftrace_stub时，标记为STUB防止不必要的处理
 */
# define FTRACE_OPS_GRAPH_STUB FTRACE_OPS_FL_STUB
/*
 * The function graph is called every time the function tracer is called.
 * It must always test the ops hash and cannot just directly call
 * the handler.
 */
/*
 * FGRAPH_NO_DIRECT - 禁止函数图跟踪器使用DIRECT调用优化
 *
 * 设计原因:
 * - 函数图在每次函数跟踪时都被调用
 * - 必须每次测试ops哈希表，不能直接调用处理器
 * - 因此不能使用绕过ftrace基础设施的DIRECT调用
 */
# define FGRAPH_NO_DIRECT	1
#else
/* 架构提供了自定义ftrace_graph_func */
# define FTRACE_OPS_GRAPH_STUB	0
# define FGRAPH_NO_DIRECT	0
#endif
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */

/* totally disable ftrace - can not re-enable after this */
/*
 * ftrace_kill - 永久禁用ftrace
 *
 * 警告: 调用后无法重新启用ftrace，仅用于严重错误情况
 *
 * 用途: 当ftrace自身出现bug导致系统不稳定时的紧急措施
 */
void ftrace_kill(void);

/*
 * tracer_disable - 临时禁用函数跟踪
 *
 * 设置ftrace_enabled=0，停止所有函数跟踪
 * 与ftrace_kill不同，此操作可逆
 */
static inline void tracer_disable(void)
{
#ifdef CONFIG_FUNCTION_TRACER
	ftrace_enabled = 0;
#endif
}

/*
 * Ftrace disable/restore without lock. Some synchronization mechanism
 * must be used to prevent ftrace_enabled to be changed between
 * disable/restore.
 */
/*
 * __ftrace_enabled_save - 保存并禁用ftrace状态（无锁版本）
 *
 * 返回值: 之前的ftrace_enabled值
 *
 * 注意事项:
 * - 调用者必须提供同步机制防止并发修改ftrace_enabled
 * - 用于性能关键路径，避免锁开销
 * - 必须与__ftrace_enabled_restore配对使用
 *
 * 典型用途: 临时禁用ftrace执行敏感操作
 */
static inline int __ftrace_enabled_save(void)
{
#ifdef CONFIG_FUNCTION_TRACER
	int saved_ftrace_enabled = ftrace_enabled;
	ftrace_enabled = 0;
	return saved_ftrace_enabled;
#else
	return 0;
#endif
}

/*
 * __ftrace_enabled_restore - 恢复ftrace状态（无锁版本）
 * @enabled: 之前保存的ftrace_enabled值
 *
 * 与__ftrace_enabled_save配对，恢复之前保存的状态
 */
static inline void __ftrace_enabled_restore(int enabled)
{
#ifdef CONFIG_FUNCTION_TRACER
	ftrace_enabled = enabled;
#endif
}

/* All archs should have this, but we define it for consistency */
/*
 * ftrace_return_address0 - 获取当前函数的返回地址
 *
 * 所有架构都应支持，使用编译器内建函数__builtin_return_address(0)
 * 返回值: 调用当前函数的返回地址
 */
#ifndef ftrace_return_address0
# define ftrace_return_address0 __builtin_return_address(0)
#endif

/* Archs may use other ways for ADDR1 and beyond */
/*
 * ftrace_return_address - 获取调用栈上第n层的返回地址
 * @n: 栈帧深度（0=当前函数，1=调用者，2=调用者的调用者，...）
 *
 * 依赖CONFIG_FRAME_POINTER:
 * - 启用时: 可通过帧指针遍历栈获取任意深度
 * - 禁用时: 只能返回0（栈遍历不可靠）
 *
 * 注意: 深度>0时需要帧指针支持，否则结果不可靠
 */
#ifndef ftrace_return_address
# ifdef CONFIG_FRAME_POINTER
#  define ftrace_return_address(n) __builtin_return_address(n)
# else
#  define ftrace_return_address(n) 0UL
# endif
#endif

/*
 * CALLER_ADDR0-6 - 便捷宏，获取调用栈各层的返回地址
 *
 * CALLER_ADDR0: 当前函数的返回地址
 * CALLER_ADDR1-6: 调用链上层函数的返回地址
 *
 * 用途: 跟踪、日志记录、锁调试等需要调用栈信息的场景
 */
#define CALLER_ADDR0 ((unsigned long)ftrace_return_address0)
#define CALLER_ADDR1 ((unsigned long)ftrace_return_address(1))
#define CALLER_ADDR2 ((unsigned long)ftrace_return_address(2))
#define CALLER_ADDR3 ((unsigned long)ftrace_return_address(3))
#define CALLER_ADDR4 ((unsigned long)ftrace_return_address(4))
#define CALLER_ADDR5 ((unsigned long)ftrace_return_address(5))
#define CALLER_ADDR6 ((unsigned long)ftrace_return_address(6))

/*
 * get_lock_parent_ip - 获取实际获取锁的代码位置
 *
 * 返回值: 第一个不在锁函数内部的返回地址
 *
 * 设计原因:
 * - 锁操作通常有多层包装（spin_lock -> _raw_spin_lock -> ...）
 * - 直接返回地址会指向锁实现内部，而非调用者
 * - 此函数跳过锁函数层，找到真正的锁使用者位置
 *
 * 实现: 逐层检查CALLER_ADDR，返回第一个非锁函数地址
 */
static __always_inline unsigned long get_lock_parent_ip(void)
{
	unsigned long addr = CALLER_ADDR0;

	if (!in_lock_functions(addr))
		return addr;
	addr = CALLER_ADDR1;
	if (!in_lock_functions(addr))
		return addr;
	return CALLER_ADDR2;
}

#ifdef CONFIG_TRACE_PREEMPT_TOGGLE
  /*
   * trace_preempt_on - 抢占启用时的跟踪钩子
   * @a0: 第一个参数（通常是IP地址）
   * @a1: 第二个参数（通常是父IP地址）
   *
   * 记录抢占状态变化，用于分析抢占延迟
   */
  extern void trace_preempt_on(unsigned long a0, unsigned long a1);
  /*
   * trace_preempt_off - 抢占禁用时的跟踪钩子
   * @a0: 第一个参数
   * @a1: 第二个参数
   */
  extern void trace_preempt_off(unsigned long a0, unsigned long a1);
#else
/*
 * Use defines instead of static inlines because some arches will make code out
 * of the CALLER_ADDR, when we really want these to be a real nop.
 */
/*
 * 使用宏而非inline函数，因为某些架构会为CALLER_ADDR生成代码
 * 而我们真正希望这些是完全的nop（无开销）
 */
# define trace_preempt_on(a0, a1) do { } while (0)
# define trace_preempt_off(a0, a1) do { } while (0)
#endif

#ifdef CONFIG_DYNAMIC_FTRACE
/*
 * ftrace_init - 初始化动态ftrace子系统
 *
 * 扫描内核代码段中的mcount调用点，建立dyn_ftrace记录表
 * 在内核启动早期调用
 */
extern void ftrace_init(void);
#ifdef CC_USING_PATCHABLE_FUNCTION_ENTRY
/*
 * FTRACE_CALLSITE_SECTION - 存储ftrace调用点地址的ELF节名
 *
 * 两种机制:
 * - CC_USING_PATCHABLE_FUNCTION_ENTRY: 编译器在函数入口前插入可补丁位置
 * - 传统mcount: 编译器在函数入口插入mcount调用
 */
#define FTRACE_CALLSITE_SECTION	"__patchable_function_entries"
#else
#define FTRACE_CALLSITE_SECTION	"__mcount_loc"
#endif
#else
static inline void ftrace_init(void) { }
#endif

/*
 * Structure that defines an entry function trace.
 * It's already packed but the attribute "packed" is needed
 * to remove extra padding at the end.
 */
/*
 * struct ftrace_graph_ent - 函数图跟踪的入口事件
 *
 * 记录函数调用的开始，包含函数地址和调用深度
 * 已经自然紧凑，但仍需__packed属性去除末尾填充
 */
struct ftrace_graph_ent {
	unsigned long func; /* Current function */
	                    /* 当前函数的地址 */
	long depth; /* signed to check for less than zero */
	            /* 调用深度（有符号，以检测小于0的错误情况） */
} __packed;

/*
 * Structure that defines an entry function trace with retaddr.
 */
/*
 * struct fgraph_retaddr_ent - 带返回地址的函数图入口事件
 *
 * 扩展ftrace_graph_ent，额外记录返回地址
 */
struct fgraph_retaddr_ent {
	struct ftrace_graph_ent ent;    /* 基本入口事件 */
	unsigned long retaddr;  /* Return address */
	                        /* 函数的返回地址（用于返回地址修改场景） */
} __packed;

/*
 * Structure that defines a return function trace.
 * It's already packed but the attribute "packed" is needed
 * to remove extra padding at the end.
 */
/*
 * struct ftrace_graph_ret - 函数图跟踪的返回事件
 *
 * 记录函数返回时的信息，包含执行时间、深度、溢出计数
 */
struct ftrace_graph_ret {
	unsigned long func; /* Current function */
	                    /* 返回的函数地址 */
#ifdef CONFIG_FUNCTION_GRAPH_RETVAL
	unsigned long retval;  /* 函数返回值（若启用CONFIG_FUNCTION_GRAPH_RETVAL） */
#endif
	int depth;             /* 调用深度 */
	/* Number of functions that overran the depth limit for current task */
	/* 当前任务超过深度限制的函数数量（检测无限递归） */
	unsigned int overrun;
} __packed;

struct fgraph_ops;  /* 前向声明 */

/* Type of the callback handlers for tracing function graph*/
/*
 * trace_func_graph_ret_t - 函数图返回事件的回调类型
 * @第一个参数: 返回事件数据
 * @gops: 触发此回调的fgraph_ops
 * @fregs: ftrace寄存器上下文（可能为NULL）
 */
typedef void (*trace_func_graph_ret_t)(struct ftrace_graph_ret *,
				       struct fgraph_ops *,
				       struct ftrace_regs *); /* return */

/*
 * trace_func_graph_ent_t - 函数图入口事件的回调类型
 * @第一个参数: 入口事件数据
 * @gops: 触发此回调的fgraph_ops
 * @fregs: ftrace寄存器上下文
 *
 * 返回值: 0=不跟踪此函数，非0=跟踪
 */
typedef int (*trace_func_graph_ent_t)(struct ftrace_graph_ent *,
				      struct fgraph_ops *,
				      struct ftrace_regs *); /* entry */

/*
 * ftrace_graph_entry_stub - 默认的函数图入口桩函数
 * @trace: 入口事件
 * @gops: fgraph_ops
 * @fregs: ftrace寄存器
 *
 * 返回值: 总是返回0（不跟踪）
 *
 * 用途: 作为占位符，直到实际的跟踪器注册
 */
extern int ftrace_graph_entry_stub(struct ftrace_graph_ent *trace,
				   struct fgraph_ops *gops,
				   struct ftrace_regs *fregs);

/*
 * ftrace_pids_enabled - 检查ftrace_ops是否启用了PID过滤
 * @ops: ftrace操作描述符
 *
 * 返回值: true=仅跟踪特定PID，false=跟踪所有进程
 */
bool ftrace_pids_enabled(struct ftrace_ops *ops);

#ifdef CONFIG_FUNCTION_GRAPH_TRACER

/*
 * struct fgraph_ops - 函数图跟踪操作描述符
 *
 * 定义一个函数图跟踪器实例，包含入口/返回回调和过滤配置
 */
struct fgraph_ops {
	trace_func_graph_ent_t		entryfunc;  /* 函数入口回调 */
	trace_func_graph_ret_t		retfunc;    /* 函数返回回调 */
	struct ftrace_ops		ops; /* for the hash lists */
	                                     /* 内嵌ftrace_ops，用于哈希过滤 */
	void				*private;   /* 私有数据指针 */
	trace_func_graph_ent_t		saved_func; /* 保存的入口函数（内部使用） */
	int				idx;        /* 此ops在全局数组中的索引 */
};

/*
 * fgraph_reserve_data - 为当前函数调用预留私有数据空间
 * @idx: fgraph_ops的索引
 * @size_bytes: 需要的字节数
 *
 * 返回值: 指向预留空间的指针，失败返回NULL
 *
 * 用途: 在函数入口时分配空间，在返回时通过fgraph_retrieve_data取回
 * 典型场景: 保存函数入口时的状态（如时间戳），在返回时计算差值
 */
void *fgraph_reserve_data(int idx, int size_bytes);

/*
 * fgraph_retrieve_data - 获取当前函数的私有数据
 * @idx: fgraph_ops的索引
 * @size_bytes: 输出参数，实际数据大小
 *
 * 返回值: 指向数据的指针，无数据返回NULL
 */
void *fgraph_retrieve_data(int idx, int *size_bytes);

/*
 * fgraph_retrieve_parent_data - 获取父函数（调用者）的私有数据
 * @idx: fgraph_ops的索引
 * @size_bytes: 输出参数，实际数据大小
 * @depth: 向上追溯的深度（1=直接调用者，2=调用者的调用者，...）
 *
 * 返回值: 指向父函数数据的指针
 *
 * 用途: 访问调用链上层函数保存的数据
 */
void *fgraph_retrieve_parent_data(int idx, int *size_bytes, int depth);

/*
 * Stack of return addresses for functions
 * of a thread.
 * Used in struct thread_info
 */
/*
 * struct ftrace_ret_stack - 每线程的函数返回栈元素
 *
 * 函数图跟踪器为每个线程维护一个返回地址栈，记录调用链
 * 在thread_info结构中使用
 */
struct ftrace_ret_stack {
	unsigned long ret;   /* 原始返回地址（被修改前） */
	unsigned long func;  /* 被跟踪的函数地址 */
#ifdef HAVE_FUNCTION_GRAPH_FP_TEST
	unsigned long fp;    /* 帧指针（用于栈完整性检查） */
#endif
	unsigned long *retp; /* 指向栈上返回地址位置的指针（用于恢复） */
};

/*
 * Primary handler of a function return.
 * It relays on ftrace_return_to_handler.
 * Defined in entry_32/64.S
 */
/*
 * return_to_handler - 函数返回的主处理器（汇编实现）
 *
 * 设计原因:
 * - 函数图跟踪通过修改栈上的返回地址，使函数返回到此处理器
 * - 处理器记录返回事件，然后跳转到真正的返回地址
 * - 必须用汇编实现以正确恢复寄存器和栈状态
 *
 * 定义位置: arch/*/kernel/entry_32/64.S
 */
extern void return_to_handler(void);

/*
 * function_graph_enter_regs - 记录函数入口（带寄存器上下文）
 * @ret: 原始返回地址
 * @func: 被调用函数的地址
 * @frame_pointer: 帧指针
 * @retp: 指向栈上返回地址位置的指针
 * @fregs: ftrace寄存器上下文（可选）
 *
 * 返回值: 0=成功，负数=失败
 *
 * 用途: 在函数入口调用，将返回地址修改为return_to_handler
 */
extern int
function_graph_enter_regs(unsigned long ret, unsigned long func,
			  unsigned long frame_pointer, unsigned long *retp,
			  struct ftrace_regs *fregs);

/*
 * function_graph_enter - 记录函数入口（无寄存器上下文）
 * @ret: 原始返回地址
 * @func: 被调用函数的地址
 * @fp: 帧指针
 * @retp: 指向栈上返回地址位置的指针
 *
 * 返回值: 0=成功，负数=失败
 *
 * 简化版本: 内部调用function_graph_enter_regs(ret, func, fp, retp, NULL)
 */
static inline int function_graph_enter(unsigned long ret, unsigned long func,
				       unsigned long fp, unsigned long *retp)
{
	return function_graph_enter_regs(ret, func, fp, retp, NULL);
}

/*
 * ftrace_graph_get_ret_stack - 获取任务的返回栈中某个条目
 * @task: 目标任务
 * @skip: 跳过的栈帧数（0=最新）
 *
 * 返回值: 指向ftrace_ret_stack的指针，无效返回NULL
 */
struct ftrace_ret_stack *
ftrace_graph_get_ret_stack(struct task_struct *task, int skip);

/*
 * ftrace_graph_top_ret_addr - 获取任务返回栈顶的返回地址
 * @task: 目标任务
 *
 * 返回值: 栈顶的原始返回地址
 */
unsigned long ftrace_graph_top_ret_addr(struct task_struct *task);

/*
 * ftrace_graph_ret_addr - 解析返回地址（考虑函数图修改）
 * @task: 目标任务
 * @idx: 输入输出参数，返回栈索引（用于遍历）
 * @ret: 从栈上读取的返回地址
 * @retp: 返回地址在栈上的位置
 *
 * 返回值: 真实的返回地址（若ret被修改为return_to_handler，则返回原始地址）
 *
 * 用途: 栈回溯时调用，恢复被函数图修改的返回地址
 */
unsigned long ftrace_graph_ret_addr(struct task_struct *task, int *idx,
				    unsigned long ret, unsigned long *retp);

/*
 * fgraph_get_task_var - 获取fgraph_ops的任务变量
 * @gops: fgraph_ops
 *
 * 返回值: 指向当前任务的此ops私有变量的指针
 *
 * 用途: 存储per-task状态（如统计计数）
 */
unsigned long *fgraph_get_task_var(struct fgraph_ops *gops);

/*
 * Sometimes we don't want to trace a function with the function
 * graph tracer but we want them to keep traced by the usual function
 * tracer if the function graph tracer is not configured.
 */
/*
 * __notrace_funcgraph - 标记函数不被函数图跟踪器跟踪
 *
 * 用途: 某些函数希望被普通ftrace跟踪，但不希望被函数图跟踪
 * （例如：函数图基础设施自身的代码）
 */
#define __notrace_funcgraph		notrace

/*
 * FTRACE_RETFUNC_DEPTH - 函数图跟踪的最大深度
 *
 * 限制调用链深度，防止栈溢出和无限递归
 */
#define FTRACE_RETFUNC_DEPTH 50

/*
 * FTRACE_RETSTACK_ALLOC_SIZE - 每次分配返回栈的大小
 *
 * 返回栈按需增长，每次增长此数量的条目
 */
#define FTRACE_RETSTACK_ALLOC_SIZE 32

/*
 * register_ftrace_graph - 注册函数图跟踪器
 * @ops: fgraph_ops描述符
 *
 * 返回值: 0=成功，负数=失败
 */
extern int register_ftrace_graph(struct fgraph_ops *ops);

/*
 * unregister_ftrace_graph - 注销函数图跟踪器
 * @ops: 之前注册的fgraph_ops
 */
extern void unregister_ftrace_graph(struct fgraph_ops *ops);

/**
 * ftrace_graph_is_dead - returns true if ftrace_graph_stop() was called
 *
 * ftrace_graph_stop() is called when a severe error is detected in
 * the function graph tracing. This function is called by the critical
 * paths of function graph to keep those paths from doing any more harm.
 */
/*
 * ftrace_graph_is_dead - 判断函数图跟踪是否已停止
 *
 * 返回值: true=已停止（因严重错误），false=正常运行
 *
 * 设计原因:
 * - ftrace_graph_stop()在检测到严重错误时调用
 * - 关键路径通过此函数检查状态，避免造成更多破坏
 * - 使用static_key实现零开销检查（正常情况下）
 */
DECLARE_STATIC_KEY_FALSE(kill_ftrace_graph);

static inline bool ftrace_graph_is_dead(void)
{
	return static_branch_unlikely(&kill_ftrace_graph);
}

/*
 * ftrace_graph_stop - 永久停止函数图跟踪
 *
 * 警告: 调用后无法重新启用，仅用于严重错误情况
 */
extern void ftrace_graph_stop(void);

/* The current handlers in use */
/*
 * ftrace_graph_return - 当前使用的函数图返回处理器
 *
 * 全局变量，指向当前活跃的返回回调
 */
extern trace_func_graph_ret_t ftrace_graph_return;

/*
 * ftrace_graph_entry - 当前使用的函数图入口处理器
 *
 * 全局变量，指向当前活跃的入口回调
 */
extern trace_func_graph_ent_t ftrace_graph_entry;

/*
 * ftrace_graph_init_task - 为新任务初始化函数图跟踪状态
 * @t: 新创建的任务
 *
 * 分配返回栈等资源
 */
extern void ftrace_graph_init_task(struct task_struct *t);

/*
 * ftrace_graph_exit_task - 任务退出时清理函数图状态
 * @t: 退出的任务
 *
 * 释放返回栈等资源
 */
extern void ftrace_graph_exit_task(struct task_struct *t);

/*
 * ftrace_graph_init_idle_task - 为idle任务初始化函数图状态
 * @t: idle任务
 * @cpu: CPU编号
 *
 * idle任务的特殊初始化路径
 */
extern void ftrace_graph_init_idle_task(struct task_struct *t, int cpu);

/* Used by assembly, but to quiet sparse warnings */
/*
 * function_trace_op - 当前执行的ftrace_ops（汇编代码使用）
 *
 * 汇编蹦床通过此全局变量获取当前的ftrace_ops
 * 声明为extern以消除sparse警告
 */
extern struct ftrace_ops *function_trace_op;

/*
 * pause_graph_tracing - 暂停当前任务的函数图跟踪
 *
 * 递增暂停计数器，嵌套调用需要相应次数的unpause
 */
static inline void pause_graph_tracing(void)
{
	atomic_inc(&current->tracing_graph_pause);
}

/*
 * unpause_graph_tracing - 恢复当前任务的函数图跟踪
 *
 * 递减暂停计数器，当计数器归零时恢复跟踪
 */
static inline void unpause_graph_tracing(void)
{
	atomic_dec(&current->tracing_graph_pause);
}
#else /* !CONFIG_FUNCTION_GRAPH_TRACER */

/* CONFIG_FUNCTION_GRAPH_TRACER未启用时的空桩实现 */

#define __notrace_funcgraph

static inline void ftrace_graph_init_task(struct task_struct *t) { }
static inline void ftrace_graph_exit_task(struct task_struct *t) { }
static inline void ftrace_graph_init_idle_task(struct task_struct *t, int cpu) { }

/* Define as macros as fgraph_ops may not be defined */
/* 使用宏定义，因为fgraph_ops类型可能未定义 */
#define register_ftrace_graph(ops) ({ -1; })
#define unregister_ftrace_graph(ops) do { } while (0)

static inline unsigned long
ftrace_graph_ret_addr(struct task_struct *task, int *idx, unsigned long ret,
		      unsigned long *retp)
{
	return ret;  /* 无函数图跟踪，直接返回原始地址 */
}

static inline void pause_graph_tracing(void) { }
static inline void unpause_graph_tracing(void) { }
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */

#ifdef CONFIG_TRACING
enum ftrace_dump_mode;  /* 前向声明，转储模式枚举 */

/*
 * ftrace_dump_on_oops_enabled - 检查oops时是否启用ftrace转储
 *
 * 返回值: 非0=启用，0=禁用
 *
 * 用途: 系统崩溃时自动转储ftrace缓冲区用于调试
 */
extern int ftrace_dump_on_oops_enabled(void);

/*
 * disable_trace_on_warning - 遇到警告时禁用跟踪
 *
 * 防止警告后的大量跟踪数据淹没有用信息
 * 通常由WARN_ON等宏触发
 */
extern void disable_trace_on_warning(void);

#else /* CONFIG_TRACING */
static inline void  disable_trace_on_warning(void) { }
#endif /* CONFIG_TRACING */

#ifdef CONFIG_FTRACE_SYSCALLS

/*
 * arch_syscall_addr - 获取系统调用的内核地址
 * @nr: 系统调用号
 *
 * 返回值: 系统调用处理函数的地址
 *
 * 用途: 系统调用跟踪，将系统调用号映射到实际内核函数
 * 架构特定实现，因为系统调用表布局因架构而异
 */
unsigned long arch_syscall_addr(int nr);

#endif /* CONFIG_FTRACE_SYSCALLS */

#endif /* _LINUX_FTRACE_H */
