// SPDX-License-Identifier: GPL-2.0
/*
 * Coherent per-device memory handling.
 * Borrowed from i386
 */
/*
 * 学习说明：本文件实现“预留一致性 DMA 内存池”。平台代码可以把一段物理内存
 * 声明为某个设备的专用 coherent pool；通用 DMA 分配路径会先尝试从该池分配，
 * 从而满足设备只能访问特定地址窗口、必须使用保留内存等平台约束。
 *
 * 这里的“一致性”描述 CPU 与设备对内存内容的可见性契约，并不表示池管理无需
 * 同步。池内空闲页由位图管理，位图操作受自旋锁保护；整池的创建、挂接与销毁则
 * 依赖设备初始化/拆除路径在外部串行化。成功分配同时产生 CPU 虚拟地址和设备所见
 * DMA 地址，这两个地址指向同一段存储，但数值通常不同。
 *
 * 原注释翻译：处理每个设备各自的一致性内存；该实现最初借鉴自 i386。
 */
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>

/*
 * struct dma_coherent_mem - 一段 coherent pool 的地址视图与分配状态
 * @virt_base: CPU 通过 memremap() 得到的池起始虚拟地址
 * @device_base: 不使用设备 PFN 偏移换算时，设备看到的池起始 DMA 地址
 * @pfn_base: 池起始 CPU 物理页帧号，供 phys_to_dma() 动态换算 DMA 基址
 * @size: 池可分配容量，单位是页而不是字节
 * @bitmap: 页分配位图；置位区间表示已占用的二次幂页块
 * @spinlock: 保护 @bitmap 以及依赖位图结果的地址计算
 * @use_dev_dma_pfn_offset: 是否依据具体设备的 DMA/物理地址偏移计算基址
 *
 * @virt_base、@bitmap 和本对象均由 dma_init_coherent_memory() 创建，最终由
 * _dma_release_coherent_memory() 释放。挂到 dev->dma_mem 后，设备只借用这个
 * 指针；调用方必须保证整池销毁时没有尚未释放的块，也没有并发分配者。
 */
struct dma_coherent_mem {
	void		*virt_base;
	dma_addr_t	device_base;
	unsigned long	pfn_base;
	int		size;
	unsigned long	*bitmap;
	spinlock_t	spinlock;
	bool		use_dev_dma_pfn_offset;
};

/*
 * dev_get_coherent_memory - 取得设备当前挂接的专用 coherent pool
 * @dev: 待查询设备；允许为 NULL
 *
 * 返回值：设备及 dev->dma_mem 均存在时返回借用指针，否则返回 NULL。函数不增加
 * 引用计数，也不加锁，调用者只能在设备/pool 生命周期已由外层稳定住时使用结果。
 * 典型调用者是本文件中的设备池分配、释放和 mmap 包装函数；返回 NULL 会让它们
 * 指示上层继续尝试全局池或通用 DMA 路径。
 */
static inline struct dma_coherent_mem *dev_get_coherent_memory(struct device *dev)
{
	if (dev && dev->dma_mem)
		return dev->dma_mem;
	return NULL;
}

/*
 * dma_get_device_base - 计算当前设备访问该池时使用的 DMA 起始地址
 * @dev: 将要执行 DMA 的设备；动态换算分支要求它有效
 * @mem: 已初始化且生命周期稳定的 coherent pool
 *
 * @use_dev_dma_pfn_offset 为假时，返回平台声明时给出的固定 @device_base；为真时，
 * 先把 @pfn_base 还原成物理地址，再通过 phys_to_dma() 纳入该设备的 DMA 偏移。
 * 后一种模式允许一个共享预留池面对不同设备生成各自的 DMA 地址视图。
 *
 * 返回值：池首字节对应的 dma_addr_t。函数不修改状态、不睡眠；它本身不加锁，
 * 通常在分配函数持有池锁时调用。后续还要叠加所分配页块在池内的偏移。
 */
static inline dma_addr_t dma_get_device_base(struct device *dev,
					     struct dma_coherent_mem * mem)
{
	if (mem->use_dev_dma_pfn_offset)
		return phys_to_dma(dev, PFN_PHYS(mem->pfn_base));
	return mem->device_base;
}

