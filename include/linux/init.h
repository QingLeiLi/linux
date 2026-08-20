/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_INIT_H
#define _LINUX_INIT_H

#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/stringify.h>
#include <linux/types.h>

/* These macros are used to mark some functions or
 * initialized data (doesn't apply to uninitialized data)
 * as `initialization' functions. The kernel can take this
 * as hint that the function is used only during the initialization
 * phase and free up used memory resources after
 *
 * Usage:
 * For functions:
 *
 * You should add __init immediately before the function name, like:
 *
 * static void __init initme(int x, int y)
 * {
 *    extern int z; z = x * y;
 * }
 *
 * If the function has a prototype somewhere, you can also add
 * __init between closing brace of the prototype and semicolon:
 *
 * extern int initialize_foobar_device(int, int, int) __init;
 *
 * For initialized data:
 * You should insert __initdata or __initconst between the variable name
 * and equal sign followed by value, e.g.:
 *
 * static int init_variable __initdata = 0;
 * static const char linux_logo[] __initconst = { 0x32, 0x36, ... };
 *
 * Don't forget to initialize data not at file scope, i.e. within a function,
 * as gcc otherwise puts the data into the bss section and not into the init
 * section.
 */
/**
 * 初始化标记宏
 *
 * 这些宏用于标记仅在内核初始化阶段使用的函数和数据。
 *
 * 【目的】
 * - 内存优化：启动完成后释放初始化代码/数据，节省运行时内存
 * - 原理：将标记的代码/数据放入特殊段（.init.*），启动后通过free_initmem()释放
 *
 * 【使用方法】
 * 函数：在函数名前添加__init
 *   static void __init initme(int x, int y) { ... }
 * 或在函数原型的括号后添加__init
 *   extern int initialize_foobar_device(int, int, int) __init;
 *
 * 数据：在变量名和等号之间添加__initdata或__initconst
 *   static int init_variable __initdata = 0;
 *   static const char linux_logo[] __initconst = { 0x32, 0x36, ... };
 *
 * 【注意】
 * - 只能标记已初始化的数据（未初始化数据在BSS段，不适用）
 * - 函数内的静态变量必须显式初始化，否则gcc会放入BSS而非.init段
 * - __init函数只能在初始化阶段调用，运行时调用会访问已释放内存
 */

/* These are for everybody (although not all archs will actually
   discard it in modules) */
/**
 * 这些宏适用于所有代码（内核和模块）
 * 但某些架构的模块可能不会真正丢弃这些段
 */
#define __init		__section(".init.text") __cold __latent_entropy	\
						__no_kstack_erase
/* __init - 标记初始化函数，放入.init.text段，启动后释放
 * __cold: 标记为冷代码，优化缓存布局
 * __latent_entropy: 收集熵用于随机数生成
 * __no_kstack_erase: 不擦除栈（因为代码会被释放）
 */

#define __initdata	__section(".init.data")
/* __initdata - 标记可修改的初始化数据，放入.init.data段 */

#define __initconst	__section(".init.rodata")
/* __initconst - 标记只读的初始化常量，放入.init.rodata段 */

#define __exitdata	__section(".exit.data")
/* __exitdata - 标记退出数据，用于模块卸载 */

#define __exit_call	__maybe_unused __section(".exitcall.exit")
/* __exit_call - 标记退出回调
 * __maybe_unused: 内核内置代码永不调用，但需保持接口一致
 */

/*
 * modpost check for section mismatches during the kernel build.
 * A section mismatch happens when there are references from a
 * code or data section to an init section (both code or data).
 * The init sections are (for most archs) discarded by the kernel
 * when early init has completed so all such references are potential bugs.
 * For exit sections the same issue exists.
 *
 * The following markers are used for the cases where the reference to
 * the *init / *exit section (code or data) is valid and will teach
 * modpost not to issue a warning.  Intended semantics is that a code or
 * data tagged __ref* can reference code or data from init section without
 * producing a warning (of course, no warning does not mean code is
 * correct, so optimally document why the __ref is needed and why it's OK).
 *
 * The markers follow same syntax rules as __init / __initdata.
 */
