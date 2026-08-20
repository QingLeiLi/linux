/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  linux/include/linux/nmi.h
 */
/*
 * ============================================================================
 * 【NMI 和锁死检测器（Lockup Detector）概述】
 *
 * 【什么是 NMI？】
 * NMI (Non-Maskable Interrupt，不可屏蔽中断) 是一种特殊的硬件中断：
 * - 即使 CPU 禁用了中断（cli/local_irq_disable），NMI 仍然能触发
 * - 优先级最高，用于处理紧急事件（硬件错误、系统崩溃检测）
 * - 不能被软件屏蔽或延迟
 *
 * 【本头文件的作用】
 * 提供 Linux 的锁死检测（Lockup Detection）机制，用于检测系统挂起：
 * 1. 硬锁死（Hard Lockup）：CPU 长时间不响应中断（通常由死循环导致）
 * 2. 软锁死（Soft Lockup）：进程长时间不让出 CPU（内核态长时间运行）
 * 3. 挂起任务（Hung Task）：进程长时间处于 D 状态（不可中断睡眠）
 *
 * 【锁死检测的重要性】
 * - 调试：快速定位导致系统挂起的代码
 * - 可靠性：自动触发崩溃转储或重启，避免系统永久挂死
 * - 生产环境：watchdog 机制保证服务可用性
 *
 * 【检测机制】
 * 1. 硬锁死检测（Hard Lockup Detector）
 *    - 实现方式：NMI watchdog 或高精度定时器
 *    - 工作原理：每隔一段时间发送 NMI，检查 CPU 是否响应
 *    - 触发条件：CPU 超过阈值时间（默认 10 秒）未响应中断
 *
 * 2. 软锁死检测（Soft Lockup Detector）
 *    - 实现方式：每 CPU 的 watchdog 线程 + 高精度定时器
 *    - 工作原理：定时器定期更新时间戳，watchdog 线程检查时间戳
 *    - 触发条件：超过阈值时间（默认 20 秒）未调度 watchdog 线程
 *
 * 3. 挂起任务检测（Hung Task Detector）
 *    - 实现方式：内核线程 khungtaskd
 *    - 工作原理：定期扫描处于 D 状态的任务
 *    - 触发条件：任务超过 120 秒处于 D 状态
 *
 * 【配置选项】
 * - CONFIG_HARDLOCKUP_DETECTOR: 启用硬锁死检测
 * - CONFIG_SOFTLOCKUP_DETECTOR: 启用软锁死检测
 * - CONFIG_DETECT_HUNG_TASK: 启用挂起任务检测
 * - CONFIG_LOCKUP_DETECTOR: 总开关（包含硬/软锁死检测）
 *
 * 【sysctl 参数】(/proc/sys/kernel)
 * - watchdog_thresh: 阈值（秒），默认 10 秒
 * - soft_watchdog: 启用/禁用软锁死检测（0/1）
 * - hardlockup_panic: 硬锁死时是否 panic（0/1）
 * - softlockup_panic: 软锁死时是否 panic（0/1）
 *
 * 【使用场景】
 * - 长时间禁用中断的代码需要主动 touch_nmi_watchdog() 重置 watchdog
 * - 调试系统挂起问题时查看 lockup 日志
 * - 生产环境配置 watchdog 自动重启挂死系统
 * ============================================================================
 */
#ifndef LINUX_NMI_H
#define LINUX_NMI_H

#include <linux/sched.h>
#include <asm/irq.h>

/* Arch specific watchdogs might need to share extra watchdog-related APIs. */
/*
 * 某些架构的 watchdog 实现可能需要额外的 API（如 x86, SPARC64）
 */
#if defined(CONFIG_HARDLOCKUP_DETECTOR_ARCH) || defined(CONFIG_HARDLOCKUP_DETECTOR_SPARC64)
#include <asm/nmi.h>
/* 包含架构特定的 NMI 定义
 * - x86: NMI 处理、APIC 配置
 * - SPARC64: 特殊的 watchdog 实现（始终启用）
 */
#endif

#ifdef CONFIG_LOCKUP_DETECTOR
/*
 * ============================================================================
 * 【锁死检测器核心函数】
 *
 * CONFIG_LOCKUP_DETECTOR 启用时提供的函数和变量。
 * ============================================================================
 */

void lockup_detector_init(void);
/* 初始化锁死检测器
 *
 * 【调用时机】
 * 内核启动时调用，设置硬/软锁死检测器的基础设施。
 *
 * 【工作内容】
 * 1. 初始化 watchdog 线程（每 CPU 一个）
 * 2. 配置硬件 watchdog（如果支持）
 * 3. 设置高精度定时器
 * 4. 注册 CPU hotplug 回调
 */

