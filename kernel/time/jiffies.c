// SPDX-License-Identifier: GPL-2.0+
/*
 * This file contains the jiffies based clocksource.
 *
 * Copyright (C) 2004, 2005 IBM, John Stultz (johnstul@us.ibm.com)
 */
/*
 * 本文件提供以全局 jiffies 为周期源的最低保真 clocksource，并在 32 位平台提供无撕裂的
 * jiffies_64 读取；还可按平台基准频率注册 refined-jiffies，并为 sysctl 在秒、USER_HZ、毫秒与
 * 内核 jiffy 之间转换。jiffies clocksource 是所有平台都能工作的后备，不是高精度首选。
 */
#include <linux/clocksource.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/init.h>

#include "timekeeping.h"
#include "tick-internal.h"


/*
 * jiffies_read() - 向 clocksource 核心返回低 32 位 jiffies 周期值。
 *
 * @cs 是 clocksource 回调 ABI 传入的借用指针，本实现不需读取它；返回值低 32 位有效，与对象 mask
 * 配对处理自然回绕。只读全局标量、不加锁、不睡眠；clocksource 累积层负责缩放和回绕扩展。
 */
static u64 jiffies_read(struct clocksource *cs)
{
	return (u64) jiffies;
}

/*
 * The Jiffies based clocksource is the lowest common
 * denominator clock source which should function on
 * all systems. It has the same coarse resolution as
 * the timer interrupt frequency HZ and it suffers
 * inaccuracies caused by missed or lost timer
 * interrupts and the inability for the timer
 * interrupt hardware to accurately tick at the
 * requested HZ value. It is also not recommended
 * for "tick-less" systems.
 */
/*
 * jiffies 是所有系统共有的最低能力 clocksource，分辨率仅为 1/HZ。丢失 tick 中断、硬件无法精确
 * 产生目标 HZ 都会形成误差；tickless 系统长时间停周期 tick 时也不适合依赖它。因此 rating 设为
 * 最低有效值 1，只在更可靠 clocksource 不可用时兜底。
 */
/*
 * clocksource_jiffies 是静态永久对象：read 返回 32 位周期，mult/shift 把一个 jiffy 换成纳秒；
 * max_cycles=10 表示普通 cycles*mult 快式的安全 delta 上限，更大差值由 core 改走防溢出的慢式。
 * 注册后 core 只借用该对象，不发生释放。
 */
static struct clocksource clocksource_jiffies = {
	.name			= "jiffies",
	.rating			= 1, /* lowest valid rating*/
	/* rating=1 是最低有效优先级，保证任何质量更高的硬件源都能替换它。 */
	.read			= jiffies_read,
	.mask			= CLOCKSOURCE_MASK(32),
	.mult			= TICK_NSEC << JIFFIES_SHIFT, /* details above */
	/* mult 预乘 2^shift，clocksource 通式 (cycles * mult) >> shift 恢复每 tick 纳秒。 */
	.shift			= JIFFIES_SHIFT,
	.max_cycles		= 10,
};

/*
 * jiffies_lock 串行化 tick 写者；SMP 缓存行对齐避免与无关热变量伪共享。jiffies_seq 绑定该 raw lock，
 * 写侧在 tick-common/timekeeping 路径持锁并包住 jiffies_64 更新，32 位无锁读者用 seqcount 重试。
 * 锁负责写者互斥，seqcount 负责读快照一致性；二者都不延长任何对象生命周期。
 */
__cacheline_aligned_in_smp DEFINE_RAW_SPINLOCK(jiffies_lock);
__cacheline_aligned_in_smp seqcount_raw_spinlock_t jiffies_seq =
	SEQCNT_RAW_SPINLOCK_ZERO(jiffies_seq, &jiffies_lock);

#if (BITS_PER_LONG < 64)
/*
 * get_jiffies_64() - 在 32 位平台取得一致的 64 位全局 tick 快照。
 *
 * 入参：无。主要调用者是需要长期、不受低位回绕影响的超时/统计代码；64 位平台由头文件直接读取。
 * 本函数不持写锁、不可睡眠；seqcount 读段可能因并发 tick 写入重试。返回某个完整写段前或后的
 * jiffies_64，不保证与随后读取的其他状态同一时刻，也不修改全局状态。
 */