/**
 * 段不匹配（Section Mismatch）检测
 *
 * modpost在编译时检查段不匹配错误。
 *
 * 【什么是段不匹配】
 * 当常规代码/数据引用__init段的代码/数据时发生。
 * - __init段在启动后会被释放（通过free_initmem()）
 * - 运行时引用已释放内存会导致崩溃
 *
 * 【__ref*标记的用途】
 * 某些情况下引用是安全的（如CPU热插拔需要重新初始化）。
 * __ref*标记告诉modpost："这是经过验证的安全引用，不要警告"。
 *
 * 【重要】使用__ref*时必须：
 * 1. 确保引用时目标代码未被释放
 * 2. 添加注释说明为何安全
 * 3. 不要滥用来掩盖真正的bug
 *
 * 语法规则与__init/__initdata相同。
 */

#define __ref            __section(".ref.text") noinline
/* __ref - 标记可以安全引用__init代码的函数
 * noinline: 禁止内联，保持函数独立以便modpost正确分析引用关系
 */

#define __refdata        __section(".ref.data")
/* __refdata - 标记可以引用__init数据的数据 */

#define __refconst       __section(".ref.rodata")
/* __refconst - 标记可以引用__init数据的只读数据 */

#ifdef MODULE
#define __exitused
#else
#define __exitused  __used
#endif
/**
 * __exitused - 控制__exit函数是否被保留
 *
 * - MODULE定义时（模块）：定义为空，编译器会保留exit函数（模块可卸载）
 * - MODULE未定义时（内核内置）：定义为__used，强制保留符号
 *   原因：内核不能卸载，exit函数永不调用，但保持接口一致
 */

#define __exit          __section(".exit.text") __exitused __cold notrace
/* __exit - 标记退出/清理函数
 * __exitused: 根据是否为模块决定是否保留
 * __cold: 冷代码
 * notrace: 禁止函数跟踪（ftrace等工具不会记录）
 */

#ifdef CONFIG_MEMORY_HOTPLUG
#define __meminit
#define __meminitdata
#define __meminitconst
#else
#define __meminit	__init
#define __meminitdata	__initdata
#define __meminitconst	__initconst
#endif
/**
 * 内存热插拔相关的初始化标记
 *
 * 【设计考虑】
 * 启用CONFIG_MEMORY_HOTPLUG时，内存可以动态添加/移除。
 * 内存初始化代码需要在运行时重复执行，因此不能标记为__init释放。
 *
 * - CONFIG_MEMORY_HOTPLUG=y: 定义为空，代码保留在常规段
 * - CONFIG_MEMORY_HOTPLUG=n: 定义为__init*，启动后释放节省内存
 */

/* For assembly routines */
/** 汇编代码使用的段定义宏 */
#define __HEAD		.section	".head.text","ax"
/* __HEAD - 头部代码段，用于架构相关的最早期启动代码 */

#define __INIT		.section	".init.text","ax"
/* __INIT - 初始化代码段 */

#define __FINIT		.previous
/* __FINIT - 结束初始化段，返回之前的段 */

#define __INITDATA	.section	".init.data","aw",%progbits
/* __INITDATA - 初始化数据段（可读写） */

#define __INITRODATA	.section	".init.rodata","a",%progbits
/* __INITRODATA - 初始化只读数据段 */

#define __FINITDATA	.previous
/* __FINITDATA - 结束初始化数据段，返回之前的段 */

/* silence warnings when references are OK */
/** 用于汇编代码中抑制段引用警告 */
#define __REF            .section       ".ref.text", "ax"
/* __REF - 引用段，可以安全引用__init代码 */

#define __REFDATA        .section       ".ref.data", "aw"
/* __REFDATA - 引用数据段 */

#define __REFCONST       .section       ".ref.rodata", "a"
/* __REFCONST - 引用只读数据段 */

#ifndef __ASSEMBLY__
/*
 * Used for initialization calls..
 */
/**
 * 初始化调用相关的类型定义
 *
 * initcall机制：内核启动时按顺序调用各子系统的初始化函数
 */
