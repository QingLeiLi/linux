/* SPDX-License-Identifier: GPL-2.0 */
/*
 * NUMA memory policies for Linux.
 * Copyright 2003,2004 Andi Kleen SuSE Labs
 */
/*
 * ============================================================================
 * 【NUMA 内存策略（Memory Policy）概述】
 *
 * 【什么是 NUMA？】
 * NUMA (Non-Uniform Memory Access，非一致性内存访问) 是多处理器系统的内存架构。
 * 每个 CPU（或 CPU 组）有自己的本地内存，访问本地内存快，访问远程内存慢。
 *
 * 【为什么需要内存策略？】
 * 在 NUMA 系统中，内存分配的位置直接影响性能：
 * - 本地内存访问延迟：约 100ns
 * - 远程内存访问延迟：约 200-300ns（慢 2-3 倍）
 *
 * 内存策略允许应用程序控制内存分配的 NUMA 节点，优化性能。
 *
 * 【支持的策略类型】（见 uapi/linux/mempolicy.h）
 * - MPOL_DEFAULT:       默认策略，优先分配本地节点内存
 * - MPOL_BIND:          严格绑定到指定节点
 * - MPOL_INTERLEAVE:    交错分配到多个节点（负载均衡）
 * - MPOL_PREFERRED:     优先从指定节点分配，不足时使用其他节点
 * - MPOL_PREFERRED_MANY:优先从多个节点分配（新增）
 * - MPOL_LOCAL:         优先从当前 CPU 的节点分配
 *
 * 【策略的作用范围】
 * 1. 进程级策略（Process Policy）：set_mempolicy() 系统调用
 *    - 影响进程的所有内存分配（栈、堆、mmap）
 * 2. VMA 级策略（VMA Policy）：mbind() 系统调用
 *    - 只影响特定虚拟内存区域（如某个 mmap 区域）
 * 3. 共享内存策略（Shared Policy）：shmem/tmpfs
 *    - 多个进程共享的内存区域（如 /dev/shm）
 *
 * 【使用场景】
 * - 数据库：将数据页绑定到运行查询的 CPU 所在节点
 * - 高性能计算：交错分配减少内存带宽瓶颈
 * - 容器：限制容器使用的 NUMA 节点
 * - 虚拟机：NUMA 感知的内存分配
 * ============================================================================
 */
#ifndef _LINUX_MEMPOLICY_H
#define _LINUX_MEMPOLICY_H 1

#include <linux/sched.h>
#include <linux/mmzone.h>
#include <linux/slab.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/node.h>
#include <linux/nodemask.h>
#include <linux/pagemap.h>
#include <uapi/linux/mempolicy.h>

struct mm_struct;

#define NO_INTERLEAVE_INDEX (-1UL)	/* use task il_prev for interleaving */
/* 交错索引的特殊值
 * 当 interleave_index == NO_INTERLEAVE_INDEX 时，
 * 使用进程的 task_struct->il_prev 字段记录上次分配的节点，
 * 实现跨进程的交错分配。
 *
 * 【为什么需要 il_prev？】
 * MPOL_INTERLEAVE 策略需要记住上次分配到哪个节点，
 * 下次分配到下一个节点，实现轮转（round-robin）。
 */

#ifdef CONFIG_NUMA

/*
 * Describe a memory policy.
 *
 * A mempolicy can be either associated with a process or with a VMA.
 * For VMA related allocations the VMA policy is preferred, otherwise
 * the process policy is used. Interrupts ignore the memory policy
 * of the current process.
 *
 * Locking policy for interleave:
 * In process context there is no locking because only the process accesses
 * its own state. All vma manipulation is somewhat protected by a down_read on
 * mmap_lock.
 *
 * Freeing policy:
 * Mempolicy objects are reference counted.  A mempolicy will be freed when
 * mpol_put() decrements the reference count to zero.
 *
 * Duplicating policy objects:
 * mpol_dup() allocates a new mempolicy and copies the specified mempolicy
 * to the new storage.  The reference count of the new object is initialized
 * to 1, representing the caller of mpol_dup().
 */
