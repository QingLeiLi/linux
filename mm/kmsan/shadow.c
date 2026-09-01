// SPDX-License-Identifier: GPL-2.0
/*
 * KMSAN shadow implementation.
 *
 * Copyright (C) 2017-2022 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */
/*
 * 本文件把被跟踪内核地址映射到两类 KMSAN metadata：shadow 每个数据字节记录是否
 * 已初始化，origin 每 KMSAN_ORIGIN_SIZE 字节保存污染来源的 stack-depot handle。
 * 直接映射页通过 struct page 中的指针逐页关联，vmalloc/module 区则用预留虚拟区的
 * 固定偏移建立连续别名；无法跟踪的插桩访存退化到互相隔离的 dummy load/store 页。
 */

#include <asm/kmsan.h>
#include <asm/tlbflush.h>
#include <linux/cacheflush.h>
#include <linux/memblock.h>
#include <linux/mm_types.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/stddef.h>
/* 上述头提供地址转换、TLB/cache 刷新、memblock、页描述符与临时数组分配接口。 */

#include "../internal.h"
#include "kmsan.h"

/* 两个左值宏把数据页的 metadata 关联字段用于读取或发布对应 backing page。 */
#define shadow_page_for(page) ((page)->kmsan_shadow)

#define origin_page_for(page) ((page)->kmsan_origin)

/*
 * shadow_ptr_for() - 取得数据页所关联 shadow 页的直接映射地址
 * 业务背景：metadata 读写最终需要从 struct page 关联转成 CPU 指针。
 * 入参：page 是已通过 page_has_metadata() 或分配协议保证有关联 shadow 的借用数据页。
 * 出参/返回：返回 shadow backing 的借用直接映射指针，不增页引用、不转移 ownership。
 * 注意事项：调用者必须保证关联在访问期间有效；高端页不适用于此 KMSAN backing 协议。
 */
static void *shadow_ptr_for(struct page *page)
{
	return page_address(shadow_page_for(page));
}

/*
 * origin_ptr_for() - 取得数据页所关联 origin 页的直接映射地址
 * 业务背景：与 shadow_ptr_for() 成对，为来源 handle 的读写提供 CPU 地址。
 * 入参：page 是具有有效 kmsan_origin 关联的借用数据页。
 * 出参/返回：返回借用 origin 地址，无引用或所有权变化。
 * 注意事项：有效期由数据页/metadata 页分配协议保证，本函数不检查 NULL 且不睡眠。
 */
static void *origin_ptr_for(struct page *page)
{
	return page_address(origin_page_for(page));
}

/*
 * page_has_metadata() - 检查数据页是否同时绑定 shadow 与 origin
 * 业务背景：KMSAN 只能在两类 metadata 都存在时安全传播状态，不能接受半绑定页。
 * 入参：page 是有效的借用 struct page。
 * 出参/返回：两个关联字段均非 NULL 返回 true，否则 false；无副作用和 ownership 变化。
 * 注意事项：这里只读取发布后的字段，不稳定并发重绑；调用者依赖页分配生命周期防止变化。
 */
static bool page_has_metadata(struct page *page)
{
	return shadow_page_for(page) && origin_page_for(page);
}

/*
 * set_no_shadow_origin_page() - 标记一个 metadata backing 页自身不再需要 metadata
 * 业务背景：shadow/origin 页若再递归配 metadata 会无限膨胀；启动和 buddy 配对阶段
 * 因而显式清空它们的关联字段。
 * 入参：page 是初始化路径独占的输入输出 metadata 页描述符。
 * 出参/返回：无直接返回；把 kmsan_shadow/kmsan_origin 同时置 NULL，不释放任何页。
 * 注意事项：必须在该页发布给 KMSAN 前调用；并发读者存在时清空会破坏关联不变量。
 */
static void set_no_shadow_origin_page(struct page *page)
{
	shadow_page_for(page) = NULL;
	origin_page_for(page) = NULL;
}

/*
 * Dummy load and store pages to be used when the real metadata is unavailable.
 * There are separate pages for loads and stores, so that every load returns a
 * zero, and every store doesn't affect other loads.
 */
