// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/kernel/setup.c
 *
 * Copyright (C) 1995-2001 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

/*
 * ARM64 架构相关的早期初始化
 *
 * 【在启动链中的位置】
 *   汇编入口 (_start / head.S)
 *     └─ start_kernel()          (init/main.c)
 *           └─ setup_arch()      (本文件，第 281 行) ← 当前位置
 *
 * 【本文件负责的工作】
 * setup_arch() 是 start_kernel() 中最重要的单个调用，完成所有
 * ARM64 架构相关的早期硬件与内存初始化，主要包括：
 *   1. 解析 DTB（设备树二进制），从 bootloader 获取硬件拓扑描述
 *   2. 建立 fixmap / early ioremap，提供早期阶段的虚拟地址映射
 *   3. 初始化 memblock 内存分配器，注册所有可用物理内存
 *   4. 建立最终的内核页表（paging_init），替换 head.S 的临时映射
 *   5. 检测 boot CPU 硬件特性（HWCAP / cpufeature）
 *   6. 初始化 SMP 多核，构建 MPIDR 哈希表
 *   7. 注册内核占用的物理内存区域到资源树
 *
 * 【调用完成后系统具备的能力】
 *   - 完整的内核虚拟地址空间（线性映射 + vmalloc + fixmap）
 *   - 可用的 memblock / bootmem 内存分配
 *   - KASLR 地址随机化已确定，内核镜像偏移已知
 *   - 所有 CPU 的 MPIDR 映射已建立，可以启动次级 CPU
 *   - 可以继续进行 start_kernel() 后续的通用子系统初始化
 */

#include <linux/acpi.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/initrd.h>
#include <linux/console.h>
#include <linux/cache.h>
#include <linux/screen_info.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/root_dev.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/fs.h>
#include <linux/panic_notifier.h>
#include <linux/proc_fs.h>
#include <linux/memblock.h>
#include <linux/of_fdt.h>
#include <linux/efi.h>
#include <linux/psci.h>
#include <linux/sched/task.h>
#include <linux/scs.h>
#include <linux/mm.h>

#include <asm/acpi.h>
#include <asm/fixmap.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/daifflags.h>
#include <asm/elf.h>
#include <asm/cpufeature.h>
#include <asm/cpu_ops.h>
#include <asm/kasan.h>
#include <asm/numa.h>
#include <asm/rsi.h>
#include <asm/scs.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/smp_plat.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/traps.h>
#include <asm/efi.h>
#include <asm/xen/hypervisor.h>
#include <asm/mmu_context.h>

/* standard_resources：指向系统 RAM 物理区域的资源描述符数组，
 * 在 request_standard_resources() 中按 memblock 区域数量动态分配。
 * num_standard_resources 记录数组元素数，等于 memblock.memory.cnt。 */
static int num_standard_resources;
static struct resource *standard_resources;

/* __fdt_pointer：保存 bootloader 通过寄存器 x0 传入的 DTB 物理地址。
 * 在 head.S 极早期（MMU 开启之前）被保存，供 setup_machine_fdt() 使用。
 * __initdata 表示该变量仅初始化阶段使用，之后内存可被回收。 */
phys_addr_t __fdt_pointer __initdata;

/* mmu_enabled_at_boot：记录内核入口时 MMU 是否已开启。
 * 正常启动时 MMU 应当关闭（0），若非 EFI 启动且 MMU 已开启则属于
 * 固件 bug，后续 check_mmu_enabled_at_boot() 会触发 panic。 */
u64 mmu_enabled_at_boot __initdata;

/*
 * 内核代码段和数据段在物理地址空间中的资源描述符。
 * start/end 初始为 0，在 request_standard_resources() 中根据
 * 内核链接符号（_text、_etext、_sdata、_end）填入真实的物理地址范围，
 * 然后通过 insert_resource() 挂入全局 iomem_resource 资源树，
 * 防止其他驱动误用这段物理地址。
 */
static struct resource mem_res[] = {
	{
		.name = "Kernel code",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	},
	{
		.name = "Kernel data",
		.start = 0,
		.end = 0,
		.flags = IORESOURCE_SYSTEM_RAM
	}
};

#define kernel_code mem_res[0]
#define kernel_data mem_res[1]

/*
 * boot_args[]：保存内核入口时寄存器 x0～x3 的原始值。
 * ARM64 Linux 启动协议规定：
 *   x0 = DTB 物理地址（__fdt_pointer）
 *   x1 = 0（保留）
 *   x2 = 0（保留）
 *   x3 = 0（保留）
 * 若 x1～x3 非零，说明 bootloader 不符合协议，setup_arch() 末尾
 * 会打印警告。__cacheline_aligned 确保该数组独占一条缓存行，
 * 避免 CPU 之间的伪共享。
 */
u64 __cacheline_aligned boot_args[4];

