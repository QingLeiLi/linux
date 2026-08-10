/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_IDLE_H
#define _LINUX_SCHED_IDLE_H

#include <linux/sched.h>

/*
 * enum cpu_idle_type - 调度负载均衡描述 CPU 空闲程度的离散类别。
 *
 * __CPU_NOT_IDLE 表示 CPU 正忙；CPU_IDLE 表示已经运行 idle task；
 * CPU_NEWLY_IDLE 表示刚失去 runnable 工作、可触发 newidle balance；
 * CPU_MAX_IDLE_TYPES 是数组维度/边界哨兵而非真实状态。值由调度域代码瞬时使用，
 * 不拥有 CPU，也不等同于 cpuidle 驱动的硬件 C-state。
 */
enum cpu_idle_type {
	__CPU_NOT_IDLE = 0,
	CPU_IDLE,
	CPU_NEWLY_IDLE,
	CPU_MAX_IDLE_TYPES
};

/*
 * wake_up_if_idle() - 仅当 @cpu 当前仍运行 idle task 时请求其重调度。
 *
 * @cpu 是输入逻辑 CPU 编号，调用者保证编号在相关 hotplug/调度保护下可用。实现
 * 先在 RCU 下无锁筛选，再锁 rq 复查并调用 resched_curr()；不保证目标立刻运行，
 * 不取得 task/rq ownership，返回无直接值。可用于不可睡眠路径。
 */
extern void wake_up_if_idle(int cpu);

/*
 * Idle thread specific functions to determine the need_resched
 * polling state.
 */
/*
 * 英文说明：以下 helper 只服务 idle thread 的 need_resched 轮询状态。
 * polling=1 表示 idle CPU 会主动检查 TIF_NEED_RESCHED，waker 可省 IPI；清零表示
 * CPU 可能进入硬件睡眠，之后的重调度请求必须发送 IPI。位更新与 need_resched
 * 检查之间的屏障和 resched_curr() 配对，确保两侧至少一方观察到对方状态。
 */
#ifdef TIF_POLLING_NRFLAG

#ifdef _ASM_GENERIC_BITOPS_INSTRUMENTED_ATOMIC_H

/*
 * __current_set_polling() - 用架构插桩原子位操作发布 current 正在轮询。
 *
 * 入参、返回值均无；只修改当前 thread_info flags 中 TIF_POLLING_NRFLAG，不包含
 * 后续屏障或 need_resched 复查。只能用于当前 idle task 的不可睡眠路径；调用者
 * 必须按协议补屏障/检查。instrumented bitops 让调试工具观察这次原子写。
 */
static __always_inline void __current_set_polling(void)
{
	arch_set_bit(TIF_POLLING_NRFLAG,
		     (unsigned long *)(&current_thread_info()->flags));
}

/*
 * __current_clr_polling() - 用架构插桩原子位操作撤销 current 的轮询承诺。
 *
 * 无参数、无返回且不睡眠；仅清位，不自行复查 need_resched。调用者必须随后
 * 使用配对屏障，才能安全走向睡眠或调度。
 */
static __always_inline void __current_clr_polling(void)
{
	arch_clear_bit(TIF_POLLING_NRFLAG,
		       (unsigned long *)(&current_thread_info()->flags));
}

#else

/*
 * __current_set_polling() - 用通用原子 set_bit 发布 current 正在轮询。
 *
 * 无参数、无返回、不睡眠；只置位而不含屏障/复查，调用者承担完整协议。该分支
 * 用于未通过 instrumented atomic bitops 提供架构实现的配置。
 */
static __always_inline void __current_set_polling(void)
{
	set_bit(TIF_POLLING_NRFLAG,
		(unsigned long *)(&current_thread_info()->flags));
}

/*
 * __current_clr_polling() - 用通用原子 clear_bit 清除 current polling 位。
 *
 * 无参数、无返回、不睡眠；只执行原子清位，后续屏障与 need_resched 折叠由调用
 * wrapper 完成。
 */
static __always_inline void __current_clr_polling(void)
{
	clear_bit(TIF_POLLING_NRFLAG,
		  (unsigned long *)(&current_thread_info()->flags));
}

#endif /* _ASM_GENERIC_BITOPS_INSTRUMENTED_ATOMIC_H */
/* 两个 bitops 分支只改变插桩实现，不改变 polling 位的并发语义。 */

/*
 * current_set_polling_and_test() - 发布 polling 后复查是否已需重调度。
 *
 * 入参无；current 必须是本 CPU idle task。不可睡眠。返回 true 表示
 * TIF_NEED_RESCHED 已经置位，调用者不得进入 idle；false 表示本次未观察到请求，
 * 且未来 waker 会看到 polling 或按竞态协议发 IPI。__must_check 禁止忽略这个
 * 安全判定。函数不取得对象 ownership。
 */
