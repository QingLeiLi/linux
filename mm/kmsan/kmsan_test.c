// SPDX-License-Identifier: GPL-2.0
/*
 * Test cases for KMSAN.
 * For each test case checks the presence (or absence) of generated reports.
 * Relies on 'console' tracepoint to capture reports as they appear in the
 * kernel log.
 *
 * Copyright (C) 2021-2022, Google LLC.
 * Author: Alexander Potapenko <glider@google.com>
 *
 */
/*
 * 原注释译注：这是 KMSAN 的测试用例；每项检查生成报告是否存在或不存在，依赖
 * console tracepoint 捕获内核日志。文件测试 KMSAN 的报告契约，不实现 sanitizer。
 * 版权与作者信息属于上游历史材料；以下补注不改变其归属或测试的运行配置。
 */

#include <kunit/test.h>
#include "kmsan.h"

#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kmsan.h>
#include <linux/mm.h>
#include <linux/random.h>
#include <linux/slab.h>
/* 内存分配和锁依赖在此成组引入；下方报告快照使用 spinlock，测试 case 使用 slab/vmalloc 页面对象。 */
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/vmalloc.h>
#include <trace/events/printk.h>

/* 依赖分层：KUnit 提供断言，kmsan.h 提供内部常量，其余头分别提供分配、映射、trace 与字符串 API。 */
/*
 * 这个每 CPU 槽是传播测试的中转站：写入本 CPU 后立即在同一 CPU 读取，
 * 因而报告只能来自 KMSAN 对值及其 shadow/origin 的复制，而不是跨 CPU
 * 同步或远端可见性。DEFINE_PER_CPU() 让每个处理器拥有独立实例；测试不能
 * 保存该实例地址并跨越迁移边界使用它。
 */
static DEFINE_PER_CPU(int, per_cpu_var);

/* Report as observed from console. */
/*
 * console tracepoint 观察到的一份报告快照。suite_init() 注册 probe，
 * suite_exit() 注销并等待旧 probe 退出；所以本对象只在套件存活期发布给
 * tracepoint 回调和串行 KUnit case。lock 同时保护 header、ignore 与它们的
 * 状态组合，available 用 READ_ONCE/WRITE_ONCE 允许锁外的快速轮询。
 */
static struct {
	spinlock_t lock;
	bool available;
	bool ignore; /* Stop console output collection. */
	char header[256];
} observed = {
	.lock = __SPIN_LOCK_UNLOCKED(observed.lock),
};

/* Probe for console output: obtains observed lines of interest. */
/*
 * 业务背景：KMSAN 通常把诊断写入 console，而每个 KUnit case 需要把预期
 * UMR/UAF 转为可断言的状态；此 tracepoint probe 是报告路径和测试之间的
 * 观察适配层，而非产生报告的 KMSAN 实现。
 * 入参：ignore 是 tracepoint 的私有数据，本套件传 NULL 且不解引用；buf 是
 * 非 NUL 终止的借用日志片段，len 是其有效字节数，均仅在本回调期间有效。
 * 出参/返回：无直接返回值；首个含 "BUG: KMSAN:" 的片段被复制到 observed，
 * 并发布 available。其它片段和后续报告保持不影响当前 case 的首份证据。
 * 注意事项：可能在 console/tracepoint 上下文执行，不能睡眠；irqsave 锁与
 * report_reset()/report_matches() 配对，避免 header 与标志被不同 case 交错。
 */
static void probe_console(void *ignore, const char *buf, size_t len)
{
	unsigned long flags;

	/* 已捕获首份报告的快速路径不取锁；写者在锁内设置 ignore。 */
	if (observed.ignore)
		return;
	spin_lock_irqsave(&observed.lock, flags);

	if (strnstr(buf, "BUG: KMSAN: ", len)) {
		/*
		 * KMSAN report and related to the test.
		 *
		 * The provided @buf is not NUL-terminated; copy no more than
		 * @len bytes and let strscpy() add the missing NUL-terminator.
		 */
		/*
		 * 原注释译注：这是一条关联当前测试的 KMSAN 报告；buf 不保证 NUL
		 * 终止，故最多复制 len 字节并由 strscpy() 补终止符。min() 同时把
		 * 拷贝限制在 header 容量内，避免 tracepoint 的瞬时缓冲区越界读取。
		 */
		strscpy(observed.header, buf,
			min(len + 1, sizeof(observed.header)));
		/* header 完整写入后才发布 available；比对者取得同一把锁再消费文本。 */
		WRITE_ONCE(observed.available, true);
		observed.ignore = true;
	}
	spin_unlock_irqrestore(&observed.lock, flags);
}

/* Check if a report related to the test exists. */
/*
 * 业务背景：报告是否到达是多数 KUnit case 的共同快速断言条件，单独包装可
 * 使锁外读取遵守与 probe 发布相同的单次访问规则。
 * 入参：无。
 * 出参/返回：true 表示已有首份捕获报告，false 表示尚无；不转移任何对象。
 * 注意事项：只保证 bool 的无数据竞争读取，不保证 header 内容稳定；需要
 * 文本匹配的调用者必须进入 report_matches() 的锁保护阶段。
 */
static bool report_available(void)
{
	return READ_ONCE(observed.available);
}

/* Reset observed.available, so that the test can trigger another report. */
/*
 * 业务背景：一个 case 可故意触发多次检查；每次消费首份报告后需重新打开
 * 收集窗口，才能验证下一段 shadow/origin 的独立结果。
 * 入参：无。
 * 出参/返回：无直接返回值；available 清零且 ignore 清除，旧 header 仅作为
 * 不可见残留，下一次发布会覆盖它。
 * 注意事项：spin_lock_irqsave 与 probe 的同一锁配对，不能睡眠；必须在已
 * 完成上一次 report_matches() 后调用，否则会丢弃尚未断言的报告。
 */
static void report_reset(void)
{
	unsigned long flags;

	spin_lock_irqsave(&observed.lock, flags);
	WRITE_ONCE(observed.available, false);
	observed.ignore = false;
	spin_unlock_irqrestore(&observed.lock, flags);
}

/* Information we expect in a report. */
/*
 * 每个 case 的最小预期：error_type 对应 KMSAN 标题中的分类，symbol 是应
 * 出现在标题中的报告位置。宏在 case 栈上创建它，report_matches() 仅借用，
 * 不保存指针；symbol 为 NULL 明确表示该 case 期待没有报告。
 */
