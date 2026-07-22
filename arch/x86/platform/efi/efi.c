// SPDX-License-Identifier: GPL-2.0
/*
 * x86 EFI 启动与运行时映射学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。此标识只说明新增中文注释
 * 的生成来源，不属于上游作者或版权声明。
 *
 * 文件职责：把 boot loader 留在 boot_params 中的 EFI 物理地址信息，
 * 转换成 Linux 可以长期使用的系统表、配置表、内存类型和 EFI runtime
 * 页表。它负责 x86 特有的 e820 协调、32/64 位固件模式判断以及进入
 * EFI 虚拟地址模式；具体 runtime service 调用桩、变量服务和通用表解析
 * 位于其他 EFI 文件，本文件不实现固件服务本身。
 *
 * 主调用链：
 *   setup_arch()
 *     -> efi_memblock_x86_reserve_range() 早期映射并保留固件 memmap
 *     -> efi_init() 解析 system table/config tables，判断 runtime 可用性
 *   identify_boot_cpu()
 *     -> efi_enter_virtual_mode()
 *        -> 普通启动：__efi_enter_virtual_mode()
 *           -> 合并 descriptor -> 建 EFI 页表 -> SetVirtualAddressMap()
 *        -> kexec：kexec_enter_virtual_mode() 复用前一内核选定的虚拟地址
 *
 * 核心对象：efi.memmap 描述固件物理内存；efi_systab_phys 指向 system
 * table；efi_runtime/efi_config_table/efi_fw_vendor 是该表导出的物理地址；
 * efi_pgd（由架构 helper 管理）只在调用 runtime service 时作为专用页表。
 * init 阶段先借助 early_memremap 临时访问固件页，进入虚拟模式后再发布
 * 长期页表与服务函数，临时映射必须逐一撤销。
 *
 * 并发模型：绝大多数函数带 __init，只在单 CPU 启动阶段串行修改全局
 * EFI 状态；进入运行期后，服务调用的并发与页表切换由 EFI runtime 层
 * 的锁和调用包装器负责。本文件的地址查询/sysfs getter 只读取初始化后
 * 不再变化的结果，因此无需在这里另加锁。
 *
 * 方案权衡：专用 EFI 页表隔离固件所需映射，避免把 runtime 区域永久
 * 暴露在普通内核页表中；代价是每次固件调用需要受控页表上下文切换，
 * 还必须兼容有缺陷的固件、mixed mode 和 kexec 的地址连续性要求。
 */
/*
 * Common EFI (Extensible Firmware Interface) support functions
 * Based on Extensible Firmware Interface Specification version 1.0
 *
 * Copyright (C) 1999 VA Linux Systems
 * Copyright (C) 1999 Walt Drummond <drummond@valinux.com>
 * Copyright (C) 1999-2002 Hewlett-Packard Co.
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 *	Stephane Eranian <eranian@hpl.hp.com>
 * Copyright (C) 2005-2008 Intel Co.
 *	Fenghua Yu <fenghua.yu@intel.com>
 *	Bibo Mao <bibo.mao@intel.com>
 *	Chandramouli Narayanan <mouli@linux.intel.com>
 *	Huang Ying <ying.huang@intel.com>
 * Copyright (C) 2013 SuSE Labs
 *	Borislav Petkov <bp@suse.de> - runtime services VA mapping
 *
 * Copied from efi_32.c to eliminate the duplicated code between EFI
 * 32/64 support code. --ying 2007-10-26
 *
 * All EFI Runtime Services are not implemented yet as EFI only
 * supports physical mode addressing on SoftSDV. This is to be fixed
 * in a future version.  --drummond 1999-07-20
 *
 * Implemented EFI runtime services and virtual mode calls.  --davidm
 *
 * Goutham Rao: <goutham.rao@intel.com>
 *	Skip non-WB memory and ignore empty memory ranges.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/efi.h>
#include <linux/efi-bgrt.h>
#include <linux/export.h>
#include <linux/memblock.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/time.h>
#include <linux/io.h>
#include <linux/reboot.h>
#include <linux/bcd.h>

#include <asm/setup.h>
#include <asm/efi.h>
#include <asm/e820/api.h>
#include <asm/time.h>
#include <asm/tlbflush.h>
#include <asm/x86_init.h>
#include <asm/uv/uv.h>

static unsigned long efi_systab_phys __initdata;
/*
 * efi_systab_phys 是 boot loader 给出的 EFI system table 物理地址，仅
 * 初始化期使用，故可随 __initdata 回收。efi_runtime 是 runtime-services
 * table 地址，efi_nr_tables 是 configuration table 表项数量；后两者还
 * 被 sysfs/后续虚拟模式建立过程读取，不能标为 __initdata。
 */
static unsigned long efi_runtime, efi_nr_tables;

/* 固件厂商字符串和 configuration-table 数组的物理地址，初始化后只读。 */
unsigned long efi_fw_vendor, efi_config_table;

/*
 * x86 在通用 EFI 配置表之外额外识别的 GUID 表。每项把 GUID 对应的
 * 物理地址写入目标全局变量；空项是解析器的终止哨兵。__initconst 允许
 * 解析完成后回收该只读描述数组。
 */
static const efi_config_table_type_t arch_tables[] __initconst = {
#ifdef CONFIG_X86_UV
	{UV_SYSTEM_TABLE_GUID,		&uv_systab_phys,	"UVsystab"	},
#endif
	{},
};

static const unsigned long * const efi_tables[] = {
	/*
	 * 该数组保存“地址变量的地址”，而不是固件表地址本身。这样下面的
	 * efi_is_table_address() 可统一遍历所有已发现表，并让各变量在解析后
	 * 更新而无需重建索引；数组及其指针均只读，目标变量在 init 后稳定。
	 */
	&efi.acpi,
	&efi.acpi20,
	&efi.smbios,
	&efi.smbios3,
#ifdef CONFIG_X86_UV
	&uv_systab_phys,
#endif
	&efi_fw_vendor,
	&efi_runtime,
	&efi_config_table,
	&efi.esrt,
	&efi_mem_attr_table,
#ifdef CONFIG_EFI_RCI2_TABLE
	&rci2_table_phys,
#endif
	&efi.tpm_log,
	&efi.tpm_final_log,
	&efi_rng_seed,
#ifdef CONFIG_LOAD_UEFI_KEYS
	&efi.mokvar_table,
#endif
#ifdef CONFIG_EFI_COCO_SECRET
	&efi.coco_secret,
#endif
#ifdef CONFIG_UNACCEPTED_MEMORY
	&efi.unaccepted,
#endif
};

u64 efi_setup;		/* efi setup_data physical address */
/*
 * efi_setup 是 kexec/boot protocol 的 setup_data 物理地址。它补充 system
 * table 中在第二内核环境可能不可直接复用的固件指针，并作为“应复用
 * 前一内核 EFI 虚拟映射”的标志；0 表示普通冷启动。
 */

