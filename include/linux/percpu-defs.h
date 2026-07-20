/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * linux/percpu-defs.h - basic definitions for percpu areas
 *
 * DO NOT INCLUDE DIRECTLY OUTSIDE PERCPU IMPLEMENTATION PROPER.
 *
 * This file is separate from linux/percpu.h to avoid cyclic inclusion
 * dependency from arch header files.  Only to be included from
 * asm/percpu.h.
 *
 * This file includes macros necessary to declare percpu sections and
 * variables, and definitions of percpu accessors and operations.  It
 * should provide enough percpu features to arch header files even when
 * they can only include asm/percpu.h to avoid cyclic inclusion dependency.
 *
 * ============================================================
 * 【per-CPU 变量机制总览】
 *
 * per-CPU 变量为系统中每个 CPU 核心各保留一份独立副本。其核心思想是：
 * 通过消除跨 CPU 的共享访问，从根本上避免缓存行竞争（false sharing）
 * 和自旋锁开销，是内核高频路径（调度器、统计计数、中断处理等）的
 * 重要性能优化手段。
 *
 * 内存布局（示意）：
 *
 *   __per_cpu_start                   __per_cpu_end
 *   |<---------- per-CPU 模板区 ---------->|
 *   | var_A | var_B | var_C | ...          |  ← 链接器放在 .data..percpu 节
 *
 *   运行时，内核为每个 CPU 复制一份模板区：
 *   CPU0 数据区: | var_A副本 | var_B副本 | ... |  基址 = __per_cpu_offset[0]
 *   CPU1 数据区: | var_A副本 | var_B副本 | ... |  基址 = __per_cpu_offset[1]
 *   CPU2 数据区: | var_A副本 | var_B副本 | ... |  基址 = __per_cpu_offset[2]
 *
 * 访问时：
 *   per_cpu_ptr(ptr, cpu) = ptr（模板区中的地址）+ __per_cpu_offset[cpu]
 *
 * 单处理器（!CONFIG_SMP）退化为普通全局变量，cpu 参数被忽略。
 * ============================================================
 */
/*
var 存储在哪？
通过 DEFINE_PER_CPU(int, my_var) 定义后，my_var 被放入 ELF 的 .data..percpu 节（per-CPU 模板区）。内核启动时，setup_per_cpu_areas() 为每个 CPU 把这段模板区复制一份，各自分配独立内存，每份的起始偏移记录在 __per_cpu_offset[cpu] 数组里。

cpu 是什么？
就是 CPU 编号整数（0、1、2……），用来查 __per_cpu_offset[cpu]。

per_cpu 如何取出来？
per_cpu(my_var, 2)
  → *per_cpu_ptr(&my_var, 2)
  → *(SHIFT_PERCPU_PTR(&my_var, __per_cpu_offset[2]))
  → *(&my_var 的整数值 + 0x3000)   ← 0x3000 是 CPU2 的偏移
  → CPU2 那份 my_var 的左值
*/
#ifndef _LINUX_PERCPU_DEFS_H
#define _LINUX_PERCPU_DEFS_H

/*
 * per-CPU 变量的 ELF section 名称后缀。
 *
 * 链接器脚本用这些后缀将 per-CPU 变量归入不同的子节，从而控制对齐方式：
 *   - 普通变量放在 .data..percpu（后缀为 ""）
 *   - shared_aligned 变量要求 SMP 下按缓存行对齐，减少不同 CPU 访问时的
 *     缓存行争用（false sharing）
 *   - 模块（MODULE）不使用命名子节，因为模块的 per-CPU 区域由模块加载器
 *     动态分配，无需链接器静态布局
 */
#ifdef CONFIG_SMP

#ifdef MODULE
#define PER_CPU_SHARED_ALIGNED_SECTION ""
#define PER_CPU_ALIGNED_SECTION ""
#else
#define PER_CPU_SHARED_ALIGNED_SECTION "..shared_aligned"
#define PER_CPU_ALIGNED_SECTION "..shared_aligned"
#endif

#else

#define PER_CPU_SHARED_ALIGNED_SECTION ""
#define PER_CPU_ALIGNED_SECTION "..shared_aligned"

#endif

/*
 * per-CPU 变量声明/定义的底层属性宏。
 *
 * __PCPU_ATTRS(sec)：为 per-CPU 变量附加必要的编译器属性：
 *   - __percpu：sparse 静态分析标记，标识该指针指向 per-CPU 地址空间，
 *     防止将 per-CPU 指针与普通指针混用（会产生 sparse 警告）。
 *   - __attribute__((section(...)))：将变量放入 per-CPU 专用 ELF 节
 *     （PER_CPU_BASE_SECTION + sec），链接器据此构建 per-CPU 模板区。
 *   - PER_CPU_ATTRIBUTES：体系结构相关的额外属性（如 alpha 的 __weak）。
 *
 * NOTE! DECLARE 和 DEFINE 使用的 sec 必须完全相同，否则链接器会因找不到
 * 匹配节而报错（编译器对不同 section 的变量生成不同的寻址代码）。
 *
 * __PCPU_DUMMY_ATTRS：用于辅助变量（仅做编译期检查，不占运行时空间）：
 *   - __section(".discard")：放入 .discard 节，链接时被丢弃。
 *   - __attribute__((unused))：抑制"变量未使用"编译警告。
 */
