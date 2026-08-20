// SPDX-License-Identifier: GPL-2.0
/*
 * Generic sched_clock() support, to extend low level hardware time
 * counters to full 64-bit ns values.
 */
/*
 * 本文件把可能位宽很窄、会回绕的硬件调度计数器扩展成高频、近似单调的 64 位纳秒 sched_clock。
 * 读路径可出现在 NMI、trace 和 noinstr 上下文，不能取普通锁；写侧用 seqcount_latch 双副本发布换算代，
 * 硬 hrtimer 在回绕前推进 epoch。未注册硬件源时以 jiffies 兜底，并在 syscore suspend 期间冻结读数。
 */
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/syscore_ops.h>
#include <linux/hrtimer.h>
#include <linux/sched_clock.h>
#include <linux/seqlock.h>
#include <linux/bitops.h>

#include "timekeeping.h"

/**
 * struct clock_data - all data needed for sched_clock() (including
 *                     registration of a new clock source)
 *
 * @seq:		Sequence counter for protecting updates. The lowest
 *			bit is the index for @read_data.
 * @read_data:		Data required to read from sched_clock.
 * @wrap_kt:		Duration for which clock can run before wrapping.
 * @rate:		Tick rate of the registered clock.
 * @actual_read_sched_clock: Registered hardware level clock read function.
 *
 * The ordering of this structure has been chosen to optimize cache
 * performance. In particular 'seq' and 'read_data[0]' (combined) should fit
 * into a single 64-byte cache line.
 */
/*
 * clock_data 汇总全局 sched_clock 状态：@seq 的最低位选择 @read_data[2]；每份读数据含回调、mask、
 * mult/shift 和 epoch cycle/ns。@wrap_kt 是必须刷新 epoch 的最长间隔，@rate 用于拒绝更慢的后注册源，
 * @actual_read_sched_clock 永远保存真实硬件回调，即使 suspend 暂时把公开读回调替换为冻结函数。
 * 字段顺序让 seq+偶副本尽量落在一个 64 字节 cache line，优化每次调度/trace 都会经过的热读路径。
 * 全局对象静态存续；写侧在关中断或 syscore 单 CPU 阶段更新，读侧只借用 latch 选中的副本。
 */
struct clock_data {
	seqcount_latch_t	seq;
	struct clock_read_data	read_data[2];
	ktime_t			wrap_kt;
	unsigned long		rate;

	u64 (*actual_read_sched_clock)(void);
};

/*
 * sched_clock_timer 是 CLOCK_MONOTONIC 硬 hrtimer，周期推进 epoch 防止窄 counter 多次回绕；irqtime=-1 表示
 * 按源频率自动启用 IRQ 时间记账，0/正值分别强制关闭/开启。core_param 以只读 0400 暴露启动参数结果。
 */
static struct hrtimer sched_clock_timer;
static int irqtime = -1;

core_param(irqtime, irqtime, int, 0400);

/*
 * jiffy_sched_clock_read() - 提供未注册硬件 sched_clock 时的最低精度 cycle 源。
 * 入参：无；可从 NMI/notrace 热路径调用，不取锁、不睡眠。返回无符号 long 宽度的
 * jiffies-INITIAL_JIFFIES，再提升为 u64；注册时 bits=BITS_PER_LONG，所以 32 位平台只需本机宽度原子读，
 * 不必使用 get_jiffies_64。回绕由通用 mask/epoch 机制扩展，无 errno 或副作用。
 */
static u64 notrace jiffy_sched_clock_read(void)
{
	/*
	 * We don't need to use get_jiffies_64 on 32-bit arches here
	 * because we register with BITS_PER_LONG
	 */
	/* 注册位宽与 unsigned long 相同，单次本机 jiffies 读足以满足原子性，通用层负责扩展回绕。 */
	return (u64)(jiffies - INITIAL_JIFFIES);
}

/*
 * cd 初始偶副本把一个 jiffy 乘以 NSEC_PER_SEC/HZ，真实回调也指向 jiffies；其余字段为 0，后续
 * generic init 会正式注册并补齐 mask/epoch/wrap。cache-line 对齐减少热读与相邻全局数据的伪共享。
 */
