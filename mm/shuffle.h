// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2018 Intel Corporation. All rights reserved.
#ifndef _MM_SHUFFLE_H
#define _MM_SHUFFLE_H
#include <linux/jump_label.h>

/*
 * SHUFFLE_ORDER 是 page allocator 进行随机头/尾插入以及全区洗牌的 buddy 阶数，
 * 当前取 MAX_PAGE_ORDER。以较大连续块为粒度洗牌，后续拆分会把随机性传给小页，
 * 同时避免每个 order 都承担启动期遍历成本；单位是 order，即每块 2^order 页。
 */
#define SHUFFLE_ORDER MAX_PAGE_ORDER

#ifdef CONFIG_SHUFFLE_PAGE_ALLOCATOR
/*
 * page_alloc_shuffle_key 是运行期开关的 static key，定义于 shuffle.c；配置编译进
 * 能力并不自动开启，page_alloc.shuffle 参数置 true 后才 patch 热路径分支。
 * 调用者只查询该全局键，不拥有或释放它。
 */
DECLARE_STATIC_KEY_FALSE(page_alloc_shuffle_key);
/*
 * __shuffle_free_memory() - 对一个 NUMA node 的所有 zone 执行初始 freelist 洗牌。
 * 业务背景：启动内存统计稳定后由包装函数进入核心实现，降低页分配物理位置的
 * 可预测性并改善直映 memory-side cache 的平均利用率。
 * 入参：pgdat 是不可为 NULL 的借用 node 数据，所属 zones 已初始化且调用期间稳定。
 * 出参/返回：无直接返回值；可能重排各 zone 同 order、同 migratetype 的 free list，
 * 不改变空闲页数量、页面 ownership 或 migratetype。
 * 注意事项：__meminit 生命周期，只能在启动/内存初始化允许的阶段调用；实现会取
 * zone->lock、阶段性释放锁并 cond_resched()，因此不能从原子上下文进入。
 */
extern void __shuffle_free_memory(pg_data_t *pgdat);
/*
 * shuffle_pick_tail() - 为高阶 buddy 释放随机选择 freelist 头或尾。
 * 业务背景：__free_one_page() 在 is_shuffle_order() 命中时用随机位替代可预测的
 * 合并启发式，使后续分配顺序持续保持扰动。
 * 入参：无。
 * 出参/返回：返回一位伪随机布尔值；true 表示插到尾部，false 表示头部，无所有权变化。
 * 注意事项：实现故意不加锁，共享随机缓存竞态只增加熵；不能用于安全随机数或公平性。
 */
extern bool shuffle_pick_tail(void);
/*
 * shuffle_free_memory() - 以 static key 门控整 node 的启动期洗牌。
 * 业务背景：mm_init 可无条件调用该包装；运行参数未开启时保持几乎零成本快路径，
 * 开启时才进入 __shuffle_free_memory() 遍历所有 zone。
 * 入参：pgdat 是不可为 NULL 的借用 node 数据；禁用快路径不会解引用它。
 * 出参/返回：无直接返回值；禁用时无副作用，启用时只改变 freelist 链接顺序。
 * 注意事项：__meminit 上下文；核心实现可调度并获取 zone 锁，调用者不能持冲突锁。
 */
static inline void __meminit shuffle_free_memory(pg_data_t *pgdat)
{
	/* static branch 是提交边界：配置存在但参数关闭时不进入昂贵遍历。 */
	if (!static_branch_unlikely(&page_alloc_shuffle_key))
		return;
	__shuffle_free_memory(pgdat);
}

/*
 * __shuffle_zone() - 对单个 zone 的 SHUFFLE_ORDER 空闲块执行 Fisher-Yates 式洗牌。
 * 业务背景：启动 node 洗牌和内存热插拔都落到此核心实现，使新上线页不会集中在
 * freelist 一端。
 * 入参：z 是不可为 NULL 的输入输出借用 zone，页框和 free_area 已初始化。
 * 出参/返回：无直接返回值；在 zone->lock 下交换合格空闲块的 lru 链接，不改变计数。
 * 注意事项：__meminit 且可 cond_resched；只交换同 order、同 migratetype 的 PageBuddy。
 */
