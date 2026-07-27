/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * arm64 地址空间与地址转换学习导读
 *
 * 中文学习注释生成模型：OpenAI GPT-5 Codex（2026-07-27）。
 * 源码分析基线：doc/lql 分支，commit f8f7ac7435bf。
 * 宏观学习入口：doc/01 arm64-boot-internals.md 和
 * doc/09 linux-memory-management-internals.md。
 *
 * 本文件定义 arm64 内核虚拟地址布局、线性映射与内核镜像映射的转换、
 * vmemmap、KASAN shadow、内核栈尺寸、页表内存类型及地址标签操作。它提供
 * 编译期常量和极短的转换 helper，不负责建立页表、分配物理页、刷新
 * TLB/cache，也不能替代驱动 DMA 地址转换 API。
 *
 * 两条核心地址路径必须区分：
 *
 *   普通 RAM：物理地址 <-> [PAGE_OFFSET, PAGE_END) 线性映射
 *   内核镜像：链接/运行虚址 <-> 加载物理地址，通过 kimage_voffset 转换
 *
 * struct page 与 RAM 的关系则由 VMEMMAP_START 和 PAGE_SIZE/sizeof(struct
 * page) 的比例建立。多数宏不加锁：布局常量在编译/启动期确定，
 * memstart_addr、kimage_voffset 和 vabits_actual 在普通并发访问前完成发布，
 * 运行期读者只做纯计算。收益是热路径无需页表查询；代价是调用者必须
 * 保证地址属于正确映射域，错误地把 vmalloc、I/O 或 DMA 地址传入会得到
 * 无意义的转换结果，DEBUG_VIRTUAL 只能检测其中一部分误用。
 */
/*
 * Based on arch/arm/include/asm/memory.h
 *
 * Copyright (C) 2000-2002 Russell King
 * Copyright (C) 2012 ARM Ltd.
 *
 * Note: this file should not be included by non-asm/.h files
 */
/*
 * 本文件以 32 位 ARM 的对应头文件为基础。注意：它只能由汇编文件或其他
 * 头文件包含，普通 .c 文件应通过更高层接口间接获得定义，以控制依赖
 * 关系和避免体系结构私有布局扩散。
 */
#ifndef __ASM_MEMORY_H
#define __ASM_MEMORY_H

#include <linux/const.h>
#include <linux/sizes.h>
#include <asm/page-def.h>

/*
 * Size of the PCI I/O space. This must remain a power of two so that
 * IO_SPACE_LIMIT acts as a mask for the low bits of I/O addresses.
 */
/*
 * PCI 端口 I/O 虚拟窗口固定为 16 MiB。大小必须保持 2 的幂，才能让
 * IO_SPACE_LIMIT 用位掩码截取 I/O 地址低位；改变它还必须同步检查窗口
 * 起止地址和通用 PCI I/O 边界计算。
 */
#define PCI_IO_SIZE		SZ_16M

/*
 * VMEMMAP_SIZE - allows the whole linear region to be covered by
 *                a struct page array
 *
 * If we are configured with a 52-bit kernel VA then our VMEMMAP_SIZE
 * needs to cover the memory region from the beginning of the 52-bit
 * PAGE_OFFSET all the way to PAGE_END for 48-bit. This allows us to
 * keep a constant PAGE_OFFSET and "fallback" to using the higher end
 * of the VMEMMAP where 52-bit support is not available in hardware.
 */
/*
 * VMEMMAP 为线性映射可能覆盖的每个物理页预留一个 struct page 槽。
 * VMEMMAP_RANGE 是最小受支持 VA 布局下 PAGE_OFFSET 到 PAGE_END 的字节数；
 * 右移 PAGE_SHIFT 得到页数，再乘 struct page 大小得到元数据虚拟区长度。
 *
 * 52 位 VA 内核仍保持一个固定 PAGE_OFFSET。若某颗 CPU 只能使用 48 位 VA，
 * 页表布局退化到 vmemmap 预留区的高端，而不是改变所有地址转换常量；
 * 代价是必须按最宽兼容范围预留 vmemmap 虚拟空间。
 */
#define VMEMMAP_RANGE	(_PAGE_END(VA_BITS_MIN) - PAGE_OFFSET)
#define VMEMMAP_SIZE	((VMEMMAP_RANGE >> PAGE_SHIFT) * sizeof(struct page))

/*
 * PAGE_OFFSET - the virtual address of the start of the linear map, at the
 *               start of the TTBR1 address space.
 * PAGE_END - the end of the linear map, where all other kernel mappings begin.
 * KIMAGE_VADDR - the virtual address of the start of the kernel image.
 * VA_BITS - the maximum number of bits for virtual addresses.
 */
/*
 * 地址布局说明：
 *
 * PAGE_OFFSET 是 TTBR1 内核地址空间最低端，也是线性映射起点；PAGE_END
 * 是线性映射的开区间上界，其上依次放置 vmemmap、PCI I/O、modules、
 * kernel image、fixmap 等区域。KIMAGE_VADDR 等于模块区末端，内核镜像
 * 从这里开始。VA_BITS 是编译期最大 VA 宽度，并不总等于硬件实际
 * 启用宽度。
 */
