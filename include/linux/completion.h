/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_COMPLETION_H
#define __LINUX_COMPLETION_H

/*
 * (C) Copyright 2001 Linus Torvalds
 *
 * Atomic wait-for-completion handler data structures.
 * See kernel/sched/completion.c for details.
 */

/*
 * 本头文件定义 completion 的对象布局、初始化方式与公开 API；状态转换的实现
 * 位于 kernel/sched/completion.c。所谓 atomic 是“等待状态与通知不会丢失”，
 * 并不表示 wait_for_completion*() 不会睡眠：等待端必须运行在可调度上下文，
 * 而 complete*() 和非阻塞查询可以用于原子/中断上下文。
 */

#include <linux/swait.h>

/*
 * struct completion - structure used to maintain state for a "completion"
 *
 * This is the opaque structure used to maintain the state for a "completion".
 * Completions currently use a FIFO to queue threads that have to wait for
 * the "completion" event.
 *
 * See also:  complete(), wait_for_completion() (and friends _timeout,
 * _interruptible, _interruptible_timeout, and _killable), init_completion(),
 * reinit_completion(), and macros DECLARE_COMPLETION(),
 * DECLARE_COMPLETION_ONSTACK().
 */
/*
 * completion 对象由使用它的上层对象创建和销毁，本身没有引用计数：调用者必须
 * 保证 producer、所有 waiter 和查询者结束后才能释放或重新初始化。
 *
 * @done: 受 @wait.lock 保护的完成令牌数；0 为未完成，有限正数逐次消费，
 *     UINT_MAX 为 complete_all() 的永久完成哨兵。无锁 READ_ONCE 只作提示。
 * @wait: FIFO simple-wait 队列，其 raw spinlock 同时串行化 @done 与队列变化；
 *     队列节点通常由等待者放在自己的栈上，返回前必须摘除。
 *
 * 英文所列 API 共同遵循“发布完成前写结果，成功等待后读结果”的同步协议。
 * completion 不是互斥锁；成功 waiter 不获得需要随后释放的临界区所有权。
 */
struct completion {
	unsigned int done;
	struct swait_queue_head wait;
};

/*
 * init_completion_map() 当前忽略 lockdep map 参数并转调 init_completion()；保留
 * 两参数形式让带/不带 lockdep 的调用点使用同一宏接口。@x 由调用者持有，@m
 * 不被求值为独立状态，返回语义与 init_completion() 相同。
 */
#define init_completion_map(x, m) init_completion(x)

/*
 * complete_acquire()/complete_release() 是等待实现围绕临界段调用的语义钩子。
 * 当前配置下均为空：@x 是不可空借用输入，无返回和副作用，不取得引用、不睡眠；
 * 真正的锁与内存序仍由 x->wait.lock 及 wakeup/schedule 协议完成。
 */
/* complete_acquire() 借用不可空 @x；当前无返回/副作用且不睡眠。 */
static inline void complete_acquire(struct completion *x) {}
/* complete_release() 借用同一 @x；当前无返回/副作用且不睡眠。 */
static inline void complete_release(struct completion *x) {}

/*
 * COMPLETION_INITIALIZER() 供静态存储期对象使用：把 done 置 0，并用对象名构造
 * swait 队列头和锁。@work 必须是正在声明的 completion 标识符；结果是初始化
 * 表达式，不分配内存也不睡眠，对象 ownership 仍属于声明者。
 */
#define COMPLETION_INITIALIZER(work) \
	{ 0, __SWAIT_QUEUE_HEAD_INITIALIZER((work).wait) }

/*
 * 两个 ONSTACK 初始化表达式先在运行期初始化栈对象，再返回该对象本身用于声明。
 * 语句表达式只在声明点同步执行；MAP 版本保留 lockdep map 形参，普通版本直接
 * 使用 init_completion()。离开作用域前必须保证没有 producer/waiter 再访问。
 */