extern void __shuffle_zone(struct zone *z);
/*
 * shuffle_zone() - 以 static key 门控单 zone 洗牌。
 * 业务背景：memory hotplug 完成基础上线并解除隔离后调用，确保新页分散到整个
 * freelist；启动期的 node 核心实现也通过该包装逐 zone 调用。
 * 入参：z 是不可为 NULL 的借用 zone；禁用时不解引用。
 * 出参/返回：无直接返回值；关闭时无副作用，开启时把工作交给 __shuffle_zone()。
 * 注意事项：调用点不得持 z->lock，因为核心实现自行加锁并会临时释放锁调度。
 */
static inline void __meminit shuffle_zone(struct zone *z)
{
	/* 运行期开关关闭时直接返回，不触碰 zone 的 free_area。 */
	if (!static_branch_unlikely(&page_alloc_shuffle_key))
		return;
	__shuffle_zone(z);
}

/*
 * is_shuffle_order() - 判断本次 buddy 释放是否应用随机头/尾插入策略。
 * 业务背景：__free_one_page() 完成合并后在 FPI_TO_TAIL 与普通合并启发式之间分派；
 * 只有运行期开启且最终 order 达到洗牌粒度才消耗随机位。
 * 入参：order 是待发布空闲块的 buddy 阶数，表示 2^order 页，可为 allocator 支持范围。
 * 出参/返回：static key 关闭返回 false；开启时返回 order >= SHUFFLE_ORDER，无副作用。
 * 注意事项：热路径内联、不睡眠；它只选择链表位置，不改变块合法性或 migratetype。
 */
static inline bool is_shuffle_order(int order)
{
	/* 先检查 static key，避免关闭功能时执行阈值比较和随机状态访问。 */
	if (!static_branch_unlikely(&page_alloc_shuffle_key))
		return false;
	return order >= SHUFFLE_ORDER;
}
#else
/*
 * CONFIG_SHUFFLE_PAGE_ALLOCATOR=n 时保留四个中性内联接口：选择函数恒 false，
 * node/zone 操作为空，使调用点无需条件编译且编译器能完全消除功能开销。
 */
/*
 * shuffle_pick_tail() - 未编译洗牌功能时选择 freelist 头部。
 * 业务背景：保持 page allocator 的统一调用接口。
 * 入参：无。
 * 出参/返回：恒为 false，无随机状态或其他副作用。
 * 注意事项：通常 is_shuffle_order() 已先返回 false，本桩不应成为随机源。
 */
static inline bool shuffle_pick_tail(void)
{
	return false;
}

/*
 * shuffle_free_memory() - 未编译功能时的整 node 空操作。
 * 业务背景：mm_init 可无条件遍历 node 并调用该名称。
 * 入参：pgdat 是未读取的借用指针，可在本桩中为任意值，ownership 不变。
 * 出参/返回：无直接返回值，不修改 zone 或 freelist。
 * 注意事项：不睡眠、不取锁，编译器会消除调用。
 */
static inline void shuffle_free_memory(pg_data_t *pgdat)
{
}

/*
 * shuffle_zone() - 未编译功能时的单 zone 空操作。
 * 业务背景：memory hotplug 可保留无条件调用，而不引入链接依赖。
 * 入参：z 是未读取的借用指针，ownership 不变。
 * 出参/返回：无直接返回值和副作用。
 * 注意事项：不会重新分布新上线页；这是配置选择而不是运行期失败。
 */
static inline void shuffle_zone(struct zone *z)
{
}

/*
 * is_shuffle_order() - 未编译功能时禁止随机插入策略。
 * 业务背景：让 buddy allocator 继续使用 FPI_TO_TAIL 或 merge-likely 既有决策。
 * 入参：order 是未读取的 buddy 阶数值。
 * 出参/返回：恒为 false，无副作用。
 * 注意事项：结果只表达配置关闭，不判断 order 是否属于 allocator 合法范围。
 */
static inline bool is_shuffle_order(int order)
{
	return false;
}
#endif
#endif /* _MM_SHUFFLE_H */
/* 结束 shuffle.h 的配置分支与防重复包含范围。 */