#define __PCPU_ATTRS(sec)						\
	__percpu __attribute__((section(PER_CPU_BASE_SECTION sec)))	\
	PER_CPU_ATTRIBUTES

#define __PCPU_DUMMY_ATTRS						\
	__section(".discard") __attribute__((unused))

/*
 * alpha 架构的模块需要将 per-CPU 变量声明为 __weak，迫使编译器生成基于
 * GOT（Global Offset Table）的外部引用。原因是 alpha 模块加载后，per-CPU
 * 节可能位于普通寻址范围之外，必须通过 GOT 间接寻址才能访问。
 *
 * 这带来两条额外限制：
 *   1. 符号名必须全局唯一，即使是 static 变量也不例外。
 *   2. static per-CPU 变量不能定义在函数内部。
 *
 * 需要此行为的架构应在 Kconfig 中设置 CONFIG_ARCH_MODULE_NEEDS_WEAK_PER_CPU。
 * CONFIG_DEBUG_FORCE_WEAK_PER_CPU 可在所有架构上强制启用弱定义，
 * 用于在通用代码中测试上述两条限制是否被遵守。
 */
#if (defined(CONFIG_ARCH_MODULE_NEEDS_WEAK_PER_CPU) && defined(MODULE)) || \
	defined(CONFIG_DEBUG_FORCE_WEAK_PER_CPU)
/*
 * 弱符号模式下的 per-CPU 声明/定义（alpha 模块专用路径）。
 *
 * __pcpu_scope_##name：作用域检查辅助变量（放入 .discard，链接时丢弃）。
 *   当 DEFINE_PER_CPU 前有 static 修饰时，该辅助变量也是 static；
 *   若再出现 DECLARE_PER_CPU（extern），链接器因属性冲突报错，
 *   从而在编译期强制保证"同一变量不能既 define 又 declare"。
 *
 * __pcpu_unique_##name：符号唯一性检查辅助变量。
 *   同时出现 extern 声明和普通（强）定义：若两个编译单元定义了同名
 *   per-CPU 变量，__weak 符号会静默合并，而 __pcpu_unique_* 的强符号冲突
 *   会让链接器报重复定义错误，提前暴露隐藏的命名冲突。
 */
#define DECLARE_PER_CPU_SECTION(type, name, sec)			\
	extern __PCPU_DUMMY_ATTRS char __pcpu_scope_##name;		\
	extern __PCPU_ATTRS(sec) __typeof__(type) name

#define DEFINE_PER_CPU_SECTION(type, name, sec)				\
	__PCPU_DUMMY_ATTRS char __pcpu_scope_##name;			\
	extern __PCPU_DUMMY_ATTRS char __pcpu_unique_##name;		\
	__PCPU_DUMMY_ATTRS char __pcpu_unique_##name;			\
	extern __PCPU_ATTRS(sec) __typeof__(type) name;			\
	__PCPU_ATTRS(sec) __weak __typeof__(type) name
#else
/*
 * 普通模式下的 per-CPU 声明/定义（绝大多数架构走这条路径）。
 *
 * DECLARE_PER_CPU_SECTION：在头文件中声明 per-CPU 变量（extern），
 *   告知编译器变量存在于某个 per-CPU 节，不分配存储空间。
 *
 * DEFINE_PER_CPU_SECTION：在 .c 文件中定义 per-CPU 变量，
 *   通过 __PCPU_ATTRS(sec) 将其放入指定的 per-CPU ELF 节，
 *   链接器将其纳入 per-CPU 模板区，启动时为每个 CPU 复制一份。
 */
#define DECLARE_PER_CPU_SECTION(type, name, sec)			\
	extern __PCPU_ATTRS(sec) __typeof__(type) name

#define DEFINE_PER_CPU_SECTION(type, name, sec)				\
	__PCPU_ATTRS(sec) __typeof__(type) name
#endif

/*
 * 普通 per-CPU 变量的声明/定义入口（section 后缀为空，即放入默认的
 * .data..percpu 节）。这是内核代码中最常用的两个宏：
 *
 *   DECLARE_PER_CPU(int, my_var);   // 头文件中：extern 声明，不分配空间
 *   DEFINE_PER_CPU(int, my_var);    // .c 文件中：实际定义，进入 per-CPU 模板区
 */
#define DECLARE_PER_CPU(type, name)					\
	DECLARE_PER_CPU_SECTION(type, name, "")