/*
 * dma_init_coherent_memory - 映射物理区间并建立 coherent pool 管理对象
 * @phys_addr: CPU 物理地址空间中的池起始地址
 * @device_addr: 固定地址模式下设备所见的池起始 DMA 地址
 * @size: 池长度，单位为字节；上层契约要求按 PAGE_SIZE 对齐
 * @use_dma_pfn_offset: 是否忽略固定 @device_addr，按设备动态换算 DMA 基址
 *
 * 执行路径：先用 MEMREMAP_WC 建立 CPU 写合并映射，再分配管理对象和每页一位的
 * 位图，最后初始化地址字段与自旋锁。@pages 使用右移换算，因而调用者必须遵守
 * 页整倍数契约；本函数只拒绝零长度，并不会自行补齐非对齐的尾部。
 *
 * 返回值：成功返回由调用者拥有的 pool 指针；零长度或映射失败返回
 * ERR_PTR(-EINVAL)，元数据/位图分配失败返回 ERR_PTR(-ENOMEM)。失败路径按构造的
 * 逆序撤销资源。memremap()、kzalloc_obj() 和 bitmap_zalloc() 均可能睡眠，故只能
 * 在可睡眠的初始化上下文调用，不能处于自旋锁或中断上下文。
 */
static struct dma_coherent_mem *dma_init_coherent_memory(phys_addr_t phys_addr,
		dma_addr_t device_addr, size_t size, bool use_dma_pfn_offset)
{
	/*
	 * @dma_mem 是逐步构造并最终返回的管理对象；@pages 是可由位图管理的整页数；
	 * @mem_base 是 memremap() 返回的 CPU 映射基址，构造失败时需单独撤销。
	 */
	struct dma_coherent_mem *dma_mem;
	int pages = size >> PAGE_SHIFT;
	void *mem_base;

	if (!size)
		return ERR_PTR(-EINVAL);

	mem_base = memremap(phys_addr, size, MEMREMAP_WC);
	if (!mem_base)
		return ERR_PTR(-EINVAL);

	dma_mem = kzalloc_obj(struct dma_coherent_mem);
	if (!dma_mem)
		goto out_unmap_membase;
	dma_mem->bitmap = bitmap_zalloc(pages, GFP_KERNEL);
	if (!dma_mem->bitmap)
		goto out_free_dma_mem;

	dma_mem->virt_base = mem_base;
	dma_mem->device_base = device_addr;
	dma_mem->pfn_base = PFN_DOWN(phys_addr);
	dma_mem->size = pages;
	dma_mem->use_dev_dma_pfn_offset = use_dma_pfn_offset;
	spin_lock_init(&dma_mem->spinlock);

	return dma_mem;

out_free_dma_mem:
	kfree(dma_mem);
out_unmap_membase:
	memunmap(mem_base);
	pr_err("Reserved memory: failed to init DMA memory pool at %pa, size %zd MiB\n",
		&phys_addr, size / SZ_1M);
	return ERR_PTR(-ENOMEM);
}

/*
 * _dma_release_coherent_memory - 销毁尚未或不再挂接使用的 coherent pool
 * @mem: 待销毁 pool；允许为 NULL
 *
 * 依次撤销 CPU 映射、释放位图和管理对象。函数不会检查位图中是否仍有分配，也不会
 * 清理任何 dev->dma_mem 指针，因此调用方必须先阻止新访问并确保所有块已归还；
 * 否则设备或 CPU 仍可能使用已解除映射/释放的对象。memunmap() 与释放操作不应在
 * 持有池自旋锁时执行。本函数无返回值，NULL 输入直接忽略。
 */
static void _dma_release_coherent_memory(struct dma_coherent_mem *mem)
{
	if (!mem)
		return;

	memunmap(mem->virt_base);
	bitmap_free(mem->bitmap);
	kfree(mem);
}

/*
 * dma_assign_coherent_memory - 把一个已创建的 pool 发布到设备
 * @dev: 接收该 pool 的设备
 * @mem: 已初始化且尚未交给其他设备槽位的 pool
 *
 * 返回值：成功写入 dev->dma_mem 并返回 0；@dev 为空返回 -ENODEV；设备已经挂有
 * pool 时返回 -EBUSY，原指针保持不变。该函数既不取得 @mem 的额外引用，也不在
 * 失败时释放它，所有权转移只发生在成功返回后。
 *
 * 这里没有内部锁或原子发布操作，必须由设备注册/保留内存初始化等串行路径调用。
 * 成功后，设备池分配、释放和 mmap 包装函数会通过 dev->dma_mem 找到该对象。
 */
static int dma_assign_coherent_memory(struct device *dev,
				      struct dma_coherent_mem *mem)
{
	if (!dev)
		return -ENODEV;

	if (dev->dma_mem)
		return -EBUSY;

	dev->dma_mem = mem;
	return 0;
}

