/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TIMER_H
#define _LINUX_TIMER_H

/*
 * 本头文件公开低精度 jiffies timer 的初始化、排队、修改、删除和 shutdown 契约。
 * timer_list 存储由调用者拥有；timer wheel 只在 pending/回调期间异步借用它，因此释放
 * 容器前必须选择正确的同步删除或 shutdown。回调默认在 timer softirq 上下文执行。
 */
#include <linux/list.h>
#include <linux/ktime.h>
#include <linux/stddef.h>
#include <linux/debugobjects.h>
#include <linux/stringify.h>
#include <linux/timer_types.h>

#ifdef CONFIG_LOCKDEP
/*
 * NB: because we have to copy the lockdep_map, setting the lockdep_map key
 * (second argument) here is required, otherwise it could be initialised to
 * the copy of the lockdep_map later! We use the pointer to and the string
 * "<file>:<line>" as the key resp. the name of the lockdep_map.
 */
/*
 * timer 对象会复制预置 lockdep_map，所以初始化器必须直接使用每个展开点的静态 key，
 * 不能先构造临时 map 再复制，否则多个 timer 可能错误共享或丢失锁类别。FILE_LINE 生成
 * 可定位的名称；关闭 LOCKDEP 时该字段初始化为空，不改变 timer 的运行时语义。
 */
#define __TIMER_LOCKDEP_MAP_INITIALIZER(_kn)				\
	.lockdep_map = STATIC_LOCKDEP_MAP_INIT(_kn, &_kn),
#else
#define __TIMER_LOCKDEP_MAP_INITIALIZER(_kn)
#endif

/*
 * @TIMER_DEFERRABLE: A deferrable timer will work normally when the
 * system is busy, but will not cause a CPU to come out of idle just
 * to service it; instead, the timer will be serviced when the CPU
 * eventually wakes up with a subsequent non-deferrable timer.
 *
 * @TIMER_IRQSAFE: An irqsafe timer is executed with IRQ disabled and
 * it's safe to wait for the completion of the running instance from
 * IRQ handlers, for example, by calling timer_delete_sync().
 *
 * Note: The irq disabled callback execution is a special case for
 * workqueue locking issues. It's not meant for executing random crap
 * with interrupts disabled. Abuse is monitored!
 *
 * @TIMER_PINNED: A pinned timer will always expire on the CPU on which the
 * timer was enqueued. When a particular CPU is required, add_timer_on()
 * has to be used. Enqueue via mod_timer() and add_timer() is always done
 * on the local CPU.
 */
/*
 * TIMER_* flags 同时编码 timer base/CPU 位置和公开行为：DEFERRABLE 不单独唤醒 idle CPU；
 * PINNED 保持入队 CPU（精确目标需 add_timer_on）；IRQSAFE 让回调在 IRQ-off 环境执行，
 * 因而允许 IRQ handler 用 timer_delete_sync() 等它结束。IRQSAFE 只为锁依赖场景服务，
 * 不是在回调中执行长耗时工作的许可。CPU/迁移/数组位由 timer core 内部维护。
 */
/* flags 低位保存 timer base 的 CPU 编号。 */
#define TIMER_CPUMASK		0x0003FFFF
/* 迁移中哨兵：base 指针/CPU 位正在两把 base 锁之间转换。 */
#define TIMER_MIGRATING		0x00040000
/* 选择 timer base 所需的全部内部位置位。 */
#define TIMER_BASEMASK		(TIMER_CPUMASK | TIMER_MIGRATING)
/* idle CPU 可延后处理，不以该 timer 单独触发唤醒。 */
#define TIMER_DEFERRABLE	0x00080000
/* timer 保持在入队 CPU 的 base，不参与普通迁移选择。 */
#define TIMER_PINNED		0x00100000
/* 回调 IRQ-off，且允许中断上下文同步等待当前实例结束。 */
#define TIMER_IRQSAFE		0x00200000
/* 调用者在初始化阶段可传入的公开行为位集合。 */
#define TIMER_INIT_FLAGS	(TIMER_DEFERRABLE | TIMER_PINNED | TIMER_IRQSAFE)
/* 高位保存 timer wheel 数组索引；偏移与掩码供 core 编解码。 */
#define TIMER_ARRAYSHIFT	22
#define TIMER_ARRAYMASK		0xFFC00000

/* trace 只导出迁移和公开行为位，不泄露 CPU/base 的完整内部编码。 */
#define TIMER_TRACE_FLAGMASK	(TIMER_MIGRATING | TIMER_DEFERRABLE | TIMER_PINNED | TIMER_IRQSAFE)

