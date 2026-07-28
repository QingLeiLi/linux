// SPDX-License-Identifier: GPL-2.0
/*
 * slab_common.c 学习导读
 *
 * 中文学习注释模型：OpenAI GPT-5 Codex。
 *
 * 本文件放置不依赖具体 SLAB/SLUB 后端策略的公共控制层：它把
 * kmem_cache 的创建、合并、销毁，通用 kmalloc 尺寸类的引导建立，
 * /proc/slabinfo，以及“等待 RCU 宽限期后再释放”的 kvfree_rcu 批处理
 * 协议连接起来。对象如何装入 slab、空闲对象如何组织等后端细节由
 * mm/slub.c 等文件实现，不在这里决定。
 *
 * 三条主线：
 *   kmem_cache_create() -> __kmem_cache_create_args()
 *       -> 查找可合并 cache，或 create_cache() -> do_kmem_cache_create()
 *   引导期 create_boot_cache()/create_kmalloc_caches()
 *       -> kmalloc_caches[type][size-index] 变为可用 -> slab_state = UP
 *   kfree_rcu()/kvfree_rcu() -> kvfree_call_rcu()
 *       -> 每 CPU 暂存 -> RCU work 等待宽限期 -> kfree()/vfree()
 *
 * 核心生命周期：
 *   struct kmem_cache 由公共层分配描述符，后端完成布局和节点状态；
 *   加入 slab_caches 后可被查找和观测；合并以 refcount 表示多个逻辑
 *   名称共享同一物理 cache；销毁先阻断/排空异步释放，再从全局表摘除，
 *   最后释放 sysfs、后端状态、名称和描述符。
 *
 * 并发模型：
 *   slab_mutex 串行化 slab_caches、合并引用和 cache 创建/销毁；
 *   CPU hotplug 读锁使后端逐 CPU 状态在 shutdown 期间保持稳定；
 *   kvfree_rcu 使用每 CPU raw spinlock 保护批次容器，用 RCU GP snapshot
 *   证明旧读者已经离开，再在可睡眠的 workqueue 上执行实际释放。
 *
 * 方案权衡：cache 合并减少元数据和碎片，但会共享对象布局与调试属性，
 * 因而带构造器、usercopy 区域、特殊元数据或隔离要求的 cache 禁止合并；
 * kvfree_rcu 批处理摊薄宽限期和回调成本，却增加每 CPU 缓存、延迟与
 * barrier/shrinker 协调复杂度，并在内存压力下退化到链表或同步释放。
 */
/*
 * Slab allocator functions that are independent of the allocator strategy
 *
 * (C) 2012 Christoph Lameter <cl@gentwo.org>
 */
/*
 * 本文件包含“与具体分配器策略无关的 slab 分配器函数”。版权块保持
 * 上游原样；这里的公共层同时服务于配置所选择的实际 slab 后端。
 */
#include <linux/slab.h>

#include <linux/mm.h>
#include <linux/poison.h>
#include <linux/interrupt.h>
#include <linux/memory.h>
#include <linux/cache.h>
#include <linux/compiler.h>
#include <linux/kfence.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/dma-mapping.h>
#include <linux/swiotlb.h>
#include <linux/proc_fs.h>
#include <linux/debugfs.h>
#include <linux/kmemleak.h>
#include <linux/kasan.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/page.h>
#include <linux/memcontrol.h>
#include <linux/stackdepot.h>
#include <trace/events/rcu.h>

#include "../kernel/rcu/rcu.h"
#include "internal.h"
#include "slab.h"

#define CREATE_TRACE_POINTS
#include <trace/events/kmem.h>

enum slab_state slab_state;
LIST_HEAD(slab_caches);
DEFINE_MUTEX(slab_mutex);
struct kmem_cache *kmem_cache;
/*
 * 全局对象地图：
 *   slab_state  引导状态机；读者据此判断通用 kmalloc/cache 服务是否可用。
 *   slab_caches 已发布 cache 的全局链表；增删和稳定遍历受 slab_mutex 保护。
 *   slab_mutex  串行化 cache 名称、合并 refcount、链表和 shutdown 转换；
 *               它是可睡眠锁，因此 cache 创建/销毁不能在中断上下文执行。
 *   kmem_cache  “cache 描述符自身”的引导 cache；create_cache() 从中分配
 *               struct kmem_cache，形成 slab 元数据自举。
 */

/*
 * Set of flags that will prevent slab merging.
 * Any flag that adds per-object metadata should be included,
 * since slab merging can update s->inuse that affects the metadata layout.
 */
/*
 * 下列标志会阻止 slab cache 合并。任何增加逐对象元数据的标志都必须
 * 列入，因为合并可能扩大 s->inuse，继而改变空闲指针、调试信息等布局；
 * 让布局契约不同的逻辑 cache 共用存储会造成越界或错误解释对象。
 */
#define SLAB_NEVER_MERGE (SLAB_DEBUG_FLAGS | SLAB_TYPESAFE_BY_RCU | \
		SLAB_NOLEAKTRACE | SLAB_FAILSLAB | SLAB_NO_MERGE | \
		SLAB_OBJ_EXT_IN_OBJ)

#define SLAB_MERGE_SAME (SLAB_RECLAIM_ACCOUNT | SLAB_CACHE_DMA | \
			 SLAB_CACHE_DMA32 | SLAB_ACCOUNT)
/*
 * SLAB_MERGE_SAME 是允许合并时仍必须逐位相同的资源域属性：可回收统计、
 * DMA/DMA32 内存域和 memcg 记账语义不能因共享物理 cache 而混淆。
 */

/*
 * Merge control. If this is set then no merging of slab caches will occur.
 */
/*
 * 合并总开关。置位后任何 slab cache 都不会合并；初值由构建配置给出，
 * 下面的早期启动参数可在创建 cache 之前覆盖它。引导后只读使用。
 */
static bool slab_nomerge = !IS_ENABLED(CONFIG_SLAB_MERGE_DEFAULT);

/*
 * setup_slab_nomerge() - 处理 slab/slub_nomerge 早期启动参数。
 * @str: 参数尾部字符串，本开关不消费其内容，是借用指针。
 * 引导期单线程调用、可直接写 slab_nomerge；返回 1 表示参数已处理。
 */
static int __init setup_slab_nomerge(char *str)
{
	slab_nomerge = true;
	return 1;
}

/*
 * setup_slab_merge() - 处理 slab/slub_merge 早期启动参数。
 * @str: 未使用的借用字符串。
 * 与上函数相反地允许后续 cache 合并；返回 1 表示识别成功。
 */
static int __init setup_slab_merge(char *str)
{
	slab_nomerge = false;
	return 1;
}

__setup_param("slub_nomerge", slub_nomerge, setup_slab_nomerge, 0);
__setup_param("slub_merge", slub_merge, setup_slab_merge, 0);

__setup("slab_nomerge", setup_slab_nomerge);
__setup("slab_merge", setup_slab_merge);

/*
 * Determine the size of a slab object
 */
/*
 * 返回 slab 对外承诺的对象有效字节数，而非含对齐、空闲指针或调试元数据的
 * s->size。@s 是调用者保证存活的借用 cache 指针；无锁只读，返回字节数，
 * 不转移引用、无副作用，也不睡眠。
 */
unsigned int kmem_cache_size(struct kmem_cache *s)
{
	return s->object_size;
}
EXPORT_SYMBOL(kmem_cache_size);

#ifdef CONFIG_DEBUG_VM

/*
 * kmem_cache_is_duplicate_name() - 在已发布 cache 中查重名称。
 * @name: NUL 结尾借用字符串；调用者持有 slab_mutex，保证链表和名称稳定。
 * 返回 true 表示至少已有同名项；仅用于诊断，不取得 cache 引用。
 */
static bool kmem_cache_is_duplicate_name(const char *name)
{
	struct kmem_cache *s;

	list_for_each_entry(s, &slab_caches, list) {
		if (!strcmp(s->name, name))
			return true;
	}

	return false;
}

/*
 * kmem_cache_sanity_check() - 在创建或合并前验证公共入口约束。
 * @name: cache 名称，必须非 NULL；@size: 请求对象字节数。
 * 调用者持有 slab_mutex、位于可睡眠进程上下文。返回 0 可继续，-EINVAL
 * 表示硬性无效；重名和空格只 WARN，因为历史接口仍允许继续创建。
 */
static int kmem_cache_sanity_check(const char *name, unsigned int size)
{
	if (!name || in_interrupt() || size > KMALLOC_MAX_SIZE) {
		pr_err("kmem_cache_create(%s) integrity check failed\n", name);
		return -EINVAL;
	}

	/* Duplicate names will confuse slabtop, et al */
	/* 重名会使 slabtop 等按名称展示/解析的工具无法区分逻辑 cache。 */
	WARN(kmem_cache_is_duplicate_name(name),
			"kmem_cache of name '%s' already exists\n", name);

	WARN_ON(strchr(name, ' '));	/* It confuses parsers */
	/* 名称中的空格会破坏以空白分列的 /proc/slabinfo 解析器。 */
	return 0;
}
#else
/*
 * 非 DEBUG_VM 构建省略昂贵诊断。参数仍由上层接口约束；该薄 stub 总返回
 * 0、无副作用且不睡眠，使发布构建不为调试扫描付费。
 */
static inline int kmem_cache_sanity_check(const char *name, unsigned int size)
{
	return 0;
}
#endif

/*
 * Figure out what the alignment of the objects will be given a set of
 * flags, a user specified alignment and the size of the objects.
 */
/*
 * 根据 @flags、调用者请求 @align 和对象 @size（均以字节计）求最终对齐。
 * 结果至少满足架构 slab 最小对齐和指针存储要求，并返回 sizeof(void *)
 * 的整数倍；纯计算、无所有权变化且不睡眠。
 */
static unsigned int calculate_alignment(slab_flags_t flags,
		unsigned int align, unsigned int size)
{
	/*
	 * If the user wants hardware cache aligned objects then follow that
	 * suggestion if the object is sufficiently large.
	 *
	 * The hardware cache alignment cannot override the specified
	 * alignment though. If that is greater then use it.
	 */
	/*
	 * SLAB_HWCACHE_ALIGN 表示希望按硬件 cache line 对齐；小对象逐次减半，
	 * 避免为了避免 false sharing 而付出过高内部碎片。用户显式要求的更大
	 * 对齐不能被此启发式覆盖，因此取二者最大值。
	 */
	if (flags & SLAB_HWCACHE_ALIGN) {
		unsigned int ralign;

		ralign = cache_line_size();
		while (size <= ralign / 2)
			ralign /= 2;
		align = max(align, ralign);
	}

	align = max(align, arch_slab_minalign());

	return ALIGN(align, sizeof(void *));
}

/*
 * Find a mergeable slab cache
 */
/*
 * slab_unmergeable() - 判断已存在 cache 是否禁止作为合并目标。
 * @s: 在 slab_mutex 保护下借用的已发布 cache。
 * 返回 1 表示布局、构造器、usercopy、引导状态或全局策略要求隔离；返回
 * 0 仅表示“可继续比较”，并不保证尺寸/标志兼容。无引用转移。
 */
int slab_unmergeable(struct kmem_cache *s)
{
	if (slab_nomerge || (s->flags & SLAB_NEVER_MERGE))
		return 1;

	if (s->ctor)
		return 1;

#ifdef CONFIG_HARDENED_USERCOPY
	if (s->usersize)
		return 1;
#endif

	/*
	 * We may have set a slab to be unmergeable during bootstrap.
	 */
	/*
	 * 引导 cache 曾以负 refcount 作为“暂不可合并”哨兵；在完整创建流程
	 * 建立稳定布局前，不能让普通逻辑名称别名到它。
	 */
	if (s->refcount < 0)
		return 1;

	return 0;
}

/*
 * slab_args_unmergeable() - 在尚无 struct kmem_cache 时检查创建参数。
 * @args: 借用的完整创建参数；@flags: 已经后端规范化的 slab 标志。
 * 返回 true 代表必须创建独立 cache；只读参数，不睡眠、无副作用。
 */
bool slab_args_unmergeable(struct kmem_cache_args *args, slab_flags_t flags)
{
	if (slab_nomerge)
		return true;

	if (args->ctor)
		return true;

	if (IS_ENABLED(CONFIG_HARDENED_USERCOPY) && args->usersize)
		return true;

	if (flags & SLAB_NEVER_MERGE)
		return true;

	return false;
}

/*
 * find_mergeable() - 为新逻辑 cache 查找布局兼容的现有物理 cache。
 * @size: 请求对象字节数；@flags: 请求语义；@name: 借用名称，供后端调整
 * 标志；@args: 构造器、对齐和 usercopy 等借用参数。
 * 调用者持有 slab_mutex。返回借用的 cache 或 NULL，不增加 refcount；
 * 真正建立别名由 __kmem_cache_alias() 完成。
 */
static struct kmem_cache *find_mergeable(unsigned int size, slab_flags_t flags,
		const char *name, struct kmem_cache_args *args)
{
	struct kmem_cache *s;
	unsigned int align;

	flags = kmem_cache_flags(flags, name);
	if (slab_args_unmergeable(args, flags))
		return NULL;

	size = ALIGN(size, sizeof(void *));
	align = calculate_alignment(flags, args->align, size);
	size = ALIGN(size, align);
	/*
	 * 先把请求规范化为真实可放置尺寸，再逆序扫描，使近期创建、通常更贴近
	 * 当前策略的 cache 优先。候选必须语义域相同、对齐兼容，而且额外浪费
	 * 小于一个指针大小；否则宁可创建独立 cache。
	 */

	list_for_each_entry_reverse(s, &slab_caches, list) {
		if (slab_unmergeable(s))
			continue;

		if (size > s->size)
			continue;

		if ((flags & SLAB_MERGE_SAME) != (s->flags & SLAB_MERGE_SAME))
			continue;
		/*
		 * Check if alignment is compatible.
		 * Courtesy of Adrian Drzewiecki
		 */
		/*
		 * 候选 stride 必须是请求对齐的整数倍，否则第二个及后续对象地址
		 * 会偏离契约。该检查归功于 Adrian Drzewiecki。
		 */
		if ((s->size & ~(align - 1)) != s->size)
			continue;

		if (s->size - size >= sizeof(void *))
			continue;

		return s;
	}
	return NULL;
}

/*
 * create_cache() - 分配并发布一个全新的 kmem_cache。
 * @name: 已由调用者取得持久所有权的名称；成功后所有权转给 cache。
 * @object_size: 对外对象字节数；@args/@flags: 借用的布局和策略。
 * 调用者持有 slab_mutex，进程上下文中可睡眠。
 * 成功返回持有的 cache 指针，refcount=1 且已加入 slab_caches；失败返回
 * ERR_PTR(-EINVAL/-ENOMEM/后端 errno)，名称仍由调用者释放。
 */
static struct kmem_cache *create_cache(const char *name,
				       unsigned int object_size,
				       struct kmem_cache_args *args,
				       slab_flags_t flags)
{
	struct kmem_cache *s;
	int err;

	/* If a custom freelist pointer is requested make sure it's sane. */
	/*
	 * 自定义 freeptr 必须落在对象内、自然对齐；非 TYPESAFE_BY_RCU cache
	 * 若无构造器，空闲指针可能覆盖调用者仍依赖的对象内容，因而拒绝。
	 */
	err = -EINVAL;
	if (args->use_freeptr_offset &&
	    (args->freeptr_offset >= object_size ||
	     (!(flags & SLAB_TYPESAFE_BY_RCU) && !args->ctor) ||
	     !IS_ALIGNED(args->freeptr_offset, __alignof__(freeptr_t))))
		goto out;

	err = -ENOMEM;
	s = kmem_cache_zalloc(kmem_cache, GFP_KERNEL);
	if (!s)
		goto out;
	err = do_kmem_cache_create(s, name, object_size, args, flags);
	if (err)
		goto out_free_cache;

	s->refcount = 1;
	/*
	 * 后端已完整初始化后才加入全局链表，这是发布边界：此后 slabinfo、
	 * 合并查找和销毁路径可观察它。slab_mutex 阻止并发看到半初始化对象。
	 */
	list_add(&s->list, &slab_caches);
	return s;

out_free_cache:
	/* 后端创建失败，描述符尚未发布，只需归还自举 cache。 */
	kmem_cache_free(kmem_cache, s);
out:
	/* ERR_PTR 保留精确 errno，便于上层统一释放尚未转移的名称。 */
	return ERR_PTR(err);
}