/*
 * 当真实 metadata 不可用时，load 统一读独立的全零页，避免误报；store 写另一页并被
 * 丢弃，不能污染后续 load。两个 PAGE_SIZE 对齐的静态页全局常驻，最大单次访问由
 * kmsan_get_shadow_origin_ptr() 限制为一页，故 dummy 重定向不会越界。
 */
static char dummy_load_page[PAGE_SIZE] __aligned(PAGE_SIZE);
static char dummy_store_page[PAGE_SIZE] __aligned(PAGE_SIZE);

/*
 * vmalloc_meta() - 计算 vmalloc/module 地址在固定 metadata 虚拟区中的同偏移地址
 * 业务背景：这些映射的物理页可离散，但插桩需要连续 shadow/origin VA；vmalloc 建图时
 * 已把每个数据页的 metadata 页映射到预留区域，本函数只做确定性地址换算。
 * 入参：addr 是借用内核虚拟地址；is_origin 选择 origin 区，false 选择 shadow 区。
 * 出参/返回：地址属于 vmalloc 或 module 区时返回相应 metadata VA，否则返回 0；无引用。
 * 注意事项：origin 地址必须按 KMSAN_ORIGIN_SIZE 对齐；区间测试使用非插桩 helper，
 * 可在 KMSAN runtime 中调用且不睡眠。返回非零不单独证明页表已建立。
 */
static unsigned long vmalloc_meta(void *addr, bool is_origin)
{
	/* addr64 用于边界算术，off 始终相对所属数据虚拟区起点。 */
	unsigned long addr64 = (unsigned long)addr, off;

	/* origin handle 覆盖固定粒度，未对齐地址会让来源单元选择不确定。 */
	KMSAN_WARN_ON(is_origin && !IS_ALIGNED(addr64, KMSAN_ORIGIN_SIZE));
	if (kmsan_internal_is_vmalloc_addr(addr)) {
		/* 保留同一 offset，把数据区一一映射到 shadow 或 origin 预留区。 */
		off = addr64 - VMALLOC_START;
		return off + (is_origin ? KMSAN_VMALLOC_ORIGIN_START :
					  KMSAN_VMALLOC_SHADOW_START);
	}
	if (kmsan_internal_is_module_addr(addr)) {
		/* module 有独立的 metadata 预留基址，不能复用 VMALLOC_START 的偏移。 */
		off = addr64 - MODULES_VADDR;
		return off + (is_origin ? KMSAN_MODULES_ORIGIN_START :
					  KMSAN_MODULES_SHADOW_START);
	}
	return 0;
}

/*
 * virt_to_page_or_null() - 安全尝试把直接映射虚拟地址转换为 struct page
 * 业务背景：kmsan_get_metadata() 在架构/vmalloc 特例之后用它识别普通内核直接映射。
 * 入参：vaddr 是任意借用指针，不保证可由 virt_to_page() 转换。
 * 出参/返回：kmsan_virt_addr_valid() 认可时返回借用 page，否则 NULL；不增加引用。
 * 注意事项：数值有效性不固定页生命周期，调用者须处在原内存对象有效期内；不睡眠。
 */
static struct page *virt_to_page_or_null(void *vaddr)
{
	if (kmsan_virt_addr_valid(vaddr))
		return virt_to_page(vaddr);
	else
		return NULL;
}

/*
 * kmsan_get_shadow_origin_ptr() - 为一次插桩访存取得成对 metadata 指针
 *
 * 业务背景：instrumentation hook 在执行原 load/store 前调用这里；真实 metadata 缺失
 * 时必须继续安全执行，而不能因 sanitizer 自身访问 NULL 或越界崩溃。
 * 入参：address 是待访问内核地址的借用指针；size 是访问字节数且至多 PAGE_SIZE；
 * store 为 true 表示写 metadata、false 表示读 metadata。
 * 出参/返回：可跟踪时返回真实 shadow/origin 借用指针；禁用或缺失时，store 返回 dummy
 * store 页以丢弃写入，load 返回全零 dummy load 页以视作已初始化。无资源 ownership 变化。
 * 注意事项：真实范围必须具有连续 metadata；警告只诊断契约违规，不替代调用者分块。
 * dummy 两指针指向同页是有意设计，返回指针均只在本次插桩操作期间使用。
 */
