// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM32 EFI 初始化、运行时页表映射与固件入口状态诊断学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 32 位 ARM 不能直接沿用内核 init_mm 调用 EFI Runtime Services：通用 EFI
 * 运行时代码先为 efi_mm 分配独立 PGD，再遍历 EFI_MEMORY_RUNTIME 描述符调用
 * efi_create_mapping() 建立 VA->PA 映射；调用固件前由 efi_virtmap_load() 切换
 * 到该 mm，返回后恢复正常上下文。映射先按 cache 类型建立，随后尽可能依据
 * EFI_MEMORY_RO/XP 收紧为只读或不可执行。
 *
 * ARM 的 section mapping 无法由 apply_to_page_range() 原地拆成 PTE，因此权限
 * 收紧只作用于完全由页映射表示的描述符。这保留 section 带来的较低页表/TLB
 * 成本，但代价是含完整、自然对齐 section 的区域不能在这里细化 RO/XN。
 * 所有建表函数均为 __init：efi_mm 在启动期一次性构造，运行期只切换使用。
 *
 * 另一条独立链路是 setup_arch()->arm_efi_init()->efi_init()：解析 EFI 配置表，
 * 撤销不能跨 paging_init 保留的 early memmap，再读取 EFI stub 发布的 CPU 状态表，
 * 检查固件在进入 stub 和 ExitBootServices() 返回时是否保持 MMU 开启。
 */
/*
 * Copyright (C) 2015 Linaro Ltd <ard.biesheuvel@linaro.org>
 */

#include <linux/efi.h>
#include <linux/memblock.h>
#include <linux/screen_info.h>

#include <asm/efi.h>
#include <asm/mach/map.h>
#include <asm/mmu_context.h>

/*
 * apply_to_page_range() 的 PTE 回调。
 *
 * ptep 指向 efi_mm 中 addr 对应的基础页表项；addr 仅用于统一回调 ABI；data
 * 必须是覆盖该范围的 EFI memory descriptor。函数按描述符的 RO/XP 属性只增
 * 不减权限限制，并以 PTE_EXT_NG 写回 non-global 项，使 EFI 专用地址空间的
 * TLB 翻译不会跨上下文泄漏。返回 0 表示继续遍历；不分配、不可睡眠、无失败态。
 */
static int __init set_permissions(pte_t *ptep, unsigned long addr, void *data)
{
	efi_memory_desc_t *md = data;
	pte_t pte = *ptep;

	/* EFI_MEMORY_RO 去掉普通写权限；EFI_MEMORY_XP 禁止从该固件区域取指。 */
	if (md->attribute & EFI_MEMORY_RO)
		pte = set_pte_bit(pte, __pgprot(L_PTE_RDONLY));
	if (md->attribute & EFI_MEMORY_XP)
		pte = set_pte_bit(pte, __pgprot(L_PTE_XN));
	/* set_pte_ext() 负责 ARM PTE 编码和必要发布，PTE_EXT_NG 固定上下文私有语义。 */
	set_pte_ext(ptep, pte, PTE_EXT_NG);
	return 0;
}

/*
 * 尝试收紧一个 EFI 描述符的现有映射。
 *
 * mm 是已经由 efi_create_mapping() 填充的 EFI 地址空间；md 的 virt_addr 是
 * 字节 VA，num_pages 以 EFI_PAGE_SIZE 为单位；ignored 是通用 memattr setter
 * ABI 的附加布尔参数，ARM32 不使用。返回 apply_to_page_range() 的 0/错误码；
 * 若区间包含至少一个完整、自然对齐的 SECTION_SIZE 块，则映射可能含 section
 * descriptor，函数保守返回 0 且不改变权限，因为 PTE walker 不能处理块叶子。
 */
int __init efi_set_mapping_permissions(struct mm_struct *mm,
				       efi_memory_desc_t *md,
				       bool ignored)
{
	unsigned long base, size;

	base = md->virt_addr;
	size = md->num_pages << EFI_PAGE_SHIFT;

	/*
	 * We can only use apply_to_page_range() if we can guarantee that the
	 * entire region was mapped using pages. This should be the case if the
	 * region does not cover any naturally aligned SECTION_SIZE sized
	 * blocks.
	 */
	/*
	 * round_up(base) 是首个完整 section 的起点；若 round_down(end) 尚未到它的
	 * 下一 section 边界，区间中就不存在完整 section，create_mapping_late()
	 * 只能用 PTE 表示，可安全逐页改权限。否则保持原映射，避免把 section 当表。
	 */
	if (round_down(base + size, SECTION_SIZE) <
	    round_up(base, SECTION_SIZE) + SECTION_SIZE)
		return apply_to_page_range(mm, base, size, set_permissions, md);

	return 0;
}

/*
 * 把一个 EFI_RUNTIME 描述符映入专用 mm。
 *
 * 输入 md 同时给出固件指定 VA、物理页首址、页数和 cache/权限属性；mm 通常是
 * drivers/firmware/efi/arm-runtime.c 中的 efi_mm。函数先选择 ARM map_desc 内存
 * 类型，再由 create_mapping_late() 分配缺失页表并建立 non-global 映射；若有
 * RO/XP，再调用 efi_set_mapping_permissions() 收紧基础页映射。返回 0 或权限
 * walker 错误；建表 helper 本身无错误返回，因此失败时可能已有映射，调用者会
 * 放弃启用 Runtime Services 并整体丢弃该初始化结果。
 */
