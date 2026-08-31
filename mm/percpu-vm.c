// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/percpu-vm.c - vmalloc area based chunk allocation
 *
 * Copyright (C) 2010		SUSE Linux Products GmbH
 * Copyright (C) 2010		Tejun Heo <tj@kernel.org>
 *
 * Chunks are mapped into vmalloc areas and populated page by page.
 * This is the default chunk allocator.
 */
/*
 * 本文件是 percpu 动态分配器的默认 VM 后端，由 percpu.c 直接 include。每个 chunk 先为
 * 各 NUMA group 保留连续 vmalloc 虚拟区，再按 CPU/unit、按页分配物理 backing 并映射；
 * 空闲页可单独 unmap/free，兼顾稳定 percpu 地址与内存回收。所有接口只供同一编译单元使用。
 */
#include "internal.h"

/*
 * 把 chunk 中某 CPU/unit 的逻辑页索引反查为当前映射的 struct page。
 * 业务背景：depopulate 要在清 PTE 前保存物理页指针，随后才能释放 backing。
 * 入参：@chunk 是借用的可变 VM chunk；@cpu 为 possible CPU，@page_idx 在 chunk 页范围内。
 * 出参/返回：返回 vmalloc 映射当前指向的借用 page，未映射时可能为 NULL；不增加引用。
 * 注意事项：预映射 immutable 首块不走 VM 后端的动态反查，误用会 WARN；调用者以
 * pcpu_alloc_mutex 稳定映射，不睡眠。
 */
static struct page *pcpu_chunk_page(struct pcpu_chunk *chunk,
				    unsigned int cpu, int page_idx)
{
	/* must not be used on pre-mapped chunk */
	/* immutable 表示启动期预映射 chunk，不允许按本后端协议拆除其 PTE。 */
	WARN_ON(chunk->immutable);

	/* pcpu_chunk_addr 先换算 unit 虚址，vmalloc_to_page 再读取页表反查 backing。 */
	return vmalloc_to_page((void *)pcpu_chunk_addr(chunk, cpu, page_idx));
}

/**
 * pcpu_get_pages - get temp pages array
 *
 * Returns pointer to array of pointers to struct page which can be indexed
 * with pcpu_page_idx().  Note that there is only one array and accesses
 * should be serialized by pcpu_alloc_mutex.
 *
 * RETURNS:
 * Pointer to temp pages array on success.
 */
/*
 * 懒分配并返回全局唯一的临时 page* 工作数组。
 * 业务背景：populate/depopulate 都需为所有 unit 暂存页指针；串行操作允许复用一个最大
 * 数组，避免每个区间反复分配。数组用 pcpu_page_idx(cpu,page) 索引。
 * 入参：无。出参/返回：成功返回长期持有的共享数组，首次分配失败返回 NULL；调用者只
 * 借用，不能释放或跨 pcpu_alloc_mutex 保存其中内容。
 * 注意事项：必须持 pcpu_alloc_mutex、可睡眠；pages 静态指针只在首次成功后发布，后续
 * 调用复用，数组大小由启动期固定的 nr_units*unit_pages 决定。
 */
static struct page **pcpu_get_pages(void)
{
	/* pages 是后端终生共享工作区；pages_size 是所有 unit 页指针的字节容量。 */
	static struct page **pages;
	size_t pages_size = pcpu_nr_units * pcpu_unit_pages * sizeof(pages[0]);

	lockdep_assert_held(&pcpu_alloc_mutex);

	/* 只有首次调用分配并清零；失败不缓存，下一次仍可重试。 */
	if (!pages)
		pages = pcpu_mem_zalloc(pages_size, GFP_KERNEL);
	return pages;
}

/**
 * pcpu_free_pages - free pages which were allocated for @chunk
 * @chunk: chunk pages were allocated for
 * @pages: array of pages to be freed, indexed by pcpu_page_idx()
 * @page_start: page index of the first page to be freed
 * @page_end: page index of the last page to be freed + 1
 *
 * Free pages [@page_start and @page_end) in @pages for all units.
 * The pages were allocated for @chunk.
 */
