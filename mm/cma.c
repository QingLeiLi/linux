// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Contiguous Memory Allocator
 *
 * Copyright (c) 2010-2011 by Samsung Electronics.
 * Copyright IBM Corporation, 2013
 * Copyright LG Electronics Inc., 2014
 * Written by:
 *	Marek Szyprowski <m.szyprowski@samsung.com>
 *	Michal Nazarewicz <mina86@mina86.com>
 *	Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 *	Joonsoo Kim <iamjoonsoo.kim@lge.com>
 */

#define pr_fmt(fmt) "cma: " fmt

#define CREATE_TRACE_POINTS

/* tracepoints 记录 CMA 分配开始、busy 重试、成功/失败和释放，便于把位图状态关联到迁移结果。 */
#include <linux/memblock.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/string_choices.h>
#include <linux/log2.h>
#include <linux/cma.h>
/* cma.h 定义 area/range/flag 和公开分配接口；本文件实现其状态机。 */
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/kmemleak.h>
#include <trace/events/cma.h>

/* CMA 将启动期保留的物理页转换为可在运行期迁移腾挪后取得连续块的分配池。 */
#include "internal.h"
#include "cma.h"

/*
 * cma_areas/cma_area_count - 启动期登记的全局 CMA area 表及其有效长度。
 * 业务背景：早期 memblock 声明先占用静态槽位，core_initcall 激活后供运行期分配器遍历。
 * 输入/输出：下标 [0,cma_area_count) 有效；两个对象均由本文件拥有，外部只能借用 area。
 * 并发/生命周期：计数仅在单线程 __init 路径增减，激活后固定；每个 area 的可变位图另由自身锁保护。
 */
struct cma cma_areas[MAX_CMA_AREAS];
unsigned int cma_area_count;

/*
 * cma_get_base() - 返回单 range CMA 的物理起始地址。
 * 业务背景：只为把传统单段 area 暴露成一个物理基址；多 range 没有唯一连续基址。
 * 入参：cma 是已登记 area 的非 NULL 借用指针，调用期间必须保持有效。
 * 返回/副作用：返回 ranges[0] 的字节地址，不转移 ownership；多 range 仍返回首段但触发一次 WARN。
 * 注意事项：不加锁，因为几何信息激活后只读；调用者不能据此推导多 range 的整体连续性。
 */
phys_addr_t cma_get_base(const struct cma *cma)
{
	WARN_ON_ONCE(cma->nranges != 1);
	return PFN_PHYS(cma->ranges[0].base_pfn);
}

/*
 * cma_get_size() - 返回 area 声明的总字节数。
 * 业务背景：为不需要了解 PFN/多 range 布局的调用者提供总容量查询。
 * 入参：cma 为有效借用指针；count 已按基础页计量且在激活后不增长。
 * 返回/副作用：返回 count << PAGE_SHIFT，无状态修改和 ownership 变化；失败激活的 area 返回 0。
 * 注意事项：不加锁且不汇总 ranges；结果表示逻辑总容量，不保证物理连续。
 */
unsigned long cma_get_size(const struct cma *cma)
{
	return cma->count << PAGE_SHIFT;
}

/*
 * cma_get_name() - 返回启动期写入的 area 名称。
 * 业务背景：trace、sysfs 和客户日志用稳定名称关联同一个 CMA 池。
 * 入参：cma 为非 NULL 借用指针；name 在 cma 全寿命内只读。
 * 返回/副作用：返回内部字符数组的借用指针，无分配和同步；调用者不得写入或释放。
 * 注意事项：返回值不增加 area 生命周期，不能保存到 cma 全局寿命之外。
 */
const char *cma_get_name(const struct cma *cma)
{
	return cma->name;
}
EXPORT_SYMBOL_GPL(cma_get_name);

/*
 * cma_bitmap_aligned_mask() - 把页阶对齐要求换算成 bitmap 搜索掩码。
 * 业务背景：bitmap_find_next_zero_area_off() 以 bit 为单位，而客户 align 以 PAGE_SIZE 阶表示。
 * 入参：cma 为借用 area；align_order 是请求首页的页阶，须小于机器字可移位范围。
 * 返回/副作用：要求不强于 bit 粒度时返回 0，否则返回低位 mask；纯计算，不修改状态。
 * 注意事项：与 aligned_offset 配套使用，单独使用 mask 无法补偿 range 基址偏移。
 */
static unsigned long cma_bitmap_aligned_mask(const struct cma *cma,
					     unsigned int align_order)
{
	if (align_order <= cma->order_per_bit)
		return 0;
	return (1UL << (align_order - cma->order_per_bit)) - 1;
}

/*
 * Find the offset of the base PFN from the specified align_order.
 * The value returned is represented in order_per_bits.
 */
/*
 * cma_bitmap_aligned_offset() - 计算 range 基址相对请求对齐的 bitmap 偏移。
 * 业务背景：物理基址若未对齐到请求阶，搜索必须补偿前导 bit 才能使最终 PFN 对齐。
 * 入参：cma/cmr 为匹配的借用对象，align_order 是 PAGE_SIZE 阶；只读取几何信息。
 * 返回/副作用：返回以 2^order_per_bit 页为单位的 offset，无状态修改；与 aligned_mask 配套。
 * 注意事项：调用者须保证移位阶合法；本函数不验证 cmr 是否属于 cma。
 */
static unsigned long cma_bitmap_aligned_offset(const struct cma *cma,
					       const struct cma_memrange *cmr,
					       unsigned int align_order)
{
	return (cmr->base_pfn & ((1UL << align_order) - 1))
		>> cma->order_per_bit;
}

/*
 * cma_bitmap_pages_to_bits() - 把基础页数向上换算为 bitmap bit 数。
 * 业务背景：一个 bit 可覆盖多页，非整粒度请求也必须独占最后一个完整 bit。
 * 入参：cma 为借用 area，pages 为基础页数；调用者负责防止 ALIGN 溢出。
 * 返回/副作用：返回 ceil(pages / 2^order_per_bit)，纯计算；pages 为 0 时返回 0。
 * 注意事项：向上取整意味着最后一个 bit 可能覆盖请求范围之外的页，释放必须使用同一 count。
 */
static unsigned long cma_bitmap_pages_to_bits(const struct cma *cma,
					      unsigned long pages)
{
	return ALIGN(pages, 1UL << cma->order_per_bit) >> cma->order_per_bit;
}

/*
 * cma_clear_bitmap() - 归还指定 PFN 区间的 CMA 记账。
 * 业务背景：连续页释放或迁移失败回滚后，让同一范围重新参与 bitmap 搜索。
 * 入参：cma/cmr 是匹配的借用对象；[pfn,pfn+count) 必须位于 cmr 且由当前请求占用。
 * 返回/副作用：无返回；清除向上取整的 bitmap bits，并把实际请求 count 加回 available_count。
 * 注意事项：spin_lock_irqsave 原子提交两项状态；调用者须先完成底层页状态回滚，且不得重复清除。
 */
static void cma_clear_bitmap(struct cma *cma, const struct cma_memrange *cmr,
			     unsigned long pfn, unsigned long count)
{
	unsigned long bitmap_no, bitmap_count;
	unsigned long flags;

	bitmap_no = (pfn - cmr->base_pfn) >> cma->order_per_bit;
	bitmap_count = cma_bitmap_pages_to_bits(cma, count);

	/* 位图清零与可用计数恢复是同一状态转换，必须在一个临界区内提交。 */
	spin_lock_irqsave(&cma->lock, flags);
	bitmap_clear(cmr->bitmap, bitmap_no, bitmap_count);
	cma->available_count += count;
	spin_unlock_irqrestore(&cma->lock, flags);
}

/*
 * Check if a CMA area contains no ranges that intersect with
 * multiple zones. Store the result in the flags in case
 * this gets called more than once.
 */
/*
 * cma_validate_zones() - 验证每个 CMA range 不跨越 zone，并缓存结论。
 * 业务背景：alloc_contig_range() 要求一次 PFN 范围位于同一 zone，激活前必须排除非法几何。
 * 入参：cma 为已登记但可尚未激活的借用 area；ranges/nid 必须已经定稿。
 * 返回/副作用：全部合法返回 true 并置 VALID；任一跨 zone 返回 false 并置 INVALID。
 * 注意事项：flags 只按位操作但未把“检查中”串行化，设计调用场景是启动期；缓存后不再重扫。
 */
bool cma_validate_zones(struct cma *cma)
{
	int r;
	unsigned long base_pfn;
	struct cma_memrange *cmr;
	bool valid_bit_set;

	/*
	 * If already validated, return result of previous check.
	 * Either the valid or invalid bit will be set if this
	 * check has already been done. If neither is set, the
	 * check has not been performed yet.
	 */
	/* valid/invalid 任一已置都代表启动期检查已结束，结果不可再由后续调用改变。 */
	valid_bit_set = test_bit(CMA_ZONES_VALID, &cma->flags);
	if (valid_bit_set || test_bit(CMA_ZONES_INVALID, &cma->flags))
		/* 一经缓存，调用者得到同一布尔结论，避免运行期重复做可能昂贵的 zone 检查。 */
		return valid_bit_set;

	/* 每个物理 range 必须完整落在 cma->nid 的一个 zone 内。 */
	for (r = 0; r < cma->nranges; r++) {
		cmr = &cma->ranges[r];
		/* start/end 均以基础 PFN 计；free_reserved_page 才把每页重新交伙伴系统。 */
		base_pfn = cmr->base_pfn;

		/*
		 * alloc_contig_range() requires the pfn range specified
		 * to be in the same zone. Simplify by forcing the entire
		 * CMA resv range to be in the same zone.
		 */
		WARN_ON_ONCE(!pfn_valid(base_pfn));
		/* 无有效 memmap 本身已违反已保留区应可初始化为 pageblock 的前提。 */
		if (pfn_range_intersects_zones(cma->nid, base_pfn, cmr->count)) {
			set_bit(CMA_ZONES_INVALID, &cma->flags);
			return false;
		}
	}

	set_bit(CMA_ZONES_VALID, &cma->flags);

	return true;
}