struct expect_report {
	const char *error_type; /* Error type. */
	/* error_type 是报告标题中的分类字符串，供每个 case 指定预期。 */
	/*
	 * Kernel symbol from the error header, or NULL if no report is
	 * expected.
	 */
	/*
	 * 原注释译注：报告标题中的内核符号；NULL 表示本 case 预期没有报告。
	 * 它是静态函数名或字符串字面量的借用指针，必须在断言完成前保持有效。
	 */
	const char *symbol;
};

/* Check observed report matches information in @r. */
/*
 * 业务背景：KUnit 不能只知道“出现某条日志”，还须确认 KMSAN 把错误归因于
 * 当前 case；本函数把 console 快照和 case 的最小报告契约比对。
 * 入参：r 是非空、由调用 case 持有的预期；其中 symbol=NULL 表示无报告，
 * error_type/symbol 均只读借用。
 * 出参/返回：true 表示“有报告且标题匹配”或“无报告且明确预期无报告”；
 * false 表示缺报、意外报告或归因不符，不修改 KMSAN 元数据。
 * 注意事项：先用 READ_ONCE 走快速判定，再取 observed.lock 稳定 header；
 * console 回调可能正写入，锁内二次检查避免把尚未完成的捕获误判为失败。
 */
static bool report_matches(const struct expect_report *r)
{
	typeof(observed.header) expected_header;
	unsigned long flags;
	bool ret = false;
	const char *end;
	char *cur;

	/* Doubled-checked locking. */
	/* 原注释译注：双重检查锁定；无报告/期待无报告时避免不必要地关中断取锁。 */
	if (!report_available() || !r->symbol)
		return (!report_available() && !r->symbol);

	/* Generate expected report contents. */
	/* 在栈上构造可比较前缀，绝不修改 tracepoint 所有的 observed.header。 */

	/* Title */
	/* 第一阶段：标题必须使用 KMSAN 的稳定前缀与本 case 指定的错误类型。 */
	cur = expected_header;
	end = ARRAY_END(expected_header);

	cur += scnprintf(cur, end - cur, "BUG: KMSAN: %s", r->error_type);

	scnprintf(cur, end - cur, " in %s", r->symbol);
	/* The exact offset won't match, remove it; also strip module name. */
	/* 原注释译注：偏移和模块名随链接布局变化，截到 '+' 前以只比较稳定符号。 */
	cur = strchr(expected_header, '+');
	if (cur)
		*cur = '\0';

	spin_lock_irqsave(&observed.lock, flags);
	if (!report_available())
		/* 新报告正在捕获，不能读取尚未发布完整的 header。 */
		goto out; /* A new report is being captured. */

	/* Finally match expected output to what we actually observed. */
	ret = strstr(observed.header, expected_header);
out:
	spin_unlock_irqrestore(&observed.lock, flags);

	return ret;
}

/* ===== Test cases ===== */
/*
 * 以下 case 从分配、栈、参数传递、页/虚拟内存到 memcpy origin 逐层验证：
 * KMSAN 插桩应把未初始化状态传播到真正的使用点，初始化/反毒路径不应误报。
 * 每个 case 由 KUnit 串行调度，但 console 仍可异步到达，因此统一经上述
 * observed 协议断言。
 */

/* Prevent replacing branch with select in LLVM. */
/*
 * 原注释译注：禁止 LLVM 将分支折叠成 select；保留真实调用边界，才能让
 * 插桩后的未初始化值在被使用时产生可归因于 case 的报告。
 * 业务背景：USE() 通过这两个 noinline 端点把值送入可观察的 printk 路径。
 * 入参：arg 是只读借用的字符串化表达式；出参/返回：无，输出一条日志。
 * 注意事项：测试依赖调用未被内联或消除，不能把它改成纯表达式包装。
 */
static noinline void check_true(char *arg)
{
	pr_info("%s is true\n", arg);
}

/*
 * 业务背景：与 check_true() 共同构成 USE() 的假分支消费端，确保无论值为真
 * 还是假都真实读取插桩值。
 * 入参：arg 是当前表达式名称的借用字符串；出参/返回：无，打印 false 分支。
 * 注意事项：noinline 是测试 ABI 的一部分；它避免编译器绕开 KMSAN 检查点。
 */
static noinline void check_false(char *arg)
{
	pr_info("%s is false\n", arg);
}

/*
 * USE(x) 把 x 放入不可内联的控制流并打印其文本；#x 是预处理器字符串化，
 * 因而日志不会再次求值。它只应接收本 case 有意消费的标量，不能用于带副作用
 * 的表达式，否则 if 条件求值本身已是测试的未初始化使用点。
 */
