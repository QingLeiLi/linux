/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MODULELOADER_H
#define _LINUX_MODULELOADER_H

/**
 * 文件目的 (File Purpose):
 * ============================================================================
 * 定义内核模块加载器的架构相关接口。Linux 支持动态加载内核模块(LKM),
 * 但不同CPU架构(x86、ARM、RISC-V等)处理可执行文件格式的方式不同。
 * 这个头文件定义了架构必须实现的钩子函数。
 *
 * Defines architecture-specific interfaces for kernel module loader. Linux
 * supports dynamically loadable kernel modules (LKM), but different CPU
 * architectures (x86, ARM, RISC-V, etc.) handle executable file formats
 * differently. This header defines hook functions that architectures must
 * implement.
 *
 * 背景知识 (Background):
 * ============================================================================
 * 1. 内核模块是编译后的 ELF (Executable and Linkable Format) 文件
 * 2. 加载模块时需要:
 *    - 解析 ELF 头和段(sections)
 *    - 分配内核内存
 *    - 应用重定位(relocations) - 修正代码中的地址引用
 *    - 解析符号表并链接到内核符号
 * 3. 不同架构的指令集、调用约定、地址模式不同，因此重定位逻辑
 *    必须由架构代码实现
 *
 * 1. Kernel modules are compiled ELF (Executable and Linkable Format) files
 * 2. When loading a module, need to:
 *    - Parse ELF headers and sections
 *    - Allocate kernel memory
 *    - Apply relocations - fix address references in code
 *    - Parse symbol table and link to kernel symbols
 * 3. Different architectures have different instruction sets, calling
 *    conventions, and address modes, so relocation logic must be
 *    implemented by architecture code
 *
 * 设计原则 (Design Principles):
 * ============================================================================
 * - 通用代码在 kernel/module.c (ELF解析、内存管理)
 * - 架构特定代码在 arch/*/kernel/module.c (重定位、最终处理)
 * - 使用弱符号(weak symbols)提供默认实现，架构可以覆盖
 * - Generic code in kernel/module.c (ELF parsing, memory management)
 * - Architecture-specific code in arch/*/kernel/module.c (relocation, finalization)
 * - Use weak symbols to provide default implementations, architectures can override
 */

/* The stuff needed for archs to support modules. */
/* 架构支持模块所需的功能 */

#include <linux/module.h>
#include <linux/elf.h>

/* These may be implemented by architectures that need to hook into the
 * module loader code.  Architectures that don't need to do anything special
 * can just rely on the 'weak' default hooks defined in kernel/module.c.
 * Note, however, that at least one of apply_relocate or apply_relocate_add
 * must be implemented by each architecture.
 */
/* 这些函数可以由需要钩入模块加载代码的架构实现。不需要特殊处理的架构
 * 可以依赖 kernel/module.c 中定义的'弱'默认钩子。
 * 但是注意，每个架构必须至少实现 apply_relocate 或 apply_relocate_add
 * 其中之一。
 */

/**
 * module_elf_check_arch - 检查 ELF 文件是否适用于当前架构
 * Check if ELF file is suitable for current architecture
 *
 * @hdr: ELF 头指针 - Pointer to ELF header
 *
 * 返回值 (Return):
 * true  - ELF 文件与当前架构兼容
 * false - 不兼容，拒绝加载
 *
 * 检查内容 (What to Check):
 * - e_machine 字段(机器类型): x86_64=62, ARM64=183, RISC-V=243
 * - e_ident[EI_CLASS]: 32位(ELFCLASS32) 或 64位(ELFCLASS64)
 * - 字节序: 大端或小端
 * - ELF 版本
 *
 * - e_machine field (machine type): x86_64=62, ARM64=183, RISC-V=243
 * - e_ident[EI_CLASS]: 32-bit (ELFCLASS32) or 64-bit (ELFCLASS64)
 * - Endianness: big-endian or little-endian
 * - ELF version
 *
 * 为什么需要 (Why Needed):
 * 防止加载错误架构的模块(如在 x86 系统上加载 ARM 模块)导致系统崩溃
 * Prevents loading modules for wrong architecture (e.g., ARM module on x86
 * system) which would crash the system
 */