typedef int (*initcall_t)(void);
/* initcall_t - 初始化函数指针类型，返回int（0=成功） */

typedef void (*exitcall_t)(void);
/* exitcall_t - 退出函数指针类型 */

#ifdef CONFIG_HAVE_ARCH_PREL32_RELOCATIONS
typedef int initcall_entry_t;

static inline initcall_t initcall_from_entry(initcall_entry_t *entry)
{
	return offset_to_ptr(entry);
}
#else
typedef initcall_t initcall_entry_t;

static inline initcall_t initcall_from_entry(initcall_entry_t *entry)
{
	return *entry;
}
#endif
/**
 * initcall条目类型
 *
 * 【两种存储方式】
 * 1. CONFIG_HAVE_ARCH_PREL32_RELOCATIONS=y（位置无关）
 *    - 存储32位相对偏移量（节省空间）
 *    - initcall_from_entry()将偏移量转换为实际指针
 *
 * 2. CONFIG_HAVE_ARCH_PREL32_RELOCATIONS=n（绝对地址）
 *    - 直接存储函数指针
 *    - initcall_from_entry()直接解引用
 */

extern initcall_entry_t __con_initcall_start[], __con_initcall_end[];
/* __con_initcall_* - 控制台初始化调用表的起止位置 */

/* Used for constructor calls. */
/** 用于构造函数调用 */
typedef void (*ctor_fn_t)(void);
/* ctor_fn_t - 构造函数指针类型 */

struct file_system_type;

/* Defined in init/main.c */
/** 定义在init/main.c中的核心初始化函数 */
extern int do_one_initcall(initcall_t fn);
/* do_one_initcall - 执行一个initcall函数，处理错误和日志 */

extern char __initdata boot_command_line[];
/* boot_command_line - 启动命令行参数（initdata，启动后释放） */

extern char *saved_command_line;
/* saved_command_line - 保存的命令行参数（运行时可访问） */

extern unsigned int saved_command_line_len;
/* saved_command_line_len - 保存的命令行长度 */

extern unsigned int reset_devices;
/* reset_devices - 是否重置设备标志（从内核参数"reset_devices"设置） */

/* used by init/main.c */
/** init/main.c使用的架构相关初始化函数 */
void setup_arch(char **);
/* setup_arch - 架构相关的初始化（每个架构在arch/*/kernel/setup.c实现） */

void prepare_namespace(void);
/* prepare_namespace - 准备根文件系统挂载 */

void __init init_rootfs(void);
/* init_rootfs - 初始化rootfs文件系统 */

void init_IRQ(void);
/* init_IRQ - 初始化中断子系统 */

void time_init(void);
/* time_init - 初始化时间子系统 */

void poking_init(void);
/* poking_init - 初始化内存poking机制（安全地写入特殊地址） */

void pgtable_cache_init(void);
/* pgtable_cache_init - 初始化页表缓存 */

extern initcall_entry_t __initcall_start[];
extern initcall_entry_t __initcall0_start[];
extern initcall_entry_t __initcall1_start[];
extern initcall_entry_t __initcall2_start[];
extern initcall_entry_t __initcall3_start[];
extern initcall_entry_t __initcall4_start[];
extern initcall_entry_t __initcall5_start[];
extern initcall_entry_t __initcall6_start[];
extern initcall_entry_t __initcall7_start[];
extern initcall_entry_t __initcall_end[];
/**
 * initcall级别标记
 *
 * 内核初始化分为8个级别（0-7），按顺序执行：
 * - 0: early (最早期)
 * - 1: core (核心子系统)
 * - 2: postcore (核心后)
 * - 3: arch (架构相关)
 * - 4: subsys (子系统)
 * - 5: fs (文件系统)
 * - 6: device (设备驱动，默认级别)
 * - 7: late (最后)
 *
 * 链接器脚本将这些符号放在initcall段的不同位置，
 * 内核启动时遍历这些段依次调用初始化函数。
 */

extern struct file_system_type rootfs_fs_type;
/* rootfs_fs_type - rootfs文件系统类型 */

extern bool rodata_enabled;
/* rodata_enabled - 只读数据保护是否启用 */

