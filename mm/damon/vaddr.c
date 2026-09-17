// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Code for Virtual Address Spaces
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#define pr_fmt(fmt) "damon-va: " fmt

#include <linux/highmem.h>
#include <linux/hugetlb.h>
#include <linux/mman.h>
#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/pagewalk.h>
#include <linux/sched/mm.h>

#include "../internal.h"
#include "ops-common.h"

/*
 * 学习入口：DAMON_VADDR 以进程虚拟地址为观察坐标。一个采样周期先为每个 region 选随机地址并清 young，
 * 下一周期再检查 PTE/PMD/HugeTLB 和 folio idle 状态，把“是否又被访问”反馈给 DAMON 核心。
 * 后续 scheme 可把观测结果转为 madvise、迁移或统计；所有 task/mm/PTE 指针都只在相应引用或锁窗口中有效。
 */

#ifdef CONFIG_DAMON_VADDR_KUNIT_TEST
/* KUnit 用 1 字节最小 region 放大边界情形；生产配置仍使用正常最小 region 粒度。 */
#undef DAMON_MIN_REGION_SZ
#define DAMON_MIN_REGION_SZ 1
#endif

/*
 * 't->pid' should be the pointer to the relevant 'struct pid' having reference
 * count.  Caller must put the returned task, unless it is NULL.
 */
static inline struct task_struct *damon_get_task_struct(struct damon_target *t)
{
	/*
	 * 业务背景：DAMON target 仅保存 pid 引用，采样时需临时解析为任务对象。
	 * 入参：t 持有有效 struct pid。出参/返回：成功返回带引用 task，失败 NULL。
	 * 注意事项：调用者必须 put_task_struct()；不能跨越任务退出边界缓存返回指针。
	 */
	return get_pid_task(t->pid, PIDTYPE_PID);
}

/*
 * Get the mm_struct of the given target
 *
 * Caller _must_ put the mm_struct after use, unless it is NULL.
 *
 * Returns the mm_struct of the target on success, NULL on failure
 */
static struct mm_struct *damon_get_mm(struct damon_target *t)
{
	/*
	 * 业务背景：虚拟地址监测必须获取目标进程 mm 的稳定引用，而非借用 task->mm。
	 * 入参：t。出参/返回：成功返回带引用 mm，目标不存在或无用户 mm 时 NULL。
	 * 注意事项：task 引用仅覆盖 get_task_mm 调用；调用者必须 mmput 返回值。
	 */
	struct task_struct *task;
	struct mm_struct *mm;

	task = damon_get_task_struct(t);
	/* PID 可能已退出；先判断 task 才能避免把 NULL 当作有效进程并在下一步解引用。 */
	if (!task)
		return NULL;

	mm = get_task_mm(task);
	/* get_task_mm 增加 mm 引用，使 task 随后 put 后地址空间仍可被本次采样安全使用。 */
	put_task_struct(task);
	/* 不及时归还 task 会阻止其最终释放；mm 的独立引用才是后续 VMA/pagewalk 的生命周期凭据。 */
	return mm;
}

static unsigned long sz_range(struct damon_addr_range *r)
{
	/* 业务背景：空洞和 region 比较以半开区间长度排序；入参 r，返回 end-start，调用者保证范围合法。 */
	return r->end - r->start;
}

/*
 * Find three regions separated by two biggest unmapped regions
 *
 * vma		the head vma of the target address space
 * regions	an array of three address ranges that results will be saved
 *
 * This function receives an address space and finds three regions in it which
 * separated by the two biggest unmapped regions in the space.  Please refer to
 * below comments of '__damon_va_init_regions()' function to know why this is
 * necessary.
 *
 * Returns 0 if success, or negative error code otherwise.
 */
static int __damon_va_three_regions(struct mm_struct *mm,
				       struct damon_addr_range regions[3])
{
	/*
	 * 业务背景：DAMON 把地址空间按两个最大未映射洞拆为三段，避免空洞主导采样 region。
	 * 入参：mm 和三个输出 range 槽。出参/返回：0 成功，缺少两个有效洞时 -EINVAL。
	 * 注意事项：VMA maple-tree 遍历受 RCU 读锁保护；输出按地址排序并按最小 region 大小对齐。
	 */
	struct damon_addr_range first_gap = {0}, second_gap = {0};
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma, *prev = NULL;
	unsigned long start;

	/*
	 * Find the two biggest gaps so that first_gap > second_gap > others.
	 * If this is too slow, it can be optimised to examine the maple
	 * tree gaps.
	 */
	rcu_read_lock();
	/*
	 * 为什么先取 RCU 读锁：VMA tree 可被 mmap/munmap 并发改写；无锁遍历可能读到已拆除 VMA。
	 * 这里仅复制端点快照，绝不把 vma 指针带出临界区；否则解锁后解引用会变成 use-after-free。
	 */
	for_each_vma(vmi, vma) {
		unsigned long gap;

		if (!prev) {
			/* 第一段映射确定整个地址空间有效观察区的低端，尚无前驱可计算空洞。 */
			start = vma->vm_start;
			goto next;
		}
		gap = vma->vm_start - prev->vm_end;
		/*
		 * 相邻半开 VMA 的端点差是未映射洞。保留两个最大洞是因为完整 VMA 列表可能很多，
		 * 逐映射建 region 会让采样和调整成本随 mmap 数暴涨；不排除特大洞又会把大量样本浪费在永远不会访问的地址。
		 */

		if (gap > sz_range(&first_gap)) {
		/* 新最大洞必须把旧最大下推：否则会丢掉第二大洞，最终三段 region 仍包含一个应剔除的大空洞。 */
			second_gap = first_gap;
			first_gap.start = prev->vm_end;
			first_gap.end = vma->vm_start;
		} else if (gap > sz_range(&second_gap)) {
			/* 只有未超过最大洞的候选才可能替换次大洞，避免丢失当前最大。 */
			second_gap.start = prev->vm_end;
			second_gap.end = vma->vm_start;
		}
next:
		prev = vma;
	}
	rcu_read_unlock();
	/* 解锁后只能使用范围值；继续使用 prev/vma 会与 munmap 竞态并触发悬空指针访问。 */

	if (!sz_range(&second_gap) || !sz_range(&first_gap))
		/* 缺少两个洞时强行构造三段会产生零长或重叠 region，破坏 DAMON region 的范围不变量。 */
		return -EINVAL;

	/* Sort the two biggest gaps by address */
	if (first_gap.start > second_gap.start)
		/* 最终输出必须按虚拟地址升序，即使“最大”按长度选出时顺序相反。 */
		swap(first_gap, second_gap);

	/* Store the result */
	regions[0].start = ALIGN(start, DAMON_MIN_REGION_SZ);
	/* 对齐避免后续 region 调整器反复制造小于最小粒度的边界碎片；碎片会放大元数据和采样开销。 */
	regions[0].end = ALIGN(first_gap.start, DAMON_MIN_REGION_SZ);
	regions[1].start = ALIGN(first_gap.end, DAMON_MIN_REGION_SZ);
	regions[1].end = ALIGN(second_gap.start, DAMON_MIN_REGION_SZ);
	regions[2].start = ALIGN(second_gap.end, DAMON_MIN_REGION_SZ);
	regions[2].end = ALIGN(prev->vm_end, DAMON_MIN_REGION_SZ);

	return 0;
}

/*
 * Get the three regions in the given target (task)
 *
 * Returns 0 on success, negative error code otherwise.
 */
