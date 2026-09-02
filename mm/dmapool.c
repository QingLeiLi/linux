// SPDX-License-Identifier: GPL-2.0-only
/*
 * DMA Pool allocator
 *
 * Copyright 2001 David Brownell
 * Copyright 2007 Intel Corporation
 *   Author: Matthew Wilcox <willy@linux.intel.com>
 *
 * This allocator returns small blocks of a given size which are DMA-able by
 * the given device.  It uses the dma_alloc_coherent page allocator to get
 * new pages, then splits them up into blocks of the required size.
 * Many older drivers still have their own code to do this.
 *
 * The current design of this allocator is fairly simple.  The pool is
 * represented by the 'struct dma_pool' which keeps a doubly-linked list of
 * allocated pages.  Each page in the page_list is split into blocks of at
 * least 'size' bytes.  Free blocks are tracked in an unsorted singly-linked
 * list of free blocks across all pages.  Used blocks aren't tracked, but we
 * keep a count of how many are currently allocated from each page.
 */
/*
 * 译注：本分配器面向设备提供可 DMA 的定长小块。它从 dma_alloc_coherent() 取得较大的
 * coherent backing，再按对齐和边界限制切成小块；dma_pool 以双向链表拥有所有 backing 页，
 * 所有页上的空闲块共用一条无序单链表，已借出块不另建索引，只在所属页之外汇总活动计数。
 * 这种设计替代了许多旧驱动各自实现的同类小块分配器。
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/export.h>
#include <linux/mutex.h>
/* poison 与 init-on-alloc/free 策略决定调试配置下的块内容状态。 */
#include <linux/poison.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/wait.h>

/*
 * 学习提示：dma_pool 用 coherent 大块承载定长小块，空闲块跨页串成单链表。
 * pool->lock 保护页/空闲链及计数，设备 pools 列表和 sysfs 发布由 mutex 串行。
 */
#ifdef CONFIG_SLUB_DEBUG_ON
#define DMAPOOL_DEBUG 1
#endif

struct dma_block {
	/* 空闲时块头存链指针与 DMA 地址；分配后整个块交给调用者。 */
	/* next_block 仅在空闲期有效，把本块链接到 pool 的 LIFO 空闲栈。 */
	struct dma_block *next_block;
	/* dma 是与块起始 CPU 地址配对的设备地址，alloc 时经 handle 输出。 */
	dma_addr_t dma;
};

struct dma_pool {		/* the pool */
	/* page_list 拥有 coherent 页包装，next_block 是跨页空闲栈。 */
	struct list_head page_list;
	/* lock 以 irqsave 方式保护 page_list、空闲栈及三个计数。 */
	spinlock_t lock;
	struct dma_block *next_block;
	/* nr_blocks 是全部 backing 可切出的块总数，单位为块。 */
	size_t nr_blocks;
	/* active 统计借出块，pages 统计 coherent backing 数。 */
	size_t nr_active;
	size_t nr_pages;
	/* dev 提供 DMA 映射上下文并拥有 dma_pools 发布链；pool 只借用其生命周期。 */
	struct device *dev;
	/* size/allocation/boundary 均以字节计，创建后只读。 */
	unsigned int size;
	unsigned int allocation;
	unsigned int boundary;
	/* boundary 限制块跨界，node 只约束元数据的 NUMA 分配。 */
	int node;
	/* name 是截断保存的诊断名；pools 把本对象链接到 dev->dma_pools。 */
	char name[32];
	struct list_head pools;
};

struct dma_page {		/* cacheable header for 'allocation' bytes */
	/* vaddr/dma 是同一 coherent allocation 的 CPU/设备双地址。 */
	/* page_list 受 pool->lock 保护，并由 pool 从初始化到 destroy 全程拥有。 */
	struct list_head page_list;
	void *vaddr;
	dma_addr_t dma;
};

/* pools_lock 保护所有 device->dma_pools 链的增删与 sysfs 遍历。 */
static DEFINE_MUTEX(pools_lock);
/* reg_lock 串行首个池的 sysfs 创建与末个池的移除。 */
static DEFINE_MUTEX(pools_reg_lock);

