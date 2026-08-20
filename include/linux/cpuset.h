/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CPUSET_H
#define _LINUX_CPUSET_H
/*
 *  cpuset interface
 *  cpuset 接口
 *
 *  Copyright (C) 2003 BULL SA
 *  Copyright (C) 2004-2006 Silicon Graphics, Inc.
 *
 */

/**
 * cpuset - CPU和内存节点集合管理
 *
 * 【什么是cpuset】
 * cpuset是Linux的资源隔离机制，允许将CPU核心和内存节点分组，
 * 限制进程只能使用特定的CPU和内存。主要用于：
 * - NUMA系统的性能优化
 * - 容器资源隔离（Docker/K8s使用）
 * - 实时任务的CPU隔离
 *
 * 【基本概念】
 * - cpus_allowed: 进程可以运行的CPU集合
 * - mems_allowed: 进程可以分配内存的NUMA节点集合
 * - 层级结构: cpuset可以嵌套，子cpuset继承父cpuset的限制
 */

#include <linux/sched.h>           /* 进程调度相关 */
#include <linux/sched/topology.h>  /* 调度域拓扑 */
#include <linux/sched/task.h>      /* 任务管理 */
#include <linux/cpumask.h>         /* CPU位掩码 */
#include <linux/nodemask.h>        /* NUMA节点位掩码 */
#include <linux/mm.h>              /* 内存管理 */
#include <linux/mmu_context.h>     /* MMU上下文 */
#include <linux/jump_label.h>      /* 静态分支优化 */

/**
 * lockdep_is_cpuset_held - 检查是否持有cpuset锁（用于lockdep调试）
 *
 * 返回值：true=持有锁，false=未持有
 */
extern bool lockdep_is_cpuset_held(void);

#ifdef CONFIG_CPUSETS  /* 启用cpuset功能时的定义 */

/*
 * Static branch rewrites can happen in an arbitrary order for a given
 * key. In code paths where we need to loop with read_mems_allowed_begin() and
 * read_mems_allowed_retry() to get a consistent view of mems_allowed, we need
 * to ensure that begin() always gets rewritten before retry() in the
 * disabled -> enabled transition. If not, then if local irqs are disabled
 * around the loop, we can deadlock since retry() would always be
 * comparing the latest value of the mems_allowed seqcount against 0 as
 * begin() still would see cpusets_enabled() as false. The enabled -> disabled
 * transition should happen in reverse order for the same reasons (want to stop
 * looking at real value of mems_allowed.sequence in retry() first).
 */
/**
 * 【静态分支重写顺序问题】
 *
 * 静态分支（static branch）可以在运行时被重写，但重写顺序是任意的。
 * 在使用 read_mems_allowed_begin() 和 read_mems_allowed_retry() 循环
 * 获取一致性视图时，必须确保正确的重写顺序：
 *
 * - disabled -> enabled转换：begin()必须先于retry()被重写
 *   否则如果循环内禁用了本地中断，会死锁。因为retry()会将最新的
 *   mems_allowed seqcount与0比较，而begin()仍然认为cpusets未启用。
 *
 * - enabled -> disabled转换：顺序相反，retry()先于begin()
 *   确保先停止查看mems_allowed.sequence的真实值
 *
 * 【静态分支优化】
 * static_key机制允许在运行时动态修改代码分支，避免运行时判断开销。
 * 通过直接修改机器码的跳转指令实现零开销的条件分支。
 */
extern struct static_key_false cpusets_pre_enable_key;   /* 预启用键，用于顺序控制 */
extern struct static_key_false cpusets_enabled_key;      /* cpuset启用键 */
extern struct static_key_false cpusets_insane_config_key; /* 不合理配置键 */

/**
 * cpusets_enabled - 检查cpuset是否启用
 *
 * 返回值：true=启用，false=未启用
 *
 * 【static_branch_unlikely】
 * 使用静态分支优化，预期大多数情况下返回false（未启用）
 * 编译器会优化为"快速路径"不需要跳转
 */
static inline bool cpusets_enabled(void)
{
	return static_branch_unlikely(&cpusets_enabled_key);
}

/**
 * cpuset_inc - 增加cpuset引用计数，启用cpuset
 *
 * 【顺序很重要】
 * 先增加pre_enable_key，再增加enabled_key
 * 确保read_mems_allowed_begin()在retry()之前生效
 *
 * _cpuslocked后缀：调用者必须持有cpu_hotplug_lock
 */
