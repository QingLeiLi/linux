// SPDX-License-Identifier: GPL-2.0
/*
 * stop-task scheduling class.
 *
 * The stop task is the highest priority task in the system, it preempts
 * everything and will be preempted by nothing.
 *
 * See kernel/stop_machine.c
 */
/*
 * stop task 调度类：系统中的最高优先级内核调度实体。
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 原文说明 stop task 会抢占系统中的一切任务且不会被任何任务抢占；完整的 per-CPU
 * stopper 工作提交、线程和 CPU 热插拔生命周期见 kernel/stop_machine.c。本文件只提供
 * sched_class 适配层，不创建 stopper 线程，也不管理 cpu_stop_work 队列。
 *
 * 每个 CPU 的 rq->stop 指向固定 stopper kthread。stop_machine/stop_one_cpu 把工作排入
 * 对应 CPU 的 stopper 队列并唤醒它；sched_set_stop_task() 把线程切到本类。stop task
 * 从不迁移、yield、被常规策略切入或改变优先级，因此这些“不可能”入口用 BUG 暴露协议
 * 破坏。rq 锁串行化入队、出队、选择和运行记账；stop callback 另在禁止抢占的进程上下文
 * 执行且不得睡眠。
 */
#include "sched.h"

/*
 * select_task_rq_stop() - 保持 stopper 固定在其所属 CPU
 * @p: 纯输入、不可为 NULL 的 stopper task，借用且由 stopper 生命周期保活。
 * @cpu: 通用选核层给出的候选 CPU；本类不采用。
 * @flags: wake/migrate 选核原因；本类不采用。
 * 返回 p 当前绑定 CPU。调用者已在唤醒/迁移协议中稳定 task_cpu；无副作用、不睡眠。
 */
static int
select_task_rq_stop(struct task_struct *p, int cpu, int flags)
{
	return task_cpu(p); /* stop tasks as never migrate */
	/* 原文强调 stop task 永不迁移；返回绑定 CPU 是该 per-CPU 独占关系的直接实现。 */
}

/*
 * balance_stop() - 告知核心本 rq 是否存在可运行 stopper
 * @rq: 纯输入、不可为 NULL 的借用运行队列，调用者持 rq 锁。
 * @rf: 通用 balance 锁状态，当前实现不消费也不解锁。
 * 返回 sched_stop_runnable() 的布尔结果；只在 rq->stop 存在且已 queued 时为真，无副作用。
 */
static int
balance_stop(struct rq *rq, struct rq_flags *rf)
{
	return sched_stop_runnable(rq);
}

/*
 * wakeup_preempt_stop() - 表达 stopper 已经处于不可被更高类抢占的位置
 * @rq: 借用目标 rq；@p: 借用新唤醒 stopper；@flags: 未消费的 wakeup 标志。
 * 返回：无直接返回值，也不设置 resched。stop 是链接顺序最高的类，不存在更高类可使
 * 当前 stopper 的 wakeup-preempt 决策发生变化；调用者持 rq 锁且本函数不可睡眠。
 */
static void
wakeup_preempt_stop(struct rq *rq, struct task_struct *p, int flags)
{
	/* we're never preempted */
	/* 原文说明本类永不被抢占，因此无需比较优先级或触发重新调度。 */
}

/*
 * set_next_task_stop() - 在 stopper 被选中运行时建立本轮执行起点
 * @rq: 输入输出当前运行队列，调用者持 rq 锁且时钟已更新。
 * @stop: 输入输出、即将成为 curr 的 stopper；不转移 task ownership。
 * @first: 是否首次 set_next；本类无需区分。
 * 返回：无直接返回值。把 task 时钟 ns 快照写入 stop->se.exec_start，供退出运行时记账。
 */
static void set_next_task_stop(struct rq *rq, struct task_struct *stop, bool first)
{
	stop->se.exec_start = rq_clock_task(rq);
}

/*
 * pick_task_stop() - 从最高调度类选择本 CPU 唯一 stopper
 * @rq: 纯输入借用运行队列，调用者持 rq 锁。
 * @rf: 通用选择路径的 rq 锁状态，本实现不消费。
 * 返回 rq->stop 借用指针表示应运行；未安装或未 queued 时返回 NULL，让核心继续扫描下一类。
 * 不改变队列、引用或 ownership，不能睡眠。
 */
static struct task_struct *pick_task_stop(struct rq *rq, struct rq_flags *rf)
{
	/* 安装指针本身不代表 runnable，必须同时确认 stopper 已在 rq 上排队。 */
	if (!sched_stop_runnable(rq))
		return NULL;

	return rq->stop;
}

/*
 * enqueue_task_stop() - 把唯一 stopper 计入 rq 的总 runnable 数
 * @rq: 输入输出目标运行队列，调用者持 rq 锁；@p: 已成为 runnable 的借用 stopper；
 * @flags: 通用 enqueue 原因，本类不需要区分。
 * 返回：无直接返回值。仅增加 rq->nr_running；stopper 身份由 rq->stop 固定，无私有队列可插入。
 */
static void
enqueue_task_stop(struct rq *rq, struct task_struct *p, int flags)
{
	add_nr_running(rq, 1);
}

/*
 * dequeue_task_stop() - 从 rq 总 runnable 数移除 stopper
 * @rq: 输入输出目标运行队列，调用者持 rq 锁；@p: 借用 stopper；@flags: 未消费的 dequeue 原因。
 * 返回 true，向核心确认 dequeue 已完成；副作用仅是 nr_running 减一，无资源释放或失败路径。
 */
static bool
dequeue_task_stop(struct rq *rq, struct task_struct *p, int flags)
{
	sub_nr_running(rq, 1);
	return true;
}

