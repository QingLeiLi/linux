// SPDX-License-Identifier: GPL-2.0
/*
 * ARM/ARM64 与 RISC-V 通用 EFI 早期初始化学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 【文件职责】
 * EFI stub 在 ExitBootServices() 交接过程中，把 EFI System Table 地址和
 * 最终 Memory Map 的物理地址、长度、descriptor 大小/版本写入 FDT。体系
 * 结构的 setup_arch() 随后调用本文件的 efi_init()，把这份“固件启动世界
 * 的快照”转换成内核早期启动可以消费的三类状态：
 *
 *   EFI stub / Xen
 *       │  FDT /chosen 或 /hypervisor/uefi
 *       ▼
 *   efi_init()
 *       ├── efi_memmap_init_early()：临时映射 EFI Memory Map
 *       ├── uefi_init()：校验 System Table，解析 GUID 配置表
 *       ├── reserve_regions()：以 EFI Memory Map 重建 memblock.memory
 *       └── ESRT、MOK、镜像内存与主显示信息的早期登记
 *       ▼
 *   paging_init()/体系结构 EFI runtime 初始化/普通 memblock 分配
 *
 * 本文件不负责调用 EFI Runtime Services，也不建立其最终虚拟地址映射；
 * arm-runtime.c、riscv-runtime.c 等后续代码会使用这里保存的 efi.runtime
 * 和 efi.memmap 完成该工作。x86 和 LoongArch 有各自的 efi_init()，不走
 * 本文件。
 *
 * 【核心对象与生命周期】
 * - struct efi_memory_map_data：栈上的 FDT 交接参数，仅在 efi_init() 中有效。
 * - efi.memmap：early_memremap() 建立的全局早期映射；失败路径立即撤销，
 *   成功路径保留给紧随其后的消费者，再由体系结构代码撤销或换成持久映射。
 * - efi 全局对象：保存 system/config table 中筛出的物理地址、runtime 入口
 *   和能力位；这些地址只有经相应映射 helper 后才能解引用。
 * - memblock.memory/reserved：reserve_regions() 根据 EFI map 重建的物理内存
 *   可用性模型，随后成为早期分配、页表建立和最终伙伴系统初始化的依据。
 * - 本文件的函数和多数状态带 __init/__initdata，表示只服务启动阶段；init
 *   完成后相应代码/数据 section 可回收，运行期不得继续保存指向它们的引用。
 *
 * 【并发与方案权衡】
 * 全流程发生在启动 CPU 的 setup_arch() 早期阶段，其他 CPU 尚未上线，
 * 常规分配器和完整虚拟内存尚未建立；因此没有锁/RCU 竞争，也不能依赖普通
 * kmalloc/vmalloc。代码使用容量有限的 early_memremap 与 memblock。选择以
 * ExitBootServices() 时的 EFI map 取代 DT memory nodes，可获得与固件最终
 * 内存类型一致的视图；代价是缺失或无法映射该表时已没有可靠 RAM 描述，
 * 启动只能 panic，而不能退回一份可能过期的 DT 内存图。
 */
/*
 * Extensible Firmware Interface
 *
 * Based on Extensible Firmware Interface Specification version 2.4
 *
 * Copyright (C) 2013 - 2015 Linaro Ltd.
 */

/* 让本文件所有 pr_*() 日志统一带 "efi: " 前缀，不改变日志级别或控制流。 */
#define pr_fmt(fmt)	"efi: " fmt

#include <linux/efi.h>
#include <linux/fwnode.h>
#include <linux/init.h>
#include <linux/kexec_handover.h>
#include <linux/memblock.h>
#include <linux/mm_types.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/platform_device.h>
#include <linux/sysfb.h>

#include <asm/efi.h>

/*
 * EFI 配置表中 LINUX_EFI_PRIMARY_DISPLAY_TABLE_GUID 对应表的物理地址。
 *
 * 初值 EFI_INVALID_TABLE_ADDR 表示配置表尚未发现；efi_config_parse_tables()
 * 匹配 GUID 后写入，init_primary_display() 读取。变量位于 __initdata，启动
 * 完成后其存储可回收；它只保存数值地址，不持有映射或引用。EFI stub 采用
 * 配置表方式传递显示信息时才会产生有效值。
 */