#define COMPLETION_INITIALIZER_ONSTACK_MAP(work, map) \
	(*({ init_completion_map(&(work), &(map)); &(work); }))

#define COMPLETION_INITIALIZER_ONSTACK(work) \
	(*({ init_completion(&work); &work; }))

/**
 * DECLARE_COMPLETION - declare and initialize a completion structure
 * @work:  identifier for the completion structure
 *
 * This macro declares and initializes a completion structure. Generally used
 * for static declarations. You should use the _ONSTACK variant for automatic
 * variables.
 */
/*
 * 上述英文说明：DECLARE_COMPLETION() 同时声明并初始化名为 @work 的 completion，
 * 适合静态/全局对象；自动栈变量应使用 DECLARE_COMPLETION_ONSTACK()，让 lockdep
 * 获得每次栈实例的运行期初始化。宏无返回值，声明者拥有对象生命周期。
 */
#define DECLARE_COMPLETION(work) \
	struct completion work = COMPLETION_INITIALIZER(work)

/*
 * Lockdep needs to run a non-constant initializer for on-stack
 * completions - so we use the _ONSTACK() variant for those that
 * are on the kernel stack:
 */
/*
 * lockdep 需要为每个栈上 completion 执行非常量初始化，才能正确跟踪具体锁类；
 * 因此不能无条件复用静态 initializer。对象仍由当前栈帧拥有，宏不延长生命期。
 */
/**
 * DECLARE_COMPLETION_ONSTACK - declare and initialize a completion structure
 * @work:  identifier for the completion structure
 *
 * This macro declares and initializes a completion structure on the kernel
 * stack.
 */
/*
 * 上述英文说明：@work 是要在当前内核栈帧中声明并初始化的对象标识符。
 * CONFIG_LOCKDEP=y 时走运行期 ONSTACK 初始化（MAP 版本还接受调用者的 map），
 * 关闭时退化为普通静态形式；两种配置的 done/wait 行为相同。宏不睡眠、无错误
 * 返回，调用者必须保证栈帧结束前所有异步完成端和 waiter 都已退出。
 */
#ifdef CONFIG_LOCKDEP
# define DECLARE_COMPLETION_ONSTACK(work) \
	struct completion work = COMPLETION_INITIALIZER_ONSTACK(work)
# define DECLARE_COMPLETION_ONSTACK_MAP(work, map) \
	struct completion work = COMPLETION_INITIALIZER_ONSTACK_MAP(work, map)
#else
# define DECLARE_COMPLETION_ONSTACK(work) DECLARE_COMPLETION(work)
# define DECLARE_COMPLETION_ONSTACK_MAP(work, map) DECLARE_COMPLETION(work)
#endif

/**
 * init_completion - Initialize a dynamically allocated completion
 * @x:  pointer to completion structure that is to be initialized
 *
 * This inline function will initialize a dynamically created completion
 * structure.
 */
/*
 * init_completion() - 把动态/嵌入式 completion 初始化为“尚未完成”。
 *
 * @x 是不可空的输入/输出借用指针，由调用者拥有；入口对象尚未对并发使用者
 * 发布，且不存在旧 waiter。任意上下文均可执行，不睡眠。返回无直接值；把
 * done 置 0 并完整初始化 swait 队列头及其锁。成功后上层才可发布 @x。
 * 对已在使用的对象再次调用会破坏队列，复用只能遵循 reinit_completion() 契约。
 */
static inline void init_completion(struct completion *x)
{
	/* 初始化顺序发生在对象发布前，因此此处无需取得尚未可见的 wait.lock。 */
	x->done = 0;
	init_swait_queue_head(&x->wait);
}

/**
 * reinit_completion - reinitialize a completion structure
 * @x:  pointer to completion structure that is to be reinitialized
 *
 * This inline function should be used to reinitialize a completion structure so it can
 * be reused. This is especially important after complete_all() is used.
 */
