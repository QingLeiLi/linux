// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains KASAN runtime code that manages shadow memory for
 * generic and software tag-based KASAN modes.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * Some code borrowed from https://github.com/xairy/kasan-prototype by
 *        Andrey Konovalov <andreyknvl@gmail.com>
 */
/*
 * 本文件管理 Generic 与软件 tag KASAN 的 shadow 映射，并提供编译器 mem* ABI；
 * 原版权、作者和借用来源保持不变。主线是检查实际访问、写 shadow 编码，以及为
 * hotplug/vmalloc/module 动态建立和撤销 shadow 页表。
 */

#include <linux/init.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/kfence.h>
#include <linux/kmemleak.h>
/* memory/mm 提供 hotplug 通知、页表遍历与 shadow backing page 生命周期。 */
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include <asm/cacheflush.h>
#include <asm/tlbflush.h>

#include "kasan.h"

/*
 * 业务背景：显式读检查 API 把调用点和读语义交给模式实现。
 * 入参：p 为借用地址，size 为字节数；出参/返回：范围可继续访问返回 true，否则 false。
 * 注意事项：不取得内存引用；并发释放仍由调用者同步，异常可能产生 KASAN 报告。
 */
bool __kasan_check_read(const volatile void *p, unsigned int size)
{
	return kasan_check_range((void *)p, size, false, _RET_IP_);
}
EXPORT_SYMBOL(__kasan_check_read);

/*
 * 业务背景：显式写检查 API 与 read 对称，但报告标记为写访问。
 * 入参：p 为借用地址，size 为字节数；出参/返回：合法 true，非法 false。
 * 注意事项：只检查不写数据，_RET_IP_ 保留真实调用点。
 */
bool __kasan_check_write(const volatile void *p, unsigned int size)
{
	return kasan_check_range((void *)p, size, true, _RET_IP_);
}
EXPORT_SYMBOL(__kasan_check_write);

#if !defined(CONFIG_CC_HAS_KASAN_MEMINTRINSIC_PREFIX) && !defined(CONFIG_GENERIC_ENTRY)
/*
 * CONFIG_GENERIC_ENTRY relies on compiler emitted mem*() calls to not be
 * instrumented. KASAN enabled toolchains should emit __asan_mem*() functions
 * for the sites they want to instrument.
 *
 * If we have a compiler that can instrument meminstrinsics, never override
 * these, so that non-instrumented files can safely consider them as builtins.
 */
/*
 * GENERIC_ENTRY 假设编译器产生的 mem* 不被插桩；支持 memintrinsic 前缀的工具链会生成
 * __asan_mem*。只有两者都不具备时才覆盖普通 memset/memcpy/memmove，避免未插桩文件把
 * 内建函数意外递归到 KASAN 包装。
 */
#undef memset
/*
 * 业务背景：旧工具链下为普通 memset 增加目标写范围检查。
 * 入参：addr/c/len 同 memset，addr 为借用目标；出参/返回：合法返回 addr，失败 NULL。
 * 注意事项：检查通过后调用未插桩 __memset，避免包装自身递归。
 */
void *memset(void *addr, int c, size_t len)
{
	if (!kasan_check_range(addr, len, true, _RET_IP_))
		return NULL;

	return __memset(addr, c, len);
}

#ifdef __HAVE_ARCH_MEMMOVE
#undef memmove
/*
 * 业务背景：为可能重叠的 memmove 同时检查源读与目标写。
 * 入参：dest/src 为借用范围，len 为字节数；出参/返回：成功 dest，任一失败 NULL。
 * 注意事项：短路检查后调用体系结构 __memmove，重叠语义由底层保持。
 */
void *memmove(void *dest, const void *src, size_t len)
{
	if (!kasan_check_range(src, len, false, _RET_IP_) ||
	    !kasan_check_range(dest, len, true, _RET_IP_))
		return NULL;

	return __memmove(dest, src, len);
}
#endif

#undef memcpy
/*
 * 业务背景：为普通 memcpy 增加源读和目标写 shadow 检查。
 * 入参：dest/src/len 同 memcpy；出参/返回：成功 dest，非法范围 NULL。
 * 注意事项：调用者仍须保证不重叠；KASAN 只验证可访问性。
 */
void *memcpy(void *dest, const void *src, size_t len)
{
	if (!kasan_check_range(src, len, false, _RET_IP_) ||
	    !kasan_check_range(dest, len, true, _RET_IP_))
		return NULL;

	return __memcpy(dest, src, len);
}
#endif

/*
 * 业务背景：编译器明确插桩的 memset ABI，不依赖是否覆盖普通 libc 名称。
 * 入参：addr/c/len 同 memset；出参/返回：成功 addr，检查失败 NULL。
 * 注意事项：len 为 ssize_t 但编译器保证非负；实际写由 __memset 完成。
 */
void *__asan_memset(void *addr, int c, ssize_t len)
{
	if (!kasan_check_range(addr, len, true, _RET_IP_))
		return NULL;

	return __memset(addr, c, len);
}
EXPORT_SYMBOL(__asan_memset);

