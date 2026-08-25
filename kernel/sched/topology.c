// SPDX-License-Identifier: GPL-2.0
/*
 * Scheduler topology setup/handling methods
 */
/*
 * 本文件把体系结构提供的 SMT/cluster/LLC/package/NUMA mask 构造成每 CPU 的
 * sched_domain 父链、环形 sched_group 及共享 capacity，并把同一负载均衡分区
 * 的 CPU 连接到 root_domain。构建在 hotplug lock+sched_domains_mutex 下完成，
 * 新指针用 RCU 挂接；旧 domain/root/perf-domain 摘除后延迟到 grace period 释放。
 *
 * 构建分阶段分配所有 per-CPU 对象，任一步失败按 s_alloc 状态逆序回滚；group
 * span 必须不重叠并完整覆盖 domain span（NUMA overlap 例外），child span 必须是
 * parent 子集。capacity、asym/LLC/NUMA per-CPU cache 都是派生索引，只有在完整组装
 * 后才能发布，CPU hotplug/partition 重建时必须同步撤销，避免 reader 看到半成品。
 */

#include <linux/sched/isolation.h>
#include <linux/sched/clock.h>
#include <linux/bsearch.h>
#include "sched.h"

DEFINE_MUTEX(sched_domains_mutex);
/* 获取可睡眠的全局 topology 重建互斥锁。 */
/*
 * 业务背景：外部 hotplug/partition 控制路径需与所有 sched_domain 重建事务串行。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；当前线程取得 sched_domains_mutex。
 * 注意事项：可睡眠且不可递归；调用者必须与 sched_domains_mutex_unlock 严格配对。
 */
void sched_domains_mutex_lock(void)
{
	mutex_lock(&sched_domains_mutex);
}
/* 释放 topology 重建互斥锁；此前发布的 RCU 指针仍由 grace period 保护。 */
/*
 * 业务背景：完整 topology 更新提交后需允许下一次重建进入，同时旧对象继续由 RCU 延迟回收。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；释放当前线程持有的 sched_domains_mutex。
 * 注意事项：必须由持锁线程调用；解锁不等于 RCU grace period 已结束。
 */
void sched_domains_mutex_unlock(void)
{
	mutex_unlock(&sched_domains_mutex);
}

/* Protected by sched_domains_mutex: */
static cpumask_var_t sched_domains_llc_id_allocmask;
static cpumask_var_t sched_domains_tmpmask;
static cpumask_var_t sched_domains_tmpmask2;
int max_lid;

/* early 参数 sched_verbose 开启 domain 结构校验输出，成功消费参数。 */
/*
 * 业务背景：启动参数需在 topology 初次构建前打开详细结构校验和诊断打印。
 * 入参：str 是 early_param 传入但实现未使用的可空借用字符串。
 * 出参/返回：恒返回 0表示参数已处理；无输出参数，设置全局 verbose 状态。
 * 注意事项：仅启动早期单次语义；无法通过参数值关闭，运行期同步不适用。
 */
static int __init sched_debug_setup(char *str)
{
	sched_debug_verbose = true;

	return 0;
}
early_param("sched_verbose", sched_debug_setup);

/* 返回只读 verbose 开关。 */
/*
 * 业务背景：EAS/topology 控制路径需要统一查询是否输出昂贵的结构诊断。
 * 入参：无。
 * 出参/返回：返回当前 sched_debug_verbose 布尔值；无输出参数和 ownership 变化。
 * 注意事项：无锁快照只控制日志，不可用作结构同步条件。
 */
static inline bool sched_debug(void)
{
	return sched_debug_verbose;
}

#define SD_FLAG(_name, mflags) [__##_name] = { .meta_flags = mflags, .name = #_name },
const struct sd_flag_debug sd_flag_debug[] = {
#include <linux/sched/sd_flags.h>
};
#undef SD_FLAG

/* 校验并打印单层 domain 的 span、flags、环形 groups 与 parent/child 包含关系。 */
/*
 * 业务背景：verbose 构建审计要验证一层 domain 的 CPU 覆盖、flag 传播和环形 group 不变量。
 * 入参：sd/groupmask 不可空借用，groupmask 为可写 scratch；cpu/level 是目标 CPU 和缩进层级。
 * 出参/返回：恒返回 0；覆盖 groupmask 为 groups 并集并输出诊断，不转移 ownership。
 * 注意事项：仅构建/挂接控制路径调用；可打印错误但不修复结构，NUMA group 允许 span 重叠。
 */
static int sched_domain_debug_one(struct sched_domain *sd, int cpu, int level,
				  struct cpumask *groupmask)
{
	struct sched_group *group = sd->groups;
	unsigned long flags = sd->flags;
	unsigned int idx;

	/* scratch mask 从空集开始，后面累加每个 group span 检查完整覆盖。 */
	cpumask_clear(groupmask);

	/* 先输出层级和 span，让后续任何不变量错误都有定位上下文。 */
	printk(KERN_DEBUG "%*s domain-%d: ", level, "", level);
	printk(KERN_CONT "span=%*pbl level=%s\n",
	       cpumask_pr_args(sched_domain_span(sd)), sd->name);

	/* 本 CPU 必须同时属于 domain span 和当前起始 group。 */
	if (!cpumask_test_cpu(cpu, sched_domain_span(sd))) {
		printk(KERN_ERR "ERROR: domain->span does not contain CPU%d\n", cpu);
	}
	if (group && !cpumask_test_cpu(cpu, sched_group_span(group))) {
		printk(KERN_ERR "ERROR: domain->groups does not contain CPU%d\n", cpu);
	}

	/* 逐 flag 核对需向 child 或 parent 传播的元属性约束。 */
	for_each_set_bit(idx, &flags, __SD_FLAG_CNT) {
		unsigned int flag = BIT(idx);
		unsigned int meta_flags = sd_flag_debug[idx].meta_flags;

		if ((meta_flags & SDF_SHARED_CHILD) && sd->child &&
		    !(sd->child->flags & flag))
			printk(KERN_ERR "ERROR: flag %s set here but not in child\n",
			       sd_flag_debug[idx].name);

		/* parent 共享约束与上面的 child 约束对称，只报告不修补。 */
		if ((meta_flags & SDF_SHARED_PARENT) && sd->parent &&
		    !(sd->parent->flags & flag))
			printk(KERN_ERR "ERROR: flag %s set here but not in parent\n",
			       sd_flag_debug[idx].name);
	}

	/* groups 是环链；从 sd->groups 出发并在再次回到链首时结束。 */
	printk(KERN_DEBUG "%*s groups:", level + 1, "");
	do {
		if (!group) {
			printk("\n");
			printk(KERN_ERR "ERROR: group is NULL\n");
			break;
		}

		/* 空 span 无法代表任何均衡目标，结构已不可继续安全走读。 */
		if (cpumask_empty(sched_group_span(group))) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: empty group\n");
			break;
		}

		/* 非 NUMA group 必须互斥；NUMA 距离层的 group 则允许重叠。 */
		if (!(sd->flags & SD_NUMA) &&
		    cpumask_intersects(groupmask, sched_group_span(group))) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: repeated CPUs\n");
			break;
		}

		/* 累加已访问 CPU，最后将与整个 domain span 做等值校验。 */
		cpumask_or(groupmask, groupmask, sched_group_span(group));

		printk(KERN_CONT " %d:{ span=%*pbl",
				group->sgc->id,
				cpumask_pr_args(sched_group_span(group)));

		/* NUMA 层的 balance mask 可是 span 子集，不同时额外打印以便诊断。 */
		if ((sd->flags & SD_NUMA) &&
		    !cpumask_equal(group_balance_mask(group), sched_group_span(group))) {
			printk(KERN_CONT " mask=%*pbl",
				cpumask_pr_args(group_balance_mask(group)));
		}

		/* 只显示非默认容量，避免 verbose 输出被常量噪声淹没。 */
		if (group->sgc->capacity != SCHED_CAPACITY_SCALE)
			printk(KERN_CONT " cap=%lu", group->sgc->capacity);

		/* 环链首 group 应与当前 CPU 的 child span 对应，否则层级映射错位。 */
		if (group == sd->groups && sd->child &&
		    !cpumask_equal(sched_domain_span(sd->child),
				   sched_group_span(group))) {
			printk(KERN_ERR "ERROR: domain->groups does not match domain->child\n");
		}

		/* 完成当前 group 的人类可读输出后再前进环链。 */
		printk(KERN_CONT " }");

		group = group->next;

		if (group != sd->groups)
			printk(KERN_CONT ",");

	} while (group != sd->groups);
	printk(KERN_CONT "\n");

	/* 并集必须恰好覆盖 domain，且 parent span 必须是它的超集。 */
	if (!cpumask_equal(sched_domain_span(sd), groupmask))
		printk(KERN_ERR "ERROR: groups don't span domain->span\n");

	if (sd->parent &&
	    !cpumask_subset(groupmask, sched_domain_span(sd->parent)))
		printk(KERN_ERR "ERROR: parent span is not a superset of domain->span\n");
	return 0;
}

/* verbose 模式沿 @cpu domain 父链逐层执行结构校验；NULL 表示解绑。 */
/*
 * 业务背景：每个 CPU 发布新 domain 链前后需要可选地输出整条父链并定位结构不变量破坏。
 * 入参：sd 是可空借用链首，NULL 表示解绑；cpu 是有效目标 CPU。
 * 出参/返回：无直接返回值、无输出参数；verbose 开启时打印并审计各层。
 * 注意事项：借用全局 tmpmask，调用者须持 topology 串行锁；诊断失败不阻止挂接。
 */
static void sched_domain_debug(struct sched_domain *sd, int cpu)
{
	int level = 0;

	if (!sched_debug_verbose)
		return;

	/* NULL 链首表示 CPU 正在解除 domain，记录后无需进入父链。 */
	if (!sd) {
		printk(KERN_DEBUG "CPU%d attaching NULL sched-domain.\n", cpu);
		return;
	}

	printk(KERN_DEBUG "CPU%d attaching sched-domain(s):\n", cpu);

	/* 每层共用全局 scratch mask，所以整个遍历必须保持串行。 */
	for (;;) {
		/* 本层校验失败或到达父链顶端都结束诊断走读。 */
		if (sched_domain_debug_one(sd, cpu, level, sched_domains_tmpmask))
			break;
		level++;
		sd = sd->parent;
		if (!sd)
			break;
	}
}

/* 通过重新展开 sd_flags.h，把所有依赖多 group 的 flag 编译成常量 mask。 */
/* Generate a mask of SD flags with the SDF_NEEDS_GROUPS metaflag */
#define SD_FLAG(name, mflags) (name * !!((mflags) & SDF_NEEDS_GROUPS)) |
static const unsigned int SD_DEGENERATE_GROUPS_MASK =
#include <linux/sched/sd_flags.h>
0;
#undef SD_FLAG

/* 单 CPU 或没有任何有效多组功能的 domain 可退化删除。 */
/*
 * 业务背景：构建后要删除不提供负载均衡能力的冗余 domain，缩短每 CPU 遍历链。
 * 入参：sd 是不可空、尚未发布或由构建锁稳定的只读借用 domain。
 * 出参/返回：可安全退化返回 1，否则 0；无输出参数和 ownership 变化。
 * 注意事项：要求 groups 环已初始化；WAKE_AFFINE 即使无多组也使 domain 保留。
 */
static int sd_degenerate(struct sched_domain *sd)
{
	if (cpumask_weight(sched_domain_span(sd)) == 1)
		return 1;

	/* 需要 group 的功能只在环中真有多组时构成保留理由。 */
	/* Following flags need at least 2 groups */
	if ((sd->flags & SD_DEGENERATE_GROUPS_MASK) &&
	    (sd->groups != sd->groups->next))
		return 0;

	/* Following flags don't use groups */
	if (sd->flags & (SD_WAKE_AFFINE))
		return 0;

	return 1;
}

/* parent span/flags 不增加有效能力时可与 child 合并，返回 true。 */
/*
 * 业务背景：相邻 domain 层覆盖相同 CPU 且 parent 没有新增有效 flag 时可折叠父层。
 * 入参：sd/parent 是不可空、构建期稳定的只读借用 child 与 parent。
 * 出参/返回：parent 可删除返回 1，否则 0；无输出参数和 ownership 变化。
 * 注意事项：单 group parent 的 NEEDS_GROUPS flags 会先忽略；函数不执行链表修改或释放。
 */
static int
sd_parent_degenerate(struct sched_domain *sd, struct sched_domain *parent)
{
	unsigned long cflags = sd->flags, pflags = parent->flags;

	if (sd_degenerate(parent))
		return 1;

	/* span 不同说明 parent 扩大了均衡范围，无论 flags 如何都不能折叠。 */
	if (!cpumask_equal(sched_domain_span(sd), sched_domain_span(parent)))
		return 0;

	/* Flags needing groups don't count if only 1 group in parent */
	if (parent->groups == parent->groups->next)
		pflags &= ~SD_DEGENERATE_GROUPS_MASK;

	/* parent 留下任一 child 没有的有效 flag，就代表它仍提供新能力。 */
	if (~cflags & pflags)
		return 0;

	return 1;
}

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)
DEFINE_STATIC_KEY_FALSE(sched_energy_present);
static unsigned int sysctl_sched_energy_aware = 1;
static DEFINE_MUTEX(sched_energy_mutex);
static bool sched_energy_update;

/* 校验非对称容量、无 SMT、频率不变性、cpufreq/EM 准备情况，判断 EAS 可用性。 */
/*
 * 业务背景：EAS 只能在容量非对称且能可靠比较能耗/利用率的 root-domain CPU 集上启用。
 * 入参：cpu_mask 是不可空只读借用候选 CPU 集合。
 * 出参/返回：全部前置条件成立返回 true，否则 false；无输出参数和状态修改。
 * 注意事项：读取 RCU 发布的 asym domain 与动态 cpufreq 状态；失败可按 verbose 打印具体原因。
 */
static bool sched_is_eas_possible(const struct cpumask *cpu_mask)
{
	bool any_asym_capacity = false;
	int i;

	/* EAS 只有在至少一个 CPU 已发布容量非对称 domain 时才有决策空间。 */
	/* EAS is enabled for asymmetric CPU capacity topologies. */
	for_each_cpu(i, cpu_mask) {
		if (rcu_access_pointer(per_cpu(sd_asym_cpucapacity, i))) {
			any_asym_capacity = true;
			break;
		}
	}
	/* verbose 诊断仅解释拒绝原因，不改变能力判定结果。 */
	if (!any_asym_capacity) {
		if (sched_debug()) {
			pr_info("rd %*pbl: Checking EAS, CPUs do not have asymmetric capacities\n",
				cpumask_pr_args(cpu_mask));
		}
		return false;
	}

	/* SMT sibling 共享实际算力，会破坏当前 EAS 能耗/容量模型的独立性假设。 */
	/* EAS definitely does *not* handle SMT */
	if (sched_smt_active()) {
		if (sched_debug()) {
			pr_info("rd %*pbl: Checking EAS, SMT is not supported\n",
				cpumask_pr_args(cpu_mask));
		}
		return false;
	}

	/* 若利用率未做频率不变归一化，不同 CPU 的能耗比较不可靠。 */
	if (!arch_scale_freq_invariant()) {
		if (sched_debug()) {
			pr_info("rd %*pbl: Checking EAS: frequency-invariant load tracking not yet supported",
				cpumask_pr_args(cpu_mask));
		}
		return false;
	}

	/* cpufreq policy 和 energy model 必须已为整个候选 mask 就绪。 */
	if (!cpufreq_ready_for_eas(cpu_mask)) {
		if (sched_debug()) {
			pr_info("rd %*pbl: Checking EAS: cpufreq is not ready\n",
				cpumask_pr_args(cpu_mask));
		}
		return false;
	}

	return true;
}

/* 在 energy mutex 下强制重建 perf domains，避免并发 sysctl/hotplug 交叉发布。 */
/*
 * 业务背景：EM/cpufreq 条件改变时必须重建 sched domains 才能重新计算并发布 perf_domain 链。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；在 energy mutex 下临时置 update 标志并触发全局重建。
 * 注意事项：可睡眠且可能执行昂贵重建；不得在持 rq 锁或 energy mutex 递归上下文调用。
 */
void rebuild_sched_domains_energy(void)
{
	/* mutex 把 sysctl、EM 更新与热插拔触发的重建串行化。 */
	mutex_lock(&sched_energy_mutex);
	sched_energy_update = true;
	rebuild_sched_domains();
	sched_energy_update = false;
	mutex_unlock(&sched_energy_mutex);
}

#ifdef CONFIG_PROC_SYSCTL
/* CAP_SYS_ADMIN 控制 EAS sysctl；写后重建 domains，当前拓扑不支持则拒绝。 */
/*
 * 业务背景：用户通过 sysctl 查询或切换 EAS 时需做权限、能力条件和 domain 重建协调。
 * 入参：table/buffer/lenp/ppos 遵循 proc handler 借用契约；write 表示读写方向，lenp/ppos 为输入输出。
 * 出参/返回：成功返回 0并更新缓冲/位置；权限 -EPERM，不支持 -EOPNOTSUPP，解析返回对应 errno。
 * 注意事项：写路径可睡眠并重建全局 topology；不支持的读将 *lenp 清零而不报错。
 */
static int sched_energy_aware_handler(const struct ctl_table *table, int write,
		void *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	/* 写入会改变全局调度策略，所以比普通 sysctl 解析先做管理权限门禁。 */
	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	/* 当前拓扑不支持 EAS 时，读返空、写显式拒绝，避免伪成功。 */
	if (!sched_is_eas_possible(cpu_active_mask)) {
		if (write) {
			return -EOPNOTSUPP;
		} else {
			*lenp = 0;
			return 0;
		}
	}

	/* 标准 handler 负责 [0,1] 范围、用户缓冲区和文件位置更新。 */
	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (!ret && write) {
		/* 只有实际目标值改变时才触发昂贵的全局 domain 重建。 */
		if (sysctl_sched_energy_aware != sched_energy_enabled())
			rebuild_sched_domains_energy();
	}

	return ret;
}

/* 表项限定值域为 0/1，并将读写统一转发到上面的能力门禁。 */
static const struct ctl_table sched_energy_aware_sysctls[] = {
	{
		.procname       = "sched_energy_aware",
		.data           = &sysctl_sched_energy_aware,
		.maxlen         = sizeof(unsigned int),
		.mode           = 0644,
		/* handler 统一执行权限、EAS 能力与重建检查，extra1/2 约束布尔值域。 */
		.proc_handler   = sched_energy_aware_handler,
		.extra1         = SYSCTL_ZERO,
		.extra2         = SYSCTL_ONE,
	},
};

/* 启动期注册 EAS sysctl。 */
/*
 * 业务背景：EAS 配置需要在 late init 阶段发布 /proc/sys/kernel/sched_energy_aware。
 * 入参：无。
 * 出参/返回：恒返回 0；尝试注册静态 sysctl 表，无输出参数。
 * 注意事项：仅 CONFIG_PROC_SYSCTL 启动期；当前实现不传播注册失败，表生命周期为永久。
 */
static int __init sched_energy_aware_sysctl_init(void)
{
	/* 静态表与内核同寿命，init helper 只需将它挂到 kernel sysctl 目录。 */
	register_sysctl_init("kernel", sched_energy_aware_sysctls);
	return 0;
}

late_initcall(sched_energy_aware_sysctl_init);
#endif /* CONFIG_PROC_SYSCTL */

/* 释放一条尚未 RCU 发布或 grace period 已结束的 perf_domain 链。 */
/*
 * 业务背景：EAS 构建失败或旧 root-domain 退出后需回收按 next 串联的 perf_domain 对象。
 * 入参：pd 是可空、调用者拥有的链首；NULL 为安全空操作。
 * 出参/返回：无直接返回值、无输出参数；释放整链并终结 ownership。
 * 注意事项：不等待 RCU；调用者必须保证链未发布或 grace period 已结束，释放后所有节点失效。
 */
static void free_pd(struct perf_domain *pd)
{
	struct perf_domain *tmp;

	/* 先保存 next 再释放当前节点，避免从已释放内存取链接。 */
	while (pd) {
		tmp = pd->next;
		kfree(pd);
		pd = tmp;
	}
}

/* 在 perf_domain 链中查覆盖 @cpu 的节点；返回借用指针或 NULL。 */
/*
 * 业务背景：构建 EAS 链时要去重已覆盖同一 CPU 的 EM performance domain。
 * 入参：pd 是可空只读借用链首；cpu 是有效 CPU 编号。
 * 出参/返回：返回首个 span 含 cpu 的借用节点，未找到返回 NULL；无输出参数。
 * 注意事项：不取得引用；调用者须稳定整链生命周期，CPU 范围由上层保证。
 */
static struct perf_domain *find_pd(struct perf_domain *pd, int cpu)
{
	/* span 中的任一 CPU 都唯一标识同一 EM performance domain。 */
	while (pd) {
		if (cpumask_test_cpu(cpu, perf_domain_span(pd)))
			return pd;
		pd = pd->next;
	}

	return NULL;
}

/* 从 energy model 为 @cpu perf domain 分配 span 节点并取得 em_pd 引用。 */
/*
 * 业务背景：EAS root-domain 链需要为每个未覆盖 EM performance domain 建立内核调度包装节点。
 * 入参：cpu 是有效且属于当前构建分区的 CPU 编号。
 * 出参/返回：成功返回调用者拥有的新 perf_domain；无 EM或分配失败返回 NULL；无输出参数。
 * 注意事项：返回对象引用由 EM 生命周期支持并须经 free_pd 回收；NULL 不区分缺 EM 与 -ENOMEM。
 */
static struct perf_domain *pd_init(int cpu)
{
	struct em_perf_domain *obj = em_cpu_get(cpu);
	struct perf_domain *pd;

	/* EM 缺失表示该 CPU 无法参与 EAS，verbose 时记录具体 CPU。 */
	if (!obj) {
		if (sched_debug())
			pr_info("%s: no EM found for CPU%d\n", __func__, cpu);
		return NULL;
	}

	/* 包装节点初始为零，next 稍后由链表构建者设置。 */
	pd = kzalloc_obj(*pd);
	if (!pd)
		return NULL;
	pd->em_pd = obj;

	return pd;
}

/* verbose 输出新 perf-domain 链及其 CPU span。 */
/*
 * 业务背景：EAS 发布前需可选输出 root-domain CPU 集与每个 EM domain 的状态数，便于核对覆盖。
 * 入参：cpu_map 不可空只读借用；pd 是可空只读借用链首。
 * 出参/返回：无直接返回值、无输出参数；verbose 且 pd 非空时打印链内容。
 * 注意事项：不加锁、不取得引用；调用者必须稳定链与 EM 对象，日志可能较长。
 */
static void perf_domain_debug(const struct cpumask *cpu_map,
						struct perf_domain *pd)
{
	if (!sched_debug() || !pd)
		return;

	/* 先打印 root-domain mask，后续每个 pd span 才有归属上下文。 */
	printk(KERN_DEBUG "root_domain %*pbl:", cpumask_pr_args(cpu_map));

	/* 链表节点只被读取，输出首 CPU、span 和 EM 性能状态数。 */
	while (pd) {
		printk(KERN_CONT " pd%d:{ cpus=%*pbl nr_pstate=%d }",
				cpumask_first(perf_domain_span(pd)),
				cpumask_pr_args(perf_domain_span(pd)),
				em_pd_nr_perf_states(pd->em_pd));
		pd = pd->next;
	}

	printk(KERN_CONT "\n");
}

/* RCU 回调释放旧 root_domain 的 perf-domain 链并归还 energy-model 引用。 */
/*
 * 业务背景：rd->pd 被新链替换后，旧链必须等所有 RCU 读者退出再释放。
 * 入参：rp 是不可空、内嵌于旧链首节点的 RCU 回调对象，ownership 已交给 RCU。
 * 出参/返回：无直接返回值、无输出参数；回收旧 perf_domain 整链。
 * 注意事项：RCU callback 上下文不可睡眠；container_of 要求 rp 确为链首内嵌成员。
 */
static void destroy_perf_domain_rcu(struct rcu_head *rp)
{
	struct perf_domain *pd;

	pd = container_of(rp, struct perf_domain, rcu);
	/* callback 触发即证明旧链已越过 grace period，可同步释放全链。 */
	free_pd(pd);
}

/*
 * 业务背景：所有分区重建完成后要让全局 EAS static key 与是否至少存在可用 perf-domain 保持一致。
 * 入参：has_eas 是重建结果汇总布尔值，纯输入。
 * 出参/返回：无直接返回值、无输出参数；按需启用或关闭 sched_energy_present。
 * 注意事项：调用者持 CPU hotplug 锁并使用 cpuslocked API；重复设置为空操作。
 */
static void sched_energy_set(bool has_eas)
{
	/* 只在目标值与 static key 现值不同时修改，避免多余 jump-label patch。 */
	if (!has_eas && sched_energy_enabled()) {
		if (sched_debug())
			pr_info("%s: stopping EAS\n", __func__);
		static_branch_disable_cpuslocked(&sched_energy_present);
	} else if (has_eas && !sched_energy_enabled()) {
		if (sched_debug())
			pr_info("%s: starting EAS\n", __func__);
		static_branch_enable_cpuslocked(&sched_energy_present);
	}
}

/*
 * EAS can be used on a root domain if it meets all the following conditions:
 *    1. an Energy Model (EM) is available;
 *    2. the SD_ASYM_CPUCAPACITY flag is set in the sched_domain hierarchy.
 *    3. no SMT is detected.
 *    4. schedutil is driving the frequency of all CPUs of the rd;
 *    5. frequency invariance support is present;
 */
