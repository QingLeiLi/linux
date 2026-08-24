/* SPDX-License-Identifier: GPL-2.0 */

/*
 * CPU 实时优先级索引接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本头文件定义每个 root_domain 的 RT CPU 候选索引；具体的无锁查询、分桶更新和
 * 内存序协议位于 cpupri.c。索引把 CPU 按其顶层 RT 运行队列当前最高优先级分桶，
 * 使 RT 唤醒、push/pull 和同优先级抢占路径能够先找到“正在运行更低优先级任务”的
 * CPU，再由调度域拓扑和目标 rq 加锁复核选择；它不负责迁移 task，也不承诺查询结果
 * 在返回后仍保持不变。
 *
 * cpupri 嵌入 struct root_domain：init_rootdomain() 以可睡眠分配完成初始化，在线 rq
 * 的最高 RT 优先级变化以及上下线路径在持有该 rq 锁时调用 cpupri_set()；最后一个
 * root-domain 引用消失并经过 RCU 宽限期后，free_rootdomain() 调用 cleanup()。查询
 * 不取得共享锁，更新方用“先加入新桶、再移出旧桶”和成对内存屏障避免优先级提高时
 * CPU 短暂从所有可搜索桶消失；余下竞态由后续 push/pull 与目标 rq 复核收敛。
 *
 * 无亲和性限制时，计数器加位图通常可用常数次位搜索找到候选；受限 task 最坏需要
 * 扫描优先级桶。代价是每个优先级一份 cpumask、一个原子计数以及 CPU 到桶的反向表，
 * 并且结果只能作为可能过期的候选提示，不能代替运行队列锁提供的一致状态。
 */
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/sched/rt.h>

/* NORMAL、RT1..RT99 和 HIGHER 共占 MAX_RT_PRIO + 1 个可搜索桶。 */
#define CPUPRI_NR_PRIORITIES	(MAX_RT_PRIO+1)

/* CPU 不参与路由时使用 -1；其余值按“CPU 当前负载优先级”从低到高排列。 */
#define CPUPRI_INVALID		-1
#define CPUPRI_NORMAL		 0
/* values 1-99 are for RT1-RT99 priorities */
/* 1～99 分别表示 CPU 当前最高可运行实体为 RT1～RT99，数值越大优先级越高。 */
#define CPUPRI_HIGHER		100

/*
 * 一个 CPU 优先级桶的无锁读取视图。
 *
 * mask 标出当前归入该桶的 CPU，动态位图由 cpupri_init() 分配并由 cleanup() 释放；
 * count 是该桶成员数的快速提示。更新方按规定次序修改二者，查询方先读 count、执行
 * 读屏障再读 mask。二者仍可能因并发更新而不构成同一时刻的快照：误多做一次位图
 * 求交可以接受，漏掉瞬时候选则由 RT pull/rebalance 路径纠正。
 */
struct cpupri_vec {
	atomic_t		count;
	cpumask_var_t		mask;
};

/*
 * 一个 root_domain 内全部 CPU 的 RT 优先级索引。
 *
 * pri_to_cpu 按 cpupri 编码保存 CPUPRI_NR_PRIORITIES 个桶；cpu_to_pri 则按 CPU 编号
 * 反查其当前桶，长度为 nr_cpu_ids。成功初始化后每个可能 CPU 的反向值均为 INVALID，
 * 在线路径再发布实际优先级；每次 set 必须保持“一 CPU 至多属于一个桶”以及反向值
 * 与位图最终一致。对象本身随 root_domain 存活，两个动态存储层由 init/cleanup 配对。
 */
struct cpupri {
	struct cpupri_vec	pri_to_cpu[CPUPRI_NR_PRIORITIES];
	int			*cpu_to_pri;
};