#ifdef __HAVE_ARCH_MEMMOVE
/*
 * 业务背景：编译器 memmove intrinsic 的 KASAN ABI。
 * 入参：dest/src 为借用范围，len 为非负字节数；出参/返回：成功 dest，失败 NULL。
 * 注意事项：先读后写检查，底层 __memmove 保证重叠复制。
 */
void *__asan_memmove(void *dest, const void *src, ssize_t len)
{
	if (!kasan_check_range(src, len, false, _RET_IP_) ||
	    !kasan_check_range(dest, len, true, _RET_IP_))
		return NULL;

	return __memmove(dest, src, len);
}
EXPORT_SYMBOL(__asan_memmove);
#endif

/*
 * 业务背景：编译器 memcpy intrinsic 的 KASAN ABI。
 * 入参：dest/src 为借用不重叠范围；len 为非负字节数。
 * 出参/返回：检查通过返回 dest，否则 NULL；不改变 ownership。
 * 注意事项：只调用未插桩 __memcpy，防止检测路径递归。
 */
void *__asan_memcpy(void *dest, const void *src, ssize_t len)
{
	if (!kasan_check_range(src, len, false, _RET_IP_) ||
	    !kasan_check_range(dest, len, true, _RET_IP_))
		return NULL;

	return __memcpy(dest, src, len);
}
EXPORT_SYMBOL(__asan_memcpy);

#ifdef CONFIG_KASAN_SW_TAGS
/* HWASan 名称是软件 tag 工具链 ABI，直接别名到同语义的 ASan mem* 包装。 */
void *__hwasan_memset(void *addr, int c, ssize_t len) __alias(__asan_memset);
EXPORT_SYMBOL(__hwasan_memset);
#ifdef __HAVE_ARCH_MEMMOVE
void *__hwasan_memmove(void *dest, const void *src, ssize_t len) __alias(__asan_memmove);
EXPORT_SYMBOL(__hwasan_memmove);
#endif
void *__hwasan_memcpy(void *dest, const void *src, ssize_t len) __alias(__asan_memcpy);
EXPORT_SYMBOL(__hwasan_memcpy);
#endif

/*
 * 业务背景：把粒度对齐应用范围的 shadow 全部写为指定毒码或 tag。
 * 入参：addr 为可带 tag 的借用应用地址；size 为对齐字节数；value 为 shadow 值；init 为模式参数。
 * 出参/返回：无；KASAN 关闭或未对齐时不变，成功批量更新 shadow。
 * 注意事项：先 reset tag 再换算；调用者负责对象生命周期，init 当前不影响本实现写法。
 */
void kasan_poison(const void *addr, size_t size, u8 value, bool init)
{
	void *shadow_start, *shadow_end;

	if (!kasan_enabled())
		return;

	/*
	 * Perform shadow offset calculation based on untagged address, as
	 * some of the callers (e.g. kasan_poison_new_object) pass tagged
	 * addresses to this function.
	 */
	/* tagged caller 很常见；页表索引必须使用去 tag 的规范地址。 */
	addr = kasan_reset_tag(addr);

	if (WARN_ON((unsigned long)addr & KASAN_GRANULE_MASK))
		return;
	if (WARN_ON(size & KASAN_GRANULE_MASK))
		return;

	/* shadow_start/end 是 1:KASAN_GRANULE_SIZE 缩放后的半开 metadata 区间。 */
	shadow_start = kasan_mem_to_shadow(addr);
	shadow_end = kasan_mem_to_shadow(addr + size);

	__memset(shadow_start, value, shadow_end - shadow_start);
}
EXPORT_SYMBOL_GPL(kasan_poison);

#ifdef CONFIG_KASAN_GENERIC
/*
 * 业务背景：Generic 模式用正 shadow 值编码末 granule 前 N 字节可访问。
 * 入参：addr 为借用对象起点；size 为实际字节数。
 * 出参/返回：无；size 非对齐时写末 shadow，对齐时不变。
 * 注意事项：调用者先开放/poison 其余 granule；KASAN 关闭时跳过。
 */
void kasan_poison_last_granule(const void *addr, size_t size)
{
	if (!kasan_enabled())
		return;

	if (size & KASAN_GRANULE_MASK) {
		u8 *shadow = (u8 *)kasan_mem_to_shadow(addr + size);
		*shadow = size & KASAN_GRANULE_MASK;
	}
}
#endif

/*
 * 业务背景：分配提交时按对象 tag 开放覆盖范围，并在 Generic 模式精确收紧末粒度。
 * 入参：addr 为可带 tag 的借用起点；size 为实际字节数；init 为模式初始化语义。
 * 出参/返回：无；更新覆盖对象的 shadow，不改变底层内存 ownership。
 * 注意事项：起点必须 granule 对齐；先保存 tag 再 reset 地址，尾粒度由 Generic 单独编码。
 */