struct shadow_origin_ptr kmsan_get_shadow_origin_ptr(void *address, u64 size,
						     bool store)
{
	/* ret 按值返回一对借用地址；shadow 暂存真实 shadow 查询结果。 */
	struct shadow_origin_ptr ret;
	void *shadow;

	/*
	 * Even if we redirect this memory access to the dummy page, it will
	 * go out of bounds.
	 */
	/* 即使重定向到单页 dummy，超过 PAGE_SIZE 的访问仍会越界，故必须诊断。 */
	KMSAN_WARN_ON(size > PAGE_SIZE);

	/* runtime 尚未发布时所有 metadata 访问都走无副作用的 dummy 退化路径。 */
	if (!kmsan_enabled)
		goto return_dummy;

	/* 连续性保证 instrumentation 可用起始指针线性处理整个 size。 */
	KMSAN_WARN_ON(!kmsan_metadata_is_contiguous(address, size));
	shadow = kmsan_get_metadata(address, KMSAN_META_SHADOW);
	if (!shadow)
		goto return_dummy;

	/* shadow 存在时按同一规范化地址取得 origin；正常关联协议保证二者成对。 */
	ret.shadow = shadow;
	ret.origin = kmsan_get_metadata(address, KMSAN_META_ORIGIN);
	return ret;

return_dummy:
	/* 到达时没有可安全访问的真实 metadata；按访存方向选择隔离 backing。 */
	if (store) {
		/* Ignore this store. */
		/* store 写入独立垃圾页，返回后不会改变任一真实地址的初始化状态。 */
		ret.shadow = dummy_store_page;
		ret.origin = dummy_store_page;
	} else {
		/* This load will return zero. */
		/* 静态 load 页初始为零，读取 shadow/origin 都得到“无污染来源”。 */
		ret.shadow = dummy_load_page;
		ret.origin = dummy_load_page;
	}
	return ret;
}

/*
 * Obtain the shadow or origin pointer for the given address, or NULL if there's
 * none. The caller must check the return value for being non-NULL if needed.
 * The return value of this function should not depend on whether we're in the
 * runtime or not.
 */
/*
 * 给定任意内核地址，返回对应 shadow 或 origin 指针；无 metadata 时返回 NULL，调用者
 * 必须按需检查。该纯地址映射不依赖“当前是否进入 KMSAN runtime”，否则 sanitizer
 * 内外对同一地址会得到不同映射并破坏递归防护。
 */
/*
 * kmsan_get_metadata() - 在三类地址模型中解析单个 metadata 地址
 * 业务背景：KMSAN core、hooks 和插桩包装都经此统一入口查询 vmalloc/module、架构特例
 * 或普通直接映射页的 shadow/origin。
 * 入参：address 是借用内核地址；is_origin 为 true 选择 origin 并先按来源粒度向下对齐，
 * false 选择逐字节 shadow。
 * 出参/返回：返回可直接访问的借用 metadata 指针，地址不可跟踪或页未绑定时返回 NULL；
 * 不增加页引用、不分配内存、不改变 metadata。
 * 注意事项：调用者负责保证访问范围连续和 backing 生命周期；vmalloc 返回固定别名，架构
 * hook 可优先处理特殊地址，普通页必须同时具有 shadow/origin。函数不看 kmsan_enabled。
 */
void *kmsan_get_metadata(void *address, bool is_origin)
{
	/* addr/off 以字节计；page 是直接映射页借用描述符；ret 承接架构特例。 */
	u64 addr = (u64)address, off;
	struct page *page;
	void *ret;

	/* 一个 origin handle 覆盖固定字节组，组内地址必须落到同一来源单元。 */
	if (is_origin)
		addr = ALIGN_DOWN(addr, KMSAN_ORIGIN_SIZE);
	address = (void *)addr;
	/* vmalloc/module 直接按固定 VA 偏移解析，不需要先找到离散数据页。 */
	if (kmsan_internal_is_vmalloc_addr(address) ||
	    kmsan_internal_is_module_addr(address))
		return (void *)vmalloc_meta(address, is_origin);

	/* 架构可为 per-CPU 等特殊区提供优先映射；非 NULL 即为最终借用地址。 */
	ret = arch_kmsan_get_meta_or_null(address, is_origin);
	if (ret)
		return ret;

	/* 通用路径只接受有效直接映射且成对绑定 metadata 的数据页。 */
	page = virt_to_page_or_null(address);
	if (!page)
		return NULL;
	if (!page_has_metadata(page))
		return NULL;
	/* 保留页内字节偏移，在对应 metadata backing 页中定位同一数据位置。 */
	off = offset_in_page(addr);

	return (is_origin ? origin_ptr_for(page) : shadow_ptr_for(page)) + off;
}