/* 为分区构建去重 perf-domain 链并 RCU 替换 rd->pd；失败不发布半链。 */
/*
 * 业务背景：每个 root-domain 要把 CPU 集映射为不重复的 EM domains，供 EAS 在唤醒/负载均衡路径读取。
 * 入参：cpu_map 是不可空、非空且拓扑稳定的只读分区 CPU mask。
 * 出参/返回：发布非空链返回 true；禁用、条件不满足或构建失败返回 false并清 rd->pd。
 * 注意事项：调用者持重建锁；新链先本地完成再 RCU 发布，旧链 call_rcu，失败不返回具体 errno。
 */
static bool build_perf_domains(const struct cpumask *cpu_map)
{
	int i;
	struct perf_domain *pd = NULL, *tmp;
	int cpu = cpumask_first(cpu_map);
	struct root_domain *rd = cpu_rq(cpu)->rd;

	/* 用户关闭 EAS 或平台条件不满足时，统一走 free 路径撤销旧发布链。 */
	if (!sysctl_sched_energy_aware)
		goto free;

	if (!sched_is_eas_possible(cpu_map))
		goto free;

	/* 一个 EM domain 可覆盖多个 CPU，find_pd() 保证每个 span 只建一个节点。 */
	for_each_cpu(i, cpu_map) {
		/* Skip already covered CPUs. */
		if (find_pd(pd, i))
			continue;

		/* 新节点先挂到本地链首，整链成功前对 RCU 读者不可见。 */
		/* Create the new pd and add it to the local list. */
		tmp = pd_init(i);
		if (!tmp)
			goto free;
		tmp->next = pd;
		pd = tmp;
	}

	/* 发布前诊断可看到完整新链，不会与旧链混杂。 */
	perf_domain_debug(cpu_map, pd);

	/* Attach the new list of performance domains to the root domain. */
	/* RCU 替换后新读者看新链，旧链的实际销毁交给 callback。 */
	tmp = rd->pd;
	rcu_assign_pointer(rd->pd, pd);
	if (tmp)
		call_rcu(&tmp->rcu, destroy_perf_domain_rcu);

	return !!pd;

free:
	/* 未发布的半成品可立即释放；旧 rd->pd 仍需先摘除再延迟销毁。 */
	free_pd(pd);
	tmp = rd->pd;
	rcu_assign_pointer(rd->pd, NULL);
	if (tmp)
		call_rcu(&tmp->rcu, destroy_perf_domain_rcu);

	return false;
}
#else /* !(CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL): */
/*
 * 业务背景：未同时启用 EM 与 schedutil 时保留 root-domain 通用清理调用接口。
 * 入参：pd 是实现未使用的可空借用指针。
 * 出参/返回：无直接返回值、无输出参数，也不释放对象。
 * 注意事项：仅禁用配置 stub；该构建不会创建真实 perf_domain 链。
 */
static void free_pd(struct perf_domain *pd) { }
#endif /* !(CONFIG_ENERGY_MODEL && CONFIG_CPU_FREQ_GOV_SCHEDUTIL) */

/* root_domain 最后引用归零后的 RCU 回调，释放 cpumask/cpupri/DL/perf-domain。 */
/*
 * 业务背景：动态 root-domain 从所有 rq/异步用户解绑后要在 RCU grace period 后统一销毁其共享索引。
 * 入参：rcu 是不可空、内嵌于待销毁 root_domain 的回调对象，ownership 已交给 RCU。
 * 出参/返回：无直接返回值、无输出参数；清理 cpupri/cpudl/masks/pd并释放 rd。
 * 注意事项：RCU callback 不可睡眠；def_root_domain 保留永久引用，不应进入本回调。
 */
static void free_rootdomain(struct rcu_head *rcu)
{
	struct root_domain *rd = container_of(rcu, struct root_domain, rcu);

	/* RT/DL 搜索结构可内含动态内存，必须在释放 masks 和 rd 前先清理。 */
	cpupri_cleanup(&rd->cpupri);
	cpudl_cleanup(&rd->cpudl);
	free_cpumask_var(rd->dlo_mask);
	free_cpumask_var(rd->rto_mask);
	/* online/span 是 root-domain 最后的 CPU 归属快照，grace period 后已无读者。 */
	free_cpumask_var(rd->online);
	free_cpumask_var(rd->span);
	free_pd(rd->pd);
	kfree(rd);
}

/*
 * rq 锁下从旧 root_domain 摘 @rq 并加入 @rd；更新 online/active span、RT/DL
 * 索引与引用，旧 rd 最后引用通过 RCU 延迟释放。
 */
/*
 * 业务背景：分区重建要把单个 rq 从旧 root-domain 原子迁到新 rd，并同步 RT/DL/SCX server 归属。
 * 入参：rq 是不可空输入/输出队列；rd 是不可空、已初始化且由调用者借出的目标 root-domain。
 * 出参/返回：无直接返回值、无输出参数；更新 rq->rd、两边 masks/refcount及 server bandwidth。
 * 注意事项：内部 irqsave 获取 rq 锁；最后旧引用在解锁后 call_rcu，目标 rd ownership 由 rq 引用持有。
 */
void rq_attach_root(struct rq *rq, struct root_domain *rd)
{
	struct root_domain *old_rd = NULL;
	struct rq_flags rf;

	/* rq 锁和 IRQ 关闭保证 rd 指针、span 与 server 归属作为一个提交单元。 */
	rq_lock_irqsave(rq, &rf);

	/* 若 rq 已属于旧 rd，先撤销 online 语义再从 span 移除。 */
	if (rq->rd) {
		old_rd = rq->rd;

		if (cpumask_test_cpu(rq->cpu, old_rd->online))
			set_rq_offline(rq);

		/* span bit 清除后，新的 RT/DL 选择不再把该 rq 当作旧分区成员。 */
		cpumask_clear_cpu(rq->cpu, old_rd->span);

		/*
		 * If we don't want to free the old_rd yet then
		 * set old_rd to NULL to skip the freeing later
		 * in this function:
		 */
		/* 只有减到零的旧 rd 才保留在 old_rd，供解锁后安排 RCU 销毁。 */
		if (!atomic_dec_and_test(&old_rd->refcount))
			old_rd = NULL;
	}

	/* 先给目标 rd 加引用再发布 rq->rd，防止可见对象没有生命期保护。 */
	atomic_inc(&rd->refcount);
	rq->rd = rd;

	/* span 表示归属，online 只对当前 active CPU 设置并触发各类上线处理。 */
	cpumask_set_cpu(rq->cpu, rd->span);
	if (cpumask_test_cpu(rq->cpu, cpu_active_mask))
		set_rq_online(rq);

	/*
	 * Because the rq is not a task, dl_add_task_root_domain() did not
	 * move the fair server bw to the rd if it already started.
	 * Add it now.
	 */
	/* 已启动的 fair deadline server 不是 task，不会由通用 task 迁移路径自动更换 rd。 */
	if (rq->fair_server.dl_server)
		__dl_server_attach_root(&rq->fair_server, rq);

#ifdef CONFIG_SCHED_CLASS_EXT
	if (rq->ext_server.dl_server)
		__dl_server_attach_root(&rq->ext_server, rq);
#endif

	/* 先完成新 rd 的全部可见状态，再恢复 IRQ 并处理旧对象。 */
	rq_unlock_irqrestore(rq, &rf);

	if (old_rd)
		call_rcu(&old_rd->rcu, free_rootdomain);
}

/* 增加 root_domain 引用，允许异步 push/work 跨越 rq 挂接变化。 */
/*
 * 业务背景：异步调度工作在离开 rq 锁后仍要稳定 root-domain 生命周期。
 * 入参：rd 是不可空、仍存活的输入/输出 root-domain 借用指针。
 * 出参/返回：无直接返回值、无输出参数；原子增加引用计数。
 * 注意事项：调用者必须已有一个有效引用或其他生命周期保护，不能复活已归零对象。
 */
void sched_get_rd(struct root_domain *rd)
{
	/* 原子计数仅管生命期，不保护 rd 内部可变字段的一致性。 */
	atomic_inc(&rd->refcount);
}

/* 释放 root_domain 引用；最后一个引用用 call_rcu 延迟销毁。 */
/*
 * 业务背景：异步用户或 rq 结束借用时需归还 root-domain 引用，并保护仍在 RCU 读侧的访问者。
 * 入参：rd 是不可空、调用者持有一份引用的输入/输出对象。
 * 出参/返回：无直接返回值、无输出参数；减少引用，归零时把对象 ownership 交给 RCU 回调。
 * 注意事项：每次 get/初始引用仅 put 一次；过量 put 会下溢或释放仍在使用的对象。
 */
void sched_put_rd(struct root_domain *rd)
{
	/* 非最后引用仅减计数；最后一份引用不能直接 kfree，因为可能存在 RCU 读者。 */
	if (!atomic_dec_and_test(&rd->refcount))
		return;

	call_rcu(&rd->rcu, free_rootdomain);
}

/* 分配并初始化 rd span/online/dloverload/cpupri/cpudl 等共享索引；失败逆序回滚。 */
/*
 * 业务背景：新 root-domain 在挂接 rq 前必须完整拥有 CPU masks、RT/DL 搜索索引与 bandwidth 状态。
 * 入参：rd 是不可空、零初始化且由调用者独占的输入/输出对象。
 * 出参/返回：成功返回 0；任一分配/索引初始化失败返回 -ENOMEM并回滚本函数已建资源。
 * 注意事项：GFP_KERNEL 可睡眠；成功不设置 refcount，调用者在发布前负责建立初始引用。
 */
static int init_rootdomain(struct root_domain *rd)
{
	/* 四个 mask 按 span、online、DL overload、RT overload 顺序分配，便于标签逆序回滚。 */
	if (!zalloc_cpumask_var(&rd->span, GFP_KERNEL))
		goto out;
	if (!zalloc_cpumask_var(&rd->online, GFP_KERNEL))
		goto free_span;
	if (!zalloc_cpumask_var(&rd->dlo_mask, GFP_KERNEL))
		goto free_online;
	if (!zalloc_cpumask_var(&rd->rto_mask, GFP_KERNEL))
		goto free_dlo_mask;

#ifdef HAVE_RT_PUSH_IPI
	/* RT push IPI 状态在对象发布前初始化，硬 irq_work 不得看到半成品锁。 */
	rd->rto_cpu = -1;
	raw_spin_lock_init(&rd->rto_lock);
	rd->rto_push_work = IRQ_WORK_INIT_HARD(rto_push_irq_work_func);
#endif

	/* 通用遍历 cookie 和 DL bandwidth 先就绪，再创建可失败的 cpudl/cpupri 索引。 */
	rd->visit_cookie = 0;
	init_dl_bw(&rd->dl_bw);
	if (cpudl_init(&rd->cpudl) != 0)
		goto free_rto_mask;

	/* cpupri 失败需额外回收已完成的 cpudl，其他标签再回收 masks。 */
	if (cpupri_init(&rd->cpupri) != 0)
		goto free_cpudl;
	return 0;

free_cpudl:
	/* 标签严格逆着成功初始化顺序回收，每个资源只被处理一次。 */
	cpudl_cleanup(&rd->cpudl);
free_rto_mask:
	free_cpumask_var(rd->rto_mask);
free_dlo_mask:
	free_cpumask_var(rd->dlo_mask);
	/* 后两个标签依次回收 online/span，最终所有失败都归一为 -ENOMEM。 */
free_online:
	free_cpumask_var(rd->online);
free_span:
	free_cpumask_var(rd->span);
out:
	return -ENOMEM;
}

/*
 * By default the system creates a single root-domain with all CPUs as
 * members (mimicking the global state we have today).
 */
struct root_domain def_root_domain;

/* 启动期初始化永久 fallback root_domain；失败属于不可恢复启动错误。 */
/*
 * 业务背景：系统必须始终有一个全局 fallback root-domain，供初始 rq 和分区重建失败路径使用。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；初始化 def_root_domain并建立永久引用 1。
 * 注意事项：仅启动期；当前不检查 init 失败，依赖早期内存分配可用且对象永不释放。
 */
void __init init_defrootdomain(void)
{
	/* fallback 对象的初始化失败无可用的上层恢复路径，因此启动期假定必然成功。 */
	init_rootdomain(&def_root_domain);

	atomic_set(&def_root_domain.refcount, 1);
}

/* 分配并初始化动态 root_domain，失败返回 NULL。 */
/*
 * 业务背景：每个独立 sched-domain 分区需要一个新的 root-domain 承载共享 overload/bandwidth 索引。
 * 入参：无。
 * 出参/返回：成功返回调用者拥有且尚无初始引用的 root-domain；分配/初始化失败返回 NULL。
 * 注意事项：GFP_KERNEL 可睡眠；调用者必须随后挂接 rq 建引用或清理未发布对象。
 */
static struct root_domain *alloc_rootdomain(void)
{
	struct root_domain *rd;

	/* 容器先零填充，使 init_rootdomain() 的失败标签可安全识别已建资源。 */
	rd = kzalloc_obj(*rd);
	if (!rd)
		return NULL;

	/* 子资源由 init_rootdomain() 自行回滚，此处只需释放外层容器。 */
	if (init_rootdomain(rd) != 0) {
		kfree(rd);
		return NULL;
	}

	return rd;
}

/* 按环形链引用计数释放 sched_group，按 @free_sgc 决定是否同时释放 capacity。 */
/*
 * 业务背景：domain 折叠/销毁时要遍历环形 groups 并按共享引用释放 group 与 capacity 对象。
 * 入参：sg 是可空环首借用/待释放指针；free_sgc 指示是否归还每组 capacity 引用。
 * 出参/返回：无直接返回值、无输出参数；减少环中引用并释放归零对象。
 * 注意事项：环必须闭合且 next 在释放前读取；错误 free_sgc 会泄漏或过早释放共享 sgc。
 */
static void free_sched_groups(struct sched_group *sg, int free_sgc)
{
	struct sched_group *tmp, *first;

	/* NULL 表示该 domain 没有已建 group，常见于部分分配失败回滚。 */
	if (!sg)
		return;

	/* 保存环首作为结束哨兵，每次释放前先取 next。 */
	first = sg;
	do {
		tmp = sg->next;

		/* capacity 可跨 group 共享，仅当本层拥有引用且减到零时回收。 */
		if (free_sgc && atomic_dec_and_test(&sg->sgc->ref))
			kfree(sg->sgc);

		if (atomic_dec_and_test(&sg->ref))
			kfree(sg);
		sg = tmp;
	} while (sg != first);
}

/* shared 引用归零时释放对象，否则仅减引用。 */
/*
 * 业务背景：相邻/同层 sched domains 可共享 domain-wide 状态，销毁时需引用计数回收。
 * 入参：sds 是可空、调用者持有一份引用的输入/输出 shared 对象。
 * 出参/返回：无直接返回值、无输出参数；减少引用并在归零时释放。
 * 注意事项：不使用 RCU；调用者保证对象已从所有可见 domain 摘除且引用配对正确。
 */
static void free_sched_domain_shared(struct sched_domain_shared *sds)
{
	if (sds && atomic_dec_and_test(&sds->ref))
		kfree(sds);
}

/* 释放单层 domain 的 groups/shared 与自身；调用前已从 RCU reader 摘除。 */
/*
 * 业务背景：退化层或旧 domain 链在安全期结束后需释放该层拥有的 groups/shared 和 domain 本体。
 * 入参：sd 是不可空、调用者拥有且已不可达的单层 domain。
 * 出参/返回：无直接返回值、无输出参数；释放该层全部 ownership。
 * 注意事项：不递归父子链；调用者先断链并满足 RCU 条件，groups 引用关系必须已正确建立。
 */
static void destroy_sched_domain(struct sched_domain *sd)
{
	/*
	 * A normal sched domain may have multiple group references, an
	 * overlapping domain, having private groups, only one.  Iterate,
	 * dropping group/capacity references, freeing where none remain.
	 */
	/* groups/capacity 和 shared 都可被其他 domain 引用，因此通过各自计数决定实际释放。 */
	free_sched_groups(sd->groups, 1);
	free_sched_domain_shared(sd->shared);

#ifdef CONFIG_SCHED_CACHE
	/* llc_counts 只属于底层 domain，折叠时若已转移给 parent 就会被置 NULL。 */
	/* only the bottom sd has llc_counts array */
	kfree(sd->llc_counts);
#endif
	kfree(sd);
}

/* grace period 后沿 parent 链销毁旧 domain。 */
/*
 * 业务背景：CPU rq 发布新 domain 链后，旧链需等待所有无锁调度读者退出再逐层回收。
 * 入参：rcu 是不可空、内嵌于旧链首 sched_domain 的 RCU 回调对象。
 * 出参/返回：无直接返回值、无输出参数；沿 parent 链销毁所有层。
 * 注意事项：RCU callback 不可睡眠；链必须已与当前 rq 断开且 parent 指针稳定。
 */
static void destroy_sched_domains_rcu(struct rcu_head *rcu)
{
	struct sched_domain *sd = container_of(rcu, struct sched_domain, rcu);

	/* 每次先保存 parent，因为 destroy_sched_domain() 会立即使当前 sd 失效。 */
	while (sd) {
		struct sched_domain *parent = sd->parent;
		destroy_sched_domain(sd);
		sd = parent;
	}
}

/* 将旧 domain 链挂到 RCU callback，立即返回给重建路径。 */
/*
 * 业务背景：挂接新 topology 后不能同步释放旧链，否则仍在 RCU 读侧的调度路径会悬空。
 * 入参：sd 是可空、调用者交出 ownership 的旧链首；NULL 为安全空操作。
 * 出参/返回：无直接返回值、无输出参数；非空时安排 RCU 延迟销毁。
 * 注意事项：调用后不得再直接访问或释放 sd；函数本身不等待 grace period。
 */
static void destroy_sched_domains(struct sched_domain *sd)
{
	/* NULL 旧链常见于 CPU 首次接入，无需安排空 callback。 */
	if (sd)
		call_rcu(&sd->rcu, destroy_sched_domains_rcu);
}

/*
 * Keep a special pointer to the highest sched_domain that has SD_SHARE_LLC set
 * (Last Level Cache Domain) for this allows us to avoid some pointer chasing
 * select_idle_sibling().
 *
 * Also keep a unique ID per domain (we use the first CPU number in the cpumask
 * of the domain), this allows us to quickly tell if two CPUs are in the same
 * cache domain, see cpus_share_cache().
 */
/* 这组 per-CPU 快捷指针由 domain 提交路径成组刷新，调度热路径按 RCU 借用。 */
DEFINE_PER_CPU(struct sched_domain __rcu *, sd_llc);
DEFINE_PER_CPU(int, sd_llc_size);
DEFINE_PER_CPU(int, sd_llc_id) = -1;
DEFINE_PER_CPU(int, sd_share_id);
DEFINE_PER_CPU(struct sched_domain_shared __rcu *, sd_llc_shared);
DEFINE_PER_CPU(struct sched_domain_shared __rcu *, sd_balance_shared);
DEFINE_PER_CPU(struct sched_domain __rcu *, sd_numa);
DEFINE_PER_CPU(struct sched_domain __rcu *, sd_asym_packing);
DEFINE_PER_CPU(struct sched_domain __rcu *, sd_asym_cpucapacity);

/* static key 汇总全局是否存在异构容量或 cluster，让无此特性的机器裁掉分支成本。 */
DEFINE_STATIC_KEY_FALSE(sched_asym_cpucapacity);
DEFINE_STATIC_KEY_FALSE(sched_cluster_active);

/* 从新 domain 链派生并 RCU 发布 LLC/NUMA/asym per-CPU 快捷指针和 id。 */
/*
 * 业务背景：唤醒和负载均衡热路径需要免遍历访问 CPU 的 LLC、NUMA、cluster 和非对称容量层。
 * 入参：cpu 是有效且新 sched_domain 链已发布/构建稳定的 CPU 编号。
 * 出参/返回：无直接返回值、无输出参数；覆盖该 CPU 的快捷 RCU 指针、size 和共享 id。
 * 注意事项：调用者持 hotplug/topology 锁；读者依赖 RCU，sds 选择须遵守 overlap ownership。
 */
static void update_top_cache_domain(int cpu)
{
	struct sched_domain_shared *sds = NULL;
	struct sched_domain *sd;
	int id = cpu;
	int size = 1;

	/* LLC 缺失时保留单 CPU 默认值；存在时以 span 首 CPU 作稳定共享 id。 */
	sd = highest_flag_domain(cpu, SD_SHARE_LLC);
	if (sd) {
		id = cpumask_first(sched_domain_span(sd));
		size = cpumask_weight(sched_domain_span(sd));

		/* LLC domain 应在构建阶段认领 shared，WARN 用于暴露该内部不变量破坏。 */
		/* If sd_llc exists, sd_llc_shared should exist too. */
		WARN_ON_ONCE(!sd->shared);
		sds = sd->shared;
	}

	/* 先 RCU 发布 domain/shared 快捷指针，size 是与它们同次重建的标量快照。 */
	rcu_assign_pointer(per_cpu(sd_llc, cpu), sd);
	per_cpu(sd_llc_size, cpu) = size;
	rcu_assign_pointer(per_cpu(sd_llc_shared, cpu), sds);

	/* cluster 机器使 share_id 表示更内层 cluster，否则沿用 LLC id。 */
	sd = lowest_flag_domain(cpu, SD_CLUSTER);
	if (sd)
		id = cpumask_first(sched_domain_span(sd));

	/*
	 * This assignment should be placed after the sd_llc_id as
	 * we want this id equals to cluster id on cluster machines
	 * but equals to LLC id on non-Cluster machines.
	 */
	per_cpu(sd_share_id, cpu) = id;

	/* NUMA 使用最低距离层，asym packing 使用最高可见层。 */
	sd = lowest_flag_domain(cpu, SD_NUMA);
	rcu_assign_pointer(per_cpu(sd_numa, cpu), sd);

	sd = highest_flag_domain(cpu, SD_ASYM_PACKING);
	rcu_assign_pointer(per_cpu(sd_asym_packing, cpu), sd);

	/* FULL 容量层是跨全部容量等级做选择的最内层快捷入口。 */
	sd = lowest_flag_domain(cpu, SD_ASYM_CPUCAPACITY_FULL);
	/*
	 * The shared object is attached to sd_asym_cpucapacity only when the
	 * asym domain is non-overlapping (i.e., not built from SD_NUMA).
	 * On overlapping (NUMA) asym domains we fall back to letting the
	 * SD_SHARE_LLC path own the shared object, so sd->shared may be NULL
	 * here.
	 */
	/* 非重叠 asym domain 自带 shared；否则继续沿用上面的 LLC shared 作 balance 统计。 */
	if (sd && sd->shared)
		sds = sd->shared;

	rcu_assign_pointer(per_cpu(sd_asym_cpucapacity, cpu), sd);
	rcu_assign_pointer(per_cpu(sd_balance_shared, cpu), sds);
}

/*
 * Attach the domain 'sd' to 'cpu' as its base domain. Callers must
 * hold the hotplug lock.
 */
/*
 * 删除退化层、修正 parent/child 后先挂 root_domain，再 RCU 发布 @cpu 的 sd；
 * 旧链延迟释放，最后刷新 per-CPU topology cache。
 */
/*
 * 业务背景：单 CPU topology 提交前要折叠无效层、迁移共享状态，并原子更换 rq 的 root/domain 指针。
 * 入参：sd 是可空且调用者拥有的新链；rd 是不可空目标 root-domain；cpu 是有效目标 CPU。
 * 出参/返回：无直接返回值、无输出参数；发布新链并把旧链 ownership 交给 RCU。
 * 注意事项：调用者持 CPU hotplug 锁；内部 rq 锁挂 root，折叠时必须维护 parent/child/group flags。
 */
static void
cpu_attach_domain(struct sched_domain *sd, struct root_domain *rd, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct sched_domain *tmp;

	/* 先在未发布链上从底向上删除不增加调度能力的 parent。 */
	/* Remove the sched domains which do not contribute to scheduling. */
	for (tmp = sd; tmp; ) {
		struct sched_domain *parent = tmp->parent;
		if (!parent)
			break;

		/* 折叠 parent 时先跨过它重连链，再转移它拥有的共享状态。 */
		if (sd_parent_degenerate(tmp, parent)) {
			tmp->parent = parent->parent;

			/* Pick reference to parent->shared. */
			/* shared 转给 child 后必须清 parent 指针，避免销毁 parent 时再次 put。 */
			if (parent->shared) {
				/*
				 * It is safe to free a sd->shared that
				 * has not been published yet. If a
				 * sd->shared was published, the refcount
				 * will end up being non-zero and it will
				 * not be freed here.
				 */
				free_sched_domain_shared(tmp->shared);
				tmp->shared = parent->shared;
				parent->shared = NULL;
			}

			/* 更高一层的 child/group flags 必须同步指向折叠后的真实下层。 */
			if (parent->parent) {
				parent->parent->child = tmp;
				parent->parent->groups->flags = tmp->flags;
			}

			/*
			 * Transfer SD_PREFER_SIBLING down in case of a
			 * degenerate parent; the spans match for this
			 * so the property transfers.
			 */
			/* span 相等时 PREFER_SIBLING 可安全下沉，保留原 parent 的均衡偏好。 */
			if (parent->flags & SD_PREFER_SIBLING)
				tmp->flags |= SD_PREFER_SIBLING;
			destroy_sched_domain(parent);
		} else
			tmp = tmp->parent;
	}

	/* 链首自身退化时不能由上面的 parent 折叠分支处理，需单独摘除。 */
	if (sd && sd_degenerate(sd)) {
		tmp = sd;
		sd = sd->parent;

		/* 若仍有 parent，先转移仅底层拥有的 cache 缓冲并清除 child-derived flags。 */
		if (sd) {
			struct sched_group *sg = sd->groups;

#ifdef CONFIG_SCHED_CACHE
			/* move buffer to parent as child is being destroyed */
			sd->llc_counts = tmp->llc_counts;
			sd->llc_max = tmp->llc_max;
			sd->llc_bytes = tmp->llc_bytes;
			/* make sure destroy_sched_domain() does not free it */
			tmp->llc_counts = NULL;
			tmp->llc_max = 0;
			tmp->llc_bytes = 0;
#endif
			/*
			 * sched groups hold the flags of the child sched
			 * domain for convenience. Clear such flags since
			 * the child is being destroyed.
			 */
			/* 每个 parent group 之前缓存的是即将销毁 child flags，必须整环清零。 */
			do {
				sg->flags = 0;
			} while (sg != sd->groups);

			sd->child = NULL;
		}

		/* 所有可转移资源已摘走，现在才能销毁原链首。 */
		destroy_sched_domain(tmp);
	}

	/* 新链发布前做 verbose 结构审计，诊断不会改变提交结果。 */
	sched_domain_debug(sd, cpu);

	/* root-domain 先挂接，随后 RCU 替换 rq->sd，旧链交给延迟销毁。 */
	rq_attach_root(rq, rd);
	tmp = rq->sd;
	rcu_assign_pointer(rq->sd, sd);
	dirty_sched_domain_sysctl(cpu);
	destroy_sched_domains(tmp);

	update_top_cache_domain(cpu);
}

