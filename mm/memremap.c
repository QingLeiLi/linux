// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2015 Intel Corporation. All rights reserved. */
/* 设备/devres、I/O 属性、KASAN 与内存热插拔共同构成设备页上线和撤销边界。 */
#include <linux/device.h>
#include <linux/io.h>
#include <linux/kasan.h>
#include <linux/memory_hotplug.h>
#include <linux/memremap.h>
/* folio 释放、页区/PFN 编码及 swap 侧设备页路径提供核心 MM 对象契约。 */
#include <linux/swap.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/swapops.h>
/* 基础类型、按变量等待与 XArray 分别承载地址、FS_DAX 唤醒和 PFN 索引。 */
#include <linux/types.h>
#include <linux/wait_bit.h>
#include <linux/xarray.h>
#include "internal.h"

static DEFINE_XARRAY(pgmap_array);

/*
 * pgmap_array 以物理页帧号为索引，发布每段已上线 ZONE_DEVICE 内存所属的
 * dev_pagemap。写者通过 XArray 更新并在删除后等待 RCU；读者在 RCU 临界区
 * 查找并尝试取得 pgmap->ref，因而“索引可见”和“对象仍存活”共同成立。
 */

/*
 * The memremap() and memremap_pages() interfaces are alternately used
 * to map persistent memory namespaces. These interfaces place different
 * constraints on the alignment and size of the mapping (namespace).
 * memremap() can map individual PAGE_SIZE pages. memremap_pages() can
 * only map subsections (2MB), and at least one architecture (PowerPC)
 * the minimum mapping granularity of memremap_pages() is 16MB.
 *
 * The role of memremap_compat_align() is to communicate the minimum
 * arch supported alignment of a namespace such that it can freely
 * switch modes without violating the arch constraint. Namely, do not
 * allow a namespace to be PAGE_SIZE aligned since that namespace may be
 * reconfigured into a mode that requires SUBSECTION_SIZE alignment.
 */
/*
 * memremap() 与 memremap_pages() 可交替映射持久内存 namespace，但前者能按
 * PAGE_SIZE 映射，后者至少要求 SUBSECTION_SIZE，PowerPC 等架构甚至要求
 * 16 MiB。该接口返回两种模式都能接受的最小对齐，阻止先以页粒度创建、
 * 后切换到 ZONE_DEVICE 模式时才发现物理范围无法上线。
 */
#ifndef CONFIG_ARCH_HAS_MEMREMAP_COMPAT_ALIGN
/*
 * 业务背景：为未提供架构覆盖的系统给 DAX/NVDIMM namespace 校验提供通用
 * 对齐下限；调用链通常是设备范围创建/重配置 -> 本函数 -> 范围对齐检查。
 * 入参：无。
 * 出参/返回：返回 SUBSECTION_SIZE 字节，不取得引用也不改变任何状态。
 * 注意事项：纯常量查询，不睡眠、无需锁；定义了架构实现时本函数不编译，
 * 调用者必须使用返回值而不能硬编码通用粒度。
 */
unsigned long memremap_compat_align(void)
{
	return SUBSECTION_SIZE;
}
EXPORT_SYMBOL_GPL(memremap_compat_align);
#endif

/*
 * 业务背景：memunmap_pages() 撤销物理范围时先阻止后续 PFN 查找命中，避免
 * 新读者在页元数据拆除期间取得 pgmap；这是 XArray 摘除与最终释放的边界。
 * 入参：range 是只借用的闭区间，单位为物理字节；非空且调用期间稳定。
 * 出参/返回：无直接返回值；对应 PFN 槽被清空，并等待先前 RCU 读者离开。
 * 注意事项：xa_store_range() 可分配内部节点并可能睡眠，故要求可睡眠上下文；
 * synchronize_rcu() 只结束旧读侧生命周期，旧读者已取得的 percpu_ref 仍由
 * memunmap_pages() 的 completion 等待负责排空。
 */
static void pgmap_array_delete(struct range *range)
{
	/* 先禁止新查找命中，再跨越 grace period 排空仍持裸 XArray 值的读者。 */
	xa_store_range(&pgmap_array, PHYS_PFN(range->start), PHYS_PFN(range->end),
			NULL, GFP_KERNEL);
	synchronize_rcu();
}

/*
 * 业务背景：把一个 dev_pagemap 子范围的字节起点换算为第一个可作为设备页
 * 使用的 PFN；首范围可能把前缀页留给 altmap 中的 vmemmap 元数据。
 * 入参：pgmap 为借用且已初始化的映射；range_id 是 [0,nr_range) 的输入索引。
 * 出参/返回：返回首个可用 PFN，不改变 pgmap 或引用所有权。
 * 注意事项：调用者须保证 ranges 稳定；只有 range 0 应用 altmap 偏移，多范围
 * 配合 altmap 会在 pagemap_range() 被拒绝；函数不睡眠且不自行加锁。
 */
static unsigned long pfn_first(struct dev_pagemap *pgmap, int range_id)
{
	/* range/pfn 均是借用的局部视图，pfn 的单位是 PAGE_SIZE 页帧。 */
	struct range *range = &pgmap->ranges[range_id];
	unsigned long pfn = PHYS_PFN(range->start);

	/* 非首范围没有 altmap 保留前缀，物理起点就是可用起点。 */
	if (range_id)
		return pfn;
	return pfn + vmem_altmap_offset(pgmap_altmap(pgmap));
}

