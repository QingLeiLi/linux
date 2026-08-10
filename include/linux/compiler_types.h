/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_COMPILER_TYPES_H
#define __LINUX_COMPILER_TYPES_H

/*
 * 编译器类型与属性的统一词汇表。它先把 GCC/Clang/Sparse/BTF/KASAN/KCSAN 的
 * 能力差异归一化，再让通用内核用 __user、notrace、noinstr、__counted_by 等
 * 稳定名字表达地址空间、插桩边界、对象大小和编译期契约。多数宏只影响编译、
 * 静态分析或链接，不在运行时取得对象引用；空实现表示当前工具不具备该检查，
 * 不能反向证明代码已经安全。
 */

/*
 * __has_builtin is supported on gcc >= 10, clang >= 3 and icc >= 21.
 * In the meantime, to support gcc < 10, we implement __has_builtin
 * by hand.
 */
/* GCC >=10、Clang >=3、ICC >=21 支持 __has_builtin；旧 GCC 兜底恒返回 0，使能力探测选择保守路径。 */
#ifndef __has_builtin
#define __has_builtin(x) (0)
#endif

/* Indirect macros required for expanded argument pasting, eg. __LINE__. */
/* 两级 token 粘贴先展开参数再拼接，例如让 __LINE__/__COUNTER__ 的值进入标识符。 */
#define ___PASTE(a, b) a##b
#define __PASTE(a, b) ___PASTE(a, b)

#ifndef __ASSEMBLY__

/*
 * C23 introduces "auto" as a standard way to define type-inferred
 * variables, but "auto" has been a (useless) keyword even since K&R C,
 * so it has always been "namespace reserved."
 *
 * Until at some future time we require C23 support, we need the gcc
 * extension __auto_type, but there is no reason to put that elsewhere
 * in the source code.
 */
/*
 * C23 才把 auto 标准化为类型推导；此前它虽是保留关键字却没有该用途。内核在
 * 要求 C23 前统一映射到 GCC __auto_type，调用点因此可以只求值初始化式一次并
 * 保留其推导类型，而无需在其他源码直接依赖扩展拼写。
 */
#if __STDC_VERSION__ < 202311L
# define auto __auto_type
#endif

/*
 * Skipped when running bindgen due to a libclang issue;
 * see https://github.com/rust-lang/rust-bindgen/issues/2244.
 */
/* bindgen 因 libclang issue 2244 跳过 BTF type tag；其他满足配置/能力的构建把 @value 字符串化为 btf_type_tag。 */
#if defined(CONFIG_DEBUG_INFO_BTF) && defined(CONFIG_PAHOLE_HAS_BTF_TAG) && \
	__has_attribute(btf_type_tag) && !defined(__BINDGEN__)
# define BTF_TYPE_TAG(value) __attribute__((btf_type_tag(#value)))
#else
# define BTF_TYPE_TAG(value) /* nothing */
#endif
/* 不满足条件时 BTF_TYPE_TAG 明确展开为空，不生成任何类型标签。 */

#include <linux/compiler-context-analysis.h>

/* sparse defines __CHECKER__; see Documentation/dev-tools/sparse.rst */
/* Sparse 以 __CHECKER__ 标识静态分析构建，详细使用见原文文档。 */
#ifdef __CHECKER__
/* address spaces */
/*
 * 地址空间标注让 Sparse 区分普通内核、用户、I/O、per-CPU 与 RCU 指针，noderef
 * 阻止未经 accessor 的直接解引用。__chk_* 接收借用指针只做类型验证；__force
 * 是显式越过检查的审计点，ACCESS_PRIVATE 则在确知封装条件时访问受限成员。
 */
# define __kernel	__attribute__((address_space(0)))
# define __user		__attribute__((noderef, address_space(__user)))
# define __iomem	__attribute__((noderef, address_space(__iomem)))
# define __percpu	__attribute__((noderef, address_space(__percpu)))
# define __rcu		__attribute__((noderef, address_space(__rcu)))
/* __chk_user_ptr() 只让 Sparse 验证借用用户指针地址空间；无运行时副作用或返回值。 */
static inline void __chk_user_ptr(const volatile void __user *ptr) { }
/* __chk_io_ptr() 只让 Sparse 验证借用 I/O 指针地址空间；无运行时副作用或返回值。 */
static inline void __chk_io_ptr(const volatile void __iomem *ptr) { }
/* other */
# define __force	__attribute__((force))
# define __nocast	__attribute__((nocast))
# define __safe		__attribute__((safe))
# define __private	__attribute__((noderef))
# define ACCESS_PRIVATE(p, member) (*((typeof((p)->member) __force *) &(p)->member))
#else /* __CHECKER__ */
/* address spaces */
/*
 * 普通编译器不执行 Sparse 地址空间检查：能保留的信息尽量转成 STRUCTLEAK/BTF
 * tag，其余为空；检查函数退化为空表达式。此分支保持代码可编译，不提供运行时
 * 地址验证，用户/I/O 指针仍必须通过 copy_*_user/readl 等正确接口访问。
 */
