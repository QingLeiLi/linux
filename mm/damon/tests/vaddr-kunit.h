/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Data Access Monitor Unit Tests
 *
 * Copyright 2019 Amazon.com, Inc. or its affiliates.  All rights reserved.
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/*
 * 本头文件被 mm/damon/vaddr.c 在实现之后直接包含，只在 CONFIG_DAMON_VADDR_KUNIT_TEST
 * 下编译。测试用人工 maple tree 验证“两大空洞切成三个监控区”以及映射变化时
 * damon_set_regions() 对 region 链表的复用、裁剪、删除和新建行为。
 */

#ifdef CONFIG_DAMON_VADDR_KUNIT_TEST

#ifndef _DAMON_VADDR_TEST_H
#define _DAMON_VADDR_TEST_H

#include <kunit/test.h>

/*
 * __link_vmas() - 把一组测试 VMA 按半开地址范围插入指定 maple tree。
 * 业务背景：three-regions 测试不创建真实进程 mm，只构造最小 mm_mt 供 VMA_ITERATOR 读取。
 * 入参：mt 是已初始化、由测试拥有的树；vmas 是 nr_vmas 个长期有效测试对象；nr_vmas
 * 是元素数，可为 0，成功后树仅借用这些 VMA 指针。
 * 出参/返回：全部插入返回 0；任一 mas_store_gfp() 失败返回 -ENOMEM，树可能已有前缀项。
 * 注意事项：内部获取 maple tree 锁，可用 GFP_KERNEL 分配并睡眠；失败不回滚已插入项。
 */
static int __link_vmas(struct maple_tree *mt, struct vm_area_struct *vmas,
			ssize_t nr_vmas)
{
	int i, ret = -ENOMEM;
	MA_STATE(mas, mt, 0, 0);

	/* 空输入无需取锁或改变树。 */
	if (!nr_vmas)
		return 0;

	/* 每个 VMA 用 [vm_start, vm_end-1] 的 maple 闭区间键保存。 */
	mas_lock(&mas);
	for (i = 0; i < nr_vmas; i++) {
		mas_set_range(&mas, vmas[i].vm_start, vmas[i].vm_end - 1);
		if (mas_store_gfp(&mas, &vmas[i], GFP_KERNEL))
			goto failed;
	}

	ret = 0;
failed:
	/* 正常和部分失败统一释放树锁；调用者只依赖返回码决定 skip。 */
	mas_unlock(&mas);
	return ret;
}

/*
 * Test __damon_va_three_regions() function
 *
 * In case of virtual memory address spaces monitoring, DAMON converts the
 * complex and dynamic memory mappings of each target task to three
 * discontiguous regions which cover every mapped areas.  However, the three
 * regions should not include the two biggest unmapped areas in the original
 * mapping, because the two biggest areas are normally the areas between 1)
 * heap and the mmap()-ed regions, and 2) the mmap()-ed regions and stack.
 * Because these two unmapped areas are very huge but obviously never accessed,
 * covering the region is just a waste.
 *
 * '__damon_va_three_regions() receives an address space of a process.  It
 * first identifies the start of mappings, end of mappings, and the two biggest
 * unmapped areas.  After that, based on the information, it constructs the
 * three regions and returns.  For more detail, refer to the comment of
 * 'damon_init_regions_of()' function definition in 'mm/damon.c' file.
 *
 * For example, suppose virtual address ranges of 10-20, 20-25, 200-210,
 * 210-220, 300-305, and 307-330 (Other comments represent this mappings in
 * more short form: 10-20-25, 200-210-220, 300-305, 307-330) of a process are
 * mapped.  To cover every mappings, the three regions should start with 10,
 * and end with 305.  The process also has three unmapped areas, 25-200,
 * 220-300, and 305-307.  Among those, 25-200 and 220-300 are the biggest two
 * unmapped areas, and thus it should be converted to three regions of 10-25,
 * 200-220, and 300-330.
 */
