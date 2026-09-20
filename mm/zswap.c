// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * zswap.c - zswap driver file
 *
 * zswap is a cache that takes pages that are in the process
 * of being swapped out and attempts to compress and store them in a
 * RAM-based memory pool.  This can result in a significant I/O reduction on
 * the swap device and, in the case where decompressing from RAM is faster
 * than reading from the swap device, can also improve workload performance.
 *
 * Copyright (C) 2012  Seth Jennings <sjenning@linux.vnet.ibm.com>
*/
/*
 * zswap 位于 swap 前端：换出时先把页压缩到 RAM 中，换入时优先从 RAM 解压，
 * 从而减少块设备 I/O；内存池达到上限后，后台回写再把较冷条目落到真实 swap。
 * 本文件同时管理压缩算法池、按 swap offset 索引的条目、memcg/NUMA LRU 与回写收缩器。
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* 基础内核设施：模块参数、CPU hotplug、页映射、slab、锁、原子量与 swap 类型。 */
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/swap.h>
/* 压缩、scatterlist、内存策略与 mempool 支撑按 CPU 算法请求和 NUMA 分配。 */
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/mempolicy.h>
#include <linux/mempool.h>
#include <crypto/acompress.h>
#include <crypto/scatterwalk.h>
#include <linux/zswap.h>
/* MM、swapops、回写和页缓存接口连接 swap cache 的 store/load/writeback 生命周期。 */
#include <linux/mm_types.h>
#include <linux/page-flags.h>
#include <linux/swapops.h>
#include <linux/writeback.h>
#include <linux/pagemap.h>
#include <linux/workqueue.h>
/* list_lru 负责 memcg/NUMA 回收分桶，zsmalloc 保存变长压缩对象。 */
#include <linux/list_lru.h>
#include <linux/zsmalloc.h>

#include "swap.h"
#include "internal.h"

/*********************************
* statistics
**********************************/
/* statistics：下列计数用于观察缓存容量、失败原因以及回写效果。 */
/* The number of pages currently stored in zswap */
/* 当前仍由 zswap 持有的 swap 页数；发布/删除条目时成对增减。 */
atomic_long_t zswap_stored_pages = ATOMIC_LONG_INIT(0);
/* The number of incompressible pages currently stored in zswap */
/* 其中以 PAGE_SIZE 原样保存、未获得压缩收益的页数。 */
static atomic_long_t zswap_stored_incompressible_pages = ATOMIC_LONG_INIT(0);

/*
 * The statistics below are not protected from concurrent access for
 * performance reasons so they may not be a 100% accurate.  However,
 * they do provide useful information on roughly how many times a
 * certain event is occurring.
*/
/*
 * 为避免热路径上的同步开销，以下诊断计数不加锁，读者只能把它们当作近似事件次数，
 * 不能据此推导严格守恒关系。
 */

/* Pool limit was hit (see zswap_max_pool_percent) */
/* 压缩池占用达到 max_pool_percent 的次数。 */
static u64 zswap_pool_limit_hit;
/* Pages written back when pool limit was reached */
/* 池满压力下成功回写到 swap 设备的页数。 */
static u64 zswap_written_back_pages;
/* Store failed due to a reclaim failure after pool limit was reached */
/* 池满后回收未能腾出空间，因而拒绝缓存的次数。 */
static u64 zswap_reject_reclaim_fail;
/* Store failed due to compression algorithm failure */
/* 压缩算法返回错误而拒绝缓存的次数。 */
static u64 zswap_reject_compress_fail;
/* Compressed page was too big for the allocator to (optimally) store */
/* 压缩结果过大、不值得按压缩对象保存的次数。 */
static u64 zswap_reject_compress_poor;
/* Load or writeback failed due to decompression failure */
/* 换入或回写时解压失败的次数。 */
static u64 zswap_decompress_fail;
/* Store failed because underlying allocator could not get memory */
/* zsmalloc 无法分配对象空间而拒绝缓存的次数。 */
static u64 zswap_reject_alloc_fail;
/* Store failed because the entry metadata could not be allocated (rare) */
/* 无法分配 zswap_entry 元数据而拒绝缓存的次数。 */
static u64 zswap_reject_kmemcache_fail;

/* Shrinker work queue */
/* 承载异步 memcg 回收工作的专用队列。 */
static struct workqueue_struct *shrink_wq;
/* Pool limit was hit, we need to calm down */
/* 达到高水位后保持拒收，直到占用降至 accept threshold。 */
static bool zswap_pool_reached_full;

/*********************************
* tunables
**********************************/
/* tunables：模块参数控制开关、压缩算法以及容量滞回阈值。 */

#define ZSWAP_PARAM_UNSET ""

static int zswap_setup(void);

/* Enable/disable zswap */
/* enabled 控制新请求是否使用 zswap；static key 记录本次启动是否曾启用过它。 */
static DEFINE_STATIC_KEY_MAYBE(CONFIG_ZSWAP_DEFAULT_ON, zswap_ever_enabled);
static bool zswap_enabled = IS_ENABLED(CONFIG_ZSWAP_DEFAULT_ON);
static int zswap_enabled_param_set(const char *,
				   const struct kernel_param *);
static const struct kernel_param_ops zswap_enabled_param_ops = {
	.set =		zswap_enabled_param_set,
	.get =		param_get_bool,
};
module_param_cb(enabled, &zswap_enabled_param_ops, &zswap_enabled, 0644);

/* Crypto compressor to use */
/* compressor 是新建当前池使用的异步压缩算法名；旧池可继续服务既有条目。 */
static char *zswap_compressor = CONFIG_ZSWAP_COMPRESSOR_DEFAULT;
static int zswap_compressor_param_set(const char *,
				      const struct kernel_param *);
static const struct kernel_param_ops zswap_compressor_param_ops = {
	.set =		zswap_compressor_param_set,
	.get =		param_get_charp,
	.free =		param_free_charp,
};
module_param_cb(compressor, &zswap_compressor_param_ops,
		&zswap_compressor, 0644);

/* The maximum percentage of memory that the compressed pool can occupy */
/* 所有 zsmalloc 池合计最多可占系统总页数的百分比。 */
static unsigned int zswap_max_pool_percent = 20;
module_param_named(max_pool_percent, zswap_max_pool_percent, uint, 0644);

/* The threshold for accepting new pages after the max_pool_percent was hit */
/* 触及上限后，只有降到该百分比对应的低水位才重新接收条目。 */
static unsigned int zswap_accept_thr_percent = 90; /* of max pool size */
/* 此百分比以 max_pool_percent 对应的池上限为基数，而非直接以总内存为基数。 */
module_param_named(accept_threshold_percent, zswap_accept_thr_percent,
		   uint, 0644);

/* Enable/disable memory pressure-based shrinker. */
/* 是否让内存压力通过 shrinker 主动回写 zswap 条目。 */
static bool zswap_shrinker_enabled = IS_ENABLED(
		CONFIG_ZSWAP_SHRINKER_DEFAULT_ON);
module_param_named(shrinker_enabled, zswap_shrinker_enabled, bool, 0644);

/*
 * zswap_is_enabled() - 查询当前是否允许 zswap 接收新的换出页。
 * 返回：模块参数 enabled 的瞬时布尔值；不取得引用、不睡眠，供 swap 热路径快速判断。
 * 并发说明：参数写入由内核参数框架串行化，调用者只需要一个近似的当前开关视图。
 */
bool zswap_is_enabled(void)
{
	return zswap_enabled;
}

/*
 * zswap_never_enabled() - 判断本次启动中 zswap 是否从未完成过启用。
 * 返回：static key 尚未被打开时为 true；无参数、无所有权转移且可用于裁剪永久无效路径。
 */
bool zswap_never_enabled(void)
{
	return !static_branch_maybe(CONFIG_ZSWAP_DEFAULT_ON, &zswap_ever_enabled);
}

/*********************************
* data structures
**********************************/
/* data structures：这些对象把压缩数据、索引、计费和回收生命周期连接起来。 */

struct crypto_acomp_ctx {
	/* 每 CPU 保存算法实例、复用请求和一页输出缓冲区，mutex 串行化同一 CPU 上的压缩操作。 */
	struct crypto_acomp *acomp;
	struct acomp_req *req;
	struct crypto_wait wait;
	u8 *buffer;
	struct mutex mutex;
};

/*
 * The lock ordering is zswap_tree.lock -> zswap_pool.lru_lock.
 * The only case where lru_lock is not acquired while holding tree.lock is
 * when a zswap_entry is taken off the lru for writeback, in that case it
 * needs to be verified that it's still valid in the tree.
 */
/*
 * 锁顺序固定为 zswap_tree.lock 再到 zswap_pool.lru_lock；回写从 LRU 摘取条目时是唯一反向入口，
 * 因而释放 LRU 锁后必须回到 xarray 验证条目仍是同一对象，才能继续访问。
 */
struct zswap_pool {
	/* zs_pool 拥有压缩对象；acomp_ctx 是按 CPU 分配的算法上下文。 */
	struct zs_pool *zs_pool;
	struct crypto_acomp_ctx __percpu *acomp_ctx;
	struct percpu_ref ref;
	struct list_head list;
	struct work_struct release_work;
	struct hlist_node node;
	char tfm_name[CRYPTO_MAX_ALG_NAME];
};

/* Global LRU lists shared by all zswap pools. */
/* 所有池共享一个按 memcg/NUMA 分桶的 list_lru，条目自身仍记录所属 pool。 */
static struct list_lru zswap_list_lru;

/* The lock protects zswap_next_shrink updates. */
/* 该锁只保护全局轮转游标 zswap_next_shrink，实际 LRU 有自己的锁。 */
static DEFINE_SPINLOCK(zswap_shrink_lock);
static struct mem_cgroup *zswap_next_shrink;
static struct work_struct zswap_shrink_work;
static struct shrinker *zswap_shrinker;

/*
 * struct zswap_entry
 *
 * This structure contains the metadata for tracking a single compressed
 * page within zswap.
 *
 * swpentry - associated swap entry, the offset indexes into the xarray
 * length - the length in bytes of the compressed page data.  Needed during
 *          decompression.
 * referenced - true if the entry recently entered the zswap pool. Unset by the
 *              writeback logic. The entry is only reclaimed by the writeback
 *              logic if referenced is unset. See comments in the shrinker
 *              section for context.
 * pool - the zswap_pool the entry's data is in
 * handle - zsmalloc allocation handle that stores the compressed page data
 * objcg - the obj_cgroup that the compressed memory is charged to
 * lru - handle to the pool's lru used to evict pages.
 */
/*
 * zswap_entry 是一个 swap slot 在 RAM 缓存中的完整元数据：swpentry 负责 xarray 定位，
 * handle/length/pool 定位压缩数据，objcg 维持计费生命周期，lru 参与 memcg/NUMA 回收。
 * referenced 实现收缩器的二次机会；条目发布后由对应 swap tree 拥有，删除路径最终统一释放资源。
 */
struct zswap_entry {
	swp_entry_t swpentry;
	unsigned int length;
	bool referenced;
	struct zswap_pool *pool;
	unsigned long handle;
	struct obj_cgroup *objcg;
	struct list_head lru;
};

/* 每个 swap type 按 64M swap 区间切成 xarray 数组；计数用于 swapon/swapoff 生命周期。 */
static struct xarray *zswap_trees[MAX_SWAPFILES];
static unsigned int nr_zswap_trees[MAX_SWAPFILES];

/* RCU-protected iteration */
/* 池链表按“当前池在头部”排列，读侧用 RCU，修改侧还需 zswap_pools_lock。 */
static LIST_HEAD(zswap_pools);
/* protects zswap_pools list modification */
/* 保护池链表增删、换头以及需要稳定当前池身份的操作。 */
static DEFINE_SPINLOCK(zswap_pools_lock);
/* pool counter to provide unique names to zsmalloc */
/* 只用于生成 zsmalloc 可区分的池名，不表达活跃池数量。 */
static atomic_t zswap_pools_count = ATOMIC_INIT(0);

enum zswap_init_type {
	ZSWAP_UNINIT,
	ZSWAP_INIT_SUCCEED,
	ZSWAP_INIT_FAILED
};

/* 初始化状态把未尝试、成功和不可重试失败区分开。 */
static enum zswap_init_type zswap_init_state;

/* used to ensure the integrity of initialization */
/* 串行化首次 setup 与运行时参数变更，避免观察到半初始化状态。 */
static DEFINE_MUTEX(zswap_init_lock);

/* init completed, but couldn't create the initial pool */
/* setup 框架已就绪但没有可用压缩池时为 false，此时不能真正启用缓存。 */
static bool zswap_has_pool;

/*********************************
* helpers and fwd declarations
**********************************/
/* helpers：swap 地址分片和池调试输出等不改变业务状态的基础操作。 */