/*
 * __kmem_cache_alias() - 尝试把新名称合并到已有 cache。
 * 参数语义同 find_mergeable()；调用者持有 slab_mutex。
 * 成功返回共享 cache 并增加逻辑 refcount，同时扩大零填充/在用边界；
 * 失败返回 NULL，调用者继续创建独立 cache。sysfs alias 失败只影响展示。
 */
static struct kmem_cache *
__kmem_cache_alias(const char *name, unsigned int size, slab_flags_t flags,
		   struct kmem_cache_args *args)
{
	struct kmem_cache *s;

	s = find_mergeable(size, flags, name, args);
	if (s) {
		if (sysfs_slab_alias(s, name))
			pr_err("SLUB: Unable to add cache alias %s to sysfs\n",
			       name);

		s->refcount++;

		/*
		 * Adjust the object sizes so that we clear
		 * the complete object on kzalloc.
		 */
		/*
		 * 合并后物理 stride 不变，但逻辑请求可能更大；提升 object_size 和
		 * inuse，保证 kzalloc 清零整个最大逻辑对象，且后端元数据不会落入
		 * 新增有效区。它也是为何含逐对象元数据的 cache 禁止随意合并。
		 */
		s->object_size = max(s->object_size, size);
		s->inuse = max(s->inuse, ALIGN(size, sizeof(void *)));
	}

	return s;
}

/**
 * __kmem_cache_create_args - Create a kmem cache.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @object_size: The size of objects to be created in this cache.
 * @args: Additional arguments for the cache creation (see
 *        &struct kmem_cache_args).
 * @flags: See the descriptions of individual flags. The common ones are listed
 *         in the description below.
 *
 * Not to be called directly, use the kmem_cache_create() wrapper with the same
 * parameters.
 *
 * Commonly used @flags:
 *
 * &SLAB_ACCOUNT - Account allocations to memcg.
 *
 * &SLAB_HWCACHE_ALIGN - Align objects on cache line boundaries.
 *
 * &SLAB_RECLAIM_ACCOUNT - Objects are reclaimable.
 *
 * &SLAB_TYPESAFE_BY_RCU - Slab page (not individual objects) freeing delayed
 * by a grace period - see the full description before using.
 *
 * Context: Cannot be called within a interrupt, but can be interrupted.
 *
 * Return: a pointer to the cache on success, NULL on failure.
 */
/*
 * 创建 kmem cache 的公共核心入口。
 *
 * @name 为借用名称，成功时复制为持久名称；@object_size 是对象有效字节数；
 * @args 是可写的输入/输出创建参数（本函数可能规范化 align、清除非法
 * usercopy 范围）；@flags 控制记账、对齐、RCU 与调试策略。
 * 必须在可睡眠的非中断上下文调用，入口不持 slab_mutex。
 *
 * 阶段为：启用必要调试基础设施 -> 强制特殊 cache 隔离 -> 加锁校验 ->
 * 尝试合并 -> 复制名称并创建 -> 解锁后报告。成功返回借用给调用者使用的
 * 已发布 cache；失败统一返回 NULL，SLAB_PANIC 时直接 panic。所有临时
 * 名称/描述符在失败路径释放，不遗留半发布项。
 */
struct kmem_cache *__kmem_cache_create_args(const char *name,
					    unsigned int object_size,
					    struct kmem_cache_args *args,
					    slab_flags_t flags)
{
	struct kmem_cache *s = NULL;
	const char *cache_name;
	int err;

#ifdef CONFIG_SLUB_DEBUG
	/*
	 * If no slab_debug was enabled globally, the static key is not yet
	 * enabled by setup_slub_debug(). Enable it if the cache is being
	 * created with any of the debugging flags passed explicitly.
	 * It's also possible that this is the first cache created with
	 * SLAB_STORE_USER and we should init stack_depot for it.
	 */
	/*
	 * 若全局 slab_debug 未开启，显式调试标志仍需打开 static key；首次
	 * STORE_USER 还要准备 stack depot。两者可能分配/修改全局状态，故在
	 * 获取 slab_mutex 前完成，避免扩大公共 cache 锁的职责。
	 */
	if (flags & SLAB_DEBUG_FLAGS)
		static_branch_enable(&slub_debug_enabled);
	if (flags & SLAB_STORE_USER)
		stack_depot_init();
#else
	flags &= ~SLAB_DEBUG_FLAGS;
#endif

	/*
	 * Caches with specific capacity are special enough. It's simpler to
	 * make them unmergeable.
	 */
	/*
	 * 指定 sheaf 容量的 cache 带有独立的逐 CPU 容量契约，直接禁止合并
	 * 比证明两个 cache 的全部容量语义等价更可靠。
	 */
	if (args->sheaf_capacity)
		flags |= SLAB_NO_MERGE;

	mutex_lock(&slab_mutex);
	/* 从这里到 out_unlock，名称查重、合并 refcount 和全局链表保持稳定。 */

	err = kmem_cache_sanity_check(name, object_size);
	if (err) {
		goto out_unlock;
	}

	if (flags & ~SLAB_FLAGS_PERMITTED) {
		err = -EINVAL;
		goto out_unlock;
	}

	/* Fail closed on bad usersize of useroffset values. */
	/*
	 * usercopy 区域非法时采用 fail-closed：清成“没有允许复制的区域”，
	 * 而不是保留可能越过 object_size 的范围。WARN 暴露调用者错误。
	 */
	if (!IS_ENABLED(CONFIG_HARDENED_USERCOPY) ||
	    WARN_ON(!args->usersize && args->useroffset) ||
	    WARN_ON(object_size < args->usersize ||
		    object_size - args->usersize < args->useroffset))
		args->usersize = args->useroffset = 0;

	s = __kmem_cache_alias(name, object_size, flags, args);
	if (s)
		goto out_unlock;

	cache_name = kstrdup_const(name, GFP_KERNEL);
	if (!cache_name) {
		err = -ENOMEM;
		goto out_unlock;
	}

	args->align = calculate_alignment(flags, args->align, object_size);
	/* 名称和最终对齐准备完成；create_cache 成功后接管 cache_name。 */
	s = create_cache(cache_name, object_size, args, flags);
	if (IS_ERR(s)) {
		err = PTR_ERR(s);
		kfree_const(cache_name);
	}

out_unlock:
	mutex_unlock(&slab_mutex);
	/* 错误报告可能打印栈或 panic，放在锁外避免持 mutex 执行重操作。 */

	if (err) {
		if (flags & SLAB_PANIC)
			panic("%s: Failed to create slab '%s'. Error %d\n",
				__func__, name, err);
		else {
			pr_warn("%s(%s) failed with error %d\n",
				__func__, name, err);
			dump_stack();
		}
		return NULL;
	}
	return s;
}
EXPORT_SYMBOL(__kmem_cache_create_args);

static struct kmem_cache *kmem_buckets_cache __ro_after_init;
/*
 * 分配 kmem_buckets 描述数组的专用 cache；只在引导创建一次，之后
 * __ro_after_init 阻止指针被改写。NULL 表示功能尚未准备好或创建失败。
 */

/**
 * kmem_buckets_create - Create a set of caches that handle dynamic sized
 *			 allocations via kmem_buckets_alloc()
 * @name: A prefix string which is used in /proc/slabinfo to identify this
 *	  cache. The individual caches with have their sizes as the suffix.
 * @flags: SLAB flags (see kmem_cache_create() for details).
 * @useroffset: Starting offset within an allocation that may be copied
 *		to/from userspace.
 * @usersize: How many bytes, starting at @useroffset, may be copied
 *		to/from userspace.
 * @ctor: A constructor for the objects, run when new allocations are made.
 *
 * Cannot be called within an interrupt, but can be interrupted.
 *
 * Return: a pointer to the cache on success, NULL on failure. When
 * CONFIG_SLAB_BUCKETS is not enabled, ZERO_SIZE_PTR is returned, and
 * subsequent calls to kmem_buckets_alloc() will fall back to kmalloc().
 * (i.e. callers only need to check for NULL on failure.)
 */
/*
 * 为动态尺寸分配建立一组彼此隔离的尺寸 cache。
 *
 * @name 是各 cache 名称前缀；@flags 是共同 slab 策略；@useroffset 和
 * @usersize 以字节指定允许与用户态复制的区间；@ctor 是可空构造器，cache
 * 保存并在新对象建立时调用。均不发生字符串/回调所有权转移。
 *
 * 非中断、可睡眠上下文调用。成功返回持有的 kmem_buckets 表，调用者随后
 * 交给 kmem_buckets_alloc()；未编译该能力时返回 ZERO_SIZE_PTR 作为非空
 * 哨兵并退化到 kmalloc；分配/任一子 cache 创建失败返回 NULL，已创建项
 * 按 mask 精确回滚，不留下共享指针的重复销毁。
 */
kmem_buckets *kmem_buckets_create(const char *name, slab_flags_t flags,
				  unsigned int useroffset,
				  unsigned int usersize,
				  void (*ctor)(void *))
{
	unsigned long mask = 0;
	unsigned int idx;
	kmem_buckets *b;

	BUILD_BUG_ON(ARRAY_SIZE(kmalloc_caches[KMALLOC_NORMAL]) > BITS_PER_LONG);

	/*
	 * When the separate buckets API is not built in, just return
	 * a non-NULL value for the kmem_buckets pointer, which will be
	 * unused when performing allocations.
	 */
	/*
	 * 未启用独立 buckets API 时返回不会解引用的非 NULL 哨兵；这使调用者
	 * 只把 NULL 当失败，实际分配路径自动回落到通用 kmalloc。
	 */
	if (!IS_ENABLED(CONFIG_SLAB_BUCKETS))
		return ZERO_SIZE_PTR;

	if (WARN_ON(!kmem_buckets_cache))
		return NULL;

	b = kmem_cache_alloc(kmem_buckets_cache, GFP_KERNEL|__GFP_ZERO);
	if (WARN_ON(!b))
		return NULL;

	flags |= SLAB_NO_MERGE;
	/*
	 * b 是零初始化的尺寸索引表；强制 NO_MERGE，保证每个 usercopy 范围与
	 * 构造器属于本 buckets 集合，不会别名到普通 kmalloc cache。
	 */

	for (idx = 0; idx < ARRAY_SIZE(kmalloc_caches[KMALLOC_NORMAL]); idx++) {
		char *short_size, *cache_name;
		unsigned int cache_useroffset, cache_usersize;
		unsigned int size, aligned_idx;

		if (!kmalloc_caches[KMALLOC_NORMAL][idx])
			continue;

		size = kmalloc_caches[KMALLOC_NORMAL][idx]->object_size;
		if (!size)
			continue;

		short_size = strchr(kmalloc_caches[KMALLOC_NORMAL][idx]->name, '-');
		if (WARN_ON(!short_size))
			goto fail;

		if (useroffset >= size) {
			cache_useroffset = 0;
			cache_usersize = 0;
		} else {
			cache_useroffset = useroffset;
			cache_usersize = min(size - cache_useroffset, usersize);
		}
		/*
		 * 每个尺寸只能允许对象内部的 usercopy 子区间；当起点越过对象时
		 * 禁用，否则把长度截到对象末尾，避免白名单越界。
		 */

		aligned_idx = __kmalloc_index(size, false);
		if (!(*b)[aligned_idx]) {
			cache_name = kasprintf(GFP_KERNEL, "%s-%s", name, short_size + 1);
			if (WARN_ON(!cache_name))
				goto fail;
			(*b)[aligned_idx] = kmem_cache_create_usercopy(cache_name, size,
					0, flags, cache_useroffset,
					cache_usersize, ctor);
			kfree(cache_name);
			if (WARN_ON(!(*b)[aligned_idx]))
				goto fail;
			set_bit(aligned_idx, &mask);
		}
		if (idx != aligned_idx)
			(*b)[idx] = (*b)[aligned_idx];
		/*
		 * 多个原始索引可能因架构对齐映射到同一实际尺寸。只创建一次，
		 * mask 仅记录真正拥有的 cache；别名槽不承担第二份销毁责任。
		 */
	}

	return b;

fail:
	/* 只销毁 mask 标记的独立 cache，再释放 buckets 表，完成精确回滚。 */
	for_each_set_bit(idx, &mask, ARRAY_SIZE(kmalloc_caches[KMALLOC_NORMAL]))
		kmem_cache_destroy((*b)[idx]);
	kmem_cache_free(kmem_buckets_cache, b);

	return NULL;
}
EXPORT_SYMBOL(kmem_buckets_create);

/*
 * For a given kmem_cache, kmem_cache_destroy() should only be called
 * once or there will be a use-after-free problem. The actual deletion
 * and release of the kobject does not need slab_mutex or cpu_hotplug_lock
 * protection. So they are now done without holding those locks.
 */
/*
 * 同一 kmem_cache 只能进入一次最终销毁，否则会 use-after-free。kobject
 * 的实际删除和释放不需要 slab_mutex/cpu_hotplug_lock，因此拆到锁外；
 * 这也避免 sysfs/RCU 等可睡眠等待扩大锁区。@s 的最终所有权在此消耗。
 */
static void kmem_cache_release(struct kmem_cache *s)
{
	/*
	 * 阶段 1：先让 KFENCE 扫描这个 cache 的专用池对象。KFENCE 对象不挂在普通
	 * slab 列表上，若直接释放 cache，metadata->cache 可能变成悬空指针；这里把
	 * 活动对象转成 zombie，并清理已 freed 对象对 @s 的借用。
	 */
	kfence_shutdown_cache(s);
	/*
	 * 阶段 2：再释放普通 cache 载体。支持 sysfs 的配置需要走 kobject 生命周期，
	 * 否则直接释放 kmem_cache；这一步消耗 @s 的最终所有权。
	 */
	if (__is_defined(SLAB_SUPPORTS_SYSFS) && slab_state >= FULL)
		sysfs_slab_release(s);
	else
		slab_kmem_cache_release(s);
}

/*
 * slab_kmem_cache_release() - 完成 cache 描述符最终回收。
 * @s: 已摘除、后端停止且不再被引用的 cache；调用者转移最终所有权。
 * 依次释放后端状态、持久名称和自举描述符；无返回值，完成后不可再使用
 * @s。不得在持有 slab_mutex 时调用。
 */
void slab_kmem_cache_release(struct kmem_cache *s)
{
	__kmem_cache_release(s);
	kfree_const(s->name);
	kmem_cache_free(kmem_cache, s);
}

/*
 * kmem_cache_destroy() - 撤销一个逻辑 cache 引用，必要时最终销毁。
 * @s: 可空 cache；有效参数代表调用者的一份创建/别名引用。
 * 必须在可睡眠进程上下文调用，入口不持 slab_mutex。
 *
 * 先排空可能仍引用对象的异步释放；随后在 CPU hotplug 读锁和 slab_mutex
 * 下减 refcount。仍有别名时仅归还本引用；归零时关闭 KASAN/后端并从
 * slab_caches 摘除。锁外解除 sysfs/debugfs 和 RCU 延迟，只有后端确认
 * 无活对象才释放描述符；否则保留它以避免 UAF。
 */
