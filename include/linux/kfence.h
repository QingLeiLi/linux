/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 中文学习注释：由 OpenAI GPT-5 Codex 于 2026-07-28 追加。
 *
 * 文件地图：
 *   本头文件是 KFENCE 对外暴露给通用分配器、page fault 处理路径和 printk
 *   对象诊断路径的契约层；真正的对象状态机、元数据数组、
 *   guard page 映射和错误报告实现在 mm/kfence/core.c 与 mm/kfence/report.c。
 *
 * 主调用链：
 *   slab_alloc_node()/__kmem_cache_alloc_node() 等 SLUB 分配入口
 *     -> kfence_alloc()
 *     -> __kfence_alloc()
 *     -> 成功时返回位于 KFENCE pool 的单对象页
 *
 *   slab_free()/kfree() 等释放入口
 *     -> kfence_free()
 *     -> __kfence_free()
 *     -> 标记对象 freed，必要时通过 RCU 延迟归还
 *
 *   架构 page fault 入口
 *     -> kfence_handle_page_fault()
 *     -> 把 guard page 或 freed object 访问转化为 KFENCE 报告
 *
 * 核心对象与生命周期：
 *   __kfence_pool 指向一段专用页区间，偶数编号页承载对象，奇数编号页通常作为
 *   guard/redzone；每个可分配对象都有一份 kfence_metadata。普通 slab freelist
 *   不能接收 KFENCE 对象，因此所有分配器集成点都必须先用 is_kfence_address()
 *   区分“普通堆对象”和“KFENCE 专用池对象”。
 *
 * 并发模型：
 *   这个头文件的 fast path 只做静态分支、原子门闩和地址范围判断；需要睡眠、
 *   取元数据锁、更新对象状态或打印报告的工作交给 .c 文件。读者阅读此文件时
 *   要把它理解成“低开销分流器”：多数分配/释放直接返回给普通分配器，极少数
 *   采样请求才进入 KFENCE 慢路径。
 */
/*
 * Kernel Electric-Fence (KFENCE). Public interface for allocator and fault
 * handler integration. For more info see Documentation/dev-tools/kfence.rst.
 *
 * Copyright (C) 2020, Google LLC.
 */

#ifndef _LINUX_KFENCE_H
#define _LINUX_KFENCE_H

#include <linux/mm.h>
#include <linux/types.h>

#ifdef CONFIG_KFENCE

#include <linux/atomic.h>
#include <linux/static_key.h>

extern unsigned long kfence_sample_interval;
/*
 * kfence_sample_interval 保存两次采样打开之间的时间间隔，单位由
 * mm/kfence/core.c 的定时器逻辑解释。启动参数 kfence.sample_interval
 * 会影响它：值越小，KFENCE 越频繁尝试接管普通分配；为 0 时表示启动后
 * 进入特殊的静态分支策略，而不是让每次分配都无条件走慢路径。
 *
 * 读写关系：
 *   - 启动参数和 debugfs/sysctl 类控制路径写入；
 *   - 分配 gate 定时器读取并决定何时重新打开一次采样机会。
 * 该变量本身不代表“当前这次分配能否由 KFENCE 接管”，后者还要看
 * kfence_allocation_key 与 kfence_allocation_gate。
 */

/*
 * We allocate an even number of pages, as it simplifies calculations to map
 * address to metadata indices; effectively, the very first page serves as an
 * extended guard page, but otherwise has no special purpose.
 */
/*
 * 中文翻译与补充：KFENCE pool 总是分配偶数个页，这样由地址反推元数据索引
 * 时可以按“对象页 + guard 页”的固定节奏计算。CONFIG_KFENCE_NUM_OBJECTS
 * 表示可跟踪对象数；额外的 +1 让起始处也拥有保护间隔，所以第一页等价于
 * 一个扩展 guard page。它没有独立对象语义，只是让边界越界访问更容易落入
 * 不可访问页。
 *
 * KFENCE_POOL_SIZE 必须保持编译期常量，因为 is_kfence_address() 位于分配器
 * fast path；若这里变成运行时加载，就会让每次普通分配/释放都多付成本。
 */