/*
 * cma_activate_area() - 把启动期描述符转换为可运行的 CMA 池。
 * 业务背景：memblock 阶段只能登记范围；slab、memmap 和 pageblock 可用后才建立 bitmap/锁。
 * 入参：cma 为全局表中的未激活借用对象；nranges、early_pfn、count 和 nid 必须已经定稿。
 * 返回/副作用：无返回；成功初始化所有 range、锁与 pageblock 后置 ACTIVATED；失败将 count 清零。
 * 失败/ownership：释放已建 bitmap，并按 RESERVE_PAGES_ON_ERROR 决定是否把未早期取走的后缀交 buddy。
 * 注意事项：只允许统一 initcall 调用一次；部分激活状态不可重入或重试。
 */
static void __init cma_activate_area(struct cma *cma)
{
	unsigned long pfn, end_pfn, early_pfn[CMA_MAX_RANGES];
	int allocrange, r;
	struct cma_memrange *cmr;
	unsigned long bitmap_count, count;

	/* 先完成所有位图分配，避免部分 range 可分配而另一些没有跟踪状态。 */
	for (allocrange = 0; allocrange < cma->nranges; allocrange++) {
		cmr = &cma->ranges[allocrange];
		early_pfn[allocrange] = cmr->early_pfn;
		cmr->bitmap = bitmap_zalloc(cma_bitmap_maxno(cma, cmr),
		/* 位图 bit 的数量取决于 range 页数和 order_per_bit，不是字节大小。 */
					    GFP_KERNEL);
		if (!cmr->bitmap)
			goto cleanup;
	}

	/* 跨 zone range 不能交给 alloc_contig_range，必须在启用前整体失败。 */
	if (!cma_validate_zones(cma))
		goto cleanup;

	/* early_pfn 前缀已被早期使用，位图先占住；余下 pageblock 改为 MIGRATE_CMA。 */
	for (r = 0; r < cma->nranges; r++) {
		cmr = &cma->ranges[r];
		if (early_pfn[r] != cmr->base_pfn) {
			/* 早期 reserve 从 range 起点推进，前缀仍不可用于普通 CMA 分配。 */
			count = early_pfn[r] - cmr->base_pfn;
			bitmap_count = cma_bitmap_pages_to_bits(cma, count);
			bitmap_set(cmr->bitmap, 0, bitmap_count);
		}

		for (pfn = early_pfn[r]; pfn < cmr->base_pfn + cmr->count;
			/* 每个 pageblock 设置 MIGRATE_CMA，compaction 才能把可迁移页推出目标范围。 */
		     pfn += pageblock_nr_pages)
			init_cma_reserved_pageblock(pfn_to_page(pfn));
	}

	/* lock 保护 bitmap/available_count，alloc_mutex 串行化昂贵的迁移冻结操作。 */
	spin_lock_init(&cma->lock);

	mutex_init(&cma->alloc_mutex);

#ifdef CONFIG_CMA_DEBUGFS
	INIT_HLIST_HEAD(&cma->mem_head);
	spin_lock_init(&cma->mem_head_lock);
#endif
	set_bit(CMA_ACTIVATED, &cma->flags);

	return;

cleanup:
	/* 此时 area 未发布，依次撤销已分配位图并恢复可用物理页的原有归属。 */
	for (r = 0; r < allocrange; r++)
		/* 仅已成功分配的 bitmap 可以释放，未到达的 range 指针仍为初始值。 */
		bitmap_free(cma->ranges[r].bitmap);

	/* Expose all pages to the buddy, they are useless for CMA. */
	/* 补充说明：明确要求保留错误页的调用者自行负责，不能被本通用清理路径释放。 */
	if (!test_bit(CMA_RESERVE_PAGES_ON_ERROR, &cma->flags)) {
		for (r = 0; r < cma->nranges; r++) {
			/* 每段从实际 early 边界开始回收，避免释放调用者自己仍持有的前缀页。 */
			unsigned long start_pfn;

			/* 当前 range 的几何参数仍有效；只把可交回的后缀页逐一恢复。 */
			cmr = &cma->ranges[r];
			/* 早期前缀之外的 reserved 页在失败后不再具有 CMA 用途。 */
			start_pfn = r <= allocrange ? early_pfn[r] : cmr->early_pfn;
			end_pfn = cmr->base_pfn + cmr->count;
			for (pfn = start_pfn; pfn < end_pfn; pfn++)
				free_reserved_page(pfn_to_page(pfn));
		}
	}
	totalcma_pages -= cma->count;
	cma->available_count = cma->count = 0;
	pr_err("CMA area %s could not be activated\n", cma->name);
}

/*
 * cma_init_reserved_areas() - 在 core_initcall 阶段激活全部已登记 area。
 * 业务背景：集中等待通用内存子系统就绪，再把早期 memblock 保留区接入运行期 CMA。
 * 入参：无；读取启动期冻结的全局数组 [0,cma_area_count)。
 * 返回/副作用：始终返回 0；逐项激活，单项失败由 cma_activate_area() 清零并记录日志，不阻断其余项。
 * 注意事项：返回 0 不代表每个 area 都激活成功，客户需以 area count/flags 判断可用性。
 */
static int __init cma_init_reserved_areas(void)
{
	int i;

	for (i = 0; i < cma_area_count; i++)
		/* 各 area 激活失败互不阻断，失败 area 自身 count 会变为零。 */
		cma_activate_area(&cma_areas[i]);

	return 0;
}
core_initcall(cma_init_reserved_areas);

/*
 * cma_reserve_pages_on_error() - 要求 area 激活失败时仍保留其物理页。
 * 业务背景：HugeTLB 等早期用户可能已从 area 前缀取得页，不能让通用失败清理释放同一保留区。
 * 入参：cma 为尚未激活的借用 area；调用者仍拥有已取走页的后续初始化/释放责任。
 * 返回/副作用：无返回，只置策略位；必须在 activation 前调用，不能恢复已经交给 buddy 的页。
 * 注意事项：该策略会在激活失败时继续占用物理内存，调用者必须有独立生命周期方案。
 */
void __init cma_reserve_pages_on_error(struct cma *cma)
{
	set_bit(CMA_RESERVE_PAGES_ON_ERROR, &cma->flags);
}

/*
 * cma_new_area() - 从固定全局表领取一个未激活 CMA 描述符。
 * 业务背景：早期启动不能依赖动态 area 容器，先登记总容量，稍后 activation 再分配 bitmap。
 * 入参：name 可为 NULL；size 是字节且应页对齐；order_per_bit 定义粒度；res_cma 为非 NULL 输出槽。
 * 返回/副作用：成功返回 0、发布借用指针并增加 area_count/totalcma_pages；槽位耗尽返回 -ENOSPC。
 * 注意事项：仅单线程 __init 使用；领取后若后续失败，调用者必须以 cma_drop_area() 后进先出回滚。
 */
static int __init cma_new_area(const char *name, phys_addr_t size,
			       unsigned int order_per_bit,
			       struct cma **res_cma)
{
	struct cma *cma;

	if (cma_area_count == ARRAY_SIZE(cma_areas)) {
		/* 数组是静态上限，不能动态扩容以免早期内存布局不可预测。 */
		pr_err("Not enough slots for CMA reserved regions!\n");
		return -ENOSPC;
	}

	/*
	 * Each reserved area must be initialised later, when more kernel
	 * subsystems (like slab allocator) are available.
	 */
	/* 补充说明：此时 slab/bitmap 尚不可依赖，area 只登记几何参数，稍后统一 activate。 */
	cma = &cma_areas[cma_area_count];
	cma_area_count++;

	if (name)
		/* 显式名称供 sysfs/debug/trace 归因；缺省名按当前计数生成。 */
		strscpy(cma->name, name);
	else
		snprintf(cma->name, CMA_MAX_NAME,  "cma%d\n", cma_area_count);

	cma->available_count = cma->count = size >> PAGE_SHIFT;
	cma->order_per_bit = order_per_bit;
	*res_cma = cma;
	totalcma_pages += cma->count;

	return 0;
}

/*
 * cma_drop_area() - 回滚最近一次 cma_new_area()。
 * 业务背景：物理 reserve 或多 range 组装失败时撤销尚未对外发布的静态槽位。
 * 入参：cma 必须恰为 cma_areas[cma_area_count-1] 且未激活、未发布；本函数不自行校验。
 * 返回/副作用：无返回；减少 totalcma_pages 和有效槽位数，不释放 bitmap 或物理内存。
 * 注意事项：只能后进先出回滚；对非尾槽调用会让全局表和容量统计失配。
 */
static void __init cma_drop_area(struct cma *cma)
{
	totalcma_pages -= cma->count;
	cma_area_count--;
}