void kmem_cache_destroy(struct kmem_cache *s)
{
	int err;

	if (unlikely(!s) || !kasan_check_byte(s))
		return;

	/* in-flight kfree_rcu()'s may include objects from our cache */
	/*
	 * 调用者虽已逻辑释放对象，kfree_rcu 实际回收可能尚未执行；先销毁
	 * cache 会使回调访问失效元数据，因此必须在 refcount 决策前排空。
	 */
	kvfree_rcu_barrier_on_cache(s);

	if (IS_ENABLED(CONFIG_SLUB_RCU_DEBUG) &&
	    (s->flags & SLAB_TYPESAFE_BY_RCU)) {
		/*
		 * Under CONFIG_SLUB_RCU_DEBUG, when objects in a
		 * SLAB_TYPESAFE_BY_RCU slab are freed, SLUB will internally
		 * defer their freeing with call_rcu().
		 * Wait for such call_rcu() invocations here before actually
		 * destroying the cache.
		 *
		 * It doesn't matter that we haven't looked at the slab refcount
		 * yet - slabs with SLAB_TYPESAFE_BY_RCU can't be merged, so
		 * the refcount should be 1 here.
		 */
		/*
		 * CONFIG_SLUB_RCU_DEBUG 下，TYPESAFE_BY_RCU 对象 free 由 SLUB
		 * 内部 call_rcu 延迟，故此处等待全部旧回调。尚未检查 refcount
		 * 也安全，因为此类 cache 禁止合并，按契约应只有一份引用。
		 */
		rcu_barrier();
	}

	/* Wait for deferred work from kmalloc/kfree_nolock() */
	/* 等待 kmalloc/kfree_nolock 的延迟工作，封闭另一条异步访问入口。 */
	defer_free_barrier();

	cpus_read_lock();
	mutex_lock(&slab_mutex);
	/*
	 * hotplug 读锁稳定后端逐 CPU 状态；slab_mutex 串行化 refcount、
	 * slab_caches 和 shutdown。RCU barrier 等长等待刻意不放在锁内。
	 */

	s->refcount--;
	if (s->refcount) {
		mutex_unlock(&slab_mutex);
		cpus_read_unlock();
		return;
	}
	/* refcount 归零后，本调用取得唯一最终销毁责任。 */

	/* free asan quarantined objects */
	/* 先释放 KASAN 隔离对象，让后端准确判断是否仍有真正活对象。 */
	kasan_cache_shutdown(s);

	err = __kmem_cache_shutdown(s);
	if (!slab_in_kunit_test())
		WARN(err, "%s %s: Slab cache still has objects when called from %pS",
		     __func__, s->name, (void *)_RET_IP_);

	list_del(&s->list);
	/* 这是全局不可见边界：出锁后新的遍历/合并不再取得该 cache。 */

	mutex_unlock(&slab_mutex);
	cpus_read_unlock();

	if (slab_state >= FULL)
		sysfs_slab_unlink(s);
	debugfs_slab_release(s);

	if (err)
		/*
		 * 后端报告仍有对象时保留描述符；泄漏比把现存对象变成 UAF 安全。
		 */
		return;

	if (s->flags & SLAB_TYPESAFE_BY_RCU)
		/*
		 * 等 slab 页相关宽限期结束后，才可释放 cache 元数据。
		 */
		rcu_barrier();

	kmem_cache_release(s);
}
EXPORT_SYMBOL(kmem_cache_destroy);

/**
 * kmem_cache_shrink - Shrink a cache.
 * @cachep: The cache to shrink.
 *
 * Releases as many slabs as possible for a cache.
 * To help debugging, a zero exit status indicates all slabs were released.
 *
 * Return: %0 if all slabs were released, non-zero otherwise
 */
/*
 * 尝试回收 @cachep 中尽可能多的空 slab。参数是调用期间保持存活的借用
 * cache；可睡眠。先让 KASAN 释放隔离对象，再交给后端收缩。返回 0 表示
 * 所有 slab 均已释放，非 0 表示仍有对象/页；cache 自身始终保持发布。
 */
int kmem_cache_shrink(struct kmem_cache *cachep)
{
	kasan_cache_shrink(cachep);

	return __kmem_cache_shrink(cachep);
}
EXPORT_SYMBOL(kmem_cache_shrink);

/*
 * slab_is_available() - 查询通用 slab 服务是否至少进入 UP 状态。
 * 入参：无。无锁只读、不睡眠；true 表示 kmalloc 尺寸 cache 已可用，
 * 但不等同于 sysfs/proc 等 FULL 阶段设施均已建立。无其他副作用。
 */
bool slab_is_available(void)
{
	return slab_state >= UP;
}

#ifdef CONFIG_PRINTK
/*
 * kmem_obj_info() - 统一提取 KFENCE 或普通 slab 对象来源信息。
 * @kpp: 调用者提供的输出结构；@object/@slab: 借用对象地址及所属 slab。
 * KFENCE 能识别时由其完整填充，否则转给后端；无返回值、不取得引用。
 */
static void kmem_obj_info(struct kmem_obj_info *kpp, void *object, struct slab *slab)
{
	/*
	 * KFENCE 对象使用专用 pool 和 metadata，不满足普通 slab 页内对象布局。必须
	 * 先让 __kfence_obj_info() 尝试识别；返回 true 表示 @kpp 已被 KFENCE 填充，
	 * 不能再落到 __kmem_obj_info() 用普通 slab 规则解释同一地址。
	 */
	if (__kfence_obj_info(kpp, object, slab))
		return;
	/*
	 * 非 KFENCE 对象继续普通 slab 诊断路径。这里的 @object/@slab 仍是借用输入，
	 * __kmem_obj_info() 只填充输出快照，不改变对象生命周期。
	 */
	__kmem_obj_info(kpp, object, slab);
}

/**
 * kmem_dump_obj - Print available slab provenance information
 * @object: slab object for which to find provenance information.
 *
 * This function uses pr_cont(), so that the caller is expected to have
 * printed out whatever preamble is appropriate.  The provenance information
 * depends on the type of object and on how much debugging is enabled.
 * For a slab-cache object, the fact that it is a slab object is printed,
 * and, if available, the slab name, return address, and stack trace from
 * the allocation and last free path of that object.
 *
 * Return: %true if the pointer is to a not-yet-freed object from
 * kmalloc() or kmem_cache_alloc(), either %true or %false if the pointer
 * is to an already-freed object, and %false otherwise.
 */
/*
 * 打印 @object 可获得的 slab 来源信息。@object 只是待诊断地址，函数不会
 * 取得对象所有权；调用者应先打印同行前缀，因为这里使用 pr_cont()。
 * 阶段为地址验证、解析 slab、采集快照、打印 cache/偏移/大小及分配释放栈。
 * true 表示识别为 slab/KFENCE 对象；对已释放对象结果依赖调试信息，
 * false 也可能仅代表无法证明。该诊断读取不提供并发生命周期保证。
 */
bool kmem_dump_obj(void *object)
{
	char *cp = IS_ENABLED(CONFIG_MMU) ? "" : "/vmalloc";
	int i;
	struct slab *slab;
	unsigned long ptroffset;
	struct kmem_obj_info kp = { };

	/* Some arches consider ZERO_SIZE_PTR to be a valid address. */
	/*
	 * 某些架构把 ZERO_SIZE_PTR 当形式上有效地址；显式排除低于一页的哨兵，
	 * 并要求直接映射地址有效，避免 virt_to_slab 解引用垃圾地址。
	 */
	if (object < (void *)PAGE_SIZE || !virt_addr_valid(object))
		return false;
	slab = virt_to_slab(object);
	if (!slab)
		return false;

	kmem_obj_info(&kp, object, slab);
	/*
	 * kp 是一次性快照：kp_objp 为对象槽起点，kp_data_offset 为有效数据
	 * 相对槽的偏移，两个栈数组分别描述最近分配与释放路径。
	 */
	if (kp.kp_slab_cache)
		pr_cont(" slab%s %s", cp, kp.kp_slab_cache->name);
	else
		pr_cont(" slab%s", cp);
	if (is_kfence_address(object))
		pr_cont(" (kfence)");
	if (kp.kp_objp)
		pr_cont(" start %px", kp.kp_objp);
	if (kp.kp_data_offset)
		pr_cont(" data offset %lu", kp.kp_data_offset);
	if (kp.kp_objp) {
		ptroffset = ((char *)object - (char *)kp.kp_objp) - kp.kp_data_offset;
		pr_cont(" pointer offset %lu", ptroffset);
	}
	if (kp.kp_slab_cache && kp.kp_slab_cache->object_size)
		pr_cont(" size %u", kp.kp_slab_cache->object_size);
	if (kp.kp_ret)
		pr_cont(" allocated at %pS\n", kp.kp_ret);
	else
		pr_cont("\n");
	for (i = 0; i < ARRAY_SIZE(kp.kp_stack); i++) {
		if (!kp.kp_stack[i])
			break;
		pr_info("    %pS\n", kp.kp_stack[i]);
	}

	if (kp.kp_free_stack[0])
		pr_cont(" Free path:\n");

	for (i = 0; i < ARRAY_SIZE(kp.kp_free_stack); i++) {
		if (!kp.kp_free_stack[i])
			break;
		pr_info("    %pS\n", kp.kp_free_stack[i]);
	}

	return true;
}
EXPORT_SYMBOL_GPL(kmem_dump_obj);
#endif

/* Create a cache during boot when no slab services are available yet */
/*
 * 在 slab 服务尚不能自举分配描述符时，就地初始化调用者提供的 @s。
 * @name 借用静态名称；@size/@useroffset/@usersize 均为字节；@flags 为
 * 引导 cache 策略。__init 单线程、允许 panic。成功无直接返回值，把
 * @s 初始化为暂不可合并的 cache；失败无法恢复而 panic。
 */
void __init create_boot_cache(struct kmem_cache *s, const char *name,
		unsigned int size, slab_flags_t flags,
		unsigned int useroffset, unsigned int usersize)
{
	int err;
	unsigned int align = ARCH_KMALLOC_MINALIGN;
	struct kmem_cache_args kmem_args = {};

	/*
	 * kmalloc caches guarantee alignment of at least the largest
	 * power-of-two divisor of the size. For power-of-two sizes,
	 * it is the size itself.
	 */
	/*
	 * kmalloc cache 至少按 size 的最大 2 次幂因子对齐；若 size 本身为
	 * 2 次幂，对齐就是 size，满足常见自然对齐而不额外制造尺寸类。
	 */
	if (flags & SLAB_KMALLOC)
		align = max(align, 1U << (ffs(size) - 1));
	kmem_args.align = calculate_alignment(flags, align, size);

#ifdef CONFIG_HARDENED_USERCOPY
	kmem_args.useroffset = useroffset;
	kmem_args.usersize = usersize;
#endif

	err = do_kmem_cache_create(s, name, size, &kmem_args, flags);

	if (err)
		panic("Creation of kmalloc slab %s size=%u failed. Reason %d\n",
					name, size, err);

	s->refcount = -1;	/* Exempt from merging for now */
	/* -1 是引导期“暂时禁止合并”哨兵，待全局表稳定后改成真实引用数。 */
}

/*
 * create_kmalloc_cache() - 在引导阶段建立一个通用 kmalloc 尺寸 cache。
 * @name 为持久静态名称；@size 为对象字节数；@flags 为类型策略。
 * GFP_NOWAIT 避免自举时进入依赖 kmalloc 的回收；失败无法继续启动而
 * panic。成功返回已加入 slab_caches、refcount=1 的持有指针。
 */
static struct kmem_cache *__init create_kmalloc_cache(const char *name,
						      unsigned int size,
						      slab_flags_t flags)
{
	struct kmem_cache *s = kmem_cache_zalloc(kmem_cache, GFP_NOWAIT);

	if (!s)
		panic("Out of memory when creating slab %s\n", name);

	create_boot_cache(s, name, size, flags | SLAB_KMALLOC, 0, size);
	list_add(&s->list, &slab_caches);
	s->refcount = 1;
	return s;
}

kmem_buckets kmalloc_caches[NR_KMALLOC_TYPES] __ro_after_init =
{ /* initialization for https://llvm.org/pr42570 */ };
/*
 * 二维表按分配域和尺寸索引定位 cache；元素在引导期写入，之后由 kmalloc
 * fast path 借用。显式空初始化用于规避所链接 LLVM 问题，未创建槽为 NULL。
 */
EXPORT_SYMBOL(kmalloc_caches);

#ifdef CONFIG_KMALLOC_PARTITION_RANDOM
unsigned long random_kmalloc_seed __ro_after_init;
/* 分区随机选择的引导种子；cache 建立后只读，供分配点选择隔离分区。 */
EXPORT_SYMBOL(random_kmalloc_seed);
#endif

/*
 * Conversion table for small slabs sizes / 8 to the index in the
 * kmalloc array. This is necessary for slabs < 192 since we have non power
 * of two cache sizes there. The size of larger slabs can be determined using
 * fls.
 */
/*
 * 小请求以 size/8 查表；192 字节以下包含 96、192 等非 2 次幂 cache，
 * 不能只用 fls。更大尺寸才可按最高位推导。表会按架构最小对齐在引导期
 * 修补，随后 __ro_after_init 固化。
 */
u8 kmalloc_size_index[24] __ro_after_init = {
	3,	/* 8 */
	4,	/* 16 */
	5,	/* 24 */
	5,	/* 32 */
	6,	/* 40 */
	6,	/* 48 */
	6,	/* 56 */
	6,	/* 64 */
	1,	/* 72 */
	1,	/* 80 */
	1,	/* 88 */
	1,	/* 96 */
	7,	/* 104 */
	7,	/* 112 */
	7,	/* 120 */
	7,	/* 128 */
	2,	/* 136 */
	2,	/* 144 */
	2,	/* 152 */
	2,	/* 160 */
	2,	/* 168 */
	2,	/* 176 */
	2,	/* 184 */
	2	/* 192 */
};

/*
 * kmalloc_size_roundup() - 返回 kmalloc(@size) 对应的实际容量粒度。
 * @size 为请求字节数。小请求读取 slab object_size；中等请求按页阶取整；
 * 0 和超过支持上限的请求原样返回。纯查询、不分配、不睡眠、无所有权变化。
 */
size_t kmalloc_size_roundup(size_t size)
{
	if (size && size <= KMALLOC_MAX_CACHE_SIZE) {
		/*
		 * The flags don't matter since size_index is common to all.
		 * Neither does the caller for just getting ->object_size.
		 */
		/*
		 * 这里只读统一 size_index，GFP 类型和调用点 token 不影响结果；
		 * 传普通值只是满足 kmalloc_slab 接口，并不会分配对象。
		 */
		return kmalloc_slab(size, NULL, GFP_KERNEL, __kmalloc_token(0))->object_size;
	}

	/* Above the smaller buckets, size is a multiple of page size. */
	/* 超过 slab 小尺寸 cache 后走页分配，容量为覆盖请求的最小 2 次幂页阶。 */
	if (size && size <= KMALLOC_MAX_SIZE)
		return PAGE_SIZE << get_order(size);

	/*
	 * Return 'size' for 0 - kmalloc() returns ZERO_SIZE_PTR
	 * and very large size - kmalloc() may fail.
	 */
	/*
	 * 0 对应 ZERO_SIZE_PTR；超大请求可能失败，无法承诺更大的可用容量，
	 * 因而两者均原样返回。
	 */
	return size;

}
EXPORT_SYMBOL(kmalloc_size_roundup);

#ifdef CONFIG_ZONE_DMA
#define KMALLOC_DMA_NAME(sz)	.name[KMALLOC_DMA] = "dma-kmalloc-" #sz,
#else
#define KMALLOC_DMA_NAME(sz)
#endif
/*
 * 这些条件宏只生成 kmalloc_info 的名称槽：配置关闭时展开为空，因此同一
 * 初始化表可覆盖 DMA、memcg、reclaim 和 partition 各构建组合。它们不在
 * 运行时分支，也不能在反斜杠续行中插入独立语句。
 */

#ifdef CONFIG_MEMCG
#define KMALLOC_CGROUP_NAME(sz)	.name[KMALLOC_CGROUP] = "kmalloc-cg-" #sz,
#else
#define KMALLOC_CGROUP_NAME(sz)
#endif

#ifndef CONFIG_SLUB_TINY
#define KMALLOC_RCL_NAME(sz)	.name[KMALLOC_RECLAIM] = "kmalloc-rcl-" #sz,
#else
#define KMALLOC_RCL_NAME(sz)
#endif

