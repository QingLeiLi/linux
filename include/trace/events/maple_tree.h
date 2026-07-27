/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Maple Tree tracepoint 学习地图
 *
 * 本文件只定义观测接口，不参与 Maple Tree 的查找、写入或平衡算法。
 * lib/maple_tree.c 在关键位置调用 trace_ma_op()/trace_ma_read()/
 * trace_ma_write()；启用相应 tracepoint 后，TRACE_EVENT() 生成的代码把
 * 当时的 ma_state 快照写入 tracing ring buffer，用户可通过 tracefs、
 * perf 或 eBPF 读取。
 *
 * 三类事件的关注点：
 * - ma_op：记录一次通用内部操作及游标位置；
 * - ma_read：记录面向读取路径的游标快照；
 * - ma_write：在通用状态之外记录 pivot 和待写入值。
 *
 * 关键边界：
 * - 事件字段是触发瞬间的标量/指针值快照，不会延长 tree、node 或 entry
 *   的生命周期，也不能保证打印时这些地址仍指向存活对象；
 * - tracepoint 只观测一次函数执行到达了某个位置，不能单独证明写入已经
 *   发布、RCU 宽限期已经结束，或整个操作最终成功；
 * - ma_state 使用闭区间：[min,max] 是当前节点覆盖范围，
 *   [index,last] 是调用者正在查找或修改的范围。
 */
#undef TRACE_SYSTEM
/*
 * tracepoint 框架以 TRACE_SYSTEM 组成 tracefs 中的系统名，因而这些
 * 事件显示为 maple_tree:ma_op、maple_tree:ma_read 和
 * maple_tree:ma_write。
 */
#define TRACE_SYSTEM maple_tree

/*
 * trace event 头文件会被 define_trace.h 以不同宏定义重复包含：
 * 第一次建立声明，TRACE_HEADER_MULTI_READ 阶段再生成定义。这个条件
 * 不能改成普通的一次性 include guard，否则 tracepoint 只能声明而无法
 * 生成实际代码。
 *
 * _TRACE_MM_H 是本文件沿用的 guard 名称；虽然名字偏通用，它只在这里
 * 控制 Maple Tree 事件主体的重复展开。
 */
#if !defined(_TRACE_MM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_MM_H


#include <linux/tracepoint.h>

/*
 * 这里只需声明 ma_state：TP_PROTO() 保存的是指针，真正访问其字段的
 * TP_fast_assign() 会在包含者已经取得完整 Maple Tree 定义后展开。
 * tracepoint 不取得 ma_state 的 ownership。
 */
struct ma_state;

/*
 * TRACE_EVENT(ma_op) - 记录通用 Maple Tree 内部操作的游标快照。
 *
 * 【调用位置】
 * lib/maple_tree.c 的跨节点写、节点重建、重平衡等内部路径使用
 * trace_ma_op(TP_FCT, mas)。TP_FCT 由 tracepoint_string(__func__)
 * 生成稳定的函数名字符串。
 *
 * 【参数与生命周期】
 * @fn：触发点所在函数名；只借用，事件记录保存指针值。
 * @mas：当前 Maple 游标；只在 tracepoint 执行期间借用，不转移所有权。
 *
 * 【记录语义】
 * TP_STRUCT__entry 定义 ring-buffer 记录布局；TP_fast_assign 在调用点
 * 立即复制字段；TP_printk 只定义用户读取事件时的文本格式。记录事件
 * 无返回值，也不改变 mas、tree 或节点状态。
 */
TRACE_EVENT(ma_op,

	/* 生成 trace_ma_op(fn, mas) 探针函数的参数原型。 */
	TP_PROTO(const char *fn, struct ma_state *mas),

	/* 把调用实参原样传给生成的 probe/fast-assign 代码。 */
	TP_ARGS(fn, mas),

	/*
	 * 事件记录保存六个固定大小字段。这里保存 node 的 encoded pointer，
	 * 其低位可能含 Maple 节点类型等 tag，不能直接当作长期有效裸节点。
	 */
	TP_STRUCT__entry(
			/* TP_FCT 提供的触发函数名。 */
			__field(const char *, fn)
			/* 当前节点覆盖闭区间的左、右端点。 */
			__field(unsigned long, min)
			__field(unsigned long, max)
			/* 当前操作目标闭区间的左、右端点。 */
			__field(unsigned long, index)
			__field(unsigned long, last)
			/* ma_state 当前 encoded node/root/error 状态的指针值。 */
			__field(void *, node)
	),

	/*
	 * 阶段：在触发 CPU 上取得一致的“调用点快照”。
	 *
	 * 这里只逐字段复制，没有锁、RCU 引用或对象 pin；一致性来自调用者
	 * 原本所处的 Maple 锁/RCU 上下文，而不是 tracepoint 自身。
	 */
	TP_fast_assign(
			__entry->fn		= fn;
			__entry->min		= mas->min;
			__entry->max		= mas->max;
			__entry->index		= mas->index;
			__entry->last		= mas->last;
			__entry->node		= mas->node;
	),

	/*
	 * 文本输出依次为函数名、encoded node、节点边界和目标 range。
	 * 原始数值字段仍可由 perf/eBPF 直接读取，不必解析这段字符串。
	 */
	TP_printk("%s\tNode: %p (%lu %lu) range: %lu-%lu",
		  __entry->fn,
		  (void *) __entry->node,
		  (unsigned long) __entry->min,
		  (unsigned long) __entry->max,
		  (unsigned long) __entry->index,
		  (unsigned long) __entry->last
	)
)

