// SPDX-License-Identifier: GPL-2.0-only
/* Page fragment allocator
 *
 * Page Fragment:
 *  An arbitrary-length arbitrary-offset area of memory which resides within a
 *  0 or higher order page.  Multiple fragments within that page are
 *  individually refcounted, in the page's reference counter.
 *
 * The page_frag functions provide a simple allocation framework for page
 * fragments.  This is used by the network stack and network device drivers to
 * provide a backing region of memory for use as either an sk_buff->head, or to
 * be used in the "frags" portion of skb_shared_info.
 */
/*
 * 中文概览：缓存以一页或高阶复合页为 backing，从低地址向高地址切出网络缓冲区。
 * 每个已返回 fragment 最终以 page_frag_free() 归还一次页引用；pagecnt_bias 把多次
 * 引用增减合并成批量操作，减少对 page->_refcount 所在缓存线的争用。
 */

#include <linux/build_bug.h>
#include <linux/export.h>
#include <linux/gfp_types.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/page_frag_cache.h>
#include "internal.h"

static unsigned long encoded_page_create(struct page *page, unsigned int order,
					 bool pfmemalloc)
{
	/*
	 * 页虚拟地址按 PAGE_SIZE 对齐，其低位可编码 order 和 pfmemalloc。编译期断言
	 * 保证两个字段互不重叠且都落在地址天然为零的页内偏移位中。
	 */
	BUILD_BUG_ON(PAGE_FRAG_CACHE_MAX_ORDER > PAGE_FRAG_CACHE_ORDER_MASK);
	BUILD_BUG_ON(PAGE_FRAG_CACHE_PFMEMALLOC_BIT >= PAGE_SIZE);

	/* page_address 要求这里的 backing 可由线性映射直接访问。 */
	return (unsigned long)page_address(page) |
		(order & PAGE_FRAG_CACHE_ORDER_MASK) |
		((unsigned long)pfmemalloc * PAGE_FRAG_CACHE_PFMEMALLOC_BIT);
}

static unsigned long encoded_page_decode_order(unsigned long encoded_page)
{
	/* order 位只在 PAGE_SIZE 小于最大缓存大小时存在，否则掩码恒为 0。 */
	return encoded_page & PAGE_FRAG_CACHE_ORDER_MASK;
}

static void *encoded_page_decode_virt(unsigned long encoded_page)
{
	/* PAGE_MASK 同时清除 order 与 pfmemalloc，恢复 backing 的页对齐虚拟地址。 */
	return (void *)(encoded_page & PAGE_MASK);
}

static struct page *encoded_page_decode_page(unsigned long encoded_page)
{
	/* virt_to_page 忽略页内低位，因此可直接接收带编码的线性映射地址。 */
	return virt_to_page((void *)encoded_page);
}

/*
 * __page_frag_cache_refill() - 为耗尽或空的 fragment cache 准备新 backing。
 *
 * nc 由调用者独占或在外部串行化，gfp_mask 是原始分配约束。小页系统先以不直接
 * reclaim、不告警、不重试且不动用紧急储备的方式尝试最大阶复合页；失败再用原始
 * GFP 标志申请 order-0 页。成功把页、阶数和 pfmemalloc 来源编码进 nc，失败清零；
 * 返回的是借用页指针，引用和 bias 的建立由 __page_frag_alloc_align() 完成。
 */
static struct page *__page_frag_cache_refill(struct page_frag_cache *nc,
					     gfp_t gfp_mask)
{
	unsigned long order = PAGE_FRAG_CACHE_MAX_ORDER;
	struct page *page = NULL;
	gfp_t gfp = gfp_mask;

#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	/* 高阶尝试必须廉价失败，避免仅为缓存大块而触发昂贵 reclaim 或保留内存。 */
	gfp_mask = (gfp_mask & ~__GFP_DIRECT_RECLAIM) |  __GFP_COMP |
		   __GFP_NOWARN | __GFP_NORETRY | __GFP_NOMEMALLOC;
	page = __alloc_pages(gfp_mask, PAGE_FRAG_CACHE_MAX_ORDER,
			     numa_mem_id(), NULL);
#endif
	if (unlikely(!page)) {
		/* order-0 回退保留调用者原始 GFP 语义，因此可能得到 pfmemalloc 页。 */
		page = __alloc_pages(gfp, 0, numa_mem_id(), NULL);
		order = 0;
	}

	/* 以 0 作为“缓存无 backing”的唯一状态，便于下一次分配重新 refill。 */
	nc->encoded_page = page ?
		encoded_page_create(page, order, page_is_pfmemalloc(page)) : 0;

	return page;
}

/*
 * page_frag_cache_drain() - 销毁 cache 对当前 backing 的保留状态。
 *
 * nc 必须已由调用者停止并发分配；函数结算剩余 pagecnt_bias 并清除 encoded_page。
 * 外部尚持有的 fragment 继续各自拥有页引用，故 drain 返回不代表页必然已释放。
 */
void page_frag_cache_drain(struct page_frag_cache *nc)
{
	/* 空缓存无需归还 bias；非空时 drain 会结算尚未物化为 fragment 的引用。 */
	if (!nc->encoded_page)
		return;

	__page_frag_cache_drain(encoded_page_decode_page(nc->encoded_page),
				nc->pagecnt_bias);
	/* 清零解除 cache 对 backing 的所有权；offset/bias 的旧值随后不再可用。 */
	nc->encoded_page = 0;
}
EXPORT_SYMBOL(page_frag_cache_drain);