/**
 * cma_init_reserved_mem() - create custom contiguous area from reserved memory
 * @base: Base address of the reserved area
 * @size: Size of the reserved area (in bytes),
 * @order_per_bit: Order of pages represented by one bit on bitmap.
 * @name: The name of the area. If this parameter is NULL, the name of
 *        the area will be set to "cmaN", where N is a running counter of
 *        used areas.
 * @res_cma: Pointer to store the created cma region.
 *
 * This function creates custom contiguous area from already reserved memory.
 */
/*
 * cma_init_reserved_mem() - 为已经保留的物理内存建立单 range CMA 描述符。
 * 业务背景：调用者先完成 memblock reserve，本函数只登记几何与容量，bitmap 留到统一激活。
 * 入参：base/size 为字节且都须按 CMA 最小粒度对齐；order_per_bit 是 bit 页阶；name 可空；
 * res_cma 是非 NULL 输出槽。完整 [base,base+size) 必须已被 memblock 标为 reserved。
 * 返回/副作用：成功 0 并发布全局表内借用指针、增加 CMA 总量；非法参数 -EINVAL，满槽等原样返回。
 * 注意事项：不取得物理内存 ownership、不分配 bitmap；失败不替调用者释放原有 memblock reserve。
 */
int __init cma_init_reserved_mem(phys_addr_t base, phys_addr_t size,
				 unsigned int order_per_bit,
				 const char *name,
				 struct cma **res_cma)
{
	struct cma *cma;
	int ret;

	/* Sanity checks */
	/* 必须验证 reserve 状态，否则 CMA 激活可能把普通可分配内存误改成 MIGRATE_CMA。 */
	if (!size || !memblock_is_region_reserved(base, size))
		/* size 为零或非 reserve 区会让 CMA 侵占普通 memblock 分配，必须拒绝。 */
		return -EINVAL;

	/*
	 * CMA uses CMA_MIN_ALIGNMENT_BYTES as alignment requirement which
	 * needs pageblock_order to be initialized. Let's enforce it.
	 */
	if (!pageblock_order) {
		/* pageblock 阶尚未知时无法验证 CMA_MIN_ALIGNMENT_BYTES。 */
		pr_err("pageblock_order not yet initialized. Called during early boot?\n");
		return -EINVAL;
	}

	/* ensure minimal alignment required by mm core */
	if (!IS_ALIGNED(base | size, CMA_MIN_ALIGNMENT_BYTES))
		/* 起址和长度必须整块对齐，否则 reserve 尾部会跨迁移策略边界。 */
		return -EINVAL;

	/* 只有所有几何检查通过才占用全局槽位，之后失败可不留幽灵 area。 */
	ret = cma_new_area(name, size, order_per_bit, &cma);
	if (ret != 0)
		return ret;

	cma->ranges[0].base_pfn = PFN_DOWN(base);
	/* 单 range 描述的 early_pfn 初始等于 base，表示尚无 cma_reserve_early 前缀。 */
	cma->ranges[0].early_pfn = PFN_DOWN(base);
	cma->ranges[0].count = cma->count;
	cma->nranges = 1;
	cma->nid = NUMA_NO_NODE;

	*res_cma = cma;

	return 0;
}

/*
 * Structure used while walking physical memory ranges and finding out
 * which one(s) to use for a CMA area.
 */
struct cma_init_memrange {
	/* 候选半开物理区间的起点，已按最终 CMA align 向上规整。 */
	phys_addr_t base;
	/* 可用字节数，已按 align 与 bitmap 粒度向下规整。 */
	phys_addr_t size;
	/* 工作链节点：先进入按大小排序的 ranges，再转移到按地址排序的 final_ranges。 */
	struct list_head list;
};

/*
 * Work array used during CMA initialization.
 */
static struct cma_init_memrange memranges[CMA_MAX_RANGES] __initdata;

/* memranges 仅存放最多 CMA_MAX_RANGES 个入选候选；__initdata 在启动后释放，不得外借指针。 */

/*
 * revsizecmp() - 判断左候选是否应排在右候选之前（大小降序）。
 * 业务背景：多 range 慢路径只保留最大候选，降序链表让尾部成为可淘汰的最小项。
 * 入参：两个 __init 工作项的借用指针；只读取 size。
 * 返回/副作用：左侧更大返回 true，相等返回 false；供稳定性不作保证的插入排序使用。
 * 注意事项：只在 memranges 的 __init 生命周期内调用，不定义相等元素的稳定顺序。
 */
static bool __init revsizecmp(struct cma_init_memrange *mlp,
			      struct cma_init_memrange *mrp)
{
	return mlp->size > mrp->size;
}

/*
 * basecmp() - 判断左候选是否应排在右候选之前（地址升序）。
 * 业务背景：容量筛选后按低地址优先 reserve，模拟 memblock bottom-up 分配策略。
 * 入参：两个 __init 工作项的借用指针；只读取 base。
 * 返回/副作用：左侧地址更低返回 true，相等返回 false；用于形成 bottom-up reserve 顺序。
 * 注意事项：只比较起址，不检查区间重叠；候选来源必须已经保证其为空闲 memblock range。
 */
static bool __init basecmp(struct cma_init_memrange *mlp,
			   struct cma_init_memrange *mrp)
{
	return mlp->base < mrp->base;
}

/*
 * Helper function to create sorted lists.
 */
/*
 * list_insert_sorted() - 按调用者比较规则把工作项插入循环链表。
 * 业务背景：同一 memrange 工作项先按容量筛选，再按物理地址决定 reserve 顺序。
 * 入参：ranges 是已按 cmp 排序的哨兵头；mrp 必须当前不在链上；cmp 为严格“左在右前”。
 * 返回/副作用：无返回，把 mrp->list 链入且保持排序；不复制对象、不取得其生命周期 ownership。
 * 注意事项：非空链表路径依赖循环结束时 mlp 仍指向尾元素；只能用于本文件的小型 __init 工作链。
 */
static void __init list_insert_sorted(
	struct list_head *ranges,
	struct cma_init_memrange *mrp,
	bool (*cmp)(struct cma_init_memrange *lh, struct cma_init_memrange *rh))
{
	struct list_head *mp;
	struct cma_init_memrange *mlp;

	if (list_empty(ranges))
		/* list_head 是循环哨兵；空表时 list_add 建立第一个候选。 */
		list_add(&mrp->list, ranges);
	else {
		list_for_each(mp, ranges) {
			mlp = list_entry(mp, struct cma_init_memrange, list);
			if (cmp(mlp, mrp))
				break;
		}
		__list_add(&mrp->list, mlp->list.prev, &mlp->list);
	}
}

/*
 * cma_fixed_reserve() - 在调用者指定的精确物理地址建立 memblock 保留。
 * 业务背景：设备固定窗口不能由分配器移动，但仍须满足单 zone 的连续迁移前提。
 * 入参：base/size 为已由上层对齐和限界检查的字节区间；本函数仅在 __init 阶段调用。
 * 返回/副作用：成功 0 并由 memblock 接管该区；跨 low/highmem 边界 -EINVAL，重叠或 reserve 失败 -EBUSY。
 * 注意事项：失败不改变保留状态；成功后的回滚责任转给上层声明器。
 */
static int __init cma_fixed_reserve(phys_addr_t base, phys_addr_t size)
{
	if (IS_ENABLED(CONFIG_HIGHMEM)) {
		/* 固定区不能跨 low/highmem，后续连续迁移只能在一个 zone 内执行。 */
		phys_addr_t highmem_start = __pa(high_memory - 1) + 1;

		/*
		 * If allocating at a fixed base the request region must not
		 * cross the low/high memory boundary.
		 */
		if (base < highmem_start && base + size > highmem_start) {
			pr_err("Region at %pa defined on low/high memory boundary (%pa)\n",
			       &base, &highmem_start);
			return -EINVAL;
		}
	}

	if (memblock_is_region_reserved(base, size) ||
		/* 已重叠或 reserve 失败都表示这个精确地址不可作为 CMA 私有区。 */
	    memblock_reserve(base, size) < 0) {
		return -EBUSY;
	}

	return 0;
}

/*
 * cma_alloc_mem() - 为非固定 CMA 从 memblock 搜索并保留一个连续物理范围。
 * 业务背景：优先避开 DMA/DMA32 低地址，并让未来 compaction 把页移出而非移入 CMA 区。
 * 入参：base/limit 定义字节搜索窗口，size/align 是已规范化要求，nid 可为 NUMA_NO_NODE。
 * 返回/副作用：成功返回非零物理基址且范围已 reserved；失败返回 0。会临时切换 memblock 搜索方向后恢复。
 * 注意事项：HIGHMEM 跨界时先试高端再收紧到低端；成功 ownership 由上层继续登记或回滚。
 */