/*
 * 业务背景：memory failure 等路径拿到 PFN 后，用本函数确认它既落在 pgmap
 * 的声明范围内，也没有落入首范围由 altmap 占用的元数据前缀。
 * 入参：pgmap 为调用者借用且需保持存活；pfn 是待验证的物理页帧号。
 * 出参/返回：true 表示该 PFN 属于可用设备页，false 表示越界或位于保留前缀；
 * 不获取引用、不修改对象。
 * 注意事项：只读线性扫描，不睡眠、无内部锁；调用者必须另行稳定 pgmap
 * 生命周期，不能把 true 当作对 page 内容或在线状态的长期锁定。
 */
bool pgmap_pfn_valid(struct dev_pagemap *pgmap, unsigned long pfn)
{
	/* i 是范围游标；range 仅在本次迭代借用。 */
	int i;

	/* 多范围可能不连续，必须逐段比较闭区间，不能只检查首尾总包络。 */
	for (i = 0; i < pgmap->nr_range; i++) {
		struct range *range = &pgmap->ranges[i];

		/* 命中声明区间后再排除 altmap 元数据占用的首部 PFN。 */
		if (pfn >= PHYS_PFN(range->start) &&
		    pfn <= PHYS_PFN(range->end))
			return pfn >= pfn_first(pgmap, i);
	}

	return false;
}

/*
 * 业务背景：为初始化、引用计费和拆除计算某一物理范围的半开 PFN 终点。
 * 入参：pgmap 为借用映射；range_id 是有效范围索引。
 * 出参/返回：返回 range 末字节之后的 PFN，不改变状态或 ownership。
 * 注意事项：range_len() 与右移共同把闭字节区间转成半开页帧区间；调用者
 * 已保证范围页对齐，函数不校验、不睡眠、无需锁。
 */
static unsigned long pfn_end(struct dev_pagemap *pgmap, int range_id)
{
	/* range 是当前子范围的只读借用视图，生命周期受 pgmap 覆盖。 */
	const struct range *range = &pgmap->ranges[range_id];

	return (range->start + range_len(range)) >> PAGE_SHIFT;
}

/*
 * 业务背景：计算一个范围贡献多少个 vmemmap 复合元数据单元，供 percpu_ref
 * 批量持有/归还映射生命周期；一个单元覆盖 2^vmemmap_shift 个基础页。
 * 入参：pgmap 为借用；range_id 是 [0,nr_range) 的输入索引。
 * 出参/返回：返回排除 altmap 前缀后的元数据单元数，不产生副作用。
 * 注意事项：依赖范围长度与复合阶数已通过 memremap_pages() 校验；不睡眠。
 */
static unsigned long pfn_len(struct dev_pagemap *pgmap, unsigned long range_id)
{
	/* 先算可用 PFN 数，再按 vmemmap_shift 折算复合 folio 个数。 */
	return (pfn_end(pgmap, range_id) -
		pfn_first(pgmap, range_id)) >> pgmap->vmemmap_shift;
}

/*
 * 业务背景：在 pgmap 引用已排空后撤销一个已上线的 ZONE_DEVICE 范围，是
 * memunmap_pages() 对每段执行的实际拆除步骤。
 * 入参：pgmap 是仍由调用者拥有的映射描述；range_id 为有效段索引，二者均
 * 为借用输入，范围内设备 folio 已不再使用。
 * 出参/返回：无直接返回值；zone/架构映射、PFN 跟踪和全局 PFN 索引被撤销。
 * 注意事项：可睡眠；mem_hotplug_begin/done 串行化内存拓扑修改。调用前必须
 * kill 并排空 pgmap->ref，否则并发页用户会观察被拆除的 memmap。
 */
static void pageunmap_range(struct dev_pagemap *pgmap, int range_id)
{
	/* range 是物理字节闭区间；first_page 用来定位它所属的 ZONE_DEVICE。 */
	struct range *range = &pgmap->ranges[range_id];
	struct page *first_page;

	/* make sure to access a memmap that was actually initialized */
	/*
	 * 只取排除 altmap 后确实初始化过的首个 struct page，避免触碰保留给
	 * vmemmap 自身、并未按设备页初始化的元数据页。
	 */
	first_page = pfn_to_page(pfn_first(pgmap, range_id));

	/* pages are dead and unused, undo the arch mapping */
	/*
	 * 这些页已死亡且无人使用；在内存热插拔写侧临界区先从 zone 摘除，
	 * 再按 CPU 是否可访问选择仅移除 memmap 或同时撤销线性映射/KASAN shadow。
	 */
	mem_hotplug_begin();
	remove_pfn_range_from_zone(page_zone(first_page), PHYS_PFN(range->start),
				   PHYS_PFN(range_len(range)));
	/* PRIVATE 内存无 CPU 线性映射，只需撤销 add_pages() 创建的 page 元数据。 */
	if (pgmap->type == MEMORY_DEVICE_PRIVATE) {
		__remove_pages(PHYS_PFN(range->start),
			       PHYS_PFN(range_len(range)), NULL, pgmap);
	} else {
		/* CPU 可访问类型与 arch_add_memory()/KASAN shadow 的上线步骤严格配对。 */
		arch_remove_memory(range->start, range_len(range),
				pgmap_altmap(pgmap), pgmap);
		kasan_remove_zero_shadow(__va(range->start), range_len(range));
	}
	mem_hotplug_done();

	/* 拓扑已撤销，再取消 PFN 映射属性跟踪并阻止后续 get_dev_pagemap()。 */
	pfnmap_untrack(PHYS_PFN(range->start), range_len(range));
	pgmap_array_delete(range);
}

