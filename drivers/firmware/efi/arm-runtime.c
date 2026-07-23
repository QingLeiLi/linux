// SPDX-License-Identifier: GPL-2.0
/*
 * ARM/arm64 EFI Runtime Services 地址空间构造、切换与启用流程学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * EFI stub 给每个 EFI_MEMORY_RUNTIME 描述符填入固件虚拟地址；本文件在
 * early_initcall 阶段重新映射 EFI memory map，为全局 efi_mm 分配 PGD/ASID，
 * 逐描述符调用架构 efi_create_mapping()，再依据 Memory Attributes Table 收紧
 * 权限。全部成功后安装 native runtime function pointers 并发布 EFI flag。
 *
 * 运行期调用通过 efi_virtmap_load()->efi_set_pgd(&efi_mm) 临时切换专用页表，
 * 返回后恢复 current->active_mm。preempt_disable 保证切换期间任务不迁移；更高层
 * EFI runtime lock 串行固件调用。独立 mm 隔离固件 VA，代价是每次服务调用都有
 * 上下文切换成本，且初始化中途失败会整体禁用 native Runtime Services。
 */
/*
 * Extensible Firmware Interface
 *
 * Based on Extensible Firmware Interface Specification version 2.4
 *
 * Copyright (C) 2013, 2014 Linaro Ltd.
 */

#include <linux/dmi.h>
#include <linux/efi.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/mm_types.h>
#include <linux/pgalloc.h>
#include <linux/pgtable.h>
#include <linux/preempt.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <asm/cacheflush.h>
#include <asm/efi.h>
#include <asm/mmu.h>

#if defined(CONFIG_PTDUMP_DEBUGFS) || defined(CONFIG_ARM_PTDUMP_DEBUGFS)
#include <asm/ptdump.h>

/*
 * efi_mm 的页表诊断描述符。markers 限定 runtime VA 窗口并给输出标注边界；
 * 结构和复合数组均为静态生命周期，debugfs inode 只借用指针、不取得所有权。
 */
static struct ptdump_info efi_ptdump_info = {
	.mm		= &efi_mm,
	.markers	= (struct addr_marker[]){
		{ 0,				"UEFI runtime start" },
		{ EFI_RUNTIME_MAP_END,		"UEFI runtime end" },
		{ -1,				NULL }
	},
	.base_addr	= 0,
};

/* Runtime Services 真正启用后才暴露只读诊断文件，失败不影响启动。 */
static int __init ptdump_init(void)
{
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		ptdump_debugfs_register(&efi_ptdump_info, "efi_page_tables");

	return 0;
}
device_initcall(ptdump_init);

#endif

/*
 * 一次性构造 efi_mm。
 *
 * 先分配根 PGD、初始化 CPU mask 和架构 MM context，再遍历 EFI memory map 中
 * EFI_MEMORY_RUNTIME 项。virt_addr==U64_MAX 表示 stub 未提供可用虚拟映射；任一
 * 建表或权限收紧失败都返回 false，调用者不发布 Runtime Services。成功返回 true
 * 后 efi_mm 页表和 context 持续存活到关机，不走普通进程 mm 的销毁路径。
 */
static bool __init efi_virtmap_init(void)
{
	efi_memory_desc_t *md;

	/* efi_mm 是静态专用 mm；PGD 页和 ASID/context 在此进入可切换状态。 */
	efi_mm.pgd = pgd_alloc(&efi_mm);
	mm_init_cpumask(&efi_mm);
	init_new_context(NULL, &efi_mm);

	for_each_efi_memory_desc(md) {
		phys_addr_t phys = md->phys_addr;
		int ret;

		/* 非 runtime 区不应出现在固件运行时地址空间，减少暴露和页表占用。 */
		if (!(md->attribute & EFI_MEMORY_RUNTIME))
			continue;
		if (md->virt_addr == U64_MAX)
			return false;

		/* 每个架构后端负责 cache 类型、页表粒度和 non-global 属性。 */
		ret = efi_create_mapping(&efi_mm, md);
		if (ret) {
			pr_warn("  EFI remap %pa: failed to create mapping (%d)\n",
				&phys, ret);
			return false;
		}
	}

	/* 建表完成后才可安全 walk PTE，并按 companion table 追加 RO/XN/BTI。 */
	if (efi_memattr_apply_permissions(&efi_mm, efi_set_mapping_permissions))
		return false;

	return true;
}