/* arch may override to do additional checking of ELF header architecture */
/* 架构可以覆盖此函数以对 ELF 头架构进行额外检查 */
bool module_elf_check_arch(Elf_Ehdr *hdr);

/**
 * module_frob_arch_sections - 调整架构特定的段
 * Adjust architecture-specific sections
 *
 * @hdr: ELF 头 - ELF header
 * @sechdrs: 段头数组 - Array of section headers
 * @secstrings: 段名字符串表 - Section name string table
 * @mod: 正在加载的模块对象 - Module object being loaded
 *
 * 返回值 (Return):
 * 0     - 成功 (Success)
 * 负值  - 错误码 (Error code)
 *
 * 使用场景 (Use Cases):
 * - 某些架构有特殊段需要预处理(如 ARM 的异常表)
 * - 调整段对齐方式
 * - 标记需要特殊处理的段
 * - 为架构特定数据分配额外空间
 *
 * - Some architectures have special sections needing preprocessing
 *   (e.g., ARM exception tables)
 * - Adjust section alignment
 * - Mark sections needing special handling
 * - Allocate extra space for architecture-specific data
 *
 * "frob" 的含义:
 * 黑客俚语,意为"调整"或"修改"某物使其正常工作
 * Hacker slang meaning "adjust" or "tweak" something to make it work
 */
/* Adjust arch-specific sections.  Return 0 on success.  */
/* 调整架构特定的段。成功返回0。*/
int module_frob_arch_sections(Elf_Ehdr *hdr,
			      Elf_Shdr *sechdrs,
			      char *secstrings,
			      struct module *mod);

/**
 * arch_mod_section_prepend - 计算段前需要的额外字节数
 * Calculate extra bytes needed before a section
 *
 * @mod: 模块对象 - Module object
 * @section: 段索引 - Section index
 *
 * 返回值 (Return):
 * 需要在段前添加的字节数(通常返回0)
 * Number of bytes to prepend before the section (usually returns 0)
 *
 * 使用场景 (Use Cases):
 * - 某些架构需要在代码段前放置跳转表或桩代码
 * - 为 PLT (Procedure Linkage Table) 保留空间
 * - 对齐要求
 *
 * - Some architectures need to place jump tables or stub code before
 *   code sections
 * - Reserve space for PLT (Procedure Linkage Table)
 * - Alignment requirements
 */
/* Additional bytes needed by arch in front of individual sections */
/* 架构在各个段前需要的额外字节 */
unsigned int arch_mod_section_prepend(struct module *mod, unsigned int section);

/**
 * module_init_section - 判断段是否是初始化段
 * Determine if section is an initialization section
 *
 * @name: 段名称 - Section name
 *
 * 返回值 (Return):
 * true  - 这是初始化段,仅在模块加载时使用
 * false - 不是初始化段
 *
 * 初始化段 (Init Sections):
 * - .init.text, .init.data - 初始化代码和数据
 * - 加载完成后可以释放这些内存,节省空间
 * - 类似用户空间程序的构造函数
 *
 * - .init.text, .init.data - Initialization code and data
 * - Can free this memory after loading completes, saving space
 * - Similar to constructors in userspace programs
 *
 * 典型段名 (Typical Section Names):
 * - .init.*  -> 返回 true
 * - .text    -> 返回 false
 */
/* Determines if the section name is an init section (that is only used during
 * module loading).
 */
/* 判断段名是否是初始化段(仅在模块加载期间使用) */
bool module_init_section(const char *name);

/**
 * module_exit_section - 判断段是否是退出段
 * Determine if section is an exit section
 *
 * @name: 段名称 - Section name
 *
 * 返回值 (Return):
 * true  - 这是退出段,仅在模块卸载时使用
 * false - 不是退出段
 *
 * 退出段 (Exit Sections):
 * - .exit.text, .exit.data - 清理代码和数据
 * - 仅在模块卸载时调用
 * - 如果模块编译时配置为不可卸载,这些段可能被完全丢弃
 *
 * - .exit.text, .exit.data - Cleanup code and data
 * - Only called when module is unloaded
 * - If module compiled as non-unloadable, these sections may be
 *   completely discarded
 *
 * 典型段名 (Typical Section Names):
 * - .exit.*  -> 返回 true
 * - .text    -> 返回 false
 */