/*
 * 业务背景：驱动解绑或上线失败时完整撤销 memremap_pages() 建立的映射；调用
 * 链是驱动/devres release -> 本函数 -> 每段 pageunmap_range()。
 * 入参：pgmap 为调用者拥有且已成功初始化的输入输出对象；调用结束后其 ref
 * 已销毁、范围不再可查，调用者不得继续分配其中页面。
 * 出参/返回：无直接返回值；杀死并排空所有页/查找引用，拆除全部已登记范围。
 * 注意事项：会等待 completion、RCU 和热插拔锁，必须在可睡眠上下文调用；
 * nr_range 必须只包含已成功上线的前缀，失败展开正依赖这个不变量。
 */
void memunmap_pages(struct dev_pagemap *pgmap)
{
	/* i 是已上线范围游标。 */
	int i;

	/* kill 关闭新引用入口；此后旧的 get_dev_pagemap()/folio 引用只能递减。 */
	percpu_ref_kill(&pgmap->ref);
	/*
	 * FS_DAX/GENERIC/P2PDMA 上线时按元数据 folio 预持一组基准引用，先归还
	 * 它们；PRIVATE/COHERENT 的引用则由实际 folio 分配和释放动态配对。
	 */
	if (pgmap->type != MEMORY_DEVICE_PRIVATE &&
	    pgmap->type != MEMORY_DEVICE_COHERENT)
		for (i = 0; i < pgmap->nr_range; i++)
			percpu_ref_put_many(&pgmap->ref, pfn_len(pgmap, i));

	/* release 回调完成意味着 percpu_ref 已归零，页元数据现在可以安全拆除。 */
	wait_for_completion(&pgmap->done);

	/* 按成功上线顺序逐段撤销，随后销毁 percpu_ref 的内部资源。 */
	for (i = 0; i < pgmap->nr_range; i++)
		pageunmap_range(pgmap, i);
	percpu_ref_exit(&pgmap->ref);

	/* altmap.alloc 非零说明驱动/架构未归还全部自托管 vmemmap 页，只能告警。 */
	WARN_ONCE(pgmap->altmap.alloc, "failed to free all reserved pages\n");
}
EXPORT_SYMBOL_GPL(memunmap_pages);

/*
 * 业务背景：把 devres 的无类型 release 回调适配到 memunmap_pages()。
 * 入参：data 必须是 devm_memremap_pages() 注册且仍有效的 dev_pagemap，借用。
 * 出参/返回：无直接返回值；同步拆除映射，副作用与 memunmap_pages() 相同。
 * 注意事项：由设备资源释放路径在可睡眠上下文调用；不能重复注册/手工拆除。
 */
static void devm_memremap_pages_release(void *data)
{
	memunmap_pages(data);
}

/*
 * 业务背景：percpu_ref 最后一个引用消失时唤醒正在拆图的 memunmap_pages()。
 * 入参：ref 是嵌入 pgmap 的已归零引用对象，回调期间仅借用。
 * 出参/返回：无直接返回值；完成 pgmap->done，不释放 pgmap 内存。
 * 注意事项：可能在原子上下文执行，不能睡眠；container_of 只恢复宿主地址，
 * 宿主生命周期由等待该 completion 的驱动/拆除路径保证。
 */
static void dev_pagemap_percpu_release(struct percpu_ref *ref)
{
	/* ref 是嵌入成员，container_of 得到仍由拆除者持有的 dev_pagemap。 */
	struct dev_pagemap *pgmap = container_of(ref, struct dev_pagemap, ref);

	complete(&pgmap->done);
}

/*
 * 业务背景：把 dev_pagemap 的一个物理范围注册为 ZONE_DEVICE，并建立 PFN
 * 查找、属性跟踪、架构映射和 struct page；由 memremap_pages() 逐段调用。
 * 入参：pgmap 是待上线的输入输出描述且由调用者持有；params 携带 pgprot、
 * altmap 和 pgmap 的借用热插拔参数；range_id 为有效索引；nid<0 表示采用
 * 当前内存 NUMA 节点，否则指定目标节点。任何指针都不可为 NULL。
 * 出参/返回：0 表示该段已完成页元数据初始化并可由后续驱动使用；负 errno
 * 表示未上线，函数撤销本段已发布的 XArray/PFN/KASAN/架构状态，不接管对象。
 * 注意事项：会分配并取得内存热插拔写锁，允许睡眠；pgmap 尚未对驱动发布，
 * 同区间不得已有 pgmap 或 System RAM。成功后的拆除责任交给 memunmap_pages()。
 */