unsigned long __initdata primary_display_table = EFI_INVALID_TABLE_ADDR;

/*
 * 判断 EFI descriptor 是否描述可纳入“内存候选集”的介质。
 *
 * 调用位置：reserve_regions() 遍历已经映射的 efi.memmap 时调用。
 * @md：当前 EFI descriptor 的借用指针，不能为空；只在遍历本次迭代内有效，
 *      本函数不修改 descriptor，也不取得所有权。
 *
 * EFI 类型本身不足以判断区域能否作为 CPU 内存访问；这里以 WB/WT/WC 任一
 * cacheability 属性为第一层筛选。返回 1 表示交给后续 is_usable_memory()
 * 和 soft-reserve 规则继续分类，返回 0 表示不加入 memblock.memory。
 * 函数处于单 CPU 早期启动上下文，不持锁、不睡眠、无副作用。
 */
static int __init is_memory(efi_memory_desc_t *md)
{
	if (md->attribute & (EFI_MEMORY_WB|EFI_MEMORY_WT|EFI_MEMORY_WC))
		return 1;
	return 0;
}

/*
 * Translate a EFI virtual address into a physical address: this is necessary,
 * as some data members of the EFI system table are virtually remapped after
 * SetVirtualAddressMap() has been called.
 */
/*
 * 将 EFI System Table 字段中的地址恢复为当前内核可映射的物理地址。
 *
 * 某些 kexec/固件交接场景下，System Table 内的 fw_vendor、tables 等成员
 * 已经在早先的 SetVirtualAddressMap() 调用后改写成 EFI runtime VA；但此处
 * early_memremap*() 需要物理地址。函数遍历 efi.memmap 的 RUNTIME 区域，
 * 用 descriptor 记录的 virt_addr 区间反算 phys_addr。
 *
 * @addr：待转换的 EFI 地址，按字节计；是纯输入数值，不代表当前内核可直接
 *        解引用的指针。返回匹配区间中的物理字节地址；若尚未安装虚拟映射或
 *        没有匹配 descriptor，则原样返回 addr，兼容普通首次启动时表成员
 *        本来就是物理地址的情形。
 *
 * efi.memmap 必须已由 efi_memmap_init_early() 成功安装。函数仅借用 descriptor，
 * 不创建映射、不持锁、不睡眠。区间判断先做 addr - virt_addr，避免直接计算
 * 末地址时溢出；页数单位经 EFI_PAGE_SHIFT 转为字节。
 */
static phys_addr_t __init efi_to_phys(unsigned long addr)
{
	/*
	 * md 是当前 EFI runtime descriptor 的借用游标；退出单次循环后不可保存。
	 * for_each_efi_memory_desc() 按固件给出的 desc_size 前进，不能用 sizeof(*md)，
	 * 因为 UEFI 允许未来版本扩展每个 descriptor。
	 */
	efi_memory_desc_t *md;

	for_each_efi_memory_desc(md) {
		/* 非 runtime 区域不会参与 SetVirtualAddressMap()，无需做 VA 反查。 */
		if (!(md->attribute & EFI_MEMORY_RUNTIME))
			continue;
		if (md->virt_addr == 0)
			/* no virtual mapping has been installed by the stub */
			/*
			 * virt_addr 为 0 表示 stub 没有安装 virtual map；此时 System
			 * Table 成员仍按物理地址解释，直接结束扫描而非制造伪转换。
			 */
			break;
		/*
		 * 命中 [virt_addr, virt_addr + num_pages * EFI_PAGE_SIZE) 后保留
		 * 区间内偏移，实现同一 runtime descriptor 内的 VA->PA 平移。
		 */
		if (md->virt_addr <= addr &&
		    (addr - md->virt_addr) < (md->num_pages << EFI_PAGE_SHIFT))
			return md->phys_addr + addr - md->virt_addr;
	}
	return addr;
}