/*
 * smp_setup_processor_id - 读取 boot CPU 的 MPIDR 并建立逻辑 CPU 0 的映射
 *
 * MPIDR_EL1（Multiprocessor Affinity Register）是 ARM64 标准寄存器，
 * 用于唯一标识 SMP 系统中的每个 CPU 核心。它以亲和层级（Aff0～Aff3）
 * 编码 CPU 的物理拓扑位置（例如 cluster/core/thread）。
 *
 * 本函数在启动最早期（setup_arch 之前由 start_kernel 调用）执行：
 *   1. 读取当前 CPU 的 MPIDR 硬件 ID（屏蔽保留位后取物理 ID 部分）
 *   2. 将逻辑 CPU 编号 0 与该硬件 ID 绑定，写入 __cpu_logical_map[0]
 *   3. 打印启动日志，帮助识别在哪个物理核心上引导
 *
 * 注意：此时其他 CPU 核心尚未启动，只有逻辑 CPU 0 处于运行状态。
 */
void __init smp_setup_processor_id(void)
{
	u64 mpidr = read_cpuid_mpidr() & MPIDR_HWID_BITMASK;
	set_cpu_logical_map(0, mpidr);

	pr_info("Booting Linux on physical CPU 0x%010lx [0x%08x]\n",
		(unsigned long)mpidr, read_cpuid_id());
}

/*
 * arch_match_cpu_phys_id - 判断给定物理 CPU ID 是否与指定逻辑 CPU 匹配
 *
 * 内核通用层（generic SMP / CPU topology）在解析 DT 或 ACPI 时，
 * 会拿硬件描述中的物理 ID 来匹配已知的逻辑 CPU 编号。
 * 本函数作为 ARM64 架构实现，直接比较 phys_id 与
 * __cpu_logical_map[cpu] 中存储的 MPIDR 硬件 ID。
 */
bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpu_logical_map(cpu);
}

/*
 * mpidr_hash：全局 MPIDR 哈希表描述符，由 smp_build_mpidr_hash() 填充。
 * 用于在中断/调度路径中将 64 位 MPIDR 快速映射为连续的逻辑 CPU 编号索引，
 * 避免遍历所有 CPU 进行线性搜索。
 */
struct mpidr_hash mpidr_hash;

/**
 * smp_build_mpidr_hash - 预计算各亲和层级的位移量，以便从 MPIDR 构造线性索引
 *
 * ARM64 的 MPIDR_EL1 以 Aff0～Aff3 四个亲和字段描述 CPU 位置，
 * 但实际系统中并非所有比特都区分不同 CPU（例如固定为 0 的位无意义）。
 * 本函数的目标：预计算一套"移位 + OR"方案，把稀疏的 MPIDR 空间
 * 压缩成连续的哈希索引，既无冲突又尽量紧凑。
 *
 * 算法步骤：
 *   1. 扫描所有可能 CPU 的 MPIDR，通过异或找出"实际变化的比特掩码"
 *   2. 对每个亲和层级，找出最低有效位（ffs）和最高有效位（fls），
 *      计算该层级需要多少比特来表示
 *   3. 将各层级的有效比特依次拼接，计算出各层级的最终移位量，
 *      存入 mpidr_hash.shift_aff[]
 *   4. 所有层级总比特数存入 mpidr_hash.bits，决定哈希表大小（2^bits）
 *
 * 在热路径（如 IPI 分发）中使用 mpidr_hash 可以 O(1) 定位目标 CPU。
 */
static void __init smp_build_mpidr_hash(void)
{
	u32 i, affinity, fs[4], bits[4], ls;
	u64 mask = 0;
	/*
	 * Pre-scan the list of MPIDRS and filter out bits that do
	 * not contribute to affinity levels, ie they never toggle.
	 */
	for_each_possible_cpu(i)
		mask |= (cpu_logical_map(i) ^ cpu_logical_map(0));
	pr_debug("mask of set bits %#llx\n", mask);
	/*
	 * Find and stash the last and first bit set at all affinity levels to
	 * check how many bits are required to represent them.
	 */
	for (i = 0; i < 4; i++) {
		affinity = MPIDR_AFFINITY_LEVEL(mask, i);
		/*
		 * Find the MSB bit and LSB bits position
		 * to determine how many bits are required
		 * to express the affinity level.
		 */
		ls = fls(affinity);
		fs[i] = affinity ? ffs(affinity) - 1 : 0;
		bits[i] = ls - fs[i];
	}
	/*
	 * An index can be created from the MPIDR_EL1 by isolating the
	 * significant bits at each affinity level and by shifting
	 * them in order to compress the 32 bits values space to a
	 * compressed set of values. This is equivalent to hashing
	 * the MPIDR_EL1 through shifting and ORing. It is a collision free
	 * hash though not minimal since some levels might contain a number
	 * of CPUs that is not an exact power of 2 and their bit
	 * representation might contain holes, eg MPIDR_EL1[7:0] = {0x2, 0x80}.
	 */
	mpidr_hash.shift_aff[0] = MPIDR_LEVEL_SHIFT(0) + fs[0];
	mpidr_hash.shift_aff[1] = MPIDR_LEVEL_SHIFT(1) + fs[1] - bits[0];
	mpidr_hash.shift_aff[2] = MPIDR_LEVEL_SHIFT(2) + fs[2] -
						(bits[1] + bits[0]);
	mpidr_hash.shift_aff[3] = MPIDR_LEVEL_SHIFT(3) +
				  fs[3] - (bits[2] + bits[1] + bits[0]);
	mpidr_hash.mask = mask;
	mpidr_hash.bits = bits[3] + bits[2] + bits[1] + bits[0];
	pr_debug("MPIDR hash: aff0[%u] aff1[%u] aff2[%u] aff3[%u] mask[%#llx] bits[%u]\n",
		mpidr_hash.shift_aff[0],
		mpidr_hash.shift_aff[1],
		mpidr_hash.shift_aff[2],
		mpidr_hash.shift_aff[3],
		mpidr_hash.mask,
		mpidr_hash.bits);
	/*
	 * 4x is an arbitrary value used to warn on a hash table much bigger
	 * than expected on most systems.
	 */
	if (mpidr_hash_size() > 4 * num_possible_cpus())
		pr_warn("Large number of MPIDR hash buckets detected\n");
}

