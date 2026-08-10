/*
 * cpuidle.h - a generic framework for CPU idle power management
 *
 * (C) 2007 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>
 *          Shaohua Li <shaohua.li@intel.com>
 *          Adam Belay <abelay@novell.com>
 *
 * This code is licenced under the GPL.
 */

#ifndef _LINUX_CPUIDLE_H
#define _LINUX_CPUIDLE_H

#include <linux/percpu.h>
#include <linux/list.h>
#include <linux/hrtimer.h>
#include <linux/context_tracking.h>

/*
 * cpuidle 公共数据模型与接口总览：
 *
 * cpuidle_driver 描述平台可进入的状态表，cpuidle_device 保存每 CPU 的启用状态与统计，
 * cpuidle_governor 在两者之间选择状态。驱动拥有 driver 及回调依赖的硬件资源，设备
 * 所有者负责其存储期，核心仅在注册期间持有指针并通过模块引用固定驱动代码。
 * idle 快路径以本 CPU、IRQ/RCU 的严格上下文约束换取无锁读取，控制面修改必须先暂停。
 */

/* 固定数组容量以及供 sysfs/参数使用的名称、描述最大长度（均包含末尾 NUL 空间）。 */
#define CPUIDLE_STATE_MAX	10
#define CPUIDLE_NAME_LEN	16
#define CPUIDLE_DESC_LEN	32

struct module;

struct cpuidle_device;
struct cpuidle_driver;


/****************************
 * CPUIDLE DEVICE INTERFACE *
 ****************************/

/* disable 是原因位图：用户策略与驱动固有限制可独立设置，任一位存在即不可选择。 */
#define CPUIDLE_STATE_DISABLED_BY_USER		BIT(0)
#define CPUIDLE_STATE_DISABLED_BY_DRIVER	BIT(1)

/*
 * cpuidle_state_usage - 某 CPU 上某个状态的运行统计和禁用原因
 * @disable: CPUIDLE_STATE_DISABLED_* 位图，不是简单布尔值
 * @usage: 成功进入次数
 * @time_ns: 普通 idle 累计驻留纳秒数
 * @above: 实际驻留过短、所选状态可能过深的次数
 * @below: 实际驻留足够长、所选状态可能过浅的次数
 * @rejected: 后端拒绝本次进入的次数
 * @s2idle_usage: suspend-to-idle 回调执行次数
 * @s2idle_time: suspend-to-idle 累计时间，历史接口单位为微秒
 *
 * 统计槽由 device 拥有，注册时清零；普通更新发生在所属 CPU 的 idle 路径，控制面
 * 读取需接受并发快照语义。disable 的多个来源必须按位增删，不能覆盖其他来源。
 */
struct cpuidle_state_usage {
	unsigned long long	disable;
	unsigned long long	usage;
	u64			time_ns;
	unsigned long long	above; /* Number of times it's been too deep */
	/* above：所选状态相对实际空闲时长过深的次数。 */
	unsigned long long	below; /* Number of times it's been too shallow */
	/* below：所选状态相对实际空闲时长过浅的次数。 */
	unsigned long long	rejected; /* Number of times idle entry was rejected */
	/* rejected：驱动 enter 返回负值、未真正进入状态的次数。 */
#ifdef CONFIG_SUSPEND
	unsigned long long	s2idle_usage;
	unsigned long long	s2idle_time; /* in US */
	/* s2idle_time 保持用户 ABI 所需的微秒单位，与普通 time_ns 不同。 */
#endif
};

/*
 * cpuidle_state - 驱动提供的一个硬件空闲状态描述
 * @name/@desc: 面向用户和诊断的短名称与描述
 * @exit_latency_ns: 从状态唤醒到可运行的最坏/约定退出时延，纳秒
 * @target_residency_ns: 抵消进入/退出代价所需的目标驻留时间，纳秒
 * @flags: CPUIDLE_FLAG_* 能力及约束
 * @exit_latency/@target_residency: 兼容旧驱动/接口的微秒字段
 * @power_usage: 旧式功耗提示，单位毫瓦
 * @enter: 普通 idle 回调，返回实际状态索引或负错误码，必须按约定带 IRQ 关闭返回
 * @enter_dead: CPU 下线回调；成功后不返回
 * @enter_s2idle: suspend-to-idle 专用回调，不得在任何时刻开启 IRQ或改时钟事件设备
 *
 * states 数组由 driver 拥有并在注册期间保持不变；索引也是统计和 governor 交流的 ABI。
 */