/*
 * 释放工作数组中指定逻辑页区间对应的所有 unit backing 页。
 * 业务背景：populate 分配/映射失败回滚，以及 depopulate 清 PTE 后，都需批量归还物理页。
 * 入参：@chunk 标识这些页所属 chunk（仅契约/诊断，当前体不读取）；@pages 是借用工作
 * 数组；@page_start/@page_end 是半开逻辑页区间，应用于每个 possible CPU。
 * 出参/返回：无直接返回；非 NULL page 交回 buddy，数组槽内容不清零且不再有效。
 * 注意事项：调用者必须确保页未再映射或映射失败未发布；可在 mutex 下执行，不取得引用。
 */
static void pcpu_free_pages(struct pcpu_chunk *chunk,
			    struct page **pages, int page_start, int page_end)
{
	/* cpu/i 组成 pcpu_page_idx 二维索引；page 是本次待释放的借用指针。 */
	unsigned int cpu;
	int i;

	/* 按 unit 遍历半开区间；NULL 槽允许部分分配失败共用回滚。 */
	for_each_possible_cpu(cpu) {
		for (i = page_start; i < page_end; i++) {
			struct page *page = pages[pcpu_page_idx(cpu, i)];

			if (page)
				__free_page(page);
		}
	}
}

/**
 * pcpu_alloc_pages - allocates pages for @chunk
 * @chunk: target chunk
 * @pages: array to put the allocated pages into, indexed by pcpu_page_idx()
 * @page_start: page index of the first page to be allocated
 * @page_end: page index of the last page to be allocated + 1
 * @gfp: allocation flags passed to the underlying allocator
 *
 * Allocate pages [@page_start,@page_end) into @pages for all units.
 * The allocation is for @chunk.  Percpu core doesn't care about the
 * content of @pages and will pass it verbatim to pcpu_map_pages().
 */
/*
 * 为所有 possible CPU 的同一逻辑区间分配 order-0 backing 页。
 * 业务背景：VM 后端把物理分配和虚拟映射分成两阶段；本函数只填充临时数组，成功后由
 * pcpu_map_pages() 原样消费，尚未发布到 chunk 地址空间。
 * 入参：@chunk 是目标借用 chunk（当前体不读取）；@pages 为输出工作数组；半开区间
 * @page_start..@page_end 适用于每个 unit；@gfp 是调用者分配策略，函数额外允许 HIGHMEM。
 * 出参/返回：全部分配成功返回 0；任一页失败返回 -ENOMEM，并释放本次已分配的当前 CPU
 * 前缀和所有早先 CPU 完整区间，调用前已有槽不属于本次契约。
 * 注意事项：可睡眠；按 cpu_to_node(cpu) 优先本地 NUMA 节点。成功页 ownership 暂归
 * 工作批次，映射成功后转为 chunk backing；错误回滚顺序不能漏掉当前行之前的页。
 */
static int pcpu_alloc_pages(struct pcpu_chunk *chunk,
			    struct page **pages, int page_start, int page_end,
			    gfp_t gfp)
{
	/* cpu 是当前 unit，tcpu 回滚已完成 unit，i 是页索引。 */
	unsigned int cpu, tcpu;
	int i;

	/* percpu 通过 vmalloc 映射访问，物理页无需永久 direct map，可使用 HIGHMEM。 */
	gfp |= __GFP_HIGHMEM;

	/* 阶段 1：逐 CPU、逐页分配，数组槽本身就是尚未映射的 ownership 账本。 */
	for_each_possible_cpu(cpu) {
		for (i = page_start; i < page_end; i++) {
			struct page **pagep = &pages[pcpu_page_idx(cpu, i)];

			*pagep = alloc_pages_node(cpu_to_node(cpu), gfp, 0);
			if (!*pagep)
				goto err;
		}
	}
	return 0;

err:
	/* 阶段 2：先释放失败 CPU 已完成前缀，再释放所有更早 CPU 的完整区间。 */
	while (--i >= page_start)
		__free_page(pages[pcpu_page_idx(cpu, i)]);

	for_each_possible_cpu(tcpu) {
		if (tcpu == cpu)
			break;
		for (i = page_start; i < page_end; i++)
			__free_page(pages[pcpu_page_idx(tcpu, i)]);
	}
	return -ENOMEM;
}

/**
 * pcpu_pre_unmap_flush - flush cache prior to unmapping
 * @chunk: chunk the regions to be flushed belongs to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages in [@page_start,@page_end) of @chunk are about to be
 * unmapped.  Flush cache.  As each flushing trial can be very
 * expensive, issue flush on the whole region at once rather than
 * doing it for each cpu.  This could be an overkill but is more
 * scalable.
 */
