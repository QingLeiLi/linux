// SPDX-License-Identifier: GPL-2.0
/*
 * Compatibility functions which bloat the callers too much to make inline.
 * All of the callers of these functions should be converted to use folios
 * eventually.
 */
/*
 * 这些导出包装为仍使用 struct page 的旧调用者提供 folio API；若内联会把转换与
 * 慢路径逻辑复制到大量调用点而增大代码。page_folio() 只把任意成员页归一化到
 * 所属 folio，不增加引用；长期方向是让调用者直接持有 folio 后删除本兼容层。
 */

#include <linux/migrate.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include "internal.h"

/*
 * unlock_page() - 解锁 page 所属 folio 并唤醒等待者。
 * 业务背景：旧文件系统/故障路径完成页内容或状态更新后调用，实际状态转换在 folio_unlock()。
 * 入参：page 是调用者持有引用的非 NULL 成员页，所属 folio 必须已由当前路径锁定。
 * 出参/返回：无直接返回值；清 PG_locked 并可能唤醒 waiter，不释放调用者的页引用。
 * 注意事项：删除锁前的状态写会破坏发布顺序；错误解锁未持有的 folio 属调用者 bug。
 */
void unlock_page(struct page *page)
{
	return folio_unlock(page_folio(page));
}
EXPORT_SYMBOL(unlock_page);

/*
 * end_page_writeback() - 宣告 page 所属 folio 的写回完成。
 * 业务背景：旧 I/O completion 用它进入 folio_end_writeback()，完成 writeback 账目和唤醒。
 * 入参：page 为借用成员页，其 folio 必须正处于 writeback；ownership 不变。
 * 出参/返回：无直接返回值；清写回状态、更新统计并唤醒等待者。
 * 注意事项：通常可在完成上下文调用；重复结束会破坏 writeback 计数与等待协议。
 */
void end_page_writeback(struct page *page)
{
	return folio_end_writeback(page_folio(page));
}
EXPORT_SYMBOL(end_page_writeback);

/*
 * wait_on_page_writeback() - 等待 page 所属 folio 退出写回状态。
 * 业务背景：截断、换出和旧文件系统在修改/释放内容前需要与异步 I/O completion 汇合。
 * 入参：page 是持有引用的非 NULL 成员页；等待期间该引用保证 folio 不被释放。
 * 出参/返回：无直接返回值；返回时当前观察到 writeback 已清，无 ownership 变化。
 * 注意事项：可能睡眠，不可在原子上下文或持有与 completion 冲突的锁时调用。
 */
void wait_on_page_writeback(struct page *page)
{
	return folio_wait_writeback(page_folio(page));
}
EXPORT_SYMBOL_GPL(wait_on_page_writeback);

/*
 * mark_page_accessed() - 把一次旧 page 访问反馈给 folio/LRU 工作集算法。
 * 业务背景：读取、GUP 等路径用 referenced/active 状态避免热页被过早回收。
 * 入参：page 是借用成员页；调用者持有足以稳定 folio 的引用。
 * 出参/返回：无直接返回值；可能设置 referenced、激活 folio 或更新 workingset 统计。
 * 注意事项：内部处理 lruvec 并发；这是回收提示，不保证 folio 永不被回收。
 */
void mark_page_accessed(struct page *page)
{
	folio_mark_accessed(page_folio(page));
}
EXPORT_SYMBOL(mark_page_accessed);

/*
 * set_page_writeback() - 把 page 所属 folio 发布为正在写回。
 * 业务背景：旧 writepage 实现在提交 I/O 前调用，与 end_page_writeback() 成对。
 * 入参：page 为借用且已按 address_space 规则锁定的成员页。
 * 出参/返回：无直接返回值；设置 writeback、更新 mapping/bdi 账目并使 waiter 可观察。
 * 注意事项：必须早于 I/O completion；包装丢弃底层内部细节但不取得页引用。
 */
void set_page_writeback(struct page *page)
{
	folio_start_writeback(page_folio(page));
}
EXPORT_SYMBOL(set_page_writeback);

/*
 * set_page_dirty() - 将 page 所属 folio 标脏并通知 mapping。
 * 业务背景：旧写路径在修改缓存内容后调用，folio_mark_dirty() 负责 address_space 账目。
 * 入参：page 是持引用的非 NULL 成员页；锁/引用须能稳定 mapping。
 * 出参/返回：新建立 dirty 状态返回 true，原已脏等未转换情形返回 false；引用不转移。
 * 注意事项：无 folio 锁时可能与 truncate 竞争，调用者应满足底层 mapping 稳定契约。
 */
bool set_page_dirty(struct page *page)
{
	return folio_mark_dirty(page_folio(page));
}
EXPORT_SYMBOL(set_page_dirty);

