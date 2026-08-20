/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_SMP_H
#define __LINUX_SMP_H

/*
 *	Generic SMP support
 *		Alan Cox. <alan@redhat.com>
 */
/*
 * ============================================================================
 * 【SMP（Symmetric Multi-Processing，对称多处理）概述】
 *
 * 【什么是 SMP？】
 * SMP 是指所有 CPU 核心地位平等，共享同一块物理内存和 I/O 设备。
 * 与之相对的是非对称多处理（AMP），其中不同核心有不同的任务分工。
 *
 * 【本头文件的作用】
 * 提供 CPU 间通信和控制的核心接口：
 * 1. 跨 CPU 函数调用（smp_call_function）
 * 2. CPU 启动和停止（smp_prepare_cpus, smp_send_stop）
 * 3. CPU ID 获取（smp_processor_id）
 * 4. 重调度通知（smp_send_reschedule）
 *
 * 【关键概念】
 * - IPI（Inter-Processor Interrupt，处理器间中断）：CPU 间通信的底层机制
 * - CPU hotplug：动态启用/禁用 CPU（节能、故障隔离）
 * - CPU affinity：将任务绑定到特定 CPU
 * - Per-CPU 数据：每个 CPU 独立的数据副本（避免缓存伪共享）
 *
 * 【使用场景】
 * - TLB shootdown：使其他 CPU 的 TLB 缓存失效
 * - 远程唤醒：唤醒其他 CPU 上的进程
 * - 系统关闭：停止所有 CPU 进入关机流程
 * - 调试：在所有 CPU 上触发栈回溯
 * - RCU 同步：等待所有 CPU 完成宽限期
 * ============================================================================
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/smp_types.h>

/*
 * 【函数指针类型】SMP 调用的回调函数签名
 */
typedef void (*smp_call_func_t)(void *info);
/* 跨 CPU 调用的函数类型
 * @info: 传递给函数的参数（void * 可以是任意数据的指针）
 *
 * 【函数要求】
 * - 必须快速执行（在中断上下文中运行，禁止睡眠）
 * - 不能调用可能阻塞的函数（mutex_lock, schedule, msleep 等）
 * - 可以使用自旋锁（spinlock），但要小心死锁
 * - 可以访问 per-CPU 数据
 *
 * 【典型用途】
 * - TLB 刷新：flush_tlb_mm_range(mm, start, end)
 * - 缓存刷新：flush_cache_range(vma, start, end)
 * - 时钟同步：update_process_times()
 */

typedef bool (*smp_cond_func_t)(int cpu, void *info);
/* 条件判断函数类型（决定是否在某个 CPU 上执行）
 * @cpu:  候选 CPU 编号
 * @info: 传递的参数
 *
 * 返回值：true=在该 CPU 上执行，false=跳过该 CPU
 *
 * 【使用场景】
 * on_each_cpu_cond(): 只在满足条件的 CPU 上执行函数。
 *
 * 【示例】
 * bool should_run_on_cpu(int cpu, void *info) {
 *     // 只在偶数 CPU 上运行
 *     return (cpu % 2 == 0);
 * }
 */

/*
 * structure shares (partial) layout with struct irq_work
 */
/*
 * 【结构体】__call_single_data - 单个 CPU 调用的数据结构
 *
 * 用于 smp_call_function_single() 等函数，封装要在目标 CPU 上执行的任务。
 *
 * 【为什么与 irq_work 共享布局？】
 * 两者都是异步执行机制，共享布局可以复用代码路径，节省内存。
 */
struct __call_single_data {
	struct __call_single_node node;
	/* 链表节点，用于排队到目标 CPU 的调用队列
	 * 每个 CPU 维护一个调用队列（call_single_queue）
	 */

	smp_call_func_t func;
	/* 要在目标 CPU 上执行的函数 */

	void *info;
	/* 传递给 func 的参数 */
};

/*
 * 【宏】CSD_INIT - 初始化 call_single_data 结构
 * @_func: 要执行的函数
 * @_info: 传递的参数
 *
 * 【使用示例】
 * struct __call_single_data csd = CSD_INIT(my_func, my_data);
 */
#define CSD_INIT(_func, _info) \
	(struct __call_single_data){ .func = (_func), .info = (_info), }

/* Use __aligned() to avoid to use 2 cache lines for 1 csd */
typedef struct __call_single_data call_single_data_t
	__aligned(sizeof(struct __call_single_data));