/*
 * Enable the UEFI Runtime Services if all prerequisites are in place, i.e.,
 * non-early mapping of the UEFI system table and virtual mappings for all
 * EFI_MEMORY_RUNTIME regions.
 */
/*
 * native EFI Runtime Services 的事务式启用入口。
 *
 * 返回 0 表示“启动继续”，不一定代表 runtime 可用；只有虚拟页表构造失败返回
 * -ENOMEM。顺序不可交换：先把 early memmap 换成可长期使用的 late 映射并登记
 * soft-reserved 资源，再处理命令行禁用/半虚拟化分支，最后构造 efi_mm、安装
 * 函数指针并以 EFI_RUNTIME_SERVICES flag 发布。发布前任何退出都不会让普通
 * 调用者看到一套半初始化的 native runtime 接口。
 */
static int __init arm_enable_runtime_services(void)
{
	u64 mapsize;

	if (!efi_enabled(EFI_BOOT)) {
		pr_info("EFI services will not be available.\n");
		return 0;
	}

	/* arm_efi_init 的 early 映射不能跨页表重建；这里按保存 PA 建立长期 memmap。 */
	efi_memmap_unmap();

	mapsize = efi.memmap.desc_size * efi.memmap.nr_map;

	if (efi_memmap_init_late(efi.memmap.phys_map, mapsize)) {
		pr_err("Failed to remap EFI memory map\n");
		return 0;
	}

	if (efi_soft_reserve_enabled()) {
		efi_memory_desc_t *md;

		for_each_efi_memory_desc(md) {
			u64 md_size = md->num_pages << EFI_PAGE_SHIFT;
			struct resource *res;

			/* EFI_MEMORY_SP 区不能交普通 allocator，需在 iomem_resource 中占位。 */
			if (!(md->attribute & EFI_MEMORY_SP))
				continue;

			res = kzalloc_obj(*res);
			/* 资源描述分配失败只停止补登记；不回滚此前已插入的全局资源节点。 */
			if (WARN_ON(!res))
				break;

			res->start	= md->phys_addr;
			res->end	= md->phys_addr + md_size - 1;
			res->name	= "Soft Reserved";
			res->flags	= IORESOURCE_MEM;
			res->desc	= IORES_DESC_SOFT_RESERVED;

			insert_resource(&iomem_resource, res);
		}
	}

	if (efi_runtime_disabled()) {
		pr_info("EFI runtime services will be disabled.\n");
		return 0;
	}

	/* hypervisor/平台已提供 paravirt runtime 时，不再创建 native efi_mm。 */
	if (efi_enabled(EFI_RUNTIME_SERVICES)) {
		pr_info("EFI runtime services access via paravirt.\n");
		return 0;
	}

	pr_info("Remapping and enabling EFI services.\n");

	if (!efi_virtmap_init()) {
		pr_err("UEFI virtual mapping missing or invalid -- runtime services will not be available\n");
		return -ENOMEM;
	}

	/* Set up runtime services function pointers */
	/* 先写函数表，最后 set_bit 发布；读者以 flag 作为完整初始化的可见条件。 */
	efi_native_runtime_setup();
	set_bit(EFI_RUNTIME_SERVICES, &efi.flags);

	return 0;
}
early_initcall(arm_enable_runtime_services);

/*
 * 进入 EFI 专用地址空间。禁止抢占把当前 CPU、其 active ASID/TLB 状态和后续
 * unload 配对绑定在一起；调用者已持 EFI runtime 串行锁。无返回值，返回时
 * 当前 TTBR/PGD 指向 efi_mm，但 preempt count 仍增加一层。
 */
void efi_virtmap_load(void)
{
	preempt_disable();
	efi_set_pgd(&efi_mm);
}

/* 恢复当前任务借用/拥有的 active_mm，随后才允许抢占；必须与 load 同 CPU 配对。 */
void efi_virtmap_unload(void)
{
	efi_set_pgd(current->active_mm);
	preempt_enable();
}


/* DMI 依赖 UEFI 系统表，core_initcall 保证早于 dmi_id_init 的 arch_initcall。 */
static int __init arm_dmi_init(void)
{
	/*
	 * On arm64/ARM, DMI depends on UEFI, and dmi_setup() needs to
	 * be called early because dmi_id_init(), which is an arch_initcall
	 * itself, depends on dmi_scan_machine() having been called already.
	 */
	/* dmi_setup() 发现并扫描 SMBIOS/DMI 表；失败由 DMI 子系统内部降级处理。 */
	dmi_setup();
	return 0;
}
core_initcall(arm_dmi_init);
