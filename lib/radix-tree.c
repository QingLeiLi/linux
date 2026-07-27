// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2001 Momchil Velikov
 * Portions Copyright (C) 2001 Christoph Hellwig
 * Copyright (C) 2005 SGI, Christoph Lameter
 * Copyright (C) 2006 Nick Piggin
 * Copyright (C) 2012 Konstantin Khlebnikov
 * Copyright (C) 2016 Intel, Matthew Wilcox
 * Copyright (C) 2016 Intel, Ross Zwisler
 */
/*
 * Radix Tree 兼容层学习地图
 *
 * 当前内核的 radix_tree_root 在实现上复用 XArray 字段和内部编码。本文件
 * 主要维持旧 Radix Tree/IDR API 的语义，并把操作落实到 xa_head、节点
 * slots、tags 和 RCU 发布协议上；它不是一份与 XArray 完全独立的树实现。
 *
 * 建议按以下主线阅读：
 * 1. 节点编码、tag 和 per-CPU preload；
 * 2. radix_tree_extend()/shrink() 的树高生命周期；
 * 3. create/insert/lookup/replace/delete 的闭环；
 * 4. tag 从叶 slot 向根聚合，以及 tagged iteration；
 * 5. idr_get_free() 如何把 IDR_FREE tag 当作空闲子树摘要。
 *
 * 并发不变量：
 * - 写侧由具体调用者的树锁串行化，本文件多数 helper 不自行加树锁；
 * - RCU reader 可沿旧节点继续读取，因此摘除节点必须 call_rcu() 延迟释放；
 * - rcu_assign_pointer() 负责发布新 slot/root，但不管理叶 item 的生命周期；
 * - preload 通过 local_lock 固定当前 CPU，成功接口返回时仍保持该保护，
 *   调用者最终必须用 radix_tree_preload_end()/idr_preload_end() 结束。
 */

#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/bug.h>
#include <linux/cpu.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kmemleak.h>
#include <linux/percpu.h>
#include <linux/preempt.h>		/* in_interrupt() */
/* 提供中断上下文判断，决定是否允许消费任务上下文的 per-CPU preload。 */
#include <linux/radix-tree.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xarray.h>

#include "radix-tree.h"

/*
 * Radix tree node cache.
 */
/* 全局专用 slab cache；只管理 radix_tree_node，不管理用户存入的 item。 */
struct kmem_cache *radix_tree_node_cachep;

/*
 * The radix tree is variable-height, so an insert operation not only has
 * to build the branch to its corresponding item, it also has to build the
 * branch to existing items if the size has to be increased (by
 * radix_tree_extend).
 *
 * The worst case is a zero height tree with just a single item at index 0,
 * and then inserting an item at index ULONG_MAX. This requires 2 new branches
 * of RADIX_TREE_MAX_PATH size to be created, with only the root node shared.
 * Hence:
 */
/*
 * 树高扩展最坏需要同时为旧 entry 和新 ULONG_MAX 路径补两条分支，根层
 * 可共享，所以预加载上界为 2 * MAX_PATH - 1。
 */
#define RADIX_TREE_PRELOAD_SIZE (RADIX_TREE_MAX_PATH * 2 - 1)

/*
 * The IDR does not have to be as high as the radix tree since it uses
 * signed integers, not unsigned longs.
 */
/* IDR 只分配非负 int ID，路径位数和预加载上界可小于 unsigned long 树。 */
#define IDR_INDEX_BITS		(8 /* CHAR_BIT */ * sizeof(int) - 1)
/* 8 表示每字节位数；减去符号位后得到可分配的非负 int ID 位数。 */
#define IDR_MAX_PATH		(DIV_ROUND_UP(IDR_INDEX_BITS, \
						RADIX_TREE_MAP_SHIFT))
#define IDR_PRELOAD_SIZE	(IDR_MAX_PATH * 2 - 1)

/*
 * Per-cpu pool of preloaded nodes
 */
/*
 * 每 CPU 预加载栈。local_lock 同时保护链表并阻止任务迁移，保证之后的
 * 原子分配从同一 CPU 池消费；CPU 下线回调负责归还遗留节点。
 */
DEFINE_PER_CPU(struct radix_tree_preload, radix_tree_preloads) = {
	.lock = INIT_LOCAL_LOCK(lock),
};
EXPORT_PER_CPU_SYMBOL_GPL(radix_tree_preloads);

/* entry_to_node() - 清除 internal-node tag，得到借用节点裸指针。 */
static inline struct radix_tree_node *entry_to_node(void *ptr)
{
	return (void *)((unsigned long)ptr & ~RADIX_TREE_INTERNAL_NODE);
}

/* node_to_entry() - 给对齐节点地址加 internal tag，形成可存入 slot 的 entry。 */
static inline void *node_to_entry(void *ptr)
{
	return (void *)((unsigned long)ptr | RADIX_TREE_INTERNAL_NODE);
}

#define RADIX_TREE_RETRY	XA_RETRY_ENTRY
/*
 * RETRY 是给无锁 reader 的哨兵：表示所持 slot 已因扩缩树而过期，应从根
 * 重做查找，而不是把它当作用户 item。
 */

/* get_slot_offset() - 计算 slot 在 parent->slots 中的下标；根直接槽为 0。 */
static inline unsigned long
get_slot_offset(const struct radix_tree_node *parent, void __rcu **slot)
{
	return parent ? slot - parent->slots : 0;
}

/*
 * radix_tree_descend() - 用 parent->shift 取出 index 的本层 radix 数字。
 *
 * 通过 rcu_dereference_raw() 读取 child，写回 *nodep 并返回 slot 下标；
 * caller 已负责 RCU/锁条件以及遇到 RETRY 时的重走。
 */
static unsigned int radix_tree_descend(const struct radix_tree_node *parent,
			struct radix_tree_node **nodep, unsigned long index)
{
	unsigned int offset = (index >> parent->shift) & RADIX_TREE_MAP_MASK;
	void __rcu **entry = rcu_dereference_raw(parent->slots[offset]);

	*nodep = (void *)entry;
	return offset;
}

/* root_gfp_mask() - 从复用的 xa_flags 中提取该树允许的分配标志。 */
static inline gfp_t root_gfp_mask(const struct radix_tree_root *root)
{
	return root->xa_flags & (__GFP_BITS_MASK & ~GFP_ZONEMASK);
}

/* tag_set() - 设置节点某 tag 位；调用者负责向祖先传播聚合状态。 */
static inline void tag_set(struct radix_tree_node *node, unsigned int tag,
		int offset)
{
	__set_bit(offset, node->tags[tag]);
}

/* tag_clear() - 清除节点某 tag 位；调用者负责判断祖先摘要是否也要清除。 */
static inline void tag_clear(struct radix_tree_node *node, unsigned int tag,
		int offset)
{
	__clear_bit(offset, node->tags[tag]);
}

/* tag_get() - 测试节点某 slot 的 tag，仅返回当次读取结果。 */
static inline int tag_get(const struct radix_tree_node *node, unsigned int tag,
		int offset)
{
	return test_bit(offset, node->tags[tag]);
}

/* root_tag_set() - 在 xa_flags 高位设置“整棵树存在此 tag”的摘要。 */
static inline void root_tag_set(struct radix_tree_root *root, unsigned tag)
{
	root->xa_flags |= (__force gfp_t)(1 << (tag + ROOT_TAG_SHIFT));
}

/* root_tag_clear() - 清除根级 tag 摘要，表示全树已无对应 tag。 */
static inline void root_tag_clear(struct radix_tree_root *root, unsigned tag)
{
	root->xa_flags &= (__force gfp_t)~(1 << (tag + ROOT_TAG_SHIFT));
}

/* root_tag_clear_all() - 清空全部根 tag，同时保留低位 GFP/IDR 属性。 */
static inline void root_tag_clear_all(struct radix_tree_root *root)
{
	root->xa_flags &= (__force gfp_t)((1 << ROOT_TAG_SHIFT) - 1);
}

/* root_tag_get() - 读取根级 tag 摘要；并发修改下只是瞬时观察。 */
static inline int root_tag_get(const struct radix_tree_root *root, unsigned tag)
{
	return (__force int)root->xa_flags & (1 << (tag + ROOT_TAG_SHIFT));
}

/* root_tags_get() - 一次取得所有根 tag 位，主要用于一致性断言。 */
static inline unsigned root_tags_get(const struct radix_tree_root *root)
{
	return (__force unsigned)root->xa_flags >> ROOT_TAG_SHIFT;
}

/* is_idr() - 判断同一底层树是否采用允许 NULL item 的 IDR 计数语义。 */
static inline bool is_idr(const struct radix_tree_root *root)
{
	return !!(root->xa_flags & ROOT_IS_IDR);
}

/*
 * Returns 1 if any slot in the node has this tag set.
 * Otherwise returns 0.
 */
/* any_tag_set() - 扫描节点 tag bitmap，判断任一 child 子树是否仍带 tag。 */
static inline int any_tag_set(const struct radix_tree_node *node,
							unsigned int tag)
{
	unsigned idx;
	for (idx = 0; idx < RADIX_TREE_TAG_LONGS; idx++) {
		if (node->tags[tag][idx])
			return 1;
	}
	return 0;
}