static int damon_va_three_regions(struct damon_target *t,
				struct damon_addr_range regions[3])
{
	/*
	 * 业务背景：target 的地址布局只能在所持 mm 的 mmap 读锁下转换为 DAMON 三段初始范围。
	 * 入参：t 与三元素 regions 输出数组。出参/返回：0 或负 errno。
	 * 注意事项：damon_get_mm() 返回值必须 mmput；mmap 锁只覆盖 VMA 遍历，不跨越结果消费。
	 */
	struct mm_struct *mm;
	int rc;

	mm = damon_get_mm(t);
	/* 目标退出时不再有 mm；把它当 -EINVAL 返回可让上层跳过本 target，而非访问已回收地址空间。 */
	if (!mm)
		return -EINVAL;

	mmap_read_lock(mm);
	/* 三段计算读取整个 VMA 集合，必须用 mmap 读锁阻止 map/unmap 令两个最大洞来自不一致快照。 */
	rc = __damon_va_three_regions(mm, regions);
	/* 内层失败不会留下部分输出；只有完整三段可交给 DAMON region 管理器。 */
	mmap_read_unlock(mm);

	mmput(mm);
	/* 返回前归还 mm，否则每次周期更新都会泄漏一个地址空间引用并阻止进程 mm 回收。 */
	return rc;
}

/*
 * Initialize the monitoring target regions for the given target (task)
 *
 * t	the given target
 *
 * Because only a number of small portions of the entire address space
 * is actually mapped to the memory and accessed, monitoring the unmapped
 * regions is wasteful.  That said, because we can deal with small noises,
 * tracking every mapping is not strictly required but could even incur a high
 * overhead if the mapping frequently changes or the number of mappings is
 * high.  The adaptive regions adjustment mechanism will further help to deal
 * with the noise by simply identifying the unmapped areas as a region that
 * has no access.  Moreover, applying the real mappings that would have many
 * unmapped areas inside will make the adaptive mechanism quite complex.  That
 * said, too huge unmapped areas inside the monitoring target should be removed
 * to not take the time for the adaptive mechanism.
 *
 * For the reason, we convert the complex mappings to three distinct regions
 * that cover every mapped area of the address space.  Also the two gaps
 * between the three regions are the two biggest unmapped areas in the given
 * address space.  In detail, this function first identifies the start and the
 * end of the mappings and the two biggest unmapped areas of the address space.
 * Then, it constructs the three regions as below:
 *
 *     [mappings[0]->start, big_two_unmapped_areas[0]->start)
 *     [big_two_unmapped_areas[0]->end, big_two_unmapped_areas[1]->start)
 *     [big_two_unmapped_areas[1]->end, mappings[nr_mappings - 1]->end)
 *
 * As usual memory map of processes is as below, the gap between the heap and
 * the uppermost mmap()-ed region, and the gap between the lowermost mmap()-ed
 * region and the stack will be two biggest unmapped regions.  Because these
 * gaps are exceptionally huge areas in usual address space, excluding these
 * two biggest unmapped regions will be sufficient to make a trade-off.
 *
 *   <heap>
 *   <BIG UNMAPPED REGION 1>
 *   <uppermost mmap()-ed region>
 *   (other mmap()-ed regions and small unmapped regions)
 *   <lowermost mmap()-ed region>
 *   <BIG UNMAPPED REGION 2>
 *   <stack>
 */
static void __damon_va_init_regions(struct damon_ctx *ctx,
				     struct damon_target *t)
{
	/*
	 * 业务背景：未显式配置 region 的 target 采用三段近似，减少大空洞上的无效采样。
	 * 入参：ctx、target。出参/返回：无，失败仅记录 debug 并保留原 region 状态。
	 * 注意事项：damon_set_regions() 接收构造好的局部范围；不能在取得布局失败时写入部分结果。
	 */
	struct damon_target *ti;
	struct damon_addr_range regions[3];
	int tidx = 0;

	if (damon_va_three_regions(t, regions)) {
		/* 一个进程可能正退出或布局暂时不足三段；跳过它比清空既有 region 更安全，后者会让采样核心失去稳定对象。 */
		damon_for_each_target(ti, ctx) {
			if (ti == t)
				break;
			tidx++;
		}
		pr_debug("Failed to get three regions of %dth target\n", tidx);
		/* 仅 debug 记录避免瞬态退出把 DAMON 周期变成用户可见错误；下一轮可重新尝试。 */
		return;
	}

	damon_set_regions(t, regions, 3, DAMON_MIN_REGION_SZ);
	/* 只在三段均完整后原子替换 target region；逐段更新会让并发核心看到混合的旧/新布局。 */
}

/* Initialize '->regions_list' of every target (task) */
static void damon_va_init(struct damon_ctx *ctx)
{
	/* 业务背景：ctx 启动时只初始化用户尚未指定 region 的 target；入参 ctx，返回无，保留用户配置优先级。 */
	struct damon_target *t;

	damon_for_each_target(t, ctx) {
		/* target 相互独立，某一 PID 失败不可阻断其他进程的初始化，否则一个退出进程会拖垮整个 ctx。 */
		/* the user may set the target regions as they want */
		if (!damon_nr_regions(t))
			/* 用户已设 region 时尊重其精确意图；自动三段近似只填补“未配置”的默认情况。 */
			__damon_va_init_regions(ctx, t);
	}
}

/*
 * Update regions for current memory mappings
 */
static void damon_va_update(struct damon_ctx *ctx)
{
	/* 业务背景：地址布局会随 mmap 变化而刷新三段近似；入参 ctx，返回无，单 target 失败不影响其余目标。 */
	struct damon_addr_range three_regions[3];
	struct damon_target *t;

	damon_for_each_target(t, ctx) {
		/* 周期更新重新读取每个地址空间；不更新会让新 mmap 永远不被采样，已 unmapped 范围却持续浪费工作。 */
		if (damon_va_three_regions(t, three_regions))
			/* 暂时失败时保留上轮可用结果，避免用空集合抖动 region 数量和访问统计。 */
			continue;
		damon_set_regions(t, three_regions, 3, DAMON_MIN_REGION_SZ);
	}
}

static void damon_va_walk_page_range(struct mm_struct *mm, unsigned long start,
		unsigned long end, struct mm_walk_ops *ops, void *private)
{
	/*
	 * 业务背景：DAMON 常采样单地址，优先用 RCU VMA 锁避开整 mm mmap_lock 的竞争，失败再退化。
	 * 入参：mm、半开范围、pagewalk ops/private。出参/返回：无。
	 * 注意事项：仅范围完全位于同一非 PFNMAP VMA 才可验证 VMA 读锁；否则必须使用 mmap read lock walker。
	 */
	struct vm_area_struct *vma;

	vma = lock_vma_under_rcu(mm, start);
	/*
	 * 先尝试 VMA 读锁是为了避免每个采样点都抢整把 mmap_lock；采样频繁时后者会干扰 mmap/fault。
	 * lock_vma_under_rcu 失败并不代表地址无效，只代表无法安全获得局部稳定窗口，必须退化到 mmap 锁而非继续裸走页表。
	 */
	if (!vma)
		goto lock_mmap;

	if (end > vma->vm_end) {
		/* 跨 VMA 时单个 VMA 锁不再覆盖整个范围；若强行继续，后半段页表可被并发拆除。 */
		vma_end_read(vma);
		goto lock_mmap;
	}

	if (!(vma->vm_flags & VM_PFNMAP)) {
		/* PFNMAP 没有普通 struct folio，DAMON 不能把设备 PFN 当作可追踪用户页，否则 idle/迁移判断会失真。 */
		ops->walk_lock = PGWALK_VMA_RDLOCK_VERIFY;
		walk_page_range_vma(vma, start, end, ops, private);
	}

	vma_end_read(vma);
	/* VMA 锁必须在回调结束后立即归还；保留它会阻塞修改映射的写者并放大采样对业务线程的影响。 */
	return;

lock_mmap:
	/*
	 * 退化路径锁住整个 VMA tree：代价更高，但它覆盖跨 VMA 范围和 RCU 快路径失败的情况。
	 * 不退化就无法同时保证 walker 找到正确 VMA 与页表页在回调期间不被 unmap 释放。
	 */
	mmap_read_lock(mm);
	ops->walk_lock = PGWALK_RDLOCK;
	walk_page_range(mm, start, end, ops, private);
	mmap_read_unlock(mm);
}