#ifdef CONFIG_KMALLOC_PARTITION_CACHES
#define __KMALLOC_PARTITION_CONCAT(a, b) a ## b
#define KMALLOC_PARTITION_NAME(N, sz) __KMALLOC_PARTITION_CONCAT(KMA_PART_, N)(sz)
#define KMA_PART_1(sz)                  .name[KMALLOC_PARTITION_START +  1] = "kmalloc-part-01-" #sz,
#define KMA_PART_2(sz)  KMA_PART_1(sz)  .name[KMALLOC_PARTITION_START +  2] = "kmalloc-part-02-" #sz,
#define KMA_PART_3(sz)  KMA_PART_2(sz)  .name[KMALLOC_PARTITION_START +  3] = "kmalloc-part-03-" #sz,
#define KMA_PART_4(sz)  KMA_PART_3(sz)  .name[KMALLOC_PARTITION_START +  4] = "kmalloc-part-04-" #sz,
#define KMA_PART_5(sz)  KMA_PART_4(sz)  .name[KMALLOC_PARTITION_START +  5] = "kmalloc-part-05-" #sz,
#define KMA_PART_6(sz)  KMA_PART_5(sz)  .name[KMALLOC_PARTITION_START +  6] = "kmalloc-part-06-" #sz,
#define KMA_PART_7(sz)  KMA_PART_6(sz)  .name[KMALLOC_PARTITION_START +  7] = "kmalloc-part-07-" #sz,
#define KMA_PART_8(sz)  KMA_PART_7(sz)  .name[KMALLOC_PARTITION_START +  8] = "kmalloc-part-08-" #sz,
#define KMA_PART_9(sz)  KMA_PART_8(sz)  .name[KMALLOC_PARTITION_START +  9] = "kmalloc-part-09-" #sz,
#define KMA_PART_10(sz) KMA_PART_9(sz)  .name[KMALLOC_PARTITION_START + 10] = "kmalloc-part-10-" #sz,
#define KMA_PART_11(sz) KMA_PART_10(sz) .name[KMALLOC_PARTITION_START + 11] = "kmalloc-part-11-" #sz,
#define KMA_PART_12(sz) KMA_PART_11(sz) .name[KMALLOC_PARTITION_START + 12] = "kmalloc-part-12-" #sz,
#define KMA_PART_13(sz) KMA_PART_12(sz) .name[KMALLOC_PARTITION_START + 13] = "kmalloc-part-13-" #sz,
#define KMA_PART_14(sz) KMA_PART_13(sz) .name[KMALLOC_PARTITION_START + 14] = "kmalloc-part-14-" #sz,
#define KMA_PART_15(sz) KMA_PART_14(sz) .name[KMALLOC_PARTITION_START + 15] = "kmalloc-part-15-" #sz,
#else // CONFIG_KMALLOC_PARTITION_CACHES
#define KMALLOC_PARTITION_NAME(N, sz)
#endif

#define INIT_KMALLOC_INFO(__size, __short_size)			\
{								\
	.name[KMALLOC_NORMAL]  = "kmalloc-" #__short_size,	\
	KMALLOC_RCL_NAME(__short_size)				\
	KMALLOC_CGROUP_NAME(__short_size)			\
	KMALLOC_DMA_NAME(__short_size)				\
	KMALLOC_PARTITION_NAME(KMALLOC_PARTITION_CACHES_NR, __short_size)	\
	.size = __size,						\
}
/*
 * INIT_KMALLOC_INFO 为一个尺寸同时生成各分配域的名称并记录真实字节数；
 * 分区宏递归展开出固定数量的隔离名称。名称是静态存储，不发生释放责任。
 */

/*
 * kmalloc_info[] is to make slab_debug=,kmalloc-xx option work at boot time.
 * kmalloc_index() supports up to 2^21=2MB, so the final entry of the table is
 * kmalloc-2M.
 */
/*
 * kmalloc_info[] 让 slab_debug=,kmalloc-xx 在 cache 创建前就能匹配名称。
 * 索引布局必须与 kmalloc_index() 一致；其最大 2^21 字节决定末项为 2M。
 */
const struct kmalloc_info_struct kmalloc_info[] __initconst = {
	INIT_KMALLOC_INFO(0, 0),
	INIT_KMALLOC_INFO(96, 96),
	INIT_KMALLOC_INFO(192, 192),
	INIT_KMALLOC_INFO(8, 8),
	INIT_KMALLOC_INFO(16, 16),
	INIT_KMALLOC_INFO(32, 32),
	INIT_KMALLOC_INFO(64, 64),
	INIT_KMALLOC_INFO(128, 128),
	INIT_KMALLOC_INFO(256, 256),
	INIT_KMALLOC_INFO(512, 512),
	INIT_KMALLOC_INFO(1024, 1k),
	INIT_KMALLOC_INFO(2048, 2k),
	INIT_KMALLOC_INFO(4096, 4k),
	INIT_KMALLOC_INFO(8192, 8k),
	INIT_KMALLOC_INFO(16384, 16k),
	INIT_KMALLOC_INFO(32768, 32k),
	INIT_KMALLOC_INFO(65536, 64k),
	INIT_KMALLOC_INFO(131072, 128k),
	INIT_KMALLOC_INFO(262144, 256k),
	INIT_KMALLOC_INFO(524288, 512k),
	INIT_KMALLOC_INFO(1048576, 1M),
	INIT_KMALLOC_INFO(2097152, 2M)
};

/*
 * Patch up the size_index table if we have strange large alignment
 * requirements for the kmalloc array. This is only the case for
 * MIPS it seems. The standard arches will not generate any code here.
 *
 * Largest permitted alignment is 256 bytes due to the way we
 * handle the index determination for the smaller caches.
 *
 * Make sure that nothing crazy happens if someone starts tinkering
 * around with ARCH_KMALLOC_MINALIGN
 */
/*
 * setup_kmalloc_cache_index_table() - 按架构最小对齐修补小尺寸索引表。
 * 入参/返回：无；__init 单线程，只写 kmalloc_size_index。
 * BUILD_BUG_ON 把“最小对齐必须是 <=256 的 2 次幂”变成编译期不变量；
 * 普通架构循环界为空或写回原值，编译器通常可消除全部代码。
 */
void __init setup_kmalloc_cache_index_table(void)
{
	unsigned int i;

	BUILD_BUG_ON(KMALLOC_MIN_SIZE > 256 ||
		!is_power_of_2(KMALLOC_MIN_SIZE));

	for (i = 8; i < KMALLOC_MIN_SIZE; i += 8) {
		unsigned int elem = size_index_elem(i);

		if (elem >= ARRAY_SIZE(kmalloc_size_index))
			break;
		kmalloc_size_index[elem] = KMALLOC_SHIFT_LOW;
	}

	if (KMALLOC_MIN_SIZE >= 64) {
		/*
		 * The 96 byte sized cache is not used if the alignment
		 * is 64 byte.
		 */
		/* 64 字节对齐使 96 cache 无收益，72..96 请求统一落到 128。 */
		for (i = 64 + 8; i <= 96; i += 8)
			kmalloc_size_index[size_index_elem(i)] = 7;

	}

	if (KMALLOC_MIN_SIZE >= 128) {
		/*
		 * The 192 byte sized cache is not used if the alignment
		 * is 128 byte. Redirect kmalloc to use the 256 byte cache
		 * instead.
		 */
		/* 128 字节对齐同理淘汰 192 cache，把 136..192 重定向到 256。 */
		for (i = 128 + 8; i <= 192; i += 8)
			kmalloc_size_index[size_index_elem(i)] = 8;
	}
}

/*
 * __kmalloc_minalign() - 计算通用 kmalloc cache 的 DMA 安全最小对齐。
 * 入参：无；返回字节数。通常取 DMA cache 对齐与架构 slab 对齐的最大值；
 * 使用 SWIOTLB 处理非对齐 kmalloc 时可回到 ARCH_KMALLOC_MINALIGN，避免
 * 为 bounce 已解决的问题扩大所有对象。只读全局状态，不睡眠。
 */
static unsigned int __kmalloc_minalign(void)
{
	unsigned int minalign = dma_get_cache_alignment();

	if (IS_ENABLED(CONFIG_DMA_BOUNCE_UNALIGNED_KMALLOC) &&
	    is_swiotlb_allocated())
		minalign = ARCH_KMALLOC_MINALIGN;

	return max(minalign, arch_slab_minalign());
}

/*
 * new_kmalloc_cache() - 建立一个“分配域 × 尺寸索引”槽。
 * @idx 是 kmalloc_info 索引；@type 指 normal/reclaim/memcg/DMA/partition。
 * __init 单线程调用。可能创建新 cache，也可能因 memcg 关闭或对齐提升而
 * 把槽别名到已有 cache；无直接返回值，失败由 create_kmalloc_cache panic。
 */
static void __init
new_kmalloc_cache(int idx, enum kmalloc_cache_type type)
{
	slab_flags_t flags = 0;
	unsigned int minalign = __kmalloc_minalign();
	unsigned int aligned_size = kmalloc_info[idx].size;
	int aligned_idx = idx;

	if ((KMALLOC_RECLAIM != KMALLOC_NORMAL) && (type == KMALLOC_RECLAIM)) {
		flags |= SLAB_RECLAIM_ACCOUNT;
	} else if (IS_ENABLED(CONFIG_MEMCG) && (type == KMALLOC_CGROUP)) {
		if (mem_cgroup_kmem_disabled()) {
			kmalloc_caches[type][idx] = kmalloc_caches[KMALLOC_NORMAL][idx];
			return;
		}
		flags |= SLAB_ACCOUNT;
	} else if (IS_ENABLED(CONFIG_ZONE_DMA) && (type == KMALLOC_DMA)) {
		flags |= SLAB_CACHE_DMA;
	}
	/*
	 * 先把 type 转成后端语义标志。memcg 运行时禁用时复用 normal cache；
	 * reclaim、memcg 和 DMA 必须保持各自记账/内存域契约。
	 */

#ifdef CONFIG_KMALLOC_PARTITION_CACHES
	if (type >= KMALLOC_PARTITION_START && type <= KMALLOC_PARTITION_END)
		flags |= SLAB_NO_MERGE;
#endif

	/*
	 * If CONFIG_MEMCG is enabled, disable cache merging for
	 * KMALLOC_NORMAL caches.
	 */
	/*
	 * memcg 构建下 normal cache 禁止合并，避免普通 cache 别名破坏后续
	 * memcg 派生/记账关系。
	 */
	if (IS_ENABLED(CONFIG_MEMCG) && (type == KMALLOC_NORMAL))
		flags |= SLAB_NO_MERGE;

	if (minalign > ARCH_KMALLOC_MINALIGN) {
		aligned_size = ALIGN(aligned_size, minalign);
		aligned_idx = __kmalloc_index(aligned_size, false);
	}
	/*
	 * DMA 要求更大时，把有效尺寸向上对齐并重新求索引；原 idx 槽随后只
	 * 保存别名，保证返回对象满足对齐且不重复创建等价 cache。
	 */

	if (!kmalloc_caches[type][aligned_idx])
		kmalloc_caches[type][aligned_idx] = create_kmalloc_cache(
					kmalloc_info[aligned_idx].name[type],
					aligned_size, flags);
	if (idx != aligned_idx)
		kmalloc_caches[type][idx] = kmalloc_caches[type][aligned_idx];
}

/*
 * Create the kmalloc array. Some of the regular kmalloc arrays
 * may already have been created because they were needed to
 * enable allocations for slab creation.
 */
/*
 * create_kmalloc_caches() - 完成全部通用 kmalloc 尺寸/分配域 cache。
 * 入参/返回：无；引导期单线程，可通过下层 panic 失败。
 * 先创建 96/192 特殊类和 2 次幂类，再发布随机种子，最后以 slab_state=UP
 * 宣告数组可供普通分配使用；该状态写入必须晚于所有表项初始化。
 */
void __init create_kmalloc_caches(void)
{
	int i;
	enum kmalloc_cache_type type;

	/*
	 * Including KMALLOC_CGROUP if CONFIG_MEMCG defined
	 */
	/* NR_KMALLOC_TYPES 已按配置包含可用域，逐域建立相同的尺寸覆盖。 */
	for (type = KMALLOC_NORMAL; type < NR_KMALLOC_TYPES; type++) {
		/* Caches that are NOT of the two-to-the-power-of size. */
		/* 96/192 cache 只在最小对齐允许时创建，用来降低内部碎片。 */
		if (KMALLOC_MIN_SIZE <= 32)
			new_kmalloc_cache(1, type);
		if (KMALLOC_MIN_SIZE <= 64)
			new_kmalloc_cache(2, type);

		/* Caches that are of the two-to-the-power-of size. */
		/* 主尺寸类从 KMALLOC_SHIFT_LOW 连续覆盖到最大 slab cache。 */
		for (i = KMALLOC_SHIFT_LOW; i <= KMALLOC_SHIFT_HIGH; i++)
			new_kmalloc_cache(i, type);
	}
#ifdef CONFIG_KMALLOC_PARTITION_RANDOM
	random_kmalloc_seed = get_random_u64();
#endif

	/* Kmalloc array is now usable */
	/*
	 * 发布边界：UP 之前任何普通 kmalloc 都不能假定二维表完整；写入之后
	 * 读者只消费已构造指针。引导顺序提供可见性，不依赖运行时锁。
	 */
	slab_state = UP;

	if (IS_ENABLED(CONFIG_SLAB_BUCKETS))
		kmem_buckets_cache = kmem_cache_create("kmalloc_buckets",
						       sizeof(kmem_buckets),
						       0, SLAB_NO_MERGE, NULL);
}

/*
 * kmalloc_fix_flags() - 对传给 slab 的非法 GFP 位做诊断性修复。
 * @flags: 原始分配标志值。返回清除 GFP_SLAB_BUG_MASK 后的可用标志；
 * 同时告警并打印调用栈，提醒调用者修正。可在分配路径调用，不取得资源。
 */
gfp_t kmalloc_fix_flags(gfp_t flags)
{
	gfp_t invalid_mask = flags & GFP_SLAB_BUG_MASK;

	flags &= ~GFP_SLAB_BUG_MASK;
	pr_warn("Unexpected gfp: %#x (%pGg). Fixing up to gfp: %#x (%pGg). Fix your code!\n",
			invalid_mask, &invalid_mask, flags, &flags);
	dump_stack();

	return flags;
}

#ifdef CONFIG_SLAB_FREELIST_RANDOM
/* Randomize a generic freelist */
/*
 * 随机化通用 freelist 索引。@list 是至少 @count 项的调用者所有输出数组；
 * 函数先写 0..count-1，再原地 Fisher-Yates 洗牌，保证每个索引恰好一次。
 * 调用者需保证 count>0；无分配、无锁、无返回值。
 */
static void freelist_randomize(unsigned int *list,
			       unsigned int count)
{
	unsigned int rand;
	unsigned int i;

	for (i = 0; i < count; i++)
		list[i] = i;

	/* Fisher-Yates shuffle */
	/* Fisher-Yates 从尾到头等概率选择交换位置，避免简单随机交换的偏差。 */
	for (i = count - 1; i > 0; i--) {
		rand = get_random_u32_below(i + 1);
		swap(list[i], list[rand]);
	}
}

/* Create a random sequence per cache */
/*
 * 为 @cachep 创建包含 @count 个对象索引的随机 freelist 序列；@gfp 决定
 * 分配上下文。count<2 或已有序列视为成功；分配失败返回 -ENOMEM，成功
 * 返回 0 并把数组 ownership 交给 cache，供后端建立 slab 时读取。
 */
int cache_random_seq_create(struct kmem_cache *cachep, unsigned int count,
				    gfp_t gfp)
{

	if (count < 2 || cachep->random_seq)
		return 0;

	cachep->random_seq = kcalloc(count, sizeof(unsigned int), gfp);
	if (!cachep->random_seq)
		return -ENOMEM;

	freelist_randomize(cachep->random_seq, count);
	return 0;
}

/* Destroy the per-cache random freelist sequence */
/*
 * 销毁 @cachep 持有的随机序列。参数在调用期间保持存活；kfree(NULL) 安全，
 * 随后清 NULL 防止重复释放/旧指针复用。无直接返回值。
 */
void cache_random_seq_destroy(struct kmem_cache *cachep)
{
	kfree(cachep->random_seq);
	cachep->random_seq = NULL;
}
#endif /* CONFIG_SLAB_FREELIST_RANDOM */

#ifdef CONFIG_SLUB_DEBUG
#define SLABINFO_RIGHTS (0400)

/*
 * print_slabinfo_header() - 输出 /proc/slabinfo 版本和列名。
 * @m: seq_file 输出上下文，借用且由 VFS 持有。无返回值；只追加文本，
 * 不改变 cache 状态。调用时 slab_mutex 已由迭代器 start 持有。
 */
static void print_slabinfo_header(struct seq_file *m)
{
	/*
	 * Output format version, so at least we can change it
	 * without _too_ many complaints.
	 */
	/* 先输出格式版本，使用户态在列布局演进时能选择相应解析规则。 */
	seq_puts(m, "slabinfo - version: 2.1\n");
	seq_puts(m, "# name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>");
	seq_puts(m, " : tunables <limit> <batchcount> <sharedfactor>");
	seq_puts(m, " : slabdata <active_slabs> <num_slabs> <sharedavail>");
	seq_putc(m, '\n');
}