/*
 * setup_machine_fdt - 通过 DTB 获取硬件描述，完成设备树的早期解析
 * @dt_phys: bootloader 传入的 DTB（Device Tree Blob）物理地址
 *
 * 【什么是 DTB】
 * DTB 是描述硬件拓扑的二进制文件（扁平设备树，FDT 格式）。
 * 它由 bootloader（如 U-Boot）或固件在启动时准备，通过寄存器 x0
 * 传递给内核入口（head.S 将其保存为 __fdt_pointer）。
 * DTB 中描述了：内存范围、CPU 数量与拓扑、时钟、中断控制器、
 * 总线、外设等硬件信息，内核依赖它来驱动硬件。
 *
 * 【函数工作流程】
 *   1. fixmap_remap_fdt()：通过 fixmap 机制把 DTB 物理地址映射到
 *      固定虚拟地址窗口（此时通用 vmalloc 尚未就绪，只能用 fixmap）。
 *      成功后立即调用 memblock_reserve() 预留 DTB 占用的物理内存，
 *      防止后续内存分配覆盖它。
 *
 *   2. early_init_dt_scan()：扫描 DTB 头部、/chosen 节点（获取
 *      boot 命令行、initrd 地址）、/memory 节点（向 memblock 注册
 *      可用物理内存范围）。若 DTB 无效（魔数错误、未对齐、超过 2MB）
 *      则进入死循环——此时异常/oops 机制尚未就绪，无法正常 panic。
 *
 *   3. 解析完成后将 DTB 的 fixmap 映射权限降级为只读（PAGE_KERNEL_RO），
 *      防止后续代码意外修改设备树内容。
 *
 *   4. 读取 DTB 根节点的 "model" 属性，打印机器型号日志，
 *      并设置 dump_stack 的架构描述字符串（出现 panic 时会显示）。
 */
static void __init setup_machine_fdt(phys_addr_t dt_phys)
{
	int size = 0;
	void *dt_virt = fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL);
	const char *name;

	if (dt_virt)
		memblock_reserve(dt_phys, size);

	/*
	 * dt_virt is a fixmap address, hence __pa(dt_virt) can't be used.
	 * Pass dt_phys directly.
	 */
	if (!early_init_dt_scan(dt_virt, dt_phys)) {
		pr_crit("\n"
			"Error: invalid device tree blob: PA=%pa, VA=%px, size=%d bytes\n"
			"The dtb must be 8-byte aligned and must not exceed 2 MB in size.\n"
			"\nPlease check your bootloader.\n",
			&dt_phys, dt_virt, size);

		/*
		 * Note that in this _really_ early stage we cannot even BUG()
		 * or oops, so the least terrible thing to do is cpu_relax(),
		 * or else we could end-up printing non-initialized data, etc.
		 */
		while (true)
			cpu_relax();
	}

	/* Early fixups are done, map the FDT as read-only now */
	fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO);

	name = of_flat_dt_get_machine_name();
	if (!name)
		return;

	pr_info("Machine model: %s\n", name);
	dump_stack_set_arch_desc("%s (DT)", name);
}

/*
 * request_standard_resources - 向内核资源树注册内核镜像和系统 RAM 的物理地址范围
 *
 * Linux 内核维护一棵全局资源树（iomem_resource），记录系统中所有
 * 物理地址区间的归属（System RAM、Kernel code、I/O 映射等）。
 * 驱动申请 I/O 内存时会检查该树，避免与已注册区域冲突。
 *
 * 本函数完成以下注册工作：
 *
 *   1. 注册内核代码段和数据段：
 *      - kernel_code: [_text, __init_begin)  内核可执行代码（含 .init.text）
 *      - kernel_data: [_sdata, _end)         内核数据（含 BSS、init 数据）
 *      使用链接脚本符号转换为物理地址后插入资源树。
 *
 *   2. 按 memblock 内存区域逐一注册系统 RAM：
 *      - 普通可用内存区域：标记为 "System RAM"（IORESOURCE_SYSTEM_RAM | BUSY）
 *      - nomap 区域（DTB 中标记 no-map 的区域，内核不建立线性映射）：
 *        标记为 "reserved"（IORESOURCE_MEM）
 *      每个区域都通过 insert_resource() 挂入 iomem_resource 子树。
 *
 * 注意：standard_resources 数组本身用 memblock_alloc 分配，因为此时
 * slab 分配器还未初始化，不能使用 kmalloc。
 */
