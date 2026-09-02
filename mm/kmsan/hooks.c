// SPDX-License-Identifier: GPL-2.0
/*
 * KMSAN hooks for kernel subsystems.
 *
 * These functions handle creation of KMSAN metadata for memory allocations.
 *
 * Copyright (C) 2018-2022 Google LLC
 * Author: Alexander Potapenko <glider@google.com>
 *
 */
/*
 * 译注：本文件提供内核子系统调用的 KMSAN 钩子，负责在内存分配与外部 I/O 生命周期边界
 * 创建、传播或清除 KMSAN 元数据；版权与作者信息如上游原文所列。
 */

#include <linux/cacheflush.h>
#include <linux/dma-direction.h>
#include <linux/gfp.h>
#include <linux/kmsan.h>
/* MM、scatterlist、USB 与 uaccess 依赖对应四类需要元数据同步的边界。 */
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/usb.h>

#include "../internal.h"
#include "../slab.h"
#include "kmsan.h"

/*
 * 学习提示：这些钩子把子系统的“分配、释放、I/O 完成”转换为 shadow/origin 状态变化。
 * 外层先阻止 runtime 递归，再进入未插桩实现；退出时必须恢复当前任务的递归深度。
 */
/*
 * Instrumented functions shouldn't be called under
 * kmsan_enter_runtime()/kmsan_leave_runtime(), because this will lead to
 * skipping effects of functions like memset() inside instrumented code.
 */
/*
 * 译注：进入 KMSAN runtime 后不得调用带插桩函数，否则其中 memset 等操作的元数据副作用会被
 * runtime 递归门禁跳过；因此外层包装与 internal 未插桩实现之间必须严格划清边界。
 */

/*
 * 业务背景：任务创建路径需要先建立 KMSAN 任务上下文，供后续插桩代码保存参数与返回值元数据。
 * 入参：task 是待初始化的新任务借用指针，必须非空；本函数不取得 task 引用或所有权。
 * 出参/返回：无直接返回值；初始化 task->kmsan_ctx，task 的所有权保持属于进程创建路径。
 * 注意事项：可在创建路径睡眠规则下调用；用 runtime 深度屏蔽内部初始化，进入与离开必须严格配对。
 */
void kmsan_task_create(struct task_struct *task)
{
	/* 新任务的 KMSAN 上下文在 runtime 内初始化，避免初始化代码自身被跟踪。 */
	kmsan_enter_runtime();
	kmsan_internal_task_create(task);
	kmsan_leave_runtime();
}

/*
 * 业务背景：任务退出后不应再产生 KMSAN 报告，此钩子把当前任务永久切入禁用状态。
 * 入参：task 是退出任务的借用指针；当前实现依据 current 操作，不转移或释放 task。
 * 出参/返回：无直接返回值；启用且不在 runtime 时递增 current 的禁用深度。
 * 注意事项：必须由退出任务自身调用；全局关闭或 runtime 递归时无副作用，禁用状态无需在退出后恢复。
 */
void kmsan_task_exit(struct task_struct *task)
{
	/* 未启用或已经在 runtime 中时不再改变当前任务深度。 */
	if (!kmsan_enabled || kmsan_in_runtime())
		return;

	/* 退出任务后不应再报告其未初始化读取，故永久增加禁用深度。 */
	kmsan_disable_current();
}

/*
 * 业务背景：SLAB 分配成功后需按清零标志建立对象的已初始化或未初始化元数据状态。
 * 入参：s 是非空 cache 借用指针；object 是新对象、可为 NULL；flags 是本次 GFP 输入且不被修改。
 * 出参/返回：无直接返回值；可能更新 object[0..object_size) 的 shadow/origin，不改变对象 ownership。
 * 注意事项：ctor 和 TYPESAFE_BY_RCU cache 保留旧状态而跳过；runtime 内旁路，内部 poison 可能受 GFP 约束。
 */
void kmsan_slab_alloc(struct kmem_cache *s, void *object, gfp_t flags)
{
	/* 分配失败没有对象元数据可更新，runtime 递归也必须旁路。 */
	if (unlikely(object == NULL))
		return;
	if (!kmsan_enabled || kmsan_in_runtime())
		return;
	/*
	 * There's a ctor or this is an RCU cache - do nothing. The memory
	 * status hasn't changed since last use.
	 */
	/* 译注：存在构造函数或属于 RCU cache 时不处理，因为内存状态应从上次使用延续。 */
	/* ctor/RCU cache 保留上次对象状态，不能按普通“新分配”重新毒化。 */
	if (s->ctor || (s->flags & SLAB_TYPESAFE_BY_RCU))
		return;

	kmsan_enter_runtime();
	/* 清零分配已初始化；普通分配则产生新的未初始化 origin。 */
	if (flags & __GFP_ZERO)
		kmsan_internal_unpoison_memory(object, s->object_size,
					       KMSAN_POISON_CHECK);
	else
		kmsan_internal_poison_memory(object, s->object_size, flags,
					     KMSAN_POISON_CHECK);
	kmsan_leave_runtime();
}

