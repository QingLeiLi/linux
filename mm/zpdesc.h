/* SPDX-License-Identifier: GPL-2.0 */
/* zpdesc.h: zsmalloc pool memory descriptor
 *
 * Written by Alex Shi <alexs@kernel.org>
 *	      Hyeonggon Yoo <42.hyeyoo@gmail.com>
 */
/* 本头文件定义 zsmalloc 池页的专用描述符视图；作者信息与许可证保持原样。 */
#ifndef __MM_ZPDESC_H__
#define __MM_ZPDESC_H__
/* 保护宏避免结构体、转换宏和 static inline 包装被重复定义。 */

#include <linux/migrate.h>
#include <linux/pagemap.h>

/*
 * struct zpdesc -	Memory descriptor for zsmalloc pool memory.
 * @flags:		Page flags, mostly unused by zsmalloc.
 * @lru:		Indirectly used by page migration.
 * @movable_ops:	Used by page migration.
 * @next:		Next zpdesc in a zspage in zsmalloc pool.
 * @handle:		For huge zspage in zsmalloc pool.
 * @zspage:		Points to the zspage this zpdesc is a part of.
 * @first_obj_offset:	First object offset in zsmalloc pool.
 * @_refcount:		The number of references to this zpdesc.
 *
 * This struct overlays struct page for now. Do not modify without a good
 * understanding of the issues. In particular, do not expand into the overlap
 * with memcg_data.
 *
 * Page flags used:
 * * PG_private identifies the first component page.
 * * PG_locked is used by page migration code.
 */
/*
 * struct zpdesc 把 zsmalloc folio 的首个 struct page 原地解释为专用描述符，
 * 不额外分配 metadata。@flags 保留 page flags；@lru 与 @movable_ops 供可移动页
 * 框架使用；union 在普通 zspage 中以 @next 串页，在单对象 huge zspage 中保存
 * @handle；@zspage 反向指向所属逻辑页；@first_obj_offset 的低 24 位保存首对象
 * 字节偏移、高 8 位保留 PGTY_zsmalloc；@_refcount 仍是底层 folio/page 引用计数。
 *
 * 对象由 zsmalloc 以 order-0 folio 创建，链入 zspage 后由池持有，迁移可替换底层
 * 页，最终 reset 后 folio_put()/buddy 回收。它当前与 struct page 逐字段重叠，
 * 尤其不得扩展到 memcg_data 所占区域。PG_private 只标记链首，PG_locked 与迁移、
 * zspage 销毁互斥；字段一致性由下方编译期断言而不是 C 类型系统保证。
 */
struct zpdesc {
	/* 与 page.flags 同址；大多数普通 page flag 对 zsmalloc 没有业务含义。 */
	unsigned long flags;
	/* 与 page.lru 同址，由迁移/隔离框架临时挂链，zsmalloc 不把它当 zspage 链。 */
	struct list_head lru;
	/* 与 page.mapping 同址，为迁移框架按 struct page 布局访问保留；zpdesc 不直接读取。 */
	unsigned long movable_ops;
	union {
		/* 非 huge zspage 的下一页借用指针；由 zspage 链生命周期保护。 */
		struct zpdesc *next;
		/* 单页单对象 zspage 复用同一槽保存对象 handle，二者不会同时有效。 */
		unsigned long handle;
	};
	/* 所属 zspage 的借用反向指针；reset_zpdesc() 在释放前清空。 */
	struct zspage *zspage;
	/*
	 * Only the lower 24 bits are available for offset, limiting a page
	 * to 16 MiB. The upper 8 bits are reserved for PGTY_zsmalloc.
	 *
	 * Do not access this field directly.
	 * Instead, use {get,set}_first_obj_offset() helpers.
	 */
	/*
	 * 低 24 位最多表达 16 MiB 页内字节偏移，高 8 位属于 PGTY_zsmalloc 页类型。
	 * 直接读写会破坏页类型或把类型位误当偏移，必须经 get/set helper 掩码访问。
	 */
	unsigned int first_obj_offset;
	/* 与 page._refcount 同址；get/put 包装直接维护底层 folio 生命周期。 */
	atomic_t _refcount;
};