static inline void cpuset_inc(void)
{
	static_branch_inc_cpuslocked(&cpusets_pre_enable_key);
	static_branch_inc_cpuslocked(&cpusets_enabled_key);
}

/**
 * cpuset_dec - 减少cpuset引用计数，可能禁用cpuset
 *
 * 【顺序相反】
 * 先减少enabled_key，再减少pre_enable_key
 * 与inc操作相反，确保正确的禁用顺序
 */
static inline void cpuset_dec(void)
{
	static_branch_dec_cpuslocked(&cpusets_enabled_key);
	static_branch_dec_cpuslocked(&cpusets_pre_enable_key);
}

/*
 * This will get enabled whenever a cpuset configuration is considered
 * unsupportable in general. E.g. movable only node which cannot satisfy
 * any non movable allocations (see update_nodemask). Page allocator
 * needs to make additional checks for those configurations and this
 * check is meant to guard those checks without any overhead for sane
 * configurations.
 */
/**
 * cpusets_insane_config - 检查cpuset配置是否不合理
 *
 * 【何时启用】
 * 当cpuset配置不合理时启用，例如：
 * - 仅可移动节点（movable only node）无法满足非可移动内存分配需求
 * - 详见update_nodemask()函数
 *
 * 【目的】
 * 页分配器需要对这些配置做额外检查，此标志用于保护这些检查，
 * 在正常配置下无性能开销（静态分支优化）
 *
 * 返回值：true=配置不合理，false=配置正常
 */
static inline bool cpusets_insane_config(void)
{
	return static_branch_unlikely(&cpusets_insane_config_key);
}

/**
 * cpuset核心API函数声明
 */

extern int cpuset_init(void);              /* 初始化cpuset子系统 */
extern void cpuset_init_smp(void);         /* SMP初始化时的cpuset设置 */
extern void cpuset_force_rebuild(void);    /* 强制重建调度域 */
extern void cpuset_update_active_cpus(void); /* 更新活动CPU集合 */
extern void inc_dl_tasks_cs(struct task_struct *task); /* 增加deadline任务计数 */
extern void dec_dl_tasks_cs(struct task_struct *task); /* 减少deadline任务计数 */

/**
 * cpuset锁管理
 * 用于保护cpuset数据结构的并发访问
 */
extern void cpuset_lock(void);                       /* 获取cpuset锁 */
extern void cpuset_unlock(void);                     /* 释放cpuset锁 */
extern void lockdep_assert_cpuset_lock_held(void);   /* 断言持有cpuset锁 */

/**
 * cpuset_cpus_allowed_locked - 获取任务允许的CPU掩码（已持锁版本）
 * @p: 任务结构指针
 * @mask: 输出的CPU掩码
 *
 * 调用者必须持有cpuset_lock
 */
extern void cpuset_cpus_allowed_locked(struct task_struct *p, struct cpumask *mask);

/**
 * cpuset_cpus_allowed - 获取任务允许的CPU掩码（自动加锁版本）
 * @p: 任务结构指针
 * @mask: 输出的CPU掩码
 *
 * 内部会自动加锁保护
 */
extern void cpuset_cpus_allowed(struct task_struct *p, struct cpumask *mask);

/**
 * cpuset_cpus_allowed_fallback - 任务CPU限制失败时的回退处理
 * @p: 任务结构指针
 *
 * 当任务无法在其允许的CPU上运行时调用，尝试回退到其他CPU
 *
 * 返回值：true=执行了回退，false=无需回退
 */
extern bool cpuset_cpus_allowed_fallback(struct task_struct *p);

/**
 * cpuset_mems_allowed - 获取任务允许的内存节点掩码
 * @p: 任务结构指针
 *
 * 返回值：NUMA节点掩码，标记任务可以从哪些节点分配内存
 */
extern nodemask_t cpuset_mems_allowed(struct task_struct *p);

#define cpuset_current_mems_allowed (current->mems_allowed)  /* 当前任务的内存节点掩码 */

void cpuset_init_current_mems_allowed(void);  /* 初始化当前任务的mems_allowed */

/**
 * cpuset_nodemask_valid_mems_allowed - 验证节点掩码是否有效
 * @nodemask: 要验证的节点掩码
 *
 * 返回值：非0=有效，0=无效
 */