/* One swap address space for each 64M swap space */
/* 一个 xarray 覆盖 2^14 个页槽，即 64 MiB 的 4 KiB swap 地址空间。 */
#define ZSWAP_ADDRESS_SPACE_SHIFT 14
#define ZSWAP_ADDRESS_SPACE_PAGES (1 << ZSWAP_ADDRESS_SPACE_SHIFT)
/*
 * swap_zswap_tree() - 找到 swap entry 所属的分片 xarray。
 * @swp: 含 swap type 与页偏移的条目值，必须来自已完成 zswap_swapon() 的 swap 设备。
 * 返回：借用的 xarray 指针；不取得引用、不睡眠，调用者负责使用 xa 锁或相应 xa API。
 */
static inline struct xarray *swap_zswap_tree(swp_entry_t swp)
{
	return &zswap_trees[swp_type(swp)][swp_offset(swp)
		>> ZSWAP_ADDRESS_SPACE_SHIFT];
}

#define zswap_pool_debug(msg, p)			\
	pr_debug("%s pool %s\n", msg, (p)->tfm_name)

/*********************************
* pool functions
**********************************/
/* pool functions：RCU 链表决定当前池，percpu_ref 决定退役池何时可销毁。 */
static void __zswap_pool_empty(struct percpu_ref *ref);

/*
 * acomp_ctx_free() - 释放一个 per-CPU 异步压缩上下文中已经成功建立的部分。
 * @acomp_ctx: 可为 NULL；成员可能是未初始化、错误指针或已被本函数清空的状态。
 * 返回：无；依次释放请求、算法实例和缓冲区，并把指针清零，允许错误回滚重复调用。
 * 上下文：只能在该 CPU 上下文已停止使用后调用，释放路径可能睡眠。
 */
static void acomp_ctx_free(struct crypto_acomp_ctx *acomp_ctx)
{
	if (!acomp_ctx)
		return;

	/*
	 * If there was an error in allocating @acomp_ctx->req, it
	 * would be set to NULL.
	 */
	/* 请求分配失败时 req 为 NULL，因此只释放已成功创建的请求。 */
	if (acomp_ctx->req)
		acomp_request_free(acomp_ctx->req);

	acomp_ctx->req = NULL;

	/*
	 * We have to handle both cases here: an error pointer return from
	 * crypto_alloc_acomp_node(); and a) NULL initialization by zswap, or
	 * b) NULL assignment done in a previous call to acomp_ctx_free().
	 */
	/* 同时兼容算法分配的 ERR_PTR、初始 NULL 和先前清理留下的 NULL。 */
	if (!IS_ERR_OR_NULL(acomp_ctx->acomp))
		crypto_free_acomp(acomp_ctx->acomp);

	acomp_ctx->acomp = NULL;

	kfree(acomp_ctx->buffer);
	acomp_ctx->buffer = NULL;
}

/*
 * zswap_pool_create() - 为指定压缩算法创建尚未发布的新池。
 * @compressor: NUL 结尾的算法名；空占位只在已有池的特殊场景有效，字符串由调用者持有。
 * 返回：成功返回拥有初始 current 引用的 pool；失败返回 NULL 并回滚所有 CPU/crypto/zsmalloc 资源。
 * 上下文：可睡眠；CPU hotplug instance 使初始化期间相关 CPU 不能下线，调用者随后须发布或销毁池。
 */
static struct zswap_pool *zswap_pool_create(char *compressor)
{
	struct zswap_pool *pool;
	char name[38]; /* 'zswap' + 32 char (max) num + \0 */
	/* 名称容量覆盖 "zswap"、最大 32 位十六进制序号以及结尾 NUL。 */
	int ret, cpu;

	if (!zswap_has_pool && !strcmp(compressor, ZSWAP_PARAM_UNSET))
		return NULL;

	pool = kzalloc_obj(*pool);
	if (!pool)
		return NULL;

	/* unique name for each pool specifically required by zsmalloc */
	/* zsmalloc 要求池名唯一，序号只用于命名而不参与查找。 */
	snprintf(name, 38, "zswap%x", atomic_inc_return(&zswap_pools_count));
	pool->zs_pool = zs_create_pool(name);
	if (!pool->zs_pool)
		goto error;

	strscpy(pool->tfm_name, compressor, sizeof(pool->tfm_name));

	/* Many things rely on the zero-initialization. */
	/* 错误回滚以成员初始为零为判据，因此 per-CPU 块必须带 __GFP_ZERO。 */
	pool->acomp_ctx = alloc_percpu_gfp(*pool->acomp_ctx,
					   GFP_KERNEL | __GFP_ZERO);
	if (!pool->acomp_ctx) {
		pr_err("percpu alloc failed\n");
		goto error;
	}

	/*
	 * This is serialized against CPU hotplug operations. Hence, cores
	 * cannot be offlined until this finishes.
	 */
	/* 注册 instance 与 CPU hotplug 串行化；回调完成前 CPU 不会被并发下线。 */
	ret = cpuhp_state_add_instance(CPUHP_MM_ZSWP_POOL_PREPARE,
				       &pool->node);

	/*
	 * cpuhp_state_add_instance() will not cleanup on failure since
	 * we don't register a hotunplug callback.
	 */
	/* 本 instance 没有 hotunplug 回调，注册失败不会替调用者清理由已上线 CPU 建立的上下文。 */
	if (ret)
		goto cpuhp_add_fail;

	/* being the current pool takes 1 ref; this func expects the
	 * caller to always add the new pool as the current pool
	 */
	/* 初始 percpu_ref 代表“当前池”身份；调用者发布时必须把新池放到链表头。 */
	ret = percpu_ref_init(&pool->ref, __zswap_pool_empty,
			      PERCPU_REF_ALLOW_REINIT, GFP_KERNEL);
	if (ret)
		goto ref_fail;
	INIT_LIST_HEAD(&pool->list);

	/* 返回前池只由初始 ref 拥有，尚未进入 RCU 链表。 */
	zswap_pool_debug("created", pool);

	return pool;

ref_fail:
	/* percpu_ref 建立失败时先撤销 hotplug instance，再清理它已准备的各 CPU 上下文。 */
	cpuhp_state_remove_instance(CPUHP_MM_ZSWP_POOL_PREPARE, &pool->node);

cpuhp_add_fail:
	for_each_possible_cpu(cpu)
		acomp_ctx_free(per_cpu_ptr(pool->acomp_ctx, cpu));
error:
	/* 所有错误出口按成员是否成功建立逆序释放，零初始化使这些判断可靠。 */
	if (pool->acomp_ctx)
		free_percpu(pool->acomp_ctx);
	if (pool->zs_pool)
		zs_destroy_pool(pool->zs_pool);
	kfree(pool);
	return NULL;
}

/*
 * __zswap_pool_create_fallback() - 建立初始池，并在配置算法缺失时退回 Kconfig 默认算法。
 * 返回：成功返回尚未发布且持初始引用的池；默认算法也不可用时返回 NULL 并清空算法参数。
 * 上下文：仅初始化/受 zswap_init_lock 保护的路径调用，可睡眠；默认算法缺失被视为配置缺陷。
 */
static struct zswap_pool *__zswap_pool_create_fallback(void)
{
	if (!crypto_has_acomp(zswap_compressor, 0, 0) &&
	    strcmp(zswap_compressor, CONFIG_ZSWAP_COMPRESSOR_DEFAULT)) {
		pr_err("compressor %s not available, using default %s\n",
		       zswap_compressor, CONFIG_ZSWAP_COMPRESSOR_DEFAULT);
		param_free_charp(&zswap_compressor);
		zswap_compressor = CONFIG_ZSWAP_COMPRESSOR_DEFAULT;
	}

	/* Default compressor should be available. Kconfig bug? */
	/* Kconfig 承诺默认算法可用；这里保留告警并安全退化为“没有池”。 */
	if (WARN_ON_ONCE(!crypto_has_acomp(zswap_compressor, 0, 0))) {
		zswap_compressor = ZSWAP_PARAM_UNSET;
		return NULL;
	}

	return zswap_pool_create(zswap_compressor);
}

/*
 * zswap_pool_destroy() - 最终销毁已从池链表摘除且引用归零的池。
 * @pool: 独占拥有的池；不得再有 RCU 读者、条目引用或当前池引用。
 * 返回：无；撤销 CPU hotplug instance，释放每 CPU crypto 上下文和 zsmalloc 池，最后释放元数据。
 * 上下文：可能睡眠，只能由 RCU 宽限期后的 release work 调用。
 */
static void zswap_pool_destroy(struct zswap_pool *pool)
{
	int cpu;

	zswap_pool_debug("destroying", pool);

	cpuhp_state_remove_instance(CPUHP_MM_ZSWP_POOL_PREPARE, &pool->node);

	/* instance 已移除，后续不会再有 CPU prepare 回调触及这些 per-CPU 对象。 */
	for_each_possible_cpu(cpu)
		acomp_ctx_free(per_cpu_ptr(pool->acomp_ctx, cpu));

	free_percpu(pool->acomp_ctx);

	zs_destroy_pool(pool->zs_pool);
	kfree(pool);
}

/*
 * __zswap_pool_release() - 在工作队列中完成 percpu_ref 归零池的延迟销毁。
 * @work: 嵌入 pool 的 release_work，ownership 随本工作传入。
 * 返回：无；等待 RCU 旧读者退出，退出 percpu_ref 后销毁池；可睡眠。
 */
static void __zswap_pool_release(struct work_struct *work)
{
	struct zswap_pool *pool = container_of(work, typeof(*pool),
						release_work);

	synchronize_rcu();

	/* nobody should have been able to get a ref... */
	/* 池已不可获取新引用；宽限期后仍非零说明生命周期协议被破坏。 */
	WARN_ON(!percpu_ref_is_zero(&pool->ref));
	percpu_ref_exit(&pool->ref);

	/* pool is now off zswap_pools list and has no references. */
	/* 此时链表和引用两条可达路径都已消失，可以释放底层资源。 */
	zswap_pool_destroy(pool);
}

static struct zswap_pool *zswap_pool_current(void);

/*
 * __zswap_pool_empty() - percpu_ref 归零回调，把退役池摘链并安排最终释放。
 * @ref: 已进入 atomic/kill 流程且刚归零的 pool 引用对象。
 * 返回：无；在 zswap_pools_lock 下更新 RCU 链表，实际释放延后到可睡眠 work。
 * 上下文：回调不能睡眠；当前池按协议始终持有初始引用，因此不应在这里出现。
 */
static void __zswap_pool_empty(struct percpu_ref *ref)
{
	struct zswap_pool *pool;

	pool = container_of(ref, typeof(*pool), ref);

	/* empty 回调与参数切换共用池锁，确保当前池检查和摘链是同一个原子状态转换。 */
	spin_lock_bh(&zswap_pools_lock);

	WARN_ON(pool == zswap_pool_current());

	list_del_rcu(&pool->list);

	/* RCU 等待和 crypto/zsmalloc 释放可能睡眠，必须转交普通 work context。 */
	INIT_WORK(&pool->release_work, __zswap_pool_release);
	schedule_work(&pool->release_work);

	spin_unlock_bh(&zswap_pools_lock);
}

/*
 * zswap_pool_tryget() - 尝试为仍存活的池取得一个条目/临时引用。
 * @pool: 可为 NULL 的借用指针。
 * 返回：成功取得引用返回非零；NULL 或已被 kill 的池返回 0；不睡眠。
 */
static int __must_check zswap_pool_tryget(struct zswap_pool *pool)
{
	if (!pool)
		return 0;

	return percpu_ref_tryget(&pool->ref);
}

/* The caller must already have a reference. */
/* 调用者已持一个有效引用时，才能无条件再复制一份引用。 */
/*
 * zswap_pool_get() - 在调用者已稳定 pool 生命周期的前提下增加引用。
 * @pool: 已持引用的池；返回无，新增引用必须由 zswap_pool_put() 配对释放；不睡眠。
 */
static void zswap_pool_get(struct zswap_pool *pool)
{
	percpu_ref_get(&pool->ref);
}

/*
 * zswap_pool_put() - 释放一个池引用。
 * @pool: 调用者拥有一份引用的池；返回无，最后引用可能触发异步 empty 回调；不睡眠。
 */
static void zswap_pool_put(struct zswap_pool *pool)
{
	percpu_ref_put(&pool->ref);
}

/*
 * __zswap_pool_current() - 在 RCU 读侧或池锁保护下读取链表头的当前池。
 * 返回：借用的 pool 指针，可能为 NULL；本函数不取引用，离开保护区前不得长期保存。
 */
static struct zswap_pool *__zswap_pool_current(void)
{
	struct zswap_pool *pool;

