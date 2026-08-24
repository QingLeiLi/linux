// SPDX-License-Identifier: GPL-2.0-only
/*
 *  kernel/sched/cpudeadline.c
 *
 *  Global CPU deadline management
 *
 *  Author: Juri Lelli <j.lelli@sssup.it>
 */
/*
 * 本文件实现每个 root_domain 的 SCHED_DEADLINE CPU 候选索引。非空 DL rq
 * 以“本队列最早绝对截止期”为键进入最大堆，堆根因而是最容易容纳更早 deadline
 * 任务的 CPU；没有 DL 实体且在线的 rq 另记入 free_cpus。索引只筛选候选，真正
 * 的迁移仍由 deadline.c 按亲和性、拓扑、容量和目标 rq 锁重新验证。
 */
#include "sched.h"

/*
 * 返回数组堆节点 i 的父节点下标。i 必须大于 0；调用者在使用前排除堆根。
 * 纯算术、无状态副作用，不取锁且不能失败。
 */
static inline int parent(int i)
{
	return (i - 1) >> 1;
}

/*
 * 返回节点 i 的左孩子下标；结果可能超出 cp->size，调用者据此判断孩子不存在。
 * 参数是非负有效堆位置，函数无副作用。
 */
static inline int left_child(int i)
{
	return (i << 1) + 1;
}

/*
 * 返回节点 i 的右孩子下标；与 left_child() 一样只计算位置，不检查堆边界。
 * 调用者必须用 cp->size 验证结果，函数无锁、不可睡眠。
 */
static inline int right_child(int i)
{
	return (i << 1) + 2;
}

/*
 * 把 idx 处键值变小后可能违反最大堆关系的节点向下移动。
 * @cp 是借用的可写 cpudl，调用者必须持有 cp->lock；@idx 位于有效堆区间。
 * 函数保存原节点，逐层把 deadline 更晚的孩子上提，最后一次性落下原节点；每次
 * 移动都同步 elements[cpu].idx 反向索引。无返回值、无分配且不能睡眠。
 */
static void cpudl_heapify_down(struct cpudl *cp, int idx)
{
	/* l/r/largest 是本轮孩子和胜出位置；orig_* 保存待下沉节点。 */
	int l, r, largest;

	int orig_cpu = cp->elements[idx].cpu;
	u64 orig_dl = cp->elements[idx].dl;

	if (left_child(idx) >= cp->size)
		/* 叶子没有需要比较的孩子，堆性质已成立。 */
		return;

	/* adapted from lib/prio_heap.c */
	/* 此下沉算法改编自 lib/prio_heap.c。 */
	while (1) {
		/* largest_dl 缓存当前候选的键，避免覆写数组后丢失比较基准。 */
		u64 largest_dl;

		l = left_child(idx);
		r = right_child(idx);
		largest = idx;
		largest_dl = orig_dl;

		if ((l < cp->size) && dl_time_before(orig_dl,
						cp->elements[l].dl)) {
			largest = l;
			largest_dl = cp->elements[l].dl;
		}
		if ((r < cp->size) && dl_time_before(largest_dl,
						cp->elements[r].dl))
			largest = r;

		if (largest == idx)
			/* 两个孩子都不比原节点更晚，已找到最终位置。 */
			break;

		/* pull largest child onto idx */
		/* 把截止期最晚的孩子上提填洞，并立即修正该 CPU 的反向位置。 */
		cp->elements[idx].cpu = cp->elements[largest].cpu;
		cp->elements[idx].dl = cp->elements[largest].dl;
		cp->elements[cp->elements[idx].cpu].idx = idx;
		idx = largest;
	}
	/* actual push down of saved original values orig_* */
	/* 最后把保存的原节点放入空洞，补齐它自己的 cpu→idx 映射。 */
	cp->elements[idx].cpu = orig_cpu;
	cp->elements[idx].dl = orig_dl;
	cp->elements[cp->elements[idx].cpu].idx = idx;
}

/*
 * 把 idx 处新插入或键值变大的节点向堆根移动。调用者持有 cp->lock，@idx 是
 * elements[0..size) 的有效位置。算法保存原节点并逐层下移父节点，维持最大堆和
 * CPU 反向索引；到根或遇到 deadline 不早于原节点的父节点时停止。无失败路径。
 */