/* Determines if the section name is an exit section (that is only used during
 * module unloading)
 */
/* 判断段名是否是退出段(仅在模块卸载期间使用) */
bool module_exit_section(const char *name);

/**
 * module_init_layout_section - 判断段在布局中是否被视为初始化段
 * Determine if section is considered init section in layout
 *
 * @sname: 段名称 - Section name
 *
 * 返回值 (Return):
 * true  - 在内存布局中作为初始化段处理
 * false - 作为核心段处理
 *
 * 与 module_init_section 的区别 (Difference from module_init_section):
 * 行为取决于 CONFIG_MODULE_UNLOAD 配置:
 * - 如果禁用模块卸载: exit段也被视为init段(反正不会卸载,可以一起释放)
 * - 如果启用模块卸载: exit段必须保留(卸载时需要)
 *
 * Behavior depends on CONFIG_MODULE_UNLOAD configuration:
 * - If module unloading disabled: exit sections also treated as init sections
 *   (won't unload anyway, can free together)
 * - If module unloading enabled: exit sections must be kept (needed for unload)
 *
 * 这影响内存分配策略和段的生命周期
 * This affects memory allocation strategy and section lifetime
 */
/* Describes whether within_module_init() will consider this an init section
 * or not. This behaviour changes with CONFIG_MODULE_UNLOAD.
 */
/* 描述 within_module_init() 是否会将其视为初始化段。
 * 这个行为随 CONFIG_MODULE_UNLOAD 改变。
 */
bool module_init_layout_section(const char *sname);

/**
 * apply_relocate - 应用 REL 类型的重定位
 * Apply REL-type relocations
 *
 * @sechdrs: 段头数组 - Array of section headers
 * @strtab: 字符串表 - String table
 * @symindex: 符号表段索引 - Symbol table section index
 * @relsec: 重定位段索引 - Relocation section index
 * @mod: 模块对象 - Module object
 *
 * 返回值 (Return):
 * 0     - 成功 (Success)
 * -ENOEXEC - 不支持 REL 重定位 (REL relocation unsupported)
 * 其他负值 - 其他错误 (Other errors)
 *
 * REL vs RELA 重定位 (REL vs RELA Relocations):
 * ============================================================================
 * ELF 定义了两种重定位格式:
 * ELF defines two relocation formats:
 *
 * 1. REL (相对重定位):
 *    - 只存储位置和类型
 *    - 加数(addend)隐式存储在被修改位置的原始值中
 *    - 更紧凑,但需要读取原始值
 *    - 用于 x86-32 等架构
 *
 *    - Only stores location and type
 *    - Addend implicitly stored in original value at location being modified
 *    - More compact, but requires reading original value
 *    - Used by architectures like x86-32
 *
 * 2. RELA (绝对重定位):
 *    - 存储位置、类型和显式加数
 *    - 不需要读取原始值
 *    - 更大,但处理更简单直接
 *    - 用于 x86-64, ARM64, RISC-V 等架构
 *
 *    - Stores location, type, and explicit addend
 *    - Doesn't need to read original value
 *    - Larger, but handling is simpler and more straightforward
 *    - Used by x86-64, ARM64, RISC-V, etc.
 *
 * 重定位示例 (Relocation Example):
 * 假设代码中有 "call function_x",编译时 function_x 的地址未知,
 * 链接器放置占位符。加载模块时,重定位过程:
 * 1. 查找 function_x 的实际地址
 * 2. 计算相对偏移或绝对地址
 * 3. 将正确的地址写回代码中的调用指令
 *
 * Suppose code has "call function_x", function_x's address unknown at compile time,
 * linker places placeholder. When loading module, relocation process:
 * 1. Look up actual address of function_x
 * 2. Calculate relative offset or absolute address
 * 3. Write correct address back into call instruction in code
 *
 * 注意事项 (Important Notes):
 * - 每个架构必须实现 apply_relocate 或 apply_relocate_add 至少一个
 * - 不支持的类型应该返回 -ENOEXEC 并打印错误信息
 * - 重定位失败会导致模块加载失败
 *
 * - Each architecture must implement at least one of apply_relocate
 *   or apply_relocate_add
 * - Unsupported types should return -ENOEXEC and print error message
 * - Relocation failure causes module load to fail
 */