static int damon_mkold_pmd_entry(pmd_t *pmd, unsigned long addr,
		unsigned long next, struct mm_walk *walk)
{
	/*
	 * 业务背景：一次 DAMON 采样先清除 PTE/PMD young 位，以后续检查是否被重新访问。
	 * 入参：当前 PMD、地址范围和 pagewalk 上下文。出参/返回：始终 0，错误页表项视为未采样。
	 * 注意事项：THP 与 PTE 走不同 PTL；取得 PTE 映射后必须 pte_unmap_unlock。
	 */
	pte_t *pte;
	spinlock_t *ptl;

	ptl = pmd_trans_huge_lock(pmd, walk->vma);
	/* 先尝试 THP 锁：把 THP 当普通 PTE 会在 split/merge 并发时读取不一致条目，进而清错 young 位。 */
	if (ptl) {
		pmd_t pmde = pmdp_get(pmd);

		if (pmd_present(pmde))
			damon_pmdp_mkold(pmd, walk->vma, addr);
		spin_unlock(ptl);
		return 0;
	}

	pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	/* 非 THP 才映射 PTE；map-lock 同时取得 PTL，省略锁会与 fault/unmap 并发修改条目。 */
	if (!pte)
		/* PTE 表可能刚被并发回收；把 map 失败当作本样本缺失比重试裸指针安全，下一周期会重新观察。 */
		return 0;
	if (!pte_present(ptep_get(pte)))
		/* 清 young 前复读 present，避免对已变成 swap/migration entry 的槽写入错误 PTE 状态。 */
		goto out;
	damon_ptep_mkold(pte, walk->vma, addr);
out:
	pte_unmap_unlock(pte, ptl);
	return 0;
}

#ifdef CONFIG_HUGETLB_PAGE
static void damon_hugetlb_mkold(pte_t *pte, struct mm_struct *mm,
				struct vm_area_struct *vma, unsigned long addr)
{
	/*
	 * 业务背景：HugeTLB 不经过普通 PMD/PTE 路径，DAMON 仍需清其 young 位建立采样基线。
	 * 入参：huge PTE、mm、VMA 和地址。出参/返回：无。
	 * 注意事项：临时 folio 引用覆盖 idle/young 标志更新；PTE 写回使用 huge 页大小，MMU notifier 结果也算访问。
	 */
	bool referenced = false;
	pte_t entry = huge_ptep_get(mm, addr, pte);
	struct folio *folio = pfn_folio(pte_pfn(entry));
	unsigned long psize = huge_page_size(hstate_vma(vma));

	folio_get(folio);
	/* Huge folio 的 PTE 锁不等于 folio 生命周期引用；没有 get，解锁后并发回收可让后续标志更新访问已释放内存。 */

	if (pte_young(entry)) {
		/* 先记录旧访问再清位；反过来会丢失本周期刚发现的访问事实。 */
		/* PTE young 是 CPU 硬件访问证据；不清除它，下一周期会把历史访问误算成新访问。 */
		referenced = true;
		entry = pte_mkold(entry);
		set_huge_pte_at(mm, addr, pte, entry, psize);
	}

	if (mmu_notifier_clear_young(mm, addr,
				     addr + huge_page_size(hstate_vma(vma))))
		referenced = true;
	/* 二级地址转换器也可能访问页；只看 CPU PTE 会漏掉设备访问，导致 DAMON 错误回收仍在用的页。 */

	if (referenced)
		/* 被任一访问源证明活跃才恢复 folio young；否则 reclaim 可能把刚用过的页当冷页。 */
		folio_set_young(folio);

	folio_set_idle(folio);
	/* 设置 idle 创建下一次采样的软件基线；若不设置，folio idle 状态无法补偿架构未暴露的 PTE young。 */
	folio_put(folio);
	/* 结果写入 private 后立即放引用；把引用带出回调会让每个采样周期额外钉住 HugeTLB 内存。 */
}

static int damon_mkold_hugetlb_entry(pte_t *pte, unsigned long hmask,
				     unsigned long addr, unsigned long end,
				     struct mm_walk *walk)
{
	/*
	 * 业务背景：pagewalk 在 HugeTLB VMA 锁窗口调用此回调以处理一个逻辑 huge entry。
	 * 入参：huge PTE、掩码、地址范围及 walk。出参/返回：0。
	 * 注意事项：huge_pte_lock 保护 entry 复读；非 present 项直接解锁，不可调用下层 mkold。
	 */
	struct hstate *h = hstate_vma(walk->vma);
	spinlock_t *ptl;
	pte_t entry;
	/* h/ptl/entry 必须在同一回调内使用；回调返回后 pagewalk 允许 VMA/页表变化，缓存它们会悬空。 */

	ptl = huge_pte_lock(h, walk->mm, pte);
	/* HugeTLB PTE 有专属锁规则；沿用普通 PTL 可能无法和 huge fault/拆映射同步。 */
	entry = huge_ptep_get(walk->mm, addr, pte);
	if (!pte_present(entry))
		/* 非 present huge entry 没有可归属 folio，若继续 pfn_folio 会把 swap/marker 错认内存页。 */
		goto out;

	damon_hugetlb_mkold(pte, walk->mm, walk->vma, addr);
	/* 清基线后立刻离开回调；继续在锁内做无关工作会拉长 HugeTLB fault 的串行等待。 */

out:
	spin_unlock(ptl);
	/* 无论 THP 分支是否筛选出 folio 都必须解锁；遗漏会阻塞后续 fault、split 或迁移。 */
	return 0;
}
#else
#define damon_mkold_hugetlb_entry NULL
#endif /* CONFIG_HUGETLB_PAGE */

static void damon_va_mkold(struct mm_struct *mm, unsigned long addr)
{
	/*
	 * 业务背景：对 region 随机采样地址建立“之后是否访问”的基线。
	 * 入参：带引用 mm、单一虚拟地址。出参/返回：无。
	 * 注意事项：只 walk [addr, addr+1)；ops 同时覆盖 PMD/hugetlb，锁策略由 damon_va_walk_page_range 选择。
	 */
	struct mm_walk_ops damon_mkold_ops = {
		/* 回调表只安装能够清基线的层；没有 PTE 回调是因为 PMD callback 已自行处理其下 PTE。 */
		.pmd_entry = damon_mkold_pmd_entry,
		.hugetlb_entry = damon_mkold_hugetlb_entry,
	};

	damon_va_walk_page_range(mm, addr, addr + 1, &damon_mkold_ops, NULL);
}

/*
 * Functions for the access checking of the regions
 */

static void __damon_va_prepare_access_check(struct mm_struct *mm,
					struct damon_region *r,
					struct damon_ctx *ctx)
{
	/*
	 * 业务背景：每个 region 在一个采样周期选取随机点并清旧访问状态。
	 * 入参：已获得 mm、region、ctx 随机源。出参/返回：无。
	 * 注意事项：sampling_addr 必须在 region 半开范围内；清 young 后才允许下一阶段解释为新访问。
	 */
	r->sampling_addr = damon_rand(ctx, r->ar.start, r->ar.end);
	/* 随机抽样避免每轮固定地址形成偏差；固定热点会把整个 region 错误概括为热或冷。 */

