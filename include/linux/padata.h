/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * padata.h - header for the padata parallelization interface
 *
 * Copyright (C) 2008, 2009 secunet Security Networks AG
 * Copyright (C) 2008, 2009 Steffen Klassert <steffen.klassert@secunet.com>
 *
 * Copyright (c) 2020 Oracle and/or its affiliates.
 * Author: Daniel Jordan <daniel.m.jordan@oracle.com>
 */
/*
 * ============================================================================
 * 【padata - 并行数据处理框架（Parallel Data Processing Framework）】
 *
 * 【什么是 padata？】
 * padata（parallel data）是 Linux 内核的并行化框架，用于在多个 CPU 核心上
 * 并行处理数据任务，同时保证结果的顺序性。
 *
 * 【核心特性】
 * 1. 并行执行：将任务分发到多个 CPU 并行处理
 * 2. 顺序保证：即使并行处理，结果仍按提交顺序输出
 * 3. 负载均衡：自动在可用 CPU 间分配工作
 * 4. CPU 亲和性：支持指定 parallel 和 serial 工作的 CPU 集合
 *
 * 【工作流程】
 * 1. 提交阶段（padata_do_parallel）
 *    - 用户提交 padata_priv 任务
 *    - 分配序列号（保证顺序）
 *    - 将任务加入并行队列
 *
 * 2. 并行执行阶段（parallel 回调）
 *    - 多个 CPU 同时执行 parallel() 函数
 *    - 每个任务独立处理（无共享状态）
 *    - 处理完成后调用 padata_do_serial
 *
 * 3. 重排序阶段（reorder）
 *    - 按序列号对完成的任务排序
 *    - 确保结果按提交顺序输出
 *
 * 4. 串行化阶段（serial 回调）
 *    - 按顺序调用 serial() 函数
 *    - 处理最终结果（如写入输出）
 *    - 释放任务资源
 *
 * 【使用场景】
 * 1. 加密/解密：IPsec、disk encryption
 *    - 并行加密/解密数据包
 *    - 保证数据包顺序不变
 *
 * 2. 压缩/解压缩：文件系统、网络传输
 *    - 并行压缩数据块
 *    - 按顺序组装结果
 *
 * 3. 校验和计算：数据完整性检查
 *    - 并行计算不同块的校验和
 *    - 顺序合并结果
 *
 * 4. 数据转换：格式转换、编码转换
 *    - 并行处理数据记录
 *    - 保持记录顺序
 *
 * 5. 批量操作：页面迁移、内存初始化
 *    - 并行处理多个对象
 *    - 减少初始化时间
 *
 * 【两种使用模式】
 *
 * 1. 流式处理（padata_do_parallel / padata_do_serial）
 *    - 适合连续的数据流（如网络数据包）
 *    - 需要保持严格顺序
 *    - 示例：IPsec 加密
 *
 * 2. 批量处理（padata_do_multithreaded）
 *    - 适合一次性批量任务（如内存初始化）
 *    - 自动分片和负载均衡
 *    - 更简单的 API（无需手动管理序列号）
 *    - 示例：deferred struct page init
 *
 * 【性能优势】
 * - 多核利用率：充分利用多核 CPU
 * - 缓存友好：任务本地化到特定 CPU
 * - 低开销：避免不必要的锁竞争
 * - 可扩展：支持大规模 CPU 系统
 *
 * 【设计原则】
 * - 无锁并行：parallel 阶段无共享状态
 * - 顺序串行：serial 阶段保证顺序
 * - 灵活配置：可指定 parallel 和 serial 的 CPU 集合
 * - 动态调整：支持 CPU hotplug
 * ============================================================================
 */

#ifndef PADATA_H
#define PADATA_H

#include <linux/refcount.h>
#include <linux/compiler_types.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/kobject.h>

#define PADATA_CPU_SERIAL   0x01
/* CPU 类型标志：串行（serial）CPU
 * 用于 padata_set_cpumask() 指定要设置哪个 CPU 掩码
 */

#define PADATA_CPU_PARALLEL 0x02
/* CPU 类型标志：并行（parallel）CPU
 * 用于 padata_set_cpumask() 指定要设置哪个 CPU 掩码
 */

