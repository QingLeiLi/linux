/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 * Copyright (c) 2024 Andrea Righi <arighi@nvidia.com>
 */
/*
 * BPF 可扩展调度类接口见 Documentation/scheduler/sched-ext.rst；许可证和版权名单
 * 原样保留。本头文件连接 sched_ext 核心与 idle.c：后者维护全局/每 NUMA 节点的
 * idle CPU/完整空闲 SMT core 掩码，提供默认选核策略，并注册允许 BPF 调度器调用的
 * idle 查询及 select_cpu kfunc 集合。
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 内建追踪开启时，rq 的 busy/idle 转换先更新掩码，再通知 ops.update_idle()，从而与
 * enqueue 建立“要么入队方看到 idle 位、要么 update_idle 回调看到已入队 task”的交锁。
 * 默认选核只原子领取候选 idle 位并返回可能过期的 CPU，调用链仍须检查 task 亲和性、
 * CPU online/active 状态和 rq；拓扑静态键只决定是否增加 LLC/NUMA 局部搜索层次。
 *
 * 内建位图使常见唤醒无需遍历所有 rq，并能优先完整空闲 core、缓存域和 NUMA 节点；
 * 代价是并发视图允许短暂陈旧，且实现 ops.update_idle() 的 BPF 调度器若不请求 KEEP
 * 标志就接管状态跟踪，不能再假设内建 idle kfunc 可用。
 */
#ifndef _KERNEL_SCHED_EXT_IDLE_H
#define _KERNEL_SCHED_EXT_IDLE_H

#include <linux/btf_ids.h>

/* 以下对象均由调用者持有；前置声明避免本内部接口为类型布局引入额外依赖。 */
struct cpumask;
struct sched_ext_ops;
struct task_struct;

/*
 * 两个 BTF ID 集合分别描述普通 idle helper 与可能内部取得 task rq 锁的选核 helper。
 * idle.c 定义并注册它们，ext.c 的上下文过滤器按当前 BPF 回调类型决定 kfunc 是否可调用；
 * 集合是静态元数据，不拥有 task/rq，也不能替代运行时的 RCU 和锁前置条件。
 */
extern struct btf_id_set8 scx_kfunc_ids_idle;
extern struct btf_id_set8 scx_kfunc_ids_select_cpu;

/*
 * scx_idle_update_selcpu_topology() - 根据在线 CPU 拓扑更新默认选核优化静态键
 * @ops: 输入、不可为 NULL 的待启用调度器操作表，借用且不转移所有权；其 per-node
 *       标志会决定是否省略重复的 NUMA 优化。
 * 返回：无直接返回值。调用者须在可睡眠的控制路径持有 CPU 热插拔锁；函数仅在拓扑
 * 读取小段进入 RCU，退出后才启停 LLC/NUMA static branch。它不选择 CPU、不修改 task。
 */
void scx_idle_update_selcpu_topology(struct sched_ext_ops *ops);

/*
 * scx_idle_init_masks() - 启动期分配全局及逐 NUMA 节点的 idle/SMT 掩码
 * 入参：无。返回：无直接返回值；使用 GFP_KERNEL 分配，允许睡眠，任何分配失败均以
 * BUG_ON 终止启动而非返回 errno。成功后全局静态对象拥有所有动态掩码，供后续更新和
 * 查询长期使用；本接口没有对应运行期释放函数。
 */
void scx_idle_init_masks(void);

/*
 * scx_select_cpu_dfl() - 按亲和性、idle 状态和拓扑层次执行 sched_ext 默认唤醒选核
 * @p: 纯输入、不可为 NULL 的被唤醒 task，借用且不取得引用。
 * @prev_cpu: p 上次运行的 CPU；用于缓存/NUMA 局部性和同步唤醒决策。
 * @wake_flags: 唤醒属性位图，SCX_WAKE_SYNC 允许优先考虑 waker CPU。
 * @cpus_allowed: 可为 NULL 的额外候选约束；非 NULL 时还会与 p->cpus_ptr 求交。
 * @flags: idle 选择策略位，如仅选完整空闲 core 或限定节点搜索。
 * 返回非负 CPU 编号表示原子领取到 idle 候选，负 errno 表示约束无交集或没有候选。
 * 函数禁抢占并在读取 LLC 拓扑时进入 RCU，不能睡眠；结果仍需上层选核协议复核，且
 * 领取 idle 位是防止并发唤醒重复选择同一 CPU 的提示，不转移 rq 或 task ownership。
 */
s32 scx_select_cpu_dfl(struct task_struct *p, s32 prev_cpu, u64 wake_flags,
		       const struct cpumask *cpus_allowed, u64 flags);

/*
 * scx_idle_enable() - 为即将发布的 ops 配置内建 idle 追踪模式并重置掩码
 * @ops: 输入、不可为 NULL 的稳定操作表；函数借用它读取 update_idle 与策略标志。
 * 返回：无直接返回值。调用者须持 CPU 热插拔写侧约束；函数启停 static branch，并将
 * 在线 CPU 暂视为 idle，之后由真实 rq 转换快速收敛。它不调用 BPF 回调且不能并发卸载 ops。
 */
void scx_idle_enable(struct sched_ext_ops *ops);

/*
 * scx_idle_disable() - 在 sched_ext 调度器退出时关闭内建 idle 与逐节点静态键
 * 入参：无。返回：无直接返回值。调用链须先阻止新的相关快速路径；函数不清空或释放
 * 掩码，也不等待 BPF/rq reader，后续重新 enable 会重建位图内容。
 */
void scx_idle_disable(void);

/*
 * scx_idle_init() - 注册 idle 与 select_cpu 两组 BPF kfunc 的程序类型可见性
 * 入参：无。返回 0 表示全部集合注册成功，否则返回第一个注册失败的负 errno；`?:`
 * 链在任一步失败时停止，已成功注册的前序集合不在本函数回滚。该启动期过程可能睡眠，
 * 不操作 idle 掩码；select_cpu 集合刻意不暴露给任意 tracing 上下文，以免未知 pi_lock
 * 状态下再次取得 task rq 锁。
 */
int scx_idle_init(void);

/* 结束本头文件的重复包含保护；原有条件编译结构保持不变。 */
#endif /* _KERNEL_SCHED_EXT_IDLE_H */