#define VA_BITS			(CONFIG_ARM64_VA_BITS)
/* 对 2^va 取负，利用 64 位补码得到高半区规范地址的起点。 */
#define _PAGE_OFFSET(va)	(-(UL(1) << (va)))
#define PAGE_OFFSET		(_PAGE_OFFSET(VA_BITS))
#define KIMAGE_VADDR		(MODULES_END)
#define MODULES_END		(MODULES_VADDR + MODULES_VSIZE)
#define MODULES_VADDR		(_PAGE_END(VA_BITS_MIN))
#define MODULES_VSIZE		(SZ_2G)
#define VMEMMAP_START		(VMEMMAP_END - VMEMMAP_SIZE)
#define VMEMMAP_END		(-UL(SZ_1G))
#define PCI_IO_START		(VMEMMAP_END + SZ_8M)
#define PCI_IO_END		(PCI_IO_START + PCI_IO_SIZE)
#define FIXADDR_TOP		(-UL(SZ_8M))

/*
 * VA_BITS_MIN 是同一内核镜像必须兼容的最小运行时 VA 宽度。配置超过 48 位
 * 时，16K 页因页表级别组合限制以 47 位作为 fallback，其余页粒度为 48 位；
 * 普通配置下最小值就是编译值。
 */
#if VA_BITS > 48
#ifdef CONFIG_ARM64_16K_PAGES
#define VA_BITS_MIN		(47)
#else
#define VA_BITS_MIN		(48)
#endif
#else
#define VA_BITS_MIN		(VA_BITS)
#endif

/* 给定 VA 宽度下线性映射的上界，也是 TTBR1 地址空间中点边界。 */
#define _PAGE_END(va)		(-(UL(1) << ((va) - 1)))

/* 链接器符号给出内核镜像全部段的虚拟起止，不代表线性映射边界。 */
#define KERNEL_START		_text
#define KERNEL_END		_end

/*
 * Generic and Software Tag-Based KASAN modes require 1/8th and 1/16th of the
 * kernel virtual address space for storing the shadow memory respectively.
 *
 * The mapping between a virtual memory address and its corresponding shadow
 * memory address is defined based on the formula:
 *
 *     shadow_addr = (addr >> KASAN_SHADOW_SCALE_SHIFT) + KASAN_SHADOW_OFFSET
 *
 * where KASAN_SHADOW_SCALE_SHIFT is the order of the number of bits that map
 * to a single shadow byte and KASAN_SHADOW_OFFSET is a constant that offsets
 * the mapping. Note that KASAN_SHADOW_OFFSET does not point to the start of
 * the shadow memory region.
 *
 * Based on this mapping, we define two constants:
 *
 *     KASAN_SHADOW_START: the start of the shadow memory region;
 *     KASAN_SHADOW_END: the end of the shadow memory region.
 *
 * KASAN_SHADOW_END is defined first as the shadow address that corresponds to
 * the upper bound of possible virtual kernel memory addresses UL(1) << 64
 * according to the mapping formula.
 *
 * KASAN_SHADOW_START is defined second based on KASAN_SHADOW_END. The shadow
 * memory start must map to the lowest possible kernel virtual memory address
 * and thus it depends on the actual bitness of the address space.
 *
 * As KASAN inserts redzones between stack variables, this increases the stack
 * memory usage significantly. Thus, we double the (minimum) stack size.
 */
/*
 * Generic KASAN 每 8 字节被监控内存需要 1 字节 shadow，SW_TAGS 每 16 字节
 * 需要 1 字节。映射公式先按 KASAN_SHADOW_SCALE_SHIFT 缩小原地址，再加
 * 固定 offset；offset 是仿射变换常量，并不等于 shadow 区实际起点。
 *
 * KASAN_SHADOW_END 先用 64 位虚拟地址上界代入公式求得；START 再从 END
 * 减去实际 VA 宽度所需 shadow 容量。启用 KASAN 后 PAGE_END 被压低到
 * shadow 起点，使线性映射不会与 shadow 重叠。KASAN 为栈变量插入 redzone，
 * 因而 KASAN_THREAD_SHIFT 把最小内核栈翻倍；未启用时不增加栈阶数。
 */
#if defined(CONFIG_KASAN_GENERIC) || defined(CONFIG_KASAN_SW_TAGS)
#define KASAN_SHADOW_OFFSET	_AC(CONFIG_KASAN_SHADOW_OFFSET, UL)
#define KASAN_SHADOW_END	((UL(1) << (64 - KASAN_SHADOW_SCALE_SHIFT)) + KASAN_SHADOW_OFFSET)
#define _KASAN_SHADOW_START(va)	(KASAN_SHADOW_END - (UL(1) << ((va) - KASAN_SHADOW_SCALE_SHIFT)))
#define KASAN_SHADOW_START	_KASAN_SHADOW_START(vabits_actual)
#define PAGE_END		KASAN_SHADOW_START
#define KASAN_THREAD_SHIFT	1
#else
#define KASAN_THREAD_SHIFT	0
#define PAGE_END		(_PAGE_END(VA_BITS_MIN))
#endif /* CONFIG_KASAN */