/*
 * reinit_completion() - 保留等待队列结构，仅把完成状态重新置为未完成。
 *
 * @x 是不可空的输入/输出借用对象，调用者拥有其生命周期。函数不睡眠、无返回，
 * 只写 done=0，不初始化或清空 wait 队列，也不取得 wait.lock。因此调用者必须
 * 用外层协议证明没有 complete*() 并发执行，且 complete_all() 唤醒的全部旧
 * waiter 已结束；否则会丢失令牌、让旧新两代 waiter 混在同一队列或导致竞态。
 */
static inline void reinit_completion(struct completion *x)
{
	x->done = 0;
}

/*
 * 等待 API 均借用 @x 且可能睡眠；调用者保证对象跨等待存活。无后缀版本无限且
 * 不可中断；_io 还进行 I/O-wait 记账。成功会消费有限令牌，UINT_MAX 不递减。
 */
/* 无限不可中断等待；借用参数，可能睡眠，返回无值。 */
extern void wait_for_completion(struct completion *);
/* 无限不可中断 I/O 记账等待；借用参数，可能睡眠，返回无值。 */
extern void wait_for_completion_io(struct completion *);

/*
 * 可中断无限等待：interruptible 可被普通信号打断，killable 仅响应致命信号，
 * state 由调用者提供合法 task state。均返回 0 或 -ERESTARTSYS，不转移 ownership。
 */
/* 普通信号可中断的无限等待；@x 借用，可能睡眠。 */
extern int wait_for_completion_interruptible(struct completion *x);
/* 仅致命信号可中断的无限等待；@x 借用，可能睡眠。 */
extern int wait_for_completion_killable(struct completion *x);
/* @state 决定信号规则的无限等待；@x 借用，可能睡眠。 */
extern int wait_for_completion_state(struct completion *x, unsigned int state);

/*
 * timeout 族的 @timeout 单位为 jiffies；返回 0 表示超时，正数表示完成及可能的
 * 剩余时间，可中断版本还返回 -ERESTARTSYS。_io 版本仅改变等待记账。
 */
/* 不可中断有期限等待；@timeout 是 jiffies 输入，函数可能睡眠。 */
extern unsigned long wait_for_completion_timeout(struct completion *x,
						   unsigned long timeout);
/* 不可中断且按 I/O 记账的有期限等待；函数可能睡眠。 */
extern unsigned long wait_for_completion_io_timeout(struct completion *x,
						    unsigned long timeout);
/* 普通信号可中断的有期限等待；还可能返回 -ERESTARTSYS。 */
extern long wait_for_completion_interruptible_timeout(
	struct completion *x, unsigned long timeout);
/* 致命信号可中断的有期限等待；还可能返回 -ERESTARTSYS。 */
extern long wait_for_completion_killable_timeout(
	struct completion *x, unsigned long timeout);
/*
 * try_wait_for_completion() 不睡眠，true 时消费有限令牌；completion_done() 只
 * 查询当前完成状态并与发布者锁区同步，不消费令牌，也不能用于统计 waiter。
 */
/* 非阻塞尝试消费；@x 借用，任意上下文可用，返回是否成功。 */
extern bool try_wait_for_completion(struct completion *x);
/* 非消费式查询；@x 借用，任意上下文可用，返回瞬时完成状态。 */
extern bool completion_done(struct completion *x);

/*
 * 完成端 API 均借用 @x、不可睡眠且可用于原子上下文：complete() 发布一个令牌，
 * current_cpu 版本增加唤醒放置提示，complete_all() 发布 UINT_MAX 并广播唤醒。
 */
/* 发布一个令牌并至多唤醒一个 waiter；@x 借用，无返回。 */
extern void complete(struct completion *);
/* 发布一个令牌并提示当前 CPU 唤醒放置；@x 借用，无返回。 */
extern void complete_on_current_cpu(struct completion *x);
/* 发布永久完成并唤醒全部 waiter；@x 借用，无返回。 */
extern void complete_all(struct completion *);

#endif