int cpuset_nodemask_valid_mems_allowed(nodemask_t *nodemask);

/**
 * cpuset_current_node_allowed - 检查当前任务是否允许从指定节点分配内存
 * @node: NUMA节点ID
 * @gfp_mask: 内存分配标志
 *
 * 返回值：true=允许，false=不允许
 */
extern bool cpuset_current_node_allowed(int node, gfp_t gfp_mask);

/**
 * __cpuset_zone_allowed - 检查内存区域是否允许分配（内部函数）
 * @z: 内存区域（zone）指针
 * @gfp_mask: 内存分配标志
 *
 * 返回值：true=允许，false=不允许
 *
 * 【内存区域（zone）】
 * 每个NUMA节点的内存被划分为多个区域：ZONE_DMA, ZONE_NORMAL, ZONE_HIGHMEM等
 */
static inline bool __cpuset_zone_allowed(struct zone *z, gfp_t gfp_mask)
{
	return cpuset_current_node_allowed(zone_to_nid(z), gfp_mask);
}

/**
 * cpuset_zone_allowed - 检查内存区域是否允许分配（公开接口）
 * @z: 内存区域指针
 * @gfp_mask: 内存分配标志
 *
 * 返回值：true=允许，false=不允许
 *
 * 【快速路径优化】
 * 如果cpuset未启用，直接返回true，避免函数调用开销
 */
static inline bool cpuset_zone_allowed(struct zone *z, gfp_t gfp_mask)
{
	if (cpusets_enabled())
		return __cpuset_zone_allowed(z, gfp_mask);
	return true;
}

/**
 * cpuset_mems_allowed_intersects - 检查两个任务的内存节点是否有交集
 * @tsk1: 任务1
 * @tsk2: 任务2
 *
 * 返回值：非0=有交集，0=无交集
 *
 * 【用途】
 * 判断两个任务是否可能共享内存页，用于内存管理优化
 */
extern int cpuset_mems_allowed_intersects(const struct task_struct *tsk1,
					  const struct task_struct *tsk2);

#ifdef CONFIG_CPUSETS_V1  /* cpuset v1版本特性 */
/**
 * cpuset_memory_pressure_bump - 记录内存压力事件
 *
 * 【内存压力监控】
 * cpuset v1提供内存压力统计，记录内存分配失败或回收事件
 * 用于监控容器的内存使用情况
 *
 * 【实现】
 * 只在启用内存压力监控时才调用实际函数，避免性能开销
 */
#define cpuset_memory_pressure_bump() 				\
	do {							\
		if (cpuset_memory_pressure_enabled)		\
			__cpuset_memory_pressure_bump();	\
	} while (0)
extern int cpuset_memory_pressure_enabled;  /* 是否启用内存压力监控 */
extern void __cpuset_memory_pressure_bump(void);  /* 实际的压力记录函数 */
#else
static inline void cpuset_memory_pressure_bump(void) { }  /* v2版本空实现 */
#endif

/**
 * cpuset_task_status_allowed - 输出任务的cpuset状态信息
 * @m: seq_file序列文件（用于/proc输出）
 * @task: 任务结构指针
 *
 * 用于/proc/<pid>/status文件中显示Cpus_allowed等信息
 */
extern void cpuset_task_status_allowed(struct seq_file *m,
					struct task_struct *task);

/**
 * proc_cpuset_show - 显示任务所属的cpuset路径
 * @m: seq_file序列文件
 * @ns: PID命名空间
 * @pid: 进程ID
 * @tsk: 任务结构指针
 *
 * 返回值：0=成功，负数=错误码
 *
 * 用于/proc/<pid>/cpuset文件，显示如：/sys/fs/cgroup/cpuset/docker/xxx
 */
extern int proc_cpuset_show(struct seq_file *m, struct pid_namespace *ns,
			    struct pid *pid, struct task_struct *tsk);

/**
 * cpuset_mem_spread_node - 获取内存分散分配的节点
 *
 * 返回值：应该分配内存的NUMA节点ID
 *
 * 【内存分散策略】
 * 将内存分配分散到多个NUMA节点，避免单一节点过载
 * 提高大规模系统的内存带宽利用率
 */
extern int cpuset_mem_spread_node(void);

