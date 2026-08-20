/* SPDX-License-Identifier: GPL-2.0 */

/*
 * 本头文件定义通用 SMP 回调框架与调度器之间的内部边界：远端唤醒请求和普通
 * smp_call_function 请求共用目标 CPU 的 call_single_queue，但在消费时分别进入
 * sched_ttwu_pending() 和通用回调执行路径。这里只声明跨文件契约，不拥有队列，
 * 也不为调用者取得 task 或 call_single_data 的引用。
 */
#ifndef _KERNEL_SCHED_SMP_H
#define _KERNEL_SCHED_SMP_H

/*
 * Scheduler internal SMP callback types and methods between the scheduler
 * and other internal parts of the core kernel:
 */
/*
 * 这里汇集调度器与内核其他核心组件之间使用的 SMP 回调类型和方法。主要连接为：
 * 唤醒方/跨 CPU 回调发送方 → 每 CPU call_single_queue → 目标 CPU IPI 或 idle/迁移
 * 主动冲刷 → 调度入队或普通回调执行。
 */
/* bool 等基础类型来自目标内核类型体系，避免依赖调用者的间接包含顺序。 */
#include <linux/types.h>

/*
 * sched_ttwu_pending() - 在目标 CPU 上批量接收远端 task 唤醒节点
 *
 * 调用位置：kernel/smp.c 排空 call_single_queue 时最后处理 CSD_TYPE_TTWU 节点，
 * 把整条无锁链表交给本函数；通常处于目标 CPU 的 IPI、关本地中断上下文。
 * @arg: 纯输入的 struct llist_node 链表头，节点嵌在待唤醒 task 中；允许为 NULL，
 *       此时直接返回。排队协议保证消费期间 task 存活，本函数不长期持有其引用。
 * 返回：无直接返回值。非空时取得本 CPU rq 锁，等待旧 on_cpu 发布完成，逐项激活
 * task，并在至少完成入队后清 rq->ttwu_pending；不能睡眠。链表节点的队列归属在
 * 消费时结束，task 随后由运行队列管理；本次节点被消费完成前不得重复提交。
 */
extern void sched_ttwu_pending(void *arg);

/*
 * call_function_single_prep_ipi() - 判断单目标 SMP 回调是否还需发送硬件 IPI
 *
 * 调用位置：kernel/smp.c 已把回调排入 @cpu 的 call_single_queue 后、调用体系结构
 * 发 IPI 之前。
 * @cpu: 目标 CPU 编号，调用者保证其对应的永久 idle task/per-CPU rq 可访问；纯输入。
 * 返回 true 表示目标未处于可由 need_resched 唤醒的 polling idle，调用者必须继续
 * 发送 IPI；返回 false 表示已原子设置 polling idle task 的重调度标志，目标会主动
 * 冲刷队列，调用者必须省略 IPI。函数不睡眠、不转移对象所有权；false 路径会记录
 * wake-idle-without-IPI tracepoint。
 */
extern bool call_function_single_prep_ipi(int cpu);

#ifdef CONFIG_SMP
/*
 * flush_smp_call_function_queue() - 在当前 CPU 的任务上下文主动排空 SMP 回调队列
 *
 * 调用位置：polling idle 省略 IPI 后、进入 schedule_idle() 前，以及 migration
 * stopper 真正迁移 task 前。入参：无；只处理当前 CPU 的 per-CPU 队列。
 * 函数可在可抢占性受调用者约束的任务上下文进入，内部保存并关闭本地中断，且不
 * 睡眠；它按 SYNC、ASYNC/IRQ_WORK、TTWU 次序消费节点，并在 PREEMPT_RT 场景补做
 * 新增 softirq 处理。返回：无直接返回值；空队列无副作用，非空时执行回调并恢复
 * 原中断状态。各 CSD/task 的生命周期仍遵守各自排队协议，本函数不保留引用。
 */
extern void flush_smp_call_function_queue(void);
#else
/*
 * 单处理器配置没有远端 CPU 和 SMP call_single_queue；保持同名空 stub 让 idle/迁移
 * 调用点无需条件编译。入参：无。返回：无直接返回值；不睡眠且没有任何副作用。
 */
static inline void flush_smp_call_function_queue(void) { }
#endif

#endif /* _KERNEL_SCHED_SMP_H */
