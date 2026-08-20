// SPDX-License-Identifier: GPL-2.0+
/*
 * Unit test for the clocksource watchdog.
 *
 * Copyright (C) 2021 Facebook, Inc.
 * Copyright (C) 2026 Intel Corp.
 *
 * Author: Paul E. McKenney <paulmck@kernel.org>
 * Author: Thomas Gleixner <tglx@kernel.org>
 */
/*
 * 本文件是 clocksource watchdog 的内核线程自测：注册一个以 raw-fast 纳秒为 1GHz cycle 的伪
 * clocksource，再通过读回调注入长延迟、交替正/负频偏或远端 CPU 偏移，观察核心是否授予
 * VALID_FOR_HRES、保持待验证或标记 UNSTABLE。它不进入生产 clocksource 选择，也不是 KUnit 用例；
 * 测试随内建 initcall 或模块加载启动，模块形态保留线程直到卸载以稳定代码和静态对象生命周期。
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kthread.h>

#include "tick-internal.h"
#include "timekeeping_internal.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Clocksource watchdog unit test");
MODULE_AUTHOR("Paul E. McKenney <paulmck@kernel.org>");
MODULE_AUTHOR("Thomas Gleixner <tglx@kernel.org>");

/*
 * wdtest_states 选择读回调的故障模型：NONE 返回真实 raw-fast 时间；DELAY 延迟一次读取；POSITIVE/
 * NEGATIVE 在读数上加/减人工偏移。WDTEST_INJECT_PERCPU 是与低位模型正交的模式位，值高于全部基础
 * 状态，使简单大小比较能区分全局频偏与跨 CPU 偏斜测试；枚举只描述静态协议，不拥有资源。
 */
enum wdtest_states {
	WDTEST_INJECT_NONE,
	WDTEST_INJECT_DELAY,
	WDTEST_INJECT_POSITIVE,
	WDTEST_INJECT_NEGATIVE,
	WDTEST_INJECT_PERCPU	= 0x100,
};

/*
 * 测试全局状态只在伪源注销后由 wdtest 线程重置，在重新注册期间供 watchdog 的本地/远端 read 回调读取。
 * wdtest_state 固定本轮模型；wdtest_test_count 记录跨过的监督间隔并由控制线程 READ_ONCE 轮询；
 * wdtest_last_ts/offset 分别保存上个计数点和本步真实间隔。per-CPU 乒乓通过核心 seq 串行读序，普通模式
 * 核心跳过远端检查；注销/注册边界阻止跨测试重置与活动 read 并发。
 */
static enum wdtest_states wdtest_state;
static unsigned long wdtest_test_count;
static ktime_t wdtest_last_ts, wdtest_offset;

/* 右移 8 位取本步间隔约 1/256，即约 3906ppm，明显超过 calibrated clocksource 的 500ppm 门限。 */
#define SHIFT_4000PPM	8

/*
 * wdtest_get_offset() - 为当前读数计算本轮应注入的纳秒偏移。
 * @cs 是已注册测试 clocksource 的借用指针，生命周期由 watchdog_list 和本轮注册状态保证；函数只读其
 * wd_cpu。普通模式按 test_count 奇偶在 0 与 offset/256 间交替，使相邻 watchdog delta 产生约 4000ppm
 * 跳变；per-CPU 模式在核心记录的控制 CPU 返回 0，在被调度的远端 CPU 返回 1ms，专门制造跨 CPU 偏斜。
 * 可在关中断的本地或远端 watchdog 回调中调用，不睡眠、不改状态；返回有符号 ktime_t 纳秒偏移，无
 * errno 或 ownership 变化。调用者决定把它加到还是减出真实时间。
 */
static ktime_t wdtest_get_offset(struct clocksource *cs)
{
	if (wdtest_state < WDTEST_INJECT_PERCPU)
		return wdtest_test_count & 0x1 ? 0 : wdtest_offset >> SHIFT_4000PPM;

	/* Only affect the readout of the "remote" CPU */
	/* 只改变“远端”CPU 的读数；控制 CPU 号由核心发起跨 CPU 检查前写入 cs->wd_cpu。 */
	return cs->wd_cpu == smp_processor_id() ? 0 : NSEC_PER_MSEC;
}

