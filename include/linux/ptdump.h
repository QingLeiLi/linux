/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_PTDUMP_H
#define _LINUX_PTDUMP_H

#include <linux/mm_types.h>

/**
 * struct ptdump_range - 页表转储的地址范围
 * Page table dump address range
 *
 * 设计目的 (Design Purpose):
 * 定义需要遍历和转储的虚拟地址空间范围。内核的虚拟地址空间很大，
 * 通过定义范围可以只转储感兴趣的区域，提高效率并减少输出。
 * Defines the virtual address space range that needs to be traversed and dumped.
 * Since kernel virtual address space is huge, defining ranges allows dumping only
 * regions of interest, improving efficiency and reducing output.
 *
 * 使用场景 (Use Cases):
 * - 调试特定内存区域的页表映射
 * - 检查内核代码段、数据段的保护属性
 * - 验证内存隔离和权限设置
 */
struct ptdump_range {
	unsigned long start;  /* 起始虚拟地址 (包含) - Start virtual address (inclusive) */
	unsigned long end;    /* 结束虚拟地址 (不包含) - End virtual address (exclusive) */
};

/**
 * struct ptdump_state - 页表转储的状态和回调函数集合
 * Page table dump state and callback function collection
 *
 * 设计思想 (Design Philosophy):
 * 采用回调函数机制实现灵活的页表遍历。调用者通过提供回调函数来定义
 * 如何处理每一级页表项，这种设计将遍历逻辑与处理逻辑解耦。
 * Uses callback mechanism for flexible page table traversal. Callers define
 * how to handle each level of page table entries by providing callbacks,
 * decoupling traversal logic from processing logic.
 *
 * 为什么有多级回调 (Why Multiple Level Callbacks):
 * Linux使用多级页表结构(5级: PGD->P4D->PUD->PMD->PTE)，每一级都可能有
 * 不同的映射属性。提供每一级的回调让调用者能够：
 * 1. 检测大页映射(huge pages)出现在哪一级
 * 2. 追踪每一级的保护属性变化
 * 3. 识别页表层次结构中的异常
 * Linux uses multi-level page table structure (5 levels: PGD->P4D->PUD->PMD->PTE).
 * Each level may have different mapping attributes. Providing callbacks for each
 * level allows callers to:
 * 1. Detect at which level huge page mappings appear
 * 2. Track protection attribute changes at each level
 * 3. Identify anomalies in the page table hierarchy
 *
 * 注意事项 (Important Notes):
 * - 所有回调函数指针可以为NULL，表示不关心该级别
 * - 回调函数在持有页表锁的情况下被调用，不应执行耗时操作
 * - effective_prot_* 回调用于计算有效权限(上级权限的累积效果)
 */
struct ptdump_state {
	/**
	 * note_page_pte - 页表最底层(PTE级别)的回调
	 * Callback for the lowest level of page table (PTE level)
	 * @st: 状态对象自身 - The state object itself
	 * @addr: 此PTE对应的虚拟地址 - Virtual address this PTE maps
	 * @pte: 页表项内容 - Page table entry content
	 *
	 * 处理4KB页面映射，这是最细粒度的映射级别
	 * Handles 4KB page mappings, the finest granularity mapping level
	 */
	void (*note_page_pte)(struct ptdump_state *st, unsigned long addr, pte_t pte);

	/**
	 * note_page_pmd - 页中间目录级别的回调
	 * Callback for Page Middle Directory level
	 * @st: 状态对象 - State object
	 * @addr: 虚拟地址 - Virtual address
	 * @pmd: PMD项内容 - PMD entry content
	 *
	 * 可能是指向下级页表的指针，也可能是2MB大页映射(x86_64)
	 * Could be a pointer to next level table, or a 2MB huge page mapping (x86_64)
	 */
	void (*note_page_pmd)(struct ptdump_state *st, unsigned long addr, pmd_t pmd);

	/**
	 * note_page_pud - 页上层目录级别的回调
	 * Callback for Page Upper Directory level
	 * @st: 状态对象 - State object
	 * @addr: 虚拟地址 - Virtual address
	 * @pud: PUD项内容 - PUD entry content
	 *
	 * 可能是指向PMD的指针，也可能是1GB大页映射(x86_64)
	 * Could be a pointer to PMD, or a 1GB huge page mapping (x86_64)
	 */
	void (*note_page_pud)(struct ptdump_state *st, unsigned long addr, pud_t pud);

