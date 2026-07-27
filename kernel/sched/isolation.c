// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU 隔离与 housekeeping 管理学习导读
 *
 * 中文学习注释模型：OpenAI Codex（GPT-5）。
 *
 * 【文件职责】
 * CPU 隔离不是简单地把 CPU 从系统中“关掉”，而是把可迁移的内核杂务集中到
 * 一组 housekeeping CPU，让 nohz_full 或 isolated CPU 尽量少受调度负载均衡、
 * 非绑定中断、内核线程、定时器和工作队列等噪声干扰。本文件负责：
 *
 *   启动命令行 nohz_full=/isolcpus=
 *          → 解析“非 housekeeping CPU”列表
 *          → 按 hk_type 建立其补集，即 housekeeping cpumask
 *          → housekeeping_init() 将早期 memblock 掩码迁入 slab
 *          → 各子系统通过查询接口选择或绑定 housekeeping CPU
 *
 * cpuset v2 还可在运行期建立 isolated partition。该路径调用
 * housekeeping_update()，只更新动态的 HK_TYPE_DOMAIN 掩码，再把变化传播给
 * unbound workqueue、timer migration hierarchy 和 kthread affinity。
 *
 * 【职责边界】
 * 本文件只维护隔离策略的公共事实和查询/传播入口，不负责真正停止 tick、
 * 重建调度域、迁移 IRQ、执行 workqueue 工作或管理 cpuset 层级；这些动作由
 * tick、scheduler topology、IRQ、workqueue、kthread 和 cpuset 子系统完成。
 *
 * 【核心对象与生命周期】
 * housekeeping.flags 表示哪些 hk_type 已被命令行或运行期策略覆盖；
 * housekeeping.cpumasks[type] 指向该类型允许承接杂务的 CPU 集合。启动早期
 * 掩码由 memblock 分配，housekeeping_init() 后改由 kmalloc 分配。运行期
 * HK_TYPE_DOMAIN 更新使用 RCU 发布新指针，等待旧读者退出后才 kfree(old)。
 * 查询接口返回的是借用指针，调用者不得保存、修改或释放它。
 *
 * 【并发模型】
 * housekeeping_overridden static key 让完全未启用隔离的常见配置跳过掩码查询；
 * flags 用 READ_ONCE() 保证无锁读取得单次一致标量值；动态 DOMAIN 指针用 RCU
 * 替换。DOMAIN 掩码的普通读者必须处于启动阶段、CPU hotplug 写侧或 cpuset
 * 锁保护下，housekeeping_dereference_check() 把这份约束交给 lockdep 检查。
 * RCU只保证旧掩码内存不会过早释放，并不替代 cpuset 状态的一致性锁。
 *
 * 【方案权衡】
 * 按类型保存掩码，使调度域、managed IRQ 和 kernel-noise 隔离可以分别启用；
 * 类型别名又让 nohz_full 相关杂务共享一张掩码，降低查询与维护成本。代价是
 * 所有可卸载子系统都必须遵循这套掩码，并在动态 DOMAIN 更新后显式刷新其
 * 缓存/亲和性；短暂不一致通过“先发布新掩码、再逐个传播”的顺序收敛。
 */
/*
 *  Housekeeping management. Manage the targets for routine code that can run on
 *  any CPU: unbound workqueues, timers, kthreads and any offloadable work.
 *
 * Copyright (C) 2017 Red Hat, Inc., Frederic Weisbecker
 * Copyright (C) 2017-2018 SUSE, Frederic Weisbecker
 *
 */
/*
 * Housekeeping 管理：为原本可以在任意 CPU 上运行的日常内核工作选择承载目标，
 * 包括 unbound workqueue、定时器、内核线程以及其他能够卸载的工作。这里保存的
 * 是“允许承担杂务”的 CPU，而启动参数通常给出的是希望免受杂务干扰的 CPU，
 * 因此解析阶段会取补集。
 */
#include <linux/sched/isolation.h>
#include <linux/pci.h>
#include "sched.h"

/*
 * 内部 flag 与公开 hk_type 严格按 BIT(type) 对齐，因此同一个 unsigned long
 * 既能表示“哪些类型已启用”，也能直接供 for_each_set_bit() 遍历。
 *
 * DOMAIN_BOOT 是 isolcpus=domain 在启动时确定的固定上界；DOMAIN 初始与它相同，
 * 但运行期还会扣除 cpuset isolated partition，所以 DOMAIN 始终是 DOMAIN_BOOT
 * 的子集。MANAGED_IRQ 只影响 managed interrupt 的目标选择。KERNEL_NOISE 对应
 * nohz_full/isolcpus=nohz，并由 TICK、TIMER、RCU、MISC、WQ 等公开别名共享。
 * enum 只形成位值映射，不拥有任何资源，生命周期贯穿整个内核运行期。
 */
enum hk_flags {
	HK_FLAG_DOMAIN_BOOT	= BIT(HK_TYPE_DOMAIN_BOOT),
	HK_FLAG_DOMAIN		= BIT(HK_TYPE_DOMAIN),
	HK_FLAG_MANAGED_IRQ	= BIT(HK_TYPE_MANAGED_IRQ),
	HK_FLAG_KERNEL_NOISE	= BIT(HK_TYPE_KERNEL_NOISE),
};

/*
 * static key 初始为 false：没有隔离参数时，查询热路径会被 jump-label 机制修补成
 * 几乎无额外分支开销的“所有 CPU 都可用”。一旦任一类型被覆盖便只启用、不再
 * 关闭，因为启动参数不可撤销，而运行期 DOMAIN 即使变化也仍处于覆盖模式。
 * 该键导出给头文件中的 housekeeping_cpu() 热路径使用。
 */
DEFINE_STATIC_KEY_FALSE(housekeeping_overridden);
EXPORT_SYMBOL_GPL(housekeeping_overridden);