/*
 * slab_start()/slab_next()/slab_stop() 构成 seq_file 迭代协议。
 * start 在整个一次遍历期间持有 slab_mutex，稳定 slab_caches 和 cache
 * 名称；next 推进借用链表位置；stop 在正常、EOF 或错误退出时统一解锁。
 * @pos 是逻辑记录号输入输出；返回链表位置或 NULL，均不取得 cache 引用。
 */
static void *slab_start(struct seq_file *m, loff_t *pos)
{
	mutex_lock(&slab_mutex);
	return seq_list_start(&slab_caches, *pos);
}

static void *slab_next(struct seq_file *m, void *p, loff_t *pos)
{
	return seq_list_next(p, &slab_caches, pos);
}

static void slab_stop(struct seq_file *m, void *p)
{
	mutex_unlock(&slab_mutex);
}

/*
 * cache_show() - 把单个 @s 的统计快照格式化到 @m。
 * 两个参数均为借用；调用者持 slab_mutex，get_slabinfo 读取后端统计并填充
 * 栈上 sinfo。无返回值，只产生文本副作用，不改变 cache。
 */
static void cache_show(struct kmem_cache *s, struct seq_file *m)
{
	struct slabinfo sinfo;

	memset(&sinfo, 0, sizeof(sinfo));
	get_slabinfo(s, &sinfo);

	seq_printf(m, "%-17s %6lu %6lu %6u %4u %4d",
		   s->name, sinfo.active_objs, sinfo.num_objs, s->size,
		   sinfo.objects_per_slab, (1 << sinfo.cache_order));

	seq_printf(m, " : tunables %4u %4u %4u",
		   sinfo.limit, sinfo.batchcount, sinfo.shared);
	seq_printf(m, " : slabdata %6lu %6lu %6lu",
		   sinfo.active_slabs, sinfo.num_slabs, sinfo.shared_avail);
	seq_putc(m, '\n');
}

/*
 * slab_show() - seq_file 的单记录回调。
 * @p 是嵌入 kmem_cache 的 list 节点，受 slab_mutex 稳定；首项前先打印
 * 表头，再打印本 cache。返回 0 表示继续遍历，无 ownership 变化。
 */
static int slab_show(struct seq_file *m, void *p)
{
	struct kmem_cache *s = list_entry(p, struct kmem_cache, list);

	if (p == slab_caches.next)
		print_slabinfo_header(m);
	cache_show(s, m);
	return 0;
}

/*
 * dump_unreclaimable_slab() - OOM 诊断时打印不可回收 slab 占用。
 * 入参/返回：无。OOM 路径不愿睡眠等待 slab_mutex，故 trylock 失败就放弃
 * 输出；取得锁后稳定遍历并跳过 SLAB_RECLAIM_ACCOUNT cache。副作用仅日志。
 */
void dump_unreclaimable_slab(void)
{
	struct kmem_cache *s;
	struct slabinfo sinfo;

	/*
	 * Here acquiring slab_mutex is risky since we don't prefer to get
	 * sleep in oom path. But, without mutex hold, it may introduce a
	 * risk of crash.
	 * Use mutex_trylock to protect the list traverse, dump nothing
	 * without acquiring the mutex.
	 */
	/*
	 * 在 OOM 路径阻塞等待 mutex 有死锁/长睡眠风险；但无锁遍历又可能与
	 * 销毁竞争而崩溃。因此使用 trylock：宁可缺少诊断，也不牺牲正确性。
	 */
	if (!mutex_trylock(&slab_mutex)) {
		pr_warn("excessive unreclaimable slab but cannot dump stats\n");
		return;
	}

	pr_info("Unreclaimable slab info:\n");
	pr_info("Name                      Used          Total\n");

	list_for_each_entry(s, &slab_caches, list) {
		if (s->flags & SLAB_RECLAIM_ACCOUNT)
			continue;

		get_slabinfo(s, &sinfo);

		if (sinfo.num_objs > 0)
			pr_info("%-17s %10luKB %10luKB\n", s->name,
				(sinfo.active_objs * s->size) / 1024,
				(sinfo.num_objs * s->size) / 1024);
	}
	mutex_unlock(&slab_mutex);
}

/*
 * slabinfo_op - iterator that generates /proc/slabinfo
 *
 * Output layout:
 * cache-name
 * num-active-objs
 * total-objs
 * object size
 * num-active-slabs
 * total-slabs
 * num-pages-per-slab
 * + further values on SMP and with statistics enabled
 */
/*
 * slabinfo_op 把 start/next/stop/show 绑定为 /proc/slabinfo 的迭代器。
 * 输出依次含 cache 名、活跃/总对象数、对象尺寸、slab 数和每 slab 页数，
 * SMP/统计配置还追加字段；静态表在模块生命周期内永久有效。
 */
static const struct seq_operations slabinfo_op = {
	.start = slab_start,
	.next = slab_next,
	.stop = slab_stop,
	.show = slab_show,
};

/*
 * slabinfo_open() - 为一次打开建立 seq_file 状态。
 * @inode 未使用；@file 是 VFS 持有的输入输出对象。成功返回 0，失败返回
 * seq_open errno；成功后的释放由 proc_ops.proc_release=seq_release 配对。
 */
static int slabinfo_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &slabinfo_op);
}

static const struct proc_ops slabinfo_proc_ops = {
	.proc_flags	= PROC_ENTRY_PERMANENT,
	.proc_open	= slabinfo_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= seq_release,
};
/*
 * proc 操作表把打开、读取、定位、释放委派给 seq_file；PERMANENT 表示该
 * 内建 proc 项不走模块动态移除生命周期，函数指针在运行期只读借用。
 */

/*
 * slab_proc_init() - 在启用 SLUB_DEBUG 时注册 /proc/slabinfo。
 * 入参：无；__init 可睡眠。proc_create 结果未保存，内建永久项无需卸载；
 * 返回 0 使启动继续，创建失败仅表现为文件缺失。
 */
static int __init slab_proc_init(void)
{
	proc_create("slabinfo", SLABINFO_RIGHTS, NULL, &slabinfo_proc_ops);
	return 0;
}
module_init(slab_proc_init);

#endif /* CONFIG_SLUB_DEBUG */

/**
 * kfree_sensitive - Clear sensitive information in memory before freeing
 * @p: object to free memory of
 *
 * The memory of the object @p points to is zeroed before freed.
 * If @p is %NULL, kfree_sensitive() does nothing.
 *
 * Note: this function zeroes the whole allocated buffer which can be a good
 * deal bigger than the requested buffer size passed to kmalloc(). So be
 * careful when using this function in performance sensitive code.
 */
/*
 * 释放前显式清除 @p 的全部实际分配容量。@p 可为 NULL，调用者转移对象
 * 最终所有权；函数用 ksize 得到的可能大于原请求，先取消 KASAN poison，
 * 再用不会被编译器消除的 memzero_explicit 清零，最后 kfree。
 * 无返回值，可按 kfree 允许的上下文调用；大尺寸清零会增加敏感路径延迟。
 */
void kfree_sensitive(const void *p)
{
	size_t ks;
	void *mem = (void *)p;

	ks = ksize(mem);
	if (ks) {
		/*
		 * 清零范围是分配器实际容量而非请求长度，消除尾部残留；先 unpoison
		 * 使 KASAN 接受整段写入。ks=0 时直接依赖 kfree(NULL/哨兵) 语义。
		 */
		kasan_unpoison_range(mem, ks);
		memzero_explicit(mem, ks);
	}
	kfree(mem);
}
EXPORT_SYMBOL(kfree_sensitive);

#ifdef CONFIG_BPF_SYSCALL
#include <linux/btf.h>

__bpf_kfunc_start_defs();

/*
 * bpf_get_kmem_cache() - 供受验证 BPF 程序从内核地址查询所属 cache。
 * @addr 是未经信任的整数地址；先要求直接映射有效，再借助 virt_to_slab。
 * 返回借用的 kmem_cache 或 NULL，不增加引用；调用方不能据此延长 cache
 * 生命周期或修改元数据。函数不睡眠、无副作用。
 */
__bpf_kfunc struct kmem_cache *bpf_get_kmem_cache(u64 addr)
{
	struct slab *slab;

	if (!virt_addr_valid((void *)(long)addr))
		return NULL;

	slab = virt_to_slab((void *)(long)addr);
	return slab ? slab->slab_cache : NULL;
}

__bpf_kfunc_end_defs();
#endif /* CONFIG_BPF_SYSCALL */

/* Tracepoints definitions. */
/*
 * 导出 kmalloc/cache alloc/free tracepoint 符号，使模块可注册观察者。
 * 这只是接口发布，不触发事件，也不改变对象 ownership。
 */
EXPORT_TRACEPOINT_SYMBOL(kmalloc);
EXPORT_TRACEPOINT_SYMBOL(kmem_cache_alloc);
EXPORT_TRACEPOINT_SYMBOL(kfree);
EXPORT_TRACEPOINT_SYMBOL(kmem_cache_free);

#ifndef CONFIG_KVFREE_RCU_BATCHED

/*
 * 非批处理实现：@ptr 在 RCU 宽限期后释放。
 * @head 非 NULL 时嵌入待释放对象，函数把释放责任交给 call_rcu 回调并立即
 * 返回；@head 为 NULL 是单参数形式，只能在可睡眠上下文同步等待 GP 后
 * kvfree。无返回值；返回后调用者均不得再访问 @ptr。
 */
void kvfree_call_rcu(struct rcu_head *head, void *ptr)
{
	if (head) {
		kasan_record_aux_stack(ptr);
		call_rcu(head, kvfree_rcu_cb);
		return;
	}

	// kvfree_rcu(one_arg) call.
	/*
	 * 单参数形式没有 rcu_head 可挂异步回调，只能确认当前可睡眠，同步等待
	 * 所有旧读者退出再释放；延迟较高但不需要额外元数据。
	 */
	might_sleep();
	synchronize_rcu();
	kvfree(ptr);
}
EXPORT_SYMBOL_GPL(kvfree_call_rcu);

/*
 * 非批处理配置无需建立每 CPU 队列、workqueue 或 shrinker；空实现保持
 * 公共初始化调用点一致。入参/返回：无，无副作用。
 */
void __init kvfree_rcu_init(void)
{
}

#else /* CONFIG_KVFREE_RCU_BATCHED */

/*
 * This rcu parameter is runtime-read-only. It reflects
 * a minimum allowed number of objects which can be cached
 * per-CPU. Object size is equal to one page. This value
 * can be changed at boot time.
 */
/*
 * 运行时只读参数：每 CPU 最少允许缓存多少个一页大小的 bulk 节点。
 * 启动时可调；缓存降低热路径页分配失败率，代价是少量常驻页面。
 */
static int rcu_min_cached_objs = 5;
module_param(rcu_min_cached_objs, int, 0444);

// A page shrinker can ask for pages to be freed to make them
// available for other parts of the system. This usually happens
// under low memory conditions, and in that case we should also
// defer page-cache filling for a short time period.
/*
 * shrinker 通常在内存压力下要求释放页面，因此短期推迟重新填充，避免
 * “刚排空又分配”与回收器对抗；这不延迟已经安全的对象回收。
 */
//
// The default value is 5 seconds, which is long enough to reduce
// interference with the shrinker while it asks other systems to
// drain their caches.
/*
 * 默认 5 秒给 shrinker 足够时间驱动其他子系统排空缓存；仅控制节点页
 * 补充退避，不是 RCU 宽限期长度。
 */
static int rcu_delay_page_cache_fill_msec = 5000;
module_param(rcu_delay_page_cache_fill_msec, int, 0444);

static struct workqueue_struct *rcu_reclaim_wq;
/*
 * 专用 unbound、mem-reclaim workqueue 承载 GP 后的 kfree/vfree；
 * WQ_MEM_RECLAIM 为内存压力场景保留前进能力。
 */

/* Maximum number of jiffies to wait before draining a batch. */
/* 批次最长等待 5 秒；积满一页记录时会把监控延迟缩短到 1 jiffy。 */
#define KFREE_DRAIN_JIFFIES (5 * HZ)
#define KFREE_N_BATCHES 2
#define FREE_N_CHANNELS 2
/*
 * 每 CPU 有两个可轮换 RCU work batch；两个 bulk 通道分别处理 slab 和
 * vmalloc 指针，第三条嵌入 rcu_head 的退化链由独立字段表示。
 */

/**
 * struct kvfree_rcu_bulk_data - single block to store kvfree_rcu() pointers
 * @list: List node. All blocks are linked between each other
 * @gp_snap: Snapshot of RCU state for objects placed to this bulk
 * @nr_records: Number of active pointers in the array
 * @records: Array of the kvfree_rcu() pointers
 */
/*
 * 单页 bulk 节点：@list 串接同一通道；@gp_snap 记录最近加入对象时的 RCU
 * 状态；@nr_records 是柔性数组有效项数；@records 保存待释放裸指针。
 * 节点来自每 CPU 页缓存或页分配，批次完成后缓存复用或 free_page。
 * krcp->lock 保护热队列中的链入、计数和追加；交给 worker 后由其独占。
 */
struct kvfree_rcu_bulk_data {
	struct list_head list;
	struct rcu_gp_oldstate gp_snap;
	unsigned long nr_records;
	void *records[] __counted_by(nr_records);
};

/*
 * This macro defines how many entries the "records" array
 * will contain. It is based on the fact that the size of
 * kvfree_rcu_bulk_data structure becomes exactly one page.
 */
/*
 * 容量用一页减固定头部再除指针大小，使节点恰占一页：分配/缓存简单，
 * 同时限制一次 kfree_bulk 的最大记录数和常驻内存。
 */
#define KVFREE_BULK_MAX_ENTR \
	((PAGE_SIZE - sizeof(struct kvfree_rcu_bulk_data)) / sizeof(void *))

/**
 * struct kfree_rcu_cpu_work - single batch of kfree_rcu() requests
 * @rcu_work: Let queue_rcu_work() invoke workqueue handler after grace period
 * @head_free: List of kfree_rcu() objects waiting for a grace period
 * @head_free_gp_snap: Grace-period snapshot to check for attempted premature frees.
 * @bulk_head_free: Bulk-List of kvfree_rcu() objects waiting for a grace period
 * @krcp: Pointer to @kfree_rcu_cpu structure
 */
/*
 * 一个已经交给 RCU work 的批次。@rcu_work 保证 handler 在 GP 后执行；
 * @head_free/@head_free_gp_snap 保存退化链及快照；@bulk_head_free[] 接管
 * 两个 bulk 通道；@krcp 借用所属 CPU 状态。只有所有通道为空时才可复用，
 * 字段的接管与清空受 krcp->lock 串行。
 */

struct kfree_rcu_cpu_work {
	struct rcu_work rcu_work;
	struct rcu_head *head_free;
	struct rcu_gp_oldstate head_free_gp_snap;
	struct list_head bulk_head_free[FREE_N_CHANNELS];
	struct kfree_rcu_cpu *krcp;
};

/**
 * struct kfree_rcu_cpu - batch up kfree_rcu() requests for RCU grace period
 * @head: List of kfree_rcu() objects not yet waiting for a grace period
 * @head_gp_snap: Snapshot of RCU state for objects placed to "@head"
 * @bulk_head: Bulk-List of kvfree_rcu() objects not yet waiting for a grace period
 * @krw_arr: Array of batches of kfree_rcu() objects waiting for a grace period
 * @lock: Synchronize access to this structure
 * @monitor_work: Promote @head to @head_free after KFREE_DRAIN_JIFFIES
 * @initialized: The @rcu_work fields have been initialized
 * @head_count: Number of objects in rcu_head singular list
 * @bulk_count: Number of objects in bulk-list
 * @bkvcache:
 *	A simple cache list that contains objects for reuse purpose.
 *	In order to save some per-cpu space the list is singular.
 *	Even though it is lockless an access has to be protected by the
 *	per-cpu lock.
 * @page_cache_work: A work to refill the cache when it is empty
 * @backoff_page_cache_fill: Delay cache refills
 * @work_in_progress: Indicates that page_cache_work is running
 * @hrtimer: A hrtimer for scheduling a page_cache_work
 * @nr_bkv_objs: number of allocated objects at @bkvcache.
 *
 * This is a per-CPU structure.  The reason that it is not included in
 * the rcu_data structure is to permit this code to be extracted from
 * the RCU files.  Such extraction could allow further optimization of
 * the interactions with the slab allocators.
 */
