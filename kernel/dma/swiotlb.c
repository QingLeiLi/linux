// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dynamic DMA mapping support.
 *
 * This implementation is a fallback for platforms that do not support
 * I/O TLBs (aka DMA address translation hardware).
 * Copyright (C) 2000 Asit Mallick <Asit.K.Mallick@intel.com>
 * Copyright (C) 2000 Goutham Rao <goutham.rao@intel.com>
 * Copyright (C) 2000, 2003 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 *
 * 03/05/07 davidm	Switch from PCI-DMA to generic device DMA API.
 * 00/12/13 davidm	Rename to swiotlb.c and add mark_clean() to avoid
 *			unnecessary i-cache flushing.
 * 04/07/.. ak		Better overflow handling. Assorted fixes.
 * 05/09/10 linville	Add support for syncing ranges, support syncing for
 *			DMA_BIDIRECTIONAL mappings, miscellaneous cleanup.
 * 08/12/11 beckyb	Add highmem support
 */

/*
 * 文件职责与主路径：
 *
 * SWIOTLB 在设备不能直接寻址原始物理内存、平台强制隔离 DMA，或内存加密要求设备只访问
 * 解密页时，提供一段设备可达的“中转”内存。映射时从池中领取连续 slot，并按 DMA 方向把
 * CPU 原缓冲区复制到 bounce buffer；同步或解除映射时再把设备写回的数据复制回来。
 *
 * 生命周期分为：
 *   启动参数/架构策略确定容量与地址上限
 *     -> 启动期 memblock 或晚期页分配器建立默认池
 *     -> 映射路径按 area 加锁搜索并登记 orig_addr
 *     -> bounce/sync 在原缓冲区与池之间搬运数据
 *     -> unmap 合并空闲 slot；动态临时池还需从 RCU 链表摘除后延迟释放
 *     -> 不再需要默认池时 swiotlb_exit() 恢复加密属性并按来源释放。
 *
 * io_tlb_mem 是一个分配器，io_tlb_pool 是它拥有的一段物理池，io_tlb_area 是降低竞争的
 * 独立加锁分区，io_tlb_slot 则记录最小 2 KiB 分配单元。锁只保护分配元数据；正在使用的
 * bounce buffer 内容由 DMA API 的 map/sync/unmap 时序和方向约束保护。
 *
 * 原文件头还记录了实现沿革：最初它作为没有硬件 I/O TLB（DMA 地址转换硬件）平台的动态 DMA
 * mapping fallback；随后从 PCI-DMA 迁移到通用 DMA API，加入避免多余指令缓存刷新的
 * mark_clean()、更稳健的溢出处理、范围同步和 DMA_BIDIRECTIONAL 支持，最终补上 highmem。
 * 这些历史解释了本文件为何同时保留通用 DMA、边界防护、双向同步和逐页复制路径。
 */

#define pr_fmt(fmt) "software IO TLB: " fmt

#include <linux/cache.h>
#include <linux/cc_platform.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/kmsan-checks.h>
#include <linux/iommu-helper.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pfn.h>
#include <linux/rculist.h>
#include <linux/scatterlist.h>
#include <linux/set_memory.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/swiotlb.h>
#include <linux/types.h>
#ifdef CONFIG_DMA_RESTRICTED_POOL
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/slab.h>
#endif

#define CREATE_TRACE_POINTS
#include <trace/events/swiotlb.h>

#define SLABS_PER_PAGE (1 << (PAGE_SHIFT - IO_TLB_SHIFT))

/*
 * Minimum IO TLB size to bother booting with.  Systems with mainly
 * 64bit capable cards will only lightly use the swiotlb.  If we can't
 * allocate a contiguous 1MB, we're probably in trouble anyway.
 */
/*
 * 上述最小池为 1 MiB。低于该值时即使继续缩容也难以提供有用的连续映射能力，因此启动期
 * 重试和动态扩池都以它为下限；宏值的单位已由字节换算为 slot 数。
 */
#define IO_TLB_MIN_SLABS ((1<<20) >> IO_TLB_SHIFT)

/**
 * struct io_tlb_slot - IO TLB slot descriptor
 * @orig_addr:	The original address corresponding to a mapped entry.
 * @alloc_size:	Size of the allocated buffer.
 * @list:	The free list describing the number of free entries available
 *		from each index.
 * @pad_slots:	Number of preceding padding slots. Valid only in the first
 *		allocated non-padding slot.
 */
/*
 * io_tlb_slot 描述池中一个最小分配单元：orig_addr 是当前对应的原始物理地址，
 * INVALID_PHYS_ADDR 表示尚未映射；alloc_size 保存从当前 slot 对应位置到本次分配末尾的剩余
 * 字节数；list 在空闲时保存不跨 segment 的连续空闲 slot 数、占用时为 0；pad_slots 只在首个
 * 非填充 slot 中记录其前方一并领取的对齐 slot 数。分配与释放在所属 area->lock 下成组改写
 * 这些元数据。描述符不持有原始页引用，调用者必须让原缓冲区存活到 DMA 映射解除。
 */
struct io_tlb_slot {
	phys_addr_t orig_addr;
	size_t alloc_size;
	unsigned short list;
	unsigned short pad_slots;
};

/*
 * 以上两个启动策略量分别表示强制所有适用映射经过中转、完全禁止建立默认池。它们由早期
 * 命令行解析在单 CPU 启动阶段写入，初始化完成后只读，运行期无需加锁。
 */
static bool swiotlb_force_bounce;
static bool swiotlb_force_disable;

#ifdef CONFIG_SWIOTLB_DYNAMIC

static void swiotlb_dyn_alloc(struct work_struct *work);

/*
 * 动态配置下，默认分配器在编译期完成锁、池链表和扩池 work 初始化；后续 add_mem_pool()
 * 在 mem->lock 下发布池。非动态配置只保留零初始化对象，初始化路径直接填充 defpool。
 */
static struct io_tlb_mem io_tlb_default_mem = {
	.lock = __SPIN_LOCK_UNLOCKED(io_tlb_default_mem.lock),
	.pools = LIST_HEAD_INIT(io_tlb_default_mem.pools),
	.dyn_alloc = __WORK_INITIALIZER(io_tlb_default_mem.dyn_alloc,
					swiotlb_dyn_alloc),
};

#else  /* !CONFIG_SWIOTLB_DYNAMIC */
/* 未启用动态 SWIOTLB 时，默认分配器依赖静态零初始化，不维护池链表或扩池 work。 */

static struct io_tlb_mem io_tlb_default_mem;

#endif	/* CONFIG_SWIOTLB_DYNAMIC */

/* 以上结束默认分配器在动态与静态配置下的两种初始化形态。 */
/*
 * default_nslabs 是期望的默认池 slot 总数，default_nareas 是并发搜索分区数；二者都是建池前
 * 的策略值。实际池可能因连续内存不足而缩小，且每个 area 至少要容纳一个完整 segment。
 */
static unsigned long default_nslabs = IO_TLB_DEFAULT_SIZE >> IO_TLB_SHIFT;
static unsigned long default_nareas;

/**
 * struct io_tlb_area - IO TLB memory area descriptor
 *
 * This is a single area with a single lock.
 *
 * @used:	The number of used IO TLB block.
 * @index:	The slot index to start searching in this area for next round.
 * @lock:	The lock to protect the above data structures in the map and
 *		unmap calls.
 */
/*
 * io_tlb_area 把 pool 均分成独立分配域以降低 CPU 间锁竞争。used 是本区已占 slot 数，index
 * 是下次循环搜索的区内起点；lock 同时保护这两个字段以及本区 slots[] 的空闲链与映射元数据。
 * 单次分配不会跨 area，所以 area_nslabs 必须满足完整 segment 的布局约束。
 */
struct io_tlb_area {
	unsigned long used;
	unsigned int index;
	spinlock_t lock;
};

/*
 * Round up number of slabs to the next power of 2. The last area is going
 * be smaller than the rest if default_nslabs is not power of two.
 * The number of slot in an area should be a multiple of IO_TLB_SEGSIZE,
 * otherwise a segment may span two or more areas. It conflicts with free
 * contiguous slots tracking: free slots are treated contiguous no matter
 * whether they cross an area boundary.
 *
 * Return true if default_nslabs is rounded up.
 */
/*
 * round_up_default_nslabs() - 使默认池容量满足按 2 的幂均分的约束
 *
 * 调用者是启动参数和架构容量调整路径；此时尚未分配池、无并发访问，可以直接修改全局策略。
 * 入参：无。函数先保证每个 area 至少有 IO_TLB_SEGSIZE 个 slot；容量已是 2 的幂时保持不变，
 * 否则向上取整。返回 true 表示 default_nslabs 被改写，调用者据此修正最终容量日志。
 * 不分配资源、不转移所有权，也不睡眠。
 */
static bool round_up_default_nslabs(void)
{
	if (!default_nareas)
		return false;

	if (default_nslabs < IO_TLB_SEGSIZE * default_nareas)
		default_nslabs = IO_TLB_SEGSIZE * default_nareas;
	else if (is_power_of_2(default_nslabs))
		return false;
	default_nslabs = roundup_pow_of_two(default_nslabs);
	return true;
}

/**
 * swiotlb_adjust_nareas() - adjust the number of areas and slots
 * @nareas:	Desired number of areas. Zero is treated as 1.
 *
 * Adjust the default number of areas in a memory pool.
 * The default size of the memory pool may also change to meet minimum area
 * size requirements.
 */
/*
 * swiotlb_adjust_nareas() - 规范化默认 area 数并同步容量下限
 *
 * @nareas: 启动参数或 possible CPU 数给出的期望分区数；0 表示 1，非 2 的幂向上取整。
 *
 * 本函数只在池建立前运行，无需锁且不睡眠。它先更新 default_nareas，再调用
 * round_up_default_nslabs() 保证每区至少有完整 segment。返回：无直接返回值；副作用是两个
 * 默认策略全局量和启动日志可能变化，随后初始化路径据此分配 areas[] 与 slots[]。
 */
static void swiotlb_adjust_nareas(unsigned int nareas)
{
	if (!nareas)
		nareas = 1;
	else if (!is_power_of_2(nareas))
		nareas = roundup_pow_of_two(nareas);

	default_nareas = nareas;

	pr_info("area num %d.\n", nareas);
	if (round_up_default_nslabs())
		pr_info("SWIOTLB bounce buffer size roundup to %luMB",
			(default_nslabs << IO_TLB_SHIFT) >> 20);
}

/**
 * limit_nareas() - get the maximum number of areas for a given memory pool size
 * @nareas:	Desired number of areas.
 * @nslots:	Total number of slots in the memory pool.
 *
 * Limit the number of areas to the maximum possible number of areas in
 * a memory pool of the given size.
 *
 * Return: Maximum possible number of areas.
 */
/*
 * limit_nareas() - 在实际 slot 数缩小时限制可用 area 数
 *
 * @nareas: 期望分区数，调用者已保证非零且通常为 2 的幂。
 * @nslots: 当前候选池的总 slot 数，单位为 IO_TLB_SIZE。
 *
 * 每个 area 必须至少容纳 IO_TLB_SEGSIZE 个 slot；不足时返回能容纳的最大分区数，否则原样
 * 返回。该纯计算 helper 不改状态、不持有对象且不睡眠；结果供缩容循环和建池使用。
 */
static unsigned int limit_nareas(unsigned int nareas, unsigned long nslots)
{
	if (nslots < nareas * IO_TLB_SEGSIZE)
		return nslots / IO_TLB_SEGSIZE;
	return nareas;
}

/*
 * setup_io_tlb_npages() - 解析 swiotlb=<slots>[,<areas>][,force|noforce]
 *
 * @str: early_param 传入的可写解析游标；借用自启动命令行，只移动局部指针，不接管内存。
 *
 * 它在内存池建立前的早期启动上下文运行，不持锁、不睡眠。首个数字按完整 segment 对齐，
 * 第二个数字交给 swiotlb_adjust_nareas() 规范化，尾部关键字分别设置强制 bounce 或禁用策略。
 * 返回始终为 0，表示参数处理完成；畸形可选字段被忽略，最终由初始化路径读取全局策略。
 */
static int __init
setup_io_tlb_npages(char *str)
{
	if (isdigit(*str)) {
		/* avoid tail segment of size < IO_TLB_SEGSIZE */
		/*
		 * 尾部不足一个 segment 会破坏连续空闲计数不跨 segment 的不变量，因此整体向上对齐，
		 * 不留下分配器无法正确描述的残段。
		 */
		default_nslabs =
			ALIGN(simple_strtoul(str, &str, 0), IO_TLB_SEGSIZE);
	}
	if (*str == ',')
		++str;
	if (isdigit(*str))
		swiotlb_adjust_nareas(simple_strtoul(str, &str, 0));
	if (*str == ',')
		++str;
	if (!strcmp(str, "force"))
		swiotlb_force_bounce = true;
	else if (!strcmp(str, "noforce"))
		swiotlb_force_disable = true;

	return 0;
}
early_param("swiotlb", setup_io_tlb_npages);

/*
 * swiotlb_size_or_default() - 返回启动策略当前要求的默认池容量
 *
 * 入参：无；调用者包括架构初始化代码，用于在实际建池前预估预留量。返回值单位为字节，由
 * default_nslabs 左移 slot 大小得到；它可能反映命令行或架构调整，但不保证等于内存不足缩容后
 * 的实际池大小。函数只读启动期全局量，不持锁、不睡眠，也不改变所有权或外部状态。
 */
unsigned long swiotlb_size_or_default(void)
{
	return default_nslabs << IO_TLB_SHIFT;
}

/*
 * swiotlb_adjust_size() - 在用户未显式指定容量时允许架构调整默认池
 *
 * @size: 架构建议的字节数；函数按 IO_TLB_SIZE 和完整 segment 约束向上对齐。
 *
 * 该 __init helper 在建池前调用，不持锁、可使用启动期日志且不取得资源。如果启动参数已改变
 * default_nslabs，则立即返回以尊重用户配置；否则更新全局策略并记录最终 MiB。返回：无直接
 * 返回值；副作用仅限 default_nslabs 和日志，后续 swiotlb_init_remap() 消费该值。
 */
void __init swiotlb_adjust_size(unsigned long size)
{
	/*
	 * If swiotlb parameter has not been specified, give a chance to
	 * architectures such as those supporting memory encryption to
	 * adjust/expand SWIOTLB size for their use.
	 */
	/*
	 * 只有仍保持编译期 64 MiB 默认值时，才允许内存加密等架构扩大或调整 SWIOTLB；用户通过
	 * swiotlb= 指定的容量具有更高优先级，不能被架构建议静默覆盖。
	 */
	if (default_nslabs != IO_TLB_DEFAULT_SIZE >> IO_TLB_SHIFT)
		return;

	size = ALIGN(size, IO_TLB_SIZE);
	default_nslabs = ALIGN(size >> IO_TLB_SHIFT, IO_TLB_SEGSIZE);
	if (round_up_default_nslabs())
		size = default_nslabs << IO_TLB_SHIFT;
	pr_info("SWIOTLB bounce buffer size adjusted to %luMB", size >> 20);
}

/*
 * swiotlb_print_info() - 输出默认池的实际物理范围和容量
 *
 * 入参：无。函数借用 io_tlb_default_mem.defpool，只读 nslabs/start/end；初始化路径在池完全建立
 * 后调用，因此无需锁且不改变对象。若池为空则发出警告，否则打印闭区间和 MiB。返回：无直接
 * 返回值；除日志外无副作用，不分配资源也不睡眠。
 */
void swiotlb_print_info(void)
{
	struct io_tlb_pool *mem = &io_tlb_default_mem.defpool;

	if (!mem->nslabs) {
		pr_warn("No low mem\n");
		return;
	}

	pr_info("mapped [mem %pa-%pa] (%luMB)\n", &mem->start, &mem->end,
	       (mem->nslabs << IO_TLB_SHIFT) >> 20);
}