static int add_efi_memmap __initdata;
/*
 * 解析 add_efi_memmap 启动参数。arg 无需内容，参数出现即把 init-only
 * 开关置 1；始终返回 0 表示参数已消费。它只影响早期是否把完整 EFI
 * map 合并进 e820，不持有 arg，也不分配资源。
 */
static int __init setup_add_efi_memmap(char *arg)
{
	add_efi_memmap = 1;
	return 0;
}
early_param("add_efi_memmap", setup_add_efi_memmap);
/* early_param 保证 e820/memblock 定型前执行，否则导入 EFI 区域已来不及。 */

/*
 * Tell the kernel about the EFI memory map.  This might include
 * more than the max 128 entries that can fit in the passed in e820
 * legacy (zeropage) memory map, but the kernel's e820 table can hold
 * E820_MAX_ENTRIES.
 */
/*
 * 把 EFI descriptor 转换并追加到 x86 e820 表。
 *
 * 调用者已确保 efi.memmap 可遍历；函数无入参/返回值，输出是 e820_table
 * 被追加并重新规范化。类型转换采取保守策略：只有带 WB 属性的普通可用
 * 内存才成为 RAM，未知类型全部保留，避免 Linux 分配固件仍可能访问的页。
 * 该函数只在 __init 串行环境运行，不负责保留原 EFI memmap 存储。
 */

static void __init do_add_efi_memmap(void)
{
	/* md 是遍历宏逐项借用的 descriptor 指针，所有权仍属 efi.memmap。 */
	efi_memory_desc_t *md;

	if (!efi_enabled(EFI_MEMMAP))
		return;

	for_each_efi_memory_desc(md) {
		/* EFI 页数转为字节区间；e820_type 是 Linux 对应资源类别。 */
		unsigned long long start = md->phys_addr;
		unsigned long long size = md->num_pages << EFI_PAGE_SHIFT;
		int e820_type;

		switch (md->type) {
		case EFI_LOADER_CODE:
		case EFI_LOADER_DATA:
		case EFI_BOOT_SERVICES_CODE:
		case EFI_BOOT_SERVICES_DATA:
		case EFI_CONVENTIONAL_MEMORY:
			/*
			 * EFI_MEMORY_SP 表示固件指定用途内存：策略允许时转成
			 * soft-reserved，用户仍可显式交给专用驱动；否则只有
			 * write-back 普通内存能进入伙伴系统。
			 */
			if (efi_soft_reserve_enabled()
			    && (md->attribute & EFI_MEMORY_SP))
				e820_type = E820_TYPE_SOFT_RESERVED;
			else if (md->attribute & EFI_MEMORY_WB)
				e820_type = E820_TYPE_RAM;
			else
				e820_type = E820_TYPE_RESERVED;
			break;
		case EFI_ACPI_RECLAIM_MEMORY:
			e820_type = E820_TYPE_ACPI;
			break;
		case EFI_ACPI_MEMORY_NVS:
			e820_type = E820_TYPE_NVS;
			break;
		case EFI_UNUSABLE_MEMORY:
			e820_type = E820_TYPE_UNUSABLE;
			break;
		case EFI_PERSISTENT_MEMORY:
			e820_type = E820_TYPE_PMEM;
			break;
		default:
			/*
			 * EFI_RESERVED_TYPE EFI_RUNTIME_SERVICES_CODE
			 * EFI_RUNTIME_SERVICES_DATA EFI_MEMORY_MAPPED_IO
			 * EFI_MEMORY_MAPPED_IO_PORT_SPACE EFI_PAL_CODE
			 */
			e820_type = E820_TYPE_RESERVED;
			break;
		}

		e820__range_add(start, size, e820_type);
		/* range_add 先收集可能重叠区间，循环后统一排序/合并。 */
	}
	/* 解析全部 descriptor 后建立无序/重叠已规整的最终 e820 视图。 */
	e820__update_table(e820_table);
}

/*
 * Given add_efi_memmap defaults to 0 and there is no alternative
 * e820 mechanism for soft-reserved memory, import the full EFI memory
 * map if soft reservations are present and enabled. Otherwise, the
 * mechanism to disable the kernel's consideration of EFI_MEMORY_SP is
 * the efi=nosoftreserve option.
 */
/* 函数契约详见紧随声明后的说明；本函数无副作用，只返回是否需要导入。 */
static bool do_efi_soft_reserve(void)
/*
 * 判断是否必须自动导入完整 EFI map 来保住 specific-purpose 区域。
 * 无入参；返回 true 仅表示 map 有可用 conventional+SP 项且策略未禁用。
 * 这是 add_efi_memmap 默认关闭时的安全 fallback：不导入会让旧 e820 将
 * 这些页当普通 RAM。函数只读启动期 map，无锁且无资源副作用。
 */
{
	efi_memory_desc_t *md;

	if (!efi_enabled(EFI_MEMMAP))
		return false;

	if (!efi_soft_reserve_enabled())
		return false;

	for_each_efi_memory_desc(md)
		if (md->type == EFI_CONVENTIONAL_MEMORY &&
		    (md->attribute & EFI_MEMORY_SP))
			return true;
	return false;
}

/* 函数契约详见紧随声明后的说明；这是 setup_arch() 的早期保留入口。 */
int __init efi_memblock_x86_reserve_range(void)
/*
 * 建立最早期 EFI memory-map 视图并从 memblock 保留其底层物理存储。
 *
 * 调用关系：setup_arch() 在通用内存分配器启动前调用；成功后的
 * efi.memmap 供 efi_init()、e820 导入和虚拟模式建立消费。
 * 入参：无。返回 0 表示成功或 paravirt 无需本地处理；负 errno 表示
 * 物理地址不可表示、临时映射失败等，调用者将禁用相应 EFI 路径。
 *
 * memblock_reserve() 只防止该物理区被早期分配覆盖，不建立永久虚拟映射。
 * EFI_PRESERVE_BS_REGIONS 则要求后续保留 boot-services 区，以兼容退出
 * boot services 后仍错误访问这些内存的固件，安全性更高但会占用内存。
 */
{
	/* e 借用 zero-page boot_params；data 描述传给通用 memmap 层的布局。 */
	struct efi_info *e = &boot_params.efi_info;
	struct efi_memory_map_data data;
	phys_addr_t pmap;
	int rv;

	if (efi_enabled(EFI_PARAVIRT))
		/* Xen 等 paravirt 平台由其 EFI 代理建立表，不可重复映射物理固件页。 */
		return 0;

	/* Can't handle firmware tables above 4GB on i386 */
	/* i386 的 phys_addr_t/早期映射无法表示该位置，继续会截断并映射错误页。 */
	if (IS_ENABLED(CONFIG_X86_32) && e->efi_memmap_hi > 0) {
		pr_err("Memory map is above 4GB, disabling EFI.\n");
		return -EINVAL;
	}
	pmap = (phys_addr_t)(e->efi_memmap | ((u64)e->efi_memmap_hi << 32));
	/* boot protocol 拆成高低 32 位，此处先扩成 64 位再组合以免移位截断。 */

	data.phys_map		= pmap;
	data.size 		= e->efi_memmap_size;
	data.desc_size		= e->efi_memdesc_size;
	data.desc_version	= e->efi_memdesc_version;

	if (!efi_enabled(EFI_PARAVIRT)) {
		/* 注册 early_memremap 支持的临时 map；失败时尚未取得 memblock 保留。 */
		rv = efi_memmap_init_early(&data);
		if (rv)
			return rv;
	}

	if (add_efi_memmap || do_efi_soft_reserve())
		do_add_efi_memmap();

	WARN(efi.memmap.desc_version != 1,
	     "Unexpected EFI_MEMORY_DESCRIPTOR version %ld",
	     efi.memmap.desc_version);

	memblock_reserve(pmap, efi.memmap.nr_map * efi.memmap.desc_size);
	/* 按校验后的实际 descriptor 数保留原始数组字节范围。 */
	set_bit(EFI_PRESERVE_BS_REGIONS, &efi.flags);

	return 0;
}