/* all_tag_set() - 将本节点所有 slots 标为空闲/带 tag；IDR 新节点使用。 */
static inline void all_tag_set(struct radix_tree_node *node, unsigned int tag)
{
	bitmap_fill(node->tags[tag], RADIX_TREE_MAP_SIZE);
}

/**
 * radix_tree_find_next_bit - find the next set bit in a memory region
 *
 * @node: where to begin the search
 * @tag: the tag index
 * @offset: the bitnumber to start searching at
 *
 * Unrollable variant of find_next_bit() for constant size arrays.
 * Tail bits starting from size to roundup(size, BITS_PER_LONG) must be zero.
 * Returns next bit offset, or size if nothing found.
 */
/*
 * radix_tree_find_next_bit() - 在固定大小 tag bitmap 中寻找下一置位 bit。
 *
 * @node/@tag 选择位图，@offset 是包含式起点。返回 slot 下标；未找到返回
 * RADIX_TREE_MAP_SIZE。纯位图扫描，不改变树或 iterator。
 */
static __always_inline unsigned long
radix_tree_find_next_bit(struct radix_tree_node *node, unsigned int tag,
			 unsigned long offset)
{
	const unsigned long *addr = node->tags[tag];

	if (offset < RADIX_TREE_MAP_SIZE) {
		unsigned long tmp;

		addr += offset / BITS_PER_LONG;
		tmp = *addr >> (offset % BITS_PER_LONG);
		if (tmp)
			return __ffs(tmp) + offset;
		offset = (offset + BITS_PER_LONG) & ~(BITS_PER_LONG - 1);
		while (offset < RADIX_TREE_MAP_SIZE) {
			tmp = *++addr;
			if (tmp)
				return __ffs(tmp) + offset;
			offset += BITS_PER_LONG;
		}
	}
	return RADIX_TREE_MAP_SIZE;
}

/* iter_offset() - 从 iterator 全局 index 取得叶节点内 slot 下标。 */
static unsigned int iter_offset(const struct radix_tree_iter *iter)
{
	return iter->index & RADIX_TREE_MAP_MASK;
}

/*
 * The maximum index which can be stored in a radix tree
 */
/* shift_maxindex() - 计算根 shift 给定时整棵树可覆盖的最大 index。 */
static inline unsigned long shift_maxindex(unsigned int shift)
{
	return (RADIX_TREE_MAP_SIZE << shift) - 1;
}

/* node_maxindex() - 计算一个节点及其子树覆盖宽度对应的最大相对 index。 */
static inline unsigned long node_maxindex(const struct radix_tree_node *node)
{
	return shift_maxindex(node->shift);
}

/* next_index() - 把同层 offset 转换为其子树的全局起始 index。 */
static unsigned long next_index(unsigned long index,
				const struct radix_tree_node *node,
				unsigned long offset)
{
	return (index & ~node_maxindex(node)) + (offset << node->shift);
}

/*
 * This assumes that the caller has performed appropriate preallocation, and
 * that the caller has pinned this thread of control to the current CPU.
 */
/*
 * radix_tree_node_alloc() - 为写路径取得并初始化一个节点。
 *
 * @gfp_mask 决定是否可阻塞；@parent/@offset 建立反向链接；@root 是所属
 * 树；@shift 决定该层覆盖粒度；@count/@nr_values 是初始占用统计。
 * 原子且非中断上下文优先尝试带 memcg 记账的 slab，失败再消费当前 CPU
 * preload；可阻塞路径直接分配。返回新节点或 NULL，不发布到树。
 *
 * 调用者必须已经按需 preload 并由 local_lock 固定 CPU；函数不取得树锁。
 */
static struct radix_tree_node *
radix_tree_node_alloc(gfp_t gfp_mask, struct radix_tree_node *parent,
			struct radix_tree_root *root,
			unsigned int shift, unsigned int offset,
			unsigned int count, unsigned int nr_values)
{
	struct radix_tree_node *ret = NULL;

	/*
	 * Preload code isn't irq safe and it doesn't make sense to use
	 * preloading during an interrupt anyway as all the allocations have
	 * to be atomic. So just do normal allocation when in interrupt.
	 */
	/*
	 * preload 栈依赖任务上下文的 per-CPU local_lock，不供硬中断使用；
	 * 中断分配本就必须原子完成，因此直接尝试 slab。
	 */
	if (!gfpflags_allow_blocking(gfp_mask) && !in_interrupt()) {
		struct radix_tree_preload *rtp;

		/*
		 * Even if the caller has preloaded, try to allocate from the
		 * cache first for the new node to get accounted to the memory
		 * cgroup.
		 */
		/* 先走正常 slab，使成功分配正确计入当前 memcg。 */
		ret = kmem_cache_alloc(radix_tree_node_cachep,
				       gfp_mask | __GFP_NOWARN);
		if (ret)
			goto out;

		/*
		 * Provided the caller has preloaded here, we will always
		 * succeed in getting a node here (and never reach
		 * kmem_cache_alloc)
		 */
		/* 原子分配失败后弹出预加载栈；正确使用时该池保证成功。 */
		rtp = this_cpu_ptr(&radix_tree_preloads);
		if (rtp->nr) {
			ret = rtp->nodes;
			rtp->nodes = ret->parent;
			rtp->nr--;
		}
		/*
		 * Update the allocation stack trace as this is more useful
		 * for debugging.
		 */
		/* 节点被本次调用消费时更新 kmemleak 栈，便于定位所有者。 */
		kmemleak_update_trace(ret);
		goto out;
	}
	ret = kmem_cache_alloc(radix_tree_node_cachep, gfp_mask);
out:
	BUG_ON(radix_tree_is_internal_node(ret));
	if (ret) {
		ret->shift = shift;
		ret->offset = offset;
		ret->count = count;
		ret->nr_values = nr_values;
		ret->parent = parent;
		ret->array = root;
	}
	return ret;
}

/*
 * radix_tree_node_rcu_free() - RCU 宽限期后清理并归还一个摘除节点。
 *
 * @head 内嵌于节点；callback 取得 ownership 后清空 slots/tags 和私有链表，
 * 再归还 slab。此时旧 RCU reader 已退出，但叶 item 从不由本函数释放。
 */
void radix_tree_node_rcu_free(struct rcu_head *head)
{
	struct radix_tree_node *node =
			container_of(head, struct radix_tree_node, rcu_head);

	/*
	 * Must only free zeroed nodes into the slab.  We can be left with
	 * non-NULL entries by radix_tree_free_nodes, so clear the entries
	 * and tags here.
	 */
	/*
	 * bulk destroy 为保护旧 reader 会保留非 NULL slots；slab 构造/复用要求
	 * 干净节点，所以只能在宽限期结束后的 callback 中统一清零。
	 */
	memset(node->slots, 0, sizeof(node->slots));
	memset(node->tags, 0, sizeof(node->tags));
	INIT_LIST_HEAD(&node->private_list);

	kmem_cache_free(radix_tree_node_cachep, node);
}

/* radix_tree_node_free() - 将已摘除节点交给 call_rcu() 延迟回收。 */
static inline void
radix_tree_node_free(struct radix_tree_node *node)
{
	call_rcu(&node->rcu_head, radix_tree_node_rcu_free);
}

/*
 * Load up this CPU's radix_tree_node buffer with sufficient objects to
 * ensure that the addition of a single element in the tree cannot fail.  On
 * success, return zero, with preemption disabled.  On error, return -ENOMEM
 * with preemption not disabled.
 *
 * To make use of this facility, the radix tree must be initialised without
 * __GFP_DIRECT_RECLAIM being passed to INIT_RADIX_TREE().
 */
/*
 * __radix_tree_preload() - 为当前 CPU 准备 @nr 个紧急节点。
 *
 * 成功返回 0 且保持 radix_tree_preloads.lock，使任务不会迁移；调用者必须
 * 调用 preload_end。失败返回 -ENOMEM 且不保留锁。分配时暂时释放
 * local_lock 以允许睡眠，重获后若其他任务已补满则释放多余节点。
 */
static __must_check int __radix_tree_preload(gfp_t gfp_mask, unsigned nr)
{
	struct radix_tree_preload *rtp;
	struct radix_tree_node *node;
	int ret = -ENOMEM;

	/*
	 * Nodes preloaded by one cgroup can be used by another cgroup, so
	 * they should never be accounted to any particular memory cgroup.
	 */
	/* per-CPU 池可跨 memcg 消费，预加载节点不能记账给当前 cgroup。 */
	gfp_mask &= ~__GFP_ACCOUNT;

	local_lock(&radix_tree_preloads.lock);
	rtp = this_cpu_ptr(&radix_tree_preloads);
	while (rtp->nr < nr) {
		local_unlock(&radix_tree_preloads.lock);
		node = kmem_cache_alloc(radix_tree_node_cachep, gfp_mask);
		if (node == NULL)
			goto out;
		local_lock(&radix_tree_preloads.lock);
		rtp = this_cpu_ptr(&radix_tree_preloads);
		if (rtp->nr < nr) {
			node->parent = rtp->nodes;
			rtp->nodes = node;
			rtp->nr++;
		} else {
			kmem_cache_free(radix_tree_node_cachep, node);
		}
	}
	ret = 0;
out:
	return ret;
}

/*
 * Load up this CPU's radix_tree_node buffer with sufficient objects to
 * ensure that the addition of a single element in the tree cannot fail.  On
 * success, return zero, with preemption disabled.  On error, return -ENOMEM
 * with preemption not disabled.
 *
 * To make use of this facility, the radix tree must be initialised without
 * __GFP_DIRECT_RECLAIM being passed to INIT_RADIX_TREE().
 */
