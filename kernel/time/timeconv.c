// SPDX-License-Identifier: LGPL-2.0+
/*
 * Copyright (C) 1993, 1994, 1995, 1996, 1997 Free Software Foundation, Inc.
 * This file is part of the GNU C Library.
 * Contributed by Paul Eggert (eggert@twinsun.com).
 *
 * The GNU C Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * The GNU C Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with the GNU C Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/*
 * Converts the calendar time to broken-down time representation
 *
 * 2009-7-14:
 *   Moved from glibc-2.6 to kernel by Zhaolei<zhaolei@cn.fujitsu.com>
 * 2021-06-02:
 *   Reimplemented by Cassio Neri <cassio.neri@gmail.com>
 */
/*
 * 本文件把“自 Unix epoch 起的线性秒数”拆成 struct tm 的年、月、日、时、分、秒、星期和年内日。
 * 代码最初来自 glibc，2009 年移入内核，2021 年改用常数时间的 Gregorian 算法；它不查询时区、
 * 夏令时或闰秒表，调用者若要本地时间必须显式提供固定秒偏移。
 */

#include <linux/time.h>
#include <linux/module.h>
#include <linux/kernel.h>

#define SECS_PER_HOUR	(60 * 60)
#define SECS_PER_DAY	(SECS_PER_HOUR * 24)
/* 两个宏分别表示每小时 3600 秒和每个公历日 86400 秒，是拆分日数与日内余数的统一单位。 */

/**
 * time64_to_tm - converts the calendar time to local broken-down time
 *
 * @totalsecs:	the number of seconds elapsed since 00:00:00 on January 1, 1970,
 *		Coordinated Universal Time (UTC).
 * @offset:	offset seconds adding to totalsecs.
 * @result:	pointer to struct tm variable to receive broken-down time
 */
/*
 * time64_to_tm() - 把 64 位 Unix 秒数和固定偏移拆成内核 struct tm。
 *
 * 【宏观位置】调用者包括 printk 时间格式化、FAT/UDF/exFAT 时间戳编码和测试驱动；本函数完成
 * 纯日历换算，调用者随后格式化字段或编码到外部格式。公开声明位于 include/linux/time.h。
 *
 * 【参数】
 * @totalsecs：纯输入、有符号 time64_t，表示从 1970-01-01 00:00:00 UTC 起经过的秒数，可为负。
 * @offset：纯输入、有符号秒偏移，直接加到日内余数；它不是时区对象，不执行 DST 规则。
 * @result：调用者提供的非 NULL 输出对象借用指针；函数覆盖全部八个字段，不保存指针、不转移所有权。
 *
 * 【上下文与副作用】只做固定次数整数运算，不分配、不持锁、不访问全局可变状态、不可失败，因而
 * 可在任何允许普通算术的上下文调用且不睡眠。返回无直接值；成功保证 tm_sec/min/hour、tm_wday、
 * tm_mon、tm_mday、tm_yday 和 tm_year 均按 struct tm 约定填写。闰秒不会由该线性秒算法产生 60。
 *
 * 【变量地图】days/rem/remainder 把输入拆成公历日编号和规范化日内秒；udays 加大常量后把整个
 * time64 范围映射到非负域。century/year_of_century/day_of_* 是 March-based 计算历中间量；
 * month/day 是零基月和一基日前的临时值；两个 bool 负责闰年以及一、二月跨计算年边界的修正。
 */