# define __kernel
# ifdef STRUCTLEAK_PLUGIN
#  define __user	__attribute__((user))
# else
#  define __user	BTF_TYPE_TAG(user)
# endif
# define __iomem
# define __percpu	__percpu_qual BTF_TYPE_TAG(percpu)
# define __rcu		BTF_TYPE_TAG(rcu)

# define __chk_user_ptr(x)	(void)0
# define __chk_io_ptr(x)	(void)0
/* other */
# define __force
# define __nocast
# define __safe
# define __private
# define ACCESS_PRIVATE(p, member) ((p)->member)
# define __builtin_warning(x, y...) (1)
#endif /* __CHECKER__ */

#ifdef __KERNEL__

/* Attributes */
/* 内核模式下引入基础 attribute 别名，再叠加与配置和体系结构有关的语义。 */
#include <linux/compiler_attributes.h>

#if CONFIG_FUNCTION_ALIGNMENT > 0
#define __function_aligned		__aligned(CONFIG_FUNCTION_ALIGNMENT)
#else
#define __function_aligned
#endif

/* __function_aligned 仅在配置要求时把函数入口对齐到 CONFIG_FUNCTION_ALIGNMENT。 */

/*
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-cold-function-attribute
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Label-Attributes.html#index-cold-label-attribute
 *
 * When -falign-functions=N is in use, we must avoid the cold attribute as
 * GCC drops the alignment for cold functions. Worse, GCC can implicitly mark
 * callees of cold functions as cold themselves, so it's not sufficient to add
 * __function_aligned here as that will not ensure that callees are correctly
 * aligned.
 *
 * See:
 *
 *   https://lore.kernel.org/lkml/Y77%2FqVgvaJidFpYt@FVFF77S0Q05N
 *   https://gcc.gnu.org/bugzilla/show_bug.cgi?id=88345#c9
 */
/*
 * GCC 的 cold 会在 -falign-functions=N 下丢失函数对齐，还可能把被调函数隐式
 * 传播为 cold；仅给当前函数补 __function_aligned 不能修复调用链。因此只有
 * 编译器对齐行为可靠或未要求对齐时才启用 __cold，否则宁可放弃冷路径布局。
 */
#if defined(CONFIG_CC_HAS_SANE_FUNCTION_ALIGNMENT) || (CONFIG_FUNCTION_ALIGNMENT == 0)
#define __cold				__attribute__((__cold__))
#else
#define __cold
#endif

/*
 * On x86-64 and arm64 targets, __preserve_most changes the calling convention
 * of a function to make the code in the caller as unintrusive as possible. This
 * convention behaves identically to the C calling convention on how arguments
 * and return values are passed, but uses a different set of caller- and callee-
 * saved registers.
 *
 * The purpose is to alleviates the burden of saving and recovering a large
 * register set before and after the call in the caller.  This is beneficial for
 * rarely taken slow paths, such as error-reporting functions that may be called
 * from hot paths.
 *
 * Note: This may conflict with instrumentation inserted on function entry which
 * does not use __preserve_most or equivalent convention (if in assembly). Since
 * function tracing assumes the normal C calling convention, where the attribute
 * is supported, __preserve_most implies notrace.  It is recommended to restrict
 * use of the attribute to functions that should or already disable tracing.
 *
 * Optional: not supported by gcc.
 *
 * clang: https://clang.llvm.org/docs/AttributeReference.html#preserve-most
 */
/*
 * __preserve_most 在 x86-64/arm64 Clang 上改变 caller/callee 保存寄存器集合，
 * 参数和返回 ABI 不变，可降低热路径调用罕见错误处理时的保存开销。入口插桩若
 * 仍用普通 C ABI 会冲突，所以该属性隐含 notrace；GCC 不支持时为空。调用者只
 * 应用于本就不需跟踪的罕见慢路径。
 */
#if __has_attribute(__preserve_most__) && (defined(CONFIG_X86_64) || defined(CONFIG_ARM64))
# define __preserve_most notrace __attribute__((__preserve_most__))
#else
# define __preserve_most
#endif

/*
 * Annotating a function/variable with __retain tells the compiler to place
 * the object in its own section and set the flag SHF_GNU_RETAIN. This flag
 * instructs the linker to retain the object during garbage-cleanup or LTO
 * phases.
 *
 * Note that the __used macro is also used to prevent functions or data
 * being optimized out, but operates at the compiler/IR-level and may still
 * allow unintended removal of objects during linking.
 *
 * Optional: only supported since gcc >= 11, clang >= 13
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Common-Function-Attributes.html#index-retain-function-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#retain
 */