/*
 * Declare a region of memory to be handed out by dma_alloc_coherent() when it
 * is asked for coherent memory for this device.  This shall only be used
 * from platform code, usually based on the device tree description.
 *
 * phys_addr is the CPU physical address to which the memory is currently
 * assigned (this will be ioremapped so the CPU can access the region).
 *
 * device_addr is the DMA address the device needs to be programmed with to
 * actually address this memory (this will be handed out as the dma_addr_t in
 * dma_alloc_coherent()).
 *
 * size is the size of the area (must be a multiple of PAGE_SIZE).
 *
 * As a simplification for the platforms, only *one* such region of memory may
 * be declared per device.
 */
/*
 * dma_declare_coherent_memory - 为一个设备声明专用的一致性 DMA 内存区
 * @dev: 使用该内存区的设备
 * @phys_addr: 该内存区在 CPU 物理地址空间中的起始地址
 * @device_addr: 设备访问该内存区时需要使用的 DMA 起始地址
 * @size: 区域字节数；调用契约要求是 PAGE_SIZE 的整数倍
 *
 * 原注释翻译与展开：当 dma_alloc_coherent() 为 @dev 请求内存时，可从这里声明的
 * 区域交付内存。它只应由平台代码使用，通常来自设备树描述。@phys_addr 指定当前
 * 分配给该区域的 CPU 物理地址，本文件会将其重新映射以供 CPU 访问；@device_addr
 * 则直接作为设备编程所需的 DMA 地址基准。为简化平台接口，每个设备最多声明一池。
 *
 * 函数先完整创建 pool，再尝试挂到设备；挂接失败会立即销毁新 pool，因此不会把
 * 半初始化对象留给调用者。成功返回 0，此后 pool 由 dev->dma_mem 路径拥有；失败
 * 返回 dma_init_coherent_memory() 的指针错误或挂接阶段的 -ENODEV/-EBUSY。创建
 * 过程会睡眠，调用者须处于可睡眠且设备配置已串行化的上下文。
 */
int dma_declare_coherent_memory(struct device *dev, phys_addr_t phys_addr,
				dma_addr_t device_addr, size_t size)
{
	/* @mem 暂存新建 pool；@ret 传递挂接阶段状态并决定是否回滚 @mem。 */
	struct dma_coherent_mem *mem;
	int ret;

	mem = dma_init_coherent_memory(phys_addr, device_addr, size, false);
	if (IS_ERR(mem))
		return PTR_ERR(mem);

	ret = dma_assign_coherent_memory(dev, mem);
	if (ret)
		_dma_release_coherent_memory(mem);
	return ret;
}

/*
 * dma_release_coherent_memory - 解除并销毁设备的专用 coherent pool
 * @dev: 待解除 pool 的设备；允许为 NULL
 *
 * @dev 有效时销毁其当前 dev->dma_mem（NULL 也可），随后清空设备指针。无返回值。
 * 该顺序依赖外部拆除串行化：若仍有并发读取者，它们可能在指针清零前看到已释放
 * 对象；若仍有已分配 DMA 块，则解除映射会使 CPU/设备访问失效。调用前必须停止
 * DMA、归还全部分配并阻止新请求。销毁可能执行 memunmap()，不用于原子上下文。
 */
void dma_release_coherent_memory(struct device *dev)
{
	if (dev) {
		_dma_release_coherent_memory(dev->dma_mem);
		dev->dma_mem = NULL;
	}
}

/*
 * __dma_alloc_from_coherent - 从指定 pool 分配一个按页阶对齐的块
 * @dev: 请求设备，用于把物理基址换算成设备所见 DMA 地址
 * @mem: 目标 coherent pool，必须有效且生命周期稳定
 * @size: 请求的有效字节数，必须大于零
 * @dma_handle: 成功时写入设备可使用的 DMA 地址
 *
 * get_order(@size) 把请求提升为 2^order 个连续页，位图实际占用的是这个完整页块，
 * 而清零只覆盖调用者请求的 @size 字节。锁的 IRQ-save 形式既保护位图，也允许本
 * 函数被可能处于中断上下文的 DMA 分配路径使用；持锁期间不执行可能睡眠的操作。
 * 位图置位后先计算 DMA/CPU 两种地址，再释放锁并 memset()。虽然清零发生在锁外，
 * 该块的位已经占用，其他分配者无法得到它；返回前调用者也尚未看到地址。
 *
 * 返回值：成功返回 CPU 虚拟地址并填写 *@dma_handle；请求大于整池或找不到足够的
 * 二次幂连续页时返回 NULL，且不修改 *@dma_handle。此函数只表示指定池内的结果，
 * 是否允许回退到其他分配器由外层包装函数决定。
 */
