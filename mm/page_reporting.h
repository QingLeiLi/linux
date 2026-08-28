/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_PAGE_REPORTING_H
#define _MM_PAGE_REPORTING_H

/*
 * 这是伙伴分配器与通用 page reporting 实现之间的 mm 私有热路径接口。
 * mm/page_alloc.c 在页取出或释放时调用这里的轻量 helper；mm/page_reporting.c
 * 管理设备注册、RCU 发布、延迟工作和批量 scatterlist。对外驱动契约位于
 * include/linux/page_reporting.h，不能用本头文件绕过注册/注销生命周期。
 */

/*
 * 依赖地图：mmzone/pageblock/isolation 提供 zone、buddy order 与隔离协议；
 * jump_label 让功能关闭时热路径近似没有分支成本；scatterlist/slab/pgtable
 * 支撑实现侧批量页描述与基础类型。这里只声明/内联，不取得任何对象所有权。
 */
#include <linux/mmzone.h>
#include <linux/pageblock-flags.h>
#include <linux/page-isolation.h>
#include <linux/jump_label.h>
#include <linux/slab.h>
#include <linux/pgtable.h>
#include <linux/scatterlist.h>

#ifdef CONFIG_PAGE_REPORTING
/*
 * page_reporting_enabled 初始为 false。page_reporting_register() 在完整初始化
 * prdev、提交首次延迟工作并通过 RCU 发布设备后启用静态键；伙伴分配热路径据此
 * 跳过全部 reporting 逻辑。注销会先把 RCU 指针置空并等待读者，再同步取消工作；
 * 当前实现不关闭静态键，所以注销后的罕见调用仍安全进入通知函数并看到 NULL。
 */
DECLARE_STATIC_KEY_FALSE(page_reporting_enabled);

/*
 * page_reporting_order 是允许触发/收集的最小伙伴 order。模块参数、注册设备的
 * order 或 pageblock_order 依次决定它；值代表 2^order 个连续页，而不是字节数。
 * 该全局也导出给唯一注册驱动协调批量粒度，读者不能把它当作 per-zone 配置。
 */
extern unsigned int page_reporting_order;

/*
 * __page_reporting_notify() - 把一次合格释放转换为异步 reporting 请求。
 *
 * 业务背景：page_reporting_notify_free() 的慢端；借助 RCU 借用当前唯一 prdev，
 * 再以原子状态请求两秒后执行批处理，避免在伙伴系统释放热路径直接调用设备。
 * 入参：无；调用者不传 page，函数只表达“已有足够大的新空闲块”这一事件。
 * 出参/返回：无直接返回值；设备存在时可能把 IDLE 状态改为 REQUESTED 并调度
 * delayed_work，设备正在工作或已经有请求时合并事件；关闭/注销窗口中无副作用。
 * 注意事项：可从 __free_one_page() 路径调用；函数自身用 RCU 保护 prdev 生命周期，
 * 不等待设备完成，也不保证本次释放的具体页一定进入下一批报告。
 */
void __page_reporting_notify(void);

/*
 * page_reported() - 判断即将离开 buddy free list 的页块是否保有 reported 标记。
 *
 * 业务背景：mm/page_alloc.c:__del_page_from_free_list() 在把空闲块交给分配者前
 * 调用；若为 true，调用者清除 PageReported，防止页被修改/重新分配后沿用旧报告。
 * 入参：@page 是仍位于伙伴系统 free list 的借用页指针，调用者持有 zone->lock；
 * 本函数不增引用、不改变页状态，PageReported 描述的是该 buddy 块头页。
 * 出参/返回：功能启用且页标志为真时返回 true，其他情况返回 false；无副作用。
 * 注意事项：先检查 static key 再读 PageReported，使未注册设备时不触碰冷门状态；
 * 静态键只控制快路径成本，页标志的设置/清除仍由 zone 锁下的 buddy 协议串行化。
 */
static inline bool page_reported(struct page *page)
{
	return static_branch_unlikely(&page_reporting_enabled) &&
	       PageReported(page);
}

/**
 * page_reporting_notify_free - Free page notification to start page processing
 *
 * This function is meant to act as a screener for __page_reporting_notify
 * which will determine if a give zone has crossed over the high-water mark
 * that will justify us beginning page treatment. If we have crossed that
 * threshold then it will start the process of pulling some pages and
 * placing them in the batch list for treatment.
 */
/*
 * page_reporting_notify_free() - 在页释放完成后筛选是否值得请求批量报告。
 *
 * 上述英文说明表示：本函数是 __page_reporting_notify() 的前置筛选器；只有某个
 * zone 的空闲状态越过足以开始页处理的高水位语义时，后台路径才会抽取空闲页，
 * 放入批量列表并交给设备。这里不直接计算 zone watermark；实现侧处理具体 zone
 * 时再次用 low watermark 加一个完整批次容量保证进度。
 *
 * 业务背景：调用链为 __free_one_page() 合并 buddy → 本函数 → 异步 request →
 * page_reporting_process() 隔离空闲页并调用驱动 report()。两级筛选把高频释放与
 * 低频设备操作解耦。
 * 入参：@order 是刚放回伙伴系统的最终合并 order，表示 2^order 页；纯值输入，
 * 没有指针、引用或 ownership 转移。
 * 出参/返回：无直接返回值；静态键关闭或 order 小于阈值时无副作用，否则仅请求
 * 合并式 delayed_work，不承诺该页、该 zone 或本轮工作一定被设备报告。
 * 注意事项：从 __free_one_page() 热路径、页已加入 free list 后调用；必须保持
 * 两个快速返回，避免每次释放承担 RCU/原子操作。实际隔离和 report 可睡眠/失败，
 * 都发生在工作队列中，不延长当前释放路径的锁持有时间。
 */
static inline void page_reporting_notify_free(unsigned int order)
{
	/* Called from hot path in __free_one_page() */
	/* 由 __free_one_page() 热路径调用，静态键关闭时跳转被 patch 成近似空操作。 */
	if (!static_branch_unlikely(&page_reporting_enabled))
		return;

	/* Determine if we have crossed reporting threshold */
	/* 小于最小 order 的零散释放不触发批处理，避免通知频率压过实际设备收益。 */
	if (order < page_reporting_order)
		return;

	/* This will add a few cycles, but should be called infrequently */
	/* 合格大块释放才进入 RCU/原子请求慢端；请求会合并，通常不会逐次排队。 */
	__page_reporting_notify();
}
#else /* CONFIG_PAGE_REPORTING */
/*
 * CONFIG_PAGE_REPORTING 关闭时，编译期常量 false 让 page allocator 的标志清理
 * 分支完全消失；参数只用于保持调用语法一致，不会求值或取得 page 引用。
 */
#define page_reported(_page)	false

/*
 * page_reporting_notify_free() - 未构建 page reporting 时的零成本兼容桩。
 *
 * 业务背景：让 __free_one_page() 无须散布条件编译，关闭功能时由编译器删除调用。
 * 入参：@order 为释放块 order，但本配置下不读取，任意合法 order 均可传入。
 * 出参/返回：无直接返回值，无输出、状态变化、调度或 ownership 副作用。
 * 注意事项：不需要锁且不会睡眠；该空实现与上面的启用分支保持相同调用契约。
 */
static inline void page_reporting_notify_free(unsigned int order)
{
}
#endif /* CONFIG_PAGE_REPORTING */
/* 结束配置分支：调用者在两种构建下都得到同名接口，但运行成本和副作用不同。 */
#endif /*_MM_PAGE_REPORTING_H */