/*
 * 【内存策略的生命周期管理】
 *
 * 【策略的关联】
 * 1. 进程策略：task->mempolicy
 * 2. VMA 策略：vma->vm_policy
 * 3. 共享策略：共享内存的红黑树（sp_node）
 *
 * 【优先级】
 * VMA 策略 > 进程策略 > 默认策略
 * 中断上下文忽略进程策略（使用默认策略）
 *
 * 【锁机制】
 * - 进程策略：无需锁（只有进程自己访问）
 * - VMA 策略：mmap_lock 保护（down_read）
 * - 共享策略：sp->lock（rwlock_t）
 *
 * 【引用计数】
 * struct mempolicy 使用 atomic_t refcnt 管理生命周期：
 * - mpol_get(): refcnt++
 * - mpol_put(): refcnt--, 降为 0 时释放
 *
 * 【复制策略】
 * mpol_dup() 分配新的 mempolicy 并复制内容，
 * 新对象的 refcnt 初始化为 1（调用者持有一个引用）。
 */
struct mempolicy {
	atomic_t refcnt;
	/* 引用计数
	 * 每个持有该策略引用的对象（进程、VMA、共享内存）都会增加计数。
	 */

	unsigned short mode; 	/* See MPOL_* above */
	/* 策略模式（见 uapi/linux/mempolicy.h）
	 * - MPOL_DEFAULT (0):       默认策略
	 * - MPOL_PREFERRED (1):     优先节点
	 * - MPOL_BIND (2):          绑定节点
	 * - MPOL_INTERLEAVE (3):    交错分配
	 * - MPOL_LOCAL (4):         本地节点
	 * - MPOL_PREFERRED_MANY (5):优先多节点
	 */

	unsigned short flags;	/* See set_mempolicy() MPOL_F_* above */
	/* 策略标志位（见 uapi/linux/mempolicy.h）
	 * - MPOL_F_STATIC_NODES (1 << 15): 使用绝对节点号（不受 cpuset 影响）
	 * - MPOL_F_RELATIVE_NODES (1 << 14): 使用相对节点号（相对于 cpuset）
	 * - MPOL_F_SHARED (内部使用): 共享策略标志
	 *
	 * 【STATIC vs RELATIVE 的区别】
	 * - STATIC: nodes 直接表示物理节点（如节点 0, 1, 2）
	 * - RELATIVE: nodes 相对于 cpuset.mems（如 cpuset.mems={2,3,4}，nodes={0,1} 表示节点 2,3）
	 *
	 * 【为什么需要 RELATIVE？】
	 * 容器或 cgroup 限制了进程可用的 NUMA 节点（cpuset.mems），
	 * RELATIVE 模式让策略可以在不同的 cpuset 环境中复用。
	 */

	nodemask_t nodes;	/* interleave/bind/preferred/etc */
	/* 节点掩码（nodemask_t 是位图）
	 * 根据策略模式有不同含义：
	 * - MPOL_BIND: 只能从这些节点分配
	 * - MPOL_INTERLEAVE: 在这些节点间交错分配
	 * - MPOL_PREFERRED: 优先从第一个节点分配
	 * - MPOL_PREFERRED_MANY: 优先从这些节点分配
	 */

	int home_node;		/* Home node to use for MPOL_BIND and MPOL_PREFERRED_MANY */
	/* 主节点（用于 MPOL_BIND 和 MPOL_PREFERRED_MANY）
	 * 表示进程的"家乡节点"，优先在该节点分配内存。
	 * -1 表示无主节点（使用当前 CPU 所在节点）。
	 *
	 * 【为什么需要 home_node？】
	 * 即使策略允许多个节点，也希望优先使用某个节点（通常是进程运行的节点），
	 * 减少远程内存访问。
	 */

	union {
		nodemask_t cpuset_mems_allowed;	/* relative to these nodes */
		/* cpuset 允许的内存节点（用于 MPOL_F_RELATIVE_NODES）
		 * 保存设置策略时的 cpuset.mems，用于后续节点映射。
		 */

		nodemask_t user_nodemask;	/* nodemask passed by user */
		/* 用户传入的原始节点掩码
		 * 保存用户空间传入的 nodemask，用于策略导出（get_mempolicy）。
		 */
	} w;

	struct rcu_head rcu;
	/* RCU 回调头
	 * 用于延迟释放 mempolicy 对象（等待 RCU grace period）。
	 *
	 * 【为什么需要 RCU？】
	 * 策略查询（get_vma_policy）可能在无锁的 RCU 读侧临界区中进行，
	 * 释放策略时需要等待所有读者完成，才能安全释放内存。
	 */
};

