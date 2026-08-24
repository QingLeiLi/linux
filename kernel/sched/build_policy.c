// SPDX-License-Identifier: GPL-2.0-only
/*
 * These are the scheduling policy related scheduler files, built
 * in a single compilation unit for build efficiency reasons.
 *
 * ( Incidentally, the size of the compilation unit is roughly
 *   comparable to core.c and fair.c, the other two big
 *   compilation units. This helps balance build time, while
 *   coalescing source files to amortize header inclusion
 *   cost. )
 *
 * core.c and fair.c are built separately.
 */
/*
 * 调度策略聚合编译单元学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 上述原文说明：这里把与调度策略有关的源文件合并成一个翻译单元，以摊薄重复解析
 * 大型公共头文件的成本；该单元规模刻意与单独编译的 core.c、fair.c 接近，使并行构建
 * 更均衡。Makefile 将本文件编译成 build_policy.o 并链接进 sched/built-in.a；这是一项
 * 构建组织策略，不表示下列源文件在运行时按文本顺序执行。
 *
 * `.c` 被 include 后，预处理器会把它们依次展开到同一编译单元：共享前面的头文件、
 * 编译选项和静态符号命名空间，也让前文定义可被后文直接看见。因此本文件不拥有独立
 * 运行时对象、函数或失败路径，但 include 集合与顺序是构建契约，不能把某个成员同时
 * 单独编译，也不能未核对静态名称/声明依赖就任意重排。启用分支分析时，Makefile 还为
 * 整个对象定义 DISABLE_BRANCH_PROFILING，避免聚合进来的 noinstr 调度路径被插桩。
 *
 * 聚合边界覆盖 idle、RT、DL、PELT、CPU 时间、sched_ext 和调度系统调用；核心调度与
 * CFS 公平类仍分别位于 core.o/fair.o。收益是降低头文件解析总成本，代价是任一成员变化
 * 都会重编整个对象、单元内静态符号必须全局唯一，且诊断位置可能跨越被包含的源文件。
 */

/* Headers: */
/*
 * 原文“头文件”：先集中建立所有成员需要的公共类型、配置和 helper 声明。第一组是
 * 调度器专用接口：clock/cputime/hotplug/isolation/posix timer/RT 分别提供时钟、CPU
 * 记账、热插拔、隔离 CPU、定时器和实时策略契约；成员 `.c` 无需各自重复解析。
 */
#include <linux/sched/clock.h>
#include <linux/sched/cputime.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/isolation.h>
#include <linux/sched/posix-timers.h>
#include <linux/sched/rt.h>

/*
 * 第二组提供 idle、时间基准、sysfs/livepatch、电源管理、PSI、哈希表、顺序缓冲区、
 * seqcount、内存分配、挂起、进程时间统计、虚拟 CPU 时间、SysRq 和 percpu rwsem 等
 * 基础设施。它们只引入声明/宏；具体所有权、锁和睡眠规则仍由各成员实现负责。
 */
#include <linux/cpuidle.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/livepatch.h>
#include <linux/pm.h>
#include <linux/psi.h>
#include <linux/rhashtable.h>
#include <linux/seq_buf.h>
#include <linux/seqlock_api.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/tsacct_kern.h>
#include <linux/vtime.h>
#include <linux/sysrq.h>
#include <linux/percpu-rwsem.h>

/* 用户 ABI 的 sched_attr 等布局来自 UAPI；聚合成员必须与用户空间可见结构保持一致。 */
#include <uapi/linux/sched/types.h>

/* 内部 sched.h 建立 rq/task_group/class 公共模型，smp.h 提供跨 CPU 调度接口。 */
#include "sched.h"
#include "smp.h"

/* 三个局部头分别公开自动分组、调度统计和 PELT 跨成员契约。 */
#include "autogroup.h"
#include "stats.h"
#include "pelt.h"

/* Source code modules: */
/*
 * 原文“源代码模块”：从这里开始是文本合并而非普通头文件依赖。idle.c 提供 idle 类和
 * CPU idle 进入/退出边界；它仍按任务清单明确排除学习补注，但构建上必须参与本对象。
 */

#include "idle.c"

/* RT 类实现先建立固定优先级队列，cpudeadline.c 随后提供 DL 使用的 CPU 截止期索引。 */
#include "rt.c"
#include "cpudeadline.c"

/* PELT 实现维护调度实体/运行队列衰减信号，供后续策略和统计路径消费。 */
#include "pelt.c"

/* cputime.c 负责任务/线程组 CPU 时间归集，deadline.c 实现 EDF/CBS 截止期调度类。 */
#include "cputime.c"
#include "deadline.c"

/*
 * sched_ext 仅在配置启用时并入同一对象。先补齐 BTF、位搜索和 gen_pool 公共依赖，再按
 * types → internal → 子模块接口 → 主实现/子实现的次序展开。ext.c 定义核心调度类和
 * BPF 控制面，cid/arena/idle 实现其 CPU-ID、共享 arena 与空闲选核子系统；关闭配置时
 * 整段不进入预处理结果，也不产生这些符号或资源生命周期。
 */
#ifdef CONFIG_SCHED_CLASS_EXT
# include <linux/btf_ids.h>
# include <linux/find.h>
# include <linux/genalloc.h>
# include "ext/types.h"
# include "ext/internal.h"
# include "ext/cid.h"
# include "ext/arena.h"
# include "ext/idle.h"
# include "ext/ext.c"
# include "ext/cid.c"
# include "ext/arena.c"
# include "ext/idle.c"
#endif

/* 最后并入调度系统调用/属性入口，使其调用前面各策略实现公开的校验与状态转换接口。 */
#include "syscalls.c"