	pool = list_first_or_null_rcu(&zswap_pools, typeof(*pool), list);
	WARN_ONCE(!pool && zswap_has_pool,
		  "%s: no page storage pool!\n", __func__);

	return pool;
}

/*
 * zswap_pool_current() - 在持有 zswap_pools_lock 时取得当前池借用指针。
 * 返回：链表头或 NULL；锁断言保证调用者可稳定其身份，但不增加引用。
 */
static struct zswap_pool *zswap_pool_current(void)
{
	assert_spin_locked(&zswap_pools_lock);

	return __zswap_pool_current();
}

/*
 * zswap_pool_current_get() - 取得当前池的稳定引用。
 * 返回：成功返回需由 zswap_pool_put() 释放的 pool；无池或池正退役返回 NULL。
 * 并发说明：RCU 稳定指针，percpu_ref_tryget() 阻止池在返回后销毁；不睡眠。
 */
static struct zswap_pool *zswap_pool_current_get(void)
{
	struct zswap_pool *pool;

	rcu_read_lock();

	pool = __zswap_pool_current();
	if (!zswap_pool_tryget(pool))
		pool = NULL;

	rcu_read_unlock();

	return pool;
}

/* type and compressor must be null-terminated */
/* compressor 必须是 NUL 结尾字符串；历史注释中的 type 在当前签名中已不存在。 */
/*
 * zswap_pool_find_get() - 在池链表中按压缩算法查找并取得可复用池引用。
 * @compressor: NUL 结尾算法名，调用期间借用。
 * 返回：匹配且未归零的池引用，需 put；否则返回 NULL。
 * 锁：调用者必须持 zswap_pools_lock；即将销毁的匹配池会被跳过，不睡眠。
 */
static struct zswap_pool *zswap_pool_find_get(char *compressor)
{
	struct zswap_pool *pool;

	assert_spin_locked(&zswap_pools_lock);

	list_for_each_entry_rcu(pool, &zswap_pools, list) {
		if (strcmp(pool->tfm_name, compressor))
			continue;
		/* if we can't get it, it's about to be destroyed */
		/* 名称虽匹配但 tryget 失败表示 kill 已开始，不能复活这次退役。 */
		if (!zswap_pool_tryget(pool))
			continue;
		return pool;
	}

	return NULL;
}

/*
 * zswap_max_pages() - 把容量参数换算为允许占用的物理页数上限。
 * 返回：totalram_pages() 的 max_pool_percent 百分比；只做快照计算，不睡眠。
 */
static unsigned long zswap_max_pages(void)
{
	return totalram_pages() * zswap_max_pool_percent / 100;
}

/*
 * zswap_accept_thr_pages() - 计算池满后重新接收条目的低水位。
 * 返回：最大池页数的 accept_thr_percent 百分比，形成高低水位滞回；不睡眠。
 */
static unsigned long zswap_accept_thr_pages(void)
{
	return zswap_max_pages() * zswap_accept_thr_percent / 100;
}

/*
 * zswap_total_pages() - 汇总所有活跃/退役未销毁 zsmalloc 池占用的物理页。
 * 返回：RCU 快照下各池 zs_get_total_pages() 之和；不取得池引用，RCU 保证遍历安全。
 */
unsigned long zswap_total_pages(void)
{
	struct zswap_pool *pool;
	unsigned long total = 0;

	rcu_read_lock();
	list_for_each_entry_rcu(pool, &zswap_pools, list)
		total += zs_get_total_pages(pool->zs_pool);
	rcu_read_unlock();

	return total;
}

/*
 * zswap_check_limits() - 更新并查询 zswap 容量滞回状态。
 * 返回：仍处于“池已满、拒收新页”状态时为 true；降到低水位后返回 false。
 * 并发说明：诊断计数和布尔状态允许近似竞争，结果用于限流而非资源安全；不睡眠。
 */
static bool zswap_check_limits(void)
{
	unsigned long cur_pages = zswap_total_pages();
	unsigned long max_pages = zswap_max_pages();

	if (cur_pages >= max_pages) {
		zswap_pool_limit_hit++;
		zswap_pool_reached_full = true;
	} else if (zswap_pool_reached_full &&
		   cur_pages <= zswap_accept_thr_pages()) {
		/* 只有越过低水位才清除满标记，避免在上限附近反复启停。 */
			zswap_pool_reached_full = false;
	}
	return zswap_pool_reached_full;
}

/*********************************
* param callbacks
**********************************/
/* param callbacks：运行时换算法会发布新当前池，旧池保留到其中条目全部释放。 */

/*
 * zswap_compressor_param_set() - 处理 compressor 模块参数并原子切换当前压缩池。
 * @val: 用户输入的算法名，解析期间临时借用；@kp: 指向 zswap_compressor 参数描述。
 * 返回：0 表示参数与当前池已切换；-ENOENT 表示算法不存在，-EINVAL 表示建池失败，或参数层 errno。
 * 生命周期：可睡眠；init_lock 串行化初始化状态，pools_lock 保护 RCU 链表，旧当前池通过 kill 异步退役。
 */
static int zswap_compressor_param_set(const char *val, const struct kernel_param *kp)
{
	struct zswap_pool *pool, *put_pool = NULL;
	char *s = strstrip((char *)val);
	bool create_pool = false;
	int ret = 0;

	mutex_lock(&zswap_init_lock);
	switch (zswap_init_state) {
	case ZSWAP_UNINIT:
		/* Handled in zswap_setup() */
		/* 尚未 setup 时只保存字符串，初始池稍后统一创建。 */
		ret = param_set_charp(s, kp);
		break;
	case ZSWAP_INIT_SUCCEED:
		/* 已初始化时只有“尚无池”或算法名变化才需要新建/复用池。 */
		if (!zswap_has_pool || strcmp(s, *(char **)kp->arg))
			create_pool = true;
		break;
	case ZSWAP_INIT_FAILED:
		pr_err("can't set param, initialization failed\n");
		ret = -ENODEV;
	}
	mutex_unlock(&zswap_init_lock);

	/* 算法名未变化且已有池时，无需触碰 RCU 池链表。 */
	if (!create_pool)
		return ret;

	if (!crypto_has_acomp(s, 0, 0)) {
		pr_err("compressor %s not available\n", s);
		return -ENOENT;
	}

	spin_lock_bh(&zswap_pools_lock);

	/* 先尝试复用尚在链表中的同算法退役池，减少昂贵的 crypto/zsmalloc 重建。 */
	pool = zswap_pool_find_get(s);
	if (pool) {
		zswap_pool_debug("using existing", pool);
		WARN_ON(pool == zswap_pool_current());
		list_del_rcu(&pool->list);
	}

	spin_unlock_bh(&zswap_pools_lock);

	/* 链表锁外创建池，因为分配和 CPU hotplug 注册都可能睡眠。 */
	if (!pool)
		pool = zswap_pool_create(s);
	else {
		/*
		 * Restore the initial ref dropped by percpu_ref_kill()
		 * when the pool was decommissioned and switch it again
		 * to percpu mode.
		 */
		/* 复用退役但尚未销毁的空池时，恢复其“当前池”初始引用并切回 percpu 模式。 */
		percpu_ref_resurrect(&pool->ref);

		/* Drop the ref from zswap_pool_find_get(). */
		/* resurrect 已建立身份引用，释放查找阶段取得的临时引用。 */
		zswap_pool_put(pool);
	}

	if (pool)
		ret = param_set_charp(s, kp);
	else
		ret = -EINVAL;

	/* 参数字符串写入成功与新池成为链表头必须在同一受锁发布阶段收口。 */
	spin_lock_bh(&zswap_pools_lock);

	if (!ret) {
		put_pool = zswap_pool_current();
		list_add_rcu(&pool->list, &zswap_pools);
		zswap_has_pool = true;
	} else if (pool) {
		/*
		 * Add the possibly pre-existing pool to the end of the pools
		 * list; if it's new (and empty) then it'll be removed and
		 * destroyed by the put after we drop the lock
		 */
		/* 参数更新失败时把候选池放到尾部，使其不是 current；随后 kill 会在归零时摘链销毁。 */
		list_add_tail_rcu(&pool->list, &zswap_pools);
		put_pool = pool;
	}

	spin_unlock_bh(&zswap_pools_lock);

	/*
	 * Drop the ref from either the old current pool,
	 * or the new pool we failed to add
	 */
	/* 成功时 kill 旧 current，失败时 kill 未采用候选；两者都由现存 entry 引用延长寿命。 */
	if (put_pool)
		percpu_ref_kill(&put_pool->ref);

	return ret;
}

/*
 * zswap_enabled_param_set() - 处理 enabled 参数，必要时延迟执行一次 zswap_setup()。
 * @val: 布尔文本；@kp: enabled 参数描述，均只在调用期借用。
 * 返回：参数写入结果；setup/状态失败返回 -ENODEV。运行期路径持 init_lock 且可能睡眠。
 */
static int zswap_enabled_param_set(const char *val,
				   const struct kernel_param *kp)
{
	int ret = -ENODEV;

	/* if this is load-time (pre-init) param setting, only set param. */
	/* 启动参数解析早于完整内核运行态，此时不能分配整套资源，只记录期望值。 */
	if (system_state != SYSTEM_RUNNING)
		return param_set_bool(val, kp);

	mutex_lock(&zswap_init_lock);
	/* 运行期首次从 off 切 on 可以在这里惰性 setup；失败状态禁止再次使用半初始化资源。 */
	switch (zswap_init_state) {
	case ZSWAP_UNINIT:
		if (zswap_setup())
			break;
		fallthrough;
	case ZSWAP_INIT_SUCCEED:
		/* setup 成功但无池时仍不能接受 enabled=true，避免热路径取得空 current。 */
		if (!zswap_has_pool)
			pr_err("can't enable, no pool configured\n");
		else
			ret = param_set_bool(val, kp);
		break;
	case ZSWAP_INIT_FAILED:
		pr_err("can't enable, initialization failed\n");
	}
	/* 参数更新完成后才释放 init_lock，使并发写者只能看到完整状态。 */
	mutex_unlock(&zswap_init_lock);

	return ret;
}

/*********************************
* lru functions
**********************************/
/* lru functions：把条目按其元数据 NUMA 节点和 objcg 组织到全局 list_lru。 */

/* should be called under RCU */
/* 必须在 RCU 读侧临界区内调用，防止 objcg 到 memcg 的关联在解析时消失。 */
#ifdef CONFIG_MEMCG
/*
 * mem_cgroup_from_entry() - 从条目的 objcg 解析负责回收的 memcg。
 * @entry: 已稳定生命周期的 zswap 条目。
 * 返回：RCU 保护下借用的 memcg，未计费或关闭 CONFIG_MEMCG 时返回 NULL；不增加引用。
 */
static inline struct mem_cgroup *mem_cgroup_from_entry(struct zswap_entry *entry)
{
	return entry->objcg ? obj_cgroup_memcg(entry->objcg) : NULL;
}
#else
/* CONFIG_MEMCG 关闭时所有条目都归入根 list_lru。 */
static inline struct mem_cgroup *mem_cgroup_from_entry(struct zswap_entry *entry)
{
	return NULL;
}
#endif

/*
 * entry_to_nid() - 以 entry 元数据所在页确定 list_lru 的 NUMA 分桶。
 * @entry: slab 分配且仍存活的条目；返回其 backing page 的 nid，不转移所有权、不睡眠。
 */
static inline int entry_to_nid(struct zswap_entry *entry)
{
	return page_to_nid(virt_to_page(entry));
}

/*
 * zswap_lru_add() - 把新发布条目加入对应 memcg/NUMA 的回收链表。
 * @list_lru: 全局 zswap list_lru；@entry: 已初始化但尚未在该 LRU 上的条目。
 * 返回：无；RCU 稳定 memcg，list_lru 内部锁串行化链表；调用者仍拥有 entry。
 */
static void zswap_lru_add(struct list_lru *list_lru, struct zswap_entry *entry)
{
	int nid = entry_to_nid(entry);
	struct mem_cgroup *memcg;

	/*
	 * Note that it is safe to use rcu_read_lock() here, even in the face of
	 * concurrent memcg offlining:
	 *
	 * 1. list_lru_add() is called before list_lru_one is dead. The
	 *    new entry will be reparented to memcg's parent's list_lru.
	 * 2. list_lru_add() is called after list_lru_one is dead. The
	 *    new entry will be added directly to memcg's parent's list_lru.
	 *
	 * Similar reasoning holds for list_lru_del().
	 */
	/*
	 * memcg 下线与这里并发仍安全：若子 list 尚活，离线流程稍后把新条目迁给父组；若已死，
	 * list_lru_add() 会直接解析到父组。删除路径也遵循同一重定向规则。
	 */
	rcu_read_lock();
	memcg = mem_cgroup_from_entry(entry);
	/* will always succeed */
	/* entry 尚未在 LRU 上，且目标桶由 list_lru 保证存在，因此加入按协议必定成功。 */
	list_lru_add(list_lru, &entry->lru, nid, memcg);
	rcu_read_unlock();
}