/*
 * 线性映射最后一个虚拟字节转换得到可直接映射的最大物理地址。减 1
 * 是因为 PAGE_END 自身不属于线性映射；__pa() 只做地址域转换，
 * 不验证 RAM 是否存在。
 */
#define DIRECT_MAP_PHYSMEM_END	__pa(PAGE_END - 1)

/* arm64 基础内核栈为 2^14 字节，软件 KASAN 配置再提高一阶。 */
#define MIN_THREAD_SHIFT	(14 + KASAN_THREAD_SHIFT)

/*
 * VMAP'd stacks are allocated at page granularity, so we must ensure that such
 * stacks are a multiple of page size.
 */
/*
 * vmalloc 内核栈以整页分配，THREAD_SHIFT 至少等于 PAGE_SHIFT；否则选择
 * MIN_THREAD_SHIFT。THREAD_SIZE_ORDER 是相对 PAGE_SIZE 的伙伴分配阶数，
 * THREAD_SIZE 则是最终字节数。
 */
#if (MIN_THREAD_SHIFT < PAGE_SHIFT)
#define THREAD_SHIFT		PAGE_SHIFT
#else
#define THREAD_SHIFT		MIN_THREAD_SHIFT
#endif

#if THREAD_SHIFT >= PAGE_SHIFT
#define THREAD_SIZE_ORDER	(THREAD_SHIFT - PAGE_SHIFT)
#endif

#define THREAD_SIZE		(UL(1) << THREAD_SHIFT)

/*
 * By aligning VMAP'd stacks to 2 * THREAD_SIZE, we can detect overflow by
 * checking sp & (1 << THREAD_SHIFT), which we can do cheaply in the entry
 * assembly.
 */
/*
 * VMAP 栈按 2*THREAD_SIZE 对齐，使正常栈和相邻 guard/overflow 半区可由
 * SP 的 THREAD_SHIFT 位快速区分。异常入口汇编只需一次位测试即可检测
 * 越界，不必访问页表；代价是提高虚拟地址对齐要求。
 */
#define THREAD_ALIGN		(2 * THREAD_SIZE)

/* IRQ 栈沿用普通线程栈尺寸；紧急 overflow 栈固定为 4 KiB。 */
#define IRQ_STACK_SIZE		THREAD_SIZE

#define OVERFLOW_STACK_SIZE	SZ_4K

/* 非 VHE hypervisor 栈以一页为单位，随内核页粒度变化。 */
#define NVHE_STACK_SHIFT       PAGE_SHIFT
#define NVHE_STACK_SIZE        (UL(1) << NVHE_STACK_SHIFT)

/*
 * With the minimum frame size of [x29, x30], exactly half the combined
 * sizes of the hyp and overflow stacks is the maximum size needed to
 * save the unwinded stacktrace; plus an additional entry to delimit the
 * end.
 */
/*
 * 每个最小栈帧由 x29/x30 两个 long 构成，而保存回溯时每帧只需记录一个
 * long 地址，所以输出缓冲区的最大字节数是两块栈总大小的一半；再增加
 * 一个 long 用作回溯终止标记。
 */
#define NVHE_STACKTRACE_SIZE	((OVERFLOW_STACK_SIZE + NVHE_STACK_SIZE) / 2 + sizeof(long))

/*
 * Alignment of kernel segments (e.g. .text, .data).
 *
 *  4 KB granule:  16 level 3 entries, with contiguous bit
 * 16 KB granule:   4 level 3 entries, without contiguous bit
 * 64 KB granule:   1 level 3 entry
 */
/*
 * 内核段统一按 64 KiB 对齐：4K 页可用 16 个连续 L3 项，16K 页用 4 项，
 * 64K 页用单项。统一边界让不同页粒度共享链接布局，并满足连续
 * 映射优化。
 */
#define SEGMENT_ALIGN		SZ_64K

/*
 * Memory types available.
 *
 * IMPORTANT: MT_NORMAL must be index 0 since vm_get_page_prot() may 'or' in
 *	      the MT_NORMAL_TAGGED memory type for PROT_MTE mappings. Note
 *	      that protection_map[] only contains MT_NORMAL attributes.
 */
/*
 * 这些数字是 MAIR 属性槽索引，不是完整页表属性。MT_NORMAL 必须为 0，
 * 因为 vm_get_page_prot() 会在 protection_map[] 的普通内存属性上按位并入
 * MT_NORMAL_TAGGED；若普通类型不是零，组合结果会选择错误 MAIR 槽。
 *
 * NORMAL_TAGGED 用于 MTE，NORMAL_NC 为普通不可缓存内存；Device nGnRnE
 * 比 nGnRE 更严格，后者允许 early write acknowledgment。
 */
#define MT_NORMAL		0
#define MT_NORMAL_TAGGED	1
#define MT_NORMAL_NC		2
#define MT_DEVICE_nGnRnE	3
#define MT_DEVICE_nGnRE		4