void kasan_unpoison(const void *addr, size_t size, bool init)
{
	u8 tag = get_tag(addr);

	/*
	 * Perform shadow offset calculation based on untagged address, as
	 * some of the callers (e.g. kasan_unpoison_new_object) pass tagged
	 * addresses to this function.
	 */
	/* tag 值将作为软件 tag shadow 的开放值；地址换算必须先去 tag。 */
	addr = kasan_reset_tag(addr);

	if (WARN_ON((unsigned long)addr & KASAN_GRANULE_MASK))
		return;

	/* Unpoison all granules that cover the object. */
	/* 先向上取整开放完整覆盖，再由下一步恢复 Generic 尾部的精确边界。 */
	kasan_poison(addr, round_up(size, KASAN_GRANULE_SIZE), tag, false);

	/* Partially poison the last granule for the generic mode. */
	/* tag 模式无需正数部分编码；其粒度访问由 tag 比较控制。 */
	if (IS_ENABLED(CONFIG_KASAN_GENERIC))
		kasan_poison_last_granule(addr, size);
}

#ifdef CONFIG_MEMORY_HOTPLUG
/*
 * 业务背景：memory notifier 在复用离线内存前判断对应 shadow 页表是否已存在。
 * 入参：addr 为 shadow 虚拟地址；出参/返回：任一级缺失 false，叶或 PTE 存在 true。
 * 注意事项：只做无锁快照查询，不取得页表页引用；hotplug 锁稳定映射生命周期。
 */
static bool shadow_mapped(unsigned long addr)
{
	/* pgd..pte 是逐级借用指针；大页叶项在 PUD/PMD 即可确认已映射。 */
	pgd_t *pgd = pgd_offset_k(addr);
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if (pgd_none(*pgd))
		return false;
	/* 每一级只有父项 present 才能安全计算下级指针；空项立即结束。 */
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d))
		return false;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud))
		return false;
	/* PUD/PMD 叶映射无需继续走到 PTE；非叶才逐级检查下层存在性。 */
	if (pud_leaf(*pud))
		return true;
	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return false;
	if (pmd_leaf(*pmd))
		return true;
	pte = pte_offset_kernel(pmd, addr);
	return !pte_none(ptep_get(pte));
}

/*
 * 业务背景：物理内存上线前分配 shadow，取消上线或下线时释放运行期 vmalloc shadow。
 * 入参：nb 未使用；action 为 hotplug 阶段；data 借用 memory_notify。
 * 出参/返回：成功/无需动作 NOTIFY_OK，对齐或分配失败 NOTIFY_BAD。
 * 注意事项：启动期 shadow 无 vm_struct 因当前缺少释放实现而保留；运行期映射由 vfree 回收。
 */
static int __meminit kasan_mem_notifier(struct notifier_block *nb,
			unsigned long action, void *data)
{
	/* 原内存 PFN/页数换算成 shadow 页数、内核地址及半开虚拟区间。 */
	struct memory_notify *mem_data = data;
	unsigned long nr_shadow_pages, start_kaddr, shadow_start;
	unsigned long shadow_end, shadow_size;

	nr_shadow_pages = mem_data->nr_pages >> KASAN_SHADOW_SCALE_SHIFT;
	start_kaddr = (unsigned long)pfn_to_kaddr(mem_data->start_pfn);
	shadow_start = (unsigned long)kasan_mem_to_shadow((void *)start_kaddr);
	shadow_size = nr_shadow_pages << PAGE_SHIFT;
	shadow_end = shadow_start + shadow_size;

	/* notifier 只接受能完整换算成 shadow 页的 hotplug 粒度。 */
	if (WARN_ON(mem_data->nr_pages % KASAN_GRANULE_SIZE) ||
		WARN_ON(start_kaddr % KASAN_MEMORY_PER_SHADOW_PAGE))
		return NOTIFY_BAD;

	/* action 分开准备、回滚/下线；其他通知不改变 shadow。 */
	switch (action) {
	case MEM_GOING_ONLINE: {
		/* 上线前保证 shadow 可达；已有启动映射是离线内存再次上线的快速路径。 */
		void *ret;

		/*
		 * If shadow is mapped already than it must have been mapped
		 * during the boot. This could happen if we onlining previously
		 * offlined memory.
		 */
		/* 若已映射，必来自 boot 或前次生命周期，可直接复用。 */
		if (shadow_mapped(shadow_start))
			return NOTIFY_OK;

		/* 在精确 shadow 地址、对应 NUMA 节点分配无 guard 的页并发布页表。 */
		ret = __vmalloc_node_range(shadow_size, PAGE_SIZE, shadow_start,
					shadow_end, GFP_KERNEL,
					PAGE_KERNEL, VM_NO_GUARD,
					pfn_to_nid(mem_data->start_pfn),
					__builtin_return_address(0));
		if (!ret)
			return NOTIFY_BAD;

		/* shadow 仅为检测元数据，不应被 kmemleak 当成未引用普通分配。 */
		kmemleak_ignore(ret);
		return NOTIFY_OK;
	}
	case MEM_CANCEL_ONLINE:
	case MEM_OFFLINE: {
		/* 取消/下线都撤销本轮运行期 vmalloc shadow，启动期映射则无法释放。 */
		struct vm_struct *vm;

		/*
		 * shadow_start was either mapped during boot by kasan_init()
		 * or during memory online by __vmalloc_node_range().
		 * In the latter case we can use vfree() to free shadow.
		 * Non-NULL result of the find_vm_area() will tell us if
		 * that was the second case.
		 *
		 * Currently it's not possible to free shadow mapped
		 * during boot by kasan_init(). It's because the code
		 * to do that hasn't been written yet. So we'll just
		 * leak the memory.
		 */
		/*
		 * shadow_start 可能由启动 kasan_init() 建立，也可能由本通知器上线时 vmalloc。
		 * 后者能被 find_vm_area() 找到并用 vfree 配对；启动映射没有 vm_struct，当前尚无
		 * 拆除实现，只能保留这段内存。该有意泄漏只发生在启动映射覆盖的离线范围。
		 */
		/* find_vm_area 非 NULL 是运行期 __vmalloc_node_range 创建的 ownership 证据。 */
		vm = find_vm_area((void *)shadow_start);
		if (vm)
			vfree((void *)shadow_start);
	}
	}

	return NOTIFY_OK;
}

