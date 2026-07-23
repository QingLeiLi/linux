// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 物理内存边界、memblock、DMA zone 与早期内存生命周期学习导读。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 固件先给出离散物理 RAM，arm64_memblock_init() 将其裁剪到 CPU PA 位宽
 * 和内核 linear-map VA 窗口，选择 memstart_addr，并把 kernel/initrd/
 * reserved-memory 从早期分配中保留。bootmem_init() 随后确定 PFN/NUMA、
 * KVM、DMA/CMA/crashkernel；mem_init() 才发布伙伴分配器可用。
 *
 * memblock 在启动单 CPU 阶段串行修改；标为 __ro_after_init 的边界在启动
 * 完成后冻结，运行期转换公式依赖它们不变。地址有物理字节、PFN、linear
 * VA 三种表示，所有裁剪/对齐必须明确单位。free_initmem 最后释放物理页
 * 并撤销镜像 VA 映射，但保留 VA 洞以满足 kallsyms/module 布局约束。
 */
/*
 * Based on arch/arm/mm/init.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/errno.h>
#include <linux/swap.h>
#include <linux/init.h>
#include <linux/cache.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/initrd.h>
#include <linux/gfp.h>
#include <linux/math.h>
#include <linux/memblock.h>
#include <linux/sort.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/efi.h>
#include <linux/swiotlb.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/kexec.h>
#include <linux/crash_dump.h>
#include <linux/hugetlb.h>
#include <linux/acpi_iort.h>
#include <linux/kmemleak.h>
#include <linux/execmem.h>

#include <asm/boot.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/kvm_host.h>
#include <asm/memory.h>
#include <asm/numa.h>
#include <asm/rsi.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <linux/sizes.h>
#include <asm/tlb.h>
#include <asm/alternative.h>
#include <asm/xen/swiotlb-xen.h>

/*
 * We need to be able to catch inadvertent references to memstart_addr
 * that occur (potentially in generic code) before arm64_memblock_init()
 * executes, which assigns it its actual value. So use a default value
 * that cannot be mistaken for a real physical address.
 */
/*
 * linear-map 换算的物理基准。初值 -1 让过早使用立即产生明显错误而非映射
 * 到貌似合法的 PA 0；memblock 初始化选定后冻结并导出给架构模块。
 */
s64 memstart_addr __ro_after_init = -1;
EXPORT_SYMBOL(memstart_addr);

/*
 * If the corresponding config options are enabled, we create both ZONE_DMA
 * and ZONE_DMA32. By default ZONE_DMA covers the 32-bit addressable memory
 * unless restricted on specific platforms (e.g. 30-bit on Raspberry Pi 4).
 * In such case, ZONE_DMA32 covers the rest of the 32-bit addressable memory,
 * otherwise it is empty.
 */
/* DMA/ZONE_DMA 可直接寻址的物理上界（exclusive），由固件约束与 RAM 末端共同决定。 */
phys_addr_t __ro_after_init arm64_dma_phys_limit;

/*
 * To make optimal use of block mappings when laying out the linear
 * mapping, round down the base of physical memory to a size that can
 * be mapped efficiently, i.e., either PUD_SIZE (4k granule) or PMD_SIZE
 * (64k granule), or a multiple that can be mapped using contiguous bits
 * in the page tables: 32 * PMD_SIZE (16k granule)
 */
/* 对齐 memstart 可让绝大多数 linear map 使用 block/contiguous 项，节省页表和 TLB。 */
#if defined(CONFIG_ARM64_4K_PAGES)
#define ARM64_MEMSTART_SHIFT		PUD_SHIFT
#elif defined(CONFIG_ARM64_16K_PAGES)
#define ARM64_MEMSTART_SHIFT		CONT_PMD_SHIFT
#else
#define ARM64_MEMSTART_SHIFT		PMD_SHIFT
#endif
/* 不同 granule 选择可高效 block/contiguous 映射的 memstart 对齐粒度。 */

/*
 * sparsemem vmemmap imposes an additional requirement on the alignment of
 * memstart_addr, due to the fact that the base of the vmemmap region
 * has a direct correspondence, and needs to appear sufficiently aligned
 * in the virtual address space.
 */
