// SPDX-License-Identifier: GPL-2.0-only
/*
 * async.c: Asynchronous function calls for boot performance
 * async.c: 用于提升启动性能的异步函数调用机制
 *
 * (C) Copyright 2009 Intel Corporation
 * Author: Arjan van de Ven <arjan@linux.intel.com>
 */


/*

Goals and Theory of Operation
目标和运行原理

The primary goal of this feature is to reduce the kernel boot time,
by doing various independent hardware delays and discovery operations
decoupled and not strictly serialized.
本特性的主要目标是减少内核启动时间，通过将各种独立的硬件延迟和发现操作解耦，
使它们不必严格串行化执行。

More specifically, the asynchronous function call concept allows
certain operations (primarily during system boot) to happen
asynchronously, out of order, while these operations still
have their externally visible parts happen sequentially and in-order.
(not unlike how out-of-order CPUs retire their instructions in order)
更具体地说，异步函数调用概念允许某些操作（主要是系统启动期间）以异步、乱序的方式发生，
但这些操作的外部可见部分仍然按顺序、有序地发生。
（类似于乱序执行CPU以顺序方式提交指令的机制）

Key to the asynchronous function call implementation is the concept of
a "sequence cookie" (which, although it has an abstracted type, can be
thought of as a monotonically incrementing number).
异步函数调用实现的关键是"序列cookie"的概念（虽然它有抽象类型，但可以理解为单调递增的数字）。

The async core will assign each scheduled event such a sequence cookie and
pass this to the called functions.
异步核心会为每个调度的事件分配这样一个序列cookie，并将其传递给被调用的函数。

The asynchronously called function should before doing a globally visible
operation, such as registering device numbers, call the
async_synchronize_cookie() function and pass in its own cookie. The
async_synchronize_cookie() function will make sure that all asynchronous
operations that were scheduled prior to the operation corresponding with the
cookie have completed.
异步调用的函数在执行全局可见操作（例如注册设备号）之前，应该调用
async_synchronize_cookie() 函数并传入自己的cookie。
async_synchronize_cookie() 函数将确保在该cookie对应操作之前调度的所有异步操作都已完成。

Subsystem/driver initialization code that scheduled asynchronous probe
functions, but which shares global resources with other drivers/subsystems
that do not use the asynchronous call feature, need to do a full
synchronization with the async_synchronize_full() function, before returning
from their init function. This is to maintain strict ordering between the
asynchronous and synchronous parts of the kernel.
调度了异步探测函数，但与其他不使用异步调用特性的驱动程序/子系统共享全局资源的
子系统/驱动程序初始化代码，需要在从其init函数返回之前使用async_synchronize_full()
函数进行完全同步。这是为了在内核的异步和同步部分之间保持严格的顺序。

*/

#include <linux/async.h>        // 异步操作框架的头文件
#include <linux/atomic.h>       // 原子操作（线程安全的计数器等）
#include <linux/export.h>       // 导出符号供模块使用
#include <linux/ktime.h>        // 内核时间操作
#include <linux/pid.h>          // 进程ID相关
#include <linux/sched.h>        // 进程调度相关
#include <linux/slab.h>         // 内存分配（kmalloc等）
#include <linux/wait.h>         // 等待队列
#include <linux/workqueue.h>    // 工作队列机制

#include "workqueue_internal.h" // 工作队列内部接口

/*
 * next_cookie: 下一个要分配的cookie值，初始为1
 * cookie用于标识和跟踪异步操作的顺序
 * 每次调度新的异步任务时会递增
 */
static async_cookie_t next_cookie = 1;

/*
 * MAX_WORK: 最大待处理工作项数量
 * 当待处理工作超过此数量时，新的异步调用会退化为同步执行
 * 这是为了防止内存耗尽
 */
#define MAX_WORK		32768

/*
 * ASYNC_COOKIE_MAX: 无穷大cookie值
 * 用于等待所有异步操作完成的场景
 */
#define ASYNC_COOKIE_MAX	ULLONG_MAX	/* infinity cookie */
                                                /* 无限大cookie */

/*
 * async_global_pending: 全局待处理列表
 * 包含所有已注册域（domain）的待处理异步操作
 * 用于全局同步操作
 */
static LIST_HEAD(async_global_pending);	/* pending from all registered doms */
                                        /* 来自所有已注册域的待处理任务 */