static int pagemap_range(struct dev_pagemap *pgmap, struct mhp_params *params,
		int range_id, int nid)
{
	/*
	 * 变量地图：is_private 决定是否建立 CPU 线性映射；range 是当前物理段；
	 * conflict_pgmap 临时持有冲突映射引用；error/is_ram 分别传递 errno 和
	 * System RAM 相交分类。
	 */
	const bool is_private = pgmap->type == MEMORY_DEVICE_PRIVATE;
	struct range *range = &pgmap->ranges[range_id];
	struct dev_pagemap *conflict_pgmap;
	int error, is_ram;

	/* altmap 只描述首段内的 vmemmap 存储，多段使用会让偏移/回收账本失真。 */
	if (WARN_ONCE(pgmap_altmap(pgmap) && range_id > 0,
				"altmap not supported for multiple ranges\n"))
		return -EINVAL;

	/*
	 * 分别探测首尾 PFN，拒绝落在已有 ZONE_DEVICE section 的范围；取得的
	 * 临时引用必须立即 put，-ENOMEM 在这里表示资源区间冲突而非本次分配失败。
	 */
	conflict_pgmap = get_dev_pagemap(PHYS_PFN(range->start));
	if (conflict_pgmap) {
		WARN(1, "Conflicting mapping in same section\n");
		put_dev_pagemap(conflict_pgmap);
		return -ENOMEM;
	}

	/* 首 PFN 无冲突仍不足以证明末端无冲突，因此对闭区间终点重复检查。 */
	conflict_pgmap = get_dev_pagemap(PHYS_PFN(range->end));
	if (conflict_pgmap) {
		WARN(1, "Conflicting mapping in same section\n");
		put_dev_pagemap(conflict_pgmap);
		return -ENOMEM;
	}

	/* ZONE_DEVICE 不能覆盖或部分覆盖已登记的 System RAM 资源。 */
	is_ram = region_intersects(range->start, range_len(range),
		IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE);

	/* mixed 与完全 RAM 都拒绝；继续会为同一 PFN 建立冲突的内存模型。 */
	if (is_ram != REGION_DISJOINT) {
		WARN_ONCE(1, "attempted on %s region %#llx-%#llx\n",
				is_ram == REGION_MIXED ? "mixed" : "ram",
				range->start, range->end);
		return -ENXIO;
	}

	/*
	 * 先把 PFN->pgmap 发布给子节在线查询；后续任一步失败都从 err_pfn_remap
	 * 摘除并等待 RCU。此时 ref 已 live，读者可安全取得生命周期引用。
	 */
	error = xa_err(xa_store_range(&pgmap_array, PHYS_PFN(range->start),
				PHYS_PFN(range->end), pgmap, GFP_KERNEL));
	if (error)
		return error;

	/* 未指定 NUMA 归属时采用当前 CPU 的内存节点作为放置策略。 */
	if (nid < 0)
		nid = numa_mem_id();

	/* 让架构验证/规范化 PFN 映射 pgprot；失败时仅需撤销已发布的 XArray。 */
	error = pfnmap_track(PHYS_PFN(range->start), range_len(range),
			     &params->pgprot);
	if (error)
		goto err_pfn_remap;

	/*
	 * 热插拔层检查对齐和可添加范围；CPU 可访问内存要求可建立 direct map，
	 * PRIVATE 类型只需要 page 元数据，因此传入相反的 mapping_required。
	 */
	if (!mhp_range_allowed(range->start, range_len(range), !is_private)) {
		error = -EINVAL;
		goto err_kasan;
	}

	mem_hotplug_begin();

	/*
	 * For device private memory we call add_pages() as we only need to
	 * allocate and initialize struct page for the device memory. More-
	 * over the device memory is un-accessible thus we do not want to
	 * create a linear mapping for the memory like arch_add_memory()
	 * would do.
	 *
	 * For all other device memory types, which are accessible by
	 * the CPU, we do want the linear mapping and thus use
	 * arch_add_memory().
	 */
	/*
	 * PRIVATE 内存 CPU 不可读写，只调用 add_pages() 分配并初始化 struct page，
	 * 不创建 arch_add_memory() 会建立的线性映射。其余类型可由 CPU 访问，必须
	 * 先建立 KASAN 零 shadow，再交给架构上线；两者失败路径分别成对撤销。
	 */
	if (is_private) {
		error = add_pages(nid, PHYS_PFN(range->start),
				PHYS_PFN(range_len(range)), params);
	} else {
		/* KASAN shadow 必须早于 direct map 发布，防止新虚拟地址无 shadow。 */
		error = kasan_add_zero_shadow(__va(range->start), range_len(range));
		if (error) {
			mem_hotplug_done();
			goto err_kasan;
		}

		error = arch_add_memory(nid, range->start, range_len(range),
					params);
	}

	/*
	 * 架构页表/section 创建成功后，在同一 hotplug 临界区把 PFN 移入
	 * ZONE_DEVICE；这是 zone 统计与 page_zone() 开始反映设备区的提交点。
	 */
	if (!error) {
		struct zone *zone;

		zone = &NODE_DATA(nid)->node_zones[ZONE_DEVICE];
		move_pfn_range_to_zone(zone, PHYS_PFN(range->start),
				PHYS_PFN(range_len(range)), params->altmap,
				MIGRATE_MOVABLE, false);
	}

	mem_hotplug_done();
	/* add/arch_add 失败时尚未完成 zone 提交，按类型撤销 shadow 后统一回滚。 */
	if (error)
		goto err_add_memory;

	/*
	 * Initialization of the pages has been deferred until now in order
	 * to allow us to do the work while not holding the hotplug lock.
	 */
	/*
	 * 页结构初始化特意延迟到这里，以免长时间持有全局 hotplug 锁；拓扑已经
	 * 稳定地属于 ZONE_DEVICE，但驱动尚未拿到成功返回，故外部还不会使用页。
	 */
	memmap_init_zone_device(&NODE_DATA(nid)->node_zones[ZONE_DEVICE],
				PHYS_PFN(range->start),
				PHYS_PFN(range_len(range)), pgmap);
	/*
	 * 静态类型为每个 vmemmap folio 预持映射引用，直至统一拆除；PRIVATE 与
	 * COHERENT 则在 zone_device_page_init()/free 回调间动态计数。
	 */
	if (pgmap->type != MEMORY_DEVICE_PRIVATE &&
	    pgmap->type != MEMORY_DEVICE_COHERENT)
		percpu_ref_get_many(&pgmap->ref, pfn_len(pgmap, range_id));
	return 0;

err_add_memory:
	/* CPU 可访问分支已创建 KASAN shadow，但架构上线失败，先按逆序撤销。 */
	if (!is_private)
		kasan_remove_zero_shadow(__va(range->start), range_len(range));
err_kasan:
	/* 到此 PFN 属性跟踪已成功，而页/架构映射未提交或已自行撤销。 */
	pfnmap_untrack(PHYS_PFN(range->start), range_len(range));
err_pfn_remap:
	/* 最后摘除最早发布的 PFN->pgmap，RCU 排空后本段恢复为完全未登记。 */
	pgmap_array_delete(range);
	return error;
}