/*
 * radix_tree_preload() - 为一次最坏插入强制准备完整节点上界。
 *
 * @gfp_mask 必须允许阻塞。成功 0/失败 -ENOMEM；成功后仍禁止迁移，调用者
 * 在完成原子写入后必须调用 radix_tree_preload_end()。
 */
int radix_tree_preload(gfp_t gfp_mask)
{
	/* Warn on non-sensical use... */
	/* 不可阻塞 mask 无法提前睡眠分配，属于错误用法。 */
	WARN_ON_ONCE(!gfpflags_allow_blocking(gfp_mask));
	return __radix_tree_preload(gfp_mask, RADIX_TREE_PRELOAD_SIZE);
}
EXPORT_SYMBOL(radix_tree_preload);

/*
 * The same as above function, except we don't guarantee preloading happens.
 * We do it, if we decide it helps. On success, return zero with preemption
 * disabled. On error, return -ENOMEM with preemption not disabled.
 */
/*
 * radix_tree_maybe_preload() - 仅在 gfp 允许阻塞时实际预加载。
 *
 * 原子 mask 下直接取得 local_lock 并返回成功，因为 preload 对这种分配
 * 无帮助；两种成功路径都要求调用者执行 preload_end。
 */
int radix_tree_maybe_preload(gfp_t gfp_mask)
{
	if (gfpflags_allow_blocking(gfp_mask))
		return __radix_tree_preload(gfp_mask, RADIX_TREE_PRELOAD_SIZE);
	/* Preloading doesn't help anything with this gfp mask, skip it */
	/* 仍取得 local_lock，以保持与 preload_end 配对的 API 契约。 */
	local_lock(&radix_tree_preloads.lock);
	return 0;
}
EXPORT_SYMBOL(radix_tree_maybe_preload);

/*
 * radix_tree_load_root() - 读取根并返回当前树高的 shift 表示。
 *
 * 输出 *nodep 为原始根 entry，*maxindex 为当前容量上界；直接根/空树返回
 * shift 0，内部节点返回 node->shift + 一层位数。借用指针，不取得引用。
 */
static unsigned radix_tree_load_root(const struct radix_tree_root *root,
		struct radix_tree_node **nodep, unsigned long *maxindex)
{
	struct radix_tree_node *node = rcu_dereference_raw(root->xa_head);

	*nodep = node;

	if (likely(radix_tree_is_internal_node(node))) {
		node = entry_to_node(node);
		*maxindex = node_maxindex(node);
		return node->shift + RADIX_TREE_MAP_SHIFT;
	}

	*maxindex = 0;
	return 0;
}

/*
 *	Extend a radix tree so it can store key @index.
 */
/*
 * radix_tree_extend() - 在根上方逐层加节点，使容量覆盖 @index。
 *
 * @shift 是旧树高度表示，@gfp 用于新节点；返回新的总 shift，失败返回
 * -ENOMEM。旧根始终放入新根 slot 0，并传播普通 tag 或 IDR_FREE 摘要。
 * 每层完成初始化后通过 rcu_assign_pointer(root->xa_head) 发布。
 */
static int radix_tree_extend(struct radix_tree_root *root, gfp_t gfp,
				unsigned long index, unsigned int shift)
{
	void *entry;
	unsigned int maxshift;
	int tag;

	/* Figure out what the shift should be.  */
	/* 先只计算目标高度，避免分配过程中反复判断容量。 */
	maxshift = shift;
	while (index > shift_maxindex(maxshift))
		maxshift += RADIX_TREE_MAP_SHIFT;

	entry = rcu_dereference_raw(root->xa_head);
	if (!entry && (!is_idr(root) || root_tag_get(root, IDR_FREE)))
		goto out;

	do {
		struct radix_tree_node *node = radix_tree_node_alloc(gfp, NULL,
							root, shift, 0, 1, 0);
		if (!node)
			return -ENOMEM;

		if (is_idr(root)) {
			all_tag_set(node, IDR_FREE);
			if (!root_tag_get(root, IDR_FREE)) {
				tag_clear(node, IDR_FREE, 0);
				root_tag_set(root, IDR_FREE);
			}
		} else {
			/* Propagate the aggregated tag info to the new child */
			/* 新根 slot 0 包含旧树，必须继承全部 tag 摘要。 */
			for (tag = 0; tag < RADIX_TREE_MAX_TAGS; tag++) {
				if (root_tag_get(root, tag))
					tag_set(node, tag, 0);
			}
		}

		BUG_ON(shift > BITS_PER_LONG);
		if (radix_tree_is_internal_node(entry)) {
			entry_to_node(entry)->parent = node;
		} else if (xa_is_value(entry)) {
			/* Moving a value entry root->xa_head to a node */
			/* 直接根 value 下沉后，首个节点的 value 计数为 1。 */
			node->nr_values = 1;
		}
		/*
		 * entry was already in the radix tree, so we do not need
		 * rcu_assign_pointer here
		 */
		/*
		 * node 尚未发布，写其 slot 0 无需 RCU 发布；真正的可见性边界是
		 * 随后的 root->xa_head rcu_assign_pointer()。
		 */
		node->slots[0] = (void __rcu *)entry;
		entry = node_to_entry(node);
		rcu_assign_pointer(root->xa_head, entry);
		shift += RADIX_TREE_MAP_SHIFT;
	} while (shift <= maxshift);
out:
	return maxshift + RADIX_TREE_MAP_SHIFT;
}

/**
 *	radix_tree_shrink    -    shrink radix tree to minimum height
 *	@root:		radix tree root
 */
/*
 * radix_tree_shrink() - 反复移除只有 slot 0 一个 child 的冗余根层。
 *
 * 调用者持写锁；返回是否至少缩过一层。新根先接管 child，再把旧根标成
 * RETRY 并 call_rcu()，使持有旧叶 slot 的无锁 reader 从根重走。
 * IDR 底层叶不会缩成直接根，以免 NULL/internal 编码语义产生歧义。
 */
static inline bool radix_tree_shrink(struct radix_tree_root *root)
{
	bool shrunk = false;

	for (;;) {
		struct radix_tree_node *node = rcu_dereference_raw(root->xa_head);
		struct radix_tree_node *child;

		if (!radix_tree_is_internal_node(node))
			break;
		node = entry_to_node(node);

		/*
		 * The candidate node has more than one child, or its child
		 * is not at the leftmost slot, we cannot shrink.
		 */
		/* 只有唯一且位于 slot 0 的 child 才能保持所有 index 不变。 */
		if (node->count != 1)
			break;
		child = rcu_dereference_raw(node->slots[0]);
		if (!child)
			break;

		/*
		 * For an IDR, we must not shrink entry 0 into the root in
		 * case somebody calls idr_replace() with a pointer that
		 * appears to be an internal entry
		 */
		/* IDR 允许特殊值/NULL，保留叶节点可避免编码歧义。 */
		if (!node->shift && is_idr(root))
			break;

		if (radix_tree_is_internal_node(child))
			entry_to_node(child)->parent = NULL;

		/*
		 * We don't need rcu_assign_pointer(), since we are simply
		 * moving the node from one part of the tree to another: if it
		 * was safe to dereference the old pointer to it
		 * (node->slots[0]), it will be safe to dereference the new
		 * one (root->xa_head) as far as dependent read barriers go.
		 */
		/* child 已发布过；这里只移动指针，依赖读取顺序仍成立。 */
		root->xa_head = (void __rcu *)child;
		if (is_idr(root) && !tag_get(node, IDR_FREE, 0))
			root_tag_clear(root, IDR_FREE);

		/*
		 * We have a dilemma here. The node's slot[0] must not be
		 * NULLed in case there are concurrent lookups expecting to
		 * find the item. However if this was a bottom-level node,
		 * then it may be subject to the slot pointer being visible
		 * to callers dereferencing it. If item corresponding to
		 * slot[0] is subsequently deleted, these callers would expect
		 * their slot to become empty sooner or later.
		 *
		 * For example, lockless pagecache will look up a slot, deref
		 * the page pointer, and if the page has 0 refcount it means it
		 * was concurrently deleted from pagecache so try the deref
		 * again. Fortunately there is already a requirement for logic
		 * to retry the entire slot lookup -- the indirect pointer
		 * problem (replacing direct root node with an indirect pointer
		 * also results in a stale slot). So tag the slot as indirect
		 * to force callers to retry.
		 */
		/*
		 * 旧节点不能立即清空 slot 0，否则旧 reader 会把暂时 NULL
		 * 当作真实缺失；但保留直接 item 又可能让外部持有陈旧 slot。
		 * RETRY 要求 reader 重新查根，随后 RCU 延迟释放旧节点。
		 */
		node->count = 0;
		if (!radix_tree_is_internal_node(child)) {
			node->slots[0] = (void __rcu *)RADIX_TREE_RETRY;
		}

		WARN_ON_ONCE(!list_empty(&node->private_list));
		radix_tree_node_free(node);
		shrunk = true;
	}

	return shrunk;
}

/*
 * delete_node() - 从空节点向根递归摘除，并在可能时收缩树高。
 *
 * @node 必须已更新 count。返回是否有节点被摘除/收缩；每个摘除节点通过
 * call_rcu() 延迟释放。函数不释放叶 item。
 */