/* call_single_data_t 类型定义
 *
 * 【为什么需要对齐？】
 * __aligned(sizeof(struct __call_single_data)):
 * 确保结构体按自身大小对齐，避免跨越缓存行（cache line）。
 *
 * 【缓存行伪共享（False Sharing）问题】
 * 如果一个 call_single_data 跨越两条缓存行（通常 64 字节），
 * 当多个 CPU 同时访问同一缓存行的不同数据时，会导致缓存行乒乓
 * （cache line bouncing），严重降低性能。
 *
 * 【对齐的好处】
 * - 每个 call_single_data 占用完整的缓存行（或对齐到缓存行边界）
 * - 减少缓存一致性协议的开销（MESI/MOESI 协议）
 * - 提高并发性能（不同 CPU 操作不同的缓存行）
 */

/*
 * 【宏】INIT_CSD - 动态初始化 call_single_data（赋值版本）
 * @_csd:  要初始化的 call_single_data 指针
 * @_func: 要执行的函数
 * @_info: 传递的参数
 *
 * 【与 CSD_INIT 的区别】
 * - CSD_INIT:  用于声明时初始化（复合字面量）
 * - INIT_CSD:  用于已分配的结构体赋值
 *
 * 【使用示例】
 * call_single_data_t *csd = kmalloc(sizeof(*csd), GFP_KERNEL);
 * INIT_CSD(csd, my_func, my_data);
 */
#define INIT_CSD(_csd, _func, _info)		\
do {						\
	*(_csd) = CSD_INIT((_func), (_info));	\
} while (0)

/*
 * Enqueue a llist_node on the call_single_queue; be very careful, read
 * flush_smp_call_function_queue() in detail.
 */
extern void __smp_call_single_queue(int cpu, struct llist_node *node);
/* 将调用节点入队到目标 CPU 的调用队列（底层函数，通常不直接调用）
 * @cpu:  目标 CPU 编号
 * @node: 调用节点（llist_node，无锁链表节点）
 *
 * 【注意事项】
 * 这是底层函数，使用前必须仔细阅读 flush_smp_call_function_queue() 的实现。
 * 不正确的使用可能导致竞态条件、死锁或内存泄漏。
 *
 * 【工作原理】
 * 1. 将 node 加入目标 CPU 的无锁链表（call_single_queue）
 * 2. 发送 IPI（处理器间中断）通知目标 CPU
 * 3. 目标 CPU 在中断处理中执行队列中的所有调用
 */

/* total number of cpus in this system (may exceed NR_CPUS) */
extern unsigned int total_cpus;
/* 系统中 CPU 的总数（可能超过编译时的 NR_CPUS）
 *
 * 【NR_CPUS vs total_cpus】
 * - NR_CPUS: 编译时的 CPU 数量上限（Kconfig 配置，如 256, 512, 8192）
 * - total_cpus: 运行时检测到的实际 CPU 数量
 *
 * 【为什么可能超过 NR_CPUS？】
 * 实际不会超过，这里的注释是历史遗留。total_cpus <= NR_CPUS。
 *
 * 【使用场景】
 * - 统计和监控：显示系统有多少个 CPU
 * - 资源分配：根据 CPU 数量调整缓冲区大小
 * - 负载均衡：均匀分配任务到所有 CPU
 */

int smp_call_function_single(int cpuid, smp_call_func_t func, void *info,
			     int wait);
/* 在指定的单个 CPU 上执行函数
 * @cpuid: 目标 CPU 编号（0 到 nr_cpu_ids-1）
 * @func:  要执行的函数
 * @info:  传递给函数的参数
 * @wait:  是否等待执行完成（1=等待，0=不等待）
 *
 * 返回值：0=成功，负值=失败（如目标 CPU 离线）
 *
 * 【wait 参数的含义】
 * - wait=1（同步调用）：
 *   - 阻塞直到目标 CPU 执行完 func
 *   - 适合需要保证顺序的操作（如 TLB 刷新）
 * - wait=0（异步调用）：
 *   - 立即返回，func 稍后在目标 CPU 上执行
 *   - 适合发送通知类操作（如唤醒任务）
 *
 * 【调用上下文】
 * - 可以在进程上下文或中断上下文中调用
 * - 如果 cpuid == smp_processor_id()（调用自己），直接执行 func，不发送 IPI
 *
 * 【使用场景】
 * - CPU affinity 迁移：在目标 CPU 上设置任务状态
 * - 远程唤醒：唤醒在其他 CPU 上休眠的任务
 * - Per-CPU 数据操作：在数据所属的 CPU 上修改
 *
 * 【性能考虑】
 * - IPI 开销约 1-5 微秒（取决于架构和距离）
 * - 频繁调用会导致性能下降（考虑批量处理）
 */