/*
 * 业务背景：普通 slab 对象归还后通过 free poison 检测后续未合法重新分配的访问。
 * 入参：s 是对象所属 cache 的借用指针；object 是即将释放的非空对象借用地址，ownership 由 slab 回收。
 * 出参/返回：无直接返回值；适用时毒化对象元数据并记录 free origin，不负责实际释放对象。
 * 注意事项：TYPESAFE_BY_RCU/ctor cache 明确豁免；禁止 reclaim 的 GFP 避免 origin 构造递归回收。
 */
void kmsan_slab_free(struct kmem_cache *s, void *object)
{
	/* free 钩子只在 KMSAN 正常跟踪上下文中制造 use-after-free 毒。 */
	if (!kmsan_enabled || kmsan_in_runtime())
		return;

	/* RCU slabs could be legally used after free within the RCU period */
	/* 译注：RCU slab 在 free 后的宽限期内仍允许合法访问，因此不能立即毒化。 */
	/* TYPESAFE_BY_RCU 允许宽限期内合法访问，过早毒化会误报。 */
	if (unlikely(s->flags & SLAB_TYPESAFE_BY_RCU))
		return;
	/*
	 * If there's a constructor, freed memory must remain in the same state
	 * until the next allocation. We cannot save its state to detect
	 * use-after-free bugs, instead we just keep it unpoisoned.
	 */
	/*
	 * 译注：带构造函数的对象在下次分配前必须保持原状态，无法保存 free 状态来检测 UAF，
	 * 所以这里继续保持未毒化。
	 */
	/* ctor 对象跨分配周期保持构造状态，无法用 free poison 表示生命期。 */
	if (s->ctor)
		return;
	kmsan_enter_runtime();
	/* 禁止 reclaim，防止构造 origin 时递归进入内存回收。 */
	kmsan_internal_poison_memory(object, s->object_size,
				     GFP_KERNEL & ~(__GFP_RECLAIM),
				     KMSAN_POISON_CHECK | KMSAN_POISON_FREE);
	kmsan_leave_runtime();
}

/*
 * 业务背景：页分配器承载的大 kmalloc 对象绕过 slab 钩子，需要独立初始化 KMSAN 元数据。
 * 入参：ptr 是新分配对象借用地址、可为 NULL；size 是有效字节数；flags 描述清零和 origin 分配约束。
 * 出参/返回：无直接返回值；更新给定范围元数据，不取得 ptr 所有权。
 * 注意事项：全局关闭/runtime 内或 NULL 时无副作用；调用者保证范围已映射且覆盖完整分配。
 */
void kmsan_kmalloc_large(const void *ptr, size_t size, gfp_t flags)
{
	/* 大对象没有 slab ctor 例外，按调用者给出的精确大小更新元数据。 */
	if (unlikely(ptr == NULL))
		return;
	if (!kmsan_enabled || kmsan_in_runtime())
		return;
	kmsan_enter_runtime();
	/* __GFP_ZERO 与 slab 路径保持同一初始化语义。 */
	if (flags & __GFP_ZERO)
		kmsan_internal_unpoison_memory((void *)ptr, size,
					       /*checked*/ true);
	else
		kmsan_internal_poison_memory((void *)ptr, size, flags,
					     KMSAN_POISON_CHECK);
	kmsan_leave_runtime();
}

/*
 * 业务背景：大 kmalloc 释放前按 compound page 实际大小制造 free poison，覆盖整个页级分配。
 * 入参：ptr 必须是分配起始地址的借用指针，不可为空或指向对象内部；本函数不释放 backing。
 * 出参/返回：无直接返回值；毒化完整 page_size(head_page) 范围并记录 free origin。
 * 注意事项：依赖线性映射和 virt_to_head_page()；错误起始地址触发 KMSAN 警告，runtime/关闭时旁路。
 */
void kmsan_kfree_large(const void *ptr)
{
	/* large free 从虚拟地址反查 compound head，并毒化完整分配阶。 */
	struct page *page;

	if (!kmsan_enabled || kmsan_in_runtime())
		return;
	kmsan_enter_runtime();
	page = virt_to_head_page((void *)ptr);
	/* API 只接受分配起始地址，内部指针会破坏 size 推导。 */
	KMSAN_WARN_ON(ptr != page_address(page));
	kmsan_internal_poison_memory((void *)ptr, page_size(page),
				     GFP_KERNEL & ~(__GFP_RECLAIM),
				     KMSAN_POISON_CHECK | KMSAN_POISON_FREE);
	kmsan_leave_runtime();
}

