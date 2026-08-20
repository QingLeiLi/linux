// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains functions which emulate a local clock-event
 * device via a broadcast event source.
 *
 * Copyright(C) 2005-2006, Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 */
/*
 * 本文件用一个共享 clockevent 或每 CPU 专用 wakeup clockevent，替代深 idle 中会停止的本地 tick device。
 * periodic 模式向 broadcast_mask 同步发节拍；oneshot 模式扫描各 CPU本地 next_event，只唤醒已到期者并重编
 * 最早剩余期限。tick_broadcast_lock 保护共享设备、全部 masks、模式与本地设备状态切换；handler 在锁外调用
 * 本 CPU回调以避免重入死锁。设备指针由 clockevents exchange/module 引用协议稳定，getter 仅借用。
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/module.h>

#include "tick-internal.h"

/*
 * Broadcast support for broken x86 hardware, where the local apic
 * timer stops in C3 state.
 */
/* 最初用于 x86 local APIC timer 在 C3 停止的硬件缺陷，现已抽象成通用 C3STOP/broadcast 框架。 */

static struct tick_device tick_broadcast_device;
static cpumask_var_t tick_broadcast_mask __cpumask_var_read_mostly;
static cpumask_var_t tick_broadcast_on __cpumask_var_read_mostly;
static cpumask_var_t tmpmask __cpumask_var_read_mostly;
static int tick_broadcast_forced;

static __cacheline_aligned_in_smp DEFINE_RAW_SPINLOCK(tick_broadcast_lock);
/*
 * broadcast_mask 是当前需要代打 tick 的 CPU，broadcast_on 是 periodic 电源状态请求，tmpmask 是锁内临时集合；
 * forced 一旦由 FORCE 置位便禁止 OFF 撤销。broadcast_device 保存共享设备与 PERIODIC/ONESHOT 模式。
 */

#ifdef CONFIG_TICK_ONESHOT
static DEFINE_PER_CPU(struct clock_event_device *, tick_oneshot_wakeup_device);
/* per-CPU wakeup device 若存在，优先于共享广播承担该 CPU deep-idle oneshot 唤醒，并持有 driver module 引用。 */

static void tick_broadcast_setup_oneshot(struct clock_event_device *bc, bool from_periodic);
static void tick_broadcast_clear_oneshot(int cpu);
static void tick_resume_broadcast_oneshot(struct clock_event_device *bc);
# ifdef CONFIG_HOTPLUG_CPU
static void tick_broadcast_oneshot_offline(unsigned int cpu);
# endif
#else
/* 无 oneshot 时 setup 是不可达 BUG；clear/resume/offline 无对应状态，均安全退化为空。 */
static inline void
tick_broadcast_setup_oneshot(struct clock_event_device *bc, bool from_periodic) { BUG(); }
static inline void tick_broadcast_clear_oneshot(int cpu) { }
static inline void tick_resume_broadcast_oneshot(struct clock_event_device *bc) { }
# ifdef CONFIG_HOTPLUG_CPU
static inline void tick_broadcast_oneshot_offline(unsigned int cpu) { }
# endif
#endif

/*
 * Debugging: see timer_list.c
 */
/* 返回静态共享 tick_device 借用指针，仅供诊断/内部读取；不加锁且不得释放。 */
struct tick_device *tick_get_broadcast_device(void)
{
	return &tick_broadcast_device;
}

/* 返回 broadcast_mask 内部可写指针；调用者必须遵守 broadcast lock/只读诊断语境。 */
struct cpumask *tick_get_broadcast_mask(void)
{
	return tick_broadcast_mask;
}

static struct clock_event_device *tick_get_oneshot_wakeup_device(int cpu);

/* 返回 @cpu oneshot wakeup device 的 const 借用指针；可能 NULL，不固定 CPU online 或设备后续替换。 */
const struct clock_event_device *tick_get_wakeup_device(int cpu)
{
	return tick_get_oneshot_wakeup_device(cpu);
}

/*
 * Start the device in periodic mode
 */
/* tick_broadcast_start_periodic() 清 forced 镜像并以 broadcast handler 配置非空 @bc；NULL 无动作，调用者持锁。 */
static void tick_broadcast_start_periodic(struct clock_event_device *bc)
{
	if (bc) {
		bc->next_event_forced = 0;
		tick_setup_periodic(bc, 1);
	}
}

/*
 * Check, if the device can be utilized as broadcast device:
 */
/*
 * tick_check_broadcast_device() - 只做共享广播候选过滤/评级比较。
 * DUMMY、PERCPU、C3STOP 均不适合作共享源；系统已 ONESHOT 时还要求 ONESHOT feature。通过后仅在无当前设备或
 * rating 更高时 true。函数不取 module 引用、不交换设备，@curdev 可 NULL。
 */
static bool tick_check_broadcast_device(struct clock_event_device *curdev,
					struct clock_event_device *newdev)
{
	if ((newdev->features & CLOCK_EVT_FEAT_DUMMY) ||
	    (newdev->features & CLOCK_EVT_FEAT_PERCPU) ||
	    (newdev->features & CLOCK_EVT_FEAT_C3STOP))
		return false;

	if (tick_broadcast_device.mode == TICKDEV_MODE_ONESHOT &&
	    !(newdev->features & CLOCK_EVT_FEAT_ONESHOT))
		return false;

	return !curdev || newdev->rating > curdev->rating;
}

#ifdef CONFIG_TICK_ONESHOT
/* 返回 @cpu per-CPU wakeup device 借用指针；无锁，调用者在注册/hotplug 或只读稳定语境使用。 */
static struct clock_event_device *tick_get_oneshot_wakeup_device(int cpu)
{
	return per_cpu(tick_oneshot_wakeup_device, cpu);
}

/*
 * tick_oneshot_wakeup_handler() - 专用 wakeup device 到期回调。
 * 清该设备 forced 镜像后调用本 CPU broadcast receive，间隙中若本地期限已重编，额外回调被视为无害 spurious。
 */
static void tick_oneshot_wakeup_handler(struct clock_event_device *wd)
{
	wd->next_event_forced = 0;
	/*
	 * If we woke up early and the tick was reprogrammed in the
	 * meantime then this may be spurious but harmless.
	 */
	/* 提前唤醒与并发重编最多多执行一次本地 handler，其自身会依据最新期限/状态收敛。 */
	tick_receive_broadcast();
}

/*
 * tick_set_oneshot_wakeup_device() - 为 @cpu 条件安装/替换专用 idle wakeup clockevent。
 * NULL 表示卸载。新设备必须非 DUMMY/C3STOP、同时 PERCPU+ONESHOT、cpumask 恰为该 CPU、rating 更高且 module
 * 可取引用；成功安装 handler，exchange 归还旧引用/状态并发布 per-CPU 指针，返回 true。任一校验失败 false且不改。
 */
static bool tick_set_oneshot_wakeup_device(struct clock_event_device *newdev,
					   int cpu)
{
	struct clock_event_device *curdev = tick_get_oneshot_wakeup_device(cpu);

	if (!newdev)
		goto set_device;

	if ((newdev->features & CLOCK_EVT_FEAT_DUMMY) ||
	    (newdev->features & CLOCK_EVT_FEAT_C3STOP))
		 return false;

	if (!(newdev->features & CLOCK_EVT_FEAT_PERCPU) ||
	    !(newdev->features & CLOCK_EVT_FEAT_ONESHOT))
		return false;

	if (!cpumask_equal(newdev->cpumask, cpumask_of(cpu)))
		return false;

	if (curdev && newdev->rating <= curdev->rating)
		return false;

	if (!try_module_get(newdev->owner))
		return false;

	newdev->event_handler = tick_oneshot_wakeup_handler;
set_device:
	clockevents_exchange_device(curdev, newdev);
	per_cpu(tick_oneshot_wakeup_device, cpu) = newdev;
	return true;
}
#else
/* 无 oneshot 支持时 per-CPU wakeup getter 恒 NULL，setter 恒 false且不接管设备。 */
static struct clock_event_device *tick_get_oneshot_wakeup_device(int cpu)
{
	return NULL;
}