/*
 * __retain 令对象独占 section 并带 SHF_GNU_RETAIN，使链接垃圾回收/LTO 也保留
 * 它；__used 只阻止编译器/IR 删除，仍可能在链接时消失。仅在工具链和相关
 * dead-code/LTO 配置需要时启用，否则为空。
 */
#if __has_attribute(__retain__) && \
	(defined(CONFIG_LD_DEAD_CODE_DATA_ELIMINATION) || \
	 defined(CONFIG_LTO_CLANG))
# define __retain			__attribute__((__retain__))
#else
# define __retain
#endif

/* Compiler specific macros. */
/* 先判断 Clang，因为它也定义 __GNUC__；未知编译器直接报错，防止静默缺失契约。 */
#ifdef __clang__
#include <linux/compiler-clang.h>
#elif defined(__GNUC__)
/* The above compilers also define __GNUC__, so order is important here. */
/* 上述编译器同样定义 __GNUC__，故顺序不可交换。 */
#include <linux/compiler-gcc.h>
#else
#error "Unknown compiler"
#endif

/*
 * Some architectures need to provide custom definitions of macros provided
 * by linux/compiler-*.h, and can do so using asm/compiler.h. We include that
 * conditionally rather than using an asm-generic wrapper in order to avoid
 * build failures if any C compilation, which will include this file via an
 * -include argument in c_flags, occurs prior to the asm-generic wrappers being
 * generated.
 */
/*
 * 某些体系结构需覆盖 compiler-*.h 的定义，CONFIG_HAVE_ARCH_COMPILER_H 时直接
 * 包含 asm/compiler.h。这里不用 asm-generic wrapper，是因为 c_flags 会强制
 * 预包含本文件，可能早于生成 wrapper；直接依赖会造成早期构建失败。
 */
#ifdef CONFIG_HAVE_ARCH_COMPILER_H
#include <asm/compiler.h>
#endif

/*
 * ftrace_branch_data 描述一个静态分支调用点：func/file/line 是编译期生成且随
 * 模块存在的借用字符串；union 让 annotated likely 统计的 correct/incorrect
 * 与 all-branches 统计的 miss/hit 共用布局，miss_hit[] 便于按真假索引更新。
 * 多 CPU 递增当前可能非原子，字段是诊断计数而非同步状态。
 */
struct ftrace_branch_data {
	const char *func;
	const char *file;
	unsigned line;
	union {
		struct {
			unsigned long correct;
			unsigned long incorrect;
		};
		struct {
			unsigned long miss;
			unsigned long hit;
		};
		unsigned long miss_hit[2];
	};
};

/* ftrace_likely_data 在基础调用点统计外增加 constant 次数，用来排除恒定条件的预测意义。 */
struct ftrace_likely_data {
	struct ftrace_branch_data	data;
	unsigned long			constant;
};

#if defined(CC_USING_HOTPATCH)
#define notrace			__attribute__((hotpatch(0, 0)))
#elif defined(CC_USING_PATCHABLE_FUNCTION_ENTRY)
#define notrace			__attribute__((patchable_function_entry(0, 0)))
#else
#define notrace			__attribute__((__no_instrument_function__))
#endif

/* notrace 按工具链选择 hotpatch、patchable entry 或 no_instrument，统一禁止函数入口跟踪。 */

/*
 * it doesn't make sense on ARM (currently the only user of __naked)
 * to trace naked functions because then mcount is called without
 * stack and frame pointer being set up and there is no chance to
 * restore the lr register to the value before mcount was called.
 */
/*
 * ARM naked 函数没有编译器建立的栈/帧指针，若插入 mcount，无法把 lr 恢复到
 * 调用前值；因此 __naked 必须同时 notrace。此属性要求函数体严格遵守架构汇编
 * 约束，普通 C 局部变量和返回序列不能按常规函数假定。
 */
#define __naked			__attribute__((__naked__)) notrace

/*
 * Prefer gnu_inline, so that extern inline functions do not emit an
 * externally visible function. This makes extern inline behave as per gnu89
 * semantics rather than c99. This prevents multiple symbol definition errors
 * of extern inline functions at link time.
 * A lot of inline functions can cause havoc with function tracing.
 */
/*
 * 全局 inline 采用 gnu_inline，使 extern inline 遵循 gnu89、不发出外部定义，
 * 避免 C99 语义造成重复符号；同时附加 unused 策略与 notrace，避免大量内联
 * helper 干扰函数跟踪。
 */
#define inline inline __gnu_inline __inline_maybe_unused notrace

/*
 * gcc provides both __inline__ and __inline as alternate spellings of
 * the inline keyword, though the latter is undocumented. New kernel
 * code should only use the inline spelling, but some existing code
 * uses __inline__. Since we #define inline above, to ensure
 * __inline__ has the same semantics, we need this #define.
 *
 * However, the spelling __inline is strictly reserved for referring
 * to the bare keyword.
 */
