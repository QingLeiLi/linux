/* SPDX-License-Identifier: GPL-2.0 */
/*
 * This provides the callbacks and functions that KGDB needs to share between
 * the core, I/O and arch-specific portions.
 *
 * Author: Amit Kale <amitkale@linsyssoft.com> and
 *         Tom Rini <trini@kernel.crashing.org>
 *
 * 2001-2004 (c) Amit S. Kale and 2003-2005 (c) MontaVista Software, Inc.
 */
/*
 * 【KGDB 概述】
 * KGDB (Kernel GNU Debugger) 是 Linux 内核的远程调试工具，允许通过串口、网络等
 * 连接 GDB 调试器来调试运行中的内核。
 *
 * 【工作原理】
 * 1. 内核中嵌入 KGDB stub（存根代码），监听调试请求
 * 2. 当触发断点或异常时，KGDB 接管 CPU 控制权
 * 3. 通过 I/O 驱动（串口/网络）与远程 GDB 通信
 * 4. GDB 发送命令（读内存、设置断点、单步执行等）
 * 5. KGDB stub 解析 GDB 协议，调用架构相关代码执行
 * 6. 返回结果给 GDB，用户看到内核状态
 *
 * 【三层架构】
 * 1. 核心层（kgdb core）：协议解析、状态管理、断点管理
 * 2. I/O 层（kgdb I/O）：串口、网络等通信驱动（kgdboc, kgdboe）
 * 3. 架构层（arch-specific）：寄存器读写、断点设置、单步执行（x86, arm, riscv 等）
 *
 * 【典型使用场景】
 * - 内核启动早期调试（initcall, driver probe）
 * - 死锁、panic 后的现场分析
 * - 驱动开发调试（不方便用 printk 的场景）
 * - 实时系统调试（单步执行中断处理代码）
 *
 * 【配置选项】
 * - CONFIG_KGDB: 启用 KGDB 核心功能
 * - CONFIG_KGDB_SERIAL_CONSOLE: 串口控制台 I/O 驱动
 * - CONFIG_KGDB_KDB: 集成 KDB（内核调试器，可在目标机器上交互）
 * - CONFIG_HAVE_ARCH_KGDB: 架构支持（由 arch/xxx/Kconfig 定义）
 */
#ifndef _KGDB_H_
#define _KGDB_H_

#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/kprobes.h>
#ifdef CONFIG_HAVE_ARCH_KGDB
#include <asm/kgdb.h>
#endif

#ifdef CONFIG_KGDB
struct pt_regs;

/**
 *	kgdb_skipexception - (optional) exit kgdb_handle_exception early
 *	@exception: Exception vector number
 *	@regs: Current &struct pt_regs.
 *
 *	On some architectures it is required to skip a breakpoint
 *	exception when it occurs after a breakpoint has been removed.
 *	This can be implemented in the architecture specific portion of kgdb.
 */
/*
 * 【函数说明】kgdb_skipexception - 跳过异常处理（架构可选实现）
 * @exception: 异常向量号（如 x86 的 #BP 是 3，#DB 是 1）
 * @regs:      当前 CPU 寄存器状态（struct pt_regs *）
 *
 * 【设计目的】
 * 某些架构在移除断点后，CPU 可能仍会触发一次"残留"的断点异常。
 * 此函数允许架构层判断是否应跳过该异常，直接恢复执行而不进入 KGDB。
 *
 * 【实现要点】
 * - 检查异常地址是否是刚移除的断点
 * - 检查是否是单步执行产生的异常（EFLAGS.TF）
 * - 返回值：非零表示跳过异常，零表示正常处理
 *
 * 【架构差异】
 * - x86: 需要处理 INT3 (#BP) 和 Debug (#DB) 异常
 * - ARM: 需要处理 BKPT 指令异常
 * - 如果架构不需要，可以不实现（返回 0）
 */
extern int kgdb_skipexception(int exception, struct pt_regs *regs);

struct tasklet_struct;
struct task_struct;
struct uart_port;

/**
 *	kgdb_breakpoint - compiled in breakpoint
 *
 *	This will be implemented as a static inline per architecture.  This
 *	function is called by the kgdb core to execute an architecture
 *	specific trap to cause kgdb to enter the exception processing.
 *
 */
/*
 * 【函数说明】kgdb_breakpoint - 编译时断点（进入 KGDB 的入口）
 *
 * 【设计目的】
 * 在内核代码中插入断点，主动进入 KGDB 调试状态。
 * 类似用户空间的 __builtin_trap() 或 int 3。
 *
 * 【实现方式】（架构相关）
 * - x86:   asm volatile("int $3")  // INT3 指令
 * - ARM:   asm volatile(".inst 0xe7f001f0")  // BKPT #0
 * - ARM64: asm volatile("brk #0")
 * - RISC-V: asm volatile("ebreak")
 *
 * 【使用场景】
 * 1. 早期启动调试：在 start_kernel() 中插入 kgdb_breakpoint()
 * 2. 驱动探测调试：在 probe 函数中插入
 * 3. 条件断点：if (rare_condition) kgdb_breakpoint();
 *
 * 【与硬件断点的区别】
 * - kgdb_breakpoint: 软件断点，修改代码插入 trap 指令
 * - 硬件断点: 利用 CPU 调试寄存器（如 x86 的 DR0-DR3），不修改代码
 *
 * 【注意事项】
 * - 如果 KGDB 未连接，会触发 panic（除非设置了 kgdb_early）
 * - 在中断上下文中使用要小心（可能导致死锁）
 */