static bool tick_set_oneshot_wakeup_device(struct clock_event_device *newdev,
					   int cpu)
{
	return false;
}
#endif

/*
 * Conditionally install/replace broadcast device
 */
/*
 * tick_install_broadcast_device() - 注册路径中优先安装 per-CPU wakeup，否则择优替换共享广播设备。
 * wakeup setter 成功即返回。共享候选须通过 feature/rating 与 module 引用；exchange 后旧设备 handler 置 noop，
 * 发布新 evtdev。已有 periodic 用户则启动。无 ONESHOT feature 到此结束；系统已 oneshot 时立即切新设备，
 * 否则通知所有 CPU重评估因旧时缺少 oneshot broadcast 而卡住的模式。调用者处于 clockevents 注册锁语境。
 */
void tick_install_broadcast_device(struct clock_event_device *dev, int cpu)
{
	struct clock_event_device *cur = tick_broadcast_device.evtdev;

	if (tick_set_oneshot_wakeup_device(dev, cpu))
		return;

	if (!tick_check_broadcast_device(cur, dev))
		return;

	if (!try_module_get(dev->owner))
		return;

	clockevents_exchange_device(cur, dev);
	if (cur)
		cur->event_handler = clockevents_handle_noop;
	tick_broadcast_device.evtdev = dev;
	if (!cpumask_empty(tick_broadcast_mask))
		tick_broadcast_start_periodic(dev);

	if (!(dev->features & CLOCK_EVT_FEAT_ONESHOT))
		return;

	/*
	 * If the system already runs in oneshot mode, switch the newly
	 * registered broadcast device to oneshot mode explicitly.
	 */
	/* 全局模式已切换时替换设备必须同步配置 handler/state，不能等下一次 CPU 通知。 */
	if (tick_broadcast_oneshot_active()) {
		tick_broadcast_switch_to_oneshot();
		return;
	}

	/*
	 * Inform all cpus about this. We might be in a situation
	 * where we did not switch to oneshot mode because the per cpu
	 * devices are affected by CLOCK_EVT_FEAT_C3STOP and the lack
	 * of a oneshot capable broadcast device. Without that
	 * notification the systems stays stuck in periodic mode
	 * forever.
	 */
	/* 新增 oneshot 能力可能解除各 CPU C3STOP 门槛；广播 check_clocks 让 softirq 路径重新尝试。 */
	tick_clock_notify();
}

/*
 * Check, if the device is the broadcast device
 */
/* tick_is_broadcast_device() 仅做非空指针同一性比较；返回 1/0，不验证注册状态。 */
int tick_is_broadcast_device(struct clock_event_device *dev)
{
	return (dev && tick_broadcast_device.evtdev == dev);
}

/*
 * tick_broadcast_update_freq() - 仅允许当前共享广播设备更新频率。
 * 非当前设备返回 -ENODEV；命中后持 broadcast raw lock 调 core 重算并原样返回 0/errno，不替换设备。
 */
int tick_broadcast_update_freq(struct clock_event_device *dev, u32 freq)
{
	int ret = -ENODEV;

	if (tick_is_broadcast_device(dev)) {
		raw_spin_lock(&tick_broadcast_lock);
		ret = __clockevents_update_freq(dev, freq);
		raw_spin_unlock(&tick_broadcast_lock);
	}
	return ret;
}


/* err_broadcast() 是缺少架构 IPI broadcast hook 的兜底：全局仅打印一次严重错误，@mask 不被处理。 */
static void err_broadcast(const struct cpumask *mask)
{
	pr_crit_once("Failed to broadcast timer tick. Some CPUs may be unresponsive.\n");
}

/*
 * tick_device_setup_broadcast_func() - 确保本地设备有可调用的 broadcast(mask) hook。
 * 优先采用架构全局 tick_broadcast；仍为空则告警并安装 err_broadcast，防后续 NULL 调用但不能真正唤醒 CPU。
 */
static void tick_device_setup_broadcast_func(struct clock_event_device *dev)
{
	if (!dev->broadcast)
		dev->broadcast = tick_broadcast;
	if (!dev->broadcast) {
		pr_warn_once("%s depends on broadcast, but no broadcast function available\n",
			     dev->name);
		dev->broadcast = err_broadcast;
	}
}

/*
 * Check, if the device is dysfunctional and a placeholder, which
 * needs to be handled by the broadcast device.
 */
/*
 * tick_device_uses_broadcast() - 注册/替换本 CPU设备时更新 periodic/oneshot 广播 membership。
 * 持 irqsave broadcast lock。非功能 DUMMY 设 periodic handler/hook、加入 mask、启动对应共享后端并返回 1。
 * 功能设备若不 C3STOP 则退出 mask，否则确保 hook；未显式 broadcast_on 也清 periodic mask。ONESHOT 清本 CPU
 * oneshot/pending 让调用者初始化本地设备并返回 0；PERIODIC 在 mask 空时停共享设备，对非 hrtimer 共享源返回
 * 本 CPU是否应保持本地 shutdown。所有 mask/device 变更在解锁前发布。
 */
int tick_device_uses_broadcast(struct clock_event_device *dev, int cpu)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	/*
	 * Devices might be registered with both periodic and oneshot
	 * mode disabled. This signals, that the device needs to be
	 * operated from the broadcast device and is a placeholder for
	 * the cpu local device.
	 */
	/* 同时缺 periodic/oneshot feature 的设备是占位符，本地不可编程，必须完全由广播代打。 */
	if (!tick_device_is_functional(dev)) {
		dev->event_handler = tick_handle_periodic;
		tick_device_setup_broadcast_func(dev);
		cpumask_set_cpu(cpu, tick_broadcast_mask);
		if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
			tick_broadcast_start_periodic(bc);
		else
			tick_broadcast_setup_oneshot(bc, false);
		ret = 1;
	} else {
		/*
		 * Clear the broadcast bit for this cpu if the
		 * device is not power state affected.
		 */
		/* 功能设备若深 idle 仍运行，可永久退出广播；C3STOP 设备只安装 hook，membership 还由 on 状态决定。 */
		if (!(dev->features & CLOCK_EVT_FEAT_C3STOP))
			cpumask_clear_cpu(cpu, tick_broadcast_mask);
		else
			tick_device_setup_broadcast_func(dev);

		/*
		 * Clear the broadcast bit if the CPU is not in
		 * periodic broadcast on state.
		 */
		/* broadcast_on 是显式电源状态请求；未请求者即使 C3STOP 也不应留在 periodic 目标集合。 */
		if (!cpumask_test_cpu(cpu, tick_broadcast_on))
			cpumask_clear_cpu(cpu, tick_broadcast_mask);

		switch (tick_broadcast_device.mode) {
		case TICKDEV_MODE_ONESHOT:
			/*
			 * If the system is in oneshot mode we can
			 * unconditionally clear the oneshot mask bit,
			 * because the CPU is running and therefore
			 * not in an idle state which causes the power
			 * state affected device to stop. Let the
			 * caller initialize the device.
			 */
			/* CPU正在运行，不会受 idle 停钟影响；清 oneshot 状态后由设备安装调用者重新初始化本地源。 */
			tick_broadcast_clear_oneshot(cpu);
			ret = 0;
			break;

		case TICKDEV_MODE_PERIODIC:
			/*
			 * If the system is in periodic mode, check
			 * whether the broadcast device can be
			 * switched off now.
			 */
			/* periodic 最后一个用户退出时立即 shutdown 共享源，避免无目标空中断。 */
			if (cpumask_empty(tick_broadcast_mask) && bc)
				clockevents_shutdown(bc);
			/*
			 * If we kept the cpu in the broadcast mask,
			 * tell the caller to leave the per cpu device
			 * in shutdown state. The periodic interrupt
			 * is delivered by the broadcast device, if
			 * the broadcast device exists and is not
			 * hrtimer based.
			 */
			/* 普通共享硬件可完全代打并让本地保持 shutdown；hrtimer 广播仍依赖本地硬件承载，不能这样返回。 */
			if (bc && !(bc->features & CLOCK_EVT_FEAT_HRTIMER))
				ret = cpumask_test_cpu(cpu, tick_broadcast_mask);
			break;
		default:
			break;
		}
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
	return ret;
}