/*
 * 体系结构可追加的 EFI GUID->地址槽映射表。
 *
 * 当前文件只借用该只读表并传给 efi_config_parse_tables()；ARM32 提供 CPU
 * entry-state table 等扩展。若体系结构没有提供强定义，未定义 weak 符号在
 * 链接后表现为 NULL，解析器只匹配 common tables。每个匹配项的目标槽通常是
 * __initdata，解析器写入配置表物理地址。
 */
extern __weak const efi_config_table_type_t efi_arch_tables[];

/*
 * x86 defines its own instance of sysfb_primary_display and uses
 * it even without EFI, everything else can get them from here.
 */
/*
 * x86 即使非 EFI 启动也从 boot_params 填充自己的全局实例，因此不能在这里
 * 重复定义。编入本文件的 ARM/ARM64/RISC-V 仅在至少一个消费者存在时提供该
 * 对象：SYSFB 创建平台 framebuffer，EFI_EARLYCON 早期输出，FIRMWARE_EDID
 * 保存显示器 EDID。对象显式放在 .data 而非会由内核入口清零的 .bss，使
 * EFI stub 直接写入内核全局符号的交接方式可以跨越进入内核后的 BSS 清零；
 * 导出后，后续显示/PCI 辅助代码只借用该全局对象。早期阶段单写，设备初始
 * 化以后主要读取，无需此处加锁。
 */
#if !defined(CONFIG_X86) && (defined(CONFIG_SYSFB) || defined(CONFIG_EFI_EARLYCON) || defined(CONFIG_FIRMWARE_EDID))
struct sysfb_display_info sysfb_primary_display __section(".data");
EXPORT_SYMBOL_GPL(sysfb_primary_display);
#endif

/*
 * 接收 EFI stub 通过 Linux primary-display 配置表传来的显示快照。
 *
 * 调用位置：efi_init() 完成配置表解析和 memblock 重建后，在 x86/SYSFB/
 * EFI_EARLYCON 相关配置下调用。入参、输出参数和直接返回值均无。
 *
 * 隐式输入 primary_display_table 是配置表物理地址；成功时把表内容复制到
 * sysfb_primary_display，清零 stub 分配的源表，必要时把线性 framebuffer
 * 从普通直接映射中标为 NOMAP，并通知 EFI earlycon 重新探测。源表仅被临时
 * 映射，函数不保留 dpy 指针；全局副作用供 sysfb、earlycon、EDID 和 PCI
 * framebuffer 归属判断消费。
 *
 * 表不存在时无操作；映射失败时仅记录错误并保留现状，启动仍可继续。函数
 * 运行于单 CPU 早期上下文，不持锁；early_memremap/memblock 操作不依赖普通
 * 分配器，不在这里形成可与其他 CPU 竞争的发布。
 */
static void __init init_primary_display(void)
{
	/*
	 * dpy 是配置表物理页的短期可写映射；只在 if 块中有效。复制完成后内核
	 * 使用独立全局快照，撤销映射不会使 sysfb_primary_display 失效。
	 */
	struct sysfb_display_info *dpy;

	if (primary_display_table != EFI_INVALID_TABLE_ADDR) {
		/* 映射完整结构；失败时尚未改变目标全局对象，也无需回滚资源。 */
		dpy = early_memremap(primary_display_table, sizeof(*dpy));
		if (!dpy) {
			pr_err("Could not map primary_display config table\n");
			return;
		}
		/*
		 * 先复制再清零明确完成一次性消费：有效 screen/EDID 已进入内核全局
		 * 快照，EFI_ACPI_RECLAIM_MEMORY 中的交接缓冲不再宣称仍含有效内容。
		 * 此处不释放源表，后续仍由固件内存类型/回收阶段管理其物理存储。
		 */
		sysfb_primary_display = *dpy;
		memset(dpy, 0, sizeof(*dpy));
		early_memunmap(dpy, sizeof(*dpy));

		/*
		 * 若 framebuffer 地址此前被当作普通 RAM 加入 memblock.memory，
		 * 标记 NOMAP 可阻止它进入常规线性映射/页分配用途，避免 CPU RAM
		 * 别名与显示设备正在扫描的显存相互破坏；本来就不是 map memory
		 * 时无需制造一个新的 memblock 区间。
		 */
		if (memblock_is_map_memory(sysfb_primary_display.screen.lfb_base))
			memblock_mark_nomap(sysfb_primary_display.screen.lfb_base,
					    sysfb_primary_display.screen.lfb_size);

		/*
		 * primary-display 数据现在才完整可见；earlycon 若编入，重新探测可从
		 * 新 screen_info 建立 efifb 控制台。IS_ENABLED 保持单一控制流，
		 * 关闭配置时编译器会消去调用。
		 */
		if (IS_ENABLED(CONFIG_EFI_EARLYCON))
			efi_earlycon_reprobe();
	}
}