static bool delete_node(struct radix_tree_root *root,
			struct radix_tree_node *node)
{
	bool deleted = false;

	do {
		struct radix_tree_node *parent;

		if (node->count) {
			if (node_to_entry(node) ==
					rcu_dereference_raw(root->xa_head))
				deleted |= radix_tree_shrink(root);
			return deleted;
		}

		parent = node->parent;
		if (parent) {
			parent->slots[node->offset] = NULL;
			parent->count--;
		} else {
			/*
			 * Shouldn't the tags already have all been cleared
			 * by the caller?
			 */
			/* 普通树删除最后节点时清根 tag；IDR 保留 FREE 语义。 */
			if (!is_idr(root))
				root_tag_clear_all(root);
			root->xa_head = NULL;
		}

		WARN_ON_ONCE(!list_empty(&node->private_list));
		radix_tree_node_free(node);
		deleted = true;

		node = parent;
	} while (node);

	return deleted;
}

/**
 *	__radix_tree_create	-	create a slot in a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *	@nodep:		returns node
 *	@slotp:		returns slot
 *
 *	Create, if necessary, and return the node and slot for an item
 *	at position @index in the radix tree @root.
 *
 *	Until there is more than one item in the tree, no nodes are
 *	allocated and @root->xa_head is used as a direct slot instead of
 *	pointing to a node, in which case *@nodep will be NULL.
 *
 *	Returns -ENOMEM, or 0 for success.
 */
/*
 * __radix_tree_create() - 确保 @index 对应路径存在，并返回叶 node/slot。
 *
 * @root 是输入输出树；@nodep/@slotp 可为 NULL，非 NULL 时接收借用位置。
 * 空树或单索引树可直接使用 root->xa_head，此时 *nodep==NULL。函数先扩高，
 * 再逐层补节点并通过 RCU 发布；成功 0，分配失败 -ENOMEM。调用者持写锁，
 * 且若分配不能睡眠应事先 preload。
 */
static int __radix_tree_create(struct radix_tree_root *root,
		unsigned long index, struct radix_tree_node **nodep,
		void __rcu ***slotp)
{
	struct radix_tree_node *node = NULL, *child;
	void __rcu **slot = (void __rcu **)&root->xa_head;
	unsigned long maxindex;
	unsigned int shift, offset = 0;
	unsigned long max = index;
	gfp_t gfp = root_gfp_mask(root);

	shift = radix_tree_load_root(root, &child, &maxindex);

	/* Make sure the tree is high enough.  */
	/* 阶段 1：必要时在旧根上方加层，使容量覆盖 index。 */
	if (max > maxindex) {
		int error = radix_tree_extend(root, gfp, max, shift);
		if (error < 0)
			return error;
		shift = error;
		child = rcu_dereference_raw(root->xa_head);
	}

	while (shift > 0) {
		shift -= RADIX_TREE_MAP_SHIFT;
		if (child == NULL) {
			/* Have to add a child node.  */
			/* 阶段 2：分配、初始化并发布缺失的中间 child。 */
			child = radix_tree_node_alloc(gfp, node, root, shift,
							offset, 0, 0);
			if (!child)
				return -ENOMEM;
			rcu_assign_pointer(*slot, node_to_entry(child));
			if (node)
				node->count++;
		} else if (!radix_tree_is_internal_node(child))
			break;

		/* Go a level down */
		/* 依据 index 的本层 radix 数字进入下一层。 */
		node = entry_to_node(child);
		offset = radix_tree_descend(node, &child, index);
		slot = &node->slots[offset];
	}

	if (nodep)
		*nodep = node;
	if (slotp)
		*slotp = slot;
	return 0;
}

/*
 * Free any nodes below this node.  The tree is presumed to not need
 * shrinking, and any user data in the tree is presumed to not need a
 * destructor called on it.  If we need to add a destructor, we can
 * add that functionality later.  Note that we may not clear tags or
 * slots from the tree as an RCU walker may still have a pointer into
 * this subtree.  We could replace the entries with RADIX_TREE_RETRY,
 * but we'll still have to clear those in rcu_free.
 */
/*
 * radix_tree_free_nodes() - 后序摘除一棵无需收缩的完整内部子树。
 *
 * 不调用用户 item 析构，也不清 slot/tag：旧 RCU reader 可能仍在子树内。
 * 节点逐个交给 call_rcu()，最终 callback 再清零。调用者已使根不可达。
 */
static void radix_tree_free_nodes(struct radix_tree_node *node)
{
	unsigned offset = 0;
	struct radix_tree_node *child = entry_to_node(node);

	for (;;) {
		void *entry = rcu_dereference_raw(child->slots[offset]);
		if (xa_is_node(entry) && child->shift) {
			child = entry_to_node(entry);
			offset = 0;
			continue;
		}
		offset++;
		while (offset == RADIX_TREE_MAP_SIZE) {
			struct radix_tree_node *old = child;
			offset = child->offset + 1;
			child = child->parent;
			WARN_ON_ONCE(!list_empty(&old->private_list));
			radix_tree_node_free(old);
			if (old == entry_to_node(node))
				return;
		}
	}
}

/*
 * insert_entries() - 仅在空 slot 中发布 item，并维护节点计数。
 *
 * 已占用返回 -EEXIST；成功通过 rcu_assign_pointer() 发布并返回 1。直接
 * 根 node==NULL 时没有节点统计。item ownership 不转移给树。
 */
static inline int insert_entries(struct radix_tree_node *node,
		void __rcu **slot, void *item)
{
	if (*slot)
		return -EEXIST;
	rcu_assign_pointer(*slot, item);
	if (node) {
		node->count++;
		if (xa_is_value(item))
			node->nr_values++;
	}
	return 1;
}

/**
 *	radix_tree_insert    -    insert into a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *	@item:		item to insert
 *
 *	Insert an item into the radix tree at position @index.
 */
/*
 * radix_tree_insert() - 在 @index 为空时插入 @item。
 *
 * 拒绝内部节点编码；先创建路径，再原子发布 item。返回 0、-EEXIST 或
 * -ENOMEM。调用者必须持写锁，并管理 item 生命周期；函数不替换旧 entry。
 * 新 entry 的全部 tags 初始必须为清除状态，末尾 BUG_ON 验证该不变量。
 */
int radix_tree_insert(struct radix_tree_root *root, unsigned long index,
			void *item)
{
	struct radix_tree_node *node;
	void __rcu **slot;
	int error;

	BUG_ON(radix_tree_is_internal_node(item));

	error = __radix_tree_create(root, index, &node, &slot);
	if (error)
		return error;

	error = insert_entries(node, slot, item);
	if (error < 0)
		return error;

	if (node) {
		unsigned offset = get_slot_offset(node, slot);
		BUG_ON(tag_get(node, 0, offset));
		BUG_ON(tag_get(node, 1, offset));
		BUG_ON(tag_get(node, 2, offset));
	} else {
		BUG_ON(root_tags_get(root));
	}

	return 0;
}
EXPORT_SYMBOL(radix_tree_insert);

/**
 *	__radix_tree_lookup	-	lookup an item in a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *	@nodep:		returns node
 *	@slotp:		returns slot
 *
 *	Lookup and return the item at position @index in the radix
 *	tree @root.
 *
 *	Until there is more than one item in the tree, no nodes are
 *	allocated and @root->xa_head is used as a direct slot instead of
 *	pointing to a node, in which case *@nodep will be NULL.
 */
/*
 * __radix_tree_lookup() - 精确查找 index，并可返回承载它的 node 和 slot。
 *
 * @nodep/@slotp 是可选输出，均为借用指针；找到返回 item，未找到返回
 * NULL。直接根时 node 为 NULL、slot 指向 xa_head。RCU reader 遇到
 * RADIX_TREE_RETRY 会从根重走；函数不稳定叶 item 的外部生命周期。
 */
void *__radix_tree_lookup(const struct radix_tree_root *root,
			  unsigned long index, struct radix_tree_node **nodep,
			  void __rcu ***slotp)
{
	struct radix_tree_node *node, *parent;
	unsigned long maxindex;
	void __rcu **slot;

 restart:
	/* RETRY 表示树高变化使旧路径失效，所有局部位置必须重新建立。 */
	parent = NULL;
	slot = (void __rcu **)&root->xa_head;
	radix_tree_load_root(root, &node, &maxindex);
	if (index > maxindex)
		return NULL;

	while (radix_tree_is_internal_node(node)) {
		unsigned offset;

		parent = entry_to_node(node);
		offset = radix_tree_descend(parent, &node, index);
		slot = parent->slots + offset;
		if (node == RADIX_TREE_RETRY)
			goto restart;
		if (parent->shift == 0)
			break;
	}

	if (nodep)
		*nodep = parent;
	if (slotp)
		*slotp = slot;
	return node;
}

/**
 *	radix_tree_lookup_slot    -    lookup a slot in a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *
 *	Returns:  the slot corresponding to the position @index in the
 *	radix tree @root. This is useful for update-if-exists operations.
 *
 *	This function can be called under rcu_read_lock iff the slot is not
 *	modified by radix_tree_replace_slot, otherwise it must be called
 *	exclusive from other writers. Any dereference of the slot must be done
 *	using radix_tree_deref_slot.
 */