void lockup_detector_retry_init(void);
/* 重试初始化锁死检测器
 *
 * 【使用场景】
 * 初次初始化失败时（如硬件资源不可用），稍后重试。
 */

void lockup_detector_soft_poweroff(void);
/* 软关机时停止锁死检测器
 *
 * 【调用时机】
 * 系统关机或重启时调用，避免在关机流程中误报 lockup。
 *
 * 【为什么需要？】
 * 关机流程可能需要较长时间（刷新磁盘、停止服务），
 * 不应该被误判为 lockup。
 */

extern int watchdog_user_enabled;
/* 用户是否启用了 watchdog（通过 sysctl）
 * - 0: 用户禁用
 * - 1: 用户启用
 *
 * 【sysctl 接口】
 * /proc/sys/kernel/watchdog
 */

extern int watchdog_thresh;
/* watchdog 阈值（秒）
 * - 硬锁死阈值：watchdog_thresh 秒
 * - 软锁死阈值：watchdog_thresh * 2 秒
 * - 默认值：10 秒
 * - 特殊值：0 表示暂停 watchdog
 *
 * 【sysctl 接口】
 * /proc/sys/kernel/watchdog_thresh
 *
 * 【为什么软锁死是 2 倍？】
 * 软锁死比硬锁死宽松，给予更多时间让系统恢复。
 */

extern unsigned long watchdog_enabled;
/* watchdog 启用状态位图
 * - bit 0 (WATCHDOG_HARDLOCKUP_ENABLED_BIT): 硬锁死检测
 * - bit 1 (WATCHDOG_SOFTOCKUP_ENABLED_BIT):  软锁死检测
 *
 * 【使用宏】
 * - WATCHDOG_HARDLOCKUP_ENABLED: 测试/设置 bit 0
 * - WATCHDOG_SOFTOCKUP_ENABLED:  测试/设置 bit 1
 *
 * 【为什么用位图？】
 * 可以独立启用/禁用硬锁死和软锁死检测。
 */

extern int watchdog_hardlockup_miss_thresh;
/* 硬锁死漏检阈值
 * 允许的连续漏检次数，超过后才报告硬锁死。
 *
 * 【用途】
 * 减少误报：某些情况下（如虚拟机暂停）可能暂时漏检，
 * 但不是真正的 lockup。
 */

extern struct cpumask watchdog_cpumask;
/* watchdog 监控的 CPU 掩码
 * 只有在掩码中的 CPU 会启用 watchdog 线程。
 *
 * 【使用场景】
 * - 节能：减少 watchdog 开销，只监控关键 CPU
 * - 调试：临时禁用某些 CPU 的 watchdog
 *
 * 【sysctl 接口】
 * /proc/sys/kernel/watchdog_cpumask
 */

extern unsigned long *watchdog_cpumask_bits;
/* watchdog_cpumask 的位数组指针（内部使用） */

#ifdef CONFIG_SMP
extern int sysctl_softlockup_all_cpu_backtrace;
/* 软锁死时是否打印所有 CPU 的栈回溯
 * - 0: 只打印触发 lockup 的 CPU
 * - 1: 打印所有 CPU 的栈（帮助定位死锁）
 *
 * 【sysctl 接口】
 * /proc/sys/kernel/softlockup_all_cpu_backtrace
 */

extern int sysctl_hardlockup_all_cpu_backtrace;
/* 硬锁死时是否打印所有 CPU 的栈回溯 */
#else
#define sysctl_softlockup_all_cpu_backtrace 0
#define sysctl_hardlockup_all_cpu_backtrace 0
/* 单 CPU 系统：无需打印"所有 CPU"（只有一个 CPU） */
#endif /* !CONFIG_SMP */

#else /* CONFIG_LOCKUP_DETECTOR */
/* CONFIG_LOCKUP_DETECTOR 未启用时的空实现 */
static inline void lockup_detector_init(void) { }
static inline void lockup_detector_retry_init(void) { }
static inline void lockup_detector_soft_poweroff(void) { }
#endif /* !CONFIG_LOCKUP_DETECTOR */

