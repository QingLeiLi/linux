// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (C) 2020 Google LLC
 */
#include <linux/cma.h>
#include <linux/debugfs.h>
#include <linux/dma-map-ops.h>
#include <linux/dma-direct.h>
#include <linux/init.h>
#include <linux/genalloc.h>
#include <linux/set_memory.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/*
 * 三个 gen_pool 分别承载 GFP_DMA、GFP_DMA32 与普通内核 zone 候选块；指针在 postcore 初始化后
 * 只读，pool 内部位图仍可并发分配/归还。对应 size 统计累计成功扩入的字节数，只由初始化或单条
 * 扩容 work 更新，debugfs 只读展示。某个 zone 不存在或初始化失败时其指针可保持 NULL。
 */
static struct gen_pool *atomic_pool_dma __ro_after_init;
static unsigned long pool_size_dma;
static struct gen_pool *atomic_pool_dma32 __ro_after_init;
static unsigned long pool_size_dma32;
static struct gen_pool *atomic_pool_kernel __ro_after_init;
static unsigned long pool_size_kernel;

/* Size can be defined by the coherent_pool command line */
/* coherent_pool= 可覆盖的初始容量/低水位；为 0 时 initcall 根据总 RAM 计算默认值。 */
static size_t atomic_pool_size;

/* Dynamic background expansion when the atomic pool is near capacity */
/* 原子分配热路径只调度该静态 work；workqueue 合并重复调度，并在可睡眠上下文实际扩容。 */
static struct work_struct atomic_pool_work;

/*
 * 解析早期启动参数 coherent_pool=。
 *
 * @p: 带 K/M/G 等可选后缀的容量字符串。
 * 返回值: early_param 约定的 0；解析结果写入 atomic_pool_size。
 *
 * 在早期启动串行阶段调用，不分配池、不校验是否能实际满足，后续 initcall 才执行分配。
 */
static int __init early_coherent_pool(char *p)
{
	atomic_pool_size = memparse(p, &p);
	return 0;
}
/* 把 coherent_pool= 注册为早期参数，保证 postcore initcall 前已确定目标容量。 */
early_param("coherent_pool", early_coherent_pool);

/*
 * 为三类原子池建立只读 debugfs 容量视图。
 *
 * 创建根目录 `dma_pools`，并导出 DMA/DMA32/kernel 累计字节数；debugfs 失败按其诊断接口惯例忽略，
 * 不影响池本身工作。函数仅在 initcall 串行上下文运行，可睡眠，dentry 生命周期由 debugfs 持有。
 */
static void __init dma_atomic_pool_debugfs_init(void)
{
	struct dentry *root;

	root = debugfs_create_dir("dma_pools", NULL);
	debugfs_create_ulong("pool_size_dma", 0400, root, &pool_size_dma);
	debugfs_create_ulong("pool_size_dma32", 0400, root, &pool_size_dma32);
	debugfs_create_ulong("pool_size_kernel", 0400, root, &pool_size_kernel);
}

/*
 * 按扩容使用的 zone 标志累加对应池容量统计。
 *
 * @gfp: 本次扩容的 zone 选择。
 * @size: 成功加入 gen_pool 的字节数。
 *
 * DMA 优先于 DMA32 分类，其余记入 kernel。只在启动初始化或同一静态 work 中调用，不使用原子量；
 * debugfs 并发读可能看到某次加法前后的任一稳定值，结果仅用于观测。
 */
static void dma_atomic_pool_size_add(gfp_t gfp, size_t size)
{
	if (gfp & __GFP_DMA)
		pool_size_dma += size;
	else if (gfp & __GFP_DMA32)
		pool_size_dma32 += size;
	else
		pool_size_kernel += size;
}

/*
 * 判断默认 CMA 区是否整体位于请求的 DMA zone 内。
 *
 * @gfp: 目标 zone 标志。
 * 返回值: 无默认 CMA/空区域为 false；DMA/DMA32 请求按区域末地址检查，普通请求为 true。
 *
 * CMA 区不能跨 zone，故验证末端即可判定整区归属；DMA32 上限取 32 位与 zone_dma_limit 较大者以
 * 适配特殊体系结构。纯查询不取得页面，CMA 描述在系统期稳定，本函数不睡眠。
 */
