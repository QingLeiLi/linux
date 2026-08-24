/* SPDX-License-Identifier: GPL-2.0 */

/*
 * CPU 截止期索引接口学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 本头文件定义每个 root_domain 内的 SCHED_DEADLINE CPU 选择索引；真正的堆操作
 * 位于 cpudeadline.c。每个在线 CPU 若没有可运行的 DL 实体，就记录在 free_cpus；
 * 否则以该 CPU 的最早绝对截止期进入最大堆。这样迁移路径先找空闲且亲和性允许的
 * CPU，系统较忙时再用堆根快速判断是否存在“当前最早截止期更晚”的可抢占目标。
 * 它只生成候选，不迁移 task，也不替代调度域拓扑、容量和加锁后的运行队列复核。
 *
 * cpudl 嵌入 struct root_domain：init_rootdomain() 初始化并分配其动态存储，运行队列
 * 上下线及 DL 入队/出队路径更新它；root_domain 最后一个引用消失并经过 RCU 宽限期
 * 后，free_rootdomain() 调用 cpudl_cleanup()。更新方先持目标 rq 锁，再由 lock 串行化
 * 同一 root_domain 内不同 CPU 的堆和位图写入；无锁查找可能只得到瞬时候选，迁移路径
 * 随后锁住目标 rq 并复核，不能把一次 cpudl_find() 成功理解成迁移已承诺。
 *
 * 最大堆把“最晚的最早截止期”放在根部，查找近似 O(1)，更新为 O(log CPU 数)；
 * 代价是需要 nr_cpu_ids 个元素、CPU 到堆位置的反向索引以及共享原始自旋锁。空闲位图
 * 避免在轻载时扫描堆，但必须同时反映 rq 在线状态，否则可能把任务推向已下线 CPU。
 */
#include <linux/types.h>
#include <linux/spinlock.h>

/* CPU 当前不在堆中时，按 CPU 编号访问的 idx 字段使用该哨兵值。 */
#define IDX_INVALID		-1

/*
 * 一个数组槽同时承载堆节点字段和反向索引字段。
 *
 * 当槽位处于 [0, cpudl.size) 时，dl/cpu 表示该堆位置保存的 CPU 及其本地 DL
 * 运行队列最早绝对截止期；dl_time_before() 按可回绕时间语义比较 dl。另一方面，
 * elements[cpu].idx 始终按 CPU 编号使用，指出该 CPU 当前所在堆位置，或为
 * IDX_INVALID。两种视图复用同一数组但使用不同字段，堆上移/下移时必须同步更新 idx，
 * 否则后续按 CPU 更新会改到别的节点。数组由 cpudl_init() 分配，由 cleanup() 释放。
 */
struct cpudl_item {
	u64			dl;
	int			cpu;
	int			idx;
};

/*
 * 一个 root_domain 的 DL CPU 候选索引。
 *
 * lock 以 irqsave 方式串行化 size、elements 和 free_cpus 的所有更新，避免两个 rq
 * 同时调整堆时破坏“父节点截止期不早于子节点”以及 cpu→idx 反向映射；它不延长
 * root_domain 生命周期。size 是 elements[0..size) 中有效堆节点数。free_cpus 中置位
 * 的 CPU 必须同时满足“该 root_domain 中 rq 在线且没有可运行 DL 实体”；离线 CPU
 * 即使没有 DL 实体也必须清位。elements 的存储容量为 nr_cpu_ids。
 *
 * 查找方不取得 lock，因此这些字段对它是用于选候选的动态索引，而不是目标 rq 状态
 * 的长期快照；调用者仍须在迁移或抢占前按运行队列锁协议验证目标。
 */
struct cpudl {
	raw_spinlock_t		lock;
	int			size;
	cpumask_var_t		free_cpus;
	struct cpudl_item	*elements;
};