/* vmemmap 的 PFN->struct page 直接换算要求物理基准至少按 section 粒度稳定对齐。 */
#if ARM64_MEMSTART_SHIFT < SECTION_SIZE_BITS
#define ARM64_MEMSTART_ALIGN	(1UL << SECTION_SIZE_BITS)
#else
#define ARM64_MEMSTART_ALIGN	(1UL << ARM64_MEMSTART_SHIFT)
#endif
/* 最终对齐还必须满足 sparsemem section->vmemmap 的直接对应关系。 */

/*
 * 解析 crashkernel 命令行并通过通用 helper 预留 crash capture kernel 内存。
 * 无配置/无有效参数时空操作；crash_size/base/low_size 单位字节，high 表示
 * 可放高端。预留必须早于标准资源树发布，失败由通用解析/日志策略处理。
 */
static void __init arch_reserve_crashkernel(void)
{
	unsigned long long low_size = 0;
	unsigned long long crash_base, crash_size;
	bool high = false;
	int ret;

	if (!IS_ENABLED(CONFIG_CRASH_RESERVE))
		return;

	ret = parse_crashkernel(boot_command_line, memblock_phys_mem_size(),
				&crash_size, &crash_base,
				&low_size, NULL, &high);
	if (ret)
		return;

	reserve_crashkernel_generic(crash_size, crash_base, low_size, high);
}

/* 把 zone 物理上限裁到实际 DRAM 的 exclusive 末端，避免减一时越界。 */
static phys_addr_t __init max_zone_phys(phys_addr_t zone_limit)
{
	return min(zone_limit, memblock_end_of_DRAM() - 1) + 1;
}

/*
 * 向通用 page allocator 输出各 zone 的最大 PFN（exclusive）。数组由调用者
 * 提供；DMA/DMA32 按构建配置填充，NORMAL 总到 max_pfn。只读全局边界。
 */
void __init arch_zone_limits_init(unsigned long *max_zone_pfns)
{
	phys_addr_t __maybe_unused dma32_phys_limit =
		max_zone_phys(DMA_BIT_MASK(32));

#ifdef CONFIG_ZONE_DMA
	max_zone_pfns[ZONE_DMA] = PFN_DOWN(max_zone_phys(zone_dma_limit));
#endif
#ifdef CONFIG_ZONE_DMA32
	max_zone_pfns[ZONE_DMA32] = PFN_DOWN(dma32_phys_limit);
#endif
	max_zone_pfns[ZONE_NORMAL] = max_pfn;
}

/*
 * 综合 ACPI IORT、DT dma-ranges、32-bit 设备传统约束与 RAM 末端，确定
 * zone_dma_limit/arm64_dma_phys_limit。没有 DMA zone 时退到整个 PHYS_MASK
 * 可寻址范围。启动串行，无返回。
 */
static void __init dma_limits_init(void)
{
	phys_addr_t __maybe_unused acpi_zone_dma_limit;
	phys_addr_t __maybe_unused dt_zone_dma_limit;
	phys_addr_t __maybe_unused dma32_phys_limit =
		max_zone_phys(DMA_BIT_MASK(32));

#ifdef CONFIG_ZONE_DMA
	acpi_zone_dma_limit = acpi_iort_dma_get_max_cpu_address();
	dt_zone_dma_limit = of_dma_get_max_cpu_address(NULL);
	zone_dma_limit = min(dt_zone_dma_limit, acpi_zone_dma_limit);
	/*
	 * Information we get from firmware (e.g. DT dma-ranges) describe DMA
	 * bus constraints. Devices using DMA might have their own limitations.
	 * Some of them rely on DMA zone in low 32-bit memory. Keep low RAM
	 * DMA zone on platforms that have RAM there.
	 */
	/* 固件描述总线窗口，不涵盖所有设备自身 mask，低 4GiB RAM 仍需保守 DMA zone。 */
	if (memblock_start_of_DRAM() < U32_MAX)
		zone_dma_limit = min(zone_dma_limit, U32_MAX);
	arm64_dma_phys_limit = max_zone_phys(zone_dma_limit);
#endif
#ifdef CONFIG_ZONE_DMA32
	if (!arm64_dma_phys_limit)
		arm64_dma_phys_limit = dma32_phys_limit;
#endif
	if (!arm64_dma_phys_limit)
		arm64_dma_phys_limit = PHYS_MASK + 1;
}