/*
 * Memory types for Stage-2 translation when HCR_EL2.FWB=0. See R_HMNDG,
 * R_TNHFM, R_GQFSF and I_MCQKW for the details on how these attributes get
 * combined with Stage-1.
 */
/*
 * HCR_EL2.FWB=0 时，Stage-2 属性按架构规则与 Stage-1 合并。这里的编码
 * 直接写入 S2 页表字段；AS_S1 选择 normal 编码，让最终类型继续受 S1
 * 约束。R_HMNDG 等名称是 Arm 架构规则标识。
 */
#define MT_S2_NORMAL		0xf
#define MT_S2_NORMAL_NC		0x5
#define MT_S2_DEVICE_nGnRE	0x1
#define MT_S2_AS_S1		MT_S2_NORMAL

/*
 * Memory types for Stage-2 translation when HCR_EL2.FWB=1. Stage-2 enforces
 * Normal-WB and Device-nGnRE, unless we actively say that S1 wins. See
 * R_VRJSW and R_RHWZM for details.
 */
/*
 * FWB=1 时硬件默认把 Stage-2 强制为 Normal-WB 或 Device-nGnRE，减少
 * hypervisor 手工组合属性的负担；AS_S1 编码显式让 Stage-1 获胜。
 * 两组常量不可混用，选择由 HCR_EL2.FWB 的实际配置决定。
 */
#define MT_S2_FWB_NORMAL	6
#define MT_S2_FWB_NORMAL_NC	5
#define MT_S2_FWB_DEVICE_nGnRE	1
#define MT_S2_FWB_AS_S1		7

/*
 * ioremap 允许尝试的最大块级别：4K 页可到 PUD，大页配置限制到 PMD，
 * 避免生成当前页表粒度无法合法表示或拆分代价过高的 I/O 映射块。
 */
#ifdef CONFIG_ARM64_4K_PAGES
#define IOREMAP_MAX_ORDER	(PUD_SHIFT)
#else
#define IOREMAP_MAX_ORDER	(PMD_SHIFT)
#endif

/*
 *  Open-coded (swapper_pg_dir - reserved_pg_dir) as this cannot be calculated
 *  until link time.
 */
/*
 * reserved_pg_dir 紧邻 swapper_pg_dir，二者差值要到链接时才确定；汇编早期
 * 代码无法使用 C 地址相减，按链接脚本不变量直接编码为一页。
 */
#define RESERVED_SWAPPER_OFFSET	(PAGE_SIZE)

/*
 *  Open-coded (swapper_pg_dir - tramp_pg_dir) as this cannot be calculated
 *  until link time.
 */
/*
 * tramp_pg_dir 与 swapper_pg_dir 相隔两页，同样把链接期符号差值开编码为
 * 常量。若链接脚本布局改变，这两个 offset 必须同步更新。
 */
#define TRAMP_SWAPPER_OFFSET	(2 * PAGE_SIZE)

#ifndef __ASSEMBLER__

#include <linux/bitops.h>
#include <linux/compiler.h>
#include <linux/mmdebug.h>
#include <linux/types.h>
#include <asm/boot.h>
#include <asm/bug.h>
#include <asm/sections.h>
#include <asm/sysreg.h>

/*
 * read_tcr - 读取当前 CPU 的 EL1 Translation Control Register。
 *
 * 调用者：VA_BITS>48 时的 vabits_actual 宏及其地址布局用户。
 * 入参：无。任意不可睡眠上下文均可调用；不持锁，只读取本地
 * 系统寄存器。
 * 返回：当前 TCR_EL1 的 64 位快照，无错误返回和其他副作用。
 *
 * __pure 允许编译器在输入状态未显式变化的表达式中复用结果，因此
 * 这里故意使用非 volatile 内联汇编；通用 read_sysreg() 带 asm
 * volatile，会阻止这种优化。调用协议要求 TCR 已在 CPU 启动阶段稳定，
 * 不能用本 helper 观察正在并发修改的 TCR。
 */
static inline u64 __pure read_tcr(void)
{
	/* tcr 仅承接一次 mrs 输出，单位是寄存器位图。 */
	u64  tcr;

	// read_sysreg() uses asm volatile, so avoid it here
	/*
	 * read_sysreg() 使用 volatile 汇编，所以这里避免调用它；直接 mrs 仍
	 * 读取同一 TCR_EL1，但允许 __pure 契约下的公共子表达式消除。
	 */
	asm("mrs %0, tcr_el1" : "=r"(tcr));
	return tcr;
}

#if VA_BITS > 48
// For reasons of #include hell, we can't use TCR_T1SZ_OFFSET/TCR_T1SZ_MASK here
/*
 * 为避免头文件循环依赖，这里直接使用 TCR_EL1.T1SZ 的架构位置 [21:16]，
 * 而不引用 sysreg 宏。TTBR1 地址宽度等于 64-T1SZ，因此得到当前硬件实际
 * 启用的内核 VA 位数。普通 VA 配置没有 runtime fallback，直接用 VA_BITS。
 */
#define vabits_actual		(64 - ((read_tcr() >> 16) & 63))
#else
#define vabits_actual		((u64)VA_BITS)
#endif