	/**
	 * note_page_p4d - 第4级页目录的回调(5级页表中使用)
	 * Callback for 4th level page directory (used in 5-level paging)
	 * @st: 状态对象 - State object
	 * @addr: 虚拟地址 - Virtual address
	 * @p4d: P4D项内容 - P4D entry content
	 *
	 * 仅在支持5级页表的架构上有意义(如x86_64启用LA57)
	 * Only meaningful on architectures with 5-level paging (e.g., x86_64 with LA57)
	 */
	void (*note_page_p4d)(struct ptdump_state *st, unsigned long addr, p4d_t p4d);

	/**
	 * note_page_pgd - 页全局目录(顶层)的回调
	 * Callback for Page Global Directory (top level)
	 * @st: 状态对象 - State object
	 * @addr: 虚拟地址 - Virtual address
	 * @pgd: PGD项内容 - PGD entry content
	 *
	 * 页表层次结构的最顶层
	 * The topmost level of the page table hierarchy
	 */
	void (*note_page_pgd)(struct ptdump_state *st, unsigned long addr, pgd_t pgd);

	/**
	 * note_page_flush - 完成一个连续区域转储后的回调
	 * Callback after completing a contiguous region dump
	 * @st: 状态对象 - State object
	 *
	 * 用于刷新缓冲的输出或进行区域统计
	 * Used to flush buffered output or perform region statistics
	 */
	void (*note_page_flush)(struct ptdump_state *st);

	/**
	 * effective_prot_pte - 计算PTE级别的有效保护属性
	 * Calculate effective protection attributes at PTE level
	 * @st: 状态对象 - State object
	 * @pte: PTE项 - PTE entry
	 *
	 * 有效权限是所有上层权限位的AND结果(最严格的限制生效)
	 * Effective permissions are the AND of all upper level permission bits
	 * (most restrictive constraint takes effect)
	 */
	void (*effective_prot_pte)(struct ptdump_state *st, pte_t pte);

	/**
	 * effective_prot_pmd - 计算PMD级别的有效保护属性
	 * Calculate effective protection attributes at PMD level
	 */
	void (*effective_prot_pmd)(struct ptdump_state *st, pmd_t pmd);

	/**
	 * effective_prot_pud - 计算PUD级别的有效保护属性
	 * Calculate effective protection attributes at PUD level
	 */
	void (*effective_prot_pud)(struct ptdump_state *st, pud_t pud);

	/**
	 * effective_prot_p4d - 计算P4D级别的有效保护属性
	 * Calculate effective protection attributes at P4D level
	 */
	void (*effective_prot_p4d)(struct ptdump_state *st, p4d_t p4d);

	/**
	 * effective_prot_pgd - 计算PGD级别的有效保护属性
	 * Calculate effective protection attributes at PGD level
	 */
	void (*effective_prot_pgd)(struct ptdump_state *st, pgd_t pgd);

	/**
	 * range - 要转储的地址范围数组(以NULL终止)
	 * Array of address ranges to dump (NULL-terminated)
	 *
	 * 如果为NULL，则转储整个地址空间
	 * If NULL, dump the entire address space
	 */
	const struct ptdump_range *range;
};