/*
 * 测试 __damon_va_three_regions()：DAMON 面对动态、复杂的进程虚拟映射，不逐一监控
 * 每个 VMA，而是用三个互不连续的区域覆盖所有映射，同时排除原地址空间中最大的两个
 * unmapped gap。通常这两处正是 heap 与 mmap 区、mmap 区与 stack 之间的巨大空洞，
 * 永不会访问，纳入采样只会浪费时间。
 *
 * 被测函数读取进程地址空间，找出首/尾映射和两个最大空洞，再据此构造三个区域。例子
 * 映射为 10-20-25、200-210-220、300-305、307-330；全部映射跨度从 10 到 330，空洞
 * 为 25-200、220-300、305-307。排除最大的前两者后，期望得到 10-25、200-220、
 * 300-330。更完整设计背景位于 vaddr.c 的 __damon_va_init_regions() 注释。
 * 修正说明：上方英文“three regions ... end with 305”与其后列出的 300-330 及实际断言
 * 不一致；当前实现以最后一个 VMA 的 vm_end=330 作为第三段终点，305 只是倒数第二个
 * VMA 的终点，并非全部映射的结束地址。
 *
 * 业务背景：此 case 验证 VMA maple tree 扫描、空洞排名和输出边界的端到端组合。
 * 入参：test 是 KUnit 借用上下文，用于 skip 与 EXPECT 记录。
 * 出参/返回：无直接返回值；成功记录六个边界断言，建树失败把 case 标为 skipped。
 * 注意事项：static mm/vmas 在 case 生命周期内有效；测试不销毁树节点，KUnit 用例串行使用。
 */
static void damon_test_three_regions_in_vmas(struct kunit *test)
{
	static struct mm_struct mm;
	struct damon_addr_range regions[3] = {0};
	/* 10-20-25, 200-210-220, 300-305, 307-330 */
	/* 六个 VMA 中相邻端点仍是连续映射，只有三处真正 gap。 */
	static struct vm_area_struct vmas[] = {
		(struct vm_area_struct) {.vm_start = 10, .vm_end = 20},
		(struct vm_area_struct) {.vm_start = 20, .vm_end = 25},
		(struct vm_area_struct) {.vm_start = 200, .vm_end = 210},
		(struct vm_area_struct) {.vm_start = 210, .vm_end = 220},
		(struct vm_area_struct) {.vm_start = 300, .vm_end = 305},
		(struct vm_area_struct) {.vm_start = 307, .vm_end = 330},
	};

	/* 初始化与真实 mm 相同的 range/RCU maple tree；分配失败则本 case 无法给出结论。 */
	mt_init_flags(&mm.mm_mt, MT_FLAGS_ALLOC_RANGE | MT_FLAGS_USE_RCU);
	if (__link_vmas(&mm.mm_mt, vmas, ARRAY_SIZE(vmas)))
		kunit_skip(test, "Failed to create VMA tree");

	/* 被测 helper 在当前构造中应成功；随后逐个验证三组半开边界。 */
	__damon_va_three_regions(&mm, regions);

	KUNIT_EXPECT_EQ(test, 10ul, regions[0].start);
	KUNIT_EXPECT_EQ(test, 25ul, regions[0].end);
	KUNIT_EXPECT_EQ(test, 200ul, regions[1].start);
	KUNIT_EXPECT_EQ(test, 220ul, regions[1].end);
	KUNIT_EXPECT_EQ(test, 300ul, regions[2].start);
	KUNIT_EXPECT_EQ(test, 330ul, regions[2].end);
}

/*
 * __nth_region_of() - 按链表顺序借用目标的第 idx 个 DAMON region。
 * 业务背景：apply-three-regions 测试需把被测链表逐项与 expected 端点数组对应。
 * 入参：t 是测试持有的 target；idx 是从 0 开始的索引，负值/越界均不会命中。
 * 出参/返回：命中返回借用 region 指针，越界返回 NULL；不增加引用、不修改链表。
 * 注意事项：测试上下文无并发，返回指针只在 damon_destroy_target() 前有效，不睡眠。
 */
static struct damon_region *__nth_region_of(struct damon_target *t, int idx)
{
	struct damon_region *r;
	unsigned int i = 0;

	/* i 在访问每个链表节点后递增，首次相等即返回当前借用指针。 */
	damon_for_each_region(r, t) {
		if (i++ == idx)
			return r;
	}

	return NULL;
}