/*
 * zswap_lru_del() - 从对应 memcg/NUMA 回收链表摘除条目。
 * @list_lru: 全局 zswap list_lru；@entry: 当前仍链接在其中的稳定条目。
 * 返回：无；RCU 处理 memcg 下线重定向，摘链完成后调用者可释放 entry。
 */
static void zswap_lru_del(struct list_lru *list_lru, struct zswap_entry *entry)
{
	int nid = entry_to_nid(entry);
	struct mem_cgroup *memcg;

	rcu_read_lock();
	memcg = mem_cgroup_from_entry(entry);
	/* will always succeed */
	/* 生命周期协议保证 entry 尚在 LRU 中，因此删除无需失败分支。 */
	list_lru_del(list_lru, &entry->lru, nid, memcg);
	rcu_read_unlock();
}

/*
 * zswap_lruvec_state_init() - 初始化 lruvec 的 zswap 换入反馈计数。
 * @lruvec: 正在建立的 lruvec，调用者独占初始化阶段。
 * 返回：无；把磁盘 swapin 次数清零，不取得引用、不睡眠。
 */
void zswap_lruvec_state_init(struct lruvec *lruvec)
{
	atomic_long_set(&lruvec->zswap_lruvec_state.nr_disk_swapins, 0);
}

/*
 * zswap_folio_swapin() - 记录一次真实磁盘换入，作为 zswap shrinker 减速反馈。
 * @folio: 可为 NULL；非空时借其当前 memcg/node 定位 lruvec。
 * 返回：无；RCU 稳定 folio_lruvec 映射，原子增加计数，不保留 folio 或 lruvec 引用。
 */
void zswap_folio_swapin(struct folio *folio)
{
	struct lruvec *lruvec;

	if (folio) {
		rcu_read_lock();
		lruvec = folio_lruvec(folio);
		atomic_long_inc(&lruvec->zswap_lruvec_state.nr_disk_swapins);
		rcu_read_unlock();
	}
}

/*
 * This function should be called when a memcg is being offlined.
 *
 * Since the global shrinker shrink_worker() may hold a reference
 * of the memcg, we must check and release the reference in
 * zswap_next_shrink.
 *
 * shrink_worker() must handle the case where this function releases
 * the reference of memcg being shrunk.
 */
/*
 * memcg 下线前必须调用本函数。全局 shrink_worker 可能通过 zswap_next_shrink 持有该 memcg 的
 * iterator 引用，因此这里在游标锁下跳过离线组；worker 也必须容忍游标已被本路径推进。
 */
/*
 * zswap_memcg_offline_cleanup() - 在 memcg 下线时把全局收缩游标移到下一个在线组。
 * @memcg: 正在下线且可能正被 zswap_next_shrink 指向的组。
 * 返回：无；zswap_shrink_lock 串行化游标，mem_cgroup_iter() 同时交接 iterator 引用；不睡眠。
 */
void zswap_memcg_offline_cleanup(struct mem_cgroup *memcg)
{
	/* lock out zswap shrinker walking memcg tree */
	/* 阻止 shrink worker 同时读取或推进全局 memcg 游标。 */
	spin_lock(&zswap_shrink_lock);
	if (zswap_next_shrink == memcg) {
		do {
			zswap_next_shrink = mem_cgroup_iter(NULL, zswap_next_shrink, NULL);
		} while (zswap_next_shrink && !mem_cgroup_online(zswap_next_shrink));
	}
	spin_unlock(&zswap_shrink_lock);
}

/*********************************
* zswap entry functions
**********************************/
/* zswap entry functions：slab 元数据及其 zsmalloc、池引用、objcg 计费的统一释放。 */
static struct kmem_cache *zswap_entry_cache;

/*
 * zswap_entry_cache_alloc() - 从指定 NUMA 节点分配条目元数据。
 * @gfp: slab 分配约束；@nid: 首选节点。
 * 返回：调用者独占且尚未初始化的 entry，失败为 NULL；是否睡眠由 gfp 决定。
 */
static struct zswap_entry *zswap_entry_cache_alloc(gfp_t gfp, int nid)
{
	struct zswap_entry *entry;
	entry = kmem_cache_alloc_node(zswap_entry_cache, gfp, nid);
	if (!entry)
		return NULL;
	return entry;
}

/*
 * zswap_entry_cache_free() - 把不再可达的条目元数据归还 slab。
 * @entry: 调用者独占、已解除 xarray/LRU 和外部资源关联的条目；返回无。
 */
static void zswap_entry_cache_free(struct zswap_entry *entry)
{
	kmem_cache_free(zswap_entry_cache, entry);
}

/*
 * Carries out the common pattern of freeing an entry's zsmalloc allocation,
 * freeing the entry itself, and decrementing the number of stored pages.
 */
/* 统一执行“摘 LRU、释放压缩对象、池引用与 objcg 计费、释放元数据、减全局计数”的销毁序列。 */
/*
 * zswap_entry_free() - 最终释放一个已从 xarray 删除的 zswap 条目及全部从属资源。
 * @entry: 调用者独占的已发布条目；仍须在 LRU 上并持 pool/objcg 引用。
 * 返回：无；释放后 entry 不可再访问。内部 list_lru 操作可加锁，但本函数不主动睡眠。
 */
static void zswap_entry_free(struct zswap_entry *entry)
{
	zswap_lru_del(&zswap_list_lru, entry);
	zs_free(entry->pool->zs_pool, entry->handle);
	zswap_pool_put(entry->pool);
	/* objcg 引用与压缩字节计费在 entry 发布成功时一起取得，此处一起撤销。 */
	if (entry->objcg) {
		obj_cgroup_uncharge_zswap(entry->objcg, entry->length);
		obj_cgroup_put(entry->objcg);
	}
	if (entry->length == PAGE_SIZE)
		atomic_long_dec(&zswap_stored_incompressible_pages);
	zswap_entry_cache_free(entry);
	atomic_long_dec(&zswap_stored_pages);
}

/*********************************
* compressed storage functions
**********************************/
/* compressed storage：每 CPU crypto 上下文负责压缩/解压，实际字节存入所属 zsmalloc 池。 */
/*
 * zswap_cpu_comp_prepare() - 为某个池在指定 CPU/NUMA 节点建立压缩上下文。
 * @cpu: 目标 CPU；@node: 嵌入 zswap_pool 的 hotplug instance 节点。
 * 返回：成功或已初始化返回 0；分配失败返回 errno，并清理该 CPU 已建成员。
 * 上下文：CPU hotplug 串行化调用，可睡眠；online-offline-online 重入通过 acomp 非空幂等处理。
 */
static int zswap_cpu_comp_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zswap_pool *pool = hlist_entry(node, struct zswap_pool, node);
	struct crypto_acomp_ctx *acomp_ctx = per_cpu_ptr(pool->acomp_ctx, cpu);
	int ret = -ENOMEM;

	/*
	 * To handle cases where the CPU goes through online-offline-online
	 * transitions, we return if the acomp_ctx has already been initialized.
	 */
	/* CPU 反复上线时沿用已初始化上下文，避免重复分配并覆盖仍有效的资源。 */
	if (acomp_ctx->acomp) {
		WARN_ON_ONCE(IS_ERR(acomp_ctx->acomp));
		return 0;
	}

	acomp_ctx->buffer = kmalloc_node(PAGE_SIZE, GFP_KERNEL, cpu_to_node(cpu));
	if (!acomp_ctx->buffer)
		return ret;

	/*
	 * In case of an error, crypto_alloc_acomp_node() returns an
	 * error pointer, never NULL.
	 */
	/* crypto_alloc_acomp_node() 失败用 ERR_PTR 编码，清理函数会识别而不误释放。 */
	acomp_ctx->acomp = crypto_alloc_acomp_node(pool->tfm_name, 0, 0, cpu_to_node(cpu));
	if (IS_ERR(acomp_ctx->acomp)) {
		pr_err("could not alloc crypto acomp %s : %pe\n",
				pool->tfm_name, acomp_ctx->acomp);
		ret = PTR_ERR(acomp_ctx->acomp);
		goto fail;
	}

	/* acomp_request_alloc() returns NULL in case of an error. */
	/* 请求对象分配失败返回 NULL，与算法实例的 ERR_PTR 约定不同。 */
	acomp_ctx->req = acomp_request_alloc(acomp_ctx->acomp);
	if (!acomp_ctx->req) {
		pr_err("could not alloc crypto acomp_request %s\n",
		       pool->tfm_name);
		goto fail;
	}

	crypto_init_wait(&acomp_ctx->wait);

	/*
	 * if the backend of acomp is async zip, crypto_req_done() will wakeup
	 * crypto_wait_req(); if the backend of acomp is scomp, the callback
	 * won't be called, crypto_wait_req() will return without blocking.
	 */
	/* 异步后端由回调唤醒等待者；同步 scomp 后端不会回调，wait helper 会直接返回完成结果。 */
	acomp_request_set_callback(acomp_ctx->req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				   crypto_req_done, &acomp_ctx->wait);

	mutex_init(&acomp_ctx->mutex);
	return 0;

fail:
	acomp_ctx_free(acomp_ctx);
	return ret;
}

/*
 * zswap_compress() - 压缩一个换出页并在 pool 中为 entry 建立 zsmalloc 对象。
 * @page: 已由 swapout 路径稳定的一页输入；@entry: 未发布元数据；@pool: 已持引用的目标池。
 * 返回：成功保存数据并填写 handle/length 时为 true；压缩或分配失败为 false，entry 仍未拥有对象。
 * 上下文：以当前 CPU 的 mutex 串行化共享请求/缓冲区，可能等待 crypto 完成，但 zsmalloc 使用 NOWAIT。
 */
static bool zswap_compress(struct page *page, struct zswap_entry *entry,
			   struct zswap_pool *pool)
{
	struct crypto_acomp_ctx *acomp_ctx;
	struct scatterlist input, output;
	int comp_ret = 0, alloc_ret = 0;
	unsigned int dlen = PAGE_SIZE;
	unsigned long handle;
	gfp_t gfp;
	/* dst 通常指向 per-CPU 压缩缓冲，原样保存时改指源页临时映射。 */
	u8 *dst;
	bool mapped = false;

	/* raw_cpu_ptr 后立即用该 CPU 上下文的 mutex 固定共享 buffer/request 的独占使用。 */
	acomp_ctx = raw_cpu_ptr(pool->acomp_ctx);
	mutex_lock(&acomp_ctx->mutex);

	dst = acomp_ctx->buffer;
	sg_init_table(&input, 1);
	sg_set_page(&input, page, PAGE_SIZE, 0);

	sg_init_one(&output, dst, PAGE_SIZE);
	acomp_request_set_params(acomp_ctx->req, &input, &output, PAGE_SIZE, dlen);

	/*
	 * it maybe looks a little bit silly that we send an asynchronous request,
	 * then wait for its completion synchronously. This makes the process look
	 * synchronous in fact.
	 * Theoretically, acomp supports users send multiple acomp requests in one
	 * acomp instance, then get those requests done simultaneously. but in this
	 * case, zswap actually does store and load page by page, there is no
	 * existing method to send the second page before the first page is done
	 * in one thread doing zswap.
	 * but in different threads running on different cpu, we have different
	 * acomp instance, so multiple threads can do (de)compression in parallel.
	 */
	/*
	 * 接口虽提交异步请求，本热路径每个线程一次只处理一页，因而立即同步等待；并行度来自不同 CPU
	 * 各自独立的 acomp instance，而不是同一请求队列中批量流水多页。
	 */
	comp_ret = crypto_wait_req(crypto_acomp_compress(acomp_ctx->req), &acomp_ctx->wait);
	dlen = acomp_ctx->req->dlen;

	/*
	 * If a page cannot be compressed into a size smaller than PAGE_SIZE,
	 * save the content as is without a compression, to keep the LRU order
	 * of writebacks.  If writeback is disabled, reject the page since it
	 * only adds metadata overhead.  swap_writeout() will put the page back
	 * to the active LRU list in the case.
	 */
	/*
	 * 压缩失败或结果不小于一页时，若 memcg 允许回写则原样缓存，以保留统一 LRU/回写顺序；
	 * 禁止回写时保存原页只会增加元数据且无法回收，因此拒绝，让 swap_writeout() 恢复 active LRU。
	 */
	if (comp_ret || !dlen || dlen >= PAGE_SIZE) {
		rcu_read_lock();
		/* RCU 稳定 folio 当前 memcg，以读取该组是否允许日后把原样页回写出去。 */
		if (!mem_cgroup_zswap_writeback_enabled(
					folio_memcg(page_folio(page)))) {
			rcu_read_unlock();
			comp_ret = comp_ret ? comp_ret : -EINVAL;
			goto unlock;
		}
		rcu_read_unlock();
		comp_ret = 0;
		dlen = PAGE_SIZE;
		/* 原样保存时临时映射源页，并在统一 unlock 出口撤销映射。 */
		dst = kmap_local_page(page);
		mapped = true;
	}