/*
 * 校验并导入 EFI System Table 与 Configuration Table。
 *
 * 宏观位置：
 *   efi_init() -> uefi_init()
 *                 ├── efi_systab_check_header()
 *                 ├── efi_systab_report_header()
 *                 └── efi_config_parse_tables()
 *
 * @efi_system_table：EFI System Table 的物理字节地址，来自 FDT；非零且不转移
 *                    所有权。函数只建立短期只读映射，不保留 systab 指针。
 *
 * 入口要求 efi.memmap 已有效，因为 efi_to_phys() 可能需要借助 runtime
 * descriptor 翻译已虚拟化的表成员。单 CPU 早期上下文，无锁；使用 early
 * mapping，不依赖普通内存分配器。
 *
 * 返回 0 表示表头有效且配置表数组已完成解析；返回 -ENOMEM 表示 system/config
 * table 无法映射；其他负 errno 来自表头校验或配置表解析。所有成功建立的短期
 * 映射在返回前撤销。可观察副作用包括设置 EFI_BOOT/EFI_64BIT，保存 runtime
 * 服务地址和版本，以及把已识别 GUID 的表地址写入 efi/common/arch 槽；解析
 * helper 还可能导入随机种子、保留 firmware 表内存。失败不会统一回滚这些已
 * 发生的全局标志/槽写入，调用者以返回值决定停止后续 EFI 初始化。
 */
static int __init uefi_init(u64 efi_system_table)
{
	/*
	 * 变量地图：
	 *   systab        System Table 的临时只读映射，成功映射后必须 unmap。
	 *   config_tables 配置表数组的临时只读映射，仅在解析调用期间有效。
	 *   table_size    配置表数组总字节数，用于成对映射/撤销映射。
	 *   retval        精确向 efi_init() 传播的初始化结果。
	 */
	efi_config_table_t *config_tables;
	efi_system_table_t *systab;
	size_t table_size;
	int retval;

	/* 阶段 1：只映射固定头部；地址无效时尚未发布任何表内容。 */
	systab = early_memremap_ro(efi_system_table, sizeof(efi_system_table_t));
	if (systab == NULL) {
		pr_warn("Unable to map EFI system table.\n");
		return -ENOMEM;
	}

	/*
	 * 映射成功证明本次存在 EFI 启动交接；位宽描述固件表布局/地址宽度，供
	 * 通用解析器选择正确 ABI。它们是全局能力状态，不由 out 标签清除。
	 */
	set_bit(EFI_BOOT, &efi.flags);
	if (IS_ENABLED(CONFIG_64BIT))
		set_bit(EFI_64BIT, &efi.flags);

	/*
	 * 阶段 2：当前通用 helper 校验 System Table signature，再读取其余成员；
	 * revision 在后续报告/能力路径中解释。失败时 systab 是唯一短期资源，
	 * 统一从 out 撤销。
	 */
	retval = efi_systab_check_header(&systab->hdr);
	if (retval)
		goto out;

	/*
	 * runtime 是固件 runtime-services table 地址；这里只保存地址值，不调用
	 * 服务也不取得映射。revision 同时作为 runtime ABI 版本供后续能力判断。
	 */
	efi.runtime = systab->runtime;
	efi.runtime_version = systab->hdr.revision;

	/* 报告固件厂商时先把可能已虚拟化的 fw_vendor 指针恢复为物理地址。 */
	efi_systab_report_header(&systab->hdr, efi_to_phys(systab->fw_vendor));

	/*
	 * 阶段 3：配置表是 nr_tables 个等长条目；tables 字段同样可能已由
	 * SetVirtualAddressMap() 改成 VA。映射只覆盖数组，解析结束立即撤销。
	 */
	table_size = sizeof(efi_config_table_t) * systab->nr_tables;
	config_tables = early_memremap_ro(efi_to_phys(systab->tables),
					  table_size);
	if (config_tables == NULL) {
		pr_warn("Unable to map EFI config table array.\n");
		retval = -ENOMEM;
		goto out;
	}
	/*
	 * 解析器按 GUID 将物理地址写入 common_tables 和 efi_arch_tables 的目标
	 * 槽，并完成依赖这些表的早期副作用；返回后不借用 config_tables 映射。
	 */
	retval = efi_config_parse_tables(config_tables, systab->nr_tables,
					 efi_arch_tables);

	/* 清理栈：配置数组只在成功映射分支存在，System Table 始终在 out 释放。 */
	early_memunmap(config_tables, table_size);
out:
	early_memunmap(systab, sizeof(efi_system_table_t));
	return retval;
}