struct cpuidle_state {
	char		name[CPUIDLE_NAME_LEN];
	char		desc[CPUIDLE_DESC_LEN];

	s64		exit_latency_ns;
	s64		target_residency_ns;
	unsigned int	flags;
	unsigned int	exit_latency; /* in US */
	int		power_usage; /* in mW */
	unsigned int	target_residency; /* in US */

	int (*enter)	(struct cpuidle_device *dev,
			struct cpuidle_driver *drv,
			int index);

	void (*enter_dead) (struct cpuidle_device *dev, int index);

	/*
	 * CPUs execute ->enter_s2idle with the local tick or entire timekeeping
	 * suspended, so it must not re-enable interrupts at any point (even
	 * temporarily) or attempt to change states of clock event devices.
	 *
	 * This callback may point to the same function as ->enter if all of
	 * the above requirements are met by it.
	 * CPU 执行 enter_s2idle 时，本地 tick 乃至整个 timekeeping 已暂停，因此回调即使
	 * 短暂开启中断也不安全，也不得切换 clock event device。若普通 enter 完全满足
	 * 这些约束，两个函数指针可以指向同一实现。
	 */
	int (*enter_s2idle)(struct cpuidle_device *dev,
			    struct cpuidle_driver *drv,
			    int index);
};

/* Idle State Flags */
/* Idle 状态标志：描述进入机制，不是运行时统计。 */
#define CPUIDLE_FLAG_NONE       	(0x00)
#define CPUIDLE_FLAG_POLLING		BIT(0) /* polling state */
/* POLLING：忙轮询而非硬件睡眠。 */
#define CPUIDLE_FLAG_COUPLED		BIT(1) /* state applies to multiple cpus */
/* COUPLED：多个 CPU 必须通过 coupled 框架协调进入。 */
#define CPUIDLE_FLAG_TIMER_STOP 	BIT(2) /* timer is stopped on this state */
/* TIMER_STOP：本地 tick 停止，进入前需要广播定时器。 */
#define CPUIDLE_FLAG_UNUSABLE		BIT(3) /* avoid using this state */
/* UNUSABLE：驱动判定永久不可用，注册时映射为驱动禁用位。 */
#define CPUIDLE_FLAG_OFF		BIT(4) /* disable this state by default */
/* OFF：默认按用户禁用处理，可经策略显式开启。 */
#define CPUIDLE_FLAG_TLB_FLUSHED	BIT(5) /* idle-state flushes TLBs */
/* TLB_FLUSHED：硬件状态丢失 TLB，核心进入前执行 leave_mm()。 */
#define CPUIDLE_FLAG_RCU_IDLE		BIT(6) /* idle-state takes care of RCU */
/* RCU_IDLE：驱动自行管理 RCU/context tracking，核心不再包裹 enter。 */

struct cpuidle_device_kobj;
struct cpuidle_state_kobj;
struct cpuidle_driver_kobj;

/*
 * cpuidle_device - cpuidle 在单个逻辑 CPU 上的实例
 * @registered: 已发布到 per-CPU 槽及全局设备链表
 * @enabled: 已接入 governor/sysfs，可被 idle 快路径选择
 * @poll_time_limit: 轮询状态是否达到其动态时间限制
 * @cpu: 所属逻辑 CPU 编号，注册期间保持不变
 * @next_hrtimer: 本次 idle 前观察到的下一 hrtimer 期限，0 表示当前未进入
 * @last_state_idx/@last_residency_ns: 最近一次选择/实际驻留结果
 * @poll_limit_ns: governor 覆盖或核心缓存的轮询上限
 * @forced_idle_latency_limit_ns: 非零时覆盖 governor，强制选择预算内最深状态
 * @states_usage: 与 driver->states 相同索引的每状态统计
 * @kobjs/@kobj_driver/@kobj_dev: sysfs 表示，由 cpuidle sysfs 层创建和销毁
 * @device_list: 链接到 cpuidle_detected_devices，不拥有队头
 * @coupled_cpus/@coupled: coupled 状态的参与 CPU 集合与共享协调对象
 *
 * registered 与 enabled 是两阶段生命周期：先注册核心身份，再启用快路径；解除注册
 * 必须逆序进行。设备对象由架构或通用 per-CPU 存储拥有，框架不负责释放其内存。
 */