u64 get_jiffies_64(void)
{
	unsigned int seq;
	u64 ret;

	/* seq 为版本戳，ret 为候选快照；奇数版本或读取期间变化都要求重新读取两个 32 位半部。 */
	do {
		seq = read_seqcount_begin(&jiffies_seq);
		ret = jiffies_64;
	} while (read_seqcount_retry(&jiffies_seq, seq));
	return ret;
}
EXPORT_SYMBOL(get_jiffies_64);
#endif

/* jiffies 的实际存储与低/高位别名由 timer.c/链接脚本提供；这里把低位 ABI 名称导出给模块。 */
EXPORT_SYMBOL(jiffies);

/* 启动期一次性标志，防止两个默认时钟查询入口重复把同一静态对象加入 clocksource 队列。 */
static bool cs_jiffies_registered __initdata;

/*
 * clocksource_default_clock() - 提供并按需注册架构默认的最小 clocksource。
 *
 * 入参：无；返回永久静态 clocksource_jiffies 的借用指针，不会为 NULL。仅在 __init 启动串行阶段
 * 调用，可睡眠性取决于注册核心但调用点不处于中断。首次调用注册并置标志，后续幂等返回。
 * __weak 允许 s390 等架构用强定义替换默认选择；本函数和标志在 init 段释放后不可再调用/访问。
 */
struct clocksource * __init __weak clocksource_default_clock(void)
{
	if (!cs_jiffies_registered) {
		__clocksource_register(&clocksource_jiffies);
		cs_jiffies_registered = true;
	}
	return &clocksource_jiffies;
}

/* refined_jiffies 是启动期填充、注册后永久存活的静态副本；使用平台实测/标称频率改进 mult。 */
static struct clocksource refined_jiffies;

/*
 * register_refined_jiffies() - 用平台每秒输入周期数校准并注册更精确的 jiffies clocksource。
 *
 * @cycles_per_second 是平台 timer 输入频率（周期/秒），必须为正且足以使四舍五入后的
 * cycles_per_tick 非零；纯输入、不保存。x86 setup 等启动路径调用，函数标记 __init、不可并发重入。
 * 返回无直接值；成功后 refined_jiffies 被注册且 rating 比后备源高 1。无显式错误返回或回滚，
 * 静态对象由 clocksource core 借用到系统结束。
 */
void __init register_refined_jiffies(long cycles_per_second)
{
	u64 nsec_per_tick, shift_hz;
	long cycles_per_tick;

	/* 阶段 1：复制全部基础契约，只替换身份、选择优先级和换算乘数。 */
	refined_jiffies = clocksource_jiffies;
	refined_jiffies.name = "refined-jiffies";
	refined_jiffies.rating++;

	/* Calc cycles per tick */
	/* 计算每个 jiffy 对应的输入周期数；加 HZ/2 后整数除法实现最近整数舍入。 */
	cycles_per_tick = (cycles_per_second + HZ/2)/HZ;
	/* shift_hz stores hz<<8 for extra accuracy */
	/* shift_hz 用 8 个小数位保存实际 tick 频率；除法前加一半分母同样做最近整数舍入。 */
	shift_hz = (u64)cycles_per_second << 8;
	shift_hz += cycles_per_tick/2;
	do_div(shift_hz, cycles_per_tick);
	/* Calculate nsec_per_tick using shift_hz */
	/* 以相同 Q8 精度反求每 tick 纳秒，避免先把非整数实际 HZ 截断。 */
	nsec_per_tick = (u64)NSEC_PER_SEC << 8;
	nsec_per_tick += (u32)shift_hz/2;
	do_div(nsec_per_tick, (u32)shift_hz);

	/* 阶段 2：写入定点乘数并注册；本函数不再改对象，core 会补齐 max_idle 等派生字段。 */
	refined_jiffies.mult = ((u32)nsec_per_tick) << JIFFIES_SHIFT;

	__clocksource_register(&refined_jiffies);
}

#ifdef CONFIG_PROC_SYSCTL
/*
 * mult_hz() - 把用户可见的整秒数转换为内核 jiffies。
 * @val 是无符号纯输入秒数；返回 val*HZ，不保存状态。调用它的通用转换层随后检查是否可装入 int。
 */