#ifdef CONFIG_SOFTLOCKUP_DETECTOR
/*
 * ============================================================================
 * 【软锁死检测器（Soft Lockup Detector）】
 *
 * 【检测原理】
 * 每个 CPU 运行一个低优先级的 watchdog 内核线程（watchdog/N）。
 * 高精度定时器定期触发，更新时间戳。watchdog 线程醒来时检查时间戳：
 * - 如果时间戳太旧（超过 2*watchdog_thresh 秒），说明该 CPU 长时间
 *   没有调度 watchdog 线程，即发生了软锁死。
 *
 * 【软锁死的典型原因】
 * 1. 内核代码长时间禁用抢占（preempt_disable）
 * 2. 长时间持有自旋锁（spinlock）
 * 3. 死循环或极慢的循环
 * 4. 中断风暴（大量中断导致内核态一直运行）
 *
 * 【与硬锁死的区别】
 * - 软锁死：中断仍能响应，但进程调度被阻塞
 * - 硬锁死：连中断都无法响应（更严重）
 * ============================================================================
 */

extern void touch_softlockup_watchdog_sched(void);
/* 重置软锁死 watchdog 时间戳（调度器路径）
 *
 * 【调用时机】
 * 调度器内部调用，每次发生进程切换时自动重置。
 *
 * 【为什么需要单独的函数？】
 * 调度器路径性能敏感，使用优化的实现（避免额外开销）。
 */

extern void touch_softlockup_watchdog(void);
/* 重置软锁死 watchdog 时间戳
 *
 * 【使用场景】
 * 内核代码需要长时间禁用抢占时主动调用，告诉 watchdog "我还活着"。
 *
 * 【典型场景】
 * - 长时间的内存拷贝（copy_to/from_user 大数据）
 * - 复杂的硬件初始化（可能需要轮询几秒）
 * - 刷新大量脏页到磁盘
 *
 * 【调用频率】
 * 应该在循环中定期调用（如每秒一次），不要等到接近阈值才调用。
 *
 * 【示例】
 * void long_running_function(void) {
 *     for (int i = 0; i < HUGE_NUMBER; i++) {
 *         // 做一些工作
 *         if (i % 1000000 == 0)
 *             touch_softlockup_watchdog();  // 定期重置
 *     }
 * }
 */

extern void touch_softlockup_watchdog_sync(void);
/* 同步重置软锁死 watchdog（等待重置完成）
 *
 * 【与 touch_softlockup_watchdog 的区别】
 * - touch_softlockup_watchdog: 异步，立即返回
 * - touch_softlockup_watchdog_sync: 同步，等待所有 CPU 的 watchdog 都重置
 *
 * 【使用场景】
 * 需要确保所有 CPU 的 watchdog 都已重置才能继续的场景：
 * - 系统挂起前（避免在挂起过程中误报）
 * - 长时间的全局操作（如系统范围的 TLB 刷新）
 */

extern void touch_all_softlockup_watchdogs(void);
/* 重置所有 CPU 的软锁死 watchdog
 *
 * 【使用场景】
 * 全局操作可能导致所有 CPU 都暂时无法调度时：
 * - 系统挂起/恢复（suspend/resume）
 * - 全局停止（stop_machine）
 * - kexec 加载新内核
 *
 * 【工作原理】
 * 遍历所有在线 CPU，重置每个 CPU 的时间戳。
 */

extern unsigned int  softlockup_panic;
/* 软锁死时是否触发 panic
 * - 0: 只打印警告，系统继续运行（默认）
 * - 1: 立即 panic，触发崩溃转储
 *
 * 【sysctl 接口】
 * /proc/sys/kernel/softlockup_panic
 *
 * 【使用场景】
 * - 开发/调试：设为 1，触发 kdump 收集崩溃信息
 * - 生产环境：通常设为 0（软锁死可能是暂时的）
 */

extern int lockup_detector_online_cpu(unsigned int cpu);
/* CPU 上线时的 lockup detector 回调
 * @cpu: 上线的 CPU 编号
 *
 * 返回值：0=成功，负值=失败
 *
 * 【工作内容】
 * 1. 为该 CPU 创建 watchdog 线程
 * 2. 初始化时间戳
 * 3. 启动高精度定时器
 */

extern int lockup_detector_offline_cpu(unsigned int cpu);
/* CPU 下线时的 lockup detector 回调
 * @cpu: 下线的 CPU 编号
 *
 * 返回值：0=成功，负值=失败
 *
 * 【工作内容】
 * 1. 停止该 CPU 的高精度定时器
 * 2. 停止 watchdog 线程
 * 3. 清理资源
 */

#else /* CONFIG_SOFTLOCKUP_DETECTOR */
/* CONFIG_SOFTLOCKUP_DETECTOR 未启用时的空实现 */
static inline void touch_softlockup_watchdog_sched(void) { }
static inline void touch_softlockup_watchdog(void) { }
static inline void touch_softlockup_watchdog_sync(void) { }
static inline void touch_all_softlockup_watchdogs(void) { }

