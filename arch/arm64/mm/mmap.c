// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 用户 VMA flags 到 PTE 属性的转换及 /dev/mem 物理范围校验。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * protection_map 用 4 个基础 VM 位索引常见权限，启动后再按 EPAN/LPA2
 * 能力修正；GCS、BTI、MTE、POE 等稀有扩展在表外叠加，避免指数膨胀。
 * 初始化后表只读，vm_get_page_prot() 可在 mmap/mprotect 并发路径无锁读取。
 */
/*
 * Based on arch/arm/mm/mmap.c
 *
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/types.h>

#include <asm/cpufeature.h>
#include <asm/page.h>

/* 16 项基础权限矩阵；私有 VM_WRITE 初始只读，写 fault 时由 COW 建可写 PTE。 */
static pgprot_t protection_map[16] __ro_after_init = {
	[VM_NONE]					= PAGE_NONE,
	[VM_READ]					= PAGE_READONLY,
	[VM_WRITE]					= PAGE_READONLY,
	[VM_WRITE | VM_READ]				= PAGE_READONLY,
	/* PAGE_EXECONLY if Enhanced PAN */
	/* 启动能力修正会把这组私有 execute-only 组合替换为 EPAN 专用属性。 */
	[VM_EXEC]					= PAGE_READONLY_EXEC,
	[VM_EXEC | VM_READ]				= PAGE_READONLY_EXEC,
	[VM_EXEC | VM_WRITE]				= PAGE_READONLY_EXEC,
	[VM_EXEC | VM_WRITE | VM_READ]			= PAGE_READONLY_EXEC,
	[VM_SHARED]					= PAGE_NONE,
	[VM_SHARED | VM_READ]				= PAGE_READONLY,
	[VM_SHARED | VM_WRITE]				= PAGE_SHARED,
	[VM_SHARED | VM_WRITE | VM_READ]		= PAGE_SHARED,
	/* PAGE_EXECONLY if Enhanced PAN */
	/* shared execute-only 组合也在 adjust_protection_map 中按 EPAN 能力收紧。 */
	[VM_SHARED | VM_EXEC]				= PAGE_READONLY_EXEC,
	[VM_SHARED | VM_EXEC | VM_READ]			= PAGE_READONLY_EXEC,
	[VM_SHARED | VM_EXEC | VM_WRITE]		= PAGE_SHARED_EXEC,
	[VM_SHARED | VM_EXEC | VM_WRITE | VM_READ]	= PAGE_SHARED_EXEC
};

/* GCS shadow stack 的架构只读属性，LPA2 初始化可能去掉 shareability 位。 */
static ptval_t gcs_page_prot __ro_after_init = _PAGE_GCS_RO;

/*
 * You really shouldn't be using read() or write() on /dev/mem.  This might go
 * away in the future.
 */
/*
 * 检查 /dev/mem read/write 的 [addr,addr+size) 是否完整落入可映射 RAM。
 * 返回 1/0，无资源副作用。必须同时是 memblock memory 且首地址非 NOMAP；
 * 相邻但属性不同区域可能保守拒绝，避免跨越固件/安全边界。
 */
int valid_phys_addr_range(phys_addr_t addr, size_t size)
{
	/*
	 * Check whether addr is covered by a memory region without the
	 * MEMBLOCK_NOMAP attribute, and whether that region covers the
	 * entire range. In theory, this could lead to false negatives
	 * if the range is covered by distinct but adjacent memory regions
	 * that only differ in other attributes. However, few of such
	 * attributes have been defined, and it is debatable whether it
	 * follows that /dev/mem read() calls should be able traverse
	 * such boundaries.
	 */
	/* false negative 是有意的兼容/安全取舍，不尝试拼接多个 memblock region。 */
	return memblock_is_region_memory(addr, size) &&
	       memblock_is_map_memory(addr);
}

/*
 * Do not allow /dev/mem mappings beyond the supported physical range.
 */