/*
 * wdtest_ktime_read() - 实现伪 1GHz clocksource 的 read 回调并按状态注入故障。
 * @cs 是 watchdog 持有的测试对象借用指针；回调运行在 timer/CSD 原子上下文，可能位于不同 CPU，不能
 * 睡眠（DELAY 使用忙等）。now 是当前 raw-fast 纳秒/cycle，intv 是距上次计数点的真实间隔。超过 250ms
 * 才把 test_count 加一并提交 last_ts/offset，使一次约 500ms watchdog 扫描即使因参考读超时重试，也只
 * 推进一步故障序列；WRITE_ONCE/READ_ONCE 让线程轮询看到单次标量提交，跨 CPU 读序由核心乒乓协议安排。
 *
 * NONE 返回 now；POSITIVE/NEGATIVE 加减 helper 偏移；DELAY 忙等 500us 后仍返回进入回调时的 now，令
 * watchdog 的参考钟包夹窗口超过 50us 而走可重试 timeout。返回值是 64 位 cycle，无失败码。全局状态
 * 仅在对象注销后重置，回调不取得引用、不分配资源。
 */
static u64 wdtest_ktime_read(struct clocksource *cs)
{
	ktime_t now = ktime_get_raw_fast_ns();
	ktime_t intv = now - wdtest_last_ts;

	/*
	 * Only increment the test counter once per watchdog interval and
	 * store the interval for the offset calculation of this step. This
	 * guarantees a consistent behaviour even if the other side needs
	 * to repeat due to a watchdog read timeout.
	 */
	/*
	 * 每个 watchdog 间隔只推进一次测试计数，并保存本步区间用于偏移计算；即使另一侧因 watchdog
	 * 读取超时而重试，故障注入序列也保持一致。250ms 门限低于正常半秒周期、高于同轮重试间隔。
	 */
	if (intv > (NSEC_PER_SEC / 4)) {
		WRITE_ONCE(wdtest_test_count, wdtest_test_count + 1);
		wdtest_last_ts = now;
		wdtest_offset = intv;
	}

	switch (wdtest_state & ~WDTEST_INJECT_PERCPU) {
	case WDTEST_INJECT_POSITIVE:
		return now + wdtest_get_offset(cs);
	case WDTEST_INJECT_NEGATIVE:
		return now - wdtest_get_offset(cs);
	case WDTEST_INJECT_DELAY:
		udelay(500);
		return now;
	default:
		return now;
	}
}

/*
 * 测试源声明连续、已校准且必须由 watchdog 验证；WDTEST 使选择器永远不把它装为系统主源，并让核心
 * 跳过普通测试的跨 CPU 机制。per-CPU 子测试会在重置时另加 WDTEST_PERCPU，改走跨 CPU 专项检查。
 */
#define KTIME_FLAGS (CLOCK_SOURCE_IS_CONTINUOUS |	\
		     CLOCK_SOURCE_CALIBRATED |		\
		     CLOCK_SOURCE_MUST_VERIFY |		\
		     CLOCK_SOURCE_WDTEST)

/*
 * 静态测试 clocksource 以 raw-fast 纳秒直接充当 cycle，故 64 位 mask、注册频率 1GHz，初始低 rating
 * 只用于监督表排序。list 在每轮 unregister/register 间复用；core 仅借用对象，模块线程退出前必须注销。
 */
static struct clocksource clocksource_wdtest_ktime = {
	.name			= "wdtest-ktime",
	.rating			= 10,
	.read			= wdtest_ktime_read,
	.mask			= CLOCKSOURCE_MASK(64),
	.flags			= KTIME_FLAGS,
	.list			= LIST_HEAD_INIT(clocksource_wdtest_ktime.list),
};

/*
 * wdtest_clocksource_reset() - 结束上一子测试、重建故障状态并重新发布测试 clocksource。
 * @which 是低位基础注入状态；@percpu 决定同时设置测试状态高位和 CLOCK_SOURCE_WDTEST_PERCPU 标志。
 * 由唯一 wdtest kthread 在可睡眠上下文调用，入口不持 core 锁。先 unregister，确保 watchdog 不再执行
 * read 回调，随后写全局状态、清 test_count/last_ts，并恢复上轮可能被 core 改成 0 的 rating 和已变化
 * flags；offset 无需显式清零，因为 last_ts=0 使注册后的首次有效读覆盖它。最后按 1000000kHz 注册，
 * 与 read 直接返回纳秒一致为 1GHz。
 *
 * 函数无返回值，当前 core 注册固定成功；测试源因 WDTEST 不会成为主源且 MUST_VERIFY 不会成为参考源，
 * 正常注销可完成。若这些核心契约改变而注销失败，本函数没有错误处理，随后改静态对象会破坏生命周期，
 * 这是自测对 clocksource core 的前置依赖而非可恢复路径。
 */
