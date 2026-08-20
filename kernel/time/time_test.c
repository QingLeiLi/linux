// SPDX-License-Identifier: LGPL-2.1+

#include <kunit/test.h>
#include <linux/time.h>

/*
 * Traditional implementation of leap year evaluation, but note that long
 * is a signed type and the tests do cover negative year values. So this
 * can't use the is_leap_year() helper from rtc.h.
 */
/*
 * 这里采用传统 Gregorian 闰年规则，但必须注意 year 是有符号 long，测试范围确实跨越公元前年份；
 * rtc.h 的 is_leap_year() 接收无符号年份，负值转换后会改变取模语义，所以不能拿它构造参考答案。
 *
 * is_leap() - 为测试的独立参考日历判断一个有符号年份是否为闰年。
 *
 * 【调用位置】last_day_of_month() 在处理二月时调用；结果随后决定 advance_date() 是否允许 2 月
 * 29 日。它刻意不调用被测的 time64_to_tm()，避免“实现与参考答案共享同一个错误”。
 *
 * 【参数】@year 是纯输入的 Gregorian 年份，可为负、零或正；函数不保存该值，也不涉及指针和
 * ownership。调用者无需持锁；这里只做整数取模，不访问共享状态、不会睡眠。
 *
 * 【返回】能被 4 整除且不能被 100 整除，或能被 400 整除时返回 true，否则返回 false；无失败
 * 返回和外部副作用。负年份仍按 C 的“余数为零”判整除，因此该条件在测试的正负年份上对称。
 */