void on_each_cpu_cond_mask(smp_cond_func_t cond_func, smp_call_func_t func,
			   void *info, bool wait, const struct cpumask *mask);
/* 在 mask 中满足条件的 CPU 上执行函数
 * @cond_func: 条件判断函数（可为 NULL，表示无条件执行）
 * @func:      要执行的函数
 * @info:      传递给函数的参数
 * @wait:      是否等待执行完成
 * @mask:      候选 CPU 集合
 *
 * 无返回值
 *
 * 【工作流程】
 * 1. 遍历 mask 中的每个 CPU
 * 2. 如果 cond_func == NULL 或 cond_func(cpu, info) 返回 true
 * 3. 在该 CPU 上执行 func(info)
 * 4. 如果 wait=true，等待所有 CPU 执行完成
 *
 * 【cond_func 的作用】
 * 动态过滤 CPU，例如：
 * - 只在运行特定任务的 CPU 上执行
 * - 只在空闲的 CPU 上执行
 * - 只在特定 NUMA 节点的 CPU 上执行
 *
 * 【使用场景】
 * - 有条件的 TLB 刷新：只刷新使用了该地址空间的 CPU
 * - 负载感知操作：只在负载低于阈值的 CPU 上执行
 */

int smp_call_function_single_async(int cpu, call_single_data_t *csd);
/* 异步在指定 CPU 上执行函数（非阻塞版本）
 * @cpu: 目标 CPU 编号
 * @csd: 预先初始化的 call_single_data_t 结构
 *
 * 返回值：0=成功排队，负值=失败（如 -EBUSY，上次调用还在执行）
 *
 * 【与 smp_call_function_single 的区别】
 * - smp_call_function_single: 每次调用都分配新的 csd（在栈上或内部）
 * - smp_call_function_single_async: 调用者提供 csd（可复用）
 *
 * 【为什么需要这个函数？】
 * 1. 避免重复分配：如果频繁调用，可以复用同一个 csd
 * 2. 非阻塞：立即返回，适合中断上下文
 * 3. 状态跟踪：可以检查 csd 是否仍在执行中
 *
 * 【使用注意】
 * - csd 必须在调用完成前保持有效（不能是栈变量，除非能保证）
 * - 同一个 csd 不能并发调用（返回 -EBUSY）
 * - 调用者负责 csd 的生命周期管理
 *
 * 【典型模式】
 * struct my_data {
 *     call_single_data_t csd;
 *     // ... 其他数据
 * };
 *
 * static void my_func(void *info) {
 *     struct my_data *data = info;
 *     // 处理 data
 * }
 *
 * struct my_data *data = kmalloc(...);
 * INIT_CSD(&data->csd, my_func, data);
 * smp_call_function_single_async(cpu, &data->csd);
 */

/*
 * Cpus stopping functions in panic. All have default weak definitions.
 * Architecture-dependent code may override them.
 */
/*
 * 【Panic 时的 CPU 停止函数】
 *
 * 当内核遇到致命错误（panic）时，需要停止所有 CPU 以防止进一步损坏。
 * 这些函数有弱定义（weak symbol），架构代码可以提供自己的实现。
 */

void __noreturn panic_smp_self_stop(void);
/* 当前 CPU 在 panic 时自我停止（不返回）
 *
 * 【调用时机】
 * 非主 CPU 在 panic 处理中调用，停止自己的执行。
 *
 * 【典型实现】(x86)
 * 1. 禁用本地中断（local_irq_disable）
 * 2. 禁用本地 APIC（apic_soft_disable）
 * 3. 进入死循环或 halt 状态
 *
 * 【为什么 __noreturn？】
 * 函数永不返回，帮助编译器优化和检测错误。
 */

void __noreturn nmi_panic_self_stop(struct pt_regs *regs);
/* 在 NMI 上下文中自我停止（用于 NMI watchdog panic）
 * @regs: 发生 NMI 时的寄存器状态
 *
 * 【与 panic_smp_self_stop 的区别】
 * - panic_smp_self_stop: 普通 panic 路径（可以使用中断）
 * - nmi_panic_self_stop: NMI panic 路径（中断已禁用，更严格的约束）
 *
 * 【NMI 的特殊性】
 * NMI（不可屏蔽中断）即使在中断禁用时也会触发，
 * 因此 panic 处理必须更加小心，避免死锁和递归。
 */

