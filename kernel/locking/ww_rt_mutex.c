// SPDX-License-Identifier: GPL-2.0-only
/*
 * rtmutex API
 */
/* 本文件为 PREEMPT_RT 生成并包装基于 rtmutex 的 ww_mutex API。 */
#include <linux/spinlock.h>
#include <linux/export.h>

#define RT_MUTEX_BUILD_MUTEX
#define WW_RT
/* 这两个生成开关让 rtmutex.c 选择 mutex 包装形态，并实例化 ww_mutex.h 的 RT 后端。 */
#include "rtmutex.c"

/*
 * ww_mutex_trylock - 不等待地尝试取得 RT 后端 ww_mutex
 * @lock: 已初始化且保持存活的 ww_mutex
 * @ww_ctx: 可选有效 acquire context
 *
 * 无 ctx 时委托 public rt_mutex_trylock()；有 ctx 时重置零持锁 context 的 wounded，再尝试内部
 * rtmutex。成功发布 ww context 与 lockdep nest 并返回 1，竞争返回 0；成功必须 ww_mutex_unlock()。
 */
int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ww_ctx)
{
	struct rt_mutex *rtm = &lock->base;

	if (!ww_ctx)
		return rt_mutex_trylock(rtm);

	/*
	 * Reset the wounded flag after a kill. No other process can
	 * race and wound us here, since they can't have a valid owner
	 * pointer if we don't have any locks held.
	 */
	/* acquired 为零时没有有效 owner 能引用并并发 wound 此 context，因此可重置 wounded。 */
	if (ww_ctx->acquired == 0)
		ww_ctx->wounded = 0;

	if (__rt_mutex_trylock(&rtm->rtmutex)) {
		ww_mutex_set_context_fastpath(lock, ww_ctx);
		mutex_acquire_nest(&rtm->dep_map, 0, 1, &ww_ctx->dep_map, _RET_IP_);
		return 1;
	}

	return 0;
}
EXPORT_SYMBOL(ww_mutex_trylock);

/*
 * __ww_rt_mutex_lock - 统一执行 RT 后端 ww_mutex 的可睡眠获取
 * @lock: 已初始化且在调用及成功持有期间保持存活的 ww_mutex
 * @ww_ctx: 可选 acquire context
 * @state: TASK_UNINTERRUPTIBLE 或 TASK_INTERRUPTIBLE
 * @ip: lockdep 归因地址
 *
 * 可睡眠任务上下文调用。先拒绝同 context 重入、重置 wounded 并登记 lockdep；RT fast acquire
 * 成功时发布 context，失败则进入带 PI 与 ww 检查的 slowlock。返回 0 时 current 持锁，负错误时
 * 撤销 lockdep 且不持锁；nest_lock/rtm 都是本栈帧借用别名，ret 传递慢路径结果。
 */
static int __sched
__ww_rt_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ww_ctx,
		   unsigned int state, unsigned long ip)
{
	struct lockdep_map __maybe_unused *nest_lock = NULL;
	struct rt_mutex *rtm = &lock->base;
	int ret;

	might_sleep();

	if (ww_ctx) {
		if (unlikely(ww_ctx == READ_ONCE(lock->ctx)))
			return -EALREADY;

		/*
		 * Reset the wounded flag after a kill. No other process can
		 * race and wound us here, since they can't have a valid owner
		 * pointer if we don't have any locks held.
		 */
		/* acquired 为零时没有有效 owner 可并发 wound，安全开始新一轮 context 使用。 */
		if (ww_ctx->acquired == 0)
			ww_ctx->wounded = 0;

#ifdef CONFIG_DEBUG_LOCK_ALLOC
		nest_lock = &ww_ctx->dep_map;
#endif
	}
	mutex_acquire_nest(&rtm->dep_map, 0, 0, nest_lock, ip);

	if (likely(rt_mutex_try_acquire(&rtm->rtmutex))) {
		if (ww_ctx)
			ww_mutex_set_context_fastpath(lock, ww_ctx);
		return 0;
	}

	ret = rt_mutex_slowlock(&rtm->rtmutex, ww_ctx, state);

	if (ret)
		mutex_release(&rtm->dep_map, ip);
	return ret;
}

/*
 * ww_mutex_lock - 不可中断地获取 RT 后端 ww_mutex
 * @lock: 已初始化且保持存活的 ww_mutex
 * @ctx: 可选 acquire context
 *
 * 把 TASK_UNINTERRUPTIBLE 和调用地址交给共享实现；返回 0 时持锁，ww 错误时不持锁。
 */
int __sched
ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __ww_rt_mutex_lock(lock, ctx, TASK_UNINTERRUPTIBLE, _RET_IP_);
}
EXPORT_SYMBOL(ww_mutex_lock);

/*
 * ww_mutex_lock_interruptible - 可被信号打断地获取 RT 后端 ww_mutex
 * @lock: 已初始化且保持存活的 ww_mutex
 * @ctx: 可选 acquire context
 *
 * 以 TASK_INTERRUPTIBLE 调用共享实现；返回 0 时持锁，-EINTR/-EDEADLK/-EALREADY 时不持锁。
 */
int __sched
ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __ww_rt_mutex_lock(lock, ctx, TASK_INTERRUPTIBLE, _RET_IP_);
}
EXPORT_SYMBOL(ww_mutex_lock_interruptible);

/*
 * ww_mutex_unlock - 释放 RT 后端 ww_mutex
 * @lock: current 持有且保持存活的 ww_mutex
 *
 * 先递减 ww acquired 并清 ctx，再登记 lockdep release，最后由内部 rtmutex 解锁完成 PI waiter 交接。
 * 无返回值；返回时 current 不再持锁，对象/context 存储仍归调用者。
 */
void __sched ww_mutex_unlock(struct ww_mutex *lock)
	__no_context_analysis
{
	struct rt_mutex *rtm = &lock->base;

	__ww_mutex_unlock(lock);

	mutex_release(&rtm->dep_map, _RET_IP_);
	__rt_mutex_unlock(&rtm->rtmutex);
}
EXPORT_SYMBOL(ww_mutex_unlock);