void kgdb_breakpoint(void);

/*
 * 【全局变量】KGDB 连接和模块状态
 */
extern int kgdb_connected;           /* 是否有 GDB 客户端已连接（1=已连接，0=未连接） */
extern int kgdb_io_module_registered; /* 是否已注册 I/O 模块（串口/网络驱动） */

/*
 * 【原子变量】KGDB 运行时状态（用于多核同步）
 */
extern atomic_t			kgdb_setting_breakpoint;  /* 正在设置断点（防止递归） */
extern atomic_t			kgdb_cpu_doing_single_step; /* 哪个 CPU 正在单步执行 */

/*
 * 【任务指针】KGDB 线程上下文切换
 */
extern struct task_struct	*kgdb_usethread;  /* 用户请求查看的线程（GDB 的 "thread" 命令） */
extern struct task_struct	*kgdb_contthread; /* 继续执行时使用的线程 */

/*
 * 【枚举】kgdb_bptype - 断点类型
 *
 * KGDB 支持多种类型的断点和观察点（watchpoint），由 GDB 客户端请求。
 */
enum kgdb_bptype {
	BP_BREAKPOINT = 0,        /* 软件断点：在指令处插入 trap（如 x86 的 INT3）
	                           * 修改代码内存，触发异常时进入 KGDB */
	BP_HARDWARE_BREAKPOINT,   /* 硬件断点：使用 CPU 调试寄存器（如 x86 的 DR0-DR3）
	                           * 不修改代码，支持数量有限（通常 4 个） */
	BP_WRITE_WATCHPOINT,      /* 写观察点：监视内存写操作
	                           * 当目标地址被写入时触发异常 */
	BP_READ_WATCHPOINT,       /* 读观察点：监视内存读操作
	                           * 当目标地址被读取时触发异常 */
	BP_ACCESS_WATCHPOINT,     /* 访问观察点：监视读或写
	                           * 等价于 BP_READ_WATCHPOINT | BP_WRITE_WATCHPOINT */
	BP_POKE_BREAKPOINT,       /* Poke 断点：临时断点，用于内存写入操作
	                           * KGDB 内部使用，写入内存后立即移除 */
};

/*
 * 【枚举】kgdb_bpstate - 断点状态
 *
 * 断点的生命周期状态机：
 * BP_UNDEFINED -> BP_REMOVED -> BP_SET -> BP_ACTIVE -> BP_REMOVED
 */
enum kgdb_bpstate {
	BP_UNDEFINED = 0,  /* 未定义：断点槽位未使用（初始状态） */
	BP_REMOVED,        /* 已移除：断点已被删除，但槽位保留（可复用） */
	BP_SET,            /* 已设置：断点已配置但尚未激活（等待安装到内存/寄存器） */
	BP_ACTIVE          /* 已激活：断点正在工作中（代码已修改或寄存器已配置） */
};

/*
 * 【结构体】kgdb_bkpt - 断点描述符
 *
 * KGDB 维护一个断点数组（大小为 KGDB_MAX_BREAKPOINTS），
 * 每个元素描述一个断点的详细信息。
 */
struct kgdb_bkpt {
	unsigned long		bpt_addr;               /* 断点地址（虚拟地址）
	                                             * 对于指令断点是 PC 地址
	                                             * 对于观察点是内存地址 */
	unsigned char		saved_instr[BREAK_INSTR_SIZE]; /* 保存的原始指令字节
	                                             * 用于软件断点：保存被 trap 指令覆盖的原始代码
	                                             * 移除断点时恢复这些字节
	                                             * BREAK_INSTR_SIZE 是架构相关的（x86=1, ARM=4） */
	enum kgdb_bptype	type;                   /* 断点类型（见上面的枚举） */
	enum kgdb_bpstate	state;                  /* 断点状态（见上面的枚举） */
};

/*
 * 【结构体】dbg_reg_def_t - 寄存器定义
 *
 * 描述 CPU 寄存器的元数据，用于 GDB 的寄存器读写协议。
 * 架构层需要提供一个 dbg_reg_def[] 数组，定义所有可调试的寄存器。
 */
struct dbg_reg_def_t {
	char *name;    /* 寄存器名称（如 "rax", "rsp", "pc"）
	                * GDB 客户端显示时使用 */
	int size;      /* 寄存器大小（字节）
	                * 例如：x86_64 通用寄存器是 8 字节
	                *       x86_32 通用寄存器是 4 字节
	                *       ARM Thumb 寄存器可能是 2 或 4 字节 */
	int offset;    /* 在 pt_regs 结构中的偏移量（字节）
	                * 用于快速定位寄存器在内核寄存器快照中的位置
	                * 例如：offsetof(struct pt_regs, rax) */
};