/*
 * Support for managing mempolicy data objects (clone, copy, destroy)
 * The default fast path of a NULL MPOL_DEFAULT policy is always inlined.
 */
/*
 * ============================================================================
 * 【内存策略管理函数】
 *
 * 这些函数用于创建、复制、销毁内存策略对象。
 * 为了优化性能，NULL 策略（等价于 MPOL_DEFAULT）的快速路径被内联。
 * ============================================================================
 */

extern void __mpol_put(struct mempolicy *pol);
static inline void mpol_put(struct mempolicy *pol)
{
	if (pol)
		__mpol_put(pol);
}
/* 释放内存策略的引用
 * @pol: 要释放的策略（可以为 NULL）
 *
 * 【工作流程】
 * 1. 如果 pol == NULL，直接返回（无操作）
 * 2. 原子递减 refcnt
 * 3. 如果 refcnt 降为 0，通过 RCU 延迟释放内存
 *
 * 【为什么内联 NULL 检查？】
 * 大多数情况下策略为 NULL（使用默认策略），
 * 内联 NULL 检查避免函数调用开销。
 */

/*
 * Does mempolicy pol need explicit unref after use?
 * Currently only needed for shared policies.
 */
static inline int mpol_needs_cond_ref(struct mempolicy *pol)
{
	return (pol && (pol->flags & MPOL_F_SHARED));
}
/* 检查策略是否需要条件引用（仅共享策略需要）
 * @pol: 要检查的策略
 *
 * 返回值：true=需要显式释放引用，false=不需要
 *
 * 【为什么共享策略特殊？】
 * - 进程策略：由进程生命周期管理，fork/exec 时自动处理
 * - VMA 策略：由 VMA 生命周期管理，munmap 时自动处理
 * - 共享策略：多个进程共享，需要显式引用计数管理
 *
 * 【使用场景】
 * 临时获取共享策略时，需要检查是否要调用 mpol_cond_put()。
 */

static inline void mpol_cond_put(struct mempolicy *pol)
{
	if (mpol_needs_cond_ref(pol))
		__mpol_put(pol);
}
/* 条件释放策略引用（仅对共享策略有效）
 * @pol: 要释放的策略
 *
 * 【使用模式】
 * struct mempolicy *pol = get_vma_policy(vma, addr, order, &ilx);
 * // 使用 pol
 * mpol_cond_put(pol);  // 如果是共享策略则释放
 */

extern struct mempolicy *__mpol_dup(struct mempolicy *pol);
static inline struct mempolicy *mpol_dup(struct mempolicy *pol)
{
	if (pol)
		pol = __mpol_dup(pol);
	return pol;
}
/* 复制内存策略
 * @pol: 要复制的策略（可以为 NULL）
 *
 * 返回值：新分配的策略副本（引用计数为 1），或 NULL
 *
 * 【使用场景】
 * - fork(): 子进程继承父进程的策略
 * - mbind(): 为 VMA 设置独立的策略副本
 *
 * 【注意事项】
 * 返回的策略是新对象，调用者负责最终释放（mpol_put）。
 */

static inline void mpol_get(struct mempolicy *pol)
{
	if (pol)
		atomic_inc(&pol->refcnt);
}
/* 增加策略的引用计数
 * @pol: 要增加引用的策略（可以为 NULL）
 *
 * 【使用场景】
 * 需要长期持有策略引用时：
 * - 将策略保存到数据结构
 * - 跨函数调用传递策略
 *
 * 【配对规则】
 * 每次 mpol_get 必须配对一次 mpol_put。
 */

extern bool __mpol_equal(struct mempolicy *a, struct mempolicy *b);
static inline bool mpol_equal(struct mempolicy *a, struct mempolicy *b)
{
	if (a == b)
		return true;
	return __mpol_equal(a, b);
}
/* 比较两个策略是否相等
 * @a: 第一个策略
 * @b: 第二个策略
 *
 * 返回值：true=相等，false=不相等
 *
 * 【快速路径】
 * 如果两个指针相同（包括都为 NULL），直接返回 true。
 *
 * 【比较内容】
 * - 策略模式（mode）
 * - 策略标志（flags）
 * - 节点掩码（nodes）
 * - 主节点（home_node）
 *
 * 【使用场景】
 * - 检查策略是否需要更新（避免不必要的操作）
 * - 合并相同策略的 VMA（vma_merge）
 */

