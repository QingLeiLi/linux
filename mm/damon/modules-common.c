// SPDX-License-Identifier: GPL-2.0
/*
 * 中文学习注释由 OpenAI Codex（GPT-5，2026-08-27）生成。
 *
 * 文件地图：DAMON_RECLAIM 与 DAMON_LRU_SORT 都监控物理地址空间，本文件把
 * “创建 context → 选择 paddr operations → 创建并挂入唯一 target”封装成共同
 * 构造事务。它只建立空的运行骨架；监控区间、采样参数、DAMOS scheme、启动
 * 与停止仍由各模块负责。
 *
 * 对象生命周期：成功后 ctx 拥有 target，两个输出指针分别提供容器和其中的
 * 借用入口；调用者最终只需 damon_destroy_ctx(ctx)，销毁过程会遍历 target
 * 链表释放 target。所有对象在返回前尚未交给 kdamond，不涉及并发发布。
 */
/*
 * Common Code for DAMON Modules
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/*
 * DAMON 内核模块共享的构造代码。
 * 作者信息保持上游原文；当前公共部分专门消除 reclaim 与 LRU sort 模块创建
 * 物理地址监控上下文时的重复资源获取和错误回滚。
 */

#include <linux/damon.h>

#include "modules-common.h"

/*
 * Allocate, set, and return a DAMON context for the physical address space.
 * @ctxp:	Pointer to save the point to the newly created context
 * @targetp:	Pointer to save the point to the newly created target
 */
/*
 * damon_modules_new_paddr_ctx_target() - 原子式构造物理地址 DAMON 空骨架。
 *
 * 上游说明的含义是：分配并配置一个用于物理地址空间的 DAMON context，随后
 * 返回它和新 target；@ctxp 保存新 context 指针，@targetp 保存新 target 指针。
 *
 * 业务背景：reclaim/lru_sort 的参数试算路径和模块初始化路径都需要相同起点；
 * 本函数把三步构造与逆序回滚集中起来，保证调用者只会看到“两者均成功”或
 * “没有任何对象交付”。调用链为模块 init/apply_parameters → 本函数 →
 * damon_new_ctx()/damon_select_ops()/damon_new_target()。
 * 入参：ctxp、targetp 都是不可为 NULL 的输出槽地址；调用前其中旧值不被读取，
 * 失败时也不会写入。成功后 *ctxp 持有 context 的销毁责任，*targetp 指向已挂
 * 入该 context 的 target，不形成需要单独释放的第二份所有权。
 * 出参/返回：0 表示两个输出均有效；-ENOMEM 表示 context 或 target 分配失败；
 * -EINVAL 表示 PADDR operations 尚未注册/不可选择。失败时内部释放已分配对象，
 * 输出槽保持调用前内容；成功后调用者通常继续设置 attrs、region 和 scheme。
 * 注意事项：只能在允许内存分配和 mutex 睡眠的进程上下文调用；成功对象尚未
 * 启动或发布给 kdamond。最终释放必须调用 damon_destroy_ctx(*ctxp)，不可再对
 * 已归 ctx 所有的 *targetp 单独销毁，否则会破坏链表和产生重复释放。
 */
int damon_modules_new_paddr_ctx_target(struct damon_ctx **ctxp,
		struct damon_target **targetp)
{
	/*
	 * 变量地图：ctx 是本函数在提交前独占的新监控容器；target 是稍后挂入
	 * ctx->adaptive_targets 的唯一空目标。写入输出槽之前，错误路径可完全
	 * 在本函数内回滚，不会让调用者观察到半构造对象。
	 */
	struct damon_ctx *ctx;
	struct damon_target *target;

	/*
	 * 阶段 1：先取得带默认 attrs、锁和空链表的 context。分配失败时尚无
	 * 资源需要回收，直接返回 -ENOMEM，两个输出槽保持不变。
	 */
	ctx = damon_new_ctx();
	if (!ctx)
		return -ENOMEM;

	/*
	 * 阶段 2：在全局 operations 注册表的 mutex 保护下复制 PADDR 操作表。
	 * 选择失败说明该后端未注册；context 仍由本函数独占，可立即整体销毁。
	 */
	if (damon_select_ops(ctx, DAMON_OPS_PADDR)) {
		damon_destroy_ctx(ctx);
		return -EINVAL;
	}

	/*
	 * 阶段 3：分配空 target。此时 ctx 尚无 target/scheme，失败回滚只需
	 * damon_destroy_ctx()；成功的 damon_add_target() 把 target 链入 ctx，
	 * 从该点起 target 的最终释放责任随 context 生命周期一起管理。
	 */
	target = damon_new_target();
	if (!target) {
		damon_destroy_ctx(ctx);
		return -ENOMEM;
	}
	damon_add_target(ctx, target);

	/*
	 * 阶段 4（提交点）：所有可能失败的步骤均已结束，最后才同时发布两个
	 * 输出。先前错误路径从不触碰输出槽，因此调用者可仅以返回值判断有效性。
	 */
	*ctxp = ctx;
	*targetp = target;
	return 0;
}