static void wdtest_clocksource_reset(enum wdtest_states which, bool percpu)
{
	clocksource_unregister(&clocksource_wdtest_ktime);

	pr_info("Test: State %d percpu %d\n", which, percpu);

	wdtest_state = which;
	if (percpu)
		wdtest_state |= WDTEST_INJECT_PERCPU;
	wdtest_test_count = 0;
	wdtest_last_ts = 0;

	clocksource_wdtest_ktime.rating = 10;
	clocksource_wdtest_ktime.flags = KTIME_FLAGS;
	if (percpu)
		clocksource_wdtest_ktime.flags |= CLOCK_SOURCE_WDTEST_PERCPU;
	clocksource_register_khz(&clocksource_wdtest_ktime, 1000 * 1000);
}

/*
 * wdtest_execute() - 运行一个故障场景并等待 watchdog 给出预期终态。
 * @which/@percpu 选择 reset 模式；@expect 是期望出现的单个 clocksource flag，当前为 VALID_FOR_HRES、
 * UNSTABLE 或 0；@calls 是允许 test_count 推进的 watchdog 间隔数。参数均按值，不保存。
 *
 * reset 后每 100ms 以 READ_ONCE 轮询 test_count 和 flags。收到 kthread stop 立即返回 false；若先看到
 * UNSTABLE/HRES，则只有该位也在 expect 中才成功，否则记录 unexpected 并失败。达到 calls 仍无终态时，
 * expect=0 表示 DELAY 场景正确地只触发可重试读超时，返回 true；非零期望则打印 timed out 并返回
 * false。该“超时”按 watchdog 调用数而非墙钟计数：若系统没有可用参考源而测试 read 从未执行，循环
 * 只能由 stop 请求终止。函数可睡眠，不注销对象；调用者负责下一轮 reset 或最终 cleanup。
 */
static bool wdtest_execute(enum wdtest_states which, bool percpu, unsigned int expect,
			   unsigned long calls)
{
	wdtest_clocksource_reset(which, percpu);

	for (; READ_ONCE(wdtest_test_count) < calls; msleep(100)) {
		unsigned int flags = READ_ONCE(clocksource_wdtest_ktime.flags);

		if (kthread_should_stop())
			return false;

		if (flags & CLOCK_SOURCE_UNSTABLE) {
			if (expect & CLOCK_SOURCE_UNSTABLE)
				return true;
			pr_warn("Fail: Unexpected unstable\n");
			return false;
		}
		if (flags & CLOCK_SOURCE_VALID_FOR_HRES) {
			if (expect & CLOCK_SOURCE_VALID_FOR_HRES)
				return true;
			pr_warn("Fail: Unexpected valid for highres\n");
			return false;
		}
	}

	if (!expect)
		return true;

	pr_warn("Fail: Timed out\n");
	return false;
}

/*
 * wdtest_run() - 按固定顺序执行一套普通或 per-CPU watchdog 场景。
 * @percpu=false 时先验证无注入源经过约 8 轮取得 HRES，500us 读延迟在 4 轮内既不 HRES 也不 unstable，
 * 再验证交替加/减约 4000ppm 都被标 unstable。@percpu=true 时 NONE 证明跨 CPU 一致读可通过，DELAY
 * 验证交接 timeout 可重试，POSITIVE/NEGATIVE 则让远端分别领先/落后 1ms，验证跨 CPU skew 降级。
 * 运行在测试 kthread，可睡眠；任一子测试失败或 stop 都短路返回 false，四项全过返回 true。当前注册
 * 对象留给下一子测试 reset，最后一项则由 wdtest_func 注销；无资源 ownership 转移。
 */