/*
 * async_dfl_domain: 默认异步域
 * 当不指定特定域时，异步操作会加入此默认域
 */
static ASYNC_DOMAIN(async_dfl_domain);

/*
 * async_lock: 保护异步子系统数据结构的自旋锁
 * 用于保护cookie分配、链表操作等关键区域
 */
static DEFINE_SPINLOCK(async_lock);

/*
 * async_wq: 专用工作队列
 * 所有异步操作都在此工作队列上执行
 */
static struct workqueue_struct *async_wq;

/*
 * struct async_entry: 异步任务条目结构
 * 描述一个待执行或正在执行的异步任务
 */
struct async_entry {
	struct list_head	domain_list;  // 链接到所属域的链表节点
	struct list_head	global_list;  // 链接到全局链表的节点
	struct work_struct	work;         // 工作队列项，用于实际执行
	async_cookie_t		cookie;       // 此任务的唯一标识cookie
	async_func_t		func;         // 要执行的异步函数
	void			*data;        // 传递给函数的数据指针
	struct async_domain	*domain;      // 此任务所属的域
};

/*
 * async_done: 等待队列头
 * 当异步任务完成时会唤醒等待在此队列上的进程
 */
static DECLARE_WAIT_QUEUE_HEAD(async_done);

/*
 * entry_count: 当前活动的异步任务计数
 * 使用原子操作保证线程安全
 * 用于限制并发异步任务数量
 */
static atomic_t entry_count;

/*
 * microseconds_since - 计算从start到现在经过的微秒数
 * @start: 起始时间点
 *
 * 返回值: 经过的微秒数
 *
 * 实现细节:
 * 1. 获取当前时间
 * 2. 计算时间差并转换为纳秒
 * 3. 右移10位将纳秒转换为微秒（除以1024，约等于1000）
 *
 * 注意: 使用位移而非除法是性能优化，但会有约2.4%的误差
 */
static long long microseconds_since(ktime_t start)
{
	ktime_t now = ktime_get();  // 获取当前内核时间
	return ktime_to_ns(ktime_sub(now, start)) >> 10;  // 转为纳秒后右移10位得到微秒
}

/*
 * lowest_in_progress - 查找域中最早的待处理任务cookie
 * @domain: 要查询的域，NULL表示查询全局列表
 *
 * 返回值: 最小的待处理cookie，如果没有待处理任务则返回ASYNC_COOKIE_MAX
 *
 * 设计原因:
 * - 用于判断某个cookie之前的所有任务是否已完成
 * - 支持细粒度的同步控制（可按域或全局查询）
 *
 * 注意事项:
 * - 必须在持有async_lock时访问链表
 * - 使用中断安全的自旋锁以支持中断上下文调用
 */
static async_cookie_t lowest_in_progress(struct async_domain *domain)
{
	struct async_entry *first = NULL;  // 指向第一个任务的指针
	async_cookie_t ret = ASYNC_COOKIE_MAX;  // 默认返回最大值（表示无待处理任务）
	unsigned long flags;  // 保存中断状态

	spin_lock_irqsave(&async_lock, flags);  // 加锁并禁用中断

	if (domain) {
		/* 查询特定域的待处理列表 */
		if (!list_empty(&domain->pending))
			first = list_first_entry(&domain->pending,
					struct async_entry, domain_list);
	} else {
		/* 查询全局待处理列表 */
		if (!list_empty(&async_global_pending))
			first = list_first_entry(&async_global_pending,
					struct async_entry, global_list);
	}

	if (first)
		ret = first->cookie;  // 链表第一个元素的cookie就是最小的（因为按顺序插入）

	spin_unlock_irqrestore(&async_lock, flags);  // 解锁并恢复中断状态
	return ret;
}

/*
 * pick the first pending entry and run it
 * 选择第一个待处理条目并运行它
 */
/*
 * async_run_entry_fn - 异步任务的实际执行函数
 * @work: 工作队列项指针
 *
 * 这是所有异步任务的统一入口点，由工作队列调用
 *
 * 执行流程:
 * 1. 从work_struct获取async_entry结构
 * 2. 打印调试信息并记录开始时间
 * 3. 执行用户提供的异步函数
 * 4. 打印执行耗时
 * 5. 从待处理队列中移除自己
 * 6. 释放entry内存并减少计数
 * 7. 唤醒等待的进程
 *
 * 设计原因:
 * - 统一的执行封装便于调试和监控
 * - 自动管理生命周期避免内存泄漏
 * - 及时唤醒等待者提高响应性
 *
 * 注意: 此函数在工作队列上下文执行，可能睡眠
 */