/*
 * memstart_addr 是线性映射 PAGE_OFFSET 所对应的物理基址，由
 * arm64_memblock_init() 根据 DRAM 和 VA 窗口选择，启动后只读。初值 -1
 * 的最低位为 1，而合法值按至少页大小对齐；PHYS_OFFSET 的 VM_BUG_ON
 * 因而能捕获初始化前误用。返回单位为物理字节地址。
 */
extern s64			memstart_addr;
/* PHYS_OFFSET - the physical address of the start of memory. */
/*
 * PHYS_OFFSET 即内核线性映射中第一字节对应的物理地址，不一定为
 * 物理 0。
 */
#define PHYS_OFFSET		({ VM_BUG_ON(memstart_addr & 1); memstart_addr; })

/* the offset between the kernel virtual and physical mappings */
/*
 * kimage_voffset = 内核镜像运行虚址 - 镜像加载物理地址。它由早期
 * 汇编设置，用于独立于线性映射的 kernel image 转换；启动完成后只读。
 */
extern u64			kimage_voffset;

/*
 * kaslr_offset - 返回内核镜像相对固定 KIMAGE_VADDR 的随机化偏移。
 *
 * 入参：无；纯地址计算，不睡眠、无锁、无副作用。返回单位为字节；
 * 未随机化时为 0。&_text 是实际运行虚址，KIMAGE_VADDR 是链接布局基准。
 */
static inline unsigned long kaslr_offset(void)
{
	return (u64)&_text - KIMAGE_VADDR;
}

#ifdef CONFIG_RANDOMIZE_BASE
/* KASLR 初始化由启动路径实现；它在普通并发执行前确定镜像随机位置。 */
void kaslr_init(void);
/*
 * kaslr_enabled - 查询本次启动是否实际启用了 KASLR。
 *
 * 入参：无；任意上下文只读启动期布尔状态，不睡眠。返回 true/false，
 * 无副作用；编译支持 KASLR 不代表本次启动一定启用。
 */
static inline bool kaslr_enabled(void)
{
	extern bool __kaslr_is_enabled;
	return __kaslr_is_enabled;
}
#else
/*
 * 未编译 RANDOMIZE_BASE 时的接口桩：kaslr_init() 无动作，
 * kaslr_enabled() 固定返回 false。两者均无入参、不睡眠、无副作用，
 * 让通用调用点无需条件编译。
 */
static inline void kaslr_init(void) { }
static inline bool kaslr_enabled(void) { return false; }
#endif

/*
 * Allow all memory at the discovery stage. We will clip it later.
 */
/*
 * 早期 memblock 发现阶段先接受完整 64 位物理范围，随后
 * arm64_memblock_init() 再按 PA 位宽、线性映射窗口和 mem= 参数裁剪。
 * 先发现再裁剪可避免固件表解析过早丢失仍需诊断或保留的区域。
 */
#define MIN_MEMBLOCK_ADDR	0
#define MAX_MEMBLOCK_ADDR	U64_MAX

/*
 * PFNs are used to describe any physical page; this means
 * PFN 0 == physical address 0.
 *
 * This is the PFN of the first RAM page in the kernel
 * direct-mapped view.  We assume this is the first page
 * of RAM in the mem_map as well.
 */
/*
 * PFN 是物理地址右移 PAGE_SHIFT 的绝对页号，所以 PFN 0 永远代表物理
 * 地址 0；PHYS_PFN_OFFSET 则是线性映射第一 RAM 页的 PFN。它同时作为
 * mem_map/vmemmap 的基准偏移，不能误解为“PFN 从这里重新编号为零”。
 */
#define PHYS_PFN_OFFSET	(PHYS_OFFSET >> PAGE_SHIFT)

/*
 * When dealing with data aborts, watchpoints, or instruction traps we may end
 * up with a tagged userland pointer. Clear the tag to get a sane pointer to
 * pass on to access_ok(), for instance.
 */
/*
 * 异常地址可能带用户 Top Byte Ignore 标签。__untagged_addr() 以 bit 55
 * 为规范地址符号位做扩展，构造去除 top-byte tag 后的规范形式；外层
 * untagged_addr() 保留原表达式类型，并以单次求值的临时量避免带副作用
 * 参数被重复执行。结果用于 access_ok() 等地址范围判断，不改变原指针对象
 * 或 ownership。
 */
#define __untagged_addr(addr)	\
	((__force __typeof__(addr))sign_extend64((__force u64)(addr), 55))

#define untagged_addr(addr)	({					\
	u64 __addr = (__force u64)(addr);					\
	__addr &= __untagged_addr(__addr);				\
	(__force __typeof__(addr))__addr;				\
})

#if defined(CONFIG_KASAN_SW_TAGS) || defined(CONFIG_KASAN_HW_TAGS)
/*
 * KASAN/MTE 使用地址 [63:56] 保存 8 位 tag：
 * __tag_shifted() 把 tag 放入 top byte，__tag_reset() 恢复无标签规范地址，
 * __tag_get() 取回 tag。未启用标签配置时三者退化为零开销恒等/零值接口。
 */
