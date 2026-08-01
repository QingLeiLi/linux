/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 早期启动与 CPU 汇合学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 业务位置：固件或 boot loader 把已加载的 Image 交给 primary_entry；本文件在
 * 普通内核运行环境尚不存在时，建立“能够进入 start_kernel()”所需的最小可信
 * 世界。它负责核对启动 CPU 状态、保留设备树入口、创建恒等映射、统一 EL1/EL2、
 * 配置页表与 MMU、建立 init_task/每 CPU 基址和异常向量。它不解析设备树、不
 * 初始化伙伴分配器/调度器/驱动；这些业务从 start_kernel() 后才逐步开始。
 *
 * 主 CPU 端到端路径：
 *   固件/boot loader -> Image 固定头 -> primary_entry
 *   -> 记录并规范化 MMU/端序 -> 保存 FDT -> 创建 init idmap
 *   -> init_kernel_el -> __cpu_setup -> __primary_switch
 *   -> 开 MMU并映射/重定位内核 -> __primary_switched -> start_kernel
 *
 * 次 CPU 路径：
 *   固件自旋表或 CPU hotplug -> secondary_holding_pen/secondary_entry
 *   -> secondary_startup -> 检查与主 CPU 兼容的 VA/页粒度
 *   -> __cpu_setup -> __enable_mmu -> __secondary_switched
 *   -> 取得主 CPU 发布的 idle task -> secondary_start_kernel
 *
 * 核心状态及生命周期：Image 头只供加载阶段读取；boot_args/__fdt_pointer 把固件
 * 参数跨越 MMU 开关保存到 C 世界；init/idmap 页表只负责安全穿越开 MMU边界，
 * swapper_pg_dir 才是稳定内核页表；early_init_stack 只服务正式 task 栈建立前；
 * secondary_data 由主 CPU 为一次次 CPU bring-up 发布，消费者取走 CPU_BOOT_TASK
 * 后建立自己的 current、栈、SCS 与 per-CPU 基址。
 *
 * 并发与失败模型：主 CPU 早期阶段实际上单线程运行，没有锁、调度和动态内存；
 * 正确性依靠固件启动协议、callee-saved 寄存器、显式 cache 维护与 DMB/ISB。
 * 次 CPU 可并行进入等待区，主 CPU通过 release 变量、事件和共享 boot status
 * 协调；硬件能力不兼容时不能像普通函数返回 errno，只能把原因写入共享状态并
 * 永久停在低功耗循环，防止不兼容 CPU 污染已经运行的系统。
 *
 * 方案权衡：先用恒等映射执行，可让开 MMU 前后的 PC 指向同一物理指令，降低
 * “第一条虚拟地址指令”失败风险；代价是必须维护临时页表、缓存一致性及一次
 * 从 idmap 到高地址内核映射的显式跳转。主/次 CPU 共用异常级和 MMU helper，
 * 保持体系结构状态一致，但每条路径仍需处理不同的参数发布与失败语义。
 */
/*
 * ownership 补充：secondary_data 中的 CPU_BOOT_TASK 是启动控制线程借给次 CPU
 * 的 idle task 指针，不因汇编读取而转移引用或释放责任。“取走”仅表示次 CPU
 * 消费本轮单槽交接值；CPU hotplug 控制线程会在握手结束后清槽，并按启动结果
 * 继续管理 idle task 生命周期。
 */
/*
 * Low-level CPU initialisation
 * Based on arch/arm/kernel/head.S
 *
 * Copyright (C) 1994-2002 Russell King
 * Copyright (C) 2003-2012 ARM Ltd.
 * Authors:	Catalin Marinas <catalin.marinas@arm.com>
 *		Will Deacon <will.deacon@arm.com>
 */

# 这是内核启动的汇编入口文件，负责最早期的 CPU 初始化工作，是内核执行的第一条指令所在的地方。
// ============================================================
// 头文件说明
// ============================================================
// 这些 #include 经过 C 预处理器展开，汇编文件可以使用 C 宏。
// .S 文件（大写）= 先经 cpp 预处理再汇编。
// .s 文件（小写）= 纯汇编，不预处理。

// 提供 SYM_CODE_START / SYM_FUNC_START 等符号定义宏
// 让调试器和链接器能识别函数边界
#include <linux/linkage.h>

// 提供 __HEAD / __INIT 等段（section）标记宏
// 控制代码放入哪个 ELF 段
#include <linux/init.h>

// 提供页表相关的宏和常量（PAGE_OFFSET 等）
#include <linux/pgtable.h>

// 提供指针认证（PAC）相关的汇编宏
// PAC 是 ARMv8.3 的安全特性，防止 ROP 攻击
#include <asm/asm_pointer_auth.h>

// 提供大量汇编辅助宏，如 adr_l / str_l / mov_q 等
// 封装了复杂的地址计算，让代码更可读
#include <asm/assembler.h>

// 提供启动相关的常量，如内存对齐要求
#include <asm/boot.h>

// 提供 ASM_BUG() 宏，触发不可达代码的 panic
#include <asm/bug.h>

// 提供寄存器状态（pt_regs）相关的偏移量常量
#include <asm/ptrace.h>

// 提供结构体成员偏移量（由 C 代码生成的汇编常量）
// 例如 TSK_STACK 是 task_struct.stack 的字节偏移
#include <asm/asm-offsets.h>

// 提供 Cache 行大小等缓存相关常量
#include <asm/cache.h>

// 提供 CPU 类型检测相关的宏和常量
#include <asm/cputype.h>

// 提供 EL2 初始化相关的宏（Hypervisor 模式配置）
#include <asm/el2_setup.h>

// 提供 ELF 格式相关的常量
#include <asm/elf.h>

// 提供内核镜像头部格式（ARM64_IMAGE_MAGIC 等）
// Boot loader 通过这些魔数识别合法的内核镜像
#include <asm/image.h>

// 提供内核页表相关的宏和常量
#include <asm/kernel-pgtable.h>

// 提供 KVM（虚拟化）相关的常量
#include <asm/kvm_arm.h>

// 提供内存布局相关的宏（PAGE_OFFSET / KERNEL_START 等）
#include <asm/memory.h>

// 提供页表硬件格式定义（页表项的 bit 布局）
#include <asm/pgtable-hwdef.h>

// 提供页大小相关的常量（PAGE_SIZE / PAGE_SHIFT 等）
#include <asm/page.h>

// 提供 Shadow Call Stack 相关的宏（控制流保护特性）
#include <asm/scs.h>

// 提供 SMP（多核）相关的常量
#include <asm/smp.h>

// 提供系统寄存器的名字宏，如 SCTLR_ELx_M（MMU 使能位）
// 没有这些宏就要直接写魔数，可读性极差
#include <asm/sysreg.h>

// 提供栈帧（stack frame）格式相关的常量
#include <asm/stacktrace/frame.h>

// 提供线程信息（thread_info）相关的偏移量
#include <asm/thread_info.h>

// 提供虚拟化（EL2/HYP）相关的宏和常量
#include <asm/virt.h>

// 包含 EFI（UEFI 固件）PE/COFF 头部的汇编片段
// 让内核镜像同时是合法的 EFI 可执行文件
#include "efi-header.S"

// ============================================================
// 编译期检查
// ============================================================
// PAGE_OFFSET 是内核虚拟地址空间的起始地址
// 必须 2MB 对齐，因为内核使用 2MB 大页（Block 映射）
// 0x1fffff = 2MB - 1，与运算不为 0 说明未对齐
// 这是编译期检查，违反则直接编译失败，不会等到运行时崩溃
#if (PAGE_OFFSET & 0x1fffff) != 0
#error PAGE_OFFSET must be at least 2MB aligned
#endif

// ============================================================
// 内核镜像头部
// ============================================================
/*
 * 内核启动入口点
 * ---------------------------
 *
 * 进入条件（Boot loader 必须保证）：
 *   MMU = 关闭, D-cache = 关闭, I-cache = 开或关,
 *   x0 = FDT（扁平设备树）blob 的物理地址
 *
 * 注意：callee-saved 寄存器（x19-x28）在 MMU 开启前用于
 * 存储跨函数调用需要保留的变量，具体分配见各入口注释。
 */

// __HEAD 是一个段标记宏，展开为 .section ".head.text","ax"
// 链接脚本确保这个段放在内核镜像的最开头
// Boot loader 跳转到内核物理地址时，第一条执行的就是这里
	__HEAD

/*
 * 这里同时服务两种外部消费者：裸 Image boot loader 按 arm64 boot protocol 读取
 * 固定偏移字段并跳入口；UEFI 把同一字节流当 PE/COFF 文件解析。efi_signature_nop
 * 让首条机器指令的低字节兼作 “MZ” 签名，避免维护两份内核载荷。代价是下面
 * 头字段的顺序、宽度和偏移属于外部 ABI，不能像普通内部结构那样随意调整。
 */

	/*
	 * 不要修改以下内容。
	 * 这是 Boot loader 期望的固定格式镜像头部。
	 * Boot loader 通过这个头部判断镜像是否合法、加载偏移等。
	 */

// efi_signature_nop 展开为一条特殊的 NOP 指令
// 同时在二进制层面是 "MZ" 这两个字节
// "MZ" 是 PE/COFF 可执行文件的魔数（MS-DOS 头部签名）
// 让内核镜像对 UEFI 固件来说是合法的 EFI 应用程序
	efi_signature_nop