/**
 * cpuset_do_page_mem_spread - 检查是否对页缓存启用内存分散
 *
 * 返回值：非0=启用，0=不启用
 *
 * 【页缓存分散】
 * 控制文件页缓存是否分散到多个NUMA节点
 */
static inline int cpuset_do_page_mem_spread(void)
{
	return task_spread_page(current);
}

/**
 * current_cpuset_is_being_rebound - 检查当前cpuset是否正在重新绑定
 *
 * 返回值：true=正在重新绑定，false=不是
 *
 * 【重新绑定】
 * 当cpuset的CPU或内存节点配置改变时，需要重新绑定进程
 * 此函数用于检测这个过渡状态
 */
extern bool current_cpuset_is_being_rebound(void);

/**
 * dl_rebuild_rd_accounting - 重建deadline调度器的根域统计
 *
 * deadline调度器需要跟踪每个根域（root domain）的deadline任务统计
 */
extern void dl_rebuild_rd_accounting(void);

/**
 * rebuild_sched_domains - 重建调度域
 *
 * 当cpuset配置改变时，需要重建调度域以反映新的CPU拓扑结构
 */
extern void rebuild_sched_domains(void);

/**
 * cpuset_print_current_mems_allowed - 打印当前允许的内存节点
 *
 * 调试用，打印当前任务的mems_allowed到内核日志
 */
extern void cpuset_print_current_mems_allowed(void);

/**
 * cpuset_reset_sched_domains - 重置调度域
 *
 * 将调度域重置为默认配置
 */
extern void cpuset_reset_sched_domains(void);

/*
 * read_mems_allowed_begin is required when making decisions involving
 * mems_allowed such as during page allocation. mems_allowed can be updated in
 * parallel and depending on the new value an operation can fail potentially
 * causing process failure. A retry loop with read_mems_allowed_begin and
 * read_mems_allowed_retry prevents these artificial failures.
 */
/**
 * read_mems_allowed_begin - 开始读取mems_allowed的临界区
 *
 * 【并发问题】
 * mems_allowed（允许的内存节点）可以被并发更新。在页分配等需要根据
 * mems_allowed做决策的操作中，如果mems_allowed在操作过程中改变，
 * 可能导致操作失败，进而可能引起进程失败。
 *
 * 【解决方案：重试循环】
 * 使用read_mems_allowed_begin()和read_mems_allowed_retry()构成重试循环，
 * 防止这些"人为"的失败：
 *
 * unsigned int seq;
 * do {
 *     seq = read_mems_allowed_begin();
 *     // 读取mems_allowed并执行操作
 * } while (read_mems_allowed_retry(seq));
 *
 * 【seqcount机制】
 * 使用序列计数器（seqcount）实现读-写并发控制：
 * - 读者记录开始时的序列号
 * - 写者修改时递增序列号
 * - 读者完成后检查序列号是否改变，若改变则重试
 *
 * 返回值：序列号，用于后续的retry检查
 */
static inline unsigned int read_mems_allowed_begin(void)
{
	if (!static_branch_unlikely(&cpusets_pre_enable_key))
		return 0;  /* cpuset未启用，无需序列号 */

	return read_seqcount_begin(&current->mems_allowed_seq);
}

/*
 * If this returns true, the operation that took place after
 * read_mems_allowed_begin may have failed artificially due to a concurrent
 * update of mems_allowed. It is up to the caller to retry the operation if
 * appropriate.
 */
/**
 * read_mems_allowed_retry - 检查是否需要重试
 * @seq: read_mems_allowed_begin()返回的序列号
 *
 * 【返回值】
 * - true: 在begin和retry之间mems_allowed被并发修改，操作可能失败，应该重试
 * - false: mems_allowed未改变，操作结果有效，无需重试
 *
 * 【调用者责任】
 * 由调用者决定是否重试操作。某些操作即使失败也可以接受，
 * 则可以忽略此返回值。
 *
 * 【实现细节】
 * 比较当前序列号与保存的序列号：
 * - 序列号相同：未改变
 * - 序列号不同：被并发修改
 */
static inline bool read_mems_allowed_retry(unsigned int seq)
{
	if (!static_branch_unlikely(&cpusets_enabled_key))
		return false;  /* cpuset未启用，无需重试 */

	return read_seqcount_retry(&current->mems_allowed_seq, seq);
}