void mark_rodata_ro(void);
/* mark_rodata_ro - 将内核只读数据段标记为真正只读（W^X保护） */

extern void (*late_time_init)(void);
/* late_time_init - 延迟时间初始化函数指针 */

extern bool initcall_debug;
/* initcall_debug - initcall调试标志（内核参数"initcall_debug"） */

#ifdef MODULE
extern struct module __this_module;
#define THIS_MODULE (&__this_module)
#else
#define THIS_MODULE ((struct module *)0)
#endif
/**
 * THIS_MODULE - 当前模块指针
 *
 * - 模块中：指向当前模块的module结构
 * - 内核内置代码：NULL指针
 *
 * 用于注册设备驱动时填充owner字段，防止模块在使用时被卸载。
 */

#endif

#ifndef MODULE

#ifndef __ASSEMBLY__

/*
 * initcalls are now grouped by functionality into separate
 * subsections. Ordering inside the subsections is determined
 * by link order.
 * For backwards compatibility, initcall() puts the call in
 * the device init subsection.
 *
 * The `id' arg to __define_initcall() is needed so that multiple initcalls
 * can point at the same handler without causing duplicate-symbol build errors.
 *
 * Initcalls are run by placing pointers in initcall sections that the
 * kernel iterates at runtime. The linker can do dead code / data elimination
 * and remove that completely, so the initcall sections have to be marked
 * as KEEP() in the linker script.
 */
/**
 * initcall分组和执行机制
 *
 * 【设计】
 * initcall现在按功能分组到不同的子段中。
 * 子段内的顺序由链接顺序决定。
 *
 * 【向后兼容】
 * initcall()宏将调用放入device级别（级别6）。
 *
 * 【id参数的作用】
 * __define_initcall()的id参数用于避免重复符号错误。
 * 允许多个initcall指向同一个处理函数。
 *
 * 【执行机制】
 * 通过将函数指针放入initcall段，内核启动时遍历这些段依次调用。
 *
 * 【链接器脚本要求】
 * initcall段必须标记为KEEP()，防止链接器dead code elimination优化移除。
 */

/* Format: <modname>__<counter>_<line>_<fn> */
/**
 * initcall唯一标识符生成
 * 格式: <模块名>__<计数器>_<行号>_<函数名>
 */
#define __initcall_id(fn)					\
	__PASTE(kmod_,						\
	__PASTE(__KBUILD_MODNAME,				\
	__PASTE(__,						\
	__PASTE(__COUNTER__,					\
	__PASTE(_,						\
	__PASTE(__LINE__,					\
	__PASTE(_, fn)))))))
/* __initcall_id - 为每个initcall生成唯一ID
 * 使用模块名、计数器、行号和函数名组合，保证唯一性
 */

/* Format: __<prefix>__<iid><id> */
/**
 * initcall符号名称生成
 * 格式: __<前缀>__<iid><id>
 */
#define __initcall_name(prefix, __iid, id)			\
	__PASTE(__,						\
	__PASTE(prefix,						\
	__PASTE(__,						\
	__PASTE(__iid, id))))
/* __initcall_name - 生成initcall的符号名称 */

#ifdef CONFIG_LTO_CLANG
/*
 * With LTO, the compiler doesn't necessarily obey link order for
 * initcalls. In order to preserve the correct order, we add each
 * variable into its own section and generate a linker script (in
 * scripts/link-vmlinux.sh) to specify the order of the sections.
 */
/**
 * LTO（链接时优化）下的initcall处理
 *
 * 启用LTO时，编译器不一定遵守链接顺序。
 * 为保持正确顺序，我们将每个变量放入独立的段，
 * 并生成链接器脚本（scripts/link-vmlinux.sh）指定段的顺序。
 */
#define __initcall_section(__sec, __iid)			\
	#__sec ".init.." #__iid
/* __initcall_section - 为每个initcall生成独立的段名 */

/*
 * With LTO, the compiler can rename static functions to avoid
 * global naming collisions. We use a global stub function for
 * initcalls to create a stable symbol name whose address can be
 * taken in inline assembly when PREL32 relocations are used.
 */