/*
 * 业务背景：设备的只读 pools 属性需要输出该设备当前注册 DMA 池的容量快照。
 * 入参：dev 是 sysfs 属性所属设备借用指针；attr 是未使用的属性描述；buf 是 PAGE_SIZE 输出缓冲区。
 * 出参/返回：返回已写字节数；按池输出名称、活动块、总块、块大小和 backing 页数，不转移对象所有权。
 * 注意事项：进程上下文可睡眠；pools_lock 排斥 create/destroy，单池计数仍可能被自旋锁侧并发更新而近似。
 */
static ssize_t pools_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	/* sysfs 快照在 pools_lock 下遍历，避免池并发注册或销毁。 */
	struct dma_pool *pool;
	unsigned size;

	/* size 同时是已写长度和下一次 emit 的偏移。 */
	size = sysfs_emit(buf, "poolinfo - 0.1\n");

	mutex_lock(&pools_lock);
	/* 每行读取锁下稳定的池身份与计数。 */
	list_for_each_entry(pool, &dev->dma_pools, pools) {
		/* per-pool info, no real statistics yet */
		size += sysfs_emit_at(buf, size, "%-16s %4zu %4zu %4u %2zu\n",
				      pool->name, pool->nr_active,
				      pool->nr_blocks, pool->size,
				      pool->nr_pages);
	}
	/* emit 完成后即可释放列表锁，返回长度不依赖池对象。 */
	mutex_unlock(&pools_lock);

	return size;
}

/* dev_attr_pools 是设备级只读属性，首个 pool 创建时发布、末个 pool 销毁时摘除。 */
static DEVICE_ATTR_RO(pools);

#ifdef DMAPOOL_DEBUG
/*
 * 业务背景：调试分配路径在块再次借出前检查 freed poison，以发现空闲期写入。
 * 入参：pool 是锁外稳定的池；block 是已从空闲栈弹出的独占块；mem_flags 决定是否稍后清零。
 * 出参/返回：无返回；损坏时打印首个异常和十六进制内容，未请求 init-on-alloc 时写 allocated poison。
 * 注意事项：只诊断而不拒绝分配；块头被空闲链字段覆盖，故检查从 sizeof(dma_block) 开始。
 */
static void pool_check_block(struct dma_pool *pool, struct dma_block *block,
			     gfp_t mem_flags)
{
	/* 块头供 allocator 使用，毒值检查从头部之后开始。 */
	u8 *data = (void *)block;
	int i;

	/* 首个非 freed poison 字节表示空闲期间发生写入。 */
	for (i = sizeof(struct dma_block); i < pool->size; i++) {
		if (data[i] == POOL_POISON_FREED)
			continue;
		dev_err(pool->dev, "%s %s, %p (corrupted)\n", __func__,
			pool->name, block);

		/*
		 * Dump the first 4 bytes even if they are not
		 * POOL_POISON_FREED
		 */
		/* 译注：即使块头前四字节本来就不是 freed poison，也仍从块首开始打印。 */
		/* 打印整个块，块头本就不要求保持 poison。 */
		print_hex_dump(KERN_ERR, "", DUMP_PREFIX_OFFSET, 16, 1,
				data, pool->size, 1);
		break;
	}

	/* init-on-alloc 稍后清零，否则以 allocated poison 标记借出。 */
	if (!want_init_on_alloc(mem_flags))
		memset(block, POOL_POISON_ALLOCATED, pool->size);
}

/*
 * 业务背景：debug free 必须先确认调用者给出的 DMA handle 属于当前池的某个 backing。
 * 入参：pool 是持 lock 的稳定池；dma 是待归还块的设备地址，纯输入。
 * 出参/返回：命中时返回借用 dma_page，未命中返回 NULL；不改变链表、引用或所有权。
 * 注意事项：page_list 无序且函数不自行加锁，调用者必须持 pool->lock 并保证 pool 未销毁。
 */
static struct dma_page *pool_find_page(struct dma_pool *pool, dma_addr_t dma)
{
	/* DMA 地址必须落入某个 backing 的半开范围。 */
	struct dma_page *page;

	/* page_list 未排序，只能在持 pool 锁时线性查找。 */
	list_for_each_entry(page, &pool->page_list, page_list) {
		if (dma < page->dma)
			continue;
		if ((dma - page->dma) < pool->allocation)
			return page;
	}
	return NULL;
}

