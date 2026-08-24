/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
/*
 * BPF 可扩展调度类的调度器内部接口，整体设计见
 * Documentation/scheduler/sched-ext.rst。
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本头文件把通用调度核心、fork、CPU 热插拔、NO_HZ、CPUFreq 和 cgroup 路径连接到
 * sched_ext 实现。CONFIG_SCHED_CLASS_EXT 打开时，状态转换由 ext.c/idle.c 完成；关闭时
 * 同名 static inline stub 保留调用面，并选择“不改变普通调度行为”的中性返回值。所有
 * task/rq/task_group 指针均为调用者在既有锁与生命周期协议内借用，本接口不凭空取得引用。
 *
 * 关键不变量是配置关闭后不能让 SCX 阻止 fork、唤醒、调度策略变更或 tick 停止；配置打开
 * 后则必须尊重 SCX 的启停状态、task 状态机、rq 锁和 cgroup 两阶段迁移协议。声明本身不
 * 提供同步，具体入口要求在各函数契约中说明。
 */
#ifdef CONFIG_SCHED_CLASS_EXT

/*
 * scx_tick() - 在调度 tick 中执行 SCX 全局看门狗与负载记账
 * @rq: 当前 CPU 的输入输出运行队列，调用者持有 rq 锁且已稳定其生命周期。
 * 返回：无直接返回值。SCX 未启用时快速返回；启用时检查 watchdog 是否失联并更新其他
 * PELT 信号，可能触发 scheduler 退出。运行在 tick/原子上下文，不得睡眠。
 */
void scx_tick(struct rq *rq);
/*
 * init_scx_entity() - 把新 task 内嵌的 sched_ext_entity 初始化为未入队状态
 * @scx: 纯输出、不可为 NULL 的 task 内嵌实体；调用者独占尚未发布的 task。
 * 返回：无直接返回值。清零字段、初始化链表/RB 节点和默认 slice/CPU/DSQ 哨兵；无失败、
 * 无分配、不可睡眠，不转移实体 ownership。
 */
void init_scx_entity(struct sched_ext_entity *scx);
/*
 * scx_pre_fork() - 在 fork 构造 SCX 状态前取得 fork 读侧门闩
 * @p: 正在构造的 child，借用且尚未对外发布；当前实现不解引用它。
 * 返回：无直接返回值。取得 scx_fork_rwsem 读锁，可能睡眠；必须由 post/cancel 路径释放，
 * 用来与极冷的 SCX 全任务启停写侧互斥。
 */
void scx_pre_fork(struct task_struct *p);
/*
 * scx_fork() - 为 child 分配 SCX task id，并在需要时执行 BPF init_task
 * @p: 输入输出 child，调用者仍拥有且 scx_fork_rwsem 读锁已由 scx_pre_fork() 持有。
 * @kargs: 借用 clone 参数；子调度器配置下从其 cgroup 集合选择目标 scheduler。
 * 返回 0 表示 SCX 实体进入 INIT/保持未启用，负 errno 表示 init_task 拒绝或失败；失败会
 * 恢复 NONE，但读锁仍由调用者通过 scx_cancel_fork() 释放。回调可能睡眠与否受 SCX 约束。
 */
int scx_fork(struct task_struct *p, struct kernel_clone_args *kargs);
/*
 * scx_post_fork() - 在 fork 成功后发布 child 到 SCX 全任务集合并释放门闩
 * @p: 输入输出 child；若其策略已是 SCHED_EXT，会在 task rq 锁下完成 enable。
 * 返回：无直接返回值。把 task 加入 scx_tasks/可选 tid 哈希，最后释放 fork 读锁；从此 SCX
 * 启停遍历可以观察它。无可报告失败，调用者仍持有 task 的正常生命周期 ownership。
 */
void scx_post_fork(struct task_struct *p);
/*
 * scx_cancel_fork() - 回滚失败 fork 已建立的 SCX 状态并释放门闩
 * @p: 输入输出、尚未发布的 child；可能已经完成 init_task，但不得到达 READY 之后。
 * 返回：无直接返回值。启用态下锁住 task rq、执行 disable/exit，再释放 scx_fork_rwsem；
 * 不释放 task 本身，后续通用 fork cleanup 继续回滚。
 */