/*
 * 一次拓扑重建的临时所有权集合：对象先按 CPU 分配，成功接入的条目由
 * claim_allocations() 摘走，失败路径再按 s_alloc 所示阶段逆序释放。
 */
struct s_data {
	struct sched_domain_shared * __percpu *sds;
	struct sched_domain * __percpu *sd;
	struct root_domain	*rd;
};

/* 分配进度同时也是回滚边界，避免半成品 root_domain/sd/group 泄漏。 */
enum s_alloc {
	/* 枚举由完整成功阶段向未分配递减，switch 分支顺序贯穿并据此逆序回滚。 */
	sa_rootdomain,
	sa_sd,
	sa_sd_shared,
	sa_sd_storage,
	sa_none,
};

#ifdef CONFIG_SCHED_CACHE
/* hardware support for cache aware scheduling */
DEFINE_STATIC_KEY_FALSE(sched_cache_present);
/*
 * Indicator of whether cache aware scheduling
 * is active, used by the scheduler.
 */
DEFINE_STATIC_KEY_FALSE(sched_cache_active);
/* user wants cache aware scheduling [0 or 1] */
int sysctl_sched_cache_user = 1;

/*
 * Get the effective LLC size in bytes that @cpu's bottom sched_domain
 * can use. A CPU within a cpuset partition can only use a proportion
 * of the physical LLC, scaled by the ratio of the partition's span
 * weight to the hardware LLC sharing weight. @sd should be the
 * topmost domain with SD_SHARE_LLC.
 *
 * Returns 0 if cacheinfo is not yet populated. This happens during
 * early boot when build_sched_domains() runs before the generic
 * cacheinfo framework has been initialized (cacheinfo_cpu_online()
 * is a device_initcall cpuhp callback). In that case,
 * cacheinfo_cpu_online() will later call sched_update_llc_bytes()
 * to fill in the bottom domain's llc_bytes once the cache attributes
 * are available.
 */
/* 按分区实际覆盖的兄弟比例折算可用 LLC；cacheinfo 未就绪时返回 0。 */
/*
 * 业务背景：cpuset 分区只占物理 LLC 的一部分，cache-aware 调度需按 span 比例估算可用字节数。
 * 入参：cpu 是有效 CPU；sd 是不可空、最顶层 SHARE_LLC domain 的只读借用对象。
 * 出参/返回：返回折算后的 LLC 字节数；cacheinfo/共享权重缺失返回 0；无输出参数。
 * 注意事项：整数除法向下取整；cacheinfo 早期未就绪会由后续 online 回调补写。
 */
static unsigned long get_effective_llc_bytes(int cpu,
					     struct sched_domain *sd)
{
	struct cacheinfo *ci;
	unsigned int hw_weight;

	/* cacheinfo 在早期启动可能尚未发布，0 是等待后续 online 回填的哨兵。 */
	ci = get_cpu_cacheinfo_llc(cpu);
	if (!ci)
		return 0;

	/* 共享 CPU 权重是分区 span 折算的分母，异常为零时禁止除法。 */
	hw_weight = cpumask_weight(&ci->shared_cpu_map);
	if (!hw_weight)
		return 0;

	return div_u64((u64)ci->size * sd->span_weight, hw_weight);
}

/* 为每个底层 domain 建立 LLC 计数存储；任一失败即清空本轮全部结果。 */
/*
 * 业务背景：cache-aware 调度需为分区中每个 CPU 的底层 domain 分配按 logical LLC id 索引的计数数组。
 * 入参：cpu_map 不可空只读分区 mask；d 是不可空、构建期独占的输入/输出分配集合。
 * 出参/返回：全部 CPU 成功返回 true；缺 sd或分配失败返回 false并清理本轮数组。
 * 注意事项：GFP_KERNEL 可睡眠；对象在尚未发布链上，失败回滚不能触碰旧 per-CPU domain。
 */
static bool alloc_sd_llc(const struct cpumask *cpu_map,
			 struct s_data *d)
{
	struct sched_domain *sd, *top_llc, *parent;
	unsigned int *p;
	int i;

	/* 每个 CPU 的底层 domain 单独拥有 llc_counts，数组上界取当前全局 max_lid。 */
	for_each_cpu(i, cpu_map) {
		sd = *per_cpu_ptr(d->sd, i);
		if (!sd)
			goto err;

		/* 按 CPU NUMA node 分配可减少热路径更新计数时的远端访存。 */
		p = kcalloc_node(max_lid + 1, sizeof(unsigned int),
				 GFP_KERNEL, cpu_to_node(i));
		if (!p)
			goto err;

		/* 新链尚未接入 per-CPU cache，只能沿本地 parent 链找到最高 LLC 层。 */
		top_llc = sd;
		/*
		 * Find the topmost SD_SHARE_LLC domain.
		 * Not yet attached to the CPU, so per_cpu(sd_llc, i)
		 * can not be used.
		 */
		while ((parent = rcu_dereference_protected(top_llc->parent, true)) &&
		       (parent->flags & SD_SHARE_LLC))
			top_llc = parent;

		/* 确实存在 LLC domain 才转移 p 的所有权，否则本 CPU 不需计数数组。 */
		if (top_llc->flags & SD_SHARE_LLC) {
			sd->llc_max = max_lid + 1;
			sd->llc_counts = p;
			sd->llc_bytes = get_effective_llc_bytes(i, top_llc);
		} else {
			/* avoid memory leak */
			kfree(p);
		}
	}

	/* 所有 CPU 都完成后才保留本轮数组，以免发布不对称能力。 */
	return true;
err:
	/* 任一 CPU 失败都撤销本分区已建数组，并恢复字段的零初始状态。 */
	for_each_cpu(i, cpu_map) {
		sd = *per_cpu_ptr(d->sd, i);
		if (sd) {
			/* 回收后同时清指针、上界和字节数，防止后续把部分状态误当可用。 */
			kfree(sd->llc_counts);
			sd->llc_counts = NULL;
			sd->llc_max = 0;
			sd->llc_bytes = 0;
		}
	}

	return false;
}

/*
 * Enable/disable cache aware scheduling according to
 * user input and the presence of hardware support.
 */
/* 调用者同时持 CPU 热插拔锁和 domains_mutex，静态键才不会与重建竞态。 */
/*
 * 业务背景：cache-aware 最终启用状态由硬件多 LLC 能力与用户开关共同决定。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；按条件切换 sched_cache_active并可打印原因。
 * 注意事项：必须同时持 CPU hotplug 锁和 sched_domains_mutex；static key 更新成本高但不在热路径。
 */
static void _sched_cache_active_set(void)
{
	lockdep_assert_cpus_held();
	lockdep_assert_held(&sched_domains_mutex);

	/* 没有多 LLC 硬件能力时强制关闭 active，用户开关不能越过这一门禁。 */
	/* hardware does not support */
	if (!static_branch_likely(&sched_cache_present)) {
		static_branch_disable_cpuslocked(&sched_cache_active);
		if (sched_debug())
			pr_info("%s: cache aware scheduling not supported on this platform\n", __func__);
		return;
	}

	/*
	 * user wants it or not ?
	 * TBD: read before writing the static key.
	 * It is not in the critical path, leave as-is
	 * for now.
	 */
	/* 硬件可用后，用户开关才决定最终 static key 状态。 */
	if (sysctl_sched_cache_user) {
		static_branch_enable_cpuslocked(&sched_cache_active);
		if (sched_debug())
			pr_info("%s: enabling cache aware scheduling\n", __func__);
	} else {
		static_branch_disable_cpuslocked(&sched_cache_active);
		if (sched_debug())
			pr_info("%s: disabling cache aware scheduling\n", __func__);
	}
}

/* used by debugfs */
/* debugfs 入口自行取得两层锁，再统一更新 cache-aware 静态键。 */
/*
 * 业务背景：用户修改 cache-aware debugfs/sysctl 状态时需要按统一锁序安全刷新 static key。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；取得 hotplug 读锁和 domains mutex 后更新状态。
 * 注意事项：可睡眠；不得在已持这两把锁的上下文调用，否则可能递归死锁。
 */
void sched_cache_active_set(void)
{
	cpus_read_lock();
	sched_domains_mutex_lock();
	_sched_cache_active_set();
	sched_domains_mutex_unlock();
	cpus_read_unlock();
}

/*
 * Update the bottom sched_domain's llc_bytes for @cpu and all its
 * LLC siblings. Called from cacheinfo_cpu_online() or
 * cacheinfo_cpu_pre_down() with cpu hotplug lock held.
 *
 * Note: get_effective_llc_bytes() returns 0 on PowerPC.
 * thus cache aware scheduling is disabled on PowerPC for
 * now. PowerPC does not use the generic cacheinfo framework --
 * it has its own cacheinfo with a separate struct cache hierarchy
 * and does not populates the per-CPU struct cpu_cacheinfo array
 * that get_cpu_cacheinfo_llc() reads.
 */
/* cacheinfo 变化后重算整个 LLC 兄弟集合，修正 CPU 逐个上线时的临时高估。 */
/*
 * 业务背景：cacheinfo shared map 随 CPU 上线逐步完善，必须回填整个 LLC 中各底层 domain 的有效字节数。
 * 入参：cpu 是有效且 hotplug 保护下正在 online/pre-down 的 CPU 编号。
 * 出参/返回：无直接返回值、无输出参数；更新 LLC span 内可达 domain 的 llc_bytes。
 * 注意事项：调用者持 hotplug 锁，内部可睡眠取得 domains mutex；无 sd_llc 时安全跳过。
 */
void sched_update_llc_bytes(unsigned int cpu)
{
	struct sched_domain *sd, *sdp;
	unsigned int i;

	/* domain mutex 保证重算期间 sd_llc 链不会被另一次拓扑重建替换。 */
	sched_domains_mutex_lock();

	sdp = rcu_dereference_sched_domain(per_cpu(sd_llc, cpu));
	if (!sdp)
		goto unlock;

	/*
	 * ci->shared_cpu_map is built incrementally as CPUs come
	 * online, so the first CPU in an LLC initially sees
	 * hw_weight == 1 and computes an inflated llc_bytes in
	 * get_effective_llc_bytes().  Re-evaluating every LLC
	 * sibling on each online event corrects this once the full
	 * shared_cpu_map is known.
	 */
	/* 必须重算全部 LLC sibling，因为 shared_cpu_map 扩大会同时改变它们的分母。 */
	for_each_cpu(i, sched_domain_span(sdp)) {
		sd = rcu_dereference_sched_domain(cpu_rq(i)->sd);
		if (sd)
			sd->llc_bytes = get_effective_llc_bytes(i, sdp);
	}

unlock:
	sched_domains_mutex_unlock();
}

/* 将重建结果发布为“硬件存在”静态键，并与用户开关合成最终启用状态。 */
/*
 * 业务背景：完整 topology 重建后要发布是否跨多个 LLC，并据此重新计算 cache-aware 运行开关。
 * 入参：has_multi_llcs 是所有新分区是否至少存在多 LLC 的汇总输入。
 * 出参/返回：无直接返回值、无输出参数；切换 present 后刷新 active static key。
 * 注意事项：调用者持 hotplug 和 domains mutex；false 会立即裁掉热路径，即使用户开关仍为 1。
 */
static void sched_cache_set(bool has_multi_llcs)
{
	/*
	 * TBD: check before writing to it. sched domain rebuild
	 * is not in the critical path, leave as-is for now.
	 */
	if (has_multi_llcs)
		static_branch_enable_cpuslocked(&sched_cache_present);
	else
		static_branch_disable_cpuslocked(&sched_cache_present);

	_sched_cache_active_set();
}
#else
/*
 * 业务背景：未启用 SCHED_CACHE 时构建器仍调用统一 LLC storage 分配接口。
 * 入参：cpu_map/d 均为实现未使用的借用输入。
 * 出参/返回：恒返回 false表示无 cache-aware 支持；无输出参数或分配。
 * 注意事项：仅禁用配置 stub；不得据此判断普通 SD_SHARE_LLC topology 不存在。
 */
static bool alloc_sd_llc(const struct cpumask *cpu_map,
			 struct s_data *d)
{
	return false;
}
/*
 * 业务背景：未启用 SCHED_CACHE 时保留统一重建提交调用，编译器将其消除。
 * 入参：has_multi_llcs 是实现未使用的布尔输入。
 * 出参/返回：无直接返回值、无输出参数和状态变化。
 * 注意事项：仅禁用配置 stub；不会创建或切换任何 cache static key。
 */
static inline void sched_cache_set(bool has_multi_llcs) { }
#endif

/*
 * Return true if @sd belongs to an LLC group whose enclosing
 * partition spans more than one LLC. @sd must be the topmost
 * SD_SHARE_LLC domain.
 *
 * Any duplicated parent domains with the same span as @sd are
 * skipped: before cpu_attach_domain() degeneration these still
 * exist, after degeneration the loop is a no-op. This makes the
 * helper usable both during sched domain build and against an
 * already-attached domain tree.
 *
 * Note: For systems with a single LLC per node, cache-aware
 * scheduling is still enabled when multiple nodes exist.
 * However, NUMA balancing decisions take precedence over
 * cache-aware scheduling. Conversely, if there is only one
 * LLC per partition, cache-aware scheduling should be disabled.
 */
/* 跳过同 span 的退化父层，判断分区中是否确实包含多个 LLC。 */
/*
 * 业务背景：cache-aware 调度只在一个分区跨越多个真正 LLC 时有区分价值。
 * 入参：sd 是不可空、最顶层 SHARE_LLC 且构建期稳定的只读借用 domain。
 * 出参/返回：找到不同 span 的上层返回 true，否则 false；无输出参数。
 * 注意事项：会跳过待折叠的同 span parents；span_weight 为 1时强制 false。
 */
static bool sd_in_multi_llcs(struct sched_domain *sd)
{
	struct sched_domain *sdp = sd->parent;

	/* it does not make sense to aggregate to 1 CPU */
	if (sd->span_weight == 1)
		return false;

	while (sdp && sdp->span_weight == sd->span_weight)
		sdp = sdp->parent;

	return !!sdp;
}

/*
 * Return the canonical balance CPU for this group, this is the first CPU
 * of this group that's also in the balance mask.
 *
 * The balance mask are all those CPUs that could actually end up at this
 * group. See build_balance_mask().
 *
 * Also see should_we_balance().
 */
/* balance_mask 的首个 CPU 是该组唯一向上继续做负载均衡的代表。 */
/*
 * 业务背景：overlap groups 需选唯一 canonical CPU 执行该组 capacity 更新和向上均衡。
 * 入参：sg 是不可空、balance mask 已构建的只读借用 sched_group。
 * 出参/返回：返回 balance mask 中首个 CPU；无输出参数和 ownership 变化。
 * 注意事项：调用者保证 mask 非空；空 mask 会返回 nr_cpu_ids 并破坏后续索引。
 */
int group_balance_cpu(struct sched_group *sg)
{
	return cpumask_first(group_balance_mask(sg));
}


/*
 * NUMA topology (first read the regular topology blurb below)
 *
 * Given a node-distance table, for example:
 *
 *   node   0   1   2   3
 *     0:  10  20  30  20
 *     1:  20  10  20  30
 *     2:  30  20  10  20
 *     3:  20  30  20  10
 *
 * which represents a 4 node ring topology like:
 *
 *   0 ----- 1
 *   |       |
 *   |       |
 *   |       |
 *   3 ----- 2
 *
 * We want to construct domains and groups to represent this. The way we go
 * about doing this is to build the domains on 'hops'. For each NUMA level we
 * construct the mask of all nodes reachable in @level hops.
 *
 * For the above NUMA topology that gives 3 levels:
 *
 * NUMA-2	0-3		0-3		0-3		0-3
 *  groups:	{0-1,3},{1-3}	{0-2},{0,2-3}	{1-3},{0-1,3}	{0,2-3},{0-2}
 *
 * NUMA-1	0-1,3		0-2		1-3		0,2-3
 *  groups:	{0},{1},{3}	{0},{1},{2}	{1},{2},{3}	{0},{2},{3}
 *
 * NUMA-0	0		1		2		3
 *
 *
 * As can be seen; things don't nicely line up as with the regular topology.
 * When we iterate a domain in child domain chunks some nodes can be
 * represented multiple times -- hence the "overlap" naming for this part of
 * the topology.
 *
 * In order to minimize this overlap, we only build enough groups to cover the
 * domain. For instance Node-0 NUMA-2 would only get groups: 0-1,3 and 1-3.
 *
 * Because:
 *
 *  - the first group of each domain is its child domain; this
 *    gets us the first 0-1,3
 *  - the only uncovered node is 2, who's child domain is 1-3.
 *
 * However, because of the overlap, computing a unique CPU for each group is
 * more complicated. Consider for instance the groups of NODE-1 NUMA-2, both
 * groups include the CPUs of Node-0, while those CPUs would not in fact ever
 * end up at those groups (they would end up in group: 0-1,3).
 *
 * To correct this we have to introduce the group balance mask. This mask
 * will contain those CPUs in the group that can reach this group given the
 * (child) domain tree.
 *
 * With this we can once again compute balance_cpu and sched_group_capacity
 * relations.
 *
 * XXX include words on how balance_cpu is unique and therefore can be
 * used for sched_group_capacity links.
 *
 *
 * Another 'interesting' topology is:
 *
 *   node   0   1   2   3
 *     0:  10  20  20  30
 *     1:  20  10  20  20
 *     2:  20  20  10  20
 *     3:  30  20  20  10
 *
 * Which looks a little like:
 *
 *   0 ----- 1
 *   |     / |
 *   |   /   |
 *   | /     |
 *   2 ----- 3
 *
 * This topology is asymmetric, nodes 1,2 are fully connected, but nodes 0,3
 * are not.
 *
 * This leads to a few particularly weird cases where the sched_domain's are
 * not of the same number for each CPU. Consider:
 *
 * NUMA-2	0-3						0-3
 *  groups:	{0-2},{1-3}					{1-3},{0-2}
 *
 * NUMA-1	0-2		0-3		0-3		1-3
 *
 * NUMA-0	0		1		2		3
 *
 */


/*
 * Build the balance mask; it contains only those CPUs that can arrive at this
 * group and should be considered to continue balancing.
 *
 * We do this during the group creation pass, therefore the group information
 * isn't complete yet, however since each group represents a (child) domain we
 * can fully construct this using the sched_domain bits (which are already
 * complete).
 */
/* 只保留能够从子 domain 到达本组的 CPU；空 mask 表示拓扑构造已损坏。 */
/*
 * 业务背景：NUMA overlap group 的 span 含不可实际到达该组的 CPU，需派生真实向上均衡代表集合。
 * 入参：sd/sg 不可空只读借用；mask 是不可空、调用者拥有的可写输出 cpumask。
 * 出参/返回：无直接返回值；清空并重建 mask，不转移 ownership。
 * 注意事项：构建期调用且依赖完整 per-CPU child domains；空结果仅 WARN，后续结构仍可能无效。
 */
static void
build_balance_mask(struct sched_domain *sd, struct sched_group *sg, struct cpumask *mask)
{
	const struct cpumask *sg_span = sched_group_span(sg);
	struct sd_data *sdd = sd->private;
	struct sched_domain *sibling;
	int i;

	/* 输出 mask 每次从空集重建，不依赖调用者传入的旧内容。 */
	cpumask_clear(mask);

	/* 只有 sibling child span 恰好等于目标 group span 的 CPU 才能沿子树到达该组。 */
	for_each_cpu(i, sg_span) {
		sibling = *per_cpu_ptr(sdd->sd, i);

		/*
		 * Can happen in the asymmetric case, where these siblings are
		 * unused. The mask will not be empty because those CPUs that
		 * do have the top domain _should_ span the domain.
		 */
		/* 非对称树中某些 sibling 未构建 child，这些 CPU 不能成为本组 balance CPU。 */
		if (!sibling->child)
			continue;

		/* If we would not end up here, we can't continue from here */
		/* child span 不同即表示向上走会进入另一 group，必须排除。 */
		if (!cpumask_equal(sg_span, sched_domain_span(sibling->child)))
			continue;

		cpumask_set_cpu(i, mask);
	}

	/* 空结果说明 group 与 per-CPU domain 树断开，后续容量代表无法选出。 */
	/* We must not have empty masks here */
	WARN_ON_ONCE(cpumask_empty(mask));
}

/*
 * XXX: This creates per-node group entries; since the load-balancer will
 * immediately access remote memory to construct this group's load-balance
 * statistics having the groups node local is of dubious benefit.
 */
/* 用子 domain 的 span 构造重叠组；分配失败由上层拆除已串起的环。 */
/*
 * 业务背景：NUMA overlap domain 的每个 group 需拥有私有 span，通常代表某 CPU 的 child domain。
 * 入参：sd 是不可空构建期借用 domain；cpu 是有效 CPU且决定 NUMA 分配节点。
 * 出参/返回：成功返回调用者拥有的新 group；分配失败返回 NULL；无输出参数。
 * 注意事项：仅建立 group/ref，不分配或绑定 sgc；上层失败路径负责释放已创建环。
 */
static struct sched_group *
build_group_from_child_sched_domain(struct sched_domain *sd, int cpu)
{
	struct sched_group *sg;
	struct cpumask *sg_span;

	/* 就近在起始 CPU 的 NUMA node 分配 group，span 紧随结构一次完成分配。 */
	sg = kzalloc_node(sizeof(struct sched_group) + cpumask_size(),
			GFP_KERNEL, cpu_to_node(cpu));

	if (!sg)
		return NULL;

	/* 有 child 时 group 代表下一层 span 并缓存其 flags，叶子则代表当前 domain。 */
	sg_span = sched_group_span(sg);
	if (sd->child) {
		cpumask_copy(sg_span, sched_domain_span(sd->child));
		sg->flags = sd->child->flags;
	} else {
		cpumask_copy(sg_span, sched_domain_span(sd));
	}

	/* 初始引用属于刚建立的 group 环，后续链入失败时由 free_sched_groups() 归还。 */
	atomic_inc(&sg->ref);
	return sg;
}

/* 共享 sgc 以引用计数归并，首次访问者负责写入 balance_mask 和初始容量。 */
/*
 * 业务背景：overlap group 的 capacity 按 canonical balance CPU 跨副本共享，且需防除零初始值。
 * 入参：sd/sg 不可空构建期输入/输出对象，sg 尚未初始化 sgc。
 * 出参/返回：无直接返回值、无输出参数；绑定共享 sgc、建立引用并初始化 mask/capacity。
 * 注意事项：依赖非空 balance mask；重复访问必须得到完全相同 mask，否则 WARN。
 */
static void init_overlap_sched_group(struct sched_domain *sd,
				     struct sched_group *sg)
{
	struct cpumask *mask = sched_domains_tmpmask2;
	struct sd_data *sdd = sd->private;
	struct cpumask *sg_span;
	int cpu;

	/* 先计算真实可到达 CPU，最小 CPU 是共享 sgc storage 的 canonical owner。 */
	build_balance_mask(sd, sg, mask);
	cpu = cpumask_first(mask);

	/* 首个引用者初始化 balance mask，后续引用者必须计算出完全相同结果。 */
	sg->sgc = *per_cpu_ptr(sdd->sgc, cpu);
	if (atomic_inc_return(&sg->sgc->ref) == 1)
		cpumask_copy(group_balance_mask(sg), mask);
	else
		WARN_ON_ONCE(!cpumask_equal(group_balance_mask(sg), mask));

	/*
	 * Initialize sgc->capacity such that even if we mess up the
	 * domains and no possible iteration will get us here, we won't
	 * die on a /0 trap.
	 */
	/* 先按 CPU 数给出非零保守容量，后续正常 capacity 自底向上迭代会覆盖它。 */
	sg_span = sched_group_span(sg);
	sg->sgc->capacity = SCHED_CAPACITY_SCALE * cpumask_weight(sg_span);
	sg->sgc->min_capacity = SCHED_CAPACITY_SCALE;
	sg->sgc->max_capacity = SCHED_CAPACITY_SCALE;
}

/* 向下寻找 span 不越出当前 domain 且不会随后退化掉的有效兄弟层。 */
/*
 * 业务背景：高直径 NUMA topology 中直接 sibling child 可能越出目标 span，需找到可代表 group 的后代。
 * 入参：sd/sibling 不可空且完整构建的只读借用 domain。
 * 出参/返回：返回借用的合适 sibling/descendant；无输出参数和引用变化。
 * 注意事项：先满足 child span 子集，再跳过同 span 退化层；要求 child 链无环。
 */