/*
 * io_tlb_offset() - 计算 slot 索引在一个 segment 内的偏移
 *
 * @val: 全局 slot 索引或等价计数；单位为 slot。IO_TLB_SEGSIZE 为 2 的幂，因此位与得到范围
 * [0, IO_TLB_SEGSIZE-1]。该纯内联计算不改状态、不睡眠，用于限制空闲链合并不跨 segment。
 */
static inline unsigned long io_tlb_offset(unsigned long val)
{
	return val & (IO_TLB_SEGSIZE - 1);
}

/*
 * nr_slots() - 把字节长度向上换算为所需 slot 数
 *
 * @val: 待覆盖的字节数；允许非 slot 对齐。返回 DIV_ROUND_UP 后的 slot 数；0 字节返回 0。
 * 该纯计算 helper 不持锁、不改变所有权，搜索入口会对不允许的零值另行断言。
 */
static inline unsigned long nr_slots(u64 val)
{
	return DIV_ROUND_UP(val, IO_TLB_SIZE);
}

/*
 * Early SWIOTLB allocation may be too early to allow an architecture to
 * perform the desired operations.  This function allows the architecture to
 * call SWIOTLB when the operations are possible.  It needs to be called
 * before the SWIOTLB memory is used.
 */
/*
 * swiotlb_update_mem_attributes() - 在架构准备就绪后把早期默认池改为设备可访问属性
 *
 * 入参：无。架构必须在任何 DMA 使用该池之前调用；函数借用默认池，不取得引用。若池不存在或
 * 它由晚期分配路径创建（该路径已经设置属性），则无事可做。否则按页覆盖完整数据区并调用
 * set_memory_decrypted()。返回：无直接返回值；副作用是页的加密属性改变，可能触发架构页表操作，
 * 调用环境必须允许该架构实现所需的同步。这里忽略返回值沿用初始化期既有契约。
 */
void __init swiotlb_update_mem_attributes(void)
{
	struct io_tlb_pool *mem = &io_tlb_default_mem.defpool;
	unsigned long bytes;

	if (!mem->nslabs || mem->late_alloc)
		return;
	bytes = PAGE_ALIGN(mem->nslabs << IO_TLB_SHIFT);
	set_memory_decrypted((unsigned long)mem->vaddr, bytes >> PAGE_SHIFT);
}

/*
 * swiotlb_init_io_tlb_pool() - 把已取得的数据区和元数据数组初始化成可分配 pool
 *
 * @mem: 待初始化的 pool，调用者独占；areas[]、slots[] 已分配，成功后由上层分配器拥有。
 * @start: bounce 数据区起始物理地址，按页对齐。
 * @nslabs: 数据区 slot 总数，必须能被 @nareas 整除并满足 segment 布局。
 * @late_alloc: 数据区/元数据是否来自页分配器，决定 swiotlb_exit() 的释放方式。
 * @nareas: 独立加锁区数量，非零且为 2 的幂。
 *
 * pool 尚未发布，调用者独占且无需锁。函数建立范围和分区，初始化 area 锁与游标，再构造每个
 * slot 不跨 segment 的连续空闲计数，清零数据区并保存 vaddr。返回：无直接返回值；成功出口保证
 * pool 可由 add_mem_pool() 发布。该初始化可能耗时，不能放在原子上下文。
 */
static void swiotlb_init_io_tlb_pool(struct io_tlb_pool *mem, phys_addr_t start,
		unsigned long nslabs, bool late_alloc, unsigned int nareas)
{
	void *vaddr = phys_to_virt(start);
	unsigned long bytes = nslabs << IO_TLB_SHIFT, i;

	mem->nslabs = nslabs;
	mem->start = start;
	mem->end = mem->start + bytes;
	mem->late_alloc = late_alloc;
	mem->nareas = nareas;
	mem->area_nslabs = nslabs / mem->nareas;

	/* 阶段 1：为每个独立分配域建立锁、循环搜索起点和已用计数。 */
	for (i = 0; i < mem->nareas; i++) {
		spin_lock_init(&mem->areas[i].lock);
		mem->areas[i].index = 0;
		mem->areas[i].used = 0;
	}

	/*
	 * 阶段 2：list 值随位置递减并在 segment 或池尾截断，使搜索只读起点描述符即可判断是否有
	 * 足够连续空间；其余字段设置为“未映射”状态。
	 */
	for (i = 0; i < mem->nslabs; i++) {
		mem->slots[i].list = min(IO_TLB_SEGSIZE - io_tlb_offset(i),
					 mem->nslabs - i);
		mem->slots[i].orig_addr = INVALID_PHYS_ADDR;
		mem->slots[i].alloc_size = 0;
		mem->slots[i].pad_slots = 0;
	}

	/*
	 * 阶段 3：清除旧数据以免首次设备读取泄漏内容，再保存线性映射地址。直到 add_mem_pool()
	 * 执行发布之前，并发查找者仍不可见此 pool。
	 */
	memset(vaddr, 0, bytes);
	mem->vaddr = vaddr;
	return;
}

/**
 * add_mem_pool() - add a memory pool to the allocator
 * @mem:	Software IO TLB allocator.
 * @pool:	Memory pool to be added.
 */
/*
 * add_mem_pool() - 把完整 pool 发布给 SWIOTLB 分配器
 *
 * @mem: 目标分配器，借用指针；动态模式下其 lock 保护 pools 链表和总 nslabs。
 * @pool: 已完全初始化、尚未发布的 pool；成功后其生命周期交给 @mem 链表或默认池所有者。
 *
 * 动态模式在 mem->lock 下用 list_add_rcu() 发布对象并同步总容量，RCU 读者随后可发现它；锁内
 * 不睡眠。静态模式没有池链表，只记录 defpool 的容量。返回：无直接返回值；发布后不得在未摘链
 * 且未等待 RCU 宽限期的情况下释放动态 pool。
 */
static void add_mem_pool(struct io_tlb_mem *mem, struct io_tlb_pool *pool)
{
#ifdef CONFIG_SWIOTLB_DYNAMIC
	spin_lock(&mem->lock);
	list_add_rcu(&pool->node, &mem->pools);
	mem->nslabs += pool->nslabs;
	spin_unlock(&mem->lock);
#else
	mem->nslabs = pool->nslabs;
#endif
}

/*
 * swiotlb_memblock_alloc() - 从启动期 memblock 取得并可选重映射 bounce 数据区
 *
 * @nslabs: 期望 slot 数；函数按页计算实际分配字节数。
 * @flags: SWIOTLB_ANY 允许放在任意物理位置，否则必须来自低地址内存。
 * @remap: 可空架构回调；非空时借用已分配虚拟地址和 slot 数改变映射属性，可能失败。
 *
 * 运行于 memblock 可用的 __init 阶段，无运行期锁。成功返回数据区虚拟地址，所有权交给调用者并
 * 最终由默认池管理；分配失败返回 NULL。remap 失败时先归还 memblock 区域再返回 NULL，不泄漏。
 */
static void __init *swiotlb_memblock_alloc(unsigned long nslabs,
		unsigned int flags,
		int (*remap)(void *tlb, unsigned long nslabs))
{
	size_t bytes = PAGE_ALIGN(nslabs << IO_TLB_SHIFT);
	void *tlb;

	/*
	 * By default allocate the bounce buffer memory from low memory, but
	 * allow to pick a location everywhere for hypervisors with guest
	 * memory encryption.
	 */
	/*
	 * 默认选择低地址是为了让受限 DMA mask 的设备可达；只有明确的 SWIOTLB_ANY 场景（典型是
	 * 客户机内存加密由 hypervisor 提供额外转换）才放宽物理位置。
	 */
	if (flags & SWIOTLB_ANY)
		tlb = memblock_alloc(bytes, PAGE_SIZE);
	else
		tlb = memblock_alloc_low(bytes, PAGE_SIZE);

	if (!tlb) {
		pr_warn("%s: Failed to allocate %zu bytes tlb structure\n",
			__func__, bytes);
		return NULL;
	}

	if (remap && remap(tlb, nslabs) < 0) {
		memblock_free(tlb, PAGE_ALIGN(bytes));
		pr_warn("%s: Failed to remap %zu bytes\n", __func__, bytes);
		return NULL;
	}

	return tlb;
}

/*
 * Statically reserve bounce buffer space and initialize bounce buffer data
 * structures for the software IO TLB used to implement the DMA API.
 */
/*
 * swiotlb_init_remap() - 启动期预留、初始化并发布默认 SWIOTLB 池
 *
 * @addressing_limit: 平台是否存在需要 bounce 的设备寻址限制。
 * @flags: SWIOTLB_VERBOSE/FORCE/ANY 策略位；决定日志、强制中转和物理位置。
 * @remap: 可空架构回调；非空时在池发布前调整数据区映射，借用参数且不接管回调。
 *
 * 这是默认池的早期构造入口，在并发 DMA 开始前运行、不持锁。无需 SWIOTLB 或被 noforce 禁用时
 * 直接返回；否则确定动态增长上限和 area 数，连续内存失败时逐次减半到 1 MiB。取得数据区后再
 * 分配 slots/areas 元数据，初始化并通过 add_mem_pool() 发布。返回：无直接返回值；任何失败都
 * 留下分配器不可用状态。早期元数据失败分支不回收此前 memblock 分配，这是启动期保留行为而非
 * 可重试事务；成功后可选打印实际范围。
 */
void __init swiotlb_init_remap(bool addressing_limit, unsigned int flags,
		int (*remap)(void *tlb, unsigned long nslabs))
{
	struct io_tlb_pool *mem = &io_tlb_default_mem.defpool;
	unsigned long nslabs;
	unsigned int nareas;
	size_t alloc_size;
	void *tlb;

	/* 阶段 1：没有寻址需求且未强制 bounce，或命令行明确禁用时，不建立任何池。 */
	if (!addressing_limit && !swiotlb_force_bounce)
		return;
	if (swiotlb_force_disable)
		return;

	io_tlb_default_mem.force_bounce =
		swiotlb_force_bounce || (flags & SWIOTLB_FORCE);

#ifdef CONFIG_SWIOTLB_DYNAMIC
	/*
	 * 动态池只有在无需架构私有 remap 时才可增长，因为运行期 worker 无法重放该回调。phys_limit
	 * 则限定后续动态数据区必须落在设备可用的物理范围内。
	 */
	if (!remap)
		io_tlb_default_mem.can_grow = true;
	if (flags & SWIOTLB_ANY)
		io_tlb_default_mem.phys_limit = virt_to_phys(high_memory - 1);
	else
		io_tlb_default_mem.phys_limit = ARCH_LOW_ADDRESS_LIMIT;
#endif

	/* 阶段 2：默认按 possible CPU 数分区，随后在连续内存压力下成比例缩小池和 area 数。 */
	if (!default_nareas)
		swiotlb_adjust_nareas(num_possible_cpus());

	nslabs = default_nslabs;
	nareas = limit_nareas(default_nareas, nslabs);
	while ((tlb = swiotlb_memblock_alloc(nslabs, flags, remap)) == NULL) {
		if (nslabs <= IO_TLB_MIN_SLABS)
			return;
		nslabs = ALIGN(nslabs >> 1, IO_TLB_SEGSIZE);
		nareas = limit_nareas(nareas, nslabs);
	}

	/* 阶段 3：数据区取得成功后记录真实容量，再分别取得 slot 与 area 元数据。 */
	if (default_nslabs != nslabs) {
		pr_info("SWIOTLB bounce buffer size adjusted %lu -> %lu slabs",
			default_nslabs, nslabs);
		default_nslabs = nslabs;
	}

	alloc_size = PAGE_ALIGN(array_size(sizeof(*mem->slots), nslabs));
	mem->slots = memblock_alloc(alloc_size, PAGE_SIZE);
	if (!mem->slots) {
		pr_warn("%s: Failed to allocate %zu bytes align=0x%lx\n",
			__func__, alloc_size, PAGE_SIZE);
		return;
	}

	mem->areas = memblock_alloc(array_size(sizeof(struct io_tlb_area),
		nareas), SMP_CACHE_BYTES);
	if (!mem->areas) {
		pr_warn("%s: Failed to allocate mem->areas.\n", __func__);
		return;
	}

	/* 阶段 4：构造所有空闲元数据并发布默认池；此后 DMA 映射路径才可观察并使用它。 */
	swiotlb_init_io_tlb_pool(mem, __pa(tlb), nslabs, false, nareas);
	add_mem_pool(&io_tlb_default_mem, mem);

	if (flags & SWIOTLB_VERBOSE)
		swiotlb_print_info();
}

/*
 * swiotlb_init() - 使用普通线性映射策略建立启动期默认池
 *
 * @addressing_limit: 平台是否有设备寻址限制。
 * @flags: 传给 swiotlb_init_remap() 的初始化策略位。
 *
 * 这是无需架构 remap 回调时的薄封装，运行于 __init 阶段且继承下层的睡眠、失败和所有权语义。
 * 返回：无直接返回值；池是否成功建立需由 is_swiotlb_allocated() 等查询，下一步通常是设备初始化。
 */
void __init swiotlb_init(bool addressing_limit, unsigned int flags)
{
	swiotlb_init_remap(addressing_limit, flags, NULL);
}

/*
 * Systems with larger DMA zones (those that don't support ISA) can
 * initialize the swiotlb later using the slab allocator if needed.
 * This should be just like above, but with some error catching.
 */
/*
 * swiotlb_init_late() - 使用页分配器在运行期初始化默认池
 *
 * @size: 期望 bounce 数据区字节数，向完整 segment/page order 对齐，内存不足时可缩小。
 * @gfp_mask: 数据区的页分配约束，决定可睡眠性和 DMA/DMA32 zone，也派生动态池物理上限。
 * @remap: 可空架构回调；非空时借用候选数据区并改变属性，失败可触发缩容重试。
 *
 * 供具有较大 DMA zone、无需早期预留的平台在页分配器就绪后调用。已有池或 noforce 时返回 0；
 * 数据区、areas 或 slots 获取失败返回 -ENOMEM，remap 反复失败且低于下限时返回其 errno。成功后
 * pool 接管三类分配，并将数据区设置为解密后发布；错误标签按 areas -> 数据区逆序回滚。调用者
 * 不持 SWIOTLB 锁，gfp 允许时函数可睡眠。
 */
