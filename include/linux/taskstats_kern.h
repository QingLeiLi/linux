/* SPDX-License-Identifier: GPL-2.0 */
/*
 * taskstats 内核侧接口学习导读
 *
 * 本头文件连接进程生命周期与 kernel/taskstats.c：启动阶段创建统计对象
 * cache，线程退出阶段累计/发送快照，signal_struct 最终释放时回收 TGID
 * 累计对象。UAPI 数据布局位于 <linux/taskstats.h>，这里不重复定义 ABI。
 *
 * CONFIG_TASKSTATS=y 时声明真实入口；关闭时提供同签名空实现，使 fork/
 * exit/init 主路径无需散布条件编译。空实现不采集也不发送数据，编译器可
 * 将调用完全消除。
 */
/* taskstats_kern.h - kernel header for per-task statistics interface
 *
 * Copyright (C) Shailabh Nagar, IBM Corp. 2006
 *           (C) Balbir Singh,   IBM Corp. 2006
 */

#ifndef _LINUX_TASKSTATS_KERN_H
#define _LINUX_TASKSTATS_KERN_H

#include <linux/taskstats.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>

#ifdef CONFIG_TASKSTATS
/*
 * taskstats_cache 在 taskstats_init_early() 中以 SLAB_PANIC 创建，保存
 * signal_struct::stats 指向的 TGID 累计对象；对象随 signal_struct 回收。
 * taskstats_exit_mutex 是历史接口声明，当前源码树没有定义者或使用者，
 * 阅读本版本同步协议时不能据此推断退出路径由该 mutex 串行化。
 */
extern struct kmem_cache *taskstats_cache;
extern struct mutex taskstats_exit_mutex;

/*
 * taskstats_tgid_free() - 回收线程组共享的 taskstats 累计对象。
 *
 * @sig: 已到最终释放阶段的 signal_struct，调用者拥有最后生命周期控制权；
 *       函数借用指针，不释放 signal_struct 本身。
 *
 * 调用位置：free_signal_struct() 在释放 signal_struct 前调用。此时不会再有
 * 组内线程查询或累计 sig->stats，因此无需 siglock；若从未按需分配则为空
 * 操作。返回无直接值，非 NULL 对象归还 taskstats_cache，@sig->stats 随
 * signal_struct 一同失效，调用者随后继续释放其他组资源。
 */
static inline void taskstats_tgid_free(struct signal_struct *sig)
{
	if (sig->stats)
		kmem_cache_free(taskstats_cache, sig->stats);
}

/*
 * taskstats_exit() 由 do_exit() 在 mm/files 等资源仍可采集时调用；
 * @group_dead 指示当前任务是否为线程组最后一个存活成员。函数不把统计
 * 失败传播到退出协议。taskstats_init_early() 则由 start_kernel() 调用，
 * 在任务退出钩子可能使用 cache/per-CPU listener 表之前完成基础初始化。
 */
extern void taskstats_exit(struct task_struct *, int group_dead);
extern void taskstats_init_early(void);
#else
/*
 * 关闭 CONFIG_TASKSTATS 时，三个 stub 保持调用点和资源释放代码结构不变。
 * @tsk/@sig/@group_dead 均为借用输入且不会被访问；函数不睡眠、无返回值、
 * 无副作用，也不存在需要调用者清理的统计对象。
 */
static inline void taskstats_exit(struct task_struct *tsk, int group_dead)
{}
static inline void taskstats_tgid_free(struct signal_struct *sig)
{}
static inline void taskstats_init_early(void)
{}
#endif /* CONFIG_TASKSTATS */

#endif