/*
 * 业务背景：核心初始化阶段注册 KASAN memory-hotplug notifier。
 * 入参：无；出参/返回：恒 0；副作用是把回调挂入 hotplug 通知链。
 * 注意事项：core_initcall 保证运行期热插拔前完成，回调优先级使用默认值。
 */
static int __init kasan_memhotplug_init(void)
{
	hotplug_memory_notifier(kasan_mem_notifier, DEFAULT_CALLBACK_PRI);

	return 0;
}

core_initcall(kasan_memhotplug_init);
#endif

#ifdef CONFIG_KASAN_VMALLOC

/*
 * 业务背景：允许体系结构在早期 vmalloc 区建立 shadow；通用弱实现为空。
 * 入参：start/size 描述借用 vmalloc 区；出参/返回：无，默认无副作用。
 * 注意事项：架构强定义可覆盖；调用者不能假设弱桩已映射 shadow。
 */
void __init __weak kasan_populate_early_vm_area_shadow(void *start,
						       unsigned long size)
{
}

/*
 * vmalloc_populate_data 是 apply_to_page_range 回调的批次上下文：start 对应 pages[0] 的
 * shadow 地址，pages 数组持有尚未成功装入 PTE 的页；PTE 发布后槽位置 NULL 转移 ownership。
 */
struct vmalloc_populate_data {
	unsigned long start;
	struct page **pages;
};

/*
 * 业务背景：为一个 shadow PTE 安装预分配页，处理与并发 populate 的“先到者获胜”。
 * 入参：ptep/addr 为借用目标项与地址；_data 为输入输出批次上下文。
 * 出参/返回：恒 0；成功发布时 pages[index]=NULL 表示页 ownership 转给 init_mm。
 * 注意事项：page_table_lock 关闭并发安装竞态；失败竞争者保留页供批次释放，lazy MMU 配对。
 */
static int kasan_populate_vmalloc_pte(pte_t *ptep, unsigned long addr,
				      void *_data)
{
	/* data/page/pte/index 描述当前目标及预初始化为 VMALLOC_INVALID 的物理页。 */
	struct vmalloc_populate_data *data = _data;
	struct page *page;
	pte_t pte;
	int index;

	/* 暂停批量页表模式，避免回调嵌套破坏调用者的 lazy MMU 状态。 */
	lazy_mmu_mode_pause();

	index = PFN_DOWN(addr - data->start);
	page = data->pages[index];
	__memset(page_to_virt(page), KASAN_VMALLOC_INVALID, PAGE_SIZE);
	pte = pfn_pte(page_to_pfn(page), PAGE_KERNEL);

	/* 与另一 CPU 建同一 PTE 竞争；锁内只有胜者 set，随后转移页 ownership。 */
	spin_lock(&init_mm.page_table_lock);
	if (likely(pte_none(ptep_get(ptep)))) {
		set_pte_at(&init_mm, addr, ptep, pte);
		data->pages[index] = NULL;
	}
	spin_unlock(&init_mm.page_table_lock);

	lazy_mmu_mode_resume();

	return 0;
}

/*
 * 业务背景：批次结束/失败时释放 pages 数组中仍未装入页表的页。
 * 入参：pages 为输入输出持有页数组；nr_pages 为槽数。
 * 出参/返回：无；非 NULL 页被释放并清槽，已转移的 NULL 槽跳过。
 * 注意事项：仅释放 order-0 页；调用者仍拥有数组本身。
 */
static void ___free_pages_bulk(struct page **pages, int nr_pages)
{
	int i;

	for (i = 0; i < nr_pages; i++) {
		if (pages[i]) {
			__free_pages(pages[i], 0);
			pages[i] = NULL;
		}
	}
}

/*
 * 业务背景：为一个 vmalloc shadow 批次反复调用 bulk allocator，直到数组完全填满。
 * 入参：pages 为输出数组；nr_pages 为页数；gfp_mask 为分配约束。
 * 出参/返回：成功 0；无法继续 -ENOMEM，并释放本函数此前取得的全部页。
 * 注意事项：成功后页 ownership 交给调用者；部分返回不是错误，会继续补齐。
 */