	damon_va_mkold(mm, r->sampling_addr);
}

static void damon_va_prepare_access_checks(struct damon_ctx *ctx)
{
	/*
	 * 业务背景：批量准备所有 target/region 的访问观测基线。
	 * 入参：ctx。出参/返回：无。
	 * 注意事项：每 target 只获取一次 mm 引用，所有 region 完成后 mmput；退出目标会被安全跳过。
	 */
	struct damon_target *t;
	struct mm_struct *mm;
	struct damon_region *r;

	damon_for_each_target(t, ctx) {
		/* 一次取得 mm 后遍历该 target 所有 region，避免每个 region 往返取/放引用带来开销和退出竞态窗口。 */
		mm = damon_get_mm(t);
		if (!mm)
			continue;
		damon_for_each_region(r, t)
			__damon_va_prepare_access_check(mm, r, ctx);
		mmput(mm);
	}
}

struct damon_young_walk_private {
	/* pagewalk 回调与外层 damon_va_young() 的短期通信对象，只在同步 walk 期间借用。 */
	/* size of the folio for the access checked virtual memory address */
	unsigned long *folio_sz;
	bool young;
};

static int damon_young_pmd_entry(pmd_t *pmd, unsigned long addr,
		unsigned long next, struct mm_walk *walk)
{
	/*
	 * 业务背景：检查采样地址是否在上次 mkold 后重新被 CPU 或二级地址转换器访问。
	 * 入参：PMD、地址、pagewalk private。出参/返回：0，并通过 private 写 young/folio 大小。
	 * 注意事项：THP/PTE 都需在相应 PTL 下复读；special/device 映射没有 normal folio，不可作为访问证据。
	 */
	pte_t *pte;
	pte_t ptent;
	spinlock_t *ptl;
	struct folio *folio;
	struct damon_young_walk_private *priv = walk->private;
	/* private 是外层栈对象，回调只写结果位和大小；若改为静态会让不同 target 的并发采样互相污染。 */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	ptl = pmd_trans_huge_lock(pmd, walk->vma);
	/* THP 分支先在锁下判定，防止其在检查期间被拆为 PTE 表而让 folio 大小与条目不匹配。 */
	if (ptl) {
		/* 进入 THP 分支即表示普通 PTE 表不应再碰；两条路径混用会 double-unlock 或错误 map PTE。 */
		pmd_t pmde = pmdp_get(pmd);

		if (!pmd_present(pmde))
			goto huge_out;
		folio = vm_normal_folio_pmd(walk->vma, addr, pmde);
		if (!folio)
			goto huge_out;
		if (pmd_young(pmde) || !folio_test_idle(folio) ||
					mmu_notifier_test_young(walk->mm,
						addr))
			priv->young = true;
	/* 三个信号取或：任何一个成立都表明访问发生；取交集会漏报，导致错误的冷页建议或迁移。 */
		*priv->folio_sz = HPAGE_PMD_SIZE;
huge_out:
		spin_unlock(ptl);
		return 0;
	}
#endif	/* CONFIG_TRANSPARENT_HUGEPAGE */

	pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	/* PTE 路径把映射和锁成对取得；无锁读取可能把刚失效映射误报为访问。 */
	if (!pte)
		return 0;
	ptent = ptep_get(pte);
	if (!pte_present(ptent))
		/* non-present PTE 可能编码 swap/migration；它不是当前可采样的普通 folio。 */
		goto out;
	folio = vm_normal_folio(walk->vma, addr, ptent);
	if (!folio)
		/* special/device PFN 没有普通 LRU/idle 语义，硬把它纳入会污染 DAMON 的冷热统计。 */
		goto out;
	if (pte_young(ptent) || !folio_test_idle(folio) ||
			mmu_notifier_test_young(walk->mm, addr))
		priv->young = true;
	/* 这里同样把硬件、软件 idle 和 notifier 三种观察合并，避免架构/设备差异改变 DAMON 结论。 */
	*priv->folio_sz = folio_size(folio);
out:
	pte_unmap_unlock(pte, ptl);
	return 0;
}

#ifdef CONFIG_HUGETLB_PAGE
static int damon_young_hugetlb_entry(pte_t *pte, unsigned long hmask,
				     unsigned long addr, unsigned long end,
				     struct mm_walk *walk)
{
	/*
	 * 业务背景：HugeTLB 采样以该巨大 folio 的 young、idle 和 notifier 状态共同判断访问。
	 * 入参：huge PTE、地址范围和 walk private。出参/返回：0，并写 private 结果。
	 * 注意事项：folio_get 防止锁外标志读取期间释放；无 present entry 只解锁并报告未访问。
	 */
	struct damon_young_walk_private *priv = walk->private;
	struct hstate *h = hstate_vma(walk->vma);
	struct folio *folio;
	spinlock_t *ptl;
	pte_t entry;
	/* HugeTLB 的 folio/锁只在本回调锁窗口内可信；返回后不能向外发布这些借用对象。 */
	/* HugeTLB 需要从 VMA 得 hstate 才能取得正确 PTL 和页大小；把它当普通 PTE 会错锁不同 huge 页池。 */

	ptl = huge_pte_lock(h, walk->mm, pte);
	/* HugeTLB 回调要自行取其 PTL；VMA 锁只保证 PTE 指针寿命，不替代条目内容互斥。 */
	entry = huge_ptep_get(walk->mm, addr, pte);
	if (!pte_present(entry))
		/* huge entry 非 present 时无采样对象，继续转换 PFN 会把编码条目伪装为真实 folio。 */
		goto out;

	folio = pfn_folio(pte_pfn(entry));
	/* present huge entry 才可从 PFN 得 folio；该转换不持引用，后面立刻 folio_get 以跨越标志检查窗口。 */
	folio_get(folio);

	if (pte_young(entry) || !folio_test_idle(folio) ||
	    mmu_notifier_test_young(walk->mm, addr))
		/* 任一访问证据成立就置 young；若只信 PTE，会漏掉设备/IOMMU 访问而错迁冷页。 */
		priv->young = true;
	*priv->folio_sz = huge_page_size(h);
	/* 返回真实 huge 大小让外层缓存按整个 folio 对齐；误用 PAGE_SIZE 会把同一大页重复 pagewalk。 */

	folio_put(folio);
	/* huge 页结果已复制到 private，继续持引用没有保护价值，反而会延迟其释放或迁移。 */

out:
	spin_unlock(ptl);
	return 0;
}
#else
#define damon_young_hugetlb_entry NULL
#endif /* CONFIG_HUGETLB_PAGE */

static bool damon_va_young(struct mm_struct *mm, unsigned long addr,
		unsigned long *folio_sz)
{
	/*
	 * 业务背景：统一 PTE、THP 与 hugetlb 的 young/idle/notifier 检查供 region 统计消费。
	 * 入参：mm、采样地址、folio_sz 输出槽。出参/返回：是否观测到访问。
	 * 注意事项：folio_sz 供相邻 region 结果复用决定对齐粒度；pagewalk 回调仅在锁窗口内使用条目指针。
	 */
	struct damon_young_walk_private arg = {
		/* private 用栈对象承接同步 walk 结果；不能把它挂到 ctx，因为并行 target 会互相覆盖。 */
		.folio_sz = folio_sz,
		.young = false,
	};

	struct mm_walk_ops damon_young_ops = {
		.pmd_entry = damon_young_pmd_entry,
		.hugetlb_entry = damon_young_hugetlb_entry,
	};

	damon_va_walk_page_range(mm, addr, addr + 1, &damon_young_ops, &arg);
	/* walker 返回后 arg 是值拷贝结果，不能保留任何回调中获得的 PTE/folio 指针。 */
	return arg.young;
}