int swiotlb_init_late(size_t size, gfp_t gfp_mask,
		int (*remap)(void *tlb, unsigned long nslabs))
{
	struct io_tlb_pool *mem = &io_tlb_default_mem.defpool;
	unsigned long nslabs = ALIGN(size >> IO_TLB_SHIFT, IO_TLB_SEGSIZE);
	unsigned int nareas;
	unsigned char *vstart = NULL;
	unsigned int order, area_order;
	bool retried = false;
	int rc = 0;

	if (io_tlb_default_mem.nslabs)
		return 0;

	if (swiotlb_force_disable)
		return 0;

	io_tlb_default_mem.force_bounce = swiotlb_force_bounce;

#ifdef CONFIG_SWIOTLB_DYNAMIC
	if (!remap)
		io_tlb_default_mem.can_grow = true;
	if (IS_ENABLED(CONFIG_ZONE_DMA) && (gfp_mask & __GFP_DMA))
		io_tlb_default_mem.phys_limit = zone_dma_limit;
	else if (IS_ENABLED(CONFIG_ZONE_DMA32) && (gfp_mask & __GFP_DMA32))
		io_tlb_default_mem.phys_limit = max(DMA_BIT_MASK(32), zone_dma_limit);
	else
		io_tlb_default_mem.phys_limit = virt_to_phys(high_memory - 1);
#endif

	/* 阶段 1：确定 area 数，把请求转换为 buddy order，并在分配失败时逐阶降级。 */
	if (!default_nareas)
		swiotlb_adjust_nareas(num_possible_cpus());

retry:
	order = get_order(nslabs << IO_TLB_SHIFT);
	nslabs = SLABS_PER_PAGE << order;

	while ((SLABS_PER_PAGE << order) > IO_TLB_MIN_SLABS) {
		vstart = (void *)__get_free_pages(gfp_mask | __GFP_NOWARN,
						  order);
		if (vstart)
			break;
		order--;
		nslabs = SLABS_PER_PAGE << order;
		retried = true;
	}

	if (!vstart)
		return -ENOMEM;

	/*
	 * 阶段 2：架构 remap 失败时释放当前候选区并把目标减半；这与单纯分配失败共享缩容策略，
	 * 但最终向调用者保留回调给出的具体 errno。
	 */
	if (remap)
		rc = remap(vstart, nslabs);
	if (rc) {
		free_pages((unsigned long)vstart, order);

		nslabs = ALIGN(nslabs >> 1, IO_TLB_SEGSIZE);
		if (nslabs < IO_TLB_MIN_SLABS)
			return rc;
		retried = true;
		goto retry;
	}

	if (retried) {
		pr_warn("only able to allocate %ld MB\n",
			(PAGE_SIZE << order) >> 20);
	}

	/* 阶段 3：数据区已确定，分配同生命周期的 area/slot 元数据；失败由末尾标签逆序回滚。 */
	nareas = limit_nareas(default_nareas, nslabs);
	area_order = get_order(array_size(sizeof(*mem->areas), nareas));
	mem->areas = (struct io_tlb_area *)
		__get_free_pages(GFP_KERNEL | __GFP_ZERO, area_order);
	if (!mem->areas)
		goto error_area;

	mem->slots = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
		get_order(array_size(sizeof(*mem->slots), nslabs)));
	if (!mem->slots)
		goto error_slots;

	/* 阶段 4：使数据区对加密内存平台的设备可见，初始化元数据并发布池。 */
	set_memory_decrypted((unsigned long)vstart,
			     (nslabs << IO_TLB_SHIFT) >> PAGE_SHIFT);
	swiotlb_init_io_tlb_pool(mem, virt_to_phys(vstart), nslabs, true,
				 nareas);
	add_mem_pool(&io_tlb_default_mem, mem);

	swiotlb_print_info();
	return 0;

error_slots:
	/* areas 已取得但 slots 失败，只需先释放 areas，再落入数据区回滚。 */
	free_pages((unsigned long)mem->areas, area_order);
error_area:
	/* 此处只有数据区仍归本函数，释放后返回统一的元数据分配失败。 */
	free_pages((unsigned long)vstart, order);
	return -ENOMEM;
}

/*
 * swiotlb_exit() - 在默认池不再需要时恢复内存属性并释放其全部资源
 *
 * 入参：无。仅初始化/回收阶段调用，要求没有未完成映射或并发池使用者；函数借用默认池后完成
 * 最终销毁。强制 bounce 的系统不能撤池，空池也直接返回。根据 late_alloc 区分页分配器与
 * memblock 两种所有权来源逆向释放 areas、数据区和 slots，最后清零描述符使查询者看到未初始化。
 * 返回：无直接返回值；set_memory_encrypted() 的结果沿用现有初始化期契约未向外报告。
 */
void __init swiotlb_exit(void)
{
	struct io_tlb_pool *mem = &io_tlb_default_mem.defpool;
	unsigned long tbl_vaddr;
	size_t tbl_size, slots_size;
	unsigned int area_order;

	if (swiotlb_force_bounce)
		return;

	if (!mem->nslabs)
		return;

	/* 阶段 1：在释放物理页前恢复 CPU 侧加密属性，并计算与原分配匹配的尺寸/order。 */
	pr_info("tearing down default memory pool\n");
	tbl_vaddr = (unsigned long)phys_to_virt(mem->start);
	tbl_size = PAGE_ALIGN(mem->end - mem->start);
	slots_size = PAGE_ALIGN(array_size(sizeof(*mem->slots), mem->nslabs));

	set_memory_encrypted(tbl_vaddr, tbl_size >> PAGE_SHIFT);
	/* 阶段 2：严格按 late_alloc 记录选择对应分配器，不能混用 memblock_free 与 free_pages。 */
	if (mem->late_alloc) {
		area_order = get_order(array_size(sizeof(*mem->areas),
			mem->nareas));
		free_pages((unsigned long)mem->areas, area_order);
		free_pages(tbl_vaddr, get_order(tbl_size));
		free_pages((unsigned long)mem->slots, get_order(slots_size));
	} else {
		memblock_free(mem->areas,
			array_size(sizeof(*mem->areas), mem->nareas));
		memblock_phys_free(mem->start, tbl_size);
		memblock_free(mem->slots, slots_size);
	}

	/* 阶段 3：资源全部归还后清除范围和指针，防止后续查询把旧地址误判为有效池。 */
	memset(mem, 0, sizeof(*mem));
}

#ifdef CONFIG_SWIOTLB_DYNAMIC

/**
 * alloc_dma_pages() - allocate pages to be used for DMA
 * @gfp:	GFP flags for the allocation.
 * @bytes:	Size of the buffer.
 * @phys_limit:	Maximum allowed physical address of the buffer.
 *
 * Allocate pages from the buddy allocator. If successful, make the allocated
 * pages decrypted that they can be used for DMA.
 *
 * Return: Decrypted pages, %NULL on allocation failure, or ERR_PTR(-EAGAIN)
 * if the allocated physical address was above @phys_limit.
 */
/*
 * alloc_dma_pages() - 分配物理上限内、已解密且可供设备 DMA 的连续页
 *
 * @gfp: buddy 分配标志；是否可阻塞由调用者选择。
 * @bytes: 所需连续字节数，函数按 get_order() 实际取得 2 的幂页块。
 * @phys_limit: 允许的最后物理地址（含），整段页块不得越界。
 *
 * 无外部锁。分配失败返回 NULL；页块落在上限之外时先释放并返回 ERR_PTR(-EAGAIN)，提示上层换
 * 更低 zone 重试；解密失败返回 NULL。成功返回持有的 struct page，所有权交给调用者并最终由
 * swiotlb_free_tlb() 释放。set_memory_decrypted() 可能阻塞，所以原子场景不得走此路径。
 */
static struct page *alloc_dma_pages(gfp_t gfp, size_t bytes, u64 phys_limit)
{
	unsigned int order = get_order(bytes);
	struct page *page;
	phys_addr_t paddr;
	void *vaddr;

	/* 阶段 1：按连续页 order 分配；NULL 表示当前 GFP/zone 内无可用页块。 */
	page = alloc_pages(gfp, order);
	if (!page)
		return NULL;

	/* 阶段 2：验证整个请求长度而非仅首页地址都没有超过设备物理上限。 */
	paddr = page_to_phys(page);
	if (paddr + bytes - 1 > phys_limit) {
		__free_pages(page, order);
		return ERR_PTR(-EAGAIN);
	}

	/* 阶段 3：把页改为共享/解密属性，设备才能看到与 CPU 一致的数据。 */
	vaddr = phys_to_virt(paddr);
	if (set_memory_decrypted((unsigned long)vaddr, PFN_UP(bytes)))
		goto error;
	return page;

error:
	/* Intentional leak if pages cannot be encrypted again. */
	/*
	 * 若解密失败后连恢复加密也失败，释放页面会让伙伴分配器把属性未知的页重新交给普通内存，
	 * 可能破坏机密性或一致性；因此仅在恢复成功时释放，否则有意泄漏以隔离危险页。
	 */
	if (!set_memory_encrypted((unsigned long)vaddr, PFN_UP(bytes)))
		__free_pages(page, order);
	return NULL;
}

/**
 * swiotlb_alloc_tlb() - allocate a dynamic IO TLB buffer
 * @dev:	Device for which a memory pool is allocated.
 * @bytes:	Size of the buffer.
 * @phys_limit:	Maximum allowed physical address of the buffer.
 * @gfp:	GFP flags for the allocation.
 *
 * Return: Allocated pages, or %NULL on allocation failure.
 */
/*
 * swiotlb_alloc_tlb() - 按上下文和物理上限选择动态 bounce 数据区来源
 *
 * @dev: 目标设备，可为 NULL（后台扩默认池）；借用，不改变引用。
 * @bytes: 连续数据区字节数。
 * @phys_limit: 末地址上限（含）。
 * @gfp: 分配/阻塞约束。
 *
 * 原子且设备要求未加密 DMA 时，解密操作不能现场执行，改从预先准备的 coherent atomic pool
 * 领取；其他情况清除调用者 zone 位并由 phys_limit 选择 DMA/DMA32/普通 zone。地址过高时逐级
 * 降到更低 zone。成功返回持有页，失败返回 NULL；所有权交给新 pool 并最终对称释放。
 */
static struct page *swiotlb_alloc_tlb(struct device *dev, size_t bytes,
		u64 phys_limit, gfp_t gfp)
{
	struct page *page;

	/*
	 * Allocate from the atomic pools if memory is encrypted and
	 * the allocation is atomic, because decrypting may block.
	 */
	/*
	 * 内存加密平台的属性转换可能睡眠；当 GFP 禁止阻塞时只能使用启动期已解密的 coherent pool。
	 * 未配置该 pool 时必须失败，不能在原子上下文冒险调用 set_memory_decrypted()。
	 */
	if (!gfpflags_allow_blocking(gfp) && dev && force_dma_unencrypted(dev)) {
		void *vaddr;

		if (!IS_ENABLED(CONFIG_DMA_COHERENT_POOL))
			return NULL;

		return dma_alloc_from_pool(dev, bytes, &vaddr, gfp,
					   dma_coherent_ok);
	}

	/* 阶段 2：由实际物理上限重建 zone 约束，避免调用者传入与设备能力矛盾的 zone 位。 */
	gfp &= ~GFP_ZONEMASK;
	if (phys_limit <= zone_dma_limit)
		gfp |= __GFP_DMA;
	else if (phys_limit <= DMA_BIT_MASK(32))
		gfp |= __GFP_DMA32;

	/* 阶段 3：-EAGAIN 只表示地址过高，逐级收紧 zone；真正缺页的 NULL 会结束循环并返回。 */
	while (IS_ERR(page = alloc_dma_pages(gfp, bytes, phys_limit))) {
		if (IS_ENABLED(CONFIG_ZONE_DMA32) &&
		    phys_limit < DMA_BIT_MASK(64) &&
		    !(gfp & (__GFP_DMA32 | __GFP_DMA)))
			gfp |= __GFP_DMA32;
		else if (IS_ENABLED(CONFIG_ZONE_DMA) &&
			 !(gfp & __GFP_DMA))
			gfp = (gfp & ~__GFP_DMA32) | __GFP_DMA;
		else
			return NULL;
	}

	return page;
}

/**
 * swiotlb_free_tlb() - free a dynamically allocated IO TLB buffer
 * @vaddr:	Virtual address of the buffer.
 * @bytes:	Size of the buffer.
 */
/*
 * swiotlb_free_tlb() - 释放动态池的数据区并恢复普通内存属性
 *
 * @vaddr: 数据区线性虚拟地址，来自 coherent pool 或 alloc_dma_pages()。
 * @bytes: 原请求字节数，用于匹配 pool/page order。
 *
 * 调用者必须先摘除 pool 并等到 RCU 读者退出。若地址属于 coherent pool，helper 已完成释放；
 * 否则先恢复加密属性，成功后归还 buddy。返回：无直接返回值。恢复失败时故意保留页，防止属性
 * 异常的内存再次进入普通分配器；该路径可能执行架构同步，不能在硬中断中调用。
 */
static void swiotlb_free_tlb(void *vaddr, size_t bytes)
{
	if (IS_ENABLED(CONFIG_DMA_COHERENT_POOL) &&
	    dma_free_from_pool(NULL, vaddr, bytes))
		return;

	/* Intentional leak if pages cannot be encrypted again. */
	/* 恢复加密失败时有意泄漏；安全隔离优先于回收容量，原因同 alloc_dma_pages() 错误路径。 */
	if (!set_memory_encrypted((unsigned long)vaddr, PFN_UP(bytes)))
		__free_pages(virt_to_page(vaddr), get_order(bytes));
}

/**
 * swiotlb_alloc_pool() - allocate a new IO TLB memory pool
 * @dev:	Device for which a memory pool is allocated.
 * @minslabs:	Minimum number of slabs.
 * @nslabs:	Desired (maximum) number of slabs.
 * @nareas:	Number of areas.
 * @phys_limit:	Maximum DMA buffer physical address.
 * @gfp:	GFP flags for the allocations.
 *
 * Allocate and initialize a new IO TLB memory pool. The actual number of
 * slabs may be reduced if allocation of @nslabs fails. If even
 * @minslabs cannot be allocated, this function fails.
 *
 * Return: New memory pool, or %NULL on allocation failure.
 */
/*
 * swiotlb_alloc_pool() - 分配并完整构造一个尚未发布的动态 pool
 *
 * @dev: 发起瞬态扩池的设备，可为 NULL（后台公共扩池）；借用。
 * @minslabs: 可接受的最小 slot 数。
 * @nslabs: 期望最大 slot 数，受 MAX_PAGE_ORDER 限制并可逐次减半。
 * @nareas: 期望独立锁分区数，随实际容量下调。
 * @phys_limit: 数据区末地址上限（含）。
 * @gfp: 描述符、slot 元数据和数据页共同遵守的分配约束。
 *
 * 返回前对象不对 RCU 读者可见。先让 areas[] 尾随在 pool 描述符后，再取得可达且解密的数据区，
 * 最后取得 slots[] 并初始化。成功返回独占 pool，调用者须发布或释放；失败返回 NULL，cleanup
 * 标签按资源取得顺序逆向撤销，不留下半初始化对象。
 */
static struct io_tlb_pool *swiotlb_alloc_pool(struct device *dev,
		unsigned long minslabs, unsigned long nslabs,
		unsigned int nareas, u64 phys_limit, gfp_t gfp)
{
	struct io_tlb_pool *pool;
	unsigned int slot_order;
	struct page *tlb;
	size_t pool_size;
	size_t tlb_size;

	/* 阶段 1：单个连续页块不能超过 buddy 最大 order，同时收缩 area 数保持布局合法。 */
	if (nslabs > SLABS_PER_PAGE << MAX_PAGE_ORDER) {
		nslabs = SLABS_PER_PAGE << MAX_PAGE_ORDER;
		nareas = limit_nareas(nareas, nslabs);
	}

	/* 阶段 2：描述符与 areas[] 同一 kzalloc 生命周期；areas 指向尾随空间，不可单独释放。 */
	pool_size = sizeof(*pool) + array_size(sizeof(*pool->areas), nareas);
	pool = kzalloc(pool_size, gfp);
	if (!pool)
		goto error;
	pool->areas = (void *)pool + sizeof(*pool);

	/* 阶段 3：数据区分配失败时逐次减半，但不能低于调用者要求的 minslabs。 */
	tlb_size = nslabs << IO_TLB_SHIFT;
	while (!(tlb = swiotlb_alloc_tlb(dev, tlb_size, phys_limit, gfp))) {
		if (nslabs <= minslabs)
			goto error_tlb;
		nslabs = ALIGN(nslabs >> 1, IO_TLB_SEGSIZE);
		nareas = limit_nareas(nareas, nslabs);
		tlb_size = nslabs << IO_TLB_SHIFT;
	}

	/* 阶段 4：取得独立 slot 描述符页块，随后初始化；成功出口仍未发布。 */
	slot_order = get_order(array_size(sizeof(*pool->slots), nslabs));
	pool->slots = (struct io_tlb_slot *)
		__get_free_pages(gfp, slot_order);
	if (!pool->slots)
		goto error_slots;

	swiotlb_init_io_tlb_pool(pool, page_to_phys(tlb), nslabs, true, nareas);
	return pool;

error_slots:
	/* slots 分配失败：先释放已解密的数据区，再落入描述符释放。 */
	swiotlb_free_tlb(page_address(tlb), tlb_size);
error_tlb:
	/* pool 与内嵌 areas[] 共用一次 kzalloc，单次 kfree 即完整撤销。 */
	kfree(pool);
error:
	return NULL;
}