#define DEFINE_PER_CPU(type, name)					\
	DEFINE_PER_CPU_SECTION(type, name, "")

/*
 * Declaration/definition used for per-CPU variables that are frequently
 * accessed and should be in a single cacheline.
 *
 * For use only by architecture and core code.  Only use scalar or pointer
 * types to maximize density.
 */
#define DECLARE_PER_CPU_CACHE_HOT(type, name)				\
	DECLARE_PER_CPU_SECTION(type, name, "..hot.." #name)

#define DEFINE_PER_CPU_CACHE_HOT(type, name)				\
	DEFINE_PER_CPU_SECTION(type, name, "..hot.." #name)

/*
 * Declaration/definition used for per-CPU variables that must be cacheline
 * aligned under SMP conditions so that, whilst a particular instance of the
 * data corresponds to a particular CPU, inefficiencies due to direct access by
 * other CPUs are reduced by preventing the data from unnecessarily spanning
 * cachelines.
 *
 * An example of this would be statistical data, where each CPU's set of data
 * is updated by that CPU alone, but the data from across all CPUs is collated
 * by a CPU processing a read from a proc file.
 */
#define DECLARE_PER_CPU_SHARED_ALIGNED(type, name)			\
	DECLARE_PER_CPU_SECTION(type, name, PER_CPU_SHARED_ALIGNED_SECTION) \
	____cacheline_aligned_in_smp

#define DEFINE_PER_CPU_SHARED_ALIGNED(type, name)			\
	DEFINE_PER_CPU_SECTION(type, name, PER_CPU_SHARED_ALIGNED_SECTION) \
	____cacheline_aligned_in_smp

#define DECLARE_PER_CPU_ALIGNED(type, name)				\
	DECLARE_PER_CPU_SECTION(type, name, PER_CPU_ALIGNED_SECTION)	\
	____cacheline_aligned

#define DEFINE_PER_CPU_ALIGNED(type, name)				\
	DEFINE_PER_CPU_SECTION(type, name, PER_CPU_ALIGNED_SECTION)	\
	____cacheline_aligned

/*
 * Declaration/definition used for per-CPU variables that must be page aligned.
 */
#define DECLARE_PER_CPU_PAGE_ALIGNED(type, name)			\
	DECLARE_PER_CPU_SECTION(type, name, "..page_aligned")		\
	__aligned(PAGE_SIZE)

#define DEFINE_PER_CPU_PAGE_ALIGNED(type, name)				\
	DEFINE_PER_CPU_SECTION(type, name, "..page_aligned")		\
	__aligned(PAGE_SIZE)

/*
 * Declaration/definition used for per-CPU variables that must be read mostly.
 */
#define DECLARE_PER_CPU_READ_MOSTLY(type, name)			\
	DECLARE_PER_CPU_SECTION(type, name, "..read_mostly")

#define DEFINE_PER_CPU_READ_MOSTLY(type, name)				\
	DEFINE_PER_CPU_SECTION(type, name, "..read_mostly")

/*
 * Declaration/definition used for per-CPU variables that should be accessed
 * as decrypted when memory encryption is enabled in the guest.
 */
#ifdef CONFIG_AMD_MEM_ENCRYPT
#define DECLARE_PER_CPU_DECRYPTED(type, name)				\
	DECLARE_PER_CPU_SECTION(type, name, "..decrypted")

#define DEFINE_PER_CPU_DECRYPTED(type, name)				\
	DEFINE_PER_CPU_SECTION(type, name, "..decrypted")
#else
#define DEFINE_PER_CPU_DECRYPTED(type, name)	DEFINE_PER_CPU(type, name)
#endif

/*
 * Intermodule exports for per-CPU variables.  sparse forgets about
 * address space across EXPORT_SYMBOL(), change EXPORT_SYMBOL() to
 * noop if __CHECKER__.
 */
#ifndef __CHECKER__
#define EXPORT_PER_CPU_SYMBOL(var) EXPORT_SYMBOL(var)
#define EXPORT_PER_CPU_SYMBOL_GPL(var) EXPORT_SYMBOL_GPL(var)
#else
#define EXPORT_PER_CPU_SYMBOL(var)
#define EXPORT_PER_CPU_SYMBOL_GPL(var)
#endif

/*
 * Accessors and operations.
 */
#ifndef __ASSEMBLY__

/*
 * __verify_pcpu_ptr() - 编译期校验 ptr 是合法的 per-CPU 指针（__percpu 地址空间）
 *
 * 原理：将 ptr 强制转换为 "const void __percpu *"。
 *   - 若 ptr 确实带有 __percpu 标记，sparse 认为地址空间匹配，无警告。
 *   - 若 ptr 是普通指针（缺少 __percpu），sparse 报告地址空间不匹配。
 *   - (void)__vpp_verify 防止"变量未使用"警告；do { } while (0) 保证
 *     宏在任何语句上下文中展开安全。
 *
 * "(ptr) + 0"：将数组类型退化为指针类型（C 标准：对数组取地址再加 0
 * 得到指向首元素的指针），确保类型转换对数组和指针都能正确工作。
 *
 * 所有 per-CPU 访问器在实际访问前都调用此宏一次，架构实现无需重复校验。
 */
#define __verify_pcpu_ptr(ptr)						\
do {									\
	const void __percpu *__vpp_verify = (typeof((ptr) + 0))NULL;	\
	(void)__vpp_verify;						\
} while (0)

/*
 * PERCPU_PTR(__p) - 将 per-CPU 指针转换为可直接解引用的内核普通指针
 *
 * per-CPU 变量在链接时存储的是相对于 per-CPU 模板区起始地址的"偏移量"，
 * 而非真实的内存地址（sparse 用 __percpu 地址空间区分它们）。
 * 在对其解引用之前，必须先去掉 __percpu 标记并转换为 __kernel 地址空间：
 *
 *   TYPEOF_UNQUAL(*(__p))：得到 __p 所指向元素的类型（去掉 const/volatile），
 *   __force __kernel *：告知 sparse 强制切换到内核地址空间（绕过地址空间检查），
 *   ((__force unsigned long)(__p))：将 per-CPU 地址当作整数传递，
 *     由调用方（SHIFT_PERCPU_PTR）加上 CPU 偏移后得到真实地址。
 */
#define PERCPU_PTR(__p)							\
	((TYPEOF_UNQUAL(*(__p)) __force __kernel *)((__force unsigned long)(__p)))

#ifdef CONFIG_SMP

/*
 * SMP 路径：per-CPU 变量真正做到了每 CPU 一份独立副本。
 *
 * SHIFT_PERCPU_PTR(__p, __offset)：
 *   将 per-CPU 模板区中的地址 __p 加上 CPU 专属偏移 __offset，
 *   得到该 CPU 数据区中对应变量的真实地址。
 *   RELOC_HIDE() 通过内联汇编屏蔽编译器对指针运算的别名分析，
 *   防止编译器错误地将偏移后的指针与原始指针视为同一对象。
 *
 * per_cpu_offset(cpu)：返回 __per_cpu_offset[cpu]，即 CPU cpu
 *   的 per-CPU 数据区相对于模板区的字节偏移。
 */
#define SHIFT_PERCPU_PTR(__p, __offset)					\
	RELOC_HIDE(PERCPU_PTR(__p), (__offset))

/*
 * per_cpu_ptr(ptr, cpu) - 获取指定 CPU 上 per-CPU 指针 ptr 对应的真实地址
 * @ptr: per-CPU 变量的模板区地址（__percpu 指针）
 * @cpu: 目标 CPU 编号（整数，范围 [0, nr_cpu_ids)）
 *
 * 展开过程：
 *   1. __verify_pcpu_ptr(ptr)          → 编译期校验 ptr 是 __percpu 指针
 *   2. per_cpu_offset(cpu)             → 查 __per_cpu_offset[cpu] 得到偏移量
 *   3. SHIFT_PERCPU_PTR(ptr, offset)   → ptr 的整数值 + offset = 真实地址
 *   4. 结果转换回正确的指针类型并返回
 *
 * 示例（4 核机器，my_var 在模板区偏移 0x100）：
 *   __per_cpu_offset = {0x1000, 0x2000, 0x3000, 0x4000}
 *   per_cpu_ptr(&my_var, 2) = 0x100 + 0x3000 = 0x3100（CPU2 的副本地址）
 */
#define per_cpu_ptr(ptr, cpu)						\
({									\
	__verify_pcpu_ptr(ptr);						\
	SHIFT_PERCPU_PTR((ptr), per_cpu_offset((cpu)));			\
})

/*
 * raw_cpu_ptr(ptr) - 获取当前 CPU 上 per-CPU 指针对应的真实地址（不检查抢占）
 *
 * 使用架构提供的 arch_raw_cpu_ptr()，通常直接读取当前 CPU 的段基址寄存器
 * 或 tp 寄存器，比 per_cpu_ptr(ptr, smp_processor_id()) 更高效。
 * 调用者须自行保证在访问期间不会被调度到其他 CPU（关中断或关抢占）。
 */
#define raw_cpu_ptr(ptr)						\
({									\
	__verify_pcpu_ptr(ptr);						\
	arch_raw_cpu_ptr(ptr);						\
})

/*
 * this_cpu_ptr(ptr) - 获取当前 CPU 上 per-CPU 指针对应的真实地址
 *
 * CONFIG_DEBUG_PREEMPT 开启时使用 SHIFT_PERCPU_PTR + my_cpu_offset，
 * 并在调试版本中检查是否在抢占禁用状态下调用（保证访问期间不换 CPU）；
 * 否则直接等价于 raw_cpu_ptr()，性能更优。
 */
#ifdef CONFIG_DEBUG_PREEMPT
#define this_cpu_ptr(ptr)						\
({									\
	__verify_pcpu_ptr(ptr);						\
	SHIFT_PERCPU_PTR(ptr, my_cpu_offset);				\
})
#else
#define this_cpu_ptr(ptr) raw_cpu_ptr(ptr)
#endif

#else	/* CONFIG_SMP */

/*
 * 单处理器（UP）路径：系统只有一个 CPU，per-CPU 变量退化为普通全局变量。
 *
 * per_cpu_ptr(ptr, cpu)：
 *   - (void)(cpu)：忽略 cpu 参数，消除"未使用变量"编译警告。
 *   - __verify_pcpu_ptr(ptr)：仍保留 sparse 地址空间检查，确保类型安全。
 *   - PERCPU_PTR(ptr)：去掉 __percpu 标记，返回可直接使用的内核指针；
 *     UP 下无需加偏移（只有一份数据，偏移恒为 0）。
 */
#define per_cpu_ptr(ptr, cpu)						\
({									\
	(void)(cpu);							\
	__verify_pcpu_ptr(ptr);						\
	PERCPU_PTR(ptr);						\
})

#define raw_cpu_ptr(ptr)	per_cpu_ptr(ptr, 0)
#define this_cpu_ptr(ptr)	raw_cpu_ptr(ptr)

#endif	/* CONFIG_SMP */

/*
 * per_cpu(var, cpu) - 获取指定 CPU 上 per-CPU 变量 var 的左值引用
 * @var: per-CPU 变量名（不是指针，是变量本身）
 * @cpu: 目标 CPU 编号（整数）
 *
 * 展开为 *per_cpu_ptr(&(var), cpu)：
 *   1. &(var)  → 取 var 在 per-CPU 模板区中的地址（__percpu 指针）
 *   2. per_cpu_ptr(..., cpu) → 加上 cpu 的偏移，得到该 CPU 副本的真实地址
 *   3. *(...) → 解引用，得到可读写的左值
 *
 * 返回左值，因此可以直接赋值：
 *   per_cpu(my_counter, 1) = 42;   // 写 CPU1 的副本
 *   x = per_cpu(my_counter, 0);    // 读 CPU0 的副本
 *
 * 注意：不禁用抢占，调用者须自行保证访问期间不被迁移到其他 CPU，
 * 或者接受读到"错误 CPU"副本的风险（适用于统计场景）。
 * 若需要安全访问当前 CPU 的副本，应使用 get_cpu_var() / this_cpu_ptr()。
 */
#define per_cpu(var, cpu)	(*per_cpu_ptr(&(var), cpu))

/*
 * get_cpu_var(var) - 禁用抢占并返回当前 CPU 上 per-CPU 变量 var 的左值
 * @var: per-CPU 变量名（简单标识符，不能是表达式）
 *
 * 使用模式：
 *   get_cpu_var(my_var)++;         // 原子地递增当前 CPU 的副本
 *   put_cpu_var(my_var);           // 必须配对调用以重新启用抢占
 *
 * 内部展开：
 *   1. preempt_disable()   → 禁止抢占，确保访问期间不会被调度到其他 CPU
 *   2. this_cpu_ptr(&var)  → 获取当前 CPU 副本的地址
 *   3. *(...) → 解引用得到左值（整个表达式作为语句表达式返回）
 *
 * "@var must be a simple identifier"：宏用 &var 取地址，若 var 不是
 * 标识符（如表达式），编译器会报语法错误，这是故意的保护。
 */
#define get_cpu_var(var)						\
(*({									\
	preempt_disable();						\
	this_cpu_ptr(&var);						\
}))

/*
 * put_cpu_var(var) - 重新启用抢占，与 get_cpu_var() 配对使用
 * @var: 与 get_cpu_var() 相同的 per-CPU 变量名
 *
 * (void)&(var)：用取地址而非直接引用 var，原因是 sparse 会将 "(void)(var)"
 * 解释为对 per-CPU 变量的直接解引用（缺少偏移计算），产生误报警告；
 * 改用 &(var) 则只是取地址，sparse 不会误判为解引用。
 */
#define put_cpu_var(var)						\
do {									\
	(void)&(var);							\
	preempt_enable();						\
} while (0)

/*
 * get_cpu_ptr(var) - 禁用抢占并返回当前 CPU 上 per-CPU 指针变量的值
 * @var: per-CPU 指针变量（已经是指针，与 get_cpu_var 接受变量名不同）
 *
 * 用于 per-CPU 指针变量（如 DEFINE_PER_CPU(struct foo *, my_ptr)）：
 *   struct foo *p = get_cpu_ptr(my_ptr);
 *   // 使用 p 操作当前 CPU 的数据结构
 *   put_cpu_ptr(my_ptr);
 *
 * 与 get_cpu_var 的区别：get_cpu_var 对变量解引用返回左值；
 * get_cpu_ptr 对指针变量本身取当前 CPU 的地址，返回指针值。
 */
#define get_cpu_ptr(var)						\
({									\
	preempt_disable();						\
	this_cpu_ptr(var);						\
})

/*
 * put_cpu_ptr(var) - 重新启用抢占，与 get_cpu_ptr() 配对使用
 * (void)(var)：此处 var 是普通指针（非 per-CPU 变量），sparse 不会误报。
 */
#define put_cpu_ptr(var)						\
do {									\
	(void)(var);							\
	preempt_enable();						\
} while (0)

/*
 * 按标量大小分发的辅助宏族：将一次 per-CPU 操作分派到对应大小的具体实现。
 *
 * 内核的 per-CPU 原子操作针对 1/2/4/8 字节各有独立实现（如 this_cpu_add_1、
 * this_cpu_add_2 等），以匹配不同体系结构的原子指令宽度。这四个宏根据
 * sizeof(variable) 在编译期选择正确的实现，若大小不在支持范围内则调用
 * __bad_size_call_parameter()（未定义函数）触发链接错误，提前暴露问题。
 */

/* __bad_size_call_parameter：占位符，故意不提供定义，用于触发链接期错误 */
extern void __bad_size_call_parameter(void);

/*
 * __this_cpu_preempt_check(op)：调试辅助，在 CONFIG_DEBUG_PREEMPT 开启时
 * 检查调用 __this_cpu_* 操作时抢占是否已被禁用；非调试版本为空内联函数。
 */
#ifdef CONFIG_DEBUG_PREEMPT
extern void __this_cpu_preempt_check(const char *op);
#else
static __always_inline void __this_cpu_preempt_check(const char *op) { }
#endif

/*
 * __pcpu_size_call_return(stem, variable)
 *   按 variable 的字节大小选择 stem##1/2/4/8，调用并返回结果（有返回值版本）。
 *   例：__pcpu_size_call_return(raw_cpu_read_, my_u32)
 *       → switch(4) → raw_cpu_read_4(my_u32)
 */
#define __pcpu_size_call_return(stem, variable)				\
({									\
	TYPEOF_UNQUAL(variable) pscr_ret__;				\
	__verify_pcpu_ptr(&(variable));					\
	switch(sizeof(variable)) {					\
	case 1: pscr_ret__ = stem##1(variable); break;			\
	case 2: pscr_ret__ = stem##2(variable); break;			\
	case 4: pscr_ret__ = stem##4(variable); break;			\
	case 8: pscr_ret__ = stem##8(variable); break;			\
	default:							\
		__bad_size_call_parameter(); break;			\
	}								\
	pscr_ret__;							\
})

/*
 * __pcpu_size_call_return2(stem, variable, ...)
 *   同上，但操作需要额外参数（如 xchg/cmpxchg 的新值/期望值），有返回值。
 *   例：__pcpu_size_call_return2(raw_cpu_xchg_, my_u64, new_val)
 *       → raw_cpu_xchg_8(my_u64, new_val)
 */
#define __pcpu_size_call_return2(stem, variable, ...)			\
({									\
	TYPEOF_UNQUAL(variable) pscr2_ret__;				\
	__verify_pcpu_ptr(&(variable));					\
	switch(sizeof(variable)) {					\
	case 1: pscr2_ret__ = stem##1(variable, __VA_ARGS__); break;	\
	case 2: pscr2_ret__ = stem##2(variable, __VA_ARGS__); break;	\
	case 4: pscr2_ret__ = stem##4(variable, __VA_ARGS__); break;	\
	case 8: pscr2_ret__ = stem##8(variable, __VA_ARGS__); break;	\
	default:							\
		__bad_size_call_parameter(); break;			\
	}								\
	pscr2_ret__;							\
})

/*
 * __pcpu_size_call_return2bool(stem, variable, ...)
 *   同 __pcpu_size_call_return2，但返回类型固定为 bool。
 *   用于 try_cmpxchg 类操作（成功返回 true，失败返回 false）。
 */
#define __pcpu_size_call_return2bool(stem, variable, ...)		\
({									\
	bool pscr2_ret__;						\
	__verify_pcpu_ptr(&(variable));					\
	switch(sizeof(variable)) {					\
	case 1: pscr2_ret__ = stem##1(variable, __VA_ARGS__); break;	\
	case 2: pscr2_ret__ = stem##2(variable, __VA_ARGS__); break;	\
	case 4: pscr2_ret__ = stem##4(variable, __VA_ARGS__); break;	\
	case 8: pscr2_ret__ = stem##8(variable, __VA_ARGS__); break;	\
	default:							\
		__bad_size_call_parameter(); break;			\
	}								\
	pscr2_ret__;							\
})

/*
 * __pcpu_size_call(stem, variable, ...)
 *   同上，但操作没有返回值（如 write/add/and/or），展开为语句而非表达式。
 */
#define __pcpu_size_call(stem, variable, ...)				\
do {									\
	__verify_pcpu_ptr(&(variable));					\
	switch(sizeof(variable)) {					\
		case 1: stem##1(variable, __VA_ARGS__);break;		\
		case 2: stem##2(variable, __VA_ARGS__);break;		\
		case 4: stem##4(variable, __VA_ARGS__);break;		\
		case 8: stem##8(variable, __VA_ARGS__);break;		\
		default: 						\
			__bad_size_call_parameter();break;		\
	}								\
} while (0)

/*
 * this_cpu operations (C) 2008-2013 Christoph Lameter <cl@gentwo.org>
 *
 * Optimized manipulation for memory allocated through the per cpu
 * allocator or for addresses of per cpu variables.
 *
 * These operation guarantee exclusivity of access for other operations
 * on the *same* processor. The assumption is that per cpu data is only
 * accessed by a single processor instance (the current one).
 *
 * The arch code can provide optimized implementation by defining macros
 * for certain scalar sizes. F.e. provide this_cpu_add_2() to provide per
 * cpu atomic operations for 2 byte sized RMW actions. If arch code does
 * not provide operations for a scalar size then the fallback in the
 * generic code will be used.
 *
 * cmpxchg_double replaces two adjacent scalars at once.  The first two
 * parameters are per cpu variables which have to be of the same size.  A
 * truth value is returned to indicate success or failure (since a double
 * register result is difficult to handle).  There is very limited hardware
 * support for these operations, so only certain sizes may work.
 */

/*
 * ── raw_cpu_* 操作族 ──────────────────────────────────────────────────────
 * 不做任何抢占检查的 per-CPU 原子操作，直接作用于当前 CPU 的副本。
 *
 * 使用前提：调用者必须通过其他方式保证 CPU 稳定性，例如：
 *   - 已禁用中断（local_irq_disable / irq_save）
 *   - 已禁用抢占（preempt_disable）
 *   - 在 NMI/hardirq 上下文中（天然不可被抢占）
 *
 * 若缺乏上述保护，RMW（读-改-写）操作可能发生如下竞态：
 *   1. 读取 CPU0 的副本
 *   2. 被调度到 CPU1（现在访问的是 CPU1 的副本！）
 *   3. 写入 CPU1 的副本 → 数据错乱
 * 因此，除非明确知道上下文已受保护，否则优先使用 this_cpu_* 族。
 *
 * @pcp:  per-CPU 变量名（非指针）
 * @val:  操作数
 * @nval: 新值（xchg）
 * @oval: 期望旧值（cmpxchg），@ovalp: 指向期望旧值的指针（try_cmpxchg）
 */
#define raw_cpu_read(pcp)		__pcpu_size_call_return(raw_cpu_read_, pcp)
#define raw_cpu_write(pcp, val)		__pcpu_size_call(raw_cpu_write_, pcp, val)
#define raw_cpu_add(pcp, val)		__pcpu_size_call(raw_cpu_add_, pcp, val)
#define raw_cpu_and(pcp, val)		__pcpu_size_call(raw_cpu_and_, pcp, val)
#define raw_cpu_or(pcp, val)		__pcpu_size_call(raw_cpu_or_, pcp, val)
#define raw_cpu_add_return(pcp, val)	__pcpu_size_call_return2(raw_cpu_add_return_, pcp, val)
#define raw_cpu_xchg(pcp, nval)		__pcpu_size_call_return2(raw_cpu_xchg_, pcp, nval)
/* 若当前值 == oval，写入 nval 并返回 oval；否则返回当前值 */
#define raw_cpu_cmpxchg(pcp, oval, nval) \
	__pcpu_size_call_return2(raw_cpu_cmpxchg_, pcp, oval, nval)
/* 若当前值 == *ovalp，写入 nval 返回 true；否则将当前值写入 *ovalp 返回 false */
#define raw_cpu_try_cmpxchg(pcp, ovalp, nval) \
	__pcpu_size_call_return2bool(raw_cpu_try_cmpxchg_, pcp, ovalp, nval)
/* 以下派生操作均转发到 raw_cpu_add，符号扩展由强制类型转换处理 */
#define raw_cpu_sub(pcp, val)		raw_cpu_add(pcp, -(val))
#define raw_cpu_inc(pcp)		raw_cpu_add(pcp, 1)
#define raw_cpu_dec(pcp)		raw_cpu_sub(pcp, 1)
#define raw_cpu_sub_return(pcp, val)	raw_cpu_add_return(pcp, -(typeof(pcp))(val))
#define raw_cpu_inc_return(pcp)		raw_cpu_add_return(pcp, 1)
#define raw_cpu_dec_return(pcp)		raw_cpu_add_return(pcp, -1)

/*
 * ── __this_cpu_* 操作族 ───────────────────────────────────────────────────
 * 在 raw_cpu_* 基础上增加抢占状态断言（CONFIG_DEBUG_PREEMPT 下）。
 * 语义与 raw_cpu_* 完全相同，但会在调试版本中通过 __this_cpu_preempt_check()
 * 验证调用时抢占已被禁用，帮助定位忘记禁抢占的 bug。
 * 生产版本（非 DEBUG_PREEMPT）检查函数为空内联，无性能开销。
 */
#define __this_cpu_read(pcp)						\
({									\
	__this_cpu_preempt_check("read");				\
	raw_cpu_read(pcp);						\
})

#define __this_cpu_write(pcp, val)					\
({									\
	__this_cpu_preempt_check("write");				\
	raw_cpu_write(pcp, val);					\
})

#define __this_cpu_add(pcp, val)					\
({									\
	__this_cpu_preempt_check("add");				\
	raw_cpu_add(pcp, val);						\
})

#define __this_cpu_and(pcp, val)					\
({									\
	__this_cpu_preempt_check("and");				\
	raw_cpu_and(pcp, val);						\
})

#define __this_cpu_or(pcp, val)						\
({									\
	__this_cpu_preempt_check("or");					\
	raw_cpu_or(pcp, val);						\
})

#define __this_cpu_add_return(pcp, val)					\
({									\
	__this_cpu_preempt_check("add_return");				\
	raw_cpu_add_return(pcp, val);					\
})

#define __this_cpu_xchg(pcp, nval)					\
({									\
	__this_cpu_preempt_check("xchg");				\
	raw_cpu_xchg(pcp, nval);					\
})

#define __this_cpu_cmpxchg(pcp, oval, nval)				\
({									\
	__this_cpu_preempt_check("cmpxchg");				\
	raw_cpu_cmpxchg(pcp, oval, nval);				\
})

#define __this_cpu_try_cmpxchg(pcp, ovalp, nval)			\
({									\
	__this_cpu_preempt_check("try_cmpxchg");			\
	raw_cpu_try_cmpxchg(pcp, ovalp, nval);				\
})

#define __this_cpu_sub(pcp, val)	__this_cpu_add(pcp, -(typeof(pcp))(val))
#define __this_cpu_inc(pcp)		__this_cpu_add(pcp, 1)
#define __this_cpu_dec(pcp)		__this_cpu_sub(pcp, 1)
#define __this_cpu_sub_return(pcp, val)	__this_cpu_add_return(pcp, -(typeof(pcp))(val))
#define __this_cpu_inc_return(pcp)	__this_cpu_add_return(pcp, 1)
#define __this_cpu_dec_return(pcp)	__this_cpu_add_return(pcp, -1)

/*
 * ── this_cpu_* 操作族 ────────────────────────────────────────────────────
 * 隐含抢占/中断保护的 per-CPU 操作，是最常用的公开接口。
 * 架构在实现 this_cpu_*_N() 时通常会自动关中断或使用无需关中断的原子指令
 * （如 x86 的 LOCK 前缀或段寄存器隐式保护），调用者无需手动禁用抢占。
 * 适用于绝大多数普通内核代码场景。
 */
#define this_cpu_read(pcp)		__pcpu_size_call_return(this_cpu_read_, pcp)
#define this_cpu_write(pcp, val)	__pcpu_size_call(this_cpu_write_, pcp, val)
#define this_cpu_add(pcp, val)		__pcpu_size_call(this_cpu_add_, pcp, val)
#define this_cpu_and(pcp, val)		__pcpu_size_call(this_cpu_and_, pcp, val)
#define this_cpu_or(pcp, val)		__pcpu_size_call(this_cpu_or_, pcp, val)
#define this_cpu_add_return(pcp, val)	__pcpu_size_call_return2(this_cpu_add_return_, pcp, val)
#define this_cpu_xchg(pcp, nval)	__pcpu_size_call_return2(this_cpu_xchg_, pcp, nval)
#define this_cpu_cmpxchg(pcp, oval, nval) \
	__pcpu_size_call_return2(this_cpu_cmpxchg_, pcp, oval, nval)
#define this_cpu_try_cmpxchg(pcp, ovalp, nval) \
	__pcpu_size_call_return2bool(this_cpu_try_cmpxchg_, pcp, ovalp, nval)
#define this_cpu_sub(pcp, val)		this_cpu_add(pcp, -(typeof(pcp))(val))
#define this_cpu_inc(pcp)		this_cpu_add(pcp, 1)
#define this_cpu_dec(pcp)		this_cpu_sub(pcp, 1)
#define this_cpu_sub_return(pcp, val)	this_cpu_add_return(pcp, -(typeof(pcp))(val))
#define this_cpu_inc_return(pcp)	this_cpu_add_return(pcp, 1)
#define this_cpu_dec_return(pcp)	this_cpu_add_return(pcp, -1)

#endif /* __ASSEMBLY__ */
#endif /* _LINUX_PERCPU_DEFS_H */