/**
 * struct padata_priv - Represents one job
 *
 * @list: List entry, to attach to the padata lists.
 * @pd: Pointer to the internal control structure.
 * @cb_cpu: Callback cpu for serializatioon.
 * @seq_nr: Sequence number of the parallelized data object.
 * @info: Used to pass information from the parallel to the serial function.
 * @parallel: Parallel execution function.
 * @serial: Serial complete function.
 */
/*
 * 【结构体】padata_priv - 表示一个并行任务
 *
 * 这是 padata 框架的核心数据结构，每个要并行处理的任务对应一个 padata_priv。
 */
struct padata_priv {
	struct list_head	list;
	/* 链表节点
	 * 用于将任务链入各种队列：
	 * - 并行队列（等待执行）
	 * - 重排序队列（等待按序输出）
	 * - 串行队列（等待 serial 回调）
	 */

	struct parallel_data	*pd;
	/* 指向控制结构
	 * 包含所有并行处理的状态信息（序列号、CPU 掩码等）
	 */

	int			cb_cpu;
	/* 串行化回调的目标 CPU
	 * serial() 函数将在该 CPU 上执行。
	 *
	 * 【为什么需要指定 CPU？】
	 * - 缓存局部性：如果 parallel 和 serial 在同一 CPU，性能更好
	 * - 负载均衡：可以分散 serial 工作到不同 CPU
	 * - 特殊需求：某些操作需要在特定 CPU 上执行
	 *
	 * 【如何设置？】
	 * padata_do_parallel() 的 cb_cpu 参数：
	 * - 指定 CPU 编号：在该 CPU 上执行 serial
	 * - -1：由 padata 自动选择（轮询）
	 */

	unsigned int		seq_nr;
	/* 序列号
	 * 用于保证输出顺序与提交顺序一致。
	 *
	 * 【分配方式】
	 * padata_do_parallel() 分配，单调递增（原子操作）。
	 *
	 * 【重排序】
	 * 即使任务并行完成顺序不同，serial() 也按 seq_nr 顺序调用。
	 */

	int			info;
	/* 信息字段
	 * 用于从 parallel 函数传递信息到 serial 函数。
	 *
	 * 【典型用途】
	 * - 错误码：parallel 处理失败时设置
	 * - 处理结果：简单的状态信息
	 * - 标志位：控制 serial 的行为
	 *
	 * 【注意】
	 * 如果需要传递复杂数据，应该将 padata_priv 嵌入到更大的结构中。
	 */

	void                    (*parallel)(struct padata_priv *padata);
	/* 并行执行函数
	 *
	 * 【调用时机】
	 * padata_do_parallel() 提交后，由 parallel_wq 工作队列调度执行。
	 *
	 * 【执行上下文】
	 * - 工作队列上下文（可睡眠）
	 * - 在 parallel CPU 集合中的某个 CPU 上
	 * - 多个任务可能并行执行（不同 CPU 上）
	 *
	 * 【函数职责】
	 * 1. 执行实际的并行计算（加密、压缩等）
	 * 2. 将结果存储到 padata_priv（或外层结构）
	 * 3. 调用 padata_do_serial(padata) 完成任务
	 *
	 * 【注意事项】
	 * - 不要访问共享的可变状态（竞态条件）
	 * - 必须调用 padata_do_serial，否则任务泄漏
	 * - 不要阻塞太久（影响其他任务）
	 *
	 * 【示例】
	 * void my_parallel(struct padata_priv *padata) {
	 *     struct my_job *job = container_of(padata, struct my_job, padata);
	 *     // 执行并行计算
	 *     job->result = compute(job->input);
	 *     // 通知完成
	 *     padata_do_serial(padata);
	 * }
	 */

	void                    (*serial)(struct padata_priv *padata);
	/* 串行完成函数
	 *
	 * 【调用时机】
	 * parallel() 调用 padata_do_serial() 后，由 serial_wq 调度执行。
	 * 重要：按 seq_nr 顺序调用，即使任务完成顺序不同。
	 *
	 * 【执行上下文】
	 * - 工作队列上下文（可睡眠）
	 * - 在 cb_cpu（或 serial CPU 集合中的 CPU）上
	 * - 串行执行（同一时刻只有一个 serial 在运行）
	 *
	 * 【函数职责】
	 * 1. 处理并行计算的结果（写入输出、更新状态）
	 * 2. 释放资源（kfree(job)）
	 * 3. 可选：触发后续操作（通知完成、唤醒等待者）
	 *
	 * 【注意事项】
	 * - 可以安全访问共享状态（串行执行保证）
	 * - 必须释放 padata_priv 及其外层结构
	 * - 尽量快速完成（阻塞后续 serial）
	 *
	 * 【示例】
	 * void my_serial(struct padata_priv *padata) {
	 *     struct my_job *job = container_of(padata, struct my_job, padata);
	 *     // 处理结果
	 *     output_result(job->result);
	 *     // 释放资源
	 *     kfree(job);
	 * }
	 */
};