static void __init request_standard_resources(void)
{
	struct memblock_region *region;
	struct resource *res;
	unsigned long i = 0;
	size_t res_size;

	kernel_code.start   = __pa_symbol(_text);
	kernel_code.end     = __pa_symbol(__init_begin - 1);
	kernel_data.start   = __pa_symbol(_sdata);
	kernel_data.end     = __pa_symbol(_end - 1);
	insert_resource(&iomem_resource, &kernel_code);
	insert_resource(&iomem_resource, &kernel_data);

	num_standard_resources = memblock.memory.cnt;
	res_size = num_standard_resources * sizeof(*standard_resources);
	standard_resources = memblock_alloc_or_panic(res_size, SMP_CACHE_BYTES);

	for_each_mem_region(region) {
		res = &standard_resources[i++];
		if (memblock_is_nomap(region)) {
			res->name  = "reserved";
			res->flags = IORESOURCE_MEM;
			res->start = __pfn_to_phys(memblock_region_reserved_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_reserved_end_pfn(region)) - 1;
		} else {
			res->name  = "System RAM";
			res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;
			res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));
			res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;
		}

		insert_resource(&iomem_resource, res);
	}
}

/*
 * reserve_memblock_reserved_regions - 将 memblock 预留区标记到资源树
 *
 * request_standard_resources() 把物理内存注册为 "System RAM" 资源后，
 * 其中仍有一些子区间被 memblock 标记为 reserved（例如内核镜像自身、
 * DTB、initrd、crash kernel 区域等）。
 *
 * 本函数作为 arch_initcall 在稍晚阶段运行，遍历所有 System RAM 资源，
 * 找出与 memblock reserved 区域重叠的部分，以 "reserved" 标签将其
 * 从父资源中拆分出来（reserve_region_with_split），使 /proc/iomem
 * 能准确反映哪些物理内存是内核保留的、哪些是可用系统 RAM。
 *
 * 使用 arch_initcall 而非在 setup_arch 中直接调用，是因为此时
 * memblock reserved 列表已经完整，不会再有新的预留操作。
 */
static int __init reserve_memblock_reserved_regions(void)
{
	u64 i, j;

	for (i = 0; i < num_standard_resources; ++i) {
		struct resource *mem = &standard_resources[i];
		phys_addr_t r_start, r_end, mem_size = resource_size(mem);

		if (!memblock_is_region_reserved(mem->start, mem_size))
			continue;

		for_each_reserved_mem_range(j, &r_start, &r_end) {
			resource_size_t start, end;

			start = max(PFN_PHYS(PFN_DOWN(r_start)), mem->start);
			end = min(PFN_PHYS(PFN_UP(r_end)) - 1, mem->end);

			if (start > mem->end || end < mem->start)
				continue;

			reserve_region_with_split(mem, start, end, "reserved");
		}
	}

	return 0;
}
arch_initcall(reserve_memblock_reserved_regions);

/*
 * __cpu_logical_map[]：逻辑 CPU 编号到 MPIDR 硬件 ID 的映射表。
 *
 * 内核使用从 0 开始的连续整数作为逻辑 CPU 编号，而硬件通过 MPIDR
 * 寄存器提供稀疏的物理 ID。该数组建立两者之间的对应关系：
 *   index = 逻辑 CPU 编号（0, 1, 2, ...）
 *   value = MPIDR 硬件 ID（从 DTB/ACPI 或 MPIDR 寄存器读取）
 *
 * 初始值全部设为 INVALID_HWID（~0ULL），表示尚未映射。
 * boot CPU（逻辑 0）在 smp_setup_processor_id() 中被填充；
 * 其余 CPU 在 smp_init_cpus() 解析 DT/ACPI 时逐一填入。
 */
u64 __cpu_logical_map[NR_CPUS] = { [0 ... NR_CPUS-1] = INVALID_HWID };

/*
 * cpu_logical_map - 查询指定逻辑 CPU 的 MPIDR 硬件 ID
 * @cpu: 逻辑 CPU 编号
 *
 * 提供对 __cpu_logical_map[] 的统一访问接口，避免外部代码直接操作数组。
 */
u64 cpu_logical_map(unsigned int cpu)
{
	return __cpu_logical_map[cpu];
}

/*
 * setup_arch - ARM64 架构相关的早期初始化总入口
 * @cmdline_p: 输出参数，指向内核命令行字符串的指针
 *
 * 由 start_kernel()（init/main.c）在极早期调用，是整个启动链中
 * 最重要的单个函数。执行完毕后内核具备完整的虚拟地址空间、
 * 内存分配能力以及 SMP 拓扑信息，可以进行后续通用子系统初始化。
 *
 * __no_sanitize_address：禁止 KASAN 对本函数插桩，因为此时 KASAN
 * 自身尚未初始化，过早使用会导致崩溃。
 *
 * 初始化顺序及原因说明见函数体内各步骤注释。
 */