/*
 * kmsan_copy_page_meta() - 把一个数据页的 shadow/origin 状态复制给另一个数据页
 *
 * 业务背景：页内容复制或迁移后，KMSAN 状态也必须同步；否则目标数据与“是否初始化、
 * 来自哪里”的诊断信息会错配。
 * 入参：dst 是输入输出目标 page，可为 NULL；src 是只读来源 page，可为 NULL。二者均为
 * 借用指针，本函数不取得页引用，固定复制 PAGE_SIZE metadata。
 * 出参/返回：无直接返回；目标无 metadata、KMSAN 禁用或已在 runtime 中时无副作用；
 * 来源无 metadata 时把目标数据视为已初始化，否则覆盖目标 shadow 和 origin 整页。
 * 注意事项：调用者必须稳定两页及其 backing，并协调与同页 metadata 写者；runtime guard
 * 阻止 __memcpy 自身插桩递归。函数不报告分配失败且不转移 ownership。
 */
void kmsan_copy_page_meta(struct page *dst, struct page *src)
{
	/* 禁用时无状态可维护；runtime 内再次进入会造成 sanitizer 递归。 */
	if (!kmsan_enabled || kmsan_in_runtime())
		return;
	/* 目标不存在或没有完整 backing 时无处写入，安全忽略。 */
	if (!dst || !page_has_metadata(dst))
		return;
	if (!src || !page_has_metadata(src)) {
		/* 未跟踪来源按“已初始化”传播，避免把缺失 metadata 误报成污染。 */
		kmsan_internal_unpoison_memory(page_address(dst), PAGE_SIZE,
					       /*checked*/ false);
		return;
	}

	/*
	 * 两页都可跟踪：在 runtime guard 内使用未插桩 memcpy 同步逐字节 shadow 和
	 * origin handle 数组，完成后目标 metadata 与数据复制路径保持一一对应。
	 */
	kmsan_enter_runtime();
	__memcpy(shadow_ptr_for(dst), shadow_ptr_for(src), PAGE_SIZE);
	__memcpy(origin_ptr_for(dst), origin_ptr_for(src), PAGE_SIZE);
	kmsan_leave_runtime();
}
EXPORT_SYMBOL(kmsan_copy_page_meta);

/*
 * kmsan_alloc_page() - 在 buddy 分配成功后初始化整块页的 KMSAN 状态
 *
 * 业务背景：page allocator 在把 order 阶页交给调用者前通知 KMSAN；零填充分配应标为
 * 已初始化，普通分配应标为全污染并记录本次分配栈作为 origin。
 * 入参：page 是分配块首个借用 page，可为 NULL；order 决定连续 2^order 页；flags 是
 * 原 alloc_pages GFP 标志，__GFP_ZERO 改变初始化语义并传给 stack 保存策略。
 * 出参/返回：无直接返回；清零 shadow/origin 表示初始化，普通路径把 shadow 填 0xff、
 * origin 填同一 stack handle。页和 metadata ownership 仍归分配器/调用者协议。
 * 注意事项：调用前这些数据页必须已绑定同阶连续 metadata。KMSAN runtime 内的分配不
 * 再毒化以防递归；函数用未插桩 memset，保存栈阶段显式进入 runtime，可因 depot 压力退化。
 */