static ulong mult_hz(const ulong val)
{
	return val * HZ;
}

/*
 * div_hz() - 把内核 jiffies 截断转换为用户可见整秒。
 * @val 是无符号 jiffy 数；返回 val/HZ，余下不足一秒部分被舍弃。纯算术、无副作用。
 */
static ulong div_hz(const ulong val)
{
	return val / HZ;
}

/*
 * sysctl_u2k_int_conv_hz() - 写 sysctl 时把符号+秒数转换并提交为内核 jiffy int。
 * @negp/@u_ptr 是解析器借用的符号和幅值输入，@k_ptr 是 WRITE_ONCE 输出；返回 0 或溢出 -EINVAL。
 */
static int sysctl_u2k_int_conv_hz(const bool *negp, const ulong *u_ptr, int *k_ptr)
{
	return proc_int_u2k_conv_uop(u_ptr, k_ptr, negp, mult_hz);
}

/*
 * sysctl_k2u_int_conv_hz() - 读 sysctl 时把内核 jiffy int 拆成符号+整秒幅值。
 * @k_ptr 是 READ_ONCE 输入，@negp/@u_ptr 是输出；返回 0。div_hz 截断亚秒部分且 helper 不查溢出。
 */
static int sysctl_k2u_int_conv_hz(bool *negp, ulong *u_ptr, const int *k_ptr)
{
	return proc_int_k2u_conv_kop(u_ptr, k_ptr, negp, div_hz);
}

/*
 * sysctl_u2k_int_conv_userhz() - 把 USER_HZ tick 幅值转换为内核 jiffies 并写入 int。
 * 三个指针的输入/输出属性同 hz 写转换；clock_t_to_jiffies() 处理 USER_HZ 与 HZ 不同的比例。
 * 返回 0 或 -EINVAL，不保存指针。
 */
static int sysctl_u2k_int_conv_userhz(const bool *negp, const ulong *u_ptr, int *k_ptr)
{
	return proc_int_u2k_conv_uop(u_ptr, k_ptr, negp, clock_t_to_jiffies);
}

/*
 * sysctl_jiffies_to_clock_t() - 为函数指针 ABI 包装 jiffies_to_clock_t()。
 * @val 是纯输入 jiffy 幅值；返回 USER_HZ 单位幅值，无外部副作用。
 */
static ulong sysctl_jiffies_to_clock_t(const ulong val)
{
	return jiffies_to_clock_t(val);
}

/*
 * sysctl_k2u_int_conv_userhz() - 把内核 jiffy int 读取为符号+USER_HZ tick 幅值。
 * @k_ptr 为输入，@negp/@u_ptr 为输出；返回 0，换算由上述包装器完成，不转移 ownership。
 */
static int sysctl_k2u_int_conv_userhz(bool *negp, ulong *u_ptr, const int *k_ptr)
{
	return proc_int_k2u_conv_kop(u_ptr, k_ptr, negp, sysctl_jiffies_to_clock_t);
}

/*
 * sysctl_msecs_to_jiffies() - 为转换回调包装毫秒到 jiffy 的饱和/舍入规则。
 * @val 是纯输入毫秒幅值；返回 msecs_to_jiffies() 结果，无状态副作用。
 */
static ulong sysctl_msecs_to_jiffies(const ulong val)
{
	return msecs_to_jiffies(val);
}

/*
 * sysctl_u2k_int_conv_ms() - 把用户毫秒符号+幅值转换并 WRITE_ONCE 提交为 jiffy int。
 * @negp/@u_ptr 输入，@k_ptr 输出；返回 0 或换算后超出 int 范围的 -EINVAL。
 */
static int sysctl_u2k_int_conv_ms(const bool *negp, const ulong *u_ptr, int *k_ptr)
{
	return proc_int_u2k_conv_uop(u_ptr, k_ptr, negp, sysctl_msecs_to_jiffies);
}

/*
 * sysctl_jiffies_to_msecs() - 为读取转换回调包装 jiffy 到毫秒。
 * @val 是纯输入 jiffy 幅值；返回 jiffies_to_msecs() 结果，无外部副作用。
 */