static struct sched_domain *
find_descended_sibling(struct sched_domain *sd, struct sched_domain *sibling)
{
	/*
	 * The proper descendant would be the one whose child won't span out
	 * of sd
	 */
	/* 先向下走到 child span 不再越出正在构建的 sd。 */
	while (sibling->child &&
	       !cpumask_subset(sched_domain_span(sibling->child),
			       sched_domain_span(sd)))
		sibling = sibling->child;

	/*
	 * As we are referencing sgc across different topology level, we need
	 * to go down to skip those sched_domains which don't contribute to
	 * scheduling because they will be degenerated in cpu_attach_domain
	 */
	/* 再跳过 span 完全相同、提交时必定被折叠的中间层。 */
	while (sibling->child &&
	       cpumask_equal(sched_domain_span(sibling->child),
			     sched_domain_span(sibling)))
		sibling = sibling->child;

	return sibling;
}

/* 为非树形 NUMA span 建立可重叠的组环；ENOMEM 时释放此前完成的组。 */
/*
 * 业务背景：NUMA 距离图不是严格树时，一个 domain 要用最少 overlap groups 覆盖完整 span。
 * 入参：sd 是不可空输入/输出构建对象；cpu 是该 per-CPU domain 的有效起始 CPU。
 * 出参/返回：成功返回 0并设置闭合 groups 环；分配失败返回 -ENOMEM并释放已建 groups。
 * 注意事项：借用全局 scratch 且要求 domains mutex 串行；失败不释放共享 sgc storage。
 */
static int
build_overlap_sched_groups(struct sched_domain *sd, int cpu)
{
	struct sched_group *first = NULL, *last = NULL, *sg;
	const struct cpumask *span = sched_domain_span(sd);
	struct cpumask *covered = sched_domains_tmpmask;
	struct sd_data *sdd = sd->private;
	struct sched_domain *sibling;
	int i;

	/* covered 记录已由新 group 覆盖的 CPU，使重叠 span 不会产生重复组。 */
	cpumask_clear(covered);

	for_each_cpu_wrap(i, span, cpu) {
		struct cpumask *sg_span;

		/* 从指定 cpu 环形遍历以确保本 CPU 对应 group 成为环首。 */
		if (cpumask_test_cpu(i, covered))
			continue;

		/* 每个未覆盖 CPU 的 per-CPU domain 树提供一个候选 group span。 */
		sibling = *per_cpu_ptr(sdd->sd, i);

		/*
		 * Asymmetric node setups can result in situations where the
		 * domain tree is of unequal depth, make sure to skip domains
		 * that already cover the entire range.
		 * 中文补充：非对称拓扑会让不同 CPU 的 domain 深度不同，已提前终止的空 span 不能构造 group。
		 *
		 * In that case build_sched_domains() will have terminated the
		 * iteration early and our sibling sd spans will be empty.
		 * Domains should always include the CPU they're built on, so
		 * check that.
		 */
		/* domain 若连它的基准 CPU 都不包含，即是上述未使用的不等深度条目。 */
		if (!cpumask_test_cpu(i, sched_domain_span(sibling)))
			continue;

		/*
		 * Usually we build sched_group by sibling's child sched_domain
		 * But for machines whose NUMA diameter are 3 or above, we move
		 * 中文补充：NUMA 直径较大时，直接 child 可超出当前 sd span，必须下沉到合适后代再取其 child。
		 * to build sched_group by sibling's proper descendant's child
		 * domain because sibling's child sched_domain will span out of
		 * the sched_domain being built as below.
		 *
		 * Smallest diameter=3 topology is:
		 *
		 *   node   0   1   2   3
		 *     0:  10  20  30  40
		 *     1:  20  10  20  30
		 *     2:  30  20  10  20
		 *     3:  40  30  20  10
		 *
		 *   0 --- 1 --- 2 --- 3
		 *
		 * NUMA-3       0-3             N/A             N/A             0-3
		 *  groups:     {0-2},{1-3}                                     {1-3},{0-2}
		 *
		 * NUMA-2       0-2             0-3             0-3             1-3
		 *  groups:     {0-1},{1-3}     {0-2},{2-3}     {1-3},{0-1}     {2-3},{0-2}
		 *
		 * NUMA-1       0-1             0-2             1-3             2-3
		 *  groups:     {0},{1}         {1},{2},{0}     {2},{3},{1}     {3},{2}
		 *
		 * NUMA-0       0               1               2               3
		 *
		 * The NUMA-2 groups for nodes 0 and 3 are obviously buggered, as the
		 * group span isn't a subset of the domain span.
		 * 因此下面的子集检查是结构正确性门禁，而不是单纯的拓扑优化。
		 */
		if (sibling->child &&
		    !cpumask_subset(sched_domain_span(sibling->child), span))
			sibling = find_descended_sibling(sd, sibling);

		/* 有效 sibling 确定后才分配 group，避免为将被跳过的层级建立私有对象。 */
		sg = build_group_from_child_sched_domain(sibling, cpu);
		if (!sg)
			goto fail;

		/* 新 span 立即并入 covered，后续 CPU 若落在其中就共用当前 group。 */
		sg_span = sched_group_span(sg);
		cpumask_or(covered, covered, sg_span);

		/* 绑定共享 sgc 后再链入环，确保已可见节点总是完整初始化。 */
		init_overlap_sched_group(sibling, sg);

		/* first/last 每轮都维持闭环，即使下一步分配失败也可从 first 安全回收。 */
		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
		last->next = first;
	}
	/* 全部 span 覆盖完成后才将环首提交给 sd。 */
	sd->groups = first;

	return 0;

fail:
	free_sched_groups(first, 0);

	return -ENOMEM;
}


/*
 * Package topology (also see the load-balance blurb in fair.c)
 *
 * The scheduler builds a tree structure to represent a number of important
 * topology features. By default (default_topology[]) these include:
 *
 *  - Simultaneous multithreading (SMT)
 *  - Multi-Core Cache (MC)
 *  - Package (PKG)
 *
 * Where the last one more or less denotes everything up to a NUMA node.
 *
 * The tree consists of 3 primary data structures:
 *
 *	sched_domain -> sched_group -> sched_group_capacity
 *	    ^ ^             ^ ^
 *          `-'             `-'
 *
 * The sched_domains are per-CPU and have a two way link (parent & child) and
 * denote the ever growing mask of CPUs belonging to that level of topology.
 *
 * Each sched_domain has a circular (double) linked list of sched_group's, each
 * denoting the domains of the level below (or individual CPUs in case of the
 * first domain level). The sched_group linked by a sched_domain includes the
 * CPU of that sched_domain [*].
 *
 * Take for instance a 2 threaded, 2 core, 2 cache cluster part:
 *
 * CPU   0   1   2   3   4   5   6   7
 *
 * PKG  [                             ]
 * MC   [             ] [             ]
 * SMT  [     ] [     ] [     ] [     ]
 *
 *  - or -
 *
 * PKG  0-7 0-7 0-7 0-7 0-7 0-7 0-7 0-7
 * MC	0-3 0-3 0-3 0-3 4-7 4-7 4-7 4-7
 * SMT  0-1 0-1 2-3 2-3 4-5 4-5 6-7 6-7
 *
 * CPU   0   1   2   3   4   5   6   7
 *
 * One way to think about it is: sched_domain moves you up and down among these
 * topology levels, while sched_group moves you sideways through it, at child
 * domain granularity.
 *
 * sched_group_capacity ensures each unique sched_group has shared storage.
 *
 * There are two related construction problems, both require a CPU that
 * uniquely identify each group (for a given domain):
 *
 *  - The first is the balance_cpu (see should_we_balance() and the
 *    load-balance blurb in fair.c); for each group we only want 1 CPU to
 *    continue balancing at a higher domain.
 *
 *  - The second is the sched_group_capacity; we want all identical groups
 *    to share a single sched_group_capacity.
 *
 * Since these topologies are exclusive by construction. That is, its
 * impossible for an SMT thread to belong to multiple cores, and cores to
 * be part of multiple caches. There is a very clear and unique location
 * for each CPU in the hierarchy.
 *
 * Therefore computing a unique CPU for each group is trivial (the iteration
 * mask is redundant and set all 1s; all CPUs in a group will end up at _that_
 * group), we can simply pick the first CPU in each group.
 *
 *
 * [*] in other words, the first group of each domain is its child domain.
 */

/* 以规范 CPU 索引取得共享 group/sgc；只有第一次引用执行初始化。 */
/*
 * 业务背景：普通树形 topology 中同一 child span 的 per-CPU domains 必须共享唯一 group 与 capacity。
 * 入参：cpu 是候选 CPU；sdd 是不可空、构建期稳定的 per-level 分配集合。
 * 出参/返回：返回非空借用共享 group并增加 group/sgc 引用；无输出参数。
 * 注意事项：要求预分配条目完整；首次访问初始化 span，后续访问只能复用相同对象。
 */
static struct sched_group *get_group(int cpu, struct sd_data *sdd)
{
	struct sched_domain *sd = *per_cpu_ptr(sdd->sd, cpu);
	struct sched_domain *child = sd->child;
	struct sched_group *sg;
	bool already_visited;

	/* 有 child 时以 child span 的最小 CPU 作 canonical key，使各 per-CPU domain 命中同一 group。 */
	if (child)
		cpu = cpumask_first(sched_domain_span(child));

	/* group 与 capacity 必须使用同一 canonical CPU 槽，否则引用趋势会分叉。 */
	sg = *per_cpu_ptr(sdd->sg, cpu);
	sg->sgc = *per_cpu_ptr(sdd->sgc, cpu);

	/* Increase refcounts for claim_allocations: */
	/* 两个对象的首访/复用状态必须一致，WARN 抓取不对称 claim。 */
	already_visited = atomic_inc_return(&sg->ref) > 1;
	/* sgc visits should follow a similar trend as sg */
	WARN_ON(already_visited != (atomic_inc_return(&sg->sgc->ref) > 1));

	/* If we have already visited that group, it's already initialized. */
	if (already_visited)
		return sg;

	/* 首次访问才初始化 span：非叶子复制 child，叶子只放 canonical CPU。 */
	if (child) {
		cpumask_copy(sched_group_span(sg), sched_domain_span(child));
		cpumask_copy(group_balance_mask(sg), sched_group_span(sg));
		sg->flags = child->flags;
	} else {
		cpumask_set_cpu(cpu, sched_group_span(sg));
		cpumask_set_cpu(cpu, group_balance_mask(sg));
	}

	/* 预置非零线性容量，防止正式 capacity 迭代未到达时出现除零。 */
	sg->sgc->capacity = SCHED_CAPACITY_SCALE * cpumask_weight(sched_group_span(sg));
	sg->sgc->min_capacity = SCHED_CAPACITY_SCALE;
	sg->sgc->max_capacity = SCHED_CAPACITY_SCALE;

	return sg;
}

/*
 * build_sched_groups will build a circular linked list of the groups
 * covered by the given span, will set each group's ->cpumask correctly,
 * and will initialize their ->sgc.
 *
 * Assumes the sched_domain tree is fully constructed
 */
/* 对非重叠拓扑按子 domain 划分 span，最终形成覆盖当前 domain 的闭环。 */
/*
 * 业务背景：树形 SMT/cluster/package domain 要按下一层互斥 spans 创建环形 groups。
 * 入参：sd 是不可空输入/输出 domain；cpu 是有效且决定环遍历起点的 CPU。
 * 出参/返回：成功恒返回 0并设置 sd->groups；无错误返回类别或输出参数。
 * 注意事项：调用者持 domains mutex且完整预分配；假定至少一个 group，空 span 会使 last 无效。
 */
static int
build_sched_groups(struct sched_domain *sd, int cpu)
{
	struct sched_group *first = NULL, *last = NULL;
	struct sd_data *sdd = sd->private;
	const struct cpumask *span = sched_domain_span(sd);
	struct cpumask *covered;
	int i;

	/* 全局 scratch mask 使构建无需临时分配，也要求 domains_mutex 串行。 */
	lockdep_assert_held(&sched_domains_mutex);
	covered = sched_domains_tmpmask;

	cpumask_clear(covered);

	/* 环形遍历从当前 cpu 开始，使包含它的 group 稳定成为 sd->groups 环首。 */
	for_each_cpu_wrap(i, span, cpu) {
		struct sched_group *sg;

		if (cpumask_test_cpu(i, covered))
			continue;

		/* 未覆盖 CPU 标识一个新 child span，get_group() 返回其共享 canonical 对象。 */
		sg = get_group(i, sdd);

		cpumask_or(covered, covered, sched_group_span(sg));

		/* 普通树形 topology 的 group 不重叠，每个新 span 只需按顺序追加。 */
		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
	}
	/* span 非空的前置保证 last 已设置，最后一条边将单链闭合成环。 */
	last->next = first;
	sd->groups = first;

	return 0;
}

/*
 * Initialize sched groups cpu_capacity.
 *
 * cpu_capacity indicates the capacity of sched group, which is used while
 * distributing the load between different sched groups in a sched domain.
 * Typically cpu_capacity for all the groups in a sched domain will be same
 * unless there are asymmetries in the topology. If there are asymmetries,
 * group having more cpu_capacity will pickup more load compared to the
 * group having less cpu_capacity.
 */
/* 汇总组权重、核心数和非对称首选 CPU，仅 balance CPU 刷新共享容量。 */
/*
 * 业务背景：groups 环完成后要派生 CPU/物理 core 数、asym packing 首选 CPU及运行容量。
 * 入参：cpu 是当前 domain 所属 CPU；sd 是不可空输入/输出且 groups 已闭合的 domain。
 * 出参/返回：无直接返回值、无输出参数；更新各 group 元数据，canonical CPU 再刷新共享 capacity。
 * 注意事项：借用全局 scratch；仅 group_balance_cpu 执行 update_group_capacity以避免重复写共享 sgc。
 */
static void init_sched_groups_capacity(int cpu, struct sched_domain *sd)
{
	struct sched_group *sg = sd->groups;
	struct cpumask *mask = sched_domains_tmpmask2;

	/* groups 应在前一构建阶段完成，WARN 后继续解引用仅用于暴露内核缺陷。 */
	WARN_ON(!sg);

	do {
		int cpu, cores = 0, max_cpu = -1;

		/* group_weight 是逻辑 CPU 数，cores 则通过每次删掉一整组 SMT siblings 计数。 */
		sg->group_weight = cpumask_weight(sched_group_span(sg));

		cpumask_copy(mask, sched_group_span(sg));
		for_each_cpu(cpu, mask) {
			cores++;
			cpumask_andnot(mask, mask, cpu_smt_mask(cpu));
		}
		sg->cores = cores;

		/* 只有 asym packing domain 需扫描架构优先级，其他层保留默认值。 */
		if (!(sd->flags & SD_ASYM_PACKING))
			goto next;

		/* 组内逐 CPU 比较得到架构定义的最优 packing 目标。 */
		for_each_cpu(cpu, sched_group_span(sg)) {
			if (max_cpu < 0)
				max_cpu = cpu;
			else if (sched_asym_prefer(cpu, max_cpu))
				max_cpu = cpu;
		}
		sg->asym_prefer_cpu = max_cpu;

next:
		sg = sg->next;
	} while (sg != sd->groups);

	/* sg 经完整环遍历已回到环首；只有 canonical balance CPU 可写共享 capacity。 */
	if (cpu != group_balance_cpu(sg))
		return;

	update_group_capacity(sd, cpu);
}

/* Update the "asym_prefer_cpu" when arch_asym_cpu_priority() changes. */
/* 架构优先级变化时更新所有受影响组；RCU 保护正在发布的 domain 链。 */
/*
 * 业务背景：架构动态改变 CPU packing 优先级后，调度器必须修正各层 group 的 asym_prefer_cpu。
 * 入参：cpu 是变化 CPU；old_prio/new_prio 是变化前后可比较的架构优先级。
 * 出参/返回：无直接返回值、无输出参数；RCU 下按需 WRITE_ONCE 更新受影响 group。
 * 注意事项：overlap NUMA 当前仅 WARN不完整支持；排名不再可能胜出时可提前终止父链遍历。
 */
void sched_update_asym_prefer_cpu(int cpu, int old_prio, int new_prio)
{
	int asym_prefer_cpu = cpu;
	struct sched_domain *sd;

	/* guard 保证整个 parent 链遍历期间 domain/group 不被旧拓扑销毁。 */
	guard(rcu)();

	/* 从最内层向上更新，利用 parent span 为子层超集的性质做提前终止。 */
	for_each_domain(cpu, sd) {
		struct sched_group *sg;
		int group_cpu;

		/* 没有 asym packing 语义的层不消费 asym_prefer_cpu，无需改写。 */
		if (!(sd->flags & SD_ASYM_PACKING))
			continue;

		/*
		 * Groups of overlapping domain are replicated per NUMA
		 * node and will require updating "asym_prefer_cpu" on
		 * each local copy.
		 * 中文补充：重叠 NUMA group 存在多份本地副本，当前字段位置无法一次更新全部副本。
		 *
		 * If you are hitting this warning, consider moving
		 * "sg->asym_prefer_cpu" to "sg->sgc->asym_prefer_cpu"
		 * which is shared by all the overlapping groups.
		 */
		WARN_ON_ONCE(sd->flags & SD_NUMA);

		/* 当前 CPU 对应的 group 是环首，因此可直接检查其缓存的首选 CPU。 */
		sg = sd->groups;
		if (cpu != sg->asym_prefer_cpu) {
			/*
			 * Since the parent is a superset of the current group,
			 * if the cpu is not the "asym_prefer_cpu" at the
			 * current level, it cannot be the preferred CPU at a
			 * higher levels either.
			 */
			/* cpu 在当前层尚不能胜出，在包含更多候选的父层也不可能胜出。 */
			if (!sched_asym_prefer(cpu, sg->asym_prefer_cpu))
				return;

			WRITE_ONCE(sg->asym_prefer_cpu, cpu);
			continue;
		}

		/* Ranking has improved; CPU is still the preferred one. */
		/* 原本就是首选且排名提升/不变时，不需重扫描同组 CPU。 */
		if (new_prio >= old_prio)
			continue;

		/* 排名下降时从整个 group 重新选最优 CPU，并把结果传递到更高层。 */
		for_each_cpu(group_cpu, sched_group_span(sg)) {
			if (sched_asym_prefer(group_cpu, asym_prefer_cpu))
				asym_prefer_cpu = group_cpu;
		}

		WRITE_ONCE(sg->asym_prefer_cpu, asym_prefer_cpu);
	}
}

/*
 * Set of available CPUs grouped by their corresponding capacities
 * Each list entry contains a CPU mask reflecting CPUs that share the same
 * capacity.
 * The lifespan of data is unlimited.
 */
LIST_HEAD(asym_cap_list);

/*
 * Verify whether there is any CPU capacity asymmetry in a given sched domain.
 * Provides sd_flags reflecting the asymmetry scope.
 */
/* 统计 span 中容量类别；缺失系统中仍存在的类别时只能标记局部非对称。 */
/*
 * 业务背景：domain flags 要区分仅局部看到多容量与已覆盖分区全部容量类别的完整非对称层。
 * 入参：sd_span/cpu_map 均不可空只读借用 masks，分别表示本层和完整构建分区。
 * 出参/返回：对称返回 0；局部返回 SD_ASYM_CPUCAPACITY；全覆盖再加 FULL；无输出参数。
 * 注意事项：遍历全局容量类别列表，调用者须处于其更新同步或 RCU 保护条件。
 */
static inline int
asym_cpu_capacity_classify(const struct cpumask *sd_span,
			   const struct cpumask *cpu_map)
{
	struct asym_cap_data *entry;
	int count = 0, miss = 0;

	/* count 统计本 sd 看到的容量类，miss 统计分区存在但本 sd 未覆盖的类。 */
	/*
	 * Count how many unique CPU capacities this domain spans across
	 * (compare sched_domain CPUs mask with ones representing  available
	 * CPUs capacities). Take into account CPUs that might be offline:
	 * skip those.
	 */
	/* 下线 CPU 已在重建 scan 中从类别 mask 清除，此处只比较当前有效交集。 */
	list_for_each_entry(entry, &asym_cap_list, link) {
		if (cpumask_intersects(sd_span, cpu_capacity_span(entry)))
			++count;
		else if (cpumask_intersects(cpu_map, cpu_capacity_span(entry)))
			++miss;
	}

	/* 列表非空但本分区一类都没命中，说明 scan 与 domain 范围发生了同步破坏。 */
	WARN_ON_ONCE(!count && !list_empty(&asym_cap_list));

	/* No asymmetry detected */
	/* 少于两类没有非对称；有 miss 只标局部；无 miss 才标全容量覆盖。 */
	if (count < 2)
		return 0;
	/* Some of the available CPU capacity values have not been detected */
	if (miss)
		return SD_ASYM_CPUCAPACITY;

	/* Full asymmetry */
	return SD_ASYM_CPUCAPACITY | SD_ASYM_CPUCAPACITY_FULL;

}

/* 容量类别可能仍被 RCU 读者遍历，宽限期结束后才释放。 */
/*
 * 业务背景：CPU 下线删除最后一个容量类别后，已有 topology 读者仍可能持有该链节点。
 * 入参：head 是不可空、内嵌于待释放 asym_cap_data 的 RCU 回调对象。
 * 出参/返回：无直接返回值、无输出参数；释放对应类别节点。
 * 注意事项：RCU callback 不可睡眠；调用前节点必须已从 asym_cap_list 摘除。
 */
static void free_asym_cap_entry(struct rcu_head *head)
{
	struct asym_cap_data *entry = container_of(head, struct asym_cap_data, rcu);
	/* cpumask 紧随 entry 分配，释放外层结构即同时终结两者生命期。 */
	kfree(entry);
}

/* 将 CPU 放入按容量降序排列的类别；内存不足时保留可运行但较保守的分类。 */
/*
 * 业务背景：CPU online 时要把架构容量加入全局有序类别，以供各 sched_domain 判定非对称范围。
 * 入参：cpu 是有效且 topology 更新锁保护下的 CPU 编号。
 * 出参/返回：无直接返回值、无输出参数；复用现有类别或分配并插入新类别，再设置 CPU 位。
 * 注意事项：可分配；失败时跳过新类别而不阻止 CPU online，分类可能暂时低估非对称性。
 */
static inline void asym_cpu_capacity_update_data(int cpu)
{
	unsigned long capacity = arch_scale_cpu_capacity(cpu);
	struct asym_cap_data *insert_entry = NULL;
	struct asym_cap_data *entry;

	/* 架构容量是排序 key，相同值的 CPU 共享一个类别节点。 */
	/*
	 * Search if capacity already exits. If not, track which the entry
	 * where we should insert to keep the list ordered descending.
	 */
	/* 列表保持降序；insert_entry 记录首个更小值之前的插入位置。 */
	list_for_each_entry(entry, &asym_cap_list, link) {
		if (capacity == entry->capacity)
			goto done;
		else if (!insert_entry && capacity > entry->capacity)
			insert_entry = list_prev_entry(entry, link);
	}

	/* 没有现成类别才分配节点和内联 cpumask；失败不影响 CPU 可运行性。 */
	entry = kzalloc(sizeof(*entry) + cpumask_size(), GFP_KERNEL);
	if (WARN_ONCE(!entry, "Failed to allocate memory for asymmetry data\n"))
		return;
	entry->capacity = capacity;

	/* If NULL then the new capacity is the smallest, add last. */
	/* 没找到更小元素说明新容量最小，否则插到记录位置之后。 */
	if (!insert_entry)
		list_add_tail_rcu(&entry->link, &asym_cap_list);
	else
		list_add_rcu(&entry->link, &insert_entry->link);
done:
	/* 无论复用还是新建类别，最后都把 CPU 放入对应 capacity span。 */
	__cpumask_set_cpu(cpu, cpu_capacity_span(entry));
}

/*
 * Build-up/update list of CPUs grouped by their capacities
 * An update requires explicit request to rebuild sched domains
 * with state indicating CPU topology changes.
 */
/* 重建容量类别并用 call_rcu() 回收消失项；单一类别无需保留。 */
/*
 * 业务背景：CPU topology 变化后需从 possible housekeeping CPUs 重建容量类别，清除下线/隔离残留。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参数；更新全局有序 asym_cap_list并 RCU 回收空/单一类别。
 * 注意事项：调用者持 topology 更新锁；列表节点删除后仍可能被 RCU 读者访问。
 */
static void asym_cpu_capacity_scan(void)
{
	struct asym_cap_data *entry, *next;
	int cpu;

	/* 先清空所有旧 span，但保留节点以复用已存在的 capacity 类别。 */
	list_for_each_entry(entry, &asym_cap_list, link)
		cpumask_clear(cpu_capacity_span(entry));

	/* 只有 possible 且参与 domain housekeeping 的 CPU 进入新分类。 */
	for_each_cpu_and(cpu, cpu_possible_mask, housekeeping_cpumask(HK_TYPE_DOMAIN))
		asym_cpu_capacity_update_data(cpu);

	/* 无 CPU 的旧类别先 RCU 摘链，实际内存回收等待旧读者。 */
	list_for_each_entry_safe(entry, next, &asym_cap_list, link) {
		if (cpumask_empty(cpu_capacity_span(entry))) {
			list_del_rcu(&entry->link);
			call_rcu(&entry->rcu, free_asym_cap_entry);
		}
	}

	/*
	 * Only one capacity value has been detected i.e. this system is symmetric.
	 * No need to keep this data around.
	 */
	/* 仅一类意味着系统对称，删除唯一节点可让后续分类走空列表快路径。 */
	if (list_is_singular(&asym_cap_list)) {
		entry = list_first_entry(&asym_cap_list, typeof(*entry), link);
		list_del_rcu(&entry->link);
		call_rcu(&entry->rcu, free_asym_cap_entry);
	}
}

/*
 * Initializers for schedule domains
 * Non-inlined to reduce accumulated stack pressure in build_sched_domains()
 */

static int default_relax_domain_level = -1;
int sched_domain_level_max;