/**
 * LTO下的函数重命名问题
 *
 * 启用LTO时，编译器可能重命名静态函数以避免全局命名冲突。
 * 我们使用全局stub函数为initcall创建稳定的符号名，
 * 这样在使用PREL32重定位时可以在内联汇编中获取其地址。
 */
#define __initcall_stub(fn, __iid, id)				\
	__initcall_name(initstub, __iid, id)
/* __initcall_stub - 生成stub函数名 */

#define __define_initcall_stub(__stub, fn)			\
	int __init __stub(void);				\
	int __init __stub(void)					\
	{ 							\
		return fn();					\
	}							\
	__ADDRESSABLE(__stub)
/* __define_initcall_stub - 定义stub函数包装实际的initcall
 * __ADDRESSABLE确保符号不被优化掉
 */
#else
#define __initcall_section(__sec, __iid)			\
	#__sec ".init"
/* 非LTO模式：所有同级别initcall放在同一个段 */

#define __initcall_stub(fn, __iid, id)	fn
/* 非LTO模式：直接使用原函数，无需stub */

#define __define_initcall_stub(__stub, fn)			\
	__ADDRESSABLE(fn)
/* 非LTO模式：只需确保函数地址可被引用 */
#endif

#ifdef CONFIG_HAVE_ARCH_PREL32_RELOCATIONS
#define ____define_initcall(fn, __stub, __name, __sec)		\
	__define_initcall_stub(__stub, fn)			\
	asm(".section	\"" __sec "\", \"a\"		\n"	\
	    __stringify(__name) ":			\n"	\
	    ".long	" __stringify(__stub) " - .	\n"	\
	    ".previous					\n");	\
	static_assert(__same_type(initcall_t, &fn));
/**
 * 使用PREL32重定位的initcall定义
 *
 * 存储32位相对偏移量而非绝对地址：
 * - 节省空间（32位 vs 64位）
 * - 位置无关，利于KASLR（内核地址空间布局随机化）
 *
 * 内联汇编生成的段结构：
 *   .section "<段名>", "a"    // "a"表示可分配
 *   __name:                    // 符号标签
 *   .long __stub - .           // 相对偏移量（当前位置到stub函数）
 *   .previous                  // 返回之前的段
 *
 * static_assert确保fn的类型是initcall_t
 */
#else
#define ____define_initcall(fn, __unused, __name, __sec)	\
	static initcall_t __name __used 			\
		__attribute__((__section__(__sec))) = fn;
/**
 * 使用绝对地址的initcall定义
 *
 * 直接将函数指针存储在指定段中。
 * __used防止编译器优化掉未被显式调用的变量。
 */
#endif

#define __unique_initcall(fn, id, __sec, __iid)			\
	____define_initcall(fn,					\
		__initcall_stub(fn, __iid, id),			\
		__initcall_name(initcall, __iid, id),		\
		__initcall_section(__sec, __iid))
/* __unique_initcall - 组装所有参数，生成唯一的initcall定义 */

#define ___define_initcall(fn, id, __sec)			\
	__unique_initcall(fn, id, __sec, __initcall_id(fn))
/* ___define_initcall - 生成唯一ID并传递给__unique_initcall */

#define __define_initcall(fn, id) ___define_initcall(fn, id, .initcall##id)
/* __define_initcall - 根据级别id生成段名（如.initcall6） */

/*
 * Early initcalls run before initializing SMP.
 *
 * Only for built-in code, not modules.
 */
/**
 * early_initcall - 在SMP初始化前运行
 *
 * 只用于内核内置代码，不适用于模块。
 * 用于需要在多核启动前完成的初始化。
 */
#define early_initcall(fn)		__define_initcall(fn, early)

/*
 * A "pure" initcall has no dependencies on anything else, and purely
 * initializes variables that couldn't be statically initialized.
 *
 * This only exists for built-in code, not for modules.
 * Keep main.c:initcall_level_names[] in sync.
 */
/**
 * pure_initcall - "纯"初始化调用（级别0）
 *
 * 无任何依赖，纯粹初始化无法静态初始化的变量。
 * 只用于内核内置代码，不适用于模块。
 *
 * 【注意】需与main.c:initcall_level_names[]保持同步。
 */