/*
 * GCC 的 __inline__ 是 inline 别名；内核已重定义 inline，故把双下划线拼写也
 * 映射到同一语义。单尾下划线 __inline 保留为编译器原始关键字，不重定义。
 */
#define __inline__ inline

/*
 * GCC does not warn about unused static inline functions for -Wunused-function.
 * Suppress the warning in clang as well by using __maybe_unused, but enable it
 * for W=2 build. This will allow clang to find unused functions.
 */
/* GCC 默认不警告未使用 static inline；Clang 通常加 __maybe_unused 对齐行为，但 W=2 刻意移除以发现死 helper。 */
#ifdef KBUILD_EXTRA_WARN2
#define __inline_maybe_unused
#else
#define __inline_maybe_unused __maybe_unused
#endif

/*
 * Rather then using noinline to prevent stack consumption, use
 * noinline_for_stack instead.  For documentation reasons.
 */
/* 为减少栈消耗而禁止内联时使用 noinline_for_stack，机器效果同 noinline，但把设计原因留在调用点。 */
#define noinline_for_stack noinline

/*
 * Use noinline_for_tracing for functions that should not be inlined.
 * For tracing reasons.
 */
/* 因跟踪边界必须保留函数实体时使用 noinline_for_tracing，效果同 noinline。 */
#define noinline_for_tracing noinline

/*
 * Sanitizer helper attributes: Because using __always_inline and
 * __no_sanitize_* conflict, provide helper attributes that will either expand
 * to __no_sanitize_* in compilation units where instrumentation is enabled
 * (__SANITIZE_*__), or __always_inline in compilation units without
 * instrumentation (__SANITIZE_*__ undefined).
 */
/*
 * sanitizer helper 在“关闭插桩”与“强制内联”冲突时二选一：当前翻译单元正被
 * sanitizer 插桩就使用 no_sanitize+notrace+maybe_unused，否则用
 * __always_inline，避免属性冲突或无意丢失检查。
 */
#ifdef __SANITIZE_ADDRESS__
/*
 * We can't declare function 'inline' because __no_sanitize_address conflicts
 * with inlining. Attempt to inline it may cause a build failure.
 *     https://gcc.gnu.org/bugzilla/show_bug.cgi?id=67368
 * '__maybe_unused' allows us to avoid defined-but-not-used warnings.
 */
/* KASAN 构建中 no_sanitize_address 与 inline 冲突（GCC PR67368），故禁止插桩且不强制内联。 */
# define __no_kasan_or_inline __no_sanitize_address notrace __maybe_unused
# define __no_sanitize_or_inline __no_kasan_or_inline
#else
# define __no_kasan_or_inline __always_inline
#endif

#ifdef CONFIG_KCSAN
/*
 * Type qualifier to mark variables where all data-racy accesses should be
 * ignored by KCSAN. Note, the implementation simply marks these variables as
 * volatile, since KCSAN will treat such accesses as "marked".
 *
 * Defined here because defining __data_racy as volatile for KCSAN objects only
 * causes problems in BPF Type Format (BTF) generation since struct members
 * of core kernel data structs will be volatile in some objects and not in
 * others.  Instead define it globally for KCSAN kernels.
 */
/*
 * __data_racy 标记“此变量的所有竞态访问都由设计允许”。KCSAN 以 volatile
 * 识别 marked access；必须在所有对象中全局一致，否则核心结构成员的限定符
 * 差异会破坏 BTF。它只抑制检测，不提供原子性或内存顺序。
 */
# define __data_racy volatile
#else
# define __data_racy
#endif

#ifdef __SANITIZE_THREAD__
/*
 * Clang still emits instrumentation for __tsan_func_{entry,exit}() and builtin
 * atomics even with __no_sanitize_thread (to avoid false positives in userspace
 * ThreadSanitizer). The kernel's requirements are stricter and we really do not
 * want any instrumentation with __no_kcsan.
 *
 * Therefore we add __disable_sanitizer_instrumentation where available to
 * disable all instrumentation. See Kconfig.kcsan where this is mandatory.
 */
/*
 * Clang 即使用 no_sanitize_thread 仍可能插入函数入口/出口和 builtin atomic；
 * 内核 __no_kcsan 叠加 disable_sanitizer_instrumentation，确保完全无插桩。
 */
# define __no_kcsan __no_sanitize_thread __disable_sanitizer_instrumentation
# define __no_sanitize_or_inline __no_kcsan notrace __maybe_unused
#else
# define __no_kcsan
#endif

#ifdef __SANITIZE_MEMORY__
/*
 * Similarly to KASAN and KCSAN, KMSAN loses function attributes of inlined
 * functions, therefore disabling KMSAN checks also requires disabling inlining.
 *
 * __no_sanitize_or_inline effectively prevents KMSAN from reporting errors
 * within the function and marks all its outputs as initialized.
 */