int __init efi_create_mapping(struct mm_struct *mm, efi_memory_desc_t *md)
{
	struct map_desc desc = {
		.virtual	= md->virt_addr,
		.pfn		= __phys_to_pfn(md->phys_addr),
		.length		= md->num_pages * EFI_PAGE_SIZE,
	};

	/*
	 * Order is important here: memory regions may have all of the
	 * bits below set (and usually do), so we check them in order of
	 * preference.
	 */
	/*
	 * EFI 属性可同时置位，故必须从语义最适合普通 RAM 的 WB 开始匹配，再退到
	 * WT、WC，最后按强顺序 Device。若反序，常见 WB|WT 描述符会被误映成较慢、
	 * 甚至别名属性不一致的设备/非缓存内存。
	 */
	if (md->attribute & EFI_MEMORY_WB)
		desc.type = MT_MEMORY_RWX;
	else if (md->attribute & EFI_MEMORY_WT)
		desc.type = MT_MEMORY_RWX_NONCACHED;
	else if (md->attribute & EFI_MEMORY_WC)
		desc.type = MT_DEVICE_WC;
	else
		desc.type = MT_DEVICE;

	/* true 要求叶子 non-global；页表页由 late allocator 分配并归 efi_mm 所有。 */
	create_mapping_late(mm, &desc, true);

	/*
	 * If stricter permissions were specified, apply them now.
	 */
	/* 初始 type 为可读写/可执行兼容映射，只有固件明确声明时才追加 RO/XN。 */
	if (md->attribute & (EFI_MEMORY_RO | EFI_MEMORY_XP))
		return efi_set_mapping_permissions(mm, md, false);
	return 0;
}

/* EFI 配置表解析器写入的 CPU 状态表物理地址；只在启动期诊断阶段有效。 */
static unsigned long __initdata cpu_state_table = EFI_INVALID_TABLE_ADDR;

/* GUID->接收槽映射；efi_init() 扫描系统配置表时把匹配表地址写入上面的变量。 */
const efi_config_table_type_t efi_arch_tables[] __initconst = {
	{LINUX_EFI_ARM_CPU_STATE_TABLE_GUID, &cpu_state_table},
	{}
};

/*
 * 读取 EFI stub 留下的进入/退出 Boot Services CPU 控制状态。
 *
 * 表不存在时静默返回；存在时用 early_memremap_ro() 临时映射只读结构，映射
 * 失败只告警。SCTLR.M 为 0 表示 MMU 被固件关闭，违反 ARM EFI 启动约定；异常
 * 时或 EFI_DBG 开启时输出前后 CPSR/SCTLR，最后无条件撤销成功的临时映射。
 * 函数只产生诊断日志，不保留 state 指针，也不改变 CPU 寄存器。
 */
static void __init load_cpu_state_table(void)
{
	if (cpu_state_table != EFI_INVALID_TABLE_ADDR) {
		struct efi_arm_entry_state *state;
		bool dump_state = true;

		state = early_memremap_ro(cpu_state_table,
					  sizeof(struct efi_arm_entry_state));
		if (state == NULL) {
			pr_warn("Unable to map CPU entry state table.\n");
			return;
		}

		/*
		 * 代码实际只检查 SCTLR bit 0（M/MMU enable）。上游告警文本同时提到
		 * D-cache，但这里没有检查 SCTLR.C；因此能直接判定的是 MMU 状态。
		 */
		if ((state->sctlr_before_ebs & 1) == 0)
			pr_warn(FW_BUG "EFI stub was entered with MMU and Dcache disabled, please fix your firmware!\n");
		else if ((state->sctlr_after_ebs & 1) == 0)
			pr_warn(FW_BUG "ExitBootServices() returned with MMU and Dcache disabled, please fix your firmware!\n");
		else
			dump_state = false;

		/* 正常状态只在 EFI 调试模式打印，避免每次启动产生无用寄存器日志。 */
		if (dump_state || efi_enabled(EFI_DBG)) {
			pr_info("CPSR at EFI stub entry        : 0x%08x\n",
				state->cpsr_before_ebs);
			pr_info("SCTLR at EFI stub entry       : 0x%08x\n",
				state->sctlr_before_ebs);
			pr_info("CPSR after ExitBootServices() : 0x%08x\n",
				state->cpsr_after_ebs);
			pr_info("SCTLR after ExitBootServices(): 0x%08x\n",
				state->sctlr_after_ebs);
		}
		early_memunmap(state, sizeof(struct efi_arm_entry_state));
	}
}

/*
 * setup_arch() 中的 ARM EFI 总入口。
 *
 * efi_init() 解析系统表、内存图和 efi_arch_tables；随后立即撤销 EFI early
 * memmap，因为 ARM paging_init() 会重建页表，旧映射既不能安全继承也不应留下
 * 悬空 VA。最后按保存的物理地址独立映射 CPU 状态表完成固件契约诊断。
 * 全流程仅启动 CPU 调用、允许 early mapping 操作、无返回值；Runtime Services
 * 是否启用由后续 arm-runtime 初始化依据独立 efi_mm 建表结果决定。
 */
void __init arm_efi_init(void)
{
	efi_init();

	/* ARM does not permit early mappings to persist across paging_init() */
	/* 这里只撤销临时 VA，EFI memmap 的物理地址/元数据仍保留供 late remap 使用。 */
	efi_memmap_unmap();

	load_cpu_state_table();
}
