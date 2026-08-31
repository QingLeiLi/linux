// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Functions for initializing, allocating, freeing and duplicating VMAs. Shared
 * between CONFIG_MMU and non-CONFIG_MMU kernel configurations.
 */
/*
 * 本文件集中实现 VMA 的 slab 初始化、对象分配、复制和最终释放；MMU 与
 * NOMMU 路径共享这套对象生命周期，差异字段由下方条件编译分支处理。
 */

#include "vma_internal.h"
#include "vma.h"

/* SLAB cache for vm_area_struct structures */
/*
 * vm_area_struct 的专用 slab 缓存。启动期由 vma_state_init() 创建，之后
 * vm_area_alloc()/vm_area_dup() 分配对象，vm_area_free() 将对象归还缓存。
 */
static struct kmem_cache *vm_area_cachep;

/*
 * vma_state_init() - 在 mmap 子系统启动时创建 VMA 专用 slab 缓存。
 *
 * 业务背景：MMU 的 mmap_init() 与 NOMMU 的同名入口都在普通 VMA 分配发生前
 * 调用本函数，使两种配置使用相同的对象缓存和 RCU 类型安全约束。
 * 入参：无。
 * 出参/返回：无直接返回值；成功后发布只读使用的 vm_area_cachep。创建失败由
 * SLAB_PANIC 直接终止启动，不向调用者返回 errno。
 * 注意事项：仅限 __init 启动阶段、不可重复并发调用；创建过程可能分配内存。
 * SLAB_TYPESAFE_BY_RCU 只延迟 slab 对象复用后的身份判定责任，并不让 VMA 字段
 * 在无锁下保持稳定。
 */
void __init vma_state_init(void)
{
	/*
	 * vm_freeptr 为释放态对象提供独立 freelist 指针，避免覆盖仍可能被 RCU
	 * 读者观察的业务字段；每 CPU sheaf 以 32 个对象为批次降低分配器争用。
	 */
	struct kmem_cache_args args = {
		.use_freeptr_offset = true,
		.freeptr_offset = offsetof(struct vm_area_struct, vm_freeptr),
		.sheaf_capacity = 32,
	};

	/*
	 * 硬件缓存行对齐减少热点字段伪共享，SLAB_ACCOUNT 计入 memcg；返回后
	 * vm_area_cachep 成为后续全部 VMA 分配/释放的唯一缓存句柄。
	 */
	vm_area_cachep = kmem_cache_create("vm_area_struct",
			sizeof(struct vm_area_struct), &args,
			SLAB_HWCACHE_ALIGN|SLAB_PANIC|SLAB_TYPESAFE_BY_RCU|
			SLAB_ACCOUNT);
}

/*
 * vm_area_alloc() - 为指定 mm 分配并初始化一个尚未发布的空 VMA。
 *
 * 业务背景：mmap、exec 特殊映射和 NOMMU 建图路径先取得裸 VMA，再填写范围、
 * flags、文件等字段，最后才插入 mm 的 VMA 树。
 * 入参：@mm 是新 VMA 所属地址空间的借用指针，不可为 NULL；本函数不取得
 * mm 引用，也不改变其 VMA 树。
 * 出参/返回：成功返回由调用者独占、已清零并绑定 @mm 的 VMA；失败返回 NULL，
 * 无资源需要调用者回滚。成功对象须由发布路径接管或用 vm_area_free() 释放。
 * 注意事项：GFP_KERNEL 分配可睡眠，调用者不得处于原子上下文；返回对象尚未
 * detached/published，范围和策略等业务字段仍待调用者填写。
 */
struct vm_area_struct *vm_area_alloc(struct mm_struct *mm)
{
	/* vma 在分配成功到插入 VMA 树前完全归当前调用者所有。 */
	struct vm_area_struct *vma;

	vma = kmem_cache_alloc(vm_area_cachep, GFP_KERNEL);
	if (!vma)
		return NULL;