static void cpudl_heapify_up(struct cpudl *cp, int idx)
{
	/* p 是父位置；orig_* 在移动期间持有待上浮节点的完整键和值。 */
	int p;

	int orig_cpu = cp->elements[idx].cpu;
	u64 orig_dl = cp->elements[idx].dl;

	if (idx == 0)
		/* 新值已经位于根，数组和反向索引无需移动。 */
		return;

	do {
		p = parent(idx);
		/* 父节点 deadline 更晚时最大堆关系成立；相等值允许继续上浮。 */
		if (dl_time_before(orig_dl, cp->elements[p].dl))
			break;
		/* pull parent onto idx */
		/* 父节点下移填洞时同步更新被移动 CPU 的反向位置。 */
		cp->elements[idx].cpu = cp->elements[p].cpu;
		cp->elements[idx].dl = cp->elements[p].dl;
		cp->elements[cp->elements[idx].cpu].idx = idx;
		idx = p;
	} while (idx != 0);
	/* actual push up of saved original values orig_* */
	/* 把原节点写入最终空洞，并发布其新的反向位置。 */
	cp->elements[idx].cpu = orig_cpu;
	cp->elements[idx].dl = orig_dl;
	cp->elements[cp->elements[idx].cpu].idx = idx;
}

/*
 * 在某个已有节点 deadline 改变或删除填洞后恢复最大堆。
 * @cp/@idx 的所有权与锁前置条件同上下浮 helper。若新键比父键更晚则上浮，否则
 * 下沉；根节点只能下沉。函数同时保持 cpu→idx 映射，无返回值且不会睡眠。
 */
static void cpudl_heapify(struct cpudl *cp, int idx)
{
	if (idx > 0 && dl_time_before(cp->elements[parent(idx)].dl,
				cp->elements[idx].dl))
		cpudl_heapify_up(cp, idx);
	else
		cpudl_heapify_down(cp, idx);
}

/*
 * 返回最大堆根记录的 CPU 编号。调用者保证 elements 已分配；空堆初始化为零，
 * kzalloc 使根 cpu 为 0，而清空最后节点后旧根值可能保留，因此调用路径只在相应
 * 候选判断中使用结果。函数只读动态索引，不取得 cp->lock，结果可能立即过期。
 */
static inline int cpudl_maximum(struct cpudl *cp)
{
	return cp->elements[0].cpu;
}

/*
 * cpudl_find - find the best (later-dl) CPU in the system
 * @cp: the cpudl max-heap context
 * @p: the task
 * @later_mask: a mask to fill in with the selected CPUs (or NULL)
 *
 * Returns: int - CPUs were found
 */
/*
 * cpudl_find() - 为 DL 任务寻找截止期更宽松的候选 CPU
 *
 * @cp: 借用的 root_domain 索引；本函数不取得 cp->lock，也不接管生命周期。
 * @p: 不可为 NULL 的 DL 任务，只读取亲和性、deadline、当前 CPU 和容量需求。
 * @later_mask: 可为 NULL 的输出掩码；非 NULL 时由调用者提供可写存储。
 *
 * 首选在线且没有可运行 DL 实体的 free CPU；非对称容量机器再删除装不下任务的
 * CPU，若全部被删则保留容量最大的退化候选。没有 free CPU 时只检查最大堆根：
 * 它必须在任务亲和性内，且其本地最早 deadline 晚于任务 deadline。返回 1 只表示
 * 找到瞬时候选，0 表示当前索引无候选；函数不迁移任务、不锁 rq、不能睡眠，调用者
 * 在真正 push/wakeup 前必须结合拓扑并锁住目标 rq 复核。
 */
int cpudl_find(struct cpudl *cp, struct task_struct *p,
	       struct cpumask *later_mask)
{
	/* dl_se 只借用 p 内嵌实体，用于回绕安全的绝对 deadline 比较。 */
	const struct sched_dl_entity *dl_se = &p->dl;

	/* 写出 free_cpus、任务亲和性两者交集；非空即走轻载快速路径。 */
	if (later_mask &&
	    cpumask_and(later_mask, cp->free_cpus, &p->cpus_mask)) {
		/* max_* 记录所有不适配 CPU 中容量最大的保底目标。 */
		unsigned long cap, max_cap = 0;
		int cpu, max_cpu = -1;

		/* 同构容量无需逐 CPU 筛选，整个交集都可交给后续拓扑选择。 */
		if (!sched_asym_cpucap_active())
			return 1;

		/* Ensure the capacity of the CPUs fits the task. */
		/* 确保保留 CPU 的算力能够容纳该任务。 */
		for_each_cpu(cpu, later_mask) {
			if (!dl_task_fits_capacity(p, cpu)) {
				/* 不适配 CPU 从正常候选中删除，但仍参与退化容量比较。 */
				cpumask_clear_cpu(cpu, later_mask);

				cap = arch_scale_cpu_capacity(cpu);

				if (cap > max_cap ||
				    (cpu == task_cpu(p) && cap == max_cap)) {
					/* 容量相等时偏向任务当前 CPU，以保留缓存局部性。 */
					max_cap = cap;
					max_cpu = cpu;
				}
			}
		}

		/* 所有 free CPU 都容量不足时仍返回最大的一个，让后续路径决定取舍。 */
		if (cpumask_empty(later_mask))
			cpumask_set_cpu(max_cpu, later_mask);

		return 1;
	} else {
		/* 无输出掩码或无 free 交集时，以堆根作唯一繁忙 CPU 候选。 */
		int best_cpu = cpudl_maximum(cp);

		/* 索引中不应出现已从 present mask 移除的 CPU；告警后仍继续判定。 */
		WARN_ON(best_cpu != -1 && !cpu_present(best_cpu));

		/* 亲和性允许且任务更紧迫时，该 rq 上当前 DL 实体可被任务抢占。 */
		if (cpumask_test_cpu(best_cpu, &p->cpus_mask) &&
		    dl_time_before(dl_se->deadline, cp->elements[0].dl)) {
			if (later_mask)
				cpumask_set_cpu(best_cpu, later_mask);

			return 1;
		}
	}
	/* 两级索引都不能给出合法目标，later_mask 保持为空或原来的失败结果。 */
	return 0;
}

