// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/percpu-km.c - kernel memory based chunk allocation
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * Chunks are allocated as a contiguous kernel memory using gfp
 * allocation.  This is to be used on nommu architectures.
 *
 * To use percpu-km,
 *
 * - define CONFIG_NEED_PER_CPU_KM from the arch Kconfig.
 *
 * - CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK must not be defined.  It's
 *   not compatible with PER_CPU_KM.  EMBED_FIRST_CHUNK should work
 *   fine.
 *
 * - NUMA is not supported.  When setting up the first chunk,
 *   @cpu_distance_fn should be NULL or report all CPUs to be nearer
 *   than or at LOCAL_DISTANCE.
 *
 * - It's best if the chunk size is power of two multiple of
 *   PAGE_SIZE.  Because each chunk is allocated as a contiguous
 *   kernel memory block using alloc_pages(), memory will be wasted if
 *   chunk size is not aligned.  percpu-km code will whine about it.
 */
/*
 * 本文件是基于连续内核内存的 percpu chunk 后端。它用带 GFP 标志的
 * alloc_pages() 为整个 chunk 一次取得物理连续页，供 NOMMU 架构使用。
 *
 * 启用约束是：架构 Kconfig 选择 CONFIG_NEED_PER_CPU_KM；不能同时选择分页式
 * first chunk（嵌入式 first chunk 可以）；不支持 NUMA，因而 first chunk 的
 * cpu_distance_fn 必须为空或把所有 CPU 报告为 LOCAL_DISTANCE 内。chunk 大小
 * 最好是 PAGE_SIZE 的 2 次幂倍数，否则伙伴分配向上取 order 后会浪费尾部页，
 * pcpu_verify_alloc_info() 会发出告警。该后端由 percpu.c 直接 include，接口
 * 与默认 percpu-vm.c 后端互斥，不是单独链接的模块。
 */

#if defined(CONFIG_SMP) && defined(CONFIG_NEED_PER_CPU_PAGE_FIRST_CHUNK)
#error "contiguous percpu allocation is incompatible with paged first chunk"
#endif

#include <linux/log2.h>

/*
 * pcpu_post_unmap_tlb_flush() - 满足 percpu core 的解除映射后 TLB 刷新接口。
 * 业务背景：KM 后端从未建立 vmalloc 映射，chunk 地址始终来自 direct map，
 * 所以 VM 后端需要的 unmap 后刷新在这里无事可做。
 * 入参：@chunk 为借用 chunk；@page_start/@page_end 是半开页索引区间，均只为
 * 接口一致性保留，函数不读取、不修改、不接管它们。
 * 出参/返回：无直接返回值，无副作用。
 * 注意事项：不睡眠、无锁要求；空实现仅适用于 CONFIG_NEED_PER_CPU_KM 后端。
 */
static void pcpu_post_unmap_tlb_flush(struct pcpu_chunk *chunk,
				      int page_start, int page_end)
{
	/* nothing */
	/* direct map 没有被撤销，因此没有陈旧 TLB 映射需要失效。 */
}

/*
 * pcpu_populate_chunk() - 报告 KM chunk 的指定范围已具备物理后备。
 * 业务背景：pcpu_create_chunk() 已一次性分配整个连续块，后续 core 的按区间
 * populate 请求无需再分配或映射页。
 * 入参：@chunk 为借用 chunk；@page_start/@page_end 是半开页索引；@gfp 是
 * 分配约束。四者在该后端均不被消费或保存。
 * 出参/返回：恒返回 0，表示范围可用；无额外分配和 ownership 变化。
 * 注意事项：不睡眠、无失败路径；成功只因 create 阶段已经完成全部 backing。
 */
static int pcpu_populate_chunk(struct pcpu_chunk *chunk,
			       int page_start, int page_end, gfp_t gfp)
{
	return 0;
}