#define KFENCE_POOL_SIZE ((CONFIG_KFENCE_NUM_OBJECTS + 1) * 2 * PAGE_SIZE)
extern char *__kfence_pool;
/*
 * __kfence_pool 是 KFENCE 专用页池的起始虚拟地址，由 early memblock 或
 * 后续 alloc_pages_exact() 初始化，并在 mm/kfence/core.c 中导出给测试模块。
 * NULL 是“pool 尚未建立或已不可用”的哨兵值。调用者只借用这个全局地址做
 * 范围判定，不能把普通对象插入这段地址，也不能把这段地址交给普通 slab
 * freelist 管理。
 */

DECLARE_STATIC_KEY_FALSE(kfence_allocation_key);
extern atomic_t kfence_allocation_gate;
/*
 * kfence_allocation_key 是分配 fast path 的第一道静态分支门：关闭时普通分配器
 * 几乎不承担额外分支成本；打开时才继续检查原子 gate。真正打开/关闭由 KFENCE
 * 定时器和配置选项控制。
 *
 * kfence_allocation_gate 是一次采样窗口内的原子门闩。通常 gate > 0 表示本窗口
 * 已经被消耗或尚未开放，kfence_alloc() 必须返回 NULL 让普通分配继续；当 gate
 * 被定时器置到可用状态后，__kfence_alloc() 通过 atomic_inc_return() 竞争唯一
 * 接管机会。它只保证采样节流，不保证一定能拿到对象，慢路径还会检查 size、
 * GFP zone、cache 标志、覆盖率和池容量。
 */

/**
 * is_kfence_address() - check if an address belongs to KFENCE pool
 * @addr: address to check
 *
 * Return: true or false depending on whether the address is within the KFENCE
 * object range.
 *
 * KFENCE objects live in a separate page range and are not to be intermixed
 * with regular heap objects (e.g. KFENCE objects must never be added to the
 * allocator freelists). Failing to do so may and will result in heap
 * corruptions, therefore is_kfence_address() must be used to check whether
 * an object requires specific handling.
 *
 * Note: This function may be used in fast-paths, and is performance critical.
 * Future changes should take this into account; for instance, we want to avoid
 * introducing another load and therefore need to keep KFENCE_POOL_SIZE a
 * constant (until immediate patching support is added to the kernel).
 */
/*
 * 中文翻译与补充：
 *   is_kfence_address() 用来判断 @addr 是否属于 KFENCE pool。
 *
 * 入参：
 *   @addr: 借用的任意内核堆地址或疑似堆地址；函数只读取地址数值，不解引用，
 *          因此可用于释放路径、ksize 路径和 fault 路径的早期分流。NULL 或
 *          普通 slab/vmalloc 地址都会被当作非 KFENCE 地址。
 *
 * 返回：
 *   true  表示地址数值落在 __kfence_pool 覆盖的 KFENCE 对象/guard 页范围内；
 *         调用者必须改走 KFENCE 专用处理，不能把对象继续交给普通 slab freelist。
 *   false 表示地址不属于当前可用的 KFENCE pool，调用者应继续普通分配器路径。
 *
 * 调用位置与副作用：
 *   这是分配器 fast path 和 fault path 的低成本分类器，不加锁、不睡眠、不修改
 *   元数据，也不证明 @addr 指向一个“当前已分配的有效对象”。有效性、UAF/OOB
 *   分类和报告都在 mm/kfence/core.c 中依据 metadata 完成。
 *
 * 原英文说明强调：KFENCE 对象生活在独立页区间，不能与普通堆对象混用，例如
 * 绝不能进入普通 allocator freelist。若分配器忘记用本函数分流，就可能把
 * KFENCE 专用页按普通 slab 对象回收，最终造成堆元数据破坏。
 *
 * 性能注意：该函数会出现在极热路径，所以把范围判断写成一个常量大小比较，并把
 * __kfence_pool 非 NULL 检查留在慢路径位置，避免给绝大多数普通地址增加额外加载。
 */