/*
 * cpudl_clear - remove a CPU from the cpudl max-heap
 * @cp: the cpudl max-heap context
 * @cpu: the target CPU
 * @online: the online state of the deadline runqueue
 *
 * Notes: assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
/*
 * cpudl_clear() - 移除 CPU 的 DL 堆节点并同步空闲/在线位图
 *
 * @cp: 借用且可写的 root_domain 索引；不转移所有权。
 * @cpu: present CPU 编号；调用者已经持有 cpu_rq(cpu)->lock。
 * @online: true 表示清空 DL 实体后该 rq 仍在线，应进入 free_cpus；false 表示 rq
 *          下线，必须清位，不能再成为迁移目标。
 *
 * 函数 irqsave 获取 cp->lock，与同 domain 其他 CPU 的 set/clear 串行化。存在节点时
 * 用末节点填洞、缩小 size、修正反向索引并恢复堆；节点本就不存在时仍更新 online
 * 位图。无返回值、无分配、不可睡眠，重复清除是允许的。
 */
void cpudl_clear(struct cpudl *cp, int cpu, bool online)
{
	/* old_idx 定位待删节点，new_cpu 是从堆尾搬来填洞的 CPU。 */
	int old_idx, new_cpu;
	/* flags 保存调用 CPU 的中断状态，解锁时原样恢复。 */
	unsigned long flags;

	/* 错误调用只告警；后续仍依赖 cpu 下标有效，调用者必须保证范围。 */
	WARN_ON(!cpu_present(cpu));

	/* 锁同时保护 size、堆数组的节点视图、反向 idx 和 free_cpus。 */
	raw_spin_lock_irqsave(&cp->lock, flags);

	old_idx = cp->elements[cpu].idx;
	if (old_idx == IDX_INVALID) {
		/*
		 * Nothing to remove if old_idx was invalid.
		 * This could happen if rq_online_dl or rq_offline_dl is
		 * called for a CPU without -dl tasks running.
		 */
		/*
		 * old_idx 无效时没有节点可删；无 DL 任务的 CPU 执行 rq 上下线
		 * 回调就会出现这种情况，下面仍需按 online 修正 free_cpus。
		 */
	} else {
		/* 用最后一个有效节点覆盖删除位置；删除末节点时等价于自覆盖。 */
		new_cpu = cp->elements[cp->size - 1].cpu;
		cp->elements[old_idx].dl = cp->elements[cp->size - 1].dl;
		cp->elements[old_idx].cpu = new_cpu;
		cp->size--;
		cp->elements[new_cpu].idx = old_idx;
		/* 被删 CPU 先失去反向入口，后续 set 会走重新插入路径。 */
		cp->elements[cpu].idx = IDX_INVALID;
		/* 填洞键可能比父大或比孩子小，统一 helper 决定上浮或下沉。 */
		cpudl_heapify(cp, old_idx);
	}
	/* online 决定“空 rq”是否可选；它与堆成员资格是两个相邻但独立的状态。 */
	if (likely(online))
		__cpumask_set_cpu(cpu, cp->free_cpus);
	else
		__cpumask_clear_cpu(cpu, cp->free_cpus);

	raw_spin_unlock_irqrestore(&cp->lock, flags);
}

/*
 * cpudl_set - update the cpudl max-heap
 * @cp: the cpudl max-heap context
 * @cpu: the target CPU
 * @dl: the new earliest deadline for this CPU
 *
 * Notes: assumes cpu_rq(cpu)->lock is locked
 *
 * Returns: (void)
 */