static void async_run_entry_fn(struct work_struct *work)
{
	struct async_entry *entry =
		container_of(work, struct async_entry, work);  // 通过work成员获取包含它的entry
	unsigned long flags;
	ktime_t calltime;  // 记录函数调用开始时间

	/* 1) run (and print duration) */
	/* 1) 运行（并打印持续时间） */
	pr_debug("calling  %lli_%pS @ %i\n", (long long)entry->cookie,
		 entry->func, task_pid_nr(current));  // 打印: cookie_函数名 @ 进程PID
	calltime = ktime_get();

	entry->func(entry->data, entry->cookie);  // 调用用户提供的异步函数

	pr_debug("initcall %lli_%pS returned after %lld usecs\n",
		 (long long)entry->cookie, entry->func,
		 microseconds_since(calltime));  // 打印执行耗时

	/* 2) remove self from the pending queues */
	/* 2) 从待处理队列中移除自己 */
	spin_lock_irqsave(&async_lock, flags);
	list_del_init(&entry->domain_list);  // 从域列表移除
	list_del_init(&entry->global_list);  // 从全局列表移除

	/* 3) free the entry */
	/* 3) 释放条目 */
	kfree(entry);  // 释放内存
	atomic_dec(&entry_count);  // 减少活动任务计数

	spin_unlock_irqrestore(&async_lock, flags);

	/* 4) wake up any waiters */
	/* 4) 唤醒任何等待者 */
	wake_up(&async_done);  // 唤醒所有等待异步完成的进程
}

/*
 * __async_schedule_node_domain - 内部实现：在指定NUMA节点和域上调度异步任务
 * @func:   要异步执行的函数
 * @data:   传递给函数的数据指针
 * @node:   期望运行的NUMA节点编号（NUMA_NO_NODE 表示不指定）
 * @domain: 所属的异步域，用于分组同步
 * @entry:  调用者已分配好的 async_entry 结构
 *
 * 返回值: 分配给此任务的 async_cookie_t，可用于后续同步
 *
 * 设计原因:
 * - 将内存分配（可能失败）与实际调度逻辑分离
 * - 调用者负责分配 entry，此函数只负责初始化和入队
 * - 这样可以支持不同分配策略（GFP_ATOMIC / GFP_KERNEL 等）
 *
 * 执行流程:
 * 1. 初始化 entry 的各字段
 * 2. 持锁分配 cookie 并加入两个链表
 * 3. 提交 work 到工作队列
 *
 * 注意: 调用此函数前 entry 必须已经分配，函数接管其生命周期
 */
static async_cookie_t __async_schedule_node_domain(async_func_t func,
						   void *data, int node,
						   struct async_domain *domain,
						   struct async_entry *entry)
{
	async_cookie_t newcookie;
	unsigned long flags;

	INIT_LIST_HEAD(&entry->domain_list);           // 初始化域链表节点
	INIT_LIST_HEAD(&entry->global_list);           // 初始化全局链表节点
	INIT_WORK(&entry->work, async_run_entry_fn);   // 绑定工作队列回调
	entry->func = func;
	entry->data = data;
	entry->domain = domain;

	spin_lock_irqsave(&async_lock, flags);

	/* allocate cookie and queue */
	/* 分配cookie并加入队列 */
	newcookie = entry->cookie = next_cookie++;  // 原子分配cookie（持锁保护）

	list_add_tail(&entry->domain_list, &domain->pending);  // 加入域的待处理列表尾部
	if (domain->registered)
		list_add_tail(&entry->global_list, &async_global_pending);  // 若域已注册，也加入全局列表

	atomic_inc(&entry_count);  // 增加活动任务计数
	spin_unlock_irqrestore(&async_lock, flags);

	/* schedule for execution */
	/* 提交到工作队列执行 */
	queue_work_node(node, async_wq, &entry->work);  // 尽量调度到指定NUMA节点

	return newcookie;
}