static struct clock_data cd ____cacheline_aligned = {
	.read_data[0] = { .mult = NSEC_PER_SEC / HZ,
			  .read_sched_clock = jiffy_sched_clock_read, },
	.actual_read_sched_clock = jiffy_sched_clock_read,
};

/*
 * cyc_to_ns() - 用给定定点比例执行 (cyc×mult)>>shift。
 * 三个参数均按值；调用者保证乘积在 u64 安全范围内，纯算术、始终内联、不睡眠。返回向下取整纳秒，无
 * 错误码或状态副作用；窄 counter 的回绕模差必须在调用前完成。
 */
static __always_inline u64 cyc_to_ns(u64 cyc, u32 mult, u32 shift)
{
	return (cyc * mult) >> shift;
}

/*
 * sched_clock_read_begin() - 为外部 latch 读者取得一致代序号和对应 clock_read_data。
 * @seq 是调用者持有的非 NULL 输出指针；函数写入 read_seqcount_latch 返回值，并按最低位返回 cd 双副本的
 * 借用地址。notrace、无锁、不睡眠；调用者读取所需字段后必须用 sched_clock_read_retry(*seq) 验证，失败
 * 就丢弃全部结果重试。返回指针不增加引用，静态终身有效但内容只在该 latch 临界读内自洽。
 */
notrace struct clock_read_data *sched_clock_read_begin(unsigned int *seq)
{
	*seq = read_seqcount_latch(&cd.seq);
	return cd.read_data + (*seq & 1);
}

/*
 * sched_clock_read_retry() - 检查从 begin 取得的 @seq 期间写侧是否切换过 latch。
 * @seq 按值输入；返回非零表示副本可能不自洽，调用者必须完整重读，0 表示本轮字段可用。可在 NMI/notrace
 * 上下文调用，无锁、不睡眠、无副作用或 ownership 变化。
 */
notrace int sched_clock_read_retry(unsigned int seq)
{
	return read_seqcount_latch_retry(&cd.seq, seq);
}

/*
 * __sched_clock() - 从 latch 选中的一代参数计算当前扩展纳秒时间。
 * 入参：无；始终内联且使用未插桩 raw seqcount helper，适用于 NMI/noinstr 热路径。rd 是当前副本借用指针，
 * seq 保存代次；cyc 为 (read()-epoch_cyc)&mask 的单圈模差，res=epoch_ns+定点换算。写侧若在期间切换
 * latch，retry 使整组回调/mask/比例/epoch 重新读取，绝不混用两代字段。返回 64 位 ns，无 errno、锁、
 * 睡眠或状态修改；单次 epoch 刷新间隔必须短于 counter 可安全回绕范围。
 */
static __always_inline unsigned long long __sched_clock(void)
{
	struct clock_read_data *rd;
	unsigned int seq;
	u64 cyc, res;

	do {
		seq = raw_read_seqcount_latch(&cd.seq);
		rd = cd.read_data + (seq & 1);

		cyc = (rd->read_sched_clock() - rd->epoch_cyc) &
		      rd->sched_clock_mask;
		res = rd->epoch_ns + cyc_to_ns(cyc, rd->mult, rd->shift);
	} while (raw_read_seqcount_latch_retry(&cd.seq, seq));

	return res;
}

/*
 * sched_clock_noinstr() - 为明确禁止插桩的调用者提供最薄 sched_clock 包装。
 * 入参：无；直接返回 __sched_clock 的纳秒值，不额外关闭抢占或建立 KCSAN 区域，调用者负责其 noinstr/
 * CPU 稳定语境。无锁、不睡眠、无副作用。
 */
unsigned long long noinstr sched_clock_noinstr(void)
{
	return __sched_clock();
}

/*
 * sched_clock() - 通用 notrace 调度时钟入口。
 * 入参：无；先以 notrace 方式关闭抢占，保证本次热读不迁移，并用 KCSAN nestable atomic 区把 raw latch
 * reader 的所有字段访问声明为原子协议，避免未插桩 helper 造成误报。随后调用 __sched_clock，按逆序结束
 * KCSAN 区并恢复抢占，返回 64 位 ns。可从广泛的调度/trace/中断上下文调用，不睡眠、不改时钟状态。
 */
