/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __MM_MEMCONTROL_V1_H
#define __MM_MEMCONTROL_V1_H

/* 头文件保护宏让公共声明与 CONFIG_MEMCG_V1 两套 inline 桩只展开一次。 */

#include <linux/cgroup-defs.h>

/* Cgroup v1 and v2 common declarations */
/* 下列遍历、统计和私有 ID 接口由 cgroup v1/v2 共同使用。 */

/*
 * Iteration constructs for visiting all cgroups (under a tree).  If
 * loops are exited prematurely (break), mem_cgroup_iter_break() must
 * be used for reference counting.
 */
/*
 * 这两个宏构造对全部 memcg（或 @root 子树）的引用安全遍历。每轮由
 * mem_cgroup_iter() 交还上一个节点并取得下一个节点；若用 break 提前退出，
 * 必须调用 mem_cgroup_iter_break() 释放游标持有的 css 引用，否则会阻止销毁。
 * @iter 是由宏反复写入的 owning 游标，@root 是可为 NULL 的借用根节点。
 */
#define for_each_mem_cgroup_tree(iter, root)		\
	for (iter = mem_cgroup_iter(root, NULL, NULL);	\
	     iter != NULL;				\
	     iter = mem_cgroup_iter(root, iter, NULL))

#define for_each_mem_cgroup(iter)			\
	for (iter = mem_cgroup_iter(NULL, NULL, NULL);	\
	     iter != NULL;				\
	     iter = mem_cgroup_iter(NULL, iter, NULL))

/*
 * drain_all_stock() - 把各 CPU 的 memcg 预充额度冲回全局计数器。
 * 业务背景：缩限、offline 或回收需要看见 per-CPU stock 中尚未消费的额度；调用链
 * 通过 CPU work 完成本地 drain。入参 @root_memcg 是非 NULL 的借用过滤根。
 * 出参/返回：无直接返回值；若已有 drain 在运行可提前返回，本 CPU 同步冲刷，
 * 远端 CPU 只排队 work，函数可能在它们真正完成前返回。
 * 注意事项：可睡眠，不能持 stock 本地锁；CPU hotplug 由各路径仅操作本地数据保证。
 */
void drain_all_stock(struct mem_cgroup *root_memcg);

/*
 * memcg_events() - 读取一个 memcg 子树的聚合 VM 事件快照。
 * 业务背景：memory.stat 等控制面复用 vmstats 聚合结果。入参 @memcg 是非 NULL
 * 借用对象，@event 是 vm_event_item 编号；不取得引用，也不主动 flush rstat。
 * 出参/返回：返回 unsigned long 次数；坏索引告警并返回 0，无 ownership 变化。
 * 注意事项：并发更新下只保证 READ_ONCE 快照，精确读取前由上层显式 flush。
 */
unsigned long memcg_events(struct mem_cgroup *memcg, int event);
/*
 * memory_stat_show() - 把目标 memcg 的 memory.stat 内容写入 seq_file。
 * 业务背景：cgroup 文件读回调经它分配临时 seq_buf 并调用格式化核心。入参 @m
 * 是含目标 memcg 的借用 seq_file，@v 未使用且可为 NULL，二者 ownership 不变。
 * 出参/返回：成功写出并返回 0；临时缓冲分配失败返回 -ENOMEM，不留下资源。
 * 注意事项：GFP_KERNEL 分配使其可睡眠；并发统计以 flush 后的近似快照输出。
 */
int memory_stat_show(struct seq_file *m, void *v);

/*
 * mem_cgroup_private_id_get_online() - 为持久记录取得可用 memcg 私有 ID 引用。
 * 业务背景：swap slot 等跨越 memcg offline 的记录不能保存易失裸指针；若 @memcg
 * 已离线便沿父链寻找在线祖先。@memcg 是 RCU 下借用对象，@n 是要增加的引用数。
 * 出参/返回：返回其 id.ref 成功增加 @n 的 memcg；调用者须按 @n 对称 put。
 * 注意事项：@n 必须非零且父链有效；根组引用按不变量永不降至零，本函数不睡眠。
 */