/*
 * 判断 pfn 是否对应 linear map 可访问的 memblock memory。先 PFN->PA->PFN
 * 回验防止超宽 bogus PFN 在移位时截断产生假阳性；返回 1/0，不取得 page 引用。
 */
int pfn_is_map_memory(unsigned long pfn)
{
	phys_addr_t addr = PFN_PHYS(pfn);

	/* avoid false positives for bogus PFNs, see comment in pfn_valid() */
	/* 超出 PA 位宽的 pfn 左移会截断；反向换算不相等时必须在查 memblock 前拒绝。 */
	if (PHYS_PFN(addr) != pfn)
		return 0;

	return memblock_is_map_memory(addr);
}
EXPORT_SYMBOL(pfn_is_map_memory);

/* mem= 启动参数裁剪的物理内存字节数/上界；默认不限制，init 后冻结。 */
static phys_addr_t memory_limit __ro_after_init = PHYS_ADDR_MAX;

/*
 * Limit the memory size that was specified via FDT.
 */
/* 解析 mem=<size>，向下页对齐后发布 memory_limit；缺参数返回 1，成功 0。 */
static int __init early_mem(char *p)
{
	if (!p)
		return 1;

	memory_limit = memparse(p, &p) & PAGE_MASK;
	pr_notice("Memory limited to %lldMB\n", memory_limit >> 20);

	return 0;
}
early_param("mem", early_mem);

/*
 * arm64 早期 RAM 布局主入口。无入参/返回；对 memblock.memory 原地删除 CPU
 * PA 位宽、linear VA 窗口和 mem= 限制之外区间，选择对齐 memstart_addr，
 * 必要时重新加入 kernel/initrd，最后保留镜像并扫描 DT reserved-memory。
 * 成功后所有保留 RAM 均能由 __phys_to_virt 覆盖。
 */
