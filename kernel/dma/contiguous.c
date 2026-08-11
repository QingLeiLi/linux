// SPDX-License-Identifier: GPL-2.0+
/*
 * Contiguous Memory Allocator for DMA mapping framework
 * Copyright (c) 2010-2011 by Samsung Electronics.
 * Written by:
 *	Marek Szyprowski <m.szyprowski@samsung.com>
 *	Michal Nazarewicz <mina86@mina86.com>
 *
 * Contiguous Memory Allocator
 *
 *   The Contiguous Memory Allocator (CMA) makes it possible to
 *   allocate big contiguous chunks of memory after the system has
 *   booted.
 *
 * Why is it needed?
 *
 *   Various devices on embedded systems have no scatter-getter and/or
 *   IO map support and require contiguous blocks of memory to
 *   operate.  They include devices such as cameras, hardware video
 *   coders, etc.
 *
 *   Such devices often require big memory buffers (a full HD frame
 *   is, for instance, more than 2 mega pixels large, i.e. more than 6
 *   MB of memory), which makes mechanisms such as kmalloc() or
 *   alloc_page() ineffective.
 *
 *   At the same time, a solution where a big memory region is
 *   reserved for a device is suboptimal since often more memory is
 *   reserved then strictly required and, moreover, the memory is
 *   inaccessible to page system even if device drivers don't use it.
 *
 *   CMA tries to solve this issue by operating on memory regions
 *   where only movable pages can be allocated from.  This way, kernel
 *   can use the memory for pagecache and when device driver requests
 *   it, allocated pages can be migrated.
 */
/*
 * 学习说明：连续内存分配器（CMA）在启动早期预留一组物理页框，但这些页在设备
 * 尚未请求时仍可作为 MIGRATE_CMA 页供页缓存等可迁移用户使用。运行期申请大块
 * 连续 DMA 内存时，MM 核心迁走区域内的可移动页，最终交付一段连续 PFN；因此它
 * 同时缓解了“大块连续内存难以临时获得”和“设备专用保留区长期闲置”两个问题。
 *
 * 原文要点翻译：许多没有 scatter-gather 或 I/O 地址映射能力的嵌入式设备（如
 * 相机、视频编解码器）需要数 MiB 的物理连续缓冲区，kmalloc()/alloc_page() 很难
 * 在系统运行后满足。静态独占内存又会浪费容量且无法被页分配器利用。CMA 只允许
 * 可移动页临时进入其区域，在设备需要时通过迁移回收出连续空间。
 *
 * 本文件负责 DMA 框架侧的策略与生命周期连接：解析早期参数，建立默认、每设备及
 * 每 NUMA 节点的 CMA 区域，选择运行期分配回退顺序，并把设备树 shared-dma-pool
 * 挂到 dev->cma_area。具体位图、迁移和 page 引用操作由 mm/cma.c 实现。
 */

#define pr_fmt(fmt) "cma: " fmt
/* pr_fmt 让本文件通过 pr_*() 输出的日志统一带有 "cma: " 子系统前缀。 */

#include <asm/page.h>

#include <linux/memblock.h>
#include <linux/err.h>
#include <linux/sizes.h>
#include <linux/dma-map-ops.h>
#include <linux/cma.h>
#include <linux/nospec.h>

#ifdef CONFIG_CMA_SIZE_MBYTES
/* CMA_SIZE_MBYTES 是 Kconfig 选出的默认全局 CMA 容量，单位 MiB；未配置时为 0。 */
#define CMA_SIZE_MBYTES CONFIG_CMA_SIZE_MBYTES
#else
#define CMA_SIZE_MBYTES 0
#endif

/*
 * dma_contiguous_areas - 供 CMA heap 等枚举者访问的区域借用指针数组
 * dma_contiguous_areas_num - 数组中已经发布的有效元素数量
 *
 * 数组容量由 CONFIG_CMA_AREAS 决定。区域对象由 CMA 核心静态池持有并系统期常驻；
 * 本数组不拥有或释放它们。写入发生在早期启动/设备树初始化，运行期只读枚举，
 * 因而这里没有锁；调用顺序必须保证发布结束后消费者才开始读取。
 */
static struct cma *dma_contiguous_areas[MAX_CMA_AREAS];
static unsigned int dma_contiguous_areas_num;

/*
 * dma_contiguous_insert_area - 把一个 CMA 区域追加到可枚举列表
 * @cma: 已成功创建、生命周期覆盖系统运行期的 CMA 区域
 *
 * 容量已满时返回 -EINVAL，数组和计数均不变；成功时先写入当前尾槽，再递增计数并
 * 返回 0。函数不取得引用、不检查重复项，也没有内部锁，只能在串行初始化阶段调用。
 * 后续 dma_contiguous_get_area_by_idx() 与 dma-buf CMA heap 初始化会读取该列表。
 */
static int dma_contiguous_insert_area(struct cma *cma)
{
	if (dma_contiguous_areas_num >= ARRAY_SIZE(dma_contiguous_areas))
		return -EINVAL;

	dma_contiguous_areas[dma_contiguous_areas_num++] = cma;

	return 0;
}

/**
 * dma_contiguous_get_area_by_idx() - Get contiguous area at given index
 * @idx: index of the area we query
 *
 * Queries for the contiguous area located at index @idx.
 *
 * Returns:
 * A pointer to the requested contiguous area, or NULL otherwise.
 */
