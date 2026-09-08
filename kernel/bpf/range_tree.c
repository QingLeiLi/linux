// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */
#include <linux/interval_tree_generic.h>
#include <linux/slab.h>
#include <linux/bpf.h>
#include "range_tree.h"

/*
 * struct range_tree is a data structure used to allocate contiguous memory
 * ranges in bpf arena. It's a large bitmap. The contiguous sequence of bits is
 * represented by struct range_node or 'rn' for short.
 * rn->rn_rbnode links it into an interval tree while
 * rn->rb_range_size links it into a second rbtree sorted by size of the range.
 * __find_range() performs binary search and best fit algorithm to find the
 * range less or equal requested size.
 * range_tree_clear/set() clears or sets a range of bits in this bitmap. The
 * adjacent ranges are merged or split at the same time.
 *
 * The split/merge logic is based/borrowed from XFS's xbitmap32 added
 * in commit 6772fcc8890a ("xfs: convert xbitmap to interval tree").
 *
 * The implementation relies on external lock to protect rbtree-s.
 * The alloc/free of range_node-s is done via kmalloc_nolock().
 *
 * bpf arena is using range_tree to represent unallocated slots.
 * At init time:
 *   range_tree_set(rt, 0, max);
 * Then:
 *   start = range_tree_find(rt, len);
 *   if (start >= 0)
 *     range_tree_clear(rt, start, len);
 * to find free range and mark slots as allocated and later:
 *   range_tree_set(rt, start, len);
 * to mark as unallocated after use.
 */
/*
 * range_tree 实现学习导读
 *
 * 本结构等价于稀疏“大位图”：存在于树中的 [rn_start,rn_last] 表示对应 arena
 * 页槽为 1/空闲。每个节点同时进入地址 interval tree 与长度排序树；任何修改
 * 必须先从两个索引摘除，改完端点后再同时插回，否则增强字段、长度排序和地址
 * 查询会彼此矛盾。clear() 把位清零并可能切割节点，set() 把位置一并合并邻段。
 *
 * 原英文说明指出实现借鉴 XFS xbitmap32（提交 6772fcc8890a），依赖外部锁保护
 * 两棵红黑树，并用 kmalloc_nolock()/kfree_nolock() 管理节点。arena 初始化时把
 * [0,max) 全部 set；分配在同一锁内 find 后 clear，释放则 set 回去。
 *
 * 修正说明：原文称 __find_range() 寻找“小于等于请求大小”的区间，与当前代码
 * 不符。搜索条件是 `len <= rn_size(rn)`，实际选择“大小至少为 len 的最小区间”，
 * 即 best fit；否则找到的区间无法容纳请求。
 */
struct range_node {
	/* rn_rbnode 挂地址 interval tree；rb_range_size 把同一对象挂长度树，不能同时复用一个 rb_node。 */
	struct rb_node rn_rbnode;
	struct rb_node rb_range_size;
	/* rn_start/rn_last 是含首含尾的 u32 槽编号，因此长度为 last-start+1。 */
	u32 rn_start;
	u32 rn_last; /* inclusive */
	/* 原行尾英文说明 last 为闭区间端点；__rn_subtree_last 是 interval-tree 宏维护的子树最大末端。 */
	u32 __rn_subtree_last;
};

/*
 * rb_to_range_node() - 从长度树链接恢复所属 range_node。
 * 业务背景：长度树遍历拿到的是嵌入 rb_node，比较区间大小前需 container_of 式还原对象。
 * 入参：@rb 为长度树中非 NULL、仍链接的借用节点。
 * 出参/返回：返回借用的所属 range_node，不增加引用或改变树。
 * 注意事项：只能传 rb_range_size 成员，误传 rn_rbnode 会计算错误地址；纯计算、不睡眠。
 */
static struct range_node *rb_to_range_node(struct rb_node *rb)
{
	return rb_entry(rb, struct range_node, rb_range_size);
}

/*
 * rn_size() - 计算闭区间节点包含的槽数。
 * 业务背景：长度索引的比较键和 best-fit 条件都使用同一长度定义。
 * 入参：@rn 为外锁稳定的非 NULL 借用节点，要求 rn_start<=rn_last。
 * 出参/返回：返回 `last-start+1` 个槽，无副作用。
 * 注意事项：端点不变量被破坏会导致 u32 环绕；函数自身不校验、不睡眠。
 */
