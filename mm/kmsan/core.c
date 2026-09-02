// SPDX-License-Identifier: GPL-2.0
/*
 * KMSAN runtime library.
 *
 * Copyright (C) 2017-2022 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */

#include <asm/page.h>
#include <linux/compiler.h>
#include <linux/export.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/kmsan_types.h>
/* 内存布局、页表与 slab 头提供被跟踪地址到 metadata/backing 的转换契约。 */
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmzone.h>
#include <linux/percpu-defs.h>
#include <linux/preempt.h>
#include <linux/slab.h>
#include <linux/stackdepot.h>
#include <linux/stacktrace.h>
/* vmalloc 接口用于验证模块区和动态映射区背后的普通物理页。 */
#include <linux/types.h>
#include <linux/vmalloc.h>

#include "../slab.h"
#include "kmsan.h"

/*
 * 本文件是 KMSAN 元数据运行时的核心：编译器钩子和分配器钩子最终在这里
 * 写 shadow/origin，使用点则从这里按 origin 分组并交给 report.c 输出。
 * shadow 逐字节描述“是否初始化”，origin 每四字节保存一条 Stack Depot 链；
 * core.c 不拥有被检测内存，只维护与其生命周期平行的诊断元数据。
 */
/* 初始化末期由 init.c 发布的只读热路径开关；发布后普通钩子才开始工作。 */
bool kmsan_enabled __read_mostly;

/*
 * Per-CPU KMSAN context to be used in interrupts, where current->kmsan is
 * unavailable.
 */
/*
 * 中文翻译：中断中 current->kmsan 不可用，因此使用每 CPU 的 KMSAN 上下文。
 * 它只在禁止抢占/中断的相应运行时路径借用，不表示任务间共享 ownership。
 */
DEFINE_PER_CPU(struct kmsan_ctx, kmsan_percpu_ctx);

/*
 * kmsan_internal_task_create() - 为新任务建立干净的 KMSAN 运行时状态。
 * 业务背景：fork 的 KMSAN hook 在任务对外可运行前调用，避免继承父任务的
 * 递归深度、报告状态和临时返回元数据。@task 是已分配且由 fork 路径持有的
 * 新任务，只借用不接管；无返回值。函数可在进程上下文执行，不能自行加锁。
 * 阶段是清零 task 上下文，再把当前 thread_info 标为已初始化；后者防止编译器
 * 插桩把内核维护的线程字段误报为未初始化，完成后 fork 可继续发布任务。
 */
void kmsan_internal_task_create(struct task_struct *task)
{
	/* ctx 属于 @task；info 是当前创建者的 thread_info，不发生引用转移。 */
	struct kmsan_ctx *ctx = &task->kmsan_ctx;
	struct thread_info *info = current_thread_info();

	__memset(ctx, 0, sizeof(*ctx));
	kmsan_internal_unpoison_memory(info, sizeof(*info), false);
}

/*
 * kmsan_internal_poison_memory() - 把范围标成未初始化并记录其来源栈。
 * 分配/释放 hook 传入 @address、字节数 @size、Stack Depot 分配掩码 @flags，
 * @poison_flags 还编码“释放后使用”和“必须存在 metadata”。本函数不拥有范围，
 * 可否睡眠由 @flags 决定；返回 void，但会写 shadow/origin 并分配栈句柄。
 * 先编码深度 0 的来源类别并保存栈，再一次性提交元数据；checked 失败会告警，
 * 非 checked 的未跟踪范围则允许静默退化。
 */
void kmsan_internal_poison_memory(void *address, size_t size, gfp_t flags,
				  unsigned int poison_flags)
{
	/* extra_bits 与 Stack Depot 句柄同行传播，保留 poison 类型而不占 shadow。 */
	u32 extra_bits =
		kmsan_extra_bits(/*depth*/ 0, poison_flags & KMSAN_POISON_FREE);
	bool checked = poison_flags & KMSAN_POISON_CHECK;
	depot_stack_handle_t handle;

	handle = kmsan_save_stack_with_flags(flags, extra_bits);
	kmsan_internal_set_shadow_origin(address, size, -1, handle, checked);
}

/*
 * kmsan_internal_unpoison_memory() - 发布一段内存已经完成初始化。
 * @address/@size 是借用的字节范围，@checked 要求范围必须有 metadata；无返回值。
 * 它把 shadow 清零，并仅在整个四字节槽均已初始化时清 origin，避免抹掉相邻
 * 仍带毒字节的来源。调用后编译器检查可把该范围当作已初始化。
 */
