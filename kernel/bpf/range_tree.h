/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2024 Meta Platforms, Inc. and affiliates. */
#ifndef _RANGE_TREE_H
#define _RANGE_TREE_H 1

/*
 * range_tree 接口学习导读
 *
 * BPF arena 用它保存“尚未分配”的页槽区间，而不是已占用区间。每段闭区间同时
 * 挂入按地址组织的 interval tree 和按长度组织的 best-fit 红黑树：前者负责覆盖、
 * 拆分与相邻合并，后者快速找出最小的可容纳区间。两个索引必须始终包含同一批
 * range_node；节点由实现层 kmalloc_nolock() 创建并在删除/销毁时释放。
 *
 * 结构不内置锁。arena 调用者以 `bpf_arena.spinlock` 串行保护查询和修改；尤其
 * find() 只返回候选起点，不预留空间，必须与随后的 clear() 位于同一临界区。
 * 所有 start/len 单位都是页槽编号/数量，len 必须非零且 start+len-1 不得溢出。
 */
struct range_tree {
	/* root of interval tree */
	/* 地址区间树根：按 rn_start/rn_last 查相交节点，并缓存最左节点。 */
	struct rb_root_cached it_root;
	/* root of rbtree of interval sizes */
	/* 长度索引树根：按 rn_size 排序，供 best-fit 搜索；节点集合必须与 it_root 一致。 */
	struct rb_root_cached range_size_root;
};

/*
 * range_tree_init() - 初始化一个空范围树。
 * 业务背景：arena 建立 free-slot 索引前先初始化两个树根。
 * 入参：@rt 为调用者拥有的非 NULL 输出对象。
 * 出参/返回：无直接返回，不分配节点；两个根均变为空。
 * 注意事项：发布/使用前由调用者负责外部锁，最终用 destroy() 配对。
 */
void range_tree_init(struct range_tree *rt);
/*
 * range_tree_destroy() - 摘除并释放 @rt 的全部节点。
 * 业务背景：arena 销毁/构造回滚时闭合所有 range_node ownership。
 * 入参：@rt 为已初始化的借用对象；本体仍归调用者。
 * 出参/返回：无直接返回；两树全部清空并释放节点。
 * 注意事项：调用者须排除并发访问；nolock 释放不会自行获取 arena 锁。
 */
void range_tree_destroy(struct range_tree *rt);

/*
 * range_tree_clear() - 从 @rt 的 set/free 集合删除 [@start,@start+@len-1]。
 * 业务背景：arena 分配/guard 时把原 free 槽标成不可再次分配。
 * 入参：@rt 借用且由外锁保护；@start 为槽偏移，@len 为非零槽数。
 * 出参/返回：成功 0；拆分分配失败 -ENOMEM，树可能已经收缩旧区间。
 * 注意事项：start+len-1 不得溢出；失败时调用者必须把该空间视为不可用。
 */
int range_tree_clear(struct range_tree *rt, u32 start, u32 len);
/*
 * range_tree_set() - 把给定非零槽区间加入 free 集合并与左右邻段合并。
 * 业务背景：arena 释放页或失败回滚时让槽重新可分配。
 * 入参：@rt 借用且须外锁保护；@start 为槽偏移，@len 为非零槽数。
 * 出参/返回：成功/已覆盖 0，分配失败 -ENOMEM，索引异常 -EFAULT。
 * 注意事项：失败可能发生在清除重叠段之后，调用者不可假设状态完全回滚。
 */
int range_tree_set(struct range_tree *rt, u32 start, u32 len);
/*
 * is_range_tree_set() - 证明整个查询区间都属于同一 set/free 节点。
 * 业务背景：固定地址分配/guard 必须先确认全部槽仍为空闲。
 * 入参：@rt 为外锁保护的借用树；@start/@len 为非溢出的非空槽区间。
 * 出参/返回：全覆盖返回 0，否则 -ESRCH；不修改树或 ownership。
 * 注意事项：只接受单个节点完整覆盖；调用者须保持查询到后续修改原子化。
 */
int is_range_tree_set(struct range_tree *rt, u32 start, u32 len);
/*
 * range_tree_find() - best-fit 查找至少容纳 @len 个槽的 free 节点。
 * 业务背景：无固定地址的 arena 分配需选择最小可容纳空闲段。
 * 入参：@rt 是调用者持外锁稳定的借用树；@len 为非零槽数。
 * 出参/返回：返回非负起点或 -ENOENT；不修改树，也不预留结果。
 * 注意事项：须在同一临界区立即 clear()，否则并发者可能取得相同区间。
 */
s64 range_tree_find(struct range_tree *rt, u32 len);

#endif