#define OVERFLOW_ADDR_SHIFT	(64 - EFI_PAGE_SHIFT)
#define OVERFLOW_ADDR_MASK	(U64_MAX << OVERFLOW_ADDR_SHIFT)
#define U64_HIGH_BIT		(~(U64_MAX >> 1))
/*
 * 这些常量用于在没有 128 位算术的情况下诊断 EFI 页区间溢出：高于
 * EFI_PAGE_SHIFT 可表达范围的页数位构成 end_hi，U64_HIGH_BIT 则检测
 * phys_addr + size 在 64 位边界回绕。它们只服务启动期固件输入校验。
 */

/*
 * 校验单个 EFI descriptor 的 [phys_addr, end] 是否能由 64 位物理地址
 * 表示。md 为只读借用指针，i 仅用于诊断编号；返回 true 表示区间有效，
 * false 表示零页或加法溢出并已打印一次总告警及本项详情。函数不修改
 * descriptor，调用者 efi_clean_memmap() 决定如何剔除无效项。
 */
static bool __init efi_memmap_entry_valid(const efi_memory_desc_t *md, int i)
{
	/* end 是低 64 位闭区间末端；end_hi 非零表示数学结果超过 64 位。 */
	u64 end = (md->num_pages << EFI_PAGE_SHIFT) + md->phys_addr - 1;
	u64 end_hi = 0;
	char buf[64];

	if (md->num_pages == 0) {
		/* 零长度 descriptor 无可映射范围，即使算术未溢出也判无效。 */
		end = 0;
	} else if (md->num_pages > EFI_PAGES_MAX ||
		   EFI_PAGES_MAX - md->num_pages <
		   (md->phys_addr >> EFI_PAGE_SHIFT)) {
		end_hi = (md->num_pages & OVERFLOW_ADDR_MASK)
			>> OVERFLOW_ADDR_SHIFT;

		if ((md->phys_addr & U64_HIGH_BIT) && !(end & U64_HIGH_BIT))
			end_hi += 1;
	} else {
		/* 常见有效路径避免后续格式化与告警开销。 */
		return true;
	}

	pr_warn_once(FW_BUG "Invalid EFI memory map entries:\n");

	if (end_hi) {
		pr_warn("mem%02u: %s range=[0x%016llx-0x%llx%016llx] (invalid)\n",
			i, efi_md_typeattr_format(buf, sizeof(buf), md),
			md->phys_addr, end_hi, end);
	} else {
		pr_warn("mem%02u: %s range=[0x%016llx-0x%016llx] (invalid)\n",
			i, efi_md_typeattr_format(buf, sizeof(buf), md),
			md->phys_addr, end);
	}
	return false;
}

/* 原地删除无效 descriptor，并在有变化时重新发布缩短后的 map。 */
static void __init efi_clean_memmap(void)
/*
 * 原地压缩 EFI memory map，删除 efi_memmap_entry_valid() 拒绝的项。
 * 无入参/返回值；成功后 efi.memmap 仍指向同一物理缓冲区，但 nr_map/size
 * 缩短且有效 descriptor 连续排列。in 只读向前扫描，out 指向下一个写入
 * 槽，因 out 永不超过 in，memcpy 不会覆盖尚未检查的输入。若有删除，
 * efi_memmap_install() 重新发布元数据；没有删除则保持原对象不变。
 */
{
	/* out/in/end 都以 desc_size 字节步进，不能用 C 结构体大小代替固件步长。 */
	efi_memory_desc_t *out = efi.memmap.map;
	const efi_memory_desc_t *in = out;
	const efi_memory_desc_t *end = efi.memmap.map_end;
	int i, n_removal;

	for (i = n_removal = 0; in < end; i++) {
		if (efi_memmap_entry_valid(in, i)) {
			if (out != in)
				memcpy(out, in, efi.memmap.desc_size);
			out = (void *)out + efi.memmap.desc_size;
		} else {
			/* 无效项不复制，out 停留，从而被下一有效项覆盖。 */
			n_removal++;
		}
		in = (void *)in + efi.memmap.desc_size;
	}

	if (n_removal > 0) {
		/* data 复用原物理基址/版本/步长，只缩短公开字节数。 */
		struct efi_memory_map_data data = {
			.phys_map	= efi.memmap.phys_map,
			.desc_version	= efi.memmap.desc_version,
			.desc_size	= efi.memmap.desc_size,
			.size		= efi.memmap.desc_size * (efi.memmap.nr_map - n_removal),
			.flags		= 0,
		};

		pr_warn("Removing %d invalid memory map entries.\n", n_removal);
		efi_memmap_install(&data);
		/* 安装的是对现有缓冲区的描述，不转移或释放底层物理页。 */
	}
}

/*
 * Firmware can use EfiMemoryMappedIO to request that MMIO regions be
 * mapped by the OS so they can be accessed by EFI runtime services, but
 * should have no other significance to the OS (UEFI r2.10, sec 7.2).
 * However, most bootloaders and EFI stubs convert EfiMemoryMappedIO
 * regions to E820_TYPE_RESERVED entries, which prevent Linux from
 * allocating space from them (see remove_e820_regions()).
 *
 * Some platforms use EfiMemoryMappedIO entries for PCI MMCONFIG space and
 * PCI host bridge windows, which means Linux can't allocate BAR space for
 * hot-added devices.
 *
 * Remove large EfiMemoryMappedIO regions from the E820 map to avoid this
 * problem.
 *
 * Retain small EfiMemoryMappedIO regions because on some platforms, these
 * describe non-window space that's included in host bridge _CRS.  If we
 * assign that space to PCI devices, they don't work.
 */
/*
 * 固件把 EfiMemoryMappedIO 同时塞入 e820 reserved 会造成资源窗口冲突：
 * Linux 既不能把大窗口用于热插 PCI BAR，又不能简单删除所有小区域，
 * 因为部分平台用小项描述 host bridge _CRS 中不可分配的洞。这里采用
 * 256 KiB 经验阈值：大项从 e820 reserved 去除以恢复 PCI 分配空间，
 * 小项保留以避免设备 BAR 落入真实 MMIO 寄存器。EFI descriptor 本身
 * 不删除，runtime 页表仍能按固件要求映射它。
 */