/*
 * 全局 housekeeping 策略快照：
 * @cpumasks[type]：该类型的 housekeeping CPU 集合。指针本身通过 RCU 发布；
 *                  指向的 cpumask 发布后只读，更新时分配新对象而不原地改写。
 * @flags：          已配置类型的位图；某位为 0 时相应 cpumasks 槽位无须有效，
 *                  查询接口回退到 cpu_possible_mask。
 *
 * 对象是静态存储期，不销毁；只有各槽位指向的动态掩码会在初始化迁移或运行期
 * DOMAIN 更新时更换。flags 位必须在发布对应指针之前已有有效掩码，或与发布顺序
 * 配合，使读者绝不会把未初始化槽位当成有效策略。
 */
struct housekeeping {
	struct cpumask __rcu *cpumasks[HK_TYPE_MAX];
	unsigned long flags;
};

/* 唯一全局策略实例；静态零初始化同时表示“无类型被隔离策略覆盖”。 */
static struct housekeeping housekeeping;

/*
 * housekeeping_enabled() - 查询某类 housekeeping 策略是否被显式启用。
 *
 * 调用关系：IRQ、workqueue、cpuset、timer 等子系统在决定是否需要特殊处理前
 * 调用；若返回 false，它们可采用未隔离系统的默认路径。
 * 入参 @type：HK_TYPE_* 类型索引，必须位于 [0, HK_TYPE_MAX)，纯输入，无所有权。
 * 上下文：无锁、不可睡眠，适用于热路径；READ_ONCE 防止编译器拆分或合并对
 * flags 的读取，但它不是锁，也不为 cpumask 内容提供生命周期保证。
 * 返回：对应位已设置返回 true，否则 false；无输出参数、无状态副作用。
 */
bool housekeeping_enabled(enum hk_type type)
{
	return !!(READ_ONCE(housekeeping.flags) & BIT(type));
}
EXPORT_SYMBOL_GPL(housekeeping_enabled);

/*
 * housekeeping_dereference_check() - 为 RCU/lockdep 描述 cpumask 解引用条件。
 *
 * 调用关系：仅由 housekeeping_cpumask_dereference() 传给
 * rcu_dereference_all_check()。@type 是待读取槽位，纯输入、无所有权。
 *
 * HK_TYPE_DOMAIN 会被 cpuset 在运行期替换，因此读者必须处于以下任一稳定窗口：
 * cpuset 尚不可写的早期启动阶段、持有 CPU hotplug 写锁，或持有 cpuset 锁。
 * 其他类型只在启动阶段建立、之后不替换，所以总是允许解引用。
 *
 * 返回：true 表示当前上下文符合读侧协议；false 只供 lockdep 在调试配置下报告
 * 可疑访问，不阻止实际读取。函数不睡眠、不修改状态。
 */
static bool housekeeping_dereference_check(enum hk_type type)
{
	if (IS_ENABLED(CONFIG_LOCKDEP) && type == HK_TYPE_DOMAIN) {
		/* Cpuset isn't even writable yet? */
		/*
		 * cpuset 甚至还不能写入吗？在 SYSTEM_SCHEDULING 及更早阶段，
		 * 动态 isolated partition 尚未出现，启动掩码不会并发替换。
		 */
		if (system_state <= SYSTEM_SCHEDULING)
			return true;

		/* CPU hotplug write locked, so cpuset partition can't be overwritten */
		/*
		 * 已持有 CPU hotplug 写锁，因此 cpuset partition 不能被并发覆盖。
		 * 这提供的是状态稳定条件；旧 cpumask 的内存寿命仍由 RCU 更新协议处理。
		 */
		if (IS_ENABLED(CONFIG_HOTPLUG_CPU) && lockdep_is_cpus_write_held())
			return true;

		/* Cpuset lock held, partitions not writable */
		/*
		 * 已持有 cpuset 锁，partition 不可写。cpuset 控制文件更新路径以该锁
		 * 串行化策略修改，读者因而能把 DOMAIN 掩码与相应 cpuset 状态配套理解。
		 */
		if (IS_ENABLED(CONFIG_CPUSETS) && lockdep_is_cpuset_held())
			return true;

		/* DOMAIN 存在运行期写者，而当前没有任何已知的状态稳定条件。 */
		return false;
	}

	/* 启动后不再动态替换的类型不需要额外 cpuset/CPU-hotplug 锁。 */
	return true;
}

/*
 * housekeeping_cpumask_dereference() - 按读侧协议取得某类型的当前掩码。
 *
 * @type：有效的 HK_TYPE_* 索引，纯输入。返回 housekeeping 持有的借用指针；
 * 调用者不得修改、释放或越过其同步保护长期保存。函数不睡眠、无副作用。
 *
 * rcu_dereference_all_check() 同时保留 RCU 指针读取所需的依赖/编译器语义，并让
 * lockdep 接受上面列出的非传统保护条件。这里没有获取引用；对可动态更新的
 * DOMAIN，调用者必须遵循检查函数描述的外部锁协议。
 */
static inline struct cpumask *housekeeping_cpumask_dereference(enum hk_type type)
{
	return rcu_dereference_all_check(housekeeping.cpumasks[type],
					 housekeeping_dereference_check(type));
}

/*
 * housekeeping_cpumask() - 返回某类工作当前允许使用的 housekeeping CPU 集合。
 *
 * 调用关系：调度、IRQ、workqueue、timer、RCU 和 kthread 等消费者的公共查询
 * 入口。@type 为有效 HK_TYPE_*，纯输入、无所有权变化。
 * 返回：若该类型已覆盖，返回全局策略持有的只读借用掩码；否则返回永久存在的
 * cpu_possible_mask，表达“所有可能 CPU 都可承担该类工作”。永不返回 NULL。
 * 函数无锁、不睡眠、不分配；返回指针不可修改或释放。
 *
 * static key 先过滤绝大多数未隔离系统；flags 再区分具体类型。两层检查不可只看
 * static key，因为系统可能只启用了 managed_irq，而查询的是 DOMAIN。
 */