void __init arm64_memblock_init(void)
{
	s64 linear_region_size = PAGE_END - _PAGE_OFFSET(vabits_actual);

	/*
	 * Corner case: 52-bit VA capable systems running KVM in nVHE mode may
	 * be limited in their ability to support a linear map that exceeds 51
	 * bits of VA space, depending on the placement of the ID map. Given
	 * that the placement of the ID map may be randomized, let's simply
	 * limit the kernel's linear map to 51 bits as well if we detect this
	 * configuration.
	 */
	/* nVHE hyp 与 host 的 idmap/linear VA 约束交叠，51-bit cap 换取所有随机布局可用。 */
	if (IS_ENABLED(CONFIG_KVM) && vabits_actual == 52 &&
	    is_hyp_mode_available() && !is_kernel_in_hyp_mode()) {
		pr_info("Capping linear region to 51 bits for KVM in nVHE mode on LVA capable hardware.\n");
		linear_region_size = min_t(u64, linear_region_size, BIT(51));
	}

	/* Remove memory above our supported physical address size */
	/* 从 2^PHYS_MASK_SHIFT 起删除，防止页表 OA 字段截断高 PA。 */
	memblock_remove(1ULL << PHYS_MASK_SHIFT, ULLONG_MAX);

	/*
	 * Select a suitable value for the base of physical memory.
	 */
	/* 向下对齐可让 linear map 起始尽量使用大 block，减少页表页/TLB 项。 */
	memstart_addr = round_down(memblock_start_of_DRAM(),
				   ARM64_MEMSTART_ALIGN);

	if ((memblock_end_of_DRAM() - memstart_addr) > linear_region_size)
		pr_warn("Memory doesn't fit in the linear mapping, VA_BITS too small\n");

	/*
	 * Remove the memory that we will not be able to cover with the
	 * linear mapping. Take care not to clip the kernel which may be
	 * high in memory.
	 */
	/* 上端删除界限至少覆盖 _end；内核自身即使超窗口候选也不能被 memblock 丢弃。 */
	memblock_remove(max_t(u64, memstart_addr + linear_region_size,
			__pa_symbol(_end)), ULLONG_MAX);
	if (memstart_addr + linear_region_size < memblock_end_of_DRAM()) {
		/* RAM 高端必须保留时向上移动窗口基准，再删除低端无法覆盖部分。 */
		/* ensure that memstart_addr remains sufficiently aligned */
		/* 上移窗口仍按 ARM64_MEMSTART_ALIGN 取整，不能用恰好覆盖 RAM 的任意基址。 */
		memstart_addr = round_up(memblock_end_of_DRAM() - linear_region_size,
					 ARM64_MEMSTART_ALIGN);
		memblock_remove(0, memstart_addr);
	}

	/*
	 * If we are running with a 52-bit kernel VA config on a system that
	 * does not support it, we have to place the available physical
	 * memory in the 48-bit addressable part of the linear region, i.e.,
	 * we have to move it upward. Since memstart_addr represents the
	 * physical address of PAGE_OFFSET, we have to *subtract* from it.
	 */
	/* 52-bit 构建跑 48-bit CPU 时，把 RAM 放入 TTBR1 可实际寻址的高端子窗口。 */
	if (IS_ENABLED(CONFIG_ARM64_VA_BITS_52) && (vabits_actual != 52))
		memstart_addr -= _PAGE_OFFSET(vabits_actual) - _PAGE_OFFSET(52);

	/*
	 * Apply the memory limit if it was set. Since the kernel may be loaded
	 * high up in memory, add back the kernel region that must be accessible
	 * via the linear mapping.
	 */
	/* mem= 是用户策略，但执行中的 kernel text/data 必须重新加入以维持启动。 */
	if (memory_limit != PHYS_ADDR_MAX) {
		memblock_mem_limit_remove_map(memory_limit);
		memblock_add(__pa_symbol(_text), (resource_size_t)(_end - _text));
	}

	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
		/*
		 * Add back the memory we just removed if it results in the
		 * initrd to become inaccessible via the linear mapping.
		 * Otherwise, this is a no-op
		 */
		/* initrd 可能位于 mem= 裁掉区；按页扩区间后尝试恢复。 */
		phys_addr_t base = phys_initrd_start & PAGE_MASK;
		resource_size_t size = PAGE_ALIGN(phys_initrd_start + phys_initrd_size) - base;

		/*
		 * We can only add back the initrd memory if we don't end up
		 * with more memory than we can address via the linear mapping.
		 * It is up to the bootloader to position the kernel and the
		 * initrd reasonably close to each other (i.e., within 32 GB of
		 * each other) so that all granule/#levels combinations can
		 * always access both.
		 */
		/* 若 kernel/initrd 跨距超过 linear window，只能禁用 initrd 而非建立不可达 VA。 */
		if (WARN(base < memblock_start_of_DRAM() ||
			 base + size > memblock_start_of_DRAM() +
				       linear_region_size,
			"initrd not fully accessible via the linear mapping -- please check your bootloader ...\n")) {
			phys_initrd_size = 0;
		} else {
			memblock_add(base, size);
			memblock_clear_nomap(base, size);
			memblock_reserve(base, size);
		}
	}

	/*
	 * Register the kernel text, kernel data, initrd, and initial
	 * pagetables with memblock.
	 */
	/* reserve 防止后续 memblock/CMA 把正在执行的镜像和启动表重新分配。 */
	memblock_reserve(__pa_symbol(_text), _end - _text);
	if (IS_ENABLED(CONFIG_BLK_DEV_INITRD) && phys_initrd_size) {
		/* the generic initrd code expects virtual addresses */
		/* 此时已证明 initrd 在 linear map，物理地址可安全转长期内核 VA。 */
		initrd_start = __phys_to_virt(phys_initrd_start);
		initrd_end = initrd_start + phys_initrd_size;
	}

	early_init_fdt_scan_reserved_mem();
}

