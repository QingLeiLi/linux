// SPDX-License-Identifier: GPL-2.0
/*
 * EFI Memory Attributes Table 校验、保留与运行时页表权限收紧学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * EFI memory map 描述物理区类型、cache 属性和 runtime VA；可选的 companion
 * Memory Attributes Table 可把一个 runtime code/data 区再切成更小范围，为每段
 * 声明 RO/XP 以及新版 BTI 能力。efi_memattr_init() 在 memblock 阶段只映射表头，
 * 验证尺寸并保留整表物理内存；架构构造完 efi_mm 后再调用
 * efi_memattr_apply_permissions()，逐项匹配原 memory map、补出 VA，并通过架构
 * callback 修改页表。
 *
 * 两阶段设计避免早期尚无稳定页表时修改权限，也防止固件表在启动分配中被覆盖。
 * 严格的类型、对齐、包含关系和数量上限把损坏固件输入限制在可控范围；单项无效
 * 会被跳过，架构 callback 的首个真实错误则停止后续修改并返回调用者。
 */
/*
 * Copyright (C) 2016 Linaro Ltd. <ard.biesheuvel@linaro.org>
 */

#define pr_fmt(fmt)	"efi: memattr: " fmt

#include <linux/efi.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/memblock.h>

#include <asm/early_ioremap.h>

/* 验证后整张表的字节数，仅 __init apply 阶段使用；0 表示表不可用。 */
static int __initdata tbl_size;
/* EFI 配置表给出的物理地址；初始化后冻结，供 late memremap 使用。 */
unsigned long __ro_after_init efi_mem_attr_table = EFI_INVALID_TABLE_ADDR;

/*
 * Reserve the memory associated with the Memory Attributes configuration
 * table, if it exists.
 */
/*
 * 早期验证并 memblock_reserve Memory Attributes Table。
 *
 * 输入来自全局 efi_mem_attr_table/efi.memmap，不接受参数。表缺失时空操作；映射
 * 失败、未知版本、描述符大小异常或条目过多时打印诊断并保持功能禁用。成功后
 * tbl_size 记录精确长度、物理区被保留，并设置 EFI_MEM_ATTR flag。临时表头映射
 * 在所有出口统一撤销；函数不保留 tbl 指针。
 */
void __init efi_memattr_init(void)
{
	efi_memory_attributes_table_t *tbl;

	if (efi_mem_attr_table == EFI_INVALID_TABLE_ADDR)
		return;

	tbl = early_memremap(efi_mem_attr_table, sizeof(*tbl));
	if (!tbl) {
		pr_err("Failed to map EFI Memory Attributes table @ 0x%lx\n",
		       efi_mem_attr_table);
		return;
	}

	if (tbl->version > 2) {
		pr_warn("Unexpected EFI Memory Attributes table version %d\n",
			tbl->version);
		goto unmap;
	}

	/*
	 * The EFI memory attributes table descriptors might potentially be
	 * smaller than those used by the EFI memory map, as long as they can
	 * fit a efi_memory_desc_t. However, a larger descriptor size makes no
	 * sense, and might be an indication that the table is corrupted.
	 *
	 * The only exception is kexec_load(), where the EFI memory map is
	 * reconstructed by user space, and may use a smaller descriptor size
	 * than the original. Given that, ignoring this companion table is
	 * still the right thing to do here, but don't complain too loudly when
	 * this happens.
	 */
	/*
	 * companion descriptor 至少容纳标准结构且不应大于主 map stride。kexec 场景
	 * 可能由用户态重建出更小的主 stride，因此异常只告警并忽略 companion 表，
	 * 不阻断新内核启动。
	 */
	if (tbl->desc_size < sizeof(efi_memory_desc_t) ||
	    tbl->desc_size > efi.memmap.desc_size) {
		pr_warn("Unexpected EFI Memory Attributes descriptor size %u (expected: %lu)\n",
			tbl->desc_size, efi.memmap.desc_size);
		goto unmap;
	}

	/*
	 * Sanity check: the Memory Attributes Table contains multiple entries
	 * for each EFI runtime services code or data region in the EFI memory
	 * map, each with the permission attributes that may be applied when
	 * mapping the region.  There is no upper bound for the number of
	 * entries, as it could conceivably contain more entries than the EFI
	 * memory map itself. So pick an arbitrary limit of 64k, which is
	 * ludicrously high. This prevents a corrupted table from eating all
	 * system RAM.
	 */
	/* 64K 是防御性资源上限；随后长度计算因此不会被固件的任意计数放大到耗尽 RAM。 */
	if (tbl->num_entries > SZ_64K) {
		pr_warn(FW_BUG "Corrupted EFI Memory Attributes Table detected! (version == %u, desc_size == %u, num_entries == %u)\n",
			tbl->version, tbl->desc_size, tbl->num_entries);
		goto unmap;
	}

	/* 从此处开始发布：先算长度并保留 backing，最后置 flag 让后续阶段发现。 */
	tbl_size = sizeof(*tbl) + tbl->num_entries * tbl->desc_size;
	memblock_reserve(efi_mem_attr_table, tbl_size);
	set_bit(EFI_MEM_ATTR, &efi.flags);

unmap:
	early_memunmap(tbl, sizeof(*tbl));
}