/*
 * cpudl_set() - 发布某 CPU 当前最早的可运行 DL deadline
 *
 * @cp: 借用且可写的 root_domain 索引。
 * @cpu: present CPU 编号，调用者已经持有 cpu_rq(cpu)->lock。
 * @dl: 该 rq 新的最早绝对截止期，按 dl_time_before() 的回绕规则比较。
 *
 * CPU 不在堆中时追加节点、建立反向索引、上浮并清除 free 位；已在堆中时改键后
 * 双向恢复堆。cp->lock 以 irqsave 方式保护跨 rq 更新；函数不分配、不失败、不睡眠，
 * 也不修改任务或 rq 本身。
 */
void cpudl_set(struct cpudl *cp, int cpu, u64 dl)
{
	/* old_idx 是 cpu→堆位置的反向索引；flags 保存本地中断状态。 */
	int old_idx;
	unsigned long flags;

	WARN_ON(!cpu_present(cpu));

	raw_spin_lock_irqsave(&cp->lock, flags);

	old_idx = cp->elements[cpu].idx;
	if (old_idx == IDX_INVALID) {
		/* size 旧值是新节点位置，递增后把它纳入有效堆区间。 */
		int new_idx = cp->size++;

		/* 先写完整节点和反向映射，再在锁内恢复堆并撤销 free 候选。 */
		cp->elements[new_idx].dl = dl;
		cp->elements[new_idx].cpu = cpu;
		cp->elements[cpu].idx = new_idx;
		cpudl_heapify_up(cp, new_idx);
		__cpumask_clear_cpu(cpu, cp->free_cpus);
	} else {
		/* 已有节点只需改键；新 deadline 可能要求向任一方向移动。 */
		cp->elements[old_idx].dl = dl;
		cpudl_heapify(cp, old_idx);
	}

	raw_spin_unlock_irqrestore(&cp->lock, flags);
}

/*
 * cpudl_init - initialize the cpudl structure
 * @cp: the cpudl max-heap context
 */
/*
 * cpudl_init() - 初始化新 root_domain 的 CPU deadline 索引
 *
 * @cp: 调用者提供的输出对象；成功后拥有 nr_cpu_ids 个 elements 和动态 CPU 位图，
 *      必须由 cpudl_cleanup() 成对释放。失败时对象未完成初始化，不得发布使用。
 *
 * 依次初始化锁/空堆、分配零填充节点数组、分配零填充 free_cpus，再把每个 possible
 * CPU 的反向位置设为 IDX_INVALID。返回 0 成功，任一分配失败返回 -ENOMEM；位图失败
 * 会先归还数组。GFP_KERNEL 允许睡眠，因此只能在可睡眠的 root_domain 初始化路径调用。
 */
int cpudl_init(struct cpudl *cp)
{
	/* i 遍历 possible CPU，为按 CPU 使用的 idx 视图建立无效哨兵。 */
	int i;

	/* 锁必须早于对象发布初始化；size=0 表示有效堆区间为空。 */
	raw_spin_lock_init(&cp->lock);
	cp->size = 0;

	cp->elements = kzalloc_objs(struct cpudl_item, nr_cpu_ids);
	/* 第一处分配失败时没有动态资源需要回滚。 */
	if (!cp->elements)
		return -ENOMEM;

	if (!zalloc_cpumask_var(&cp->free_cpus, GFP_KERNEL)) {
		/* 位图失败时逆序释放已取得的节点数组。 */
		kfree(cp->elements);
		return -ENOMEM;
	}

	for_each_possible_cpu(i)
		/* 节点数组的 idx 字段按 CPU 编号访问，初始均不属于堆。 */
		cp->elements[i].idx = IDX_INVALID;

	/* 此时对象可由 init_rootdomain() 发布给各 rq 使用。 */
	return 0;
}

/*
 * cpudl_cleanup - clean up the cpudl structure
 * @cp: the cpudl max-heap context
 */
/*
 * cpudl_cleanup() - 释放成功初始化的 deadline 索引
 *
 * @cp: 必须来自成功的 cpudl_init()；调用者已阻止所有并发 find/set/clear。正常路径
 *      是 root_domain 最后一个引用消失并经过 RCU 宽限期后的 free_rootdomain()。
 *
 * 函数先释放动态位图再释放节点数组，无返回值、不取 cp->lock，也不清空悬空字段；
 * 调用后对象不可继续访问或重复 cleanup。释放本身不要求转移任何 task/rq ownership。
 */
void cpudl_cleanup(struct cpudl *cp)
{
	/* 两项资源都由 init 成功路径独占获得，按对象销毁协议一次性释放。 */
	free_cpumask_var(cp->free_cpus);
	kfree(cp->elements);
}