static bool cma_in_zone(gfp_t gfp)
{
	unsigned long size;
	phys_addr_t end;
	struct cma *cma;

	cma = dev_get_cma_area(NULL);
	if (!cma)
		return false;

	size = cma_get_size(cma);
	if (!size)
		return false;

	/* CMA can't cross zone boundaries, see cma_activate_area() */
	/* CMA 激活保证区域不跨 zone，因此用最后一字节与相应 zone 上限比较即可。 */
	end = cma_get_base(cma) + size - 1;
	if (IS_ENABLED(CONFIG_ZONE_DMA) && (gfp & GFP_DMA))
		return end <= zone_dma_limit;
	if (IS_ENABLED(CONFIG_ZONE_DMA32) && (gfp & GFP_DMA32))
		return end <= max(DMA_BIT_MASK(32), zone_dma_limit);
	return true;
}

/*
 * 为一个原子 DMA gen_pool 增加一块连续、coherent 且未加密的后备内存。
 *
 * @pool: 已创建的目标 gen_pool。
 * @pool_size: 期望扩容字节数；会被压到伙伴系统最大 order，并在失败时逐阶减半。
 * @gfp: 目标 zone 与可睡眠分配标志。
 * 返回值: 正常加入池为 0；取页、remap、解密等失败返回负错误。当前源码在 gen_pool_add_virt()
 * 失败后用 set_memory_encrypted() 的结果覆盖原错误，因此“重新加密成功”会返回 0，虽未扩入新块；
 * 这是本版本的可观测边界，注释不替代码修正。
 *
 * 每阶先在默认 CMA 完全位于目标 zone 时尝试 CMA，再退伙伴系统。取得页后准备缓存，可选建立
 * dma-coherent 解密映射，并把永久线性别名切为 decrypted，最后发布虚拟/物理对应关系。失败路径按
 * 已完成阶段尝试重新加密；DMA_DIRECT_REMAP 配置还会撤销映射并释放页，无 remap 配置按当前实现
 * 保留未发布块。若解密成功但重新加密失败则明确有意泄漏，绝不把属性不确定的页交回分配器。
 * 函数在 init/workqueue 可睡眠上下文串行执行，不与原子分配热路径持同一锁。
 */
static int atomic_pool_expand(struct gen_pool *pool, size_t pool_size,
			      gfp_t gfp)
{
	unsigned int order;
	struct page *page = NULL;
	void *addr;
	int ret = -ENOMEM;

	/* Cannot allocate larger than MAX_PAGE_ORDER */
	/* 单块不能超过伙伴系统允许的最大页阶；更大的扩容目标先截到该上限。 */
	order = min(get_order(pool_size), MAX_PAGE_ORDER);

	do {
		pool_size = 1 << (PAGE_SHIFT + order);
		if (cma_in_zone(gfp))
			page = dma_alloc_from_contiguous(NULL, 1 << order,
							 order, false);
		if (!page)
			page = alloc_pages(gfp | __GFP_NOWARN, order);
	} while (!page && order-- > 0);
	if (!page)
		goto out;

	arch_dma_prep_coherent(page, pool_size);

#ifdef CONFIG_DMA_DIRECT_REMAP
	addr = dma_common_contiguous_remap(page, pool_size,
			pgprot_decrypted(pgprot_dmacoherent(PAGE_KERNEL)),
			__builtin_return_address(0));
	if (!addr)
		goto free_page;
#else
	addr = page_to_virt(page);
#endif
	/*
	 * Memory in the atomic DMA pools must be unencrypted, the pools do not
	 * shrink so no re-encryption occurs in dma_direct_free().
	 */
	/*
	 * 原子 DMA 池必须永久使用未加密内存；池只增长不收缩，正常子块 free 只回位图，因此不会经过
	 * dma_direct_free() 做重新加密。这里只在发布到 gen_pool 前完成一次属性切换。
	 */
	ret = set_memory_decrypted((unsigned long)page_to_virt(page),
				   1 << order);
	if (ret)
		goto remove_mapping;
	ret = gen_pool_add_virt(pool, (unsigned long)addr, page_to_phys(page),
				pool_size, NUMA_NO_NODE);
	if (ret)
		goto encrypt_mapping;

	dma_atomic_pool_size_add(gfp, pool_size);
	return 0;

encrypt_mapping:
	ret = set_memory_encrypted((unsigned long)page_to_virt(page),
				   1 << order);
	/* 此赋值覆盖 gen_pool_add_virt() 原错误；重新加密成功时 ret 变成 0，但该块并未加入 pool。 */
	if (WARN_ON_ONCE(ret)) {
		/* Decrypt succeeded but encrypt failed, purposely leak */
		/* 解密已成功但恢复加密失败时故意泄漏整块页，避免错误属性页面进入普通分配器。 */
		goto out;
	}
remove_mapping:
#ifdef CONFIG_DMA_DIRECT_REMAP
	dma_common_free_remap(addr, pool_size);
free_page:
	__free_pages(page, order);
#endif
out:
	return ret;
}