/*
 * radix_tree_lookup_slot() - 返回 index 对应的 slot 地址，供“存在则更新”。
 *
 * 返回的是树内部位置而非 item。只读 RCU 使用时须用
 * radix_tree_deref_slot() 解引用；若要随后修改，查找到替换全程必须排除
 * 其他 writer，否则节点可能被删除而使 slot 地址失效。
 */
void __rcu **radix_tree_lookup_slot(const struct radix_tree_root *root,
				unsigned long index)
{
	void __rcu **slot;

	if (!__radix_tree_lookup(root, index, NULL, &slot))
		return NULL;
	return slot;
}
EXPORT_SYMBOL(radix_tree_lookup_slot);

/**
 *	radix_tree_lookup    -    perform lookup operation on a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *
 *	Lookup the item at the position @index in the radix tree @root.
 *
 *	This function can be called under rcu_read_lock, however the caller
 *	must manage lifetimes of leaf nodes (eg. RCU may also be used to free
 *	them safely). No RCU barriers are required to access or modify the
 *	returned item, however.
 */
/*
 * radix_tree_lookup() - 精确返回 index 对应 item 的简单包装。
 *
 * 可在 RCU 读段调用，但 RCU 仅保护树节点；叶 item 的引用/释放协议由
 * 调用者负责。返回借用 item 或 NULL，不修改树。
 */
void *radix_tree_lookup(const struct radix_tree_root *root, unsigned long index)
{
	return __radix_tree_lookup(root, index, NULL, NULL);
}
EXPORT_SYMBOL(radix_tree_lookup);

/*
 * replace_slot() - 调整 node 统计后，以 RCU 语义发布 slot 新值。
 *
 * @count/@values 是调用者算好的增量；node 可为 NULL（直接根）。无返回，
 * 不释放旧 item。
 */
static void replace_slot(void __rcu **slot, void *item,
		struct radix_tree_node *node, int count, int values)
{
	if (node && (count || values)) {
		node->count += count;
		node->nr_values += values;
	}

	rcu_assign_pointer(*slot, item);
}

/* node_tag_get() - 统一读取普通节点 tag 或直接根的根级 tag。 */
static bool node_tag_get(const struct radix_tree_root *root,
				const struct radix_tree_node *node,
				unsigned int tag, unsigned int offset)
{
	if (node)
		return tag_get(node, tag, offset);
	return root_tag_get(root, tag);
}

/*
 * IDR users want to be able to store NULL in the tree, so if the slot isn't
 * free, don't adjust the count, even if it's transitioning between NULL and
 * non-NULL.  For the IDA, we mark slots as being IDR_FREE while they still
 * have empty bits, but it only stores NULL in slots when they're being
 * deleted.
 */
/*
 * IDR 允许“已占用但 item 为 NULL”，所以 count 不能简单按指针是否为空
 * 计算；IDR_FREE tag 才是 slot 是否空闲的权威。普通树返回新旧非空状态
 * 的差，IDR 已占用 slot 在 NULL/非 NULL 间切换时返回 0。
 */
static int calculate_count(struct radix_tree_root *root,
				struct radix_tree_node *node, void __rcu **slot,
				void *item, void *old)
{
	if (is_idr(root)) {
		unsigned offset = get_slot_offset(node, slot);
		bool free = node_tag_get(root, node, IDR_FREE, offset);
		if (!free)
			return 0;
		if (!old)
			return 1;
	}
	return !!item - !!old;
}

/**
 * __radix_tree_replace		- replace item in a slot
 * @root:		radix tree root
 * @node:		pointer to tree node
 * @slot:		pointer to slot in @node
 * @item:		new item to store in the slot.
 *
 * For use with __radix_tree_lookup().  Caller must hold tree write locked
 * across slot lookup and replacement.
 */
/*
 * __radix_tree_replace() - 在已知 node/slot 上替换、插入或删除 entry。
 *
 * 调用者须从 lookup 到替换始终持写锁。函数计算 count/value 变化，RCU
 * 发布新 item，并在删除后尝试回收空节点。旧 item 返回给上层语义处理，
 * 本函数不释放它；无直接返回值。
 */
void __radix_tree_replace(struct radix_tree_root *root,
			  struct radix_tree_node *node,
			  void __rcu **slot, void *item)
{
	void *old = rcu_dereference_raw(*slot);
	int values = !!xa_is_value(item) - !!xa_is_value(old);
	int count = calculate_count(root, node, slot, item, old);

	/*
	 * This function supports replacing value entries and
	 * deleting entries, but that needs accounting against the
	 * node unless the slot is root->xa_head.
	 */
	/* 非直接根的任意类型变化都必须有 node 承载并更新统计。 */
	WARN_ON_ONCE(!node && (slot != (void __rcu **)&root->xa_head) &&
			(count || values));
	replace_slot(slot, item, node, count, values);

	if (!node)
		return;

	delete_node(root, node);
}

/**
 * radix_tree_replace_slot	- replace item in a slot
 * @root:	radix tree root
 * @slot:	pointer to slot
 * @item:	new item to store in the slot.
 *
 * For use with radix_tree_lookup_slot() and
 * radix_tree_gang_lookup_tag_slot().  Caller must hold tree write locked
 * across slot lookup and replacement.
 *
 * NOTE: This cannot be used to switch between non-entries (empty slots),
 * regular entries, and value entries, as that requires accounting
 * inside the radix tree node. When switching from one type of entry or
 * deleting, use __radix_tree_lookup() and __radix_tree_replace() or
 * radix_tree_iter_replace().
 */
/*
 * radix_tree_replace_slot() - 用预先取得的 slot 替换同类型 entry。
 *
 * 调用者持写锁，且不得借此在 empty/普通/value 三类之间切换，因为这里
 * 没有 node 上下文可可靠修正计数。无返回，不接管新旧 item。
 */
void radix_tree_replace_slot(struct radix_tree_root *root,
			     void __rcu **slot, void *item)
{
	__radix_tree_replace(root, NULL, slot, item);
}
EXPORT_SYMBOL(radix_tree_replace_slot);

/**
 * radix_tree_iter_replace - replace item in a slot
 * @root:	radix tree root
 * @iter:	iterator state
 * @slot:	pointer to slot
 * @item:	new item to store in the slot.
 *
 * For use with radix_tree_for_each_slot().
 * Caller must hold tree write locked.
 */
/*
 * radix_tree_iter_replace() - 使用 iterator 保存的 node 完成可计数的替换。
 *
 * 适用于遍历中替换/删除；调用者持写锁，slot 必须属于 iter 当前节点。
 */
void radix_tree_iter_replace(struct radix_tree_root *root,
				const struct radix_tree_iter *iter,
				void __rcu **slot, void *item)
{
	__radix_tree_replace(root, iter->node, slot, item);
}

/*
 * node_tag_set() - 从叶 slot 向根设置 tag 聚合链。
 *
 * 某层位已设置说明祖先摘要已存在，可提前停止；到根后设置 root tag。
 */
static void node_tag_set(struct radix_tree_root *root,
				struct radix_tree_node *node,
				unsigned int tag, unsigned int offset)
{
	while (node) {
		if (tag_get(node, tag, offset))
			return;
		tag_set(node, tag, offset);
		offset = node->offset;
		node = node->parent;
	}

	if (!root_tag_get(root, tag))
		root_tag_set(root, tag);
}

/**
 *	radix_tree_tag_set - set a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag:		tag index
 *
 *	Set the search tag (which must be < RADIX_TREE_MAX_TAGS)
 *	corresponding to @index in the radix tree.  From
 *	the root all the way down to the leaf node.
 *
 *	Returns the address of the tagged item.  Setting a tag on a not-present
 *	item is a bug.
 */
/*
 * radix_tree_tag_set() - 给已存在 index 设置 tag，并沿路径建立聚合摘要。
 *
 * @tag 必须小于上限，index 必须存在；违反条件是调用者 bug。调用者持
 * 写锁。返回被标记的借用 item，不改变 item ownership。
 */
void *radix_tree_tag_set(struct radix_tree_root *root,
			unsigned long index, unsigned int tag)
{
	struct radix_tree_node *node, *parent;
	unsigned long maxindex;

	radix_tree_load_root(root, &node, &maxindex);
	BUG_ON(index > maxindex);

	while (radix_tree_is_internal_node(node)) {
		unsigned offset;

		parent = entry_to_node(node);
		offset = radix_tree_descend(parent, &node, index);
		BUG_ON(!node);

		if (!tag_get(parent, tag, offset))
			tag_set(parent, tag, offset);
	}

	/* set the root's tag bit */
	/* 根摘要让 tagged 查询可在空结果时 O(1) 提前结束。 */
	if (!root_tag_get(root, tag))
		root_tag_set(root, tag);

	return node;
}
EXPORT_SYMBOL(radix_tree_tag_set);

/*
 * node_tag_clear() - 清叶 tag，并仅在本层已无该 tag 时继续向根清摘要。
 */
static void node_tag_clear(struct radix_tree_root *root,
				struct radix_tree_node *node,
				unsigned int tag, unsigned int offset)
{
	while (node) {
		if (!tag_get(node, tag, offset))
			return;
		tag_clear(node, tag, offset);
		if (any_tag_set(node, tag))
			return;

		offset = node->offset;
		node = node->parent;
	}

	/* clear the root's tag bit */
	/* 所有层都无该 tag 后，根摘要也必须清除。 */
	if (root_tag_get(root, tag))
		root_tag_clear(root, tag);
}