/**
 * struct padata_list - one per work type per CPU
 *
 * @list: List head.
 * @lock: List lock.
 */
/*
 * 【结构体】padata_list - 每个 CPU 每种工作类型的队列
 *
 * padata 使用多个 per-CPU 队列来组织任务，避免锁竞争。
 */
struct padata_list {
	struct list_head        list;
	/* 链表头
	 * 存储 padata_priv 任务
	 */

	spinlock_t              lock;
	/* 自旋锁
	 * 保护链表的并发访问（添加/删除任务）
	 *
	 * 【为什么需要锁？】
	 * 虽然是 per-CPU 数据，但可能被其他 CPU 访问：
	 * - 任务分发：主 CPU 将任务放入目标 CPU 的队列
	 * - 重排序：检查多个 CPU 的完成状态
	 */
};

/**
* struct padata_serial_queue - The percpu padata serial queue
*
* @serial: List to wait for serialization after reordering.
* @work: work struct for serialization.
* @pd: Backpointer to the internal control structure.
*/
/*
 * 【结构体】padata_serial_queue - 每 CPU 的串行队列
 *
 * 每个 CPU 有一个串行队列，用于按序执行 serial 回调。
 */
struct padata_serial_queue {
       struct padata_list    serial;
       /* 串行任务链表
	* 存储等待执行 serial() 的任务（已完成 parallel）
	*/

       struct work_struct    work;
       /* 工作队列项
	* 用于在 serial_wq 上调度 serial() 执行
	*
	* 【工作流程】
	* 1. 任务完成 parallel，进入重排序队列
	* 2. 重排序后，按序加入 serial 队列
	* 3. 触发 work，在 serial_wq 上执行
	* 4. 工作函数依次调用每个任务的 serial()
	*/

       struct parallel_data *pd;
       /* 反向指针
	* 指向所属的 parallel_data 控制结构
	*/
};

/**
 * struct padata_cpumask - The cpumasks for the parallel/serial workers
 *
 * @pcpu: cpumask for the parallel workers.
 * @cbcpu: cpumask for the serial (callback) workers.
 */
/*
 * 【结构体】padata_cpumask - 并行/串行工作的 CPU 掩码
 *
 * 分离 parallel 和 serial 的 CPU 集合，提供灵活的负载分配。
 */
struct padata_cpumask {
	cpumask_var_t	pcpu;
	/* 并行 CPU 掩码
	 * parallel() 函数在这些 CPU 上执行。
	 *
	 * 【典型配置】
	 * - 所有 CPU：充分利用多核
	 * - 高性能核：在大小核系统中选择大核
	 * - 特定 NUMA 节点：减少远程内存访问
	 */

	cpumask_var_t	cbcpu;
	/* 串行（回调）CPU 掩码
	 * serial() 函数在这些 CPU 上执行。
	 *
	 * 【为什么分离？】
	 * 1. 负载隔离：parallel 是计算密集型，serial 可能是 I/O 密集型
	 * 2. 缓存优化：serial 可以在特定 CPU 上执行，提高缓存命中
	 * 3. 延迟优化：serial 可以在低延迟 CPU 上执行
	 *
	 * 【典型配置】
	 * - 与 pcpu 相同：不特殊区分
	 * - 少量 CPU：减少 serial 的并发度（简化同步）
	 * - 特定 CPU：固定在某些 CPU 上（如靠近 I/O 设备）
	 */
};

/**
 * struct parallel_data - Internal control structure, covers everything
 * that depends on the cpumask in use.
 *
 * @ps: padata_shell object.
 * @reorder_list: percpu reorder lists
 * @squeue: percpu padata queues used for serialuzation.
 * @refcnt: Number of objects holding a reference on this parallel_data.
 * @seq_nr: Sequence number of the parallelized data object.
 * @processed: Number of already processed objects.
 * @cpu: Next CPU to be processed.
 * @cpumask: The cpumasks in use for parallel and serial workers.
 */