/*
 * yield_task_stop() - 拒绝 stopper 主动让出 CPU
 * @rq: 借用当前运行队列，调用者持锁；实现不读取它。
 * 返回：正常情况下不返回，直接 BUG。stop callback 必须完成或退出，若允许 yield，低优先级
 * task 可能在“机器应停止”的窗口运行并破坏 stop_machine 排他保证。
 */
static void yield_task_stop(struct rq *rq)
{
	BUG(); /* the stop task should never yield, its pointless. */
	/* 原文说明 stop task 永远不应 yield，这种请求没有意义，故视为内核协议错误。 */
}

/*
 * put_prev_task_stop() - stopper 离开 CPU 前结算本轮通用执行时间
 * @rq: 输入输出当前 rq，调用者持锁且时钟已更新。
 * @prev: 即将离开的借用 stopper；@next: 后继借用 task；二者由切换路径保活且实现无需解引用。
 * 返回：无直接返回值。update_curr_common() 以 exec_start 结算 task/rq 统计及 DL server 传播；
 * 不重新入队、不释放 task、不可睡眠。
 */
static void put_prev_task_stop(struct rq *rq, struct task_struct *prev, struct task_struct *next)
{
	update_curr_common(rq);
}

/*
 * scheduler tick hitting a task of our scheduling class.
 *
 * NOTE: This function can be called remotely by the tick offload that
 * goes along full dynticks. Therefore no local assumption can be made
 * and everything must be accessed through the @rq and @curr passed in
 * parameters.
 */
/*
 * 原文说明：当调度 tick 命中本类 task 时进入该回调。full dynticks 的 tick offload 可以
 * 从远端 CPU 调用，所以不能假定“当前 CPU 就是 @rq 所属 CPU”；所有状态必须经传入的
 * @rq 和 @curr 访问。
 *
 * task_tick_stop() - 处理 stopper 的调度 tick（当前为空操作）
 * @rq: tick 对应的借用 rq；@curr: 该 rq 的借用 stopper；@queued: 是否来自排队 tick 语境。
 * 返回：无直接返回值。stopper 无时间片、不会被同类轮转，故不修改状态；支持远端调用且不睡眠。
 */
static void task_tick_stop(struct rq *rq, struct task_struct *curr, int queued)
{
}

/*
 * switching_to_stop() - 捕获普通策略切换非法进入 stop 类
 * @rq: 借用且已锁定的 task rq；@p: 输入输出目标 task，但正常路径绝不会到达。
 * 返回：正常情况下不返回，直接 BUG。只有 sched_set_stop_task() 可直接安装此类，通用
 * sched_setscheduler 不得构造 stopper。
 */
static void switching_to_stop(struct rq *rq, struct task_struct *p)
{
	BUG(); /* its impossible to change to this class */
	/* 原文说明通过普通类切换进入 stop 在设计上不可能，触发即表示核心不变量被破坏。 */
}

/*
 * prio_changed_stop() - 验证 stopper 的有效优先级从未被改变
 * @rq: 借用并已锁定的 rq；@p: 输入 stopper；@oldprio: 改变前的内部优先级数值。
 * 返回：值未变时快速返回；真正变化则 BUG。stop 类排序由链接顺序而非动态 prio 决定，
 * PI/用户策略都不应改写它。
 */
static void
prio_changed_stop(struct rq *rq, struct task_struct *p, u64 oldprio)
{
	if (p->prio == oldprio)
		return;

	BUG(); /* how!?, what priority? */
	/* 原文质问这种变化如何发生以及应赋何种优先级，表明它没有可恢复语义。 */
}

/*
 * update_curr_stop() - 满足 sched_class 的当前 task 更新接口
 * @rq: 借用且已锁定的当前 rq。
 * 返回：无直接返回值且无副作用；stopper 的通用统计在 put_prev_task_stop() 统一结算，
 * 本类没有时间片、带宽或虚拟运行时间需要每次更新。
 */
static void update_curr_stop(struct rq *rq)
{
}

/*
 * Simple, special scheduling class for the per-CPU stop tasks:
 */
/*
 * 原文说明这是服务于 per-CPU stop task 的简单专用调度类。
 *
 * DEFINE_SCHED_CLASS(stop) 把常量操作表放入链接脚本排序的专属 section；stop section 位于
 * DL/RT/fair/idle 之前，因此核心扫描时天然拥有最高优先级。表中回调均借用 rq/task，rq 锁
 * 由调度核心持有；未提供的普通 task 生命周期/负载均衡操作对 stopper 不适用。
 */
DEFINE_SCHED_CLASS(stop) = {
	/* runnable 变化只维护 rq 总数，唯一实体由 rq->stop 直接定位。 */
	.enqueue_task		= enqueue_task_stop,
	.dequeue_task		= dequeue_task_stop,
	.yield_task		= yield_task_stop,

	/* 最高类不需要 wakeup 优先级比较。 */
	.wakeup_preempt		= wakeup_preempt_stop,

	/* pick 返回固定 stopper；切换边界只记录/结算通用 task 时钟。 */
	.pick_task		= pick_task_stop,
	.put_prev_task		= put_prev_task_stop,
	.set_next_task          = set_next_task_stop,

	/* stopper 固定 CPU，balance 只报告 runnable，不实施迁移。 */
	.balance		= balance_stop,
	.select_task_rq		= select_task_rq_stop,
	.set_cpus_allowed	= set_cpus_allowed_common,

	/* 无时间片轮转，tick 回调为空。 */
	.task_tick		= task_tick_stop,

	/* 动态切入或改优先级均是 BUG；update_curr 本类无私有状态。 */
	.prio_changed		= prio_changed_stop,
	.switching_to		= switching_to_stop,
	.update_curr		= update_curr_stop,
};