void __init __no_sanitize_address setup_arch(char **cmdline_p)
{
	/*
	 * 步骤 1：初始化内核虚拟地址空间描述符 init_mm
	 *
	 * init_mm 是内核地址空间的 mm_struct，描述内核代码段、数据段等
	 * 虚拟地址范围。此处用链接脚本符号填充 start_code/end_code/
	 * end_data/brk 字段，使内核内存管理子系统能识别自身布局。
	 */
	setup_initial_init_mm(_text, _etext, _edata, _end);

	/*
	 * 步骤 2：向调用者暴露内核命令行
	 *
	 * boot_command_line[] 在 head.S 中从 DTB /chosen 节点的
	 * "bootargs" 属性拷贝而来。将其地址赋给 *cmdline_p，
	 * start_kernel() 随后会把它传递给 parse_early_param() 等。
	 */
	*cmdline_p = boot_command_line;

	/*
	 * 步骤 3：确定 KASLR 偏移
	 *
	 * KASLR（Kernel Address Space Layout Randomization）在 head.S
	 * 中已选定随机偏移，此处 kaslr_init() 将偏移值固化到全局变量，
	 * 使后续的符号地址计算（如 __pa_symbol）能正确处理随机基址。
	 */
	kaslr_init();

	/*
	 * 步骤 4：建立早期固定地址映射（fixmap）
	 *
	 * fixmap 是内核虚拟地址空间末尾的一段固定槽位，用于在页表
	 * 完整建立之前映射少量物理地址（如 DTB、早期 UART 等）。
	 * early_fixmap_init() 建立 fixmap 区域的页表项，使后续
	 * fixmap_remap_fdt() 等调用能够正常使用。
	 */
	early_fixmap_init();

	/*
	 * 步骤 5：初始化早期 I/O 重映射（early ioremap）
	 *
	 * early_ioremap 基于 fixmap 提供一套临时的 ioremap 接口，
	 * 供在 vmalloc/ioremap 完整实现初始化之前访问 MMIO 寄存器
	 * （例如平台 UART、GIC 等早期探测）。
	 */
	early_ioremap_init();

	/*
	 * 步骤 6：解析设备树（DTB）
	 *
	 * setup_machine_fdt() 完成以下工作：
	 *   a. 通过 fixmap 将 DTB 物理地址映射到内核虚拟地址
	 *   b. 用 memblock_reserve() 预留 DTB 物理内存
	 *   c. 扫描 /chosen（命令行、initrd）和 /memory（物理内存范围）
	 *   d. 将内存范围注册进 memblock，使后续分配知道可用地址空间
	 *   e. 将 DTB 改为只读映射，打印机器型号
	 *
	 * 这是最早确定"系统有多少内存、内存在哪里"的关键步骤。
	 */
	setup_machine_fdt(__fdt_pointer);

	/*
	 * Initialise the static keys early as they may be enabled by the
	 * cpufeature code and early parameters.
	 */
	/*
	 * 步骤 7：初始化 jump_label（静态键）和解析早期命令行参数
	 *
	 * jump_label_init()：初始化 Linux 的"静态键"机制——一种将
	 * 条件分支替换为 NOP/JMP 指令的优化手段，避免热路径中的分支预测
	 * 开销。cpufeature 和 early_param 代码可能启用静态键，因此必须
	 * 在它们之前初始化。
	 *
	 * parse_early_param()：扫描 boot_command_line，调用所有通过
	 * early_param() 宏注册的早期参数处理函数（如 "mem=", "earlyprintk=",
	 * "kaslr_offset=" 等），在通用 param 解析框架就绪前完成关键配置。
	 */
	jump_label_init();
	parse_early_param();

	/*
	 * 步骤 8：动态 SCS（Shadow Call Stack）初始化
	 *
	 * SCS 是一种硬件/软件结合的控制流完整性机制，为每个线程维护
	 * 一份独立的返回地址影子栈，防止 ROP 攻击篡改返回地址。
	 * dynamic_scs_init() 根据 CPU 特性决定是否启用运行时 SCS，
	 * 必须在调度器初始化之前完成，因为新线程创建时需要分配影子栈。
	 */
	dynamic_scs_init();

	/*
	 * The primary CPU enters the kernel with all DAIF exceptions masked.
	 *
	 * We must unmask Debug and SError before preemption or scheduling is
	 * possible to ensure that these are consistently unmasked across
	 * threads, and we want to unmask SError as soon as possible after
	 * initializing earlycon so that we can report any SErrors immediately.
	 *
	 * IRQ and FIQ will be unmasked after the root irqchip has been
	 * detected and initialized.
	 */
	/*
	 * 步骤 9：解除 Debug 和 SError 异常屏蔽
	 *
	 * ARM64 使用 DAIF 寄存器控制异常屏蔽：
	 *   D = Debug，A = SError（异步中止），I = IRQ，F = FIQ
	 * 内核入口时所有位均被屏蔽（head.S 设置），此处恢复
	 * DAIF_PROCCTX_NOIRQ 状态（D、A 解除屏蔽，I、F 仍屏蔽）：
	 *   - Debug 解除屏蔽：允许断点/单步调试异常
	 *   - SError 解除屏蔽：尽早捕获总线错误/内存错误，便于诊断
	 *   - IRQ/FIQ 继续屏蔽：待中断控制器（root irqchip）初始化后再开放
	 */
	local_daif_restore(DAIF_PROCCTX_NOIRQ);

	/*
	 * TTBR0 is only used for the identity mapping at this stage. Make it
	 * point to zero page to avoid speculatively fetching new entries.
	 */
	/*
	 * 步骤 10：卸载恒等映射（identity map）
	 *
	 * head.S 建立了一份物理地址 == 虚拟地址的恒等映射（idmap），
	 * 用于 MMU 开启前后的过渡。现在已不再需要，cpu_uninstall_idmap()
	 * 将 TTBR0_EL1 指向全零页（reserved_pg_dir 中的空页表），
	 * 任何用户空间地址的访问都会触发翻译错误，防止内核态代码通过
	 * TTBR0 进行推测性访问带来的安全隐患。
	 */
	cpu_uninstall_idmap();

	/*
	 * 步骤 11：Xen 和 EFI 早期初始化
	 *
	 * xen_early_init()：若运行在 Xen hypervisor 下，探测 Xen 接口
	 * 并建立 hypercall 页映射。
	 *
	 * efi_init()：若 bootloader 通过 EFI 引导（如 UEFI 固件），
	 * 解析 EFI 系统表，获取内存映射、UEFI 变量服务等。
	 * EFI 内存映射也会更新 memblock，补充固件描述的内存信息。
	 */
	xen_early_init();
	efi_init();

	/*
	 * 步骤 12：检查内核镜像对齐和 MMU 状态（非 EFI 启动路径）
	 *
	 * MIN_KIMG_ALIGN：内核镜像要求的最小对齐（通常 2MB，与大页对齐）。
	 * 若镜像未对齐，说明 bootloader 不符合规范，打印警告。
	 *
	 * mmu_enabled_at_boot：若非 EFI 路径且 MMU 在内核入口时已开启，
	 * 这属于固件 bug（非 EFI 启动应当关闭 MMU 后再跳转内核），
	 * 内核打上 TAINT_FIRMWARE_WORKAROUND 污点标记。
	 */
	if (!efi_enabled(EFI_BOOT)) {
		if ((u64)_text % MIN_KIMG_ALIGN)
			pr_warn(FW_BUG "Kernel image misaligned at boot, please fix your bootloader!");
		WARN_TAINT(mmu_enabled_at_boot, TAINT_FIRMWARE_WORKAROUND,
			   FW_BUG "Booted with MMU enabled!");
	}

	/*
	 * 步骤 13：建立 memblock 内存管理器
	 *
	 * arm64_memblock_init() 是内存管理初始化的核心，完成：
	 *   a. 将 DTB /memory 节点描述的物理内存注册进 memblock.memory
	 *   b. 预留内核镜像（含 .init 段）占用的物理内存
	 *   c. 预留 DTB 自身、initrd、EFI 内存映射等区域
	 *   d. 处理 "mem=", "memmap=" 等命令行内存限制参数
	 *   e. 设置 memblock_end_of_DRAM() 等边界
	 *
	 * memblock 是最早期的物理内存分配器（位图式），在 buddy allocator
	 * 建立之前为内核提供基本的内存分配能力（memblock_alloc）。
	 */
	arm64_memblock_init();

	/*
	 * 步骤 14：建立完整的内核页表（最终形态）
	 *
	 * paging_init() 是虚拟地址空间建立的决定性时刻：
	 *   a. 根据 memblock 记录的所有物理内存，建立线性映射
	 *      （PAGE_OFFSET 开始的直接映射区，覆盖全部物理 RAM）
	 *   b. 为 vmalloc 区域准备页目录项
	 *   c. 建立 fixmap、PCI I/O 等特殊区域的页表
	 *   d. 将 TTBR1_EL1 切换到新页表，替换 head.S 中建立的临时映射
	 *   e. 刷新 TLB，确保新映射立即生效
	 *
	 * 调用完成后，内核虚拟地址空间进入最终形态，所有物理内存均可
	 * 通过线性映射地址（phys_to_virt / __va）直接访问。
	 */
	paging_init();

	/*
	 * 步骤 15：ACPI 表处理
	 *
	 * acpi_table_upgrade()：如果 initrd 中包含覆盖 ACPI 表的文件
	 * （通过 ACPI Override 机制），在此处将其合并到 ACPI 表集合中。
	 * 这允许在不修改固件的情况下打补丁修复 ACPI Bug。
	 *
	 * acpi_boot_table_init()：解析 ACPI 根表（RSDP/XSDT），判断
	 * 系统是使用 ACPI 还是 DTB 描述硬件。若 ACPI 有效则关闭 DTB
	 * 路径（acpi_disabled = false）；若 ACPI 缺失则回退到 DTB。
	 */
	acpi_table_upgrade();

	/* Parse the ACPI tables for possible boot-time configuration */
	acpi_boot_table_init();

	/*
	 * 步骤 16：展开设备树（DTB 路径）
	 *
	 * 仅在 ACPI 不可用时（acpi_disabled == true）执行。
	 * unflatten_device_tree() 将 DTB 的扁平二进制格式解析为
	 * 内核内部的 device_node 树（of_root），后续驱动通过
	 * of_find_node_by_name() 等接口访问设备描述。
	 * 这是 OF（Open Firmware / Device Tree）子系统的核心数据结构。
	 */
	if (acpi_disabled)
		unflatten_device_tree();

	/*
	 * 步骤 17：初始化 bootmem / 页帧分配器
	 *
	 * bootmem_init() 在 memblock 基础上完成以下工作：
	 *   a. 调用 sparse_init()，建立 struct page 数组（mem_map）
	 *      使每个物理页帧都有对应的 page 描述符
	 *   b. 将 memblock 中可用的内存交给 zone_sizes_init()，
	 *      按 ZONE_DMA / ZONE_NORMAL 等区划分 buddy allocator 的管理范围
	 *   c. 完成后 memblock 进入只读模式，buddy allocator 接管内存分配
	 */
	bootmem_init();

	/*
	 * 步骤 18：初始化 KASAN（内核地址消毒器）
	 *
	 * KASAN 是内核运行时内存安全检测工具，能检测越界访问、
	 * use-after-free 等内存错误。kasan_init() 建立 KASAN 影子内存：
	 * 用 1/8 的虚拟地址空间作为影子区域，记录每个字节的合法性。
	 * 必须在 paging_init（页表就绪）之后、实际内存分配使用之前执行。
	 * 若未开启 CONFIG_KASAN 则此函数为空操作。
	 */
	kasan_init();

	/*
	 * 步骤 19：注册内核物理内存区域到资源树
	 *
	 * request_standard_resources() 将内核镜像代码段/数据段和
	 * 全部 System RAM 注册到 iomem_resource 资源树，使 /proc/iomem
	 * 能显示完整的物理地址布局，并防止驱动误用这些区域。
	 */
	request_standard_resources();

	/*
	 * 步骤 20：重置早期 ioremap
	 *
	 * early_ioremap_reset() 标志着早期 ioremap 阶段的结束。
	 * 从此之后 ioremap() 将使用完整的 vmalloc 机制，不再依赖
	 * fixmap 的临时槽位，early ioremap 相关的状态可以释放。
	 */
	early_ioremap_reset();

	/*
	 * 步骤 21：初始化 PSCI（电源状态协调接口）
	 *
	 * PSCI（Power State Coordination Interface）是 ARM 定义的固件
	 * 接口规范，用于 CPU 热插拔、系统挂起/恢复、CPU 启动/下线等
	 * 电源管理操作。内核通过 PSCI 的 CPU_ON 调用来启动次级 CPU。
	 *
	 * 根据硬件描述来源选择不同初始化路径：
	 *   - DTB 路径（acpi_disabled）：psci_dt_init() 从 DT 的
	 *     "arm,psci-1.0" 等兼容字符串中获取 PSCI 版本和调用约定
	 *   - ACPI 路径：psci_acpi_init() 从 ACPI MADT 表中获取 PSCI 信息
	 */
	if (acpi_disabled)
		psci_dt_init();
	else
		psci_acpi_init();

	/*
	 * 步骤 22：ARM Realm Security Interface（RSI）初始化
	 *
	 * RSI 是 ARMv9 CCA（Confidential Compute Architecture）的一部分，
	 * 当内核运行在 Realm 环境中时（由 RMM 管理），arm64_rsi_init()
	 * 探测并初始化与 Realm Monitor（RMM）的通信接口。
	 * 非 Realm 环境下为空操作。
	 */
	arm64_rsi_init();

	/*
	 * 步骤 23：初始化 SMP 多核支持
	 *
	 * init_bootcpu_ops()：为 boot CPU 查找并绑定 cpu_operations，
	 *   即该 CPU 的启动/停止操作集（PSCI、spin-table 等具体实现）。
	 *
	 * smp_init_cpus()：解析 DTB 或 ACPI 中的 CPU 节点，
	 *   为每个 CPU 填充 __cpu_logical_map[]，并调用 set_cpu_possible()
	 *   标记所有可能存在的 CPU，建立 SMP 拓扑。
	 *
	 * smp_build_mpidr_hash()：预计算 MPIDR 到逻辑 CPU 索引的哈希映射，
	 *   加速中断路由等热路径中的 CPU 查找操作。
	 */
	init_bootcpu_ops();
	smp_init_cpus();
	smp_build_mpidr_hash();

#ifdef CONFIG_ARM64_SW_TTBR0_PAN
	/*
	 * Make sure init_thread_info.ttbr0 always generates translation
	 * faults in case uaccess_enable() is inadvertently called by the init
	 * thread.
	 */
	/*
	 * 步骤 24：为 init 任务设置软件 PAN 保护（SW_TTBR0_PAN）
	 *
	 * PAN（Privileged Access Never）是 ARMv8.1 硬件特性，防止内核
	 * 态代码直接解引用用户空间指针（需通过 copy_from_user 等专用接口）。
	 * 对于没有硬件 PAN 的旧 CPU，CONFIG_ARM64_SW_TTBR0_PAN 使用
	 * 软件模拟：将 TTBR0_EL1 在内核路径中指向无效页表（reserved_pg_dir），
	 * 任何直接访问用户地址都会触发翻译异常。
	 * 此处为 init_task 设置初始 ttbr0 值，防止 uaccess_enable()
	 * 被 init 线程意外调用时造成安全漏洞。
	 */
	init_task.thread_info.ttbr0 = phys_to_ttbr(__pa_symbol(reserved_pg_dir));
#endif

	/*
	 * 步骤 25：检查 bootloader 是否遵守 ARM64 启动协议
	 *
	 * ARM64 Linux 启动协议规定 x1～x3 必须为 0，只有 x0 携带 DTB 地址。
	 * 若 x1～x3 非零，说明 bootloader 有 bug 或者是为旧内核设计的，
	 * 打印警告帮助调试。boot_args[] 在 head.S 中已保存入口寄存器值。
	 */
	if (boot_args[1] || boot_args[2] || boot_args[3]) {
		pr_err("WARNING: x1-x3 nonzero in violation of boot protocol:\n"
			"\tx1: %016llx\n\tx2: %016llx\n\tx3: %016llx\n"
			"This indicates a broken bootloader or old kernel\n",
			boot_args[1], boot_args[2], boot_args[3]);
	}
}

