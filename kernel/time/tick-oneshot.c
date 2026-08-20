// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains functions which manage high resolution tick
 * related events.
 *
 * Copyright(C) 2005-2006, Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 */
/*
 * 本文件管理本地 CPU clockevent 的 oneshot 形态，供 hrtimer 高分辨率中断和低分辨率 NO_HZ 共用：
 * `tick_device.mode` 表示 tick 层选择，`clock_event_device.state/next_event` 表示 core/硬件状态。函数只借用
 * per-CPU 或传入设备，不取得引用；调用路径必须保持本 CPU 稳定并关闭本地中断，设备状态转换/期限编程不可睡眠。
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>

#include "tick-internal.h"

/**
 * tick_program_event - program the CPU local timer device for the next event
 * @expires: the time at which the next timer event should occur
 * @force: flag to force reprograming even if the event time hasn't changed
 *
 * Return: 0 on success, negative error code on failure
 */
/*
 * tick_program_event() - 为本 CPU tick device 提交下一 monotonic 绝对期限。
 * @expires=KTIME_MAX 表示暂不再需要中断：切到 ONESHOT_STOPPED 并同步软件 next_event，返回 0；后续有限期限
 * 会先从 stopped 恢复 ONESHOT。普通路径把 @force 传给 clockevents_program_event，返回其 0 或 -ETIME 等
 * 错误；state switch 本身无错误返回。@dev 从本 CPU per-CPU 槽借用且不得为空，调用者负责 IRQ-off/不可迁移。
 * 上方英文参数说明中 force 的“期限未变化也强制”是旧的概括；当前底层主要用它控制最小 delta 失败后的重试。
 */
int tick_program_event(ktime_t expires, int force)
{
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);

	if (unlikely(expires == KTIME_MAX)) {
		/*
		 * We don't need the clock event device any more, stop it.
		 */
		/* 无待处理期限时停止 oneshot 硬件，保留设备归属，next_event 以 KTIME_MAX 发布“未编程”。 */
		clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT_STOPPED);
		dev->next_event = KTIME_MAX;
		return 0;
	}

	if (unlikely(clockevent_state_oneshot_stopped(dev))) {
		/*
		 * We need the clock event again, configure it in ONESHOT mode
		 * before using it.
		 */
		/* stopped 只是临时省电态；有限期限到来先恢复 ONESHOT，再交底层按绝对时间编程。 */
		clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);
	}

	return clockevents_program_event(dev, expires, force);
}

/**
 * tick_resume_oneshot - resume oneshot mode
 */
/*
 * tick_resume_oneshot() - 系统恢复后重新激活本 CPU oneshot 设备。
 * 从 per-CPU tick_device 借用 @dev，切回 ONESHOT，再以当前 monotonic 时间、force=true 编程一个立即事件，
 * 让正常中断路径重算真正 deadline。两个 void/返回值均未检查，恢复路径以尽快触发重评估收敛；要求 IRQ-off。
 */
void tick_resume_oneshot(void)
{
	struct clock_event_device *dev = __this_cpu_read(tick_cpu_device.evtdev);

	clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);
	clockevents_program_event(dev, ktime_get(), true);
}

/**
 * tick_setup_oneshot - setup the event device for oneshot mode (hres or nohz)
 * @newdev: Pointer to the clock event device to configure
 * @handler: Function to be called when the event device triggers an interrupt
 * @next_event: Initial expiry time for the next event (in ktime)
 *
 * Configures the specified clock event device for onshot mode,
 * assigns the given handler as its event callback, and programs
 * the device to trigger at the specified next event time.
 */
/*
 * tick_setup_oneshot() - 在设备安装/替换时建立 oneshot 回调和首个期限。
 * @newdev 生命周期由 clockevents/tick 交换协议稳定；先写 @handler，再切 ONESHOT，最后以 force=true 编程
 * @next_event。无返回值且忽略驱动编程错误，调用者不能从本接口获知首次期限是否提交；不修改
 * tick_device.mode，该模式由上层安装或 switch 路径维护。上方英文说明即为“配置设备、赋回调并编程首次触发”。
 */