// b = Branch（无条件跳转）
// 跳转到 primary_entry 标签，这是真正的内核启动代码
// 头部的其他字段是数据，不能执行，必须跳过
	b	primary_entry

// .quad = 定义一个 64 位（8字节）的数据
// 内核镜像从 RAM 起始地址的加载偏移，小端序
// 0 表示由 Boot loader 决定，或放在 RAM 起始处
	.quad	0

// le64sym = 定义一个 64 位小端序符号引用
// _kernel_size_le 是链接时计算的内核镜像大小
// Boot loader 用这个字段知道要加载多少字节
	le64sym	_kernel_size_le

// 内核标志位（只读信息，不影响启动逻辑）
// 包含大小端、页大小等信息供 Boot loader 参考
	le64sym	_kernel_flags_le

// 三个保留字段，目前未使用，必须为 0
	.quad	0
	.quad	0
	.quad	0

// .ascii = 定义 ASCII 字符串数据（不含终止符）
// ARM64_IMAGE_MAGIC 展开为 "ARM\x64"
// Boot loader 通过这个魔数验证这是 ARM64 内核
	.ascii	ARM64_IMAGE_MAGIC

// .long = 定义一个 32 位数据
// PE 头部的偏移量，供 UEFI 固件解析 PE/COFF 格式使用
	.long	.Lpe_header_offset

// 插入完整的 EFI PE/COFF 头部（来自 efi-header.S）
// 让内核可以直接被 UEFI 固件作为 EFI 应用启动
	__EFI_PE_HEADER

// 切换到 .idmap.text 段
// "idmap" = identity map（恒等映射）
// 这个段的代码在 MMU 开启前后都需要能执行
// 必须建立虚拟地址 = 物理地址的恒等映射
// "a" = allocatable（需要加载到内存）
	.section ".idmap.text","a"

	/*
	 * 主启动路径使用的 callee-saved 寄存器分配表：
	 *
	 *  寄存器   有效范围                              用途
	 *  x19     primary_entry() .. start_kernel()    记录进入时 MMU 是否开启
	 *  x20     primary_entry() .. __primary_switch() CPU 启动模式（EL1/EL2）
	 *  x21     primary_entry() .. start_kernel()    启动时 x0 传入的 FDT 指针
	 *
	 * 使用 callee-saved 寄存器的原因：
	 * 这些值需要跨越多次函数调用保持不变，
	 * 而 callee-saved 寄存器由被调用者负责保存和恢复，
	 * 调用者可以放心使用而不担心被破坏。
	 */

// ============================================================
// primary_entry：主 CPU 启动入口
// ============================================================
// SYM_CODE_START 宏定义一个全局可见的代码符号
// 与 SYM_FUNC_START 的区别：CODE 用于不遵循 ABI 的汇编代码
// 此时 MMU 关闭，Cache 关闭，栈未建立
SYM_CODE_START(primary_entry)

/*
 * primary_entry - 把 boot CPU 从加载器交付状态推进到可开启正式内核的状态。
 *
 * 调用关系：Image 头的第一条分支进入；成功路径不返回加载器，而是尾跳
 * __primary_switch，最终进入 start_kernel()。
 * 入参/前置条件：x0 是 FDT 物理地址且在早期可访问范围；x1-x3 是启动协议保留
 * 参数。常规协议要求 MMU/D-cache 关闭，但 record_mmu_state 也兼容某些包装入口
 * 已开启二者的情况。当前无 task、无正式栈、无异常向量、不能睡眠或分配内存。
 * 输出/副作用：x19 保存入口 MMU 状态，x20 保存 EL/VHE boot mode，x21 保存 FDT；
 * 创建 init idmap，规范化异常级和 CPU 控制寄存器，并由 __cpu_setup 产出供
 * __enable_mmu 使用的 SCTLR/TCR/MAIR 状态。任何致命硬件不兼容都进入停机路径，
 * 不存在返回错误码或资源回滚。
 */

// bl = Branch with Link（带返回地址的跳转，即函数调用）
// 跳转到 record_mmu_state，返回地址存入 lr（x30）
// 目的：检查 Boot loader 传来时 MMU 是否已开启
// 结果存入 x19（callee-saved，后续可安全使用）
	bl	record_mmu_state

/*
 * 阶段 1：冻结加载器交付信息。必须先知道 MMU/cache 状态，后续保存 boot_args
 * 和写页表才能选择正确的 cache clean/invalidate 协议；preserve_boot_args 再把
 * 易失的 x0-x3 写入稳定全局区，并让 x21 跨越后续 helper 保存 FDT。
 */

// 保存 Boot loader 传入的参数（x0-x3）到内存
// x0 = FDT 物理地址（最重要）
// x1/x2/x3 = 通常为 0，保留给未来使用
	bl	preserve_boot_args

// adrp = Address of Page（PC 相对寻址，取页地址）
// 语法：adrp 目标寄存器, 符号名
// 计算 early_init_stack 的页地址（4KB 对齐）存入 x1
// adrp 只能寻址 ±4GB 范围，适合内核内部地址引用
// 注意：此时还没有栈，所有局部状态只能存寄存器
	adrp	x1, early_init_stack

// mov sp, x1：将 x1 的值写入栈指针寄存器 SP
// 建立早期启动栈，之后才能调用更复杂的 C 风格函数
// early_init_stack 是链接脚本预留的一块内存区域
	mov	sp, x1

// xzr = 零寄存器，读取永远返回 0，写入丢弃
// x29 = 帧指针（Frame Pointer）
// 置零帧指针表示这是调用栈的最底部（无父帧）
// 调试器展开调用栈时遇到 x29=0 会停止
	mov	x29, xzr

/*
 * 阶段 2：建立开 MMU 所需的 init idmap。early_init_stack 与 init 页目录均由
 * 链接脚本预留，因此此时不依赖尚未启动的页分配器。返回 x0 是实际使用的页表
 * 末端，用它限定 cache 维护范围，避免处理无关内存。
 */

// 获取初始恒等映射页目录的地址
// __pi_create_init_idmap 的第一个参数（x0）= 页目录起始地址
	adrp	x0, __pi_init_idmap_pg_dir

// x1 = 0（第二个参数）
// 在 __pi_create_init_idmap 中表示无额外标志
	mov	x1, xzr

// 调用 __pi_create_init_idmap 建立早期恒等映射页表
// 恒等映射 = 虚拟地址 == 物理地址
// 目的：开启 MMU 的瞬间，当前 PC 的地址能被正确翻译
// 如果没有这个映射，开启 MMU 后下一条指令的地址翻译就会失败
// 返回值 x0 = 页表使用区域的结束地址
	bl	__pi_create_init_idmap

	/*
	 * 如果页表是在 MMU 关闭状态下写入的（非缓存访问），
	 * 需要使这些页表的 Cache 行失效，
	 * 清除任何推测性预取（speculative prefetch）加载的缓存内容。
	 *
	 * 背景：MMU 关闭时写内存不经过 Cache，
	 * 但 CPU 可能已经推测性地把这块内存预取到了 Cache，
	 * Cache 里的旧数据和刚写的新数据不一致，
	 * 必须让 Cache 失效，强制下次从内存重新读取。
	 */

// cbnz = Compare and Branch if Not Zero
// 语法：cbnz 寄存器, 标签
// x19 != 0 说明进入时 MMU 已开启，跳转到标签 0f
// 0f 中 f = forward（向前找标签 0:）
// MMU 已开启时 Cache 也是开启的，走不同的处理路径
	cbnz	x19, 0f

/*
 * 阶段 3A（入口 MMU/cache 关闭）：页表写直接到一致性点，但 CPU 可能曾把旧内存
 * 推测读入 cache；先 DMB 完成写，再 invalidate 对应范围，保证随后 table walker
 * 不会命中陈旧 cache line。这里不是“释放页表”，只是在建立硬件可见性。
 */

// dmb sy = Data Memory Barrier, System（全系统内存屏障）
// 确保在这条指令之前的所有内存写入都已完成
// 在执行 Cache 无效化操作前必须确保数据已写入内存
// 否则 Cache 失效后重新从内存读，读到的可能还是旧数据
	dmb     sy

// 把 x0（页表结束地址）移到 x1 作为范围的结束地址
	mov	x1, x0

// 重新获取页表起始地址作为范围的开始
	adrp    x0, __pi_init_idmap_pg_dir

// adr_l = 获取符号的运行时地址（支持更大范围的 PC 相对寻址）
// 与 adrp 不同，adr_l 可以获取精确地址而非页对齐地址
// dcache_inval_poc 是一个函数：使指定范围的 D-Cache 失效到 PoC
// PoC = Point of Coherency（一致性点，通常是主内存）
	adr_l	x2, dcache_inval_poc

// blr = Branch with Link to Register
// 通过寄存器中的地址调用函数（间接调用）
// 调用 dcache_inval_poc(x0=起始地址, x1=结束地址)
// 让这段页表内存的 Cache 行全部失效
	blr	x2