unsigned long long notrace sched_clock(void)
{
	unsigned long long ns;
	preempt_disable_notrace();
	/*
	 * All of __sched_clock() is a seqcount_latch reader critical section,
	 * but relies on the raw helpers which are uninstrumented. For KCSAN,
	 * mark all accesses in __sched_clock() as atomic.
	 */
	/* raw latch helper 本身不向 KCSAN 描述同步；显式原子区覆盖整次多字段读并允许嵌套调用。 */
	kcsan_nestable_atomic_begin();
	ns = __sched_clock();
	kcsan_nestable_atomic_end();
	preempt_enable_notrace();
	return ns;
}

/*
 * Updating the data required to read the clock.
 *
 * sched_clock() will never observe mis-matched data even if called from
 * an NMI. We do this by maintaining an odd/even copy of the data and
 * steering sched_clock() to one or the other using a sequence counter.
 * In order to preserve the data cache profile of sched_clock() as much
 * as possible the system reverts back to the even copy when the update
 * completes; the odd copy is used *only* during an update.
 */
/*
 * update_clock_read_data() - 用 seqcount_latch 把完整的新读参数原子发布到偶/奇双副本。
 * @rd 是写者栈上或稳定存储中的完整输入快照，只在调用期间借用。写者必须由关中断、timer 或 syscore
 * 协议彼此串行；函数本身不提供多写者锁，且不可睡眠。begin 先把读者导向仍旧自洽的奇副本，再更新正常
 * 偶副本；中间 latch 把新读者切回偶副本，再更新备用奇副本；end 完成发布屏障。无返回值，退出时两份
 * 数据相同且 seq 回到偶副本，NMI 读者最多重试而不会看到混代字段。
 */
static void update_clock_read_data(struct clock_read_data *rd)
{
	/* steer readers towards the odd copy */
	/* 阶段 1：奇副本仍保存旧完整代，先把新读者导向它。 */
	write_seqcount_latch_begin(&cd.seq);

	/* now its safe for us to update the normal (even) copy */
	/* 阶段 2：没有新读者选择偶副本后，整结构覆盖为新代。 */
	cd.read_data[0] = *rd;

	/* switch readers back to the even copy */
	/* 阶段 3：新偶副本完整后切回热路径的 cache-friendly 常态。 */
	write_seqcount_latch(&cd.seq);

	/* update the backup (odd) copy with the new data */
	/* 阶段 4：现有读者已不再进入奇副本，补齐备用副本供下一次更新使用。 */
	cd.read_data[1] = *rd;

	write_seqcount_latch_end(&cd.seq);
}

/*
 * Atomically update the sched_clock() epoch.
 */
/*
 * update_sched_clock() - 读取真实 counter，把当前累积时间折叠为新的 epoch 并发布。
 * 入参：无；由硬 hrtimer、注册或 suspend 路径在写者串行语境调用。rd 复制当前偶副本，cyc 是真实硬件
 * 当前值，ns 用旧 epoch/mask/mult/shift 结算单圈模差；随后 epoch_ns=ns、epoch_cyc=cyc，再通过 latch
 * 发布。返回无直接值；保持 sched_clock 连续并重新获得一个完整 wrap 窗口。actual read 必须原子且不可
 * 睡眠；若两次更新间已多绕一圈，mask 模差无法恢复丢失时间。
 */
static void update_sched_clock(void)
{
	u64 cyc;
	u64 ns;
	struct clock_read_data rd;

	rd = cd.read_data[0];

	cyc = cd.actual_read_sched_clock();
	ns = rd.epoch_ns + cyc_to_ns((cyc - rd.epoch_cyc) & rd.sched_clock_mask, rd.mult, rd.shift);

	rd.epoch_ns = ns;
	rd.epoch_cyc = cyc;

	update_clock_read_data(&rd);
}

/*
 * sched_clock_poll() - 硬 hrtimer 回调，在 counter 安全回绕前推进 epoch 并安排下一轮。
 * @hrt 是静态 sched_clock_timer 借用指针；运行在 hardirq、不可睡眠。先 update 结算真实 counter，再按当前
 * cd.wrap_kt 用 hrtimer_forward_now 跳过已错过周期，返回 HRTIMER_RESTART。若系统延迟超过硬件一整圈，
 * 本回调只能看到 mask 后模差，无法补回多圈，故 wrap_kt 已按 50% 安全裕量计算。
 */
