/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Data Access Monitor Unit Tests
 *
 * Copyright 2019 Amazon.com, Inc. or its affiliates.  All rights reserved.
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/*
 * DAMON（数据访问监控器）核心单元测试。版权和作者信息保持原样；本文件在
 * CONFIG_DAMON_KUNIT_TEST 下被核心实现包含，直接测试 region/target 生命周期、
 * 监控结果换算、operations 注册以及 DAMOS 配额、过滤器和配置提交协议。
 * 所有用例由底部 damon_test_cases 注册给 KUnit；断言失败由框架记录，分配失败
 * 使用 kunit_skip() 终止当前用例，测试自己取得的对象必须在退出前释放。
 */

#ifdef CONFIG_DAMON_KUNIT_TEST

#ifndef _DAMON_CORE_TEST_H
#define _DAMON_CORE_TEST_H

#include <kunit/test.h>

/*
 * 业务背景：`damon_test_cases` 注册本用例，验证 region 初值、加入 target 后计数，
 * 以及 destroy 从链表摘除并释放的完整生命周期。
 * 入参：test 是 KUnit 持有的用例上下文，只借用来报告断言/跳过。
 * 出参/返回：无直接返回值；成功不遗留 region/target，分配失败跳过并清理已得对象。
 * 注意事项：测试串行操作私有对象、无需生产锁；r ownership 从本函数转给 target 后
 * 由 damon_destroy_region() 消费，t 最终由 damon_free_target() 消费。
 */
static void damon_test_regions(struct kunit *test)
{
	/* r/t 是测试拥有的对象；断言依次覆盖构造初值、链接计数和摘除计数。 */
	struct damon_region *r;
	struct damon_target *t;

	/* 阶段一：构造半开区间 [1,2)，访问计数必须从零开始。 */
	r = damon_new_region(1, 2);
	if (!r)
		kunit_skip(test, "region alloc fail");
	KUNIT_EXPECT_EQ(test, 1ul, r->ar.start);
	KUNIT_EXPECT_EQ(test, 2ul, r->ar.end);
	KUNIT_EXPECT_EQ(test, 0u, r->nr_accesses);

	/* 阶段二：空 target 没有 region；把 r 加入后计数从 0 变为 1。 */
	t = damon_new_target();
	if (!t) {
		damon_free_region(r);
		kunit_skip(test, "target alloc fail");
	}
	KUNIT_EXPECT_EQ(test, 0u, damon_nr_regions(t));

	damon_add_region(r, t);
	KUNIT_EXPECT_EQ(test, 1u, damon_nr_regions(t));

	/* 阶段三：destroy 同时摘链并释放 r，随后释放空 target。 */
	damon_destroy_region(r, t);
	KUNIT_EXPECT_EQ(test, 0u, damon_nr_regions(t));

	damon_free_target(t);
}

/*
 * 业务背景：多个 target 用例需要核对 ctx 链表长度，本 helper 用 DAMON 官方遍历宏计数。
 * 入参：ctx 是测试持有的非空借用对象，调用期间 target 链表不并发变化。
 * 出参/返回：返回 target 个数，不取得 target 引用、不修改 ctx。
 * 注意事项：仅供本 KUnit 文件调用；nr_targets 是无符号计数，链表规模受测试输入限制。
 */
static unsigned int nr_damon_targets(struct damon_ctx *ctx)
{
	struct damon_target *t;
	unsigned int nr_targets = 0;

	damon_for_each_target(t, ctx)
		nr_targets++;

	return nr_targets;
}

/*
 * 业务背景：`damon_test_cases` 注册本用例，验证 target 创建、加入 ctx、销毁的计数协议。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；成功释放 ctx 和 target，OOM 时
 * 清理已得对象后跳过。注意事项：damon_add_target() 后 t ownership 属于 ctx，
 * damon_destroy_target() 同时摘除/释放，不能再单独 free。
 */
static void damon_test_target(struct kunit *test)
{
	/* c/t 是测试私有对象；nr_damon_targets() 在每个状态转换后读取链表。 */
	struct damon_ctx *c = damon_new_ctx();
	struct damon_target *t;

	if (!c)
		kunit_skip(test, "ctx alloc fail");

	/* 空 ctx 计数为 0，链接 t 后为 1，destroy 后恢复 0。 */
	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(c);
		kunit_skip(test, "target alloc fail");
	}
	KUNIT_EXPECT_EQ(test, 0u, nr_damon_targets(c));

	damon_add_target(c, t);
	KUNIT_EXPECT_EQ(test, 1u, nr_damon_targets(c));

	/* destroy 必须同时摘链和释放 t，ctx 随后仍是可销毁的空对象。 */
	damon_destroy_target(t, c);
	KUNIT_EXPECT_EQ(test, 0u, nr_damon_targets(c));

	damon_destroy_ctx(c);
}

/*
 * Test kdamond_reset_aggregated()
 *
 * DAMON checks access to each region and aggregates this information as the
 * access frequency of each region.  In detail, it increases '->nr_accesses' of
 * regions that an access has confirmed.  'kdamond_reset_aggregated()' flushes
 * the aggregated information ('->nr_accesses' of each regions) to the result
 * buffer.  As a result of the flushing, the '->nr_accesses' of regions are
 * initialized to zero.
 */
/*
 * 测试 kdamond_reset_aggregated()：DAMON 每确认一次访问便累加 region 的
 * nr_accesses；重置函数把聚合访问频率刷新到结果缓冲区后，将各 region 的计数
 * 清零，但不得删除 region 或 target。
 *
 * 业务背景：KUnit 用 3×3 个 region 构造多 target 聚合状态，验证重置只改计数。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；ctx 最终递归释放所有 target/region。
 * 注意事项：数组第一维是 target、第二维是 region；任意 OOM 都 destroy ctx 后跳过。
 */
static void damon_test_aggregate(struct kunit *test)
{
	/* saddr/eaddr/访问数组共同描述 3 个 target 各 3 个 region；it/ir 是对应索引。 */
	struct damon_ctx *ctx = damon_new_ctx();
	unsigned long saddr[][3] = {{10, 20, 30}, {5, 42, 49}, {13, 33, 55} };
	unsigned long eaddr[][3] = {{15, 27, 40}, {31, 45, 55}, {23, 44, 66} };
	unsigned long accesses[][3] = {{42, 95, 84}, {10, 20, 30}, {0, 1, 2} };
	struct damon_target *t;
	struct damon_region *r;
	int it, ir;

	if (!ctx)
		kunit_skip(test, "ctx alloc fail");

	/* 阶段一：创建三个空 target；失败时 ctx 递归清理此前已链接对象。 */
	for (it = 0; it < 3; it++) {
		t = damon_new_target();
		if (!t) {
			damon_destroy_ctx(ctx);
			kunit_skip(test, "target alloc fail");
		}
		damon_add_target(ctx, t);
	}

	/* 阶段二：为每个 target 建三个 region，并注入聚合计数和万分比计数。 */
	it = 0;
	damon_for_each_target(t, ctx) {
		for (ir = 0; ir < 3; ir++) {
			r = damon_new_region(saddr[it][ir], eaddr[it][ir]);
			if (!r) {
				damon_destroy_ctx(ctx);
				kunit_skip(test, "region alloc fail");
			}
			/* 两种访问计数字段使用整数次数和乘 10000 的万分比刻度。 */
			r->nr_accesses = accesses[it][ir];
			r->nr_accesses_bp = accesses[it][ir] * 10000;
			damon_add_region(r, t);
		}
		it++;
	}
	/* 被测提交点：刷新聚合结果并把每个 nr_accesses 清零。 */
	kdamond_reset_aggregated(ctx);
	it = 0;
	damon_for_each_target(t, ctx) {
		ir = 0;
		/* '->nr_accesses' should be zeroed */
		/* 每个 `nr_accesses` 都应归零。 */
		damon_for_each_region(r, t) {
			KUNIT_EXPECT_EQ(test, 0u, r->nr_accesses);
			ir++;
		}
		/* regions should be preserved */
		/* 每个 target 的三个 region 必须保留，重置不改变拓扑。 */
		KUNIT_EXPECT_EQ(test, 3, ir);
		it++;
	}
	/* targets also should be preserved */
	/* 三个 target 同样必须保留；最终由 ctx 销毁路径递归释放。 */
	KUNIT_EXPECT_EQ(test, 3, it);

	damon_destroy_ctx(ctx);
}

/*
 * 业务背景：KUnit 验证 damon_split_region_at() 在 25 处分割 [0,100)，并把访问率、
 * 上次访问次数和 age 等监控状态复制给新后半 region。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；target 最终释放两个 region。
 * 注意事项：r 链入 t 后由 t 持有；r_new 只是链表中的借用后继，不单独释放。
 */
static void damon_test_split_at(struct kunit *test)
{
	/* r 是原区间，r_new 是分割后从链表取得的后半区借用指针。 */
	struct damon_target *t;
	struct damon_region *r, *r_new;

	/* 先分别创建容器和唯一输入段；任一 OOM 都不进入被测分割路径。 */
	t = damon_new_target();
	if (!t)
		kunit_skip(test, "target alloc fail");
	r = damon_new_region(0, 100);
	if (!r) {
		damon_free_target(t);
		kunit_skip(test, "region alloc fail");
	}
	/* 使用彼此不同的非零值，让每个被复制字段都能独立暴露遗漏。 */
	r->nr_accesses_bp = 420000;
	r->nr_accesses = 42;
	r->last_nr_accesses = 15;
	r->age = 10;
	/* 四个字段构成待复制的监控快照，不能只验证地址分割。 */
	/* 注入非零监控状态后分割，排除实现只正确切地址却丢统计的缺陷。 */
	damon_add_region(r, t);
	damon_split_region_at(t, r, 25);
	KUNIT_EXPECT_EQ(test, r->ar.start, 0ul);
	KUNIT_EXPECT_EQ(test, r->ar.end, 25ul);

	/* 阶段二：验证地址无缝覆盖 [0,100) 且所有历史字段相等。 */
	r_new = damon_next_region(r);
	KUNIT_EXPECT_EQ(test, r_new->ar.start, 25ul);
	KUNIT_EXPECT_EQ(test, r_new->ar.end, 100ul);

	KUNIT_EXPECT_EQ(test, r->nr_accesses_bp, r_new->nr_accesses_bp);
	KUNIT_EXPECT_EQ(test, r->nr_accesses, r_new->nr_accesses);
	KUNIT_EXPECT_EQ(test, r->last_nr_accesses, r_new->last_nr_accesses);
	KUNIT_EXPECT_EQ(test, r->age, r_new->age);

	damon_free_target(t);
}

/*
 * 业务背景：KUnit 验证 damon_merge_two_regions() 合并相邻 [0,100)/[100,300)，
 * 新访问次数和 age 按区间大小加权，并只保留第一个 region 节点。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；t 最终释放合并结果。
 * 注意事项：r2 加入后由 t 持有且被 merge 消费，合并后不得再解引用 r2。
 */