/*
 * dma_contiguous_get_area_by_idx - 按发布顺序查询 CMA 区域
 * @idx: 从 0 开始的区域索引
 *
 * 原 kernel-doc 翻译与展开：@idx 小于当前区域数时返回对应 struct cma 借用指针，
 * 越界返回 NULL。调用者常以 NULL 作为枚举结束哨兵；函数不增加引用、不修改状态，
 * 依赖 CMA 区域系统期常驻以及早期发布完成后列表不再变化。
 */
struct cma *dma_contiguous_get_area_by_idx(unsigned int idx)
{
	if (idx >= dma_contiguous_areas_num)
		return NULL;

	return dma_contiguous_areas[idx];
}

/* 仅向 GPL 模块导出只读区域枚举入口；导出不改变 struct cma 的 ownership。 */
EXPORT_SYMBOL_GPL(dma_contiguous_get_area_by_idx);

/*
 * dma_contiguous_default_area - 没有设备专用区域时使用的全局 CMA 区域
 *
 * 它可由 cma= 参数、Kconfig 策略或设备树 linux,cma-default 建立。指针只借用 CMA
 * 核心对象，早期初始化后系统期稳定；dev_get_cma_area(NULL) 也用它查询默认区域。
 */
static struct cma *dma_contiguous_default_area;

/*
 * Default global CMA area size can be defined in kernel's .config.
 * This is useful mainly for distro maintainers to create a kernel
 * that works correctly for most supported systems.
 * The size can be set in bytes or as a percentage of the total memory
 * in the system.
 *
 * Users, who want to set the size of global CMA area for their system
 * should use cma= kernel parameter.
 */
/*
 * 原注释翻译：发行版可在 .config 中用固定字节数或物理内存百分比定义默认全局
 * CMA 大小，以覆盖多数机器；具体系统管理员应优先使用 cma= 内核参数覆盖它。
 */
/* size_bytes 把 Kconfig 的 MiB 数转换为物理地址宽度的字节数，供选择策略使用。 */
#define size_bytes ((phys_addr_t)CMA_SIZE_MBYTES * SZ_1M)
/*
 * size_cmdline - cma= 指定的字节数；全 1 哨兵表示参数从未出现
 * base_cmdline - cma=size@base[-limit] 中的候选起始物理地址
 * limit_cmdline - 候选区间末地址；省略时保持 0，由体系结构上限补齐
 *
 * 三者仅供早期参数解析和 memblock 保留阶段使用，故标记 __initdata 并在启动后回收。
 */
static phys_addr_t  size_cmdline __initdata = -1;
static phys_addr_t base_cmdline __initdata;
static phys_addr_t limit_cmdline __initdata;

/*
 * early_cma - 解析 cma=size[@base[-limit]] 早期内核参数
 * @p: 参数值的可写解析游标；NULL 表示只出现参数名而没有值
 *
 * memparse() 依次消费带 K/M/G 后缀的数值。只有 size 时不改 base/limit（首次解析
 * 时二者初值为 0）；出现
 * @base 但没有 -limit 时，把 limit 推导为 base + size，后续据此识别固定区域；
 * 显式 -limit 则允许在 [base, limit) 中寻找区域。成功解析到当前支持的形式返回 0，
 * 缺失参数串返回 -EINVAL。函数不验证溢出/对齐，CMA 声明阶段会继续规范化和校验。
 * 它由 early_param 在 memblock 保留前调用，修改的仅是三个 __initdata 选择量。
 */
static int __init early_cma(char *p)
{
	if (!p) {
		pr_err("Config string not provided\n");
		return -EINVAL;
	}

	size_cmdline = memparse(p, &p);
	if (*p != '@')
		return 0;
	base_cmdline = memparse(p + 1, &p);
	if (*p != '-') {
		limit_cmdline = base_cmdline + size_cmdline;
		return 0;
	}
	limit_cmdline = memparse(p + 1, &p);

	return 0;
}

/* 在 early_param 表中登记 cma=，使其在常规参数和内存保留之前完成解析。 */
early_param("cma", early_cma);

/*
 * dev_get_cma_area - 选择设备专用 CMA 区域或全局默认区域
 * @dev: 待查询设备；允许为 NULL
 *
 * @dev 及 dev->cma_area 均有效时返回设备区域，否则返回
 * dma_contiguous_default_area。返回值是系统期 CMA 对象的借用指针，可能为 NULL；
 * 函数不加锁、不取得引用，依赖设备初始化/解绑与分配路径由外部正确串行化。
 */
struct cma *dev_get_cma_area(struct device *dev)
{
	if (dev && dev->cma_area)
		return dev->cma_area;

	return dma_contiguous_default_area;
}

/* 向 GPL 模块导出区域选择器；调用者得到的仍是无引用的借用指针。 */
EXPORT_SYMBOL_GPL(dev_get_cma_area);

#ifdef CONFIG_DMA_NUMA_CMA

