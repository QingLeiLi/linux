/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Base unit test (KUnit) API.
 *
 * Copyright (C) 2019, Google LLC.
 * Author: Brendan Higgins <brendanhiggins@google.com>
 */
/*
 * ============================================================================
 * 【KUnit - Linux 内核单元测试框架】
 *
 * 【什么是 KUnit？】
 * KUnit 是 Linux 内核的轻量级单元测试框架，类似于用户空间的 JUnit、pytest。
 * 它允许开发者为内核代码编写和运行单元测试，提高代码质量和可维护性。
 *
 * 【核心特性】
 * 1. 内核内测试：在内核空间运行，无需用户空间工具
 * 2. TAP 输出：遵循 Test Anything Protocol，易于集成 CI/CD
 * 3. 断言宏：EXPECT/ASSERT 风格的测试断言
 * 4. 测试套件：组织相关测试用例
 * 5. 参数化测试：用不同参数运行同一测试
 * 6. 模拟和隔离：支持测试替身（test doubles）
 * 7. 快速反馈：编译时和运行时快速检测问题
 *
 * 【基本概念】
 *
 * 1. 测试用例（Test Case）
 *    - 单个测试函数：void test_func(struct kunit *test)
 *    - 包含多个断言（expectations/assertions）
 *    - 使用 KUNIT_CASE() 宏定义
 *
 * 2. 测试套件（Test Suite）
 *    - 相关测试用例的集合
 *    - 可以有 init/exit 函数（setup/teardown）
 *    - 使用 kunit_test_suite() 宏注册
 *
 * 3. 断言类型
 *    - EXPECT: 失败后继续执行（软断言）
 *    - ASSERT: 失败后立即停止测试（硬断言）
 *
 * 【工作流程】
 * 1. 编写测试：定义测试函数和套件
 * 2. 编译：CONFIG_KUNIT=y，编译测试到内核
 * 3. 运行：启动内核或加载模块，自动运行测试
 * 4. 查看结果：dmesg 或 /sys/kernel/debug/kunit/<suite>/results
 *
 * 【使用示例】
 * ```c
 * #include <kunit/test.h>
 *
 * // 测试函数
 * static void example_add_test(struct kunit *test)
 * {
 *     KUNIT_EXPECT_EQ(test, 2, 1 + 1);
 *     KUNIT_EXPECT_NE(test, 3, 1 + 1);
 * }
 *
 * // 测试用例数组
 * static struct kunit_case example_test_cases[] = {
 *     KUNIT_CASE(example_add_test),
 *     {}  // 终止符
 * };
 *
 * // 测试套件
 * static struct kunit_suite example_test_suite = {
 *     .name = "example",
 *     .test_cases = example_test_cases,
 * };
 *
 * // 注册套件
 * kunit_test_suite(example_test_suite);
 * ```
 *
 * 【运行测试】
 * 1. 命令行工具：tools/testing/kunit/kunit.py run
 * 2. 内核参数：kunit.enable=1
 * 3. 模块加载：modprobe kunit-example-test
 *
 * 【输出格式（TAP）】
 * ```
 * TAP version 14
 * 1..1
 *   # Subtest: example
 *   1..1
 *     ok 1 example_add_test
 * ok 1 example
 * ```
 *
 * 【最佳实践】
 * 1. 每个函数一个测试文件（如 foo.c -> foo_test.c）
 * 2. 测试应该快速、独立、可重复
 * 3. 优先使用 EXPECT（更多信息），必要时用 ASSERT
 * 4. 使用有意义的测试名称（描述测试内容）
 * 5. 一个测试只验证一个行为
 *
 * 【与其他测试框架的对比】
 * - kselftest: 用户空间测试，测试整个内核功能
 * - KUnit: 内核空间单元测试，测试单个函数/模块
 * - LTP: 大规模系统测试
 *
 * 【配置选项】
 * - CONFIG_KUNIT: 启用 KUnit 框架
 * - CONFIG_KUNIT_DEBUGFS: debugfs 接口
 * - CONFIG_KUNIT_EXAMPLE_TEST: 示例测试
 * ============================================================================
 */

#ifndef _KUNIT_TEST_H
#define _KUNIT_TEST_H

#include <kunit/assert.h>
#include <kunit/try-catch.h>

#include <linux/args.h>
#include <linux/compiler.h>
#include <linux/container_of.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kconfig.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/rwonce.h>
#include <asm/sections.h>

/* Static key: true if any KUnit tests are currently running */
DECLARE_STATIC_KEY_FALSE(kunit_running);
/* 静态键：是否有 KUnit 测试正在运行
 *
 * 【用途】
 * 代码可以检查此键，在测试运行时改变行为：
 * - 启用额外的调试检查
 * - 跳过某些可能干扰测试的操作
 * - 注入错误（fault injection）
 *
 * 【使用】
 * if (static_branch_unlikely(&kunit_running)) {
 *     // 测试模式下的特殊处理
 * }
 *
 * 【性能】
 * 静态键编译为 nop 指令（当测试未运行时），零开销。
 */

struct kunit;
struct string_stream;

/* Maximum size of parameter description string. */
#define KUNIT_PARAM_DESC_SIZE 128
/* 参数描述字符串的最大大小
 * 用于参数化测试中描述当前参数
 */

/* Maximum size of a status comment. */
#define KUNIT_STATUS_COMMENT_SIZE 256
/* 状态注释的最大大小
 * TAP 输出中附加的诊断信息
 */

/*
 * TAP specifies subtest stream indentation of 4 spaces, 8 spaces for a
 * sub-subtest.  See the "Subtests" section in
 * https://node-tap.org/tap-protocol/
 */
/*
 * TAP 协议规定子测试缩进 4 个空格，子子测试缩进 8 个空格。
 */
#define KUNIT_INDENT_LEN		4
#define KUNIT_SUBTEST_INDENT		"    "
#define KUNIT_SUBSUBTEST_INDENT		"        "

/**
 * enum kunit_status - Type of result for a test or test suite
 * @KUNIT_SUCCESS: Denotes the test suite has not failed nor been skipped
 * @KUNIT_FAILURE: Denotes the test has failed.
 * @KUNIT_SKIPPED: Denotes the test has been skipped.
 */
/*
 * 【枚举】kunit_status - 测试或测试套件的结果类型
 */
enum kunit_status {
	KUNIT_SUCCESS,
	/* 成功：测试通过，没有失败或跳过
	 *
	 * 【含义】
	 * 所有断言都通过，没有错误发生。
	 */

	KUNIT_FAILURE,
	/* 失败：测试失败
	 *
	 * 【原因】
	 * - 断言失败（EXPECT/ASSERT 条件不满足）
	 * - 代码崩溃（段错误、panic）
	 * - 超时（测试执行时间过长）
	 */

	KUNIT_SKIPPED,
	/* 跳过：测试被跳过
	 *
	 * 【原因】
	 * - 前提条件不满足（如硬件不支持）
	 * - 明确调用 kunit_skip()
	 * - 配置不满足（如 CONFIG_XXX 未启用）
	 *
	 * 【与失败的区别】
	 * 跳过不算测试失败，通常表示测试不适用于当前环境。
	 */
};

/* Attribute struct/enum definitions */
/*
 * 属性结构体和枚举定义
 */

/*
 * Speed Attribute is stored as an enum and separated into categories of
 * speed: very_slow, slow, and normal. These speeds are relative to
 * other KUnit tests.
 *
 * Note: unset speed attribute acts as default of KUNIT_SPEED_NORMAL.
 */
/*
 * 【枚举】kunit_speed - 测试速度属性
 *
 * 速度属性分为三类：非常慢、慢、正常。
 * 这些速度是相对于其他 KUnit 测试而言的。
 *
 * 【用途】
 * - 过滤测试：只运行快速测试（CI 中）
 * - 优先级：优先运行慢测试（及早发现问题）
 * - 超时设置：慢测试允许更长的超时时间
 */
enum kunit_speed {
	KUNIT_SPEED_UNSET,
	/* 未设置：默认为 KUNIT_SPEED_NORMAL */

	KUNIT_SPEED_VERY_SLOW,
	/* 非常慢：通常超过 1 秒
	 *
	 * 【示例】
	 * - 大量数据处理
	 * - 复杂算法测试
	 * - 硬件初始化测试
	 */

	KUNIT_SPEED_SLOW,
	/* 慢：通常 100ms - 1 秒
	 *
	 * 【示例】
	 * - I/O 操作测试
	 * - 多次迭代测试
	 */

	KUNIT_SPEED_NORMAL,
	/* 正常：通常小于 100ms
	 *
	 * 【示例】
	 * - 简单函数测试
	 * - 数据结构操作测试
	 * - 大多数单元测试
	 */

	KUNIT_SPEED_MAX = KUNIT_SPEED_NORMAL,
	/* 最大速度值（用于验证） */
};

/* Holds attributes for each test case and suite */
/*
 * 【结构体】kunit_attributes - 测试用例和套件的属性
 */
struct kunit_attributes {
	enum kunit_speed speed;
	/* 速度属性
	 *
	 * 【设置方式】
	 * KUNIT_CASE_SLOW(test_func)  // 慢测试
	 * KUNIT_CASE(test_func)        // 正常速度（默认）
	 */
};

/**
 * struct kunit_case - represents an individual test case.
 *
 * @run_case: the function representing the actual test case.
 * @name:     the name of the test case.
 * @generate_params: the generator function for parameterized tests.
 * @attr:     the attributes associated with the test
 * @param_init: The init function to run before a parameterized test.
 * @param_exit: The exit function to run after a parameterized test.
 *
 * A test case is a function with the signature,
 * ``void (*)(struct kunit *)``
 * that makes expectations and assertions (see KUNIT_EXPECT_TRUE() and
 * KUNIT_ASSERT_TRUE()) about code under test. Each test case is associated
 * with a &struct kunit_suite and will be run after the suite's init
 * function and followed by the suite's exit function.
 *
 * A test case should be static and should only be created with the
 * KUNIT_CASE() macro; additionally, every array of test cases should be
 * terminated with an empty test case.
 *
 * Example:
 *
 * .. code-block:: c
 *
 *	void add_test_basic(struct kunit *test)
 *	{
 *		KUNIT_EXPECT_EQ(test, 1, add(1, 0));
 *		KUNIT_EXPECT_EQ(test, 2, add(1, 1));
 *		KUNIT_EXPECT_EQ(test, 0, add(-1, 1));
 *		KUNIT_EXPECT_EQ(test, INT_MAX, add(0, INT_MAX));
 *		KUNIT_EXPECT_EQ(test, -1, add(INT_MAX, INT_MIN));
 *	}
 *
 *	static struct kunit_case example_test_cases[] = {
 *		KUNIT_CASE(add_test_basic),
 *		{}
 *	};
 *
 */
/*
 * 【结构体】kunit_case - 表示单个测试用例
 *
 * 测试用例是 KUnit 的基本单元，每个测试用例测试一个特定的行为。
 */