/* 解析启动参数；非法值只告警并保留默认配置。 */
/*
 * 业务背景：启动参数允许限制从哪一层开始关闭 wake/newidle balance，以控制跨域迁移积极度。
 * 入参：str 是不可空、NUL 结尾的只读借用参数值。
 * 出参/返回：恒返回 1表示参数已消费；解析成功写全局层级，失败仅告警。
 * 注意事项：仅启动早期；不传播 errno，非法输入保留此前默认值。
 */
static int __init setup_relax_domain_level(char *str)
{
	if (kstrtoint(str, 0, &default_relax_domain_level))
		pr_warn("Unable to set relax_domain_level\n");

	return 1;
}
__setup("relax_domain_level=", setup_relax_domain_level);

/* 达到 relax 层级后关闭唤醒/新空闲均衡，限制主动跨域搬迁。 */
/*
 * 业务背景：domain 构建需把全局或分区 relax_domain_level 转换为具体负载均衡 flags。
 * 入参：sd 是不可空输入/输出构建对象；attr 是可空只读分区属性，负值表示使用全局默认。
 * 出参/返回：无直接返回值、无输出参数；达到请求层级时清 WAKE/NEWIDLE flags。
 * 注意事项：只在发布前修改 sd；无有效请求时保持原 flags，不校验超出最大层级。
 */
static void set_domain_attribute(struct sched_domain *sd,
				 struct sched_domain_attr *attr)
{
	int request;

	/* 分区未指定有效值时回退到启动全局值，两者均为负则保持原策略。 */
	if (!attr || attr->relax_domain_level < 0) {
		if (default_relax_domain_level < 0)
			return;
		request = default_relax_domain_level;
	} else
		request = attr->relax_domain_level;

	/* 请求层级以上关闭唤醒/新空闲均衡，更低层仍保留快速迁移。 */
	if (sd->level >= request) {
		/* Turn off idle balance on this domain: */
		sd->flags &= ~(SD_BALANCE_WAKE|SD_BALANCE_NEWIDLE);
	}
}

static void __sdt_free(const struct cpumask *cpu_map);
static int __sdt_alloc(const struct cpumask *cpu_map);

static void __sds_free(struct s_data *d, const struct cpumask *cpu_map);
static int __sds_alloc(struct s_data *d, const struct cpumask *cpu_map);

/* 按最后成功阶段逆序释放，已被 claim 的 NULL 条目不会被重复释放。 */
/*
 * 业务背景：多阶段 topology 分配失败或提交后要从精确阶段逆序释放仍由临时集合拥有的资源。
 * 入参：d 是不可空输入/输出分配集合；what 是最后成功阶段；cpu_map 是不可空只读分区 mask。
 * 出参/返回：无直接返回值、无输出参数；释放未 claim 的 root/percpu/domain/group storage。
 * 注意事项：switch 分支的顺序贯穿定义严格逆序；错误阶段会泄漏或重复释放，已发布条目必须先置 NULL。
 */
static void __free_domain_allocs(struct s_data *d, enum s_alloc what,
				 const struct cpumask *cpu_map)
{
	switch (what) {
	case sa_rootdomain:
		/* 已被 rq 认领的 rd 引用非零，此时临时集不再拥有它。 */
		if (!atomic_read(&d->rd->refcount))
			free_rootdomain(&d->rd->rcu);
		fallthrough;
	case sa_sd:
		/* per-CPU 链首容器不拥有已 claim 的 domain 本体，只回收指针存储。 */
		free_percpu(d->sd);
		fallthrough;
	case sa_sd_shared:
		/* shared 和 topology storage 内部都会跳过已被 claim 置 NULL 的槽。 */
		__sds_free(d, cpu_map);
		fallthrough;
	case sa_sd_storage:
		__sdt_free(cpu_map);
		fallthrough;
	case sa_none:
		break;
	}
}

/* 逐层建立临时存储并返回精确回滚点，不把半初始化对象发布给 rq。 */
/*
 * 业务背景：domain 构建需要事务式预分配全部层级、共享数据、per-CPU 链首和 root-domain。
 * 入参：d 是不可空、调用者拥有的输出集合；cpu_map 是不可空只读目标分区。
 * 出参/返回：返回最高成功分配阶段，sa_rootdomain 表示完整成功，其余值指示回滚边界。
 * 注意事项：可睡眠且先清零 d；失败不自行全回滚，调用者必须传返回值给 __free_domain_allocs。
 */
static enum s_alloc
__visit_domain_allocation_hell(struct s_data *d, const struct cpumask *cpu_map)
{
	/* 将所有指针初始为 NULL，使任一失败阶段的逆序回滚都能区分未建资源。 */
	memset(d, 0, sizeof(*d));

	/* 分配顺序与 s_alloc 阶段一致，每个返回值表示当前可安全回滚的最高层。 */
	if (__sdt_alloc(cpu_map))
		return sa_sd_storage;
	if (__sds_alloc(d, cpu_map))
		return sa_sd_shared;
	/* 层级 storage/shared 完整后再建每 CPU 基链首，最后才分配 root-domain。 */
	d->sd = alloc_percpu(struct sched_domain *);
	if (!d->sd)
		return sa_sd_shared;
	d->rd = alloc_rootdomain();
	if (!d->rd)
		return sa_sd;

	return sa_rootdomain;
}

/*
 * NULL the sd_data elements we've used to build the sched_domain and
 * sched_group structure so that the subsequent __free_domain_allocs()
 * will not free the data we're using.
 */
/* 把已接入拓扑的共享对象从临时数组摘除，将其所有权移交给 domain 链。 */
/*
 * 业务背景：成功构建的 group/shared 对象仍登记在临时数组，提交前必须防止统一清理重复释放。
 * 入参：cpu 是已构建 CPU；d 是不可空输入/输出临时 ownership 集合。
 * 出参/返回：无直接返回值、无输出参数；把已有引用的 sds/sd/sg/sgc 槽置 NULL。
 * 注意事项：仅构建成功后调用；WARN 校验 per-CPU sd 身份，遗漏 claim 会造成已发布对象 UAF。
 */
static void claim_allocations(int cpu, struct s_data *d)
{
	struct sched_domain *sd;

	/* shared 引用非零说明至少有一层已认领，临时槽立即放弃 ownership。 */
	if (atomic_read(&(*per_cpu_ptr(d->sds, cpu))->ref))
		*per_cpu_ptr(d->sds, cpu) = NULL;

	/* 沿本 CPU 完整 parent 链逐层摘除 sd/group/sgc 的预分配槽。 */
	for (sd = *per_cpu_ptr(d->sd, cpu); sd; sd = sd->parent) {
		struct sd_data *sdd = sd->private;

		/* 槽中对象必须恰好是当前链节点，否则构建索引已错位。 */
		WARN_ON_ONCE(*per_cpu_ptr(sdd->sd, cpu) != sd);
		*per_cpu_ptr(sdd->sd, cpu) = NULL;

		if (atomic_read(&(*per_cpu_ptr(sdd->sg, cpu))->ref))
			*per_cpu_ptr(sdd->sg, cpu) = NULL;

		/* 只摘除已建引用的 group/capacity，未使用预分配仍由统一回滚释放。 */
		if (atomic_read(&(*per_cpu_ptr(sdd->sgc, cpu))->ref))
			*per_cpu_ptr(sdd->sgc, cpu) = NULL;
	}
}

#ifdef CONFIG_NUMA
enum numa_topology_type sched_numa_topology_type;

/*
 * sched_domains_numa_distance is derived from sched_numa_node_distance
 * 中文补充：node 距离保留原始值，domain 距离则是专为拓扑分层简化的可覆盖视图。
 * and provides a simplified view of NUMA distances used specifically
 * for building NUMA scheduling domains.
 */
/* 层数、距离数组和三级 mask 指针由 sched_init/reset_numa() 成组发布与撤销。 */
static int			sched_domains_numa_levels;
static int			sched_numa_node_levels;

int				sched_max_numa_distance;
static int			*sched_domains_numa_distance;
static int			*sched_numa_node_distance;
static struct cpumask		***sched_domains_numa_masks;
#endif /* CONFIG_NUMA */

/*
 * SD_flags allowed in topology descriptions.
 *
 * These flags are purely descriptive of the topology and do not prescribe
 * behaviour. Behaviour is artificial and mapped in the below sd_init()
 * 中文补充：列表中大部分 flag 只描述硬件共享关系，真正均衡行为集中在 sd_init() 中映射。
 * function. For details, see include/linux/sched/sd_flags.h.
 *
 *   SD_SHARE_CPUCAPACITY
 *   SD_SHARE_LLC
 *   SD_CLUSTER
 *   SD_NUMA
 *
 * Odd one out, which beside describing the topology has a quirk also
 * prescribes the desired behaviour that goes along with it:
 *
 *   SD_ASYM_PACKING        - describes SMT quirks
 */
#define TOPOLOGY_SD_FLAGS		\
	(SD_SHARE_CPUCAPACITY	|	\
	 SD_CLUSTER		|	\
	 SD_SHARE_LLC		|	\
	 SD_NUMA		|	\
	 SD_ASYM_PACKING)

/* 把 topology level 的描述转换成一个 per-CPU domain，并连接 child。 */
/*
 * 业务背景：架构 topology 描述必须实例化为带默认均衡策略、span和 child 链的 per-CPU sched_domain。
 * 入参：tl/cpu_map 不可空只读；child 是可空已建下层借用；cpu 是有效目标 CPU。
 * 出参/返回：返回预分配且原地初始化的非空 domain；无输出参数或新 ownership。
 * 注意事项：构建期独占；会按容量/SMT/LLC/NUMA 改 flags，非法架构 flags 被 WARN 后裁剪。
 */
static struct sched_domain *
sd_init(struct sched_domain_topology_level *tl,
	const struct cpumask *cpu_map,
	struct sched_domain *child, int cpu)
{
	struct sd_data *sdd = &tl->data;
	struct sched_domain *sd = *per_cpu_ptr(sdd->sd, cpu);
	int sd_id, sd_weight, sd_flags = 0;
	struct cpumask *sd_span;
	u64 now = sched_clock();

	/* 实际 span 是分区 cpu_map 与架构本层 mask 的交集，id 取最小 CPU 作稳定标识。 */
	sd_span = sched_domain_span(sd);
	cpumask_and(sd_span, cpu_map, tl->mask(tl, cpu));
	sd_weight = cpumask_weight(sd_span);
	sd_id = cpumask_first(sd_span);

	/* 架构只允许返回描述性 topology flags，非法行为 flag 在 WARN 后被裁掉。 */
	if (tl->sd_flags)
		sd_flags = (*tl->sd_flags)();
	if (WARN_ONCE(sd_flags & ~TOPOLOGY_SD_FLAGS,
		      "wrong sd_flags in topology description\n"))
		sd_flags &= TOPOLOGY_SD_FLAGS;
	sd_flags |= asym_cpu_capacity_classify(sd_span, cpu_map);

	/* 先以与 span 权重相关的通用均衡参数整体初始化 domain。 */
	*sd = (struct sched_domain){
		.min_interval		= sd_weight,
		.max_interval		= 2*sd_weight,
		.busy_factor		= 16,
		.imbalance_pct		= 117,

		.cache_nice_tries	= 0,

		/* 默认行为基线与架构/容量分类 flags 合并，后面再按层类型精调。 */
		.flags			= 1*SD_BALANCE_NEWIDLE
					| 1*SD_BALANCE_EXEC
					| 1*SD_BALANCE_FORK
					| 0*SD_BALANCE_WAKE
					| 1*SD_WAKE_AFFINE
					| 0*SD_SHARE_CPUCAPACITY
					| 0*SD_SHARE_LLC
					| 0*SD_SERIALIZE
					/* PREFER_SIBLING 为默认值，NUMA/异构分支会在初始器之后按需清除。 */
					| 1*SD_PREFER_SIBLING
					| 0*SD_NUMA
					| sd_flags
					,

		/* 时间字段以构建时刻为起点，避免新 domain 立即被当作长期未均衡。 */
		.last_balance		= jiffies,
		.balance_interval	= sd_weight,

		/* newidle 先验设定为 50% 成功率，运行统计会逐步更新这些计数。 */
		/* 50% success rate */
		.newidle_call		= 512,
		.newidle_success	= 256,
		.newidle_ratio		= 512,
		.newidle_stamp		= now,

		/* 成本与衰减基线清零/当前 jiffies，child/name 保留拓扑层级关系。 */
		.max_newidle_lb_cost	= 0,
		.last_decay_max_lb_cost	= jiffies,
		.child			= child,
		.name			= tl->name,
	};

	/* SMT 共享算力与异构 CPU 容量模型目前不兼容，同时出现是架构描述错误。 */
	WARN_ONCE((sd->flags & (SD_SHARE_CPUCAPACITY | SD_ASYM_CPUCAPACITY)) ==
		  (SD_SHARE_CPUCAPACITY | SD_ASYM_CPUCAPACITY),
		  "CPU capacity asymmetry not supported on SMT\n");

	/*
	 * Convert topological properties into behaviour.
	 */
	/* Don't attempt to spread across CPUs of different capacities. */
	/* 异构容量层不应鼓励向 sibling 扩散，因此清除 child 的 PREFER_SIBLING。 */
	if ((sd->flags & SD_ASYM_CPUCAPACITY) && sd->child)
		sd->child->flags &= ~SD_PREFER_SIBLING;

	/* 按互斥的主要拓扑属性调整 imbalance 阈值、cache 尝试和均衡 flags。 */
	if (sd->flags & SD_SHARE_CPUCAPACITY) {
		sd->imbalance_pct = 110;

	} else if (sd->flags & SD_SHARE_LLC) {
		sd->imbalance_pct = 117;
		sd->cache_nice_tries = 1;

#ifdef CONFIG_NUMA
	} else if (sd->flags & SD_NUMA) {
		sd->cache_nice_tries = 2;

		/* NUMA 层应串行跨节点均衡，且不将子组当成普通 sibling 优先。 */
		sd->flags &= ~SD_PREFER_SIBLING;
		sd->flags |= SD_SERIALIZE;
		/* 超过 node reclaim 距离后，exec/fork/wake 不主动跨层迁移，避免远端内存代价。 */
		if (sched_domains_numa_distance[tl->numa_level] > node_reclaim_distance) {
			sd->flags &= ~(SD_BALANCE_EXEC |
				       SD_BALANCE_FORK |
				       SD_WAKE_AFFINE);
		}

#endif /* CONFIG_NUMA */
	} else {
		sd->cache_nice_tries = 1;
	}

	/* private 回指本层预分配数据，供后续 group 构建找同层 per-CPU 对象。 */
	sd->private = sdd;

	return sd;
}

#ifdef CONFIG_SCHED_SMT
/* SMT 层共享算力和 LLC，这些描述标志随后由 sd_init() 映射为均衡策略。 */
/*
 * 业务背景：SMT 层的逻辑 CPU 共享执行能力和末级缓存，domain 需据此设置均衡语义。
 * 入参：无。
 * 出参/返回：返回 SD_SHARE_CPUCAPACITY | SD_SHARE_LLC 标志集；无输出参。
 * 注意事项：仅 CONFIG_SCHED_SMT 下存在；结果由 sd_init() 消费，函数无锁、不睡眠。
 */
int cpu_smt_flags(void)
{
	return SD_SHARE_CPUCAPACITY | SD_SHARE_LLC;
}

/* 返回架构维护的 SMT 兄弟掩码，生命周期由拓扑代码而非调用者管理。 */
/*
 * 业务背景：domain 构建需用架构 SMT sibling mask 确定最底层 span。
 * 入参：tl 是未使用的可空只读层级描述；cpu 是有效 CPU 编号、仅输入。
 * 出参/返回：返回 cpu 所在 SMT 兄弟的只读借用 cpumask；无输出参。
 * 注意事项：返回对象由架构拓扑维护，调用者不得修改或释放；热插拔稳定性由上层构建上下文保证。
 */
const struct cpumask *tl_smt_mask(struct sched_domain_topology_level *tl, int cpu)
{
	return cpu_smt_mask(cpu);
}
#endif

#ifdef CONFIG_SCHED_CLUSTER
/* cluster 是共享 LLC 的中间拓扑层。 */
/*
 * 业务背景：cluster 是介于 SMT 与 package 之间的共享 LLC 调度层。
 * 入参：无。
 * 出参/返回：返回 SD_CLUSTER | SD_SHARE_LLC 标志集；无输出参。
 * 注意事项：仅 CONFIG_SCHED_CLUSTER 下存在；无状态、无锁且不睡眠。
 */
int cpu_cluster_flags(void)
{
	return SD_CLUSTER | SD_SHARE_LLC;
}

/* 返回 CPU 所属 cluster 的只读掩码。 */
/*
 * 业务背景：domain 构建用 cluster group mask 界定中间缓存拓扑 span。
 * 入参：tl 是未使用的可空只读层级描述；cpu 是有效 CPU 编号、仅输入。
 * 出参/返回：返回 cpu 所属 cluster 的只读借用 cpumask；无输出参。
 * 注意事项：返回值由架构拓扑拥有，不得修改或释放；热插拔同步由调用上下文负责。
 */
const struct cpumask *tl_cls_mask(struct sched_domain_topology_level *tl, int cpu)
{
	return cpu_clustergroup_mask(cpu);
}
#endif

#ifdef CONFIG_SCHED_MC
/* MC 层通常对应共享末级缓存。 */
/*
 * 业务背景：MC 层通常映射共享末级缓存的 core group。
 * 入参：无。
 * 出参/返回：返回 SD_SHARE_LLC 标志；无输出参。
 * 注意事项：仅 CONFIG_SCHED_MC 下存在；少数架构可另行覆盖 LLC mask，本函数不睡眠。
 */
int cpu_core_flags(void)
{
	return SD_SHARE_LLC;
}

/* 返回 CPU 所属 core group 的只读掩码。 */
/*
 * 业务背景：MC domain 需用 core group mask 建立缓存共享边界。
 * 入参：tl 是未使用的可空只读层级描述；cpu 是有效 CPU 编号、仅输入。
 * 出参/返回：返回 cpu 的 core group 只读借用 cpumask；无输出参。
 * 注意事项：调用者不得释放或修改掩码；需在拓扑稳定的热插拔上下文使用。
 */
const struct cpumask *tl_mc_mask(struct sched_domain_topology_level *tl, int cpu)
{
	return cpu_coregroup_mask(cpu);
}

/*
 * Majority of architectures have LLC at MC domain level with exception
 * 中文补充：大多架构在 MC 层界定 LLC，PowerPC 等例外可覆盖 arch_llc_mask()。
 * such as powerpc. Provide a way for arch to specify where its LLC is
 * if it falls in exception category
 */
# ifndef arch_llc_mask
#define arch_llc_mask(cpu) cpu_coregroup_mask(cpu)
# endif

#else
#define arch_llc_mask(cpu) cpumask_of(cpu)
#endif

#define llc_mask(cpu) arch_llc_mask(cpu)

/* package 默认以 NUMA node 的 CPU 集合作为 span。 */
/*
 * 业务背景：默认 package 层以 CPU 所在 NUMA node 的 CPU 集合作为 span。
 * 入参：tl 是未使用的可空只读层级描述；cpu 是有效 CPU 编号、仅输入。
 * 出参/返回：返回 cpu 所在 node 的只读借用 cpumask；无输出参。
 * 注意事项：返回值生命周期由 NUMA/拓扑子系统维护；调用者不得修改或释放。
 */
const struct cpumask *tl_pkg_mask(struct sched_domain_topology_level *tl, int cpu)
{
	return cpu_node_mask(cpu);
}

/*
 * Topology list, bottom-up.
 */
static struct sched_domain_topology_level default_topology[] = {
	/* 默认层级自底向上排列，未启用的配置层在编译期被裁掉。 */
#ifdef CONFIG_SCHED_SMT
	SDTL_INIT(tl_smt_mask, cpu_smt_flags, SMT),
#endif

#ifdef CONFIG_SCHED_CLUSTER
	SDTL_INIT(tl_cls_mask, cpu_cluster_flags, CLS),
#endif

#ifdef CONFIG_SCHED_MC
	SDTL_INIT(tl_mc_mask, cpu_core_flags, MC),
#endif
	/* package 是默认非 NUMA 顶层，最后的 NULL mask 条目是遍历哨兵。 */
	SDTL_INIT(tl_pkg_mask, NULL, PKG),
	{ NULL, },
};

static struct sched_domain_topology_level *sched_domain_topology =
	default_topology;
static struct sched_domain_topology_level *sched_domain_topology_saved;

#define for_each_sd_topology(tl)			\
	for (tl = sched_domain_topology; tl->mask; tl++)

/* 架构只能在调度器 SMP 初始化前替换层级表，之后调用仅告警不生效。 */
/*
 * 业务背景：架构可在 SMP 调度初始化前用自定义层级表替换默认 topology。
 * 入参：tl 是不可空、以 mask==NULL 结尾的只读层级数组，调用后长期由调度器借用。
 * 出参/返回：无直接返回值、无输出参；成功替换全局 sched_domain_topology。
 * 注意事项：仅 __init 阶段使用；sched_smp_initialized 后只 WARN 并拒绝替换；数组不得提前释放。
 */
void __init set_sched_topology(struct sched_domain_topology_level *tl)
{
	if (WARN_ON_ONCE(sched_smp_initialized))
		return;

	sched_domain_topology = tl;
	sched_domain_topology_saved = NULL;
}

#ifdef CONFIG_NUMA
/* NUMA 层允许重叠，并触发远距离均衡语义。 */
/*
 * 业务背景：NUMA domain 允许 span 重叠，并需启用跨节点均衡语义。
 * 入参：无。
 * 出参/返回：返回 SD_NUMA 标志；无输出参。
 * 注意事项：仅 CONFIG_NUMA 下存在；结果由 sd_init() 解释，无锁、不睡眠。
 */
static int cpu_numa_flags(void)
{
	return SD_NUMA;
}

/* 从 RCU 发布的距离层表取得该 CPU 所在 node 的覆盖掩码。 */
/*
 * 业务背景：每个 NUMA topology level 需按距离取得 cpu 所在 node 的覆盖 mask。
 * 入参：tl 是不可空只读层级描述，numa_level 须在已发布范围；cpu 是有效 CPU 编号、仅输入。
 * 出参/返回：返回对应距离层和 node 的只读借用 cpumask；无输出参。
 * 注意事项：返回指针依赖 RCU 发布表生命周期；本函数不做越界/NULL 检查，只能在拓扑构建保证下调用。
 */
static const struct cpumask *sd_numa_mask(struct sched_domain_topology_level *tl, int cpu)
{
	return sched_domains_numa_masks[tl->numa_level][cpu_to_node(cpu)];
}

/* 拓扑异常只完整打印一次，避免每次重建重复淹没日志。 */
/*
 * 业务背景：NUMA 距离矩阵异常时需输出现场，但应避免每次重建淹没日志。
 * 入参：str 是不可空、NUL 结尾的只读错误描述，函数不保存也不释放。
 * 出参/返回：无直接返回值、无输出参；首次调用打印描述和完整距离矩阵。
 * 注意事项：静态 done 没有加锁，预期在串行化拓扑重建路径使用；只记录一次，不传播错误。
 */
static void sched_numa_warn(const char *str)
{
	static int done = false;
	int i,j;

	/* 第一个告警负责打印全部矩阵，后续重建遇到同类错误直接静默返回。 */
	if (done)
		return;

	done = true;

	/* 标题先给出调用点描述，随后矩阵为每一对 node 提供原始距离现场。 */
	printk(KERN_WARNING "ERROR: %s\n\n", str);

	for (i = 0; i < nr_node_ids; i++) {
		printk(KERN_WARNING "  ");
		/* 无 CPU node 的距离用括号标记，以区分当前不参与调度的矩阵项。 */
		for (j = 0; j < nr_node_ids; j++) {
			if (!node_state(i, N_CPU) || !node_state(j, N_CPU))
				printk(KERN_CONT "(%02d) ", node_distance(i,j));
			else
				printk(KERN_CONT " %02d  ", node_distance(i,j));
		}
		printk(KERN_CONT "\n");
	}
	printk(KERN_WARNING "\n");
}

/* 在 RCU 读侧检查原始 node 距离集合；本地距离始终存在。 */
/*
 * 业务背景：其他 NUMA 策略需判断某个距离值是否出现在调度拓扑中。
 * 入参：distance 是非负的 NUMA 距离数值、仅输入，单位与 node_distance() 一致。
 * 出参/返回：本地距离或已发布数组中存在返回 true，否则 false；无输出参。
 * 注意事项：内部持 RCU 读锁保护数组；表未初始化时除本地距离外均返回 false，不睡眠。
 */
bool find_numa_distance(int distance)
{
	bool found = false;
	int i, *distances;

	/* 本地距离是架构必然存在的基准值，不需依赖动态数组发布。 */
	if (distance == node_distance(0, 0))
		return true;

	/* 非本地值在 RCU 读侧扫描当前升序数组，重建可并发替换指针。 */
	rcu_read_lock();
	distances = rcu_dereference(sched_numa_node_distance);
	if (!distances)
		goto unlock;
	/* 层数与指针在发布/撤销路径成对管理，本路径只读取快照。 */
	for (i = 0; i < sched_numa_node_levels; i++) {
		if (distances[i] == distance) {
			found = true;
			break;
		}
	}
unlock:
	rcu_read_unlock();

	return found;
}

/* 遍历当前有 CPU 的 nodes，但跳过热插拔重建显式排除的那一个 node。 */
#define for_each_cpu_node_but(n, nbut)		\
	for_each_node_state(n, N_CPU)		\
		if (n == nbut)			\
			continue;		\
		else

