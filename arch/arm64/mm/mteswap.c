// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 MTE allocation tags 的 swap 旁路存储。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * swap 只保存页面数据，不保存 MTE 每 16 字节一个的 4-bit allocation tag。
 * 本文件以 swap entry.val 为 XArray 索引，另存每页 tag blob；换入时恢复，
 * swap slot 失效时删除。XArray 自带并发锁，单项 xa_store/erase 可并发；
 * 整个 swap type 批量清理显式持 xa_lock 并使用内部 __xa_erase。
 *
 * 生命周期：arch_prepare_to_swap() 分配并发布 blob -> mte_restore_tags()
 * 可重复读取恢复 -> mte_invalidate_tags[_area]() 从索引摘除并立即释放。
 * XArray 锁保护可发现性，但 xa_load 返回后没有额外引用，因此上层 swap
 * 生命周期必须保证恢复与 slot 失效不并发释放同一 blob。
 */

#include <linux/pagemap.h>
#include <linux/xarray.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <asm/mte.h>

/* 全局索引：key 是含 type+offset 的 swap entry.val，value 是 kmalloc tag blob。 */
static DEFINE_XARRAY(mte_pages);

/*
 * 分配恰好容纳一页 MTE tags 的缓冲。每 16 字节 granule 一个 4-bit tag，
 * 两个 tag/字节，MTE_PAGE_TAG_STORAGE 给出固定字节数。返回指针或 NULL，
 * GFP_KERNEL 允许睡眠；成功所有权交给调用者，须用 mte_free_tag_storage。
 */
void *mte_allocate_tag_storage(void)
{
	/* tags granule is 16 bytes, 2 tags stored per byte */
	/* 每个 tag 占 4 bit，故一页 tag blob 的大小是 PAGE_SIZE/16/2。 */
	return kmalloc(MTE_PAGE_TAG_STORAGE, GFP_KERNEL);
}

/* 释放 tag blob；storage 可为 NULL，kfree 将其视为空操作。 */
void mte_free_tag_storage(char *storage)
{
	kfree(storage);
}

/*
 * 把已带 MTE tag 的 page 保存到其当前 swap slot。未 tagged 返回 0 且不
 * 建索引；分配失败返回 -ENOMEM。成功时 xa_store 接管新 blob 的可发现性，
 * 若 key 已有旧 blob 则原子替换后释放旧值。xa_store 错误不发布新值，
 * 当前函数释放新 blob 并返回其 errno。
 */
int mte_save_tags(struct page *page)
{
	void *tag_storage, *ret;

	if (!page_mte_tagged(page))
		return 0;

	tag_storage = mte_allocate_tag_storage();
	if (!tag_storage)
		return -ENOMEM;

	mte_save_page_tags(page_address(page), tag_storage);

	/* lookup the swap entry.val from the page */
	/* page 必须已绑定稳定 swap entry；该复合值同时区分 swap type 和 offset。 */
	ret = xa_store(&mte_pages, page_swap_entry(page).val, tag_storage,
		       GFP_KERNEL);
	if (WARN(xa_is_err(ret), "Failed to store MTE tags")) {
		mte_free_tag_storage(tag_storage);
		return xa_err(ret);
	} else if (ret) {
		/* Entry is being replaced, free the old entry */
		/* xa_store 返回被替换的旧指针，其索引引用已消失，可按上层串行保证释放。 */
		mte_free_tag_storage(ret);
	}

	return 0;
}

/*
 * 从 entry 对应 blob 恢复 page 的硬件 tag storage。无 blob 是合法无 tag
 * 页面；try_page_mte_tagging() 仲裁页面 tagged 状态，成功才写 tags 并发布
 * PG_mte_tagged。函数不删除 blob，因为同一 swap slot 数据在 swap cache
 * 生命周期内可能需要再次恢复；失效路径负责最终回收。
 */
void mte_restore_tags(swp_entry_t entry, struct page *page)
{
	void *tags = xa_load(&mte_pages, entry.val);

	if (!tags)
		return;

	if (try_page_mte_tagging(page)) {
		mte_restore_page_tags(page_address(page), tags);
		set_page_mte_tagged(page);
	}
}

/*
 * 使一个 swap slot 的 tag 失效。type/offset 组成唯一 entry；xa_erase 原子
 * 摘除并返回旧 blob，随后释放。不存在时 tags=NULL，仍是安全空操作。
 */
void mte_invalidate_tags(int type, pgoff_t offset)
{
	swp_entry_t entry = swp_entry(type, offset);
	void *tags = xa_erase(&mte_pages, entry.val);

	mte_free_tag_storage(tags);
}

/* page 包装：从 page private swap entry 拆出 type/offset 后调用公共失效接口。 */
static inline void __mte_invalidate_tags(struct page *page)
{
	swp_entry_t entry = page_swap_entry(page);

	mte_invalidate_tags(swp_type(entry), swp_offset(entry));
}

/*
 * 删除整个 swap type 的所有 tag blob，通常在 swapoff/设备移除时调用。
 * [swp_entry(type,0), swp_entry(type+1,0)) 是该 type 的连续 key 区间。
 * 显式 xa_lock 后必须用不重复加锁的 __xa_erase；值从树中摘除后立即 kfree。
 * 返回时该 type 无可发现 tag，其他 type 不受影响。
 */
void mte_invalidate_tags_area(int type)
{
	swp_entry_t entry = swp_entry(type, 0);
	swp_entry_t last_entry = swp_entry(type + 1, 0);
	void *tags;

	XA_STATE(xa_state, &mte_pages, entry.val);

	xa_lock(&mte_pages);
	xas_for_each(&xa_state, tags, last_entry.val - 1) {
		__xa_erase(&mte_pages, xa_state.xa_index);
		mte_free_tag_storage(tags);
	}
	xa_unlock(&mte_pages);
}

/*
 * folio 写入 swap 前逐页保存 tags。无 MTE 硬件直接成功；任一页失败时，
 * 逆向删除本次已成功发布的前 i 页 blob，形成事务式“全保存或全不保存”。
 * folio 必须已拥有稳定连续 swap entries；返回 0 或首个负错误码。
 */
int arch_prepare_to_swap(struct folio *folio)
{
	long i, nr;
	int err;

	if (!system_supports_mte())
		return 0;

	nr = folio_nr_pages(folio);

	for (i = 0; i < nr; i++) {
		err = mte_save_tags(folio_page(folio, i));
		if (err)
			goto out;
	}
	return 0;

out:
	/* i 当前指向失败页，while(i--) 仅回滚 [0,i) 已发布项，不碰旧的其他 slot。 */
	while (i--)
		__mte_invalidate_tags(folio_page(folio, i));
	return err;
}

/*
 * 换入 folio 时从起始 entry 逐页恢复 tag；swap offset 对每个子页连续递增。
 * 无 MTE 时空操作。该函数不使 slot 失效，swap 核心稍后的生命周期回调
 * 决定何时删除 blob，从而避免恢复与 swap-cache 重用之间过早释放。
 */
void arch_swap_restore(swp_entry_t entry, struct folio *folio)
{
	long i, nr;

	if (!system_supports_mte())
		return;

	nr = folio_nr_pages(folio);

	for (i = 0; i < nr; i++) {
		mte_restore_tags(entry, folio_page(folio, i));
		entry.val++;
	}
}