struct mem_cgroup *mem_cgroup_private_id_get_online(struct mem_cgroup *memcg,
						    unsigned int n);

/* Cgroup v1-specific declarations */
/* 以下接口只承载 legacy memory controller 的 soft-limit、memsw、事件与 OOM 语义。 */
#ifdef CONFIG_MEMCG_V1

/* Whether legacy memory+swap accounting is active */
/* legacy memory+swap 联合计费仅在 memory controller 挂载于 cgroup v1 层级时启用。 */
/*
 * do_memsw_account() - 判断当前 memory controller 是否使用 v1 memsw 计费。
 * 业务背景：swapout/charge 路径据此决定是否维护 memory+swap 联合 page_counter。
 * 入参：无。出参/返回：v1 legacy hierarchy 返回 true，统一 v2 层级返回 false。
 * 注意事项：纯策略查询，无锁、无引用和副作用；结果来自启动/挂载后的 cgroup 根。
 */
static inline bool do_memsw_account(void)
{
	return !cgroup_subsys_on_dfl(memory_cgrp_subsys);
}

/*
 * memcg_events_local() - 读取 v1 本组、不含后代的一项 VM 事件。
 * 业务背景：legacy memory.stat 同时展示 local 与 total 视图。@memcg 是借用对象，
 * @event 是 vm_event_item 编号。返回并发安全的 unsigned long 快照，坏索引为 0。
 * 注意事项：不 flush、不取引用且不睡眠；调用者需自行决定快照新鲜度。
 */
unsigned long memcg_events_local(struct mem_cgroup *memcg, int event);
/*
 * memcg_page_state_local() - 读取 v1 本组、不含后代的一项 page state。
 * 业务背景：为 legacy local 统计保留 non-hierarchical 视图。@memcg 为借用对象，
 * @idx 为 memcg state 项。返回原生计数单位快照；坏索引或暂时负值对外为 0。
 * 注意事项：不 flush、不睡眠；SMP 聚合传播期间结果允许短暂滞后。
 */
unsigned long memcg_page_state_local(struct mem_cgroup *memcg, int idx);
/*
 * memcg_page_state_local_output() - 把 v1 local state 换算为用户输出单位。
 * 业务背景：memory.stat 对字节项与计数项需要统一格式。@memcg 为借用对象，
 * @item 是 state 编号。返回 local 值乘该项倍率后的 unsigned long，无副作用。
 * 注意事项：继承 local 读取的近似快照与坏索引语义，不取得任何引用。
 */
unsigned long memcg_page_state_local_output(struct mem_cgroup *memcg, int item);
/*
 * memcg1_alloc_events() - 为新 memcg 分配 v1 per-CPU 事件节流状态。
 * 业务背景：charge/uncharge 热路径需本地累计后再触发 threshold/soft-limit 检查。
 * @memcg 为创建期独占输入输出对象。成功返回 true 并让其持有 percpu allocation；
 * 失败返回 false 且字段为 NULL。GFP_KERNEL_ACCOUNT 可睡眠，须由 free 配对。
 */
bool memcg1_alloc_events(struct mem_cgroup *memcg);
/*
 * memcg1_free_events() - 释放 memcg 持有的 v1 per-CPU 事件状态。
 * 业务背景：css 销毁路径与 alloc_events() 配对。@memcg 是生命周期稳定的独占对象。
 * 出参/返回：无直接返回值；memcg 放弃 events_percpu backing，memcg 自身仍归销毁路径。
 * 注意事项：调用前必须阻止热路径继续访问且只释放一次；函数本身不报告失败。
 */
void memcg1_free_events(struct mem_cgroup *memcg);