static __always_inline bool is_kfence_address(const void *addr)
{
	/*
	 * The __kfence_pool != NULL check is required to deal with the case
	 * where __kfence_pool == NULL && addr < KFENCE_POOL_SIZE. Keep it in
	 * the slow-path after the range-check!
	 */
	/*
	 * 中文翻译与补充：必须检查 __kfence_pool != NULL，否则在 pool 尚未建立时，
	 * 小数值地址可能因为“addr - NULL < KFENCE_POOL_SIZE”而被误判为 KFENCE。
	 * 这里故意先做范围比较再判断 pool 指针，是为了让普通地址快速失败；只有
	 * 看起来落入 pool 窗口的极少数地址才需要真正读取 __kfence_pool 的空值状态。
	 */
	return unlikely((unsigned long)((char *)addr - __kfence_pool) < KFENCE_POOL_SIZE && __kfence_pool);
}

/**
 * kfence_alloc_pool_and_metadata() - allocate the KFENCE pool and KFENCE
 * metadata via memblock
 */
/*
 * 中文翻译与补充：
 *   通过 memblock 在早期启动阶段分配 KFENCE pool 和元数据。
 *
 * 入参：无。
 * 返回：无直接返回值。
 *
 * 副作用与调用位置：
 *   该函数在普通 page allocator/slab 完全可用前运行，给后续 kfence_init()
 *   准备 __kfence_pool 与 metadata 存储。失败时实现会保持 KFENCE 不可用，
 *   后续 public helper 仍会按“没有 KFENCE 对象”安全退化。调用者不取得对象
 *   所有权，只触发全局初始化状态变化。
 *
 * 上下文：
 *   __init 入口，只在启动初始化阶段使用；不能作为运行期重新分配 pool 的接口。
 */
void __init kfence_alloc_pool_and_metadata(void);

/**
 * kfence_init() - perform KFENCE initialization at boot time
 *
 * Requires that kfence_alloc_pool_and_metadata() was called before. This sets
 * up the allocation gate timer, and requires that workqueues are available.
 */
/*
 * 中文翻译与补充：
 *   kfence_init() 完成启动期 KFENCE 初始化。
 *
 * 入参：无。
 * 返回：无直接返回值。
 *
 * 前置条件：
 *   kfence_alloc_pool_and_metadata() 已先尝试分配 pool/metadata；系统 workqueue
 *   已可用，因为初始化会设置 allocation gate 定时器并把后续采样窗口交给延迟工作。
 *
 * 副作用：
 *   初始化 metadata、保护页映射、静态分支/定时器状态，并在成功后让分配 fast path
 *   可以偶发进入 kfence_alloc()。失败或配置不满足时，公共接口继续返回普通退化值。
 *   __init 表示启动后代码可丢弃，运行期不应再调用。
 */
void __init kfence_init(void);

/**
 * kfence_shutdown_cache() - handle shutdown_cache() for KFENCE objects
 * @s: cache being shut down
 *
 * Before shutting down a cache, one must ensure there are no remaining objects
 * allocated from it. Because KFENCE objects are not referenced from the cache
 * directly, we need to check them here.
 *
 * Note that shutdown_cache() is internal to SL*B, and kmem_cache_destroy() does
 * not return if allocated objects still exist: it prints an error message and
 * simply aborts destruction of a cache, leaking memory.
 *
 * If the only such objects are KFENCE objects, we will not leak the entire
 * cache, but instead try to provide more useful debug info by making allocated
 * objects "zombie allocations". Objects may then still be used or freed (which
 * is handled gracefully), but usage will result in showing KFENCE error reports
 * which include stack traces to the user of the object, the original allocation
 * site, and caller to shutdown_cache().
 */