/*
 * Check whether the region was accessed after the last preparation
 *
 * mm	'mm_struct' for the given virtual address space
 * r	the region to be checked
 */
static void __damon_va_check_access(struct mm_struct *mm,
				struct damon_region *r, bool same_target,
				struct damon_attrs *attrs)
{
	/*
	 * 业务背景：把单个 region 的 sampling_addr 转换为一次“自上次清零后是否访问”的统计样本。
	 * 入参：可为 NULL 的 mm、region、是否同 target、attrs。出参/返回：无，更新 region access rate。
	 * 注意事项：同一 folio 中相邻 region 复用静态结果；NULL mm 代表 target 已退出，必须记未访问而非解引用。
	 */
	static unsigned long last_addr;
	static unsigned long last_folio_sz = PAGE_SIZE;
	static bool last_accessed;

	if (!mm) {
		/* 进程已无用户地址空间时不能沿用静态缓存；否则上一 target 的结果会误计给退出 target 的 region。 */
		damon_update_region_access_rate(r, false, attrs);
		return;
	}

	/* If the region is in the last checked page, reuse the result */
	if (same_target && (ALIGN_DOWN(last_addr, last_folio_sz) ==
				ALIGN_DOWN(r->sampling_addr, last_folio_sz))) {
		/* 只有两地址落在同一 folio 才共享一次 pagewalk；忽略 folio 边界会把相邻冷页误标为热。 */
		damon_update_region_access_rate(r, last_accessed, attrs);
		return;
	}

	last_accessed = damon_va_young(mm, r->sampling_addr, &last_folio_sz);
	/* 缓存本次结果和 folio 大小，使下一相邻 region 能判断复用是否安全，而非猜测 PAGE_SIZE。 */
	damon_update_region_access_rate(r, last_accessed, attrs);

	last_addr = r->sampling_addr;
	/* 地址必须随结果一起更新；只更新 bool 会让下一轮拿旧地址比较并错误命中缓存。 */
}

static unsigned int damon_va_check_accesses(struct damon_ctx *ctx)
{
	/*
	 * 业务背景：一个采样周期批量检查所有 target 的 region，并返回最大访问计数供核心自适应调整。
	 * 入参：ctx。出参/返回：所有 region 中最大 nr_accesses。
	 * 注意事项：每 target 只借一次 mm；同 target 的 region 才允许复用上一 folio 结果，跨 target 禁止复用。
	 */
	struct damon_target *t;
	struct mm_struct *mm;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;
	bool same_target;

	damon_for_each_target(t, ctx) {
		/* 每个 target 的 mm 独立，进入新 target 前显式重置 same_target 阻断跨进程缓存复用。 */
		mm = damon_get_mm(t);
		same_target = false;
		damon_for_each_region(r, t) {
			__damon_va_check_access(mm, r, same_target,
					&ctx->attrs);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
			same_target = true;
		}
		/* 该 target 的所有 region 检查完才 mmput；提前 put 会让循环内 pagewalk 使用悬空地址空间。 */
		if (mm)
			mmput(mm);
	}

	return max_nr_accesses;
}

static bool damos_va_filter_young_match(struct damos_filter *filter,
		struct folio *folio, struct vm_area_struct *vma,
		unsigned long addr, pte_t *ptep, pmd_t *pmdp)
{
	/*
	 * 业务背景：vaddr walk 已直接持有页表项，可避免 young filter 重新做 rmap 查找。
	 * 入参：filter、folio、VMA、地址及互斥的 PTE/PMD 指针。出参/返回：是否符合 filter。
	 * 注意事项：检测到 young 后立即 mkold 建立下一次比较基线；页表指针仅在 walker 锁窗口有效。
	 */
	bool young = false;

	if (ptep)
		young = pte_young(ptep_get(ptep));
	else if (pmdp)
		young = pmd_young(pmdp_get(pmdp));

	young = young || !folio_test_idle(folio) ||
		mmu_notifier_test_young(vma->vm_mm, addr);

	if (young && ptep)
		/* 发现访问后立即清 PTE young，确保下一轮 filter 询问的是“之后是否又访问”而非重复历史。 */
		damon_ptep_mkold(ptep, vma, addr);
	else if (young && pmdp)
		/* THP 只有 PMD 项，错误地当 PTE 清理会遗漏其硬件访问位并让过滤永远匹配。 */
		damon_pmdp_mkold(pmdp, vma, addr);

	return young == filter->matching;
}

static bool damos_va_filter_out(struct damos *scheme, struct folio *folio,
		struct vm_area_struct *vma, unsigned long addr,
		pte_t *ptep, pmd_t *pmdp)
{
	/*
	 * 业务背景：scheme 操作前先按 vaddr 语义过滤 folio，young filter 可复用当前页表而非 rmap。
	 * 入参：scheme、folio、VMA、地址和当前 PTE/PMD。出参/返回：是否应跳过该 folio。
	 * 注意事项：core_filters_allowed 时交由通用核心；默认拒绝策略只在没有 filter 匹配时生效。
	 */
	struct damos_filter *filter;
	bool matched;

	if (scheme->core_filters_allowed)
		/* 核心已声明可自行处理 filter 时这里不得重复筛选，否则 allow/reject 会被应用两次。 */
		return false;

	damos_for_each_ops_filter(filter, scheme) {
		/* filter 按配置顺序短路：先匹配的拒绝规则应立即生效，否则后续 allow 会错误覆盖策略。 */
		/*
		 * damos_folio_filter_match checks the young filter by doing an
		 * rmap on the folio to find its page table. However, being the
		 * vaddr scheme, we have direct access to the page tables, so
		 * use that instead.
		 */
		if (filter->type == DAMOS_FILTER_TYPE_YOUNG)
			/* 直接利用当前页表避免 rmap 反查；后者既更贵又可能在映射变化时得到不同观察点。 */
			matched = damos_va_filter_young_match(filter, folio,
				vma, addr, ptep, pmdp);
		else
			matched = damos_folio_filter_match(filter, folio);

		if (matched)
			/* 匹配项用 allow 决定是否保留；继续扫描会破坏“第一个匹配 filter 决定结果”的配置语义。 */
			return !filter->allow;
	}
	/* 没有规则匹配时才使用默认策略；把默认提前应用会让显式 allow 规则永远没有机会生效。 */
	return scheme->ops_filters_default_reject;
}

struct damos_va_migrate_private {
	/* migration_lists 的数组由外层分配和最终 kfree；回调只向已选中的目的链追加隔离 folio。 */
	struct list_head *migration_lists;
	struct damos *scheme;
};

/*
 * Place the given folio in the migration_list corresponding to where the folio
 * should be migrated.
 *
 * The algorithm used here is similar to weighted_interleave_nid()
 */
static void damos_va_migrate_dests_add(struct folio *folio,
		struct vm_area_struct *vma, unsigned long addr,
		struct damos_migrate_dests *dests,
		struct list_head *migration_lists)
{
	/*
	 * 业务背景：DAMOS migrate 按虚拟页索引和权重把 folio 分发到迁移目的节点队列。
	 * 入参：folio、VMA/地址、dests 及按目的节点排列的迁移链。出参/返回：无。
	 * 注意事项：总权重为零或目标节点已一致时不隔离；folio_isolate_lru 成功后才可借用其 lru 字段挂队。
	 */
	pgoff_t ilx;
	int order;
	unsigned int target;
	unsigned int weight_total = 0;
	int i;
	/* i 最终既标识选中目的链也标识目标 node；两者必须同一索引，否则会把页挂到 A 链却迁往 B 节点。 */