#define __tag_shifted(tag)	((u64)(tag) << 56)
#define __tag_reset(addr)	__untagged_addr(addr)
#define __tag_get(addr)		(__u8)((u64)(addr) >> 56)
#else
#define __tag_shifted(tag)	0UL
#define __tag_reset(addr)	(addr)
#define __tag_get(addr)		0
#endif /* CONFIG_KASAN_SW_TAGS || CONFIG_KASAN_HW_TAGS */

/*
 * __tag_set - 用指定 tag 替换内核地址的 top byte。
 *
 * addr 是非 NULL 要求由调用者决定的借用地址值，本函数不解引用它；
 * tag 是完整 8 位标签。任意上下文不睡眠、无内存副作用。返回带新标签的
 * const void *，所指对象及 ownership 不变。
 */
static inline const void *__tag_set(const void *addr, u8 tag)
{
	/* __addr 先清除旧 tag，再与移到 [63:56] 的新 tag 合并。 */
	u64 __addr = (u64)addr & ~__tag_shifted(0xff);
	return (const void *)(__addr | __tag_shifted(tag));
}

#ifdef CONFIG_KASAN_HW_TAGS
/*
 * 这些架构钩子把通用 KASAN tag 操作直接分派到 arm64 MTE：
 * sync/async/asymm/store-only 选择 tag fault 报告模式；TCO start/stop
 * 临时抑制或恢复检查；fault/get/set 操作读写本地状态或内存 allocation tag。
 * 它们可能产生系统寄存器或 tagged-memory 副作用，参数和上下文契约继承
 * 对应 mte_* 实现。arch_get_mem_tag(addr) 借用并读取 addr 对应 allocation
 * tag；arch_set_mem_tag_range(addr, size, tag, init) 要求 addr 非 NULL 且
 * 与 size 都按 MTE granule 对齐，size 单位字节，tag 为 8 位 allocation
 * tag，init 决定是否同时初始化内存内容。所有地址 ownership 均不转移。
 * 只有 HW_TAGS 构建才暴露这些宏。
 */
#define arch_enable_tag_checks_sync()		mte_enable_kernel_sync()
#define arch_enable_tag_checks_async()		mte_enable_kernel_async()
#define arch_enable_tag_checks_asymm()		mte_enable_kernel_asymm()
#define arch_enable_tag_checks_write_only()	mte_enable_kernel_store_only()
#define arch_suppress_tag_checks_start()	mte_enable_tco()
#define arch_suppress_tag_checks_stop()		mte_disable_tco()
#define arch_force_async_tag_fault()		mte_check_tfsr_exit()
#define arch_get_random_tag()			mte_get_random_tag()
#define arch_get_mem_tag(addr)			mte_get_mem_tag(addr)
#define arch_set_mem_tag_range(addr, size, tag, init)	\
			mte_set_mem_tag_range((addr), (size), (tag), (init))
#endif /* CONFIG_KASAN_HW_TAGS */

/*
 * Physical vs virtual RAM address space conversion.  These are
 * private definitions which should NOT be used outside memory.h
 * files.  Use virt_to_phys/phys_to_virt/__pa/__va instead.
 */
/*
 * 以下 __lm/__kimg helper 是本头文件的内部算术基元，不应被外部直接使用；
 * 对外应选择 virt_to_phys/phys_to_virt 或 __pa/__va。关键原因是线性映射
 * 和内核镜像使用不同偏移，调用者必须通过统一入口完成地址域判定。
 */


/*
 * Check whether an arbitrary address is within the linear map, which
 * lives in the [PAGE_OFFSET, PAGE_END) interval at the bottom of the
 * kernel's TTBR1 address range.
 */
/*
 * 用无符号减法做单次范围检查：仅当 addr-PAGE_OFFSET 小于区间长度时，
 * 地址才位于 [PAGE_OFFSET, PAGE_END)。低于起点的地址会下溢成大值，
 * 高于终点同样失败，无需两个可能受规范高地址影响的比较。
 */
#define __is_lm_address(addr)	(((u64)(addr) - PAGE_OFFSET) < (PAGE_END - PAGE_OFFSET))

/* 线性映射 VA 去掉 PAGE_OFFSET 后加物理基址，结果单位为物理字节地址。 */
#define __lm_to_phys(addr)	(((addr) - PAGE_OFFSET) + PHYS_OFFSET)
/* 内核镜像运行 VA 减去启动期镜像偏移，得到加载物理地址。 */
#define __kimg_to_phys(addr)	((addr) - kimage_voffset)

/*
 * 无调试虚转物理核心：参数只求值一次，先清除 KASAN/MTE tag，再按
 * 地址是否在线性映射中选择 PHYS_OFFSET 或 kimage_voffset。它不验证
 * 页表 present、RAM 属性或 vmalloc 地址；调用者必须提供有效地址。
 */
#define __virt_to_phys_nodebug(x) ({					\
	phys_addr_t __x = (phys_addr_t)(__tag_reset(x));		\
	__is_lm_address(__x) ? __lm_to_phys(__x) : __kimg_to_phys(__x);	\
})