/*
 * pcpu_depopulate_chunk() - 满足 core 的区间去填充接口而保留整个 KM backing。
 * 业务背景：连续块不能像 vmalloc chunk 那样逐页拆除，否则会破坏一次性按
 * order 分配/释放的 ownership，因此实际释放推迟到 pcpu_destroy_chunk()。
 * 入参：@chunk 为借用 chunk；@page_start/@page_end 为半开页索引，均不修改。
 * 出参/返回：无直接返回值，无副作用。
 * 注意事项：不睡眠；调用者的 bookkeeping 可以变化，但物理页仍归 chunk。
 */
static void pcpu_depopulate_chunk(struct pcpu_chunk *chunk,
				  int page_start, int page_end)
{
	/* nada */
	/* 连续页只能整体归还，区间 depopulate 在 KM 后端故意为空。 */
}

/*
 * pcpu_create_chunk() - 创建并发布一个全范围已填充的连续 percpu chunk。
 *
 * 业务背景：percpu 分配器无可用空间时调用本函数；它先创建通用 metadata，
 * 再取得 group 0 所需的连续页，建立 page->chunk 反查并提交 populated 状态。
 * 入参：@gfp 是 metadata 与伙伴页分配共同使用的 GFP 约束，不被保存。
 * 出参/返回：成功返回由 percpu core 接管的 chunk；metadata、连续页和每页反查
 * 均有效。任一分配失败返回 NULL，并释放此前取得的 metadata，不泄漏资源。
 * 注意事项：可睡眠性由 @gfp 决定；创建阶段尚未在 chunk list 发布，只有更新
 * 全局 populated 计数时持 pcpu_lock 并关本地中断。NUMA 与多 group 不支持。
 */
static struct pcpu_chunk *pcpu_create_chunk(gfp_t gfp)
{
	/* nr_pages 是 group 0 的页数；验证阶段保证这里只有一个 group。 */
	const int nr_pages = pcpu_group_sizes[0] >> PAGE_SHIFT;
	/* chunk 持有通用 allocator metadata；pages 是连续块的首页及最终释放句柄。 */
	struct pcpu_chunk *chunk;
	struct page *pages;
	/* flags 保存 pcpu_lock 的中断状态；i 为逐页建立反查的索引。 */
	unsigned long flags;
	int i;

	/* 阶段 1：构造 bitmap/metadata；失败时尚无物理页需要回滚。 */
	chunk = pcpu_alloc_chunk(gfp);
	if (!chunk)
		return NULL;

	/* 阶段 2：伙伴分配按 2 次幂 order 取得连续块，失败只撤销 metadata。 */
	pages = alloc_pages(gfp, order_base_2(nr_pages));
	if (!pages) {
		pcpu_free_chunk(chunk);
		return NULL;
	}

	/* 阶段 3：让任一 percpu 地址经 page 都能反查所属 chunk。 */
	for (i = 0; i < nr_pages; i++)
		pcpu_set_page_chunk(pages + i, chunk);

	/* data 持有释放句柄，base_addr 则是 unit0 的 direct-map 虚拟起点。 */
	chunk->data = pages;
	chunk->base_addr = page_address(pages);

	/* 阶段 4：在 pcpu_lock 下把整个范围提交为 populated 并更新全局计数。 */
	spin_lock_irqsave(&pcpu_lock, flags);
	pcpu_chunk_populated(chunk, 0, nr_pages);
	spin_unlock_irqrestore(&pcpu_lock, flags);

	/* 提交后再更新统计和 trace；此时观察者看到的是完整可用 chunk。 */
	pcpu_stats_chunk_alloc();
	trace_percpu_create_chunk(chunk->base_addr);

	return chunk;
}

/*
 * pcpu_destroy_chunk() - 整体销毁连续 KM chunk 及其通用 metadata。
 * 业务背景：percpu core 已把目标从可分配列表隔离后调用；KM 后端不做逐页
 * depopulate，所以这里按创建的逆序整体归还伙伴页和 chunk 描述符。
 * 入参：@chunk 是可空、由调用者移交销毁责任的 chunk；非 NULL 时不得再有
 * 分配者访问，函数消费其 data 与 metadata ownership。
 * 出参/返回：无直接返回值；NULL 无副作用，成功后 chunk 指针失效。
 * 注意事项：__free_pages() 以创建时相同 order 释放；调用者必须先隔离 chunk，
 * 且不可传 first/reserved immutable chunk。释放可能触发调试路径但不失败。
 */