// b = 无条件跳转，跳过 MMU 已开启时的处理路径
	b	1f

	/*
	 * 如果进入时 MMU 和 Cache 都已开启，
	 * 需要把 ID 映射代码段 clean 到 PoC，
	 * 这样关闭 MMU 后执行这段代码时能读到正确内容。
	 *
	 * 背景：MMU 开启时写入的数据在 Cache 中，
	 * 关闭 MMU 后直接访问物理内存，如果 Cache 没有刷回内存，
	 * 物理内存里的数据可能是旧的。
	 */

// 标签 0:（被上面的 0f 引用）
// 获取 idmap 代码段的起始和结束地址
0:	adrp	x0, __idmap_text_start
	adr_l	x1, __idmap_text_end

/*
 * 阶段 3B（入口 MMU/cache 开启）：稍后规范化 EL 时可能关闭 MMU，届时 CPU 将以
 * 物理地址执行 .idmap.text；先把该段 dirty cache line clean 到 PoC，才能保证
 * 关闭地址翻译后仍取到当前镜像指令。该路径维护代码而非刚创建的页表。
 */

// dcache_clean_poc：把指定范围的 D-Cache 清理（写回）到 PoC
// Clean 和 Invalidate 的区别：
//   Clean（清理）= 把 dirty Cache 行写回内存，但不删除 Cache 行
//   Invalidate（失效）= 标记 Cache 行无效，但不写回
//   Clean + Invalidate = 写回后再标记无效
	adr_l	x2, dcache_clean_poc
	blr	x2

// 标签 1:（两条路径在这里汇合）
// x19 中保存了之前记录的 MMU 状态
1:	mov	x0, x19

/*
 * 阶段 4：把各固件可能留下的 EL1/EL2 状态收敛为 Linux 约定。init_kernel_el
 * 返回启动模式供虚拟化层记录；__cpu_setup 根据已探测 CPU 实现配置翻译控制，
 * 最终不再返回本入口，而由 __primary_switch 执行不可逆的 MMU/地址空间切换。
 */

// 调用 init_kernel_el 初始化内核执行级别
// 根据当前处于 EL1 还是EL2，做相应的系统寄存器配置
// w0 = cpu_boot_mode（返回值，32位），表示从哪个 EL 启动
	bl	init_kernel_el			// w0=cpu_boot_mode

// 把 init_kernel_el 的返回值保存到 x20（callee-saved）
// x20 将一直保留 CPU 启动模式直到 __primary_switch
	mov	x20, x0

	/*
	 * 调用 CPU 设置代码，详见 arch/arm64/mm/proc.S
	 * 返回后 CPU 已准备好开启 MMU，TCR 寄存器已配置。
	 * TCR（Translation Control Register）控制页表的各种参数：
	 *   - 虚拟地址位数（48位或52位）
	 *   - 页大小（4KB/16KB/64KB）
	 *   - TTBR0/TTBR1 分别管理的地址范围
	 */
	bl	__cpu_setup			// initialise processor

// b = 无条件跳转（不保存返回地址，相当于 goto）
// 跳转到 __primary_switch，开启 MMU
	b	__primary_switch

// SYM_CODE_END 标记函数结束，提供给调试器和链接器
SYM_CODE_END(primary_entry)

// ============================================================
// 热插拔和次级 CPU 代码从这里开始
// ============================================================
// __INIT 段在内核初始化完成后会被释放以节省内存
	__INIT

/*
 * 从这里起代码标为 init 生命周期：它在启动/CPU bring-up 阶段可执行，系统稳定后
 * 主内核可回收相应 init 内存。永久运行路径不能保存指向这些指令或只在本段存活
 * 数据的长期引用；CPU hotplug 所需入口则由链接布局和专用保留规则保证仍可调用。
 */

// ============================================================
// record_mmu_state：记录进入时的 MMU 状态
// ============================================================
// SYM_CODE_START_LOCAL = 局部符号，只在本文件可见
SYM_CODE_START_LOCAL(record_mmu_state)

/*
 * record_mmu_state - 识别并规范化加载器留下的地址翻译、cache 与端序状态。
 *
 * 调用者仅为 primary_entry；入口没有可用内存栈，x19 是跨后续调用保存的输出。
 * 正常端序下，只有“MMU 与 D-cache 同时开启”才返回非零，作为后续 cache 维护
 * 协议的分流条件；任一关闭都按早期非缓存路径处理。若当前端序与内核编译端序
 * 不同，本函数先按 CPU erratum 要求关闭 MMU、修正 EE 并 ISB，同样返回 0。
 * 它不建立页表，也不证明加载器页表可继续作为 Linux 页表使用。
 */

// mrs = Move to Register from System register
// 语法：mrs 目标通用寄存器, 系统寄存器
// CurrentEL 是一个只读系统寄存器，存储当前异常级别
// 读取到 x19，判断当前在 EL1 还是 EL2
	mrs	x19, CurrentEL

// cmp = Compare（实际是做减法，只更新标志位，不保存结果）
// CurrentEL_EL2 是 EL2 的编码值
// 比较当前 EL 是否等于 EL2
	cmp	x19, #CurrentEL_EL2

// 读取 SCTLR_EL1（System Control Register for EL1）
// SCTLR 控制 MMU、Cache、对齐检查等
// 先读 EL1 的值，下面根据实际 EL 决定是否改读 EL2 的
	mrs	x19, sctlr_el1

// b.ne = Branch if Not Equal（条件跳转，条件：不相等）
// 如果不在 EL2，跳转到标签 0f（即不需要读 EL2 的 SCTLR）
	b.ne	0f

// 走到这里说明当前在 EL2
// 改读 SCTLR_EL2，覆盖刚才读的 SCTLR_EL1
	mrs	x19, sctlr_el2

// 标签 0:
0:
// CPU_LE / CPU_BE 是大小端条件编译宏
// 只有对应端序的编译才会包含其中的指令