struct kunit_case {
	void (*run_case)(struct kunit *test);
	/* 测试函数指针
	 *
	 * 【函数签名】
	 * void test_func(struct kunit *test)
	 *
	 * 【函数职责】
	 * 1. 执行被测代码
	 * 2. 使用断言验证结果（KUNIT_EXPECT_* / KUNIT_ASSERT_*）
	 * 3. 记录日志（kunit_info / kunit_warn）
	 *
	 * 【执行上下文】
	 * - 进程上下文（可睡眠）
	 * - 在 kunit 工作线程中运行
	 * - 有独立的栈空间
	 *
	 * 【测试隔离】
	 * 每个测试用例独立运行，不应该依赖其他测试的状态。
	 * 使用 init/exit 函数设置/清理共享资源。
	 *
	 * 【示例】
	 * static void test_list_add(struct kunit *test)
	 * {
	 *     struct list_head list;
	 *     struct item *item;
	 *
	 *     INIT_LIST_HEAD(&list);
	 *     item = kunit_kzalloc(test, sizeof(*item), GFP_KERNEL);
	 *     KUNIT_ASSERT_NOT_NULL(test, item);
	 *
	 *     list_add(&item->node, &list);
	 *     KUNIT_EXPECT_FALSE(test, list_empty(&list));
	 *     KUNIT_EXPECT_PTR_EQ(test, &item->node, list.next);
	 * }
	 */

	const char *name;
	/* 测试用例名称
	 *
	 * 【命名规则】
	 * - 描述性：说明测试什么（如 "test_list_add_empty"）
	 * - 小写字母和下划线
	 * - 前缀：通常以被测函数名开头
	 *
	 * 【使用】
	 * - TAP 输出中显示
	 * - debugfs 中标识测试
	 * - 过滤测试时使用（kunit.filter="name_pattern"）
	 *
	 * 【由宏自动设置】
	 * KUNIT_CASE(test_func) 自动将 name 设为 "test_func"
	 */

	const void* (*generate_params)(struct kunit *test,
				       const void *prev, char *desc);
	/* 参数生成器（用于参数化测试）
	 *
	 * 【参数化测试】
	 * 用不同参数多次运行同一测试函数，避免重复代码。
	 *
	 * 【函数签名】
	 * const void *generate(struct kunit *test, const void *prev, char *desc)
	 *
	 * @test: 当前测试上下文
	 * @prev: 前一个参数（NULL 表示第一次调用）
	 * @desc: 输出参数，填写参数描述（最大 KUNIT_PARAM_DESC_SIZE）
	 * 返回值：下一个参数指针，NULL 表示没有更多参数
	 *
	 * 【工作原理】
	 * KUnit 反复调用 generate_params，每次返回一个新参数：
	 * 1. 第一次调用：prev = NULL，返回第一个参数
	 * 2. 后续调用：prev = 上次返回值，返回下一个参数
	 * 3. 结束：返回 NULL
	 *
	 * 【参数访问】
	 * 测试函数通过 test->param_value 访问当前参数。
	 *
	 * 【示例】
	 * struct test_param {
	 *     int input;
	 *     int expected;
	 * };
	 *
	 * static const struct test_param params[] = {
	 *     { .input = 0, .expected = 0 },
	 *     { .input = 1, .expected = 1 },
	 *     { .input = 5, .expected = 120 },
	 * };
	 *
	 * static const void *factorial_gen_params(struct kunit *test,
	 *                                         const void *prev, char *desc)
	 * {
	 *     const struct test_param *param = prev;
	 *     int idx = param ? (param - params + 1) : 0;
	 *
	 *     if (idx >= ARRAY_SIZE(params))
	 *         return NULL;
	 *
	 *     snprintf(desc, KUNIT_PARAM_DESC_SIZE, "input=%d", params[idx].input);
	 *     return &params[idx];
	 * }
	 *
	 * static void test_factorial(struct kunit *test)
	 * {
	 *     const struct test_param *param = test->param_value;
	 *     KUNIT_EXPECT_EQ(test, param->expected, factorial(param->input));
	 * }
	 *
	 * static struct kunit_case math_test_cases[] = {
	 *     KUNIT_CASE_PARAM(test_factorial, factorial_gen_params),
	 *     {}
	 * };
	 */

	struct kunit_attributes attr;
	/* 测试属性
	 *
	 * 【当前支持的属性】
	 * - speed: 测试速度（normal, slow, very_slow）
	 *
	 * 【用途】
	 * - 过滤：只运行特定速度的测试
	 * - 调度：慢测试可能有更长超时
	 * - 报告：按速度分类测试结果
	 */

	int (*param_init)(struct kunit *test);
	/* 参数初始化函数（可选）
	 *
	 * 【调用时机】
	 * 在参数化测试的每次迭代之前调用（在 run_case 之前）。
	 *
	 * 【函数签名】
	 * int init(struct kunit *test)
	 *
	 * 返回值：0=成功，负值=失败（跳过此参数）
	 *
	 * 【用途】
	 * 为特定参数准备资源：
	 * - 分配内存
	 * - 初始化数据结构
	 * - 设置硬件状态
	 *
	 * 【与套件 init 的区别】
	 * - 套件 init: 整个套件运行前调用一次
	 * - param_init: 每个参数运行前调用一次
	 *
	 * 【示例】
	 * static int param_init(struct kunit *test)
	 * {
	 *     struct my_context *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	 *     KUNIT_ASSERT_NOT_NULL(test, ctx);
	 *     test->priv = ctx;
	 *     return 0;
	 * }
	 */

	void (*param_exit)(struct kunit *test);
	/* 参数清理函数（可选）
	 *
	 * 【调用时机】
	 * 在参数化测试的每次迭代之后调用（在 run_case 之后）。
	 *
	 * 【函数签名】
	 * void exit(struct kunit *test)
	 *
	 * 【用途】
	 * 清理参数特定的资源：
	 * - 释放内存
	 * - 关闭文件
	 * - 恢复状态
	 *
	 * 【注意】
	 * 使用 kunit_kzalloc 等 KUnit 管理的资源会自动释放，
	 * 无需在 param_exit 中手动释放。
	 *
	 * 【示例】
	 * static void param_exit(struct kunit *test)
	 * {
	 *     struct my_context *ctx = test->priv;
	 *     cleanup_context(ctx);
	 * }
	 */

	/* private: internal use only. */
	enum kunit_status status;
	/* 测试状态（内部使用）
	 * 记录测试结果：SUCCESS, FAILURE, SKIPPED
	 */

	char *module_name;
	/* 模块名称（内部使用）
	 * 测试所属的内核模块名
	 */

	struct string_stream *log;
	/* 日志流（内部使用）
	 * 存储测试执行过程中的日志消息
	 */
};

static inline char *kunit_status_to_ok_not_ok(enum kunit_status status)
{
	switch (status) {
	case KUNIT_SKIPPED:
	case KUNIT_SUCCESS:
		return "ok";
	case KUNIT_FAILURE:
		return "not ok";
	}
	return "invalid";
}
/* 将状态转换为 TAP 协议的 "ok"/"not ok"
 * @status: 测试状态
 *
 * 返回值：TAP 状态字符串
 *
 * 【TAP 协议】
 * - "ok": 测试通过或跳过
 * - "not ok": 测试失败
 */

/**
 * KUNIT_CASE - A helper for creating a &struct kunit_case
 *
 * @test_name: a reference to a test case function.
 *
 * Takes a symbol for a function representing a test case and creates a
 * &struct kunit_case object from it. See the documentation for
 * &struct kunit_case for an example on how to use it.
 */
/*
 * 【宏】KUNIT_CASE - 创建测试用例的辅助宏
 *
 * 这是定义测试用例的标准方式。
 *
 * 【使用示例】
 * static void test_addition(struct kunit *test)
 * {
 *     KUNIT_EXPECT_EQ(test, 4, 2 + 2);
 * }
 *
 * static struct kunit_case math_test_cases[] = {
 *     KUNIT_CASE(test_addition),
 *     {}  // 终止符
 * };
 */
#define KUNIT_CASE(test_name)			\
		{ .run_case = test_name, .name = #test_name,	\
		  .module_name = KBUILD_MODNAME}

/**
 * KUNIT_CASE_ATTR - A helper for creating a &struct kunit_case
 * with attributes
 *
 * @test_name: a reference to a test case function.
 * @attributes: a reference to a struct kunit_attributes object containing
 * test attributes
 */
/*
 * 【宏】KUNIT_CASE_ATTR - 创建带属性的测试用例
 *
 * 允许为测试用例指定自定义属性。
 *
 * 【使用示例】
 * static struct kunit_attributes my_attrs = {
 *     .speed = KUNIT_SPEED_SLOW,
 * };
 *
 * static struct kunit_case math_test_cases[] = {
 *     KUNIT_CASE_ATTR(test_slow_operation, my_attrs),
 *     {}
 * };
 */
#define KUNIT_CASE_ATTR(test_name, attributes)			\
		{ .run_case = test_name, .name = #test_name,	\
		  .attr = attributes, .module_name = KBUILD_MODNAME}

/**
 * KUNIT_CASE_SLOW - A helper for creating a &struct kunit_case
 * with the slow attribute
 *
 * @test_name: a reference to a test case function.
 */
/*
 * 【宏】KUNIT_CASE_SLOW - 创建慢速测试用例的快捷方式
 *
 * 等价于 KUNIT_CASE_ATTR，但自动设置 speed = KUNIT_SPEED_SLOW。
 *
 * 【使用场景】
 * 标记执行时间较长的测试（通常 > 100ms）。
 *
 * 【使用示例】
 * static struct kunit_case math_test_cases[] = {
 *     KUNIT_CASE(test_fast),
 *     KUNIT_CASE_SLOW(test_slow),
 *     {}
 * };
 */

#define KUNIT_CASE_SLOW(test_name)			\
		{ .run_case = test_name, .name = #test_name,	\
		  .attr.speed = KUNIT_SPEED_SLOW, .module_name = KBUILD_MODNAME}

/**
 * KUNIT_CASE_PARAM - A helper for creation a parameterized &struct kunit_case
 *
 * @test_name: a reference to a test case function.
 * @gen_params: a reference to a parameter generator function.
 *
 * The generator function::
 *
 *	const void* gen_params(const void *prev, char *desc)
 *
 * is used to lazily generate a series of arbitrarily typed values that fit into
 * a void*. The argument @prev is the previously returned value, which should be
 * used to derive the next value; @prev is set to NULL on the initial generator
 * call. When no more values are available, the generator must return NULL.
 * Optionally write a string into @desc (size of KUNIT_PARAM_DESC_SIZE)
 * describing the parameter.
 */