void scx_cancel_fork(struct task_struct *p);
/*
 * scx_can_stop_tick() - 判断 NO_HZ_FULL 能否为当前 rq 停止周期 tick
 * @rq: 纯输入、不可为 NULL 的借用运行队列，调用者在调度器锁协议内稳定 curr/SCX 字段。
 * 返回 true 表示非 EXT、无 SCX runnable task 或 BPF 已声明可停 tick；bypass 中或仍依赖
 * slice/tick 时返回 false。只读状态、无副作用、不可睡眠。
 */
bool scx_can_stop_tick(struct rq *rq);
/*
 * scx_rq_activate() - 向 SCX 发布一个 rq/CPU 上线事件
 * @rq: 输入输出目标运行队列，CPU 热插拔核心持有必要锁并稳定 scx_root。
 * 返回：无直接返回值。更新热插拔序列并调用 cpu_online；缺少回调时可退出 scheduler。
 */
void scx_rq_activate(struct rq *rq);
/*
 * scx_rq_deactivate() - 向 SCX 发布一个 rq/CPU 下线事件
 * @rq: 输入输出目标运行队列，生命周期由 CPU 热插拔协议保证。
 * 返回：无直接返回值。调用 cpu_offline；scheduler 不支持该事件时触发退出，不转移 rq。
 */
void scx_rq_deactivate(struct rq *rq);
/*
 * scx_check_setscheduler() - 在策略提交前检查 task 是否允许进入 SCHED_EXT
 * @p: 输入 task，调用者必须持有其 rq 锁；只借用，不取得引用。
 * @policy: 目标 Linux 调度策略编号。
 * 返回 0 表示允许；SCX 启用、task 被 disallow 且确实要切入 SCHED_EXT 时返回 -EACCES。
 * 无状态修改、无睡眠，调用者据此继续或中止 sched_setscheduler 事务。
 */
int scx_check_setscheduler(struct task_struct *p, int policy);
/*
 * task_should_scx() - 为 fork/策略切换选择 ext_sched_class 是否接管 task
 * @policy: task 的 Linux 调度策略；DL/RT 已由上层先行处理。
 * 返回：SCX 关闭或拆除期通常为 false；全量接管窗口为 true；稳定期仅 SCHED_EXT 为 true。
 * READ_ONCE/启停状态检查避免把新 task 放入被 next_active_class() 跳过的类，无副作用。
 */
bool task_should_scx(int policy);
/*
 * scx_allow_ttwu_queue() - 判断唤醒能否使用远端 ttwu_queue 延迟入队
 * @p: 被唤醒 task 的只读借用指针，调用者负责在 wakeup 协议中保活。
 * 返回 true 表示 SCX 关闭、task 无 scheduler、scheduler 显式允许或 task 非 EXT；否则 false，
 * 迫使唤醒路径采用避免 BPF/远端排队语义冲突的路径。无副作用、不可睡眠。
 */
bool scx_allow_ttwu_queue(const struct task_struct *p);
/*
 * init_sched_ext_class() - 启动早期初始化所有 SCX 全局/per-CPU 基础设施
 * 入参：无。返回：无直接返回值；初始化 idle 掩码、每 CPU DSQ/链表/cpumask/irq_work、
 * watchdog 与 SysRq，并保留 BPF ABI 枚举。带 __init、仅启动期调用，关键分配失败会 BUG。
 */
void init_sched_ext_class(void);

/*
 * scx_cpuperf_target() - 读取 SCX 为指定 CPU 提出的归一化性能目标
 * @cpu: 纯输入逻辑 CPU 编号，调用者保证有效；本函数借用其永久 rq。
 * 返回 SCX 启用时 rq->scx.cpuperf_target 的 u32 快照，否则返回 0。该值由 schedutil
 * 合并进利用率/频率决策；无锁读取只提供瞬时提示，不构成跨字段一致快照，也无副作用。
 */