/*
 * Test 'damon_set_regions()'
 *
 * test			kunit object
 * regions		an array containing start/end addresses of current
 *			monitoring target regions
 * nr_regions		the number of the addresses in 'regions'
 * three_regions	The three regions that need to be applied now
 * expected		start/end addresses of monitoring target regions that
 *			'three_regions' are applied
 * nr_expected		the number of addresses in 'expected'
 *
 * The memory mapping of the target processes changes dynamically.  To follow
 * the change, DAMON periodically reads the mappings, simplifies it to the
 * three regions, and updates the monitoring target regions to fit in the three
 * regions.  The update of current target regions is the role of
 * 'damon_set_regions()'.
 *
 * This test passes the given target regions and the new three regions that
 * need to be applied to the function and check whether it updates the regions
 * as expected.
 */
/*
 * 测试 damon_set_regions()。
 * 参数 test 是 KUnit 上下文；regions 是当前监控区 start/end 交替数组，nr_regions 是其
 * 地址元素数；three_regions 是现在要应用的三个范围；expected/nr_expected 是应用后的
 * start/end 交替期望数组及元素数。
 *
 * 目标进程映射会动态变化。DAMON 周期性重读映射、简化为三个大区，再由
 * damon_set_regions() 调整当前 target regions 以跟随变化。本 helper 先用 regions 建立
 * 初态，再应用 three_regions，最后逐项核对边界。
 *
 * 业务背景：四个场景 case 共用此 fixture/ownership/断言流程，覆盖轻微边界调整到整区替换。
 * 入参：所有数组均为只借用；两个 nr_* 都是地址元素数且应为偶数；three_regions 固定 3 项。
 * 出参/返回：无直接返回值；断言写入 test。任一分配或 set 失败会释放已取得对象后 skip。
 * 注意事项：可用 GFP_KERNEL 睡眠；target/ranges 的 ownership 在每条失败及成功路径闭环，
 * __nth_region_of() 按测试期望应非 NULL，case 数据必须保证 expected 数量正确。
 */
static void damon_do_test_apply_three_regions(struct kunit *test,
				unsigned long *regions, int nr_regions,
				struct damon_addr_range *three_regions,
				unsigned long *expected, int nr_expected)
{
	struct damon_target *t;
	struct damon_addr_range *ranges;
	struct damon_region *r;
	int i;

	/* 阶段 1：创建独立 target；失败没有 fixture 可测，直接 skip。 */
	t = damon_new_target();
	if (!t)
		kunit_skip(test, "target alloc fail");

	/* 把交替端点转换为 damon_addr_range 数组；失败先销毁 target。 */
	ranges = kmalloc_array(nr_regions / 2, sizeof(*ranges), GFP_KERNEL);
	if (!ranges) {
		damon_destroy_target(t, NULL);
		kunit_skip(test, "ranges alloc fail");
	}
	/* 每两个 unsigned long 组成一个半开监控范围。 */
	for (i = 0; i < nr_regions / 2; i++) {
		ranges[i].start = regions[i * 2];
		ranges[i].end = regions[i * 2 + 1];
	}
	/* 第一次 set 建立初态；失败时 ranges 仍归测试，target 负责清理可能的部分 region。 */
	if (damon_set_regions(t, ranges, nr_regions / 2,
				DAMON_MIN_REGION_SZ)) {
		kfree(ranges);
		damon_destroy_target(t, NULL);
		kunit_skip(test, "damon_set_regions() fail");
	}
	/* set 只消费范围值、不接管数组，初态建立后立即释放临时转换缓冲区。 */
	kfree(ranges);

	/* 阶段 2：应用三个新范围；失败仍由 destroy_target 回收当前/部分更新链表。 */
	if (damon_set_regions(t, three_regions, 3, DAMON_MIN_REGION_SZ)) {
		damon_destroy_target(t, NULL);
		kunit_skip(test, "second damon_set_regions() fail");
	}

	/* 阶段 3：按稳定链表顺序核对每一对边界，不转移 region ownership。 */
	for (i = 0; i < nr_expected / 2; i++) {
		r = __nth_region_of(t, i);
		KUNIT_EXPECT_EQ(test, r->ar.start, expected[i * 2]);
		KUNIT_EXPECT_EQ(test, r->ar.end, expected[i * 2 + 1]);
	}

	/* 无论 EXPECT 是否失败，最后统一销毁 target 及其全部 region。 */
	damon_destroy_target(t, NULL);
}