static void pcpu_destroy_chunk(struct pcpu_chunk *chunk)
{
	/* nr_pages 只用于重建 alloc_pages() 对应的释放 order。 */
	const int nr_pages = pcpu_group_sizes[0] >> PAGE_SHIFT;

	/* 允许 cleanup 无条件传 NULL，避免重复分支。 */
	if (!chunk)
		return;

	/* 先记录生命周期终点，随后释放后便不能再读取 base_addr。 */
	pcpu_stats_chunk_dealloc();
	trace_percpu_destroy_chunk(chunk->base_addr);

	/* data 非空时持有连续块首页；最后释放内含 bitmap 的通用 chunk metadata。 */
	if (chunk->data)
		__free_pages(chunk->data, order_base_2(nr_pages));
	pcpu_free_chunk(chunk);
}

/*
 * pcpu_addr_to_page() - 把 KM 后端的 direct-map 地址转换为 struct page。
 * 业务背景：pcpu_chunk_addr_search() 需要由动态 percpu 地址反查 page->chunk。
 * 入参：@addr 是 chunk 范围内的借用内核虚拟地址，非 NULL，不转移 ownership。
 * 出参/返回：返回对应页的借用描述符，不增加页引用。
 * 注意事项：仅适用于 direct map；调用者须保证 chunk 未被并发销毁，函数不睡眠。
 */
static struct page *pcpu_addr_to_page(void *addr)
{
	return virt_to_page(addr);
}

/*
 * pcpu_verify_alloc_info() - 在启动提交前验证 KM 后端可表示的布局。
 * 业务背景：pcpu_setup_first_chunk() 用它拒绝多 group，并提示伙伴 order 向上
 * 取整造成的内部浪费，防止后续只访问 groups[0] 时遗漏 CPU 单元。
 * 入参：@ai 是启动期借用的只读布局，非 NULL；单位数和 unit_size 共同给出字节量。
 * 出参/返回：单 group 返回 0（即使有浪费）；多 group 打印 critical 并返回
 * -EINVAL。函数不接管 @ai，也不分配资源。
 * 注意事项：__init，仅启动期调用；告警不是失败，nr_pages 必须由布局保证非零。
 */
static int __init pcpu_verify_alloc_info(const struct pcpu_alloc_info *ai)
{
	/* nr_pages 是实际需求；alloc_pages 是伙伴系统按 2 次幂 order 占用的页数。 */
	size_t nr_pages, alloc_pages;

	/* all units must be in a single group */
	/* KM 后端只保存 group 0，因此所有 unit 必须处于单一 group。 */
	if (ai->nr_groups != 1) {
		pr_crit("can't handle more than one group\n");
		return -EINVAL;
	}

	/* 用 unit 总数乘单元字节数得到 chunk 需求，再比较伙伴分配的向上取整量。 */
	nr_pages = (ai->groups[0].nr_units * ai->unit_size) >> PAGE_SHIFT;
	alloc_pages = roundup_pow_of_two(nr_pages);

	/* 浪费不影响正确性，只提醒架构调整 chunk/atom 布局。 */
	if (alloc_pages > nr_pages)
		pr_warn("wasting %zu pages per chunk\n",
			alloc_pages - nr_pages);

	return 0;
}

/*
 * pcpu_should_reclaim_chunk() - 禁止 KM 后端进入逐页回收状态机。
 * 业务背景：VM 后端可把空 populated 页送入 sidelined/to_depopulate 列表，
 * 连续 KM chunk 不能部分释放，空闲块只能由整体 destroy 策略回收。
 * 入参：@chunk 是被考察的借用 chunk，非 NULL；函数不读取或修改它。
 * 出参/返回：恒为 false，无副作用和 ownership 变化。
 * 注意事项：不睡眠、无锁要求；恒假是连续分配/释放不变量的一部分。
 */
static bool pcpu_should_reclaim_chunk(struct pcpu_chunk *chunk)
{
	return false;
}