/*
 * 业务背景：vmalloc 数据区的元数据映射需要把数据虚拟地址转换到 shadow 虚拟区。
 * 入参：addr 是内核 vmalloc 数据虚拟地址数值，纯输入且必须属于 KMSAN 可解析范围。
 * 出参/返回：返回对应 shadow 地址数值；不取得 backing 引用，也不改变任何映射。
 * 注意事项：这是纯转换 helper；调用者负责保证返回地址有效并在需要时持有页表/映射同步条件。
 */
static unsigned long vmalloc_shadow(unsigned long addr)
{
	/* vmalloc 数据地址与 shadow 虚拟地址保持固定元数据映射。 */
	return (unsigned long)kmsan_get_metadata((void *)addr,
						 KMSAN_META_SHADOW);
}

/*
 * 业务背景：与 shadow 转换配对，为同一 vmalloc 数据地址定位 origin 元数据虚拟区。
 * 入参：addr 是 vmalloc 数据虚拟地址数值，纯输入且不可是无法解析的任意地址。
 * 出参/返回：返回对应 origin 地址数值；无输出参数、引用或 ownership 变化。
 * 注意事项：只做地址计算，不验证映射存在；调用者必须与 shadow 范围保持相同端点。
 */
static unsigned long vmalloc_origin(unsigned long addr)
{
	/* origin 区与 shadow 区分离，但覆盖相同数据地址范围。 */
	return (unsigned long)kmsan_get_metadata((void *)addr,
						 KMSAN_META_ORIGIN);
}

/*
 * 业务背景：数据 vmalloc 区撤销时同步撤销 shadow/origin 页表，防止元数据别名继续可见。
 * 入参：start/end 是半开数据虚拟区间且须页对齐、start<=end；均为纯输入地址值。
 * 出参/返回：无直接返回值；撤销两类元数据映射并刷新 vmap cache，但不执行最终 TLB flush。
 * 注意事项：调用者负责 noflush 契约要求的后续 TLB 同步及 backing 生命周期，区间端点必须成对转换。
 */
void kmsan_vunmap_range_noflush(unsigned long start, unsigned long end)
{
	/* 先撤销两类元数据 PTE，再刷新相应 vmap cache；本函数不做 TLB flush。 */
	__vunmap_range_noflush(vmalloc_shadow(start), vmalloc_shadow(end));
	__vunmap_range_noflush(vmalloc_origin(start), vmalloc_origin(end));
	flush_cache_vmap(vmalloc_shadow(start), vmalloc_shadow(end));
	flush_cache_vmap(vmalloc_origin(start), vmalloc_origin(end));
}

/*
 * This function creates new shadow/origin pages for the physical pages mapped
 * into the virtual memory. If those physical pages already had shadow/origin,
 * those are ignored.
 */
/*
 * 译注：该函数为映入虚拟区的物理页创建新的 shadow/origin 页；物理页此前可能关联的元数据
 * 不会被复用，因为这里跟踪的是新 ioremap 虚拟别名的独立元数据映射。
 */
/*
 * 业务背景：ioremap 数据映射需要新建独立 shadow/origin backing，使设备内存也可参与 KMSAN 跟踪。
 * 入参：start/end 是页对齐半开虚拟区间；phys_addr/page_shift 当前不决定元数据页粒度；prot 是映射属性。
 * 出参/返回：成功返回 0 并发布两类映射；失败返回 -ENOMEM 或 vmap 错误，并回滚本调用已建映射/backing。
 * 注意事项：可能睡眠和分配内存；全局关闭/runtime 内返回 0；成功 ownership 交给映射，iounmap 负责释放。
 */