/*
 * This function test most common case where the three big regions are only
 * slightly changed.  Target regions should adjust their boundary (10-20-30,
 * 50-55, 70-80, 90-100) to fit with the new big regions or remove target
 * regions (57-79) that now out of the three regions.
 */
/*
 * 场景 1 验证最常见的轻微变化：既有 10-20-30、50-55、70-80、90-100 等边界
 * 应向新的三个大区贴合，而 57-59 等已落在新区间外的 region 应被删除。
 * 业务背景：覆盖 damon_set_regions() 的边界裁剪与局部删除；入参只有借用 test；
 * 出参/返回：无直接返回值，公共 helper 记录断言；注意 fixture 都是栈上数组且调用期有效。
 */
static void damon_test_apply_three_regions1(struct kunit *test)
{
	/* 10-20-30, 50-55-57-59, 70-80-90-100 */
	/* regions 以相邻 start/end 对表达当前八个监控 region。 */
	unsigned long regions[] = {10, 20, 20, 30, 50, 55, 55, 57, 57, 59,
				70, 80, 80, 90, 90, 100};
	/* 5-27, 45-55, 73-104 */
	/* 新三个大区保留部分旧边界，并向两端扩张或收缩。 */
	struct damon_addr_range new_three_regions[3] = {
		(struct damon_addr_range){.start = 5, .end = 27},
		(struct damon_addr_range){.start = 45, .end = 55},
		(struct damon_addr_range){.start = 73, .end = 104} };
	/* 5-20-27, 45-55, 73-80-90-104 */
	/* expected 验证可复用旧切分点时不粗暴退化为仅三个 region。 */
	unsigned long expected[] = {5, 20, 20, 27, 45, 55,
				73, 80, 80, 90, 90, 104};

	damon_do_test_apply_three_regions(test, regions, ARRAY_SIZE(regions),
			new_three_regions, expected, ARRAY_SIZE(expected));
}

/*
 * Test slightly bigger change.  Similar to above, but the second big region
 * now require two target regions (50-55, 57-59) to be removed.
 */
/*
 * 场景 2 的变化稍大：新的第二大区 56-57 不再覆盖旧 50-55 与 57-59，二者都应删除；
 * 第三大区仍复用 80、90 切分点。入参 test 仅借用；无直接返回值，断言/skip 由公共
 * helper 记录；栈上输入数组不发生 ownership 转移。
 */
static void damon_test_apply_three_regions2(struct kunit *test)
{
	/* 10-20-30, 50-55-57-59, 70-80-90-100 */
	/* 初态与场景 1 相同，用于隔离新范围差异。 */
	unsigned long regions[] = {10, 20, 20, 30, 50, 55, 55, 57, 57, 59,
				70, 80, 80, 90, 90, 100};
	/* 5-27, 56-57, 65-104 */
	/* 狭窄第二范围迫使两个旧 region 同时退出监控链表。 */
	struct damon_addr_range new_three_regions[3] = {
		(struct damon_addr_range){.start = 5, .end = 27},
		(struct damon_addr_range){.start = 56, .end = 57},
		(struct damon_addr_range){.start = 65, .end = 104} };
	/* 5-20-27, 56-57, 65-80-90-104 */
	/* 新建 56-57，同时第三范围保留原有内部边界。 */
	unsigned long expected[] = {5, 20, 20, 27, 56, 57,
				65, 80, 80, 90, 90, 104};

	damon_do_test_apply_three_regions(test, regions, ARRAY_SIZE(regions),
			new_three_regions, expected, ARRAY_SIZE(expected));
}

/*
 * Test a big change.  The second big region has totally freed and mapped to
 * different area (50-59 -> 61-63).  The target regions which were in the old
 * second big region (50-55-57-59) should be removed and new target region
 * covering the second big region (61-63) should be created.
 */
/*
 * 场景 3 验证整个第二大区搬迁：旧 50-59 及其两个 region 被删除，并在 61-63 新建
 * region。入参 test 为借用 KUnit 上下文；无直接返回值；公共 helper 负责全部分配回滚，
 * 此 case 的栈数组只描述输入与期望。
 */
