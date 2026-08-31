/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Data Access Monitor Unit Tests
 *
 * Author: SeongJae Park <sj@kernel.org>
 */
/*
 * 本头文件被 mm/damon/sysfs.c 在实现之后直接 include，用同一翻译单元内的 static
 * helper 测试 sysfs target 配置转换为 DAMON core target 的追加语义。仅在
 * CONFIG_DAMON_SYSFS_KUNIT_TEST 下编译，不是供其他源码包含的公共接口。
 */

/* 只有启用 DAMON sysfs KUnit 配置时，才把下述测试注册进当前翻译单元。 */
#ifdef CONFIG_DAMON_SYSFS_KUNIT_TEST

/* 双重 include guard 防止 sysfs.c 或测试构建路径重复展开同一批 static 实体。 */
#ifndef _DAMON_SYSFS_TEST_H
#define _DAMON_SYSFS_TEST_H

#include <kunit/test.h>

/*
 * nr_damon_targets() - 统计 DAMON context 当前链接的 target 数。
 * 业务背景：测试不窥探链表实现细节的数值字段，而用官方遍历宏验证
 * damon_sysfs_add_targets() 是否每次追加一个 core target。
 * 入参：@ctx 是测试独占、非 NULL 的借用 context，不转移 ownership。
 * 出参/返回：返回 adaptive_targets 链表节点数；空表返回 0，无副作用。
 * 注意事项：测试未启动 kdamond，无并发修改且无需锁；计数类型为 unsigned int。
 */
static unsigned int nr_damon_targets(struct damon_ctx *ctx)
{
	/* t 是宏提供的借用游标；nr_targets 从空表语义 0 开始逐节点递增。 */
	struct damon_target *t;
	unsigned int nr_targets = 0;

	damon_for_each_target(t, ctx)
		nr_targets++;

	return nr_targets;
}

/*
 * __damon_sysfs_test_get_any_pid() - 在闭区间内寻找一个当前存在的 PID。
 * 业务背景：VADDR DAMON target 创建会对 sysfs pid 调用 find_get_pid()；测试选择
 * 实际 PID，避免硬编码不存在的任务让 add_targets() 以 -EINVAL 失败。
 * 入参：@min/@max 是包含式整数 PID 边界；允许空区间，此时直接失败。
 * 出参/返回：找到时返回首个 pid number；全区间无对象返回 -1。临时 pid 引用
 * 在返回 number 前由 put_pid() 释放，调用者不获得 struct pid ownership。
 * 注意事项：进程可并发退出，因此结果只是尽力选择；真正 add 时会再次 find/get。
 */
static int __damon_sysfs_test_get_any_pid(int min, int max)
{
	/* pid 是每轮临时 owning 引用；i 是当前候选 PID。 */
	struct pid *pid;
	int i;

	/* 每次成功 get 都在返回前 put，避免测试辅助函数泄漏 pid 引用。 */
	for (i = min; i <= max; i++) {
		pid = find_get_pid(i);
		if (pid) {
			put_pid(pid);
			return i;
		}
	}
	return -1;
}

/*
 * damon_sysfs_test_add_targets() - 验证同一 sysfs 描述可连续追加两个 DAMON target。
 *
 * 业务背景：sysfs commit 会把 targets_arr 转成 core context 的 target 链表；本例
 * 手工构造一个 sysfs target，先用 PID 区间 12..100 添加，再更换为后续 PID 并
 * 再添加，期望 core target 数从 1 变 2，而不是覆盖首项。
 * 入参：@test 是 KUnit 借用上下文，用于 skip 和 EXPECT 记录，不由函数释放。
 * 出参/返回：无直接返回值；成功留下两个 target 供断言后销毁。任一测试夹具
 * 分配失败会 kunit_skip() 终止本 case；断言失败记录结果但继续 cleanup。
 * 注意事项：可用 GFP_KERNEL 分配并睡眠；所有 sysfs fixture 由测试手工 owning，
 * core target ownership 在 add 后归 @ctx 并由 damon_destroy_ctx() 递归释放。
 */