/*
 * 业务背景：debug free 在重新发布块前拒绝坏 DMA 地址和重复释放，并建立下一轮 freed poison。
 * 入参：pool 是持 lock 的池；vaddr/dma 必须是同次 alloc 返回的 CPU/设备地址对。
 * 出参/返回：发现归属错误或 double free 返回 true 且不入链；合法时毒化块并返回 false。
 * 注意事项：线性扫描空闲栈；错误只报告且把块留在调用者之外，active 计数由上层保持不变。
 */
static bool pool_block_err(struct dma_pool *pool, void *vaddr, dma_addr_t dma)
{
	/* debug free 同时验证 DMA 归属和 double free。 */
	struct dma_block *block = pool->next_block;
	struct dma_page *page;

	/* 错误 DMA 直接拒绝入链，避免污染后续 handle。 */
	page = pool_find_page(pool, dma);
	if (!page) {
		dev_err(pool->dev, "%s %s, %p/%pad (bad dma)\n",
			__func__, pool->name, vaddr, &dma);
		return true;
	}

	/* 扫描空闲栈检测同一 CPU 地址是否已被归还。 */
	while (block) {
		if (block != vaddr) {
			block = block->next_block;
			continue;
		}
		dev_err(pool->dev, "%s %s, dma %pad already free\n",
			__func__, pool->name, &dma);
		return true;
	}

	/* 校验后先毒化，push 会重写块头链字段。 */
	memset(vaddr, POOL_POISON_FREED, pool->size);
	return false;
}

/*
 * 业务背景：debug 配置下新 coherent backing 在切块前应统一呈现“已释放”初态。
 * 入参：pool 提供 allocation 字节数；page 是尚未发布的新 backing 借用指针。
 * 出参/返回：无返回；把整个 CPU 映射填为 freed poison，不改变 DMA 映射 ownership。
 * 注意事项：仅在新页锁外准备阶段调用，后续切块会覆盖每个块头。
 */
static void pool_init_page(struct dma_pool *pool, struct dma_page *page)
{
	/* 新 coherent 页先整体置 freed poison。 */
	memset(page->vaddr, POOL_POISON_FREED, pool->allocation);
}
#else
/*
 * 业务背景：非调试配置不需要在分配时验证或写 poison，本桩保持统一调用结构。
 * 入参：pool、block、mem_flags 均仅为接口兼容且不使用。
 * 出参/返回：无返回、无副作用，块状态随后仅受通用 init-on-alloc 处理。
 * 注意事项：编译期替代调试实现，不提供空闲期越界/UAF 诊断。
 */
static void pool_check_block(struct dma_pool *pool, struct dma_block *block,
			     gfp_t mem_flags)
{
	/* 关闭调试时无毒值验证。 */
}

/*
 * 业务背景：非调试 free 仍需兑现全局 init-on-free 策略，再允许上层把块重新入链。
 * 入参：pool 给出块大小；vaddr 是独占待归还块；dma 在此配置下不校验。
 * 出参/返回：始终返回 false；启用 init-on-free 时清零整块，否则不改内容。
 * 注意事项：调用者持 pool->lock；本配置无法检测错池 DMA handle 或 double free。
 */
static bool pool_block_err(struct dma_pool *pool, void *vaddr, dma_addr_t dma)
{
	/* 非调试配置只兑现 init-on-free，始终允许归还。 */
	if (want_init_on_free())
		memset(vaddr, 0, pool->size);
	return false;
}

/*
 * 业务背景：非调试新页无需预填充，此桩使页初始化调用不含条件编译分叉。
 * 入参：pool/page 是未使用的借用指针。
 * 出参/返回：无返回、无副作用，coherent backing 原内容留待块分配策略处理。
 * 注意事项：仅配置桩，不改变 page 的发布时序。
 */
static void pool_init_page(struct dma_pool *pool, struct dma_page *page)
{
	/* 非调试新页无需预填充。 */
}
#endif

/*
 * 业务背景：alloc 快路径从跨页 LIFO 空闲栈取得一个独占块并同步活动计数。
 * 入参：pool 是持 irqsave lock 的有效池，空闲栈可为空。
 * 出参/返回：成功返回 ownership 已转给调用者的块并递增 nr_active；空栈返回 NULL 且无副作用。
 * 注意事项：不自行加锁、不清毒或输出 DMA handle；调用者解锁后完成这些步骤。
 */