static inline u32 scx_cpuperf_target(s32 cpu)
{
	/* SCX 未启用时 rq 中的旧目标不可作为调频输入，使用 0 表示没有额外性能请求。 */
	if (scx_enabled())
		return cpu_rq(cpu)->scx.cpuperf_target;
	else
		return 0;
}

/*
 * task_on_scx() - 判断 task 此刻是否由已启用的 ext_sched_class 管理
 * @p: 只读、不可为 NULL 的借用 task，调用者负责用 task/rq/RCU 协议保活。
 * 返回 true 需同时满足全局 SCX 静态键开启且 sched_class 指向 ext；否则 false。
 * 不取得 rq 锁或引用、无副作用，结果可在并发策略切换后立即过期。
 */
static inline bool task_on_scx(const struct task_struct *p)
{
	/* 同时核对全局静态键与 task 的类，避免在启停窗口把残留类指针误判为有效 SCX。 */
	return scx_enabled() && p->sched_class == &ext_sched_class;
}

#ifdef CONFIG_SCHED_CORE
/*
 * scx_prio_less() - 为 core scheduling 比较两个 SCX task 的相对顺序
 * @a: 第一个只读借用 task；@b: 第二个只读借用 task，二者由 core-sched 锁协议保活。
 * @in_fi: 是否处于 forced-idle；当前实现把它传入契约但不直接消费。
 * 返回 true 表示 @a 的优先级低于 @b。相同 scheduler 可调用 BPF core_sched_before；否则
 * 按 core_sched_at 的全局 FIFO 时间排序。回调在 rq 锁/原子上下文执行，不得睡眠。
 */
bool scx_prio_less(const struct task_struct *a, const struct task_struct *b,
		   bool in_fi);
#endif

#else	/* CONFIG_SCHED_CLASS_EXT */
/*
 * 原配置标签表示以下定义服务于未构建 sched_ext 的内核。每个 stub 都消费同名接口形状，
 * 但不得改变通用调度状态；指针均不解引用，因而也不取得 ownership 或引入睡眠点。
 */

/* @rq 未消费；SCX 不存在时 tick 无额外工作，返回：无直接返回值。 */
static inline void scx_tick(struct rq *rq) {}
/* @p 未消费；无 SCX 启停写侧需要排斥，返回：无直接返回值且不取锁。 */
static inline void scx_pre_fork(struct task_struct *p) {}
/* @p/@kargs 均未消费；返回 0 让普通 fork 继续，不建立任何 SCX 状态。 */
static inline int scx_fork(struct task_struct *p, struct kernel_clone_args *kargs) { return 0; }
/* @p 未消费；fork 成功无需发布到 SCX，返回：无直接返回值。 */
static inline void scx_post_fork(struct task_struct *p) {}
/* @p 未消费；失败 fork 没有 SCX 资源可回滚，返回：无直接返回值。 */
static inline void scx_cancel_fork(struct task_struct *p) {}
/* @cpu 未消费；无 SCX 性能请求，恒返回 0，且不读取 rq。 */
static inline u32 scx_cpuperf_target(s32 cpu) { return 0; }
/* @rq 未消费；无 SCX task 依赖 tick，恒返回 true 允许 NO_HZ 停 tick。 */
static inline bool scx_can_stop_tick(struct rq *rq) { return true; }
/* @rq 未消费；CPU 上线无需通知 SCX，返回：无直接返回值。 */
static inline void scx_rq_activate(struct rq *rq) {}
/* @rq 未消费；CPU 下线无需通知 SCX，返回：无直接返回值。 */
static inline void scx_rq_deactivate(struct rq *rq) {}
/* @p/@policy 未消费；无 SCX disallow 约束，恒返回 0 允许上层按正常规则处理。 */
static inline int scx_check_setscheduler(struct task_struct *p, int policy) { return 0; }
/* @p 未消费；未构建 ext_sched_class，恒返回 false。 */
static inline bool task_on_scx(const struct task_struct *p) { return false; }
/* @p 未消费；无 BPF 排队语义，恒返回 true 允许通用 ttwu_queue。 */
static inline bool scx_allow_ttwu_queue(const struct task_struct *p) { return true; }
/* 入参：无；未构建 SCX 时没有基础设施要初始化，返回：无直接返回值。 */
static inline void init_sched_ext_class(void) {}