/*
 * 无入参/返回值；仅在 init 串行阶段修改 e820 资源视图。范围单位由
 * EFI 页转换为字节，日志分别以 MiB/KiB 输出。该策略兼容性优先，阈值
 * 并非 EFI ABI，收益是热插可用窗口增加，代价是平台启发式判断。
 */
static void __init efi_remove_e820_mmio(void)
{
	/* i 是原 descriptor 编号，即使非 MMIO 项也递增，便于和 memmap 日志对应。 */
	efi_memory_desc_t *md;
	u64 size, start, end;
	int i = 0;

	for_each_efi_memory_desc(md) {
		if (md->type == EFI_MEMORY_MAPPED_IO) {
			size = md->num_pages << EFI_PAGE_SHIFT;
			start = md->phys_addr;
			end = start + size - 1;
			if (size >= 256*1024) {
				/* 只移除 E820_TYPE_RESERVED 的重叠，不触碰其他类型所有权。 */
				pr_info("Remove mem%02u: MMIO range=[0x%08llx-0x%08llx] (%lluMB) from e820 map\n",
					i, start, end, size >> 20);
				e820__range_remove(start, size, E820_TYPE_RESERVED);
			} else {
				pr_info("Not removing mem%02u: MMIO range=[0x%08llx-0x%08llx] (%lluKB) from e820 map\n",
					i, start, end, size >> 10);
			}
		}
		i++;
	}
}

/* 只读诊断接口：打印当前 EFI map，不改变 descriptor 或映射生命周期。 */
void __init efi_print_memmap(void)
/*
 * 以固件 descriptor 顺序打印当前 efi.memmap。无入参、无返回和状态
 * 修改；每项输出类型/属性、闭区间与 MiB 大小，供 EFI_DBG 诊断清理、
 * 合并和虚拟地址赋值结果。调用期 map 必须已映射且稳定。
 */
{
	efi_memory_desc_t *md;
	int i = 0;

	for_each_efi_memory_desc(md) {
		char buf[64];

		pr_info("mem%02u: %s range=[0x%016llx-0x%016llx] (%lluMB)\n",
			i++, efi_md_typeattr_format(buf, sizeof(buf), md),
			md->phys_addr,
			md->phys_addr + (md->num_pages << EFI_PAGE_SHIFT) - 1,
			(md->num_pages >> (20 - EFI_PAGE_SHIFT)));
	}
}

/* 解析 phys 指向的 system table，成功后发布 runtime/config/vendor 根地址。 */
static int __init efi_systab_init(unsigned long phys)
/*
 * 临时映射并解析 EFI system table。
 *
 * phys 是 system table 物理字节地址，必须指向与 EFI_64BIT 标志匹配的
 * 32/64 位表；函数根据模式只映射对应结构大小。成功返回 0，并发布
 * efi_runtime、efi_fw_vendor、efi_config_table、efi_nr_tables 以及 runtime
 * revision；失败返回负 errno，所有 early_memremap 映射均已撤销。
 *
 * kexec 的 efi_setup_data 可覆盖 vendor/tables 指针，保证第二内核使用
 * 第一内核传递的有效物理位置。32 位内核无法寻址 4 GiB 以上数据，因此
 * over4g 是整组关键指针的累积判定，任一越界便整体禁用 EFI。
 */
{
	/* size 由固件位数而非本内核指针大小决定；hdr/p 只在临时映射期间有效。 */
	int size = efi_enabled(EFI_64BIT) ? sizeof(efi_system_table_64_t)
					  : sizeof(efi_system_table_32_t);
	const efi_table_hdr_t *hdr;
	bool over4g = false;
	void *p;
	int ret;

	hdr = p = early_memremap_ro(phys, size);
	/* 只读映射阻止内核意外改写固件 system table；此 helper 可用于极早期。 */
	if (p == NULL) {
		pr_err("Couldn't map the system table!\n");
		return -ENOMEM;
	}

	ret = efi_systab_check_header(hdr);
	/* 校验签名、头长度/版本等 ABI；失败先撤销映射再原样返回错误。 */
	if (ret) {
		early_memunmap(p, size);
		return ret;
	}

	if (efi_enabled(EFI_64BIT)) {
		const efi_system_table_64_t *systab64 = p;

		efi_runtime	= systab64->runtime;
		over4g		= systab64->runtime > U32_MAX;

		if (efi_setup) {
			/* data 是 kexec setup_data 的短期只读映射，不取得其物理页所有权。 */
			struct efi_setup_data *data;

			data = early_memremap_ro(efi_setup, sizeof(*data));
			if (!data) {
				early_memunmap(p, size);
				return -ENOMEM;
			}

			efi_fw_vendor		= (unsigned long)data->fw_vendor;
			efi_config_table	= (unsigned long)data->tables;

			over4g |= data->fw_vendor	> U32_MAX ||
				  data->tables		> U32_MAX;

			early_memunmap(data, sizeof(*data));
			/* 先把标量地址复制到全局，再撤销临时虚拟映射。 */
		} else {
			efi_fw_vendor		= systab64->fw_vendor;
			efi_config_table	= systab64->tables;

			over4g |= systab64->fw_vendor	> U32_MAX ||
				  systab64->tables	> U32_MAX;
		}
		efi_nr_tables = systab64->nr_tables;
	} else {
		const efi_system_table_32_t *systab32 = p;

		efi_fw_vendor		= systab32->fw_vendor;
		efi_runtime		= systab32->runtime;
		efi_config_table	= systab32->tables;
		efi_nr_tables		= systab32->nr_tables;
	}

	efi.runtime_version = hdr->revision;

	efi_systab_report_header(hdr, efi_fw_vendor);
	/* report 必须在 unmap 前完成，因为 hdr 仍指向临时映射。 */
	early_memunmap(p, size);

	if (IS_ENABLED(CONFIG_X86_32) && over4g) {
		pr_err("EFI data located above 4GB, disabling EFI.\n");
		return -EINVAL;
	}

	return 0;
}

/* 按 GUID 解析 configuration table，并始终撤销其 early 临时映射。 */
static int __init efi_config_init(const efi_config_table_type_t *arch_tables)
/*
 * 映射 system table 指向的 configuration-table 数组并交通用解析器按 GUID
 * 发布各表地址。arch_tables 是以空项结尾的架构扩展描述数组，只读借用且
 * 不可为 NULL（本文件传全局 arch_tables）；返回 0 或解析/映射负 errno。
 * 无表时直接成功。映射大小是 nr_tables * 固件位数对应 entry 大小；
 * early_memunmap 在成功和解析失败时都执行，解析结果已复制到全局变量。
 */
{
	void *config_tables;
	int sz, ret;

	if (efi_nr_tables == 0)
		return 0;

	if (efi_enabled(EFI_64BIT))
		sz = sizeof(efi_config_table_64_t);
	else
		sz = sizeof(efi_config_table_32_t);

	/*
	 * Let's see what config tables the firmware passed to us.
	 */
	config_tables = early_memremap(efi_config_table, efi_nr_tables * sz);
	/* 固件可能把数组放在普通内核尚未映射的物理区，故不能直接解引用地址。 */
	if (config_tables == NULL) {
		pr_err("Could not map Configuration table!\n");
		return -ENOMEM;
	}

	ret = efi_config_parse_tables(config_tables, efi_nr_tables,
				      arch_tables);
	/* parser 识别 ACPI/SMBIOS/ESRT 等 GUID，并把物理地址写到目标全局槽。 */

	early_memunmap(config_tables, efi_nr_tables * sz);
	return ret;
}