/*
 * 【结构体】parallel_data - 内部控制结构
 *
 * 包含所有依赖于当前 CPU 掩码的状态。当 CPU 掩码改变时，创建新的 parallel_data。
 */
struct parallel_data {
	struct padata_shell		*ps;
	/* 指向 padata_shell
	 * shell 是外层包装，允许通过 RCU 替换 parallel_data
	 */

	struct padata_list		__percpu *reorder_list;
	/* 每 CPU 的重排序链表
	 * 存储已完成 parallel() 的任务，等待按 seq_nr 排序后执行 serial()。
	 *
	 * 【为什么需要重排序？】
	 * 任务并行执行，完成顺序不可预测，但 serial() 必须按提交顺序调用。
	 *
	 * 【重排序算法】
	 * 维护 processed 计数器（下一个期望的 seq_nr）。
	 * 扫描所有 CPU 的 reorder_list，找到 seq_nr == processed 的任务，
	 * 移到 serial 队列并递增 processed。
	 */

	struct padata_serial_queue	__percpu *squeue;
	/* 每 CPU 的串行队列
	 * 存储等待执行 serial() 的任务（已重排序）
	 */

	refcount_t			refcnt;
	/* 引用计数
	 * 每个持有 parallel_data 引用的对象（任务、shell）都会增加计数。
	 *
	 * 【生命周期】
	 * 1. 创建时 refcnt = 1（shell 持有）
	 * 2. 每个提交的任务增加 refcnt（padata_do_parallel）
	 * 3. 任务完成时减少 refcnt（serial 执行后）
	 * 4. refcnt 降为 0 时释放 parallel_data
	 */

	unsigned int			seq_nr;
	/* 序列号生成器
	 * 每次 padata_do_parallel() 调用时原子递增，分配给新任务。
	 *
	 * 【溢出处理】
	 * unsigned int 溢出后回绕到 0，但由于任务数量有限，
	 * 不会有 2^32 个任务同时存在，所以安全。
	 */

	unsigned int			processed;
	/* 已处理的任务数量
	 * 也是下一个期望执行 serial() 的 seq_nr。
	 *
	 * 【重排序关键】
	 * 只有 seq_nr == processed 的任务才能执行 serial()，
	 * 保证顺序性。
	 */

	int				cpu;
	/* 下一个要处理的 CPU
	 * 用于轮询分配任务到不同 CPU（负载均衡）。
	 *
	 * 【轮询算法】
	 * padata_do_parallel() 选择下一个 parallel CPU 时，
	 * 从 cpu 开始在 cpumask.pcpu 中查找下一个在线 CPU。
	 */

	struct padata_cpumask		cpumask;
	/* 当前使用的 CPU 掩码 */
};

/**
 * struct padata_shell - Wrapper around struct parallel_data, its
 * purpose is to allow the underlying control structure to be replaced
 * on the fly using RCU.
 *
 * @pinst: padat instance.
 * @pd: Actual parallel_data structure which may be substituted on the fly.
 * @opd: Pointer to old pd to be freed by padata_replace.
 * @list: List entry in padata_instance list.
 */
/*
 * 【结构体】padata_shell - parallel_data 的包装器
 *
 * 【设计目的】
 * 允许在运行时替换 parallel_data（如 CPU 掩码改变），而无需停止正在执行的任务。
 *
 * 【RCU 保护】
 * parallel_data 通过 RCU 保护，读者无锁访问，写者延迟释放。
 */
struct padata_shell {
	struct padata_instance		*pinst;
	/* 指向所属的 padata 实例 */

	struct parallel_data __rcu	*pd;
	/* 当前的 parallel_data（RCU 保护）
	 *
	 * 【访问方式】
	 * 读者：rcu_dereference(ps->pd)
	 * 写者：rcu_assign_pointer(ps->pd, new_pd)
	 *
	 * 【为什么用 RCU？】
	 * padata_do_parallel() 频繁访问 pd，如果用锁保护性能差。
	 * RCU 允许无锁读取，只在替换时需要同步。
	 */

	struct parallel_data		*opd;
	/* 旧的 parallel_data 指针
	 * 替换 pd 时，旧的 pd 保存在这里，等待 RCU grace period 后释放。
	 *
	 * 【替换流程】
	 * 1. 分配新的 parallel_data
	 * 2. ps->opd = ps->pd（保存旧指针）
	 * 3. rcu_assign_pointer(ps->pd, new_pd)
	 * 4. 等待所有读者完成（synchronize_rcu）
	 * 5. 释放 opd
	 */