/**
 * async_schedule_node_domain - NUMA specific version of async_schedule_domain
 * @func: function to execute asynchronously
 * @data: data pointer to pass to the function
 * @node: NUMA node that we want to schedule this on or close to
 * @domain: the domain
 *
 * Returns an async_cookie_t that may be used for checkpointing later.
 * @domain may be used in the async_synchronize_*_domain() functions to
 * wait within a certain synchronization domain rather than globally.
 *
 * Note: This function may be called from atomic or non-atomic contexts.
 *
 * The node requested will be honored on a best effort basis. If the node
 * has no CPUs associated with it then the work is distributed among all
 * available CPUs.
 */
/*
 * async_schedule_node_domain - async_schedule_domain 的 NUMA 感知版本
 * @func:   要异步执行的函数，原型为 void func(void *data, async_cookie_t cookie)
 * @data:   透传给 func 的任意指针（设备结构、配置块等）
 * @node:   期望运行的 NUMA 节点；NUMA_NO_NODE 表示不指定
 * @domain: 同步域，可用于 async_synchronize_*_domain() 等待一组任务
 *
 * 返回值: 分配给此次调度的 async_cookie_t
 *         可传入 async_synchronize_cookie() 等待此任务及其之前任务完成
 *
 * 降级策略（内存不足 或 待处理任务过多时）:
 * - 仍然分配一个 cookie 以维持全局顺序
 * - 直接在调用者上下文同步执行 func，不再异步
 * - 这确保功能正确性，即使性能有所下降
 *
 * 注意: 可在原子上下文（中断、spinlock持有期间）调用，内部使用 GFP_ATOMIC
 */
async_cookie_t async_schedule_node_domain(async_func_t func, void *data,
					  int node, struct async_domain *domain)
{
	struct async_entry *entry;
	unsigned long flags;
	async_cookie_t newcookie;

	/* allow irq-off callers */
	/* 允许在关中断上下文调用，GFP_ATOMIC 不会睡眠 */
	entry = kzalloc_obj(struct async_entry, GFP_ATOMIC);

	/*
	 * If we're out of memory or if there's too much work
	 * pending already, we execute synchronously.
	 */
	/*
	 * 如果内存不足或待处理工作过多，则同步执行
	 * MAX_WORK 限制是为了防止系统资源耗尽
	 */
	if (!entry || atomic_read(&entry_count) > MAX_WORK) {
		kfree(entry);  // entry 可能为 NULL，kfree(NULL) 是安全的
		spin_lock_irqsave(&async_lock, flags);
		newcookie = next_cookie++;  // 即使同步执行也要分配cookie维持顺序
		spin_unlock_irqrestore(&async_lock, flags);

		/* low on memory.. run synchronously */
		/* 内存不足，同步运行 */
		func(data, newcookie);
		return newcookie;
	}

	return __async_schedule_node_domain(func, data, node, domain, entry);
}
EXPORT_SYMBOL_GPL(async_schedule_node_domain);  // 导出为GPL专用符号，供内核模块使用

/**
 * async_schedule_node - NUMA specific version of async_schedule
 * @func: function to execute asynchronously
 * @data: data pointer to pass to the function
 * @node: NUMA node that we want to schedule this on or close to
 *
 * Returns an async_cookie_t that may be used for checkpointing later.
 * Note: This function may be called from atomic or non-atomic contexts.
 *
 * The node requested will be honored on a best effort basis. If the node
 * has no CPUs associated with it then the work is distributed among all
 * available CPUs.
 */
/*
 * async_schedule_node - async_schedule 的 NUMA 感知版本
 * @func: 要异步执行的函数
 * @data: 传递给函数的数据指针
 * @node: 期望运行的 NUMA 节点
 *
 * 与 async_schedule_node_domain 的区别:
 * - 自动使用默认域 async_dfl_domain
 * - 适合不需要精细域控制的简单用例
 *
 * 返回值: 可用于同步检查点的 async_cookie_t
 */
async_cookie_t async_schedule_node(async_func_t func, void *data, int node)
{
	return async_schedule_node_domain(func, data, node, &async_dfl_domain);
}
EXPORT_SYMBOL_GPL(async_schedule_node);