	gfp = GFP_NOWAIT | __GFP_NORETRY | __GFP_HIGHMEM | __GFP_MOVABLE;
	/* 换出热路径不做直接回收重试；zsmalloc 对象可移动并尽量落在源页节点。 */
	handle = zs_malloc(pool->zs_pool, dlen, gfp, page_to_nid(page));
	if (IS_ERR_VALUE(handle)) {
		alloc_ret = PTR_ERR((void *)handle);
		goto unlock;
	}

	zs_obj_write(pool->zs_pool, handle, dst, dlen);
	/* 数据复制成功后 entry 才拥有 handle，失败出口因此不会误释放未建立的对象。 */
	entry->handle = handle;
	entry->length = dlen;

unlock:
	/* 统一恢复临时映射并按首要失败来源记账，最后释放 per-CPU 上下文。 */
	if (mapped)
		kunmap_local(dst);
	/* ENOSPC 归类为压缩收益不足，其余 crypto 与 allocator 错误分别计数。 */
	if (comp_ret == -ENOSPC || alloc_ret == -ENOSPC)
		zswap_reject_compress_poor++;
	else if (comp_ret)
		zswap_reject_compress_fail++;
	else if (alloc_ret)
		zswap_reject_alloc_fail++;

	mutex_unlock(&acomp_ctx->mutex);
	return comp_ret == 0 && alloc_ret == 0;
}

/*
 * zswap_decompress() - 把 entry 数据恢复到调用者提供的单页 folio。
 * @entry: xarray/锁协议稳定且持 pool 引用的条目；@folio: 已锁定的目标单页 folio。
 * 返回：恰好恢复 PAGE_SIZE 字节时为 true；crypto 或长度异常为 false 并记录告警。
 * 上下文：串行化当前 CPU crypto 上下文并可能等待异步完成；不消费 entry，也不解锁 folio。
 */
static bool zswap_decompress(struct zswap_entry *entry, struct folio *folio)
{
	struct zswap_pool *pool = entry->pool;
	struct scatterlist input[2]; /* zsmalloc returns an SG list 1-2 entries */
	/* zsmalloc 对象最多跨两个物理段，因此两个 scatterlist 元素足够。 */
	struct scatterlist output;
	struct crypto_acomp_ctx *acomp_ctx;
	int ret = 0, dlen;

	acomp_ctx = raw_cpu_ptr(pool->acomp_ctx);
	mutex_lock(&acomp_ctx->mutex);
	/* begin/end 把可能跨页的 zsmalloc 对象稳定为 scatterlist 读视图。 */
	zs_obj_read_sg_begin(pool->zs_pool, entry->handle, input, entry->length);

	/* zswap entries of length PAGE_SIZE are not compressed. */
	/* length 等于整页是原样保存标记，直接复制而不经过解压算法。 */
	if (entry->length == PAGE_SIZE) {
		void *dst;

		WARN_ON_ONCE(input->length != PAGE_SIZE);

		dst = kmap_local_folio(folio, 0);
		memcpy_from_sglist(dst, input, 0, PAGE_SIZE);
		/* 高端映射只覆盖复制阶段；flush 让后续 CPU/设备观察到新页内容。 */
		dlen = PAGE_SIZE;
		kunmap_local(dst);
		flush_dcache_folio(folio);
	} else {
		/* 压缩对象作为输入，目标 folio 的完整一页作为唯一输出段。 */
		sg_init_table(&output, 1);
		sg_set_folio(&output, folio, PAGE_SIZE, 0);
		acomp_request_set_params(acomp_ctx->req, input, &output,
					 entry->length, PAGE_SIZE);
		ret = crypto_acomp_decompress(acomp_ctx->req);
		ret = crypto_wait_req(ret, &acomp_ctx->wait);
		/* 即使算法返回成功，也必须验证实际输出长度恰为一页。 */
		dlen = acomp_ctx->req->dlen;
	}

	zs_obj_read_sg_end(pool->zs_pool, entry->handle);
	mutex_unlock(&acomp_ctx->mutex);

	/* 错误日志携带 slot、算法和长度转换，便于定位数据或后端异常。 */
	if (!ret && dlen == PAGE_SIZE)
		return true;

	zswap_decompress_fail++;
	pr_alert_ratelimited("Decompression error from zswap (%d:%lu %s %u->%d)\n",
						swp_type(entry->swpentry),
						swp_offset(entry->swpentry),
						entry->pool->tfm_name,
						entry->length, dlen);
	return false;
}

/*********************************
* writeback code
**********************************/
/* writeback code：把 RAM 中的压缩副本恢复进 swap cache，再续接原本被截获的块设备写回。 */
/*
 * Attempts to free an entry by adding a folio to the swap cache,
 * decompressing the entry data into the folio, and issuing a
 * bio write to write the folio back to the swap device.
 *
 * This can be thought of as a "resumed writeback" of the folio
 * to the swap device.  We are basically resuming the same swap
 * writeback path that was intercepted with the zswap_store()
 * in the first place.  After the folio has been decompressed into
 * the swap cache, the compressed version stored by zswap can be
 * freed.
 */
/*
 * 回写先为同一 swap slot 建立并锁住 swap-cache folio，解压后删除 zswap 条目并提交 bio；
 * 这等价于从 zswap_store() 截停的位置恢复换出，成功后 RAM 压缩副本即可释放。
 */
/*
 * zswap_writeback_entry() - 尝试把一个冷 zswap 条目回写到底层 swap 设备。
 * @entry: 从 LRU 观察到但未持独立引用的候选；@swpentry: 在解锁前复制到栈的稳定 slot 值。
 * 返回：成功提交写回为 0；slot 失效/已缓存、分配或解压失败返回 errno。
 * 并发：swap-cache folio 锁稳定 slot 后以指针相等重新验证 entry；成功消费 entry，失败不再解引用它。
 */
static int zswap_writeback_entry(struct zswap_entry *entry,
				 swp_entry_t swpentry)
{
	struct xarray *tree;
	pgoff_t offset = swp_offset(swpentry);
	struct folio *folio;
	struct mempolicy *mpol;
	struct swap_info_struct *si;
	int ret = 0;

	/* try to allocate swap cache folio */
	/* 先 pin swap 设备并分配带锁 folio，避免 swapoff 或并发 swapin 复用该 slot。 */
	si = get_swap_device(swpentry);
	if (!si)
		return -EEXIST;

	mpol = get_task_policy(current);
	folio = swap_cache_alloc_folio(swpentry, GFP_KERNEL, BIT(0), NULL, mpol,
				       NO_INTERLEAVE_INDEX);
	put_swap_device(si);

	/*
	 * Swap cache allocation might fail due to OOM, or the entry
	 * may already be cached due to concurrent swapin or have been
	 * freed. If already cached, a concurrent swapin made the folio
	 * hot, so skip it. For the unlikely concurrent shrinker case,
	 * it will be unlinked and freed when invalidated anyway.
	 */
	/*
	 * 分配可能因 OOM、并发换入已建立缓存或 slot 已释放而失败；已缓存说明该页变热，应停止追赶，
	 * 并发 shrinker 的重复候选则会在其后续 invalidation 中自然摘除。
	 */
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	/*
	 * folio is locked, and the swapcache is now secured against
	 * concurrent swapping to and from the slot, and concurrent
	 * swapoff so we can safely dereference the zswap tree here.
	 * Verify that the swap entry hasn't been invalidated and recycled
	 * behind our backs, to avoid overwriting a new swap folio with
	 * old compressed data. Only when this is successful can the entry
	 * be dereferenced.
	 */
	/*
	 * 锁住的 swap-cache folio 排除了该 slot 的换入、换出与 swapoff；仍需检查 xarray 中还是原指针，
	 * 防止 LRU 解锁后旧 entry 被释放、同一 slot 又发布新版本而被旧压缩数据覆盖。
	 */
	tree = swap_zswap_tree(swpentry);
	if (entry != xa_load(tree, offset)) {
		ret = -ENOMEM;
		goto out;
	}

	if (!zswap_decompress(entry, folio)) {
		ret = -EIO;
		goto out;
	}

	/* 解压成功后先从索引删除，确保此 slot 不再同时指向即将释放的压缩副本。 */
	xa_erase(tree, offset);

	count_vm_event(ZSWPWB);
	if (entry->objcg)
		count_objcg_events(entry->objcg, ZSWPWB, 1);

	zswap_entry_free(entry);

	/* folio is up to date */
	/* 解压完成后目标 folio 含有效整页数据，可向读者声明 uptodate。 */
	folio_mark_uptodate(folio);

	/* move it to the tail of the inactive list after end_writeback */
	/* reclaim 标记使写回结束后 folio 落到 inactive 尾部，维持冷页回收方向。 */
	folio_set_reclaim(folio);

	/* start writeback */
	/* 提交 swap 写回后由 writeback 完成路径负责解锁 folio。 */
	__swap_writepage(folio, NULL);

out:
	if (ret) {
		swap_cache_del_folio(folio);
		folio_unlock(folio);
	}
	folio_put(folio);
	return ret;
}

/*********************************
* shrinker functions
**********************************/
/* shrinker functions：结合二次机会、近期磁盘换入和压缩比，决定回写哪些缓存页。 */
/*
 * The dynamic shrinker is modulated by the following factors:
 *
 * 1. Each zswap entry has a referenced bit, which the shrinker unsets (giving
 *    the entry a second chance) before rotating it in the LRU list. If the
 *    entry is considered again by the shrinker, with its referenced bit unset,
 *    it is written back. The writeback rate as a result is dynamically
 *    adjusted by the pool activities - if the pool is dominated by new entries
 *    (i.e lots of recent zswapouts), these entries will be protected and
 *    the writeback rate will slow down. On the other hand, if the pool has a
 *    lot of stagnant entries, these entries will be reclaimed immediately,
 *    effectively increasing the writeback rate.
 *
 * 2. Swapins counter: If we observe swapins, it is a sign that we are
 *    overshrinking and should slow down. We maintain a swapins counter, which
 *    is consumed and subtract from the number of eligible objects on the LRU
 *    in zswap_shrinker_count().
 *
 * 3. Compression ratio. The better the workload compresses, the less gains we
 *    can expect from writeback. We scale down the number of objects available
 *    for reclaim by this ratio.
 */
/*
 * 动态收缩由三类反馈共同调节：referenced 位先给新条目一次 LRU 二次机会；近期磁盘 swapin
 * 表明回收已触及热区，会抵扣可回收量；压缩率越高，每次释放条目带来的物理页收益越小，
 * 因而按 backing/stored 比例降低回写数量，避免用大量 I/O 换取很少内存。
 */
/*
 * shrink_memcg_cb() - list_lru 单条目回调，为候选提供二次机会或尝试回写。
 * @item: 嵌入 entry 的 LRU 节点；@l: 当前持锁的桶；@arg: 可选的“遇到 swapcache 热页”输出布尔值。
 * 返回：ROTATE 表示二次机会，REMOVED_RETRY/RETRY 表示已解锁需重试遍历，STOP 表示动态回收触热区。
 * 并发：回调主动释放 l->lock；解锁后只能用栈上 swpentry，直至 writeback 在 xarray 重新验证 entry。
 */
