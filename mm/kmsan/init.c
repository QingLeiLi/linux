// SPDX-License-Identifier: GPL-2.0
/*
 * KMSAN initialization routines.
 *
 * Copyright (C) 2017-2021 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */
/*
 * 本文件把启动期物理内存按“数据页 : shadow 页 : origin 页 = 1:1:1”组织起来：
 * memblock 仍可分配时先覆盖已有映射，向 buddy 移交页时再每三块组成一组，最后拆分
 * 零散高阶块并回收可用的三分之一。所有状态均为 __init 生命周期，运行期只消费绑定结果。
 */

#include "kmsan.h"

#include <asm/sections.h>
#include <linux/mm.h>
#include <linux/memblock.h>

#include "../internal.h"

#define NUM_FUTURE_RANGES 128
/*
 * start_end_pair 表示一个页对齐的半开虚拟地址区间 [start, end)，只在启动阶段记录
 * 尚待分配 metadata 的直接映射。字段是地址数值而非持有指针，不承担对象 ownership。
 */
struct start_end_pair {
	u64 start, end;
};

/* 最多缓存 128 个待覆盖区间；future_index 既是有效元素数也是下一写入槽。 */
static struct start_end_pair start_end_pairs[NUM_FUTURE_RANGES] __initdata;
static int future_index __initdata;

/*
 * Record a range of memory for which the metadata pages will be created once
 * the page allocator becomes available.
 */
/*
 * 记录一个当前已存在、但要等页分配器准备后才创建 metadata 页的内存区间。
 *
 * 业务背景：memblock 保留区、内核 .data、NODE_DATA 及早期 percpu 等映射出现顺序不同，
 * kmsan_init_shadow() 先汇总并合并它们，避免为重叠区间重复分配 shadow/origin。
 * 入参：start/end 是借用边界，组成 [start,end)；允许未页对齐但必须非空（s390 可从 0
 * 开始），函数只保存数值，不持有映射引用。
 * 出参/返回：无直接返回值；合并已有元素或追加一个页对齐区间并更新 future_index。
 * 注意事项：仅 __init 串行阶段调用，不睡眠、不加锁；容量和非法范围通过 KMSAN_WARN_ON
 * 诊断，调用者仍必须保证不超过固定数组。
 */
static void __init kmsan_record_future_shadow_range(void *start, void *end)
{
	u64 nstart = (u64)start, nend = (u64)end, cstart, cend;
	bool merged = false;

	/* 先检查固定容量和边界，再把记录扩大到整页，metadata 也按页绑定。 */
	KMSAN_WARN_ON(future_index == NUM_FUTURE_RANGES);
	KMSAN_WARN_ON((nstart >= nend) ||
		      /* Virtual address 0 is valid on s390. */
		      (!IS_ENABLED(CONFIG_S390) && !nstart) || !nend);
	nstart = ALIGN_DOWN(nstart, PAGE_SIZE);
	nend = ALIGN(nend, PAGE_SIZE);

	/*
	 * Scan the existing ranges to see if any of them overlaps with
	 * [start, end). In that case, merge the two ranges instead of
	 * creating a new one.
	 * The number of ranges is less than 20, so there is no need to organize
	 * them into a more intelligent data structure.
	 */
	/*
	 * 扫描已有区间寻找与 [start,end) 重叠或首尾相接者；命中就扩张原元素，
	 * 不新增记录。启动期实际少于 20 段，线性扫描比额外树结构更简单。
	 */
	for (int i = 0; i < future_index; i++) {
		cstart = start_end_pairs[i].start;
		cend = start_end_pairs[i].end;
		if ((cstart < nstart && cend < nstart) ||
		    (cstart > nend && cend > nend))
			/* ranges are disjoint - do not merge */
			/* 两段严格分离时保持现有记录，继续寻找其他可合并项。 */
			continue;
		/* 合并后仍维持半开区间；本次调用至多更新一个已有槽。 */
		start_end_pairs[i].start = min(nstart, cstart);
		start_end_pairs[i].end = max(nend, cend);
		merged = true;
		break;
	}
	/* 未命中才占用新槽，future_index 的递增构成记录发布点。 */
	if (merged)
		return;
	start_end_pairs[future_index].start = nstart;
	start_end_pairs[future_index].end = nend;
	future_index++;
}