static void *__dma_alloc_from_coherent(struct device *dev,
				       struct dma_coherent_mem *mem,
				       ssize_t size, dma_addr_t *dma_handle)
{
	/*
	 * @order 是位图按二次幂分配的页阶；@flags 保存调用现场 IRQ 状态；@pageno 是
	 * 成功块相对池首的页号；@ret 是由该页号换算出的 CPU 虚拟地址。
	 */
	int order = get_order(size);
	unsigned long flags;
	int pageno;
	void *ret;

	spin_lock_irqsave(&mem->spinlock, flags);

	if (unlikely(size > ((dma_addr_t)mem->size << PAGE_SHIFT)))
		goto err;

	pageno = bitmap_find_free_region(mem->bitmap, mem->size, order);
	if (unlikely(pageno < 0))
		goto err;

	/*
	 * Memory was found in the coherent area.
	 */
	/*
	 * 原注释翻译：已在 coherent 区域中找到内存。
	 * 到达这里说明位图区域已经原子地归属于本次请求，可以安全生成两种地址。
	 */
	*dma_handle = dma_get_device_base(dev, mem) +
			((dma_addr_t)pageno << PAGE_SHIFT);
	ret = mem->virt_base + ((dma_addr_t)pageno << PAGE_SHIFT);
	spin_unlock_irqrestore(&mem->spinlock, flags);
	memset(ret, 0, size);
	return ret;
err:
	spin_unlock_irqrestore(&mem->spinlock, flags);
	return NULL;
}

/**
 * dma_alloc_from_dev_coherent() - allocate memory from device coherent pool
 * @dev:	device from which we allocate memory
 * @size:	size of requested memory area
 * @dma_handle:	This will be filled with the correct dma handle
 * @ret:	This pointer will be filled with the virtual address
 *		to allocated area.
 *
 * This function should be only called from per-arch dma_alloc_coherent()
 * to support allocation from per-device coherent memory pools.
 *
 * Returns 0 if dma_alloc_coherent should continue with allocating from
 * generic memory areas, or !0 if dma_alloc_coherent should return @ret.
 */
/*
 * dma_alloc_from_dev_coherent - 尝试从设备专用 coherent pool 分配
 * @dev: 发起分配的设备
 * @size: 请求的有效字节数
 * @dma_handle: 成功时接收设备可用的 DMA 地址
 * @ret: 接收分配到的 CPU 虚拟地址；池耗尽时接收 NULL
 *
 * 原 kernel-doc 翻译与展开：本函数供体系结构或通用 coherent 分配路径调用，用来
 * 支持每设备内存池。没有专用池时返回 0，表示调用者应继续从通用区域分配；只要池
 * 存在就返回 1，表示本层已经接管请求，调用者必须直接返回 *@ret。特别注意，池
 * 耗尽时 *@ret 为 NULL 但返回值仍为 1，不能再回退，否则会绕过“必须使用此池”的
 * 平台约束。函数本身不睡眠，内部用 IRQ-safe 自旋锁同步并在成功时清零请求范围。
 */
int dma_alloc_from_dev_coherent(struct device *dev, ssize_t size,
		dma_addr_t *dma_handle, void **ret)
{
	/* @mem 是从设备槽位取得的借用指针，其是否存在决定本层是否接管请求。 */
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);

	if (!mem)
		return 0;

	*ret = __dma_alloc_from_coherent(dev, mem, size, dma_handle);
	return 1;
}

/*
 * __dma_release_from_coherent - 若地址属于指定 pool，则归还对应页阶区域
 * @mem: 候选 coherent pool；允许为 NULL
 * @order: 原始分配使用的页阶，表示要清除 2^order 个位图页
 * @vaddr: 原始分配返回的 CPU 虚拟起始地址
 *
 * 函数先做半开区间归属判断，命中后把地址差换算为起始页，在 IRQ-safe 自旋锁内
 * 清除位图区域。返回 1 表示地址被该池接管并已释放；返回 0 表示池不存在或地址不
 * 属于它，调用者应尝试其他释放路径。它不会验证页对齐、@order 是否匹配原分配，
 * 也不会防止重复释放；这些都属于“必须用原始地址和阶数且只释放一次”的调用契约。
 * 该路径不睡眠，但 pool 生命周期必须由外部保证，不能与整池销毁并发。
 */
static int __dma_release_from_coherent(struct dma_coherent_mem *mem,
				       int order, void *vaddr)
{
	if (mem && vaddr >= mem->virt_base && vaddr <
		   (mem->virt_base + ((dma_addr_t)mem->size << PAGE_SHIFT))) {
		/*
		 * @page 是 @vaddr 在位图中的起始页索引；@flags 保存加锁前 IRQ 状态，
		 * 使释放路径退出临界区后精确恢复调用现场。
		 */
		int page = (vaddr - mem->virt_base) >> PAGE_SHIFT;
		unsigned long flags;

		spin_lock_irqsave(&mem->spinlock, flags);
		bitmap_release_region(mem->bitmap, page, order);
		spin_unlock_irqrestore(&mem->spinlock, flags);
		return 1;
	}
	return 0;
}