/*
 * memcg1_memcg_init() - 初始化新 memcg 的 v1 事件与阈值同步实体。
 * 业务背景：通用 memcg 分配完成后补齐 legacy OOM notify、threshold 和 event 链表。
 * @memcg 为创建期独占输入输出对象。无直接返回；初始化链表、mutex 与 spinlock。
 * 注意事项：发布 css 前调用、无需锁且不睡眠；不得对已在使用的对象重复初始化。
 */
void memcg1_memcg_init(struct mem_cgroup *memcg);
/*
 * memcg1_remove_from_trees() - 从所有 NUMA soft-limit 红黑树摘除 memcg。
 * 业务背景：offline/销毁前阻止 legacy 回收器再选中该组。@memcg 为稳定借用对象。
 * 出参/返回：无直接返回；逐节点删除其 mem_cgroup_per_node 索引，不释放 memcg。
 * 注意事项：调用链负责生命周期与并发序列；空的 per-node tree 被安全跳过。
 */
void memcg1_remove_from_trees(struct mem_cgroup *memcg);

/*
 * memcg1_soft_limit_reset() - 把 v1 soft limit 恢复为“不限”。
 * 业务背景：初始化/重置 legacy soft-limit 控制值。@memcg 是稳定输入输出对象。
 * 出参/返回：无直接返回；WRITE_ONCE 将 soft_limit 发布为 PAGE_COUNTER_MAX。
 * 注意事项：无锁且不睡眠；WRITE_ONCE 只防编译器撕裂，不提供复合状态事务。
 */
static inline void memcg1_soft_limit_reset(struct mem_cgroup *memcg)
{
	WRITE_ONCE(memcg->soft_limit, PAGE_COUNTER_MAX);
}

/* 仅声明 cgroup attach 使用的不透明 taskset 类型，本头文件不拥有其实例。 */
struct cgroup_taskset;
/*
 * memcg1_css_offline() - 在 v1 css 下线时注销事件并通知用户态。
 * 业务背景：cgroup 目录 rmdir 后把 event_list 项摘除并排队异步 remove work。
 * @memcg 为 offline 流程固定的输入输出对象。无直接返回；事件对象转交 workqueue。
 * 注意事项：内部以 event_list_lock_irq 与注册路径竞争；排队后回调可睡眠清理。
 */
void memcg1_css_offline(struct mem_cgroup *memcg);

/* for encoding cft->private value on file */
/* 下列枚举编码进 cftype.private，让共享读写回调识别具体 legacy 资源计数器。 */
enum res_type {
	/* _MEM：仅物理内存 page_counter，典型文件为 memory.limit_in_bytes。 */
	_MEM,
	/* _MEMSWAP：物理内存加 swap 的联合计数器，只有 v1 memsw 文件使用。 */
	_MEMSWAP,
	/* _KMEM：内核内存 legacy 计数器，用于已弃用的 kmem.* 控制文件。 */
	_KMEM,
	/* _TCP：socket/TCP 内存 page_counter，对应 kmem.tcp.* 文件。 */
	_TCP,
};

/*
 * memcg1_oom_prepare() - 在通用 memcg OOM killer 前处理 v1 特有策略。
 * 业务背景：charge 失败时区分禁用内核 OOM 的用户态处理与可立即杀进程路径。
 * @memcg 为借用 OOM 域；@locked 为非 NULL 输出，写入是否取得层级 OOM 锁。
 * 返回 false 表示延后/放弃内核 kill，true 表示可继续；可能取得 css 引用或发通知。
 * 注意事项：charge 上下文不能随意睡眠，返回 true 后必须调用 oom_finish() 解锁。
 */
bool memcg1_oom_prepare(struct mem_cgroup *memcg, bool *locked);
/*
 * memcg1_oom_finish() - 结束 prepare 后的 v1 层级 OOM 临界区。
 * 业务背景：通用 killer 完成后按 prepare 输出对称解锁。@memcg 是同一借用 OOM 域，
 * @locked 是原样传回的 ownership 标志。无直接返回；true 时释放层级 OOM 锁。
 * 注意事项：不得伪造或重复消费 @locked；false 路径为空操作且不睡眠。
 */
