// SPDX-License-Identifier: GPL-2.0+
/*
 * debugfs file to track time spent in suspend
 *
 * Copyright (c) 2011, Google, Inc.
 */
/*
 * 本文件通过 debugfs 暴露两类纯诊断统计：每次注入的 suspend 秒数按 2 的幂分桶，以及 multigrain
 * timestamp floor 成功交换的 per-CPU 累计次数。读取不冻结写者，结果允许近似；这些计数不参与
 * timekeeping 正确性、时间发布或电源管理决策。
 */

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/seq_file.h>
#include <linux/suspend.h>
#include <linux/time.h>

#include "timekeeping_internal.h"

#define NUM_BINS 32

/* Incremented every time mg_floor is updated */
/* 每次 mg_floor 成功更新时递增；per-CPU 布局减少文件时间戳热路径的 cacheline 争用。 */
DEFINE_PER_CPU(unsigned long, timekeeping_mg_floor_swaps);

/* 下标 0 表示约 0..1 秒，后续下标表示相邻 2 次幂秒区间；无锁计数仅用于诊断展示。 */
static unsigned int sleep_time_bin[NUM_BINS] = {0};

/*
 * tk_debug_sleep_time_show() - 输出所有非零 suspend 时长桶的区间和累计次数。
 * @s 是 seq_file 输出上下文借用指针；@data 未使用。bin 从 0 遍历固定 32 桶，空桶跳过；成功返回 0，
 * seq_file 写错误由其缓冲机制记录而非本函数返回。读取与记账无锁并发，单次展示可能混合相邻更新；
 * 静态数组和 seq_file 均不转移所有权，本函数可睡眠于 debugfs 读取上下文。
 */
static int tk_debug_sleep_time_show(struct seq_file *s, void *data)
{
	unsigned int bin;
	/* 表头固定说明单位为秒，随后每行打印 [下界, 上界) 形式的 2 次幂区间。 */
	seq_puts(s, "      time (secs)        count\n");
	seq_puts(s, "------------------------------\n");
	for (bin = 0; bin < 32; bin++) {
		/* 省略 0 计数桶，保持 debugfs 输出紧凑；并发递增可能使本次判断稍旧。 */
		if (sleep_time_bin[bin] == 0)
			continue;
		seq_printf(s, "%10u - %-10u %4u\n",
			bin ? 1 << (bin - 1) : 0, 1 << bin,
				sleep_time_bin[bin]);
	}
	return 0;
}
/* 生成只读 open/read/llseek/release 包装和静态 file_operations，show 是其借用回调。 */
DEFINE_SHOW_ATTRIBUTE(tk_debug_sleep_time);

/*
 * tk_debug_sleep_time_init() - 在 late init 阶段创建根 debugfs 的只读 `sleep_time` 文件。
 * 无入参，固定返回 0；debugfs 不可用或创建失败时 helper 可返回错误指针/NULL，但本实现刻意不传播，
 * 因诊断文件不能阻止启动。file_operations 静态常驻，无私有数据；debugfs 负责目录项生命周期。
 */
static int __init tk_debug_sleep_time_init(void)
{
	debugfs_create_file("sleep_time", 0444, NULL, NULL,
			    &tk_debug_sleep_time_fops);
	return 0;
}
/* 文件在主要子系统初始化后注册一次，不提供卸载路径，因为本对象内建且静态常驻。 */
late_initcall(tk_debug_sleep_time_init);

/*
 * tk_debug_account_sleep_time() - 把一次已验证的 suspend 注入时长计入秒级对数桶并延迟打印日志。
 * @t 是 timekeeping 注入路径借用的非空、非负规范 timespec64，不保存。bin 取 tv_sec 最高有效位位置并
 * 钳到最后一桶，因此不足 1 秒进入桶 0，1 秒进入桶 1，32 位秒范围的大值合并入桶 31；tv_nsec 只用于
 * 毫秒日志。`fls()` 接收 unsigned int，极端的 >=2^32 秒会先截断低 32 位，这是诊断接口的现有边界。
 * 无返回值。调用者以 timekeeper 写事务串行正常注入，但 debugfs 读侧不加锁，展示允许竞态近似；
 * `pm_deferred_pr_dbg` 避免在时间维护临界区同步执行普通 printk 工作，本函数本身不睡眠。
 */
void tk_debug_account_sleep_time(const struct timespec64 *t)
{
	/* Cap bin index so we don't overflow the array */
	/* 将桶号钳在 NUM_BINS-1，保证任何合理 suspend 秒数都不会越过静态数组。 */
	int bin = min(fls(t->tv_sec), NUM_BINS-1);

	/* 这是诊断计数而非同步状态；读者可观察递增前或后的完整 unsigned int。 */
	sleep_time_bin[bin]++;
	pm_deferred_pr_dbg("Timekeeping suspended for %lld.%03lu seconds\n",
			   (s64)t->tv_sec, t->tv_nsec / NSEC_PER_MSEC);
}

/*
 * timekeeping_get_mg_floor_swaps() - 汇总所有 possible CPU 的 multigrain floor 成功交换次数。
 * 无入参；sum 是可能发生 unsigned long 自然回绕的累计值，cpu 遍历 possible CPU，包括当前离线槽位。
 * 用 `data_race` 明示接受与本地无锁递增并发，故返回近似快照而非同一时刻总和；不重置计数、不睡眠、
 * 不取得 CPU hotplug 锁或 per-CPU 对象引用。
 */
unsigned long timekeeping_get_mg_floor_swaps(void)
{
	unsigned long sum = 0;
	int cpu;

	/* 每个槽位独立累加；并发写只影响本次读到旧值还是新值，不要求跨 CPU 一致版本。 */
	for_each_possible_cpu(cpu)
		sum += data_race(per_cpu(timekeeping_mg_floor_swaps, cpu));

	return sum;
}