const struct cpumask *housekeeping_cpumask(enum hk_type type)
{
	/* NULL 是局部哨兵：尚未找到已配置掩码，出口将统一回退。 */
	const struct cpumask *mask = NULL;

	if (static_branch_unlikely(&housekeeping_overridden)) {
		if (READ_ONCE(housekeeping.flags) & BIT(type))
			/*
			 * 这里只借用已发布指针。DOMAIN 的运行期调用者还必须满足
			 * housekeeping_dereference_check() 所描述的外部同步条件。
			 */
			mask = housekeeping_cpumask_dereference(type);
	}
	/*
	 * 未启用隔离或该类型未配置时，不制造空掩码：默认允许全部 possible CPU，
	 * 保持消费者在普通配置下原有语义，也避免每个调用者重复写 fallback。
	 */
	if (!mask)
		mask = cpu_possible_mask;
	return mask;
}
EXPORT_SYMBOL_GPL(housekeeping_cpumask);

/*
 * housekeeping_any_cpu() - 为指定类型选择一个可运行的在线 housekeeping CPU。
 *
 * 调用关系：timer、ring buffer、调度等需要把可迁移工作投递到某个 CPU 时调用。
 * @type 是工作类别，纯输入；通常在不可睡眠路径中使用。
 * 返回：优先返回与当前 CPU NUMA 距离近的合格 CPU；若没有该结果，则在
 * housekeeping 掩码与 cpu_online_mask 的交集中分散选择；启动早期极端情况下
 * 回退当前 CPU。返回值是 CPU 编号，不转移资源、无直接状态修改。
 *
 * 选择是瞬时快照，返回后 CPU 仍可能 hot-unplug；真正投递工作的调用者仍需按
 * 自身 API 处理热插拔竞态。本函数只保证选择时尽量符合策略。
 */
int housekeeping_any_cpu(enum hk_type type)
{
	/* cpu 保存候选 CPU 编号；nr_cpu_ids 及以上表示“未找到”。 */
	int cpu;

	if (static_branch_unlikely(&housekeeping_overridden)) {
		if (housekeeping.flags & BIT(type)) {
			/*
			 * 第一阶段：借助调度器维护的 NUMA 距离掩码，在允许集合中寻找
			 * 靠近当前 CPU 的候选，减少跨节点访问代价；找到即走快速出口。
			 */
			cpu = sched_numa_find_closest(housekeeping_cpumask(type), smp_processor_id());
			if (cpu < nr_cpu_ids)
				return cpu;

			/*
			 * 第二阶段：只在当前在线 CPU 中选择，并用 distribute 版本避免所有
			 * 调用者长期集中到掩码中的第一个 CPU。它牺牲严格 NUMA 就近以确保
			 * 找到实际可接收工作的 housekeeping CPU。
			 */
			cpu = cpumask_any_and_distribute(housekeeping_cpumask(type), cpu_online_mask);
			if (likely(cpu < nr_cpu_ids))
				return cpu;
			/*
			 * Unless we have another problem this can only happen
			 * at boot time before start_secondary() brings the 1st
			 * housekeeping CPU up.
			 */
			/*
			 * 除非系统另有错误，这只会发生在启动阶段：start_secondary()
			 * 尚未把第一个 housekeeping CPU 拉起。正常运行期或非 TIMER 类型
			 * 出现空在线交集都违反配置不变量，因此告警；随后仍回退当前 CPU，
			 * 让启动计时工作能够继续，而不是返回无效 CPU 编号。
			 */
			WARN_ON_ONCE(system_state == SYSTEM_RUNNING ||
				     type != HK_TYPE_TIMER);
		}
	}
	/*
	 * 未启用该类型，或仅在上述启动窗口暂无在线 housekeeper：当前 CPU 是唯一
	 * 无需额外选择/迁移的安全退化结果，但隔离保证在异常窗口内可能暂时不成立。
	 */
	return smp_processor_id();
}
EXPORT_SYMBOL_GPL(housekeeping_any_cpu);

/*
 * housekeeping_affine() - 将任务的允许 CPU 收紧到某类 housekeeping 集合。
 *
 * 调用关系：RCU 等后台 kthread 在启动后调用，把自身移出隔离 CPU。
 * @t：待改亲和性的 task_struct 借用指针，必须指向仍存活任务，不可为 NULL；
 * @type：决定使用哪张掩码，纯输入。调用者不向本函数转移 task 引用。
 * 上下文：set_cpus_allowed_ptr() 可能获取调度锁并迁移/等待任务，调用者必须处于
 * 可睡眠的进程上下文，不能持有与调度亲和性更新冲突的锁。
 * 返回：无直接返回值；仅在该类型已配置时修改 t 的 cpus_mask/迁移状态。
 *
 * 本接口为 void，因而不会把 set_cpus_allowed_ptr() 的错误传回调用者；配置阶段
 * 保证 housekeeping 集合非空，调用者仍应把它视为策略性亲和操作而非资源事务。
 * 未覆盖配置时保持任务原亲和性。
 */
void housekeeping_affine(struct task_struct *t, enum hk_type type)
{
	if (static_branch_unlikely(&housekeeping_overridden))
		if (housekeeping.flags & BIT(type))
			/*
			 * 真正的状态转换点在调度器 helper 内：它串行化亲和性修改，并在
			 * 必要时把正在错误 CPU 上运行的任务迁往允许集合。
			 */
			set_cpus_allowed_ptr(t, housekeeping_cpumask(type));
}
EXPORT_SYMBOL_GPL(housekeeping_affine);

/*
 * housekeeping_test_cpu() - 判断 CPU 是否允许承担指定类型的 housekeeping 工作。
 *
 * @cpu：待测试的 CPU 编号，必须是 cpumask 可表示的有效编号；纯输入。
 * @type：工作类型，纯输入。函数可用于热路径，不睡眠、无副作用。
 * 返回：该类型未配置时恒为 true；已配置时返回 cpu 是否在相应掩码中。
 *
 * 这表示“策略允许”，不同时保证 CPU online/active；需要实际投递工作的调用者
 * 还必须与 cpu_online_mask 或 CPU hotplug 协议组合。
 */
bool housekeeping_test_cpu(int cpu, enum hk_type type)
{
	if (static_branch_unlikely(&housekeeping_overridden) &&
	    READ_ONCE(housekeeping.flags) & BIT(type))
		/* 借用只读掩码做成员测试，不获取掩码所有权或 CPU 热插拔引用。 */
		return cpumask_test_cpu(cpu, housekeeping_cpumask(type));
	return true;
}
EXPORT_SYMBOL_GPL(housekeeping_test_cpu);