void kmsan_alloc_page(struct page *page, unsigned int order, gfp_t flags)
{
	/* initialized 是整个块的发布状态；pages 为基础页数，handle 是共享分配来源。 */
	bool initialized = (flags & __GFP_ZERO) || !kmsan_enabled;
	struct page *shadow, *origin;
	depot_stack_handle_t handle;
	int pages = 1 << order;

	/* allocator 的失败通知可传 NULL，此时没有数据块和 metadata 需要处理。 */
	if (!page)
		return;

	/* 关联由启动或 buddy 三块配对协议预先建立；块首指向连续 backing。 */
	shadow = shadow_page_for(page);
	origin = origin_page_for(page);

	if (initialized) {
		/* 数据已清零或 KMSAN 尚未启用：shadow/origin 同时归零，消除旧页状态。 */
		__memset(page_address(shadow), 0, PAGE_SIZE * pages);
		__memset(page_address(origin), 0, PAGE_SIZE * pages);
		return;
	}

	/* Zero pages allocated by the runtime should also be initialized. */
	/* runtime 自身申请的页由内部用途管理，保持其既有零/已初始化 metadata，避免递归污染。 */
	if (kmsan_in_runtime())
		return;

	/* 普通分配先把每个 shadow 字节置为 poisoned，再为全块生成一次共同来源。 */
	__memset(page_address(shadow), -1, PAGE_SIZE * pages);
	kmsan_enter_runtime();
	handle = kmsan_save_stack_with_flags(flags, /*extra_bits*/ 0);
	kmsan_leave_runtime();
	/*
	 * Addresses are page-aligned, pages are contiguous, so it's ok
	 * to just fill the origin pages with @handle.
	 */
	/*
	 * 数据、shadow、origin 块均页对齐且按 order 连续，所以可把 origin 当连续 handle
	 * 数组填充；每个 KMSAN_ORIGIN_SIZE 单元都指回同一本次分配栈。
	 */
	for (int i = 0; i < PAGE_SIZE * pages / sizeof(handle); i++)
		((depot_stack_handle_t *)page_address(origin))[i] = handle;
}

/*
 * kmsan_free_page() - 在页块归还 buddy 前把原数据范围标记为已释放污染
 *
 * 业务背景：free 后残留指针再次读取应报告 use-after-free，而不是继承释放前的初始化状态；
 * page allocator 的 free 路径在真正复用页之前调用本函数。
 * 入参：page 是待释放 order 阶块首个借用 page；order 指定 PAGE_SIZE<<order 字节范围。
 * 出参/返回：无直接返回；启用且不在 runtime 时毒化数据对应 metadata，并以
 * KMSAN_POISON_FREE/POISON_CHECK 记录释放来源；不释放页或 metadata backing。
 * 注意事项：进入 runtime 防止 poison 过程递归；GFP 掩码去掉 reclaim，避免释放路径为
 * 保存 origin 再进入内存回收。调用者负责 page 非 NULL、关联有效和排除并发使用。
 */
void kmsan_free_page(struct page *page, unsigned int order)
{
	/* 禁用或 runtime 内部释放不维护普通对象毒化，防止 sanitizer 自举递归。 */
	if (!kmsan_enabled || kmsan_in_runtime())
		return;
	/* poison helper 更新整块 shadow/origin；页的真正释放仍由 page allocator 完成。 */
	kmsan_enter_runtime();
	kmsan_internal_poison_memory(page_address(page), PAGE_SIZE << order,
				     GFP_KERNEL & ~(__GFP_RECLAIM),
				     KMSAN_POISON_CHECK | KMSAN_POISON_FREE);
	kmsan_leave_runtime();
}

/*
 * kmsan_vmap_pages_range_noflush() - 为一段新 vmap 同步建立连续 shadow/origin 映射
 *
 * 业务背景：vmalloc.c 在发布真实数据页映射前调用这里；离散 pages 的 metadata backing
 * 必须按相同次序映射到固定 KMSAN_VMALLOC/MODULE metadata 区，供地址偏移查询。
 * 入参：start/end 是待映射数据 VA 左闭右开区间；prot 是数据映射权限但 metadata 强制
 * PAGE_KERNEL；pages 是 nr 个借用数据页指针数组；page_shift 描述 vmap 大页粒度；
 * gfp_mask 仅供两份临时指针数组分配。所有输入 ownership 保持不变。
 * 出参/返回：KMSAN 禁用或地址不属于可映射 metadata 区返回 0；成功建立并刷新两区返回 0；
 * 临时数组失败返回 -ENOMEM，底层 vmap 的非零错误原样返回。函数释放自有数组。
 * 注意事项：shadow 成功、origin 失败时本函数不撤销 shadow 前缀；掌握完整 vmap 范围的
 * 上层失败清理负责统一 unmap。只有两区都成功后才做 TLB/cache 刷新，函数可睡眠。
 */