/*
 * 在 memblock 裁剪后建立最终 PFN/NUMA/DMA/CMA/crashkernel 启动状态。先做
 * early memtest，再发布 min/max PFN；KVM hyp 和 DMA limit 必须早于 CMA，
 * crashkernel 必须早于资源树。无返回，严重子系统失败由各 helper panic/降级。
 */
void __init bootmem_init(void)
{
	unsigned long min, max;

	min = PFN_UP(memblock_start_of_DRAM());
	max = PFN_DOWN(memblock_end_of_DRAM());

	early_memtest(min << PAGE_SHIFT, max << PAGE_SHIFT);

	max_pfn = max_low_pfn = max;
	min_low_pfn = min;

	arch_numa_init();

	kvm_hyp_reserve();
	dma_limits_init();

	/*
	 * Reserve the CMA area after arm64_dma_phys_limit was initialised.
	 */
	/* CMA 选址依赖设备可达上界，顺序倒置可能预留设备无法 DMA 的高端页。 */
	dma_contiguous_reserve(arm64_dma_phys_limit);

	/*
	 * request_standard_resources() depends on crashkernel's memory being
	 * reserved, so do it here.
	 */
	/* 先完成 crashkernel 预留，稍后资源树登记才会把该区正确标为不可分配资源。 */
	arch_reserve_crashkernel();

	memblock_dump_all();
}

/* 把静态 empty_zero_page 的镜像物理地址转换为全局 struct page。 */
void __init arch_setup_zero_pages(void)
{
	__zero_page = phys_to_page(__pa_symbol(empty_zero_page));
}

/*
 * 伙伴分配器前配置 SWIOTLB 与页表布局编译期约束。RAM 超 DMA limit 或 Realm
 * 强制 bounce；小系统仅为 unaligned kmalloc bounce 缩小 buffer。函数还在
 * 极小大页系统默认开启 overcommit，避免页粒度导致可用内存无法启动。
 */
void __init arch_mm_preinit(void)
{
	unsigned int flags = SWIOTLB_VERBOSE;
	bool swiotlb = max_pfn > PFN_DOWN(arm64_dma_phys_limit);

	if (is_realm_world()) {
		swiotlb = true;
		flags |= SWIOTLB_FORCE;
	}

	if (IS_ENABLED(CONFIG_DMA_BOUNCE_UNALIGNED_KMALLOC) && !swiotlb) {
		/*
		 * If no bouncing needed for ZONE_DMA, reduce the swiotlb
		 * buffer for kmalloc() bouncing to 1MB per 1GB of RAM.
		 */
		/* 只为对齐 bounce 服务时按 RAM/1024 缩放，避免默认大池浪费低内存。 */
		unsigned long size =
			DIV_ROUND_UP(memblock_phys_mem_size(), 1024);
		swiotlb_adjust_size(min(swiotlb_size_or_default(), size));
		swiotlb = true;
	}

	swiotlb_init(swiotlb, flags);

	/*
	 * Check boundaries twice: Some fundamental inconsistencies can be
	 * detected at build time already.
	 */
	/* 下列 BUILD_BUG_ON 验证用户窗口与配置页表级数的静态一致性。 */
#ifdef CONFIG_COMPAT
	BUILD_BUG_ON(TASK_SIZE_32 > DEFAULT_MAP_WINDOW_64);
#endif

	/*
	 * Selected page table levels should match when derived from
	 * scratch using the virtual address range and page size.
	 */
	/* 编译配置的层数必须等于 VA 位宽和 granule 推导值，否则页表宏会索引错层。 */
	BUILD_BUG_ON(ARM64_HW_PGTABLE_LEVELS(CONFIG_ARM64_VA_BITS) !=
		     CONFIG_PGTABLE_LEVELS);

	if (PAGE_SIZE >= 16384 && get_num_physpages() <= 128) {
		extern int sysctl_overcommit_memory;
		/*
		 * On a machine this small we won't get anywhere without
		 * overcommit, so turn it on by default.
		 */
		/* 仅极小机器调整默认策略，用户仍可通过 sysctl 覆盖。 */
		sysctl_overcommit_memory = OVERCOMMIT_ALWAYS;
	}
}