struct cpuidle_device {
	unsigned int		registered:1;
	unsigned int		enabled:1;
	unsigned int		poll_time_limit:1;
	unsigned int		cpu;
	ktime_t			next_hrtimer;

	int			last_state_idx;
	u64			last_residency_ns;
	u64			poll_limit_ns;
	u64			forced_idle_latency_limit_ns;
	struct cpuidle_state_usage	states_usage[CPUIDLE_STATE_MAX];
	struct cpuidle_state_kobj *kobjs[CPUIDLE_STATE_MAX];
	struct cpuidle_driver_kobj *kobj_driver;
	struct cpuidle_device_kobj *kobj_dev;
	struct list_head 	device_list;

#ifdef CONFIG_ARCH_NEEDS_CPU_IDLE_COUPLED
	cpumask_t		coupled_cpus;
	struct cpuidle_coupled	*coupled;
#endif
};

/* 指针槽供 idle loop 查找已注册设备；cpuidle_dev 是通用批量注册助手使用的实体。 */
DECLARE_PER_CPU(struct cpuidle_device *, cpuidle_devices);
DECLARE_PER_CPU(struct cpuidle_device, cpuidle_dev);

/*
 * ct_cpuidle_enter - 从 IRQ-off idle 入口切换到扩展静止状态
 *
 * 进入时必须已关闭 IRQ。idle 回调允许临时开启 IRQ，但必须关闭返回，因此先把 lockdep/
 * tracing 的逻辑 IRQ 状态切为开启，再结束 instrumentation 并通知 context tracking/RCU。
 * 最后的 lockdep_hardirqs_on 必须晚于 ct_idle_enter()，其顺序与低级入口代码一致。
 */
static __always_inline void ct_cpuidle_enter(void)
{
	lockdep_assert_irqs_disabled();
	/*
	 * Idle is allowed to (temporary) enable IRQs. It
	 * will return with IRQs disabled.
	 *
	 * Trace IRQs enable here, then switch off RCU, and have
	 * arch_cpu_idle() use raw_local_irq_enable(). Note that
	 * ct_idle_enter() relies on lockdep IRQ state, so switch that
	 * last -- this is very similar to the entry code.
	 * idle 可临时打开 IRQ，但必须带 IRQ 关闭返回。这里先记录 IRQ-on tracing，再关闭
	 * RCU watching，并让 arch_cpu_idle() 使用 raw_local_irq_enable()。ct_idle_enter()
	 * 依赖 lockdep 当前仍认定 IRQ-off，所以 lockdep 的最终状态切换必须放在最后。
	 */
	trace_hardirqs_on_prepare();
	lockdep_hardirqs_on_prepare();
	instrumentation_end();
	ct_idle_enter();
	lockdep_hardirqs_on(_RET_IP_);
}

/*
 * ct_cpuidle_exit - 精确逆转 ct_cpuidle_enter() 的逻辑状态
 *
 * 回调已带硬件 IRQ 关闭返回；先让 lockdep 观察 IRQ-off，再退出 RCU idle，最后重新
 * 开启 instrumentation。不得调换次序，否则 tracing/RCU 可能在错误上下文运行。
 */
static __always_inline void ct_cpuidle_exit(void)
{
	/*
	 * Carefully undo the above.
	 * 严格按相反顺序撤销上面的 context tracking 与插桩状态。
	 */
	lockdep_hardirqs_off(_RET_IP_);
	ct_idle_exit();
	instrumentation_begin();
}

/****************************
 * CPUIDLE DRIVER INTERFACE *
 ****************************/

/*
 * cpuidle_driver - 一组 CPU 共用的空闲状态提供者
 * @name: 驱动身份；注册期间必须保持有效
 * @owner: 持有回调代码的模块，设备注册会取得引用
 * @bctimer: 框架是否需为该驱动设置广播定时器
 * @states: 按功耗递减/深度递增排列的状态表
 * @state_count: states 中有效元素数，不得超过 CPUIDLE_STATE_MAX
 * @safe_state_index: 平台认为可安全回退的状态索引
 * @cpumask: 驱动覆盖的 CPU 集合，注册期间由驱动拥有并保持稳定
 * @governor: 注册时优先选择的 governor 名称，可为 NULL
 */
struct cpuidle_driver {
	const char		*name;
	struct module 		*owner;