int kmsan_ioremap_page_range(unsigned long start, unsigned long end,
			     phys_addr_t phys_addr, pgprot_t prot,
			     unsigned int page_shift)
{
	/* 每个数据页各分配 order-1 的 shadow/origin backing，并清零初态。 */
	gfp_t gfp_mask = GFP_KERNEL | __GFP_ZERO;
	/* shadow/origin 是当前迭代新分配页；映射成功后以置 NULL 表示 ownership 已交接。 */
	struct page *shadow, *origin;
	/* off 是数据区字节偏移；nr 是页数；err/mapped 是错误；clean 是可回滚成功页数。 */
	unsigned long off = 0;
	int nr, err = 0, clean = 0, mapped;

	/* 旁路返回成功，表示 KMSAN 未参与而非 ioremap 失败。 */
	if (!kmsan_enabled || kmsan_in_runtime())
		return 0;

	/* 数据范围按基础页迭代，page_shift 参数不改变元数据粒度。 */
	nr = (end - start) / PAGE_SIZE;
	kmsan_enter_runtime();
	/* i 是零基页索引，每轮结束同步推进 off，并把已完成页数写入 clean。 */
	for (int i = 0; i < nr; i++, off += PAGE_SIZE, clean = i) {
		/* 两块 backing 都准备好后才尝试发布映射。 */
		shadow = alloc_pages(gfp_mask, 1);
		origin = alloc_pages(gfp_mask, 1);
		/* 部分分配由统一 ret 路径释放，既有迭代映射也会回滚。 */
		if (!shadow || !origin) {
			err = -ENOMEM;
			goto ret;
		}
		/* shadow 映射成功即转移 backing 所有权，用 NULL 防止重复释放。 */
		mapped = __vmap_pages_range_noflush(
			vmalloc_shadow(start + off),
			vmalloc_shadow(start + off + PAGE_SIZE), prot, &shadow,
			PAGE_SHIFT);
		if (mapped) {
			err = mapped;
			goto ret;
		}
		/* NULL 表示当前 shadow 页已由 vmap 映射持有。 */
		shadow = NULL;
		mapped = __vmap_pages_range_noflush(
			vmalloc_origin(start + off),
			vmalloc_origin(start + off + PAGE_SIZE), prot, &origin,
			PAGE_SHIFT);
		/* origin 发布失败需先撤销同页 shadow，再进入跨迭代回滚。 */
		if (mapped) {
			__vunmap_range_noflush(
				vmalloc_shadow(start + off),
				vmalloc_shadow(start + off + PAGE_SIZE));
			err = mapped;
			goto ret;
		}
		/* 两类映射均成功后，本轮 backing 所有权完全交给 vmap。 */
		origin = NULL;
	}
	/* Page mapping loop finished normally, nothing to clean up. */
	/* 译注：逐页映射循环正常结束，不存在需要回滚的已完成前缀。 */
	/* clean=0 区分正常完成，避免把成功映射误当作失败回滚。 */
	clean = 0;

ret:
	if (clean > 0) {
		/* clean 记录此前完整成功的页数，不包含当前失败迭代。 */
		/*
		 * Something went wrong. Clean up shadow/origin pages allocated
		 * on the last loop iteration, then delete mappings created
		 * during the previous iterations.
		 */
		/*
		 * 译注：发生错误时先释放最后一次迭代中尚未交给映射的 shadow/origin 页，
		 * 再删除此前各次迭代已经创建的映射。
		 */
		/* 当前尚未发布的 backing 直接 free，已发布部分通过 vunmap 撤销。 */
		if (shadow)
			__free_pages(shadow, 1);
		if (origin)
			__free_pages(origin, 1);
		/* 两片区间使用相同 clean 终点，保持 shadow/origin 覆盖一致。 */
		__vunmap_range_noflush(
			vmalloc_shadow(start),
			vmalloc_shadow(start + clean * PAGE_SIZE));
		__vunmap_range_noflush(
			vmalloc_origin(start),
			vmalloc_origin(start + clean * PAGE_SIZE));
	}
	/* 成功与失败都刷新完整元数据范围，使页表变更对体系结构可见。 */
	flush_cache_vmap(vmalloc_shadow(start), vmalloc_shadow(end));
	flush_cache_vmap(vmalloc_origin(start), vmalloc_origin(end));
	kmsan_leave_runtime();
	return err;
}

/*
 * 业务背景：ioremap 生命周期结束时撤销并释放该范围逐页创建的 shadow/origin backing。
 * 入参：start/end 是与创建时一致的页对齐半开数据虚拟区间，纯输入且 start<=end。
 * 出参/返回：无直接返回值；撤销元数据映射、释放存在的 order-1 backing 并刷新 vmap cache。
 * 注意事项：可在 KMSAN 正常上下文调用且内部进入 runtime；缺失映射可退化跳过，调用后地址不可再使用。
 */
