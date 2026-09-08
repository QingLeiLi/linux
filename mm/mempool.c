// SPDX-License-Identifier: GPL-2.0
/*
 *  memory buffer pool support. Such pools are mostly used
 *  for guaranteed, deadlock-free memory allocations during
 *  extreme VM load.
 *
 *  started by Ingo Molnar, Copyright (C) 2001
 *  debugging by David Rientjes, Copyright (C) 2015
 */
/*
 * 学习提示：mempool 先尝试普通分配，只有普通路径失败才消耗预留元素；
 * 因而它保证的是特定调用链能取得对象，而不是保证系统仍有通用内存。
 * curr_nr/min_nr 与 elements[] 由 pool->lock 保护，取出者拥有元素，归还时
 * 若池已满则直接交给底层 free 回调。销毁前外部必须停止全部并发使用者。
 */
#include <linux/fault-inject.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/kasan.h>
/* KASAN/SLUB 调试负责元素可访问状态，kmemleak 更新每次重新分配的调用栈。 */
#include <linux/kmemleak.h>
#include <linux/export.h>
#include <linux/mempool.h>
#include <linux/writeback.h>
#include "slab.h"

static DECLARE_FAULT_ATTR(fail_mempool_alloc);
static DECLARE_FAULT_ATTR(fail_mempool_alloc_bulk);

/*
 * 业务背景：为单个与批量分配注册 fault-injection 控制项。
 * 入参：无；出参：0 成功，负 errno 使 late_initcall 报告失败。
 * 注意事项：第二项失败会导致启动失败，因此无需回滚第一项 debugfs 节点。
 */
static int __init mempool_faul_inject_init(void)
{
	int error;

	error = PTR_ERR_OR_ZERO(fault_create_debugfs_attr("fail_mempool_alloc",
			NULL, &fail_mempool_alloc));
	if (error)
		return error;

	/* booting will fail on error return here, don't bother to cleanup */
	/* 译注：这里返回错误会令启动失败，所以不必为第一项做清理。 */
	return PTR_ERR_OR_ZERO(
		fault_create_debugfs_attr("fail_mempool_alloc_bulk", NULL,
		&fail_mempool_alloc_bulk));
}
late_initcall(mempool_faul_inject_init);

#ifdef CONFIG_SLUB_DEBUG_ON
/*
 * 业务背景：预留元素毒值损坏时打印邻近字节定位越界写。
 * 入参：池、元素、对象大小和首个坏字节；出参：仅输出诊断。
 * 注意事项：调用者持 pool 锁，打印快照中的 curr_nr 不需另取锁。
 */
static void poison_error(struct mempool *pool, void *element, size_t size,
			 size_t byte)
{
	const int nr = pool->curr_nr;
	const int start = max_t(int, byte - (BITS_PER_LONG / 8), 0);
	const int end = min_t(int, byte + (BITS_PER_LONG / 8), size);
	int i;

	pr_err("BUG: mempool element poison mismatch\n");
	pr_err("Mempool %p size %zu\n", pool, size);
	pr_err(" nr=%d @ %p: %s0x", nr, element, start > 0 ? "... " : "");
	/* 只打印坏点左右一个机器字，避免在损坏路径制造过量日志。 */
	for (i = start; i < end; i++)
		pr_cont("%x ", *(u8 *)(element + i));
	pr_cont("%s\n", end < size ? "..." : "");
	dump_stack();
}

/*
 * 业务背景：元素离开预留池前验证整块 free poison。
 * 入参：元素和精确可访问大小；出参：成功后改写为 POISON_INUSE。
 * 注意事项：发现首个坏字节即报告并返回，不把可疑对象再次标成 in-use。
 */
static void __check_element(struct mempool *pool, void *element, size_t size)
{
	u8 *obj = element;
	size_t i;

	for (i = 0; i < size; i++) {
		/* 末字节使用独立哨兵，可同时发现对象尾部被覆盖。 */
		u8 exp = (i < size - 1) ? POISON_FREE : POISON_END;

		if (obj[i] != exp) {
			poison_error(pool, element, size, i);
			return;
		}
	}
	memset(obj, POISON_INUSE, size);
}

/*
 * 业务背景：按底层分配器推导对象大小并执行 SLUB 调试检查。
 * 入参：池与刚取出的元素；出参：返回 void，可能输出损坏报告。
 * 注意事项：KASAN 会在对象内保存元数据，启用时必须跳过毒值检查。
 */
