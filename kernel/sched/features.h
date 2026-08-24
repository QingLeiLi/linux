/* SPDX-License-Identifier: GPL-2.0 */

/*
 * 调度器实验特性表学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本文件故意没有 include guard，也不是普通声明头：调用者先把 SCHED_FEAT(name, default)
 * 定义成不同生成宏，再重复 include 本表，分别产生枚举编号、默认位图、特性名称、
 * static-key 初值和每项查询 helper。每行第二个布尔值是编译配置允许时的启动默认值；
 * 配置条件可改变默认值或彻底移除条目，而不是在这里直接执行调度逻辑。
 *
 * `sched_feat(NAME)` 是各热路径的消费边界。有 jump label 时，debug.c 维护与特性位同步
 * 的 static key，使默认稳定分支接近零额外开销；否则读取 sysctl_sched_features 位图。
 * 调试接口可用 `NAME`/`NO_NAME` 动态切换，但这种切换是全局策略实验，不提供任务级
 * ownership 或事务回滚，改变后只影响随后经过相应分支的调度决策。
 *
 * 条目大致分为 EEVDF 放置/抢占、buddy/cache、延迟出队、精确 tick、容量与远端唤醒、
 * RT push/runtime、负载均衡/wake-affine、利用率估计及 newidle 节流。启用某项获得对应
 * 公平性、局部性或诊断能力，也可能增加 IPI、扫描、计时器或一致性检查成本。
 */

/*
 * Using the avg_vruntime, do the right thing and preserve lag across
 * sleep+wake cycles. EEVDF placement strategy #1, #2 if disabled.
 */
/*
 * 利用 cfs_rq 的平均虚拟运行时间在睡眠—唤醒后保留实体 lag；开启采用 EEVDF 放置
 * 策略 1，关闭退回策略 2。它决定唤醒实体的 vruntime/deadline 基准，避免睡眠本身
 * 无条件抹掉欠账或信用；代价是必须维护并读取 avg_vruntime。
 */
SCHED_FEAT(PLACE_LAG, true)
/*
 * Give new tasks half a slice to ease into the competition.
 */
/* 新任务初始只给半个 slice，使其渐进参与竞争；后续正常重算 deadline，不改变权重。 */
SCHED_FEAT(PLACE_DEADLINE_INITIAL, true)
/*
 * Preserve relative virtual deadline on 'migration'.
 */
/* “迁移”实体时保留相对虚拟 deadline，避免仅因换 cfs_rq 而重获或丢失调度紧迫性。 */
SCHED_FEAT(PLACE_REL_DEADLINE, true)
/*
 * Inhibit (wakeup) preemption until the current task has either matched the
 * 0-lag point or until is has exhausted it's slice.
 */
/*
 * 当前实体到达零 lag（与公平份额持平）或耗尽 slice 前，抑制普通唤醒抢占；这让已开始
 * 的服务运行到 EEVDF parity，降低切换抖动，但可能延后新唤醒实体开始执行。
 */
SCHED_FEAT(RUN_TO_PARITY, true)
/*
 * Allow wakeup of tasks with a shorter slice to cancel RUN_TO_PARITY for
 * current.
 */
/* 唤醒实体 slice 更短时允许突破 RUN_TO_PARITY，以免低延迟请求被长 slice 当前任务阻塞。 */
SCHED_FEAT(PREEMPT_SHORT, true)

/*
 * Prefer to schedule the task we woke last (assuming it failed
 * wakeup-preemption), since its likely going to consume data we
 * touched, increases cache locality.
 */
/*
 * 最近唤醒但未能立即抢占的实体可标为 next buddy，下次 pick 时优先复用双方刚触碰的数据；
 * 默认关闭，避免局部性启发式过度偏离 EEVDF 的正常资格/deadline 次序。
 */
SCHED_FEAT(NEXT_BUDDY, false)

/*
 * Allow completely ignoring cfs_rq->next; which can be set from various
 * places:
 *   - NEXT_BUDDY (wakeup preemption)
 *   - yield_to_task()
 *   - cgroup dequeue / pick
 */