/*
 * 【宏定义】DBG_MAX_REG_NUM - 最大寄存器数量
 *
 * 架构层定义支持调试的寄存器总数。
 * 如果未定义，默认为 0（表示不支持寄存器访问）。
 */
#ifndef DBG_MAX_REG_NUM
#define DBG_MAX_REG_NUM 0
#else
/*
 * 【架构接口】寄存器读写回调
 *
 * 如果架构定义了 DBG_MAX_REG_NUM > 0，需要提供：
 */
extern struct dbg_reg_def_t dbg_reg_def[]; /* 寄存器定义数组 */

/*
 * dbg_get_reg - 读取指定寄存器的值
 * @regno: 寄存器编号（索引到 dbg_reg_def[]）
 * @mem:   输出缓冲区（存放寄存器值，格式为大端字节序）
 * @regs:  当前 CPU 寄存器状态（struct pt_regs *）
 *
 * 返回值：指向下一个字节的指针（mem + size），用于连续读取多个寄存器
 */
extern char *dbg_get_reg(int regno, void *mem, struct pt_regs *regs);

/*
 * dbg_set_reg - 写入指定寄存器的值
 * @regno: 寄存器编号
 * @mem:   输入缓冲区（包含新值，格式为大端字节序）
 * @regs:  要修改的寄存器状态（struct pt_regs *）
 *
 * 返回值：0=成功，负值=失败（如寄存器只读）
 */
extern int dbg_set_reg(int regno, void *mem, struct pt_regs *regs);
#endif

/*
 * 【宏定义】KGDB_MAX_BREAKPOINTS - 最大断点数量
 *
 * KGDB 支持的断点总数上限（软件断点 + 硬件断点 + 观察点）。
 * 默认 1000 个，架构可以重新定义（如嵌入式系统可能减少到 64）。
 *
 * 【为什么需要限制？】
 * - 每个断点占用内存（struct kgdb_bkpt 约 32 字节）
 * - 断点查找是线性扫描（O(n)），太多会影响性能
 * - 实际使用中很少超过 100 个断点
 */
#ifndef KGDB_MAX_BREAKPOINTS
# define KGDB_MAX_BREAKPOINTS	1000
#endif

/*
 * 【宏定义】KGDB_HW_BREAKPOINT - 硬件断点标志
 *
 * 架构层在 kgdb_arch.flags 中设置此标志，表示支持硬件断点。
 * KGDB 核心根据此标志决定是否允许 GDB 设置硬件断点。
 */
#define KGDB_HW_BREAKPOINT	1

/*
 * Functions each KGDB-supporting architecture must provide:
 */
/*
 * ============================================================================
 * 【架构必须实现的函数接口】
 *
 * 以下函数由各个 CPU 架构（x86, ARM, RISC-V 等）实现，提供平台相关的调试功能。
 * KGDB 核心通过这些接口与硬件交互，实现跨平台的调试支持。
 * ============================================================================
 */

/**
 *	kgdb_arch_init - Perform any architecture specific initialization.
 *
 *	This function will handle the initialization of any architecture
 *	specific callbacks.
 */
/*
 * 【函数】kgdb_arch_init - 架构特定的初始化
 *
 * 返回值：0=成功，负值=失败
 *
 * 【调用时机】
 * 在 KGDB 核心初始化时调用（dbg_late_init），通常在内核启动后期。
 *
 * 【典型实现内容】
 * 1. 设置调试寄存器（如 x86 的 DR7）
 * 2. 注册异常处理器（如 ARM 的 undefined instruction handler）
 * 3. 初始化断点指令（如 x86 的 INT3，ARM 的 BKPT）
 * 4. 配置硬件断点/观察点支持
 * 5. 设置单步执行标志位（如 x86 的 EFLAGS.TF）
 *
 * 【示例】(x86)
 * - 清除调试寄存器 DR0-DR7
 * - 设置断点指令为 0xCC (INT3)
 * - 注册 #BP (INT 3) 和 #DB (INT 1) 异常处理
 *
 * 【注意事项】
 * - 必须在中断可用后调用（需要注册异常处理器）
 * - 失败时内核会禁用 KGDB 功能
 */
extern int kgdb_arch_init(void);

/**
 *	kgdb_arch_exit - Perform any architecture specific uninitalization.
 *
 *	This function will handle the uninitalization of any architecture
 *	specific callbacks, for dynamic registration and unregistration.
 */
/*
 * 【函数】kgdb_arch_exit - 架构特定的清理
 *
 * 无返回值
 *
 * 【调用时机】
 * 在 KGDB 模块卸载或系统关闭时调用。
 *
 * 【典型实现内容】
 * 1. 移除所有断点（软件和硬件）
 * 2. 清除调试寄存器
 * 3. 注销异常处理器
 * 4. 恢复被修改的指令
 *
 * 【为什么需要？】
 * - 支持 KGDB 模块的动态加载/卸载（CONFIG_KGDB=m）
 * - 防止卸载后触发野指针异常
 * - 清理调试硬件状态，避免影响其他调试工具（如 perf）
 */
extern void kgdb_arch_exit(void);