static ulong sysctl_jiffies_to_msecs(const ulong val)
{
	return jiffies_to_msecs(val);
}

/*
 * sysctl_k2u_int_conv_ms() - 把内核 jiffy int 读取为符号+毫秒幅值。
 * @k_ptr 为 READ_ONCE 输入，@negp/@u_ptr 为输出；返回 0，不保存任何指针。
 */
static int sysctl_k2u_int_conv_ms(bool *negp, ulong *u_ptr, const int *k_ptr)
{
	return proc_int_k2u_conv_kop(u_ptr, k_ptr, negp, sysctl_jiffies_to_msecs);
}

/*
 * do_proc_int_conv_jiffies() - 按 @dir 在整秒文本幅值与 jiffy int 之间双向转换。
 * @negp/@u_ptr 是通用解析器的符号和幅值槽，@k_ptr 指向 table 数据，@tbl 为借用元数据；三者方向随
 * @dir 改变。返回 0 或下层 -EINVAL；false 表示不在换算后检查 extra1/extra2。
 */
static int do_proc_int_conv_jiffies(bool *negp, ulong *u_ptr, int *k_ptr,
				    int dir, const struct ctl_table *tbl)
{
	return proc_int_conv(negp, u_ptr, k_ptr, dir, tbl, false,
			     sysctl_u2k_int_conv_hz, sysctl_k2u_int_conv_hz);
}

/*
 * do_proc_int_conv_userhz_jiffies() - 按 @dir 双向转换 USER_HZ tick 与内核 jiffy int。
 * @negp/@u_ptr/@k_ptr/@tbl 均为调用期间借用的转换槽；返回 0 或 -EINVAL，不做 min/max 范围检查。
 */
static int do_proc_int_conv_userhz_jiffies(bool *negp, ulong *u_ptr,
					   int *k_ptr, int dir,
					   const struct ctl_table *tbl)
{
	return proc_int_conv(negp, u_ptr, k_ptr, dir, tbl, false,
			     sysctl_u2k_int_conv_userhz,
			     sysctl_k2u_int_conv_userhz);
}

/*
 * do_proc_int_conv_ms_jiffies() - 按 @dir 双向转换毫秒与内核 jiffy int。
 * 所有指针均为借用输入/输出槽，@tbl 仅透传；返回 0 或 -EINVAL，不检查 extra1/extra2。
 */
static int do_proc_int_conv_ms_jiffies(bool *negp, ulong *u_ptr, int *k_ptr,
				       int dir, const struct ctl_table *tbl)
{
	return proc_int_conv(negp, u_ptr, k_ptr, dir, tbl, false,
			     sysctl_u2k_int_conv_ms, sysctl_k2u_int_conv_ms);
}

/*
 * do_proc_int_conv_ms_jiffies_minmax() - 为 minmax 公共入口提供毫秒/jiffy 转换回调。
 * 参数与返回类别同普通 ms 回调。当前代码同样向 proc_int_conv() 传 false，因此不会使用 @tbl 的
 * extra1/extra2 做 int 范围检查；名称不应被理解为当前实现已经执行 min/max 校验。
 */
static int do_proc_int_conv_ms_jiffies_minmax(bool *negp, ulong *u_ptr,
					      int *k_ptr, int dir,
					      const struct ctl_table *tbl)
{
	return proc_int_conv(negp, u_ptr, k_ptr, dir, tbl, false,
			     sysctl_u2k_int_conv_ms, sysctl_k2u_int_conv_ms);
}

#else // CONFIG_PROC_SYSCTL
/* 未启用 proc sysctl 时保留同签名内部桩，所有参数均未读取，统一以 -ENOSYS 表示接口不可用。 */
/*
 * do_proc_int_conv_jiffies() - 无 CONFIG_PROC_SYSCTL 时的秒/jiffy 转换桩。
 * @negp/@u_ptr/@k_ptr/@tbl 均为未使用借用指针，@dir 未使用；返回固定 -ENOSYS，无副作用。
 */
static int do_proc_int_conv_jiffies(bool *negp, ulong *u_ptr, int *k_ptr,
				    int dir, const struct ctl_table *tbl)
{
	return -ENOSYS;
}