/*
 * Tree of shared policies for a shared memory region.
 */
/*
 * 【共享策略树】
 *
 * 共享内存区域（shmem/tmpfs）的策略存储在红黑树中，
 * 每个节点（sp_node）表示一个地址范围的策略。
 */
struct shared_policy {
	struct rb_root root;
	/* 红黑树根节点
	 * 存储所有 sp_node，按地址范围排序
	 */

	rwlock_t lock;
	/* 读写锁
	 * - 读锁：查找策略（mpol_shared_policy_lookup）
	 * - 写锁：设置/删除策略（mpol_set_shared_policy）
	 *
	 * 【为什么用读写锁？】
	 * 策略查找频繁（每次页面分配），策略修改罕见（只在 mbind 时），
	 * 读写锁允许多个读者并发，提高性能。
	 */
};

struct sp_node {
	struct rb_node nd;
	/* 红黑树节点（用于链入 shared_policy.root） */

	pgoff_t start, end;
	/* 策略适用的页面偏移范围 [start, end)
	 * pgoff_t 是页面偏移量（page offset），单位是页（PAGE_SIZE）
	 *
	 * 【示例】
	 * 假设 PAGE_SIZE=4096，start=10, end=20：
	 * 策略适用于偏移 40960 到 81919 字节的范围
	 */

	struct mempolicy *policy;
	/* 该范围的内存策略
	 * 策略对象被多个 sp_node 共享（引用计数管理）
	 */
};

/*
 * ============================================================================
 * 【共享策略管理函数】
 * ============================================================================
 */

int vma_dup_policy(struct vm_area_struct *src, struct vm_area_struct *dst);
/* 复制 VMA 的策略到另一个 VMA
 * @src: 源 VMA
 * @dst: 目标 VMA
 *
 * 返回值：0=成功，负值=失败（如内存不足）
 *
 * 【使用场景】
 * - fork(): 复制父进程的 VMA 到子进程
 * - mremap(): 扩展或移动 VMA
 */

void mpol_shared_policy_init(struct shared_policy *sp, struct mempolicy *mpol);
/* 初始化共享策略树
 * @sp:   要初始化的共享策略树
 * @mpol: 初始策略（可为 NULL，表示默认策略）
 *
 * 【使用场景】
 * 创建共享内存对象（shmem/tmpfs）时初始化策略树。
 */

int mpol_set_shared_policy(struct shared_policy *sp,
			   struct vm_area_struct *vma, struct mempolicy *mpol);
/* 为共享内存的某个范围设置策略
 * @sp:   共享策略树
 * @vma:  要设置策略的 VMA（提供地址范围）
 * @mpol: 要设置的策略
 *
 * 返回值：0=成功，负值=失败
 *
 * 【工作流程】
 * 1. 查找 vma->vm_pgoff 到 vma->vm_pgoff + vma_pages(vma) 范围的节点
 * 2. 删除重叠的旧节点
 * 3. 插入新的 sp_node，关联新策略
 * 4. 合并相邻的相同策略节点（优化树结构）
 */

void mpol_free_shared_policy(struct shared_policy *sp);
/* 释放共享策略树的所有节点
 * @sp: 要释放的共享策略树
 *
 * 【使用场景】
 * 销毁共享内存对象时释放策略。
 */

struct mempolicy *mpol_shared_policy_lookup(struct shared_policy *sp,
					    pgoff_t idx);
/* 查找指定页面偏移的策略
 * @sp:  共享策略树
 * @idx: 页面偏移量
 *
 * 返回值：找到的策略，或 NULL（使用默认策略）
 *
 * 【注意事项】
 * - 返回的策略引用计数未增加（调用者持有 sp->lock 读锁）
 * - 如果需要保存策略，必须 mpol_get() 并在持有锁时使用
 */

struct mempolicy *get_task_policy(struct task_struct *p);
struct mempolicy *__get_vma_policy(struct vm_area_struct *vma,
		unsigned long addr, pgoff_t *ilx);
struct mempolicy *get_vma_policy(struct vm_area_struct *vma,
		unsigned long addr, int order, pgoff_t *ilx);
bool vma_policy_mof(struct vm_area_struct *vma);