/*
 * dma_contiguous_numa_area - 每个 NUMA 节点实际建立的 CMA 区域借用指针
 * numa_cma_size - numa_cma= 为各节点显式指定的字节数
 * pernuma_size_bytes - cma_pernuma= 或自动策略给所有节点的默认字节数
 * numa_cma_configured - 用户是否显式配置过任一 NUMA CMA 参数
 *
 *后三项仅在早期保留阶段使用并带 __initdata；区域指针在初始化后供运行期分配读取。
 */
static struct cma *dma_contiguous_numa_area[MAX_NUMNODES];
static phys_addr_t numa_cma_size[MAX_NUMNODES] __initdata;
static phys_addr_t pernuma_size_bytes __initdata;
static bool numa_cma_configured __initdata;

/*
 * early_numa_cma - 解析 numa_cma=nid:size[,nid:size...] 参数
 * @p: 以节点号开头的参数字符串
 *
 * @nid 保存经过边界与 nospec 约束的节点索引；@count 是 sscanf() 消费的字符数；
 * @tmp 先接节点号、再接 memparse() 得到的容量；@s 是当前解析游标。每个合法项写入
 * numa_cma_size[nid]，重复节点以后项覆盖前项。遇到格式错误或越界节点就停止，
 * 已解析项不会回滚；无论是否完整解析都设置 @numa_cma_configured 并返回 0，表示
 * 自动派生策略不应再覆盖用户意图。函数只在早期启动串行上下文执行。
 */
static int __init early_numa_cma(char *p)
{
	int nid, count = 0;
	unsigned long tmp;
	char *s = p;

	while (*s) {
		if (sscanf(s, "%lu%n", &tmp, &count) != 1)
			break;

		if (s[count] == ':') {
			if (tmp >= MAX_NUMNODES)
				break;
			nid = array_index_nospec(tmp, MAX_NUMNODES);

			s += count + 1;
			tmp = memparse(s, &s);
			numa_cma_size[nid] = tmp;

			if (*s == ',')
				s++;
			else
				break;
		} else
			break;
	}

	numa_cma_configured = true;
	return 0;
}

/* 登记逐节点容量参数 numa_cma=，由早期命令行扫描器调用上述解析函数。 */
early_param("numa_cma", early_numa_cma);

/*
 * early_cma_pernuma - 解析所有 NUMA 节点共用的 CMA 容量
 * @p: cma_pernuma= 后的容量字符串
 *
 * memparse() 的字节结果写入 @pernuma_size_bytes，并标记 NUMA CMA 已由用户配置。
 * 返回 0；不在此处验证节点在线状态或实际可保留容量，这些由后续 reserve 阶段处理。
 */
static int __init early_cma_pernuma(char *p)
{
	pernuma_size_bytes = memparse(p, &p);
	numa_cma_configured = true;
	return 0;
}

/* 登记统一每节点容量参数 cma_pernuma=，与 numa_cma= 共同关闭自动派生。 */
early_param("cma_pernuma", early_cma_pernuma);
#endif

#ifdef CONFIG_CMA_SIZE_PERCENTAGE

/*
 * cma_early_percent_memory - 计算 Kconfig 百分比策略对应的 CMA 字节数
 *
 * @total_pages 是 memblock 当前记录的全部物理内存页数。函数先乘
 * CONFIG_CMA_SIZE_PERCENTAGE 再除以 100，最后左移 PAGE_SHIFT 转回字节；返回值供
 * 默认 CMA 大小的 min/max/percentage 选择分支使用。仅在启动期调用，无副作用。
 */
static phys_addr_t __init __maybe_unused cma_early_percent_memory(void)
{
	unsigned long total_pages = PHYS_PFN(memblock_phys_mem_size());

	return (total_pages * CONFIG_CMA_SIZE_PERCENTAGE / 100) << PAGE_SHIFT;
}

#else

/*
 * cma_early_percent_memory - 未启用百分比策略时的编译期占位实现
 *
 * 无参数、无副作用，固定返回 0；__maybe_unused 允许固定 MiB 策略不引用它。
 */
static inline __maybe_unused phys_addr_t cma_early_percent_memory(void)
{
	return 0;
}

#endif

#ifdef CONFIG_DMA_NUMA_CMA
/*
 * dma_numa_cma_reserve - 为在线 NUMA 节点声明各自的 CMA 区域
 *
 * 没有显式 NUMA 参数、启用 CMA_SIZE_PERNUMA、已存在默认区且在线节点多于一个时，
 * 自动把默认区大小作为每节点目标容量。随后遍历所有可能节点：离线但被指定容量的
 * 节点只告警；在线节点优先使用 numa_cma_size[nid]，否则使用统一容量，零容量跳过。
 *
 * 局部 @nid 是节点号；int 类型的 @size 承接本节点最终字节数，实际配置必须能以
 * 正 int 表示，避免从 phys_addr_t 截断；@ret 接收声明错误；@name 生成
 * “numaN”区域名；@cma 指向 dma_contiguous_numa_area[nid] 输出槽。声明失败只告警并
 * 继续其他节点，不回滚既有区域。函数在 memblock 可用的串行启动期执行，成功区域
 * 系统期常驻；没有运行期锁或释放路径。
 */