/**
 *	pt_regs_to_gdb_regs - Convert ptrace regs to GDB regs
 *	@gdb_regs: A pointer to hold the registers in the order GDB wants.
 *	@regs: The &struct pt_regs of the current process.
 *
 *	Convert the pt_regs in @regs into the format for registers that
 *	GDB expects, stored in @gdb_regs.
 */
/*
 * 【函数】pt_regs_to_gdb_regs - 内核寄存器格式 → GDB 寄存器格式
 * @gdb_regs: 输出缓冲区（unsigned long 数组），存放 GDB 格式的寄存器
 * @regs:     输入参数（struct pt_regs *），内核保存的寄存器快照
 *
 * 无返回值
 *
 * 【为什么需要转换？】
 * 内核的 struct pt_regs 布局是架构相关的，而 GDB 期望的寄存器顺序
 * 是由 GDB 的目标描述（target description）定义的。两者不一定匹配。
 *
 * 【转换规则】
 * GDB 寄存器编号（regno）到 pt_regs 字段的映射，例如 x86_64：
 * - gdb_regs[0]  = regs->rax
 * - gdb_regs[1]  = regs->rbx
 * - gdb_regs[16] = regs->rip (程序计数器)
 * - gdb_regs[17] = regs->eflags
 *
 * 【字节序问题】
 * GDB 协议使用大端字节序（big-endian），但大多数 CPU 是小端（little-endian）。
 * 架构实现需要处理字节序转换（通常 KGDB 核心会处理）。
 *
 * 【特殊寄存器】
 * - 程序计数器（PC/RIP/EIP）：必须正确转换，GDB 依赖它显示当前位置
 * - 栈指针（SP/RSP/ESP）：用于回溯调用栈
 * - 标志寄存器（EFLAGS/CPSR）：包含条件码和控制位
 *
 * 【使用场景】
 * - GDB 的 "info registers" 命令
 * - GDB 读取寄存器（g 命令）
 * - 回溯调用栈时读取 PC 和 SP
 */
extern void pt_regs_to_gdb_regs(unsigned long *gdb_regs, struct pt_regs *regs);

/**
 *	sleeping_thread_to_gdb_regs - Convert ptrace regs to GDB regs
 *	@gdb_regs: A pointer to hold the registers in the order GDB wants.
 *	@p: The &struct task_struct of the desired process.
 *
 *	Convert the register values of the sleeping process in @p to
 *	the format that GDB expects.
 *	This function is called when kgdb does not have access to the
 *	&struct pt_regs and therefore it should fill the gdb registers
 *	@gdb_regs with what has	been saved in &struct thread_struct
 *	thread field during switch_to.
 */
/*
 * 【函数】sleeping_thread_to_gdb_regs - 休眠线程寄存器 → GDB 格式
 * @gdb_regs: 输出缓冲区，存放 GDB 格式的寄存器
 * @p:        目标进程（struct task_struct *）
 *
 * 无返回值
 *
 * 【为什么需要这个函数？】
 * 当调试非当前线程（sleeping thread）时，内核没有保存完整的 pt_regs，
 * 只有在进程切换（context switch）时保存的关键寄存器（如 SP, PC, 被调用者保存寄存器）。
 * 这些寄存器存储在 task_struct->thread 中。
 *
 * 【pt_regs vs thread_struct】
 * - pt_regs:       保存所有寄存器（异常/中断发生时）
 * - thread_struct: 仅保存调度器需要恢复的寄存器（SP, PC, callee-saved）
 *
 * 【典型实现】(x86_64)
 * - gdb_regs[SP] = p->thread.sp  (栈指针)
 * - gdb_regs[PC] = p->thread.ip  (返回地址，通常指向 schedule())
 * - gdb_regs[BP] = *(unsigned long *)(p->thread.sp) (栈帧基址)
 * - 其他寄存器设为 0 或未定义（GDB 会显示 <unavailable>）
 *
 * 【调用场景】
 * - GDB 的 "thread" 命令切换到其他线程
 * - "info threads" 显示所有线程状态
 * - "backtrace" 回溯非当前线程的调用栈
 *
 * 【限制】
 * - 无法获取调用者保存的寄存器（caller-saved，如 x86 的 rax, rcx, rdx）
 * - PC 指向 schedule() 内部，而不是线程被抢占的实际位置
 * - 某些架构可能无法实现（返回全 0）
 */
extern void
sleeping_thread_to_gdb_regs(unsigned long *gdb_regs, struct task_struct *p);

/**
 *	gdb_regs_to_pt_regs - Convert GDB regs to ptrace regs.
 *	@gdb_regs: A pointer to hold the registers we've received from GDB.
 *	@regs: A pointer to a &struct pt_regs to hold these values in.
 *
 *	Convert the GDB regs in @gdb_regs into the pt_regs, and store them
 *	in @regs.
 */