#define lockup_detector_online_cpu	NULL
#define lockup_detector_offline_cpu	NULL
#endif /* CONFIG_SOFTLOCKUP_DETECTOR */

#ifdef CONFIG_DETECT_HUNG_TASK
/*
 * 【挂起任务检测器（Hung Task Detector）】
 *
 * 检测长时间处于 D 状态（TASK_UNINTERRUPTIBLE，不可中断睡眠）的任务。
 */

void reset_hung_task_detector(void);
/* 重置挂起任务检测器
 *
 * 【使用场景】
 * 某些操作可能合法地让任务长时间处于 D 状态时调用：
 * - 系统挂起（suspend）：所有任务冻结
 * - 大文件同步（sync）：等待 I/O 完成
 * - 网络文件系统超时：等待远程服务器响应
 *
 * 【工作原理】
 * 重置 khungtaskd 内核线程的检查计时器，避免误报。
 */
#else
static inline void reset_hung_task_detector(void) { }
#endif

/*
 * The run state of the lockup detectors is controlled by the content of the
 * 'watchdog_enabled' variable. Each lockup detector has its dedicated bit -
 * bit 0 for the hard lockup detector and bit 1 for the soft lockup detector.
 *
 * 'watchdog_user_enabled', 'watchdog_hardlockup_user_enabled' and
 * 'watchdog_softlockup_user_enabled' are variables that are only used as an
 * 'interface' between the parameters in /proc/sys/kernel and the internal
 * state bits in 'watchdog_enabled'. The 'watchdog_thresh' variable is
 * handled differently because its value is not boolean, and the lockup
 * detectors are 'suspended' while 'watchdog_thresh' is equal zero.
 */
/*
 * ============================================================================
 * 【锁死检测器运行状态控制】
 *
 * 【watchdog_enabled 位图】
 * 控制锁死检测器的启用状态，每个检测器占一位：
 * - bit 0: 硬锁死检测器
 * - bit 1: 软锁死检测器
 *
 * 【用户接口与内部状态的映射】
 * /proc/sys/kernel 中的参数          内部状态位
 * --------------------------------   ------------------
 * watchdog (总开关)                → watchdog_user_enabled
 * watchdog_thresh (阈值)            → watchdog_thresh (特殊处理)
 * hardlockup (硬锁死开关)           → watchdog_hardlockup_user_enabled
 * softlockup (软锁死开关)           → watchdog_softlockup_user_enabled
 *
 * 这些 *_user_enabled 变量是用户接口，最终映射到 watchdog_enabled 位图。
 *
 * 【watchdog_thresh 的特殊处理】
 * - 非布尔值（整数，表示秒数）
 * - 当 watchdog_thresh == 0 时，锁死检测器被"暂停"
 * - 这是临时禁用 watchdog 的推荐方式（不改变启用状态）
 *
 * 【为什么需要这种设计？】
 * - 用户接口简单：通过 sysctl 设置布尔值
 * - 内部实现高效：位图操作快速
 * - 状态持久化：禁用后再启用，保持之前的配置
 * ============================================================================
 */
#define WATCHDOG_HARDLOCKUP_ENABLED_BIT  0
/* 硬锁死检测器的位号 */

#define WATCHDOG_SOFTOCKUP_ENABLED_BIT   1
/* 软锁死检测器的位号 */

#define WATCHDOG_HARDLOCKUP_ENABLED     (1 << WATCHDOG_HARDLOCKUP_ENABLED_BIT)
/* 硬锁死检测器启用标志（bit 0 = 1）
 * 用于测试或设置 watchdog_enabled 的 bit 0
 */

#define WATCHDOG_SOFTOCKUP_ENABLED      (1 << WATCHDOG_SOFTOCKUP_ENABLED_BIT)
/* 软锁死检测器启用标志（bit 1 = 1）
 * 用于测试或设置 watchdog_enabled 的 bit 1
 */

#if defined(CONFIG_HARDLOCKUP_DETECTOR)
/*
 * ============================================================================
 * 【硬锁死检测器（Hard Lockup Detector）】
 *
 * 【检测原理】
 * CPU 长时间不响应中断（包括普通中断和调度器），通常由以下原因导致：
 * - 内核死循环（中断禁用状态）
 * - 硬件故障（CPU 挂死）
 * - 恶意或错误的内核代码
 *
 * 【实现方式】
 * 1. NMI watchdog（推荐）
 *    - 利用 CPU 性能计数器（PMU, Performance Monitoring Unit）
 *    - 配置计数器在一定周期后触发 NMI
 *    - NMI 处理程序检查 CPU 是否在持续进展
 *
 * 2. 高精度定时器（hrtimer）
 *    - 定时器触发时检查目标 CPU 的时间戳
 *    - 依赖其他 CPU 发送 IPI 检查
 *
 * 3. Buddy 检测器
 *    - 一个 CPU 监控另一个 CPU（伙伴系统）
 *    - 降低硬件依赖
 *
 * 【为什么需要 NMI？】
 * 普通中断可能被禁用（local_irq_disable），只有 NMI 能打断挂死的 CPU。
 * ============================================================================
 */