	/* 清零旧 slab 内容，绑定 mm，并初始化默认 vm_ops、anon 链和 VMA 锁。 */
	vma_init(vma, mm);

	return vma;
}

/*
 * vm_area_init_from() - 复制 VMA 的可继承字段但不发布目标对象。
 *
 * 业务背景：vm_area_dup() 需要为 fork、split 或 merge 建立结构快照；本 helper
 * 只复制共享语义字段，把链表节点、锁、NUMA 状态和需单独引用的资源留给后续阶段。
 * 入参：@src 是仍有效的源 VMA 借用指针；@dest 是刚从 slab 分配、尚未发布的
 * 输出对象。两者不可为 NULL、不可重叠，ownership 均不在此转移。
 * 出参/返回：无直接返回值；@dest 获得字段浅拷贝，但尚不是可插树的完整 VMA。
 * 注意事项：调用者须保证 vm_flags/vm_file 的独占写语义；此处不会增加 file、
 * anon_vma、mempolicy 等引用，后续具体操作必须按自身协议补齐或回滚。
 */
static void vm_area_init_from(const struct vm_area_struct *src,
			      struct vm_area_struct *dest)
{
	/* 第一阶段复制地址范围、后端回调和借用资源指针，暂不取得新引用。 */
	dest->vm_mm = src->vm_mm;
	dest->vm_ops = src->vm_ops;
	dest->vm_start = src->vm_start;
	dest->vm_end = src->vm_end;
	dest->anon_vma = src->anon_vma;
	dest->vm_pgoff = src->vm_pgoff;
	dest->vm_file = src->vm_file;
	dest->vm_private_data = src->vm_private_data;
	/* flags 与页保护共同描述访问语义，仍只是一致性快照而非树内发布。 */
	vm_flags_init(dest, src->vm_flags);
	memcpy(&dest->vm_page_prot, &src->vm_page_prot,
	       sizeof(dest->vm_page_prot));
	/*
	 * src->shared.rb may be modified concurrently when called from
	 * dup_mmap(), but the clone will reinitialize it.
	 */
	/*
	 * 从 dup_mmap() 调用时，源 VMA 的 shared.rb 可能被并发修改；该竞态可接受，
	 * 因为克隆路径随后会按新 address_space 关系重建此节点，而不会使用复制出的
	 * 红黑树链接进行遍历。data_race() 明确记录这一有意的无锁快照。
	 */
	data_race(memcpy(&dest->shared, &src->shared, sizeof(dest->shared)));
	/* 第二阶段复制按配置存在的策略快照；这些指针的引用仍由外层复制协议补齐。 */
	memcpy(&dest->vm_userfaultfd_ctx, &src->vm_userfaultfd_ctx,
	       sizeof(dest->vm_userfaultfd_ctx));
#ifdef CONFIG_ANON_VMA_NAME
	dest->anon_name = src->anon_name;
#endif
#ifdef CONFIG_SWAP
	memcpy(&dest->swap_readahead_info, &src->swap_readahead_info,
	       sizeof(dest->swap_readahead_info));
#endif
	/* NOMMU region 与 NUMA policy 都是浅拷贝，引用处理由克隆调用链完成。 */
#ifndef CONFIG_MMU
	dest->vm_region = src->vm_region;
#endif
#ifdef CONFIG_NUMA
	dest->vm_policy = src->vm_policy;
#endif
#ifdef __HAVE_PFNMAP_TRACKING
	/* PFN 跟踪上下文必须通过 kref 单独复制，先置空避免失败清理误放源引用。 */
	dest->pfnmap_track_ctx = NULL;
#endif
}