#endif	/* CONFIG_SCHED_CLASS_EXT */

#ifdef CONFIG_SCHED_CLASS_EXT
/*
 * __scx_update_idle() - 在 rq 锁下更新 SCX idle 掩码并可通知 BPF scheduler
 * @rq: 输入输出目标运行队列，调用者必须持有其 rq 锁。
 * @idle: true 表示 CPU 应进入 idle 集合，false 表示离开。
 * @do_notify: true 表示真实 idle/busy 转换并允许 ops.update_idle；false 只刷新内建掩码。
 * 返回：无直接返回值。先发布内建 idle 位，再回调 BPF，以保证 enqueue/update_idle 至少
 * 一方观察到对方状态；不分配、不转移 rq，回调不可睡眠。
 */
void __scx_update_idle(struct rq *rq, bool idle, bool do_notify);

/*
 * scx_update_idle() - 仅在 SCX 已启用时转发 idle 状态更新
 * @rq/@idle/@do_notify: 与 __scx_update_idle() 相同，均为借用输入/状态参数。
 * 返回：无直接返回值。静态键关闭时快速无操作；打开时继承 rq 锁与不可睡眠约束。
 */
static inline void scx_update_idle(struct rq *rq, bool idle, bool do_notify)
{
	/* 启停静态键是调用实现的发布门禁，避免关闭态触碰尚未就绪或已拆除的 SCX 状态。 */
	if (scx_enabled())
		__scx_update_idle(rq, idle, do_notify);
}
#else
/* @rq/@idle/@do_notify 均未消费；未构建 SCX 时返回无副作用。 */
static inline void scx_update_idle(struct rq *rq, bool idle, bool do_notify) {}
#endif

#ifdef CONFIG_CGROUP_SCHED
#ifdef CONFIG_EXT_GROUP_SCHED
/*
 * scx_tg_init() - 为新 task_group 填入 SCX 默认 weight、带宽和非 idle 状态
 * @tg: 输入输出、尚未 online 的借用组对象；调用者独占初始化阶段。
 * 返回：无直接返回值，无分配/失败；只建立随后 online/BPF cgroup_init 使用的初值。
 */
void scx_tg_init(struct task_group *tg);
/*
 * scx_tg_online() - 把 task_group 发布给 SCX/BPF cgroup 生命周期
 * @tg: 输入输出借用组，必须已 init 且尚未带 ONLINE/INITED 标志。
 * 返回 0 表示 ONLINE 已发布；BPF cgroup_init 失败返回清洗后的负 errno，且不置状态位。
 */
int scx_tg_online(struct task_group *tg);
/*
 * scx_tg_offline() - 撤销 task_group 的 SCX/BPF online 状态
 * @tg: 输入输出借用组，入口必须带 ONLINE；外层 cgroup 生命周期保证没有非法并发销毁。
 * 返回：无直接返回值。需要时调用 cgroup_exit，再清 ONLINE/INITED；不释放 tg 本身。
 */
void scx_tg_offline(struct task_group *tg);
/*
 * scx_cgroup_can_attach() - 为一批 task 的 cgroup 迁移执行 SCX prepare 阶段
 * @tset: 输入输出借用 taskset；为实际跨组 task 记录 cgrp_moving_from。
 * 返回 0 表示全批次可提交；任一 BPF prep_move 失败时反向通知已准备项、清临时状态并返回
 * 清洗后的负 errno。该函数是 move/cancel 的前半事务，不转移 task ownership。
 */
int scx_cgroup_can_attach(struct cgroup_taskset *tset);
/*
 * scx_cgroup_move_task() - 在 cgroup 事务提交后通知一个 task 已完成迁移
 * @p: 输入输出借用 task；仅 cgrp_moving_from 非 NULL 时向 BPF 报告成对 move。
 * 返回：无直接返回值。回调后清临时来源指针；identity migration 无操作。
 */