static u32 rn_size(struct range_node *rn)
{
	return rn->rn_last - rn->rn_start + 1;
}

/* Find range that fits best to requested size */
/*
 * __find_range() - 在长度树中寻找可容纳 @len 的最小空闲节点。
 * 业务背景：arena 无固定地址分配用 best fit 降低外部碎片，再由 clear() 真正预留。
 * 入参：@rt 为外锁保护的借用树；@len 为非零槽数。
 * 出参/返回：返回借用节点或 NULL；只查询、不摘链、不转移 ownership。
 * 注意事项：原英文意为“寻找最适配请求长度的范围”；结果并未预留，锁不能提前释放。
 */
static inline struct range_node *__find_range(struct range_tree *rt, u32 len)
{
	/* rb 沿长度树下降；best 保存迄今最小的合格节点。 */
	struct rb_node *rb = rt->range_size_root.rb_root.rb_node;
	struct range_node *best = NULL;

	while (rb) {
		struct range_node *rn = rb_to_range_node(rb);

		if (len <= rn_size(rn)) {
			/* 当前可容纳请求；转向较小值所在的右子树，继续尝试减少浪费。 */
			best = rn;
			rb = rb->rb_right;
		} else {
			/* 当前过小，只能转向保存更大区间的左子树。 */
			rb = rb->rb_left;
		}
	}

	return best;
}

/*
 * range_tree_find() - 返回 best-fit 空闲段的起点。
 * 业务背景：公开包装隐藏节点布局；arena 随后在同一 spinlock 临界区调用 clear()。
 * 入参：@rt 为外锁保护的借用树；@len 是非零请求槽数。
 * 出参/返回：成功返回非负 u32 起点（提升为 s64），无合格节点返回 -ENOENT；树不变。
 * 注意事项：返回起点不持引用也不完成预留，调用者必须保持锁并检查 clear() 结果。
 */
s64 range_tree_find(struct range_tree *rt, u32 len)
{
	struct range_node *rn;

	rn = __find_range(rt, len);
	if (!rn)
		return -ENOENT;
	return rn->rn_start;
}

/* Insert the range into rbtree sorted by the range size */
/*
 * __range_size_insert() - 把新/已修改节点插入长度排序红黑树。
 * 业务背景：所有区间端点变化后都需重建长度索引，供 best-fit 对数搜索。
 * 入参：@rn 为尚未链接到长度树、ownership 仍属 rt 的节点；@root 为借用树根。
 * 出参/返回：无直接返回；发布 rn 到长度树并更新 cached leftmost。
 * 注意事项：调用者持外锁；较大长度走左、较小/相等走右，函数不分配、不睡眠。
 */
static inline void __range_size_insert(struct range_node *rn,
				       struct rb_root_cached *root)
{
	/* link/rb 是标准 rb 插入游标；size 为比较键；leftmost 跟踪是否插到最大值最左端。 */
	struct rb_node **link = &root->rb_root.rb_node, *rb = NULL;
	u64 size = rn_size(rn);
	bool leftmost = true;

	while (*link) {
		rb = *link;
		if (size > rn_size(rb_to_range_node(rb))) {
			/* 大区间放左侧，使搜索遇到过小节点时向左即可扩大候选。 */
			link = &rb->rb_left;
		} else {
			/* 小于或等于放右侧；相等项无需稳定次序，且不可能成为 cached leftmost。 */
			link = &rb->rb_right;
			leftmost = false;
		}
	}

	rb_link_node(&rn->rb_range_size, rb, link);
	rb_insert_color_cached(&rn->rb_range_size, root, leftmost);
}

/* interval-tree 生成器通过 START/LAST 读取含首含尾端点；宏只在本文件参与模板展开。 */
#define START(node) ((node)->rn_start)
#define LAST(node)  ((node)->rn_last)

/*
 * 该宏以 rn_rbnode、__rn_subtree_last 和 START/LAST 生成 __range_it_insert/remove/
 * iter_first 等静态 helper。增强字段保存每棵子树最大 rn_last，使相交查询可跳过
 * 不可能子树；它不生成锁，所有调用仍依赖 range_tree 的外部锁。
 */