/**
 * dma_release_from_dev_coherent() - free memory to device coherent memory pool
 * @dev:	device from which the memory was allocated
 * @order:	the order of pages allocated
 * @vaddr:	virtual address of allocated pages
 *
 * This checks whether the memory was allocated from the per-device
 * coherent memory pool and if so, releases that memory.
 *
 * Returns 1 if we correctly released the memory, or 0 if the caller should
 * proceed with releasing memory from generic pools.
 */
/*
 * dma_release_from_dev_coherent - 尝试把块归还给设备专用 coherent pool
 * @dev: 最初分配该块的设备
 * @order: 分配块的原始页阶
 * @vaddr: 分配时取得的 CPU 虚拟起始地址
 *
 * 原 kernel-doc 翻译与展开：先取得 @dev 的专用池，再检查 @vaddr 是否来自该池；
 * 若是则释放并返回 1，调用者必须结束释放流程；否则返回 0，让调用者继续检查全局
 * 或通用内存池。函数不改变设备/pool 的所有权，只修改受池锁保护的位图状态。
 */
int dma_release_from_dev_coherent(struct device *dev, int order, void *vaddr)
{
	/* @mem 是设备当前 pool 的借用指针；NULL 会自然产生“未接管”的返回值。 */
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);

	return __dma_release_from_coherent(mem, order, vaddr);
}

/*
 * __dma_mmap_from_coherent - 若缓冲区属于 pool，则映射其物理页到用户 VMA
 * @mem: 候选 coherent pool；允许为 NULL
 * @vma: 用户请求的目标虚拟内存区域，vm_pgoff 表示缓冲区内页偏移
 * @vaddr: DMA 分配返回的 CPU 虚拟起始地址
 * @size: 已分配缓冲区的有效字节数
 * @ret: 地址属于 pool 时接收 mmap 结果
 *
 * 首先验证整个 [@vaddr, @vaddr + @size) 位于池内，再把 CPU 地址换成池内起始页。
 * VMA 页偏移和页数必须完全落入 PAGE_ALIGN(@size) 所覆盖的页范围；默认写入
 * -ENXIO，边界合法时才调用 remap_pfn_range() 并保存其返回值。PFN 由池物理基页、
 * 缓冲区起始页和用户偏移三者相加得到。
 *
 * 返回 1 只表示该地址归此 pool 处理，调用者必须返回 *@ret；它不保证映射成功。
 * 返回 0 才表示可以尝试通用 mmap 路径。函数不操作分配位图，也不取得 VMA 之外
 * 的额外引用；调用者必须保证缓冲区在 VMA 建立期间仍已分配，pool 不会并发销毁。
 */
static int __dma_mmap_from_coherent(struct dma_coherent_mem *mem,
		struct vm_area_struct *vma, void *vaddr, size_t size, int *ret)
{
	if (mem && vaddr >= mem->virt_base && vaddr + size <=
		   (mem->virt_base + ((dma_addr_t)mem->size << PAGE_SHIFT))) {
		/*
		 * @off 是用户要求跳过的缓冲区页数；@start 是缓冲区相对池首的页号；
		 * @user_count 是 VMA 需要映射的页数；@count 是缓冲区向上页对齐后的页数。
		 */
		unsigned long off = vma->vm_pgoff;
		int start = (vaddr - mem->virt_base) >> PAGE_SHIFT;
		unsigned long user_count = vma_pages(vma);
		int count = PAGE_ALIGN(size) >> PAGE_SHIFT;

		*ret = -ENXIO;
		if (off < count && user_count <= count - off) {
			/* @pfn 是用户映射首字节对应的 CPU 物理页帧号。 */
			unsigned long pfn = mem->pfn_base + start + off;
			*ret = remap_pfn_range(vma, vma->vm_start, pfn,
					       user_count << PAGE_SHIFT,
					       vma->vm_page_prot);
		}
		return 1;
	}
	return 0;
}

/**
 * dma_mmap_from_dev_coherent() - mmap memory from the device coherent pool
 * @dev:	device from which the memory was allocated
 * @vma:	vm_area for the userspace memory
 * @vaddr:	cpu address returned by dma_alloc_from_dev_coherent
 * @size:	size of the memory buffer allocated
 * @ret:	result from remap_pfn_range()
 *
 * This checks whether the memory was allocated from the per-device
 * coherent memory pool and if so, maps that memory to the provided vma.
 *
 * Returns 1 if @vaddr belongs to the device coherent pool and the caller
 * should return @ret, or 0 if they should proceed with mapping memory from
 * generic areas.
 */