/* 串联 system/config 解析并决定是否向后续阶段发布 runtime-services 能力。 */
void __init efi_init(void)
/*
 * x86 EFI 元数据初始化总入口。
 *
 * 调用关系：在 efi_memblock_x86_reserve_range() 已建立 early memmap 后由
 * setup_arch 路径调用；成功结果交给 efi_enter_virtual_mode()。无入参和
 * 返回值，以 EFI flags 和全局表地址表示成功程度。任一关键解析失败都
 * 提前返回，不发布 runtime-services 位；runtime 不支持/被禁用时主动
 * unmap early memmap，配置表结果仍可供非 runtime 功能使用。
 */
{
	if (IS_ENABLED(CONFIG_X86_32) &&
	    (boot_params.efi_info.efi_systab_hi ||
	     boot_params.efi_info.efi_memmap_hi)) {
		pr_info("Table located above 4GB, disabling EFI.\n");
		return;
	}

	efi_systab_phys = boot_params.efi_info.efi_systab |
			  ((__u64)boot_params.efi_info.efi_systab_hi << 32);

	if (efi_systab_init(efi_systab_phys))
		/* system table 是所有后续地址的根；失败时不能进行部分解析。 */
		return;

	if (efi_reuse_config(efi_config_table, efi_nr_tables))
		/* kexec 已从 setup_data 恢复配置表时无需再次映射固件数组。 */
		/*
		 * 【对上句返回语义的修正】efi_reuse_config() 返回 0 表示无需
		 * 复用或复用成功，随后仍会正常解析配置表；这里只有非零错误
		 * 才提前返回，并不是“已经复用所以返回”。
		 */
		return;

	if (efi_config_init(arch_tables))
		return;

	/*
	 * Note: We currently don't support runtime services on an EFI
	 * that doesn't match the kernel 32/64-bit mode.
	 */
	/*
	 * system/config 表仍可解析，但跨位数调用需要 thunk；当前只支持
	 * 64 位内核调用 32 位 EFI 的既定 mixed mode，其他不匹配必须关闭
	 * runtime，不能把不同宽度的函数指针直接当本地 ABI 调用。
	 */

	if (!efi_runtime_supported())
		pr_err("No EFI runtime due to 32/64-bit mismatch with kernel\n");

	if (!efi_runtime_supported() || efi_runtime_disabled()) {
		/* 不会进入虚拟模式，尽早撤销只为 runtime 保留的 early memmap 映射。 */
		efi_memmap_unmap();
		return;
	}

	set_bit(EFI_RUNTIME_SERVICES, &efi.flags);
	/* 先发布能力位，再清理 map；后续失败路径负责清除此位实现降级。 */
	efi_clean_memmap();

	efi_remove_e820_mmio();

	if (efi_enabled(EFI_DBG))
		efi_print_memmap();
}

/* Merge contiguous regions of the same type and attribute */
/*
 * 把物理地址连续且 type/attribute 完全相同的 descriptor 合并到前一项。
 * 无入参/返回值，原地修改 efi.memmap；被吞并项不移动，而是改成零属性
 * EFI_RESERVED_TYPE，后续 should_map_region() 会跳过它。这样减少需要传给
 * SetVirtualAddressMap() 的有效片段，同时保持固定 desc_size 数组布局。
 * 只有真正相邻才合并，跨洞或不同 cache/runtime 属性绝不能合并，否则
 * 会把固件页表权限扩大到不属于该区域的地址。
 */
static void __init efi_merge_regions(void)
{
	/* prev_md 指向当前可合并链的首项，md 顺序遍历固件 map。 */
	efi_memory_desc_t *md, *prev_md = NULL;

	for_each_efi_memory_desc(md) {
		u64 prev_size;

		if (!prev_md) {
			prev_md = md;
			continue;
		}

		if (prev_md->type != md->type ||
		    prev_md->attribute != md->attribute) {
			prev_md = md;
			continue;
		}

		prev_size = prev_md->num_pages << EFI_PAGE_SHIFT;
		/* EFI 页数转换为字节后才能与物理字节地址比较连续性。 */

		if (md->phys_addr == (prev_md->phys_addr + prev_size)) {
			prev_md->num_pages += md->num_pages;
			md->type = EFI_RESERVED_TYPE;
			md->attribute = 0;
			continue;
		}
		prev_md = md;
	}
}

/* 扩大临时 descriptor 数组；无论成功失败都消耗并释放旧块所有权。 */
static void *realloc_pages(void *old_memmap, int old_shift)
/*
 * 将临时 runtime descriptor 缓冲区扩为原来的两倍。
 * old_memmap 可为 NULL，非 NULL 时必须是 order=old_shift 的 page allocator
 * 分配；old_shift 表示 2^order 个 PAGE_SIZE 页。成功返回 order+1 新块并
 * 复制旧块全部字节，同时释放旧块；首次分配直接返回新块。失败返回
 * NULL，但仍释放 old_memmap——这是“消耗输入所有权”的接口，调用者
 * 不得在失败后再次释放旧指针。
 */
{
	void *ret;

	ret = (void *)__get_free_pages(GFP_KERNEL, old_shift + 1);
	/* GFP_KERNEL 允许启动线程睡眠/reclaim；分配的是物理连续 2^(shift+1) 页。 */
	if (!ret)
		goto out;

	/*
	 * A first-time allocation doesn't have anything to copy.
	 */
	/* old_memmap==NULL 时新块尚无有效 descriptor，直接移交给调用者填充。 */
	if (!old_memmap)
		return ret;

	memcpy(ret, old_memmap, PAGE_SIZE << old_shift);
	/* 只复制旧容量，新扩出的一半稍后由 descriptor 逐项写入，无需清零。 */

out:
	/* free_pages(NULL/0) 不安全语义由这里的调用约定规避：首次失败 old 为 NULL。 */
	/*
	 * 【对上句的修正】free_pages() 明确检查 addr != 0，因此 old_memmap
	 * 为 NULL 时是安全空操作；这里无需依赖“首次分配不会失败”的假设。
	 */
	free_pages((unsigned long)old_memmap, old_shift);
	return ret;
}

/*
 * Iterate the EFI memory map in reverse order because the regions
 * will be mapped top-down. The end result is the same as if we had
 * mapped things forward, but doesn't require us to change the
 * existing implementation of efi_map_region().
 */