/**
 * set_mems_allowed - 设置允许的内存节点掩码
 * @nodemask: 新的节点掩码
 *
 * 【同步机制】
 * 1. task_lock: 保护任务结构
 * 2. local_irq_save/restore: 禁用本地中断，防止中断处理中读取不一致状态
 * 3. write_seqcount_begin/end: 更新序列计数器，通知并发读者
 *
 * 【为什么要禁用中断】
 * 如果中断处理中有代码读取mems_allowed，必须确保读到一致的数据。
 * 禁用中断保证写操作的原子性。
 */
static inline void set_mems_allowed(nodemask_t nodemask)
{
	unsigned long flags;

	task_lock(current);
	local_irq_save(flags);
	write_seqcount_begin(&current->mems_allowed_seq);
	current->mems_allowed = nodemask;
	write_seqcount_end(&current->mems_allowed_seq);
	local_irq_restore(flags);
	task_unlock(current);
}

/**
 * cpuset_nodes_allowed - 获取cgroup允许的节点掩码
 * @cgroup: cgroup结构指针
 * @mask: 输出的节点掩码
 */
extern void cpuset_nodes_allowed(struct cgroup *cgroup, nodemask_t *mask);

#else /* !CONFIG_CPUSETS - cpuset未启用时的空实现 */

/**
 * 【CONFIG_CPUSETS未启用时的空实现】
 *
 * 当内核配置中未启用cpuset功能时，所有cpuset函数都被定义为
 * 空函数或返回默认值，确保代码可以正常编译和运行。
 *
 * 这种设计允许内核代码无需条件编译就能调用cpuset函数，
 * 编译器会优化掉这些空函数调用。
 */

static inline bool cpusets_enabled(void) { return false; }  /* 总是未启用 */

static inline bool cpusets_insane_config(void) { return false; }  /* 总是正常 */

static inline int cpuset_init(void) { return 0; }  /* 初始化成功 */
static inline void cpuset_init_smp(void) {}  /* 空操作 */

static inline void cpuset_force_rebuild(void) { }  /* 无需重建 */

/**
 * cpuset_update_active_cpus - 更新活动CPU（未启用版本）
 *
 * 直接调用partition_sched_domains创建单一调度域，
 * 包含所有CPU，无cpuset限制
 */
static inline void cpuset_update_active_cpus(void)
{
	partition_sched_domains(1, NULL, NULL);
}

static inline void inc_dl_tasks_cs(struct task_struct *task) { }  /* 无需计数 */
static inline void dec_dl_tasks_cs(struct task_struct *task) { }  /* 无需计数 */
static inline void cpuset_lock(void) { }  /* 无需加锁 */
static inline void cpuset_unlock(void) { }  /* 无需解锁 */
static inline void lockdep_assert_cpuset_lock_held(void) { }  /* 无需断言 */

/**
 * cpuset_cpus_allowed_locked - 获取任务允许的CPU（未启用版本）
 * @p: 任务结构指针
 * @mask: 输出的CPU掩码
 *
 * 返回任务的可能CPU集合，通常是所有在线CPU
 */
static inline void cpuset_cpus_allowed_locked(struct task_struct *p,
					struct cpumask *mask)
{
	cpumask_copy(mask, task_cpu_possible_mask(p));
}

/**
 * cpuset_cpus_allowed - 获取任务允许的CPU（未启用版本）
 *
 * 直接调用_locked版本，因为无需实际加锁
 */
static inline void cpuset_cpus_allowed(struct task_struct *p,
				       struct cpumask *mask)
{
	cpuset_cpus_allowed_locked(p, mask);
}

/**
 * cpuset_cpus_allowed_fallback - CPU限制回退（未启用版本）
 *
 * 返回值：false - 无需回退，因为没有限制
 */
static inline bool cpuset_cpus_allowed_fallback(struct task_struct *p)
{
	return false;
}

/**
 * cpuset_mems_allowed - 获取任务允许的内存节点（未启用版本）
 * @p: 任务结构指针
 *
 * 返回值：所有可能的内存节点（node_possible_map）
 */
static inline nodemask_t cpuset_mems_allowed(struct task_struct *p)
{
	return node_possible_map;
}

/**
 * cpuset_current_mems_allowed - 当前任务的内存节点（未启用版本）
 *
 * 定义为所有内存节点（N_MEMORY状态的节点）
 */
#define cpuset_current_mems_allowed (node_states[N_MEMORY])
static inline void cpuset_init_current_mems_allowed(void) {}  /* 无需初始化 */