#define pure_initcall(fn)		__define_initcall(fn, 0)

#define core_initcall(fn)		__define_initcall(fn, 1)
/* core_initcall - 核心子系统初始化（级别1） */

#define core_initcall_sync(fn)		__define_initcall(fn, 1s)
/* core_initcall_sync - 核心子系统同步点（级别1s） */

#define postcore_initcall(fn)		__define_initcall(fn, 2)
/* postcore_initcall - 核心后初始化（级别2） */

#define postcore_initcall_sync(fn)	__define_initcall(fn, 2s)
/* postcore_initcall_sync - 核心后同步点（级别2s） */

#define arch_initcall(fn)		__define_initcall(fn, 3)
/* arch_initcall - 架构相关初始化（级别3） */

#define arch_initcall_sync(fn)		__define_initcall(fn, 3s)
/* arch_initcall_sync - 架构相关同步点（级别3s） */

#define subsys_initcall(fn)		__define_initcall(fn, 4)
/* subsys_initcall - 子系统初始化（级别4） */

#define subsys_initcall_sync(fn)	__define_initcall(fn, 4s)
/* subsys_initcall_sync - 子系统同步点（级别4s） */

#define fs_initcall(fn)			__define_initcall(fn, 5)
/* fs_initcall - 文件系统初始化（级别5） */

#define fs_initcall_sync(fn)		__define_initcall(fn, 5s)
/* fs_initcall_sync - 文件系统同步点（级别5s） */

#define rootfs_initcall(fn)		__define_initcall(fn, rootfs)
/* rootfs_initcall - rootfs初始化（在级别6之前） */

#define device_initcall(fn)		__define_initcall(fn, 6)
/* device_initcall - 设备驱动初始化（级别6，默认级别） */

#define device_initcall_sync(fn)	__define_initcall(fn, 6s)
/* device_initcall_sync - 设备驱动同步点（级别6s） */

#define late_initcall(fn)		__define_initcall(fn, 7)
/* late_initcall - 延迟初始化（级别7，最后执行） */

#define late_initcall_sync(fn)		__define_initcall(fn, 7s)
/* late_initcall_sync - 延迟初始化同步点（级别7s） */

#define __initcall(fn) device_initcall(fn)
/* __initcall - 默认使用device级别（向后兼容） */

#define __exitcall(fn)						\
	static exitcall_t __exitcall_##fn __exit_call = fn
/* __exitcall - 定义模块退出函数 */

#define console_initcall(fn)	___define_initcall(fn, con, .con_initcall)
/* console_initcall - 控制台初始化，放入独立的.con_initcall段 */

struct obs_kernel_param {
	const char *str;
	int (*setup_func)(char *);
	int early;
};
/**
 * obs_kernel_param - 内核参数结构
 *
 * @str: 参数名称字符串（如"debug"）
 * @setup_func: 参数处理函数，接收参数值字符串
 * @early: 是否为early参数（在启动早期处理）
 *
 * 用于__setup和early_param宏定义的内核命令行参数。
 */

extern const struct obs_kernel_param __setup_start[], __setup_end[];
/* __setup_start/end - 内核参数表的起止位置 */

/*
 * Only for really core code.  See moduleparam.h for the normal way.
 *
 * Force the alignment so the compiler doesn't space elements of the
 * obs_kernel_param "array" too far apart in .init.setup.
 */
/**
 * __setup_param - 定义内核参数
 *
 * 只用于真正核心的代码。模块参数请使用moduleparam.h中的接口。
 *
 * 【对齐说明】
 * 强制对齐，防止编译器在.init.setup段中将obs_kernel_param数组元素
 * 间隔得太远（确保数组紧凑，易于遍历）。
 */