void kmsan_internal_unpoison_memory(void *address, size_t size, bool checked)
{
	kmsan_internal_set_shadow_origin(address, size, 0, 0, checked);
}

/*
 * kmsan_save_stack_with_flags() - 保存当前调用栈并附带 KMSAN 私有位。
 * @flags 控制 Stack Depot 的分配上下文，@extra 是 chain depth/UAF 编码；返回的
 * handle 由全局 Stack Depot 管理，调用者只保存整数句柄，无单独释放责任。
 * 栈保存和去重可能按 GFP 约束分配；失败句柄仍经 extra-bit helper 原样返回。
 */
depot_stack_handle_t kmsan_save_stack_with_flags(gfp_t flags,
						 unsigned int extra)
{
	/* entries 仅在本调用栈有效，stack_depot_save() 会复制/去重其内容。 */
	unsigned long entries[KMSAN_STACK_DEPTH];
	unsigned int nr_entries;
	depot_stack_handle_t handle;

	/* 第一阶段抓取至多 KMSAN_STACK_DEPTH 个返回地址。 */
	nr_entries = stack_trace_save(entries, KMSAN_STACK_DEPTH, 0);

	/* 第二阶段取得稳定句柄，最后在句柄保留位中发布 KMSAN 分类。 */
	handle = stack_depot_save(entries, nr_entries, flags);
	return stack_depot_set_extra_bits(handle, extra);
}

/* Copy the metadata following the memmove() behavior. */
/* 中文翻译：按照 memmove() 的重叠方向复制元数据。 */
/*
 * kmsan_internal_memmove_metadata() - 同步搬运数据对应的 shadow/origin。
 * memcpy/memmove hooks 在真实数据搬运后调用；@dst/@src 均为借用地址，@n 为字节
 * 数，函数不复制数据本身且无返回值。要求每个范围内部 metadata 连续；不加锁，
 * 调用者必须像保护原数据一样排除并发写。目标未跟踪时无事可做，源未跟踪则把
 * 目标发布为已初始化；正常路径按重叠方向逐字节复制并给传播来源追加链节点。
 */
void kmsan_internal_memmove_metadata(void *dst, void *src, size_t n)
{
	/* prev_* 缓存相邻同源字节的链结果，避免重复保存同一传播栈。 */
	depot_stack_handle_t prev_old_origin = 0, prev_new_origin = 0;
	int i, iter, step, src_off, dst_off, oiter_src, oiter_dst;
	depot_stack_handle_t old_origin = 0, new_origin = 0;
	depot_stack_handle_t *origin_src, *origin_dst;
	u8 *shadow_src, *shadow_dst;
	u32 *align_shadow_dst;
	bool backwards;

	/* 第一阶段解析目标；没有目标 shadow 表示整个目标不受 KMSAN 跟踪。 */
	shadow_dst = kmsan_get_metadata(dst, KMSAN_META_SHADOW);
	if (!shadow_dst)
		return;
	KMSAN_WARN_ON(!kmsan_metadata_is_contiguous(dst, n));
	align_shadow_dst =
		(u32 *)ALIGN_DOWN((u64)shadow_dst, KMSAN_ORIGIN_SIZE);

	shadow_src = kmsan_get_metadata(src, KMSAN_META_SHADOW);
	if (!shadow_src) {
		/* @src is untracked: mark @dst as initialized. */
		/* 中文翻译：源未被跟踪，因而把目标按“已初始化”处理。 */
		kmsan_internal_unpoison_memory(dst, n, /*checked*/ false);
		return;
	}
	KMSAN_WARN_ON(!kmsan_metadata_is_contiguous(src, n));

	/* 正常路径要求 shadow 与 origin 成对存在；WARN 捕获映射协议破坏。 */
	origin_dst = kmsan_get_metadata(dst, KMSAN_META_ORIGIN);
	origin_src = kmsan_get_metadata(src, KMSAN_META_ORIGIN);
	KMSAN_WARN_ON(!origin_dst || !origin_src);

	/* 第二阶段选择方向；倒序避免重叠且 dst 在后时先覆盖尚未读取的源 metadata。 */
	backwards = dst > src;
	step = backwards ? -1 : 1;
	iter = backwards ? n - 1 : 0;
	src_off = (u64)src % KMSAN_ORIGIN_SIZE;
	dst_off = (u64)dst % KMSAN_ORIGIN_SIZE;

	/* Copy shadow bytes one by one, updating the origins if necessary. */
	/* 中文翻译：逐字节复制 shadow，并在需要时同步更新 origin。 */
	for (i = 0; i < n; i++, iter += step) {
		/* origin 是四字节槽，源/目标地址未对齐时槽下标可能不同。 */
		oiter_src = (iter + src_off) / KMSAN_ORIGIN_SIZE;
		oiter_dst = (iter + dst_off) / KMSAN_ORIGIN_SIZE;
		if (!shadow_src[iter]) {
			/* 已初始化字节先清 shadow；只有整个目标槽为零才可清共享 origin。 */
			shadow_dst[iter] = 0;
			if (!align_shadow_dst[oiter_dst])
				origin_dst[oiter_dst] = 0;
			continue;
		}
		/* 带毒字节复制 poison 值，并把“本次传播栈”接到旧来源之前。 */
		shadow_dst[iter] = shadow_src[iter];
		old_origin = origin_src[oiter_src];
		if (old_origin == prev_old_origin)
			new_origin = prev_new_origin;
		else {
			/*
			 * kmsan_internal_chain_origin() may return
			 * NULL, but we don't want to lose the previous
			 * origin value.
			 */
			/* 中文翻译：链构造可能返回空句柄，此时必须保留旧来源而不能丢诊断。 */
			new_origin = kmsan_internal_chain_origin(old_origin);
			if (!new_origin)
				new_origin = old_origin;
		}
		/* 写 origin 是本轮对外可观察的来源提交点，随后更新同源缓存。 */
		origin_dst[oiter_dst] = new_origin;
		prev_new_origin = new_origin;
		prev_old_origin = old_origin;
	}
}