/*
 * cpupri_find() - 按优先级和亲和性为 RT task 查询可抢占的 CPU 集合
 *
 * 调用位置：RT 唤醒选核、push/pull 以及同优先级抢占判断；它是 fitness 回调为空的
 * 薄包装，只读索引，不改变 task、rq 或任何 ownership，也不能睡眠。
 * @cp: 纯输入、借用的 root_domain 索引；调用者须通过 rq/RCU 协议保证其生命周期。
 * @p: 纯输入、不可为 NULL 的 RT task；读取动态优先级和允许 CPU 掩码，不取得引用。
 * @lowest_mask: 可为 NULL 的输出掩码；非 NULL 时写入最低优先级且 p 允许、并仍为
 *               active 的候选 CPU。返回 1 表示当次扫描找到候选，0 表示未找到；
 *               返回值不是 CPU 编号。结果可立即过期，迁移前必须锁住目标 rq 复核。
 */
int  cpupri_find(struct cpupri *cp, struct task_struct *p,
		 struct cpumask *lowest_mask);

/*
 * cpupri_find_fitness() - 查询最低优先级且满足额外容量条件的候选 CPU
 *
 * @cp、@p、@lowest_mask: 语义与 cpupri_find() 相同，均为借用输入；lowest_mask 可空
 *                        且仅在非 NULL 时承接候选集合。
 * @fitness_fn: 可为 NULL 的只读筛选回调；仅当 lowest_mask 也非 NULL 时才对候选 CPU
 *              逐一检查 p 是否适配，回调不得接管 task。若所有候选均不适配，函数会放弃容量
 *              条件重新调用 cpupri_find()，优先保证更高优先级 RT task 能运行。
 * 返回 1 表示找到候选，0 表示没有。扫描和回调不持 cpupri 锁，结果只是瞬时提示；
 * 函数不分配内存、不睡眠，也不会报告并发更新为错误。
 */
int  cpupri_find_fitness(struct cpupri *cp, struct task_struct *p,
			 struct cpumask *lowest_mask,
			 bool (*fitness_fn)(struct task_struct *p, int cpu));

/*
 * cpupri_set() - 发布一个 CPU 当前顶层 RT 运行队列的最高优先级
 *
 * @cp: 输入输出的 root_domain 索引，借用且不转移所有权。
 * @cpu: 要更新的 possible CPU 编号；调用者必须持有 cpu_rq(cpu)->lock。
 * @pri: 调度器内部优先级 0..MAX_RT_PRIO，或 CPUPRI_INVALID；函数会转换为桶编号。
 * 返回：无直接返回值，也没有可恢复失败。新旧桶相同则直接返回；否则先将 CPU 发布到
 * 新桶，再从旧桶摘除并更新反向表。原子计数与屏障让无锁 reader 至少看见新旧桶之一，
 * 但不提供完整快照。函数不分配内存、不睡眠；非法转换结果触发 BUG_ON。
 */
void cpupri_set(struct cpupri *cp, int cpu, int pri);

/*
 * cpupri_init() - 初始化新 root_domain 的 RT CPU 优先级索引
 *
 * @cp: 纯输出对象，由调用者提供稳定存储；成功后拥有每个桶的动态 cpumask 和长度为
 *      nr_cpu_ids 的反向表，后续必须由 cpupri_cleanup() 成对释放。
 * 返回 0 表示所有桶计数/位图及 INVALID 反向值均建立；返回 -ENOMEM 表示任一分配失败。
 * GFP_KERNEL 分配允许睡眠。失败路径释放已经分配的桶位图；反向表仅在全部位图成功后
 * 分配，因此失败对象不会发布，init_rootdomain() 负责继续回滚更早取得的资源。
 */
int  cpupri_init(struct cpupri *cp);

/*
 * cpupri_cleanup() - 释放成功初始化的 RT CPU 优先级索引
 *
 * @cp: 输入输出对象，必须来自成功的 cpupri_init()，且所有查询和更新已经停止；
 *      free_rootdomain() 在最后一个引用消失并经过 RCU 宽限期后满足此前置条件。
 * 返回：无直接返回值。函数释放反向表和全部桶位图，不加锁、不等待读者，也不清空
 * 字段；调用后不得继续访问或重复清理，除非重新成功初始化。
 */
void cpupri_cleanup(struct cpupri *cp);