int kmsan_vmap_pages_range_noflush(unsigned long start, unsigned long end,
				   pgprot_t prot, struct page **pages,
				   unsigned int page_shift, gfp_t gfp_mask)
{
	/* 四个地址界定两段 metadata VA；数组按页借用 backing；mapped/err 传递底层状态。 */
	unsigned long shadow_start, origin_start, shadow_end, origin_end;
	struct page **s_pages, **o_pages;
	int nr, mapped, err = 0;

	/* 未启用时真实 vmap 无需附加 metadata 映射。 */
	if (!kmsan_enabled)
		return 0;

	shadow_start = vmalloc_meta((void *)start, KMSAN_META_SHADOW);
	shadow_end = vmalloc_meta((void *)end, KMSAN_META_SHADOW);
	/* 0 表示 start 不在 KMSAN 支持的 vmalloc/module 数据区，按不可跟踪成功退化。 */
	if (!shadow_start)
		return 0;

	/* 每个基础数据页对应一个 shadow 与 origin page 指针槽。 */
	nr = (end - start) / PAGE_SIZE;
	s_pages = kzalloc_objs(*s_pages, nr, gfp_mask);
	o_pages = kzalloc_objs(*o_pages, nr, gfp_mask);
	if (!s_pages || !o_pages) {
		/* 任一数组失败都走共同出口，kfree(NULL) 安全，尚未建立任何映射。 */
		err = -ENOMEM;
		goto ret;
	}
	/* 从数据页关联字段构造两个与 pages 顺序完全一致的临时 backing 数组。 */
	for (int i = 0; i < nr; i++) {
		s_pages[i] = shadow_page_for(pages[i]);
		o_pages[i] = origin_page_for(pages[i]);
	}
	/* metadata 只供内核 runtime 访问，不继承调用者的数据页 prot。 */
	prot = PAGE_KERNEL;

	/* start/end 属于同一数据区，按相同偏移得到 origin 半开区间。 */
	origin_start = vmalloc_meta((void *)start, KMSAN_META_ORIGIN);
	origin_end = vmalloc_meta((void *)end, KMSAN_META_ORIGIN);
	/* 页表构造会触及内存，runtime guard 阻止其插桩再次查询尚未完成的 metadata。 */
	kmsan_enter_runtime();
	mapped = __vmap_pages_range_noflush(shadow_start, shadow_end, prot,
					    s_pages, page_shift);
	kmsan_leave_runtime();
	if (mapped) {
		/* 底层可能只映射成功前缀；把错误交给上层完整 vmap 失败回滚。 */
		err = mapped;
		goto ret;
	}
	/* shadow 全部成功后再建立 origin；二者在最终 flush 前均尚未作为完整对发布。 */
	kmsan_enter_runtime();
	mapped = __vmap_pages_range_noflush(origin_start, origin_end, prot,
					    o_pages, page_shift);
	kmsan_leave_runtime();
	if (mapped) {
		/* shadow 已存在而 origin 失败，仍只记录错误；上层按整个虚拟区间撤销。 */
		err = mapped;
		goto ret;
	}
	/*
	 * 两组页表都完整后先刷新内核 TLB，再刷新 vmap cache，使 CPU 后续通过固定
	 * metadata VA 观察到新映射；数据区本身由 vmalloc.c 随后建立并提交。
	 */
	flush_tlb_kernel_range(shadow_start, shadow_end);
	flush_tlb_kernel_range(origin_start, origin_end);
	flush_cache_vmap(shadow_start, shadow_end);
	flush_cache_vmap(origin_start, origin_end);

ret:
	/* 临时数组只持借用 page 指针，释放数组本身不会 put metadata 页。 */
	kfree(s_pages);
	kfree(o_pages);
	return err;
}