static struct dma_block *pool_block_pop(struct dma_pool *pool)
{
	/* 调用者持自旋锁；pop 转移一个空闲块并增加 active。 */
	struct dma_block *block = pool->next_block;

	/* 空链不改变计数，由 alloc 慢路径扩充 backing。 */
	if (block) {
		pool->next_block = block->next_block;
		pool->nr_active++;
	}
	return block;
}

/*
 * 业务背景：合法 free 把调用者交还的块头重建为 allocator 元数据并发布到空闲栈。
 * 入参：pool 是持 lock 的池；block 是独占待发布块；dma 是与其配对的设备地址。
 * 出参/返回：无返回；以 LIFO 方式修改 next_block，ownership 转回 pool；不更新 nr_active。
 * 注意事项：写块头后调用者不得再访问；活动计数由紧邻的 free 上层在同一锁区减少。
 */
static void pool_block_push(struct dma_pool *pool, struct dma_block *block,
			    dma_addr_t dma)
{
	/* 以 LIFO 压栈，并恢复下次分配输出的 DMA handle。 */
	block->dma = dma;
	block->next_block = pool->next_block;
	pool->next_block = block;
}


/**
 * dma_pool_create_node - Creates a pool of coherent DMA memory blocks.
 * @name: name of pool, for diagnostics
 * @dev: device that will be doing the DMA
 * @size: size of the blocks in this pool.
 * @align: alignment requirement for blocks; must be a power of two
 * @boundary: returned blocks won't cross this power of two boundary
 * @node: optional NUMA node to allocate structs 'dma_pool' and 'dma_page' on
 * Context: not in_interrupt()
 *
 * Given one of these pools, dma_pool_alloc()
 * may be used to allocate memory.  Such memory will all have coherent
 * DMA mappings, accessible by the device and its driver without using
 * cache flushing primitives.  The actual size of blocks allocated may be
 * larger than requested because of alignment.
 *
 * If @boundary is nonzero, objects returned from dma_pool_alloc() won't
 * cross that size boundary.  This is useful for devices which have
 * addressing restrictions on individual DMA transfers, such as not crossing
 * boundaries of 4KBytes.
 *
 * Return: a dma allocation pool with the requested characteristics, or
 * %NULL if one can't be created.
 */
/*
 * 译注：创建一个由指定设备使用的 coherent DMA 小块池。name 用于诊断；size 是请求块大小；
 * align 必须为二次幂；非零 boundary 也必须为二次幂且返回块不会跨越它；node 只选择 pool/page
 * 元数据的 NUMA 节点。实际块可能因对齐而更大；成功返回可交给 dma_pool_alloc() 的池，失败返回 NULL。
 */
/*
 * 业务背景：驱动初始化用本入口把设备约束规范化并向 dev->dma_pools/sysfs 原子发布新池。
 * 入参：name/dev 为借用诊断名和设备；size/align/boundary 均为字节；node 是元数据目标 NUMA 节点。
 * 出参/返回：成功返回由调用者拥有的空池；非法约束、元数据或 sysfs 发布失败返回 NULL 并完整回滚。
 * 注意事项：不得在中断上下文，可睡眠；dev 生命周期必须覆盖 pool；destroy 前调用者须归还全部块。
 */