/**
 * swiotlb_dyn_alloc() - dynamic memory pool allocation worker
 * @work:	Pointer to dyn_alloc in struct io_tlb_mem.
 */
/*
 * swiotlb_dyn_alloc() - 在可睡眠 workqueue 上为公共分配器补充常驻 pool
 *
 * @work: 嵌入 io_tlb_mem 的 dyn_alloc；借用，container_of() 恢复所属分配器且不增加引用。
 *
 * 映射快路径发现容量不足时调度本 worker。它用 GFP_KERNEL 建立从 1 MiB 到默认容量的 pool；
 * 失败仅限速告警，成功则由 add_mem_pool() 转移所有权并 RCU 发布。workqueue 进程上下文可睡眠，
 * 入口不持 area/mem 锁。返回：无直接返回值。
 */
static void swiotlb_dyn_alloc(struct work_struct *work)
{
	struct io_tlb_mem *mem =
		container_of(work, struct io_tlb_mem, dyn_alloc);
	struct io_tlb_pool *pool;

	pool = swiotlb_alloc_pool(NULL, IO_TLB_MIN_SLABS, default_nslabs,
				  default_nareas, mem->phys_limit, GFP_KERNEL);
	if (!pool) {
		pr_warn_ratelimited("Failed to allocate new pool");
		return;
	}

	add_mem_pool(mem, pool);
}

/**
 * swiotlb_dyn_free() - RCU callback to free a memory pool
 * @rcu:	RCU head in the corresponding struct io_tlb_pool.
 */
/*
 * swiotlb_dyn_free() - RCU 宽限期后最终释放动态 pool
 *
 * @rcu: 嵌入待销毁 io_tlb_pool 的回调头；call_rcu() 已把最终释放责任转交给本函数。
 *
 * 回调运行时 pool 已从所有查找链表摘除，先前 RCU 读者也已退出，因此可安全释放 slots、恢复并
 * 释放 bounce 数据区，最后 kfree 包含 areas[] 的 pool。返回：无直接返回值。RCU callback 不能
 * 睡眠；这里依赖相应内存属性 helper 在该回调环境可用的现有契约。
 */
static void swiotlb_dyn_free(struct rcu_head *rcu)
{
	struct io_tlb_pool *pool = container_of(rcu, struct io_tlb_pool, rcu);
	size_t slots_size = array_size(sizeof(*pool->slots), pool->nslabs);
	size_t tlb_size = pool->end - pool->start;

	free_pages((unsigned long)pool->slots, get_order(slots_size));
	swiotlb_free_tlb(pool->vaddr, tlb_size);
	kfree(pool);
}

/**
 * __swiotlb_find_pool() - find the IO TLB pool for a physical address
 * @dev:        Device which has mapped the DMA buffer.
 * @paddr:      Physical address within the DMA buffer.
 *
 * Find the IO TLB memory pool descriptor which contains the given physical
 * address, if any. This function is for use only when the dev is known to
 * be using swiotlb. Use swiotlb_find_pool() for the more general case
 * when this condition is not met.
 *
 * Return: Memory pool which contains @paddr, or %NULL if none.
 */
/*
 * __swiotlb_find_pool() - 在公共池和设备瞬态池中定位包含物理地址的 pool
 *
 * @dev: 已知使用 SWIOTLB 的设备，借用；其 dma_io_tlb_mem 与私有池链表已初始化。
 * @paddr: 待分类的物理地址，可以位于 bounce buffer 任意字节。
 *
 * 函数内部持 rcu_read_lock()，先查分配器公共 pools，再查设备私有瞬态 pools；命中返回借用的
 * pool 指针，未命中返回 NULL。返回指针不会因摘链立即释放，但调用者必须处在映射仍有效的更高层
 * 生命周期内，不能把该裸指针无限期保存。读侧不睡眠、不修改状态。
 */
struct io_tlb_pool *__swiotlb_find_pool(struct device *dev, phys_addr_t paddr)
{
	struct io_tlb_mem *mem = dev->dma_io_tlb_mem;
	struct io_tlb_pool *pool;

	/* 阶段 1：RCU 保证遍历期间节点内存存活；范围字段在发布前已完整初始化且之后不变。 */
	rcu_read_lock();
	list_for_each_entry_rcu(pool, &mem->pools, node) {
		if (paddr >= pool->start && paddr < pool->end)
			goto out;
	}

	/* 阶段 2：公共池未命中后再查仅属于该设备的按需瞬态池。 */
	list_for_each_entry_rcu(pool, &dev->dma_io_tlb_pools, node) {
		if (paddr >= pool->start && paddr < pool->end)
			goto out;
	}
	pool = NULL;
out:
	rcu_read_unlock();
	return pool;
}

/**
 * swiotlb_del_pool() - remove an IO TLB pool from a device
 * @dev:	Owning device.
 * @pool:	Memory pool to be removed.
 */
/*
 * swiotlb_del_pool() - 从设备瞬态池链表摘除 pool 并安排延迟释放
 *
 * @dev: pool 所属设备，借用；dma_io_tlb_lock 保护其私有链表更新。
 * @pool: 已发布在 dev->dma_io_tlb_pools、且调用者确认可销毁的对象；所有权转给 RCU 回调。
 *
 * spin_lock_irqsave() 与映射路径的发布竞争并兼容本地中断上下文；list_del_rcu() 阻止新读者发现，
 * call_rcu() 则等旧读者退出后执行 swiotlb_dyn_free()。返回：无直接返回值，不睡眠；调用者之后
 * 不得再解引用 pool。
 */
static void swiotlb_del_pool(struct device *dev, struct io_tlb_pool *pool)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->dma_io_tlb_lock, flags);
	list_del_rcu(&pool->node);
	spin_unlock_irqrestore(&dev->dma_io_tlb_lock, flags);

	call_rcu(&pool->rcu, swiotlb_dyn_free);
}

#endif	/* CONFIG_SWIOTLB_DYNAMIC */
/* 以上结束动态 pool 的分配、发布、查找、摘除与 RCU 延迟释放实现。 */

/**
 * swiotlb_dev_init() - initialize swiotlb fields in &struct device
 * @dev:	Device to be initialized.
 */
/*
 * swiotlb_dev_init() - 为新 device 建立 SWIOTLB 关联和动态查找状态
 *
 * @dev: 尚未开始 DMA 映射的设备对象，借用；调用者保证独占初始化。
 *
 * 默认把设备指向全局分配器；动态配置再初始化设备私有瞬态池链表、保护锁和“曾使用 SWIOTLB”
 * 快速判断位。返回：无直接返回值，不分配内存、不睡眠。初始化完成后 DMA map/unmap 可并发使用
 * 这些字段，设备销毁须晚于所有映射完成。
 */
void swiotlb_dev_init(struct device *dev)
{
	dev->dma_io_tlb_mem = &io_tlb_default_mem;
#ifdef CONFIG_SWIOTLB_DYNAMIC
	INIT_LIST_HEAD(&dev->dma_io_tlb_pools);
	spin_lock_init(&dev->dma_io_tlb_lock);
	dev->dma_uses_io_tlb = false;
#endif
}

/**
 * swiotlb_align_offset() - Get required offset into an IO TLB allocation.
 * @dev:         Owning device.
 * @align_mask:  Allocation alignment mask.
 * @addr:        DMA address.
 *
 * Return the minimum offset from the start of an IO TLB allocation which is
 * required for a given buffer address and allocation alignment to keep the
 * device happy.
 *
 * First, the address bits covered by min_align_mask must be identical in the
 * original address and the bounce buffer address. High bits are preserved by
 * choosing a suitable IO TLB slot, but bits below IO_TLB_SHIFT require extra
 * padding bytes before the bounce buffer.
 *
 * Second, @align_mask specifies which bits of the first allocated slot must
 * be zero. This may require allocating additional padding slots, and then the
 * offset (in bytes) from the first such padding slot is returned.
 */
/*
 * swiotlb_align_offset() - 计算映射返回地址在已分配空间内必须保留的字节偏移
 *
 * @dev: 目标设备，借用，用于读取最小对齐掩码。
 * @align_mask: 本次分配首地址必须满足的额外对齐掩码。
 * @addr: 原始物理/DMA 地址，其低位需在 bounce 地址中保留。
 *
 * 返回小于等于掩码覆盖范围的字节偏移，使设备要求的 min_align_mask 低位与原地址一致，同时兼顾
 * 显式分配对齐。纯计算、不持锁、不睡眠；调用者把跨 slot 的部分转为 pad_slots。
 */
static unsigned int swiotlb_align_offset(struct device *dev,
					 unsigned int align_mask, u64 addr)
{
	return addr & dma_get_min_align_mask(dev) &
		(align_mask | (IO_TLB_SIZE - 1));
}

/*
 * Bounce: copy the swiotlb buffer from or back to the original dma location
 */
/*
 * swiotlb_bounce() - 在原始缓冲区与 bounce buffer 之间复制一段映射数据
 *
 * @dev: 执行 DMA 的设备，借用；用于一致性、对齐和告警策略。
 * @tlb_addr: bounce buffer 内本次同步起点的物理地址，必须属于 @mem 的有效映射。
 * @size: 请求复制的字节数；若超过登记的剩余分配长度会告警并截断，防止越界。
 * @dir: 此次复制方向；DMA_TO_DEVICE 表示原缓冲区到 bounce，DMA_FROM_DEVICE 表示反向写回。
 * @mem: 包含地址的 pool，借用；映射生命周期保证其及 slot 元数据存活。
 *
 * map、sync 与 unmap 路径调用本函数。它不取得 area 锁，因为 slot 在 DMA 映射解除前不会被重新
 * 分配；调用者负责 DMA 所有权时序。函数支持低端线性映射和逐页 highmem，后者短暂关本地中断。
 * 返回：无直接返回值；副作用是目标缓冲区内容改变，FROM_DEVICE 时还把 KMSAN 状态标为已初始化。
 */
static void swiotlb_bounce(struct device *dev, phys_addr_t tlb_addr, size_t size,
			   enum dma_data_direction dir, struct io_tlb_pool *mem)
{
	int index = (tlb_addr - mem->start) >> IO_TLB_SHIFT;
	phys_addr_t orig_addr = mem->slots[index].orig_addr;
	size_t alloc_size = mem->slots[index].alloc_size;
	unsigned long pfn = PFN_DOWN(orig_addr);
	unsigned char *vaddr = mem->vaddr + tlb_addr - mem->start;
	int tlb_offset;

	/* 阶段 1：无原地址的 padding slot 不承载数据；正常调用应落在首个非 padding slot 之后。 */
	if (orig_addr == INVALID_PHYS_ADDR)
		return;

	/* 非一致设备写回 CPU 前先完成架构要求的 DMA flush，再读取 bounce 数据。 */
	if (dir == DMA_FROM_DEVICE && !dev_is_dma_coherent(dev))
		arch_sync_dma_flush();

	/*
	 * It's valid for tlb_offset to be negative. This can happen when the
	 * "offset" returned by swiotlb_align_offset() is non-zero, and the
	 * tlb_addr is pointing within the first "offset" bytes of the second
	 * or subsequent slots of the allocated swiotlb area. While it's not
	 * valid for tlb_addr to be pointing within the first "offset" bytes
	 * of the first slot, there's no way to check for such an error since
	 * this function can't distinguish the first slot from the second and
	 * subsequent slots.
	 */
	/*
	 * tlb_offset 允许为负：返回地址带有对齐偏移，而调用者可能同步后续 slot 开头的子范围。函数无法
	 * 仅凭地址区分“首 slot 的非法前缀”和“后续 slot 的合法前缀”，因此由上层保证范围合法。
	 */
	tlb_offset = (tlb_addr & (IO_TLB_SIZE - 1)) -
		     swiotlb_align_offset(dev, 0, orig_addr);

	orig_addr += tlb_offset;
	alloc_size -= tlb_offset;

	/* 阶段 2：用每个 slot 登记的剩余长度约束复制，告警后截断以避免损坏相邻映射。 */
	if (size > alloc_size) {
		dev_WARN_ONCE(dev, 1,
			"Buffer overflow detected. Allocation size: %zu. Mapping size: %zu.\n",
			alloc_size, size);
		size = alloc_size;
	}

	/* 阶段 3：highmem 原缓冲区没有永久线性映射，必须逐页复制；低端内存可一次完成。 */
	if (PageHighMem(pfn_to_page(pfn))) {
		unsigned int offset = orig_addr & ~PAGE_MASK;
		struct page *page;
		unsigned int sz = 0;
		unsigned long flags;

		while (size) {
			sz = min_t(size_t, PAGE_SIZE - offset, size);

			local_irq_save(flags);
			page = pfn_to_page(pfn);
			if (dir == DMA_TO_DEVICE) {
				/*
				 * Ideally, kmsan_check_highmem_page()
				 * could be used here to detect infoleaks,
				 * but callers may map uninitialized buffers
				 * that will be written by the device,
				 * causing false positives.
				 */
				/*
				 * 理想情况下可检查 highmem 页是否含未初始化数据以发现信息泄漏，但合法驱动会把
				 * 未初始化缓冲区交给设备写满；在映射时检查会误报，所以这里只执行复制。
				 */
				memcpy_from_page(vaddr, page, offset, sz);
			} else {
				kmsan_unpoison_memory(vaddr, sz);
				memcpy_to_page(page, offset, vaddr, sz);
			}
			local_irq_restore(flags);

			size -= sz;
			pfn++;
			vaddr += sz;
			offset = 0;
		}
	} else if (dir == DMA_TO_DEVICE) {
		/*
		 * Ideally, kmsan_check_memory() could be used here to detect
		 * infoleaks (uninitialized data being sent to device), but
		 * callers may map uninitialized buffers that will be written
		 * by the device, causing false positives.
		 */
		/*
		 * 低端内存同样不做 KMSAN 源检查：设备写入型缓冲区在 DMA 前允许未初始化，检查会把合法
		 * 用法误判为向设备泄漏。FROM_DEVICE 分支则在写回前解除 bounce 数据的毒化状态。
		 */
		memcpy(vaddr, phys_to_virt(orig_addr), size);
	} else {
		kmsan_unpoison_memory(vaddr, size);
		memcpy(phys_to_virt(orig_addr), vaddr, size);
	}
}

/*
 * slot_addr() - 把 pool 起点和 slot 索引换算为物理地址
 *
 * @start: pool 起始物理地址；@idx: slot 索引。返回 start + idx * IO_TLB_SIZE。纯算术、不检查
 * 越界、不改状态；调用者必须保证索引属于相应 pool。
 */
static inline phys_addr_t slot_addr(phys_addr_t start, phys_addr_t idx)
{
	return start + (idx << IO_TLB_SHIFT);
}

/*
 * Carefully handle integer overflow which can occur when boundary_mask == ~0UL.
 */
/*
 * get_max_slots() - 把 DMA segment boundary 掩码换算为最多可跨越的 slot 数
 *
 * @boundary_mask: 设备允许单段地址变化的低位掩码。先右移再加一，避免对 ~0UL 直接加一溢出；
 * 返回单位为 slot。该纯计算 helper 不持锁、不睡眠，供边界检查和搜索步长计算使用。
 */
static inline unsigned long get_max_slots(unsigned long boundary_mask)
{
	return (boundary_mask >> IO_TLB_SHIFT) + 1;
}

/*
 * wrap_area_index() - 将区内搜索索引环回到起点
 *
 * @mem: 被搜索 pool，借用，仅读取 area_nslabs。
 * @index: 候选区内索引。小于区长度时原样返回，否则返回 0；调用者的步长保证无需保留多次绕回
 * 余数。纯计算、不睡眠，用于维护 area->index 的循环搜索策略。
 */
static unsigned int wrap_area_index(struct io_tlb_pool *mem, unsigned int index)
{
	if (index >= mem->area_nslabs)
		return 0;
	return index;
}

