/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * scx_arena_pool: kernel-side sub-allocator over BPF-arena pages.
 *
 * Each chunk added to @sch->arena_pool comes from one
 * bpf_arena_alloc_pages_sleepable() call and is registered at the
 * kernel-side mapping address. Callers translate to the BPF-arena form
 * themselves if needed.
 *
 * Allocations grow the pool on demand. Underlying arena pages are released
 * when the arena map itself is torn down.
 *
 * Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2026 Tejun Heo <tj@kernel.org>
 */
/*
 * sched_ext 的 BPF arena 内核侧子分配器，接口背景见
 * Documentation/scheduler/sched-ext.rst。
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * scx_arena_pool 是叠加在 BPF arena 页之上的内核子分配器。加入
 * sch->arena_pool 的每个 chunk 都来自一次 bpf_arena_alloc_pages_sleepable()，并按内核
 * 映射地址登记；需要 BPF arena 地址形式的调用者自行转换。池不足时按需扩容，底层页在
 * arena map 本身拆除时释放。
 *
 * 两层 ownership 必须区分：gen_pool 只拥有“哪些字节区间已分配”的元数据，arena map
 * 拥有实际页。alloc 返回内核 VA 的子区间使用权，free 只归还区间；pool_destroy 为满足
 * gen_pool_destroy() 的一致性要求清空尚未归还的位图，却不逐页释放 arena。调度器实例的
 * 装载/RCU 延迟销毁协议保证 sch、map 和 pool 生命周期；destroy 前必须停止并发分配和使用。
 */
#include <linux/genalloc.h>

#include "internal.h"
#include "arena.h"

enum scx_arena_consts {
	SCX_ARENA_MIN_ORDER		= 3,	/* 8-byte minimum sub-allocation */
	/* 原文说明最小子分配为 8 字节；order=3 使 gen_pool 以 2^3 字节为一个位图单位。 */
	SCX_ARENA_GROW_PAGES		= 4,	/* per growth */
	/* 原文说明每次常规扩容 4 页；大请求会改用足以容纳自身的更多页。 */
};

/*
 * scx_arena_pool_init() - 为 scheduler 实例创建 arena 子分配池
 * @sch: 输入输出、不可为 NULL 的未发布/正在启用实例，调用者持有其生命周期 ownership。
 * 返回 0 表示未配置 arena 而无需创建，或 gen_pool 已写入 sch->arena_pool；返回 -ENOMEM
 * 表示池元数据分配失败。gen_pool_create() 可睡眠；本函数不分配 arena 页，失败不改变 map。
 */
s32 scx_arena_pool_init(struct scx_sched *sch)
{
	/* 未绑定 arena map 是合法配置，后续 alloc 以 NULL 表示该能力不可用。 */
	if (!sch->arena_map)
		return 0;

	sch->arena_pool = gen_pool_create(SCX_ARENA_MIN_ORDER, NUMA_NO_NODE);
	if (!sch->arena_pool)
		return -ENOMEM;
	return 0;
}

/*
 * scx_arena_clear_chunk() - 在销毁前清除一个 gen_pool chunk 的所有占用位
 * @pool: 输入输出、不可为 NULL 的目标池，销毁路径已排除并发 alloc/free。
 * @chunk: 输入输出的当前 chunk；start/end 为含首尾的内核 VA 范围，bits 中置位表示已分配。
 * @data: gen_pool 遍历器透传参数，当前未消费。
 * 返回：无直接返回值。按连续置位区间调用 gen_pool_free()，只清分配元数据，不释放 arena 页。
 */