struct dma_pool *dma_pool_create_node(const char *name, struct device *dev,
		size_t size, size_t align, size_t boundary, int node)
{
	/* 参数规范化决定实际块步长、coherent allocation 和边界。 */
	struct dma_pool *retval;
	size_t allocation;
	bool empty;

	/* pool 必须绑定设备 DMA 上下文。 */
	if (!dev)
		return NULL;

	/* 零对齐退化为 1，其余必须为二次幂。 */
	if (align == 0)
		align = 1;
	else if (align & (align - 1))
		return NULL;

	/* 块至少容纳空闲链头，且内部规格字段可表示。 */
	if (size == 0 || size > INT_MAX)
		return NULL;
	if (size < sizeof(struct dma_block))
		size = sizeof(struct dma_block);

	/* 对齐后 size 是块步长，allocation 至少一页。 */
	size = ALIGN(size, align);
	allocation = max_t(size_t, size, PAGE_SIZE);

	/* 显式 boundary 必须为二次幂且至少容纳一块。 */
	if (!boundary)
		boundary = allocation;
	else if ((boundary < size) || (boundary & (boundary - 1)))
		return NULL;

	/* 大于 allocation 的边界无额外效果，夹紧简化分块。 */
	boundary = min(boundary, allocation);

	/* pool 元数据按 node 分配，coherent 页位置仍由 DMA API 决定。 */
	retval = kzalloc_node(sizeof(*retval), GFP_KERNEL, node);
	if (!retval)
		return retval;

	strscpy(retval->name, name, sizeof(retval->name));

	retval->dev = dev;

	/* 发布到设备列表前完成所有内部字段和锁初始化。 */
	INIT_LIST_HEAD(&retval->page_list);
	spin_lock_init(&retval->lock);
	retval->size = size;
	retval->boundary = boundary;
	retval->allocation = allocation;
	retval->node = node;
	INIT_LIST_HEAD(&retval->pools);

	/*
	 * pools_lock ensures that the ->dma_pools list does not get corrupted.
	 * pools_reg_lock ensures that there is not a race between
	 * dma_pool_create() and dma_pool_destroy() or within dma_pool_create()
	 * when the first invocation of dma_pool_create() failed on
	 * device_create_file() and the second assumes that it has been done (I
	 * know it is a short window).
	 */
	/*
	 * 译注：pools_lock 防止 dev->dma_pools 链损坏；pools_reg_lock 还排斥 create/destroy，
	 * 并关闭“第一次创建设备属性失败、第二次却误以为属性已存在”的短暂竞态窗口。
	 */
	/* create/destroy 固定使用 reg_lock→pools_lock 锁序。 */
	mutex_lock(&pools_reg_lock);
	mutex_lock(&pools_lock);
	/* 首个池负责创建设备 pools 属性。 */
	empty = list_empty(&dev->dma_pools);
	list_add(&retval->pools, &dev->dma_pools);
	mutex_unlock(&pools_lock);
	if (empty) {
		int err;

		/* sysfs 发布失败需撤销列表项并释放未返回对象。 */
		err = device_create_file(dev, &dev_attr_pools);
		if (err) {
			mutex_lock(&pools_lock);
			list_del(&retval->pools);
			mutex_unlock(&pools_lock);
			mutex_unlock(&pools_reg_lock);
			kfree(retval);
			return NULL;
		}
	}
	/* 返回前释放注册锁，此后调用者拥有已发布 pool。 */
	mutex_unlock(&pools_reg_lock);
	return retval;
}
EXPORT_SYMBOL(dma_pool_create_node);

/*
 * 业务背景：alloc 慢路径取得新 coherent backing 后，需按 size/boundary 切块并一次发布到池。
 * 入参：pool 是持 irqsave lock 的池；page 是尚未入 page_list、ownership 属于调用者的新 backing。
 * 出参/返回：无返回；创建全部合法块、接入空闲栈和页链并更新三个总量，page ownership 转给 pool。
 * 注意事项：必须持 pool->lock；尾部碎片和跨 boundary 的空洞不会成块，发布后由 destroy 统一释放。
 */
static void pool_initialise_page(struct dma_pool *pool, struct dma_page *page)
{
	/* 新页切成不跨 boundary 的块，再整体拼入空闲栈。 */
	unsigned int next_boundary = pool->boundary, offset = 0;
	struct dma_block *block, *first = NULL, *last = NULL;

	pool_init_page(pool, page);
	/* 仅创建完整块，尾部不足一块的空间留作内部碎片。 */
	while (offset + pool->size <= pool->allocation) {
		/* 会跨界时跳到下一边界起点。 */
		if (offset + pool->size > next_boundary) {
			offset = next_boundary;
			next_boundary += pool->boundary;
			continue;
		}

		/* CPU/DMA 地址使用相同 offset 构成地址对。 */
		block = page->vaddr + offset;
		block->dma = page->dma + offset;
		block->next_block = NULL;

		/* 先形成页内链，末尾再接旧全局空闲链。 */
		if (last)
			last->next_block = block;
		else
			first = block;
		last = block;

		offset += pool->size;
		pool->nr_blocks++;
	}

	/* allocation>=size 保证至少一个块，last 必然存在。 */
	last->next_block = pool->next_block;
	pool->next_block = first;

	/* 空闲块发布后登记 backing 和页计数。 */
	list_add(&page->page_list, &pool->page_list);
	pool->nr_pages++;
}