/*
 * Not device managed version of devm_memremap_pages, undone by
 * memunmap_pages().  Please use devm_memremap_pages if you have a struct
 * device available.
 */
/*
 * 这是不绑定 struct device 的版本，调用者必须显式以 memunmap_pages() 撤销；
 * 若已有设备对象，应使用 devm 版本让解绑/探测失败自动清理。
 */
/*
 * 业务背景：把一个或多个设备物理范围变成带 struct page 的 ZONE_DEVICE，
 * 供 DAX、设备私有/一致内存和 PCI P2P 等上层按页参与 MM 协议。
 * 入参：pgmap 是调用者拥有的输入输出描述，ranges/type/ops/owner 按类型预置；
 * nid 是目标 NUMA 节点，负值表示由每段上线时选择当前内存节点。
 * 出参/返回：成功返回首范围起点的线性虚拟地址；失败返回 ERR_PTR(errno)，
 * 已成功的前缀范围会全部撤销，pgmap ownership 始终留给调用者。
 * 注意事项：会分配、等待 RCU 并操作热插拔拓扑，允许且要求可睡眠上下文；
 * 成功后必须与 memunmap_pages() 配对，拆除前驱动不得再分配或持有其中 folio。
 */
void *memremap_pages(struct dev_pagemap *pgmap, int nid)
{
	/*
	 * params 是逐段热插拔共享的栈内参数；nr_range 保存原始总数，因为 pgmap 的
	 * 同名字段在循环中临时充当“已成功上线段数”的回滚水位；error/i 记录失败。
	 */
	struct mhp_params params = {
		.altmap = pgmap_altmap(pgmap),
		.pgmap = pgmap,
		.pgprot = PAGE_KERNEL,
	};
	const int nr_range = pgmap->nr_range;
	int error, i;

	/* 空范围或超过核心可表达的复合 folio 阶数都在创建 ref 前无副作用失败。 */
	if (WARN_ONCE(!nr_range, "nr_range must be specified\n"))
		return ERR_PTR(-EINVAL);
	if (WARN_ONCE(pgmap->vmemmap_shift > MAX_FOLIO_ORDER,
		      "requested folio size unsupported\n"))
		return ERR_PTR(-EINVAL);

	/* 每个设备类型在上线前验证其必需回调，并选择 CPU 映射属性。 */
	switch (pgmap->type) {
	case MEMORY_DEVICE_PRIVATE:
		/* CPU 不可访问页必须能迁回 RAM、回收 folio，并以 owner 限制跨设备访问。 */
		if (!IS_ENABLED(CONFIG_DEVICE_PRIVATE)) {
			WARN(1, "Device private memory not supported\n");
			return ERR_PTR(-EINVAL);
		}
		if (!pgmap->ops || !pgmap->ops->migrate_to_ram) {
			WARN(1, "Missing migrate_to_ram method\n");
			return ERR_PTR(-EINVAL);
		}
		/* folio_free 与每次设备页最终 put 配对，缺失会泄漏设备 backing/ref。 */
		if (!pgmap->ops->folio_free) {
			WARN(1, "Missing folio_free method\n");
			return ERR_PTR(-EINVAL);
		}
		/* owner 用于迁移/访问路径确认页属于目标设备，禁止外来 pgmap 混用。 */
		if (!pgmap->owner) {
			WARN(1, "Missing owner\n");
			return ERR_PTR(-EINVAL);
		}
		break;
	case MEMORY_DEVICE_COHERENT:
		/* CPU/设备一致内存无需 migrate_to_ram，但仍必须有释放回调与 owner。 */
		if (!pgmap->ops->folio_free) {
			WARN(1, "Missing folio_free method\n");
			return ERR_PTR(-EINVAL);
		}
		if (!pgmap->owner) {
			WARN(1, "Missing owner\n");
			return ERR_PTR(-EINVAL);
		}
		break;
	case MEMORY_DEVICE_FS_DAX:
		/* 文件 DAX 的持久内存不得继承内存加密属性，改用 decrypted direct map。 */
		params.pgprot = pgprot_decrypted(params.pgprot);
		break;
	case MEMORY_DEVICE_GENERIC:
		/* 通用 CPU 可访问设备内存沿用 PAGE_KERNEL，不要求类型专用回调。 */
		break;
	case MEMORY_DEVICE_PCI_P2PDMA:
		/* PCI BAR 面向 peer DMA，CPU 映射采用 noncached 避免普通 RAM 缓存语义。 */
		params.pgprot = pgprot_noncached(params.pgprot);
		break;
	default:
		/* 未初始化/未知类型仅告警；随后创建会缺少可靠语义，供开发期尽早暴露。 */
		WARN(1, "Invalid pgmap type %d\n", pgmap->type);
		break;
	}

	/* done 与 ref 构成拆除栅栏：kill 后最后一次 put 触发 release 并完成等待。 */
	init_completion(&pgmap->done);
	error = percpu_ref_init(&pgmap->ref, dev_pagemap_percpu_release, 0,
				GFP_KERNEL);
	if (error)
		return ERR_PTR(error);

	/*
	 * Clear the pgmap nr_range as it will be incremented for each
	 * successfully processed range. This communicates how many
	 * regions to unwind in the abort case.
	 */
	/*
	 * 暂把 nr_range 清零，并在每段完全成功后递增，使失败时 memunmap_pages()
	 * 只遍历已经提交的前缀，不会触碰当前失败段或尚未处理段。
	 */
	pgmap->nr_range = 0;
	error = 0;
	/* 逐段上线；当前段失败立即停止，成功计数只在 pagemap_range() 返回 0 后发布。 */
	for (i = 0; i < nr_range; i++) {
		error = pagemap_range(pgmap, &params, i, nid);
		if (error)
			break;
		pgmap->nr_range++;
	}

	/* 部分成功属于整体失败：同步拆除成功前缀，再恢复调用者声明的原始段数。 */
	if (i < nr_range) {
		memunmap_pages(pgmap);
		pgmap->nr_range = nr_range;
		return ERR_PTR(error);
	}

	/* 全部范围上线后，返回首物理起点的 direct-map 地址作为驱动访问基址。 */
	return __va(pgmap->ranges[0].start);
}
EXPORT_SYMBOL_GPL(memremap_pages);