/*
 * __TIMER_INITIALIZER() - 构造静态未 pending timer。
 * @_function: 覆盖 timer 生命期的回调借用；@_flags: TIMER_INIT_FLAGS 子集。entry 的特殊
 * TIMER_ENTRY_STATIC 哨兵供 debug/core 识别静态对象，lockdep map 在同一展开点初始化。
 */
#define __TIMER_INITIALIZER(_function, _flags) {		\
		.entry = { .next = TIMER_ENTRY_STATIC },	\
		.function = (_function),			\
		.flags = (_flags),				\
		__TIMER_LOCKDEP_MAP_INITIALIZER(FILE_LINE)	\
	}

/* DEFINE_TIMER() - 定义名为 @_name 的静态普通 timer，初始 flags=0 且尚未排队。 */
#define DEFINE_TIMER(_name, _function)				\
	struct timer_list _name =				\
		__TIMER_INITIALIZER(_function, 0)

/*
 * LOCKDEP and DEBUG timer interfaces.
 */
/* 以下初始化接口同时登记 debugobjects 和 lockdep；普通用户应优先使用 timer_setup。 */
/*
 * timer_init_key() - 初始化非栈 timer、回调、行为位和可选 lockdep 类。
 * @timer: 调用者独占输出对象。@func: 非空回调借用。@flags: TIMER_INIT_FLAGS 子集。
 * @name/@key: 可空 lockdep 元数据借用，key 须静态存活。不排队、不睡眠、返回 void；
 * pending/running 对象不得重新初始化，否则会破坏异步 ownership。
 */
void timer_init_key(struct timer_list *timer,
		    void (*func)(struct timer_list *), unsigned int flags,
		    const char *name, struct lock_class_key *key);

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS
/*
 * timer_init_key_on_stack() - 初始化栈上 timer 并向 debugobjects 登记栈生命周期。
 * 参数同 timer_init_key()；必须在离开栈帧前以 timer_destroy_on_stack() 配对。
 */
extern void timer_init_key_on_stack(struct timer_list *timer,
				    void (*func)(struct timer_list *),
				    unsigned int flags, const char *name,
				    struct lock_class_key *key);
#else
/* 未启用 timer debugobjects 时退化为普通初始化，参数/运行时 timer 语义不变。 */
static inline void timer_init_key_on_stack(struct timer_list *timer,
					   void (*func)(struct timer_list *),
					   unsigned int flags,
					   const char *name,
					   struct lock_class_key *key)
{
	/* 单阶段包装不保留参数指针；底层完成全部字段初始化。 */
	timer_init_key(timer, func, flags, name, key);
}
#endif