void scx_cgroup_move_task(struct task_struct *p);
/*
 * scx_cgroup_cancel_attach() - 取消整批已 prepare 的 cgroup 迁移
 * @tset: 输入输出借用 taskset；逐 task 调用可选 cancel_move 并清 cgrp_moving_from。
 * 返回：无直接返回值；使失败事务恢复到 prepare 前的 SCX 可观察状态。
 */
void scx_cgroup_cancel_attach(struct cgroup_taskset *tset);
/*
 * scx_group_set_weight() - 向 BPF 发布并缓存 task_group 新权重
 * @tg: 输入输出借用组；@cgrp_weight: cgroup 权重尺度的新值。
 * 返回：无直接返回值。以 scx_cgroup_ops_rwsem 读侧稳定 scheduler，值变化时先回调再缓存；
 * 即使 SCX 未启用也更新缓存，供以后 online 使用。读锁可能睡眠。
 */
void scx_group_set_weight(struct task_group *tg, unsigned long cgrp_weight);
/*
 * scx_group_set_idle() - 向 BPF 发布并缓存 task_group idle 属性
 * @tg: 输入输出借用组；@idle: 新 idle 标志。
 * 返回：无直接返回值。在 cgroup ops 读侧可选回调后始终更新缓存，无错误返回。
 */
void scx_group_set_idle(struct task_group *tg, bool idle);
/*
 * scx_group_set_bandwidth() - 向 BPF 发布并缓存 task_group 带宽三元组
 * @tg: 输入输出借用组；@period_us: 周期微秒；@quota_us: 配额微秒或 RUNTIME_INF；
 * @burst_us: 允许突发的微秒数。
 * 返回：无直接返回值。任一值变化才回调，随后在同一读侧临界区内依次缓存三项；cgroup ops 读侧
 * 防止 scheduler 与回调表被并发拆除，不取得 tg ownership。
 */
void scx_group_set_bandwidth(struct task_group *tg, u64 period_us, u64 quota_us, u64 burst_us);
#else	/* CONFIG_EXT_GROUP_SCHED */
/*
 * 原配置标签表示通用 cgroup 调度存在、但 SCX group scheduling 未构建。以下 stub 保留
 * CPU controller 调用面；除 init/属性缓存策略由其他类负责外，均不得阻止 cgroup 事务。
 */
/* @tg 未消费；无 SCX 字段生命周期要初始化，返回：无直接返回值。 */
static inline void scx_tg_init(struct task_group *tg) {}
/* @tg 未消费；恒返回 0，允许 cgroup online。 */
static inline int scx_tg_online(struct task_group *tg) { return 0; }
/* @tg 未消费；无 BPF cgroup_exit，返回：无直接返回值。 */
static inline void scx_tg_offline(struct task_group *tg) {}
/* @tset 未消费；恒返回 0，不能因缺少 SCX 阻止 attach。 */
static inline int scx_cgroup_can_attach(struct cgroup_taskset *tset) { return 0; }
/* @p 未消费；提交迁移时无 SCX 状态可更新。 */
static inline void scx_cgroup_move_task(struct task_struct *p) {}
/* @tset 未消费；取消事务时无 SCX prepare 状态可撤销。 */
static inline void scx_cgroup_cancel_attach(struct cgroup_taskset *tset) {}
/* @tg/@cgrp_weight 未消费；无 SCX 权重缓存或回调。 */
static inline void scx_group_set_weight(struct task_group *tg, unsigned long cgrp_weight) {}
/* @tg/@idle 未消费；无 SCX idle 属性缓存或回调。 */
static inline void scx_group_set_idle(struct task_group *tg, bool idle) {}
/* 三个 us 参数和 @tg 均未消费；无 SCX 带宽缓存或回调。 */
static inline void scx_group_set_bandwidth(struct task_group *tg, u64 period_us, u64 quota_us, u64 burst_us) {}
#endif	/* CONFIG_EXT_GROUP_SCHED */
#endif	/* CONFIG_CGROUP_SCHED */