extern void hardlockup_detector_disable(void);
/* 禁用硬锁死检测器
 *
 * 【使用场景】
 * - 调试时临时禁用（避免干扰）
 * - 某些硬件不支持 NMI watchdog
 * - 虚拟化环境中可能误报
 */

extern unsigned int hardlockup_panic;
/* 硬锁死时是否触发 panic
 * - 0: 打印警告，系统继续（如果可能）
 * - 1: 立即 panic，触发崩溃转储
 *
 * 【sysctl 接口】
 * /proc/sys/kernel/hardlockup_panic
 *
 * 【推荐设置】
 * 生产环境通常设为 1，因为硬锁死几乎不可恢复，
 * panic 后由 kdump 收集信息并重启是更好的选择。
 */

extern unsigned long hardlockup_si_mask;
/* 硬锁死信息掩码（用于统计和调试）
 * 记录哪些信号被触发或检测到
 */
#else
static inline void hardlockup_detector_disable(void) {}
#endif

/* Sparc64 has special implemetantion that is always enabled. */
/*
 * SPARC64 的特殊实现（始终启用）
 * SPARC64 架构有内置的 watchdog，不需要软件配置。
 */
#if defined(CONFIG_HARDLOCKUP_DETECTOR) || defined(CONFIG_HARDLOCKUP_DETECTOR_SPARC64)
void arch_touch_nmi_watchdog(void);
/* 架构特定的 NMI watchdog 重置
 *
 * 【实现】
 * 各架构提供自己的实现：
 * - x86: 重置 PMU 计数器
 * - ARM: 重置 ARM PMU
 * - SPARC64: 重置硬件 watchdog
 *
 * 【调用者】
 * 通常不直接调用，而是通过 touch_nmi_watchdog() 间接调用。
 */
#else
static inline void arch_touch_nmi_watchdog(void) { }
#endif

#if defined(CONFIG_HARDLOCKUP_DETECTOR_COUNTS_HRTIMER)
/*
 * 【基于 hrtimer 的硬锁死检测】
 * 当硬件不支持 NMI watchdog 时的备选方案。
 */

void watchdog_hardlockup_touch_cpu(unsigned int cpu);
/* 重置指定 CPU 的硬锁死计数器
 * @cpu: 目标 CPU 编号
 *
 * 【使用场景】
 * 远程重置其他 CPU 的 watchdog（跨 CPU 操作）
 */

void watchdog_hardlockup_check(unsigned int cpu, struct pt_regs *regs);
/* 检查指定 CPU 是否发生硬锁死
 * @cpu:  要检查的 CPU 编号
 * @regs: 寄存器状态（用于打印栈回溯）
 *
 * 【调用时机】
 * hrtimer 触发时调用，检查目标 CPU 的时间戳。
 */
#endif

#if defined(CONFIG_HARDLOCKUP_DETECTOR_PERF)
/*
 * 【基于性能计数器（perf）的硬锁死检测】
 * 使用 CPU PMU（Performance Monitoring Unit）触发 NMI。
 */

extern void hardlockup_detector_perf_stop(void);
/* 停止基于 perf 的硬锁死检测
 *
 * 【调用时机】
 * - 系统挂起前
 * - 动态禁用 watchdog
 * - 性能分析工具独占 PMU 时
 */

extern void hardlockup_detector_perf_restart(void);
/* 重启基于 perf 的硬锁死检测
 *
 * 【调用时机】
 * - 系统恢复后
 * - 重新启用 watchdog
 */

extern void hardlockup_config_perf_event(const char *str);
/* 配置 perf 事件
 * @str: 事件配置字符串
 *
 * 【使用场景】
 * 自定义 PMU 事件（高级用户）
 */

extern void hardlockup_detector_perf_adjust_period(u64 period);
/* 调整 perf 事件的采样周期
 * @period: 新的采样周期（CPU 周期数）
 *
 * 【使用场景】
 * 根据 watchdog_thresh 动态调整检测频率
 */
#else
static inline void hardlockup_detector_perf_stop(void) { }
static inline void hardlockup_detector_perf_restart(void) { }
static inline void hardlockup_config_perf_event(const char *str) { }
static inline void hardlockup_detector_perf_adjust_period(u64 period) { }
#endif