/*
 * cpu_can_disable - 查询指定 CPU 是否支持热下线（offline）
 * @cpu: 逻辑 CPU 编号
 *
 * 通过该 CPU 绑定的 cpu_operations（如 PSCI ops）中的
 * cpu_can_disable 回调判断。PSCI 实现通常返回 true，
 * spin-table 方式的 CPU 因无法重新上线，通常返回 false。
 * 未开启 CONFIG_HOTPLUG_CPU 时始终返回 false。
 */
static inline bool cpu_can_disable(unsigned int cpu)
{
#ifdef CONFIG_HOTPLUG_CPU
	const struct cpu_operations *ops = get_cpu_ops(cpu);

	if (ops && ops->cpu_can_disable)
		return ops->cpu_can_disable(cpu);
#endif
	return false;
}

/*
 * arch_cpu_is_hotpluggable - 判断指定逻辑 CPU 是否支持热插拔
 * @num: 逻辑 CPU 编号
 *
 * 内核通用 CPU hotplug 框架在建立 CPU sysfs 节点时调用此架构回调，
 * 决定是否为该 CPU 暴露 /sys/devices/system/cpu/cpuN/online 接口。
 * ARM64 直接委托给 cpu_can_disable()。
 */
bool arch_cpu_is_hotpluggable(int num)
{
	return cpu_can_disable(num);
}