/*
 * 【函数】gdb_regs_to_pt_regs - GDB 寄存器格式 → 内核寄存器格式
 * @gdb_regs: 输入缓冲区（unsigned long 数组），GDB 发送的寄存器值
 * @regs:     输出参数（struct pt_regs *），要更新的内核寄存器
 *
 * 无返回值
 *
 * 【为什么需要转换？】
 * GDB 的 "set $reg = value" 命令会修改寄存器，KGDB 需要将修改
 * 应用到内核的 pt_regs 结构，这样恢复执行时寄存器值生效。
 *
 * 【转换规则】
 * pt_regs_to_gdb_regs 的逆操作，例如 x86_64：
 * - regs->rax    = gdb_regs[0]
 * - regs->rip    = gdb_regs[16]
 * - regs->eflags = gdb_regs[17]
 *
 * 【安全性考虑】
 * 某些寄存器不允许修改（安全风险），需要过滤：
 * - 特权级位（x86 EFLAGS 的 IOPL, VM, RF）
 * - 系统寄存器（CR0-CR4, MSR）
 * - 保留位（必须保持固定值）
 *
 * 典型实现会屏蔽危险位：
 *   regs->eflags = (gdb_regs[17] & SAFE_MASK) | (regs->eflags & ~SAFE_MASK)
 *
 * 【使用场景】
 * - GDB 的 "set $rax = 0x1234" 命令
 * - GDB 的 "jump *0xaddress" 命令（修改 PC）
 * - 修复寄存器值以跳过错误指令
 *
 * 【注意事项】
 * - PC 修改后可能跳到非法地址（需要架构验证）
 * - SP 修改可能破坏栈（导致崩溃）
 * - 某些架构禁止修改某些寄存器（返回错误）
 */
extern void gdb_regs_to_pt_regs(unsigned long *gdb_regs, struct pt_regs *regs);

/**
 *	kgdb_arch_handle_exception - Handle architecture specific GDB packets.
 *	@vector: The error vector of the exception that happened.
 *	@signo: The signal number of the exception that happened.
 *	@err_code: The error code of the exception that happened.
 *	@remcom_in_buffer: The buffer of the packet we have read.
 *	@remcom_out_buffer: The buffer of %BUFMAX bytes to write a packet into.
 *	@regs: The &struct pt_regs of the current process.
 *
 *	This function MUST handle the 'c' and 's' command packets,
 *	as well packets to set / remove a hardware breakpoint, if used.
 *	If there are additional packets which the hardware needs to handle,
 *	they are handled here.  The code should return -1 if it wants to
 *	process more packets, and a %0 or %1 if it wants to exit from the
 *	kgdb callback.
 */
extern int
kgdb_arch_handle_exception(int vector, int signo, int err_code,
			   char *remcom_in_buffer,
			   char *remcom_out_buffer,
			   struct pt_regs *regs);

/**
 *	kgdb_arch_handle_qxfer_pkt - Handle architecture specific GDB XML
 *				     packets.
 *	@remcom_in_buffer: The buffer of the packet we have read.
 *	@remcom_out_buffer: The buffer of %BUFMAX bytes to write a packet into.
 */

extern void
kgdb_arch_handle_qxfer_pkt(char *remcom_in_buffer,
			   char *remcom_out_buffer);

/**
 *	kgdb_call_nmi_hook - Call kgdb_nmicallback() on the current CPU
 *	@ignored: This parameter is only here to match the prototype.
 *
 *	If you're using the default implementation of kgdb_roundup_cpus()
 *	this function will be called per CPU.  If you don't implement
 *	kgdb_call_nmi_hook() a default will be used.
 */

extern void kgdb_call_nmi_hook(void *ignored);

/**
 *	kgdb_roundup_cpus - Get other CPUs into a holding pattern
 *
 *	On SMP systems, we need to get the attention of the other CPUs
 *	and get them into a known state.  This should do what is needed
 *	to get the other CPUs to call kgdb_handle_exception().  Note that
 *	on some arches, the NMI approach is not used for rounding up all
 *	the CPUs.  Normally those architectures can just not implement
 *	this and get the default.
 *
 *	On non-SMP systems, this is not called.
 */
extern void kgdb_roundup_cpus(void);

/**
 *	kgdb_arch_set_pc - Generic call back to the program counter
 *	@regs: Current &struct pt_regs.
 *  @pc: The new value for the program counter
 *
 *	This function handles updating the program counter and requires an
 *	architecture specific implementation.
 */
extern void kgdb_arch_set_pc(struct pt_regs *regs, unsigned long pc);


/* Optional functions. */
extern int kgdb_validate_break_address(unsigned long addr);
extern int kgdb_arch_set_breakpoint(struct kgdb_bkpt *bpt);
extern int kgdb_arch_remove_breakpoint(struct kgdb_bkpt *bpt);

/**
 *	kgdb_arch_late - Perform any architecture specific initialization.
 *
 *	This function will handle the late initialization of any
 *	architecture specific callbacks.  This is an optional function for
 *	handling things like late initialization of hw breakpoints.  The
 *	default implementation does nothing.
 */
extern void kgdb_arch_late(void);