static void damon_sysfs_test_add_targets(struct kunit *test)
{
	/* 变量地图：sysfs_* 构成输入夹具；ctx 持有被测试函数创建的 core targets。 */
	struct damon_sysfs_targets *sysfs_targets;
	struct damon_sysfs_target *sysfs_target;
	struct damon_ctx *ctx;

	/* 阶段 1：构造单元素 targets 容器；kunit_skip 会抛出并终止当前 case。 */
	sysfs_targets = damon_sysfs_targets_alloc();
	if (!sysfs_targets)
		kunit_skip(test, "sysfs_targets alloc fail");
	/* targets_arr 只持一个 sysfs_target 借用槽，数组本身由测试负责 kfree。 */
	sysfs_targets->nr = 1;
	sysfs_targets->targets_arr = kmalloc_objs(*sysfs_targets->targets_arr,
						  1);
	if (!sysfs_targets->targets_arr) {
		kfree(sysfs_targets);
		kunit_skip(test, "targets_arr alloc fail");
	}

	/* 阶段 2：创建 target 和空 regions 描述；每个失败分支逆序释放已取得对象。 */
	sysfs_target = damon_sysfs_target_alloc();
	if (!sysfs_target) {
		kfree(sysfs_targets->targets_arr);
		kfree(sysfs_targets);
		kunit_skip(test, "sysfs_target alloc fail");
	}
	/* 第一个现存 PID 作为 VADDR target id；regions 保持 nr=0，测试只关注追加。 */
	sysfs_target->pid = __damon_sysfs_test_get_any_pid(12, 100);
	sysfs_target->regions = damon_sysfs_regions_alloc();
	if (!sysfs_target->regions) {
		kfree(sysfs_targets->targets_arr);
		kfree(sysfs_targets);
		kfree(sysfs_target);
		kunit_skip(test, "sysfs_regions alloc fail");
	}

	/* 此时 sysfs_targets 借用 sysfs_target，最终仍由本测试统一手工释放。 */
	sysfs_targets->targets_arr[0] = sysfs_target;

	/* 阶段 3：创建默认 VADDR context；失败时 sysfs fixture 尚未转移给 core。 */
	ctx = damon_new_ctx();
	if (!ctx) {
		kfree(sysfs_targets->targets_arr);
		kfree(sysfs_targets);
		kfree(sysfs_target->regions);
		kfree(sysfs_target);
		kunit_skip(test, "ctx alloc fail");
	}

	/*
	 * 第一次转换先把新 core target 链入 ctx，再尝试为 PID 取得独立引用；本测试
	 * 刻意只断言 target 数量而不检查 int 错误码，因此它验证的是“追加”属性，
	 * 不能单独证明 PID 解析与 region 转换成功。sysfs fixture 的 ownership 不变，
	 * 可修改后再次作为输入；任一部分创建的 core target 最终也由 ctx 销毁。
	 */
	damon_sysfs_add_targets(ctx, sysfs_targets);
	KUNIT_EXPECT_EQ(test, 1u, nr_damon_targets(ctx));

	/* 选择不同的后续 PID，再次调用应追加而不是原地更新第一个 core target。 */
	sysfs_target->pid = __damon_sysfs_test_get_any_pid(
			sysfs_target->pid + 1, 200);
	damon_sysfs_add_targets(ctx, sysfs_targets);
	KUNIT_EXPECT_EQ(test, 2u, nr_damon_targets(ctx));

	/* 阶段 4：先销毁 ctx 及其 target/PID 引用，再释放四层测试夹具。 */
	damon_destroy_ctx(ctx);
	kfree(sysfs_targets->targets_arr);
	kfree(sysfs_targets);
	kfree(sysfs_target->regions);
	kfree(sysfs_target);
}

/* case 数组以空元素作 KUnit 哨兵；当前 suite 只注册 add_targets 场景。 */
static struct kunit_case damon_sysfs_test_cases[] = {
	KUNIT_CASE(damon_sysfs_test_add_targets),
	{},
};

/* suite 静态存活到 KUnit 注册完成，test_cases 指向上述只读执行清单。 */
static struct kunit_suite damon_sysfs_test_suite = {
	.name = "damon-sysfs",
	.test_cases = damon_sysfs_test_cases,
};

/* 宏生成模块/init 注册胶水；仅测试配置下把 damon-sysfs suite 交给 KUnit。 */
kunit_test_suite(damon_sysfs_test_suite);

#endif /* _DAMON_SYSFS_TEST_H */
/* 至此结束头文件防重复展开范围，static 测试实体只生成一份。 */

#endif /* CONFIG_DAMON_SYSFS_KUNIT_TEST */
/* 关闭该配置时整个文件退化为空，不引入 KUnit 依赖或运行期副作用。 */