/*
 * 【宏】KUNIT_CASE_PARAM - 创建参数化测试用例
 *
 * 参数化测试允许用不同参数多次运行同一测试函数。
 *
 * 【参数生成器要求】
 * const void *gen_params(struct kunit *test, const void *prev, char *desc)
 * - prev: 前一个参数（首次调用为 NULL）
 * - desc: 输出参数描述（可选，最大 KUNIT_PARAM_DESC_SIZE）
 * - 返回值：下一个参数指针，NULL 表示结束
 *
 * 【使用示例】
 * // 参数结构
 * struct math_param {
 *     int a, b, expected;
 * };
 *
 * static const struct math_param params[] = {
 *     { 1, 1, 2 },
 *     { 2, 3, 5 },
 *     { -1, 1, 0 },
 * };
 *
 * // 参数生成器
 * static const void *add_gen_params(struct kunit *test,
 *                                   const void *prev, char *desc)
 * {
 *     const struct math_param *p = prev;
 *     int idx = p ? (p - params + 1) : 0;
 *
 *     if (idx >= ARRAY_SIZE(params))
 *         return NULL;
 *
 *     snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%d+%d",
 *              params[idx].a, params[idx].b);
 *     return &params[idx];
 * }
 *
 * // 测试函数
 * static void test_add(struct kunit *test)
 * {
 *     const struct math_param *p = test->param_value;
 *     KUNIT_EXPECT_EQ(test, p->expected, p->a + p->b);
 * }
 *
 * // 注册
 * static struct kunit_case math_test_cases[] = {
 *     KUNIT_CASE_PARAM(test_add, add_gen_params),
 *     {}
 * };
 */
#define KUNIT_CASE_PARAM(test_name, gen_params)			\
		{ .run_case = test_name, .name = #test_name,	\
		  .generate_params = gen_params, .module_name = KBUILD_MODNAME}

/**
 * KUNIT_CASE_PARAM_ATTR - A helper for creating a parameterized &struct
 * kunit_case with attributes
 *
 * @test_name: a reference to a test case function.
 * @gen_params: a reference to a parameter generator function.
 * @attributes: a reference to a struct kunit_attributes object containing
 * test attributes
 */
/*
 * 【宏】KUNIT_CASE_PARAM_ATTR - 创建带属性的参数化测试用例
 *
 * 结合参数化测试和自定义属性。
 */
#define KUNIT_CASE_PARAM_ATTR(test_name, gen_params, attributes)	\
		{ .run_case = test_name, .name = #test_name,	\
		  .generate_params = gen_params,				\
		  .attr = attributes, .module_name = KBUILD_MODNAME}

/**
 * KUNIT_CASE_PARAM_WITH_INIT - Define a parameterized KUnit test case with custom
 * param_init() and param_exit() functions.
 * @test_name: The function implementing the test case.
 * @gen_params: The function to generate parameters for the test case.
 * @init: A reference to the param_init() function to run before a parameterized test.
 * @exit: A reference to the param_exit() function to run after a parameterized test.
 *
 * Provides the option to register param_init() and param_exit() functions.
 * param_init/exit will be passed the parameterized test context and run once
 * before and once after the parameterized test. The init function can be used
 * to add resources to share between parameter runs, pass parameter arrays,
 * and any other setup logic. The exit function can be used to clean up resources
 * that were not managed by the parameterized test, and any other teardown logic.
 *
 * Note: If you are registering a parameter array in param_init() with
 * kunit_register_param_array() then you need to pass kunit_array_gen_params()
 * to this as the generator function.
 */
/*
 * 【宏】KUNIT_CASE_PARAM_WITH_INIT - 创建带初始化/清理的参数化测试用例
 *
 * 为参数化测试提供额外的设置和清理钩子。
 *
 * 【使用场景】
 * 每个参数需要独立的资源准备：
 * - 分配参数特定的内存
 * - 打开参数特定的文件
 * - 初始化参数特定的硬件状态
 *
 * 【执行顺序】
 * 对于每个参数：
 * 1. param_init(test)
 * 2. run_case(test)  // 测试函数
 * 3. param_exit(test)
 *
 * 【使用示例】
 * static int param_init(struct kunit *test)
 * {
 *     struct my_ctx *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
 *     KUNIT_ASSERT_NOT_NULL(test, ctx);
 *     // 根据 test->param_value 初始化 ctx
 *     test->priv = ctx;
 *     return 0;
 * }
 *
 * static void param_exit(struct kunit *test)
 * {
 *     struct my_ctx *ctx = test->priv;
 *     // 清理 ctx（如果未使用 KUnit 资源管理）
 * }
 *
 * static struct kunit_case test_cases[] = {
 *     KUNIT_CASE_PARAM_WITH_INIT(test_func, gen_params,
 *                                param_init, param_exit),
 *     {}
 * };
 */
#define KUNIT_CASE_PARAM_WITH_INIT(test_name, gen_params, init, exit)		\
		{ .run_case = test_name, .name = #test_name,			\
		  .generate_params = gen_params,				\
		  .param_init = init, .param_exit = exit,			\
		  .module_name = KBUILD_MODNAME}

/**
 * struct kunit_suite - describes a related collection of &struct kunit_case
 *
 * @name:	the name of the test. Purely informational.
 * @suite_init:	called once per test suite before the test cases.
 * @suite_exit:	called once per test suite after all test cases.
 * @init:	called before every test case.
 * @exit:	called after every test case.
 * @test_cases:	a null terminated array of test cases.
 * @attr:	the attributes associated with the test suite
 *
 * A kunit_suite is a collection of related &struct kunit_case s, such that
 * @init is called before every test case and @exit is called after every
 * test case, similar to the notion of a *test fixture* or a *test class*
 * in other unit testing frameworks like JUnit or Googletest.
 *
 * Note that @exit and @suite_exit will run even if @init or @suite_init
 * fail: make sure they can handle any inconsistent state which may result.
 *
 * Every &struct kunit_case must be associated with a kunit_suite for KUnit
 * to run it.
 */
/*
 * 【结构体】kunit_suite - 测试套件（相关测试用例的集合）
 *
 * 测试套件是 KUnit 的组织单元，将相关的测试用例组合在一起。
 * 类似于其他测试框架（JUnit、pytest）中的测试类或测试夹具。
 */
struct kunit_suite {
	const char name[256];
	/* 套件名称
	 *
	 * 【命名规则】
	 * - 描述性：说明测试的模块或功能（如 "list_test"）
	 * - 小写字母和下划线
	 * - 通常以被测模块名开头
	 *
	 * 【用途】
	 * - TAP 输出中显示
	 * - 过滤测试（kunit.filter="suite_name"）
	 * - debugfs 路径（/sys/kernel/debug/kunit/suite_name）
	 */

	int (*suite_init)(struct kunit_suite *suite);
	/* 套件初始化函数（可选）
	 *
	 * 【调用时机】
	 * 在运行套件中的任何测试用例之前调用一次。
	 *
	 * 【函数签名】
	 * int suite_init(struct kunit_suite *suite)
	 *
	 * 返回值：0=成功，负值=失败（跳过整个套件）
	 *
	 * 【用途】
	 * 准备整个套件共享的资源：
	 * - 初始化全局状态
	 * - 分配共享内存
	 * - 设置硬件环境
	 * - 创建测试数据
	 *
	 * 【与 init 的区别】
	 * - suite_init: 整个套件运行前调用一次
	 * - init: 每个测试用例运行前调用一次
	 *
	 * 【失败处理】
	 * 如果 suite_init 失败，整个套件被跳过，
	 * 但 suite_exit 仍然会被调用（需要处理部分初始化的状态）。
	 *
	 * 【示例】
	 * static int my_suite_init(struct kunit_suite *suite)
	 * {
	 *     // 初始化全局测试数据
	 *     global_test_data = kzalloc(sizeof(*global_test_data), GFP_KERNEL);
	 *     if (!global_test_data)
	 *         return -ENOMEM;
	 *     return 0;
	 * }
	 */

	void (*suite_exit)(struct kunit_suite *suite);
	/* 套件清理函数（可选）
	 *
	 * 【调用时机】
	 * 在运行完套件中的所有测试用例之后调用一次。
	 *
	 * 【函数签名】
	 * void suite_exit(struct kunit_suite *suite)
	 *
	 * 【用途】
	 * 清理 suite_init 分配的资源：
	 * - 释放全局内存
	 * - 关闭文件
	 * - 恢复硬件状态
	 *
	 * 【重要】
	 * 即使 suite_init 失败，suite_exit 也会被调用！
	 * 必须能够处理部分初始化的状态。
	 *
	 * 【示例】
	 * static void my_suite_exit(struct kunit_suite *suite)
	 * {
	 *     // 清理全局测试数据
	 *     kfree(global_test_data);
	 *     global_test_data = NULL;
	 * }
	 */

	int (*init)(struct kunit *test);
	/* 测试用例初始化函数（可选）
	 *
	 * 【调用时机】
	 * 在每个测试用例运行之前调用。
	 *
	 * 【函数签名】
	 * int init(struct kunit *test)
	 *
	 * 返回值：0=成功，负值=失败（跳过该测试用例）
	 *
	 * 【用途】
	 * 为每个测试用例准备独立的环境：
	 * - 分配测试特定的资源
	 * - 初始化被测对象
	 * - 设置测试前提条件
	 *
	 * 【test->priv 的使用】
	 * 通常在 init 中分配上下文数据，存储到 test->priv，
	 * 测试函数和 exit 可以通过 test->priv 访问。
	 *
	 * 【失败处理】
	 * 如果 init 失败，测试用例被跳过，
	 * 但 exit 仍然会被调用。
	 *
	 * 【示例】
	 * struct my_test_context {
	 *     struct list_head list;
	 *     int test_value;
	 * };
	 *
	 * static int my_init(struct kunit *test)
	 * {
	 *     struct my_test_context *ctx;
	 *
	 *     ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	 *     KUNIT_ASSERT_NOT_NULL(test, ctx);
	 *
	 *     INIT_LIST_HEAD(&ctx->list);
	 *     ctx->test_value = 42;
	 *     test->priv = ctx;
	 *
	 *     return 0;
	 * }
	 */

	void (*exit)(struct kunit *test);
	/* 测试用例清理函数（可选）
	 *
	 * 【调用时机】
	 * 在每个测试用例运行之后调用。
	 *
	 * 【函数签名】
	 * void exit(struct kunit *test)
	 *
	 * 【用途】
	 * 清理 init 分配的资源：
	 * - 释放内存
	 * - 关闭文件
	 * - 重置状态
	 *
	 * 【重要】
	 * 即使 init 或测试函数失败，exit 也会被调用！
	 *
	 * 【KUnit 资源管理】
	 * 使用 kunit_kzalloc、kunit_kmalloc 等分配的资源会自动释放，
	 * 无需在 exit 中手动释放。只有手动分配的资源才需要在 exit 中清理。
	 *
	 * 【示例】
	 * static void my_exit(struct kunit *test)
	 * {
	 *     struct my_test_context *ctx = test->priv;
	 *     // kunit_kzalloc 分配的 ctx 会自动释放
	 *     // 如果有手动分配的资源，在这里清理
	 * }
	 */

