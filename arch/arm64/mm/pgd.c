// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 用户地址空间顶级页表（PGD）的分配与回收。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 有效 VA 位数/页表级数可能让 PGD 恰好占一页，也可能只占页内一小段。
 * 前者走通用 page-table allocator，后者用按 PGD_SIZE 对齐的 slab，避免
 * 每个 mm 浪费整页。分配和释放必须由同一次 pgdir_is_page_size() 决策
 * 配对；配置/capability 在启动后稳定，因此不会跨生命周期改变分配器。
 */
/*
 * PGD allocation/freeing
 *
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/slab.h>

#include <asm/pgalloc.h>
#include <asm/page.h>
#include <asm/tlbflush.h>

/* 小 PGD 专用 slab；pgtable_cache_init() 发布后只读，slab 持有对象所有权。 */
static struct kmem_cache *pgd_cache __ro_after_init;

/*
 * 返回当前运行配置下顶级目录是否占完整 PAGE_SIZE。编译期最大级数还会
 * 被运行时 L4/L5 capability 折叠，所以不能只比较 CONFIG_PGTABLE_LEVELS。
 * 无副作用，结果在初始化完成后恒定。
 */
static bool pgdir_is_page_size(void)
{
	if (PGD_SIZE == PAGE_SIZE)
		return true;
	if (CONFIG_PGTABLE_LEVELS == 4)
		return !pgtable_l4_enabled();
	if (CONFIG_PGTABLE_LEVELS == 5)
		return !pgtable_l5_enabled();
	return false;
}

/*
 * 为 mm 分配空 PGD。mm 是新/复制地址空间，供通用页表记账使用；返回
 * pgd_t 指针或 NULL。整页路径由 __pgd_alloc 初始化架构所需内容，小对象
 * 路径从 pgd_cache 取自然对齐对象；GFP_PGTABLE_USER 体现用户页表 reclaim
 * 策略。成功后所有权交给 mm，必须以 pgd_free() 配对。
 */
pgd_t *pgd_alloc(struct mm_struct *mm)
{
	gfp_t gfp = GFP_PGTABLE_USER;

	if (pgdir_is_page_size())
		return __pgd_alloc(mm, 0);
	else
		return kmem_cache_alloc(pgd_cache, gfp);
}

/*
 * 释放 pgd_alloc() 返回的 PGD。mm 与 pgd 必须配对，且调用前页表已从
 * 硬件 TTBR/页表树摘除并完成必要 TLB 同步；本函数只归还存储，不清 TLB。
 */
void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	if (pgdir_is_page_size())
		__pgd_free(mm, pgd);
	else
		kmem_cache_free(pgd_cache, pgd);
}

/*
 * 启动期创建小 PGD slab。整页配置无需 cache；52-bit PA 要求顶级表至少
 * 64 字节对齐，BUILD_BUG_ON 在不可能满足时编译失败。SLAB_PANIC 表示
 * cache 创建失败无法继续建立进程地址空间，直接 panic 而非返回错误。
 */
void __init pgtable_cache_init(void)
{
	if (pgdir_is_page_size())
		return;

#ifdef CONFIG_ARM64_PA_BITS_52
	/*
	 * With 52-bit physical addresses, the architecture requires the
	 * top-level table to be aligned to at least 64 bytes.
	 */
	/* 编译期证明 PGD_SIZE 本身满足架构 52-bit 描述符取址对齐。 */
	BUILD_BUG_ON(!IS_ALIGNED(PGD_SIZE, 64));
#endif

	/*
	 * Naturally aligned pgds required by the architecture.
	 */
	/* size 与 align 同为 PGD_SIZE，保证每个对象不跨错误的硬件对齐边界。 */
	pgd_cache = kmem_cache_create("pgd_cache", PGD_SIZE, PGD_SIZE,
				      SLAB_PANIC, NULL);
}