/*
 * A system can have three types of NUMA topology:
 * NUMA_DIRECT: all nodes are directly connected, or not a NUMA system
 * NUMA_GLUELESS_MESH: some nodes reachable through intermediary nodes
 * NUMA_BACKPLANE: nodes can reach other nodes through a backplane
 *
 * The difference between a glueless mesh topology and a backplane
 * topology lies in whether communication between not directly
 * connected nodes goes through intermediary nodes (where programs
 * could run), or through backplane controllers. This affects
 * placement of programs.
 *
 * The type of topology can be discerned with the following tests:
 * - If the maximum distance between any nodes is 1 hop, the system
 *   is directly connected.
 * - If for two nodes A and B, located N > 1 hops away from each other,
 *   there is an intermediary node C, which is < N hops away from both
 *   nodes A and B, the system is a glueless mesh.
 */
/* 根据最远节点间是否存在中继节点区分直连、无胶合网格和背板。 */
/*
 * 业务背景：调度器需区分直连、无胶合网格和背板 NUMA，以解释远距离层。
 * 入参：offline_node 是本次重建要排除的 node id，NUMA_NO_NODE 表示不排除，仅输入。
 * 出参/返回：无直接返回值、无输出参；更新全局 sched_numa_topology_type。
 * 注意事项：依赖已构建的距离层和最大距离，应在热插拔/domain mutex 串行路径调用；分类失败降级 DIRECT。
 */
static void init_numa_topology_type(int offline_node)
{
	int a, b, c, n;

	n = sched_max_numa_distance;

	/* 只有本地和一个远端距离层时，所有 node 可视为直连。 */
	if (sched_domains_numa_levels <= 2) {
		sched_numa_topology_type = NUMA_DIRECT;
		return;
	}

	/* 扫描最远 node 对；非最远组合不能决定全局 topology 类型。 */
	for_each_cpu_node_but(a, offline_node) {
		for_each_cpu_node_but(b, offline_node) {
			/* Find two nodes furthest removed from each other. */
			if (node_distance(a, b) < n)
				continue;

			/* Is there an intermediary node between a and b? */
			/* 同时比 a/b 更接近的中继 node c 证明通信途经可运行节点，即 glueless mesh。 */
			for_each_cpu_node_but(c, offline_node) {
				if (node_distance(a, c) < n &&
				    node_distance(b, c) < n) {
					sched_numa_topology_type =
							NUMA_GLUELESS_MESH;
					return;
				}
			}

			/* 对一个最远节点对找不到中继，则它们通过不可运行的背板连接。 */
			sched_numa_topology_type = NUMA_BACKPLANE;
			return;
		}
	}

	/* 理论上有 CPU node 应命中一类，扫描失败时保守降级为 DIRECT 并留日志。 */
	pr_err("Failed to find a NUMA topology type, defaulting to DIRECT\n");
	sched_numa_topology_type = NUMA_DIRECT;
}


#define NR_DISTANCE_VALUES (1 << DISTANCE_BITS)

/*
 * An architecture could modify its NUMA distance, to change
 * grouping of NUMA nodes and number of NUMA levels when creating
 * NUMA level sched domains.
 *
 * A NUMA level is created for each unique
 * arch_sched_node_distance.
 */
/* 默认距离来源是固件 node_distance()，架构可覆盖弱别名改变分组。 */
/*
 * 业务背景：默认调度 NUMA 分层直接采用固件/架构 node_distance()。
 * 入参：i 和 j 是有效 NUMA node id、仅输入，无可空指针。
 * 出参/返回：返回 i 到 j 的距离数值，单位/范围由架构 node_distance() 定义；无输出参。
 * 注意事项：作为 arch_sched_node_distance 弱别名的默认实现；不校验 node id，不睡眠。
 */
static int numa_node_dist(int i, int j)
{
	return node_distance(i, j);
}

/* 架构可覆盖该弱符号；返回值决定创建哪些 NUMA domain 层。 */
int arch_sched_node_distance(int from, int to)
			     __weak __alias(numa_node_dist);

/*
 * 业务背景：只有架构改写了调度距离函数时，domain 距离层才需与原始 node 距离分开记录。
 * 入参：无。
 * 出参/返回：arch_sched_node_distance 不再指向默认 numa_node_dist 时返回 true，否则 false；无输出参。
 * 注意事项：比较的是函数地址而非距离值；链接期弱符号覆盖决定结果，运行期无竞态。
 */
static bool modified_sched_node_distance(void)
{
	return numa_node_dist != arch_sched_node_distance;
}

/* 去重并排序距离矩阵；非法距离或分配失败时不发布半成品数组。 */
/*
 * 业务背景：NUMA domain 每个唯一距离值对应一层，构建前需扫描矩阵并去重排序。
 * 入参：offline_node 是排除 node id 或 NUMA_NO_NODE；n_dist 是不可空只读回调，输入两个 node id 并返回距离；dist/levels 是不可空输出槽。
 * 出参/返回：成功返回 0，*dist 获得升序距离数组所有权、*levels 得到元素数；分配失败返回 -ENOMEM，距离越界返回 -EINVAL。
 * 注意事项：可因 GFP_KERNEL 分配睡眠；失败时不写两个输出槽，成功后调用者必须 kfree(*dist)。
 */
static int sched_record_numa_dist(int offline_node, int (*n_dist)(int, int),
				  int **dist, int *levels)
{
	unsigned long *distance_map __free(bitmap) = NULL;
	int nr_levels = 0;
	int i, j;
	int *distances;

	/* bitmap 以距离数值作 bit 下标，一次完成去重并为后续升序遍历提供索引。 */
	/*
	 * O(nr_nodes^2) de-duplicating selection sort -- in order to find the
	 * unique distances in the node_distance() table.
	 */
	distance_map = bitmap_alloc(NR_DISTANCE_VALUES, GFP_KERNEL);
	if (!distance_map)
		return -ENOMEM;

	/* 跳过 offline_node 后扫描全部有 CPU node 对，回调值必须能安全作 bitmap 下标。 */
	bitmap_zero(distance_map, NR_DISTANCE_VALUES);
	for_each_cpu_node_but(i, offline_node) {
		for_each_cpu_node_but(j, offline_node) {
			int distance = n_dist(i, j);

			/* 越界值无法表示且可破坏 bitmap，记录矩阵后立即失败。 */
			if (distance < LOCAL_DISTANCE || distance >= NR_DISTANCE_VALUES) {
				sched_numa_warn("Invalid distance value range");
				return -EINVAL;
			}

			bitmap_set(distance_map, distance, 1);
		}
	}
	/*
	 * We can now figure out how many unique distance values there are and
	 * allocate memory accordingly.
	 */
	/* bit 数即唯一层数，输出数组按精确长度分配。 */
	nr_levels = bitmap_weight(distance_map, NR_DISTANCE_VALUES);

	distances = kzalloc_objs(int, nr_levels);
	if (!distances)
		return -ENOMEM;

	/* find_next_bit() 自然按数值升序填充，j++ 确保下次从已命中 bit 之后开始。 */
	for (i = 0, j = 0; i < nr_levels; i++, j++) {
		j = find_next_bit(distance_map, NR_DISTANCE_VALUES, j);
		distances[i] = j;
	}
	*dist = distances;
	*levels = nr_levels;

	return 0;
}

/* 构造逐距离、逐 node 的 CPU mask，再一次性发布并扩展 topology 表。 */
/*
 * 业务背景：调度器需把 NUMA 距离矩阵转换为逐距离、逐 node 的 CPU mask 和动态 topology 层。
 * 入参：offline_node 是本次构建要排除的 node id，NUMA_NO_NODE 表示全部包含，仅输入。
 * 出参/返回：无直接返回值、无输出参；成功时 RCU 发布距离/mask，替换全局 topology 并更新层数和类型。
 * 注意事项：可因 GFP_KERNEL 分配睡眠，预期由热插拔和 domains_mutex 串行；任一分配失败会提前返回并保留已发布/已分配部分，随后必须由 sched_reset_numa() 收敛。
 */
void sched_init_numa(int offline_node)
{
	struct sched_domain_topology_level *tl;
	int nr_levels, nr_node_levels;
	int i, j;
	int *distances, *domain_distances;
	struct cpumask ***masks;

	/* 原始 SLIT/node_distance 数组用于对外距离查询，记录失败时不改全局状态。 */
	/* Record the NUMA distances from SLIT table */
	if (sched_record_numa_dist(offline_node, numa_node_dist, &distances,
				   &nr_node_levels))
		return;

	/* 架构覆盖距离时另建 domain 层数组，否则与原始数组共享所有权。 */
	/* Record modified NUMA distances for building sched domains */
	if (modified_sched_node_distance()) {
		if (sched_record_numa_dist(offline_node, arch_sched_node_distance,
					   &domain_distances, &nr_levels)) {
			kfree(distances);
			return;
		}
	} else {
		domain_distances = distances;
		nr_levels = nr_node_levels;
	}
	/* 原始距离首先 RCU 发布，最大值和层数以 READ/WRITE_ONCE 形成配套标量快照。 */
	rcu_assign_pointer(sched_numa_node_distance, distances);
	WRITE_ONCE(sched_max_numa_distance, distances[nr_node_levels - 1]);
	WRITE_ONCE(sched_numa_node_levels, nr_node_levels);

	/*
	 * 'nr_levels' contains the number of unique distances
	 *
	 * The sched_domains_numa_distance[] array includes the actual distance
	 * numbers.
	 */

	/*
	 * Here, we should temporarily reset sched_domains_numa_levels to 0.
	 * If it fails to allocate memory for array sched_domains_numa_masks[][],
	 * the array will contain less then 'nr_levels' members. This could be
	 * dangerous when we use it to iterate array sched_domains_numa_masks[][]
	 * in other functions.
	 *
	 * We reset it to 'nr_levels' at the end of this function.
	 * 中文补充：构建 mask 前临时将可遍历层数置 0，防止失败时读者越过已分配边界。
	 */
	rcu_assign_pointer(sched_domains_numa_distance, domain_distances);

	sched_domains_numa_levels = 0;

	/* 外层数组按距离层索引，零填充使 reset 能识别中途失败的空层。 */
	masks = kzalloc(sizeof(void *) * nr_levels, GFP_KERNEL);
	if (!masks)
		return;

	/*
	 * Now for each level, construct a mask per node which contains all
	 * CPUs of nodes that are that many hops away from us.
	 */
	/* 每个距离层拥有一组按 node id 索引的 CPU mask 指针。 */
	for (i = 0; i < nr_levels; i++) {
		masks[i] = kzalloc(nr_node_ids * sizeof(void *), GFP_KERNEL);
		if (!masks[i])
			return;

		/* 仅为当前有 CPU 且未排除的 node 分配实体 cpumask。 */
		for_each_cpu_node_but(j, offline_node) {
			struct cpumask *mask = kzalloc(cpumask_size(), GFP_KERNEL);
			int k;

			if (!mask)
				return;

			/* 指针先记入层数组，使后续 reset 能回收这个部分完成对象。 */
			masks[i][j] = mask;

			/* 对所有有 CPU node k，把与 j 的架构距离不超本层阈值的 CPU 并入。 */
			for_each_cpu_node_but(k, offline_node) {
				if (sched_debug() &&
				    (arch_sched_node_distance(j, k) !=
				     arch_sched_node_distance(k, j)))
					sched_numa_warn("Node-distance not symmetric");

				/* 非对称距离只告警，实际覆盖仍按 j 到 k 的有向值判定。 */
				if (arch_sched_node_distance(j, k) >
				    sched_domains_numa_distance[i])
					continue;

				cpumask_or(mask, mask, cpumask_of_node(k));
			}
		}
	}
	/* 全部层/node mask 完成后才一次性发布外层指针。 */
	rcu_assign_pointer(sched_domains_numa_masks, masks);

	/* 计算旧 topology 层数，新数组还需容纳所有 NUMA 层和一个哨兵。 */
	/* Compute default topology size */
	for (i = 0; sched_domain_topology[i].mask; i++);

	tl = kzalloc((i + nr_levels + 1) *
			sizeof(struct sched_domain_topology_level), GFP_KERNEL);
	if (!tl)
		return;

	/*
	 * Copy the default topology bits..
	 */
	/* 先复制原有 SMT/cluster/MC/package 描述，保持它们的 data 和 callback。 */
	for (i = 0; sched_domain_topology[i].mask; i++)
		tl[i] = sched_domain_topology[i];

	/*
	 * Add the NUMA identity distance, aka single NODE.
	 */
	/* identity NODE 层覆盖本 node，不需 SD_NUMA 重叠行为 flag。 */
	tl[i++] = SDTL_INIT(sd_numa_mask, NULL, NODE);

	/*
	 * .. and append 'j' levels of NUMA goodness.
	 */
	/* 距离层 0 已由 NODE 表示，从 1 开始追加真正的 NUMA domains。 */
	for (j = 1; j < nr_levels; i++, j++) {
		tl[i] = SDTL_INIT(sd_numa_mask, cpu_numa_flags, NUMA);
		tl[i].numa_level = j;
	}

	/* 保存旧 topology ownership 供 reset 恢复，再发布新动态数组。 */
	sched_domain_topology_saved = sched_domain_topology;
	sched_domain_topology = tl;

	/* 层数在所有数组和 topology 就绪后最后恢复，它是读者进入遍历的门闩。 */
	sched_domains_numa_levels = nr_levels;

	init_numa_topology_type(offline_node);
}


/* 先撤销 RCU 指针，等待旧读者退出后释放距离表、mask 和动态层级。 */
/*
 * 业务背景：NUMA 节点热插拔重建前必须撤销旧距离表、mask 和动态 topology。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参；全局 NUMA 发布指针置空并恢复默认 topology。
 * 注意事项：预期在热插拔/domains_mutex 串行上下文；可在 synchronize_rcu() 等待，必须先断开全局指针再释放旧内存。
 */
static void sched_reset_numa(void)
{
	int nr_levels, *distances, *dom_distances = NULL;
	struct cpumask ***masks;

	/* 先保存旧层数用于释放，随后将所有对外标量重置为未初始化。 */
	nr_levels = sched_domains_numa_levels;
	sched_numa_node_levels = 0;
	sched_domains_numa_levels = 0;
	sched_max_numa_distance = 0;
	sched_numa_topology_type = NUMA_DIRECT;
	/* 原始与 domain 距离可指向同一数组，只有地址不同时才单独回收后者。 */
	distances = sched_numa_node_distance;
	if (sched_numa_node_distance != sched_domains_numa_distance)
		dom_distances = sched_domains_numa_distance;
	/* 在取得全部旧本地指针后，将三个 RCU 入口全部撤销。 */
	rcu_assign_pointer(sched_numa_node_distance, NULL);
	rcu_assign_pointer(sched_domains_numa_distance, NULL);
	masks = sched_domains_numa_masks;
	rcu_assign_pointer(sched_domains_numa_masks, NULL);
	/* 只要曾发布距离或 mask，都必须等待共同 grace period 再回收两组对象。 */
	if (distances || masks) {
		int i, j;

		synchronize_rcu();
		kfree(distances);
		kfree(dom_distances);
		/* 外层可为 NULL，每层也可因旧构建失败而为 NULL，逐级容错释放。 */
		for (i = 0; i < nr_levels && masks; i++) {
			if (!masks[i])
				continue;
			for_each_node(j)
				kfree(masks[i][j]);
			kfree(masks[i]);
		}
		kfree(masks);
	}
	/* 只有 sched_init_numa() 替换过动态表时 saved 才非空，恢复后清哨兵防重复释放。 */
	if (sched_domain_topology_saved) {
		kfree(sched_domain_topology);
		sched_domain_topology = sched_domain_topology_saved;
		sched_domain_topology_saved = NULL;
	}
}

/*
 * Call with hotplug lock held
 */
/* 仅 node 首个 CPU 上线或最后一个下线时重建 NUMA 拓扑。 */
/*
 * 业务背景：node 从无 CPU 变为有 CPU 或反向变化时，NUMA domain 距离层需整体重建。
 * 入参：cpu 是有效热插拔 CPU 编号；online 是仅输入状态，true 表示上线、false 表示下线。
 * 出参/返回：无直接返回值、无输出参；边界转换时重置并重建 NUMA 全局状态。
 * 注意事项：调用者必须持 CPU hotplug 锁；可睡眠并等待 RCU，非 node 首/末 CPU 转换直接返回。
 */
void sched_update_numa(int cpu, bool online)
{
	int node;

	/* 先把 CPU 映射到 node，只有当前 node mask 权重为 1 才是首个上线/最后下线边界。 */
	node = cpu_to_node(cpu);
	/*
	 * Scheduler NUMA topology is updated when the first CPU of a
	 * node is onlined or the last CPU of a node is offlined.
	 */
	if (cpumask_weight(cpumask_of_node(node)) != 1)
		return;

	/* 上线重建包含所有 node，下线重建显式排除即将失去 CPU 的 node。 */
	sched_reset_numa();
	sched_init_numa(online ? NUMA_NO_NODE : node);
}

/* CPU 上线后把它加入所有能覆盖其 node 的远端距离掩码。 */
/*
 * 业务背景：CPU 上线后，所有能覆盖其 node 的 NUMA 距离 mask 都必须包含该 CPU。
 * 入参：cpu 是小于 nr_cpu_ids 的有效 CPU 编号、仅输入，无指针所有权。
 * 出参/返回：无直接返回值、无输出参；原地修改已发布的 NUMA cpumask 表。
 * 注意事项：只能在 CPU hotplug 串行且表已初始化的上下文调用；无 NULL 检查，不分配、不睡眠。
 */
void sched_domains_numa_masks_set(unsigned int cpu)
{
	int node = cpu_to_node(cpu);
	int i, j;

	/* 逐距离层、逐有 CPU node 更新，使任意基准 node 的候选 mask 都与热插拔状态一致。 */
	for (i = 0; i < sched_domains_numa_levels; i++) {
		for (j = 0; j < nr_node_ids; j++) {
			if (!node_state(j, N_CPU))
				continue;

			/* Set ourselves in the remote node's masks */
			/* 仅当 j 到新 CPU node 的距离不超该层阈值时，新 CPU 才属于其候选集。 */
			if (arch_sched_node_distance(j, node) <=
			    sched_domains_numa_distance[i])
				cpumask_set_cpu(cpu, sched_domains_numa_masks[i][j]);
		}
	}
}

/* CPU 下线前从全部 NUMA 距离掩码移除，避免后续选择到失效 CPU。 */
/*
 * 业务背景：CPU 下线前必须从全部 NUMA 候选 mask 移除，防止后续调度选到失效 CPU。
 * 入参：cpu 是小于 nr_cpu_ids 的有效 CPU 编号、仅输入，无指针所有权。
 * 出参/返回：无直接返回值、无输出参；原地清除所有已分配距离 mask 中的 cpu bit。
 * 注意事项：只能在 CPU hotplug 串行上下文调用；允许单个 mask 为 NULL，不分配、不睡眠。
 */
void sched_domains_numa_masks_clear(unsigned int cpu)
{
	int i, j;

	/* 下线 CPU 必须从所有已分配 node mask 无条件清除，不再需重算距离。 */
	for (i = 0; i < sched_domains_numa_levels; i++) {
		for (j = 0; j < nr_node_ids; j++) {
			if (sched_domains_numa_masks[i][j])
				cpumask_clear_cpu(cpu, sched_domains_numa_masks[i][j]);
		}
	}
}

/*
 * sched_numa_find_closest() - given the NUMA topology, find the cpu
 *                             closest to @cpu from @cpumask.
 * cpumask: cpumask to find a cpu from
 * cpu: cpu to be close to
 *
 * returns: cpu, or nr_cpu_ids when nothing found.
 */
/*
 * sched_numa_find_closest() - 从候选掩码中按 NUMA 距离选择靠近基准 CPU 的目标。
 *
 * @cpus：候选 CPU 的只读借用掩码，不可为 NULL，函数不保存或修改它；
 * @cpu：距离计算的基准 CPU，必须是可映射到 NUMA node 的有效编号。
 * 调用者包括 housekeeping_any_cpu()，后者用它优先把可迁移内核工作留在邻近
 * NUMA 范围，再退化到全局在线 housekeeper 选择。
 *
 * 函数不睡眠、无状态副作用。返回找到的 CPU 编号；若 NUMA masks 尚未发布或
 * 所有距离层次都与 @cpus 无交集，返回 nr_cpu_ids 哨兵。返回的是瞬时选择，
 * 不获取 CPU hotplug 引用，调用者仍负责后续投递竞态。
 */
/*
 * 业务背景：工作队列等调用者需从候选集中选择与基准 CPU NUMA 距离最近的 CPU。
 * 入参：cpus 是不可空只读借用候选 mask；cpu 是小于 nr_cpu_ids 且可映射 node 的基准 CPU，均仅输入。
 * 出参/返回：返回首个有交集的最近距离层中的 CPU；表未发布或无候选时返回 nr_cpu_ids，无输出参。
 * 注意事项：内部 RCU 仅保护 NUMA mask 表，cpus 稳定性由调用者保证；返回编号不携带 hotplug 引用。
 */
int sched_numa_find_closest(const struct cpumask *cpus, int cpu)
{
	/*
	 * 变量地图：
	 *   i     从近到远遍历 NUMA 距离层级；
	 *   j     基准 CPU 所在 node；
	 *   found 最终 CPU，初始 nr_cpu_ids 表示未找到；
	 *   masks RCU 发布的 [distance level][node] CPU 掩码表借用指针。
	 */
	int i, j = cpu_to_node(cpu), found = nr_cpu_ids;
	struct cpumask ***masks;

	/*
	 * sched_domains_numa_masks 可在拓扑重建时替换。RCU 读锁保证 masks 及其层级
	 * 数组在整个扫描期间不被释放；它不冻结 @cpus，后者的稳定性由调用者保证。
	 */
	rcu_read_lock();
	masks = rcu_dereference(sched_domains_numa_masks);
	if (!masks)
		goto unlock;
	/*
	 * 距离层级按由近到远排列。每层从调用者候选和该 node 距离掩码的交集中
	 * distribute 选择，避免总压到第一个 CPU；首次命中即是最近可用层级。
	 */
	for (i = 0; i < sched_domains_numa_levels; i++) {
		if (!masks[i][j])
			break;
		cpu = cpumask_any_and_distribute(cpus, masks[i][j]);
		if (cpu < nr_cpu_ids) {
			found = cpu;
			break;
		}
	}
unlock:
	/* 到达此处时 found 已是有效 CPU 或保持哨兵；先结束 RCU 生命周期再返回编号。 */
	rcu_read_unlock();

	return found;
}

/* bsearch 的状态：寻找累计候选数首次超过第 cpu 个候选的距离层。 */
struct __cmp_key {
	const struct cpumask *cpus;
	struct cpumask ***masks;
	int node;
	int cpu;
	int w;
};

/* 比较同时记录上一层累计权重，供最终换算成当前 hop 内的序号。 */
/*
 * 业务背景：bsearch 需定位“累计候选数首次超过目标序号”的 NUMA hop。
 * 入参：a 是不可空、可写的 __cmp_key 借用指针；b 是不可空、指向当前 cpumask ** 元素的只读借用指针。
 * 出参/返回：返回 1/0/-1 指示目标在当前元素之后/当前/之前；命中时通过 k->w 输出上一层累计候选数。
 * 注意事项：调用者必须在 RCU 读侧保证 masks 存活；函数会修改 key，不能将 a 作为 const 共享。
 */
static int hop_cmp(const void *a, const void *b)
{
	struct cpumask **prev_hop, **cur_hop = *(struct cpumask ***)b;
	struct __cmp_key *k = (struct __cmp_key *)a;

	/* 当前层累计候选数仍不超过目标序号，目标必在更远层。 */
	if (cpumask_weight_and(k->cpus, cur_hop[k->node]) <= k->cpu)
		return 1;

	/* 最近层没有前一层，命中时的之前累计值定义为 0。 */
	if (b == k->masks) {
		k->w = 0;
		return 0;
	}

	/* 计算前一层累计数，用于把全局序号换算为当前 hop 内序号。 */
	prev_hop = *((struct cpumask ***)b - 1);
	k->w = cpumask_weight_and(k->cpus, prev_hop[k->node]);
	if (k->w <= k->cpu)
		return 0;

	return -1;
}

/**
 * sched_numa_find_nth_cpu() - given the NUMA topology, find the Nth closest CPU
 *                             from @cpus to @cpu, taking into account distance
 *                             from a given @node.
 * @cpus: cpumask to find a cpu from
 * @cpu: CPU to start searching
 * @node: NUMA node to order CPUs by distance
 *
 * Return: cpu, or nr_cpu_ids when nothing found.
 */
/* 在 RCU 保护下按距离层选第 N 个在线候选；不存在时返回 nr_cpu_ids。 */
/*
 * 业务背景：调度器需按相对 node 的 NUMA 距离对候选 CPU 排序并取第 N 个。
 * 入参：cpus 是不可空只读候选 mask；cpu 是从 0 开始的非负序号；node 是基准 node id 或 NUMA_NO_NODE，均仅输入。
 * 出参/返回：返回按距离排序的第 cpu 个在线候选 CPU；不存在或表未发布时返回 nr_cpu_ids，无输出参。
 * 注意事项：NUMA_NO_NODE 走普通在线交集路径；内部 RCU 不保护 cpus，返回 CPU 不携带 hotplug 引用。
 */