/**
 * devm_memremap_pages - remap and provide memmap backing for the given resource
 * @dev: hosting device for @res
 * @pgmap: pointer to a struct dev_pagemap
 *
 * Notes:
 * 1/ At a minimum the range and type members of @pgmap must be initialized
 *    by the caller before passing it to this function
 *
 * 2/ The altmap field may optionally be initialized, in which case
 *    PGMAP_ALTMAP_VALID must be set in pgmap->flags.
 *
 * 3/ The ref field may optionally be provided, in which pgmap->ref must be
 *    'live' on entry and will be killed and reaped at
 *    devm_memremap_pages_release() time, or if this routine fails.
 *
 * 4/ range is expected to be a host memory range that could feasibly be
 *    treated as a "System RAM" range, i.e. not a device mmio range, but
 *    this is not enforced.
 */
/*
 * 该接口为给定资源重映射并提供 memmap backing。调用者至少初始化 pgmap 的
 * range/type；若使用 altmap，必须同时置 PGMAP_ALTMAP_VALID。原文还允许入口
 * 提供已 live 的 ref，并说明失败或 devm release 时会 kill/reap。范围预期是
 * 可按 System RAM 方式管理的主机内存，而不是普通设备 MMIO，但原文说明这里
 * 不强制该约束。
 *
 * 修正说明：当前实现无条件 init_completion() 和 percpu_ref_init()，并不复用
 * 调用者预初始化的 ref；上述第 3 点描述的是旧接口契约。当前映射统一在函数
 * 内建立 ref，并在失败或 devres 释放时 kill、排空和销毁。
 */
/*
 * 业务背景：在 memremap_pages() 上增加设备资源生命周期绑定，使 probe
 * 失败、driver detach 和显式 devm release 都能自动撤销 ZONE_DEVICE。
 * 入参：dev 是非空宿主设备的借用指针并提供 NUMA 节点/devres 栈；pgmap 是
 * 调用者持有的输入输出描述，必须满足底层各类型契约。
 * 出参/返回：成功返回首范围虚拟基址并向 devres 转移“调用 memunmap_pages”
 * 的清理责任；失败返回 ERR_PTR(errno)，底层映射已回滚或 reset action 已拆除。
 * 注意事项：可睡眠；dev 必须比 devres action 活得久，pgmap 存储也必须持续
 * 到 action 被释放，成功后不得再另行直接 memunmap_pages() 造成双重拆除。
 */
void *devm_memremap_pages(struct device *dev, struct dev_pagemap *pgmap)
{
	/* ret 承载地址/错误指针，error 只承载 devres action 注册结果。 */
	int error;
	void *ret;

	/* 先完成实际上线；失败时尚未向 devres 发布任何清理动作。 */
	ret = memremap_pages(pgmap, dev_to_node(dev));
	if (IS_ERR(ret))
		return ret;

	/*
	 * add_action_or_reset 原子化“登记清理”与“登记失败立即清理”：错误返回时
	 * release 已同步拆图，因此调用者不会收到一个无人负责的成功映射。
	 */
	error = devm_add_action_or_reset(dev, devm_memremap_pages_release,
			pgmap);
	if (error)
		return ERR_PTR(error);
	return ret;
}
EXPORT_SYMBOL_GPL(devm_memremap_pages);

/*
 * 业务背景：允许驱动早于设备整体销毁主动执行并移除对应 devres action。
 * 入参：dev 是登记 action 的宿主设备；pgmap 是登记时相同的映射描述，均借用。
 * 出参/返回：无直接返回值；若匹配 action 存在则同步执行 memunmap_pages() 并
 * 从 devres 栈摘除，之后设备释放不会再次执行它。
 * 注意事项：可睡眠；dev/pgmap 必须精确匹配且调用前无活跃设备页使用者。
 */