void memcg1_oom_finish(struct mem_cgroup *memcg, bool locked);
/*
 * memcg1_oom_recover() - 用户态放宽限制后唤醒等待该 v1 OOM 域的任务。
 * 业务背景：memory.oom_control/eventfd 控制路径需要让延后的 page-fault charge 重试。
 * @memcg 是可为 NULL 的借用对象。无直接返回；under_oom 时广播 memcg_oom_waitq。
 * 注意事项：lockless 状态检查依赖 mark-under-oom 先于通知；唤醒不保证重试成功。
 */
void memcg1_oom_recover(struct mem_cgroup *memcg);

/*
 * memcg1_commit_charge() - 在 folio 计费提交后更新 v1 统计并检查事件阈值。
 * 业务背景：通用 charge 建立归属后，legacy 控制面还需 page-event 与 soft-limit 反馈。
 * @folio/@memcg 均为调用期稳定的借用对象，页数取 folio_nr_pages()、节点取 folio_nid()。
 * 无直接返回；更新 per-CPU 统计并可能更新 soft-limit tree/触发阈值，无引用转移。
 * 注意事项：内部关本地 IRQ 保护 per-CPU 状态，不睡眠；必须在归属提交后调用一次。
 */
void memcg1_commit_charge(struct folio *folio, struct mem_cgroup *memcg);
/*
 * memcg1_uncharge_batch() - 汇总一批解除计费产生的 v1 事件。
 * 业务背景：批量 uncharge 后一次更新 PGPGOUT、页事件频率并检查 soft limit。
 * @memcg 是借用账主；@pgpgout/@nr_memory 单位均为页；@nid 是原 folio NUMA 节点。
 * 无直接返回；仅更新统计和事件树，不释放 memcg/folio ownership。
 * 注意事项：内部关本地 IRQ，不睡眠；调用者须保证批量数值与实际 uncharge 一致。
 */
void memcg1_uncharge_batch(struct mem_cgroup *memcg, unsigned long pgpgout,
			   unsigned long nr_memory, int nid);

/*
 * memcg1_stat_format() - 追加 legacy memory.stat 的 local、total 与层级限制字段。
 * 业务背景：通用 stat 输出调用它补齐 v1 ABI。@memcg 是借用目标，@s 是调用者
 * owning 的可写 seq_buf；函数 flush stats 后向其追加文本，不接管 backing buffer。
 * 出参/返回：无直接返回；缓冲不足由 seq_buf 状态记录，字段值为读取时快照。
 * 注意事项：flush/祖先遍历可有并发变化；调用环境可睡眠且须稳定 memcg 生命周期。
 */
void memcg1_stat_format(struct mem_cgroup *memcg, struct seq_buf *s);
/*
 * reparent_memcg1_state_local() - 把全部 v1 local VM state 守恒搬到父组。
 * 业务背景：子 css offline 后，legacy non-hierarchical 统计不能凭空消失。
 * @memcg/@parent 是稳定借用对象。无直接返回；对子扣除、父组增加同量统计。
 * 注意事项：调用者已阻止新归属更新；本函数不转移 css ownership，也不睡眠。
 */
void reparent_memcg1_state_local(struct mem_cgroup *memcg, struct mem_cgroup *parent);
/*
 * reparent_memcg1_lruvec_state_local() - 搬移各 LRU list 的 v1 node-local 统计。
 * 业务背景：css offline 时保持每个 NUMA lruvec 视图的系统总量守恒。
 * @memcg/@parent 均为稳定借用对象。无直接返回；逐 LRU/节点反向更新子父计数。
 * 注意事项：依赖 offline 序列排除新更新；不搬移 folio，只搬统计且不睡眠。
 */
void reparent_memcg1_lruvec_state_local(struct mem_cgroup *memcg, struct mem_cgroup *parent);