/*
 * __page_frag_cache_drain() - 从指定 backing 批量扣除 count 份缓存偏置引用。
 *
 * page/count 必须来自同一活动缓存快照；函数不清理任何 page_frag_cache 字段。
 * 若扣减使引用归零则释放整页，否则剩余引用继续由已交付 fragment 等持有。
 */
void __page_frag_cache_drain(struct page *page, unsigned int count)
{
	/* count 是缓存尚未消费的引用偏置，调用者保证 page/count 属于同一 backing。 */
	VM_BUG_ON_PAGE(page_ref_count(page) == 0, page);

	/* 最后引用归零时页处于 frozen 状态，按真实 compound order 归还伙伴系统。 */
	if (page_ref_sub_and_test(page, count))
		free_frozen_pages(page, compound_order(page));
}
EXPORT_SYMBOL(__page_frag_cache_drain);

/*
 * __page_frag_alloc_align() - 从 cache 分配一个满足掩码对齐的连续 fragment。
 *
 * fragsz/offset 加法须在 unsigned int 可表示范围内，align_mask 由公开包装转换为
 * 内部反掩码形式；nc 需要由调用者串行使用。成功返回线性映射中的 fragment 地址
 * 并转交一份页引用，失败返回 NULL 且不产生待释放 fragment。cache 可能保留旧小页，
 * 或在页耗尽时批量结算 bias、复用无外部引用的普通页/替换仍被引用或 pfmemalloc 页。
 */
void *__page_frag_alloc_align(struct page_frag_cache *nc,
			      unsigned int fragsz, gfp_t gfp_mask,
			      unsigned int align_mask)
{
	/* encoded_page 是本次操作快照；size/offset 均以字节计。 */
	unsigned long encoded_page = nc->encoded_page;
	unsigned int size, offset;
	struct page *page;

	if (unlikely(!encoded_page)) {
refill:
		/* refill 失败不改变调用者对 fragment 的 ownership，直接返回 NULL。 */
		page = __page_frag_cache_refill(nc, gfp_mask);
		if (!page)
			return NULL;

		encoded_page = nc->encoded_page;

		/* Even if we own the page, we do not use atomic_set().
		 * This would break get_page_unless_zero() users.
		 */
		/*
		 * 分配器交付的初始引用仍在；额外预充 MAX_SIZE 个引用作为 bias。使用 add
		 * 而非 set，避免与观察非零引用的并发用户破坏引用计数协议。
		 */
		page_ref_add(page, PAGE_FRAG_CACHE_MAX_SIZE);

		/* reset page count bias and offset to start of new frag */
		nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
		nc->offset = 0;
	}

	size = PAGE_SIZE << encoded_page_decode_order(encoded_page);
	/* align_mask 采用反掩码接口：~align_mask 是常规 alignment-1 掩码。 */
	offset = __ALIGN_KERNEL_MASK(nc->offset, ~align_mask);
	if (unlikely(offset + fragsz > size)) {
		if (unlikely(fragsz > PAGE_SIZE)) {
			/*
			 * The caller is trying to allocate a fragment
			 * with fragsz > PAGE_SIZE but the cache isn't big
			 * enough to satisfy the request, this may
			 * happen in low memory conditions.
			 * We don't release the cache page because
			 * it could make memory pressure worse
			 * so we simply return NULL here.
			 */
			/*
			 * 这是高阶快速尝试失败后留下 order-0 backing 的退化情形。保留小页可
			 * 服务后续普通请求，也避免在内存压力下反复释放/重分配。
			 */
			return NULL;
		}

		page = encoded_page_decode_page(encoded_page);

		/*
		 * 一次性扣除尚未被 fragment 消费的 bias。非零说明旧 fragment 仍在外部，
		 * cache 不能复用该页，只能转去申请新 backing。
		 */
		if (!page_ref_sub_and_test(page, nc->pagecnt_bias))
			goto refill;

		if (unlikely(encoded_page_decode_pfmemalloc(encoded_page))) {
			/* 紧急储备页不长期滞留在缓存中，耗尽后立即释放并换页。 */
			free_frozen_pages(page,
					encoded_page_decode_order(encoded_page));
			goto refill;
		}

		/* OK, page count is 0, we can safely set it */
		/* 无外部 fragment 后独占旧页，可直接重建初始引用+bias 并从头复用。 */
		set_page_count(page, PAGE_FRAG_CACHE_MAX_SIZE + 1);

		/* reset page count bias and offset to start of new frag */
		nc->pagecnt_bias = PAGE_FRAG_CACHE_MAX_SIZE + 1;
		offset = 0;
	}

	/* 每交付一个 fragment 就把一份预充引用从 cache bias 转为调用者所有。 */
	nc->pagecnt_bias--;
	nc->offset = offset + fragsz;

	/* 返回 backing 内的借用地址；调用者最终必须且只能 page_frag_free() 一次。 */
	return encoded_page_decode_virt(encoded_page) + offset;
}
EXPORT_SYMBOL(__page_frag_alloc_align);

/*
 * Frees a page fragment allocated out of either a compound or order 0 page.
 */
void page_frag_free(void *addr)
{
	/* fragment 可位于复合页任意偏移，先恢复 head page 再归还它持有的一份引用。 */
	struct page *page = virt_to_head_page(addr);

	/* 最后一个 fragment/缓存引用消失时按复合页阶数归还 frozen backing。 */
	if (unlikely(put_page_testzero(page)))
		free_frozen_pages(page, compound_order(page));
}
EXPORT_SYMBOL(page_frag_free);