/*
 * housekeeping_update() - 按 cpuset isolated partition 动态更新 DOMAIN 掩码。
 *
 * 调用关系：cpuset 在 partition 配置提交后、释放 cpu hotplug/cpuset 内层锁，
 * 但仍持有 cpuset_top_mutex 时调用；该互斥量串行化并发控制文件更新。
 * @isol_mask：当前所有 cpuset isolated partition CPU 的借用掩码，纯输入，
 *             调用期间必须稳定，函数不保存、不修改也不释放它。
 * 上下文：进程上下文，可睡眠；函数会 GFP_KERNEL 分配、synchronize_rcu()、
 * flush 多个 workqueue 并更新任务亲和性，绝不能在自旋锁或 RCU 读侧调用。
 *
 * 返回：0 表示新 DOMAIN 掩码已发布并已尝试传播；-ENOMEM 表示发布前分配失败；
 * -EINVAL 表示更新后没有任何在线 housekeeping CPU，此时旧策略保持不变。
 * 下游传播 helper 的错误只触发 WARN，不能回滚已发布掩码，因此返回仍为 0；
 * 这是一个发布后尽力收敛、而非跨子系统原子事务的接口。
 *
 * 所有权：trial 分配后由本函数持有；发布成功即转成全局当前掩码，原 old 在
 * RCU grace period 后由本函数释放。无论成功失败，调用者仍拥有 isol_mask。
 */
int housekeeping_update(struct cpumask *isol_mask)
{
	/*
	 * 变量地图：
	 *   trial 新的 DOMAIN housekeeping 集合，发布前由本函数独占；
	 *   old   被替换的旧集合，若此前无 DOMAIN 策略则保持 NULL；
	 *   err   各传播 helper 的诊断结果，不作为本函数事务回滚依据。
	 */
	struct cpumask *trial, *old = NULL;
	int err;

	/* 阶段 1：先在任何全局状态变化前准备新快照，分配失败可无副作用返回。 */
	trial = kmalloc(cpumask_size(), GFP_KERNEL);
	if (!trial)
		return -ENOMEM;

	/*
	 * DOMAIN_BOOT 是启动参数给出的最大 housekeeping 集合；运行期 cpuset
	 * isolated CPU 再从中扣除。不能从 cpu_possible_mask 直接扣除，否则会把
	 * isolcpus=domain 已永久隔离的 CPU 错误加回调度域 housekeeping 集合。
	 */
	cpumask_andnot(trial, housekeeping_cpumask(HK_TYPE_DOMAIN_BOOT), isol_mask);
	/*
	 * 至少要有一个当前在线 CPU 承担调度域及关联杂务。失败发生在发布前，
	 * 所以释放 trial 后旧掩码和所有消费者均保持原状。
	 */
	if (!cpumask_intersects(trial, cpu_online_mask)) {
		kfree(trial);
		return -EINVAL;
	}

	/*
	 * 阶段 2：首次由纯 cpuset 动态启用 DOMAIN 时打开全局 static key。
	 * 键只启用一次；此后即使掩码继续更新也无需重复修补 jump label。
	 */
	if (!housekeeping.flags)
		static_branch_enable(&housekeeping_overridden);

	/*
	 * 若 DOMAIN 已存在，先借用旧指针供发布后回收；否则先标记该类型有效。
	 * trial 已完全初始化，紧随其后的 rcu_assign_pointer() 才是新读者可观察到
	 * 新集合的发布点。cpuset_top_mutex 保证不会有第二个 writer 同时换表。
	 */
	if (housekeeping.flags & HK_FLAG_DOMAIN)
		old = housekeeping_cpumask_dereference(HK_TYPE_DOMAIN);
	else
		WRITE_ONCE(housekeeping.flags, housekeeping.flags | HK_FLAG_DOMAIN);
	rcu_assign_pointer(housekeeping.cpumasks[HK_TYPE_DOMAIN], trial);

	/*
	 * 阶段 3：等待发布前已经取得 old 的 RCU 读者全部离开。此后 old 不再可能
	 * 被合法新读者获得，已经满足最终释放的生命周期条件；实现把实际 kfree()
	 * 统一放在下游传播步骤之后，使发布、传播、旧对象回收形成清晰顺序。
	 */
	synchronize_rcu();

	/*
	 * 阶段 4：在修改 unbound workqueue 亲和性前，先排空几个已知会把工作固定到
	 * 旧 CPU 策略的专用队列。flush 可能睡眠；这也是 cpuset 调用者必须提前释放
	 * cpus_read_lock/cpuset_mutex、只保留顶层串行 mutex 的原因。
	 */
	pci_probe_flush_workqueue();
	mem_cgroup_flush_workqueue();
	vmstat_flush_workqueue();

	/*
	 * 将新 DOMAIN 集合叠加到用户请求的 unbound workqueue affinity。失败时
	 * workqueue helper 保留其 fallback 掩码并返回 errno；全局 DOMAIN 已发布，
	 * 无法无竞态地整体回滚，因此这里只告警，暴露暂时/局部不一致。
	 */
	err = workqueue_unbound_housekeeping_update(housekeeping_cpumask(HK_TYPE_DOMAIN));
	WARN_ON_ONCE(err < 0);

	/*
	 * timer migration hierarchy 必须排除 isolated partition CPU，避免其他 CPU
	 * 的可迁移定时器又被推到隔离 CPU；helper 会在目标 CPU 上调度并 flush work，
	 * 因而同样允许睡眠。失败只告警。
	 */
	err = tmigr_isolated_exclude_cpumask(isol_mask);
	WARN_ON_ONCE(err < 0);

	/*
	 * 最后重算内核线程的首选/回退亲和性，使没有可用首选 CPU 的 kthread 落入
	 * 新 housekeeping 集合。该步骤可能分配临时 cpumask 并调用调度亲和性接口。
	 */
	err = kthreads_update_housekeeping();
	WARN_ON_ONCE(err < 0);

	/*
	 * 所有需要看到新策略的同步传播步骤已结束，且 RCU grace period 已经过；
	 * old 的生命周期闭环到此结束。首次创建 DOMAIN 时 old 为 NULL，kfree(NULL)
	 * 是安全空操作。
	 */
	kfree(old);

	return 0;
}