/*
 * 允许 pick 路径实际考虑 cfs_rq->next；关闭时完全忽略该提示。next 可能来自 NEXT_BUDDY、
 * yield_to_task() 或 cgroup 出队/选择，它只是候选偏好，不绕过实体资格和队列有效性检查。
 */
SCHED_FEAT(PICK_BUDDY, true)

/*
 * Consider buddies to be cache hot, decreases the likeliness of a
 * cache buddy being migrated away, increases cache locality.
 */
/* 把 buddy 视为 cache-hot，降低负载均衡迁走它的可能性；收益是局部性，代价是迁移选择受限。 */
SCHED_FEAT(CACHE_HOT_BUDDY, true)

/*
 * Delay dequeueing tasks until they get selected or woken.
 *
 * By delaying the dequeue for non-eligible tasks, they remain in the
 * competition and can burn off their negative lag. When they get selected
 * they'll have positive lag by definition.
 *
 * DELAY_ZERO clips the lag on dequeue (or wakeup) to 0.
 */
/*
 * 延迟出队让暂时不 eligible 的睡眠实体继续留在竞争模型中消耗负 lag，直到被选中或再次
 * 唤醒；被真正选中时按定义已具有正 lag。DELAY_ZERO 又在出队/唤醒边界把 lag 上限裁到
 * 0，避免携带正信用。两项减少睡眠操纵公平性的空间，但增加队列状态与选择逻辑复杂度。
 */
SCHED_FEAT(DELAY_DEQUEUE, true)
SCHED_FEAT(DELAY_ZERO, true)

/*
 * 使用带乘加溢出检查的 avg_vruntime 更新；溢出时提高 sum_shift、遍历时间线重建加权和，
 * 连续移位仍无法容纳则 BUG。默认关闭，以避免热路径检查和重建成本。
 */
SCHED_FEAT(PARANOID_AVG, false)

/*
 * Allow wakeup-time preemption of the current task:
 */
/* 允许公平类在唤醒时比较当前实体并请求抢占；关闭后新任务等待常规 tick/调度点。 */
SCHED_FEAT(WAKEUP_PREEMPTION, true)

/*
 * 只有体系结构/时钟事件层支持延后重装 hrtimer 时，默认启用高精度调度 tick：HRTICK
 * 服务公平类精确 slice 边界，HRTICK_DL 服务 deadline 类；否则同名条目仍存在但默认
 * false，运行依赖周期 tick 等较粗粒度事件。动态打开仍不能弥补底层能力缺失。
 */
#ifdef CONFIG_HRTIMER_REARM_DEFERRED
SCHED_FEAT(HRTICK, true)
SCHED_FEAT(HRTICK_DL, true)
#else
SCHED_FEAT(HRTICK, false)
SCHED_FEAT(HRTICK_DL, false)
#endif

/*
 * Decrement CPU capacity based on time not spent running tasks
 */
/* 将 IRQ/steal 等非 task 执行时间从 CPU 可用容量估计中扣除，避免负载均衡高估可运行能力。 */
SCHED_FEAT(NONTASK_CAPACITY, true)

/*
 * PREEMPT_RT 把 TTWU_QUEUE 的启动默认值设为 false，此时调用者走直接锁目标 rq 的路径；
 * 非 RT 默认把符合条件的远端唤醒排入目标 CPU call-single 队列，由 scheduler IPI 消费。
 */
#ifdef CONFIG_PREEMPT_RT
SCHED_FEAT(TTWU_QUEUE, false)
#else

/*
 * Queue remote wakeups on the target CPU and process them
 * using the scheduler IPI. Reduces rq->lock contention/bounces.
 */
/*
 * 原文说明：远端唤醒先在目标 CPU 排队并由调度 IPI 处理，可减少唤醒 CPU 争用及来回
 * 传递目标 rq->lock；代价是一次队列发布/IPI 和可能的处理延迟。
 */
SCHED_FEAT(TTWU_QUEUE, true)
#endif

/*
 * When doing wakeups, attempt to limit superfluous scans of the LLC domain.
 */