static void __init dma_numa_cma_reserve(void)
{
	int nid;

	if (IS_ENABLED(CONFIG_CMA_SIZE_PERNUMA) &&
	    !numa_cma_configured && dma_contiguous_default_area &&
	    nr_online_nodes > 1)
		pernuma_size_bytes = cma_get_size(dma_contiguous_default_area);

	for_each_node(nid) {
		int size, ret;
		char name[CMA_MAX_NAME];
		struct cma **cma;

		if (!node_online(nid)) {
			if (pernuma_size_bytes || numa_cma_size[nid])
				pr_warn("invalid node %d specified\n", nid);
			continue;
		}

		/* per-node numa setting has the priority */
		/* 原注释翻译：逐节点 numa_cma= 设置优先于统一的 cma_pernuma= 容量。 */
		size = numa_cma_size[nid] ?: pernuma_size_bytes;
		if (!size)
			continue;

		cma = &dma_contiguous_numa_area[nid];
		snprintf(name, sizeof(name), "numa%d", nid);
		ret = cma_declare_contiguous_nid(0, size, 0, 0, 0, false, name, cma, nid);
		if (ret)
			pr_warn("%s: reservation failed: err %d, node %d", __func__,
				ret, nid);
	}
}
#else
/*
 * dma_numa_cma_reserve - CONFIG_DMA_NUMA_CMA 关闭时的空占位
 *
 * 无参数、无返回值和副作用，使默认 CMA 保留主流程无需条件化调用点。
 */
static inline void __init dma_numa_cma_reserve(void)
{
}
#endif

/**
 * dma_contiguous_reserve() - reserve area(s) for contiguous memory handling
 * @limit: End address of the reserved memory (optional, 0 for any).
 *
 * This function reserves memory from early allocator. It should be
 * called by arch specific code once the early allocator (memblock or bootmem)
 * has been activated and all other subsystems have already allocated/reserved
 * memory.
 */
/*
 * dma_contiguous_reserve - 选择并保留默认及 NUMA CMA 区域
 * @limit: 体系结构允许默认 CMA 使用的物理末地址；0 表示由 CMA 核心取 DRAM 末端
 *
 * 原 kernel-doc 翻译与展开：体系结构应在 memblock/bootmem 已启用且其他子系统完成
 * 早期保留后调用。@selected_size/@selected_base/@selected_limit 是最终交给 CMA 核心
 * 的三元组，@fixed 表示必须精确使用 base；它们先以体系结构 limit 初始化。
 *
 * cma= 命令行存在时完全覆盖 Kconfig 大小策略；没有命令行时才按固定 MiB、百分比、
 * 二者 min 或 max 的配置选择。命令行的 @base 且末端恰为 base+size 时判为固定区。
 * 若目标非零且尚无设备树默认区，则调用 dma_contiguous_reserve_area()；@ret 记录
 * 保留或列表插入结果。保留失败会直接返回，连 NUMA 保留也不再执行；列表插入失败
 * 仅告警，默认指针仍可用于分配。最后为各 NUMA 节点尝试独立保留。
 *
 * 函数仅在串行启动期调用；它修改 memblock 预留状态与全局区域指针，不提供回滚。
 */
void __init dma_contiguous_reserve(phys_addr_t limit)
{
	phys_addr_t selected_size = 0;
	phys_addr_t selected_base = 0;
	phys_addr_t selected_limit = limit;
	bool fixed = false;

	pr_debug("%s(limit %08lx)\n", __func__, (unsigned long)limit);

	if (size_cmdline != -1) {
		selected_size = size_cmdline;
		selected_base = base_cmdline;

		/* Hornor the user setup dma address limit */
		/* 原注释翻译：遵从用户通过 cma= 设置的 DMA 地址上限。 */
		selected_limit = limit_cmdline ?: limit;

		if (base_cmdline + size_cmdline == limit_cmdline)
			fixed = true;
	} else {
#ifdef CONFIG_CMA_SIZE_SEL_MBYTES
		selected_size = size_bytes;
#elif defined(CONFIG_CMA_SIZE_SEL_PERCENTAGE)
		selected_size = cma_early_percent_memory();
#elif defined(CONFIG_CMA_SIZE_SEL_MIN)
		selected_size = min(size_bytes, cma_early_percent_memory());
#elif defined(CONFIG_CMA_SIZE_SEL_MAX)
		selected_size = max(size_bytes, cma_early_percent_memory());
#endif
	}

	if (selected_size && !dma_contiguous_default_area) {
		int ret;

		pr_debug("%s: reserving %ld MiB for global area\n", __func__,
			 (unsigned long)selected_size / SZ_1M);

		ret = dma_contiguous_reserve_area(selected_size, selected_base,
						  selected_limit,
						  &dma_contiguous_default_area,
						  fixed);
		if (ret)
			return;

		/*
		 * We need to insert the new area in our list to avoid
		 * any inconsistencies between having the default area
		 * listed in the DT or not.
		 *
		 * The DT case is handled by rmem_cma_setup() and will
		 * always insert all its areas in our list. However, if
		 * it didn't run (because OF_RESERVED_MEM isn't set, or
		 * there's no DT region specified), then we don't have a
		 * default area yet, and no area in our list.
		 *
		 * This block creates the default area in such a case,
		 * but we also need to insert it in our list to avoid
		 * having a default area but an empty list.
		 */
		/*
		 * 原注释翻译与展开：设备树路径 rmem_cma_setup() 会把所有区域加入枚举列表；
		 * 若没有 OF reserved-memory 或没有 DT 区域，本分支自行建立默认区，也必须
		 * 同步插入列表，避免“默认区存在但 CMA heap 枚举结果为空”的不一致状态。
		 */
		ret = dma_contiguous_insert_area(dma_contiguous_default_area);
		if (ret)
			pr_warn("Couldn't queue default CMA region for heap creation.");
	}

	dma_numa_cma_reserve();
}