	struct list_head		list;
	/* 链表节点
	 * 链入 padata_instance->pslist
	 */
};

/**
 * struct padata_mt_job - represents one multithreaded job
 *
 * @thread_fn: Called for each chunk of work that a padata thread does.
 * @fn_arg: The thread function argument.
 * @start: The start of the job (units are job-specific).
 * @size: size of this node's work (units are job-specific).
 * @align: Ranges passed to the thread function fall on this boundary, with the
 *         possible exceptions of the beginning and end of the job.
 * @min_chunk: The minimum chunk size in job-specific units.  This allows
 *             the client to communicate the minimum amount of work that's
 *             appropriate for one worker thread to do at once.
 * @max_threads: Max threads to use for the job, actual number may be less
 *               depending on task size and minimum chunk size.
 * @numa_aware: Distribute jobs to different nodes with CPU in a round robin fashion.
 */
/*
 * 【结构体】padata_mt_job - 表示一个多线程批量任务
 *
 * 【设计目的】
 * 提供更简单的 API 用于批量并行处理，无需手动管理序列号和重排序。
 * 适合一次性批量任务（如内存初始化、页面迁移）。
 *
 * 【与 padata_priv 的区别】
 * - padata_priv: 流式处理，需要保持严格顺序
 * - padata_mt_job: 批量处理，无需顺序，更简单
 */
struct padata_mt_job {
	void (*thread_fn)(unsigned long start, unsigned long end, void *arg);
	/* 线程函数
	 * 每个工作线程调用此函数处理一块数据。
	 *
	 * @start: 当前块的起始位置（包含）
	 * @end:   当前块的结束位置（不包含），即 [start, end) 区间
	 * @arg:   用户参数（fn_arg）
	 *
	 * 【调用方式】
	 * padata 自动将 [job->start, job->start + job->size) 分成多块，
	 * 每块调用一次 thread_fn。
	 *
	 * 【并发性】
	 * 多个 CPU 可能同时调用 thread_fn，处理不同的块。
	 * 函数必须是线程安全的（不同块之间无共享可变状态）。
	 *
	 * 【示例】初始化内存页
	 * void init_pages(unsigned long start, unsigned long end, void *arg) {
	 *     for (unsigned long pfn = start; pfn < end; pfn++) {
	 *         struct page *page = pfn_to_page(pfn);
	 *         init_page(page);
	 *     }
	 * }
	 */

	void			*fn_arg;
	/* 传递给 thread_fn 的用户参数
	 * 可以是任意指针（上下文数据）
	 */

	unsigned long		start;
	/* 任务的起始位置（单位由任务定义）
	 *
	 * 【单位示例】
	 * - 内存页号（PFN）
	 * - 数组索引
	 * - 字节偏移
	 */

	unsigned long		size;
	/* 任务的总大小（单位与 start 相同）
	 * 实际处理的范围是 [start, start + size)
	 */

	unsigned long		align;
	/* 对齐边界
	 * thread_fn 接收的 start/end 会在此边界上对齐（除了首尾）。
	 *
	 * 【用途】
	 * - 缓存行对齐（64 字节）：减少伪共享
	 * - 页对齐（4096 字节）：与硬件页表对齐
	 * - 特定数据结构对齐
	 *
	 * 【示例】
	 * align = 64，可能的分块：
	 * - [0, 192)    // 首块可能不对齐
	 * - [192, 256)  // 对齐到 64
	 * - [256, 320)  // 对齐到 64
	 * - [320, 350)  // 尾块可能不对齐
	 */

	unsigned long		min_chunk;
	/* 最小块大小
	 * 每个 thread_fn 调用至少处理这么多单位的工作。
	 *
	 * 【目的】
	 * - 减少调度开销：太小的块会导致频繁的线程切换
	 * - 提高缓存效率：连续处理更多数据
	 *
	 * 【选择原则】
	 * - 太小：开销大，性能差
	 * - 太大：并行度低，负载不均
	 * - 经验值：足够让 CPU 工作几毫秒
	 *
	 * 【示例】
	 * 初始化页面：min_chunk = 1024（每次至少处理 1024 个页）
	 */