static void damon_test_merge_two(struct kunit *test)
{
	/* r/r2 是待合并拥有对象，r3 是遍历时借用；i 验证最终节点数。 */
	struct damon_target *t;
	struct damon_region *r, *r2, *r3;
	int i;

	/* 第一段是合并后保留下来的链表节点，先完成其分配和错误清理。 */
	t = damon_new_target();
	if (!t)
		kunit_skip(test, "target alloc fail");
	r = damon_new_region(0, 100);
	if (!r) {
		damon_free_target(t);
		kunit_skip(test, "region alloc fail");
	}
	/* 第一段长度 100，统计值为第二段的一半，为加权结果提供可辨输入。 */
	r->nr_accesses = 10;
	r->nr_accesses_bp = 100000;
	r->age = 9;
	damon_add_region(r, t);
	/* 第二段长度是第一段两倍，并使用不同统计值以验证加权而非简单平均。 */
	r2 = damon_new_region(100, 300);
	if (!r2) {
		damon_free_target(t);
		kunit_skip(test, "second region alloc fail");
	}
	r2->nr_accesses = 20;
	r2->nr_accesses_bp = 200000;
	r2->age = 21;
	damon_add_region(r2, t);

	/* 被测转换：区间长度比 1:2，所以 10/20 和 age 9/21 加权为 16/17。 */
	damon_merge_two_regions(t, r, r2);
	KUNIT_EXPECT_EQ(test, r->ar.start, 0ul);
	KUNIT_EXPECT_EQ(test, r->ar.end, 300ul);
	KUNIT_EXPECT_EQ(test, r->nr_accesses, 16u);
	KUNIT_EXPECT_EQ(test, r->nr_accesses_bp, 160000u);
	KUNIT_EXPECT_EQ(test, r->age, 17u);

	/* 地址/统计正确之外，还要求 target 链表中只剩 r 一个节点。 */
	i = 0;
	damon_for_each_region(r3, t) {
		KUNIT_EXPECT_PTR_EQ(test, r, r3);
		i++;
	}
	KUNIT_EXPECT_EQ(test, i, 1);

	damon_free_target(t);
}

/*
 * 业务背景：`damon_test_merge_regions_of()` 需按序号检查合并后区间，本 helper
 * 沿 target region 链表返回第 idx 个节点。
 * 入参：t 为非空借用 target；idx 为从 0 开始的索引，负数或越界均不匹配。
 * 出参/返回：命中返回借用 region，否则 NULL；不改变链表或引用。
 * 注意事项：返回值只在 t 及其 region 拓扑不变期间有效，本测试无并发修改。
 */
static struct damon_region *__nth_region_of(struct damon_target *t, int idx)
{
	struct damon_region *r;
	unsigned int i = 0;

	damon_for_each_region(r, t) {
		if (i++ == idx)
			return r;
	}

	return NULL;
}

/*
 * 业务背景：KUnit 构造九段不同访问频率的 region，验证 damon_merge_regions_of()
 * 只合并访问差距不超过 9 且合并后大小不超过 9999 的相邻段。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；最终 target 及 region 全释放。
 * 注意事项：sa/ea/nrs 同索引组成输入，saddrs/eaddrs 是六段期望输出；OOM 跳过。
 */
static void damon_test_merge_regions_of(struct kunit *test)
{
	/* 输入与期望数组单位均为地址/访问次数；i 驱动构造和结果核对。 */
	struct damon_target *t;
	struct damon_region *r;
	unsigned long sa[] = {0, 100, 114, 122, 130, 156, 170, 184, 230};
	unsigned long ea[] = {100, 112, 122, 130, 156, 170, 184, 230, 10170};
	unsigned int nrs[] = {0, 0, 10, 10, 20, 30, 1, 2, 5};

	/* saddrs/eaddrs 是合并后逐节点地址 oracle，不与输入数组共享存储。 */
	unsigned long saddrs[] = {0, 114, 130, 156, 170, 230};
	unsigned long eaddrs[] = {112, 130, 156, 170, 230, 10170};
	int i;

	/* 空 target 是被测合并的容器；构造失败时本用例没有可验证状态。 */
	t = damon_new_target();
	if (!t)
		kunit_skip(test, "target alloc fail");
	/* 按表逐项建立九个输入节点，保持空洞和相邻关系与 oracle 完全一致。 */
	for (i = 0; i < ARRAY_SIZE(sa); i++) {
		r = damon_new_region(sa[i], ea[i]);
		if (!r) {
			damon_free_target(t);
			kunit_skip(test, "region alloc fail");
		}
		/* 每段访问次数同时写普通与 bp 表示，使相似度判断输入一致。 */
		r->nr_accesses = nrs[i];
		r->nr_accesses_bp = nrs[i] * 10000;
		damon_add_region(r, t);
	}

	/* 相邻访问次数差阈值为 9，单个合并 region 最大长度为 9999。 */
	damon_merge_regions_of(t, 9, 9999);
	/* 0-112, 114-130, 130-156, 156-170, 170-230, 230-10170 */
	/* 期望六段地址分别如下；不相邻空洞 112-114 不能跨越合并。 */
	KUNIT_EXPECT_EQ(test, damon_nr_regions(t), 6u);
	for (i = 0; i < 6; i++) {
		r = __nth_region_of(t, i);
		KUNIT_EXPECT_EQ(test, r->ar.start, saddrs[i]);
		KUNIT_EXPECT_EQ(test, r->ar.end, eaddrs[i]);
	}
	damon_free_target(t);
}

/*
 * 业务背景：KUnit 以单小段、单大段和三个离散段覆盖 damon_split_regions_of() 的
 * 区域数上限，并在第三轮传入最小 region 大小 5。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；ctx 与每轮 target 均被释放。
 * 注意事项：每个 target 加入 region 后拥有其生命周期；OOM 先清理再跳过。
 */
static void damon_test_split_regions_of(struct kunit *test)
{
	/* sa/ea 描述第三轮三段输入；c 跨三轮复用，t/r 每轮重新创建。 */
	struct damon_ctx *c;
	struct damon_target *t;
	struct damon_region *r;
	unsigned long sa[] = {0, 300, 500};
	unsigned long ea[] = {220, 400, 700};
	int i;

	c = damon_new_ctx();
	if (!c)
		kunit_skip(test, "ctx alloc fail");

	/* 场景一：[0,22) 最多拆成 2 段且最小长度为 1。 */
	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(c);
		kunit_skip(test, "target alloc fail");
	}
	/* 本轮只需一个大于拆分上限的连续输入段。 */
	r = damon_new_region(0, 22);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(c);
		kunit_skip(test, "region alloc fail");
	}
	damon_add_region(r, t);
	/* 本轮 target 不加入 ctx；ctx 只提供 attrs/随机拆分上下文，target 由测试释放。 */
	damon_split_regions_of(c, t, 2, 1);
	KUNIT_EXPECT_LE(test, damon_nr_regions(t), 2u);
	damon_free_target(t);

	/* 场景二：[0,220) 最多拆成 4 段。 */
	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(c);
		kunit_skip(test, "second target alloc fail");
	}
	/* 放大地址长度，确认上限由 nr_subs 而非偶然的小区间控制。 */
	r = damon_new_region(0, 220);
	if (!r) {
		damon_free_target(t);
		damon_destroy_ctx(c);
		kunit_skip(test, "second region alloc fail");
	}
	/* 单段输入加入后立即执行并核对数量，随后结束该轮 ownership。 */
	damon_add_region(r, t);
	damon_split_regions_of(c, t, 4, 1);
	KUNIT_EXPECT_LE(test, damon_nr_regions(t), 4u);
	damon_free_target(t);

	/* 场景三：三段总预算最多 12，并把 min_region_sz=5 传给随机拆分。 */
	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(c);
		kunit_skip(test, "third target alloc fail");
	}
	/* 三个地址对分别建段；中间空洞用于确认拆分不会连通离散区间。 */
	for (i = 0; i < ARRAY_SIZE(sa); i++) {
		r = damon_new_region(sa[i], ea[i]);
		if (!r) {
			damon_free_target(t);
			damon_destroy_ctx(c);
			kunit_skip(test, "region alloc fail");
		}
		damon_add_region(r, t);
	}
	/* 三个输入 region 分别独立拆分，每段上限 4 因而总数上限为 12。 */
	damon_split_regions_of(c, t, 4, 5);
	KUNIT_EXPECT_LE(test, damon_nr_regions(t), 12u);
	/* 现有无符号余数 >= 0 断言恒真，不能证明 5 字节对齐，仅保留原测试行为。 */
	damon_for_each_region(r, t)
		KUNIT_EXPECT_GE(test, damon_sz_region(r) % 5ul, 0ul);
	damon_free_target(t);

	damon_destroy_ctx(c);
}

/*
 * 业务背景：KUnit 验证 DAMON operations 注册表的配置可选初态、成功选择、重复
 * 注册拒绝、未知 id 拒绝、注销后重注册及最终恢复。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；ctx 释放，临时注册状态恢复。
 * 注意事项：直接改 damon_registered_ops 时持 damon_ops_lock；need_cleanup 记录
 * 测试是否为缺省未注册配置临时安装了 VADDR，避免污染后续用例。
 */
static void damon_test_ops_registration(struct kunit *test)
{
	/* ops 是重复注册候选，bak 保存全局槽原值，need_cleanup 追踪初态差异。 */
	struct damon_ctx *c = damon_new_ctx();
	struct damon_operations ops = {.id = DAMON_OPS_VADDR}, bak;
	bool need_cleanup = false;

	if (!c)
		kunit_skip(test, "ctx alloc fail");

	/* DAMON_OPS_VADDR is registered only if CONFIG_DAMON_VADDR is set */
	/* 只有 CONFIG_DAMON_VADDR 会预注册该实现；否则测试先临时注册一个占位 ops。 */
	if (!damon_is_registered_ops(DAMON_OPS_VADDR)) {
		bak.id = DAMON_OPS_VADDR;
		KUNIT_EXPECT_EQ(test, damon_register_ops(&bak), 0);
		need_cleanup = true;
	}

	/* DAMON_OPS_VADDR is ensured to be registered */
	/* 注册存在后，ctx 按 VADDR id 选择实现必须成功。 */
	KUNIT_EXPECT_EQ(test, damon_select_ops(c, DAMON_OPS_VADDR), 0);

	/* Double-registration is prohibited */
	/* 同一 id 的第二次注册必须返回 -EINVAL，不能覆盖现有函数表。 */
	KUNIT_EXPECT_EQ(test, damon_register_ops(&ops), -EINVAL);

	/* Unknown ops id cannot be registered */
	/* NR_DAMON_OPS 是哨兵而非有效 id，select 必须拒绝。 */
	KUNIT_EXPECT_EQ(test, damon_select_ops(c, NR_DAMON_OPS), -EINVAL);

	/* Registration should success after unregistration */
	/* 锁内暂存并清空全局槽模拟注销，随后相同 id 应能重新注册。 */
	mutex_lock(&damon_ops_lock);
	bak = damon_registered_ops[DAMON_OPS_VADDR];
	damon_registered_ops[DAMON_OPS_VADDR] = (struct damon_operations){};
	mutex_unlock(&damon_ops_lock);

	ops.id = DAMON_OPS_VADDR;
	KUNIT_EXPECT_EQ(test, damon_register_ops(&ops), 0);

	mutex_lock(&damon_ops_lock);
	damon_registered_ops[DAMON_OPS_VADDR] = bak;
	mutex_unlock(&damon_ops_lock);

	/* Check double-registration failure again */
	/* 恢复原槽后再次验证临时 ops 会因重复 id 被拒绝。 */
	KUNIT_EXPECT_EQ(test, damon_register_ops(&ops), -EINVAL);

	damon_destroy_ctx(c);

	/* 若测试改变了配置初态，锁内清空临时槽，确保用例间隔离。 */
	if (need_cleanup) {
		mutex_lock(&damon_ops_lock);
		damon_registered_ops[DAMON_OPS_VADDR] =
			(struct damon_operations){};
		mutex_unlock(&damon_ops_lock);
	}
}