	/*
	 * If dests is empty, there is only one migration list corresponding
	 * to s->target_nid.
	 */
	if (!dests->nr_dests) {
		/* 空目的表仍需支持旧的单 target_nid 配置；直接访问 node_id_arr[0] 会越界。 */
		i = 0;
		goto isolate;
	}

	order = folio_order(folio);
	/* 用 folio 阶数把 VMA 页偏移折算到大页单位；忽略 order 会让 THP 的权重分布偏斜。 */
	ilx = vma->vm_pgoff >> order;
	ilx += (addr - vma->vm_start) >> (PAGE_SHIFT + order);

	for (i = 0; i < dests->nr_dests; i++)
		/* 累加前不假定权重规范；异常全零值必须在取模前发现，不能让配置错误变成内核异常。 */
		weight_total += dests->weight_arr[i];

	/* If the total weights are somehow 0, don't migrate at all */
	if (!weight_total)
		/* 全零权重没有可定义的目的节点；继续取模会除零或随机选节点，必须放弃本页迁移。 */
		return;

	target = ilx % weight_total;
	/* 以页索引而非地址低位分配，使同一 VMA 的连续 folio 按权重稳定分布，减少跨周期抖动。 */
	for (i = 0; i < dests->nr_dests; i++) {
		/* 逐桶减权重实现确定性加权轮转；随机选择会让同一 VMA 跨周期迁移来回抖动。 */
		if (target < dests->weight_arr[i])
			break;
		target -= dests->weight_arr[i];
	}

	/* If the folio is already in the right node, don't do anything */
	if (folio_nid(folio) == dests->node_id_arr[i])
		/* 已在目标节点就不隔离；无谓迁移会制造 NUMA 流量、TLB 失效和短暂不可访问窗口。 */
		return;

isolate:
	/* 只有此处获得 LRU 所有权后才能复用 folio->lru 链接；未 isolate 直接 list_add 会令 folio 同时属于两条链。 */
	if (!folio_isolate_lru(folio))
		/* isolate 失败通常表示 folio 被并发回收/迁移或不在 LRU；强行挂链会破坏 LRU 所有权。 */
		return;

	list_add(&folio->lru, &migration_lists[i]);
	/* isolate 后 lru 字段暂时转作迁移私有链；只能由 damon_migrate_pages 消费并恢复/释放它。 */
}

static int damos_va_migrate_pmd_entry(pmd_t *pmd, unsigned long addr,
		unsigned long next, struct mm_walk *walk)
{
	/*
	 * 业务背景：页表 walk 收集一个 PMD 范围中通过 filter 的 normal folio，后续统一迁移。
	 * 入参：PMD、地址范围、携带 scheme/list 的 walk。出参/返回：0，异常或特殊映射被跳过。
	 * 注意事项：THP 与 PTE 路径各持对应 PTL；PTE 连续同 folio 时以 folio_nr_pages 前进避免重复隔离。
	 */
	struct damos_va_migrate_private *priv = walk->private;
	struct list_head *migration_lists = priv->migration_lists;
	struct damos *s = priv->scheme;
	struct damos_migrate_dests *dests = &s->migrate_dests;
	struct folio *folio;
	spinlock_t *ptl;
	pte_t *start_pte, *pte, ptent;
	int nr;
	/* 这些变量均只服务一个 PMD 回调；PTE 起点单独保存以确保退出时解除正确的临时映射。 */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	ptl = pmd_trans_huge_lock(pmd, walk->vma);
	if (ptl) {
		/* THP 锁成功时禁止落入 PTE loop；否则会把 huge PMD 当下级表并触发错误解引用。 */
		pmd_t pmde = pmdp_get(pmd);

		/* THP 条目可在锁前后被撤销；锁下复读仍非 present 时只跳过，不能使用锁前快照。 */
		if (!pmd_present(pmde))
			goto huge_out;
		folio = vm_normal_folio_pmd(walk->vma, addr, pmde);
		if (!folio)
			goto huge_out;
		/* filter 拒绝后不隔离，避免策略明确排除的页仍被迁移并打破用户的 allow/reject 预期。 */
		if (damos_va_filter_out(s, folio, walk->vma, addr, NULL, pmd))
			goto huge_out;
		/* isolate 成功的 folio 已转交目的链；本回调不能再直接改其 LRU 状态，否则会与迁移器竞争。 */
		damos_va_migrate_dests_add(folio, walk->vma, addr, dests,
				migration_lists);
huge_out:
		spin_unlock(ptl);
		return 0;
	}
#endif	/* CONFIG_TRANSPARENT_HUGEPAGE */

	start_pte = pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	if (!pte)
		/* PMD 页表可能被并发撤销；跳过本次迁移比持残指针隔离错误 folio 更安全。 */
		return 0;

	for (; addr < next; pte += nr, addr += nr * PAGE_SIZE) {
		nr = 1;
		ptent = ptep_get(pte);

		/* 空洞、swap 与 migration entry 不代表常驻 folio；跳过可防止访问错误物理页。 */
		if (pte_none(ptent) || !pte_present(ptent))
			continue;
		folio = vm_normal_folio(walk->vma, addr, ptent);
		if (!folio)
			continue;
		if (damos_va_filter_out(s, folio, walk->vma, addr, pte, NULL))
			continue;
		damos_va_migrate_dests_add(folio, walk->vma, addr, dests,
				migration_lists);
		/* 一次跨过整个 folio 的所有 PTE，既避免重复隔离也防止同页被多次加入迁移链。 */
		nr = folio_nr_pages(folio);
	}
	pte_unmap_unlock(start_pte, ptl);
	return 0;
}

/*
 * Functions for the target validity check and cleanup
 */

static bool damon_va_target_valid(struct damon_target *t)
{
	/* 业务背景：target 生命周期随 pid 退出；入参 t，返回是否仍可取得 task。注意：临时 task 引用必须立即归还。 */
	struct task_struct *task;

	task = damon_get_task_struct(t);
	if (task) {
		put_task_struct(task);
		return true;
	}

	return false;
}

static void damon_va_cleanup_target(struct damon_target *t)
{
	/* 业务背景：DAMON 不再使用 target 时归还其 pid 所有权；入参 t，返回无；不得重复 put_pid。 */
	put_pid(t->pid);
}

#ifndef CONFIG_ADVISE_SYSCALLS
static unsigned long damos_madvise(struct damon_target *target,
		struct damon_region *r, int behavior)
{
	/*
	 * 业务背景：未启用 advise syscall 时 vaddr scheme 不能改变用户地址空间策略。
	 * 入参：target、region、madvise 行为。出参/返回：始终 0 表示未应用字节数。
	 * 注意事项：该配置桩必须保留统一调用契约，不能伪造已完成操作。
	 */
	return 0;
}
#else
static unsigned long damos_madvise(struct damon_target *target,
		struct damon_region *r, int behavior)
{
	/*
	 * 业务背景：DAMOS 的 WILLNEED/COLD/PAGEOUT/THP 类操作通过目标 mm 的 do_madvise 落地。
	 * 入参：target、region、行为。出参/返回：成功返回页对齐应用字节数，失败 0。
	 * 注意事项：获取 mm 后必须 mmput；起点与长度页对齐，部分内核错误不会向上暴露 errno。
	 */
	struct mm_struct *mm;
	unsigned long start = PAGE_ALIGN(r->ar.start);
	unsigned long len = PAGE_ALIGN(damon_sz_region(r));
	unsigned long applied;

	mm = damon_get_mm(target);
	/* madvise 必须作用在当前 target mm；目标退出时返回 0，而不是将 region 地址用于任何其他进程。 */
	if (!mm)
		return 0;