static enum hrtimer_restart sched_clock_poll(struct hrtimer *hrt)
{
	update_sched_clock();
	hrtimer_forward_now(hrt, cd.wrap_kt);

	return HRTIMER_RESTART;
}

/*
 * sched_clock_register() - 选择不慢于当前源的新硬件 counter，并保持已累计 sched_clock 纳秒连续。
 *
 * 【参数与前置】@read 是不可睡眠、可从 NMI/notrace 调用且返回单调模计数的函数指针；@bits 是有效位宽，
 * @rate 是非零 Hz。参数不保存 ownership，但回调代码/硬件必须覆盖内核余生或下一次替换。比当前 cd.rate
 * 更低的源直接忽略；相同频率允许后注册替换。注册通常在启动串行上下文，函数在接受后 irqsave，阻止
 * 本 CPU hrtimer writer 并发；NMI 读者由 latch 保护。
 *
 * 【换算与连续性】以 1 小时安全范围求 new_mult/new_shift，以 bits 建 mask，再由
 * clocks_calc_max_nsecs() 的 50% 裕量求 wrap/wrap_kt。new_epoch 读取新 counter；cyc/ns 用旧真实回调和
 * 旧读参数结算到切换点，随后把 actual 回调和 rd 的回调、mask、比例、新 cycle epoch、连续 ns epoch
 * 成组更新，并通过 latch 发布。bits/rate 非法可能导致 mask/除零，函数不校验。
 *
 * 【后续与副作用】timer 已 setup 时按新 wrap 重新启动；日志将 rate 缩放为 M/k/Hz，并报告单 cycle
 * 分辨率和 wrap ns。irqtime>0 强制启用 IRQ 时间记账，-1 时 rate>=1MHz 自动启用，0 不启用；一旦启用
 * 本函数不负责关闭。恢复 IRQ 后打印回调地址。无返回值，不能报告配置错误；较慢源忽略时无副作用。
 */
void sched_clock_register(u64 (*read)(void), int bits, unsigned long rate)
{
	u64 res, wrap, new_mask, new_epoch, cyc, ns;
	u32 new_mult, new_shift;
	unsigned long r, flags;
	char r_unit;
	struct clock_read_data rd;

	if (cd.rate > rate)
		return;

	/* Cannot register a sched_clock with interrupts on */
	/* 写者必须与本 CPU hard hrtimer 串行，NMI 读者则继续由双副本 latch 服务。 */
	local_irq_save(flags);

	/* Calculate the mult/shift to convert counter ticks to ns. */
	/* 以一小时作为运行期单次换算安全范围，在乘法不溢出的前提下尽量提高精度。 */
	clocks_calc_mult_shift(&new_mult, &new_shift, rate, NSEC_PER_SEC, 3600);

	new_mask = CLOCKSOURCE_MASK(bits);
	cd.rate = rate;

	/* Calculate how many nanosecs until we risk wrapping */
	/* max helper 已含 50% 安全裕量，timer 必须在该窗口内折叠 epoch。 */
	wrap = clocks_calc_max_nsecs(new_mult, new_shift, 0, new_mask, NULL);
	cd.wrap_kt = ns_to_ktime(wrap);

	rd = cd.read_data[0];

	/* Update epoch for new counter and update 'epoch_ns' from old counter*/
	/* 先分别取新源 cycle 基点和旧源连续 ns，发布后读者从同一纳秒值继续而不会因换源跳变。 */
	new_epoch = read();
	cyc = cd.actual_read_sched_clock();
	ns = rd.epoch_ns + cyc_to_ns((cyc - rd.epoch_cyc) & rd.sched_clock_mask, rd.mult, rd.shift);
	cd.actual_read_sched_clock = read;

	rd.read_sched_clock	= read;
	rd.sched_clock_mask	= new_mask;
	rd.mult			= new_mult;
	rd.shift		= new_shift;
	rd.epoch_cyc		= new_epoch;
	rd.epoch_ns		= ns;

	update_clock_read_data(&rd);

	if (ACCESS_PRIVATE(&sched_clock_timer, function) != NULL) {
		/* update timeout for clock wrap */
		/* timer 已初始化说明系统进入运行期；用新 counter 的 wrap 周期覆盖旧到期。 */
		hrtimer_start(&sched_clock_timer, cd.wrap_kt,
			      HRTIMER_MODE_REL_HARD);
	}

	r = rate;
	if (r >= 4000000) {
		r = DIV_ROUND_CLOSEST(r, 1000000);
		r_unit = 'M';
	} else if (r >= 4000) {
		r = DIV_ROUND_CLOSEST(r, 1000);
		r_unit = 'k';
	} else {
		r_unit = ' ';
	}

	/* Calculate the ns resolution of this counter */
	/* 一个 cycle 经新比例向下换算的 ns 值用于诊断，极高频源可能显示 0ns。 */
	res = cyc_to_ns(1ULL, new_mult, new_shift);

	pr_info("sched_clock: %u bits at %lu%cHz, resolution %lluns, wraps every %lluns\n",
		bits, r, r_unit, res, wrap);

	/* Enable IRQ time accounting if we have a fast enough sched_clock() */
	/* 启动参数正值强制开启；自动模式仅信任至少 1MHz 的调度时钟精度。 */
	if (irqtime > 0 || (irqtime == -1 && rate >= 1000000))
		enable_sched_clock_irqtime();

	local_irq_restore(flags);

	pr_debug("Registered %pS as sched_clock source\n", read);
}
EXPORT_SYMBOL_GPL(sched_clock_register);