static phys_addr_t __init cma_alloc_mem(phys_addr_t base, phys_addr_t size,
			phys_addr_t align, phys_addr_t limit, int nid)
{
	phys_addr_t addr = 0;

	/*
	 * If there is enough memory, try a bottom-up allocation first.
	 * It will place the new cma area close to the start of the node
	 * and guarantee that the compaction is moving pages out of the
	 * cma area and not into it.
	 * Avoid using first 4GB to not interfere with constrained zones
	 * like DMA/DMA32.
	 */
#ifdef CONFIG_PHYS_ADDR_T_64BIT
	/* 临时改 memblock 搜索方向后必须恢复全局设置，不能泄漏给其它启动分配者。 */
	if (!memblock_bottom_up() && limit >= SZ_4G + size) {
		/* 仅原本非 bottom-up 时临时反转，保持外部启动分配者策略不变。 */
		memblock_set_bottom_up(true);
		addr = memblock_alloc_range_nid(size, align, SZ_4G, limit,
						nid, true);
		memblock_set_bottom_up(false);
	}
#endif

	/*
	 * On systems with HIGHMEM try allocating from there before consuming
	 * memory in lower zones.
	 */
	/* bottom-up 未成功时，HIGHMEM 平台优先消耗 high memory 以保留低端 DMA 区。 */
	if (!addr && IS_ENABLED(CONFIG_HIGHMEM)) {
		phys_addr_t highmem = __pa(high_memory - 1) + 1;

		/*
		 * All pages in the reserved area must come from the same zone.
		 * If the requested region crosses the low/high memory boundary,
		 * try allocating from high memory first and fall back to low
		 * memory in case of failure.
		 */
		if (base < highmem && limit > highmem) {
			/* 跨边界候选先收紧到 highmem；失败后低端搜索从 highmem 以下继续。 */
			addr = memblock_alloc_range_nid(size, align, highmem,
							limit, nid, true);
			limit = highmem;
		}
	}

	/* 前两种偏好都失败才使用调用者给出的通用 [base,limit) 范围。 */
	if (!addr)
		addr = memblock_alloc_range_nid(size, align, base, limit, nid,
						true);

	return addr;
}

/*
 * __cma_declare_contiguous_nid() - 声明单 range CMA 的完整启动期事务。
 * 业务背景：统一参数规范化、固定或动态 memblock reserve、描述符登记以及失败回滚。
 * 入参：basep 既给出候选下界/固定地址又接收实际地址；size/limit/alignment 为字节；
 * order_per_bit 为位图页阶；fixed 决定是否必须精确命中；name 可空；res_cma 输出借用指针；nid 限节点。
 * 返回/副作用：成功 0，更新 *basep 和 *res_cma 并保留物理区；失败返回 errno 且不遗留本次 reserve/area。
 * 注意事项：仅 __init；0 base 会强制动态模式，所有 range 必须同时满足 pageblock 与 bitmap 粒度。
 */
static int __init __cma_declare_contiguous_nid(phys_addr_t *basep,
			phys_addr_t size, phys_addr_t limit,
			phys_addr_t alignment, unsigned int order_per_bit,
			bool fixed, const char *name, struct cma **res_cma,
			int nid)
{
	phys_addr_t memblock_end = memblock_end_of_DRAM();
	phys_addr_t base = *basep;
	int ret;

	/* base 是调用者可更新输出；其余局部变量仅服务于启动期参数规范化和日志。 */
	pr_debug("%s(size %pa, base %pa, limit %pa alignment %pa)\n",
		__func__, &size, &base, &limit, &alignment);

	if (cma_area_count == ARRAY_SIZE(cma_areas)) {
		/* 在任何 memblock reserve 前检查槽位，避免失败路径留下物理区泄漏。 */
		pr_err("Not enough slots for CMA reserved regions!\n");
		return -ENOSPC;
	}

	if (!size)
		/* 空区域既没有可激活页也不能形成有效 cma 句柄。 */
		return -EINVAL;

	if (alignment && !is_power_of_2(alignment))
		/* 位图搜索以位掩码实现，只接受二次幂对齐。 */
		return -EINVAL;

	if (!IS_ENABLED(CONFIG_NUMA))
		/* 非 NUMA 内核不能解释 nid，统一退化到任意节点搜索。 */
		nid = NUMA_NO_NODE;

	/* Sanitise input arguments. */
	/* 对齐同时决定 bitmap 粒度和 pageblock 可迁移边界，所有边界值先向安全方向规整。 */
	alignment = max_t(phys_addr_t, alignment, CMA_MIN_ALIGNMENT_BYTES);
	/* 对齐下限保证任何最终 range 都可被完整 pageblock 覆盖。 */
	if (fixed && base & (alignment - 1)) {
		pr_err("Region at %pa must be aligned to %pa bytes\n",
			&base, &alignment);
		return -EINVAL;
	}
	base = ALIGN(base, alignment);
	/* 动态搜索允许基址上调；固定请求已在此前拒绝未对齐地址。 */
	size = ALIGN(size, alignment);
	limit &= ~(alignment - 1);

	if (!base)
		fixed = false;

	/* size should be aligned with order_per_bit */
	if (!IS_ALIGNED(size >> PAGE_SHIFT, 1 << order_per_bit))
		/* 最后一个 bitmap bit 也必须代表完整数量的基础页。 */
		return -EINVAL;


	/*
	 * If the limit is unspecified or above the memblock end, its effective
	 * value will be the memblock end. Set it explicitly to simplify further
	 * checks.
	 */
	if (limit == 0 || limit > memblock_end)
		/* 归一化上限使后续 base + size 比较不需要再处理“无限制”。 */
		limit = memblock_end;

	if (base + size > limit) {
		/* 对齐后的请求若超过上限，不能截断 size，否则会违背客户的连续容量约定。 */
		pr_err("Size (%pa) of region at %pa exceeds limit (%pa)\n",
			&size, &base, &limit);
		return -EINVAL;
	}

	/* Reserve memory */
	/* fixed 要求精确 reserve；动态路径选择新地址，两者之后共享描述符创建与回滚协议。 */
	if (fixed) {
		/* 固定路径绝不移动基址，调用者通常依赖设备 DMA 地址。 */
		ret = cma_fixed_reserve(base, size);
		if (ret)
			return ret;
	} else {
		base = cma_alloc_mem(base, size, alignment, limit, nid);
		/* 动态 reserve 已由 memblock_alloc_range_nid 完成；0 是约定的失败哨兵。 */
		if (!base)
			return -ENOMEM;

		/*
		 * kmemleak scans/reads tracked objects for pointers to other
		 * objects but this address isn't mapped and accessible
		 */
		kmemleak_ignore_phys(base);
	}

	ret = cma_init_reserved_mem(base, size, order_per_bit, name, res_cma);
	/* 此调用只建 area 描述；运行期位图/锁要等 core_initcall activation。 */
	/* 描述符初始化失败必须归还刚才的物理 reserve，防止启动期永久泄漏。 */
	if (ret) {
		memblock_phys_free(base, size);
		return ret;
	}

	/* 成功时 res_cma 已指向完整单 range 描述，area 留待后续 core init 激活。 */
	(*res_cma)->nid = nid;
	*basep = base;

	return 0;
}

/*
 * Create CMA areas with a total size of @total_size. A normal allocation
 * for one area is tried first. If that fails, the biggest memblock
 * ranges above 4G are selected, and allocated bottom up.
 *
 * The complexity here is not great, but this function will only be
 * called during boot, and the lists operated on have fewer than
 * CMA_MAX_RANGES elements (default value: 8).
 */
/*
 * cma_declare_contiguous_multi() - 以多个物理 range 组成一个启动期 CMA area。
 * 业务背景：单个物理洞容不下总容量时，为 HugeTLB 等客户跨洞凑足同一逻辑 CMA 池。
 * 入参：total_size/align 为字节，order_per_bit 为位图页阶，name 可空，res_cma 为输出，nid 限节点。
 * 返回/副作用：优先尝试单 range；多段成功返回 0、保留总容量并发布 area，失败返回 errno 并回滚。
 * 注意事项：仅 __init 单线程运行，最多 CMA_MAX_RANGES，慢路径只考虑 4GB 以上空闲区。
 */