/*
 * dma_mmap_from_dev_coherent - 尝试映射设备专用池中的 DMA 缓冲区
 * @dev: 最初分配缓冲区的设备
 * @vma: 待建立映射的用户虚拟内存区域
 * @vaddr: dma_alloc_from_dev_coherent() 返回的 CPU 地址
 * @size: 缓冲区有效字节数
 * @ret: 地址属于专用池时接收 remap_pfn_range() 结果或 -ENXIO
 *
 * 原 kernel-doc 翻译与展开：本函数只选择设备池并转交内部映射器。返回 1 表示
 * @vaddr 属于该池，调用者必须把 *@ret 作为 mmap 结果；返回 0 表示不属于该池，
 * 可继续尝试全局池或通用区域。控制返回值与实际映射错误分离是该接口的关键。
 */
int dma_mmap_from_dev_coherent(struct device *dev, struct vm_area_struct *vma,
			   void *vaddr, size_t size, int *ret)
{
	/* @mem 是设备 pool 的借用指针，内部函数同时处理 NULL 与区间不命中。 */
	struct dma_coherent_mem *mem = dev_get_coherent_memory(dev);

	return __dma_mmap_from_coherent(mem, vma, vaddr, size, ret);
}

#ifdef CONFIG_DMA_GLOBAL_POOL
/*
 * dma_coherent_default_memory - 非一致性设备共用的默认 coherent pool
 *
 * 指针在体系结构初始化或设备树保留内存初始化阶段设置，随后由 __ro_after_init
 * 阻止运行期改写。pool 本身在系统生命周期内常驻，不提供整池销毁路径；页分配
 * 状态仍由对象内自旋锁保护。初始化顺序必须保证消费者开始分配前指针已经发布。
 */
static struct dma_coherent_mem *dma_coherent_default_memory __ro_after_init;

/*
 * dma_alloc_from_global_coherent - 从系统默认 coherent pool 分配
 * @dev: 请求设备，用于按设备 DMA 偏移换算地址
 * @size: 请求的有效字节数
 * @dma_handle: 成功时接收设备可用的 DMA 地址
 *
 * 返回值：全局池不存在或池内分配失败均返回 NULL；成功返回 CPU 虚拟地址。直接
 * DMA 分配路径对非一致性设备启用全局池时会直接返回本函数结果，因此 NULL 通常
 * 终止该次分配，而不是继续使用可能不满足一致性要求的普通内存。并发与清零规则
 * 由 __dma_alloc_from_coherent() 提供。
 */
void *dma_alloc_from_global_coherent(struct device *dev, ssize_t size,
				     dma_addr_t *dma_handle)
{
	if (!dma_coherent_default_memory)
		return NULL;

	return __dma_alloc_from_coherent(dev, dma_coherent_default_memory, size,
					 dma_handle);
}

/*
 * dma_release_from_global_coherent - 尝试向默认 coherent pool 归还页块
 * @order: 原分配使用的页阶
 * @vaddr: 原分配返回的 CPU 虚拟起始地址
 *
 * 返回 1 表示地址属于全局池且位图区域已释放；池未初始化或地址不属于它时返回 0。
 * 非一致性设备的 direct free 路径预期其地址必定属于该池，返回 0 会触发警告。
 * 调用者必须传入与分配匹配的地址和阶数；本函数不销毁常驻全局池。
 */
int dma_release_from_global_coherent(int order, void *vaddr)
{
	if (!dma_coherent_default_memory)
		return 0;

	return __dma_release_from_coherent(dma_coherent_default_memory, order,
			vaddr);
}

/*
 * dma_mmap_from_global_coherent - 尝试映射默认 pool 中的 DMA 缓冲区
 * @vma: 用户请求的目标虚拟内存区域
 * @vaddr: 待映射缓冲区的 CPU 地址
 * @size: 缓冲区有效字节数
 * @ret: 地址属于全局池时接收实际映射结果
 *
 * 全局池不存在或地址不属于它时返回 0，让 dma_direct_mmap() 继续普通 PFN 映射；
 * 命中时返回 1，调用者必须返回 *@ret，即使它是 -ENXIO 或 remap_pfn_range() 错误。
 * pool 和缓冲区的生命周期约束与 __dma_mmap_from_coherent() 相同。
 */