static enum lru_status shrink_memcg_cb(struct list_head *item, struct list_lru_one *l,
				       void *arg)
{
	struct zswap_entry *entry = container_of(item, struct zswap_entry, lru);
	bool *encountered_page_in_swapcache = (bool *)arg;
	swp_entry_t swpentry;
	enum lru_status ret = LRU_REMOVED_RETRY;
	int writeback_result;

	/*
	 * Second chance algorithm: if the entry has its referenced bit set, give it
	 * a second chance. Only clear the referenced bit and rotate it in the
	 * zswap's LRU list.
	 */
	/* 第一次扫描只清 referenced 并移到尾部，让新换出或近期受保护的条目获得一次完整轮转。 */
	if (entry->referenced) {
		entry->referenced = false;
		return LRU_ROTATE;
	}

	/*
	 * As soon as we drop the LRU lock, the entry can be freed by
	 * a concurrent invalidation. This means the following:
	 *
	 * 1. We extract the swp_entry_t to the stack, allowing
	 *    zswap_writeback_entry() to pin the swap entry and
	 *    then validate the zswap entry against that swap entry's
	 *    tree using pointer value comparison. Only when that
	 *    is successful can the entry be dereferenced.
	 *
	 * 2. Usually, objects are taken off the LRU for reclaim. In
	 *    this case this isn't possible, because if reclaim fails
	 *    for whatever reason, we have no means of knowing if the
	 *    entry is alive to put it back on the LRU.
	 *
	 *    So rotate it before dropping the lock. If the entry is
	 *    written back or invalidated, the free path will unlink
	 *    it. For failures, rotation is the right thing as well.
	 *
	 *    Temporary failures, where the same entry should be tried
	 *    again immediately, almost never happen for this shrinker.
	 *    We don't do any trylocking; -ENOMEM comes closest,
	 *    but that's extremely rare and doesn't happen spuriously
	 *    either. Don't bother distinguishing this case.
	 */
	/*
	 * 放开 LRU 锁后 invalidation 可立即释放 entry，因此先复制 swpentry，回写必须 pin slot 并按指针
	 * 复核 xarray。候选不能先摘链：失败时已无法安全判断 entry 是否仍活着而重新挂回；先旋到尾部，
	 * 成功/并发失效由 free 路径摘除，失败也自然获得退避。此路径几乎没有值得立即重试的瞬时失败。
	 */
	list_move_tail(item, &l->list);

	/*
	 * Once the lru lock is dropped, the entry might get freed. The
	 * swpentry is copied to the stack, and entry isn't deref'd again
	 * until the entry is verified to still be alive in the tree.
	 */
	/* entry 解锁后可能消失；这里只复制值类型 slot，验证成功前不再读取 entry 字段。 */
	swpentry = entry->swpentry;

	/*
	 * It's safe to drop the lock here because we return either
	 * LRU_REMOVED_RETRY, LRU_RETRY or LRU_STOP.
	 */
	/* 这些返回值都通知 list_lru 核心本回调已放锁，避免框架按仍持锁路径继续。 */
	spin_unlock(&l->lock);

	writeback_result = zswap_writeback_entry(entry, swpentry);

	if (writeback_result) {
		zswap_reject_reclaim_fail++;
		ret = LRU_RETRY;

		/*
		 * Encountering a page already in swap cache is a sign that we are shrinking
		 * into the warmer region. We should terminate shrinking (if we're in the dynamic
		 * shrinker context).
		 */
		/* 已在 swap cache 的页说明扫描进入热区；动态 shrinker 应停止，本地强制回收则可忽略该信号。 */
		if (writeback_result == -EEXIST && encountered_page_in_swapcache) {
			ret = LRU_STOP;
			*encountered_page_in_swapcache = true;
		}
	} else {
		zswap_written_back_pages++;
	}

	return ret;
}

/*
 * zswap_shrinker_scan() - shrinker 扫描入口，按 sc 指定 memcg/node 回写候选。
 * @shrinker: 已注册的 zswap shrinker；@sc: 回收目标、预算和 GFP 上下文。
 * 返回：list_lru 报告的回收量；禁用、不可回写、无进展或遇到热页时返回 SHRINK_STOP。
 * 副作用：更新 sc->nr_scanned，回调可睡眠并提交 swap I/O。
 */
static unsigned long zswap_shrinker_scan(struct shrinker *shrinker,
		struct shrink_control *sc)
{
	unsigned long shrink_ret;
	bool encountered_page_in_swapcache = false;

	/* 用户关闭 shrinker 或目标 memcg 禁止 writeback 时，不扫描也明确清零反馈。 */
	if (!zswap_shrinker_enabled ||
			!mem_cgroup_zswap_writeback_enabled(sc->memcg)) {
		sc->nr_scanned = 0;
		return SHRINK_STOP;
	}

	shrink_ret = list_lru_shrink_walk(&zswap_list_lru, sc, &shrink_memcg_cb,
		&encountered_page_in_swapcache);

	/* 命中已在 swapcache 的页意味着进入热区，即使前面有回收量也要求本轮停止。 */
	if (encountered_page_in_swapcache)
		return SHRINK_STOP;

	return shrink_ret ? shrink_ret : SHRINK_STOP;
}

/*
 * zswap_shrinker_count() - 估算当前 sc 范围内值得回写的 zswap 条目数。
 * @shrinker: zswap shrinker；@sc: memcg、nid 与允许的 I/O/FS 上下文。
 * 返回：经 swapin 反馈和压缩率缩放后的对象数；不能安全写回时返回 0。
 * 并发：统计是快照；以 cmpxchg 原子消费磁盘换入反馈，不要求与实际 LRU 数严格一致。
 */
static unsigned long zswap_shrinker_count(struct shrinker *shrinker,
		struct shrink_control *sc)
{
	struct mem_cgroup *memcg = sc->memcg;
	struct lruvec *lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(sc->nid));
	atomic_long_t *nr_disk_swapins =
		&lruvec->zswap_lruvec_state.nr_disk_swapins;
	unsigned long nr_backing, nr_stored, nr_freeable, nr_disk_swapins_cur,
		nr_remain;

	/* count 与 scan 使用相同开关，避免向 vmscan 宣称实际不能释放的对象。 */
	if (!zswap_shrinker_enabled || !mem_cgroup_zswap_writeback_enabled(memcg))
		return 0;

	/*
	 * The shrinker resumes swap writeback, which will enter block
	 * and may enter fs. XXX: Harmonize with vmscan.c __GFP_FS
	 * rules (may_enter_fs()), which apply on a per-folio basis.
	 */
	/* 回写会进入块层且可能进入文件系统；当前 reclaim 上下文不允许 IO/FS 时不能从这里发起。 */
	if (!gfp_has_io_fs(sc->gfp_mask))
		return 0;

	/*
	 * For memcg, use the cgroup-wide ZSWAP stats since we don't
	 * have them per-node and thus per-lruvec. Careful if memcg is
	 * runtime-disabled: we can get sc->memcg == NULL, which is ok
	 * for the lruvec, but not for memcg_page_state().
	 *
	 * Without memcg, use the zswap pool-wide metrics.
	 */
	/*
	 * memcg 模式只有 cgroup 总量、没有逐 node 指标，因此所有 lruvec 共用组级统计；运行时关闭 memcg
	 * 时 sc->memcg 可为 NULL，只能改用全局 zswap 占用与条目数，不能调用 memcg_page_state(NULL)。
	 */
	if (!mem_cgroup_disabled()) {
		mem_cgroup_flush_stats(memcg);
		nr_backing = memcg_page_state(memcg, MEMCG_ZSWAP_B) >> PAGE_SHIFT;
		nr_stored = memcg_page_state(memcg, MEMCG_ZSWAPPED);
	} else {
		nr_backing = zswap_total_pages();
		nr_stored = atomic_long_read(&zswap_stored_pages);
	}

	/* 没有已存条目时既不能计算压缩比，也没有回收意义。 */
	if (!nr_stored)
		return 0;

	nr_freeable = list_lru_shrink_count(&zswap_list_lru, sc);
	if (!nr_freeable)
		return 0;

	/*
	 * Subtract from the lru size the number of pages that are recently swapped
	 * in from disk. The idea is that had we protect the zswap's LRU by this
	 * amount of pages, these disk swapins would not have happened.
	 */
	/* 把近期真实磁盘换入视作“本应留在 zswap 的热页”，从本轮候选数中原子抵扣并消费反馈。 */
	nr_disk_swapins_cur = atomic_long_read(nr_disk_swapins);
	do {
		if (nr_freeable >= nr_disk_swapins_cur)
			nr_remain = 0;
		else
			nr_remain = nr_disk_swapins_cur - nr_freeable;
		/* 并发 swapin 更新计数时 cmpxchg 会刷新旧值并重新计算本轮可消费量。 */
	} while (!atomic_long_try_cmpxchg(
		nr_disk_swapins, &nr_disk_swapins_cur, nr_remain));

	nr_freeable -= nr_disk_swapins_cur - nr_remain;
	if (!nr_freeable)
		return 0;

	/*
	 * Scale the number of freeable pages by the memory saving factor.
	 * This ensures that the better zswap compresses memory, the fewer
	 * pages we will evict to swap (as it will otherwise incur IO for
	 * relatively small memory saving).
	 */
	/* backing/stored 近似每条目实际占用比例；压缩越好，该比例越小，允许回写的数量也越少。 */
	return mult_frac(nr_freeable, nr_backing, nr_stored);
}

/*
 * zswap_alloc_shrinker() - 分配并配置 NUMA/memcg 感知的 zswap shrinker。
 * 返回：尚未注册且由调用者拥有的 shrinker，分配失败为 NULL；成功对象需注册或释放。
 * 上下文：初始化路径调用，可睡眠。
 */
static struct shrinker *zswap_alloc_shrinker(void)
{
	struct shrinker *shrinker;

	shrinker =
		shrinker_alloc(SHRINKER_NUMA_AWARE | SHRINKER_MEMCG_AWARE, "mm-zswap");
	if (!shrinker)
		return NULL;

	/* batch=0 交由框架选择批量，DEFAULT_SEEKS 表示普通回收成本。 */
	shrinker->scan_objects = zswap_shrinker_scan;
	shrinker->count_objects = zswap_shrinker_count;
	shrinker->batch = 0;
	shrinker->seeks = DEFAULT_SEEKS;
	return shrinker;
}

/*
 * shrink_memcg() - 为指定 memcg 在每个普通内存节点最多尝试一个 zswap 候选。
 * @memcg: 目标组，可为根组语义的 NULL；调用者负责其生命周期。
 * 返回：成功回写至少一页为 0；无候选/不可回写为 -ENOENT，扫描过但未成功为 -EAGAIN。
 * 上下文：可睡眠和发起 I/O；离线 memcg 必须跳过，因为其 LRU 已重挂到父组。
 */
static int shrink_memcg(struct mem_cgroup *memcg)
{
	int nid, shrunk = 0, scanned = 0;

	if (!mem_cgroup_zswap_writeback_enabled(memcg))
		return -ENOENT;

	/*
	 * Skip zombies because their LRUs are reparented and we would be
	 * reclaiming from the parent instead of the dead memcg.
	 */
	/* zombie 的条目已经 reparent；继续按旧组扫描会误把父组内容算作它的回收。 */
	if (memcg && !mem_cgroup_online(memcg))
		return -ENOENT;

	for_each_node_state(nid, N_NORMAL_MEMORY) {
		unsigned long nr_to_walk = 1;

		/* 每个节点只给一个候选，跨节点小步轮询避免单个 memcg 一次回写过猛。 */
		shrunk += list_lru_walk_one(&zswap_list_lru, nid, memcg,
					    &shrink_memcg_cb, NULL, &nr_to_walk);
		scanned += 1 - nr_to_walk;
	}

	if (!scanned)
		return -ENOENT;

	return shrunk ? 0 : -EAGAIN;
}

/*
 * shrink_worker() - 池满后的全局后台回写器，轮转在线 memcg 直到降到接收低水位。
 * @w: zswap_shrink_work，参数本身不携带额外状态。
 * 返回：无；可睡眠/调度并发起 I/O，连续找不到候选或回写失败达到上限时提前停止。
 * 并发：zswap_shrink_lock 保护持 iterator 引用的全局游标，并与 memcg 下线清理协调。
 */