/*
 * tick_receive_broadcast() - 在目标 CPU上下文直接执行其本地 clockevent handler。
 * 无 evtdev 返回 -ENODEV，无 handler 返回 -EINVAL；否则调用并返回 0。对象只借用，handler 可进入完整 tick 路径。
 */
int tick_receive_broadcast(void)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	struct clock_event_device *evt = td->evtdev;

	if (!evt)
		return -ENODEV;

	if (!evt->event_handler)
		return -EINVAL;

	evt->event_handler(evt);
	return 0;
}

/*
 * Broadcast the event to the cpus, which are set in the mask (mangled).
 */
/*
 * tick_do_broadcast() - 消费可修改 @mask，把事件发给其中 CPU并返回是否需本地直调。
 * 当前 CPU命中时先从 mask 清除；共享源非 HRTIMER 才返回 local=true，避免 hrtimer callback 递归。剩余集合取
 * 首 CPU本地设备的 broadcast hook 发 IPI；假设同平台 hook 兼容。调用者持 broadcast lock，@mask 通常是 tmpmask。
 */
static bool tick_do_broadcast(struct cpumask *mask)
{
	int cpu = smp_processor_id();
	struct tick_device *td;
	bool local = false;

	/*
	 * Check, if the current cpu is in the mask
	 */
	/* 本 CPU不能通过 IPI 发给自己，先从目标集合摘除并决定是否锁外直调。 */
	if (cpumask_test_cpu(cpu, mask)) {
		struct clock_event_device *bc = tick_broadcast_device.evtdev;

		cpumask_clear_cpu(cpu, mask);
		/*
		 * We only run the local handler, if the broadcast
		 * device is not hrtimer based. Otherwise we run into
		 * a hrtimer recursion.
		 *
		 * local timer_interrupt()
		 *   local_handler()
		 *     expire_hrtimers()
		 *       bc_handler()
		 *         local_handler()
		 *	     expire_hrtimers()
		 */
		/* hrtimer 型 bc 回调内再调用本地 hrtimer handler 会递归；其本地事件已由当前 hrtimer 扫描自然处理。 */
		local = !(bc->features & CLOCK_EVT_FEAT_HRTIMER);
	}

	if (!cpumask_empty(mask)) {
		/*
		 * It might be necessary to actually check whether the devices
		 * have different broadcast functions. For now, just use the
		 * one of the first device. This works as long as we have this
		 * misfeature only on x86 (lapic)
		 */
		/* 当前假设所有目标共享同一架构 broadcast hook，故借第一个 CPU设备发送整张 mask。 */
		td = &per_cpu(tick_cpu_device, cpumask_first(mask));
		td->evtdev->broadcast(mask);
	}
	return local;
}

/*
 * Periodic broadcast:
 * - invoke the broadcast handlers
 */
/* tick_do_periodic_broadcast() 取 online 与 broadcast_mask 交集到 tmpmask，分发并返回是否需锁外本地 handler。 */
static bool tick_do_periodic_broadcast(void)
{
	cpumask_and(tmpmask, cpu_online_mask, tick_broadcast_mask);
	return tick_do_broadcast(tmpmask);
}

/*
 * Event handler for periodic broadcast ticks
 */
/*
 * tick_handle_periodic_broadcast() - 共享 periodic tick IRQ handler。
 * 持 broadcast lock 清 forced，shutdown spurious 直接返回；构造在线目标并发 IPI。若共享硬件实际以 oneshot
 * 仿 periodic，则从原 next_event 加 TICK_NSEC 强制重编。解锁后才执行本 CPU handler，避免其切 oneshot 反取锁。
 */
static void tick_handle_periodic_broadcast(struct clock_event_device *dev)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	bool bc_local;

	raw_spin_lock(&tick_broadcast_lock);
	tick_broadcast_device.evtdev->next_event_forced = 0;

	/* Handle spurious interrupts gracefully */
	/* suspend/shutdown 后迟到 IRQ 不应广播，也不能重编已停设备。 */
	if (clockevent_state_shutdown(tick_broadcast_device.evtdev)) {
		raw_spin_unlock(&tick_broadcast_lock);
		return;
	}

	bc_local = tick_do_periodic_broadcast();

	if (clockevent_state_oneshot(dev)) {
		ktime_t next = ktime_add_ns(dev->next_event, TICK_NSEC);

		clockevents_program_event(dev, next, true);
	}
	raw_spin_unlock(&tick_broadcast_lock);

	/*
	 * We run the handler of the local cpu after dropping
	 * tick_broadcast_lock because the handler might deadlock when
	 * trying to switch to oneshot mode.
	 */
	/* 本地 handler 可能进入 clockevents/tick 模式转换并再次需要 broadcast lock，必须锁外调用。 */
	if (bc_local)
		td->evtdev->event_handler(td->evtdev);
}

/**
 * tick_broadcast_control - Enable/disable or force broadcast mode
 * @mode:	The selected broadcast mode
 *
 * Called when the system enters a state where affected tick devices
 * might stop. Note: TICK_BROADCAST_FORCE cannot be undone.
 */
/*
 * tick_broadcast_control() - 当前 CPU进入/退出会停本地 timer 的电源状态，或永久强制 periodic 广播。
 * irqsave 持锁并保护本地 device；非 C3STOP/非功能设备无动作。FORCE 置不可撤销全局位后按 ON 加入 on/mask；
 * 首次加入时仅在非 hrtimer 且 periodic 共享源存在时 shutdown 本地。OFF 在未 forced 时清 on/mask并恢复本地
 * periodic。最后按 mask 空↔非空边沿停止/启动共享设备或设置 oneshot。无返回值，缺共享设备时只维护 masks。
 */