/*
 * 64 位路径的反向迭代器。entry 是上次返回的 descriptor，首次必须传
 * NULL；返回前一项，越过 map 起点返回 NULL。指针每次按固件 desc_size
 * 字节移动而非 sizeof。之所以倒序遍历，是 efi_map_region() 从高虚拟
 * 地址向下分配：倒序处理物理 map 最终可令原顺序中 entry N 的虚拟地址
 * 低于 N+1，满足新固件代码/数据相对引用要求，无需重写底层映射器。
 */
static inline void *efi_map_next_entry_reverse(void *entry)
{
	/* Initial call */
	/* 首次从 map_end 减一个 desc_size，取得最后一个有效 descriptor。 */
	if (!entry)
		return efi.memmap.map_end - efi.memmap.desc_size;

	entry -= efi.memmap.desc_size;
	if (entry < efi.memmap.map)
		return NULL;

	return entry;
}

/*
 * efi_map_next_entry - Return the next EFI memory map descriptor
 * @entry: Previous EFI memory map descriptor
 *
 * This is a helper function to iterate over the EFI memory map, which
 * we do in different orders depending on the current configuration.
 *
 * To begin traversing the memory map @entry must be %NULL.
 *
 * Returns %NULL when we reach the end of the memory map.
 */
/*
 * 根据固件/内核模式选择 descriptor 遍历方向。entry 的输入输出契约同
 * 上述英文说明，返回值是 efi.memmap 内部借用指针，调用者不可释放。
 * 64 位使用反向物理遍历配合 top-down VA 分配，32 位从头正向遍历；
 * 两种方式都只决定映射次序，不改变 map 中 descriptor 的存储顺序。
 */
static void *efi_map_next_entry(void *entry)
{
	if (efi_enabled(EFI_64BIT)) {
		/*
		 * Starting in UEFI v2.5 the EFI_PROPERTIES_TABLE
		 * config table feature requires us to map all entries
		 * in the same order as they appear in the EFI memory
		 * map. That is to say, entry N must have a lower
		 * virtual address than entry N+1. This is because the
		 * firmware toolchain leaves relative references in
		 * the code/data sections, which are split and become
		 * separate EFI memory regions. Mapping things
		 * out-of-order leads to the firmware accessing
		 * unmapped addresses.
		 *
		 * Since we need to map things this way whether or not
		 * the kernel actually makes use of
		 * EFI_PROPERTIES_TABLE, let's just switch to this
		 * scheme by default for 64-bit.
		 */
		/*
		 * 由于底层 VA 自顶向下，反向取 descriptor 才能让最终 VA 顺序
		 * 与固件 map 正向顺序一致，保住跨 code/data section 的相对引用。
		 */
		return efi_map_next_entry_reverse(entry);
		/*
		 * EFI_PROPERTIES_TABLE 的相对引用隐含“相邻 section 保持虚拟
		 * 顺序”。无条件采用该顺序可覆盖新旧固件，代价仅是迭代方向。
		 */
	}

	/* Initial call */
	/* 32 位正向迭代首次返回 map 起点。 */
	if (!entry)
		return efi.memmap.map;

	entry += efi.memmap.desc_size;
	if (entry >= efi.memmap.map_end)
		return NULL;

	return entry;
}

/* 依据 runtime、位数、mixed mode 和缺陷固件策略筛选专用页表区域。 */
static bool should_map_region(efi_memory_desc_t *md)
/*
 * 判定 descriptor 是否必须出现在 EFI 专用 runtime 页表中。
 * md 是当前 map 中可修改但本函数只读的借用项；返回 true 仅表示需要
 * 建映射，不代表该区可由 Linux 分配。规则优先级体现安全/兼容折中：
 * runtime 属性必映射；32 位除此之外不扩张；mixed mode 为参数的 1:1
 * 地址访问映射普通 RAM；64 位还保留 boot-services 区兼容违规固件。
 */
{
	/*
	 * Runtime regions always require runtime mappings (obviously).
	 */
	if (md->attribute & EFI_MEMORY_RUNTIME)
		/* 这是 UEFI SetVirtualAddressMap 契约要求，不能由类型替代。 */
		return true;

	/*
	 * 32-bit EFI doesn't suffer from the bug that requires us to
	 * reserve boot services regions, and mixed mode support
	 * doesn't exist for 32-bit kernels.
	 */
	if (IS_ENABLED(CONFIG_X86_32))
		/* 32 位没有 mixed-mode thunk 的全 RAM 1:1 参数需求。 */
		return false;

	/*
	 * EFI specific purpose memory may be reserved by default
	 * depending on kernel config and boot options.
	 */
	/*
	 * specific-purpose conventional memory若按策略 soft-reserve，就不应
	 * 仅为 EFI 调用扩大专用页表映射；真正 runtime 项已在首个分支命中。
	 */
	if (md->type == EFI_CONVENTIONAL_MEMORY &&
	    efi_soft_reserve_enabled() &&
	    (md->attribute & EFI_MEMORY_SP))
		return false;

	/*
	 * Map all of RAM so that we can access arguments in the 1:1
	 * mapping when making EFI runtime calls.
	 */
	if (efi_is_mixed()) {
		/* 64 位内核调用 32 位固件时，thunk 只能用低地址/恒等映射参数。 */
		if (md->type == EFI_CONVENTIONAL_MEMORY ||
		    md->type == EFI_LOADER_DATA ||
		    md->type == EFI_LOADER_CODE)
			return true;
	}

	/*
	 * Map boot services regions as a workaround for buggy
	 * firmware that accesses them even when they shouldn't.
	 *
	 * See efi_{reserve,free}_boot_services().
	 */
	/*
	 * 规范上 ExitBootServices 后固件不应访问这些区，但现实固件会违规；
	 * 暂时映射换取兼容性，成功 SVAM 后由 efi_unmap_boot_services() 清理。
	 */
	if (md->type == EFI_BOOT_SERVICES_CODE ||
	    md->type == EFI_BOOT_SERVICES_DATA)
		return true;

	return false;
}

/*
 * Map the efi memory ranges of the runtime services and update new_mmap with
 * virtual addresses.
 */
/* 建立选中区域的映射，并返回仅含这些 descriptor 的新连续数组。 */
static void * __init efi_map_regions(int *count, int *pg_shift)
/*
 * 为所有 should_map_region() 项建立 efi_pgd 映射，并构造只含这些项的
 * 新 descriptor 数组。
 *
 * count/pg_shift 都是不可为 NULL 的输入输出参数，调用时初值必须为 0；
 * count 返回已复制项数，pg_shift 返回当前分配块的 order+1 计数，供
 * efi_setup_page_tables() 计算缓冲区页数。成功返回由 page allocator
 * 拥有的新数组；失败返回 NULL，realloc_pages 已释放此前数组，但已经
 * 建立的 efi_pgd 映射由上层禁用 runtime 后随 init 资源统一处理。
 */
{
	/* left 是当前缓冲区尚可写字节数，desc_size 来自固件 map ABI。 */
	void *p, *new_memmap = NULL;
	unsigned long left = 0;
	unsigned long desc_size;
	efi_memory_desc_t *md;

	desc_size = efi.memmap.desc_size;

	p = NULL;
	while ((p = efi_map_next_entry(p))) {
		md = p;

		if (!should_map_region(md))
			continue;

		efi_map_region(md);
		/* helper 为该物理区选择 runtime VA，并把结果写入 md->virt_addr。 */

		if (left < desc_size) {
			/* 容量不足时按 1、2、4... 页增长，摊销重复复制成本。 */
			new_memmap = realloc_pages(new_memmap, *pg_shift);
			if (!new_memmap)
				return NULL;

			left += PAGE_SIZE << *pg_shift;
			(*pg_shift)++;
		}

		memcpy(new_memmap + (*count * desc_size), md, desc_size);
		/* void* 算术按字节是 GNU C 扩展，偏移必须乘固件 descriptor 步长。 */

		left -= desc_size;
		(*count)++;
	}

	return new_memmap;
}