/* true 表示伙伴分配器已进入可用阶段；init 后只读供早期架构代码选择 allocator。 */
bool page_alloc_available __ro_after_init;

/* 发布 page allocator 可用，并让 SWIOTLB 根据最终 direct-map 属性更新缓冲区。 */
void __init mem_init(void)
{
	page_alloc_available = true;
	swiotlb_update_mem_attributes();
}

/*
 * 释放内核 __init 物理页并撤销其镜像虚拟映射。lm_alias 取得线性别名交给
 * free_reserved_area，填 poison 后归伙伴；随后 vunmap 原 kernel image VA，
 * 但保留 VA 区域不供模块复用，避免 kallsyms 对地址归属产生歧义。
 */
void free_initmem(void)
{
	void *lm_init_begin = lm_alias(__init_begin);
	void *lm_init_end = lm_alias(__init_end);

	WARN_ON(!IS_ALIGNED((unsigned long)lm_init_begin, PAGE_SIZE));
	WARN_ON(!IS_ALIGNED((unsigned long)lm_init_end, PAGE_SIZE));

	free_reserved_area(lm_init_begin, lm_init_end,
			   POISON_FREE_INITMEM, "unused kernel");
	/*
	 * Unmap the __init region but leave the VM area in place. This
	 * prevents the region from being reused for kernel modules, which
	 * is not supported by kallsyms.
	 */
	/* 先释放 linear alias 对应页，再撤销 image alias；两者指向相同物理区。 */
	vunmap_range((u64)__init_begin, (u64)__init_end);
}

/* panic/oops 诊断打印 mem= 限制，默认状态明确输出 none。 */
void dump_mem_limit(void)
{
	if (memory_limit != PHYS_ADDR_MAX) {
		pr_emerg("Memory Limit: %llu MB\n", memory_limit >> 20);
	} else {
		pr_emerg("Memory Limit: none\n");
	}
}

#ifdef CONFIG_EXECMEM
/* 模块直接分支窗口和需 PLT 的后备窗口基址；module_init_limits 后冻结。 */
static u64 module_direct_base __ro_after_init = 0;
static u64 module_plt_base __ro_after_init = 0;

/*
 * Choose a random page-aligned base address for a window of 'size' bytes which
 * entirely contains the interval [start, end - 1].
 */
/*
 * 从所有能完整包住 [start,end) 的 size 窗口中随机选择页对齐基址。区间
 * 已不小于窗口返回 0 表示无解；否则返回 start 向下随机 0..max_pgoff 页。
 */
static u64 __init random_bounding_box(u64 size, u64 start, u64 end)
{
	u64 max_pgoff, pgoff;

	if ((end - start) >= size)
		return 0;

	max_pgoff = (size - (end - start)) / PAGE_SIZE;
	pgoff = get_random_u32_inclusive(0, max_pgoff);

	return start - pgoff * PAGE_SIZE;
}

/*
 * Modules may directly reference data and text anywhere within the kernel
 * image and other modules. References using PREL32 relocations have a +/-2G
 * range, and so we need to ensure that the entire kernel image and all modules
 * fall within a 2G window such that these are always within range.
 *
 * Modules may directly branch to functions and code within the kernel text,
 * and to functions and code within other modules. These branches will use
 * CALL26/JUMP26 relocations with a +/-128M range. Without PLTs, we must ensure
 * that the entire kernel text and all module text falls within a 128M window
 * such that these are always within range. With PLTs, we can expand this to a
 * 2G window.
 *
 * We chose the 128M region to surround the entire kernel image (rather than
 * just the text) as using the same bounds for the 128M and 2G regions ensures
 * by construction that we never select a 128M region that is not a subset of
 * the 2G region. For very large and unusual kernel configurations this means
 * we may fall back to PLTs where they could have been avoided, but this keeps
 * the logic significantly simpler.
 */