static __always_inline bool __must_check current_set_polling_and_test(void)
{
	__current_set_polling();

	/*
	 * Polling state must be visible before we test NEED_RESCHED,
	 * paired by resched_curr()
	 */
	/*
	 * 英文说明：置 polling 必须先于读取 NEED_RESCHED 对其他 CPU 可见；与
	 * resched_curr() 中“设置 resched 位并测试 polling”的原子协议配对。若 waker
	 * 先发生，本 CPU 会读到 resched；若本 CPU 先发布，waker 会看到 polling。
	 */
	smp_mb__after_atomic();

	return unlikely(tif_need_resched());
}

/*
 * current_clr_polling_and_test() - 撤销 polling 后复查睡眠窗口中的重调度请求。
 *
 * 无参数；current 必须是本 CPU idle task且调用者正准备进入低功耗状态。不可
 * 睡眠。返回 true 表示竞态窗口已有工作，调用者应跳过 idle；false 表示后续
 * waker 将因 polling=0 发送 IPI，可安全执行睡眠指令。无 ownership 变化。
 */
static __always_inline bool __must_check current_clr_polling_and_test(void)
{
	__current_clr_polling();

	/*
	 * Polling state must be visible before we test NEED_RESCHED,
	 * paired by resched_curr()
	 */
	/*
	 * 英文说明：清 polling 必须先于 NEED_RESCHED 复查可见。屏障与 resched_curr()
	 * 配对，排除“waker 看到旧 polling 而省 IPI、idle 又看到旧 resched 而睡下”。
	 */
	smp_mb__after_atomic();

	return unlikely(tif_need_resched());
}

/*
 * current_clr_polling() - 在退出 idle 轮询时清位并折叠抢占重调度状态。
 *
 * 入参、返回值均无；current 必须是本 CPU idle task，函数不可睡眠。清位后屏障
 * 保证未来 waker 发送 IPI，并把 thread flag 折叠进 PREEMPT_NEED_RESCHED，供
 * 随后的抢占/调度检查使用。它不消费重调度请求，也不转移 ownership。
 */
static __always_inline void current_clr_polling(void)
{
	__current_clr_polling();

	/*
	 * Ensure we check TIF_NEED_RESCHED after we clear the polling bit.
	 * Once the bit is cleared, we'll get IPIs with every new
	 * TIF_NEED_RESCHED and the IPI handler, scheduler_ipi(), will also
	 * fold.
	 */
	/*
	 * 英文说明：polling 清除后每个新的 NEED_RESCHED 都会触发 IPI，且 IPI handler
	 * scheduler_ipi() 也会执行 fold；这里的屏障和本地 fold 覆盖没有 IPI 的旧请求。
	 */
	smp_mb__after_atomic(); /* paired with resched_curr() */
	/* 上述行尾英文指出此屏障与 resched_curr() 的原子发布协议配对。 */

	preempt_fold_need_resched();
}

#else
/*
 * 架构没有 TIF_POLLING_NRFLAG 时，idle CPU 从不向 waker 承诺主动轮询；底层置位
 * helper 因而为空，正确性依赖 resched_curr() 总能按非 polling 路径发送通知。
 */
/* 无参数、无返回、无副作用且不睡眠；仅为跨架构统一调用接口。 */
static inline void __current_set_polling(void) { }
/* 无参数、无返回、无副作用且不睡眠；该配置没有 polling 位可清。 */
static inline void __current_clr_polling(void) { }

/*
 * current_set_polling_and_test() - 无 polling 位配置下直接读取 need_resched。
 *
 * 无参数、不可睡眠、无副作用；返回当前瞬时重调度状态。__must_check 要求调用者
 * 据此决定是否进入 idle，waker 通知不依赖本函数发布任何位。
 */
static inline bool __must_check current_set_polling_and_test(void)
{
	return unlikely(tif_need_resched());
}
/*
 * current_clr_polling_and_test() - 无 polling 位配置下直接复查 need_resched。
 *
 * 无参数、不可睡眠、无副作用；true 时调用者跳过 idle，false 时依赖架构正常
 * 中断通知唤醒。返回值必须检查。
 */
static inline bool __must_check current_clr_polling_and_test(void)
{
	return unlikely(tif_need_resched());
}

/*
 * current_clr_polling() - 无 polling 位配置下建立全屏障并折叠重调度状态。
 *
 * 无参数、无返回且不睡眠。空清位 helper 后使用 smp_mb()，因为没有原子 bitop
 * 可供 smp_mb__after_atomic() 依附；再把 thread flag 折叠到抢占状态。
 */
static __always_inline void current_clr_polling(void)
{
	__current_clr_polling();

	smp_mb(); /* paired with resched_curr() */
	/* 上述全屏障与 resched_curr() 配对，维持有/无 polling 位配置的一致顺序保证。 */

	preempt_fold_need_resched();
}
#endif

#endif /* _LINUX_SCHED_IDLE_H */