/*
 * generic_sched_clock_init() - 完成最终源选择、建立 epoch 并启动防回绕 hrtimer。
 * 入参：无；在 __init 启动串行上下文调用。若 actual 回调仍是初始 jiffies，按 BITS_PER_LONG/HZ 正式注册
 * 兜底源；架构已注册硬件时保留它。随后 update_sched_clock 建立当前 epoch，setup 一个 CLOCK_MONOTONIC
 * REL_HARD timer 并以 wrap_kt 启动。无返回值；返回后 timer function 非 NULL，运行期换源会同步更新周期。
 * hrtimer 为静态对象，无释放路径，回调贯穿系统运行期。
 */
void __init generic_sched_clock_init(void)
{
	/*
	 * If no sched_clock() function has been provided at that point,
	 * make it the final one.
	 */
	/* 没有架构源时把早期 jiffies 兜底补齐为完整注册参数；已有源则不降级。 */
	if (cd.actual_read_sched_clock == jiffy_sched_clock_read)
		sched_clock_register(jiffy_sched_clock_read, BITS_PER_LONG, HZ);

	update_sched_clock();

	/*
	 * Start the timer to keep sched_clock() properly updated and
	 * sets the initial epoch.
	 */
	/* hard timer 不依赖调度进程运行，能在窄计数器回绕安全窗口内持续推进 epoch。 */
	hrtimer_setup(&sched_clock_timer, sched_clock_poll, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	hrtimer_start(&sched_clock_timer, cd.wrap_kt, HRTIMER_MODE_REL_HARD);
}

/*
 * Clock read function for use when the clock is suspended.
 *
 * This function makes it appear to sched_clock() as if the clock
 * stopped counting at its last update.
 *
 * This function must only be called from the critical
 * section in sched_clock(). It relies on the read_seqcount_retry()
 * at the end of the critical section to be sure we observe the
 * correct copy of 'epoch_cyc'.
 */
/*
 * suspended_sched_clock_read() - suspend 期间让 sched_clock 看起来停在最后一次 epoch。
 * 入参：无；只允许作为 __sched_clock latch 临界读中 rd->read_sched_clock 回调执行。它再次读取 cd.seq，按
 * 最低位返回对应副本的 epoch_cyc，使外层计算的模差为 0；若内外序号在期间变化，外层 retry 会丢弃结果。
 * notrace、可从 NMI 调用、不睡眠；返回原始 cycle 基点，无状态或 ownership 变化，不能独立在普通上下文
 * 当作当前硬件 counter 使用。
 */
static u64 notrace suspended_sched_clock_read(void)
{
	unsigned int seq = read_seqcount_latch(&cd.seq);

	return cd.read_data[seq & 1].epoch_cyc;
}

/*
 * sched_clock_suspend() - 折叠当前时间、停掉 wrap timer 并把公开读数冻结在 epoch。
 * 入参：无；由 syscore suspend 的冻结/单 CPU 阶段调用，写者已串行。rd 指向常态偶副本。先用真实硬件
 * 更新双副本，使 epoch_ns/cyc 是最终运行点；同步取消 hrtimer，保证回调不再更新；再把偶副本回调换成
 * suspended read。latch 当前稳定选择偶副本，NMI 读者要么完成旧读要么重试到冻结回调。
 * 固定返回 0，无失败/回滚；不修改 actual 回调和 epoch_ns，因此 suspend 时长不会计入 sched_clock。
 */
int sched_clock_suspend(void)
{
	struct clock_read_data *rd = &cd.read_data[0];

	update_sched_clock();
	hrtimer_cancel(&sched_clock_timer);
	rd->read_sched_clock = suspended_sched_clock_read;

	return 0;
}

/*
 * sched_clock_syscore_suspend() - 适配 syscore 回调签名。
 * @data 是注册框架传入的 NULL/私有占位，当前不读取；原样返回 sched_clock_suspend() 的 0。运行上下文、
 * 并发和副作用完全由内层定义，不拥有 data。
 */
static int sched_clock_syscore_suspend(void *data)
{
	return sched_clock_suspend();
}

/*
 * sched_clock_resume() - 以恢复时真实 cycle 重新锚定冻结的 epoch 并恢复正常读回调。
 * 入参：无；由 syscore resume 单 CPU 串行阶段调用。rd 是偶副本；先读取 actual 硬件值覆盖 epoch_cyc，
 * 保持 epoch_ns 不变，从而丢弃 suspend 期间的 counter 增量；再启动 wrap timer，最后把公开回调恢复为
 * actual。冻结回调在恢复提交前仍返回新的 epoch_cyc，故读数保持 epoch_ns。无返回值/错误码，不更新
 * rate/比例或 ownership；后续 timer 再通过 latch 同步双副本。
 */
void sched_clock_resume(void)
{
	struct clock_read_data *rd = &cd.read_data[0];

	rd->epoch_cyc = cd.actual_read_sched_clock();
	hrtimer_start(&sched_clock_timer, cd.wrap_kt, HRTIMER_MODE_REL_HARD);
	rd->read_sched_clock = cd.actual_read_sched_clock;
}

/*
 * sched_clock_syscore_resume() - 适配无返回值 syscore resume 回调。
 * @data 未使用且不拥有；直接调用 sched_clock_resume。运行在系统核心恢复阶段，不睡眠、无额外状态。
 */
static void sched_clock_syscore_resume(void *data)
{
	sched_clock_resume();
}

/*
 * sched_clock_syscore_ops 把上方冻结/恢复包装登记为静态操作表；sched_clock_syscore 再封装 ops 供统一
 * syscore 注册表借用。两者内核全生命周期常驻，无动态释放或模块引用。
 */
static const struct syscore_ops sched_clock_syscore_ops = {
	.suspend	= sched_clock_syscore_suspend,
	.resume		= sched_clock_syscore_resume,
};

static struct syscore sched_clock_syscore = {
	.ops = &sched_clock_syscore_ops,
};

/*
 * sched_clock_syscore_init() - 在 device_initcall 阶段把 sched_clock 加入 syscore suspend/resume 顺序。
 * 入参：无；运行在可睡眠启动初始化上下文。register_syscore 保存静态对象借用指针且无失败返回，本函数
 * 固定返回 0。注册后每次系统 suspend/resume 都会调用包装；本文件没有注销路径，适合内建永久设施。
 */
static int __init sched_clock_syscore_init(void)
{
	register_syscore(&sched_clock_syscore);

	return 0;
}
/* syscore 注册晚于 generic sched_clock 初始化，首次 suspend 前真实回调、epoch 和 timer 已就绪。 */
device_initcall(sched_clock_syscore_init);