/*
 * set_page_dirty_lock() - 在内部锁住 folio 后执行可靠的标脏。
 * 业务背景：调用者无法证明 mapping 在无锁下稳定时使用，避免 truncate 竞态。
 * 入参：page 为持有引用的成员页，入口不要求已锁；函数临时取得并释放 folio lock。
 * 出参/返回：返回 folio_mark_dirty() 的 1/0 状态；返回时不再额外持锁或引用。
 * 注意事项：可能睡眠，不能在原子上下文；若调用者已持不兼容锁可能死锁。
 */
int set_page_dirty_lock(struct page *page)
{
	return folio_mark_dirty_lock(page_folio(page));
}
EXPORT_SYMBOL(set_page_dirty_lock);

/*
 * clear_page_dirty_for_io() - 在 writepage 决策点为 I/O 清除 folio dirty 状态。
 * 业务背景：写回路径先清脏再提交；并发重新写入可再次置脏，避免丢失后续修改。
 * 入参：page 为借用成员页，调用者通常持 folio lock 并已稳定 mapping。
 * 出参/返回：确实清除 dirty 返回 true，否则 false；同步更新 dirty 账目，无引用变化。
 * 注意事项：返回 true 不等于 I/O 已开始，调用者随后须 start writeback 或重新标脏。
 */
bool clear_page_dirty_for_io(struct page *page)
{
	return folio_clear_dirty_for_io(page_folio(page));
}
EXPORT_SYMBOL(clear_page_dirty_for_io);

/*
 * redirty_page_for_writepage() - writepage 暂不能写时重新标脏并修正扫描账目。
 * 业务背景：回写实现遇到拥塞/依赖时保留数据供后续扫描，而不是把清脏页遗漏。
 * 入参：wbc 是借用的本轮 writeback 控制块并会更新；page 是持引用的成员页。
 * 出参/返回：返回 folio 是否发生 dirty 转换；folio 保持待写，ownership 不变。
 * 注意事项：应在 clear_page_dirty_for_io() 后、结束本次 writepage 前调用。
 */
bool redirty_page_for_writepage(struct writeback_control *wbc,
		struct page *page)
{
	return folio_redirty_for_writepage(wbc, page_folio(page));
}
EXPORT_SYMBOL(redirty_page_for_writepage);

/*
 * add_to_page_cache_lru() - 以旧 page 接口把 folio 插入 mapping 并加入 LRU。
 * 业务背景：尚未 folio 化的文件系统创建缓存页后进入 filemap_add_folio() 公共发布路径。
 * 入参：page 为调用者拥有的未归属成员页；mapping 为借用地址空间；index 是页偏移；
 * gfp 控制 memcg/XArray 分配。出参/返回：成功 0，失败为 -ENOMEM/-EEXIST 等 errno。
 * 注意事项：成功后 page cache 持有引用且 folio 保持 locked；失败 ownership 仍归调用者。
 */
int add_to_page_cache_lru(struct page *page, struct address_space *mapping,
		pgoff_t index, gfp_t gfp)
{
	return filemap_add_folio(mapping, page_folio(page), index, gfp);
}
EXPORT_SYMBOL(add_to_page_cache_lru);

/*
 * pagecache_get_page() - 用旧 page ABI 查找或按 flags 创建指定 index 的缓存页。
 * 业务背景：pagemap.h 的 find/get/lock page helpers 汇入 __filemap_get_folio()，再把
 * folio 引用转换为包含 index 的成员 page，避免调用者理解大 folio 索引范围。
 * 入参：mapping 是借用地址空间；index 是 page-cache 页索引；fgp_flags 控制查找、
 * 创建、锁定和等待；gfp 仅在需要分配时生效，四者均不转移 ownership。
 * 出参/返回：成功返回持有引用的成员 page，调用者负责 put_page，并按 flags 可能持锁；
 * 所有 ERR_PTR 失败（含未找到/分配失败）都折叠为 NULL，以保持旧 ABI。
 * 注意事项：noinline 避免兼容慢路径膨胀；可能分配/睡眠取决于 flags/gfp。
 */
noinline
struct page *pagecache_get_page(struct address_space *mapping, pgoff_t index,
		fgf_t fgp_flags, gfp_t gfp)
{
	/* folio 保存持有引用的查找结果；错误指针不携带可释放对象。 */
	struct folio *folio;

	/* 统一 folio 核心执行 XArray 查找、并发重试、可选创建与锁定。 */
	folio = __filemap_get_folio(mapping, index, fgp_flags, gfp);
	/* 旧 ABI 无 errno 通道，主动丢弃具体错误，仅向调用者返回 NULL。 */
	if (IS_ERR(folio))
		return NULL;
	/* 大 folio 可能覆盖多个 index；返回对应成员页，同时保留 folio 查找取得的引用。 */
	return folio_file_page(folio, index);
}
EXPORT_SYMBOL(pagecache_get_page);