/**
 * struct kgdb_arch - Describe architecture specific values.
 * @gdb_bpt_instr: The instruction to trigger a breakpoint.
 * @flags: Flags for the breakpoint, currently just %KGDB_HW_BREAKPOINT.
 * @set_breakpoint: Allow an architecture to specify how to set a software
 * breakpoint.
 * @remove_breakpoint: Allow an architecture to specify how to remove a
 * software breakpoint.
 * @set_hw_breakpoint: Allow an architecture to specify how to set a hardware
 * breakpoint.
 * @remove_hw_breakpoint: Allow an architecture to specify how to remove a
 * hardware breakpoint.
 * @disable_hw_break: Allow an architecture to specify how to disable
 * hardware breakpoints for a single cpu.
 * @remove_all_hw_break: Allow an architecture to specify how to remove all
 * hardware breakpoints.
 * @correct_hw_break: Allow an architecture to specify how to correct the
 * hardware debug registers.
 */
/*
 * 【结构体】kgdb_arch - 架构特定的 KGDB 配置和回调函数
 *
 * 每个支持 KGDB 的架构必须定义一个全局变量 arch_kgdb_ops，
 * 类型为 const struct kgdb_arch。
 */
struct kgdb_arch {
	unsigned char		gdb_bpt_instr[BREAK_INSTR_SIZE];
	/* 断点指令的机器码字节序列
	 * 【x86】    BREAK_INSTR_SIZE=1,  gdb_bpt_instr={0xCC}           (INT3)
	 * 【ARM】    BREAK_INSTR_SIZE=4,  gdb_bpt_instr={0xF0, 0x01, 0xF0, 0xE7} (BKPT #0)
	 * 【ARM64】  BREAK_INSTR_SIZE=4,  gdb_bpt_instr={0x00, 0x00, 0x20, 0xD4} (BRK #0)
	 * 【RISC-V】 BREAK_INSTR_SIZE=2,  gdb_bpt_instr={0x02, 0x90}     (C.EBREAK)
	 *
	 * 【为什么需要？】
	 * 软件断点通过在目标地址插入 trap 指令实现，不同架构的 trap 指令不同。
	 * KGDB 核心读取这个数组，写入目标内存时使用。
	 */

	unsigned long		flags;
	/* 架构能力标志位
	 * 【当前仅有一个标志】
	 * KGDB_HW_BREAKPOINT (1): 支持硬件断点和观察点
	 *
	 * 【如何使用？】
	 * if (arch_kgdb_ops.flags & KGDB_HW_BREAKPOINT) {
	 *     // 允许 GDB 设置硬件断点
	 * }
	 */

	int	(*set_breakpoint)(unsigned long, char *);
	/* 设置软件断点（可选回调）
	 * @unsigned long: 断点地址（虚拟地址）
	 * @char *:        保存原始指令的缓冲区（输出参数）
	 *
	 * 返回值：0=成功，负值=失败（如地址不可写）
	 *
	 * 【默认行为】
	 * 如果为 NULL，KGDB 核心使用 probe_kernel_write() 直接写入 gdb_bpt_instr。
	 *
	 * 【为什么需要自定义？】
	 * 某些架构有特殊要求：
	 * - ARM Thumb 模式需要判断指令宽度（2 字节或 4 字节）
	 * - 某些架构需要刷新指令缓存（I-cache）
	 * - 某些架构需要处理对齐问题
	 */

	int	(*remove_breakpoint)(unsigned long, char *);
	/* 移除软件断点（可选回调）
	 * @unsigned long: 断点地址
	 * @char *:        原始指令（从 saved_instr 获取）
	 *
	 * 返回值：0=成功，负值=失败
	 *
	 * 【默认行为】
	 * 如果为 NULL，KGDB 核心使用 probe_kernel_write() 恢复原始指令。
	 *
	 * 【典型实现】
	 * 1. 写入原始指令
	 * 2. 刷新指令缓存（flush_icache_range）
	 * 3. 同步所有 CPU 的指令流水线
	 */

	int	(*set_hw_breakpoint)(unsigned long, int, enum kgdb_bptype);
	/* 设置硬件断点/观察点（必需，如果 flags 包含 KGDB_HW_BREAKPOINT）
	 * @unsigned long:      断点地址
	 * @int:                断点长度（字节）
	 *                      - 指令断点通常为指令宽度（x86=1, ARM=4）
	 *                      - 观察点为监视的内存范围（1, 2, 4, 8 字节）
	 * @enum kgdb_bptype:   断点类型（BP_HARDWARE_BREAKPOINT, BP_WRITE_WATCHPOINT 等）
	 *
	 * 返回值：0=成功，负值=失败（如寄存器用完）
	 *
	 * 【典型实现】(x86)
	 * 1. 查找空闲的调试寄存器（DR0-DR3）
	 * 2. 写入断点地址到 DRi
	 * 3. 配置 DR7 寄存器（使能断点、设置类型和长度）
	 * 4. 返回 0 或 -ENOSPC（寄存器用完）
	 *
	 * 【硬件限制】
	 * - x86/x86_64: 4 个断点寄存器（DR0-DR3）
	 * - ARM: 通常 4-16 个（取决于实现）
	 * - RISC-V: Sdext 扩展提供 0-16 个
	 */

