// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/pgtable.h>

/*
 * 本测试调用只为 KUnit 导出的 lazy-MMU 状态查询符号；namespace 导入是模块链接
 * 契约，不会在运行时启用 lazy mode。测试围绕 current->lazy_mmu_state 的嵌套计数，
 * 验证 enable/disable 与 pause/resume 配对及暂停期间操作被屏蔽的不变量。
 */
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");

/*
 * expect_not_active() - 断言当前任务此刻不处于真实 lazy MMU 模式。
 * 业务背景：主用例在每个关闭/暂停边界调用，验证状态机对架构 enter/leave 的外观。
 * 入参：test 是 KUnit 框架拥有的非 NULL 测试上下文，借用且仅用于记录期望结果。
 * 出参/返回：无直接返回值；失败会在 test 中累计非致命 assertion，测试继续执行。
 * 注意事项：查询 current 的动态状态，不改变计数；中断上下文会被定义为 inactive。
 */
static void expect_not_active(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, is_lazy_mmu_mode_active());
}

/*
 * expect_active() - 断言当前任务此刻处于真实 lazy MMU 模式。
 * 业务背景：主用例在首次 enable、嵌套 disable 和 resume 后调用，检查外层配对仍生效。
 * 入参：test 为 KUnit 持有的非 NULL输入输出上下文，函数只借用并写测试结果。
 * 出参/返回：无直接返回值；期望不满足时记录失败但不终止用例，无 ownership 变化。
 * 注意事项：测试必须在普通任务上下文运行；中断上下文查询恒 false 会使本断言失败。
 */
static void expect_active(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, is_lazy_mmu_mode_active());
}

/*
 * lazy_mmu_mode_active() - 验证 lazy MMU enable 深度与 pause 深度组成的状态机。
 * 业务背景：批量页表修改用 enable/disable 让架构延后昂贵更新，KASAN 等临时需要
 * 正常 MMU 行为的路径用 pause/resume；该用例防止嵌套调用过早 leave 或暂停内误 enter。
 * 入参：test 是 KUnit 框架拥有的非 NULL输入输出上下文；测试借用它记录所有期望。
 * 出参/返回：无直接返回值；完成时 enable_count/pause_count 均恢复为 0、模式 inactive，
 * 每个不变量违例作为 KUnit 失败保存，无其他对象 ownership 转移。
 * 注意事项：要求入口 current 未激活且所有 API 严格配对；这些 helper 作用于 current，
 * 不提供跨任务同步。若架构未启用 lazy MMU，API 是空桩，本套件的 active 期望不适用。
 */
static void lazy_mmu_mode_active(struct kunit *test)
{
	/* 阶段 1：基线 inactive；第一层 enable 触发架构 enter 并使动态查询为 true。 */
	expect_not_active(test);

	lazy_mmu_mode_enable();
	expect_active(test);

	{
		/* Nested section */
		/* 嵌套区：第二次 enable 只增加深度；一次 disable 后外层仍保持 active。 */
		lazy_mmu_mode_enable();
		expect_active(test);

		lazy_mmu_mode_disable();
		expect_active(test);
	}

	{
		/* Paused section */
		/* 暂停区：pause 临时 leave，但保留外层 enable 深度，等待匹配 resume 恢复。 */
		lazy_mmu_mode_pause();
		expect_not_active(test);

		{
			/* No effect (paused) */
			/*
			 * 已暂停时 enable/disable 被屏蔽而不改 enable_count；更深层
			 * pause/resume 只配对 pause_count。外层恢复前不能重新进入架构
			 * lazy mode，因此每一步的动态查询都应保持 inactive。
			 */
			lazy_mmu_mode_enable();
			expect_not_active(test);

			lazy_mmu_mode_disable();
			expect_not_active(test);

			lazy_mmu_mode_pause();
			expect_not_active(test);

			lazy_mmu_mode_resume();
			expect_not_active(test);
		}

		/* 外层 resume 把 pause_count 降到 0；enable 深度仍非零，因此重新 enter。 */
		lazy_mmu_mode_resume();
		expect_active(test);
	}

	/* 阶段 4：最后一次 disable 消耗外层 enable，触发 leave 并恢复入口基线。 */
	lazy_mmu_mode_disable();
	expect_not_active(test);
}

/*
 * 用例表由 KUnit core 只读遍历；KUNIT_CASE 保存上方测试函数指针，空结构体是
 * 必需的终止哨兵。数组静态存储到模块卸载，框架不取得函数之外的资源 ownership。
 */
static struct kunit_case lazy_mmu_mode_test_cases[] = {
	KUNIT_CASE(lazy_mmu_mode_active),
	{}
};

/*
 * suite 把稳定名称映射到用例表；kunit_test_suite() 生成模块/内建注册入口，加载时
 * 发布给 KUnit、卸载时撤销。没有 init/exit fixture，故各用例必须自行恢复 current 状态。
 */
static struct kunit_suite lazy_mmu_mode_test_suite = {
	.name = "lazy_mmu_mode",
	.test_cases = lazy_mmu_mode_test_cases,
};
kunit_test_suite(lazy_mmu_mode_test_suite);

/* 模块元数据只描述测试用途和许可证，不参与 lazy MMU 状态机。 */
MODULE_DESCRIPTION("Tests for the lazy MMU mode");
MODULE_LICENSE("GPL");