/*
 * 每 CPU 聚合器把调用热路径与 RCU/workqueue 慢路径解耦：
 *   head/head_gp_snap/head_count       嵌入 rcu_head 的退化链；
 *   bulk_head/bulk_count               slab/vmalloc 两个记录页通道；
 *   krw_arr                            已离开热队列、等待 GP/work 的批次；
 *   lock                               保护容器、转移和页缓存计数；
 *   monitor_work                       定时把热队列提升为 RCU work；
 *   page_cache_work/hrtimer/backoff    在可睡眠上下文补充节点页；
 *   bkvcache/nr_bkv_objs               可复用空节点页。
 *
 * per-CPU 不代表无需锁：workqueue、shrinker、barrier 和远端 CPU 都可能
 * 访问同一实例。结构按 possible CPU 永久存在，初始化后不迁移。
 */
struct kfree_rcu_cpu {
	// Objects queued on a linked list
	// through their rcu_head structures.
	/* 通过对象内嵌 rcu_head 串接的退化队列，head_count 记录对象数。 */
	struct rcu_head *head;
	unsigned long head_gp_snap;
	atomic_t head_count;

	// Objects queued on a bulk-list.
	/* 两条 bulk 链分别保存 slab 与 vmalloc 指针页，bulk_count 用于记账。 */
	struct list_head bulk_head[FREE_N_CHANNELS];
	atomic_t bulk_count[FREE_N_CHANNELS];

	struct kfree_rcu_cpu_work krw_arr[KFREE_N_BATCHES];
	raw_spinlock_t lock;
	struct delayed_work monitor_work;
	bool initialized;

	struct delayed_work page_cache_work;
	atomic_t backoff_page_cache_fill;
	atomic_t work_in_progress;
	struct hrtimer hrtimer;

	struct llist_head bkvcache;
	int nr_bkv_objs;
};

static DEFINE_PER_CPU(struct kfree_rcu_cpu, krc) = {
	.lock = __RAW_SPIN_LOCK_UNLOCKED(krc.lock),
};
/*
 * 每个 possible CPU 静态分配一个聚合器并初始化 raw spinlock；其余零值
 * 表示空队列、未初始化和无缓存，kvfree_rcu_init 再建立 work/list。
 */

/*
 * debug_rcu_bhead_unqueue() - 撤销 bulk 节点内所有对象的 debugobjects
 * 排队标记。@bhead 已由 worker 独占；无返回值，非调试构建为空操作。
 */
static __always_inline void
debug_rcu_bhead_unqueue(struct kvfree_rcu_bulk_data *bhead)
{
#ifdef CONFIG_DEBUG_OBJECTS_RCU_HEAD
	int i;

	for (i = 0; i < bhead->nr_records; i++)
		debug_rcu_head_unqueue((struct rcu_head *)(bhead->records[i]));
#endif
}

/*
 * krc_this_cpu_lock() - 禁止本地中断、定位当前 CPU 聚合器并加 raw lock。
 * @flags 是输出的旧中断状态，必须交给配对 unlock。返回借用 krcp。
 * 关中断既稳定 this_cpu_ptr，也避免本地中断重入同一锁；不可睡眠。
 */
static inline struct kfree_rcu_cpu *
krc_this_cpu_lock(unsigned long *flags)
{
	struct kfree_rcu_cpu *krcp;

	local_irq_save(*flags);	// For safely calling this_cpu_ptr().
	/* 保存并关闭中断，保证 safely calling this_cpu_ptr() 且不被本地重入。 */
	krcp = this_cpu_ptr(&krc);
	raw_spin_lock(&krcp->lock);

	return krcp;
}

/*
 * krc_this_cpu_unlock() - 解 raw lock 并恢复 @flags 中的中断状态。
 * 参数必须来自配对 lock；无返回值。之后不能再假定队列字段稳定。
 */
static inline void
krc_this_cpu_unlock(struct kfree_rcu_cpu *krcp, unsigned long flags)
{
	raw_spin_unlock_irqrestore(&krcp->lock, flags);
}

/*
 * get_cached_bnode() - 在持有 krcp->lock 时领取一个缓存节点页。
 * 返回节点 ownership 或 NULL；成功时计数和 llist 同步减少。
 */
static inline struct kvfree_rcu_bulk_data *
get_cached_bnode(struct kfree_rcu_cpu *krcp)
{
	if (!krcp->nr_bkv_objs)
		return NULL;

	WRITE_ONCE(krcp->nr_bkv_objs, krcp->nr_bkv_objs - 1);
	return (struct kvfree_rcu_bulk_data *)
		llist_del_first(&krcp->bkvcache);
}

/*
 * put_cached_bnode() - 尝试把空节点 ownership 交回每 CPU 缓存。
 * 调用者持 krcp->lock。达到上限返回 false，调用者仍须 free_page；成功
 * 返回 true，调用者不得再访问节点。
 */
static inline bool
put_cached_bnode(struct kfree_rcu_cpu *krcp,
	struct kvfree_rcu_bulk_data *bnode)
{
	// Check the limit.
	/* 上限控制每 CPU 常驻页；达到阈值即把多余页面交还伙伴系统。 */
	if (krcp->nr_bkv_objs >= rcu_min_cached_objs)
		return false;

	llist_add((struct llist_node *) bnode, &krcp->bkvcache);
	WRITE_ONCE(krcp->nr_bkv_objs, krcp->nr_bkv_objs + 1);
	return true;
}

/*
 * drain_page_cache() - 排空指定 CPU 的空 bulk 节点页。
 * @krcp 可来自任意 CPU；锁内原子摘下整条 llist 并清计数，锁外逐页释放，
 * 避免在 raw spinlock 内进入页分配器。返回释放页数，供 shrinker 记账。
 */
static int
drain_page_cache(struct kfree_rcu_cpu *krcp)
{
	unsigned long flags;
	struct llist_node *page_list, *pos, *n;
	int freed = 0;

	if (!rcu_min_cached_objs)
		return 0;

	raw_spin_lock_irqsave(&krcp->lock, flags);
	page_list = llist_del_all(&krcp->bkvcache);
	WRITE_ONCE(krcp->nr_bkv_objs, 0);
	raw_spin_unlock_irqrestore(&krcp->lock, flags);
	/* 摘链后本函数独占 page_list；并发归还者只会进入新的缓存链。 */

	llist_for_each_safe(pos, n, page_list) {
		free_page((unsigned long)pos);
		freed++;
	}

	return freed;
}

/*
 * kvfree_rcu_bulk() - 释放一个已离开热队列的 bulk 节点。
 * @krcp 用于归还节点缓存；@bnode 由调用者转移 ownership；@idx=0 表示
 * slab/kfree_bulk，@idx=1 表示 vmalloc/vfree 循环。
 * worker 上下文可调度。只有 gp_snap 已完成才释放记录；随后节点尝试缓存，
 * 超额则 free_page。无直接返回值，所有记录 ownership 在此终结。
 */
static void
kvfree_rcu_bulk(struct kfree_rcu_cpu *krcp,
	struct kvfree_rcu_bulk_data *bnode, int idx)
{
	unsigned long flags;
	int i;

	if (!WARN_ON_ONCE(!poll_state_synchronize_rcu_full(&bnode->gp_snap))) {
		/*
		 * full GP snapshot 覆盖节点中最近加入的对象；通过才证明所有更早
		 * RCU 读者离开。失败只 WARN 且跳过对象释放，宁可泄漏也不早释放。
		 */
		debug_rcu_bhead_unqueue(bnode);
		rcu_lock_acquire(&rcu_callback_map);
		if (idx == 0) { // kmalloc() / kfree().
			/* 通道 0：对象均来自 slab，批量释放减少逐对象锁/元数据成本。 */
			trace_rcu_invoke_kfree_bulk_callback(
				"slab", bnode->nr_records,
				bnode->records);

			kfree_bulk(bnode->nr_records, bnode->records);
		} else { // vmalloc() / vfree().
			/* 通道 1：vmalloc 映射需逐个 vfree，可能执行更重的拆映射工作。 */
			for (i = 0; i < bnode->nr_records; i++) {
				trace_rcu_invoke_kvfree_callback(
					"slab", bnode->records[i], 0);

				vfree(bnode->records[i]);
			}
		}
		rcu_lock_release(&rcu_callback_map);
	}

	raw_spin_lock_irqsave(&krcp->lock, flags);
	if (put_cached_bnode(krcp, bnode))
		bnode = NULL;
	raw_spin_unlock_irqrestore(&krcp->lock, flags);

	if (bnode)
		/* 缓存已满，当前函数仍拥有节点页，直接归还伙伴系统。 */
		free_page((unsigned long) bnode);

	cond_resched_tasks_rcu_qs();
}

/*
 * kvfree_rcu_list() - 释放 bulk 路径不可用时的 rcu_head 单链表。
 * @head ownership 由 worker/ready-drain 转入；每个 head->func 暂存原对象
 * 起点，head 与起点之差是嵌入偏移。逐个撤销调试标记、trace、kvfree，
 * 并报告 Tasks RCU 静止点，避免长链独占 CPU。无返回值。
 */
static void
kvfree_rcu_list(struct rcu_head *head)
{
	struct rcu_head *next;

	for (; head; head = next) {
		void *ptr = (void *) head->func;
		unsigned long offset = (void *) head - ptr;
		/*
		 * 入队时借用 func 字段保存对象起点；由此既能 kvfree(ptr)，也能在
		 * trace 中报告 rcu_head 偏移。对象在释放前不再作为普通回调执行。
		 */

		next = head->next;
		debug_rcu_head_unqueue((struct rcu_head *)ptr);
		rcu_lock_acquire(&rcu_callback_map);
		trace_rcu_invoke_kvfree_callback("slab", head, offset);

		kvfree(ptr);

		rcu_lock_release(&rcu_callback_map);
		cond_resched_tasks_rcu_qs();
	}
}

/*
 * This function is invoked in workqueue context after a grace period.
 * It frees all the objects queued on ->bulk_head_free or ->head_free.
 */
/*
 * RCU work 在宽限期之后调用本函数，释放 krwp 已接管的 bulk_head_free 和
 * head_free。@work 嵌入 rcu_work；container_of 恢复批次，再借用所属 krcp。
 * 锁内把三通道替换到栈上私有容器并清空 batch，锁外执行实际释放；因此
 * 同一 batch 可在 handler 释放期间重新被生产侧判断，但字段已为空且对象
 * ownership 已完全转到本函数。
 */
static void kfree_rcu_work(struct work_struct *work)
{
	unsigned long flags;
	struct kvfree_rcu_bulk_data *bnode, *n;
	struct list_head bulk_head[FREE_N_CHANNELS];
	struct rcu_head *head;
	struct kfree_rcu_cpu *krcp;
	struct kfree_rcu_cpu_work *krwp;
	struct rcu_gp_oldstate head_gp_snap;
	int i;

	krwp = container_of(to_rcu_work(work),
		struct kfree_rcu_cpu_work, rcu_work);
	krcp = krwp->krcp;

	raw_spin_lock_irqsave(&krcp->lock, flags);
	// Channels 1 and 2.
	/* 通道 1/2：原子转移两条 bulk 链，batch 槽立刻恢复为空链。 */
	for (i = 0; i < FREE_N_CHANNELS; i++)
		list_replace_init(&krwp->bulk_head_free[i], &bulk_head[i]);

	// Channel 3.
	/* 通道 3：取走退化链及其 GP 快照，并清 NULL，防止重复释放。 */
	head = krwp->head_free;
	krwp->head_free = NULL;
	head_gp_snap = krwp->head_free_gp_snap;
	raw_spin_unlock_irqrestore(&krcp->lock, flags);

	// Handle the first two channels.
	/* 从私有链逐节点回收；无需再持 krcp->lock。 */
	for (i = 0; i < FREE_N_CHANNELS; i++) {
		// Start from the tail page, so a GP is likely passed for it.
		/*
		 * 从较老的尾节点开始，更可能已跨过自身 snapshot；虽然 rcu_work
		 * 已等待批次 GP，逐节点检查仍防止 snapshot 关联错误。
		 */
		list_for_each_entry_safe(bnode, n, &bulk_head[i], list)
			kvfree_rcu_bulk(krcp, bnode, i);
	}

	/*
	 * This is used when the "bulk" path can not be used for the
	 * double-argument of kvfree_rcu().  This happens when the
	 * page-cache is empty, which means that objects are instead
	 * queued on a linked list through their rcu_head structures.
	 * This list is named "Channel 3".
	 */
	/*
	 * bulk 节点页耗尽时，双参数形式利用对象内嵌 rcu_head 排成第三通道。
	 * 只有对应 full snapshot 已完成才释放；不满足时 WARN 并保留安全性。
	 */
	if (head && !WARN_ON_ONCE(!poll_state_synchronize_rcu_full(&head_gp_snap)))
		kvfree_rcu_list(head);
}

/*
 * kfree_rcu_sheaf() - 尝试走 SLUB 的 cache-local RCU sheaf 快路径。
 * @obj 为待释放借用地址；成功返回 true，ownership 已交给 sheaf；false
 * 表示调用者继续公共批处理。vmalloc、无 slab 或远端 NUMA 节点不适用。
 * 不睡眠；仅在本地节点上使用以避免跨节点 cache 操作成本。
 */
static bool kfree_rcu_sheaf(void *obj)
{
	struct kmem_cache *s;
	struct slab *slab;

	if (is_vmalloc_addr(obj))
		return false;

	slab = virt_to_slab(obj);
	if (unlikely(!slab))
		return false;

	s = slab->slab_cache;
	if (likely(!IS_ENABLED(CONFIG_NUMA) || slab_nid(slab) == numa_mem_id()))
		return __kfree_rcu_sheaf(s, obj);

	return false;
}

/*
 * need_offload_krc() - 无锁近似查询热队列是否仍有待转移对象。
 * @krcp 借用；返回 true 表示任一 bulk 链非空或 head 非 NULL。
 * 精确转移仍须在 lock 下重新验证，READ_ONCE 仅防编译器重复/合并读取。
 */
static bool
need_offload_krc(struct kfree_rcu_cpu *krcp)
{
	int i;

	for (i = 0; i < FREE_N_CHANNELS; i++)
		if (!list_empty(&krcp->bulk_head[i]))
			return true;

	return !!READ_ONCE(krcp->head);
}

/*
 * need_wait_for_krwp_work() - 判断一个 batch 槽是否仍被旧 RCU work 占用。
 * 调用者持 krcp->lock，返回 true 表示任一通道非空，不能覆盖；只读借用。
 */
static bool
need_wait_for_krwp_work(struct kfree_rcu_cpu_work *krwp)
{
	int i;

	for (i = 0; i < FREE_N_CHANNELS; i++)
		if (!list_empty(&krwp->bulk_head_free[i]))
			return true;

	return !!krwp->head_free;
}

/*
 * krc_count() - 汇总一个 CPU 热队列中的待释放对象数。
 * 原子读取 head_count 和两个 bulk_count，返回近似瞬时总数；并发入队允许
 * 数值马上变化，适合调度阈值/shrinker 估算，不作为 ownership 证明。
 */
static int krc_count(struct kfree_rcu_cpu *krcp)
{
	int sum = atomic_read(&krcp->head_count);
	int i;

	for (i = 0; i < FREE_N_CHANNELS; i++)
		sum += atomic_read(&krcp->bulk_count[i]);

	return sum;
}

/*
 * __schedule_delayed_monitor_work() - 在持有 krcp->lock 时安排批次监控。
 * 队列达到一页容量则 1 jiffy 后处理，否则最多等 KFREE_DRAIN_JIFFIES；
 * 已有更晚定时器时只向前缩短，绝不推迟现有回收。无返回值。
 */
static void
__schedule_delayed_monitor_work(struct kfree_rcu_cpu *krcp)
{
	long delay, delay_left;

	delay = krc_count(krcp) >= KVFREE_BULK_MAX_ENTR ? 1:KFREE_DRAIN_JIFFIES;
	if (delayed_work_pending(&krcp->monitor_work)) {
		delay_left = krcp->monitor_work.timer.expires - jiffies;
		if (delay < delay_left)
			mod_delayed_work(rcu_reclaim_wq, &krcp->monitor_work, delay);
		return;
	}
	queue_delayed_work(rcu_reclaim_wq, &krcp->monitor_work, delay);
}