static int ___alloc_pages_bulk(struct page **pages, int nr_pages, gfp_t gfp_mask)
{
	unsigned long nr_populated, nr_total = nr_pages;
	struct page **page_array = pages;

	/* alloc_pages_bulk 可少量返回；指针和剩余数共同维持已填前缀不变量。 */
	while (nr_pages) {
		nr_populated = alloc_pages_bulk(gfp_mask, nr_pages, pages);
		if (!nr_populated) {
			___free_pages_bulk(page_array, nr_total - nr_pages);
			return -ENOMEM;
		}
		pages += nr_populated;
		nr_pages -= nr_populated;
	}

	/* 返回 0 时数组所有槽均持有一页，ownership 整体交给 populate 批次。 */
	return 0;
}

/*
 * 业务背景：分批为 [start,end) shadow 分配页并通过 apply_to_page_range 发布 PTE。
 * 入参：start/end 为页对齐半开区间；gfp_mask 控制数据页及页表分配上下文。
 * 出参/返回：成功 0；数组页或数据页分配/页表遍历失败返回 errno，未发布页全部回滚。
 * 注意事项：已发布 PTE 不在失败时撤销；memalloc scope 强制页表分配遵守外部 GFP。
 */
static int __kasan_populate_vmalloc_do(unsigned long start, unsigned long end, gfp_t gfp_mask)
{
	/* nr_total 是剩余页；data.pages 用一页作为批次数组；ret 汇总当前阶段错误。 */
	unsigned long nr_pages, nr_total = PFN_UP(end - start);
	struct vmalloc_populate_data data;
	unsigned int flags;
	int ret = 0;

	data.pages = (struct page **)__get_free_page(gfp_mask | __GFP_ZERO);
	if (!data.pages)
		return -ENOMEM;

	/* 每批不超过指针数组容量：预分配 → 页表发布 → 释放竞争落败页。 */
	while (nr_total) {
		nr_pages = min(nr_total, PAGE_SIZE / sizeof(data.pages[0]));
		ret = ___alloc_pages_bulk(data.pages, nr_pages, gfp_mask);
		if (ret)
			break;

		data.start = start;

		/*
		 * page tables allocations ignore external gfp mask, enforce it
		 * by the scope API
		 */
		/* scope 临时覆盖 current 分配约束，apply 返回后必须无条件恢复。 */
		flags = memalloc_apply_gfp_scope(gfp_mask);
		ret = apply_to_page_range(&init_mm, start, nr_pages * PAGE_SIZE,
					  kasan_populate_vmalloc_pte, &data);
		memalloc_restore_scope(flags);

		___free_pages_bulk(data.pages, nr_pages);
		if (ret)
			break;

		start += nr_pages * PAGE_SIZE;
		nr_total -= nr_pages;
	}

	/* 无论成功失败都释放仅承载指针的批次数组页，已发布数据页不在其中。 */
	free_page((unsigned long)data.pages);

	return ret;
}

/*
 * 业务背景：vmalloc/module 地址预留后为其 shadow 建物理页和页表，供稍后的 unpoison 使用。
 * 入参：addr/size 是应用虚拟半开范围；gfp_mask 为分配约束，均不转移应用区 ownership。
 * 出参/返回：非 vmalloc/module 或成功返回 0；分配/建表失败返回 errno。
 * 注意事项：UML 启动期已全映射只需重置毒值；成功后的 cache/发布屏障由本函数与调用者配对。
 */
int __kasan_populate_vmalloc(unsigned long addr, unsigned long size, gfp_t gfp_mask)
{
	/* shadow_start/end 是应用范围缩放并最终扩到完整 shadow 页的边界。 */
	unsigned long shadow_start, shadow_end;
	int ret;

	if (!is_vmalloc_or_module_addr((void *)addr))
		return 0;

	shadow_start = (unsigned long)kasan_mem_to_shadow((void *)addr);
	shadow_end = (unsigned long)kasan_mem_to_shadow((void *)addr + size);

	/*
	 * User Mode Linux maps enough shadow memory for all of virtual memory
	 * at boot, so doesn't need to allocate more on vmalloc, just clear it.
	 *
	 * The remaining CONFIG_UML checks in this file exist for the same
	 * reason.
	 */
	/* UML 无需分配页，只把既有 shadow 恢复为 VMALLOC_INVALID。 */
	if (IS_ENABLED(CONFIG_UML)) {
		__memset((void *)shadow_start, KASAN_VMALLOC_INVALID, shadow_end - shadow_start);
		return 0;
	}

	shadow_start = PAGE_ALIGN_DOWN(shadow_start);
	shadow_end = PAGE_ALIGN(shadow_end);

	/* 非 UML 建表成功后刷新 vmap cache，再依赖调用者发布屏障对其他 CPU 可见。 */
	ret = __kasan_populate_vmalloc_do(shadow_start, shadow_end, gfp_mask);
	if (ret)
		return ret;

	flush_cache_vmap(shadow_start, shadow_end);

	/*
	 * We need to be careful about inter-cpu effects here. Consider:
	 *
	 *   CPU#0				  CPU#1
	 * WRITE_ONCE(p, vmalloc(100));		while (x = READ_ONCE(p)) ;
	 *					p[99] = 1;
	 *
	 * With compiler instrumentation, that ends up looking like this:
	 *
	 *   CPU#0				  CPU#1
	 * // vmalloc() allocates memory
	 * // let a = area->addr
	 * // we reach kasan_populate_vmalloc
	 * // and call kasan_unpoison:
	 * STORE shadow(a), unpoison_val
	 * ...
	 * STORE shadow(a+99), unpoison_val	x = LOAD p
	 * // rest of vmalloc process		<data dependency>
	 * STORE p, a				LOAD shadow(x+99)
	 *
	 * If there is no barrier between the end of unpoisoning the shadow
	 * and the store of the result to p, the stores could be committed
	 * in a different order by CPU#0, and CPU#1 could erroneously observe
	 * poison in the shadow.
	 *
	 * We need some sort of barrier between the stores.
	 *
	 * In the vmalloc() case, this is provided by a smp_wmb() in
	 * clear_vm_uninitialized_flag(). In the per-cpu allocator and in
	 * get_vm_area() and friends, the caller gets shadow allocated but
	 * doesn't have any pages mapped into the virtual address space that
	 * has been reserved. Mapping those pages in will involve taking and
	 * releasing a page-table lock, which will provide the barrier.
	 */
	/*
	 * CPU0 必须在发布 vmalloc 指针前先让全部 shadow store 可见；否则 CPU1 通过已发布
	 * 指针访问尾字节时可能仍看见 poison。普通 vmalloc 由 clear_vm_uninitialized_flag()
	 * 的 smp_wmb 配对；仅预留地址的路径随后建应用页表，其页表锁解锁提供发布顺序。
	 */

	return 0;
}

