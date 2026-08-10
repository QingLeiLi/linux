/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_COMPILER_H
#define __LINUX_COMPILER_H

#include <linux/compiler_types.h>

/*
 * 本文件位于“通用内核代码 → 编译器能力/静态分析工具”的适配层：
 * compiler_types.h 先给出类型属性、READ_ONCE() 所需的底层标注等定义，
 * 本文件再提供分支预测、优化屏障、编译期类型检查和符号保留等操作。
 * 大多数接口是宏，因为它们必须观察调用点的表达式、类型、文件名和行号；
 * 它们只约束编译器或构建工具，除非注释明确指出，否则不等同于 CPU 内存屏障。
 *
 * __ASSEMBLY__ 分支让汇编源只看到汇编可用的少量定义；__KERNEL__ 分支
 * 隔离仅内核本体需要的设施。体系结构可在包含本文件前预定义同名宏，下面的
 * #ifndef 便会保留体系结构实现，而不是用通用兜底覆盖它。
 */

#ifndef __ASSEMBLY__

#ifdef __KERNEL__

/*
 * Note: DISABLE_BRANCH_PROFILING can be used by special lowlevel code
 * to disable branch tracing on a per file basis.
 */
/*
 * DISABLE_BRANCH_PROFILING 可由特殊底层代码按源文件关闭分支跟踪。这样的代码
 * 往往运行得太早、处于递归敏感路径，或其指令布局不能承受插桩；该开关只影响
 * 当前翻译单元，不会全局关闭 ftrace 分支统计。
 */
/*
 * ftrace_likely_update() - 把一次 likely()/unlikely() 的实际结果记入调用点统计
 *
 * 调用位置：启用 CONFIG_TRACE_BRANCH_PROFILING 时，由 __branch_check__() 在
 * 求值原表达式后调用，最终由 kernel/trace/trace_branch.c 更新命中/未命中计数。
 * @f: 当前宏展开点的静态描述符，借用指针；对象存活到内核映像卸载。
 * @val: 条件本次归一化后的 0/1 结果。
 * @expect: 编写者预测的 0/1 结果。
 * @is_constant: 条件是否为编译期常量，用于排除没有分析价值的固定分支。
 * 上下文/并发：可从被插桩分支所在的普通内核上下文调用，不取得对象所有权且
 * 不睡眠。当前实现的统计递增并非原子操作（实现中保留 FIXME），因此数值用于
 * 性能诊断而不是精确同步计数。返回：无直接返回值；副作用仅是跟踪统计。
 */
void ftrace_likely_update(struct ftrace_likely_data *f, int val,
			  int expect, int is_constant);
#if defined(CONFIG_TRACE_BRANCH_PROFILING) \
    && !defined(DISABLE_BRANCH_PROFILING) && !defined(__CHECKER__)
#define likely_notrace(x)	__builtin_expect(!!(x), 1)
#define unlikely_notrace(x)	__builtin_expect(!!(x), 0)

/*
 * likely_notrace()/unlikely_notrace() 仍把 @x 归一化为布尔值并向编译器提供
 * 热/冷分支提示，但刻意绕过 ftrace 计数；@x 只求值一次，宏返回其 0/1 结果。
 * 它们适合跟踪实现自身等不能递归插桩的路径，不提供同步、原子性或正确性保证。
 */

#define __branch_check__(x, expect, is_constant) ({			\
			long ______r;					\
			static struct ftrace_likely_data		\
				__aligned(4)				\
				__section("_ftrace_annotated_branch")	\
				______f = {				\
				.data.func = __func__,			\
				.data.file = __FILE__,			\
				.data.line = __LINE__,			\
			};						\
			______r = __builtin_expect(!!(x), expect);	\
			ftrace_likely_update(&______f, ______r,		\
					     expect, is_constant);	\
			______r;					\
		})