/*
 * reparent_memcg_state_local() - 守恒搬移指定的一项 memcg local state。
 * 业务背景：供上面的 v1 批量包装复用通用 vmstats 更新。@memcg/@parent 为借用对象，
 * @idx 为 state 编号。无直接返回；把当前 local 值从子组扣除并加到父组。
 * 注意事项：调用者负责 offline 并发隔离；不取得引用且不改变对象归属。
 */
void reparent_memcg_state_local(struct mem_cgroup *memcg,
				struct mem_cgroup *parent, int idx);
/*
 * reparent_memcg_lruvec_state_local() - 搬移一项跨所有节点的 local lruvec state。
 * 业务背景：为 v1 LRU 批量包装提供 node-aware 通用原语。@memcg/@parent 为借用对象，
 * @idx 为 node_stat_item。无直接返回；每节点对子扣除、对父增加相同原生单位值。
 * 注意事项：只搬统计不搬 folio；调用者稳定生命周期并排除新的子组更新。
 */
void reparent_memcg_lruvec_state_local(struct mem_cgroup *memcg,
				       struct mem_cgroup *parent, int idx);

/*
 * memcg1_account_kmem() - 调整 v1 legacy 内核内存 page_counter。
 * 业务背景：kmem charge 已由统一 memcg 路径完成后，v1 还维护已弃用的 kmem.* ABI。
 * @memcg 是借用账主；@nr_pages 单位为页，正值计入、非正值按绝对值解除。
 * 无直接返回；只在非 default hierarchy 更新 kmem counter，无 ownership 变化。
 * 注意事项：调用者保证不会发生有符号最小值溢出，且增减与真实 charge 配对。
 */
void memcg1_account_kmem(struct mem_cgroup *memcg, int nr_pages);
/*
 * memcg1_tcpmem_active() - 查询 v1 socket memory 控制是否已激活。
 * 业务背景：网络栈据此决定是否走 tcpmem page_counter。@memcg 为非 NULL 借用对象。
 * 出参/返回：返回 tcpmem_active 布尔快照，无输出参数、引用或其他副作用。
 * 注意事项：纯字段读取，不提供跨后续操作的稳定性；调用者已稳定 memcg 生命周期。
 */
static inline bool memcg1_tcpmem_active(struct mem_cgroup *memcg)
{
	return memcg->tcpmem_active;
}

/*
 * memcg1_charge_skmem() - 尝试向 v1 TCP memory counter 计入 socket 页。
 * 业务背景：网络分配在执行真实内存分配前检查 cgroup TCP 限额。@memcg 为借用账主，
 * @nr_pages 单位为页且应非零，@gfp_mask 只用 __GFP_NOFAIL 决定是否强制超限计费。
 * 成功返回 true 并增加 tcpmem；普通限额失败返回 false 并置 pressure，ownership 不变。
 * 注意事项：成功必须由 uncharge_skmem() 配对；NOFAIL 会绕过限额但仍记账。
 */
bool memcg1_charge_skmem(struct mem_cgroup *memcg, unsigned int nr_pages,
			 gfp_t gfp_mask);
/*
 * memcg1_uncharge_skmem() - 归还先前成功计入的 v1 TCP memory 页数。
 * 业务背景：socket backing 释放时与 charge_skmem() 配对。@memcg 是同一借用账主，
 * @nr_pages 为要归还的页数。无直接返回；递减 tcpmem page_counter。
 * 注意事项：不得重复或超量 uncharge；本函数不清 pressure，也不释放 memcg 引用。
 */
static inline void memcg1_uncharge_skmem(struct mem_cgroup *memcg, unsigned int nr_pages)
{
	page_counter_uncharge(&memcg->tcpmem, nr_pages);
}

/* memsw_files 由 memcontrol-v1.c 定义，静态存活并描述 memory.memsw.* 控制文件。 */
extern struct cftype memsw_files[];
/* legacy_files 同样由实现文件持有，供 cgroup v1 注册 memory.* 与 kmem.* ABI。 */
extern struct cftype mem_cgroup_legacy_files[];