static void scx_arena_clear_chunk(struct gen_pool *pool, struct gen_pool_chunk *chunk,
				  void *data)
{
	/*
	 * order 决定位号到字节偏移的换算；chunk_sz 含 end_addr 所在最后一个字节，end_bit
	 * 是有效位数。b/e 是每段 [b,e) 的置位边界，生命周期仅限本次遍历。
	 */
	int order = pool->min_alloc_order;
	size_t chunk_sz = chunk->end_addr - chunk->start_addr + 1;
	unsigned long end_bit = chunk_sz >> order;
	unsigned long b, e;

	/* 合并连续置位可用一次 free 清整段，避免逐 8 字节更新位图。 */
	for_each_set_bitrange(b, e, chunk->bits, end_bit)
		gen_pool_free(pool, chunk->start_addr + (b << order),
			      (e - b) << order);
}

/*
 * Tear down the pool. Outstanding gen_pool allocations are freed via
 * scx_arena_clear_chunk() so gen_pool_destroy() doesn't BUG. The underlying
 * arena pages are released when the arena map itself is torn down.
 */
/*
 * 拆除池时，先由 scx_arena_clear_chunk() 归还仍未释放的 gen_pool 分配，避免
 * gen_pool_destroy() 因非空池触发 BUG；底层 arena 页在 arena map 自身拆除时释放。
 *
 * scx_arena_pool_destroy() - 销毁 scheduler 的内核侧 arena 池元数据
 * @sch: 输入输出、不可为 NULL 的下线实例；所有 alloc/free 与地址使用必须已经停止。
 * 返回：无直接返回值。无池时幂等返回；否则清占用位、销毁 pool 并把指针置 NULL，防止
 * 后续误用/重复销毁。函数不释放 arena_map、不等待 reader，也不转移 sch ownership。
 */
void scx_arena_pool_destroy(struct scx_sched *sch)
{
	/* 无 arena 配置或初始化早期失败都可能没有 pool，cleanup 可直接收敛。 */
	if (!sch->arena_pool)
		return;
	/* 先满足 gen_pool_destroy() 的“没有 outstanding allocation”前置条件。 */
	gen_pool_for_each_chunk(sch->arena_pool, scx_arena_clear_chunk, NULL);
	gen_pool_destroy(sch->arena_pool);
	sch->arena_pool = NULL;
}

/*
 * Grow the pool by @page_cnt pages. bpf_arena_alloc_pages_sleepable() and
 * gen_pool_add() (which calls vzalloc(GFP_KERNEL)) require a sleepable
 * context.
 */
/*
 * 按 @page_cnt 页扩容；bpf_arena_alloc_pages_sleepable() 与会调用
 * vzalloc(GFP_KERNEL) 的 gen_pool_add() 都要求可睡眠上下文。
 *
 * scx_arena_grow() - 从 BPF arena 取得整页并作为内核 VA chunk 加入 gen_pool
 * @sch: 输入输出活动实例，arena_map/pool 必须同时有效；借用且不取得引用。
 * @page_cnt: 纯输入非零页数，决定新 chunk 字节大小。
 * 返回 0 表示页已登记；-EINVAL 表示两层对象不完整，-ENOMEM 表示 arena 页分配失败，或
 * 透传 gen_pool_add() 的负 errno。登记失败会归还刚取得的页，pool 保持调用前状态。
 */
static int scx_arena_grow(struct scx_sched *sch, u32 page_cnt)
{
	/*
	 * kern_vm_start 是 arena 的内核映射基址；uaddr32 保存 BPF/user arena 内的 32 位偏移；
	 * p 是页分配器返回的 arena 形式地址；ret 只承载 gen_pool_add() 结果。
	 */
	u64 kern_vm_start;
	u32 uaddr32;
	void *p;
	int ret;

	/* map 提供实际页，pool 管理子区间；缺少任一层都不能扩容。 */
	if (!sch->arena_map || !sch->arena_pool)
		return -EINVAL;

	/* NUMA_NO_NODE 不限定节点，flags=0；成功后新页暂由本函数负责登记或回滚。 */
	p = bpf_arena_alloc_pages_sleepable(sch->arena_map, NULL,
					    page_cnt, NUMA_NO_NODE, 0);
	if (!p)
		return -ENOMEM;

	uaddr32 = (u32)(unsigned long)p;
	/* arena.o, which defines these, is built only on MMU && 64BIT */
	/* 原文说明定义这些 helper 的 arena.o 仅在 MMU 且 64 位配置构建；另一分支是编译兜底。 */
#if defined(CONFIG_MMU) && defined(CONFIG_64BIT)
	kern_vm_start = bpf_arena_map_kern_vm_start(sch->arena_map);
#else
	kern_vm_start = 0;
#endif

	/* 把 arena 偏移加到内核映射基址，gen_pool 此后只向内核调用者返回 kernel VA。 */
	ret = gen_pool_add(sch->arena_pool, kern_vm_start + uaddr32,
			   page_cnt * PAGE_SIZE, NUMA_NO_NODE);
	if (ret) {
		/* 登记未发布，仍可用原 arena 地址精确归还全部新页，避免 map 泄漏。 */
		bpf_arena_free_pages_non_sleepable(sch->arena_map, p, page_cnt);
		return ret;
	}
	return 0;
}