/* 符号地址明确属于 kernel image，跳过线性映射判定。 */
#define __pa_symbol_nodebug(x)	__kimg_to_phys((phys_addr_t)(x))

#ifdef CONFIG_DEBUG_VIRTUAL
/*
 * DEBUG_VIRTUAL 把转换交给 out-of-line 检查实现，以发现地址域误用；
 * 关闭时直接展开快速算术。两种配置返回值相同，但调试版可能报告 BUG。
 */
extern phys_addr_t __virt_to_phys(unsigned long x);
extern phys_addr_t __phys_addr_symbol(unsigned long x);
#else
#define __virt_to_phys(x)	__virt_to_phys_nodebug(x)
#define __phys_addr_symbol(x)	__pa_symbol_nodebug(x)
#endif /* CONFIG_DEBUG_VIRTUAL */

/* 物理 RAM 地址转换为线性映射 VA；仅对可直接映射范围有意义。 */
#define __phys_to_virt(x)	((unsigned long)((x) - PHYS_OFFSET) | PAGE_OFFSET)
/* 内核镜像物理地址加 kimage_voffset，得到镜像运行 VA。 */
#define __phys_to_kimg(x)	((unsigned long)((x) + kimage_voffset))

/*
 * Note: Drivers should NOT use these.  They are the wrong
 * translation for translating DMA addresses.  Use the driver
 * DMA support - see dma-mapping.h.
 */
/*
 * 完整含义：驱动禁止使用以下 CPU 地址转换来处理 DMA。设备看到的 DMA
 * 地址可能经过 IOMMU、总线窗口或 bounce buffer，并不等于 CPU 物理地址；
 * 驱动必须使用 dma-mapping.h 接口建立映射和同步缓存。
 */
#define virt_to_phys virt_to_phys
/*
 * virt_to_phys - 把有效内核线性/镜像虚址转换为 CPU 物理地址。
 *
 * x 是仅作为数值读取的借用指针，可带 volatile 限定；函数不解引用、不
 * 改变 ownership，任意上下文不睡眠。返回物理字节地址，无 errno。
 * 自指宏让预处理器认为接口已由架构定义，同时保留内联函数的
 * 类型检查。
 */
static inline phys_addr_t virt_to_phys(const volatile void *x)
{
	return __virt_to_phys((unsigned long)(x));
}

#define phys_to_virt phys_to_virt
/*
 * phys_to_virt - 把直接映射 RAM 的 CPU 物理地址转换为线性映射指针。
 *
 * x 单位为物理字节地址，必须属于 direct map 可表示范围；任意上下文纯
 * 计算、不睡眠。返回借用地址值，无映射创建和页引用；对 MMIO/DMA 地址
 * 使用结果无效。
 */
static inline void *phys_to_virt(phys_addr_t x)
{
	return (void *)(__phys_to_virt(x));
}

/* Needed already here for resolving __phys_to_pfn() in virt_to_pfn() */
/*
 * 此处提前包含通用 memory_model，是为了让下面 virt_to_pfn() 能解析
 * __phys_to_pfn()；这是声明依赖顺序，不会在这里创建 mem_map。
 */
#include <asm-generic/memory_model.h>

/*
 * virt_to_pfn - 将有效内核地址转换为绝对物理页帧号。
 *
 * kaddr 是非 NULL 与否由调用者语义决定的借用地址，不解引用；任意上下文
 * 不睡眠。返回 virt_to_phys(kaddr)>>PAGE_SHIFT，页内偏移被丢弃，
 * 无 page 引用和其他副作用。
 */
static inline unsigned long virt_to_pfn(const void *kaddr)
{
	return __phys_to_pfn(virt_to_phys(kaddr));
}

/*
 * Drivers should NOT use these either.
 */
/*
 * 完整含义：驱动同样不应使用 __pa/__va 系列处理 DMA；这些是内核 MM
 * 内部的 CPU 地址域算术。
 *
 * __pa       自动区分线性映射和镜像地址；
 * __pa_symbol 仅接收内核链接符号，RELOC_HIDE 防止编译器基于 C 对象关系
 *             错误折叠重定位算术；
 * __pa_nodebug 强制无调试快速转换；
 * __va       仅生成线性映射地址；
 * pfn_to_kaddr 把绝对 PFN 先还原为物理字节地址；
 * sym_to_pfn 先走镜像符号转换再换算 PFN。
 *
 * 所有宏都不创建映射、不验证 RAM 存在，也不取得 struct page 引用。
 */
#define __pa(x)			__virt_to_phys((unsigned long)(x))
#define __pa_symbol(x)		__phys_addr_symbol(RELOC_HIDE((unsigned long)(x), 0))
#define __pa_nodebug(x)		__virt_to_phys_nodebug((unsigned long)(x))
#define __va(x)			((void *)__phys_to_virt((phys_addr_t)(x)))
#define pfn_to_kaddr(pfn)	__va((pfn) << PAGE_SHIFT)
#define sym_to_pfn(x)		__phys_to_pfn(__pa_symbol(x))

/*
 *  virt_to_page(x)	convert a _valid_ virtual address to struct page *
 *  virt_addr_valid(x)	indicates whether a virtual address is valid
 */