/*
 * Return true for regions that can be used as System RAM.
 */
/*
 * 判断一个已具备 CPU cacheability 的 EFI 区域能否作为普通 System RAM。
 *
 * @md：reserve_regions() 当前 descriptor 的只读借用指针，不能为空；本函数
 *      不修改它、不延长其生命周期。返回 true 仅适用于 UEFI 规定在
 *      ExitBootServices() 后可归 OS 使用的类型，并且必须具有 WB 属性；
 *      其他类型或非 WB 区域返回 false。
 *
 * 该判断是 is_memory() 之后的第二层分类：false 并不让区域从 memblock.memory
 * 消失，而会由调用者标为 NOMAP，从而保留物理范围信息但禁止作为普通线性映射
 * RAM 分配。无锁、不睡眠、无其他副作用。
 */
static __init int is_usable_memory(efi_memory_desc_t *md)
{
	switch (md->type) {
	case EFI_LOADER_CODE:
	case EFI_LOADER_DATA:
	case EFI_ACPI_RECLAIM_MEMORY:
	case EFI_BOOT_SERVICES_CODE:
	case EFI_BOOT_SERVICES_DATA:
	case EFI_CONVENTIONAL_MEMORY:
	case EFI_PERSISTENT_MEMORY:
		/*
		 * According to the spec, these regions are no longer reserved
		 * after calling ExitBootServices(). However, we can only use
		 * them as System RAM if they can be mapped writeback cacheable.
		 */
		/*
		 * UEFI 规范允许 OS 在 ExitBootServices() 后接管上述类型；但 Linux
		 * 普通 RAM 的直接映射和页分配器假设 WB 一致性。缺少 WB 时即使类型
		 * 可回收，也不能安全混入普通页，否则可能产生 cache 属性别名。
		 */
		return (md->attribute & EFI_MEMORY_WB);
	default:
		break;
	}
	return false;
}

/*
 * 以 EFI Memory Map 为权威来源重建 memblock.memory。
 *
 * 调用位置：uefi_init() 成功解析系统/配置表后，由 efi_init() 调用。入参和
 * 直接返回值均无；隐式输入是只读 efi.memmap，隐式输出是重写后的
 * memblock.memory，并可能更新 memblock.reserved 与 NOMAP 标记。
 *
 * 主要阶段：
 * 1. 丢弃 DT memory nodes 形成的旧 memory 视图，仅在 KHO 启动时保留 scratch；
 * 2. 逐项把 EFI 4 KiB 页范围规范化为内核 PAGE_SIZE 范围；
 * 3. 跳过 soft-reserved 特殊用途内存，把其余 memory 候选加入 memblock；
 * 4. 对非普通 WB RAM 标 NOMAP，并额外保留 ACPI reclaim 区域。
 *
 * 函数运行于单 CPU、早期 memblock 可变阶段，无锁且不依赖普通分配器。它没有
 * 错误返回：EFI map 已在入口前成功映射并安装元数据，memblock helper 在此
 * 阶段直接修改全局区间表。完成后，普通早期分配只能从新模型中的可映射且未
 * 保留区域取页。
 */