/* KMSAN 同样会在内联时丢属性；该分支关闭函数内报告，并把所有输出视作已初始化。 */
# define __no_sanitize_or_inline __no_kmsan_checks notrace __maybe_unused
#endif

#ifndef __no_sanitize_or_inline
#define __no_sanitize_or_inline __always_inline
#endif

/*
 * The assume attribute is used to indicate that a certain condition is
 * assumed to be true. If this condition is violated at runtime, the behavior
 * is undefined. Compilers may or may not use this indication to generate
 * optimized code.
 *
 * Note that the clang documentation states that optimizers may react
 * differently to this attribute, and this may even have a negative
 * performance impact. Therefore this attribute should be used with care.
 *
 * Optional: only supported since gcc >= 13
 * Optional: only supported since clang >= 19
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Statement-Attributes.html#index-assume-statement-attribute
 * clang: https://clang.llvm.org/docs/AttributeReference.html#id13
 *
 */
/*
 * __assume(@expr) 告诉优化器条件恒真；运行时违反是未定义行为，且可能负优化。
 * 仅在配置确认工具链支持时启用，必须用真实不变量证明，不能替代输入校验。
 */
#ifdef CONFIG_CC_HAS_ASSUME
# define __assume(expr)			__attribute__((__assume__(expr)))
#else
# define __assume(expr)
#endif

/*
 * Optional: only supported since gcc >= 15
 * Optional: only supported since clang >= 18
 *
 *   gcc: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=108896
 * clang: https://clang.llvm.org/docs/AttributeReference.html#counted-by-counted-by-or-null-sized-by-sized-by-or-null
 *
 * __bdos on clang < 19.1.2 can erroneously return 0:
 * https://github.com/llvm/llvm-project/pull/110497
 *
 * __bdos on clang < 19.1.3 can be off by 4:
 * https://github.com/llvm/llvm-project/pull/112636
 */
/*
 * __counted_by(@member) 把柔性数组与元素计数字段关联，供动态对象大小、FORTIFY
 * 和边界 sanitizer 使用。旧 Clang 的 __bdos 返回 0/偏差 4 缺陷见原文；空
 * 实现不代表计数不变量可以省略。
 */
#ifdef CONFIG_CC_HAS_COUNTED_BY
# define __counted_by(member)		__attribute__((__counted_by__(member)))
#else
# define __counted_by(member)
#endif

/*
 * Runtime track number of objects pointed to by a pointer member for use by
 * CONFIG_FORTIFY_SOURCE and CONFIG_UBSAN_BOUNDS.
 *
 * Optional: only supported since gcc >= 16
 * Optional: only supported since clang >= 22
 *
 *   gcc: https://gcc.gnu.org/pipermail/gcc-patches/2025-April/681727.html
 * clang: https://clang.llvm.org/docs/AttributeReference.html#counted-by-counted-by-or-null-sized-by-sized-by-or-null
 */
/* __counted_by_ptr(@member) 为指针成员记录所指对象数量，供 FORTIFY/UBSAN_BOUNDS 跟踪；不支持时为空。 */
#ifdef CONFIG_CC_HAS_COUNTED_BY_PTR
#define __counted_by_ptr(member)	__attribute__((__counted_by__(member)))
#else
#define __counted_by_ptr(member)
#endif

/*
 * Optional: only supported since gcc >= 15
 * Optional: not supported by Clang
 *
 * gcc: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=117178
 */
/* __nonstring_array 标记多维数组不可假定 NUL 结尾；当前仅新 GCC 支持。 */
#ifdef CONFIG_CC_HAS_MULTIDIMENSIONAL_NONSTRING
# define __nonstring_array		__attribute__((__nonstring__))
#else
# define __nonstring_array
#endif

/*
 * Apply __counted_by() when the Endianness matches to increase test coverage.
 */
/* 仅在目标端序匹配时应用 counted_by，便于同一布局在大小端构建中分别增加覆盖。 */
#ifdef __LITTLE_ENDIAN
#define __counted_by_le(member)	__counted_by(member)
#define __counted_by_be(member)
#else
#define __counted_by_le(member)
#define __counted_by_be(member)	__counted_by(member)
#endif

/*
 * This designates the minimum number of elements a passed array parameter must
 * have. For example:
 *
 *     void some_function(u8 param[at_least 7]);
 *
 * If a caller passes an array with fewer than 7 elements, the compiler will
 * emit a warning.
 */
/* at_least 展开为数组形参 static，声明调用者至少提供指定元素数；Sparse 分支为空，且它不是运行时检查。 */
#ifndef __CHECKER__
#define at_least static
#else
#define at_least
#endif

/* Section for code which can't be instrumented at all */
/*
 * __noinstr_section(@section) 同时禁止内联、ftrace、KCSAN、KASAN、profile、
 * coverage 与 KMSAN。noinstr 选择 .noinstr.text；属性本身不关闭中断或抢占。
 */
