// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/seq_file.h>

/*
 * done 区分“测试过且零坏字节”与“根本未运行”；bad_size 累计已从 memblock
 * 隔离的半开物理区间长度，供 /proc/meminfo 的 EarlyMemtestBad 输出。
 */
static bool early_memtest_done;
static phys_addr_t early_memtest_bad_size;

/* 每一项是一轮 64 位写入/读回值；__initdata 表示启动测试结束后可回收。 */
static u64 patterns[] __initdata = {
	/* The first entry has to be 0 to leave memtest with zeroed memory */
	/* 反向执行的最后一轮必须使用 0，避免把可用 RAM 留成诊断花纹。 */
	0,
	0xffffffffffffffffULL,
	0x5555555555555555ULL,
	0xaaaaaaaaaaaaaaaaULL,
	0x1111111111111111ULL,
	0x2222222222222222ULL,
	0x4444444444444444ULL,
	0x8888888888888888ULL,
	/* 下面继续组合相邻位，覆盖不同数据线同时为 1/0 时的耦合错误。 */
	0x3333333333333333ULL,
	0x6666666666666666ULL,
	0x9999999999999999ULL,
	0xccccccccccccccccULL,
	0x7777777777777777ULL,
	0xbbbbbbbbbbbbbbbbULL,
	0xddddddddddddddddULL,
	0xeeeeeeeeeeeeeeeeULL,
	0x7a6c7258554e494cULL, /* yeah ;-) */
};

/*
 * reserve_bad_mem() - 记录并隔离一段连续读回失败的物理内存。
 *
 * 区间采用 [start_bad, end_bad)；日志按内存字节顺序展示 pattern，随后用
 * memblock_reserve() 使该范围不再参与后续 free-range 遍历和启动分配。函数累计
 * 隔离字节数但不处理 reserve 失败，调用期仍处于 memblock 可修改的早期启动阶段。
 */
static void __init reserve_bad_mem(u64 pattern, phys_addr_t start_bad, phys_addr_t end_bad)
{
	/* cpu_to_be64 让不同端序机器的十六进制日志对应一致的内存字节花纹。 */
	pr_info("  %016llx bad mem addr %pa - %pa reserved\n",
		cpu_to_be64(pattern), &start_bad, &end_bad);
	memblock_reserve(start_bad, end_bad - start_bad);
	early_memtest_bad_size += (end_bad - start_bad);
}

/*
 * memtest() - 对一个可直接映射的物理区间执行单一 64 位花纹写入/校验。
 *
 * 输入区间为 [start_phys, start_phys + size)，首地址向上按 8 字节对齐，不能组成
 * 完整 u64 的边缘字节不测试。函数先写完整区间再逐字读回，将相邻失败字合并后
 * reserve；成功字不保留引用或状态。调用者保证区间可由 __va() 访问且长度足以
 * 容纳对齐偏移。完成哪怕一个区间后设置 done，供 proc 报告区分未运行状态。
 */
static void __init memtest(u64 pattern, phys_addr_t start_phys, phys_addr_t size)
{
	u64 *p, *start, *end;
	phys_addr_t start_bad, last_bad;
	phys_addr_t start_phys_aligned;
	const size_t incr = sizeof(pattern);

	/* 虚拟游标与物理游标保持同一 8 字节步进，便于把失败位置映射回 memblock。 */
	start_phys_aligned = ALIGN(start_phys, incr);
	start = __va(start_phys_aligned);
	end = start + (size - (start_phys_aligned - start_phys)) / incr;
	start_bad = 0;
	last_bad = 0;

	VM_WARN_ON_ONCE(size < start_phys_aligned - start_phys);

	/* WRITE_ONCE/READ_ONCE 阻止编译器合并或消除这类有意的 RAM 探测访问。 */
	for (p = start; p < end; p++)
		WRITE_ONCE(*p, pattern);

	for (p = start; p < end; p++, start_phys_aligned += incr) {
		/* 匹配的字结束任何潜在坏段之前的搜索，但无需立即执行动作。 */
		if (READ_ONCE(*p) == pattern)
			continue;
		if (start_phys_aligned == last_bad + incr) {
			/* 当前失败紧邻上一失败，只扩展半开区间尾部，暂不重复 reserve。 */
			last_bad += incr;
			continue;
		}
		/* 新的非连续失败出现前，先提交上一段；物理地址 0 用作“无坏段”哨兵。 */
		if (start_bad)
			reserve_bad_mem(pattern, start_bad, last_bad + incr);
		start_bad = last_bad = start_phys_aligned;
	}
	/* 循环结束后冲刷最后一个尚未提交的连续坏段。 */
	if (start_bad)
		reserve_bad_mem(pattern, start_bad, last_bad + incr);

	early_memtest_done = true;
}