/**
 * async_schedule_dev_nocall - A simplified variant of async_schedule_dev()
 * @func: function to execute asynchronously
 * @dev: device argument to be passed to function
 *
 * @dev is used as both the argument for the function and to provide NUMA
 * context for where to run the function.
 *
 * If the asynchronous execution of @func is scheduled successfully, return
 * true. Otherwise, do nothing and return false, unlike async_schedule_dev()
 * that will run the function synchronously then.
 */
/*
 * async_schedule_dev_nocall - async_schedule_dev() 的简化变体
 * @func: 要异步执行的函数
 * @dev:  设备指针，同时作为:
 *        1. 传递给 func 的参数
 *        2. 确定 NUMA 本地性的来源（dev_to_node）
 *
 * 与 async_schedule_dev() 的关键区别:
 * - 调度失败时返回 false，不降级为同步执行
 * - 调用者可自行决定失败时的处理策略
 * - 适用于"调度成功最好，失败也无妨"的场景
 *
 * 返回值: true  - 成功调度异步执行
 *         false - 内存不足或任务队列已满，未执行
 */
bool async_schedule_dev_nocall(async_func_t func, struct device *dev)
{
	struct async_entry *entry;

	entry = kzalloc_obj(struct async_entry);  // 使用默认 GFP_KERNEL 分配，可能睡眠

	/* Give up if there is no memory or too much work. */
	/* 内存不足或工作过多时直接放弃，不降级同步执行 */
	if (!entry || atomic_read(&entry_count) > MAX_WORK) {
		kfree(entry);
		return false;
	}

	__async_schedule_node_domain(func, dev, dev_to_node(dev),
				     &async_dfl_domain, entry);
	return true;
}

/**
 * async_synchronize_full - synchronize all asynchronous function calls
 *
 * This function waits until all asynchronous function calls have been done.
 */
/*
 * async_synchronize_full - 等待所有异步函数调用完成
 *
 * 阻塞当前进程，直到全局所有域中的所有异步任务都执行完毕
 *
 * 典型用途:
 * - 驱动/子系统 init 函数返回前调用，确保其调度的异步任务已完成
 * - 与不使用异步机制的代码共享资源时，必须先调用此函数
 *
 * 注意: 此函数可能睡眠，不能在原子上下文调用
 */
void async_synchronize_full(void)
{
	async_synchronize_full_domain(NULL);  // NULL 表示等待全局所有域
}
EXPORT_SYMBOL_GPL(async_synchronize_full);

/**
 * async_synchronize_full_domain - synchronize all asynchronous function within a certain domain
 * @domain: the domain to synchronize
 *
 * This function waits until all asynchronous function calls for the
 * synchronization domain specified by @domain have been done.
 */
/*
 * async_synchronize_full_domain - 等待特定域内所有异步调用完成
 * @domain: 要同步的域；NULL 表示全局所有域
 *
 * 与 async_synchronize_full() 的区别:
 * - 可以只等待特定域的任务，粒度更细
 * - 适合只关心自己调度任务的子系统
 *
 * 实现: 传入 ASYNC_COOKIE_MAX 等待"无穷大"之前的所有任务
 */
void async_synchronize_full_domain(struct async_domain *domain)
{
	async_synchronize_cookie_domain(ASYNC_COOKIE_MAX, domain);
}
EXPORT_SYMBOL_GPL(async_synchronize_full_domain);

/**
 * async_synchronize_cookie_domain - synchronize asynchronous function calls within a certain domain with cookie checkpointing
 * @cookie: async_cookie_t to use as checkpoint
 * @domain: the domain to synchronize (%NULL for all registered domains)
 *
 * This function waits until all asynchronous function calls for the
 * synchronization domain specified by @domain submitted prior to @cookie
 * have been done.
 */
/*
 * async_synchronize_cookie_domain - 等待指定域内某cookie之前的所有异步调用完成
 * @cookie: 同步检查点，等待所有编号小于此cookie的任务完成
 * @domain: 要同步的域；NULL 表示所有已注册域
 *
 * 这是最底层的同步原语，其他 async_synchronize_* 函数都基于此实现
 *
 * 实现原理:
 * - 使用 wait_event() 等待条件满足
 * - 条件: 域中最小的待处理cookie >= 目标cookie
 *   （意味着目标cookie之前的所有任务已完成）
 *
 * 注意:
 * - 可能长时间睡眠，不能在原子上下文调用
 * - 调试模式下会打印等待时间
 */