/*
 * Apply the given relocation to the (simplified) ELF.  Return -error
 * or 0.
 */
/* 应用给定的重定位到(简化的) ELF。返回 -错误码 或 0。*/
#ifdef CONFIG_MODULES_USE_ELF_REL
int apply_relocate(Elf_Shdr *sechdrs,
		   const char *strtab,
		   unsigned int symindex,
		   unsigned int relsec,
		   struct module *mod);
#else
/**
 * 默认实现: 打印错误并返回 -ENOEXEC
 * Default implementation: print error and return -ENOEXEC
 *
 * 如果架构不支持 REL 重定位,应该使用 RELA (apply_relocate_add)
 * If architecture doesn't support REL relocations, should use RELA
 * (apply_relocate_add)
 */
static inline int apply_relocate(Elf_Shdr *sechdrs,
				 const char *strtab,
				 unsigned int symindex,
				 unsigned int relsec,
				 struct module *me)
{
	printk(KERN_ERR "module %s: REL relocation unsupported\n",
	       module_name(me));
	return -ENOEXEC;
}
#endif

/**
 * apply_relocate_add - 应用 RELA 类型的重定位
 * Apply RELA-type relocations
 *
 * @sechdrs: 段头数组 - Array of section headers
 * @strtab: 字符串表 - String table
 * @symindex: 符号表段索引 - Symbol table section index
 * @relsec: 重定位段索引 - Relocation section index
 * @mod: 模块对象 - Module object
 *
 * 返回值 (Return):
 * 0     - 成功 (Success)
 * -ENOEXEC - 不支持 RELA 重定位 (RELA relocation unsupported)
 * 其他负值 - 其他错误 (Other errors)
 *
 * 与 apply_relocate 的区别:
 * 处理 RELA 格式,其中加数(addend)显式存储在重定位条目中
 * Difference from apply_relocate:
 * Handles RELA format where addend is explicitly stored in relocation entry
 *
 * 典型重定位类型 (Typical Relocation Types):
 * x86-64:
 * - R_X86_64_64: 64位绝对地址
 * - R_X86_64_PC32: 32位PC相对地址
 * - R_X86_64_PLT32: PLT 过程链接表项
 *
 * ARM64:
 * - R_AARCH64_ABS64: 64位绝对地址
 * - R_AARCH64_CALL26: 26位分支指令
 * - R_AARCH64_ADR_PREL_PG_HI21: 页相对寻址高21位
 *
 * 实现复杂性 (Implementation Complexity):
 * 每个架构的指令编码不同,需要:
 * - 解析重定位类型
 * - 计算目标地址
 * - 检查地址范围(某些指令只能寻址有限范围)
 * - 将地址编码到指令的特定位域
 *
 * Each architecture has different instruction encoding, requiring:
 * - Parse relocation type
 * - Calculate target address
 * - Check address range (some instructions can only address limited range)
 * - Encode address into specific bit fields of instruction
 */
/*
 * Apply the given add relocation to the (simplified) ELF.  Return
 * -error or 0
 */
/* 应用给定的 add 重定位到(简化的) ELF。返回 -错误码 或 0。*/
#ifdef CONFIG_MODULES_USE_ELF_RELA
int apply_relocate_add(Elf_Shdr *sechdrs,
		       const char *strtab,
		       unsigned int symindex,
		       unsigned int relsec,
		       struct module *mod);