void tick_setup_oneshot(struct clock_event_device *newdev,
			void (*handler)(struct clock_event_device *),
			ktime_t next_event)
{
	newdev->event_handler = handler;
	clockevents_switch_state(newdev, CLOCK_EVT_STATE_ONESHOT);
	clockevents_program_event(newdev, next_event, true);
}

/**
 * tick_switch_to_oneshot - switch to oneshot mode
 * @handler: function to call when an event occurs on the tick device
 *
 * Return: 0 on success, -EINVAL if the tick device is not present,
 *         not functional, or does not support oneshot mode.
 */
/*
 * tick_switch_to_oneshot() - 把本 CPU tick 层、设备和广播侧切入 oneshot。
 * 先验证 evtdev 存在、声明 ONESHOT feature 且非 DUMMY；失败按具体原因打印并返回 -EINVAL，不改 mode/handler。
 * 成功则先置 `td->mode=ONESHOT`、安装 @handler、请求设备状态转换，再通知 broadcast 侧切换并返回 0。
 * clockevents_switch_state 是 void，若驱动状态回调失败，本函数仍继续并返回 0；诊断只能结合设备 state/日志。
 * td/dev 均为本 CPU 借用对象，要求 IRQ-off；函数不编程首个 deadline，调用者随后由 hrtimer/NO_HZ 路径完成。
 */
int tick_switch_to_oneshot(void (*handler)(struct clock_event_device *))
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	struct clock_event_device *dev = td->evtdev;

	if (!dev || !(dev->features & CLOCK_EVT_FEAT_ONESHOT) ||
		    !tick_device_is_functional(dev)) {

		pr_info("Clockevents: could not switch to one-shot mode:");
		if (!dev) {
			pr_cont(" no tick device\n");
		} else {
			if (!tick_device_is_functional(dev))
				pr_cont(" %s is not functional.\n", dev->name);
			else
				pr_cont(" %s does not support one-shot mode.\n",
					dev->name);
		}
		return -EINVAL;
	}

	td->mode = TICKDEV_MODE_ONESHOT;
	dev->event_handler = handler;
	clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);
	tick_broadcast_switch_to_oneshot();
	return 0;
}

/**
 * tick_oneshot_mode_active - check whether the system is in oneshot mode
 *
 * Return: 1 when either nohz or highres are enabled, otherwise 0.
 */
/*
 * tick_oneshot_mode_active() - 查询当前 CPU 的 tick 层模式是否为 ONESHOT。
 * local_irq_save 同时防止本地 tick 状态转换并固定 per-CPU 访问，读取后恢复原 IRQ 状态；返回 1/0，不代表设备
 * state 回调一定成功，也不区分启用者是 highres 还是 NO_HZ。上方英文的 system 实际由本实现按当前 CPU 判断。
 */
int tick_oneshot_mode_active(void)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = __this_cpu_read(tick_cpu_device.mode) == TICKDEV_MODE_ONESHOT;
	local_irq_restore(flags);

	return ret;
}

#ifdef CONFIG_HIGH_RES_TIMERS
/**
 * tick_init_highres - switch to high resolution mode
 *
 * Called with interrupts disabled.
 *
 * Return: 0 on success, -EINVAL if the tick device cannot switch
 *         to oneshot/high-resolution mode.
 */
/*
 * tick_init_highres() - 以 hrtimer_interrupt 作为事件 handler 请求进入高分辨率模式。
 * 必须 IRQ-off；直接返回 tick_switch_to_oneshot 的 0/-EINVAL。成功只完成模式/handler 切换，具体 hrtimer
 * deadline 由 hrtimer 初始化/重编程路径提交；失败时高分辨率层回退，不创建额外设备或引用。
 */
int tick_init_highres(void)
{
	return tick_switch_to_oneshot(hrtimer_interrupt);
}
#endif