#define __noinstr_section(section)					\
	noinline notrace __attribute((__section__(section)))		\
	__no_kcsan __no_sanitize_address __no_profile __no_sanitize_coverage \
	__no_sanitize_memory

#define noinstr __noinstr_section(".noinstr.text")

/*
 * The __cpuidle section is used twofold:
 *
 *  1) the original use -- identifying if a CPU is 'stuck' in idle state based
 *     on it's instruction pointer. See cpu_in_idle().
 *
 *  2) supressing instrumentation around where cpuidle disables RCU; where the
 *     function isn't strictly required for #1, this is interchangeable with
 *     noinstr.
 */
/*
 * __cpuidle 既让 cpu_in_idle() 通过指令指针识别 idle，也覆盖 cpuidle 关闭 RCU
 * 后不可插桩的区域；不需要前一用途时可与 noinstr 互换。
 */
#define __cpuidle __noinstr_section(".cpuidle.text")

#endif /* __KERNEL__ */

#endif /* __ASSEMBLY__ */

/*
 * The below symbols may be defined for one or more, but not ALL, of the above
 * compilers. We don't consider that to be an error, so set them to nothing.
 * For example, some of them are for compiler specific plugins.
 */
/* 下列名字可能只由部分编译器/插件定义；缺失不是错误，统一补为空保持调用点可移植。 */
/*
 * __latent_entropy 标记可为熵插件贡献状态；RANDSTRUCT 的 randomize/no_randomize
 * 控制结构布局随机化，fields_start/end 只在启用时生成可能带 padding 的匿名
 * 结构。__no_kstack_erase、__noscs 分别允许工具链跳过内核栈擦除与 shadow call
 * stack；__nocfi 关闭 KCFI，__nocfi_generic 只在通用 LLVM CFI pass 架构映射到
 * 它。所有空兜底都表示本构建无对应插件/机制，不能作为运行时安全保证。
 */
#ifndef __latent_entropy
# define __latent_entropy
#endif

#if defined(RANDSTRUCT) && !defined(__CHECKER__)
# define __randomize_layout __designated_init __attribute__((randomize_layout))
# define __no_randomize_layout __attribute__((no_randomize_layout))
/* This anon struct can add padding, so only enable it under randstruct. */
/* 匿名结构可能增加 padding，只在 RANDSTRUCT 真正随机布局时包裹字段。 */
# define randomized_struct_fields_start	struct {
# define randomized_struct_fields_end	} __randomize_layout;
#else
# define __randomize_layout __designated_init
# define __no_randomize_layout
# define randomized_struct_fields_start
# define randomized_struct_fields_end
#endif

#ifndef __no_kstack_erase
# define __no_kstack_erase
#endif

#ifndef __noscs
# define __noscs
#endif

#if defined(CONFIG_CFI)
# define __nocfi		__attribute__((__no_sanitize__("kcfi")))
#else
# define __nocfi
#endif

#if defined(CONFIG_ARCH_USES_CFI_GENERIC_LLVM_PASS)
# define __nocfi_generic	__nocfi
#else
# define __nocfi_generic
#endif

/*
 * Any place that could be marked with the "alloc_size" attribute is also
 * a place to be marked with the "malloc" attribute, except those that may
 * be performing a _reallocation_, as that may alias the existing pointer.
 * For these, use __realloc_size().
 */
/*
 * 新分配函数可同时标 alloc_size 与 malloc（返回不别名）；realloc 可能与旧指针
 * 别名，必须用不带 malloc 的 __realloc_size。工具链缺能力时走保守空标注。
 */
#ifdef __alloc_size__
# define __alloc_size(x, ...)	__alloc_size__(x, ## __VA_ARGS__) __malloc
# define __realloc_size(x, ...)	__alloc_size__(x, ## __VA_ARGS__)
#else
# define __alloc_size(x, ...)	__malloc
# define __realloc_size(x, ...)
#endif

/*
 * When the size of an allocated object is needed, use the best available
 * mechanism to find it. (For cases where sizeof() cannot be used.)
 *
 * Optional: only supported since gcc >= 12
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 * clang: https://clang.llvm.org/docs/LanguageExtensions.html#evaluating-object-size
 */
/*
 * sizeof 不可用时，__struct_size(@p) 查询完整目标对象，__member_size 查询最近
 * 子对象；优先 dynamic builtin，否则退回静态 object_size。它们不延长对象生命周期。
 */
#if __has_builtin(__builtin_dynamic_object_size)
#define __struct_size(p)	__builtin_dynamic_object_size(p, 0)
#define __member_size(p)	__builtin_dynamic_object_size(p, 1)
#else
#define __struct_size(p)	__builtin_object_size(p, 0)
#define __member_size(p)	__builtin_object_size(p, 1)
#endif

/*
 * Determine if an attribute has been applied to a variable.
 * Using __annotated needs to check for __annotated being available,
 * or negative tests may fail when annotation cannot be checked. For
 * example, see the definition of __is_cstr().
 */