#ifdef CONFIG_LIVEPATCH
/**
 * clear_relocate_add - 清除之前应用的重定位
 * Clear previously applied relocations
 *
 * @sechdrs: 段头数组 - Array of section headers
 * @strtab: 字符串表 - String table
 * @symindex: 符号表段索引 - Symbol table section index
 * @relsec: 重定位段索引 - Relocation section index
 * @me: 模块对象 - Module object
 *
 * LIVEPATCH 专用功能 (LIVEPATCH-specific Feature):
 * ============================================================================
 * 内核热补丁(livepatch)允许在不重启的情况下修复内核bug。
 * Kernel livepatching allows fixing kernel bugs without rebooting.
 *
 * 问题场景 (Problem Scenario):
 * 1. 加载补丁模块 A,应用重定位,函数被替换
 * 2. 卸载补丁模块 A
 * 3. 重新加载补丁模块 A (或新版本)
 * 4. 某些架构在应用重定位时会进行合理性检查
 * 5. 如果第2步没有清除旧的重定位信息,第3步的检查可能失败
 *
 * 1. Load patch module A, apply relocations, function replaced
 * 2. Unload patch module A
 * 3. Reload patch module A (or new version)
 * 4. Some architectures perform sanity checks when applying relocations
 * 5. If step 2 didn't clear old relocation info, step 3's checks may fail
 *
 * 合理性检查示例 (Sanity Check Examples):
 * - x86_64: 检查指令字节是否符合预期模式
 * - ppc64: 验证跳转目标是否在有效范围内
 *
 * - x86_64: Check if instruction bytes match expected pattern
 * - ppc64: Verify jump target is within valid range
 *
 * 为什么需要清除 (Why Clearing Is Needed):
 * 避免旧的重定位残留触发新的合理性检查,导致重新加载失败
 * Avoid leftover old relocations triggering new sanity checks, causing
 * reload to fail
 *
 * 实现方式 (Implementation):
 * 通常将重定位位置恢复为原始状态或写入空操作指令(NOP)
 * Usually restore relocation locations to original state or write
 * no-operation instructions (NOP)
 */
/*
 * Some architectures (namely x86_64 and ppc64) perform sanity checks when
 * applying relocations.  If a patched module gets unloaded and then later
 * reloaded (and re-patched), klp re-applies relocations to the replacement
 * function(s).  Any leftover relocations from the previous loading of the
 * patched module might trigger the sanity checks.
 *
 * To prevent that, when unloading a patched module, clear out any relocations
 * that might trigger arch-specific sanity checks on a future module reload.
 */
/* 某些架构(特别是 x86_64 和 ppc64)在应用重定位时执行合理性检查。
 * 如果一个已打补丁的模块被卸载,然后稍后重新加载(并重新打补丁),
 * klp 会重新应用重定位到替换函数。任何来自先前加载的已打补丁模块的
 * 残留重定位可能触发合理性检查。
 *
 * 为了防止这种情况,在卸载已打补丁的模块时,清除任何可能在将来模块
 * 重新加载时触发架构特定合理性检查的重定位。
 */
void clear_relocate_add(Elf_Shdr *sechdrs,
		   const char *strtab,
		   unsigned int symindex,
		   unsigned int relsec,
		   struct module *me);
#endif
#else
/**
 * 默认实现: RELA 重定位不支持
 * Default implementation: RELA relocation unsupported
 */
static inline int apply_relocate_add(Elf_Shdr *sechdrs,
				     const char *strtab,
				     unsigned int symindex,
				     unsigned int relsec,
				     struct module *me)
{
	printk(KERN_ERR "module %s: REL relocation unsupported\n",
	       module_name(me));
	return -ENOEXEC;
}
#endif