void async_synchronize_cookie_domain(async_cookie_t cookie, struct async_domain *domain)
{
	ktime_t starttime;

	pr_debug("async_waiting @ %i\n", task_pid_nr(current));  // 调试: 记录等待开始
	starttime = ktime_get();

	/*
	 * wait_event(): 睡眠等待直到条件为真
	 * 条件: 域中最小的待处理cookie >= 目标cookie
	 * 每次有任务完成（wake_up(&async_done)）时都会重新检查条件
	 */
	wait_event(async_done, lowest_in_progress(domain) >= cookie);

	pr_debug("async_continuing @ %i after %lli usec\n", task_pid_nr(current),
		 microseconds_since(starttime));  // 调试: 打印等待耗时
}
EXPORT_SYMBOL_GPL(async_synchronize_cookie_domain);

/**
 * async_synchronize_cookie - synchronize asynchronous function calls with cookie checkpointing
 * @cookie: async_cookie_t to use as checkpoint
 *
 * This function waits until all asynchronous function calls prior to @cookie
 * have been done.
 */
/*
 * async_synchronize_cookie - 等待某cookie之前的所有异步调用完成（默认域）
 * @cookie: 同步检查点
 *
 * 用法示例:
 *   async_cookie_t my_cookie = async_schedule(my_func, data);
 *   // ... 做其他工作 ...
 *   async_synchronize_cookie(my_cookie + 1);  // 等待 my_func 及其之前的任务完成
 *
 * 与 async_synchronize_cookie_domain 的区别:
 * - 只等待默认域 async_dfl_domain 中的任务
 */
void async_synchronize_cookie(async_cookie_t cookie)
{
	async_synchronize_cookie_domain(cookie, &async_dfl_domain);
}
EXPORT_SYMBOL_GPL(async_synchronize_cookie);

/**
 * current_is_async - is %current an async worker task?
 *
 * Returns %true if %current is an async worker task.
 */
/*
 * current_is_async - 判断当前进程是否是异步工作者任务
 *
 * 返回值: true  - 当前代码运行在异步工作队列的上下文中
 *         false - 不是
 *
 * 用途:
 * - 避免在异步任务内部调用会死锁的同步操作
 * - 例如: 异步任务内调用 async_synchronize_full() 可能导致死锁
 *
 * 实现原理:
 * - 获取当前工作队列 worker 结构
 * - 检查其正在执行的函数是否是 async_run_entry_fn
 */
bool current_is_async(void)
{
	struct worker *worker = current_wq_worker();  // 获取当前的工作队列worker

	return worker && worker->current_func == async_run_entry_fn;
}
EXPORT_SYMBOL_GPL(current_is_async);

/*
 * async_init - 初始化异步执行子系统
 *
 * 在内核启动早期调用（__init 标记），创建专用工作队列
 *
 * 设计原因:
 * - 使用专用工作队列而非系统工作队列，避免与其他工作竞争
 * - WQ_UNBOUND: 不绑定到特定CPU，可在任意CPU运行，提高并行度
 * - 提高 min_active 到 WQ_DFL_ACTIVE: 允许更多并发工作项
 *   （默认 min_active=8 不足以处理相互依赖的大量启动任务）
 *
 * BUG_ON: 若分配失败则内核崩溃——async是启动必需组件，无法降级
 */
void __init async_init(void)
{
	/*
	 * Async can schedule a number of interdependent work items. However,
	 * unbound workqueues can handle only upto min_active interdependent
	 * work items. The default min_active of 8 isn't sufficient for async
	 * and can lead to stalls. Let's use a dedicated workqueue with raised
	 * min_active.
	 */
	/*
	 * 异步框架可能调度大量相互依赖的工作项。unbound工作队列最多只能同时处理
	 * min_active 个相互依赖的工作项。默认的 min_active=8 对于异步启动不够用，
	 * 可能导致停顿。因此使用提高了 min_active 的专用工作队列。
	 */
	async_wq = alloc_workqueue("async", WQ_UNBOUND, 0);
	BUG_ON(!async_wq);  // 分配失败则panic，启动过程不可缺少
	workqueue_set_min_active(async_wq, WQ_DFL_ACTIVE);  // 提高并发上限
}