/* __annotated(@var,@attr) 查询属性；调用方须先确认宏存在，避免缺能力时负向测试误判。 */
#if __has_builtin(__builtin_has_attribute)
#define __annotated(var, attr)	__builtin_has_attribute(var, attr)
#endif

/*
 * Optional: only supported since gcc >= 15, clang >= 19
 *
 *   gcc: https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html#index-_005f_005fbuiltin_005fcounted_005fby_005fref
 * clang: https://clang.llvm.org/docs/LanguageExtensions.html#builtin-counted-by-ref
 */
#if __has_builtin(__builtin_counted_by_ref) && \
    !defined(CONFIG_CC_HAS_BROKEN_COUNTED_BY_REF)
/**
 * __flex_counter() - Get pointer to counter member for the given
 *                    flexible array, if it was annotated with __counted_by()
 * @FAM: Pointer to flexible array member of an addressable struct instance
 *
 * For example, with:
 *
 *	struct foo {
 *		int counter;
 *		short array[] __counted_by(counter);
 *	} *p;
 *
 * __flex_counter(p->array) will resolve to &p->counter.
 *
 * Note that Clang may not allow this to be assigned to a separate
 * variable; it must be used directly.
 *
 * If p->array is unannotated, this returns (void *)NULL.
 */
/*
 * __flex_counter() - 取得带 __counted_by 的柔性数组对应计数字段地址
 * @FAM: 可寻址结构实例中的柔性数组成员，借用且必须来自真实成员。
 * 示例 p->array 解析为 &p->counter；未标注或工具链不支持返回 NULL。Clang
 * 可能要求结果直接使用。宏不读取计数，也不取得结构引用。
 */
#define __flex_counter(FAM)	__builtin_counted_by_ref(FAM)
#else
#define __flex_counter(FAM)	((void *)NULL)
#endif

/*
 * Some versions of gcc do not mark 'asm goto' volatile:
 *
 *  https://gcc.gnu.org/bugzilla/show_bug.cgi?id=103979
 *
 * We do it here by hand, because it doesn't hurt.
 */
/* 某些 GCC 未把 asm goto 视为 volatile（PR103979），显式补 volatile 即使冗余也无害。 */
#ifndef asm_goto_output
#define asm_goto_output(x...) asm volatile goto(x)
#endif

/*
 * Clang has trouble with constraints with multiple
 * alternative behaviors ("g" , "rm" and "=rm").
 */
/* Clang 难处理多替代约束，缺省集中定义简单输入/输出约束供编译器头覆盖。 */
#ifndef ASM_INPUT_G
  #define ASM_INPUT_G "g"
  #define ASM_INPUT_RM "rm"
  #define ASM_OUTPUT_RM "=rm"
#endif

#ifdef CONFIG_CC_HAS_ASM_INLINE
#define asm_inline asm __inline
#else
#define asm_inline asm
#endif

/* asm_inline 在工具链支持时提示汇编体按 inline 成本计算，否则保持普通 asm；不改变汇编约束语义。 */

#ifndef __ASSEMBLY__
/*
 * Use __typeof_unqual__() when available.
 */
/* 工具链或 Sparse 支持时启用 __typeof_unqual__，使后续类型推导直接去限定符。 */
#if CC_HAS_TYPEOF_UNQUAL || defined(__CHECKER__)
# define USE_TYPEOF_UNQUAL 1
#endif

/* Are two types/vars the same type (ignoring qualifiers)? */
/* __same_type(@a,@b) 不求值参数，比较忽略限定符后的类型兼容性。 */
#define __same_type(a, b) __builtin_types_compatible_p(typeof(a), typeof(b))

/*
 * __unqual_scalar_typeof(x) - Declare an unqualified scalar type, leaving
 *			       non-scalar types unchanged.
 */
/* __unqual_scalar_typeof(@x) 去除标量限定符、保留非标量类型；结果是类型，不执行 @x。 */
#ifndef USE_TYPEOF_UNQUAL
/*
 * Prefer C11 _Generic for better compile-times and simpler code. Note: 'char'
 * is not type-compatible with 'signed char', and we define a separate case.
 */
/* 无原生 typeof_unqual 时用 C11 _Generic 映射标量；char 与 signed char 不兼容，故单列。 */
#define __scalar_type_to_expr_cases(type)				\
		unsigned type:	(unsigned type)0,			\
		signed type:	(signed type)0

#define __unqual_scalar_typeof(x) typeof(				\
		_Generic((x),						\
			 char:	(char)0,				\
			 __scalar_type_to_expr_cases(char),		\
			 __scalar_type_to_expr_cases(short),		\
			 __scalar_type_to_expr_cases(int),		\
			 __scalar_type_to_expr_cases(long),		\
			 __scalar_type_to_expr_cases(long long),	\
			 default: (x)))