/*
 * schedule_delayed_monitor_work() - 可从未持锁上下文安排监控的包装。
 * @krcp 借用；短暂加 raw lock 调用内部版本。不可睡眠，无返回值。
 */
static void
schedule_delayed_monitor_work(struct kfree_rcu_cpu *krcp)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&krcp->lock, flags);
	__schedule_delayed_monitor_work(krcp);
	raw_spin_unlock_irqrestore(&krcp->lock, flags);
}

/*
 * kvfree_rcu_drain_ready() - 不再等待 work，直接摘取已越过 GP 的热队列项。
 * @krcp 可由 monitor/shrinker 调用。锁内从每条链尾扫描最老节点，遇到首个
 * 未完成 snapshot 即停止，并在 head 整体安全时一次摘下；锁外执行释放。
 * 无返回值。该快速排空与 queue_batch 并发，二者靠 krcp->lock 唯一领取。
 */
static void
kvfree_rcu_drain_ready(struct kfree_rcu_cpu *krcp)
{
	struct list_head bulk_ready[FREE_N_CHANNELS];
	struct kvfree_rcu_bulk_data *bnode, *n;
	struct rcu_head *head_ready = NULL;
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&krcp->lock, flags);
	for (i = 0; i < FREE_N_CHANNELS; i++) {
		INIT_LIST_HEAD(&bulk_ready[i]);

		list_for_each_entry_safe_reverse(bnode, n, &krcp->bulk_head[i], list) {
			if (!poll_state_synchronize_rcu_full(&bnode->gp_snap))
				break;
			/*
			 * 链尾最老，snapshot 随入队向链头更新；首个未完成后更年轻
			 * 节点也不能安全释放，故可立即停止扫描。
			 */

			atomic_sub(bnode->nr_records, &krcp->bulk_count[i]);
			list_move(&bnode->list, &bulk_ready[i]);
		}
	}

	if (krcp->head && poll_state_synchronize_rcu(krcp->head_gp_snap)) {
		head_ready = krcp->head;
		atomic_set(&krcp->head_count, 0);
		WRITE_ONCE(krcp->head, NULL);
	}
	raw_spin_unlock_irqrestore(&krcp->lock, flags);
	/* ready 容器现由本函数独占；所有可能调度/释放的操作都在锁外完成。 */

	for (i = 0; i < FREE_N_CHANNELS; i++) {
		list_for_each_entry_safe(bnode, n, &bulk_ready[i], list)
			kvfree_rcu_bulk(krcp, bnode, i);
	}

	if (head_ready)
		kvfree_rcu_list(head_ready);
}

/*
 * Return: %true if a work is queued, %false otherwise.
 */
/*
 * kvfree_rcu_queue_batch() - 把热队列转移到一个空闲 RCU work batch。
 * @krcp 借用；函数内部持 raw lock。成功返回 true 并由 rcu_work 接管所有
 * 转移对象；false 表示没有对象或两个 batch 均在途，调用者稍后重试/flush。
 * 转移时清零热计数和指针，保证对象只由热队列或某个 batch 唯一拥有。
 */
static bool
kvfree_rcu_queue_batch(struct kfree_rcu_cpu *krcp)
{
	unsigned long flags;
	bool queued = false;
	int i, j;

	raw_spin_lock_irqsave(&krcp->lock, flags);

	// Attempt to start a new batch.
	/* 依次寻找三个通道均为空的 batch，避免覆盖仍在 worker 使用的字段。 */
	for (i = 0; i < KFREE_N_BATCHES; i++) {
		struct kfree_rcu_cpu_work *krwp = &(krcp->krw_arr[i]);

		// Try to detach bulk_head or head and attach it, only when
		// all channels are free.  Any channel is not free means at krwp
		// there is on-going rcu work to handle krwp's free business.
		/*
		 * 任一通道非空说明旧 work 尚未消费完；必须跳过整个 batch，不能
		 * 只复用其中空通道，否则一次 rcu_work 无法表达两代 GP ownership。
		 */
		if (need_wait_for_krwp_work(krwp))
			continue;

		// kvfree_rcu_drain_ready() might handle this krcp, if so give up.
		/* 加锁后的复查与 direct-drain 竞争；若已被摘空就不再排 work。 */
		if (need_offload_krc(krcp)) {
			// Channel 1 corresponds to the SLAB-pointer bulk path.
			// Channel 2 corresponds to vmalloc-pointer bulk path.
			/* bulk 通道整体 replace_init，旧热链变空，batch 取得节点 ownership。 */
			for (j = 0; j < FREE_N_CHANNELS; j++) {
				if (list_empty(&krwp->bulk_head_free[j])) {
					atomic_set(&krcp->bulk_count[j], 0);
					list_replace_init(&krcp->bulk_head[j],
						&krwp->bulk_head_free[j]);
				}
			}

			// Channel 3 corresponds to both SLAB and vmalloc
			// objects queued on the linked list.
			/*
			 * 第三通道接管 head 后重新取得 full snapshot，覆盖链上所有对象；
			 * WRITE_ONCE(NULL) 是生产者可观察的“热链已转移”边界。
			 */
			if (!krwp->head_free) {
				krwp->head_free = krcp->head;
				get_state_synchronize_rcu_full(&krwp->head_free_gp_snap);
				atomic_set(&krcp->head_count, 0);
				WRITE_ONCE(krcp->head, NULL);
			}

			// One work is per one batch, so there are three
			// "free channels", the batch can handle. Break
			// the loop since it is done with this CPU thus
			// queuing an RCU work is _always_ success here.
			/*
			 * 每 batch 只有一个 rcu_work，统一承载三通道；对象已转移且 work
			 * 未在途，因此 queue 必须成功，失败意味着协议 bug。
			 */
			queued = queue_rcu_work(rcu_reclaim_wq, &krwp->rcu_work);
			WARN_ON_ONCE(!queued);
			break;
		}
	}

	raw_spin_unlock_irqrestore(&krcp->lock, flags);
	return queued;
}

/*
 * This function is invoked after the KFREE_DRAIN_JIFFIES timeout.
 */
/*
 * kfree_rcu_monitor() - 定时推进一个 CPU 的待释放队列。
 * @work 嵌入 monitor_work。先直接释放已安全项，再把剩余项交给 RCU work；
 * 若 batch 槽仍忙导致热队列未空，则重新武装定时器。无返回值，可睡眠的
 * workqueue 上下文，不持调用者锁。
 */
static void kfree_rcu_monitor(struct work_struct *work)
{
	struct kfree_rcu_cpu *krcp = container_of(work,
		struct kfree_rcu_cpu, monitor_work.work);

	// Drain ready for reclaim.
	/* 已跨 GP 的对象无需再排一轮 rcu_work，可直接回收以降低延迟。 */
	kvfree_rcu_drain_ready(krcp);

	// Queue a batch for a rest.
	/* 尚未安全的对象通过 queue_rcu_work 等待对应 GP。 */
	kvfree_rcu_queue_batch(krcp);

	// If there is nothing to detach, it means that our job is
	// successfully done here. In case of having at least one
	// of the channels that is still busy we should rearm the
	// work to repeat an attempt. Because previous batches are
	// still in progress.
	/*
	 * 仍非空通常表示两个 batch 都在途；重试保证最终前进，不把临时拥塞
	 * 误判为已完成。
	 */
	if (need_offload_krc(krcp))
		schedule_delayed_monitor_work(krcp);
}

/*
 * fill_page_cache_func() - 在可睡眠上下文补充当前 CPU 聚合器的空节点页。
 * @work 嵌入 page_cache_work。正常补到 rcu_min_cached_objs；shrinker 设置
 * backoff 时最多补 1 页。页分配禁止重试、动用储备和告警，失败就停止；
 * 加锁归还时若并发已补满则释放多余页。最后清 work/backoff 状态。
 */
static void fill_page_cache_func(struct work_struct *work)
{
	struct kvfree_rcu_bulk_data *bnode;
	struct kfree_rcu_cpu *krcp =
		container_of(work, struct kfree_rcu_cpu,
			page_cache_work.work);
	unsigned long flags;
	int nr_pages;
	bool pushed;
	int i;

	nr_pages = atomic_read(&krcp->backoff_page_cache_fill) ?
		1 : rcu_min_cached_objs;
	/* 回收退避期只保留一页以维持热路径前进，同时尊重内存压力。 */

	for (i = READ_ONCE(krcp->nr_bkv_objs); i < nr_pages; i++) {
		bnode = (struct kvfree_rcu_bulk_data *)
			__get_free_page(GFP_KERNEL | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN);

		if (!bnode)
			break;

		raw_spin_lock_irqsave(&krcp->lock, flags);
		pushed = put_cached_bnode(krcp, bnode);
		raw_spin_unlock_irqrestore(&krcp->lock, flags);

		if (!pushed) {
			free_page((unsigned long) bnode);
			break;
		}
	}

	atomic_set(&krcp->work_in_progress, 0);
	atomic_set(&krcp->backoff_page_cache_fill, 0);
	/* 先完成所有页面 ownership，再允许下一次 refill 被调度。 */
}

// Record ptr in a page managed by krcp, with the pre-krc_this_cpu_lock()
// state specified by flags.  If can_alloc is true, the caller must
// be schedulable and not be holding any locks or mutexes that might be
// acquired by the memory allocator or anything that it might invoke.
// Returns true if ptr was successfully recorded, else the caller must
// use a fallback.
/*
 * 在 krc_this_cpu_lock 前的 @flags 状态下，把 @ptr 记录进 @krcp 所属页。
 * @krcp/@flags 为输出；@can_alloc=true 只允许用于可调度且未持 allocator
 * 可能获取之锁的调用者。成功返回 true，函数保持 krcp->lock 和中断关闭，
 * @ptr ownership 已进入 bulk 队列；失败同样保持锁，调用者必须走 fallback
 * 并最终配对 unlock。若临时解锁分配页面，会重新加同一 CPU 实例的锁。
 */
static inline bool
add_ptr_to_bulk_krc_lock(struct kfree_rcu_cpu **krcp,
	unsigned long *flags, void *ptr, bool can_alloc)
{
	struct kvfree_rcu_bulk_data *bnode;
	int idx;

	*krcp = krc_this_cpu_lock(flags);
	if (unlikely(!(*krcp)->initialized))
		return false;

	idx = !!is_vmalloc_addr(ptr);
	bnode = list_first_entry_or_null(&(*krcp)->bulk_head[idx],
		struct kvfree_rcu_bulk_data, list);

	/* Check if a new block is required. */
	/* 链头是当前填充页；不存在或已满时先尝试每 CPU 空页缓存。 */
	if (!bnode || bnode->nr_records == KVFREE_BULK_MAX_ENTR) {
		bnode = get_cached_bnode(*krcp);
		if (!bnode && can_alloc) {
			krc_this_cpu_unlock(*krcp, *flags);

			// __GFP_NORETRY - allows a light-weight direct reclaim
			// what is OK from minimizing of fallback hitting point of
			// view. Apart of that it forbids any OOM invoking what is
			// also beneficial since we are about to release memory soon.
			/*
			 * __GFP_NORETRY 只允许轻量直接回收且禁止 OOM；本路径即将释放
			 * 内存并有 fallback，不应为记录元数据触发激进回收。
			 */
			//
			// __GFP_NOMEMALLOC - prevents from consuming of all the
			// memory reserves. Please note we have a fallback path.
			/* __GFP_NOMEMALLOC 不消耗紧急储备，失败交给第三通道/同步释放。 */
			//
			// __GFP_NOWARN - it is supposed that an allocation can
			// be failed under low memory or high memory pressure
			// scenarios.
			/* __GFP_NOWARN 接受低内存下正常失败，避免日志风暴。 */
			bnode = (struct kvfree_rcu_bulk_data *)
				__get_free_page(GFP_KERNEL | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN);
			raw_spin_lock_irqsave(&(*krcp)->lock, *flags);
		}

		if (!bnode)
			return false;

		// Initialize the new block and attach it.
		/* 新页尚无记录；链入通道头后 ownership 转给 krcp 热队列。 */
		bnode->nr_records = 0;
		list_add(&bnode->list, &(*krcp)->bulk_head[idx]);
	}

	// Finally insert and update the GP for this page.
	/*
	 * 先写记录，再刷新节点 GP snapshot 和计数；同一 raw lock 使 monitor
	 * 不会看到“计数已增加但指针/快照未初始化”的半状态。
	 */
	bnode->nr_records++;
	bnode->records[bnode->nr_records - 1] = ptr;
	get_state_synchronize_rcu_full(&bnode->gp_snap);
	atomic_inc(&(*krcp)->bulk_count[idx]);

	return true;
}

/*
 * schedule_page_work_fn() - hrtimer 回调，把页缓存补充转交高优先级 workqueue。
 * @t 嵌入 krcp->hrtimer；hrtimer 上下文不可睡眠/分配，因此只排 delayed_work。
 * 返回 HRTIMER_NORESTART 表示一次性定时器，work_in_progress 防止重复调度。
 */
static enum hrtimer_restart
schedule_page_work_fn(struct hrtimer *t)
{
	struct kfree_rcu_cpu *krcp =
		container_of(t, struct kfree_rcu_cpu, hrtimer);

	queue_delayed_work(system_highpri_wq, &krcp->page_cache_work, 0);
	return HRTIMER_NORESTART;
}

/*
 * run_page_cache_worker() - 在 bulk 页缺失时触发异步补充。
 * @krcp 借用；cache 禁用或 RCU 调度器未运行时不动作。atomic_xchg 唯一领取
 * worker 调度权：退避期排延迟 work，否则用 hrtimer 跨出当前可能不可睡眠
 * 上下文，再由高优先级 workqueue 分配页面。无返回值。
 */
static void
run_page_cache_worker(struct kfree_rcu_cpu *krcp)
{
	// If cache disabled, bail out.
	/* rcu_min_cached_objs=0 明确禁用节点页缓存，不应偷偷分配。 */
	if (!rcu_min_cached_objs)
		return;

	if (rcu_scheduler_active == RCU_SCHEDULER_RUNNING &&
			!atomic_xchg(&krcp->work_in_progress, 1)) {
		/* 0->1 的唯一成功者负责排 work，其他并发调用直接复用该承诺。 */
		if (atomic_read(&krcp->backoff_page_cache_fill)) {
			queue_delayed_work(rcu_reclaim_wq,
				&krcp->page_cache_work,
					msecs_to_jiffies(rcu_delay_page_cache_fill_msec));
		} else {
			hrtimer_setup(&krcp->hrtimer, schedule_page_work_fn, CLOCK_MONOTONIC,
				      HRTIMER_MODE_REL);
			hrtimer_start(&krcp->hrtimer, 0, HRTIMER_MODE_REL);
		}
	}
}

/*
 * kfree_rcu_scheduler_running() - RCU 调度器开始运行后补排早期积压。
 * 入参/返回：无；__init 遍历 possible CPU。早期 kvfree_rcu 可能已入队却
 * 不能安排 monitor，本函数为所有非空聚合器启动延迟工作，保证最终回收。
 */
void __init kfree_rcu_scheduler_running(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct kfree_rcu_cpu *krcp = per_cpu_ptr(&krc, cpu);

		if (need_offload_krc(krcp))
			schedule_delayed_monitor_work(krcp);
	}
}

/*
 * Queue a request for lazy invocation of the appropriate free routine
 * after a grace period.  Please note that three paths are maintained,
 * two for the common case using arrays of pointers and a third one that
 * is used only when the main paths cannot be used, for example, due to
 * memory pressure.
 *
 * Each kvfree_call_rcu() request is added to a batch. The batch will be drained
 * every KFREE_DRAIN_JIFFIES number of jiffies. All the objects in the batch will
 * be free'd in workqueue context. This allows us to: batch requests together to
 * reduce the number of grace periods during heavy kfree_rcu()/kvfree_rcu() load.
 */