/*
 * housekeeping_init() - 完成启动期 housekeeping 策略的正式初始化。
 *
 * 调用关系：start_kernel() 在调度器基础设施建立、workqueue 早期初始化之前调用。
 * 入参：无。入口时 boot command-line 的 __setup handler 已解析 nohz_full=/
 * isolcpus=，flags 指示已启用类型，各 cpumasks 仍由早期 memblock 分配。
 * 上下文：单 CPU 启动阶段，无并发策略写者，可使用 GFP_KERNEL；函数标记 __init，
 * 初始化完成后其代码可释放，运行期不得再调用。
 *
 * 返回：无直接返回值。未配置隔离时无副作用；已配置时启用 static key，必要时
 * 初始化 full-nohz tick offload，并把每张已配置掩码从 memblock 迁移到 slab。
 * 若某次 kmalloc 异常失败则 WARN 后提前返回，已迁移槽位保持有效，后续槽位仍为
 * memblock 存储；正常内核启动通常不会走该退化路径。
 */
void __init housekeeping_init(void)
{
	/* type 是当前迁移的 hk_type 位号，仅在遍历 flags 的启动阶段有效。 */
	enum hk_type type;

	/* 没有命令行配置时保留 static-key false 快路径，也无需迁移任何掩码。 */
	if (!housekeeping.flags)
		return;

	// 开启静态分支
	// 通知内核"housekeeping 模式已激活"。后续 housekeeping_cpu() 等函数通过这个 static key 判断是否需要做 CPU 过滤，未激活时直接返回 true（所有 CPU 均可）。
	/*
	 * 这是查询路径开始服从 cpumask 的全局开关。此时所有已置位类型的早期掩码
	 * 均已由参数解析器完整建立，故启用后不会暴露半初始化槽位。
	 */
	static_branch_enable(&housekeeping_overridden);

	// 初始化 tick offload（若启用 nohz_full）
	// nohz_full CPU 上关闭了周期性 tick，调度时钟的职责需要 offload 到 housekeeping CPU 上代为处理。
	/*
	 * 仅 DOMAIN/managed_irq 隔离不停止调度 tick；只有 KERNEL_NOISE 意味着
	 * full-nohz CPU 需要远端 tick_work 存储。helper 分配 per-CPU tick 工作对象，
	 * 后续调度器才可替隔离 CPU 执行必要的远端调度 tick。
	 */
	if (housekeeping.flags & HK_FLAG_KERNEL_NOISE)
		sched_tick_offload_init();
	/*
	将 cpumask 从 memblock 迁移到 slab
	 	每种隔离类型（调度域、中断、内核噪声等）都有一张 housekeeping CPU 的 cpumask。
		这些 cpumask 在早期由 memblock_alloc 分配（slab 还未就绪）。
		此时 slab 已可用，将它们迁移到 kmalloc 分配的内存，
		使后续 kfree 可以直接释放旧版本（内核动态更新 cpumask 时需要 kfree 旧的）
	 * Realloc with a proper allocator so that any cpumask update
	 * can indifferently free the old version with kfree().
	 */
	/*
	 * 使用常规分配器重新分配，这样任何 cpumask 更新都能不区分来源地用 kfree()
	 * 释放旧版本。启动参数解析时 slab 尚不可用，只能用 memblock；迁移后统一
	 * ownership 规则，尤其方便运行期 DOMAIN 的 RCU 换表回收。
	 */
	for_each_set_bit(type, &housekeeping.flags, HK_TYPE_MAX) {
		/*
		 * 变量地图：
		 *   omask 当前槽位的 memblock 旧掩码，借用到复制完成；
		 *   nmask 新的 slab 掩码，分配成功后将由全局槽位持有。
		 * 每轮只迁移一个已启用类型，未置位槽位从不解引用。
		 */
		struct cpumask *omask, *nmask = kmalloc(cpumask_size(), GFP_KERNEL);

		/*
		 * 这里无法把错误返回给 start_kernel()；告警并停止后续迁移，避免对
		 * NULL 写入。已完成的槽位不能回滚，因为 static key 已经发布策略。
		 */
		if (WARN_ON_ONCE(!nmask))
			return;

		/*
		 * 启动阶段没有并发 writer/reader 争用；rcu_dereference() 仍保留该
		 * __rcu 指针的正规读取语义。omask 的 ownership 仍属于 memblock。
		 */
		omask = rcu_dereference(housekeeping.cpumasks[type]);

		/* We need at least one CPU to handle housekeeping work */
		/*
		 * 至少需要一个 CPU 承担 housekeeping 工作。解析器已强制 boot CPU
		 * 留在集合中；此 WARN 验证该关键不变量，而不尝试在这里重新选择 CPU。
		 */
		WARN_ON_ONCE(cpumask_empty(omask));
		/*
		 * 先完整复制，再用 RCU_INIT_POINTER() 替换。启动期尚无并发读者，
		 * 不需要 grace period；初始化宏也不承担运行期 publish barrier 的角色。
		 */
		cpumask_copy(nmask, omask);
		RCU_INIT_POINTER(housekeeping.cpumasks[type], nmask);
		/*
		 * 指针切换后旧 memblock 区域不再被引用，按精确 cpumask_size() 归还。
		 * 新 nmask 从此由 housekeeping 槽位持有，动态 DOMAIN 更新时可 kfree。
		 */
		memblock_free(omask, cpumask_size());
	}
}

/*
 * housekeeping_setup_type() - 为一个 hk_type 固化启动期 housekeeping 掩码。
 *
 * 调用关系：housekeeping_setup() 在首次配置或补充新类型时调用。
 * @type：目标类型索引，纯输入，调用前相应 flags 位尚未发布。
 * @housekeeping_staging：解析结果的借用 cpumask，纯输入，调用期间稳定；本函数
 *                       复制内容，不保存该临时对象，也不转移其所有权。
 * 上下文：boot command-line __setup 阶段，slab 未必可用，不存在并发读者。
 * 返回：无直接返回值；成功后 cpumasks[type] 指向一张由 memblock 持有的新快照。
 * 分配失败由 memblock_alloc_or_panic() 终止启动，不存在可恢复的部分成功结果。
 */