/*
 * kmsan_internal_chain_origin() - 为一次值传播把当前栈接到旧 origin。
 * @id 是借用的 Stack Depot 句柄，0 表示无来源；返回新句柄，达到深度上限或
 * 无来源时返回原值，分配失败可返回 0。调用者因此必须保留旧 @id 作退化路径。
 * 使用 __GFP_HIGH 保存诊断链，可能触发受该掩码约束的分配；没有外部锁要求。
 */
depot_stack_handle_t kmsan_internal_chain_origin(depot_stack_handle_t id)
{
	/* entries 的三元组依次是链魔数、本次传播栈句柄和上一节点句柄。 */
	unsigned long entries[3];
	u32 extra_bits;
	int depth;
	bool uaf;
	depot_stack_handle_t handle;

	if (!id)
		return id;
	/*
	 * Make sure we have enough spare bits in @id to hold the UAF bit and
	 * the chain depth.
	 */
	/* 中文翻译：编译期确认句柄保留位足以同时容纳 UAF 标志和链深度。 */
	BUILD_BUG_ON((1 << STACK_DEPOT_EXTRA_BITS) <=
		     (KMSAN_MAX_ORIGIN_DEPTH << 1));

	/* 第一阶段从旧句柄恢复深度和 UAF 属性，传播不得丢失释放来源。 */
	extra_bits = stack_depot_get_extra_bits(id);
	depth = kmsan_depth_from_eb(extra_bits);
	uaf = kmsan_uaf_from_eb(extra_bits);

	/*
	 * Stop chaining origins once the depth reached KMSAN_MAX_ORIGIN_DEPTH.
	 * This mostly happens in the case structures with uninitialized padding
	 * are copied around many times. Origin chains for such structures are
	 * usually periodic, and it does not make sense to fully store them.
	 */
	/* 中文翻译：达到最大深度就停止；反复复制 padding 常形成周期链，无需全存。 */
	if (depth == KMSAN_MAX_ORIGIN_DEPTH)
		return id;

	/* 第二阶段递增深度并构造新节点，旧 @id 仍是链尾。 */
	depth++;
	extra_bits = kmsan_extra_bits(depth, uaf);

	entries[0] = KMSAN_CHAIN_MAGIC_ORIGIN;
	entries[1] = kmsan_save_stack_with_flags(__GFP_HIGH, 0);
	entries[2] = id;
	/*
	 * @entries is a local var in non-instrumented code, so KMSAN does not
	 * know it is initialized. Explicitly unpoison it to avoid false
	 * positives when stack_depot_save() passes it to instrumented code.
	 */
	/* 中文翻译：entries 位于未插桩代码的栈上，显式反毒后才能交给插桩代码。 */
	kmsan_internal_unpoison_memory(entries, sizeof(entries), false);
	/* stack_depot_save() 复制数组并发布稳定句柄，本地 entries 随后即可失效。 */
	handle = stack_depot_save(entries, ARRAY_SIZE(entries), __GFP_HIGH);
	return stack_depot_set_extra_bits(handle, extra_bits);
}