/**
 * module_finalize - 模块访问前的最后处理
 * Final processing of module before access
 *
 * @hdr: ELF 头 - ELF header
 * @sechdrs: 段头数组 - Array of section headers
 * @mod: 模块对象 - Module object
 *
 * 返回值 (Return):
 * 0     - 成功 (Success)
 * 负值  - 错误码,模块加载将失败 (Error code, module load will fail)
 *
 * 调用时机 (When Called):
 * 在所有重定位完成、符号解析完成之后,但在模块初始化函数(__init)
 * 执行之前调用。这是架构进行最终设置的最后机会。
 * Called after all relocations complete and symbols are resolved, but
 * before module initialization function (__init) executes. This is the
 * last chance for architecture to perform final setup.
 *
 * 典型用途 (Typical Uses):
 * ============================================================================
 * 1. 刷新指令缓存 (Flush Instruction Cache):
 *    很多 CPU 有独立的数据缓存和指令缓存。写入代码后必须刷新
 *    指令缓存,否则 CPU 可能执行旧的(未修改的)指令。
 *    Many CPUs have separate data cache and instruction cache. After
 *    writing code, must flush instruction cache, otherwise CPU may
 *    execute old (unmodified) instructions.
 *
 * 2. 应用替代指令 (Apply Alternative Instructions):
 *    x86 的 alternatives 机制:根据 CPU 特性动态选择最优指令序列。
 *    例如:在支持 SSE 的 CPU 上用 SSE 指令替换通用指令。
 *    x86 alternatives mechanism: dynamically select optimal instruction
 *    sequences based on CPU features. E.g., replace generic instructions
 *    with SSE instructions on CPUs supporting SSE.
 *
 * 3. 设置异常表 (Set Up Exception Tables):
 *    注册模块的异常处理表,用于处理可恢复的错误(如访问用户空间地址)。
 *    Register module's exception handling table for handling recoverable
 *    errors (like accessing userspace addresses).
 *
 * 4. 设置跳转标签 (Set Up Jump Labels):
 *    静态键(static keys)的优化:将运行时分支转换为直接跳转或空操作。
 *    Static keys optimization: convert runtime branches to direct jumps
 *    or no-ops.
 *
 * 5. 设置跟踪点 (Set Up Tracepoints):
 *    注册模块中定义的内核跟踪点。
 *    Register tracepoints defined in the module.
 *
 * 为什么不在 apply_relocate 中做 (Why Not Do This in apply_relocate):
 * - 重定位是每个段独立处理的,finalize 看到完整的模块
 * - 某些操作(如缓存刷新)需要在所有修改完成后一次性执行
 * - 保持关注点分离:重定位处理地址,finalize 处理架构特定优化
 *
 * - Relocation processes each section independently, finalize sees
 *   the complete module
 * - Some operations (like cache flush) need to execute once after
 *   all modifications complete
 * - Maintains separation of concerns: relocation handles addresses,
 *   finalize handles architecture-specific optimizations
 *
 * 安全考虑 (Security Considerations):
 * 这是在模块代码可执行之前修改它的最后机会,之后代码段应该变为只读。
 * This is the last chance to modify module code before it becomes
 * executable, after which code sections should become read-only.
 */
/* Any final processing of module before access.  Return -error or 0. */
/* 模块访问前的任何最终处理。返回 -错误码 或 0。*/
int module_finalize(const Elf_Ehdr *hdr,
		    const Elf_Shdr *sechdrs,
		    struct module *mod);

#ifdef CONFIG_MODULES
/**
 * flush_module_init_free_work - 刷新待释放的模块初始化内存
 * Flush pending module initialization memory to be freed
 *
 * 背景 (Background):
 * ============================================================================
 * 模块加载完成后,.init.* 段(初始化代码和数据)不再需要,可以释放
 * 以节省内存。但是不能立即释放,因为:
 * After module loads, .init.* sections (initialization code and data)
 * are no longer needed and can be freed to save memory. But cannot
 * free immediately because:
 *
 * 1. 可能有 RCU 读端临界区正在访问这些内存
 * 2. 其他 CPU 的缓存中可能还有对这些地址的引用
 * 3. 需要等待所有 CPU 完成相关操作
 *
 * 1. RCU read-side critical sections may be accessing this memory
 * 2. Other CPUs' caches may still have references to these addresses
 * 3. Need to wait for all CPUs to complete related operations
 *
 * 解决方案 (Solution):
 * 使用工作队列(workqueue)延迟释放。释放操作被放入队列,在稍后的
 * 安全时间点执行。
 * Use workqueue to defer freeing. Free operations are queued and
 * executed at a later safe point.
 *
 * 调用时机 (When to Call):
 * - 系统关机/重启前
 * - 内存压力大需要回收内存时
 * - 测试代码验证释放逻辑时
 *
 * - Before system shutdown/reboot
 * - Under memory pressure needing to reclaim memory
 * - Test code verifying free logic
 *
 * 注意 (Note):
 * 这是同步等待所有待处理的释放完成,可能阻塞较长时间
 * This synchronously waits for all pending frees to complete,
 * may block for extended time
 */
