/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Linux 内存管理核心头文件
 * Linux Memory Management Core Header File
 *
 * 这是 Linux 内核中最重要的头文件之一，定义了虚拟内存管理的核心数据结构、
 * 宏定义和函数接口。主要包括：
 * - 虚拟内存区域 (VMA) 的管理
 * - 页框 (page frame) 的操作
 * - 页表 (page table) 的处理
 * - 内存分配和释放接口
 * - 内存映射标志和保护位
 *
 * 设计原因：
 * 将所有与内存管理相关的核心定义集中在一个文件中，便于内核其他子系统
 * 引用和使用。采用分层设计，从底层的页框管理到高层的虚拟内存管理。
 */
#ifndef _LINUX_MM_H
#define _LINUX_MM_H

/*
 * 头文件依赖说明 - Header File Dependencies
 *
 * 这些包含的头文件提供了内存管理所需的基础设施：
 */
#include <linux/args.h>          /* 参数处理宏 - Argument handling macros */
#include <linux/errno.h>         /* 错误码定义 - Error code definitions */
#include <linux/mmdebug.h>       /* 内存管理调试接口 - MM debugging interface */
#include <linux/gfp.h>           /* 页面分配标志 - Get Free Pages flags */
#include <linux/pgalloc_tag.h>   /* 页面分配标记 - Page allocation tagging */
#include <linux/bug.h>           /* BUG() 和警告宏 - BUG() and warning macros */
#include <linux/list.h>          /* 链表数据结构 - Linked list data structure */
#include <linux/mmzone.h>        /* 内存区域定义 - Memory zone definitions */
#include <linux/rbtree.h>        /* 红黑树实现 - Red-black tree implementation */
#include <linux/atomic.h>        /* 原子操作 - Atomic operations */
#include <linux/debug_locks.h>   /* 调试锁 - Debug locks */
#include <linux/compiler.h>      /* 编译器相关宏 - Compiler-related macros */
#include <linux/mm_types.h>      /* 内存管理类型定义 - MM type definitions */
#include <linux/mmap_lock.h>     /* mmap 锁机制 - mmap locking mechanism */
#include <linux/range.h>         /* 范围数据结构 - Range data structure */
#include <linux/pfn.h>           /* 页框号操作 - Page Frame Number operations */
#include <linux/percpu-refcount.h> /* Per-CPU 引用计数 - Per-CPU reference counting */
#include <linux/bit_spinlock.h>  /* 位自旋锁 - Bit spinlock */
#include <linux/shrinker.h>      /* 内存回收器 - Memory shrinker */
#include <linux/resource.h>      /* 资源限制 - Resource limits */
#include <linux/page_ext.h>      /* 页面扩展信息 - Page extension info */
#include <linux/err.h>           /* 错误指针 - Error pointers */
#include <linux/page-flags.h>    /* 页面标志位 - Page flags */
#include <linux/page_ref.h>      /* 页面引用计数 - Page reference counting */
#include <linux/overflow.h>      /* 溢出检查 - Overflow checking */
#include <linux/sched.h>         /* 进程调度 - Process scheduling */
#include <linux/pgtable.h>       /* 页表操作 - Page table operations */
#include <linux/kasan.h>         /* 内核地址消毒器 - Kernel Address Sanitizer */
#include <linux/memremap.h>      /* 内存重映射 - Memory remapping */
#include <linux/slab.h>          /* Slab 分配器 - Slab allocator */
#include <linux/cacheinfo.h>     /* 缓存信息 - Cache information */
#include <linux/rcuwait.h>       /* RCU 等待 - RCU wait */
#include <linux/bitmap.h>        /* 位图操作 - Bitmap operations */
#include <linux/bitops.h>        /* 位操作 - Bit operations */
#include <linux/iommu-debug-pagealloc.h> /* IOMMU 调试页面分配 - IOMMU debug page allocation */

/*
 * 前向声明 - Forward Declarations
 *
 * 这些是不完整类型声明，用于解决头文件之间的循环依赖问题。
 * 在 C 语言中，如果两个结构体互相引用，需要使用前向声明。
 */
struct mempolicy;        /* 内存策略（NUMA 相关）- Memory policy for NUMA */
struct anon_vma;         /* 匿名页面的反向映射 - Reverse mapping for anonymous pages */
struct anon_vma_chain;   /* 匿名 VMA 链 - Anonymous VMA chain */
struct user_struct;      /* 用户信息结构 - User information structure */
struct pt_regs;          /* 处理器寄存器状态 - Processor register state */
struct folio_batch;      /* Folio 批处理结构 - Folio batch structure */

/*
 * 内存管理初始化函数 - Memory Management Initialization Functions
 *
 * 这些函数在系统启动时按顺序调用，用于初始化内存管理子系统。
 * 设计原因：分阶段初始化可以处理不同启动阶段的依赖关系。
 */
void arch_mm_preinit(void);      /* 架构相关的内存管理预初始化 - Arch-specific MM pre-init */
void mm_core_init_early(void);   /* 内存管理核心早期初始化 - MM core early init */
void mm_core_init(void);         /* 内存管理核心初始化 - MM core init */
void init_mm_internals(void);    /* 初始化内存管理内部结构 - Init MM internals */

/*
 * 系统总内存页数管理 - Total RAM Pages Management
 *
 * _totalram_pages: 系统中总的可用 RAM 页数（原子变量）
 *
 * 设计原因：
 * 1. 使用 atomic_long_t 确保多核系统中的原子性操作
 * 2. 提供内联函数接口，避免直接访问全局变量
 * 3. 这个计数器在内存热插拔、页面分配等场景中动态更新
 *
 * 注意事项：
 * - 这个值反映的是物理内存页数，不是虚拟内存
 * - 在内存热插拔时会动态变化
 * - 读取操作是原子的，但不保证读到的值在返回后仍然有效（可能被其他 CPU 修改）
 */
extern atomic_long_t _totalram_pages;

/**
 * totalram_pages - 获取系统总内存页数
 *
 * 返回值：当前系统中的总 RAM 页数（无符号长整型）
 *
 * 这是一个只读操作，使用原子读取确保数据一致性。
 */
static inline unsigned long totalram_pages(void)
{
	return (unsigned long)atomic_long_read(&_totalram_pages);
}

/**
 * totalram_pages_inc - 增加系统总内存页数（加 1）
 *
 * 使用场景：内存热插拔添加新页面时调用
 *
 * 注意：这是原子操作，可以在中断上下文中安全调用
 */
static inline void totalram_pages_inc(void)
{
	atomic_long_inc(&_totalram_pages);
}

/**
 * totalram_pages_dec - 减少系统总内存页数（减 1）
 *
 * 使用场景：内存热插拔移除页面时调用
 *
 * 注意：这是原子操作，可以在中断上下文中安全调用
 */
static inline void totalram_pages_dec(void)
{
	atomic_long_dec(&_totalram_pages);
}

/**
 * totalram_pages_add - 批量增减系统总内存页数
 * @count: 要增加的页数（正数）或减少的页数（负数）
 *
 * 使用场景：批量内存操作，避免多次调用 inc/dec
 *
 * 注意：count 可以是负数，此时相当于减少页数
 */
static inline void totalram_pages_add(long count)
{
	atomic_long_add(count, &_totalram_pages);
}

/*
 * 高端内存边界指针 - High Memory Boundary Pointer
 *
 * high_memory: 指向直接映射内存区域的上界
 *
 * 设计原因：
 * 在某些架构上（如 32 位 x86），物理内存可能超过虚拟地址空间。
 * Linux 将物理内存分为：
 * - 低端内存（直接映射到内核虚拟地址空间）
 * - 高端内存（需要动态映射才能访问）
 * high_memory 标记了这个分界点。
 *
 * 注意事项：
 * - 在 64 位系统上，通常所有内存都是直接映射的
 * - 地址低于 high_memory 的可以直接访问
 * - 地址高于 high_memory 的需要使用 kmap() 等函数映射
 */
extern void * high_memory;

/*
 * Convert between pages and MB
 * 20 is the shift for 1MB (2^20 = 1MB)
 * PAGE_SHIFT is the shift for page size (e.g., 12 for 4KB pages)
 * So (20 - PAGE_SHIFT) converts between pages and MB
 * 页面与 MB 之间的转换宏 - Page and MB Conversion Macros
 *
 * 设计原因：
 * 在内核中，内存通常以页为单位管理，但用户界面需要以 MB 显示。
 * 这些宏提供了快速的转换方法。
 *
 * 数学原理（C 语言位运算知识）：
 * - 1 MB = 2^20 字节 = 1048576 字节
 * - 1 页通常是 2^12 字节（4KB）= 4096 字节
 * - 因此：1 MB = 2^20 / 2^12 = 2^(20-12) = 2^8 = 256 页
 * - 左移（<<）相当于乘以 2 的幂次
 * - 右移（>>）相当于除以 2 的幂次
 *
 * PAGE_SHIFT 是页面大小的位移量（通常是 12，表示 4KB 页）
 * 20 是 1MB 的位移量（2^20 = 1MB）
 */

/**
 * PAGES_TO_MB - 将页数转换为 MB 数
 * @pages: 页数
 *
 * 返回值：对应的 MB 数
 *
 * 例如：如果 PAGE_SHIFT=12（4KB 页），PAGES_TO_MB(256) = 1 MB
 */
#define PAGES_TO_MB(pages) ((pages) >> (20 - PAGE_SHIFT))

/**
 * MB_TO_PAGES - 将 MB 数转换为页数
 * @mb: MB 数
 *
 * 返回值：对应的页数
 *
 * 例如：如果 PAGE_SHIFT=12（4KB 页），MB_TO_PAGES(1) = 256 页
 */
#define MB_TO_PAGES(mb)    ((mb) << (20 - PAGE_SHIFT))

/*
 * 虚拟地址空间布局配置 - Virtual Address Space Layout Configuration
 *
 * CONFIG_SYSCTL: 内核配置选项，表示是否启用 sysctl 接口
 *
 * sysctl_legacy_va_layout: 控制是否使用传统的虚拟地址空间布局
 *
 * 设计原因：
 * Linux 支持两种虚拟地址空间布局：
 * 1. 传统布局：堆和栈之间有固定的位置关系
 * 2. 新布局：使用地址空间随机化（ASLR）提高安全性
 *
 * 通过 sysctl 可以在运行时切换，用于兼容性或调试。
 *
 * 注意：如果未启用 CONFIG_SYSCTL，则默认使用新布局（值为 0）
 */
#ifdef CONFIG_SYSCTL
extern int sysctl_legacy_va_layout;
#else
#define sysctl_legacy_va_layout 0
#endif

/*
 * 地址空间随机化（ASLR）配置 - Address Space Layout Randomization
 *
 * 设计原因：
 * ASLR 通过随机化内存映射的基地址，增加攻击者预测内存布局的难度。
 * mmap_rnd_bits 控制随机化的位数（熵），位数越多，地址空间越随机。
 *
 * CONFIG_HAVE_ARCH_MMAP_RND_BITS: 架构支持配置 mmap 随机化位数
 *
 * mmap_rnd_bits_min: 最小随机化位数（编译时常量）
 * mmap_rnd_bits_max: 最大随机化位数（初始化后只读，__ro_after_init）
 * mmap_rnd_bits: 当前使用的随机化位数（可读，__read_mostly 表示大多数时候只读）
 *
 * __ro_after_init: 这是一个编译器标记，表示变量在初始化后变为只读
 *                  这样可以防止运行时被恶意修改，提高安全性
 * __read_mostly: 表示变量大部分时间只读，很少写入
 *                编译器会将这些变量放在同一缓存行，提高性能
 */
#ifdef CONFIG_HAVE_ARCH_MMAP_RND_BITS
extern const int mmap_rnd_bits_min;
extern int mmap_rnd_bits_max __ro_after_init;
extern int mmap_rnd_bits __read_mostly;
#endif

/*
 * 兼容模式的地址空间随机化配置 - ASLR for Compat Mode
 *
 * CONFIG_HAVE_ARCH_MMAP_RND_COMPAT_BITS: 架构支持兼容模式的 ASLR
 *
 * 设计原因：
 * 在 64 位系统上运行 32 位程序时（兼容模式），需要不同的随机化参数。
 * 因为 32 位地址空间较小，不能使用与 64 位相同的随机化位数。
 *
 * mmap_rnd_compat_bits_*: 兼容模式专用的随机化位数配置
 */
#ifdef CONFIG_HAVE_ARCH_MMAP_RND_COMPAT_BITS
extern const int mmap_rnd_compat_bits_min;
extern const int mmap_rnd_compat_bits_max;
extern int mmap_rnd_compat_bits __read_mostly;
#endif

/*
 * 直接映射物理内存的上限 - Direct Map Physical Memory End
 *
 * 设计原因：
 * 内核需要知道哪些物理地址可以被直接映射到虚拟地址空间。
 * 这个宏定义了物理内存直接映射区域的结束地址。
 *
 * MAX_PHYSMEM_BITS: 系统支持的最大物理地址位数
 * 例如：如果是 46 位，则最大物理地址是 2^46 - 1
 *
 * C 语言知识：
 * - 1ULL: 无符号长长整型常量 1（64 位）
 * - << : 左移运算符
 * - (1ULL << 46) 表示 2^46
 * - ~(1ULL<<63): 对位 63 取反，用于清除符号位（某些架构需要）
 * - phys_addr_t: 物理地址类型（可能是 32 位或 64 位，取决于架构）
 * - (type)-1: 将 -1 转换为无符号类型，得到该类型的最大值（全 1）
 */
#ifndef DIRECT_MAP_PHYSMEM_END
# ifdef MAX_PHYSMEM_BITS
# define DIRECT_MAP_PHYSMEM_END	((1ULL << MAX_PHYSMEM_BITS) - 1)
# else
# define DIRECT_MAP_PHYSMEM_END	(((phys_addr_t)-1)&~(1ULL<<63))
# endif
#endif

/*
 * 无效物理地址标记 - Invalid Physical Address Marker
 *
 * INVALID_PHYS_ADDR: 表示一个无效的物理地址
 *
 * 设计原因：
 * 在某些情况下需要表示"无效"或"未分配"的物理地址。
 * 使用全 1 作为无效标记是一个常见做法，因为这通常不是有效的物理地址。
 *
 * C 语言知识：
 * - ~0: 按位取反，得到全 1
 * - (phys_addr_t)~0: 将全 1 转换为 phys_addr_t 类型
 */
#define INVALID_PHYS_ADDR (~(phys_addr_t)0)

/*
 * 架构相关的头文件包含 - Architecture-specific Headers
 *
 * 这些头文件必须在定义了基本宏之后包含，因为它们可能依赖前面的定义。
 */
#include <asm/page.h>       /* 架构相关的页面定义 - Arch-specific page definitions */
#include <asm/processor.h>  /* 架构相关的处理器定义 - Arch-specific processor definitions */

/*
 * 虚拟地址与物理地址转换宏 - Virtual/Physical Address Conversion
 *
 * 设计原因：
 * 内核需要在虚拟地址和物理地址之间进行转换。不同架构有不同的实现，
 * 这里提供默认实现（如果架构没有定义自己的版本）。
 *
 * __pa(): 虚拟地址转物理地址 (Virtual Address to Physical Address)
 * __va(): 物理地址转虚拟地址 (Physical Address to Virtual Address)
 *
 * C 语言知识：
 * - #ifndef: 如果未定义（if not defined）
 * - RELOC_HIDE: 隐藏重定位信息，防止编译器优化干扰地址计算
 */

/**
 * __pa_symbol - 将内核符号地址转换为物理地址
 * @x: 内核符号的虚拟地址
 *
 * 设计原因：
 * 内核符号（如全局变量、函数）的地址需要特殊处理，因为它们可能
 * 在链接时被重定位。RELOC_HIDE 确保编译器不会对地址计算做假设。
 *
 * 返回值：对应的物理地址
 */
#ifndef __pa_symbol
#define __pa_symbol(x)  __pa(RELOC_HIDE((unsigned long)(x), 0))
#endif

/**
 * page_to_virt - 将 page 结构转换为对应的虚拟地址
 * @x: 指向 struct page 的指针
 *
 * 转换步骤（C 语言函数调用链）：
 * 1. page_to_pfn(x): 将 page 结构转换为页框号 (Page Frame Number)
 * 2. PFN_PHYS(): 将页框号转换为物理地址（乘以页面大小）
 * 3. __va(): 将物理地址转换为虚拟地址
 *
 * 设计原因：
 * struct page 是内核用来描述物理页面的结构，但访问页面内容需要虚拟地址。
 *
 * 返回值：页面内容的内核虚拟地址
 */
#ifndef page_to_virt
#define page_to_virt(x)	__va(PFN_PHYS(page_to_pfn(x)))
#endif

/**
 * lm_alias - 获取内核符号的线性映射别名
 * @x: 内核符号地址
 *
 * 设计原因：
 * 在某些架构上，同一物理内存可能有多个虚拟地址映射：
 * - 内核代码段的映射（带执行权限）
 * - 线性映射区域（用于数据访问）
 *
 * 这个宏返回符号在线性映射区域的地址，用于数据访问而非代码执行。
 *
 * 返回值：线性映射区域的虚拟地址
 */
#ifndef lm_alias
#define lm_alias(x)	__va(__pa_symbol(x))
#endif

/*
 * To prevent common memory management code establishing
 * a zero page mapping on a read fault.
 * This macro should be defined within <asm/pgtable.h>.
 * s390 does this to prevent multiplexing of hardware bits
 * related to the physical page in case of virtualization.
 * 零页面映射控制 - Zero Page Mapping Control
 *
 * mm_forbids_zeropage - 检查是否禁止零页面映射
 * @X: 内存描述符 (mm_struct)
 *
 * 设计原因：
 * Linux 有一个优化：当进程读取未初始化的内存时，可以映射到一个共享的
 * "零页面"（全零内容），避免分配真实的物理页面（写时复制 COW）。
 *
 * 但在某些架构（如 s390 虚拟化环境）上，这个优化会导致问题：
 * - 硬件位复用：物理页面的某些硬件位可能被复用
 * - 虚拟化冲突：多个虚拟地址映射到同一物理页会混淆硬件位
 *
 * 这个宏允许架构禁用零页面优化。
 *
 * 默认返回 0（允许零页面），架构可以在 <asm/pgtable.h> 中重新定义。
 *
 * 注意：s390 是唯一需要禁用此功能的架构。
 */
#ifndef mm_forbids_zeropage
#define mm_forbids_zeropage(X)	(0)
#endif

/*
 * On some architectures it is expensive to call memset() for small sizes.
 * If an architecture decides to implement their own version of
 * mm_zero_struct_page they should wrap the defines below in a #ifndef and
 * define their own version of this macro in <asm/pgtable.h>

 * struct page 快速清零优化 - Fast Zero-out for struct page
 *
 * 设计原因：
 * 在分配新页面时，需要将 struct page 清零以避免使用未初始化的数据。
 * memset() 对于小结构体来说开销较大（函数调用、循环等）。
 *
 * 64 位系统的优化策略：
 * 1. 将 struct page 看作 unsigned long 数组（每个元素 8 字节）
 * 2. 使用 switch-case 根据结构体大小展开为直接赋值
 * 3. 编译器会优化掉 switch，只留下实际需要的赋值语句
 * 4. 编译器还可能合并连续的赋值为更高效的批量写入
 *
 * C 语言知识：
 * - BITS_PER_LONG: 每个 long 类型的位数（64 位系统是 64）
 * - BUILD_BUG_ON: 编译时断言，如果条件为真则编译失败
 * - fallthrough: C 语言 switch 的"贯穿"，继续执行下一个 case
 * - (void *)page: 将 page 指针转换为 void* 再转为 unsigned long*，
 *                 这样可以按 8 字节为单位访问内存
 *
 * 注意事项：
 * - 这个函数假设 struct page 的大小是 56, 64, 72, 80, 88 或 96 字节之一
 * - 大小必须是 8 的倍数（通过 BUILD_BUG_ON 检查）
 * - 如果 struct page 大小改变，必须更新这个函数
 * - 架构可以在 <asm/pgtable.h> 中定义自己的版本
 */
#if BITS_PER_LONG == 64
/*
 * 当 struct page 超过 96 字节或小于 56 字节时，必须更新此函数。
 * This function must be updated when the size of struct page grows above 96
 * or reduces below 56.
 *
 * 编译器优化说明：
 * 编译器会优化掉 switch 语句，只保留 move/store 指令。
 * 如果多个写操作可以重排序且都是赋值，编译器还会合并它们。
 * The idea that compiler optimizes out switch() statement, and only leaves
 * move/store instructions. Also the compiler can combine write statements
 * if they are both assignments and can be reordered, this can result in
 * several of the writes here being dropped.
 */
#define	mm_zero_struct_page(pp) __mm_zero_struct_page(pp)
static inline void __mm_zero_struct_page(struct page *page)
{
	unsigned long *_pp = (void *)page;  /* 将 page 转换为 unsigned long 数组 */

	 /*
	  * 检查 struct page 的大小是否符合预期
	  * Check that struct page is either 56, 64, 72, 80, 88 or 96 bytes
	  *
	  * sizeof(struct page) & 7: 检查大小是否是 8 的倍数
	  * 如果不是 8 的倍数，& 7 的结果非零，BUILD_BUG_ON 会触发编译错误
	  */
	BUILD_BUG_ON(sizeof(struct page) & 7);
	BUILD_BUG_ON(sizeof(struct page) < 56);
	BUILD_BUG_ON(sizeof(struct page) > 96);

	/*
	 * 根据 struct page 的实际大小，清零对应数量的 8 字节元素
	 * switch 会被编译器优化为直接跳转到对应的 case
	 */
	switch (sizeof(struct page)) {
	case 96:
		_pp[11] = 0;  /* 清零第 12 个 8 字节（88-95 字节）*/
		fallthrough;  /* 继续执行下一个 case */
	case 88:
		_pp[10] = 0;  /* 清零第 11 个 8 字节（80-87 字节）*/
		fallthrough;
	case 80:
		_pp[9] = 0;   /* 清零第 10 个 8 字节（72-79 字节）*/
		fallthrough;
	case 72:
		_pp[8] = 0;   /* 清零第 9 个 8 字节（64-71 字节）*/
		fallthrough;
	case 64:
		_pp[7] = 0;   /* 清零第 8 个 8 字节（56-63 字节）*/
		fallthrough;
	case 56:
		/* 前 56 字节（7 个 8 字节元素）总是需要清零 */
		_pp[6] = 0;   /* 清零第 7 个 8 字节（48-55 字节）*/
		_pp[5] = 0;   /* 清零第 6 个 8 字节（40-47 字节）*/
		_pp[4] = 0;   /* 清零第 5 个 8 字节（32-39 字节）*/
		_pp[3] = 0;   /* 清零第 4 个 8 字节（24-31 字节）*/
		_pp[2] = 0;   /* 清零第 3 个 8 字节（16-23 字节）*/
		_pp[1] = 0;   /* 清零第 2 个 8 字节（8-15 字节）*/
		_pp[0] = 0;   /* 清零第 1 个 8 字节（0-7 字节）*/
	}
}
#else
/*
 * 32 位系统或其他情况：直接使用 memset
 *
 * 设计原因：
 * 在 32 位系统上，上述优化效果不明显，直接用 memset 更简单。
 *
 * C 语言知识：
 * - ((void)expr): 将表达式结果转为 void，表示忽略返回值
 * - memset(ptr, 0, size): 将内存区域填充为 0
 */
#define mm_zero_struct_page(pp)  ((void)memset((pp), 0, sizeof(struct page)))
#endif

/*
 * Default maximum number of active map areas, this limits the number of vmas
 * per mm struct. Users can overwrite this number by sysctl but there is a
 * problem.
 * 默认最大内存映射区域数量 - Default Maximum Map Count
 *
 * 设计原因：
 * 每个进程都有一个 mm_struct 结构来管理其虚拟内存，其中包含多个
 * vm_area_struct (VMA)，每个 VMA 代表一个连续的虚拟内存区域。
 *
 * 为什么需要限制 VMA 数量？
 * 1. 内存开销：每个 VMA 占用内核内存（约 200 字节）
 * 2. 性能影响：VMA 数量过多会降低查找和操作效率
 * 3. Coredump 限制：见下文详细说明
 *
 * ELF Coredump 的限制：
 * - 当程序崩溃时，内核会生成 coredump 文件（ELF 格式）
 * - ELF 文件中，每个 VMA 对应一个 section（段）
 * - 传统 ELF 格式中，section 数量用 unsigned short (16 位) 表示
 * - unsigned short 最大值是 65535 (USHRT_MAX)
 * - 但内核会添加一些额外的 section（如 NOTE segment，包含寄存器状态等）
 * - 这些额外 section 数量为 1-3 个，取决于架构
 * - 因此需要预留 5 个作为安全边界
 *
 * C 语言知识：
 * - USHRT_MAX: unsigned short 的最大值（65535）
 * - #define: 预处理器宏定义
 *
 * 注意事项：
 * - ELF 扩展编号格式可以支持超过 65535 个 section，但某些用户空间
 *   工具可能不支持，会感到"惊讶"（崩溃或错误）
 * - 用户可以通过 sysctl 修改这个限制（/proc/sys/vm/max_map_count）
 * - 某些应用（如 Java 虚拟机）可能需要大量 VMA，需要调整此限制
 *
 * When a program's coredump is generated as ELF format, a section is created
 * per a vma. In ELF, the number of sections is represented in unsigned short.
 * This means the number of sections should be smaller than 65535 at coredump.
 * Because the kernel adds some informative sections to a image of program at
 * generating coredump, we need some margin. The number of extra sections is
 * 1-3 now and depends on arch. We use "5" as safe margin, here.
 *
 * ELF extended numbering allows more than 65535 sections, so 16-bit bound is
 * not a hard limit any more. Although some userspace tools can be surprised by
 * that.
 */
#define MAPCOUNT_ELF_CORE_MARGIN	(5)
#define DEFAULT_MAX_MAP_COUNT	(USHRT_MAX - MAPCOUNT_ELF_CORE_MARGIN)

/*
 * 内存预留配置 - Memory Reservation Configuration
 *
 * sysctl_user_reserve_kbytes: 为普通用户预留的内存（KB）
 * sysctl_admin_reserve_kbytes: 为管理员（root）预留的内存（KB）
 *
 * 设计原因：
 * 当系统内存不足时，需要预留一些内存以保证：
 * 1. 管理员可以登录系统进行故障排除（需要启动 shell、SSH 等）
 * 2. 普通用户的关键进程可以继续运行
 * 3. 避免完全的系统死锁（所有进程都因内存不足而无法执行）
 *
 * 这些值可以通过 sysctl 调整：
 * - /proc/sys/vm/user_reserve_kbytes
 * - /proc/sys/vm/admin_reserve_kbytes
 *
 * 注意：预留内存不是"锁定"的，而是 OOM killer 在选择杀死进程时的参考
 */
extern unsigned long sysctl_user_reserve_kbytes;
extern unsigned long sysctl_admin_reserve_kbytes;

/*
 * 页面范围连续性检查 - Page Range Contiguity Check
 *
 * CONFIG_SPARSEMEM: 稀疏内存模型配置选项
 * CONFIG_SPARSEMEM_VMEMMAP: 稀疏内存的虚拟内存映射优化
 *
 * 设计原因：
 * Linux 支持三种内存模型来管理物理内存：
 * 1. FLATMEM（平坦内存）：假设物理内存是连续的
 * 2. DISCONTIGMEM（不连续内存）：支持内存空洞，但已过时
 * 3. SPARSEMEM（稀疏内存）：最灵活，支持内存热插拔和大量空洞
 *
 * 在 SPARSEMEM 模式下，物理内存被分为多个"section"。
 * struct page 数组可能不是物理连续的，因此需要检查页面范围的连续性。
 *
 * VMEMMAP 优化：
 * - 将所有 struct page 映射到一个连续的虚拟地址空间
 * - 即使物理内存不连续，struct page 数组在虚拟地址上也是连续的
 * - 启用 VMEMMAP 后，页面范围总是"连续的"（从虚拟地址角度）
 *
 * 函数说明：
 * @page: 起始页面
 * @nr_pages: 页面数量
 * 返回值：true 表示这个范围内的 struct page 是连续的
 *
 * 使用场景：
 * - 批量页面操作前需要确认 struct page 数组可以作为连续数组访问
 * - 某些硬件 DMA 操作需要物理连续的页面
 */
#if defined(CONFIG_SPARSEMEM) && !defined(CONFIG_SPARSEMEM_VMEMMAP)
bool page_range_contiguous(const struct page *page, unsigned long nr_pages);
#else
/*
 * 非 SPARSEMEM 或启用了 VMEMMAP：页面范围总是连续的
 * 这个函数会被编译器优化为常量，不产生任何代码
 */
static inline bool page_range_contiguous(const struct page *page,
		unsigned long nr_pages)
{
	return true;
}
#endif

/*
 * 页面对齐宏 - Page Alignment Macros
 *
 * 设计原因：
 * 内存管理中经常需要将地址对齐到页边界。这些宏提供便捷的对齐操作。
 *
 * C 语言知识：
 * - ALIGN(x, a): 将 x 向上对齐到 a 的倍数
 *   例如：ALIGN(4097, 4096) = 8192
 * - ALIGN_DOWN(x, a): 将 x 向下对齐到 a 的倍数
 *   例如：ALIGN_DOWN(4097, 4096) = 4096
 * - IS_ALIGNED(x, a): 检查 x 是否已经对齐到 a
 *   例如：IS_ALIGNED(4096, 4096) = true
 *
 * PAGE_SIZE: 页面大小（通常是 4096 字节，即 4KB）
 */

/**
 * PAGE_ALIGN - 将地址向上对齐到下一个页边界
 * @addr: 要对齐的地址
 *
 * 例如：PAGE_ALIGN(0x1001) = 0x2000 (假设 PAGE_SIZE=4096)
 *
 * 使用场景：分配内存时需要按页对齐
 *
 * to align the pointer to the (next) page boundary
 */
#define PAGE_ALIGN(addr) ALIGN(addr, PAGE_SIZE)

/**
 * PAGE_ALIGN_DOWN - 将地址向下对齐到前一个页边界
 * @addr: 要对齐的地址
 *
 * 例如：PAGE_ALIGN_DOWN(0x1001) = 0x1000 (假设 PAGE_SIZE=4096)
 *
 * 使用场景：计算地址所在的页起始位置
 *
 * to align the pointer to the (prev) page boundary
 */
#define PAGE_ALIGN_DOWN(addr) ALIGN_DOWN(addr, PAGE_SIZE)

/**
 * PAGE_ALIGNED - 测试地址是否已经页对齐
 * @addr: 要测试的地址（可以是 unsigned long 或指针）
 *
 * 返回值：true 表示地址是页面大小的整数倍
 *
 * 例如：PAGE_ALIGNED(0x1000) = true
 *       PAGE_ALIGNED(0x1001) = false
 *
 * test whether an address (unsigned long or pointer) is aligned to PAGE_SIZE
 */
#define PAGE_ALIGNED(addr)	IS_ALIGNED((unsigned long)(addr), PAGE_SIZE)


/**
 * folio_page_idx - Return the number of a page in a folio.
 * @folio: The folio.
 * @page: The folio page.
 * folio_page_idx - 返回页面在 folio 中的索引号
 * @folio: 指向 folio 的指针
 * @page: 指向 folio 中某一页的指针
 *
 * Folio 概念说明：
 * - Folio 是 Linux 5.16+ 引入的新抽象，代表一组连续的物理页面
 * - 设计目的：统一处理单页和复合页（compound page），简化内存管理代码
 * - 一个 folio 可以是：
 *   * 单个页面（4KB）
 *   * 透明大页（THP，如 2MB）
 *   * 大页（Huge pages）
 *
 * 设计原因：
 * 传统上，内核使用 struct page 管理内存，但对于大页需要特殊处理。
 * Folio 提供了统一的接口，无论底层是单页还是多页。
 *
 * 函数实现说明：
 * - folio->page: folio 结构的第一个成员是 struct page
 * - page - &folio->page: 指针相减得到元素间隔（数组索引）
 *
 * C 语言知识：
 * - 指针相减：两个同类型指针相减得到它们之间的元素个数
 *   例如：int arr[5]; int *p1 = &arr[1], *p2 = &arr[3];
 *        p2 - p1 = 2（相差 2 个元素）
 * - &folio->page: 获取 folio 第一个页面的地址
 * - page - &folio->page: 计算页面在 folio 中的偏移
 *
 * 返回值：页面在 folio 中的索引（从 0 开始）
 *
 * 使用场景：
 * - 需要知道一个页面在其所属 folio 中的位置
 * - 用于计算页内偏移或进行页面操作
 *
 * 注意事项：
 * - 这个函数假设 page 确实属于 folio（不做越界检查）
 * - 调用者需要保证 page 指针有效
 * - 返回值相对于 folio 的起始位置，不是全局页框号
 *
 * Return: the index of the page in the folio; the offset in pages.
 * This function expects that the page is actually part of the folio.
 * The returned number is relative to the start of the folio.
 */
static inline unsigned long folio_page_idx(const struct folio *folio,
		const struct page *page)
{
	return page - &folio->page;
}

/**
 * lru_to_folio - 从 LRU 链表节点获取对应的 folio
 * @head: LRU 链表头
 *
 * LRU（Least Recently Used）概念说明：
 * - 内核使用 LRU 链表管理页面回收
 * - 最近使用的页面在链表前端，最久未使用的在尾部
 * - 当内存不足时，优先回收 LRU 尾部的页面
 *
 * 设计原因：
 * - struct folio 包含一个 lru 成员（struct list_head）用于链入 LRU 链表
 * - 给定链表节点，需要找到包含它的 folio 结构
 *
 * C 语言知识：
 * - list_entry(): 这是 Linux 内核的容器宏（container_of 的封装）
 * - head->prev: 链表的尾部（最久未使用的页面）
 * - 原理：已知结构体成员的地址，反推结构体首地址
 *
 * 返回值：包含该 LRU 节点的 folio 指针
 *
 * 使用场景：
 * - 页面回收（page reclaim）
 * - 遍历 LRU 链表进行内存整理
 */
static inline struct folio *lru_to_folio(struct list_head *head)
{
	return list_entry((head)->prev, struct folio, lru);
}

/**
 * setup_initial_init_mm - 设置初始进程的内存描述符
 * @start_code: 代码段起始地址
 * @end_code: 代码段结束地址
 * @end_data: 数据段结束地址
 * @brk: 堆的起始/结束地址
 *
 * 设计原因：
 * - 内核启动时需要为初始进程（init_mm）设置内存布局
 * - init_mm 是所有内核线程共享的内存描述符
 * - 这个函数在内核启动早期调用，设置内核代码和数据的地址范围
 *
 * 参数说明：
 * - start_code: 内核代码段的起始虚拟地址（通常是 _text 符号）
 * - end_code: 内核代码段的结束虚拟地址（通常是 _etext 符号）
 * - end_data: 内核数据段的结束虚拟地址（通常是 _edata 符号）
 * - brk: 堆的当前位置（通常是 _end 符号）
 *
 * 注意：这些是内核自身的内存布局，不是用户进程的
 */
void setup_initial_init_mm(void *start_code, void *end_code,
			   void *end_data, void *brk);


/*
 * Linux 内核虚拟内存管理原语 - Linux Kernel Virtual Memory Manager Primitives
 *
 * 设计哲学：
 * 内核的目标是提供一个"虚拟"的内存管理器，类似于提供"虚拟"文件系统。
 * 这样做的好处：
 * 1. 为内存管理提供清晰的接口，隐藏底层细节
 * 2. 允许不同类型的内存映射（共享内存、可执行文件加载、任意 mmap 等）
 * 3. 统一的抽象层，便于移植和维护
 *
 * The idea being to have a "virtual" mm in the same way
 * we have a virtual fs - giving a cleaner interface to the
 * mm details, and allowing different kinds of memory mappings
 * (from shared memory to executable loading to arbitrary
 * mmap() functions).
 */

/**
 * vm_area_alloc - 分配一个新的 VMA 结构
 * @mm: 所属的内存描述符
 *
 * VMA (Virtual Memory Area) 说明：
 * - 每个 vm_area_struct 代表进程虚拟地址空间中的一个连续区域
 * - VMA 包含：起始地址、结束地址、访问权限、映射类型等信息
 * - 一个进程通常有多个 VMA，例如：
 *   * 代码段 VMA（可读可执行）
 *   * 数据段 VMA（可读可写）
 *   * 堆 VMA（可读可写，可扩展）
 *   * 栈 VMA（可读可写，向下增长）
 *   * 共享库映射 VMA
 *   * mmap 创建的 VMA
 *
 * 设计原因：
 * 使用独立的分配函数而不是直接 kmalloc，便于：
 * 1. 统一的内存统计和调试
 * 2. 可以使用专门的 slab 缓存优化性能
 * 3. 未来可以改变分配策略而不影响调用者
 *
 * 返回值：新分配的 VMA 指针，失败返回 NULL
 */
struct vm_area_struct *vm_area_alloc(struct mm_struct *);

/**
 * vm_area_dup - 复制一个 VMA 结构
 * @vma: 要复制的源 VMA
 *
 * 使用场景：
 * - fork() 系统调用：子进程需要复制父进程的所有 VMA
 * - mremap() 时可能需要分裂或复制 VMA
 *
 * 设计原因：
 * - 不能直接 memcpy，因为 VMA 包含引用计数、链表节点等需要特殊处理的成员
 * - 某些字段需要重新初始化（如锁、引用计数）
 * - 某些字段需要增加引用计数（如映射的文件）
 *
 * 返回值：新的 VMA 副本，失败返回 NULL
 */
struct vm_area_struct *vm_area_dup(struct vm_area_struct *);

/**
 * vm_area_free - 释放 VMA 结构
 * @vma: 要释放的 VMA
 *
 * 设计原因：
 * - 与 vm_area_alloc 配对使用
 * - 不能直接 kfree，需要处理：
 *   * 释放对文件的引用（如果是文件映射）
 *   * 从红黑树和链表中移除
 *   * 更新统计信息
 *
 * 注意事项：
 * - 调用前必须确保 VMA 已经从所有数据结构中移除
 * - 必须持有适当的锁（mmap_lock）
 */
void vm_area_free(struct vm_area_struct *);

/*
 * 无 MMU 系统的特殊支持 - Support for Systems without MMU
 *
 * CONFIG_MMU: 内核配置选项，表示是否有内存管理单元（MMU）
 *
 * MMU（Memory Management Unit）说明：
 * - MMU 是硬件组件，负责虚拟地址到物理地址的转换
 * - 有 MMU 的系统：可以使用虚拟内存、页保护、按需分页等
 * - 无 MMU 的系统（如某些嵌入式设备）：程序直接使用物理地址
 *
 * 设计原因：
 * Linux 需要同时支持有 MMU 和无 MMU 的系统。
 * 无 MMU 系统需要不同的内存管理策略。
 */
#ifndef CONFIG_MMU
/*
 * nommu_region_tree: 无 MMU 系统的内存区域红黑树
 *
 * 设计原因：
 * - 无 MMU 系统中，需要跟踪所有已分配的物理内存区域
 * - 使用红黑树实现快速查找和插入（O(log n)复杂度）
 * - 避免地址空间碎片和重叠分配
 *
 * rb_root: 红黑树的根节点类型
 */
extern struct rb_root nommu_region_tree;

/*
 * nommu_region_sem: 保护 nommu_region_tree 的读写信号量
 *
 * C 语言并发知识：
 * - rw_semaphore: 读写信号量，允许多个读者或一个写者
 * - 读操作（查找）：多个 CPU 可以同时进行
 * - 写操作（插入/删除）：独占访问
 *
 * 设计原因：
 * 读操作比写操作频繁，使用读写锁提高并发性能。
 */
extern struct rw_semaphore nommu_region_sem;

/**
 * kobjsize - 返回内核对象的大小
 * @objp: 对象指针
 *
 * 设计原因：
 * - 无 MMU 系统中，某些操作需要知道已分配对象的大小
 * - 有 MMU 的系统可以通过页表获取这些信息
 * - 无 MMU 系统需要显式跟踪
 *
 * 返回值：对象占用的字节数
 *
 * 使用场景：
 * - 内存统计和调试
 * - 释放内存时需要知道大小
 *
 * 返回值：对象的字节大小
 *
 * 注意：这个函数只在无 MMU 系统中存在
 */
extern unsigned int kobjsize(const void *objp);
#endif

/*
 * VMA 标志位定义 - VM Flags in vm_area_struct
 *
 * 重要提示：
 * 修改这些标志时，必须同时更新 include/trace/events/mmflags.h
 * 否则 tracing 系统会显示错误的标志名称。
 *
 * vm_flags in vm_area_struct, see mm_types.h.
 * When changing, update also include/trace/events/mmflags.h
 */

/*
 * VM_NONE: 无特殊标志（空标志）
 *
 * 设计原因：
 * 提供一个明确的"无标志"常量，比使用 0 更清晰。
 */
#define VM_NONE		0x00000000

/**
 * typedef vma_flag_t - specifies an individual VMA flag by bit number.
 * vma_flag_t - VMA 标志位类型
 *
 * 设计原因：
 * 使用类型安全的枚举来定义标志位，而不是普通整数。
 * 这样可以：
 * 1. 让 sparse 工具检查类型错误（传递了错误的标志类型）
 * 2. 防止意外地将标志值与其他整数混用
 * 3. 提供更好的文档和代码可读性
 *
 * C 语言知识：
 * - typedef: 定义类型别名
 * - __bitwise: sparse 工具的注解，用于类型检查
 * - 这不是标准 C，而是 Linux 内核使用的扩展
 *
 * This value is made type safe by sparse to avoid passing invalid flag values
 * around.
 */
typedef int __bitwise vma_flag_t;

/*
 * VMA 标志位声明宏 - VMA Flag Declaration Macros
 *
 * 设计原因：
 * 使用宏来声明标志位，提供统一的命名规范和类型转换。
 *
 * C 语言宏知识：
 * - ## : 宏连接符，用于拼接标识符
 * - __force: sparse 注解，表示强制类型转换（忽略 __bitwise 检查）
 */

/**
 * DECLARE_VMA_BIT - 声明一个 VMA 标志位
 * @name: 标志位名称（如 READ, WRITE）
 * @bitnum: 位号（0-63）
 *
 * 例如：DECLARE_VMA_BIT(READ, 0) 展开为：
 *       VMA_READ_BIT = ((__force vma_flag_t)0)
 */
#define DECLARE_VMA_BIT(name, bitnum) \
	VMA_ ## name ## _BIT = ((__force vma_flag_t)bitnum)

/**
 * DECLARE_VMA_BIT_ALIAS - 声明一个标志位别名
 * @name: 新标志位名称
 * @aliased: 要别名的标志位名称
 *
 * 设计原因：
 * 某些标志位在不同架构上有不同的含义，但使用相同的位。
 * 使用别名可以提供语义明确的名称。
 *
 * 例如：DECLARE_VMA_BIT_ALIAS(GROWSUP, ARCH_1)
 *       表示 VMA_GROWSUP_BIT 与 VMA_ARCH_1_BIT 是同一个位
 */
#define DECLARE_VMA_BIT_ALIAS(name, aliased) \
	VMA_ ## name ## _BIT = (VMA_ ## aliased ## _BIT)
/*
 * VMA 标志位枚举定义 - VMA Flag Bit Enumeration
 *
 * 这个枚举定义了所有 VMA 标志位的位号。
 * 每个标志控制虚拟内存区域的不同属性和行为。
 *
 * 设计原因：
 * 使用枚举而不是 #define 可以：
 * 1. 让编译器自动分配位号（减少人为错误）
 * 2. 提供更好的调试信息
 * 3. 方便工具（如 gdb）显示符号名称
 */
enum {
	/*
	 * 基本访问权限标志 - Basic Access Permission Flags (0-3)
	 *
	 * 这些标志控制内存区域的读/写/执行权限和共享属性。
	 * 它们对应于 mmap() 系统调用的 PROT_* 标志。
	 */
	DECLARE_VMA_BIT(READ, 0),    /* 可读 - Readable */
	DECLARE_VMA_BIT(WRITE, 1),   /* 可写 - Writable */
	DECLARE_VMA_BIT(EXEC, 2),    /* 可执行 - Executable */
	DECLARE_VMA_BIT(SHARED, 3),  /* 共享映射 - Shared mapping (vs private) */

	/*
	 * 权限上限标志 - Permission Limit Flags (4-7)
	 *
	 * 设计原因：
	 * mprotect() 系统调用可以修改内存区域的保护属性，但不能超过这些上限。
	 * 例如：如果 MAYWRITE 未设置，mprotect() 永远不能使该区域可写。
	 *
	 * 重要实现细节：
	 * mprotect() 的实现硬编码了 "VM_MAYREAD >> 4 == VM_READ"
	 * 这意味着 MAY* 标志必须比对应的权限标志大 4 位。
	 * 因此这些位号是固定的，不能随意修改！
	 *
	 * mprotect() hardcodes VM_MAYREAD >> 4 == VM_READ, and so for r/w/x bits.
	 */
	DECLARE_VMA_BIT(MAYREAD, 4),   /* mprotect 可以设置为可读 - Limits for mprotect() */
	DECLARE_VMA_BIT(MAYWRITE, 5),  /* mprotect 可以设置为可写 */
	DECLARE_VMA_BIT(MAYEXEC, 6),   /* mprotect 可以设置为可执行 */
	DECLARE_VMA_BIT(MAYSHARE, 7),  /* 可以变为共享映射 */

	/*
	 * 段增长方向标志 - Segment Growth Direction (8)
	 *
	 * GROWSDOWN: 段向下增长（用于栈）
	 *
	 * 设计原因：
	 * - 栈通常从高地址向低地址增长
	 * - 当访问栈下方的地址时，内核会自动扩展栈
	 * - 这个标志告诉内核这个 VMA 可以自动向下扩展
	 *
	 * general info on the segment
	 */
	DECLARE_VMA_BIT(GROWSDOWN, 8),

#ifdef CONFIG_MMU
	/*
	 * userfaultfd 缺失页跟踪 - userfaultfd Missing Page Tracking (9)
	 *
	 * UFFD_MISSING: 使用 userfaultfd 跟踪缺失的页面
	 *
	 * userfaultfd 概念：
	 * - 用户空间缺页处理机制
	 * - 允许用户空间程序处理页面错误，而不是内核
	 * - 用途：实时迁移虚拟机、用户空间内存管理等
	 *
	 * 设计原因：
	 * 当访问带有此标志的区域中不存在的页面时，内核会通知
	 * userfaultfd，而不是立即分配页面或触发 SIGSEGV。
	 *
	 * missing pages tracking
	 */
	DECLARE_VMA_BIT(UFFD_MISSING, 9),
#else
	/*
	 * nommu（无 MMU）系统的特殊标志
	 *
	 * MAYOVERLAY: 只读的 MAP_PRIVATE 映射可能覆盖文件映射
	 *
	 * 设计原因：
	 * 在无 MMU 系统中，无法实现真正的写时复制（COW）。
	 * 这个标志表示该区域可能直接映射到文件，而不是复制。
	 *
	 * nommu: R/O MAP_PRIVATE mapping that might overlay a file mapping
	 */
	DECLARE_VMA_BIT(MAYOVERLAY, 9),
#endif /* CONFIG_MMU */

	/*
	 * 特殊映射类型标志 - Special Mapping Type Flags (10-11)
	 */

	/**
	 * PFNMAP: 页框号映射（不使用 struct page）
	 *
	 * 设计原因：
	 * 某些内存区域（如设备内存、保留内存）没有对应的 struct page。
	 * 这些区域直接使用物理页框号（PFN）进行映射。
	 *
	 * 使用场景：
	 * - 设备驱动的 mmap（映射设备寄存器或 DMA 缓冲区）
	 * - /dev/mem 访问
	 * - 某些特殊的内核映射
	 *
	 * 注意事项：
	 * - 这些页面不能被交换出去
	 * - 不参与 LRU 管理
	 * - 不能使用普通的页面管理 API
	 *
	 * Page-ranges managed without "struct page", just pure PFN
	 */
	DECLARE_VMA_BIT(PFNMAP, 10),

	/**
	 * MAYBE_GUARD: 可能包含保护页
	 *
	 * 保护页（Guard Page）概念：
	 * - 不可访问的页面，放置在栈或其他区域的边界
	 * - 访问保护页会触发 SIGSEGV
	 * - 用于检测栈溢出或缓冲区溢出
	 *
	 * 设计原因：
	 * 这个标志标记可能包含保护页的区域，帮助内核正确处理页面错误。
	 */
	DECLARE_VMA_BIT(MAYBE_GUARD, 11),

	/**
	 * UFFD_WP: userfaultfd 写保护跟踪 (12)
	 *
	 * 设计原因：
	 * 允许用户空间程序跟踪哪些页面被写入，用于：
	 * - 实时虚拟机迁移（跟踪脏页）
	 * - 增量快照
	 * - 用户空间垃圾回收
	 *
	 * 工作原理：
	 * - 页面被标记为写保护
	 * - 写入时触发 userfaultfd 通知
	 * - 用户空间处理后，内核允许写入继续
	 *
	 * wrprotect pages tracking
	 */
	DECLARE_VMA_BIT(UFFD_WP, 12),

	/**
	 * LOCKED: 内存锁定，不允许交换 (13)
	 *
	 * 设计原因：
	 * 某些应用需要保证内存始终在物理内存中，不被交换出去：
	 * - 实时应用（避免页面错误延迟）
	 * - 加密密钥（防止被交换到磁盘）
	 * - 性能关键代码
	 *
	 * 对应系统调用：mlock(), mlockall()
	 *
	 * 注意：需要特权（CAP_IPC_LOCK）或在 rlimit 限制内
	 */
	DECLARE_VMA_BIT(LOCKED, 13),

	/**
	 * IO: 映射到 I/O 设备内存 (14)
	 *
	 * 设计原因：
	 * I/O 内存（如设备寄存器）需要特殊处理：
	 * - 不能缓存（必须直接访问硬件）
	 * - 读写可能有副作用（改变设备状态）
	 * - 不能被优化或重排
	 *
	 * Memory mapped I/O or similar
	 */
	DECLARE_VMA_BIT(IO, 14),

	/*
	 * 内存访问模式提示 - Memory Access Pattern Hints (15-16)
	 *
	 * 这些标志是应用程序通过 madvise() 提供的提示，帮助内核优化性能。
	 */

	/**
	 * SEQ_READ: 顺序读取访问模式 (15)
	 *
	 * 设计原因：
	 * 如果应用声明会顺序读取内存，内核可以：
	 * - 增加预读（readahead）大小
	 * - 提前丢弃已读过的页面（释放内存）
	 *
	 * 对应 madvise 标志：MADV_SEQUENTIAL
	 *
	 * App will access data sequentially
	 */
	DECLARE_VMA_BIT(SEQ_READ, 15),

	/**
	 * RAND_READ: 随机读取访问模式 (16)
	 *
	 * 设计原因：
	 * 如果应用声明会随机访问内存，内核可以：
	 * - 禁用或减少预读（预读无效）
	 * - 使用不同的页面回收策略
	 *
	 * 对应 madvise 标志：MADV_RANDOM
	 *
	 * App will not benefit from clustered reads
	 */
	DECLARE_VMA_BIT(RAND_READ, 16),

	/*
	 * fork 和 mremap 行为控制 - Fork and mremap Behavior Control (17-19)
	 */

	/**
	 * DONTCOPY: fork 时不复制此 VMA (17)
	 *
	 * 设计原因：
	 * 某些内存区域不应该被子进程继承：
	 * - 设备映射（子进程可能不应访问该设备）
	 * - 某些共享内存区域
	 * - 父进程特有的资源
	 *
	 * 对应 madvise 标志：MADV_DONTFORK
	 *
	 * Do not copy this vma on fork
	 */
	DECLARE_VMA_BIT(DONTCOPY, 17),

	/**
	 * DONTEXPAND: 不能通过 mremap 扩展 (18)
	 *
	 * 设计原因：
	 * 某些特殊映射（如设备映射）有固定大小，不能扩展。
	 * 设置此标志防止 mremap() 意外扩展这些区域。
	 *
	 * Cannot expand with mremap()
	 */
	DECLARE_VMA_BIT(DONTEXPAND, 18),

	/**
	 * LOCKONFAULT: 只在访问时锁定页面 (19)
	 *
	 * 设计原因：
	 * 传统的 mlock() 会立即锁定整个区域的所有页面。
	 * LOCKONFAULT 是一种"懒惰"版本：
	 * - 只标记区域为"应该锁定"
	 * - 页面在第一次访问时才被锁定到物理内存
	 * - 节省内存，因为未使用的页面不会被锁定
	 *
	 * 对应 mlock 标志：MLOCK_ONFAULT
	 *
	 * Lock pages covered when faulted in
	 */
	DECLARE_VMA_BIT(LOCKONFAULT, 19),
	/*
	 * 内存统计和会计标志 - Memory Accounting Flags (20-22)
	 */

	/**
	 * ACCOUNT: 这是一个需要统计的虚拟内存对象 (20)
	 *
	 * 设计原因：
	 * 内核需要跟踪每个进程使用了多少虚拟内存，用于：
	 * - 资源限制检查（RLIMIT_AS, RLIMIT_DATA）
	 * - /proc/[pid]/status 中的 VmSize 等统计
	 * - OOM killer 的决策依据
	 *
	 * 某些特殊映射（如内核对象映射）不需要计入用户空间限制。
	 *
	 * Is a VM accounted object
	 */
	DECLARE_VMA_BIT(ACCOUNT, 20),

	/**
	 * NORESERVE: 不预留交换空间 (21)
	 *
	 * 设计原因：
	 * 默认情况下，当创建私有可写映射时，内核会预留足够的交换空间，
	 * 保证即使所有页面都被写入（COW），也有空间可以交换出去。
	 *
	 * 设置 NORESERVE 后：
	 * - 不预留交换空间（过度承诺，overcommit）
	 * - 节省交换空间，但可能在后续写入时因空间不足而失败
	 * - 对应 mmap 标志：MAP_NORESERVE
	 *
	 * 使用场景：
	 * - 大量分配内存但实际只使用一小部分（稀疏访问）
	 * - 应用自己确保不会超过物理内存+交换空间
	 *
	 * should the VM suppress accounting
	 */
	DECLARE_VMA_BIT(NORESERVE, 21),

	/**
	 * HUGETLB: 大页（Huge Page）映射 (22)
	 *
	 * 大页概念：
	 * - 普通页面大小：4KB
	 * - 大页大小：2MB（x86）或 1GB（某些架构）
	 * - 优势：减少 TLB（Translation Lookaside Buffer）缺失，提高性能
	 *
	 * 设计原因：
	 * 对于大内存应用（数据库、科学计算等），使用大页可以：
	 * - 减少页表占用的内存
	 * - 减少 TLB 缺失（提高地址转换速度）
	 * - 提高内存访问性能（10-30% 对某些工作负载）
	 *
	 * 注意事项：
	 * - 大页不能被交换出去
	 * - 大页必须物理连续
	 * - 需要预先配置和预留
	 *
	 * 对应 mmap 标志：MAP_HUGETLB
	 *
	 * mmap legacy hugetlbfs
	 * Huge TLB Page VM
	 */
	DECLARE_VMA_BIT(HUGETLB, 22),

	/**
	 * SYNC: 同步页面错误 (23)
	 *
	 * 设计原因：
	 * 对于某些持久内存（DAX - Direct Access）设备：
	 * - 传统磁盘 I/O 是异步的（可以延迟写入）
	 * - DAX 设备的写入应该是同步的（立即持久化）
	 * - 这个标志确保页面错误处理是同步完成的
	 *
	 * 对应 mmap 标志：MAP_SYNC
	 *
	 * 使用场景：
	 * - 持久内存（NVDIMM）
	 * - 需要保证数据立即写入存储介质的应用
	 *
	 * Synchronous page faults
	 */
	DECLARE_VMA_BIT(SYNC, 23),

	/*
	 * 架构特定标志 - Architecture-specific Flags (24)
	 */

	/**
	 * ARCH_1: 架构特定标志 1 (24)
	 *
	 * 设计原因：
	 * 不同的 CPU 架构有不同的内存管理特性。
	 * 这个位可以被架构代码用于特定目的（见下面的别名定义）。
	 *
	 * Architecture-specific flag
	 */
	DECLARE_VMA_BIT(ARCH_1, 24),

	/*
	 * 特殊行为标志 - Special Behavior Flags (25-27)
	 */

	/**
	 * WIPEONFORK: fork 时清空 VMA 内容 (25)
	 *
	 * 设计原因：
	 * 某些敏感数据（如加密密钥、随机数生成器状态）不应该被子进程继承。
	 * 设置此标志后，fork 时：
	 * - VMA 结构被复制到子进程
	 * - 但所有页面被标记为清零（读取时返回全零）
	 * - 写入时分配新的零页面
	 *
	 * 对应 madvise 标志：MADV_WIPEONFORK
	 *
	 * 使用场景：
	 * - OpenSSL 的随机数生成器状态
	 * - 防止子进程读取父进程的密钥材料
	 *
	 * Wipe VMA contents in child.
	 */
	DECLARE_VMA_BIT(WIPEONFORK, 25),

	/**
	 * DONTDUMP: 不包含在 core dump 中 (26)
	 *
	 * 设计原因：
	 * Core dump 文件可能会：
	 * - 非常大（包含所有内存）
	 * - 包含敏感信息（密码、密钥）
	 * - 浪费磁盘空间（某些内存不重要）
	 *
	 * 设置此标志的区域不会被写入 core dump，可以：
	 * - 减小 core dump 大小
	 * - 保护敏感数据
	 *
	 * 对应 madvise 标志：MADV_DONTDUMP
	 *
	 * 使用场景：
	 * - 大型数据缓存（可以重新加载）
	 * - 敏感数据区域
	 * - 共享库映射（可以从文件恢复）
	 *
	 * Do not include in the core dump
	 */
	DECLARE_VMA_BIT(DONTDUMP, 26),

	/**
	 * SOFTDIRTY: 软脏位（表示页面不是"干净"的）(27)
	 *
	 * 软脏位概念：
	 * - 跟踪页面自上次检查点以来是否被修改
	 * - "软"是指由软件跟踪，而不是硬件（硬件有自己的脏位）
	 * - 用于用户空间的增量快照和检查点
	 *
	 * 设计原因：
	 * 允许应用程序（如 CRIU - Checkpoint/Restore In Userspace）：
	 * - 记录内存快照
	 * - 跟踪哪些页面被修改
	 * - 只保存修改过的页面（增量备份）
	 *
	 * 工作原理：
	 * - 清除软脏位后，所有后续的写入会重新设置这个位
	 * - 应用可以读取 /proc/[pid]/pagemap 检查哪些页面是脏的
	 *
	 * 注意：这个注释有点误导，"NOT soft dirty clean area" 表示
	 * 标志被设置时，区域是"脏的"（已修改）
	 *
	 * NOT soft dirty clean area
	 */
	DECLARE_VMA_BIT(SOFTDIRTY, 27),

	/**
	 * MIXEDMAP: 混合映射（包含 struct page 和纯 PFN 页面）(28)
	 *
	 * 设计原因：
	 * 某些映射包含两种类型的页面：
	 * 1. 正常页面（有 struct page）
	 * 2. 特殊页面（没有 struct page，只有 PFN）
	 *
	 * 例如：/dev/mem 映射可能同时包含系统 RAM 和设备内存。
	 *
	 * 这个标志告诉内核：
	 * - 不能假设所有页面都有 struct page
	 * - 需要逐页检查类型
	 *
	 * Can contain struct page and pure PFN pages
	 */
	DECLARE_VMA_BIT(MIXEDMAP, 28),

	/*
	 * 透明大页（THP）提示标志 - Transparent Huge Page Hints (29-30)
	 *
	 * 透明大页（THP）说明：
	 * - 内核自动将小页合并为大页（2MB）
	 * - "透明"是指应用无需修改代码
	 * - 提高性能但可能增加内存使用
	 */

	/**
	 * HUGEPAGE: 建议使用透明大页 (29)
	 *
	 * 设计原因：
	 * 应用可以提示内核这个区域适合使用大页：
	 * - 频繁访问的大内存区域
	 * - 性能关键的数据结构
	 *
	 * 内核会更积极地为这个区域分配和合并大页。
	 *
	 * 对应 madvise 标志：MADV_HUGEPAGE
	 *
	 * MADV_HUGEPAGE marked this vma
	 */
	DECLARE_VMA_BIT(HUGEPAGE, 29),

	/**
	 * NOHUGEPAGE: 不要使用透明大页 (30)
	 *
	 * 设计原因：
	 * 某些场景下大页反而有害：
	 * - 稀疏访问模式（大页浪费内存）
	 * - 短期存活的内存
	 * - 频繁 madvise(MADV_DONTNEED) 的区域
	 *
	 * 对应 madvise 标志：MADV_NOHUGEPAGE
	 *
	 * MADV_NOHUGEPAGE marked this vma
	 */
	DECLARE_VMA_BIT(NOHUGEPAGE, 30),

	/**
	 * MERGEABLE: KSM 可以合并相同页面 (31)
	 *
	 * KSM (Kernel Same-page Merging) 概念：
	 * - 扫描内存寻找内容相同的页面
	 * - 将相同页面合并为一个（写时复制）
	 * - 节省内存，特别是在虚拟化环境中
	 *
	 * 设计原因：
	 * KSM 扫描需要 CPU 资源，因此：
	 * - 默认不启用
	 * - 应用需要显式标记哪些区域可以合并
	 *
	 * 对应 madvise 标志：MADV_MERGEABLE
	 *
	 * 使用场景：
	 * - 虚拟机内存（多个虚拟机可能有相同的页面）
	 * - 相同程序的多个实例
	 * - 只读数据区域
	 *
	 * KSM may merge identical pages
	 */
	DECLARE_VMA_BIT(MERGEABLE, 31),
	/*
	 * 高位架构标志 - High Architecture Flags (32-38)
	 *
	 * 设计原因：
	 * 这些位被标记为"可复用"（These bits are reused），意味着
	 * 不同的架构可以将这些位用于不同的目的。
	 * 通过别名机制，每个架构可以给这些位赋予特定的语义。
	 */
	DECLARE_VMA_BIT(HIGH_ARCH_0, 32),
	DECLARE_VMA_BIT(HIGH_ARCH_1, 33),
	DECLARE_VMA_BIT(HIGH_ARCH_2, 34),
	DECLARE_VMA_BIT(HIGH_ARCH_3, 35),
	DECLARE_VMA_BIT(HIGH_ARCH_4, 36),
	DECLARE_VMA_BIT(HIGH_ARCH_5, 37),
	DECLARE_VMA_BIT(HIGH_ARCH_6, 38),

	/**
	 * ALLOW_ANY_UNCACHED: 允许任何非缓存内存类型 (39)
	 *
	 * VFIO（Virtual Function I/O）和 KVM 集成说明：
	 *
	 * 设计原因：
	 * 这个标志连接 VFIO 到架构特定的 KVM 代码。用于解决一个硬件安全问题。
	 *
	 * 问题背景：
	 * - 在某些平台上，某些 VFIO 设备被认为是"不安全的"
	 * - 如果 KVM（虚拟机）不锁定内存类型，可能导致机器崩溃
	 * - 例如：某些设备的 DMA 配合错误的内存缓存属性会出问题
	 *
	 * 解决方案：
	 * - VFIO 驱动在 mmap 设备内存时设置这个标志
	 * - 表示这块内存可以安全地使用任何非缓存类型
	 * - KVM 检查这个标志，决定是否允许虚拟机使用非缓存映射
	 *
	 * 使用场景：
	 * - PCI 设备直通（GPU, 网卡等）到虚拟机
	 * - 设备内存需要以非缓存方式访问（避免缓存一致性问题）
	 *
	 * This flag is used to connect VFIO to arch specific KVM code. It
	 * indicates that the memory under this VMA is safe for use with any
	 * non-cachable memory type inside KVM. Some VFIO devices, on some
	 * platforms, are thought to be unsafe and can cause machine crashes
	 * if KVM does not lock down the memory type.
	 */
	DECLARE_VMA_BIT(ALLOW_ANY_UNCACHED, 39),

	/*
	 * DROPPABLE 标志的架构特定定义 - Architecture-specific DROPPABLE Flag
	 *
	 * DROPPABLE 概念：
	 * - 标记可以在内存压力下被丢弃的页面（不需要写回）
	 * - 类似于"可以随时删除"的缓存
	 * - 丢弃后可以重新生成或重新加载
	 *
	 * 设计原因：
	 * 不同架构对可丢弃内存的支持不同，使用条件编译选择：
	 */
#if defined(CONFIG_PPC32)
	/*
	 * PowerPC 32 位：复用 ARCH_1 位
	 * 原因：32 位系统地址空间有限，VMA 标志位宝贵
	 */
	DECLARE_VMA_BIT_ALIAS(DROPPABLE, ARCH_1),
#elif defined(CONFIG_64BIT)
	/*
	 * 64 位系统：使用独立的位 40
	 * 原因：64 位系统有足够的标志位空间
	 */
	DECLARE_VMA_BIT(DROPPABLE, 40),
#endif

	/**
	 * UFFD_MINOR: userfaultfd 次要错误跟踪 (41)
	 *
	 * Minor Fault（次要错误）vs Major Fault（主要错误）：
	 * - Major Fault: 页面不在内存中，需要从磁盘读取（慢）
	 * - Minor Fault: 页面在内存中，但页表未设置（快）
	 *
	 * 设计原因：
	 * userfaultfd 可以拦截 minor fault，允许用户空间：
	 * - 在页表设置前插入自定义逻辑
	 * - 修改页面内容或属性
	 * - 实现自定义的页面管理策略
	 *
	 * 使用场景：
	 * - 实时虚拟机迁移中的页面预复制
	 * - 用户空间内存压缩
	 * - 自定义内存分层
	 */
	DECLARE_VMA_BIT(UFFD_MINOR, 41),

	/**
	 * SEALED: VMA 被密封，不能修改 (42)
	 *
	 * VMA Sealing（密封）概念：
	 * - 一旦密封，VMA 的属性和范围不能再改变
	 * - 防止恶意或意外的内存布局修改
	 * - 提高安全性
	 *
	 * 设计原因：
	 * 某些应用需要保证内存布局不被篡改：
	 * - JIT 编译器生成的代码
	 * - 安全敏感的数据结构
	 * - 沙箱环境中的内存隔离
	 *
	 * 密封后不允许的操作：
	 * - munmap（取消映射）
	 * - mremap（重新映射/调整大小）
	 * - mprotect（修改保护属性）
	 * - madvise 的某些操作
	 *
	 * 使用场景：
	 * - memfd_create() 配合 F_ADD_SEALS 使用
	 * - 共享内存的发送方密封内存，接收方只读访问
	 * - 防御性编程：防止库函数意外修改关键内存区域
	 *
	 * 某些安全敏感的应用需要锁定内存布局：
	 * - 防止攻击者通过 mprotect/mremap 改变内存属性
	 * - 防止意外的内存区域修改
	 * - 沙箱环境中限制内存操作
	 *
	 * 限制的操作：
	 * - 不能改变大小（mremap）
	 * - 不能改变权限（mprotect）
	 * - 不能取消映射（munmap）
	 * - 不能添加/移除内存映射
	 *
	 * 使用场景：
	 * - 浏览器沙箱
	 * - 容器隔离
	 * - 安全关键代码
	 */
	DECLARE_VMA_BIT(SEALED, 42),

	/*
	 * 复用高位架构标志的特定用途 - Specific Uses of High Architecture Flags
	 *
	 * 设计原因：
	 * 通过别名机制，不同的架构和功能可以复用相同的位，
	 * 但在不同的上下文中有不同的名称和含义。
	 *
	 * Flags that reuse flags above.
	 */

	/*
	 * 内存保护密钥（Memory Protection Keys）标志
	 *
	 * Protection Keys 概念：
	 * - Intel/AMD x86_64 的硬件功能
	 * - 允许为页面分配"密钥"（0-15）
	 * - 用户空间可以快速改变对不同密钥页面的访问权限
	 * - 不需要修改页表（比 mprotect 快得多）
	 *
	 * 设计原因：
	 * 传统的内存保护（mprotect）需要：
	 * - 修改页表（昂贵）
	 * - TLB 刷新（非常昂贵）
	 * - 系统调用开销
	 *
	 * Protection Keys 优势：
	 * - 用户空间直接修改 PKRU 寄存器（一条指令）
	 * - 无需系统调用
	 * - 无需修改页表
	 * - 无需 TLB 刷新
	 *
	 * 使用 5 个位来编码 16 种可能的密钥（2^4 = 16，但需要 5 位存储所有组合）
	 */
	DECLARE_VMA_BIT_ALIAS(PKEY_BIT0, HIGH_ARCH_0),  /* Protection Key 位 0 */
	DECLARE_VMA_BIT_ALIAS(PKEY_BIT1, HIGH_ARCH_1),  /* Protection Key 位 1 */
	DECLARE_VMA_BIT_ALIAS(PKEY_BIT2, HIGH_ARCH_2),  /* Protection Key 位 2 */
	DECLARE_VMA_BIT_ALIAS(PKEY_BIT3, HIGH_ARCH_3),  /* Protection Key 位 3 */
	DECLARE_VMA_BIT_ALIAS(PKEY_BIT4, HIGH_ARCH_4),  /* Protection Key 位 4 */

#if defined(CONFIG_X86_USER_SHADOW_STACK) || defined(CONFIG_RISCV_USER_CFI)
	/*
	 * 影子栈（Shadow Stack）支持 - x86 和 RISC-V
	 *
	 * Shadow Stack 概念：
	 * - 硬件支持的安全特性，防止返回地址被篡改（ROP 攻击）
	 * - 维护一个单独的栈，只存储返回地址
	 * - CPU 在函数调用/返回时自动检查两个栈是否匹配
	 * - 如果不匹配，触发异常（检测到攻击）
	 *
	 * CFI (Control Flow Integrity) - 控制流完整性：
	 * - RISC-V 的类似机制
	 * - 确保程序的控制流不被恶意修改
	 *
	 * 设计原因：
	 * ROP（Return-Oriented Programming）攻击：
	 * - 攻击者覆盖栈上的返回地址
	 * - 劫持控制流执行恶意代码
	 * - Shadow Stack 使这种攻击变得困难
	 *
	 * VM_SHADOW_STACK 不应与 VM_SHARED 同时设置：
	 * - Shadow stack 是每个线程私有的
	 * - 不支持共享影子栈（核心内存管理代码限制）
	 *
	 * 保护页设计：
	 * - 影子栈 VMA 会获得一个结束保护页（end guard page）
	 * - 帮助用户空间保护自己免受攻击
	 * - 对于 x86，一个页面就够了（见 arch/x86/kernel/shstk.c 中的注释）
	 *
	 * VM_SHADOW_STACK should not be set with VM_SHARED because of lack of
	 * support core mm.
	 *
	 * These VMAs will get a single end guard page. This helps userspace
	 * protect itself from attacks. A single page is enough for current
	 * shadow stack archs (x86). See the comments near alloc_shstk() in
	 * arch/x86/kernel/shstk.c for more details on the guard size.
	 */
	DECLARE_VMA_BIT_ALIAS(SHADOW_STACK, HIGH_ARCH_5),
#elif defined(CONFIG_ARM64_GCS)
	/*
	 * ARM64 的 Guarded Control Stack (GCS) - 类似影子栈
	 *
	 * GCS 概念：
	 * - ARM64 的控制流保护机制
	 * - 类似于 x86 的 Shadow Stack
	 * - 实现类似功能，但硬件实现细节不同
	 * - 也有类似的约束和保护需求
	 *
	 * 设计原因：
	 * 不同架构的控制流保护机制使用相同的位，但可能有不同的实现细节。
	 *
	 * arm64's Guarded Control Stack implements similar functionality and
	 * has similar constraints to shadow stacks.
	 */
	DECLARE_VMA_BIT_ALIAS(SHADOW_STACK, HIGH_ARCH_6),
#endif

	/*
	 * 其他架构特定的标志别名 - Other Architecture-specific Flag Aliases
	 *
	 * 这些是不同架构复用 ARCH_1 位的不同用途：
	 */

	/**
	 * SAO (Strong Access Ordering) - PowerPC 特定 (复用 ARCH_1)
	 *
	 * 设计原因：
	 * PowerPC 架构的内存排序特性。
	 * SAO 确保内存访问按照程序顺序执行，不被乱序优化。
	 *
	 * 使用场景：
	 * - 某些驱动需要严格的内存访问顺序
	 * - 多处理器同步
	 */
	DECLARE_VMA_BIT_ALIAS(SAO, ARCH_1),		/* Strong Access Ordering (powerpc) */

	/**
	 * GROWSUP - 段向上增长（PA-RISC 特定）
	 *
	 * 设计原因：
	 * 大多数架构的栈向下增长（从高地址到低地址）。
	 * 但 PA-RISC 架构的栈向上增长。
	 *
	 * 注意：这复用了 ARCH_1 位，与其他架构的用法不同。
	 */
	DECLARE_VMA_BIT_ALIAS(GROWSUP, ARCH_1),		/* parisc */

	/**
	 * SPARC_ADI - SPARC ADI (Application Data Integrity) 特性
	 *
	 * ADI 概念：
	 * - SPARC M7+ 处理器的硬件功能
	 * - 为内存地址添加"版本标签"
	 * - 访问时检查标签是否匹配
	 * - 检测缓冲区溢出和使用后释放（use-after-free）
	 *
	 * 设计原因：
	 * 硬件辅助的内存安全检测，类似于软件的 AddressSanitizer，但更快。
	 */
	DECLARE_VMA_BIT_ALIAS(SPARC_ADI, ARCH_1),	/* sparc64 */

	/**
	 * ARM64_BTI - ARM64 Branch Target Identification
	 *
	 * BTI 概念：
	 * - ARM64 的控制流完整性特性
	 * - 标记合法的跳转目标
	 * - 防止跳转到任意代码位置（JOP 攻击）
	 *
	 * 设计原因：
	 * 与影子栈配合，提供全面的控制流保护。
	 */
	DECLARE_VMA_BIT_ALIAS(ARM64_BTI, ARCH_1),	/* arm64 */

	/**
	 * ARCH_CLEAR - 需要清除的架构位（SPARC64 和 ARM64）
	 *
	 * 设计原因：
	 * 某些架构位在特定操作后需要被清除。
	 * 这个别名标识这些位。
	 */
	DECLARE_VMA_BIT_ALIAS(ARCH_CLEAR, ARCH_1),	/* sparc64, arm64 */

	/**
	 * MAPPED_COPY - nommu 系统的已映射副本标志
	 *
	 * 设计原因：
	 * 在无 MMU 系统中，标记这个 VMA 是否有对应的物理内存副本。
	 */
	DECLARE_VMA_BIT_ALIAS(MAPPED_COPY, ARCH_1),	/* !CONFIG_MMU */

	/**
	 * MTE - ARM64 Memory Tagging Extension
	 *
	 * MTE 概念：
	 * - ARM64 的硬件内存标签功能
	 * - 每个 16 字节内存块有一个 4 位标签
	 * - 指针中也包含标签
	 * - 访问时检查指针标签和内存标签是否匹配
	 * - 检测各种内存错误（溢出、use-after-free 等）
	 *
	 * 设计原因：
	 * 硬件级别的内存安全，比软件工具（ASan）开销小得多。
	 *
	 * 使用场景：
	 * - 生产环境的内存错误检测
	 * - 安全关键应用
	 */
	DECLARE_VMA_BIT_ALIAS(MTE, HIGH_ARCH_4),	/* arm64 */

	/**
	 * MTE_ALLOWED - 允许在此 VMA 中使用 MTE
	 *
	 * 设计原因：
	 * MTE 不是对所有内存都启用。应用需要显式启用。
	 * 这个标志标记哪些 VMA 可以使用 MTE。
	 */
	DECLARE_VMA_BIT_ALIAS(MTE_ALLOWED, HIGH_ARCH_5),/* arm64 */

	/*
	 * 栈标志的架构适配 - Stack Flag Adaptation
	 *
	 * 设计原因：
	 * 不同架构的栈增长方向不同，使用条件编译统一接口。
	 */
#ifdef CONFIG_STACK_GROWSUP
	/*
	 * 栈向上增长的架构（如 PA-RISC）
	 * - VM_STACK 映射到 GROWSUP 标志
	 * - VM_STACK_EARLY 映射到 GROWSDOWN（早期启动时使用）
	 */
	DECLARE_VMA_BIT_ALIAS(STACK, GROWSUP),
	DECLARE_VMA_BIT_ALIAS(STACK_EARLY, GROWSDOWN),
#else
	/*
	 * 栈向下增长的架构（大多数）
	 * - VM_STACK 映射到 GROWSDOWN 标志
	 */
	DECLARE_VMA_BIT_ALIAS(STACK, GROWSDOWN),
#endif
};
#undef DECLARE_VMA_BIT
#undef DECLARE_VMA_BIT_ALIAS


/*
 * VMA 标志位宏定义 - VMA Flag Macros
 *
 * 设计原因：
 * 前面定义的枚举只是位号（0, 1, 2, ...），实际使用时需要将它们转换为位掩码。
 * 这些宏将位号转换为可以直接使用的标志值。
 *
 * C 语言位运算知识：
 * - BIT(n): 将位号 n 转换为位掩码，即 1 << n
 *   例如：BIT(0) = 0x00000001 (二进制: ...00001)
 *        BIT(1) = 0x00000002 (二进制: ...00010)
 *        BIT(2) = 0x00000004 (二进制: ...00100)
 * - __force: sparse 工具注解，强制类型转换（忽略类型安全检查）
 */

/**
 * INIT_VM_FLAG - 将 VMA 位号转换为标志值
 * @name: 标志名称（如 READ, WRITE）
 *
 * 展开示例：
 * INIT_VM_FLAG(READ)
 * -> BIT((__force int) VMA_READ_BIT)
 * -> BIT(0)
 * -> 0x00000001
 */
#define INIT_VM_FLAG(name) BIT((__force int) VMA_ ## name ## _BIT)

/*
 * 基本访问权限标志 - Basic Access Permission Flags
 */
#define VM_READ		INIT_VM_FLAG(READ)      /* 0x00000001 - 可读 */
#define VM_WRITE	INIT_VM_FLAG(WRITE)     /* 0x00000002 - 可写 */
#define VM_EXEC		INIT_VM_FLAG(EXEC)      /* 0x00000004 - 可执行 */
#define VM_SHARED	INIT_VM_FLAG(SHARED)    /* 0x00000008 - 共享映射 */

/*
 * 权限上限标志 - Permission Limit Flags
 */
#define VM_MAYREAD	INIT_VM_FLAG(MAYREAD)   /* 0x00000010 - mprotect 可设为可读 */
#define VM_MAYWRITE	INIT_VM_FLAG(MAYWRITE)  /* 0x00000020 - mprotect 可设为可写 */
#define VM_MAYEXEC	INIT_VM_FLAG(MAYEXEC)   /* 0x00000040 - mprotect 可设为可执行 */
#define VM_MAYSHARE	INIT_VM_FLAG(MAYSHARE)  /* 0x00000080 - 可变为共享 */

/*
 * 段增长方向标志 - Growth Direction Flag
 */
#define VM_GROWSDOWN	INIT_VM_FLAG(GROWSDOWN) /* 0x00000100 - 向下增长（栈） */

#ifdef CONFIG_MMU
/*
 * userfaultfd 缺失页跟踪（有 MMU 系统）
 */
#define VM_UFFD_MISSING	INIT_VM_FLAG(UFFD_MISSING) /* 0x00000200 */
#else
/*
 * 无 MMU 系统：UFFD_MISSING 不可用，定义为 VM_NONE
 */
#define VM_UFFD_MISSING	VM_NONE
/*
 * 无 MMU 系统：可能覆盖文件映射
 */
#define VM_MAYOVERLAY	INIT_VM_FLAG(MAYOVERLAY)   /* 0x00000200 */
#endif

/*
 * 特殊映射类型标志 - Special Mapping Type Flags
 */
#define VM_PFNMAP	INIT_VM_FLAG(PFNMAP)       /* 0x00000400 - 页框号映射 */
#define VM_MAYBE_GUARD	INIT_VM_FLAG(MAYBE_GUARD)  /* 0x00000800 - 可能有保护页 */
#define VM_UFFD_WP	INIT_VM_FLAG(UFFD_WP)      /* 0x00001000 - userfaultfd 写保护 */
#define VM_LOCKED	INIT_VM_FLAG(LOCKED)       /* 0x00002000 - 锁定内存 */
#define VM_IO		INIT_VM_FLAG(IO)           /* 0x00004000 - I/O 设备内存 */

/*
 * 访问模式提示标志 - Access Pattern Hints
 */
#define VM_SEQ_READ	INIT_VM_FLAG(SEQ_READ)     /* 0x00008000 - 顺序读取 */
#define VM_RAND_READ	INIT_VM_FLAG(RAND_READ)    /* 0x00010000 - 随机读取 */

/*
 * fork 和 mremap 行为控制 - Fork and mremap Control
 */
#define VM_DONTCOPY	INIT_VM_FLAG(DONTCOPY)     /* 0x00020000 - fork 时不复制 */
#define VM_DONTEXPAND	INIT_VM_FLAG(DONTEXPAND)   /* 0x00040000 - 不能 mremap 扩展 */
#define VM_LOCKONFAULT	INIT_VM_FLAG(LOCKONFAULT)  /* 0x00080000 - 访问时锁定 */

/*
 * 内存统计标志 - Memory Accounting Flags
 */
#define VM_ACCOUNT	INIT_VM_FLAG(ACCOUNT)      /* 0x00100000 - 需要统计 */
#define VM_NORESERVE	INIT_VM_FLAG(NORESERVE)    /* 0x00200000 - 不预留交换空间 */
#define VM_HUGETLB	INIT_VM_FLAG(HUGETLB)      /* 0x00400000 - 大页映射 */
#define VM_SYNC		INIT_VM_FLAG(SYNC)         /* 0x00800000 - 同步页面错误 */
#define VM_ARCH_1	INIT_VM_FLAG(ARCH_1)       /* 0x01000000 - 架构特定标志 */
#define VM_WIPEONFORK	INIT_VM_FLAG(WIPEONFORK)   /* 0x02000000 - fork 时清空 */
#define VM_DONTDUMP	INIT_VM_FLAG(DONTDUMP)     /* 0x04000000 - 不进入 core dump */

#ifdef CONFIG_MEM_SOFT_DIRTY
/*
 * 软脏位支持（需要内核配置启用）
 */
#define VM_SOFTDIRTY	INIT_VM_FLAG(SOFTDIRTY)    /* 0x08000000 - 软脏位 */
#else
/*
 * 未启用软脏位支持时，定义为 VM_NONE
 */
#define VM_SOFTDIRTY	VM_NONE
#endif

/*
 * 页面类型和大页提示 - Page Type and Huge Page Hints
 */
#define VM_MIXEDMAP	INIT_VM_FLAG(MIXEDMAP)     /* 0x10000000 - 混合映射 */
#define VM_HUGEPAGE	INIT_VM_FLAG(HUGEPAGE)     /* 0x20000000 - 使用透明大页 */
#define VM_NOHUGEPAGE	INIT_VM_FLAG(NOHUGEPAGE)   /* 0x40000000 - 不用透明大页 */
#define VM_MERGEABLE	INIT_VM_FLAG(MERGEABLE)    /* 0x80000000 - KSM 可合并 */

/*
 * 栈标志定义 - Stack Flag Definition
 */
#define VM_STACK	INIT_VM_FLAG(STACK)        /* 栈区域标志 */

#ifdef CONFIG_STACK_GROWSUP
/*
 * 栈向上增长的架构（PA-RISC）
 * 需要一个额外的早期栈标志
 */
#define VM_STACK_EARLY	INIT_VM_FLAG(STACK_EARLY)
#else
/*
 * 栈向下增长的架构（大多数）
 * STACK_EARLY 定义为 VM_NONE（不需要）
 */
#define VM_STACK_EARLY	VM_NONE
#endif

/*
 * 架构保护密钥（Protection Keys）支持 - Architecture Protection Keys Support
 *
 * CONFIG_ARCH_HAS_PKEYS: 架构支持保护密钥特性
 */
#ifdef CONFIG_ARCH_HAS_PKEYS
/*
 * VM_PKEY_SHIFT: 保护密钥在 vm_flags 中的起始位位置
 *
 * 设计原因：
 * 保护密钥需要 4-5 位来编码（支持 16 或 32 个密钥）。
 * 这些位从 HIGH_ARCH_0 开始存储。
 *
 * VMA_HIGH_ARCH_0_BIT 是 HIGH_ARCH_0 标志的位号（32）。
 */
#define VM_PKEY_SHIFT ((__force int)VMA_HIGH_ARCH_0_BIT)

/*
 * 注意：尽管命名为 BIT，但这些实际是 FLAGS（标志值），不是位号
 * Despite the naming, these are FLAGS not bits.
 */
#define VM_PKEY_BIT0 INIT_VM_FLAG(PKEY_BIT0)  /* 保护密钥位 0 */
#define VM_PKEY_BIT1 INIT_VM_FLAG(PKEY_BIT1)  /* 保护密钥位 1 */
#define VM_PKEY_BIT2 INIT_VM_FLAG(PKEY_BIT2)  /* 保护密钥位 2 */

#if CONFIG_ARCH_PKEY_BITS > 3
/*
 * 如果架构支持超过 8 个保护密钥（需要第 4 位）
 */
#define VM_PKEY_BIT3 INIT_VM_FLAG(PKEY_BIT3)  /* 保护密钥位 3 */
#else
#define VM_PKEY_BIT3  VM_NONE
#endif /* CONFIG_ARCH_PKEY_BITS > 3 */

#if CONFIG_ARCH_PKEY_BITS > 4
/*
 * 如果架构支持超过 16 个保护密钥（需要第 5 位）
 */
#define VM_PKEY_BIT4 INIT_VM_FLAG(PKEY_BIT4)  /* 保护密钥位 4 */
#else
#define VM_PKEY_BIT4  VM_NONE
#endif /* CONFIG_ARCH_PKEY_BITS > 4 */
#endif /* CONFIG_ARCH_HAS_PKEYS */

/*
 * 影子栈和控制流完整性支持 - Shadow Stack and CFI Support
 */
#if defined(CONFIG_X86_USER_SHADOW_STACK) || defined(CONFIG_ARM64_GCS) || \
	defined(CONFIG_RISCV_USER_CFI)
/*
 * VM_SHADOW_STACK: 影子栈标志
 *
 * 使用场景：
 * - 标记用于影子栈的 VMA
 * - 内核会为这些 VMA 提供特殊的保护和处理
 */
#define VM_SHADOW_STACK	INIT_VM_FLAG(SHADOW_STACK)

/*
 * VMA_STARTGAP_FLAGS: 需要起始间隙的 VMA 标志
 *
 * 设计原因：
 * 某些 VMA 需要在起始位置有一个保护间隙（guard gap）：
 * - GROWSDOWN（栈）：防止栈向下溢出到其他区域
 * - SHADOW_STACK：防止影子栈被意外访问
 *
 * mk_vma_flags: 从多个位号创建标志位掩码的宏
 */
#define VMA_STARTGAP_FLAGS mk_vma_flags(VMA_GROWSDOWN_BIT, VMA_SHADOW_STACK_BIT)
#else
/*
 * 不支持影子栈的架构
 */
#define VM_SHADOW_STACK	VM_NONE
#define VMA_STARTGAP_FLAGS mk_vma_flags(VMA_GROWSDOWN_BIT)
#endif

/*
 * 架构特定的标志定义 - Architecture-specific Flag Definitions
 *
 * 设计原因：
 * 不同架构使用相同的位（ARCH_1 等）表示不同的特性。
 * 通过条件编译，每个架构只定义自己需要的标志。
 */

#if defined(CONFIG_PPC64)
/*
 * PowerPC 64 位：SAO (Strong Access Ordering)
 */
#define VM_SAO		INIT_VM_FLAG(SAO)

#elif defined(CONFIG_PARISC)
/*
 * PA-RISC：栈向上增长
 */
#define VM_GROWSUP	INIT_VM_FLAG(GROWSUP)

#elif defined(CONFIG_SPARC64)
/*
 * SPARC 64 位：ADI (Application Data Integrity)
 */
#define VM_SPARC_ADI	INIT_VM_FLAG(SPARC_ADI)
#define VM_ARCH_CLEAR	INIT_VM_FLAG(ARCH_CLEAR)

#elif defined(CONFIG_ARM64)
/*
 * ARM64：BTI (Branch Target Identification)
 */
#define VM_ARM64_BTI	INIT_VM_FLAG(ARM64_BTI)
#define VM_ARCH_CLEAR	INIT_VM_FLAG(ARCH_CLEAR)

#elif !defined(CONFIG_MMU)
/*
 * 无 MMU 系统：已映射副本标志
 */
#define VM_MAPPED_COPY	INIT_VM_FLAG(MAPPED_COPY)
#endif

#ifndef VM_GROWSUP
/*
 * 如果架构没有定义 VM_GROWSUP，则定义为 VM_NONE
 *
 * 设计原因：
 * 大多数架构的栈向下增长，不需要 GROWSUP 标志。
 * 定义为 VM_NONE 使代码可以统一使用这个宏而不需要条件编译。
 */
#define VM_GROWSUP	VM_NONE
#endif

/*
 * ARM64 内存标签扩展（MTE）支持 - ARM64 Memory Tagging Extension
 */
#ifdef CONFIG_ARM64_MTE
#define VM_MTE		INIT_VM_FLAG(MTE)           /* MTE 启用 */
#define VM_MTE_ALLOWED	INIT_VM_FLAG(MTE_ALLOWED)   /* 允许使用 MTE */
#else
#define VM_MTE		VM_NONE
#define VM_MTE_ALLOWED	VM_NONE
#endif

/*
 * userfaultfd minor fault 支持 - userfaultfd Minor Fault Support
 */
#ifdef CONFIG_HAVE_ARCH_USERFAULTFD_MINOR
#define VM_UFFD_MINOR	INIT_VM_FLAG(UFFD_MINOR)    /* Minor fault 跟踪 */
#else
#define VM_UFFD_MINOR	VM_NONE
#endif

/*
 * VMA 标志类型的 userfaultfd 掩码 - vma_flags_t masks for userfaultfd
 *
 * 设计原因：
 * VMA_UFFD_MINOR 受相同的配置门控（CONFIG_HAVE_ARCH_USERFAULTFD_MINOR），
 * 这意味着 64 位系统（位数足够），因此永远不会在无法容纳它的构建中
 * 将超出范围的位传递给 mk_vma_flags()。
 *
 * VMA_UFFD_MINOR is gated on the same config as VM_UFFD_MINOR -- which implies
 * 64BIT, where the bit fits -- so an out-of-range bit is never fed to
 * mk_vma_flags() on a build whose bitmap cannot hold it.
 */

/*
 * userfaultfd 标志的位掩码定义 - Bitmask Definitions for userfaultfd Flags
 *
 * C 语言知识：
 * mk_vma_flags() 是一个宏，用于从位号创建位掩码
 * 例如：mk_vma_flags(VMA_UFFD_MISSING_BIT) 将位号转换为可用的标志值
 */
#define VMA_UFFD_MISSING	mk_vma_flags(VMA_UFFD_MISSING_BIT)
#define VMA_UFFD_WP		mk_vma_flags(VMA_UFFD_WP_BIT)
#ifdef CONFIG_HAVE_ARCH_USERFAULTFD_MINOR
#define VMA_UFFD_MINOR		mk_vma_flags(VMA_UFFD_MINOR_BIT)
#else
#define VMA_UFFD_MINOR		EMPTY_VMA_FLAGS
#endif

/*
 * 64 位特定标志 - 64-bit Specific Flags
 *
 * 设计原因：
 * 这些功能需要额外的标志位，只在 64 位系统上可用（有足够的位）
 */
#ifdef CONFIG_64BIT
#define VM_ALLOW_ANY_UNCACHED	INIT_VM_FLAG(ALLOW_ANY_UNCACHED)  /* VFIO 非缓存内存 */
#define VM_SEALED		INIT_VM_FLAG(SEALED)              /* VMA 密封 */
#else
#define VM_ALLOW_ANY_UNCACHED	VM_NONE
#define VM_SEALED		VM_NONE
#endif

/*
 * 可丢弃内存标志 - Droppable Memory Flag
 *
 * 支持平台：64 位或 PowerPC 32 位
 */
#if defined(CONFIG_64BIT) || defined(CONFIG_PPC32)
#define VM_DROPPABLE		INIT_VM_FLAG(DROPPABLE)
#define VMA_DROPPABLE		mk_vma_flags(VMA_DROPPABLE_BIT)
#else
#define VM_DROPPABLE		VM_NONE
#define VMA_DROPPABLE		EMPTY_VMA_FLAGS
#endif

/*
 * 栈未完全设置时的标志 - Flags Set Until Stack is in Final Location
 *
 * 设计原因：
 * 在程序启动时，栈还没有完全建立。这些临时标志用于标记栈处于不完整状态。
 *
 * 包含的标志：
 * - VM_RAND_READ: 随机访问（临时）
 * - VM_SEQ_READ: 顺序访问（临时）
 * - VM_STACK_EARLY: 早期栈标志
 *
 * Bits set in the VMA until the stack is in its final location
 */
#define VM_STACK_INCOMPLETE_SETUP (VM_RAND_READ | VM_SEQ_READ | VM_STACK_EARLY)

/*
 * TASK_EXEC_BIT - 任务执行位的动态选择
 *
 * 设计原因：
 * 某些进程的 personality（个性）设置了 READ_IMPLIES_EXEC，
 * 意味着"可读"隐含"可执行"（为了兼容旧程序）。
 *
 * 根据当前进程的 personality：
 * - 如果设置了 READ_IMPLIES_EXEC: 使用 VMA_EXEC_BIT（可执行）
 * - 否则：使用 VMA_READ_BIT（只可读）
 *
 * C 语言知识：
 * current: 内核宏，指向当前正在运行的进程的 task_struct
 * ? : 是三元运算符（条件表达式）
 */
#define TASK_EXEC_BIT ((current->personality & READ_IMPLIES_EXEC) ? \
		       VMA_EXEC_BIT : VMA_READ_BIT)

/*
 * 常见数据区域标志组合 - Common Data Flag Combinations
 *
 * 设计原因：
 * 数据段（.data, .bss, 堆）有常见的权限组合。
 * 预定义这些组合简化代码，确保一致性。
 */

/**
 * VMA_DATA_FLAGS_TSK_EXEC - 数据区域标志（根据任务决定是否可执行）
 *
 * 包含的权限：
 * - VMA_READ_BIT: 可读
 * - VMA_WRITE_BIT: 可写
 * - TASK_EXEC_BIT: 根据进程 personality 决定
 * - VMA_MAYREAD_BIT: 可以变为可读
 * - VMA_MAYWRITE_BIT: 可以变为可写
 * - VMA_MAYEXEC_BIT: 可以变为可执行
 *
 * Common data flag combinations
 */
#define VMA_DATA_FLAGS_TSK_EXEC	mk_vma_flags(VMA_READ_BIT, VMA_WRITE_BIT, \
		TASK_EXEC_BIT, VMA_MAYREAD_BIT, VMA_MAYWRITE_BIT,	  \
		VMA_MAYEXEC_BIT)

/**
 * VMA_DATA_FLAGS_NON_EXEC - 数据区域标志（不可执行）
 *
 * 现代安全实践：数据段应该不可执行（NX/DEP 保护）
 */
#define VMA_DATA_FLAGS_NON_EXEC	mk_vma_flags(VMA_READ_BIT, VMA_WRITE_BIT, \
		VMA_MAYREAD_BIT, VMA_MAYWRITE_BIT, VMA_MAYEXEC_BIT)

/**
 * VMA_DATA_FLAGS_EXEC - 数据区域标志（可执行）
 *
 * 用于 JIT 编译器等需要执行数据段的场景
 */
#define VMA_DATA_FLAGS_EXEC	mk_vma_flags(VMA_READ_BIT, VMA_WRITE_BIT, \
		VMA_EXEC_BIT, VMA_MAYREAD_BIT, VMA_MAYWRITE_BIT,	  \
		VMA_MAYEXEC_BIT)

/*
 * 默认数据标志（架构可以覆盖）
 * arch can override this
 */
#ifndef VMA_DATA_DEFAULT_FLAGS
#define VMA_DATA_DEFAULT_FLAGS  VMA_DATA_FLAGS_EXEC
#endif

/*
 * 默认栈标志（架构可以覆盖）
 * arch can override this
 */
#ifndef VMA_STACK_DEFAULT_FLAGS
#define VMA_STACK_DEFAULT_FLAGS VMA_DATA_DEFAULT_FLAGS
#endif

/**
 * VMA_STACK_FLAGS - 栈区域的完整标志集
 *
 * 设计原因：
 * 栈需要默认权限 + 栈特定标志 + 内存统计
 *
 * append_vma_flags: 向基础标志追加额外的标志位
 */
#define VMA_STACK_FLAGS	append_vma_flags(VMA_STACK_DEFAULT_FLAGS,	\
		VMA_STACK_BIT, VMA_ACCOUNT_BIT)

/*
 * 临时定义：在 VMA 标志转换完成前使用
 * 将新式 VMA 标志转换回旧式标志（兼容性）
 *
 * Temporary until VMA flags conversion complete.
 */
#define VM_STACK_FLAGS vma_flags_to_legacy(VMA_STACK_FLAGS)

/*
 * 系统映射密封支持 - System Mapping Sealing Support
 *
 * 设计原因：
 * 如果配置启用了系统映射密封（CONFIG_MSEAL_SYSTEM_MAPPINGS），
 * 系统创建的映射（如 vDSO）会被自动密封，防止篡改。
 */
#ifdef CONFIG_MSEAL_SYSTEM_MAPPINGS
#define VM_SEALED_SYSMAP	VM_SEALED
#else
#define VM_SEALED_SYSMAP	VM_NONE
#endif

/*
 * VMA 基本访问权限标志 - VMA Basic Access Permission Flags
 *
 * 设计原因：
 * 提供快速检查或设置基本读/写/执行权限的掩码
 *
 * VMA basic access permission flags
 */
#define VM_ACCESS_FLAGS (VM_READ | VM_WRITE | VM_EXEC)
#define VMA_ACCESS_FLAGS mk_vma_flags(VMA_READ_BIT, VMA_WRITE_BIT, VMA_EXEC_BIT)


/*
 * 特殊 VMA 标志 - Special VMA Flags
 *
 * 设计原因：
 * 某些 VMA 具有特殊属性，不应该被合并或锁定。
 * 这些标志标识这类特殊的内存区域。
 *
 * 包含的标志：
 * - VMA_IO_BIT: I/O 设备内存
 * - VMA_DONTEXPAND_BIT: 不能扩展
 * - VMA_PFNMAP_BIT: 页框号映射
 * - VMA_MIXEDMAP_BIT: 混合映射（物理页和页框号）
 *
 * Special vmas that are non-mergable, non-mlock()able.
 */

#define VMA_SPECIAL_FLAGS mk_vma_flags(VMA_IO_BIT, VMA_DONTEXPAND_BIT, \
				       VMA_PFNMAP_BIT, VMA_MIXEDMAP_BIT)
#define VM_SPECIAL vma_flags_to_legacy(VMA_SPECIAL_FLAGS)

/*
 * 物理重映射页标志 - Physically Remapped Pages Flags
 *
 * 设计原因：
 * 当直接映射物理内存（如设备内存、DMA 缓冲区）到用户空间时，
 * 需要特殊处理，因为这些页面没有对应的 struct page。
 *
 * 标志含义：
 * - IO: 告诉其他代码不要访问这些页面（访问可能有副作用，如设备寄存器）
 * - PFNMAP: 告诉核心内存管理这些只是原始的页框号映射，没有对应的 struct page
 * - DONTEXPAND: 禁止 VMA 合并和通过 mremap() 扩展
 * - DONTDUMP: 即使关闭了 VM_IO，也从 core dump 中排除这个 VMA
 *
 * 使用场景：
 * - 设备驱动映射设备内存到用户空间（如显卡显存、DMA 缓冲区）
 * - 直接访问物理内存的特权程序
 *
 * Physically remapped pages are special. Tell the
 * rest of the world about it:
 *   IO tells people not to look at these pages
 *	(accesses can have side effects).
 *   PFNMAP tells the core MM that the base pages are just
 *	raw PFN mappings, and do not have a "struct page" associated
 *	with them.
 *   DONTEXPAND
 *      Disable vma merging and expanding with mremap().
 *   DONTDUMP
 *      Omit vma from core dump, even when VM_IO turned off.
 */
#define VMA_REMAP_FLAGS mk_vma_flags(VMA_IO_BIT, VMA_PFNMAP_BIT,	\
				     VMA_DONTEXPAND_BIT, VMA_DONTDUMP_BIT)

/*
 * VM_NO_KHUGEPAGED - 防止 khugepaged 扫描的掩码
 *
 * 设计原因：
 * khugepaged 是内核后台线程，尝试将小页合并为透明大页（THP）。
 * 但某些 VMA 不应该被扫描：
 * - VM_SPECIAL: 特殊映射（设备内存等）
 * - VM_HUGETLB: 已经是大页
 *
 * This mask prevents VMA from being scanned with khugepaged
 */
#define VM_NO_KHUGEPAGED (VM_SPECIAL | VM_HUGETLB)

/*
 * VM_INIT_DEF_MASK - 进程可以从父进程继承的 mm->def_flags
 *
 * 设计原因：
 * 某些内存标志应该在 fork 时从父进程继承给子进程。
 * 目前只有 VM_NOHUGEPAGE（不使用透明大页）会被继承。
 *
 * This mask defines which mm->def_flags a process can inherit its parent
 */
#define VM_INIT_DEF_MASK	VM_NOHUGEPAGE

/*
 * VM_LOCKED_MASK - mlock 使用的所有 VMA 标志位
 *
 * 设计原因：
 * mlock 相关的标志有两个：
 * - VM_LOCKED: 已经锁定（所有页都在内存中）
 * - VM_LOCKONFAULT: 延迟锁定（页面在首次访问时才锁定）
 *
 * This mask represents all the VMA flag bits used by mlock
 */
#define VM_LOCKED_MASK	(VM_LOCKED | VM_LOCKONFAULT)

#define VMA_LOCKED_MASK	mk_vma_flags(VMA_LOCKED_BIT, VMA_LOCKONFAULT_BIT)

/*
 * VM_ATOMIC_SET_ALLOWED - 可以通过 VMA/mmap 读锁原子更新的标志
 *
 * 设计原因：
 * 通常修改 VMA 标志需要写锁。但某些标志（如 VM_MAYBE_GUARD）
 * 允许在只持有读锁的情况下原子更新，提高性能。
 *
 * These flags can be updated atomically via VMA/mmap read lock.
 */
#define VM_ATOMIC_SET_ALLOWED VM_MAYBE_GUARD

/*
 * VM_ARCH_CLEAR - 更新保护时需要清除的架构特定标志
 *
 * 设计原因：
 * 当通过 mprotect() 等系统调用修改内存保护属性时，
 * 某些架构特定的标志可能需要被清除（如 SPARC ADI、ARM64 BTI）。
 *
 * Arch-specific flags to clear when updating VM flags on protection change
 */
#ifndef VM_ARCH_CLEAR
#define VM_ARCH_CLEAR	VM_NONE
#endif

/*
 * VM_FLAGS_CLEAR - 保护改变时需要清除的所有标志
 *
 * 包括：
 * - ARCH_VM_PKEY_FLAGS: 架构保护密钥标志
 * - VM_ARCH_CLEAR: 架构特定标志
 */
#define VM_FLAGS_CLEAR	(ARCH_VM_PKEY_FLAGS | VM_ARCH_CLEAR)

/*
 * Flags which should be 'sticky' on merge - that is, flags which, when one VMA
 * possesses it but the other does not, the merged VMA should nonetheless have
 * applied to it:
 *
 *   VMA_SOFTDIRTY_BIT - if a VMA is marked soft-dirty, that is has not had its
 *                       references cleared via /proc/$pid/clear_refs, any
 *                       merged VMA should be considered soft-dirty also as it
 *                       operates at a VMA granularity.
 *
 * VMA_MAYBE_GUARD_BIT - If a VMA may have guard regions in place it implies
 *                       that mapped page tables may contain metadata not
 *                       described by the VMA and thus any merged VMA may also
 *                       contain this metadata, and thus we must make this flag
 *                       sticky.
/*
 * VMA_STICKY_FLAGS - 粘性标志（在合并时会被保留）
 *
 * 设计原因：
 * "粘性"标志是指在 VMA 合并时，如果两个 VMA 中任何一个有这个标志，
 * 合并后的 VMA 就会有这个标志（类似"粘"上去了）。
 *
 * 包含的粘性标志：
 * - VMA_SOFTDIRTY_BIT: 软脏位（如果启用）
 * - VMA_MAYBE_GUARD_BIT: 可能有保护页
 *
 * 为什么这些是粘性的：
 * - SOFTDIRTY: 用于跟踪内存变化，不能因为合并而丢失
 * - MAYBE_GUARD: 保护页信息必须保留以确保安全
 */
#ifdef CONFIG_MEM_SOFT_DIRTY
#define VMA_STICKY_FLAGS mk_vma_flags(VMA_SOFTDIRTY_BIT, VMA_MAYBE_GUARD_BIT)
#else
#define VMA_STICKY_FLAGS mk_vma_flags(VMA_MAYBE_GUARD_BIT)
#endif

/*
 * VMA_IGNORE_MERGE_FLAGS - 合并时忽略的 VMA 标志
 *
 * 设计原因：
 * 通常，两个 VMA 只有在标志完全匹配时才能合并。
 * 但"粘性"标志是例外：即使一个 VMA 有、另一个没有，也可以合并。
 * 合并时，简单地将所有粘性标志都设置到合并后的 VMA 上。
 *
 * VMA flags we ignore for the purposes of merge, i.e. one VMA possessing one
 * of these flags and the other not does not preclude a merge.
 *
 *    VMA_STICKY_FLAGS - When merging VMAs, VMA flags must match, unless they
 *                       are 'sticky'. If any sticky flags exist in either VMA,
 *                       we simply set all of them on the merged VMA.
 */
#define VMA_IGNORE_MERGE_FLAGS VMA_STICKY_FLAGS

/*
 * VM_COPY_ON_FORK - fork 时需要复制页表的标志
 *
 * 设计原因：
 * 通常 fork 时采用 COW（写时复制），子进程和父进程共享页表，
 * 只在写入时才复制。但某些 VMA 映射的内容无法通过页面错误重建，
 * 必须在 fork 时就复制页表。
 *
 * 注意：应该与目标 VMA 比较，而不是源 VMA，因为 VM_UFFD_WP 可能不会
 * 传播到目标，而所有其他标志都会传播。
 *
 * 需要复制的标志：
 *
 * VM_PFNMAP / VM_MIXEDMAP - 包含无法在页面错误时合理重建的内核映射数据
 *
 * VM_UFFD_WP - 编码已安装的 userfaultfd 写保护处理器的元数据，
 *              无法在页面错误时重建。即使是文件映射（如 shmem），
 *              只要启用了 uffd-wp，我们总是复制页表。因为启用 uffd-wp 时，
 *              页表包含 uffd-wp 保护信息，这些信息无法从页缓存检索，
 *              跳过复制会丢失这些信息。
 *
 * VM_MAYBE_GUARD - 可能包含页面保护区域标记，这些标记按设计只是
 *                  页表的属性，无法在页面错误时重建。
 *
 * Flags which should result in page tables being copied on fork. These are
 * flags which indicate that the VMA maps page tables which cannot be
 * reconsistuted upon page fault, so necessitate page table copying upon fork.
 *
 * Note that these flags should be compared with the DESTINATION VMA not the
 * source, as VM_UFFD_WP may not be propagated to destination, while all other
 * flags will be.
 *
 * VM_PFNMAP / VM_MIXEDMAP - These contain kernel-mapped data which cannot be
 *                           reasonably reconstructed on page fault.
 *
 *              VM_UFFD_WP - Encodes metadata about an installed uffd
 *                           write protect handler, which cannot be
 *                           reconstructed on page fault.
 *
 *                           We always copy pgtables when dst_vma has uffd-wp
 *                           enabled even if it's file-backed
 *                           (e.g. shmem). Because when uffd-wp is enabled,
 *                           pgtable contains uffd-wp protection information,
 *                           that's something we can't retrieve from page cache,
 *                           and skip copying will lose those info.
 *
 *          VM_MAYBE_GUARD - Could contain page guard region markers which
 *                           by design are a property of the page tables
 *                           only and thus cannot be reconstructed on page
 *                           fault.
 */
#define VM_COPY_ON_FORK (VM_PFNMAP | VM_MIXEDMAP | VM_UFFD_WP | VM_MAYBE_GUARD)

/*
 * 页保护映射 - Page Protection Mapping
 *
 * 设计原因：
 * 从 vm_flags 的低 4 位（读/写/执行权限位）映射到页保护掩码。
 * 这个映射由架构提供（protection_map[] 数组）。
 *
 * mapping from the currently active vm_flags protection bits (the
 * low four bits) to a page protection mask..
 */

/*
 * FAULT_FLAG_DEFAULT - 默认的页面错误标志
 *
 * 设计原因：
 * 大多数架构特定的页面错误处理器应该使用的默认标志。
 *
 * 包含的标志：
 * - FAULT_FLAG_ALLOW_RETRY: 允许重试（可以释放 mmap_lock 后重试）
 * - FAULT_FLAG_KILLABLE: 可以被信号杀死
 * - FAULT_FLAG_INTERRUPTIBLE: 可以被中断
 *
 * 为什么这些是默认的：
 * - 允许重试提高并发性能
 * - 可杀死/可中断防止进程永久挂起
 *
 * The default fault flags that should be used by most of the
 * arch-specific page fault handlers.
 */
#define FAULT_FLAG_DEFAULT  (FAULT_FLAG_ALLOW_RETRY | \
			     FAULT_FLAG_KILLABLE | \
			     FAULT_FLAG_INTERRUPTIBLE)

/**
 * fault_flag_allow_retry_first - 检查是否是首次尝试且允许重试
 * @flags: 页面错误标志
 *
 * 设计原因：
 * 这主要用于我们想要避免长时间持有 mmap_lock 的地方。
 * 当等待另一个条件改变时，我们可以礼貌地在第一轮就释放 mmap_lock，
 * 避免其他也需要 mmap_lock 的进程可能的饥饿。
 *
 * C 语言知识：
 * - static inline: 内联函数，编译器会将函数体直接插入调用处（提高性能）
 * - bool: 布尔类型（true 或 false）
 * - enum fault_flag: 页面错误标志的枚举类型
 * - &: 按位与运算符
 * - !: 逻辑非运算符
 *
 * 返回值：
 * - true: 页面错误允许重试且这是第一次尝试
 * - false: 否则
 *
 * This is mostly used for places where we want to try to avoid taking
 * the mmap_lock for too long a time when waiting for another condition
 * to change, in which case we can try to be polite to release the
 * mmap_lock in the first round to avoid potential starvation of other
 * processes that would also want the mmap_lock.
 *
 * Return: true if the page fault allows retry and this is the first
 * attempt of the fault handling; false otherwise.
 */
static inline bool fault_flag_allow_retry_first(enum fault_flag flags)
{
	return (flags & FAULT_FLAG_ALLOW_RETRY) &&
	    (!(flags & FAULT_FLAG_TRIED));
}

/*
 * FAULT_FLAG_TRACE - 用于跟踪页面错误的标志映射
 *
 * 设计原因：
 * 这个宏用于内核的 tracepoint 机制，将每个错误标志映射到对应的字符串名称，
 * 用于调试和性能分析工具（如 perf、ftrace）。
 *
 * C 语言知识：
 * - 这是一个宏定义，展开后是一系列逗号分隔的初始化器
 * - 每一行格式为 { 标志值, "标志名称" }
 * - \ 表示宏定义跨多行
 *
 * 标志含义：
 * - WRITE: 写入操作触发的错误
 * - MKWRITE: 从只读变为可写（make writable）
 * - ALLOW_RETRY: 允许重试
 * - RETRY_NOWAIT: 重试但不等待
 * - KILLABLE: 可被信号杀死
 * - TRIED: 已经尝试过一次
 * - USER: 用户空间触发
 * - REMOTE: 远程访问（如通过网络）
 * - INSTRUCTION: 指令获取错误（执行权限问题）
 * - INTERRUPTIBLE: 可中断
 * - VMA_LOCK: 使用 VMA 锁
 */
#define FAULT_FLAG_TRACE \
	{ FAULT_FLAG_WRITE,		"WRITE" }, \
	{ FAULT_FLAG_MKWRITE,		"MKWRITE" }, \
	{ FAULT_FLAG_ALLOW_RETRY,	"ALLOW_RETRY" }, \
	{ FAULT_FLAG_RETRY_NOWAIT,	"RETRY_NOWAIT" }, \
	{ FAULT_FLAG_KILLABLE,		"KILLABLE" }, \
	{ FAULT_FLAG_TRIED,		"TRIED" }, \
	{ FAULT_FLAG_USER,		"USER" }, \
	{ FAULT_FLAG_REMOTE,		"REMOTE" }, \
	{ FAULT_FLAG_INSTRUCTION,	"INSTRUCTION" }, \
	{ FAULT_FLAG_INTERRUPTIBLE,	"INTERRUPTIBLE" }, \
	{ FAULT_FLAG_VMA_LOCK,		"VMA_LOCK" }

/*
 * struct vm_fault - 页面错误信息结构
 *
 * 设计原因：
 * 当用户空间程序访问一个虚拟地址但对应的物理页不存在（或权限不对）时，
 * CPU 会触发"页面错误"（page fault）。内核需要处理这个错误，可能的操作包括：
 * 1. 从磁盘加载页面（文件映射）
 * 2. 分配新的物理页（匿名内存）
 * 3. 执行 COW（写时复制）
 * 4. 返回错误（非法访问）
 *
 * 这个结构体包含了处理页面错误所需的所有上下文信息。
 *
 * C 语言知识：
 * - struct: 结构体，包含多个不同类型的字段
 * - const struct { ... }: 匿名结构体，其成员在错误处理期间是只读的
 * - union: 联合体，所有成员共享同一块内存（只能同时使用其中一个）
 * - ->: 结构体指针访问成员的运算符
 *
 * 注意事项：
 * 第一个匿名结构体中的字段是常量（只读），由页面错误处理器填充。
 * 错误处理函数不应修改这些字段。
 *
 * vm_fault is filled by the pagefault handler and passed to the vma's
 * ->fault function. The vma's ->fault is responsible for returning a bitmask
 * of VM_FAULT_xxx flags that give details about how the fault was handled.
 *
 * MM layer fills up gfp_mask for page allocations but fault handler might
 * alter it if its implementation requires a different allocation context.
 *
 * pgoff should be used in favour of virtual_address, if possible.
 */
struct vm_fault {
	const struct {
		struct vm_area_struct *vma;	/* 目标 VMA - Target VMA */
		gfp_t gfp_mask;			/* 页面分配的 GFP 标志 - gfp mask to be used for allocations */
		pgoff_t pgoff;			/* 基于 VMA 的逻辑页偏移 - Logical page offset based on vma */
		unsigned long address;		/* 触发错误的虚拟地址（已掩码对齐） - Faulting virtual address - masked */
		unsigned long real_address;	/* 触发错误的虚拟地址（未掩码，原始） - Faulting virtual address - unmasked */
	};
	enum fault_flag flags;		/* FAULT_FLAG_xxx 标志 - FAULT_FLAG_xxx flags
					 * XXX: 实际上应该是 'const'，但历史原因没有 - should really be 'const' */
	pmd_t *pmd;			/* 指向匹配该地址的 PMD（页中间目录）表项的指针 - Pointer to pmd entry matching
					 * the 'address' */
	pud_t *pud;			/* 指向匹配该地址的 PUD（页上层目录）表项的指针 - Pointer to pud entry matching
					 * the 'address'
					 */
	union {
		pte_t orig_pte;		/* 错误发生时 PTE（页表项）的值 - Value of PTE at the time of fault */
		pmd_t orig_pmd;		/* 错误发生时 PMD 的值（用于透明大页） - Value of PMD at the time of fault,
					 * used by PMD fault only.
					 */
	};

	struct page *cow_page;		/* 页面处理器可用于 COW（写时复制）错误 - Page handler may use for COW fault */
	struct page *page;		/* ->fault 处理器应该在这里返回一个页面 - ->fault handlers should return a
					 * page here, unless VM_FAULT_NOPAGE
					 * 除非设置了 VM_FAULT_NOPAGE（VM_FAULT_ERROR 也隐含此设置） - is set (which is also implied by
					 * VM_FAULT_ERROR).
					 */
	/* 以下三个条目仅在持有 ptl 锁时有效 - These three entries are valid only while holding ptl lock */
	pte_t *pte;			/* 指向匹配该地址的 PTE 表项的指针 - Pointer to pte entry matching
					 * the 'address'. NULL if the page
					 * 如果页表尚未分配则为 NULL - table hasn't been allocated.
					 */
	spinlock_t *ptl;		/* 页表锁 - Page table lock.
					 * 如果 'pte' 不为 NULL，保护 pte 页表，否则保护 pmd - Protects pte page table if 'pte'
					 * is not NULL, otherwise pmd.
					 */
	pgtable_t prealloc_pte;		/* 预分配的 pte 页表 - Pre-allocated pte page table.
					 * vm_ops->map_pages() 从原子上下文设置页表 - vm_ops->map_pages() sets up a page
					 * table from atomic context.
					 * do_fault_around() 预分配页表以避免从原子上下文分配 - do_fault_around() pre-allocates
					 * page table to avoid allocation from
					 * atomic context.
					 */
};

/*
 * struct vm_uffd_ops - userfaultfd 操作（前向声明）
 *
 * 设计原因：
 * userfaultfd 是一种用户空间页面错误处理机制。
 * 这里只是前向声明，具体定义在其他地方。
 */
struct vm_uffd_ops;

/*
 * struct vm_operations_struct - VMA 操作函数表
 *
 * 设计原因：
 * 不同类型的内存映射需要不同的处理逻辑。例如：
 * - 匿名内存：分配新页面
 * - 文件映射：从磁盘读取文件内容
 * - 设备映射：映射设备内存
 *
 * 这个结构体是一个"虚函数表"（面向对象编程中的概念），
 * 定义了一组回调函数指针，不同的映射类型可以提供不同的实现。
 *
 * C 语言知识：
 * - 函数指针：void (*open)(...) 表示指向函数的指针
 * - 回调机制：内核在特定事件发生时调用这些函数
 *
 * 注意事项：
 * 所有这些回调函数都可能在用户上下文中调用，可能睡眠，
 * 调用者持有 mmap_lock。
 *
 * These are the virtual MM functions - opening of an area, closing and
 * unmapping it (needed to keep files on disk up-to-date etc), pointer
 * to the functions called when a no-page or a wp-page exception occurs.
 */
struct vm_operations_struct {
	/**
	 * @open: VMA 被重新映射、分割或 fork 时调用，首次映射 VMA 时不调用
	 *
	 * 设计原因：
	 * 当 VMA 需要被复制或修改时，某些资源可能需要额外的引用计数或初始化。
	 *
	 * 上下文：用户上下文，可能睡眠，调用者持有 mmap_lock
	 *
	 * @open: Called when a VMA is remapped, split or forked. Not called
	 * upon first mapping a VMA.
	 * Context: User context.  May sleep.  Caller holds mmap_lock.
	 */
	void (*open)(struct vm_area_struct *vma);
	/**
	 * @close: 当 VMA 从 MM 中移除时调用
	 *
	 * 设计原因：
	 * 释放与 VMA 关联的资源，如关闭文件描述符、释放引用等。
	 *
	 * 上下文：用户上下文，可能睡眠，调用者持有 mmap_lock
	 *
	 * @close: Called when the VMA is being removed from the MM.
	 * Context: User context.  May sleep.  Caller holds mmap_lock.
	 */
	void (*close)(struct vm_area_struct *vma);
	/**
	 * @mapped: 当 VMA 首次映射到 MM 时调用，如果新 VMA 与相邻 VMA 合并则不调用
	 *
	 * 设计原因：
	 * 某些驱动程序或文件系统需要知道何时首次建立映射，以便进行初始化。
	 *
	 * 输入参数：
	 * @vma: 被映射的 VMA
	 * @vm_private_data: 输出参数，允许修改 vma->vm_private_data
	 *
	 * @mapped: Called when the VMA is first mapped in the MM. Not called if
	 * the new VMA is merged with an adjacent VMA.
	 *
	 * The @vm_private_data field is an output field allowing the user to
	 * modify vma->vm_private_data as necessary.
	 *
	 * 注意事项：
	 * 仅当从 f_op->mmap_prepare 设置时有效。如果从 f_op->mmap 设置会导致错误。
	 *
	 * 返回值：
	 * 成功返回 0，否则返回错误码。出错时 VMA 将被取消映射。
	 *
	 * 上下文：用户上下文，可能睡眠，调用者持有 mmap_lock
	 *
	 * ONLY valid if set from f_op->mmap_prepare. Will result in an error if
	 * set from f_op->mmap.
	 *
	 * Returns %0 on success, or an error otherwise. On error, the VMA will
	 * be unmapped.
	 *
	 * Context: User context.  May sleep.  Caller holds mmap_lock.
	 */
	int (*mapped)(unsigned long start, unsigned long end, pgoff_t pgoff,
		      const struct file *file, void **vm_private_data);
	/**
	 * @may_split: 在分割 VMA 之前检查是否允许分割
	 *
	 * 设计原因：
	 * 某些特殊的 VMA（如设备驱动映射）可能不允许分割，需要提前检查。
	 *
	 * 输入参数：
	 * @vma: 要分割的 VMA
	 * @addr: 分割点的地址
	 *
	 * 返回值：
	 * 0 表示允许分割，非 0 表示不允许
	 *
	 * Called any time before splitting to check if it's allowed
	 */
	int (*may_split)(struct vm_area_struct *vma, unsigned long addr);
	/**
	 * @mremap: VMA 被 mremap() 系统调用重新映射时调用
	 *
	 * 设计原因：
	 * mremap() 可以移动或调整内存映射的大小，某些驱动需要在此时更新内部状态。
	 *
	 * 输入参数：
	 * @vma: 被重新映射的 VMA
	 *
	 * 返回值：
	 * 0 表示成功，非 0 表示失败
	 */
	int (*mremap)(struct vm_area_struct *vma);
	/**
	 * @mprotect: mprotect() 调用时进行驱动特定的权限检查
	 *
	 * 设计原因：
	 * 在 mprotect() 最终确定之前，允许驱动程序检查权限变更是否合法。
	 * VMA 不能被修改。
	 *
	 * 输入参数：
	 * @vma: 要修改权限的 VMA
	 * @start: 起始地址
	 * @end: 结束地址
	 * @newflags: 新的保护标志
	 *
	 * 返回值：
	 * 0 表示 mprotect() 可以继续，非 0 表示拒绝
	 *
	 * Called by mprotect() to make driver-specific permission
	 * checks before mprotect() is finalised.   The VMA must not
	 * be modified.  Returns 0 if mprotect() can proceed.
	 */
	int (*mprotect)(struct vm_area_struct *vma, unsigned long start,
			unsigned long end, unsigned long newflags);
	/**
	 * @fault: 页面错误处理函数
	 *
	 * 设计原因：
	 * 这是最核心的回调函数。当访问 VMA 中的地址触发页面错误时，
	 * 内核会调用这个函数来处理错误（分配页面、从磁盘加载等）。
	 *
	 * 输入参数：
	 * @vmf: 包含错误详细信息的结构体
	 *
	 * 返回值：
	 * vm_fault_t 类型，表示错误处理结果（成功、需要重试、错误等）
	 */
	vm_fault_t (*fault)(struct vm_fault *vmf);
	/**
	 * @huge_fault: 大页（huge page）的页面错误处理
	 *
	 * 设计原因：
	 * 透明大页（THP）或明确的大页映射需要特殊处理。
	 *
	 * 输入参数：
	 * @vmf: 错误信息
	 * @order: 页面大小的阶数（2^order 个普通页）
	 *
	 * 返回值：
	 * vm_fault_t 类型，错误处理结果
	 */
	vm_fault_t (*huge_fault)(struct vm_fault *vmf, unsigned int order);
	/**
	 * @map_pages: 批量映射页面（预映射优化）
	 *
	 * 设计原因：
	 * 发生页面错误时，除了处理当前页，还可以预先映射附近的页面，
	 * 减少后续的页面错误次数（预读优化）。
	 *
	 * 输入参数：
	 * @vmf: 错误信息
	 * @start_pgoff: 起始页偏移
	 * @end_pgoff: 结束页偏移
	 *
	 * 返回值：
	 * vm_fault_t 类型
	 */
	vm_fault_t (*map_pages)(struct vm_fault *vmf,
			pgoff_t start_pgoff, pgoff_t end_pgoff);
	/**
	 * @pagesize: 获取此 VMA 使用的页面大小
	 *
	 * 设计原因：
	 * 某些 VMA 可能使用非标准页面大小（如大页）。
	 *
	 * 输入参数：
	 * @vma: 目标 VMA
	 *
	 * 返回值：
	 * 页面大小（字节）
	 */
	unsigned long (*pagesize)(struct vm_area_struct *vma);

	/**
	 * @page_mkwrite: 之前只读的页面即将变为可写时的通知
	 *
	 * 设计原因：
	 * 用于 COW（写时复制）机制。当进程第一次写入共享的只读页面时，
	 * 需要复制页面。此回调允许文件系统或驱动在页面变为可写前执行操作。
	 *
	 * 输入参数：
	 * @vmf: 错误信息
	 *
	 * 返回值：
	 * vm_fault_t 类型，如果返回错误会导致 SIGBUS 信号
	 *
	 * notification that a previously read-only page is about to become
	 * writable, if an error is returned it will cause a SIGBUS
	 */
	vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);

	/**
	 * @pfn_mkwrite: 与 page_mkwrite 相同，但用于 VM_PFNMAP|VM_MIXEDMAP
	 *
	 * 设计原因：
	 * VM_PFNMAP 和 VM_MIXEDMAP 映射的是物理帧号（PFN）而不是 struct page，
	 * 需要单独的回调。
	 *
	 * 输入参数：
	 * @vmf: 错误信息
	 *
	 * 返回值：
	 * vm_fault_t 类型
	 *
	 * same as page_mkwrite when using VM_PFNMAP|VM_MIXEDMAP
	 */
	vm_fault_t (*pfn_mkwrite)(struct vm_fault *vmf);

	/**
	 * @access: 访问进程虚拟内存（用于调试、ptrace 等）
	 *
	 * 设计原因：
	 * 当 get_user_pages() 失败时被 access_process_vm() 调用，
	 * 通常用于特殊的 VMA（如设备内存映射）。
	 *
	 * 输入参数：
	 * @vma: 目标 VMA
	 * @addr: 要访问的虚拟地址
	 * @buf: 数据缓冲区
	 * @len: 访问长度
	 * @write: 是否为写操作（0 = 读，1 = 写）
	 *
	 * 返回值：
	 * 实际访问的字节数，失败返回负数
	 *
	 * 注意事项：
	 * 参见 generic_access_phys() 以获取适用于任何 iomem 映射的通用实现。
	 *
	 * called by access_process_vm when get_user_pages() fails, typically
	 * for use by special VMAs. See also generic_access_phys() for a generic
	 * implementation useful for any iomem mapping.
	 */
	int (*access)(struct vm_area_struct *vma, unsigned long addr,
		      void *buf, int len, int write);

	/**
	 * @name: 获取 VMA 的特殊名称
	 *
	 * 设计原因：
	 * /proc/PID/maps 代码调用此函数询问 VMA 是否有特殊名称
	 * （如 "[stack]"、"[heap]"、"[vdso]" 等）。
	 *
	 * 输入参数：
	 * @vma: 目标 VMA
	 *
	 * 返回值：
	 * 非 NULL 表示特殊名称，同时会导致此 VMA 无条件地被转储
	 *
	 * Called by the /proc/PID/maps code to ask the vma whether it
	 * has a special name.  Returning non-NULL will also cause this
	 * vma to be dumped unconditionally.
	 */
	const char *(*name)(struct vm_area_struct *vma);

#ifdef CONFIG_NUMA
	/**
	 * @set_policy: 设置 NUMA 内存策略
	 *
	 * 设计原因：
	 * NUMA（非一致性内存访问）系统中，不同的内存节点访问速度不同。
	 * 此回调允许为 VMA 设置特定的内存分配策略。
	 *
	 * 注意事项：
	 * set_policy() 必须对任何非 NULL 的 @new mempolicy 添加引用计数，
	 * 以在返回时持有策略。调用者应传递 NULL @new 来移除策略并回退到
	 * 周围的上下文（即不安装默认策略）。
	 *
	 * 输入参数：
	 * @new: 新的内存策略（NULL 表示移除）
	 *
	 * 返回值：
	 * 0 表示成功，非 0 表示失败
	 *
	 * set_policy() op must add a reference to any non-NULL @new mempolicy
	 * to hold the policy upon return.  Caller should pass NULL @new to
	 * remove a policy and fall back to surrounding context--i.e. do not
	 * 不安装 MPOL_DEFAULT 策略，也不安装任务或系统默认的 mempolicy - install a MPOL_DEFAULT policy, nor the task or system default
	 * mempolicy.
	 */
	int (*set_policy)(struct vm_area_struct *vma, struct mempolicy *new);

	/**
	 * @get_policy: 获取 NUMA 内存策略
	 *
	 * 设计原因：
	 * 查询指定地址的内存分配策略。
	 *
	 * 注意事项：
	 * get_policy() 必须对任何在 (vma, addr) 标记为 MPOL_SHARED 的策略
	 * 添加引用 [mpol_get()]。共享策略基础设施（mm/mempolicy.c）会自动执行此操作。
	 *
	 * get_policy() 不能对未标记为 MPOL_SHARED 的策略添加引用。
	 * VMA 策略由 mmap_lock 保护。
	 *
	 * 如果在该地址不存在 [共享/VMA] 内存策略，get_policy() 必须返回 NULL
	 * （即不"回退"到任务或系统默认策略）。
	 *
	 * 输入参数：
	 * @vma: 目标 VMA
	 * @addr: 虚拟地址
	 * @ilx: 交错索引（interleave index）的输出参数
	 *
	 * 返回值：
	 * 内存策略指针，如果没有则返回 NULL
	 *
	 * get_policy() op must add reference [mpol_get()] to any policy at
	 * (vma,addr) marked as MPOL_SHARED.  The shared policy infrastructure
	 * in mm/mempolicy.c will do this automatically.
	 * get_policy() must NOT add a ref if the policy at (vma,addr) is not
	 * marked as MPOL_SHARED. vma policies are protected by the mmap_lock.
	 * If no [shared/vma] mempolicy exists at the addr, get_policy() op
	 * must return NULL--i.e., do not "fallback" to task or system default
	 * policy.
	 */
	struct mempolicy *(*get_policy)(struct vm_area_struct *vma,
					unsigned long addr, pgoff_t *ilx);
#endif
#ifdef CONFIG_FIND_NORMAL_PAGE
	/**
	 * @find_normal_page: 为特殊 PTE 查找"普通"页面
	 *
	 * 设计原因：
	 * 有些 PTE（页表项）被标记为"特殊"（special），通常表示该页面
	 * 不应该被正常处理。但在某些情况下，我们仍然需要访问原始页面。
	 *
	 * 注意事项：
	 * 不要添加新用户！这个机制只在以下情况下工作：
	 * 一个"普通"页面被映射，但随后 PTE 被改成了奇怪的东西
	 * （并标记为 special），这会导致 pte_pfn() 无法识别原始插入的页面。
	 *
	 * 输入参数：
	 * @vma: 目标 VMA
	 * @addr: 虚拟地址
	 *
	 * 返回值：
	 * struct page 指针，如果找不到则返回 NULL
	 *
	 * Called by vm_normal_page() for special PTEs in @vma at @addr. This
	 * allows for returning a "normal" page from vm_normal_page() even
	 * though the PTE indicates that the "struct page" either does not exist
	 * or should not be touched: "special".
	 *
	 * Do not add new users: this really only works when a "normal" page
	 * was mapped, but then the PTE got changed to something weird (+
	 * marked special) that would not make pte_pfn() identify the originally
	 * inserted page.
	 */
	struct page *(*find_normal_page)(struct vm_area_struct *vma,
					 unsigned long addr);
#endif /* CONFIG_FIND_NORMAL_PAGE */
#ifdef CONFIG_USERFAULTFD
	const struct vm_uffd_ops *uffd_ops;	/* userfaultfd 操作函数表 - userfaultfd operations */
#endif
};

#ifdef CONFIG_NUMA_BALANCING
/**
 * vma_numab_state_init - 初始化 VMA 的 NUMA 平衡状态
 * @vma: 目标 VMA
 *
 * 设计原因：
 * NUMA 平衡（NUMA balancing）是一种自动优化机制，
 * 通过监控页面访问模式并将页面迁移到访问它的 CPU 所在的内存节点，
 * 以减少跨节点内存访问的延迟。
 */
static inline void vma_numab_state_init(struct vm_area_struct *vma)
{
	vma->numab_state = NULL;
}
/**
 * vma_numab_state_free - 释放 VMA 的 NUMA 平衡状态
 * @vma: 目标 VMA
 */
static inline void vma_numab_state_free(struct vm_area_struct *vma)
{
	kfree(vma->numab_state);
}
#else
static inline void vma_numab_state_init(struct vm_area_struct *vma) {}
static inline void vma_numab_state_free(struct vm_area_struct *vma) {}
#endif /* CONFIG_NUMA_BALANCING */

/*
 * 页面错误锁释放和断言函数
 *
 * 设计原因：
 * 这些必须在此处而不是 mmap_lock.h 中，因为它们依赖于 vm_fault 类型，
 * 而该类型在本头文件中声明。
 *
 * These must be here rather than mmap_lock.h as dependent on vm_fault type,
 * declared in this header.
 */
#ifdef CONFIG_PER_VMA_LOCK
/**
 * release_fault_lock - 释放页面错误处理期间持有的锁
 * @vmf: 页面错误信息
 *
 * 设计原因：
 * CONFIG_PER_VMA_LOCK 启用时，页面错误处理可以使用两种锁之一：
 * 1. VMA 级别的锁（更细粒度，性能更好）
 * 2. MM 级别的 mmap_lock（粗粒度，但通用）
 *
 * 此函数根据错误标志选择正确的锁来释放。
 *
 * C 语言知识：
 * - static inline: 内联函数，在头文件中定义
 * - if-else: 条件分支
 */
static inline void release_fault_lock(struct vm_fault *vmf)
{
	if (vmf->flags & FAULT_FLAG_VMA_LOCK)
		vma_end_read(vmf->vma);		/* 释放 VMA 读锁 */
	else
		mmap_read_unlock(vmf->vma->vm_mm);	/* 释放 MM 读锁 */
}

/**
 * assert_fault_locked - 断言页面错误期间持有了正确的锁
 * @vmf: 页面错误信息
 *
 * 设计原因：
 * 在调试构建中验证锁定协议是否正确遵守。
 */
static inline void assert_fault_locked(const struct vm_fault *vmf)
{
	if (vmf->flags & FAULT_FLAG_VMA_LOCK)
		vma_assert_locked(vmf->vma);
	else
		mmap_assert_locked(vmf->vma->vm_mm);
}
#else
static inline void release_fault_lock(struct vm_fault *vmf)
{
	mmap_read_unlock(vmf->vma->vm_mm);
}

static inline void assert_fault_locked(const struct vm_fault *vmf)
{
	mmap_assert_locked(vmf->vma->vm_mm);
}
#endif /* CONFIG_PER_VMA_LOCK */

/**
 * mm_flags_test - 测试 MM 结构的标志位
 * @flag: 要测试的标志位编号
 * @mm: 目标 MM 结构
 *
 * 设计原因：
 * mm_struct 的 flags 字段需要特殊的访问方式（通过 ACCESS_PRIVATE 宏），
 * 以便进行并发控制和调试。
 *
 * C 语言知识：
 * - test_bit: 测试位图中的某一位是否被设置
 * - ACCESS_PRIVATE: 访问私有字段的宏（用于 KCSAN 等工具）
 *
 * 返回值：
 * true 表示标志位被设置，false 表示未设置
 */
static inline bool mm_flags_test(int flag, const struct mm_struct *mm)
{
	return test_bit(flag, ACCESS_PRIVATE(&mm->flags, __mm_flags));
}

/**
 * mm_flags_test_and_set - 测试并设置 MM 标志位（原子操作）
 * @flag: 要测试和设置的标志位编号
 * @mm: 目标 MM 结构
 *
 * 设计原因：
 * 原子地测试并设置标志位，常用于实现只执行一次的初始化。
 *
 * 返回值：
 * 返回标志位的旧值（设置前的值）
 */
static inline bool mm_flags_test_and_set(int flag, struct mm_struct *mm)
{
	return test_and_set_bit(flag, ACCESS_PRIVATE(&mm->flags, __mm_flags));
}

/**
 * mm_flags_test_and_clear - 测试并清除 MM 标志位（原子操作）
 * @flag: 要测试和清除的标志位编号
 * @mm: 目标 MM 结构
 *
 * 设计原因：
 * 原子地测试并清除标志位。
 *
 * 返回值：
 * 返回标志位的旧值（清除前的值）
 */
static inline bool mm_flags_test_and_clear(int flag, struct mm_struct *mm)
{
	return test_and_clear_bit(flag, ACCESS_PRIVATE(&mm->flags, __mm_flags));
}

/**
 * mm_flags_clear - 清除 MM 标志位
 * @flag: 要清除的标志位编号
 * @mm: 目标 MM 结构
 *
 * 设计原因：
 * 清除指定的标志位（不返回旧值，比 test_and_clear 稍快）。
 */
/**
 * mm_flags_set - 设置 MM 标志位
 * @flag: 要设置的标志位编号
 * @mm: 目标 MM 结构
 *
 * 设计原因：
 * 设置指定的标志位。
 */
static inline void mm_flags_set(int flag, struct mm_struct *mm)
{
	set_bit(flag, ACCESS_PRIVATE(&mm->flags, __mm_flags));
}

static inline void mm_flags_clear(int flag, struct mm_struct *mm)
{
	clear_bit(flag, ACCESS_PRIVATE(&mm->flags, __mm_flags));
}

/**
 * mm_flags_clear_all - 清除 MM 的所有标志位
 * @mm: 目标 MM 结构
 *
 * 设计原因：
 * 一次性清除所有标志位，用于初始化或重置。
 *
 * C 语言知识：
 * - bitmap_zero: 将位图的所有位设置为 0
 */
static inline void mm_flags_clear_all(struct mm_struct *mm)
{
	bitmap_zero(ACCESS_PRIVATE(&mm->flags, __mm_flags), NUM_MM_FLAG_BITS);
}

/*
 * vma_dummy_vm_ops - 虚拟的 VMA 操作表
 *
 * 设计原因：
 * 提供一个空的操作表，用于初始化 VMA。
 */
extern const struct vm_operations_struct vma_dummy_vm_ops;

/**
 * vma_init - 初始化 VMA 结构
 * @vma: 要初始化的 VMA
 * @mm: VMA 所属的 MM 结构
 *
 * 设计原因：
 * 在分配 VMA 后进行初始化，设置默认值。
 *
 * C 语言知识：
 * - memset: 将内存块设置为指定值（这里是 0，即清零）
 * - sizeof(*vma): 获取 vma 指向的结构体的大小
 * - &: 取地址运算符
 * - INIT_LIST_HEAD: 初始化链表头
 *
 * 初始化步骤：
 * 1. 清零整个结构体
 * 2. 设置所属的 MM
 * 3. 设置默认的虚拟操作表
 * 4. 初始化匿名 VMA 链表
 * 5. 初始化 VMA 锁
 */
static inline void vma_init(struct vm_area_struct *vma, struct mm_struct *mm)
{
	memset(vma, 0, sizeof(*vma));
	vma->vm_mm = mm;
	vma->vm_ops = &vma_dummy_vm_ops;
	INIT_LIST_HEAD(&vma->anon_vma_chain);
	vma_lock_init(vma, false);
}

/**
 * vm_flags_init - 初始化 VMA 标志
 * @vma: 目标 VMA
 * @flags: 要设置的标志
 *
 * 设计原因：
 * 当 VMA 不是 VMA 树的一部分且不需要锁定时使用。
 * 直接设置标志，不进行同步。
 *
 * 注意事项：
 * 如果页表不支持软脏位（soft dirty）但标志中包含 VM_SOFTDIRTY，
 * 会发出警告。
 *
 * Use when VMA is not part of the VMA tree and needs no locking
 */
static inline void vm_flags_init(struct vm_area_struct *vma,
				 vm_flags_t flags)
{
	VM_WARN_ON_ONCE(!pgtable_supports_soft_dirty() && (flags & VM_SOFTDIRTY));
	vma_flags_clear_all(&vma->flags);
	vma_flags_overwrite_word(&vma->flags, flags);
}

/**
 * vm_flags_reset - 重置 VMA 标志（需要锁）
 * @vma: 目标 VMA
 * @flags: 新的标志值
 *
 * 设计原因：
 * 当 VMA 是 VMA 树的一部分且修改需要协调时使用。
 *
 * 注意事项：
 * vm_flags_reset 和 vm_flags_reset_once 不会自动锁定 VMA，
 * 必须在调用前显式锁定。
 *
 * Use when VMA is part of the VMA tree and modifications need coordination
 * Note: vm_flags_reset and vm_flags_reset_once do not lock the vma and
 * it should be locked explicitly beforehand.
 */
static inline void vm_flags_reset(struct vm_area_struct *vma,
				  vm_flags_t flags)
{
	VM_WARN_ON_ONCE(!pgtable_supports_soft_dirty() && (flags & VM_SOFTDIRTY));
	vma_assert_write_locked(vma);
	vm_flags_init(vma, flags);
}

/**
 * vma_flags_reset_once - 原子地重置 VMA 标志（只写一次）
 * @vma: 目标 VMA
 * @flags: 新的标志值
 *
 * 设计原因：
 * 对于某些架构，VMA 标志的第一个字（word）需要原子写入一次，
 * 以避免并发读取时看到部分更新的值。其余部分可以正常复制。
 *
 * C 语言知识：
 * - const: 常量，不能被修改
 * - bitmap_copy: 复制位图
 * - BITS_PER_LONG: 一个 long 类型有多少位（32 位或 64 位）
 *
 * 注意事项：
 * 假设只有第一个系统字必须原子写入一次。
 */
static inline void vma_flags_reset_once(struct vm_area_struct *vma,
					vma_flags_t *flags)
{
	const unsigned long word = flags->__vma_flags[0];

	/* 假设只有第一个系统字必须原子写入一次 - It is assumed only the first system word must be written once. */
	vma_flags_overwrite_word_once(&vma->flags, word);
	/* 其余部分可以正常复制 - The remainder can be copied normally. */
	if (NUM_VMA_FLAG_BITS > BITS_PER_LONG) {
		unsigned long *dst = &vma->flags.__vma_flags[1];
		const unsigned long *src = &flags->__vma_flags[1];

		bitmap_copy(dst, src, NUM_VMA_FLAG_BITS - BITS_PER_LONG);
	}
}

/**
 * vm_flags_set - 设置 VMA 标志位（带锁）
 * @vma: 目标 VMA
 * @flags: 要设置的标志位
 *
 * 设计原因：
 * 安全地设置 VMA 标志位，自动获取写锁。
 */
static inline void vm_flags_set(struct vm_area_struct *vma,
				vm_flags_t flags)
{
	vma_start_write(vma);
	vma_flags_set_word(&vma->flags, flags);
}

/**
 * vm_flags_clear - 清除 VMA 标志位（带锁）
 * @vma: 目标 VMA
 * @flags: 要清除的标志位
 *
 * 设计原因：
 * 安全地清除 VMA 标志位，自动获取写锁。
 *
 * 注意事项：
 * 如果页表不支持软脏位但尝试清除 VM_SOFTDIRTY，会发出警告。
 */
static inline void vm_flags_clear(struct vm_area_struct *vma,
				  vm_flags_t flags)
{
	VM_WARN_ON_ONCE(!pgtable_supports_soft_dirty() && (flags & VM_SOFTDIRTY));
	vma_start_write(vma);
	vma_flags_clear_word(&vma->flags, flags);
}

/**
 * __vm_flags_mod - 修改 VMA 标志（无锁版本）
 * @vma: 目标 VMA
 * @set: 要设置的标志位
 * @clear: 要清除的标志位
 *
 * 设计原因：
 * 仅当 VMA 不是 VMA 树的一部分或没有其他用户时使用，
 * 因此不需要锁定。
 *
 * C 语言知识：
 * - |: 按位或运算符（设置位）
 * - &: 按位与运算符
 * - ~: 按位取反运算符
 * - (vma->vm_flags | set) & ~clear: 先设置 set 的位，再清除 clear 的位
 *
 * Use only if VMA is not part of the VMA tree or has no other users and
 * therefore needs no locking.
 */
static inline void __vm_flags_mod(struct vm_area_struct *vma,
				  vm_flags_t set, vm_flags_t clear)
{
	vm_flags_init(vma, (vma->vm_flags | set) & ~clear);
}

/**
 * vm_flags_mod - 修改 VMA 标志（带锁版本）
 * @vma: 目标 VMA
 * @set: 要设置的标志位
 * @clear: 要清除的标志位
 *
 * 设计原因：
 * 仅当 set/clear 操作的顺序不重要时使用，
 * 否则应显式使用 vm_flags_set 或 vm_flags_clear。
 *
 * Use only when the order of set/clear operations is unimportant, otherwise
 * use vm_flags_{set|clear} explicitly.
 */
static inline void vm_flags_mod(struct vm_area_struct *vma,
				vm_flags_t set, vm_flags_t clear)
{
	vma_start_write(vma);
	__vm_flags_mod(vma, set, clear);
}

/**
 * __vma_atomic_valid_flag - 验证 VMA 标志是否允许原子操作
 * @vma: 目标 VMA
 * @bit: 要验证的标志位
 *
 * 设计原因：
 * 并非所有 VMA 标志都允许原子操作（只需读锁）。
 * 此函数检查指定的标志是否在允许的列表中。
 *
 * C 语言知识：
 * - __always_inline: 强制内联，比 static inline 更强制
 * - __force: 类型转换注解（用于静态分析工具）
 * - BIT(n): 创建一个只有第 n 位为 1 的掩码
 * - WARN_ON_ONCE: 如果条件为真，输出警告（只警告一次）
 *
 * 返回值：
 * true 表示该标志允许原子操作，false 表示不允许
 */
static __always_inline bool __vma_atomic_valid_flag(struct vm_area_struct *vma,
		vma_flag_t bit)
{
	const vm_flags_t mask = BIT((__force int)bit);

	/* 只有特定标志被允许 - Only specific flags are permitted */
	if (WARN_ON_ONCE(!(mask & VM_ATOMIC_SET_ALLOWED)))
		return false;

	return true;
}

/**
 * vma_set_atomic_flag - 原子地设置 VMA 标志
 * @vma: 目标 VMA
 * @bit: 要设置的标志位
 *
 * 设计原因：
 * 某些操作需要快速设置 VMA 标志而不获取昂贵的写锁。
 * 只需要 VMA/mmap 读锁。只有特定的有效标志允许这样做。
 *
 * 注意事项：
 * 并非所有标志都允许原子设置，只有 VM_ATOMIC_SET_ALLOWED 中的标志才行。
 *
 * Set VMA flag atomically. Requires only VMA/mmap read lock. Only specific
 * valid flags are allowed to do this.
 */
static __always_inline void vma_set_atomic_flag(struct vm_area_struct *vma,
		vma_flag_t bit)
{
	unsigned long *bitmap = vma->flags.__vma_flags;

	vma_assert_stabilised(vma);
	if (__vma_atomic_valid_flag(vma, bit))
		set_bit((__force int)bit, bitmap);
}

/**
 * vma_test_atomic_flag - 原子地测试 VMA 标志
 * @vma: 目标 VMA
 * @bit: 要测试的标志位
 *
 * 设计原因：
 * 快速测试 VMA 标志，不需要任何锁。只有特定的有效标志允许这样做。
 *
 * 注意事项：
 * 这必然是有竞争的（racey），因此调用者必须确保通过其他方式实现序列化，
 * 或者竞争是允许的。
 *
 * Test for VMA flag atomically. Requires no locks. Only specific valid flags
 * are allowed to do this.
 *
 * This is necessarily racey, so callers must ensure that serialisation is
 * achieved through some other means, or that races are permissible.
 */
static __always_inline bool vma_test_atomic_flag(struct vm_area_struct *vma,
		vma_flag_t bit)
{
	if (__vma_atomic_valid_flag(vma, bit))
		return test_bit((__force int)bit, &vma->vm_flags);

	return false;
}

/**
 * vma_flags_set_flag - 设置单个 VMA 标志（非原子）
 * @flags: 标志结构
 * @bit: 要设置的标志位
 *
 * 设计原因：
 * 在 flags 中设置单个标志位，非原子操作。
 * 用于构建标志组合，不是在已经使用的 VMA 上操作。
 *
 * C 语言知识：
 * - __set_bit: 非原子版本的 set_bit（更快，但不保证并发安全）
 *
 * Set an individual VMA flag in flags, non-atomically.
 */
static __always_inline void vma_flags_set_flag(vma_flags_t *flags,
		vma_flag_t bit)
{
	unsigned long *bitmap = flags->__vma_flags;

	__set_bit((__force int)bit, bitmap);
}

/**
 * __mk_vma_flags - 从位数组创建 VMA 标志
 * @flags: 初始标志值
 * @count: 位数组的元素数量
 * @bits: 要设置的标志位数组
 *
 * 设计原因：
 * 用于从多个标志位构建一个完整的标志集。
 * 通常通过 mk_vma_flags 宏调用，不直接使用。
 *
 * C 语言知识：
 * - size_t: 无符号整数类型，用于表示大小
 * - const vma_flag_t *bits: 指向常量标志位数组的指针
 * - for 循环: 遍历数组中的每个元素
 *
 * 返回值：
 * 设置了所有指定位的标志集
 */
static __always_inline vma_flags_t __mk_vma_flags(vma_flags_t flags,
		size_t count, const vma_flag_t *bits)
{
	int i;

	for (i = 0; i < count; i++)
		vma_flags_set_flag(&flags, bits[i]);
	return flags;
}

/*
 * mk_vma_flags - 从多个标志位创建 VMA 标志的辅助宏
 *
 * 设计原因：
 * 将指定的输入标志按位或组合成一个 vma_flags_t 位图值。
 *
 * 使用示例：
 * vma_flags_t flags = mk_vma_flags(VMA_IO_BIT, VMA_PFNMAP_BIT,
 *              VMA_DONTEXPAND_BIT, VMA_DONTDUMP_BIT);
 *
 * 注意事项：
 * 编译器会巧妙地优化掉所有工作，最终相当于手动聚合这些值。
 *
 * C 语言知识：
 * - #define: 定义宏
 * - __VA_ARGS__: 可变参数宏，代表传入的所有参数
 * - COUNT_ARGS: 计算参数数量的宏
 * - (const vma_flag_t []){...}: 复合字面量，创建一个临时数组
 *
 * Helper macro which bitwise-or combines the specified input flags into a
 * vma_flags_t bitmap value. E.g.:
 *
 * vma_flags_t flags = mk_vma_flags(VMA_IO_BIT, VMA_PFNMAP_BIT,
 *              VMA_DONTEXPAND_BIT, VMA_DONTDUMP_BIT);
 *
 * The compiler cleverly optimises away all of the work and this ends up being
 * equivalent to aggregating the values manually.
 */
#define mk_vma_flags(...) __mk_vma_flags(EMPTY_VMA_FLAGS,			\
		COUNT_ARGS(__VA_ARGS__), (const vma_flag_t []){__VA_ARGS__})

/*
 * append_vma_flags - 向现有标志追加新标志的辅助宏
 *
 * 设计原因：
 * 类似于 mk_vma_flags，但不是建立新标志，而是追加到指定标志的副本。
 *
 * 使用示例：
 * vma_flags_t flags = append_vma_flags(VMA_STACK_DEFAULT_FLAGS, VMA_STACK_BIT,
 *              VMA_ACCOUNT_BIT);
 *
 * Helper macro which acts like mk_vma_flags, only appending to a copy of the
 * specified flags rather than establishing new flags. E.g.:
 *
 * vma_flags_t flags = append_vma_flags(VMA_STACK_DEFAULT_FLAGS, VMA_STACK_BIT,
 *              VMA_ACCOUNT_BIT);
 */
#define append_vma_flags(flags, ...) __mk_vma_flags(flags,			\
		COUNT_ARGS(__VA_ARGS__), (const vma_flag_t []){__VA_ARGS__})

/**
 * vma_flags_count - 计算 VMA 标志中设置的位数
 * @flags: 要计数的标志
 *
 * 设计原因：
 * 统计标志中有多少位被设置为 1。
 *
 * C 语言知识：
 * - bitmap_weight: 计算位图中设置为 1 的位数
 *
 * 返回值：
 * 设置的位数
 *
 * Calculates the number of set bits in the specified VMA flags.
 */
static __always_inline int vma_flags_count(const vma_flags_t *flags)
{
	const unsigned long *bitmap = flags->__vma_flags;

	return bitmap_weight(bitmap, NUM_VMA_FLAG_BITS);
}

/**
 * vma_flags_test - 测试特定的 VMA 标志是否被设置
 * @flags: 要测试的标志
 * @bit: 要测试的标志位
 *
 * 设计原因：
 * 检查标志中的某一位是否为 1。
 *
 * 使用示例：
 * if (vma_flags_test(flags, VMA_READ_BIT)) { ... }
 *
 * 返回值：
 * true 表示该位被设置，false 表示未设置
 *
 * Test whether a specific VMA flag is set, e.g.:
 *
 * if (vma_flags_test(flags, VMA_READ_BIT)) { ... }
 */
static __always_inline bool vma_flags_test(const vma_flags_t *flags,
		vma_flag_t bit)
{
	const unsigned long *bitmap = flags->__vma_flags;

	return test_bit((__force int)bit, bitmap);
}

/**
 * vma_flags_and_mask - 获取两个标志集的交集
 * @flags: 第一个标志集
 * @to_and: 第二个标志集
 *
 * 设计原因：
 * 获取 flags 和 to_and 中都存在的标志（按位与操作）。
 *
 * C 语言知识：
 * - bitmap_and: 对两个位图执行按位与操作
 *
 * 返回值：
 * 包含两个标志集重叠部分的新标志集
 *
 * Obtain a set of VMA flags which contain the overlapping flags contained
 * within flags and to_and.
 */
static __always_inline vma_flags_t vma_flags_and_mask(const vma_flags_t *flags,
						      vma_flags_t to_and)
{
	vma_flags_t dst;
	unsigned long *bitmap_dst = dst.__vma_flags;
	const unsigned long *bitmap = flags->__vma_flags;
	const unsigned long *bitmap_to_and = to_and.__vma_flags;

	bitmap_and(bitmap_dst, bitmap, bitmap_to_and, NUM_VMA_FLAG_BITS);
	return dst;
}

/**
 * vma_flags_and - 获取指定标志的交集（宏）
 *
 * 设计原因：
 * 更方便的方式来获取标志交集，可以直接传入多个标志位。
 *
 * 使用示例：
 * vma_flags_t read_flags = vma_flags_and(&flags, VMA_READ_BIT,
 *                                        VMA_MAY_READ_BIT);
 *
 * Obtain a set of VMA flags which contains the specified overlapping flags,
 * e.g.:
 *
 * vma_flags_t read_flags = vma_flags_and(&flags, VMA_READ_BIT,
 *                                        VMA_MAY_READ_BIT);
 */
#define vma_flags_and(flags, ...)				\
	vma_flags_and_mask(flags, mk_vma_flags(__VA_ARGS__))

/**
 * vma_flags_test_any_mask - 测试是否有任何指定标志被设置
 * @flags: 要测试的标志
 * @to_test: 要检查的标志
 *
 * 设计原因：
 * 检查 to_test 中的任何标志是否在 flags 中被设置（非原子操作）。
 *
 * C 语言知识：
 * - bitmap_intersects: 检查两个位图是否有交集
 *
 * 返回值：
 * true 表示至少有一个标志被设置，false 表示没有
 *
 * Test each of to_test flags in flags, non-atomically.
 */
static __always_inline bool vma_flags_test_any_mask(const vma_flags_t *flags,
		vma_flags_t to_test)
{
	const unsigned long *bitmap = flags->__vma_flags;
	const unsigned long *bitmap_to_test = to_test.__vma_flags;

	return bitmap_intersects(bitmap_to_test, bitmap, NUM_VMA_FLAG_BITS);
}

/**
 * vma_flags_test_any - 测试是否有任何指定标志被设置（宏）
 *
 * 设计原因：
 * 更方便的方式来测试多个标志位。
 *
 * 使用示例：
 * if (vma_flags_test_any(flags, VMA_READ_BIT, VMA_MAYREAD_BIT)) { ... }
 *
 * Test whether any specified VMA flag is set, e.g.:
 *
 * if (vma_flags_test_any(flags, VMA_READ_BIT, VMA_MAYREAD_BIT)) { ... }
 */
#define vma_flags_test_any(flags, ...) \
	vma_flags_test_any_mask(flags, mk_vma_flags(__VA_ARGS__))

/**
 * vma_flags_test_all_mask - 测试是否所有指定标志都被设置
 * @flags: 要测试的标志
 * @to_test: 要检查的标志
 *
 * 设计原因：
 * 检查 to_test 中的所有标志是否都在 flags 中被设置（非原子操作）。
 *
 * C 语言知识：
 * - bitmap_subset: 检查第一个位图是否是第二个的子集
 *
 * 返回值：
 * true 表示所有标志都被设置，false 表示至少有一个未设置
 *
 * Test that ALL of the to_test flags are set, non-atomically.
 */
static __always_inline bool vma_flags_test_all_mask(const vma_flags_t *flags,
		vma_flags_t to_test)
{
	const unsigned long *bitmap = flags->__vma_flags;
	const unsigned long *bitmap_to_test = to_test.__vma_flags;

	return bitmap_subset(bitmap_to_test, bitmap, NUM_VMA_FLAG_BITS);
}

/**
 * vma_flags_test_all - 测试是否所有指定标志都被设置（宏）
 *
 * 设计原因：
 * 更方便的方式来测试所有标志位是否都被设置。
 *
 * 使用示例：
 * if (vma_flags_test_all(flags, VMA_READ_BIT, VMA_MAYREAD_BIT)) { ... }
 *
 * Test whether ALL specified VMA flags are set, e.g.:
 *
 * if (vma_flags_test_all(flags, VMA_READ_BIT, VMA_MAYREAD_BIT)) { ... }
 */
#define vma_flags_test_all(flags, ...) \
	vma_flags_test_all_mask(flags, mk_vma_flags(__VA_ARGS__))

/**
 * vma_flags_test_single_mask - 测试标志掩码是否只设置了单个标志
 * @flagmask: 要测试的标志掩码
 *
 * 设计原因：
 * 用于测试 vma_flags_t 类型的标志掩码是否只有一个标志被设置（如果没有标志被设置则返回 false）。
 * 这个函数使语义更清晰，特别是在测试可选定义的 VMA 标志掩码时。
 *
 * 使用场景：
 * 当某个标志（如 VMA_DROPPABLE）在某些配置下被定义，在其他配置下被设置为 EMPTY_VMA_FLAGS 时。
 *
 * C 语言知识：
 * - vma_flags_count: 计算设置的标志位数量
 *
 * 返回值：
 * true 表示只有一个标志被设置，false 表示没有标志或有多个标志被设置
 *
 * 使用示例：
 * if (vma_flags_test_single_mask(&flags, VMA_DROPPABLE)) { ... }
 *
 * Helper to test that a flag mask of type vma_flags_t has a SINGLE flag set
 * (returning false if flagmask has no flags set).
 *
 * This is defined to make the semantics clearer when testing an optionally
 * defined VMA flags mask, e.g.:
 *
 * if (vma_flags_test_single_mask(&flags, VMA_DROPPABLE)) { ... }
 *
 * When VMA_DROPPABLE is defined if available, or set to EMPTY_VMA_FLAGS
 * otherwise.
 */
static __always_inline bool vma_flags_test_single_mask(const vma_flags_t *flags,
		vma_flags_t flagmask)
{
	VM_WARN_ON_ONCE(vma_flags_count(&flagmask) > 1);

	return vma_flags_test_any_mask(flags, flagmask);
}

/**
 * vma_flags_set_mask - 设置指定的标志（非原子操作）
 * @flags: 目标标志集
 * @to_set: 要设置的标志
 *
 * 设计原因：
 * 在 flags 中设置 to_set 中的每个标志（非原子操作）。
 *
 * C 语言知识：
 * - bitmap_or: 对两个位图执行按位或操作
 *
 * Set each of the to_set flags in flags, non-atomically.
 */
static __always_inline void vma_flags_set_mask(vma_flags_t *flags,
		vma_flags_t to_set)
{
	unsigned long *bitmap = flags->__vma_flags;
	const unsigned long *bitmap_to_set = to_set.__vma_flags;

	bitmap_or(bitmap, bitmap, bitmap_to_set, NUM_VMA_FLAG_BITS);
}

/**
 * vma_flags_set - 设置所有指定的 VMA 标志（宏）
 *
 * 设计原因：
 * 更方便地设置多个标志位。
 *
 * 使用示例：
 * vma_flags_set(&flags, VMA_READ_BIT, VMA_WRITE_BIT, VMA_EXEC_BIT);
 *
 * Set all specified VMA flags, e.g.:
 *
 * vma_flags_set(&flags, VMA_READ_BIT, VMA_WRITE_BIT, VMA_EXEC_BIT);
 */
#define vma_flags_set(flags, ...) \
	vma_flags_set_mask(flags, mk_vma_flags(__VA_ARGS__))

/**
 * __mk_vma_flags_from_masks - 从掩码数组创建 VMA 标志
 * @count: 掩码数量
 * @masks: 掩码数组
 *
 * 设计原因：
 * 将多个预先计算的 vma_flags_t 掩码合并成一个值。
 * 与 mk_vma_flags()（接受位编号）不同，这个函数接受完整的掩码。
 *
 * 使用场景：
 * 每个掩码在功能不可用时可能是 EMPTY_VMA_FLAGS，这样当前构建中不存在的位就不会被实例化。
 *
 * 返回值：
 * 合并后的标志集
 *
 * C 语言知识：
 * - size_t: 无符号整数类型，用于表示对象大小
 * - for 循环: 遍历所有掩码并合并
 */
static __always_inline vma_flags_t __mk_vma_flags_from_masks(size_t count,
		const vma_flags_t *masks)
{
	vma_flags_t flags = EMPTY_VMA_FLAGS;
	size_t i;

	for (i = 0; i < count; i++)
		vma_flags_set_mask(&flags, masks[i]);
	return flags;
}

/**
 * mk_vma_flags_from_masks - 从预计算的掩码合并标志（宏）
 *
 * 设计原因：
 * 将预先计算的 vma_flags_t 掩码合并成一个值。
 * 与 mk_vma_flags()（接受位编号）不同，这个宏接受完整的掩码——
 * 每个掩码在其功能不可用时可能是 EMPTY_VMA_FLAGS——
 * 因此当前构建中不存在的位永远不会被实例化。
 *
 * 使用示例：
 * vma_flags_t flags = mk_vma_flags_from_masks(VMA_UFFD_WP, VMA_UFFD_MINOR);
 *
 * C 语言知识：
 * - COUNT_ARGS: 计算可变参数的数量
 * - 复合字面量: (const vma_flags_t []){...} 创建临时数组
 *
 * Combine pre-computed vma_flags_t masks into one value, e.g.:
 *
 * vma_flags_t flags = mk_vma_flags_from_masks(VMA_UFFD_WP, VMA_UFFD_MINOR);
 *
 * Unlike mk_vma_flags(), which takes bit numbers, this takes whole masks --
 * each of which may be EMPTY_VMA_FLAGS when its feature is unavailable -- so a
 * bit that does not exist on the current build is never materialised.
 */
#define mk_vma_flags_from_masks(...)					\
	__mk_vma_flags_from_masks(COUNT_ARGS(__VA_ARGS__),		\
		(const vma_flags_t []){__VA_ARGS__})

/**
 * vma_flags_clear_mask - 清除指定的标志（非原子操作）
 * @flags: 目标标志集
 * @to_clear: 要清除的标志
 *
 * 设计原因：
 * 在 flags 中清除 to_clear 中的所有标志（非原子操作）。
 *
 * C 语言知识：
 * - bitmap_andnot: 对两个位图执行按位与非操作（第一个与第二个的反）
 *
 * Clear all of the to-clear flags in flags, non-atomically.
 */
static __always_inline void vma_flags_clear_mask(vma_flags_t *flags,
		vma_flags_t to_clear)
{
	unsigned long *bitmap = flags->__vma_flags;
	const unsigned long *bitmap_to_clear = to_clear.__vma_flags;

	bitmap_andnot(bitmap, bitmap, bitmap_to_clear, NUM_VMA_FLAG_BITS);
}

/**
 * vma_flags_clear - 清除所有指定的单个标志（宏）
 *
 * 设计原因：
 * 更方便地清除多个标志位。
 *
 * 使用示例：
 * vma_flags_clear(&flags, VMA_READ_BIT, VMA_WRITE_BIT, VMA_EXEC_BIT);
 *
 * Clear all specified individual flags, e.g.:
 *
 * vma_flags_clear(&flags, VMA_READ_BIT, VMA_WRITE_BIT, VMA_EXEC_BIT);
 */
#define vma_flags_clear(flags, ...) \
	vma_flags_clear_mask(flags, mk_vma_flags(__VA_ARGS__))

/**
 * vma_flags_xor - 获取两个标志集的异或结果
 * @flags: 第一个标志集
 * @flags_other: 第二个标志集
 *
 * 设计原因：
 * 获取一个 VMA 标志值，包含在 flags 或 flags_other 中存在但不在两者中同时存在的标志。
 *
 * C 语言知识：
 * - XOR（异或）: 按位异或操作，相同为 0，不同为 1
 *
 * 返回值：
 * 包含异或结果的新标志集
 *
 * Obtain a VMA flags value containing those flags that are present in flags or
 * flags_other but not in both.
 */
static __always_inline vma_flags_t vma_flags_diff_pair(const vma_flags_t *flags,
		const vma_flags_t *flags_other)
{
	vma_flags_t dst;
	const unsigned long *bitmap_other = flags_other->__vma_flags;
	const unsigned long *bitmap = flags->__vma_flags;
	unsigned long *bitmap_dst = dst.__vma_flags;

	bitmap_xor(bitmap_dst, bitmap, bitmap_other, NUM_VMA_FLAG_BITS);
	return dst;
}

/**
 * vma_flags_same_pair - 判断两个标志集是否完全相同
 * @flags: 第一个标志集指针
 * @flags_other: 第二个标志集指针
 *
 * 设计原因：
 * 确定 flags 和 flags_other 是否设置了完全相同的标志。
 *
 * C 语言知识：
 * - bitmap_equal: 检查两个位图是否相等
 *
 * 返回值：
 * true 表示两个标志集完全相同，false 表示不同
 *
 * Determine if flags and flags_other have precisely the same flags set.
 */
static __always_inline bool vma_flags_same_pair(const vma_flags_t *flags,
						const vma_flags_t *flags_other)
{
	const unsigned long *bitmap = flags->__vma_flags;
	const unsigned long *bitmap_other = flags_other->__vma_flags;

	return bitmap_equal(bitmap, bitmap_other, NUM_VMA_FLAG_BITS);
}

/**
 * vma_flags_same_mask - 判断两个标志集是否完全相同（按值传递版本）
 * @flags: 第一个标志集指针
 * @flags_other: 第二个标志集（按值传递）
 *
 * 设计原因：
 * 确定 flags 和 flags_other 是否设置了完全相同的标志（按值传递版本）。
 *
 * 返回值：
 * true 表示两个标志集完全相同，false 表示不同
 *
 * Determine if flags and flags_other have precisely the same flags set.
 */
static __always_inline bool vma_flags_same_mask(const vma_flags_t *flags,
						vma_flags_t flags_other)
{
	const unsigned long *bitmap = flags->__vma_flags;
	const unsigned long *bitmap_other = flags_other.__vma_flags;

	return bitmap_equal(bitmap, bitmap_other, NUM_VMA_FLAG_BITS);
}

/**
 * vma_flags_same - 判断是否只设置了指定的标志（宏）
 *
 * 设计原因：
 * 辅助宏，用于确定是否只设置了特定的标志。
 *
 * 使用示例：
 * if (vma_flags_same(&flags, VMA_WRITE_BIT)) { ... }
 *
 * Helper macro to determine if only the specific flags are set, e.g.:
 *
 * if (vma_flags_same(&flags, VMA_WRITE_BIT) { ... }
 */
#define vma_flags_same(flags, ...) \
	vma_flags_same_mask(flags, mk_vma_flags(__VA_ARGS__))

/**
 * vma_test - 测试 VMA 中的特定标志是否被设置
 * @vma: 虚拟内存区域结构体指针
 * @bit: 要测试的标志位
 *
 * 设计原因：
 * 测试 VMA 中的特定标志是否被设置。
 *
 * 使用示例：
 * if (vma_test(vma, VMA_READ_BIT)) { ... }
 *
 * 返回值：
 * true 表示该标志被设置，false 表示未设置
 *
 * Test whether a specific flag in the VMA is set, e.g.:
 *
 * if (vma_test(vma, VMA_READ_BIT)) { ... }
 */
static __always_inline bool vma_test(const struct vm_area_struct *vma,
		vma_flag_t bit)
{
	return vma_flags_test(&vma->flags, bit);
}

/**
 * vma_test_any_mask - 测试 VMA 中是否有任何指定标志被设置
 * @vma: 虚拟内存区域结构体指针
 * @flags: 要测试的标志集
 *
 * 设计原因：
 * 辅助函数，用于测试 VMA 中的任何 VMA 标志。
 *
 * 返回值：
 * true 表示至少有一个标志被设置，false 表示没有
 *
 * Helper to test any VMA flags in a VMA .
 */
static __always_inline bool vma_test_any_mask(const struct vm_area_struct *vma,
		vma_flags_t flags)
{
	return vma_flags_test_any_mask(&vma->flags, flags);
}

/**
 * vma_test_any - 测试 VMA 中是否有任何指定标志被设置（宏）
 *
 * 设计原因：
 * 辅助宏，用于测试 VMA 中是否设置了任何 VMA 标志。
 *
 * 使用示例：
 * if (vma_test_any(vma, VMA_IO_BIT, VMA_PFNMAP_BIT,
 *                  VMA_DONTEXPAND_BIT, VMA_DONTDUMP_BIT)) { ... }
 *
 * Helper macro for testing whether any VMA flags are set in a VMA,
 * e.g.:
 *
 * if (vma_test_any(vma, VMA_IO_BIT, VMA_PFNMAP_BIT,
 *		VMA_DONTEXPAND_BIT, VMA_DONTDUMP_BIT)) { ... }
 */
#define vma_test_any(vma, ...) \
	vma_test_any_mask(vma, mk_vma_flags(__VA_ARGS__))

/**
 * vma_test_all_mask - 测试 VMA 中是否所有指定标志都被设置
 * @vma: 虚拟内存区域结构体指针
 * @flags: 要测试的标志集
 *
 * 设计原因：
 * 辅助函数，用于测试 VMA 中是否设置了所有指定的标志。
 *
 * 注意事项：
 * 必须持有适当的锁，此函数不会为你获取它们。
 *
 * 返回值：
 * true 表示所有标志都被设置，false 表示至少有一个未设置
 *
 * Helper to test that ALL specified flags are set in a VMA.
 *
 * Note: appropriate locks must be held, this function does not acquire them for
 * you.
 */
static __always_inline bool vma_test_all_mask(const struct vm_area_struct *vma,
		vma_flags_t flags)
{
	return vma_flags_test_all_mask(&vma->flags, flags);
}

/**
 * vma_test_all - 测试 VMA 中是否所有指定标志都被设置（宏）
 *
 * 设计原因：
 * 辅助宏，用于检查 VMA 中是否设置了所有指定的标志。
 *
 * 使用示例：
 * if (vma_test_all(vma, VMA_READ_BIT, VMA_MAYREAD_BIT)) { ... }
 *
 * Helper macro for checking that ALL specified flags are set in a VMA, e.g.:
 *
 * if (vma_test_all(vma, VMA_READ_BIT, VMA_MAYREAD_BIT) { ... }
 */
#define vma_test_all(vma, ...) \
	vma_test_all_mask(vma, mk_vma_flags(__VA_ARGS__))

/**
 * vma_test_single_mask - 测试 VMA 中是否只设置了单个标志
 * @vma: 虚拟内存区域结构体指针
 * @flagmask: 要测试的标志掩码
 *
 * 设计原因：
 * 辅助函数，用于测试 vma_flags_t 类型的标志掩码是否只有一个标志被设置
 * （如果 flagmask 没有标志被设置则返回 false）。
 *
 * 使用场景：
 * 当某个标志需要根据内核配置定义或不定义时，这很有用。
 *
 * 使用示例：
 * if (vma_test_single_mask(vma, VMA_DROPPABLE)) { ... }
 *
 * 当 VMA_DROPPABLE 在可用时被定义，否则被设置为 EMPTY_VMA_FLAGS。
 *
 * 返回值：
 * true 表示只有一个标志被设置，false 表示没有标志或有多个标志被设置
 *
 * Helper to test that a flag mask of type vma_flags_t has a SINGLE flag set
 * (returning false if flagmask has no flags set).
 *
 * This is useful when a flag needs to be either defined or not depending upon
 * kernel configuration, e.g.:
 *
 * if (vma_test_single_mask(vma, VMA_DROPPABLE)) { ... }
 *
 * When VMA_DROPPABLE is defined if available, or set to EMPTY_VMA_FLAGS
 * otherwise.
 */
static __always_inline bool
vma_test_single_mask(const struct vm_area_struct *vma, vma_flags_t flagmask)
{
	return vma_flags_test_single_mask(&vma->flags, flagmask);
}

/**
 * vma_set_flags_mask - 在 VMA 中设置所有指定标志
 * @vma: 虚拟内存区域结构体指针
 * @flags: 要设置的标志集
 *
 * 设计原因：
 * 辅助函数，用于在 VMA 中设置所有 VMA 标志。
 *
 * 注意事项：
 * 必须持有适当的锁，此函数不会为你获取它们。
 *
 * Helper to set all VMA flags in a VMA.
 *
 * Note: appropriate locks must be held, this function does not acquire them for
 * you.
 */
static __always_inline void vma_set_flags_mask(struct vm_area_struct *vma,
		vma_flags_t flags)
{
	vma_flags_set_mask(&vma->flags, flags);
}

/**
 * vma_set_flags - 在 VMA 中设置指定标志（宏）
 *
 * 设计原因：
 * 辅助宏，用于在 VMA 中指定 VMA 标志。
 *
 * 使用示例：
 * vma_set_flags(vma, VMA_IO_BIT, VMA_PFNMAP_BIT, VMA_DONTEXPAND_BIT,
 *               VMA_DONTDUMP_BIT);
 *
 * 注意事项：
 * 必须持有适当的锁，此函数不会为你获取它们。
 *
 * Helper macro for specifying VMA flags in a VMA, e.g.:
 *
 * vma_set_flags(vma, VMA_IO_BIT, VMA_PFNMAP_BIT, VMA_DONTEXPAND_BIT,
 * 		VMA_DONTDUMP_BIT);
 *
 * Note: appropriate locks must be held, this function does not acquire them for
 * you.
 */
#define vma_set_flags(vma, ...) \
	vma_set_flags_mask(vma, mk_vma_flags(__VA_ARGS__))

/**
 * vma_clear_flags_mask - 在 VMA 中清除所有指定标志
 * @vma: 虚拟内存区域结构体指针
 * @flags: 要清除的标志集
 *
 * 设计原因：
 * 辅助函数，用于在 VMA 中清除所有 VMA 标志。
 *
 * Helper to clear all VMA flags in a VMA.
 */
static __always_inline void vma_clear_flags_mask(struct vm_area_struct *vma,
		vma_flags_t flags)
{
	vma_flags_clear_mask(&vma->flags, flags);
}

/**
 * vma_clear_flags - 在 VMA 中清除指定标志（宏）
 *
 * 设计原因：
 * 辅助宏，用于清除 VMA 标志。
 *
 * 使用示例：
 * vma_clear_flags(vma, VMA_IO_BIT, VMA_PFNMAP_BIT, VMA_DONTEXPAND_BIT,
 *                 VMA_DONTDUMP_BIT);
 *
 * Helper macro for clearing VMA flags, e.g.:
 *
 * vma_clear_flags(vma, VMA_IO_BIT, VMA_PFNMAP_BIT, VMA_DONTEXPAND_BIT,
 * 		VMA_DONTDUMP_BIT);
 */
#define vma_clear_flags(vma, ...) \
	vma_clear_flags_mask(vma, mk_vma_flags(__VA_ARGS__))

/**
 * vma_desc_test - 测试 VMA 描述符中的特定标志是否被设置
 * @desc: VMA 描述符指针
 * @bit: 要测试的标志位
 *
 * 设计原因：
 * 测试 VMA 描述符中的特定 VMA 标志是否被设置。
 *
 * 使用示例：
 * if (vma_desc_test(desc, VMA_READ_BIT)) { ... }
 *
 * 返回值：
 * true 表示该标志被设置，false 表示未设置
 *
 * Test whether a specific VMA flag is set in a VMA descriptor, e.g.:
 *
 * if (vma_desc_test(desc, VMA_READ_BIT)) { ... }
 */
static __always_inline bool vma_desc_test(const struct vm_area_desc *desc,
		vma_flag_t bit)
{
	return vma_flags_test(&desc->vma_flags, bit);
}

/**
 * vma_desc_test_any_mask - 测试 VMA 描述符中是否有任何指定标志被设置
 * @desc: VMA 描述符指针
 * @flags: 要测试的标志集
 *
 * 设计原因：
 * 辅助函数，用于测试 VMA 描述符中的任何 VMA 标志。
 *
 * 返回值：
 * true 表示至少有一个标志被设置，false 表示没有
 *
 * Helper to test any VMA flags in a VMA descriptor.
 */
static __always_inline bool vma_desc_test_any_mask(const struct vm_area_desc *desc,
		vma_flags_t flags)
{
	return vma_flags_test_any_mask(&desc->vma_flags, flags);
}

/**
 * vma_desc_test_any - 测试 VMA 描述符中是否有任何指定标志被设置（宏）
 *
 * 设计原因：
 * 辅助宏，用于测试 VMA 描述符中是否设置了任何 VMA 标志。
 *
 * 使用示例：
 * if (vma_desc_test_any(desc, VMA_IO_BIT, VMA_PFNMAP_BIT,
 *                       VMA_DONTEXPAND_BIT, VMA_DONTDUMP_BIT)) { ... }
 *
 * Helper macro for testing whether any VMA flags are set in a VMA descriptor,
 * e.g.:
 *
 * if (vma_desc_test_any(desc, VMA_IO_BIT, VMA_PFNMAP_BIT,
 *		VMA_DONTEXPAND_BIT, VMA_DONTDUMP_BIT)) { ... }
 */
#define vma_desc_test_any(desc, ...) \
	vma_desc_test_any_mask(desc, mk_vma_flags(__VA_ARGS__))

/**
 * vma_desc_test_all_mask - 测试 VMA 描述符中是否所有指定标志都被设置
 * @desc: VMA 描述符指针
 * @flags: 要测试的标志集
 *
 * 设计原因：
 * 辅助函数，用于测试 VMA 描述符中的所有 VMA 标志。
 *
 * 返回值：
 * true 表示所有标志都被设置，false 表示至少有一个未设置
 *
 * Helper to test all VMA flags in a VMA descriptor.
 */
static __always_inline bool vma_desc_test_all_mask(const struct vm_area_desc *desc,
		vma_flags_t flags)
{
	return vma_flags_test_all_mask(&desc->vma_flags, flags);
}

/**
 * vma_desc_test_all - 测试 VMA 描述符中是否所有指定标志都被设置（宏）
 *
 * 设计原因：
 * 辅助宏，用于测试 VMA 描述符中是否设置了所有 VMA 标志。
 *
 * 使用示例：
 * if (vma_desc_test_all(desc, VMA_READ_BIT, VMA_MAYREAD_BIT)) { ... }
 *
 * Helper macro for testing whether ALL VMA flags are set in a VMA descriptor,
 * e.g.:
 *
 * if (vma_desc_test_all(desc, VMA_READ_BIT, VMA_MAYREAD_BIT)) { ... }
 */
#define vma_desc_test_all(desc, ...) \
	vma_desc_test_all_mask(desc, mk_vma_flags(__VA_ARGS__))

/**
 * vma_desc_set_flags_mask - 在 VMA 描述符中设置所有指定标志
 * @desc: VMA 描述符指针
 * @flags: 要设置的标志集
 *
 * 设计原因：
 * 辅助函数，用于在 VMA 描述符中设置所有 VMA 标志。
 *
 * Helper to set all VMA flags in a VMA descriptor.
 */
static __always_inline void vma_desc_set_flags_mask(struct vm_area_desc *desc,
		vma_flags_t flags)
{
	vma_flags_set_mask(&desc->vma_flags, flags);
}

/**
 * vma_desc_set_flags - 在 VMA 描述符中设置指定标志（宏）
 *
 * 设计原因：
 * 辅助宏，用于为描述建议 VMA 的 struct vm_area_desc 对象的输入指针指定 VMA 标志。
 *
 * 使用示例：
 * vma_desc_set_flags(desc, VMA_IO_BIT, VMA_PFNMAP_BIT, VMA_DONTEXPAND_BIT,
 *                    VMA_DONTDUMP_BIT);
 *
 * Helper macro for specifying VMA flags for an input pointer to a struct
 * vm_area_desc object describing a proposed VMA, e.g.:
 *
 * vma_desc_set_flags(desc, VMA_IO_BIT, VMA_PFNMAP_BIT, VMA_DONTEXPAND_BIT,
 * 		VMA_DONTDUMP_BIT);
 */
#define vma_desc_set_flags(desc, ...) \
	vma_desc_set_flags_mask(desc, mk_vma_flags(__VA_ARGS__))

/**
 * vma_desc_clear_flags_mask - 在 VMA 描述符中清除所有指定标志
 * @desc: VMA 描述符指针
 * @flags: 要清除的标志集
 *
 * 设计原因：
 * 辅助函数，用于在 VMA 描述符中清除所有 VMA 标志。
 *
 * Helper to clear all VMA flags in a VMA descriptor.
 */
static __always_inline void vma_desc_clear_flags_mask(struct vm_area_desc *desc,
		vma_flags_t flags)
{
	vma_flags_clear_mask(&desc->vma_flags, flags);
}

/**
 * vma_desc_clear_flags - 在 VMA 描述符中清除指定标志（宏）
 *
 * 设计原因：
 * 辅助宏，用于为描述建议 VMA 的 struct vm_area_desc 对象的输入指针清除 VMA 标志。
 *
 * 使用示例：
 * vma_desc_clear_flags(desc, VMA_IO_BIT, VMA_PFNMAP_BIT, VMA_DONTEXPAND_BIT,
 *                      VMA_DONTDUMP_BIT);
 *
 * Helper macro for clearing VMA flags for an input pointer to a struct
 * vm_area_desc object describing a proposed VMA, e.g.:
 *
 * vma_desc_clear_flags(desc, VMA_IO_BIT, VMA_PFNMAP_BIT, VMA_DONTEXPAND_BIT,
 * 		VMA_DONTDUMP_BIT);
 */
#define vma_desc_clear_flags(desc, ...) \
	vma_desc_clear_flags_mask(desc, mk_vma_flags(__VA_ARGS__))

/**
 * vma_set_anonymous - 将 VMA 设置为匿名映射
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 将 VMA 标记为匿名映射（没有关联的文件）。
 * 通过将 vm_ops 设置为 NULL 来实现。
 *
 * C 语言知识：
 * - 匿名映射：没有后备文件的内存映射，通常用于堆和栈
 * - vm_ops：指向虚拟内存操作函数表的指针，NULL 表示匿名映射
 */
static inline void vma_set_anonymous(struct vm_area_struct *vma)
{
	vma->vm_ops = NULL;
}

/**
 * vma_desc_set_anonymous - 将 VMA 描述符设置为匿名映射
 * @desc: VMA 描述符指针
 *
 * 设计原因：
 * 将 VMA 描述符标记为匿名映射。
 */
static inline void vma_desc_set_anonymous(struct vm_area_desc *desc)
{
	desc->vm_ops = NULL;
}

/**
 * vma_is_anonymous - 判断 VMA 是否为匿名映射
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 检查 VMA 是否为匿名映射（没有关联的文件）。
 *
 * 返回值：
 * true 表示是匿名映射，false 表示是文件映射
 */
static inline bool vma_is_anonymous(struct vm_area_struct *vma)
{
	return !vma->vm_ops;
}

/**
 * vma_is_initial_heap - 判断 VMA 是否为初始堆
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 指示 VMA 是否为给定任务的堆；
 * 对于 /proc/PID/maps，这是主任务的堆。
 *
 * 实现逻辑：
 * 堆位于 start_brk 和 brk 之间，所以如果 VMA 的起始地址小于 brk，
 * 且结束地址大于 start_brk，则该 VMA 是堆的一部分。
 *
 * C 语言知识：
 * - brk: 当前堆的结束地址
 * - start_brk: 堆的起始地址
 *
 * 返回值：
 * true 表示是初始堆，false 表示不是
 *
 * Indicate if the VMA is a heap for the given task; for
 * /proc/PID/maps that is the heap of the main task.
 */
static inline bool vma_is_initial_heap(const struct vm_area_struct *vma)
{
	return vma->vm_start < vma->vm_mm->brk &&
		vma->vm_end > vma->vm_mm->start_brk;
}

/**
 * vma_is_initial_stack - 判断 VMA 是否为初始栈
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 指示 VMA 是否为给定任务的栈；
 * 对于 /proc/PID/maps，这是主任务的栈。
 *
 * 注意事项：
 * 我们不会尝试猜测给定线程认为什么是它的"栈"。
 * 对于用 Go 等语言编写的程序，这甚至没有明确定义。
 *
 * 实现逻辑：
 * 检查 VMA 是否包含 start_stack（栈的起始地址）。
 *
 * 返回值：
 * true 表示是初始栈，false 表示不是
 *
 * Indicate if the VMA is a stack for the given task; for
 * /proc/PID/maps that is the stack of the main task.
 */
static inline bool vma_is_initial_stack(const struct vm_area_struct *vma)
{
	/*
	 * We make no effort to guess what a given thread considers to be
	 * its "stack".  It's not even well-defined for programs written
	 * languages like Go.
	 */
	return vma->vm_start <= vma->vm_mm->start_stack &&
		vma->vm_end >= vma->vm_mm->start_stack;
}

/**
 * vma_is_temporary_stack - 判断 VMA 是否为临时栈
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 检查 VMA 是否为临时栈（尚未完全设置的栈）。
 *
 * 实现逻辑：
 * 1. 首先检查是否有 VM_GROWSDOWN 或 VM_GROWSUP 标志（可增长的栈）
 * 2. 如果没有这些标志，肯定不是临时栈
 * 3. 如果有这些标志，检查是否设置了 VM_STACK_INCOMPLETE_SETUP 标志
 *
 * 返回值：
 * true 表示是临时栈，false 表示不是
 */
static inline bool vma_is_temporary_stack(const struct vm_area_struct *vma)
{
	int maybe_stack = vma->vm_flags & (VM_GROWSDOWN | VM_GROWSUP);

	if (!maybe_stack)
		return false;

	if ((vma->vm_flags & VM_STACK_INCOMPLETE_SETUP) ==
						VM_STACK_INCOMPLETE_SETUP)
		return true;

	return false;
}

/**
 * vma_is_foreign - 判断 VMA 是否属于外部进程
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 检查 VMA 是否属于当前进程之外的另一个进程。
 *
 * 实现逻辑：
 * 1. 如果当前进程没有 mm 结构体（可能是内核线程），则 VMA 是外部的
 * 2. 如果当前进程的 mm 与 VMA 的 mm 不同，则 VMA 是外部的
 *
 * 使用场景：
 * 在访问其他进程的内存时（如调试、进程监控等）需要判断。
 *
 * 返回值：
 * true 表示是外部进程的 VMA，false 表示属于当前进程
 */
static inline bool vma_is_foreign(const struct vm_area_struct *vma)
{
	if (!current->mm)
		return true;

	if (current->mm != vma->vm_mm)
		return true;

	return false;
}

/**
 * vma_is_accessible - 判断 VMA 是否可访问
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 检查 VMA 是否具有任何访问权限（读/写/执行）。
 *
 * C 语言知识：
 * - VM_ACCESS_FLAGS: 访问权限标志的组合（VM_READ | VM_WRITE | VM_EXEC）
 *
 * 返回值：
 * true 表示 VMA 可访问，false 表示不可访问
 */
static inline bool vma_is_accessible(const struct vm_area_struct *vma)
{
	return vma->vm_flags & VM_ACCESS_FLAGS;
}

/**
 * is_shared_maywrite - 判断标志是否为共享且可写
 * @flags: VMA 标志指针
 *
 * 设计原因：
 * 检查 VMA 标志是否同时设置了共享和可写标志。
 *
 * 返回值：
 * true 表示是共享且可写的，false 表示不是
 */
static inline bool is_shared_maywrite(const vma_flags_t *flags)
{
	return vma_flags_test_all(flags, VMA_SHARED_BIT, VMA_MAYWRITE_BIT);
}

/**
 * vma_is_shared_maywrite - 判断 VMA 是否为共享且可写
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 检查 VMA 是否同时为共享映射且可写。
 *
 * 使用场景：
 * 共享可写映射用于进程间共享内存，修改会影响其他进程。
 *
 * 返回值：
 * true 表示是共享且可写的，false 表示不是
 */
static inline bool vma_is_shared_maywrite(const struct vm_area_struct *vma)
{
	return is_shared_maywrite(&vma->flags);
}

/**
 * vma_kernel_pagesize - 获取此 VMA 的默认页面大小粒度
 * @vma: 用户映射
 *
 * 设计原因：
 * 内核页面大小指定了可以执行 VMA 修改的粒度。
 * 此 VMA 中的 Folios 将对齐到此函数返回的字节数，并且至少为该大小。
 *
 * 注意事项：
 * 默认内核页面大小不受透明大页（Transparent Huge Pages）生效的影响。
 *
 * 实现逻辑：
 * 1. 如果 VMA 有操作表且定义了 pagesize 回调，则调用它
 * 2. 否则返回系统默认的 PAGE_SIZE
 *
 * C 语言知识：
 * - unlikely: 编译器优化提示，表示条件不太可能为真
 * - PAGE_SIZE: 系统页面大小，通常为 4KB
 *
 * 返回值：
 * 此 VMA 的默认页面大小粒度
 *
 * vma_kernel_pagesize - Default page size granularity for this VMA.
 * @vma: The user mapping.
 *
 * The kernel page size specifies in which granularity VMA modifications
 * can be performed. Folios in this VMA will be aligned to, and at least
 * the size of the number of bytes returned by this function.
 *
 * The default kernel page size is not affected by Transparent Huge Pages
 * being in effect.
 *
 * Return: The default page size granularity for this VMA.
 */
static inline unsigned long vma_kernel_pagesize(struct vm_area_struct *vma)
{
	if (unlikely(vma->vm_ops && vma->vm_ops->pagesize))
		return vma->vm_ops->pagesize(vma);
	return PAGE_SIZE;
}

/**
 * vma_mmu_pagesize - 获取此 VMA 的 MMU 页面大小
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 获取 MMU（内存管理单元）为此 VMA 使用的页面大小。
 * 这可能与内核页面大小不同，特别是在使用大页时。
 *
 * 返回值：
 * MMU 页面大小
 */
unsigned long vma_mmu_pagesize(struct vm_area_struct *vma);

/**
 * vma_find - 在迭代器中查找 VMA
 * @vmi: VMA 迭代器
 * @max: 最大地址
 *
 * 设计原因：
 * 在 VMA 迭代器中查找地址范围内的 VMA。
 *
 * 实现逻辑：
 * 使用 maple tree（枫树）的 mas_find() 函数在 max-1 之前查找。
 *
 * C 语言知识：
 * - mas_find: maple tree 查找函数
 * - max - 1: 因为范围是闭区间 [start, max-1]
 *
 * 返回值：
 * 找到的 VMA 指针，如果没有则为 NULL
 */
static inline
struct vm_area_struct *vma_find(struct vma_iterator *vmi, unsigned long max)
{
	return mas_find(&vmi->mas, max - 1);
}

/**
 * vma_next - 获取迭代器中的下一个 VMA
 * @vmi: VMA 迭代器
 *
 * 设计原因：
 * 获取 VMA 迭代器中的下一个 VMA。
 *
 * 注意事项：
 * 使用 mas_find() 在迭代器启动时获取第一个 VMA。
 * 调用 mas_next() 可能会跳过第一个条目。
 *
 * C 语言知识：
 * - ULONG_MAX: 无符号长整型的最大值，表示搜索到地址空间末尾
 *
 * 返回值：
 * 下一个 VMA 指针，如果没有则为 NULL
 */
static inline struct vm_area_struct *vma_next(struct vma_iterator *vmi)
{
	/*
	 * Uses mas_find() to get the first VMA when the iterator starts.
	 * Calling mas_next() could skip the first entry.
	 */
	return mas_find(&vmi->mas, ULONG_MAX);
}

/**
 * vma_iter_next_range - 获取迭代器中的下一个范围的 VMA
 * @vmi: VMA 迭代器
 *
 * 设计原因：
 * 获取 VMA 迭代器中下一个范围的 VMA。
 *
 * 实现逻辑：
 * 使用 mas_next_range() 查找下一个范围内的 VMA。
 *
 * 返回值：
 * 下一个范围的 VMA 指针，如果没有则为 NULL
 */
static inline
struct vm_area_struct *vma_iter_next_range(struct vma_iterator *vmi)
{
	return mas_next_range(&vmi->mas, ULONG_MAX);
}


/**
 * vma_prev - 获取迭代器中的前一个 VMA
 * @vmi: VMA 迭代器
 *
 * 设计原因：
 * 获取 VMA 迭代器中的前一个 VMA，用于反向遍历。
 *
 * C 语言知识：
 * - mas_prev: maple tree 反向查找函数
 * - 0: 从地址 0 开始向前查找
 *
 * 返回值：
 * 前一个 VMA 指针，如果没有则为 NULL
 */
static inline struct vm_area_struct *vma_prev(struct vma_iterator *vmi)
{
	return mas_prev(&vmi->mas, 0);
}

/**
 * vma_iter_clear_gfp - 清除 VMA 迭代器范围内的条目
 * @vmi: VMA 迭代器
 * @start: 起始地址
 * @end: 结束地址
 * @gfp: 内存分配标志
 *
 * 设计原因：
 * 在指定的地址范围内清除 VMA 条目（存储 NULL）。
 *
 * 实现逻辑：
 * 1. 设置 maple tree 的范围为 [start, end-1]
 * 2. 使用指定的 gfp 标志存储 NULL
 * 3. 如果出错则返回 -ENOMEM
 *
 * C 语言知识：
 * - gfp_t: 内核内存分配标志类型
 * - -ENOMEM: 内存不足错误码
 *
 * 返回值：
 * 成功返回 0，内存不足返回 -ENOMEM
 */
static inline int vma_iter_clear_gfp(struct vma_iterator *vmi,
			unsigned long start, unsigned long end, gfp_t gfp)
{
	__mas_set_range(&vmi->mas, start, end - 1);
	mas_store_gfp(&vmi->mas, NULL, gfp);
	if (unlikely(mas_is_err(&vmi->mas)))
		return -ENOMEM;

	return 0;
}

/**
 * vma_iter_free - 释放迭代器中未使用的预分配内存
 * @vmi: VMA 迭代器
 *
 * 设计原因：
 * 释放 VMA 迭代器中任何未使用的预分配资源。
 *
 * C 语言知识：
 * - mas_destroy: 销毁 maple tree 状态并释放资源
 *
 * Free any unused preallocations
 */
static inline void vma_iter_free(struct vma_iterator *vmi)
{
	mas_destroy(&vmi->mas);
}

/**
 * vma_iter_bulk_store - 批量存储 VMA 到迭代器
 * @vmi: VMA 迭代器
 * @vma: 要存储的 VMA
 *
 * 设计原因：
 * 将 VMA 批量存储到迭代器管理的 maple tree 中。
 *
 * 实现逻辑：
 * 1. 设置 maple tree 的索引范围为 VMA 的地址范围 [vm_start, vm_end-1]
 * 2. 存储 VMA 到 maple tree
 * 3. 如果成功，标记 VMA 为已附加状态
 *
 * 返回值：
 * 成功返回 0，内存不足返回 -ENOMEM
 */
static inline int vma_iter_bulk_store(struct vma_iterator *vmi,
				      struct vm_area_struct *vma)
{
	vmi->mas.index = vma->vm_start;
	vmi->mas.last = vma->vm_end - 1;
	mas_store(&vmi->mas, vma);
	if (unlikely(mas_is_err(&vmi->mas)))
		return -ENOMEM;

	vma_mark_attached(vma);
	return 0;
}

/**
 * vma_iter_invalidate - 使 VMA 迭代器暂停
 * @vmi: VMA 迭代器
 *
 * 设计原因：
 * 暂停 VMA 迭代器，使其当前状态失效。
 *
 * C 语言知识：
 * - mas_pause: 暂停 maple tree 状态，下次访问将重新查找
 */
static inline void vma_iter_invalidate(struct vma_iterator *vmi)
{
	mas_pause(&vmi->mas);
}

/**
 * vma_iter_set - 设置 VMA 迭代器的位置
 * @vmi: VMA 迭代器
 * @addr: 要设置的地址
 *
 * 设计原因：
 * 将 VMA 迭代器的位置设置到指定的地址。
 *
 * C 语言知识：
 * - mas_set: 设置 maple tree 状态的索引位置
 */
static inline void vma_iter_set(struct vma_iterator *vmi, unsigned long addr)
{
	mas_set(&vmi->mas, addr);
}

/**
 * for_each_vma - 遍历所有 VMA 的宏
 * @__vmi: VMA 迭代器
 * @__vma: VMA 指针变量（在循环中被赋值）
 *
 * 设计原因：
 * 提供便捷的方式遍历所有 VMA。
 *
 * 使用示例：
 * struct vma_iterator vmi;
 * struct vm_area_struct *vma;
 * for_each_vma(vmi, vma) {
 *     // 处理每个 vma
 * }
 */
#define for_each_vma(__vmi, __vma)					\
	while (((__vma) = vma_next(&(__vmi))) != NULL)

/**
 * for_each_vma_range - 遍历指定范围内的 VMA 的宏
 * @__vmi: VMA 迭代器
 * @__vma: VMA 指针变量（在循环中被赋值）
 * @__end: 结束地址
 *
 * 设计原因：
 * MM 代码喜欢使用独占的结束地址（不包括 end 本身）。
 *
 * 使用示例：
 * for_each_vma_range(vmi, vma, end_addr) {
 *     // 处理范围内的每个 vma
 * }
 *
 * The MM code likes to work with exclusive end addresses
 */
#define for_each_vma_range(__vmi, __vma, __end)				\
	while (((__vma) = vma_find(&(__vmi), (__end))) != NULL)

#ifdef CONFIG_SHMEM
/**
 * vma_is_shmem - 判断 VMA 是否为共享内存
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * vma_is_shmem 不是内联函数，因为它仅在 userfault 的慢速路径中使用。
 *
 * 返回值：
 * true 表示是共享内存，false 表示不是
 *
 * The vma_is_shmem is not inline because it is used only by slow
 * paths in userfault.
 */
bool vma_is_shmem(const struct vm_area_struct *vma);
bool vma_is_anon_shmem(const struct vm_area_struct *vma);
#else
static inline bool vma_is_shmem(const struct vm_area_struct *vma) { return false; }
static inline bool vma_is_anon_shmem(const struct vm_area_struct *vma) { return false; }
#endif

/**
 * vma_is_stack_for_current - 判断 VMA 是否为当前进程的栈
 * @vma: 虚拟内存区域结构体指针
 *
 * 设计原因：
 * 检查 VMA 是否为当前进程的栈。
 *
 * 返回值：
 * 非零表示是当前进程的栈，0 表示不是
 */
int vma_is_stack_for_current(const struct vm_area_struct *vma);

/**
 * TLB_FLUSH_VMA - 创建 TLB 刷新的 VMA 结构的宏
 * @mm: 内存描述符
 * @flags: VMA 标志
 *
 * 设计原因：
 * flush_tlb_range() 接受一个 vma 而不是 mm，并且可以关注标志。
 * 这个宏创建一个临时的 VMA 结构体用于 TLB 刷新。
 *
 * C 语言知识：
 * - 复合字面量: { .member = value, ... } 创建临时结构体
 *
 * flush_tlb_range() takes a vma, not a mm, and can care about flags
 */
#define TLB_FLUSH_VMA(mm,flags) { .vm_mm = (mm), .vm_flags = (flags) }

struct mmu_gather;
struct inode;

/**
 * prep_compound_page - 准备复合页面
 * @page: 页面指针
 * @order: 页面阶数（2^order 个页面）
 *
 * 设计原因：
 * 准备一个复合页面（由多个连续的物理页面组成的大页）。
 *
 * C 语言知识：
 * - order: 阶数，order=0 是 1 个页面，order=1 是 2 个页面，order=2 是 4 个页面，依此类推
 */
extern void prep_compound_page(struct page *page, unsigned int order);

/**
 * folio_large_order - 获取大 folio 的阶数
 * @folio: folio 指针
 *
 * 设计原因：
 * 获取大 folio（多页面组成的 folio）的阶数。
 *
 * 实现逻辑：
 * 从 folio 的 _flags_1 字段中提取低 8 位作为阶数。
 *
 * C 语言知识：
 * - & 0xff: 按位与操作，提取低 8 位（0-255）
 *
 * 返回值：
 * folio 的阶数（0-255）
 */
static inline unsigned int folio_large_order(const struct folio *folio)
{
	return folio->_flags_1 & 0xff;
}

#ifdef NR_PAGES_IN_LARGE_FOLIO
static inline unsigned long folio_large_nr_pages(const struct folio *folio)
{
	return folio->_nr_pages;
}
#else
static inline unsigned long folio_large_nr_pages(const struct folio *folio)
{
	return 1L << folio_large_order(folio);
}
#endif

/**
 * compound_order() - 获取复合页的阶数（order）
 * @page: 要查询的页面
 *
 * 【设计考虑】为什么需要 compound_order()？
 * - 复合页（compound page）由多个连续的物理页组成，形成一个逻辑上的大页
 * - order 值决定了复合页的大小：2^order 个基础页面
 * - 这个函数提供了快速查询复合页大小的方法
 *
 * 【重要注意事项】
 * - 可以在不持有页面引用的情况下调用此函数
 * - 这意味着像 page_folio() 这样需要稳定状态的辅助函数无法使用
 * - 调用者必须准备好处理"野"返回值（wild return values）
 * - 例如：PG_head 标志可能在 order 初始化之前就被设置
 * - 或者传入的可能是尾页（tail page）而不是头页
 * - 参见 compaction.c 中的良好示例
 *
 * 【C语言概念】const 指针参数
 * - const struct page *page 表示不会修改 page 指向的内容
 * - 这是一种安全保证，告诉编译器和调用者此函数只读取页面信息
 *
 * 返回值：
 * - 对于复合页：返回其阶数（order），表示包含 2^order 个基础页
 * - 对于普通页：返回 0（表示单页，2^0 = 1）
 * - 不稳定状态下可能返回未初始化或不一致的值
 *
 * compound_order() can be called without holding a reference, which means
 * that niceties like page_folio() don't work.  These callers should be
 * prepared to handle wild return values.  For example, PG_head may be
 * set before the order is initialised, or this may be a tail page.
 * See compaction.c for some good examples.
 */
static inline unsigned int compound_order(const struct page *page)
{
	/* 将 page 强制转换为 folio 指针，因为复合页的头页就是 folio
	 * 【C语言概念】类型转换（cast）
	 * - (struct folio *)page 是显式类型转换
	 * - 这里假设传入的 page 可能是复合页的头页
	 * Cast page to folio pointer, as the head page of compound page is the folio */
	const struct folio *folio = (struct folio *)page;

	/* 检查 PG_head 标志位，判断是否为复合页的头页
	 * 【设计原理】为什么先检查 PG_head？
	 * - PG_head 标志标识复合页的头页
	 * - 只有头页才存储 order 信息
	 * - 如果不是头页（可能是尾页或普通页），直接返回 0
	 * Check PG_head flag to determine if this is the head page of a compound page */
	if (!test_bit(PG_head, &folio->flags.f))
		return 0;  // 不是复合页头页，返回 0（表示单页） / Not a compound head, return 0 (single page)

	/* 是复合页头页，返回实际的阶数
	 * Return the actual order for compound page head */
	return folio_large_order(folio);
}

/**
 * folio_order - folio 的分配阶数
 * @folio: 要查询的 folio
 *
 * 【folio 的组成】
 * - 一个 folio 由 2^order 个页面组成
 * - order 是分配阶数，决定了 folio 的大小
 * - 参见 get_order() 函数了解 order 的定义
 *
 * 【设计考虑】folio_order() vs compound_order()
 * - folio_order() 是类型安全的版本，接受 folio 指针
 * - compound_order() 接受 page 指针，可能不稳定
 * - folio_order() 首先检查 folio 是否为大页，然后才查询 order
 *
 * 【实现逻辑】
 * - 如果不是大页（large folio），返回 0（表示单页，2^0 = 1）
 * - 如果是大页，调用 folio_large_order() 获取实际阶数
 *
 * 返回值：folio 的阶数
 * - 0 表示单页 folio（一个基础页）
 * - n 表示包含 2^n 个基础页的大页 folio
 *
 * folio_order - The allocation order of a folio.
 * @folio: The folio.
 *
 * A folio is composed of 2^order pages.  See get_order() for the definition
 * of order.
 *
 * Return: The order of the folio.
 */
static inline unsigned int folio_order(const struct folio *folio)
{
	/* 如果不是大页 folio，返回 0
	 * 【设计原理】为什么先测试 large？
	 * - 单页 folio 的 order 总是 0
	 * - 避免对单页 folio 调用 folio_large_order()（可能未定义或无效）
	 * If not a large folio, return 0 */
	if (!folio_test_large(folio))
		return 0;

	/* 是大页 folio，返回实际的阶数
	 * For large folio, return the actual order */
	return folio_large_order(folio);
}

/**
 * folio_reset_order - 重置 folio 的阶数和派生的 _nr_pages
 * @folio: 要重置的 folio
 *
 * 【功能说明】
 * - 将 order 和派生的 _nr_pages 重置为 0
 * - 仅在拆分大页 folio 的过程中使用
 *
 * 【使用场景】大页拆分
 * - 当需要将一个大页 folio 拆分为多个小页时
 * - 必须先重置原大页的 order 信息
 * - 然后才能重新设置拆分后各个页面的属性
 *
 * 【重要注意事项】
 * - 此函数只能在拆分大页 folio 的过程中调用
 * - 在其他情况下调用可能导致内存管理状态不一致
 * - 调用此函数后，folio 的页数会被重置为单页（2^0 = 1）
 *
 * 【设计原理】为什么需要重置？
 * - 大页拆分是一个多步骤的过程
 * - 需要先清除原有的大页属性
 * - 然后逐个设置拆分后的小页属性
 * - 保证状态转换的原子性和一致性
 *
 * Reset the order and derived _nr_pages to 0. Must only be used in the
 * process of splitting large folios.
 */
static inline void folio_reset_order(struct folio *folio)
{
	/* 检查是否为大页 folio，如果不是则发出警告并返回
	 * 【安全检查】WARN_ON_ONCE 宏
	 * - 如果条件为真（不是大页），会打印一次警告信息
	 * - ONCE 表示只警告一次，避免日志泛滥
	 * - 这是防御性编程，防止误用此函数
	 * Check if it's a large folio, warn and return if not */
	if (WARN_ON_ONCE(!folio_test_large(folio)))
		return;

	/* 清除 _flags_1 字段的低 8 位（0xff），这些位存储 order 信息
	 * 【位操作详解】
	 * - 0xffUL 是无符号长整型的 0xFF（二进制 11111111）
	 * - ~0xffUL 取反得到高位全为 1，低 8 位全为 0 的掩码
	 * - &= 操作保留高位，清零低 8 位
	 * - 低 8 位用于存储 order 值（最大支持 255，即 2^255 个页）
	 * Clear the lower 8 bits of _flags_1 which store order information */
	folio->_flags_1 &= ~0xffUL;

#ifdef NR_PAGES_IN_LARGE_FOLIO
	/* 如果定义了 NR_PAGES_IN_LARGE_FOLIO 配置
	 * - 需要显式将 _nr_pages 字段重置为 0
	 * - _nr_pages 缓存了此 folio 包含的页数（2^order）
	 * - 与 order 保持同步
	 * Reset _nr_pages field if NR_PAGES_IN_LARGE_FOLIO is configured */
	folio->_nr_pages = 0;
#endif
}

#include <linux/huge_mm.h>

/*
 * 修改页面使用计数的方法
 * Methods to modify the page usage count.
 *
 * 【页面使用计数包括什么】
 * What counts for a page usage:
 * - cache mapping   (page->mapping)     // 缓存映射（页面的 mapping 字段）
 * - private data    (page->private)     // 私有数据（页面的 private 字段）
 * - page mapped in a task's page tables, each mapping
 *   is counted separately                // 页面映射到任务的页表中，每个映射单独计数
 *
 * 【内核例程的使用模式】
 * Also, many kernel routines increase the page count before a critical
 * routine so they can be sure the page doesn't go away from under them.
 * 此外，许多内核例程在执行关键操作之前会增加页面计数，
 * 以确保页面不会在它们使用时被释放
 *
 * 【设计原理】引用计数
 * - 引用计数是内存管理的核心机制
 * - 当计数降为 0 时，页面可以被释放
 * - 每个使用者必须持有引用，使用完毕后释放引用
 * - 防止"使用后释放"（use-after-free）错误
 */

/*
 * 释放一个引用，如果引用计数降为零则返回 true（页面没有使用者了）
 * Drop a ref, return true if the refcount fell to zero (the page has no users)
 *
 * 【函数功能】
 * - 递减页面的引用计数
 * - 如果递减后计数变为 0，返回 true
 * - 如果计数仍大于 0，返回 false
 *
 * 【返回值含义】
 * - true: 引用计数降为 0，调用者应该释放页面
 * - false: 还有其他使用者，页面不能释放
 */
static inline int put_page_testzero(struct page *page)
{
	/* 调试检查：确保引用计数不为 0
	 * 【防御性编程】VM_BUG_ON_PAGE 宏
	 * - 在调试模式下检查条件
	 * - 如果引用计数已经为 0，说明有严重的错误（可能重复释放）
	 * - 会触发内核 bug 报告
	 * Debug check: ensure refcount is not already zero */
	VM_BUG_ON_PAGE(page_ref_count(page) == 0, page);

	/* 原子递减引用计数并测试是否为 0
	 * 【原子操作】page_ref_dec_and_test
	 * - 原子地递减计数器
	 * - 如果递减后为 0 返回 true，否则返回 false
	 * - 保证多核环境下的线程安全
	 * Atomically decrement refcount and test if zero */
	return page_ref_dec_and_test(page);
}

/**
 * folio_put_testzero - 释放 folio 的一个引用，测试是否降为零
 * @folio: 要操作的 folio
 *
 * 【功能说明】
 * - folio 版本的 put_page_testzero
 * - 递减 folio 的引用计数
 * - 如果递减后计数为 0，返回 true
 *
 * 【实现方式】
 * - 直接调用 put_page_testzero 操作 folio 的第一个页面
 * - folio 的引用计数存储在头页（&folio->page）中
 *
 * 返回值：
 * - 非零值（true）：引用计数降为 0，可以释放 folio
 * - 0（false）：仍有其他引用，不能释放
 */
static inline int folio_put_testzero(struct folio *folio)
{
	/* 通过 folio 的头页调用 put_page_testzero
	 * Call put_page_testzero on the folio's head page */
	return put_page_testzero(&folio->page);
}

/*
 * 尝试获取引用，除非页面的引用计数为零，如果是零则返回 false
 * Try to grab a ref unless the page has a refcount of zero, return false if
 * that is the case.
 *
 * 【使用场景】
 * - 在不确定页面是否仍然有效时尝试获取引用
 * - 如果页面正在被释放（引用计数为 0），则获取失败
 * - 如果成功获取，调用者持有一个引用，可以安全使用页面
 *
 * 【特殊约束】MMU 关闭时可用
 * This can be called when MMU is off so it must not access
 * any of the virtual mappings.
 * 此函数可以在 MMU（内存管理单元）关闭时调用，
 * 因此它不能访问任何虚拟映射
 *
 * 【设计原理】条件引用获取
 * - 与直接增加引用计数不同，这是一个"测试并设置"操作
 * - 原子地检查计数是否为 0，如果不为 0 则增加
 * - 防止对即将释放的页面获取引用
 * - 这是一种常见的引用计数模式，用于并发环境
 *
 * 返回值：
 * - true: 成功获取引用，页面可用
 * - false: 页面引用计数为 0，获取失败
 */
static inline bool get_page_unless_zero(struct page *page)
{
	/* 原子地尝试将引用计数加 1，除非当前为 0
	 * 【原子操作】page_ref_add_unless_zero
	 * - 如果当前引用计数不为 0，则加 1 并返回 true
	 * - 如果当前引用计数为 0，则不修改并返回 false
	 * - 整个"检查-修改"过程是原子的，保证线程安全
	 * Atomically try to add 1 to refcount, unless it's zero */
	return page_ref_add_unless_zero(page, 1);
}

/**
 * folio_get_nontail_page - 获取非尾页的 folio 引用
 * @page: 要操作的页面
 *
 * 【函数功能】
 * - 尝试获取页面的引用
 * - 如果成功，将页面作为 folio 返回
 * - 如果失败（引用计数为 0），返回 NULL
 *
 * 【命名含义】nontail（非尾页）
 * - 此函数假设传入的 page 不是复合页的尾页
 * - 只有头页或单页才能安全地转换为 folio
 * - 尾页转换为 folio 是无效的
 *
 * 【使用场景】
 * - 当需要将 page 作为 folio 使用时
 * - 在不确定页面是否仍然存活时
 * - 通过条件引用获取保证安全性
 *
 * 【实现逻辑】
 * 1. 尝试获取页面引用（除非计数为 0）
 * 2. 如果失败，返回 NULL
 * 3. 如果成功，将 page 强制转换为 folio 指针并返回
 *
 * 返回值：
 * - 成功：返回 folio 指针（持有引用）
 * - 失败：返回 NULL（页面引用计数为 0 或即将释放）
 */
static inline struct folio *folio_get_nontail_page(struct page *page)
{
	/* 尝试获取页面引用，如果引用计数为 0 则失败
	 * 【C语言概念】unlikely 宏
	 * - 告诉编译器这个条件不太可能发生
	 * - 编译器可以优化分支预测，提高性能
	 * - 大多数情况下页面是有效的，失败是罕见的
	 * Try to get page reference, fail if refcount is zero */
	if (unlikely(!get_page_unless_zero(page)))
		return NULL;  // 获取引用失败，返回 NULL / Failed to get reference, return NULL

	/* 成功获取引用，将 page 转换为 folio 并返回
	 * 【类型转换】(struct folio *)page
	 * - page 指针和 folio 指针在内存布局上兼容
	 * - 对于非尾页，这种转换是安全的
	 * - 调用者现在持有此 folio 的一个引用
	 * Successfully got reference, cast to folio and return */
	return (struct folio *)page;
}

/**
 * page_is_ram - 检查物理页帧号（PFN）是否对应 RAM
 * @pfn: 物理页帧号（Physical Frame Number）
 *
 * 【功能说明】
 * - 判断给定的物理页帧号是否对应实际的 RAM（随机访问内存）
 * - 某些物理地址范围可能映射到设备内存、保留区域或其他非 RAM 资源
 *
 * 【C语言概念】extern 声明
 * - extern 表示此函数在其他编译单元中定义
 * - 这是一个函数声明，不是定义
 * - 实际实现在内核的其他源文件中
 *
 * 返回值：
 * - 非零值：该 PFN 对应 RAM
 * - 0：该 PFN 不对应 RAM（可能是设备内存或保留区域）
 */
extern int page_is_ram(unsigned long pfn);

/**
 * 区域相交关系的枚举值
 * Enumeration for region intersection relationships
 *
 * 【使用场景】
 * - 用于描述两个内存区域或资源区域之间的关系
 * - region_intersects() 函数的返回值类型
 */
enum {
	REGION_INTERSECTS,  // 区域相交（有重叠部分） / Regions intersect (overlap)
	REGION_DISJOINT,    // 区域不相交（完全分离） / Regions are disjoint (completely separate)
	REGION_MIXED,       // 混合情况（部分相交、部分不相交） / Mixed case (partially intersects)
};

/**
 * region_intersects - 检查内存区域是否与指定类型的资源相交
 * @offset: 要检查的区域起始偏移量
 * @size: 要检查的区域大小
 * @flags: 资源标志位（例如 IORESOURCE_MEM, IORESOURCE_IO 等）
 * @desc: 资源描述符（用于进一步识别资源类型）
 *
 * 【功能说明】
 * - 检查指定的内存区域 [offset, offset+size) 是否与系统中某类资源重叠
 * - 用于验证内存区域是否可以安全使用
 * - 避免访问保留的或正在使用的资源区域
 *
 * 【参数说明】
 * - offset: 区域的起始物理地址或偏移
 * - size: 区域的字节大小
 * - flags: 要匹配的资源标志（如内存、I/O 端口等）
 * - desc: 资源描述符，用于更精确地匹配特定类型的资源
 *
 * 【使用场景】
 * - 驱动程序在映射物理内存前检查地址范围
 * - 内核在分配资源时验证冲突
 * - 确保不会访问到设备保留的内存区域
 *
 * 返回值：
 * - REGION_INTERSECTS: 区域与指定资源相交
 * - REGION_DISJOINT: 区域与指定资源不相交
 * - REGION_MIXED: 混合情况（部分相交）
 */
int region_intersects(resource_size_t offset, size_t size, unsigned long flags,
		      unsigned long desc);

/* 对虚拟映射页面的支持
 * Support for virtually mapped pages
 *
 * 【虚拟映射页面】vmalloc
 * - vmalloc() 分配的内存在虚拟地址空间中是连续的
 * - 但在物理内存中可能是不连续的（由多个分散的物理页组成）
 * - 与 kmalloc() 不同，kmalloc() 分配的是物理上连续的内存
 * - vmalloc 常用于分配大块内存，因为不需要连续的物理内存
 */

/**
 * vmalloc_to_page - 将 vmalloc 地址转换为对应的页面结构
 * @addr: vmalloc 分配的虚拟地址
 *
 * 【功能说明】
 * - 给定一个 vmalloc 区域中的虚拟地址，返回该地址对应的 struct page
 * - 用于访问 vmalloc 内存的底层物理页面
 *
 * 【使用场景】
 * - 需要获取 vmalloc 内存的页面描述符时
 * - 在进行 DMA 操作或其他需要物理页面信息的场景
 *
 * 返回值：指向对应页面的 struct page 指针
 */
struct page *vmalloc_to_page(const void *addr);

/**
 * vmalloc_to_pfn - 将 vmalloc 地址转换为物理页帧号
 * @addr: vmalloc 分配的虚拟地址
 *
 * 【功能说明】
 * - 给定一个 vmalloc 区域中的虚拟地址，返回该地址对应的物理页帧号（PFN）
 * - PFN 是物理页面的唯一标识符
 *
 * 返回值：对应的物理页帧号（unsigned long）
 */
unsigned long vmalloc_to_pfn(const void *addr);

/*
 * 判断地址是否在 vmalloc 范围内
 * Determine if an address is within the vmalloc range
 *
 * 【配置差异】CONFIG_MMU
 * On nommu, vmalloc/vfree wrap through kmalloc/kfree directly, so there
 * is no special casing required.
 * 在无 MMU（nommu）系统上，vmalloc/vfree 直接包装 kmalloc/kfree，
 * 因此不需要特殊处理
 *
 * 【设计原理】为什么有两种实现？
 * - 有 MMU 的系统：vmalloc 有独立的地址空间范围，需要实际检查
 * - 无 MMU 的系统：vmalloc 就是 kmalloc，没有独立范围，总是返回 false
 */
#ifdef CONFIG_MMU
/**
 * is_vmalloc_addr - 检查地址是否为 vmalloc 地址
 * @x: 要检查的地址
 *
 * 【功能说明】
 * - 判断给定的虚拟地址是否位于 vmalloc 区域
 * - 有 MMU 系统才有实际的 vmalloc 区域
 *
 * 返回值：
 * - true: 地址在 vmalloc 范围内
 * - false: 地址不在 vmalloc 范围内
 */
extern bool is_vmalloc_addr(const void *x);

/**
 * is_vmalloc_or_module_addr - 检查地址是否为 vmalloc 或模块地址
 * @x: 要检查的地址
 *
 * 【功能说明】
 * - 判断地址是否位于 vmalloc 区域或内核模块加载区域
 * - 内核模块通常也使用 vmalloc 分配的内存
 *
 * 返回值：
 * - 非零值: 地址在 vmalloc 或模块范围内
 * - 0: 地址不在这些范围内
 */
extern int is_vmalloc_or_module_addr(const void *x);
#else
/* 无 MMU 系统的桩实现（stub implementations）
 * 【设计考虑】为什么返回 false/0？
 * - 无 MMU 系统没有独立的 vmalloc 区域
 * - vmalloc 实际上就是 kmalloc，没有特殊地址范围
 * - 总是返回 false/0，表示"不是特殊的 vmalloc 地址"
 */
static inline bool is_vmalloc_addr(const void *x)
{
	return false;  // 无 MMU 系统没有 vmalloc 区域 / No vmalloc region on nommu
}
static inline int is_vmalloc_or_module_addr(const void *x)
{
	return 0;  // 无 MMU 系统没有 vmalloc 或模块区域 / No vmalloc or module region on nommu
}
#endif

/*
 * 整个 folio 作为单个单元被映射的次数（例如通过 PMD 或 PUD 条目）
 * How many times the entire folio is mapped as a single unit (eg by a
 * PMD or PUD entry).
 *
 * 【重要提示】使用场景限制
 * This is probably not what you want, except for
 * debugging purposes or implementation of other core folio_*() primitives.
 * 这可能不是你想要的，除非用于调试目的或实现其他核心 folio_*() 原语
 *
 * 【设计背景】PMD/PUD 映射
 * - PMD (Page Middle Directory) 和 PUD (Page Upper Directory) 是页表层级
 * - 大页可以通过一个 PMD/PUD 条目映射整个 folio，而不是逐个页面映射
 * - 这种"整体映射"计数与普通的页面级映射计数不同
 * - 通常情况下，你需要的是 folio_mapcount()，而不是这个函数
 */
static inline int folio_entire_mapcount(const struct folio *folio)
{
	/* 确保这是一个大页 folio
	 * Ensure this is a large folio */
	VM_BUG_ON_FOLIO(!folio_test_large(folio), folio);

	/* 【特殊处理】32 位系统上的 order-1 folio
	 * - 在非 64 位系统上，如果 folio 的 order 为 1（2 个页面）
	 * - 返回 0，因为这种情况下不使用 _entire_mapcount
	 * Special case for order-1 folio on 32-bit systems */
	if (!IS_ENABLED(CONFIG_64BIT) && unlikely(folio_large_order(folio) == 1))
		return 0;

	/* 读取整体映射计数并加 1
	 * 【设计原理】为什么加 1？
	 * - _entire_mapcount 从 -1 开始（表示 0 个映射）
	 * - 加 1 后得到实际的映射次数
	 * - 这种设计使得原子操作更高效
	 * Read entire mapcount and add 1 (bias adjustment) */
	return atomic_read(&folio->_entire_mapcount) + 1;
}

/**
 * folio_large_mapcount - 获取大页 folio 的映射计数
 * @folio: 要查询的 folio
 *
 * 【功能说明】
 * - 返回大页 folio 的映射计数
 * - 这是大页特有的映射计数机制
 *
 * 【设计原理】_large_mapcount 的偏移
 * - _large_mapcount 字段从 -1 开始（表示 0 个映射）
 * - 加 1 后得到实际的映射次数
 * - 这种偏移设计使得原子递增/递减操作更高效
 *
 * 【重要注意事项】
 * - VM_WARN_ON_FOLIO 在非大页时会发出警告
 * - 这个函数应该只用于大页 folio
 *
 * 返回值：大页 folio 的映射计数
 */
static inline int folio_large_mapcount(const struct folio *folio)
{
	/* 警告检查：确保这是一个大页 folio
	 * Warn if not a large folio */
	VM_WARN_ON_FOLIO(!folio_test_large(folio), folio);

	/* 读取大页映射计数并加 1（偏移调整）
	 * Read large mapcount and add 1 (bias adjustment) */
	return atomic_read(&folio->_large_mapcount) + 1;
}

/**
 * folio_mapcount() - folio 的映射次数
 * @folio: 要查询的 folio
 *
 * 【核心概念】映射计数（mapcount）
 * The folio mapcount corresponds to the number of present user page table
 * entries that reference any part of a folio. Each such present user page
 * table entry must be paired with exactly on folio reference.
 * folio 映射计数对应于引用 folio 任何部分的用户页表条目数量。
 * 每个这样的用户页表条目必须与恰好一个 folio 引用配对。
 *
 * 【普通 folio 的计数规则】
 * For ordindary folios, each user page table entry (PTE/PMD/PUD/...) counts
 * exactly once.
 * 对于普通 folio，每个用户页表条目（PTE/PMD/PUD/...）计数恰好一次。
 *
 * 【大页（hugetlb）folio 的计数规则】
 * For hugetlb folios, each abstracted "hugetlb" user page table entry that
 * references the entire folio counts exactly once, even when such special
 * page table entries are comprised of multiple ordinary page table entries.
 * 对于 hugetlb folio，每个抽象的 "hugetlb" 用户页表条目引用整个 folio 时计数恰好一次，
 * 即使这种特殊页表条目由多个普通页表条目组成。
 *
 * 【特殊情况】无法映射到用户空间的页面
 * Will report 0 for pages which cannot be mapped into userspace, such as
 * slab, page tables and similar.
 * 对于无法映射到用户空间的页面（如 slab、页表等），将报告 0。
 *
 * 返回值：此 folio 被映射的次数
 * Return: The number of times this folio is mapped.
 */
static inline int folio_mapcount(const struct folio *folio)
{
	int mapcount;

	/* 【快速路径】处理单页 folio
	 * 【C语言概念】likely 宏
	 * - 告诉编译器这个条件很可能为真
	 * - 优化分支预测，提高性能
	 * - 大多数 folio 是单页，这是常见情况
	 * Fast path for single-page folio */
	if (likely(!folio_test_large(folio))) {
		/* 读取单页的映射计数并加 1（偏移调整）
		 * Read single-page mapcount and add 1 */
		mapcount = atomic_read(&folio->_mapcount) + 1;

		/* 检查是否为特殊类型的映射计数
		 * 【设计原理】page_mapcount_is_type
		 * - 某些特殊页面（如 slab、页表）使用特殊的 mapcount 值标记
		 * - 这些页面不能映射到用户空间，应该报告 0
		 * Check if this is a special type (slab, page table, etc.) */
		if (page_mapcount_is_type(mapcount))
			mapcount = 0;
		return mapcount;
	}

	/* 【慢速路径】处理大页 folio
	 * 调用专门的大页映射计数函数
	 * Slow path for large folio */
	return folio_large_mapcount(folio);
}

/**
 * folio_mapped - 此 folio 是否映射到用户空间？
 * @folio: 要查询的 folio
 *
 * 【功能说明】
 * - 检查 folio 中的任何页面是否被用户页表引用
 * - 只要有至少一个用户页表条目引用此 folio，就返回 true
 *
 * 【实现方式】
 * - 通过检查 folio_mapcount() >= 1 来判断
 * - 映射计数 >= 1 表示至少有一个用户空间映射
 *
 * 返回值：
 * Return: True if any page in this folio is referenced by user page tables.
 * - true: folio 中的任何页面被用户页表引用
 * - false: folio 没有被任何用户页表引用
 */
static inline bool folio_mapped(const struct folio *folio)
{
	/* 映射计数 >= 1 表示有用户空间映射
	 * Mapcount >= 1 means there is at least one user mapping */
	return folio_mapcount(folio) >= 1;
}

/**
 * virt_to_head_page - 将虚拟地址转换为其头页
 * @x: 虚拟地址
 *
 * 【功能说明】
 * - 给定一个内核虚拟地址，返回对应的头页（head page）
 * - 如果地址对应复合页的尾页，返回该复合页的头页
 * - 如果地址对应单页或头页，直接返回该页
 *
 * 【实现步骤】
 * 1. virt_to_page(x): 虚拟地址 → 对应的 page 结构
 * 2. compound_head(page): 如果是尾页，找到头页；否则返回自身
 *
 * 【使用场景】
 * - 当需要操作页面的元数据时（元数据只存储在头页中）
 * - 当需要确保操作的是完整的复合页时
 *
 * 返回值：指向头页的 struct page 指针
 */
static inline struct page *virt_to_head_page(const void *x)
{
	/* 第一步：虚拟地址转换为页面结构
	 * Step 1: Convert virtual address to page structure */
	struct page *page = virt_to_page(x);

	/* 第二步：获取头页（如果是尾页则返回其头页）
	 * Step 2: Get head page (returns head if this is a tail page) */
	return compound_head(page);
}

/**
 * virt_to_folio - 将虚拟地址转换为 folio
 * @x: 内核虚拟地址
 *
 * 【功能说明】
 * - 给定一个内核虚拟地址，返回对应的 folio
 * - 先将虚拟地址转换为页面，再将页面转换为 folio
 *
 * 【实现步骤】
 * 1. virt_to_page(x): 虚拟地址 → struct page
 * 2. page_folio(page): struct page → struct folio
 *
 * 【设计原理】为什么需要这个函数？
 * - 新的内核代码倾向于使用 folio 而不是 page
 * - folio 是更高级的抽象，可以表示一组页面
 * - 这个函数提供了从虚拟地址直接获取 folio 的便捷方法
 *
 * 返回值：对应的 struct folio 指针
 */
static inline struct folio *virt_to_folio(const void *x)
{
	/* 第一步：虚拟地址转换为页面结构
	 * Step 1: Convert virtual address to page structure */
	struct page *page = virt_to_page(x);

	/* 第二步：页面转换为 folio
	 * Step 2: Convert page to folio */
	return page_folio(page);
}

/**
 * __folio_put - 释放 folio 的引用（内部函数）
 * @folio: 要释放的 folio
 *
 * 【函数命名】双下划线前缀
 * - __folio_put 是内部实现函数
 * - 通常有对应的外部包装函数 folio_put
 * - 双下划线前缀表示这是底层实现，应谨慎使用
 *
 * 【功能说明】
 * - 递减 folio 的引用计数
 * - 如果引用计数降为 0，释放 folio
 * - 这是引用计数管理的核心函数
 *
 * 【C语言概念】extern 声明
 * - 此函数在其他编译单元中定义
 * - 这里只是声明，不是定义
 */
void __folio_put(struct folio *folio);

/**
 * split_page - 拆分复合页为独立的单页
 * @page: 要拆分的复合页的头页
 * @order: 复合页的 order（2^order 个页面）
 *
 * 【功能说明】
 * - 将一个大的复合页拆分为多个独立的单页
 * - 拆分后每个页面都可以独立管理和释放
 *
 * 【使用场景】
 * - 当需要将大页的一部分分配给其他用途时
 * - 内存碎片化管理
 * - 页面迁移或重新分配
 */
void split_page(struct page *page, unsigned int order);

/**
 * folio_copy - 复制 folio 的内容
 * @dst: 目标 folio
 * @src: 源 folio
 *
 * 【功能说明】
 * - 将源 folio 的内容复制到目标 folio
 * - 复制的是页面数据，不是元数据
 */
void folio_copy(struct folio *dst, struct folio *src);

/**
 * folio_mc_copy - 多核优化的 folio 复制
 * @dst: 目标 folio
 * @src: 源 folio
 *
 * 【功能说明】
 * - mc 可能代表 "multi-core" 或 "memory controller"
 * - 针对多核系统优化的 folio 复制函数
 * - 可能利用并行复制或特殊的内存控制器功能
 *
 * 返回值：
 * - 0: 成功
 * - 非零: 失败（错误码）
 */
int folio_mc_copy(struct folio *dst, struct folio *src);

/**
 * nr_free_buffer_pages - 获取可用于缓冲区的空闲页面数
 *
 * 【功能说明】
 * - 返回当前系统中可用于缓冲区分配的空闲页面数量
 * - 用于判断是否有足够的内存用于缓冲区分配
 *
 * 返回值：可用的空闲页面数
 */
unsigned long nr_free_buffer_pages(void);

/* 返回此（可能是复合页的）页面的字节数
 * Returns the number of bytes in this potentially compound page.
 *
 * 【功能说明】
 * - 计算页面的总字节数
 * - 对于单页：返回 PAGE_SIZE（通常是 4KB）
 * - 对于复合页：返回 PAGE_SIZE * 2^order
 */
static inline unsigned long page_size(const struct page *page)
{
	/* 【位移操作】<< compound_order(page)
	 * - PAGE_SIZE << order 等价于 PAGE_SIZE * 2^order
	 * - 例如：PAGE_SIZE=4KB, order=2 → 4KB << 2 = 16KB
	 * - 左移 N 位相当于乘以 2^N
	 * Calculate size as PAGE_SIZE * 2^order */
	return PAGE_SIZE << compound_order(page);
}

/* 返回表示页面字节数所需的位数
 * Returns the number of bits needed for the number of bytes in a page
 *
 * 【功能说明】
 * - 返回页面大小对应的位移量（shift 值）
 * - 用于地址计算和对齐检查
 *
 * 【举例说明】
 * - 单页（4KB）：PAGE_SHIFT = 12（因为 2^12 = 4096）
 * - order=2 复合页（16KB）：12 + 2 = 14（因为 2^14 = 16384）
 */
static inline unsigned int page_shift(struct page *page)
{
	/* PAGE_SHIFT + order 得到页面大小的位移量
	 * PAGE_SHIFT + order gives the shift for page size */
	return PAGE_SHIFT + compound_order(page);
}

/**
 * thp_order - 透明大页的 order 值
 * @page: 透明大页的头页
 *
 * 【概念说明】THP（Transparent Huge Page）
 * - 透明大页是一种自动使用大页的机制
 * - 对应用程序透明，无需特殊的编程接口
 * - 可以减少 TLB（Translation Lookaside Buffer）缺失，提高性能
 *
 * 【功能说明】
 * - 返回透明大页的 order 值
 * - order 表示页面数量：2^order 个基础页面
 *
 * 【安全检查】
 * - VM_BUG_ON_PGFLAGS 确保传入的不是尾页
 * - 只有头页才能查询 order
 *
 * 返回值：透明大页的 order 值（无符号整数）
 */
static inline unsigned int thp_order(struct page *page)
{
	/* 确保不是尾页（只有头页才能查询 order）
	 * Ensure not a tail page (only head page can query order) */
	VM_BUG_ON_PGFLAGS(PageTail(page), page);

	/* 返回复合页的 order
	 * Return compound page order */
	return compound_order(page);
}

/**
 * thp_size - 透明大页的字节大小
 * @page: 透明大页的头页
 *
 * 【功能说明】
 * - 计算透明大页的总字节数
 * - 通过 PAGE_SIZE * 2^order 计算
 *
 * 返回值：此页面中的字节数
 * Return: Number of bytes in this page.
 */
static inline unsigned long thp_size(struct page *page)
{
	/* PAGE_SIZE << thp_order(page) 等价于 PAGE_SIZE * 2^order
	 * Calculate size as PAGE_SIZE * 2^order */
	return PAGE_SIZE << thp_order(page);
}

#ifdef CONFIG_MMU
/*
 * 执行 pte_mkwrite，但仅当 VMA 标记为 VM_WRITE 时
 * Do pte_mkwrite, but only if the vma says VM_WRITE.
 *
 * 【使用场景】服务写访问缺页时
 * We do this when servicing faults for write access.
 * 我们在处理写访问的缺页异常时执行此操作。
 *
 * 【正常情况】总是希望使用 pte_mkwrite
 * In the normal case, do always want pte_mkwrite.
 * 在正常情况下，我们总是希望使用 pte_mkwrite。
 *
 * 【特殊情况】get_user_pages 的写缺页
 * But get_user_pages can cause write faults for mappings
 * that do not have writing enabled, when used by access_process_vm.
 * 但是 get_user_pages 可能会对没有启用写入的映射引起写缺页，
 * 当它被 access_process_vm 使用时。
 *
 * 【设计原理】为什么需要检查？
 * - 写缺页不一定意味着需要可写的 PTE
 * - get_user_pages() 用于内核访问用户内存，可能需要临时的写权限
 * - 但如果 VMA 本身不允许写入，不应该真正标记 PTE 为可写
 * - 这样可以保持权限的一致性
 */
static inline pte_t maybe_mkwrite(pte_t pte, struct vm_area_struct *vma)
{
	/* 【条件检查】VMA 是否允许写入
	 * 【C语言概念】likely 宏
	 * - 告诉编译器这个条件很可能为真
	 * - 大多数写缺页发生在可写的 VMA 中
	 * Check if VMA allows writing */
	if (likely(vma->vm_flags & VM_WRITE))
		pte = pte_mkwrite(pte, vma);  // 标记 PTE 为可写 / Mark PTE as writable

	/* 返回可能被修改的 PTE
	 * Return potentially modified PTE */
	return pte;
}

/**
 * do_set_pmd - 设置 PMD（Page Middle Directory）条目
 * @vmf: 缺页异常上下文
 * @folio: 要映射的 folio
 * @page: 要映射的页面
 *
 * 【功能说明】
 * - 在缺页处理中设置 PMD 级别的页表条目
 * - 用于大页映射（通过 PMD 直接映射，跳过 PTE 级别）
 * - 可以提高 TLB 效率
 *
 * 返回值：vm_fault_t 类型的缺页处理结果
 */
vm_fault_t do_set_pmd(struct vm_fault *vmf, struct folio *folio, struct page *page);

/**
 * set_pte_range - 设置一系列连续的 PTE（Page Table Entry）
 * @vmf: 缺页异常上下文
 * @folio: 要映射的 folio
 * @page: 起始页面
 * @nr: 要设置的 PTE 数量
 * @addr: 起始虚拟地址
 *
 * 【功能说明】
 * - 批量设置连续的页表条目
 * - 比逐个设置更高效
 * - 用于映射连续的页面范围
 *
 * 【参数说明】
 * - vmf: 包含缺页上下文信息
 * - folio: 包含要映射的页面的 folio
 * - page: 第一个要映射的页面
 * - nr: 要映射的页面数量
 * - addr: 起始虚拟地址
 */
void set_pte_range(struct vm_fault *vmf, struct folio *folio,
		struct page *page, unsigned int nr, unsigned long addr);

/**
 * finish_fault - 完成缺页处理
 * @vmf: 缺页异常上下文
 *
 * 【功能说明】
 * - 完成缺页异常处理的最后步骤
 * - 设置页表条目，更新 TLB 等
 * - 是缺页处理流程的收尾工作
 *
 * 返回值：vm_fault_t 类型的缺页处理结果
 */
vm_fault_t finish_fault(struct vm_fault *vmf);
#endif

/*
 * 【页面共享与引用计数】Page Sharing and Reference Counting
 *
 * 多个进程可能"看到"同一个页面
 * Multiple processes may "see" the same page.
 *
 * 【共享页面的例子】
 * E.g. for untouched mappings of /dev/null, all processes see the same page full of
 * zeroes, and text pages of executables and shared libraries have
 * only one copy in memory, at most, normally.
 * 例如：对于未触及的 /dev/null 映射，所有进程看到相同的全零页面，
 * 而可执行文件和共享库的代码页在内存中通常最多只有一份副本。
 *
 * 【引用计数机制】Reference Counting for Non-Reserved Pages
 * For the non-reserved pages, page_count(page) denotes a reference count.
 * 对于非保留页面，page_count(page) 表示引用计数。
 *
 *   page_count() == 0 means the page is free. page->lru is then used for
 *   freelist management in the buddy allocator.
 *   page_count() == 0 表示页面是空闲的。此时 page->lru 用于伙伴分配器的空闲列表管理。
 *
 *   page_count() > 0  means the page has been allocated.
 *   page_count() > 0 表示页面已被分配。
 *
 * 【Slab 分配器使用的页面】Pages Used by Slab Allocator
 * Pages are allocated by the slab allocator in order to provide memory
 * to kmalloc and kmem_cache_alloc. In this case, the management of the
 * page, and the fields in 'struct page' are the responsibility of mm/slab.c
 * unless a particular usage is carefully commented. (the responsibility of
 * freeing the kmalloc memory is the caller's, of course).
 * 页面由 slab 分配器分配，以便为 kmalloc 和 kmem_cache_alloc 提供内存。
 * 在这种情况下，页面的管理和 'struct page' 中的字段是 mm/slab.c 的责任，
 * 除非特定用途有明确注释。（释放 kmalloc 内存的责任当然是调用者的。）
 *
 * 【通用页面分配】Generic Page Allocation
 * A page may be used by anyone else who does a __get_free_page().
 * In this case, page_count still tracks the references, and should only
 * be used through the normal accessor functions. The top bits of page->flags
 * and page->virtual store page management information, but all other fields
 * are unused and could be used privately, carefully. The management of this
 * page is the responsibility of the one who allocated it, and those who have
 * subsequently been given references to it.
 * 页面可以被任何调用 __get_free_page() 的人使用。
 * 在这种情况下，page_count 仍然跟踪引用，应该只通过正常的访问函数使用。
 * page->flags 的高位和 page->virtual 存储页面管理信息，但所有其他字段
 * 未使用，可以小心地私有使用。此页面的管理是分配它的人以及后续获得引用的人的责任。
 *
 * 【页缓存页面】Pagecache Pages
 * The other pages (we may call them "pagecache pages") are completely
 * managed by the Linux memory manager: I/O, buffers, swapping etc.
 * The following discussion applies only to them.
 * 其他页面（我们可以称之为"页缓存页面"）完全由 Linux 内存管理器管理：I/O、缓冲区、交换等。
 * 以下讨论仅适用于它们。
 *
 * 【页缓存的 private 字段】
 * A pagecache page contains an opaque `private' member, which belongs to the
 * page's address_space. Usually, this is the address of a circular list of
 * the page's disk buffers. PG_private must be set to tell the VM to call
 * into the filesystem to release these pages.
 * 页缓存页面包含一个不透明的 `private' 成员，它属于页面的 address_space。
 * 通常，这是页面磁盘缓冲区的循环列表的地址。
 * 必须设置 PG_private 以告诉 VM 调用文件系统来释放这些页面。
 *
 * 【Folio 与 Inode 的关联】
 * A folio may belong to an inode's memory mapping. In this case,
 * folio->mapping points to the inode, and folio->index is the file
 * offset of the folio, in units of PAGE_SIZE.
 * folio 可能属于 inode 的内存映射。在这种情况下，
 * folio->mapping 指向 inode，folio->index 是 folio 的文件偏移量，以 PAGE_SIZE 为单位。
 *
 * 【匿名页面】Anonymous Pages
 * If pagecache pages are not associated with an inode, they are said to be
 * anonymous pages. These may become associated with the swapcache, and in that
 * case PG_swapcache is set, and page->private is an offset into the swapcache.
 * 如果页缓存页面不与 inode 关联，则称为匿名页面。
 * 这些页面可能与交换缓存关联，在这种情况下设置 PG_swapcache，
 * 并且 page->private 是交换缓存中的偏移量。
 *
 * 【引用计数规则】
 * In either case (swapcache or inode backed), the pagecache itself holds one
 * reference to the page. Setting PG_private should also increment the
 * refcount. The each user mapping also has a reference to the page.
 * 无论哪种情况（交换缓存或 inode 支持），页缓存本身持有对页面的一个引用。
 * 设置 PG_private 也应该增加引用计数。每个用户映射也持有对页面的引用。
 *
 * 【页缓存的存储结构】
 * The pagecache pages are stored in a per-mapping radix tree, which is
 * rooted at mapping->i_pages, and indexed by offset.
 * Where 2.4 and early 2.6 kernels kept dirty/clean pages in per-address_space
 * lists, we instead now tag pages as dirty/writeback in the radix tree.
 * 页缓存页面存储在每个映射的基数树中，该树根植于 mapping->i_pages，并按偏移量索引。
 * 在 2.4 和早期 2.6 内核中，脏/干净页面保存在每个 address_space 的列表中，
 * 而现在我们在基数树中将页面标记为脏/回写。
 *
 * 【页缓存的 I/O 操作】
 * All pagecache pages may be subject to I/O:
 * 所有页缓存页面都可能涉及 I/O：
 * - inode pages may need to be read from disk,
 *   inode 页面可能需要从磁盘读取，
 * - inode pages which have been modified and are MAP_SHARED may need
 *   to be written back to the inode on disk,
 *   已修改且为 MAP_SHARED 的 inode 页面可能需要写回磁盘上的 inode，
 * - anonymous pages (including MAP_PRIVATE file mappings) which have been
 *   modified may need to be swapped out to swap space and (later) to be read
 *   back into memory.
 *   已修改的匿名页面（包括 MAP_PRIVATE 文件映射）可能需要交换到交换空间，
 *   并且（稍后）读回内存。
 */

/* 127: 任意随机数，足够小以便良好汇编
 * 127: arbitrary random number, small enough to assemble well
 *
 * 【设计原理】检测引用计数溢出
 * - 如果 folio_ref_count + 127 <= 127，则引用计数为 0 或接近溢出
 * - 这利用了无符号整数溢出的回绕特性
 * - 引用计数为 0 时：0 + 127 = 127 <= 127（真）
 * - 引用计数接近 UINT_MAX 时：UINT_MAX + 127 会回绕到接近 127（真）
 * - 正常引用计数时：例如 10 + 127 = 137 > 127（假）
 */
#define folio_ref_zero_or_close_to_overflow(folio) \
	((unsigned int) folio_ref_count(folio) + 127u <= 127u)

/**
 * folio_get - 增加 folio 的引用计数
 * @folio: 目标 folio
 *
 * 【功能说明】
 * - 增加 folio 的引用计数
 * - 表示有新的引用者持有此 folio
 * - 阻止 folio 被释放
 *
 * 【使用前提】
 * Context: May be called in any context, as long as you know that
 * you have a refcount on the folio.  If you do not already have one,
 * folio_try_get() may be the right interface for you to use.
 * 上下文：只要你知道自己已经持有 folio 的引用计数，就可以在任何上下文中调用。
 * 如果你还没有引用，folio_try_get() 可能是你应该使用的接口。
 *
 * 【安全检查】
 * - VM_BUG_ON_FOLIO 检查引用计数是否为 0 或接近溢出
 * - 在引用计数为 0 时调用此函数是错误的（页面已释放）
 * - 引用计数接近溢出时也是错误的（溢出会导致释放错误）
 */
static inline void folio_get(struct folio *folio)
{
	/* 【调试检查】确保引用计数不是 0 或接近溢出
	 * Debug check: ensure refcount is not 0 or close to overflow */
	VM_BUG_ON_FOLIO(folio_ref_zero_or_close_to_overflow(folio), folio);

	/* 原子地增加引用计数
	 * Atomically increment reference count */
	folio_ref_inc(folio);
}

/**
 * get_page - 增加页面的引用计数
 * @page: 目标页面
 *
 * 【功能说明】
 * - 旧的页面引用计数接口（推荐使用 folio_get）
 * - 内部转换为 folio 操作
 *
 * 【特殊处理】Slab 和 kmalloc 页面
 * - Slab 分配器和大对象 kmalloc 使用的页面有特殊的引用计数机制
 * - 对这些页面发出警告并返回，不增加引用计数
 */
static inline void get_page(struct page *page)
{
	/* 将页面转换为 folio
	 * Convert page to folio */
	struct folio *folio = page_folio(page);

	/* 【特殊情况】Slab 页面
	 * - Slab 分配器管理的页面不应该通过 get_page 增加引用
	 * - 发出一次性警告
	 * Special case: slab pages */
	if (WARN_ON_ONCE(folio_test_slab(folio)))
		return;

	/* 【特殊情况】大对象 kmalloc 页面
	 * - 通过 kmalloc 分配的大对象也有特殊处理
	 * Special case: large kmalloc pages */
	if (WARN_ON_ONCE(folio_test_large_kmalloc(folio)))
		return;

	/* 正常情况：增加 folio 引用计数
	 * Normal case: increment folio refcount */
	folio_get(folio);
}

/**
 * try_get_page - 尝试增加页面引用计数
 * @page: 目标页面
 *
 * 【功能说明】
 * - 安全地尝试增加页面引用计数
 * - 如果页面引用计数已经为 0 或负数，则失败
 * - 用于不确定页面是否仍然有效的情况
 *
 * 【C语言概念】__must_check 属性
 * - 强制调用者检查返回值
 * - 忽略返回值会导致编译器警告
 * - 确保调用者知道操作是否成功
 *
 * 返回值：
 * - true: 成功增加引用计数
 * - false: 失败（页面引用计数 <= 0）
 */
static inline __must_check bool try_get_page(struct page *page)
{
	/* 获取复合页的头页
	 * Get head page of compound page */
	page = compound_head(page);

	/* 【安全检查】引用计数是否有效
	 * - 引用计数 <= 0 表示页面已释放或即将释放
	 * - 此时不应该增加引用计数
	 * Check if refcount is valid */
	if (WARN_ON_ONCE(page_ref_count(page) <= 0))
		return false;

	/* 增加引用计数
	 * Increment reference count */
	page_ref_inc(page);
	return true;
}

/**
 * folio_put - 递减 folio 的引用计数
 * @folio: 目标 folio
 *
 * 【功能说明】
 * - 递减 folio 的引用计数
 * - 如果引用计数降为 0，释放内存回页面分配器
 *
 * 【重要警告】引用计数归零后的访问
 * If the folio's reference count reaches zero, the memory will be
 * released back to the page allocator and may be used by another
 * allocation immediately.  Do not access the memory or the struct folio
 * after calling folio_put() unless you can be sure that it wasn't the
 * last reference.
 * 如果 folio 的引用计数降为零，内存将被释放回页面分配器，
 * 并可能立即被另一个分配使用。除非你能确定这不是最后一个引用，
 * 否则在调用 folio_put() 后不要访问内存或 struct folio。
 *
 * 【调用上下文】
 * Context: May be called in process or interrupt context, but not in NMI
 * context.  May be called while holding a spinlock.
 * 上下文：可以在进程或中断上下文中调用，但不能在 NMI 上下文中调用。
 * 可以在持有自旋锁时调用。
 */
static inline folio_put(struct folio *folio)
{
	/* 【原子操作】测试并递减引用计数
	 * - folio_put_testzero: 递减引用计数，如果变为 0 则返回 true
	 * - 只有最后一个引用者会看到返回值为 true
	 * Test and decrement: returns true if refcount reaches zero */
	if (folio_put_testzero(folio))
		__folio_put(folio);  // 引用计数为 0，执行实际释放 / Actually free the folio
}

/**
 * folio_put_refs - 递减 folio 的引用计数（指定减少量）
 * @folio: 目标 folio
 * @refs: 要从 folio 引用计数中减去的数量
 *
 * 【功能说明】
 * - 一次性减少多个引用计数
 * - 比多次调用 folio_put 更高效
 *
 * 【重要警告】引用计数归零后的访问
 * If the folio's reference count reaches zero, the memory will be
 * released back to the page allocator and may be used by another
 * allocation immediately.  Do not access the memory or the struct folio
 * after calling folio_put_refs() unless you can be sure that these weren't
 * the last references.
 * 如果 folio 的引用计数降为零，内存将被释放回页面分配器，
 * 并可能立即被另一个分配使用。除非你能确定这些不是最后的引用，
 * 否则在调用 folio_put_refs() 后不要访问内存或 struct folio。
 *
 * 【调用上下文】
 * Context: May be called in process or interrupt context, but not in NMI
 * context.  May be called while holding a spinlock.
 * 上下文：可以在进程或中断上下文中调用，但不能在 NMI 上下文中调用。
 * 可以在持有自旋锁时调用。
 */
static inline void folio_put_refs(struct folio *folio, int refs)
{
	/* 【原子操作】减少指定数量的引用计数并测试是否为零
	 * - folio_ref_sub_and_test: 减去 refs，如果结果为 0 则返回 true
	 * Subtract refs and test if zero */
	if (folio_ref_sub_and_test(folio, refs))
		__folio_put(folio);  // 引用计数为 0，执行实际释放 / Actually free the folio
}

/**
 * folios_put_refs - 批量递减多个 folio 的引用计数
 * @folios: folio 批次（数组）
 * @refs: 每个 folio 对应的引用计数减少量数组
 *
 * 【功能说明】
 * - 批量处理多个 folio 的引用计数递减
 * - 比逐个调用 folio_put_refs 更高效
 * - 可以优化锁的使用
 */
void folios_put_refs(struct folio_batch *folios, unsigned int *refs);

/*
 * union release_pages_arg - 页面或 folio 的数组
 *
 * 【功能说明】
 * release_pages() releases a simple array of multiple pages, and
 * accepts various different forms of said page array: either
 * a regular old boring array of pages, an array of folios, or
 * an array of encoded page pointers.
 * release_pages() 释放多个页面的简单数组，并接受多种不同形式的页面数组：
 * 普通的页面数组、folio 数组或编码的页面指针数组。
 *
 * 【C语言概念】透明联合（transparent union）
 * The transparent union syntax for this kind of "any of these
 * argument types" is all kinds of ugly, so look away.
 * 这种"任意参数类型"的透明联合语法非常丑陋，所以请忽略它。
 *
 * 【设计原理】为什么使用透明联合？
 * - 允许同一个函数接受不同类型的参数
 * - 编译器会自动转换类型
 * - __transparent_union__ 属性告诉编译器自动处理类型转换
 * - 避免了重载函数或宏的复杂性
 */
typedef union {
	struct page **pages;           // 页面指针数组 / Array of page pointers
	struct folio **folios;         // folio 指针数组 / Array of folio pointers
	struct encoded_page **encoded_pages;  // 编码的页面指针数组 / Array of encoded page pointers
} release_pages_arg __attribute__ ((__transparent_union__));

/**
 * release_pages - 释放多个页面或 folio
 * @arg: 页面/folio 数组（透明联合类型）
 * @nr: 数组中的元素数量
 *
 * 【功能说明】
 * - 批量释放页面或 folio
 * - 接受多种类型的数组作为参数
 * - 比逐个释放更高效
 */
void release_pages(release_pages_arg, int nr);

/**
 * folios_put - 递减 folio 数组的引用计数
 * @folios: folio 批次
 *
 * 【功能说明】
 * Like folio_put(), but for a batch of folios.  This is more efficient
 * than writing the loop yourself as it will optimise the locks which need
 * to be taken if the folios are freed.  The folios batch is returned
 * empty and ready to be reused for another batch; there is no need to
 * reinitialise it.
 * 类似于 folio_put()，但用于一批 folio。这比自己编写循环更高效，
 * 因为它会优化释放 folio 时需要获取的锁。
 * folio 批次返回时为空，可以立即重用于另一批；无需重新初始化。
 *
 * 【设计优势】批量处理的好处
 * - 减少锁的获取和释放次数
 * - 可以对同一个内存区域的多个页面一起处理
 * - 提高缓存局部性
 *
 * 【调用上下文】
 * Context: May be called in process or interrupt context, but not in NMI
 * context.  May be called while holding a spinlock.
 * 上下文：可以在进程或中断上下文中调用，但不能在 NMI 上下文中调用。
 * 可以在持有自旋锁时调用。
 */
static inline void folios_put(struct folio_batch *folios)
{
	/* 调用 folios_put_refs，传入 NULL 表示每个 folio 减少 1 个引用
	 * Call folios_put_refs with NULL to decrement each folio by 1 */
	folios_put_refs(folios, NULL);
}

/**
 * put_page - 递减页面的引用计数
 * @page: 目标页面
 *
 * 【功能说明】
 * - 旧的页面引用计数递减接口（推荐使用 folio_put）
 * - 内部转换为 folio 操作
 *
 * 【特殊处理】Slab 和 kmalloc 页面
 * - Slab 分配器和大对象 kmalloc 使用的页面有特殊的引用计数机制
 * - 这些页面不通过 put_page 释放，直接返回
 * - 它们由 slab 分配器或 kmalloc 内部管理
 */
static inline void put_page(struct page *page)
{
	/* 将页面转换为 folio
	 * Convert page to folio */
	struct folio *folio = page_folio(page);

	/* 【特殊情况】Slab 或大对象 kmalloc 页面
	 * - 这些页面不使用标准的引用计数机制
	 * - 直接返回，不递减引用计数
	 * Special case: slab or large kmalloc pages */
	if (folio_test_slab(folio) || folio_test_large_kmalloc(folio))
		return;

	/* 正常情况：递减 folio 引用计数
	 * Normal case: decrement folio refcount */
	folio_put(folio);
}

/*
 * 【GUP Pin 计数偏移】GUP_PIN_COUNTING_BIAS
 *
 * GUP_PIN_COUNTING_BIAS, and the associated functions that use it, overload
 * the page's refcount so that two separate items are tracked: the original page
 * reference count, and also a new count of how many pin_user_pages() calls were
 * made against the page. ("gup-pinned" is another term for the latter).
 * GUP_PIN_COUNTING_BIAS 及其相关函数重载了页面的引用计数，
 * 以便跟踪两个独立的项目：原始页面引用计数，以及对页面进行了多少次 pin_user_pages() 调用的新计数。
 * （"gup-pinned" 是后者的另一个术语。）
 *
 * 【设计原理】为什么需要 Pin 计数？
 * With this scheme, pin_user_pages() becomes special: such pages are marked as
 * distinct from normal pages. As such, the unpin_user_page() call (and its
 * variants) must be used in order to release gup-pinned pages.
 * 通过这种方案，pin_user_pages() 变得特殊：这些页面被标记为与普通页面不同。
 * 因此，必须使用 unpin_user_page() 调用（及其变体）才能释放 gup-pinned 页面。
 *
 * 【值的选择】Choice of value:
 *
 * By making GUP_PIN_COUNTING_BIAS a power of two, debugging of page reference
 * counts with respect to pin_user_pages() and unpin_user_page() becomes
 * simpler, due to the fact that adding an even power of two to the page
 * refcount has the effect of using only the upper N bits, for the code that
 * counts up using the bias value. This means that the lower bits are left for
 * the exclusive use of the original code that increments and decrements by one
 * (or at least, by much smaller values than the bias value).
 * 通过将 GUP_PIN_COUNTING_BIAS 设置为 2 的幂，使用 pin_user_pages() 和 unpin_user_page()
 * 调试页面引用计数变得更简单，因为向页面引用计数添加 2 的幂的效果是仅使用高 N 位，
 * 用于使用偏移值计数的代码。这意味着低位保留给原始代码专用，
 * 原始代码以 1 递增和递减（或至少以比偏移值小得多的值）。
 *
 * Of course, once the lower bits overflow into the upper bits (and this is
 * OK, because subtraction recovers the original values), then visual inspection
 * no longer suffices to directly view the separate counts. However, for normal
 * applications that don't have huge page reference counts, this won't be an
 * issue.
 * 当然，一旦低位溢出到高位（这没问题，因为减法可以恢复原始值），
 * 那么视觉检查就不再足以直接查看单独的计数。但是，对于没有大量页面引用计数的正常应用程序，
 * 这不会是问题。
 *
 * 【锁机制】Locking:
 * the lockless algorithm described in folio_try_get_rcu()
 * provides safe operation for get_user_pages(), folio_mkclean() and
 * other calls that race to set up page table entries.
 * folio_try_get_rcu() 中描述的无锁算法为 get_user_pages()、folio_mkclean()
 * 和其他竞争设置页表条目的调用提供安全操作。
 *
 * 【实际值】1U << 10 = 1024
 * - Pin 计数使用高位，每次 pin 增加 1024
 * - 普通引用计数使用低位，每次增加 1
 * - 这样可以在同一个引用计数中跟踪两种不同的计数
 */
#define GUP_PIN_COUNTING_BIAS (1U << 10)

/**
 * unpin_user_page - 释放一个 pinned 用户页面
 * @page: 要释放的页面
 *
 * 【功能说明】
 * - 释放通过 pin_user_pages() 获得的页面
 * - 必须与 pin_user_pages() 配对使用
 * - 不能用 put_page() 替代
 */
void unpin_user_page(struct page *page);

/**
 * unpin_folio - 释放一个 pinned folio
 * @folio: 要释放的 folio
 *
 * 【功能说明】
 * - folio 版本的 unpin_user_page
 */
void unpin_folio(struct folio *folio);

/**
 * unpin_user_pages_dirty_lock - 批量释放 pinned 页面并标记为脏
 * @pages: 页面数组
 * @npages: 页面数量
 * @make_dirty: 是否标记页面为脏
 *
 * 【功能说明】
 * - 批量释放通过 pin_user_pages() 获得的页面
 * - 可选地将页面标记为脏（已修改）
 * - 用于 Direct I/O 等场景
 */
void unpin_user_pages_dirty_lock(struct page **pages, unsigned long npages,
				 bool make_dirty);

/**
 * unpin_user_page_range_dirty_lock - 释放连续范围的 pinned 页面并标记为脏
 * @page: 起始页面
 * @npages: 页面数量
 * @make_dirty: 是否标记页面为脏
 *
 * 【功能说明】
 * - 释放连续范围的 pinned 页面
 * - 可选地标记为脏
 */
void unpin_user_page_range_dirty_lock(struct page *page, unsigned long npages,
				      bool make_dirty);

/**
 * unpin_user_pages - 批量释放 pinned 用户页面
 * @pages: 页面数组
 * @npages: 页面数量
 *
 * 【功能说明】
 * - 批量释放 pinned 页面，不标记为脏
 */
void unpin_user_pages(struct page **pages, unsigned long npages);

/**
 * unpin_user_folio - 释放 pinned 用户 folio
 * @folio: 要释放的 folio
 * @npages: folio 中的页面数量
 *
 * 【功能说明】
 * - folio 版本的 unpin_user_pages
 */
void unpin_user_folio(struct folio *folio, unsigned long npages);

/**
 * unpin_folios - 批量释放 pinned folio
 * @folios: folio 数组
 * @nfolios: folio 数量
 *
 * 【功能说明】
 * - 批量释放 pinned folio
 */
void unpin_folios(struct folio **folios, unsigned long nfolios);

/**
 * is_cow_mapping - 判断是否为 COW (Copy-On-Write) 映射
 * @flags: vm_flags 标志位
 *
 * 【功能说明】
 * - 判断一个内存映射是否是 COW 映射
 * - COW 映射：私有可写映射，写时复制
 *
 * 【设计原理】为什么这么判断？
 * - VM_SHARED：共享映射（多个进程共享同一物理页）
 * - VM_MAYWRITE：可能可写
 * - 如果 VM_MAYWRITE 置位但 VM_SHARED 未置位，说明是私有可写映射
 * - 私有可写映射使用 COW 机制：读时共享，写时复制
 *
 * 【返回值】
 * - true: 是 COW 映射（私有可写）
 * - false: 不是 COW 映射（共享映射或只读映射）
 */
static inline bool is_cow_mapping(vm_flags_t flags)
{
	return (flags & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE;
}

/**
 * vma_desc_is_cow_mapping - 判断 VMA 描述符是否为 COW 映射
 * @desc: VMA 描述符
 *
 * 【功能说明】
 * - vma_desc 版本的 is_cow_mapping
 * - 使用 vma_flags_test 检查标志位
 *
 * 【返回值】
 * - true: 是 COW 映射
 * - false: 不是 COW 映射
 */
static inline bool vma_desc_is_cow_mapping(struct vm_area_desc *desc)
{
	const vma_flags_t *flags = &desc->vma_flags;

	return vma_flags_test(flags, VMA_MAYWRITE_BIT) &&
		!vma_flags_test(flags, VMA_SHARED_BIT);
}

#ifndef CONFIG_MMU
/**
 * is_nommu_shared_mapping - 判断 NOMMU 系统中是否为共享映射
 * @flags: vm_flags 标志位
 *
 * 【NOMMU 系统说明】
 * - NOMMU: No Memory Management Unit，无内存管理单元
 * - 在嵌入式系统中，可能没有 MMU 硬件
 * - 无 MMU 系统无法实现虚拟内存和页表机制
 *
 * 【功能说明】
 * NOMMU shared mappings are ordinary MAP_SHARED mappings and selected
 * R/O MAP_PRIVATE file mappings that are an effective R/O overlay of
 * a file mapping. R/O MAP_PRIVATE mappings might still modify
 * underlying memory if ptrace is active, so this is only possible if
 * ptrace does not apply. Note that there is no mprotect() to upgrade
 * write permissions later.
 * NOMMU 共享映射是普通的 MAP_SHARED 映射和选定的只读 MAP_PRIVATE 文件映射，
 * 它们是文件映射的有效只读覆盖。如果 ptrace 处于活动状态，
 * 只读 MAP_PRIVATE 映射可能仍会修改底层内存，因此仅当 ptrace 不适用时才可能。
 * 请注意，以后没有 mprotect() 来升级写权限。
 *
 * 【设计原理】
 * - VM_MAYSHARE: 可能共享（MAP_SHARED 映射）
 * - VM_MAYOVERLAY: 可能覆盖（只读 MAP_PRIVATE 文件映射）
 * - 这两种情况下物理内存可以被多个进程共享
 *
 * 【返回值】
 * - true: 是 NOMMU 共享映射
 * - false: 不是共享映射
 */
static inline bool is_nommu_shared_mapping(vm_flags_t flags)
{
	/*
	 * NOMMU shared mappings are ordinary MAP_SHARED mappings and selected
	 * R/O MAP_PRIVATE file mappings that are an effective R/O overlay of
	 * a file mapping. R/O MAP_PRIVATE mappings might still modify
	 * underlying memory if ptrace is active, so this is only possible if
	 * ptrace does not apply. Note that there is no mprotect() to upgrade
	 * write permissions later.
	 */
	return flags & (VM_MAYSHARE | VM_MAYOVERLAY);
}

/**
 * is_nommu_shared_vma_flags - vma_flags 版本的 is_nommu_shared_mapping
 * @flags: vma_flags 标志位指针
 *
 * 【功能说明】
 * - 使用 vma_flags_test_any 测试多个标志位
 * - 任意一个标志位置位即返回 true
 *
 * 【返回值】
 * - true: 是 NOMMU 共享映射
 * - false: 不是共享映射
 */
static inline bool is_nommu_shared_vma_flags(const vma_flags_t *flags)
{
	return vma_flags_test_any(flags, VMA_MAYSHARE_BIT, VMA_MAYOVERLAY_BIT);
}
#endif

#if defined(CONFIG_SPARSEMEM) && !defined(CONFIG_SPARSEMEM_VMEMMAP)
/**
 * 【稀疏内存模型】SECTION_IN_PAGE_FLAGS
 *
 * 【配置说明】
 * - CONFIG_SPARSEMEM: 稀疏内存模型配置
 * - CONFIG_SPARSEMEM_VMEMMAP: 虚拟内存映射优化
 *
 * 【设计原理】
 * - 当使用稀疏内存模型但未启用 VMEMMAP 优化时
 * - 需要在页面标志中存储 section 编号
 * - 用于定位页面所属的内存段
 */
#define SECTION_IN_PAGE_FLAGS
#endif

/*
 * The identification function is mainly used by the buddy allocator for
 * determining if two pages could be buddies. We are not really identifying
 * the zone since we could be using the section number id if we do not have
 * node id available in page flags.
 * We only guarantee that it will return the same value for two combinable
 * pages in a zone.
 * 识别函数主要由伙伴分配器用于确定两个页面是否可以成为伙伴。
 * 我们并不是真正识别区域，因为如果页面标志中没有节点 ID，
 * 我们可能会使用段号 ID。
 * 我们只保证它对区域中两个可组合的页面返回相同的值。
 *
 * 【伙伴系统说明】
 * - 伙伴系统用于管理空闲页面
 * - 两个连续的相同大小的页面块可以合并为一个更大的块
 * - 这些可以合并的页面对称为"伙伴"（buddies）
 * - 判断伙伴关系需要知道页面的区域信息
 */
/**
 * page_zone_id - 获取页面的区域 ID
 * @page: 页面结构体指针
 *
 * 【功能说明】
 * - 从页面标志中提取区域 ID
 * - 用于伙伴分配器判断页面是否可以合并
 *
 * 【实现原理】
 * - 右移 ZONEID_PGSHIFT 位，获取区域 ID 字段
 * - 与 ZONEID_MASK 掩码进行按位与操作，提取有效位
 *
 * 【C 语言知识】位操作
 * - >>: 右移运算符，将二进制位向右移动
 * - &: 按位与运算符，用于提取特定位
 * - 掩码（mask）用于选择或清除特定的二进制位
 *
 * 【返回值】
 * - 区域 ID 值
 */
static inline int page_zone_id(struct page *page)
{
	return (page->flags.f >> ZONEID_PGSHIFT) & ZONEID_MASK;
}

#ifdef NODE_NOT_IN_PAGE_FLAGS
/**
 * memdesc_nid - 获取内存描述符的节点 ID（外部函数版本）
 * @mdf: 内存描述符标志
 *
 * 【功能说明】
 * - 当页面标志中没有存储节点 ID 时使用
 * - 需要通过外部函数获取节点 ID
 *
 * 【NUMA 说明】
 * - NUMA: Non-Uniform Memory Access，非统一内存访问
 * - 在 NUMA 系统中，内存按节点划分
 * - 访问本地节点内存比远程节点内存更快
 *
 * 【返回值】
 * - 节点 ID（node ID）
 */
int memdesc_nid(memdesc_flags_t mdf);
#else
/**
 * memdesc_nid - 获取内存描述符的节点 ID（内联函数版本）
 * @mdf: 内存描述符标志
 *
 * 【功能说明】
 * - 当页面标志中存储了节点 ID 时使用
 * - 直接从标志位中提取节点 ID
 *
 * 【实现原理】
 * - 右移 NODES_PGSHIFT 位
 * - 与 NODES_MASK 掩码进行按位与操作
 * - 提取节点 ID 字段
 *
 * 【返回值】
 * - 节点 ID
 */
static inline int memdesc_nid(memdesc_flags_t mdf)
{
	return (mdf.f >> NODES_PGSHIFT) & NODES_MASK;
}
#endif

/**
 * page_to_nid - 获取页面所属的 NUMA 节点 ID
 * @page: 页面结构体指针
 *
 * 【功能说明】
 * - 获取页面所在的 NUMA 节点
 * - 用于 NUMA 感知的内存分配和调度
 *
 * 【实现说明】
 * - PF_POISONED_CHECK: 检查页面是否被污染（已释放或损坏）
 * - 从页面标志中提取节点信息
 *
 * 【注意事项】
 * - 页面污染检查用于调试，防止访问已释放的页面
 * - 生产环境中通常优化为直接访问
 *
 * 【返回值】
 * - 页面所属的节点 ID
 */
static inline int page_to_nid(const struct page *page)
{
	return memdesc_nid(PF_POISONED_CHECK(page)->flags);
}

/**
 * folio_nid - 获取 folio 所属的 NUMA 节点 ID
 * @folio: folio 结构体指针
 *
 * 【功能说明】
 * - folio 版本的 page_to_nid
 * - 直接从 folio 标志中提取节点 ID
 *
 * 【返回值】
 * - folio 所属的节点 ID
 */
static inline int folio_nid(const struct folio *folio)
{
	return memdesc_nid(folio->flags);
}

#ifdef CONFIG_NUMA_BALANCING
/* page access time bits needs to hold at least 4 seconds */
/* 页面访问时间位需要至少保存 4 秒 */
/**
 * 【NUMA 自动平衡】PAGE_ACCESS_TIME_MIN_BITS
 *
 * 【功能说明】
 * - NUMA 自动平衡需要跟踪页面访问时间
 * - 至少需要 12 位来存储 4 秒的时间信息
 * - 用于判断页面是否应该迁移到访问它的 CPU 所在节点
 *
 * 【设计原理】
 * - 如果页面频繁被某个 CPU 访问，迁移到该 CPU 所在节点可以提高性能
 * - 需要记录最后访问页面的 CPU 和进程 ID
 * - 时间信息用于判断访问的"新鲜度"
 */
#define PAGE_ACCESS_TIME_MIN_BITS	12
#if LAST_CPUPID_SHIFT < PAGE_ACCESS_TIME_MIN_BITS
/**
 * 【时间桶数】PAGE_ACCESS_TIME_BUCKETS
 *
 * 【设计原理】
 * - 当 CPUPID 字段位数不足时，需要使用时间桶
 * - 时间桶将时间量化为更粗粒度的区间
 * - 例如：不是精确到秒，而是精确到 2 秒、4 秒等
 */
#define PAGE_ACCESS_TIME_BUCKETS				\
	(PAGE_ACCESS_TIME_MIN_BITS - LAST_CPUPID_SHIFT)
#else
#define PAGE_ACCESS_TIME_BUCKETS	0
#endif

/**
 * 【访问时间掩码】PAGE_ACCESS_TIME_MASK
 *
 * 【功能说明】
 * - 用于从页面标志中提取访问时间信息
 * - 将 CPUPID 掩码左移时间桶位数
 */
#define PAGE_ACCESS_TIME_MASK				\
	(LAST_CPUPID_MASK << PAGE_ACCESS_TIME_BUCKETS)

/**
 * cpu_pid_to_cpupid - 将 CPU ID 和进程 ID 编码为 CPUPID
 * @cpu: CPU ID
 * @pid: 进程 ID
 *
 * 【功能说明】
 * - 将 CPU ID 和进程 ID 组合成一个整数
 * - 用于标识最后访问页面的 CPU 和进程
 *
 * 【编码格式】
 * - 高位存储 CPU ID
 * - 低位存储进程 ID
 *
 * 【设计原理】为什么需要 CPUPID？
 * - NUMA 自动平衡需要知道哪个 CPU 上的哪个进程访问了页面
 * - 如果同一个进程持续从同一个 CPU 访问页面，考虑迁移页面
 * - 节省空间：两个 ID 编码在一个整数中
 *
 * 【C 语言知识】位运算编码
 * - &: 按位与，用掩码提取有效位
 * - <<: 左移，将 CPU ID 移动到高位
 * - |: 按位或，将两个字段组合
 *
 * 【返回值】
 * - 编码后的 CPUPID 值
 */
static inline int cpu_pid_to_cpupid(int cpu, int pid)
{
	return ((cpu & LAST__CPU_MASK) << LAST__PID_SHIFT) | (pid & LAST__PID_MASK);
}

/**
 * cpupid_to_pid - 从 CPUPID 中提取进程 ID
 * @cpupid: 编码的 CPUPID 值
 *
 * 【功能说明】
 * - 从组合值中解码出进程 ID
 * - 与 cpu_pid_to_cpupid 相反的操作
 *
 * 【实现原理】
 * - 进程 ID 存储在低位
 * - 直接与掩码进行按位与操作即可提取
 *
 * 【返回值】
 * - 解码后的进程 ID
 */
static inline int cpupid_to_pid(int cpupid)
{
	return cpupid & LAST__PID_MASK;
}

/**
 * cpupid_to_cpu - 从 CPUPID 中提取 CPU ID
 * @cpupid: 编码的 CPUPID 值
 *
 * 【功能说明】
 * - 从组合值中解码出 CPU ID
 *
 * 【实现原理】
 * - CPU ID 存储在高位
 * - 先右移 LAST__PID_SHIFT 位，将 CPU ID 移到低位
 * - 再与掩码进行按位与操作，提取 CPU ID
 *
 * 【返回值】
 * - 解码后的 CPU ID
 */
static inline int cpupid_to_cpu(int cpupid)
{
	return (cpupid >> LAST__PID_SHIFT) & LAST__CPU_MASK;
}

/**
 * cpupid_to_nid - 从 CPUPID 中获取 NUMA 节点 ID
 * @cpupid: 编码的 CPUPID 值
 *
 * 【功能说明】
 * - 先提取 CPU ID，再通过 CPU ID 获取节点 ID
 * - 每个 CPU 属于一个 NUMA 节点
 *
 * 【设计原理】
 * - 不直接存储节点 ID，而是通过 CPU ID 间接获取
 * - 节省存储空间
 * - cpu_to_node 是查找表或计算函数
 *
 * 【返回值】
 * - CPU 所属的节点 ID
 */
static inline int cpupid_to_nid(int cpupid)
{
	return cpu_to_node(cpupid_to_cpu(cpupid));
}

/**
 * cpupid_pid_unset - 检查 CPUPID 中的进程 ID 是否未设置
 * @cpupid: CPUPID 值
 *
 * 【功能说明】
 * - 判断进程 ID 字段是否为 -1（未设置）
 *
 * 【实现原理】
 * - -1 的二进制表示所有位都是 1
 * - -1 & LAST__PID_MASK 得到 PID 字段的全 1 模式
 * - 表示该字段未初始化或已重置
 *
 * 【C 语言知识】负数的二进制表示
 * - C 语言中整数使用补码表示
 * - -1 的补码是所有位都为 1
 * - 与掩码进行按位与操作后得到该字段的最大值
 *
 * 【返回值】
 * - true: 进程 ID 未设置
 * - false: 进程 ID 已设置
 */
static inline bool cpupid_pid_unset(int cpupid)
{
	return cpupid_to_pid(cpupid) == (-1 & LAST__PID_MASK);
}

/**
 * cpupid_cpu_unset - 检查 CPUPID 中的 CPU ID 是否未设置
 * @cpupid: CPUPID 值
 *
 * 【功能说明】
 * - 判断 CPU ID 字段是否为 -1（未设置）
 *
 * 【返回值】
 * - true: CPU ID 未设置
 * - false: CPU ID 已设置
 */
static inline bool cpupid_cpu_unset(int cpupid)
{
	return cpupid_to_cpu(cpupid) == (-1 & LAST__CPU_MASK);
}

/**
 * __cpupid_match_pid - 检查任务进程 ID 是否匹配 CPUPID 中的进程 ID
 * @task_pid: 任务的进程 ID
 * @cpupid: CPUPID 值
 *
 * 【功能说明】
 * - 判断任务的 PID 是否与 CPUPID 中存储的 PID 相同
 * - 用于 NUMA 平衡判断是否是同一个进程在访问页面
 *
 * 【设计原理】
 * - 只比较低位的 PID 部分
 * - 由于 PID 字段位数有限，可能发生冲突
 * - 但冲突概率较低，对 NUMA 平衡的影响可接受
 *
 * 【返回值】
 * - true: 进程 ID 匹配
 * - false: 进程 ID 不匹配
 */
static inline bool __cpupid_match_pid(pid_t task_pid, int cpupid)
{
	return (task_pid & LAST__PID_MASK) == cpupid_to_pid(cpupid);
}

/**
 * 【宏定义】cpupid_match_pid - 检查任务是否匹配 CPUPID
 * @task: 任务结构体指针
 * @cpupid: CPUPID 值
 *
 * 【功能说明】
 * - 从任务结构体中提取 PID 并匹配
 * - 便捷宏，避免手动访问 task->pid
 */
#define cpupid_match_pid(task, cpupid) __cpupid_match_pid(task->pid, cpupid)
#ifdef LAST_CPUPID_NOT_IN_PAGE_FLAGS
/**
 * folio_xchg_last_cpupid - 原子交换 folio 的最后 CPUPID（独立字段版本）
 * @folio: folio 结构体指针
 * @cpupid: 新的 CPUPID 值
 *
 * 【功能说明】
 * - 当 CPUPID 不存储在页面标志中时使用
 * - CPUPID 存储在 folio 结构体的独立字段 _last_cpupid 中
 *
 * 【实现原理】
 * - xchg: 原子交换操作
 * - 返回旧值，同时设置新值
 * - 原子操作保证在多处理器环境下的正确性
 *
 * 【C 语言知识】原子操作
 * - xchg (exchange) 是原子交换指令
 * - 在单条 CPU 指令中完成读取旧值和写入新值
 * - 不会被其他 CPU 的操作中断
 * - 避免竞态条件 (race condition)
 *
 * 【返回值】
 * - 旧的 CPUPID 值
 */
static inline int folio_xchg_last_cpupid(struct folio *folio, int cpupid)
{
	return xchg(&folio->_last_cpupid, cpupid & LAST_CPUPID_MASK);
}

/**
 * folio_last_cpupid - 获取 folio 的最后 CPUPID（独立字段版本）
 * @folio: folio 结构体指针
 *
 * 【功能说明】
 * - 读取 folio 的 _last_cpupid 字段
 * - 直接访问，不需要位操作
 *
 * 【返回值】
 * - 最后的 CPUPID 值
 */
static inline int folio_last_cpupid(struct folio *folio)
{
	return folio->_last_cpupid;
}

/**
 * page_cpupid_reset_last - 重置页面的最后 CPUPID（独立字段版本）
 * @page: 页面结构体指针
 *
 * 【功能说明】
 * - 将 CPUPID 重置为 -1（未设置状态）
 * - 用于页面分配时的初始化
 *
 * 【注意事项】
 * - -1 & LAST_CPUPID_MASK 得到 CPUPID 字段的全 1 值
 * - 表示该页面尚未被任何 CPU/进程访问
 */
static inline void page_cpupid_reset_last(struct page *page)
{
	page->_last_cpupid = -1 & LAST_CPUPID_MASK;
}
#else
/**
 * folio_last_cpupid - 获取 folio 的最后 CPUPID（标志位版本）
 * @folio: folio 结构体指针
 *
 * 【功能说明】
 * - 当 CPUPID 存储在页面标志中时使用
 * - 从标志位中提取 CPUPID 字段
 *
 * 【实现原理】
 * - 右移 LAST_CPUPID_PGSHIFT 位
 * - 与 LAST_CPUPID_MASK 掩码进行按位与操作
 *
 * 【返回值】
 * - 最后的 CPUPID 值
 */
static inline int folio_last_cpupid(struct folio *folio)
{
	return (folio->flags.f >> LAST_CPUPID_PGSHIFT) & LAST_CPUPID_MASK;
}

/**
 * folio_xchg_last_cpupid - 原子交换 folio 的最后 CPUPID（标志位版本）
 * @folio: folio 结构体指针
 * @cpupid: 新的 CPUPID 值
 *
 * 【功能说明】
 * - 标志位版本的 CPUPID 交换
 * - 需要更复杂的实现，因为 CPUPID 与其他标志位共享同一字段
 * - 声明为外部函数，实现在其他文件中
 *
 * 【设计原理】
 * - 需要原子地修改标志位中的特定位段
 * - 不能影响其他标志位
 * - 可能使用 cmpxchg (compare-and-exchange) 循环实现
 *
 * 【返回值】
 * - 旧的 CPUPID 值
 */
int folio_xchg_last_cpupid(struct folio *folio, int cpupid);

/**
 * page_cpupid_reset_last - 重置页面的最后 CPUPID（标志位版本）
 * @page: 页面结构体指针
 *
 * 【功能说明】
 * - 将标志位中的 CPUPID 字段设置为全 1
 * - 使用按位或操作设置相应的位
 *
 * 【实现原理】
 * - LAST_CPUPID_MASK << LAST_CPUPID_PGSHIFT 生成 CPUPID 字段的掩码
 * - |= 按位或赋值，将 CPUPID 位全部置 1
 * - 不影响其他标志位
 */
static inline void page_cpupid_reset_last(struct page *page)
{
	page->flags.f |= LAST_CPUPID_MASK << LAST_CPUPID_PGSHIFT;
}
#endif /* LAST_CPUPID_NOT_IN_PAGE_FLAGS */

/**
 * folio_xchg_access_time - 原子交换 folio 的访问时间
 * @folio: folio 结构体指针
 * @time: 新的访问时间值
 *
 * 【功能说明】
 * - 更新页面的最后访问时间
 * - 用于 NUMA 自动平衡判断页面迁移时机
 *
 * 【设计原理】为什么需要访问时间？
 * - NUMA 平衡不应该仅根据最后一次访问就迁移页面
 * - 需要考虑访问的时间间隔
 * - 频繁访问的页面迁移收益更大
 *
 * 【实现原理】
 * - 访问时间存储在 CPUPID 字段中
 * - 右移 PAGE_ACCESS_TIME_BUCKETS 位，将时间转换为粗粒度
 * - 通过 folio_xchg_last_cpupid 原子交换
 * - 返回时左移恢复原始时间刻度
 *
 * 【时间桶机制】
 * - 不存储精确时间戳，而是使用时间桶
 * - 例如：不是 1秒、2秒、3秒，而是 0-2秒、2-4秒、4-6秒
 * - 节省存储空间，对 NUMA 平衡影响不大
 *
 * 【返回值】
 * - 旧的访问时间值
 */
static inline int folio_xchg_access_time(struct folio *folio, int time)
{
	int last_time;

	last_time = folio_xchg_last_cpupid(folio,
					   time >> PAGE_ACCESS_TIME_BUCKETS);
	return last_time << PAGE_ACCESS_TIME_BUCKETS;
}

/**
 * vma_set_access_pid_bit - 设置 VMA 的访问进程 ID 位
 * @vma: VMA 结构体指针
 *
 * 【功能说明】
 * - 记录当前进程访问了该 VMA
 * - 用于 NUMA 自动平衡的多进程访问跟踪
 *
 * 【设计原理】为什么使用位图？
 * - 一个 VMA 可能被多个进程访问
 * - 不需要精确知道是哪些进程，只需要知道有多少不同的进程
 * - 使用哈希位图：将 PID 哈希到一个位
 * - 节省内存，快速查询
 *
 * 【实现原理】
 * - hash_32: 32 位哈希函数，将 PID 映射到位索引
 * - ilog2(BITS_PER_LONG): 计算位图大小的对数（位数）
 * - test_bit: 测试位是否已设置
 * - __set_bit: 设置位（非原子版本，在锁保护下使用）
 * - pids_active[1]: 当前活跃的进程 ID 位图
 *
 * 【C 语言知识】位图操作
 * - 位图 (bitmap): 用位表示集合成员关系
 * - 每个位代表一个元素是否存在
 * - 空间效率高：1 位 = 1 个元素
 *
 * 【注意事项】
 * - 哈希可能冲突，但对统计影响不大
 * - 只在 vma->numab_state 非空时操作
 */
static inline void vma_set_access_pid_bit(struct vm_area_struct *vma)
{
	unsigned int pid_bit;

	pid_bit = hash_32(current->pid, ilog2(BITS_PER_LONG));
	if (vma->numab_state && !test_bit(pid_bit, &vma->numab_state->pids_active[1])) {
		__set_bit(pid_bit, &vma->numab_state->pids_active[1]);
	}
}

/**
 * folio_use_access_time - 判断 folio 是否使用访问时间
 * @folio: folio 结构体指针
 *
 * 【功能说明】
 * - 判断 folio 是否参与基于访问时间的 NUMA 平衡
 * - 外部函数声明，实现在其他文件中
 *
 * 【设计原理】
 * - 不是所有页面都需要跟踪访问时间
 * - 例如内核页面、文件缓存页面可能不需要
 * - 只有匿名页面和某些文件页面才需要
 *
 * 【返回值】
 * - true: 使用访问时间
 * - false: 不使用访问时间
 */
bool folio_use_access_time(struct folio *folio);
#else /* !CONFIG_NUMA_BALANCING */
/**
 * 【CONFIG_NUMA_BALANCING 未启用时的存根函数】
 *
 * 【设计原理】
 * - 当内核配置中未启用 NUMA 自动平衡时
 * - 提供空实现或返回默认值
 * - 这样调用代码不需要使用 #ifdef 条件编译
 * - 编译器会优化掉这些空函数（内联优化）
 *
 * 【C 语言知识】条件编译
 * - #ifdef / #else / #endif: 预处理器指令
 * - 根据宏定义选择编译哪段代码
 * - 未选择的代码完全不编译进二进制
 */

/**
 * folio_xchg_last_cpupid - 空实现版本
 *
 * 【功能说明】
 * - 返回 folio 的节点 ID
 * - XXX 注释表示这是临时实现
 */
static inline int folio_xchg_last_cpupid(struct folio *folio, int cpupid)
{
	return folio_nid(folio); /* XXX */
}

/**
 * folio_xchg_access_time - 空实现版本
 *
 * 【功能说明】
 * - 返回 0，表示没有访问时间
 */
static inline int folio_xchg_access_time(struct folio *folio, int time)
{
	return 0;
}

/**
 * folio_last_cpupid - 空实现版本
 *
 * 【功能说明】
 * - 返回 folio 的节点 ID
 */
static inline int folio_last_cpupid(struct folio *folio)
{
	return folio_nid(folio); /* XXX */
}

/**
 * cpupid_to_nid - 空实现版本
 *
 * 【功能说明】
 * - 返回 -1，表示无效节点 ID
 */
static inline int cpupid_to_nid(int cpupid)
{
	return -1;
}

/**
 * cpupid_to_pid - 空实现版本
 *
 * 【功能说明】
 * - 返回 -1，表示无效进程 ID
 */
static inline int cpupid_to_pid(int cpupid)
{
	return -1;
}

/**
 * cpupid_to_cpu - 空实现版本
 *
 * 【功能说明】
 * - 返回 -1，表示无效 CPU ID
 */
static inline int cpupid_to_cpu(int cpupid)
{
	return -1;
}

/**
 * cpu_pid_to_cpupid - 空实现版本
 *
 * 【功能说明】
 * - 返回 -1，表示无效 CPUPID
 * - 参数名从 cpu 改为 nid，但实际不使用
 */
static inline int cpu_pid_to_cpupid(int nid, int pid)
{
	return -1;
}

/**
 * cpupid_pid_unset - 空实现版本
 *
 * 【功能说明】
 * - 始终返回 true，因为没有 NUMA 平衡时 PID 始终未设置
 */
static inline bool cpupid_pid_unset(int cpupid)
{
	return true;
}

/**
 * page_cpupid_reset_last - 空实现版本
 *
 * 【功能说明】
 * - 空函数体，什么都不做
 */
static inline void page_cpupid_reset_last(struct page *page)
{
}

/**
 * cpupid_match_pid - 空实现版本
 *
 * 【功能说明】
 * - 始终返回 false，因为没有 NUMA 平衡时不跟踪 PID
 */
static inline bool cpupid_match_pid(struct task_struct *task, int cpupid)
{
	return false;
}

/**
 * vma_set_access_pid_bit - 空实现版本
 *
 * 【功能说明】
 * - 空函数体，什么都不做
 */
static inline void vma_set_access_pid_bit(struct vm_area_struct *vma)
{
}

/**
 * folio_use_access_time - 空实现版本
 *
 * 【功能说明】
 * - 始终返回 false，因为没有 NUMA 平衡时不使用访问时间
 */
static inline bool folio_use_access_time(struct folio *folio)
{
	return false;
}
#endif /* CONFIG_NUMA_BALANCING */

#if defined(CONFIG_KASAN_SW_TAGS) || defined(CONFIG_KASAN_HW_TAGS)

/*
 * KASAN per-page tags are stored xor'ed with 0xff. This allows to avoid
 * setting tags for all pages to native kernel tag value 0xff, as the default
 * value 0x00 maps to 0xff.
 * KASAN 每页标签与 0xff 进行异或存储。这样可以避免将所有页面的标签设置为
 * 原生内核标签值 0xff，因为默认值 0x00 映射到 0xff。
 *
 * 【KASAN 说明】
 * - KASAN: Kernel Address SANitizer，内核地址消毒器
 * - 用于检测内存访问错误（越界、释放后使用等）
 * - 为每个内存区域分配标签 (tag)
 * - 访问内存时检查标签是否匹配
 *
 * 【标签存储优化】为什么异或 0xff？
 * - 内核标签值通常是 0xff
 * - 页面标志默认初始化为 0x00
 * - 0x00 ^ 0xff = 0xff，无需显式初始化
 * - 节省初始化开销
 */

/**
 * page_kasan_tag - 获取页面的 KASAN 标签
 * @page: 页面结构体指针
 *
 * 【功能说明】
 * - 从页面标志中提取 KASAN 标签
 * - 标签用于内存安全检查
 *
 * 【实现原理】
 * - 右移 KASAN_TAG_PGSHIFT 位提取标签字段
 * - 与 KASAN_TAG_MASK 掩码进行按位与操作
 * - 与 0xff 进行异或操作，恢复原始标签值
 *
 * 【C 语言知识】异或运算
 * - ^: 异或运算符，相同为 0，不同为 1
 * - x ^ 0xff ^ 0xff = x（异或两次恢复原值）
 * - 异或具有可逆性
 *
 * 【返回值】
 * - 页面的 KASAN 标签值
 */
static inline u8 page_kasan_tag(const struct page *page)
{
	u8 tag = KASAN_TAG_KERNEL;

	if (kasan_enabled()) {
		tag = (page->flags.f >> KASAN_TAG_PGSHIFT) & KASAN_TAG_MASK;
		tag ^= 0xff;
	}

	return tag;
}

/**
 * page_kasan_tag_set - 设置页面的 KASAN 标签
 * @page: 页面结构体指针
 * @tag: 要设置的标签值
 *
 * 【功能说明】
 * - 在页面标志中设置 KASAN 标签
 * - 用于内存分配时标记页面
 *
 * 【实现原理】
 * - 标签与 0xff 进行异或后存储
 * - 使用 try_cmpxchg 原子操作更新标志位
 * - 循环直到成功更新（处理并发更新）
 *
 * 【C 语言知识】CAS 操作
 * - try_cmpxchg: Compare-And-Swap，比较并交换
 * - 原子地比较内存值是否等于期望值
 * - 如果相等，写入新值并返回 true
 * - 如果不相等，更新期望值并返回 false
 * - 用于实现无锁并发算法
 *
 * 【注意事项】
 * - READ_ONCE: 防止编译器优化掉重复读取
 * - unlikely: 提示编译器 CAS 失败的情况不太可能发生
 * - do-while 循环处理并发更新的情况
 */
static inline void page_kasan_tag_set(struct page *page, u8 tag)
{
	unsigned long old_flags, flags;

	if (!kasan_enabled())
		return;

	tag ^= 0xff;
	old_flags = READ_ONCE(page->flags.f);
	do {
		flags = old_flags;
		flags &= ~(KASAN_TAG_MASK << KASAN_TAG_PGSHIFT);
		flags |= (tag & KASAN_TAG_MASK) << KASAN_TAG_PGSHIFT;
	} while (unlikely(!try_cmpxchg(&page->flags.f, &old_flags, flags)));
}

/**
 * page_kasan_tag_reset - 重置页面的 KASAN 标签为内核默认值
 * @page: 页面结构体指针
 *
 * 【功能说明】
 * - 将页面标签重置为 KASAN_TAG_KERNEL
 * - 用于页面释放时清理标签
 *
 * 【设计原理】
 * - 释放的页面应重置标签
 * - 防止释放后使用 (use-after-free) 检测漏报
 */
static inline void page_kasan_tag_reset(struct page *page)
{
	if (kasan_enabled())
		page_kasan_tag_set(page, KASAN_TAG_KERNEL);
}

#else /* CONFIG_KASAN_SW_TAGS || CONFIG_KASAN_HW_TAGS */

/**
 * 【KASAN 未启用时的存根函数】
 *
 * 【设计原理】
 * - 当 KASAN 未配置时，提供空实现
 * - 返回默认标签值 0xff
 * - 编译器会优化掉这些空函数
 */

/**
 * page_kasan_tag - 空实现版本
 *
 * 【功能说明】
 * - 返回 0xff（内核默认标签）
 */
static inline u8 page_kasan_tag(const struct page *page)
{
	return 0xff;
}

/**
 * page_kasan_tag_set - 空实现版本
 *
 * 【功能说明】
 * - 空函数体，什么都不做
 */
static inline void page_kasan_tag_set(struct page *page, u8 tag) { }

/**
 * page_kasan_tag_reset - 空实现版本
 *
 * 【功能说明】
 * - 空函数体，什么都不做
 */
static inline void page_kasan_tag_reset(struct page *page) { }

#endif /* CONFIG_KASAN_SW_TAGS || CONFIG_KASAN_HW_TAGS */

/**
 * page_zone - 获取页面所属的内存区域
 * @page: 页面结构体指针
 *
 * 【功能说明】
 * - 获取页面所属的 zone（内存区域）
 * - zone 是内存管理的基本单位
 *
 * 【内存区域说明】
 * - Linux 将物理内存划分为多个 zone
 * - 常见 zone: ZONE_DMA, ZONE_NORMAL, ZONE_HIGHMEM
 * - 不同 zone 有不同的使用限制
 *
 * 【实现原理】
 * - 先通过 page_to_nid 获取页面所属的 NUMA 节点
 * - NODE_DATA 获取节点的 pg_data_t 结构体
 * - 再通过 page_zonenum 获取区域编号
 * - 从 node_zones 数组中取出对应的 zone
 *
 * 【C 语言知识】结构体成员访问
 * - ->: 指针访问成员运算符
 * - []: 数组下标运算符
 * - 链式访问：ptr->member1[index]->member2
 *
 * 【返回值】
 * - 指向 zone 结构体的指针
 */
static inline struct zone *page_zone(const struct page *page)
{
	return &NODE_DATA(page_to_nid(page))->node_zones[page_zonenum(page)];
}

/**
 * page_pgdat - 获取页面所属的节点数据结构
 * @page: 页面结构体指针
 *
 * 【功能说明】
 * - 获取页面所属 NUMA 节点的 pg_data_t 结构体
 * - pg_data_t 包含节点的所有内存管理信息
 *
 * 【pg_data_t 说明】
 * - pg_data_t: Per-Node Data，每个 NUMA 节点的数据
 * - 包含该节点的所有 zone
 * - 包含该节点的空闲页面列表
 * - 包含该节点的内存统计信息
 *
 * 【返回值】
 * - 指向 pg_data_t 结构体的指针
 */
static inline pg_data_t *page_pgdat(const struct page *page)
{
	return NODE_DATA(page_to_nid(page));
}

/**
 * folio_pgdat - 获取 folio 所属的节点数据结构
 * @folio: folio 结构体指针
 *
 * 【功能说明】
 * - folio 版本的 page_pgdat
 *
 * 【返回值】
 * - 指向 pg_data_t 结构体的指针
 */
static inline pg_data_t *folio_pgdat(const struct folio *folio)
{
	return NODE_DATA(folio_nid(folio));
}

/**
 * folio_zone - 获取 folio 所属的内存区域
 * @folio: folio 结构体指针
 *
 * 【功能说明】
 * - folio 版本的 page_zone
 * - 先获取节点数据，再通过区域编号获取 zone
 *
 * 【返回值】
 * - 指向 zone 结构体的指针
 */
static inline struct zone *folio_zone(const struct folio *folio)
{
	return &folio_pgdat(folio)->node_zones[folio_zonenum(folio)];
}

#ifdef SECTION_IN_PAGE_FLAGS
/**
 * set_page_section - 设置页面的内存段号
 * @page: 页面结构体指针
 * @section: 内存段号
 *
 * 【功能说明】
 * - 在页面标志中设置内存段号
 * - 用于稀疏内存模型
 *
 * 【稀疏内存模型说明】
 * - 物理内存可能不连续（有空洞）
 * - 将内存划分为多个 section（段）
 * - 每个 section 通常是 128MB
 * - 通过段号快速定位内存块
 *
 * 【实现原理】
 * - 先清除旧的段号字段（与取反后的掩码进行按位与）
 * - 再设置新的段号（与掩码后左移，再按位或）
 * - 不影响标志位中的其他字段
 *
 * 【C 语言知识】位域清除和设置
 * - &= ~(mask << shift): 清除字段（将相应位置 0）
 * - |= (value & mask) << shift: 设置字段（将相应位置 1）
 */
static inline void set_page_section(struct page *page, unsigned long section)
{
	page->flags.f &= ~(SECTIONS_MASK << SECTIONS_PGSHIFT);
	page->flags.f |= (section & SECTIONS_MASK) << SECTIONS_PGSHIFT;
}

/**
 * memdesc_section - 获取内存描述符的段号
 * @mdf: 内存描述符标志
 *
 * 【功能说明】
 * - 从内存描述符标志中提取段号
 *
 * 【实现原理】
 * - 右移 SECTIONS_PGSHIFT 位
 * - 与 SECTIONS_MASK 掩码进行按位与操作
 *
 * 【返回值】
 * - 段号
 */
static inline unsigned long memdesc_section(memdesc_flags_t mdf)
{
	return (mdf.f >> SECTIONS_PGSHIFT) & SECTIONS_MASK;
}
#else /* !SECTION_IN_PAGE_FLAGS */
/**
 * memdesc_section - 空实现版本
 *
 * 【功能说明】
 * - 当段号不存储在页面标志中时
 * - 返回 0（无段号信息）
 *
 * 【返回值】
 * - 0
 */
static inline unsigned long memdesc_section(memdesc_flags_t mdf)
{
	return 0;
}
#endif /* SECTION_IN_PAGE_FLAGS */

/**
 * folio_pfn - Return the Page Frame Number of a folio.
 * 返回 folio 的页帧号。
 * @folio: The folio.
 *         folio 指针
 *
 * A folio may contain multiple pages.  The pages have consecutive
 * Page Frame Numbers.
 * folio 可能包含多个页面。这些页面具有连续的页帧号。
 *
 * 【页帧号说明】PFN (Page Frame Number)
 * - 物理内存被划分为固定大小的页帧（page frame）
 * - 每个页帧有唯一的编号，称为页帧号
 * - PFN 是物理地址到页面的映射单位
 * - 物理地址 = PFN × PAGE_SIZE
 *
 * 【设计原理】
 * - folio 中的多个页面在物理内存中是连续的
 * - 只需返回第一个页面的 PFN
 * - 其他页面的 PFN = 第一个页面 PFN + 页面偏移
 *
 * Return: The Page Frame Number of the first page in the folio.
 *         folio 中第一个页面的页帧号。
 */
static inline unsigned long folio_pfn(const struct folio *folio)
{
	return page_to_pfn(&folio->page);
}

/**
 * pfn_folio - 从页帧号获取 folio
 * @pfn: 页帧号
 *
 * 【功能说明】
 * - 将页帧号转换为 folio 指针
 * - 与 folio_pfn 相反的操作
 *
 * 【实现原理】
 * - pfn_to_page: 页帧号转换为 page 指针
 * - page_folio: page 转换为 folio 指针
 *
 * 【返回值】
 * - 指向 folio 的指针
 */
static inline struct folio *pfn_folio(unsigned long pfn)
{
	return page_folio(pfn_to_page(pfn));
}

#ifdef CONFIG_MMU
/**
 * mk_pte - 创建页表项 (PTE)
 * @page: 页面结构体指针
 * @pgprot: 页面保护位
 *
 * 【功能说明】
 * - 为页面创建页表项
 * - PTE 用于虚拟地址到物理地址的映射
 *
 * 【页表项说明】PTE (Page Table Entry)
 * - 存储在页表中的条目
 * - 包含物理页帧号 (PFN)
 * - 包含页面保护位（可读、可写、可执行等）
 * - CPU 通过页表项进行地址转换
 *
 * 【页面保护位】pgprot_t
 * - 控制页面的访问权限
 * - 例如：只读、可写、可执行、缓存属性等
 * - 由硬件 MMU 强制执行
 *
 * 【实现原理】
 * - page_to_pfn: 获取页面的物理页帧号
 * - pfn_pte: 将 PFN 和保护位组合成 PTE
 *
 * 【C 语言知识】MMU
 * - MMU: Memory Management Unit，内存管理单元
 * - 硬件组件，负责虚拟地址到物理地址转换
 * - 通过页表实现地址映射
 *
 * 【返回值】
 * - 页表项 (pte_t)
 */
static inline pte_t mk_pte(const struct page *page, pgprot_t pgprot)
{
	return pfn_pte(page_to_pfn(page), pgprot);
}

/**
 * folio_mk_pte - Create a PTE for this folio
 *                为 folio 创建页表项
 * @folio: The folio to create a PTE for
 *         要创建 PTE 的 folio
 * @pgprot: The page protection bits to use
 *          要使用的页面保护位
 *
 * Create a page table entry for the first page of this folio.
 * This is suitable for passing to set_ptes().
 * 为 folio 的第一个页面创建页表项。
 * 这适合传递给 set_ptes()。
 *
 * 【功能说明】
 * - folio 版本的 mk_pte
 * - 为 folio 的第一个页面创建页表项
 * - 用于映射 folio 到虚拟地址空间
 *
 * 【设计原理】
 * - folio 可能包含多个连续页面
 * - 只需为第一个页面创建 PTE
 * - set_ptes() 会处理后续连续页面
 *
 * Return: A page table entry suitable for mapping this folio.
 *         适合映射此 folio 的页表项。
 */
static inline pte_t folio_mk_pte(const struct folio *folio, pgprot_t pgprot)
{
	return pfn_pte(folio_pfn(folio), pgprot);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/**
 * folio_mk_pmd - Create a PMD for this folio
 *                为 folio 创建 PMD（页中间目录项）
 * @folio: The folio to create a PMD for
 *         要创建 PMD 的 folio
 * @pgprot: The page protection bits to use
 *          要使用的页面保护位
 *
 * Create a page table entry for the first page of this folio.
 * This is suitable for passing to set_pmd_at().
 * 为 folio 的第一个页面创建页表项。
 * 这适合传递给 set_pmd_at()。
 *
 * 【PMD 说明】PMD (Page Middle Directory)
 * - 多级页表中的中间目录
 * - 用于映射大页（Huge Page）
 * - 典型大小：2MB（x86-64 架构）
 * - 减少页表层级，提高 TLB 命中率
 *
 * 【透明大页】Transparent Huge Pages
 * - 自动将小页合并为大页
 * - 对应用程序透明，无需修改代码
 * - 减少页表项数量，节省内存
 * - 提高 TLB 利用率，加速地址转换
 *
 * 【实现原理】
 * - folio_pfn: 获取 folio 的页帧号
 * - pfn_pmd: 创建 PMD 项
 * - pmd_mkhuge: 标记为大页 PMD
 *
 * 【设计原理】为什么需要 pmd_mkhuge？
 * - 需要在 PMD 中设置大页标志位
 * - CPU 通过此标志识别这是大页映射
 * - 不同架构有不同的大页标志位置
 *
 * Return: A page table entry suitable for mapping this folio.
 *         适合映射此 folio 的页表项。
 */
static inline pmd_t folio_mk_pmd(const struct folio *folio, pgprot_t pgprot)
{
	return pmd_mkhuge(pfn_pmd(folio_pfn(folio), pgprot));
}

#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
/**
 * folio_mk_pud - Create a PUD for this folio
 *                为 folio 创建 PUD（页上级目录项）
 * @folio: The folio to create a PUD for
 *         要创建 PUD 的 folio
 * @pgprot: The page protection bits to use
 *          要使用的页面保护位
 *
 * Create a page table entry for the first page of this folio.
 * This is suitable for passing to set_pud_at().
 * 为 folio 的第一个页面创建页表项。
 * 这适合传递给 set_pud_at()。
 *
 * 【PUD 说明】PUD (Page Upper Directory)
 * - 多级页表中的上级目录
 * - 用于映射超大页（Gigantic Page）
 * - 典型大小：1GB（x86-64 架构）
 * - 进一步减少页表层级
 *
 * 【架构支持】
 * - 需要硬件架构支持
 * - x86-64 的某些 CPU 支持 1GB 大页
 * - ARM64 也支持多种大页尺寸
 * - CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD 宏控制
 *
 * 【使用场景】
 * - 大内存数据库
 * - 大规模科学计算
 * - 虚拟化 hypervisor
 * - 减少页表遍历层数
 *
 * Return: A page table entry suitable for mapping this folio.
 *         适合映射此 folio 的页表项。
 */
static inline pud_t folio_mk_pud(const struct folio *folio, pgprot_t pgprot)
{
	return pud_mkhuge(pfn_pud(folio_pfn(folio), pgprot));
}
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif /* CONFIG_MMU */

/**
 * folio_has_pincount - 判断 folio 是否有独立的 pin 计数
 * @folio: folio 结构体指针
 *
 * 【功能说明】
 * - 判断 folio 是否使用独立的 _pincount 字段跟踪 pin 计数
 * - 不同大小的 folio 使用不同的 pin 计数方法
 *
 * 【设计原理】为什么大 folio 需要独立 pin 计数？
 * - 小 folio：使用 GUP_PIN_COUNTING_BIAS 在引用计数中编码 pin 计数
 * - 大 folio：有足够空间存储独立的 _pincount 字段
 * - 独立字段更精确，避免引用计数溢出问题
 *
 * 【实现原理】
 * - 64 位系统：所有 large folio 都有独立 pin 计数
 * - 32 位系统：只有 order > 1 的 folio 有独立 pin 计数
 * - IS_ENABLED: 编译时判断配置
 *
 * 【C 语言知识】IS_ENABLED 宏
 * - 编译时求值的宏
 * - 检查内核配置选项是否启用
 * - 编译器会优化掉未启用分支的代码
 *
 * 【返回值】
 * - true: 使用独立 _pincount 字段
 * - false: 使用引用计数中的 GUP_PIN_COUNTING_BIAS
 */
static inline bool folio_has_pincount(const struct folio *folio)
{
	if (IS_ENABLED(CONFIG_64BIT))
		return folio_test_large(folio);
	return folio_order(folio) > 1;
}

/**
 * folio_maybe_dma_pinned - Report if a folio may be pinned for DMA.
 *                          报告 folio 是否可能为 DMA 而被 pinned
 * @folio: The folio.
 *         folio 指针
 *
 * This function checks if a folio has been pinned via a call to
 * a function in the pin_user_pages() family.
 * 此函数检查 folio 是否已通过 pin_user_pages() 系列函数被 pinned。
 *
 * 【DMA Pinning 说明】
 * - DMA: Direct Memory Access，直接内存访问
 * - DMA 设备需要访问物理内存而不通过 CPU
 * - Pin：固定页面在物理内存中，防止被交换出去或迁移
 * - 确保 DMA 传输期间物理地址不变
 *
 * 【为什么需要 Pin？】
 * - DMA 设备使用物理地址访问内存
 * - 如果页面被交换到磁盘，DMA 会访问错误地址
 * - 如果页面被 NUMA 平衡迁移，DMA 会访问旧地址
 * - Pin 保证页面在 DMA 期间保持原地
 *
 * For small folios, the return value is partially fuzzy: false is not fuzzy,
 * because it means "definitely not pinned for DMA", but true means "probably
 * pinned for DMA, but possibly a false positive due to having at least
 * GUP_PIN_COUNTING_BIAS worth of normal folio references".
 * 对于小 folio，返回值是部分模糊的：false 不模糊，因为它意味着"肯定没有为 DMA 而 pinned"，
 * 但 true 意味着"可能为 DMA 而 pinned，但也可能是误报，因为至少有 GUP_PIN_COUNTING_BIAS
 * 那么多的普通 folio 引用"。
 *
 * 【小 folio 的模糊性】
 * - 使用引用计数编码 pin 计数
 * - 如果引用计数 >= GUP_PIN_COUNTING_BIAS，可能是：
 *   1. 真的被 pinned 了
 *   2. 只是有很多普通引用（误报）
 * - 误报概率低，且调用者可以优雅处理
 *
 * False positives are OK, because: a) it's unlikely for a folio to
 * get that many refcounts, and b) all the callers of this routine are
 * expected to be able to deal gracefully with a false positive.
 * 误报是可以接受的，因为：a) folio 不太可能获得那么多引用计数，
 * b) 此函数的所有调用者都应该能够优雅地处理误报。
 *
 * For most large folios, the result will be exactly correct. That's because
 * we have more tracking data available: the _pincount field is used
 * instead of the GUP_PIN_COUNTING_BIAS scheme.
 * 对于大多数大 folio，结果将完全正确。这是因为我们有更多可用的跟踪数据：
 * 使用 _pincount 字段而不是 GUP_PIN_COUNTING_BIAS 方案。
 *
 * 【大 folio 的精确性】
 * - 使用独立的 _pincount 字段
 * - 不会与普通引用计数混淆
 * - 返回值是精确的，没有误报
 *
 * For more information, please see Documentation/core-api/pin_user_pages.rst.
 * 更多信息，请参见 Documentation/core-api/pin_user_pages.rst。
 *
 * 【实现原理】
 * - 如果有独立 _pincount：检查 _pincount > 0
 * - 如果没有独立 _pincount：检查引用计数 >= GUP_PIN_COUNTING_BIAS
 * - atomic_read: 原子读取 _pincount
 * - folio_ref_count: 获取 folio 引用计数
 *
 * 【C 语言知识】有符号数溢出处理
 * - folio_ref_count() 返回有符号整数
 * - 如果引用计数溢出，返回负值
 * - 转换为无符号整数可以正确比较
 * - 利用符号位提高计数范围
 *
 * Return: True, if it is likely that the folio has been "dma-pinned".
 *         False, if the folio is definitely not dma-pinned.
 *         如果 folio 可能被 "dma-pinned"，则返回 True。
 *         如果 folio 肯定没有被 dma-pinned，则返回 False。
 */
static inline bool folio_maybe_dma_pinned(struct folio *folio)
{
	if (folio_has_pincount(folio))
		return atomic_read(&folio->_pincount) > 0;

	/*
	 * folio_ref_count() is signed. If that refcount overflows, then
	 * folio_ref_count() returns a negative value, and callers will avoid
	 * further incrementing the refcount.
	 * folio_ref_count() 是有符号的。如果该引用计数溢出，
	 * 则 folio_ref_count() 返回负值，调用者将避免进一步增加引用计数。
	 *
	 * Here, for that overflow case, use the sign bit to count a little
	 * bit higher via unsigned math, and thus still get an accurate result.
	 * 在这里，对于溢出情况，通过无符号数学使用符号位计数得更高一些，
	 * 从而仍然获得准确的结果。
	 */
	return ((unsigned int)folio_ref_count(folio)) >=
		GUP_PIN_COUNTING_BIAS;
}

/*
 * This should most likely only be called during fork() to see whether we
 * should break the cow immediately for an anon page on the src mm.
 * 这最有可能仅在 fork() 期间调用，以查看我们是否应该立即为 src mm 上的匿名页面
 * 打破写时复制 (COW)。
 *
 * The caller has to hold the PT lock and the vma->vm_mm->->write_protect_seq.
 * 调用者必须持有页表锁和 vma->vm_mm->write_protect_seq。
 *
 * 【fork() 与 COW 说明】
 * - fork() 创建子进程时，父子进程共享页面
 * - 页面被标记为只读，实现写时复制 (Copy-On-Write)
 * - 当任一进程写入时，内核复制页面
 * - 这样避免了 fork() 时的大量内存复制
 *
 * 【DMA Pinned 页面的特殊处理】
 * - 如果页面被 DMA pinned，不能使用 COW
 * - DMA 设备需要访问原始物理页面
 * - 如果使用 COW，DMA 可能访问到错误的页面
 * - 因此必须立即复制页面，打破共享
 *
 * 【为什么需要立即打破 COW？】
 * - DMA 写入会影响父进程的页面
 * - 或者 DMA 读取可能读到子进程修改后的数据
 * - 立即复制确保父子进程的页面独立
 */

/**
 * folio_needs_cow_for_dma - 判断 folio 是否因 DMA 需要打破 COW
 * @vma: VMA 结构体指针
 * @folio: folio 结构体指针
 *
 * 【功能说明】
 * - 在 fork() 期间判断是否需要立即复制页面
 * - 如果页面被 DMA pinned，则需要打破 COW
 *
 * 【锁保护要求】
 * - 必须持有页表锁 (PT lock)
 * - 必须持有 write_protect_seq 序列锁
 * - VM_BUG_ON 检查序列计数为奇数（表示正在写入）
 *
 * 【实现原理】
 * - 检查 mm 是否设置了 MMF_HAS_PINNED 标志
 * - 如果没有 pinned 页面，快速返回 false
 * - 如果可能有 pinned 页面，调用 folio_maybe_dma_pinned 检查
 *
 * 【C 语言知识】序列锁
 * - raw_read_seqcount: 读取序列计数
 * - & 1: 检查最低位，奇数表示正在写入
 * - 用于检测并发修改
 *
 * 【优化设计】
 * - MMF_HAS_PINNED: 快速路径优化
 * - 大多数进程没有 pinned 页面
 * - 快速检查标志避免昂贵的 folio 检查
 *
 * 【返回值】
 * - true: 需要立即打破 COW
 * - false: 可以使用正常的 COW 机制
 */
static inline bool folio_needs_cow_for_dma(struct vm_area_struct *vma,
					  struct folio *folio)
{
	VM_BUG_ON(!(raw_read_seqcount(&vma->vm_mm->write_protect_seq) & 1));

	if (!mm_flags_test(MMF_HAS_PINNED, vma->vm_mm))
		return false;

	return folio_maybe_dma_pinned(folio);
}

/**
 * is_zero_page - Query if a page is a zero page
 *                查询页面是否是零页
 * @page: The page to query
 *        要查询的页面
 *
 * This returns true if @page is one of the permanent zero pages.
 * 如果 @page 是永久零页之一，则返回 true。
 *
 * 【零页说明】Zero Page
 * - 内核维护的特殊只读页面，内容全为 0
 * - 用于优化内存分配和初始化
 * - 多个虚拟地址可以映射到同一个零页
 * - 节省物理内存
 *
 * 【使用场景】
 * - 匿名页面初始化：新分配的匿名页面映射到零页
 * - 写时复制：第一次写入时才分配真实页面
 * - BSS 段：程序的未初始化全局变量区域
 * - 大量零值数据的稀疏表示
 *
 * 【设计原理】为什么使用零页？
 * - 避免为初始值为 0 的页面分配物理内存
 * - 延迟分配：只在实际写入时分配
 * - 减少内存碎片
 * - 加速进程启动
 *
 * 【实现原理】
 * - page_to_pfn: 获取页面的页帧号
 * - is_zero_pfn: 检查 PFN 是否是零页的 PFN
 * - 内核维护零页的 PFN 集合
 *
 * 【注意事项】
 * - 零页是只读的
 * - 写入时会触发页面错误
 * - 内核会分配新页面并复制零内容
 * - 然后建立可写映射
 *
 * 【返回值】
 * - true: 是零页
 * - false: 不是零页
 */
static inline bool is_zero_page(const struct page *page)
{
	return is_zero_pfn(page_to_pfn(page));
}

/**
 * is_zero_folio - Query if a folio is a zero page
 * @folio: The folio to query
 *
 * This returns true if @folio is one of the permanent zero pages.
 *
 * 【功能说明】检查 folio 是否是零页
 *
 * 【设计原理】为什么需要 folio 版本的零页检查？
 * - folio 是页面管理的高级抽象
 * - 提供类型安全的接口
 * - 与现代内存管理 API 一致
 * - 避免直接操作 struct page
 *
 * 【实现原理】
 * - 通过 &folio->page 获取基础页面结构
 * - 调用 is_zero_page() 进行实际检查
 * - folio 的第一个成员就是 struct page
 *
 * 【C语言知识】结构体成员访问
 * - folio->page: 访问 folio 结构的 page 成员
 * - &folio->page: 获取该成员的地址
 * - struct folio 的第一个字段是 struct page
 *
 * 【参数说明】
 * @folio: 要检查的 folio
 *        - 类型：const struct folio *（只读指针）
 *        - const 表示不会修改 folio 内容
 *
 * 【返回值】
 * - true: 是零页
 * - false: 不是零页
 */
static inline bool is_zero_folio(const struct folio *folio)
{
	return is_zero_page(&folio->page);
}

/* MIGRATE_CMA and ZONE_MOVABLE do not allow pin folios */
/* MIGRATE_CMA 和 ZONE_MOVABLE 不允许长期固定 folio */
#ifdef CONFIG_MIGRATION
/**
 * folio_is_longterm_pinnable - 检查 folio 是否可以长期固定
 * @folio: 要检查的 folio
 *
 * 【功能说明】判断一个 folio 是否适合长期 DMA 固定
 *
 * 【设计原理】为什么需要限制长期固定？
 * - 长期固定会阻止页面迁移和回收
 * - 某些内存区域需要动态管理
 * - 平衡性能和灵活性
 * - 防止内存碎片化
 *
 * 【不允许长期固定的页面类型】
 * 1. MIGRATE_CMA 页面
 *    - Contiguous Memory Allocator 管理的页面
 *    - 用于需要连续物理内存的设备（如相机、视频编解码器）
 *    - 必须能够被迁移以满足 CMA 分配请求
 *    - 长期固定会破坏 CMA 的连续性保证
 *
 * 2. MIGRATE_ISOLATE 页面
 *    - 正在被隔离的页面（如热插拔、内存故障处理）
 *    - 不应该被新的固定操作使用
 *    - 避免干扰隔离过程
 *
 * 3. ZONE_MOVABLE 页面
 *    - 专门用于可移动内存的区域
 *    - 支持内存热插拔
 *    - 长期固定会阻止内存移除
 *
 * 4. 设备一致性内存（Device Coherent Memory）
 *    - 设备本地内存，但 CPU 可以访问
 *    - 必须允许驱逐以便设备使用
 *    - 不适合长期固定
 *
 * 5. FS-DAX 页面
 *    - 文件系统直接访问（Direct Access）页面
 *    - 映射持久性内存设备
 *    - 文件系统需要能够截断（truncate）和打洞（hole-punch）
 *    - 只能容忍短暂延迟
 *
 * 【特殊情况】允许长期固定的页面
 * - 零页：虽然可以"固定"，但有特殊处理
 *   * 零页是只读的，不会真正被修改
 *   * 写时会触发 COW，分配新页面
 *   * 因此固定零页不会造成问题
 *
 * 【C语言知识】条件编译
 * - #ifdef CONFIG_CMA: 如果启用了 CMA 支持
 * - #ifdef CONFIG_MIGRATION: 如果启用了页面迁移
 * - #else: 否则
 * - #endif: 结束条件编译块
 *
 * 【注意事项】
 * - 短期固定（transient pin）通常是允许的
 * - 长期固定可能导致内存碎片
 * - 页面迁移失败会影响系统稳定性
 * - DMA 操作完成后应尽快解除固定
 *
 * 【返回值】
 * - true: 可以长期固定
 * - false: 不能长期固定
 */
static inline bool folio_is_longterm_pinnable(struct folio *folio)
{
#ifdef CONFIG_CMA
	int mt = folio_migratetype(folio);

	if (mt == MIGRATE_CMA || mt == MIGRATE_ISOLATE)
		return false;
#endif
	/* The zero page can be "pinned" but gets special handling. */
	/* 零页可以被"固定"，但有特殊处理 */
	if (is_zero_folio(folio))
		return true;

	/* Coherent device memory must always allow eviction. */
	/* 一致性设备内存必须始终允许驱逐 */
	if (folio_is_device_coherent(folio))
		return false;

	/*
	 * Filesystems can only tolerate transient delays to truncate and
	 * hole-punch operations
	 */
	/*
	 * 文件系统只能容忍截断和打洞操作的短暂延迟
	 * - FS-DAX: 文件系统直接访问持久性内存
	 * - truncate: 截断文件，释放超出部分的页面
	 * - hole-punch: 在文件中打洞，释放中间的页面
	 * - 长期固定会阻止这些操作
	 */
	if (folio_is_fsdax(folio))
		return false;

	/* Otherwise, non-movable zone folios can be pinned. */
	/* 否则，非可移动区域的 folio 可以被固定 */
	return !folio_is_zone_movable(folio);

}
#else
/**
 * folio_is_longterm_pinnable - 检查 folio 是否可以长期固定（无迁移支持版本）
 * @folio: 要检查的 folio
 *
 * 【功能说明】当未启用页面迁移时，所有页面都可以长期固定
 *
 * 【设计原理】为什么在无迁移支持时返回 true？
 * - 没有页面迁移功能，不存在迁移冲突
 * - 不需要支持内存热插拔
 * - 简化的内存管理模型
 * - 所有页面都可以安全固定
 *
 * 【返回值】
 * - true: 始终返回 true（无迁移支持时）
 */
static inline bool folio_is_longterm_pinnable(struct folio *folio)
{
	return true;
}
#endif

/**
 * set_page_zone - 设置页面所属的内存区域
 * @page: 要设置的页面
 * @zone: 内存区域类型
 *
 * 【功能说明】在页面标志中设置内存区域信息
 *
 * 【设计原理】为什么需要在页面中存储区域信息？
 * - 快速查询页面所属区域，无需遍历
 * - 支持多区域内存管理（DMA、Normal、HighMem）
 * - 优化内存分配和回收路径
 * - 减少指针间接访问
 *
 * 【内存区域类型】Zone Types
 * - ZONE_DMA: 用于 DMA 操作的低端内存（通常 <16MB）
 * - ZONE_DMA32: 32位 DMA 可访问的内存（<4GB）
 * - ZONE_NORMAL: 常规内存，内核直接映射
 * - ZONE_HIGHMEM: 高端内存（32位系统）
 * - ZONE_MOVABLE: 可移动内存，支持热插拔
 * - ZONE_DEVICE: 设备内存（如 GPU 内存）
 *
 * 【实现原理】位域操作
 * 1. page->flags.f &= ~(ZONES_MASK << ZONES_PGSHIFT)
 *    - 清除现有的区域位
 *    - ZONES_MASK: 区域字段的掩码（如 0x3 表示 2 位）
 *    - ZONES_PGSHIFT: 区域字段在 flags 中的起始位
 *    - ~(...): 按位取反，用于清除操作
 *
 * 2. page->flags.f |= (zone & ZONES_MASK) << ZONES_PGSHIFT
 *    - 设置新的区域值
 *    - zone & ZONES_MASK: 确保区域值在有效范围内
 *    - << ZONES_PGSHIFT: 移位到正确的位置
 *    - |=: 按位或，设置新位
 *
 * 【C语言知识】位操作
 * - &=: 按位与赋值，用于清除位
 * - |=: 按位或赋值，用于设置位
 * - ~: 按位取反
 * - <<: 左移运算符
 * - &: 按位与，用于提取位
 *
 * 【参数说明】
 * @page: 目标页面结构
 * @zone: 区域类型（enum zone_type）
 *        - 必须是有效的区域类型枚举值
 *
 * 【注意事项】
 * - 此函数不做边界检查
 * - 调用者必须确保 zone 值有效
 * - 通常在页面初始化时调用
 * - 页面区域一般不会改变
 */
static inline void set_page_zone(struct page *page, enum zone_type zone)
{
	page->flags.f &= ~(ZONES_MASK << ZONES_PGSHIFT);
	page->flags.f |= (zone & ZONES_MASK) << ZONES_PGSHIFT;
}

/**
 * set_page_node - 设置页面所属的 NUMA 节点
 * @page: 要设置的页面
 * @node: NUMA 节点编号
 *
 * 【功能说明】在页面标志中设置 NUMA 节点信息
 *
 * 【设计原理】为什么需要在页面中存储节点信息？
 * - NUMA 系统中快速查询页面所属节点
 * - 优化内存访问性能（本地节点访问更快）
 * - 支持 NUMA 感知的内存分配
 * - 辅助 NUMA 平衡和页面迁移
 *
 * 【NUMA 架构】Non-Uniform Memory Access
 * - 每个 CPU 有本地内存节点
 * - 访问本地内存快，访问远程内存慢
 * - 节点编号标识物理内存位置
 * - 内核尽量从本地节点分配内存
 *
 * 【实现原理】位域操作
 * 1. page->flags.f &= ~(NODES_MASK << NODES_PGSHIFT)
 *    - 清除现有的节点位
 *    - NODES_MASK: 节点字段的掩码（如 0xFF 表示 8 位，支持 256 节点）
 *    - NODES_PGSHIFT: 节点字段在 flags 中的起始位
 *
 * 2. page->flags.f |= (node & NODES_MASK) << NODES_PGSHIFT
 *    - 设置新的节点编号
 *    - node & NODES_MASK: 确保节点编号在有效范围内
 *    - << NODES_PGSHIFT: 移位到正确的位置
 *
 * 【参数说明】
 * @page: 目标页面结构
 * @node: NUMA 节点编号（unsigned long）
 *        - 通常是 0 到 MAX_NUMNODES-1
 *        - MAX_NUMNODES 可达 1024（大型系统）
 *
 * 【注意事项】
 * - 节点编号在页面生命周期中通常不变
 * - 页面迁移时会更新节点编号
 * - 与 set_page_zone 配合使用
 * - NODES_MASK 限制了最大节点数
 */
static inline void set_page_node(struct page *page, unsigned long node)
{
	page->flags.f &= ~(NODES_MASK << NODES_PGSHIFT);
	page->flags.f |= (node & NODES_MASK) << NODES_PGSHIFT;
}

/**
 * set_page_links - 设置页面的区域、节点和内存段信息
 * @page: 要设置的页面
 * @zone: 内存区域类型
 * @node: NUMA 节点编号
 * @pfn: 页帧号（Page Frame Number）
 *
 * 【功能说明】一次性设置页面的所有位置信息
 *
 * 【设计原理】为什么需要组合设置函数？
 * - 页面初始化时需要同时设置多个字段
 * - 减少重复的位操作代码
 * - 提供统一的初始化接口
 * - 在某些配置下还需设置内存段信息
 *
 * 【调用时机】
 * - 页面分配器初始化新页面时
 * - 伙伴系统分配页面时
 * - 内存热插拔添加新内存时
 * - 页面迁移到新位置时
 *
 * 【实现步骤】
 * 1. set_page_zone(page, zone)
 *    - 设置页面所属的内存区域
 *    - 如 ZONE_NORMAL、ZONE_DMA
 *
 * 2. set_page_node(page, node)
 *    - 设置页面所属的 NUMA 节点
 *    - 用于 NUMA 感知的内存管理
 *
 * 3. set_page_section(page, pfn_to_section_nr(pfn))
 *    - 仅在 SECTION_IN_PAGE_FLAGS 配置下
 *    - 设置页面所属的内存段
 *    - 用于稀疏内存模型（Sparse Memory Model）
 *
 * 【内存段】Memory Section
 * - 稀疏内存模型将物理内存划分为固定大小的段
 * - 每段通常 128MB 或 256MB
 * - 支持内存热插拔（整段添加/移除）
 * - pfn_to_section_nr: 从页帧号计算段号
 *
 * 【配置说明】SECTION_IN_PAGE_FLAGS
 * - 定义时：将段号存储在 page->flags 中
 * - 未定义时：通过 PFN 计算，不占用 flags 位
 * - 取决于系统架构和内存模型
 *
 * 【C语言知识】条件编译
 * - #ifdef SECTION_IN_PAGE_FLAGS: 如果定义了此宏
 * - #endif: 结束条件编译块
 * - 编译时决定是否包含某段代码
 *
 * 【参数说明】
 * @page: 目标页面结构
 * @zone: 内存区域类型（enum zone_type）
 * @node: NUMA 节点编号（unsigned long）
 * @pfn: 页帧号（unsigned long）
 *       - 物理地址 / PAGE_SIZE
 *       - 唯一标识物理页面
 *
 * 【注意事项】
 * - 通常在页面初始化的早期调用
 * - 这些信息在页面生命周期中较少改变
 * - 页面迁移时需要更新这些信息
 * - 不做参数有效性检查，调用者负责
 */
static inline void set_page_links(struct page *page, enum zone_type zone,
	unsigned long node, unsigned long pfn)
{
	set_page_zone(page, zone);
	set_page_node(page, node);
#ifdef SECTION_IN_PAGE_FLAGS
	set_page_section(page, pfn_to_section_nr(pfn));
#endif
}

/**
 * folio_nr_pages - The number of pages in the folio.
 * @folio: The folio.
 *
 * Return: A positive power of two.
 *
 * 【功能说明】获取 folio 包含的页面数量
 *
 * 【设计原理】为什么需要此函数？
 * - folio 可以是单页或多页（复合页）
 * - 统一接口处理不同大小的 folio
 * - 避免调用者直接检查 folio 类型
 * - 支持透明大页和巨型页
 *
 * 【Folio 大小】
 * - 小 folio: 1 页（4KB）
 * - 大 folio: 2^n 页
 *   * 透明大页（THP）: 512 页（2MB）
 *   * 巨型页（Huge Page）: 131072 页（512MB）
 *   * 超大页（Gigantic Page）: 262144 页（1GB）
 *
 * 【实现原理】
 * 1. folio_test_large(folio)
 *    - 检查是否是大 folio
 *    - 测试 PG_head 标志
 *
 * 2. 小 folio: 返回 1
 *    - 只包含一个页面
 *    - 最常见的情况
 *
 * 3. 大 folio: 调用 folio_large_nr_pages(folio)
 *    - 从 folio 结构中读取实际页面数
 *    - 存储在特定字段中
 *
 * 【C语言知识】条件表达式
 * - if (!condition) return value: 如果条件为假，返回值
 * - 早期返回优化：先处理简单情况
 *
 * 【返回值】
 * - 正整数，且是 2 的幂
 * - 1: 单页 folio（4KB）
 * - 2: 2 页（8KB）
 * - 4: 4 页（16KB）
 * - 512: 透明大页（2MB）
 * - 262144: 1GB 巨型页
 *
 * 【使用场景】
 * - 计算 folio 占用的内存大小
 * - 遍历 folio 中的所有页面
 * - 内存统计和会计
 * - I/O 操作的大小计算
 *
 * 【注意事项】
 * - 返回值始终是 2 的幂
 * - 即使是尾页也应该使用头页的 folio
 * - 不要假设返回值为 1
 */
static inline unsigned long folio_nr_pages(const struct folio *folio)
{
	if (!folio_test_large(folio))
		return 1;
	return folio_large_nr_pages(folio);
}

/*
 * compound_nr() returns the number of pages in this potentially compound
 * page.  compound_nr() can be called on a tail page, and is defined to
 * return 1 in that case.
 */
/*
 * compound_nr() 返回此潜在复合页中的页面数量
 * compound_nr() 可以在尾页上调用，在这种情况下定义为返回 1
 *
 * 【功能说明】获取复合页包含的页面数量（兼容尾页调用）
 *
 * 【设计原理】为什么允许在尾页上调用？
 * - 向后兼容旧代码
 * - 某些代码路径可能不知道是头页还是尾页
 * - 尾页调用返回 1 是安全的默认行为
 * - 避免强制调用者做类型检查
 *
 * 【复合页】Compound Page
 * - 由多个连续物理页面组成
 * - 第一个页面是头页（head page）
 * - 其余页面是尾页（tail page）
 * - 用于实现大页（huge pages）
 *
 * 【实现原理】
 * 1. const struct folio *folio = (struct folio *)page
 *    - 将 page 转换为 folio 指针
 *    - 类型转换（casting）
 *    - 无论是头页还是尾页都可以转换
 *
 * 2. if (!test_bit(PG_head, &folio->flags.f))
 *    - 测试 PG_head 标志位
 *    - PG_head: 标记复合页的头页
 *    - 如果不是头页，返回 1
 *
 * 3. 如果是头页: return folio_large_nr_pages(folio)
 *    - 读取实际页面数量
 *    - 从头页的元数据中获取
 *
 * 【与 folio_nr_pages 的区别】
 * - compound_nr: 可以在任何页面上调用，尾页返回 1
 * - folio_nr_pages: 应该在 folio（头页）上调用，返回实际数量
 *
 * 【C语言知识】类型转换
 * - (struct folio *)page: 强制类型转换
 * - 将 page 指针转换为 folio 指针
 * - 两者内存布局兼容
 *
 * 【C语言知识】test_bit 函数
 * - test_bit(bit, addr): 测试地址 addr 的第 bit 位
 * - 返回 0（未设置）或非 0（已设置）
 * - 原子操作，线程安全
 *
 * 【参数说明】
 * @page: 要查询的页面
 *        - 可以是头页、尾页或普通单页
 *        - const 表示不会修改页面
 *
 * 【返回值】
 * - 头页: 返回复合页的实际页面数（2 的幂）
 * - 尾页或单页: 返回 1
 *
 * 【使用场景】
 * - 不确定页面类型时的安全查询
 * - 遍历页面时的大小计算
 * - 向后兼容的代码路径
 *
 * 【注意事项】
 * - 尾页调用返回 1，不是整个复合页的大小
 * - 如果需要复合页的真实大小，应使用头页调用
 * - 新代码应优先使用 folio_nr_pages
 */
static inline unsigned long compound_nr(const struct page *page)
{
	const struct folio *folio = (struct folio *)page;

	if (!test_bit(PG_head, &folio->flags.f))
		return 1;
	return folio_large_nr_pages(folio);
}

/**
 * folio_next - Move to the next physical folio.
 * @folio: The folio we're currently operating on.
 *
 * If you have physically contiguous memory which may span more than
 * one folio (eg a &struct bio_vec), use this function to move from one
 * folio to the next.  Do not use it if the memory is only virtually
 * contiguous as the folios are almost certainly not adjacent to each
 * other.  This is the folio equivalent to writing ``page++``.
 *
 * Context: We assume that the folios are refcounted and/or locked at a
 * higher level and do not adjust the reference counts.
 * Return: The next struct folio.
 *
 * 【功能说明】移动到下一个物理连续的 folio
 *
 * 【设计原理】为什么需要此函数？
 * - 物理连续内存可能跨越多个 folio
 * - 提供类型安全的 folio 遍历接口
 * - 相当于 page++ 的 folio 版本
 * - 处理不同大小的 folio（单页和复合页）
 *
 * 【物理连续 vs 虚拟连续】
 * - 物理连续：内存在物理地址空间中相邻
 *   * 如 DMA 缓冲区、bio_vec 的页面
 *   * 可以使用此函数遍历
 *   * 下一个 folio 在物理地址上紧邻
 *
 * - 虚拟连续：内存在虚拟地址空间中相邻
 *   * 如 vmalloc 分配的内存
 *   * 不能使用此函数
 *   * folio 在物理上可能分散
 *
 * 【使用场景】
 * - 遍历 bio_vec 的页面（块 I/O 向量）
 * - 处理物理连续的大缓冲区
 * - DMA 散列/聚集列表遍历
 * - 连续内存区域的批处理
 *
 * 【实现原理】
 * - folio 包含 1 个或多个连续物理页面
 * - 当前 folio 地址 + 当前 folio 大小 = 下一个 folio 地址
 * - 通过 folio_nr_pages() 获取当前 folio 的页面数
 * - 指针算术：folio + folio_nr_pages(folio)
 *
 * 【C语言知识】指针算术
 * - page++: 指针向后移动一个 struct page 的大小
 * - folio + n: 指针向后移动 n 个 struct folio 的大小
 * - 注意：struct folio 和 struct page 大小相同
 *
 * 【参数说明】
 * @folio: 当前正在操作的 folio
 *         - 必须是物理连续内存区域的一部分
 *
 * 【返回值】
 * - 下一个 struct folio 指针
 * - 物理地址上紧邻的 folio
 *
 * 【上下文假设】
 * - folio 已被引用计数或锁定
 * - 此函数不调整引用计数
 * - 调用者负责生命周期管理
 * - 假设存在下一个 folio（不做边界检查）
 *
 * 【注意事项】
 * - 仅用于物理连续内存
 * - 不适用于虚拟连续内存（如 vmalloc）
 * - 不检查是否越界，调用者负责
 * - 不修改引用计数
 * - 大 folio 会跳过其包含的所有页面
 *
 * 【示例】
 * 遍历物理连续的 folio：
 * for (i = 0; i < nr_folios; i++) {
 *     process_folio(folio);
 *     folio = folio_next(folio);
 * }
 */
static inline struct folio *folio_next(struct folio *folio)
{
	return (struct folio *)folio_page(folio, folio_nr_pages(folio));
}

/**
 * folio_shift - The size of the memory described by this folio.
 * @folio: The folio.
 *
 * A folio represents a number of bytes which is a power-of-two in size.
 * This function tells you which power-of-two the folio is.  See also
 * folio_size() and folio_order().
 *
 * Context: The caller should have a reference on the folio to prevent
 * it from being split.  It is not necessary for the folio to be locked.
 * Return: The base-2 logarithm of the size of this folio.
 *
 * 【功能说明】获取 folio 大小的以 2 为底的对数（位移量）
 *
 * 【设计原理】为什么返回对数而不是字节数？
 * - 内存大小总是 2 的幂
 * - 对数形式更紧凑（用整数表示）
 * - 便于位移运算（size = 1 << shift）
 * - 与硬件页表结构对齐
 *
 * 【Folio 大小的表示方式】
 * - folio_shift: 返回位移量（对数）
 * - folio_size: 返回字节数
 * - folio_order: 返回页面数的对数
 * - folio_nr_pages: 返回页面数
 *
 * 【实现原理】
 * - PAGE_SHIFT: 单页的位移量（通常是 12，表示 4KB = 2^12）
 * - folio_order(folio): folio 包含的页面数的对数
 *   * 1 页: order = 0
 *   * 2 页: order = 1
 *   * 512 页: order = 9（透明大页）
 * - folio_shift = PAGE_SHIFT + folio_order
 *   * 单页: 12 + 0 = 12（4KB = 2^12）
 *   * 透明大页: 12 + 9 = 21（2MB = 2^21）
 *   * 1GB 巨型页: 12 + 18 = 30（1GB = 2^30）
 *
 * 【C语言知识】位移运算
 * - 1 << n: 等于 2^n
 * - 1 << 12 = 4096（4KB）
 * - 1 << 21 = 2097152（2MB）
 * - 1 << 30 = 1073741824（1GB）
 *
 * 【参数说明】
 * @folio: 要查询的 folio
 *         - const 表示不会修改
 *
 * 【返回值】
 * - 以 2 为底的对数（位移量）
 * - 12: 4KB 单页
 * - 21: 2MB 透明大页
 * - 30: 1GB 巨型页
 *
 * 【上下文要求】
 * - 调用者应持有 folio 的引用，防止被拆分
 * - 不需要锁定 folio
 * - 大 folio 可能被拆分成小 folio
 *
 * 【关联函数】
 * - folio_size(folio): 返回字节数 (PAGE_SIZE << folio_order)
 * - folio_order(folio): 返回页面数的对数
 * - folio_nr_pages(folio): 返回页面数
 *
 * 【注意事项】
 * - 大 folio 在没有引用时可能被拆分
 * - 返回值在 folio 生命周期内可能改变（如果被拆分）
 */
static inline unsigned int folio_shift(const struct folio *folio)
{
	return PAGE_SHIFT + folio_order(folio);
}

/**
 * folio_size - The number of bytes in a folio.
 * @folio: The folio.
 *
 * Context: The caller should have a reference on the folio to prevent
 * it from being split.  It is not necessary for the folio to be locked.
 * Return: The number of bytes in this folio.
 *
 * 【功能说明】获取 folio 的字节大小
 *
 * 【设计原理】为什么需要此函数？
 * - 提供直观的字节数接口
 * - 简化内存大小计算
 * - 避免调用者手动计算位移
 * - 与标准 C 库的 size_t 类型一致
 *
 * 【实现原理】
 * - PAGE_SIZE: 单页大小（通常 4096 字节）
 * - folio_order(folio): folio 包含的页面数的对数
 * - PAGE_SIZE << folio_order(folio)
 *   * 相当于 PAGE_SIZE * (2^folio_order)
 *   * 相当于 PAGE_SIZE * folio_nr_pages(folio)
 *
 * 【计算示例】
 * - 单页 folio (order=0):
 *   * 4096 << 0 = 4096 字节（4KB）
 * - 2 页 folio (order=1):
 *   * 4096 << 1 = 8192 字节（8KB）
 * - 透明大页 (order=9):
 *   * 4096 << 9 = 2097152 字节（2MB）
 * - 1GB 巨型页 (order=18):
 *   * 4096 << 18 = 1073741824 字节（1GB）
 *
 * 【C语言知识】左移运算符
 * - <<: 左移运算符
 * - x << n: 相当于 x * 2^n
 * - 4096 << 9 = 4096 * 512 = 2097152
 *
 * 【C语言知识】size_t 类型
 * - size_t: 无符号整数类型
 * - 用于表示对象大小
 * - 在 64 位系统上通常是 64 位无符号整数
 * - 在 32 位系统上通常是 32 位无符号整数
 *
 * 【参数说明】
 * @folio: 要查询的 folio
 *         - const 表示不会修改
 *
 * 【返回值】
 * - folio 的字节大小（size_t）
 * - 4096: 4KB 单页
 * - 8192: 8KB（2 页）
 * - 2097152: 2MB 透明大页
 * - 1073741824: 1GB 巨型页
 *
 * 【上下文要求】
 * - 调用者应持有 folio 的引用，防止被拆分
 * - 不需要锁定 folio
 * - 大 folio 可能被拆分成小 folio
 *
 * 【使用场景】
 * - 计算 I/O 操作的大小
 * - 内存统计和会计
 * - 分配器的大小检查
 * - DMA 传输大小计算
 *
 * 【与其他函数的关系】
 * - folio_size = PAGE_SIZE * folio_nr_pages
 * - folio_size = 1 << folio_shift
 * - folio_nr_pages = folio_size / PAGE_SIZE
 *
 * 【注意事项】
 * - 大 folio 在没有引用时可能被拆分
 * - 返回值在 folio 生命周期内可能改变（如果被拆分）
 * - 总是返回 2 的幂
 */
static inline size_t folio_size(const struct folio *folio)
{
	return PAGE_SIZE << folio_order(folio);
}

/**
 * folio_maybe_mapped_shared - Whether the folio is mapped into the page
 *			       tables of more than one MM
 * @folio: The folio.
 *
 * This function checks if the folio maybe currently mapped into more than one
 * MM ("maybe mapped shared"), or if the folio is certainly mapped into a single
 * MM ("mapped exclusively").
 *
 * For KSM folios, this function also returns "mapped shared" when a folio is
 * mapped multiple times into the same MM, because the individual page mappings
 * are independent.
 *
 * For small anonymous folios and anonymous hugetlb folios, the return
 * value will be exactly correct: non-KSM folios can only be mapped at most once
 * into an MM, and they cannot be partially mapped. KSM folios are
 * considered shared even if mapped multiple times into the same MM.
 *
 * For other folios, the result can be fuzzy:
 *    #. For partially-mappable large folios (THP), the return value can wrongly
 *       indicate "mapped shared" (false positive) if a folio was mapped by
 *       more than two MMs at one point in time.
 *    #. For pagecache folios (including hugetlb), the return value can wrongly
 *       indicate "mapped shared" (false positive) when two VMAs in the same MM
 *       cover the same file range.
 *
 * Further, this function only considers current page table mappings that
 * are tracked using the folio mapcount(s).
 *
 * This function does not consider:
 *    #. If the folio might get mapped in the (near) future (e.g., swapcache,
 *       pagecache, temporary unmapping for migration).
 *    #. If the folio is mapped differently (VM_PFNMAP).
 *    #. If hugetlb page table sharing applies. Callers might want to check
 *       hugetlb_pmd_shared().
 *
 * Return: Whether the folio is estimated to be mapped into more than one MM.
 *
 * 【功能说明】判断 folio 是否被映射到多个 MM（内存描述符）的页表中
 *
 * 【设计原理】为什么需要此函数？
 * - 确定页面是共享还是独占
 * - 优化写时复制（COW）决策
 * - 辅助页面迁移和回收
 * - 支持透明大页的拆分决策
 *
 * 【核心概念】
 * - MM (Memory Management): 进程的内存描述符（struct mm_struct）
 * - Mapped shared: 映射到多个 MM 或同一 MM 的多个位置
 * - Mapped exclusively: 仅映射到一个 MM 的一个位置
 * - mapcount: 页面的映射计数
 *
 * 【KSM 特殊处理】Kernel Samepage Merging
 * - KSM: 内核合并相同内容的页面
 * - KSM folio 即使在同一 MM 中多次映射也视为共享
 * - 因为每个映射是独立的
 * - 一处写入会触发解除合并
 *
 * 【准确性说明】
 * 1. 完全准确的情况：
 *    - 小匿名 folio（非 KSM）
 *    - 匿名 hugetlb folio
 *    - 不能部分映射
 *    - 最多映射一次到一个 MM
 *
 * 2. 可能不准确的情况（误报 - false positive）：
 *    - 部分可映射的大 folio（THP）
 *      * 如果曾被超过两个 MM 映射
 *      * 即使现在已取消某些映射
 *    - 页缓存 folio（包括 hugetlb）
 *      * 同一 MM 中两个 VMA 覆盖相同文件范围
 *      * 会被误认为共享
 *
 * 【未考虑的情况】
 * - 将来可能的映射（交换缓存、页缓存、迁移临时取消映射）
 * - 特殊映射（VM_PFNMAP）
 * - Hugetlb 页表共享（需要额外检查 hugetlb_pmd_shared）
 *
 * 【实现逻辑】
 * 1. 获取 mapcount（映射计数）
 * 2. 小 folio 或 hugetlb: mapcount > 1 即为共享
 * 3. 大 folio 且无 CONFIG_MM_ID: 保守返回 true（假设共享）
 * 4. 大 folio 且 mapcount <= 1: 独占映射
 * 5. 大 folio 且 mapcount > 1: 检查 FOLIO_MM_IDS_SHARED_BITNUM 标志
 *
 * 【C语言知识】unlikely 宏
 * - unlikely(condition): 提示编译器此条件不太可能为真
 * - 优化分支预测，提升性能
 * - 用于罕见情况
 *
 * 【C语言知识】IS_ENABLED 宏
 * - IS_ENABLED(CONFIG_XXX): 检查配置选项是否启用
 * - 编译时求值
 * - 未启用的代码会被优化掉
 *
 * 【参数说明】
 * @folio: 要检查的 folio
 *
 * 【返回值】
 * - true: 可能被共享映射（映射到多个 MM）
 * - false: 独占映射（仅映射到一个 MM 的一个位置）
 *
 * 【使用场景】
 * - 写时复制决策：独占映射可以直接写入
 * - 页面迁移：共享映射需要更新所有页表
 * - 透明大页拆分：共享映射可能需要拆分
 * - 内存回收：共享映射影响回收策略
 *
 * 【注意事项】
 * - 结果可能是保守的（误报为共享）
 * - 映射的 folio 上调用会产生不稳定的结果
 * - 无锁调用可能不稳定（并发修改）
 * - 早期检测意外引用很有用
 */
static inline bool folio_maybe_mapped_shared(struct folio *folio)
{
	int mapcount = folio_mapcount(folio);

	/* Only partially-mappable folios require more care. */
	/* 只有部分可映射的 folio 需要更仔细的处理 */
	if (!folio_test_large(folio) || unlikely(folio_test_hugetlb(folio)))
		return mapcount > 1;

	/*
	 * vm_insert_page() without CONFIG_TRANSPARENT_HUGEPAGE ...
	 * simply assume "mapped shared", nobody should really care
	 * about this for arbitrary kernel allocations.
	 */
	/*
	 * 不支持透明大页时通过 vm_insert_page() 插入的页面
	 * 简单假设为"共享映射"
	 * 对于任意内核分配，没人真正关心这一点
	 */
	if (!IS_ENABLED(CONFIG_MM_ID))
		return true;

	/*
	 * A single mapping implies "mapped exclusively", even if the
	 * folio flag says something different: it's easier to handle this
	 * case here instead of on the RMAP hot path.
	 */
	/*
	 * 单个映射意味着"独占映射"，即使 folio 标志显示不同
	 * 在这里处理这种情况比在 RMAP 热路径上处理更容易
	 * RMAP: Reverse Mapping（反向映射）
	 */
	if (mapcount <= 1)
		return false;
	return test_bit(FOLIO_MM_IDS_SHARED_BITNUM, &folio->_mm_ids);
}

/**
 * folio_expected_ref_count - calculate the expected folio refcount
 * @folio: the folio
 *
 * Calculate the expected folio refcount, taking references from the pagecache,
 * swapcache, PG_private and page table mappings into account. Useful in
 * combination with folio_ref_count() to detect unexpected references (e.g.,
 * GUP or other temporary references).
 *
 * Does currently not consider references from the LRU cache. If the folio
 * was isolated from the LRU (which is the case during migration or split),
 * the LRU cache does not apply.
 *
 * Calling this function on an unmapped folio -- !folio_mapped() -- that is
 * locked will return a stable result.
 *
 * Calling this function on a mapped folio will not result in a stable result,
 * because nothing stops additional page table mappings from coming (e.g.,
 * fork()) or going (e.g., munmap()).
 *
 * Calling this function without the folio lock will also not result in a
 * stable result: for example, the folio might get dropped from the swapcache
 * concurrently.
 *
 * However, even when called without the folio lock or on a mapped folio,
 * this function can be used to detect unexpected references early (for example,
 * if it makes sense to even lock the folio and unmap it).
 *
 * The caller must add any reference (e.g., from folio_try_get()) it might be
 * holding itself to the result.
 *
 * Returns: the expected folio refcount.
 *
 * 【功能说明】计算 folio 的预期引用计数
 *
 * 【设计原理】为什么需要此函数？
 * - 检测意外的引用（如 GUP 或临时引用）
 * - 与 folio_ref_count() 结合使用
 * - 辅助调试内存泄漏
 * - 验证页面回收和迁移的前提条件
 *
 * 【引用来源】计入的引用
 * 1. 页缓存（Pagecache）：
 *    - 文件映射页面在页缓存中的引用
 *    - 每页一个引用
 *
 * 2. 交换缓存（Swapcache）：
 *    - 正在换出或换入的页面
 *    - 每页一个引用
 *
 * 3. PG_private 标志：
 *    - 页面有私有数据（如文件系统元数据）
 *    - 整个 folio 一个引用
 *
 * 4. 页表映射（Page table mappings）：
 *    - 映射到进程地址空间
 *    - 每个映射一个引用
 *
 * 【未计入的引用】
 * - LRU 缓存引用（除非 folio 已从 LRU 隔离）
 * - GUP 引用（Get User Pages）
 * - 临时引用（如正在进行的 I/O）
 * - 调用者自己持有的引用
 *
 * 【稳定性保证】
 * 1. 稳定的情况：
 *    - 未映射的 folio (!folio_mapped())
 *    - 且 folio 已锁定
 *    - 此时引用计数不会并发改变
 *
 * 2. 不稳定的情况：
 *    - 已映射的 folio：
 *      * fork() 可能增加映射
 *      * munmap() 可能减少映射
 *    - 未锁定的 folio：
 *      * 可能并发从交换缓存移除
 *      * 可能并发添加/移除映射
 *
 * 3. 早期检测用途：
 *    - 即使不稳定，也可用于早期检测
 *    - 决定是否值得锁定和取消映射
 *    - 快速筛选明显有问题的情况
 *
 * 【实现原理】
 * 1. 检查页面类型：
 *    - page_has_type: 特殊页面类型（如 buddy、table）
 *    - hugetlb 除外（允许）
 *    - 其他特殊类型返回 0（不应该计算引用）
 *
 * 2. 交换缓存引用：
 *    - folio_test_swapcache(folio) << order
 *    - 如果在交换缓存中，每页 1 个引用
 *    - << order 相当于乘以页面数
 *
 * 3. 非匿名 folio 的额外引用：
 *    - !!folio->mapping << order: 页缓存引用（每页 1 个）
 *    - folio_test_private(folio): PG_private 引用（整个 folio 1 个）
 *
 * 4. 页表映射引用：
 *    - folio_mapcount(folio)
 *    - 每个映射 1 个引用
 *
 * 【C语言知识】位移运算
 * - x << order: 相当于 x * (2^order)
 * - 1 << order: 等于页面数
 * - 用于将 per-page 引用乘以页面数
 *
 * 【C语言知识】双重取反
 * - !!x: 将非零值转换为 1，零值保持 0
 * - !x: 取反（非零 -> 0，零 -> 非零）
 * - !!x: 再次取反（非零 -> 1，零 -> 0）
 * - 用于布尔值标准化
 *
 * 【C语言知识】WARN_ON_ONCE 宏
 * - WARN_ON_ONCE(condition): 条件为真时打印警告
 * - 只警告一次（避免日志洪泛）
 * - 用于检测不应该发生的情况
 *
 * 【参数说明】
 * @folio: 要计算预期引用计数的 folio
 *         - const 表示不会修改
 *
 * 【返回值】
 * - 预期的引用计数（int）
 * - 0: 特殊页面类型或异常情况
 * - > 0: 正常的预期引用计数
 *
 * 【使用场景】
 * - 页面回收前检查是否有意外引用
 * - 页面迁移前验证引用状态
 * - 调试内存泄漏
 * - 检测 GUP 引用
 *
 * 【注意事项】
 * - 调用者需要将自己持有的引用加到结果上
 * - 不计入 LRU 引用（迁移/拆分时 folio 已从 LRU 隔离）
 * - 结果可能不稳定（取决于锁定和映射状态）
 * - 用于检测，不用于精确计数
 */
static inline int folio_expected_ref_count(const struct folio *folio)
{
	const int order = folio_order(folio);
	int ref_count = 0;

	if (WARN_ON_ONCE(page_has_type(&folio->page) && !folio_test_hugetlb(folio)))
		return 0;

	/* One reference per page from the swapcache. */
	/* 交换缓存中每页一个引用 */
	ref_count += folio_test_swapcache(folio) << order;

	if (!folio_test_anon(folio)) {
		/* One reference per page from the pagecache. */
		/* 页缓存中每页一个引用 */
		ref_count += !!folio->mapping << order;
		/* One reference from PG_private. */
		/* PG_private 标志的一个引用 */
		ref_count += folio_test_private(folio);
	}

	/* One reference per page table mapping. */
	/* 每个页表映射一个引用 */
	return ref_count + folio_mapcount(folio);
}

#ifndef HAVE_ARCH_MAKE_FOLIO_ACCESSIBLE
/**
 * arch_make_folio_accessible - 架构特定的 folio 可访问性设置
 * @folio: 要处理的 folio
 *
 * 【功能说明】使 folio 可被 CPU 访问（架构特定的默认实现）
 *
 * 【设计原理】为什么需要此函数？
 * - 某些架构有特殊的内存访问限制
 * - 加密内存需要解密后才能访问
 * - 受保护的内存区域需要权限设置
 * - 提供架构相关的内存准备操作
 *
 * 【架构特定性】
 * - HAVE_ARCH_MAKE_FOLIO_ACCESSIBLE: 架构是否提供自己的实现
 * - 未定义时使用此默认实现（什么都不做）
 * - 定义时使用架构提供的实现（如 x86 的内存加密）
 *
 * 【可能的架构实现】
 * 1. x86 架构（支持 SME/SEV）：
 *    - SME: Secure Memory Encryption（安全内存加密）
 *    - SEV: Secure Encrypted Virtualization（安全加密虚拟化）
 *    - 需要设置页表项以解密内存
 *
 * 2. ARM 架构（支持 TrustZone）：
 *    - 可能需要设置安全/非安全属性
 *    - 配置内存保护单元（MPU）
 *
 * 3. 其他架构：
 *    - 缓存一致性操作
 *    - 内存屏障
 *    - 特殊的访问权限设置
 *
 * 【默认实现】
 * - 返回 0（成功）
 * - 不做任何操作
 * - 适用于没有特殊要求的架构
 *
 * 【调用时机】
 * - 分配新页面后
 * - 从交换空间换入页面后
 * - 页面迁移后
 * - 内存解密/解锁操作
 *
 * 【C语言知识】条件编译
 * - #ifndef MACRO: 如果 MACRO 未定义
 * - #endif: 结束条件编译块
 * - 允许架构提供自己的实现
 *
 * 【参数说明】
 * @folio: 要使其可访问的 folio
 *
 * 【返回值】
 * - 0: 成功
 * - < 0: 失败（错误码，架构实现可能返回）
 *
 * 【注意事项】
 * - 默认实现总是成功
 * - 架构实现可能会失败
 * - 调用者应检查返回值
 * - 失败时页面不应被访问
 */
static inline int arch_make_folio_accessible(struct folio *folio)
{
	return 0;
}
#endif

/*
 * Some inline functions in vmstat.h depend on page_zone()
 */
/*
 * vmstat.h 中的一些内联函数依赖于 page_zone()
 * - vmstat.h: 虚拟内存统计头文件
 * - 包含内存区域统计、页面状态等信息
 * - 必须在定义 page_zone() 之后包含
 */
#include <linux/vmstat.h>

#if defined(CONFIG_HIGHMEM) && !defined(WANT_PAGE_VIRTUAL)
#define HASHED_PAGE_VIRTUAL
#endif

#if defined(WANT_PAGE_VIRTUAL)
/**
 * page_address - 获取页面的虚拟地址
 * @page: 要查询的页面
 *
 * 【功能说明】返回页面的内核虚拟地址（直接从 page->virtual 读取）
 *
 * 【设计原理】WANT_PAGE_VIRTUAL 配置
 * - 每个 struct page 包含 virtual 字段
 * - 直接存储虚拟地址，查询速度快
 * - 占用额外内存（每页 8 字节指针）
 * - 适用于内存充足的系统
 *
 * 【虚拟地址】
 * - 内核虚拟地址：内核可以直接访问的地址
 * - 与物理地址有固定映射关系
 * - 用于 CPU 访问页面内容
 *
 * 【参数说明】
 * @page: 目标页面
 *        - const 表示只读
 *
 * 【返回值】
 * - void *: 页面的内核虚拟地址
 *
 * 【C语言知识】void * 指针
 * - void *: 通用指针类型
 * - 可以指向任何类型的数据
 * - 使用前需要转换为具体类型
 */
static inline void *page_address(const struct page *page)
{
	return page->virtual;
}

/**
 * set_page_address - 设置页面的虚拟地址
 * @page: 要设置的页面
 * @address: 虚拟地址
 *
 * 【功能说明】设置页面的 virtual 字段
 *
 * 【调用时机】
 * - 页面初始化时
 * - 建立内核映射时
 * - 内存热插拔时
 *
 * 【参数说明】
 * @page: 目标页面
 * @address: 要设置的虚拟地址
 */
static inline void set_page_address(struct page *page, void *address)
{
	page->virtual = address;
}

/**
 * page_address_init - 初始化页面地址子系统
 *
 * 【功能说明】空操作（WANT_PAGE_VIRTUAL 模式不需要初始化）
 *
 * 【设计原理】
 * - virtual 字段直接存储在 page 结构中
 * - 不需要额外的数据结构
 * - 不需要初始化操作
 *
 * 【C语言知识】do { } while(0) 宏
 * - 创建空操作的惯用法
 * - 使宏可以作为单个语句使用
 * - 支持在 if 等控制结构中使用
 */
#define page_address_init()  do { } while(0)
#endif

#if defined(HASHED_PAGE_VIRTUAL)
/**
 * page_address - 获取页面的虚拟地址（哈希表查找版本）
 * @page: 要查询的页面
 *
 * 【功能说明】通过哈希表查找页面的虚拟地址
 *
 * 【设计原理】HASHED_PAGE_VIRTUAL 配置
 * - 不在 struct page 中存储 virtual 字段
 * - 使用哈希表存储页面到虚拟地址的映射
 * - 节省内存（不是每页都有虚拟地址）
 * - 查询稍慢，但内存开销小
 *
 * 【使用场景】
 * - 配置了 CONFIG_HIGHMEM 但未配置 WANT_PAGE_VIRTUAL
 * - 高端内存（HIGHMEM）系统
 * - 32位系统，内核地址空间有限
 *
 * 【高端内存】HIGHMEM
 * - 32位系统的物理内存超过直接映射范围
 * - 需要临时映射才能访问
 * - 哈希表跟踪当前映射关系
 *
 * 【返回值】
 * - void *: 页面的内核虚拟地址
 * - NULL: 页面未映射
 */
void *page_address(const struct page *page);

/**
 * set_page_address - 设置页面的虚拟地址（哈希表版本）
 * @page: 要设置的页面
 * @virtual: 虚拟地址
 *
 * 【功能说明】在哈希表中记录页面的虚拟地址映射
 *
 * 【调用时机】
 * - 建立临时内核映射（kmap）时
 * - 取消映射（kunmap）时设置为 NULL
 *
 * 【参数说明】
 * @page: 目标页面
 * @virtual: 虚拟地址（或 NULL 表示取消映射）
 */
void set_page_address(struct page *page, void *virtual);

/**
 * page_address_init - 初始化页面地址哈希表
 *
 * 【功能说明】初始化用于存储页面虚拟地址映射的哈希表
 *
 * 【调用时机】
 * - 系统启动早期
 * - 在使用 page_address 之前
 *
 * 【实现】
 * - 初始化哈希表结构
 * - 分配必要的数据结构
 * - 设置锁保护
 */
void page_address_init(void);
#endif

/**
 * lowmem_page_address - 获取低端内存页面的虚拟地址
 * @page: 要查询的页面
 *
 * 【功能说明】直接计算低端内存页面的虚拟地址
 *
 * 【设计原理】为什么叫 lowmem？
 * - 低端内存（Low Memory）：内核直接映射的物理内存
 * - 在内核地址空间有固定的线性映射
 * - 物理地址和虚拟地址有固定偏移关系
 * - 不需要临时映射，可以直接访问
 *
 * 【低端内存 vs 高端内存】
 * - 低端内存（Low Memory）：
 *   * 内核直接映射区域
 *   * 虚拟地址 = 物理地址 + 固定偏移
 *   * 总是可访问，速度快
 *   * 32位系统通常是前 896MB
 *
 * - 高端内存（High Memory）：
 *   * 超出直接映射范围的内存
 *   * 需要临时映射（kmap）才能访问
 *   * 访问开销大
 *   * 64位系统通常没有高端内存
 *
 * 【实现原理】
 * - page_to_virt(page): 将页面转换为虚拟地址
 * - 基于页面的物理地址计算虚拟地址
 * - 公式: virt = phys + PAGE_OFFSET
 *
 * 【C语言知识】__always_inline 属性
 * - __always_inline: 强制内联
 * - 即使在 -O0 (无优化) 下也内联
 * - 用于性能关键路径
 * - 避免函数调用开销
 *
 * 【参数说明】
 * @page: 目标页面
 *        - 必须是低端内存页面
 *        - const 表示只读
 *
 * 【返回值】
 * - void *: 页面的内核虚拟地址
 *
 * 【注意事项】
 * - 仅用于低端内存页面
 * - 高端内存页面需要使用 page_address 或 kmap
 * - 64位系统通常所有内存都是低端内存
 */
static __always_inline void *lowmem_page_address(const struct page *page)
{
	return page_to_virt(page);
}

#if !defined(HASHED_PAGE_VIRTUAL) && !defined(WANT_PAGE_VIRTUAL)
/**
 * page_address - 获取页面的虚拟地址（简化版本）
 * @page: 要查询的页面
 *
 * 【功能说明】对于没有高端内存的系统，直接返回低端内存地址
 *
 * 【设计原理】为什么使用宏定义？
 * - 既不使用哈希表（HASHED_PAGE_VIRTUAL）
 * - 也不在 page 结构中存储（WANT_PAGE_VIRTUAL）
 * - 所有内存都是低端内存（如 64 位系统）
 * - 直接映射，不需要查找
 *
 * 【适用场景】
 * - 64位系统（地址空间足够大）
 * - 没有配置 CONFIG_HIGHMEM
 * - 所有物理内存都在直接映射范围内
 *
 * 【C语言知识】宏定义
 * - #define macro(arg) expression: 函数式宏
 * - 参数会被文本替换
 * - 没有函数调用开销
 * - 编译时展开
 *
 * 【参数说明】
 * @page: 目标页面
 *
 * 【返回值】
 * - 页面的内核虚拟地址
 */
#define page_address(page) lowmem_page_address(page)

/**
 * set_page_address - 设置页面地址（空操作版本）
 * @page: 页面
 * @address: 地址
 *
 * 【功能说明】不需要设置地址（空操作）
 *
 * 【设计原理】为什么是空操作？
 * - 所有页面都是低端内存
 * - 虚拟地址可以直接计算
 * - 不需要存储或记录映射关系
 * - 提供接口兼容性
 *
 * 【C语言知识】do { } while(0) 宏
 * - 创建空操作的标准方式
 * - 使宏可以像函数一样使用
 * - 支持在 if/else 等控制结构中使用
 * - 末尾需要分号
 *
 * 【示例】
 * if (condition)
 *     set_page_address(page, addr);  // 宏展开后仍然是单个语句
 * else
 *     do_something();
 */
#define set_page_address(page, address)  do { } while(0)

/**
 * page_address_init - 初始化页面地址子系统（空操作版本）
 *
 * 【功能说明】不需要初始化（空操作）
 *
 * 【设计原理】
 * - 没有哈希表或额外数据结构
 * - 所有地址都可以直接计算
 * - 不需要初始化操作
 * - 提供接口兼容性
 */
#define page_address_init()  do { } while(0)
#endif

/**
 * folio_address - 获取 folio 的虚拟地址
 * @folio: 要查询的 folio
 *
 * 【功能说明】返回 folio 第一个页面的虚拟地址
 *
 * 【设计原理】为什么通过第一个页面获取地址？
 * - folio 是连续的物理页面集合
 * - folio 的地址就是第一个页面的地址
 * - folio->page 是头页（第一个页面）
 * - 其他页面的地址可以通过偏移计算
 *
 * 【Folio 内存布局】
 * - folio 包含 1 个或多个连续页面
 * - 第一个页面的地址是 folio 的起始地址
 * - 第 n 个页面地址 = folio_address + n * PAGE_SIZE
 *
 * 【实现原理】
 * - &folio->page: 获取头页的地址
 * - page_address(&folio->page): 获取头页的虚拟地址
 * - 返回的地址就是 folio 的起始地址
 *
 * 【参数说明】
 * @folio: 目标 folio
 *         - const 表示只读
 *
 * 【返回值】
 * - void *: folio 的内核虚拟地址
 * - 指向 folio 第一个字节
 *
 * 【使用场景】
 * - 访问 folio 的内容
 * - 计算 folio 内偏移地址
 * - 内存拷贝和操作
 * - I/O 操作的缓冲区地址
 *
 * 【注意事项】
 * - 高端内存 folio 可能返回 NULL（未映射时）
 * - 需要时应该使用 kmap_local_folio
 * - 不要假设地址总是有效
 */
static inline void *folio_address(const struct folio *folio)
{
	return page_address(&folio->page);
}

/*
 * Return true only if the page has been allocated with
 * ALLOC_NO_WATERMARKS and the low watermark was not
 * met implying that the system is under some pressure.
 */
/*
 * 仅当页面使用 ALLOC_NO_WATERMARKS 分配
 * 且未满足低水位线时返回 true
 * 这表明系统正处于内存压力下
 *
 * 【功能说明】判断页面是否从紧急内存储备分配
 *
 * 【设计原理】什么是 pfmemalloc？
 * - PF: Page Frame Reclaim (页帧回收)
 * - memalloc: Memory Allocation (内存分配)
 * - 系统内存紧张时的紧急分配机制
 * - 绕过水位线限制，从储备内存分配
 * - 用于关键路径（如网络接收、交换）
 *
 * 【水位线】Watermarks
 * - 高水位线（high）：内存充足
 * - 低水位线（low）：开始回收
 * - 最低水位线（min）：紧急状态
 * - ALLOC_NO_WATERMARKS：绕过水位线检查
 *
 * 【为什么需要 pfmemalloc？】
 * - 避免死锁：网络接收需要内存，但回收也需要网络
 * - 保证前进：即使内存紧张也能完成关键操作
 * - 交换路径：换出页面需要分配临时内存
 *
 * 【实现原理】
 * - 使用 lru.next 的第 1 位（bit 1）标记
 * - bit 0 通常用于其他目的
 * - bit 1 = 1：从 pfmemalloc 储备分配
 * - bit 1 = 0：正常分配
 *
 * 【C语言知识】位操作
 * - (uintptr_t): 将指针转换为无符号整数
 * - & BIT(1): 测试第 1 位是否设置
 * - BIT(1) 等于 2（二进制 0b10）
 *
 * 【参数说明】
 * @page: 要检查的页面
 *        - const 表示只读
 *
 * 【返回值】
 * - true: 页面从 pfmemalloc 储备分配
 * - false: 页面正常分配
 *
 * 【注意事项】
 * - 调用者可以覆盖此信息（如果不需要保留）
 * - 仅在系统内存压力下才会设置
 * - 这些页面应该尽快释放
 */
static inline bool page_is_pfmemalloc(const struct page *page)
{
	/*
	 * lru.next has bit 1 set if the page is allocated from the
	 * pfmemalloc reserves.  Callers may simply overwrite it if
	 * they do not need to preserve that information.
	 */
	/*
	 * 如果页面从 pfmemalloc 储备分配，lru.next 的第 1 位被设置
	 * 如果不需要保留此信息，调用者可以简单地覆盖它
	 */
	return (uintptr_t)page->lru.next & BIT(1);
}

/*
 * Return true only if the folio has been allocated with
 * ALLOC_NO_WATERMARKS and the low watermark was not
 * met implying that the system is under some pressure.
 */
/*
 * 仅当 folio 使用 ALLOC_NO_WATERMARKS 分配
 * 且未满足低水位线时返回 true
 * 这表明系统正处于内存压力下
 *
 * 【功能说明】判断 folio 是否从紧急内存储备分配
 *
 * 【与 page_is_pfmemalloc 的关系】
 * - 功能相同，但操作对象是 folio
 * - 检查 folio 头页的 lru.next 标志
 * - folio 作为整体从储备分配
 *
 * 【实现原理】
 * - 检查 folio->lru.next 的第 1 位
 * - folio 的 lru 字段在头页中
 * - 整个 folio 共享相同的分配来源
 *
 * 【参数说明】
 * @folio: 要检查的 folio
 *         - const 表示只读
 *
 * 【返回值】
 * - true: folio 从 pfmemalloc 储备分配
 * - false: folio 正常分配
 */
static inline bool folio_is_pfmemalloc(const struct folio *folio)
{
	/*
	 * lru.next has bit 1 set if the page is allocated from the
	 * pfmemalloc reserves.  Callers may simply overwrite it if
	 * they do not need to preserve that information.
	 */
	/*
	 * 如果页面从 pfmemalloc 储备分配，lru.next 的第 1 位被设置
	 * 如果不需要保留此信息，调用者可以简单地覆盖它
	 */
	return (uintptr_t)folio->lru.next & BIT(1);
}

/*
 * Only to be called by the page allocator on a freshly allocated
 * page.
 */
/*
 * 仅由页面分配器在新分配的页面上调用
 *
 * 【功能说明】标记页面为从 pfmemalloc 储备分配
 *
 * 【为什么这么设计】
 * - 在分配时立即标记，避免后续判断错误
 * - 使用 lru.next 的第 1 位保存状态
 * - 新页面的 lru 链表为空，可以直接设置
 *
 * 【调用时机】
 * - 页面分配器刚分配页面后
 * - 使用 ALLOC_NO_WATERMARKS 标志分配时
 * - 在页面加入 LRU 链表之前
 *
 * 【安全性】
 * - 仅在"freshly allocated"(刚分配)的页面上调用
 * - 此时 lru 链表未使用，覆盖安全
 * - 后续如果加入 LRU 链表，标志会被保留在第 1 位
 *
 * 【参数说明】
 * @page: 刚分配的页面
 *        - 必须是新分配的，lru 未使用
 *        - 非 const，因为要修改状态
 */
static inline void set_page_pfmemalloc(struct page *page)
{
	page->lru.next = (void *)BIT(1);
}

/*
 * 清除页面的 pfmemalloc 标志
 *
 * 【功能说明】将页面标记为非 pfmemalloc 分配
 *
 * 【为什么这么设计】
 * - 当页面不再需要 pfmemalloc 标记时清除
 * - 将 lru.next 置为 NULL，同时清除第 1 位
 * - 简单粗暴的实现：直接置空而不是位操作
 *
 * 【使用场景】
 * - 页面被释放回正常内存池
 * - 页面状态改变，不再从储备分配
 * - 在重用页面前清理标志
 *
 * 【实现细节】
 * - 直接设置 lru.next = NULL
 * - NULL 的所有位都是 0，包括第 1 位
 * - 比位清除操作更简洁
 *
 * 【参数说明】
 * @page: 要清除标志的页面
 *        - 非 const，因为要修改状态
 */
static inline void clear_page_pfmemalloc(struct page *page)
{
	page->lru.next = NULL;
}

/*
 * Can be called by the pagefault handler when it gets a VM_FAULT_OOM.
 */
/*
 * 当页面故障处理程序收到 VM_FAULT_OOM 时可以调用
 *
 * 【功能说明】处理页面故障导致的内存耗尽 (Out of Memory)
 *
 * 【为什么这么设计】
 * - 页面故障时可能无法分配内存
 * - 需要触发 OOM killer 释放内存
 * - 集中处理 OOM 逻辑，避免重复代码
 *
 * 【调用场景】
 * - 页面故障处理程序返回 VM_FAULT_OOM
 * - 无法分配页面来满足故障请求
 * - 系统内存极度紧张
 *
 * 【VM_FAULT_OOM 说明】
 * - VM: Virtual Memory (虚拟内存)
 * - FAULT: 页面故障
 * - OOM: Out of Memory (内存耗尽)
 * - 这是一个错误码，表示故障处理失败
 *
 * 【OOM killer】
 * - Linux 内核的内存回收最后手段
 * - 选择并杀死占用内存多的进程
 * - 释放内存给关键操作使用
 *
 * 【extern 说明】
 * - 函数在其他文件中定义
 * - 这里只是声明，供调用者使用
 */
extern void pagefault_out_of_memory(void);

/*
 * 计算指针在页面内的偏移量
 *
 * 【功能说明】获取指针 p 相对于其所在页面起始地址的偏移
 *
 * 【为什么这么设计】
 * - 经常需要知道地址在页面内的位置
 * - 使用宏定义提高性能，编译时展开
 * - 位运算实现，比除法和取模快
 *
 * 【实现原理】
 * - PAGE_MASK: 页面对齐掩码，低位全 0
 * - ~PAGE_MASK: 取反，低位全 1，高位全 0
 * - p & ~PAGE_MASK: 保留低位，去除高位
 * - 结果就是页内偏移 (0 到 PAGE_SIZE-1)
 *
 * 【示例】假设 PAGE_SIZE = 4096 (0x1000)
 * - PAGE_MASK = 0xFFFFF000
 * - ~PAGE_MASK = 0x00000FFF
 * - 地址 0x12345678 & 0x00000FFF = 0x678 (页内偏移)
 *
 * 【参数说明】
 * @p: 指针或地址
 *     - 转换为 unsigned long 进行位运算
 *
 * 【返回值】
 * - 页内偏移，范围 [0, PAGE_SIZE-1]
 * - unsigned long 类型
 */
#define offset_in_page(p)	((unsigned long)(p) & ~PAGE_MASK)

/*
 * 计算指针在 folio 内的偏移量
 *
 * 【功能说明】获取指针 p 相对于 folio 起始地址的偏移
 *
 * 【与 offset_in_page 的区别】
 * - offset_in_page: 单页内偏移，最大 PAGE_SIZE-1
 * - offset_in_folio: folio 内偏移，可能跨多个页面
 * - folio 可能是多页 (large folio / huge page)
 *
 * 【为什么这么设计】
 * - folio 大小是 2 的幂次，可以用位运算
 * - folio_size(folio) - 1 生成掩码
 * - 例如 8KB folio: size=0x2000, mask=0x1FFF
 *
 * 【实现原理】
 * - folio_size(folio): 获取 folio 字节大小
 * - folio_size(folio) - 1: 生成低位全 1 掩码
 * - p & mask: 保留 folio 大小范围内的低位
 *
 * 【示例】假设 folio_size = 8192 (0x2000)
 * - mask = 8192 - 1 = 0x1FFF
 * - 地址 0x12345678 & 0x1FFF = 0x1678 (folio 内偏移)
 *
 * 【参数说明】
 * @folio: folio 指针
 *         - 用于计算 folio 大小
 * @p: 指针或地址
 *     - 转换为 unsigned long 进行位运算
 *
 * 【返回值】
 * - folio 内偏移，范围 [0, folio_size-1]
 * - unsigned long 类型
 */
#define offset_in_folio(folio, p) ((unsigned long)(p) & (folio_size(folio) - 1))

/*
 * Parameter block passed down to zap_pte_range in exceptional cases.
 */
/*
 * 在特殊情况下传递给 zap_pte_range 的参数块
 *
 * 【功能说明】控制页表项 (PTE) 清除操作的参数结构
 *
 * 【为什么这么设计】
 * - zap 操作有多种变体和特殊情况
 * - 使用结构体传递参数，比多个函数参数清晰
 * - 便于扩展新的控制选项
 *
 * 【zap 的含义】
 * - zap: 清除、删除页表映射
 * - zap_pte_range: 批量清除一段 PTE (页表项)
 * - 用于 munmap、进程退出等场景
 *
 * 【PTE 说明】
 * - PTE: Page Table Entry (页表项)
 * - 记录虚拟地址到物理页面的映射
 * - zap 就是解除这种映射关系
 *
 * 【使用场景】
 * - 不是所有 zap 操作都需要此结构
 * - 仅在"exceptional cases"(特殊情况)使用
 * - 例如：只解除特定 folio 的映射、跳过 COW 页面等
 */
struct zap_details {
	/*
	 * 要解除映射的已锁定 folio
	 *
	 * 【用途】
	 * - 当只需要解除特定 folio 的映射时设置
	 * - 其他映射保持不变
	 * - folio 必须已被锁定，防止并发修改
	 *
	 * 【为什么需要】
	 * - 有时只需移除特定页面的映射
	 * - 例如文件截断、页面迁移等
	 * - 避免影响其他正常映射
	 */
	struct folio *single_folio;	/* Locked folio to be unmapped */

	/*
	 * 是否跳过 COW 的私有页面
	 *
	 * 【COW 说明】
	 * - COW: Copy-On-Write (写时复制)
	 * - 多个进程共享只读页面
	 * - 写入时才复制出私有副本
	 *
	 * 【为什么需要】
	 * - 某些场景不想 zap 私有的 COW 页面
	 * - 例如只想解除共享映射
	 * - 保留进程的私有修改
	 *
	 * 【true 的含义】
	 * - 跳过 COW 产生的私有页面
	 * - 只 zap 共享页面
	 */
	bool skip_cows;			/* Do not zap COWed private pages */

	/*
	 * 是否需要回收页表
	 *
	 * 【为什么需要】
	 * - zap 后可能整个页表变空
	 * - 空页表占用内存，应该释放
	 * - 减少内存开销
	 *
	 * 【页表层级】
	 * - 现代系统使用多级页表 (PGD->PUD->PMD->PTE)
	 * - zap 清除 PTE 后，可能整个 PMD 为空
	 * - 此标志控制是否释放空的页表页
	 *
	 * 【true 的含义】
	 * - zap 后检查并回收空页表
	 * - 节省内存
	 */
	bool reclaim_pt;		/* Need reclaim page tables? */

	/*
	 * 是否正在"收割"(reaping)，不要阻塞
	 *
	 * 【reaping 的含义】
	 * - reap: 收割，这里指快速清理
	 * - 通常用于进程退出时的快速清理路径
	 * - 不希望被阻塞或等待
	 *
	 * 【为什么需要】
	 * - 进程退出时需要快速释放资源
	 * - 不应该被锁或其他操作阻塞
	 * - 可以跳过某些非关键操作
	 *
	 * 【true 的含义】
	 * - 处于快速清理模式
	 * - 遇到锁就跳过，不等待
	 * - 优先速度而非完整性
	 */
	bool reaping;			/* Reaping, do not block. */

	/*
	 * zap 操作的额外标志位
	 *
	 * 【zap_flags_t 类型】
	 * - 可能是枚举或位掩码类型
	 * - 包含更多细粒度的控制选项
	 * - 扩展性更好
	 *
	 * 【用途】
	 * - 控制 zap 的具体行为
	 * - 例如是否刷新 TLB、是否释放页面等
	 * - 通过位标志组合多种选项
	 */
	zap_flags_t zap_flags;		/* Extra flags for zapping */
};

/*
 * Whether to drop the pte markers, for example, the uffd-wp information for
 * file-backed memory.  This should only be specified when we will completely
 * drop the page in the mm, either by truncation or unmapping of the vma.  By
 * default, the flag is not set.
 */
/*
 * 是否丢弃 PTE 标记，例如文件支持内存的 uffd-wp 信息
 * 仅当我们将完全从 mm 中丢弃页面时才应指定此标志
 * 例如通过截断或解除 vma 的映射
 * 默认情况下，此标志未设置
 *
 * 【功能说明】控制是否删除 PTE 的特殊标记
 *
 * 【PTE markers 说明】
 * - PTE 标记：页表项中的特殊标记位
 * - 即使页面不在内存中，标记仍然保留
 * - 用于记录页面的历史状态或特殊属性
 *
 * 【uffd-wp 说明】
 * - uffd: userfaultfd (用户态页面故障处理)
 * - wp: write-protect (写保护)
 * - 用户空间可以监控页面的写操作
 * - 标记记录哪些页面被写保护
 *
 * 【为什么需要这个标志】
 * - 有时只是临时解除映射，稍后还会映射回来
 * - 此时应保留 PTE 标记，保持状态连续性
 * - 只有彻底删除页面时才清除标记
 *
 * 【使用场景】
 * - truncation: 文件截断，页面永久删除
 * - unmapping of vma: 完全解除 VMA 映射
 * - 这些是"完全丢弃"的场景
 *
 * 【__force 说明】
 * - Sparse 静态分析工具的注解
 * - 强制类型转换，避免类型检查警告
 *
 * 【BIT(0) 说明】
 * - 设置第 0 位为 1
 * - 这是标志位的最低位
 */
#define  ZAP_FLAG_DROP_MARKER        ((__force zap_flags_t) BIT(0))

/* Set in unmap_vmas() to indicate a final unmap call.  Only used by hugetlb */
/*
 * 在 unmap_vmas() 中设置以表示最终的 unmap 调用
 * 仅由 hugetlb 使用
 *
 * 【功能说明】标记这是最后一次解除映射调用
 *
 * 【为什么需要】
 * - unmap 可能分多次调用
 * - hugetlb (大页) 需要知道何时是最后一次
 * - 最后一次调用时才执行某些清理工作
 *
 * 【hugetlb 说明】
 * - Huge Pages (大页): 比普通页面大得多的页面
 * - 例如 2MB 或 1GB，而不是 4KB
 * - 减少页表层级，提高 TLB 命中率
 *
 * 【使用场景】
 * - 大页的解除映射操作
 * - 需要区分中间步骤和最终步骤
 * - 最终步骤时释放大页资源
 *
 * 【BIT(1) 说明】
 * - 设置第 1 位为 1
 * - 与 ZAP_FLAG_DROP_MARKER 不同位
 * - 可以组合使用多个标志
 */
#define  ZAP_FLAG_UNMAP              ((__force zap_flags_t) BIT(1))

/*
 * 【条件编译】MMU (内存管理单元) 配置
 *
 * 【MMU 说明】
 * - MMU: Memory Management Unit (内存管理单元)
 * - 硬件组件，负责虚拟地址到物理地址转换
 * - 提供内存保护和页表管理
 *
 * 【CONFIG_MMU】
 * - 配置选项，表示系统是否有 MMU
 * - 大多数现代系统都有 MMU
 * - 某些嵌入式系统可能没有 MMU (No-MMU)
 *
 * 【为什么需要条件编译】
 * - 有 MMU 和无 MMU 系统的实现不同
 * - 某些功能在无 MMU 系统上不可用
 * - 条件编译避免编译无用代码
 */
#ifdef CONFIG_MMU
/*
 * 检查是否可以执行 mlock 操作
 *
 * 【功能说明】判断当前进程是否有权限锁定内存
 *
 * 【mlock 说明】
 * - mlock: memory lock (内存锁定)
 * - 将页面锁定在物理内存中
 * - 防止被交换到磁盘 (swap)
 *
 * 【为什么需要 mlock】
 * - 某些应用需要保证响应时间
 * - 避免页面故障导致的延迟
 * - 例如实时应用、密码处理等
 *
 * 【权限检查】
 * - 需要特定权限才能 mlock
 * - 普通用户有资源限制 (RLIMIT_MEMLOCK)
 * - root 用户通常可以无限制锁定
 *
 * 【返回值】
 * - true: 可以执行 mlock
 * - false: 没有权限或达到限制
 *
 * 【extern 说明】
 * - 函数在其他文件中定义
 * - 这里只是声明
 */
extern bool can_do_mlock(void);
#else
/*
 * 无 MMU 系统的 can_do_mlock 实现
 *
 * 【为什么返回 false】
 * - 无 MMU 系统没有虚拟内存和页面交换
 * - 所有内存已经是"物理"内存
 * - mlock 概念不适用
 *
 * 【inline 说明】
 * - 编译器会将函数代码直接插入调用处
 * - 避免函数调用开销
 * - 对于简单函数很有效
 */
static inline bool can_do_mlock(void) { return false; }
#endif

/*
 * 用户共享内存锁定
 *
 * 【功能说明】为用户锁定指定大小的共享内存
 *
 * 【SHM 说明】
 * - SHM: Shared Memory (共享内存)
 * - 多个进程可以访问的内存区域
 * - 用于进程间通信 (IPC)
 *
 * 【为什么需要这个函数】
 * - 共享内存也可以被 mlock
 * - 需要跟踪每个用户锁定的内存量
 * - 防止用户滥用锁定功能
 *
 * 【ucounts 说明】
 * - User Counts: 用户资源计数
 * - 跟踪用户使用的各种资源
 * - 包括锁定内存、打开文件等
 *
 * 【参数说明】
 * @size: 要锁定的内存大小 (字节)
 * @ucounts: 用户资源计数结构指针
 *           - 用于记账和限制检查
 *
 * 【返回值】
 * - 0: 成功
 * - 非 0: 失败 (可能超出限制)
 */
extern int user_shm_lock(size_t, struct ucounts *);

/*
 * 用户共享内存解锁
 *
 * 【功能说明】解锁之前锁定的共享内存
 *
 * 【为什么需要】
 * - 与 user_shm_lock 配对使用
 * - 释放资源计数
 * - 允许页面被交换出去
 *
 * 【参数说明】
 * @size: 要解锁的内存大小 (字节)
 *        - 应与 lock 时的大小匹配
 * @ucounts: 用户资源计数结构指针
 *           - 更新计数信息
 *
 * 【注意事项】
 * - 必须与 user_shm_lock 成对调用
 * - size 参数应该匹配
 * - 否则资源计数会不准确
 */
extern void user_shm_unlock(size_t, struct ucounts *);

/*
 * 从 VMA 和 PTE 获取普通 folio
 *
 * 【功能说明】根据虚拟地址和页表项返回对应的 folio
 *
 * 【为什么叫"normal"】
 * - 排除特殊类型的 folio
 * - 例如排除 VM_PFNMAP (直接物理地址映射)
 * - 只返回常规的、可管理的 folio
 *
 * 【使用场景】
 * - 需要操作页面内容时
 * - 检查页面状态时
 * - 确保获取的是普通页面，不是特殊映射
 *
 * 【参数说明】
 * @vma: 虚拟内存区域指针
 *       - 包含映射属性和标志
 * @addr: 虚拟地址
 *        - unsigned long 类型
 * @pte: 页表项
 *       - pte_t 是架构相关的页表项类型
 *
 * 【返回值】
 * - 非 NULL: 指向普通 folio 的指针
 * - NULL: 不是普通 folio (可能是特殊映射或无效)
 */
struct folio *vm_normal_folio(struct vm_area_struct *vma, unsigned long addr,
			     pte_t pte);

/*
 * 从 VMA 和 PTE 获取普通页面
 *
 * 【功能说明】与 vm_normal_folio 类似，但返回 page 而不是 folio
 *
 * 【page vs folio】
 * - 旧接口返回 struct page
 * - 新接口返回 struct folio
 * - page 可以是 folio 的任意一页
 *
 * 【为什么保留 page 接口】
 * - 兼容旧代码
 * - 某些场景只需要单页
 * - 逐步迁移到 folio
 *
 * 【参数说明】
 * @vma: 虚拟内存区域指针
 * @addr: 虚拟地址
 * @pte: 页表项
 *
 * 【返回值】
 * - 非 NULL: 指向普通页面的指针
 * - NULL: 不是普通页面
 */
struct page *vm_normal_page(struct vm_area_struct *vma, unsigned long addr,
			     pte_t pte);

/*
 * 从 VMA 和 PMD 获取普通 folio
 *
 * 【功能说明】处理 PMD 级别映射的 folio (通常是大页)
 *
 * 【PMD 说明】
 * - PMD: Page Middle Directory (页中间目录)
 * - 多级页表的中间层
 * - PMD 直接映射时通常是大页 (2MB on x86-64)
 *
 * 【为什么需要单独的函数】
 * - PMD 映射与 PTE 映射结构不同
 * - 大页需要特殊处理
 * - 检查和验证逻辑不同
 *
 * 【使用场景】
 * - THP (Transparent Huge Pages) 透明大页
 * - 2MB 或更大的页面映射
 * - 减少页表层级和 TLB 开销
 *
 * 【参数说明】
 * @vma: 虚拟内存区域指针
 * @addr: 虚拟地址
 * @pmd: 页中间目录项
 *       - pmd_t 是架构相关的 PMD 类型
 *
 * 【返回值】
 * - 非 NULL: 指向普通大页 folio 的指针
 * - NULL: 不是普通大页映射
 */
struct folio *vm_normal_folio_pmd(struct vm_area_struct *vma,
				  unsigned long addr, pmd_t pmd);

/*
 * 从 VMA 和 PMD 获取普通页面
 *
 * 【功能说明】vm_normal_folio_pmd 的 page 版本
 *
 * 【参数说明】
 * @vma: 虚拟内存区域指针
 * @addr: 虚拟地址
 * @pmd: 页中间目录项
 *
 * 【返回值】
 * - 非 NULL: 指向普通大页的 page 指针
 * - NULL: 不是普通大页映射
 */
struct page *vm_normal_page_pmd(struct vm_area_struct *vma, unsigned long addr,
				pmd_t pmd);

/*
 * 从 VMA 和 PUD 获取普通页面
 *
 * 【功能说明】处理 PUD 级别映射的页面 (巨大页)
 *
 * 【PUD 说明】
 * - PUD: Page Upper Directory (页上层目录)
 * - 比 PMD 更高一级的页表
 * - PUD 直接映射时是巨大页 (1GB on x86-64)
 *
 * 【为什么需要】
 * - 支持 1GB 大页
 * - 进一步减少页表开销
 * - 适合大内存数据库等应用
 *
 * 【使用场景】
 * - 1GB 大页映射
 * - 超大内存区域
 * - hugetlbfs 文件系统
 *
 * 【参数说明】
 * @vma: 虚拟内存区域指针
 * @addr: 虚拟地址
 * @pud: 页上层目录项
 *       - pud_t 是架构相关的 PUD 类型
 *
 * 【返回值】
 * - 非 NULL: 指向普通巨大页的 page 指针
 * - NULL: 不是普通巨大页映射
 */
struct page *vm_normal_page_pud(struct vm_area_struct *vma, unsigned long addr,
		pud_t pud);

/*
 * 清除 VMA 中特殊映射的范围
 *
 * 【功能说明】解除 VMA 中指定范围内的特殊页表映射
 *
 * 【special vma 说明】
 * - 特殊 VMA：具有特殊标志的虚拟内存区域
 * - 例如 VM_PFNMAP (直接映射物理地址)
 * - 例如 VM_IO (映射 I/O 内存)
 * - 不能用普通方式处理
 *
 * 【为什么需要专门的函数】
 * - 特殊映射没有对应的 struct page
 * - 不能执行正常的引用计数操作
 * - 需要特殊的清理逻辑
 *
 * 【使用场景】
 * - 解除设备内存映射
 * - 清理 VM_PFNMAP 映射
 * - 部分 munmap 特殊区域
 *
 * 【参数说明】
 * @vma: 虚拟内存区域指针
 *       - 必须是特殊类型的 VMA
 * @address: 起始虚拟地址
 *           - 要清除的范围起点
 * @size: 大小 (字节)
 *        - 要清除的范围长度
 *
 * 【注意事项】
 * - 地址和大小通常要页面对齐
 * - 范围必须在 VMA 边界内
 */
void zap_special_vma_range(struct vm_area_struct *vma, unsigned long address,
		  unsigned long size);

/*
 * 清除 VMA 中指定范围的页表映射
 *
 * 【功能说明】解除 VMA 中指定地址范围的所有页表项
 *
 * 【与 zap_special_vma_range 的区别】
 * - zap_vma_range: 适用于普通 VMA
 * - 处理有 struct page 的正常映射
 * - 更新引用计数，释放页面
 *
 * 【为什么这么设计】
 * - 提供精确范围控制的 zap 操作
 * - 支持部分 munmap
 * - 允许保留 VMA 的其他部分
 *
 * 【使用场景】
 * - munmap 系统调用的实现
 * - 部分解除映射
 * - VMA 分割和合并
 *
 * 【参数说明】
 * @vma: 虚拟内存区域指针
 * @address: 起始虚拟地址
 * @size: 大小 (字节)
 *
 * 【与 zap_vma 的关系】
 * - zap_vma 清除整个 VMA
 * - zap_vma_range 清除部分范围
 * - zap_vma 内部调用 zap_vma_range
 */
void zap_vma_range(struct vm_area_struct *vma, unsigned long address,
			   unsigned long size);

/**
 * zap_vma - zap all page table entries in a vma
 * @vma: The vma to zap.
 */
/**
 * zap_vma - 清除 vma 中的所有页表项
 * @vma: 要清除的 vma
 *
 * 【功能说明】完全清除一个 VMA 的所有页表映射
 *
 * 【为什么这么设计】
 * - 提供便捷的整个 VMA 清除接口
 * - 自动计算起始地址和大小
 * - 避免调用者计算错误
 *
 * 【实现原理】
 * - vm_start: VMA 的起始地址
 * - vm_end: VMA 的结束地址
 * - vm_end - vm_start: VMA 的大小
 * - 调用 zap_vma_range 清除整个范围
 *
 * 【使用场景】
 * - 进程退出时清理所有 VMA
 * - 完全 munmap 一个区域
 * - VMA 被删除时
 *
 * 【inline 说明】
 * - 简单的包装函数
 * - 编译器会内联展开
 * - 没有函数调用开销
 */
static inline void zap_vma(struct vm_area_struct *vma)
{
	zap_vma_range(vma, vma->vm_start, vma->vm_end - vma->vm_start);
}
/*
 * MMU notifier 范围结构的前向声明
 *
 * 【前向声明说明】
 * - 告诉编译器存在这个结构类型
 * - 完整定义在其他头文件中
 * - 这里只需要知道类型名称
 *
 * 【MMU notifier 说明】
 * - MMU: Memory Management Unit (内存管理单元)
 * - notifier: 通知机制
 * - 当页表发生变化时通知订阅者
 *
 * 【为什么需要】
 * - KVM 虚拟化需要监控页表变化
 * - RDMA 设备需要同步页表状态
 * - 其他需要跟踪内存映射的子系统
 *
 * 【mmu_notifier_range】
 * - 描述页表变化的地址范围
 * - 包含起始地址、结束地址等信息
 * - 通知回调函数使用此结构
 */
struct mmu_notifier_range;

/*
 * 释放页全局目录 (PGD) 范围
 *
 * 【功能说明】释放指定地址范围内的页全局目录及其下级页表
 *
 * 【PGD 说明】
 * - PGD: Page Global Directory (页全局目录)
 * - 多级页表的最顶层
 * - x86-64: PGD -> PUD -> PMD -> PTE (四级页表)
 *
 * 【为什么这么设计】
 * - 页表占用大量内存
 * - 不用的页表应该释放
 * - 释放整个范围的所有层级
 *
 * 【mmu_gather 说明】
 * - MMU gather (TLB 批量收集)
 * - 批量收集要刷新的 TLB 条目
 * - 最后一次性刷新，提高性能
 *
 * 【floor 和 ceiling 说明】
 * - floor: 地板，不能释放低于此地址的页表
 * - ceiling: 天花板，不能释放高于此地址的页表
 * - 用于保护边界区域的页表
 *
 * 【参数说明】
 * @tlb: MMU gather 结构指针
 *       - 用于批量 TLB 刷新
 * @addr: 起始地址
 * @end: 结束地址
 * @floor: 下限地址，保护低地址页表
 * @ceiling: 上限地址，保护高地址页表
 */
void free_pgd_range(struct mmu_gather *tlb, unsigned long addr,
		unsigned long end, unsigned long floor, unsigned long ceiling);

/*
 * 复制页面范围
 *
 * 【功能说明】将源 VMA 的页表复制到目标 VMA
 *
 * 【为什么需要】
 * - fork() 系统调用需要复制父进程的页表
 * - 子进程继承父进程的内存映射
 * - 使用 COW (Copy-On-Write) 优化
 *
 * 【COW 优化】
 * - 复制时不复制页面内容
 * - 只复制页表项，指向相同物理页
 * - 标记为只读，写时才真正复制
 *
 * 【使用场景】
 * - fork() 复制地址空间
 * - 创建新进程
 * - 共享内存初始化
 *
 * 【参数说明】
 * @dst_vma: 目标虚拟内存区域
 *           - 新进程的 VMA
 * @src_vma: 源虚拟内存区域
 *           - 原进程的 VMA
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码 (例如内存不足)
 */
int
copy_page_range(struct vm_area_struct *dst_vma, struct vm_area_struct *src_vma);

/*
 * 通用物理地址访问
 *
 * 【功能说明】直接访问 VMA 映射的物理内存
 *
 * 【为什么需要】
 * - 某些场景需要绕过正常的页面故障机制
 * - 调试器访问进程内存
 * - /proc/<pid>/mem 文件实现
 *
 * 【与正常访问的区别】
 * - 正常访问通过虚拟地址，可能触发页面故障
 * - 此函数直接操作物理内存
 * - 即使页面不在内存也能访问
 *
 * 【使用场景】
 * - ptrace 调试接口
 * - /proc 文件系统
 * - 内核调试工具
 *
 * 【参数说明】
 * @vma: 虚拟内存区域指针
 * @addr: 虚拟地址
 * @buf: 数据缓冲区
 *       - 读取时存放数据
 *       - 写入时提供数据
 * @len: 长度 (字节)
 * @write: 访问类型
 *         - 0: 读取
 *         - 非 0: 写入
 *
 * 【返回值】
 * - 正数: 实际访问的字节数
 * - 负数: 错误码
 */
int generic_access_phys(struct vm_area_struct *vma, unsigned long addr,
			void *buf, int len, int write);

/*
 * 跟踪 PFNMAP 的参数结构
 *
 * 【功能说明】用于 follow_pfnmap 操作的参数和结果
 *
 * 【PFNMAP 说明】
 * - PFN: Page Frame Number (页帧号)
 * - MAP: 映射
 * - PFNMAP: 直接映射物理页帧号的 VMA (VM_PFNMAP)
 * - 不经过 struct page，直接映射物理地址
 *
 * 【为什么需要 PFNMAP】
 * - 设备内存 (显卡、网卡等) 没有 struct page
 * - 直接映射物理地址到用户空间
 * - 例如 mmap 设备文件
 *
 * 【结构体组织】
 * - Inputs: 调用者提供的输入参数
 * - Internals: 内部使用，调用者不应触碰
 * - Outputs: 函数返回的结果
 *
 * 【为什么这么设计】
 * - 清晰区分输入、内部、输出
 * - 避免调用者误用内部字段
 * - 便于扩展新的字段
 */
struct follow_pfnmap_args {
	/**
	 * Inputs:
	 * @vma: Pointer to @vm_area_struct struct
	 * @address: the virtual address to walk
	 */
	/**
	 * 输入参数:
	 * @vma: 指向 vm_area_struct 结构的指针
	 *       - 要查询的虚拟内存区域
	 * @address: 要遍历的虚拟地址
	 *           - 需要查询 PFN 的虚拟地址
	 */
	struct vm_area_struct *vma;
	unsigned long address;

	/**
	 * Internals:
	 *
	 * The caller shouldn't touch any of these.
	 */
	/**
	 * 内部字段:
	 *
	 * 调用者不应该触碰这些字段
	 *
	 * 【为什么需要这些内部字段】
	 * - lock: 保护页表项的自旋锁
	 *         - 页表访问需要加锁
	 *         - 防止并发修改
	 * - ptep: 指向页表项的指针
	 *         - 内部使用，查找 PFN
	 *         - 调用者不应直接访问
	 *
	 * 【spinlock_t 说明】
	 * - 自旋锁类型
	 * - 短时间持有的锁
	 * - 等待时忙等待 (spin)
	 *
	 * 【pte_t 说明】
	 * - Page Table Entry 类型
	 * - 架构相关的页表项
	 */
	spinlock_t *lock;
	pte_t *ptep;

	/**
	 * Outputs:
	 *
	 * @pfn: the PFN of the address
	 * @addr_mask: address mask covering pfn
	 * @pgprot: the pgprot_t of the mapping
	 * @writable: whether the mapping is writable
	 * @special: whether the mapping is a special mapping (real PFN maps)
	 */
	/**
	 * 输出结果:
	 *
	 * @pfn: 地址对应的页帧号
	 *       - Page Frame Number
	 *       - 物理页面的编号
	 *
	 * @addr_mask: 覆盖 pfn 的地址掩码
	 *             - 用于确定映射的粒度
	 *             - 例如大页的掩码更大
	 *
	 * @pgprot: 映射的页保护属性
	 *          - pgprot_t 类型
	 *          - 包含读/写/执行权限
	 *          - 包含缓存属性等
	 *
	 * @writable: 映射是否可写
	 *            - true: 可写
	 *            - false: 只读
	 *
	 * @special: 是否为特殊映射 (真正的 PFN 映射)
	 *           - true: VM_PFNMAP 等特殊映射
	 *           - false: 普通页面映射
	 *           - 特殊映射没有 struct page
	 */
	unsigned long pfn;
	unsigned long addr_mask;
	pgprot_t pgprot;
	bool writable;
	bool special;
};

/*
 * 开始 PFNMAP 跟踪
 *
 * 【功能说明】初始化并查询 PFNMAP 映射信息
 *
 * 【为什么需要】
 * - 获取 VM_PFNMAP 映射的 PFN
 * - 查询映射的权限和属性
 * - 需要持有锁保护页表
 *
 * 【使用流程】
 * 1. 填充 args 的输入字段 (vma, address)
 * 2. 调用 follow_pfnmap_start
 * 3. 使用 args 的输出字段 (pfn, pgprot 等)
 * 4. 调用 follow_pfnmap_end 释放锁
 *
 * 【参数说明】
 * @args: follow_pfnmap_args 结构指针
 *        - 输入: vma, address
 *        - 输出: pfn, addr_mask, pgprot, writable, special
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 *
 * 【注意事项】
 * - 必须与 follow_pfnmap_end 配对
 * - 持有锁期间不能睡眠
 * - 内部字段由函数管理
 */
int follow_pfnmap_start(struct follow_pfnmap_args *args);

/*
 * 结束 PFNMAP 跟踪
 *
 * 【功能说明】释放 follow_pfnmap_start 获取的锁
 *
 * 【为什么需要】
 * - 与 follow_pfnmap_start 配对
 * - 释放页表锁
 * - 清理内部状态
 *
 * 【参数说明】
 * @args: follow_pfnmap_args 结构指针
 *        - 必须是之前传给 follow_pfnmap_start 的同一个结构
 *
 * 【注意事项】
 * - 即使 follow_pfnmap_start 失败也应该调用
 * - 保证锁的正确释放
 * - 调用后不应再使用 args 的输出字段
 */
void follow_pfnmap_end(struct follow_pfnmap_args *args);

/*
 * 截断页面缓存
 *
 * 【功能说明】截断 inode 的页面缓存到新的大小
 *
 * 【为什么需要】
 * - 文件被截断 (truncate) 时
 * - 超出新大小的页面缓存变为无效
 * - 需要删除这些无效的缓存页面
 *
 * 【pagecache 说明】
 * - 页面缓存：内核缓存文件内容的内存
 * - 加速文件读写，避免重复磁盘 I/O
 * - 文件修改后缓存需要同步更新
 *
 * 【使用场景】
 * - truncate() 系统调用
 * - ftruncate() 系统调用
 * - 文件大小变化时
 *
 * 【参数说明】
 * @inode: 文件的 inode 指针
 *         - 包含文件元数据
 * @new: 新的文件大小 (字节)
 *       - loff_t 类型，支持大文件
 *       - 超过此大小的缓存被删除
 */
extern void truncate_pagecache(struct inode *inode, loff_t new);

/*
 * 设置截断大小
 *
 * 【功能说明】设置 inode 的新大小并截断页面缓存
 *
 * 【与 truncate_pagecache 的区别】
 * - truncate_setsize: 先更新 inode 大小，再截断缓存
 * - truncate_pagecache: 只截断缓存，不改 inode
 * - truncate_setsize 是更高层的接口
 *
 * 【为什么这么设计】
 * - 提供原子性的大小更新+缓存截断
 * - 避免大小和缓存不一致
 * - 简化调用者的操作
 *
 * 【参数说明】
 * @inode: 文件的 inode 指针
 * @newsize: 新的文件大小 (字节)
 *           - loff_t 类型
 */
extern void truncate_setsize(struct inode *inode, loff_t newsize);

/*
 * 页面缓存 inode 大小扩展
 *
 * 【功能说明】处理 inode 大小扩展时的页面缓存
 *
 * 【为什么需要】
 * - 文件大小增长时
 * - 需要处理新旧大小之间的页面
 * - 可能存在部分页面需要特殊处理
 *
 * 【与截断的区别】
 * - 截断是缩小，删除多余页面
 * - 扩展是增大，处理新增区域
 * - 扩展时原有数据保留
 *
 * 【参数说明】
 * @inode: 文件的 inode 指针
 * @from: 原始文件大小 (字节)
 *        - 扩展前的大小
 * @to: 新的文件大小 (字节)
 *      - 扩展后的大小
 *      - to > from
 */
void pagecache_isize_extended(struct inode *inode, loff_t from, loff_t to);

/*
 * 截断页面缓存范围
 *
 * 【功能说明】删除文件指定范围内的页面缓存
 *
 * 【与 truncate_pagecache 的区别】
 * - truncate_pagecache: 从某个偏移开始到文件末尾
 * - truncate_pagecache_range: 指定起始和结束偏移
 * - 可以删除文件中间的缓存
 *
 * 【使用场景】
 * - 打孔 (punch hole) 操作
 * - 删除文件中间的数据块
 * - fallocate() 的 FALLOC_FL_PUNCH_HOLE 模式
 *
 * 【参数说明】
 * @inode: 文件的 inode 指针
 * @offset: 起始偏移 (字节)
 *          - 范围的开始位置
 * @end: 结束偏移 (字节)
 *       - 范围的结束位置
 *       - 包含此偏移
 */
void truncate_pagecache_range(struct inode *inode, loff_t offset, loff_t end);

/*
 * 通用错误移除 folio
 *
 * 【功能说明】从地址空间中移除出错的 folio
 *
 * 【为什么需要】
 * - 页面缓存可能损坏或出错
 * - 硬件错误 (内存错误、磁盘错误)
 * - 需要从缓存中移除坏页
 *
 * 【address_space 说明】
 * - 文件或设备的页面缓存集合
 * - 管理页面缓存的结构
 * - 包含页面缓存树和操作方法
 *
 * 【使用场景】
 * - 检测到内存硬件错误
 * - I/O 读取错误
 * - 文件系统错误
 *
 * 【参数说明】
 * @mapping: 地址空间指针
 *           - folio 所属的地址空间
 * @folio: 要移除的 folio
 *         - 出错的页面
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
int generic_error_remove_folio(struct address_space *mapping,
		struct folio *folio);

/*
 * 锁定 mm 并查找 VMA
 *
 * 【功能说明】锁定内存描述符并查找指定地址的 VMA
 *
 * 【为什么这么设计】
 * - 页面故障处理需要原子性操作
 * - 先锁定 mm，防止并发修改
 * - 再查找 VMA，保证结果有效
 * - 两个操作必须原子进行
 *
 * 【mm_struct 说明】
 * - 进程的内存描述符
 * - 管理进程的所有 VMA
 * - 包含页表、内存统计等
 *
 * 【使用场景】
 * - 页面故障处理
 * - 需要同时锁定和查找
 * - 避免竞态条件
 *
 * 【pt_regs 说明】
 * - Processor Registers: 处理器寄存器
 * - 保存故障发生时的 CPU 状态
 * - 包含程序计数器、栈指针等
 * - 用于故障诊断和恢复
 *
 * 【参数说明】
 * @mm: 内存描述符指针
 *      - 要搜索的地址空间
 * @address: 虚拟地址
 *           - 要查找的地址
 * @regs: 处理器寄存器状态
 *        - 故障时的 CPU 状态
 *
 * 【返回值】
 * - 非 NULL: 找到的 VMA 指针，已持有 mm 锁
 * - NULL: 未找到 VMA
 *
 * 【注意事项】
 * - 返回非 NULL 时已持有锁
 * - 使用完毕后必须释放锁
 */
struct vm_area_struct *lock_mm_and_find_vma(struct mm_struct *mm,
		unsigned long address, struct pt_regs *regs);

/*
 * 【条件编译】MMU 配置的页面故障处理函数
 */
#ifdef CONFIG_MMU
/*
 * 处理内存故障
 *
 * 【功能说明】处理页面故障 (page fault)
 *
 * 【为什么需要】
 * - 访问未映射或无效的虚拟地址时触发
 * - 需要建立页表映射或加载页面
 * - 实现按需分页 (demand paging)
 *
 * 【页面故障类型】
 * - 缺页故障：页面不在内存，需要从磁盘加载
 * - 保护故障：违反权限，例如写只读页
 * - 非法地址：访问未映射区域
 *
 * 【按需分页】
 * - 进程启动时不加载所有页面
 * - 首次访问时才分配和加载
 * - 节省内存，加快启动速度
 *
 * 【vm_fault_t 类型】
 * - 故障处理结果的返回类型
 * - 包含多种状态码
 * - 例如 VM_FAULT_OOM, VM_FAULT_SIGBUS 等
 *
 * 【参数说明】
 * @vma: 发生故障的虚拟内存区域
 * @address: 触发故障的虚拟地址
 * @flags: 故障标志
 *         - FAULT_FLAG_WRITE: 写访问
 *         - FAULT_FLAG_USER: 用户空间访问
 *         - 等等
 * @regs: 处理器寄存器状态
 *        - 故障时的 CPU 状态
 *
 * 【返回值】
 * - VM_FAULT_* 系列常量
 * - 指示故障处理结果
 */
extern vm_fault_t handle_mm_fault(struct vm_area_struct *vma,
				  unsigned long address, unsigned int flags,
				  struct pt_regs *regs);

/*
 * 修复用户空间故障
 *
 * 【功能说明】在内核上下文中修复用户空间的页面故障
 *
 * 【为什么需要】
 * - 内核代码访问用户空间内存时
 * - 可能触发用户空间的页面故障
 * - 需要在内核上下文中处理
 *
 * 【使用场景】
 * - copy_from_user/copy_to_user 实现
 * - get_user_pages 系列函数
 * - 内核需要访问用户内存时
 *
 * 【unlocked 参数说明】
 * - 输出参数，表示是否释放了 mmap_lock
 * - true: 函数中释放了锁 (可能睡眠等待)
 * - false: 持有锁返回
 * - 调用者根据此值决定是否重新加锁
 *
 * 【参数说明】
 * @mm: 内存描述符指针
 * @address: 故障地址
 * @fault_flags: 故障标志
 *               - FAULT_FLAG_* 系列
 * @unlocked: 输出参数，指示是否释放了锁
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
extern int fixup_user_fault(struct mm_struct *mm,
			    unsigned long address, unsigned int fault_flags,
			    bool *unlocked);

/*
 * 解除映射的页面
 *
 * 【功能说明】解除地址空间中指定页面索引范围的映射
 *
 * 【为什么需要】
 * - 文件内容改变时，使页面缓存失效
 * - 截断文件时，删除超出部分的映射
 * - 保持映射和文件内容一致
 *
 * 【pgoff_t 说明】
 * - Page Offset: 页面偏移类型
 * - 文件中的页面索引 (不是字节偏移)
 * - 例如 pgoff=0 是文件的第一个页面
 *
 * 【even_cows 说明】
 * - 是否解除 COW (Copy-On-Write) 的私有页面
 * - true: 也解除私有页面映射
 * - false: 只解除共享页面映射
 *
 * 【参数说明】
 * @mapping: 地址空间指针
 *           - 文件的页面缓存
 * @start: 起始页面索引
 * @nr: 页面数量
 * @even_cows: 是否包括 COW 私有页面
 */
void unmap_mapping_pages(struct address_space *mapping,
		pgoff_t start, pgoff_t nr, bool even_cows);

/*
 * 解除映射范围
 *
 * 【功能说明】解除地址空间中指定字节范围的映射
 *
 * 【与 unmap_mapping_pages 的区别】
 * - unmap_mapping_pages: 以页面索引为单位
 * - unmap_mapping_range: 以字节偏移为单位
 * - unmap_mapping_range 是更高层的接口
 *
 * 【参数说明】
 * @mapping: 地址空间指针
 * @holebegin: 起始字节偏移
 *             - loff_t 类型，支持大文件
 *             - const 表示不会修改
 * @holelen: 长度 (字节)
 *           - 0 表示到文件末尾
 * @even_cows: 是否包括 COW 私有页面
 */
void unmap_mapping_range(struct address_space *mapping,
		loff_t const holebegin, loff_t const holelen, int even_cows);

#else
/*
 * 无 MMU 系统的 handle_mm_fault 实现
 *
 * 【为什么这样实现】
 * - 无 MMU 系统没有虚拟内存和页面故障
 * - 如果调用到这里说明代码有问题
 * - BUG() 触发内核崩溃，帮助发现错误
 *
 * 【VM_FAULT_SIGBUS】
 * - 总线错误 (Bus Error) 故障码
 * - 通常表示硬件访问错误
 * - 这里作为错误返回值
 */
static inline vm_fault_t handle_mm_fault(struct vm_area_struct *vma,
					 unsigned long address, unsigned int flags,
					 struct pt_regs *regs)
{
	/* should never happen if there's no MMU */
	/* 如果没有 MMU 就不应该发生 */
	BUG();
	return VM_FAULT_SIGBUS;
}

/*
 * 无 MMU 系统的 fixup_user_fault 实现
 *
 * 【EFAULT 说明】
 * - Error Fault: 故障错误
 * - 表示内存访问失败
 * - 标准的错误码
 */
static inline int fixup_user_fault(struct mm_struct *mm, unsigned long address,
		unsigned int fault_flags, bool *unlocked)
{
	/* should never happen if there's no MMU */
	/* 如果没有 MMU 就不应该发生 */
	BUG();
	return -EFAULT;
}

/*
 * 无 MMU 系统的 unmap_mapping_pages 实现
 *
 * 【为什么是空实现】
 * - 无 MMU 系统没有页表映射
 * - 不需要解除映射操作
 * - 空函数避免编译错误
 */
static inline void unmap_mapping_pages(struct address_space *mapping,
		pgoff_t start, pgoff_t nr, bool even_cows) { }

/*
 * 无 MMU 系统的 unmap_mapping_range 实现
 */
static inline void unmap_mapping_range(struct address_space *mapping,
		loff_t const holebegin, loff_t const holelen, int even_cows) { }
#endif

/*
 * 解除共享映射范围
 *
 * 【功能说明】解除地址空间中共享映射的指定范围
 *
 * 【为什么这么设计】
 * - 提供专门的共享映射解除接口
 * - 只影响共享页面，不影响私有 COW 页面
 * - 简化调用者的操作
 *
 * 【与 unmap_mapping_range 的关系】
 * - 这是 unmap_mapping_range 的包装
 * - 最后一个参数传 0，表示不解除 COW 私有页面
 * - even_cows=0: 只解除共享映射
 *
 * 【使用场景】
 * - 文件截断时使共享映射失效
 * - 文件内容改变时刷新共享映射
 * - 不影响进程的私有修改
 *
 * 【inline 说明】
 * - 简单的包装函数
 * - 编译器会内联展开
 * - 没有函数调用开销
 *
 * 【参数说明】
 * @mapping: 地址空间指针
 * @holebegin: 起始字节偏移
 * @holelen: 长度 (字节)
 */
static inline void unmap_shared_mapping_range(struct address_space *mapping,
		loff_t const holebegin, loff_t const holelen)
{
	unmap_mapping_range(mapping, holebegin, holelen, 0);
}

/*
 * VMA 查找函数的前向声明
 *
 * 【功能说明】在内存描述符中查找包含指定地址的 VMA
 *
 * 【为什么需要前向声明】
 * - 函数定义在文件后面
 * - 这里需要先声明，供前面的代码使用
 * - 避免编译顺序问题
 *
 * 【vma_lookup 与其他查找函数的区别】
 * - vma_lookup: 简单查找，不加锁
 * - lock_mm_and_find_vma: 加锁后查找
 * - find_vma: 查找包含或在地址后的第一个 VMA
 *
 * 【参数说明】
 * @mm: 内存描述符指针
 * @addr: 要查找的虚拟地址
 *
 * 【返回值】
 * - 非 NULL: 包含此地址的 VMA
 * - NULL: 没有 VMA 包含此地址
 */
static inline struct vm_area_struct *vma_lookup(struct mm_struct *mm,
						unsigned long addr);

/*
 * 访问进程虚拟内存
 *
 * 【功能说明】从另一个进程的地址空间读取或写入数据
 *
 * 【为什么需要】
 * - ptrace 调试需要读写被调试进程的内存
 * - /proc/<pid>/mem 文件实现
 * - 进程间调试和监控
 *
 * 【task_struct 说明】
 * - 进程控制块 (Process Control Block)
 * - 描述一个进程的所有信息
 * - 包含 mm_struct 等内存信息
 *
 * 【gup_flags 说明】
 * - GUP: Get User Pages (获取用户页面)
 * - 控制页面获取行为的标志
 * - FOLL_WRITE: 写访问
 * - FOLL_FORCE: 强制访问 (调试用)
 * - 等等
 *
 * 【参数说明】
 * @tsk: 目标任务 (进程) 指针
 *       - 要访问的进程
 * @addr: 虚拟地址
 *        - 目标进程中的地址
 * @buf: 数据缓冲区
 *       - 读取时存放数据
 *       - 写入时提供数据
 * @len: 长度 (字节)
 * @gup_flags: 获取用户页面的标志
 *
 * 【返回值】
 * - 正数: 实际访问的字节数
 * - 0: 无法访问
 */
extern int access_process_vm(struct task_struct *tsk, unsigned long addr,
		void *buf, int len, unsigned int gup_flags);

/*
 * 访问远程虚拟内存
 *
 * 【功能说明】访问指定内存描述符的虚拟地址空间
 *
 * 【与 access_process_vm 的区别】
 * - access_process_vm: 通过 task_struct 访问
 * - access_remote_vm: 直接通过 mm_struct 访问
 * - access_remote_vm 更底层，更直接
 *
 * 【为什么叫"remote"】
 * - 访问的是其他地址空间的内存
 * - 相对于当前进程是"远程"的
 * - 不是网络意义的远程
 *
 * 【参数说明】
 * @mm: 内存描述符指针
 *      - 目标地址空间
 * @addr: 虚拟地址
 * @buf: 数据缓冲区
 * @len: 长度 (字节)
 * @gup_flags: 获取用户页面的标志
 *
 * 【返回值】
 * - 正数: 实际访问的字节数
 * - 0: 无法访问
 */
extern int access_remote_vm(struct mm_struct *mm, unsigned long addr,
		void *buf, int len, unsigned int gup_flags);

/*
 * 【条件编译】BPF 系统调用配置
 */
#ifdef CONFIG_BPF_SYSCALL
/*
 * 复制远程虚拟内存字符串
 *
 * 【功能说明】从另一个进程的地址空间复制字符串
 *
 * 【为什么需要】
 * - BPF (Berkeley Packet Filter) 程序需要读取进程字符串
 * - 跟踪和监控需要获取进程的字符串数据
 * - 例如读取进程的命令行参数、文件路径等
 *
 * 【BPF 说明】
 * - 内核中的虚拟机和程序执行环境
 * - 用于网络过滤、跟踪、安全等
 * - 可以安全地在内核中执行用户编写的程序
 *
 * 【与普通内存访问的区别】
 * - 专门针对字符串优化
 * - 自动处理字符串结束符
 * - 避免读取过多数据
 *
 * 【参数说明】
 * @tsk: 目标任务 (进程) 指针
 * @addr: 字符串的虚拟地址
 *        - 目标进程中的地址
 * @buf: 数据缓冲区
 *       - 存放复制的字符串
 * @len: 缓冲区长度 (字节)
 *       - 最多复制的字节数
 * @gup_flags: 获取用户页面的标志
 *
 * 【返回值】
 * - 正数: 实际复制的字节数 (包括结束符)
 * - 负数: 错误码
 */
extern int copy_remote_vm_str(struct task_struct *tsk, unsigned long addr,
			      void *buf, int len, unsigned int gup_flags);
#endif

/*
 * 获取远程用户页面
 *
 * 【功能说明】获取另一个地址空间中的用户页面
 *
 * 【为什么需要】
 * - 内核需要直接访问用户空间的物理页面
 * - 例如 DMA 传输需要页面的物理地址
 * - 避免页面在访问期间被交换出去
 *
 * 【GUP 机制】
 * - GUP: Get User Pages
 * - 增加页面引用计数，防止被释放
 * - 确保页面在内存中可用
 * - 使用完毕后必须调用 put_page 释放
 *
 * 【与 pin_user_pages_remote 的区别】
 * - get_user_pages: 普通引用，允许页面迁移
 * - pin_user_pages: 固定引用，防止页面移动
 * - DMA 等需要稳定物理地址时用 pin
 *
 * 【参数说明】
 * @mm: 内存描述符指针
 *      - 目标地址空间
 * @start: 起始虚拟地址
 * @nr_pages: 页面数量
 *            - 要获取多少个页面
 * @gup_flags: 获取用户页面的标志
 *             - FOLL_WRITE: 写访问
 *             - FOLL_FORCE: 强制访问
 *             - 等等
 * @pages: 页面指针数组
 *         - 输出参数，存放获取的页面
 *         - 调用者分配数组
 * @locked: 锁状态指针
 *          - 输出参数，指示 mmap_lock 的状态
 *          - 可以为 NULL
 *
 * 【返回值】
 * - 正数: 实际获取的页面数量
 * - 负数: 错误码
 *
 * 【注意事项】
 * - 获取的页面必须用 put_page 释放
 * - 否则会导致内存泄漏
 */
long get_user_pages_remote(struct mm_struct *mm,
			   unsigned long start, unsigned long nr_pages,
			   unsigned int gup_flags, struct page **pages,
			   int *locked);

/*
 * 固定远程用户页面
 *
 * 【功能说明】固定另一个地址空间中的用户页面
 *
 * 【PIN 机制】
 * - PIN: 固定页面，防止移动和迁移
 * - 比普通 GUP 更强的保证
 * - 确保物理地址不变
 *
 * 【为什么需要 PIN】
 * - DMA 传输需要稳定的物理地址
 * - RDMA 需要长期固定页面
 * - 某些硬件操作不能处理页面迁移
 *
 * 【与 get_user_pages_remote 的关键区别】
 * - get: 允许页面迁移、合并、拆分
 * - pin: 阻止页面移动，保持物理地址
 * - pin 开销更大，仅在必要时使用
 *
 * 【参数说明】
 * @mm: 内存描述符指针
 * @start: 起始虚拟地址
 * @nr_pages: 页面数量
 * @gup_flags: 获取用户页面的标志
 * @pages: 页面指针数组 (输出)
 * @locked: 锁状态指针 (输出)
 *
 * 【返回值】
 * - 正数: 实际固定的页面数量
 * - 负数: 错误码
 *
 * 【注意事项】
 * - 固定的页面必须用 unpin_user_page 释放
 * - 不能用 put_page，必须用 unpin
 * - 否则引用计数会出错
 */
long pin_user_pages_remote(struct mm_struct *mm,
			   unsigned long start, unsigned long nr_pages,
			   unsigned int gup_flags, struct page **pages,
			   int *locked);

/*
 * Retrieves a single page alongside its VMA. Does not support FOLL_NOWAIT.
 */
/*
 * 获取单个用户页面及其 VMA
 *
 * 【功能说明】获取远程地址空间的单个页面，同时返回其所属的 VMA
 *
 * 【为什么这么设计】
 * - 很多场景只需要一个页面
 * - 同时需要知道页面的 VMA 信息
 * - 提供便捷的单页面接口
 *
 * 【与 get_user_pages_remote 的区别】
 * - get_user_pages_remote: 可以获取多个页面
 * - get_user_page_vma_remote: 只获取一个页面
 * - 额外返回 VMA 信息
 *
 * 【FOLL_NOWAIT 不支持】
 * - FOLL_NOWAIT: 非阻塞模式，不等待页面
 * - 此函数不支持此标志
 * - 调用者不应传递 FOLL_NOWAIT
 *
 * 【实现原理】
 * - 内部调用 get_user_pages_remote 获取一个页面
 * - gup_flags 强制设置 FOLL_WRITE 和 FOLL_FORCE
 * - 返回单个页面和其 VMA
 *
 * 【参数说明】
 * @mm: 内存描述符指针
 * @addr: 虚拟地址
 * @gup_flags: 获取用户页面的标志
 * @vma: 输出参数，返回 VMA 指针
 *
 * 【返回值】
 * - 非 NULL: 获取的页面指针
 * - NULL: 获取失败
 *
 * 【注意事项】
 * - 返回的页面必须用 put_page 释放
 * - 不支持 FOLL_NOWAIT 标志
 */
static inline struct page *get_user_page_vma_remote(struct mm_struct *mm,
						    unsigned long addr,
						    int gup_flags,
						    struct vm_area_struct **vmap)
{
	/*
	 * 【局部变量声明】
	 */
	struct page *page;                     /* 获取的页面指针 */
	struct vm_area_struct *vma;            /* 查找到的 VMA */
	int got;                               /* get_user_pages_remote 的返回值 */

	/*
	 * 【FOLL_NOWAIT 检查】
	 * - FOLL_NOWAIT: 非阻塞标志，不等待页面
	 * - 此函数不支持非阻塞模式
	 * - unlikely: 提示编译器这是不常见的情况
	 * - WARN_ON_ONCE: 警告一次，帮助调试
	 * - 返回 -EINVAL (无效参数) 错误
	 */
	if (WARN_ON_ONCE(unlikely(gup_flags & FOLL_NOWAIT)))
		return ERR_PTR(-EINVAL);

	/*
	 * 【获取单个页面】
	 * - 调用 get_user_pages_remote 获取一个页面
	 * - 参数 1: 只获取一个页面
	 * - &page: 输出参数，存放页面指针
	 * - NULL: 不需要 locked 输出
	 */
	got = get_user_pages_remote(mm, addr, 1, gup_flags, &page, NULL);

	/*
	 * 【错误处理】
	 * - got < 0: 获取失败
	 * - ERR_PTR: 将错误码转换为错误指针
	 */
	if (got < 0)
		return ERR_PTR(got);

	/*
	 * 【查找 VMA】
	 * - vma_lookup: 查找包含此地址的 VMA
	 * - 正常情况下应该能找到 (因为页面已经获取成功)
	 */
	vma = vma_lookup(mm, addr);
	/*
	 * 【VMA 缺失处理】
	 * - 如果找不到 VMA，说明状态不一致
	 * - put_page: 释放已获取的页面，避免泄漏
	 * - WARN_ON_ONCE: 警告，这是异常情况
	 * - 返回 -EINVAL 错误
	 */
	if (WARN_ON_ONCE(!vma)) {
		put_page(page);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * 【返回结果】
	 * - *vmap = vma: 输出 VMA 指针
	 * - return page: 返回页面指针
	 */
	*vmap = vma;
	return page;
}

/*
 * 获取当前进程的用户页面
 *
 * 【功能说明】从当前进程的地址空间获取用户页面
 *
 * 【与 get_user_pages_remote 的区别】
 * - get_user_pages_remote: 访问其他进程的地址空间，需要传递 mm
 * - get_user_pages: 访问当前进程的地址空间，使用 current->mm
 * - get_user_pages 更简单，不需要指定 mm
 *
 * 【为什么需要】
 * - 当前进程访问自己的用户空间页面
 * - 例如直接 I/O 操作
 * - 内核代码需要固定用户页面时
 *
 * 【参数说明】
 * @start: 起始虚拟地址
 * @nr_pages: 页面数量
 * @gup_flags: 获取用户页面的标志
 * @pages: 页面指针数组 (输出)
 *
 * 【返回值】
 * - 正数: 实际获取的页面数量
 * - 负数: 错误码
 */
long get_user_pages(unsigned long start, unsigned long nr_pages,
		    unsigned int gup_flags, struct page **pages);

/*
 * 固定当前进程的用户页面
 *
 * 【功能说明】从当前进程的地址空间固定用户页面
 *
 * 【与 pin_user_pages_remote 的区别】
 * - pin_user_pages_remote: 固定其他进程的页面
 * - pin_user_pages: 固定当前进程的页面
 *
 * 【参数说明】
 * @start: 起始虚拟地址
 * @nr_pages: 页面数量
 * @gup_flags: 获取用户页面的标志
 * @pages: 页面指针数组 (输出)
 *
 * 【返回值】
 * - 正数: 实际固定的页面数量
 * - 负数: 错误码
 */
long pin_user_pages(unsigned long start, unsigned long nr_pages,
		    unsigned int gup_flags, struct page **pages);

/*
 * 无锁获取用户页面
 *
 * 【功能说明】获取用户页面，不持有 mmap_lock
 *
 * 【为什么需要】
 * - 某些场景不能持有 mmap_lock
 * - 避免死锁或提高并发性
 * - 内部会临时获取和释放锁
 *
 * 【与 get_user_pages 的区别】
 * - get_user_pages: 调用者需要持有 mmap_lock
 * - get_user_pages_unlocked: 不需要预先持有锁
 *
 * 【参数说明】
 * @start: 起始虚拟地址
 * @nr_pages: 页面数量
 * @pages: 页面指针数组 (输出)
 * @gup_flags: 获取用户页面的标志
 *
 * 【返回值】
 * - 正数: 实际获取的页面数量
 * - 负数: 错误码
 */
long get_user_pages_unlocked(unsigned long start, unsigned long nr_pages,
		    struct page **pages, unsigned int gup_flags);

/*
 * 无锁固定用户页面
 *
 * 【功能说明】固定用户页面，不持有 mmap_lock
 *
 * 【参数说明】
 * @start: 起始虚拟地址
 * @nr_pages: 页面数量
 * @pages: 页面指针数组 (输出)
 * @gup_flags: 获取用户页面的标志
 *
 * 【返回值】
 * - 正数: 实际固定的页面数量
 * - 负数: 错误码
 */
long pin_user_pages_unlocked(unsigned long start, unsigned long nr_pages,
		    struct page **pages, unsigned int gup_flags);

/*
 * 固定内存文件描述符的 folio
 *
 * 【功能说明】从 memfd (内存文件描述符) 固定 folio
 *
 * 【memfd 说明】
 * - memfd: Memory File Descriptor (内存文件描述符)
 * - memfd_create 系统调用创建
 * - 匿名内存，但有文件接口
 * - 可以在进程间共享
 *
 * 【为什么需要】
 * - 需要长期访问 memfd 中的页面
 * - DMA 或 RDMA 操作需要稳定的物理地址
 * - 防止页面被交换或移动
 *
 * 【参数说明】
 * @memfd: 内存文件描述符的 file 指针
 * @start: 起始字节偏移
 *         - loff_t 类型，支持大文件
 * @end: 结束字节偏移
 * @folios: folio 指针数组 (输出)
 *          - 存放固定的 folio
 * @max_folios: 数组最大容量
 *              - 最多固定多少个 folio
 * @offset: 页面偏移 (输出)
 *          - 返回第一个 folio 的页面索引
 *
 * 【返回值】
 * - 正数: 实际固定的 folio 数量
 * - 负数: 错误码
 */
long memfd_pin_folios(struct file *memfd, loff_t start, loff_t end,
		      struct folio **folios, unsigned int max_folios,
		      pgoff_t *offset);

/*
 * 为 folio 添加多个固定引用
 *
 * 【功能说明】一次性为 folio 增加多个 pin 引用计数
 *
 * 【为什么需要】
 * - 批量固定引用，比逐个添加高效
 * - 减少原子操作次数
 * - 适合批量处理场景
 *
 * 【参数说明】
 * @folio: 要添加引用的 folio
 * @pins: 要添加的引用数量
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
int folio_add_pins(struct folio *folio, unsigned int pins);

/*
 * 快速获取用户页面
 *
 * 【功能说明】快速路径获取用户页面
 *
 * 【为什么叫 "fast"】
 * - 优化的快速路径实现
 * - 尽量不加锁或使用轻量级锁
 * - 避免复杂的页面错误处理
 * - 适合页面已经在内存中的情况
 *
 * 【与 get_user_pages 的区别】
 * - get_user_pages: 需要持有 mmap_lock，完整的页面错误处理
 * - get_user_pages_fast: 快速路径，优化的锁策略
 *
 * 【使用场景】
 * - 性能敏感的路径
 * - 页面很可能已经在内存中
 * - 需要快速响应
 *
 * 【参数说明】
 * @start: 起始虚拟地址
 * @nr_pages: 页面数量
 *            - 注意这里是 int 类型，不是 unsigned long
 * @gup_flags: 获取用户页面的标志
 * @pages: 页面指针数组 (输出)
 *
 * 【返回值】
 * - 正数: 实际获取的页面数量
 * - 负数: 错误码
 */
int get_user_pages_fast(unsigned long start, int nr_pages,
			unsigned int gup_flags, struct page **pages);

/*
 * 快速固定用户页面
 *
 * 【功能说明】快速路径固定用户页面
 *
 * 【参数说明】
 * @start: 起始虚拟地址
 * @nr_pages: 页面数量
 * @gup_flags: 获取用户页面的标志
 * @pages: 页面指针数组 (输出)
 *
 * 【返回值】
 * - 正数: 实际固定的页面数量
 * - 负数: 错误码
 */
int pin_user_pages_fast(unsigned long start, int nr_pages,
			unsigned int gup_flags, struct page **pages);

/*
 * 为 folio 添加单个固定引用
 *
 * 【功能说明】为 folio 增加一个 pin 引用计数
 *
 * 【与 folio_add_pins 的区别】
 * - folio_add_pin: 添加单个引用
 * - folio_add_pins: 批量添加多个引用
 *
 * 【参数说明】
 * @folio: 要添加引用的 folio
 *
 * 【返回值】
 * - 无返回值 (void)
 */
void folio_add_pin(struct folio *folio);

/*
 * 统计锁定的虚拟内存
 *
 * 【功能说明】更新进程锁定的虚拟内存统计
 *
 * 【锁定内存说明】
 * - 锁定内存 (Locked Memory): 不能被换出到交换区的内存
 * - mlock 系统调用锁定内存
 * - 需要统计以执行资源限制 (RLIMIT_MEMLOCK)
 *
 * 【为什么需要统计】
 * - 防止用户锁定过多内存
 * - RLIMIT_MEMLOCK 限制每个用户可以锁定的内存量
 * - 避免系统内存不足
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pages: 页面数量 (增加或减少)
 * @inc: true 表示增加，false 表示减少
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码 (例如超过限制)
 */
int account_locked_vm(struct mm_struct *mm, unsigned long pages, bool inc);

/*
 * 内部锁定内存统计函数
 *
 * 【功能说明】锁定内存统计的内部实现
 *
 * 【与 account_locked_vm 的区别】
 * - account_locked_vm: 简化版，使用当前进程
 * - __account_locked_vm: 完整版，可以指定任务和绕过限制
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pages: 页面数量
 * @inc: true 表示增加，false 表示减少
 * @task: 任务结构体指针
 *        - 用于检查资源限制
 * @bypass_rlim: 是否绕过资源限制
 *               - true: 不检查 RLIMIT_MEMLOCK
 *               - false: 检查限制
 *
 * 【为什么需要 bypass_rlim】
 * - 某些内核操作需要锁定内存但不应受用户限制约束
 * - 例如内核内部的 DMA 缓冲区
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
int __account_locked_vm(struct mm_struct *mm, unsigned long pages, bool inc,
			const struct task_struct *task, bool bypass_rlim);

/*
 * kvec 结构体前向声明
 *
 * 【kvec 说明】
 * - kvec: Kernel Vector (内核向量)
 * - 用于描述内核空间的内存块
 * - 类似于 iovec，但用于内核
 * - 定义在 <linux/uio.h>
 */
struct kvec;

/*
 * 获取核心转储页面
 *
 * 【功能说明】获取用于核心转储 (core dump) 的页面
 *
 * 【核心转储说明】
 * - core dump: 进程崩溃时保存的内存镜像
 * - 用于调试和事后分析
 * - 包含进程的内存内容
 *
 * 【为什么需要特殊函数】
 * - 核心转储需要访问崩溃进程的内存
 * - 可能需要特殊处理（例如锁定）
 * - 要避免在转储过程中再次崩溃
 *
 * 【参数说明】
 * @addr: 要转储的虚拟地址
 * @locked: 锁定状态 (输入/输出)
 *          - 输入: 当前是否持有锁
 *          - 输出: 返回后的锁定状态
 *
 * 【返回值】
 * - 非 NULL: 页面指针
 * - NULL: 无法获取页面
 */
struct page *get_dump_page(unsigned long addr, int *locked);

/*
 * 标记 folio 为脏
 *
 * 【功能说明】将 folio 标记为脏 (已修改)
 *
 * 【脏页说明】
 * - 脏页 (Dirty Page): 内存中的内容与磁盘不一致
 * - 需要写回磁盘才能保证数据不丢失
 * - 内核会定期刷新脏页到磁盘
 *
 * 【为什么需要标记】
 * - 告诉内核这个页面已经被修改
 * - 内核会在适当时候写回磁盘
 * - 保证数据持久化
 *
 * 【参数说明】
 * @folio: 要标记的 folio
 *
 * 【返回值】
 * - true: 页面之前是干净的，现在标记为脏
 * - false: 页面之前已经是脏的
 */
bool folio_mark_dirty(struct folio *folio);

/*
 * 加锁标记 folio 为脏
 *
 * 【功能说明】先加锁，然后标记 folio 为脏
 *
 * 【与 folio_mark_dirty 的区别】
 * - folio_mark_dirty: 不加锁，调用者需要确保同步
 * - folio_mark_dirty_lock: 内部加锁，可以安全并发调用
 *
 * 【参数说明】
 * @folio: 要标记的 folio
 *
 * 【返回值】
 * - true: 成功标记为脏
 * - false: 页面之前已经是脏的
 */
bool folio_mark_dirty_lock(struct folio *folio);

/*
 * 标记页面为脏
 *
 * 【功能说明】将页面标记为脏 (已修改)
 *
 * 【与 folio_mark_dirty 的区别】
 * - folio_mark_dirty: 操作 folio（可能包含多个页面）
 * - set_page_dirty: 操作单个页面
 * - set_page_dirty 是传统接口，folio_mark_dirty 是新接口
 *
 * 【参数说明】
 * @page: 要标记的页面
 *
 * 【返回值】
 * - true: 成功标记为脏
 * - false: 页面之前已经是脏的
 */
bool set_page_dirty(struct page *page);

/*
 * 加锁标记页面为脏
 *
 * 【功能说明】先加锁，然后标记页面为脏
 *
 * 【参数说明】
 * @page: 要标记的页面
 *
 * 【返回值】
 * - 正数: 成功
 * - 0: 失败
 */
int set_page_dirty_lock(struct page *page);

/*
 * 获取进程的命令行
 *
 * 【功能说明】读取进程的命令行参数
 *
 * 【命令行说明】
 * - 进程启动时的 argv 参数
 * - 存储在进程的用户空间
 * - 内核需要从用户空间读取
 *
 * 【使用场景】
 * - 进程监控工具 (如 ps, top)
 * - 调试和诊断
 * - /proc/<pid>/cmdline 接口的实现
 *
 * 【参数说明】
 * @task: 目标进程的任务结构体
 * @buffer: 输出缓冲区
 * @buflen: 缓冲区长度
 *
 * 【返回值】
 * - 正数: 实际读取的字节数
 * - 负数: 错误码
 */
int get_cmdline(struct task_struct *task, char *buffer, int buflen);

/*
 * change_protection() 使用的标志
 *
 * 【功能说明】用于 change_protection() 函数的位图标志
 *
 * 【change_protection 说明】
 * - 改变内存区域的保护属性
 * - 例如改变可读、可写、可执行权限
 * - 用于 mprotect 系统调用的实现
 *
 * 【设计说明】
 * - 现在使用位图，可以传递多个标志
 * - 但目前所有调用者每次只使用一个标志
 * - 为将来扩展做准备
 */
/*
 * Flags used by change_protection().  For now we make it a bitmap so
 * that we can pass in multiple flags just like parameters.  However
 * for now all the callers are only use one of the flags at the same
 * time.
 */
/*
 * 是否应该手动检查单个 PTE 是否可写
 *
 * 【说明】
 * - 某些情况下（如 COW、uffd-wp）阻止自动使所有 PTE 可写
 * - 需要逐个检查 PTE 是否可以映射为可写
 *
 * 【COW 说明】
 * - COW: Copy-On-Write (写时复制)
 * - fork 后父子进程共享页面，标记为只读
 * - 写入时触发页面错误，复制页面
 *
 * 【uffd-wp 说明】
 * - uffd-wp: userfaultfd write-protect (用户错误处理写保护)
 * - 用户空间可以监控页面写入
 * - 用于实现自定义的写保护策略
 */
/*
 * Whether we should manually check if we can map individual PTEs writable,
 * because something (e.g., COW, uffd-wp) blocks that from happening for all
 * PTEs automatically in a writable mapping.
 */
#define  MM_CP_TRY_CHANGE_WRITABLE	   (1UL << 0)

/*
 * 保护变更是否用于 NUMA 提示
 *
 * 【NUMA 提示说明】
 * - NUMA: Non-Uniform Memory Access (非一致性内存访问)
 * - 将页面临时标记为不可访问
 * - 触发页面错误时收集 NUMA 访问信息
 * - 用于优化页面在 NUMA 节点间的放置
 *
 * 【为什么需要】
 * - 自动 NUMA 平衡
 * - 将页面迁移到访问它的 CPU 附近
 * - 提高 NUMA 系统性能
 */
/* Whether this protection change is for NUMA hints */
#define  MM_CP_PROT_NUMA                   (1UL << 1)

/*
 * 保护变更是否用于写保护
 *
 * 【说明】
 * - 设置写保护
 */
/* Whether this change is for write protecting */
#define  MM_CP_UFFD_WP                     (1UL << 2) /* do wp */

/*
 * 保护变更是否用于解除写保护
 *
 * 【说明】
 * - 解除写保护
 */
#define  MM_CP_UFFD_WP_RESOLVE             (1UL << 3) /* Resolve wp */

/*
 * 所有 userfaultfd 写保护相关标志
 *
 * 【说明】
 * - 包含 MM_CP_UFFD_WP 和 MM_CP_UFFD_WP_RESOLVE
 * - 用于检查是否有任何 uffd-wp 操作
 */
#define  MM_CP_UFFD_WP_ALL                 (MM_CP_UFFD_WP | \
					    MM_CP_UFFD_WP_RESOLVE)

/*
 * 检查 PTE 是否可以改为可写
 *
 * 【功能说明】判断给定的 PTE 是否可以安全地修改为可写
 *
 * 【为什么需要】
 * - COW 页面不能直接改为可写
 * - uffd-wp 保护的页面需要特殊处理
 * - 需要检查各种约束条件
 *
 * 【参数说明】
 * @vma: 虚拟内存区域
 * @addr: 虚拟地址
 * @pte: 页表项
 *
 * 【返回值】
 * - true: 可以改为可写
 * - false: 不能改为可写
 */
bool can_change_pte_writable(struct vm_area_struct *vma, unsigned long addr,
			     pte_t pte);

/*
 * 改变内存保护属性
 *
 * 【功能说明】改变指定范围内的内存保护属性
 *
 * 【使用场景】
 * - mprotect 系统调用的实现
 * - NUMA 自动平衡
 * - userfaultfd 写保护
 *
 * 【参数说明】
 * @tlb: TLB gather 结构体
 *       - 用于批量刷新 TLB
 * @vma: 虚拟内存区域
 * @start: 起始地址
 * @end: 结束地址
 * @cp_flags: change_protection 标志 (MM_CP_*)
 *
 * 【返回值】
 * - 正数: 实际改变的页面数量
 * - 负数: 错误码
 */
extern long change_protection(struct mmu_gather *tlb,
			      struct vm_area_struct *vma, unsigned long start,
			      unsigned long end, unsigned long cp_flags);

/*
 * 修复 mprotect 的 VMA
 *
 * 【功能说明】mprotect 系统调用的核心实现
 *
 * 【mprotect 说明】
 * - mprotect: 修改内存保护属性的系统调用
 * - 可以改变可读、可写、可执行权限
 * - 必须在页面边界上操作
 *
 * 【为什么叫 "fixup"】
 * - 需要调整 VMA 结构
 * - 可能需要分割或合并 VMA
 * - 更新页表项的保护位
 *
 * 【参数说明】
 * @vmi: VMA 迭代器
 * @tlb: TLB gather 结构体
 * @vma: 要修改的 VMA
 * @pprev: 前一个 VMA 的指针 (输入/输出)
 * @start: 起始地址
 * @end: 结束地址
 * @newflags: 新的保护标志
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
extern int mprotect_fixup(struct vma_iterator *vmi, struct mmu_gather *tlb,
	  struct vm_area_struct *vma, struct vm_area_struct **pprev,
	  unsigned long start, unsigned long end, vm_flags_t newflags);

/*
 * 快速获取用户页面（仅限已存在的）
 *
 * 【功能说明】获取用户页面，但不会触发页面错误
 *
 * 【与 get_user_pages_fast 的区别】
 * - get_user_pages_fast: 页面不存在时会触发页面错误
 * - get_user_pages_fast_only: 页面不存在时返回短结果
 *
 * 【为什么需要】
 * - 某些场景不能阻塞等待页面错误
 * - 只想获取已经在内存中的页面
 * - 适合探测性访问
 *
 * 【"short" 的含义】
 * - 可能返回少于请求的页面数量
 * - 遇到不存在的页面就停止
 *
 * 【参数说明】
 * @start: 起始虚拟地址
 * @nr_pages: 请求的页面数量
 * @gup_flags: 获取用户页面的标志
 * @pages: 页面指针数组 (输出)
 *
 * 【返回值】
 * - 正数: 实际获取的页面数量（可能少于 nr_pages）
 * - 负数: 错误码
 */
/*
 * doesn't attempt to fault and will return short.
 */
int get_user_pages_fast_only(unsigned long start, int nr_pages,
			     unsigned int gup_flags, struct page **pages);

/*
 * 快速获取单个用户页面（仅限已存在的）
 *
 * 【功能说明】获取单个用户页面，不触发页面错误
 *
 * 【与 get_user_pages_fast_only 的区别】
 * - get_user_pages_fast_only: 可以获取多个页面
 * - get_user_page_fast_only: 只获取单个页面，更简单
 *
 * 【参数说明】
 * @addr: 虚拟地址
 * @gup_flags: 获取用户页面的标志
 * @pagep: 页面指针的指针 (输出)
 *
 * 【返回值】
 * - true: 成功获取页面
 * - false: 未能获取页面
 */
static inline bool get_user_page_fast_only(unsigned long addr,
			unsigned int gup_flags, struct page **pagep)
{
	return get_user_pages_fast_only(addr, 1, gup_flags, pagep) == 1;
}

/*
 * 每进程 (每个 mm_struct) 统计信息
 *
 * 【统计信息说明】
 * - RSS: Resident Set Size (常驻集大小)
 * - 进程实际占用的物理内存
 * - 包括匿名页、文件页、共享内存页
 */
/*
 * per-process(per-mm_struct) statistics.
 */

/*
 * 获取内存统计计数器
 *
 * 【功能说明】读取进程的内存统计计数器
 *
 * 【percpu_counter 说明】
 * - percpu_counter: 每 CPU 计数器
 * - 减少计数器争用，提高性能
 * - 每个 CPU 有独立的计数值
 * - 读取时汇总所有 CPU 的值
 *
 * 【read_positive 说明】
 * - 返回正数值
 * - 如果计数器为负，返回 0
 * - 适合统计用途
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @member: 统计成员 (MM_FILEPAGES, MM_ANONPAGES, MM_SHMEMPAGES 等)
 *
 * 【返回值】
 * - 计数器的当前值（非负）
 */
static inline unsigned long get_mm_counter(struct mm_struct *mm, int member)
{
	return percpu_counter_read_positive(&mm->rss_stat[member]);
}

/*
 * 获取内存统计计数器总和
 *
 * 【功能说明】精确读取进程的内存统计计数器
 *
 * 【与 get_mm_counter 的区别】
 * - get_mm_counter: 快速读取，可能不太精确
 * - get_mm_counter_sum: 精确读取，汇总所有 CPU 的值
 * - get_mm_counter_sum 更慢但更准确
 *
 * 【为什么需要】
 * - 某些场景需要精确的统计值
 * - 例如资源限制检查
 * - 性能不敏感的场景
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @member: 统计成员
 *
 * 【返回值】
 * - 计数器的精确值（非负）
 */
static inline unsigned long get_mm_counter_sum(struct mm_struct *mm, int member)
{
	return percpu_counter_sum_positive(&mm->rss_stat[member]);
}

/*
 * 追踪内存统计变化
 *
 * 【功能说明】记录内存统计的变化，用于调试和分析
 *
 * 【tracepoint 说明】
 * - 内核追踪点
 * - 可以用工具 (如 ftrace) 监控
 * - 用于性能分析和调试
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @member: 统计成员
 */
void mm_trace_rss_stat(struct mm_struct *mm, int member);

/*
 * 增加内存统计计数器
 *
 * 【功能说明】为内存统计计数器增加指定值
 *
 * 【使用场景】
 * - 映射新页面时增加计数
 * - 解除映射时减少计数（传递负值）
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @member: 统计成员
 * @value: 要增加的值（可以为负）
 */
static inline void add_mm_counter(struct mm_struct *mm, int member, long value)
{
	/*
	 * 【更新计数器】
	 * - percpu_counter_add: 增加每 CPU 计数器
	 */
	percpu_counter_add(&mm->rss_stat[member], value);

	/*
	 * 【记录追踪点】
	 * - 用于调试和性能分析
	 */
	mm_trace_rss_stat(mm, member);
}

/*
 * 递增内存统计计数器
 *
 * 【功能说明】将内存统计计数器加 1
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @member: 统计成员
 */
static inline void inc_mm_counter(struct mm_struct *mm, int member)
{
	/*
	 * 【递增计数器】
	 * - percpu_counter_inc: 每 CPU 计数器加 1
	 */
	percpu_counter_inc(&mm->rss_stat[member]);

	/*
	 * 【记录追踪点】
	 */
	mm_trace_rss_stat(mm, member);
}

/*
 * 递减内存统计计数器
 *
 * 【功能说明】将内存统计计数器减 1
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @member: 统计成员
 */
static inline void dec_mm_counter(struct mm_struct *mm, int member)
{
	/*
	 * 【递减计数器】
	 * - percpu_counter_dec: 每 CPU 计数器减 1
	 */
	percpu_counter_dec(&mm->rss_stat[member]);

	/*
	 * 【记录追踪点】
	 */
	mm_trace_rss_stat(mm, member);
}

/*
 * 获取文件页的计数器类型（优化版本）
 *
 * 【功能说明】确定 folio 属于哪种文件页计数器
 *
 * 【前提条件】
 * - folio 已知不是匿名页
 * - 只需要区分文件页和共享内存页
 *
 * 【swapbacked 说明】
 * - swapbacked: 可以交换到 swap 的页面
 * - 共享内存页是 swapbacked 的
 * - 普通文件页不是 swapbacked 的
 *
 * 【参数说明】
 * @folio: folio 指针
 *
 * 【返回值】
 * - MM_SHMEMPAGES: 共享内存页
 * - MM_FILEPAGES: 文件页
 */
/* Optimized variant when folio is already known not to be anon */
static inline int mm_counter_file(struct folio *folio)
{
	/*
	 * 【检查是否 swapbacked】
	 * - swapbacked 的文件页是共享内存页
	 */
	if (folio_test_swapbacked(folio))
		return MM_SHMEMPAGES;
	/*
	 * 【否则是普通文件页】
	 */
	return MM_FILEPAGES;
}

/*
 * 获取 folio 的计数器类型
 *
 * 【功能说明】确定 folio 属于哪种内存类型计数器
 *
 * 【内存类型】
 * - MM_ANONPAGES: 匿名页（进程私有内存）
 * - MM_FILEPAGES: 文件页（文件映射）
 * - MM_SHMEMPAGES: 共享内存页
 *
 * 【参数说明】
 * @folio: folio 指针
 *
 * 【返回值】
 * - MM_ANONPAGES: 匿名页
 * - MM_FILEPAGES: 文件页
 * - MM_SHMEMPAGES: 共享内存页
 */
static inline int mm_counter(struct folio *folio)
{
	/*
	 * 【先检查是否匿名页】
	 * - 匿名页独立统计
	 */
	if (folio_test_anon(folio))
		return MM_ANONPAGES;
	/*
	 * 【否则是文件页，调用优化版本】
	 */
	return mm_counter_file(folio);
}

/*
 * 获取进程的 RSS 总和
 *
 * 【功能说明】获取进程的常驻集大小（RSS）
 *
 * 【RSS 组成】
 * - 文件页 (MM_FILEPAGES)
 * - 匿名页 (MM_ANONPAGES)
 * - 共享内存页 (MM_SHMEMPAGES)
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 *
 * 【返回值】
 * - RSS 总和（页面数量）
 */
static inline unsigned long get_mm_rss(struct mm_struct *mm)
{
	return get_mm_counter(mm, MM_FILEPAGES) +
		get_mm_counter(mm, MM_ANONPAGES) +
		get_mm_counter(mm, MM_SHMEMPAGES);
}

/*
 * 获取进程的精确 RSS 总和
 *
 * 【功能说明】获取进程的精确常驻集大小
 *
 * 【与 get_mm_rss 的区别】
 * - get_mm_rss: 快速读取，可能不太精确
 * - get_mm_rss_sum: 精确读取，汇总所有 CPU 的值
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 *
 * 【返回值】
 * - 精确的 RSS 总和（页面数量）
 */
static inline unsigned long get_mm_rss_sum(struct mm_struct *mm)
{
	return get_mm_counter_sum(mm, MM_FILEPAGES) +
		get_mm_counter_sum(mm, MM_ANONPAGES) +
		get_mm_counter_sum(mm, MM_SHMEMPAGES);
}

/*
 * 获取进程的 RSS 高水位
 *
 * 【功能说明】获取进程 RSS 的历史最大值
 *
 * 【高水位说明】
 * - hiwater_rss: 进程运行期间 RSS 的最大值
 * - 用于统计进程的峰值内存使用
 * - /proc/<pid>/status 中显示为 VmHWM
 *
 * 【为什么需要】
 * - 了解进程的内存使用峰值
 * - 容量规划
 * - 性能分析
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 *
 * 【返回值】
 * - RSS 高水位（页面数量）
 */
static inline unsigned long get_mm_hiwater_rss(struct mm_struct *mm)
{
	/*
	 * 【返回历史最大值和当前值的较大者】
	 * - mm->hiwater_rss: 记录的历史最大值
	 * - get_mm_rss(mm): 当前 RSS
	 * - 返回两者中的较大值
	 */
	return max(mm->hiwater_rss, get_mm_rss(mm));
}

/*
 * 获取进程的虚拟内存高水位
 *
 * 【功能说明】获取进程虚拟内存的历史最大值
 *
 * 【高水位说明】
 * - hiwater_vm: 进程运行期间虚拟内存的最大值
 * - 包括所有映射的虚拟地址空间
 * - /proc/<pid>/status 中显示为 VmPeak
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 *
 * 【返回值】
 * - 虚拟内存高水位（页面数量）
 */
static inline unsigned long get_mm_hiwater_vm(struct mm_struct *mm)
{
	/*
	 * 【返回历史最大值和当前值的较大者】
	 * - mm->hiwater_vm: 记录的历史最大值
	 * - mm->total_vm: 当前虚拟内存总量
	 * - 返回两者中的较大值
	 */
	return max(mm->hiwater_vm, mm->total_vm);
}

/*
 * 更新 RSS 高水位
 *
 * 【功能说明】更新进程 RSS 的历史最大值
 *
 * 【data_race 说明】
 * - data_race(): 标记数据竞争
 * - 告诉编译器和静态分析工具这是已知的竞争
 * - 不会影响正确性的良性竞争
 * - 高水位统计不需要强一致性
 *
 * 【为什么允许数据竞争】
 * - 高水位只是统计信息
 * - 不需要精确的同步
 * - 避免加锁的性能开销
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void update_hiwater_rss(struct mm_struct *mm)
{
	/*
	 * 【读取当前 RSS】
	 */
	unsigned long _rss = get_mm_rss(mm);

	/*
	 * 【更新高水位（允许数据竞争）】
	 * - 先读取 hiwater_rss（允许竞争）
	 * - 如果当前 RSS 更大，更新 hiwater_rss（允许竞争）
	 */
	if (data_race(mm->hiwater_rss) < _rss)
		data_race(mm->hiwater_rss = _rss);
}

/*
 * 更新虚拟内存高水位
 *
 * 【功能说明】更新进程虚拟内存的历史最大值
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void update_hiwater_vm(struct mm_struct *mm)
{
	/*
	 * 【更新高水位】
	 * - 如果当前虚拟内存总量更大，更新 hiwater_vm
	 * - 这里不使用 data_race，可能需要更强的一致性
	 */
	if (mm->hiwater_vm < mm->total_vm)
		mm->hiwater_vm = mm->total_vm;
}

/*
 * 重置 RSS 高水位
 *
 * 【功能说明】将 RSS 高水位设置为当前值
 *
 * 【使用场景】
 * - fork 后子进程初始化
 * - 进程重新开始统计
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void reset_mm_hiwater_rss(struct mm_struct *mm)
{
	mm->hiwater_rss = get_mm_rss(mm);
}

/*
 * 设置最大 RSS 高水位
 *
 * 【功能说明】更新最大 RSS 高水位（如果当前进程的更大）
 *
 * 【使用场景】
 * - 进程树统计
 * - 找出所有进程中 RSS 最大的
 * - wait4 系统调用返回子进程资源使用情况
 *
 * 【参数说明】
 * @maxrss: 最大 RSS 的指针（输入/输出）
 * @mm: 进程的内存描述符
 */
static inline void setmax_mm_hiwater_rss(unsigned long *maxrss,
					 struct mm_struct *mm)
{
	/*
	 * 【获取当前进程的 RSS 高水位】
	 */
	unsigned long hiwater_rss = get_mm_hiwater_rss(mm);

	/*
	 * 【更新最大值】
	 * - 如果当前进程的高水位更大，更新 maxrss
	 */
	if (*maxrss < hiwater_rss)
		*maxrss = hiwater_rss;
}

/*
 * CONFIG_ARCH_HAS_PTE_SPECIAL 未定义时的存根
 *
 * 【PTE special 位说明】
 * - special 位: 标记特殊映射的 PTE
 * - 用于 VM_PFNMAP、VM_IO 等特殊映射
 * - 不是所有架构都支持
 *
 * 【为什么需要存根】
 * - 不支持的架构提供空实现
 * - 代码可以统一调用，不需要到处 #ifdef
 */
#ifndef CONFIG_ARCH_HAS_PTE_SPECIAL
/*
 * 检查 PTE 是否为特殊映射
 *
 * 【功能说明】不支持 special 位的架构总是返回 0
 *
 * 【参数说明】
 * @pte: 页表项
 *
 * 【返回值】
 * - 0: 不支持 special 位
 */
static inline int pte_special(pte_t pte)
{
	return 0;
}

/*
 * 将 PTE 标记为特殊映射
 *
 * 【功能说明】不支持 special 位的架构直接返回原 PTE
 *
 * 【参数说明】
 * @pte: 页表项
 *
 * 【返回值】
 * - 原 PTE（未修改）
 */
static inline pte_t pte_mkspecial(pte_t pte)
{
	return pte;
}
#endif

/*
 * CONFIG_ARCH_SUPPORTS_PMD_PFNMAP 未定义时的存根
 *
 * 【PMD PFNMAP 说明】
 * - PMD 级别的 PFNMAP 支持
 * - 大页的特殊映射
 * - 不是所有架构都支持
 */
#ifndef CONFIG_ARCH_SUPPORTS_PMD_PFNMAP
/*
 * 检查 PMD 是否为特殊映射
 *
 * 【功能说明】不支持 PMD PFNMAP 的架构总是返回 false
 *
 * 【参数说明】
 * @pmd: PMD 页表项
 *
 * 【返回值】
 * - false: 不支持 PMD special
 */
static inline bool pmd_special(pmd_t pmd)
{
	return false;
}

/*
 * 将 PMD 标记为特殊映射
 *
 * 【功能说明】不支持 PMD PFNMAP 的架构直接返回原 PMD
 *
 * 【参数说明】
 * @pmd: PMD 页表项
 *
 * 【返回值】
 * - 原 PMD（未修改）
 */
static inline pmd_t pmd_mkspecial(pmd_t pmd)
{
	return pmd;
}
#endif	/* CONFIG_ARCH_SUPPORTS_PMD_PFNMAP */

/*
 * CONFIG_ARCH_SUPPORTS_PUD_PFNMAP 未定义时的存根
 *
 * 【PUD PFNMAP 说明】
 * - PUD 级别的 PFNMAP 支持
 * - 超大页的特殊映射
 * - 很少架构支持
 */
#ifndef CONFIG_ARCH_SUPPORTS_PUD_PFNMAP
/*
 * 检查 PUD 是否为特殊映射
 *
 * 【功能说明】不支持 PUD PFNMAP 的架构总是返回 false
 *
 * 【参数说明】
 * @pud: PUD 页表项
 *
 * 【返回值】
 * - false: 不支持 PUD special
 */
static inline bool pud_special(pud_t pud)
{
	return false;
}

/*
 * 将 PUD 标记为特殊映射
 *
 * 【功能说明】不支持 PUD PFNMAP 的架构直接返回原 PUD
 *
 * 【参数说明】
 * @pud: PUD 页表项
 *
 * 【返回值】
 * - 原 PUD（未修改）
 */
static inline pud_t pud_mkspecial(pud_t pud)
{
	return pud;
}
#endif	/* CONFIG_ARCH_SUPPORTS_PUD_PFNMAP */

/*
 * 获取已加锁的 PTE
 *
 * 【功能说明】获取指定地址的 PTE，并锁定对应的页表锁
 *
 * 【为什么需要】
 * - 修改 PTE 需要持有页表锁
 * - 这个函数同时完成查找和加锁
 * - 避免查找后再加锁的竞争窗口
 *
 * 【使用场景】
 * - 需要修改单个 PTE 时
 * - 例如改变页面保护属性
 * - 插入特殊映射
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @addr: 虚拟地址
 * @ptl: 页表锁指针的指针（输出）
 *       - 返回持有的锁，调用者需要释放
 *
 * 【返回值】
 * - 非 NULL: PTE 指针，锁已持有
 * - NULL: 页表不存在
 */
extern pte_t *get_locked_pte(struct mm_struct *mm, unsigned long addr,
			     spinlock_t **ptl);

/*
 * P4D 分配相关函数
 *
 * 【P4D 说明】
 * - P4D: Page 4th-level Directory (第四级页目录)
 * - 5 级页表架构使用
 * - 4 级及以下架构会折叠
 *
 * 【__PAGETABLE_P4D_FOLDED 说明】
 * - P4D 被折叠（不使用）
 * - 分配函数变成空操作
 */
#ifdef __PAGETABLE_P4D_FOLDED
/*
 * P4D 分配（折叠版本）
 *
 * 【功能说明】P4D 被折叠时，不需要分配
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pgd: PGD 页表项指针
 * @address: 虚拟地址
 *
 * 【返回值】
 * - 0: 成功（无操作）
 */
static inline int __p4d_alloc(struct mm_struct *mm, pgd_t *pgd,
						unsigned long address)
{
	return 0;
}
#else
/*
 * P4D 分配（非折叠版本）
 *
 * 【功能说明】实际分配 P4D 页表
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pgd: PGD 页表项指针
 * @address: 虚拟地址
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
int __p4d_alloc(struct mm_struct *mm, pgd_t *pgd, unsigned long address);
#endif

/*
 * PUD 分配相关函数
 *
 * 【PUD 说明】
 * - PUD: Page Upper Directory (页上级目录)
 * - 3 级及以上页表架构使用
 * - 2 级页表架构会折叠
 */
#if defined(__PAGETABLE_PUD_FOLDED) || !defined(CONFIG_MMU)
/*
 * PUD 分配（折叠版本或无 MMU）
 *
 * 【功能说明】PUD 被折叠或无 MMU 时，不需要分配
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @p4d: P4D 页表项指针
 * @address: 虚拟地址
 *
 * 【返回值】
 * - 0: 成功（无操作）
 */
static inline int __pud_alloc(struct mm_struct *mm, p4d_t *p4d,
						unsigned long address)
{
	return 0;
}

/*
 * 增加 PUD 数量统计（折叠版本）
 *
 * 【功能说明】PUD 被折叠时，不需要统计
 */
static inline void mm_inc_nr_puds(struct mm_struct *mm) {}

/*
 * 减少 PUD 数量统计（折叠版本）
 *
 * 【功能说明】PUD 被折叠时，不需要统计
 */
static inline void mm_dec_nr_puds(struct mm_struct *mm) {}

#else
/*
 * PUD 分配（非折叠版本）
 *
 * 【功能说明】实际分配 PUD 页表
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @p4d: P4D 页表项指针
 * @address: 虚拟地址
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
int __pud_alloc(struct mm_struct *mm, p4d_t *p4d, unsigned long address);

/*
 * 增加 PUD 数量统计
 *
 * 【功能说明】分配 PUD 时增加页表字节统计
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void mm_inc_nr_puds(struct mm_struct *mm)
{
	/*
	 * 【检查是否折叠】
	 * - 如果 PUD 被折叠，不需要统计
	 */
	if (mm_pud_folded(mm))
		return;
	/*
	 * 【增加字节统计】
	 * - PTRS_PER_PUD: 每个 PUD 的指针数量
	 * - sizeof(pud_t): 每个 PUD 项的大小
	 * - 原子操作更新 pgtables_bytes
	 */
	atomic_long_add(PTRS_PER_PUD * sizeof(pud_t), &mm->pgtables_bytes);
}

/*
 * 减少 PUD 数量统计
 *
 * 【功能说明】释放 PUD 时减少页表字节统计
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void mm_dec_nr_puds(struct mm_struct *mm)
{
	/*
	 * 【检查是否折叠】
	 */
	if (mm_pud_folded(mm))
		return;
	/*
	 * 【减少字节统计】
	 */
	atomic_long_sub(PTRS_PER_PUD * sizeof(pud_t), &mm->pgtables_bytes);
}
#endif

/*
 * PMD 分配相关函数
 *
 * 【PMD 说明】
 * - PMD: Page Middle Directory (页中间目录)
 * - 2 级及以上页表架构使用
 * - 单级页表架构会折叠
 */
#if defined(__PAGETABLE_PMD_FOLDED) || !defined(CONFIG_MMU)
/*
 * PMD 分配（折叠版本或无 MMU）
 *
 * 【功能说明】PMD 被折叠或无 MMU 时，不需要分配
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pud: PUD 页表项指针
 * @address: 虚拟地址
 *
 * 【返回值】
 * - 0: 成功（无操作）
 */
static inline int __pmd_alloc(struct mm_struct *mm, pud_t *pud,
						unsigned long address)
{
	return 0;
}

/*
 * 增加 PMD 数量统计（折叠版本）
 *
 * 【功能说明】PMD 被折叠时，不需要统计
 */
static inline void mm_inc_nr_pmds(struct mm_struct *mm) {}

/*
 * 减少 PMD 数量统计（折叠版本）
 *
 * 【功能说明】PMD 被折叠时，不需要统计
 */
static inline void mm_dec_nr_pmds(struct mm_struct *mm) {}

#else
/*
 * PMD 分配（非折叠版本）
 *
 * 【功能说明】实际分配 PMD 页表
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pud: PUD 页表项指针
 * @address: 虚拟地址
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
int __pmd_alloc(struct mm_struct *mm, pud_t *pud, unsigned long address);

/*
 * 增加 PMD 数量统计
 *
 * 【功能说明】分配 PMD 时增加页表字节统计
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void mm_inc_nr_pmds(struct mm_struct *mm)
{
	/*
	 * 【检查是否折叠】
	 */
	if (mm_pmd_folded(mm))
		return;
	/*
	 * 【增加字节统计】
	 * - PTRS_PER_PMD: 每个 PMD 的指针数量
	 * - sizeof(pmd_t): 每个 PMD 项的大小
	 */
	atomic_long_add(PTRS_PER_PMD * sizeof(pmd_t), &mm->pgtables_bytes);
}

/*
 * 减少 PMD 数量统计
 *
 * 【功能说明】释放 PMD 时减少页表字节统计
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void mm_dec_nr_pmds(struct mm_struct *mm)
{
	/*
	 * 【检查是否折叠】
	 */
	if (mm_pmd_folded(mm))
		return;
	/*
	 * 【减少字节统计】
	 */
	atomic_long_sub(PTRS_PER_PMD * sizeof(pmd_t), &mm->pgtables_bytes);
}
#endif

/*
 * 页表字节统计函数
 *
 * 【为什么需要统计页表字节】
 * - 页表本身也占用内存
 * - 需要统计进程的总内存开销
 * - 用于内存审计和限制
 */
#ifdef CONFIG_MMU
/*
 * 初始化页表字节统计
 *
 * 【功能说明】将页表字节计数器初始化为 0
 *
 * 【使用场景】
 * - 新进程创建时
 * - mm_struct 初始化时
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void mm_pgtables_bytes_init(struct mm_struct *mm)
{
	atomic_long_set(&mm->pgtables_bytes, 0);
}

/*
 * 获取页表字节统计
 *
 * 【功能说明】读取进程的页表字节数
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 *
 * 【返回值】
 * - 页表占用的字节数
 */
static inline unsigned long mm_pgtables_bytes(const struct mm_struct *mm)
{
	return atomic_long_read(&mm->pgtables_bytes);
}

/*
 * 增加 PTE 数量统计
 *
 * 【功能说明】分配 PTE 时增加页表字节统计
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void mm_inc_nr_ptes(struct mm_struct *mm)
{
	/*
	 * 【增加字节统计】
	 * - PTRS_PER_PTE: 每个 PTE 表的指针数量
	 * - sizeof(pte_t): 每个 PTE 项的大小
	 */
	atomic_long_add(PTRS_PER_PTE * sizeof(pte_t), &mm->pgtables_bytes);
}

/*
 * 减少 PTE 数量统计
 *
 * 【功能说明】释放 PTE 时减少页表字节统计
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 */
static inline void mm_dec_nr_ptes(struct mm_struct *mm)
{
	/*
	 * 【减少字节统计】
	 */
	atomic_long_sub(PTRS_PER_PTE * sizeof(pte_t), &mm->pgtables_bytes);
}
#else

/*
 * 无 MMU 版本的页表统计函数
 *
 * 【说明】
 * - 无 MMU 系统不使用页表
 * - 所有函数都是空操作
 */

/*
 * 初始化页表字节统计（无 MMU 版本）
 */
static inline void mm_pgtables_bytes_init(struct mm_struct *mm) {}

/*
 * 获取页表字节统计（无 MMU 版本）
 *
 * 【返回值】
 * - 0: 无 MMU 系统没有页表
 */
static inline unsigned long mm_pgtables_bytes(const struct mm_struct *mm)
{
	return 0;
}

/*
 * 增加 PTE 数量统计（无 MMU 版本）
 */
static inline void mm_inc_nr_ptes(struct mm_struct *mm) {}

/*
 * 减少 PTE 数量统计（无 MMU 版本）
 */
static inline void mm_dec_nr_ptes(struct mm_struct *mm) {}
#endif

/*
 * PTE 分配函数
 *
 * 【__pte_alloc 说明】
 * - 用户空间页表分配
 * - 在 PMD 下分配 PTE 表
 *
 * 【__pte_alloc_kernel 说明】
 * - 内核空间页表分配
 * - 内核页表需要特殊处理
 */

/*
 * 分配 PTE 表
 *
 * 【功能说明】为用户空间分配 PTE 页表
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pmd: PMD 页表项指针
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
int __pte_alloc(struct mm_struct *mm, pmd_t *pmd);

/*
 * 分配内核 PTE 表
 *
 * 【功能说明】为内核空间分配 PTE 页表
 *
 * 【参数说明】
 * @pmd: PMD 页表项指针
 *
 * 【返回值】
 * - 0: 成功
 * - 负数: 错误码
 */
int __pte_alloc_kernel(pmd_t *pmd);

/*
 * 页表分配的便捷包装函数
 *
 * 【说明】
 * - 检查上级页表项是否为空
 * - 如果为空，调用分配函数
 * - 返回下级页表的偏移
 */
#if defined(CONFIG_MMU)

/*
 * 分配 P4D
 *
 * 【功能说明】分配 P4D 页表（如果需要）
 *
 * 【实现说明】
 * - unlikely(pgd_none(*pgd)): PGD 项为空的可能性小（优化分支预测）
 * - 如果 PGD 为空，调用 __p4d_alloc 分配
 * - 分配失败返回 NULL
 * - 成功返回 P4D 表中的偏移
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pgd: PGD 页表项指针
 * @address: 虚拟地址
 *
 * 【返回值】
 * - 非 NULL: P4D 表项指针
 * - NULL: 分配失败
 */
static inline p4d_t *p4d_alloc(struct mm_struct *mm, pgd_t *pgd,
		unsigned long address)
{
	return (unlikely(pgd_none(*pgd)) && __p4d_alloc(mm, pgd, address)) ?
		NULL : p4d_offset(pgd, address);
}

/*
 * 分配 PUD
 *
 * 【功能说明】分配 PUD 页表（如果需要）
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @p4d: P4D 页表项指针
 * @address: 虚拟地址
 *
 * 【返回值】
 * - 非 NULL: PUD 表项指针
 * - NULL: 分配失败
 */
static inline pud_t *pud_alloc(struct mm_struct *mm, p4d_t *p4d,
		unsigned long address)
{
	return (unlikely(p4d_none(*p4d)) && __pud_alloc(mm, p4d, address)) ?
		NULL : pud_offset(p4d, address);
}

/*
 * 分配 PMD
 *
 * 【功能说明】分配 PMD 页表（如果需要）
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pud: PUD 页表项指针
 * @address: 虚拟地址
 *
 * 【返回值】
 * - 非 NULL: PMD 表项指针
 * - NULL: 分配失败
 */
static inline pmd_t *pmd_alloc(struct mm_struct *mm, pud_t *pud, unsigned long address)
{
	return (unlikely(pud_none(*pud)) && __pmd_alloc(mm, pud, address))?
		NULL: pmd_offset(pud, address);
}
#endif /* CONFIG_MMU */

/*
 * 页表标志枚举
 *
 * 【pt_flags 说明】
 * - 页表描述符的标志位
 * - 复用 page flags 的定义
 *
 * 【为什么复用 page flags】
 * - ptdesc 基于 struct page
 * - 重用现有的标志位机制
 * - 避免重复定义
 */
enum pt_flags {
	/*
	 * PT_kernel: 页表用于映射内核
	 *
	 * 【说明】
	 * - 复用 PG_referenced 标志位
	 * - 内核页表需要特殊处理
	 * - 例如不能被换出
	 */
	PT_kernel = PG_referenced,

	/*
	 * PT_reserved: 页表被预留
	 *
	 * 【说明】
	 * - 复用 PG_reserved 标志位
	 * - 标记特殊用途的页表
	 */
	PT_reserved = PG_reserved,

	/*
	 * 高位用于 zone/node/section
	 *
	 * 【说明】
	 * - 标志位的高位部分保留
	 * - 用于存储 NUMA 节点、内存区域等信息
	 */
	/* High bits are used for zone/node/section */
};

/*
 * 虚拟地址转 ptdesc
 *
 * 【功能说明】从虚拟地址获取页表描述符
 *
 * 【实现说明】
 * - virt_to_page: 虚拟地址转 page
 * - page_ptdesc: page 转 ptdesc
 *
 * 【使用场景】
 * - 已知页表的虚拟地址
 * - 需要获取对应的 ptdesc
 *
 * 【参数说明】
 * @x: 页表的虚拟地址
 *
 * 【返回值】
 * - 页表描述符指针
 */
static inline struct ptdesc *virt_to_ptdesc(const void *x)
{
	return page_ptdesc(virt_to_page(x));
}

/**
 * ptdesc_address - 页表的虚拟地址
 * @pt: 页表描述符
 *
 * 【功能说明】获取页表描述符对应的虚拟地址
 *
 * 【返回值】
 * - 页表的第一个字节的虚拟地址
 */
/**
 * ptdesc_address - Virtual address of page table.
 * @pt: Page table descriptor.
 *
 * Return: The first byte of the page table described by @pt.
 */
static inline void *ptdesc_address(const struct ptdesc *pt)
{
	return folio_address(ptdesc_folio(pt));
}

/*
 * 检查页表是否被预留
 *
 * 【功能说明】测试页表的 PT_reserved 标志
 *
 * 【参数说明】
 * @pt: 页表描述符
 *
 * 【返回值】
 * - true: 页表被预留
 * - false: 页表未被预留
 */
static inline bool pagetable_is_reserved(struct ptdesc *pt)
{
	return test_bit(PT_reserved, &pt->pt_flags.f);
}

/**
 * ptdesc_set_kernel - 标记 ptdesc 用于映射内核
 * @ptdesc: 要标记的 ptdesc
 *
 * 【功能说明】将页表标记为内核页表
 *
 * 【为什么需要】
 * - 内核页表需要特殊处理
 * - 设置标志让处理代码知道这不会用于用户空间
 * - 例如内核页表不能被换出
 */
/**
 * ptdesc_set_kernel - Mark a ptdesc used to map the kernel
 * @ptdesc: The ptdesc to be marked
 *
 * Kernel page tables often need special handling. Set a flag so that
 * the handling code knows this ptdesc will not be used for userspace.
 */
static inline void ptdesc_set_kernel(struct ptdesc *ptdesc)
{
	set_bit(PT_kernel, &ptdesc->pt_flags.f);
}

/**
 * ptdesc_clear_kernel - 清除 ptdesc 的内核标记
 * @ptdesc: 要清除标记的 ptdesc
 *
 * 【功能说明】标记页表不再用于映射内核
 *
 * 【使用场景】
 * - 页表不再用于映射内核
 * - 不再需要特殊处理
 */
/**
 * ptdesc_clear_kernel - Mark a ptdesc as no longer used to map the kernel
 * @ptdesc: The ptdesc to be unmarked
 *
 * Use when the ptdesc is no longer used to map the kernel and no longer
 * needs special handling.
 */
static inline void ptdesc_clear_kernel(struct ptdesc *ptdesc)
{
	/*
	 * 注意: PG_referenced 位在释放页面前不严格需要清除
	 * 但为了对称性这样做更好
	 */
	/*
	 * Note: the 'PG_referenced' bit does not strictly need to be
	 * cleared before freeing the page. But this is nice for
	 * symmetry.
	 */
	clear_bit(PT_kernel, &ptdesc->pt_flags.f);
}

/**
 * ptdesc_test_kernel - 检查 ptdesc 是否用于映射内核
 * @ptdesc: 要测试的 ptdesc
 *
 * 【功能说明】判断页表是否用于映射内核
 *
 * 【返回值】
 * - true: 用于映射内核
 * - false: 不用于映射内核
 */
/**
 * ptdesc_test_kernel - Check if a ptdesc is used to map the kernel
 * @ptdesc: The ptdesc being tested
 *
 * Call to tell if the ptdesc used to map the kernel.
 */
static inline bool ptdesc_test_kernel(const struct ptdesc *ptdesc)
{
	return test_bit(PT_kernel, &ptdesc->pt_flags.f);
}

/**
 * pagetable_alloc - 分配页表
 * @gfp:    GFP 分配标志
 * @order:  期望的页表 order（页数的对数）
 *
 * 【功能说明】
 * - pagetable_alloc 为页表分配内存
 * - 同时分配页表描述符来描述该内存
 *
 * 【返回值】
 * - 描述已分配页表的 ptdesc
 */
/**
 * pagetable_alloc - Allocate pagetables
 * @gfp:    GFP allocation flags
 * @order:  desired pagetable order
 *
 * pagetable_alloc allocates memory for page tables as well as a page table
 * descriptor to describe that memory.
 *
 * Return: The ptdesc describing the allocated page tables.
 */
static inline struct ptdesc *pagetable_alloc_noprof(gfp_t gfp, unsigned int order)
{
	/*
	 * 【分配复合页】
	 * - __GFP_COMP: 分配复合页（多个连续物理页组成一个大页）
	 * - order: 分配 2^order 个页
	 */
	struct page *page = alloc_pages_noprof(gfp | __GFP_COMP, order);

	/*
	 * 【转换为 ptdesc】
	 * - page 转为 ptdesc 描述符
	 */
	return page_ptdesc(page);
}
/*
 * 【pagetable_alloc 宏】
 * - 包装 pagetable_alloc_noprof
 * - alloc_hooks: 添加内存分配追踪钩子
 * - 用于调试和性能分析
 */
#define pagetable_alloc(...)	alloc_hooks(pagetable_alloc_noprof(__VA_ARGS__))

/*
 * 内部页表释放函数
 *
 * 【功能说明】释放页表内存的底层实现
 *
 * 【参数说明】
 * @pt: 页表描述符
 */
static inline void __pagetable_free(struct ptdesc *pt)
{
	/*
	 * 【获取 page】
	 * - ptdesc 转为 page
	 */
	struct page *page = ptdesc_page(pt);

	/*
	 * 【释放页面】
	 * - compound_order: 获取复合页的 order
	 * - 释放对应数量的页
	 */
	__free_pages(page, compound_order(page));
}

/*
 * 内核页表异步释放支持
 *
 * 【CONFIG_ASYNC_KERNEL_PGTABLE_FREE 说明】
 * - 异步释放内核页表
 * - 避免在关键路径上长时间持锁
 */
#ifdef CONFIG_ASYNC_KERNEL_PGTABLE_FREE
/*
 * 异步释放内核页表（外部实现）
 *
 * 【功能说明】
 * - 将页表放入队列
 * - 稍后异步释放
 * - 减少同步开销
 */
void pagetable_free_kernel(struct ptdesc *pt);
#else
/*
 * 同步释放内核页表
 *
 * 【功能说明】
 * - 不支持异步释放时
 * - 直接同步释放
 */
static inline void pagetable_free_kernel(struct ptdesc *pt)
{
	__pagetable_free(pt);
}
#endif

/**
 * pagetable_free - 释放页表
 * @pt:	页表描述符
 *
 * 【功能说明】
 * - pagetable_free 释放页表描述符描述的所有页表内存
 * - 同时释放描述符自身的内存
 *
 * 【实现说明】
 * - 检查是否是内核页表
 * - 内核页表使用特殊的释放路径
 * - 用户页表直接释放
 */
/**
 * pagetable_free - Free pagetables
 * @pt:	The page table descriptor
 *
 * pagetable_free frees the memory of all page tables described by a page
 * table descriptor and the memory for the descriptor itself.
 */
static inline void pagetable_free(struct ptdesc *pt)
{
	/*
	 * 【检查是否是内核页表】
	 */
	if (ptdesc_test_kernel(pt)) {
		/*
		 * 【清除内核标志】
		 * - 释放前清除标志
		 */
		ptdesc_clear_kernel(pt);
		/*
		 * 【调用内核页表释放】
		 * - 可能是异步释放
		 * - 也可能是同步释放
		 */
		pagetable_free_kernel(pt);
	} else {
		/*
		 * 【用户页表直接释放】
		 */
		__pagetable_free(pt);
	}
}

/*
 * 分割 PTE 锁（Split PTE Locks）
 *
 * 【CONFIG_SPLIT_PTE_PTLOCKS 说明】
 * - 每个 PTE 页表有独立的自旋锁
 * - 提高并发性能
 * - 避免多个线程争用同一个锁
 *
 * 【为什么需要分割锁】
 * - 传统方式: 所有 PTE 共用 mm->page_table_lock
 * - 分割锁: 每个 PTE 表有自己的锁
 * - 减少锁竞争，提高多核性能
 */
#if defined(CONFIG_SPLIT_PTE_PTLOCKS)
/*
 * 【ALLOC_SPLIT_PTLOCKS 说明】
 * - 动态分配 ptlock
 * - 用于 PTE 表很大的架构
 */
#if ALLOC_SPLIT_PTLOCKS
/*
 * 初始化 ptlock 缓存
 *
 * 【功能说明】
 * - 系统启动时初始化
 * - 创建 ptlock 的 slab 缓存
 */
void __init ptlock_cache_init(void);

/*
 * 分配 ptlock
 *
 * 【功能说明】为 ptdesc 分配锁
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 *
 * 【返回值】
 * - true: 成功
 * - false: 失败
 */
bool ptlock_alloc(struct ptdesc *ptdesc);

/*
 * 释放 ptlock
 *
 * 【功能说明】释放 ptdesc 的锁
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 */
void ptlock_free(struct ptdesc *ptdesc);

/*
 * 获取 ptlock 指针
 *
 * 【功能说明】返回 ptdesc 的锁指针
 *
 * 【实现说明】
 * - ptdesc->ptl: 指向动态分配的锁
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 *
 * 【返回值】
 * - 锁指针
 */
static inline spinlock_t *ptlock_ptr(struct ptdesc *ptdesc)
{
	return ptdesc->ptl;
}
#else /* ALLOC_SPLIT_PTLOCKS */
/*
 * 嵌入式 ptlock 版本
 *
 * 【说明】
 * - 不动态分配锁
 * - 锁直接嵌入在 ptdesc 中
 */

/*
 * 初始化 ptlock 缓存（嵌入版本）
 *
 * 【说明】空操作，因为锁嵌入在结构体中
 */
static inline void ptlock_cache_init(void)
{
}

/*
 * 分配 ptlock（嵌入版本）
 *
 * 【说明】
 * - 锁已经嵌入
 * - 总是返回 true
 */
static inline bool ptlock_alloc(struct ptdesc *ptdesc)
{
	return true;
}

/*
 * 释放 ptlock（嵌入版本）
 *
 * 【说明】空操作，因为锁嵌入在结构体中
 */
static inline void ptlock_free(struct ptdesc *ptdesc)
{
}

/*
 * 获取 ptlock 指针（嵌入版本）
 *
 * 【说明】返回嵌入锁的地址
 */
static inline spinlock_t *ptlock_ptr(struct ptdesc *ptdesc)
{
	return &ptdesc->ptl;
}
#endif /* ALLOC_SPLIT_PTLOCKS */

/*
 * 从 PMD 获取 PTE 锁
 *
 * 【功能说明】获取 PTE 表对应的锁
 *
 * 【实现说明】
 * - pmd_page(*pmd): PMD 指向的 page
 * - page_ptdesc: page 转 ptdesc
 * - ptlock_ptr: 获取锁指针
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pmd: PMD 页表项指针
 *
 * 【返回值】
 * - PTE 表的锁指针
 */
static inline spinlock_t *pte_lockptr(struct mm_struct *mm, pmd_t *pmd)
{
	return ptlock_ptr(page_ptdesc(pmd_page(*pmd)));
}

/*
 * 从 PTE 指针获取锁
 *
 * 【功能说明】根据 PTE 地址获取对应的锁
 *
 * 【编译时检查】
 * - CONFIG_HIGHPTE: 不支持高端内存 PTE（因为 virt_to_ptdesc 需要直接映射）
 * - MAX_PTRS_PER_PTE * sizeof(pte_t) > PAGE_SIZE: PTE 表必须在一页内
 *
 * 【实现说明】
 * - virt_to_ptdesc(pte): PTE 虚拟地址转 ptdesc
 * - ptlock_ptr: 获取锁指针
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pte: PTE 指针
 *
 * 【返回值】
 * - PTE 表的锁指针
 */
static inline spinlock_t *ptep_lockptr(struct mm_struct *mm, pte_t *pte)
{
	/*
	 * 【编译时断言】
	 * - 确保不启用 HIGHPTE
	 */
	BUILD_BUG_ON(IS_ENABLED(CONFIG_HIGHPTE));
	/*
	 * 【编译时断言】
	 * - 确保 PTE 表不超过一页
	 */
	BUILD_BUG_ON(MAX_PTRS_PER_PTE * sizeof(pte_t) > PAGE_SIZE);
	return ptlock_ptr(virt_to_ptdesc(pte));
}

/*
 * 初始化 ptlock
 *
 * 【功能说明】初始化页表描述符的锁
 *
 * 【实现说明】
 * - 检查 ptl 字段是否为 0
 * - 如果不为 0，说明有其他代码在使用
 * - 初始化自旋锁
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 *
 * 【返回值】
 * - true: 成功
 * - false: 失败（无法分配）
 */
static inline bool ptlock_init(struct ptdesc *ptdesc)
{
	/*
	 * prep_new_page() 将 page->private（因此 page->ptl）初始化为 0
	 * 确保在此期间没有人使用它
	 *
	 * 如果架构尝试使用 slab 分配页表，可能会发生这种情况：
	 * slab 代码使用 page->slab_cache，它与 page->ptl 共享存储
	 */
	/*
	 * prep_new_page() initialize page->private (and therefore page->ptl)
	 * with 0. Make sure nobody took it in use in between.
	 *
	 * It can happen if arch try to use slab for page table allocation:
	 * slab code uses page->slab_cache, which share storage with page->ptl.
	 */
	VM_BUG_ON_PAGE(*(unsigned long *)&ptdesc->ptl, ptdesc_page(ptdesc));
	/*
	 * 【分配锁】
	 * - 如果启用了 ALLOC_SPLIT_PTLOCKS，动态分配锁
	 * - 否则使用嵌入的锁，总是成功
	 */
	if (!ptlock_alloc(ptdesc))
		return false;
	/*
	 * 【初始化自旋锁】
	 */
	spin_lock_init(ptlock_ptr(ptdesc));
	return true;
}

#else	/* !defined(CONFIG_SPLIT_PTE_PTLOCKS) */
/*
 * 不使用分割 PTE 锁的版本
 *
 * 【说明】
 * - 所有页表使用 mm->page_table_lock
 * - 传统的粗粒度锁
 * - 实现简单但并发性差
 */

/*
 * 我们使用 mm->page_table_lock 来保护 mm 的所有页表页
 */
/*
 * We use mm->page_table_lock to guard all pagetable pages of the mm.
 */

/*
 * 获取 PTE 锁（共享版本）
 *
 * 【说明】返回整个 mm 的页表锁
 */
static inline spinlock_t *pte_lockptr(struct mm_struct *mm, pmd_t *pmd)
{
	return &mm->page_table_lock;
}

/*
 * 获取 PTE 指针的锁（共享版本）
 *
 * 【说明】返回整个 mm 的页表锁
 */
static inline spinlock_t *ptep_lockptr(struct mm_struct *mm, pte_t *pte)
{
	return &mm->page_table_lock;
}

/*
 * 【空操作函数】
 * - 不使用分割锁时，这些函数都是空操作
 */
static inline void ptlock_cache_init(void) {}
static inline bool ptlock_init(struct ptdesc *ptdesc) { return true; }
static inline void ptlock_free(struct ptdesc *ptdesc) {}
#endif /* defined(CONFIG_SPLIT_PTE_PTLOCKS) */

/*
 * 页表构造函数
 *
 * 【__pagetable_ctor 说明】
 * - 初始化页表描述符
 * - 设置页表标志
 * - 更新统计信息
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 */
static inline void __pagetable_ctor(struct ptdesc *ptdesc)
{
	/*
	 * 【获取 folio】
	 */
	struct folio *folio = ptdesc_folio(ptdesc);

	/*
	 * 【设置页表标志】
	 * - 标记 folio 是页表
	 */
	__folio_set_pgtable(folio);
	/*
	 * 【更新统计】
	 * - NR_PAGETABLE: 页表页数量
	 * - 增加 LRU 向量统计
	 */
	lruvec_stat_add_folio(folio, NR_PAGETABLE);
}

/*
 * 页表析构函数
 *
 * 【功能说明】清理页表描述符
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 */
static inline void pagetable_dtor(struct ptdesc *ptdesc)
{
	/*
	 * 【获取 folio】
	 */
	struct folio *folio = ptdesc_folio(ptdesc);

	/*
	 * 【释放锁】
	 */
	ptlock_free(ptdesc);
	/*
	 * 【清除页表标志】
	 */
	__folio_clear_pgtable(folio);
	/*
	 * 【更新统计】
	 * - 减少页表页数量
	 */
	lruvec_stat_sub_folio(folio, NR_PAGETABLE);
}

/*
 * 析构并释放页表
 *
 * 【功能说明】
 * - 调用析构函数
 * - 释放页表内存
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 */
static inline void pagetable_dtor_free(struct ptdesc *ptdesc)
{
	pagetable_dtor(ptdesc);
	pagetable_free(ptdesc);
}

/*
 * PTE 页表构造函数
 *
 * 【功能说明】构造 PTE 页表
 *
 * 【实现说明】
 * - 初始化 ptlock（对于用户进程）
 * - 调用通用构造函数
 *
 * 【为什么 init_mm 不需要 ptlock】
 * - init_mm 是内核的内存描述符
 * - 内核页表不需要分割锁
 * - 内核使用全局锁保护
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @ptdesc: 页表描述符
 *
 * 【返回值】
 * - true: 成功
 * - false: 失败（无法分配锁）
 */
static inline bool pagetable_pte_ctor(struct mm_struct *mm,
				      struct ptdesc *ptdesc)
{
	/*
	 * 【初始化 ptlock】
	 * - init_mm 不需要（内核页表）
	 * - 用户进程需要初始化锁
	 */
	if (mm != &init_mm && !ptlock_init(ptdesc))
		return false;
	/*
	 * 【调用通用构造函数】
	 */
	__pagetable_ctor(ptdesc);
	return true;
}

/*
 * PTE 偏移和映射函数
 *
 * 【__pte_offset_map 说明】
 * - 底层实现，获取 PTE 指针
 * - 处理页表可能不存在的情况
 */

/*
 * PTE 偏移映射（外部实现）
 *
 * 【功能说明】
 * - 映射 PMD 指向的 PTE 表
 * - 返回地址对应的 PTE 指针
 *
 * 【参数说明】
 * @pmd: PMD 页表项指针
 * @addr: 虚拟地址
 * @pmdvalp: 输出 PMD 值（可选）
 *
 * 【返回值】
 * - PTE 指针
 * - NULL: PMD 为空或页表不存在
 */
pte_t *__pte_offset_map(pmd_t *pmd, unsigned long addr, pmd_t *pmdvalp);

/*
 * PTE 偏移映射
 *
 * 【功能说明】获取地址对应的 PTE 指针
 *
 * 【实现说明】
 * - 调用 __pte_offset_map
 * - 不需要返回 PMD 值
 *
 * 【参数说明】
 * @pmd: PMD 页表项指针
 * @addr: 虚拟地址
 *
 * 【返回值】
 * - PTE 指针
 * - NULL: PMD 为空
 */
static inline pte_t *pte_offset_map(pmd_t *pmd, unsigned long addr)
{
	return __pte_offset_map(pmd, addr, NULL);
}

/*
 * PTE 偏移映射并加锁（外部实现）
 *
 * 【功能说明】
 * - 映射 PTE 表
 * - 获取对应的锁
 * - 返回 PTE 指针和锁指针
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pmd: PMD 页表项指针
 * @addr: 虚拟地址
 * @ptlp: 输出锁指针
 *
 * 【返回值】
 * - PTE 指针
 * - NULL: PMD 为空
 */
pte_t *pte_offset_map_lock(struct mm_struct *mm, pmd_t *pmd,
			   unsigned long addr, spinlock_t **ptlp);

/*
 * PTE 偏移映射（只读，不加锁）
 *
 * 【功能说明】
 * - 只读访问 PTE
 * - 不获取锁
 * - 用于读取操作
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pmd: PMD 页表项指针
 * @addr: 虚拟地址
 * @ptlp: 输出锁指针（用于后续可能的加锁）
 *
 * 【返回值】
 * - PTE 指针
 * - NULL: PMD 为空
 */
pte_t *pte_offset_map_ro_nolock(struct mm_struct *mm, pmd_t *pmd,
				unsigned long addr, spinlock_t **ptlp);

/*
 * PTE 偏移映射（读写，不加锁）
 *
 * 【功能说明】
 * - 读写访问 PTE
 * - 不获取锁
 * - 返回 PMD 值供调用者检查
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pmd: PMD 页表项指针
 * @addr: 虚拟地址
 * @pmdvalp: 输出 PMD 值
 * @ptlp: 输出锁指针
 *
 * 【返回值】
 * - PTE 指针
 * - NULL: PMD 为空
 */
pte_t *pte_offset_map_rw_nolock(struct mm_struct *mm, pmd_t *pmd,
				unsigned long addr, pmd_t *pmdvalp,
				spinlock_t **ptlp);

/*
 * PTE 解映射并解锁宏
 *
 * 【功能说明】
 * - 释放锁
 * - 解除 PTE 映射
 *
 * 【使用说明】
 * - 与 pte_offset_map_lock 配对使用
 * - 先解锁，再解映射
 */
#define pte_unmap_unlock(pte, ptl)	do {		\
	spin_unlock(ptl);				\
	pte_unmap(pte);					\
} while (0)

/*
 * PTE 分配宏
 *
 * 【功能说明】
 * - 检查 PMD 是否为空
 * - 如果为空，分配 PTE 表
 *
 * 【实现说明】
 * - unlikely: PMD 为空的可能性小
 */
#define pte_alloc(mm, pmd) (unlikely(pmd_none(*(pmd))) && __pte_alloc(mm, pmd))

/*
 * PTE 分配并映射宏
 *
 * 【功能说明】
 * - 分配 PTE（如果需要）
 * - 返回 PTE 指针
 */
#define pte_alloc_map(mm, pmd, address)			\
	(pte_alloc(mm, pmd) ? NULL : pte_offset_map(pmd, address))

/*
 * PTE 分配、映射并加锁宏
 *
 * 【功能说明】
 * - 分配 PTE（如果需要）
 * - 映射并加锁
 * - 返回 PTE 指针和锁指针
 */
#define pte_alloc_map_lock(mm, pmd, address, ptlp)	\
	(pte_alloc(mm, pmd) ?			\
		 NULL : pte_offset_map_lock(mm, pmd, address, ptlp))

/*
 * 内核 PTE 分配宏
 *
 * 【功能说明】
 * - 为内核空间分配 PTE
 * - 使用 pte_offset_kernel 而不是 pte_offset_map
 *
 * 【为什么内核不用 map】
 * - 内核地址直接映射
 * - 不需要 kmap/kunmap
 */
#define pte_alloc_kernel(pmd, address)			\
	((unlikely(pmd_none(*(pmd))) && __pte_alloc_kernel(pmd))? \
		NULL: pte_offset_kernel(pmd, address))

static inline void __pagetable_ctor(struct ptdesc *ptdesc)
{
	struct folio *folio = ptdesc_folio(ptdesc);

	__folio_set_pgtable(folio);
	lruvec_stat_add_folio(folio, NR_PAGETABLE);
}

static inline void pagetable_dtor(struct ptdesc *ptdesc)
{
	struct folio *folio = ptdesc_folio(ptdesc);

	ptlock_free(ptdesc);
	__folio_clear_pgtable(folio);
	lruvec_stat_sub_folio(folio, NR_PAGETABLE);
}

static inline void pagetable_dtor_free(struct ptdesc *ptdesc)
{
	pagetable_dtor(ptdesc);
	pagetable_free(ptdesc);
}

static inline bool pagetable_pte_ctor(struct mm_struct *mm,
				      struct ptdesc *ptdesc)
{
	if (mm != &init_mm && !ptlock_init(ptdesc))
		return false;
	__pagetable_ctor(ptdesc);
	return true;
}

pte_t *__pte_offset_map(pmd_t *pmd, unsigned long addr, pmd_t *pmdvalp);

static inline pte_t *pte_offset_map(pmd_t *pmd, unsigned long addr)
{
	return __pte_offset_map(pmd, addr, NULL);
}

pte_t *pte_offset_map_lock(struct mm_struct *mm, pmd_t *pmd,
			   unsigned long addr, spinlock_t **ptlp);

pte_t *pte_offset_map_ro_nolock(struct mm_struct *mm, pmd_t *pmd,
				unsigned long addr, spinlock_t **ptlp);
pte_t *pte_offset_map_rw_nolock(struct mm_struct *mm, pmd_t *pmd,
				unsigned long addr, pmd_t *pmdvalp,
				spinlock_t **ptlp);

#define pte_unmap_unlock(pte, ptl)	do {		\
	spin_unlock(ptl);				\
	pte_unmap(pte);					\
} while (0)

#define pte_alloc(mm, pmd) (unlikely(pmd_none(*(pmd))) && __pte_alloc(mm, pmd))

#define pte_alloc_map(mm, pmd, address)			\
	(pte_alloc(mm, pmd) ? NULL : pte_offset_map(pmd, address))

#define pte_alloc_map_lock(mm, pmd, address, ptlp)	\
	(pte_alloc(mm, pmd) ?			\
		 NULL : pte_offset_map_lock(mm, pmd, address, ptlp))

#define pte_alloc_kernel(pmd, address)			\
	((unlikely(pmd_none(*(pmd))) && __pte_alloc_kernel(pmd))? \
		NULL: pte_offset_kernel(pmd, address))

/*
 * 分割 PMD 锁（Split PMD Locks）
 *
 * 【CONFIG_SPLIT_PMD_PTLOCKS 说明】
 * - 每个 PMD 页表有独立的锁
 * - 类似于分割 PTE 锁
 * - 提高大页（huge page）和 PMD 级别操作的并发性
 */
#if defined(CONFIG_SPLIT_PMD_PTLOCKS)

/*
 * 获取 PMD 的页表页
 *
 * 【功能说明】从 PMD 指针计算对应的 page
 *
 * 【实现说明】
 * - PTRS_PER_PMD * sizeof(pmd_t) - 1: 创建掩码
 * - mask: 对齐到 PMD 表的起始地址
 * - virt_to_page: 虚拟地址转 page
 *
 * 【为什么需要掩码】
 * - PMD 指针可能指向 PMD 表的任意项
 * - 需要对齐到表的起始地址
 * - 才能获取正确的 page
 *
 * 【参数说明】
 * @pmd: PMD 页表项指针
 *
 * 【返回值】
 * - PMD 表所在的 page
 */
static inline struct page *pmd_pgtable_page(pmd_t *pmd)
{
	unsigned long mask = ~(PTRS_PER_PMD * sizeof(pmd_t) - 1);
	return virt_to_page((void *)((unsigned long) pmd & mask));
}

/*
 * 获取 PMD 的 ptdesc
 *
 * 【功能说明】从 PMD 指针获取页表描述符
 *
 * 【参数说明】
 * @pmd: PMD 页表项指针
 *
 * 【返回值】
 * - PMD 表的 ptdesc
 */
static inline struct ptdesc *pmd_ptdesc(pmd_t *pmd)
{
	return page_ptdesc(pmd_pgtable_page(pmd));
}

/*
 * 获取 PMD 锁指针
 *
 * 【功能说明】返回 PMD 表的锁
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pmd: PMD 页表项指针
 *
 * 【返回值】
 * - PMD 表的锁指针
 */
static inline spinlock_t *pmd_lockptr(struct mm_struct *mm, pmd_t *pmd)
{
	return ptlock_ptr(pmd_ptdesc(pmd));
}

/*
 * PMD ptlock 初始化
 *
 * 【功能说明】初始化 PMD 页表的锁
 *
 * 【实现说明】
 * - 初始化 pmd_huge_pte（透明大页支持）
 * - 调用 ptlock_init 初始化锁
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 *
 * 【返回值】
 * - true: 成功
 * - false: 失败
 */
static inline bool pmd_ptlock_init(struct ptdesc *ptdesc)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	/*
	 * 【透明大页支持】
	 * - pmd_huge_pte: 指向大页的 PTE
	 * - 初始化为 NULL
	 */
	ptdesc->pmd_huge_pte = NULL;
#endif
	return ptlock_init(ptdesc);
}

/*
 * 获取 PMD 的大页 PTE 宏
 *
 * 【功能说明】
 * - 返回 PMD 关联的大页 PTE
 * - 用于透明大页
 */
#define pmd_huge_pte(mm, pmd) (pmd_ptdesc(pmd)->pmd_huge_pte)

#else

/*
 * 不使用分割 PMD 锁的版本
 *
 * 【说明】
 * - 所有 PMD 使用 mm->page_table_lock
 */

/*
 * 获取 PMD 锁指针（共享版本）
 *
 * 【说明】返回整个 mm 的页表锁
 */
static inline spinlock_t *pmd_lockptr(struct mm_struct *mm, pmd_t *pmd)
{
	return &mm->page_table_lock;
}

/*
 * PMD ptlock 初始化（共享版本）
 *
 * 【说明】总是返回 true
 */
static inline bool pmd_ptlock_init(struct ptdesc *ptdesc) { return true; }

/*
 * 获取 PMD 的大页 PTE 宏（共享版本）
 *
 * 【说明】
 * - 从 mm 获取 pmd_huge_pte
 * - 所有 PMD 共享一个
 */
#define pmd_huge_pte(mm, pmd) ((mm)->pmd_huge_pte)

#endif

/*
 * PMD 加锁函数
 *
 * 【功能说明】
 * - 获取 PMD 的锁
 * - 加锁
 * - 返回锁指针
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pmd: PMD 页表项指针
 *
 * 【返回值】
 * - 锁指针（已加锁）
 */
static inline spinlock_t *pmd_lock(struct mm_struct *mm, pmd_t *pmd)
{
	spinlock_t *ptl = pmd_lockptr(mm, pmd);
	spin_lock(ptl);
	return ptl;
}

/*
 * PMD 页表构造函数
 *
 * 【功能说明】构造 PMD 页表
 *
 * 【实现说明】
 * - 初始化 PMD ptlock（对于用户进程）
 * - 初始化 PMD PTS（Page Table Statistics）
 * - 调用通用构造函数
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @ptdesc: 页表描述符
 *
 * 【返回值】
 * - true: 成功
 * - false: 失败
 */
static inline bool pagetable_pmd_ctor(struct mm_struct *mm,
				      struct ptdesc *ptdesc)
{
	/*
	 * 【初始化 PMD ptlock】
	 * - init_mm 不需要（内核页表）
	 */
	if (mm != &init_mm && !pmd_ptlock_init(ptdesc))
		return false;
	/*
	 * 【初始化 PMD PTS】
	 * - 页表统计信息
	 */
	ptdesc_pmd_pts_init(ptdesc);
	/*
	 * 【调用通用构造函数】
	 */
	__pagetable_ctor(ptdesc);
	return true;
}

/*
 * PUD 锁（未分割）
 *
 * 【说明】
 * - 暂时没有分割 PUD 锁的可扩展性理由
 * - 但遵循与 PMD 锁相同的模式，以便在需要时更容易实现
 * - VM 还未准备好切换到分割 PUD 锁
 * - 可能有些地方需要从 page_table_lock 转换
 */
/*
 * No scalability reason to split PUD locks yet, but follow the same pattern
 * as the PMD locks to make it easier if we decide to.  The VM should not be
 * considered ready to switch to split PUD locks yet; there may be places
 * which need to be converted from page_table_lock.
 */

/*
 * 获取 PUD 锁指针
 *
 * 【说明】
 * - 返回整个 mm 的页表锁
 * - 目前不支持分割 PUD 锁
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pud: PUD 页表项指针
 *
 * 【返回值】
 * - 锁指针
 */
static inline spinlock_t *pud_lockptr(struct mm_struct *mm, pud_t *pud)
{
	return &mm->page_table_lock;
}

/*
 * PUD 加锁函数
 *
 * 【功能说明】
 * - 获取 PUD 的锁
 * - 加锁
 * - 返回锁指针
 *
 * 【参数说明】
 * @mm: 进程的内存描述符
 * @pud: PUD 页表项指针
 *
 * 【返回值】
 * - 锁指针（已加锁）
 */
static inline spinlock_t *pud_lock(struct mm_struct *mm, pud_t *pud)
{
	spinlock_t *ptl = pud_lockptr(mm, pud);

	spin_lock(ptl);
	return ptl;
}

/*
 * PUD 页表构造函数
 *
 * 【功能说明】构造 PUD 页表
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 */
static inline void pagetable_pud_ctor(struct ptdesc *ptdesc)
{
	__pagetable_ctor(ptdesc);
}

/*
 * P4D 页表构造函数
 *
 * 【功能说明】构造 P4D 页表
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 */
static inline void pagetable_p4d_ctor(struct ptdesc *ptdesc)
{
	__pagetable_ctor(ptdesc);
}

/*
 * PGD 页表构造函数
 *
 * 【功能说明】构造 PGD 页表
 *
 * 【参数说明】
 * @ptdesc: 页表描述符
 */
static inline void pagetable_pgd_ctor(struct ptdesc *ptdesc)
{
	__pagetable_ctor(ptdesc);
}

/*
 * 页缓存和内存初始化函数
 *
 * 【pagecache_init 说明】
 * - 初始化页缓存子系统
 * - 系统启动时调用
 */

/*
 * 初始化页缓存（外部实现）
 */
extern void __init pagecache_init(void);

/*
 * 释放初始化内存（外部实现）
 *
 * 【功能说明】
 * - 释放 __init 段的内存
 * - 系统启动完成后调用
 * - 回收初始化代码使用的内存
 */
extern void free_initmem(void);

/*
 * 释放预留区域
 *
 * 【功能说明】
 * - 释放 [PAGE_ALIGN(start), end & PAGE_MASK) 范围内的预留页面到伙伴系统
 * - 如果 poison 在 [0, UCHAR_MAX] 范围内，用该模式毒化释放的页面
 *
 * 【参数说明】
 * @start: 起始地址
 * @end: 结束地址
 * @poison: 毒化模式
 * @s: 描述字符串（用于统计）
 *
 * 【返回值】
 * - 释放到伙伴系统的页面数
 */
/*
 * Free reserved pages within range [PAGE_ALIGN(start), end & PAGE_MASK)
 * into the buddy system. The freed pages will be poisoned with pattern
 * "poison" if it's within range [0, UCHAR_MAX].
 * Return pages freed into the buddy system.
 */
extern unsigned long free_reserved_area(void *start, void *end,
					int poison, const char *s);

/*
 * 调整受管理的页面计数（外部实现）
 *
 * 【功能说明】
 * - 调整 zone 的 managed_pages 计数
 * - 用于页面预留/释放
 *
 * 【参数说明】
 * @page: 页面指针
 * @count: 调整量（可正可负）
 */
extern void adjust_managed_page_count(struct page *page, long count);

/*
 * 释放预留页面（外部实现）
 *
 * 【功能说明】
 * - 将预留页面释放到伙伴系统
 * - 使其被管理
 */
/*
 * Free the reserved page into the buddy system, so it gets managed.
 */
void free_reserved_page(struct page *page);

/*
 * 标记页面为预留
 *
 * 【功能说明】
 * - 设置 PG_reserved 标志
 * - 减少受管理的页面计数
 *
 * 【参数说明】
 * @page: 页面指针
 */
static inline void mark_page_reserved(struct page *page)
{
	SetPageReserved(page);
	adjust_managed_page_count(page, -1);
}

/*
 * 释放预留的 ptdesc
 *
 * 【功能说明】
 * - 将 ptdesc 对应的页面释放
 * - 用于页表的特殊情况
 *
 * 【参数说明】
 * @pt: 页表描述符
 */
static inline void free_reserved_ptdesc(struct ptdesc *pt)
{
	free_reserved_page(ptdesc_page(pt));
}

/*
 * 释放初始化内存的默认方法
 *
 * 【功能说明】
 * - 将所有 __init 内存释放到伙伴系统
 * - 如果 poison 在 [0, UCHAR_MAX] 范围内，用该模式毒化释放的页面
 *
 * 【参数说明】
 * @poison: 毒化模式
 *
 * 【返回值】
 * - 释放到伙伴系统的页面数
 */
/*
 * Default method to free all the __init memory into the buddy system.
 * The freed pages will be poisoned with pattern "poison" if it's within
 * range [0, UCHAR_MAX].
 * Return pages freed into the buddy system.
 */
static inline unsigned long free_initmem_default(int poison)
{
	/*
	 * 【__init 段边界】
	 * - __init_begin: 初始化段起始
	 * - __init_end: 初始化段结束
	 * - 链接器定义的符号
	 */
	extern char __init_begin[], __init_end[];

	/*
	 * 【释放初始化内存】
	 * - "unused kernel image (initmem)": 统计描述
	 */
	return free_reserved_area(&__init_begin, &__init_end,
				  poison, "unused kernel image (initmem)");
}

/*
 * 获取物理页面数
 *
 * 【功能说明】返回系统的物理页面总数
 */
static inline unsigned long get_num_physpages(void)
{
	int nid;
	unsigned long phys_pages = 0;

	for_each_online_node(nid)
		phys_pages += node_present_pages(nid);

	return phys_pages;
}

/*
 * FIXME: Using memblock node mappings, an architecture may initialise its
 * zones, allocate the backing mem_map and account for memory holes in an
 * architecture independent manner.
 *
 * An architecture is expected to register range of page frames backed by
 * physical memory with memblock_add[_node]() before calling
 * free_area_init() passing in the PFN each zone ends at. At a basic
 * usage, an architecture is expected to do something like
 *
 * unsigned long max_zone_pfns[MAX_NR_ZONES] = {max_dma, max_normal_pfn,
 * 							 max_highmem_pfn};
 * for_each_valid_physical_page_range()
 *	memblock_add_node(base, size, nid, MEMBLOCK_NONE)
 * free_area_init(max_zone_pfns);
 */
void arch_zone_limits_init(unsigned long *max_zone_pfn);
unsigned long node_map_pfn_alignment(void);
extern unsigned long absent_pages_in_range(unsigned long start_pfn,
						unsigned long end_pfn);
extern void get_pfn_range_for_nid(unsigned int nid,
			unsigned long *start_pfn, unsigned long *end_pfn);

#ifndef CONFIG_NUMA
static inline int early_pfn_to_nid(unsigned long pfn)
{
	return 0;
}
#else
/* please see mm/page_alloc.c */
extern int __meminit early_pfn_to_nid(unsigned long pfn);
#endif

extern void mem_init(void);
extern void __init mmap_init(void);

extern void __show_mem(unsigned int flags, nodemask_t *nodemask, int max_zone_idx);
static inline void show_mem(void)
{
	__show_mem(0, NULL, MAX_NR_ZONES - 1);
}
extern long si_mem_available(void);
extern void si_meminfo(struct sysinfo * val);
extern void si_meminfo_node(struct sysinfo *val, int nid);

extern __printf(3, 4)
void warn_alloc(gfp_t gfp_mask, nodemask_t *nodemask, const char *fmt, ...);

extern void setup_per_cpu_pageset(void);

/* nommu.c */
extern atomic_long_t mmap_pages_allocated;
extern int nommu_shrink_inode_mappings(struct inode *, size_t, size_t);

/* interval_tree.c */
void vma_interval_tree_insert(struct vm_area_struct *node,
			      struct rb_root_cached *root);
void vma_interval_tree_insert_after(struct vm_area_struct *node,
				    struct vm_area_struct *prev,
				    struct rb_root_cached *root);
void vma_interval_tree_remove(struct vm_area_struct *node,
			      struct rb_root_cached *root);
struct vm_area_struct *vma_interval_tree_subtree_search(struct vm_area_struct *node,
				unsigned long start, unsigned long last);
struct vm_area_struct *vma_interval_tree_iter_first(struct rb_root_cached *root,
				unsigned long start, unsigned long last);
struct vm_area_struct *vma_interval_tree_iter_next(struct vm_area_struct *node,
				unsigned long start, unsigned long last);

#define vma_interval_tree_foreach(vma, root, start, last)		\
	for (vma = vma_interval_tree_iter_first(root, start, last);	\
	     vma; vma = vma_interval_tree_iter_next(vma, start, last))

void anon_vma_interval_tree_insert(struct anon_vma_chain *node,
				   struct rb_root_cached *root);
void anon_vma_interval_tree_remove(struct anon_vma_chain *node,
				   struct rb_root_cached *root);
struct anon_vma_chain *
anon_vma_interval_tree_iter_first(struct rb_root_cached *root,
				  unsigned long start, unsigned long last);
struct anon_vma_chain *anon_vma_interval_tree_iter_next(
	struct anon_vma_chain *node, unsigned long start, unsigned long last);
#ifdef CONFIG_DEBUG_VM_RB
void anon_vma_interval_tree_verify(struct anon_vma_chain *node);
#endif

#define anon_vma_interval_tree_foreach(avc, root, start, last)		 \
	for (avc = anon_vma_interval_tree_iter_first(root, start, last); \
	     avc; avc = anon_vma_interval_tree_iter_next(avc, start, last))

/* mmap.c */
extern int __vm_enough_memory(const struct mm_struct *mm, long pages, int cap_sys_admin);
extern int insert_vm_struct(struct mm_struct *, struct vm_area_struct *);
extern void exit_mmap(struct mm_struct *);
bool mmap_read_lock_maybe_expand(struct mm_struct *mm, struct vm_area_struct *vma,
				 unsigned long addr, bool write);

static inline int check_data_rlimit(unsigned long rlim,
				    unsigned long new,
				    unsigned long start,
				    unsigned long end_data,
				    unsigned long start_data)
{
	if (rlim < RLIM_INFINITY) {
		if (((new - start) + (end_data - start_data)) > rlim)
			return -ENOSPC;
	}

	return 0;
}

extern int mm_take_all_locks(struct mm_struct *mm);
extern void mm_drop_all_locks(struct mm_struct *mm);

extern int set_mm_exe_file(struct mm_struct *mm, struct file *new_exe_file);
extern int replace_mm_exe_file(struct mm_struct *mm, struct file *new_exe_file);
extern struct file *get_mm_exe_file(struct mm_struct *mm);
extern struct file *get_task_exe_file(struct task_struct *task);

extern void vm_stat_account(struct mm_struct *, vm_flags_t, long npages);

extern bool vma_is_special_mapping(const struct vm_area_struct *vma,
				   const struct vm_special_mapping *sm);
struct vm_area_struct *_install_special_mapping(struct mm_struct *mm,
				   unsigned long addr, unsigned long len,
				   vm_flags_t vm_flags,
				   const struct vm_special_mapping *spec);

unsigned long randomize_stack_top(unsigned long stack_top);
unsigned long randomize_page(unsigned long start, unsigned long range);

unsigned long
__get_unmapped_area(struct file *file, unsigned long addr, unsigned long len,
		    unsigned long pgoff, unsigned long flags, vm_flags_t vm_flags);

static inline unsigned long
get_unmapped_area(struct file *file, unsigned long addr, unsigned long len,
		  unsigned long pgoff, unsigned long flags)
{
	return __get_unmapped_area(file, addr, len, pgoff, flags, 0);
}

extern unsigned long do_mmap(struct file *file, unsigned long addr,
	unsigned long len, unsigned long prot, unsigned long flags,
	vm_flags_t vm_flags, unsigned long pgoff, unsigned long *populate,
	struct list_head *uf);
extern int do_vmi_munmap(struct vma_iterator *vmi, struct mm_struct *mm,
			 unsigned long start, size_t len, struct list_head *uf,
			 bool unlock);
int do_vmi_align_munmap(struct vma_iterator *vmi, struct vm_area_struct *vma,
		    struct mm_struct *mm, unsigned long start,
		    unsigned long end, struct list_head *uf, bool unlock);
extern int do_munmap(struct mm_struct *, unsigned long, size_t,
		     struct list_head *uf);
extern int do_madvise(struct mm_struct *mm, unsigned long start, size_t len_in, int behavior);

#ifdef CONFIG_MMU
extern int __mm_populate(unsigned long addr, unsigned long len,
			 int ignore_errors);
static inline void mm_populate(unsigned long addr, unsigned long len)
{
	/* Ignore errors */
	(void) __mm_populate(addr, len, 1);
}
#else
static inline void mm_populate(unsigned long addr, unsigned long len) {}
#endif

/* This takes the mm semaphore itself */
int __must_check vm_brk_flags(unsigned long addr, unsigned long request, bool is_exec);
int vm_munmap(unsigned long start, size_t len);
unsigned long __must_check vm_mmap(struct file *file, unsigned long addr,
		unsigned long len, unsigned long prot,
		unsigned long flag, unsigned long offset);
unsigned long __must_check vm_mmap_shadow_stack(unsigned long addr,
		unsigned long len, unsigned long flags);

struct vm_unmapped_area_info {
#define VM_UNMAPPED_AREA_TOPDOWN 1
	unsigned long flags;
	unsigned long length;
	unsigned long low_limit;
	unsigned long high_limit;
	unsigned long align_mask;
	unsigned long align_offset;
	unsigned long start_gap;
};

extern unsigned long vm_unmapped_area(struct vm_unmapped_area_info *info);

/* truncate.c */
void truncate_inode_pages(struct address_space *mapping, loff_t lstart);
void truncate_inode_pages_range(struct address_space *mapping, loff_t lstart,
		uoff_t lend);
void truncate_inode_pages_final(struct address_space *mapping);

/* generic vm_area_ops exported for stackable file systems */
extern vm_fault_t filemap_fault(struct vm_fault *vmf);
extern vm_fault_t filemap_map_pages(struct vm_fault *vmf,
		pgoff_t start_pgoff, pgoff_t end_pgoff);
extern vm_fault_t filemap_page_mkwrite(struct vm_fault *vmf);

extern unsigned long stack_guard_gap;
/* Generic expand stack which grows the stack according to GROWS{UP,DOWN} */
int expand_stack_locked(struct vm_area_struct *vma, unsigned long address);
struct vm_area_struct *expand_stack(struct mm_struct * mm, unsigned long addr);

/* Look up the first VMA which satisfies  addr < vm_end,  NULL if none. */
extern struct vm_area_struct * find_vma(struct mm_struct * mm, unsigned long addr);
extern struct vm_area_struct * find_vma_prev(struct mm_struct * mm, unsigned long addr,
					     struct vm_area_struct **pprev);

/*
 * Look up the first VMA which intersects the interval [start_addr, end_addr)
 * NULL if none.  Assume start_addr < end_addr.
 */
struct vm_area_struct *find_vma_intersection(struct mm_struct *mm,
			unsigned long start_addr, unsigned long end_addr);

/**
 * vma_lookup() - Find a VMA at a specific address
 * @mm: The process address space.
 * @addr: The user address.
 *
 * Return: The vm_area_struct at the given address, %NULL otherwise.
 */
static inline
struct vm_area_struct *vma_lookup(struct mm_struct *mm, unsigned long addr)
{
	return mtree_load(&mm->mm_mt, addr);
}

static inline unsigned long stack_guard_start_gap(const struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_GROWSDOWN)
		return stack_guard_gap;

	/* See reasoning around the VM_SHADOW_STACK definition */
	if (vma->vm_flags & VM_SHADOW_STACK)
		return PAGE_SIZE;

	return 0;
}

static inline unsigned long vm_start_gap(const struct vm_area_struct *vma)
{
	unsigned long gap = stack_guard_start_gap(vma);
	unsigned long vm_start = vma->vm_start;

	vm_start -= gap;
	if (vm_start > vma->vm_start)
		vm_start = 0;
	return vm_start;
}

static inline unsigned long vm_end_gap(const struct vm_area_struct *vma)
{
	unsigned long vm_end = vma->vm_end;

	if (vma->vm_flags & VM_GROWSUP) {
		vm_end += stack_guard_gap;
		if (vm_end < vma->vm_end)
			vm_end = -PAGE_SIZE;
	}
	return vm_end;
}

static inline unsigned long vma_pages(const struct vm_area_struct *vma)
{
	return (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
}

static inline unsigned long vma_last_pgoff(struct vm_area_struct *vma)
{
	return vma->vm_pgoff + vma_pages(vma) - 1;
}

static inline unsigned long vma_desc_size(const struct vm_area_desc *desc)
{
	return desc->end - desc->start;
}

static inline unsigned long vma_desc_pages(const struct vm_area_desc *desc)
{
	return vma_desc_size(desc) >> PAGE_SHIFT;
}

/**
 * mmap_action_remap - helper for mmap_prepare hook to specify that a pure PFN
 * remap is required.
 * @desc: The VMA descriptor for the VMA requiring remap.
 * @start: The virtual address to start the remap from, must be within the VMA.
 * @start_pfn: The first PFN in the range to remap.
 * @size: The size of the range to remap, in bytes, at most spanning to the end
 * of the VMA.
 */
static inline void mmap_action_remap(struct vm_area_desc *desc,
				     unsigned long start,
				     unsigned long start_pfn,
				     unsigned long size)
{
	struct mmap_action *action = &desc->action;

	/* [start, start + size) must be within the VMA. */
	WARN_ON_ONCE(start < desc->start || start >= desc->end);
	WARN_ON_ONCE(start + size > desc->end);

	action->type = MMAP_REMAP_PFN;
	action->remap.start = start;
	action->remap.start_pfn = start_pfn;
	action->remap.size = size;
	action->remap.pgprot = desc->page_prot;
}

/**
 * mmap_action_remap_full - helper for mmap_prepare hook to specify that the
 * entirety of a VMA should be PFN remapped.
 * @desc: The VMA descriptor for the VMA requiring remap.
 * @start_pfn: The first PFN in the range to remap.
 */
static inline void mmap_action_remap_full(struct vm_area_desc *desc,
					  unsigned long start_pfn)
{
	mmap_action_remap(desc, desc->start, start_pfn, vma_desc_size(desc));
}

/**
 * mmap_action_ioremap - helper for mmap_prepare hook to specify that a pure PFN
 * I/O remap is required.
 * @desc: The VMA descriptor for the VMA requiring remap.
 * @start: The virtual address to start the remap from, must be within the VMA.
 * @start_pfn: The first PFN in the range to remap.
 * @size: The size of the range to remap, in bytes, at most spanning to the end
 * of the VMA.
 */
static inline void mmap_action_ioremap(struct vm_area_desc *desc,
				       unsigned long start,
				       unsigned long start_pfn,
				       unsigned long size)
{
	mmap_action_remap(desc, start, start_pfn, size);
	desc->action.type = MMAP_IO_REMAP_PFN;
}

/**
 * mmap_action_ioremap_full - helper for mmap_prepare hook to specify that the
 * entirety of a VMA should be PFN I/O remapped.
 * @desc: The VMA descriptor for the VMA requiring remap.
 * @start_pfn: The first PFN in the range to remap.
 */
static inline void mmap_action_ioremap_full(struct vm_area_desc *desc,
					    unsigned long start_pfn)
{
	mmap_action_ioremap(desc, desc->start, start_pfn, vma_desc_size(desc));
}

/**
 * mmap_action_simple_ioremap - helper for mmap_prepare hook to specify that the
 * physical range in [start_phys_addr, start_phys_addr + size) should be I/O
 * remapped.
 * @desc: The VMA descriptor for the VMA requiring remap.
 * @start_phys_addr: Start of the physical memory to be mapped.
 * @size: Size of the area to map.
 *
 * NOTE: Some drivers might want to tweak desc->page_prot for purposes of
 * write-combine or similar.
 */
static inline void mmap_action_simple_ioremap(struct vm_area_desc *desc,
					      phys_addr_t start_phys_addr,
					      unsigned long size)
{
	struct mmap_action *action = &desc->action;

	action->simple_ioremap.start_phys_addr = start_phys_addr;
	action->simple_ioremap.size = size;
	action->type = MMAP_SIMPLE_IO_REMAP;
}

/**
 * mmap_action_map_kernel_pages - helper for mmap_prepare hook to specify that
 * @num kernel pages contained in the @pages array should be mapped to userland
 * starting at virtual address @start.
 * @desc: The VMA descriptor for the VMA requiring kernel pags to be mapped.
 * @start: The virtual address from which to map them.
 * @pages: An array of struct page pointers describing the memory to map.
 * @nr_pages: The number of entries in the @pages aray.
 */
static inline void mmap_action_map_kernel_pages(struct vm_area_desc *desc,
		unsigned long start, struct page **pages,
		unsigned long nr_pages)
{
	struct mmap_action *action = &desc->action;

	action->type = MMAP_MAP_KERNEL_PAGES;
	action->map_kernel.start = start;
	action->map_kernel.pages = pages;
	action->map_kernel.nr_pages = nr_pages;
	action->map_kernel.pgoff = desc->pgoff;
}

/**
 * mmap_action_map_kernel_pages_full - helper for mmap_prepare hook to specify that
 * kernel pages contained in the @pages array should be mapped to userland
 * from @desc->start to @desc->end.
 * @desc: The VMA descriptor for the VMA requiring kernel pags to be mapped.
 * @pages: An array of struct page pointers describing the memory to map.
 *
 * The caller must ensure that @pages contains sufficient entries to cover the
 * entire range described by @desc.
 */
static inline void mmap_action_map_kernel_pages_full(struct vm_area_desc *desc,
		struct page **pages)
{
	mmap_action_map_kernel_pages(desc, desc->start, pages,
				     vma_desc_pages(desc));
}

int mmap_action_prepare(struct vm_area_desc *desc);
int mmap_action_complete(struct vm_area_struct *vma,
			 struct mmap_action *action, bool is_compat);

/* Look up the first VMA which exactly match the interval vm_start ... vm_end */
static inline struct vm_area_struct *find_exact_vma(struct mm_struct *mm,
				unsigned long vm_start, unsigned long vm_end)
{
	struct vm_area_struct *vma = vma_lookup(mm, vm_start);

	if (vma && (vma->vm_start != vm_start || vma->vm_end != vm_end))
		vma = NULL;

	return vma;
}

/**
 * range_is_subset - Is the specified inner range a subset of the outer range?
 * @outer_start: The start of the outer range.
 * @outer_end: The exclusive end of the outer range.
 * @inner_start: The start of the inner range.
 * @inner_end: The exclusive end of the inner range.
 *
 * Returns: %true if [inner_start, inner_end) is a subset of [outer_start,
 * outer_end), otherwise %false.
 */
static inline bool range_is_subset(unsigned long outer_start,
				   unsigned long outer_end,
				   unsigned long inner_start,
				   unsigned long inner_end)
{
	return outer_start <= inner_start && inner_end <= outer_end;
}

/**
 * range_in_vma - is the specified [@start, @end) range a subset of the VMA?
 * @vma: The VMA against which we want to check [@start, @end).
 * @start: The start of the range we wish to check.
 * @end: The exclusive end of the range we wish to check.
 *
 * Returns: %true if [@start, @end) is a subset of [@vma->vm_start,
 * @vma->vm_end), %false otherwise.
 */
static inline bool range_in_vma(const struct vm_area_struct *vma,
				unsigned long start, unsigned long end)
{
	if (!vma)
		return false;

	return range_is_subset(vma->vm_start, vma->vm_end, start, end);
}

/**
 * range_in_vma_desc - is the specified [@start, @end) range a subset of the VMA
 * described by @desc, a VMA descriptor?
 * @desc: The VMA descriptor against which we want to check [@start, @end).
 * @start: The start of the range we wish to check.
 * @end: The exclusive end of the range we wish to check.
 *
 * Returns: %true if [@start, @end) is a subset of [@desc->start, @desc->end),
 * %false otherwise.
 */
static inline bool range_in_vma_desc(const struct vm_area_desc *desc,
				     unsigned long start, unsigned long end)
{
	if (!desc)
		return false;

	return range_is_subset(desc->start, desc->end, start, end);
}

#ifdef CONFIG_MMU
pgprot_t vm_get_page_prot(vm_flags_t vm_flags);

static inline pgprot_t vma_get_page_prot(vma_flags_t vma_flags)
{
	const vm_flags_t vm_flags = vma_flags_to_legacy(vma_flags);

	return vm_get_page_prot(vm_flags);
}

void vma_set_page_prot(struct vm_area_struct *vma);
#else
static inline pgprot_t vm_get_page_prot(vm_flags_t vm_flags)
{
	return __pgprot(0);
}
static inline pgprot_t vma_get_page_prot(vma_flags_t vma_flags)
{
	return __pgprot(0);
}
static inline void vma_set_page_prot(struct vm_area_struct *vma)
{
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
}
#endif

void vma_set_file(struct vm_area_struct *vma, struct file *file);

#ifdef CONFIG_NUMA_BALANCING
unsigned long change_prot_numa(struct vm_area_struct *vma,
			unsigned long start, unsigned long end);
#endif

struct vm_area_struct *find_extend_vma_locked(struct mm_struct *,
		unsigned long addr);
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
		    unsigned long pfn, unsigned long size, pgprot_t pgprot);

int vm_insert_page(struct vm_area_struct *, unsigned long addr, struct page *);
int vm_insert_pages(struct vm_area_struct *vma, unsigned long addr,
			struct page **pages, unsigned long *num);
int map_kernel_pages_prepare(struct vm_area_desc *desc);
int map_kernel_pages_complete(struct vm_area_struct *vma,
			      struct mmap_action *action);
int vm_map_pages(struct vm_area_struct *vma, struct page **pages,
				unsigned long num);
int vm_map_pages_zero(struct vm_area_struct *vma, struct page **pages,
				unsigned long num);
vm_fault_t vmf_insert_page_mkwrite(struct vm_fault *vmf, struct page *page,
			bool write);
vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr,
			unsigned long pfn);
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
			unsigned long pfn, pgprot_t pgprot);
vm_fault_t vmf_insert_mixed(struct vm_area_struct *vma, unsigned long addr,
			unsigned long pfn);
vm_fault_t vmf_insert_mixed_mkwrite(struct vm_area_struct *vma,
		unsigned long addr, unsigned long pfn);
int vm_iomap_memory(struct vm_area_struct *vma, phys_addr_t start, unsigned long len);

static inline vm_fault_t vmf_insert_page(struct vm_area_struct *vma,
				unsigned long addr, struct page *page)
{
	int err = vm_insert_page(vma, addr, page);

	if (err == -ENOMEM)
		return VM_FAULT_OOM;
	if (err < 0 && err != -EBUSY)
		return VM_FAULT_SIGBUS;

	return VM_FAULT_NOPAGE;
}

#ifndef io_remap_pfn_range_pfn
static inline unsigned long io_remap_pfn_range_pfn(unsigned long pfn,
		unsigned long size)
{
	return pfn;
}
#endif

static inline int io_remap_pfn_range(struct vm_area_struct *vma,
				     unsigned long addr, unsigned long orig_pfn,
				     unsigned long size, pgprot_t orig_prot)
{
	const unsigned long pfn = io_remap_pfn_range_pfn(orig_pfn, size);
	const pgprot_t prot = pgprot_decrypted(orig_prot);

	return remap_pfn_range(vma, addr, pfn, size, prot);
}

static inline vm_fault_t vmf_error(int err)
{
	if (err == -ENOMEM)
		return VM_FAULT_OOM;
	else if (err == -EHWPOISON)
		return VM_FAULT_HWPOISON;
	return VM_FAULT_SIGBUS;
}

/*
 * Convert errno to return value for ->page_mkwrite() calls.
 *
 * This should eventually be merged with vmf_error() above, but will need a
 * careful audit of all vmf_error() callers.
 */
static inline vm_fault_t vmf_fs_error(int err)
{
	if (err == 0)
		return VM_FAULT_LOCKED;
	if (err == -EFAULT || err == -EAGAIN)
		return VM_FAULT_NOPAGE;
	if (err == -ENOMEM)
		return VM_FAULT_OOM;
	/* -ENOSPC, -EDQUOT, -EIO ... */
	return VM_FAULT_SIGBUS;
}

static inline int vm_fault_to_errno(vm_fault_t vm_fault, int foll_flags)
{
	if (vm_fault & VM_FAULT_OOM)
		return -ENOMEM;
	if (vm_fault & (VM_FAULT_HWPOISON | VM_FAULT_HWPOISON_LARGE))
		return (foll_flags & FOLL_HWPOISON) ? -EHWPOISON : -EFAULT;
	if (vm_fault & (VM_FAULT_SIGBUS | VM_FAULT_SIGSEGV))
		return -EFAULT;
	return 0;
}

/*
 * Indicates whether GUP can follow a PROT_NONE mapped page, or whether
 * a (NUMA hinting) fault is required.
 */
static inline bool gup_can_follow_protnone(const struct vm_area_struct *vma,
					   unsigned int flags)
{
	/*
	 * If callers don't want to honor NUMA hinting faults, no need to
	 * determine if we would actually have to trigger a NUMA hinting fault.
	 */
	if (!(flags & FOLL_HONOR_NUMA_FAULT))
		return true;

	/*
	 * NUMA hinting faults don't apply in inaccessible (PROT_NONE) VMAs.
	 *
	 * Requiring a fault here even for inaccessible VMAs would mean that
	 * FOLL_FORCE cannot make any progress, because handle_mm_fault()
	 * refuses to process NUMA hinting faults in inaccessible VMAs.
	 */
	return !vma_is_accessible(vma);
}

typedef int (*pte_fn_t)(pte_t *pte, unsigned long addr, void *data);
extern int apply_to_page_range(struct mm_struct *mm, unsigned long address,
			       unsigned long size, pte_fn_t fn, void *data);
extern int apply_to_existing_page_range(struct mm_struct *mm,
				   unsigned long address, unsigned long size,
				   pte_fn_t fn, void *data);

#ifdef CONFIG_PAGE_POISONING
extern void __kernel_poison_pages(struct page *page, int numpages);
extern void __kernel_unpoison_pages(struct page *page, int numpages);
extern bool _page_poisoning_enabled_early;
DECLARE_STATIC_KEY_FALSE(_page_poisoning_enabled);
static inline bool page_poisoning_enabled(void)
{
	return _page_poisoning_enabled_early;
}
/*
 * For use in fast paths after init_mem_debugging() has run, or when a
 * false negative result is not harmful when called too early.
 */
static inline bool page_poisoning_enabled_static(void)
{
	return static_branch_unlikely(&_page_poisoning_enabled);
}
static inline void kernel_poison_pages(struct page *page, int numpages)
{
	if (page_poisoning_enabled_static())
		__kernel_poison_pages(page, numpages);
}
static inline void kernel_unpoison_pages(struct page *page, int numpages)
{
	if (page_poisoning_enabled_static())
		__kernel_unpoison_pages(page, numpages);
}
#else
static inline bool page_poisoning_enabled(void) { return false; }
static inline bool page_poisoning_enabled_static(void) { return false; }
static inline void __kernel_poison_pages(struct page *page, int nunmpages) { }
static inline void kernel_poison_pages(struct page *page, int numpages) { }
static inline void kernel_unpoison_pages(struct page *page, int numpages) { }
#endif

DECLARE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_ALLOC_DEFAULT_ON, init_on_alloc);
static inline bool want_init_on_alloc(gfp_t flags)
{
	if (static_branch_maybe(CONFIG_INIT_ON_ALLOC_DEFAULT_ON,
				&init_on_alloc))
		return true;
	return flags & __GFP_ZERO;
}

DECLARE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_FREE_DEFAULT_ON, init_on_free);
static inline bool want_init_on_free(void)
{
	return static_branch_maybe(CONFIG_INIT_ON_FREE_DEFAULT_ON,
				   &init_on_free);
}

extern bool _debug_pagealloc_enabled_early;
DECLARE_STATIC_KEY_FALSE(_debug_pagealloc_enabled);

static inline bool debug_pagealloc_enabled(void)
{
	return IS_ENABLED(CONFIG_DEBUG_PAGEALLOC) &&
		_debug_pagealloc_enabled_early;
}

/*
 * For use in fast paths after mem_debugging_and_hardening_init() has run,
 * or when a false negative result is not harmful when called too early.
 */
static inline bool debug_pagealloc_enabled_static(void)
{
	if (!IS_ENABLED(CONFIG_DEBUG_PAGEALLOC))
		return false;

	return static_branch_unlikely(&_debug_pagealloc_enabled);
}

/*
 * To support DEBUG_PAGEALLOC architecture must ensure that
 * __kernel_map_pages() never fails
 */
extern void __kernel_map_pages(struct page *page, int numpages, int enable);
#ifdef CONFIG_DEBUG_PAGEALLOC
static inline void debug_pagealloc_map_pages(struct page *page, int numpages)
{
	iommu_debug_check_unmapped(page, numpages);

	if (debug_pagealloc_enabled_static())
		__kernel_map_pages(page, numpages, 1);
}

static inline void debug_pagealloc_unmap_pages(struct page *page, int numpages)
{
	iommu_debug_check_unmapped(page, numpages);

	if (debug_pagealloc_enabled_static())
		__kernel_map_pages(page, numpages, 0);
}

extern unsigned int _debug_guardpage_minorder;
DECLARE_STATIC_KEY_FALSE(_debug_guardpage_enabled);

static inline unsigned int debug_guardpage_minorder(void)
{
	return _debug_guardpage_minorder;
}

static inline bool debug_guardpage_enabled(void)
{
	return static_branch_unlikely(&_debug_guardpage_enabled);
}

static inline bool page_is_guard(const struct page *page)
{
	if (!debug_guardpage_enabled())
		return false;

	return PageGuard(page);
}

bool __set_page_guard(struct zone *zone, struct page *page, unsigned int order);
static inline bool set_page_guard(struct zone *zone, struct page *page,
				  unsigned int order)
{
	if (!debug_guardpage_enabled())
		return false;
	return __set_page_guard(zone, page, order);
}

void __clear_page_guard(struct zone *zone, struct page *page, unsigned int order);
static inline void clear_page_guard(struct zone *zone, struct page *page,
				    unsigned int order)
{
	if (!debug_guardpage_enabled())
		return;
	__clear_page_guard(zone, page, order);
}

#else	/* CONFIG_DEBUG_PAGEALLOC */
static inline void debug_pagealloc_map_pages(struct page *page, int numpages) {}
static inline void debug_pagealloc_unmap_pages(struct page *page, int numpages) {}
static inline unsigned int debug_guardpage_minorder(void) { return 0; }
static inline bool debug_guardpage_enabled(void) { return false; }
static inline bool page_is_guard(const struct page *page) { return false; }
static inline bool set_page_guard(struct zone *zone, struct page *page,
			unsigned int order) { return false; }
static inline void clear_page_guard(struct zone *zone, struct page *page,
				unsigned int order) {}
#endif	/* CONFIG_DEBUG_PAGEALLOC */

#ifndef clear_pages
/**
 * clear_pages() - clear a page range for kernel-internal use.
 * @addr: start address
 * @npages: number of pages
 *
 * Use clear_user_pages() instead when clearing a page range to be
 * mapped to user space.
 *
 * Does absolutely no exception handling.
 *
 * Note that even though the clearing operation is preemptible, clear_pages()
 * does not (and on architectures where it reduces to a few long-running
 * instructions, might not be able to) call cond_resched() to check if
 * rescheduling is required.
 *
 * When running under preemptible models this is not a problem. Under
 * cooperatively scheduled models, however, the caller is expected to
 * limit @npages to no more than PROCESS_PAGES_NON_PREEMPT_BATCH.
 */
static inline void clear_pages(void *addr, unsigned int npages)
{
	do {
		clear_page(addr);
		addr += PAGE_SIZE;
	} while (--npages);
}
#endif

#ifndef PROCESS_PAGES_NON_PREEMPT_BATCH
#ifdef clear_pages
/*
 * The architecture defines clear_pages(), and we assume that it is
 * generally "fast". So choose a batch size large enough to allow the processor
 * headroom for optimizing the operation and yet small enough that we see
 * reasonable preemption latency for when this optimization is not possible
 * (ex. slow microarchitectures, memory bandwidth saturation.)
 *
 * With a value of 32MB and assuming a memory bandwidth of ~10GBps, this should
 * result in worst case preemption latency of around 3ms when clearing pages.
 *
 * (See comment above clear_pages() for why preemption latency is a concern
 * here.)
 */
#define PROCESS_PAGES_NON_PREEMPT_BATCH		(SZ_32M >> PAGE_SHIFT)
#else /* !clear_pages */
/*
 * The architecture does not provide a clear_pages() implementation. Assume
 * that clear_page() -- which clear_pages() will fallback to -- is relatively
 * slow and choose a small value for PROCESS_PAGES_NON_PREEMPT_BATCH.
 */
#define PROCESS_PAGES_NON_PREEMPT_BATCH		1
#endif
#endif

#ifdef __HAVE_ARCH_GATE_AREA
extern struct vm_area_struct *get_gate_vma(struct mm_struct *mm);
extern int in_gate_area_no_mm(unsigned long addr);
extern int in_gate_area(struct mm_struct *mm, unsigned long addr);
#else
static inline struct vm_area_struct *get_gate_vma(struct mm_struct *mm)
{
	return NULL;
}
static inline int in_gate_area_no_mm(unsigned long addr) { return 0; }
static inline int in_gate_area(struct mm_struct *mm, unsigned long addr)
{
	return 0;
}
#endif	/* __HAVE_ARCH_GATE_AREA */

bool process_shares_mm(const struct task_struct *p, const struct mm_struct *mm);

void drop_slab(void);

#ifndef CONFIG_MMU
#define randomize_va_space 0
#else
extern int randomize_va_space;
#endif

const char * arch_vma_name(struct vm_area_struct *vma);
#ifdef CONFIG_MMU
void print_vma_addr(char *prefix, unsigned long rip);
#else
static inline void print_vma_addr(char *prefix, unsigned long rip)
{
}
#endif

unsigned long section_map_size(void);
struct page * __populate_section_memmap(unsigned long pfn,
		unsigned long nr_pages, int nid, struct vmem_altmap *altmap,
		struct dev_pagemap *pgmap);
void *vmemmap_alloc_block(unsigned long size, int node);
struct vmem_altmap;
void *vmemmap_alloc_block_buf(unsigned long size, int node,
			      struct vmem_altmap *altmap);
void vmemmap_verify(pte_t *, int, unsigned long, unsigned long);
void vmemmap_set_pmd(pmd_t *pmd, void *p, int node,
		     unsigned long addr, unsigned long next);
int vmemmap_check_pmd(pmd_t *pmd, int node,
		      unsigned long addr, unsigned long next);
int vmemmap_populate_basepages(unsigned long start, unsigned long end,
			       int node, struct vmem_altmap *altmap);
int vmemmap_populate_hugepages(unsigned long start, unsigned long end,
			       int node, struct vmem_altmap *altmap);
int vmemmap_populate(unsigned long start, unsigned long end, int node,
		struct vmem_altmap *altmap);
int vmemmap_populate_hvo(unsigned long start, unsigned long end,
			 unsigned int order, struct zone *zone,
			 unsigned long headsize);
void vmemmap_wrprotect_hvo(unsigned long start, unsigned long end, int node,
			  unsigned long headsize);
void vmemmap_populate_print_last(void);
#ifdef CONFIG_MEMORY_HOTPLUG
void vmemmap_free(unsigned long start, unsigned long end,
		struct vmem_altmap *altmap);
#endif

#ifdef CONFIG_SPARSEMEM_VMEMMAP
static inline unsigned long vmem_altmap_offset(const struct vmem_altmap *altmap)
{
	/* number of pfns from base where pfn_to_page() is valid */
	if (altmap)
		return altmap->reserve + altmap->free;
	return 0;
}

static inline void vmem_altmap_free(struct vmem_altmap *altmap,
				    unsigned long nr_pfns)
{
	altmap->alloc -= nr_pfns;
}
#else
static inline unsigned long vmem_altmap_offset(const struct vmem_altmap *altmap)
{
	return 0;
}

static inline void vmem_altmap_free(struct vmem_altmap *altmap,
				    unsigned long nr_pfns)
{
}
#endif

#define VMEMMAP_RESERVE_NR	2
#ifdef CONFIG_ARCH_WANT_OPTIMIZE_DAX_VMEMMAP
static inline bool __vmemmap_can_optimize(struct vmem_altmap *altmap,
					  struct dev_pagemap *pgmap)
{
	unsigned long nr_pages;
	unsigned long nr_vmemmap_pages;

	if (!pgmap || !is_power_of_2(sizeof(struct page)))
		return false;

	nr_pages = pgmap_vmemmap_nr(pgmap);
	nr_vmemmap_pages = ((nr_pages * sizeof(struct page)) >> PAGE_SHIFT);
	/*
	 * For vmemmap optimization with DAX we need minimum 2 vmemmap
	 * pages. See layout diagram in Documentation/mm/vmemmap_dedup.rst
	 */
	return !altmap && (nr_vmemmap_pages > VMEMMAP_RESERVE_NR);
}
/*
 * If we don't have an architecture override, use the generic rule
 */
#ifndef vmemmap_can_optimize
#define vmemmap_can_optimize __vmemmap_can_optimize
#endif

#else
static inline bool vmemmap_can_optimize(struct vmem_altmap *altmap,
					   struct dev_pagemap *pgmap)
{
	return false;
}
#endif

enum mf_flags {
	MF_COUNT_INCREASED = 1 << 0,
	MF_ACTION_REQUIRED = 1 << 1,
	MF_MUST_KILL = 1 << 2,
	MF_SOFT_OFFLINE = 1 << 3,
	MF_UNPOISON = 1 << 4,
	MF_SW_SIMULATED = 1 << 5,
	MF_NO_RETRY = 1 << 6,
	MF_MEM_PRE_REMOVE = 1 << 7,
};
int mf_dax_kill_procs(struct address_space *mapping, pgoff_t index,
		      unsigned long count, int mf_flags);
extern int memory_failure(unsigned long pfn, int flags);
extern int unpoison_memory(unsigned long pfn);
extern atomic_long_t num_poisoned_pages __read_mostly;
extern int soft_offline_page(unsigned long pfn, int flags);
#ifdef CONFIG_MEMORY_FAILURE
/*
 * Sysfs entries for memory failure handling statistics.
 */
extern const struct attribute_group memory_failure_attr_group;
extern void memory_failure_queue(unsigned long pfn, int flags);
void num_poisoned_pages_inc(unsigned long pfn);
void num_poisoned_pages_sub(unsigned long pfn, long i);
#else
static inline void memory_failure_queue(unsigned long pfn, int flags)
{
}

static inline void num_poisoned_pages_inc(unsigned long pfn)
{
}

static inline void num_poisoned_pages_sub(unsigned long pfn, long i)
{
}
#endif

#if defined(CONFIG_MEMORY_FAILURE) && defined(CONFIG_MEMORY_HOTPLUG)
extern void memblk_nr_poison_inc(unsigned long pfn);
extern void memblk_nr_poison_sub(unsigned long pfn, long i);
#else
static inline void memblk_nr_poison_inc(unsigned long pfn)
{
}

static inline void memblk_nr_poison_sub(unsigned long pfn, long i)
{
}
#endif

#ifndef arch_memory_failure
static inline int arch_memory_failure(unsigned long pfn, int flags)
{
	return -ENXIO;
}
#endif

#ifndef arch_is_platform_page
static inline bool arch_is_platform_page(u64 paddr)
{
	return false;
}
#endif

/*
 * Error handlers for various types of pages.
 */
enum mf_result {
	MF_IGNORED,	/* Error: cannot be handled */
	MF_FAILED,	/* Error: handling failed */
	MF_DELAYED,	/* Will be handled later */
	MF_RECOVERED,	/* Successfully recovered */
};

enum mf_action_page_type {
	MF_MSG_KERNEL,
	MF_MSG_KERNEL_HIGH_ORDER,
	MF_MSG_DIFFERENT_COMPOUND,
	MF_MSG_HUGE,
	MF_MSG_FREE_HUGE,
	MF_MSG_GET_HWPOISON,
	MF_MSG_UNMAP_FAILED,
	MF_MSG_DIRTY_SWAPCACHE,
	MF_MSG_CLEAN_SWAPCACHE,
	MF_MSG_DIRTY_MLOCKED_LRU,
	MF_MSG_CLEAN_MLOCKED_LRU,
	MF_MSG_DIRTY_UNEVICTABLE_LRU,
	MF_MSG_CLEAN_UNEVICTABLE_LRU,
	MF_MSG_DIRTY_LRU,
	MF_MSG_CLEAN_LRU,
	MF_MSG_TRUNCATED_LRU,
	MF_MSG_BUDDY,
	MF_MSG_DAX,
	MF_MSG_UNSPLIT_THP,
	MF_MSG_ALREADY_POISONED,
	MF_MSG_PFN_MAP,
	MF_MSG_UNKNOWN,
};

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) || defined(CONFIG_HUGETLBFS)
void folio_zero_user(struct folio *folio, unsigned long addr_hint);
int copy_user_large_folio(struct folio *dst, struct folio *src,
			  unsigned long addr_hint,
			  struct vm_area_struct *vma);
long copy_folio_from_user(struct folio *dst_folio,
			   const void __user *usr_src,
			   bool allow_pagefault);

#endif /* CONFIG_TRANSPARENT_HUGEPAGE || CONFIG_HUGETLBFS */

#if MAX_NUMNODES > 1
void __init setup_nr_node_ids(void);
#else
static inline void setup_nr_node_ids(void) {}
#endif

extern int memcmp_pages(struct page *page1, struct page *page2);

static inline int pages_identical(struct page *page1, struct page *page2)
{
	return !memcmp_pages(page1, page2);
}

#ifdef CONFIG_MAPPING_DIRTY_HELPERS
unsigned long clean_record_shared_mapping_range(struct address_space *mapping,
						pgoff_t first_index, pgoff_t nr,
						pgoff_t bitmap_pgoff,
						unsigned long *bitmap,
						pgoff_t *start,
						pgoff_t *end);

unsigned long wp_shared_mapping_range(struct address_space *mapping,
				      pgoff_t first_index, pgoff_t nr);
#endif

#ifdef CONFIG_ANON_VMA_NAME
int set_anon_vma_name(unsigned long addr, unsigned long size,
		      const char __user *uname);
#else
static inline
int set_anon_vma_name(unsigned long addr, unsigned long size,
		      const char __user *uname)
{
	return -EINVAL;
}
#endif

#ifdef CONFIG_UNACCEPTED_MEMORY

bool range_contains_unaccepted_memory(phys_addr_t start, unsigned long size);
void accept_memory(phys_addr_t start, unsigned long size);

#else

static inline bool range_contains_unaccepted_memory(phys_addr_t start,
						    unsigned long size)
{
	return false;
}

static inline void accept_memory(phys_addr_t start, unsigned long size)
{
}

#endif

static inline bool pfn_is_unaccepted_memory(unsigned long pfn)
{
	return range_contains_unaccepted_memory(pfn << PAGE_SHIFT, PAGE_SIZE);
}

void vma_pgtable_walk_begin(struct vm_area_struct *vma);
void vma_pgtable_walk_end(struct vm_area_struct *vma);

int reserve_mem_find_by_name(const char *name, phys_addr_t *start, phys_addr_t *size);
int reserve_mem_release_by_name(const char *name);

#ifdef CONFIG_64BIT
int do_mseal(unsigned long start, size_t len_in, unsigned long flags);
#else
static inline int do_mseal(unsigned long start, size_t len_in, unsigned long flags)
{
	/* noop on 32 bit */
	return 0;
}
#endif

/**
 * user_alloc_needs_zeroing - 检查用户 folio 分配是否需要清零
 * Check if a user folio from page allocator needs to be zeroed or not.
 *
 * 设计原因：
 * 为了安全和正确性，新分配给用户空间的页面需要清零，避免泄露内核数据。
 * 但在某些架构上，清零操作需要特殊处理以保证缓存一致性。
 *
 * 返回值：
 * - true: 调用者需要自行清零页面
 * - false: 页面已经清零或需要使用特殊的清零函数
 *
 * 三个检查条件（任一为真就返回 true）：
 * 1. cpu_dcache_is_aliasing(): 数据缓存别名问题
 * 2. cpu_icache_is_aliasing(): 指令缓存别名问题
 * 3. !init_on_alloc: 内核配置不支持分配时自动清零
 *
 * user_alloc_needs_zeroing checks if a user folio from page allocator needs to
 * be zeroed or not.
 */
static inline bool user_alloc_needs_zeroing(void)
{
	/*
	 * 对于用户 folio，具有缓存别名的架构需要缓存刷新，
	 * arc 架构会修改 folio->flags 使 icache 与 dcache 保持一致，
	 * 因此始终返回 false，让调用者使用 clear_user_page()/clear_user_highpage()。
	 *
	 * 【缓存别名 (Cache Aliasing) 说明】
	 * - 在某些架构上，不同的虚拟地址可能映射到同一物理地址
	 * - 如果这些虚拟地址在缓存中有不同的缓存行，就会产生别名
	 * - 需要特殊的清零函数来处理缓存一致性
	 *
	 * 【CONFIG_INIT_ON_ALLOC_DEFAULT_ON】
	 * - 内核配置选项，决定分配时是否自动清零
	 * - static_branch: 静态分支优化，运行时开销几乎为零
	 * - !init_on_alloc: 如果未启用自动清零，返回 true
	 *
	 * for user folios, arch with cache aliasing requires cache flush and
	 * arc changes folio->flags to make icache coherent with dcache, so
	 * always return false to make caller use
	 * clear_user_page()/clear_user_highpage().
	 */
	return cpu_dcache_is_aliasing() || cpu_icache_is_aliasing() ||
	       !static_branch_maybe(CONFIG_INIT_ON_ALLOC_DEFAULT_ON,
				   &init_on_alloc);
}

/*
 * 影子栈 (Shadow Stack) 架构接口 - Architecture-specific Shadow Stack Interface
 *
 * 【影子栈概念说明】
 * 影子栈是一种硬件安全特性，用于防止返回地址被篡改（ROP 攻击）：
 * - 普通栈：存储局部变量、参数、返回地址等
 * - 影子栈：只存储返回地址的副本，由硬件管理
 * - 函数返回时：硬件比较两个栈中的返回地址
 * - 如果不匹配：触发异常（检测到攻击）
 *
 * 【支持的架构】
 * - x86-64: Intel CET (Control-flow Enforcement Technology)
 * - ARM64: PAC (Pointer Authentication Code) + BTI (Branch Target Identification)
 *
 * 【为什么需要这些接口？】
 * - 不同进程可能有不同的影子栈配置
 * - fork/exec 时需要继承或重置影子栈状态
 * - 调试器需要能够读取/修改影子栈状态
 * - 某些程序（如 JIT 编译器）可能需要禁用影子栈
 */

/**
 * arch_get_shadow_stack_status - 获取进程的影子栈状态
 * @t: 目标进程的 task_struct
 * @status: 用户空间指针，用于返回状态值
 *
 * 返回值：0 成功，负数表示错误码
 *
 * 使用场景：
 * - prctl(PR_GET_SHADOW_STACK_STATUS) 系统调用的实现
 * - 调试器查询进程的安全特性状态
 */
int arch_get_shadow_stack_status(struct task_struct *t, unsigned long __user *status);

/**
 * arch_set_shadow_stack_status - 设置进程的影子栈状态
 * @t: 目标进程的 task_struct
 * @status: 新的状态值（启用/禁用影子栈）
 *
 * 返回值：0 成功，负数表示错误码
 *
 * 注意事项：
 * - 通常只能在进程执行前设置（如 exec 时）
 * - 运行中的进程修改影子栈状态可能导致崩溃
 * - 需要特权或特定的安全策略允许
 *
 * 使用场景：
 * - prctl(PR_SET_SHADOW_STACK_STATUS) 系统调用的实现
 * - exec 时根据程序需求配置安全特性
 */
int arch_set_shadow_stack_status(struct task_struct *t, unsigned long status);

/**
 * arch_lock_shadow_stack_status - 锁定进程的影子栈状态
 * @t: 目标进程的 task_struct
 * @status: 要锁定的状态值
 *
 * 设计原因：
 * 一旦锁定，进程无法再修改影子栈状态，增强安全性。
 * 防止恶意代码在运行时禁用影子栈保护。
 *
 * 返回值：0 成功，负数表示错误码
 *
 * 注意事项：
 * - 锁定后无法解锁（除非进程重启）
 * - 通常在程序初始化完成后调用
 * - 是单向操作，谨慎使用
 *
 * 使用场景：
 * - 安全敏感程序在初始化后锁定保护特性
 * - 防止被注入的恶意代码禁用安全机制
 */
int arch_lock_shadow_stack_status(struct task_struct *t, unsigned long status);

/*
 * page_pool 的 DMA 映射 ID 管理 - DMA mapping IDs for page_pool
 *
 * 【page_pool 概念说明】
 * page_pool 是网络子系统用于高性能页面管理的机制：
 * - 页面回收和重用：避免频繁分配/释放
 * - DMA 映射缓存：避免重复的 DMA 映射操作
 * - 用于网络驱动的接收缓冲区
 *
 * 【问题：如何识别 page_pool 页面？】
 * page_pool 需要在 struct page 中标记哪些页面属于它管理。
 * 使用 page->pp_magic 字段（与 page->lru.next 共享同一内存位置）。
 *
 * 【设计挑战】
 * 非 page_pool 页面的 page->lru.next 可能包含任意内核指针。
 * 我们必须确保 pp_magic 的值不会与有效的内核指针混淆。
 *
 * 【解决方案：利用地址空间的特性】
 * 内核指针通常位于特定的地址范围（高地址区域）。
 * 通过巧妙选择 pp_magic 的值，使其不可能是有效的内核指针。
 *
 * When DMA-mapping a page, page_pool allocates an ID (from an xarray) and
 * stashes it in the upper bits of page->pp_magic. We always want to be able to
 * unambiguously identify page pool pages (using page_pool_page_is_pp()). Non-PP
 * pages can have arbitrary kernel pointers stored in the same field as pp_magic
 * (since it overlaps with page->lru.next), so we must ensure that we cannot
 * mistake a valid kernel pointer with any of the values we write into this
 * field.
 *
 * 【POISON_POINTER_DELTA 方案】
 * 在某些架构上，内核指针会加上一个偏移量（POISON_POINTER_DELTA）。
 * 这个偏移量确保了低地址范围不会被解释为有效指针。
 * 我们可以利用这个特性，在 PP_SIGNATURE 和 POISON_POINTER_DELTA 之间分配 DMA ID。
 *
 * On architectures that set POISON_POINTER_DELTA, this is already ensured,
 * since this value becomes part of PP_SIGNATURE; meaning we can just use the
 * space between the PP_SIGNATURE value (without POISON_POINTER_DELTA), and the
 * lowest bits of POISON_POINTER_DELTA. On arches where POISON_POINTER_DELTA is
 * 0, we use the lowest bit of PAGE_OFFSET as the boundary if that value is
 * known at compile-time.
 *
 * 【PAGE_OFFSET 方案】
 * 在 POISON_POINTER_DELTA 为 0 的架构上，使用 PAGE_OFFSET 作为边界。
 * PAGE_OFFSET 是内核空间的起始地址，低于它的地址不是有效的内核指针。
 *
 * 【回退方案】
 * 如果编译时无法确定 PAGE_OFFSET，或可用位数不足 8 位，
 * 则完全关闭 DMA 索引跟踪（设置为 0 位）。
 *
 * If the value of PAGE_OFFSET is not known at compile time, or if it is too
 * small to leave at least 8 bits available above PP_SIGNATURE, we define the
 * number of bits to be 0, which turns off the DMA index tracking altogether
 * (see page_pool_register_dma_index()).
 *
 * 【pp_magic 字段的位布局】
 * 低 2 位：特殊标志（复合页头、pfmemalloc）
 * 中间若干位：DMA 索引 ID（由这里的宏定义决定）
 * 剩余高位：PP_SIGNATURE（标识这是 page_pool 页面）
 */
/**
 * PP_DMA_INDEX_SHIFT - DMA 索引在 pp_magic 中的起始位位置
 *
 * 计算公式：1 + __fls(PP_SIGNATURE - POISON_POINTER_DELTA)
 *
 * C 语言知识：
 * - __fls(): Find Last Set，找到最高位的 1 的位置
 * - PP_SIGNATURE: page_pool 的签名值
 * - POISON_POINTER_DELTA: 指针毒化偏移量
 *
 * 设计原因：
 * 在 PP_SIGNATURE 之上留出空间存储 DMA 索引。
 * "+1" 确保不与 PP_SIGNATURE 的位重叠。
 */
#define PP_DMA_INDEX_SHIFT (1 + __fls(PP_SIGNATURE - POISON_POINTER_DELTA))

#if POISON_POINTER_DELTA > 0
/*
 * PP_SIGNATURE 包含 POISON_POINTER_DELTA，
 * 因此限制 DMA 索引的大小，避免与其重叠。
 *
 * PP_SIGNATURE includes POISON_POINTER_DELTA, so limit the size of the DMA
 * index to not overlap with that if set
 *
 * 【位数计算】
 * - __ffs(): Find First Set，找到最低位的 1 的位置
 * - MIN(32, ...): 最多使用 32 位（足够大的 DMA ID 空间）
 * - 结果：可用于 DMA 索引的位数
 */
#define PP_DMA_INDEX_BITS MIN(32, __ffs(POISON_POINTER_DELTA) - PP_DMA_INDEX_SHIFT)
#else
/*
 * 如果至少有 8 位可用，则使用 PAGE_OFFSET 的最低位；见上文说明
 * Use the lowest bit of PAGE_OFFSET if there's at least 8 bits available; see above
 *
 * 【PP_DMA_INDEX_MIN_OFFSET 说明】
 * 要求至少 8 位可用于 DMA 索引（2^8 = 256 个不同的 DMA 映射）。
 * 这是最小合理的 ID 空间。
 */
#define PP_DMA_INDEX_MIN_OFFSET (1 << (PP_DMA_INDEX_SHIFT + 8))

/**
 * PP_DMA_INDEX_BITS - DMA 索引的位数
 *
 * 复杂的条件编译表达式，检查三个条件：
 * 1. __builtin_constant_p(PAGE_OFFSET): PAGE_OFFSET 在编译时是否为常量
 * 2. PAGE_OFFSET >= PP_DMA_INDEX_MIN_OFFSET: 是否有足够的地址空间
 * 3. !(PAGE_OFFSET & (PP_DMA_INDEX_MIN_OFFSET - 1)): 是否正确对齐
 *
 * C 语言知识：
 * - __builtin_constant_p(): GCC 内建函数，检查值是否为编译时常量
 * - 三元运算符 ? : 根据条件选择不同的值
 * - 如果条件满足：计算可用位数（最多 32 位）
 * - 如果条件不满足：设为 0（禁用 DMA 索引跟踪）
 *
 * 对齐检查说明：
 * - PP_DMA_INDEX_MIN_OFFSET - 1 是低位全 1 的掩码
 * - PAGE_OFFSET & mask == 0 表示 PAGE_OFFSET 在所需边界上对齐
 * - ! 运算符取反，所以检查的是"是否对齐"
 */
#define PP_DMA_INDEX_BITS ((__builtin_constant_p(PAGE_OFFSET) && \
			    PAGE_OFFSET >= PP_DMA_INDEX_MIN_OFFSET && \
			    !(PAGE_OFFSET & (PP_DMA_INDEX_MIN_OFFSET - 1))) ? \
			      MIN(32, __ffs(PAGE_OFFSET) - PP_DMA_INDEX_SHIFT) : 0)

#endif

/**
 * PP_DMA_INDEX_MASK - DMA 索引的位掩码
 *
 * GENMASK(high, low): 生成从 low 位到 high 位全为 1 的掩码
 *
 * 例如：如果 PP_DMA_INDEX_SHIFT=8，PP_DMA_INDEX_BITS=8
 *       则 GENMASK(15, 8) = 0xFF00（第 8-15 位为 1）
 *
 * 用途：从 pp_magic 中提取 DMA 索引 ID
 */
#define PP_DMA_INDEX_MASK GENMASK(PP_DMA_INDEX_BITS + PP_DMA_INDEX_SHIFT - 1, \
				  PP_DMA_INDEX_SHIFT)

/*
 * 用于 page_pool_page_is_pp() 检查的掩码。
 * page->pp_magic 在分配后会与 PP_SIGNATURE 进行 OR 运算，以保留以下位：
 * - 位 0：复合页的头页标志
 * - 位 1：pfmemalloc 页面标志
 * - DMA 索引使用的位
 *
 * page_is_pfmemalloc() 在 __page_pool_put_page() 中检查，
 * 以避免回收 pfmemalloc 页面。
 *
 * 【pfmemalloc 说明】
 * pfmemalloc (page frame from emergency memory allocation) 是从紧急内存池分配的页面。
 * 这些页面用于关键路径（如网络栈处理数据包），即使系统内存不足也不能回收。
 *
 * Mask used for checking in page_pool_page_is_pp() below. page->pp_magic is
 * OR'ed with PP_SIGNATURE after the allocation in order to preserve bit 0 for
 * the head page of compound page and bit 1 for pfmemalloc page, as well as the
 * bits used for the DMA index. page_is_pfmemalloc() is checked in
 * __page_pool_put_page() to avoid recycling the pfmemalloc page.
 *
 * C 语言知识：
 * - ~: 按位取反运算符
 * - |: 按位或运算符
 * - 0x3UL: 二进制 0b11，表示最低两位（位 0 和位 1）
 * - PP_DMA_INDEX_MASK | 0x3UL: DMA 索引位 + 低 2 位
 * - ~(...): 取反后，这些位为 0，其他位为 1
 *
 * 用途：屏蔽掉 DMA 索引和低 2 位，只保留 PP_SIGNATURE 进行比较
 */
#define PP_MAGIC_MASK ~(PP_DMA_INDEX_MASK | 0x3UL)

/**
 * page_pool_page_is_pp - 检查页面是否属于 page_pool 管理
 * @page: 要检查的 struct page 指针
 *
 * 检测原理：
 * 用 PP_MAGIC_MASK 掩码屏蔽掉 DMA 索引位和低 2 位标志位，
 * 只保留 PP_SIGNATURE 所在的位，然后与 PP_SIGNATURE 比较。
 * 若相等，说明这是一个 page_pool 页面。
 *
 * 返回值：
 * - true:  页面属于 page_pool 管理
 * - false: 页面不属于 page_pool 管理
 *
 * 使用场景：
 * - 在页面释放路径中，决定是否归还给 page_pool 而非普通分配器
 * - 网络驱动的接收路径，识别可复用的 DMA 页面
 *
 * 注意事项：
 * - 仅在启用 CONFIG_PAGE_POOL 时有实际功能
 * - 未启用时始终返回 false（编译器会优化为常量）
 */
#ifdef CONFIG_PAGE_POOL
static inline bool page_pool_page_is_pp(const struct page *page)
{
	return (page->pp_magic & PP_MAGIC_MASK) == PP_SIGNATURE;
}
#else
static inline bool page_pool_page_is_pp(const struct page *page)
{
	return false;
}
#endif

/*
 * 页面快照标志位 - Page Snapshot Flags
 *
 * 用于 struct page_snapshot 的 flags 字段，描述快照的属性。
 *
 * 设计原因：
 * 页面快照记录页面状态时，需要标记该快照的可信度和特殊属性。
 * 使用位掩码允许同时设置多个标志。
 *
 * C 语言知识：
 * - (1 << 0): 二进制 001，第 0 位
 * - (1 << 1): 二进制 010，第 1 位
 * - (1 << 2): 二进制 100，第 2 位
 * - 多个标志可以用 | 组合：flags = FAITHFUL | PG_BUDDY
 */

/** PAGE_SNAPSHOT_FAITHFUL - 快照内容是可信的（页面在快照期间未被修改） */
#define PAGE_SNAPSHOT_FAITHFUL (1 << 0)

/** PAGE_SNAPSHOT_PG_BUDDY - 快照时页面处于 buddy 分配器中（空闲页） */
#define PAGE_SNAPSHOT_PG_BUDDY (1 << 1)

/** PAGE_SNAPSHOT_PG_IDLE - 快照时页面处于空闲状态 */
#define PAGE_SNAPSHOT_PG_IDLE  (1 << 2)

/**
 * struct page_snapshot - 页面状态快照
 *
 * 设计目的：
 * 在某一时刻捕获页面（包括其所属 folio）的完整状态。
 * 用于内存调试、诊断和性能分析工具。
 *
 * 为什么需要快照？
 * - 页面状态随时可能被其他 CPU 修改
 * - 直接读取可能得到不一致的状态
 * - 快照提供某一时刻的完整状态，便于分析
 *
 * 成员说明：
 * @folio_snapshot: folio 结构的快照副本（包含 folio 级别的状态）
 * @page_snapshot:  page 结构的快照副本（包含页面级别的状态）
 * @pfn:            页框号（Physical Frame Number），标识物理页的位置
 * @idx:            页面在 folio 中的索引（对于 folio 的某一页）
 * @flags:          快照标志，使用上面的 PAGE_SNAPSHOT_* 常量
 *
 * 使用场景：
 * - /proc/kpageflags 等内核调试接口
 * - 内存热插拔的状态诊断
 * - 内存错误（ECC）的事后分析
 */
struct page_snapshot {
	struct folio folio_snapshot;   /* folio 的状态副本 */
	struct page page_snapshot;     /* page 的状态副本 */
	unsigned long pfn;             /* 物理页框号 */
	unsigned long idx;             /* 页面在 folio 中的索引 */
	unsigned long flags;           /* PAGE_SNAPSHOT_* 标志组合 */
};

/**
 * snapshot_page_is_faithful - 检查页面快照是否可信
 * @ps: 指向 page_snapshot 结构的指针
 *
 * 返回值：
 * - true:  快照内容是可信的（PAGE_SNAPSHOT_FAITHFUL 标志已设置）
 * - false: 快照可能不一致（快照期间页面被修改）
 *
 * C 语言知识：
 * - ps->flags & PAGE_SNAPSHOT_FAITHFUL: 位与运算
 *   如果该位为 1，结果非零（在 C 中非零即为真）
 *   如果该位为 0，结果为零（即为假）
 *
 * 使用场景：
 * 调用者应先检查快照是否可信，再使用快照中的数据。
 * 不可信的快照仍可能有参考价值，但不应用于精确判断。
 */
static inline bool snapshot_page_is_faithful(const struct page_snapshot *ps)
{
	return ps->flags & PAGE_SNAPSHOT_FAITHFUL;
}

/**
 * snapshot_page - 捕获页面的当前状态快照
 * @ps:   输出参数，用于存储快照结果
 * @page: 要快照的页面
 *
 * 设计原因：
 * 原子地复制页面和 folio 的状态，确保快照的一致性。
 * 实现可能需要使用特殊的同步机制来防止被其他 CPU 的修改干扰。
 *
 * 注意事项：
 * - 快照操作本身不能防止并发修改
 * - 应检查返回的 flags 中的 PAGE_SNAPSHOT_FAITHFUL 确认一致性
 * - 对于调试目的，即使不可信的快照也有参考价值
 */
void snapshot_page(struct page_snapshot *ps, const struct page *page);

/**
 * map_anon_folio_pte_nopf - 将匿名 folio 映射到页表项（不触发页面错误）
 * @folio: 要映射的匿名 folio
 * @pte:   目标页表项指针
 * @vma:   所属的虚拟内存区域
 * @addr:  映射的虚拟地址
 * @uffd_wp: 是否设置 userfaultfd 写保护标志
 *
 * 函数名后缀说明：
 * - anon: 匿名页面（不对应文件，如堆、栈上的数据）
 * - nopf: no page fault（不触发缺页异常，直接建立映射）
 *
 * 设计原因：
 * 通常，建立页面映射是在缺页异常处理中完成的。
 * 但有些场景需要提前建立映射（如 UFFD 的预填充），
 * 这时需要不触发页面错误直接建立映射。
 *
 * uffd_wp 参数说明：
 * - userfaultfd 写保护：当用户空间修改该页时，内核通知 userfaultfd
 * - 用于实现用户空间的写时复制或脏页跟踪
 *
 * 注意事项：
 * - 调用前必须确保 folio 已经分配并可用
 * - 必须持有适当的页表锁
 * - 映射建立后，folio 的引用计数会增加
 */
void map_anon_folio_pte_nopf(struct folio *folio, pte_t *pte,
		struct vm_area_struct *vma, unsigned long addr,
		bool uffd_wp);

#endif /* _LINUX_MM_H */
