// SPDX-License-Identifier: GPL-2.0
#include <linux/mm_types.h>
#include <linux/tracepoint.h>

/*
 * 在本翻译单元实例化 page_ref tracepoint 的定义；其他调用点只看到声明。
 * 下列 __page_ref_* 包装函数均位于 page_ref 原子操作完成后的观测路径：它们只
 * 上报调用者已经计算出的新值/结果，不再次修改引用，也不为 page 取得额外引用。
 */
#define CREATE_TRACE_POINTS
#include <trace/events/page_ref.h>

/*
 * __page_ref_set() - 记录一次把页引用计数直接设为指定值的事件。
 * 业务背景：page_ref_set() 在完成真实原子写入后调用，供调试器重建引用历史。
 * 入参：page 是事件期间借用且非 NULL 的页描述符；v 是已写入的新引用值。
 * 出参/返回：无直接返回值、无 ownership 变化；仅向 page_ref_set tracepoint 发布事件。
 * 注意事项：不负责同步或校验 v，trace 回调必须遵守 tracepoint 上下文且不得假定持有页引用。
 */
void __page_ref_set(struct page *page, int v)
{
	trace_page_ref_set(page, v);
}
EXPORT_SYMBOL(__page_ref_set);
EXPORT_TRACEPOINT_SYMBOL(page_ref_set);

/*
 * __page_ref_mod() - 记录一次无返回值的页引用增减。
 * 业务背景：page_ref_add/sub 等调用者已改变计数，本层把增量交给跟踪消费者。
 * 入参：page 为短期借用页；v 为有符号增量，正数增加、负数减少。
 * 出参/返回：无直接返回值和引用副作用；发布 page_ref_mod 事件后返回原调用链。
 * 注意事项：事件只是事后观测，不能凭它阻止并发释放，也不保证读取到稳定页字段。
 */
void __page_ref_mod(struct page *page, int v)
{
	trace_page_ref_mod(page, v);
}
EXPORT_SYMBOL(__page_ref_mod);
EXPORT_TRACEPOINT_SYMBOL(page_ref_mod);

/*
 * __page_ref_mod_and_test() - 记录增减引用并测试是否为零的复合操作结果。
 * 业务背景：调用者用该结果决定是否进入最后释放路径，trace 用于解释该决定。
 * 入参：page 为借用页；v 为已应用的有符号增量；ret 为原子 test 的布尔结果。
 * 出参/返回：无直接返回值；只发布 page_ref_mod_and_test，不接管最后释放责任。
 * 注意事项：ret 非零表示原操作观察到目标条件成立；包装层不得重新测试造成竞态。
 */
void __page_ref_mod_and_test(struct page *page, int v, int ret)
{
	trace_page_ref_mod_and_test(page, v, ret);
}
EXPORT_SYMBOL(__page_ref_mod_and_test);
EXPORT_TRACEPOINT_SYMBOL(page_ref_mod_and_test);

/*
 * __page_ref_mod_and_return() - 记录引用增减及原子操作返回的新计数。
 * 业务背景：page_ref_add/sub_return 已完成状态转换，本函数保存可审计的新值。
 * 入参：page 是事件期间借用页；v 是增量；ret 是原子操作直接返回的新引用值。
 * 出参/返回：无直接返回值、无额外引用；发布 page_ref_mod_and_return 事件。
 * 注意事项：并发修改可在事件后立刻改变计数，ret 只代表原操作的线性化结果。
 */
void __page_ref_mod_and_return(struct page *page, int v, int ret)
{
	trace_page_ref_mod_and_return(page, v, ret);
}
EXPORT_SYMBOL(__page_ref_mod_and_return);
EXPORT_TRACEPOINT_SYMBOL(page_ref_mod_and_return);

/*
 * __page_ref_mod_unless() - 记录“未等于哨兵值时才增减”的尝试。
 * 业务背景：page_ref_add_unless 一类接口避免复活已到特定状态的页，trace 保留条件。
 * 入参：page 为借用页；v 是尝试应用的增量；u 是禁止修改的哨兵引用值。
 * 出参/返回：无直接返回值；只发布 page_ref_mod_unless，是否成功由真实原子操作决定。
 * 注意事项：本接口没有 ret 参数，消费者不能仅由事件断言修改成功或取得页所有权。
 */
void __page_ref_mod_unless(struct page *page, int v, int u)
{
	trace_page_ref_mod_unless(page, v, u);
}
EXPORT_SYMBOL(__page_ref_mod_unless);
EXPORT_TRACEPOINT_SYMBOL(page_ref_mod_unless);

/*
 * __page_ref_freeze() - 记录把引用计数从期望值冻结到零的尝试结果。
 * 业务背景：迁移/回收在排除其他持有者后冻结页，ret 决定能否进入独占状态转换。
 * 入参：page 为借用页；v 为要求匹配的旧引用值；ret 为 cmpxchg 类操作成功标志。
 * 出参/返回：无直接返回值且不冻结页面；仅发布 page_ref_freeze 观测事件。
 * 注意事项：成功后的独占/解冻责任仍属于调用者；tracepoint 不能替代页锁和引用协议。
 */
void __page_ref_freeze(struct page *page, int v, int ret)
{
	trace_page_ref_freeze(page, v, ret);
}
EXPORT_SYMBOL(__page_ref_freeze);
EXPORT_TRACEPOINT_SYMBOL(page_ref_freeze);

/*
 * __page_ref_unfreeze() - 记录已冻结页恢复到非零引用值的事件。
 * 业务背景：独占迁移、拆分或回收准备结束后，真实 unfreeze 操作重新发布页引用状态。
 * 入参：page 为借用页；v 是调用者已经写回的新引用值，通常代表重新建立的持有者数。
 * 出参/返回：无直接返回值和所有权变化；发布 page_ref_unfreeze 事件。
 * 注意事项：必须由持有冻结状态的路径调用；本包装函数不验证冻结前置条件且不睡眠。
 */
void __page_ref_unfreeze(struct page *page, int v)
{
	trace_page_ref_unfreeze(page, v);
}
EXPORT_SYMBOL(__page_ref_unfreeze);
EXPORT_TRACEPOINT_SYMBOL(page_ref_unfreeze);