/*
 * __branch_check__() 是带统计的表达式包装器。每个展开点创建一个放入
 * _ftrace_annotated_branch section 的静态 ______f，记录函数、文件和行号；
 * ______r 保存 @x 的唯一一次求值结果，先供编译器预测布局，再交给 ftrace
 * 统计，最后仍作为整个语句表达式的值返回。静态描述符没有调用者 ownership，
 * 专用 section 使跟踪框架能够批量发现它们；宏本身不改变原条件的真假语义。
 */

/*
 * Using __builtin_constant_p(x) to ignore cases where the return
 * value is always the same.  This idea is taken from a similar patch
 * written by Daniel Walker.
 */
/*
 * 这里用 __builtin_constant_p(x) 忽略返回值永远相同的情形；思路来自
 * Daniel Walker 的类似补丁。固定条件的“预测命中率”没有调优意义，标记后
 * ftrace 可以把它与运行时分支区别开。
 */
# ifndef likely
#  define likely(x)	(__branch_check__(x, 1, __builtin_constant_p(x)))
# endif
# ifndef unlikely
#  define unlikely(x)	(__branch_check__(x, 0, __builtin_constant_p(x)))
# endif

/*
 * likely()/unlikely() 分别声明“通常为真/通常为假”，返回 @x 的布尔结果并在
 * 本配置下记录预测质量。外层 #ifndef 允许体系结构或工具链先提供实现；调用者
 * 只能把它们当性能提示，绝不能让程序正确性依赖提示是否准确。
 */

#ifdef CONFIG_PROFILE_ALL_BRANCHES
/*
 * "Define 'is'", Bill Clinton
 * "Define 'if'", Steven Rostedt
 */
/*
 * 两句引文用双关提醒读者：CONFIG_PROFILE_ALL_BRANCHES 会重新定义 C 的 if，
 * 从而统计几乎所有非常量条件，而不仅是显式 likely()/unlikely()。这是一种
 * 调试构建插桩，额外的 section 数据和计数开销不属于正常快速路径。
 */
#define if(cond, ...) if ( __trace_if_var( !!(cond , ## __VA_ARGS__) ) )

#define __trace_if_var(cond) (__builtin_constant_p(cond) ? (cond) : __trace_if_value(cond))

#define __trace_if_value(cond) ({			\
	static struct ftrace_branch_data		\
		__aligned(4)				\
		__section("_ftrace_branch")		\
		__if_trace = {				\
			.func = __func__,		\
			.file = __FILE__,		\
			.line = __LINE__,		\
		};					\
	(cond) ?					\
		(__if_trace.miss_hit[1]++,1) :		\
		(__if_trace.miss_hit[0]++,0);		\
})

/*
 * if(cond, ...) 先借助逗号表达式兼容带可选参数的宏调用，再把条件交给
 * __trace_if_var()。编译期常量直接返回；运行时条件由 __trace_if_value()
 * 创建调用点描述符，并按假/真分别递增 miss_hit[0]/[1]。@cond 只求值一次，
 * 但计数是诊断统计而非同步原语，不能据此建立跨 CPU 的 happens-before。
 */

#endif /* CONFIG_PROFILE_ALL_BRANCHES */

#else
# define likely(x)	__builtin_expect(!!(x), 1)
# define unlikely(x)	__builtin_expect(!!(x), 0)
# define likely_notrace(x)	likely(x)
# define unlikely_notrace(x)	unlikely(x)
#endif

/*
 * 未启用分支分析时，四个接口都退化为 __builtin_expect()：不生成 ftrace
 * 描述符，也无运行时统计副作用。此配置分支保持调用点返回值契约相同。
 */

/* Optimization barrier */
/* 优化屏障：只约束编译器在该点前后安排内存访问，不自行发出 CPU 栅栏指令。 */
#ifndef barrier
/* The "volatile" is due to gcc bugs */
/* volatile 是为规避 GCC 的历史缺陷，防止空 asm 被错误删除。 */
# define barrier() __asm__ __volatile__("": : :"memory")
#endif