int __init cma_declare_contiguous_multi(phys_addr_t total_size,
			phys_addr_t align, unsigned int order_per_bit,
			const char *name, struct cma **res_cma, int nid)
{
	/* 多 range 慢路径先收集大于 4GB 的候选，再按地址重排并逐段 reserve，所有失败集中回滚。 */
	phys_addr_t start = 0, end;
	phys_addr_t size, sizesum, sizeleft;
	struct cma_init_memrange *mrp, *mlp, *failed;
	struct cma_memrange *cmrp;
	LIST_HEAD(ranges);
	LIST_HEAD(final_ranges);
	struct list_head *mp, *next;
	int ret, nr = 1;
	u64 i;
	struct cma *cma;

	/* ranges 保存大小排序候选，final_ranges 保存地址排序的实际 reserve 顺序。 */
	/*
	 * First, try it the normal way, producing just one range.
	 */
	ret = __cma_declare_contiguous_nid(&start, total_size, 0, align,
	/* 优先单段，除 ENOMEM 外的参数/策略错误不应被多段机制掩盖。 */
			order_per_bit, false, name, res_cma, nid);
	/* 成功或参数错误均已有最终结论，只有碎片导致的 ENOMEM 才转 multi-range。 */
	if (ret != -ENOMEM)
		goto out;
	/* 单 range 返回 ENOMEM 才表明碎片可由多个独立 reserve 段弥补。 */

	/*
	 * Couldn't find one range that fits our needs, so try multiple
	 * ranges.
	 *
	 * No need to do the alignment checks here, the call to
	 * cma_declare_contiguous_nid above would have caught
	 * any issues. With the checks, we know that:
	 *
	 * - @align is a power of 2
	 * - @align is >= pageblock alignment
	 * - @size is aligned to @align and to @order_per_bit
	 *
	 * So, as long as we create ranges that have a base
	 * aligned to @align, and a size that is aligned to
	 * both @align and @order_to_bit, things will work out.
	 */
	nr = 0;
	/* 单段失败后重新从零计候选，sizesum 仅表示当前 top-N 候选容量。 */
	sizesum = 0;
	failed = NULL;

	ret = cma_new_area(name, total_size, order_per_bit, &cma);
	/* 预先领取 area 后，后续任何失败均须 cma_drop_area 回收全局容量统计。 */
	if (ret != 0)
		goto out;

	align = max_t(phys_addr_t, align, CMA_MIN_ALIGNMENT_BYTES);
	/* 多段也采用与单段相同的最小 pageblock 对齐，不可降低要求换取容量。 */
	/*
	 * Create a list of ranges above 4G, largest range first.
	 */
	for_each_free_mem_range(i, nid, MEMBLOCK_NONE, &start, &end, NULL) {
		/* 候选必须在 4GB 以上并两端对齐，避免 DMA 受限区和 bitmap 粒度冲突。 */
		if (upper_32_bits(start) == 0)
			continue;

		start = ALIGN(start, align);
		/* 向上对齐后的空区可能完全消失，必须重检 start/end。 */
		if (start >= end)
			continue;

		end = ALIGN_DOWN(end, align);
		if (end <= start)
			continue;

		size = end - start;
		size = ALIGN_DOWN(size, (PAGE_SIZE << order_per_bit));
		/* 每段大小按 bitmap bit 粒度裁剪，避免尾部不可表示。 */
		if (!size)
			continue;
		sizesum += size;
		/*
		 * 当前实现先累加再判断 top-N 淘汰：达到上限后若新段比尾段更小而 continue，
		 * 该新段仍留在 sizesum 中。因而下面的容量预检可能高估入选集合；后续代码也未
		 * 单独检查 sizeleft，阅读或修改此算法时不能把 sizesum 当成严格的链表容量和。
		 */

		pr_debug("consider %016llx - %016llx\n", (u64)start, (u64)end);

		/*
		 * If we don't yet have used the maximum number of
		 * areas, grab a new one.
		 *
		 * If we can't use anymore, see if this range is not
		 * smaller than the smallest one already recorded. If
		 * not, re-use the smallest element.
		 */
		if (nr < CMA_MAX_RANGES)
			/* 未达上限时取空工作槽；上限后仅以更大的候选替换当前最小项。 */
			mrp = &memranges[nr++];
		else {
			/* 链表按大小降序，尾元素正是可被更大候选替换的最小段。 */
			mrp = list_last_entry(&ranges,
					      struct cma_init_memrange, list);
			if (size < mrp->size)
				continue;
			list_del(&mrp->list);
			sizesum -= mrp->size;
			pr_debug("deleted %016llx - %016llx from the list\n",
				(u64)mrp->base, (u64)mrp->base + size);
		}
		mrp->base = start;
		/* 工作槽离链后再写新几何参数，随后排序插回。 */
		mrp->size = size;

		/*
		 * Now do a sorted insert.
		 */
		list_insert_sorted(&ranges, mrp, revsizecmp);
		/* 大小降序链表让尾部始终是最小候选，便于淘汰。 */
		pr_debug("added %016llx - %016llx to the list\n",
		    (u64)mrp->base, (u64)mrp->base + size);
		pr_debug("total size now %llu\n", (u64)sizesum);
	}

	/*
	 * There is not enough room in the CMA_MAX_RANGES largest
	 * ranges, so bail out.
	 */
	if (sizesum < total_size) {
		/* top-N 所有候选仍不足，不应 reserve 零散小段并留下不可用 CMA。 */
		/* 候选总量不足时没有 reserve 物理页，回滚的只有 area 描述。 */
		cma_drop_area(cma);
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Found ranges that provide enough combined space.
	 * Now, sorted them by address, smallest first, because we
	 * want to mimic a bottom-up memblock allocation.
	 */
	sizesum = 0;
	list_for_each_safe(mp, next, &ranges) {
		/* 从大集合取足 total_size 后按地址升序放入 final_ranges。 */
		mlp = list_entry(mp, struct cma_init_memrange, list);
		/* 从 size 链摘除后按 address 链插入，建立确定的 reserve 顺序。 */
		list_del(mp);
		list_insert_sorted(&final_ranges, mlp, basecmp);
		sizesum += mlp->size;
		if (sizesum >= total_size)
			break;
	}

	/*
	 * Walk the final list, and add a CMA range for
	 * each range, possibly not using the last one fully.
	 */
	nr = 0;
	sizeleft = total_size;
	list_for_each(mp, &final_ranges) {
		/* 最后一个 range 可以只保留所需前缀，sizeleft 到零即完成。 */
		mlp = list_entry(mp, struct cma_init_memrange, list);
		size = min(sizeleft, mlp->size);
		/* 只消费满足剩余总量的前缀，最后段未用尾部继续留作普通 memblock。 */
		if (memblock_reserve(mlp->base, size)) {
			/* 任何段 reserve 意外失败都终止，避免产生难以向客户描述的不完整 area。 */
			/*
			 * Unexpected error. Could go on to
			 * the next one, but just abort to
			 * be safe.
			 */
			failed = mlp;
			break;
		}

		pr_debug("created region %d: %016llx - %016llx\n",
		    nr, (u64)mlp->base, (u64)mlp->base + size);
		cmrp = &cma->ranges[nr++];
		cmrp->base_pfn = PHYS_PFN(mlp->base);
		/* CMA range 保存 PFN/页数，早期指针从基址开始，稍后 activation 建位图。 */
		cmrp->early_pfn = cmrp->base_pfn;
		cmrp->count = size >> PAGE_SHIFT;

		sizeleft -= size;
		if (sizeleft == 0)
			/* 已凑足请求容量；剩余候选无需保留或回滚。 */
			break;
	}

	if (failed) {
		/* 回滚已 reserve 的前缀物理段，然后撤销 area 槽位和 totalcma 计数。 */
		/* 仅释放失败项之前已经 reserve 的 range，之后项目尚未保留。 */
		list_for_each(mp, &final_ranges) {
			mlp = list_entry(mp, struct cma_init_memrange, list);
			if (mlp == failed)
				break;
			memblock_phys_free(mlp->base, mlp->size);
		}
		cma_drop_area(cma);
		ret = -ENOMEM;
		goto out;
	}

	/* 所有 physical ranges reserve 成功后，才向调用者发布 nranges/nid/area 指针。 */
	cma->nranges = nr;
	cma->nid = nid;
	*res_cma = cma;

out:
	/* 成功时发布 cma，失败时保留 errno 供架构/设备树调用者诊断。 */
	if (ret != 0)
		/* 统一日志将 total_size 从字节换算 MiB，方便启动失败定位。 */
		pr_err("Failed to reserve %lu MiB\n",
			(unsigned long)total_size / SZ_1M);
	else
		pr_info("Reserved %lu MiB in %d range%s\n",
			(unsigned long)total_size / SZ_1M, nr, str_plural(nr));
	return ret;
}

/**
 * cma_declare_contiguous_nid() - reserve custom contiguous area
 * @base: Base address of the reserved area optional, use 0 for any
 * @size: Size of the reserved area (in bytes),
 * @limit: End address of the reserved memory (optional, 0 for any).
 * @alignment: Alignment for the CMA area, should be power of 2 or zero
 * @order_per_bit: Order of pages represented by one bit on bitmap.
 * @fixed: hint about where to place the reserved area
 * @name: The name of the area. See function cma_init_reserved_mem()
 * @res_cma: Pointer to store the created cma region.
 * @nid: nid of the free area to find, %NUMA_NO_NODE for any node
 *
 * This function reserves memory from early allocator. It should be
 * called by arch specific code once the early allocator (memblock or bootmem)
 * has been activated and all other subsystems have already allocated/reserved
 * memory. This function allows to create custom reserved areas.
 *
 * If @fixed is true, reserve contiguous area at exactly @base.  If false,
 * reserve in range from @base to @limit.
 */
/*
 * cma_declare_contiguous_nid() - 从 early allocator 声明单 range CMA area。
 * 业务背景：架构/设备初始化在其它早期保留完成后，为运行期连续分配器预留物理区。
 * 入参：base/limit/size/alignment 为字节；base=0 或 limit=0 表示动态/DRAM 末端；fixed 要求精确
 * base；order_per_bit 定义粒度，name 可空，res_cma 输出借用指针，nid 可为 NUMA_NO_NODE。
 * 返回/副作用：成功 0、写 res_cma 并保留物理内存；失败返回 errno，不遗留本次 reserve。
 * 注意事项：仅登记而未激活 bitmap/锁；alignment 只能为 0 或二次幂，调用者不得提前分配。
 */
int __init cma_declare_contiguous_nid(phys_addr_t base,
			phys_addr_t size, phys_addr_t limit,
			phys_addr_t alignment, unsigned int order_per_bit,
			bool fixed, const char *name, struct cma **res_cma,
			int nid)
{
	/* 参数含义与 kernel-doc 一一对应：base/limit 定位，size/alignment/order 约束几何。 */
	/* 外部入口保留完整参数 ABI，所有实质状态转换由内部指针版本完成。 */
	/* 公共包装按值传 base；内部成功后回写局部地址仅用于日志。 */
	int ret;

	/* 固定与动态语义已由内部实现处理，本层只统一向启动日志报告最终物理起址。 */
	ret = __cma_declare_contiguous_nid(&base, size, limit, alignment,
	/* 成功后内部已把实际动态 base 写回这个局部变量。 */
			order_per_bit, fixed, name, res_cma, nid);
	if (ret != 0)
		pr_err("Failed to reserve %ld MiB\n",
				(unsigned long)size / SZ_1M);
	else
		pr_info("Reserved %ld MiB at %pa\n",
				(unsigned long)size / SZ_1M, &base);

	/* 日志之后返回内部 errno；调用者通过 ret 决定设备初始化是否继续。 */
	return ret;
}

/*
 * cma_debug_show_areas() - 打印一个 area 的位图空洞和总可用页快照。
 * 业务背景：最终分配失败且未请求 NOWARN 时，给出哪一段碎片化导致无法满足连续请求。
 * 入参：cma 为已激活 area 的借用指针；所有 range bitmap 必须已初始化。
 * 返回/副作用：无返回，只输出日志；spin_lock_irq 保证 bitmap/available_count 相互一致。
 * 注意事项：日志仍可能较长，且锁内打印会延长临界区，因此只用于限速后的失败诊断。
 */
static void cma_debug_show_areas(struct cma *cma)
{
	/* 此 helper 只在失败诊断调用，持自旋锁期间仅向日志输出，不进行迁移或分配。 */
	/* 锁内读取位图和 available_count，使打印的空闲范围与总数属于同一快照。 */
	unsigned long start, end;
	unsigned long nr_part;
	unsigned long nbits;
	int r;
	struct cma_memrange *cmr;

	spin_lock_irq(&cma->lock);
	pr_info("number of available pages: ");
	for (r = 0; r < cma->nranges; r++) {
		/* clear bit range 正是可供下次 CMA 请求预留的连续 bitmap 区间。 */
		cmr = &cma->ranges[r];

		nbits = cma_bitmap_maxno(cma, cmr);

		pr_info("range %d: ", r);
		/* 连续 clear bit 经过 order_per_bit 展开为可观察的基础页段。 */
		/* 输出只读 bitmap；真正的分配和释放仍通过各自锁协议修改状态。 */
		for_each_clear_bitrange(start, end, cmr->bitmap, nbits) {
			/* clear 区间端点是 bit，下式恢复为基础页数量再打印。 */
			nr_part = (end - start) << cma->order_per_bit;
			pr_cont("%s%lu@%lu", start ? "+" : "", nr_part, start);
		}
		pr_info("\n");
	}
	pr_cont("=> %lu free of %lu total pages\n", cma->available_count,
			cma->count);
	spin_unlock_irq(&cma->lock);
}

/*
 * cma_range_alloc() - 在单个 range 中认领 bitmap 候选并冻结连续物理页。
 * 业务背景：先用轻量锁排他认领，再在锁外迁移页面；EBUSY 时换下一个对齐候选重试。
 * 入参：cma/cmr 为已激活且匹配的借用对象；count 是基础页数，align 是页阶；pagep 为成功输出；
 * gfp 控制底层迁移/分配策略。调用者必须允许睡眠，且 count>0。
 * 返回/副作用：成功 0 并写 *pagep，bitmap 保持占用且 available_count 减少；失败返回 errno 且不写输出。
 * 并发/ownership：spinlock 保护记账，alloc_mutex 串行迁移；成功页仍 frozen，由上层决定提交或 frozen 释放。
 * 注意事项：可睡眠且 count 必须非零；只有 -EBUSY 会换候选重试，其它错误直接终止本 range。
 */
static int cma_range_alloc(struct cma *cma, struct cma_memrange *cmr,
				unsigned long count, unsigned int align,
				struct page **pagep, gfp_t gfp)
{
	/* pagep 仅为成功输出位置；cma/cmr 在整个搜索和冻结过程均由上层稳定持有。 */
	/* 先在 bitmap 预留候选，再出锁执行可能迁移/冻结的 contig 分配；失败则重新清位图。 */
	unsigned long bitmap_maxno, bitmap_no, bitmap_count;
	unsigned long start, pfn, mask, offset;
	int ret = -EBUSY;
	struct page *page = NULL;

	/* bitmap_maxno 是 range 位图容量，bitmap_count 是本请求需要锁定的 bit 数。 */
	/* ret 初始 EBUSY 表示候选仍可能通过换位图位置解决。 */
	mask = cma_bitmap_aligned_mask(cma, align);
	/* mask/offset 将客户 PAGE_SIZE 阶对齐映射为当前 range 的 bitmap 搜索约束。 */
	offset = cma_bitmap_aligned_offset(cma, cmr, align);
	bitmap_maxno = cma_bitmap_maxno(cma, cmr);
	bitmap_count = cma_bitmap_pages_to_bits(cma, count);

	if (bitmap_count > bitmap_maxno)
		/* 请求超过单 range 位图容量，切换到外层其它 range 或最终失败。 */
		goto out;

	for (start = 0; ; start = bitmap_no + mask + 1) {
		/* 每次 EBUSY 后从上一个对齐候选之后继续，避免重复迁移同一 busy 区。 */
		spin_lock_irq(&cma->lock);
		/*
		 * If the request is larger than the available number
		 * of pages, stop right away.
		 */
		if (count > cma->available_count) {
			/* 可用总数不足无需扫描 bitmap，快速终止并保留 -EBUSY。 */
			spin_unlock_irq(&cma->lock);
			break;
		}
		bitmap_no = bitmap_find_next_zero_area_off(cmr->bitmap,
		/* 搜索同时满足连续空位、对齐 mask 与基址偏移。 */
				bitmap_maxno, start, bitmap_count, mask,
				offset);
		if (bitmap_no >= bitmap_maxno) {
			/* 从 start 起没有满足对齐的空位，本 range 的本轮尝试结束。 */
			spin_unlock_irq(&cma->lock);
			break;
		}

		pfn = cmr->base_pfn + (bitmap_no << cma->order_per_bit);
		/* 候选首页由 bitmap 位置唯一确定，随后验证其 struct page 序列也连续。 */
		/* bitmap bit 转回 range 相对基础 PFN，page 指针只在物理连续性确认后交付。 */
		page = pfn_to_page(pfn);

		/*
		 * Do not hand out page ranges that are not contiguous, so
		 * callers can just iterate the pages without having to worry
		 * about these corner cases.
		 */
		if (!page_range_contiguous(page, count)) {
			/* memmap 不连续不能交给客户，哪怕 bit 位连续也必须跳过。 */
			spin_unlock_irq(&cma->lock);
			pr_warn_ratelimited("%s: %s: skipping incompatible area [0x%lx-0x%lx]",
					    __func__, cma->name, pfn, pfn + count - 1);
			continue;
		}

		bitmap_set(cmr->bitmap, bitmap_no, bitmap_count);
		/* bitmap 位是对其它 CMA 分配者的排他声明，锁外 migration 不会被重复选择。 */
		cma->available_count -= count;
		/*
		 * It's safe to drop the lock here. We've marked this region for
		 * our exclusive use. If the migration fails we will take the
		 * lock again and unmark it.
		 */
		spin_unlock_irq(&cma->lock);

		mutex_lock(&cma->alloc_mutex);
		ret = alloc_contig_frozen_range(pfn, pfn + count, ACR_FLAGS_CMA, gfp);
		/* alloc_mutex 串行化隔离/迁移，避免两个大范围操作相互制造不可迁移页。 */
		mutex_unlock(&cma->alloc_mutex);
		if (!ret)
			/* alloc_contig 成功时 bitmap 保持占用，所有权从临时预留转给调用者。 */
			break;

		/* ret 失败后 page 尚不能对调用者可见，先恢复计费/位图再决定是否重试。 */
		cma_clear_bitmap(cma, cmr, pfn, count);
		/* migration 失败必须立即撤销排他 bit，避免永久虚假占用降低 available_count。 */
		if (ret != -EBUSY)
			break;

		pr_debug("%s(): memory range at pfn 0x%lx %p is busy, retrying\n",
			 __func__, pfn, page);

		trace_cma_alloc_busy_retry(cma->name, pfn, page, count, align);
	}
out:
	/* *pagep 只在真正成功后写入，调用者可用 ret/page 双重判断。 */
	if (!ret)
		*pagep = page;
	return ret;
}

/*
 * __cma_alloc_frozen() - 跨 area 的所有 ranges 分配一段仍处于 frozen 状态的连续页。
 * 业务背景：底层客户可能需要在建立普通 refcount 前构造 compound/hugetlb 元数据。
 * 入参：cma 可空；count 为基础页数且须大于零；align 为页阶；gfp 传递给连续迁移并控制告警。
 * 返回/副作用：成功返回首页并保持 bitmap 占用，失败 NULL；两者都在有效请求后记录 trace/VM/sysfs 统计。
 * ownership/上下文：成功把 frozen range 责任交给调用者；必须以 set_pages_refcounted 或 cma_release_frozen 配对；可睡眠。
 * 注意事项：NULL/零容量 area 或 count=0 在 trace/统计前直接失败；align 必须是有效页阶。
 */
static struct page *__cma_alloc_frozen(struct cma *cma,
		unsigned long count, unsigned int align, gfp_t gfp)
{
	/* 跨各 range 尝试分配；返回页仍处于 frozen 状态，公共 cma_alloc 再设引用计数。 */
	struct page *page = NULL;
	int ret = -ENOMEM, r;
	unsigned long i;
	const char *name = cma ? cma->name : NULL;

	/* name 在 NULL cma 时保持 NULL，仅供 trace 安全输出。 */
	if (!cma || !cma->count)
		/* NULL 或零容量 area 不是可恢复的 range busy，直接返回 NULL。 */
		return page;

	pr_debug("%s(cma %p, name: %s, count %lu, align %d)\n", __func__,
		(void *)cma, cma->name, count, align);

	if (!count)
		/* 零页没有有效首页，也不应建立 trace/统计事件。 */
		return page;

	trace_cma_alloc_start(name, count, cma->available_count, cma->count, align);
	/* trace 取的是锁外近似 available_count，用于诊断而非配额决策。 */

	for (r = 0; r < cma->nranges; r++) {
		/* page 清空确保失败 range 不会遗留前一轮成功的裸指针。 */
		/* 只有 EBUSY 才值得切换 range；其它错误代表请求/资源不可恢复。 */
		page = NULL;

		ret = cma_range_alloc(cma, &cma->ranges[r], count, align,
				       &page, gfp);
		if (ret != -EBUSY || page)
			break;
	}

	/*
	 * CMA can allocate multiple page blocks, which results in different
	 * blocks being marked with different tags. Reset the tags to ignore
	 * those page blocks.
	 */
	if (page) {
		/* KASAN tag 复位发生在对客户发布页面之前，保证整个连续区同一可访问标记。 */
		/* 迁移跨 pageblock 可带不同 KASAN tag，连续分配输出前统一清除。 */
		for (i = 0; i < count; i++)
			page_kasan_tag_reset(page + i);
	}

	if (ret && !(gfp & __GFP_NOWARN)) {
		/* NOWARN 客户仍获得统计/trace，只抑制面向日志的失败转储。 */
		pr_err_ratelimited("%s: %s: alloc failed, req-size: %lu pages, ret: %d\n",
				   __func__, cma->name, count, ret);
		cma_debug_show_areas(cma);
	}

	pr_debug("%s(): returned %p\n", __func__, page);
	trace_cma_alloc_finish(name, page ? page_to_pfn(page) : 0,
			       page, count, align, ret);
	if (page) {
		/* 成功/失败均更新 VM event 和 CMA sysfs 账户，供全局回收诊断。 */
		count_vm_event(CMA_ALLOC_SUCCESS);
		cma_sysfs_account_success_pages(cma, count);
	} else {
		count_vm_event(CMA_ALLOC_FAIL);
		cma_sysfs_account_fail_pages(cma, count);
	}

	/* 此处 page 非 NULL 即表示连续隔离/迁移和 bookkeeping 都已完成。 */
	return page;
}

/*
 * cma_alloc_frozen() - 以普通内核 GFP 策略申请 frozen CMA 页。
 * 业务背景：给需要先构造页元数据再提交普通引用的内核客户提供公开 frozen 分配入口。
 * 入参：cma/count/align 同内部实现；no_warn 仅映射为 __GFP_NOWARN，不改变成功或重试语义。
 * 返回/副作用：成功返回 frozen 首页，失败 NULL；统计由内部完成。调用者必须在可睡眠上下文配对处理 ownership。
 * 注意事项：成功页不能直接按普通 refcount 页释放；须显式提交引用或调用 cma_release_frozen()。
 */
struct page *cma_alloc_frozen(struct cma *cma, unsigned long count,
		unsigned int align, bool no_warn)
{
	/* no_warn 仅控制日志，不改变 GFP_KERNEL 可睡眠的迁移/分配契约。 */
	gfp_t gfp = GFP_KERNEL | (no_warn ? __GFP_NOWARN : 0);

	return __cma_alloc_frozen(cma, count, align, gfp);
}

/*
 * cma_alloc_frozen_compound() - 申请按自身阶对齐的 frozen compound 候选。
 * 业务背景：HugeTLB CMA 在正式构造 folio 前需要 2^order 个连续、带 __GFP_COMP 意图的页。
 * 入参：cma 为已激活借用 area；order 同时决定页数与首页对齐，移位必须在有效阶范围内。
 * 返回/副作用：成功返回仍 frozen 的首页，失败 NULL且抑制告警；调用者必须 frozen release 或提交引用。
 * 注意事项：__GFP_COMP 会让底层校验 order 不超过 MAX_FOLIO_ORDER；本包装不自行截断。
 */
struct page *cma_alloc_frozen_compound(struct cma *cma, unsigned int order)
{
	/* compound 包装把 order 同时作为页数和对齐阶，保证首页满足 compound 约束。 */
	gfp_t gfp = GFP_KERNEL | __GFP_COMP | __GFP_NOWARN;

	return __cma_alloc_frozen(cma, 1 << order, order, gfp);
}

/**
 * cma_alloc() - allocate pages from contiguous area
 * @cma:   Contiguous memory region for which the allocation is performed.
 * @count: Requested number of pages.
 * @align: Requested alignment of pages (in PAGE_SIZE order).
 * @no_warn: Avoid printing message about failed allocation
 *
 * This function allocates part of contiguous memory on specific
 * contiguous memory area.
 */
/*
 * cma_alloc() - 从指定 CMA area 分配并提交普通引用计数的连续页。
 * 业务背景：面向常规客户封装 frozen 分配，把迁移所得页面转换成可正常 get/put 的所有权。
 * 入参：cma 是已激活 area 的借用指针；count 为非零基础页数；align 为页阶；no_warn 只抑制失败日志。
 * 返回/副作用：成功返回连续首页并建立每页 refcount，失败 NULL且不留 bitmap 占用；会更新统计/trace。
 * 注意事项：可睡眠；成功结果必须以同一 cma/count 调用 cma_release()，不得改用 frozen 释放接口。
 */
struct page *cma_alloc(struct cma *cma, unsigned long count,
		       unsigned int align, bool no_warn)
{
	/* 成功后将 frozen 页转换为客户可引用页；失败保持 NULL。 */
	struct page *page;

	page = cma_alloc_frozen(cma, count, align, no_warn);
	/* set_pages_refcounted 是 frozen 到普通客户所有权的提交点。 */
	if (page)
		set_pages_refcounted(page, count);

	return page;
}
EXPORT_SYMBOL_GPL(cma_alloc);

/*
 * find_cma_memrange() - 定位完整容纳待释放页段的唯一 CMA range。
 * 业务背景：释放前必须证明起点和排他终点同属一个 range，避免跨洞清除无关 bitmap。
 * 入参：cma/pages 可空；count 为基础页数，正常契约要求大于零且不超过 area 总量。
 * 返回/副作用：命中返回内部 cmr 借用指针，否则 NULL；只读几何并可能诊断跨边界，不修改页或位图。
 * 注意事项：当前代码未显式拒绝 count=0，外部调用者不能利用零长度“命中”触发释放路径。
 */
static struct cma_memrange *find_cma_memrange(struct cma *cma,
		const struct page *pages, unsigned long count)
{
	/* 验证 pages/count 整体落在同一 range，防止 release 跨边界清错 bitmap。 */
	struct cma_memrange *cmr = NULL;
	unsigned long pfn, end_pfn;
	int r;

	pr_debug("%s(page %p, count %lu)\n", __func__, (void *)pages, count);

	if (!cma || !pages || count > cma->count)
		/* 先拒绝明显无效参数，避免 page_to_pfn 对 NULL 或越界范围执行。 */
		return NULL;

	pfn = page_to_pfn(pages);
	/* end_pfn 为排他上界，count 必须完整落入而不能仅让起点命中。 */

	for (r = 0; r < cma->nranges; r++) {
		/* 必须同时容纳起始和结束 PFN，跨两个合法 range 仍不可一次 release。 */
		cmr = &cma->ranges[r];
		end_pfn = cmr->base_pfn + cmr->count;
		if (pfn >= cmr->base_pfn && pfn < end_pfn) {
			if (pfn + count <= end_pfn)
				break;

			VM_WARN_ON_ONCE(1);
		}
	}

	/* 命中范围返回借用 cmr，调用者立即用它完成同一 area 的冻结释放。 */
	if (r == cma->nranges) {
		/* range 未匹配时任何 release 会破坏别的 CMA 位图，故仅返回 NULL。 */
		pr_debug("%s(page %p, count %lu, no cma range matches the page range)\n",
			 __func__, (void *)pages, count);
		return NULL;
	}

	return cmr;
}

/*
 * __cma_release_frozen() - 归还已验证归属的 frozen CMA 页段。
 * 业务背景：两个公开释放入口在完成各自引用语义后，共用同一底层页状态与 bitmap 提交顺序。
 * 入参：cma/cmr/pages/count 必须来自成功分配并严格配对，且调用者已不再并发访问这些页。
 * 返回/副作用：无返回；解除 frozen 连续区、清 bitmap、恢复 available_count，并记录统计/trace。
 * 顺序约束：先让底层页重新可管理，再向 CMA 搜索者发布 bitmap 空闲；重复或错配释放会破坏记账。
 * 注意事项：不验证归属与 count；只能由 find_cma_memrange() 成功后的两个公开释放入口调用。
 */
static void __cma_release_frozen(struct cma *cma, struct cma_memrange *cmr,
		const struct page *pages, unsigned long count)
{
	/* 释放顺序先解除连续冻结，再清 bitmap，随后发布统计/trace，客户不得并发继续使用页。 */
	unsigned long pfn = page_to_pfn(pages);

	pr_debug("%s(page %p, count %lu)\n", __func__, (void *)pages, count);

	free_contig_frozen_range(pfn, count);
	/* 先解除 isolation/frozen 状态，随后 bitmap 才可向并发 CMA 请求重新开放。 */
	cma_clear_bitmap(cma, cmr, pfn, count);
	cma_sysfs_account_release_pages(cma, count);
	trace_cma_release(cma->name, pfn, pages, count);
	/* trace 在 bitmap 已重新可用后记录，观察者可把它视为释放完成事件。 */
}

/**
 * cma_release() - release allocated pages
 * @cma:   Contiguous memory region for which the allocation is performed.
 * @pages: Allocated pages.
 * @count: Number of allocated pages.
 *
 * This function releases memory allocated by cma_alloc().
 * It returns false when provided pages do not belong to contiguous area and
 * true otherwise.
 */
/*
 * cma_release() - 归还由 cma_alloc() 提交普通 refcount 的连续页。
 * 业务背景：先撤销客户页引用，再解除 frozen 连续区并向并发 CMA 分配重新发布 bitmap 空位。
 * 入参：cma/pages/count 必须与一次成功 cma_alloc() 完全配对，且客户已停止全部访问。
 * 返回/副作用：不属于该 area 返回 false且不改状态；匹配时归还整段并返回 true，更新统计/trace。
 * 注意事项：若仍有额外引用会 WARN 但当前实现仍强制回收，残余使用者可能访问已重新分配的页面。
 */
bool cma_release(struct cma *cma, const struct page *pages,
		 unsigned long count)
{
	/* 普通 release 先撤销客户引用；仍被使用的页告警但仍执行 CMA 回收以暴露调用者 bug。 */
	struct cma_memrange *cmr;
	unsigned long ret = 0;
	unsigned long i, pfn;

	cmr = find_cma_memrange(cma, pages, count);
	/* 非本 area 的页绝不 put 或清 bitmap，调用者据 false 纠正配对错误。 */
	if (!cmr)
		return false;

	pfn = page_to_pfn(pages);
	/* 每个 page 的客户引用都应在 release 前归零；ret 非零揭示使用者未配对 put。 */
	for (i = 0; i < count; i++, pfn++)
		ret += !put_page_testzero(pfn_to_page(pfn));

	/* WARN 保留引用泄漏证据；CMA 回收仍继续以避免位图资源永久损失。 */
	WARN(ret, "%lu pages are still in use!\n", ret);

	__cma_release_frozen(cma, cmr, pages, count);

	return true;
}
EXPORT_SYMBOL_GPL(cma_release);

/*
 * cma_release_frozen() - 归还尚未建立普通 refcount 的 CMA 连续页。
 * 业务背景：与 cma_alloc_frozen() 及 compound 版本配对，绕过普通 cma_release() 的逐页 put。
 * 入参：cma/pages/count 必须精确匹配一次 frozen 分配且 count>0；调用者已停止访问。
 * 返回/副作用：不属于同一 range 返回 false；成功解除隔离、清记账并返回 true，ownership 回到 CMA/buddy。
 * 注意事项：本入口不执行 put_page_testzero，不能用于已经 set_pages_refcounted 的普通 cma_alloc() 结果。
 */
bool cma_release_frozen(struct cma *cma, const struct page *pages,
		unsigned long count)
{
	/* frozen 版本供尚未 set_pages_refcounted 的内部客户，省略逐页 put。 */
	struct cma_memrange *cmr;

	cmr = find_cma_memrange(cma, pages, count);
	if (!cmr)
		return false;

	/* frozen 页没有客户 refcount，直接执行底层解除隔离与位图清理。 */
	__cma_release_frozen(cma, cmr, pages, count);

	return true;
}

/*
 * cma_for_each_area() - 依登记顺序访问所有全局 CMA area。
 * 业务背景：内存热插拔等子系统需要对每个 area 做只读冲突检查，而不复制全局表。
 * 入参：it 为非 NULL 回调，data 原样透传；回调得到临时借用指针且可返回状态码。
 * 返回/副作用：全部返回 0 则为 0；首个非零值立即透传并停止。函数自身不加 area 锁。
 * 注意事项：仅适用于 area_count 已冻结的运行期；回调负责其所读可变字段的同步且不得改表结构。
 */
int cma_for_each_area(int (*it)(struct cma *cma, void *data), void *data)
{
	/* 启动后 area 数量固定，按数组顺序回调；首个非零返回中止遍历。 */
	int i;

	for (i = 0; i < cma_area_count; i++) {
		/* 回调只借用 area；不可在此更改 cma_area_count 或销毁其 bitmap。 */
		int ret = it(&cma_areas[i], data);

		if (ret)
			return ret;
	}

	return 0;
}

/*
 * cma_intersects() - 判断物理字节范围是否触及 area 的任一 range。
 * 业务背景：s390 memory-offline notifier 用它阻止下线包含 CMA 保留页的 memory block。
 * 入参：cma 为稳定借用 area；start/end 是物理字节边界，调用者应保证 start < end。
 * 返回/副作用：任一 range 命中返回 true，否则 false；纯只读，不取得锁或 ownership。
 * 边界语义：range 采用 [rstart,rend)，但当前判断仅在 end < rstart 时跳过，因此 end==rstart
 * 也保守报告相交；而 start==rend 报告不相交。调用者不能把本接口当作对称的标准半开区间判定。
 * 注意事项：不验证 start/end 顺序或溢出，调用者必须传入规范化的物理地址边界。
 */
bool cma_intersects(struct cma *cma, unsigned long start, unsigned long end)
{
	/* 以物理字节边界判交集；任一 range 命中即返回。 */
	int r;
	struct cma_memrange *cmr;
	unsigned long rstart, rend;

	for (r = 0; r < cma->nranges; r++) {
		/* 将 PFN 几何转换为物理字节端点后再比较客户的地址区间。 */
		cmr = &cma->ranges[r];
		/* rstart/rend 描述 CMA 半开区间；客户 end 的左边界比较按当前代码保守包含相邻端点。 */

		rstart = PFN_PHYS(cmr->base_pfn);
		rend = PFN_PHYS(cmr->base_pfn + cmr->count);
		if (end < rstart)
			continue;
		if (start >= rend)
			continue;
		return true;
	}

	return false;
}

/*
 * Very basic function to reserve memory from a CMA area that has not
 * yet been activated. This is expected to be called early, when the
 * system is single-threaded, so there is no locking. The alignment
 * checking is restrictive - only pageblock-aligned areas
 * (CMA_MIN_ALIGNMENT_BYTES) may be reserved through this function.
 * This keeps things simple, and is enough for the current use case.
 *
 * The CMA bitmaps have not yet been allocated, so just start
 * reserving from the bottom up, using a PFN to keep track
 * of what has been reserved. Unreserving is not possible.
 *
 * The caller is responsible for initializing the page structures
 * in the area properly, since this just points to memblock-allocated
 * memory. The caller should subsequently use init_cma_pageblock to
 * set the migrate type and CMA stats  the pageblocks that were reserved.
 *
 * If the CMA area fails to activate later, memory obtained through
 * this interface is not handed to the page allocator, this is
 * the responsibility of the caller (e.g. like normal memblock-allocated
 * memory).
 */
/*
 * cma_reserve_early() - 在 area 激活前从各 range 底部不可逆地取得页块。
 * 业务背景：HugeTLB bootmem 在 CMA bitmap 尚不存在时预取 gigantic pages，随后自行初始化页结构。
 * 入参：cma 为未激活 area 的借用指针；size 为字节，须同时按 pageblock 和 bitmap 粒度对齐。
 * 返回/副作用：成功返回线性映射地址、推进对应 early_pfn 并减少 available_count；失败 NULL且不改状态。
 * 并发/ownership：只允许单线程 __init，无锁且不支持 unreserve；成功内存交给调用者，后者负责
 * 页结构、migrate type/CMA 统计，并在 area 后续激活失败时继续承担这些页的生命周期。
 * 注意事项：激活后调用、未对齐、容量不足或单个 range 放不下都会返回 NULL且不跨 range 拼接。
 */
void __init *cma_reserve_early(struct cma *cma, unsigned long size)
{
	/* ret 初始 NULL；只有某个 range 尚余足量前缀时才转换为线性虚拟地址。 */
	int r;
	struct cma_memrange *cmr;
	unsigned long available;
	void *ret = NULL;

	if (!cma || !cma->count)
		return NULL;
	/* 本接口恰恰要求尚未激活：此时 bitmap/运行期锁均未就绪，只能推进 early_pfn。 */
	/*
	 * Can only be called early in init.
	 */
	if (test_bit(CMA_ACTIVATED, &cma->flags))
		/* 激活后所有 reserve 必须走 bitmap/锁协议，不能再修改 early_pfn。 */
		return NULL;

	if (!IS_ALIGNED(size, CMA_MIN_ALIGNMENT_BYTES))
		/* pageblock 对齐确保后续 init_cma_reserved_pageblock 覆盖完整范围。 */
		return NULL;

	if (!IS_ALIGNED(size, (PAGE_SIZE << cma->order_per_bit)))
		return NULL;

	size >>= PAGE_SHIFT;
	/* 从这里起 size/available 全是基础页单位，可与 cma->available_count 比较。 */

	if (size > cma->available_count)
		return NULL;

	for (r = 0; r < cma->nranges; r++) {
		/* 按 range 顺序从低地址 early_pfn 分配，已取走部分不会回退。 */
		cmr = &cma->ranges[r];
		available = cmr->count - (cmr->early_pfn - cmr->base_pfn);
		if (size <= available) {
			/* 物理转虚拟仅适用于该早期可线性映射内存，调用者负责后续页结构初始化。 */
			ret = phys_to_virt(PFN_PHYS(cmr->early_pfn));
			cmr->early_pfn += size;
			cma->available_count -= size;
			return ret;
		}
	}

	return ret;
}
