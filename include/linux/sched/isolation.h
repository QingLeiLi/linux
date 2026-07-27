#ifndef _LINUX_SCHED_ISOLATION_H
#define _LINUX_SCHED_ISOLATION_H

#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/tick.h>

/*
 * Housekeeping 类型描述的是“哪一类可迁移内核工作允许在哪些 CPU 上执行”。
 * 每个值也是 kernel/sched/isolation.c 中 flags/cpumasks 数组的位号和下标，
 * 因而 HK_TYPE_MAX 之前的真实类型必须保持紧凑。各掩码由全局 housekeeping
 * 对象持有，调用者通过本头文件接口借用只读视图，不拥有或释放它们。
 */
enum hk_type {
	/* Inverse of boot-time isolcpus= argument */
	/*
	 * 启动参数 isolcpus=domain CPU 列表的补集。它是启动时固定的调度域
	 * housekeeping 上界；运行期 cpuset isolated partition 不会修改这张掩码。
	 */
	HK_TYPE_DOMAIN_BOOT,
	/*
	 * Same as HK_TYPE_DOMAIN_BOOT but also includes the
	 * inverse of cpuset isolated partitions. As such it
	 * is always a subset of HK_TYPE_DOMAIN_BOOT.
	 */
	/*
	 * 初始等同 HK_TYPE_DOMAIN_BOOT，但还要扣除 cpuset isolated partition 的
	 * CPU，所以始终是 DOMAIN_BOOT 的子集。调度域、普通 kthread 回退亲和性和
	 * unbound workqueue 等运行期消费者主要查询这一动态类型。
	 */
	HK_TYPE_DOMAIN,
	/* Inverse of boot-time isolcpus=managed_irq argument */
	/*
	 * isolcpus=managed_irq CPU 列表的补集，只约束 managed interrupt 的自动
	 * 亲和性选择；它不等于禁止所有中断，也不参与 timer migration 层次。
	 */
	HK_TYPE_MANAGED_IRQ,
	/* Inverse of boot-time nohz_full= or isolcpus=nohz arguments */
	/*
	 * nohz_full=/isolcpus=nohz CPU 列表的补集，代表承接可卸载内核噪声的 CPU。
	 * 下面的 TICK/TIMER/RCU/MISC/WQ 别名共享这张掩码。
	 */
	HK_TYPE_KERNEL_NOISE,
	/* 真实类型数量，同时是数组边界和位图遍历上限，不代表可查询的工作类型。 */
	HK_TYPE_MAX,

	/*
	 * HK_TYPE_KTHREAD is now an alias of HK_TYPE_DOMAIN
	 */
	/*
	 * KTHREAD 现在是 DOMAIN 的别名：普通未绑定内核线程跟随动态调度域
	 * housekeeping 集合，cpuset partition 更新后可重新调整亲和性。
	 */
	HK_TYPE_KTHREAD = HK_TYPE_DOMAIN,

	/*
	 * The following housekeeping types are only set by the nohz_full
	 * boot commandline option. So they can share the same value.
	 */
	/*
	 * 以下类型只由 nohz_full 启动参数共同设置，因此无需各存一张相同 cpumask。
	 * 别名表达的是共同隔离策略；具体子系统仍各自负责真正迁移/卸载工作。
	 */
	HK_TYPE_TICK    = HK_TYPE_KERNEL_NOISE,
	HK_TYPE_TIMER   = HK_TYPE_KERNEL_NOISE,
	HK_TYPE_RCU     = HK_TYPE_KERNEL_NOISE,
	HK_TYPE_MISC    = HK_TYPE_KERNEL_NOISE,
	HK_TYPE_WQ      = HK_TYPE_KERNEL_NOISE,
};

#ifdef CONFIG_CPU_ISOLATION
/*
 * 启用 CPU isolation 时由 kernel/sched/isolation.c 提供真实实现：
 * static key 负责普通系统的热路径优化；查询函数返回借用掩码或策略布尔值；
 * housekeeping_update() 是 cpuset isolated partition 的运行期 DOMAIN 更新入口。
 */