/**
 *	radix_tree_tag_clear - clear a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag:		tag index
 *
 *	Clear the search tag (which must be < RADIX_TREE_MAX_TAGS)
 *	corresponding to @index in the radix tree.  If this causes
 *	the leaf node to have no tags set then clear the tag in the
 *	next-to-leaf node, etc.
 *
 *	Returns the address of the tagged item on success, else NULL.  ie:
 *	has the same return value and semantics as radix_tree_lookup().
 */
/*
 * radix_tree_tag_clear() - 清除 index 的 tag，并按需向祖先撤销摘要。
 *
 * index 越界或无 item 返回 NULL；成功返回借用 item。调用者持写锁；
 * 清 tag 不删除 item，也不改变其生命周期。
 */
void *radix_tree_tag_clear(struct radix_tree_root *root,
			unsigned long index, unsigned int tag)
{
	struct radix_tree_node *node, *parent;
	unsigned long maxindex;
	int offset = 0;

	radix_tree_load_root(root, &node, &maxindex);
	if (index > maxindex)
		return NULL;

	parent = NULL;

	while (radix_tree_is_internal_node(node)) {
		parent = entry_to_node(node);
		offset = radix_tree_descend(parent, &node, index);
	}

	if (node)
		node_tag_clear(root, parent, tag, offset);

	return node;
}
EXPORT_SYMBOL(radix_tree_tag_clear);

/**
  * radix_tree_iter_tag_clear - clear a tag on the current iterator entry
  * @root: radix tree root
  * @iter: iterator state
  * @tag: tag to clear
  */
/*
 * radix_tree_iter_tag_clear() - 利用 iterator 当前 node/offset 清除 tag。
 *
 * 调用者持写锁并保证 iterator 未失效；无返回，只修改 tag 聚合状态。
 */
void radix_tree_iter_tag_clear(struct radix_tree_root *root,
			const struct radix_tree_iter *iter, unsigned int tag)
{
	node_tag_clear(root, iter->node, tag, iter_offset(iter));
}

/**
 * radix_tree_tag_get - get a tag on a radix tree node
 * @root:		radix tree root
 * @index:		index key
 * @tag:		tag index (< RADIX_TREE_MAX_TAGS)
 *
 * Return values:
 *
 *  0: tag not present or not set
 *  1: tag set
 *
 * Note that the return value of this function may not be relied on, even if
 * the RCU lock is held, unless tag modification and node deletion are excluded
 * from concurrency.
 */
/*
 * radix_tree_tag_get() - 查询 index 的 tag 聚合路径是否置位。
 *
 * 返回 0/1。即使持 RCU 锁，若 writer 可并发修改 tag 或删除节点，结果也
 * 只是瞬时观察，不能作为后续稳定前提；确定语义时须排除 writer。
 */
int radix_tree_tag_get(const struct radix_tree_root *root,
			unsigned long index, unsigned int tag)
{
	struct radix_tree_node *node, *parent;
	unsigned long maxindex;

	if (!root_tag_get(root, tag))
		return 0;

	radix_tree_load_root(root, &node, &maxindex);
	if (index > maxindex)
		return 0;

	while (radix_tree_is_internal_node(node)) {
		unsigned offset;

		parent = entry_to_node(node);
		offset = radix_tree_descend(parent, &node, index);

		if (!tag_get(parent, tag, offset))
			return 0;
		if (node == RADIX_TREE_RETRY)
			break;
	}

	return 1;
}
EXPORT_SYMBOL(radix_tree_tag_get);

/* Construct iter->tags bit-mask from node->tags[tag] array */
/* 从 node 的 tag bitmap 构造 iterator 当前 machine-word 的 tag 掩码。 */
/*
 * set_iter_tags() - 缓存从 offset 开始的 tagged slots，并裁剪 chunk 终点。
 *
 * node==NULL 表示直接根，唯一 slot 视为命中。跨两个 bitmap word 时拼接
 * 位段，使 radix_tree_next_slot() 可逐 bit 消费。
 */
static void set_iter_tags(struct radix_tree_iter *iter,
				struct radix_tree_node *node, unsigned offset,
				unsigned tag)
{
	unsigned tag_long = offset / BITS_PER_LONG;
	unsigned tag_bit  = offset % BITS_PER_LONG;

	if (!node) {
		iter->tags = 1;
		return;
	}

	iter->tags = node->tags[tag][tag_long] >> tag_bit;

	/* This never happens if RADIX_TREE_TAG_LONGS == 1 */
	/* 单 word 节点无需跨 word 拼接或额外裁剪。 */
	if (tag_long < RADIX_TREE_TAG_LONGS - 1) {
		/* Pick tags from next element */
		/* offset 非 word 边界时，把下一 word 的低位接到高位。 */
		if (tag_bit)
			iter->tags |= node->tags[tag][tag_long + 1] <<
						(BITS_PER_LONG - tag_bit);
		/* Clip chunk size, here only BITS_PER_LONG tags */
		/* tags 缓存只有一个 machine word，下一 chunk 从其后开始。 */
		iter->next_index = __radix_tree_iter_add(iter, BITS_PER_LONG);
	}
}

/*
 * radix_tree_iter_resume() - 外部暂离迭代后，使 iterator 从当前项之后重启。
 *
 * @slot 仅用于匹配迭代 API，此处不解引用；更新 index/next_index、清 tag
 * 缓存并返回 NULL，促使 for-each 宏重新调用 next_chunk()。
 */
void __rcu **radix_tree_iter_resume(void __rcu **slot,
					struct radix_tree_iter *iter)
{
	iter->index = __radix_tree_iter_add(iter, 1);
	iter->next_index = iter->index;
	iter->tags = 0;
	return NULL;
}
EXPORT_SYMBOL(radix_tree_iter_resume);

/**
 * radix_tree_next_chunk - find next chunk of slots for iteration
 *
 * @root:	radix tree root
 * @iter:	iterator state
 * @flags:	RADIX_TREE_ITER_* flags and tag index
 * Returns:	pointer to chunk first slot, or NULL if iteration is over
 */
/*
 * radix_tree_next_chunk() - 从 iter->next_index 查找下一批可线性扫描的 slots。
 *
 * @flags 可选择 tagged/contiguous 以及 tag 编号。成功返回借用 slot 数组
 * 起点，并设置 iter 的 index、next_index、node/tags；结束返回 NULL。
 * 可在 RCU 下读取，遇到 RETRY 或空洞按模式重启/停止，不产生原子快照。
 */
void __rcu **radix_tree_next_chunk(const struct radix_tree_root *root,
			     struct radix_tree_iter *iter, unsigned flags)
{
	unsigned tag = flags & RADIX_TREE_ITER_TAG_MASK;
	struct radix_tree_node *node, *child;
	unsigned long index, offset, maxindex;

	if ((flags & RADIX_TREE_ITER_TAGGED) && !root_tag_get(root, tag))
		return NULL;

	/*
	 * Catch next_index overflow after ~0UL. iter->index never overflows
	 * during iterating; it can be zero only at the beginning.
	 * And we cannot overflow iter->next_index in a single step,
	 * because RADIX_TREE_MAP_SHIFT < BITS_PER_LONG.
	 *
	 * This condition also used by radix_tree_next_slot() to stop
	 * contiguous iterating, and forbid switching to the next chunk.
	 */
	/*
	 * next_index 从 ULONG_MAX 加一会回到 0；只有 index 非 0 且 next_index
	 * 为 0 才表示真实溢出。连续迭代也依赖此边界禁止跨 chunk。
	 */
	index = iter->next_index;
	if (!index && iter->index)
		return NULL;

 restart:
	/* 树高变化、整层无候选或 RETRY 后，从当前全局 index 重新查根。 */
	radix_tree_load_root(root, &child, &maxindex);
	if (index > maxindex)
		return NULL;
	if (!child)
		return NULL;

	if (!radix_tree_is_internal_node(child)) {
		/* Single-slot tree */
		/* 高度 0 的直接根作为仅含一个 slot 的 chunk 返回。 */
		iter->index = index;
		iter->next_index = maxindex + 1;
		iter->tags = 1;
		iter->node = NULL;
		return (void __rcu **)&root->xa_head;
	}

	do {
		node = entry_to_node(child);
		offset = radix_tree_descend(node, &child, index);

		if ((flags & RADIX_TREE_ITER_TAGGED) ?
				!tag_get(node, tag, offset) : !child) {
			/* Hole detected */
			/* 连续模式遇洞结束；普通模式在同层找下一候选。 */
			if (flags & RADIX_TREE_ITER_CONTIG)
				return NULL;

			if (flags & RADIX_TREE_ITER_TAGGED)
				offset = radix_tree_find_next_bit(node, tag,
						offset + 1);
			else
				while (++offset	< RADIX_TREE_MAP_SIZE) {
					void *slot = rcu_dereference_raw(
							node->slots[offset]);
					if (slot)
						break;
				}
			index &= ~node_maxindex(node);
			index += offset << node->shift;
			/* Overflow after ~0UL */
			/* 跳到下一子树时检查全局 index 算术回绕。 */
			if (!index)
				return NULL;
			if (offset == RADIX_TREE_MAP_SIZE)
				goto restart;
			child = rcu_dereference_raw(node->slots[offset]);
		}

		if (!child)
			goto restart;
		if (child == RADIX_TREE_RETRY)
			break;
	} while (node->shift && radix_tree_is_internal_node(child));