void tick_broadcast_control(enum tick_broadcast_mode mode)
{
	struct clock_event_device *bc, *dev;
	struct tick_device *td;
	int cpu, bc_stopped;
	unsigned long flags;

	/* Protects also the local clockevent device. */
	/* 同一 raw lock 使 membership 判断与本地 shutdown/setup 原子，封闭广播遗漏窗口。 */
	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);
	td = this_cpu_ptr(&tick_cpu_device);
	dev = td->evtdev;

	/*
	 * Is the device not affected by the powerstate ?
	 */
	/* 只有功能正常且带 C3STOP 的本地设备需要动态 on/off membership。 */
	if (!dev || !(dev->features & CLOCK_EVT_FEAT_C3STOP))
		goto out;

	if (!tick_device_is_functional(dev))
		goto out;

	cpu = smp_processor_id();
	bc = tick_broadcast_device.evtdev;
	bc_stopped = cpumask_empty(tick_broadcast_mask);

	switch (mode) {
	case TICK_BROADCAST_FORCE:
		tick_broadcast_forced = 1;
		fallthrough;
	case TICK_BROADCAST_ON:
		cpumask_set_cpu(cpu, tick_broadcast_on);
		if (!cpumask_test_and_set_cpu(cpu, tick_broadcast_mask)) {
			/*
			 * Only shutdown the cpu local device, if:
			 *
			 * - the broadcast device exists
			 * - the broadcast device is not a hrtimer based one
			 * - the broadcast device is in periodic mode to
			 *   avoid a hiccup during switch to oneshot mode
			 */
			/* hrtimer 广播或模式切换期仍可能依赖本地硬件，不能贸然 shutdown。 */
			if (bc && !(bc->features & CLOCK_EVT_FEAT_HRTIMER) &&
			    tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
				clockevents_shutdown(dev);
		}
		break;

	case TICK_BROADCAST_OFF:
		if (tick_broadcast_forced)
			break;
		cpumask_clear_cpu(cpu, tick_broadcast_on);
		if (cpumask_test_and_clear_cpu(cpu, tick_broadcast_mask)) {
			if (tick_broadcast_device.mode ==
			    TICKDEV_MODE_PERIODIC)
				tick_setup_periodic(dev, 0);
		}
		break;
	}

	if (bc) {
		if (cpumask_empty(tick_broadcast_mask)) {
			if (!bc_stopped)
				clockevents_shutdown(bc);
		} else if (bc_stopped) {
			if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC)
				tick_broadcast_start_periodic(bc);
			else
				tick_broadcast_setup_oneshot(bc, false);
		}
	}
out:
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}
EXPORT_SYMBOL_GPL(tick_broadcast_control);

/*
 * Set the periodic handler depending on broadcast on/off
 */
/* tick_set_periodic_handler() 按 @broadcast 选择普通或共享周期 handler；只写回调，不切设备 state。 */
void tick_set_periodic_handler(struct clock_event_device *dev, int broadcast)
{
	if (!broadcast)
		dev->event_handler = tick_handle_periodic;
	else
		dev->event_handler = tick_handle_periodic_broadcast;
}

#ifdef CONFIG_HOTPLUG_CPU
/* periodic 模式且 mask 已空时 shutdown 共享设备；oneshot 或仍有用户均保持。调用者持 broadcast lock。 */
static void tick_shutdown_broadcast(void)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;

	if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC) {
		if (bc && cpumask_empty(tick_broadcast_mask))
			clockevents_shutdown(bc);
	}
}

/*
 * Remove a CPU from broadcasting
 */
/*
 * tick_broadcast_offline() - CPU hotplug 下线时从 periodic on/mask 和全部 oneshot 状态移除 @cpu。
 * 持 broadcast lock，释放其专用 wakeup device，再在 periodic 无用户时停共享设备；不释放共享源。
 */
void tick_broadcast_offline(unsigned int cpu)
{
	raw_spin_lock(&tick_broadcast_lock);
	cpumask_clear_cpu(cpu, tick_broadcast_mask);
	cpumask_clear_cpu(cpu, tick_broadcast_on);
	tick_broadcast_oneshot_offline(cpu);
	tick_shutdown_broadcast();
	raw_spin_unlock(&tick_broadcast_lock);
}

#endif

/* tick_suspend_broadcast() irqsave 持锁并 shutdown 当前共享设备；mask/mode 保留供 resume 重建。 */
void tick_suspend_broadcast(void)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	bc = tick_broadcast_device.evtdev;
	if (bc)
		clockevents_shutdown(bc);

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * This is called from tick_resume_local() on a resuming CPU. That's
 * called from the core resume function, tick_unfreeze() and the magic XEN
 * resume hackery.
 *
 * In none of these cases the broadcast device mode can change and the
 * bit of the resuming CPU in the broadcast mask is safe as well.
 */
/*
 * tick_resume_check_broadcast() - 单 CPU恢复路径判断本地 periodic tick 是否仍由广播代打。
 * resume 串行保证 mode/mask 稳定；ONESHOT 恒 false，PERIODIC 返回当前 CPU membership。无锁只读。
 */
bool tick_resume_check_broadcast(void)
{
	if (tick_broadcast_device.mode == TICKDEV_MODE_ONESHOT)
		return false;
	else
		return cpumask_test_cpu(smp_processor_id(), tick_broadcast_mask);
}

/*
 * tick_resume_broadcast() - 系统恢复共享设备并按保留模式/用户集合重新启动。
 * irqsave 持锁，先调用可选 driver tick_resume（错误被忽略），PERIODIC 非空则重启周期，ONESHOT 非空只恢复
 * oneshot state，后续期限由正常扫描编程。无设备无动作。
 */