/*
 * 在池可用容量低于低水位时尝试扩容。
 *
 * @pool: 可为 NULL 的某 zone 池。
 * @gfp: 该池对应的 zone 分配标志。
 *
 * 每次以当前总池大小作为新增目标，效果近似翻倍；扩容失败只保留原池，不向 work 调用者传播。
 * 函数在 workqueue 中可睡眠执行，gen_pool 自身同步并发 alloc/free。
 */
static void atomic_pool_resize(struct gen_pool *pool, gfp_t gfp)
{
	if (pool && gen_pool_avail(pool) < atomic_pool_size)
		atomic_pool_expand(pool, gen_pool_size(pool), gfp);
}

/*
 * 原子池低水位后台扩容 work。
 *
 * @work: 静态 atomic_pool_work；无需从参数反查其他对象。
 *
 * 按已编译 zone 依次处理 DMA、DMA32、kernel 池。单个失败不阻止其余池尝试；workqueue 保证同一
 * work 实例不会并发执行，故容量统计无需额外写锁。函数可睡眠。
 */
static void atomic_pool_work_fn(struct work_struct *work)
{
	if (IS_ENABLED(CONFIG_ZONE_DMA))
		atomic_pool_resize(atomic_pool_dma,
				   GFP_KERNEL | GFP_DMA);
	if (IS_ENABLED(CONFIG_ZONE_DMA32))
		atomic_pool_resize(atomic_pool_dma32,
				   GFP_KERNEL | GFP_DMA32);
	atomic_pool_resize(atomic_pool_kernel, GFP_KERNEL);
}

/*
 * 创建并预填一个指定 zone 的原子 DMA gen_pool。
 *
 * @pool_size: 初始目标容量。
 * @gfp: zone 与分配上下文标志。
 * 返回值: 成功为已含至少一块内存的 pool；创建或首次扩容失败为 NULL。
 *
 * gen_pool 以 PAGE_SHIFT 为最小分配阶，采用 first-fit/order-align 算法；只有首次扩容成功才发布给
 * 全局指针。负错误会销毁空 pool 并记录 zone/容量，0 则发布并记录实际容量；需注意 expand 中
 * gen_pool_add 失败被重新加密结果覆盖的源码边界可能让空 pool 被当成成功。仅在 postcore initcall
 * 调用，使用 GFP_KERNEL 类标志并可睡眠。
 */
static __init struct gen_pool *__dma_atomic_pool_init(size_t pool_size,
						      gfp_t gfp)
{
	struct gen_pool *pool;
	int ret;

	pool = gen_pool_create(PAGE_SHIFT, NUMA_NO_NODE);
	if (!pool)
		return NULL;

	gen_pool_set_algo(pool, gen_pool_first_fit_order_align, NULL);

	ret = atomic_pool_expand(pool, pool_size, gfp);
	if (ret) {
		gen_pool_destroy(pool);
		pr_err("DMA: failed to allocate %zu KiB %pGg pool for atomic allocation\n",
		       pool_size >> 10, &gfp);
		return NULL;
	}

	pr_info("DMA: preallocated %zu KiB %pGg pool for atomic allocations\n",
		gen_pool_size(pool) >> 10, &gfp);
	return pool;
}

#ifdef CONFIG_ZONE_DMA32
/* 有 DMA32 zone 时查询它是否包含 managed pages；未编译该 zone 时恒为 false。 */
#define has_managed_dma32 has_managed_zone(ZONE_DMA32)
#else
#define has_managed_dma32 false
#endif