	struct kunit_case *test_cases;
	/* 测试用例数组
	 *
	 * 【格式】
	 * 以 NULL 结尾的 kunit_case 数组。
	 *
	 * 【定义示例】
	 * static struct kunit_case my_test_cases[] = {
	 *     KUNIT_CASE(test_func1),
	 *     KUNIT_CASE(test_func2),
	 *     KUNIT_CASE_SLOW(test_slow_func),
	 *     KUNIT_CASE_PARAM(test_param_func, gen_params),
	 *     {}  // 终止符，必须有！
	 * };
	 */

	struct kunit_attributes attr;
	/* 套件属性
	 *
	 * 【当前支持的属性】
	 * - speed: 套件的速度属性（影响所有测试用例）
	 *
	 * 【用途】
	 * 为整个套件设置默认属性，单个测试用例可以覆盖。
	 */

	/* private: internal use only */
	char status_comment[KUNIT_STATUS_COMMENT_SIZE];
	/* 状态注释（内部使用）
	 * 存储套件级别的诊断信息
	 */

	struct dentry *debugfs;
	/* debugfs 目录项（内部使用）
	 * /sys/kernel/debug/kunit/<suite_name>
	 */

	struct string_stream *log;
	/* 日志流（内部使用）
	 * 存储套件级别的日志消息
	 */

	int suite_init_err;
	/* 套件初始化错误码（内部使用）
	 * 记录 suite_init 的返回值
	 */

	bool is_init;
	/* 是否已初始化标志（内部使用） */
};

/* Stores an array of suites, end points one past the end */
/*
 * 【结构体】kunit_suite_set - 存储套件数组
 *
 * 用于批量管理多个测试套件。
 */
struct kunit_suite_set {
	struct kunit_suite * const *start;
	/* 套件数组的起始指针 */

	struct kunit_suite * const *end;
	/* 套件数组的结束指针（指向最后一个元素之后） */
};

/* Stores the pointer to the parameter array and its metadata. */
/*
 * 【结构体】kunit_params - 存储参数数组及其元数据
 */
struct kunit_params {
	/*
	 * Reference to the parameter array for a parameterized test. This
	 * is NULL if a parameter array wasn't directly passed to the
	 * parameterized test context struct kunit via kunit_register_params_array().
	 */
	const void *params;
	/* 参数数组指针
	 * 如果未通过 kunit_register_params_array() 注册，则为 NULL
	 */

	/* Reference to a function that gets the description of a parameter. */
	void (*get_description)(struct kunit *test, const void *param, char *desc);
	/* 获取参数描述的函数指针
	 *
	 * 【函数签名】
	 * void get_description(struct kunit *test, const void *param, char *desc)
	 *
	 * @test:  测试上下文
	 * @param: 当前参数
	 * @desc:  输出参数描述（最大 KUNIT_PARAM_DESC_SIZE）
	 *
	 * 【用途】
	 * 为 TAP 输出生成参数的可读描述
	 */

	size_t num_params;
	/* 参数数量 */

	size_t elem_size;
	/* 每个参数的大小（字节）*/
};

/**
 * struct kunit - represents a running instance of a test.
 *
 * @priv: for user to store arbitrary data. Commonly used to pass data
 *	  created in the init function (see &struct kunit_suite).
 * @parent: reference to the parent context of type struct kunit that can
 *	    be used for storing shared resources.
 * @params_array: for storing the parameter array.
 *
 * Used to store information about the current context under which the test
 * is running. Most of this data is private and should only be accessed
 * indirectly via public functions; the exceptions are @priv, @parent and
 * @params_array which can be used by the test writer to store arbitrary data,
 * access the parent context, and to store the parameter array, respectively.
 */
/*
 * 【结构体】kunit - 表示正在运行的测试实例
 *
 * 这是测试函数接收的主要参数，包含测试的所有上下文信息。
 */
struct kunit {
	void *priv;
	/* 用户私有数据指针
	 *
	 * 【用途】
	 * 存储测试特定的上下文数据，通常在 init 函数中分配：
	 * - 测试夹具（test fixture）
	 * - 被测对象实例
	 * - 共享的测试数据
	 *
	 * 【生命周期】
	 * - init: 分配并初始化，存储到 test->priv
	 * - 测试函数: 通过 test->priv 访问
	 * - exit: 清理（如果未使用 KUnit 资源管理）
	 *
	 * 【示例】
	 * struct my_context {
	 *     struct device *dev;
	 *     int test_value;
	 * };
	 *
	 * static int my_init(struct kunit *test)
	 * {
	 *     struct my_context *ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	 *     test->priv = ctx;
	 *     return 0;
	 * }
	 *
	 * static void test_something(struct kunit *test)
	 * {
	 *     struct my_context *ctx = test->priv;
	 *     KUNIT_EXPECT_EQ(test, 42, ctx->test_value);
	 * }
	 */

	struct kunit *parent;
	/* 父测试上下文
	 *
	 * 【用途】
	 * 访问父级测试的资源（在嵌套测试或参数化测试中）。
	 *
	 * 【使用场景】
	 * - 子测试访问父测试的 priv 数据
	 * - 共享父测试初始化的资源
	 * - 层次化测试结构
	 *
	 * 【注意】
	 * 对于顶层测试，parent 为 NULL。
	 */

	struct kunit_params params_array;
	/* 参数数组（用于参数化测试）
	 *
	 * 【用途】
	 * 存储通过 kunit_register_param_array() 注册的参数数组。
	 */

	/* private: internal use only. */
	const char *name; /* Read only after initialization! */
	/* 测试名称（内部使用，初始化后只读） */

	struct string_stream *log; /* Points at case log after initialization */
	/* 日志流（内部使用）
	 * 指向测试用例的日志，初始化后设置
	 */

	struct kunit_try_catch try_catch;
	/* try-catch 机制（内部使用）
	 * 用于捕获测试中的异常（如段错误、panic）
	 */

	/* param_value is the current parameter value for a test case. */
	const void *param_value;
	/* 当前参数值（参数化测试中使用）
	 *
	 * 【访问方式】
	 * 在参数化测试函数中：
	 * const struct my_param *param = test->param_value;
	 *
	 * 【生命周期】
	 * 每次参数化测试迭代时，KUnit 设置 param_value 为当前参数。
	 */

	/* param_index stores the index of the parameter in parameterized tests. */
	int param_index;
	/* 参数索引（参数化测试中使用）
	 * 当前参数在参数数组中的索引（从 0 开始）
	 */

	/*
	 * success starts as true, and may only be set to false during a
	 * test case; thus, it is safe to update this across multiple
	 * threads using WRITE_ONCE; however, as a consequence, it may only
	 * be read after the test case finishes once all threads associated
	 * with the test case have terminated.
	 */
	spinlock_t lock; /* Guards all mutable test state. */
	/* 自旋锁（内部使用）
	 * 保护所有可变的测试状态
	 *
	 * 【并发安全】
	 * 测试状态的更新使用 WRITE_ONCE 和此锁保护，
	 * 支持多线程测试（虽然不常见）。
	 */

	enum kunit_status status; /* Read only after test_case finishes! */
	/* 测试状态（内部使用）
	 * SUCCESS, FAILURE, 或 SKIPPED
	 *
	 * 【注意】
	 * 只有在测试用例完成后才能安全读取（所有线程终止后）
	 */

	/*
	 * Because resources is a list that may be updated multiple times (with
	 * new resources) from any thread associated with a test case, we must
	 * protect it with some type of lock.
	 */
	struct list_head resources; /* Protected by lock. */
	/* 资源链表（内部使用）
	 * 存储测试分配的所有资源（通过 kunit_add_resource）
	 *
	 * 【资源管理】
	 * KUnit 跟踪测试分配的资源，测试结束后自动释放：
	 * - kunit_kzalloc/kunit_kmalloc 分配的内存
	 * - 通过 kunit_add_resource 注册的自定义资源
	 *
	 * 【并发保护】
	 * 由 lock 保护，因为资源可能从多个线程添加。
	 */

	char status_comment[KUNIT_STATUS_COMMENT_SIZE];
	/* 状态注释（内部使用）
	 * 存储测试失败或跳过的原因（诊断信息）
	 */

	/* Saves the last seen test. Useful to help with faults. */
	struct kunit_loc last_seen;
	/* 最后看到的位置（内部使用）
	 * 用于故障诊断，记录最后一次断言的位置
	 */
};

static inline void kunit_set_failure(struct kunit *test)
{
	WRITE_ONCE(test->status, KUNIT_FAILURE);
}
/* 设置测试失败状态
 * @test: 测试上下文
 *
 * 【使用】
 * 在自定义断言或测试辅助函数中标记测试失败。
 *
 * 【注意】
 * 通常不需要手动调用，KUNIT_EXPECT_*/KUNIT_ASSERT_* 会自动设置。
 */

bool kunit_enabled(void);
bool kunit_autorun(void);
const char *kunit_action(void);
const char *kunit_filter_glob(void);
char *kunit_filter(void);
char *kunit_filter_action(void);

void kunit_init_test(struct kunit *test, const char *name, struct string_stream *log);

int kunit_run_tests(struct kunit_suite *suite);

size_t kunit_suite_num_test_cases(struct kunit_suite *suite);

unsigned int kunit_test_case_num(struct kunit_suite *suite,
				 struct kunit_case *test_case);

struct kunit_suite_set
kunit_filter_suites(const struct kunit_suite_set *suite_set,
		    const char *filter_glob,
		    char *filters,
		    char *filter_action,
		    int *err);
void kunit_free_suite_set(struct kunit_suite_set suite_set);

int __kunit_test_suites_init(struct kunit_suite * const * const suites, int num_suites,
			     bool run_tests);

void __kunit_test_suites_exit(struct kunit_suite **suites, int num_suites);

void kunit_exec_run_tests(struct kunit_suite_set *suite_set, bool builtin);
void kunit_exec_list_tests(struct kunit_suite_set *suite_set, bool include_attr);

struct kunit_suite_set kunit_merge_suite_sets(struct kunit_suite_set init_suite_set,
		struct kunit_suite_set suite_set);

const void *kunit_array_gen_params(struct kunit *test, const void *prev, char *desc);

#if IS_BUILTIN(CONFIG_KUNIT)
int kunit_run_all_tests(void);
#else
static inline int kunit_run_all_tests(void)
{
	return 0;
}
#endif /* IS_BUILTIN(CONFIG_KUNIT) */

#define __kunit_test_suites(unique_array, ...)				       \
	static struct kunit_suite *unique_array[]			       \
	__aligned(sizeof(struct kunit_suite *))				       \
	__used __section(".kunit_test_suites") = { __VA_ARGS__ }

/**
 * kunit_test_suites() - used to register one or more &struct kunit_suite
 *			 with KUnit.
 *
 * @__suites: a statically allocated list of &struct kunit_suite.
 *
 * Registers @suites with the test framework.
 * This is done by placing the array of struct kunit_suite * in the
 * .kunit_test_suites ELF section.
 *
 * When builtin, KUnit tests are all run via the executor at boot, and when
 * built as a module, they run on module load.
 *
 */