void tick_resume_broadcast(void)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	bc = tick_broadcast_device.evtdev;

	if (bc) {
		clockevents_tick_resume(bc);

		switch (tick_broadcast_device.mode) {
		case TICKDEV_MODE_PERIODIC:
			if (!cpumask_empty(tick_broadcast_mask))
				tick_broadcast_start_periodic(bc);
			break;
		case TICKDEV_MODE_ONESHOT:
			if (!cpumask_empty(tick_broadcast_mask))
				tick_resume_broadcast_oneshot(bc);
			break;
		}
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

#ifdef CONFIG_TICK_ONESHOT

static cpumask_var_t tick_broadcast_oneshot_mask __cpumask_var_read_mostly;
static cpumask_var_t tick_broadcast_pending_mask __cpumask_var_read_mostly;
static cpumask_var_t tick_broadcast_force_mask __cpumask_var_read_mostly;

/*
 * Exposed for debugging: see timer_list.c
 */
/* 返回 oneshot membership 内部 mask 借用指针，仅供诊断/受锁调用者，不保证无锁一致快照。 */
struct cpumask *tick_get_broadcast_oneshot_mask(void)
{
	return tick_broadcast_oneshot_mask;
}

/*
 * Called before going idle with interrupts disabled. Checks whether a
 * broadcast event from the other core is about to happen. We detected
 * that in tick_broadcast_oneshot_control(). The callsite can use this
 * to avoid a deep idle transition as we are about to get the
 * broadcast IPI right away.
 */
/*
 * tick_check_broadcast_expired() - IRQ-off idle 入口无插桩查询本 CPU force bit。
 * 返回 1 表示 broadcast handler 即将以 IPI代执行过期本地事件，调用者应避免 deep idle；不清 bit。根据 bitops
 * 插桩配置选 arch_test_bit 以保持 noinstr 安全，mask 生命周期静态。
 */
noinstr int tick_check_broadcast_expired(void)
{
#ifdef _ASM_GENERIC_BITOPS_INSTRUMENTED_NON_ATOMIC_H
	return arch_test_bit(smp_processor_id(), cpumask_bits(tick_broadcast_force_mask));
#else
	return cpumask_test_cpu(smp_processor_id(), tick_broadcast_force_mask);
#endif
}

/*
 * Set broadcast interrupt affinity
 */
/*
 * tick_broadcast_set_affinity() - 对 DYNIRQ 共享源把 IRQ 目标改为 @cpumask。
 * 非动态或 mask 内容未变无动作；否则发布 bc->cpumask 并调用 irq_set_affinity，返回错误被忽略。调用者持锁。
 */
static void tick_broadcast_set_affinity(struct clock_event_device *bc,
					const struct cpumask *cpumask)
{
	if (!(bc->features & CLOCK_EVT_FEAT_DYNIRQ))
		return;

	if (cpumask_equal(bc->cpumask, cpumask))
		return;

	bc->cpumask = cpumask;
	irq_set_affinity(bc->irq, bc->cpumask);
}

/*
 * tick_broadcast_set_event() - 以 @expires 编共享 oneshot 并把动态 IRQ 亲和性指向负责 @cpu。
 * 必要时先切 ONESHOT，force 编程错误被忽略，再设置 affinity；next_event 镜像由 clockevents core 更新。
 */
static void tick_broadcast_set_event(struct clock_event_device *bc, int cpu,
				     ktime_t expires)
{
	if (!clockevent_state_oneshot(bc))
		clockevents_switch_state(bc, CLOCK_EVT_STATE_ONESHOT);

	clockevents_program_event(bc, expires, 1);
	tick_broadcast_set_affinity(bc, cpumask_of(cpu));
}

/* tick_resume_broadcast_oneshot() 仅把共享源切回 ONESHOT；不编期限，调用者持锁。 */
static void tick_resume_broadcast_oneshot(struct clock_event_device *bc)
{
	clockevents_switch_state(bc, CLOCK_EVT_STATE_ONESHOT);
}

/*
 * Called from irq_enter() when idle was interrupted to reenable the
 * per cpu device.
 */
/*
 * tick_check_oneshot_broadcast_this_cpu() - IRQ enter 时恢复处于 oneshot broadcast mask 的本地设备。
 * 若 CPU仍处于 PERIODIC→ONESHOT 切换中则保持不动；只有 td mode 已 ONESHOT 才请求设备切回 ONESHOT。
 * 不清 membership，deep-idle exit control 负责完整退出。
 */
void tick_check_oneshot_broadcast_this_cpu(void)
{
	if (cpumask_test_cpu(smp_processor_id(), tick_broadcast_oneshot_mask)) {
		struct tick_device *td = this_cpu_ptr(&tick_cpu_device);

		/*
		 * We might be in the middle of switching over from
		 * periodic to oneshot. If the CPU has not yet
		 * switched over, leave the device alone.
		 */
		if (td->mode == TICKDEV_MODE_ONESHOT) {
			clockevents_switch_state(td->evtdev,
					      CLOCK_EVT_STATE_ONESHOT);
		}
	}
}

/*
 * Handle oneshot mode broadcasting
 */
/*
 * tick_handle_oneshot_broadcast() - 共享 oneshot 到期时收集、唤醒并重编最早剩余 CPU事件。
 * 持锁清共享 next/forced，采 now，扫描 oneshot mask：到期者进 tmpmask/pending，未到期者选最小 next_event/cpu。
 * 清本 CPU pending（将锁外直调），合入并清 force mask，过滤 offline 后发 IPI。若仍有未来期限则重编共享源，
 * 解锁后按 bc_local 调本地 handler，避免 hrtimer recursion/锁反序。UP 特判空 mask，所有 masks 均锁内更新。
 */
static void tick_handle_oneshot_broadcast(struct clock_event_device *dev)
{
	struct tick_device *td;
	ktime_t now, next_event;
	int cpu, next_cpu = 0;
	bool bc_local;

	raw_spin_lock(&tick_broadcast_lock);
	dev->next_event = KTIME_MAX;
	tick_broadcast_device.evtdev->next_event_forced = 0;
	next_event = KTIME_MAX;
	cpumask_clear(tmpmask);
	now = ktime_get();
	/* Find all expired events */
	/* 扫描借用每 CPU evtdev->next_event；到期与未来最小值在同一 broadcast lock 快照内分类。 */
	for_each_cpu(cpu, tick_broadcast_oneshot_mask) {
		/*
		 * Required for !SMP because for_each_cpu() reports
		 * unconditionally CPU0 as set on UP kernels.
		 */
		/* UP 的 for_each_cpu 对空 mask 仍可能报告 CPU0，显式 break 防伪造目标。 */
		if (!IS_ENABLED(CONFIG_SMP) &&
		    cpumask_empty(tick_broadcast_oneshot_mask))
			break;

		td = &per_cpu(tick_cpu_device, cpu);
		if (td->evtdev->next_event <= now) {
			cpumask_set_cpu(cpu, tmpmask);
			/*
			 * Mark the remote cpu in the pending mask, so
			 * it can avoid reprogramming the cpu local
			 * timer in tick_broadcast_oneshot_control().
			 */
			/* pending 告诉远端 EXIT：broadcast IPI 已覆盖过期事件，不要再把旧期限编回本地。 */
			cpumask_set_cpu(cpu, tick_broadcast_pending_mask);
		} else if (td->evtdev->next_event < next_event) {
			next_event = td->evtdev->next_event;
			next_cpu = cpu;
		}
	}

	/*
	 * Remove the current cpu from the pending mask. The event is
	 * delivered immediately in tick_do_broadcast() !
	 */
	/* 当前 CPU不等 IPI，由锁外 local handler 直接消费，因此 pending 位先清。 */
	cpumask_clear_cpu(smp_processor_id(), tick_broadcast_pending_mask);

	/* Take care of enforced broadcast requests */
	/* force 表示已过期但为避免 ping-pong 未重编本地者，本轮无条件纳入唤醒并一次性清集合。 */
	cpumask_or(tmpmask, tmpmask, tick_broadcast_force_mask);
	cpumask_clear(tick_broadcast_force_mask);

	/*
	 * Sanity check. Catch the case where we try to broadcast to
	 * offline cpus.
	 */
	/* hotplug 竞态若留下 offline 目标，告警后裁剪，避免向死亡 CPU 发 broadcast IPI。 */
	if (WARN_ON_ONCE(!cpumask_subset(tmpmask, cpu_online_mask)))
		cpumask_and(tmpmask, tmpmask, cpu_online_mask);

	/*
	 * Wakeup the cpus which have an expired event.
	 */
	/* tick_do_broadcast 会修改 tmpmask、发远端 IPI并返回是否需锁外执行本地回调。 */
	bc_local = tick_do_broadcast(tmpmask);

	/*
	 * Two reasons for reprogram:
	 *
	 * - The global event did not expire any CPU local
	 * events. This happens in dyntick mode, as the maximum PIT
	 * delta is quite small.
	 *
	 * - There are pending events on sleeping CPUs which were not
	 * in the event mask
	 */
	/* 即使本轮无到期目标，硬件最大 delta 或剩余睡眠 CPU也要求继续编最早未来事件。 */
	if (next_event != KTIME_MAX)
		tick_broadcast_set_event(dev, next_cpu, next_event);

	raw_spin_unlock(&tick_broadcast_lock);

	if (bc_local) {
		td = this_cpu_ptr(&tick_cpu_device);
		td->evtdev->event_handler(td->evtdev);
	}
}

/*
 * broadcast_needs_cpu() - 判断 hrtimer 型共享源当前是否绑定且仍需 @cpu。
 * 非 hrtimer 或无期限返回 0；bound_on==cpu 返回 -EBUSY，阻止该 CPU deep idle/offline，否则 0。
 */
static int broadcast_needs_cpu(struct clock_event_device *bc, int cpu)
{
	if (!(bc->features & CLOCK_EVT_FEAT_HRTIMER))
		return 0;
	if (bc->next_event == KTIME_MAX)
		return 0;
	return bc->bound_on == cpu ? -EBUSY : 0;
}

/*
 * broadcast_shutdown_local() - 进入共享 oneshot 前按 hrtimer 归属/期限条件关闭本地设备。
 * 普通硬件广播总 shutdown；hrtimer 广播若当前 CPU拥有共享 timer，或本地期限早于共享期限，则保持本地运行，
 * 避免失去承载/更早事件。调用者持 broadcast lock。
 */
static void broadcast_shutdown_local(struct clock_event_device *bc,
				     struct clock_event_device *dev)
{
	/*
	 * For hrtimer based broadcasting we cannot shutdown the cpu
	 * local device if our own event is the first one to expire or
	 * if we own the broadcast timer.
	 */
	/* hrtimer 广播的 owner 不能关承载它的本地 clockevent；本地更早事件也必须由本地先触发。 */
	if (bc->features & CLOCK_EVT_FEAT_HRTIMER) {
		if (broadcast_needs_cpu(bc, smp_processor_id()))
			return;
		if (dev->next_event < bc->next_event)
			return;
	}
	clockevents_switch_state(dev, CLOCK_EVT_STATE_SHUTDOWN);
}

/*
 * ___tick_broadcast_oneshot_control() - 共享设备方案的 deep-idle ENTER/EXIT 状态机。
 * ENTER 拒绝 hrtimer owner/periodic hrtimer，首次入 oneshot mask 后条件 shutdown 本地；force 命中或编程后共享
 * hrtimer 迁到本 CPU均返回 -EBUSY并避免深睡。EXIT 清 membership、恢复本地 ONESHOT；pending 表示 IPI已在途，
 * 无需重编。若本地期限已过则置 force 等 broadcast handler 代执行，避免 expired force-reprogram 与反复 idle
 * 形成 ping-pong；未来期限才重编本地。全程持 raw lock，返回 0 或 -EBUSY。
 */
static int ___tick_broadcast_oneshot_control(enum tick_broadcast_state state,
					     struct tick_device *td,
					     int cpu)
{
	struct clock_event_device *bc, *dev = td->evtdev;
	int ret = 0;
	ktime_t now;

	raw_spin_lock(&tick_broadcast_lock);
	bc = tick_broadcast_device.evtdev;

	if (state == TICK_BROADCAST_ENTER) {
		/*
		 * If the current CPU owns the hrtimer broadcast
		 * mechanism, it cannot go deep idle and we do not add
		 * the CPU to the broadcast mask. We don't have to go
		 * through the EXIT path as the local timer is not
		 * shutdown.
		 */
		/* owner 返回 -EBUSY 且不入 mask/不关本地，因此调用者无需走对应 EXIT。 */
		ret = broadcast_needs_cpu(bc, cpu);
		if (ret)
			goto out;

		/*
		 * If the broadcast device is in periodic mode, we
		 * return.
		 */
		/* 共享源仍 periodic 时不能建立 per-CPU oneshot deadline；hrtimer 型更明确拒绝 deep idle。 */
		if (tick_broadcast_device.mode == TICKDEV_MODE_PERIODIC) {
			/* If it is a hrtimer based broadcast, return busy */
			if (bc->features & CLOCK_EVT_FEAT_HRTIMER)
				ret = -EBUSY;
			goto out;
		}

		if (!cpumask_test_and_set_cpu(cpu, tick_broadcast_oneshot_mask)) {
			WARN_ON_ONCE(cpumask_test_cpu(cpu, tick_broadcast_pending_mask));

			/* Conditionally shut down the local timer. */
			/* 普通共享源关闭本地；hrtimer 源按 owner/期限条件可能保留。 */
			broadcast_shutdown_local(bc, dev);

			/*
			 * We only reprogram the broadcast timer if we
			 * did not mark ourself in the force mask and
			 * if the cpu local event is earlier than the
			 * broadcast event. If the current CPU is in
			 * the force mask, then we are going to be
			 * woken by the IPI right away; we return
			 * busy, so the CPU does not try to go deep
			 * idle.
			 */
			/* force 代表 IPI马上到，深睡没有意义；更早本地期限才需要提前共享 deadline。 */
			if (cpumask_test_cpu(cpu, tick_broadcast_force_mask)) {
				ret = -EBUSY;
			} else if (dev->next_event < bc->next_event) {
				tick_broadcast_set_event(bc, cpu, dev->next_event);
				/*
				 * In case of hrtimer broadcasts the
				 * programming might have moved the
				 * timer to this cpu. If yes, remove
				 * us from the broadcast mask and
				 * return busy.
				 */
				/* hrtimer reprogram 可能把共享 timer 绑定到当前 CPU；此时撤销 membership 并拒绝深睡。 */
				ret = broadcast_needs_cpu(bc, cpu);
				if (ret) {
					cpumask_clear_cpu(cpu,
						tick_broadcast_oneshot_mask);
				}
			}
		}
	} else {
		if (cpumask_test_and_clear_cpu(cpu, tick_broadcast_oneshot_mask)) {
			clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT);
			/*
			 * The cpu which was handling the broadcast
			 * timer marked this cpu in the broadcast
			 * pending mask and fired the broadcast
			 * IPI. So we are going to handle the expired
			 * event anyway via the broadcast IPI
			 * handler. No need to reprogram the timer
			 * with an already expired event.
			 */
			/* pending 已由广播 CPU置位且 IPI 在途；恢复 state 即可，旧过期期限无需再次提交。 */
			if (cpumask_test_and_clear_cpu(cpu,
				       tick_broadcast_pending_mask))
				goto out;

			/*
			 * Bail out if there is no next event.
			 */
			/* KTIME_MAX 表示本地无事件，退出 membership 后无需启动设备。 */
			if (dev->next_event == KTIME_MAX)
				goto out;
			/*
			 * If the pending bit is not set, then we are
			 * either the CPU handling the broadcast
			 * interrupt or we got woken by something else.
			 *
			 * We are no longer in the broadcast mask, so
			 * if the cpu local expiry time is already
			 * reached, we would reprogram the cpu local
			 * timer with an already expired event.
			 *
			 * This can lead to a ping-pong when we return
			 * to idle and therefore rearm the broadcast
			 * timer before the cpu local timer was able
			 * to fire. This happens because the forced
			 * reprogramming makes sure that the event
			 * will happen in the future and depending on
			 * the min_delta setting this might be far
			 * enough out that the ping-pong starts.
			 *
			 * If the cpu local next_event has expired
			 * then we know that the broadcast timer
			 * next_event has expired as well and
			 * broadcast is about to be handled. So we
			 * avoid reprogramming and enforce that the
			 * broadcast handler, which did not run yet,
			 * will invoke the cpu local handler.
			 *
			 * We cannot call the handler directly from
			 * here, because we might be in a NOHZ phase
			 * and we did not go through the irq_enter()
			 * nohz fixups.
			 */
			/* 已过期事件留给尚未运行的 broadcast handler，并置 force 阻止 deep idle；此处不能绕过 irq_enter 的 NO_HZ 记账直接调 handler。 */
			now = ktime_get();
			if (dev->next_event <= now) {
				cpumask_set_cpu(cpu, tick_broadcast_force_mask);
				goto out;
			}
			/*
			 * We got woken by something else. Reprogram
			 * the cpu local timer device.
			 */
			/* 非 broadcast 原因提前唤醒且期限仍未来，安全恢复本地一次性事件。 */
			tick_program_event(dev->next_event, 1);
		}
	}
out:
	raw_spin_unlock(&tick_broadcast_lock);
	return ret;
}