INTERVAL_TREE_DEFINE(struct range_node, rn_rbnode, u32,
		     __rn_subtree_last, START, LAST,
		     static inline __maybe_unused,
		     __range_it)

/*
 * range_it_insert() - 把一个 range_node 同时发布到长度树和地址区间树。
 * 业务背景：集中维护双索引不变量，供新建、拆分和端点更新路径复用。
 * 入参：@rn 为两个 rb 链接均未入树的持有节点；@rt 为外锁保护的目标树。
 * 出参/返回：无直接返回；rt 接管节点的索引可见性，节点内存 ownership 不变。
 * 注意事项：任一链接已在树中都会破坏 rb 结构；不分配、不睡眠。
 */
static inline __maybe_unused void
range_it_insert(struct range_node *rn, struct range_tree *rt)
{
	__range_size_insert(rn, &rt->range_size_root);
	__range_it_insert(rn, &rt->it_root);
}

/*
 * range_it_remove() - 从两个索引同时摘除节点。
 * 业务背景：端点/长度修改和最终释放前必须先解除旧排序与增强字段关系。
 * 入参：@rn 必须同时属于 @rt 两棵树；两者均借用且由外锁稳定。
 * 出参/返回：无直接返回；节点不再可查，但内存仍归调用者，可修改重插或释放。
 * 注意事项：显式清除长度 rb_node 便于后续重插/调试；地址侧 helper 维护增强字段。
 */
static inline __maybe_unused void
range_it_remove(struct range_node *rn, struct range_tree *rt)
{
	rb_erase_cached(&rn->rb_range_size, &rt->range_size_root);
	RB_CLEAR_NODE(&rn->rb_range_size);
	__range_it_remove(rn, &rt->it_root);
}

/*
 * range_it_iter_first() - 查询首个与给定闭区间相交的地址节点。
 * 业务背景：clear/set/查询均以 interval tree 快速定位覆盖或相邻 free 段。
 * 入参：@rt 为外锁保护的借用树；@start/@last 是含首含尾 u32 查询端点。
 * 出参/返回：返回借用节点或 NULL，不增引用、不修改树。
 * 注意事项：结果只在外锁临界区稳定；start/last 可用 0/-1U 表示完整地址域。
 */
static inline __maybe_unused struct range_node *
range_it_iter_first(struct range_tree *rt, u32 start, u32 last)
{
	return __range_it_iter_first(&rt->it_root, start, last);
}

/* Clear the range in this range tree */
/*
 * range_tree_clear() - 从 free 集合删除一个闭区间，按相交形态裁剪/拆除节点。
 * 业务背景：arena 成功选择槽后把它标成已分配；地址树可能返回跨越请求的任意 free 段。
 * 入参：@rt 为调用者持 rqspinlock 稳定的借用树；@start 为页槽起点，@len 为非零槽数，
 *       调用者保证 `start+len-1` 不溢出。
 * 出参/返回：成功返回 0；仅“请求严格位于一个节点内部”的拆分需新节点，分配失败返回
 *             -ENOMEM。失败前左半已重插，右半未恢复，因而额外空间会变得不可用但不会重分配。
 * 注意事项：使用 nolock 分配，可在该 raw lock 上下文运行且不睡眠；所有端点修改前先双树摘链。
 */