/*
 * Returns a copy @out of the UEFI memory descriptor @in if it is covered
 * entirely by a UEFI memory map entry with matching attributes. The virtual
 * address of @out is set according to the matching entry that was found.
 */
/*
 * 校验 companion 表项并补全对应 runtime VA。
 *
 * in 是固件表内只读项，out 是调用者栈上的完整副本输出；函数先复制，所以失败
 * 时 out 仍可用于诊断打印。仅接受 RuntimeServicesCode/Data，必要时要求 OS page
 * 对齐，再扫描 EFI memory map，证明 [phys,phys+size) 完整落在同类型 runtime
 * 描述符中。成功按相对 PA 偏移计算 virt_addr 并返回 true；不取得任何引用。
 *
 * 原英文所说“matching attributes”容易误解：代码没有要求两表 attribute 位相等，
 * 因为 companion 表存在的目的正是提供更细的 RO/XP 等属性；实际匹配条件是
 * runtime 标志、物理包含关系和 memory type 一致。
 */
static bool entry_is_valid(const efi_memory_desc_t *in, efi_memory_desc_t *out)
{
	u64 in_paddr = in->phys_addr;
	u64 in_size = in->num_pages << EFI_PAGE_SHIFT;
	efi_memory_desc_t *md;

	*out = *in;

	/* 其他 EFI 类型不能作为 runtime code/data 权限子区，直接标为无效。 */
	if (in->type != EFI_RUNTIME_SERVICES_CODE &&
	    in->type != EFI_RUNTIME_SERVICES_DATA) {
		pr_warn("Entry type should be RuntimeServiceCode/Data\n");
		return false;
	}

	if (PAGE_SIZE > EFI_PAGE_SIZE &&
	    (!PAGE_ALIGNED(in->phys_addr) ||
	     !PAGE_ALIGNED(in->num_pages << EFI_PAGE_SHIFT))) {
		/*
		 * Since arm64 may execute with page sizes of up to 64 KB, the
		 * UEFI spec mandates that RuntimeServices memory regions must
		 * be 64 KB aligned. We need to validate this here since we will
		 * not be able to tighten permissions on such regions without
		 * affecting adjacent regions.
		 */
		/* OS page 大于 EFI 4K 时，未对齐子区会与邻区共享 PTE，无法独立收紧权限。 */
		pr_warn("Entry address region misaligned\n");
		return false;
	}

	for_each_efi_memory_desc(md) {
		u64 md_paddr = md->phys_addr;
		u64 md_size = md->num_pages << EFI_PAGE_SHIFT;

		/* companion 项只能归属主 map 中声明 EFI_MEMORY_RUNTIME 的区域。 */
		if (!(md->attribute & EFI_MEMORY_RUNTIME))
			continue;
		if (md->virt_addr == 0 && md->phys_addr != 0) {
			/* no virtual mapping has been installed by the stub */
			/* 非零 PA 配 VA 0 是“尚未安装 virtual map”的哨兵，后续项也不可可靠匹配。 */
			break;
		}

		if (md_paddr > in_paddr || (in_paddr - md_paddr) >= md_size)
			continue;

		/*
		 * This entry covers the start of @in, check whether
		 * it covers the end as well.
		 */
		/* 只覆盖起点不够；跨主描述符会让一个 callback 修改两种映射语义，必须拒绝。 */
		if (md_paddr + md_size < in_paddr + in_size) {
			pr_warn("Entry covers multiple EFI memory map regions\n");
			return false;
		}

		if (md->type != in->type) {
			pr_warn("Entry type deviates from EFI memory map region type\n");
			return false;
		}

		/* 保持在父描述符内的 PA 偏移，得到 companion 子区的 runtime VA。 */
		out->virt_addr = in_paddr + (md->virt_addr - md_paddr);

		return true;
	}

	pr_warn("No matching entry found in the EFI memory map\n");
	return false;
}