#else
#define __unqual_scalar_typeof(x) __typeof_unqual__(x)
#endif

#include <asm/percpu_types.h>

#endif /* !__ASSEMBLY__ */

/*
 * __signed_scalar_typeof(x) - Declare a signed scalar type, leaving
 *			       non-scalar types unchanged.
 */
/* __signed_scalar_typeof(@x) 把无符号标量映射为同宽有符号类型，非标量不变。 */

#define __scalar_type_to_signed_cases(type)				\
		unsigned type:	(signed type)0,				\
		signed type:	(signed type)0

#define __signed_scalar_typeof(x) typeof(				\
		_Generic((x),						\
			 char:	(signed char)0,				\
			 __scalar_type_to_signed_cases(char),		\
			 __scalar_type_to_signed_cases(short),		\
			 __scalar_type_to_signed_cases(int),		\
			 __scalar_type_to_signed_cases(long),		\
			 __scalar_type_to_signed_cases(long long),	\
			 default: (x)))

/* Is this type a native word size -- useful for atomic operations */
/* __native_word(@t) 判断大小是否为 char/short/int/long 之一，作为原子访问宽度门禁。 */
#define __native_word(t) \
	(sizeof(t) == sizeof(char) || sizeof(t) == sizeof(short) || \
	 sizeof(t) == sizeof(int) || sizeof(t) == sizeof(long))

#ifdef __OPTIMIZE__
/*
 * #ifdef __OPTIMIZE__ is only a good approximation; for instance "make
 * CFLAGS_foo.o=-Og" defines __OPTIMIZE__, does not elide the conditional code
 * and can break compilation with wrong error message(s). Combine with
 * -U__OPTIMIZE__ when needed.
 */
/*
 * __OPTIMIZE__ 只是近似，单文件 -Og 仍可能定义它并保留条件代码，必要时需
 * -U__OPTIMIZE__。优化构建借 compiletime_error 制造仅在条件为假时可达的
 * 唯一符号；非优化构建只做 void 转换，诊断能力较弱。
 */
# define __compiletime_assert(condition, msg, prefix, suffix)		\
	do {								\
		/*							\
		 * __noreturn is needed to give the compiler enough	\
		 * information to avoid certain possibly-uninitialized	\
		 * warnings (regardless of the build failing).		\
		 */							\
		__noreturn extern void prefix ## suffix(void)		\
			__compiletime_error(msg);			\
		if (!(condition))					\
			prefix ## suffix();				\
	} while (0)
#else
# define __compiletime_assert(condition, msg, prefix, suffix) ((void)(condition))
#endif

/*
 * 宏内英文说明的含义是：__noreturn 给优化器足够信息，即使构建最终失败，也能
 * 避免产生无关的 possibly-uninitialized 告警；它不改变失败符号只用于诊断的事实。
 */

#define _compiletime_assert(condition, msg, prefix, suffix) \
	__compiletime_assert(condition, msg, prefix, suffix)

/**
 * compiletime_assert - break build and emit msg if condition is false
 * @condition: a compile-time constant condition to check
 * @msg:       a message to emit if condition is false
 *
 * In tradition of POSIX assert, this macro will break the build if the
 * supplied condition is *false*, emitting the supplied error message if the
 * compiler has support to do so.
 */
/*
 * compiletime_assert(@condition,@msg) 沿用 POSIX assert 极性：条件为假时破坏
 * 构建并输出消息。__COUNTER__ 生成唯一失败符号；atomic_type 版本要求原生字长。
 */
#define compiletime_assert(condition, msg) \
	_compiletime_assert(condition, msg, __compiletime_assert_, __COUNTER__)

#define compiletime_assert_atomic_type(t)				\
	compiletime_assert(__native_word(t),				\
		"Need native word sized stores/loads for atomicity.")

/* Helpers for emitting diagnostics in pragmas. */
/*
 * __diag_* 统一 pragma 的 push/pop 和按编译器版本设置 ignore/warn/error；
 * @comment 记录调用点理由但不进入展开。设置应成对限定作用域，后端缺失时为空。
 */
#ifndef __diag
#define __diag(string)
#endif

#ifndef __diag_GCC
#define __diag_GCC(version, severity, string)
#endif

#ifndef __diag_clang
#define __diag_clang(version, severity, string)
#endif

#define __diag_push()	__diag(push)
#define __diag_pop()	__diag(pop)

#define __diag_ignore(compiler, version, option, comment) \
	__diag_ ## compiler(version, ignore, option)
#define __diag_warn(compiler, version, option, comment) \
	__diag_ ## compiler(version, warn, option)
#define __diag_error(compiler, version, option, comment) \
	__diag_ ## compiler(version, error, option)

#ifndef __diag_ignore_all
#define __diag_ignore_all(option, comment)
#endif

#endif /* __LINUX_COMPILER_TYPES_H */