#define __setup_param(str, unique_id, fn, early)			\
	static const char __setup_str_##unique_id[] __initconst		\
		__aligned(1) = str; 					\
	static struct obs_kernel_param __setup_##unique_id		\
		__used __section(".init.setup")				\
		__aligned(__alignof__(struct obs_kernel_param))		\
		= { __setup_str_##unique_id, fn, early }
/**
 * 展开说明：
 * 1. 定义参数名字符串（__initconst，启动后释放）
 * 2. 定义obs_kernel_param结构（放入.init.setup段）
 * 3. __used防止优化，__aligned确保正确对齐
 */

/*
 * NOTE: __setup functions return values:
 * @fn returns 1 (or non-zero) if the option argument is "handled"
 * and returns 0 if the option argument is "not handled".
 */
/**
 * __setup - 定义内核启动参数
 *
 * 【注意】返回值约定：
 * @fn 返回1（或非0）表示参数已处理
 * @fn 返回0表示参数未处理
 *
 * 示例：__setup("debug", debug_setup);
 * 命令行传入"debug=1"时，会调用debug_setup("1")
 */
#define __setup(str, fn)						\
	__setup_param(str, fn, fn, 0)

/*
 * NOTE: @fn is as per module_param, not __setup!
 * I.e., @fn returns 0 for no error or non-zero for error
 * (possibly @fn returns a -errno value, but it does not matter).
 * Emits warning if @fn returns non-zero.
 */
/**
 * early_param - 定义早期内核参数
 *
 * 【注意】返回值约定与module_param相同，NOT __setup！
 * @fn 返回0表示成功，非0表示错误（可能是-errno值）
 * 如果@fn返回非0，会发出警告。
 *
 * 【区别】
 * early_param在启动早期处理，早于__setup。
 * 用于需要在启动最早期生效的参数（如内存配置）。
 */
#define early_param(str, fn)						\
	__setup_param(str, fn, fn, 1)

#define early_param_on_off(str_on, str_off, var, config)		\
									\
	int var = IS_ENABLED(config);					\
									\
	static int __init parse_##var##_on(char *arg)			\
	{								\
		var = 1;						\
		return 0;						\
	}								\
	early_param(str_on, parse_##var##_on);				\
									\
	static int __init parse_##var##_off(char *arg)			\
	{								\
		var = 0;						\
		return 0;						\
	}								\
	early_param(str_off, parse_##var##_off)
/**
 * early_param_on_off - 定义开关型早期参数
 *
 * @str_on: 开启参数名（如"feature"）
 * @str_off: 关闭参数名（如"nofeature"）
 * @var: 控制变量名
 * @config: 默认配置选项（如CONFIG_FEATURE）
 *
 * 自动生成：
 * - int var变量（初始值取决于config）
 * - str_on参数处理函数（设置var=1）
 * - str_off参数处理函数（设置var=0）
 *
 * 示例：early_param_on_off("debug", "nodebug", debug_enabled, CONFIG_DEBUG);
 */

/* Relies on boot_command_line being set */
/** 依赖于boot_command_line已被设置 */
void __init parse_early_param(void);
/* parse_early_param - 解析early_param参数 */

void __init parse_early_options(char *cmdline);
/* parse_early_options - 解析早期选项（可指定命令行） */

#endif /* __ASSEMBLY__ */

#else /* MODULE */

#define __setup_param(str, unique_id, fn)	/* nothing */
#define __setup(str, func) 			/* nothing */
/**
 * 模块中的空定义
 *
 * 模块不使用__setup机制（使用module_param代替）。
 * 为保持接口一致，在MODULE定义时将这些宏定义为空。
 */
#endif

/* Data marked not to be saved by software suspend */
/** 标记为不被软件休眠保存的数据 */
#define __nosavedata __section(".data..nosave")
/* __nosavedata - 休眠时不保存的数据，放入.data..nosave段
 * 用于标记临时状态或运行时信息，恢复后需要重新初始化
 */

#ifdef MODULE
#define __exit_p(x) x
#else
#define __exit_p(x) NULL
#endif
/**
 * __exit_p - exit函数指针包装
 *
 * - MODULE定义时（模块）：返回实际函数指针（模块可卸载）
 * - MODULE未定义时（内核内置）：返回NULL（内核不卸载，exit函数永不调用）
 *
 * 用于在结构体中有条件地保存exit函数指针。
 */

#endif /* _LINUX_INIT_H */