static __init void reserve_regions(void)
{
	/*
	 * 变量地图：
	 *   md     当前 EFI descriptor 的借用游标。
	 *   paddr  当前区间的物理起始字节地址，随后向下对齐到 native page。
	 *   npages 入口时为 EFI_PAGE_SIZE 页数，转换后为 native PAGE_SIZE 页数。
	 *   size   规范化后的区间字节数。
	 */
	efi_memory_desc_t *md;
	u64 paddr, npages, size;

	if (efi_enabled(EFI_DBG))
		pr_info("Processing EFI memory map:\n");

	/*
	 * Discard memblocks discovered so far except for KHO scratch
	 * regions. Most memblocks at this point originate from memory nodes
	 * in the DT and UEFI uses its own memory map instead. However, if
	 * KHO is enabled, scratch regions, which are good known memory
	 * must be preserved.
	 */
	/*
	 * DT 扫描可能已将 /memory 节点加入 memblock，但 EFI 启动以
	 * ExitBootServices() 返回的最终 map 为准。Kexec HandOver 的 scratch
	 * 是前一内核明确交给新内核且已知完好的过渡存储，不能随旧 DT 视图一起
	 * 丢弃，否则会覆盖仍承载 handover 状态的数据。
	 */
	memblock_dump_all();

	if (is_kho_boot()) {
		/* r 指向会被 memblock_remove() 原地压缩的 memory region 数组成员。 */
		struct memblock_region *r;

		/* Remove all non-KHO regions */
		/*
		 * 移除所有非 KHO scratch 区域，只保留可信交接内存。remove 会合并/
		 * 压缩 memblock.memory 数组，所以成功删除当前项后 r--，抵消 for
		 * 循环的 r++，确保不会跳过刚移动到当前位置的下一项。
		 */
		for_each_mem_region(r) {
			if (!memblock_is_kho_scratch(r)) {
				memblock_remove(r->base, r->size);
				r--;
			}
		}
	} else {
		/*
		 * KHO is disabled. Discard memblocks discovered so far:
		 * if there are any at this point, they originate from memory
		 * nodes in the DT, and UEFI uses its own memory map instead.
		 */
		/*
		 * 普通启动没有必须跨重建保留的 memory 标记，删除完整物理地址域即可
		 * 清空 DT 导入结果。这里清的是 memblock.memory 可用集合，不等同于
		 * 释放页，也不会让尚未建立的伙伴分配器发生并发可见性问题。
		 */
		memblock_remove(0, PHYS_ADDR_MAX);
	}

	/* 阶段 2：EFI descriptor 按固件提供的 stride 顺序扫描并重建区间。 */
	for_each_efi_memory_desc(md) {
		paddr = md->phys_addr;
		npages = md->num_pages;

		if (efi_enabled(EFI_DBG)) {
			/* buf 只承载本次日志格式化结果，离开调试分支即失效。 */
			char buf[64];

			pr_info("  0x%012llx-0x%012llx %s\n",
				paddr, paddr + (npages << EFI_PAGE_SHIFT) - 1,
				efi_md_typeattr_format(buf, sizeof(buf), md));
		}

		/*
		 * EFI 页固定为 4 KiB，而内核 native PAGE_SIZE 可能更大。helper 将
		 * 起点向下、终点向上扩到 native 页边界，并把 npages 改写为 native
		 * 页数；随后 size 才能用 PAGE_SHIFT 计算。扩展可能让相邻 descriptor
		 * 在 native 页粒度重叠，memblock 的区间合并逻辑负责规范化结果。
		 */
		memrange_efi_to_native(&paddr, &npages);
		size = npages << PAGE_SHIFT;

		/* 第一层仅接纳具有 WB/WT/WC cacheability 的内存型区域。 */
		if (is_memory(md)) {
			/*
			 * Special purpose memory is 'soft reserved', which
			 * means it is set aside initially. Don't add a memblock
			 * for it now so that it can be hotplugged back in or
			 * be assigned to the dax driver after boot.
			 */
			/*
			 * EFI_MEMORY_SP 常用于高带宽/持久等特殊用途内存。“soft reserve”
			 * 选择不把它加入普通 memblock，既避免启动期分配器消费，又保留
			 * 其固件描述，稍后可由 memory hotplug 或 dax 驱动按用途接管。
			 */
			if (efi_soft_reserve_enabled() &&
			    (md->attribute & EFI_MEMORY_SP))
				continue;

			/*
			 * 体系结构 hook 对齐、裁剪并将候选区间加入 memblock.memory。
			 * 至此范围进入内核物理内存模型，但是否可普通映射仍由下一步决定。
			 */
			early_init_dt_add_memory_arch(paddr, size);

			/*
			 * 类型/属性不满足普通 WB System RAM 契约时标 NOMAP：范围仍被记录，
			 * 但不会进入常规 direct map 与页分配路径，供固件/设备语义保留。
			 */
			if (!is_usable_memory(md))
				memblock_mark_nomap(paddr, size);

			/* keep ACPI reclaim memory intact for kexec etc. */
			/*
			 * ACPI reclaim 内存在规范上最终可回收，但 ACPI 表及 kexec 交接在
			 * 更晚阶段仍可能读取它；先加入 memory 再 reserve，使其属于 RAM
			 * 模型却暂不被早期分配覆盖，待专门路径决定何时释放。
			 */
			if (md->type == EFI_ACPI_RECLAIM_MEMORY)
				memblock_reserve(paddr, size);
		}
	}
}