void watchdog_hardlockup_stop(void);
/* 停止硬锁死 watchdog
 *
 * 【调用时机】
 * - 系统关机
 * - 用户通过 sysctl 禁用
 * - CPU 下线
 */

void watchdog_hardlockup_start(void);
/* 启动硬锁死 watchdog
 *
 * 【调用时机】
 * - 系统启动
 * - 用户通过 sysctl 启用
 * - CPU 上线
 */

int watchdog_hardlockup_probe(void);
/* 探测硬件是否支持硬锁死检测
 *
 * 返回值：0=支持，负值=不支持
 *
 * 【检查内容】
 * - CPU 是否有 PMU
 * - PMU 是否支持 NMI
 * - 架构是否实现了必要的钩子
 */

void watchdog_hardlockup_enable(unsigned int cpu);
/* 为指定 CPU 启用硬锁死检测
 * @cpu: 目标 CPU 编号
 *
 * 【工作内容】
 * 1. 初始化该 CPU 的 watchdog 数据结构
 * 2. 配置硬件（PMU 或 hrtimer）
 * 3. 启动检测
 */

void watchdog_hardlockup_disable(unsigned int cpu);
/* 为指定 CPU 禁用硬锁死检测
 * @cpu: 目标 CPU 编号
 *
 * 【工作内容】
 * 1. 停止硬件（PMU 或 hrtimer）
 * 2. 清理该 CPU 的 watchdog 数据
 */

void lockup_detector_reconfigure(void);
/* 重新配置锁死检测器
 *
 * 【调用时机】
 * - sysctl 参数改变时
 * - CPU hotplug 事件
 * - 从挂起状态恢复
 *
 * 【工作内容】
 * 根据当前配置重新初始化所有 CPU 的 watchdog
 */

#ifdef CONFIG_HARDLOCKUP_DETECTOR_BUDDY
/*
 * 【伙伴硬锁死检测器（Buddy Detector）】
 * 每个 CPU 监控另一个 CPU，无需硬件支持。
 */

void watchdog_buddy_check_hardlockup(int hrtimer_interrupts);
/* 伙伴 CPU 检查硬锁死
 * @hrtimer_interrupts: 当前的 hrtimer 中断计数
 *
 * 【工作原理】
 * CPU A 定期检查 CPU B 的时间戳，如果 B 长时间未更新，
 * 则判断 B 发生了硬锁死。
 */
#else
static inline void watchdog_buddy_check_hardlockup(int hrtimer_interrupts) {}
#endif

/**
 * touch_nmi_watchdog - manually reset the hardlockup watchdog timeout.
 *
 * If we support detecting hardlockups, touch_nmi_watchdog() may be
 * used to pet the watchdog (reset the timeout) - for code which
 * intentionally disables interrupts for a long time. This call is stateless.
 *
 * Though this function has "nmi" in the name, the hardlockup watchdog might
 * not be backed by NMIs. This function will likely be renamed to
 * touch_hardlockup_watchdog() in the future.
 */
/*
 * 【函数】touch_nmi_watchdog - 手动重置硬锁死 watchdog 超时
 *
 * 【功能说明】
 * 如果系统支持硬锁死检测，touch_nmi_watchdog() 用于"喂狗"（重置超时）。
 * 适用于需要长时间禁用中断的代码。
 *
 * 【状态】
 * 这是无状态调用（stateless）：可以从任何上下文安全调用，无需配对。
 *
 * 【命名说明】
 * 虽然函数名包含 "nmi"，但硬锁死 watchdog 未必由 NMI 实现。
 * 未来可能重命名为 touch_hardlockup_watchdog()。
 *
 * 【使用场景】
 * 代码需要长时间禁用中断时：
 * - 关键的硬件初始化（可能需要轮询几秒）
 * - 原子操作序列（不能被中断打断）
 * - 某些架构的 TLB 刷新操作
 *
 * 【调用频率】
 * 在长时间操作的循环中定期调用（如每秒一次）
 *
 * 【示例】
 * void hardware_init(void) {
 *     local_irq_disable();  // 禁用中断
 *     for (int i = 0; i < 100; i++) {
 *         // 初始化硬件（每次迭代约 100ms）
 *         init_hardware_step(i);
 *         touch_nmi_watchdog();  // 告诉 watchdog 我还活着
 *     }
 *     local_irq_enable();
 * }
 */