void crash_smp_send_stop(void);
/* 停止所有 CPU（用于内核崩溃转储）
 *
 * 无返回值
 *
 * 【调用时机】
 * kdump（内核崩溃转储）机制中，在保存内存镜像前停止所有 CPU。
 *
 * 【与 smp_send_stop 的区别】
 * - smp_send_stop: 普通关机路径（可以等待一段时间）
 * - crash_smp_send_stop: 崩溃路径（尽快停止，可能使用 NMI 强制停止）
 *
 * 【工作流程】
 * 1. 向所有其他 CPU 发送停止 IPI（或 NMI）
 * 2. 等待短暂时间（通常几毫秒）
 * 3. 强制停止未响应的 CPU（写 APIC 寄存器）
 */

int panic_smp_redirect_cpu(int target_cpu, void *msg);
/* 在 panic 时将一个 CPU 重定向到处理 panic 信息
 * @target_cpu: 要重定向的 CPU
 * @msg:        panic 消息
 *
 * 返回值：0=成功，负值=失败
 *
 * 【使用场景】
 * 多 CPU 同时 panic 时，选择一个 CPU 负责打印 panic 信息，
 * 其他 CPU 停止执行，避免输出混乱。
 */

/*
 * Call a function on all processors
 */
/*
 * 【在所有处理器上调用函数】
 */

static inline void on_each_cpu(smp_call_func_t func, void *info, int wait)
{
	on_each_cpu_cond_mask(NULL, func, info, wait, cpu_online_mask);
}
/* 在所有在线 CPU 上执行函数（包括当前 CPU）
 * @func: 要执行的函数
 * @info: 传递给函数的参数
 * @wait: 是否等待执行完成
 *
 * 无返回值
 *
 * 【等价于】
 * on_each_cpu_cond_mask(NULL, func, info, wait, cpu_online_mask);
 *
 * 【使用场景】
 * - 全局 TLB 刷新：flush_tlb_all()
 * - 全局缓存刷新：flush_cache_all()
 * - 时钟同步：更新所有 CPU 的时钟源
 * - 紧急停止：在所有 CPU 上禁用某个功能
 *
 * 【性能考虑】
 * 在大规模系统（几百个 CPU）上，广播 IPI 开销很大。
 * 考虑使用更精确的 CPU 掩码（cpumask）。
 */

/**
 * on_each_cpu_mask() - Run a function on processors specified by
 * cpumask, which may include the local processor.
 * @mask: The set of cpus to run on (only runs on online subset).
 * @func: The function to run. This must be fast and non-blocking.
 * @info: An arbitrary pointer to pass to the function.
 * @wait: If true, wait (atomically) until function has completed
 *        on other CPUs.
 *
 * If @wait is true, then returns once @func has returned.
 *
 * You must not call this function with disabled interrupts or from a
 * hardware interrupt handler or from a bottom half handler.  The
 * exception is that it may be used during early boot while
 * early_boot_irqs_disabled is set.
 */
static inline void on_each_cpu_mask(const struct cpumask *mask,
				    smp_call_func_t func, void *info, bool wait)
{
	on_each_cpu_cond_mask(NULL, func, info, wait, mask);
}

/*
 * Call a function on each processor for which the supplied function
 * cond_func returns a positive value. This may include the local
 * processor.  May be used during early boot while early_boot_irqs_disabled is
 * set. Use local_irq_save/restore() instead of local_irq_disable/enable().
 */
static inline void on_each_cpu_cond(smp_cond_func_t cond_func,
				    smp_call_func_t func, void *info, bool wait)
{
	on_each_cpu_cond_mask(cond_func, func, info, wait, cpu_online_mask);
}

/*
 * Architecture specific boot CPU setup.  Defined as empty weak function in
 * init/main.c. Architectures can override it.
 */
void __init smp_prepare_boot_cpu(void);

#ifdef CONFIG_SMP

#include <linux/preempt.h>
#include <linux/compiler.h>
#include <linux/thread_info.h>
#include <asm/smp.h>

/*
 * main cross-CPU interfaces, handles INIT, TLB flush, STOP, etc.
 * (defined in asm header):
 */

/*
 * stops all CPUs but the current one:
 */
extern void smp_send_stop(void);

/*
 * sends a 'reschedule' event to another CPU:
 */
extern void arch_smp_send_reschedule(int cpu);
/*
 * scheduler_ipi() is inline so can't be passed as callback reason, but the
 * callsite IP should be sufficient for root-causing IPIs sent from here.
 */
#define smp_send_reschedule(cpu) ({		  \
	trace_ipi_send_cpu(cpu, _RET_IP_, NULL);  \
	arch_smp_send_reschedule(cpu);		  \
})

/*
 * Prepare machine for booting other CPUs.
 */
extern void smp_prepare_cpus(unsigned int max_cpus);

/*
 * Bring a CPU up
 */
extern int __cpu_up(unsigned int cpunum, struct task_struct *tidle);