#define kunit_test_suites(__suites...)						\
	__kunit_test_suites(__UNIQUE_ID(array),				\
			    ##__suites)

#define kunit_test_suite(suite)	kunit_test_suites(&suite)

#define __kunit_init_test_suites(unique_array, ...)			       \
	static struct kunit_suite *unique_array[]			       \
	__aligned(sizeof(struct kunit_suite *))				       \
	__used __section(".kunit_init_test_suites") = { __VA_ARGS__ }

/**
 * kunit_test_init_section_suites() - used to register one or more &struct
 *				      kunit_suite containing init functions or
 *				      init data.
 *
 * @__suites: a statically allocated list of &struct kunit_suite.
 *
 * This functions similar to kunit_test_suites() except that it compiles the
 * list of suites during init phase.
 *
 * This macro also suffixes the array and suite declarations it makes with
 * _probe; so that modpost suppresses warnings about referencing init data
 * for symbols named in this manner.
 *
 * Note: these init tests are not able to be run after boot so there is no
 * "run" debugfs file generated for these tests.
 *
 * Also, do not mark the suite or test case structs with __initdata because
 * they will be used after the init phase with debugfs.
 */
#define kunit_test_init_section_suites(__suites...)			\
	__kunit_init_test_suites(CONCATENATE(__UNIQUE_ID(array), _probe), \
			    ##__suites)

#define kunit_test_init_section_suite(suite)	\
	kunit_test_init_section_suites(&suite)

#define kunit_suite_for_each_test_case(suite, test_case)		\
	for (test_case = suite->test_cases; test_case->run_case; test_case++)

enum kunit_status kunit_suite_has_succeeded(struct kunit_suite *suite);

/**
 * kunit_kmalloc_array() - Like kmalloc_array() except the allocation is *test managed*.
 * @test: The test context object.
 * @n: number of elements.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * Just like `kmalloc_array(...)`, except the allocation is managed by the test case
 * and is automatically cleaned up after the test case concludes. See kunit_add_action()
 * for more information.
 *
 * Note that some internal context data is also allocated with GFP_KERNEL,
 * regardless of the gfp passed in.
 */
void *kunit_kmalloc_array(struct kunit *test, size_t n, size_t size, gfp_t gfp);

/**
 * kunit_kmalloc() - Like kmalloc() except the allocation is *test managed*.
 * @test: The test context object.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * See kmalloc() and kunit_kmalloc_array() for more information.
 *
 * Note that some internal context data is also allocated with GFP_KERNEL,
 * regardless of the gfp passed in.
 */
static inline void *kunit_kmalloc(struct kunit *test, size_t size, gfp_t gfp)
{
	return kunit_kmalloc_array(test, 1, size, gfp);
}

/**
 * kunit_kfree() - Like kfree except for allocations managed by KUnit.
 * @test: The test case to which the resource belongs.
 * @ptr: The memory allocation to free.
 */
void kunit_kfree(struct kunit *test, const void *ptr);

/**
 * kunit_kzalloc() - Just like kunit_kmalloc(), but zeroes the allocation.
 * @test: The test context object.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * See kzalloc() and kunit_kmalloc_array() for more information.
 */
static inline void *kunit_kzalloc(struct kunit *test, size_t size, gfp_t gfp)
{
	return kunit_kmalloc(test, size, gfp | __GFP_ZERO);
}

/**
 * kunit_kcalloc() - Just like kunit_kmalloc_array(), but zeroes the allocation.
 * @test: The test context object.
 * @n: number of elements.
 * @size: The size in bytes of the desired memory.
 * @gfp: flags passed to underlying kmalloc().
 *
 * See kcalloc() and kunit_kmalloc_array() for more information.
 */
static inline void *kunit_kcalloc(struct kunit *test, size_t n, size_t size, gfp_t gfp)
{
	return kunit_kmalloc_array(test, n, size, gfp | __GFP_ZERO);
}


/**
 * kunit_kfree_const() - conditionally free test managed memory
 * @test: The test context object.
 * @x: pointer to the memory
 *
 * Calls kunit_kfree() only if @x is not in .rodata section.
 * See kunit_kstrdup_const() for more information.
 */
void kunit_kfree_const(struct kunit *test, const void *x);

/**
 * kunit_kstrdup() - Duplicates a string into a test managed allocation.
 *
 * @test: The test context object.
 * @str: The NULL-terminated string to duplicate.
 * @gfp: flags passed to underlying kmalloc().
 *
 * See kstrdup() and kunit_kmalloc_array() for more information.
 */
static inline char *kunit_kstrdup(struct kunit *test, const char *str, gfp_t gfp)
{
	size_t len;
	char *buf;

	if (!str)
		return NULL;

	len = strlen(str) + 1;
	buf = kunit_kmalloc(test, len, gfp);
	if (buf)
		memcpy(buf, str, len);
	return buf;
}

/**
 * kunit_kstrdup_const() - Conditionally duplicates a string into a test managed allocation.
 *
 * @test: The test context object.
 * @str: The NULL-terminated string to duplicate.
 * @gfp: flags passed to underlying kmalloc().
 *
 * Calls kunit_kstrdup() only if @str is not in the rodata section. Must be freed with
 * kunit_kfree_const() -- not kunit_kfree().
 * See kstrdup_const() and kunit_kmalloc_array() for more information.
 */
const char *kunit_kstrdup_const(struct kunit *test, const char *str, gfp_t gfp);

/**
 * kunit_attach_mm() - Create and attach a new mm if it doesn't already exist.
 *
 * Allocates a &struct mm_struct and attaches it to @current. In most cases, call
 * kunit_vm_mmap() without calling kunit_attach_mm() directly. Only necessary when
 * code under test accesses the mm before executing the mmap (e.g., to perform
 * additional initialization beforehand).
 *
 * Return: 0 on success, -errno on failure.
 */
int kunit_attach_mm(void);

/**
 * kunit_vm_mmap() - Allocate KUnit-tracked vm_mmap() area
 * @test: The test context object.
 * @file: struct file pointer to map from, if any
 * @addr: desired address, if any
 * @len: how many bytes to allocate
 * @prot: mmap PROT_* bits
 * @flag: mmap flags
 * @offset: offset into @file to start mapping from.
 *
 * See vm_mmap() for more information.
 */
unsigned long kunit_vm_mmap(struct kunit *test, struct file *file,
			    unsigned long addr, unsigned long len,
			    unsigned long prot, unsigned long flag,
			    unsigned long offset);

void kunit_cleanup(struct kunit *test);
void kunit_free_boot_suites(void);

void __printf(2, 3) kunit_log_append(struct string_stream *log, const char *fmt, ...);

/**
 * kunit_mark_skipped() - Marks @test as skipped
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Marks the test as skipped. @fmt is given output as the test status
 * comment, typically the reason the test was skipped.
 *
 * Test execution continues after kunit_mark_skipped() is called.
 */
#define kunit_mark_skipped(test, fmt, ...)				\
	do {								\
		WRITE_ONCE((test)->status, KUNIT_SKIPPED);		\
		scnprintf((test)->status_comment,			\
			  KUNIT_STATUS_COMMENT_SIZE,			\
			  fmt, ##__VA_ARGS__);				\
	} while (0)

/**
 * kunit_skip() - Marks @test as skipped
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Skips the test. @fmt is given output as the test status
 * comment, typically the reason the test was skipped.
 *
 * Test execution is halted after kunit_skip() is called.
 */
#define kunit_skip(test, fmt, ...)					\
	do {								\
		kunit_mark_skipped((test), fmt, ##__VA_ARGS__);		\
		kunit_try_catch_throw(&((test)->try_catch));		\
	} while (0)

/*
 * printk and log to per-test or per-suite log buffer.  Logging only done
 * if CONFIG_KUNIT_DEBUGFS is 'y'; if it is 'n', no log is allocated/used.
 */
#define kunit_log(lvl, test_or_suite, fmt, ...)				\
	do {								\
		printk(lvl fmt, ##__VA_ARGS__);				\
		kunit_log_append((test_or_suite)->log,	fmt,		\
				 ##__VA_ARGS__);			\
	} while (0)

#define kunit_printk(lvl, test, fmt, ...)				\
	kunit_log(lvl, test, KUNIT_SUBTEST_INDENT "# %s: " fmt,		\
		  (test)->name,	##__VA_ARGS__)

/**
 * kunit_info() - Prints an INFO level message associated with @test.
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Prints an info level message associated with the test suite being run.
 * Takes a variable number of format parameters just like printk().
 */
#define kunit_info(test, fmt, ...) \
	kunit_printk(KERN_INFO, test, fmt, ##__VA_ARGS__)

/**
 * kunit_warn() - Prints a WARN level message associated with @test.
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Prints a warning level message.
 */
#define kunit_warn(test, fmt, ...) \
	kunit_printk(KERN_WARNING, test, fmt, ##__VA_ARGS__)

/**
 * kunit_err() - Prints an ERROR level message associated with @test.
 *
 * @test: The test context object.
 * @fmt:  A printk() style format string.
 *
 * Prints an error level message.
 */
#define kunit_err(test, fmt, ...) \
	kunit_printk(KERN_ERR, test, fmt, ##__VA_ARGS__)

/*
 * Must be called at the beginning of each KUNIT_*_ASSERTION().
 * Cf. KUNIT_CURRENT_LOC.
 */
#define _KUNIT_SAVE_LOC(test) do {					       \
	WRITE_ONCE(test->last_seen.file, __FILE__);			       \
	WRITE_ONCE(test->last_seen.line, __LINE__);			       \
} while (0)

/**
 * KUNIT_SUCCEED() - A no-op expectation. Only exists for code clarity.
 * @test: The test context object.
 *
 * The opposite of KUNIT_FAIL(), it is an expectation that cannot fail. In other
 * words, it does nothing and only exists for code clarity. See
 * KUNIT_EXPECT_TRUE() for more information.
 */
#define KUNIT_SUCCEED(test) _KUNIT_SAVE_LOC(test)

void __noreturn __kunit_abort(struct kunit *test);

void __printf(6, 7) __kunit_do_failed_assertion(struct kunit *test,
						const struct kunit_loc *loc,
						enum kunit_assert_type type,
						const struct kunit_assert *assert,
						assert_format_t assert_format,
						const char *fmt, ...);

#define _KUNIT_FAILED(test, assert_type, assert_class, assert_format, INITIALIZER, fmt, ...) do { \
	static const struct kunit_loc __loc = KUNIT_CURRENT_LOC;	       \
	const struct assert_class __assertion = INITIALIZER;		       \
	__kunit_do_failed_assertion(test,				       \
				    &__loc,				       \
				    assert_type,			       \
				    &__assertion.assert,		       \
				    assert_format,			       \
				    fmt,				       \
				    ##__VA_ARGS__);			       \
	if (assert_type == KUNIT_ASSERTION)				       \
		__kunit_abort(test);					       \
} while (0)


#define KUNIT_FAIL_ASSERTION(test, assert_type, fmt, ...) do {		       \
	_KUNIT_SAVE_LOC(test);						       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_fail_assert,				       \
		      kunit_fail_assert_format,				       \
		      {},						       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

/**
 * KUNIT_FAIL() - Always causes a test to fail when evaluated.
 * @test: The test context object.
 * @fmt: an informational message to be printed when the assertion is made.
 * @...: string format arguments.
 *
 * The opposite of KUNIT_SUCCEED(), it is an expectation that always fails. In
 * other words, it always results in a failed expectation, and consequently
 * always causes the test case to fail when evaluated. See KUNIT_EXPECT_TRUE()
 * for more information.
 */
#define KUNIT_FAIL(test, fmt, ...)					       \
	KUNIT_FAIL_ASSERTION(test,					       \
			     KUNIT_EXPECTATION,				       \
			     fmt,					       \
			     ##__VA_ARGS__)

/* Helper to safely pass around an initializer list to other macros. */
#define KUNIT_INIT_ASSERT(initializers...) { initializers }

#define KUNIT_UNARY_ASSERTION(test,					       \
			      assert_type,				       \
			      condition_,				       \
			      expected_true_,				       \
			      fmt,					       \
			      ...)					       \
do {									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (likely(!!(condition_) == !!expected_true_))			       \
		break;							       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_unary_assert,				       \
		      kunit_unary_assert_format,			       \
		      KUNIT_INIT_ASSERT(.condition = #condition_,	       \
					.expected_true = expected_true_),      \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

#define KUNIT_TRUE_MSG_ASSERTION(test, assert_type, condition, fmt, ...)       \
	KUNIT_UNARY_ASSERTION(test,					       \
			      assert_type,				       \
			      condition,				       \
			      true,					       \
			      fmt,					       \
			      ##__VA_ARGS__)

#define KUNIT_FALSE_MSG_ASSERTION(test, assert_type, condition, fmt, ...)      \
	KUNIT_UNARY_ASSERTION(test,					       \
			      assert_type,				       \
			      condition,				       \
			      false,					       \
			      fmt,					       \
			      ##__VA_ARGS__)

/*
 * A factory macro for defining the assertions and expectations for the basic
 * comparisons defined for the built in types.
 *
 * Unfortunately, there is no common type that all types can be promoted to for
 * which all the binary operators behave the same way as for the actual types
 * (for example, there is no type that long long and unsigned long long can
 * both be cast to where the comparison result is preserved for all values). So
 * the best we can do is do the comparison in the original types and then coerce
 * everything to long long for printing; this way, the comparison behaves
 * correctly and the printed out value usually makes sense without
 * interpretation, but can always be interpreted to figure out the actual
 * value.
 */
#define KUNIT_BASE_BINARY_ASSERTION(test,				       \
				    assert_class,			       \
				    format_func,			       \
				    assert_type,			       \
				    left,				       \
				    op,					       \
				    right,				       \
				    fmt,				       \
				    ...)				       \
do {									       \
	const typeof(left) __left = (left);				       \
	const typeof(right) __right = (right);				       \
	static const struct kunit_binary_assert_text __text = {		       \
		.operation = #op,					       \
		.left_text = #left,					       \
		.right_text = #right,					       \
	};								       \
									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (likely(__left op __right))					       \
		break;							       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      assert_class,					       \
		      format_func,					       \
		      KUNIT_INIT_ASSERT(.text = &__text,		       \
					.left_value = __left,		       \
					.right_value = __right),	       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

#define KUNIT_BINARY_INT_ASSERTION(test,				       \
				   assert_type,				       \
				   left,				       \
				   op,					       \
				   right,				       \
				   fmt,					       \
				    ...)				       \
	KUNIT_BASE_BINARY_ASSERTION(test,				       \
				    kunit_binary_assert,		       \
				    kunit_binary_assert_format,		       \
				    assert_type,			       \
				    left, op, right,			       \
				    fmt,				       \
				    ##__VA_ARGS__)

#define KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   assert_type,				       \
				   left,				       \
				   op,					       \
				   right,				       \
				   fmt,					       \
				    ...)				       \
	KUNIT_BASE_BINARY_ASSERTION(test,				       \
				    kunit_binary_ptr_assert,		       \
				    kunit_binary_ptr_assert_format,	       \
				    assert_type,			       \
				    left, op, right,			       \
				    fmt,				       \
				    ##__VA_ARGS__)

#define KUNIT_BINARY_STR_ASSERTION(test,				       \
				   assert_type,				       \
				   left,				       \
				   op,					       \
				   right,				       \
				   fmt,					       \
				   ...)					       \
do {									       \
	const char *__left = (left);					       \
	const char *__right = (right);					       \
	static const struct kunit_binary_assert_text __text = {		       \
		.operation = #op,					       \
		.left_text = #left,					       \
		.right_text = #right,					       \
	};								       \
									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (likely(!IS_ERR_OR_NULL(__left) && !IS_ERR_OR_NULL(__right) &&      \
	    (strcmp(__left, __right) op 0)))				       \
		break;							       \
									       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_binary_str_assert,				       \
		      kunit_binary_str_assert_format,			       \
		      KUNIT_INIT_ASSERT(.text = &__text,		       \
					.left_value = __left,		       \
					.right_value = __right),	       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

#define KUNIT_MEM_ASSERTION(test,					       \
			    assert_type,				       \
			    left,					       \
			    op,						       \
			    right,					       \
			    size_,					       \
			    fmt,					       \
			    ...)					       \
do {									       \
	const void *__left = (left);					       \
	const void *__right = (right);					       \
	const size_t __size = (size_);					       \
	static const struct kunit_binary_assert_text __text = {		       \
		.operation = #op,					       \
		.left_text = #left,					       \
		.right_text = #right,					       \
	};								       \
									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (likely(__left && __right))					       \
		if (likely(memcmp(__left, __right, __size) op 0))	       \
			break;						       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_mem_assert,					       \
		      kunit_mem_assert_format,				       \
		      KUNIT_INIT_ASSERT(.text = &__text,		       \
					.left_value = __left,		       \
					.right_value = __right,		       \
					.size = __size),		       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

#define KUNIT_PTR_NOT_ERR_OR_NULL_MSG_ASSERTION(test,			       \
						assert_type,		       \
						ptr,			       \
						fmt,			       \
						...)			       \
do {									       \
	const typeof(ptr) __ptr = (ptr);				       \
									       \
	_KUNIT_SAVE_LOC(test);						       \
	if (!IS_ERR_OR_NULL(__ptr))					       \
		break;							       \
									       \
	_KUNIT_FAILED(test,						       \
		      assert_type,					       \
		      kunit_ptr_not_err_assert,				       \
		      kunit_ptr_not_err_assert_format,			       \
		      KUNIT_INIT_ASSERT(.text = #ptr, .value = __ptr),	       \
		      fmt,						       \
		      ##__VA_ARGS__);					       \
} while (0)

/**
 * KUNIT_EXPECT_TRUE() - Causes a test failure when the expression is not true.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression. The test fails when this does
 * not evaluate to true.
 *
 * This and expectations of the form `KUNIT_EXPECT_*` will cause the test case
 * to fail when the specified condition is not met; however, it will not prevent
 * the test case from continuing to run; this is otherwise known as an
 * *expectation failure*.
 */
#define KUNIT_EXPECT_TRUE(test, condition) \
	KUNIT_EXPECT_TRUE_MSG(test, condition, NULL)

#define KUNIT_EXPECT_TRUE_MSG(test, condition, fmt, ...)		       \
	KUNIT_TRUE_MSG_ASSERTION(test,					       \
				 KUNIT_EXPECTATION,			       \
				 condition,				       \
				 fmt,					       \
				 ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_FALSE() - Makes a test failure when the expression is not false.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression. The test fails when this does
 * not evaluate to false.
 *
 * Sets an expectation that @condition evaluates to false. See
 * KUNIT_EXPECT_TRUE() for more information.
 */
#define KUNIT_EXPECT_FALSE(test, condition) \
	KUNIT_EXPECT_FALSE_MSG(test, condition, NULL)

#define KUNIT_EXPECT_FALSE_MSG(test, condition, fmt, ...)		       \
	KUNIT_FALSE_MSG_ASSERTION(test,					       \
				  KUNIT_EXPECTATION,			       \
				  condition,				       \
				  fmt,					       \
				  ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_EQ() - Sets an expectation that @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) == (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_EQ(test, left, right) \
	KUNIT_EXPECT_EQ_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_EQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_PTR_EQ() - Expects that pointers @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a pointer.
 * @right: an arbitrary expression that evaluates to a pointer.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) == (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_PTR_EQ(test, left, right)				       \
	KUNIT_EXPECT_PTR_EQ_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_PTR_EQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_NE() - An expectation that @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the values that @left and @right evaluate to are not
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) != (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_NE(test, left, right) \
	KUNIT_EXPECT_NE_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_NE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_PTR_NE() - Expects that pointers @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a pointer.
 * @right: an arbitrary expression that evaluates to a pointer.
 *
 * Sets an expectation that the values that @left and @right evaluate to are not
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) != (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_PTR_NE(test, left, right)				       \
	KUNIT_EXPECT_PTR_NE_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_PTR_NE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_LT() - An expectation that @left is less than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is less than the
 * value that @right evaluates to. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) < (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_LT(test, left, right) \
	KUNIT_EXPECT_LT_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_LT_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, <, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_LE() - Expects that @left is less than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is less than or
 * equal to the value that @right evaluates to. Semantically this is equivalent
 * to KUNIT_EXPECT_TRUE(@test, (@left) <= (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_LE(test, left, right) \
	KUNIT_EXPECT_LE_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_LE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, <=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_GT() - An expectation that @left is greater than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is greater than
 * the value that @right evaluates to. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) > (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_GT(test, left, right) \
	KUNIT_EXPECT_GT_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_GT_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, >, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_GE() - Expects that @left is greater than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an expectation that the value that @left evaluates to is greater than
 * the value that @right evaluates to. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, (@left) >= (@right)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_GE(test, left, right) \
	KUNIT_EXPECT_GE_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_GE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, >=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_STREQ() - Expects that strings @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, !strcmp((@left), (@right))). See KUNIT_EXPECT_TRUE()
 * for more information.
 */
#define KUNIT_EXPECT_STREQ(test, left, right) \
	KUNIT_EXPECT_STREQ_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_STREQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_STR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_STRNEQ() - Expects that strings @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * not equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, strcmp((@left), (@right))). See KUNIT_EXPECT_TRUE()
 * for more information.
 */
#define KUNIT_EXPECT_STRNEQ(test, left, right) \
	KUNIT_EXPECT_STRNEQ_MSG(test, left, right, NULL)

#define KUNIT_EXPECT_STRNEQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_STR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_MEMEQ() - Expects that the first @size bytes of @left and @right are equal.
 * @test: The test context object.
 * @left: An arbitrary expression that evaluates to the specified size.
 * @right: An arbitrary expression that evaluates to the specified size.
 * @size: Number of bytes compared.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, !memcmp((@left), (@right), (@size))). See
 * KUNIT_EXPECT_TRUE() for more information.
 *
 * Although this expectation works for any memory block, it is not recommended
 * for comparing more structured data, such as structs. This expectation is
 * recommended for comparing, for example, data arrays.
 */
#define KUNIT_EXPECT_MEMEQ(test, left, right, size) \
	KUNIT_EXPECT_MEMEQ_MSG(test, left, right, size, NULL)

#define KUNIT_EXPECT_MEMEQ_MSG(test, left, right, size, fmt, ...)	       \
	KUNIT_MEM_ASSERTION(test,					       \
			    KUNIT_EXPECTATION,				       \
			    left, ==, right,				       \
			    size,					       \
			    fmt,					       \
			    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_MEMNEQ() - Expects that the first @size bytes of @left and @right are not equal.
 * @test: The test context object.
 * @left: An arbitrary expression that evaluates to the specified size.
 * @right: An arbitrary expression that evaluates to the specified size.
 * @size: Number of bytes compared.
 *
 * Sets an expectation that the values that @left and @right evaluate to are
 * not equal. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, memcmp((@left), (@right), (@size))). See
 * KUNIT_EXPECT_TRUE() for more information.
 *
 * Although this expectation works for any memory block, it is not recommended
 * for comparing more structured data, such as structs. This expectation is
 * recommended for comparing, for example, data arrays.
 */
#define KUNIT_EXPECT_MEMNEQ(test, left, right, size) \
	KUNIT_EXPECT_MEMNEQ_MSG(test, left, right, size, NULL)

#define KUNIT_EXPECT_MEMNEQ_MSG(test, left, right, size, fmt, ...)	       \
	KUNIT_MEM_ASSERTION(test,					       \
			    KUNIT_EXPECTATION,				       \
			    left, !=, right,				       \
			    size,					       \
			    fmt,					       \
			    ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_NULL() - Expects that @ptr is null.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an expectation that the value that @ptr evaluates to is null. This is
 * semantically equivalent to KUNIT_EXPECT_PTR_EQ(@test, ptr, NULL).
 * See KUNIT_EXPECT_TRUE() for more information.
 */
#define KUNIT_EXPECT_NULL(test, ptr)				               \
	KUNIT_EXPECT_NULL_MSG(test,					       \
			      ptr,					       \
			      NULL)

#define KUNIT_EXPECT_NULL_MSG(test, ptr, fmt, ...)	                       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   ptr, ==, NULL,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_NOT_NULL() - Expects that @ptr is not null.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an expectation that the value that @ptr evaluates to is not null. This
 * is semantically equivalent to KUNIT_EXPECT_PTR_NE(@test, ptr, NULL).
 * See KUNIT_EXPECT_TRUE() for more information.
 */
#define KUNIT_EXPECT_NOT_NULL(test, ptr)			               \
	KUNIT_EXPECT_NOT_NULL_MSG(test,					       \
				  ptr,					       \
				  NULL)

#define KUNIT_EXPECT_NOT_NULL_MSG(test, ptr, fmt, ...)	                       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_EXPECTATION,			       \
				   ptr, !=, NULL,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_EXPECT_NOT_ERR_OR_NULL() - Expects that @ptr is not null and not err.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an expectation that the value that @ptr evaluates to is not null and not
 * an errno stored in a pointer. This is semantically equivalent to
 * KUNIT_EXPECT_TRUE(@test, !IS_ERR_OR_NULL(@ptr)). See KUNIT_EXPECT_TRUE() for
 * more information.
 */
#define KUNIT_EXPECT_NOT_ERR_OR_NULL(test, ptr) \
	KUNIT_EXPECT_NOT_ERR_OR_NULL_MSG(test, ptr, NULL)

#define KUNIT_EXPECT_NOT_ERR_OR_NULL_MSG(test, ptr, fmt, ...)		       \
	KUNIT_PTR_NOT_ERR_OR_NULL_MSG_ASSERTION(test,			       \
						KUNIT_EXPECTATION,	       \
						ptr,			       \
						fmt,			       \
						##__VA_ARGS__)

/**
 * KUNIT_FAIL_AND_ABORT() - Always causes a test to fail and abort when evaluated.
 * @test: The test context object.
 * @fmt: an informational message to be printed when the assertion is made.
 * @...: string format arguments.
 *
 * The opposite of KUNIT_SUCCEED(), it is an assertion that always fails. In
 * other words, it always results in a failed assertion, and consequently
 * always causes the test case to fail and abort when evaluated.
 * See KUNIT_ASSERT_TRUE() for more information.
 */
#define KUNIT_FAIL_AND_ABORT(test, fmt, ...) \
	KUNIT_FAIL_ASSERTION(test, KUNIT_ASSERTION, fmt, ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_TRUE() - Sets an assertion that @condition is true.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression. The test fails and aborts when
 * this does not evaluate to true.
 *
 * This and assertions of the form `KUNIT_ASSERT_*` will cause the test case to
 * fail *and immediately abort* when the specified condition is not met. Unlike
 * an expectation failure, it will prevent the test case from continuing to run;
 * this is otherwise known as an *assertion failure*.
 */
#define KUNIT_ASSERT_TRUE(test, condition) \
	KUNIT_ASSERT_TRUE_MSG(test, condition, NULL)

#define KUNIT_ASSERT_TRUE_MSG(test, condition, fmt, ...)		       \
	KUNIT_TRUE_MSG_ASSERTION(test,					       \
				 KUNIT_ASSERTION,			       \
				 condition,				       \
				 fmt,					       \
				 ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_FALSE() - Sets an assertion that @condition is false.
 * @test: The test context object.
 * @condition: an arbitrary boolean expression.
 *
 * Sets an assertion that the value that @condition evaluates to is false. This
 * is the same as KUNIT_EXPECT_FALSE(), except it causes an assertion failure
 * (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_FALSE(test, condition) \
	KUNIT_ASSERT_FALSE_MSG(test, condition, NULL)

#define KUNIT_ASSERT_FALSE_MSG(test, condition, fmt, ...)		       \
	KUNIT_FALSE_MSG_ASSERTION(test,					       \
				  KUNIT_ASSERTION,			       \
				  condition,				       \
				  fmt,					       \
				  ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_EQ() - Sets an assertion that @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal. This is the same as KUNIT_EXPECT_EQ(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_EQ(test, left, right) \
	KUNIT_ASSERT_EQ_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_EQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_PTR_EQ() - Asserts that pointers @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a pointer.
 * @right: an arbitrary expression that evaluates to a pointer.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal. This is the same as KUNIT_EXPECT_EQ(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_PTR_EQ(test, left, right) \
	KUNIT_ASSERT_PTR_EQ_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_PTR_EQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_NE() - An assertion that @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the values that @left and @right evaluate to are not
 * equal. This is the same as KUNIT_EXPECT_NE(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_NE(test, left, right) \
	KUNIT_ASSERT_NE_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_NE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_PTR_NE() - Asserts that pointers @left and @right are not equal.
 * KUNIT_ASSERT_PTR_EQ() - Asserts that pointers @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a pointer.
 * @right: an arbitrary expression that evaluates to a pointer.
 *
 * Sets an assertion that the values that @left and @right evaluate to are not
 * equal. This is the same as KUNIT_EXPECT_NE(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_PTR_NE(test, left, right) \
	KUNIT_ASSERT_PTR_NE_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_PTR_NE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)
/**
 * KUNIT_ASSERT_LT() - An assertion that @left is less than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is less than the
 * value that @right evaluates to. This is the same as KUNIT_EXPECT_LT(), except
 * it causes an assertion failure (see KUNIT_ASSERT_TRUE()) when the assertion
 * is not met.
 */
#define KUNIT_ASSERT_LT(test, left, right) \
	KUNIT_ASSERT_LT_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_LT_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, <, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)
/**
 * KUNIT_ASSERT_LE() - An assertion that @left is less than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is less than or
 * equal to the value that @right evaluates to. This is the same as
 * KUNIT_EXPECT_LE(), except it causes an assertion failure (see
 * KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_LE(test, left, right) \
	KUNIT_ASSERT_LE_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_LE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, <=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_GT() - An assertion that @left is greater than @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is greater than the
 * value that @right evaluates to. This is the same as KUNIT_EXPECT_GT(), except
 * it causes an assertion failure (see KUNIT_ASSERT_TRUE()) when the assertion
 * is not met.
 */
#define KUNIT_ASSERT_GT(test, left, right) \
	KUNIT_ASSERT_GT_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_GT_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, >, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_GE() - Assertion that @left is greater than or equal to @right.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a primitive C type.
 * @right: an arbitrary expression that evaluates to a primitive C type.
 *
 * Sets an assertion that the value that @left evaluates to is greater than the
 * value that @right evaluates to. This is the same as KUNIT_EXPECT_GE(), except
 * it causes an assertion failure (see KUNIT_ASSERT_TRUE()) when the assertion
 * is not met.
 */
#define KUNIT_ASSERT_GE(test, left, right) \
	KUNIT_ASSERT_GE_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_GE_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_INT_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, >=, right,			       \
				   fmt,					       \
				    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_STREQ() - An assertion that strings @left and @right are equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal. This is the same as KUNIT_EXPECT_STREQ(), except it causes an
 * assertion failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_STREQ(test, left, right) \
	KUNIT_ASSERT_STREQ_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_STREQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_STR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, ==, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_STRNEQ() - An assertion that strings @left and @right are not equal.
 * @test: The test context object.
 * @left: an arbitrary expression that evaluates to a null terminated string.
 * @right: an arbitrary expression that evaluates to a null terminated string.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * not equal. This is semantically equivalent to
 * KUNIT_ASSERT_TRUE(@test, strcmp((@left), (@right))). See KUNIT_ASSERT_TRUE()
 * for more information.
 */
#define KUNIT_ASSERT_STRNEQ(test, left, right) \
	KUNIT_ASSERT_STRNEQ_MSG(test, left, right, NULL)

#define KUNIT_ASSERT_STRNEQ_MSG(test, left, right, fmt, ...)		       \
	KUNIT_BINARY_STR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   left, !=, right,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_MEMEQ() - Asserts that the first @size bytes of @left and @right are equal.
 * @test: The test context object.
 * @left: An arbitrary expression that evaluates to the specified size.
 * @right: An arbitrary expression that evaluates to the specified size.
 * @size: Number of bytes compared.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * equal. This is semantically equivalent to
 * KUNIT_ASSERT_TRUE(@test, !memcmp((@left), (@right), (@size))). See
 * KUNIT_ASSERT_TRUE() for more information.
 *
 * Although this assertion works for any memory block, it is not recommended
 * for comparing more structured data, such as structs. This assertion is
 * recommended for comparing, for example, data arrays.
 */
#define KUNIT_ASSERT_MEMEQ(test, left, right, size) \
	KUNIT_ASSERT_MEMEQ_MSG(test, left, right, size, NULL)

#define KUNIT_ASSERT_MEMEQ_MSG(test, left, right, size, fmt, ...)	       \
	KUNIT_MEM_ASSERTION(test,					       \
			    KUNIT_ASSERTION,				       \
			    left, ==, right,				       \
			    size,					       \
			    fmt,					       \
			    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_MEMNEQ() - Asserts that the first @size bytes of @left and @right are not equal.
 * @test: The test context object.
 * @left: An arbitrary expression that evaluates to the specified size.
 * @right: An arbitrary expression that evaluates to the specified size.
 * @size: Number of bytes compared.
 *
 * Sets an assertion that the values that @left and @right evaluate to are
 * not equal. This is semantically equivalent to
 * KUNIT_ASSERT_TRUE(@test, memcmp((@left), (@right), (@size))). See
 * KUNIT_ASSERT_TRUE() for more information.
 *
 * Although this assertion works for any memory block, it is not recommended
 * for comparing more structured data, such as structs. This assertion is
 * recommended for comparing, for example, data arrays.
 */
#define KUNIT_ASSERT_MEMNEQ(test, left, right, size) \
	KUNIT_ASSERT_MEMNEQ_MSG(test, left, right, size, NULL)

#define KUNIT_ASSERT_MEMNEQ_MSG(test, left, right, size, fmt, ...)	       \
	KUNIT_MEM_ASSERTION(test,					       \
			    KUNIT_ASSERTION,				       \
			    left, !=, right,				       \
			    size,					       \
			    fmt,					       \
			    ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_NULL() - Asserts that pointers @ptr is null.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an assertion that the values that @ptr evaluates to is null. This is
 * the same as KUNIT_EXPECT_NULL(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_NULL(test, ptr) \
	KUNIT_ASSERT_NULL_MSG(test,					       \
			      ptr,					       \
			      NULL)

#define KUNIT_ASSERT_NULL_MSG(test, ptr, fmt, ...) \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   ptr, ==, NULL,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_NOT_NULL() - Asserts that pointers @ptr is not null.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an assertion that the values that @ptr evaluates to is not null. This
 * is the same as KUNIT_EXPECT_NOT_NULL(), except it causes an assertion
 * failure (see KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_NOT_NULL(test, ptr) \
	KUNIT_ASSERT_NOT_NULL_MSG(test,					       \
				  ptr,					       \
				  NULL)

#define KUNIT_ASSERT_NOT_NULL_MSG(test, ptr, fmt, ...) \
	KUNIT_BINARY_PTR_ASSERTION(test,				       \
				   KUNIT_ASSERTION,			       \
				   ptr, !=, NULL,			       \
				   fmt,					       \
				   ##__VA_ARGS__)

/**
 * KUNIT_ASSERT_NOT_ERR_OR_NULL() - Assertion that @ptr is not null and not err.
 * @test: The test context object.
 * @ptr: an arbitrary pointer.
 *
 * Sets an assertion that the value that @ptr evaluates to is not null and not
 * an errno stored in a pointer. This is the same as
 * KUNIT_EXPECT_NOT_ERR_OR_NULL(), except it causes an assertion failure (see
 * KUNIT_ASSERT_TRUE()) when the assertion is not met.
 */
#define KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ptr) \
	KUNIT_ASSERT_NOT_ERR_OR_NULL_MSG(test, ptr, NULL)

#define KUNIT_ASSERT_NOT_ERR_OR_NULL_MSG(test, ptr, fmt, ...)		       \
	KUNIT_PTR_NOT_ERR_OR_NULL_MSG_ASSERTION(test,			       \
						KUNIT_ASSERTION,	       \
						ptr,			       \
						fmt,			       \
						##__VA_ARGS__)

/**
 * KUNIT_ARRAY_PARAM() - Define test parameter generator from an array.
 * @name:  prefix for the test parameter generator function.
 * @array: array of test parameters.
 * @get_desc: function to convert param to description; NULL to use default
 *
 * Define function @name_gen_params which uses @array to generate parameters.
 */
#define KUNIT_ARRAY_PARAM(name, array, get_desc)						\
	static const void *name##_gen_params(struct kunit *test,				\
					     const void *prev, char *desc)			\
	{											\
		typeof((array)[0]) *__next = prev ? ((typeof(__next)) prev) + 1 : (array);	\
		if (!prev)									\
			kunit_register_params_array(test, array, ARRAY_SIZE(array), NULL);	\
		if (__next - (array) < ARRAY_SIZE((array))) {					\
			void (*__get_desc)(typeof(__next), char *) = get_desc;			\
			if (__get_desc)								\
				__get_desc(__next, desc);					\
			return __next;								\
		}										\
		return NULL;									\
	}

/**
 * KUNIT_ARRAY_PARAM_DESC() - Define test parameter generator from an array.
 * @name:  prefix for the test parameter generator function.
 * @array: array of test parameters.
 * @desc_member: structure member from array element to use as description
 *
 * Define function @name_gen_params which uses @array to generate parameters.
 */
#define KUNIT_ARRAY_PARAM_DESC(name, array, desc_member)					\
	static const void *name##_gen_params(struct kunit *test,				\
					     const void *prev, char *desc)			\
	{											\
		typeof((array)[0]) *__next = prev ? ((typeof(__next)) prev) + 1 : (array);	\
		if (!prev)									\
			kunit_register_params_array(test, array, ARRAY_SIZE(array), NULL);	\
		if (__next - (array) < ARRAY_SIZE((array))) {					\
			strscpy(desc, __next->desc_member, KUNIT_PARAM_DESC_SIZE);		\
			return __next;								\
		}										\
		return NULL;									\
	}

/**
 * kunit_register_params_array() - Register parameter array for a KUnit test.
 * @test: The KUnit test structure to which parameters will be added.
 * @array: An array of test parameters.
 * @param_count: Number of parameters.
 * @get_desc: Function that generates a string description for a given parameter
 * element.
 *
 * This macro initializes the @test's parameter array data, storing information
 * including the parameter array, its count, the element size, and the parameter
 * description function within `test->params_array`.
 *
 * Note: If using this macro in param_init(), kunit_array_gen_params()
 * will then need to be manually provided as the parameter generator function to
 * KUNIT_CASE_PARAM_WITH_INIT(). kunit_array_gen_params() is a KUnit
 * function that uses the registered array to generate parameters
 */
#define kunit_register_params_array(test, array, param_count, get_desc)				\
	do {											\
		struct kunit *_test = (test);							\
		const typeof((array)[0]) * _params_ptr = &(array)[0];				\
		_test->params_array.params = _params_ptr;					\
		_test->params_array.num_params = (param_count);					\
		_test->params_array.elem_size = sizeof(*_params_ptr);				\
		_test->params_array.get_description = (get_desc);				\
	} while (0)

// TODO(dlatypov@google.com): consider eventually migrating users to explicitly
// include resource.h themselves if they need it.
#include <kunit/resource.h>

/*
 * Warning backtrace suppression API.
 *
 * Suppresses WARN*() backtraces on the current task while active. Two forms
 * are provided:
 *
 * - Scoped: kunit_warning_suppress(test) { ... }
 *   Suppression is active for the duration of the block. On normal exit,
 *   the for-loop increment deactivates suppression. On early exit (break,
 *   return, goto), the __cleanup attribute fires. On kthread_exit() (e.g.,
 *   a failed KUnit assertion), kunit_add_action() cleans up at test
 *   teardown. The suppression handle is only accessible inside the block,
 *   so warning counts must be checked before the block exits.
 *
 * - Direct: kunit_start_suppress_warning() / kunit_end_suppress_warning()
 *   The underlying functions, returning an explicit handle pointer. Use
 *   when the handle needs to be retained (e.g., for post-suppression
 *   count checks) or passed across helper functions.
 */
struct kunit_suppressed_warning;

struct kunit_suppressed_warning *
kunit_start_suppress_warning(struct kunit *test);
void kunit_end_suppress_warning(struct kunit *test,
				struct kunit_suppressed_warning *w);
int kunit_suppressed_warning_count(struct kunit_suppressed_warning *w);
void __kunit_suppress_auto_cleanup(struct kunit_suppressed_warning **wp);
bool kunit_has_active_suppress_warning(void);

/**
 * kunit_warning_suppress() - Suppress WARN*() backtraces for the duration
 *                            of a block.
 * @test: The test context object.
 *
 * Scoped form of the suppression API. Suppression starts when the block is
 * entered and ends automatically when the block exits through any path. See
 * the section comment above for the cleanup guarantees on each exit path.
 * Fails the test if suppression is already active; nesting is not supported.
 *
 * The warning count can be checked inside the block via
 * KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(). The handle is not accessible
 * after the block exits.
 *
 * Example::
 *
 *   kunit_warning_suppress(test) {
 *       trigger_warning();
 *       KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, 1);
 *   }
 */
#define kunit_warning_suppress(test)					\
	for (struct kunit_suppressed_warning *__kunit_suppress		\
	     __cleanup(__kunit_suppress_auto_cleanup) =			\
	     kunit_start_suppress_warning(test);			\
	     __kunit_suppress;						\
	     kunit_end_suppress_warning(test, __kunit_suppress),	\
	     __kunit_suppress = NULL)

/**
 * KUNIT_SUPPRESSED_WARNING_COUNT() - Returns the suppressed warning count.
 *
 * Returns the number of WARN*() calls suppressed since the current
 * suppression block started, or 0 if the handle is NULL. Usable inside a
 * kunit_warning_suppress() block.
 */
#define KUNIT_SUPPRESSED_WARNING_COUNT() \
	kunit_suppressed_warning_count(__kunit_suppress)

/**
 * KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT() - Sets an expectation that the
 *                                           suppressed warning count equals
 *                                           @expected.
 * @test: The test context object.
 * @expected: an expression that evaluates to the expected warning count.
 *
 * Sets an expectation that the number of suppressed WARN*() calls equals
 * @expected. This is semantically equivalent to
 * KUNIT_EXPECT_EQ(@test, KUNIT_SUPPRESSED_WARNING_COUNT(), @expected).
 * See KUNIT_EXPECT_EQ() for more information.
 */
#define KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(test, expected) \
	KUNIT_EXPECT_EQ(test, KUNIT_SUPPRESSED_WARNING_COUNT(), expected)

/**
 * KUNIT_ASSERT_SUPPRESSED_WARNING_COUNT() - Sets an assertion that the
 *                                           suppressed warning count equals
 *                                           @expected.
 * @test: The test context object.
 * @expected: an expression that evaluates to the expected warning count.
 *
 * Sets an assertion that the number of suppressed WARN*() calls equals
 * @expected. This is the same as KUNIT_EXPECT_SUPPRESSED_WARNING_COUNT(),
 * except it causes an assertion failure (see KUNIT_ASSERT_TRUE()) when the
 * assertion is not met.
 */
#define KUNIT_ASSERT_SUPPRESSED_WARNING_COUNT(test, expected) \
	KUNIT_ASSERT_EQ(test, KUNIT_SUPPRESSED_WARNING_COUNT(), expected)

#endif /* _KUNIT_TEST_H */