/**
 * ptdump_walk_pgd_level_core - 核心页表遍历函数(带W^X检查)
 * Core page table walking function (with W^X checking)
 *
 * @m: seq_file用于格式化输出，如果为NULL则输出到dmesg
 *     seq_file for formatted output, if NULL output to dmesg
 * @mm: 要遍历的内存描述符，NULL表示使用内核页表
 *     Memory descriptor to walk, NULL means use kernel page tables
 * @pgd: 页全局目录的起始地址
 *     Start address of Page Global Directory
 * @checkwx: 是否检查W^X违规(可写且可执行的页面)
 *     Whether to check for W^X violations (writable and executable pages)
 * @dmesg: 是否将输出发送到内核日志而非seq_file
 *     Whether to send output to kernel log instead of seq_file
 *
 * 返回值 (Return):
 * true - 如果checkwx=true且发现W^X违规
 *        If checkwx=true and W^X violations were found
 * false - 没有发现违规或未启用检查
 *         No violations found or checking not enabled
 *
 * W^X原则 (W^X Principle):
 * "Write XOR Execute" - 内存页应该要么可写，要么可执行，但不能同时具备
 * 这两种权限。这是重要的安全原则，防止代码注入攻击。
 * "Write XOR Execute" - Memory pages should be either writable or executable,
 * but not both. This is an important security principle to prevent code
 * injection attacks.
 *
 * 使用场景 (Use Cases):
 * - 启动时验证内核内存布局的安全性
 * - 调试页表配置问题
 * - 通过/sys/kernel/debug导出页表信息
 */
bool ptdump_walk_pgd_level_core(struct seq_file *m,
				struct mm_struct *mm, pgd_t *pgd,
				bool checkwx, bool dmesg);

/**
 * ptdump_walk_pgd - 使用自定义状态遍历页表
 * Walk page tables with custom state
 *
 * @st: 包含回调函数的状态对象
 *      State object containing callback functions
 * @mm: 内存描述符，NULL表示内核页表
 *      Memory descriptor, NULL for kernel page tables
 * @pgd: 页全局目录起始地址
 *      Page Global Directory start address
 *
 * 这是更灵活的接口，调用者通过ptdump_state提供自定义的处理逻辑
 * This is a more flexible interface where callers provide custom handling
 * logic through ptdump_state
 *
 * 注意事项 (Notes):
 * - 调用者必须初始化st中的回调函数
 * - 在多核系统上，页表可能在遍历过程中被修改
 * - Caller must initialize callback functions in st
 * - On multicore systems, page tables may be modified during traversal
 */
void ptdump_walk_pgd(struct ptdump_state *st, struct mm_struct *mm, pgd_t *pgd);

/**
 * ptdump_check_wx - 检查内核页表中的W^X违规
 * Check for W^X violations in kernel page tables
 *
 * 返回值 (Return):
 * true - 发现可写且可执行的页面(安全违规)
 *        Found writable and executable pages (security violation)
 * false - 未发现违规，内核内存布局符合W^X原则
 *         No violations found, kernel memory layout follows W^X principle
 *
 * 设计目的 (Design Purpose):
 * 在系统启动或调试时验证内核代码段只读、数据段不可执行等安全属性
 * Validates security properties like read-only kernel code and non-executable
 * data segments during system boot or debugging
 *
 * 典型调用时机 (Typical Call Timing):
 * - mark_rodata_ro()之后，验证只读数据保护已生效
 * - 加载内核模块后，检查模块内存布局
 * - After mark_rodata_ro(), verify read-only data protection is effective
 * - After loading kernel modules, check module memory layout
 */
bool ptdump_check_wx(void);

/**
 * debug_checkwx - 如果配置了CONFIG_DEBUG_WX则执行W^X检查
 * Perform W^X check if CONFIG_DEBUG_WX is configured
 *
 * 这是一个条件编译的内联函数，在编译时决定是否执行检查
 * This is a conditionally compiled inline function that decides at compile
 * time whether to perform the check
 *
 * 为什么设计为static inline (Why static inline):
 * 1. 避免函数调用开销(虽然可能被优化掉)
 * 2. 允许编译器在未启用DEBUG_WX时完全移除此代码
 * 3. 头文件中定义，每个编译单元有自己的副本
 * 1. Avoids function call overhead (though may be optimized out anyway)
 * 2. Allows compiler to completely remove this code when DEBUG_WX is disabled
 * 3. Defined in header, each compilation unit gets its own copy
 *
 * CONFIG_DEBUG_WX说明:
 * 内核配置选项，启用后会在启动时自动检查W^X违规并报告
 * Kernel config option that auto-checks for W^X violations at boot and reports them
 */
static inline void debug_checkwx(void)
{
	if (IS_ENABLED(CONFIG_DEBUG_WX))
		ptdump_check_wx();
}

#endif /* _LINUX_PTDUMP_H */