/*
 * 在撤销 PTE 前一次性清理 chunk 目标区间的虚拟别名缓存。
 * 业务背景：后续会 unmap 并释放 backing，必须先按 vmap/vunmap 协议处理 cache alias；
 * 与其逐 CPU 执行昂贵 flush，不如覆盖 low_unit 到 high_unit 的整个虚拟跨度。
 * 入参：@chunk 是借用 VM chunk；@page_start/@page_end 是每 unit 半开页区间。
 * 出参/返回：无直接返回；完成该虚拟跨度的 cache vunmap 前置刷新。
 * 注意事项：调用者持 pcpu_alloc_mutex，映射仍存在；范围可能包含 group 空洞，属于以更大
 * flush 换可扩展性的有意 overkill，不释放页、不负责 TLB flush。
 */
static void pcpu_pre_unmap_flush(struct pcpu_chunk *chunk,
				 int page_start, int page_end)
{
	/* low/high unit 边界把所有 CPU 的对应区间合并成一次架构 cache flush。 */
	flush_cache_vunmap(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

/*
 * 撤销一个 unit 的连续 PTE，但故意延后 TLB flush。
 * 入参：@addr 为页对齐 vmalloc 起点，@nr_pages 为连续页数。无返回；页表映射被清除。
 * 调用者必须已做 cache 前刷，并在复用虚址/需要立即完成时统一执行后续 TLB flush。
 */
static void __pcpu_unmap_pages(unsigned long addr, int nr_pages)
{
	vunmap_range_noflush(addr, addr + (nr_pages << PAGE_SHIFT));
}

/**
 * pcpu_unmap_pages - unmap pages out of a pcpu_chunk
 * @chunk: chunk of interest
 * @pages: pages array which can be used to pass information to free
 * @page_start: page index of the first page to unmap
 * @page_end: page index of the last page to unmap + 1
 *
 * For each cpu, unmap pages [@page_start,@page_end) out of @chunk.
 * Corresponding elements in @pages were cleared by the caller and can
 * be used to carry information to pcpu_free_pages() which will be
 * called after all unmaps are finished.  The caller should call
 * proper pre/post flush functions.
 */
/*
 * 对所有 unit 撤销指定逻辑区间映射，并把原 backing 页保存进工作数组。
 * 业务背景：vmalloc_to_page 必须在 PTE 清除前执行；保存结果随后交 pcpu_free_pages，映射
 * 清除则按 unit 调用 noflush helper，最终由外层批量刷新 TLB。
 * 入参：@chunk 是借用可变 chunk；@pages 是输出工作数组；@page_start/@page_end 为半开
 * 区间。出参/返回：无直接返回；每个槽得到原 page 借用指针，各 unit PTE 被撤销。
 * 注意事项：调用者持 pcpu_alloc_mutex，且已 pcpu_pre_unmap_flush；完成后必须按场景调用
 * post TLB flush 或把区域交 vmalloc lazy flush。页此时不可再经 chunk 虚址访问。
 */
static void pcpu_unmap_pages(struct pcpu_chunk *chunk,
			     struct page **pages, int page_start, int page_end)
{
	/* cpu/i 遍历二维映射；page 在清 PTE 前由反查取得。 */
	unsigned int cpu;
	int i;

	/* 阶段 1：先收集一个 unit 的所有 page，再一次撤销该 unit 连续范围。 */
	for_each_possible_cpu(cpu) {
		for (i = page_start; i < page_end; i++) {
			struct page *page;

			page = pcpu_chunk_page(chunk, cpu, i);
			WARN_ON(!page);
			pages[pcpu_page_idx(cpu, i)] = page;
		}
		/* 工作数组已持有物理页身份，现在清除 PTE 而不逐 unit 刷 TLB。 */
		__pcpu_unmap_pages(pcpu_chunk_addr(chunk, cpu, page_start),
				   page_end - page_start);
	}
}

/**
 * pcpu_post_unmap_tlb_flush - flush TLB after unmapping
 * @chunk: pcpu_chunk the regions to be flushed belong to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages [@page_start,@page_end) of @chunk have been unmapped.  Flush
 * TLB for the regions.  This can be skipped if the area is to be
 * returned to vmalloc as vmalloc will handle TLB flushing lazily.
 *
 * As with pcpu_pre_unmap_flush(), TLB flushing also is done at once
 * for the whole region.
 */
/*
 * 在所有 unit PTE 已撤销后，一次性失效整个虚拟跨度的内核 TLB。
 * 业务背景：noflush unmap 允许把多个 unit/区间的 shootdown 合并；若 VM 区域即将归还
 * vmalloc，可由 vmalloc 的 lazy flush 接管而跳过本函数。
 * 入参：@chunk 与半开页区间均为借用定位信息。出参/返回：无直接返回；旧虚拟翻译在
 * low_unit..high_unit 范围失效。注意事项：必须晚于 PTE 清除；不处理 cache 或释放页。
 */
static void pcpu_post_unmap_tlb_flush(struct pcpu_chunk *chunk,
				      int page_start, int page_end)
{
	flush_tlb_kernel_range(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

/*
 * 把一个 unit 的连续 page 数组建立为 PAGE_KERNEL vmalloc 映射，但暂不做 cache flush。
 * 入参：@addr 为目标虚址；@pages 为借用 backing 数组；@nr_pages 为页数。
 * 返回 0 或 vmap_pages_range_noflush 的负 errno；成功发布 PTE，ownership 不变，可睡眠。
 */
static int __pcpu_map_pages(unsigned long addr, struct page **pages,
			    int nr_pages)
{
	return vmap_pages_range_noflush(addr, addr + (nr_pages << PAGE_SHIFT),
			PAGE_KERNEL, pages, PAGE_SHIFT, GFP_KERNEL);
}

/**
 * pcpu_map_pages - map pages into a pcpu_chunk
 * @chunk: chunk of interest
 * @pages: pages array containing pages to be mapped
 * @page_start: page index of the first page to map
 * @page_end: page index of the last page to map + 1
 *
 * For each cpu, map pages [@page_start,@page_end) into @chunk.  The
 * caller is responsible for calling pcpu_post_map_flush() after all
 * mappings are complete.
 *
 * This function is responsible for setting up whatever is necessary for
 * reverse lookup (addr -> chunk).
 */
/*
 * 为所有 unit 建立指定逻辑区间映射，并登记 page→chunk 反向查找。
 * 业务背景：物理页分配完成后，本函数把各 CPU 页映射到固定 percpu 虚址；反向映射供
 * pcpu_chunk_addr_search()/释放路径从地址或 page 找回 chunk。
 * 入参：@chunk 是借用目标；@pages 是 pcpu_alloc_pages 填好的 backing 数组；半开区间
 * @page_start/@page_end 对每个 unit 生效。
 * 出参/返回：全部 unit 成功返回 0；任一 vmap 失败返回负 errno，撤销截至失败 unit 的映射
 * 并完成 TLB flush。物理页始终归调用者，失败后由 populate 释放。
 * 注意事项：调用者持 pcpu_alloc_mutex、可睡眠；成功后仍须 pcpu_post_map_flush 才能安全
 * 通过新别名访问。只有映射成功的 unit 才设置 page→chunk 关系。
 */
static int pcpu_map_pages(struct pcpu_chunk *chunk,
			  struct page **pages, int page_start, int page_end)
{
	/* cpu 为正向提交游标，tcpu 为失败回滚游标，i 登记每页反查，err 保存 vmap errno。 */
	unsigned int cpu, tcpu;
	int i, err;

	/* 阶段 1：逐 unit 建 PTE；成功后再为该 unit 每页发布反向 chunk 指针。 */
	for_each_possible_cpu(cpu) {
		err = __pcpu_map_pages(pcpu_chunk_addr(chunk, cpu, page_start),
				       &pages[pcpu_page_idx(cpu, page_start)],
				       page_end - page_start);
		if (err < 0)
			goto err;

		/* PTE 已成功发布，再逐页绑定 chunk 身份，失败 unit 不会留下反查记录。 */
		for (i = page_start; i < page_end; i++)
			pcpu_set_page_chunk(pages[pcpu_page_idx(cpu, i)],
					    chunk);
	}
	return 0;
err:
	/* 阶段 2：回滚所有已尝试 unit（含可能部分建表的失败 unit），再统一失效旧 TLB。 */
	for_each_possible_cpu(tcpu) {
		__pcpu_unmap_pages(pcpu_chunk_addr(chunk, tcpu, page_start),
				   page_end - page_start);
		if (tcpu == cpu)
			break;
	}
	pcpu_post_unmap_tlb_flush(chunk, page_start, page_end);
	return err;
}

/**
 * pcpu_post_map_flush - flush cache after mapping
 * @chunk: pcpu_chunk the regions to be flushed belong to
 * @page_start: page index of the first page to be flushed
 * @page_end: page index of the last page to be flushed + 1
 *
 * Pages [@page_start,@page_end) of @chunk have been mapped.  Flush
 * cache.
 *
 * As with pcpu_pre_unmap_flush(), TLB flushing also is done at once
 * for the whole region.
 */
/*
 * 在所有 unit 映射成功后，一次性完成 vmap cache 后置刷新。
 * 入参：@chunk 与半开页区间定位刚发布的虚拟跨度。无直接返回；flush 后新 PAGE_KERNEL
 * 别名可安全读写。调用者持 pcpu_alloc_mutex；范围可能覆盖空洞以换取一次批量操作。
 */
static void pcpu_post_map_flush(struct pcpu_chunk *chunk,
				int page_start, int page_end)
{
	flush_cache_vmap(
		pcpu_chunk_addr(chunk, pcpu_low_unit_cpu, page_start),
		pcpu_chunk_addr(chunk, pcpu_high_unit_cpu, page_end));
}

/**
 * pcpu_populate_chunk - populate and map an area of a pcpu_chunk
 * @chunk: chunk of interest
 * @page_start: the start page
 * @page_end: the end page
 * @gfp: allocation flags passed to the underlying memory allocator
 *
 * For each cpu, populate and map pages [@page_start,@page_end) into
 * @chunk.
 *
 * CONTEXT:
 * pcpu_alloc_mutex, does GFP_KERNEL allocation.
 */
/*
 * 为 chunk 的一个逻辑页区间分配、映射所有 CPU backing 并完成 cache 发布。
 * 业务背景：percpu core 已在 bitmap 中保留逻辑区域，但只有非原子分配或 balance worker
 * 能在 pcpu_alloc_mutex 下补齐物理页；成功后 core 才标记 populated。
 * 入参：@chunk 是借用目标；@page_start/@page_end 为半开区间；@gfp 是底层分配策略。
 * 出参/返回：成功返回 0，所有 unit 均可访问；工作数组/页分配/映射失败统一返回 -ENOMEM。
 * map 失败会释放全部新页，未留下可访问映射；工作数组仍由后端长期持有。
 * 注意事项：必须持 pcpu_alloc_mutex、可做 GFP_KERNEL 类睡眠分配；本函数不更新 chunk 的
 * populated bitmap，那是 core 在返回成功并取得 pcpu_lock 后的发布职责。
 */
static int pcpu_populate_chunk(struct pcpu_chunk *chunk,
			       int page_start, int page_end, gfp_t gfp)
{
	/* pages 是 mutex 保护的共享工作数组，暂存当前批次物理页 ownership。 */
	struct page **pages;

	/* 阶段 1：取得工作区并为每个 unit 分配 NUMA 本地 backing。 */
	pages = pcpu_get_pages();
	if (!pages)
		return -ENOMEM;

	if (pcpu_alloc_pages(chunk, pages, page_start, page_end, gfp))
		return -ENOMEM;

	/* 阶段 2：建立映射和反查；失败时映射 helper 已清 PTE，本层释放物理页。 */
	if (pcpu_map_pages(chunk, pages, page_start, page_end)) {
		pcpu_free_pages(chunk, pages, page_start, page_end);
		return -ENOMEM;
	}
	/* 阶段 3：所有 PTE 完整后统一刷新 cache，返回给 core 作为可发布 backing。 */
	pcpu_post_map_flush(chunk, page_start, page_end);

	return 0;
}

/**
 * pcpu_depopulate_chunk - depopulate and unmap an area of a pcpu_chunk
 * @chunk: chunk to depopulate
 * @page_start: the start page
 * @page_end: the end page
 *
 * For each cpu, depopulate and unmap pages [@page_start,@page_end)
 * from @chunk.
 *
 * Caller is required to call pcpu_post_unmap_tlb_flush() if not returning the
 * region back to vmalloc() which will lazily flush the tlb.
 *
 * CONTEXT:
 * pcpu_alloc_mutex.
 */
/*
 * 撤销 chunk 指定逻辑页区间的全部 unit 映射并释放 backing。
 * 业务背景：percpu 回收只释放 bitmap 已确认完全空闲的 populated 页；本函数执行 VM 后端
 * cache flush→页反查→noflush unmap→物理释放，core 随后更新 populated 账本。
 * 入参：@chunk 是借用目标；@page_start/@page_end 为半开区间。出参/返回：无直接返回；
 * PTE 被撤销，物理页归还 buddy，虚拟区域仍保留给 chunk 将来重新 populate。
 * 注意事项：必须持 pcpu_alloc_mutex；调用者若不马上把 VM 区归还 vmalloc，必须稍后调用
 * pcpu_post_unmap_tlb_flush。至少一次成功 populate 保证共享工作数组已存在。
 */
static void pcpu_depopulate_chunk(struct pcpu_chunk *chunk,
				  int page_start, int page_end)
{
	/* pages 接收 unmap 前反查到的 backing，供随后 free。 */
	struct page **pages;

	/*
	 * If control reaches here, there must have been at least one
	 * successful population attempt so the temp pages array must
	 * be available now.
	 */
	/* 能到 depopulate 表示曾成功 populate；数组缺失是不可恢复的内部状态错误。 */
	pages = pcpu_get_pages();
	BUG_ON(!pages);

	/* unmap and free */
	/* 阶段 1：先处理 cache alias，再清 PTE 并保存 page 指针，最后释放 backing。 */
	pcpu_pre_unmap_flush(chunk, page_start, page_end);

	pcpu_unmap_pages(chunk, pages, page_start, page_end);

	pcpu_free_pages(chunk, pages, page_start, page_end);
}

/*
 * 创建一个只有虚拟地址保留、尚未 populate 物理页的 VM percpu chunk。
 * 业务背景：percpu core 无可用空间时调用；通用 metadata 描述分配 bitmap，VM 后端再按
 * group offsets/sizes 原子保留地址区，使各 unit 的固定偏移关系成立。
 * 入参：@gfp 是通用 chunk metadata 分配策略。出参/返回：成功返回 ownership 交给 core
 * 的 chunk；metadata 或 VM areas 分配失败返回 NULL 并完整回滚。
 * 注意事项：可睡眠，通常持 pcpu_alloc_mutex；成功后 chunk->data 持有 vms 数组，base_addr
 * 从第 0 group 地址减 offset 得到统一基址。尚无 backing/populated 页。
 */
static struct pcpu_chunk *pcpu_create_chunk(gfp_t gfp)
{
	/* chunk 是通用 metadata；vms 是每个 percpu group 的 vm_struct* 数组。 */
	struct pcpu_chunk *chunk;
	struct vm_struct **vms;

	/* 阶段 1：先建立 bitmap/槽位 metadata，失败无资源。 */
	chunk = pcpu_alloc_chunk(gfp);
	if (!chunk)
		return NULL;

	/* 阶段 2：按启动布局及 atom 对齐一次性保留所有 group 虚拟区。 */
	vms = pcpu_get_vm_areas(pcpu_group_offsets, pcpu_group_sizes,
				pcpu_nr_groups, pcpu_atom_size);
	if (!vms) {
		pcpu_free_chunk(chunk);
		return NULL;
	}

	/* VM areas 成功后转移给 chunk；base_addr 是 percpu 地址换算的统一原点。 */
	chunk->data = vms;
	chunk->base_addr = vms[0]->addr - pcpu_group_offsets[0];

	/* 对象已完整，最后提交统计/trace；core 返回后会把 chunk 加入槽链。 */
	pcpu_stats_chunk_alloc();
	trace_percpu_create_chunk(chunk->base_addr);

	return chunk;
}

/*
 * 销毁已完全 depopulate 的 VM chunk 及其地址保留和通用 metadata。
 * 业务背景：core 先逐区间释放所有 backing，再调用本函数归还稀缺 vmalloc 空间。
 * 入参：@chunk 为 core 转交 ownership 的可空指针。出参/返回：无直接返回；非 NULL 时
 * 撤销统计/trace，释放所有 group VM areas，最后释放 chunk；指针失效。
 * 注意事项：可睡眠，调用者持 pcpu_alloc_mutex 且已从共享槽链隔离；若 VM 区归还由
 * vmalloc lazy TLB flush 接管，不能仍有 populated 页或外部地址使用者。
 */
static void pcpu_destroy_chunk(struct pcpu_chunk *chunk)
{
	if (!chunk)
		return;

	/* 先记录逻辑销毁，再归还 VM areas；trace 仍可读取有效 base_addr。 */
	pcpu_stats_chunk_dealloc();
	trace_percpu_destroy_chunk(chunk->base_addr);

	/* data 在 VM 后端成功 create 后持有 vms；通用 alloc 的异常对象允许其为空。 */
	if (chunk->data)
		pcpu_free_vm_areas(chunk->data, pcpu_nr_groups);
	pcpu_free_chunk(chunk);
}

/*
 * 把已 populate 的动态 percpu 虚址转换为借用 struct page。
 * 入参：@addr 必须位于 VM chunk 的有效映射。返回 vmalloc backing page，不增加引用；
 * 无副作用、不睡眠。percpu core 随后用 page→chunk 反查定位 chunk。
 */
static struct page *pcpu_addr_to_page(void *addr)
{
	return vmalloc_to_page(addr);
}

/*
 * 验证启动期 pcpu_alloc_info 是否满足 VM 后端附加约束。
 * 业务背景：通用层已验证布局，默认 VM 后端能表达任意合法 group/atom 组合。
 * 入参：@ai 是借用只读启动布局。出参/返回：固定 0，无副作用。
 * 注意事项：仅 __init 阶段调用，不睡眠；与 KM 后端的连续 order 限制不同。
 */
static int __init pcpu_verify_alloc_info(const struct pcpu_alloc_info *ai)
{
	/* no extra restriction */
	/* 除通用校验外没有额外限制，所有合法布局均接受。 */
	return 0;
}

/**
 * pcpu_should_reclaim_chunk - determine if a chunk should go into reclaim
 * @chunk: chunk of interest
 *
 * This is the entry point for percpu reclaim.  If a chunk qualifies, it is then
 * isolated and managed in separate lists at the back of pcpu_slot: sidelined
 * and to_depopulate respectively.  The to_depopulate list holds chunks slated
 * for depopulation.  They no longer contribute to pcpu_nr_empty_pop_pages once
 * they are on this list.  Once depopulated, they are moved onto the sidelined
 * list which enables them to be pulled back in for allocation if no other chunk
 * can suffice the allocation.
 */
/*
 * 判断一个 VM chunk 是否应进入 percpu reclaim 的 to_depopulate 队列。
 * 业务背景：VM 后端能逐页归还 backing，balance worker 用本谓词挑选“空闲页足够多且系统
 * 仍有富余”的 chunk；隔离后先 depopulate，再放 sidelined 以便紧缺时重新用于分配。
 * 入参：@chunk 是在 pcpu_lock 下借用且槽链状态稳定的候选。
 * 出参/返回：first/reserved chunk 固定 false；已 isolated 且仍有 empty populated 页返回
 * true；普通 chunk 只有系统排除自身后仍超过高水位、且自身至少四分之一页为空时 true。
 * 无状态修改和 ownership 转移。
 * 注意事项：调用者必须持 pcpu_lock、不睡眠；返回只是瞬时决策，真正移动链表与 empty-page
 * 全局计数由 core 完成。该策略避免为回收一个 chunk 反而耗尽原子 percpu 分配备用页。
 */
static bool pcpu_should_reclaim_chunk(struct pcpu_chunk *chunk)
{
	/* do not reclaim either the first chunk or reserved chunk */
	/* 启动首块和显式 reserved chunk 承担永久/原子保障，绝不参与动态回收。 */
	if (chunk == pcpu_first_chunk || chunk == pcpu_reserved_chunk)
		return false;

	/*
	 * If it is isolated, it may be on the sidelined list so move it back to
	 * the to_depopulate list.  If we hit at least 1/4 pages empty pages AND
	 * there is no system-wide shortage of empty pages aside from this
	 * chunk, move it to the to_depopulate list.
	 */
	/*
	 * 已隔离的 sidelined chunk 若又积累空闲 populated 页，应移回待去填充队列。
	 * 普通 chunk 则要求自身空闲不少于总页数 1/4，并且从系统空页总量扣除它后仍高于
	 * PCPU_EMPTY_POP_PAGES_HIGH，确保回收不会打穿全局备用量。
	 */
	return ((chunk->isolated && chunk->nr_empty_pop_pages) ||
		(pcpu_nr_empty_pop_pages >
		 (PCPU_EMPTY_POP_PAGES_HIGH + chunk->nr_empty_pop_pages) &&
		 chunk->nr_empty_pop_pages >= chunk->nr_pages / 4));
}