/*
 * 在 postcore 阶段计算容量并初始化所有实际存在的原子 DMA 池。
 *
 * 返回值: 所需池全部建立为 0；任一存在 zone 的池失败为 -ENOMEM，但仍继续尝试其他池和 debugfs。
 *
 * 未指定 coherent_pool= 时按每 1 GiB RAM 配 128 KiB，最少 128 KiB、最多一个最大 buddy 块。
 * NORMAL、DMA、DMA32 仅在各自含 managed pages 时创建；系统可能一开始所有内存都属于低端 zone，
 * 因而不能假设 kernel 池必然存在。初始化 work 后再发布池，函数在启动串行上下文可睡眠。
 */
static int __init dma_atomic_pool_init(void)
{
	int ret = 0;

	/*
	 * If coherent_pool was not used on the command line, default the pool
	 * sizes to 128KB per 1GB of memory, min 128KB, max MAX_PAGE_ORDER.
	 */
	/* 未显式指定时按 RAM 比例估算，同时夹在 128 KiB 与伙伴系统最大阶所覆盖字节数之间。 */
	if (!atomic_pool_size) {
		unsigned long pages = totalram_pages() / (SZ_1G / SZ_128K);
		pages = min_t(unsigned long, pages, MAX_ORDER_NR_PAGES);
		atomic_pool_size = max_t(size_t, pages << PAGE_SHIFT, SZ_128K);
	}
	INIT_WORK(&atomic_pool_work, atomic_pool_work_fn);

	/* All memory might be in the DMA zone(s) to begin with */
	/* 某些系统启动时全部 managed memory 都在 DMA/DMA32，故只对实际存在的 zone 建池。 */
	if (has_managed_zone(ZONE_NORMAL)) {
		atomic_pool_kernel = __dma_atomic_pool_init(atomic_pool_size,
						    GFP_KERNEL);
		if (!atomic_pool_kernel)
			ret = -ENOMEM;
	}
	if (has_managed_dma()) {
		atomic_pool_dma = __dma_atomic_pool_init(atomic_pool_size,
						GFP_KERNEL | GFP_DMA);
		if (!atomic_pool_dma)
			ret = -ENOMEM;
	}
	if (has_managed_dma32) {
		atomic_pool_dma32 = __dma_atomic_pool_init(atomic_pool_size,
						GFP_KERNEL | GFP_DMA32);
		if (!atomic_pool_dma32)
			ret = -ENOMEM;
	}

	dma_atomic_pool_debugfs_init();
	return ret;
}
/* postcore 时机保证 gen_pool/CMA/页分配器可用，并早于多数设备驱动的原子 coherent 分配。 */
postcore_initcall(dma_atomic_pool_init);

/*
 * 按 gfp 要求和上一次候选推导下一个可尝试的原子池。
 *
 * @prev: NULL 表示第一次选择，否则为刚失败的实际 pool。
 * @gfp: 请求的 DMA/DMA32/普通 zone 偏好。
 * 返回值: 下一非空优先候选或 NULL 终止。
 *
 * 第一次按 gfp 选最合适的非空池：DMA 请求优先 DMA，DMA32 请求优先 DMA32，普通请求优先 kernel，
 * 缺失时用表达式选一个替代。一次候选失败后只沿 kernel→DMA32→DMA 的“地址更受限”方向推进，
 * 不会放宽到更高地址池。这里只选候选，不验证设备物理地址，最终由 phys_addr_ok 回调过滤。
 * 全局 pool 指针初始化后只读，函数不加锁、不睡眠。
 */
static inline struct gen_pool *dma_guess_pool(struct gen_pool *prev, gfp_t gfp)
{
	if (prev == NULL) {
		if (gfp & GFP_DMA)
			return atomic_pool_dma ?: atomic_pool_dma32 ?: atomic_pool_kernel;
		if (gfp & GFP_DMA32)
			return atomic_pool_dma32 ?: atomic_pool_dma ?: atomic_pool_kernel;
		return atomic_pool_kernel ?: atomic_pool_dma32 ?: atomic_pool_dma;
	}
	if (prev == atomic_pool_kernel)
		return atomic_pool_dma32 ? atomic_pool_dma32 : atomic_pool_dma;
	if (prev == atomic_pool_dma32)
		return atomic_pool_dma;
	return NULL;
}