/*
 * 根据 kernel image 尺寸与 KASLR 选择 128MiB direct CALL26 窗口和 2GiB
 * PREL32/PLT 窗口。返回 0；基址为 0 表示对应策略不可用。128M 窗口被
 * 强制选为 2G 子集，简化模块同时满足数据重定位和代码分支的证明。
 */
static int __init module_init_limits(void)
{
	u64 kernel_end = (u64)_end;
	u64 kernel_start = (u64)_text;
	u64 kernel_size = kernel_end - kernel_start;

	/*
	 * The default modules region is placed immediately below the kernel
	 * image, and is large enough to use the full 2G relocation range.
	 */
	/* 编译期保证默认 module VA 紧邻镜像且至少覆盖 PREL32 需要的 2GiB。 */
	BUILD_BUG_ON(KIMAGE_VADDR != MODULES_END);
	BUILD_BUG_ON(MODULES_VSIZE < SZ_2G);

	if (!kaslr_enabled()) {
		if (kernel_size < SZ_128M)
			module_direct_base = kernel_end - SZ_128M;
		if (kernel_size < SZ_2G)
			module_plt_base = kernel_end - SZ_2G;
	} else {
		u64 min = kernel_start;
		u64 max = kernel_end;

		if (IS_ENABLED(CONFIG_RANDOMIZE_MODULE_REGION_FULL)) {
			pr_info("2G module region forced by RANDOMIZE_MODULE_REGION_FULL\n");
		} else {
			module_direct_base = random_bounding_box(SZ_128M, min, max);
			if (module_direct_base) {
				min = module_direct_base;
				max = module_direct_base + SZ_128M;
			}
		}

		module_plt_base = random_bounding_box(SZ_2G, min, max);
	}

	pr_info("%llu pages in range for non-PLT usage",
		module_direct_base ? (SZ_128M - kernel_size) / PAGE_SIZE : 0);
	pr_info("%llu pages in range for PLT usage",
		module_plt_base ? (SZ_2G - kernel_size) / PAGE_SIZE : 0);

	return 0;
}

/* 最终 execmem 类型范围描述，setup 后只读并由通用 execmem 借用。 */
static struct execmem_info execmem_info __ro_after_init;

/*
 * 构造 arm64 execmem 分配策略并返回静态对象。模块优先 128MiB 免 PLT 区，
 * 失败退 2GiB PLT 区；kprobe/BPF 使用整个 vmalloc，但分别 ROX/可写初始属性。
 * 返回指针永久有效，无分配失败；alignment=1 表示无额外架构对齐约束。
 */
struct execmem_info __init *execmem_arch_setup(void)
{
	unsigned long fallback_start = 0, fallback_end = 0;
	unsigned long start = 0, end = 0;

	module_init_limits();

	/*
	 * Where possible, prefer to allocate within direct branch range of the
	 * kernel such that no PLTs are necessary.
	 */
	/* direct 区存在时主范围用它，2G 仅作为 fallback；否则直接以 2G 为主。 */
	if (module_direct_base) {
		start = module_direct_base;
		end = module_direct_base + SZ_128M;

		if (module_plt_base) {
			fallback_start = module_plt_base;
			fallback_end = module_plt_base + SZ_2G;
		}
	} else if (module_plt_base) {
		start = module_plt_base;
		end = module_plt_base + SZ_2G;
	}

	execmem_info = (struct execmem_info){
		.ranges = {
			[EXECMEM_DEFAULT] = {
				.start	= start,
				.end	= end,
				.pgprot	= PAGE_KERNEL,
				.alignment = 1,
				.fallback_start	= fallback_start,
				.fallback_end	= fallback_end,
			},
			[EXECMEM_KPROBES] = {
				.start	= VMALLOC_START,
				.end	= VMALLOC_END,
				.pgprot	= PAGE_KERNEL_ROX,
				.alignment = 1,
			},
			[EXECMEM_BPF] = {
				.start	= VMALLOC_START,
				.end	= VMALLOC_END,
				.pgprot	= PAGE_KERNEL,
				.alignment = 1,
			},
		},
	};

	return &execmem_info;
}
#endif /* CONFIG_EXECMEM */