int range_tree_clear(struct range_tree *rt, u32 start, u32 len)
{
	/* last 是含尾端点；new_rn 承接拆分右半，rn 为每轮首个相交 free 节点。 */
	u32 last = start + len - 1;
	struct range_node *new_rn;
	struct range_node *rn;

	/* 反复取首个相交节点，直至请求区间内再无 set/free 位。 */
	while ((rn = range_it_iter_first(rt, start, last))) {
		if (rn->rn_start < start && rn->rn_last > last) {
			/* 原节点严格包住整个 clear 区间，必须拆成互不相邻的左右两个 free 节点。 */
			u32 old_last = rn->rn_last;

			/* Overlaps with the entire clearing range */
			/* 请求完全位于节点内部：先摘旧节点，把其本体缩为左半 [old_start,start-1]。 */
			range_it_remove(rn, rt);
			rn->rn_last = start - 1;
			range_it_insert(rn, rt);

			/* Add a range */
			/* 再为右半 [last+1,old_last] 分配节点；不能睡眠或等待 allocator 锁。 */
			new_rn = kmalloc_nolock(sizeof(struct range_node), __GFP_ACCOUNT,
						NUMA_NO_NODE);
			/*
			 * 此时左半已经提交。失败时不把旧 rn 扩回去，否则会错误地把请求区间重新标 free；
			 * 右半暂不在树中，造成容量损失但保持“绝不重复分配”的安全方向。
			 */
			if (!new_rn)
				return -ENOMEM;
			new_rn->rn_start = last + 1;
			new_rn->rn_last = old_last;
			range_it_insert(new_rn, rt);
		} else if (rn->rn_start < start) {
			/* Overlaps with the left side of the clearing range */
			/* 节点从左侧伸入请求：保留左前缀并把 rn_last 裁到 start-1。 */
			range_it_remove(rn, rt);
			rn->rn_last = start - 1;
			range_it_insert(rn, rt);
		} else if (rn->rn_last > last) {
			/* Overlaps with the right side of the clearing range */
			/* 节点从请求右侧伸出：保留右后缀并把 rn_start 推到 last+1。 */
			range_it_remove(rn, rt);
			rn->rn_start = last + 1;
			range_it_insert(rn, rt);
			/* 已处理覆盖请求最右端的节点，之后不可能再有相交区间，可提前结束。 */
			break;
		} else {
			/* in the middle of the clearing range */
			/* 节点完全落在请求内部：从双索引摘除后释放，其全部槽都变为已分配。 */
			range_it_remove(rn, rt);
			kfree_nolock(rn);
		}
	}
	/* 没有相交节点也视为成功：clear 是幂等的“保证这些位为 0/不可用”操作。 */
	return 0;
}

/* Is the whole range set ? */
/*
 * is_range_tree_set() - 检查一个节点是否完整覆盖查询区间。
 * 业务背景：arena 固定地址分配/guard 前必须证明每个槽仍处于 free 集合。
 * 入参：@rt 为外锁保护的借用树；@start/@len 为非溢出的非空页槽区间。
 * 出参/返回：单个节点完整覆盖返回 0，否则 -ESRCH；不修改树或引用。
 * 注意事项：相邻 free 段按不变量已合并，故无需跨多个节点累计覆盖。
 */
int is_range_tree_set(struct range_tree *rt, u32 start, u32 len)
{
	/* left 是首个相交节点；只有其左右端点同时包住查询才算全 set。 */
	u32 last = start + len - 1;
	struct range_node *left;

	/* Is this whole range set ? */
	/* 原文问题意为“整个范围是否已 set”；部分相交不能满足固定地址分配。 */
	left = range_it_iter_first(rt, start, last);
	if (left && left->rn_start <= start && left->rn_last >= last)
		return 0;
	return -ESRCH;
}

/* Set the range in this range tree */
/*
 * range_tree_set() - 把区间加入 free 集合，并规范化为不相交、不相邻的最大节点。
 * 业务背景：arena 初始化、释放与分配回滚用它归还槽；重复 set 必须幂等，重叠/相邻段需合并。
 * 入参：@rt 为调用者持外部 rqspinlock 的借用树；@start 为槽起点，@len 为非零槽数，
 *       调用者保证端点及 `start-1/last+1` 探测不会命中地址域另一端的无关节点。
 * 出参/返回：已覆盖或成功合并/新建返回 0；clear 拆分失败或新节点失败返回 -ENOMEM；
 *             邻接查询违反规范不变量返回 -EFAULT。失败不保证恢复调用前 free 集合。
 * 注意事项：所有分配均为 nolock；修改端点前双树摘链，成功后两个索引重新一致。
 */