#ifdef CONFIG_LOCKDEP
/* __timer_init() - 为每个展开点生成静态 lock class，再初始化普通 timer。 */
#define __timer_init(_timer, _fn, _flags)				\
	do {								\
		static struct lock_class_key __key;			\
		timer_init_key((_timer), (_fn), (_flags), #_timer, &__key);\
	} while (0)

/* __timer_init_on_stack() - 同样生成 lock class，但使用栈对象调试初始化路径。 */
#define __timer_init_on_stack(_timer, _fn, _flags)			\
	do {								\
		static struct lock_class_key __key;			\
		timer_init_key_on_stack((_timer), (_fn), (_flags),	\
					#_timer, &__key);		 \
	} while (0)
#else
/* 关闭 LOCKDEP 后不创建 key/name，仍执行普通 timer 字段和 debugobjects 初始化。 */
#define __timer_init(_timer, _fn, _flags)				\
	timer_init_key((_timer), (_fn), (_flags), NULL, NULL)
/* 关闭 LOCKDEP 的栈上版本；仍须与 timer_destroy_on_stack() 配对。 */
#define __timer_init_on_stack(_timer, _fn, _flags)			\
	timer_init_key_on_stack((_timer), (_fn), (_flags), NULL, NULL)
#endif

/**
 * timer_setup - prepare a timer for first use
 * @timer: the timer in question
 * @callback: the function to call when timer expires
 * @flags: any TIMER_* flags
 *
 * Regular timer initialization should use either DEFINE_TIMER() above,
 * or timer_setup(). For timers on the stack, timer_setup_on_stack() must
 * be used and must be balanced with a call to timer_destroy_on_stack().
 */
/*
 * timer_setup() - 首次使用前初始化非栈 timer。
 * @timer: 调用者独占输出对象。@callback: 回调代码借用。@flags: TIMER_* 初始化行为位。
 * 不排队、不取得容器引用；静态对象可改用 DEFINE_TIMER。栈对象必须用 on_stack 版本。
 */
#define timer_setup(timer, callback, flags)			\
	__timer_init((timer), (callback), (flags))

/* timer_setup_on_stack() - 初始化栈上 timer；返回前必须先同步静止并 destroy_on_stack。 */
#define timer_setup_on_stack(timer, callback, flags)		\
	__timer_init_on_stack((timer), (callback), (flags))

#ifdef CONFIG_DEBUG_OBJECTS_TIMERS
/* timer_destroy_on_stack() - 注销已静止栈 timer 的调试状态；不负责取消 pending 回调。 */
extern void timer_destroy_on_stack(struct timer_list *timer);
#else
/* 无 debugobjects 时为空操作，但调用者仍须先保证 timer 不 pending/running。 */
static inline void timer_destroy_on_stack(struct timer_list *timer) { }
#endif

/*
 * timer_container_of() - 从回调收到的 timer 指针恢复外层容器。
 * @var 仅提供目标类型，@callback_timer 是有效借用，@timer_fieldname 是成员名；宏不取得
 * 引用，容器必须由调用者的同步删除协议覆盖整个回调。
 */
#define timer_container_of(var, callback_timer, timer_fieldname)	\
	container_of(callback_timer, typeof(*var), timer_fieldname)

/**
 * timer_pending - is a timer pending?
 * @timer: the timer in question
 *
 * timer_pending will tell whether a given timer is currently pending,
 * or not. Callers must ensure serialization wrt. other operations done
 * to this timer, eg. interrupt contexts, or other CPUs on SMP.
 *
 * Returns: 1 if the timer is pending, 0 if not.
 */
/*
 * timer_pending() - 查询 timer 当前是否连接在时间轮。
 * @timer: 生命周期稳定的输入借用。无锁、不可睡眠；返回 1/0 快照，不表示 callback
 * 是否正在运行。lockless hlist 读取只防撕裂，调用者仍须与其他 CPU/IRQ 的修改串行。
 */
static inline int timer_pending(const struct timer_list * timer)
{
	/* entry 未被 unhashed 即仍挂在某个 base；该判断不取得 base 锁或容器引用。 */
	return !hlist_unhashed_lockless(&timer->entry);
}

/*
 * add_timer_on() - 以 timer->expires 启动 inactive timer 并固定到显式 @cpu。
 * @timer: 已初始化且未 pending 的借用；@cpu: 有效 CPU 编号。不可睡眠、返回 void；
 * 重复 add 告警，shutdown 请求静默丢弃，不等待旧 callback。
 */
extern void add_timer_on(struct timer_list *timer, int cpu);
/*
 * mod_timer() - 原子启动或把 timer 重排到绝对 @expires jiffy。
 * @timer: 已初始化对象借用。可在原子上下文使用；返回 1 表示入口 active（即使有效期限
 * 未改变），0 表示 inactive 后被启动，或 shutdown 导致操作丢弃。它不等待并发 callback。
 */
extern int mod_timer(struct timer_list *timer, unsigned long expires);
/* 仅修改已 pending timer；inactive/shutdown 返回 0，active 重排返回 1，不启动 idle 对象。 */
extern int mod_timer_pending(struct timer_list *timer, unsigned long expires);
/* 仅在 @expires 更早时收紧期限，inactive 时启动；返回类别同 mod_timer。 */
extern int timer_reduce(struct timer_list *timer, unsigned long expires);

/*
 * The jiffies value which is added to now, when there is no timer
 * in the timer wheel:
 */
/* 空时间轮采用的最远相对 jiffy；限制在 30 位以保持有符号时间比较安全。 */
#define TIMER_NEXT_MAX_DELTA	((1UL << 30) - 1)

/* add_timer() - 用 timer->expires 启动 inactive 普通 timer；重复 add 告警，返回 void。 */
extern void add_timer(struct timer_list *timer);
/* add_timer_local() - 启动前设置 PINNED，使本轮归当前 CPU local base。 */
extern void add_timer_local(struct timer_list *timer);
/* add_timer_global() - 启动前清 PINNED，允许 NO_HZ timer migration 使用 global base。 */
extern void add_timer_global(struct timer_list *timer);

/*
 * timer_delete_sync_try() - 非阻塞尝试删除并确认 callback 未运行。
 * @timer: 稳定借用；返回 0/1 表示未 pending/已删除，-1 表示远端 callback 正在运行。
 * 不阻止之后 rearm，调用者仍须串行重启。
 */
extern int timer_delete_sync_try(struct timer_list *timer);
/*
 * timer_delete_sync() - 删除 pending 实例并等待当前 callback 在所有 CPU 结束。
 * @timer: 稳定借用；返回 0/1。调用者须先阻止并发 rearm，且不能持 callback 所需的锁。
 * 除 TIMER_IRQSAFE 外不能从中断上下文调用；返回后仍允许未来重新启动。
 */
extern int timer_delete_sync(struct timer_list *timer);
/*
 * timer_delete() - 只删除 pending 实例，不等待并发 callback、不阻止 rearm。
 * @timer: 稳定借用；返回 1 表示摘除，0 表示未 pending。不能单独作为释放容器的屏障。
 */
extern int timer_delete(struct timer_list *timer);
/*
 * timer_shutdown_sync() - teardown 的最终同步屏障。
 * @timer: 稳定借用；返回 0/1。出口保证不 pending、不 running 且以后 rearm 静默失败，
 * 适合释放 timer 容器；仍不能持 callback 为完成所需的锁。
 */
extern int timer_shutdown_sync(struct timer_list *timer);
/*
 * timer_shutdown() - 删除 pending 并永久禁止 rearm，但不等待远端 callback。
 * @timer: 稳定借用；返回 0/1。只有容器生命周期由其他协议稳定时才可替代 sync 版本。
 */
extern int timer_shutdown(struct timer_list *timer);

/* timers_init() - 启动期初始化 timer bases、迁移层和 softirq；无参数、返回 void。 */
extern void timers_init(void);
/* hrtimer 前向声明，避免仅声明 it_real_fn 时引入完整高精度 timer 布局。 */
struct hrtimer;
/* it_real_fn() - ITIMER_REAL 高精度回调；@hrtimer 为借用，返回是否重启的枚举值。 */
extern enum hrtimer_restart it_real_fn(struct hrtimer *);

/*
 * __round_jiffies_relative() - 按 @cpu 错峰相位近似整秒取整相对 @j。
 * @j: 相对 jiffies；@cpu: 仅选择相位。无锁、不睡眠，返回仍在未来的相对期限；
 * 允许轻微提前或延后以合并唤醒，不固定 timer 的最终执行 CPU。
 */
unsigned long __round_jiffies_relative(unsigned long j, int cpu);
/* round_jiffies() - 对绝对 @j 用当前 CPU 相位取整；返回绝对 jiffy，无副作用。 */
unsigned long round_jiffies(unsigned long j);
/* round_jiffies_relative() - 当前 CPU 包装；@j/返回值均为相对 jiffies。 */
unsigned long round_jiffies_relative(unsigned long j);

/*
 * __round_jiffies_up_relative() - 只向后取整相对 @j，@cpu 选择错峰相位。
 * 适合“可以晚、不能早”的超时；返回相对 jiffies，不修改任何 timer。
 */
unsigned long __round_jiffies_up_relative(unsigned long j, int cpu);
/* round_jiffies_up() - 当前 CPU 相位的绝对期限向上取整，不会提前。 */
unsigned long round_jiffies_up(unsigned long j);
/* round_jiffies_up_relative() - 当前 CPU 相位的相对期限向上取整。 */
unsigned long round_jiffies_up_relative(unsigned long j);

#ifdef CONFIG_HOTPLUG_CPU
/* timers_prepare_cpu() - 上线前初始化 @cpu 的所有 timer base；hotplug 串行，返回 0。 */
int timers_prepare_cpu(unsigned int cpu);
/* timers_dead_cpu() - 把下线 @cpu 的 timer 迁到当前在线 CPU；hotplug 串行，返回 0。 */
int timers_dead_cpu(unsigned int cpu);
#else
/* 未启用 CPU hotplug 时用 NULL 回调占位，注册者不会获得可调用函数。 */
#define timers_prepare_cpu	NULL
#define timers_dead_cpu		NULL
#endif

#if defined(CONFIG_SMP) && defined(CONFIG_NO_HZ_COMMON)
/*
 * tmigr_isolated_exclude_cpumask() - 让 timer migration 层排除指定隔离 CPU 集合。
 * @exclude_cpumask: 输入掩码借用，函数复制所需状态；可睡眠，返回 0 或 -ENOMEM。
 */
extern int tmigr_isolated_exclude_cpumask(struct cpumask *exclude_cpumask);
#else
/* 不具备 SMP/NO_HZ migration 时为空成功；@exclude_cpumask 不读取也不修改。 */
static inline int tmigr_isolated_exclude_cpumask(struct cpumask *exclude_cpumask)
{
	/* 配置裁剪后的稳定返回值让调用者无需条件编译错误处理。 */
	return 0;
}
#endif

/* 结束 timer API 的 include guard。 */
#endif