static void __init housekeeping_setup_type(enum hk_type type,
					   cpumask_var_t housekeeping_staging)
{
	/*
	 * mask 在分配后由本函数独占；按 SMP_CACHE_BYTES 对齐可满足 cpumask 存取，
	 * panic 语义合理，因为缺少掩码将使已请求的 CPU 隔离无法安全兑现。
	 */
	struct cpumask *mask = memblock_alloc_or_panic(cpumask_size(), SMP_CACHE_BYTES);

	/*
	 * 先形成不可变快照，再初始化 RCU 槽位。flags 位由上层在所有相关类型建立后
	 * 才统一置位，所以外部不会观察到只有指针、没有有效策略标志的中间状态。
	 */
	cpumask_copy(mask, housekeeping_staging);
	RCU_INIT_POINTER(housekeeping.cpumasks[type], mask);
}

/*
 * housekeeping_setup() - 解析一条隔离 CPU 列表并合并到启动策略。
 *
 * 调用关系：nohz_full= 和 isolcpus= 的专用 __setup handler 在启动命令行解析期
 * 调用；完成后 housekeeping_init() 接管这些早期掩码。
 * @str：NUL 结尾的 CPU list（如 "2-7,10"）借用字符串；cpulist_parse() 读取但
 *       本函数不保存、不释放，错误消息也只在调用期间引用。
 * @flags：本条参数希望启用的 HK_FLAG_* 位集合，纯输入；位必须对应 hk_type。
 * 上下文：单线程启动阶段，可使用 bootmem/memblock 接口，不存在运行期并发。
 *
 * 返回：1 表示参数有效且非空策略已经合入全局状态，供 __setup 框架认定参数已
 * 处理；0 表示功能不支持、列表非法、结果没有隔离 CPU 或与先前参数冲突。
 * 失败路径释放本次临时 bootmem cpumask；此前已由另一参数提交的全局策略不回滚。
 *
 * 核心不变量：housekeeping_staging 是 non_housekeeping_mask 相对
 * cpu_possible_mask 的补集；至少一个 present 且允许启动的 CPU 必须同时保留为
 * housekeeper；nohz_full 与 isolcpus 中重叠的类型必须给出相同 CPU 列表。
 */