extern void numa_default_policy(void);
extern void numa_policy_init(void);
extern void mpol_rebind_task(struct task_struct *tsk, const nodemask_t *new);
extern void mpol_rebind_mm(struct mm_struct *mm, nodemask_t *new);

extern int huge_node(struct vm_area_struct *vma,
				unsigned long addr, gfp_t gfp_flags,
				struct mempolicy **mpol, nodemask_t **nodemask);
extern bool init_nodemask_of_mempolicy(nodemask_t *mask);
extern bool mempolicy_in_oom_domain(struct task_struct *tsk,
				const nodemask_t *mask);
extern unsigned int mempolicy_slab_node(void);

extern enum zone_type policy_zone;

static inline void check_highest_zone(enum zone_type k)
{
	if (k > policy_zone && k != ZONE_MOVABLE)
		policy_zone = k;
}

int do_migrate_pages(struct mm_struct *mm, const nodemask_t *from,
		     const nodemask_t *to, int flags);


#ifdef CONFIG_TMPFS
extern int mpol_parse_str(char *str, struct mempolicy **mpol);
#endif

extern void mpol_to_str(char *buffer, int maxlen, struct mempolicy *pol);

/* Check if a vma is migratable */
extern bool vma_migratable(struct vm_area_struct *vma);

int mpol_misplaced(struct folio *folio, struct vm_fault *vmf,
					unsigned long addr);
extern void mpol_put_task_policy(struct task_struct *);

static inline bool mpol_is_preferred_many(struct mempolicy *pol)
{
	return  (pol->mode == MPOL_PREFERRED_MANY);
}

extern bool apply_policy_zone(struct mempolicy *policy, enum zone_type zone);

extern int mempolicy_set_node_perf(unsigned int node,
				   struct access_coordinate *coords);

#else

struct mempolicy {};

static inline struct mempolicy *get_task_policy(struct task_struct *p)
{
	return NULL;
}

static inline bool mpol_equal(struct mempolicy *a, struct mempolicy *b)
{
	return true;
}

static inline void mpol_put(struct mempolicy *pol)
{
}

static inline void mpol_cond_put(struct mempolicy *pol)
{
}

static inline void mpol_get(struct mempolicy *pol)
{
}

struct shared_policy {};

static inline void mpol_shared_policy_init(struct shared_policy *sp,
						struct mempolicy *mpol)
{
}

static inline void mpol_free_shared_policy(struct shared_policy *sp)
{
}

static inline struct mempolicy *
mpol_shared_policy_lookup(struct shared_policy *sp, pgoff_t idx)
{
	return NULL;
}

static inline struct mempolicy *get_vma_policy(struct vm_area_struct *vma,
				unsigned long addr, int order, pgoff_t *ilx)
{
	*ilx = 0;
	return NULL;
}

static inline int
vma_dup_policy(struct vm_area_struct *src, struct vm_area_struct *dst)
{
	return 0;
}

static inline void numa_policy_init(void)
{
}

static inline void numa_default_policy(void)
{
}

static inline void mpol_rebind_task(struct task_struct *tsk,
				const nodemask_t *new)
{
}

static inline void mpol_rebind_mm(struct mm_struct *mm, nodemask_t *new)
{
}

static inline int huge_node(struct vm_area_struct *vma,
				unsigned long addr, gfp_t gfp_flags,
				struct mempolicy **mpol, nodemask_t **nodemask)
{
	*mpol = NULL;
	*nodemask = NULL;
	return 0;
}

static inline bool init_nodemask_of_mempolicy(nodemask_t *m)
{
	return false;
}

static inline int do_migrate_pages(struct mm_struct *mm, const nodemask_t *from,
				   const nodemask_t *to, int flags)
{
	return 0;
}

static inline void check_highest_zone(int k)
{
}

#ifdef CONFIG_TMPFS
static inline int mpol_parse_str(char *str, struct mempolicy **mpol)
{
	return 1;	/* error */
}
#endif

static inline int mpol_misplaced(struct folio *folio,
				 struct vm_fault *vmf,
				 unsigned long address)
{
	return -1; /* no node preference */
}

static inline void mpol_put_task_policy(struct task_struct *task)
{
}

static inline bool mpol_is_preferred_many(struct mempolicy *pol)
{
	return  false;
}

#endif /* CONFIG_NUMA */
#endif