static void damon_test_apply_three_regions3(struct kunit *test)
{
	/* 10-20-30, 50-55-57-59, 70-80-90-100 */
	/* 初始 region 列表包含可观察的旧第二大区内部切分。 */
	unsigned long regions[] = {10, 20, 20, 30, 50, 55, 55, 57, 57, 59,
				70, 80, 80, 90, 90, 100};
	/* 5-27, 61-63, 65-104 */
	/* 61-63 与旧第二大区完全不相交，要求走新建路径。 */
	struct damon_addr_range new_three_regions[3] = {
		(struct damon_addr_range){.start = 5, .end = 27},
		(struct damon_addr_range){.start = 61, .end = 63},
		(struct damon_addr_range){.start = 65, .end = 104} };
	/* 5-20-27, 61-63, 65-80-90-104 */
	/* 期望只替换搬迁区，其他仍相交 region 尽量保留边界。 */
	unsigned long expected[] = {5, 20, 20, 27, 61, 63,
				65, 80, 80, 90, 90, 104};

	damon_do_test_apply_three_regions(test, regions, ARRAY_SIZE(regions),
			new_three_regions, expected, ARRAY_SIZE(expected));
}

/*
 * Test another big change.  Both of the second and third big regions (50-59
 * and 70-100) has totally freed and mapped to different area (30-32 and
 * 65-68).  The target regions which were in the old second and third big
 * regions should now be removed and new target regions covering the new second
 * and third big regions should be created.
 */
/*
 * 场景 4 验证第二、第三大区同时完全搬迁：旧 50-59 与 70-100 内所有 region 均删除，
 * 新建 30-32、65-68，第一大区也收缩到 5-7。入参 test 仅借用；无直接返回值；
 * 公共 helper 记录期望并销毁 target，栈上数组不被持有。
 */
static void damon_test_apply_three_regions4(struct kunit *test)
{
	/* 10-20-30, 50-55-57-59, 70-80-90-100 */
	/* 与前三个 case 共用初态，使测试差异只来自新三范围。 */
	unsigned long regions[] = {10, 20, 20, 30, 50, 55, 55, 57, 57, 59,
				70, 80, 80, 90, 90, 100};
	/* 5-7, 30-32, 65-68 */
	/* 三个新区间都很窄，后两者与旧相应大区不相交。 */
	struct damon_addr_range new_three_regions[3] = {
		(struct damon_addr_range){.start = 5, .end = 7},
		(struct damon_addr_range){.start = 30, .end = 32},
		(struct damon_addr_range){.start = 65, .end = 68} };
	/* expect 5-7, 30-32, 65-68 */
	/* 旧内部切分全部消失，最终恰有三个新 region。 */
	unsigned long expected[] = {5, 7, 30, 32, 65, 68};

	damon_do_test_apply_three_regions(test, regions, ARRAY_SIZE(regions),
			new_three_regions, expected, ARRAY_SIZE(expected));
}

/* case 表按发现空洞、四级映射变化的顺序运行，空项是 KUnit 终止哨兵。 */
static struct kunit_case damon_test_cases[] = {
	KUNIT_CASE(damon_test_three_regions_in_vmas),
	KUNIT_CASE(damon_test_apply_three_regions1),
	KUNIT_CASE(damon_test_apply_three_regions2),
	KUNIT_CASE(damon_test_apply_three_regions3),
	KUNIT_CASE(damon_test_apply_three_regions4),
	{},
};

/* suite 在 vaddr.c 同一翻译单元注册，生命周期由 KUnit 框架管理。 */
static struct kunit_suite damon_test_suite = {
	.name = "damon-operations",
	.test_cases = damon_test_cases,
};

/* 生成模块/内建测试注册入口，不转移静态 case 表 ownership。 */
kunit_test_suite(damon_test_suite);

#endif /* _DAMON_VADDR_TEST_H */
/* 结束头文件重复包含保护；仅影响本测试 include。 */

#endif	/* CONFIG_DAMON_VADDR_KUNIT_TEST */
/* CONFIG 关闭时整个测试头为空，不引入 KUnit 依赖或运行时副作用。 */