static bool wdtest_run(bool percpu)
{
	if (!wdtest_execute(WDTEST_INJECT_NONE, percpu, CLOCK_SOURCE_VALID_FOR_HRES, 8))
		return false;

	if (!wdtest_execute(WDTEST_INJECT_DELAY, percpu, 0, 4))
		return false;

	if (!wdtest_execute(WDTEST_INJECT_POSITIVE, percpu, CLOCK_SOURCE_UNSTABLE, 8))
		return false;

	if (!wdtest_execute(WDTEST_INJECT_NEGATIVE, percpu, CLOCK_SOURCE_UNSTABLE, 8))
		return false;

	return true;
}

/*
 * wdtest_func() - 测试 kthread 主函数，串接普通/per-CPU 套件并管理测试源的最终注销。
 * @arg 是 kthread_run 传入的 NULL 占位，不读取、不释放。先注册一次，使首个 execute/reset 的 unregister
 * 有合法对象可摘；普通套件全过才执行 per-CPU 套件，二者全过才打印总成功。任一路失败或 stop 都短路，
 * 随后无条件注销当前测试源，保证 core 不再借用模块静态对象和 read 回调。
 *
 * 内建配置在测试后返回 0 并结束线程；模块配置即使测试失败也以一小时可中断睡眠保持线程存活，直到
 * module_exit 的 kthread_stop 发布请求并唤醒它。这样卸载前可同步确认不再执行模块代码。函数所有测试
 * 结果只通过日志报告，固定返回 0，故模块加载成功不代表测试通过。per-CPU 套件需要至少一个在线远端
 * CPU；否则核心不会调用测试 read，计数循环会持续到 stop。
 */
static int wdtest_func(void *arg)
{
	clocksource_register_khz(&clocksource_wdtest_ktime, 1000 * 1000);
	if (wdtest_run(false)) {
		if (wdtest_run(true))
			pr_info("Success: All tests passed\n");
	}
	clocksource_unregister(&clocksource_wdtest_ktime);

	if (!IS_MODULE(CONFIG_TEST_CLOCKSOURCE_WATCHDOG))
		return 0;

	while (!kthread_should_stop())
		schedule_timeout_interruptible(3600 * HZ);
	return 0;
}

/*
 * wdtest_thread 保存模块创建的 kthread 借用指针；初始化成功后由模块生命周期稳定，cleanup 以
 * kthread_stop 同步消费其运行期，但不拥有/手工释放 task_struct。NULL 表示尚未成功创建。
 */
static struct task_struct *wdtest_thread;

/*
 * clocksource_wdtest_init() - 创建并立即唤醒名为 wdtest 的测试线程。
 * 入参：无；作为 module_init 在可睡眠加载/启动上下文执行。t 是 kthread_run 返回的临时结果：错误指针时
 * 打印告警并原样返回 -ENOMEM/-EINTR 等 errno，模块不发布且 cleanup 不运行；成功时把 task 指针提交到
 * 静态 wdtest_thread 并返回 0。线程可能在赋值前已经开始执行，但模块卸载只能在 init 返回后发生；模块
 * 形态的线程在测试后不会提前退出，因此 cleanup 使用指针时生命周期有效。函数不等待测试结果。
 */
static int __init clocksource_wdtest_init(void)
{
	struct task_struct *t = kthread_run(wdtest_func, NULL, "wdtest");

	if (IS_ERR(t)) {
		pr_warn("Failed to create wdtest kthread.\n");
		return PTR_ERR(t);
	}
	wdtest_thread = t;
	return 0;
}
/* 把创建入口接入内建 initcall 或模块加载路径；成功只表示线程已创建，不表示自测已通过。 */
module_init(clocksource_wdtest_init);

/*
 * clocksource_wdtest_cleanup() - 模块卸载时请求测试线程停止并同步等待退出。
 * 入参：无；运行在可睡眠 module_exit 上下文，加载/卸载串行保证已看到 init 提交。线程指针非 NULL 时
 * kthread_stop 会唤醒 100ms 轮询或一小时 interruptible 睡眠；若测试仍在进行，execute 观察 stop 后先由
 * wdtest_func 注销 clocksource，再返回。忽略线程固定的 0 返回值，无额外资源需释放。内建配置没有运行期
 * 卸载，因而其已自然退出线程不会进入本清理函数。
 */
static void clocksource_wdtest_cleanup(void)
{
	if (wdtest_thread)
		kthread_stop(wdtest_thread);
}
/* 模块形态登记卸载回调，使代码和静态测试源在 kthread 完全退出后才可回收。 */
module_exit(clocksource_wdtest_cleanup);
