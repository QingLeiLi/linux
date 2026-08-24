/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Copyright (c) 2025 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2025 Tejun Heo <tj@kernel.org>
 */
/*
 * BPF 可扩展调度类接口见 Documentation/scheduler/sched-ext.rst；许可证和版权名单
 * 原样保留。本头文件只公开每个 scx 调度器实例的 arena 子分配器生命周期接口，
 * 具体的 gen_pool 扩容、地址转换和回滚位于 arena.c。
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 启用 BPF arena 的 sch 在注册/加载的可睡眠阶段创建 gen_pool；CID 类型调度器随后
 * 用它为每 CPU scratch mask 分配可同时由内核和 BPF 访问的存储。池空间不足时 alloc
 * 以 BPF arena 页为底层存储按需扩容，再返回内核映射地址；调用者若需要 BPF 地址需
 * 自行转换。销毁先把尚未归还的 gen_pool 子分配标记释放，底层页则随 arena map 拆除。
 *
 * 这种两层设计用成熟的 gen_pool 支持小对象分配并减少逐对象页开销；代价是分配可能
 * 睡眠、返回 NULL 且池元数据与 arena 页具有不同释放边界。接口不提供并发串行化，
 * 调用者必须用 scx_sched 的装载/卸载生命周期保证 sch、arena_map 和 arena_pool 有效。
 */
#ifndef _KERNEL_SCHED_EXT_ARENA_H
#define _KERNEL_SCHED_EXT_ARENA_H

#include <linux/types.h>

/* 调度器实例的完整定义在 internal.h；这里以借用指针传递，避免公开内部布局。 */
struct scx_sched;

/*
 * scx_arena_pool_init() - 为调度器实例创建内核侧 arena 子分配池
 * @sch: 输入输出、不可为 NULL 的未发布调度器实例；函数不取得引用或转移所有权。
 * 返回 0 表示 arena 未配置而无需建池，或 gen_pool 已成功写入 sch->arena_pool；返回
 * -ENOMEM 表示池元数据分配失败。创建使用 GFP_KERNEL 语义，可以睡眠，调用者在后续
 * 初始化失败时须调用 destroy；此函数本身不分配 BPF arena 页。
 */
s32 scx_arena_pool_init(struct scx_sched *sch);

/*
 * scx_arena_pool_destroy() - 拆除内核侧子分配池
 * @sch: 输入输出、不可为 NULL 的下线中实例；所有并发 alloc/free 必须已经停止。
 * 返回：无直接返回值。池为空时幂等返回；否则先清除仍占用的 gen_pool 区间以避免
 * destroy 的一致性 BUG，再释放池元数据并把 arena_pool 置 NULL。底层 BPF arena 页
 * 不在这里释放，而由 arena map 生命周期回收；函数不等待 reader，也不接管 sch。
 */
void scx_arena_pool_destroy(struct scx_sched *sch);

/*
 * scx_arena_alloc() - 从调度器 arena 池分配指定字节数并返回内核映射地址
 * @sch: 输入输出、不可为 NULL 的活动实例；arena_pool 缺失时返回 NULL。
 * @size: 所需字节数；gen_pool 以 8 字节最小粒度管理，调用者必须保存同一 size 供 free。
 * 返回非 NULL 的内核虚拟地址表示调用者取得该子区间使用权、须以同一 size 归还；NULL
 * 表示无池或扩容失败。
 * 池不足时至少申请 4 页并加入 gen_pool；BPF 页分配或池元数据登记失败均收敛为 NULL，
 * 登记失败会先归还新页。函数显式要求可睡眠的 GFP_KERNEL 上下文，不能在原子路径调用。
 */
void *scx_arena_alloc(struct scx_sched *sch, size_t size);

/*
 * scx_arena_free() - 把一个 arena 子区间归还给 gen_pool
 * @sch: 输入输出、不可为 NULL 的活动实例；不取得引用。
 * @kern_va: alloc 返回的内核映射地址，可为 NULL；NULL 或池缺失时不执行操作。
 * @size: 必须与分配时的字节数一致，否则会破坏 gen_pool 分配位图。
 * 返回：无直接返回值。函数只归还子区间，不释放底层 arena 页、不清零内容，也不把
 * 调用者保存的指针置空；归还后该地址立即失效，调用者须保证不存在并发使用或重复释放。
 */
void scx_arena_free(struct scx_sched *sch, void *kern_va, size_t size);

/* 结束本头文件的重复包含保护；该注释不改变原有宏结构。 */
#endif /* _KERNEL_SCHED_EXT_ARENA_H */