/* 唤醒选核用利用率估计限制无收益的 LLC 扫描，减少大缓存域遍历成本，可能更早停止搜索。 */
SCHED_FEAT(SIS_UTIL, true)

/*
 * Issue a WARN when we do multiple update_rq_clock() calls
 * in a single rq->lock section. Default disabled because the
 * annotations are not complete.
 */
/*
 * 同一 rq->lock 临界区重复 update_rq_clock() 时报警；因调用点注解尚未完整，默认关闭以
 * 避免假阳性。它只诊断时钟更新协议，不修复重复更新，也不改变 rq 锁 ownership。
 */
SCHED_FEAT(WARN_DOUBLE_CLOCK, false)

#ifdef HAVE_RT_PUSH_IPI
/*
 * In order to avoid a thundering herd attack of CPUs that are
 * lowering their priorities at the same time, and there being
 * a single CPU that has an RT task that can migrate and is waiting
 * to run, where the other CPUs will try to take that CPUs
 * rq lock and possibly create a large contention, sending an
 * IPI to that CPU and let that CPU push the RT task to where
 * it should go may be a better scenario.
 *
 * This is best for PREEMPT_RT, but for non-RT it can cause issues
 * when preemption is disabled for long periods of time. Have
 * it only default enabled for PREEMPT_RT.
 */
/*
 * 原文说明：多个 CPU 同时降低 RT 优先级时，若都争抢唯一持有可迁移 RT task 的 rq 锁，
 * 会形成惊群；向源 CPU 发 IPI、由它主动 push 可集中仲裁并减少锁争用。PREEMPT_RT 的
 * 抢占延迟更可控，故默认开启；非 RT 可能长时间关抢占而延误 IPI，默认关闭。
 */
# ifdef CONFIG_PREEMPT_RT
SCHED_FEAT(RT_PUSH_IPI, true)
# else
SCHED_FEAT(RT_PUSH_IPI, false)
# endif
#endif

/* 允许 RT 运行队列跨 CPU 借用/归还带宽；默认关闭，避免共享 runtime 的锁与记账复杂度。 */
SCHED_FEAT(RT_RUNTIME_SHARE, false)
/* 负载迁移时跳过极小负载实体的启发式；默认关闭，保持完整候选扫描。 */
SCHED_FEAT(LB_MIN, false)
/* task attach 到新 cfs_rq 时按两侧时钟差老化 PELT 负载，避免搬运未衰减的旧历史。 */
SCHED_FEAT(ATTACH_AGE_LOAD, true)

/* wake-affine 先尝试 idle 条件，把同步/空闲局部性作为目标 CPU 提示。 */
SCHED_FEAT(WA_IDLE, true)
/* idle 启发式未决定时比较 this/prev CPU 的有效负载与容量。 */
SCHED_FEAT(WA_WEIGHT, true)
/* 在 WA_WEIGHT 比较中加入调度域 imbalance_pct 偏置，避免收益过小时迁移。 */
SCHED_FEAT(WA_BIAS, true)

/*
 * UtilEstimation. Use estimated CPU utilization.
 */
/*
 * 启用利用率估计：在 PELT 尚未追上突发负载时结合 enqueue/runnable 估值，为选核和
 * cpufreq 提供更快响应；代价是维护额外 per-task/per-rq 状态且估计可能短暂偏高。
 */
SCHED_FEAT(UTIL_EST, true)

/* need_resched 持续超过阈值时发调度延迟警告；默认关闭，仅作全局诊断且不主动抢占。 */
SCHED_FEAT(LATENCY_WARN, false)

/*
 * Do newidle balancing proportional to its success rate using randomization.
 */
/*
 * newidle balance 按历史成功率随机抽样：NI_RANDOM 决定是否掷 1024 面概率，NI_RATE
 * 决定是否随时间更新成功比率。两者默认开启，用少量可能错过的拉取机会换取空闲切换
 * 热路径更少的无效调度域扫描。
 */
SCHED_FEAT(NI_RANDOM, true)
SCHED_FEAT(NI_RATE, true)