int range_tree_set(struct range_tree *rt, u32 start, u32 len)
{
	/* last 为含尾端点；left/right 保存相邻节点借用指针，err 传播 clear 的失败。 */
	u32 last = start + len - 1;
	struct range_node *right;
	struct range_node *left;
	int err;

	/* Is this whole range already set ? */
	/* 已由单节点完整覆盖时直接成功，避免无意义的摘链、分配和重插。 */
	left = range_it_iter_first(rt, start, last);
	if (left && left->rn_start <= start && left->rn_last >= last)
		return 0;

	/* Clear out everything in the range we want to set. */
	/*
	 * 先清掉请求内所有重叠节点，使后续只需处理严格左/右邻居；原文即“清除将要
	 * set 的范围内的一切”。此阶段可能拆分并失败，错误原样返回且不承诺回滚。
	 */
	err = range_tree_clear(rt, start, len);
	if (err)
		return err;

	/* Do we have a left-adjacent range ? */
	/* 查询 start-1 处节点；若存在，它在 clear 后应恰好以 start-1 结束，否则索引已损坏。 */
	left = range_it_iter_first(rt, start - 1, start - 1);
	if (left && left->rn_last + 1 != start)
		return -EFAULT;

	/* Do we have a right-adjacent range ? */
	/* 同理查询 last+1；存在节点必须恰好从该点开始。两个 -EFAULT 都是内部不变量诊断。 */
	right = range_it_iter_first(rt, last + 1, last + 1);
	if (right && right->rn_start != last + 1)
		return -EFAULT;

	if (left && right) {
		/* Combine left and right adjacent ranges */
		/* 左右均相邻：摘除两节点，把请求及 right 全并入 left，重插后释放多余 right。 */
		range_it_remove(left, rt);
		range_it_remove(right, rt);
		left->rn_last = right->rn_last;
		range_it_insert(left, rt);
		kfree_nolock(right);
	} else if (left) {
		/* Combine with the left range */
		/* 仅左邻：扩展其末端到 last；长度键变化要求摘除后重插。 */
		range_it_remove(left, rt);
		left->rn_last = last;
		range_it_insert(left, rt);
	} else if (right) {
		/* Combine with the right range */
		/* 仅右邻：把其起点向左拉到 start，同样重建两个索引。 */
		range_it_remove(right, rt);
		right->rn_start = start;
		range_it_insert(right, rt);
	} else {
		/* 无邻居：需要新节点独立表示该 free 区间；失败时区间继续保持 clear/不可用。 */
		left = kmalloc_nolock(sizeof(struct range_node), __GFP_ACCOUNT, NUMA_NO_NODE);
		if (!left)
			return -ENOMEM;
		left->rn_start = start;
		left->rn_last = last;
		range_it_insert(left, rt);
	}
	/* 成功出口保证请求区间与所有邻接 free 槽共同由一个最大节点表示。 */
	return 0;
}

/*
 * range_tree_destroy() - 释放地址树中仍持有的全部 range_node。
 * 业务背景：arena 销毁或构造失败回滚时闭合节点内存 ownership。
 * 入参：@rt 为已初始化、由调用者排除并发访问的借用树。
 * 出参/返回：无直接返回；每个节点从双索引摘除并用 kfree_nolock() 释放。
 * 注意事项：rt 本体不释放；完成后两树为空，但再次使用前仍应按生命周期重新 init/set。
 */
void range_tree_destroy(struct range_tree *rt)
{
	/* [0,-1U] 覆盖完整 u32 地址域；每轮取首节点，摘链后才可释放。 */
	struct range_node *rn;

	while ((rn = range_it_iter_first(rt, 0, -1U))) {
		range_it_remove(rn, rt);
		kfree_nolock(rn);
	}
}

/*
 * range_tree_init() - 把调用者提供的 range_tree 初始化为空双索引。
 * 业务背景：arena 构造后先建立空树，再用 set(0,max_entries) 发布全部 free 槽。
 * 入参：@rt 为非 NULL、尚未发布且由调用者拥有的输出对象。
 * 出参/返回：无直接返回；两个 cached rb 根置空，不分配节点。
 * 注意事项：不得对仍含节点的树调用，否则会泄漏；初始化期间无需外部锁，发布后需要。
 */
void range_tree_init(struct range_tree *rt)
{
	/* 两个根必须成对重置，维持“同一节点集合”的初始不变量。 */
	rt->it_root = RB_ROOT_CACHED;
	rt->range_size_root = RB_ROOT_CACHED;
}