static inline void touch_nmi_watchdog(void)
{
	/*
	 * Pass on to the hardlockup detector selected via CONFIG_. Note that
	 * the hardlockup detector may not be arch-specific nor using NMIs
	 * and the arch_touch_nmi_watchdog() function will likely be renamed
	 * in the future.
	 */
	/*
	 * 转发给通过 CONFIG_ 选择的硬锁死检测器。
	 * 注意硬锁死检测器可能不是架构特定的，也可能不使用 NMI，
	 * arch_touch_nmi_watchdog() 未来可能会重命名。
	 */
	arch_touch_nmi_watchdog();

	touch_softlockup_watchdog();
	/* 同时重置软锁死 watchdog
	 * 因为禁用中断通常也会阻止调度，可能触发软锁死
	 */
}

/*
 * Create trigger_all_cpu_backtrace() out of the arch-provided
 * base function. Return whether such support was available,
 * to allow calling code to fall back to some other mechanism:
 */
/*
 * ============================================================================
 * 【CPU 栈回溯触发函数】
 *
 * 基于架构提供的底层函数创建 trigger_all_cpu_backtrace()。
 * 返回值表示是否支持该功能，允许调用者回退到其他机制。
 *
 * 【用途】
 * 调试工具，用于获取所有（或部分）CPU 的栈回溯：
 * - 死锁调试：查看所有 CPU 在做什么
 * - 性能分析：采样所有 CPU 的执行状态
 * - 挂起调试：了解系统为何无响应
 *
 * 【实现原理】
 * 向目标 CPU 发送 NMI（或架构特定的中断），强制其打印栈回溯。
 * ============================================================================
 */
#ifdef arch_trigger_cpumask_backtrace
/* 架构提供了底层实现 */

static inline bool trigger_all_cpu_backtrace(void)
{
	arch_trigger_cpumask_backtrace(cpu_online_mask, -1);
	return true;
}
/* 触发所有在线 CPU 的栈回溯
 *
 * 返回值：true=已触发，false=不支持
 *
 * 【使用场景】
 * - 系统挂起时查看所有 CPU 状态
 * - SysRq 键盘命令（Alt+SysRq+L）
 * - 软/硬锁死检测触发时（如果配置了 all_cpu_backtrace）
 *
 * 【输出】
 * 所有 CPU 的栈回溯打印到 dmesg/控制台
 */

static inline bool trigger_allbutcpu_cpu_backtrace(int exclude_cpu)
{
	arch_trigger_cpumask_backtrace(cpu_online_mask, exclude_cpu);
	return true;
}
/* 触发除指定 CPU 外所有 CPU 的栈回溯
 * @exclude_cpu: 排除的 CPU 编号（通常是当前 CPU）
 *
 * 返回值：true=已触发，false=不支持
 *
 * 【使用场景】
 * 当前 CPU 已经打印了自己的栈，只需要其他 CPU 的栈：
 * - lockup detector 检测到当前 CPU 锁死
 * - 当前 CPU 触发 panic，需要其他 CPU 的状态
 */

static inline bool trigger_cpumask_backtrace(struct cpumask *mask)
{
	arch_trigger_cpumask_backtrace(mask, -1);
	return true;
}
/* 触发指定 CPU 集合的栈回溯
 * @mask: 目标 CPU 掩码
 *
 * 返回值：true=已触发，false=不支持
 *
 * 【使用场景】
 * 只关心特定 CPU 的状态：
 * - 调试特定 NUMA 节点的问题
 * - 检查特定进程亲和的 CPU 集合
 */

static inline bool trigger_single_cpu_backtrace(int cpu)
{
	arch_trigger_cpumask_backtrace(cpumask_of(cpu), -1);
	return true;
}
/* 触发单个 CPU 的栈回溯
 * @cpu: 目标 CPU 编号
 *
 * 返回值：true=已触发，false=不支持
 *
 * 【使用场景】
 * 调试特定 CPU 的行为：
 * - 某个 CPU 看起来异常
 * - 远程调试（查看另一个 CPU 在做什么）
 */

/* generic implementation */
void nmi_trigger_cpumask_backtrace(const cpumask_t *mask,
				   int exclude_cpu,
				   void (*raise)(cpumask_t *mask));
/* 通用的 CPU 掩码栈回溯触发函数（架构实现调用）
 * @mask:        目标 CPU 掩码
 * @exclude_cpu: 排除的 CPU（-1 表示不排除）
 * @raise:       架构特定的"触发"函数（如发送 NMI）
 *
 * 【架构实现指南】
 * 架构需要提供 arch_trigger_cpumask_backtrace，通常是调用此函数：
 * void arch_trigger_cpumask_backtrace(const cpumask_t *mask, int exclude_cpu) {
 *     nmi_trigger_cpumask_backtrace(mask, exclude_cpu, arch_send_call_function_ipi_mask);
 * }
 */