/* kexec 路径按第一内核留下的固定 virt_addr 恢复 EFI runtime 页表。 */
static void __init kexec_enter_virtual_mode(void)
/*
 * kexec 第二内核恢复 EFI runtime 映射。
 *
 * 前一内核已调用过 SetVirtualAddressMap，而 UEFI 通常不允许再次任意选择
 * VA；setup_data 中 descriptor 的 virt_addr 因而是必须复用的固定值。
 * 本函数无入参/返回，以 EFI_RUNTIME_SERVICES flag 表示结果。mixed mode
 * 无法在此安全恢复，直接撤销 memmap 并降级；其他失败同样清能力位，
 * 不尝试回到物理 runtime 调用。成功后建立专用页表、native 调用接口
 * 并收紧映射权限。
 */
{
#ifdef CONFIG_KEXEC_CORE
	/* md 借用现有 map 项；num_pages 是 memmap 缓冲区向上取整后的页数。 */
	efi_memory_desc_t *md;
	unsigned int num_pages;

	/*
	 * We don't do virtual mode, since we don't do runtime services, on
	 * non-native EFI.
	 */
	/* mixed EFI 的 thunk 状态不能由第一内核可靠传给第二内核，直接降级。 */
	if (efi_is_mixed()) {
		efi_memmap_unmap();
		clear_bit(EFI_RUNTIME_SERVICES, &efi.flags);
		return;
	}

	if (efi_alloc_page_tables()) {
		/* 页表尚未发布，失败只需清能力位，无需向固件撤销任何状态。 */
		pr_err("Failed to allocate EFI page tables\n");
		clear_bit(EFI_RUNTIME_SERVICES, &efi.flags);
		return;
	}

	/*
	* Map efi regions which were passed via setup_data. The virt_addr is a
	* fixed addr which was used in first kernel of a kexec boot.
	*/
	/* 固定 VA 是固件已重定位后的 ABI，第二内核不得重新选择地址。 */
	for_each_efi_memory_desc(md)
		efi_map_region_fixed(md); /* FIXME: add error handling */
	/*
	 * fixed 使用 descriptor 内第一内核留下的 virt_addr。原 FIXME 表明
	 * 单项映射失败目前没有细粒度回滚，这是 kexec runtime 的已知代价。
	 */

	/*
	 * Unregister the early EFI memmap from efi_init() and install
	 * the new EFI memory map.
	 */
	/* early 与 late 映射不能同时发布；先注销旧虚址，再按保留物理页重映射。 */
	efi_memmap_unmap();

	if (efi_memmap_init_late(efi.memmap.phys_map,
				 efi.memmap.desc_size * efi.memmap.nr_map)) {
		pr_err("Failed to remap late EFI memory map\n");
		clear_bit(EFI_RUNTIME_SERVICES, &efi.flags);
		return;
	}

	num_pages = ALIGN(efi.memmap.nr_map * efi.memmap.desc_size, PAGE_SIZE);
	/* 先按页对齐字节长度，再右移换成页数，供页表映射 memmap 自身。 */
	num_pages >>= PAGE_SHIFT;

	if (efi_setup_page_tables(efi.memmap.phys_map, num_pages)) {
		clear_bit(EFI_RUNTIME_SERVICES, &efi.flags);
		return;
	}

	efi_sync_low_kernel_mappings();
	/* 把 runtime 调用所需的低端内核映射同步进 efi_pgd，随后发布调用桩。 */
	efi_native_runtime_setup();
	efi_runtime_update_mappings();
#endif
}

/*
 * This function will switch the EFI runtime services to virtual mode.
 * Essentially, we look through the EFI memmap and map every region that
 * has the runtime attribute bit set in its memory descriptor into the
 * efi_pgd page table.
 *
 * The new method does a pagetable switch in a preemption-safe manner
 * so that we're in a different address space when calling a runtime
 * function. For function arguments passing we do copy the PUDs of the
 * kernel page table into efi_pgd prior to each call.
 *
 * Specially for kexec boot, efi runtime maps in previous kernel should
 * be passed in via setup_data. In that case runtime ranges will be mapped
 * to the same virtual addresses as the first kernel, see
 * kexec_enter_virtual_mode().
 */
/*
 * 冷启动时把固件从物理地址模式一次性切换到虚拟地址模式。
 * 无入参/返回；成功的可观察结果是 firmware 接受新 map、boot-services
 * 映射被撤销、native/thunk runtime 操作发布且权限收紧。失败统一跳到
 * err 清 EFI_RUNTIME_SERVICES，调用者只能放弃 runtime 服务；已经对
 * 固件成功执行 SVAM 后的步骤按设计不应失败，不能再次回到物理模式。
 *
 * count 是新 map 项数，pg_shift 跟踪其分配 order，new_memmap 在转换后
 * 被 late memmap 接管；pa 是该数组物理地址，status 是 EFI 状态码。
 */