/*
 * ZPDESC_MATCH() 为每个复用字段生成 offsetof 静态断言：任何 struct page 布局变化
 * 若不再与 zpdesc 对齐都会在编译期失败，避免运行期把一个字段静默解释成另一个。
 */
#define ZPDESC_MATCH(pg, zp) \
	static_assert(offsetof(struct page, pg) == offsetof(struct zpdesc, zp))

/* 以下逐项验证 flags、迁移字段、union、所属 zspage、页类型/偏移和引用计数槽。 */
ZPDESC_MATCH(flags, flags);
ZPDESC_MATCH(lru, lru);
ZPDESC_MATCH(mapping, movable_ops);
ZPDESC_MATCH(__folio_index, next);
ZPDESC_MATCH(__folio_index, handle);
ZPDESC_MATCH(private, zspage);
ZPDESC_MATCH(page_type, first_obj_offset);
ZPDESC_MATCH(_refcount, _refcount);
/* 宏只服务上述布局表，立即取消定义以免污染包含者命名空间。 */
#undef ZPDESC_MATCH
/* 即使各字段偏移匹配，整体也不得越过 struct page 尾部覆盖相邻内存。 */
static_assert(sizeof(struct zpdesc) <= sizeof(struct page));

/*
 * zpdesc_page - The first struct page allocated for a zpdesc
 * @zp: The zpdesc.
 *
 * A convenience wrapper for converting zpdesc to the first struct page of the
 * underlying folio, to communicate with code not yet converted to folio or
 * struct zpdesc.
 *
 */
/*
 * zpdesc_page() - 把 zpdesc 视图转换为底层 folio 的第一个 struct page。
 * 业务背景：尚未转换到 folio/zpdesc 的 mm API 仍以 page 为边界；@zp 必须指向
 * 合法 zpdesc，_Generic 按 const/非 const 指针选择转换并保持只读属性。
 * 出参/返回：返回同一地址的借用 page 指针，不增引用、不验证页类型、无副作用。
 * 注意事项：不是任意 zspage 成员定位操作；布局重叠由静态断言保证，未来实现可变。
 */
#define zpdesc_page(zp)			(_Generic((zp),			\
	const struct zpdesc *:		(const struct page *)(zp),	\
	struct zpdesc *:		(struct page *)(zp)))

/**
 * zpdesc_folio - The folio allocated for a zpdesc
 * @zp: The zpdesc.
 *
 * Zpdescs are descriptors for zsmalloc memory. The memory itself is allocated
 * as folios that contain the zsmalloc objects, and zpdesc uses specific
 * fields in the first struct page of the folio - those fields are now accessed
 * by struct zpdesc.
 *
 * It is occasionally necessary convert to back to a folio in order to
 * communicate with the rest of the mm. Please use this helper function
 * instead of casting yourself, as the implementation may change in the future.
 */
/*
 * zpdesc_folio() - 把 zsmalloc 描述符转换为承载其对象内存的 folio。
 * 业务背景：zpdesc 当前使用 folio 首个 page 的特定槽，但通用 mm 的锁、引用与等待
 * API 操作 folio；@zp 是有效借用描述符，_Generic 保留 const 限定。
 * 出参/返回：返回同址借用 folio 指针，不增引用、无直接副作用。
 * 注意事项：只适用于该描述符确为 folio 头的当前布局；调用者应使用宏而非手写 cast。
 */
#define zpdesc_folio(zp)		(_Generic((zp),			\
	const struct zpdesc *:		(const struct folio *)(zp),	\
	struct zpdesc *:		(struct folio *)(zp)))
/**
 * page_zpdesc - Converts from first struct page to zpdesc.
 * @p: The first (either head of compound or single) page of zpdesc.
 *
 * A temporary wrapper to convert struct page to struct zpdesc in situations
 * where we know the page is the compound head, or single order-0 page.
 *
 * Long-term ideally everything would work with struct zpdesc directly or go
 * through folio to struct zpdesc.
 *
 * Return: The zpdesc which contains this page
 */
/*
 * page_zpdesc() - 从 folio 首 page 或单个 order-0 page 临时恢复 zpdesc 视图。
 * 业务背景：旧 page API 向 zsmalloc 回传底层页时需要逆转换；长期方向是直接传
 * zpdesc 或先经 folio。@p 必须是 compound head 或单页，_Generic 保留 const。
 * 出参/返回：返回包含该页的同址借用 zpdesc，不增引用、不检查 PageZsmalloc。
 * 注意事项：tail page 或普通非 zsmalloc page 会产生语义错误，不能靠 cast 修正。
 */