/*
 * 业务背景：`damon_test_set_regions()` 用本表驱动 helper 构造旧 region，调用
 * damon_set_regions()，再逐段比对新拓扑。
 * 入参：test 为 KUnit 上下文；old/new/expect_ranges 分别是输入旧区间、目标区间和
 * 期望区间数组，sz_* 为元素数；min_region_sz 是允许的最小地址长度。
 * 出参/返回：无；成功或计数断言失败都销毁 target，OOM 清理后跳过。
 * 注意事项：所有数组均为调用期借用；target 持有动态 region，结果数不符时禁止继续索引期望数组。
 */
static void damon_test_set_regions_for(struct kunit *test,
		struct damon_addr_range *old_ranges, int sz_old_ranges,
		struct damon_addr_range *new_ranges, int sz_new_ranges,
		unsigned long min_region_sz,
		struct damon_addr_range *expect_ranges, int sz_expect_ranges)
{
	/* t/r 构造并承载结果；i 依次关联数组元素与链表节点。 */
	struct damon_target *t;
	struct damon_region *r;
	int i;

	t = damon_new_target();
	if (!t)
		kunit_skip(test, "target alloc fail");
	/* 阶段一：按 old_ranges 顺序创建并把每个 region ownership 转给 t。 */
	for (i = 0; i < sz_old_ranges; i++) {
		r = damon_new_region(old_ranges[i].start, old_ranges[i].end);
		if (!r) {
			damon_destroy_target(t, NULL);
			kunit_skip(test, "%d-th r alloc fail\n", i);
		}
		damon_add_region(r, t);
	}

	/* 被测函数应复用、删除或补建节点，使 target 精确覆盖 new_ranges。 */
	damon_set_regions(t, new_ranges, sz_new_ranges, min_region_sz);

	KUNIT_EXPECT_EQ(test, damon_nr_regions(t), sz_expect_ranges);
	if (damon_nr_regions(t) != sz_expect_ranges) {
		damon_destroy_target(t, NULL);
		return;
	}
	/* 节点数一致后才能安全按序比较每个半开区间。 */
	i = 0;
	damon_for_each_region(r, t) {
		KUNIT_EXPECT_EQ(test, r->ar.start, expect_ranges[i].start);
		KUNIT_EXPECT_EQ(test, r->ar.end, expect_ranges[i++].end);
	}

	damon_destroy_target(t, NULL);
}

/*
 * 业务背景：注册 KUnit 用例以表驱动方式覆盖 damon_set_regions() 的空目标初始化、
 * 无交集删除、空洞补齐、尾部追加和中间插入五类拓扑变化。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；每个子场景由 helper 自行清理。
 * 注意事项：复合字面量数组只在当前完整表达式期间有效，helper 同步消费且不保存指针。
 */
static void damon_test_set_regions(struct kunit *test)
{
	/* Initial build up on empty target. */
	/* 空 target 初次建立两段 [5,15)、[15,25)。 */
	damon_test_set_regions_for(test,
			(struct damon_addr_range[]){}, 0,
			(struct damon_addr_range[]){
			{.start = 5, .end = 15},
			{.start = 15, .end = 25},
			}, 2,
			1,
			/* 期望与新范围相同，证明空 target 可直接建立。 */
			(struct damon_addr_range[]){
			{.start = 5, .end = 15},
			{.start = 15, .end = 25},
			}, 2);
	/* Un-intersecting regions should be removed. */
	/* 新旧完全无交集时删除全部旧节点，只保留 [18,23)。 */
	damon_test_set_regions_for(test,
			(struct damon_addr_range[]){
			{.start = 4, .end = 16},
			{.start = 24, .end = 32},
			}, 2,
			(struct damon_addr_range[]){
			{.start = 18, .end = 23},
			}, 1,
			1,
			/* 两个旧节点都应消失，结果仅一个新节点。 */
			(struct damon_addr_range[]){
			{.start = 18, .end = 23},
			}, 1);
	/*
	 * Holes should be filled up with new regions.
	 *
	 * old:       [4,   16)        [24,     32)
	 * new:         [8,                 28)
	 * expect:      [8, 16)[16,24),[24, 28)
	 */
	/* 新范围跨越旧两段及中间空洞：保留交集边界，并为 [16,24) 补新节点。 */
	damon_test_set_regions_for(test,
			(struct damon_addr_range[]){
			{.start = 4, .end = 16},
			{.start = 24, .end = 32},
			}, 2,
			(struct damon_addr_range[]){
			{.start = 8, .end = 28},
			}, 1,
			1,
			/* 旧边界 16/24 被保留，空洞成为中间新段。 */
			(struct damon_addr_range[]){
			{.start = 8, .end = 16},
			{.start = 16, .end = 24},
			{.start = 24, .end = 28},
			}, 3);
	/*
	 * New regions should be able to be appended.
	 *
	 * old:       [0, 4)[4,    17)
	 * new:       [0,       15)     [25, 40)
	 * expect:    [0, 4)[4, 15)     [25, 40)
	 */
	/* 新范围延伸到现有尾端之外，应裁短旧段并追加 [25,40)。 */
	damon_test_set_regions_for(test,
			(struct damon_addr_range[]){
			{.start = 0, .end = 4},
			{.start = 4, .end = 17},
			}, 2,
			(struct damon_addr_range[]){
			{.start = 0, .end = 15},
			/* 中间插入段与两侧均不相交。 */
			{.start = 25, .end = 40},
			}, 2,
			1,
			/* 前两段复用/裁剪，第三段在尾部新建。 */
			(struct damon_addr_range[]){
			{.start = 0, .end = 4},
			{.start = 4, .end = 15},
			{.start = 25, .end = 40},
			}, 3);
	/*
	 * New regions should be able to be inserted.
	 *
	 * old:       [0, 4)                      [42,    52)
	 * new:       [0,       15)     [25, 40)    [44, 50)
	 * expect:    [0,       15)     [25, 40)    [44, 50)
	 */
	/* 新范围还可插入旧节点之间，并把旧 [42,52) 裁成 [44,50)。 */
	damon_test_set_regions_for(test,
			(struct damon_addr_range[]){
			{.start = 0, .end = 4},
			{.start = 42, .end = 52},
			}, 2,
			/* 三个 src 范围同时要求扩大、插入和裁剪旧节点。 */
			(struct damon_addr_range[]){
			{.start = 0, .end = 15},
			{.start = 25, .end = 40},
			{.start = 44, .end = 50},
			}, 3,
			1,
			/* 三段最终精确等于三个 new ranges，不保留旧外侧部分。 */
			(struct damon_addr_range[]){
			{.start = 0, .end = 15},
			{.start = 25, .end = 40},
			{.start = 44, .end = 50},
			}, 3);
}

/*
 * 业务背景：验证访问次数转万分比时的大聚合周期边界；32 位上构造表达式可能
 * 溢出为零，此时生产路径本会拒绝该属性，测试必须跳过以免除零。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；可表示该周期时断言换算结果为 0。
 * 注意事项：sample/aggr_interval 均为微秒；attrs 是栈值，不涉及 ownership。
 */
static void damon_test_nr_accesses_to_accesses_bp(struct kunit *test)
{
	struct damon_attrs attrs = {
		.sample_interval = 10,
		.aggr_interval = ((unsigned long)UINT_MAX + 1) * 10
	};

	/*
	 * In some cases such as 32bit architectures where UINT_MAX is
	 * ULONG_MAX, attrs.aggr_interval becomes zero.  Calling
	 * damon_nr_accesses_to_accesses_bp() in the case will cause
	 * divide-by-zero.  Such case is prohibited in normal execution since
	 * the caution is documented on the comment for the function, and
	 * damon_update_monitoring_results() does the check.  Skip the test in
	 * the case.
	 */
	/*
	 * 某些 32 位体系结构的 UINT_MAX 等于 ULONG_MAX，表达式会使 aggr_interval
	 * 回绕为零，调用换算函数将除零。正常路径由函数契约和
	 * damon_update_monitoring_results() 禁止该属性，因此本配置下跳过测试。
	 */
	if (!attrs.aggr_interval)
		kunit_skip(test, "aggr_interval is zero.");

	KUNIT_EXPECT_EQ(test, damon_nr_accesses_to_accesses_bp(123, &attrs), 0);
}

/*
 * 业务背景：KUnit 验证监控间隔改变时 nr_accesses 与 age 按旧/新采样、聚合周期
 * 保持等价语义，覆盖同比放大、访问次数放大和 age 保持三组比例。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；动态 region 最终释放。
 * 注意事项：attrs 时间单位为微秒，nr_accesses_bp 为万分比刻度；OOM 时跳过。
 */
static void damon_test_update_monitoring_result(struct kunit *test)
{
	/* old_attrs 是共同基线，new_attrs 每轮覆盖不同周期；r 承载可观察统计状态。 */
	struct damon_attrs old_attrs = {
		.sample_interval = 10, .aggr_interval = 1000,};
	struct damon_attrs new_attrs;
	struct damon_region *r = damon_new_region(3, 7);

	if (!r)
		kunit_skip(test, "region alloc fail");

	r->nr_accesses = 15;
	r->nr_accesses_bp = 150000;
	r->age = 20;

	/* 聚合和采样同比放大十倍：次数不变，age 对应的聚合轮次缩为 2。 */
	new_attrs = (struct damon_attrs){
		.sample_interval = 100, .aggr_interval = 10000,};
	damon_update_monitoring_result(r, &old_attrs, &new_attrs, false);
	KUNIT_EXPECT_EQ(test, r->nr_accesses, 15);
	KUNIT_EXPECT_EQ(test, r->age, 2);

	/* 采样缩短十倍且聚合不变：等价访问次数放大十倍，age 仍为 2。 */
	new_attrs = (struct damon_attrs){
		.sample_interval = 1, .aggr_interval = 1000};
	damon_update_monitoring_result(r, &old_attrs, &new_attrs, false);
	KUNIT_EXPECT_EQ(test, r->nr_accesses, 150);
	KUNIT_EXPECT_EQ(test, r->age, 2);

	/* 采样缩短十倍、聚合也缩短十倍：次数放大，age 回到 20。 */
	new_attrs = (struct damon_attrs){
		.sample_interval = 1, .aggr_interval = 100};
	damon_update_monitoring_result(r, &old_attrs, &new_attrs, false);
	KUNIT_EXPECT_EQ(test, r->nr_accesses, 150);
	KUNIT_EXPECT_EQ(test, r->age, 20);

	damon_free_region(r);
}

/*
 * 业务背景：KUnit 验证 damon_set_attrs() 接受合法 region/时间边界，并拒绝最小
 * region 数过小、最大数小于最小数、聚合周期短于采样周期。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；ctx 最终释放。
 * 注意事项：invalid_attrs 每轮从合法快照复制，避免前一失败字段污染后一场景。
 */