static void __init __efi_enter_virtual_mode(void)
{
	/* count/pg_shift 初值构成 efi_map_regions() 的输入协议。 */
	int count = 0, pg_shift = 0;
	void *new_memmap = NULL;
	efi_status_t status;
	unsigned long pa;

	if (efi_alloc_page_tables()) {
		pr_err("Failed to allocate EFI page tables\n");
		goto err;
	}

	efi_merge_regions();
	/* 先减少碎片项，再分配 VA，可降低页表和传给固件的 map 体积。 */
	new_memmap = efi_map_regions(&count, &pg_shift);
	if (!new_memmap) {
		pr_err("Error reallocating memory, EFI runtime non-functional!\n");
		goto err;
	}

	pa = __pa(new_memmap);
	/* new_memmap 来自直接映射的连续页，__pa 可安全取得固件可见物理地址。 */

	/*
	 * Unregister the early EFI memmap from efi_init() and install
	 * the new EFI memory map that we are about to pass to the
	 * firmware via SetVirtualAddressMap().
	 */
	/*
	 * 新 map 已完整构造后才撤销 early 视图，避免构造途中失去源数据；
	 * 后续 late remap 失败则没有安全恢复旧映射的事务接口，只能降级。
	 */
	efi_memmap_unmap();

	if (efi_memmap_init_late(pa, efi.memmap.desc_size * count)) {
		/* early map 已撤销；失败后不再有可遍历 map，只能禁用 runtime。 */
		pr_err("Failed to remap late EFI memory map\n");
		goto err;
	}

	if (efi_enabled(EFI_DBG)) {
		pr_info("EFI runtime memory map:\n");
		efi_print_memmap();
	}

	if (efi_setup_page_tables(pa, 1 << pg_shift))
		/* pg_shift 是增长后的指数，1<<pg_shift 为实际缓冲区页数。 */
		goto err;

	efi_sync_low_kernel_mappings();

	status = efi_set_virtual_address_map(efi.memmap.desc_size * count,
					     efi.memmap.desc_size,
					     efi.memmap.desc_version,
					     (efi_memory_desc_t *)pa,
					     efi_systab_phys);
	if (status != EFI_SUCCESS) {
		/* 固件未接受 VA 布局，不能安装任何 runtime 调用入口。 */
		pr_err("Unable to switch EFI into virtual mode (status=%lx)!\n",
		       status);
		goto err;
	}

	efi_check_for_embedded_firmwares();
	/* SVAM 后固件引用已完成重定位，此时再扫描/保存需长期使用的嵌入固件。 */
	efi_unmap_boot_services();
	/* 解除仅为缺陷固件过渡保留的 boot-service 映射，缩小长期可访问面。 */

	if (!efi_is_mixed())
		/* 位数匹配可直接调用固件函数指针；mixed mode 需 32 位 thunk。 */
		efi_native_runtime_setup();
	else
		efi_thunk_runtime_setup();

	/*
	 * Apply more restrictive page table mapping attributes now that
	 * SVAM() has been called and the firmware has performed all
	 * necessary relocation fixups for the new virtual addresses.
	 */
	efi_runtime_update_mappings();
	/* 把初始宽松的可写/可执行映射按 descriptor 属性拆成最小权限。 */

	/* clean DUMMY object */
	/* 删除 EFI 变量子系统用于探测固件能力的临时变量，避免留在 NVRAM。 */
	efi_delete_dummy_variable();
	return;

err:
	/* 能力位是运行期读者的发布门；清除后所有 EFI service wrapper 应拒绝调用。 */
	clear_bit(EFI_RUNTIME_SERVICES, &efi.flags);
}

/*
 * EFI 虚拟模式公共入口，由 x86 CPU 初始化在启动单线程阶段调用。
 * paravirt 平台由 hypervisor 代理，直接返回；否则先把物理 runtime table
 * 地址写入 efi.runtime，按 efi_setup 选择 kexec 恢复或冷启动 SVAM。
 * 无返回值，最终能力由 EFI_RUNTIME_SERVICES flag 表示；页表 dump 只做
 * 诊断，即使前面降级也可帮助定位映射失败。
 */
void __init efi_enter_virtual_mode(void)
{
	if (efi_enabled(EFI_PARAVIRT))
		return;

	efi.runtime = (efi_runtime_services_t *)efi_runtime;

	if (efi_setup)
		kexec_enter_virtual_mode();
	else
		__efi_enter_virtual_mode();

	efi_dump_pagetable();
}

/* 精确匹配已发现 EFI 表的物理首地址，供 ioremap 属性决策使用。 */
bool efi_is_table_address(unsigned long phys_addr)
/*
 * 判断物理地址是否恰好等于一个已知 EFI 表的起始地址。phys_addr 单位为
 * 字节，EFI_INVALID_TABLE_ADDR 永不匹配；返回 bool，无状态副作用。该函数
 * 被 x86 ioremap 属性选择路径调用，用于避免把固件表误当普通 MMIO。
 * 这里只比较表首地址，不判断地址是否落在表覆盖范围内。
 */
{
	unsigned int i;

	if (phys_addr == EFI_INVALID_TABLE_ADDR)
		return false;

	for (i = 0; i < ARRAY_SIZE(efi_tables); i++)
		if (*(efi_tables[i]) == phys_addr)
			return true;

	return false;
}

#define EFI_FIELD(var) efi_ ## var
/* 把 sysfs 属性短名拼接到本文件的 efi_* 地址变量。 */

/*
 * 生成三个只读 sysfs show 回调。kobj/attr 由 sysfs 传入但数值来自固定
 * 全局地址，buf 是至少 PAGE_SIZE 的输出缓冲；返回写入字符数。宏保证
 * fw_vendor/runtime/config_table 使用完全一致的十六进制 ABI，避免三份
 * getter 漂移。下面的反斜杠续行属于单个宏，不能在其内部插入独立注释。
 */
#define EFI_ATTR_SHOW(name) \
static ssize_t name##_show(struct kobject *kobj, \
				struct kobj_attribute *attr, char *buf) \
{ \
	return sprintf(buf, "0x%lx\n", EFI_FIELD(name)); \
}

EFI_ATTR_SHOW(fw_vendor);
EFI_ATTR_SHOW(runtime);
EFI_ATTR_SHOW(config_table);

struct kobj_attribute efi_attr_fw_vendor = __ATTR_RO(fw_vendor);
/* 三个 kobj_attribute 把生成的 show 回调发布为默认只读 sysfs 文件。 */
struct kobj_attribute efi_attr_runtime = __ATTR_RO(runtime);
struct kobj_attribute efi_attr_config_table = __ATTR_RO(config_table);

/* 按 EFI 初始化结果返回 sysfs mode；0 表示该地址属性应完全隐藏。 */
umode_t efi_attr_is_visible(struct kobject *kobj, struct attribute *attr, int n)
/*
 * sysfs 属性组的可见性过滤器。kobj/n 是通用回调参数，此实现不使用；
 * attr 必须是组内属性。返回 0 表示隐藏无意义/不可访问的地址，其他情况
 * 返回属性原 mode。paravirt 隐藏 fw_vendor 物理地址，因为该地址不代表
 * 本机可直接访问固件；runtime/config 未发现时也不向用户空间暴露哨兵。
 */
{
	if (attr == &efi_attr_fw_vendor.attr) {
		if (efi_enabled(EFI_PARAVIRT) ||
				efi_fw_vendor == EFI_INVALID_TABLE_ADDR)
			return 0;
	} else if (attr == &efi_attr_runtime.attr) {
		if (efi_runtime == EFI_INVALID_TABLE_ADDR)
			return 0;
	} else if (attr == &efi_attr_config_table.attr) {
		if (efi_config_table == EFI_INVALID_TABLE_ADDR)
			return 0;
	}
	return attr->mode;
}

/* 返回 boot_params 中启动加载器记录的 Secure Boot 状态，不重新访问固件。 */
enum efi_secureboot_mode __x86_efi_boot_mode(void)
/*
 * 返回 boot loader 写入 boot_params 的 x86 Secure Boot 状态枚举。无入参、
 * 无副作用；值在启动后只读，供通用 EFI 安全策略判断 enabled/disabled/
 * unknown。这里不重新查询固件，也不验证签名，只暴露已建立的启动事实。
 */
{
	return boot_params.secure_boot;
}