/*
 * Track the total used slots with a global atomic value in order to have
 * correct information to determine the high water mark. The mem_used()
 * function gives imprecise results because there's no locking across
 * multiple areas.
 */
/*
 * 下方全局原子计数只为 debugfs 提供精确的总使用量与高水位；area->used 虽能无锁相加，但跨
 * area 读取不是同一时刻快照。原子统计不参与分配正确性，关闭 DEBUG_FS 时 helper 退化为空操作。
 */
#ifdef CONFIG_DEBUG_FS
/*
 * inc_used_and_hiwater() - 原子增加已用 slot 并单调推进历史高水位
 *
 * @mem: 分配器，借用；@nslots: 本次新占用 slot 数。可在持锁外并发调用，不睡眠。total_used
 * 先原子增加，再用 cmpxchg 循环仅在新值更大时更新 used_hiwater；竞争失败会带回最新旧值重试。
 * 返回：无直接返回值；只改变调试统计，不拥有资源。
 */
static void inc_used_and_hiwater(struct io_tlb_mem *mem, unsigned int nslots)
{
	unsigned long old_hiwater, new_used;

	new_used = atomic_long_add_return(nslots, &mem->total_used);
	old_hiwater = atomic_long_read(&mem->used_hiwater);
	do {
		if (new_used <= old_hiwater)
			break;
	} while (!atomic_long_try_cmpxchg(&mem->used_hiwater,
					  &old_hiwater, new_used));
}

/*
 * dec_used() - 原子扣减已释放的 slot 总数
 *
 * @mem: 分配器，借用；@nslots: 已归还 slot 数。调用于释放元数据完成之后，可并发、不睡眠。
 * 返回：无直接返回值；高水位是历史峰值，故只减少 total_used 而不回退 used_hiwater。
 */
static void dec_used(struct io_tlb_mem *mem, unsigned int nslots)
{
	atomic_long_sub(nslots, &mem->total_used);
}

#else /* !CONFIG_DEBUG_FS */
/* 未启用 DEBUG_FS 时不维护精确总量与高水位，以下 stub 会被编译器消除。 */
/*
 * 未启用 DEBUG_FS 时不维护全局统计。两个 stub 保留相同调用契约：@mem 为借用分配器，@nslots
 * 为变化量；均不读写参数、不睡眠、无返回值和副作用，编译器可将调用完全消除。
 */
static void inc_used_and_hiwater(struct io_tlb_mem *mem, unsigned int nslots)
{
}

/* dec_used() 的无统计配置实现；参数和所有权契约同上，返回：无直接返回值。 */
static void dec_used(struct io_tlb_mem *mem, unsigned int nslots)
{
}
#endif /* CONFIG_DEBUG_FS */
/* 以上结束总使用量/高水位统计的启用与关闭两种实现。 */

#ifdef CONFIG_SWIOTLB_DYNAMIC
#ifdef CONFIG_DEBUG_FS
/*
 * inc_transient_used() - 增加当前设备瞬态池占用容量统计
 *
 * @mem: 分配器，借用；@nslots: 新发布瞬态 pool 的总 slot 数。原子更新、可并发且不睡眠；
 * 返回：无直接返回值。该计数反映瞬态 pool 容量而非其中 mapping 的精确使用量。
 */
static void inc_transient_used(struct io_tlb_mem *mem, unsigned int nslots)
{
	atomic_long_add(nslots, &mem->transient_nslabs);
}

/*
 * dec_transient_used() - 在瞬态 pool 摘除后扣减其容量统计
 *
 * @mem: 分配器，借用；@nslots: 被删除 pool 的总 slot 数。原子更新、不睡眠、无直接返回值；
 * 调用顺序确保 debugfs 不会长期报告已经安排 RCU 回收的瞬态容量。
 */
static void dec_transient_used(struct io_tlb_mem *mem, unsigned int nslots)
{
	atomic_long_sub(nslots, &mem->transient_nslabs);
}

#else /* !CONFIG_DEBUG_FS */
/* 未启用 DEBUG_FS 时不维护瞬态 pool 容量，以下 stub 保持调用点统一。 */
/*
 * 未启用 DEBUG_FS 时，瞬态容量统计为空操作；@mem 与 @nslots 均为借用输入，不改变任何状态，
 * 不睡眠且无直接返回值。
 */
static void inc_transient_used(struct io_tlb_mem *mem, unsigned int nslots)
{
}

/* dec_transient_used() 的无统计配置实现；参数契约同上，无直接返回值和副作用。 */
static void dec_transient_used(struct io_tlb_mem *mem, unsigned int nslots)
{
}
#endif /* CONFIG_DEBUG_FS */
/* 以上结束瞬态 pool 容量统计的启用与关闭两种实现。 */
#endif /* CONFIG_SWIOTLB_DYNAMIC */
/* 瞬态容量计数 helper 只在动态 SWIOTLB 下存在。 */

/**
 * swiotlb_search_pool_area() - search one memory area in one pool
 * @dev:	Device which maps the buffer.
 * @pool:	Memory pool to be searched.
 * @area_index:	Index of the IO TLB memory area to be searched.
 * @orig_addr:	Original (non-bounced) IO buffer address.
 * @alloc_size: Total requested size of the bounce buffer,
 *		including initial alignment padding.
 * @alloc_align_mask:	Required alignment of the allocated buffer.
 *
 * Find a suitable sequence of IO TLB entries for the request and allocate
 * a buffer from the given IO TLB memory area.
 * This function takes care of locking.
 *
 * Return: Index of the first allocated slot, or -1 on error.
 */
/*
 * swiotlb_search_pool_area() - 在一个 pool 的指定 area 中原子搜索并占用连续 slot
 *
 * @dev: 映射设备，借用；提供 segment boundary、最小对齐和物理到 DMA 地址转换。
 * @pool: 待搜索 pool，借用；映射生命周期/RCU 读侧保证对象存活。
 * @area_index: 目标 area 下标，范围 [0, pool->nareas)。
 * @orig_addr: 原始缓冲区物理地址；0 用于 restricted pool 纯分配，不要求保留原地址低位。
 * @alloc_size: 含前置对齐 padding 的总字节数，必须非零且不超过一个 segment 容量。
 * @alloc_align_mask: 分配起点/总长度的显式对齐掩码。
 *
 * 函数内部 irqsave 获取 area->lock，与同区 map/unmap 竞争并保护 area 游标、used 和 slots[]。
 * 它逐候选验证显式对齐、设备最小对齐、DMA segment boundary 与连续空闲计数；成功把所有 slot
 * 标为占用、修正前向空闲链、推进游标并返回全 pool 下标，失败返回 -1。锁内不睡眠；成功后
 * slot 所有权转给映射，直到 swiotlb_release_slots()。
 */
static int swiotlb_search_pool_area(struct device *dev, struct io_tlb_pool *pool,
		int area_index, phys_addr_t orig_addr, size_t alloc_size,
		unsigned int alloc_align_mask)
{
	struct io_tlb_area *area = pool->areas + area_index;
	unsigned long boundary_mask = dma_get_seg_boundary(dev);
	dma_addr_t tbl_dma_addr =
		phys_to_dma_unencrypted(dev, pool->start) & boundary_mask;
	unsigned long max_slots = get_max_slots(boundary_mask);
	unsigned int iotlb_align_mask = dma_get_min_align_mask(dev);
	unsigned int nslots = nr_slots(alloc_size), stride;
	unsigned int offset = swiotlb_align_offset(dev, 0, orig_addr);
	unsigned int index, slots_checked, count = 0, i;
	unsigned long flags;
	unsigned int slot_base;
	unsigned int slot_index;

	BUG_ON(!nslots);
	BUG_ON(area_index >= pool->nareas);

	/*
	 * Historically, swiotlb allocations >= PAGE_SIZE were guaranteed to be
	 * page-aligned in the absence of any other alignment requirements.
	 * 'alloc_align_mask' was later introduced to specify the alignment
	 * explicitly, however this is passed as zero for streaming mappings
	 * and so we preserve the old behaviour there in case any drivers are
	 * relying on it.
	 */
	/*
	 * 历史 streaming mapping 不传 alloc_align_mask，却承诺不小于一页的请求页对齐。只有设备也无
	 * min_align_mask 时补回 PAGE_SIZE-1，以维持旧驱动依赖；显式对齐出现时以新契约为准。
	 */
	if (!alloc_align_mask && !iotlb_align_mask && alloc_size >= PAGE_SIZE)
		alloc_align_mask = PAGE_SIZE - 1;

	/*
	 * Ensure that the allocation is at least slot-aligned and update
	 * 'iotlb_align_mask' to ignore bits that will be preserved when
	 * offsetting into the allocation.
	 */
	/*
	 * 分配起点至少 slot 对齐。显式掩码覆盖的低位由选择起点保证，故从 iotlb_align_mask 删除，
	 * 剩余位才需要与 orig_addr 一致，避免把同一约束重复应用到返回偏移。
	 */
	alloc_align_mask |= (IO_TLB_SIZE - 1);
	iotlb_align_mask &= ~alloc_align_mask;

	/*
	 * For mappings with an alignment requirement don't bother looping to
	 * unaligned slots once we found an aligned one.
	 */
	/* 对齐周期决定候选步长；一次跳到下一个可能满足的 slot，避免逐个检查必然失败的位置。 */
	stride = get_max_slots(max(alloc_align_mask, iotlb_align_mask));

	/* 阶段 1：锁内先用 used 做容量快速失败，随后从上次停止位置环形扫描本 area。 */
	spin_lock_irqsave(&area->lock, flags);
	if (unlikely(nslots > pool->area_nslabs - area->used))
		goto not_found;

	slot_base = area_index * pool->area_nslabs;
	index = area->index;

	for (slots_checked = 0; slots_checked < pool->area_nslabs; ) {
		phys_addr_t tlb_addr;

		slot_index = slot_base + index;
		tlb_addr = slot_addr(tbl_dma_addr, slot_index);

		/* 候选必须同时满足分配起点对齐，以及需保留的原地址低位模式。 */
		if ((tlb_addr & alloc_align_mask) ||
		    (orig_addr && (tlb_addr & iotlb_align_mask) !=
				  (orig_addr & iotlb_align_mask))) {
			index = wrap_area_index(pool, index + 1);
			slots_checked++;
			continue;
		}

		/* 映射不能跨设备 segment boundary；通过后，list 才能证明连续空闲容量足够。 */
		if (!iommu_is_span_boundary(slot_index, nslots,
					    nr_slots(tbl_dma_addr),
					    max_slots)) {
			if (pool->slots[slot_index].list >= nslots)
				goto found;
		}
		index = wrap_area_index(pool, index + stride);
		slots_checked += stride;
	}

not_found:
	/* 扫完整区或容量不足；未修改任何分配元数据，解锁后向上层尝试其他 area/pool。 */
	spin_unlock_irqrestore(&area->lock, flags);
	return -1;

found:
	/*
	 * If we find a slot that indicates we have 'nslots' number of
	 * contiguous buffers, we allocate the buffers from that slot onwards
	 * and set the list of free entries to '0' indicating unavailable.
	 */
	/*
	 * 找到的起点至少有 nslots 个连续空闲项。把本次范围 list 清零，并为每个 slot 保存从其对应
	 * 数据位置到分配末尾的剩余字节数，供子范围同步和释放边界检查。
	 */
	for (i = slot_index; i < slot_index + nslots; i++) {
		pool->slots[i].list = 0;
		pool->slots[i].alloc_size = alloc_size - (offset +
				((i - slot_index) << IO_TLB_SHIFT));
	}
	for (i = slot_index - 1;
	     io_tlb_offset(i) != IO_TLB_SEGSIZE - 1 &&
	     pool->slots[i].list; i--)
		pool->slots[i].list = ++count;

	/*
	 * Update the indices to avoid searching in the next round.
	 */
	/* 推进循环游标并增加 area 已用量；这些状态与 slots[] 同在锁内提交，外部不会看见半次分配。 */
	area->index = wrap_area_index(pool, index + nslots);
	area->used += nslots;
	spin_unlock_irqrestore(&area->lock, flags);

	inc_used_and_hiwater(dev->dma_io_tlb_mem, nslots);
	return slot_index;
}

#ifdef CONFIG_SWIOTLB_DYNAMIC

/**
 * swiotlb_search_area() - search one memory area in all pools
 * @dev:	Device which maps the buffer.
 * @start_cpu:	Start CPU number.
 * @cpu_offset:	Offset from @start_cpu.
 * @orig_addr:	Original (non-bounced) IO buffer address.
 * @alloc_size: Total requested size of the bounce buffer,
 *		including initial alignment padding.
 * @alloc_align_mask:	Required alignment of the allocated buffer.
 * @retpool:	Used memory pool, updated on return.
 *
 * Search one memory area in all pools for a sequence of slots that match the
 * allocation constraints.
 *
 * Return: Index of the first allocated slot, or -1 on error.
 */
/*
 * swiotlb_search_area() - 以同一 CPU 偏移在全部公共 pool 中搜索一个 area
 *
 * @dev: 映射设备，借用；@start_cpu: 初始 CPU 编号；@cpu_offset: 本轮轮转偏移。
 * @orig_addr/@alloc_size/@alloc_align_mask: 原始地址、含 padding 字节数和分配对齐约束。
 * @retpool: 输出参数；成功写入持有 slot 的 pool 借用指针，失败内容未定义，所有权不转移。
 *
 * RCU 读锁保证遍历中的动态 pool 存活；每个候选 pool 用 2 的幂 nareas 把 CPU/偏移映射到 area，
 * 再由下层自行加锁。返回 slot 下标或 -1。RCU 读侧不可睡眠，成功映射本身让 pool 维持到 unmap。
 */
static int swiotlb_search_area(struct device *dev, int start_cpu,
		int cpu_offset, phys_addr_t orig_addr, size_t alloc_size,
		unsigned int alloc_align_mask, struct io_tlb_pool **retpool)
{
	struct io_tlb_mem *mem = dev->dma_io_tlb_mem;
	struct io_tlb_pool *pool;
	int area_index;
	int index = -1;

	rcu_read_lock();
	list_for_each_entry_rcu(pool, &mem->pools, node) {
		if (cpu_offset >= pool->nareas)
			continue;
		area_index = (start_cpu + cpu_offset) & (pool->nareas - 1);
		index = swiotlb_search_pool_area(dev, pool, area_index,
						 orig_addr, alloc_size,
						 alloc_align_mask);
		if (index >= 0) {
			*retpool = pool;
			break;
		}
	}
	rcu_read_unlock();
	return index;
}

/**
 * swiotlb_find_slots() - search for slots in the whole swiotlb
 * @dev:	Device which maps the buffer.
 * @orig_addr:	Original (non-bounced) IO buffer address.
 * @alloc_size: Total requested size of the bounce buffer,
 *		including initial alignment padding.
 * @alloc_align_mask:	Required alignment of the allocated buffer.
 * @retpool:	Used memory pool, updated on return.
 *
 * Search through the whole software IO TLB to find a sequence of slots that
 * match the allocation constraints.
 *
 * Return: Index of the first allocated slot, or -1 on error.
 */
/*
 * swiotlb_find_slots() - 动态配置下跨全部 area/pool 分配连续 slot，必要时创建扩展池
 *
 * @dev: 映射设备，借用且已完成 swiotlb_dev_init()。
 * @orig_addr: 原始物理地址；0 表示纯页分配。
 * @alloc_size: 含对齐 padding 的总字节数，最大一个 IO_TLB segment。
 * @alloc_align_mask: 分配对齐掩码。
 * @retpool: 成功输出实际 pool 借用指针；失败不提供有效对象。
 *
 * 快路径从当前 CPU 对应 area 开始轮转所有公共 pool。耗尽且允许增长时，先异步请求常驻池，再
 * 用 GFP_NOWAIT 同步创建恰好容纳本请求的设备瞬态池；发布后设置 dma_uses_io_tlb 并执行与
 * swiotlb_find_pool() 配对的全屏障。成功返回 slot 下标，失败 -1。函数可能在调用上下文原子执行，
 * 同步扩池绝不睡眠；成功后 slot/pool 生命周期持续到 unmap。
 */
