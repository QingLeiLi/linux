// SPDX-License-Identifier: GPL-2.0-only
/*
 * These are various utility functions of the scheduler,
 * built in a single compilation unit for build efficiency reasons.
 *
 * ( Incidentally, the size of the compilation unit is roughly
 *   comparable to core.c, fair.c, smp.c and policy.c, the other
 *   big compilation units. This helps balance build time, while
 *   coalescing source files to amortize header inclusion
 *   cost. )
 */
/*
 * 调度器通用设施聚合编译单元学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 上述原文说明：各类调度通用设施为提高构建效率而合并进一个翻译单元；其规模与其他
 * 大型调度对象接近，便于并行构建同时结束，并通过只解析一次公共头文件摊薄成本。
 * Makefile 把本文件编译成 build_utility.o、归档进 sched/built-in.a；这里的文本排列
 * 不是运行时调用链，也不是初始化先后承诺。
 *
 * 下方 `.c` include 会共享所有头文件、编译选项和 static 符号命名空间。成员之间可见
 * 前面已经定义的内部符号，因此集合与顺序属于构建契约；重复单独编译同一成员会造成
 * 符号冲突。配置分支在预处理期决定某项设施是否进入对象。启用分支分析时 Makefile
 * 对整个对象设置 DISABLE_BRANCH_PROFILING，保护聚合成员中的 noinstr/低层调度路径。
 *
 * 本单元覆盖时钟、统计、等待、CPU 优先级、拓扑、压力、membarrier、隔离和自动分组等
 * 横向能力。聚合降低全量构建的解析成本，代价是任一成员变化都重编整个对象，静态名称
 * 必须唯一，且一个条件分支的头文件/配置错误会阻止整个 utility 对象生成。
 */

/* 调度专用头建立 clock/cputime/debug/isolation/loadavg/nohz/mm/rseq/task-stack 契约。 */
#include <linux/sched/clock.h>
#include <linux/sched/cputime.h>
#include <linux/sched/debug.h>
#include <linux/sched/isolation.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/nohz.h>
#include <linux/sched/mm.h>
#include <linux/sched/rseq_api.h>
#include <linux/sched/task_stack.h>

/*
 * 通用基础设施头提供 CPU 频率与掩码/cpuset、字符解析、debugfs、能耗模型、哈希表、
 * IRQ、kobject、membarrier、NUMA 策略、NMI、nospec、proc/PSI/ptrace、sched_clock、
 * 安全 hook、锁与等待队列、时间/UTS 和 workqueue 接口。它们只提供声明与宏；成员实现
 * 仍须分别遵守相应锁、RCU、上下文和资源释放契约。
 */
#include <linux/cpufreq.h>
#include <linux/cpumask_api.h>
#include <linux/cpuset.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/energy_model.h>
#include <linux/hashtable_api.h>
#include <linux/irq.h>
#include <linux/kobject_api.h>
#include <linux/membarrier.h>
#include <linux/mempolicy.h>
#include <linux/nmi.h>
#include <linux/nospec.h>
#include <linux/proc_fs.h>
#include <linux/psi.h>
#include <linux/ptrace_api.h>
#include <linux/sched_clock.h>
#include <linux/security.h>
#include <linux/spinlock_api.h>
#include <linux/swait_api.h>
#include <linux/timex.h>
#include <linux/utsname.h>
#include <linux/wait_api.h>
#include <linux/workqueue_api.h>

/* 用户 ABI 的 prctl 与 sched_attr 布局必须和系统调用/调试成员看到的类型一致。 */
#include <uapi/linux/prctl.h>
#include <uapi/linux/sched/types.h>

/* 体系结构 context-switch hook 为调试、统计和切换关联设施提供底层接口。 */
#include <asm/switch_to.h>

/* 内部核心模型、生成的 PELT 常量、统计及自动分组接口供所有聚合成员共用。 */
#include "sched.h"
#include "sched-pelt.h"
#include "stats.h"
#include "autogroup.h"

/* clock.c 建立 sched_clock 到每 CPU/运行队列调度时钟的更新和稳定读取设施。 */
#include "clock.c"

/* CPU cgroup 记账仅在配置启用时产生 per-cpu usage 归集和 cgroup 文件接口。 */
#ifdef CONFIG_CGROUP_CPUACCT
# include "cpuacct.c"
#endif

/* cpufreq.c 仅在 CPU_FREQ 下把调度利用率变化发布给频率 governor hook。 */
#ifdef CONFIG_CPU_FREQ
# include "cpufreq.c"
#endif

/* schedutil governor 只在其专用配置下并入；它把调度利用率信号转换为频率更新请求。 */
#ifdef CONFIG_CPU_FREQ_GOV_SCHEDUTIL
# include "cpufreq_schedutil.c"
#endif

/* debug.c 提供 sched_debug、sysrq/proc/debugfs 状态输出及一致快照辅助。 */
#include "debug.c"

/* 仅 SCHEDSTATS 配置生成调度延迟/等待统计的运行时开关与导出路径。 */
#ifdef CONFIG_SCHEDSTATS
# include "stats.c"
#endif

/* loadavg.c 维护系统负载平均；紧随的四个成员实现 completion、简单等待及位等待通用协议。 */
#include "loadavg.c"
#include "completion.c"
#include "swait.c"
#include "wait_bit.c"
#include "wait.c"

/* cpupri.c 提供 RT CPU 优先级候选索引；stop_task.c 定义最高优先级的 CPU stopper 类。 */
#include "cpupri.c"
#include "stop_task.c"

/* topology.c 构造 sched_domain/root_domain，并处理 CPU 热插拔时的运行队列归属。 */
#include "topology.c"

/* core scheduling 配置启用时加入按安全 cookie 协调 SMT siblings 的共同调度协议。 */
#ifdef CONFIG_SCHED_CORE
# include "core_sched.c"
#endif

/* PSI 配置启用时加入 task 状态变化到 CPU/内存/I/O 压力窗口和触发器的记账。 */
#ifdef CONFIG_PSI
# include "psi.c"
#endif

/* membarrier 配置启用时加入进程级快速路径登记和跨 CPU 内存屏障调度钩子。 */
#ifdef CONFIG_MEMBARRIER
# include "membarrier.c"
#endif

/* CPU isolation 配置启用时解析隔离参数并向调度/工作队列等调用者提供掩码。 */
#ifdef CONFIG_CPU_ISOLATION
# include "isolation.c"
#endif

/* 自动分组配置启用时加入会话 task_group 的创建、引用、迁移和 proc nice 控制。 */
#ifdef CONFIG_SCHED_AUTOGROUP
# include "autogroup.c"
#endif