static void check_element(struct mempool *pool, void *element)
{
	/* Skip checking: KASAN might save its metadata in the element. */
	/* 译注：KASAN 可能复用元素内容保存元数据，不能把它误判为 poison 损坏。 */
	if (kasan_enabled())
		return;

	/* Mempools backed by slab allocator */
	/* 译注：slab/kmalloc 后端分别从 pool_data 取得大小或 cache。 */
	if (pool->free == mempool_kfree) {
		__check_element(pool, element, (size_t)pool->pool_data);
	} else if (pool->free == mempool_free_slab) {
		__check_element(pool, element, kmem_cache_size(pool->pool_data));
	} else if (pool->free == mempool_free_pages) {
		/* Mempools backed by page allocator */
		/* 译注：页分配后端的 pool_data 编码 order，高端内存需逐页临时映射。 */
		int order = (int)(long)pool->pool_data;

#ifdef CONFIG_HIGHMEM
		for (int i = 0; i < (1 << order); i++) {
			/* 每次映射单页，检查后立即 kunmap，避免跨迭代持有本地映射。 */
			struct page *page = (struct page *)element;
			void *addr = kmap_local_page(page + i);

			__check_element(pool, addr, PAGE_SIZE);
			kunmap_local(addr);
		}
#else
		/* 无 HIGHMEM 时整组连续页可经线性映射一次检查。 */
		void *addr = page_address((struct page *)element);

		__check_element(pool, addr, PAGE_SIZE << order);
#endif
	}
}

/*
 * 业务背景：元素进入预留数组前写入 free poison 与尾哨兵。
 * 入参：可写元素和非零大小；出参：整块内容被调试模式接管。
 * 注意事项：这是调试破坏性写入，只能用于当前没有调用者拥有的数据。
 */
static void __poison_element(void *element, size_t size)
{
	u8 *obj = element;

	memset(obj, POISON_FREE, size - 1);
	obj[size - 1] = POISON_END;
}

/*
 * 业务背景：按分配后端选择元素大小并写入 SLUB poison。
 * 入参：池与正要归还的元素；出参：返回 void。
 * 注意事项：与 check_element 对称，KASAN 启用时跳过，HIGHMEM 逐页映射。
 */
static void poison_element(struct mempool *pool, void *element)
{
	/* Skip poisoning: KASAN might save its metadata in the element. */
	/* 译注：避免覆盖 KASAN 放在对象中的检测元数据。 */
	if (kasan_enabled())
		return;

	/* Mempools backed by slab allocator */
	/* 译注：依据 alloc 回调识别后端，因为元素此刻正进入池。 */
	if (pool->alloc == mempool_kmalloc) {
		__poison_element(element, (size_t)pool->pool_data);
	} else if (pool->alloc == mempool_alloc_slab) {
		__poison_element(element, kmem_cache_size(pool->pool_data));
	} else if (pool->alloc == mempool_alloc_pages) {
		/* Mempools backed by page allocator */
		/* 译注：高端页不能直接线性访问，必须逐页 kmap_local。 */
		int order = (int)(long)pool->pool_data;

#ifdef CONFIG_HIGHMEM
		for (int i = 0; i < (1 << order); i++) {
			/* poison 生命周期限制在当前 kmap_local 窗口。 */
			struct page *page = (struct page *)element;
			void *addr = kmap_local_page(page + i);

			__poison_element(addr, PAGE_SIZE);
			kunmap_local(addr);
		}
#else
		/* 无 HIGHMEM 时按 order 对整个连续范围一次写 poison。 */
		void *addr = page_address((struct page *)element);

		__poison_element(addr, PAGE_SIZE << order);
#endif
	}
}
#else /* CONFIG_SLUB_DEBUG_ON */
/*
 * 业务背景：未启用启动即 SLUB 调试时消除 poison 成本。
 * 入参：保持相同 ABI；出参：无操作。
 * 注意事项：配置桩不读取参数，KASAN 生命周期仍由后续独立 helpers 维护。
 */
static inline void check_element(struct mempool *pool, void *element)
{
}
static inline void poison_element(struct mempool *pool, void *element)
{
}
#endif /* CONFIG_SLUB_DEBUG_ON */

/*
 * 业务背景：元素放入预留池时同步切换 KASAN 可访问状态。
 * 入参：池和元素；出参：true 才允许写入 elements[]。
 * 注意事项：KASAN 可拒绝重复/非法归还；未知自定义后端默认允许。
 */
static __always_inline bool kasan_poison_element(struct mempool *pool,
		void *element)
{
	if (pool->alloc == mempool_alloc_slab || pool->alloc == mempool_kmalloc)
		return kasan_mempool_poison_object(element);
	else if (pool->alloc == mempool_alloc_pages)
		return kasan_mempool_poison_pages(element,
						(unsigned long)pool->pool_data);
	return true;
}

/*
 * 业务背景：元素从预留池交给调用者前恢复 KASAN 可访问范围。
 * 入参：池和元素；出参：返回 void。
 * 注意事项：只识别标准 kmalloc/slab/page 后端，自定义后端不改 shadow。
 */