/*
 * cpudl_find() - 为 DL task 查询截止期更宽松的候选 CPU
 *
 * 调用位置：唤醒选核、DL push/CPU 下线迁移和同截止期抢占判断路径；本函数只读索引，
 * 不取得 cpudl.lock，也不修改 task 或任何 rq，不能睡眠。
 * @cp: 纯输入、借用的目标 root_domain 索引；调用期间其生命周期须由 rq 锁、RCU 或
 *      等价的调度器协议保证，函数不取得引用。
 * @p: 纯输入、不可为 NULL 的 DL task；读取其绝对 deadline、允许 CPU 掩码、当前 CPU
 *     和容量需求，不接管 task 引用。
 * @later_mask: 可为 NULL 的输出掩码。非 NULL 时优先写入“在线、无可运行 DL 实体且
 *              p 允许”的 CPU；非对称容量系统会剔除容纳不了 p 的 CPU，若全部不适配
 *              则保留容量最大的退化候选。没有空闲候选时至多加入最大堆根 CPU。
 *
 * 返回 1 表示找到了至少一个候选，返回 0 表示没有；返回值不是 CPU 编号。传 NULL
 * 不承接候选集合，因此跳过 free_cpus 快速路径，只用堆根回答存在性。堆根还必须在
 * p 的亲和性内，且 p 的截止期严格早于该 CPU 当前最早截止期。结果是可并发变化的
 * 选择提示，调用者必须在真正迁移前锁定并复核目标 rq。
 */
int  cpudl_find(struct cpudl *cp, struct task_struct *p, struct cpumask *later_mask);

/*
 * cpudl_set() - 发布某 CPU 当前最早的可运行 DL 截止期
 *
 * @cp: 输入输出的 root_domain 索引，借用且不转移所有权。
 * @cpu: present 且属于该 domain 的目标 CPU 编号；调用者须持有 cpu_rq(cpu)->lock。
 *       rq 即使正在下线也可能尚有待迁移 DL 实体，此时发布节点仍会保持 free 位清零。
 * @dl: 该 rq 新的最早绝对截止期，使用 DL 回绕时间比较语义。
 * 返回：无直接返回值，也没有可报告的失败。函数关本地中断并取得 cp->lock；CPU 首次
 * 出现时插入堆并从 free_cpus 清除，已存在时更新键值，随后恢复最大堆和反向索引。
 * 它不分配内存、不睡眠，也不改变 rq 或 task ownership。
 */
void cpudl_set(struct cpudl *cp, int cpu, u64 dl);

/*
 * cpudl_clear() - 移除某 CPU 的 DL 堆节点并同步空闲/在线状态
 *
 * @cp: 输入输出的 root_domain 索引，借用且不转移所有权。
 * @cpu: 要移除的 present CPU；调用者须持有 cpu_rq(cpu)->lock。
 * @online: 该 DL rq 此刻是否在线；true 使无 DL 实体的 CPU 进入 free_cpus，false
 *          强制清位，防止后续把任务推向已下线 CPU。
 * 返回：无直接返回值。函数以 irqsave 方式取得 cp->lock；CPU 原本不在堆中时仍会
 * 根据 @online 更新位图。存在节点时用末节点填洞、缩小 size 并恢复堆/反向索引。
 * 函数不分配内存、不睡眠，重复清除不会失败。
 */
void cpudl_clear(struct cpudl *cp, int cpu, bool online);

/*
 * cpudl_init() - 初始化新 root_domain 中的 CPU 截止期索引
 *
 * @cp: 纯输出对象，由调用者提供稳定存储；成功后持有 elements 数组和 free_cpus
 *      动态位图，后续必须由 cpudl_cleanup() 成对释放。
 * 返回 0 表示锁、空堆、空闲位图和所有 CPU 的 IDX_INVALID 反向索引均已建立；返回
 * -ENOMEM 表示数组或位图分配失败。GFP_KERNEL 分配允许睡眠，故不能从原子上下文调用。
 * 位图分配失败时函数已释放先前的数组；失败对象未完成初始化，调用者不得发布或清理
 * 它，init_rootdomain() 会沿自己的逆序回滚链释放更早取得的 root_domain 资源。
 */
int  cpudl_init(struct cpudl *cp);

/*
 * cpudl_cleanup() - 释放成功初始化的截止期索引所拥有的动态存储
 *
 * @cp: 输入输出对象，必须来自成功的 cpudl_init()，且所有并发查找/更新均已停止；
 *      free_rootdomain() 在最后一个引用消失并经过 RCU 宽限期后满足这一前置条件。
 * 返回：无直接返回值。释放 free_cpus 和 elements，不取得 cp->lock、不等待读者，也不
 * 清空字段；调用后对象不可再次查询、更新或重复清理，除非重新成功初始化。
 */
void cpudl_cleanup(struct cpudl *cp);
