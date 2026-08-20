// SPDX-License-Identifier: GPL-2.0
/*
 * Emulate a local clock event device via a pseudo clock device.
 */
/*
 * 在缺少可跨 CPU deep-idle 唤醒的硬件共享源时，本文件把一个全局 pinned hard hrtimer 包装成
 * CLOCK_EVT_FEAT_HRTIMER|ONESHOT 的伪 clockevent。broadcast core 仍按普通设备的 next_event/event_handler
 * 协议操作它；实际到期由 hrtimer callback 回调共享广播 handler。bctimer 与伪设备均静态常驻，所有启动/
 * shutdown 来自持 tick_broadcast_lock 的路径，避免并发改 base/bound_on，但不能同步等待正在执行的 callback。
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/clockchips.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/module.h>

#include "tick-internal.h"

static struct hrtimer bctimer;
/* 唯一全局 hrtimer 可在各调用 CPU间重新绑定；ce_broadcast_hrtimer.bound_on 镜像其当前 cpu_base。 */

/*
 * bc_shutdown() - 伪 clockevent 的非阻塞 shutdown 回调。
 * 在 broadcast lock 内只能 try_to_cancel：timer 未运行则摘除，callback 正在运行时返回值被忽略并允许其收尾；
 * 对 core 始终返回 0。若改用同步 cancel，会与持锁者等待 callback、callback 等 broadcast lock 形成活锁/死锁。
 */
static int bc_shutdown(struct clock_event_device *evt)
{
	/*
	 * Note, we cannot cancel the timer here as we might
	 * run into the following live lock scenario:
	 *
	 * cpu 0		cpu1
	 * lock(broadcast_lock);
	 *			hrtimer_interrupt()
	 *			bc_handler()
	 *			   tick_handle_oneshot_broadcast();
	 *			    lock(broadcast_lock);
	 * hrtimer_cancel()
	 *  wait_for_callback()
	 */
	/* CPU0 持 broadcast lock 等 callback，而 CPU1 callback 等同一锁，故绝不能在此同步等待。 */
	hrtimer_try_to_cancel(&bctimer);
	return 0;
}

/*
 * This is called from the guts of the broadcast code when the cpu
 * which is about to enter idle has the earliest broadcast timer event.
 */
/*
 * bc_set_next() - 把共享伪设备编到 monotonic 绝对 @expires，并发布实际绑定 CPU。
 * 调用者持 broadcast lock，来自 idle enter/exit 或 broadcast handler；以 ABS_PINNED_HARD 在调用 CPU启动。
 * callback 正运行时 hrtimer_start 不迁移 base，随后读取 bctimer.base 得到真实 owner 写 @bc->bound_on。所有可能
 * 改 base 的本层调用受同一锁串行，故无需再取 hrtimer base lock。始终返回 0，不传播 start 状态。
 */
static int bc_set_next(ktime_t expires, struct clock_event_device *bc)
{
	/*
	 * This is called either from enter/exit idle code or from the
	 * broadcast handler. In all cases tick_broadcast_lock is held.
	 *
	 * hrtimer_cancel() cannot be called here neither from the
	 * broadcast handler nor from the enter/exit idle code. The idle
	 * code can run into the problem described in bc_shutdown() and the
	 * broadcast handler cannot wait for itself to complete for obvious
	 * reasons.
	 *
	 * Each caller tries to arm the hrtimer on its own CPU, but if the
	 * hrtimer callback function is currently running, then
	 * hrtimer_start() cannot move it and the timer stays on the CPU on
	 * which it is assigned at the moment.
	 */
	hrtimer_start(&bctimer, expires, HRTIMER_MODE_ABS_PINNED_HARD);
	/*
	 * The core tick broadcast mode expects bc->bound_on to be set
	 * correctly to prevent a CPU which has the broadcast hrtimer
	 * armed from going deep idle.
	 *
	 * As tick_broadcast_lock is held, nothing can change the cpu
	 * base which was just established in hrtimer_start() above. So
	 * the below access is safe even without holding the hrtimer
	 * base lock.
	 */
	bc->bound_on = bctimer.base->cpu_base->cpu;

	return 0;
}

static struct clock_event_device ce_broadcast_hrtimer = {
	.name			= "bc_hrtimer",
	.set_state_shutdown	= bc_shutdown,
	.set_next_ktime		= bc_set_next,
	.features		= CLOCK_EVT_FEAT_ONESHOT |
				  CLOCK_EVT_FEAT_HRTIMER,
	.rating			= 0,
	.bound_on		= -1,
	.min_delta_ns		= 1,
	.max_delta_ns		= KTIME_MAX,
	.min_delta_ticks	= 1,
	.max_delta_ticks	= ULONG_MAX,
	.mult			= 1,
	.shift			= 0,
	.cpumask		= cpu_possible_mask,
};
/*
 * 伪设备只支持 oneshot ktime 编程，rating=0 使真实合格硬件优先；1ns..KTIME_MAX 范围和 mult=1/shift=0 表示
 * core 可直接交绝对 ktime，不做真实硬件 cycle 换算。cpumask 覆盖 possible CPU，bound_on 初始 -1 后随 start 更新。
 */

/*
 * bc_handler() - bctimer hard callback，把到期转交当前伪 clockevent 的 broadcast event_handler。
 * handler 由 broadcast 安装路径设置，执行时会取 broadcast lock 并分发 CPU；返回 NORESTART，下一共享期限必须
 * 由 handler/core 显式调用 set_next 再启动，避免旧周期自动重装。
 */
static enum hrtimer_restart bc_handler(struct hrtimer *t)
{
	ce_broadcast_hrtimer.event_handler(&ce_broadcast_hrtimer);

	return HRTIMER_NORESTART;
}

/*
 * tick_setup_hrtimer_broadcast() - 启动期初始化全局 hard monotonic hrtimer 并注册伪 clockevent。
 * hrtimer 非 pinned setup，具体 base 在每次 ABS_PINNED start 选择；register 后 broadcast core 可择优安装并取得
 * 静态设备引用。无返回值，注册失败/拒绝由 clockevents 注册流程处理，本函数不分配或回滚对象。
 */
void tick_setup_hrtimer_broadcast(void)
{
	hrtimer_setup(&bctimer, bc_handler, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_HARD);
	clockevents_register_device(&ce_broadcast_hrtimer);
}