int dma_mmap_from_global_coherent(struct vm_area_struct *vma, void *vaddr,
				   size_t size, int *ret)
{
	if (!dma_coherent_default_memory)
		return 0;

	return __dma_mmap_from_coherent(dma_coherent_default_memory, vma,
					vaddr, size, ret);
}

/*
 * dma_init_global_coherent - 建立系统默认 coherent pool
 * @phys_addr: 默认池的 CPU 物理起始地址
 * @size: 默认池字节数，调用契约要求按页对齐且非零
 *
 * 以动态设备 PFN 偏移模式创建 pool：传入的固定 DMA 基址虽然等于物理基址，但
 * 实际分配时会针对 @dev 调用 phys_to_dma()。成功后把对象发布到只读初始化指针并
 * 返回 0；失败返回 dma_init_coherent_memory() 的错误码。函数没有重复初始化保护，
 * 必须只在早期串行初始化阶段调用一次；否则旧对象会失去引用。创建过程可睡眠。
 */
int dma_init_global_coherent(phys_addr_t phys_addr, size_t size)
{
	/* @mem 暂存新建 pool；只有非错误指针才会发布到全局只读初始化槽位。 */
	struct dma_coherent_mem *mem;

	mem = dma_init_coherent_memory(phys_addr, phys_addr, size, true);
	if (IS_ERR(mem))
		return PTR_ERR(mem);
	dma_coherent_default_memory = mem;
	pr_info("DMA: default coherent area is set\n");
	return 0;
}
#endif /* CONFIG_DMA_GLOBAL_POOL */

/*
 * Support for reserved memory regions defined in device tree
 */
/*
 * 原注释翻译与章节说明：以下代码支持设备树定义的 reserved-memory 区域。
 * “shared-dma-pool”节点先在扫描扁平设备树时完成属性校验；具体设备引用该区域时，
 * 再惰性创建共享 dma_coherent_mem 并挂到设备。若节点带 linux,dma-default，则只在
 * 扫描期记录地址，稍后的 core_initcall 才建立全局默认池。
 */
#ifdef CONFIG_OF_RESERVED_MEM
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>

#ifdef CONFIG_DMA_GLOBAL_POOL
/*
 * dma_reserved_default_memory_base - 设备树默认 DMA 池的物理起始地址
 * dma_reserved_default_memory_size - 设备树默认 DMA 池的字节数兼有效标记
 *
 * 两者只在早期设备树扫描与 core_initcall 之间传递数据，故使用 __initdata；初始化
 * 阶段结束后存储可被回收。size 为零表示没有发现 linux,dma-default 区域。
 */
static phys_addr_t dma_reserved_default_memory_base __initdata;
static phys_addr_t dma_reserved_default_memory_size __initdata;
#endif

/*
 * rmem_dma_device_init - 把 shared-dma-pool 保留区挂到一个设备
 * @rmem: OF reserved-memory 框架提供的共享区域描述符
 * @dev: 将使用该区域进行 coherent DMA 分配的设备
 *
 * 首个设备到来时，根据 @rmem 的物理范围惰性创建 pool，并把拥有指针保存在
 * rmem->priv；后续设备复用同一个管理对象和位图。pool 使用设备 PFN 偏移模式，
 * 因而每次分配都能针对具体 @dev 换算 DMA 基址。创建可能睡眠，且 rmem->priv 的
 * 首次发布没有内部锁，依赖 reserved-memory 设备初始化流程串行化。
 *
 * 返回值：pool 创建失败时返回其错误码；其余路径返回 0。注意当前实现有意忽略
 * dma_assign_coherent_memory() 的返回值，所以 0 只表示 reserved-memory 回调完成，
 * 若设备原先已有 dev->dma_mem，新的共享池实际上不会覆盖它。rmem->priv 持有 pool
 * 的系统期所有权，单个设备只借用该指针。
 */
static int rmem_dma_device_init(struct reserved_mem *rmem, struct device *dev)
{
	/* @mem 借用 rmem->priv；首次调用时它也承接新建对象，随后发布回 @priv。 */
	struct dma_coherent_mem *mem = rmem->priv;

	if (!mem) {
		mem = dma_init_coherent_memory(rmem->base, rmem->base,
					       rmem->size, true);
		if (IS_ERR(mem))
			return PTR_ERR(mem);
		rmem->priv = mem;
	}

	/* Warn if the device potentially can't use the reserved memory */
	/*
	 * 原注释翻译：如果设备可能无法使用这段保留内存，就发出警告。
	 * 该检查把区域末地址与 coherent mask/bus limit 的有效上限比较，只作诊断；
	 * 超界不会阻止挂接，后续驱动仍必须正确配置设备 DMA 能力。
	 */
	if (mem->device_base + rmem->size - 1 >
	    min_not_zero(dev->coherent_dma_mask, dev->bus_dma_limit))
		dev_warn(dev, "reserved memory is beyond device's set DMA address range\n");

	dma_assign_coherent_memory(dev, mem);
	return 0;
}