	int	(*remove_hw_breakpoint)(unsigned long, int, enum kgdb_bptype);
	/* 移除硬件断点/观察点（必需，如果 flags 包含 KGDB_HW_BREAKPOINT）
	 * 参数同 set_hw_breakpoint
	 *
	 * 返回值：0=成功，负值=失败
	 *
	 * 【典型实现】(x86)
	 * 1. 查找匹配的调试寄存器（地址和类型都匹配）
	 * 2. 清除 DR7 中对应的使能位
	 * 3. 清零 DRi 寄存器
	 */

	void	(*disable_hw_break)(struct pt_regs *regs);
	/* 临时禁用所有硬件断点（可选回调）
	 * @regs: 当前 CPU 寄存器状态
	 *
	 * 无返回值
	 *
	 * 【为什么需要？】
	 * 在某些情况下需要临时禁用硬件断点：
	 * - KGDB 自身的代码执行（防止递归触发）
	 * - 单步执行时（避免干扰）
	 * - 修改断点配置时（原子性保证）
	 *
	 * 【典型实现】(x86)
	 * 清除 DR7 的全局使能位（GE），但保留断点配置。
	 */

	void	(*remove_all_hw_break)(void);
	/* 移除所有硬件断点/观察点（可选回调）
	 *
	 * 无返回值
	 *
	 * 【调用时机】
	 * - KGDB 退出时清理
	 * - 调试会话结束时重置硬件状态
	 * - 错误恢复路径
	 *
	 * 【典型实现】(x86)
	 * 1. 清零 DR0-DR3（断点地址）
	 * 2. 清零 DR7（断点配置）
	 * 3. 清零 DR6（状态寄存器）
	 */

	void	(*correct_hw_break)(void);
	/* 修正硬件断点寄存器（可选回调）
	 *
	 * 无返回值
	 *
	 * 【为什么需要？】
	 * 某些情况下调试寄存器可能被破坏：
	 * - 上下文切换（其他任务的调试状态）
	 * - 异常处理（CPU 自动修改 DR6）
	 * - 并发调试（多个调试器冲突）
	 *
	 * 【典型实现】
	 * 1. 读取当前硬件寄存器值
	 * 2. 与 KGDB 内部的断点数组对比
	 * 3. 如果不一致，重新写入正确的值
	 * 4. 用于从异常中恢复
	 */
};

/**
 * struct kgdb_io - Describe the interface for an I/O driver to talk with KGDB.
 * @name: Name of the I/O driver.
 * @read_char: Pointer to a function that will return one char.
 * @write_char: Pointer to a function that will write one char.
 * @flush: Pointer to a function that will flush any pending writes.
 * @init: Pointer to a function that will initialize the device.
 * @deinit: Pointer to a function that will deinit the device. Implies that
 * this I/O driver is temporary and expects to be replaced. Called when
 * an I/O driver is replaced or explicitly unregistered.
 * @pre_exception: Pointer to a function that will do any prep work for
 * the I/O driver.
 * @post_exception: Pointer to a function that will do any cleanup work
 * for the I/O driver.
 * @cons: valid if the I/O device is a console; else NULL.
 */
/*
 * 【结构体】kgdb_io - KGDB I/O 驱动接口（连接 GDB 客户端的通信层）
 *
 * KGDB 通过 I/O 驱动与远程 GDB 通信，支持多种传输方式：
 * - 串口（kgdboc: kgdb over console）
 * - 网络（kgdboe: kgdb over ethernet，已废弃）
 * - USB（通过 USB-serial 模拟串口）
 *
 * 【注册方式】
 * int kgdb_register_io_module(struct kgdb_io *ops);
 *
 * 【示例】串口驱动（kgdboc）
 * static struct kgdb_io kgdboc_io_ops = {
 *     .name            = "kgdboc",
 *     .read_char       = kgdboc_get_char,
 *     .write_char      = kgdboc_put_char,
 *     .init            = kgdboc_init,
 *     .pre_exception   = kgdboc_pre_exp_handler,
 *     .post_exception  = kgdboc_post_exp_handler,
 * };
 */
struct kgdb_io {
	const char		*name;
	/* I/O 驱动名称（如 "kgdboc", "kgdboe"）
	 * 用于日志输出和用户识别
	 */

	int			(*read_char) (void);
	/* 读取一个字符（阻塞）
	 * 返回值：0-255 为有效字符，负值为错误（如超时）
	 *
	 * 【实现要求】
	 * - 必须阻塞直到有字符可读或超时
	 * - 超时应返回 -EAGAIN（允许 KGDB 检查其他事件）
	 * - 不能使用中断（KGDB 在异常上下文中运行）
	 * - 必须是轮询（polling）模式
	 *
	 * 【典型实现】(串口)
	 * while (!(uart_read(STATUS_REG) & RX_READY)) {
	 *     if (timeout--) return -EAGAIN;
	 *     cpu_relax();
	 * }
	 * return uart_read(DATA_REG);
	 */

	void			(*write_char) (u8);
	/* 写入一个字符（阻塞）
	 * @u8: 要发送的字符（0-255）
	 *
	 * 无返回值
	 *
	 * 【实现要求】
	 * - 必须等待发送完成才返回（或超时）
	 * - 不能使用中断
	 * - 必须是轮询模式
	 *
	 * 【典型实现】(串口)
	 * while (!(uart_read(STATUS_REG) & TX_EMPTY))
	 *     cpu_relax();
	 * uart_write(DATA_REG, c);
	 */