static void kasan_unpoison_element(struct mempool *pool, void *element)
{
	if (pool->alloc == mempool_kmalloc)
		kasan_mempool_unpoison_object(element, (size_t)pool->pool_data);
	else if (pool->alloc == mempool_alloc_slab)
		kasan_mempool_unpoison_object(element,
					      kmem_cache_size(pool->pool_data));
	/* 页后端按原 order 恢复整组页的 shadow。 */
	else if (pool->alloc == mempool_alloc_pages)
		kasan_mempool_unpoison_pages(element,
					     (unsigned long)pool->pool_data);
}

/*
 * 业务背景：把一个由调用者移交的元素发布到预留数组。
 * 入参：持 pool->lock 的池和元素；出参：curr_nr 可能加一。
 * 注意事项：非零 min_nr 时不可越界；KASAN 拒绝时元素不进入数组。
 */
static __always_inline void add_element(struct mempool *pool, void *element)
{
	BUG_ON(pool->min_nr != 0 && pool->curr_nr >= pool->min_nr);
	poison_element(pool, element);
	if (kasan_poison_element(pool, element))
		pool->elements[pool->curr_nr++] = element;
}

/*
 * 业务背景：从预留数组尾部取走一个元素供分配者独占。
 * 入参：持 pool->lock 且 curr_nr > 0；出参：返回元素并将 curr_nr 减一。
 * 注意事项：先恢复 KASAN/SLUB 可访问状态，返回后池不再拥有该元素。
 */
static void *remove_element(struct mempool *pool)
{
	void *element = pool->elements[--pool->curr_nr];

	BUG_ON(pool->curr_nr < 0);
	kasan_unpoison_element(pool, element);
	check_element(pool, element);
	return element;
}

/**
 * mempool_exit - exit a mempool initialized with mempool_init()
 * @pool:      pointer to the memory pool which was initialized with
 *             mempool_init().
 *
 * Free all reserved elements in @pool and @pool itself.  This function
 * only sleeps if the free_fn() function sleeps.
 *
 * May be called on a zeroed but uninitialized mempool (i.e. allocated with
 * kzalloc()).
 */
/*
 * 译注：释放所有预留元素与 elements 数组，但不释放嵌入式 pool 本身；
 * pool 可是清零未初始化状态。调用前必须排空并发分配者，free 回调可睡眠。
 */
void mempool_exit(struct mempool *pool)
{
	/* 每次 remove 都把数组 ownership 交回本函数，再交给底层 free。 */
	while (pool->curr_nr) {
		void *element = remove_element(pool);
		pool->free(element, pool->pool_data);
	}
	kfree(pool->elements);
	pool->elements = NULL;
}
EXPORT_SYMBOL(mempool_exit);

/**
 * mempool_destroy - deallocate a memory pool
 * @pool:      pointer to the memory pool which was allocated via
 *             mempool_create().
 *
 * Free all reserved elements in @pool and @pool itself.  This function
 * only sleeps if the free_fn() function sleeps.
 */
/*
 * 译注：销毁由 mempool_create 分配的独立 pool；NULL 是允许的空操作。
 * 它先执行 mempool_exit 再释放容器，调用者必须保证没有并发 resize/alloc/free。
 */
void mempool_destroy(struct mempool *pool)
{
	if (unlikely(!pool))
		return;

	mempool_exit(pool);
	kfree(pool);
}
EXPORT_SYMBOL(mempool_destroy);

/*
 * 业务背景：在指定 NUMA 节点初始化嵌入式 pool 并预充保底元素。
 * 入参：清零 pool、最小数、配对 alloc/free 回调、私有数据、GFP 与节点。
 * 出参：0 后 pool 可发布；-ENOMEM 时已清理部分元素，pool 可再次 exit。
 * 注意事项：min_nr 为 0 仍分配一个槽和一个元素以支持等待/归还边界。
 */
int mempool_init_node(struct mempool *pool, int min_nr,
		mempool_alloc_t *alloc_fn, mempool_free_t *free_fn,
		void *pool_data, gfp_t gfp_mask, int node_id)
{
	spin_lock_init(&pool->lock);
	pool->min_nr	= min_nr;
	pool->pool_data = pool_data;
	pool->alloc	= alloc_fn;
	pool->free	= free_fn;
	init_waitqueue_head(&pool->wait);
	/*
	 * max() used here to ensure storage for at least 1 element to support
	 * zero minimum pool
	 */
	/* 译注：即使最小值为零也保留一个指针槽，支持零池的特殊唤醒协议。 */
	pool->elements = kmalloc_array_node(max(1, min_nr), sizeof(void *),
					    gfp_mask, node_id);
	if (!pool->elements)
		return -ENOMEM;

	/*
	 * First pre-allocate the guaranteed number of buffers,
	 * also pre-allocate 1 element for zero minimum pool.
	 */
	/* 译注：先填满保底数量；零最小池也预充一个元素以建立可用初态。 */
	while (pool->curr_nr < max(1, pool->min_nr)) {
		void *element;

		element = pool->alloc(gfp_mask, pool->pool_data);
		/* 任一预充失败都经 exit 逆序归还已经取得的元素和数组。 */
		if (unlikely(!element)) {
			mempool_exit(pool);
			return -ENOMEM;
		}
		add_element(pool, element);
	}

	return 0;
}
EXPORT_SYMBOL(mempool_init_node);