/*
 * ARM/ARM64 与 RISC-V 的通用 EFI 早期初始化总入口。
 *
 * 主要调用者是体系结构 setup_arch()（ARM32 由 arm_efi_init() 包装）；入口时
 * EFI stub 已退出 Boot Services、FDT 可读、early_ioremap 与 memblock 基础设施
 * 可用，其他 CPU 尚未启动。函数无参数、无直接返回值、不持锁，不使用普通
 * kmalloc/vmalloc；early mapping 与 memblock helper 适用于这一不可依赖完整 VM
 * 的阶段。
 *
 * 成功路径：
 *   FDT 交接参数 -> 安装 efi.memmap -> 解析 System/Config Tables
 *   -> 重建并裁剪 memblock -> 登记 mirror/ESRT/MOK/display -> 保留 map 本体。
 *
 * 未发现 EFI FDT 参数时静默退化为非 EFI 启动。EFI map 无法映射时，由于没有
 * 其他可信内存描述而 panic；System/Config Table 解析失败时撤销 EFI map 并
 * 返回，停止本文件后续副作用。成功返回后 efi.memmap 仍是有效 early mapping，
 * data 对应的物理 map 已被 memblock_reserve() 防止覆盖；体系结构后续代码负责
 * 撤销或替换映射，并继续 paging/runtime 初始化。
 */