/*
 * dma_contiguous_early_fixup - 体系结构可覆盖的 CMA 早期修正钩子
 * @base: 已成功保留区域的物理起始地址
 * @size: 已成功保留区域的字节数
 *
 * 默认弱实现无操作；体系结构可提供强符号，在区域声明后、常规运行期使用前更新
 * 页表或平台内存属性。无返回值意味着修正无法向通用路径报告失败，覆盖实现必须在
 * 启动期上下文自行处理其约束。
 */
void __weak
dma_contiguous_early_fixup(phys_addr_t base, unsigned long size)
{
}

/**
 * dma_contiguous_reserve_area() - reserve custom contiguous area
 * @size: Size of the reserved area (in bytes),
 * @base: Base address of the reserved area optional, use 0 for any
 * @limit: End address of the reserved memory (optional, 0 for any).
 * @res_cma: Pointer to store the created cma region.
 * @fixed: hint about where to place the reserved area
 *
 * This function reserves memory from early allocator. It should be
 * called by arch specific code once the early allocator (memblock or bootmem)
 * has been activated and all other subsystems have already allocated/reserved
 * memory. This function allows to create custom reserved areas for specific
 * devices.
 *
 * If @fixed is true, reserve contiguous area at exactly @base.  If false,
 * reserve in range from @base to @limit.
 */
/*
 * dma_contiguous_reserve_area - 从早期分配器声明一个自定义 CMA 区域
 * @size: 请求容量，单位字节
 * @base: 候选或固定物理起始地址；0 表示任意
 * @limit: 搜索区间的物理末地址；0 表示任意 DRAM 范围
 * @res_cma: 成功时接收 CMA 核心创建的区域借用指针
 * @fixed: 为真时必须精确放在 @base，为假时可在 [@base, @limit) 内选择
 *
 * 原 kernel-doc 翻译与展开：体系结构在早期分配器启用、其他保留完成后调用，用于
 * 默认区之外的设备专用区域。@ret 传递 cma_declare_contiguous() 的校验、容量或
 * memblock 保留错误；失败时直接返回且不调用体系结构钩子。成功后用实际区域的
 * base/size 调用 dma_contiguous_early_fixup()，再返回 0。区域所有权留在 CMA 核心，
 * *@res_cma 只是系统期借用指针；整个函数带 __init，只能在串行启动期使用。
 */
int __init dma_contiguous_reserve_area(phys_addr_t size, phys_addr_t base,
				       phys_addr_t limit, struct cma **res_cma,
				       bool fixed)
{
	int ret;

	ret = cma_declare_contiguous(base, size, limit, 0, 0, fixed,
					"reserved", res_cma);
	if (ret)
		return ret;

	/* Architecture specific contiguous memory fixup. */
	/* 原注释翻译：对已经成功保留的连续内存执行体系结构专用早期修正。 */
	dma_contiguous_early_fixup(cma_get_base(*res_cma),
				cma_get_size(*res_cma));

	return 0;
}

/**
 * dma_alloc_from_contiguous() - allocate pages from contiguous area
 * @dev:   Pointer to device for which the allocation is performed.
 * @count: Requested number of pages.
 * @align: Requested alignment of pages (in PAGE_SIZE order).
 * @no_warn: Avoid printing message about failed allocation.
 *
 * This function allocates memory buffer for specified device. It uses
 * device specific contiguous memory area if available or the default
 * global one. Requires architecture specific dev_get_cma_area() helper
 * function.
 */
/*
 * dma_alloc_from_contiguous - 从设备选定的单一 CMA 区域分配连续页
 * @dev: 请求设备；其专用区优先，否则使用全局默认区
 * @count: 请求的连续页数
 * @align: 以 PAGE_SIZE 阶数表示的起始页对齐要求
 * @no_warn: 分配失败时是否抑制 CMA 核心告警
 *
 * 原 kernel-doc 翻译与展开：先把 @align 限制到 CONFIG_CMA_ALIGNMENT，防止超过平台
 * 支持的最大对齐阶，再调用 cma_alloc()。成功返回首页 struct page，页面引用已建立，
 * 调用者拥有 @count 页并须用 dma_release_from_contiguous() 成对归还；区域不存在、
 * 迁移/隔离失败或空间不足返回 NULL。CMA 分配可能迁移页面并睡眠，不能在原子上下文。
 */
struct page *dma_alloc_from_contiguous(struct device *dev, size_t count,
				       unsigned int align, bool no_warn)
{
	if (align > CONFIG_CMA_ALIGNMENT)
		align = CONFIG_CMA_ALIGNMENT;

	return cma_alloc(dev_get_cma_area(dev), count, align, no_warn);
}