DECLARE_STATIC_KEY_FALSE(housekeeping_overridden);
extern int housekeeping_any_cpu(enum hk_type type);
extern const struct cpumask *housekeeping_cpumask(enum hk_type type);
extern bool housekeeping_enabled(enum hk_type type);
extern void housekeeping_affine(struct task_struct *t, enum hk_type type);
extern bool housekeeping_test_cpu(int cpu, enum hk_type type);
extern int housekeeping_update(struct cpumask *isol_mask);
extern void __init housekeeping_init(void);

#else

/*
 * 未编译 CONFIG_CPU_ISOLATION 时的退化契约：没有 CPU 被策略排除，选择接口使用
 * 当前 CPU，掩码接口使用全部 possible CPU，更新和初始化均无副作用。用 inline
 * stub 保持调用者无需散布 #ifdef，也让编译器彻底消除隔离分支。
 */
/*
 * housekeeping_any_cpu() 的 stub 不检查 @type，返回当前 CPU；无状态变化，
 * 不睡眠。调用者仍需自行遵循 CPU hotplug/投递 API 的要求。
 */
static inline int housekeeping_any_cpu(enum hk_type type)
{
	return smp_processor_id();
}

/*
 * 返回全局永久存在的 cpu_possible_mask 借用指针，表示任意 possible CPU 都符合
 * housekeeping 策略；@type 仅为接口一致性保留，不得由调用者修改返回掩码。
 */
static inline const struct cpumask *housekeeping_cpumask(enum hk_type type)
{
	return cpu_possible_mask;
}

/* @type 未被任何隔离策略覆盖，故返回 false；无副作用、不睡眠。 */
static inline bool housekeeping_enabled(enum hk_type type)
{
	return false;
}

/*
 * 无隔离配置时无需收紧 @t 的亲和性。@t 与 @type 都是借用输入，本 stub 不读取
 * task 字段，也不取得引用或产生调度副作用。
 */
static inline void housekeeping_affine(struct task_struct *t,
				       enum hk_type type) { }

/* 任意有效 @cpu 都通过策略测试；这里只表达策略允许，不保证 CPU online。 */
static inline bool housekeeping_test_cpu(int cpu, enum hk_type type)
{
	return true;
}

/*
 * 没有动态 housekeeping 状态可更新，因而忽略借用的 @isol_mask 并报告成功；
 * housekeeping_init() 同理是无参数、无返回值、无副作用的启动期 stub。
 */
static inline int housekeeping_update(struct cpumask *isol_mask) { return 0; }
static inline void housekeeping_init(void) { }
#endif /* CONFIG_CPU_ISOLATION */

/*
 * housekeeping_cpu() - 查询 CPU 是否允许承担某类 housekeeping 工作。
 *
 * @cpu：待测 CPU 编号；@type：工作类型，二者均为纯输入。
 * CONFIG_CPU_ISOLATION=y 时，static key 未启用直接返回 true；启用后委托
 * housekeeping_test_cpu()。返回只描述策略成员关系，不固定 CPU 生命周期。
 * 函数用于调度/timer 等热路径，不睡眠、无状态副作用。
 */
static inline bool housekeeping_cpu(int cpu, enum hk_type type)
{
#ifdef CONFIG_CPU_ISOLATION
	/*
	 * jump-label 让默认 false 情况接近无成本；只有系统确实配置了某类隔离后，
	 * 才执行较慢的 flags 与 cpumask 查询。
	 */
	if (static_branch_unlikely(&housekeeping_overridden))
		return housekeeping_test_cpu(cpu, type);
#endif
	return true;
}

/*
 * cpu_is_isolated() - 判断 CPU 是否位于动态 DOMAIN housekeeping 集合之外。
 *
 * @cpu 为纯输入 CPU 编号。返回 true 只表示它不承担 DOMAIN 类工作；不能据此
 * 推断它同时属于 nohz_full、managed_irq 隔离，也不能推断 CPU 当前 online。
 */
static inline bool cpu_is_isolated(int cpu)
{
	return !housekeeping_test_cpu(cpu, HK_TYPE_DOMAIN);
}

#endif /* _LINUX_SCHED_ISOLATION_H */