	int			max_threads;
	/* 最大线程数
	 * 限制并发工作的线程数量。
	 *
	 * 【实际线程数】
	 * min(max_threads, 在线 CPU 数, size / min_chunk)
	 *
	 * 【选择】
	 * - 0 或负数：使用所有在线 CPU
	 * - 正数：限制线程数（避免过度并行）
	 *
	 * 【为什么限制？】
	 * - 减少竞争：某些操作有共享资源
	 * - 节能：不需要唤醒所有 CPU
	 * - 调试：限制并发简化问题定位
	 */

	bool			numa_aware;
	/* 是否感知 NUMA
	 *
	 * 【true 的行为】
	 * 以轮询方式将任务分配到不同 NUMA 节点的 CPU，
	 * 减少单个节点的内存带宽压力。
	 *
	 * 【false 的行为】
	 * 简单地使用所有可用 CPU，不考虑 NUMA 拓扑。
	 *
	 * 【适用场景】
	 * - 内存密集型任务：numa_aware = true
	 * - 计算密集型任务：numa_aware = false（影响不大）
	 * - 本地内存操作：numa_aware = false（数据已在特定节点）
	 */
};

/**
 * struct padata_instance - The overall control structure.
 *
 * @cpuhp_node: Linkage for CPU hotplug callbacks.
 * @parallel_wq: The workqueue used for parallel work.
 * @serial_wq: The workqueue used for serial work.
 * @pslist: List of padata_shell objects attached to this instance.
 * @cpumask: User supplied cpumasks for parallel and serial works.
 * @validate_cpumask: Internal cpumask used to validate @cpumask during hotplug.
 * @kobj: padata instance kernel object.
 * @lock: padata instance lock.
 * @flags: padata flags.
 */
/*
 * 【结构体】padata_instance - padata 实例（顶层控制结构）
 *
 * 每个 padata 实例管理一组 padata_shell，共享工作队列和 CPU 配置。
 */
struct padata_instance {
	struct hlist_node		cpuhp_node;
	/* CPU hotplug 回调链表节点
	 * 注册到 CPU hotplug 通知链，响应 CPU 上线/下线事件。
	 *
	 * 【CPU hotplug 处理】
	 * - CPU 上线：如果在 cpumask 中，添加到工作队列
	 * - CPU 下线：从工作队列移除，重新分配任务
	 */

	struct workqueue_struct		*parallel_wq;
	/* 并行工作队列
	 * 用于调度 parallel() 函数的执行。
	 *
	 * 【特性】
	 * - 通常是 unbound workqueue（不绑定到特定 CPU）
	 * - 支持 CPU 亲和性（可指定在哪些 CPU 上运行）
	 * - 并发执行（多个工作项同时运行）
	 *
	 * 【创建】
	 * padata_alloc() 时创建，padata_free() 时销毁
	 */

	struct workqueue_struct		*serial_wq;
	/* 串行工作队列
	 * 用于调度 serial() 函数的执行。
	 *
	 * 【特性】
	 * - 可以与 parallel_wq 相同，也可以独立
	 * - 独立的好处：隔离 parallel 和 serial 的负载
	 */

	struct list_head		pslist;
	/* padata_shell 链表
	 * 所有附加到此实例的 shell
	 *
	 * 【用途】
	 * - CPU hotplug：更新所有 shell 的 parallel_data
	 * - 清理：释放实例时遍历所有 shell
	 */

	struct padata_cpumask		cpumask;
	/* 用户提供的 CPU 掩码
	 * 通过 padata_set_cpumask() 设置
	 *
	 * 【作用域】
	 * 新创建的 padata_shell 继承此掩码
	 */

	cpumask_var_t			validate_cpumask;
	/* 内部 CPU 掩码（用于 hotplug 验证）
	 * CPU hotplug 事件时，验证新的 CPU 配置是否有效
	 */

	struct kobject                   kobj;
	/* 内核对象
	 * 用于 sysfs 导出（/sys/kernel/padata/<name>）
	 *
	 * 【sysfs 接口】
	 * 可以通过 sysfs 查询和配置 padata 实例
	 */

	struct mutex			 lock;
	/* 实例锁
	 * 保护实例级别的操作（添加/删除 shell，更新 cpumask）
	 */