/*
 * do_one_pass() - 只对请求窗口内当前仍空闲的 memblock 区间执行一种花纹。
 *
 * for_each_free_mem_range 排除已保留内存，包括早先检测出的坏段；每个候选区间再
 * clamp 到架构传入的 [start,end) 窗口。空交集跳过，非空交集记录日志并交给
 * memtest()。本函数会因新坏段 reservation 改变后续轮次可见的 free ranges。
 */
static void __init do_one_pass(u64 pattern, phys_addr_t start, phys_addr_t end)
{
	u64 i;
	phys_addr_t this_start, this_end;

	/* NUMA_NO_NODE/MEMBLOCK_NONE 表示跨节点扫描普通可用范围，不按 flags 过滤。 */
	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &this_start,
				&this_end, NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);
		/* clamp 可能把窗口外候选压成空区间，必须先比较再计算长度。 */
		if (this_start < this_end) {
			pr_info("  %pa - %pa pattern %016llx\n",
				&this_start, &this_end, cpu_to_be64(pattern));
			memtest(pattern, this_start, this_end - this_start);
		}
	}
}

/* default is disabled */
/* 0 不执行；无参数的 memtest 使用完整 pattern 表，有数值时表示轮次数。 */
static unsigned int memtest_pattern __initdata;

/*
 * parse_memtest() - 解析早期 memtest[=次数] 启动参数。
 *
 * 显式值按基数 0 转为 unsigned int，转换错误原样返回；只有裸 `memtest` 才选择
 * ARRAY_SIZE(patterns)。参数本身不运行测试，early_memtest() 稍后使用该轮次数。
 */
static int __init parse_memtest(char *arg)
{
	int ret = 0;

	if (arg)
		ret = kstrtouint(arg, 0, &memtest_pattern);
	else
		memtest_pattern = ARRAY_SIZE(patterns);

	return ret;
}

early_param("memtest", parse_memtest);

/*
 * early_memtest() - 按启动参数在架构给定物理窗口中执行全部测试轮次。
 *
 * [start,end) 由架构保证已建立线性映射；0 轮直接返回。其余情况按索引反向执行，
 * 每轮只触碰当时仍空闲的 memblock 范围，最终以零花纹收尾。函数无错误返回；检测
 * 到的坏段通过 reservation 从后续启动分配中排除，轮次数过大将相应延长启动时间。
 */
void __init early_memtest(phys_addr_t start, phys_addr_t end)
{
	unsigned int i;
	unsigned int idx = 0;

	/* 默认 0 或显式 memtest=0 都保持禁用，不产生 done/report 状态。 */
	if (!memtest_pattern)
		return;

	pr_info("early_memtest: # of tests: %u\n", memtest_pattern);
	/*
	 * unsigned 反向循环恰好执行 memtest_pattern 次，并以 i==0 的 pattern[0]
	 * 收尾；次数超过表长时通过取模重复花纹序列。
	 */
	for (i = memtest_pattern-1; i < UINT_MAX; --i) {
		idx = i % ARRAY_SIZE(patterns);
		do_one_pass(patterns[idx], start, end);
	}
}

/*
 * memtest_report_meminfo() - 按 /proc/meminfo 格式输出早期测试隔离量。
 *
 * m 由 proc 生成路径借用；未启用 procfs 或尚未跑过任何实际区间时无输出。已运行
 * 时输出 KiB，非零不足 1 KiB 向上钳为 1，而精确 0 保留为“测试成功且无坏段”。
 */
void memtest_report_meminfo(struct seq_file *m)
{
	unsigned long early_memtest_bad_size_kb;

	/* 编译期关闭 procfs 时保留空接口，避免 seq_file 输出路径成为硬依赖。 */
	if (!IS_ENABLED(CONFIG_PROC_FS))
		return;

	/* 未测试时完全省略字段；这与“已测试但坏内存为 0”有意不同。 */
	if (!early_memtest_done)
		return;

	early_memtest_bad_size_kb = early_memtest_bad_size >> 10;
	/* 非零但不足 1 KiB 的坏区间向上报告为 1，避免显示成无坏内存。 */
	if (early_memtest_bad_size && !early_memtest_bad_size_kb)
		early_memtest_bad_size_kb = 1;
	/* When 0 is reported, it means there actually was a successful test */
	/* 因 done 已通过门禁，输出 0 明确表示至少一轮完成且没有检测到坏字。 */
	seq_printf(m, "EarlyMemtestBad:   %5lu kB\n", early_memtest_bad_size_kb);
}