	applied = do_madvise(mm, start, len, behavior) ? 0 : len;
	/* do_madvise 的错误折叠为 0 字节，调用者据此不会把失败区域计入 scheme 已应用量。 */
	mmput(mm);

	return applied;
}
#endif	/* CONFIG_ADVISE_SYSCALLS */

static unsigned long damos_va_migrate(struct damon_target *target,
		struct damon_region *r, struct damos *s,
		unsigned long *sz_filter_passed)
{
	/*
	 * 业务背景：迁移 action 先在页表 walk 中隔离合格 folio，再按目的节点队列统一迁移。
	 * 入参：target、region、scheme 和 filter-passed 输出。出参/返回：实际迁移字节数。
	 * 注意事项：迁移链数组分配失败或目标 mm 消失均返回 0；隔离后 list 所有权交给 damon_migrate_pages。
	 */
	LIST_HEAD(folio_list);
	/* 迁移实际使用按节点数组链表；该局部声明保留原布局，不承载跨回调所有权。 */
	struct damos_va_migrate_private priv;
	struct mm_struct *mm;
	int nr_dests;
	int nid;
	bool use_target_nid;
	unsigned long applied = 0;
	/* applied 只累计迁移 helper 确认完成的页数；不能用已隔离数量，否则失败迁移会被误报为成功。 */
	struct damos_migrate_dests *dests = &s->migrate_dests;
	struct mm_walk_ops walk_ops = {
		.pmd_entry = damos_va_migrate_pmd_entry,
		.pte_entry = NULL,
	};
	/* 只注册 PMD 回调以在其中统一处理 THP/PTE；另设 PTE 回调会重复收集同一 folio。 */

	use_target_nid = dests->nr_dests == 0;
	/* 没有显式目的表时退化为 scheme 单一 target_nid；若混淆两种模式会按未初始化数组取节点。 */
	nr_dests = use_target_nid ? 1 : dests->nr_dests;
	priv.scheme = s;
	priv.migration_lists = kmalloc_objs(*priv.migration_lists, nr_dests);
	/* 每个目的节点独立链表可避免迁移器在不同 nid 间重新分类；分配失败必须在隔离前退出，否则无人回收链项。 */
	if (!priv.migration_lists)
		return 0;

	for (int i = 0; i < nr_dests; i++)
		/* list_head 先初始化才能由 pagewalk 回调 append；遗漏任一项会把 folio->lru 写入垃圾指针。 */
		INIT_LIST_HEAD(&priv.migration_lists[i]);


	mm = damon_get_mm(target);
	/* 先持 mm 再 walk：目标退出时直接释放空链数组，不能用过期 task/mm 继续查页表。 */
	if (!mm)
		goto free_lists;

	damon_va_walk_page_range(mm, r->ar.start, r->ar.end, &walk_ops, &priv);
	/* walk 返回后不再依赖页表指针，隔离成功 folio 已转移到私有链，可安全先 mmput。 */
	mmput(mm);

	for (int i = 0; i < nr_dests; i++) {
		/* 每条链只迁往一个 nid；混合目的会让 NUMA 策略和迁移失败回滚无法归因。 */
		nid = use_target_nid ? s->target_nid : dests->node_id_arr[i];
		applied += damon_migrate_pages(&priv.migration_lists[i], nid);
		/* 迁移 helper 消费链中隔离 folio 并负责失败处理；调用者不可在此后再次访问该链项。 */
		cond_resched();
		/* 大 region 可隔离大量页，主动让出 CPU 防止 DAMON 后台工作长期占用调度器。 */
	}

free_lists:
	/* 所有退出路径释放数组容器；各 list 的 folio 所有权已在 migrate helper 或隔离失败路径处理完。 */
	kfree(priv.migration_lists);
	return applied * PAGE_SIZE;
}

struct damos_va_stat_private {
	/* scheme 给 filter 判定，sz_filter_passed 是外层累计器；两者只在同步 pagewalk 期间有效。 */
	struct damos *scheme;
	unsigned long *sz_filter_passed;
};

static inline bool damos_va_invalid_folio(struct folio *folio,
		struct damos *s)
{
	/* 业务背景：统计同一大 folio 的多个 PTE 时去重；入参 folio/scheme，返回是否为空或已在本轮应用。 */
	return !folio || folio == s->last_applied;
}

static int damos_va_stat_pmd_entry(pmd_t *pmd, unsigned long addr,
		unsigned long next, struct mm_walk *walk)
{
	/*
	 * 业务背景：在方案动作前统计过滤后可操作字节数，供 DAMOS 计费和配额判断。
	 * 入参：PMD、范围、含 scheme/计数槽的 walk。出参/返回：0。
	 * 注意事项：THP/PTE 均需 PTL；last_applied 防止同 folio 跨 PTE 重复累计，filter 会消费 young 状态。
	 */
	struct damos_va_stat_private *priv = walk->private;
	struct damos *s = priv->scheme;
	unsigned long *sz_filter_passed = priv->sz_filter_passed;
	struct vm_area_struct *vma = walk->vma;
	struct folio *folio;
	spinlock_t *ptl;
	pte_t *start_pte, *pte, ptent;
	int nr;

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	ptl = pmd_trans_huge_lock(pmd, vma);
	/* 统计也必须持 THP 锁：若 split 与统计并发，folio_size 可能与 PMD 指向的实际对象不一致，额度会失真。 */
	if (ptl) {
		pmd_t pmde = pmdp_get(pmd);

		/* 条目已撤销时不统计；把 non-present 当零长 folio 会掩盖地址空间变化并污染配额。 */
		if (!pmd_present(pmde))
			goto huge_unlock;

		folio = vm_normal_folio_pmd(vma, addr, pmde);

		/* NULL/same folio 不进入 filter：NULL 没有可读属性，同一 large folio 重复过滤会反复清 young。 */
		if (damos_va_invalid_folio(folio, s))
			goto huge_unlock;

		/* 仅 allow 的 folio 加入额度；把 reject 也累加会让 scheme 认为可处理内存比实际更多。 */
		if (!damos_va_filter_out(s, folio, vma, addr, NULL, pmd))
			*sz_filter_passed += folio_size(folio);
		s->last_applied = folio;

huge_unlock:
		spin_unlock(ptl);
		return 0;
	}
#endif
	start_pte = pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	/* PTE 映射起点需单独保存以便结束时正确 unmap；循环递增 pte 后直接 unmap 会释放错误地址。 */
	if (!start_pte)
		/* 页表撤销导致 map 失败时安全跳过该 PMD，不能以残留指针继续统计。 */
		return 0;

	for (; addr < next; pte += nr, addr += nr * PAGE_SIZE) {
		nr = 1;
		ptent = ptep_get(pte);

		if (pte_none(ptent) || !pte_present(ptent))
			continue;

		folio = vm_normal_folio(vma, addr, ptent);

		if (damos_va_invalid_folio(folio, s))
			/* 一个 large folio 可映射多个连续 PTE；不去重会多次累计同一物理内存并超额消耗 scheme 配额。 */
			continue;

		if (!damos_va_filter_out(s, folio, vma, addr, pte, NULL))
			*sz_filter_passed += folio_size(folio);
		nr = folio_nr_pages(folio);
		s->last_applied = folio;
	}
	pte_unmap_unlock(start_pte, ptl);
	/* 统计完成统一解除 PTE 映射和锁；缺失这一步会泄漏临时映射并阻塞同表的页故障。 */
	return 0;
}