void time64_to_tm(time64_t totalsecs, int offset, struct tm *result)
{
	u32 u32tmp, day_of_century, year_of_century, day_of_year, month, day;
	u64 u64tmp, udays, century, year;
	bool is_Jan_or_Feb, is_leap_year;
	long days, rem;
	int remainder;

	/*
	 * 阶段 1：拆分“整日 + 日内余秒”。div_s64_rem() 的负数余数可能为负，固定 offset 也可能
	 * 把余数推到相邻日；两个循环同步调整 days，最终建立 0 <= rem < 86400 的不变量。
	 */
	days = div_s64_rem(totalsecs, SECS_PER_DAY, &remainder);
	rem = remainder;
	rem += offset;
	while (rem < 0) {
		rem += SECS_PER_DAY;
		--days;
	}
	while (rem >= SECS_PER_DAY) {
		rem -= SECS_PER_DAY;
		++days;
	}

	/* 阶段 2：在规范化余数上依次取小时、分钟、秒，输出范围分别为 0..23、0..59、0..59。 */
	result->tm_hour = rem / SECS_PER_HOUR;
	rem %= SECS_PER_HOUR;
	result->tm_min = rem / 60;
	result->tm_sec = rem % 60;

	/* January 1, 1970 was a Thursday. */
	/* 1970-01-01 是星期四（tm_wday=4）；负模可能为负，因此再加 7 归一化到 0..6。 */
	result->tm_wday = (4 + days) % 7;
	if (result->tm_wday < 0)
		result->tm_wday += 7;

	/*
	 * The following algorithm is, basically, Proposition 6.3 of Neri
	 * and Schneider [1]. In a few words: it works on the computational
	 * (fictitious) calendar where the year starts in March, month = 2
	 * (*), and finishes in February, month = 13. This calendar is
	 * mathematically convenient because the day of the year does not
	 * depend on whether the year is leap or not. For instance:
	 *
	 * March 1st		0-th day of the year;
	 * ...
	 * April 1st		31-st day of the year;
	 * ...
	 * January 1st		306-th day of the year; (Important!)
	 * ...
	 * February 28th	364-th day of the year;
	 * February 29th	365-th day of the year (if it exists).
	 *
	 * After having worked out the date in the computational calendar
	 * (using just arithmetics) it's easy to convert it to the
	 * corresponding date in the Gregorian calendar.
	 *
	 * [1] "Euclidean Affine Functions and Applications to Calendar
	 * Algorithms". https://arxiv.org/abs/2102.06959
	 *
	 * (*) The numbering of months follows tm more closely and thus,
	 * is slightly different from [1].
	 */
	/*
	 * 下述算法基本对应 Neri 与 Schneider 论文命题 6.3。它先进入一个虚构的“计算历”：一年从
	 * 3 月（month=2，沿用 struct tm 的零基编号）开始，到次年 2 月（month=13）结束。这样闰日
	 * 永远落在计算年的末尾，计算年内日时无需先判断本年是否闰年。
	 *
	 * 在这个坐标系中：3 月 1 日是第 0 日，4 月 1 日是第 31 日，1 月 1 日是第 306 日；2 月 28 日
	 * 是第 364 日，存在的 2 月 29 日是第 365 日。先用纯整数算术得到计算历日期，再映回 Gregorian
	 * 公历。月份编号因贴近 struct tm 而与论文略有差异；论文链接和算法来源保留在上方原注释中。
	 */

	/*
	 * 阶段 3：加常量把有符号 days 平移到 u64 非负域。常量同时选择一个与 Gregorian 400 年周期
	 * 对齐的远古基点，使后续无符号除法覆盖 time64 的负年份，而不在热路径设置正负分支。
	 */
	udays	= ((u64) days) + 2305843009213814918ULL;

	/*
	 * 每 400 年恰有 146097 日。把 udays 先乘 4，使除以该常量所得 quotient 按公历世纪推进，
	 * 同时仍保留“逢 400 年不取消闰日”的修正；余数再除 4 得到当前世纪内的零基日序。
	 */
	u64tmp		= 4 * udays + 3;
	century		= div64_u64_rem(u64tmp, 146097, &u64tmp);
	day_of_century	= (u32) (u64tmp / 4);

	/*
	 * 阶段 4：常数 2939745 和 32 位高/低半部把除法变为定点乘法，同时求出 century 内年份与
	 * March-based 年内日。u64tmp 的高 32 位给 year_of_century，低位经缩放还原 day_of_year。
	 */
	u32tmp		= 4 * day_of_century + 3;
	u64tmp		= 2939745ULL * u32tmp;
	year_of_century	= upper_32_bits(u64tmp);
	day_of_year	= lower_32_bits(u64tmp) / 2939745 / 4;

	/* 组合绝对计算年；世纪内非零年份按 4 整除，世纪边界则按 400 年周期判断闰年。 */
	year		= 100 * century + year_of_century;
	is_leap_year	= year_of_century ? !(year_of_century % 4) : !(century % 4);

	/* 常数仿射式把计算年内日映射为零基 month 和月内零基 day，避免逐月表查找与分支。 */
	u32tmp		= 2141 * day_of_year + 132377;
	month		= u32tmp >> 16;
	day		= ((u16) u32tmp) / 2141;

	/*
	 * Recall that January 1st is the 306-th day of the year in the
	 * computational (not Gregorian) calendar.
	 */
	/* 计算历中 1 月 1 日是第 306 日；达到该阈值说明日期属于公历年的一月或二月。 */
	is_Jan_or_Feb	= day_of_year >= 306;

	/* Convert to the Gregorian calendar and adjust to Unix time. */
	/*
	 * 阶段 5：从远古平移基点还原 Unix 公历年。一、二月在计算历属于上一计算年的末尾，所以年加 1、
	 * month 减 12；day 从零基改为一基。tm_yday 则把 March-based 日序旋回 1 月 1 日起的零基日序，
	 * 三月至十二月需补上 1、2 月长度，一、二月需减去 306。
	 */
	year		= year + is_Jan_or_Feb - 6313183731940000ULL;
	month		= is_Jan_or_Feb ? month - 12 : month;
	day		= day + 1;
	day_of_year	+= is_Jan_or_Feb ? -306 : 31 + 28 + is_leap_year;

	/* Convert to tm's format. */
	/*
	 * 阶段 6：提交输出。struct tm 的 year 是距 1900 年数，month 是 0..11，mday 是 1..31，
	 * yday 是 0..365；前面已经写入 hour/min/sec/wday，因此返回时八个字段形成同一日期快照。
	 */
	result->tm_year = (long) (year - 1900);
	result->tm_mon  = (int) month;
	result->tm_mday = (int) day;
	result->tm_yday = (int) day_of_year;
}
EXPORT_SYMBOL(time64_to_tm);