/*
 * Initialize the shadow for existing mappings during kernel initialization.
 * These include kernel text/data sections, NODE_DATA and future ranges
 * registered while creating other data (e.g. percpu).
 *
 * Allocations via memblock can be only done before slab is initialized.
 */
/*
 * 初始化内核启动时已经存在的映射之 shadow/origin；范围包括内核 text/data、NODE_DATA
 * 以及创建 percpu 等对象时登记的未来区间。memblock 分配只能发生在 slab 初始化前。
 *
 * 业务背景：mm_init() 在释放 memblock 页以前调用本函数，为早期对象补齐 KMSAN 元数据。
 * 入参：无。
 * 出参/返回：无直接返回值；为所有合并后的区间分配等大 shadow/origin 并绑定 struct page。
 * 注意事项：仅启动串行上下文，可进行 memblock 分配且失败会 panic；完成后这些 metadata
 * backing 由 KMSAN 持有至系统生命周期结束。
 */
void __init kmsan_init_shadow(void)
{
	const size_t nd_size = sizeof(pg_data_t);
	phys_addr_t p_start, p_end;
	u64 loop;
	int nid;

	/* 变量地图：p_start/p_end 是物理半开区间，loop 是 memblock 游标，nid 遍历在线节点。 */
	for_each_reserved_mem_range(loop, &p_start, &p_end)
		kmsan_record_future_shadow_range(phys_to_virt(p_start),
						 phys_to_virt(p_end));
	/* Allocate shadow for .data */
	/* 为内核 .data 分配 shadow；其中的全局状态在 KMSAN 启用前已经可能被读写。 */
	kmsan_record_future_shadow_range(_sdata, _edata);

	/* 每个在线节点的 pg_data_t 也是启动早期建立并长期存活的管理对象。 */
	for_each_online_node(nid)
		kmsan_record_future_shadow_range(
			NODE_DATA(nid), (char *)NODE_DATA(nid) + nd_size);

	/* 汇总结束后逐区间提交 metadata；helper 为每个数据页登记对应两类页。 */
	for (int i = 0; i < future_index; i++)
		kmsan_init_alloc_meta_for_range(
			(void *)start_end_pairs[i].start,
			(void *)start_end_pairs[i].end);
}

/*
 * metadata_page_pair 暂存同一 order 的一块 shadow 和一块 origin；第三块到来时三者
 * 组成完整数据/元数据组。两个指针都代表被 KMSAN 暂扣、尚未进入 buddy 的页块。
 */
struct metadata_page_pair {
	struct page *shadow, *origin;
};

/* 每个 buddy order 独立积累 0、1 或 2 块，启动结束时由 discard 清空。 */
static struct metadata_page_pair held_back[NR_PAGE_ORDERS] __initdata;

/*
 * Eager metadata allocation. When the memblock allocator is freeing pages to
 * pagealloc, we use 2/3 of them as metadata for the remaining 1/3.
 * We store the pointers to the returned blocks of pages in held_back[] grouped
 * by their order: when kmsan_memblock_free_pages() is called for the first
 * time with a certain order, it is reserved as a shadow block, for the second
 * time - as an origin block. On the third time the incoming block receives its
 * shadow and origin ranges from the previously saved shadow and origin blocks,
 * after which held_back[order] can be used again.
 *
 * At the very end there may be leftover blocks in held_back[]. They are
 * collected later by kmsan_memblock_discard().
 */
/*
 * eager metadata 分配：memblock 向 page allocator 释放页块时，每三块暂扣前两块作为
 * 第三块的 shadow/origin，因此只有三分之一作为普通数据页进入 buddy。held_back[]
 * 按 order 分组：第一次保存 shadow，第二次保存 origin，第三次完成绑定并清空槽。
 * 启动末尾遗留的不足三块由 kmsan_memblock_discard() 继续拆分、组合和回收。
 *
 * 业务背景：mm_init.c::memblock_free_pages() 在页真正进入 buddy 前调用本函数。
 * 入参：page 是一块仍由 memblock 移交路径拥有的 2^order 连续页；order 必须落在
 * NR_PAGE_ORDERS 范围内，指针在返回 false 时转由 KMSAN 暂存。
 * 出参/返回：false 表示该块被扣作 metadata、调用者不得释放；true 表示本块已绑定两类
 * metadata，可由调用者作为数据页释放到 buddy。
 * 注意事项：启动串行、无锁、不睡眠；true 路径消耗此前两块，false 路径发生 ownership
 * 转移，调用者必须严格按布尔值分流。
 */