void kmsan_iounmap_page_range(unsigned long start, unsigned long end)
{
	/* unmap 先保存每页 backing 指针，再撤销映射并释放物理页。 */
	/* 两个 v_* 是虚拟游标，shadow/origin 是当前迭代借用的 backing 页。 */
	unsigned long v_shadow, v_origin;
	struct page *shadow, *origin;
	/* nr 是数据基础页总数，循环局部 i 是零基页索引。 */
	int nr;

	if (!kmsan_enabled || kmsan_in_runtime())
		return;

	/* shadow/origin 游标同步按页前进，保持两类元数据成对释放。 */
	nr = (end - start) / PAGE_SIZE;
	kmsan_enter_runtime();
	v_shadow = (unsigned long)vmalloc_shadow(start);
	v_origin = (unsigned long)vmalloc_origin(start);
	for (int i = 0; i < nr;
	     i++, v_shadow += PAGE_SIZE, v_origin += PAGE_SIZE) {
		/* 映射可能缺失，or_null 允许清理路径幂等退化。 */
		shadow = kmsan_vmalloc_to_page_or_null((void *)v_shadow);
		origin = kmsan_vmalloc_to_page_or_null((void *)v_origin);
		/* 从当前游标撤销到结尾，后续迭代再查询剩余 backing。 */
		__vunmap_range_noflush(v_shadow, vmalloc_shadow(end));
		__vunmap_range_noflush(v_origin, vmalloc_origin(end));
		if (shadow)
			__free_pages(shadow, 1);
		if (origin)
			__free_pages(origin, 1);
	}
	/* backing 全部释放后刷新两片 vmap cache 并退出 runtime。 */
	flush_cache_vmap(vmalloc_shadow(start), vmalloc_shadow(end));
	flush_cache_vmap(vmalloc_origin(start), vmalloc_origin(end));
	kmsan_leave_runtime();
}

/*
 * 业务背景：copy_to_user 完成后只检查实际复制前缀，防止未初始化内核数据泄露给用户。
 * 入参：to 是目标用户地址；from 是源借用指针；to_copy 是请求字节数；left 是未复制字节数且不大于请求。
 * 出参/返回：无直接返回值；真用户目标触发检查，compat 内核目标传播实际前缀元数据，ownership 均不变。
 * 注意事项：数据复制已发生，报告不能撤销泄露；保存/恢复 uaccess 状态，零长度或完全失败时无副作用。
 */
void kmsan_copy_to_user(void __user *to, const void *from, size_t to_copy,
			size_t left)
{
	/* left 是未复制字节数，实际成功范围为 to_copy-left。 */
	unsigned long ua_flags;

	if (!kmsan_enabled || kmsan_in_runtime())
		return;
	/*
	 * At this point we've copied the memory already. It's hard to check it
	 * before copying, as the size of actually copied buffer is unknown.
	 */
	/* 译注：此时数据复制已经结束；复制前无法知道最终成功字节数，所以只能事后检查成功前缀。 */

	/* copy_to_user() may copy zero bytes. No need to check. */
	/* 译注：copy_to_user() 允许请求零字节，此时没有需要检查的数据。 */
	/* 零请求或完全失败都没有已泄露到目标的字节，无需检查。 */
	if (!to_copy)
		return;
	/* Or maybe copy_to_user() failed to copy anything. */
	/* 译注：也可能一个字节都未复制成功，同样不存在实际泄露范围。 */
	if (to_copy <= left)
		return;

	/* 保存 uaccess 状态，内部元数据访问完成后必须原样恢复。 */
	ua_flags = user_access_save();
	if (!IS_ENABLED(CONFIG_ARCH_HAS_NON_OVERLAPPING_ADDRESS_SPACE) ||
	    (u64)to < TASK_SIZE) {
		/* This is a user memory access, check it. */
		/* 译注：目标确属用户地址空间，检查实际复制源范围是否已初始化。 */
		/* 真用户目标检查源已初始化性，并携带用户地址用于报告。 */
		kmsan_internal_check_memory((void *)from, to_copy - left, to,
					    REASON_COPY_TO_USER);
	} else {
		/* Otherwise this is a kernel memory access. This happens when a
		 * compat syscall passes an argument allocated on the kernel
		 * stack to a real syscall.
		 * Don't check anything, just copy the shadow of the copied
		 * bytes.
		 */
		/*
		 * 译注：否则目标是内核地址，例如 compat 系统调用把内核栈参数传给真实系统调用；
		 * 此时不做泄露检查，只复制成功字节的 shadow 元数据。
		 */
		/* compat 内核目标不报告泄露，只镜像成功复制部分的元数据。 */
		kmsan_enter_runtime();
		kmsan_internal_memmove_metadata((void *)to, (void *)from,
						to_copy - left);
		kmsan_leave_runtime();
	}
	user_access_restore(ua_flags);
}
EXPORT_SYMBOL(kmsan_copy_to_user);

/*
 * 业务背景：未插桩或汇编 memmove 完成数据移动后，由此钩子复制相同范围的 shadow/origin。
 * 入参：to/from 是可重叠的目标/源借用地址；size 是字节数，调用者保证两范围有效。
 * 出参/返回：无直接返回值；目标元数据变为源元数据的 memmove 结果，不改变数据或对象所有权。
 * 注意事项：全局关闭/runtime 内无操作；进入/离开 runtime 配对，重叠正确性由 internal helper 保证。
 */