/*
 * 业务背景：release 遍历 shadow 页表时清 PTE 并释放其 order-0 backing page。
 * 入参：ptep/addr 为借用目标；unused 无语义。
 * 出参/返回：恒 0；present PTE 被清并释放页，空项不变。
 * 注意事项：page_table_lock 与 populate 竞争；清项后 TLB flush 由外层按 flags 决定。
 */
static int kasan_depopulate_vmalloc_pte(pte_t *ptep, unsigned long addr,
					void *unused)
{
	pte_t pte;
	int none;

	lazy_mmu_mode_pause();

	/* 锁内原子取得旧项并摘除，锁外才释放物理页，缩短全局页表锁持有时间。 */
	spin_lock(&init_mm.page_table_lock);
	pte = ptep_get(ptep);
	none = pte_none(pte);
	if (likely(!none))
		pte_clear(&init_mm, addr, ptep);
	spin_unlock(&init_mm.page_table_lock);

	if (likely(!none))
		__free_page(pfn_to_page(pte_pfn(pte)));

	/* 恢复 lazy MMU 模式后回调完成；外层统一决定 TLB flush 时机。 */
	lazy_mmu_mode_resume();

	return 0;
}

/*
 * Release the backing for the vmalloc region [start, end), which
 * lies within the free region [free_region_start, free_region_end).
 *
 * This can be run lazily, long after the region was freed. It runs
 * under vmap_area_lock, so it's not safe to interact with the vmalloc/vmap
 * infrastructure.
 *
 * How does this work?
 * -------------------
 *
 * We have a region that is page aligned, labeled as A.
 * That might not map onto the shadow in a way that is page-aligned:
 *
 *                    start                     end
 *                    v                         v
 * |????????|????????|AAAAAAAA|AA....AA|AAAAAAAA|????????| < vmalloc
 *  -------- -------- --------          -------- --------
 *      |        |       |                 |        |
 *      |        |       |         /-------/        |
 *      \-------\|/------/         |/---------------/
 *              |||                ||
 *             |??AAAAAA|AAAAAAAA|AA??????|                < shadow
 *                 (1)      (2)      (3)
 *
 * First we align the start upwards and the end downwards, so that the
 * shadow of the region aligns with shadow page boundaries. In the
 * example, this gives us the shadow page (2). This is the shadow entirely
 * covered by this allocation.
 *
 * Then we have the tricky bits. We want to know if we can free the
 * partially covered shadow pages - (1) and (3) in the example. For this,
 * we are given the start and end of the free region that contains this
 * allocation. Extending our previous example, we could have:
 *
 *  free_region_start                                    free_region_end
 *  |                 start                     end      |
 *  v                 v                         v        v
 * |FFFFFFFF|FFFFFFFF|AAAAAAAA|AA....AA|AAAAAAAA|FFFFFFFF| < vmalloc
 *  -------- -------- --------          -------- --------
 *      |        |       |                 |        |
 *      |        |       |         /-------/        |
 *      \-------\|/------/         |/---------------/
 *              |||                ||
 *             |FFAAAAAA|AAAAAAAA|AAF?????|                < shadow
 *                 (1)      (2)      (3)
 *
 * Once again, we align the start of the free region up, and the end of
 * the free region down so that the shadow is page aligned. So we can free
 * page (1) - we know no allocation currently uses anything in that page,
 * because all of it is in the vmalloc free region. But we cannot free
 * page (3), because we can't be sure that the rest of it is unused.
 *
 * We only consider pages that contain part of the original region for
 * freeing: we don't try to free other pages from the free region or we'd
 * end up trying to free huge chunks of virtual address space.
 *
 * Concurrency
 * -----------
 *
 * How do we know that we're not freeing a page that is simultaneously
 * being used for a fresh allocation in kasan_populate_vmalloc(_pte)?
 *
 * We _can_ have kasan_release_vmalloc and kasan_populate_vmalloc running
 * at the same time. While we run under free_vmap_area_lock, the population
 * code does not.
 *
 * free_vmap_area_lock instead operates to ensure that the larger range
 * [free_region_start, free_region_end) is safe: because __alloc_vmap_area and
 * the per-cpu region-finding algorithm both run under free_vmap_area_lock,
 * no space identified as free will become used while we are running. This
 * means that so long as we are careful with alignment and only free shadow
 * pages entirely covered by the free region, we will not run in to any
 * trouble - any simultaneous allocations will be for disjoint regions.
 */