/*
 * tick_oneshot_wakeup_control() - 优先用 per-CPU wakeup device 处理 deep idle。
 * td 非 ONESHOT -EINVAL，无 wakeup device -ENODEV。ENTER 停本地、启 wakeup 并复制本地 deadline；EXIT 要求
 * wakeup 仍是 ONESHOT，否则 -ENODEV。编程错误不传播，成功返回 0。
 */
static int tick_oneshot_wakeup_control(enum tick_broadcast_state state,
				       struct tick_device *td,
				       int cpu)
{
	struct clock_event_device *dev, *wd;

	dev = td->evtdev;
	if (td->mode != TICKDEV_MODE_ONESHOT)
		return -EINVAL;

	wd = tick_get_oneshot_wakeup_device(cpu);
	if (!wd)
		return -ENODEV;

	switch (state) {
	case TICK_BROADCAST_ENTER:
		clockevents_switch_state(dev, CLOCK_EVT_STATE_ONESHOT_STOPPED);
		clockevents_switch_state(wd, CLOCK_EVT_STATE_ONESHOT);
		clockevents_program_event(wd, dev->next_event, 1);
		break;
	case TICK_BROADCAST_EXIT:
		/* We may have transitioned to oneshot mode while idle */
		/* idle 期间全局可能刚从 periodic 切换；wakeup device 未处于 ONESHOT 时不能宣称退出成功。 */
		if (clockevent_get_state(wd) != CLOCK_EVT_STATE_ONESHOT)
			return -ENODEV;
	}

	return 0;
}