static unsigned long damos_va_stat(struct damon_target *target,
		struct damon_region *r, struct damos *s,
		unsigned long *sz_filter_passed)
{
	/*
	 * 业务背景：有 ops filter 的 scheme 在动作前需从目标 VMA 精确统计通过过滤的页量。
	 * 入参：target、region、scheme 和累计输出。出参/返回：当前实现始终 0。
	 * 注意事项：无 filter 不需要 walk；mm 获取失败静默得到 0，调用者不可把它解释为全部匹配。
	 */
	struct damos_va_stat_private priv;
	struct mm_struct *mm;
	struct mm_walk_ops walk_ops = {
		.pmd_entry = damos_va_stat_pmd_entry,
	};

	priv.scheme = s;
	priv.sz_filter_passed = sz_filter_passed;

	if (!damos_ops_has_filter(s))
		/* 没有 filter 时无需统计“通过量”；把 0 视为零匹配会误导配额层，语义是该指标不适用。 */
		return 0;

	mm = damon_get_mm(target);
	/* target 退出时统计只能缺省为 0；不能回退读取旧 mm，否则会把已复用地址空间计给错误进程。 */
	if (!mm)
		return 0;

	damon_va_walk_page_range(mm, r->ar.start, r->ar.end, &walk_ops, &priv);
	/* walker 只累加 filter 允许的 folio 字节；范围中的空洞、special 页和拒绝项不应计入可操作额度。 */
	mmput(mm);
	return 0;
}

static unsigned long damon_va_apply_scheme(struct damon_ctx *ctx,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme, unsigned long *sz_filter_passed)
{
	/*
	 * 业务背景：DAMON 先观察 region，再依 scheme 动作要求内核预读、降温、换出、THP 建议或 NUMA 迁移。
	 * 入参：ctx、target、region、scheme 和 filter 统计输出。出参/返回：动作实际影响的字节数。
	 * 注意事项：migrate/stat 有独立 pagewalk 路径；未知或暂不支持动作必须返回 0，不能假装已经处理。
	 */
	int madv_action;

	switch (scheme->action) {
	/* action 映射保持在一处，避免调用者各自猜 MADV 常量造成用户可见策略和 DAMOS 名称不一致。 */
	case DAMOS_WILLNEED:
		/* WILLNEED 交给内核预读；若直接标访问会只改统计，不会让缺页路径提前准备内容。 */
		madv_action = MADV_WILLNEED;
		break;
	case DAMOS_COLD:
		/* COLD 降低回收保护但不立即丢页，适合“近期不再使用”而非必须同步释放的语义。 */
		madv_action = MADV_COLD;
		break;
	case DAMOS_PAGEOUT:
		/* PAGEOUT 才请求实际回收/写出；把它映射为 COLD 会让用户误以为内存已被腾出。 */
		madv_action = MADV_PAGEOUT;
		break;
	case DAMOS_HUGEPAGE:
		/* HUGEPAGE 只给内核合并提示，成功与否依赖后续 khugepaged/内存条件，返回值不能保证已合并。 */
		madv_action = MADV_HUGEPAGE;
		break;
	case DAMOS_NOHUGEPAGE:
		/* NOHUGEPAGE 阻止后续 THP 合并；不等于立刻拆分既有 huge page，避免承诺超出 madvise 语义。 */
		madv_action = MADV_NOHUGEPAGE;
		break;
	case DAMOS_COLLAPSE:
		/* COLLAPSE 请求同步合并，失败会由 do_madvise 返回；与普通 HUGEPAGE 的异步提示不同。 */
		madv_action = MADV_COLLAPSE;
		break;
	case DAMOS_MIGRATE_HOT:
	case DAMOS_MIGRATE_COLD:
		/* 迁移绕过 madvise，因为它需要隔离 LRU folio 和按节点批处理；仅给 MADV 常量会没有物理迁移。 */
		return damos_va_migrate(t, r, scheme, sz_filter_passed);
	case DAMOS_STAT:
		/* STAT 只产生统计而非修改页；若落入 madvise 会产生不应有的用户地址空间副作用。 */
		return damos_va_stat(t, r, scheme, sz_filter_passed);
	default:
		/*
		 * DAMOS actions that are not yet supported by 'vaddr'.
		 */
		/* 0 表示本 vaddr backend 不支持该 action，不是“region 已成功处理”；核心需据此保留后续决策。 */
		return 0;
	}

	return damos_madvise(t, r, madv_action);
}

static int damon_va_scheme_score(struct damon_ctx *context,
		struct damon_region *r, struct damos *scheme)
{
	/*
	 * 业务背景：多个 scheme 竞争配额时需要把 region 的冷热观测转为优先级分数。
	 * 入参：context、region、scheme。出参/返回：越符合动作目的的分数越高。
	 * 注意事项：仅 PAGEOUT 与 migrate 动作有专用冷热评分；其他动作返回最大分数以维持既有调度语义。
	 */

	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		/* pageout 按冷度排优先级，反用 hot 分数会优先回收近期访问页并恶化抖动。 */
		return damon_cold_score(context, r, scheme);
	case DAMOS_MIGRATE_HOT:
		/* 热迁移选 hot 分数以优先靠近高性能节点；冷迁移则反向选长期低访问页。 */
		return damon_hot_score(context, r, scheme);
	case DAMOS_MIGRATE_COLD:
		return damon_cold_score(context, r, scheme);
	default:
		break;
	}

	/* 其他动作无需冷热竞争，保持最大分数避免因无专用模型被意外饿死。 */
	return DAMOS_MAX_SCORE;
}

static int __init damon_va_initcall(void)
{
	/*
	 * 业务背景：启动期向 DAMON 核心登记按进程虚拟地址采样的操作集，并派生固定范围变体。
	 * 入参：无。出参/返回：首个 register 失败 errno，或固定范围 ops 的注册结果。
	 * 注意事项：FVADDR 不自动初始化/更新 region，保留用户指定范围；ops 结构仅被核心登记借用前构造。
	 */
	struct damon_operations ops = {
		/* VADDR ops 把完整自动 region 生命周期交给本文件；漏填任何回调会让核心在相应周期阶段静默失去能力。 */
		.id = DAMON_OPS_VADDR,
		.init = damon_va_init,
		.update = damon_va_update,
		.prepare_access_checks = damon_va_prepare_access_checks,
		.check_accesses = damon_va_check_accesses,
		.target_valid = damon_va_target_valid,
		.cleanup_target = damon_va_cleanup_target,
		.apply_scheme = damon_va_apply_scheme,
		.get_scheme_score = damon_va_scheme_score,
	};
	/* 回调表完整描述一次 DAMON 周期的顺序；漏掉 cleanup 会泄漏 pid，漏掉 valid 会对退出任务继续采样。 */
	/* ops for fixed virtual address ranges */
	struct damon_operations ops_fvaddr = ops;
	int err;
	/* FVADDR 从 VADDR 复制公共采样/动作实现，仅取消自动布局，避免维护两份易漂移的回调表。 */

	/* Don't set the monitoring target regions for the entire mapping */
	ops_fvaddr.id = DAMON_OPS_FVADDR;
	/* 固定范围的 region 由用户提供；若仍调用 init/update，会把精确范围替换为三段近似。 */
	ops_fvaddr.init = NULL;
	ops_fvaddr.update = NULL;

	err = damon_register_ops(&ops);
	/* 先登记 VADDR；失败时不能继续登记派生变体，否则核心会看到缺少基础语义的半套后端。 */
	if (err)
		/* 把原 errno 返回给 initcall，启动日志才能区分注册失败而不是误认为模块已就绪。 */
		return err;
	/* 第二次注册结果决定固定范围模式是否可用；静态 ops 生命周期覆盖 DAMON 核心持有期。 */
	return damon_register_ops(&ops_fvaddr);
};

subsys_initcall(damon_va_initcall);

#include "tests/vaddr-kunit.h"