static bool is_leap(long year)
{
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/*
 * Gets the last day of a month.
 */
/*
 * 返回指定月份的最后一个日号；该值也是 advance_date() 判断是否跨月的边界。
 *
 * last_day_of_month() - 用独立的 Gregorian 规则求某年某月有多少天。
 *
 * 【参数】@year 是纯输入的有符号 Gregorian 年份，仅在 month=2 时参与闰年判断；@month 是纯输入
 * 的一基月份，调用协议要求范围为 1..12。本文件内部始终满足该范围，函数不为越界值提供校验。
 * 两个参数都按值传递，无 ownership 变化。
 *
 * 【上下文与返回】无锁、不可睡眠且无副作用。二月返回 28 或 29；四、六、九、十一月返回 30；
 * 其余合法月份返回 31。函数没有错误码；若调用者违反月份范围约定，也会落入 31 天分支。
 *
 * 【阶段】先处理唯一依赖年份的二月，再合并四个 30 天月份，最后以 31 天作为合法月份快速出口。
 */
static int last_day_of_month(long year, int month)
{
	/* 二月的上界由 is_leap() 给出；bool 在算术中转为 0/1，得到 28 或 29。 */
	if (month == 2)
		return 28 + is_leap(year);
	/* 这四个月共享 30 天规则；其他合法月份都无需再查表。 */
	if (month == 4 || month == 6 || month == 9 || month == 11)
		return 30;
	return 31;
}

/*
 * Advances a date by one day.
 */
/*
 * 把一个日期推进一天；四个输出量共同组成测试侧的期望日期状态，必须在每轮断言全部完成后同步更新。
 *
 * advance_date() - 原地推进独立参考日历的年、月、日和年内日。
 *
 * 【参数与 ownership】@year、@month、@mday、@yday 都是调用者持有的非 NULL 输入输出指针，分别
 * 指向有符号 Gregorian 年、一基月、一基日和零基年内日。函数只借用指针并改写所指整数，不保存
 * 地址、不取得资源；入口要求四个值描述同一天且日期有效。
 *
 * 【上下文】仅调用纯函数 last_day_of_month() 并写调用者私有局部量，不持锁、不可睡眠，也不存在
 * 并发发布。返回无直接值；成功保证四个字段仍描述紧随输入日期的同一天，无失败出口。
 *
 * 【阶段】1. 普通日只增加日号和年内日；2. 月末把日号重置为 1，并在非十二月推进月份与年内日；
 * 3. 年末同时重置月、日和年内日，再增加年份。三个出口互斥，避免把跨月和跨年增量叠加两次。
 */
static void advance_date(long *year, int *month, int *mday, int *yday)
{
	/* 快速路径：尚未到月末，月份和年份保持不变，两个日序各增加一。 */
	if (*mday != last_day_of_month(*year, *month)) {
		++*mday;
		++*yday;
		return;
	}

	/* 月末路径先建立“下月 1 日”；若不是十二月，只需推进月份和年内日。 */
	*mday = 1;
	if (*month != 12) {
		++*month;
		++*yday;
		return;
	}

	/* 十二月末跨年：新日期是一月一日，零基 yday 回到 0，年份最后加一。 */
	*month = 1;
	*yday  = 0;
	++*year;
}

/*
 * Checks every day in a 160000 years interval centered at 1970-01-01
 * against the expected result.
 */
/*
 * 逐日检查以 1970-01-01 为中心、总跨度 160000 年的区间，把 time64_to_tm() 输出与独立推进的
 * Gregorian 参考日期比较。逐日覆盖会自然命中普通月末、闰日、世纪例外、400 年恢复规则和负年份。
 *
 * time64_to_tm_test_date_range() - 对 time64_to_tm() 的超长日期范围执行 KUnit 穷举回归测试。
 *
 * 【调用位置】KUNIT_CASE_SLOW() 把本函数登记为慢速用例；内建测试由启动期 KUnit executor 调用，
 * 模块形式则在模块加载时调用。测试结束后 KUnit 汇总断言状态，本函数没有后续业务状态要发布。
 *
 * 【参数】@test 是 KUnit 创建并持有的非 NULL 测试上下文借用指针，用于记录断言位置、失败信息和
 * 用例状态；本函数不保存、不释放该对象。输入日期没有外部来源，均由函数内局部量构造。
 *
 * 【上下文与副作用】运行在 KUnit 测试执行上下文，不持 timekeeping 锁；循环只做算术和断言，
 * 不修改系统墙钟，也不分配长期对象。返回无直接值；全部比较通过时用例成功，任一致命断言失败时
 * KUnit 记录 FAIL_MSG 并立即中止当前用例，后续日期不再检查，已经完成的比较无需回滚。
 *
 * 【变量地图】total_secs 是 80000 个公历年的精确秒跨度；year/month/mdday/yday 是独立参考日历；
 * result 是每轮被测输出；secs 是相对 epoch 的测试秒值；days 只为失败消息提供可读的日偏移。
 *
 * 【阶段】1. 以完整 400 年周期计算对称边界并初始化参考日期；2. 每次前进 86400 秒调用被测函数；
 * 3. 比较年、月、日和年内日；4. 仅在本轮全部通过后推进参考日期。测试不检查日内字段和星期字段，
 * 它的职责边界是 Gregorian 日期分解，不能单独证明 offset、时分秒或 tm_wday 正确。
 */
static void time64_to_tm_test_date_range(struct kunit *test)
{
	/*
	 * 80000 years	= (80000 / 400) * 400 years
	 *		= (80000 / 400) * 146097 days
	 *		= (80000 / 400) * 146097 * 86400 seconds
	 */
	/*
	 * 80000 年等于 200 个完整的 400 年 Gregorian 周期；每周期固定 146097 天，再乘每天 86400 秒。
	 * 先把 80000 转为 time64_t 可确保后续乘法在 64 位有符号域完成，不被较窄的 int 中间值截断。
	 */
	time64_t total_secs = ((time64_t) 80000) / 400 * 146097 * 86400;
	long year = 1970 - 80000;
	int month = 1;
	int mdday = 1;
	int yday = 0;

	struct tm result;
	time64_t secs;
	s64 days;

	/*
	 * 阶段 1：secs 从负边界到正边界（两端都包含）按整日递增；完整周期保证负边界恰好对应
	 * 1970-80000 年 1 月 1 日，所以参考状态无需先调用被测算法反推。
	 */
	for (secs = -total_secs; secs <= total_secs; secs += 86400) {

		/* 阶段 2：offset=0 隔离 UTC 公历换算；result 是本轮完全覆盖的栈上输出对象。 */
		time64_to_tm(secs, 0, &result);

		/* secs 始终整除 86400，days 是相对 1970-01-01 的有符号整日数，仅用于诊断。 */
		days = div_s64(secs, 86400);

		/*
		 * FAIL_MSG 展开时读取本轮参考日期与日偏移，让首次失败可直接定位。预处理宏虽写在循环体内，
		 * 但没有 C 块作用域；本文件后面不再使用该名字，因此无需额外 #undef。
		 */
		#define FAIL_MSG "%05ld/%02d/%02d (%2d) : %lld", \
			year, month, mdday, yday, days

		/*
		 * 阶段 3：struct tm 以“距 1900 年数”和零基月份编码，参考状态因此只在比较表达式中转换；
		 * 四个 ASSERT 都是致命断言，任一不等就保留当前参考状态并中止用例，避免级联噪声。
		 */
		KUNIT_ASSERT_EQ_MSG(test, year - 1900, result.tm_year, FAIL_MSG);
		KUNIT_ASSERT_EQ_MSG(test, month - 1, result.tm_mon, FAIL_MSG);
		KUNIT_ASSERT_EQ_MSG(test, mdday, result.tm_mday, FAIL_MSG);
		KUNIT_ASSERT_EQ_MSG(test, yday, result.tm_yday, FAIL_MSG);

		/* 阶段 4：只有当前日期四项全通过才推进期望值，使下一轮 secs 与参考日期继续一一对应。 */
		advance_date(&year, &month, &mdday, &yday);
	}
}

/*
 * time_test_cases 是静态存续期的 KUnit 用例表：首项保存测试函数、字符串名、模块名和 SLOW 属性；
 * 末尾全零 struct kunit_case 是框架遍历的哨兵。数组由下方 suite 借用，运行期不转移所有权。
 */
static struct kunit_case time_test_cases[] = {
	/* 穷举约 5844 万个日期点，显式标为慢速，便于 KUnit 按速度属性筛选。 */
	KUNIT_CASE_SLOW(time64_to_tm_test_date_range),
	{}
};

/*
 * time_test_suite 把名称与零结尾用例表组成一个静态测试套件；未设置 init/exit，说明每个用例没有
 * 共享 fixture、引用或清理阶段。KUnit executor 在注册后读取它，文件本身不主动调用测试函数。
 */
static struct kunit_suite time_test_suite = {
	.name = "time_test_cases",
	.test_cases = time_test_cases,
};

/*
 * 宏把 &time_test_suite 放入 .kunit_test_suites ELF section：内建时由启动期 executor 枚举，构建成
 * 模块时在加载阶段运行。这是测试可见性的发布点；对象均为静态存续期，不需要引用计数或释放。
 */
kunit_test_suite(time_test_suite);
/* 模块元数据只描述测试套件用途与许可证，不参与用例控制流或日期换算。 */
MODULE_DESCRIPTION("time unit test suite");
MODULE_LICENSE("GPL");