static void damon_test_set_attrs(struct kunit *test)
{
	struct damon_ctx *c = damon_new_ctx();
	struct damon_attrs valid_attrs = {
		.min_nr_regions = 10, .max_nr_regions = 1000,
		.sample_interval = 5000, .aggr_interval = 100000,};
	struct damon_attrs invalid_attrs;

	if (!c)
		kunit_skip(test, "ctx alloc fail");

	/* 合法基线先提交成功，确认后续 -EINVAL 来自单字段变化。 */
	KUNIT_EXPECT_EQ(test, damon_set_attrs(c, &valid_attrs), 0);

	/* 三个失败场景分别只改一个字段，期望均为 -EINVAL。 */
	invalid_attrs = valid_attrs;
	invalid_attrs.min_nr_regions = 1;
	KUNIT_EXPECT_EQ(test, damon_set_attrs(c, &invalid_attrs), -EINVAL);

	/* 第二轮恢复基线，仅制造 max < min 的数量边界倒置。 */
	invalid_attrs = valid_attrs;
	invalid_attrs.max_nr_regions = 9;
	KUNIT_EXPECT_EQ(test, damon_set_attrs(c, &invalid_attrs), -EINVAL);

	/* 第三轮再次恢复基线，仅制造 aggr < sample 的时间边界倒置。 */
	invalid_attrs = valid_attrs;
	invalid_attrs.aggr_interval = 4999;
	KUNIT_EXPECT_EQ(test, damon_set_attrs(c, &invalid_attrs), -EINVAL);

	damon_destroy_ctx(c);
}

/*
 * 业务背景：KUnit 用十个新样本验证 damon_moving_sum() 按窗口长度从旧总和移除
 * 均值并加入当前值，防止长时反馈统计漂移。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；逐轮只更新局部 mvsum。
 * 注意事项：所有数值为无符号测试刻度，数组同长且期望序列逐项对应。
 */
static void damon_test_moving_sum(struct kunit *test)
{
	unsigned int mvsum = 50000, nomvsum = 50000, len_window = 10;
	unsigned int new_values[] = {10000, 0, 10000, 0, 0, 0, 10000, 0, 0, 0};
	unsigned int expects[] = {55000, 50000, 55000, 50000, 45000, 40000,
		45000, 40000, 35000, 30000};
	int i;

	/* nomvsum 固定为 50000，十轮新值依次驱动同一长度为 10 的滑动窗口。 */
	for (i = 0; i < ARRAY_SIZE(new_values); i++) {
		mvsum = damon_moving_sum(mvsum, nomvsum, len_window,
				new_values[i]);
		KUNIT_EXPECT_EQ(test, mvsum, expects[i]);
	}
}

/*
 * 业务背景：KUnit 验证 damos_new_filter() 保存 ANON 类型与 matching 标志，并把
 * 嵌入 list 初始化为空环，供后续加入 scheme。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；filter 最终 destroy，OOM 跳过。
 * 注意事项：新对象 ownership 留在测试，未加入任何 scheme，故直接销毁合法。
 */
static void damos_test_new_filter(struct kunit *test)
{
	struct damos_filter *filter;

	filter = damos_new_filter(DAMOS_FILTER_TYPE_ANON, true, false);
	if (!filter)
		kunit_skip(test, "filter alloc fail");
	/* ANON 通常由 ops 层处理；本用例传 allow=false，但现有断言未覆盖 allow 字段。 */
	KUNIT_EXPECT_EQ(test, filter->type, DAMOS_FILTER_TYPE_ANON);
	KUNIT_EXPECT_EQ(test, filter->matching, true);
	KUNIT_EXPECT_PTR_EQ(test, filter->list.prev, &filter->list);
	KUNIT_EXPECT_PTR_EQ(test, filter->list.next, &filter->list);
	damos_destroy_filter(filter);
}

/*
 * 业务背景：quota-goal 表驱动 helper 提交 src 到 dst 后核对通用字段和各 metric
 * 专属字段；仅当提交前 dst 已是 PSI 且累计值非零时，条件断言其运行期值保留。
 * 入参：test 为 KUnit 上下文；dst 是输入输出目标；src 是只读借用配置。
 * 出参/返回：无；dst 就地更新，两个栈/调用者对象 ownership 均不转移。
 * 注意事项：USER_INPUT 复制 current_value；SOME_MEM_PSI_US 保留既有 last_psi_total；
 * NODE_* 复制 nid，NODE_MEMCG_* 还复制 memcg_id。
 */
static void damos_test_commit_quota_goal_for(struct kunit *test,
		struct damos_quota_goal *dst,
		struct damos_quota_goal *src)
{
	u64 dst_last_psi_total = 0;

	if (dst->metric == DAMOS_QUOTA_SOME_MEM_PSI_US)
		dst_last_psi_total = dst->last_psi_total;
	/* 提交后 metric/target 总是来自 src，运行期字段按 metric 的特殊契约处理。 */
	damos_commit_quota_goal(dst, src);

	KUNIT_EXPECT_EQ(test, dst->metric, src->metric);
	KUNIT_EXPECT_EQ(test, dst->target_value, src->target_value);
	if (src->metric == DAMOS_QUOTA_USER_INPUT)
		KUNIT_EXPECT_EQ(test, dst->current_value, src->current_value);
	if (dst_last_psi_total && src->metric == DAMOS_QUOTA_SOME_MEM_PSI_US)
		KUNIT_EXPECT_EQ(test, dst->last_psi_total, dst_last_psi_total);
	/* 按 metric 核对配置联合字段；default 指无节点附加字段的指标。 */
	switch (dst->metric) {
	case DAMOS_QUOTA_NODE_MEM_USED_BP:
	case DAMOS_QUOTA_NODE_MEM_FREE_BP:
		/* 节点已用/空闲内存万分比目标由 nid 选择被观测 NUMA 节点。 */
		KUNIT_EXPECT_EQ(test, dst->nid, src->nid);
		break;
	case DAMOS_QUOTA_NODE_MEMCG_USED_BP:
	case DAMOS_QUOTA_NODE_MEMCG_FREE_BP:
		/* 节点 memcg 已用/空闲万分比还需 memcg_id 共同定位对象。 */
		KUNIT_EXPECT_EQ(test, dst->nid, src->nid);
		KUNIT_EXPECT_EQ(test, dst->memcg_id, src->memcg_id);
		break;
	default:
		/* 用户输入和 PSI 指标没有 nid/memcg 配置字段需要断言。 */
		break;
	}
}

/*
 * 业务背景：注册用例依次把 USER_INPUT、节点空闲/已用、节点 memcg 已用/空闲和
 * SOME_MEM_PSI_US 六种目标提交到同一 dst，覆盖 helper 的全部专属字段协议。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；所有 goal 是当前栈/复合字面量。
 * 注意事项：每次提交后的 dst 成为下一次输入，专属验证由 `_for` helper 完成。
 */
static void damos_test_commit_quota_goal(struct kunit *test)
{
	struct damos_quota_goal dst = {
		.metric = DAMOS_QUOTA_SOME_MEM_PSI_US,
		.target_value = 1000,
		.current_value = 123,
		.last_psi_total = 456,
	};

	/* 用户输入指标由控制者直接给 target/current 值。 */
	damos_test_commit_quota_goal_for(test, &dst,
			&(struct damos_quota_goal){
			.metric = DAMOS_QUOTA_USER_INPUT,
			.target_value = 789,
			.current_value = 12});
	/* 节点空闲与已用内存万分比指标分别验证 nid。 */
	damos_test_commit_quota_goal_for(test, &dst,
			&(struct damos_quota_goal){
			.metric = DAMOS_QUOTA_NODE_MEM_FREE_BP,
			.target_value = 345,
			.current_value = 678,
			.nid = 9,
			});
	/* 同类 USED 指标换用另一 nid，避免实现硬编码 FREE 分支输入。 */
	damos_test_commit_quota_goal_for(test, &dst,
			&(struct damos_quota_goal){
			.metric = DAMOS_QUOTA_NODE_MEM_USED_BP,
			.target_value = 12,
			.current_value = 345,
			.nid = 6,
			});
	/* 节点 memcg 指标同时验证 nid 与 memcg_id。 */
	damos_test_commit_quota_goal_for(test, &dst,
			&(struct damos_quota_goal){
			.metric = DAMOS_QUOTA_NODE_MEMCG_USED_BP,
			.target_value = 456,
			.current_value = 567,
			.nid = 6,
			.memcg_id = 7,
			});
	/* FREE 变体同时换 nid/memcg_id，覆盖联合字段的第二个枚举分支。 */
	damos_test_commit_quota_goal_for(test, &dst,
			&(struct damos_quota_goal){
			.metric = DAMOS_QUOTA_NODE_MEMCG_FREE_BP,
			.target_value = 890,
			.current_value = 901,
			.nid = 10,
			.memcg_id = 1,
			});
	/* 最后一轮切回 PSI，覆盖无节点专属字段的 default 分支。 */
	/* src 的 last_psi_total 是运行期字段，不属于本轮配置复制期望。 */
	damos_test_commit_quota_goal_for(test, &dst,
			&(struct damos_quota_goal) {
			.metric = DAMOS_QUOTA_SOME_MEM_PSI_US,
			.target_value = 234,
			.current_value = 345,
			.last_psi_total = 567,
			});
}

/*
 * 业务背景：列表提交 helper 构造可释放的 dst goals 与借用 src goals，验证
 * damos_commit_quota_goals() 对新增、替换和删除的最终顺序/数量。
 * 入参：test 为 KUnit 上下文；dst/src_goals 及数量是调用期借用数组。
 * 出参/返回：无；所有动态 dst goal 在 out 标签销毁，分配失败清理后跳过。
 * 注意事项：src 直接链接复合字面量成员，提交函数不得释放 src；dst 多余节点会被
 * 被测函数 kfree，因此必须通过 damos_new_quota_goal() 动态创建。
 */
static void damos_test_commit_quota_goals_for(struct kunit *test,
		struct damos_quota_goal *dst_goals, int nr_dst_goals,
		struct damos_quota_goal *src_goals, int nr_src_goals)
{
	struct damos_quota dst, src;
	struct damos_quota_goal *goal, *next;
	bool skip = true;
	int i;

	INIT_LIST_HEAD(&dst.goals);
	INIT_LIST_HEAD(&src.goals);

	/* 阶段一：为 dst 复制成真正堆对象，以覆盖“src 更短时删除多余目标”。 */
	for (i = 0; i < nr_dst_goals; i++) {
		/*
		 * When nr_src_goals is smaller than dst_goals,
		 * damos_commit_quota_goals() will kfree() the dst goals.
		 * Make it kfree()-able.
		 */
		/* src 数量少于 dst 时提交函数会 kfree 多余 dst，因此这里必须使其可释放。 */
		goal = damos_new_quota_goal(dst_goals[i].metric,
				dst_goals[i].target_value);
		if (!goal)
			goto out;
		damos_add_quota_goal(&dst, goal);
	}
	/* 阶段二：src 节点只临时借用数组元素；提交后核对 dst 与 src 同序等长。 */
	skip = false;
	for (i = 0; i < nr_src_goals; i++)
		damos_add_quota_goal(&src, &src_goals[i]);

	damos_commit_quota_goals(&dst, &src);

	/* 提交结果必须与 src 一一对应，随后总节点数也由遍历索引验证。 */
	i = 0;
	damos_for_each_quota_goal(goal, (&dst)) {
		KUNIT_EXPECT_EQ(test, goal->metric, src_goals[i].metric);
		KUNIT_EXPECT_EQ(test, goal->target_value,
				src_goals[i++].target_value);
	}
	KUNIT_EXPECT_EQ(test, i, nr_src_goals);

out:
	/* 成功和 OOM 都在此释放 dst 残余动态节点；skip 决定是否报告用例跳过。 */
	damos_for_each_quota_goal_safe(goal, next, (&dst))
		damos_destroy_quota_goal(goal);
	if (skip)
		kunit_skip(test, "goal alloc fail");
}