/* pfn 为页号、size 为字节；返回非零仅当末端不设置 PHYS_MASK 之外的位。 */
int valid_mmap_phys_addr_range(unsigned long pfn, size_t size)
{
	return !(((pfn << PAGE_SHIFT) + size) & ~PHYS_MASK);
}

/* 启动期按最终 CPU 能力修正权限表；返回 0 供 arch_initcall 使用。 */
static int __init adjust_protection_map(void)
{
	/*
	 * With Enhanced PAN we can honour the execute-only permissions as
	 * there is no PAN override with such mappings.
	 */
	/* EPAN 使 execute-only 不会被内核 PAN override 绕过，才可真实发布 X-only。 */
	if (cpus_have_cap(ARM64_HAS_EPAN)) {
		protection_map[VM_EXEC] = PAGE_EXECONLY;
		protection_map[VM_EXEC | VM_SHARED] = PAGE_EXECONLY;
	}

	if (lpa2_is_enabled()) {
		/* LPA2 页表格式不使用旧 PTE_SHARED 编码，逐项清除包括 GCS 特例。 */
		for (int i = 0; i < ARRAY_SIZE(protection_map); i++)
			pgprot_val(protection_map[i]) &= ~PTE_SHARED;
		gcs_page_prot &= ~PTE_SHARED;
	}

	return 0;
}
arch_initcall(adjust_protection_map);

/*
 * 把完整 vm_flags 转为 arm64 pgprot。输入来自已校验的 VMA flags；返回值
 * 编码基础 R/W/X/shared 加 GCS、BTI guard、MTE tagged memory 与 POE pkey。
 * 函数不安装 PTE、不 flush TLB。GCS 单独 fast path避免把 shadow-stack
 * 维度扩到整个基础查表矩阵。
 */
pgprot_t vm_get_page_prot(vm_flags_t vm_flags)
{
	ptval_t prot;

	/* Short circuit GCS to avoid bloating the table. */
	/* shadow stack 有专用写入机制，普通 CPU store 看到只读；PROT_NONE 仍优先。 */
	if (system_supports_gcs() && (vm_flags & VM_SHADOW_STACK)) {
		/* Honour mprotect(PROT_NONE) on shadow stack mappings */
		/* VM_ACCESS_FLAGS 全清时必须返回真正不可访问属性，不能让 GCS 写机制绕过。 */
		if (vm_flags & VM_ACCESS_FLAGS)
			prot = gcs_page_prot;
		else
			prot = pgprot_val(protection_map[VM_NONE]);
	} else {
		prot = pgprot_val(protection_map[vm_flags &
				   (VM_READ|VM_WRITE|VM_EXEC|VM_SHARED)]);
	}

	if (vm_flags & VM_ARM64_BTI)
		prot |= PTE_GP;

	/*
	 * There are two conditions required for returning a Normal Tagged
	 * memory type: (1) the user requested it via PROT_MTE passed to
	 * mmap() or mprotect() and (2) the corresponding vma supports MTE. We
	 * register (1) as VM_MTE in the vma->vm_flags and (2) as
	 * VM_MTE_ALLOWED. Note that the latter can only be set during the
	 * mmap() call since mprotect() does not accept MAP_* flags.
	 * Checking for VM_MTE only is sufficient since arch_validate_flags()
	 * does not permit (VM_MTE & !VM_MTE_ALLOWED).
	 */
	/* 两阶段 flag 校验已由 mmap 层完成，这里只把最终 VM_MTE 转成 AttrIndx。 */
	if (vm_flags & VM_MTE)
		prot |= PTE_ATTRINDX(MT_NORMAL_TAGGED);

#ifdef CONFIG_ARCH_HAS_PKEYS
	if (system_supports_poe()) {
		if (vm_flags & VM_PKEY_BIT0)
			prot |= PTE_PO_IDX_0;
		if (vm_flags & VM_PKEY_BIT1)
			prot |= PTE_PO_IDX_1;
		if (vm_flags & VM_PKEY_BIT2)
			prot |= PTE_PO_IDX_2;
	}
#endif

	return __pgprot(prot);
}
EXPORT_SYMBOL(vm_get_page_prot);