void __init efi_init(void)
{
	/*
	 * 变量地图：
	 *   data             FDT 填充的 EFI memory-map 交接快照；phys_map 为物理
	 *                    字节地址，size/desc_size 为字节，desc_version 为 ABI。
	 *   efi_system_table FDT 提供的 System Table 物理字节地址；0 是“无 EFI”。
	 */
	struct efi_memory_map_data data;
	u64 efi_system_table;

	/* Grab UEFI information placed in FDT by stub */
	/*
	 * EFI stub（或 Xen PV 节点）把 System Table 与最终 Memory Map 元数据写入
	 * FDT。helper 只有在全部必要属性可用时才返回非零 system-table 地址，并
	 * 同时完整填充 data；返回 0 时 data 不再被读取。
	 */
	efi_system_table = efi_get_fdt_params(&data);
	if (!efi_system_table)
		return;

	/*
	 * 阶段 1：把 data.phys_map 临时映射为 efi.memmap 并发布 EFI_MEMMAP 标志。
	 * 成功后 for_each_efi_memory_desc() 才可安全使用；失败尚无映射可清理。
	 */
	if (efi_memmap_init_early(&data) < 0) {
		/*
		* If we are booting via UEFI, the UEFI memory map is the only
		* description of memory we have, so there is little point in
		* proceeding if we cannot access it.
		*/
		/*
		 * EFI 路径随后会丢弃 DT /memory 视图，以 ExitBootServices() 时的
		 * 最终 map 为唯一权威；无法访问它就无法区分 RAM、runtime、保留和
		 * 设备区间。继续启动可能覆盖固件数据，因此这里是不可恢复的 fatal
		 * 边界，而不是退回不完整的 DT 描述。
		 */
		panic("Unable to map EFI memory map.\n");
	}

	/*
	 * 当前内核按已知字段解释 descriptor，但遍历步长使用固件给出的 desc_size。
	 * version 非 1 可能提示固件 ABI 异常，WARN 留下诊断后仍尝试继续，避免把
	 * 可兼容扩展误判成必然不可启动。
	 */
	WARN(efi.memmap.desc_version != 1,
	     "Unexpected EFI_MEMORY_DESCRIPTOR version %ld",
	      efi.memmap.desc_version);

	/* 阶段 2：导入 System Table 与 GUID 配置表；失败时只需撤销已安装的 map。 */
	if (uefi_init(efi_system_table) < 0) {
		efi_memmap_unmap();
		return;
	}

	/*
	 * 阶段 3：配置表已解析，现以同一 EFI map 重建 memblock。该顺序确保后续
	 * 所有 memblock 操作都基于固件最终内存类型，而非早先 DT memory nodes。
	 */
	reserve_regions();
	/*
	 * For memblock manipulation, the cap should come after the memblock_add().
	 * And now, memblock is fully populated, it is time to do capping.
	 */
	/*
	 * /chosen/linux,usable-memory-range 是对“可用 RAM”的上限约束，必须在
	 * reserve_regions() 已 add 全部候选区间后统一求交；若提前执行，随后 add
	 * 会把被裁掉的范围重新加入。完成后 memblock.memory 才是最终可用窗口。
	 */
	early_init_dt_check_for_usable_mem_range();
	/*
	 * 阶段 4：在稳定的 EFI/memblock 模型上登记派生固件设施。
	 * - efi_find_mirror() 把 MORE_RELIABLE descriptor 标为 mirrored memory；
	 * - efi_esrt_init() 校验并保留固件资源表，供 capsule/update sysfs 使用；
	 * - efi_mokvar_table_init() 校验并保留 bootloader 交接的 MOK 变量表。
	 * 它们各自对表缺失采取无操作，不把可选设施失败升级为整机启动失败。
	 */
	efi_find_mirror();
	efi_esrt_init();
	efi_mokvar_table_init();

	/*
	 * EFI map 的源缓冲区自身也坐落在物理内存中。按页向下/向上覆盖完整映射，
	 * 防止随后 memblock 分配或页表建立覆盖仍由 efi.memmap early VA 读取的
	 * descriptor。保留责任进入全局 memblock.reserved，局部 data 随后可失效。
	 */
	memblock_reserve(data.phys_map & PAGE_MASK,
			 PAGE_ALIGN(data.size + (data.phys_map & ~PAGE_MASK)));

	/*
	 * 最后消费 primary-display 表：此时 GUID 地址已解析、memblock 已完整，
	 * 因而既能复制显示快照，也能正确判断/标记 framebuffer 所在内存。此调用
	 * 条件与 EFI stub 直接填充该对象的消费者条件保持一致；FIRMWARE_EDID 只
	 * 影响对象是否包含/导出 EDID 字段，不单独要求重探 early display。
	 */
	if (IS_ENABLED(CONFIG_X86) ||
	    IS_ENABLED(CONFIG_SYSFB) ||
	    IS_ENABLED(CONFIG_EFI_EARLYCON))
		init_primary_display();
}