	/* Update the iterator state */
	/* 保存本 chunk 首项及下一 chunk 边界，供 next_slot 宏继续消费。 */
	iter->index = (index &~ node_maxindex(node)) | offset;
	iter->next_index = (index | node_maxindex(node)) + 1;
	iter->node = node;

	if (flags & RADIX_TREE_ITER_TAGGED)
		set_iter_tags(iter, node, offset, tag);

	return node->slots + offset;
}
EXPORT_SYMBOL(radix_tree_next_chunk);

/**
 *	radix_tree_gang_lookup - perform multiple lookup on a radix tree
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *
 *	Performs an index-ascending scan of the tree for present items.  Places
 *	them at *@results and returns the number of items which were placed at
 *	*@results.
 *
 *	The implementation is naive.
 *
 *	Like radix_tree_lookup, radix_tree_gang_lookup may be called under
 *	rcu_read_lock. In this case, rather than the returned results being
 *	an atomic snapshot of the tree at a single point in time, the
 *	semantics of an RCU protected gang lookup are as though multiple
 *	radix_tree_lookups have been issued in individual locks, and results
 *	stored in 'results'.
 */
/*
 * radix_tree_gang_lookup() - 从 first_index 起批量收集最多 max_items 个 item。
 *
 * results 是调用者提供的输出数组；返回实际数量。RCU 下每项分别有效，
 * 整批不是同一时刻的原子快照。遇到内部 RETRY entry 会让 iterator 重走；
 * 返回的叶 item 生命周期仍由调用者保证。
 */
unsigned int
radix_tree_gang_lookup(const struct radix_tree_root *root, void **results,
			unsigned long first_index, unsigned int max_items)
{
	struct radix_tree_iter iter;
	void __rcu **slot;
	unsigned int ret = 0;

	if (unlikely(!max_items))
		return 0;

	radix_tree_for_each_slot(slot, root, &iter, first_index) {
		results[ret] = rcu_dereference_raw(*slot);
		if (!results[ret])
			continue;
		if (radix_tree_is_internal_node(results[ret])) {
			slot = radix_tree_iter_retry(&iter);
			continue;
		}
		if (++ret == max_items)
			break;
	}

	return ret;
}
EXPORT_SYMBOL(radix_tree_gang_lookup);

/**
 *	radix_tree_gang_lookup_tag - perform multiple lookup on a radix tree
 *	                             based on a tag
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *	@tag:		the tag index (< RADIX_TREE_MAX_TAGS)
 *
 *	Performs an index-ascending scan of the tree for present items which
 *	have the tag indexed by @tag set.  Places the items at *@results and
 *	returns the number of items which were placed at *@results.
 */
/*
 * radix_tree_gang_lookup_tag() - 批量返回带指定 tag 的非空 items。
 *
 * 输出和 RCU 快照/ownership 约定与 gang_lookup 相同，tag 摘要用于跳过
 * 不含候选的子树。返回写入 results 的数量。
 */
unsigned int
radix_tree_gang_lookup_tag(const struct radix_tree_root *root, void **results,
		unsigned long first_index, unsigned int max_items,
		unsigned int tag)
{
	struct radix_tree_iter iter;
	void __rcu **slot;
	unsigned int ret = 0;

	if (unlikely(!max_items))
		return 0;

	radix_tree_for_each_tagged(slot, root, &iter, first_index, tag) {
		results[ret] = rcu_dereference_raw(*slot);
		if (!results[ret])
			continue;
		if (radix_tree_is_internal_node(results[ret])) {
			slot = radix_tree_iter_retry(&iter);
			continue;
		}
		if (++ret == max_items)
			break;
	}

	return ret;
}
EXPORT_SYMBOL(radix_tree_gang_lookup_tag);

/**
 *	radix_tree_gang_lookup_tag_slot - perform multiple slot lookup on a
 *					  radix tree based on a tag
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *	@tag:		the tag index (< RADIX_TREE_MAX_TAGS)
 *
 *	Performs an index-ascending scan of the tree for present items which
 *	have the tag indexed by @tag set.  Places the slots at *@results and
 *	returns the number of slots which were placed at *@results.
 */
/*
 * radix_tree_gang_lookup_tag_slot() - 批量返回带 tag entry 的内部 slot 地址。
 *
 * slot 比 item 更易因 writer 删除节点而失效；调用者必须保持相应 RCU
 * 或写锁保护，并使用规定的 slot 解引用/替换接口。返回 slot 数量。
 */
unsigned int
radix_tree_gang_lookup_tag_slot(const struct radix_tree_root *root,
		void __rcu ***results, unsigned long first_index,
		unsigned int max_items, unsigned int tag)
{
	struct radix_tree_iter iter;
	void __rcu **slot;
	unsigned int ret = 0;

	if (unlikely(!max_items))
		return 0;

	radix_tree_for_each_tagged(slot, root, &iter, first_index, tag) {
		results[ret] = slot;
		if (++ret == max_items)
			break;
	}

	return ret;
}
EXPORT_SYMBOL(radix_tree_gang_lookup_tag_slot);

/*
 * __radix_tree_delete() - 清除一个已定位 slot，并维护 tag、计数和节点树高。
 *
 * IDR 删除把 IDR_FREE 重新向根传播；普通树清除所有 tags。随后发布 NULL，
 * 递减 count/value 统计，并删除空节点。返回 iterator 当前 node 是否可能
 * 被释放，旧 item 由调用者在调用前取得。
 */
static bool __radix_tree_delete(struct radix_tree_root *root,
				struct radix_tree_node *node, void __rcu **slot)
{
	void *old = rcu_dereference_raw(*slot);
	int values = xa_is_value(old) ? -1 : 0;
	unsigned offset = get_slot_offset(node, slot);
	int tag;

	if (is_idr(root))
		node_tag_set(root, node, IDR_FREE, offset);
	else
		for (tag = 0; tag < RADIX_TREE_MAX_TAGS; tag++)
			node_tag_clear(root, node, tag, offset);

	replace_slot(slot, NULL, node, -1, values);
	return node && delete_node(root, node);
}

/**
 * radix_tree_iter_delete - delete the entry at this iterator position
 * @root: radix tree root
 * @iter: iterator state
 * @slot: pointer to slot
 *
 * Delete the entry at the position currently pointed to by the iterator.
 * This may result in the current node being freed; if it is, the iterator
 * is advanced so that it will not reference the freed memory.  This
 * function may be called without any locking if there are no other threads
 * which can access this tree.
 */
/*
 * radix_tree_iter_delete() - 删除 iterator 当前 slot，并避免游标引用已释放节点。
 *
 * 若删除导致节点摘除，把 index 推到 next_index，下一轮会重新定位。并发
 * 存在并发访问时必须持写锁；单线程私有树可无锁。不释放 item。
 */
void radix_tree_iter_delete(struct radix_tree_root *root,
				struct radix_tree_iter *iter, void __rcu **slot)
{
	if (__radix_tree_delete(root, iter->node, slot))
		iter->index = iter->next_index;
}
EXPORT_SYMBOL(radix_tree_iter_delete);

/**
 * radix_tree_delete_item - delete an item from a radix tree
 * @root: radix tree root
 * @index: index key
 * @item: expected item
 *
 * Remove @item at @index from the radix tree rooted at @root.
 *
 * Return: the deleted entry, or %NULL if it was not present
 * or the entry at the given @index was not @item.
 */
/*
 * radix_tree_delete_item() - 条件删除 index：可要求当前 entry 等于 @item。
 *
 * @item 为 NULL 表示不做指针匹配。返回被摘除的借用 entry；缺失或不匹配
 * 返回 NULL。IDR 用 IDR_FREE 区分“占用的 NULL”与空槽。调用者持写锁，
 * 并负责返回 item 的后续释放。
 */
void *radix_tree_delete_item(struct radix_tree_root *root,
			     unsigned long index, void *item)
{
	struct radix_tree_node *node = NULL;
	void __rcu **slot = NULL;
	void *entry;

	entry = __radix_tree_lookup(root, index, &node, &slot);
	if (!slot)
		return NULL;
	if (!entry && (!is_idr(root) || node_tag_get(root, node, IDR_FREE,
						get_slot_offset(node, slot))))
		return NULL;

	if (item && entry != item)
		return NULL;

	__radix_tree_delete(root, node, slot);

	return entry;
}
EXPORT_SYMBOL(radix_tree_delete_item);

/**
 * radix_tree_delete - delete an entry from a radix tree
 * @root: radix tree root
 * @index: index key
 *
 * Remove the entry at @index from the radix tree rooted at @root.
 *
 * Return: The deleted entry, or %NULL if it was not present.
 */
/* radix_tree_delete() - 不校验期望 item 的单索引删除包装。 */
void *radix_tree_delete(struct radix_tree_root *root, unsigned long index)
{
	return radix_tree_delete_item(root, index, NULL);
}
EXPORT_SYMBOL(radix_tree_delete);

/**
 *	radix_tree_tagged - test whether any items in the tree are tagged
 *	@root:		radix tree root
 *	@tag:		tag to test
 */
/*
 * radix_tree_tagged() - O(1) 查询根摘要中是否存在任一指定 tag。
 *
 * 返回非零/零；并发 writer 下只是瞬时结果。
 */