/*
 * TRACE_EVENT(ma_read) - 记录 Maple Tree 读取路径的 ma_state 快照。
 *
 * 【调用位置】
 * mtree_load()、mt_find() 等读接口在进入关键查找位置时调用
 * trace_ma_read()，用于把“哪个 API、从哪个范围开始查”与后续结果关联。
 *
 * 【并发边界】
 * 事件常在 RCU 读侧附近触发，但 trace 记录本身不替调用者获取 RCU 锁；
 * node 地址离开原保护范围后只能作为诊断标识，不能解引用。
 *
 * 字段、赋值和返回约定与 ma_op 相同。单独看到 ma_read 只说明读路径
 * 到达探针，不能证明最终命中 entry。
 */
TRACE_EVENT(ma_read,

	/* 生成 trace_ma_read(fn, mas) 的参数原型和实参转发。 */
	TP_PROTO(const char *fn, struct ma_state *mas),

	TP_ARGS(fn, mas),

	/* 记录布局与 ma_op 一致，便于用同一套字段分析读/通用操作。 */
	TP_STRUCT__entry(
			/* 触发 ma_read 的函数名。 */
			__field(const char *, fn)
			/* 读取发生时，当前节点覆盖的闭区间。 */
			__field(unsigned long, min)
			__field(unsigned long, max)
			/* 本次读取正在定位的闭区间。 */
			__field(unsigned long, index)
			__field(unsigned long, last)
			/* 当前 encoded node，只作为诊断标识。 */
			__field(void *, node)
	),

	/* 在读路径当前保护上下文内复制游标字段，不保留对象引用。 */
	TP_fast_assign(
			__entry->fn		= fn;
			__entry->min		= mas->min;
			__entry->max		= mas->max;
			__entry->index		= mas->index;
			__entry->last		= mas->last;
			__entry->node		= mas->node;
	),

	/* 输出节点覆盖范围与本次读取的目标范围。 */
	TP_printk("%s\tNode: %p (%lu %lu) range: %lu-%lu",
		  __entry->fn,
		  (void *) __entry->node,
		  (unsigned long) __entry->min,
		  (unsigned long) __entry->max,
		  (unsigned long) __entry->index,
		  (unsigned long) __entry->last
	)
)

/*
 * TRACE_EVENT(ma_write) - 记录 Maple Tree 写路径及写入附加信息。
 *
 * 【调用位置】
 * mas_store()、节点内写入、split/rebalance 和简单 mtree_store_range()
 * 等路径使用 trace_ma_write()。同一逻辑操作可能在不同阶段产生多条事件，
 * 因此应结合 fn、range、node、piv 顺序分析，不能把每条都视为一次独立
 * 成功提交。
 *
 * 【参数】
 * @fn：触发函数名，借用字符串。
 * @mas：写侧游标，通常由 Maple 写锁保护；事件不接管它。
 * @piv：调用点希望额外暴露的 pivot/offset 相关数值；不同触发点可能传
 *       0、range 上界或新 data-end，必须结合 fn 解释。
 * @val：准备写入或当前处理的 entry 指针；可为 NULL，事件不取得引用。
 *
 * 【可观察边界】
 * 这是写路径的诊断快照，不等价于 rcu_assign_pointer() 发布完成，也不
 * 表示旧节点已经经过 RCU 宽限期回收。
 */
TRACE_EVENT(ma_write,

	/* 生成 trace_ma_write(fn, mas, piv, val) 的探针原型。 */
	TP_PROTO(const char *fn, struct ma_state *mas, unsigned long piv,
		 void *val),

	/* 将四个调用参数转交给事件赋值阶段。 */
	TP_ARGS(fn, mas, piv, val),

	/*
	 * 在通用游标字段上增加 piv 和 val，用于区分边界变化与写入内容。
	 */
	TP_STRUCT__entry(
			/* 触发写事件的函数名。 */
			__field(const char *, fn)
			/* 写入时当前节点覆盖的闭区间。 */
			__field(unsigned long, min)
			__field(unsigned long, max)
			/* 调用者请求修改的闭区间。 */
			__field(unsigned long, index)
			__field(unsigned long, last)
			/* 调用点提供的附加 pivot/offset 数值。 */
			__field(unsigned long, piv)
			/* entry 指针快照；NULL 通常表示清除 range。 */
			__field(void *, val)
			/* 写状态当前 encoded node 的地址/tag 快照。 */
			__field(void *, node)
	),

	/*
	 * 在写侧同步上下文内复制数据。这里只记录地址和值，不复制 entry
	 * 对象，也不阻止探针返回后的并发回收。
	 */
	TP_fast_assign(
			__entry->fn		= fn;
			__entry->min		= mas->min;
			__entry->max		= mas->max;
			__entry->index		= mas->index;
			__entry->last		= mas->last;
			__entry->piv		= piv;
			__entry->val		= val;
			__entry->node		= mas->node;
	),

	/* 输出函数、节点/目标边界、附加 pivot 以及 entry 指针。 */
	TP_printk("%s\tNode %p (%lu %lu) range:%lu-%lu piv (%lu) val %p",
		  __entry->fn,
		  (void *) __entry->node,
		  (unsigned long) __entry->min,
		  (unsigned long) __entry->max,
		  (unsigned long) __entry->index,
		  (unsigned long) __entry->last,
		  (unsigned long) __entry->piv,
		  (void *) __entry->val
	)
)
#endif /* _TRACE_MM_H */
/* Maple Tree 事件主体 guard 到此结束；define_trace.h 必须位于其外部。 */

/* This part must be outside protection */
/*
 * 以下部分必须位于 include guard 之外：tracepoint 框架通过再次包含本
 * 文件，把前面的 TRACE_EVENT 描述展开为实际定义。移入 guard 会使第二
 * 遍展开被跳过，最终产生缺失 tracepoint 定义或链接错误。
 */
#include <trace/define_trace.h>