/*
 * 业务背景：注册用例用“空→一项”“一项→一项”“一项→空”覆盖 quota goal
 * 列表提交的追加、更新和删除路径。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；各复合字面量仅同步借用。
 * 注意事项：具体 ownership/清理由 `_for` helper 统一验证。
 */
static void damos_test_commit_quota_goals(struct kunit *test)
{
	damos_test_commit_quota_goals_for(test,
			(struct damos_quota_goal[]){}, 0,
			/* 空 dst 从单项 src 新建第一个动态 goal。 */
			(struct damos_quota_goal[]){
				{
				.metric = DAMOS_QUOTA_USER_INPUT,
				.target_value = 123,
				},
			}, 1);
	/* 等长提交更新已有目标值，不增加或删除节点。 */
	damos_test_commit_quota_goals_for(test,
			(struct damos_quota_goal[]){
				{
				.metric = DAMOS_QUOTA_USER_INPUT,
				.target_value = 234,
				},

			}, 1,
			/* src 仍为一项，但内容应覆盖而不是追加到 dst。 */
			(struct damos_quota_goal[]){
				{
				.metric = DAMOS_QUOTA_USER_INPUT,
				.target_value = 345,
				},
			}, 1);
	/* 空 src 删除 dst 的唯一动态 goal，覆盖被测 kfree 路径。 */
	damos_test_commit_quota_goals_for(test,
			(struct damos_quota_goal[]){
				{
				.metric = DAMOS_QUOTA_USER_INPUT,
				.target_value = 456,
				},

			}, 1,
			(struct damos_quota_goal[]){}, 0);
}

/*
 * 业务背景：KUnit 验证 damos_commit_quota() 把周期、时间/大小额度、调谐器、失败
 * 记账比例和三类权重从 src 完整复制到 dst。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；两个 quota 均为栈对象。
 * 注意事项：goals 链表先初始化为空，目标列表的专门提交已由相邻用例验证；
 * CONSIST 与 TEMPORAL 分别代表一致性和时序 goal tuner 配置值。
 */
static void damos_test_commit_quota(struct kunit *test)
{
	/* dst 使用旧配置，提交后这些哨兵值应被 src 的不同值完全覆盖。 */
	struct damos_quota dst = {
		.reset_interval = 1,
		.ms = 2,
		.sz = 3,
		.goal_tuner = DAMOS_QUOTA_GOAL_TUNER_CONSIST,
		.fail_charge_num = 2,
		/* 分母及三种权重也都使用与 src 不同的哨兵值。 */
		.fail_charge_denom = 3,
		.weight_sz = 4,
		.weight_nr_accesses = 5,
		.weight_age = 6,
	};
	/* src 覆盖另一调谐器及所有额度/权重字段。 */
	struct damos_quota src = {
		.reset_interval = 7,
		.ms = 8,
		.sz = 9,
		.goal_tuner = DAMOS_QUOTA_GOAL_TUNER_TEMPORAL,
		.fail_charge_num = 1,
		/* 1024 分母覆盖非默认比例，三个权重分别对应大小、访问和 age。 */
		.fail_charge_denom = 1024,
		.weight_sz = 10,
		.weight_nr_accesses = 11,
		.weight_age = 12,
	};

	/* 两个 goals 头必须先为空环，避免提交把未初始化链表当作已有节点。 */
	INIT_LIST_HEAD(&dst.goals);
	INIT_LIST_HEAD(&src.goals);

	/* 被测提交后逐字段比较，防止新增配置字段遗漏复制。 */
	damos_commit_quota(&dst, &src);

	KUNIT_EXPECT_EQ(test, dst.reset_interval, src.reset_interval);
	KUNIT_EXPECT_EQ(test, dst.ms, src.ms);
	KUNIT_EXPECT_EQ(test, dst.sz, src.sz);
	KUNIT_EXPECT_EQ(test, dst.goal_tuner, src.goal_tuner);
	KUNIT_EXPECT_EQ(test, dst.fail_charge_num, src.fail_charge_num);
	KUNIT_EXPECT_EQ(test, dst.fail_charge_denom, src.fail_charge_denom);
	/* 三种权重分别影响大小、访问频率和 age 的 quota 优先级。 */
	KUNIT_EXPECT_EQ(test, dst.weight_sz, src.weight_sz);
	KUNIT_EXPECT_EQ(test, dst.weight_nr_accesses, src.weight_nr_accesses);
	KUNIT_EXPECT_EQ(test, dst.weight_age, src.weight_age);
}

/*
 * 业务背景：迁移目的地提交测试需要拥有型 node_id/weight 数组，本 helper 按
 * nr_dests 分配并从调用者数组复制。
 * 入参：dests 为输出对象；node_id_arr/weight_arr 为各 nr_dests 项的只读借用数组；
 * nr_dests 为元素数。出参/返回：0 并把两个数组 ownership 交给 dests，或 -ENOMEM
 * 且不遗留拥有资源。注意事项：第二次数组失败会释放第一数组并清 NULL。
 */
static int damos_test_help_dests_setup(struct damos_migrate_dests *dests,
		unsigned int *node_id_arr, unsigned int *weight_arr,
		size_t nr_dests)
{
	size_t i;

	/* 两个并行数组必须要么同时有效，要么失败时不留下半初始化 ownership。 */
	dests->node_id_arr = kmalloc_objs(*dests->node_id_arr, nr_dests);
	if (!dests->node_id_arr)
		return -ENOMEM;
	dests->weight_arr = kmalloc_objs(*dests->weight_arr, nr_dests);
	if (!dests->weight_arr) {
		kfree(dests->node_id_arr);
		dests->node_id_arr = NULL;
		return -ENOMEM;
	}

	/* 复制后 nr_dests 才发布，保证消费者不会看见尚未填满的数组。 */
	for (i = 0; i < nr_dests; i++) {
		dests->node_id_arr[i] = node_id_arr[i];
		dests->weight_arr[i] = weight_arr[i];
	}
	dests->nr_dests = nr_dests;
	return 0;
}

/*
 * 业务背景：与 setup 配对，释放 damos_migrate_dests 测试对象持有的两个堆数组。
 * 入参：dests 为拥有 node_id_arr/weight_arr 的非空对象；数组可为 NULL。
 * 出参/返回：无；数组 ownership 被消费。注意事项：不清字段，调用后不得重复释放
 * 或继续读取，外层测试只在对象生命周期结束时调用。
 */
static void damos_test_help_dests_free(struct damos_migrate_dests *dests)
{
	kfree(dests->node_id_arr);
	kfree(dests->weight_arr);
}

/*
 * 业务背景：表驱动 helper 为 dst/src 建立迁移目的地数组，调用 damos_commit_dests()
 * 后核对数量、节点 id 和权重，覆盖重新分配与清空路径。
 * 入参：test 为 KUnit 上下文；两组 node/weight 数组及数量均为调用期借用。
 * 出参/返回：无；两侧动态数组统一释放；setup/commit 失败清理后跳过。
 * 注意事项：skip 区分提交成功与错误出口，err 只控制资源回滚，不返回给调用者。
 */
static void damos_test_commit_dests_for(struct kunit *test,
		unsigned int *dst_node_id_arr, unsigned int *dst_weight_arr,
		size_t dst_nr_dests,
		unsigned int *src_node_id_arr, unsigned int *src_weight_arr,
		size_t src_nr_dests)
{
	struct damos_migrate_dests dst = {}, src = {};
	int i, err;
	bool skip = true;

	/* 阶段一：分别构造拥有型 dst/src；src 失败先释放已成功的 dst。 */
	err = damos_test_help_dests_setup(&dst, dst_node_id_arr,
			dst_weight_arr, dst_nr_dests);
	if (err)
		kunit_skip(test, "dests setup fail");
	err = damos_test_help_dests_setup(&src, src_node_id_arr,
			src_weight_arr, src_nr_dests);
	if (err) {
		damos_test_help_dests_free(&dst);
		kunit_skip(test, "src setup fail");
	}
	/* 阶段二：提交可能重新分配 dst；成功后所有元素必须精确等于 src。 */
	err = damos_commit_dests(&dst, &src);
	if (err)
		goto out;
	skip = false;

	/* 数量先匹配，再逐元素验证两个并行数组保持相同索引语义。 */
	KUNIT_EXPECT_EQ(test, dst.nr_dests, src_nr_dests);
	for (i = 0; i < dst.nr_dests; i++) {
		KUNIT_EXPECT_EQ(test, dst.node_id_arr[i], src_node_id_arr[i]);
		KUNIT_EXPECT_EQ(test, dst.weight_arr[i], src_weight_arr[i]);
	}

out:
	/* 无论提交是否成功，两对象当前持有的数组都由测试回收。 */
	damos_test_help_dests_free(&dst);
	damos_test_help_dests_free(&src);
	if (skip)
		kunit_skip(test, "skip");
}

/*
 * 业务背景：注册用例覆盖迁移 destinations 的等长替换、扩容、空→非空、缩容和
 * 非空→空五类提交。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；每个子场景由 helper 清理。
 * 注意事项：NULL 与数量 0 成对表示空数组，复合字面量只同步借用。
 */
static void damos_test_commit_dests(struct kunit *test)
{
	/* 等长 3→3：覆盖原地替换。 */
	damos_test_commit_dests_for(test,
			(unsigned int[]){1, 2, 3}, (unsigned int[]){2, 3, 4},
			3,
			(unsigned int[]){4, 5, 6}, (unsigned int[]){5, 6, 7},
			3);
	/* 2→3 与空→3：分别覆盖扩容及首次分配。 */
	damos_test_commit_dests_for(test,
			(unsigned int[]){1, 2}, (unsigned int[]){2, 3},
			2,
			(unsigned int[]){4, 5, 6}, (unsigned int[]){5, 6, 7},
			3);
	damos_test_commit_dests_for(test,
			NULL, NULL, 0,
			(unsigned int[]){4, 5, 6}, (unsigned int[]){5, 6, 7},
			3);
	/* 3→2 与 3→空：分别覆盖缩容及完整清空。 */
	damos_test_commit_dests_for(test,
			(unsigned int[]){1, 2, 3}, (unsigned int[]){2, 3, 4},
			3,
			(unsigned int[]){4, 5}, (unsigned int[]){5, 6}, 2);
	damos_test_commit_dests_for(test,
			(unsigned int[]){1, 2, 3}, (unsigned int[]){2, 3, 4},
			3,
			NULL, NULL, 0);
}

/*
 * 业务背景：filter 表驱动 helper 提交通用 type/matching/allow，并按过滤类型核对
 * memcg、地址、target 或大页大小专属联合字段。
 * 入参：test 为 KUnit 上下文；dst 为输入输出过滤器；src 为只读借用配置。
 * 出参/返回：无；dst 就地更新，无 ownership 转移。
 * 注意事项：只有对应 type 的联合成员有效，default 类型不读取专属字段。
 */