/*
 * 中文翻译与补充：
 *   kfence_shutdown_cache() 处理 kmem_cache 销毁时遗留的 KFENCE 对象。
 *
 * 入参：
 *   @s: 正在 shutdown_cache()/kmem_cache_destroy() 的 slab cache；借用指针，
 *       调用期间仍由 slab 销毁路径维持有效，本函数不取得 cache 引用，也不释放 @s。
 *
 * 返回：
 *   无直接返回值。可观察副作用是扫描 KFENCE metadata，把仍属于 @s 的活动对象
 *   标成 zombie allocation，并在第二轮把已 freed 的 @s 关联清空。
 *
 * 调用位置与语义：
 *   普通 slab 对象仍挂在 cache 自己的对象管理结构中，kmem_cache_destroy()
 *   可以发现“还有对象未释放”并拒绝销毁；KFENCE 对象不直接挂在 cache freelist/
 *   slab 列表上，所以必须在这里单独扫描 metadata。
 *
 * 原英文说明的关键点是：若 cache 只剩 KFENCE 对象，内核不继续泄漏整个 cache，
 * 而是把这些对象变成 zombie。后续使用或释放仍会被 KFENCE 平滑处理，但使用会
 * 打印更有价值的报告，报告中同时包含使用点、原始分配点和 shutdown_cache()
 * 调用点，有利于定位“销毁 cache 时仍有人持有对象”的生命周期 bug。
 *
 * 并发注意：
 *   实现在 mm/kfence/core.c 中通过 acquire 读取已发布的 metadata，并对单个
 *   metadata 使用 raw spinlock 复核 cache/state。这个头文件只声明契约；调用者
 *   不能假设它会释放对象内存，也不能把 zombie 当作普通成功 free。
 */
void kfence_shutdown_cache(struct kmem_cache *s);

/*
 * Allocate a KFENCE object. Allocators must not call this function directly,
 * use kfence_alloc() instead.
 */
/*
 * 中文翻译与补充：__kfence_alloc() 是 KFENCE 真正的慢路径分配实现。普通分配器
 * 必须先调用 kfence_alloc()，由静态分支和 gate 过滤后才可能到达这里；直接调用
 * 会绕过采样节流，破坏“低概率、低开销”的设计。
 *
 * 入参：
 *   @s:    借用的 slab cache，提供对象大小、DMA/RCU/cache flags 等约束；
 *          成功时 KFENCE metadata 会记录该 cache，但不把 @s 的所有权转入 KFENCE。
 *   @size: 本次请求的精确访问大小，单位字节；对 kmalloc cache 可小于 @s->size，
 *          且必须不超过 PAGE_SIZE。
 *   @flags:GFP 分配标志；慢路径会拒绝 KFENCE pool 无法保证的 zone/NUMA/DMA 约束。
 *
 * 返回：
 *   NULL     表示本次不能由 KFENCE 接管，调用者必须继续普通分配路径；
 *   non-NULL 表示返回 KFENCE pool 中的对象起始地址，调用者按普通对象交给上层，
 *            释放时必须通过 kfence_free()/__kfence_free() 识别回收。
 *
 * 上下文与副作用：
 *   可能采集栈、取 metadata 锁、修改对象状态、更新覆盖率和统计计数；它是慢路径，
 *   不应出现在每次普通分配都必经的位置。
 */
void *__kfence_alloc(struct kmem_cache *s, size_t size, gfp_t flags);

/**
 * kfence_alloc() - allocate a KFENCE object with a low probability
 * @s:     struct kmem_cache with object requirements
 * @size:  exact size of the object to allocate (can be less than @s->size
 *         e.g. for kmalloc caches)
 * @flags: GFP flags
 *
 * Return:
 * * NULL     - must proceed with allocating as usual,
 * * non-NULL - pointer to a KFENCE object.
 *
 * kfence_alloc() should be inserted into the heap allocation fast path,
 * allowing it to transparently return KFENCE-allocated objects with a low
 * probability using a static branch (the probability is controlled by the
 * kfence.sample_interval boot parameter).
 */
/*
 * 中文翻译与补充：
 *   kfence_alloc() 以低概率为普通堆分配返回一个 KFENCE 对象。
 *
 * 入参：
 *   @s:    借用的 kmem_cache，描述对象对齐、cache flags、构造策略等要求；
 *          本函数不会取得 cache 引用，成功对象的 metadata 会记录它以便释放和报告。
 *   @size: 调用者请求的精确大小，单位字节；kmalloc cache 中它可能小于 @s->size，
 *          KFENCE 用它决定可合法访问的字节数和 OOB 报告边界。
 *   @flags:GFP 标志，原样传给慢路径，用于判断当前分配约束是否能由固定 pool 满足。
 *
 * 返回：
 *   NULL     表示本次采样未打开、gate 已被消费或慢路径拒绝；调用者必须继续正常
 *            allocator 路径，不能把它视为分配失败。
 *   non-NULL 表示 KFENCE 已接管本次分配，返回值是调用者可使用的对象地址；后续
 *            释放、ksize 和对象诊断都要能识别它来自 KFENCE pool。
 *
 * 执行上下文：
 *   设计为插入 slab 分配 fast path，可在频繁调用场景执行。前两道检查只读静态
 *   分支和原子 gate，不睡眠；只有极少数采样窗口才进入 __kfence_alloc() 慢路径。
 */