int radix_tree_tagged(const struct radix_tree_root *root, unsigned int tag)
{
	return root_tag_get(root, tag);
}
EXPORT_SYMBOL(radix_tree_tagged);

/**
 * idr_preload - preload for idr_alloc()
 * @gfp_mask: allocation mask to use for preloading
 *
 * Preallocate memory to use for the next call to idr_alloc().  This function
 * returns with preemption disabled.  It will be enabled by idr_preload_end().
 */
/*
 * idr_preload() - 为下一次 idr_alloc() 准备 IDR 最坏路径节点。
 *
 * @gfp_mask 决定可否睡眠。无直接返回值；即使内部分配失败，也取得
 * local_lock 以保持 API 的“返回时禁止迁移”契约，调用者最终必须调用
 * idr_preload_end()。真正 alloc 仍可能报告 -ENOMEM。
 */
void idr_preload(gfp_t gfp_mask)
{
	if (__radix_tree_preload(gfp_mask, IDR_PRELOAD_SIZE))
		local_lock(&radix_tree_preloads.lock);
}
EXPORT_SYMBOL(idr_preload);

/*
 * idr_get_free() - 利用 IDR_FREE tag 查找/创建不大于 @max 的空闲 slot。
 *
 * @iter->next_index 是包含式起点，成功后更新 index、next_index、node 和
 * tags，并返回借用 slot；失败返回 ERR_PTR(-ENOSPC/-ENOMEM)。新节点所有
 * slots 初始标 FREE，沿路径选择下一置位 tag。调用者持 IDR 写锁，若 gfp
 * 不可阻塞则应已 preload。
 */
void __rcu **idr_get_free(struct radix_tree_root *root,
			      struct radix_tree_iter *iter, gfp_t gfp,
			      unsigned long max)
{
	struct radix_tree_node *node = NULL, *child;
	void __rcu **slot = (void __rcu **)&root->xa_head;
	unsigned long maxindex, start = iter->next_index;
	unsigned int shift, offset = 0;

 grow:
	/* 阶段 1：根摘要无空位时跳过现有容量，必要时扩高。 */
	shift = radix_tree_load_root(root, &child, &maxindex);
	if (!radix_tree_tagged(root, IDR_FREE))
		start = max(start, maxindex + 1);
	if (start > max)
		return ERR_PTR(-ENOSPC);

	if (start > maxindex) {
		int error = radix_tree_extend(root, gfp, start, shift);
		if (error < 0)
			return ERR_PTR(error);
		shift = error;
		child = rcu_dereference_raw(root->xa_head);
	}
	if (start == 0 && shift == 0)
		shift = RADIX_TREE_MAP_SHIFT;

	while (shift) {
		shift -= RADIX_TREE_MAP_SHIFT;
		if (child == NULL) {
			/* Have to add a child node.  */
			/* 缺失子树全为空，新节点的所有 IDR_FREE 位均可置位。 */
			child = radix_tree_node_alloc(gfp, node, root, shift,
							offset, 0, 0);
			if (!child)
				return ERR_PTR(-ENOMEM);
			all_tag_set(child, IDR_FREE);
			rcu_assign_pointer(*slot, node_to_entry(child));
			if (node)
				node->count++;
		} else if (!radix_tree_is_internal_node(child))
			break;

		node = entry_to_node(child);
		offset = radix_tree_descend(node, &child, start);
		if (!tag_get(node, IDR_FREE, offset)) {
			/* 当前子树已满，在本层 bitmap 中寻找下一空闲子树。 */
			offset = radix_tree_find_next_bit(node, IDR_FREE,
							offset + 1);
			start = next_index(start, node, offset);
			if (start > max || start == 0)
				return ERR_PTR(-ENOSPC);
			while (offset == RADIX_TREE_MAP_SIZE) {
				/* 本层无空闲项，向上找下一分支。 */
				offset = node->offset + 1;
				node = node->parent;
				if (!node)
					goto grow;
				shift = node->shift;
			}
			child = rcu_dereference_raw(node->slots[offset]);
		}
		slot = &node->slots[offset];
	}

	iter->index = start;
	if (node)
		iter->next_index = 1 + min(max, (start | node_maxindex(node)));
	else
		iter->next_index = 1;
	iter->node = node;
	set_iter_tags(iter, node, offset, IDR_FREE);

	return slot;
}

/**
 * idr_destroy - release all internal memory from an IDR
 * @idr: idr handle
 *
 * After this function is called, the IDR is empty, and may be reused or
 * the data structure containing it may be freed.
 *
 * A typical clean-up sequence for objects stored in an idr tree will use
 * idr_for_each() to free all objects, if necessary, then idr_destroy() to
 * free the memory used to keep track of those objects.
 */
/*
 * idr_destroy() - 释放 IDR 全部内部节点并恢复“空 IDR”根状态。
 *
 * 调用者应先自行遍历释放存储对象；本函数只销毁索引结构。节点通过 RCU
 * 延迟释放，根立即置 NULL，并设置 IDR_FREE 表示从 0 起可分配。调用者
 * 负责排除并发访问；返回后 idr 可复用或其容器可释放。
 */
void idr_destroy(struct idr *idr)
{
	struct radix_tree_node *node = rcu_dereference_raw(idr->idr_rt.xa_head);
	if (radix_tree_is_internal_node(node))
		radix_tree_free_nodes(node);
	idr->idr_rt.xa_head = NULL;
	root_tag_set(&idr->idr_rt, IDR_FREE);
}
EXPORT_SYMBOL(idr_destroy);

/*
 * radix_tree_node_ctor() - slab 新对象构造器：清零节点并初始化私有链表。
 *
 * 入参 @arg 是 slab 提供的新节点；无返回，不发布节点。
 */
static void
radix_tree_node_ctor(void *arg)
{
	struct radix_tree_node *node = arg;

	memset(node, 0, sizeof(*node));
	INIT_LIST_HEAD(&node->private_list);
}

// preload 机制：为了避免在持锁的临界区内分配内存（可能睡眠），radix tree 允许调用者提前在非临界区分配好节点，存入 per-cpu 的 radix_tree_preloads 池。CPU 下线时这些预分配的节点必须归还给 slab，否则内存泄漏。
/*
 * radix_tree_cpu_dead() - 释放下线 CPU 的预加载节点池。
 *
 * @cpu 是正在下线 CPU 的编号。回调逐个弹出其 preload 栈并
 * 直接归还 slab；热插拔框架已隔离该 CPU，不会再有本地消费者并发访问。
 * 返回 0 表示清理完成。
 */
static int radix_tree_cpu_dead(unsigned int cpu)
{
	struct radix_tree_preload *rtp;
	struct radix_tree_node *node;

	/* Free per-cpu pool of preloaded nodes */
	/* 释放该 CPU 尚未被写操作消费的全部预加载节点。 */
	rtp = &per_cpu(radix_tree_preloads, cpu);
	while (rtp->nr) {
		node = rtp->nodes;
		rtp->nodes = node->parent;
		kmem_cache_free(radix_tree_node_cachep, node);
		rtp->nr--;
	}
	return 0;
}

/*
 * radix_tree_init() - 启动期建立节点 slab，并注册 CPU 下线清理回调。
 *
 * 入参：无。返回：无直接返回值；slab 创建失败因 SLAB_PANIC 终止启动，
 * CPUHP 注册失败仅 WARN。BUILD_BUG_ON 在编译期验证 flags/tag/字段宽度
 * 可安全共存。初始化完成后全局 cache 可供 Radix Tree 与 IDR 使用。
 */
void __init radix_tree_init(void)
{
	int ret;

	// 编译时断言（BUILD_BUG_ON），检查常量约束，若不满足直接编译失败，运行时零开销，只是防止配置错误。
	BUILD_BUG_ON(RADIX_TREE_MAX_TAGS + __GFP_BITS_SHIFT > 32);
	BUILD_BUG_ON(ROOT_IS_IDR & ~GFP_ZONEMASK);
	BUILD_BUG_ON(XA_CHUNK_SIZE > 255);
	// 为 struct radix_tree_node 创建专用 slab 缓存
	// SLAB_RECLAIM_ACCOUNT：将这类对象纳入内存回收统计，内存压力大时可以被计入可回收内存
	// radix_tree_node_ctor：构造函数，每次从 slab 分配新对象时调用，将节点清零并初始化 private_list 链表头，避免使用未初始化内存
	radix_tree_node_cachep = kmem_cache_create("radix_tree_node",
			sizeof(struct radix_tree_node), 0,
			SLAB_PANIC | SLAB_RECLAIM_ACCOUNT,
			radix_tree_node_ctor);
	// 注册 CPU 下线回调
	// 向 CPU 热插拔状态机注册一个回调：当某个 CPU 下线时，调用 radix_tree_cpu_dead() 释放该 CPU 的预加载节点池（per-cpu preload pool）。
	// preload 机制：为了避免在持锁的临界区内分配内存（可能睡眠），radix tree 允许调用者提前在非临界区分配好节点，存入 per-cpu 的 radix_tree_preloads 池。CPU 下线时这些预分配的节点必须归还给 slab，否则内存泄漏。
	ret = cpuhp_setup_state_nocalls(CPUHP_RADIX_DEAD, "lib/radix:dead",
					NULL, radix_tree_cpu_dead);
	WARN_ON(ret < 0);
}