static void damos_test_commit_filter_for(struct kunit *test,
		struct damos_filter *dst, struct damos_filter *src)
{
	damos_commit_filter(dst, src);
	KUNIT_EXPECT_EQ(test, dst->type, src->type);
	KUNIT_EXPECT_EQ(test, dst->matching, src->matching);
	KUNIT_EXPECT_EQ(test, dst->allow, src->allow);
	/* 每个 case 都验证该枚举值的产生配置被提交给相应消费者字段。 */
	switch (src->type) {
	case DAMOS_FILTER_TYPE_MEMCG:
		/* MEMCG 用 memcg_id 选择控制组，提交后过滤匹配该组页面。 */
		KUNIT_EXPECT_EQ(test, dst->memcg_id, src->memcg_id);
		break;
	case DAMOS_FILTER_TYPE_ADDR:
		/* ADDR 用半开 addr_range 限定地址，start/end 必须一起复制。 */
		KUNIT_EXPECT_EQ(test, dst->addr_range.start,
				src->addr_range.start);
		KUNIT_EXPECT_EQ(test, dst->addr_range.end,
				src->addr_range.end);
		break;
	case DAMOS_FILTER_TYPE_TARGET:
		/* TARGET 用 target_idx 选择 DAMON target。 */
		KUNIT_EXPECT_EQ(test, dst->target_idx, src->target_idx);
		break;
	case DAMOS_FILTER_TYPE_HUGEPAGE_SIZE:
		/* HUGEPAGE_SIZE 用 min/max 字节范围筛选大页尺寸。 */
		KUNIT_EXPECT_EQ(test, dst->sz_range.min, src->sz_range.min);
		KUNIT_EXPECT_EQ(test, dst->sz_range.max, src->sz_range.max);
		break;
	default:
		/* ANON/YOUNG/ACTIVE/UNMAPPED 等布尔属性类型没有附加联合字段。 */
		break;
	}
}

/*
 * 业务背景：注册用例依次提交 ANON、MEMCG、YOUNG、HUGEPAGE_SIZE、UNMAPPED、
 * ADDR、TARGET，覆盖 filter 通用和全部专属字段复制。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；dst 与 src 复合字面量均在栈上。
 * 注意事项：matching 决定匹配/不匹配语义，allow 决定允许/拒绝动作，helper 逐次验证。
 */
static void damos_test_commit_filter(struct kunit *test)
{
	struct damos_filter dst = {
		.type = DAMOS_FILTER_TYPE_ACTIVE,
		.matching = false,
		.allow = false,
	};

	/* 无联合字段的 ANON/YOUNG/UNMAPPED 主要验证 type 与两个布尔策略。 */
	damos_test_commit_filter_for(test, &dst,
			&(struct damos_filter){
			.type = DAMOS_FILTER_TYPE_ANON,
			.matching = true,
			.allow = true,
			});
	/* MEMCG 额外携带 memcg_id。 */
	damos_test_commit_filter_for(test, &dst,
			&(struct damos_filter){
			.type = DAMOS_FILTER_TYPE_MEMCG,
			.matching = false,
			.allow = false,
			.memcg_id = 123,
			});
	/* YOUNG 只依赖布尔属性，没有联合字段。 */
	damos_test_commit_filter_for(test, &dst,
			&(struct damos_filter){
			.type = DAMOS_FILTER_TYPE_YOUNG,
			.matching = true,
			.allow = true,
			});
	/* 大页尺寸、地址区间和 target 索引分别覆盖三种联合布局。 */
	damos_test_commit_filter_for(test, &dst,
			&(struct damos_filter){
			.type = DAMOS_FILTER_TYPE_HUGEPAGE_SIZE,
			.matching = false,
			.allow = false,
			.sz_range = {.min = 234, .max = 345},
			});
	/* UNMAPPED 回到无联合载荷类型，检查前一尺寸范围不会改变通用字段。 */
	damos_test_commit_filter_for(test, &dst,
			&(struct damos_filter){
			.type = DAMOS_FILTER_TYPE_UNMAPPED,
			.matching = true,
			.allow = true,
			});
	/* ADDR 再切换到半开地址区间载荷。 */
	damos_test_commit_filter_for(test, &dst,
			&(struct damos_filter){
			.type = DAMOS_FILTER_TYPE_ADDR,
			.matching = false,
			.allow = false,
			.addr_range = {.start = 456, .end = 567},
			});
	/* TARGET 以索引 6 选择某个监控目标。 */
	damos_test_commit_filter_for(test, &dst,
			&(struct damos_filter){
			.type = DAMOS_FILTER_TYPE_TARGET,
			.matching = true,
			.allow = true,
			.target_idx = 6,
			});
}

/*
 * 业务背景：damos_commit() 测试前必须把 scheme 内三个拥有节点的链表初始化为空，
 * 使提交/销毁 helper 可安全遍历。
 * 入参：scheme 为可写借用对象。出参/返回：无；初始化 quota goals、core filters、
 * ops filters 三个空链表。注意事项：仅对尚无节点的测试对象调用，不释放旧链表。
 */
static void damos_test_help_initailize_scheme(struct damos *scheme)
{
	INIT_LIST_HEAD(&scheme->quota.goals);
	INIT_LIST_HEAD(&scheme->core_filters);
	INIT_LIST_HEAD(&scheme->ops_filters);
}

/*
 * 业务背景：通用 scheme 提交 helper 初始化两侧链表，调用 damos_commit()，逐项
 * 核对 access pattern、action、apply interval、watermarks 和迁移 action 的 target_nid。
 * 入参：test 为 KUnit 上下文；dst 为输入输出 scheme；src 为借用源配置。
 * 出参/返回：无；提交 OOM 时跳过，成功后 dst 配置等于 src。
 * 注意事项：MIGRATE_COLD/HOT 才消费 target_nid；本 helper 不接管栈对象 ownership。
 */
static void damos_test_commit_for(struct kunit *test, struct damos *dst,
		struct damos *src)
{
	int err;

	damos_test_help_initailize_scheme(dst);
	damos_test_help_initailize_scheme(src);

	/* 提交会复制嵌套配置并可能分配链表节点；失败由 KUnit 标记跳过。 */
	err = damos_commit(dst, src);
	if (err)
		kunit_skip(test, "damos_commit fail");

	/* 阶段一：六个 pattern 边界逐项相等，单位分别为字节、次数和聚合 age。 */
	KUNIT_EXPECT_EQ(test, dst->pattern.min_sz_region,
			src->pattern.min_sz_region);
	KUNIT_EXPECT_EQ(test, dst->pattern.max_sz_region,
			src->pattern.max_sz_region);
	KUNIT_EXPECT_EQ(test, dst->pattern.min_nr_accesses,
			src->pattern.min_nr_accesses);
	/* age 下界/上界与访问次数边界分开比较，避免相邻字段错位复制。 */
	KUNIT_EXPECT_EQ(test, dst->pattern.max_nr_accesses,
			src->pattern.max_nr_accesses);
	KUNIT_EXPECT_EQ(test, dst->pattern.min_age_region,
			src->pattern.min_age_region);
	KUNIT_EXPECT_EQ(test, dst->pattern.max_age_region,
			src->pattern.max_age_region);

	/* 阶段二：通用 action 与应用周期，再核对 watermark metric/周期/三阈值。 */
	KUNIT_EXPECT_EQ(test, dst->action, src->action);
	KUNIT_EXPECT_EQ(test, dst->apply_interval_us, src->apply_interval_us);

	KUNIT_EXPECT_EQ(test, dst->wmarks.metric, src->wmarks.metric);
	KUNIT_EXPECT_EQ(test, dst->wmarks.interval, src->wmarks.interval);
	KUNIT_EXPECT_EQ(test, dst->wmarks.high, src->wmarks.high);
	KUNIT_EXPECT_EQ(test, dst->wmarks.mid, src->wmarks.mid);
	KUNIT_EXPECT_EQ(test, dst->wmarks.low, src->wmarks.low);

	/* action 决定是否存在迁移目的 NUMA 节点这一专属配置。 */
	switch (src->action) {
	case DAMOS_MIGRATE_COLD:
	case DAMOS_MIGRATE_HOT:
		/* 冷/热页迁移都由 target_nid 指定目标节点，提交必须复制。 */
		KUNIT_EXPECT_EQ(test, dst->target_nid, src->target_nid);
		break;
	default:
		/* PAGEOUT 等非迁移动作不消费 target_nid。 */
		break;
	}
}

/*
 * 业务背景：注册用例以 PAGEOUT scheme 验证通用 pattern、微秒单位应用间隔及
 * FREE_MEM_RATE 检查周期及 high/mid/low 水位线从 src 覆盖 dst。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；两个 scheme 为复合字面量。
 * 注意事项：PAGEOUT 不使用 target_nid，具体逐字段断言由 commit helper 完成。
 */
static void damos_test_commit_pageout(struct kunit *test)
{
	damos_test_commit_for(test,
			/* dst 故意使用不同 pattern/周期/水位，确保提交确实覆盖。 */
			&(struct damos){
				.pattern = (struct damos_access_pattern){
					1, 2, 3, 4, 5, 6},
				.action = DAMOS_PAGEOUT,
				.apply_interval_us = 1000000,
				.wmarks = (struct damos_watermarks){
					DAMOS_WMARK_FREE_MEM_RATE,
					900, 100, 50},
			},
			/* src 的 watermark 周期为 800，high/mid 为 50/30，low 缺省为 0。 */
			&(struct damos){
				.pattern = (struct damos_access_pattern){
					2, 3, 4, 5, 6, 7},
				.action = DAMOS_PAGEOUT,
				.apply_interval_us = 2000000,
				.wmarks = (struct damos_watermarks){
					DAMOS_WMARK_FREE_MEM_RATE,
					800, 50, 30},
			});
}

/*
 * 业务背景：注册用例把 PAGEOUT dst 提交为 MIGRATE_HOT src，验证 action 改变及
 * 迁移专属 target_nid=5 被复制。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；复合字面量同步消费。
 * 注意事项：MIGRATE_HOT 表示把热页迁往目标 NUMA 节点，与相邻 PAGEOUT 场景互补。
 */
static void damos_test_commit_migrate_hot(struct kunit *test)
{
	damos_test_commit_for(test,
			/* dst 先为 PAGEOUT，提交必须改变 action。 */
			&(struct damos){
				.pattern = (struct damos_access_pattern){
					1, 2, 3, 4, 5, 6},
				.action = DAMOS_PAGEOUT,
				.apply_interval_us = 1000000,
				.wmarks = (struct damos_watermarks){
					DAMOS_WMARK_FREE_MEM_RATE,
					900, 100, 50},
			},
			/* src 为热页迁移，target_nid=5 是该 action 的有效专属字段。 */
			&(struct damos){
				.pattern = (struct damos_access_pattern){
					2, 3, 4, 5, 6, 7},
				.action = DAMOS_MIGRATE_HOT,
				.apply_interval_us = 2000000,
				.target_nid = 5,
			});
}

/*
 * 业务背景：target-region 提交用例需要从二维 [start,end] 数组构造拥有 region 的 target。
 * 入参：region_start_end 为 nr_regions×2 的只读借用数组；nr_regions 为元素数。
 * 出参/返回：成功返回拥有全部 region 的 target；OOM 返回 NULL 并释放已建对象。
 * 注意事项：返回 ownership 交给调用者并须 damon_free_target()；区间按输入顺序链接。
 */
static struct damon_target *damon_test_help_setup_target(
		unsigned long region_start_end[][2], int nr_regions)
{
	struct damon_target *t;
	struct damon_region *r;
	int i;

	/* target 创建成功后逐段转移 region ownership；任一失败递归清理。 */
	t = damon_new_target();
	if (!t)
		return NULL;
	/* 每个二维元素给出半开区间两端，创建后立即加入 target 链表。 */
	for (i = 0; i < nr_regions; i++) {
		r = damon_new_region(region_start_end[i][0],
				region_start_end[i][1]);
		if (!r) {
			damon_free_target(t);
			return NULL;
		}
		/* ownership 在本次加入后转移给 t，后续失败可由 t 一次清理。 */
		damon_add_region(r, t);
	}
	return t;
}