static __always_inline void *kfence_alloc(struct kmem_cache *s, size_t size, gfp_t flags)
{
#if defined(CONFIG_KFENCE_STATIC_KEYS) || CONFIG_KFENCE_SAMPLE_INTERVAL == 0
	/*
	 * 阶段 1：静态分支过滤。
	 *
	 * CONFIG_KFENCE_STATIC_KEYS 或 sample_interval=0 场景下，默认分支是不进入
	 * KFENCE。static key 让关闭状态接近一条被 patch 掉的分支，避免普通分配
	 * 为调试设施长期付固定成本。
	 */
	if (!static_branch_unlikely(&kfence_allocation_key))
		return NULL;
#else
	/*
	 * 阶段 1 的另一种配置：没有静态 key gate 时使用相反预测形式，让“已启用但
	 * 仍低概率采样”的路径保持便宜。两种配置的外部契约相同：返回 NULL 只是让
	 * allocator 继续常规分配。
	 */
	if (!static_branch_likely(&kfence_allocation_key))
		return NULL;
#endif
	/*
	 * 阶段 2：原子 gate 节流。
	 *
	 * gate > 0 表示当前采样机会不可用或已被其他 CPU 消费；此时直接返回 NULL，
	 * 不进入会采栈、取锁和扫描 freelist 的慢路径。只有 gate 到达可竞争状态时，
	 * __kfence_alloc() 才会用 atomic_inc_return() 尝试领取本窗口的唯一机会。
	 */
	if (likely(atomic_read(&kfence_allocation_gate) > 0))
		return NULL;
	/*
	 * 阶段 3：进入真正 KFENCE 分配。这里之后仍可能返回 NULL，因为对象大小、
	 * GFP zone、cache flags、覆盖率过滤或池容量都可能拒绝本次采样。
	 */
	return __kfence_alloc(s, size, flags);
}

/**
 * kfence_ksize() - get actual amount of memory allocated for a KFENCE object
 * @addr: pointer to a heap object
 *
 * Return:
 * * 0     - not a KFENCE object, must call __ksize() instead,
 * * non-0 - this many bytes can be accessed without causing a memory error.
 *
 * kfence_ksize() returns the number of bytes requested for a KFENCE object at
 * allocation time. This number may be less than the object size of the
 * corresponding struct kmem_cache.
 */
/*
 * 中文翻译与补充：
 *   kfence_ksize() 查询 KFENCE 对象在分配时记录的可访问大小。
 *
 * 入参：
 *   @addr: 借用的堆对象地址；可以是普通对象、KFENCE 对象或非法地址。本函数不
 *          转移所有权，也不释放对象；调用者通常已经在 ksize()/kmemleak 路径中
 *          做了基础有效性检查。
 *
 * 返回：
 *   0     表示 @addr 不是可识别的 KFENCE 对象，调用者应继续 __ksize() 等普通路径；
 *   non-0 表示从 metadata 读到的原始请求大小，单位字节；这是用户可无错误访问的
 *         范围，可能小于对应 kmem_cache 的对象槽大小。
 *
 * 并发注意：
 *   实现中对 metadata 的读取是 lockless 的。若与重新分配并发，KFENCE 把这种访问
 *   视为 UAF 或非法访问语义的一部分；该 helper 的职责是给诊断/ksize 提供大小，
 *   不是为对象建立新的生命周期引用。
 */
size_t kfence_ksize(const void *addr);

/**
 * kfence_object_start() - find the beginning of a KFENCE object
 * @addr: address within a KFENCE-allocated object
 *
 * Return: address of the beginning of the object.
 *
 * SL[AU]B-allocated objects are laid out within a page one by one, so it is
 * easy to calculate the beginning of an object given a pointer inside it and
 * the object size. The same is not true for KFENCE, which places a single
 * object at either end of the page. This helper function is used to find the
 * beginning of a KFENCE-allocated object.
 */