static int swiotlb_find_slots(struct device *dev, phys_addr_t orig_addr,
		size_t alloc_size, unsigned int alloc_align_mask,
		struct io_tlb_pool **retpool)
{
	struct io_tlb_mem *mem = dev->dma_io_tlb_mem;
	struct io_tlb_pool *pool;
	unsigned long nslabs;
	unsigned long flags;
	u64 phys_limit;
	int cpu, i;
	int index;

	/* 单次分配不能跨 free-list segment；超限无需扫描。 */
	if (alloc_size > IO_TLB_SEGSIZE * IO_TLB_SIZE)
		return -1;

	/* 阶段 1：以当前 CPU 为起点轮转 area，分散不同 CPU 的锁竞争。 */
	cpu = raw_smp_processor_id();
	for (i = 0; i < default_nareas; ++i) {
		index = swiotlb_search_area(dev, cpu, i, orig_addr, alloc_size,
					    alloc_align_mask, &pool);
		if (index >= 0)
			goto found;
	}

	/* 阶段 2：公共池耗尽。不能增长则失败；否则异步补常驻容量，同时尝试当前请求的瞬态池。 */
	if (!mem->can_grow)
		return -1;

	schedule_work(&mem->dyn_alloc);

	nslabs = nr_slots(alloc_size);
	phys_limit = min_not_zero(*dev->dma_mask, dev->bus_dma_limit);
	pool = swiotlb_alloc_pool(dev, nslabs, nslabs, 1, phys_limit,
				  GFP_NOWAIT);
	if (!pool)
		return -1;

	index = swiotlb_search_pool_area(dev, pool, 0, orig_addr,
					 alloc_size, alloc_align_mask);
	if (index < 0) {
		swiotlb_dyn_free(&pool->rcu);
		return -1;
	}

	/*
	 * 阶段 3：首个 mapping 已在新 pool 中占位，标记瞬态并在设备锁下 RCU 发布。只有此设备的
	 * unmap 查找需要看到它；最后一次（也是唯一一次）映射释放时会摘链并销毁整池。
	 */
	pool->transient = true;
	spin_lock_irqsave(&dev->dma_io_tlb_lock, flags);
	list_add_rcu(&pool->node, &dev->dma_io_tlb_pools);
	spin_unlock_irqrestore(&dev->dma_io_tlb_lock, flags);
	inc_transient_used(mem, pool->nslabs);

found:
	WRITE_ONCE(dev->dma_uses_io_tlb, true);

	/*
	 * The general barrier orders reads and writes against a presumed store
	 * of the SWIOTLB buffer address by a device driver (to a driver private
	 * data structure). It serves two purposes.
	 *
	 * First, the store to dev->dma_uses_io_tlb must be ordered before the
	 * presumed store. This guarantees that the returned buffer address
	 * cannot be passed to another CPU before updating dev->dma_uses_io_tlb.
	 *
	 * Second, the load from mem->pools must be ordered before the same
	 * presumed store. This guarantees that the returned buffer address
	 * cannot be observed by another CPU before an update of the RCU list
	 * that was made by swiotlb_dyn_alloc() on a third CPU (cf. multicopy
	 * atomicity).
	 *
	 * See also the comment in swiotlb_find_pool().
	 */
	/*
	 * 该全屏障把 dma_uses_io_tlb 写和本次 pools 链表读取排在“驱动保存返回地址”之前。另一 CPU
	 * 从驱动私有结构取出地址后，在 swiotlb_find_pool() 执行 smp_rmb()，于是既不会先看到地址却
	 * 仍看到 false，也不会错过第三个 CPU 已 RCU 发布的公共池；这是跨 CPU 查池的发布协议。
	 */
	smp_mb();

	*retpool = pool;
	return index;
}

#else  /* !CONFIG_SWIOTLB_DYNAMIC */

/* 未启用动态池时只轮转默认 pool 的各 area，不进行 RCU 遍历或扩池。 */
/*
 * swiotlb_find_slots() - 静态配置下轮转默认 pool 的全部 area 分配 slot
 *
 * @dev: 映射设备，借用；@orig_addr: 原始物理地址；@alloc_size: 含 padding 的总字节数；
 * @alloc_align_mask: 分配对齐掩码；@retpool: 无论成败均写为默认 pool 的借用指针。
 *
 * 从当前 CPU 映射到的 area 开始环形尝试，每个 area 的下层 helper 自行加锁。成功返回全 pool
 * slot 下标，全部失败返回 -1；静态配置没有扩池、RCU 或瞬态 pool。函数不睡眠，成功 slot 由
 * 调用者持有到 release。
 */
static int swiotlb_find_slots(struct device *dev, phys_addr_t orig_addr,
		size_t alloc_size, unsigned int alloc_align_mask,
		struct io_tlb_pool **retpool)
{
	struct io_tlb_pool *pool;
	int start, i;
	int index;

	*retpool = pool = &dev->dma_io_tlb_mem->defpool;
	i = start = raw_smp_processor_id() & (pool->nareas - 1);
	do {
		index = swiotlb_search_pool_area(dev, pool, i, orig_addr,
						 alloc_size, alloc_align_mask);
		if (index >= 0)
			return index;
		if (++i >= pool->nareas)
			i = 0;
	} while (i != start);
	return -1;
}

#endif /* CONFIG_SWIOTLB_DYNAMIC */
/* 以上结束动态扩池搜索与静态默认池轮转搜索两种实现。 */

#ifdef CONFIG_DEBUG_FS

/**
 * mem_used() - get number of used slots in an allocator
 * @mem:	Software IO TLB allocator.
 *
 * The result is accurate in this version of the function, because an atomic
 * counter is available if CONFIG_DEBUG_FS is set.
 *
 * Return: Number of used slots.
 */
/*
 * mem_used() - DEBUG_FS 配置下读取精确的分配器已用 slot 总数
 *
 * @mem: 分配器，借用。atomic_long_read() 返回与并发增减一致的单值快照，单位为 slot；不锁 area、
 * 不睡眠且无副作用。该值用于诊断/满池告警，不参与正确性决策。
 */
static unsigned long mem_used(struct io_tlb_mem *mem)
{
	return atomic_long_read(&mem->total_used);
}

#else /* !CONFIG_DEBUG_FS */

/* 未启用 DEBUG_FS 时没有 total_used 原子量，只能无锁近似汇总 area->used。 */
/**
 * mem_pool_used() - get number of used slots in a memory pool
 * @pool:	Software IO TLB memory pool.
 *
 * The result is not accurate, see mem_used().
 *
 * Return: Approximate number of used slots.
 */
/*
 * mem_pool_used() - 无全局统计时近似汇总一个 pool 的 area 已用量
 *
 * @pool: 被观测 pool，借用。逐项无锁读取 area->used 并相加，返回单位为 slot；并发 map/unmap
 * 可使各项来自不同时刻，所以只适合日志。函数不取得锁、不睡眠、不改变状态。
 */
static unsigned long mem_pool_used(struct io_tlb_pool *pool)
{
	int i;
	unsigned long used = 0;

	for (i = 0; i < pool->nareas; i++)
		used += pool->areas[i].used;
	return used;
}

/**
 * mem_used() - get number of used slots in an allocator
 * @mem:	Software IO TLB allocator.
 *
 * The result is not accurate, because there is no locking of individual
 * areas.
 *
 * Return: Approximate number of used slots.
 */
/*
 * mem_used() - 非 DEBUG_FS 配置下近似汇总分配器全部 pool 的已用 slot
 *
 * @mem: 分配器，借用。动态配置在 RCU 读侧遍历公共 pools 并累加各 area；静态配置只读 defpool。
 * 返回近似 slot 数，无副作用、不睡眠。RCU 只保证 pool 生命周期，不会让跨 area 数值成为原子快照。
 */
static unsigned long mem_used(struct io_tlb_mem *mem)
{
#ifdef CONFIG_SWIOTLB_DYNAMIC
	struct io_tlb_pool *pool;
	unsigned long used = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(pool, &mem->pools, node)
		used += mem_pool_used(pool);
	rcu_read_unlock();

	return used;
#else
	return mem_pool_used(&mem->defpool);
#endif
}

#endif /* CONFIG_DEBUG_FS */
/* 以上结束精确原子统计与近似无锁汇总两种实现。 */

/**
 * swiotlb_tbl_map_single() - bounce buffer map a single contiguous physical area
 * @dev:		Device which maps the buffer.
 * @orig_addr:		Original (non-bounced) physical IO buffer address
 * @mapping_size:	Requested size of the actual bounce buffer, excluding
 *			any pre- or post-padding for alignment
 * @alloc_align_mask:	Required start and end alignment of the allocated buffer
 * @dir:		DMA direction
 * @attrs:		Optional DMA attributes for the map operation
 *
 * Find and allocate a suitable sequence of IO TLB slots for the request.
 * The allocated space starts at an alignment specified by alloc_align_mask,
 * and the size of the allocated space is rounded up so that the total amount
 * of allocated space is a multiple of (alloc_align_mask + 1). If
 * alloc_align_mask is zero, the allocated space may be at any alignment and
 * the size is not rounded up.
 *
 * The returned address is within the allocated space and matches the bits
 * of orig_addr that are specified in the DMA min_align_mask for the device. As
 * such, this returned address may be offset from the beginning of the allocated
 * space. The bounce buffer space starting at the returned address for
 * mapping_size bytes is initialized to the contents of the original IO buffer
 * area. Any pre-padding (due to an offset) and any post-padding (due to
 * rounding-up the size) is not initialized.
 */
/*
 * swiotlb_tbl_map_single() - 为连续物理缓冲区分配、登记并预填 bounce mapping
 *
 * @dev: DMA 设备，借用且已初始化 SWIOTLB 字段。
 * @orig_addr: 原始 CPU 缓冲区起始物理地址；映射期间由调用者保持有效。
 * @mapping_size: 实际可见数据字节数，不含对齐 padding，必须非零且不超过最大映射限制。
 * @alloc_align_mask: 已分配空间首尾对齐掩码，0 表示仅遵守设备最小对齐。
 * @dir: DMA_TO_DEVICE/FROM_DEVICE/BIDIRECTIONAL，决定后续同步语义。
 * @attrs: DMA_ATTR_NO_WARN 等映射属性；本层只消费相关位，不拥有该值。
 *
 * 函数先计算前置偏移和对齐后的总分配量，再跨池领取 slot；成功后登记 pad_slots、每个数据 slot
 * 的 orig_addr，并把原内容复制到 bounce buffer 以保持透明的部分写语义。成功返回 bounce 物理
 * 地址（可能位于首个已分配 slot 内部）；失败返回 DMA_MAPPING_ERROR，且不留下 slot。函数可能在
 * 原子映射上下文调用，搜索路径不睡眠；成功后所有权交给 DMA mapping，unmap 负责归还。
 */
phys_addr_t swiotlb_tbl_map_single(struct device *dev, phys_addr_t orig_addr,
		size_t mapping_size, unsigned int alloc_align_mask,
		enum dma_data_direction dir, unsigned long attrs)
{
	struct io_tlb_mem *mem = dev->dma_io_tlb_mem;
	unsigned int offset;
	struct io_tlb_pool *pool;
	unsigned int i;
	size_t size;
	int index;
	phys_addr_t tlb_addr;
	unsigned short pad_slots;

	/* 阶段 1：未建立分配器时无法提供 fallback；失败不取得任何 slot。 */
	if (!mem || !mem->nslabs) {
		dev_warn_ratelimited(dev,
			"Can not allocate SWIOTLB buffer earlier and can't now provide you with the DMA bounce buffer");
		return (phys_addr_t)DMA_MAPPING_ERROR;
	}

	if (cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		pr_warn_once("Memory encryption is active and system is using DMA bounce buffers\n");

	/*
	 * The default swiotlb memory pool is allocated with PAGE_SIZE
	 * alignment. If a mapping is requested with larger alignment,
	 * the mapping may be unable to use the initial slot(s) in all
	 * sets of IO_TLB_SEGSIZE slots. In such case, a mapping request
	 * of or near the maximum mapping size would always fail.
	 */
	/*
	 * 默认数据区只有 PAGE_SIZE 对齐；若调用者要求更大对齐，每个 segment 前部可能永久不可选，
	 * 接近最大长度的请求便没有足够连续 slot。这里只告警，实际搜索仍按请求约束执行。
	 */
	dev_WARN_ONCE(dev, alloc_align_mask > ~PAGE_MASK,
		"Alloc alignment may prevent fulfilling requests with max mapping_size\n");

	/* 阶段 2：把字节级低位保留转成前缀 offset，并将“数据+前缀”向分配对齐整体取整。 */
	offset = swiotlb_align_offset(dev, alloc_align_mask, orig_addr);
	size = ALIGN(mapping_size + offset, alloc_align_mask + 1);
	index = swiotlb_find_slots(dev, orig_addr, size, alloc_align_mask, &pool);
	if (index == -1) {
		if (!(attrs & DMA_ATTR_NO_WARN))
			dev_warn_ratelimited(dev,
	"swiotlb buffer is full (sz: %zd bytes), total %lu (slots), used %lu (slots)\n",
				 size, mem->nslabs, mem_used(mem));
		return (phys_addr_t)DMA_MAPPING_ERROR;
	}

	/*
	 * If dma_skip_sync was set, reset it on first SWIOTLB buffer
	 * mapping to always sync SWIOTLB buffers.
	 */
	/*
	 * 设备曾可能因直接一致映射设置 dma_skip_sync；首次进入 SWIOTLB 后必须清除，因为即使硬件
	 * coherent，原缓冲区与 bounce buffer 仍是两份内存，不能省略软件复制。
	 */
	dma_reset_need_sync(dev);

	/*
	 * Save away the mapping from the original address to the DMA address.
	 * This is needed when we sync the memory.  Then we sync the buffer if
	 * needed.
	 */
	/*
	 * 阶段 3：把跨 slot 的前缀记为 pad_slots，再为每个承载数据的 slot 登记对应原物理地址。
	 * 这些字段在 slot 归还前保持稳定，使任意子范围 sync 能反查复制源；tlb_addr 是对外发布点。
	 */
	pad_slots = offset >> IO_TLB_SHIFT;
	offset &= (IO_TLB_SIZE - 1);
	index += pad_slots;
	pool->slots[index].pad_slots = pad_slots;
	for (i = 0; i < (nr_slots(size) - pad_slots); i++)
		pool->slots[index + i].orig_addr = slot_addr(orig_addr, i);
	tlb_addr = slot_addr(pool->start, index) + offset;
	/*
	 * When the device is writing memory, i.e. dir == DMA_FROM_DEVICE, copy
	 * the original buffer to the TLB buffer before initiating DMA in order
	 * to preserve the original's data if the device does a partial write,
	 * i.e. if the device doesn't overwrite the entire buffer.  Preserving
	 * the original data, even if it's garbage, is necessary to match
	 * hardware behavior.  Use of swiotlb is supposed to be transparent,
	 * i.e. swiotlb must not corrupt memory by clobbering unwritten bytes.
	 */
	/*
	 * 即便方向是 FROM_DEVICE，也先把原内容预填到 bounce buffer：设备可能只写部分字节，未写部分
	 * 应像直接 DMA 一样保留原值。为统一透明语义，这里对所有方向执行一次 TO_DEVICE 复制。
	 */
	swiotlb_bounce(dev, tlb_addr, mapping_size, DMA_TO_DEVICE, pool);
	return tlb_addr;
}

/*
 * swiotlb_release_slots() - 归还一个非瞬态 mapping 的全部 slot 并重建连续空闲计数
 *
 * @dev: 原映射设备，借用，用于重算字节偏移与调试统计。
 * @tlb_addr: 对外返回的 bounce 物理地址，可能位于首个数据 slot 内部。
 * @mem: 包含该地址的 pool，借用且由映射生命周期保证存活。
 *
 * 函数用 offset 与 pad_slots 回退到真实分配起点，从 alloc_size 恢复 slot 数；随后 irqsave 获取
 * 所属 area 锁，先与后继空闲段合并，再反向写回本段并向前更新前驱计数。返回：无直接返回值；
 * 成功后 slot 所有权归 pool，可被新映射复用。锁内不睡眠，最后在锁外更新全局调试统计。
 */
static void swiotlb_release_slots(struct device *dev, phys_addr_t tlb_addr,
				  struct io_tlb_pool *mem)
{
	unsigned long flags;
	unsigned int offset = swiotlb_align_offset(dev, 0, tlb_addr);
	int index, nslots, aindex;
	struct io_tlb_area *area;
	int count, i;