static int __init housekeeping_setup(char *str, unsigned long flags)
{
	/*
	 * 变量地图：
	 *   non_housekeeping_mask 用户明确隔离、应减少杂务的 CPU 集合；
	 *   housekeeping_staging 其相对 possible CPU 的补集，提交前的临时快照；
	 *   first_cpu             交集搜索结果，nr_cpu_ids 及以上表示未找到；
	 *   err                   __setup 返回状态，默认 0，完整提交后改为 1。
	 */
	cpumask_var_t non_housekeeping_mask, housekeeping_staging;
	unsigned int first_cpu;
	int err = 0;

	/*
	 * 阶段 1：先验证配置能力。只有本次首次引入 KERNEL_NOISE 时才需要检查，
	 * 因为重复参数稍后还要做一致性校验。未编译 NO_HZ_FULL 时不能兑现关闭 tick
	 * 的语义，打印告警并让该参数不产生全局副作用。
	 */
	if ((flags & HK_FLAG_KERNEL_NOISE) && !(housekeeping.flags & HK_FLAG_KERNEL_NOISE)) {
		if (!IS_ENABLED(CONFIG_NO_HZ_FULL)) {
			pr_warn("Housekeeping: nohz unsupported."
				" Build with CONFIG_NO_HZ_FULL\n");
			return 0;
		}
	}

	/*
	 * 阶段 2：解析用户给出的“非 housekeeper”集合。bootmem cpumask 的存储只在
	 * 本函数有效，所有出口都经 cleanup 标签释放；解析失败时尚未修改全局状态。
	 */
	alloc_bootmem_cpumask_var(&non_housekeeping_mask);
	if (cpulist_parse(str, non_housekeeping_mask) < 0) {
		pr_warn("Housekeeping: nohz_full= or isolcpus= incorrect CPU range\n");
		goto free_non_housekeeping_mask;
	}

	alloc_bootmem_cpumask_var(&housekeeping_staging);
	/*
	 * CPU 隔离参数描述需要避开的 CPU，本模块查询接口需要返回可承载杂务的 CPU，
	 * 因而在 possible CPU 范围内取补集。不存在的 CPU 不会进入任一有效策略。
	 */
	cpumask_andnot(housekeeping_staging,
		       cpu_possible_mask, non_housekeeping_mask);

	/*
	 * 阶段 3：确保至少一个 present 且不会被 setup_max_cpus 截掉的 housekeeper。
	 * present 表示物理上存在，setup_max_cpus 则反映 maxcpus= 等启动限制；只看
	 * possible 可能选到本次启动永远不会上线的 CPU。
	 */
	first_cpu = cpumask_first_and(cpu_present_mask, housekeeping_staging);
	if (first_cpu >= nr_cpu_ids || first_cpu >= setup_max_cpus) {
		/*
		 * 强制保留当前 boot CPU，并从隔离集合删除它。boot CPU 此刻正在执行
		 * 初始化，必然可承接早期杂务；若完全隔离它，定时器/工作队列可能无处运行。
		 */
		__cpumask_set_cpu(smp_processor_id(), housekeeping_staging);
		__cpumask_clear_cpu(smp_processor_id(), non_housekeeping_mask);
		if (!housekeeping.flags) {
			pr_warn("Housekeeping: must include one present CPU, "
				"using boot CPU:%d\n", smp_processor_id());
		}
	}

	/*
	 * 修正后若非 housekeeping 集合为空，等价于没有隔离任何 CPU。不要为这种
	 * 空策略启用 static key 或建立类型掩码，保持普通系统的零开销路径。
	 */
	if (cpumask_empty(non_housekeeping_mask))
		goto free_housekeeping_staging;

	/*
	 * 阶段 4A：第一条隔离参数。所有 flags 指定类型都复制同一个 staging 快照；
	 * 例如 isolcpus=domain 同时建立固定 DOMAIN_BOOT 和初始 DOMAIN。
	 */
	if (!housekeeping.flags) {
		/* First setup call ("nohz_full=" or "isolcpus=") */
		/*
		 * 第一次设置调用（来自 nohz_full= 或 isolcpus=）。type 遍历本次 flags
		 * 的置位类型，每个槽位都得到独立快照，允许以后只替换 DOMAIN。
		 */
		enum hk_type type;

		for_each_set_bit(type, &flags, HK_TYPE_MAX)
			housekeeping_setup_type(type, housekeeping_staging);
	} else {
		/* Second setup call ("nohz_full=" after "isolcpus=" or the reverse) */
		/*
		 * 第二次设置调用（isolcpus= 之后出现 nohz_full=，或顺序相反）。
		 * type 是遍历索引；iter_flags 先表示两次配置重叠的类型，随后复用于
		 * DOMAIN/KERNEL_NOISE 组合检查和“本次新增类型”集合。
		 */
		enum hk_type type;
		unsigned long iter_flags = flags & housekeeping.flags;

		/*
		 * 阶段 4B-1：同一类型若被两个参数重复描述，CPU 列表必须完全一致。
		 * 否则同一 hk_type 无法同时满足两个来源的语义；拒绝本次参数比静默取
		 * 交集/并集更可预测，也不触碰第一次已提交的槽位。
		 */
		for_each_set_bit(type, &iter_flags, HK_TYPE_MAX) {
			if (!cpumask_equal(housekeeping_staging,
					   housekeeping_cpumask(type))) {
				pr_warn("Housekeeping: nohz_full= must match isolcpus=\n");
				goto free_housekeeping_staging;
			}
		}

		/*
		 * Check the combination of nohz_full and isolcpus=domain,
		 * necessary to avoid problems with the timer migration
		 * hierarchy. managed_irq is ignored by this check since it
		 * isn't considered in the timer migration logic.
		 */
		/*
		 * 检查 nohz_full 与 isolcpus=domain 的组合：timer migration 层次需要
		 * 至少一个既不在 nohz_full、也不在 domain 隔离集合中的在线候选 CPU，
		 * 否则无法为隔离 CPU 之外的定时器建立可靠迁移目标。managed_irq 不参与
		 * timer migration，所以故意不纳入此检查。
		 */
		iter_flags = housekeeping.flags & (HK_FLAG_KERNEL_NOISE | HK_FLAG_DOMAIN);
		/*
		 * 从先前策略中选一张相关掩码作交集基准。因为同一类多次配置已经在上面
		 * 验证相等，一张代表性旧掩码足以判断是否存在共同 housekeeper。
		 */
		type = find_first_bit(&iter_flags, HK_TYPE_MAX);
		/*
		 * Pass the check if none of these flags were previously set or
		 * are not in the current selection.
		 */
		/*
		 * 若以前没有设置这两类 flag，或本次根本没选择它们，则直接通过检查；
		 * 否则在 present CPU、当前 staging 和旧相关掩码三者交集中找共同 CPU。
		 */
		iter_flags = flags & (HK_FLAG_KERNEL_NOISE | HK_FLAG_DOMAIN);
		first_cpu = (type == HK_TYPE_MAX || !iter_flags) ? 0 :
			    cpumask_first_and_and(cpu_present_mask,
						  housekeeping_staging, housekeeping_cpumask(type));
		/*
		 * setup_max_cpus 再次排除本次启动不会上线的候选。冲突时整条当前参数被
		 * 忽略，避免发布一个会破坏 timer migration 不变量的组合策略。
		 */
		if (first_cpu >= min(nr_cpu_ids, setup_max_cpus)) {
			pr_warn("Housekeeping: must include one present CPU "
				"neither in nohz_full= nor in isolcpus=domain, "
				"ignoring setting %s\n", str);
			goto free_housekeeping_staging;
		}

		/*
		 * 阶段 4B-2：只为此前尚未配置的类型建立新槽位。重叠类型已验证相等，
		 * 无需替换旧 memblock 对象；这也避免无意义的早期内存泄漏。
		 */
		iter_flags = flags & ~housekeeping.flags;

		for_each_set_bit(type, &iter_flags, HK_TYPE_MAX)
			housekeeping_setup_type(type, housekeeping_staging);
	}

	/*
	 * 阶段 5：首次引入 KERNEL_NOISE 时，把用户的隔离集合交给 tick 子系统。
	 * tick_nohz_full_setup() 会复制该集合，因此随后释放临时 mask 不会悬空。
	 * 必须在 flags 最终发布前完成，使看到 KERNEL_NOISE 已启用的消费者也能假设
	 * full-nohz 基础状态已经就绪。
	 */
	if ((flags & HK_FLAG_KERNEL_NOISE) && !(housekeeping.flags & HK_FLAG_KERNEL_NOISE))
		tick_nohz_full_setup(non_housekeeping_mask);

	/*
	 * 提交点：相关 cpumask 槽位和 tick 状态都已建立，最后统一发布 flags。
	 * 启动阶段没有并发读者，普通 OR 足够；运行期读取侧仍用 READ_ONCE。
	 */
	housekeeping.flags |= flags;
	err = 1;

free_housekeeping_staging:
	/*
	 * cleanup 1：无论提交或冲突，staging 都只是本次调用的 bootmem 临时对象；
	 * 全局槽位已经各自复制内容，释放它不会影响已提交策略。
	 */
	free_bootmem_cpumask_var(housekeeping_staging);
free_non_housekeeping_mask:
	/*
	 * cleanup 2：解析输入掩码生命周期结束。若在 cpulist_parse 后失败，
	 * 这里只释放它；若经过 staging，则从上一标签自然落入，形成逆序回收。
	 */
	free_bootmem_cpumask_var(non_housekeeping_mask);

	return err;
}