/*
 * 中文翻译与补充：
 *   kfence_object_start() 根据 KFENCE 对象内部地址找回对象起始地址。
 *
 * 入参：
 *   @addr: 位于某个 KFENCE 对象可访问范围内的借用地址；释放 RCU 回调、诊断或
 *          allocator 修正对象指针时会传入。它不一定等于对象起点。
 *
 * 返回：
 *   non-NULL 表示 metadata 可识别，并返回该 KFENCE 对象的起始地址；
 *   NULL     表示无法从 @addr 找到 KFENCE metadata，调用者需走普通对象定位逻辑
 *            或按错误路径处理。
 *
 * 原英文说明对比了 SLAB/SLUB 和 KFENCE 的布局差异：普通 slab 页内对象按固定
 * 步长连续排列，可由 object size 反推起点；KFENCE 每页只有一个对象，且可能放在
 * 页首或页尾以制造左右 guard 区，因此必须依赖 metadata 中保存的真实 addr。
 */
void *kfence_object_start(const void *addr);

/**
 * __kfence_free() - release a KFENCE heap object to KFENCE pool
 * @addr: object to be freed
 *
 * Requires: is_kfence_address(addr)
 *
 * Release a KFENCE object and mark it as freed.
 */
/*
 * 中文翻译与补充：
 *   __kfence_free() 把一个已确认属于 KFENCE pool 的对象释放回 KFENCE 状态机。
 *
 * 入参：
 *   @addr: 对象起始地址或可由 KFENCE metadata 识别的地址；调用前必须满足
 *          is_kfence_address(addr)，通常由 kfence_free() 或特殊回滚路径保证。
 *          调用成功后，上层不再拥有可继续访问的活动对象。
 *
 * 返回：
 *   无直接返回值。副作用是更新 metadata 状态、记录 free stack、重新保护对象页
 *   或在 SLAB_TYPESAFE_BY_RCU cache 中通过 call_rcu() 延迟真正归还。
 *
 * 失败/竞态语义：
 *   该函数不是普通地址分类器；传入非 KFENCE 地址属于调用者错误。对于 double free、
 *   UAF 或 RCU 延迟释放场景，真正报告和状态处理由 mm/kfence/core.c 完成。
 */
void __kfence_free(void *addr);

/**
 * kfence_free() - try to release an arbitrary heap object to KFENCE pool
 * @addr: object to be freed
 *
 * Return:
 * * false - object doesn't belong to KFENCE pool and was ignored,
 * * true  - object was released to KFENCE pool.
 *
 * Release a KFENCE object and mark it as freed. May be called on any object,
 * even non-KFENCE objects, to simplify integration of the hooks into the
 * allocator's free codepath. The allocator must check the return value to
 * determine if it was a KFENCE object or not.
 */
/*
 * 中文翻译与补充：
 *   kfence_free() 尝试把任意堆对象交给 KFENCE 释放路径。
 *
 * 入参：
 *   @addr: 借用的待释放地址；可以是普通 slab/kmalloc 对象，也可以是 KFENCE 对象。
 *          本函数先按地址范围分类，只有确认为 KFENCE pool 时才把对象所有权交给
 *          __kfence_free()。
 *
 * 返回：
 *   false 表示 @addr 不属于 KFENCE pool，函数没有任何副作用；调用者必须继续普通
 *         allocator free 流程。
 *   true  表示对象已由 KFENCE 释放逻辑处理；调用者必须停止普通 free，避免把同一
 *         地址再归还给 slab 导致 freelist 污染或 double free。
 *
 * 上下文：
 *   这是 allocator free fast path 的集成钩子。它可安全地接收非 KFENCE 地址，
 *   但调用者必须使用 __must_check 结果完成分流。
 */