	void			(*flush) (void);
	/* 刷新发送缓冲区（可选）
	 *
	 * 无返回值
	 *
	 * 【为什么需要？】
	 * 某些硬件有 FIFO 缓冲区，数据可能还未真正发送出去。
	 * flush 确保所有数据都已通过物理层传输。
	 *
	 * 【典型实现】(串口)
	 * while (!(uart_read(STATUS_REG) & TX_FIFO_EMPTY))
	 *     cpu_relax();
	 */

	int			(*init) (void);
	/* 初始化 I/O 设备（可选）
	 * 返回值：0=成功，负值=失败
	 *
	 * 【调用时机】
	 * 在 kgdb_register_io_module() 中调用。
	 *
	 * 【典型实现】
	 * 1. 保存当前串口配置（波特率、数据位等）
	 * 2. 设置 KGDB 需要的配置（通常 115200 8N1）
	 * 3. 禁用中断（改为轮询模式）
	 * 4. 清空接收/发送缓冲区
	 */

	void			(*deinit) (void);
	/* 清理 I/O 设备（可选）
	 *
	 * 无返回值
	 *
	 * 【调用时机】
	 * - I/O 驱动被替换时
	 * - kgdb_unregister_io_module() 被调用时
	 *
	 * 【典型实现】
	 * 1. 恢复原始串口配置
	 * 2. 重新启用中断
	 * 3. 释放资源
	 */

	void			(*pre_exception) (void);
	/* 异常前准备（可选）
	 *
	 * 无返回值
	 *
	 * 【调用时机】
	 * 在进入 KGDB 异常处理之前调用。
	 *
	 * 【典型实现】
	 * 1. 禁用串口控制台输出（防止 printk 干扰 GDB 协议）
	 * 2. 保存中断状态
	 * 3. 锁定串口资源
	 */

	void			(*post_exception) (void);
	/* 异常后清理（可选）
	 *
	 * 无返回值
	 *
	 * 【调用时机】
	 * 在退出 KGDB 异常处理之后调用。
	 *
	 * 【典型实现】
	 * 1. 重新启用串口控制台
	 * 2. 恢复中断状态
	 * 3. 释放串口锁
	 */

	struct console		*cons;
	/* 关联的控制台设备（如果 I/O 设备是控制台）
	 * 否则为 NULL
	 *
	 * 【为什么需要？】
	 * kgdboc（kgdb over console）复用串口控制台设备，
	 * 需要协调 KGDB 和内核日志输出，避免冲突。
	 */
};

extern const struct kgdb_arch		arch_kgdb_ops;

extern unsigned long kgdb_arch_pc(int exception, struct pt_regs *regs);

extern int kgdb_register_io_module(struct kgdb_io *local_kgdb_io_ops);
extern void kgdb_unregister_io_module(struct kgdb_io *local_kgdb_io_ops);
extern struct kgdb_io *dbg_io_ops;

extern int kgdb_hex2long(char **ptr, unsigned long *long_val);
extern char *kgdb_mem2hex(char *mem, char *buf, int count);
extern int kgdb_hex2mem(char *buf, char *mem, int count);

extern int kgdb_isremovedbreak(unsigned long addr);
extern int kgdb_has_hit_break(unsigned long addr);

extern int
kgdb_handle_exception(int ex_vector, int signo, int err_code,
		      struct pt_regs *regs);
extern int kgdb_nmicallback(int cpu, void *regs);
extern int kgdb_nmicallin(int cpu, int trapnr, void *regs, int err_code,
			  atomic_t *snd_rdy);
extern void gdbstub_exit(int status);

/*
 * kgdb and kprobes both use the same (kprobe) blocklist (which makes sense
 * given they are both typically hooked up to the same trap meaning on most
 * architectures one cannot be used to debug the other)
 *
 * However on architectures where kprobes is not (yet) implemented we permit
 * breakpoints everywhere rather than blocking everything by default.
 */
static inline bool kgdb_within_blocklist(unsigned long addr)
{
#ifdef CONFIG_KGDB_HONOUR_BLOCKLIST
	return within_kprobe_blacklist(addr);
#else
	return false;
#endif
}

extern int			kgdb_single_step;
extern atomic_t			kgdb_active;
#define in_dbg_master() \
	(irqs_disabled() && (smp_processor_id() == atomic_read(&kgdb_active)))
extern bool dbg_is_early;
extern void __init dbg_late_init(void);
extern void kgdb_panic(const char *msg);
extern void kgdb_free_init_mem(void);
#else /* ! CONFIG_KGDB */
#define in_dbg_master() (0)
#define dbg_late_init()
static inline void kgdb_panic(const char *msg) {}
static inline void kgdb_free_init_mem(void) { }
static inline int kgdb_nmicallback(int cpu, void *regs) { return 1; }
#endif /* ! CONFIG_KGDB */
#endif /* _KGDB_H_ */