	index = (tlb_addr - offset - mem->start) >> IO_TLB_SHIFT;
	index -= mem->slots[index].pad_slots;
	nslots = nr_slots(mem->slots[index].alloc_size + offset);
	aindex = index / mem->area_nslabs;
	area = &mem->areas[aindex];

	/*
	 * Return the buffer to the free list by setting the corresponding
	 * entries to indicate the number of contiguous entries available.
	 * While returning the entries to the free list, we merge the entries
	 * with slots below and above the pool being returned.
	 */
	/*
	 * list[i] 表示从 i 起向后的连续空闲数。释放时必须同时修复本范围、同 segment 的后继和前驱；
	 * area 锁防止并发搜索观察到只完成一半的 free-list 状态。
	 */
	BUG_ON(aindex >= mem->nareas);

	spin_lock_irqsave(&area->lock, flags);
	if (index + nslots < ALIGN(index + 1, IO_TLB_SEGSIZE))
		count = mem->slots[index + nslots].list;
	else
		count = 0;

	/*
	 * Step 1: return the slots to the free list, merging the slots with
	 * superceeding slots
	 */
	/* 第一步从后向前填充，起始 count 已包含同 segment 中紧邻的后继空闲段。 */
	for (i = index + nslots - 1; i >= index; i--) {
		mem->slots[i].list = ++count;
		mem->slots[i].orig_addr = INVALID_PHYS_ADDR;
		mem->slots[i].alloc_size = 0;
		mem->slots[i].pad_slots = 0;
	}

	/*
	 * Step 2: merge the returned slots with the preceding slots, if
	 * available (non zero)
	 */
	/* 第二步继续向前扩展已有空闲前缀，遇到 segment 边界或占用 slot 即停止。 */
	for (i = index - 1;
	     io_tlb_offset(i) != IO_TLB_SEGSIZE - 1 && mem->slots[i].list;
	     i--)
		mem->slots[i].list = ++count;
	area->used -= nslots;
	spin_unlock_irqrestore(&area->lock, flags);

	dec_used(dev->dma_io_tlb_mem, nslots);
}

#ifdef CONFIG_SWIOTLB_DYNAMIC

/**
 * swiotlb_del_transient() - delete a transient memory pool
 * @dev:	Device which mapped the buffer.
 * @tlb_addr:	Physical address within a bounce buffer.
 * @pool:       Pointer to the transient memory pool to be checked and deleted.
 *
 * Check whether the address belongs to a transient SWIOTLB memory pool.
 * If yes, then delete the pool.
 *
 * Return: %true if @tlb_addr belonged to a transient pool that was released.
 */
/*
 * swiotlb_del_transient() - 若 mapping 属于设备瞬态 pool，则整池摘除并安排回收
 *
 * @dev: 映射设备，借用；@tlb_addr: 正在解除的 bounce 地址，仅用于接口语义，本实现无需重查；
 * @pool: 已由查找路径确认包含地址的借用 pool。
 *
 * 非瞬态返回 false，调用者继续按 slot 释放；瞬态池专为单次请求创建，故无需重建 free-list，
 * 直接扣减统计、RCU 摘链并返回 true。成功后 pool 所有权转给 RCU 回调，调用者不可再访问。
 * 函数不睡眠。
 */
static bool swiotlb_del_transient(struct device *dev, phys_addr_t tlb_addr,
		struct io_tlb_pool *pool)
{
	if (!pool->transient)
		return false;

	dec_used(dev->dma_io_tlb_mem, pool->nslabs);
	swiotlb_del_pool(dev, pool);
	dec_transient_used(dev->dma_io_tlb_mem, pool->nslabs);
	return true;
}

#else  /* !CONFIG_SWIOTLB_DYNAMIC */

/* 静态配置不存在瞬态 pool，兼容 helper 恒返回 false。 */
/*
 * swiotlb_del_transient() - 静态配置的恒 false 兼容实现
 *
 * @dev/@tlb_addr/@pool 均为借用输入且不读取；静态配置不存在瞬态池。返回 false，要求调用者总是
 * 进入 swiotlb_release_slots()；无副作用、不睡眠。
 */
static inline bool swiotlb_del_transient(struct device *dev,
		phys_addr_t tlb_addr, struct io_tlb_pool *pool)
{
	return false;
}

#endif	/* CONFIG_SWIOTLB_DYNAMIC */
/* 以上结束瞬态 pool 删除与静态恒 false 兼容实现。 */

/*
 * tlb_addr is the physical address of the bounce buffer to unmap.
 */
/*
 * __swiotlb_tbl_unmap_single() - 完成必要写回并销毁一个已知的 SWIOTLB mapping
 *
 * @dev: 映射设备，借用；@tlb_addr: bounce buffer 物理地址；@mapping_size: 对外数据字节数；
 * @dir: 原 DMA 方向；@attrs: DMA 属性；@pool: 已查得且包含地址的借用 pool。
 *
 * 调用者已确认地址属于 SWIOTLB。除非 DMA_ATTR_SKIP_CPU_SYNC，FROM_DEVICE/BIDIRECTIONAL 先把
 * 设备结果复制回原缓冲区；随后瞬态池整池摘除，普通池则归还 slot。返回：无直接返回值；成功后
 * mapping 失效，调用者不得继续使用 tlb_addr/pool。函数不睡眠，释放与并发查找由 area 锁/RCU
 * 协调。
 */
void __swiotlb_tbl_unmap_single(struct device *dev, phys_addr_t tlb_addr,
		size_t mapping_size, enum dma_data_direction dir,
		unsigned long attrs, struct io_tlb_pool *pool)
{
	/*
	 * First, sync the memory before unmapping the entry
	 */
	/*
	 * 必须先复制再释放：一旦 slot 重新进入 free-list，另一映射即可覆盖 bounce 数据。TO_DEVICE
	 * 没有设备结果要带回；SKIP_CPU_SYNC 表示更高层已完成或明确放弃本次 CPU 同步。
	 */
	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) &&
	    (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL))
		swiotlb_bounce(dev, tlb_addr, mapping_size,
						DMA_FROM_DEVICE, pool);

	if (swiotlb_del_transient(dev, tlb_addr, pool))
		return;
	swiotlb_release_slots(dev, tlb_addr, pool);
}

/*
 * __swiotlb_sync_single_for_device() - 把 CPU 更新同步到仍有效的 bounce mapping
 *
 * @dev/@pool: 借用的设备和所属 pool；@tlb_addr: 同步起点；@size: 字节数；@dir: 映射方向。
 * TO_DEVICE/BIDIRECTIONAL 从原缓冲区复制到 bounce；FROM_DEVICE 无 CPU 数据要发送，仅验证方向。
 * 返回：无直接返回值；mapping 和 slot 所有权不变。调用者保证设备当前可接收 CPU 交还的所有权。
 */
void __swiotlb_sync_single_for_device(struct device *dev, phys_addr_t tlb_addr,
		size_t size, enum dma_data_direction dir,
		struct io_tlb_pool *pool)
{
	if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL)
		swiotlb_bounce(dev, tlb_addr, size, DMA_TO_DEVICE, pool);
	else
		BUG_ON(dir != DMA_FROM_DEVICE);
}

/*
 * __swiotlb_sync_single_for_cpu() - 把设备结果同步回仍有效的原始 CPU 缓冲区
 *
 * @dev/@pool: 借用设备和 pool；@tlb_addr: 同步起点；@size: 字节数；@dir: 映射方向。
 * FROM_DEVICE/BIDIRECTIONAL 从 bounce 写回原内存；TO_DEVICE 无设备写入，仅验证方向。返回：无
 * 直接返回值；mapping 继续有效，调用者完成 CPU 访问后可再次 sync_for_device。
 */
void __swiotlb_sync_single_for_cpu(struct device *dev, phys_addr_t tlb_addr,
		size_t size, enum dma_data_direction dir,
		struct io_tlb_pool *pool)
{
	if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL)
		swiotlb_bounce(dev, tlb_addr, size, DMA_FROM_DEVICE, pool);
	else
		BUG_ON(dir != DMA_TO_DEVICE);
}

/*
 * Create a swiotlb mapping for the buffer at @paddr, and in case of DMAing
 * to the device copy the data into it as well.
 */
/*
 * swiotlb_map() - 建立 streaming bounce mapping 并返回设备可使用的 DMA 地址
 *
 * @dev: DMA 设备，借用；@paddr: 原始物理地址；@size: 数据字节数；@dir: DMA 方向；
 * @attrs: streaming mapping 属性。
 *
 * 先记录 tracepoint，再由 swiotlb_tbl_map_single() 分配并预填 bounce；随后把其物理地址转换为
 * 未加密 DMA 地址并再次验证设备 mask/bus limit。地址不可达时用 SKIP_CPU_SYNC 回滚刚建 mapping，
 * 因为设备尚未运行，无数据需写回。非一致设备最后执行架构 device-side cache 同步。成功返回
 * dma_addr_t，失败 DMA_MAPPING_ERROR；成功 mapping 由 unmap/sync 路径负责后续生命周期。
 */
dma_addr_t swiotlb_map(struct device *dev, phys_addr_t paddr, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	phys_addr_t swiotlb_addr;
	dma_addr_t dma_addr;

	/* 阶段 1：trace 记录原 DMA 地址和长度，随后分配并登记 bounce 物理地址。 */
	trace_swiotlb_bounced(dev, phys_to_dma(dev, paddr), size);

	swiotlb_addr = swiotlb_tbl_map_single(dev, paddr, size, 0, dir, attrs);
	if (swiotlb_addr == (phys_addr_t)DMA_MAPPING_ERROR)
		return DMA_MAPPING_ERROR;

	/* Ensure that the address returned is DMA'ble */
	/*
	 * 池的物理上限通常已保证可达，但设备 mask 或 bus limit 仍可能更严格；对最终转换后的 DMA
	 * 地址做权威校验。失败回滚不复制 FROM_DEVICE 数据，因为 DMA 尚未启动。
	 */
	dma_addr = phys_to_dma_unencrypted(dev, swiotlb_addr);
	if (unlikely(!dma_capable(dev, dma_addr, size, true))) {
		__swiotlb_tbl_unmap_single(dev, swiotlb_addr, size, dir,
			attrs | DMA_ATTR_SKIP_CPU_SYNC,
			swiotlb_find_pool(dev, swiotlb_addr));
		dev_WARN_ONCE(dev, 1,
			"swiotlb addr %pad+%zu overflow (mask %llx, bus limit %llx).\n",
			&dma_addr, size, *dev->dma_mask, dev->bus_dma_limit);
		return DMA_MAPPING_ERROR;
	}

	/* 阶段 3：非一致设备在收到地址前必须看到清理后的 cache；flush 完成映射发布。 */
	if (!dev_is_dma_coherent(dev) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC)) {
		arch_sync_dma_for_device(swiotlb_addr, size, dir);
		arch_sync_dma_flush();
	}
	return dma_addr;
}

/*
 * swiotlb_max_mapping_size() - 计算设备在单个 SWIOTLB segment 中可映射的最大字节数
 *
 * @dev: 目标设备，借用；函数仅读取 min_align_mask。基础上限为 IO_TLB_SEGSIZE 个 slot；若最小
 * 对齐使搜索跳过 segment 前部，则扣除向 slot 对齐的不可用字节。返回 size_t 字节数，无副作用、
 * 不睡眠；DMA 层用它拆分或拒绝过大的 mapping。
 */
size_t swiotlb_max_mapping_size(struct device *dev)
{
	int min_align_mask = dma_get_min_align_mask(dev);
	int min_align = 0;

	/*
	 * swiotlb_find_slots() skips slots according to
	 * min align mask. This affects max mapping size.
	 * Take it into acount here.
	 */
	/*
	 * 搜索会因 min_align_mask 跳过不匹配的 slot，导致一个 segment 的有效连续后缀变短；这里扣掉
	 * 同样的 slot 对齐前缀，使对外上限与实际搜索能力一致。
	 */
	if (min_align_mask)
		min_align = roundup(min_align_mask, IO_TLB_SIZE);

	return ((size_t)IO_TLB_SIZE) * IO_TLB_SEGSIZE - min_align;
}

/**
 * is_swiotlb_allocated() - check if the default software IO TLB is initialized
 */
/*
 * is_swiotlb_allocated() - 查询全局默认分配器是否已经拥有任何 slot
 *
 * 入参：无。返回 io_tlb_default_mem.nslabs 的布尔解释；true 表示至少一个 pool 已发布，false 表示
 * 未初始化或已退出。只读查询、不睡眠、无所有权变化；调用者不能据此获得 pool 引用。
 */
bool is_swiotlb_allocated(void)
{
	return io_tlb_default_mem.nslabs;
}

/*
 * is_swiotlb_active() - 查询指定设备当前关联的分配器是否可用
 *
 * @dev: 设备，借用且生命周期稳定。返回 true 仅当 dma_io_tlb_mem 非 NULL 且总 nslabs 非零；
 * false 表示该设备不能走 SWIOTLB。函数不锁、不睡眠、不取得引用，通常在设备设置或 DMA 决策时用。
 */
bool is_swiotlb_active(struct device *dev)
{
	struct io_tlb_mem *mem = dev->dma_io_tlb_mem;

	return mem && mem->nslabs;
}

/**
 * default_swiotlb_base() - get the base address of the default SWIOTLB
 *
 * Get the lowest physical address used by the default software IO TLB pool.
 */
/*
 * default_swiotlb_base() - 返回默认固定池的最低物理地址并冻结动态增长
 *
 * 入参：无。动态配置把 can_grow 清零，因为调用者需要一个稳定、可表达为单一范围的默认池视图；
 * 随后返回 defpool.start。该函数不分配、不睡眠，但有停止后台扩池的可观察副作用；已有动态 pool
 * 不会在此释放。调用者不能把返回地址当作内存所有权。
 */
phys_addr_t default_swiotlb_base(void)
{
#ifdef CONFIG_SWIOTLB_DYNAMIC
	io_tlb_default_mem.can_grow = false;
#endif
	return io_tlb_default_mem.defpool.start;
}

/**
 * default_swiotlb_limit() - get the address limit of the default SWIOTLB
 *
 * Get the highest physical address used by the default software IO TLB pool.
 */
/*
 * default_swiotlb_limit() - 返回默认 SWIOTLB 可使用的最高物理地址（含）
 *
 * 入参：无。动态配置返回为后续 pool 预设的 phys_limit，静态配置返回 defpool.end - 1；两者语义
 * 都是闭区间上限，但动态值可能高于当前固定池末端。只读、不睡眠、无所有权变化。
 */
phys_addr_t default_swiotlb_limit(void)
{
#ifdef CONFIG_SWIOTLB_DYNAMIC
	return io_tlb_default_mem.phys_limit;
#else
	return io_tlb_default_mem.defpool.end - 1;
#endif
}

#ifdef CONFIG_DEBUG_FS
#ifdef CONFIG_SWIOTLB_DYNAMIC
/*
 * mem_transient_used() - 读取当前已发布瞬态 pool 的总 slot 容量
 *
 * @mem: 分配器，借用。返回 transient_nslabs 原子快照，单位为 slot；不加锁、不睡眠、无副作用，
 * 仅在 DYNAMIC 与 DEBUG_FS 同时启用时供诊断读取。
 */