        /* used by the cpuidle framework to setup the broadcast timer */
	/* cpuidle 核心据此安排广播定时器支持。 */
	unsigned int            bctimer:1;
	/* states array must be ordered in decreasing power consumption */
	/* 状态数组必须按功耗递减排列，索引越大通常越深，选择与统计均依赖该顺序。 */
	struct cpuidle_state	states[CPUIDLE_STATE_MAX];
	int			state_count;
	int			safe_state_index;

	/* the driver handles the cpus in cpumask */
	/* 驱动只服务 cpumask 中的 CPU；该掩码同时限定批量设备注册范围。 */
	struct cpumask		*cpumask;

	/* preferred governor to switch at register time */
	/* 注册时请求切换到的首选 governor；不可用时由核心按策略回退。 */
	const char		*governor;
};

#ifdef CONFIG_CPU_IDLE
/*
 * 框架启用时的真实接口分组：
 * - 快路径：可用性判断、select/enter/reflect、轮询时限；
 * - 控制面：驱动/设备注册、启停与 pause/resume；
 * - 热拔：play_dead；
 * - 查询：取得全局或当前 CPU 的驱动/设备。
 * 调用者必须分别遵守本 CPU/IRQ 快路径约束和 cpuidle_lock 控制面约束。
 */
extern void disable_cpuidle(void);
extern bool cpuidle_not_available(struct cpuidle_driver *drv,
				  struct cpuidle_device *dev);

extern int cpuidle_select(struct cpuidle_driver *drv,
			  struct cpuidle_device *dev,
			  bool *stop_tick);
extern int cpuidle_enter(struct cpuidle_driver *drv,
			 struct cpuidle_device *dev, int index);
extern void cpuidle_reflect(struct cpuidle_device *dev, int index);
extern u64 cpuidle_poll_time(struct cpuidle_driver *drv,
			     struct cpuidle_device *dev);

extern int cpuidle_register_driver(struct cpuidle_driver *drv);
extern struct cpuidle_driver *cpuidle_get_driver(void);
extern void cpuidle_driver_state_disabled(struct cpuidle_driver *drv, int idx,
					bool disable);
extern void cpuidle_unregister_driver(struct cpuidle_driver *drv);
extern int cpuidle_register_device(struct cpuidle_device *dev);
extern void cpuidle_unregister_device(struct cpuidle_device *dev);
extern void cpuidle_unregister_device_no_lock(struct cpuidle_device *dev);
extern int cpuidle_register(struct cpuidle_driver *drv,
			    const struct cpumask *const coupled_cpus);
extern void cpuidle_unregister(struct cpuidle_driver *drv);
extern void cpuidle_pause_and_lock(void);
extern void cpuidle_resume_and_unlock(void);
extern void cpuidle_pause(void);
extern void cpuidle_resume(void);
extern int cpuidle_enable_device(struct cpuidle_device *dev);
extern void cpuidle_disable_device(struct cpuidle_device *dev);
extern int cpuidle_play_dead(void);

extern struct cpuidle_driver *cpuidle_get_cpu_driver(struct cpuidle_device *dev);
/* 当前 CPU 的发布槽只可在禁止迁移或天然 per-CPU 的上下文读取。 */
static inline struct cpuidle_device *cpuidle_get_device(void)
{return __this_cpu_read(cpuidle_devices); }
#else
/*
 * CONFIG_CPU_IDLE=n 时提供类型兼容的零成本桩：查询返回 NULL，不可用判断恒真，可能
 * 触发功能的操作返回 -ENODEV，void 清理接口为空操作。这样调用方无需散布条件编译，
 * 但必须处理返回值，不能把桩函数的失败误认为真实状态索引。
 */
static inline void disable_cpuidle(void) { }
static inline bool cpuidle_not_available(struct cpuidle_driver *drv,
					 struct cpuidle_device *dev)
{return true; }
static inline int cpuidle_select(struct cpuidle_driver *drv,
				 struct cpuidle_device *dev, bool *stop_tick)
{return -ENODEV; }
static inline int cpuidle_enter(struct cpuidle_driver *drv,
				struct cpuidle_device *dev, int index)
{return -ENODEV; }
static inline void cpuidle_reflect(struct cpuidle_device *dev, int index) { }
static inline u64 cpuidle_poll_time(struct cpuidle_driver *drv,
			     struct cpuidle_device *dev)
{return 0; }
static inline int cpuidle_register_driver(struct cpuidle_driver *drv)
{return -ENODEV; }
static inline struct cpuidle_driver *cpuidle_get_driver(void) {return NULL; }
static inline void cpuidle_driver_state_disabled(struct cpuidle_driver *drv,
					       int idx, bool disable) { }