/*
 * do_proc_int_conv_userhz_jiffies() - 无 CONFIG_PROC_SYSCTL 时的 USER_HZ/jiffy 转换桩。
 * 五个参数均不读取或保存；返回固定 -ENOSYS，使公共包装器无需额外条件编译。
 */
static int do_proc_int_conv_userhz_jiffies(bool *negp, ulong *u_ptr,
					   int *k_ptr, int dir,
					   const struct ctl_table *tbl)
{
	return -ENOSYS;
}

/*
 * do_proc_int_conv_ms_jiffies() - 无 CONFIG_PROC_SYSCTL 时的毫秒/jiffy 转换桩。
 * 所有指针和 @dir 均未使用；返回 -ENOSYS，不修改 table 数据或用户缓冲区。
 */
static int do_proc_int_conv_ms_jiffies(bool *negp, ulong *u_ptr, int *k_ptr,
				       int dir, const struct ctl_table *tbl)
{
	return -ENOSYS;
}

/*
 * do_proc_int_conv_ms_jiffies_minmax() - 关闭 proc sysctl 时的同名 minmax 转换桩。
 * @negp/@u_ptr/@k_ptr/@tbl 为未使用借用指针，@dir 未使用；固定返回 -ENOSYS。
 */
static int do_proc_int_conv_ms_jiffies_minmax(bool *negp, ulong *u_ptr,
					      int *k_ptr, int dir,
					      const struct ctl_table *tbl)
{
	return -ENOSYS;
}
#endif

/**
 * proc_dointvec_jiffies - read a vector of integers as seconds
 * @table: the sysctl table
 * @dir: %TRUE if this is a write to the sysctl file
 * @buffer: the user buffer
 * @lenp: the size of the user buffer
 * @ppos: file position
 *
 * Reads/writes up to table->maxlen/sizeof(unsigned int) integer
 * values from/to the user buffer, treated as an ASCII string.
 * The values read are assumed to be in seconds, and are converted into
 * jiffies.
 *
 * Returns 0 on success.
 */
/*
 * 以 ASCII 整数向量读写 table->maxlen/sizeof(unsigned int) 个值；用户单位是秒，内核 table->data
 * 单位是 jiffy。读写方向由 dir 决定，成功返回 0；解析、拷贝、溢出或权限相关失败返回下层 errno。
 */
/*
 * proc_dointvec_jiffies() - sysctl 秒值与内核 jiffy int 向量的公共适配器。
 * @table 是借用描述符，@dir 选择读/写，@buffer 是用户指针，@lenp/@ppos 是输入输出长度和文件位置；
 * 不取得这些对象 ownership。可能访问用户内存并睡眠，只能在合适的 sysctl 进程上下文调用。
 */
int proc_dointvec_jiffies(const struct ctl_table *table, int dir,
			  void *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_dointvec_conv(table, dir, buffer, lenp, ppos,
				  do_proc_int_conv_jiffies);
}
EXPORT_SYMBOL(proc_dointvec_jiffies);

/**
 * proc_dointvec_userhz_jiffies - read a vector of integers as 1/USER_HZ seconds
 * @table: the sysctl table
 * @dir: %TRUE if this is a write to the sysctl file
 * @buffer: the user buffer
 * @lenp: the size of the user buffer
 * @ppos: pointer to the file position
 *
 * Reads/writes up to table->maxlen/sizeof(unsigned int) integer
 * values from/to the user buffer, treated as an ASCII string.
 * The values read are assumed to be in 1/USER_HZ seconds, and
 * are converted into jiffies.
 *
 * Returns 0 on success.
 */
/*
 * 读写 ASCII int 向量；用户单位是 1/USER_HZ 秒，内核单位是 jiffy。最多处理 table->maxlen/sizeof
 * (unsigned int) 个值，成功 0，失败传播解析、用户拷贝、转换等 errno。
 */
/*
 * proc_dointvec_userhz_jiffies() - 在 USER_HZ tick 与内核 jiffy int 向量间适配 sysctl。
 * @table/@buffer 为借用描述符和用户缓冲，@dir 决定方向，@lenp/@ppos 会随消费进度更新；可睡眠，
 * 不保存指针或转移 ownership。
 */