void kmsan_memmove(void *to, const void *from, size_t size)
{
	/* 数据复制已由调用者完成，本钩子只传播相同重叠语义的元数据。 */
	if (!kmsan_enabled || kmsan_in_runtime())
		return;

	kmsan_enter_runtime();
	kmsan_internal_memmove_metadata(to, (void *)from, size);
	kmsan_leave_runtime();
}
EXPORT_SYMBOL(kmsan_memmove);

/* Helper function to check an URB. */
/* 译注：下面的包装依据 URB 传输方向检查或解除缓冲区毒化。 */
/*
 * 业务背景：USB 提交/完成边界需要把设备方向转换为“提交前检查”或“接收后解除毒化”。
 * 入参：urb 是可空借用指针；is_out=true 表示主机到设备，false 表示设备到主机。
 * 出参/返回：无直接返回值；OUT 检查 transfer_buffer，IN 将 transfer_buffer_length 范围标为已初始化。
 * 注意事项：不取得 URB/缓冲区引用；调用者必须在缓冲区仍有效且 DMA 同步完成的正确方向边界调用。
 */
void kmsan_handle_urb(const struct urb *urb, bool is_out)
{
	/* OUT 把内存交给设备前检查，IN 完成后把设备写入范围标为已初始化。 */
	if (!urb)
		return;
	/* NULL URB 无操作；方向决定 check 与 unpoison 的单向转换。 */
	if (is_out)
		/* 提交给设备的缓冲区不得包含未初始化内核数据。 */
		kmsan_internal_check_memory(urb->transfer_buffer,
					    urb->transfer_buffer_length,
					    /*user_addr*/ NULL,
					    REASON_SUBMIT_URB);
	else
		/* 接收完成后设备写入长度内的字节成为可信初始化数据。 */
		kmsan_internal_unpoison_memory(urb->transfer_buffer,
					       urb->transfer_buffer_length,
					       /*checked*/ false);
}
EXPORT_SYMBOL_GPL(kmsan_handle_urb);

/*
 * 业务背景：连续 DMA 路径按页拆分后，本 helper 对单页片段应用方向对应的元数据规则。
 * 入参：addr/size 描述不跨页的有效借用范围；dir 允许四值：BIDIRECTIONAL 检查后解毒，TO_DEVICE 仅检查，FROM_DEVICE 仅解毒，NONE 无操作。
 * 出参/返回：无直接返回值；按 dir 检查或更新范围元数据，不保存地址、不改变 ownership。
 * 注意事项：调用者保证 DMA 所有权切换时点正确；未知枚举值不在契约内，四个 case 处理后均直接返回调用者循环。
 */
static void kmsan_handle_dma_page(const void *addr, size_t size,
				  enum dma_data_direction dir)
{
	/* 双向 DMA 先检查设备将读取的旧内容，再认可设备可能写回的新内容。 */
	switch (dir) {
	case DMA_BIDIRECTIONAL:
		/* 进入条件：设备可能双向访问；先验证出站旧内容，再认可入站覆盖，随后结束本片段。 */
		kmsan_internal_check_memory((void *)addr, size,
					    /*user_addr*/ NULL, REASON_ANY);
		kmsan_internal_unpoison_memory((void *)addr, size,
					       /*checked*/ false);
		break;
	case DMA_TO_DEVICE:
		/* 进入条件：设备只读主存；提交前验证全部字节，元数据不变，随后结束本片段。 */
		kmsan_internal_check_memory((void *)addr, size,
					    /*user_addr*/ NULL, REASON_ANY);
		break;
	case DMA_FROM_DEVICE:
		/* 进入条件：设备写主存；完成同步点后解除范围毒化，随后结束本片段。 */
		kmsan_internal_unpoison_memory((void *)addr, size,
					       /*checked*/ false);
		break;
	case DMA_NONE:
		/* 进入条件：没有 DMA 数据方向；不检查也不改元数据，直接返回上层遍历。 */
		break;
	}
}

/* Helper function to handle DMA data transfers. */
/* 译注：下面的公共入口处理连续物理 DMA 数据传输。 */
/*
 * 业务背景：DMA API 在 CPU/设备同步边界用物理范围通知 KMSAN，本函数将其拆为 allocation-safe 页片段。
 * 入参：phys 是起始物理地址；size 是字节数；dir 是 DMA 四方向枚举，语义与单页 helper 完全一致。
 * 出参/返回：无直接返回值；低端映射逐页检查/解毒，高端内存当前无副作用。
 * 注意事项：不映射 highmem；调用者保证物理范围有效且同步方向正确，函数不取得页面引用且不可跨释放并发。
 */
