// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 页面复制及 MTE allocation-tag 继承。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 普通 copy_page() 只复制数据字节；Memory Tagging Extension 的 tag 存在
 * 独立 tag storage，迁移/COW 后还要显式复制。hugetlb 的 tagged 状态以
 * 整个 folio 管理，必须一次复制所有子页；普通页则逐页处理。KASAN 硬件
 * tag 和用户 MTE tag 语义不同，目标页的 KASAN tag 先重置再处理 MTE。
 */
/*
 * Based on arch/arm/mm/copypage.c
 *
 * Copyright (C) 2002 Deep Blue Solutions Ltd, All Rights Reserved.
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/bitops.h>
#include <linux/mm.h>

#include <asm/page.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/mte.h>

/*
 * 复制 from 页面内容及适用的 MTE tags 到 to。两页由调用者持有并保证
 * 不并发修改，均可通过直接映射访问；函数无返回值，失败/不支持 tagging
 * 时至少数据复制已完成。hugetlb 仅从已标记 folio 的第一个源页触发整
 * folio tag 复制，避免每个子页重复全量工作。
 */
void copy_highpage(struct page *to, struct page *from)
{
	/* kto/kfrom 是直接映射地址；src/dst 是页面所属 folio，可能是 hugetlb。 */
	void *kto = page_address(to);
	void *kfrom = page_address(from);
	struct folio *src = page_folio(from);
	struct folio *dst = page_folio(to);
	unsigned int i, nr_pages;

	copy_page(kto, kfrom);
	/* 数据先复制，tag 状态随后发布；读者不能在函数完成前观察目标页。 */

	if (kasan_hw_tags_enabled())
		page_kasan_tag_reset(to);

	if (!system_supports_mte())
		return;

	if (folio_test_hugetlb(src)) {
		/* 只有 folio 首个源页负责复制，且源 folio 必须已发布 tagged 状态。 */
		if (!folio_test_hugetlb_mte_tagged(src) ||
		    from != folio_page(src, 0))
			return;

		folio_try_hugetlb_mte_tagging(dst);

		/*
		 * Populate tags for all subpages.
		 *
		 * Don't assume the first page is head page since
		 * huge page copy may start from any subpage.
		 */
		/* page_address(folio_page(...)) 显式逐子页，不能假设传入 from 是 head。 */
		nr_pages = folio_nr_pages(src);
		for (i = 0; i < nr_pages; i++) {
			kfrom = page_address(folio_page(src, i));
			kto = page_address(folio_page(dst, i));
			mte_copy_page_tags(kto, kfrom);
		}
		folio_set_hugetlb_mte_tagged(dst);
	} else if (page_mte_tagged(from)) {
		/*
		 * Most of the time it's a new page that shouldn't have been
		 * tagged yet. However, folio migration can end up reusing the
		 * same page without untagging it. Ignore the warning if the
		 * page is already tagged.
		 */
		/* migration 可能复用带 tag 目标页，try helper 允许该状态而不误报。 */
		try_page_mte_tagging(to);

		mte_copy_page_tags(kto, kfrom);
		set_page_mte_tagged(to);
	}
}
EXPORT_SYMBOL(copy_highpage);

/*
 * 用户映射页面复制包装。vaddr/vma 是通用 MM 接口参数，当前 arm64 的复制
 * 不依赖其值；数据与 MTE tag 完成后 flush_dcache_page() 把目标标成需要
 * 在首次可执行用户映射前完成 I/D cache 一致化。无返回值，页面所有权不变。
 */
void copy_user_highpage(struct page *to, struct page *from,
			unsigned long vaddr, struct vm_area_struct *vma)
{
	copy_highpage(to, from);
	flush_dcache_page(to);
}
EXPORT_SYMBOL_GPL(copy_user_highpage);