/*
 * 业务背景：空闲栈耗尽时在自旋锁外准备一个新 backing，避免睡眠式 DMA 分配阻塞池锁。
 * 入参：pool 提供设备、大小和 NUMA 节点；mem_flags 决定元数据/coherent 分配上下文。
 * 出参/返回：成功返回调用者拥有的 dma_page 及其 coherent 映射；失败返回 NULL 并释放已得包装。
 * 注意事项：可能睡眠；尚未修改 pool，成功对象必须随后发布或由调用者配对释放。
 */
static struct dma_page *pool_alloc_page(struct dma_pool *pool, gfp_t mem_flags)
{
	/* 此函数在锁外运行，按 GFP 语义分配包装和 coherent backing。 */
	struct dma_page *page;

	page = kmalloc_node(sizeof(*page), mem_flags, pool->node);
	if (!page)
		return NULL;

	/* DMA API 同时返回 CPU 地址和设备 handle。 */
	page->vaddr = dma_alloc_coherent(pool->dev, pool->allocation,
					 &page->dma, mem_flags);
	if (!page->vaddr) {
		kfree(page);
		return NULL;
	}

	return page;
}

/**
 * dma_pool_destroy - destroys a pool of dma memory blocks.
 * @pool: dma pool that will be destroyed
 * Context: !in_interrupt()
 *
 * Caller guarantees that no more memory from the pool is in use,
 * and that nothing will try to use the pool after this call.
 */
/*
 * 译注：销毁一个 DMA 内存块池；调用者保证池中已无在用内存，且返回后不会再有路径访问该池。
 * 调用上下文不得位于中断中。
 */
/*
 * 业务背景：驱动解绑时先从设备/sysfs 摘除池，再释放所有无活动借用的 coherent backing 和元数据。
 * 入参：pool 是调用者拥有的池，可为 NULL；调用开始后该 ownership 被消费。
 * 出参/返回：无返回；空闲池被完全释放；busy 池告警并泄漏 backing 以避免仍在用 DMA 地址变成 UAF。
 * 注意事项：可睡眠且不得在中断；调用者须排除 alloc/free/sysfs 外的后续使用，busy 不是可恢复错误返回。
 */
void dma_pool_destroy(struct dma_pool *pool)
{
	/* 调用者保证无并发使用；先撤销设备可见性，再处理 backing。 */
	struct dma_page *page, *tmp;
	bool empty, busy = false;

	if (unlikely(!pool))
		return;

	/* 与 create 相同锁序摘除，末个池移除 sysfs 属性。 */
	mutex_lock(&pools_reg_lock);
	mutex_lock(&pools_lock);
	list_del(&pool->pools);
	empty = list_empty(&pool->dev->dma_pools);
	mutex_unlock(&pools_lock);
	if (empty)
		device_remove_file(pool->dev, &dev_attr_pools);
	mutex_unlock(&pools_reg_lock);

	/* busy 说明仍有借出块：告警并保留 coherent 内存避免 UAF。 */
	if (pool->nr_active) {
		dev_err(pool->dev, "%s %s busy\n", __func__, pool->name);
		busy = true;
	}

	/* 包装总会释放；仅无活动块时释放 coherent backing。 */
	list_for_each_entry_safe(page, tmp, &pool->page_list, page_list) {
		if (!busy)
			dma_free_coherent(pool->dev, pool->allocation,
					  page->vaddr, page->dma);
		list_del(&page->page_list);
		/* dma_page 包装与 coherent backing 的释放条件刻意不同。 */
		kfree(page);
	}

	kfree(pool);
}
EXPORT_SYMBOL(dma_pool_destroy);

/**
 * dma_pool_alloc - get a block of coherent memory
 * @pool: dma pool that will produce the block
 * @mem_flags: GFP_* bitmask
 * @handle: pointer to dma address of block
 *
 * Return: the kernel virtual address of a currently unused block,
 * and reports its dma address through the handle.
 * If such a memory block can't be allocated, %NULL is returned.
 */
/*
 * 译注：从 pool 取得一个当前未使用的 coherent 块；mem_flags 是 GFP 掩码，handle 接收配对 DMA 地址。
 * 成功返回内核虚拟地址，无法取得块时返回 NULL。
 */