/**
 * mempool_init - initialize a memory pool
 * @pool:      pointer to the memory pool that should be initialized
 * @min_nr:    the minimum number of elements guaranteed to be
 *             allocated for this pool.
 * @alloc_fn:  user-defined element-allocation function.
 * @free_fn:   user-defined element-freeing function.
 * @pool_data: optional private data available to the user-defined functions.
 *
 * Like mempool_create(), but initializes the pool in (i.e. embedded in another
 * structure).
 *
 * Return: %0 on success, negative error code otherwise.
 */
/*
 * 译注：为嵌入其他结构的 pool 提供默认 GFP_KERNEL/无 NUMA 节点包装；
 * 返回 0 或负 errno，成功后外层对象负责最终调用 mempool_exit。
 */
int mempool_init_noprof(struct mempool *pool, int min_nr,
		mempool_alloc_t *alloc_fn, mempool_free_t *free_fn,
		void *pool_data)
{
	return mempool_init_node(pool, min_nr, alloc_fn, free_fn,
				 pool_data, GFP_KERNEL, NUMA_NO_NODE);

}
EXPORT_SYMBOL(mempool_init_noprof);

/**
 * mempool_create_node - create a memory pool
 * @min_nr:    the minimum number of elements guaranteed to be
 *             allocated for this pool.
 * @alloc_fn:  user-defined element-allocation function.
 * @free_fn:   user-defined element-freeing function.
 * @pool_data: optional private data available to the user-defined functions.
 * @gfp_mask:  memory allocation flags
 * @node_id:   numa node to allocate on
 *
 * this function creates and allocates a guaranteed size, preallocated
 * memory pool. The pool can be used from the mempool_alloc() and mempool_free()
 * functions. This function might sleep. Both the alloc_fn() and the free_fn()
 * functions might sleep - as long as the mempool_alloc() function is not called
 * from IRQ contexts.
 *
 * Return: pointer to the created memory pool object or %NULL on error.
 */
/*
 * 译注：分配 pool 容器并调用 node 初始化；成功返回由调用者拥有的 pool，
 * 失败返回 NULL 且释放容器/已预充元素。回调是否可睡眠决定创建上下文约束。
 */
struct mempool *mempool_create_node_noprof(int min_nr,
		mempool_alloc_t *alloc_fn, mempool_free_t *free_fn,
		void *pool_data, gfp_t gfp_mask, int node_id)
{
	struct mempool *pool;

	pool = kmalloc_node_noprof(sizeof(*pool), gfp_mask | __GFP_ZERO, node_id);
	if (!pool)
		return NULL;

	/* 容器保持私有，只有内部初始化全部成功后才通过返回值发布。 */
	if (mempool_init_node(pool, min_nr, alloc_fn, free_fn, pool_data,
			      gfp_mask, node_id)) {
		kfree(pool);
		/* init_node 已清理内部资源，这里只需释放外层容器。 */
		return NULL;
	}

	return pool;
}
EXPORT_SYMBOL(mempool_create_node_noprof);

/**
 * mempool_resize - resize an existing memory pool
 * @pool:       pointer to the memory pool which was allocated via
 *              mempool_create().
 * @new_min_nr: the new minimum number of elements guaranteed to be
 *              allocated for this pool.
 *
 * This function shrinks/grows the pool. In the case of growing,
 * it cannot be guaranteed that the pool will be grown to the new
 * size immediately, but new mempool_free() calls will refill it.
 * This function may sleep.
 *
 * Note, the caller must guarantee that no mempool_destroy is called
 * while this function is running. mempool_alloc() & mempool_free()
 * might be called (eg. from IRQ contexts) while this function executes.
 *
 * Return: %0 on success, negative error code otherwise.
 */
/*
 * 译注：调整保底容量；缩小时锁外释放多余元素，增长时先换大数组再尽力补充。
 * 返回 -ENOMEM 只表示新数组未建立；数组发布后即使补充回调失败也返回 0，
 * 后续 mempool_free 会继续填充。允许 alloc/free 并发，不允许 destroy 并发。
 */