/*
 * rmem_dma_device_release - 解除设备对 shared-dma-pool 的借用
 * @rmem: 对应 reserved-memory 描述符；共享 pool 仍由其 @priv 持有
 * @dev: 待解除的设备；允许为 NULL
 *
 * 有效设备只清空 dev->dma_mem，不释放 pool、映射或位图，因为其他设备可能仍在
 * 共享它。函数也不核对当前指针是否等于 rmem->priv；reserved-memory 框架必须成对
 * 调用正确区域的 init/release，并在设备不再有活动 DMA 后串行执行解绑。
 */
static void rmem_dma_device_release(struct reserved_mem *rmem,
				    struct device *dev)
{
	if (dev)
		dev->dma_mem = NULL;
}


/*
 * rmem_dma_setup - 校验并登记一个设备树 shared-dma-pool 节点
 * @node: 扁平设备树中的节点偏移，用于读取早期属性
 * @rmem: 已解析出 @base/@size 的 reserved-memory 描述符
 *
 * reusable 区域不能满足此处独占式位图管理，直接返回 -ENODEV；ARM 当前还要求
 * no-map，避免线性映射与本文件的 WC 映射形成不支持的别名。若存在
 * linux,dma-default，则记录为稍后建立的全局池；重复定义会 WARN，但当前节点的
 * 地址仍覆盖旧记录。成功打印区域信息并返回 0，ARM 缺少 no-map 返回 -EINVAL。
 * 函数带 __init，只在早期 OF 扫描阶段存在，不处理具体设备的挂接。
 */
static int __init rmem_dma_setup(unsigned long node, struct reserved_mem *rmem)
{
	if (of_get_flat_dt_prop(node, "reusable", NULL))
		return -ENODEV;

#ifdef CONFIG_ARM
	if (!of_get_flat_dt_prop(node, "no-map", NULL)) {
		pr_err("Reserved memory: regions without no-map are not yet supported\n");
		return -EINVAL;
	}
#endif

#ifdef CONFIG_DMA_GLOBAL_POOL
	if (of_get_flat_dt_prop(node, "linux,dma-default", NULL)) {
		WARN(dma_reserved_default_memory_size,
		     "Reserved memory: region for default DMA coherent area is redefined\n");
		dma_reserved_default_memory_base = rmem->base;
		dma_reserved_default_memory_size = rmem->size;
	}
#endif

	pr_info("Reserved memory: created DMA memory pool at %pa, size %ld MiB\n",
		&rmem->base, (unsigned long)rmem->size / SZ_1M);
	return 0;
}

#ifdef CONFIG_DMA_GLOBAL_POOL
/*
 * dma_init_reserved_memory - 从设备树暂存信息建立全局默认 coherent pool
 *
 * 无参数。没有发现非零大小的 linux,dma-default 区域时返回 -ENOMEM；否则把早期
 * 记录的物理范围交给 dma_init_global_coherent() 并透传其结果。core_initcall 保证
 * 它在常规设备驱动开始分配前执行；函数与暂存变量在初始化结束后均可回收。
 */
static int __init dma_init_reserved_memory(void)
{
	if (!dma_reserved_default_memory_size)
		return -ENOMEM;
	return dma_init_global_coherent(dma_reserved_default_memory_base,
					dma_reserved_default_memory_size);
}
core_initcall(dma_init_reserved_memory);
#endif /* CONFIG_DMA_GLOBAL_POOL */

/*
 * rmem_dma_ops - shared-dma-pool 节点的 OF reserved-memory 生命周期回调表
 * @node_init: 早期节点校验及默认池信息登记
 * @device_init: 设备绑定该区域时创建/复用并挂接 pool
 * @device_release: 设备解绑时仅撤销借用关系
 *
 * 表本身只保存函数指针且为只读；reserved-memory 核心依据下方声明找到它。
 */
static const struct reserved_mem_ops rmem_dma_ops = {
	.node_init	= rmem_dma_setup,
	.device_init	= rmem_dma_device_init,
	.device_release	= rmem_dma_device_release,
};

/*
 * 将兼容串“shared-dma-pool”与 rmem_dma_ops 注册到 reserved-memory 声明表。
 * 启动期 OF 扫描由此把匹配节点交给 rmem_dma_setup()；宏不在运行期执行分配。
 */
RESERVEDMEM_OF_DECLARE(dma, "shared-dma-pool", &rmem_dma_ops);
#endif