void devm_memunmap_pages(struct device *dev, struct dev_pagemap *pgmap)
{
	devm_release_action(dev, devm_memremap_pages_release, pgmap);
}
EXPORT_SYMBOL_GPL(devm_memunmap_pages);

/**
 * get_dev_pagemap() - take a new live reference on the dev_pagemap for @pfn
 * @pfn: page frame number to lookup page_map
 */
/*
 * 按 PFN 查找 page_map，并且只在其 percpu_ref 仍 live 时取得一个新引用；
 * NULL 同时表示未登记或映射已经进入 kill/拆除阶段。
 */
/*
 * 业务背景：页故障、热插拔与 memory failure 等路径从 PFN 安全过渡到对应
 * dev_pagemap，避免 XArray 查找与驱动拆除并发造成 use-after-free。
 * 入参：pfn 是物理页帧号纯输入，无对齐之外的额外 ownership。
 * 出参/返回：成功返回持有一个 pgmap->ref 的指针，调用者必须 put_dev_pagemap；
 * NULL 表示无映射或映射已被 kill，不产生待释放引用。
 * 注意事项：RCU 临界区内不睡眠；引用只保证 pgmap 生命周期，不冻结其字段；
 * tryget_live_rcu 与 memunmap_pages() 的 kill 构成“新引用关闭”竞态边界。
 */
struct dev_pagemap *get_dev_pagemap(unsigned long pfn)
{
	/* pgmap 是待升级为持有引用的查找结果；phys 是 PFN 对应的字节地址。 */
	struct dev_pagemap *pgmap;
	resource_size_t phys = PFN_PHYS(pfn);

	/* RCU 保证从 XArray 取值到 tryget 期间对象尚未释放。 */
	rcu_read_lock();
	pgmap = xa_load(&pgmap_array, PHYS_PFN(phys));
	/* kill 已发生时拒绝新引用并把命中折叠为 NULL。 */
	if (pgmap && !percpu_ref_tryget_live_rcu(&pgmap->ref))
		pgmap = NULL;
	rcu_read_unlock();

	return pgmap;
}
EXPORT_SYMBOL_GPL(get_dev_pagemap);

/*
 * 业务背景：folio 最后一个普通引用归零后，__folio_put() 把 ZONE_DEVICE folio
 * 交到这里，按设备内存类型恢复可再次分配状态、唤醒等待者或交还驱动。
 * 入参：folio 是引用计数刚降到零的非空输入输出对象；本函数接管这次最终
 * put 的处置，但 backing 和 pgmap 仍归驱动/映射生命周期管理。
 * 出参/返回：无直接返回值；清理 memcg/匿名状态并按 type 调用 folio_free、
 * 重置计数或唤醒 FS_DAX 等待者；PRIVATE/COHERENT 还归还映射生命周期引用。
 * 注意事项：可能从 put 热路径进入，类型回调须遵守自身上下文约束；pgmap
 * 必须有效。引用归零不等于释放 struct page，它常被设备池重新使用。
 */
void free_zone_device_folio(struct folio *folio)
{
	/* pgmap/nr 在清理前快照；i 用于清除每个基础页的匿名独占标志。 */
	struct dev_pagemap *pgmap = folio->pgmap;
	unsigned long nr = folio_nr_pages(folio);
	int i;

	/* 无 pgmap 的 ZONE_DEVICE folio 破坏初始化不变量，只告警并避免空指针。 */
	if (WARN_ON_ONCE(!pgmap))
		return;

	/* 普通内存计费先解除；驱动回收后该 folio 不再属于原进程 memcg。 */
	mem_cgroup_uncharge(folio);

	/* 匿名设备 folio 的每个基础页都可能带独占位，复用前必须全部清除。 */
	if (folio_test_anon(folio)) {
		for (i = 0; i < nr; i++)
			__ClearPageAnonExclusive(folio_page(folio, i));
	}

	/*
	 * When a device managed page is freed, the folio->mapping field
	 * may still contain a (stale) mapping value. For example, the
	 * lower bits of folio->mapping may still identify the folio as an
	 * anonymous folio. Ultimately, this entire field is just stale
	 * and wrong, and it will cause errors if not cleared.
	 *
	 * For other types of ZONE_DEVICE pages, migration is either
	 * handled differently or not done at all, so there is no need
	 * to clear folio->mapping.
	 *
	 * FS DAX pages clear the mapping when the folio->share count hits
	 * zero which indicating the page has been removed from the file
	 * system mapping.
	 */
	/*
	 * 设备管理页释放时 mapping 可能仍含旧 address_space，低位甚至仍把它编码为
	 * 匿名 folio；继续保留会让下次分配误判类型。PRIVATE/COHERENT/P2PDMA 在此
	 * 清空。GENERIC 的迁移语义不同；FS_DAX 在 share 归零、脱离文件映射时清空，
	 * 因而这两类不在这里处理。
	 */
	if (pgmap->type != MEMORY_DEVICE_FS_DAX &&
	    pgmap->type != MEMORY_DEVICE_GENERIC)
		folio->mapping = NULL;

	/* type 决定“零引用”是交还驱动、进入空闲态还是仅通知等待者。 */
	switch (pgmap->type) {
	case MEMORY_DEVICE_PRIVATE:
	case MEMORY_DEVICE_COHERENT:
		/* 驱动回收 backing 后，按基础页数归还分配时持有的 pgmap 引用。 */
		if (WARN_ON_ONCE(!pgmap->ops || !pgmap->ops->folio_free))
			break;
		pgmap->ops->folio_free(folio);
		percpu_ref_put_many(&pgmap->ref, nr);
		break;

	case MEMORY_DEVICE_GENERIC:
		/*
		 * Reset the refcount to 1 to prepare for handing out the page
		 * again.
		 */
		/* 将计数重置为 1，恢复设备池可再次借出该页的基准持有状态。 */
		folio_set_count(folio, 1);
		break;

	case MEMORY_DEVICE_FS_DAX:
		/* share/pin 协调者睡在 page 地址上；零引用只负责通知其重新检查条件。 */
		wake_up_var(&folio->page);
		break;

	case MEMORY_DEVICE_PCI_P2PDMA:
		/* P2P 页交回 BAR 管理驱动，但映射引用由整体上线基准持有，不在此 put。 */
		if (WARN_ON_ONCE(!pgmap->ops || !pgmap->ops->folio_free))
			break;
		pgmap->ops->folio_free(folio);
		break;
	}
}