#ifdef __HAVE_PFNMAP_TRACKING
/*
 * vma_pfnmap_track_ctx_dup() - 为克隆 VMA共享并持有 PFN 映射跟踪上下文。
 *
 * 业务背景：VMA split/fork 后多个 VMA 仍覆盖同一项 pfnmap_track() 登记，只有
 * 最后一个 VMA 销毁时才能执行 pfnmap_untrack()。
 * 入参：@orig 是源 VMA 借用指针；@new 是未发布的新 VMA 输出对象，二者均不可
 * 为 NULL。函数只转移/增加 ctx 引用，不取得 VMA 自身引用。
 * 出参/返回：无上下文或成功加引用返回 0，并在后者中令 @new 持有同一 ctx；
 * 引用计数饱和返回 -ENOMEM，@new 仍保持 NULL ctx，便于外层直接释放。
 * 注意事项：调用者须稳定 @orig->pfnmap_track_ctx；kref 只保证 ctx 生命周期，
 * 跟踪的 PFN 范围不因 VMA split 而缩小。
 */
static inline int vma_pfnmap_track_ctx_dup(struct vm_area_struct *orig,
		struct vm_area_struct *new)
{
	/* ctx 是借用快照；只有 kref_get() 成功后 new 才成为一名持有者。 */
	struct pfnmap_track_ctx *ctx = orig->pfnmap_track_ctx;

	if (likely(!ctx))
		return 0;

	/*
	 * We don't expect to ever hit this. If ever required, we would have
	 * to duplicate the tracking.
	 */
	/*
	 * 正常生命周期不会把引用推到饱和值；若将来确需支持该极端情况，就不能再
	 * 共享 ctx，而必须建立一份新的 PFN 跟踪登记。当前以 -ENOMEM 拒绝复制。
	 */
	if (unlikely(kref_read(&ctx->kref) >= REFCOUNT_MAX))
		return -ENOMEM;
	/* 加引用必须先于发布指针，保证 new 观察到 ctx 时释放责任已经成立。 */
	kref_get(&ctx->kref);
	new->pfnmap_track_ctx = ctx;
	return 0;
}

/*
 * vma_pfnmap_track_ctx_release() - 释放一个 VMA 持有的 PFN 跟踪引用。
 *
 * 业务背景：vm_area_free() 在归还 VMA slab 前调用；最后一个引用会进入
 * memory.c::pfnmap_track_ctx_release()，撤销原 PFN 范围跟踪并释放 ctx。
 * 入参：@vma 是已从树中摘除、仍由调用者独占的 VMA 借用指针，不可为 NULL。
 * 出参/返回：无直接返回值；若存在 ctx 则减少引用，并把字段清 NULL；无 ctx
 * 时无副作用。
 * 注意事项：最后一次 kref_put() 可触发最终释放，之后不得再解引用旧 ctx；
 * 清空字段避免同一 VMA cleanup 重复 put。
 */
static inline void vma_pfnmap_track_ctx_release(struct vm_area_struct *vma)
{
	/* 在 put 前保存借用指针，因为最后一次 put 后 ctx 内存可能立即失效。 */
	struct pfnmap_track_ctx *ctx = vma->pfnmap_track_ctx;

	if (likely(!ctx))
		return;

	kref_put(&ctx->kref, pfnmap_track_ctx_release);
	vma->pfnmap_track_ctx = NULL;
}
#else
/*
 * vma_pfnmap_track_ctx_dup() - 无 PFN 跟踪架构下的成功空操作。
 *
 * 业务背景：保持 vm_area_dup() 控制流与支持跟踪的架构一致。
 * 入参：@orig 与 @new 均为借用 VMA 指针，本配置不读取、不修改它们。
 * 出参/返回：恒返回 0，无输出和 ownership 变化。
 * 注意事项：这是编译期配置桩，不代表运行时建立了 PFN 跟踪引用。
 */
static inline int vma_pfnmap_track_ctx_dup(struct vm_area_struct *orig,
		struct vm_area_struct *new)
{
	return 0;
}