/*
 * 业务背景：表驱动 helper 构造 dst/src target，调用 damon_commit_target_regions()
 * 后逐段核对地址和总数。
 * 入参：test 为 KUnit 上下文；dst/src/expect 二维数组及各自数量均为只读借用；
 * min_region_sz 固定由调用点传 1。出参/返回：无；两个 target 最终释放。
 * 注意事项：src 构造失败先释放 dst；结果数量与遍历次数双重验证防链表损坏。
 */
static void damon_test_commit_target_regions_for(struct kunit *test,
		unsigned long dst_start_end[][2], int nr_dst_regions,
		unsigned long src_start_end[][2], int nr_src_regions,
		unsigned long expect_start_end[][2], int nr_expect_regions)
{
	struct damon_target *dst_target, *src_target;
	struct damon_region *r;
	int i;

	/* 先构造可被就地修改的 dst，再构造仅作为配置来源的独立 src。 */
	dst_target = damon_test_help_setup_target(dst_start_end, nr_dst_regions);
	if (!dst_target)
		kunit_skip(test, "dst target setup fail");
	/* src 失败时 dst 已有 ownership，必须在跳过用例前释放。 */
	src_target = damon_test_help_setup_target(src_start_end, nr_src_regions);
	if (!src_target) {
		damon_free_target(dst_target);
		kunit_skip(test, "src target setup fail");
	}
	/* min_region_sz=1 允许精确复现 src 地址，不因对齐扩大或裁剪。 */
	/* 被测提交按 src 范围重塑 dst；随后仅从 dst 读取结果，src 仍独立持有自身节点。 */
	damon_commit_target_regions(dst_target, src_target, 1);
	i = 0;
	damon_for_each_region(r, dst_target) {
		KUNIT_EXPECT_EQ(test, r->ar.start, expect_start_end[i][0]);
		KUNIT_EXPECT_EQ(test, r->ar.end, expect_start_end[i][1]);
		i++;
	}
	/* API 计数与实际遍历计数都应等于 oracle，分别防缓存和链表错误。 */
	KUNIT_EXPECT_EQ(test, damon_nr_regions(dst_target), nr_expect_regions);
	KUNIT_EXPECT_EQ(test, i, nr_expect_regions);
	/* src/dst 从未共享 region，故可独立销毁且不发生双重释放。 */
	damon_free_target(dst_target);
	damon_free_target(src_target);
}

/*
 * 业务背景：注册用例覆盖“两个旧 region→一个新 region”和“空 src 保留旧布局”
 * 两种 target-region 提交语义。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；helper 负责全部 ownership。
 * 注意事项：二维复合字面量只在同步 helper 调用期间有效。
 */
static void damon_test_commit_target_regions(struct kunit *test)
{
	/* 非空 src 把两个旧段替换为 [4,6)。 */
	damon_test_commit_target_regions_for(test,
			(unsigned long[][2]) {{3, 8}, {8, 10}}, 2,
			(unsigned long[][2]) {{4, 6}}, 1,
			(unsigned long[][2]) {{4, 6}}, 1);
	/* 空 src 是“不提供新布局”，dst 两个旧段应保持不变。 */
	damon_test_commit_target_regions_for(test,
			(unsigned long[][2]) {{3, 8}, {8, 10}}, 2,
			(unsigned long[][2]) {}, 0,
			(unsigned long[][2]) {{3, 8}, {8, 10}}, 2);
}

/*
 * 业务背景：KUnit 验证 damon_commit_ctx() 只接受 2 的幂 min_region_sz，并能提交
 * pause 状态；覆盖成功、4095 非法和恢复合法后三次提交。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；src/dst ctx 均最终销毁。
 * 注意事项：dst 创建失败先销毁 src；两个 ctx 独立拥有内部资源。
 */
static void damon_test_commit_ctx(struct kunit *test)
{
	struct damon_ctx *src, *dst;

	/* src 是期望配置，dst 是提交目标；分别分配以验证真实复制。 */
	src = damon_new_ctx();
	if (!src)
		kunit_skip(test, "src alloc fail");
	/* 第二次分配失败时回收已经创建的 src。 */
	dst = damon_new_ctx();
	if (!dst) {
		damon_destroy_ctx(src);
		kunit_skip(test, "dst alloc fail");
	}
	/* Only power of two min_region_sz is allowed. */
	/* min_region_sz 只允许 2 的幂：4096 成功，4095 返回 -EINVAL。 */
	src->min_region_sz = 4096;
	KUNIT_EXPECT_EQ(test, damon_commit_ctx(dst, src), 0);
	src->min_region_sz = 4095;
	KUNIT_EXPECT_EQ(test, damon_commit_ctx(dst, src), -EINVAL);
	src->min_region_sz = 4096;
	src->pause = true;
	/* 恢复合法大小并设置 pause，提交后 dst 必须观察到暂停状态。 */
	KUNIT_EXPECT_EQ(test, damon_commit_ctx(dst, src), 0);
	KUNIT_EXPECT_TRUE(test, dst->pause);
	damon_destroy_ctx(src);
	damon_destroy_ctx(dst);
}

/*
 * 业务背景：KUnit 用 matching=true、allow=false 的 ADDR filter [2,6) 验证 region 位于范围内、前后、
 * 跨入起点和跨出终点时的匹配结果与自动分裂边界。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；filter/target/region 最终释放。
 * 注意事项：damos_filter_match() 可修改 target 拓扑；r2 是分裂后借用节点，显式
 * destroy 后原 r 继续用于下一场景。
 */
static void damos_test_filter_out(struct kunit *test)
{
	/* f 选择地址 [2,6)，r 为反复改边界的主节点，r2 接收临时分裂后继。 */
	struct damon_target *t;
	struct damon_region *r, *r2;
	struct damos_filter *f;

	/* matching=true 且 allow=false 表示命中 [2,6) 时过滤掉该部分。 */
	f = damos_new_filter(DAMOS_FILTER_TYPE_ADDR, true, false);
	if (!f)
		kunit_skip(test, "filter alloc fail");
	f->addr_range = (struct damon_addr_range){.start = 2, .end = 6};

	/* target 为 filter_match 可能执行的原地拆分提供链表容器。 */
	t = damon_new_target();
	if (!t) {
		damos_destroy_filter(f);
		kunit_skip(test, "target alloc fail");
	}
	/* 初始段完整落入过滤范围，先建立不需要拆分的正例。 */
	r = damon_new_region(3, 5);
	if (!r) {
		damos_destroy_filter(f);
		damon_free_target(t);
		kunit_skip(test, "region alloc fail");
	}
	damon_add_region(r, t);

	/* region in the range */
	/* 完全位于过滤范围内：匹配为真，节点数保持 1。 */
	KUNIT_EXPECT_TRUE(test, damos_filter_match(NULL, t, r, f, 1));
	KUNIT_EXPECT_EQ(test, damon_nr_regions(t), 1);

	/* region before the range */
	/* 完全位于范围前：不匹配且不分裂。 */
	r->ar.start = 1;
	r->ar.end = 2;
	KUNIT_EXPECT_FALSE(test,
			damos_filter_match(NULL, t, r, f, 1));
	KUNIT_EXPECT_EQ(test, damon_nr_regions(t), 1);

	/* region after the range */
	/* 完全位于范围后：不匹配且不分裂。 */
	r->ar.start = 6;
	r->ar.end = 8;
	KUNIT_EXPECT_FALSE(test,
			damos_filter_match(NULL, t, r, f, 1));
	KUNIT_EXPECT_EQ(test, damon_nr_regions(t), 1);

	/* region started before the range */
	/* 从范围前跨入时，原 r 保留 [1,2)，匹配部分拆成后继 [2,4)。 */
	r->ar.start = 1;
	r->ar.end = 4;
	KUNIT_EXPECT_FALSE(test, damos_filter_match(NULL, t, r, f, 1));
	/* filter should have split the region */
	/* 分裂使节点数变 2；销毁临时后继后恢复单节点继续测试。 */
	KUNIT_EXPECT_EQ(test, r->ar.start, 1);
	KUNIT_EXPECT_EQ(test, r->ar.end, 2);
	KUNIT_EXPECT_EQ(test, damon_nr_regions(t), 2);
	r2 = damon_next_region(r);
	KUNIT_EXPECT_EQ(test, r2->ar.start, 2);
	KUNIT_EXPECT_EQ(test, r2->ar.end, 4);
	damon_destroy_region(r2, t);

	/* region started in the range */
	/* 起点在范围内但尾端越界：匹配 [2,6)，并拆出不匹配后继 [6,8)。 */
	r->ar.start = 2;
	r->ar.end = 8;
	KUNIT_EXPECT_TRUE(test,
			damos_filter_match(NULL, t, r, f, 1));
	/* filter should have split the region */
	/* 再次验证分裂边界与节点数，然后清理后继及整个 target。 */
	KUNIT_EXPECT_EQ(test, r->ar.start, 2);
	KUNIT_EXPECT_EQ(test, r->ar.end, 6);
	KUNIT_EXPECT_EQ(test, damon_nr_regions(t), 2);
	r2 = damon_next_region(r);
	KUNIT_EXPECT_EQ(test, r2->ar.start, 6);
	KUNIT_EXPECT_EQ(test, r2->ar.end, 8);
	damon_destroy_region(r2, t);

	damon_free_target(t);
	damos_free_filter(f);
}

/*
 * 业务背景：KUnit 验证 DAMON 反馈环以固定目标 10000 调整下一输入：当前分数低于
 * 目标时提高输入，高于目标时降低，偏差越大调整幅度越大。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；仅执行纯算术断言。
 * 注意事项：last_input/current_score 是无符号控制量，无共享状态或 ownership。
 */
static void damon_test_feed_loop_next_input(struct kunit *test)
{
	unsigned long last_input = 900000, current_score = 200;

	/*
	 * If current score is lower than the goal, which is always 10,000
	 * (read the comment on damon_feed_loop_next_input()'s comment), next
	 * input should be higher than the last input.
	 */
	/* 当前分数低于固定目标 10000（详见被测函数注释），下一输入应高于上次值。 */
	KUNIT_EXPECT_GT(test,
			damon_feed_loop_next_input(last_input, current_score),
			last_input);

	/*
	 * If current score is higher than the goal, next input should be lower
	 * than the last input.
	 */
	/* 当前分数高于目标时，负反馈应降低下一输入。 */
	current_score = 250000000;
	KUNIT_EXPECT_LT(test,
			damon_feed_loop_next_input(last_input, current_score),
			last_input);

	/*
	 * The next input depends on the distance between the current score and
	 * the goal
	 */
	/* 下一输入还取决于当前分数与目标距离：200 的偏差大于 2000，提升应更大。 */
	KUNIT_EXPECT_GT(test,
			damon_feed_loop_next_input(last_input, 200),
			damon_feed_loop_next_input(last_input, 2000));
}

/*
 * 业务背景：KUnit 验证 core/ops 两层 filter 的 default_reject 推导，依次覆盖无
 * filter、core allow、core reject、core reject+ops allow、core allow+ops allow。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；两个动态 filter 最终释放。
 * 注意事项：filter 加入 scheme 后链表借用其节点，但本栈 scheme 不执行销毁，故测试末显式 free。
 */