	u8				 flags;
	/* 标志位 */

#define	PADATA_INIT	1
	/* 已初始化标志
	 * padata_alloc() 完成后设置
	 */

#define	PADATA_RESET	2
	/* 重置标志
	 * CPU 掩码改变时设置，触发 parallel_data 重建
	 */

#define	PADATA_INVALID	4
	/* 无效标志
	 * 实例正在销毁或配置无效
	 */
};

#ifdef CONFIG_PADATA
/*
 * ============================================================================
 * 【padata API 函数】
 * ============================================================================
 */

extern void __init padata_init(void);
/* 初始化 padata 子系统
 *
 * 【调用时机】
 * 内核启动早期（__init 阶段）
 *
 * 【工作内容】
 * 注册 CPU hotplug 通知链，初始化全局数据结构
 */

extern struct padata_instance *padata_alloc(const char *name);
/* 分配 padata 实例
 * @name: 实例名称（用于 sysfs 和调试）
 *
 * 返回值：成功返回 padata_instance *，失败返回 NULL
 *
 * 【工作内容】
 * 1. 分配 padata_instance 结构
 * 2. 创建 parallel_wq 和 serial_wq 工作队列
 * 3. 初始化 CPU 掩码（默认所有在线 CPU）
 * 4. 注册 CPU hotplug 回调
 * 5. 创建 sysfs 条目（/sys/kernel/padata/<name>）
 *
 * 【使用示例】
 * struct padata_instance *pinst = padata_alloc("crypto");
 * if (!pinst)
 *     return -ENOMEM;
 */

extern void padata_free(struct padata_instance *pinst);
/* 释放 padata 实例
 * @pinst: 要释放的实例
 *
 * 【工作内容】
 * 1. 注销 CPU hotplug 回调
 * 2. 停止并销毁工作队列
 * 3. 释放所有 padata_shell
 * 4. 删除 sysfs 条目
 * 5. 释放实例内存
 *
 * 【注意】
 * 调用前应确保所有任务已完成，否则可能导致崩溃
 */

extern struct padata_shell *padata_alloc_shell(struct padata_instance *pinst);
/* 分配 padata shell
 * @pinst: 所属的实例
 *
 * 返回值：成功返回 padata_shell *，失败返回 NULL
 *
 * 【工作内容】
 * 1. 分配 padata_shell 结构
 * 2. 创建 parallel_data（根据实例的 cpumask）
 * 3. 将 shell 加入实例的 pslist
 *
 * 【使用场景】
 * 每种类型的并行任务需要一个 shell：
 * - 加密：一个 shell
 * - 解密：另一个 shell
 * - 压缩：又一个 shell
 *
 * 【使用示例】
 * struct padata_shell *ps = padata_alloc_shell(pinst);
 * if (!ps) {
 *     padata_free(pinst);
 *     return -ENOMEM;
 * }
 */

extern void padata_free_shell(struct padata_shell *ps);
/* 释放 padata shell
 * @ps: 要释放的 shell
 *
 * 【工作内容】
 * 1. 从实例的 pslist 移除
 * 2. 等待所有引用的任务完成
 * 3. 释放 parallel_data
 * 4. 释放 shell 内存
 */

extern int padata_do_parallel(struct padata_shell *ps,
			      struct padata_priv *padata, int *cb_cpu);
/* 提交并行任务
 * @ps:     padata shell
 * @padata: 任务描述（必须已初始化 parallel 和 serial 函数）
 * @cb_cpu: 输入/输出参数
 *          输入：期望的 serial CPU（-1 表示自动选择）
 *          输出：实际分配的 serial CPU
 *
 * 返回值：0=成功，负值=失败（如 -EBUSY，实例正在重置）
 *
 * 【工作流程】
 * 1. 分配序列号（seq_nr）
 * 2. 设置 cb_cpu（如果是 -1，自动选择）
 * 3. 将任务加入 parallel 队列
 * 4. 调度工作队列执行 parallel()
 *
 * 【调用上下文】
 * 任何上下文（进程、中断、softirq）
 *
 * 【注意事项】
 * - padata 生命周期：调用者负责保证在 serial() 完成前不释放
 * - parallel 和 serial 回调必须已设置
 * - 任务完成前不要修改 padata 内容
 *
 * 【使用示例】
 * struct my_job {
 *     struct padata_priv padata;
 *     // ... 其他数据
 * };
 *
 * struct my_job *job = kmalloc(sizeof(*job), GFP_KERNEL);
 * job->padata.parallel = my_parallel;
 * job->padata.serial = my_serial;
 * int cb_cpu = -1;  // 自动选择
 * int err = padata_do_parallel(ps, &job->padata, &cb_cpu);
 * if (err) {
 *     kfree(job);
 *     return err;
 * }
 */