/*
 * kmsan_internal_set_shadow_origin() - 原子语义地更新范围的两类元数据。
 * @addr/@size 指定借用字节范围，@b 是写入每个 shadow 字节的 poison 值，
 * @origin 是四字节槽句柄，@checked 决定缺 metadata 是否告警；无返回值。
 * 调用者负责排除对同一数据/metadata 的并发写。先提交 shadow，再按对齐扩展
 * 检查 origin 槽；清毒时只清完全为零的槽，避免伤及范围边缘的相邻 poison。
 */
void kmsan_internal_set_shadow_origin(void *addr, size_t size, int b,
				      u32 origin, bool checked)
{
	/* address/size 会为 origin 粒度向两端对齐，原始 shadow 写入仍严格限于范围。 */
	u64 address = (u64)addr;
	void *shadow_start;
	u32 *aligned_shadow, *origin_start;
	size_t pad = 0;

	/* 第一阶段验证单一映射区并解析 shadow；checked 把静默退化提升为诊断。 */
	KMSAN_WARN_ON(!kmsan_metadata_is_contiguous(addr, size));
	shadow_start = kmsan_get_metadata(addr, KMSAN_META_SHADOW);
	if (!shadow_start) {
		/*
		 * kmsan_metadata_is_contiguous() is true, so either all shadow
		 * and origin pages are NULL, or all are non-NULL.
		 */
		/* 中文翻译：连续性为真意味着 shadow/origin 要么全缺失，要么全存在。 */
		if (checked) {
			pr_err("%s: not memsetting %ld bytes starting at %px, because the shadow is NULL\n",
			       __func__, size, addr);
			KMSAN_WARN_ON(true);
		}
		return;
	}
	/* shadow 的逐字节写入是真正改变初始化状态的发布点。 */
	__memset(shadow_start, b, size);

	/* 第二阶段把起点向下、终点向上扩到 KMSAN_ORIGIN_SIZE 槽边界。 */
	if (IS_ALIGNED(address, KMSAN_ORIGIN_SIZE)) {
		aligned_shadow = shadow_start;
	} else {
		pad = address % KMSAN_ORIGIN_SIZE;
		address -= pad;
		aligned_shadow = shadow_start - pad;
		size += pad;
	}
	size = ALIGN(size, KMSAN_ORIGIN_SIZE);
	/* 元数据连续性保证对齐后的首个 origin 指针覆盖整个循环范围。 */
	origin_start =
		(u32 *)kmsan_get_metadata((void *)address, KMSAN_META_ORIGIN);

	/*
	 * If the new origin is non-zero, assume that the shadow byte is also non-zero,
	 * and unconditionally overwrite the old origin slot.
	 * If the new origin is zero, overwrite the old origin slot iff the
	 * corresponding shadow slot is zero.
	 */
	/*
	 * 中文翻译：非零新来源直接覆盖旧槽；清零来源时，仅当对应四个 shadow
	 * 字节全为零才清旧槽。aligned_shadow 的 u32 读取正好完成这个整体判断。
	 */
	for (int i = 0; i < size / KMSAN_ORIGIN_SIZE; i++) {
		if (origin || !aligned_shadow[i])
			origin_start[i] = origin;
	}
}

/*
 * kmsan_vmalloc_to_page_or_null() - 安全解析 vmalloc/module 地址的 backing page。
 * hooks.c 在释放 vmalloc metadata 页时传入借用的 @vaddr；仅接受两个受支持的
 * 虚拟区，返回仍由原映射拥有且未加引用的 page，失败返回 NULL。调用者必须在
 * 映射生命周期受保护时使用结果；函数不睡眠、不修改映射，也不保证设备 PFN。
 */
struct page *kmsan_vmalloc_to_page_or_null(void *vaddr)
{
	struct page *page;