#define USE(x)                           \
	do {                             \
		if (x)                   \
			check_true(#x);  \
		else                     \
			check_false(#x); \
	} while (0)

/* 这些宏在 case 栈上建立 report_matches() 只借用的预期，不持久化指针。 */
#define EXPECTATION_ETYPE_FN(e, reason, fn) \
	struct expect_report e = {          \
		.error_type = reason,       \
		.symbol = fn,               \
	}

/* EXPECTATION_ETYPE_FN 的结构体字段与 report_matches() 的 error_type/symbol 一一对应。 */
#define EXPECTATION_NO_REPORT(e) EXPECTATION_ETYPE_FN(e, NULL, NULL)
#define EXPECTATION_UNINIT_VALUE_FN(e, fn) \
	EXPECTATION_ETYPE_FN(e, "uninit-value", fn)
#define EXPECTATION_UNINIT_VALUE(e) EXPECTATION_UNINIT_VALUE_FN(e, __func__)
#define EXPECTATION_USE_AFTER_FREE(e) \
	EXPECTATION_ETYPE_FN(e, "use-after-free", __func__)

/* 预期宏结束：无报告用两个 NULL，UMR/UAF 则绑定错误类型和当前/指定符号。 */

/* Test case: ensure that kmalloc() returns uninitialized memory. */
/*
 * 原注释译注：验证 kmalloc() 返回未初始化内存。
 * 业务背景：普通 slab 分配不承诺清零；这是 KMSAN 分配 hook 产生 poison 的
 * 基线 case。入参：test 为 KUnit 借用上下文；出参/返回：无，期望 UMR 报告。
 * 注意事项：ptr 的唯一所有权本应由 kfree() 释放；此处故意只读并让 case 结束，
 * 测试环境关注报告语义而非作为通用资源管理样例。
 */
static void test_uninit_kmalloc(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE(expect);
	int *ptr;

	/* 阶段 1：取得未清零对象，再经 USE() 建立真正的被检查读取。 */
	kunit_info(test, "uninitialized kmalloc test (UMR report)\n");
	ptr = kmalloc_obj(*ptr);
	USE(*ptr);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: ensure that kmalloc'ed memory becomes initialized after memset().
 */
/*
 * 原注释译注：验证 kmalloc 内存经 memset() 后成为已初始化。
 * 业务背景：与上一 case 成对证明 shadow 会随写入清除。入参：test 为借用断言
 * 上下文；出参/返回：无，期望没有报告。注意事项：memset 必须先于 USE()，
 * 改变顺序将把正常路径变成 UMR 测试；ptr 所有权仍由当前 case 持有。
 */
static void test_init_kmalloc(struct kunit *test)
{
	EXPECTATION_NO_REPORT(expect);
	int *ptr;

	kunit_info(test, "initialized kmalloc test (no reports)\n");
	/* 阶段 1：分配后立即全对象写零，提交已初始化 shadow 状态。 */
	ptr = kmalloc_obj(*ptr);
	memset(ptr, 0, sizeof(*ptr));
	USE(*ptr);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Test case: ensure that kzalloc() returns initialized memory. */
/*
 * 原注释译注：验证 kzalloc() 直接返回已初始化内存。
 * 业务背景：分配器的 __GFP_ZERO 路径应在对象交给调用者前同时清数据和 shadow。
 * 入参：test 是借用 KUnit 上下文；出参/返回：无，期望无报告。
 * 注意事项：它验证分配边界而不是随后 memset；ptr 的释放责任仍属 case。
 */
static void test_init_kzalloc(struct kunit *test)
{
	EXPECTATION_NO_REPORT(expect);
	int *ptr;

	kunit_info(test, "initialized kzalloc test (no reports)\n");
	ptr = kzalloc_obj(*ptr);
	USE(*ptr);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Test case: ensure that local variables are uninitialized by default. */
/*
 * 原注释译注：验证默认局部变量未初始化。
 * 业务背景：编译器插桩应把未赋值的自动变量标为 poison；该 case 排除分配器影响。
 * 入参：test 为借用上下文；出参/返回：无，期望 UMR 报告。
 * 注意事项：volatile 阻止优化器消除 cond 的读取；不能用初始化器替代它。
 */
static void test_uninit_stack_var(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE(expect);
	volatile int cond;

	kunit_info(test, "uninitialized stack variable (UMR report)\n");
	USE(cond);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Test case: ensure that local variables with initializers are initialized. */
/*
 * 原注释译注：验证带初始化器的局部变量已初始化。
 * 业务背景：与上一 case 对照，前端写入应在 USE() 前清除 shadow。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，期望无报告。
 * 注意事项：cond=1 是测试输入，不是并发共享状态；volatile 仅固定读取边界。
 */
static void test_init_stack_var(struct kunit *test)
{
	EXPECTATION_NO_REPORT(expect);
	volatile int cond = 1;

	kunit_info(test, "initialized stack variable (no reports)\n");
	USE(cond);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * 业务背景：参数传播链的末端消费者，迫使 KMSAN 检查两个独立参数槽。
 * 入参：arg1、arg2 是按值复制的标量，无 ownership；出参/返回：无。
 * 注意事项：noinline 保留跨函数 shadow/origin 传递边界，调用者用它区分参数 ABI。
 */
static noinline void two_param_fn_2(int arg1, int arg2)
{
	USE(arg1);
	USE(arg2);
}

/*
 * 业务背景：在两参链中加入一参转发层，验证同一值可复制到多个被调参数。
 * 入参：arg 为按值输入；出参/返回：无；它先转交 two_param_fn_2() 再本地消费。
 * 注意事项：不允许内联，否则测试会失去函数调用参数元数据的边界。
 */
static noinline void one_param_fn(int arg)
{
	two_param_fn_2(arg, arg);
	USE(arg);
}

/*
 * 业务背景：参数 case 的第一层接收者；已初始化 init 走无误报链，arg1/arg2
 * 则分别测试未初始化值穿过实参与读取点的归因。
 * 入参：两个按值标量，无引用或所有权；出参/返回：无。
 * 注意事项：noinline 保持 CONFIG_KMSAN_CHECK_PARAM_RETVAL 的报告位置可区分。
 */
static noinline void two_param_fn(int arg1, int arg2)
{
	int init = 0;

	one_param_fn(init);
	USE(arg1);
	USE(arg2);
}

/*
 * 业务背景：验证函数参数 shadow/origin 在调用 ABI 上的传递；它位于前端插桩与
 * KMSAN hook 的交界。入参：test 为借用 KUnit 上下文；出参/返回：无，期望 UMR。
 * 注意事项：启用 eager 参数/返回检查时报告发生在调用前，否则发生在
 * two_param_fn()；两个配置的预期符号不同，不能合并为一个固定字符串。
 */
static void test_params(struct kunit *test)
{
#ifdef CONFIG_KMSAN_CHECK_PARAM_RETVAL
	/*
	 * With eager param/retval checking enabled, KMSAN will report an error
	 * before the call to two_param_fn().
	 */
	/* 原注释译注：急切参数/返回检查会在调用 two_param_fn() 前报告错误。 */
	EXPECTATION_UNINIT_VALUE_FN(expect, "test_params");
#else
	EXPECTATION_UNINIT_VALUE_FN(expect, "two_param_fn");
#endif
	volatile int uninit, init = 1;

	kunit_info(test,
		   "uninit passed through a function parameter (UMR report)\n");
	two_param_fn(uninit, init);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * 业务背景：三参按值计算是多参数 case 的最小真实使用点。
 * 入参：a、b、c 为无所有权的有符号整数；出参/返回：返回它们的和。
 * 注意事项：不处理溢出；测试只关心任一未初始化实参抵达算术读取时的报告。
 */
static int signed_sum3(int a, int b, int c)
{
	return a + b + c;
}

/*
 * Test case: ensure that uninitialized values are tracked through function
 * arguments.
 */
/*
 * 原注释译注：验证未初始化值可经函数参数被跟踪。
 * 业务背景：混合 char/int 实参检查 ABI 扩展与每个参数的 shadow 传播。
 * 入参：test 为借用上下文；出参/返回：无，期望 UMR；a/c 未初始化，b 是对照值。
 * 注意事项：USE() 消费返回值，避免优化器删除 signed_sum3() 及检查路径。
 */
static void test_uninit_multiple_params(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE(expect);
	volatile char b = 3, c;
	volatile int a;

	kunit_info(test, "uninitialized local passed to fn (UMR report)\n");
	USE(signed_sum3(a, b, c));
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Helper function to make an array uninitialized. */
/*
 * 原注释译注：把数组指定区间变为未初始化的辅助函数。
 * 业务背景：显式向 array[start, stop) 写入未初始化栈字节，以制造部分 poison
 * 而不依赖布局偶然性。入参：array 是调用者持有的可写借用缓冲区；start/stop
 * 是元素下标且要求 0 <= start <= stop。出参/返回：无，区间 shadow 变为 poison。
 * 注意事项：noinline 保留写入传播边界；越界会破坏测试对象，调用者负责范围。
 */
static noinline void do_uninit_local_array(char *array, int start, int stop)
{
	volatile char uninit;

	/* 阶段：逐字节复制同一未初始化源，使范围外字节保持原有初始化状态。 */
	for (int i = start; i < stop; i++)
		array[i] = uninit;
}

/*
 * Test case: ensure kmsan_check_memory() reports an error when checking
 * uninitialized memory.
 */
/*
 * 原注释译注：验证检查未初始化内存时 kmsan_check_memory() 会报告错误。
 * 业务背景：这是显式检查 API 的基本契约，区别于编译器自动插桩的读取检查。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期报告归因本函数。
 * 注意事项：只毒化 [5,7)，但检查整个 8 字节数组，验证扫描不会遗漏内部片段。
 */
static void test_uninit_kmsan_check_memory(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE_FN(expect, "test_uninit_kmsan_check_memory");
	volatile char local_array[8];

	kunit_info(
		test,
		"kmsan_check_memory() called on uninit local (UMR report)\n");
	/* 阶段：先构造部分坏字节，再由公开 API 扫描整个借用范围。 */
	do_uninit_local_array((char *)local_array, 5, 7);

	kmsan_check_memory((char *)local_array, 8);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: check that a virtual memory range created with vmap() from
 * initialized pages is still considered as initialized.
 */
/*
 * 原注释译注：验证由已初始化页面 vmap() 成的虚拟区仍被视为已初始化。
 * 业务背景：vmap 建立新虚拟别名不得丢失或伪造页的 KMSAN metadata。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，期望无报告。
 * 注意事项：pages 数组、每页和 vbuf 均由本 case 持有；退出按 vunmap、free page、
 * kfree 的逆序归还。分配/vmap 失败在本 KUnit 前提下未处理，不能复制到生产路径。
 */
static void test_init_kmsan_vmap_vunmap(struct kunit *test)
{
	EXPECTATION_NO_REPORT(expect);
	const int npages = 2;
	struct page **pages;
	void *vbuf;

	kunit_info(test, "pages initialized via vmap (no reports)\n");

	/* 阶段 1：建立两个物理页并映射为连续虚拟别名，然后写入初始化数据。 */
	pages = kmalloc_objs(*pages, npages);
	for (int i = 0; i < npages; i++)
		pages[i] = alloc_page(GFP_KERNEL);
	vbuf = vmap(pages, npages, VM_MAP, PAGE_KERNEL);
	memset(vbuf, 0xfe, npages * PAGE_SIZE);
	/* 阶段 2：从原页别名逐页检查，证明初始化状态跨 vmap 映射传播。 */
	for (int i = 0; i < npages; i++)
		kmsan_check_memory(page_address(pages[i]), PAGE_SIZE);

	/* 阶段 3：撤销虚拟映射，再释放每个物理页和指针数组。 */
	if (vbuf)
		vunmap(vbuf);
	for (int i = 0; i < npages; i++) {
		if (pages[i])
			__free_page(pages[i]);
	}
	kfree(pages);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: ensure that memset() can initialize a buffer allocated via
 * vmalloc().
 */
/*
 * 原注释译注：验证 memset() 能初始化经 vmalloc 分配的缓冲区。
 * 业务背景：vmalloc 非连续物理页的 metadata 更新须与线性访问语义一致。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，期望无报告。
 * 注意事项：buf 由本 case 持有并在检查后 vfree；npages 是页数，所有检查范围
 * 以 PAGE_SIZE 为单位。测试默认 vmalloc 成功，不可作为失败处理范式。
 */
static void test_init_vmalloc(struct kunit *test)
{
	EXPECTATION_NO_REPORT(expect);
	int npages = 8;
	char *buf;

	kunit_info(test, "vmalloc buffer can be initialized (no reports)\n");
	/* 阶段：分配、写入每页、逐页显式检查，最后解除该虚拟区所有权。 */
	buf = vmalloc(PAGE_SIZE * npages);
	buf[0] = 1;
	memset(buf, 0xfe, PAGE_SIZE * npages);
	USE(buf[0]);
	for (int i = 0; i < npages; i++)
		kmsan_check_memory(&buf[PAGE_SIZE * i], PAGE_SIZE);
	vfree(buf);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Test case: ensure that use-after-free reporting works for kmalloc. */
/*
 * 原注释译注：验证 kmalloc 对象释放后的访问会报告 use-after-free。
 * 业务背景：释放 hook 必须使旧对象的 metadata 无效，即使用户数据曾被初始化。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期 UAF 报告。
 * 注意事项：var 在 kfree 后仅是故意保留的失效裸指针，任何真实代码不得解引用；
 * value 先复制可把报告稳定归因到本 case 的随后 USE()。
 */
static void test_uaf(struct kunit *test)
{
	EXPECTATION_USE_AFTER_FREE(expect);
	volatile int value;
	volatile int *var;

	kunit_info(test, "use-after-free in kmalloc-ed buffer (UMR report)\n");
	/* 阶段：先初始化对象，再释放所有权，最后故意通过失效地址读取。 */
	var = kmalloc(80, GFP_KERNEL);
	var[3] = 0xfeedface;
	kfree((int *)var);
	/* Copy the invalid value before checking it. */
	/* 原注释译注：先复制失效值再检查，使后续 USE() 消费稳定的局部副本。 */
	value = var[3];
	USE(value);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * 业务背景：页分配的默认内容和 slab 一样不保证初始化；该 case 覆盖 page
 * allocator 到 page_address() 的 KMSAN 传播。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期 UMR 报告。
 * 注意事项：page 是本 case 的唯一页引用，必须以 order 0 匹配 __free_pages()。
 */
static void test_uninit_page(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE(expect);
	struct page *page;
	int *ptr;

	kunit_info(test, "uninitialized page allocation (UMR report)\n");
	/* 阶段：分配未清零页、取得直接映射地址并消费首个 int，随后归还该页。 */
	page = alloc_pages(GFP_KERNEL, 0);
	ptr = page_address(page);
	USE(*ptr);
	__free_pages(page, 0);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * 业务背景：构造页面释放后的失效地址，供单页与高阶页两个 case 共用。
 * 入参：order 是 buddy 分配阶数；offset 是页块起始地址内的字节偏移，均由
 * 调用者保证有效。出参/返回：返回不持有引用的故意失效指针。
 * 注意事项：__GFP_ZERO 使释放前数据及 shadow 已初始化；释放后不得在生产
 * 代码使用返回值，本 helper 只服务 UAF 检测测试。
 */
static volatile char *test_uaf_pages_helper(int order, int offset)
{
	struct page *page;
	volatile char *var;

	/* Memory is initialized up until __free_pages() thanks to __GFP_ZERO. */
	/* 原注释译注：__GFP_ZERO 令 __free_pages() 前内存已初始化，故报告只应来自释放。 */
	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	var = page_address(page) + offset;
	__free_pages(page, order);

	return var;
}

/* Test case: ensure that use-after-free reporting works for a freed page. */
/*
 * 原注释译注：验证已释放单页的 UAF 报告。
 * 业务背景：验证 order-0 页释放 hook 的 poison 与地址失效语义。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期 UAF。
 * 注意事项：helper 返回的指针无所有权；offset 3 有意避开自然对齐特例。
 */
static void test_uaf_pages(struct kunit *test)
{
	EXPECTATION_USE_AFTER_FREE(expect);
	volatile char value;

	kunit_info(test, "use-after-free on a freed page (UMR report)\n");
	/* Allocate a single page, free it, then try to access it. */
	value = *test_uaf_pages_helper(0, 3);
	USE(value);

	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Test case: ensure that UAF reporting works for high order pages. */
/*
 * 原注释译注：验证高阶页的 UAF 报告。
 * 业务背景：高阶块尾页同样必须在整块释放后保持不可访问的 KMSAN 状态。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期 UAF。
 * 注意事项：PAGE_SIZE+3 落在 order-1 块的第二页；它检查 tail-page 路径而非
 * 仅检查 head page，返回失效地址只可由该 case 读取。
 */
static void test_uaf_high_order_pages(struct kunit *test)
{
	EXPECTATION_USE_AFTER_FREE(expect);
	volatile char value;

	kunit_info(test,
		   "use-after-free on a freed high-order page (UMR report)\n");
	/*
	 * Create a high-order non-compound page, free it, then try to access
	 * its tail page.
	 */
	value = *test_uaf_pages_helper(1, PAGE_SIZE + 3);
	USE(value);

	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: ensure that uninitialized values are propagated through per-CPU
 * memory.
 */
/*
 * 原注释译注：验证未初始化值会经 per-CPU 内存传播。
 * 业务背景：per-CPU 访问使用特殊地址计算，KMSAN 必须随同本 CPU 写/读传播 shadow。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期 UMR。
 * 注意事项：this_cpu_* 假定执行期间不迁移；case 未跨 CPU 保存地址，因此只测试
 * 本地槽语义，不验证远端并发访问。
 */
static void test_percpu_propagate(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE(expect);
	volatile int uninit, check;

	kunit_info(test,
		   "uninit local stored to per_cpu memory (UMR report)\n");

	/* 阶段：把 poison 写入本 CPU 槽、读回局部变量，再由 USE() 触发检查。 */
	this_cpu_write(per_cpu_var, uninit);
	check = this_cpu_read(per_cpu_var);
	USE(check);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: ensure that passing uninitialized values to printk() leads to an
 * error report.
 */
/*
 * 原注释译注：验证向 printk() 传递未初始化值会产生错误报告。
 * 业务背景：格式化日志是信息泄露边界，KMSAN 必须在参数使用前阻止 poison 外流。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期 UMR。
 * 注意事项：急切参数检查启用时错误归因本函数，否则归因 format 参数名 number；
 * 该配置差异由条件预期明确表达。
 */
static void test_printk(struct kunit *test)
{
#ifdef CONFIG_KMSAN_CHECK_PARAM_RETVAL
	/*
	 * With eager param/retval checking enabled, KMSAN will report an error
	 * before the call to pr_info().
	 */
	/* 原注释译注：启用急切参数/返回检查时，错误发生在调用 pr_info() 之前。 */
	EXPECTATION_UNINIT_VALUE_FN(expect, "test_printk");
#else
	EXPECTATION_UNINIT_VALUE_FN(expect, "number");
#endif
	volatile int uninit;

	kunit_info(test, "uninit local passed to pr_info() (UMR report)\n");
	pr_info("%px contains %d\n", &uninit, uninit);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Prevent the compiler from inlining a memcpy() call. */
/*
 * 原注释译注：阻止编译器内联 memcpy() 调用。
 * 业务背景：测试需要经过真实 memcpy 插桩/拦截边界，以观察 shadow 与 origin 复制。
 * 入参：dst 为可写借用目标，src 为只读借用源，size 是字节数；出参/返回：返回
 * memcpy 的 dst 指针，不取得任何内存所有权。
 * 注意事项：调用者保证范围有效且不重叠；volatile 仅防优化，真正 memcpy 前先去除
 * 限定以匹配 API 原型。noinline 不得移除，否则 case 的归因边界会变化。
 */
static noinline void *memcpy_noinline(volatile void *dst,
				      const volatile void *src, size_t size)
{
	return memcpy((void *)dst, (const void *)src, size);
}

/* Test case: ensure that memcpy() correctly copies initialized values. */
/*
 * 原注释译注：验证 memcpy 正确复制已初始化值。
 * 业务背景：数据副本正常时目标 shadow 必须为 clean，显式检查确认不误报。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，期望无报告。
 * 注意事项：src 先赋值才复制；src/dst 均为栈对象，无需释放，volatile 固定访问。
 */
static void test_init_memcpy(struct kunit *test)
{
	EXPECTATION_NO_REPORT(expect);
	volatile long long src;
	volatile long long dst = 0;

	/* 阶段：初始化源、跨 noinline memcpy 复制，再扫描完整目标对象。 */
	src = 1;
	kunit_info(
		test,
		"memcpy()ing aligned initialized src to aligned dst (no reports)\n");
	memcpy_noinline((void *)&dst, (void *)&src, sizeof(src));
	kmsan_check_memory((void *)&dst, sizeof(dst));
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: ensure that memcpy() correctly copies uninitialized values between
 * aligned `src` and `dst`.
 */
/*
 * 原注释译注：验证 memcpy 在对齐源/目标之间正确复制未初始化值。
 * 业务背景：同对齐槽复制仍必须保留 poison 与 origin，而非只复制用户数据。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期 UMR。
 * 注意事项：dst 预先为零；只有 memcpy 后它才应获得 uninit_src 的坏状态。
 */
static void test_memcpy_aligned_to_aligned(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE_FN(expect, "test_memcpy_aligned_to_aligned");
	volatile int uninit_src;
	volatile int dst = 0;

	/* 阶段：将单个对齐 poison 槽复制到已清零的对齐目标，再扫描目标。 */
	kunit_info(
		test,
		"memcpy()ing aligned uninit src to aligned dst (UMR report)\n");
	memcpy_noinline((void *)&dst, (void *)&uninit_src, sizeof(uninit_src));
	kmsan_check_memory((void *)&dst, sizeof(dst));
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: ensure that memcpy() correctly copies uninitialized values between
 * aligned `src` and unaligned `dst`.
 *
 * Copying aligned 4-byte value to an unaligned one leads to touching two
 * aligned 4-byte values. This test case checks that KMSAN correctly reports an
 * error on the mentioned two values.
 */
/*
 * 原注释译注：验证 memcpy 在对齐源与未对齐目标之间正确复制未初始化值；4 字节
 * 源写入未对齐地址会触及两个对齐 4 字节槽，二者均须报告。
 * 业务背景：该 case 覆盖 shadow/origin 跨槽拆分，不允许只更新第一个目标槽。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期两次 UMR。
 * 注意事项：每次断言后 report_reset() 重新开启 console 捕获，避免首份报告遮住
 * 第二个槽；dst 初始为零用于隔离 memcpy 的影响。
 */
static void test_memcpy_aligned_to_unaligned(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE_FN(expect, "test_memcpy_aligned_to_unaligned");
	volatile int uninit_src;
	volatile char dst[8] = { 0 };

	kunit_info(
		test,
		"memcpy()ing aligned uninit src to unaligned dst (UMR report)\n");
	/* 阶段 1：先确认源自身 poison，再把它复制到从 dst[1] 开始的未对齐位置。 */
	kmsan_check_memory((void *)&uninit_src, sizeof(uninit_src));
	memcpy_noinline((void *)&dst[1], (void *)&uninit_src,
			sizeof(uninit_src));
	/* 阶段 2：分别检查两个受影响的对齐槽；每份报告都独立消费。 */
	kmsan_check_memory((void *)dst, 4);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
	report_reset();
	kmsan_check_memory((void *)&dst[4], sizeof(uninit_src));
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: ensure that origin slots do not accidentally get overwritten with
 * zeroes during memcpy().
 *
 * Previously, when copying memory from an aligned buffer to an unaligned one,
 * if there were zero origins corresponding to zero shadow values in the source
 * buffer, they could have ended up being copied to nonzero shadow values in the
 * destination buffer:
 *
 *  memcpy(0xffff888080a00000, 0xffff888080900002, 8)
 *
 *  src (0xffff888080900002): ..xx .... xx..
 *  src origins:              o111 0000 o222
 *  dst (0xffff888080a00000): xx.. ..xx
 *  dst origins:              o111 0000
 *                        (or 0000 o222)
 *
 * (here . stands for an initialized byte, and x for an uninitialized one.
 *
 * Ensure that this does not happen anymore, and for both destination bytes
 * the origin is nonzero (i.e. KMSAN reports an error).
 */
/*
 * 原注释译注：验证 memcpy 不会把零 shadow 对应的零 origin 错写到目标的非零
 * shadow 槽；两段目标坏字节都必须保留非零 origin 并触发报告。
 * 业务背景：origin 是诊断归因链，错误的零覆盖会让 poison 无来源或漏报。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，依次预期真、假、真三种断言。
 * 注意事项：图中的点表示已初始化、x 表示未初始化、o 表示 origin；原图保留为
 * 历史测试证据，当前判断以三个 kmsan_check_memory() 的实际范围为准。
 */
static void test_memcpy_initialized_gap(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE_FN(expect, "test_memcpy_initialized_gap");
	volatile char uninit_src[12];
	volatile char dst[8] = { 0 };

	kunit_info(
		test,
		"unaligned 4-byte initialized value gets a nonzero origin after memcpy() - (2 UMR reports)\n");

	/* 阶段 1：只初始化交错字节，留下两段 poison 与中间 clean gap。 */
	uninit_src[0] = 42;
	uninit_src[1] = 42;
	uninit_src[4] = 42;
	uninit_src[5] = 42;
	uninit_src[6] = 42;
	uninit_src[7] = 42;
	uninit_src[10] = 42;
	uninit_src[11] = 42;
	memcpy_noinline((void *)&dst[0], (void *)&uninit_src[2], 8);

	/* 阶段 2：检查左坏段、clean 中段和右坏段；每次报告后重置观察状态。 */
	kmsan_check_memory((void *)&dst[0], 4);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
	report_reset();
	kmsan_check_memory((void *)&dst[2], 4);
	KUNIT_EXPECT_FALSE(test, report_matches(&expect));
	report_reset();
	kmsan_check_memory((void *)&dst[4], 4);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/* Generate test cases for memset16(), memset32(), memset64(). */
/*
 * 原注释译注：生成 memset16()/memset32()/memset64() 的测试用例。
 * 业务背景：各宽度的专用填充函数也须清除其写入元素的 KMSAN shadow；宏按 size
 * 生成三个同构 KUnit 函数。入参：生成函数的 test 为借用上下文；出参/返回：无，
 * 期望无报告。注意事项：续行内的阶段注释必须保留反斜杠以维持一个宏替换列表；
 * size 只取 16、32、64。
 */
#define DEFINE_TEST_MEMSETXX(size)                                          \
	static void test_memset##size(struct kunit *test)                   \
	{                                                                   \
		/* 生成 case 的阶段：建立无报告预期，填充一个元素并显式验证其 shadow。 */ \
		EXPECTATION_NO_REPORT(expect);                              \
		volatile uint##size##_t uninit;                             \
                                                                            \
		kunit_info(test,                                            \
			   "memset" #size "() should initialize memory\n"); \
		/* 填充调用是发布点：返回后 uninit 的完整 size 位对象应不再带 poison。 */ \
		memset##size((uint##size##_t *)&uninit, 0, 1);              \
		kmsan_check_memory((void *)&uninit, sizeof(uninit));        \
		KUNIT_EXPECT_TRUE(test, report_matches(&expect));           \
	}

DEFINE_TEST_MEMSETXX(16)
DEFINE_TEST_MEMSETXX(32)
DEFINE_TEST_MEMSETXX(64)

/* 上述宏实例均为无报告 case：各元素宽度的填充先写数据，再由显式检查读 shadow。 */

/* Test case: ensure that KMSAN does not access shadow memory out of bounds. */
/*
 * 原注释译注：验证 KMSAN 不会越界访问 shadow 内存。
 * 业务背景：边界 memset 会让插桩计算相邻 shadow 映射，必须只触及有效字节。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无；成功标准是不崩溃。
 * 注意事项：buf 由 case 持有并由 vfree 释放；size 从 0 到 128 覆盖两端，测试
 * 假定 vmalloc 成功，不能据此省略生产代码的 NULL 检查。
 */
static void test_memset_on_guarded_buffer(struct kunit *test)
{
	void *buf = vmalloc(PAGE_SIZE);

	kunit_info(test,
		   "memset() on ends of guarded buffer should not crash\n");

	/* 阶段：同一长度分别写区首和区尾，覆盖左/右边界的元数据地址计算。 */
	for (size_t size = 0; size <= 128; size++) {
		memset(buf, 0xff, size);
		memset(buf + PAGE_SIZE - size, 0xff, size);
	}
	vfree(buf);
}

/*
 * 业务背景：递归把前一项的 poison 传播到后续项，构造超过 origin 深度上限的链。
 * 入参：array 是调用者持有的可写 int 数组；size 是元素数；start 是当前下标。
 * 出参/返回：无，写入 array[start..size)；没有取得所有权。
 * 注意事项：start<2 或 start==size 直接停止；调用者必须保证 array 有 size 项，
 * noinline 使每层递归成为可观察的 origin 栈帧。
 */
static noinline void fibonacci(int *array, int size, int start)
{
	if (start < 2 || (start == size))
		return;
	array[start] = array[start - 1] + array[start - 2];
	fibonacci(array, size, start + 1);
}

/*
 * 业务背景：KMSAN 的 origin 链有最大深度，截断长链仍须保留有效诊断且不崩溃。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，末项应触发 UMR。
 * 注意事项：accum[1] 故意保持未初始化，accum[0] 是递推种子；数组位于栈上，
 * 不涉及释放。递归深度是上限两倍，确保走到截断/折叠路径。
 */
static void test_long_origin_chain(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE_FN(expect, "test_long_origin_chain");
	/* (KMSAN_MAX_ORIGIN_DEPTH * 2) recursive calls to fibonacci(). */
	/* 原注释译注：调用 fibonacci() 的递归次数为最大 origin 深度的两倍。 */
	volatile int accum[KMSAN_MAX_ORIGIN_DEPTH * 2 + 2];
	int last = ARRAY_SIZE(accum) - 1;

	kunit_info(
		test,
		"origin chain exceeding KMSAN_MAX_ORIGIN_DEPTH (UMR report)\n");
	/*
	 * We do not set accum[1] to 0, so the uninitializedness will be carried
	 * over to accum[2..last].
	 */
	/* 原注释译注：不把 accum[1] 置零，因此未初始化状态会传到 accum[2..last]。 */
	accum[0] = 1;
	fibonacci((int *)accum, ARRAY_SIZE(accum), 2);
	kmsan_check_memory((void *)&accum[last], sizeof(int));
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: ensure that saving/restoring/printing stacks to/from stackdepot
 * does not trigger errors.
 *
 * KMSAN uses stackdepot to store origin stack traces, that's why we do not
 * instrument lib/stackdepot.c. Yet it must properly mark its outputs as
 * initialized because other kernel features (e.g. netdev tracker) may also
 * access stackdepot from instrumented code.
 */
/*
 * 原注释译注：验证 stackdepot 的保存、恢复、打印不触发错误；KMSAN 用它保存
 * origin 栈而不插桩其实现，但它必须把输出标为初始化，供其它插桩特性读取。
 * 业务背景：跨未插桩库边界时，输出 metadata 的正确初始化避免伪 UMR。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，期望无报告。
 * 注意事项：dst_entries 是 stack_depot_fetch() 借出的内部数组，不由 case 释放；
 * handle 代表 depot 所有状态，case 仅使用它进行这次往返验证。
 */
static void test_stackdepot_roundtrip(struct kunit *test)
{
	unsigned long src_entries[16], *dst_entries;
	unsigned int src_nentries, dst_nentries;
	EXPECTATION_NO_REPORT(expect);
	depot_stack_handle_t handle;

	kunit_info(test, "testing stackdepot roundtrip (no reports)\n");

	/* 阶段：采样当前栈、保存为 depot 条目、打印并取回借用数组，再检查输出。 */
	src_nentries =
		stack_trace_save(src_entries, ARRAY_SIZE(src_entries), 1);
	handle = stack_depot_save(src_entries, src_nentries, GFP_KERNEL);
	stack_depot_print(handle);
	dst_nentries = stack_depot_fetch(handle, &dst_entries);
	KUNIT_EXPECT_TRUE(test, src_nentries == dst_nentries);

	kmsan_check_memory((void *)dst_entries,
			   sizeof(*dst_entries) * dst_nentries);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * Test case: ensure that kmsan_unpoison_memory() and the instrumentation work
 * the same.
 */
/*
 * 原注释译注：验证 kmsan_unpoison_memory() 与普通插桩写入具有相同效果。
 * 业务背景：显式反毒 API 供非插桩写入者标记已初始化，必须与 C 写入同步一致。
 * 入参：test 为借用 KUnit 上下文；出参/返回：无，预期两次检查都只报告剩余 3 字节。
 * 注意事项：a[0] 通过插桩赋值，b[0] 通过 API；report_reset() 分隔两条报告。
 */
static void test_unpoison_memory(struct kunit *test)
{
	EXPECTATION_UNINIT_VALUE_FN(expect, "test_unpoison_memory");
	volatile char a[4], b[4];

	kunit_info(
		test,
		"unpoisoning via the instrumentation vs. kmsan_unpoison_memory() (2 UMR reports)\n");

	/* Initialize a[0] and check a[1]--a[3]. */
	/* 原注释译注：初始化 a[0] 后只检查 a[1]--a[3]，它们仍应保留 poison。 */
	a[0] = 0;
	kmsan_check_memory((char *)&a[1], 3);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));

	report_reset();

	/* Initialize b[0] and check b[1]--b[3]. */
	/* 原注释译注：反毒 b[0] 后只检查 b[1]--b[3]，它们同样仍应 poison。 */
	kmsan_unpoison_memory((char *)&b[0], 1);
	kmsan_check_memory((char *)&b[1], 3);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * 业务背景：nofault 复制可能绕过普通插桩，KMSAN hook 仍须把源的 poison 传播到
 * 目标或在安全边界报告，防止未初始化内核数据被静默消费。
 * 入参：test 为借用 KUnit 上下文；buf 是输出栈数组，src 是故意未初始化源，size
 * 为 4 字节。出参/返回：无；ret 是复制状态并被 USE() 消费，预期 UMR 归因 helper。
 * 注意事项：真实调用者必须处理 ret 的 -EFAULT/部分复制；本 case 只使用有效栈地址，
 * 所以失败不是预期路径。
 */
static void test_copy_from_kernel_nofault(struct kunit *test)
{
	long ret;
	char buf[4], src[4];
	size_t size = sizeof(buf);

	/* 阶段：通过 nofault helper 复制故意 poison 的源，再消费状态和报告。 */
	EXPECTATION_UNINIT_VALUE_FN(expect, "copy_from_kernel_nofault");
	kunit_info(
		test,
		"testing copy_from_kernel_nofault with uninitialized memory\n");

	ret = copy_from_kernel_nofault((char *)&buf[0], (char *)&src[0], size);
	USE(ret);
	KUNIT_EXPECT_TRUE(test, report_matches(&expect));
}

/*
 * KUnit case 表按测试主题注册所有函数；KUNIT_CASE() 保存函数指针与名称，suite
 * 取得表的借用引用。末尾空项是框架终止哨兵，不是可执行 case；遗漏某个 case
 * 会使其契约完全不被运行，重排仅改变执行顺序而不改变各 case 的所有权。
 */
static struct kunit_case kmsan_test_cases[] = {
	/* 分配与栈初始化语义。 */
	KUNIT_CASE(test_uninit_kmalloc),
	KUNIT_CASE(test_init_kmalloc),
	KUNIT_CASE(test_init_kzalloc),
	KUNIT_CASE(test_uninit_stack_var),
	KUNIT_CASE(test_init_stack_var),
	/* 参数 ABI、显式检查与虚拟映射。 */
	KUNIT_CASE(test_params),
	KUNIT_CASE(test_uninit_multiple_params),
	KUNIT_CASE(test_uninit_kmsan_check_memory),
	KUNIT_CASE(test_init_kmsan_vmap_vunmap),
	KUNIT_CASE(test_init_vmalloc),
	/* 物理页、UAF、per-CPU 和日志泄露边界。 */
	KUNIT_CASE(test_uninit_page),
	KUNIT_CASE(test_uaf),
	KUNIT_CASE(test_uaf_pages),
	KUNIT_CASE(test_uaf_high_order_pages),
	KUNIT_CASE(test_percpu_propagate),
	KUNIT_CASE(test_printk),
	/* memcpy 对齐、origin 和专用 memset 的 metadata 传播。 */
	KUNIT_CASE(test_init_memcpy),
	KUNIT_CASE(test_memcpy_aligned_to_aligned),
	KUNIT_CASE(test_memcpy_aligned_to_unaligned),
	KUNIT_CASE(test_memcpy_initialized_gap),
	KUNIT_CASE(test_memset16),
	KUNIT_CASE(test_memset32),
	KUNIT_CASE(test_memset64),
	/* 边界、长 origin、stackdepot、反毒与 nofault hook。 */
	KUNIT_CASE(test_memset_on_guarded_buffer),
	KUNIT_CASE(test_long_origin_chain),
	KUNIT_CASE(test_stackdepot_roundtrip),
	KUNIT_CASE(test_unpoison_memory),
	KUNIT_CASE(test_copy_from_kernel_nofault),
	{},
};

/* ===== End test cases ===== */
/*
 * 原注释译注：测试用例结束。接下来是每 case 初始化、套件初始化/退出及模块注册；
 * 它们负责报告探针和 panic 策略的生命周期，而不属于单个内存语义 case。
 */

/*
 * 业务背景：每个 case 开始前重置上一 case 的日志证据，避免异步 console 残留
 * 误满足新断言。入参：test 是 KUnit 提供但本函数不使用的借用上下文。
 * 出参/返回：0 表示初始化成功；清空 header、ignore、available，无所有权转移。
 * 注意事项：与 probe/report_reset() 使用同一 irqsave 锁，不能睡眠；若漏清空，
 * 首个报告会被旧 case 继承，造成假阳性。
 */
static int test_init(struct kunit *test)
{
	unsigned long flags;

	spin_lock_irqsave(&observed.lock, flags);
	observed.header[0] = '\0';
	observed.ignore = false;
	observed.available = false;
	spin_unlock_irqrestore(&observed.lock, flags);

	return 0;
}

/*
 * 业务背景：KUnit 要求成对 exit 回调；本文件全部 case 资源在自身路径归还，
 * 因此没有 per-case teardown 工作。
 * 入参：test 为未使用的借用上下文；出参/返回：无，无副作用。
 * 注意事项：suite 范围资源不在此释放，必须留给 kmsan_suite_exit()，否则后续
 * case 会在失去 console probe 的情况下运行。
 */
static void test_exit(struct kunit *test)
{
}

/* suite 初始化临时保存的全局 panic 策略；只在 suite 生命周期内由 init/exit 写读。 */
static int orig_panic_on_kmsan;

/*
 * 业务背景：套件开始时注册 console probe，并临时关闭 panic_on_kmsan，令故意
 * UMR/UAF 报告可被断言而不终止整套测试。
 * 入参：suite 是 KUnit 持有的借用描述符，本函数不修改其字段；出参/返回：0。
 * 注意事项：probe 注册先于 case 执行；全局 panic 值必须在 exit 原样恢复，测试
 * 运行期间不能假定其它使用者可安全改写它。
 */
static int kmsan_suite_init(struct kunit_suite *suite)
{
	register_trace_console(probe_console, NULL);
	orig_panic_on_kmsan = panic_on_kmsan;
	panic_on_kmsan = 0;
	return 0;
}

/*
 * 业务背景：撤销 suite_init() 发布的观察设施和全局策略，防止卸载后 probe 使用
 * 本文件静态状态。入参：suite 为未使用借用描述符；出参/返回：无。
 * 注意事项：先 unregister 阻止新回调，再 tracepoint_synchronize_unregister()
 * 等待旧回调结束，最后恢复 panic 设置；颠倒顺序会导致 UAF 或泄漏测试策略。
 */
static void kmsan_suite_exit(struct kunit_suite *suite)
{
	unregister_trace_console(probe_console, NULL);
	tracepoint_synchronize_unregister();
	panic_on_kmsan = orig_panic_on_kmsan;
}

/*
 * KUnit suite 描述符把 case 表和生命周期回调发布给 kunit_test_suites()；字段均
 * 指向本文件静态对象，模块卸载前由框架按 suite_exit → 同步注销的顺序撤销。
 */
static struct kunit_suite kmsan_test_suite = {
	.name = "kmsan",
	.test_cases = kmsan_test_cases,
	.init = test_init,
	.exit = test_exit,
	.suite_init = kmsan_suite_init,
	.suite_exit = kmsan_suite_exit,
};
/* 此宏把静态 suite 注册为 KUnit 模块测试入口，不转移 suite 所有权。 */
kunit_test_suites(&kmsan_test_suite);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Alexander Potapenko <glider@google.com>");
MODULE_DESCRIPTION("Test cases for KMSAN");