/*
 * 从一个确定的 gen_pool 尝试分配并验证子块。
 *
 * @dev: 目标设备，传给可选地址过滤回调。
 * @size: 子块字节数。
 * @pool: 当前候选池。
 * @cpu_addr: 成功时输出可直接访问的池内虚拟地址。
 * @phys_addr_ok: 可选物理区间验证器；返回 false 时立即归还候选。
 * 返回值: 成功为子块物理首页对应的 struct page，失败为 NULL。
 *
 * gen_pool_alloc() 原子地占用位图区间，再从虚拟地址反查物理地址。验证通过后若剩余容量低于
 * atomic_pool_size 只调度后台扩容，不在当前不可阻塞路径取页；最后先发布 CPU 地址并清零整块。
 * gen_pool 内部同步并发分配，调用者取得独占子块，必须用 dma_free_from_pool() 归还。
 */
static struct page *__dma_alloc_from_pool(struct device *dev, size_t size,
		struct gen_pool *pool, void **cpu_addr,
		bool (*phys_addr_ok)(struct device *, phys_addr_t, size_t))
{
	unsigned long addr;
	phys_addr_t phys;

	addr = gen_pool_alloc(pool, size);
	if (!addr)
		return NULL;

	phys = gen_pool_virt_to_phys(pool, addr);
	if (phys_addr_ok && !phys_addr_ok(dev, phys, size)) {
		gen_pool_free(pool, addr, size);
		return NULL;
	}

	if (gen_pool_avail(pool) < atomic_pool_size)
		schedule_work(&atomic_pool_work);

	*cpu_addr = (void *)addr;
	memset(*cpu_addr, 0, size);
	return pfn_to_page(__phys_to_pfn(phys));
}

/*
 * 按 gfp 约束从全局原子 DMA 池集合分配子块。
 *
 * @dev: 目标设备，用于诊断和物理地址过滤。
 * @size: 请求字节数。
 * @cpu_addr: 成功时输出已清零 CPU 地址。
 * @gfp: zone 偏好及 __GFP_NOWARN。
 * @phys_addr_ok: 可选的设备可寻址性回调。
 * 返回值: 成功为物理首页 struct page；所有候选失败为 NULL。
 *
 * dma_guess_pool() 逐个给出不放宽地址约束的候选，内部失败会继续尝试更受限池。至少存在候选但耗尽
 * 时按 NOWARN 决定告警；根本没有合适池则无条件告警。热路径不睡眠，page 与 cpu_addr 是同一子块
 * 的两种视图，后续 free 使用 CPU 地址定位池。
 */
struct page *dma_alloc_from_pool(struct device *dev, size_t size,
		void **cpu_addr, gfp_t gfp,
		bool (*phys_addr_ok)(struct device *, phys_addr_t, size_t))
{
	struct gen_pool *pool = NULL;
	struct page *page;
	bool pool_found = false;

	while ((pool = dma_guess_pool(pool, gfp))) {
		pool_found = true;
		page = __dma_alloc_from_pool(dev, size, pool, cpu_addr,
					     phys_addr_ok);
		if (page)
			return page;
	}

	if (pool_found)
		WARN(!(gfp & __GFP_NOWARN), "DMA pool exhausted for %s\n", dev_name(dev));
	else
		WARN(1, "Failed to get suitable pool for %s\n", dev_name(dev));
	return NULL;
}

/*
 * 尝试把一个 CPU 地址区间归还给它所属的原子 DMA 池。
 *
 * @dev: 保留通用释放签名；池来源探测无需读取。
 * @start: 分配时输出的 CPU 起始地址。
 * @size: 原请求长度。
 * 返回值: 找到完整包含区间的池并释放为 true；不属于任何池为 false。
 *
 * 按普通请求的 kernel→DMA32→DMA 顺序探测，gen_pool_has_addr() 要求整个区间落在池内；命中后
 * gen_pool_free() 归还位图，不清零、不缩池、不重新加密。函数不睡眠，可被上层用作来源探测器；
 * 调用者须保证设备已停止且区间只释放一次。
 */
bool dma_free_from_pool(struct device *dev, void *start, size_t size)
{
	struct gen_pool *pool = NULL;

	while ((pool = dma_guess_pool(pool, 0))) {
		if (!gen_pool_has_addr(pool, (unsigned long)start, size))
			continue;
		gen_pool_free(pool, (unsigned long)start, size);
		return true;
	}

	return false;
}