/*
 * __tick_broadcast_oneshot_control() - 当前 CPU deep-idle broadcast 总分派。
 * 先尝试专用 wakeup device，成功即 0；否则有共享设备则进入完整 mask 状态机；两者都不可用返回 -EBUSY，
 * 明确阻止无法保证唤醒的深睡。@state 仅 ENTER/EXIT，由调用者 IRQ-off 传入。
 */
int __tick_broadcast_oneshot_control(enum tick_broadcast_state state)
{
	struct tick_device *td = this_cpu_ptr(&tick_cpu_device);
	int cpu = smp_processor_id();

	if (!tick_oneshot_wakeup_control(state, td, cpu))
		return 0;

	if (tick_broadcast_device.evtdev)
		return ___tick_broadcast_oneshot_control(state, td, cpu);

	/*
	 * If there is no broadcast or wakeup device, tell the caller not
	 * to go into deep idle.
	 */
	/* 两种唤醒设施都不可用时 fail closed，避免 CPU深睡后永久失去 tick。 */
	return -EBUSY;
}

/*
 * Reset the one shot broadcast for a cpu
 *
 * Called with tick_broadcast_lock held
 */
/* tick_broadcast_clear_oneshot() 在持锁下清 @cpu membership/pending；force 位保留给 handler 消费。 */
static void tick_broadcast_clear_oneshot(int cpu)
{
	cpumask_clear_cpu(cpu, tick_broadcast_oneshot_mask);
	cpumask_clear_cpu(cpu, tick_broadcast_pending_mask);
}

/* 把 @mask 中存在本地 evtdev 的 next_event 初始化为同一 @expires；用于 periodic→oneshot 过渡的首轮唤醒。 */
static void tick_broadcast_init_next_event(struct cpumask *mask,
					   ktime_t expires)
{
	struct tick_device *td;
	int cpu;

	for_each_cpu(cpu, mask) {
		td = &per_cpu(tick_cpu_device, cpu);
		if (td->evtdev)
			td->evtdev->next_event = expires;
	}
}

/*
 * tick_get_next_period() - 在 jiffies_lock 下完整读取 32/64 位 tick_next_period。
 * 返回值即使已过期也可用，后续 force 编程会立即触发；函数不需要 jiffies_seq，因为只取单个受锁值。
 */
static inline ktime_t tick_get_next_period(void)
{
	ktime_t next;

	/*
	 * Protect against concurrent updates (store /load tearing on
	 * 32bit). It does not matter if the time is already in the
	 * past. The broadcast device which is about to be programmed will
	 * fire in any case.
	 */
	/* jiffies_lock 防 32 位撕裂；即使期限已过，force 编程会安排最近可行中断。 */
	raw_spin_lock(&jiffies_lock);
	next = tick_next_period;
	raw_spin_unlock(&jiffies_lock);
	return next;
}

/**
 * tick_broadcast_setup_oneshot - setup the broadcast device
 * @bc: the broadcast device
 * @from_periodic: true if called from periodic mode
 */
/*
 * tick_broadcast_setup_oneshot() - 一次性安装共享 oneshot handler并处理 periodic 过渡/设备替换。
 * NULL 无动作；handler 已安装时仅清本 CPU oneshot/pending，保证各 CPU首次 ENTER 可更新期限。首次安装清
 * next/forced。from_periodic 时把除本 CPU外的 periodic mask 合入 oneshot mask，给其本地 next_event 设下一
 * 全局 tick，确保仍等待周期广播者被唤醒；若硬件已 ONESHOT 保留现有编程。替换设备时不改 membership，非空
 * mask 用 nexttick（过渡为周期值、替换为 0 立即事件）编共享源。调用者持 broadcast lock。
 */
static void tick_broadcast_setup_oneshot(struct clock_event_device *bc,
					 bool from_periodic)
{
	int cpu = smp_processor_id();
	ktime_t nexttick = 0;

	if (!bc)
		return;

	/*
	 * When the broadcast device was switched to oneshot by the first
	 * CPU handling the NOHZ change, the other CPUs will reach this
	 * code via hrtimer_run_queues() -> tick_check_oneshot_change()
	 * too. Set up the broadcast device only once!
	 */
	/* 首 CPU已安装共享 handler；其他 CPU随后到达时不能重置共享期限，只需清自己的过渡 membership。 */
	if (bc->event_handler == tick_handle_oneshot_broadcast) {
		/*
		 * The CPU which switched from periodic to oneshot mode
		 * set the broadcast oneshot bit for all other CPUs which
		 * are in the general (periodic) broadcast mask to ensure
		 * that CPUs which wait for the periodic broadcast are
		 * woken up.
		 *
		 * Clear the bit for the local CPU as the set bit would
		 * prevent the first tick_broadcast_enter() after this CPU
		 * switched to oneshot state to program the broadcast
		 * device.
		 *
		 * This code can also be reached via tick_broadcast_control(),
		 * but this cannot avoid the tick_broadcast_clear_oneshot()
		 * as that would break the periodic to oneshot transition of
		 * secondary CPUs. But that's harmless as the below only
		 * clears already cleared bits.
		 */
		/* 首次切换预置所有 periodic 等待者；本 CPU清位后，下一次真实 ENTER 才能更新自己的 deadline。 */
		tick_broadcast_clear_oneshot(cpu);
		return;
	}


	bc->event_handler = tick_handle_oneshot_broadcast;
	bc->next_event_forced = 0;
	bc->next_event = KTIME_MAX;

	/*
	 * When the tick mode is switched from periodic to oneshot it must
	 * be ensured that CPUs which are waiting for periodic broadcast
	 * get their wake-up at the next tick.  This is achieved by ORing
	 * tick_broadcast_mask into tick_broadcast_oneshot_mask.
	 *
	 * For other callers, e.g. broadcast device replacement,
	 * tick_broadcast_oneshot_mask must not be touched as this would
	 * set bits for CPUs which are already NOHZ, but not idle. Their
	 * next tick_broadcast_enter() would observe the bit set and fail
	 * to update the expiry time and the broadcast event device.
	 */
	/* 只有 periodic 过渡扩张集合；设备替换保留当前 idle 集合，避免运行中的 NOHZ CPU因预置位而跳过 ENTER 更新。 */
	if (from_periodic) {
		cpumask_copy(tmpmask, tick_broadcast_mask);
		/* Remove the local CPU as it is obviously not idle */
		/* 发起切换的本 CPU正在运行，无需等待下一次共享 tick。 */
		cpumask_clear_cpu(cpu, tmpmask);
		cpumask_or(tick_broadcast_oneshot_mask, tick_broadcast_oneshot_mask, tmpmask);

		/*
		 * Ensure that the oneshot broadcast handler will wake the
		 * CPUs which are still waiting for periodic broadcast.
		 */
		/* 为过渡 CPU统一保存下一 jiffy，oneshot handler 才能按本地 next_event 判断到期。 */
		nexttick = tick_get_next_period();
		tick_broadcast_init_next_event(tmpmask, nexttick);

		/*
		 * If the underlying broadcast clock event device is
		 * already in oneshot state, then there is nothing to do.
		 * The device was already armed for the next tick
		 * in tick_handle_broadcast_periodic()
		 */
		/* 以 oneshot 硬件模拟 periodic 时已为下一 tick 编程，保留该事件即可。 */
		if (clockevent_state_oneshot(bc))
			return;
	}

	/*
	 * When switching from periodic to oneshot mode arm the broadcast
	 * device for the next tick.
	 *
	 * If the broadcast device has been replaced in oneshot mode and
	 * the oneshot broadcast mask is not empty, then arm it to expire
	 * immediately in order to reevaluate the next expiring timer.
	 * @nexttick is 0 and therefore in the past which will cause the
	 * clockevent code to force an event.
	 *
	 * For both cases the programming can be avoided when the oneshot
	 * broadcast mask is empty.
	 *
	 * tick_broadcast_set_event() implicitly switches the broadcast
	 * device to oneshot state.
	 */
	/* 过渡用真实 nexttick；替换用过期 0 促发立即重扫。mask 空时没有需要唤醒的 CPU，可省编程。 */
	if (!cpumask_empty(tick_broadcast_oneshot_mask))
		tick_broadcast_set_event(bc, cpu, nexttick);
}