/*
 * virt_to_page() 要求输入是有效线性映射地址并返回对应 struct page 借用
 * 指针；virt_addr_valid() 则先判断任意地址是否同时位于 linear map 且 PFN
 * 属于 memblock 标记的可映射内存。前者的“valid”是调用前置条件，后者才
 * 是检查接口。
 */

#if defined(CONFIG_DEBUG_VIRTUAL)
/*
 * DEBUG_VIRTUAL 路径复用经过检查的 page_to_phys()/virt_to_pfn()。
 * page_to_virt() 的 __page 是输入 struct page 借用，__addr 是无标签线性
 * 地址；返回时恢复该页的 KASAN tag，不增加 page 引用。
 */
#define page_to_virt(x)	({						\
	__typeof__(x) __page = x;					\
	void *__addr = __va(page_to_phys(__page));			\
	(void *)__tag_set((const void *)__addr, page_kasan_tag(__page));\
})
#define virt_to_page(x)		pfn_to_page(virt_to_pfn(x))
#else
/*
 * 非调试路径直接利用 vmemmap 与 linear map 的一一索引关系：
 *
 *   struct page 索引 = (page 地址 - VMEMMAP_START) / sizeof(struct page)
 *   RAM VA           = PAGE_OFFSET + 索引 * PAGE_SIZE
 *
 * 反向转换先清地址 tag，再以 PAGE_SIZE 求页索引，最后按 struct page 大小
 * 定位 vmemmap 槽。__page/__idx/__addr 都只在语句表达式内有效，输入只
 * 求值一次。算术路径更快，但无效地址不会被检测。
 */
#define page_to_virt(x)	({						\
	__typeof__(x) __page = x;					\
	u64 __idx = ((u64)__page - VMEMMAP_START) / sizeof(struct page);\
	u64 __addr = PAGE_OFFSET + (__idx * PAGE_SIZE);			\
	(void *)__tag_set((const void *)__addr, page_kasan_tag(__page));\
})

#define virt_to_page(x)	({						\
	u64 __idx = (__tag_reset((u64)x) - PAGE_OFFSET) / PAGE_SIZE;	\
	u64 __addr = VMEMMAP_START + (__idx * sizeof(struct page));	\
	(struct page *)__addr;						\
})
#endif /* CONFIG_DEBUG_VIRTUAL */

/*
 * virt_addr_valid() 先保存并去除输入 tag，随后要求地址位于 linear map，
 * 且对应 PFN 被 pfn_is_map_memory() 认定为普通可映射内存。它会排除
 * vmalloc、module、fixmap 和 linear map 中的 hole；返回布尔值，不建立
 * 映射或固定页面。
 */
#define virt_addr_valid(addr)	({					\
	__typeof__(addr) __addr = __tag_reset(addr);			\
	__is_lm_address(__addr) && pfn_is_map_memory(virt_to_pfn(__addr));	\
})

/*
 * dump_mem_limit - 输出 arm64 启动期计算出的物理内存限制。
 *
 * 入参：无。由 panic notifier 的 arm64_panic_block_dump() 调用，处于
 * panic 诊断上下文，不可依赖睡眠或普通锁。返回：无直接值；通过 pr_emerg
 * 输出 mem= 限制或 none，不转移对象所有权。
 */
void dump_mem_limit(void);
#endif /* !__ASSEMBLER__ */

/*
 * Given that the GIC architecture permits ITS implementations that can only be
 * configured with a LPI table address once, GICv3 systems with many CPUs may
 * end up reserving a lot of different regions after a kexec for their LPI
 * tables (one per CPU), as we are forced to reuse the same memory after kexec
 * (and thus reserve it persistently with EFI beforehand)
 */
/*
 * GIC ITS 的某些实现只能写一次 LPI 表地址。kexec 后必须复用每 CPU 原表，
 * EFI 因而要在新内核到来前持久保留最多 NR_CPUS 个区域，外加一个公共
 * 区域；初始 reserved memblock 容量相应扩大，避免早期动态扩容。
 */
#if defined(CONFIG_EFI) && defined(CONFIG_ARM_GIC_V3_ITS)
# define INIT_MEMBLOCK_RESERVED_REGIONS	(INIT_MEMBLOCK_REGIONS + NR_CPUS + 1)
#endif

/*
 * memory regions which marked with flag MEMBLOCK_NOMAP(for example, the memory
 * of the EFI_UNUSABLE_MEMORY type) may divide a continuous memory block into
 * multiple parts. As a result, the number of memory regions is large.
 */
/*
 * MEMBLOCK_NOMAP 区域（例如 EFI_UNUSABLE_MEMORY）会把一段连续 RAM 切成
 * 多个可映射片段，EFI 系统因此把初始 memory region 数放大 8 倍，减少
 * 早期解析复杂固件内存图时的扩容。该倍数是容量预算，不改变
 * 物理布局。
 */
#ifdef CONFIG_EFI
#define INIT_MEMBLOCK_MEMORY_REGIONS	(INIT_MEMBLOCK_REGIONS * 8)
#endif


#endif /* __ASM_MEMORY_H */