static void shrink_worker(struct work_struct *w)
{
	struct mem_cgroup *memcg;
	int ret, failures = 0, attempts = 0;
	unsigned long thr;

	/* Reclaim down to the accept threshold */
	/* 高水位触发工作，目标是回收到低水位，恢复 zswap 接收能力。 */
	thr = zswap_accept_thr_pages();

	/*
	 * Global reclaim will select cgroup in a round-robin fashion from all
	 * online memcgs, but memcgs that have no pages in zswap and
	 * writeback-disabled memcgs (memory.zswap.writeback=0) are not
	 * candidates for shrinking.
	 *
	 * Shrinking will be aborted if we encounter the following
	 * MAX_RECLAIM_RETRIES times:
	 * - No writeback-candidate memcgs found in a memcg tree walk.
	 * - Shrinking a writeback-candidate memcg failed.
	 *
	 * We save iteration cursor memcg into zswap_next_shrink,
	 * which can be modified by the offline memcg cleaner
	 * zswap_memcg_offline_cleanup().
	 *
	 * Since the offline cleaner is called only once, we cannot leave an
	 * offline memcg reference in zswap_next_shrink.
	 * We can rely on the cleaner only if we get online memcg under lock.
	 *
	 * If we get an offline memcg, we cannot determine if the cleaner has
	 * already been called or will be called later. We must put back the
	 * reference before returning from this function. Otherwise, the
	 * offline memcg left in zswap_next_shrink will hold the reference
	 * until the next run of shrink_worker().
	 */
	/*
	 * worker 以 round-robin 遍历所有在线 memcg，跳过无条目或禁止 writeback 的组；一次树遍历未找到
	 * 候选或候选回写失败累计到上限便停止。zswap_next_shrink 保存带 iterator 引用的游标，并可能被
	 * offline cleanup 推进。只有在锁内成功取得 online 额外引用，才能确信 cleanup 会处理原游标引用；
	 * 若拿到离线组则必须由本函数继续迭代交还，不能把孤立引用留到下一次 worker。
	 */
	do {
		/*
		 * Start shrinking from the next memcg after zswap_next_shrink.
		 * When the offline cleaner has already advanced the cursor,
		 * advancing the cursor here overlooks one memcg, but this
		 * should be negligibly rare.
		 *
		 * If we get an online memcg, keep the extra reference in case
		 * the original one obtained by mem_cgroup_iter() is dropped by
		 * zswap_memcg_offline_cleanup() while we are shrinking the
		 * memcg.
		 */
		/*
		 * 每轮从游标之后开始；若 cleanup 刚推进过，极少数情况下会多跳一个组。对在线组另取引用，
		 * 保证 cleanup 在扫描期间即使交还 iterator 原引用，shrink_memcg() 仍可安全访问它。
		 */
		spin_lock(&zswap_shrink_lock);
		do {
			memcg = mem_cgroup_iter(NULL, zswap_next_shrink, NULL);
			zswap_next_shrink = memcg;
		} while (memcg && !mem_cgroup_tryget_online(memcg));
		spin_unlock(&zswap_shrink_lock);

		if (!memcg) {
			/*
			 * Continue shrinking without incrementing failures if
			 * we found candidate memcgs in the last tree walk.
			 */
			/* 上一轮树遍历已有候选尝试时，走到末尾只是正常换轮，不计为一次“全树无候选”失败。 */
			if (!attempts && ++failures == MAX_RECLAIM_RETRIES)
				break;

			attempts = 0;
			goto resched;
		}

		ret = shrink_memcg(memcg);
		/* drop the extra reference */
		/* 与 tryget_online 取得的扫描期额外引用配对；游标引用仍由 iterator 管理。 */
		mem_cgroup_put(memcg);

		/*
		 * There are no writeback-candidate pages in the memcg.
		 * This is not an issue as long as we can find another memcg
		 * with pages in zswap. Skip this without incrementing attempts
		 * and failures.
		 */
		/* 当前组没有可写回页不代表全局失败，继续寻找其它组且不消耗重试预算。 */
		if (ret == -ENOENT)
			continue;
		++attempts;

		if (ret && ++failures == MAX_RECLAIM_RETRIES)
			break;
resched:
		cond_resched();
	} while (zswap_total_pages() > thr);
}

/*********************************
* main API
**********************************/
/* main API：swap 子系统通过 store/load/invalidate 维护每个 slot 在 zswap 中的唯一缓存副本。 */

/*
 * zswap_store_page() - 压缩并发布 folio 中的一个 base page 到其 swap slot。
 * @page: 所属 folio 已锁且在 swapcache；@objcg: 可空的借用计费对象；@pool: 调用者持引用的当前池。
 * 返回：完成 xarray 发布、计费和 LRU 挂接为 true；失败为 false 并回滚本页新分配资源。
 * 并发：folio 锁排除同 slot store/invalidate；发布后在入 LRU 前不会被 writeback 观察。
 */
static bool zswap_store_page(struct page *page,
			     struct obj_cgroup *objcg,
			     struct zswap_pool *pool)
{
	swp_entry_t page_swpentry = page_swap_entry(page);
	struct zswap_entry *entry, *old;

	/* allocate entry */
	/* 元数据优先分配在源页节点，后续 entry_to_nid() 也以该位置选择 LRU 桶。 */
	entry = zswap_entry_cache_alloc(GFP_KERNEL, page_to_nid(page));
	if (!entry) {
		zswap_reject_kmemcache_fail++;
		return false;
	}

	/* compress 成功后 entry 拥有 handle；xarray 发布失败需走 store_failed 额外释放它。 */
	if (!zswap_compress(page, entry, pool))
		goto compress_failed;

	old = xa_store(swap_zswap_tree(page_swpentry),
		       swp_offset(page_swpentry),
		       entry, GFP_KERNEL);
	/* xa_store 返回被替换的旧 entry；ERR_PTR 只可能表示为 xarray 节点扩容失败。 */
	if (xa_is_err(old)) {
		int err = xa_err(old);

		WARN_ONCE(err != -ENOMEM, "unexpected xarray error: %d\n", err);
		zswap_reject_alloc_fail++;
		goto store_failed;
	}

	/*
	 * We may have had an existing entry that became stale when
	 * the folio was redirtied and now the new version is being
	 * swapped out. Get rid of the old.
	 */
	/* folio 被重新置脏再换出时同一 slot 可能残留旧版本；新值原子替换后立即释放旧条目。 */
	if (old)
		zswap_entry_free(old);

	/*
	 * The entry is successfully compressed and stored in the tree, there is
	 * no further possibility of failure. Grab refs to the pool and objcg,
	 * charge zswap memory, and increment zswap_stored_pages.
	 * The opposite actions will be performed by zswap_entry_free()
	 * when the entry is removed from the tree.
	 */
	/*
	 * xarray 存储成功后不再有可失败步骤：此时补齐 pool/objcg 引用、压缩字节计费与全局计数；
	 * zswap_entry_free() 在删除条目时严格执行相反操作。
	 */
	zswap_pool_get(pool);
	if (objcg) {
		obj_cgroup_get(objcg);
		obj_cgroup_charge_zswap(objcg, entry->length);
	}
	atomic_long_inc(&zswap_stored_pages);
	if (entry->length == PAGE_SIZE)
		atomic_long_inc(&zswap_stored_incompressible_pages);

	/*
	 * We finish initializing the entry while it's already in xarray.
	 * This is safe because:
	 *
	 * 1. Concurrent stores and invalidations are excluded by folio lock.
	 *
	 * 2. Writeback is excluded by the entry not being on the LRU yet.
	 *    The publishing order matters to prevent writeback from seeing
	 *    an incoherent entry.
	 */
	/*
	 * entry 先进入 xarray 再补齐字段仍安全：folio 锁排除了并发 store/invalidate，而未加入 LRU
	 * 又排除了 writeback。必须先初始化全部可见字段、最后入 LRU，防止回写读取半初始化对象。
	 */
	entry->pool = pool;
	entry->swpentry = page_swpentry;
	entry->objcg = objcg;
	entry->referenced = true;
	/* length 对已发布条目应非零；挂 LRU 是 writeback 获得可见性的最后一步。 */
	if (entry->length) {
		INIT_LIST_HEAD(&entry->lru);
		zswap_lru_add(&zswap_list_lru, entry);
	}

	return true;

store_failed:
	/* xarray 未接管新 entry 时，先释放 compress 已建立的 zsmalloc handle。 */
	zs_free(pool->zs_pool, entry->handle);
compress_failed:
	zswap_entry_cache_free(entry);
	return false;
}

/*
 * zswap_store() - 尝试把一个已锁 swapcache folio 的全部 base page 缓存进 zswap。
 * @folio: 已锁且带连续 swap slots 的换出 folio，调用者保留所有权和锁。
 * 返回：所有页均成功发布为 true；任一步失败为 false，并清除整个 folio 范围内的新旧 zswap 条目。
 * 上下文：可睡眠；受 memcg 限额、全局水位和池可用性约束，池满失败会排队异步 shrink worker。
 */
bool zswap_store(struct folio *folio)
{
	long nr_pages = folio_nr_pages(folio);
	swp_entry_t swp = folio->swap;
	struct obj_cgroup *objcg = NULL;
	struct mem_cgroup *memcg = NULL;
	struct zswap_pool *pool;
	bool ret = false;
	/* index 同时驱动逐页发布和失败后的完整范围清理。 */
	long index;

	VM_WARN_ON_ONCE(!folio_test_locked(folio));
	VM_WARN_ON_ONCE(!folio_test_swapcache(folio));

	/* 关闭只阻止新存储，仍会在统一 check_old 路径清掉该 slot 的历史副本。 */
	if (!zswap_enabled)
		goto check_old;

	objcg = get_obj_cgroup_from_folio(folio);
	/* objcg 超过自身 zswap 限额时先同步尝试回写该 memcg，一次失败即让本 folio 走磁盘。 */
	if (objcg && !obj_cgroup_may_zswap(objcg)) {
		memcg = get_mem_cgroup_from_objcg(objcg);
		if (shrink_memcg(memcg)) {
			mem_cgroup_put(memcg);
			goto put_objcg;
		}
		mem_cgroup_put(memcg);
	}

	if (zswap_check_limits())
		goto put_objcg;

	/* current_get 的引用把选定池固定到整个 folio 的所有 base page 存储结束。 */
	pool = zswap_pool_current_get();
	if (!pool)
		goto put_objcg;

	if (objcg) {
		memcg = get_mem_cgroup_from_objcg(objcg);
		/* 发布首个条目前预建 memcg 的 list_lru 桶，避免入 LRU 阶段出现分配失败。 */
		if (memcg_list_lru_alloc(memcg, &zswap_list_lru, GFP_KERNEL)) {
			mem_cgroup_put(memcg);
			goto put_pool;
		}
		mem_cgroup_put(memcg);
	}

	for (index = 0; index < nr_pages; ++index) {
		struct page *page = folio_page(folio, index);

		/* 多页 folio 逐 base page 发布；任一失败后统一清除已发布的前缀。 */
		if (!zswap_store_page(page, objcg, pool))
			goto put_pool;
	}

	if (objcg)
		count_objcg_events(objcg, ZSWPOUT, nr_pages);

	count_vm_events(ZSWPOUT, nr_pages);

	/* ret 只在全 folio 成功后置位，决定退出阶段是否执行范围失效。 */
	ret = true;

put_pool:
	zswap_pool_put(pool);
put_objcg:
	obj_cgroup_put(objcg);
	/* 全局高水位拒收时异步继续回写；普通局部失败不盲目启动 worker。 */
	if (!ret && zswap_pool_reached_full)
		queue_work(shrink_wq, &zswap_shrink_work);
check_old:
	/*
	 * If the zswap store fails or zswap is disabled, we must invalidate
	 * the possibly stale entries which were previously stored at the
	 * offsets corresponding to each page of the folio. Otherwise,
	 * writeback could overwrite the new data in the swapfile.
	 */
	/*
	 * zswap 关闭或本次任一页保存失败时，必须清除 folio 全范围可能存在的旧/部分新条目；否则旧缓存
	 * 日后回写会用过期内容覆盖此次将直接写入 swapfile 的新数据。
	 */
	if (!ret) {
		unsigned type = swp_type(swp);
		pgoff_t offset = swp_offset(swp);
		struct zswap_entry *entry;
		struct xarray *tree;

		/* 每个子页可能落入不同 64 MiB xarray 分片，必须按完整 swp 值重新选树。 */
		for (index = 0; index < nr_pages; ++index) {
			tree = swap_zswap_tree(swp_entry(type, offset + index));
			entry = xa_erase(tree, offset + index);
			if (entry)
				zswap_entry_free(entry);
		}
	}

	return ret;
}

/**
 * zswap_load() - load a folio from zswap
 * @folio: folio to load
 *
 * Return: 0 on success, with the folio unlocked and marked up-to-date, or one
 * of the following error codes:
 *
 *  -EIO: if the swapped out content was in zswap, but could not be loaded
 *  into the page due to a decompression failure. The folio is unlocked, but
 *  NOT marked up-to-date, so that an IO error is emitted (e.g. do_swap_page()
 *  will SIGBUS).
 *
 *  -EINVAL: if the swapped out content was in zswap, but the page belongs
 *  to a large folio, which is not supported by zswap. The folio is unlocked,
 *  but NOT marked up-to-date, so that an IO error is emitted (e.g.
 *  do_swap_page() will SIGBUS).
 *
 *  -ENOENT: if the swapped out content was not in zswap. The folio remains
 *  locked on return.
 */
/*
 * 中文契约：命中时把单页 zswap 条目解压到已锁 swapcache folio，标记 uptodate/dirty，删除缓存条目
 * 并解锁后返回 0。解压失败返回 -EIO，大 folio 返回 -EINVAL，这两者都会解锁但不置 uptodate；
 * 未命中返回 -ENOENT 且保持 folio 锁，交由调用者读取底层 swap。函数成功会消费 entry。
 */