/*
 * 本算法只释放原 allocation 覆盖的 shadow 页：完整中间页必可释放；首尾部分页只有在
 * 更大的 free region 证明整页无人使用时才扩入。release 可与 populate 并发，但
 * free_vmap_area_lock 保证 free region 不会同时重新分配，因此对齐后的可释放 shadow
 * 与新分配范围不相交；启动原文图中的 (1)/(2) 可释放、(3) 因仍可能共享而保留。
 */
/*
 * 业务背景：vmalloc 延迟回收阶段释放已无应用映射依赖的 shadow backing。
 * 入参：start/end 是原分配区；free_region_start/end 是受锁保护的更大空闲区；flags 控制拆页/TLB。
 * 出参/返回：无；可安全覆盖的 shadow 页被清 PTE/释放，UML 仅重置 shadow。
 * 注意事项：运行于 vmap_area_lock 下，禁止调用 vmalloc 基础设施；对齐证明是并发安全边界。
 */
void __kasan_release_vmalloc(unsigned long start, unsigned long end,
			   unsigned long free_region_start,
			   unsigned long free_region_end,
			   unsigned long flags)
{
	/* region_* 先收缩到原区完整 shadow 页，再借 free_region 判定能否纳入首尾页。 */
	void *shadow_start, *shadow_end;
	unsigned long region_start, region_end;
	unsigned long size;

	region_start = ALIGN(start, KASAN_MEMORY_PER_SHADOW_PAGE);
	region_end = ALIGN_DOWN(end, KASAN_MEMORY_PER_SHADOW_PAGE);

	free_region_start = ALIGN(free_region_start, KASAN_MEMORY_PER_SHADOW_PAGE);

	/* 左/右部分 shadow 页只有完全落在空闲区时才扩展释放范围。 */
	if (start != region_start &&
	    free_region_start < region_start)
		region_start -= KASAN_MEMORY_PER_SHADOW_PAGE;

	free_region_end = ALIGN_DOWN(free_region_end, KASAN_MEMORY_PER_SHADOW_PAGE);

	if (end != region_end &&
	    free_region_end > region_end)
		region_end += KASAN_MEMORY_PER_SHADOW_PAGE;

	shadow_start = kasan_mem_to_shadow((void *)region_start);
	shadow_end = kasan_mem_to_shadow((void *)region_end);

	/* 空范围无需动作；有效范围按 UML 或真实页表路径处理。 */
	if (shadow_end > shadow_start) {
		size = shadow_end - shadow_start;
		if (IS_ENABLED(CONFIG_UML)) {
			__memset(shadow_start, KASAN_SHADOW_INIT, shadow_end - shadow_start);
			return;
		}


		/* 先摘除并释放 backing，再按请求 flush TLB，禁止硬件继续使用旧 PTE。 */
		if (flags & KASAN_VMALLOC_PAGE_RANGE)
			apply_to_existing_page_range(&init_mm,
					     (unsigned long)shadow_start,
					     size, kasan_depopulate_vmalloc_pte,
					     NULL);

		if (flags & KASAN_VMALLOC_TLB_FLUSH)
			flush_tlb_kernel_range((unsigned long)shadow_start,
					       (unsigned long)shadow_end);
	}
}

/*
 * 业务背景：vmalloc 映射投入使用或扩容时开放其 shadow，并为软件 tag 模式返回带 tag 地址。
 * 入参：start 为借用地址；size 为字节数；flags 描述保护、来源、初始化与 tag 保留策略。
 * 出参/返回：非 vmalloc 原样返回；否则返回可带 tag 的同一地址并更新 shadow。
 * 注意事项：软件模式忽略 VM_ALLOC/INIT；可执行内存不得带 tag，否则 PC 不被内核接受。
 */