static __always_inline __must_check bool kfence_free(void *addr)
{
	/*
	 * 阶段 1：只按 pool 范围做快速分类，不解引用对象。false 是“继续普通释放”，
	 * 不是“释放失败”。
	 */
	if (!is_kfence_address(addr))
		return false;
	/*
	 * 阶段 2：确认属于 KFENCE 后，释放责任转入 KFENCE 状态机。返回 true 后
	 * 调用者不能再触碰普通 slab 的 freelist/metadata。
	 */
	__kfence_free(addr);
	return true;
}

/**
 * kfence_handle_page_fault() - perform page fault handling for KFENCE pages
 * @addr: faulting address
 * @is_write: is access a write
 * @regs: current struct pt_regs (can be NULL, but shows full stack trace)
 *
 * Return:
 * * false - address outside KFENCE pool,
 * * true  - page fault handled by KFENCE, no additional handling required.
 *
 * A page fault inside KFENCE pool indicates a memory error, such as an
 * out-of-bounds access, a use-after-free or an invalid memory access. In these
 * cases KFENCE prints an error message and marks the offending page as
 * present, so that the kernel can proceed.
 */
/*
 * 中文翻译与补充：
 *   kfence_handle_page_fault() 处理落在 KFENCE pool 内的页错误。
 *
 * 入参：
 *   @addr: faulting 虚拟地址，单位字节；函数按地址判断是否属于 KFENCE pool，
 *          并据此寻找相邻或当前对象 metadata。
 *   @is_write: true 表示触发 fault 的访问是写，false 表示读/执行等非写访问；
 *              报告路径用它描述错误访问类型。
 *   @regs: 当前 CPU 的 pt_regs 快照；可为 NULL，但非 NULL 时报告可包含更完整的
 *          现场栈和寄存器线索。本函数只借用该指针，不保存所有权。
 *
 * 返回：
 *   false 表示 @addr 不在 KFENCE pool，架构 fault 处理必须继续普通 page fault 路径；
 *   true  表示 KFENCE 已处理该 fault，通常已经打印报告并临时解除保护页，使内核
 *         可以继续执行当前访问后的路径。
 *
 * 错误分类：
 *   guard/redzone 页 fault 通常表示越界访问；对象页 fault 通常表示 use-after-free
 *   或非法访问。实现会在 metadata 锁下固定报告对象，但为了诊断继续运行，会把
 *   offending page 标为 present。这个返回值不是“内存访问合法”的证明，而是“该
 *   fault 已被 KFENCE 消费”的控制流信号。
 */
bool __must_check kfence_handle_page_fault(unsigned long addr, bool is_write, struct pt_regs *regs);

#ifdef CONFIG_PRINTK
struct kmem_obj_info;
/**
 * __kfence_obj_info() - fill kmem_obj_info struct
 * @kpp: kmem_obj_info to be filled
 * @object: the object
 * @slab: the slab
 *
 * Return:
 * * false - not a KFENCE object
 * * true - a KFENCE object, filled @kpp
 *
 * Copies information to @kpp for KFENCE objects.
 */
/*
 * 中文翻译与补充：
 *   __kfence_obj_info() 为 printk/slab 诊断路径填充 KFENCE 对象信息。
 *
 * 入参：
 *   @kpp:   调用者提供的输出结构；成功识别 KFENCE 对象时被填充对象地址、cache、
 *           slab、分配栈和可能的释放栈。调用者保持结构体所有权。
 *   @object:借用的疑似对象地址；函数按地址反查 KFENCE metadata，不释放对象。
 *   @slab:  调用者已知的 slab 指针或上下文信息；作为诊断字段写入 @kpp，不转移引用。
 *
 * 返回：
 *   false 表示 @object 不是 KFENCE 对象，调用者应继续普通 __kmem_obj_info()；
 *   true  表示 KFENCE 已经处理该对象信息，@kpp 至少包含请求指针；若 metadata
 *         状态有效，还包含分配/释放栈等更完整诊断信息。
 *
 * 并发语义：
 *   实现会在需要读取 metadata 详细字段时取 meta->lock；对从未使用过的对象只填
 *   kp_ptr 并 WARN，因为其他字段可能是无意义数据。该接口服务于崩溃/告警输出，
 *   不为对象建立长期引用，也不改变对象生命周期。
 */
bool __kfence_obj_info(struct kmem_obj_info *kpp, void *object, struct slab *slab);
#endif