int proc_dointvec_userhz_jiffies(const struct ctl_table *table, int dir,
				 void *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_dointvec_conv(table, dir, buffer, lenp, ppos,
				  do_proc_int_conv_userhz_jiffies);
}
EXPORT_SYMBOL(proc_dointvec_userhz_jiffies);

/**
 * proc_dointvec_ms_jiffies - read a vector of integers as 1 milliseconds
 * @table: the sysctl table
 * @dir: %TRUE if this is a write to the sysctl file
 * @buffer: the user buffer
 * @lenp: the size of the user buffer
 * @ppos: the current position in the file
 *
 * Reads/writes up to table->maxlen/sizeof(unsigned int) integer
 * values from/to the user buffer, treated as an ASCII string.
 * The values read are assumed to be in 1/1000 seconds, and
 * are converted into jiffies.
 *
 * Returns 0 on success.
 */
/*
 * 读写 ASCII int 向量；用户单位是毫秒（1/1000 秒），写入时转换为 jiffy，读取时反向转换。
 * 最多处理 table 容量允许的元素；成功 0，失败返回下层 errno。
 */
/*
 * proc_dointvec_ms_jiffies() - 在毫秒与内核 jiffy int 向量间适配 sysctl。
 * @table/@buffer 借用，@dir 选择方向，@lenp/@ppos 为输入输出；用户拷贝路径可能睡眠，不取得引用。
 */
int proc_dointvec_ms_jiffies(const struct ctl_table *table, int dir, void *buffer,
		size_t *lenp, loff_t *ppos)
{
	return proc_dointvec_conv(table, dir, buffer, lenp, ppos,
				  do_proc_int_conv_ms_jiffies);
}
EXPORT_SYMBOL(proc_dointvec_ms_jiffies);

/*
 * proc_dointvec_ms_jiffies_minmax() - 使用独立回调读写毫秒/jiffy int 向量。
 *
 * @table/@buffer 是借用描述符和用户缓冲，@dir 选方向，@lenp/@ppos 是输入输出。返回 0 或下层 errno，
 * 可睡眠。当前独立回调未启用 proc_int_conv() 的范围检查，故 extra1/extra2 并未在此路径生效。
 */
int proc_dointvec_ms_jiffies_minmax(const struct ctl_table *table, int dir,
			  void *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_dointvec_conv(table, dir, buffer, lenp, ppos,
				  do_proc_int_conv_ms_jiffies_minmax);
}

/**
 * proc_doulongvec_ms_jiffies_minmax - read a vector of millisecond values with min/max values
 * @table: the sysctl table
 * @dir: %TRUE if this is a write to the sysctl file
 * @buffer: the user buffer
 * @lenp: the size of the user buffer
 * @ppos: file position
 *
 * Reads/writes up to table->maxlen/sizeof(unsigned long) unsigned long
 * values from/to the user buffer, treated as an ASCII string. The values
 * are treated as milliseconds, and converted to jiffies when they are stored.
 *
 * This routine will ensure the values are within the range specified by
 * table->extra1 (min) and table->extra2 (max).
 *
 * Returns 0 on success.
 */
/*
 * 读写 ASCII unsigned long 向量，用户值按毫秒解释，保存时换成 jiffy；extra1/extra2 给出允许的最小/
 * 最大值。成功返回 0，解析、范围、用户拷贝失败返回下层 errno。
 */
/*
 * proc_doulongvec_ms_jiffies_minmax() - 带范围约束的毫秒/内核 jiffy ulong sysctl 适配器。
 * @table/@buffer 借用，@dir 选读写，@lenp/@ppos 为输入输出；可睡眠。HZ/1000 作为转换比例传给
 * 通用 minmax helper，范围与转换顺序由该 helper 的契约决定。
 */
int proc_doulongvec_ms_jiffies_minmax(const struct ctl_table *table, int dir,
				      void *buffer, size_t *lenp, loff_t *ppos)
{
	return proc_doulongvec_minmax_conv(table, dir, buffer, lenp, ppos,
					   HZ, 1000l);
}
EXPORT_SYMBOL(proc_doulongvec_ms_jiffies_minmax);

/* 文件边界：其余常用 jiffy 换算以内联函数/宏位于 include/linux/jiffies.h，不在此重复定义。 */