/*
 * housekeeping_nohz_full_setup() - 把 nohz_full= 参数映射为内核噪声隔离。
 *
 * 调用关系：nohz_full= 的 early boot 参数处理器调用本函数；它是薄适配层，
 * 真正的 CPU list 校验、补集构造和策略合并由 housekeeping_setup() 完成。
 * @str：nohz_full= 后的 NUL 结尾 CPU list 借用字符串，可为空或非法；不保存。
 * 上下文：启动期、可使用 __init 数据；函数及其调用目标在 init 后释放。
 * 返回：原样返回 housekeeping_setup() 的 1（已处理并提交）或 0（忽略/失败）。
 * 副作用：成功时建立 HK_TYPE_KERNEL_NOISE 掩码并初始化 tick nohz-full 配置。
 */
static int __init housekeeping_nohz_full_setup(char *str)
{
	/*
	 * flags 只有 KERNEL_NOISE 位：头文件中的 TICK/TIMER/RCU/MISC/WQ 都是该
	 * hk_type 的别名，因此一次配置同时为这些可卸载杂务提供共同 housekeeper。
	 */
	unsigned long flags;

	flags = HK_FLAG_KERNEL_NOISE;

	return housekeeping_setup(str, flags);
}
__setup("nohz_full=", housekeeping_nohz_full_setup);

/*
 * housekeeping_isolcpus_setup() - 解析 isolcpus= 子参数并建立相应隔离类型。
 *
 * 调用关系：isolcpus= 的 early boot 参数处理器调用本函数；识别可选前缀
 * nohz, / domain, / managed_irq,，再把剩余 CPU list 交给 housekeeping_setup()。
 * @str：isolcpus= 后的可读 NUL 结尾字符串借用指针；函数会推进本地指针但不
 *       修改字符内容、不保存或释放底层命令行缓冲区。
 * 上下文：单线程启动期，不睡眠要求由下层 boot 参数基础设施决定。
 *
 * 返回：合法已提交返回 housekeeping_setup() 的 1；非法 flag 字符直接返回 0；
 * 未知但由字母/下划线组成的 flag 被告警后跳过，以便新内核参数在旧内核上具有
 * 一定前向兼容性。没有任何 flag 时采用历史默认 DOMAIN|DOMAIN_BOOT。
 */
static int __init housekeeping_isolcpus_setup(char *str)
{
	/*
	 * 变量地图：
	 *   flags   已识别子参数对应的 HK_FLAG 位集合；
	 *   illegal 当前未知 token 是否含非字母/下划线字符；
	 *   par     未知 token 的起始借用指针，仅用于日志；
	 *   len     未知 token 长度（字符数），供 %.*s 安全打印。
	 */
	unsigned long flags = 0;
	bool illegal = false;
	char *par;
	int len;

	/*
	 * 阶段 1：只要当前位置以字母开头，就把逗号前内容当作 flag token，而不是
	 * CPU list。合法 CPU list 通常以数字开头；循环结束时 str 正好指向列表。
	 */
	while (isalpha(*str)) {
		/*
		 * isolcpus=nohz is equivalent to nohz_full.
		 */
		/*
		 * isolcpus=nohz 等价于 nohz_full。共享 KERNEL_NOISE 位意味着 tick、
		 * timer、RCU、workqueue 等噪声使用同一张 housekeeping 补集。
		 */
		if (!strncmp(str, "nohz,", 5)) {
			str += 5;
			flags |= HK_FLAG_KERNEL_NOISE;
			continue;
		}

		/*
		 * domain 同时设置固定的 DOMAIN_BOOT 和可被 cpuset isolated partition
		 * 进一步收紧的 DOMAIN。保留前者才能让运行期更新永远不突破启动隔离边界。
		 */
		if (!strncmp(str, "domain,", 7)) {
			str += 7;
			flags |= HK_FLAG_DOMAIN | HK_FLAG_DOMAIN_BOOT;
			continue;
		}

		/*
		 * managed_irq 只约束 managed interrupt 的自动亲和性选择，不参与
		 * timer migration 的共同 housekeeper 检查。
		 */
		if (!strncmp(str, "managed_irq,", 12)) {
			str += 12;
			flags |= HK_FLAG_MANAGED_IRQ;
			continue;
		}

		/*
		 * Skip unknown sub-parameter and validate that it is not
		 * containing an invalid character.
		 */
		/*
		 * 跳过未知子参数，同时验证其中不含非法字符。允许字母和下划线可兼容
		 * 将来的命名 flag；数字、连字符等出现在这里更可能表示 flag/CPU list
		 * 分隔写错，不能继续按合法 CPU 范围解析。
		 */
		for (par = str, len = 0; *str && *str != ','; str++, len++) {
			if (!isalpha(*str) && *str != '_')
				illegal = true;
		}

		/*
		 * 非法 token 使整条 isolcpus 参数失败，且尚未调用通用 setup，所以没有
		 * 分配临时掩码或修改全局策略。%.*s 用已扫描长度限制日志读取范围。
		 */
		if (illegal) {
			pr_warn("isolcpus: Invalid flag %.*s\n", len, par);
			return 0;
		}

		/*
		 * 合法但未知 token 只记录并忽略。循环末尾 str 位于逗号或 NUL；
		 * 自增跨过逗号，使下一轮检查下一个 flag 或最终 CPU list。
		 */
		pr_info("isolcpus: Skipped unknown flag %.*s\n", len, par);
		str++;
	}

	/* Default behaviour for isolcpus without flags */
	/*
	 * isolcpus 不带 flag 时的默认行为：沿用传统语义，把所列 CPU 排除在调度域
	 * 负载均衡之外，同时记录固定 DOMAIN_BOOT 边界供未来 cpuset 动态更新使用。
	 */
	if (!flags)
		flags |= HK_FLAG_DOMAIN | HK_FLAG_DOMAIN_BOOT;

	/*
	 * 阶段 2：str 此时指向 CPU list，flags 已完成语义归一化。通用 helper 负责
	 * 检查空集合、保留 boot CPU、处理与 nohz_full 的顺序和一致性。
	 */
	return housekeeping_setup(str, flags);
}
__setup("isolcpus=", housekeeping_isolcpus_setup);