/**
 * dma_release_from_contiguous() - release allocated pages
 * @dev:   Pointer to device for which the pages were allocated.
 * @pages: Allocated pages.
 * @count: Number of allocated pages.
 *
 * This function releases memory allocated by dma_alloc_from_contiguous().
 * It returns false when provided pages do not belong to contiguous area and
 * true otherwise.
 */
/*
 * dma_release_from_contiguous - 向设备选定的 CMA 区域归还连续页
 * @dev: 原分配使用的设备
 * @pages: 原分配返回的首页
 * @count: 必须与原分配一致的正页数
 *
 * 原 kernel-doc 翻译与展开：cma_release() 确认页区间属于所选区域后，逐页放下引用、
 * 归还连续范围并清位图，返回 true；区域为空或页不属于它返回 false，状态不变。
 * 调用者必须先停止所有 CPU/设备用户，且不能重复释放。CMA 核心内部负责同步；本
 * 包装函数不持有额外引用。
 */
bool dma_release_from_contiguous(struct device *dev, struct page *pages,
				 int count)
{
	return cma_release(dev_get_cma_area(dev), pages, count);
}

/*
 * cma_alloc_aligned - 按请求大小选择合理对齐并从指定区域分配
 * @cma: 目标 CMA 区域；允许为 NULL，CMA 核心会返回失败
 * @size: 请求字节数；调用者应已按页对齐且非零
 * @gfp: 用于判断是否抑制失败告警的分配标志
 *
 * 局部 @align 取 get_order(@size) 与 CONFIG_CMA_ALIGNMENT 的较小值，使常见二次幂
 * 缓冲区按自身大小对齐但不超过平台上限。传给 cma_alloc() 的页数通过右移得到，
 * 因此非页对齐尾部不会在这里向上补齐。成功/失败、睡眠和 ownership 契约同
 * cma_alloc()；@gfp 中除 __GFP_NOWARN 外的位不会继续传入 CMA 核心。
 */
static struct page *cma_alloc_aligned(struct cma *cma, size_t size, gfp_t gfp)
{
	unsigned int align = min(get_order(size), CONFIG_CMA_ALIGNMENT);

	return cma_alloc(cma, size >> PAGE_SHIFT, align, gfp & __GFP_NOWARN);
}

/**
 * dma_alloc_contiguous() - allocate contiguous pages
 * @dev:   Pointer to device for which the allocation is performed.
 * @size:  Requested allocation size.
 * @gfp:   Allocation flags.
 *
 * tries to use device specific contiguous memory area if available, or it
 * tries to use per-numa cma, if the allocation fails, it will fallback to
 * try default global one.
 *
 * Note that it bypass one-page size of allocations from the per-numa and
 * global area as the addresses within one page are always contiguous, so
 * there is no need to waste CMA pages for that kind; it also helps reduce
 * fragmentations.
 */
/*
 * dma_alloc_contiguous - 按设备、NUMA 和默认策略尝试取得物理连续页
 * @dev: 请求设备；必须有效，用于专用 CMA 与 NUMA 节点选择
 * @size: 请求字节数；调用路径应已按页对齐
 * @gfp: 上下文、DMA zone 与告警策略标志
 *
 * 原 kernel-doc 翻译与展开：不允许阻塞时立即返回 NULL，因为 CMA 可能迁移页并
 * 睡眠。设备有专用 dev->cma_area 时只尝试该区，成功或失败都不回退；没有专用区
 * 且请求不超过一页时返回 NULL，让上层使用 buddy，避免浪费/碎片化 CMA。
 *
 * 启用 NUMA 时，局部 @nid 是设备节点。只有节点有效且请求不带 GFP_DMA/GFP_DMA32
 * 才尝试该节点的 @cma；局部 @page 保存结果，成功立即返回，失败再回退默认区。
 * 这些 zone 标志会跳过 NUMA CMA，避免从不保证低地址约束的节点区取页。默认区不
 * 存在或最终分配失败返回 NULL。成功返回首页并把页面 ownership 交给调用者。
 */
struct page *dma_alloc_contiguous(struct device *dev, size_t size, gfp_t gfp)
{
#ifdef CONFIG_DMA_NUMA_CMA
	int nid = dev_to_node(dev);
#endif

	/* CMA can be used only in the context which permits sleeping */
	/* 原注释翻译：CMA 只能用于允许睡眠的调用上下文。 */
	if (!gfpflags_allow_blocking(gfp))
		return NULL;
	if (dev->cma_area)
		return cma_alloc_aligned(dev->cma_area, size, gfp);
	if (size <= PAGE_SIZE)
		return NULL;

#ifdef CONFIG_DMA_NUMA_CMA
	if (nid != NUMA_NO_NODE && !(gfp & (GFP_DMA | GFP_DMA32))) {
		struct cma *cma = dma_contiguous_numa_area[nid];
		struct page *page;
		if (cma) {
			page = cma_alloc_aligned(cma, size, gfp);
			if (page)
				return page;
		}
	}
#endif
	if (!dma_contiguous_default_area)
		return NULL;

	return cma_alloc_aligned(dma_contiguous_default_area, size, gfp);
}

/**
 * dma_free_contiguous() - release allocated pages
 * @dev:   Pointer to device for which the pages were allocated.
 * @page:  Pointer to the allocated pages.
 * @size:  Size of allocated pages.
 *
 * This function releases memory allocated by dma_alloc_contiguous(). As the
 * cma_release returns false when provided pages do not belong to contiguous
 * area and true otherwise, this function then does a fallback __free_pages()
 * upon a false-return.
 */
