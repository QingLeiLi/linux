/* SPDX-License-Identifier: GPL-2.0 */

/*
 * 调度器自动分组内部接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 桌面负载启用 CONFIG_SCHED_AUTOGROUP 后，会把仍属于 root CPU cgroup、尚未退出的
 * 同一 signal_struct 线程组映射到一个自动创建的 CFS task_group，使不同会话先按组
 * 分享 CPU，而不是让一个拥有大量线程的会话直接压过其他会话。真实 cgroup 优先：
 * task 已进入非 root task_group 时不应用 autogroup；RT task 也被重定向到 root RT 组，
 * 避免动态维护 RT 带宽层级。
 *
 * signal->autogroup 持有引用，fork/exit 与 attach/detach 负责 get/put；最后一个可能
 * 使用者释放引用时，先从调度组 RCU 索引摘除 tg，再延迟销毁。任务移动由 siglock
 * 稳定线程列表、由每个 task 的 rq 锁更新 sched_task_group。查询映射允许与组切换
 * 短暂竞态，但切换方在放弃旧引用前会再次 sched_move_task()，使状态最终收敛。
 *
 * 本头文件只定义内部对象和配置无关调用面；autogroup.c 管理创建、引用与 proc nice，
 * core.c 执行实际调度实体迁移。关闭配置时所有入口退化为无副作用 stub，并保持原 tg。
 */
#ifndef _KERNEL_SCHED_AUTOGROUP_H
#define _KERNEL_SCHED_AUTOGROUP_H

#include "sched.h"

#ifdef CONFIG_SCHED_AUTOGROUP

/*
 * 一个会话自动组及其所拥有的 CFS task_group。
 *
 * kref 统计“仍可能使用该组”的 signal/task 路径引用，而不是当前附着线程数；最后一次
 * put 触发 autogroup_destroy()，后者摘除并延迟销毁 tg，最终由 autogroup_free() 释放
 * 反向指向它的本对象。tg 成功创建后持有 ag 的反向指针。lock 只串行化 proc 写入 nice/
 * shares 与 proc 读取；它不保护 signal->autogroup，后者由 siglock 和 kref 协议保护。
 * id 是原子递增的展示标识，nice 是 [-20, 19] 的组权重用户表示。
 */
struct autogroup {
	/*
	 * Reference doesn't mean how many threads attach to this
	 * autogroup now. It just stands for the number of tasks
	 * which could use this autogroup.
	 */
	/*
	 * 该引用不是“当前附着线程数”，而是仍可能经 signal->autogroup 使用本对象的任务
	 * 路径数量；因此线程迁移和对象最终释放不是简单的一进一出计数。
	 */
	struct kref		kref;
	struct task_group	*tg;
	struct rw_semaphore	lock;
	unsigned long		id;
	int			nice;
};

/*
 * autogroup_init() - 启动期建立永久默认组并让 init_task 的 signal 持有它
 * @init_task: 输入输出、不可为 NULL 的初始 task；函数借用指针，不取得 task 引用。
 * 返回：无直接返回值。初始化默认组的 tg/kref/rwsem，发布 signal->autogroup，并注册
 * 可选 sysctl。仅在调度器启动期调用，可以睡眠；默认对象为静态存储，不走普通释放。
 */
extern void autogroup_init(struct task_struct *init_task);

/*
 * autogroup_free() - 在 task_group RCU 销毁末段释放其 autogroup 容器
 * @tg: 输入、不可为 NULL、已摘除且无并发用户的 task_group，借用至函数返回；其
 *      autogroup 反向指针可为 NULL。
 * 返回：无直接返回值。释放 tg->autogroup（NULL 安全）；tg 自身由调度组销毁路径另行释放。
 */
extern void autogroup_free(struct task_group *tg);

/*
 * task_group_is_autogroup() - 判断 task_group 是否带自动组反向关联
 * @tg: 纯输入、不可为 NULL 的稳定借用指针；调用者负责其 RCU/锁生命周期。
 * 返回 true 表示 tg->autogroup 非 NULL，false 表示普通/root 组；无副作用、不会睡眠。
 */
