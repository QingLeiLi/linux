// SPDX-License-Identifier: GPL-2.0-only
/*
 * arm64 用户页 D-cache/I-cache 一致性维护。
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 内核经线性映射写页面后，用户可能从另一虚拟别名执行它；PoU 前的数据
 * 和指令 cache 必须同步。PG_dcache_clean 是 folio 级延迟状态：内核写入
 * 时清位，真正建立可执行 PTE 时一次性同步并置位，避免每次写都付出广播
 * 成本。持页/页表锁的上层保证 folio 不在同步期间释放或并发改变状态。
 */
/*
 * Based on arch/arm/mm/flush.c
 *
 * Copyright (C) 1995-2002 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/export.h>
#include <linux/mm.h>
#include <linux/libnvdimm.h>
#include <linux/pagemap.h>

#include <asm/cacheflush.h>
#include <asm/cache.h>
#include <asm/tlbflush.h>

/*
 * 同步 [start,end) 的数据与指令 cache 别名，地址单位为字节。aliasing
 * I-cache 必须先 clean D 到 PoU，再全局失效 I；PIPT/非别名实现可用范围
 * helper 合并操作。函数不修改页表，调用者保证范围映射有效。
 */
void sync_icache_aliases(unsigned long start, unsigned long end)
{
	if (icache_is_aliasing()) {
		dcache_clean_pou(start, end);
		icache_inval_all_pou();
	} else {
		/*
		 * Don't issue kick_all_cpus_sync() after I-cache invalidation
		 * for user mappings.
		 */
		/* 用户映射的异常返回路径已有所需同步，避免额外 kick 所有 CPU。 */
		caches_clean_inval_pou(start, end);
	}
}

/* ptrace/process_vm 等内核别名写入后，仅对可执行 VMA 做昂贵 I-cache 同步。 */
static void flush_ptrace_access(struct vm_area_struct *vma, unsigned long start,
				unsigned long end)
{
	if (vma->vm_flags & VM_EXEC)
		sync_icache_aliases(start, end);
}

/*
 * Copy user data from/to a page which is mapped into a different processes
 * address space.  Really, we want to allow our "user space" model to handle
 * this.
 */
/*
 * 在内核映射 dst 与用户 VMA 页之间复制 len 字节。page/uaddr 是通用接口
 * 上下文，本实现依靠 dst/src 实际地址；写入后若 VMA 可执行则同步刚改
 * 范围，确保用户取指不见旧代码。无返回值，页/VMA 所有权不变。
 */
void copy_to_user_page(struct vm_area_struct *vma, struct page *page,
		       unsigned long uaddr, void *dst, const void *src,
		       unsigned long len)
{
	memcpy(dst, src, len);
	flush_ptrace_access(vma, (unsigned long)dst, (unsigned long)dst + len);
}

/*
 * 在 PTE 将 folio 暴露为可执行映射前完成一次延迟 cache 同步。pte 必须
 * 指向普通可直接映射页面；PG_dcache_clean 在上层页表序列化下作为 folio
 * 状态位，未置位才同步整个 folio，随后置位避免同内容重复 flush。
 */
void __sync_icache_dcache(pte_t pte)
{
	struct folio *folio = page_folio(pte_page(pte));

	if (!test_bit(PG_dcache_clean, &folio->flags.f)) {
		sync_icache_aliases((unsigned long)folio_address(folio),
				    (unsigned long)folio_address(folio) +
					    folio_size(folio));
		set_bit(PG_dcache_clean, &folio->flags.f);
	}
}
EXPORT_SYMBOL_GPL(__sync_icache_dcache);

/*
 * This function is called when a page has been modified by the kernel. Mark
 * it as dirty for later flushing when mapped in user space (if executable,
 * see __sync_icache_dcache).
 */
/*
 * 标记 folio 已被内核修改：只清 PG_dcache_clean，不立即进行昂贵同步。
 * 调用者持有保证 folio 存活/写入有序的锁；后续可执行映射安装路径消费
 * 该脏状态。该位不等同于页回写的 dirty bit。
 */
void flush_dcache_folio(struct folio *folio)
{
	if (test_bit(PG_dcache_clean, &folio->flags.f))
		clear_bit(PG_dcache_clean, &folio->flags.f);
}
EXPORT_SYMBOL(flush_dcache_folio);

/* page 兼容包装，把 tail page 归一到所属 folio 后执行相同延迟标记。 */
void flush_dcache_page(struct page *page)
{
	flush_dcache_folio(page_folio(page));
}
EXPORT_SYMBOL(flush_dcache_page);

/*
 * Additional functions defined in assembly.
 */
/* caches_clean_inval_pou 的实现位于 cache.S，此处导出给模块使用。 */
EXPORT_SYMBOL(caches_clean_inval_pou);

#ifdef CONFIG_ARCH_HAS_PMEM_API
/*
 * 持久内存写回：addr/size 为内核虚拟字节范围。先用 outer-shareable DMB
 * 排序此前 non-cacheable 写，再 clean 到持久化点 PoP；返回只保证 cache
 * 维护完成，平台掉电持久性还依赖其 PoP 定义。
 */
void arch_wb_cache_pmem(void *addr, size_t size)
{
	/* Ensure order against any prior non-cacheable writes */
	/* DMB OSH 把外部共享域中的早先写排在随后 CVAP 之前，不能由末尾 DSB 替代。 */
	dmb(osh);
	dcache_clean_pop((unsigned long)addr, (unsigned long)addr + size);
}
EXPORT_SYMBOL_GPL(arch_wb_cache_pmem);

/* 丢弃持久内存范围的 CPU cache 副本，使后续读取观察外部更新。 */
void arch_invalidate_pmem(void *addr, size_t size)
{
	dcache_inval_poc((unsigned long)addr, (unsigned long)addr + size);
}
EXPORT_SYMBOL_GPL(arch_invalidate_pmem);
#endif