extern void padata_do_serial(struct padata_priv *padata);
/* 通知 parallel 完成，触发 serial
 * @padata: 完成的任务
 *
 * 【调用者】
 * parallel() 函数在处理完成后调用
 *
 * 【工作流程】
 * 1. 将任务从 parallel 队列移到 reorder 队列
 * 2. 检查是否可以执行 serial（seq_nr == processed）
 * 3. 如果可以，移到 serial 队列并调度 serial_wq
 * 4. serial_wq 按序执行 serial() 回调
 *
 * 【调用上下文】
 * parallel() 函数的执行上下文（工作队列上下文，可睡眠）
 *
 * 【注意】
 * parallel() 必须调用此函数，否则任务永远不会完成（泄漏）
 */

extern void __init padata_do_multithreaded(struct padata_mt_job *job);
/* 执行多线程批量任务
 * @job: 任务描述
 *
 * 【工作流程】
 * 1. 根据 job->size, job->min_chunk, job->max_threads 计算分块
 * 2. 为每个块创建工作项，调度到不同 CPU
 * 3. 并行执行 job->thread_fn
 * 4. 等待所有块完成
 *
 * 【调用上下文】
 * 进程上下文（会阻塞等待完成）
 *
 * 【与 padata_do_parallel 的区别】
 * - padata_do_parallel: 流式，异步，需要保持顺序
 * - padata_do_multithreaded: 批量，同步，无需顺序
 *
 * 【使用场景】
 * - 初始化大量内存页
 * - 批量数据转换
 * - 并行文件系统操作
 *
 * 【使用示例】
 * void init_pages(unsigned long start, unsigned long end, void *arg) {
 *     for (unsigned long pfn = start; pfn < end; pfn++)
 *         init_page(pfn_to_page(pfn));
 * }
 *
 * struct padata_mt_job job = {
 *     .thread_fn   = init_pages,
 *     .fn_arg      = NULL,
 *     .start       = start_pfn,
 *     .size        = nr_pages,
 *     .align       = 1,
 *     .min_chunk   = 1024,
 *     .max_threads = 0,  // 使用所有 CPU
 *     .numa_aware  = true,
 * };
 * padata_do_multithreaded(&job);
 */

extern int padata_set_cpumask(struct padata_instance *pinst, int cpumask_type,
			      cpumask_var_t cpumask);
/* 设置 CPU 掩码
 * @pinst:        padata 实例
 * @cpumask_type: 掩码类型（PADATA_CPU_PARALLEL 或 PADATA_CPU_SERIAL）
 * @cpumask:      新的 CPU 掩码
 *
 * 返回值：0=成功，负值=失败
 *
 * 【工作流程】
 * 1. 验证 cpumask（必须包含至少一个在线 CPU）
 * 2. 更新实例的 cpumask
 * 3. 重建所有 shell 的 parallel_data
 * 4. 通过 RCU 替换旧的 parallel_data
 *
 * 【使用场景】
 * - 动态调整 CPU 使用：根据负载调整
 * - CPU 隔离：为 padata 预留特定 CPU
 * - NUMA 优化：绑定到特定 NUMA 节点
 *
 * 【注意】
 * 正在执行的任务不受影响（使用旧的 parallel_data），
 * 只有新提交的任务使用新掩码
 *
 * 【使用示例】
 * cpumask_var_t new_mask;
 * alloc_cpumask_var(&new_mask, GFP_KERNEL);
 * cpumask_copy(new_mask, cpu_online_mask);
 * cpumask_clear_cpu(0, new_mask);  // 不使用 CPU 0
 * padata_set_cpumask(pinst, PADATA_CPU_PARALLEL, new_mask);
 * free_cpumask_var(new_mask);
 */

#else
/* CONFIG_PADATA 未启用时的空实现 */

static inline void __init padata_init(void) {}

static inline void __init padata_do_multithreaded(struct padata_mt_job *job)
{
	job->thread_fn(job->start, job->start + job->size, job->fn_arg);
}
/* 降级为串行执行：在当前 CPU 上处理整个任务 */

#endif

#endif /* PADATA_H */