/* Allocate metadata for pages allocated at boot time. */
/* 启动早期 buddy 尚不可用，使用 memblock 为既有数据区一次性保留等大的 shadow/origin。 */
/*
 * kmsan_init_alloc_meta_for_range() - 为启动期直接映射区间分配并绑定 metadata
 *
 * 业务背景：KMSAN runtime 启用前，内核镜像/pgdat 等 memblock 区已经存在，不能经正常
 * buddy 三块分配获得 metadata；init.c 因而逐个保留区间调用本函数补齐关联。
 * 入参：start/end 是借用的左闭右开内核地址；函数把 start 向下、总覆盖长度向上页对齐。
 * 出参/返回：无直接返回；为整个覆盖区分别 memblock 分配等大 shadow/origin backing，
 * 逐页写入数据页关联字段，并把 metadata 页自身标为不跟踪。backing 永久归 KMSAN。
 * 注意事项：仅 __init 阶段、串行且可睡眠前的 memblock 环境调用；分配失败直接 panic。
 * 输入区间内每页必须有有效 struct page，调用者负责不传空/反向范围。
 */
void __init kmsan_init_alloc_meta_for_range(void *start, void *end)
{
	/* *_p 是当前页描述符；shadow/origin 是两块 memblock 基址；size 为页对齐字节数。 */
	struct page *shadow_p, *origin_p;
	void *shadow, *origin;
	struct page *page;
	u64 size;

	/* 向下覆盖首个部分页，再按调整后的 start 向上覆盖 end 所在尾页。 */
	start = (void *)PAGE_ALIGN_DOWN((u64)start);
	size = PAGE_ALIGN((u64)end - (u64)start);
	/* 两份 metadata 与数据覆盖等大且页对齐，失败不能安全启动 KMSAN，故 panic。 */
	shadow = memblock_alloc_or_panic(size, PAGE_SIZE);
	origin = memblock_alloc_or_panic(size, PAGE_SIZE);

	/* 每轮把同一 offset 的数据、shadow、origin 页组成一一对应三元组。 */
	for (u64 addr = 0; addr < size; addr += PAGE_SIZE) {
		page = virt_to_page_or_null((char *)start + addr);
		shadow_p = virt_to_page((char *)shadow + addr);
		/* metadata backing 自身不递归分配 metadata，再将其发布到数据页字段。 */
		set_no_shadow_origin_page(shadow_p);
		shadow_page_for(page) = shadow_p;
		origin_p = virt_to_page((char *)origin + addr);
		set_no_shadow_origin_page(origin_p);
		origin_page_for(page) = origin_p;
	}
}

/*
 * kmsan_setup_meta() - 为同阶的 data/shadow/origin 页块建立逐页关联
 *
 * 业务背景：kmsan/init.c 从启动期暂扣页池或 buddy 三块分配中取得等阶块后调用这里，
 * 使以后每个数据页都能独立解析自己的两份 metadata backing。
 * 入参：page、shadow、origin 分别是连续 2^order 页块首个输入输出 page，均不可为 NULL；
 * order 是 buddy 阶数且必须匹配三个块。函数不取得额外引用。
 * 出参/返回：无直接返回；清空每个 metadata 页自身的关联，并把对应指针写入数据页。
 * 三块页的释放/长期持有责任仍由 KMSAN 初始化与 page allocator 协议管理。
 * 注意事项：调用者在页面发布前独占三块，故无需锁；若块大小不匹配会越界写 page 数组。
 */
void kmsan_setup_meta(struct page *page, struct page *shadow,
		      struct page *origin, int order)
{
	/* 按基础页索引建立严格一一对应关系，不能只在复合页 head 保存关联。 */
	for (int i = 0; i < (1 << order); i++) {
		/* 先消除 metadata 递归，再发布 data[i] 可解析的完整 shadow/origin 对。 */
		set_no_shadow_origin_page(&shadow[i]);
		set_no_shadow_origin_page(&origin[i]);
		shadow_page_for(&page[i]) = &shadow[i];
		origin_page_for(&page[i]) = &origin[i];
	}
}