int zswap_load(struct folio *folio)
{
	swp_entry_t swp = folio->swap;
	pgoff_t offset = swp_offset(swp);
	struct xarray *tree = swap_zswap_tree(swp);
	struct zswap_entry *entry;

	VM_WARN_ON_ONCE(!folio_test_locked(folio));
	VM_WARN_ON_ONCE(!folio_test_swapcache(folio));

	if (zswap_never_enabled())
		return -ENOENT;

	/*
	 * Large folios should not be swapped in while zswap is being used, as
	 * they are not properly handled. Zswap does not properly load large
	 * folios, and a large folio may only be partially in zswap.
	 */
	/* zswap 以 base-page slot 独立存储，无法保证 large folio 的所有子页同时命中并被完整恢复。 */
	if (WARN_ON_ONCE(folio_test_large(folio))) {
		folio_unlock(folio);
		return -EINVAL;
	}

	/* folio/swapcache 锁稳定 slot，因此命中后 entry 可安全使用到显式 xa_erase。 */
	entry = xa_load(tree, offset);
	if (!entry)
		return -ENOENT;

	if (!zswap_decompress(entry, folio)) {
		folio_unlock(folio);
		return -EIO;
	}

	/* 只有完整解压成功才置 uptodate 并累计逻辑换入事件。 */
	folio_mark_uptodate(folio);

	count_vm_event(ZSWPIN);
	if (entry->objcg)
		count_objcg_events(entry->objcg, ZSWPIN, 1);

	/*
	 * We are reading into the swapcache, invalidate zswap entry.
	 * The swapcache is the authoritative owner of the page and
	 * its mappings, and the pressure that results from having two
	 * in-memory copies outweighs any benefits of caching the
	 * compression work.
	 */
	/*
	 * 数据进入 swapcache 后它成为页内容与映射的权威副本；继续保留压缩副本会造成双份内存压力，
	 * 其代价高于省下一次压缩，因此把 folio 置脏后删除并释放 zswap 条目。
	 */
	folio_mark_dirty(folio);
	xa_erase(tree, offset);
	zswap_entry_free(entry);

	folio_unlock(folio);
	return 0;
}

/*
 * zswap_invalidate() - 删除指定 swap slot 的缓存条目。
 * @swp: 已启用 swap type 中的条目值。
 * 返回：无；存在时从 xarray 原子摘除并释放全部资源，不存在时幂等返回。
 * 并发：调用者的 swap/folio 锁协议保证不会与同 slot 的 store 破坏版本关系。
 */
void zswap_invalidate(swp_entry_t swp)
{
	pgoff_t offset = swp_offset(swp);
	struct xarray *tree = swap_zswap_tree(swp);
	struct zswap_entry *entry;

	if (xa_empty(tree))
		return;

	/* xa_empty 只是快速路径；最终仍以 xa_erase 的原子返回判断条目是否存在。 */
	entry = xa_erase(tree, offset);
	if (entry)
		zswap_entry_free(entry);
}

/*
 * zswap_swapon() - 为新启用 swap type 分配按 64 MiB 分片的 xarray 索引。
 * @type: swap 类型编号；@nr_pages: 该设备可寻址页槽数。
 * 返回：成功为 0，索引数组分配失败为 -ENOMEM；成功 ownership 交给全局 zswap_trees[type]。
 * 上下文：swapon 串行化调用，可睡眠，失败只禁用该 swap type 的 zswap。
 */
int zswap_swapon(int type, unsigned long nr_pages)
{
	struct xarray *trees, *tree;
	unsigned int nr, i;

	/* 末尾不足 64 MiB 也需要独立分片，故使用向上取整。 */
	nr = DIV_ROUND_UP(nr_pages, ZSWAP_ADDRESS_SPACE_PAGES);
	trees = kvzalloc_objs(*tree, nr);
	if (!trees) {
		pr_err("alloc failed, zswap disabled for swap type %d\n", type);
		return -ENOMEM;
	}

	for (i = 0; i < nr; i++)
		xa_init(trees + i);

	/* 先完成所有分片初始化，最后发布数量和指针供 store/load 使用。 */
	nr_zswap_trees[type] = nr;
	zswap_trees[type] = trees;
	return 0;
}

/*
 * zswap_swapoff() - 销毁一个 swap type 的全部空 xarray 分片。
 * @type: 正在完成 swapoff 的类型；返回无。
 * 前置条件：try_to_unuse() 已逐 slot invalidate，非空只告警；释放后全局指针和分片数清零。
 */
void zswap_swapoff(int type)
{
	struct xarray *trees = zswap_trees[type];
	unsigned int i;

	if (!trees)
		return;

	/* try_to_unuse() invalidated all the entries already */
	/* swapoff 的 try_to_unuse() 理应已清除所有条目；残留表示生命周期缺陷，不能在这里静默释放 entry。 */
	for (i = 0; i < nr_zswap_trees[type]; i++)
		WARN_ON_ONCE(!xa_empty(trees + i));

	kvfree(trees);
	nr_zswap_trees[type] = 0;
	zswap_trees[type] = NULL;
}

/*********************************
* debugfs functions
**********************************/
/* debugfs functions：只读导出容量和近似事件计数，不参与资源同步。 */
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static struct dentry *zswap_debugfs_root;

/*
 * debugfs_get_total_size() - 读取所有 zsmalloc 池当前占用字节数。
 * @data: 未使用；@val: 输出位置。
 * 返回：恒为 0；值是 zswap_total_pages() 的瞬时快照。
 */
static int debugfs_get_total_size(void *data, u64 *val)
{
	*val = zswap_total_pages() * PAGE_SIZE;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(total_size_fops, debugfs_get_total_size, NULL, "%llu\n");

/*
 * debugfs_get_stored_pages() - 读取当前缓存条目页数。
 * @data: 未使用；@val: 输出原子计数；返回恒为 0，不建立一致性快照。
 */
static int debugfs_get_stored_pages(void *data, u64 *val)
{
	*val = atomic_long_read(&zswap_stored_pages);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(stored_pages_fops, debugfs_get_stored_pages, NULL, "%llu\n");

/*
 * debugfs_get_stored_incompressible_pages() - 读取原样存储页数。
 * @data: 未使用；@val: 输出原子计数；返回恒为 0。
 */
static int debugfs_get_stored_incompressible_pages(void *data, u64 *val)
{
	*val = atomic_long_read(&zswap_stored_incompressible_pages);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(stored_incompressible_pages_fops,
		debugfs_get_stored_incompressible_pages, NULL, "%llu\n");

/*
 * zswap_debugfs_init() - 创建 /sys/kernel/debug/zswap 下的只读统计接口。
 * 返回：debugfs 尚未就绪时为 -ENODEV，否则完成 best-effort 创建并返回 0。
 * 上下文：初始化路径调用，可睡眠；dentry 生命周期由 debugfs/内核常驻 zswap 管理。
 */
static int zswap_debugfs_init(void)
{
	if (!debugfs_initialized())
		return -ENODEV;

	zswap_debugfs_root = debugfs_create_dir("zswap", NULL);

	/* 第一组文件直接暴露无需严格一致性的失败/限流累计计数。 */
	debugfs_create_u64("pool_limit_hit", 0444,
			   zswap_debugfs_root, &zswap_pool_limit_hit);
	debugfs_create_u64("reject_reclaim_fail", 0444,
			   zswap_debugfs_root, &zswap_reject_reclaim_fail);
	debugfs_create_u64("reject_alloc_fail", 0444,
			   zswap_debugfs_root, &zswap_reject_alloc_fail);
	debugfs_create_u64("reject_kmemcache_fail", 0444,
			   zswap_debugfs_root, &zswap_reject_kmemcache_fail);
	/* 压缩质量、算法错误、解压错误和回写成功分别保留，便于区分瓶颈。 */
	debugfs_create_u64("reject_compress_fail", 0444,
			   zswap_debugfs_root, &zswap_reject_compress_fail);
	debugfs_create_u64("reject_compress_poor", 0444,
			   zswap_debugfs_root, &zswap_reject_compress_poor);
	debugfs_create_u64("decompress_fail", 0444,
			   zswap_debugfs_root, &zswap_decompress_fail);
	debugfs_create_u64("written_back_pages", 0444,
			   zswap_debugfs_root, &zswap_written_back_pages);
	/* 容量与当前条目计数通过 getter 读取，避免把原子类型或换算逻辑直接暴露。 */
	debugfs_create_file("pool_total_size", 0444,
			    zswap_debugfs_root, NULL, &total_size_fops);
	debugfs_create_file("stored_pages", 0444,
			    zswap_debugfs_root, NULL, &stored_pages_fops);
	debugfs_create_file("stored_incompressible_pages", 0444,
			    zswap_debugfs_root, NULL,
			    &stored_incompressible_pages_fops);

	return 0;
}
#else
/* CONFIG_DEBUG_FS 关闭时保留无副作用的初始化桩，使主初始化路径无需条件分支。 */
static int zswap_debugfs_init(void)
{
	return 0;
}
#endif

/*********************************
* module init and exit
**********************************/
/* module init：依次建立 slab、CPU hotplug、工作队列、shrinker/LRU、初始池与观测接口。 */
/*
 * zswap_setup() - 一次性建立 zswap 运行所需的全部全局资源。
 * 返回：成功为 0；任一关键资源失败统一回滚并返回 -ENOMEM，将状态永久置为 INIT_FAILED。
 * 上下文：调用者持 zswap_init_lock 或处于 late init 单线程阶段，可睡眠；成功后资源常驻。
 */
static int zswap_setup(void)
{
	struct zswap_pool *pool;
	int ret;

	/* entry slab 是所有 store 的首个资源，失败时尚无其它子系统需要回滚。 */
	zswap_entry_cache = KMEM_CACHE(zswap_entry, 0);
	if (!zswap_entry_cache) {
		pr_err("entry cache creation failed\n");
		goto cache_fail;
	}

	ret = cpuhp_setup_state_multi(CPUHP_MM_ZSWP_POOL_PREPARE,
				      "mm/zswap_pool:prepare",
				      zswap_cpu_comp_prepare,
				      NULL);
	/* multi-state 让此后每个 pool 注册 instance，并为所有 possible CPU 准备独立 crypto 上下文。 */
	if (ret)
		goto hp_fail;

	shrink_wq = alloc_workqueue("zswap-shrink",
			WQ_UNBOUND|WQ_MEM_RECLAIM, 1);
	/* WQ_MEM_RECLAIM 保证内存紧张时仍有执行上下文，max_active=1 串行化全局游标。 */
	if (!shrink_wq)
		goto shrink_wq_fail;

	zswap_shrinker = zswap_alloc_shrinker();
	if (!zswap_shrinker)
		goto shrinker_fail;
	/* list_lru 与 shrinker 绑定后才能建立 memcg 桶；注册后 vmscan 才会调用它。 */
	if (list_lru_init_memcg(&zswap_list_lru, zswap_shrinker))
		goto lru_fail;
	shrinker_register(zswap_shrinker);

	INIT_WORK(&zswap_shrink_work, shrink_worker);

	/* 基础设施成功后才创建初始池；建池失败允许框架存活但强制关闭 enabled。 */
	pool = __zswap_pool_create_fallback();
	if (pool) {
		pr_info("loaded using pool %s\n", pool->tfm_name);
		list_add(&pool->list, &zswap_pools);
		/* 初始池在单线程 init 阶段直接发布为链表头，其初始 ref 即 current 身份引用。 */
		zswap_has_pool = true;
		static_branch_enable(&zswap_ever_enabled);
	} else {
		pr_err("pool creation failed\n");
		zswap_enabled = false;
	}

	if (zswap_debugfs_init())
		pr_warn("debugfs initialization failed\n");
	/* debugfs 失败不影响数据路径；到这里核心设施已经完整，发布成功状态。 */
	zswap_init_state = ZSWAP_INIT_SUCCEED;
	return 0;

lru_fail:
	/* 错误标签按成功建立的逆序释放，确保每个资源只回滚一次。 */
	shrinker_free(zswap_shrinker);
shrinker_fail:
	destroy_workqueue(shrink_wq);
shrink_wq_fail:
	cpuhp_remove_multi_state(CPUHP_MM_ZSWP_POOL_PREPARE);
hp_fail:
	kmem_cache_destroy(zswap_entry_cache);
cache_fail:
	/* if built-in, we aren't unloaded on failure; don't allow use */
	/* 内建 zswap 初始化失败后不会通过模块卸载清场，必须锁定失败状态并禁止后续使用半成品。 */
	zswap_init_state = ZSWAP_INIT_FAILED;
	zswap_enabled = false;
	return -ENOMEM;
}

/*
 * zswap_init() - late init 入口，仅在启动参数要求启用时执行完整 setup。
 * 返回：未启用或 setup 成功为 0，setup 失败返回其 errno；初始化阶段可睡眠。
 */
static int __init zswap_init(void)
{
	if (!zswap_enabled)
		return 0;
	return zswap_setup();
}
/* must be late so crypto has time to come up */
/* 放在 late_initcall，确保所选 crypto 算法驱动已有机会完成注册。 */
late_initcall(zswap_init);

MODULE_AUTHOR("Seth Jennings <sjennings@variantweb.net>");
MODULE_DESCRIPTION("Compressed cache for swap pages");