/*
 * dma_free_contiguous - 释放 dma_alloc_contiguous() 交付的页面
 * @dev: 原分配设备，用于重建区域选择分支
 * @page: 原分配返回的首页
 * @size: 原请求字节数，必须与分配配对
 *
 * 原 kernel-doc 翻译与展开：局部 @count 把字节数向上页对齐后转成页数。设备有
 * 专用区时只向该区 release；没有专用区时，先按 @page 的实际 NUMA 节点尝试节点
 * CMA，再尝试默认 CMA。任一 cma_release() 返回 true 就结束。全部返回 false 说明
 * 页面不是 CMA 所有，最后以 get_order(@size) 归还 buddy。这一回退与分配顺序配对：
 * 专用区分配从不回退，NUMA 分配失败才会落到默认区，而小页/非 CMA 由 buddy 提供。
 *
 * 调用者必须先停止 DMA 与其他引用；错误的 @size、@dev 或重复释放会破坏对应后端。
 */
void dma_free_contiguous(struct device *dev, struct page *page, size_t size)
{
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;

	/* if dev has its own cma, free page from there */
	/* 原注释翻译：若设备有专用 CMA，先且只先尝试归还到该区域。 */
	if (dev->cma_area) {
		if (cma_release(dev->cma_area, page, count))
			return;
	} else {
		/*
		 * otherwise, page is from either per-numa cma or default cma
		 */
		/* 原注释翻译：否则页面可能来自每 NUMA 节点 CMA 或全局默认 CMA。 */
#ifdef CONFIG_DMA_NUMA_CMA
		if (cma_release(dma_contiguous_numa_area[page_to_nid(page)],
					page, count))
			return;
#endif
		if (cma_release(dma_contiguous_default_area, page, count))
			return;
	}

	/* not in any cma, free from buddy */
	/* 原注释翻译：若不属于任何候选 CMA，则按原页阶归还 buddy 分配器。 */
	__free_pages(page, get_order(size));
}

/*
 * Support for reserved memory regions defined in device tree
 */
/*
 * 原注释翻译与章节说明：以下代码支持设备树 reserved-memory 定义的 CMA 区域。
 * 匹配 shared-dma-pool 后，框架依次验证对齐、必要时做体系结构修正、建立 CMA 对象，
 * 再在设备引用该区域时把对象挂到 dev->cma_area。区域必须 reusable 且不能 no-map，
 * 因为普通可迁移页面需要暂时进入并通过正常 CPU 映射访问该范围。
 */
#ifdef CONFIG_OF_RESERVED_MEM
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>

#undef pr_fmt
/* reserved-memory 日志已经自带上下文，以下回调取消文件顶部的 "cma: " 前缀。 */
#define pr_fmt(fmt) fmt

/*
 * rmem_cma_device_init - 把设备树 CMA 区域挂到设备
 * @rmem: 已完成 node_init、其 priv 指向 struct cma 的保留内存描述符
 * @dev: 使用该区域的设备
 *
 * 直接令 dev->cma_area 借用 rmem->priv 并返回 0；不取得引用、不检查或释放原区域，
 * 因而 reserved-memory 框架必须在设备初始化串行阶段正确配对调用。后续
 * dev_get_cma_area() 会优先返回此专用区域。
 */
static int rmem_cma_device_init(struct reserved_mem *rmem, struct device *dev)
{
	dev->cma_area = rmem->priv;
	return 0;
}

/*
 * rmem_cma_device_release - 解除设备对设备树 CMA 区域的借用
 * @rmem: 对应 reserved-memory 描述符；区域仍由 CMA 核心持有
 * @dev: 待解绑设备，调用契约要求有效
 *
 * 仅清空 dev->cma_area，不销毁 CMA 区域，因为区域可能是默认区或被其他设备共享。
 * 调用前必须释放该设备所有 CMA 页面并阻止新分配；函数无锁，依赖设备拆除串行化。
 */
static void rmem_cma_device_release(struct reserved_mem *rmem,
				    struct device *dev)
{
	dev->cma_area = NULL;
}

/*
 * __rmem_cma_verify_node - 验证 shared-dma-pool 是否适合作为 CMA
 * @node: 扁平设备树中的节点偏移
 *
 * CMA 区域必须带 reusable，且不能带 no-map；不满足返回 -ENODEV。若 cma= 已出现，
 * 它优先于设备树的 linux,cma-default，此类默认节点返回 -EBUSY 并记录错误，但普通
 * 非默认 CMA 节点仍可继续。其余返回 0。函数只读早期 FDT 与 __initdata 参数状态。
 */
static int __init __rmem_cma_verify_node(unsigned long node)
{
	if (!of_get_flat_dt_prop(node, "reusable", NULL) ||
	    of_get_flat_dt_prop(node, "no-map", NULL))
		return -ENODEV;

	if (size_cmdline != -1 &&
	    of_get_flat_dt_prop(node, "linux,cma-default", NULL)) {
		pr_err("Skipping dt linux,cma-default node in favor for \"cma=\" kernel param.\n");
		return -EBUSY;
	}
	return 0;
}