/*
 * dump_kernel_offset - 在 panic 时打印内核地址随机化偏移
 *
 * KASLR 开启时，内核镜像被加载到 KIMAGE_VADDR + offset 处，
 * 而非固定地址。panic 时打印此偏移对于：
 *   - 符号解析（将崩溃日志中的虚拟地址换算为符号偏移）
 *   - 确认 KASLR 是否生效
 * 至关重要。PHYS_OFFSET 指示物理内存起始地址，也一并打印。
 *
 * 若 CONFIG_RANDOMIZE_BASE 未开启或偏移为 0（随机化未生效），
 * 则打印 "disabled" 以明确标识当前状态。
 */
static void dump_kernel_offset(void)
{
	const unsigned long offset = kaslr_offset();

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE) && offset > 0) {
		pr_emerg("Kernel Offset: 0x%lx from 0x%lx\n",
			 offset, KIMAGE_VADDR);
		pr_emerg("PHYS_OFFSET: 0x%llx\n", PHYS_OFFSET);
	} else {
		pr_emerg("Kernel Offset: disabled\n");
	}
}

/*
 * arm64_panic_block_dump - panic 时转储 ARM64 关键诊断信息
 *
 * 作为 panic_notifier_list 的回调，在内核 panic 时自动触发，
 * 依次输出：
 *   - dump_kernel_offset()：KASLR 偏移，用于符号解析
 *   - dump_cpu_features()：当前 CPU 特性检测结果（cpufeature bitmap）
 *   - dump_mem_limit()：arm64 的内存限制（如 mem= 参数效果）
 *
 * 这些信息对 bug 报告和远程调试至关重要，尤其是在无法获取
 * vmlinux 符号表时，KASLR 偏移能让调试者手动还原崩溃地址。
 */