bool nmi_cpu_backtrace(struct pt_regs *regs);
/* NMI 处理程序调用：打印当前 CPU 的栈回溯
 * @regs: 中断时的寄存器状态
 *
 * 返回值：true=已处理，false=不是 backtrace NMI
 *
 * 【调用者】
 * 架构的 NMI 处理程序，当检测到这是 backtrace 请求时调用。
 *
 * 【工作流程】
 * 1. 检查是否是 backtrace NMI（通过全局标志）
 * 2. 打印当前 CPU 的栈回溯
 * 3. 清除 backtrace 请求标志
 * 4. 返回 true 表示已处理
 */

#else
/* 架构不支持 CPU backtrace */
static inline bool trigger_all_cpu_backtrace(void)
{
	return false;
}
static inline bool trigger_allbutcpu_cpu_backtrace(int exclude_cpu)
{
	return false;
}
static inline bool trigger_cpumask_backtrace(struct cpumask *mask)
{
	return false;
}
static inline bool trigger_single_cpu_backtrace(int cpu)
{
	return false;
}
#endif

#ifdef CONFIG_HARDLOCKUP_DETECTOR_PERF
/*
 * 【基于 perf 的硬锁死检测器辅助函数】
 */

u64 hw_nmi_get_sample_period(int watchdog_thresh);
/* 根据 watchdog 阈值计算硬件采样周期
 * @watchdog_thresh: watchdog 阈值（秒）
 *
 * 返回值：采样周期（CPU 周期数）
 *
 * 【工作原理】
 * PMU 计数器每隔 period 个 CPU 周期触发一次 NMI。
 * period = CPU_频率 * watchdog_thresh
 *
 * 【示例】
 * watchdog_thresh = 10 秒，CPU 频率 = 2GHz
 * period = 2,000,000,000 * 10 = 20,000,000,000 周期
 */

bool arch_perf_nmi_is_available(void);
/* 检查架构是否支持基于 perf 的 NMI
 *
 * 返回值：true=支持，false=不支持
 *
 * 【检查内容】
 * - CPU 是否有 PMU（Performance Monitoring Unit）
 * - PMU 是否能触发 NMI
 * - 是否有可用的性能计数器
 *
 * 【使用场景】
 * 初始化时判断使用哪种硬锁死检测方式
 */
#endif

#if defined(CONFIG_HARDLOCKUP_CHECK_TIMESTAMP) && \
    defined(CONFIG_HARDLOCKUP_DETECTOR_PERF)
/*
 * 【基于时间戳检查的硬锁死检测】
 */

void watchdog_update_hrtimer_threshold(u64 period);
/* 更新 hrtimer 阈值
 * @period: 新的阈值（纳秒）
 *
 * 【使用场景】
 * watchdog_thresh 改变时，同步更新 hrtimer 的触发周期
 */
#else
static inline void watchdog_update_hrtimer_threshold(u64 period) { }
#endif

#ifdef CONFIG_HAVE_ACPI_APEI_NMI
/*
 * 【ACPI APEI NMI 支持】
 * APEI (ACPI Platform Error Interface) 用于硬件错误报告。
 * 某些硬件错误通过 NMI 报告。
 */
#include <asm/nmi.h>
#endif

#ifdef CONFIG_NMI_CHECK_CPU
/*
 * 【NMI backtrace 停顿检测】
 * 检测 backtrace 过程中是否有 CPU 长时间未响应。
 */

void nmi_backtrace_stall_snap(const struct cpumask *btp);
/* 拍摄 backtrace 开始时的快照
 * @btp: 请求 backtrace 的 CPU 掩码
 *
 * 【使用场景】
 * trigger_cpumask_backtrace() 开始时调用，
 * 记录当前时间戳，用于后续检测超时。
 */

void nmi_backtrace_stall_check(const struct cpumask *btp);
/* 检查 backtrace 是否停顿
 * @btp: 请求 backtrace 的 CPU 掩码
 *
 * 【工作原理】
 * 检查距离 snap 时间是否超过阈值，
 * 如果某些 CPU 长时间未响应，打印警告。
 *
 * 【使用场景】
 * - CPU 真的挂死，无法响应 NMI
 * - NMI 被禁用或阻塞（硬件问题）
 * - 调试 backtrace 机制本身
 */
#else
static inline void nmi_backtrace_stall_snap(const struct cpumask *btp) {}
static inline void nmi_backtrace_stall_check(const struct cpumask *btp) {}
#endif

#endif /* LINUX_NMI_H */