int mempool_resize(struct mempool *pool, int new_min_nr)
{
	void *element;
	void **new_elements;
	unsigned long flags;

	BUG_ON(new_min_nr <= 0);
	might_sleep();

	spin_lock_irqsave(&pool->lock, flags);
	if (new_min_nr <= pool->min_nr) {
		/* 缩容逐个摘除，锁外调用可能睡眠的 free，再重新检查共享状态。 */
		while (new_min_nr < pool->curr_nr) {
			element = remove_element(pool);
			spin_unlock_irqrestore(&pool->lock, flags);
			pool->free(element, pool->pool_data);
			spin_lock_irqsave(&pool->lock, flags);
		}
		pool->min_nr = new_min_nr;
		goto out_unlock;
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	/* Grow the pool */
	/* 译注：增长先在锁外分配新指针数组，避免持自旋锁睡眠。 */
	new_elements = kmalloc_objs(*new_elements, new_min_nr);
	if (!new_elements)
		return -ENOMEM;

	spin_lock_irqsave(&pool->lock, flags);
	if (unlikely(new_min_nr <= pool->min_nr)) {
		/* Raced, other resize will do our work */
		/* 译注：并发 resize 已达到目标，本次丢弃未发布数组并按成功返回。 */
		spin_unlock_irqrestore(&pool->lock, flags);
		kfree(new_elements);
		goto out;
	}
	memcpy(new_elements, pool->elements,
			pool->curr_nr * sizeof(*new_elements));
	kfree(pool->elements);
	pool->elements = new_elements;
	pool->min_nr = new_min_nr;
	/* 新数组与新下限在锁内一并发布，之后缺口允许暂时存在。 */

	while (pool->curr_nr < pool->min_nr) {
		spin_unlock_irqrestore(&pool->lock, flags);
		element = pool->alloc(GFP_KERNEL, pool->pool_data);
		/* 回调失败只停止主动补充；新的 min_nr 已生效，由后续 free 填平。 */
		if (!element)
			goto out;
		spin_lock_irqsave(&pool->lock, flags);
		if (pool->curr_nr < pool->min_nr) {
			add_element(pool, element);
		} else {
			spin_unlock_irqrestore(&pool->lock, flags);
			pool->free(element, pool->pool_data);	/* Raced */
			/* 译注：锁外分配期间别人已填满池，当前元素直接归还底层。 */
			goto out;
		}
	}
out_unlock:
	spin_unlock_irqrestore(&pool->lock, flags);
out:
	return 0;
}
EXPORT_SYMBOL(mempool_resize);

/*
 * 业务背景：普通分配失败后一次性从预留池补齐批量请求。
 * 入参：目标数组、总数、已分配数和睡眠能力；出参：新的 allocated 数。
 * 注意事项：持锁摘取保证全有或不取；可直接回收时在 waitqueue 周期睡眠。
 */
static unsigned int mempool_alloc_from_pool(struct mempool *pool, void **elems,
		unsigned int count, unsigned int allocated,
		gfp_t gfp_mask)
{
	unsigned long flags;
	unsigned int i;

	/* 锁内先确认剩余预留足以一次补齐，避免批量请求只取一部分。 */
	spin_lock_irqsave(&pool->lock, flags);
	if (unlikely(pool->curr_nr < count - allocated))
		goto fail;
	while (allocated < count)
		elems[allocated++] = remove_element(pool);
	/* 全批次已从池的 ownership 转给调用者，解锁后用屏障发布这一事实。 */
	spin_unlock_irqrestore(&pool->lock, flags);

	/* Paired with rmb in mempool_free(), read comment there. */
	/* 译注：与 free 的读屏障配对，保证元素发布先于观察更新后的 curr_nr。 */
	smp_wmb();

	/*
	 * Update the allocation stack trace as this is more useful for
	 * debugging.
	 */
	/* 译注：元素现在代表一次新分配，kmemleak 栈应更新为本次调用者。 */
	for (i = 0; i < count; i++)
		kmemleak_update_trace(elems[i]);
	return allocated;

fail:
	if (gfp_mask & __GFP_DIRECT_RECLAIM) {
		/* 可睡眠调用者登记不可中断等待者，再锁外等待归还或压力缓解。 */
		DEFINE_WAIT(wait);

		prepare_to_wait(&pool->wait, &wait, TASK_UNINTERRUPTIBLE);
		spin_unlock_irqrestore(&pool->lock, flags);

		/*
		 * Wait for someone else to return an element to @pool, but wake
		 * up occasionally as memory pressure might have reduced even
		 * and the normal allocation in alloc_fn could succeed even if
		 * no element was returned.
		 */
		/* 译注：定时醒来还会重试普通回调，因为内存压力可能已自行消退。 */
		io_schedule_timeout(5 * HZ);
		finish_wait(&pool->wait, &wait);
	} else {
		/* We must not sleep if __GFP_DIRECT_RECLAIM is not set. */
		/* 译注：原子/不可回收上下文只解锁并返回当前部分结果。 */
		spin_unlock_irqrestore(&pool->lock, flags);
	}

	return allocated;
}

/*
 * Adjust the gfp flags for mempool allocations, as we never want to dip into
 * the global emergency reserves or retry in the page allocator.
 *
 * The first pass also doesn't want to go reclaim, but the next passes do, so
 * return a separate subset for that first iteration.
 */
/*
 * 业务背景：约束 mempool 的普通分配尝试，避免它耗尽全局紧急储备。
 * 入参：调用者 GFP 的可写副本；出参：返回首轮非阻塞子集，并写回完整重试标志。
 * 注意事项：首轮去掉 DIRECT_RECLAIM/IO，后续重试才允许回收。
 */
static inline gfp_t mempool_adjust_gfp(gfp_t *gfp_mask)
{
	*gfp_mask |= __GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN;
	return *gfp_mask & ~(__GFP_DIRECT_RECLAIM | __GFP_IO);
}

/**
 * mempool_alloc_bulk - allocate multiple elements from a memory pool
 * @pool:	pointer to the memory pool
 * @elems:	partially or fully populated elements array
 * @count:	number of entries in @elem that need to be allocated
 *
 * Allocate @count elements into @elems.  This is done by first calling into the
 * alloc_fn supplied at pool initialization time, and dipping into the reserved
 * pool when alloc_fn fails to allocate an element.
 *
 * On return all @count elements in @elems will be populated.
 *
 * Return: Always 0.  If it wasn't for %$#^$ alloc tags, it would return void.
 */
/*
 * 译注：为 elems[0..count) 保证填满；先逐个调用 alloc 回调，失败后从预留
 * 池补齐并循环。count 必须不超过 min_nr，接口最终总返回 0，等待可无限期。
 */
int mempool_alloc_bulk_noprof(struct mempool *pool, void **elems,
		unsigned int count)
{
	gfp_t gfp_mask = GFP_KERNEL;
	gfp_t gfp_temp = mempool_adjust_gfp(&gfp_mask);
	unsigned int allocated = 0;

	VM_WARN_ON_ONCE(count > pool->min_nr);
	might_alloc(gfp_mask);

	/*
	 * If an error is injected, fail all elements in a bulk allocation so
	 * that we stress the multiple elements missing path.
	 */
	/* 译注：故障注入强制整批绕过普通分配，覆盖多元素同时缺失的预留路径。 */
	if (should_fail_ex(&fail_mempool_alloc_bulk, 1, FAULT_NOWARN)) {
		pr_info("forcing mempool usage for %pS\n",
				(void *)_RET_IP_);
		goto use_pool;
	}

repeat_alloc:
	/*
	 * Try to allocate the elements using the allocation callback first as
	 * that might succeed even when the caller's bulk allocation did not.
	 */
	/* 译注：每轮仍先试回调，因为等待期间普通内存可能恢复。 */
	while (allocated < count) {
		elems[allocated] = pool->alloc(gfp_temp, pool->pool_data);
		if (unlikely(!elems[allocated]))
			goto use_pool;
		allocated++;
	}

	return 0;

use_pool:
	/* 预留池可只返回当前可得部分；切换完整 GFP 后继续普通分配或等待。 */
	allocated = mempool_alloc_from_pool(pool, elems, count, allocated,
			gfp_temp);
	gfp_temp = gfp_mask;
	goto repeat_alloc;
}
EXPORT_SYMBOL_GPL(mempool_alloc_bulk_noprof);

/**
 * mempool_alloc - allocate an element from a memory pool
 * @pool:	pointer to the memory pool
 * @gfp_mask:	GFP_* flags.  %__GFP_ZERO is not supported.
 *
 * Allocate an element from @pool.  This is done by first calling into the
 * alloc_fn supplied at pool initialization time, and dipping into the reserved
 * pool when alloc_fn fails to allocate an element.
 *
 * This function only sleeps if the alloc_fn callback sleeps, or when waiting
 * for elements to become available in the pool.
 *
 * Return: pointer to the allocated element or %NULL when failing to allocate
 * an element.  Allocation failure can only happen when @gfp_mask does not
 * include %__GFP_DIRECT_RECLAIM.
 */
/*
 * 译注：先以受限 GFP 调底层 alloc，失败才取保底元素；允许直接回收时会
 * 重试/等待直到成功，不允许睡眠时可返回 NULL。__GFP_ZERO 不受支持，
 * 返回元素由调用者独占，并须使用 mempool_free 归还。
 */
void *mempool_alloc_noprof(struct mempool *pool, gfp_t gfp_mask)
{
	gfp_t gfp_temp = mempool_adjust_gfp(&gfp_mask);
	void *element;

	VM_WARN_ON_ONCE(gfp_mask & __GFP_ZERO);
	might_alloc(gfp_mask);

repeat_alloc:
	/* fault-injection 仅模拟普通回调失败，仍验证预留池的保证。 */
	if (should_fail_ex(&fail_mempool_alloc, 1, FAULT_NOWARN)) {
		pr_info("forcing mempool usage for %pS\n",
				(void *)_RET_IP_);
		element = NULL;
	} else {
		element = pool->alloc(gfp_temp, pool->pool_data);
	}

	if (unlikely(!element)) {
		/*
		 * Try to allocate an element from the pool.
		 *
		 * The first pass won't have __GFP_DIRECT_RECLAIM and won't
		 * sleep in mempool_alloc_from_pool.  Retry the allocation
		 * with all flags set in that case.
		 */
		/* 译注：首轮不回收；取池失败后启用完整 GFP，必要时等待归还并循环。 */
		if (!mempool_alloc_from_pool(pool, &element, 1, 0, gfp_temp)) {
			if (gfp_temp != gfp_mask) {
				/* 首次失败后允许调用者原本请求的直接回收能力。 */
				gfp_temp = gfp_mask;
				goto repeat_alloc;
			}
			if (gfp_mask & __GFP_DIRECT_RECLAIM) {
				/* 可睡眠者继续循环，等待路径会在池空时定时休眠。 */
				goto repeat_alloc;
			}
		}
	}

	return element;
}
EXPORT_SYMBOL(mempool_alloc_noprof);

/**
 * mempool_alloc_preallocated - allocate an element from preallocated elements
 *                              belonging to a memory pool
 * @pool:	pointer to the memory pool
 *
 * This function is similar to mempool_alloc(), but it only attempts allocating
 * an element from the preallocated elements. It only takes a single spinlock_t
 * and immediately returns if no preallocated elements are available.
 *
 * Return: pointer to the allocated element or %NULL if no elements are
 * available.
 */
/*
 * 译注：只在自旋锁内尝试取一个预分配元素，不调用 alloc、也不睡眠；
 * 成功返回由调用者拥有的元素，池空返回 NULL，适合严格非阻塞路径。
 */
void *mempool_alloc_preallocated(struct mempool *pool)
{
	void *element = NULL;

	mempool_alloc_from_pool(pool, &element, 1, 0, GFP_NOWAIT);
	return element;
}
EXPORT_SYMBOL(mempool_alloc_preallocated);

/**
 * mempool_free_bulk - return elements to a mempool
 * @pool:	pointer to the memory pool
 * @elems:	elements to return
 * @count:	number of elements to return
 *
 * Returns a number of elements from the start of @elem to @pool if @pool needs
 * replenishing and sets their slots in @elem to NULL.  Other elements are left
 * in @elem.
 *
 * Return: number of elements transferred to @pool.  Elements are always
 * transferred from the beginning of @elem, so the return value can be used as
 * an offset into @elem for the freeing the remaining elements in the caller.
 */
/*
 * 译注：从 elems 起始连续吸收池缺少的元素，并把已接收槽置空；返回转移数，
 * 其余元素仍归调用者。无锁预检配合 alloc 的写屏障，锁内再次确认容量；
 * 真正加入后才唤醒等待者，零最小池有独立的一元素握手路径。
 */
unsigned int mempool_free_bulk(struct mempool *pool, void **elems,
		unsigned int count)
{
	unsigned long flags;
	unsigned int freed = 0;
	bool added = false;

	/*
	 * Paired with the wmb in mempool_alloc().  The preceding read is
	 * for @element and the following @pool->curr_nr.  This ensures
	 * that the visible value of @pool->curr_nr is from after the
	 * allocation of @element.  This is necessary for fringe cases
	 * where @element was passed to this task without going through
	 * barriers.
	 *
	 * For example, assume @p is %NULL at the beginning and one task
	 * performs "p = mempool_alloc(...);" while another task is doing
	 * "while (!p) cpu_relax(); mempool_free(p, ...);".  This function
	 * may end up using curr_nr value which is from before allocation
	 * of @p without the following rmb.
	 */
	/*
	 * 译注：读屏障避免看见已发布 element，却读到它分配前的旧 curr_nr；
	 * 这样无锁预检不会漏掉必须回填并唤醒等待者的情形。
	 */
	smp_rmb();

	/*
	 * For correctness, we need a test which is guaranteed to trigger
	 * if curr_nr + #allocated == min_nr.  Testing curr_nr < min_nr
	 * without locking achieves that and refilling as soon as possible
	 * is desirable.
	 *
	 * Because curr_nr visible here is always a value after the
	 * allocation of @element, any task which decremented curr_nr below
	 * min_nr is guaranteed to see curr_nr < min_nr unless curr_nr gets
	 * incremented to min_nr afterwards.  If curr_nr gets incremented
	 * to min_nr after the allocation of @element, the elements
	 * allocated after that are subject to the same guarantee.
	 *
	 * Waiters happen iff curr_nr is 0 and the above guarantee also
	 * ensures that there will be frees which return elements to the
	 * pool waking up the waiters.
	 *
	 * For zero-minimum pools, curr_nr < min_nr (0 < 0) never succeeds,
	 * so waiters sleeping on pool->wait would never be woken by the
	 * wake-up path of previous test. This explicit check ensures the
	 * allocation of element when both min_nr and curr_nr are 0, and
	 * any active waiters are properly awakened.
	 */
	/*
	 * 译注：curr_nr < min_nr 的松散预检允许假阳性但不能漏掉“恰好欠一个”；
	 * min_nr=0 时该条件恒假，所以单独允许空池吸收一个元素并唤醒等待者。
	 */
	if (unlikely(READ_ONCE(pool->curr_nr) < pool->min_nr)) {
		spin_lock_irqsave(&pool->lock, flags);
		while (pool->curr_nr < pool->min_nr && freed < count) {
			/* add_element 接管 ownership；已转移槽稍后统一清为 NULL。 */
			add_element(pool, elems[freed++]);
			added = true;
		}
		spin_unlock_irqrestore(&pool->lock, flags);
	} else if (unlikely(pool->min_nr == 0 &&
		     READ_ONCE(pool->curr_nr) == 0)) {
		/* Handle the min_nr = 0 edge case: */
		/* 译注：零最小池只缓存一个元素，避免建立无法唤醒的等待状态。 */
		spin_lock_irqsave(&pool->lock, flags);
		if (likely(pool->curr_nr == 0)) {
			add_element(pool, elems[freed++]);
			added = true;
		}
		spin_unlock_irqrestore(&pool->lock, flags);
	}

	if (unlikely(added) && wq_has_sleeper(&pool->wait))
		/* 仅真实加入元素后唤醒，避免无资源的空转。 */
		wake_up(&pool->wait);

	return freed;
}
EXPORT_SYMBOL_GPL(mempool_free_bulk);

/**
 * mempool_free - return an element to the pool.
 * @element:	element to return
 * @pool:	pointer to the memory pool
 *
 * Returns @element to @pool if it needs replenishing, else frees it using
 * the free_fn callback in @pool.
 *
 * This function only sleeps if the free_fn callback sleeps.
 */
/*
 * 译注：NULL 是空操作；非 NULL 元素先尝试补足 pool，池已满则调用底层
 * free。调用后调用者不再拥有元素；是否睡眠只取决于 free 回调。
 */
void mempool_free(void *element, struct mempool *pool)
{
	if (likely(element) && !mempool_free_bulk(pool, &element, 1))
		pool->free(element, pool->pool_data);
}
EXPORT_SYMBOL(mempool_free);

/*
 * A commonly used alloc and free fn.
 */
/* 译注：以下是一组常用 slab 回调，pool_data 是 kmem_cache，禁止带 ctor。 */
/*
 * 业务背景：从指定 slab cache 分配池元素；入参为 GFP 和 cache。
 * 出参：成功返回元素，失败 NULL；注意事项：带 ctor 的 cache 不受支持。
 */
void *mempool_alloc_slab(gfp_t gfp_mask, void *pool_data)
{
	struct kmem_cache *mem = pool_data;
	VM_BUG_ON(mem->ctor);
	return kmem_cache_alloc_noprof(mem, gfp_mask);
}
EXPORT_SYMBOL(mempool_alloc_slab);

/* slab 配对释放回调：接收元素 ownership 并归还 pool_data 指定的 cache。 */
void mempool_free_slab(void *element, void *pool_data)
{
	struct kmem_cache *mem = pool_data;
	kmem_cache_free(mem, element);
}
EXPORT_SYMBOL(mempool_free_slab);

/*
 * A commonly used alloc and free fn that kmalloc/kfrees the amount of memory
 * specified by pool_data
 */
/* 译注：以下 kmalloc/kfree 回调把 pool_data 直接编码为字节大小。 */
/*
 * 业务背景：按 pool_data 大小执行 kmalloc；入参含 GFP。
 * 出参：元素或 NULL；注意事项：大小必须与同池 kfree 回调约定一致。
 */
void *mempool_kmalloc(gfp_t gfp_mask, void *pool_data)
{
	size_t size = (size_t)pool_data;
	return kmalloc_noprof(size, gfp_mask);
}
EXPORT_SYMBOL(mempool_kmalloc);

/* kmalloc 配对释放回调；pool_data 仅为 ABI 对称参数，此处无需读取。 */
void mempool_kfree(void *element, void *pool_data)
{
	kfree(element);
}
EXPORT_SYMBOL(mempool_kfree);

/*
 * A simple mempool-backed page allocator that allocates pages
 * of the order specified by pool_data.
 */
/* 译注：以下页后端把 pool_data 编码为 order，分配与释放必须严格配对。 */
/*
 * 业务背景：分配 2^order 个连续页作为池元素；入参含 GFP 与编码 order。
 * 出参：首 page 或 NULL；注意事项：调用者把 void 指针按 struct page 使用。
 */
void *mempool_alloc_pages(gfp_t gfp_mask, void *pool_data)
{
	int order = (int)(long)pool_data;
	return alloc_pages_noprof(gfp_mask, order);
}
EXPORT_SYMBOL(mempool_alloc_pages);

/* 页后端配对释放：以同一 order 归还从 alloc_pages 得到的首 page。 */
void mempool_free_pages(void *element, void *pool_data)
{
	int order = (int)(long)pool_data;
	__free_pages(element, order);
}
EXPORT_SYMBOL(mempool_free_pages);