/*
 * To be called after the EFI page tables have been populated. If a memory
 * attributes table is available, its contents will be used to update the
 * mappings with tightened permissions as described by the table.
 * This requires the UEFI memory map to have already been populated with
 * virtual addresses.
 */
/*
 * 把已保留 companion 表应用到架构页表。
 *
 * mm 是已经填充完 runtime 映射的地址空间；fn 对每个验证成功的描述符收紧权限，
 * 第三个参数告知表是否声明 forward-edge control-flow guard/BTI。无表返回 0；
 * memremap 失败返回 -ENOMEM；无效固件项只记录并跳过；callback 首个非零错误终止
 * 循环并原样返回。映射用 MEMREMAP_WB 只读消费，结束时总是 memunmap。
 */
int __init efi_memattr_apply_permissions(struct mm_struct *mm,
					 efi_memattr_perm_setter fn)
{
	efi_memory_attributes_table_t *tbl;
	bool has_bti = false;
	int i, ret;

	if (tbl_size <= sizeof(*tbl))
		return 0;

	/*
	 * We need the EFI memory map to be setup so we can use it to
	 * lookup the virtual addresses of all entries in the  of EFI
	 * Memory Attributes table. If it isn't available, this
	 * function should not be called.
	 */
	/* 没有主 map 就无法把 companion PA 转成 runtime VA；WARN 暴露调用顺序错误。 */
	if (WARN_ON(!efi_enabled(EFI_MEMMAP)))
		return 0;

	tbl = memremap(efi_mem_attr_table, tbl_size, MEMREMAP_WB);
	if (!tbl) {
		pr_err("Failed to map EFI Memory Attributes table @ 0x%lx\n",
		       efi_mem_attr_table);
		return -ENOMEM;
	}

	if (tbl->version > 1 &&
	    (tbl->flags & EFI_MEMORY_ATTRIBUTES_FLAGS_RT_FORWARD_CONTROL_FLOW_GUARD))
		has_bti = true;

	if (efi_enabled(EFI_DBG))
		pr_info("Processing EFI Memory Attributes table:\n");

	for (i = ret = 0; ret == 0 && i < tbl->num_entries; i++) {
		efi_memory_desc_t md;
		unsigned long size;
		bool valid;
		char buf[64];

		/* desc_size 是固件 stride，不能假设条目按 sizeof(efi_memory_desc_t) 紧排。 */
		valid = entry_is_valid(efi_memdesc_ptr(tbl->entry, tbl->desc_size, i),
				       &md);
		size = md.num_pages << EFI_PAGE_SHIFT;
		if (efi_enabled(EFI_DBG) || !valid)
			pr_info("%s 0x%012llx-0x%012llx %s\n",
				valid ? "" : "!", md.phys_addr,
				md.phys_addr + size - 1,
				efi_md_typeattr_format(buf, sizeof(buf), &md));

		if (valid) {
			/* callback 只能进一步限制已存在映射；错误后停止，避免未知的部分状态扩大。 */
			ret = fn(mm, &md, has_bti);
			if (ret)
				pr_err("Error updating mappings, skipping subsequent md's\n");
		}
	}
	memunmap(tbl);
	return ret;
}