static void damon_test_set_filters_default_reject(struct kunit *test)
{
	struct damos scheme;
	struct damos_filter *target_filter, *anon_filter;

	INIT_LIST_HEAD(&scheme.core_filters);
	INIT_LIST_HEAD(&scheme.ops_filters);

	damos_set_filters_default_reject(&scheme);
	/*
	 * No filter is installed.  Allow by default on both core and ops layer
	 * filtering stages, since there are no filters at all.
	 */
	/* 没有任何 filter 时，两层都默认允许，即 default_reject=false。 */
	KUNIT_EXPECT_EQ(test, scheme.core_filters_default_reject, false);
	KUNIT_EXPECT_EQ(test, scheme.ops_filters_default_reject, false);

	target_filter = damos_new_filter(DAMOS_FILTER_TYPE_TARGET, true, true);
	if (!target_filter)
		kunit_skip(test, "filter alloc fail");
	damos_add_filter(&scheme, target_filter);
	damos_set_filters_default_reject(&scheme);
	/*
	 * A core-handled allow-filter is installed.
	 * Reject by default on core layer filtering stage due to the last
	 * core-layer-filter's behavior.
	 * Allow by default on ops layer filtering stage due to the absence of
	 * ops layer filters.
	 */
	/* 仅 core 层末项为 allow：core 默认拒绝以形成白名单，ops 因为空而默认允许。 */
	KUNIT_EXPECT_EQ(test, scheme.core_filters_default_reject, true);
	KUNIT_EXPECT_EQ(test, scheme.ops_filters_default_reject, false);

	target_filter->allow = false;
	damos_set_filters_default_reject(&scheme);
	/*
	 * A core-handled reject-filter is installed.
	 * Allow by default on core layer filtering stage due to the last
	 * core-layer-filter's behavior.
	 * Allow by default on ops layer filtering stage due to the absence of
	 * ops layer filters.
	 */
	/* core 末项改为 reject：core 默认允许以形成黑名单，ops 仍默认允许。 */
	KUNIT_EXPECT_EQ(test, scheme.core_filters_default_reject, false);
	KUNIT_EXPECT_EQ(test, scheme.ops_filters_default_reject, false);

	anon_filter = damos_new_filter(DAMOS_FILTER_TYPE_ANON, true, true);
	if (!anon_filter) {
		damos_free_filter(target_filter);
		kunit_skip(test, "anon_filter alloc fail");
	}
	damos_add_filter(&scheme, anon_filter);

	damos_set_filters_default_reject(&scheme);
	/*
	 * A core-handled reject-filter and ops-handled allow-filter are installed.
	 * Allow by default on core layer filtering stage due to the existence
	 * of the ops-handled filter.
	 * Reject by default on ops layer filtering stage due to the last
	 * ops-layer-filter's behavior.
	 */
	/* 加入 ops allow 后，core 因后续层仍会筛选而默认允许，ops 末项 allow 导致默认拒绝。 */
	KUNIT_EXPECT_EQ(test, scheme.core_filters_default_reject, false);
	KUNIT_EXPECT_EQ(test, scheme.ops_filters_default_reject, true);

	target_filter->allow = true;
	damos_set_filters_default_reject(&scheme);
	/*
	 * A core-handled allow-filter and ops-handled allow-filter are
	 * installed.
	 * Allow by default on core layer filtering stage due to the existence
	 * of the ops-handled filter.
	 * Reject by default on ops layer filtering stage due to the last
	 * ops-layer-filter's behavior.
	 */
	/* core filter 改回 allow 不改变跨层结论：core 默认允许，ops 默认拒绝。 */
	KUNIT_EXPECT_EQ(test, scheme.core_filters_default_reject, false);
	KUNIT_EXPECT_EQ(test, scheme.ops_filters_default_reject, true);

	damos_free_filter(anon_filter);
	damos_free_filter(target_filter);
}

/*
 * 业务背景：表驱动 helper 构造单 region ctx，调用 damon_apply_min_nr_regions()，
 * 核对返回的最大 region 大小和实际拆分数量。
 * 入参：test 为 KUnit 上下文；sz_regions/min_region_sz/max_region_sz_expect 为地址
 * 字节数，min_nr_regions/nr_regions_expect 为数量。出参/返回：无；ctx 递归清理。
 * 注意事项：ctx 持有 target、target 持有 region；任一 OOM 清理后跳过。
 */
static void damon_test_apply_min_nr_regions_for(struct kunit *test,
		unsigned long sz_regions, unsigned long min_region_sz,
		unsigned long min_nr_regions,
		unsigned long max_region_sz_expect,
		unsigned long nr_regions_expect)
{
	struct damon_ctx *ctx;
	struct damon_target *t;
	struct damon_region *r;
	unsigned long max_region_size;

	/* ctx 负责 attrs/min_region_sz，并最终递归拥有 target 与 region。 */
	ctx = damon_new_ctx();
	if (!ctx)
		kunit_skip(test, "ctx alloc fail\n");
	/* target 成功后立即交给 ctx，之后任何失败只销毁 ctx 即可。 */
	t = damon_new_target();
	if (!t) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "target alloc fail\n");
	}
	damon_add_target(ctx, t);
	/* 单段 [0,sz_regions) 是拆分输入，便于直接核对输出节点数量。 */
	r = damon_new_region(0, sz_regions);
	if (!r) {
		damon_destroy_ctx(ctx);
		kunit_skip(test, "region alloc fail\n");
	}
	damon_add_region(r, t);

	/* 被测函数综合最小大小与最小数量，可能就地拆分 target region。 */
	ctx->min_region_sz = min_region_sz;
	ctx->attrs.min_nr_regions = min_nr_regions;
	max_region_size = damon_apply_min_nr_regions(ctx);

	KUNIT_EXPECT_EQ(test, max_region_size, max_region_sz_expect);
	KUNIT_EXPECT_EQ(test, damon_nr_regions(t), nr_regions_expect);

	damon_destroy_ctx(ctx);
}

/*
 * 业务背景：注册用例覆盖普通 10 段、请求超过可拆上限、最大大小按 min_region_sz
 * 对齐，以及数量与最小大小冲突时最小大小优先。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；四个场景由 helper 独立清理。
 * 注意事项：所有数值都是小型确定性地址长度/数量，不依赖随机状态。
 */
static void damon_test_apply_min_nr_regions(struct kunit *test)
{
	/* common, expected setup */
	/* 常规：10 字节、最小 1、至少 10 段，结果十个 1 字节段。 */
	damon_test_apply_min_nr_regions_for(test, 10, 1, 10, 1, 10);
	/* no zero size limit */
	/* 请求 15 段也不能产生零长度段，最多仍为 10。 */
	damon_test_apply_min_nr_regions_for(test, 10, 1, 15, 1, 10);
	/* max size should be aligned by min_region_sz */
	/* 最小大小 2 时最大段长按 2 对齐，10 字节拆两段得到上界 6。 */
	damon_test_apply_min_nr_regions_for(test, 10, 2, 2, 6, 2);
	/*
	 * when min_nr_regions and min_region_sz conflicts, min_region_sz wins.
	 */
	/* 最小数量 10 与最小大小 2 冲突时，大小约束优先，只能得到 5 段。 */
	damon_test_apply_min_nr_regions_for(test, 10, 2, 10, 2, 5);
}

/*
 * 业务背景：KUnit 逐次追加四个 region，验证 damon_is_last_region() 对刚追加的
 * 尾节点始终为真。
 * 入参：test 为借用 KUnit 上下文。出参/返回：无；target 最终释放四个 region。
 * 注意事项：r 链入 t 后 ownership 转移；失败先释放 t 再跳过。
 */
static void damon_test_is_last_region(struct kunit *test)
{
	struct damon_region *r;
	struct damon_target *t;
	int i;

	/* 空 target 在每轮追加后都有唯一尾节点，可独立验证四次。 */
	t = damon_new_target();
	if (!t)
		kunit_skip(test, "target alloc fail\n");

	/* 相邻的等长半开区间保证“最后”判断只依赖链表位置。 */
	for (i = 0; i < 4; i++) {
		r = damon_new_region(i * 2, (i + 1) * 2);
		if (!r) {
			damon_free_target(t);
			kunit_skip(test, "region alloc %d fail\n", i);
		}
		/* 新节点加入尾部后应立即成为当前最后节点。 */
		damon_add_region(r, t);
		KUNIT_EXPECT_TRUE(test, damon_is_last_region(r, t));
	}
	damon_free_target(t);
}

/*
 * 用例表把每个 `damon_test_*`/`damos_test_*` 入口交给 KUnit；KUNIT_CASE 保存函数
 * 指针和名称，末尾空项是框架遍历哨兵。顺序按 region/ctx 基础、属性换算、DAMOS
 * commit/filter、反馈与最小 region 约束组织；表为静态只读注册数据，存活至模块卸载。
 */
static struct kunit_case damon_test_cases[] = {
	/* 基础对象、聚合以及 region 分裂/合并/重设。 */
	KUNIT_CASE(damon_test_target),
	KUNIT_CASE(damon_test_regions),
	KUNIT_CASE(damon_test_aggregate),
	KUNIT_CASE(damon_test_split_at),
	KUNIT_CASE(damon_test_merge_two),
	KUNIT_CASE(damon_test_merge_regions_of),
	KUNIT_CASE(damon_test_split_regions_of),
	KUNIT_CASE(damon_test_ops_registration),
	KUNIT_CASE(damon_test_set_regions),
	/* 监控结果单位换算、属性校验和移动和。 */
	KUNIT_CASE(damon_test_nr_accesses_to_accesses_bp),
	KUNIT_CASE(damon_test_update_monitoring_result),
	KUNIT_CASE(damon_test_set_attrs),
	KUNIT_CASE(damon_test_moving_sum),
	/* DAMOS filter、quota 目标/整体、迁移目的和 scheme 提交。 */
	KUNIT_CASE(damos_test_new_filter),
	KUNIT_CASE(damos_test_commit_quota_goal),
	KUNIT_CASE(damos_test_commit_quota_goals),
	KUNIT_CASE(damos_test_commit_quota),
	KUNIT_CASE(damos_test_commit_dests),
	KUNIT_CASE(damos_test_commit_filter),
	KUNIT_CASE(damos_test_commit_pageout),
	KUNIT_CASE(damos_test_commit_migrate_hot),
	/* target/ctx 提交、运行期过滤、反馈控制和 region 下限。 */
	KUNIT_CASE(damon_test_commit_target_regions),
	KUNIT_CASE(damon_test_commit_ctx),
	KUNIT_CASE(damos_test_filter_out),
	KUNIT_CASE(damon_test_feed_loop_next_input),
	KUNIT_CASE(damon_test_set_filters_default_reject),
	KUNIT_CASE(damon_test_apply_min_nr_regions),
	KUNIT_CASE(damon_test_is_last_region),
	{},
};

/*
 * suite 字段清单：name="damon" 是 KUnit 报告/筛选名称；test_cases 指向上方以空项
 * 终止的静态数组。kunit_test_suite() 在测试配置下生成模块/内建注册入口，不转移
 * 数组 ownership；suite 和用例表均在整个注册期保持有效。
 */
static struct kunit_suite damon_test_suite = {
	.name = "damon",
	.test_cases = damon_test_cases,
};
kunit_test_suite(damon_test_suite);

#endif /* _DAMON_CORE_TEST_H */

#endif	/* CONFIG_DAMON_KUNIT_TEST */