void kmsan_handle_dma(phys_addr_t phys, size_t size,
		      enum dma_data_direction dir)
{
	/* highmem 缺少稳定线性映射，本通用钩子无法安全取得虚拟地址。 */
	/* page_offset/to_go 以字节计且限定单页片段，addr 是当前线性映射借用游标。 */
	u64 page_offset, to_go;
	void *addr;

	if (PhysHighMem(phys))
		return;
	/* 低端物理地址转线性映射后，再按页界拆分处理。 */
	addr = phys_to_virt(phys);
	/*
	 * The kernel may occasionally give us adjacent DMA pages not belonging
	 * to the same allocation. Process them separately to avoid triggering
	 * internal KMSAN checks.
	 */
	/*
	 * 译注：内核偶尔会提交物理相邻但分属不同 allocation 的 DMA 页；逐页处理可避免一次
	 * KMSAN 内部检查跨越 allocation 边界而触发内部一致性检查。
	 */
	/* 分页避免一次内部检查跨越彼此无关的 allocation 元数据。 */
	while (size > 0) {
		page_offset = offset_in_page(addr);
		to_go = min(PAGE_SIZE - page_offset, (u64)size);
		kmsan_handle_dma_page((void *)addr, to_go, dir);
		/* 地址和剩余长度同步推进，确保循环严格终止。 */
		addr += to_go;
		size -= to_go;
	}
}
EXPORT_SYMBOL_GPL(kmsan_handle_dma);

/*
 * 业务背景：scatter-gather DMA 由多个不连续物理段组成，需要逐段复用连续范围处理协议。
 * 入参：sg 是至少含 nents 项的借用链表；nents>=0；dir 是四值 DMA 方向枚举且对全部项相同。
 * 出参/返回：无直接返回值；逐项检查/解毒对应物理范围，不修改 scatterlist 和 ownership。
 * 注意事项：调用者保证每项已完成相应 DMA 同步且 sg 生命周期覆盖遍历；高端段会由下层跳过。
 */
void kmsan_handle_dma_sg(struct scatterlist *sg, int nents,
			 enum dma_data_direction dir)
{
	/* scatterlist 每个条目独立转换物理地址并复用连续 DMA 路径。 */
	/* item 是当前借用项，i 是零基条目索引，二者仅在循环期间有效。 */
	struct scatterlist *item;
	int i;

	for_each_sg(sg, item, nents, i)
		/* sg_phys/length 描述每段真实 DMA 范围，不使用虚拟连续假设。 */
		kmsan_handle_dma(sg_phys(item), item->length, dir);
}

/* Functions from kmsan-checks.h follow. */
/* 译注：以下实现对应 kmsan-checks.h 对外声明的显式检查接口。 */

/*
 * To create an origin, kmsan_poison_memory() unwinds the stacks and stores it
 * into the stack depot. This may cause deadlocks if done from within KMSAN
 * runtime, therefore we bail out if kmsan_in_runtime().
 */
/*
 * 译注：kmsan_poison_memory() 为创建 origin 会展开栈并写入 Stack Depot；若在 KMSAN runtime
 * 内执行可能死锁，因此检测到 runtime 递归时直接退出。
 */
/*
 * 业务背景：子系统可显式声明一段内存未初始化，并为后续报告创建 origin 证据。
 * 入参：address 是有效范围借用起点；size 是字节数；flags 约束 origin 所需分配，三者均为纯输入。
 * 出参/返回：无直接返回值；成功毒化 shadow/origin，不取得内存 ownership，也不保证实际分配成功可见。
 * 注意事项：origin 构造可能分配/展开栈，故 runtime 内必须旁路；调用者保证范围有效且同步排除并发使用。
 */
void kmsan_poison_memory(const void *address, size_t size, gfp_t flags)
{
	/* poison 可能分配 origin，runtime 内调用会递归或死锁，必须跳过。 */
	if (!kmsan_enabled || kmsan_in_runtime())
		return;
	kmsan_enter_runtime();
	/* The users may want to poison/unpoison random memory. */
	/* 译注：调用者可能要求处理任意有效内存，所以使用 NOCHECK 而不限定普通分配对象。 */
	/* NOCHECK 允许外部子系统描述任意已知有效的内存范围。 */
	kmsan_internal_poison_memory((void *)address, size, flags,
				     KMSAN_POISON_NOCHECK);
	kmsan_leave_runtime();
}
EXPORT_SYMBOL(kmsan_poison_memory);

/*
 * Unlike kmsan_poison_memory(), this function can be used from within KMSAN
 * runtime, because it does not trigger allocations or call instrumented code.
 */
/*
 * 译注：与 poison 接口不同，本函数不会分配内存或调用插桩代码，因此允许在 KMSAN runtime
 * 内部使用。
 */