#else	/* CONFIG_MEMCG_V1 */
/* 未编译 v1 时保留签名兼容桩，使通用调用点无需散布条件编译且不产生 v1 副作用。 */

/*
 * do_memsw_account() - disabled-v1 策略桩。业务背景：通用 swap 路径仍可无条件调用。
 * 入参：无。出参/返回：恒为 false，无输出、引用或计数副作用。
 * 注意事项：编译期可折叠，明确阻止 v2 路径维护 legacy memory+swap counter。
 */
static inline bool do_memsw_account(void) { return false; }
/*
 * memcg1_alloc_events() - disabled-v1 创建桩。@memcg 为未使用的借用对象。
 * 业务背景：让通用 memcg 分配流程视为 v1 资源准备成功。返回恒 true，字段不变。
 * 注意事项：不分配、不睡眠；对应 free_events() 也是空操作。
 */
static inline bool memcg1_alloc_events(struct mem_cgroup *memcg) { return true; }
/*
 * memcg1_free_events() - disabled-v1 销毁桩。@memcg 为未使用的借用对象。
 * 业务背景：与成功但未分配的 alloc_events() 保持调用对称。无返回且无副作用。
 * 注意事项：不释放通用 memcg 资源，ownership 完全由调用者保持。
 */
static inline void memcg1_free_events(struct mem_cgroup *memcg) {}

/*
 * memcg1_memcg_init() - disabled-v1 初始化桩。@memcg 为未使用的创建期借用对象。
 * 业务背景：通用构造路径无需判断配置。无返回，不初始化任何 legacy 字段。
 * 注意事项：v2 所需状态由通用初始化承担，本桩不睡眠。
 */
static inline void memcg1_memcg_init(struct mem_cgroup *memcg) {}
/*
 * memcg1_remove_from_trees() - disabled-v1 soft-limit 摘除桩。@memcg 未使用。
 * 业务背景：无 legacy soft-limit tree 时 offline 路径仍保持统一。无返回、无副作用。
 * 注意事项：不释放 memcg，也不接触通用回收结构。
 */
static inline void memcg1_remove_from_trees(struct mem_cgroup *memcg) {}
/*
 * memcg1_soft_limit_reset() - disabled-v1 soft-limit 重置桩。@memcg 未使用。
 * 业务背景：v2 不提供 legacy soft limit。无返回且不写字段。
 * 注意事项：调用点不能据此假设 soft_limit 已初始化。
 */
static inline void memcg1_soft_limit_reset(struct mem_cgroup *memcg) {}
/*
 * memcg1_css_offline() - disabled-v1 事件注销桩。@memcg 未使用。
 * 业务背景：没有 legacy eventfd/threshold 对象需要排队清理。无返回、无副作用。
 * 注意事项：通用 css offline 资源仍由其他路径负责。
 */
static inline void memcg1_css_offline(struct mem_cgroup *memcg) {}

/*
 * memcg1_oom_prepare() - disabled-v1 OOM 前置桩。@memcg/@locked 均未读写、ownership 不变。
 * 业务背景：无 v1 禁杀与层级 OOM 锁时直接允许通用 killer。返回恒 true。
 * 注意事项：刻意不初始化 @locked；配套 finish() 也忽略它，二者必须成套使用。
 */
static inline bool memcg1_oom_prepare(struct mem_cgroup *memcg, bool *locked) { return true; }
/*
 * memcg1_oom_finish() - disabled-v1 OOM 收尾桩。@memcg/@locked 均未使用。
 * 业务背景：prepare 未取得 legacy 层级锁，因此无需解锁。无返回、无副作用。
 * 注意事项：即使 @locked 未初始化也安全，因为函数体不会读取它。
 */
static inline void memcg1_oom_finish(struct mem_cgroup *memcg, bool locked) {}
/*
 * memcg1_oom_recover() - disabled-v1 用户态 OOM 恢复桩。@memcg 未使用。
 * 业务背景：v2 不存在 legacy memcg_oom_waitq 协议。无返回、无唤醒副作用。
 * 注意事项：不替代通用 OOM recovery。
 */