#define page_zpdesc(p)			(_Generic((p),			\
	const struct page *:		(const struct zpdesc *)(p),	\
	struct page *:			(struct zpdesc *)(p)))

/*
 * zpdesc_lock() - 睡眠等待并取得 zpdesc 底层 folio 的 PG_locked 位。
 * 业务背景：zspage 销毁与迁移以每个成员页锁互斥。@zpdesc 是带有效生命周期的借用指针。
 * 出参/返回：无直接返回；成功时当前执行流持锁，ownership/引用不变。
 * 注意事项：可能睡眠，不能在原子上下文调用；必须由同页 zpdesc_unlock() 配对。
 */
static inline void zpdesc_lock(struct zpdesc *zpdesc)
{
	folio_lock(zpdesc_folio(zpdesc));
}

/*
 * zpdesc_trylock() - 非阻塞尝试取得 zpdesc 的 folio 锁。
 * 业务背景：迁移扫描需在竞争时回退而不能等待。@zpdesc 为稳定借用描述符。
 * 出参/返回：成功返回 true 且调用者持锁，失败返回 false 且状态/ownership 不变。
 * 注意事项：成功分支必须 unlock；返回 false 不能推断锁持有者或对象后续状态。
 */
static inline bool zpdesc_trylock(struct zpdesc *zpdesc)
{
	return folio_trylock(zpdesc_folio(zpdesc));
}

/*
 * zpdesc_unlock() - 释放当前执行流持有的 zpdesc folio 锁并唤醒 waiter。
 * 业务背景：结束迁移、链替换或销毁临界区。@zpdesc 为借用且必须对应已持锁对象。
 * 出参/返回：无直接返回；清 PG_locked，使并发 wait/trylock 得以前进。
 * 注意事项：不减少 folio 引用；未持锁、错误页或重复解锁会破坏锁协议。
 */
static inline void zpdesc_unlock(struct zpdesc *zpdesc)
{
	folio_unlock(zpdesc_folio(zpdesc));
}

/*
 * zpdesc_wait_locked() - 等待 zpdesc 当前锁持有者释放 folio 锁。
 * 业务背景：lock_zspage() 在放开 zspage 读锁后等待竞争页。@zpdesc 必须由额外引用固定。
 * 出参/返回：无直接返回；返回仅表示观察到解锁，不会替调用者取得锁。
 * 注意事项：可能睡眠；等待期间对象可变化，跨等待裸指针必须由 zpdesc_get() 保护。
 */
static inline void zpdesc_wait_locked(struct zpdesc *zpdesc)
{
	folio_wait_locked(zpdesc_folio(zpdesc));
}

/*
 * zpdesc_get() - 增加承载 zpdesc 的 folio 引用。
 * 业务背景：跨越释放 zspage 锁、等待或迁移窗口时固定底层页。@zpdesc 为借用输入。
 * 出参/返回：无直接返回；成功后调用者持有一份新引用，必须由 zpdesc_put() 归还。
 * 注意事项：对象引用必须尚未归零；引用只保生命周期，不冻结 next/zspage 等字段。
 */
static inline void zpdesc_get(struct zpdesc *zpdesc)
{
	folio_get(zpdesc_folio(zpdesc));
}

/*
 * zpdesc_put() - 归还一份 zpdesc 底层 folio 引用。
 * 业务背景：与 get 或分配路径持有配对，zspage 销毁最终靠它把页交还 buddy。
 * @zpdesc 是调用前仍有效且当前持有引用的输入。无直接返回；引用可能降到零并释放页。
 * 注意事项：put 后不能再解引用裸指针，除非调用者另有引用或更强生命周期保护。
 */
static inline void zpdesc_put(struct zpdesc *zpdesc)
{
	folio_put(zpdesc_folio(zpdesc));
}

/*
 * kmap_local_zpdesc() - 为 zpdesc 对应 page 建立仅当前执行上下文可用的内核映射。
 * 业务背景：zsmalloc 访问对象字节时需兼容 highmem。@zpdesc 为生命周期稳定的借用输入。
 * 出参/返回：返回页起始虚拟地址，不接管页引用；调用者必须用 kunmap_local() 配对。
 * 注意事项：映射作用域应短且不可跨任务迁移给别的上下文，函数本身不报告失败。
 */