void flush_module_init_free_work(void);
#else
/**
 * 空实现 (Empty implementation)
 * CONFIG_MODULES 未启用时,模块支持被完全禁用,无需刷新
 * When CONFIG_MODULES is disabled, module support is completely disabled,
 * no need to flush
 */
static inline void flush_module_init_free_work(void)
{
}
#endif

/**
 * module_arch_cleanup - 模块卸载时的架构特定清理
 * Architecture-specific cleanup when module is unloaded
 *
 * @mod: 正在被卸载的模块 - Module being unloaded
 *
 * 调用时机 (When Called):
 * 在模块的 exit 函数执行之后,释放模块内存之前
 * After module's exit function executes, before freeing module memory
 *
 * 典型用途 (Typical Uses):
 * - 清理架构特定的数据结构
 * - 取消注册架构相关的钩子
 * - 释放 module_frob_arch_sections 中分配的额外资源
 * - 恢复被模块修改的架构状态
 *
 * - Clean up architecture-specific data structures
 * - Unregister architecture-related hooks
 * - Free extra resources allocated in module_frob_arch_sections
 * - Restore architecture state modified by module
 *
 * 对称性 (Symmetry):
 * module_frob_arch_sections (加载时) <-> module_arch_cleanup (卸载时)
 * module_finalize (加载时) <-> module_arch_cleanup (卸载时)
 * module_frob_arch_sections (load) <-> module_arch_cleanup (unload)
 * module_finalize (load) <-> module_arch_cleanup (unload)
 */
/* Any cleanup needed when module leaves. */
/* 模块卸载时需要的任何清理。*/
void module_arch_cleanup(struct module *mod);

/**
 * module_arch_freeing_init - 释放模块初始化内存前的清理
 * Cleanup before freeing module initialization memory
 *
 * @mod: 模块对象 - Module object
 *
 * 调用时机 (When Called):
 * 在释放 mod->module_init (初始化段内存)之前调用
 * Called before freeing mod->module_init (initialization section memory)
 *
 * 与 module_arch_cleanup 的区别 (Difference from module_arch_cleanup):
 * ============================================================================
 * module_arch_freeing_init:
 *   - 在模块加载成功后调用(不是卸载时)
 *   - 仅释放初始化段(.init.*)
 *   - 模块核心代码继续运行
 *   - Called after successful module load (not during unload)
 *   - Only frees initialization sections (.init.*)
 *   - Module core code continues running
 *
 * module_arch_cleanup:
 *   - 在模块完全卸载时调用
 *   - 释放所有模块资源
 *   - 模块将完全消失
 *   - Called during complete module unload
 *   - Frees all module resources
 *   - Module will completely disappear
 *
 * 使用场景 (Use Cases):
 * - 取消映射初始化段中的特殊内存区域
 * - 清理初始化段中使用的临时数据结构
 * - 更新架构的内存跟踪信息
 *
 * - Unmap special memory regions in initialization sections
 * - Clean up temporary data structures used in init sections
 * - Update architecture's memory tracking information
 *
 * 为什么需要单独的钩子 (Why Separate Hook Needed):
 * 某些架构可能对初始化内存有特殊处理(如特殊映射),需要在
 * 释放前执行清理,而不是等到模块完全卸载时。
 * Some architectures may have special handling for init memory
 * (like special mappings), need to perform cleanup before freeing,
 * rather than waiting until module completely unloads.
 */
/* Any cleanup before freeing mod->module_init */
/* 释放 mod->module_init 前的任何清理 */
void module_arch_freeing_init(struct module *mod);

#endif