/**
 * cpuset_nodemask_valid_mems_allowed - 验证节点掩码（未启用版本）
 * @nodemask: 要验证的节点掩码
 *
 * 返回值：1 - 总是有效，因为没有限制
 */
static inline int cpuset_nodemask_valid_mems_allowed(nodemask_t *nodemask)
{
	return 1;
}

/**
 * __cpuset_zone_allowed - 检查区域是否允许（未启用版本）
 *
 * 返回值：true - 总是允许
 */
static inline bool __cpuset_zone_allowed(struct zone *z, gfp_t gfp_mask)
{
	return true;
}

/**
 * cpuset_zone_allowed - 检查区域是否允许（未启用版本）
 *
 * 返回值：true - 总是允许
 */
static inline bool cpuset_zone_allowed(struct zone *z, gfp_t gfp_mask)
{
	return true;
}

/**
 * cpuset_mems_allowed_intersects - 检查内存节点交集（未启用版本）
 *
 * 返回值：1 - 总是有交集，因为所有任务都能访问所有节点
 */
static inline int cpuset_mems_allowed_intersects(const struct task_struct *tsk1,
						 const struct task_struct *tsk2)
{
	return 1;
}

static inline void cpuset_memory_pressure_bump(void) {}  /* 无需记录压力 */

/**
 * cpuset_task_status_allowed - 输出任务状态（未启用版本）
 *
 * 空实现，不输出任何信息
 */
static inline void cpuset_task_status_allowed(struct seq_file *m,
						struct task_struct *task)
{
}

/**
 * cpuset_mem_spread_node - 获取分散节点（未启用版本）
 *
 * 返回值：0 - 默认使用节点0
 */
static inline int cpuset_mem_spread_node(void)
{
	return 0;
}

/**
 * cpuset_do_page_mem_spread - 页缓存分散（未启用版本）
 *
 * 返回值：0 - 不启用分散
 */
static inline int cpuset_do_page_mem_spread(void)
{
	return 0;
}

/**
 * current_cpuset_is_being_rebound - 检查重新绑定（未启用版本）
 *
 * 返回值：false - 不可能重新绑定
 */
static inline bool current_cpuset_is_being_rebound(void)
{
	return false;
}

/**
 * dl_rebuild_rd_accounting - 重建根域统计（未启用版本）
 *
 * 空实现
 */
static inline void dl_rebuild_rd_accounting(void)
{
}

/**
 * rebuild_sched_domains - 重建调度域（未启用版本）
 *
 * 创建单一的全局调度域
 */
static inline void rebuild_sched_domains(void)
{
	partition_sched_domains(1, NULL, NULL);
}

/**
 * cpuset_reset_sched_domains - 重置调度域（未启用版本）
 *
 * 同rebuild_sched_domains
 */
static inline void cpuset_reset_sched_domains(void)
{
	partition_sched_domains(1, NULL, NULL);
}

/**
 * cpuset_print_current_mems_allowed - 打印内存节点（未启用版本）
 *
 * 空实现
 */
static inline void cpuset_print_current_mems_allowed(void)
{
}

/**
 * set_mems_allowed - 设置内存节点（未启用版本）
 * @nodemask: 节点掩码（被忽略）
 *
 * 空实现，因为没有限制需要设置
 */
static inline void set_mems_allowed(nodemask_t nodemask)
{
}

/**
 * read_mems_allowed_begin - 开始读取（未启用版本）
 *
 * 返回值：0 - 无需序列号
 */
static inline unsigned int read_mems_allowed_begin(void)
{
	return 0;
}

/**
 * read_mems_allowed_retry - 检查重试（未启用版本）
 * @seq: 序列号（被忽略）
 *
 * 返回值：false - 无需重试
 */
static inline bool read_mems_allowed_retry(unsigned int seq)
{
	return false;
}

/**
 * cpuset_nodes_allowed - 获取cgroup节点（未启用版本）
 * @cgroup: cgroup结构指针
 * @mask: 输出的节点掩码
 *
 * 返回所有内存节点
 */
static inline void cpuset_nodes_allowed(struct cgroup *cgroup, nodemask_t *mask)
{
	nodes_copy(*mask, node_states[N_MEMORY]);
}
#endif /* !CONFIG_CPUSETS */

#endif /* _LINUX_CPUSET_H */