// CPU_LE：小端模式下编译
// tbnz = Test Bit and Branch if Not Zero
// 语法：tbnz 寄存器, 位号, 标签
// 测试 x19 的 SCTLR_ELx_EE_SHIFT 位（EE = Exception Endianness）
// EE=1 表示大端，大端系统跳转到标签 1f 处理字节序问题
CPU_LE( tbnz	x19, #SCTLR_ELx_EE_SHIFT, 1f	)

// CPU_BE：大端模式下编译
// tbz = Test Bit and Branch if Zero
// 大端系统中 EE=0 才是问题（期望大端但设置了小端）
CPU_BE( tbz	x19, #SCTLR_ELx_EE_SHIFT, 1f	)

// tst = Test（AND 运算，只更新标志位）
// SCTLR_ELx_C = Cache 使能位（C bit）
// 检查 Cache 是否开启，结果影响 Z 标志位
// Z=1 表示 C=0（Cache 关闭），Z=0 表示 C=1（Cache 开启）
	tst	x19, #SCTLR_ELx_C		// Z := (C == 0)

// and = 按位与，提取 M 位（MMU 使能位）到 x19
// 丢弃 SCTLR 中其他所有位，只保留 M 位
	and	x19, x19, #SCTLR_ELx_M		// isolate M bit

// csel = Conditional Select（条件选择）
// 语法：csel 目标, 真值, 假值, 条件
// 条件 eq（Z=1）成立（Cache 关闭）时：x19 = xzr = 0
// 条件不成立（Cache 开启）时：x19 = x19（保持 M 位）
// 业务逻辑：如果 Cache 关闭，即使 MMU 开着也当没开处理
// 因为 Cache 关闭时内存一致性由硬件保证，处理方式不同
	csel	x19, xzr, x19, eq		// clear x19 if Z

/*
 * 这里把 M 与 C 合并成单一业务状态，不是丢失信息：后续真正关心的是早期写入
 * 是否可能停留在 cache、以及关闭 MMU 前是否需要 clean idmap 代码。C=0 时按
 * 非缓存可见性路径处理更安全，即使 M 位仍为 1 也无需保留该区别。
 */

// ret = Return（跳转到 lr 寄存器中的地址）
// 返回 primary_entry，x19 中存储了 MMU 状态
// x19=0：MMU 关闭或 Cache 关闭
// x19=1（M位）：MMU 和 Cache 都开启
	ret

	/*
	 * 尽早设置正确的字节序，使得 init_kernel_el() 之前的
	 * 所有内存访问都以正确的字节序进行。
	 * 注意：此时 MMU 必须关闭，否则当前 ID 映射会以错误
	 * 的字节序解析，导致翻译错误。
	 */

// 标签 1:（字节序不匹配时跳转到这里）
// eor = Exclusive OR（异或）
// 翻转 EE 位来修正字节序
1:	eor	x19, x19, #SCTLR_ELx_EE

/*
 * 端序不匹配是必须在第一次依赖 C 数据布局前修复的全局契约。页表描述符本身也
 * 按 CPU 当前端序解释，所以不能在错误端序且 MMU 开启时继续执行；先清 M 位，
 * 再分别写回当前 EL 的 SCTLR，并以 ISB 让后续取指/系统寄存器观察新状态。
 */

// bic = Bit Clear（按位清除）
// 语法：bic 目标, 源, 掩码
// 清除 M 位（关闭 MMU），确保修改字节序时 MMU 是关闭的
	bic	x19, x19, #SCTLR_ELx_M

// b.ne：不相等时跳转（仍由之前的 cmp CurrentEL == EL2 决定）
// 不在 EL2，跳转到标签 2f 处理 EL1 的情况
	b.ne	2f

// pre_disable_mmu_workaround 是一个宏
// 在某些 CPU 型号上，关闭 MMU 前需要特殊处理
// 具体内容取决于 CPU 的勘误（erratum）
	pre_disable_mmu_workaround

// msr = Move to System register from Register
// 把修正后的值写回 SCTLR_EL2（修正字节序，确保 MMU 关闭）
	msr	sctlr_el2, x19

// 跳转到标签 3f，共同的后处理
	b	3f

// 标签 2:（处于 EL1 的情况）
2:	pre_disable_mmu_workaround
	msr	sctlr_el1, x19

// 标签 3:（字节序修正后的共同出口）
3:	isb

// 字节序发生了变化，MMU 状态重置为"未开启"
// 因为刚才我们强制关闭了 MMU（bic M 位），所以 x19=0
	mov	x19, xzr
	ret
SYM_CODE_END(record_mmu_state)

// ============================================================
// preserve_boot_args：保存 Boot loader 传入的参数
// ============================================================
/*
 * 保存 Boot loader 在 x0..x3 中传入的参数
 * ARM64 Linux 启动协议规定：
 *   x0 = FDT 物理地址
 *   x1 = 保留（必须为 0）
 *   x2 = 保留（必须为 0）
 *   x3 = 保留（必须为 0）
 */
SYM_CODE_START_LOCAL(preserve_boot_args)

/*
 * preserve_boot_args - 把固件寄存器协议转化为可由后续 C 代码检查的持久快照。
 *
 * x0-x3 是借来的启动参数值，没有指针 ownership 转移；x21 单独保存 FDT 物理
 * 地址，boot_args[] 保存原始四元组供协议校验和诊断。入口 x19 决定写入后的
 * cache 维护：非缓存路径尾调用 invalidate，缓存路径记录 mmu_enabled_at_boot。
 * 函数不解析 FDT、不验证 x1-x3 是否为 0，也不分配内存。
 */

// 把 x0（FDT 地址）保存到 x21（callee-saved 寄存器）
// x21 将在整个启动过程中保持 FDT 地址，直到传给 start_kernel
	mov	x21, x0				// x21=FDT

// adr_l 获取 boot_args 数组的地址
// boot_args 是内核中的全局数组，用于记录启动参数
	adr_l	x0, boot_args

// stp = Store Pair（一次存储两个 64 位寄存器）
// 语法：stp 寄存器1, 寄存器2, [基址]
// 把 x21（FDT）和 x1 存入 boot_args[0] 和 boot_args[1]
	stp	x21, x1, [x0]

// 把 x2 和 x3 存入 boot_args[2] 和 boot_args[3]
// [x0, #16] 表示 x0+16 字节处（2 个 64 位 = 16 字节偏移）
	stp	x2, x3, [x0, #16]

/*
 * 这 32 字节是从“固件拥有的寄存器状态”到“内核拥有的静态存储”的一次复制。
 * 后续 helper 可以自由破坏 x0-x3，而诊断代码仍可看到加载器原始输入。
 */

// 如果 MMU 已开启，跳过 Cache 失效操作
// x19 = MMU 状态，非 0 说明 MMU 开启
	cbnz	x19, 0f

// dmb sy：全系统数据内存屏障
// 确保上面的 stp 写入操作在 dc ivac 之前完成
// 否则 Cache 失效后重读可能得到失效前的旧数据
	dmb	sy

// add x1, x0, #0x20：计算 boot_args 结束地址
// 0x20 = 32 字节 = 4 个 64 位寄存器的大小
	add	x1, x0, #0x20			// 4 x 8 bytes

// b = 无条件跳转（尾调用优化）
// 直接跳转到 dcache_inval_poc 而不是 bl 调用
// 因为 dcache_inval_poc 返回后我们也要返回，共用同一个 ret
// 参数：x0=boot_args起始地址，x1=结束地址
	b	dcache_inval_poc		// tail call

// 标签 0:（MMU 已开启时走这里）
// str_l = 存储到较远的地址（封装了 adrp+str 的组合）
// 把 x19（MMU 状态）保存到 mmu_enabled_at_boot 变量
// 记录启动时 MMU 是否开启，供后续代码查询
0:	str_l   x19, mmu_enabled_at_boot, x0

/*
 * 只有入口确实以 MMU+cache 开启时才写此全局标志；它记录启动环境事实，而非
 * 当前 MMU 状态（后续会重配）。已经在缓存一致访问下完成写入，因此无需走
 * 非缓存分支的 invalidate 尾调用。
 */
	ret
SYM_CODE_END(preserve_boot_args)

// ============================================================
// init_cpu_task：初始化 CPU 任务上下文（汇编宏）
// ============================================================
	/*
	 * 用任务特定和 CPU 特定的上下文初始化 CPU 寄存器。
	 *
	 * 在 task_pt_regs(current)->stackframe 创建最终帧记录，
	 * 这样 unwinder（栈回溯工具）可以通过帧在任务栈中的
	 * 位置来识别任何任务的最终帧记录。
	 * 为与用户任务和 kthreads 保持一致，保留整个 pt_regs 空间。
	 */

// .macro = 定义汇编宏
// 语法：.macro 宏名 参数1, 参数2, ...
// \tsk = 任务结构体指针，\tmp1 \tmp2 = 临时寄存器
	.macro	init_cpu_task tsk, tmp1, tmp2

/*
 * init_cpu_task - 将一个已由通用代码准备好的 task_struct 绑定为本 CPU 的 current。
 *
 * tsk 是借用且必须有效的 idle/init task 指针；tmp1/tmp2 被破坏。调用时 MMU 已
 * 开启、task 内核栈和 __per_cpu_offset 已初始化，但尚不能依赖 current/per-CPU
 * 快速访问。宏依次发布 SP_EL0=current、切换任务栈、构造最终回溯帧、恢复 SCS，
 * 最后安装本 CPU offset；无返回值和失败分支。顺序不能倒置：per-CPU/SCS helper
 * 需要先有可信 task 与栈，而进入 C 代码前四者必须共同成立。
 */

// msr sp_el0, \tsk：把任务结构体地址写入 SP_EL0
// SP_EL0 在内核态用作 current 指针（指向当前任务的 task_struct）
// ARM64 Linux 的一个设计：借用 EL0 的栈指针寄存器存 current
	msr	sp_el0, \tsk

// 从 task_struct 中读取内核栈地址
// TSK_STACK 是 task_struct.stack 的字节偏移（由 asm-offsets 提供）
	ldr	\tmp1, [\tsk, #TSK_STACK]

// 计算栈顶地址：栈底 + THREAD_SIZE = 栈顶
// ARM64 栈向低地址增长，所以栈顶是高地址端
	add	sp, \tmp1, #THREAD_SIZE

// 在栈顶预留 pt_regs 空间
// PT_REGS_SIZE = sizeof(struct pt_regs)
	sub	sp, sp, #PT_REGS_SIZE

// stp xzr, xzr：在栈帧位置存入两个零值
// S_STACKFRAME 是 pt_regs 中 stackframe 成员的偏移
// 初始化为零，表示这是调用链的终点
	stp	xzr, xzr, [sp, #S_STACKFRAME]

// 设置栈帧类型为 FINAL（表示调用链终点）
	mov	\tmp1, #FRAME_META_TYPE_FINAL
	str	\tmp1, [sp, #S_STACKFRAME_TYPE]

// x29（帧指针）指向当前栈帧
	add	x29, sp, #S_STACKFRAME

// scs_load_current：加载 Shadow Call Stack 指针
// Shadow Call Stack 是防 ROP 攻击的安全特性
// 把真实返回地址存在独立的影子栈中，攻击者无法篡改
	scs_load_current

// 加载当前 CPU 的 per-cpu 偏移量
// Per-CPU 变量：每个 CPU 有独立副本，通过基址+偏移访问
// __per_cpu_offset 是偏移量数组，每个 CPU 对应一项
	adr_l	\tmp1, __per_cpu_offset

// TSK_TI_CPU = task_struct.cpu 的偏移，获取当前 CPU 编号
// w\tmp2 表示用 32 位寄存器（CPU 编号不需要 64 位）
	ldr	w\tmp2, [\tsk, #TSK_TI_CPU]

// 从偏移量数组中取当前 CPU 的偏移值
// \tmp2, lsl #3 = tmp2 * 8（64 位元素的数组索引）
	ldr	\tmp1, [\tmp1, \tmp2, lsl #3]

// set_this_cpu_offset：设置当前 CPU 的 per-cpu 基址
// 后续通过特殊指令访问 per-cpu 变量时用到这个基址
	set_this_cpu_offset \tmp1

/*
 * 宏完成后的业务不变量：current 指向 tsk，SP 位于 tsk 的内核栈，栈展开有明确
 * 终点，SCS 属于该任务，this_cpu 操作落到 tsk->cpu 对应的副本。此时才具备
 * 调用依赖普通 arm64 执行上下文的 C 初始化函数的基础。
 */

// .endm = 宏定义结束
	.endm

// ============================================================
// __primary_switched：MMU 开启后的主 CPU 初始化
// ============================================================
/*
 * 以下代码在 MMU 开启后执行。
 *
 *   x0 = __pa(KERNEL_START)（内核起始物理地址）
 */
SYM_FUNC_START_LOCAL(__primary_switched)

/*
 * __primary_switched - 主 CPU 在正式高地址内核映射中的第一个稳定运行阶段。
 *
 * __primary_switch 以 x0=KERNEL_START 物理地址间接跳入；x20/x21 仍分别保存 boot
 * mode 和 FDT。入口 MMU 已开且内核已完成早期映射/可能的 KASLR 重定位，但还没有
 * current、正式异常向量或完整 C 子系统。函数建立 init_task 上下文，发布 FDT
 * 与 kimage_voffset，记录/最终化 EL2，再调用不返回的 start_kernel()。所有对象
 * 都是静态早期对象，不涉及引用计数、锁、睡眠或失败回滚。
 */

// 加载 init_task（内核第一个进程，PID=0，手工构造）的地址
	adr_l	x4, init_task

// 用 init_cpu_task 宏初始化 CPU 任务上下文
// 建立内核栈、设置 current 指针、初始化帧记录
	init_cpu_task x4, x5, x6

/*
 * 阶段 1：把 boot CPU 绑定到静态 init_task（后续演化为 CPU0 idle task），从
 * early_init_stack 切换到该任务真正的内核栈。此后异常和 C 调用才有可回溯、
 * 可定位 current 的执行上下文。
 */

// 加载异常向量表地址到 x8
// vectors 是在 entry.S 中定义的 ARM64 异常向量表
	adr_l	x8, vectors

// msr vbar_el1, x8：把向量表地址写入 VBAR_EL1
// VBAR = Vector Base Address Register
// 从这里开始，所有 EL1 的异常（包括系统调用、缺页）
// 都会跳转到这个向量表中对应的处理函数
	msr	vbar_el1, x8			// vector table address

// isb：确保 VBAR_EL1 立即生效，后续的异常使用新向量表
	isb

/*
 * 阶段 2：发布异常入口。VBAR 写入并 ISB 后，早期故障不再落到固件未知向量，
 * 而会进入 entry.S；这必须早于越来越复杂、可能触发缺页或调试异常的 C 初始化。
 */

// stp x29, x30, [sp, #-16]!
// 在栈上保存帧指针（x29）和返回地址（x30/lr）
// ! 表示先更新 sp（sp -= 16），然后存储
// 这是标准的函数序言（prologue），建立调用帧
	stp	x29, x30, [sp, #-16]!

// 更新帧指针指向当前帧
	mov	x29, sp

// 把 FDT 地址（x21）保存到全局变量 __fdt_pointer
// 后续 C 代码通过这个全局变量访问设备树
	str_l	x21, __fdt_pointer, x5		// Save FDT pointer

// adrp x4, _text：获取内核 .text 段的虚拟地址
	adrp	x4, _text

// 计算虚拟地址和物理地址的偏移量
// x0 = 内核起始物理地址（函数参数）
// x4 - x0 = 虚拟地址 - 物理地址 = 偏移量
	sub	x4, x4, x0

// 保存偏移量到 kimage_voffset 全局变量
// 内核在运行时用这个值进行虚拟地址和物理地址的互换
	str_l	x4, kimage_voffset, x5		// physical mappings

/*
 * 阶段 3：把加载期输入转成全局地址模型。__fdt_pointer 让 setup_arch() 能找到
 * 硬件描述；kimage_voffset 记录线性关系“内核链接虚拟地址 - 实际加载物理地址”，
 * 为内核镜像专用的 __pa_symbol/地址转换提供依据，尤其覆盖重定位后的装载位置。
 */

// 把 CPU 启动模式（x20）传给 set_cpu_boot_mode_flag
// 记录是从 EL1 还是 EL2 启动，供后续虚拟化代码判断
	mov	x0, x20
	bl	set_cpu_boot_mode_flag

// 如果配置了 KASAN（内核地址消毒器，用于检测内存 bug），
// 进行 KASAN 的早期初始化
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
	bl	kasan_early_init
#endif

// 尝试最终化 EL2 配置（如果支持则优先使用 VHE 模式）
// VHE = Virtualization Host Extensions，让宿主 OS 运行在 EL2
// 可以减少虚拟机切换开销
	mov	x0, x20
	bl	finalise_el2

/*
 * 阶段 4：set_cpu_boot_mode_flag 让系统比较各 CPU 启动 EL 是否一致；KASAN 需在
 * 大量 C 内存访问前建立最早 shadow；finalise_el2 决定保留 VHE 宿主能力还是
 * 完成 nVHE 交接。它们都必须在通用 start_kernel 展开子系统初始化前完成。
 */

// 恢复帧指针和返回地址
	ldp	x29, x30, [sp], #16

// 跳转到 start_kernel！
# 开始执行 init/main.c 中的 start_kernel 函数
// 这是从汇编进入 C 世界的关键一跳
// 之后的所有内核初始化都在 C 代码中完成
	bl	start_kernel

/*
 * 这是所有权边界：从此由通用内核负责内存、调度、IRQ、时间和驱动生命周期；
 * 早期汇编不再回收临时资源，相关 init 段稍后由通用初始化流程统一释放。
 */

# 应该永远不会执行到这里
// ASM_BUG()：如果 start_kernel 返回则触发 panic
// start_kernel 永远不应该返回，返回说明出现了严重错误
# ASM_BUG 会展开为一条非法指令或 BRK，触发 CPU 异常，若 start_kernel 意外返回（代表出现了严重的内核 bug），CPU 立刻触发异常，内核 panic，而不是跑飞到未定义的代码区域造成更难排查的问题
	ASM_BUG()
SYM_FUNC_END(__primary_switched)

// ============================================================
// 次级 CPU 相关代码
// ============================================================
/*
 * 次级 CPU 进入内核的代码段
 * 主 CPU 完成初始化后，次级 CPU 从这里开始执行
 */
	.section ".idmap.text","a"

// ============================================================
// init_kernel_el：初始化内核执行级别
// ============================================================
/*
 * 从 EL2 或 EL1 启动，将 CPU 配置为内核支持的最高可达 EL。
 * 如果从 EL2 降级到 EL1，先配置 EL2 再配置 EL1。
 *
 * 由于不能总是依赖 ERET 同步系统寄存器的写入，
 * 在 ERET 之前放置 ISB。
 *
 * 返回值：
 *   x0 = BOOT_CPU_MODE_EL1 或 BOOT_CPU_MODE_EL2
 *   高 32 位可能包含上下文标志
 *
 * x0 入参：是否从主启动路径以 MMU 开启状态调用
 */
SYM_FUNC_START(init_kernel_el)

/*
 * init_kernel_el - 把固件提供的 EL1/EL2 环境收敛为 Linux 可接管的异常级状态。
 *
 * 主 CPU 与每个次 CPU 都调用。x0 仅表示入口是否处于缓存映射环境，供 EL2 路径
 * 决定是否 clean HYP idmap；返回 x0 的低位是 BOOT_CPU_MODE_EL1/EL2，高位可带
 * E2H/VHE 上下文标志。函数在 MMU 关闭的 idmap 代码中运行，不使用栈、不睡眠、
 * 不分配资源。它重置当前 EL 的 SCTLR/HCR/向量及返回 PSTATE，通过 ERET 回到
 * 调用者；这既完成异常级/状态切换，也保证后续 __cpu_setup 面对统一基线。
 */

// 读取当前异常级别
	mrs	x1, CurrentEL

// 判断是否在 EL2
	cmp	x1, #CurrentEL_EL2

// 如果在 EL2，跳转到 init_el2 分支
	b.eq	init_el2

// ============================================================
// init_el1：从 EL1 启动的初始化
// ============================================================
// SYM_INNER_LABEL：函数内部的局部标签（可被外部引用）
SYM_INNER_LABEL(init_el1, SYM_L_LOCAL)

/*
 * EL1 直启通常表示固件已保留或未提供 EL2。Linux不能凭空提升到 EL2，因此只把
 * EL1 控制寄存器重置为 MMU-off 基线，并构造一次“返回同一 EL1”的 ERET，使 DAIF
 * 等 PSTATE 同时进入内核要求的屏蔽状态。虚拟化能力边界由固件交付决定。
 */

// mov_q：把 64 位立即数加载到寄存器（需要多条指令实现）
// INIT_SCTLR_EL1_MMU_OFF：MMU 关闭状态下 SCTLR_EL1 的初始值
// 包含：关闭 MMU、关闭 Cache、设置正确的字节序等
	mov_q	x0, INIT_SCTLR_EL1_MMU_OFF
	pre_disable_mmu_workaround

// 写入 SCTLR_EL1，确保 CPU 处于已知的干净状态
	msr	sctlr_el1, x0
	isb

// INIT_PSTATE_EL1：EL1 的初始处理器状态
// 设置 DAIF（Debug/Abort/IRQ/FIQ 屏蔽位）等
	mov_q	x0, INIT_PSTATE_EL1

// spsr_el1 = Saved Program Status Register for EL1
// ERET 返回时会把这个值恢复到 PSTATE
	msr	spsr_el1, x0

// elr_el1 = Exception Link Register for EL1
// ERET 返回时跳转到这个地址
// lr = 调用者的返回地址，ERET 后继续执行 bl init_kernel_el 的下一条
	msr	elr_el1, lr

// 设置返回值：从 EL1 启动
	mov	w0, #BOOT_CPU_MODE_EL1

// eret = Exception Return（异常返回）
// 从 EL2 降级到 EL1，或从 EL1 返回
// 效果：PC = ELR_EL1，PSTATE = SPSR_EL1
// 这里用 eret 代替 ret 是为了同时切换 EL 和更新 PSTATE
	eret

// ============================================================
// init_el2：从 EL2 启动的初始化
// ============================================================
SYM_INNER_LABEL(init_el2, SYM_L_LOCAL)

/*
 * EL2 入口给 Linux 两种业务选择：在支持 VHE 时让宿主保留高效 EL2 运行语义，
 * 否则建立 nVHE 所需的最小 EL2 状态并让常规内核运行在 EL1。此阶段先清理可能
 * 仍只存在 cache 的 HYP idmap，再关闭 EL2 MMU并初始化 HCR/系统寄存器/存根向量，
 * 避免继承固件的陷入策略、页表或异常处理器。
 */

// 设置 ERET 返回地址为调用者的返回地址
	msr	elr_el2, lr

// 如果以 MMU 关闭状态进入（x0=0），跳过 HYP 代码清理
	cbz	x0, 0f

// MMU 开启时需要把 HYP 代码段 clean 到 PoC
// 原因同 preserve_boot_args 处的解释
	adrp	x0, __hyp_idmap_text_start
	adr_l	x1, __hyp_text_end
	adr_l	x2, dcache_clean_poc
	blr	x2

// 配置 EL2 的系统控制寄存器（关闭 MMU）
	mov_q	x0, INIT_SCTLR_EL2_MMU_OFF
	pre_disable_mmu_workaround
	msr	sctlr_el2, x0
	isb

// 标签 0:（两条路径汇合）
0:
// init_el2_hcr：初始化 HCR_EL2（Hypervisor Configuration Register）
// HCR_HOST_NVHE_FLAGS：非 VHE 宿主模式标志
// HCR_ATA：允许 EL1 访问标签（用于内存标签扩展 MTE）
	init_el2_hcr	HCR_HOST_NVHE_FLAGS | HCR_ATA

// 初始化 EL2 的各系统寄存器到安全的默认值
	init_el2_state

/*
 * 这不是完整 KVM 初始化，只是建立可安全继续启动的 EL2 ownership 边界：固件
 * 配置不再有效，Linux 的 HCR 和寄存器默认值成为权威；真正的虚拟机状态、每 CPU
 * HYP 数据及 stage-2 页表要到 KVM 子系统初始化后才创建。
 */

	/* 安装 Hypervisor 存根向量表 */
// __hyp_stub_vectors：最小化的 EL2 异常向量表
// 主要用于处理从 EL1 发来的 HVC（Hypervisor Call）指令
	adr_l	x0, __hyp_stub_vectors
	msr	vbar_el2, x0
	isb

// 准备 EL1 的初始 SCTLR 值
	mov_q	x1, INIT_SCTLR_EL1_MMU_OFF

// 检查是否支持 VHE（Virtualization Host Extensions）
// hcr_el2 的 E2H 位（bit 34）为 1 表示支持 VHE
	mrs	x0, hcr_el2
	and	x0, x0, #HCR_E2H
	cbz	x0, 2f

	/* 以 VHE 方式设置 SCTLR_EL1 */
// msr_s = 访问扩展系统寄存器（非标准寄存器名）
// SYS_SCTLR_EL12：在 VHE 模式下通过 EL1/2 别名访问 SCTLR_EL1
	msr_s	SYS_SCTLR_EL12, x1

// 记录 VHE 标志
	mov	x2, #BOOT_CPU_FLAG_E2H
	b	3f

// 标签 2:（非 VHE 路径）
2:
	msr	sctlr_el1, x1
	mov	x2, xzr

// 标签 3:（两条路径汇合）
3:
// 设置 EL1 的初始处理器状态
	mov	x0, #INIT_PSTATE_EL1
	msr	spsr_el2, x0

// 返回值：从 EL2 启动
// orr：把 VHE 标志位合并到返回值中
	mov	w0, #BOOT_CPU_MODE_EL2
	orr	x0, x0, x2

/*
 * 返回值同时携带“从 EL2 交付”和“当前 E2H 语义”两个事实。后续代码用它检查
 * 所有 CPU 是否一致，并由 finalise_el2 完成不能在 MMU-off 阶段处理的部分。
 */
	eret
SYM_FUNC_END(init_kernel_el)

// ============================================================
// secondary_holding_pen：次级 CPU 等待区
// ============================================================
	/*
	 * 这是一个"等待区"，次级 CPU 在主 CPU 准备好之前
	 * 在这里自旋等待。主 CPU 向 secondary_holding_pen_release
	 * 写入特定值来释放对应的次级 CPU。
	 */
SYM_FUNC_START(secondary_holding_pen)

/*
 * secondary_holding_pen - 实现旧式 spin-table CPU enable-method 的固件等待协议。
 *
 * 每个次 CPU 独立进入，先规范化本 CPU 异常级，再用 MPIDR 硬件 ID等待主 CPU
 * 在 secondary_holding_pen_release 发布相同值。共享变量由主 CPU 串行选择目标，
 * WFE 只降功耗，正确性仍依靠循环重新读取和比较；SEV 可能虚假/广播唤醒，因此
 * 不能把一次唤醒等价为“本 CPU 已获释放”。匹配后尾跳 secondary_startup。
 */

// xzr 作为参数 x0=0，表示不是从 MMU 开启的路径进入
	mov	x0, xzr
	bl	init_kernel_el			// w0=cpu_boot_mode

// mrs x2, mpidr_el1：读取 MPIDR（多处理器亲和性寄存器）
// 包含 CPU 的拓扑 ID（Cluster ID、Core ID 等）
	mrs	x2, mpidr_el1

// 提取硬件 CPU ID（屏蔽保留位）
	mov_q	x1, MPIDR_HWID_BITMASK
	and	x2, x2, x1

// 加载"释放"变量的地址
	adr_l	x3, secondary_holding_pen_release

// pen: 自旋等待标签
// ldr x4, [x3]：读取释放变量的当前值
pen:	ldr	x4, [x3]

/*
 * release 值表达的是硬件拓扑身份而非 Linux 逻辑 CPU 号；在 cpu_logical_map 尚未
 * 完整可用的固件边界，这种比较无需依赖内核数据结构。未匹配 CPU继续停留，避免
 * 多个次 CPU 同时消费为单个目标准备的 secondary_data。
 */

// cmp x4, x2：比较释放值是否等于本 CPU 的 ID
	cmp	x4, x2

// b.eq：相等则跳出等待，进入 secondary_startup
	b.eq	secondary_startup

// wfe = Wait For Event
// CPU 进入低功耗状态，等待 SEV（Send Event）指令唤醒
// 主 CPU 写入释放变量后会发送 SEV，唤醒所有等待的次级 CPU
// 减少自旋等待的功耗和总线占用
	wfe
	b	pen
SYM_FUNC_END(secondary_holding_pen)

// ============================================================
// secondary_entry：次级 CPU 直接入口
// ============================================================
	/*
	 * 直接跳入内核的次级入口。
	 * 只用于内核动态热插拔 CPU 的场景。
	 */
SYM_FUNC_START(secondary_entry)

/*
 * secondary_entry - 已由内核/固件直接定向启动的次 CPU入口，常用于 CPU hotplug。
 * 调用方已经选择具体 CPU，不需要 holding-pen release 匹配；仍必须重新执行
 * init_kernel_el，因为下线/固件可能改变该 CPU 的 EL 系统寄存器。成功后与冷启动
 * 次 CPU 在 secondary_startup 汇合，无普通返回路径。
 */
	mov	x0, xzr
	bl	init_kernel_el			// w0=cpu_boot_mode
	b	secondary_startup
SYM_FUNC_END(secondary_entry)

// ============================================================
// secondary_startup：次级 CPU 通用启动
// ============================================================
SYM_FUNC_START_LOCAL(secondary_startup)
	/*
	 * 次级 CPU 的通用入口点。
	 * 所有次级 CPU（无论通过哪种路径进入）都在这里汇合。
	 */

/*
 * 入口 x0 是该 CPU 独立探测出的 boot mode；此时仍无 current/任务栈且 MMU 关闭。
 * 本阶段先证明 CPU 支持系统已经选定的地址空间与页粒度，再用全局 swapper_pg_dir
 * 和 idmap_pg_dir 开 MMU。失败 CPU必须在发布 online 之前停住，因此不会进入调度、
 * 接收普通中断或访问不兼容页表。成功路径间接跳到虚拟地址 __secondary_switched。
 */

// 保存启动模式到 x20
	mov	x20, x0				// preserve boot mode

// 如果配置了 52 位虚拟地址，检查 CPU 是否支持
#ifdef CONFIG_ARM64_VA_BITS_52
alternative_if ARM64_HAS_VA52
	bl	__cpu_secondary_check52bitva
alternative_else_nop_endif
#endif

// 初始化处理器（配置 TCR/MAIR 等，为开启 MMU 做准备）
	bl	__cpu_setup

// 加载次级 CPU 将使用的页表
// swapper_pg_dir：内核主页表（TTBR1，内核空间）
// idmap_pg_dir：恒等映射页表（TTBR0，开启 MMU 时使用）
	adrp	x1, swapper_pg_dir
	adrp	x2, idmap_pg_dir

/*
 * 主 CPU 此时已经构造并发布稳定内核页表；次 CPU不创建私有页表，而是加入同一
 * 内核地址空间。idmap 只保证开 MMU 的当前 PC 连续，TTBR1 的 swapper_pg_dir
 * 才承载后续共享内核虚拟地址。
 */

// 开启 MMU！参数：x1=TTBR1，x2=TTBR0（恒等映射）
	bl	__enable_mmu

// MMU 开启后需要通过寄存器间接跳转（不能用 b/bl 直接跳）
// 因为 __secondary_switched 在虚拟地址空间中
	ldr	x8, =__secondary_switched
	br	x8
SYM_FUNC_END(secondary_startup)

	.text

// ============================================================
// __secondary_switched：次级 CPU MMU 开启后的初始化
// ============================================================
SYM_FUNC_START_LOCAL(__secondary_switched)

/*
 * __secondary_switched - 次 CPU进入正式内核映射后的发布前收尾阶段。
 *
 * 入口仍没有可信 current/任务栈；主 CPU已在 secondary_data 中放入本次 bring-up
 * 专用 idle task，并通过通用 SMP 启动协议保证可见。函数先记录 EL 状态、最终化
 * EL2、安装异常向量，再消费 CPU_BOOT_TASK，建立 current/栈/per-CPU/SCS/PAC，
 * 最终进入 secondary_start_kernel()。只有 C 代码完成 CPU 能力、调度和中断注册后
 * 才会把 CPU 标为 online；本函数清 boot status 只表示早期汇编阶段成功。
 */
	mov	x0, x20
	bl	set_cpu_boot_mode_flag

	mov	x0, x20
	bl	finalise_el2

// 清除早期启动状态标志（表示这个 CPU 已成功启动）
	str_l	xzr, __early_cpu_boot_status, x3

/*
 * 清零的是本次启动握手的早期失败槽，不等价于 CPU 已 online。它让等待方区分
 * “卡在 MMU/能力检查”与“已跨过低层启动”；后续失败仍由 SMP C 路径报告和回滚。
 */

// 加载并设置异常向量表
	adr_l	x5, vectors
	msr	vbar_el1, x5
	isb

// 从 secondary_data 获取本 CPU 要运行的任务
// secondary_data 是主 CPU 在唤醒次级 CPU 前设置的
	adr_l	x0, secondary_data
	ldr	x2, [x0, #CPU_BOOT_TASK]

// 如果没有分配任务，说明启动太慢了，进入错误处理
	cbz	x2, __secondary_too_slow

/*
 * CPU_BOOT_TASK 为空通常表示主 CPU等待超时后撤销了启动请求。次 CPU来得太晚，
 * 不能继续使用可能已重用的启动数据或自行加入调度器，只能永久隔离。
 */

// 初始化 CPU 任务上下文
	init_cpu_task x2, x1, x3

// 如果支持指针认证，初始化密钥
#ifdef CONFIG_ARM64_PTR_AUTH
	ptrauth_keys_init_cpu x2, x3, x4, x5

/*
 * 每个任务/CPU 的内核 PAC 状态必须在首次执行受 PAC 保护的普通 C 返回链前安装；
 * 否则返回地址可能由错误 key 签名或认证。配置关闭时该安全阶段不存在。
 */
#endif

// 调用 secondary_start_kernel（C 函数）
// 次级 CPU 从这里进入 C 世界，完成 SMP 初始化
	bl	secondary_start_kernel

/*
 * secondary_start_kernel 接管该 CPU后续生命周期：核对能力、初始化中断/定时器，
 * 进入 idle 调度循环并最终参与系统。它不应返回；返回意味着 CPU 状态已部分发布
 * 却无人接管，因此 ASM_BUG 比继续执行相邻内存更安全。
 */

// 不可达，次级 CPU 应该永远不从这里返回
	ASM_BUG()
SYM_FUNC_END(__secondary_switched)

// ============================================================
// __secondary_too_slow：次级 CPU 启动超时处理
// ============================================================
SYM_FUNC_START_LOCAL(__secondary_too_slow)
/*
 * 主 CPU已撤销本次启动请求，因而这里没有可回滚给调用者的 task ownership；该
 * CPU 也尚未 online，隔离自旋不会减少系统已公布的 CPU 集合。WFE/WFI 组合只
 * 降低功耗，循环本身保证任何无关事件或中断唤醒都不能让它误入内核。
 */
// wfe/wfi 组合让 CPU 进入深度低功耗状态
// wfe = Wait For Event（等待事件，低功耗）
// wfi = Wait For Interrupt（等待中断，更深度低功耗）
// 死循环：这个 CPU 永远不会再被唤醒
	wfe
	wfi
	b	__secondary_too_slow
SYM_FUNC_END(__secondary_too_slow)

// ============================================================
// set_cpu_boot_mode_flag：记录 CPU 启动模式
// ============================================================
/*
 * 根据 w0 中的 CPU 启动模式设置 __boot_cpu_mode 标志。
 * 详见 arch/arm64/include/asm/virt.h
 */
SYM_FUNC_START_LOCAL(set_cpu_boot_mode_flag)
/*
 * 该 helper 把每个 CPU 自己探测的启动 EL 写入 __boot_cpu_mode 对应槽，供后续
 * C 代码判断系统是否以一致模式启动及能否启用 KVM/VHE。数组在早期启动协议下
 * 由各 CPU按模式选择不同 32 位元素；函数不做原子合并，协议只需要记录观察值，
 * 不承担 CPU online 发布。x0 是值输入，x1 被破坏，返回无错误码。
 */
// 加载 __boot_cpu_mode 数组的地址
// 这个数组存储两个值：EL1 和 EL2 的启动标志
	adr_l	x1, __boot_cpu_mode

// 判断是否从 EL2 启动
	cmp	w0, #BOOT_CPU_MODE_EL2
	b.ne	1f

// 如果是 EL2，偏移 4 字节（写入数组第二个元素）
	add	x1, x1, #4

// str w0, [x1]：把启动模式写入对应位置
1:	str	w0, [x1]			// Save CPU boot mode
	ret
SYM_FUNC_END(set_cpu_boot_mode_flag)

// ============================================================
// update_early_cpu_boot_status：更新早期启动状态（宏）
// ============================================================
/*
 * 在 MMU 关闭状态下更新启动 CPU 的失败状态。
 *
 * update_early_cpu_boot_status tmp, status
 *  - 破坏 tmp1, tmp2
 *  - 把 status 写入 __early_cpu_boot_status 并确保提交到内存
 */
	.macro	update_early_cpu_boot_status status, tmp1, tmp2

/*
 * 这是 MMU-off 失败路径向主 CPU传递诊断的唯一可靠通道。status 是编译期原因码，
 * tmp1/tmp2 是可破坏寄存器；写共享槽后 DMB 保证次序，再 invalidate 可能由其他
 * 地址视图留下的陈旧 cache line，使轮询主 CPU最终能从一致性点观察原因。
 * 宏只发布状态，不尝试唤醒、下线或释放 task；失败调用点随后永久停机。
 */

// 把立即数状态值加载到寄存器
	mov	\tmp2, #\status

// 获取状态变量地址
	adr_l	\tmp1, __early_cpu_boot_status

// 写入状态
	str	\tmp2, [\tmp1]

// dmb sy：确保写入完成（MMU 关闭，无 Cache，需要显式屏障）
	dmb	sy

// dc ivac：使包含这个地址的 Cache 行失效
// 确保其他 CPU 能读到最新的状态值而不是 Cache 中的旧值
	dc	ivac, \tmp1			// Invalidate potentially stale cache line
	.endm

// ============================================================
// __enable_mmu：开启 MMU
// ============================================================
/*
 * 开启 MMU。
 *
 *  x0  = SCTLR_EL1 的目标值（开启 MMU 后的配置）
 *  x1  = TTBR1_EL1 的值（内核页表根地址）
 *  x2  = ID 映射根页表地址（TTBR0 使用）
 *
 * 通过 x30/lr 返回调用者。调用者必须在 .idmap.text 段中，
 * 因为开启 MMU 后需要恒等映射保证执行连续性。
 *
 * 检查所选粒度大小是否被 CPU 支持，不支持则停机。
 */
	.section ".idmap.text","a"
SYM_FUNC_START(__enable_mmu)

/*
 * __enable_mmu - 让当前 CPU加入 Linux 统一的虚拟地址与内存属性模型。
 *
 * 主 CPU和次 CPU共用：x0 是 __cpu_setup 计算的目标 SCTLR，x1/x2 分别是 TTBR1
 * 内核页表与 TTBR0 恒等映射的物理根地址，x3 为临时寄存器。调用者必须正在
 * .idmap.text 执行且页表内容已按 cache 协议对 walker 可见。函数先验证本 CPU
 * 支持内核编译时选定的 granule，再安装两棵页表并开启 SCTLR.M；成功返回时 MMU
 * 已开但 PC 仍由 idmap 保持连续。失败没有 errno/回滚：记录共享原因并隔离 CPU。
 */

// mrs x3, ID_AA64MMFR0_EL1：读取内存模型特性寄存器
// 包含 CPU 支持的页大小粒度信息
	mrs	x3, ID_AA64MMFR0_EL1

// ubfx = Unsigned Bit Field Extract（无符号位域提取）
// 语法：ubfx 目标, 源, 起始位, 位数
// 提取 TGRAN（Translation Granule）字段
// 这个字段表示 CPU 支持的页表粒度（4KB/16KB/64KB）
	ubfx	x3, x3, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4

// 检查粒度是否在支持范围内（最小值）
	cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MIN
	b.lt    __no_granule_support

// 检查粒度是否在支持范围内（最大值）
	cmp     x3, #ID_AA64MMFR0_EL1_TGRAN_SUPPORTED_MAX
	b.gt    __no_granule_support

/*
 * 该检查不是性能优化，而是系统一致性门禁：一个不支持全局页表粒度的次 CPU无法
 * 安全遍历 swapper_pg_dir；允许它继续会在取指前就产生翻译异常，且可能发生在
 * 正式异常向量/current 尚未建立的阶段，无法可靠恢复。
 */

// phys_to_ttbr：把物理地址转换为 TTBR 格式
// TTBR 不只是物理地址，还包含 ASID、CnP 等控制位
	phys_to_ttbr x2, x2

// msr ttbr0_el1, x2：设置 TTBR0（用户空间/恒等映射页表）
// 开启 MMU 后，低地址虚拟地址通过 TTBR0 翻译
	msr	ttbr0_el1, x2			// load TTBR0

// load_ttbr1：设置 TTBR1（内核空间页表）
// 高地址虚拟地址通过 TTBR1 翻译
	load_ttbr1 x1, x1, x3

/*
 * 安装顺序先 TTBR0 idmap、后 TTBR1 内核映射、最后 SCTLR.M。这样开 MMU 的瞬间，
 * 当前低/物理地址 PC 与即将跳入的高地址目标都已有翻译；不存在“先开 MMU 再补
 * 页表”的窗口。load_ttbr1/set_sctlr_el1 内部还承担体系结构要求的同步。
 */

// set_sctlr_el1 x0：写入 SCTLR_EL1，开启 MMU！
// 这是整个汇编启动流程最关键的一条指令
// 执行后，所有内存访问都经过 MMU 翻译
// 内部包含写寄存器 + ISB，确保立即生效
	set_sctlr_el1	x0

	ret
SYM_FUNC_END(__enable_mmu)

// ============================================================
// __cpu_secondary_check52bitva：检查 52 位虚拟地址支持
// ============================================================
#ifdef CONFIG_ARM64_VA_BITS_52
SYM_FUNC_START(__cpu_secondary_check52bitva)
/*
 * 系统已由 boot CPU选择 52 位 VA 布局，次 CPU必须证明能执行同一地址空间。
 * 非 LPA2 配置查询 VARange；LPA2 配置则检查所选 granule 的 LPA2 能力编码。
 * 成功只返回能力成立，不修改页表；失败在 CPU发布 online 前写原因并永久隔离，
 * 因为运行中的单一内核不能为某个 CPU临时改用较小 VA 布局。
 */
#ifndef CONFIG_ARM64_LPA2
// 检查 ID_AA64MMFR2_EL1 中的 VARange 字段
// 判断 CPU 是否支持 52 位虚拟地址
	mrs_s	x0, SYS_ID_AA64MMFR2_EL1
	and	x0, x0, ID_AA64MMFR2_EL1_VARange_MASK

// cbnz：不为零说明支持，跳转到成功返回
	cbnz	x0, 2f
#else
// LPA2（Large Physical Address 2）模式下的检查方式不同
	mrs	x0, id_aa64mmfr0_el1
	sbfx	x0, x0, #ID_AA64MMFR0_EL1_TGRAN_SHIFT, 4
	cmp	x0, #ID_AA64MMFR0_EL1_TGRAN_LPA2
	b.ge	2f
#endif

// CPU 不支持 52 位 VA，记录错误状态并永久停机
// CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_52_BIT_VA：原因码
	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_52_BIT_VA, x0, x1

// 死循环，交替 wfe/wfi 以最低功耗等待
1:	wfe
	wfi
	b	1b

// 标签 2:（支持 52 位 VA，正常返回）
2:	ret
SYM_FUNC_END(__cpu_secondary_check52bitva)
#endif

// ============================================================
// __no_granule_support：不支持配置的页粒度
// ============================================================
SYM_FUNC_START_LOCAL(__no_granule_support)
	/*
	 * 到达时尚未开启 MMU、尚无 task/current 和普通异常恢复环境。发布 NO_GRAN
	 * 原因后永久停机，主 CPU可把该 CPU 保持 offline；不能返回 __enable_mmu，
	 * 因为后续任何页表访问都建立在这个 granule 能被硬件解释的前提上。
	 */
	/* 标记这个 CPU 无法启动，卡在内核中 */
	update_early_cpu_boot_status \
		CPU_STUCK_IN_KERNEL | CPU_STUCK_REASON_NO_GRAN, x1, x2

// 永久死循环，CPU 就此停止
1:
	wfe
	wfi
	b	1b
SYM_FUNC_END(__no_granule_support)

// ============================================================
// __primary_switch：开启 MMU 并完成内核重定位
// ============================================================
SYM_FUNC_START_LOCAL(__primary_switch)

/*
 * __primary_switch - 主 CPU跨越“物理恒等执行”到“正式高地址内核执行”的提交点。
 *
 * 入口来自 primary_entry：x0 是 __cpu_setup 生成的 SCTLR，x20/x21 保存 boot mode
 * 与 FDT；无正式 current/异常向量。先用 reserved_pg_dir + init idmap 开 MMU，再
 * 在早期栈调用位置无关的 __pi_early_map_kernel 建立 swapper 映射并处理 KASLR
 * 重定位，最后间接跳 __primary_switched。成功不返回；开 MMU后若映射错误，当前
 * 阶段通常连可靠 C 错误路径都没有，因此所有前置验证和 cache 顺序必须在提交前完成。
 */

// 加载 reserved_pg_dir（保留的页目录，TTBR1 临时使用）
	adrp	x1, reserved_pg_dir

/*
 * reserved_pg_dir 是过渡 TTBR1，不是最终内核地址空间：首次开 MMU主要依赖 TTBR0
 * idmap 保住当前执行流；随后 __pi_early_map_kernel 才填充/切换正式内核映射。
 * 这种两阶段方案避免在 MMU-off 环境一次完成所有 KASLR/高地址映射复杂度。
 */

// 加载恒等映射页目录（TTBR0 使用）
	adrp	x2, __pi_init_idmap_pg_dir

// 调用 __enable_mmu 开启 MMU
// 参数：x0=SCTLR 值（由 __cpu_setup 准备好），x1=TTBR1，x2=TTBR0
// 执行后 MMU 开启，所有地址经过翻译
	bl	__enable_mmu

// MMU 开启后重新建立栈
// 此时地址空间已切换，需要用新地址访问栈
	adrp	x1, early_init_stack
	mov	sp, x1
	mov	x29, xzr

/*
 * MMU 已开启但 PC 仍在 idmap。重新装载 early_init_stack 是为了使用当前翻译环境
 * 可访问的栈地址，并把旧的 MMU-off 调用链截断；后续位置无关 helper 可以安全
 * 使用栈，同时仍不能假定 init_task/current 已建立。
 */

// 传递完整的启动状态和 FDT 地址
	mov	x0, x20				// pass the full boot status
	mov	x1, x21				// pass the FDT

// 调用 __pi_early_map_kernel：映射并重定位内核
// 这个函数建立完整的内核虚拟地址映射
// 如果内核需要重定位（KASLR），在这里完成
	bl	__pi_early_map_kernel		// Map and relocate the kernel

/*
 * __pi_early_map_kernel 消费 boot mode/FDT 以确定早期内存与随机化条件，建立内核
 * 镜像、线性映射等进入 C 代码所需的页表视图；它完成的是地址空间发布，不会
 * 创建 task 或启动调度器。返回后高地址 __primary_switched 已可取指访问。
 */
/*
 * 修正说明：这里的“线性映射等”不表示该 helper 已完成通用 RAM direct map。
 * 当前实现直接建立的是内核 Image 各段的高地址映射、FDT 临时可访问映射和
 * KASLR/重定位所需页表，并把根页表复制到 swapper_pg_dir；完整物理内存线性
 * 映射仍由后续 setup_arch()/paging_init() 基于 memblock 结果扩展。
 */

// ldr x8, =__primary_switched：用绝对地址加载目标
// 不能用 b/adr，因为此时 PC 在恒等映射区，
// 而 __primary_switched 在内核虚拟地址空间
// 用寄存器间接跳转才能正确到达虚拟地址
	ldr	x8, =__primary_switched

// KERNEL_START 的物理地址作为参数传给 __primary_switched
	adrp	x0, KERNEL_START		// __pa(KERNEL_START)

// br = Branch to Register（间接跳转，不保存返回地址）
// 跳转到 __primary_switched，正式进入内核虚拟地址空间
// 从这里开始，PC 运行在正式的内核虚拟地址上
	br	x8

/*
 * BR 不保存返回地址，明确表示 idmap 阶段生命周期结束。目标以链接虚拟地址装入，
 * 因而跳转同时验证正式 TTBR1 映射；之后代码不会再回到本过渡页表调用链。
 */
SYM_FUNC_END(__primary_switch)