/*
 * Allocate @size bytes from the arena pool. Returns kernel VA on success, NULL
 * on failure. May grow the pool via scx_arena_grow() which sleeps. Caller must
 * be in a GFP_KERNEL context.
 */
/*
 * 从 arena pool 分配 @size 字节；成功返回内核 VA，失败返回 NULL。池不足会经
 * 可睡眠的 scx_arena_grow() 扩容，因此调用者必须处于允许 GFP_KERNEL 的上下文。
 *
 * scx_arena_alloc() - 取得一个 arena 子区间的独占使用权
 * @sch: 输入输出活动实例；pool 生命周期由 scheduler 启停协议保证，函数不取得引用。
 * @size: 必须大于 0 的请求字节数；实际按 8 字节粒度占用，调用者必须保存原值供 free。
 * 返回非 NULL 内核 VA 表示区间已从 pool 标记占用；无池或扩容失败返回 NULL。失败不留下
 * 半登记 chunk；成功区间未清零保证不在此接口，函数可能睡眠。
 */
void *scx_arena_alloc(struct scx_sched *sch, size_t size)
{
	/* kern_va 为 gen_pool 的 0/内核地址结果；page_cnt 是本轮扩容页数。 */
	unsigned long kern_va;
	u32 page_cnt;

	/* 主动检查调用上下文，因为首次失败后的扩容会进入睡眠页分配/vzalloc。 */
	might_sleep();

	if (!sch->arena_pool)
		return NULL;

	/* 先尝试现有空洞；失败才扩容并重试，直到成功或某次 grow 报错。 */
	while (true) {
		kern_va = gen_pool_alloc(sch->arena_pool, size);
		if (kern_va)
			break;
		/* 常规至少扩 4 页；大对象按向上取整的页数一次扩到足够。 */
		page_cnt = max_t(u32, SCX_ARENA_GROW_PAGES,
				 (size + PAGE_SIZE - 1) >> PAGE_SHIFT);
		if (scx_arena_grow(sch, page_cnt))
			return NULL;
	}

	return (void *)kern_va;
}

/*
 * scx_arena_free() - 把 alloc 返回的子区间归还 gen_pool
 * @sch: 输入输出活动实例，借用且调用期间 pool 不得销毁。
 * @kern_va: scx_arena_alloc() 返回的内核 VA，可为 NULL；调用后不得再访问或重复释放。
 * @size: 必须与分配时请求值完全一致，否则会破坏 pool 位图。
 * 返回：无直接返回值。pool/地址缺失时幂等无操作；有效时只清子分配位，不清内容、不释放
 * 底层 arena 页。调用者负责排除该区间的并发 reader/writer。
 */
void scx_arena_free(struct scx_sched *sch, void *kern_va, size_t size)
{
	/* cleanup 路径允许未初始化 pool 或 NULL allocation，避免额外条件分支散落在调用者。 */
	if (sch->arena_pool && kern_va)
		gen_pool_free(sch->arena_pool, (unsigned long)kern_va, size);
}