static unsigned long mem_transient_used(struct io_tlb_mem *mem)
{
	return atomic_long_read(&mem->transient_nslabs);
}

/*
 * io_tlb_transient_used_get() - debugfs 读取瞬态 pool 容量
 *
 * @data: debugfs 注册时保存的 io_tlb_mem 借用指针；@val: 输出 u64 指针，成功写入 slot 数。
 * 返回 0；不转移所有权、不睡眠。读取是原子快照，但值可在返回后立即因 map/unmap 改变。
 */
static int io_tlb_transient_used_get(void *data, u64 *val)
{
	struct io_tlb_mem *mem = data;

	*val = mem_transient_used(mem);
	return 0;
}

/*
 * fops_io_tlb_transient_used 是宏生成的只读 file_operations：open/read/release 由 debugfs 通用层
 * 提供，读值回调落到 io_tlb_transient_used_get()；对象为静态只读表，无独立生命周期。
 */
DEFINE_DEBUGFS_ATTRIBUTE(fops_io_tlb_transient_used, io_tlb_transient_used_get,
			 NULL, "%llu\n");
#endif /* CONFIG_SWIOTLB_DYNAMIC */
/* 以上结束瞬态容量 debugfs 属性的动态配置分支。 */

/*
 * io_tlb_used_get() - debugfs 读取当前已用 slot 数
 *
 * @data: io_tlb_mem 借用指针；@val: 输出当前精确 total_used。返回 0，无所有权变化、不睡眠；
 * debugfs 文件只读，因此没有对应 set 回调。
 */
static int io_tlb_used_get(void *data, u64 *val)
{
	struct io_tlb_mem *mem = data;

	*val = mem_used(mem);
	return 0;
}

/*
 * io_tlb_hiwater_get() - debugfs 读取历史已用 slot 峰值
 *
 * @data: io_tlb_mem 借用指针；@val: 输出 used_hiwater 原子快照。返回 0，不修改统计、不睡眠。
 */
static int io_tlb_hiwater_get(void *data, u64 *val)
{
	struct io_tlb_mem *mem = data;

	*val = atomic_long_read(&mem->used_hiwater);
	return 0;
}

/*
 * io_tlb_hiwater_set() - 允许用户通过 debugfs 将历史峰值清零
 *
 * @data: io_tlb_mem 借用指针；@val: 用户写入值，只接受 0。非零返回 -EINVAL 且状态不变；0 时
 * 原子清零并返回 0。并发分配可能紧接着重新抬高峰值，因此它是诊断重置而非同步屏障。
 */
static int io_tlb_hiwater_set(void *data, u64 val)
{
	struct io_tlb_mem *mem = data;

	/* Only allow setting to zero */
	/* 只允许写 0 清除历史峰值，禁止伪造任意统计值；并发实际使用量不受影响。 */
	if (val != 0)
		return -EINVAL;

	atomic_long_set(&mem->used_hiwater, val);
	return 0;
}

/*
 * 以下两个宏分别生成只读“当前使用量”和可读写“历史高水位”的静态 file_operations；格式单位
 * 都是十进制 slot 数。真正并发语义由各 get/set 原子操作提供，而非 debugfs inode 锁。
 */
DEFINE_DEBUGFS_ATTRIBUTE(fops_io_tlb_used, io_tlb_used_get, NULL, "%llu\n");
DEFINE_DEBUGFS_ATTRIBUTE(fops_io_tlb_hiwater, io_tlb_hiwater_get,
				io_tlb_hiwater_set, "%llu\n");

/*
 * swiotlb_create_debugfs_files() - 为一个分配器建立 debugfs 目录与容量统计文件
 *
 * @mem: 分配器，借用且其生命周期覆盖 debugfs 节点；@dirname: 目录名字符串，debugfs 消费其
 * 内容但本函数不接管存储。即使分配器无 slot 也建立目录，随后直接返回；可用时发布总容量、当前
 * 已用、高水位和可选瞬态容量。返回：无直接返回值；debugfs helper 自行处理失败，函数不回滚。
 * 在 late_initcall 或 reserved pool 首次绑定的可睡眠上下文调用。
 */
static void swiotlb_create_debugfs_files(struct io_tlb_mem *mem,
					 const char *dirname)
{
	mem->debugfs = debugfs_create_dir(dirname, io_tlb_default_mem.debugfs);
	if (!mem->nslabs)
		return;

	debugfs_create_ulong("io_tlb_nslabs", 0400, mem->debugfs, &mem->nslabs);
	debugfs_create_file("io_tlb_used", 0400, mem->debugfs, mem,
			&fops_io_tlb_used);
	debugfs_create_file("io_tlb_used_hiwater", 0600, mem->debugfs, mem,
			&fops_io_tlb_hiwater);
#ifdef CONFIG_SWIOTLB_DYNAMIC
	debugfs_create_file("io_tlb_transient_nslabs", 0400, mem->debugfs,
			    mem, &fops_io_tlb_transient_used);
#endif
}

/*
 * swiotlb_create_default_debugfs() - late_initcall 阶段发布全局默认分配器的诊断节点
 *
 * 入参：无。调用下层以固定目录名 swiotlb 建立文件，无论分配器是否有容量都返回 0，避免诊断
 * 设施失败影响启动。节点借用静态 io_tlb_default_mem，生命周期覆盖系统运行期；函数可睡眠。
 */
static int __init swiotlb_create_default_debugfs(void)
{
	swiotlb_create_debugfs_files(&io_tlb_default_mem, "swiotlb");
	return 0;
}

late_initcall(swiotlb_create_default_debugfs);

#else  /* !CONFIG_DEBUG_FS */

/* 未启用 DEBUG_FS 时保留空 helper，使 restricted pool 调用者无需条件编译。 */
/*
 * swiotlb_create_debugfs_files() - 未启用 DEBUG_FS 时的空实现
 *
 * @mem: 分配器借用指针；@dirname: 借用目录名。两者均不读取，返回：无直接返回值；无副作用、
 * 不睡眠，使 restricted pool 初始化无需条件编译调用点。
 */
static inline void swiotlb_create_debugfs_files(struct io_tlb_mem *mem,
						const char *dirname)
{
}

#endif	/* CONFIG_DEBUG_FS */
/* 以上结束 debugfs 节点创建与无 DEBUG_FS 空实现。 */

#ifdef CONFIG_DMA_RESTRICTED_POOL

/*
 * swiotlb_alloc() - 从 restricted SWIOTLB 池直接分配一段页对齐内存
 *
 * @dev: 绑定 restricted-dma-pool 的设备，借用；@size: 请求字节数，调用者随后按相同大小释放。
 *
 * 与 streaming map 不同，本函数没有原始缓冲区和 bounce 复制；它把 size 对齐要求提升到覆盖该
 * 大小的 buddy order，调用通用 slot 搜索后把物理地址转换为 struct page。成功返回借用给调用者
 * 使用的首页并持有相应 slots；失败返回 NULL 且不留资源。异常非页对齐结果会告警并立即回滚。
 * 搜索路径不睡眠，调用者最终以 swiotlb_free() 归还。
 */
struct page *swiotlb_alloc(struct device *dev, size_t size)
{
	struct io_tlb_mem *mem = dev->dma_io_tlb_mem;
	struct io_tlb_pool *pool;
	phys_addr_t tlb_addr;
	unsigned int align;
	int index;

	/* 阶段 1：设备未关联 restricted 分配器时不能从 SWIOTLB 直接分配页。 */
	if (!mem)
		return NULL;

	/* 阶段 2：用覆盖请求的 2 的幂页块作为对齐掩码，保证返回首页满足页/阶对齐。 */
	align = (1 << (get_order(size) + PAGE_SHIFT)) - 1;
	index = swiotlb_find_slots(dev, 0, size, align, &pool);
	if (index == -1)
		return NULL;

	tlb_addr = slot_addr(pool->start, index);
	if (unlikely(!PAGE_ALIGNED(tlb_addr))) {
		dev_WARN_ONCE(dev, 1, "Cannot allocate pages from non page-aligned swiotlb addr 0x%pa.\n",
			      &tlb_addr);
		swiotlb_release_slots(dev, tlb_addr, pool);
		return NULL;
	}

	return pfn_to_page(PFN_DOWN(tlb_addr));
}

/*
 * swiotlb_free() - 尝试把一段页面归还给设备关联的 restricted SWIOTLB 池
 *
 * @dev: 原分配设备，借用；@page: swiotlb_alloc() 返回的首页，借用；@size: 原请求字节数，当前
 * 实现依赖 slot 元数据恢复真实长度，保留该参数以维持分配接口对称。
 *
 * 先按首页物理地址查池；不属于 SWIOTLB 返回 false 且不触碰页面，调用者可尝试其他释放后端。
 * 命中则归还全部 slots 并返回 true。无睡眠；成功后 page 所指范围不再归调用者使用。
 */
bool swiotlb_free(struct device *dev, struct page *page, size_t size)
{
	phys_addr_t tlb_addr = page_to_phys(page);
	struct io_tlb_pool *pool;

	pool = swiotlb_find_pool(dev, tlb_addr);
	if (!pool)
		return false;

	swiotlb_release_slots(dev, tlb_addr, pool);

	return true;
}

/*
 * rmem_swiotlb_device_init() - 将设备绑定到一个共享 restricted reserved-memory 分配器
 *
 * @rmem: 设备树 reserved_mem 对象，借用；priv 缓存首次创建的 io_tlb_mem。
 * @dev: 正在 attach 的设备，借用；成功后 dma_io_tlb_mem 指向共享分配器。
 *
 * 预留区必须位于线性映射可达的非 highmem。首个设备分配 mem、slots 和单个 area，解密整段预留
 * 内存并初始化强制 bounce/直接分配属性，再把指针发布到 rmem->priv；后续设备复用同一 pool。
 * 成功返回 0，内存分配失败 -ENOMEM，highmem 配置非法 -EINVAL。reserved-memory attach 上下文
 * 可睡眠并负责串行首次初始化；元数据/预留区归 rmem 生命周期，不在单设备 release 中释放。
 */
static int rmem_swiotlb_device_init(struct reserved_mem *rmem,
				    struct device *dev)
{
	struct io_tlb_mem *mem = rmem->priv;
	unsigned long nslabs = rmem->size >> IO_TLB_SHIFT;

	/* Set Per-device io tlb area to one */
	/* restricted pool 可能由多个设备共享，但采用单个 area/锁统一保护其直接页分配元数据。 */
	unsigned int nareas = 1;

	if (PageHighMem(pfn_to_page(PHYS_PFN(rmem->base)))) {
		dev_err(dev, "Restricted DMA pool must be accessible within the linear mapping.");
		return -EINVAL;
	}

	/*
	 * Since multiple devices can share the same pool, the private data,
	 * io_tlb_mem struct, will be initialized by the first device attached
	 * to it.
	 */
	/*
	 * 多设备可共享同一预留区，所以 rmem->priv 是“已初始化”发布标志；只有首个 attach 创建并
	 * 填充 io_tlb_mem，后续 attach 只借用该稳定对象，不能重复清零 slots。
	 */
	if (!mem) {
		struct io_tlb_pool *pool;

		/* 阶段 1：依次取得分配器、每 slot 描述符和单 area；任一步失败都逆序释放已取得元数据。 */
		mem = kzalloc_obj(*mem);
		if (!mem)
			return -ENOMEM;
		pool = &mem->defpool;

		pool->slots = kzalloc_objs(*pool->slots, nslabs);
		if (!pool->slots) {
			kfree(mem);
			return -ENOMEM;
		}

		pool->areas = kzalloc_objs(*pool->areas, nareas);
		if (!pool->areas) {
			kfree(pool->slots);
			kfree(mem);
			return -ENOMEM;
		}

		/* 阶段 2：使预留数据区设备可见，构造空闲链并标记该分配器只用于直接 alloc。 */
		set_memory_decrypted((unsigned long)phys_to_virt(rmem->base),
				     rmem->size >> PAGE_SHIFT);
		swiotlb_init_io_tlb_pool(pool, rmem->base, nslabs,
					 false, nareas);
		mem->force_bounce = true;
		mem->for_alloc = true;
#ifdef CONFIG_SWIOTLB_DYNAMIC
		spin_lock_init(&mem->lock);
		INIT_LIST_HEAD_RCU(&mem->pools);
#endif
		add_mem_pool(mem, pool);

		/* 阶段 3：发布共享对象后再建立诊断节点；后续设备将跳过全部初始化。 */
		rmem->priv = mem;

		swiotlb_create_debugfs_files(mem, rmem->name);
	}

	/* 最后把设备切换到 restricted 分配器；此前失败路径始终保留原设备关联。 */
	dev->dma_io_tlb_mem = mem;

	return 0;
}

/*
 * rmem_swiotlb_device_release() - 解除单个设备与 restricted pool 的关联
 *
 * @rmem: 共享预留内存对象，借用，本函数不修改；@dev: 正在 detach 的设备，借用。
 * 要求该设备的分配/mapping 已结束。函数只把 dma_io_tlb_mem 恢复为全局默认分配器；共享 pool
 * 仍可能被其他设备使用，故不释放 rmem->priv、元数据或预留页。返回：无直接返回值、不睡眠。
 */
static void rmem_swiotlb_device_release(struct reserved_mem *rmem,
					struct device *dev)
{
	dev->dma_io_tlb_mem = &io_tlb_default_mem;
}

/*
 * rmem_swiotlb_setup() - 校验并登记一个设备树 restricted-dma-pool 预留区
 *
 * @node: flattened device tree 节点偏移，仅在启动期解析窗口有效；@rmem: 已解析的预留区描述符，
 * 借用，成功后由 reserved-memory 框架持有。
 *
 * restricted pool 必须是专用、具有线性映射的固定区域，因此拒绝 reusable、默认 CMA/DMA pool
 * 和 no-map 属性；不兼容返回 -EINVAL。成功记录物理范围并返回 0，后续设备 attach 才真正创建
 * SWIOTLB 元数据。__init 阶段无运行期并发，不取得内存所有权。
 */
static int __init rmem_swiotlb_setup(unsigned long node,
				     struct reserved_mem *rmem)
{
	if (of_get_flat_dt_prop(node, "reusable", NULL) ||
	    of_get_flat_dt_prop(node, "linux,cma-default", NULL) ||
	    of_get_flat_dt_prop(node, "linux,dma-default", NULL) ||
	    of_get_flat_dt_prop(node, "no-map", NULL))
		return -EINVAL;

	pr_info("Reserved memory: created restricted DMA pool at %pa, size %ld MiB\n",
		&rmem->base, (unsigned long)rmem->size / SZ_1M);
	return 0;
}

/*
 * rmem_swiotlb_ops 把设备树预留内存框架的三个阶段接到上述实现：节点扫描时校验属性，设备绑定时
 * 初始化或复用共享分配器，解绑时仅恢复设备默认关联。静态 const 操作表由框架借用，生命周期
 * 覆盖初始化与设备运行期，不持有 rmem/dev 引用。
 */
static const struct reserved_mem_ops rmem_swiotlb_ops = {
	.node_init = rmem_swiotlb_setup,
	.device_init = rmem_swiotlb_device_init,
	.device_release = rmem_swiotlb_device_release,
};

/*
 * 该声明把 compatible="restricted-dma-pool" 注册到早期 reserved-memory 匹配表；命中后框架
 * 使用 rmem_swiotlb_ops。宏只发布静态描述，不立即初始化具体设备。
 */
RESERVEDMEM_OF_DECLARE(dma, "restricted-dma-pool", &rmem_swiotlb_ops);
#endif /* CONFIG_DMA_RESTRICTED_POOL */
/* 以上 restricted DMA 直接分配支持仅在 CONFIG_DMA_RESTRICTED_POOL 下编译。 */