/*
 * 业务背景：驱动数据路径优先复用空闲块，空栈时在锁外扩充 backing，再返回 CPU/DMA 地址对。
 * 入参：pool 是稳定借用池；mem_flags 指定可睡眠/清零语义；handle 是非空 DMA 地址输出指针。
 * 出参/返回：成功返回调用者独占块并写 *handle、nr_active 已增加；失败 NULL 且不改变输出 ownership。
 * 注意事项：可否睡眠由 mem_flags 决定；同一块必须以原 pool、返回指针和 handle 配对 free。
 */
void *dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags,
		     dma_addr_t *handle)
{
	/* 快路径在 IRQ-safe 锁下从全局空闲栈弹出。 */
	struct dma_block *block;
	struct dma_page *page;
	unsigned long flags;

	/* 校验调用上下文能否满足 mem_flags 的睡眠属性。 */
	might_alloc(mem_flags);

	spin_lock_irqsave(&pool->lock, flags);
	block = pool_block_pop(pool);
	/* 空栈需锁外分配 coherent 页，避免持自旋锁睡眠。 */
	if (!block) {
		/*
		 * pool_alloc_page() might sleep, so temporarily drop
		 * &pool->lock
		 */
		/* 译注：pool_alloc_page() 可能睡眠，所以分配期间临时释放 pool->lock。 */
		spin_unlock_irqrestore(&pool->lock, flags);

		/* 整页不清零，块级 init-on-alloc 在返回前统一处理。 */
		page = pool_alloc_page(pool, mem_flags & (~__GFP_ZERO));
		if (!page)
			return NULL;

		spin_lock_irqsave(&pool->lock, flags);
		/* 重新加锁发布新页，再立即 pop 本次所需块。 */
		pool_initialise_page(pool, page);
		block = pool_block_pop(pool);
	}
	spin_unlock_irqrestore(&pool->lock, flags);

	/* 解锁后块已独占，输出 handle 并执行调试/清零策略。 */
	*handle = block->dma;
	pool_check_block(pool, block, mem_flags);
	if (want_init_on_alloc(mem_flags))
		memset(block, 0, pool->size);

	return block;
}
EXPORT_SYMBOL(dma_pool_alloc);

/**
 * dma_pool_free - put block back into dma pool
 * @pool: the dma pool holding the block
 * @vaddr: virtual address of block
 * @dma: dma address of block
 *
 * Caller promises neither device nor driver will again touch this block
 * unless it is first re-allocated.
 */
/*
 * 译注：把 CPU 地址 vaddr 与设备地址 dma 所表示的同一个块归还 pool；调用者承诺在该块重新分配前，
 * 驱动和设备都不会再次访问它。
 */
/*
 * 业务背景：DMA 完成且 CPU/设备均停止访问后，将块恢复为 allocator 元数据并重新发布到空闲栈。
 * 入参：pool 是原分配池；vaddr/dma 是 dma_pool_alloc() 返回的精确地址对，ownership 随调用交还。
 * 出参/返回：无返回；合法块入栈且 nr_active 减一；debug 检测错误时仅报告并保持计数。
 * 注意事项：可在与 irqsave 锁兼容的上下文；重复释放、错池或错 handle 会破坏非调试配置，调用后禁用旧地址。
 */
void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t dma)
{
	/* CPU/DMA 地址对必须来自同一次 alloc。 */
	struct dma_block *block = vaddr;
	unsigned long flags;

	spin_lock_irqsave(&pool->lock, flags);
	/* 仅校验成功才重新入栈并减少 active。 */
	if (!pool_block_err(pool, vaddr, dma)) {
		pool_block_push(pool, block, dma);
		pool->nr_active--;
	}
	spin_unlock_irqrestore(&pool->lock, flags);
}
EXPORT_SYMBOL(dma_pool_free);

/*
 * Managed DMA pool
 */
/* 译注：以下是由 devres 随设备解绑自动清理的托管 DMA pool 接口。 */
/*
 * 业务背景：devres 释放阶段需把资源槽中的 pool ownership 交给普通 destroy 路径。
 * 入参：dev 是触发解绑的借用设备且未使用；res 指向保存 dma_pool 指针的 devres 私有槽。
 * 出参/返回：无返回；销毁 pool 并消费其 ownership，资源槽本体由 devres 核心释放。
 * 注意事项：运行于 devres 清理上下文、可睡眠；调用前驱动必须停止所有块与 DMA 使用。
 */