static inline void cpuidle_unregister_driver(struct cpuidle_driver *drv) { }
static inline int cpuidle_register_device(struct cpuidle_device *dev)
{return -ENODEV; }
static inline void cpuidle_unregister_device(struct cpuidle_device *dev) { }
static inline void cpuidle_unregister_device_no_lock(struct cpuidle_device *dev) {}
static inline int cpuidle_register(struct cpuidle_driver *drv,
				   const struct cpumask *const coupled_cpus)
{return -ENODEV; }
static inline void cpuidle_unregister(struct cpuidle_driver *drv) { }
static inline void cpuidle_pause_and_lock(void) { }
static inline void cpuidle_resume_and_unlock(void) { }
static inline void cpuidle_pause(void) { }
static inline void cpuidle_resume(void) { }
static inline int cpuidle_enable_device(struct cpuidle_device *dev)
{return -ENODEV; }
static inline void cpuidle_disable_device(struct cpuidle_device *dev) { }
static inline int cpuidle_play_dead(void) {return -ENODEV; }
static inline struct cpuidle_driver *cpuidle_get_cpu_driver(
	struct cpuidle_device *dev) {return NULL; }
static inline struct cpuidle_device *cpuidle_get_device(void) {return NULL; }
#endif

#ifdef CONFIG_CPU_IDLE
/* 深状态覆盖与 suspend-to-idle API；时延参数统一使用纳秒，0 覆盖值表示取消强制。 */
extern int cpuidle_find_deepest_state(struct cpuidle_driver *drv,
				      struct cpuidle_device *dev,
				      u64 latency_limit_ns);
extern int cpuidle_enter_s2idle(struct cpuidle_driver *drv,
				struct cpuidle_device *dev,
				u64 latency_limit_ns);
extern void cpuidle_use_deepest_state(u64 latency_limit_ns);
#else
/* 无 cpuidle 配置时查询/进入明确失败，设置覆盖则为空操作。 */
static inline int cpuidle_find_deepest_state(struct cpuidle_driver *drv,
					     struct cpuidle_device *dev,
					     u64 latency_limit_ns)
{return -ENODEV; }
static inline int cpuidle_enter_s2idle(struct cpuidle_driver *drv,
				       struct cpuidle_device *dev,
				       u64 latency_limit_ns)
{return -ENODEV; }
static inline void cpuidle_use_deepest_state(u64 latency_limit_ns)
{
}
#endif

/* kernel/sched/idle.c */
/* 调度 idle loop 与 cpuidle 核心的边界：发布计划状态，以及执行架构默认 idle 回退。 */
extern void sched_idle_set_state(struct cpuidle_state *idle_state);
extern void default_idle_call(void);

#ifdef CONFIG_ARCH_NEEDS_CPU_IDLE_COUPLED
/* coupled CPU 在共享 atomic 计数上会合；设备的 coupled 生命周期必须已经注册。 */
void cpuidle_coupled_parallel_barrier(struct cpuidle_device *dev, atomic_t *a);
#else
/* 架构不需要 coupled idle 时保留同形空操作，调用点无需条件编译。 */
static inline void cpuidle_coupled_parallel_barrier(struct cpuidle_device *dev, atomic_t *a)
{
}
#endif

#if defined(CONFIG_CPU_IDLE) && defined(CONFIG_ARCH_HAS_CPU_RELAX)
/* 同时具备 cpuidle 与 cpu_relax 时，由核心初始化状态 0 的轮询实现。 */
void cpuidle_poll_state_init(struct cpuidle_driver *drv);
#else
/* 缺任一能力时轮询状态初始化为空操作。 */
static inline void cpuidle_poll_state_init(struct cpuidle_driver *drv) {}
#endif

/******************************
 * CPUIDLE GOVERNOR INTERFACE *
 ******************************/

/*
 * cpuidle_governor - 状态选择策略插件
 * @name: 策略名称，用于参数和 sysfs 选择
 * @governor_list: 核心 governor 注册链表节点
 * @rating: 自动选择时的优先级，通常值越高越优先
 * @enable/@disable: 可选的每设备策略状态建立与拆除回调
 * @select: 必选选择器，返回非负状态索引并可要求保留 tick
 * @reflect: 可选反馈回调，接收实际进入的状态索引
 *
 * governor 对象由实现者长期拥有；被选为当前 governor 时，其回调和私有状态必须覆盖
 * 所有启用设备。控制面切换通过暂停 handler 避免与无锁 select/reflect 并发拆除。
 */