static inline bool task_group_is_autogroup(struct task_group *tg)
{
	return !!tg->autogroup;
}

/*
 * task_wants_autogroup() - 判断 task 当前是否应以 signal 自动组覆盖 cgroup 组
 * @p: 纯输入、不可为 NULL 的稳定 task；不取得引用。
 * @tg: 纯输入的当前 CPU cgroup task_group；非 root 时始终保留真实 cgroup。
 * 返回 true 仅表示 tg 为 root 且 p 未设置 PF_EXITING。允许与组移动并发，切换方会在
 * 放弃旧 autogroup 引用前再次迁移任务；函数不睡眠、不修改状态。
 */
extern bool task_wants_autogroup(struct task_struct *p, struct task_group *tg);

/*
 * autogroup_task_group() - 为调度器选择 task 的有效 task_group
 * @p: 纯输入、不可为 NULL 的 task；调用者通常已用 task rq 锁稳定迁移状态。
 * @tg: 纯输入、借用的 CPU cgroup 结果，也是关闭/不适用 autogroup 时的返回值。
 * 返回借用的 tg，或在 sysctl 开启且 task_wants_autogroup() 成立时返回
 * p->signal->autogroup->tg；不增加引用、不能越过调用者的锁/生命周期边界。READ_ONCE
 * 只防止 sysctl 编译器重读，不为 signal 指针提供快照；函数不睡眠且无直接副作用。
 */
static inline struct task_group *
autogroup_task_group(struct task_struct *p, struct task_group *tg)
{
	extern unsigned int sysctl_sched_autogroup_enabled;
	int enabled = READ_ONCE(sysctl_sched_autogroup_enabled);

	if (enabled && task_wants_autogroup(p, tg))
		return p->signal->autogroup->tg;

	return tg;
}

/*
 * autogroup_path() - 把自动组格式化为调度组展示路径
 * @tg: 纯输入、不可为 NULL 的稳定 task_group；非自动组返回 0 且不写缓冲区。
 * @buf: 输出缓冲区，不可为 NULL，ownership 保持在调用者。
 * @buflen: 缓冲区字节数；传给 snprintf，返回值可能大于等于该值表示发生截断。
 * 返回 0 表示非自动组，否则返回完整路径所需字符数（不含结尾 NUL）；不分配内存，
 * 读取不可变 id，调用者须保证 tg/ag 生命周期。
 */
extern int autogroup_path(struct task_group *tg, char *buf, int buflen);

#else /* !CONFIG_SCHED_AUTOGROUP: */
/* 未编译自动分组时，以下 stub 保持调用者无需散布条件编译，且绝不改变 task/group。 */

/* @init_task 被忽略；返回：无直接返回值，无分配、发布或睡眠。 */
static inline void autogroup_init(struct task_struct *init_task) {  }
/* @tg 被忽略；返回：无直接返回值，普通 task_group 由原销毁路径处理。 */
static inline void autogroup_free(struct task_group *tg) { }

/* @tg 为稳定借用输入；关闭配置时没有自动组，恒返回 false，无副作用。 */
static inline bool task_group_is_autogroup(struct task_group *tg)
{
	return 0;
}

/* @p 被忽略；返回调用者传入的借用 @tg，保持真实 CPU cgroup 映射，无副作用。 */
static inline struct task_group *
autogroup_task_group(struct task_struct *p, struct task_group *tg)
{
	return tg;
}

/* @tg/@buf/@buflen 均不消费；返回 0 表示没有可展示的自动组路径，不写缓冲区。 */
static inline int autogroup_path(struct task_group *tg, char *buf, int buflen)
{
	return 0;
}

#endif /* !CONFIG_SCHED_AUTOGROUP */
/* 上述分支分别提供真实实现接口或语义等价 stub，使调用点保持配置无关。 */

#endif /* _KERNEL_SCHED_AUTOGROUP_H */
/* 结束本头文件重复包含保护。 */