	/* 先按地址域过滤，避免 vmalloc_to_page() 接收不满足契约的任意内核地址。 */
	if (!kmsan_internal_is_vmalloc_addr(vaddr) &&
	    !kmsan_internal_is_module_addr(vaddr))
		return NULL;
	/* page 未增引用；pfn_valid 再排除没有普通 struct page 语义的映射。 */
	page = vmalloc_to_page(vaddr);
	if (pfn_valid(page_to_pfn(page)))
		return page;
	else
		return NULL;
}

/*
 * kmsan_internal_check_memory() - 按 origin 分段报告范围内的未初始化字节。
 * 编译器/uaccess/USB hooks 传入借用范围 @addr/@size；@user_addr 可空，用来标出
 * 用户目标地址，@reason 选择报告原因。函数不修复或接管内存，无返回值；它可能
 * 打印并按策略 panic。要求 metadata 映射在扫描期间稳定，报告路径自行处理递归。
 * 扫描按页切块，再按 shadow/origin 边界合并连续坏段；未跟踪页和已初始化字节
 * 都会先冲刷前一段，循环结束还必须冲刷尾段。
 */
void kmsan_internal_check_memory(void *addr, size_t size,
				 const void __user *user_addr, int reason)
{
	/* cur_origin/cur_off_start 描述尚未报告的半开坏段，pos 是范围内字节游标。 */
	depot_stack_handle_t cur_origin = 0, new_origin = 0;
	unsigned long addr64 = (unsigned long)addr;
	depot_stack_handle_t *origin = NULL;
	unsigned char *shadow = NULL;
	int cur_off_start = -1;
	int chunk_size;
	size_t pos = 0;

	/* 空范围是无副作用快速路径。 */
	if (!size)
		return;
	KMSAN_WARN_ON(!kmsan_metadata_is_contiguous(addr, size));
	/* 每轮不跨 PAGE_SIZE，确保 kmsan_get_metadata() 返回的线性片段可直接索引。 */
	while (pos < size) {
		chunk_size = min(size - pos,
				 PAGE_SIZE - ((addr64 + pos) % PAGE_SIZE));
		shadow = kmsan_get_metadata((void *)(addr64 + pos),
					    KMSAN_META_SHADOW);
		if (!shadow) {
			/*
			 * This page is untracked. If there were uninitialized
			 * bytes before, report them.
			 */
			/* 中文翻译：本页未跟踪；若前面已有坏段，必须先完成报告。 */
			if (cur_origin) {
				kmsan_report(cur_origin, addr, size,
					     cur_off_start, pos - 1, user_addr,
					     reason);
			}
			/* 清空聚合状态后整页跳过；未跟踪内存按已初始化语义退化。 */
			cur_origin = 0;
			cur_off_start = -1;
			pos += chunk_size;
			continue;
		}
		/* 页内逐字节检查 shadow，只有 poison 字节才读取四字节 origin 槽。 */
		for (int i = 0; i < chunk_size; i++) {
			if (!shadow[i]) {
				/*
				 * This byte is unpoisoned. If there were
				 * poisoned bytes before, report them.
				 */
				/* 中文翻译：遇到已反毒字节时，先报告此前连续的 poison 区间。 */
				if (cur_origin) {
					kmsan_report(cur_origin, addr, size,
						     cur_off_start, pos + i - 1,
						     user_addr, reason);
				}
				cur_origin = 0;
				cur_off_start = -1;
				continue;
			}
			/* origin 指针是借用视图；连续性不变量要求 poison 必有来源页。 */
			origin = kmsan_get_metadata((void *)(addr64 + pos + i),
						    KMSAN_META_ORIGIN);
			KMSAN_WARN_ON(!origin);
			new_origin = *origin;
			/*
			 * Encountered new origin - report the previous
			 * uninitialized range.
			 */
			/* 中文翻译：来源改变意味着上一未初始化区间已经闭合，应立即报告。 */
			if (cur_origin != new_origin) {
				if (cur_origin) {
					kmsan_report(cur_origin, addr, size,
						     cur_off_start, pos + i - 1,
						     user_addr, reason);
				}
				/* 切换聚合状态，新段从当前字节开始等待后续边界。 */
				cur_origin = new_origin;
				cur_off_start = pos + i;
			}
		}
		/* 推进到下一页片段；cur_origin 可以跨页延续同一来源。 */
		pos += chunk_size;
	}
	KMSAN_WARN_ON(pos != size);
	/* 尾段没有自然边界，必须在返回前显式提交最后一次报告。 */
	if (cur_origin) {
		kmsan_report(cur_origin, addr, size, cur_off_start, pos - 1,
			     user_addr, reason);
	}
}