static inline void *kmap_local_zpdesc(struct zpdesc *zpdesc)
{
	return kmap_local_page(zpdesc_page(zpdesc));
}

/*
 * zpdesc_pfn() - 取得 zpdesc 底层 page 的物理页帧号。
 * 业务背景：zsmalloc handle 编码与对象定位需要 PFN。@zpdesc 是稳定借用描述符。
 * 出参/返回：返回 unsigned long PFN，无引用、锁或字段副作用。
 * 注意事项：PFN 只在页未迁移/释放的保护窗口内代表该对象，不能替代页引用。
 */
static inline unsigned long zpdesc_pfn(struct zpdesc *zpdesc)
{
	return page_to_pfn(zpdesc_page(zpdesc));
}

/*
 * pfn_zpdesc() - 把有效 PFN 对应的首 page 解释为 zpdesc。
 * 业务背景：解码 zsmalloc object handle 后恢复其物理页描述符。@pfn 为物理页帧号。
 * 出参/返回：返回借用 zpdesc 指针，不增引用、不验证 PageZsmalloc。
 * 注意事项：调用者须先证明 PFN 有效且当前属于 zsmalloc；迁移/释放须由外层同步排除。
 */
static inline struct zpdesc *pfn_zpdesc(unsigned long pfn)
{
	return page_zpdesc(pfn_to_page(pfn));
}

/*
 * __zpdesc_set_movable() - 把 zpdesc 登记到 page migration 的 movable_ops 分派。
 * 业务背景：启用 compaction 时 zsmalloc 页需由 migrate.c 找到 zsmalloc 回调。
 * @zpdesc 为已锁定、生命周期稳定的输入输出描述符。无直接返回；设置 PG_movable_ops，
 * 具体回调类型仍由已经写入的 PGTY_zsmalloc 选择。
 * 注意事项：__ 前缀表示调用者负责 folio 锁和一次性初始化，发布后迁移路径可观察。
 */
static inline void __zpdesc_set_movable(struct zpdesc *zpdesc)
{
	SetPageMovableOps(zpdesc_page(zpdesc));
}

/*
 * __zpdesc_set_zsmalloc() - 设置底层 page 的 PGTY_zsmalloc 类型标记。
 * 业务背景：分配 zspage 成员后，偏移 helper、迁移与诊断据此识别 zsmalloc 页。
 * @zpdesc 为尚未发布的新页描述符。无直接返回；高类型位变为 zsmalloc 且保持到 buddy 释放。
 * 注意事项：必须早于写 first_obj_offset 和链发布；reset_zpdesc() 不清除此 sticky 标记。
 */
static inline void __zpdesc_set_zsmalloc(struct zpdesc *zpdesc)
{
	__SetPageZsmalloc(zpdesc_page(zpdesc));
}

/*
 * zpdesc_zone() - 查询 zpdesc 当前物理页所属 zone。
 * 业务背景：分配、释放与迁移后需更新正确 zone 的 NR_ZSPAGES。@zpdesc 为稳定借用对象。
 * 出参/返回：返回借用 struct zone 指针，不增引用、无副作用。
 * 注意事项：迁移替换页后 zone 可能改变；调用者应在相应锁/引用窗口内立即使用。
 */
static inline struct zone *zpdesc_zone(struct zpdesc *zpdesc)
{
	return page_zone(zpdesc_page(zpdesc));
}

/*
 * zpdesc_is_locked() - 读取 zpdesc folio 当前 PG_locked 状态。
 * 业务背景：销毁路径用它断言所有 zspage 成员已锁。@zpdesc 为稳定借用输入。
 * 出参/返回：锁位已置返回 true，否则 false；无引用或状态副作用。
 * 注意事项：这是瞬时快照，不授予锁 ownership；正确性断言还依赖外层 zspage 同步。
 */
static inline bool zpdesc_is_locked(struct zpdesc *zpdesc)
{
	return folio_test_locked(zpdesc_folio(zpdesc));
}

/* 结束 zpdesc 专用布局与包装接口；所有函数均为头文件内联实现。 */
#endif