/*
 * barrier() - 建立编译器级内存次序边界
 *
 * 入参/返回：无；不取得锁、不睡眠，也不访问运行时对象。"memory" clobber
 * 迫使编译器假定任意内存可能改变，因而不能让可见内存访问跨越此点；空 asm
 * 通常不产生机器指令，所以它不能阻止 CPU 自身乱序。跨 CPU 协议应使用
 * smp_*()，设备协议应使用 mb()/dma_*() 等与相应观察者匹配的接口。
 */

#ifndef barrier_data
/*
 * This version is i.e. to prevent dead stores elimination on @ptr
 * where gcc and llvm may behave differently when otherwise using
 * normal barrier(): while gcc behavior gets along with a normal
 * barrier(), llvm needs an explicit input variable to be assumed
 * clobbered. The issue is as follows: while the inline asm might
 * access any memory it wants, the compiler could have fit all of
 * @ptr into memory registers instead, and since @ptr never escaped
 * from that, it proved that the inline asm wasn't touching any of
 * it. This version works well with both compilers, i.e. we're telling
 * the compiler that the inline asm absolutely may see the contents
 * of @ptr. See also: https://llvm.org/bugs/show_bug.cgi?id=15495
 */
/*
 * 该版本专门阻止 @ptr 所指内容被当作死存储删除；仅用普通 barrier() 时，
 * GCC 与 LLVM 的分析可能不同。编译器可能把 @ptr 的全部状态保留在寄存器中，
 * 并因指针从未逃逸而证明空 asm 不会接触它；把 @ptr 声明为显式输入后，便是
 * 明确告诉编译器 asm 可能观察该对象内容。它兼容两种编译器，相关背景见原文
 * LLVM bug 15495。@ptr 只被借用且只求值一次；此宏仍不是硬件内存屏障。
 */
# define barrier_data(ptr) __asm__ __volatile__("": :"r"(ptr) :"memory")
#endif

/* workaround for GCC PR82365 if needed */
/* GCC PR82365 的可覆盖规避点；默认空操作，受影响体系结构/编译器可提前重定义。 */
#ifndef barrier_before_unreachable
# define barrier_before_unreachable() do { } while (0)
#endif

/* Unreachable code */
/* 不可达代码设施：同时服务编译器控制流分析和 objtool 的离线指令验证。 */
#ifdef CONFIG_OBJTOOL
/* Annotate a C jump table to allow objtool to follow the code flow */
/* 给 C 跳转表放置专用 section 标注，使 objtool 能沿间接跳转继续恢复控制流。 */
#define __annotate_jump_table __section(".data.rel.ro.c_jump_table")
#else /* !CONFIG_OBJTOOL */
#define __annotate_jump_table
#endif /* CONFIG_OBJTOOL */

/*
 * __annotate_jump_table 仅在启用 objtool 时改变数据的 section 归属；关闭时为空，
 * 不改变 C 语义。调用点无入参、返回或运行时副作用，链接阶段保留的信息供
 * objtool 判断合法目标，避免把间接跳转后的可达指令误报为不可达。
 */

/*
 * Mark a position in code as unreachable.  This can be used to
 * suppress control flow warnings after asm blocks that transfer
 * control elsewhere.
 */
/*
 * 把当前位置标为不可达，可抑制“内联汇编已经把控制权转移到别处”之后的错误
 * 控制流警告。调用者必须以真实控制流保证这里永远不会执行；否则编译器可基于
 * __builtin_unreachable() 删除检查或重排代码，实际到达将产生未定义行为。
 * barrier_before_unreachable() 先给受影响编译器/体系结构一个修复插入点。
 */
#define unreachable() do {		\
	barrier_before_unreachable();	\
	__builtin_unreachable();	\
} while (0)

/*
 * KENTRY - kernel entry point
 * This can be used to annotate symbols (functions or data) that are used
 * without their linker symbol being referenced explicitly. For example,
 * interrupt vector handlers, or functions in the kernel image that are found
 * programatically.
 *
 * Not required for symbols exported with EXPORT_SYMBOL, or initcalls. Those
 * are handled in their own way (with KEEP() in linker scripts).
 *
 * KENTRY can be avoided if the symbols in question are marked as KEEP() in the
 * linker script. For example an architecture could KEEP() its entire
 * boot/exception vector code rather than annotate each function and data.
 */