/*
 * 把 @ptr 的实际释放延迟到 RCU 宽限期之后。
 *
 * @head 非 NULL 时必须嵌入 @ptr 对象，允许在原子上下文退化为第三通道；
 * @head 为 NULL 的单参数形式无嵌入节点，只允许 might_sleep 上下文，以便
 * bulk 元数据不足时同步 synchronize_rcu。返回后调用者视 @ptr 已释放并
 * 不得再访问；实际 ownership 转给 sheaf、每 CPU bulk/链表或同步 fallback。
 *
 * 热路径先尝试本地 SLUB sheaf，再用节点页记录；失败时双参数形式串入
 * rcu_head 链，单参数形式解锁后同步等待。debugobjects 防重复入队，KASAN
 * 记录辅助栈，kmemleak 在逻辑释放点停止扫描。成功入队仅安排定时 monitor，
 * 不立即开启每对象 GP，从而批量摊薄回调成本。
 */
void kvfree_call_rcu(struct rcu_head *head, void *ptr)
{
	unsigned long flags;
	struct kfree_rcu_cpu *krcp;
	bool success;

	/*
	 * Please note there is a limitation for the head-less
	 * variant, that is why there is a clear rule for such
	 * objects: it can be used from might_sleep() context
	 * only. For other places please embed an rcu_head to
	 * your data.
	 */
	/*
	 * 无 head 形式无法在内存压力下挂入第三通道，故必须允许同步等待；
	 * 原子/持锁调用者应在对象内嵌 rcu_head 并传入。
	 */
	if (!head)
		might_sleep();

	if (!IS_ENABLED(CONFIG_PREEMPT_RT) && kfree_rcu_sheaf(ptr))
		/*
		 * PREEMPT_RT 对锁/本地性约束不同，禁用 sheaf 快路；成功时 sheaf
		 * 已接管 ownership，公共层无需再建批次。
		 */
		return;

	// Queue the object but don't yet schedule the batch.
	/* debugobjects 原子标记对象为已排队，提前捕获 double kfree_rcu。 */
	if (debug_rcu_head_queue(ptr)) {
		// Probable double kfree_rcu(), just leak.
		/* 疑似重复释放时故意泄漏，避免第二条回收路径造成 UAF。 */
		WARN_ONCE(1, "%s(): Double-freed call. rcu_head %p\n",
			  __func__, head);

		// Mark as success and leave.
		/* 返回即停止处理；首次合法队列仍拥有对象。 */
		return;
	}

	kasan_record_aux_stack(ptr);
	success = add_ptr_to_bulk_krc_lock(&krcp, &flags, ptr, !head);
	if (!success) {
		/*
		 * bulk 失败通常是节点页不足；先异步补页。此处仍持 krcp raw lock，
		 * add_ptr 契约要求所有出口最终 unlock。
		 */
		run_page_cache_worker(krcp);

		if (head == NULL)
			// Inline if kvfree_rcu(one_arg) call.
			/* 单参数形式没有链表节点，跳到解锁后同步 GP fallback。 */
			goto unlock_return;

		head->func = ptr;
		head->next = krcp->head;
		WRITE_ONCE(krcp->head, head);
		atomic_inc(&krcp->head_count);
		/*
		 * 借用 head->func 暂存对象起点，LIFO 串入第三通道；WRITE_ONCE 发布
		 * 新表头，计数供调度/shrinker 使用，ownership 转给 krcp。
		 */

		// Take a snapshot for this krcp.
		/* 快照覆盖当前链所有对象，worker 通过它证明旧读者均已离开。 */
		krcp->head_gp_snap = get_state_synchronize_rcu();
		success = true;
	}

	/*
	 * The kvfree_rcu() caller considers the pointer freed at this point
	 * and likely removes any references to it. Since the actual slab
	 * freeing (and kmemleak_free()) is deferred, tell kmemleak to ignore
	 * this object (no scanning or false positives reporting).
	 */
	/*
	 * 调用者从此不再维护引用，但真实 free/kmemleak_free 延迟；让 kmemleak
	 * 忽略对象可防止扫描陈旧指针和误报“泄漏”。
	 */
	kmemleak_ignore(ptr);

	// Set timer to drain after KFREE_DRAIN_JIFFIES.
	/* RCU 调度器可用后才排 monitor；早期积压由 scheduler_running 补排。 */
	if (rcu_scheduler_active == RCU_SCHEDULER_RUNNING)
		__schedule_delayed_monitor_work(krcp);

unlock_return:
	krc_this_cpu_unlock(krcp, flags);

	/*
	 * Inline kvfree() after synchronize_rcu(). We can do
	 * it from might_sleep() context only, so the current
	 * CPU can pass the QS state.
	 */
	/*
	 * 单参数 fallback 必须先恢复中断/解锁，再 synchronize_rcu；等待期间
	 * 当前 CPU 自身也需报告静止状态，且同步等待绝不能发生在 raw lock 内。
	 */
	if (!success) {
		debug_rcu_head_unqueue((struct rcu_head *) ptr);
		synchronize_rcu();
		kvfree(ptr);
	}
}
EXPORT_SYMBOL_GPL(kvfree_call_rcu);

/*
 * __kvfree_rcu_barrier() - 排空公共每 CPU 队列和所有在途 RCU work。
 * 入参/返回：无；必须在可睡眠上下文。第一遍逐 CPU 把热队列转成 batch；
 * 若两个 batch 忙则 flush 后重试。第二遍取消 monitor 并 flush 全部 work，
 * 返回时调用前已进入公共队列的对象均完成实际释放。并发新入队需由上层
 * 生命周期先阻止，否则 barrier 不能替代调用者的发布/停止协议。
 */
static inline void __kvfree_rcu_barrier(void)
{
	struct kfree_rcu_cpu_work *krwp;
	struct kfree_rcu_cpu *krcp;
	bool queued;
	int i, cpu;

	/*
	 * Firstly we detach objects and queue them over an RCU-batch
	 * for all CPUs. Finally queued works are flushed for each CPU.
	 *
	 * Please note. If there are outstanding batches for a particular
	 * CPU, those have to be finished first following by queuing a new.
	 */
	/*
	 * 先“摘取并排 work”，再统一 flush；若某 CPU 的两个槽都占用，先等待
	 * 旧代完成，才能把热队列的下一代安全装入空槽。
	 */
	for_each_possible_cpu(cpu) {
		krcp = per_cpu_ptr(&krc, cpu);

		/*
		 * Check if this CPU has any objects which have been queued for a
		 * new GP completion. If not(means nothing to detach), we are done
		 * with it. If any batch is pending/running for this "krcp", below
		 * per-cpu flush_rcu_work() waits its completion(see last step).
		 */
		if (!need_offload_krc(krcp))
			continue;

		while (1) {
			/*
			 * If we are not able to queue a new RCU work it means:
			 * - batches for this CPU are still in flight which should
			 *   be flushed first and then repeat;
			 * - no objects to detach, because of concurrency.
			 */
			queued = kvfree_rcu_queue_batch(krcp);

			/*
			 * Bail out, if there is no need to offload this "krcp"
			 * anymore. As noted earlier it can run concurrently.
			 */
			if (queued || !need_offload_krc(krcp))
				break;

			/* There are ongoing batches. */
			/* false 且仍有热对象说明 batch 忙，逐槽 flush 后重新竞争。 */
			for (i = 0; i < KFREE_N_BATCHES; i++) {
				krwp = &(krcp->krw_arr[i]);
				flush_rcu_work(&krwp->rcu_work);
			}
		}
	}

	/*
	 * Now we guarantee that all objects are flushed.
	 */
	/* 第二阶段封闭 monitor 与 batch work，建立 barrier 的完成保证。 */
	for_each_possible_cpu(cpu) {
		krcp = per_cpu_ptr(&krc, cpu);

		/*
		 * A monitor work can drain ready to reclaim objects
		 * directly. Wait its completion if running or pending.
		 */
		/*
		 * monitor 可能绕过 batch 直接释放 ready 对象；同步取消既等待正在
		 * 执行者，也防止它在 barrier 扫描结束后再次运行。
		 */
		cancel_delayed_work_sync(&krcp->monitor_work);

		for (i = 0; i < KFREE_N_BATCHES; i++) {
			krwp = &(krcp->krw_arr[i]);
			flush_rcu_work(&krwp->rcu_work);
		}
	}
}

/**
 * kvfree_rcu_barrier - Wait until all in-flight kvfree_rcu() complete.
 *
 * Note that a single argument of kvfree_rcu() call has a slow path that
 * triggers synchronize_rcu() following by freeing a pointer. It is done
 * before the return from the function. Therefore for any single-argument
 * call that will result in a kfree() to a cache that is to be destroyed
 * during module exit, it is developer's responsibility to ensure that all
 * such calls have returned before the call to kmem_cache_destroy().
 */
/*
 * 等待所有在途 kvfree_rcu 完成。入参/返回：无，必须可睡眠。
 * 先排空 SLUB sheaf，再排公共每 CPU 队列。单参数形式若走同步慢路，会在
 * kvfree_call_rcu 返回前自行完成，barrier 无法等待“尚未返回的并发调用”；
 * 模块退出必须先阻止新调用并等待所有调用者返回，再销毁其 cache。
 */
void kvfree_rcu_barrier(void)
{
	flush_all_rcu_sheaves();
	__kvfree_rcu_barrier();
}
EXPORT_SYMBOL_GPL(kvfree_rcu_barrier);

/**
 * kvfree_rcu_barrier_on_cache - Wait for in-flight kvfree_rcu() calls on a
 *                               specific slab cache.
 * @s: slab cache to wait for
 *
 * See the description of kvfree_rcu_barrier() for details.
 */
/*
 * 为特定 @s 的销毁建立异步释放屏障。@s 是调用期间保持存活的借用 cache。
 * 若该 cache 有 sheaf，先在 CPU hotplug 读锁下逐 CPU flush，再 rcu_barrier
 * 等相关回调；公共 bulk 当前缺乏按 cache 筛选能力，故仍全局排空。无返回
 * 值、可睡眠，通常由 kmem_cache_destroy 在摘除 cache 前调用。
 */
void kvfree_rcu_barrier_on_cache(struct kmem_cache *s)
{
	if (cache_has_sheaves(s)) {
		cpus_read_lock();
		flush_rcu_sheaves_on_cache(s);
		cpus_read_unlock();
		rcu_barrier();
	}

	/*
	 * TODO: Introduce a version of __kvfree_rcu_barrier() that works
	 * on a specific slab cache.
	 */
	/*
	 * TODO 原意：为 __kvfree_rcu_barrier 引入按 cache 过滤版本。当前全局
	 * flush 正确但会等待无关 cache，模块卸载延迟可能更高。
	 */
	__kvfree_rcu_barrier();
}
EXPORT_SYMBOL_GPL(kvfree_rcu_barrier_on_cache);

/*
 * kfree_rcu_shrink_count() - 估算 shrinker 可回收的对象与节点页数量。
 * @shrink/@sc 为 shrinker 框架借用参数，本函数不使用其字段。遍历 possible
 * CPU 汇总热队列计数和页缓存，并设置 refill 退避。返回 SHRINK_EMPTY 或
 * 近似数量；并发入队/释放使它仅是启发式快照，不提供精确 ownership。
 */
static unsigned long
kfree_rcu_shrink_count(struct shrinker *shrink, struct shrink_control *sc)
{
	int cpu;
	unsigned long count = 0;

	/* Snapshot count of all CPUs */
	/* 逐 CPU 原子/READ_ONCE 采样；同时阻止随后立即把刚排空页面补满。 */
	for_each_possible_cpu(cpu) {
		struct kfree_rcu_cpu *krcp = per_cpu_ptr(&krc, cpu);

		count += krc_count(krcp);
		count += READ_ONCE(krcp->nr_bkv_objs);
		atomic_set(&krcp->backoff_page_cache_fill, 1);
	}

	return count == 0 ? SHRINK_EMPTY : count;
}

/*
 * kfree_rcu_shrink_scan() - 在内存压力下实际推进并回收 kvfree_rcu 状态。
 * @sc->nr_to_scan 是目标数量的输入输出预算。逐 CPU 排空空节点页，并同步
 * 调用 monitor：ready 对象直接释放，其余排 RCU work。返回估计释放数量，
 * 无进展返回 SHRINK_STOP；可能调度/释放内存，运行于 shrinker 允许上下文。
 */
static unsigned long
kfree_rcu_shrink_scan(struct shrinker *shrink, struct shrink_control *sc)
{
	int cpu, freed = 0;

	for_each_possible_cpu(cpu) {
		int count;
		struct kfree_rcu_cpu *krcp = per_cpu_ptr(&krc, cpu);

		count = krc_count(krcp);
		count += drain_page_cache(krcp);
		kfree_rcu_monitor(&krcp->monitor_work.work);
		/*
		 * count 是扫描前待处理量加立即释放的缓存页；monitor 可能只把对象
		 * 异步排队，因此该返回值是回收器进度估算，不是同步释放字节数。
		 */

		sc->nr_to_scan -= count;
		freed += count;

		if (sc->nr_to_scan <= 0)
			break;
	}

	return freed == 0 ? SHRINK_STOP : freed;
}

/*
 * kvfree_rcu_init() - 建立批处理释放的全局和每 CPU 基础设施。
 * 入参/返回：无；__init 单线程、可睡眠。
 * 创建带 WQ_MEM_RECLAIM 的专用 workqueue，夹紧补页退避参数，逐 CPU 初始化
 * 两代 batch、三通道、monitor/refill work 并最后发布 initialized=true；
 * 再注册 shrinker。workqueue 失败会 WARN；shrinker 分配失败记录错误并保留
 * 正常批处理功能，只失去内存压力主动回收入口。
 */
void __init kvfree_rcu_init(void)
{
	int cpu;
	int i, j;
	struct shrinker *kfree_rcu_shrinker;

	rcu_reclaim_wq = alloc_workqueue("kvfree_rcu_reclaim",
			WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	WARN_ON(!rcu_reclaim_wq);
	/* 专用队列是 monitor/RCU work 的执行域；WQ_MEM_RECLAIM 保证回收前进。 */

	/* Clamp it to [0:100] seconds interval. */
	/*
	 * 启动参数限制在 0..100 秒，防止负值下溢或过长退避让页缓存永久枯竭。
	 */
	if (rcu_delay_page_cache_fill_msec < 0 ||
		rcu_delay_page_cache_fill_msec > 100 * MSEC_PER_SEC) {

		rcu_delay_page_cache_fill_msec =
			clamp(rcu_delay_page_cache_fill_msec, 0,
				(int) (100 * MSEC_PER_SEC));

		pr_info("Adjusting rcutree.rcu_delay_page_cache_fill_msec to %d ms.\n",
			rcu_delay_page_cache_fill_msec);
	}

	for_each_possible_cpu(cpu) {
		struct kfree_rcu_cpu *krcp = per_cpu_ptr(&krc, cpu);

		for (i = 0; i < KFREE_N_BATCHES; i++) {
			INIT_RCU_WORK(&krcp->krw_arr[i].rcu_work, kfree_rcu_work);
			krcp->krw_arr[i].krcp = krcp;

			for (j = 0; j < FREE_N_CHANNELS; j++)
				INIT_LIST_HEAD(&krcp->krw_arr[i].bulk_head_free[j]);
		}
		/*
		 * 每个 batch 的 rcu_work 回调固定为 kfree_rcu_work，并反向借用
		 * krcp；所有通道先初始化为空，才允许生产者判断槽可用。
		 */

		for (i = 0; i < FREE_N_CHANNELS; i++)
			INIT_LIST_HEAD(&krcp->bulk_head[i]);

		INIT_DELAYED_WORK(&krcp->monitor_work, kfree_rcu_monitor);
		INIT_DELAYED_WORK(&krcp->page_cache_work, fill_page_cache_func);
		/*
		 * initialized 是 per-CPU 发布位，必须最后写：一旦热路径观察为 true，
		 * 链表和 work 已全部可用。引导顺序保证此处不需额外锁。
		 */
		krcp->initialized = true;
	}

	kfree_rcu_shrinker = shrinker_alloc(0, "slab-kvfree-rcu");
	if (!kfree_rcu_shrinker) {
		pr_err("Failed to allocate kfree_rcu() shrinker!\n");
		return;
	}

	kfree_rcu_shrinker->count_objects = kfree_rcu_shrink_count;
	kfree_rcu_shrinker->scan_objects = kfree_rcu_shrink_scan;
	/* 两个回调填写完后再 register，是 shrinker 对外可见的发布边界。 */

	shrinker_register(kfree_rcu_shrinker);
}

#endif /* CONFIG_KVFREE_RCU_BATCHED */