#else /* CONFIG_KFENCE */

#define kfence_sample_interval	(0)
/*
 * CONFIG_KFENCE=n 时的契约：
 *   所有 public helper 都退化为零成本或近零成本 stub。返回值统一表达“没有对象由
 *   KFENCE 接管/没有 fault 由 KFENCE 处理/没有诊断信息由 KFENCE 填充”，让分配器、
 *   fault 和 printk 调用点无需在每个位置再包一层 #ifdef。
 *
 * 这些 stub 的入参仍保留完整签名，是为了让调用点在两种配置下共享同一类型检查和
 * 控制流。它们不解引用参数、不取得引用、不睡眠、不产生副作用。
 */

/*
 * is_kfence_address() stub
 *
 * 入参 @addr: 任意借用地址，未使用。
 * 返回 false: 当前内核没有 KFENCE pool，所有地址都必须继续普通路径。
 */
static inline bool is_kfence_address(const void *addr) { return false; }
/*
 * kfence_alloc_pool_and_metadata() stub
 *
 * 入参：无。返回：无直接返回值。副作用：无；启动路径无需分配 KFENCE pool。
 */
static inline void kfence_alloc_pool_and_metadata(void) { }
/*
 * kfence_init() stub
 *
 * 入参：无。返回：无直接返回值。副作用：无；不会启用静态分支或采样定时器。
 */
static inline void kfence_init(void) { }
/*
 * kfence_shutdown_cache() stub
 *
 * 入参 @s: 借用的待销毁 cache，未使用。
 * 返回：无直接返回值。副作用：无；没有 KFENCE metadata 需要扫描。
 */
static inline void kfence_shutdown_cache(struct kmem_cache *s) { }
/*
 * kfence_alloc() stub
 *
 * 入参 @s/@size/@flags: 保持与启用配置相同的分配契约，均未使用。
 * 返回 NULL: 本次未由 KFENCE 接管，调用者继续普通分配路径。
 */
static inline void *kfence_alloc(struct kmem_cache *s, size_t size, gfp_t flags) { return NULL; }
/*
 * kfence_ksize() stub
 *
 * 入参 @addr: 借用对象地址，未使用。
 * 返回 0: 不是 KFENCE 对象，调用者继续普通 __ksize()/诊断路径。
 */
static inline size_t kfence_ksize(const void *addr) { return 0; }
/*
 * kfence_object_start() stub
 *
 * 入参 @addr: 借用地址，未使用。
 * 返回 NULL: 无 KFENCE metadata 可反查对象起点。
 */
static inline void *kfence_object_start(const void *addr) { return NULL; }
/*
 * __kfence_free() stub
 *
 * 入参 @addr: 按启用配置本应是 KFENCE 地址；关闭配置下未使用。
 * 返回：无直接返回值。副作用：无；正常调用点应通过 kfence_free() 得到 false。
 */
static inline void __kfence_free(void *addr) { }
/*
 * kfence_free() stub
 *
 * 入参 @addr: 任意待释放地址，未使用。
 * 返回 false: 对象未由 KFENCE 释放，调用者必须继续普通 free。
 */
static inline bool __must_check kfence_free(void *addr) { return false; }
/*
 * kfence_handle_page_fault() stub
 *
 * 入参 @addr/@is_write/@regs: 保持 fault handler 集成点签名，均未使用。
 * 返回 false: fault 不属于 KFENCE，架构 page fault 处理继续执行普通路径。
 */
static inline bool __must_check kfence_handle_page_fault(unsigned long addr, bool is_write,
							 struct pt_regs *regs)
{
	return false;
}

#ifdef CONFIG_PRINTK
struct kmem_obj_info;
/*
 * __kfence_obj_info() stub
 *
 * 入参 @kpp/@object/@slab: 保持 printk 对象诊断集成点签名，均未使用。
 * 返回 false: KFENCE 未填充信息，调用者继续普通 slab 对象信息路径。
 */
static inline bool __kfence_obj_info(struct kmem_obj_info *kpp, void *object, struct slab *slab)
{
	return false;
}
#endif

#endif

#endif /* _LINUX_KFENCE_H */