void *__kasan_unpoison_vmalloc(const void *start, unsigned long size,
			       kasan_vmalloc_flags_t flags)
{
	/*
	 * Software KASAN modes unpoison both VM_ALLOC and non-VM_ALLOC
	 * mappings, so the KASAN_VMALLOC_VM_ALLOC flag is ignored.
	 * Software KASAN modes can't optimize zeroing memory by combining it
	 * with setting memory tags, so the KASAN_VMALLOC_INIT flag is ignored.
	 */
	/* 软件模式无法把清零和 tag 初始化合并，因此两个优化 flags 在这里不改变行为。 */

	if (!is_vmalloc_or_module_addr(start))
		return (void *)start;

	/*
	 * Don't tag executable memory with the tag-based mode.
	 * The kernel doesn't tolerate having the PC register tagged.
	 */
	/* SW_TAGS 对非执行且未 KEEP_TAG 的新对象生成 tag；执行地址始终保留内核 tag。 */
	if (IS_ENABLED(CONFIG_KASAN_SW_TAGS) &&
	    !(flags & KASAN_VMALLOC_PROT_NORMAL))
		return (void *)start;

	if (unlikely(!(flags & KASAN_VMALLOC_KEEP_TAG)))
		start = set_tag(start, kasan_random_tag());

	kasan_unpoison(start, size, false);
	return (void *)start;
}

/*
 * Poison the shadow for a vmalloc region. Called as part of the
 * freeing process at the time the region is freed.
 */
/* vmalloc 区逻辑释放时立即把其 shadow 标回 INVALID，阻止延迟拆页前的释放后访问。 */
/*
 * 业务背景：vmalloc 地址从调用者可见状态退出时先封闭其 KASAN 可访问范围。
 * 入参：start 为借用区起点；size 为字节数；出参/返回：无，目标 shadow 被 poison。
 * 注意事项：非 vmalloc/module 静默跳过；向上按 granule 取整，backing page 稍后才释放。
 */
void __kasan_poison_vmalloc(const void *start, unsigned long size)
{
	if (!is_vmalloc_or_module_addr(start))
		return;

	size = round_up(size, KASAN_GRANULE_SIZE);
	kasan_poison(start, size, KASAN_VMALLOC_INVALID, false);
}

#else /* CONFIG_KASAN_VMALLOC */

/*
 * 业务背景：未启用通用 vmalloc shadow 时，模块装载单独为其固定 shadow 地址分配 backing。
 * 入参：addr/size 描述借用模块区；gfp_mask 用于延迟 kmemleak 登记语义。
 * 出参/返回：成功 0；shadow 起点未页对齐 -EINVAL；分配失败 -ENOMEM。
 * 注意事项：成功设置 vm->VM_KASAN 转移 shadow 回收责任；UML 已全映射仅清毒值。
 */
int kasan_alloc_module_shadow(void *addr, size_t size, gfp_t gfp_mask)
{
	/* scaled_size 是实际 shadow 字节，shadow_size 扩到 backing page 完整页。 */
	void *ret;
	size_t scaled_size;
	size_t shadow_size;
	unsigned long shadow_start;

	shadow_start = (unsigned long)kasan_mem_to_shadow(addr);
	scaled_size = (size + KASAN_GRANULE_SIZE - 1) >>
				KASAN_SHADOW_SCALE_SHIFT;
	shadow_size = round_up(scaled_size, PAGE_SIZE);

	/* 固定地址 vmalloc 要求 shadow 起点页对齐，否则无法独立管理 backing。 */
	if (WARN_ON(!PAGE_ALIGNED(shadow_start)))
		return -EINVAL;

	if (IS_ENABLED(CONFIG_UML)) {
		/* UML 的全量启动映射只需把模块区恢复为未开放毒值。 */
		__memset((void *)shadow_start, KASAN_SHADOW_INIT, shadow_size);
		return 0;
	}

	/* 在唯一固定范围建 shadow；返回非 NULL 后用 INVALID 初始化再发布 VM_KASAN。 */
	ret = __vmalloc_node_range(shadow_size, 1, shadow_start,
			shadow_start + shadow_size,
			GFP_KERNEL,
			PAGE_KERNEL, VM_NO_GUARD, NUMA_NO_NODE,
			__builtin_return_address(0));

	if (ret) {
		/* vm 是模块主映射描述符；VM_KASAN 让卸载路径知道需配对 vfree。 */
		struct vm_struct *vm = find_vm_area(addr);
		__memset(ret, KASAN_SHADOW_INIT, shadow_size);
		vm->flags |= VM_KASAN;
		kmemleak_ignore(ret);

		if (vm->flags & VM_DEFER_KMEMLEAK)
			kmemleak_vmalloc(vm, size, gfp_mask);

		return 0;
	}

	return -ENOMEM;
}

/*
 * 业务背景：模块 vm_struct 最终释放时回收由 alloc_module_shadow 单独建立的 shadow。
 * 入参：vm 为借用、仍有效的模块映射描述符。
 * 出参/返回：无；带 VM_KASAN 时释放对应 shadow，UML 无动作。
 * 注意事项：调用者保证模块不再执行/访问且仅调用一次；vfree 处理页表与 backing 生命周期。
 */
void kasan_free_module_shadow(const struct vm_struct *vm)
{
	if (IS_ENABLED(CONFIG_UML))
		return;

	if (vm->flags & VM_KASAN)
		vfree(kasan_mem_to_shadow(vm->addr));
}

#endif