/*
 * kmsan_metadata_is_contiguous() - 验证范围能否用线性 shadow/origin 指针访问。
 * @addr/@size 是借用的数据范围；返回 true 表示范围在单页内、所有页均未跟踪，
 * 或每对相邻 metadata 页在虚拟地址上连续，false 表示映射混合/断裂且已打印
 * 诊断。函数不持锁也不取得引用，调用者须保证 vmalloc metadata 映射不被拆除；
 * 它只检查，不改变元数据，失败后调用者不得继续用线性指针跨页访问。
 */
bool kmsan_metadata_is_contiguous(void *addr, size_t size)
{
	/* cur/next 分别保存相邻数据页的 shadow/origin 起点，仅在映射稳定期借用。 */
	char *cur_shadow = NULL, *next_shadow = NULL, *cur_origin = NULL,
	     *next_origin = NULL;
	u64 cur_addr = (u64)addr, next_addr = cur_addr + PAGE_SIZE;
	depot_stack_handle_t *origin_p;
	bool all_untracked = false;

	/* 空范围与单页范围无需比较相邻映射，是常见快速路径。 */
	if (!size)
		return true;

	/* The whole range belongs to the same page. */
	/* 中文翻译：整个范围位于同一数据页中，不存在跨 metadata 页连续性问题。 */
	if (ALIGN_DOWN(cur_addr + size - 1, PAGE_SIZE) ==
	    ALIGN_DOWN(cur_addr, PAGE_SIZE))
		return true;

	/* 第一阶段以首个数据页定调：全未跟踪模式不允许后续突然出现 metadata。 */
	cur_shadow = kmsan_get_metadata((void *)cur_addr, /*is_origin*/ false);
	if (!cur_shadow)
		all_untracked = true;
	cur_origin = kmsan_get_metadata((void *)cur_addr, /*is_origin*/ true);
	if (all_untracked && cur_origin)
		goto report;

	/* 第二阶段逐页比较；循环增量把 next 滚动成下一轮的 cur。 */
	for (; next_addr < (u64)addr + size;
	     cur_addr = next_addr, cur_shadow = next_shadow,
	     cur_origin = next_origin, next_addr += PAGE_SIZE) {
		/* 两类 metadata 必须成对出现，并分别以 PAGE_SIZE 连续增长。 */
		next_shadow = kmsan_get_metadata((void *)next_addr, false);
		next_origin = kmsan_get_metadata((void *)next_addr, true);
		if (all_untracked) {
			/* 首页未跟踪时，后续页只要出现任一 metadata 就是混合映射。 */
			if (next_shadow || next_origin)
				goto report;
			if (!next_shadow && !next_origin)
				continue;
		}
		/* 非空模式要求 shadow 和 origin 两条虚拟区间同时保持页连续。 */
		if (((u64)cur_shadow == ((u64)next_shadow - PAGE_SIZE)) &&
		    ((u64)cur_origin == ((u64)next_origin - PAGE_SIZE)))
			continue;
		goto report;
	}
	/* 所有边界通过后，调用者可以安全地以首指针加字节偏移访问完整范围。 */
	return true;

report:
	/* 失败路径只诊断、不修复；打印首个断裂边界和起始 origin 帮助定位映射者。 */
	pr_err("%s: attempting to access two shadow page ranges.\n", __func__);
	pr_err("Access of size %ld at %px.\n", size, addr);
	pr_err("Addresses belonging to different ranges: %px and %px\n",
	       (void *)cur_addr, (void *)next_addr);
	pr_err("page[0].shadow: %px, page[1].shadow: %px\n", cur_shadow,
	       next_shadow);
	pr_err("page[0].origin: %px, page[1].origin: %px\n", cur_origin,
	       next_origin);
	/* origin_p 未加引用，只在仍受调用者生命周期保护的当前诊断窗口读取。 */
	origin_p = kmsan_get_metadata(addr, KMSAN_META_ORIGIN);
	if (origin_p) {
		pr_err("Origin: %08x\n", *origin_p);
		kmsan_print_origin(*origin_p);
	} else {
		pr_err("Origin: unavailable\n");
	}
	/* false 是硬边界：上层 WARN 后仍不得把不连续映射当线性数组使用。 */
	return false;
}