static void dmam_pool_release(struct device *dev, void *res)
{
	/* devres 槽保存 pool 指针，解绑时调用普通 destroy。 */
	struct dma_pool *pool = *(struct dma_pool **)res;

	dma_pool_destroy(pool);
}

/*
 * 业务背景：显式 dmam_pool_destroy() 需要在设备 devres 链中按 pool 身份找到准确资源。
 * 入参：dev 是未使用的借用设备；res 是候选资源槽；match_data 是目标 pool 指针。
 * 出参/返回：槽中指针相同返回 1，否则返回 0；不修改资源、引用或 ownership。
 * 注意事项：由 devres 核心在其同步规则下调用，只比较身份而不解引用 pool 内容。
 */
static int dmam_pool_match(struct device *dev, void *res, void *match_data)
{
	/* managed destroy 按 pool 指针身份匹配资源。 */
	return *(struct dma_pool **)res == match_data;
}

/**
 * dmam_pool_create - Managed dma_pool_create()
 * @name: name of pool, for diagnostics
 * @dev: device that will be doing the DMA
 * @size: size of the blocks in this pool.
 * @align: alignment requirement for blocks; must be a power of two
 * @allocation: returned blocks won't cross this boundary (or zero)
 *
 * Managed dma_pool_create().  DMA pool created with this function is
 * automatically destroyed on driver detach.
 *
 * Return: a managed dma allocation pool with the requested
 * characteristics, or %NULL if one can't be created.
 */
/*
 * 译注：这是 dma_pool_create() 的托管版本；name/dev/size/align/allocation 与普通创建接口含义相同，
 * 成功池会在驱动解绑时自动销毁，失败返回 NULL。
 */
/*
 * 业务背景：驱动 probe 用 devres 把 pool 生命周期绑定到设备，省去各失败出口和 detach 的手工销毁。
 * 入参：name/dev 是借用对象；size/align/allocation 均为字节，约束传给普通 create。
 * 出参/返回：成功返回仍可供驱动借用的托管 pool；任一槽/pool 分配失败返回 NULL 且不留 devres。
 * 注意事项：可睡眠；成功后的最终 ownership 属于 devres，若提前销毁必须调用配对 dmam 接口。
 */
struct dma_pool *dmam_pool_create(const char *name, struct device *dev,
				  size_t size, size_t align, size_t allocation)
{
	/* 先申请 devres 槽，再创建 pool，成功后一并发布。 */
	struct dma_pool **ptr, *pool;

	ptr = devres_alloc(dmam_pool_release, sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return NULL;

	/* 失败只释放槽；成功由 devres 接管最终销毁。 */
	pool = *ptr = dma_pool_create(name, dev, size, align, allocation);
	if (pool)
		devres_add(dev, ptr);
	else
		devres_free(ptr);

	return pool;
}
EXPORT_SYMBOL(dmam_pool_create);

/**
 * dmam_pool_destroy - Managed dma_pool_destroy()
 * @pool: dma pool that will be destroyed
 *
 * Managed dma_pool_destroy().
 */
/* 译注：这是 dma_pool_destroy() 的托管版本，用于在设备解绑前显式销毁已登记的托管 pool。 */
/*
 * 业务背景：驱动主动拆除托管 pool 时，让 devres 原子摘除资源并调用同一个 release 回调。
 * 入参：pool 是先前 dmam_pool_create() 返回的托管池，函数通过 pool->dev 定位资源链。
 * 出参/返回：无返回；匹配成功即销毁并消费 pool ownership，匹配失败触发 WARN。
 * 注意事项：可睡眠；不得传普通 pool、重复销毁或在仍有活动 DMA 块时调用。
 */
void dmam_pool_destroy(struct dma_pool *pool)
{
	/* release 同时移除 devres 并销毁池，WARN 暴露匹配失败。 */
	struct device *dev = pool->dev;

	WARN_ON(devres_release(dev, dmam_pool_release, dmam_pool_match, pool));
}
EXPORT_SYMBOL(dmam_pool_destroy);