static inline void memcg1_oom_recover(struct mem_cgroup *memcg) {}

/*
 * memcg1_commit_charge() - disabled-v1 charge 提交桩。@folio/@memcg 均为未使用借用。
 * 业务背景：v2 charge 后无需维护 legacy page-event/soft-limit 状态。无返回和副作用。
 * 注意事项：不改变 folio 归属或引用，通用统计仍由调用者路径完成。
 */
static inline void memcg1_commit_charge(struct folio *folio,
					struct mem_cgroup *memcg) {}

/*
 * memcg1_uncharge_batch() - disabled-v1 批量解除计费桩。
 * 业务背景：无 legacy PGPGOUT/soft-limit 检查。@memcg 借用，@pgpgout/@nr_memory
 * 单位为页，@nid 为节点号，均未读取。无返回、无统计或 ownership 副作用。
 * 注意事项：真实 page_counter uncharge 已由通用调用者完成。
 */
static inline void memcg1_uncharge_batch(struct mem_cgroup *memcg,
					 unsigned long pgpgout,
					 unsigned long nr_memory, int nid) {}

/*
 * memcg1_stat_format() - disabled-v1 stat 追加桩。@memcg/@s 均为未使用借用对象。
 * 业务背景：v2 memory.stat 不输出 legacy local/total 与 memsw limit 字段。
 * 出参/返回：无直接返回，seq_buf 内容保持不变；不 flush、不分配且不睡眠。
 */
static inline void memcg1_stat_format(struct mem_cgroup *memcg, struct seq_buf *s) {}

/*
 * memcg1_account_kmem() - disabled-v1 kmem 计数桩。@memcg 借用，@nr_pages 为未使用页数。
 * 业务背景：v2 不维护已弃用的 legacy kmem page_counter。无返回和计数副作用。
 * 注意事项：通用 objcg/kmem 计费不由此桩取消。
 */
static inline void memcg1_account_kmem(struct mem_cgroup *memcg, int nr_pages) {}
/*
 * memcg1_tcpmem_active() - disabled-v1 TCP memory 策略桩。@memcg 未使用。
 * 业务背景：网络栈据 false 跳过 legacy tcpmem counter。返回恒 false，无副作用。
 * 注意事项：不表示系统完全没有 socket memory accounting，只关闭 v1 接口。
 */
static inline bool memcg1_tcpmem_active(struct mem_cgroup *memcg) { return false; }
/*
 * memcg1_charge_skmem() - disabled-v1 socket charge 桩。
 * 业务背景：无 legacy TCP 限额时让分配继续。@memcg 借用，@nr_pages/@gfp_mask 未使用。
 * 出参/返回：恒 true 但不增加任何 counter；因未取得额度，不需要真实回滚。
 * 注意事项：调用者仍负责底层内存分配失败与通用 accounting。
 */
static inline bool memcg1_charge_skmem(struct mem_cgroup *memcg, unsigned int nr_pages,
				       gfp_t gfp_mask) { return true; }
/*
 * memcg1_uncharge_skmem() - disabled-v1 socket uncharge 桩。@memcg 借用、@nr_pages 未用。
 * 业务背景：与不计数但成功的 charge 桩保持控制流对称。无返回和副作用。
 * 注意事项：不归还通用 socket 内存资源，那些资源由网络栈自己的释放路径处理。
 */
static inline void memcg1_uncharge_skmem(struct mem_cgroup *memcg, unsigned int nr_pages) {}

#endif	/* CONFIG_MEMCG_V1 */
/* 以上条件结束：同一调用点在启用 v1 时落到实现，否则落到中性 inline 桩。 */

#endif	/* __MM_MEMCONTROL_V1_H */
/* 结束 __MM_MEMCONTROL_V1_H 保护范围。 */