/*
 * Select oneshot operating mode for the broadcast device
 */
/*
 * tick_broadcast_switch_to_oneshot() - 原子发布共享 tick_device 模式并配置现有设备。
 * irqsave 持锁，保存 oldmode 后置 ONESHOT；有设备则以“是否从 PERIODIC 来”调用 setup。重复调用幂等清本 CPU位。
 */
void tick_broadcast_switch_to_oneshot(void)
{
	struct clock_event_device *bc;
	enum tick_device_mode oldmode;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);

	oldmode = tick_broadcast_device.mode;
	tick_broadcast_device.mode = TICKDEV_MODE_ONESHOT;
	bc = tick_broadcast_device.evtdev;
	if (bc)
		tick_broadcast_setup_oneshot(bc, oldmode == TICKDEV_MODE_PERIODIC);

	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * hotplug_cpu__broadcast_tick_pull() - hrtimer 广播绑定 CPU死亡前把共享 timer 拉到当前 CPU。
 * 持锁且仅在 bc 仍有期限并 bound_on==deadcpu 时动作。若当前 force bit 表示本地过期事件尚未重编，先清 bit并
 * force 编本地设备，避免共享 hrtimer 因本地设备未运行而饿死；再清 bc forced 并以原期限重编，借 affinity/
 * hrtimer 迁移改变 owner。无返回值，当前 CPU必须可承载设备。
 */
void hotplug_cpu__broadcast_tick_pull(int deadcpu)
{
	struct clock_event_device *bc;
	unsigned long flags;

	raw_spin_lock_irqsave(&tick_broadcast_lock, flags);
	bc = tick_broadcast_device.evtdev;

	if (bc && broadcast_needs_cpu(bc, deadcpu)) {
		/*
		 * If the broadcast force bit of the current CPU is set,
		 * then the current CPU has not yet reprogrammed the local
		 * timer device to avoid a ping-pong race. See
		 * ___tick_broadcast_oneshot_control().
		 *
		 * If the broadcast device is hrtimer based then
		 * programming the broadcast event below does not have any
		 * effect because the local clockevent device is not
		 * running and not programmed because the broadcast event
		 * is not earlier than the pending event of the local clock
		 * event device. As a consequence all CPUs waiting for a
		 * broadcast event are stuck forever.
		 *
		 * Detect this condition and reprogram the cpu local timer
		 * device to avoid the starvation.
		 */
		/* owner 下线且当前 force 未处理会让共享 hrtimer 无本地硬件承载；先恢复本地，再把共享 timer 拉到当前 CPU。 */
		if (tick_check_broadcast_expired()) {
			struct tick_device *td = this_cpu_ptr(&tick_cpu_device);

			cpumask_clear_cpu(smp_processor_id(), tick_broadcast_force_mask);
			tick_program_event(td->evtdev->next_event, 1);
		}

		/* This moves the broadcast assignment to this CPU: */
		/* hrtimer 型设备在当前 CPU重编后更新其 bound_on/承载归属。 */
		bc->next_event_forced = 0;
		clockevents_program_event(bc, bc->next_event, 1);
	}
	raw_spin_unlock_irqrestore(&tick_broadcast_lock, flags);
}

/*
 * Remove a dying CPU from broadcasting
 */
/*
 * tick_broadcast_oneshot_offline() - 持锁清理死亡 CPU 的专用 wakeup device 与三张 oneshot masks。
 * setter(NULL) 通过 exchange 归还旧设备/module；共享 broadcast device 即使集合暂空也不在此停止。
 */
static void tick_broadcast_oneshot_offline(unsigned int cpu)
{
	if (tick_get_oneshot_wakeup_device(cpu))
		tick_set_oneshot_wakeup_device(NULL, cpu);

	/*
	 * Clear the broadcast masks for the dead cpu, but do not stop
	 * the broadcast device!
	 */
	/* 清死亡 CPU的目标与在途状态，但共享源仍可能服务其他 CPU，不能在此 shutdown。 */
	cpumask_clear_cpu(cpu, tick_broadcast_oneshot_mask);
	cpumask_clear_cpu(cpu, tick_broadcast_pending_mask);
	cpumask_clear_cpu(cpu, tick_broadcast_force_mask);
}
#endif

/*
 * Check, whether the broadcast device is in one shot mode
 */
/* tick_broadcast_oneshot_active() 无锁比较共享模式，返回 1/0；注册/切换串行语境外仅是快照。 */
int tick_broadcast_oneshot_active(void)
{
	return tick_broadcast_device.mode == TICKDEV_MODE_ONESHOT;
}

/*
 * Check whether the broadcast device supports oneshot.
 */
/* tick_broadcast_oneshot_available() 要求共享设备存在且声明 ONESHOT feature；不代表当前已切模式。 */
bool tick_broadcast_oneshot_available(void)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;

	return bc ? bc->features & CLOCK_EVT_FEAT_ONESHOT : false;
}

#else
/*
 * CONFIG_TICK_ONESHOT=n 时 deep-idle control 不维护 masks：无共享源或 hrtimer 型源返回 -EBUSY，普通硬件共享源
 * 返回 0，依赖 periodic broadcast 继续唤醒。@state 在此配置下不影响状态。
 */
int __tick_broadcast_oneshot_control(enum tick_broadcast_state state)
{
	struct clock_event_device *bc = tick_broadcast_device.evtdev;

	if (!bc || (bc->features & CLOCK_EVT_FEAT_HRTIMER))
		return -EBUSY;

	return 0;
}
#endif

/*
 * tick_broadcast_init() - early init 分配并清零 periodic masks/tmpmask 及可选三张 oneshot masks。
 * 使用 GFP_NOWAIT 且当前实现忽略每次 zalloc_cpumask_var 的 bool 返回，不报告也不回滚此前分配；依赖启动环境
 * 能提供这些基础 masks。本函数不注册设备、不设置模式，成功分配的对象随后全局常驻。
 */
void __init tick_broadcast_init(void)
{
	zalloc_cpumask_var(&tick_broadcast_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&tick_broadcast_on, GFP_NOWAIT);
	zalloc_cpumask_var(&tmpmask, GFP_NOWAIT);
#ifdef CONFIG_TICK_ONESHOT
	zalloc_cpumask_var(&tick_broadcast_oneshot_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&tick_broadcast_pending_mask, GFP_NOWAIT);
	zalloc_cpumask_var(&tick_broadcast_force_mask, GFP_NOWAIT);
#endif
}