/*
 * 业务背景：硬件、固件或未插桩代码写入后，子系统用本接口声明结果字节已经初始化。
 * 入参：address 是有效借用起点；size 是字节数；范围可来自普通内核或 uaccess 可达映射。
 * 出参/返回：无直接返回值；清除范围 poison，数据内容和对象 ownership 不变。
 * 注意事项：可从 KMSAN runtime 内调用且不分配；仍需保证范围生命周期和并发写同步，保存/恢复 uaccess 状态。
 */
void kmsan_unpoison_memory(const void *address, size_t size)
{
	/* unpoison 不分配且可在 runtime 内执行，仅全局关闭时旁路。 */
	/* ua_flags 保存调用者 uaccess 状态，必须在所有更新完成后恢复。 */
	unsigned long ua_flags;

	if (!kmsan_enabled)
		return;

	/* 保存 uaccess 标志，使任意内核/用户映射元数据地址都可安全更新。 */
	ua_flags = user_access_save();
	/* The users may want to poison/unpoison random memory. */
	/* 译注：调用者可解除任意有效范围的毒化，因此同样采用 NOCHECK。 */
	kmsan_internal_unpoison_memory((void *)address, size,
				       KMSAN_POISON_NOCHECK);
	user_access_restore(ua_flags);
}
EXPORT_SYMBOL(kmsan_unpoison_memory);

/*
 * Version of kmsan_unpoison_memory() called from IRQ entry functions.
 */
/* 译注：这是供 IRQ 入口函数使用的 kmsan_unpoison_memory() 专用包装。 */
/*
 * 业务背景：IRQ/异常入口由汇编或硬件填充 pt_regs，进入 C 代码前必须认可其为初始化数据。
 * 入参：regs 是非空入口寄存器帧借用指针，至少在本调用期间有效。
 * 出参/返回：无直接返回值；解除 sizeof(*regs) 范围的 poison，不修改寄存器值或所有权。
 * 注意事项：设计用于中断入口，继承 unpoison 可在 runtime 内调用的保证；调用者须在任何检查前执行。
 */
void kmsan_unpoison_entry_regs(const struct pt_regs *regs)
{
	/* 中断入口寄存器由硬件/汇编填充，应整体视为已初始化输入。 */
	kmsan_unpoison_memory((void *)regs, sizeof(*regs));
}

/*
 * 业务背景：子系统在安全边界显式验证内存已初始化，用通用原因生成 KMSAN 报告。
 * 入参：addr 是待读取范围借用起点；size 是字节数，调用者保证范围可访问。
 * 出参/返回：无直接返回值；发现 poison 时产生报告，未修改数据和 ownership。
 * 注意事项：全局关闭时无操作；本包装未检查 runtime 深度，调用者避免从会递归报告的内部路径误用。
 */
void kmsan_check_memory(const void *addr, size_t size)
{
	/* 通用显式检查没有用户目标地址，报告原因归入 REASON_ANY。 */
	if (!kmsan_enabled)
		return;
	return kmsan_internal_check_memory((void *)addr, size,
					   /*user_addr*/ NULL, REASON_ANY);
}
EXPORT_SYMBOL(kmsan_check_memory);

/*
 * 业务背景：成对禁用区域结束时减少当前任务 KMSAN 深度，零值重新允许插桩检查。
 * 入参：无。
 * 出参/返回：无直接返回值；current->kmsan_ctx.depth 减一，不影响其他任务。
 * 注意事项：必须与先前 disable 配对且当前深度非零；错误配对触发警告，任务上下文不可跨任务操作。
 */
void kmsan_enable_current(void)
{
	/* enable 与 disable 采用可嵌套深度；零深度才表示真正启用。 */
	KMSAN_WARN_ON(current->kmsan_ctx.depth == 0);
	current->kmsan_ctx.depth--;
}
EXPORT_SYMBOL(kmsan_enable_current);

/*
 * 业务背景：未插桩或可能递归的临界路径用可嵌套深度暂时关闭当前任务 KMSAN 检查。
 * 入参：无。
 * 出参/返回：无直接返回值；current->kmsan_ctx.depth 加一，直到相同次数 enable 后才恢复。
 * 注意事项：只作用 current；调用者必须在所有出口配对 enable，整数回绕至零会触发警告并破坏门禁。
 */
void kmsan_disable_current(void)
{
	/* 递增后回绕到零表示溢出，WARN 暴露不平衡或极端嵌套。 */
	current->kmsan_ctx.depth++;
	KMSAN_WARN_ON(current->kmsan_ctx.depth == 0);
}
EXPORT_SYMBOL(kmsan_disable_current);