/*
 * 业务背景：设备驱动从已建立的 ZONE_DEVICE memmap 中领用一个 order 阶页时，
 * 清除前次 folio 布局并建立新的锁定 folio；是驱动分配与最终 free 的配对入口。
 * 入参：page 是连续 2^order 个已归驱动管理的页描述且作为输入输出；pgmap 是
 * 这些页所属映射的借用指针；order 范围为 0..MAX_ORDER_NR_PAGES 所允许阶数。
 * 出参/返回：无直接返回值；每个页清除旧复合/mapping 状态，首 page 计数置 1、
 * 加锁并按需建立 compound folio，同时为本次分配取得一个 pgmap 引用份额。
 * 注意事项：驱动在 memunmap_pages() 后不得调用；返回时 page 仍锁定，后续
 * 初始化/发布与解锁由调用者负责。警告不替代调用者的范围和生命周期保证。
 */
void zone_device_page_init(struct page *page, struct dev_pagemap *pgmap,
			   unsigned int order)
{
	/* new_page/new_folio 逐个覆盖旧布局；i 是基础页游标。 */
	struct page *new_page = page;
	unsigned int i;

	/* 过大 order 会使遍历/复合布局无效；这是调用者 bug 的诊断检查。 */
	VM_WARN_ON_ONCE(order > MAX_ORDER_NR_PAGES);

	/* 清理整个新 folio 跨度，避免前一轮更高阶 folio 留下 head/order 编码。 */
	for (i = 0; i < (1UL << order); ++i, ++new_page) {
		struct folio *new_folio = (struct folio *)new_page;

		/*
		 * new_page could have been part of previous higher order folio
		 * which encodes the order, in page + 1, in the flags bits. We
		 * blindly clear bits which could have set my order field here,
		 * including page head.
		 */
		/*
		 * new_page 可能曾是更高阶 folio 的内部页，page+1 的 flags 低位编码过
		 * order；盲清低 8 位也清除旧 PageHead，之后再由 prep_compound_page 重建。
		 */
		new_page->flags.f &= ~0xffUL;	/* Clear possible order, page head */

#ifdef NR_PAGES_IN_LARGE_FOLIO
		/*
		 * This pointer math looks odd, but new_page could have been
		 * part of a previous higher order folio, which sets _nr_pages
		 * in page + 1 (new_page). Therefore, we use pointer casting to
		 * correctly locate the _nr_pages bits within new_page which
		 * could have modified by previous higher order folio.
		 */
		/*
		 * 启用显式大 folio 页数时，当前 new_page 也可能正是“前一页作为 folio
		 * 起点”所对应的 _nr_pages 存储槽，故反向一页后强转才能清到正确字段。
		 */
		((struct folio *)(new_page - 1))->_nr_pages = 0;
#endif

		/* 清除前次映射/共享状态并重新绑定 pgmap；赋 pgmap 同时覆盖旧 compound_head。 */
		new_folio->mapping = NULL;
		new_folio->pgmap = pgmap;	/* Also clear compound head */
		/* 写 pgmap 的联合布局同时清除了旧 compound_head 编码。 */
		new_folio->share = 0;   /* fsdax only, unused for device private */
		/* share 只供 FS_DAX pin/截断协调；PRIVATE 设备页不消费该字段。 */
		VM_WARN_ON_FOLIO(folio_ref_count(new_folio), new_folio);
		VM_WARN_ON_FOLIO(!folio_is_zone_device(new_folio), new_folio);
	}

	/*
	 * Drivers shouldn't be allocating pages after calling
	 * memunmap_pages().
	 */
	/* 驱动在拆图后继续分配属于生命周期错误；tryget 失败仅告警，不能恢复映射。 */
	WARN_ON_ONCE(!percpu_ref_tryget_many(&page_pgmap(page)->ref, 1 << order));
	/* 新 folio 以一个调用者引用且锁定的状态返回，未解锁前不会被并发使用。 */
	set_page_count(page, 1);
	lock_page(page);

	/* 高阶请求最后建立 head/tail 关系，此前必须已清完整个跨度的旧编码。 */
	if (order)
		prep_compound_page(page, order);
}
EXPORT_SYMBOL_GPL(zone_device_page_init);