/*
 * Final polishing of CPUs
 */
extern void smp_cpus_done(unsigned int max_cpus);

/*
 * Call a function on all other processors
 */
void smp_call_function(smp_call_func_t func, void *info, int wait);
void smp_call_function_many(const struct cpumask *mask,
			    smp_call_func_t func, void *info, bool wait);

int smp_call_function_any(const struct cpumask *mask,
			  smp_call_func_t func, void *info, int wait);

void kick_all_cpus_sync(void);
void wake_up_all_idle_cpus(void);
bool cpus_peek_for_pending_ipi(const struct cpumask *mask);

/*
 * Generic and arch helpers
 */
void __init call_function_init(void);
void generic_smp_call_function_single_interrupt(void);
#define generic_smp_call_function_interrupt \
	generic_smp_call_function_single_interrupt

extern unsigned int setup_max_cpus;
extern void __init setup_nr_cpu_ids(void);
extern void __init smp_init(void);

extern int __boot_cpu_id;

static inline int get_boot_cpu_id(void)
{
	return __boot_cpu_id;
}

#else /* !SMP */

static inline void smp_send_stop(void) { }

/*
 *	These macros fold the SMP functionality into a single CPU system
 */
#define raw_smp_processor_id()			0
static inline void up_smp_call_function(smp_call_func_t func, void *info)
{
}
#define smp_call_function(func, info, wait) \
			(up_smp_call_function(func, info))

static inline void smp_send_reschedule(int cpu) { }
#define smp_call_function_many(mask, func, info, wait) \
			(up_smp_call_function(func, info))
static inline void call_function_init(void) { }

static inline int
smp_call_function_any(const struct cpumask *mask, smp_call_func_t func,
		      void *info, int wait)
{
	return smp_call_function_single(0, func, info, wait);
}

static inline void kick_all_cpus_sync(void) {  }
static inline void wake_up_all_idle_cpus(void) {  }
static inline bool cpus_peek_for_pending_ipi(const struct cpumask *mask)
{
	return false;
}

#define setup_max_cpus 0

#ifdef CONFIG_UP_LATE_INIT
extern void __init up_late_init(void);
static __always_inline void smp_init(void) { up_late_init(); }
#else
static inline void smp_init(void) { }
#endif

static inline int get_boot_cpu_id(void)
{
	return 0;
}

#endif /* !SMP */

/*
 * raw_smp_processor_id() - get the current (unstable) CPU id
 *
 * raw_smp_processor_id() is arch-specific/arch-defined and
 * may be a macro or a static inline function.
 *
 * For when you know what you are doing and need an unstable
 * CPU id.
 */

/*
 * Allow the architecture to differentiate between a stable and unstable read.
 * For example, x86 uses an IRQ-safe asm-volatile read for the unstable but a
 * regular asm read for the stable.
 */
#ifndef __smp_processor_id
#define __smp_processor_id() raw_smp_processor_id()
#endif

#ifdef CONFIG_DEBUG_PREEMPT
  extern unsigned int debug_smp_processor_id(void);
# define smp_processor_id() debug_smp_processor_id()

#else
/**
 * smp_processor_id() - get the current (stable) CPU id
 *
 * This is the normal accessor to the CPU id and should be used
 * whenever possible.
 *
 * The CPU id is stable when:
 *
 *  - IRQs are disabled;
 *  - preemption is disabled;
 *  - the task is CPU affine.
 *
 * When CONFIG_DEBUG_PREEMPT=y, we verify these assumptions and WARN
 * when smp_processor_id() is used when the CPU id is not stable.
 */

# define smp_processor_id() __smp_processor_id()
#endif

#define get_cpu()		({ preempt_disable(); __smp_processor_id(); })
#define put_cpu()		preempt_enable()

/*
 * Callback to arch code if there's nosmp or maxcpus=0 on the
 * boot command line:
 */
extern void arch_disable_smp_support(void);

extern void arch_thaw_secondary_cpus_begin(void);
extern void arch_thaw_secondary_cpus_end(void);

void smp_setup_processor_id(void);

int smp_call_on_cpu(unsigned int cpu, int (*func)(void *), void *par,
		    bool phys);

/* SMP core functions */
int smpcfd_prepare_cpu(unsigned int cpu);
int smpcfd_dead_cpu(unsigned int cpu);
int smpcfd_dying_cpu(unsigned int cpu);

#ifdef CONFIG_CSD_LOCK_WAIT_DEBUG
bool csd_lock_is_stuck(void);
#else
static inline bool csd_lock_is_stuck(void) { return false; }
#endif

#endif /* __LINUX_SMP_H */