/*
 * vma_pfnmap_track_ctx_release() - 无 PFN 跟踪架构下的释放空操作。
 *
 * 业务背景：让 vm_area_free() 无需条件编译即可执行统一 cleanup。
 * 入参：@vma 为借用 VMA 指针，本配置不读取它。
 * 出参/返回：无直接返回值、无副作用和 ownership 变化。
 * 注意事项：仅在未定义 __HAVE_PFNMAP_TRACKING 时存在。
 */
static inline void vma_pfnmap_track_ctx_release(struct vm_area_struct *vma)
{
}
#endif

/*
 * vm_area_dup() - 分配源 VMA 的未发布克隆并建立克隆自身的生命周期状态。
 *
 * 业务背景：fork 的 dup_mmap()、VMA split/merge 与 NOMMU 复制路径先克隆公共
 * 字段，再由各自调用者调整范围、策略、文件引用并插入目标索引。
 * 入参：@orig 是调用期间稳定的源 VMA 借用指针，不可为 NULL；调用者须保证
 * vm_flags/vm_file 没有并发写者，函数不消费源对象。
 * 出参/返回：成功返回由调用者独占、尚未发布的新 VMA；slab 分配或 PFN 跟踪
 * 引用失败返回 NULL，内部已释放部分对象，@orig 不变。
 * 注意事项：GFP_KERNEL 可睡眠；返回对象对 file、policy、anon_vma 等仍多为
 * 浅拷贝，调用者必须按具体 fork/split 协议取得引用并处理失败回滚。
 */
struct vm_area_struct *vm_area_dup(struct vm_area_struct *orig)
{
	/* new 从分配到返回始终未发布；任一失败可直接归还 slab。 */
	struct vm_area_struct *new = kmem_cache_alloc(vm_area_cachep, GFP_KERNEL);

	if (!new)
		return NULL;

	/* KCSAN 断言记录外层同步契约，随后执行不带字段锁的浅拷贝。 */
	ASSERT_EXCLUSIVE_WRITER(orig->vm_flags);
	ASSERT_EXCLUSIVE_WRITER(orig->vm_file);
	vm_area_init_from(orig, new);

	/* PFN ctx 是第一项可能失败的持有型资源；失败时尚无其他新引用需释放。 */
	if (vma_pfnmap_track_ctx_dup(orig, new)) {
		kmem_cache_free(vm_area_cachep, new);
		return NULL;
	}
	/* 重建不可复制的锁、链表和 NUMA 状态，再为匿名名称取得独立引用。 */
	vma_lock_init(new, true);
	INIT_LIST_HEAD(&new->anon_vma_chain);
	vma_numab_state_init(new);
	dup_anon_vma_name(orig, new);

	return new;
}

/*
 * vm_area_free() - 销毁已摘除 VMA 的私有状态并将对象归还 slab。
 *
 * 业务背景：VMA 删除、失败回滚和 mm teardown 在完成索引摘除及外部 file/policy/
 * anon_vma 清理后，用本函数收尾对象自身持有的 NUMA、名称和 PFN 跟踪资源。
 * 入参：@vma 是调用者独占、已 detached 的 VMA；函数消费该对象，返回后指针失效。
 * 出参/返回：无直接返回值；释放 VMA 自身资源并归还 vm_area_cachep。
 * 注意事项：调用者必须先阻止新的树/interval-tree 查找者获得对象；本函数不替代
 * vma_close()、fput()、mpol_put() 或 unlink_anon_vmas() 等上层 teardown。
 */
void vm_area_free(struct vm_area_struct *vma)
{
	/* The vma should be detached while being destroyed. */
	/* 销毁时 VMA 必须已从可查找结构摘除；断言防止仍可见对象被提前复用。 */
	vma_assert_detached(vma);
	/* 按对象自身持有关系释放可选状态，最后归还 slab，使 @vma 立即失效。 */
	vma_numab_state_free(vma);
	free_anon_vma_name(vma);
	vma_pfnmap_track_ctx_release(vma);
	kmem_cache_free(vm_area_cachep, vma);
}