bool kmsan_memblock_free_pages(struct page *page, unsigned int order)
{
	struct page *shadow, *origin;

	/* 第一块成为 shadow backing，尚无数据页可以交给 buddy。 */
	if (!held_back[order].shadow) {
		held_back[order].shadow = page;
		return false;
	}
	/* 第二块成为 origin backing，继续由 KMSAN 暂扣。 */
	if (!held_back[order].origin) {
		held_back[order].origin = page;
		return false;
	}
	/* 第三块作为数据页，取走同阶 pair 并逐页建立 metadata 指针。 */
	shadow = held_back[order].shadow;
	origin = held_back[order].origin;
	kmsan_setup_meta(page, shadow, origin, order);

	/* 清空槽后同一 order 可重新开始下一组三块；true 把 page 归还给调用者。 */
	held_back[order].shadow = NULL;
	held_back[order].origin = NULL;
	return true;
}

#define MAX_BLOCKS 8
/*
 * smallstack 是启动期拆分回收用的定长 LIFO：items[0..index) 持有同一 order 的页块，
 * order 记录这些块的阶数。collect 是跨循环复用的唯一实例，无并发访问。
 */
struct smallstack {
	struct page *items[MAX_BLOCKS];
	int index;
	int order;
};

/* 从最大 buddy order 向 0 逐级下降，任一时刻最多积累 8 块待组合页。 */
static struct smallstack collect = {
	.index = 0,
	.order = MAX_PAGE_ORDER,
};

/*
 * smallstack_push() - 把一个同阶页块的 ownership 压入启动期临时栈。
 * 业务背景：discard 收集 held_back 或拆分结果时用它组成三块一组。
 * 入参：stack 是可写借用栈且 index<MAX_BLOCKS；pages 是转入栈管理的页块。
 * 出参/返回：无直接返回值；items[index] 接管 pages 并递增 index。
 * 注意事项：启动期串行、不睡眠；溢出仅 WARN，容量不变量必须由算法保证。
 */
static void smallstack_push(struct smallstack *stack, struct page *pages)
{
	KMSAN_WARN_ON(stack->index == MAX_BLOCKS);
	stack->items[stack->index] = pages;
	stack->index++;
}
#undef MAX_BLOCKS

/*
 * smallstack_pop() - 从启动期临时栈取回最近压入的同阶页块。
 * 业务背景：collection 每次弹出三块做 data/shadow/origin，split 则弹出后拆成低一阶。
 * 入参：stack 是可写借用栈，调用前 index 必须大于 0。
 * 出参/返回：返回页块并把 ownership 交给调用者，同时清空原槽。
 * 注意事项：无锁、不睡眠；下溢只 WARN，调用者必须维持非空不变量。
 */
static struct page *smallstack_pop(struct smallstack *stack)
{
	struct page *ret;

	KMSAN_WARN_ON(stack->index == 0);
	stack->index--;
	ret = stack->items[stack->index];
	stack->items[stack->index] = NULL;
	return ret;
}

/*
 * do_collection() - 把 collect 中每三块组合成一组 KMSAN 数据/元数据页。
 * 业务背景：启动末尾尽可能回收被暂扣页；每组第三块绑定前两块后进入 buddy。
 * 入参：无；读写全局 collect，其所有元素必须具有 collect.order。
 * 出参/返回：无直接返回值；每轮消耗三块，并释放数据块，剩余数量小于 3。
 * 注意事项：启动串行，可进入 core page free；shadow/origin 被永久绑定而不释放。
 */
static void do_collection(void)
{
	struct page *page, *shadow, *origin;

	/* LIFO 次序本身不重要，关键是不让同一块同时承担两种角色。 */
	while (collect.index >= 3) {
		page = smallstack_pop(&collect);
		shadow = smallstack_pop(&collect);
		origin = smallstack_pop(&collect);
		/* 建立逐页映射后，只有 data block 能作为普通页发布给 buddy。 */
		kmsan_setup_meta(page, shadow, origin, collect.order);
		__free_pages_core(page, collect.order, MEMINIT_EARLY);
	}
}

/*
 * collect_split() - 把不足三块的当前阶页逐块二分到下一低阶。
 * 业务背景：高阶余数无法凑齐三元组时，降阶可与 held_back[order-1] 的遗留块继续组合。
 * 入参：无；collect.order/index 描述当前持有页块。
 * 出参/返回：无直接返回值；order 为 0 时不变，否则 ownership 全部转入低一阶临时栈。
 * 注意事项：启动串行、不分配内存；page 数组偏移 1<<new_order 指向后一半伙伴块。
 */