/*
 * KENTRY 用来标记“不通过显式链接器符号引用仍会被使用”的内核入口，例如中断
 * 向量处理函数，或运行时按地址/表项发现的内核映像函数。它为 @sym 建立一个
 * __used 的地址记录并放进 ___kentry+sym section，使链接垃圾回收不能把目标
 * 当作无引用实体删除。EXPORT_SYMBOL 和 initcall 已由链接脚本 KEEP() 保留，
 * 无需重复使用；体系结构也可直接 KEEP 整段启动/异常向量代码来替代逐项标注。
 * @sym 必须是已声明的函数或数据符号；宏不在运行时调用它，也不转移 ownership。
 */
#ifndef KENTRY
# define KENTRY(sym)						\
	extern typeof(sym) sym;					\
	static const unsigned long __kentry_##sym		\
	__used							\
	__attribute__((__section__("___kentry+" #sym)))		\
	= (unsigned long)&sym;
#endif

#ifndef RELOC_HIDE
# define RELOC_HIDE(ptr, off) ((typeof(ptr))((unsigned long)(ptr) + (off)))
#endif

#define absolute_pointer(val)	RELOC_HIDE((void *)(val), 0)

/*
 * RELOC_HIDE(@ptr, @off) 先把指针转成整数完成字节偏移，再恢复 @ptr 的原类型，
 * 用于阻止编译器把结果继续绑定到原 C 对象的地址推理；参数各求值一次，返回
 * 计算后的借用指针，不创建引用也不保证越界结果可解引用。absolute_pointer()
 * 用零偏移包装整数地址，表达“这是绝对地址，不要按普通对象关系折叠”。
 */

#ifndef OPTIMIZER_HIDE_VAR
/* Make the optimizer believe the variable can be manipulated arbitrarily. */
/* 让优化器相信该变量可能被任意改变，从而忘掉此前推导出的具体值或取值范围。 */
#define OPTIMIZER_HIDE_VAR(var)						\
	__asm__ ("" : "=r" (var) : "0" (var))
#endif

/*
 * OPTIMIZER_HIDE_VAR(@var) 以同一寄存器作为 asm 输入和输出：机器层通常没有
 * 指令，但编译器必须把输出视为未知。@var 是输入输出左值，值在 C 语义上保持
 * 不变；宏不形成 CPU 或编译器内存屏障，只切断针对该变量的值传播。
 */

/* Format: __UNIQUE_ID_<name>_<__COUNTER__> */
/* 生成格式 __UNIQUE_ID_<name>_<__COUNTER__> 的翻译单元内唯一标识符。 */
#define __UNIQUE_ID(name)					\
	__PASTE(__UNIQUE_ID_,					\
	__PASTE(name,						\
	__PASTE(_, __COUNTER__)))

/*
 * __UNIQUE_ID(@name) 经过多层 __PASTE 强制展开单调递增的 __COUNTER__，适合
 * 让宏内部静态变量互不重名。它生成预处理 token 而非字符串或运行时值；唯一性
 * 仅依赖当前编译器翻译单元，不能当作跨模块的稳定 ABI 标识。
 */

/**
 * data_race - mark an expression as containing intentional data races
 *
 * This data_race() macro is useful for situations in which data races
 * should be forgiven.  One example is diagnostic code that accesses
 * shared variables but is not a part of the core synchronization design.
 * For example, if accesses to a given variable are protected by a lock,
 * except for diagnostic code, then the accesses under the lock should
 * be plain C-language accesses and those in the diagnostic code should
 * use data_race().  This way, KCSAN will complain if buggy lockless
 * accesses to that variable are introduced, even if the buggy accesses
 * are protected by READ_ONCE() or WRITE_ONCE().
 *
 * This macro *does not* affect normal code generation, but is a hint
 * to tooling that data races here are to be ignored.  If the access must
 * be atomic *and* KCSAN should ignore the access, use both data_race()
 * and READ_ONCE(), for example, data_race(READ_ONCE(x)).
 */
/*
 * data_race() 标记表达式中的数据竞争是有意的，适用于不属于核心同步设计的
 * 诊断性无锁读取。例如共享变量通常受锁保护，诊断路径例外：锁内访问仍应使用
 * 普通 C 访问，只有诊断访问包在 data_race() 中。这样，新引入的错误无锁访问
 * 即使用了 READ_ONCE()/WRITE_ONCE()，KCSAN 仍可报告。
 *
 * 该宏不改变正常代码生成，只暂时关闭当前上下文的 KCSAN 与 context analysis，
 * 求值 @expr 一次并用 auto 保存其原类型结果，随后按相反顺序恢复分析状态。
 * 它不保证访问原子性或内存顺序；若既需单次原子访问又需 KCSAN 忽略，必须组合
 * data_race(READ_ONCE(x))。入参可有原表达式副作用；返回该表达式的值，不转移
 * 所涉及对象的 ownership，也不可借此掩盖同步协议本身的竞态。
 */
#define data_race(expr)							\
({									\
	__kcsan_disable_current();					\
	disable_context_analysis();					\
	auto __v = (expr);						\
	enable_context_analysis();					\
	__kcsan_enable_current();					\
	__v;								\
})

#ifdef __CHECKER__
#define __BUILD_BUG_ON_ZERO_MSG(e, msg, ...) (0)
#else /* __CHECKER__ */
#define __BUILD_BUG_ON_ZERO_MSG(e, msg, ...) ((int)sizeof(struct {_Static_assert(!(e), msg);}))
#endif /* __CHECKER__ */

/*
 * __BUILD_BUG_ON_ZERO_MSG(@e, @msg, ...) 在普通编译中把 _Static_assert 放进
 * sizeof 的匿名结构体：@e 为真时编译失败，为假时整个宏是值 0，可嵌入数组
 * 大小或加法表达式。Sparse (__CHECKER__) 分支直接返回 0，避免工具不兼容；
 * @e 只参与编译期判定，不应含运行时副作用，@msg 是失败诊断文本。
 */

/* &a[0] degrades to a pointer: a different type from an array */
/* &a[0] 会退化为指针，其类型与数组 a 本身不同；该差异可用于拒绝误传的指针。 */
#define __is_array(a)		(!__same_type((a), &(a)[0]))
#define __must_be_array(a)	__BUILD_BUG_ON_ZERO_MSG(!__is_array(a), \
							"must be array")

#define __is_byte_array(a)	(__is_array(a) && sizeof((a)[0]) == 1)
#define __must_be_byte_array(a)	__BUILD_BUG_ON_ZERO_MSG(!__is_byte_array(a), \
							"must be byte array")

/*
 * __is_array(@a) 比较表达式与首元素地址的类型，返回编译期布尔值；
 * __must_be_array(@a) 在不是数组时制造编译错误，否则产生可组合的 0。
 * byte 版本还要求元素大小为 1，用于接受 char/u8 等字节数组而拒绝宽元素数组。
 * 这些宏不求值数组内容、不取得其地址的长期所有权；变长数组等调用点仍必须
 * 遵守所处上下文的编译器限制。
 */

/*
 * If the "nonstring" attribute isn't available, we have to return true
 * so the __must_*() checks pass when "nonstring" isn't supported.
 */
/*
 * 若工具链没有 nonstring 属性，就只能保守返回 true，让两类 __must_*() 检查
 * 都通过；这是能力缺失时避免误报的降级，并不证明对象真的满足字符串契约。
 */
#if __has_attribute(__nonstring__) && defined(__annotated)
#define __is_cstr(a)		(!__annotated(a, nonstring))
#define __is_noncstr(a)		(__annotated(a, nonstring))
#else
#define __is_cstr(a)		(true)
#define __is_noncstr(a)		(true)
#endif

/*
 * __is_cstr(@a)/__is_noncstr(@a) 查询编译器附着在对象上的 nonstring 标注：
 * 前者要求可按 NUL 结尾字符串处理，后者要求不可作此假设。它们只检查静态属性，
 * 不扫描内存寻找 NUL，也不证明缓冲区当前内容有效；无属性支持时两者均为 true。
 */

/* Require C Strings (i.e. NUL-terminated) lack the "nonstring" attribute. */
/* C 字符串（即以 NUL 结尾的对象）必须没有 nonstring 属性。 */
#define __must_be_cstr(p) \
	__BUILD_BUG_ON_ZERO_MSG(!__is_cstr(p), \
				"must be C-string (NUL-terminated)")
#define __must_be_noncstr(p) \
	__BUILD_BUG_ON_ZERO_MSG(!__is_noncstr(p), \
				"must be non-C-string (not NUL-terminated)")

/*
 * __must_be_cstr(@p)/__must_be_noncstr(@p) 把上述属性判断转成编译期断言，
 * 成功时值为 0，便于嵌入其他宏。它们验证的是调用接口的静态标注契约，不读取
 * @p、不改变指针 ownership，也不能替代实际缓冲区长度和终止符检查。
 */

/*
 * Define TYPEOF_UNQUAL() to use __typeof_unqual__() as typeof
 * operator when available, to return an unqualified type of the exp.
 */
/*
 * TYPEOF_UNQUAL() 在工具链支持时使用 __typeof_unqual__()，取得表达式去除
 * const/volatile/_Atomic 等限定后的类型；否则退回 __typeof__()，会保留限定。
 * @exp 通常只用于类型推导而不求值，调用者若依赖“去限定”必须受
 * USE_TYPEOF_UNQUAL 配置约束，不能假定所有编译器行为一致。
 */
#if defined(USE_TYPEOF_UNQUAL)
# define TYPEOF_UNQUAL(exp) __typeof_unqual__(exp)
#else
# define TYPEOF_UNQUAL(exp) __typeof__(exp)
#endif

#endif /* __KERNEL__ */

#if defined(CONFIG_CFI) && !defined(__DISABLE_EXPORTS) && !defined(BUILD_VDSO)
/*
 * Force a reference to the external symbol so the compiler generates
 * __kcfi_typid.
 */
/*
 * 强制产生对外部符号的引用，使编译器为它生成 __kcfi_typid。该类型 ID 供
 * KCFI 在间接调用处校验目标函数签名；关闭 CFI、禁用导出或构建 VDSO 时宏为空，
 * 因为这些环境不需要或不能生成该元数据。
 */
#define KCFI_REFERENCE(sym) __ADDRESSABLE(sym)
#else
#define KCFI_REFERENCE(sym)
#endif

/*
 * KCFI_REFERENCE(@sym) 只在构建/链接层强制保留地址引用，不调用 @sym，也不
 * 改变其可见性或 ownership。启用分支借助稍后定义的 __ADDRESSABLE()；预处理
 * 允许宏在定义处尚未展开被引用宏，实际调用点必须位于后者可见之后。
 */

/**
 * offset_to_ptr - convert a relative memory offset to an absolute pointer
 * @off:	the address of the 32-bit offset value
 */
/*
 * offset_to_ptr() - 把相对内存偏移转换成绝对指针
 * @off: 指向 32 位有符号偏移字段的借用指针；不可为 NULL，字段必须可安全读取。
 *
 * 调用位置：kallsyms、模块压缩符号表、tracepoint/init 表等用“字段自身地址 +
 * 字段值”编码目标，以 32 位偏移节省只读表空间。本函数不加锁、不睡眠，只读取
 * *off；表的发布者必须保证其内容和生命周期在调用期间稳定。返回计算出的借用
 * 指针，可指向字段之前或之后；不验证范围、不获取引用，调用者按所属表解释类型。
 */
static inline void *offset_to_ptr(const int *off)
{
	/* 偏移的基址正是偏移字段地址；转成整数后按字节相加，再恢复通用指针。 */
	return (void *)((unsigned long)off + *off);
}

#endif /* __ASSEMBLY__ */

/*
 * Force the compiler to emit 'sym' as a symbol, so that we can reference
 * it from inline assembler. Necessary in case 'sym' could be inlined
 * otherwise, or eliminated entirely due to lack of references that are
 * visible to the compiler.
 */
/*
 * 强制编译器把 @sym 作为真实符号发出，以便内联汇编引用。若编译器看不到普通
 * C 引用，它原本可能内联或完全删除该符号；地址记录建立了一条可见引用。
 */
#define ___ADDRESSABLE(sym, __attrs)						\
	static void * __used __attrs						\
	__UNIQUE_ID(__PASTE(addressable_, sym)) = (void *)(uintptr_t)&sym;

#define __ADDRESSABLE(sym) \
	___ADDRESSABLE(sym, __section(".discard.addressable"))

/*
 * ___ADDRESSABLE(@sym, @__attrs) 创建一个名称唯一、标为 __used 的静态 void *，
 * 其值为 @sym 地址；属性参数决定记录所在 section。__ADDRESSABLE() 选择
 * .discard.addressable，使记录完成“保活”任务后可由链接脚本丢弃。两者不在
 * 运行时取得引用；@sym 必须能取地址，重复调用依靠 __UNIQUE_ID 避免重名。
 */

/*
 * This returns a constant expression while determining if an argument is
 * a constant expression, most importantly without evaluating the argument.
 * Glory to Martin Uecker <Martin.Uecker@med.uni-goettingen.de>
 *
 * Details:
 * - sizeof() return an integer constant expression, and does not evaluate
 *   the value of its operand; it only examines the type of its operand.
 * - The results of comparing two integer constant expressions is also
 *   an integer constant expression.
 * - The first literal "8" isn't important. It could be any literal value.
 * - The second literal "8" is to avoid warnings about unaligned pointers;
 *   this could otherwise just be "1".
 * - (long)(x) is used to avoid warnings about 64-bit types on 32-bit
 *   architectures.
 * - The C Standard defines "null pointer constant", "(void *)0", as
 *   distinct from other void pointers.
 * - If (x) is an integer constant expression, then the "* 0l" resolves
 *   it into an integer constant expression of value 0. Since it is cast to
 *   "void *", this makes the second operand a null pointer constant.
 * - If (x) is not an integer constant expression, then the second operand
 *   resolves to a void pointer (but not a null pointer constant: the value
 *   is not an integer constant 0).
 * - The conditional operator's third operand, "(int *)8", is an object
 *   pointer (to type "int").
 * - The behavior (including the return type) of the conditional operator
 *   ("operand1 ? operand2 : operand3") depends on the kind of expressions
 *   given for the second and third operands. This is the central mechanism
 *   of the macro:
 *   - When one operand is a null pointer constant (i.e. when x is an integer
 *     constant expression) and the other is an object pointer (i.e. our
 *     third operand), the conditional operator returns the type of the
 *     object pointer operand (i.e. "int *"). Here, within the sizeof(), we
 *     would then get:
 *       sizeof(*((int *)(...))  == sizeof(int)  == 4
 *   - When one operand is a void pointer (i.e. when x is not an integer
 *     constant expression) and the other is an object pointer (i.e. our
 *     third operand), the conditional operator returns a "void *" type.
 *     Here, within the sizeof(), we would then get:
 *       sizeof(*((void *)(...)) == sizeof(void) == 1
 * - The equality comparison to "sizeof(int)" therefore depends on (x):
 *     sizeof(int) == sizeof(int)     (x) was a constant expression
 *     sizeof(int) != sizeof(void)    (x) was not a constant expression
 */
/*
 * __is_constexpr(@x) 在不求值 @x 的前提下返回整型常量表达式，判断 @x 本身
 * 是否为整型常量表达式。关键机制是 sizeof 不求值操作数，以及条件运算符的
 * 类型规则：@x 为常量时，(long)x * 0 形成空指针常量，与 (int *)8 合并后结果
 * 类型为 int *，解引用类型大小等于 sizeof(int)；非常量时第二操作数只是普通
 * void *，合并结果为 void *，GNU C 的 sizeof(void) 为 1，比较为假。
 * 两个字面量 8 本身不重要，第二个取 8 是避免非对齐指针警告；转 long 避免
 * 32 位架构处理 64 位类型时告警。该宏依赖 GNU C 扩展，只做编译期分类，@x
 * 即使含副作用也不会在这里执行。这一精巧写法归功于 Martin Uecker。
 */
#define __is_constexpr(x) \
	(sizeof(int) == sizeof(*(8 ? ((void *)((long)(x) * 0l)) : (int *)8)))

/*
 * Whether 'type' is a signed type or an unsigned type. Supports scalar types,
 * bool and also pointer types.
 */
/* 判断 @type 是有符号还是无符号；支持标量、bool 和指针类型。 */
#define is_signed_type(type) (((type)(-1)) < (__force type)1)
#define is_unsigned_type(type) (!is_signed_type(type))

/*
 * is_signed_type(@type) 把 -1 和 1 转成目标类型后比较：有符号类型保留 -1，
 * 无符号/布尔/指针语义下不会小于 1；__force 允许 Sparse 完成受限类型转换。
 * is_unsigned_type() 取反得到互补分类。两者产生编译期表达式，不创建对象。
 */

/*
 * Useful shorthand for "is this condition known at compile-time?"
 *
 * Note that the condition may involve non-constant values,
 * but the compiler may know enough about the details of the
 * values to determine that the condition is statically true.
 */
/*
 * statically_true(@x) 是“编译器此刻能否证明条件恒真”的简写。@x 可以包含
 * 非常量值，只要优化器能从上下文折叠出真；宏返回布尔结果，不应用于要求严格
 * 整型常量表达式的场合。短路保证无法证明为常量时不会为求结果再次执行 @x。
 */
#define statically_true(x) (__builtin_constant_p(x) && (x))

/*
 * Similar to statically_true() but produces a constant expression
 *
 * To be used in conjunction with macros, such as BUILD_BUG_ON_ZERO(),
 * which require their input to be a constant expression and for which
 * statically_true() would otherwise fail.
 *
 * This is a trade-off: const_true() requires all its operands to be
 * compile time constants. Else, it would always returns false even on
 * the most trivial cases like:
 *
 *   true || non_const_var
 *
 * On the opposite, statically_true() is able to fold more complex
 * tautologies and will return true on expressions such as:
 *
 *   !(non_const_var * 8 % 4)
 *
 * For the general case, statically_true() is better.
 */
/*
 * const_true() 与 statically_true() 相似，但保证结果本身是常量表达式，适合
 * BUILD_BUG_ON_ZERO() 等宏。代价是所有操作数必须为编译期常量，否则即使
 * true || non_const_var 也返回 false；相反 statically_true() 能借优化器证明
 * !(non_const_var * 8 % 4) 这类恒真式。一般判断优先用后者，只有调用方语法
 * 强制要求常量表达式时才用 const_true()。
 */
#define const_true(x) __builtin_choose_expr(__is_constexpr(x), x, false)

/*
 * __builtin_choose_expr 在编译期选择：@x 是常量表达式时返回 @x，否则返回
 * false；未选择分支不在运行时求值。结果不提供同步或类型范围检查。
 */

/*
 * This is needed in functions which generate the stack canary, see
 * arch/x86/kernel/smpboot.c::start_secondary() for an example.
 */
/*
 * 生成栈 canary 的函数需要阻止尾调用优化，x86 的 start_secondary() 是示例。
 * 调用者把 mb() 放在生成 canary 的工作之后，使此前的最后一个调用不再处于
 * 可直接尾调用的位置，从而保留当前函数返回路径。宏无入参和返回值，具体硬件
 * 指令由体系结构 mb() 决定；它不是通用的“禁止所有函数尾调用”属性。
 */
#define prevent_tail_call_optimization()	mb()

#include <asm/rwonce.h>

#endif /* __LINUX_COMPILER_H */