/*
 * rmem_cma_validate - OF 布局阶段验证节点并提高所需对齐
 * @node: 扁平设备树节点偏移
 * @align: 框架维护的物理对齐要求；允许为 NULL
 *
 * 局部 @ret 传递公共节点验证结果，失败原样返回。成功且 @align 有效时，把它提升为
 * 原要求与 CMA_MIN_ALIGNMENT_BYTES 的较大值，确保 base/size 能按 pageblock 工作；
 * 返回 0。这里只写输出约束，不创建或保留区域。
 */
static int __init rmem_cma_validate(unsigned long node, phys_addr_t *align)
{
	int ret = __rmem_cma_verify_node(node);

	if (ret)
		return ret;

	if (align)
		*align = max_t(phys_addr_t, *align, CMA_MIN_ALIGNMENT_BYTES);

	return 0;
}

/*
 * rmem_cma_fixup - reserved-memory 地址确定后执行体系结构 CMA 修正
 * @node: 扁平设备树节点偏移
 * @base: 已选定区域的物理起始地址
 * @size: 已选定区域的字节数
 *
 * 局部 @ret 先重复验证节点，失败时不触碰区域；成功后调用无返回值的体系结构钩子
 * 并返回 0。函数在早期启动期运行，修正必须在区域交给运行期 CMA 前完成。
 */
static int __init rmem_cma_fixup(unsigned long node, phys_addr_t base,
				    phys_addr_t size)
{
	int ret = __rmem_cma_verify_node(node);

	if (ret)
		return ret;

	/* Architecture specific contiguous memory fixup. */
	/* 原注释翻译：执行体系结构专用的连续内存早期修正。 */
	dma_contiguous_early_fixup(base, size);
	return 0;
}

/*
 * rmem_cma_setup - 从已保留的设备树区域建立并发布 CMA 对象
 * @node: 扁平设备树节点偏移
 * @rmem: 含名称、物理 base 和 size 的 reserved-memory 描述符
 *
 * 局部 @default_cma 记录 linux,cma-default 属性；@cma 接收 CMA 核心对象；@ret
 * 串联验证、初始化和列表插入状态。函数先验证 reusable/no-map/命令行优先级，再
 * 明确检查 base 与 size 均满足 CMA_MIN_ALIGNMENT_BYTES。随后
 * cma_init_reserved_mem() 把已由框架保留的 memblock 区域登记进 CMA 核心。
 *
 * 初始化失败返回具体错误且不发布指针；成功后按属性更新全局默认区，再把 @cma
 * 存入 rmem->priv 供设备回调借用。最后追加枚举列表：容量满只告警，仍返回 0，
 * 因而区域和默认/设备分配保持可用，但 CMA heap 等枚举者可能看不到它。函数带
 * __init，所有发布依赖早期启动串行化，CMA 对象及区域随后系统期常驻。
 */
static int __init rmem_cma_setup(unsigned long node, struct reserved_mem *rmem)
{
	bool default_cma = of_get_flat_dt_prop(node, "linux,cma-default", NULL);
	struct cma *cma;
	int ret;

	ret = __rmem_cma_verify_node(node);
	if (ret)
		return ret;

	if (!IS_ALIGNED(rmem->base | rmem->size, CMA_MIN_ALIGNMENT_BYTES)) {
		pr_err("Reserved memory: incorrect alignment of CMA region\n");
		return -EINVAL;
	}

	ret = cma_init_reserved_mem(rmem->base, rmem->size, 0, rmem->name, &cma);
	if (ret) {
		pr_err("Reserved memory: unable to setup CMA region\n");
		return ret;
	}

	if (default_cma)
		dma_contiguous_default_area = cma;

	rmem->priv = cma;

	pr_info("Reserved memory: created CMA memory pool at %pa, size %ld MiB\n",
		&rmem->base, (unsigned long)rmem->size / SZ_1M);

	ret = dma_contiguous_insert_area(cma);
	if (ret)
		pr_warn("Couldn't store CMA reserved area.");

	return 0;
}

/*
 * rmem_cma_ops - CMA 型 shared-dma-pool 的 reserved-memory 回调表
 * @node_validate: 在布局前校验节点并给出最低对齐
 * @node_fixup: 地址确定后执行体系结构修正
 * @node_init: 从已保留范围建立 CMA 对象并发布
 * @device_init: 设备绑定时挂接 dev->cma_area
 * @device_release: 设备解绑时只清空借用指针
 *
 * 只读函数指针表在内核镜像期常驻；reserved-memory 核心负责按阶段调用，区域实际
 * ownership 始终属于 CMA 核心而非此表或单个设备。
 */
static const struct reserved_mem_ops rmem_cma_ops = {
	.node_validate  = rmem_cma_validate,
	.node_fixup	= rmem_cma_fixup,
	.node_init	= rmem_cma_setup,
	.device_init	= rmem_cma_device_init,
	.device_release = rmem_cma_device_release,
};

/*
 * 将“shared-dma-pool”兼容串的 CMA 实现登记到早期 OF 声明表。reserved-memory
 * 核心依据节点属性与上述 validate/init 结果选择适用实现；该宏本身不分配页面。
 */
RESERVEDMEM_OF_DECLARE(cma, "shared-dma-pool", &rmem_cma_ops);
#endif