int sched_numa_find_nth_cpu(const struct cpumask *cpus, int cpu, int node)
{
	struct __cmp_key k = { .cpus = cpus, .cpu = cpu };
	struct cpumask ***hop_masks;
	int hop, ret = nr_cpu_ids;

	/* 无基准 node 时不做 NUMA 排序，直接取候选与 online mask 的第 N 个交集。 */
	if (node == NUMA_NO_NODE)
		return cpumask_nth_and(cpu, cpus, cpu_online_mask);

	/* 从此处到 unlock 保护 hop masks 外层数组、各层和内部 cpumask 的生命期。 */
	rcu_read_lock();

	/* CPU-less node entries are uninitialized in sched_domains_numa_masks */
	/* 无 CPU node 没有分配 mask 槽，先折算到最近的有 CPU node。 */
	node = numa_nearest_node(node, N_CPU);
	k.node = node;

	k.masks = rcu_dereference(sched_domains_numa_masks);
	if (!k.masks)
		goto unlock;

	/* bsearch 在单调增长的累计候选数中找到首个包含目标序号的层。 */
	hop_masks = bsearch(&k, k.masks, sched_domains_numa_levels, sizeof(k.masks[0]), hop_cmp);
	if (!hop_masks)
		goto unlock;
	hop = hop_masks	- k.masks;

	/* 非首层通过减去前层 mask 只在“本 hop 新增 CPU”中取局部序号。 */
	ret = hop ?
		cpumask_nth_and_andnot(cpu - k.w, cpus, k.masks[hop][node], k.masks[hop-1][node]) :
		cpumask_nth_and(cpu, cpus, k.masks[0][node]);
unlock:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(sched_numa_find_nth_cpu);

/**
 * sched_numa_hop_mask() - Get the cpumask of CPUs at most @hops hops away from
 *                         @node
 * @node: The node to count hops from.
 * @hops: Include CPUs up to that many hops away. 0 means local node.
 *
 * Return: On success, a pointer to a cpumask of CPUs at most @hops away from
 * @node, an error value otherwise.
 *
 * Requires rcu_lock to be held. Returned cpumask is only valid within that
 * read-side section, copy it if required beyond that.
 *
 * Note that not all hops are equal in distance; see sched_init_numa() for how
 * distances and masks are handled.
 * Also note that this is a reflection of sched_domains_numa_masks, which may change
 * during the lifetime of the system (offline nodes are taken out of the masks).
 */
/* 返回值借用自 RCU 表，仅在调用者持有的读侧临界区内有效。 */
/*
 * 业务背景：调用者需查询从某 node 出发、不超过指定 NUMA hop 的 CPU 集合。
 * 入参：node 是小于 nr_node_ids 的 node id；hops 是小于 sched_domains_numa_levels 的非负层号，均仅输入。
 * 出参/返回：成功返回只读借用 cpumask；越界返回 ERR_PTR(-EINVAL)，表未就绪返回 ERR_PTR(-EBUSY)，无输出参。
 * 注意事项：调用者必须已持 RCU 读锁；返回 mask 仅在该读侧临界区有效，需跨越时必须复制。
 */
const struct cpumask *sched_numa_hop_mask(unsigned int node, unsigned int hops)
{
	struct cpumask ***masks;

	/* 先校验两级数组下标，避免对未发布或越界槽解引用。 */
	if (node >= nr_node_ids || hops >= sched_domains_numa_levels)
		return ERR_PTR(-EINVAL);

	/* 调用者的 RCU 读锁让返回的内层 mask 在函数返回后仍可借用。 */
	masks = rcu_dereference(sched_domains_numa_masks);
	if (!masks)
		return ERR_PTR(-EBUSY);

	return masks[hops][node];
}
EXPORT_SYMBOL_GPL(sched_numa_hop_mask);

#endif /* CONFIG_NUMA */

/* 为每层、每 CPU 预分配 sd/group/sgc；失败交给阶段回滚统一收尾。 */
/*
 * 业务背景：domain 构建前需为每个 topology level 和目标 CPU 预分配 sd/group/capacity 对象。
 * 入参：cpu_map 是不可空只读借用目标 CPU mask，函数不保存也不修改它。
 * 出参/返回：全部分配成功返回 0，任一步失败返回 -ENOMEM；通过各 tl->data per-CPU 槽输出已分配指针。
 * 注意事项：GFP_KERNEL/alloc_percpu 可睡眠；失败保留已分配部分，调用者必须用 __sdt_free() 回滚。
 */
static int __sdt_alloc(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	int j;

	/* 每个 topology level 先分配三个 per-CPU 指针容器，再填充目标 CPU 实体。 */
	for_each_sd_topology(tl) {
		struct sd_data *sdd = &tl->data;

		/* 任一容器失败立即返回，已建容器由上层按阶段回滚。 */
		sdd->sd = alloc_percpu(struct sched_domain *);
		if (!sdd->sd)
			return -ENOMEM;

		sdd->sg = alloc_percpu(struct sched_group *);
		if (!sdd->sg)
			return -ENOMEM;

		sdd->sgc = alloc_percpu(struct sched_group_capacity *);
		if (!sdd->sgc)
			return -ENOMEM;

		/* 实体按目标 CPU 所在 NUMA node 就近分配，并紧跟内联 cpumask。 */
		for_each_cpu(j, cpu_map) {
			struct sched_domain *sd;
			struct sched_group *sg;
			struct sched_group_capacity *sgc;

			/* sched_domain 必须先建立并记入槽，使后续 group 失败时可精确回收。 */
			sd = kzalloc_node(sizeof(struct sched_domain) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sd)
				return -ENOMEM;

			*per_cpu_ptr(sdd->sd, j) = sd;

			/* group 初始自环，即使正式环尚未构建，单节点也满足 next 不变量。 */
			sg = kzalloc_node(sizeof(struct sched_group) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sg)
				return -ENOMEM;

			sg->next = sg;

			*per_cpu_ptr(sdd->sg, j) = sg;

			/* capacity 与 balance mask 同块分配，id 先设为 CPU，后续可作 canonical group 标识。 */
			sgc = kzalloc_node(sizeof(struct sched_group_capacity) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sgc)
				return -ENOMEM;

			sgc->id = j;

			*per_cpu_ptr(sdd->sgc, j) = sgc;
		}
	}

	return 0;
}

/* 释放尚未被 claim 的拓扑对象，并清空 per-CPU 容器指针。 */
/*
 * 业务背景：构建失败或提交后要释放 topology 临时槽中仍有所有权的对象。
 * 入参：cpu_map 是不可空只读借用目标 CPU mask，必须与 __sdt_alloc() 使用的集合一致。
 * 出参/返回：无直接返回值、无输出参；释放未 claim 对象并将 tl->data per-CPU 容器指针置 NULL。
 * 注意事项：已 claim 的槽必须事先置 NULL，否则会双重释放；本函数不同步已发布 RCU 读者。
 */
static void __sdt_free(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	int j;

	/* 回滚按与分配相同的层/CPU 范围遍历，NULL 槽表示已被 claim。 */
	for_each_sd_topology(tl) {
		struct sd_data *sdd = &tl->data;

		/* NUMA domain 拥有私有 overlap groups，在释放 sd 本体前必须先回收该环。 */
		for_each_cpu(j, cpu_map) {
			struct sched_domain *sd;

			if (sdd->sd) {
				sd = *per_cpu_ptr(sdd->sd, j);
				if (sd && (sd->flags & SD_NUMA))
					free_sched_groups(sd->groups, 0);
				kfree(*per_cpu_ptr(sdd->sd, j));
			}

			/* 未 claim 的预分配 group/sgc 没有发布引用，可直接 kfree。 */
			if (sdd->sg)
				kfree(*per_cpu_ptr(sdd->sg, j));
			if (sdd->sgc)
				kfree(*per_cpu_ptr(sdd->sgc, j));
		}
		/* 实体全部回收后释放三个 per-CPU 容器，并清全局层级指针防止重入。 */
		free_percpu(sdd->sd);
		sdd->sd = NULL;
		free_percpu(sdd->sg);
		sdd->sg = NULL;
		free_percpu(sdd->sgc);
		sdd->sgc = NULL;
	}
}

/* 分配可被同 span domain 共享的状态，引用计数在认领时建立。 */
/*
 * 业务背景：同 span domain 需候选 sched_domain_shared 状态，随后按共享标志认领。
 * 入参：d 是不可空、调用者拥有的输入/输出分配集；cpu_map 是不可空只读 CPU mask。
 * 出参/返回：成功返回 0 并通过 d->sds 输出 per-CPU 对象；失败返回 -ENOMEM。
 * 注意事项：可睡眠；失败时保留部分分配，必须交给 __sds_free() 回滚，ref 尚未建立。
 */
static int __sds_alloc(struct s_data *d, const struct cpumask *cpu_map)
{
	int j;

	/* per-CPU 指针容器先分配，部分实体失败时由 __sds_free() 扫同一 cpu_map 回滚。 */
	d->sds = alloc_percpu(struct sched_domain_shared *);
	if (!d->sds)
		return -ENOMEM;

	/* shared 对象按 CPU node 就近分配，引用和 alloc_flags 保持零初始直到被认领。 */
	for_each_cpu(j, cpu_map) {
		struct sched_domain_shared *sds;

		sds = kzalloc_node(sizeof(struct sched_domain_shared),
				GFP_KERNEL, cpu_to_node(j));
		if (!sds)
			return -ENOMEM;

		*per_cpu_ptr(d->sds, j) = sds;
	}

	return 0;
}

/* 仅回收临时数组中仍有所有权的 shared 对象。 */
/*
 * 业务背景：分配回滚/提交收尾需回收 d->sds 中仍归临时集所有的 shared 对象。
 * 入参：d 是不可空输入/输出分配集；cpu_map 是不可空只读 CPU mask，须与分配时一致。
 * 出参/返回：无直接返回值、无输出参；释放各槽和 per-CPU 容器并将 d->sds 置 NULL。
 * 注意事项：d->sds 为 NULL 时可重入返回；已 claim 槽必须先置 NULL，否则可造成 UAF。
 */
static void __sds_free(struct s_data *d, const struct cpumask *cpu_map)
{
	int j;

	if (!d->sds)
		return;

	/* claim_allocations() 已将已发布槽置 NULL，kfree(NULL) 使这里可统一遍历。 */
	for_each_cpu(j, cpu_map)
		kfree(*per_cpu_ptr(d->sds, j));

	free_percpu(d->sds);
	d->sds = NULL;
}

/* 初始化单层并连接父子；架构 mask 越界时扩大父 span 以维持包含不变量。 */
/*
 * 业务背景：每个 CPU 的 topology level 需初始化为 sched_domain，并连接已建的下层 child。
 * 入参：tl/cpu_map 是不可空只读借用；attr 是可空只读属性；child 是可空借用子 domain；cpu 是有效目标 CPU。
 * 出参/返回：返回预分配对象上已初始化的非空 domain；同时输出 child->parent 和层级/flags 更新。
 * 注意事项：构建阶段需独占；架构 mask 违反包含关系时会报错并扩大父 span，不返回错误指针。
 */
static struct sched_domain *build_sched_domain(struct sched_domain_topology_level *tl,
		const struct cpumask *cpu_map, struct sched_domain_attr *attr,
		struct sched_domain *child, int cpu)
{
	struct sched_domain *sd = sd_init(tl, cpu_map, child, cpu);

	/* 有 child 才需继承层级、建双向链，并检查父 span 覆盖子 span。 */
	if (child) {
		sd->level = child->level + 1;
		sched_domain_level_max = max(sched_domain_level_max, sd->level);
		child->parent = sd;

		/* 架构 mask 违反包含关系时不丢弃 CPU，而是扩大父 span 并记录缺陷。 */
		if (!cpumask_subset(sched_domain_span(child),
				    sched_domain_span(sd))) {
			pr_err("BUG: arch topology borken\n");
			pr_err("     the %s domain not a subset of the %s domain\n",
					child->name, sd->name);
			/* Fixup, ensure @sd has at least @child CPUs. */
			cpumask_or(sched_domain_span(sd),
				   sched_domain_span(sd),
				   sched_domain_span(child));
		}

	}
	/* 层级和 span 完成后再应用 relax 属性，因为它依赖 sd->level。 */
	set_domain_attribute(sd, attr);

	return sd;
}

/*
 * Ensure topology masks are sane, i.e. there are no conflicts (overlaps) for
 * any two given CPUs on non-NUMA topology levels.
 */
/* 非 NUMA mask 必须相等或不相交，否则共享 group 的环形链接会互相覆盖。 */
/*
 * 业务背景：非 NUMA topology mask 若部分重叠，后续构建会破坏共享 sched_group 环链。
 * 入参：cpu_map 是不可空只读借用的构建 CPU mask，不保存、不修改。
 * 出参/返回：所有非 NUMA 层 mask 两两相等或不相交返回 true，发现部分重叠返回 false；无输出参。
 * 注意事项：必须已持 sched_domains_mutex；使用全局临时 mask，不可并发调用。
 */
static bool topology_span_sane(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	struct cpumask *covered, *id_seen;
	int cpu;

	lockdep_assert_held(&sched_domains_mutex);
	covered = sched_domains_tmpmask;
	id_seen = sched_domains_tmpmask2;

	/* 每个非 NUMA 层独立清空 scratch，covered 累加 span，id_seen 记录已见 canonical id。 */
	for_each_sd_topology(tl) {
		int tl_common_flags = 0;

		/* 层级公共 flags 只需计算一次，NUMA 层显式允许重叠并跳过后续检查。 */
		if (tl->sd_flags)
			tl_common_flags = (*tl->sd_flags)();

		/* NUMA levels are allowed to overlap */
		if (tl_common_flags & SD_NUMA)
			continue;

		cpumask_clear(covered);
		cpumask_clear(id_seen);

		/*
		 * Non-NUMA levels cannot partially overlap - they must be either
		 * completely equal or completely disjoint. Otherwise we can end up
		 * breaking the sched_group lists - i.e. a later get_group() pass
		 * breaks the linking done for an earlier span.
		 */
		/* 对分区中每个 CPU 取架构 mask，其最低 bit 是该等价 span 的 canonical id。 */
		for_each_cpu(cpu, cpu_map) {
			const struct cpumask *tl_cpu_mask = tl->mask(tl, cpu);
			int id;

			/* lowest bit set in this mask is used as a unique id */
			id = cpumask_first(tl_cpu_mask);

			/* id 已见时 mask 必须完全相等；新 id 则必须与所有已覆盖 span 不相交。 */
			if (cpumask_test_cpu(id, id_seen)) {
				/* First CPU has already been seen, ensure identical spans */
				if (!cpumask_equal(tl->mask(tl, id), tl_cpu_mask))
					return false;
			} else {
				/* First CPU hasn't been seen before, ensure it's a completely new span */
				if (cpumask_intersects(tl_cpu_mask, covered))
					return false;

				/* 新等价类通过后同时记录其全 span 和 canonical id，供后续 CPU 比对。 */
				cpumask_or(covered, covered, tl_cpu_mask);
				cpumask_set_cpu(id, id_seen);
			}
		}
	}
	return true;
}

/*
 * Calculate an allowed NUMA imbalance such that LLCs do not get
 * imbalanced.
 */
/* 由每节点 LLC 数推导可容忍任务差，并按更高 NUMA span 比例放大。 */
/*
 * 业务背景：NUMA 负载均衡需允许适度不均，以避免一侧 LLC 提前共享而另一侧仍空闲。
 * 入参：sd_llc 是不可空输入/输出 LLC domain，必须含 SD_SHARE_LLC 且存在 parent。
 * 出参/返回：无直接返回值、无输出参；原地更新父链各层 imb_numa_nr。
 * 注意事项：只能在 domain 未发布的串行构建阶段调用；前置不满足仅 WARN，继续解引用可崩溃。
 */
static void adjust_numa_imbalance(struct sched_domain *sd_llc)
{
	struct sched_domain *parent;
	unsigned int imb_span = 1;
	unsigned int imb = 0;
	unsigned int nr_llcs;

	/* 前置 WARN 用于拓扑构建调用约束，本算法依赖 parent 权重做除法。 */
	WARN_ON(!(sd_llc->flags & SD_SHARE_LLC));
	WARN_ON(!sd_llc->parent);

	/*
	 * For a single LLC per node, allow an
	 * imbalance up to 12.5% of the node. This is
	 * arbitrary cutoff based two factors -- SMT and
	 * memory channels. For SMT-2, the intent is to
	 * avoid premature sharing of HT resources but
	 * SMT-4 or SMT-8 *may* benefit from a different
	 * cutoff. For memory channels, this is a very
	 * rough estimate of how many channels may be
	 * active and is based on recent CPUs with
	 * many cores.
	 *
	 * For multiple LLCs, allow an imbalance
	 * until multiple tasks would share an LLC
	 * on one node while LLCs on another node
	 * remain idle. This assumes that there are
	 * enough logical CPUs per LLC to avoid SMT
	 * 中文补充：单 LLC/node 时允许约 12.5% node CPU 差，多 LLC 时以 LLC 数为阈值避免一侧提前共享。
	 * factors and that there is a correlation
	 * between LLCs and memory channels.
	 */
	/* parent/LLC span 权重比值近似一个 node 内的 LLC 数量。 */
	nr_llcs = sd_llc->parent->span_weight / sd_llc->span_weight;
	if (nr_llcs == 1)
		imb = sd_llc->parent->span_weight >> 3;
	else
		imb = nr_llcs;

	/* 至少允许 1 个任务差，并先写入 LLC 直接 parent。 */
	imb = max(1U, imb);
	sd_llc->parent->imb_numa_nr = imb;

	/*
	 * Set span based on the first NUMA domain.
	 *
	 * NUMA systems always add a NODE domain before
	 * iterating the NUMA domains. Since this is before
	 * degeneration, start from sd_llc's parent's
	 * parent which is the lowest an SD_NUMA domain can
	 * 中文补充：从 LLC parent 的 parent 开始找第一个 NUMA 层，用其 span 作为更高层阈值缩放基准。
	 * be relative to sd_llc.
	 */
	/* NODE 层可介于 LLC 与首个 SD_NUMA 之间，遍历跳过所有非 NUMA 父层。 */
	parent = sd_llc->parent->parent;
	while (parent && !(parent->flags & SD_NUMA))
		parent = parent->parent;

	imb_span = parent ? parent->span_weight : sd_llc->parent->span_weight;

	/* Update the upper remainder of the topology */
	/* 从直接 parent 到顶层按 span 倍数放大 imbalance，但 factor 不小于 1。 */
	parent = sd_llc->parent;
	while (parent) {
		int factor = max(1U, (parent->span_weight / imb_span));

		parent->imb_numa_nr = imb * factor;
		parent = parent->parent;
	}
}

/* 为相同语义的 domain 复用 shared 状态；ref 决定最终销毁时机。 */
/*
 * 业务背景：相同 span/语义的 domain 需复用 sched_domain_shared，以聚合空闲 CPU 等统计。
 * 入参：d 是不可空只读分配集；sd 是不可空输入/输出 domain；flags 是单一 SD_* 共享用途标志。
 * 出参/返回：无直接返回值、无输出参；设置 sd->shared、alloc_flags，初始化 busy 计数并增加 ref。
 * 注意事项：只在串行构建阶段调用；无匹配候选时 WARN 并降级使用最后一个，销毁依赖引用计数。
 */
static void
init_sched_domain_shared(struct s_data *d, struct sched_domain *sd, int flags)
{
	struct sched_domain_shared *sds = NULL;
	int cpu;

	/* 一个 canonical CPU 槽可被 LLC 和 asym 语义同时候选，alloc_flags 防止不同用途误共享。 */
	/*
	 * Multiple domains can try to claim a shared object like
	 * SD_ASYM_CPUCAPACITY and SD_SHARE_LLC which can alias to
	 * same cpumask_first(sched_domain_span(sd)) CPU and can
	 * cause "nr_idle_scan" to be populated incorrectly during
	 * load balancing.
	 *
	 * Find the first CPU in sched_domain_span(sd) with an
	 * unclaimed domain (!alloc_flags) or where the alloc_flag
	 * matches the requested flag (SD_* flag)
	 *
	 * If the domain only has single CPU, allow temporary overlap
	 * in allocation since the domains will be degenerated later.
	 */
	/* 遍历 span 找未认领槽或已被同一 flags 认领的槽；单 CPU domain 允许临时重叠。 */
	for_each_cpu(cpu, sched_domain_span(sd)) {
		sds = *per_cpu_ptr(d->sds, cpu);

		if (!sds->alloc_flags ||
		    sd->span_weight == 1 ||
		    sds->alloc_flags == flags) {
			sds->alloc_flags = flags;
			sd->shared = sds;
			break;
		}
	}

	/*
	 * Use the sd_shared corresponding to the last
	 * CPU in the span if none are avaialable.
	 */
	/* 理论上应总能命中；若不能，退回最后一个槽以避免后续 NULL 解引用。 */
	if (WARN_ON_ONCE(!sd->shared))
		sd->shared = sds;

	/*
	 * nr_busy_cpus is consumed only by the NOHZ kick path via
	 * sd_balance_shared; on the asym-capacity path it is initialized but
	 * never read.
	 */
	/* 初始 busy CPU 数取整个 span 权重，引用计数在字段完整初始化后最后增加。 */
	atomic_set(&sd->shared->nr_busy_cpus, sd->span_weight);
	atomic_inc(&sd->shared->ref);
}

/*
 * For asymmetric CPU capacity, attach sched_domain_shared on the innermost
 * SD_ASYM_CPUCAPACITY_FULL ancestor of @cpu's base domain when that ancestor is
 * not an overlapping NUMA-built domain (then LLC should claim shared).
 *
 * A CPU may lack any FULL ancestor (e.g., exclusive cpuset symmetric island),
 * then LLC must claim shared instead.
 *
 * Note: SD_ASYM_CPUCAPACITY_FULL is only set when all CPU capacity values
 * are present in the domain span, so the asym domain we attach to cannot
 * degenerate into a single-capacity group. The relevant edge cases are instead
 * covered by the caveats above.
 *
 * Return true if this CPU's asym path claimed sd->shared, false otherwise.
 */
/*
 * 业务背景：异构 CPU 需在最内层完整容量 domain 上认领 shared 状态，NUMA 重叠层则留给 LLC 路径。
 * 入参：d 是不可空输入/输出分配集；cpu 是属于 d->sd 已构建集的有效 CPU 编号。
 * 出参/返回：成功绑定 shared 并增加引用返回 true；无 base/FULL domain 或命中 NUMA 层返回 false，无独立输出参。
 * 注意事项：仅在串行构建阶段调用；返回 false 不是错误，表示后续 LLC 应负责认领。
 */
static bool claim_asym_sched_domain_shared(struct s_data *d, int cpu)
{
	struct sched_domain *sd = *per_cpu_ptr(d->sd, cpu);
	struct sched_domain *sd_asym;

	/* 分区可能不含任何 domain，这是正常的“无 asym shared”结果。 */
	if (!sd)
		return false;

	/* 从 base domain 向上找最内层 FULL，它首次覆盖分区中所有容量类别。 */
	sd_asym = sd;
	while (sd_asym && !(sd_asym->flags & SD_ASYM_CPUCAPACITY_FULL))
		sd_asym = sd_asym->parent;

	/* 没有 FULL 或 FULL 是重叠 NUMA 时，shared 应由后续 LLC 路径认领。 */
	if (!sd_asym || (sd_asym->flags & SD_NUMA))
		return false;

	init_sched_domain_shared(d, sd_asym, SD_ASYM_CPUCAPACITY);
	return true;
}

/* 在 mutex 下分配最小空闲 LLC id，并维护计数数组所需的最大上界。 */
/*
 * 业务背景：每个在线 LLC 需紧凑 id，以索引全局 cache 计数数组。
 * 入参：无。
 * 出参/返回：返回 [0,nr_cpu_ids) 的最小空闲 id；空间耗尽返回 -1；同时输出 allocmask/max_lid 更新。
 * 注意事项：必须已持 sched_domains_mutex；函数不分配内存、不睡眠，-1 不会改变全局状态。
 */
static int __sched_domains_alloc_llc_id(void)
{
	int lid, max;

	lockdep_assert_held(&sched_domains_mutex);

	/* allocmask 中第一个零 bit 是最小可复用 id，上界不允许超过 possible CPU 数。 */
	lid = cpumask_first_zero(sched_domains_llc_id_allocmask);
	/*
	 * llc_id space should never grow larger than the
	 * possible number of CPUs in the system.
	 */
	if (lid >= nr_cpu_ids)
		return -1;

	/* 标记占用后重算最高 id，只在新 id 扩大范围时更新 max_lid。 */
	__cpumask_set_cpu(lid, sched_domains_llc_id_allocmask);
	max = cpumask_last(sched_domains_llc_id_allocmask);
	if (max > max_lid)
		max_lid = max;

	return lid;
}

/* 仅最后一个在线 LLC 兄弟离开时归还 id，随后收缩 max_lid。 */
/*
 * 业务背景：CPU 脱离 domain 时，只有当它是 LLC 中最后的 id 持有者才能归还该 id。
 * 入参：cpu 是小于 nr_cpu_ids 的有效 CPU 编号、仅输入。
 * 出参/返回：无直接返回值、无输出参；清空该 CPU id，必要时归还 allocmask bit 并收缩 max_lid。
 * 注意事项：必须已持 sched_domains_mutex；id 无效或仍有在线 LLC sibling 持有时提前返回。
 */
static void __sched_domains_free_llc_id(int cpu)
{
	int i, lid, max;

	lockdep_assert_held(&sched_domains_mutex);

	/* 无 id 或异常越界值都视为无可回收资源，不修改 allocmask。 */
	lid = per_cpu(sd_llc_id, cpu);
	if (lid == -1 || lid >= nr_cpu_ids)
		return;

	/* 先清当前 CPU 持有记录，再扫描其 LLC siblings 是否仍共用该 id。 */
	per_cpu(sd_llc_id, cpu) = -1;

	for_each_cpu(i, llc_mask(cpu)) {
		/* An online CPU owns the llc_id. */
		if (per_cpu(sd_llc_id, i) == lid)
			return;
	}

	/* 没有 sibling 持有才真正归还 bit，然后可能收缩计数数组所需的最高索引。 */
	__cpumask_clear_cpu(lid, sched_domains_llc_id_allocmask);

	max = cpumask_last(sched_domains_llc_id_allocmask);
	/* shrink max lid to save memory */
	if (max < max_lid)
		max_lid = max;
}

/* 对外入口串行化 LLC id 回收。 */
/*
 * 业务背景：对外的 LLC id 回收需统一与 domain 构建/分区替换串行。
 * 入参：cpu 是小于 nr_cpu_ids 的有效 CPU 编号、仅输入。
 * 出参/返回：无直接返回值、无输出参；在 mutex 下尝试归还 cpu 的 LLC id。
 * 注意事项：内部获取 sched_domains_mutex，不得在已持有该 mutex 时调用；可因 mutex 竞用睡眠。
 */
void sched_domains_free_llc_id(int cpu)
{
	sched_domains_mutex_lock();
	__sched_domains_free_llc_id(cpu);
	sched_domains_mutex_unlock();
}

/*
 * Build sched domains for a given set of CPUs and attach the sched domains
 * to the individual CPUs
 */
/* 分阶段构造 domain/group/capacity，完成后在 RCU 下接入各 rq；失败统一回滚。 */
/*
 * 业务背景：一个调度分区需事务式构建 domain/group/capacity/shared/root-domain 并接入各 runqueue。
 * 入参：cpu_map 是不可空且非空只读 CPU mask；attr 是可空只读分区属性；multi_llcs 是不可空 bool 输出槽。
 * 出参/返回：成功返回 0，分配/拓扑失败返回 -ENOMEM；始终写 *multi_llcs 表示已检测的多 LLC 状态。
 * 注意事项：必须在 hotplug/domains_mutex 串行上下文，可睡眠；接入 rq 使用 RCU，错误路径按 alloc_state 回滚未发布资源。
 */
static int
build_sched_domains(const struct cpumask *cpu_map, struct sched_domain_attr *attr,
		    bool *multi_llcs)
{
	enum s_alloc alloc_state = sa_none;
	bool has_multi_llcs = false;
	struct sched_domain *sd;
	struct s_data d;
	struct rq *rq = NULL;
	/* ret 默认为分配失败，仅全部提交成功后改写；两个 bool 用于最终 static key 计数。 */
	int i, ret = -ENOMEM;
	bool has_asym = false;
	bool has_cluster = false;

	/* 空分区无法为 cpumask_first 等操作提供有效 CPU，作为内部调用错误走统一回滚。 */
	if (WARN_ON(cpumask_empty(cpu_map)))
		goto error;

	/* 先完成全部预分配，后续构建才能假定每层/CPU 槽存在。 */
	alloc_state = __visit_domain_allocation_hell(&d, cpu_map);
	if (alloc_state != sa_rootdomain)
		goto error;

	/* 逐 CPU 自底向上构建 domain 链，达到整个分区 span 后可提前终止更高层。 */
	/* Set up domains for CPUs specified by the cpu_map: */
	for_each_cpu(i, cpu_map) {
		struct sched_domain_topology_level *tl;
		int lid;

		/* NULL child 表示底层，每次 sd_init 返回的层成为下一轮 child。 */
		sd = NULL;
		for_each_sd_topology(tl) {

			sd = build_sched_domain(tl, cpu_map, attr, sd, i);

			/* 分区级 has_asym 汇总任一层容量非对称，用于稍后认领 shared/static key。 */
			has_asym |= sd->flags & SD_ASYM_CPUCAPACITY;

			/* 只将最底层保存为 per-CPU 链首，parent 链保留其余层。 */
			if (tl == sched_domain_topology)
				*per_cpu_ptr(d.sd, i) = sd;
			if (cpumask_equal(cpu_map, sched_domain_span(sd)))
				break;
		}

		/* 新 CPU 优先复用任一 LLC sibling 的 id，确保同缓存域只占一个全局 bit。 */
		lid = per_cpu(sd_llc_id, i);
		if (lid == -1) {
			/* try to reuse the llc_id of its siblings */
			/* 扫描完整架构 LLC mask，跳过本 CPU，只接受已分配的 sibling id。 */
			for (int j = cpumask_first(llc_mask(i));
			     j < nr_cpu_ids;
			     j = cpumask_next(j, llc_mask(i))) {
				if (i == j)
					continue;

				/* 只读取 sibling 已有 id，-1 表示该 sibling 尚未建立/认领 LLC。 */
				lid = per_cpu(sd_llc_id, j);

				if (lid != -1) {
					per_cpu(sd_llc_id, i) = lid;

					break;
				}
			}

			/* 所有 sibling 都无 id 才证明这是新 LLC，此时分配新全局 id。 */
			/* a new LLC is detected */
			if (lid == -1)
				per_cpu(sd_llc_id, i) = __sched_domains_alloc_llc_id();
		}
	}

	/* domain 链全部存在后审计非 NUMA masks，防止后续 group 环在部分重叠下被覆盖。 */
	if (WARN_ON(!topology_span_sane(cpu_map)))
		goto error;

	/* 逐 CPU/逐层构建 groups：NUMA 使用私有重叠环，树形层使用共享环。 */
	/* Build the groups for the domains */
	for_each_cpu(i, cpu_map) {
		for (sd = *per_cpu_ptr(d.sd, i); sd; sd = sd->parent) {
			sd->span_weight = cpumask_weight(sched_domain_span(sd));
			/* span_weight 必须在 group/shared/imbalance 初始化之前就绪。 */
			if (sd->flags & SD_NUMA) {
				if (build_overlap_sched_groups(sd, i))
					goto error;
			} else {
				if (build_sched_groups(sd, i))
					goto error;
			}
		}
	}

	/* groups 完成后为 asym 和最高 LLC 层认领 sched_domain_shared。 */
	for_each_cpu(i, cpu_map) {
		sd = *per_cpu_ptr(d.sd, i);
		if (!sd)
			continue;

		/* asym 认领可能返回 false，此时由下面 LLC 路径保底认领 shared。 */
		if (has_asym)
			claim_asym_sched_domain_shared(&d, i);

		/* First, find the topmost SD_SHARE_LLC domain */
		/* 连续 SHARE_LLC 层中只认领最高一层，其 span 代表完整 LLC 均衡边界。 */
		while (sd->parent && (sd->parent->flags & SD_SHARE_LLC))
			sd = sd->parent;

		/* 认领 LLC shared 后，有更高层才需计算 NUMA imbalance 和 multi-LLC 状态。 */
		if (sd->flags & SD_SHARE_LLC) {
			init_sched_domain_shared(&d, sd, SD_SHARE_LLC);

			/*
			 * In presence of higher domains, adjust the
			 * NUMA imbalance stats for the hierarchy.
			 */
			if (sd->parent) {
				if (IS_ENABLED(CONFIG_NUMA))
					adjust_numa_imbalance(sd);

				if (sd_in_multi_llcs(sd))
					has_multi_llcs = true;
			}
		}
	}

	/* 容量从高 CPU id 向低遍历，claim 先转移 ownership，再逐层更新共享 group capacity。 */
	/* Calculate CPU capacity for physical packages and nodes */
	for (i = nr_cpumask_bits-1; i >= 0; i--) {
		if (!cpumask_test_cpu(i, cpu_map))
			continue;

		claim_allocations(i, &d);

		for (sd = *per_cpu_ptr(d.sd, i); sd; sd = sd->parent)
			init_sched_groups_capacity(i, sd);
	}

	/* groups/capacity 完成后才为底层分配 cache-aware 计数数组；失败仅使该优化不可用。 */
	alloc_sd_llc(cpu_map, &d);

	/* 接入阶段持 RCU 读锁，使 cpu_attach_domain() 中读取/替换旧链的过渡安全。 */
	/* Attach the domains */
	rcu_read_lock();
	for_each_cpu(i, cpu_map) {
		rq = cpu_rq(i);
		sd = *per_cpu_ptr(d.sd, i);

		/* 每个 rq 转移到同一新 root-domain，同时汇总是否存在 cluster 层。 */
		cpu_attach_domain(sd, d.rd, i);

		if (lowest_flag_domain(i, SD_CLUSTER))
			has_cluster = true;
	}
	rcu_read_unlock();

	/* per-CPU 指针全部发布后才增全局 static key 计数，热路径不会提前进入。 */
	if (has_asym)
		static_branch_inc_cpuslocked(&sched_asym_cpucapacity);

	if (has_cluster)
		static_branch_inc_cpuslocked(&sched_cluster_active);

	if (rq && sched_debug_verbose)
		pr_info("root domain span: %*pbl\n", cpumask_pr_args(cpu_map));

	/* 只有经过全部接入与 static key 发布才把 ret 改为成功。 */
	ret = 0;
error:
	/* 无论成功失败都写输出槽，并按 alloc_state 回收仍归临时集的对象。 */
	*multi_llcs = has_multi_llcs;
	__free_domain_allocs(&d, alloc_state, cpu_map);

	return ret;
}

/* Current sched domains: */
static cpumask_var_t			*doms_cur;

/* Number of sched domains in 'doms_cur': */
static int				ndoms_cur;

/* Attributes of custom domains in 'doms_cur' */
static struct sched_domain_attr		*dattr_cur;

/*
 * Special case: If a kmalloc() of a doms_cur partition (array of
 * cpumask) fails, then fallback to a single sched domain,
 * as determined by the single cpumask fallback_doms.
 */
static cpumask_var_t			fallback_doms;

/*
 * arch_update_cpu_topology lets virtualized architectures update the
 * CPU core maps. It is supposed to return 1 if the topology changed
 * or 0 if it stayed the same.
 */
/* 默认架构无需刷新拓扑；返回非零会强制现有分区全部重建。 */
/*
 * 业务背景：虚拟化架构可在 domain 重建前刷新 CPU core 映射，默认架构无需处理。
 * 入参：无。
 * 出参/返回：默认返回 0 表示拓扑未变；架构覆盖实现应以非零表示已变，无输出参。
 * 注意事项：弱符号可被架构替换；调用者将非零视为强制全量重建，具体锁/睡眠约束由覆盖实现遵守。
 */
int __weak arch_update_cpu_topology(void)
{
	return 0;
}

/* 分配分区 mask 数组；中途失败释放此前条目并返回 NULL。 */
/*
 * 业务背景：分区重建需要一个拥有 ndoms 个可变 cpumask 的数组。
 * 入参：ndoms 是要分配的 mask 数量，范围受 size_t/内存限制，仅输入。
 * 出参/返回：成功返回调用者拥有的 cpumask_var_t 数组；任一分配失败返回 NULL，无输出参。
 * 注意事项：GFP_KERNEL 可睡眠；失败会自行释放已成功条目，成功结果必须交给 free_sched_domains()。
 */
cpumask_var_t *alloc_sched_domains(unsigned int ndoms)
{
	int i;
	cpumask_var_t *doms;

	/* 外层数组只存放 cpumask_var_t 句柄，每个实际 mask 在循环中单独分配。 */
	doms = kmalloc_objs(*doms, ndoms);
	if (!doms)
		return NULL;
	/* 第 i 个失败时只有 [0,i) 已初始化，因此将 i 作为精确回滚计数。 */
	for (i = 0; i < ndoms; i++) {
		if (!alloc_cpumask_var(&doms[i], GFP_KERNEL)) {
			free_sched_domains(doms, i);
			return NULL;
		}
	}
	return doms;
}

/* 释放由 alloc_sched_domains() 创建的整个分区数组。 */
/*
 * 业务背景：分区数组替换或分配失败回滚时需统一释放各 cpumask 及容器。
 * 入参：doms 是 alloc_sched_domains() 返回的不可空所有权指针；ndoms 是已成功初始化的元素数。
 * 出参/返回：无直接返回值、无输出参；消耗并释放 doms 的全部所有权。
 * 注意事项：ndoms 不得是静态 fallback_doms，ndoms 不得超过已分配元素数；返回后指针失效。
 */
void free_sched_domains(cpumask_var_t doms[], unsigned int ndoms)
{
	unsigned int i;
	/* 先释放每个动态 mask，最后再释放存放句柄的外层数组。 */
	for (i = 0; i < ndoms; i++)
		free_cpumask_var(doms[i]);
	kfree(doms);
}

/*
 * Set up scheduler domains and groups.  For now this just excludes isolated
 * CPUs, but could be used to exclude other special cases in the future.
 */
/* 初始化全局临时 mask 和首个 housekeeping 分区，再发布缓存感知状态。 */
/*
 * 业务背景：SMP 调度初始化需建立排除 isolated CPU 的首个 housekeeping domain 分区。
 * 入参：cpu_map 是不可空只读借用的可用 CPU mask，仅在 __init 阶段读取。
 * 出参/返回：成功返回 0；domain 构建失败返回其负 errno；输出全局临时 mask、doms_cur 和 cache 状态。
 * 注意事项：仅调用一次且可因 GFP_KERNEL 分配睡眠；分区数组分配失败降级为静态 fallback_doms。
 */
int __init sched_init_domains(const struct cpumask *cpu_map)
{
	bool multi_llcs;
	int err;

	/* 全局 scratch/fallback masks 在任何 domain 构建之前分配，后续路径假定它们永久存在。 */
	zalloc_cpumask_var(&sched_domains_llc_id_allocmask, GFP_KERNEL);
	zalloc_cpumask_var(&sched_domains_tmpmask, GFP_KERNEL);
	zalloc_cpumask_var(&sched_domains_tmpmask2, GFP_KERNEL);
	zalloc_cpumask_var(&fallback_doms, GFP_KERNEL);

	/* 先刷新架构 CPU 映射和容量类别，再据此创建首个分区。 */
	arch_update_cpu_topology();
	asym_cpu_capacity_scan();
	ndoms_cur = 1;
	/* 动态数组失败时使用静态 fallback 句柄，仍可构建一个 housekeeping domain。 */
	doms_cur = alloc_sched_domains(ndoms_cur);
	if (!doms_cur)
		doms_cur = &fallback_doms;
	cpumask_and(doms_cur[0], cpu_map, housekeeping_cpumask(HK_TYPE_DOMAIN));
	/* 首个分区使用默认属性，只在构建成功后才发布 cache-aware static keys。 */
	err = build_sched_domains(doms_cur[0], NULL, &multi_llcs);
	if (!err)
		sched_cache_set(multi_llcs);

	return err;
}

/*
 * Detach sched domains from a group of CPUs specified in cpu_map
 * These CPUs will now be attached to the NULL domain
 */
/* 先修正静态键计数，再把 CPU 接回默认 root_domain；旧链由 RCU 延迟销毁。 */
/*
 * 业务背景：删除分区前要把其 CPU 脱离旧 domain，并重新接入默认 root_domain。
 * 入参：cpu_map 是不可空且非空的只读借用 CPU mask，其 CPU 应共享同一旧 domain。
 * 出参/返回：无直接返回值、无输出参；更新静态键计数并将各 rq 指向默认 root-domain。
 * 注意事项：需 CPU hotplug 和 sched_domains_mutex 串行；cpumask 为空会以无效 CPU 访问 per-CPU 数据，旧链通过 RCU 延迟销毁。
 */
static void detach_destroy_domains(const struct cpumask *cpu_map)
{
	unsigned int cpu = cpumask_any(cpu_map);
	int i;

	/* 分区内任一 CPU 的快捷指针可代表该分区是否曾增加 asym static key。 */
	if (rcu_access_pointer(per_cpu(sd_asym_cpucapacity, cpu)))
		static_branch_dec_cpuslocked(&sched_asym_cpucapacity);

	/* cluster key 在每个含 cluster 的分区构建时增加，删除分区时对称减少。 */
	if (static_branch_unlikely(&sched_cluster_active))
		static_branch_dec_cpuslocked(&sched_cluster_active);

	/* 逐 CPU 接入 NULL domain/默认 rd，旧链的真正回收交给 cpu_attach_domain() 的 RCU callback。 */
	rcu_read_lock();
	for_each_cpu(i, cpu_map)
		cpu_attach_domain(NULL, &def_root_domain, i);
	rcu_read_unlock();
}

/* handle null as "default" */
/* NULL 属性等价于 SD_ATTR_INIT，用于判断现有分区是否可以复用。 */
/*
 * 业务背景：分区差量重建需比较新旧 sched_domain_attr，NULL 应与默认属性等价。
 * 入参：cur/new 是可空只读属性数组；idx_cur/idx_new 是各自数组内的非负有效下标，对 NULL 数组不使用。
 * 出参/返回：字节级相等返回 1，不等返回 0；无输出参。
 * 注意事项：NULL 按 SD_ATTR_INIT 临时值比较；函数不检查下标越界，调用者必须保证数组稳定。
 */
static int dattrs_equal(struct sched_domain_attr *cur, int idx_cur,
			struct sched_domain_attr *new, int idx_new)
{
	struct sched_domain_attr tmp;

	/* 两个 NULL 不需构造默认对象就可直接判等。 */
	/* Fast path: */
	if (!new && !cur)
		return 1;

	/* 任一 NULL 以栈上 SD_ATTR_INIT 代替，非 NULL 则按下标选对应元素。 */
	tmp = SD_ATTR_INIT;

	return !memcmp(cur ? (cur + idx_cur) : &tmp,
			new ? (new + idx_new) : &tmp,
			sizeof(struct sched_domain_attr));
}

/*
 * Partition sched domains as specified by the 'ndoms_new'
 * cpumasks in the array doms_new[] of cpumasks. This compares
 * doms_new[] to the current sched domain partitioning, doms_cur[].
 * It destroys each deleted domain and builds each new domain.
 *
 * 'doms_new' is an array of cpumask_var_t's of length 'ndoms_new'.
 * The masks don't intersect (don't overlap.) We should setup one
 * sched domain for each mask. CPUs not in any of the cpumasks will
 * not be load balanced. If the same cpumask appears both in the
 * current 'doms_cur' domains and in the new 'doms_new', we can leave
 * it as it is.
 *
 * The passed in 'doms_new' should be allocated using
 * alloc_sched_domains.  This routine takes ownership of it and will
 * free_sched_domains it when done with it. If the caller failed the
 * alloc call, then it can pass in doms_new == NULL && ndoms_new == 1,
 * and partition_sched_domains() will fallback to the single partition
 * 'fallback_doms', it also forces the domains to be rebuilt.
 *
 * If doms_new == NULL it will be replaced with cpu_online_mask.
 * ndoms_new == 0 is a special case for destroying existing domains,
 * and it will not create the default domain.
 *
 * Call with hotplug lock and sched_domains_mutex held
 */
/* 热插拔锁和 domains_mutex 下做差量替换；函数接管 doms_new 的所有权。 */
/*
 * 业务背景：CPU/cpuset 变化时需差量复用未变分区、销毁删除分区并构建新分区。
 * 入参：ndoms_new 非空时是长度 ndoms_new 的不重叠 mask 数组并转移所有权；dattr_new 是可空、同长度属性数组并同样转移所有权。
 * 出参/返回：无直接返回值、无输出参；发布新 doms_cur/dattr_cur，更新 cache/EAS/debugfs/DL 状态。
 * 注意事项：调用者必须持 hotplug 锁和 sched_domains_mutex；doms_new==NULL 启用在线 CPU fallback，ndoms_new==0 仅销毁；可睡眠，转移后调用者不得释放数组。
 */
static void partition_sched_domains_locked(int ndoms_new, cpumask_var_t doms_new[],
				    struct sched_domain_attr *dattr_new)
{
	bool __maybe_unused has_eas = false;
	bool has_multi_llcs = false, multi_llcs;
	int i, j, n;
	int new_topology;

	/* 函数依赖调用者已建立的全局串行化，但 CPU hotplug 锁由更上层契约保证。 */
	lockdep_assert_held(&sched_domains_mutex);

	/* Let the architecture update CPU core mappings: */
	/* 架构报告拓扑改变时禁止所有新旧分区复用，并重扫容量类别。 */
	new_topology = arch_update_cpu_topology();
	/* Trigger rebuilding CPU capacity asymmetry data */
	if (new_topology)
		asym_cpu_capacity_scan();

	/* NULL 输入表示调用者无法分配新数组，先尝试内部创建单一 active-housekeeping 分区。 */
	if (!doms_new) {
		WARN_ON_ONCE(dattr_new);
		n = 0;
		doms_new = alloc_sched_domains(1);
		if (doms_new) {
			/* 内部 fallback 分区只包含 active 且允许参与 domain balance 的 housekeeping CPUs。 */
			n = 1;
			cpumask_and(doms_new[0], cpu_active_mask,
				    housekeeping_cpumask(HK_TYPE_DOMAIN));
		}
	} else {
		n = ndoms_new;
	}

	/* 第一遍对旧分区求差：只有 mask 和 attr 都相等且拓扑未变才保留。 */
	/* Destroy deleted domains: */
	for (i = 0; i < ndoms_cur; i++) {
		for (j = 0; j < n && !new_topology; j++) {
			if (cpumask_equal(doms_cur[i], doms_new[j]) &&
			    dattrs_equal(dattr_cur, i, dattr_new, j))
				goto match1;
		}
		/* 无匹配旧分区先脱离所有 CPU，但 doms_cur mask 本体保留到最后统一释放。 */
		/* No match - a current sched domain not in new doms_new[] */
		detach_destroy_domains(doms_cur[i]);
match1:
		;
	}

	n = ndoms_cur;
	/* 内部单分区分配也失败时，改用永久 fallback_doms 并将可复用旧分区数置 0。 */
	if (!doms_new) {
		n = 0;
		doms_new = &fallback_doms;
		cpumask_and(doms_new[0], cpu_active_mask,
			    housekeeping_cpumask(HK_TYPE_DOMAIN));
	}

	/* 第二遍对新分区求差：复用完全匹配项，其余逐个构建。 */
	/* Build new domains: */
	for (i = 0; i < ndoms_new; i++) {
		for (j = 0; j < n && !new_topology; j++) {
			if (cpumask_equal(doms_new[i], doms_cur[j]) &&
			    dattrs_equal(dattr_new, i, dattr_cur, j)) {
				/*
				 * Reused partition has to be taken care
				 * of here, because there could be a corner
				 * case that if the reused partition is skipped
				 * and only new partition is considered, an
				 * incorrect has_multi_llcs would be set. For
				 * example:
				 * If the only multi-LLC partition is reused
				 * and a new single-LLC partition is built,
				 * sched_cache_set(false) disables cache-aware
				 * scheduling globally despite the reused
				 * multi-LLC partition still being active.
				 * 中文补充：复用分区也必须重新汇总 multi-LLC，否则仅看新建分区会错误关闭全局 cache-aware 键。
				 */
				struct sched_domain *sd;
				int cpu = cpumask_first(doms_cur[j]);

				/* RCU 保护复用分区的已发布 domain 链，遍历到最高 LLC 层重算 multi-LLC。 */
				guard(rcu)();
				sd = rcu_dereference(cpu_rq(cpu)->sd);
				while (sd && sd->parent && (sd->parent->flags & SD_SHARE_LLC))
					sd = sd->parent;
				if (sd && (sd->flags & SD_SHARE_LLC) && sd->parent &&
				    sd_in_multi_llcs(sd))
					has_multi_llcs = true;
				goto match2;
			}
		}
		/* 无复用项时构建新分区，build 的 multi_llcs 输出无论成败都可用于汇总。 */
		/* No match - add a new doms_new */
		build_sched_domains(doms_new[i], dattr_new ? dattr_new + i : NULL,
				    &multi_llcs);
		has_multi_llcs |= multi_llcs;
match2:
		;
	}
	/* 所有新/复用分区已统计后，再一次性刷新全局 cache static keys。 */
	sched_cache_set(has_multi_llcs);

#if defined(CONFIG_ENERGY_MODEL) && defined(CONFIG_CPU_FREQ_GOV_SCHEDUTIL)
	/* EAS 同样对每个新分区复用旧 perf chain 或重建，sched_energy_update 可强制跳过复用。 */
	/* Build perf domains: */
	for (i = 0; i < ndoms_new; i++) {
		for (j = 0; j < n && !sched_energy_update; j++) {
			if (cpumask_equal(doms_new[i], doms_cur[j]) &&
			    cpu_rq(cpumask_first(doms_cur[j]))->rd->pd) {
				has_eas = true;
				goto match3;
			}
		}
		/* 无带现有 pd 的匹配 root-domain 时，为新 rd 构建并 RCU 发布 perf domains。 */
		/* No match - add perf domains for a new rd */
		has_eas |= build_perf_domains(doms_new[i]);
match3:
		;
	}
	sched_energy_set(has_eas);
#endif

	/* 新 topology 全部发布后才释放旧 mask/属性容器，静态 fallback 不可释放。 */
	/* Remember the new sched domains: */
	if (doms_cur != &fallback_doms)
		free_sched_domains(doms_cur, ndoms_cur);

	kfree(dattr_cur);
	/* 最后原子地更换记账指针/数量，输入 ownership 自此归全局当前状态。 */
	doms_cur = doms_new;
	dattr_cur = dattr_new;
	ndoms_cur = ndoms_new;

	/* debugfs 视图与 DL root-domain 记账在新分区稳定后统一重建。 */
	update_sched_domain_debugfs();
	dl_rebuild_rd_accounting();
}

/*
 * Call with hotplug lock held
 */
/* 公共入口负责 domains_mutex，调用者仍须按约定持有 CPU 热插拔读锁。 */
/*
 * 业务背景：对外分区替换 API 为 topology/domain 全局状态提供 sched_domains_mutex 串行化。
 * 入参：ndoms_new 非空时是长度 ndoms_new 的不重叠 mask 数组并转移所有权；dattr_new 是可空同长度属性数组并转移所有权。
 * 出参/返回：无直接返回值、无输出参；差量替换全局调度分区并接管输入数组。
 * 注意事项：调用者必须已持 CPU hotplug 锁，函数内部获取 sched_domains_mutex 且可睡眠；不得在已持该 mutex 时调用。
 */
void partition_sched_domains(int ndoms_new, cpumask_var_t doms_new[],
			     struct sched_domain_attr *dattr_new)
{
	sched_domains_mutex_lock();
	partition_sched_domains_locked(ndoms_new, doms_new, dattr_new);
	sched_domains_mutex_unlock();
}