static void collect_split(void)
{
	struct smallstack tmp = {
		.order = collect.order - 1,
		.index = 0,
	};
	struct page *page;

	/* order-0 已不能再拆，至多两块 metadata 遗留会保持保留状态。 */
	if (!collect.order)
		return;
	/* 每个高阶块拆成两个相邻低阶块，全部压入 tmp 后再整体替换 collect。 */
	while (collect.index) {
		page = smallstack_pop(&collect);
		smallstack_push(&tmp, &page[0]);
		smallstack_push(&tmp, &page[1 << tmp.order]);
	}
	/* tmp 不含外部资源，结构体复制同时提交新 order、index 和全部槽位。 */
	__memcpy(&collect, &tmp, sizeof(tmp));
}

/*
 * Memblock is about to go away. Split the page blocks left over in held_back[]
 * and return 1/3 of that memory to the system.
 */
/*
 * memblock 即将退出：拆分 held_back[] 中遗留页块，并把可组成三元组的数据三分之一
 * 归还系统。每个 order 先汇入 collect，尽量三块成组，再把余数拆到 order-1 重复。
 *
 * 业务背景：kmsan_init_runtime() 在 buddy/slab 就绪后调用，关闭启动期暂扣账本。
 * 入参：无。
 * 出参/返回：无直接返回值；清空 held_back，绑定新 metadata，并释放可用数据页。
 * 注意事项：只调用一次且无并发；order-0 最终不足三块无法安全成为数据页，继续保留。
 */
static void kmsan_memblock_discard(void)
{
	/*
	 * For each order=N:
	 *  - push held_back[N].shadow and .origin to @collect;
	 *  - while there are >= 3 elements in @collect, do garbage collection:
	 *    - pop 3 ranges from @collect;
	 *    - use two of them as shadow and origin for the third one;
	 *    - repeat;
	 *  - split each remaining element from @collect into 2 ranges of
	 *    order=N-1,
	 *  - repeat.
	 */
	/*
	 * 对每个 order=N：把 held_back[N] 的 shadow/origin 推入 collect；每满三块就
	 * 弹出三块，以两块作为第三块的 metadata 后释放第三块；余数全部二分成 N-1
	 * 阶块并继续。这样不会把尚无 metadata 的页发布给 buddy。
	 */
	collect.order = MAX_PAGE_ORDER;
	for (int i = MAX_PAGE_ORDER; i >= 0; i--) {
		/* 先转移本阶遗留 ownership，再清空原槽，防止后续误重复消费。 */
		if (held_back[i].shadow)
			smallstack_push(&collect, held_back[i].shadow);
		if (held_back[i].origin)
			smallstack_push(&collect, held_back[i].origin);
		held_back[i].shadow = NULL;
		held_back[i].origin = NULL;
		/* 先最大化本阶回收，再把不足三块的余数降阶。 */
		do_collection();
		collect_split();
	}
}

/*
 * kmsan_init_runtime() - 完成启动页收尾并发布 KMSAN 运行期开关。
 *
 * 业务背景：mm_init() 在 buddy、slab、vmalloc 与 init_task 已可用后调用；此前只能建立
 * metadata，不能让普通插桩路径假定完整运行时存在。
 * 入参：无。
 * 出参/返回：无直接返回值；初始化 current 的 KMSAN 上下文、回收可用遗留页并最终把
 * kmsan_enabled 置 true，随后所有插桩可观察 metadata。
 * 注意事项：仅 __init 串行调用一次；enabled 写入是不可回滚发布点，必须最后执行。
 */
void __init kmsan_init_runtime(void)
{
	/* Assuming current is init_task */
	/* 此时 current 必须是 init_task；先建立任务递归/报告状态再允许插桩进入运行时。 */
	kmsan_internal_task_create(current);
	/* 关闭 memblock 暂存账本，保证可回收数据页已有完整 shadow/origin。 */
	kmsan_memblock_discard();
	pr_info("Starting KernelMemorySanitizer\n");
	pr_info("ATTENTION: KMSAN is a debugging tool! Do not use it on production machines!\n");
	/* 最后发布全局开关；此前任何读者都不应使用尚未完备的运行时。 */
	kmsan_enabled = true;
}