static int arm64_panic_block_dump(struct notifier_block *self,
				  unsigned long v, void *p)
{
	dump_kernel_offset();
	dump_cpu_features();
	dump_mem_limit();
	return 0;
}

/* arm64_panic_block：notifier_block 静态实例，绑定上面的回调函数。 */
static struct notifier_block arm64_panic_block = {
	.notifier_call = arm64_panic_block_dump
};

/*
 * register_arm64_panic_block - 将 ARM64 诊断回调注册到 panic 通知链
 *
 * 使用 device_initcall 级别（而非 arch_initcall / core_initcall），
 * 确保在设备驱动初始化完成、系统进入稳定运行状态后再注册，
 * 避免过早注册导致回调中引用未初始化的状态。
 */
static int __init register_arm64_panic_block(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
				       &arm64_panic_block);
	return 0;
}
device_initcall(register_arm64_panic_block);

/*
 * check_mmu_enabled_at_boot - 检测非 EFI 启动时 MMU 是否被错误开启
 *
 * ARM64 Linux 启动协议（非 EFI 路径）要求：bootloader 跳转到内核
 * 入口时必须关闭 MMU 和数据缓存，由内核自己完成初始映射。
 * 若 MMU 已开启，内核无法控制早期地址翻译，可能导致：
 *   - 内核镜像映射与预期不符，造成不可预知的访问错误
 *   - 缓存一致性问题，导致数据损坏
 *
 * setup_arch() 中仅打印 WARN_TAINT（不致命），以便 earlycon
 * 有机会输出错误信息。此 device_initcall_sync 在设备初始化
 * 同步点触发正式的 panic，确保消息已经被记录。
 *
 * EFI 路径（UEFI 固件引导）例外，因为 EFI 规范本身要求 MMU 开启。
 */
static int __init check_mmu_enabled_at_boot(void)
{
	if (!efi_enabled(EFI_BOOT) && mmu_enabled_at_boot)
		panic("Non-EFI boot detected with MMU and caches enabled");
	return 0;
}
device_initcall_sync(check_mmu_enabled_at_boot);