struct cpuidle_governor {
	char			name[CPUIDLE_NAME_LEN];
	struct list_head 	governor_list;
	unsigned int		rating;

	int  (*enable)		(struct cpuidle_driver *drv,
					struct cpuidle_device *dev);
	void (*disable)		(struct cpuidle_driver *drv,
					struct cpuidle_device *dev);

	int  (*select)		(struct cpuidle_driver *drv,
					struct cpuidle_device *dev,
					bool *stop_tick);
	void (*reflect)		(struct cpuidle_device *dev, int index);
};

/* 注册 governor；latency_req 汇总指定 CPU 当前 PM QoS 唤醒时延约束并换算为纳秒。 */
extern int cpuidle_register_governor(struct cpuidle_governor *gov);
extern s64 cpuidle_governor_latency_req(unsigned int cpu);

/*
 * CPU PM 低级进入宏的共同事务：
 *
 * idx==0 时直接执行 cpu_do_idle() 并从包含宏的外层函数返回 0；非零状态若不是 retention，
 * 先用 cpu_pm_enter()/exit() 通知 CPU PM 链。若驱动未声明自行处理 RCU，则在低级回调
 * 两侧调用 ct_cpuidle_enter/exit。cpu_pm_enter 或低级回调失败统一映射为 -1，成功返回
 * 原 idx。@state 可与 @idx 不同，用于硬件私有状态编码。
 *
 * 这是包含 return 的语句表达式，调用者必须清楚它会提前返回；进入时应已关闭本地 IRQ，
 * low_level_idle_enter 必须按 context tracking 和 IRQ 契约返回。
 */
#define __CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter,			\
				idx,					\
				state,					\
				is_retention, is_rcu)			\
({									\
	int __ret = 0;							\
									\
	if (!idx) {							\
		/* 状态 0 是架构安全兜底，不经过 CPU PM 通知或低级参数回调。 */ \
		cpu_do_idle();						\
		return idx;						\
	}								\
									\
	if (!is_retention)						\
		/* 非 retention 可能丢失 CPU 上下文，先通知 CPU PM 客户端保存状态。 */ \
		__ret =  cpu_pm_enter();				\
	if (!__ret) {							\
		if (!is_rcu)						\
			/* 未由低级实现管理 RCU 时，核心包住整个硬件 idle 回调。 */ \
			ct_cpuidle_enter();				\
		__ret = low_level_idle_enter(state);			\
		if (!is_rcu)						\
			ct_cpuidle_exit();				\
		if (!is_retention)					\
			/* 仅在 cpu_pm_enter 成功且实际尝试进入后发送对称退出通知。 */ \
			cpu_pm_exit();					\
	}								\
									\
	__ret ? -1 : idx;						\
})

/* 标准变体：索引同时作为硬件状态参数，非 retention，RCU 由宏管理。 */
#define CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, idx, 0, 0)

/* retention 变体：CPU 上下文保持，不发送 cpu_pm_enter/exit，RCU 仍由宏管理。 */
#define CPU_PM_CPU_IDLE_ENTER_RETENTION(low_level_idle_enter, idx)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, idx, 1, 0)

/* 参数变体：返回/统计仍用 idx，但把独立的 state 编码交给硬件回调。 */
#define CPU_PM_CPU_IDLE_ENTER_PARAM(low_level_idle_enter, idx, state)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, state, 0, 0)

/* 参数 + RCU 变体：低级实现自行维护 RCU idle 边界。 */
#define CPU_PM_CPU_IDLE_ENTER_PARAM_RCU(low_level_idle_enter, idx, state)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, state, 0, 1)

/* retention + 参数变体：跳过 CPU PM 通知，由宏维护 RCU。 */
#define CPU_PM_CPU_IDLE_ENTER_RETENTION_PARAM(low_level_idle_enter, idx, state)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, state, 1, 0)

/* 最宽松变体：retention 且低级实现自行处理 RCU，宏只负责调用和返回值归一化。 */
#define CPU_PM_CPU_IDLE_ENTER_RETENTION_PARAM_RCU(low_level_idle_enter, idx, state)	\
	__CPU_PM_CPU_IDLE_ENTER(low_level_idle_enter, idx, state, 1, 1)

/* 结束 cpuidle 公共接口定义。 */
#endif /* _LINUX_CPUIDLE_H */
