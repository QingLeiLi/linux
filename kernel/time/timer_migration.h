/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KERNEL_TIME_MIGRATION_H
#define _KERNEL_TIME_MIGRATION_H

/* Per group capacity. Must be a power of 2! */
/* 每组最多 8 个 child，与 u8 active mask 一一对应；必须为 2 的幂，初始化时另有 BUILD_BUG_ON 强制。 */
#define TMIGR_CHILDREN_PER_GROUP 8

/**
 * struct tmigr_hierarchy - a hierarchy associated to a given CPU capacity.
 *                          Homogeneous systems have only one hierarchy.
 *                          Heterogenous have one hierarchy per CPU capacity.
 * @cpumask:	CPUs belonging to this hierarchy
 * @root:	The current root of the hierarchy
 * @capacity:	CPU capacity associated to this hierarchy
 * @node:	Node in the global hierarchy list
 * @level_list:	Per level lists of tmigr groups
 */
/*
 * 一个 CPU capacity 类别对应一棵 hierarchy：cpumask 由对象拥有并记录已接入 CPU，root 随首次 online 扩层而
 * 只向上替换；node 把对象挂入全局常驻列表，柔性 level_list[] 由 hierarchy 分配一并拥有并按层串联 groups。
 * 对象在 tmigr_mutex 下创建/建链，CPU offline 不回收，因此运行期 walk 可借用 root/group 指针。
 */
struct tmigr_hierarchy {
	struct cpumask		*cpumask;
	struct tmigr_group	*root;
	unsigned long		capacity;
	struct list_head	node;
	struct list_head	level_list[];
};

/**
 * struct tmigr_event - a timer event associated to a CPU
 * @nextevt:	The node to enqueue an event in the parent group queue
 * @cpu:	The CPU to which this event belongs
 * @ignore:	Hint whether the event could be ignored; it is set when
 *		CPU or group is active;
 */
/*
 * CPU 或 group 向 parent 汇报的内嵌事件：nextevt 是 parent timerqueue 节点，cpu 始终指向实际 leaf owner；
 * ignore 是允许惰性删除的提示，child active 或没有可迁移事件时置位。事件存储由 tmigr_cpu/tmigr_group 持有，
 * 入队不转移所有权，修改 expiry/owner/队列关系受 parent group 锁保护，active 路径可无锁置 ignore。
 */
struct tmigr_event {
	struct timerqueue_node	nextevt;
	unsigned int		cpu;
	bool			ignore;
};

/**
 * struct tmigr_group - timer migration hierarchy group
 * @lock:		Lock protecting the event information and group hierarchy
 *			information during setup
 * @parent:		Pointer to the parent group. Pointer is updated when a
 *			new hierarchy level is added because of a CPU coming
 *			online the first time. Once it is set, the pointer will
 *			not be removed or updated. When accessing parent pointer
 *			lock less to decide whether to abort a propagation or
 *			not, it is not a problem. The worst outcome is an
 *			unnecessary/early CPU wake up. But do not access parent
 *			pointer several times in the same 'action' (like
 *			activation, deactivation, check for remote expiry,...)
 *			without holding the lock as it is not ensured that value
 *			will not change.
 * @groupevt:		Next event of the group which is only used when the
 *			group is !active. The group event is then queued into
 *			the parent timer queue.
 *			Ignore bit of @groupevt is set when the group is active.
 * @next_expiry:	Base monotonic expiry time of the next event of the
 *			group; It is used for the racy lockless check whether a
 *			remote expiry is required; it is always reliable
 * @events:		Timer queue for child events queued in the group
 * @migr_state:		State of the group (see union tmigr_state)
 * @level:		Hierarchy level of the group; Required during setup
 * @numa_node:		Required for setup only to make sure CPU and low level
 *			group information is NUMA local. It is set to NUMA node
 *			as long as the group level is per NUMA node (level <
 *			tmigr_crossnode_level); otherwise it is set to
 *			NUMA_NO_NODE
 * @num_children:	Counter of group children to make sure the group is only
 *			filled with TMIGR_CHILDREN_PER_GROUP; Required for setup
 *			only
 * @groupmask:		mask of the group in the parent group; is set during
 *			setup and will never change; can be read lockless
 * @list:		List head that is added to the per level
 *			tmigr_level_list; is required during setup when a
 *			new group needs to be connected to the existing
 *			hierarchy groups
 */
/*
 * hierarchy 中间节点：lock 保护运行期 events、next_expiry/groupevt 队列信息；parent 一经 release 发布
 * 只会从 NULL 变为常驻上层，单次 action 应缓存一次无锁读。groupevt 只在本组 inactive 时进入 parent 队列；
 * next_expiry 是可无锁保守读取的本组有效队首。migr_state 原子封装 active/migrator/seq，不依赖 group 锁。
 * level/numa_node/num_children/groupmask/list 仅由 tmigr_mutex 下的 setup 写入；groupmask 发布后永不改变。
 */
struct tmigr_group {
	raw_spinlock_t		lock;
	struct tmigr_group	*parent;
	struct tmigr_event	groupevt;
	u64			next_expiry;
	struct timerqueue_head	events;
	atomic_t		migr_state;
	unsigned int		level;
	int			numa_node;
	unsigned int		num_children;
	u8			groupmask;
	struct list_head	list;
};

/**
 * struct tmigr_cpu - timer migration per CPU group
 * @lock:		Lock protecting the tmigr_cpu group information
 * @available:		Indicates whether the CPU is available for handling
 *			global timers. In the deactivate path it is required to
 *			know whether the migrator in the top level group is to
 *			be set offline, while a timer is pending. Then another
 *			available CPU needs to be notified to take over the
 *			migrator role. Furthermore the information is required
 *			in the CPU hotplug path as the CPU is able to go idle
 *			before the timer migration hierarchy hotplug callback is
 *			reached.  During this phase, the CPU has to handle the
 *			global timers on its own and must not act as a migrator.

 * @idle:		Indicates whether the CPU is idle in the timer migration
 *			hierarchy
 * @remote:		Is set when timers of the CPU are expired remotely
 * @tmgroup:		Pointer to the parent group
 * @groupmask:		mask of tmigr_cpu in the parent group
 * @wakeup:		Stores the first timer when the timer migration
 *			hierarchy is completely idle and remote expiry was done;
 *			is returned to timer code in the idle path and is only
 *			used in idle path.
 * @cpuevt:		CPU event which could be enqueued into the parent group
 */
/*
 * 每 CPU 叶节点状态：lock 串行 idle/available/remote、wakeup 与 cpuevt 更新；available 表示可承担全局 timer，
 * hotplug 摘除后即使本地暂时 idle 也必须自行处理。idle 是 migration 视角而非通用调度状态；remote 防止多个
 * migrator 同时拉取该 CPU。tmgroup/groupmask 首次 prepare 建立后常驻。wakeup 只供 idle 路径保存整个层次无人
 * active 或 remote walk 后本 CPU 必须兜底的最早 deadline；cpuevt 可借用入 leaf group 队列。
 */
struct tmigr_cpu {
	raw_spinlock_t		lock;
	bool			available;
	bool			idle;
	bool			remote;
	struct tmigr_group	*tmgroup;
	u8			groupmask;
	u64			wakeup;
	struct tmigr_event	cpuevt;
};

/**
 * union tmigr_state - state of tmigr_group
 * @state:	Combined version of the state - only used for atomic
 *		read/cmpxchg function
 * &anon struct: Split version of the state - only use the struct members to
 *		update information to stay independent of endianness
 * @active:	Contains each mask bit of the active children
 * @migrator:	Contains mask of the child which is migrator
 * @seq:	Sequence counter needs to be increased when an update
 *		to the tmigr_state is done. It prevents a race when
 *		updates in the child groups are propagated in changed
 *		order. Detailed information about the scenario is
 *		given in the documentation at the begin of
 *		timer_migration.c.
 */
/*
 * group 的 32 位原子状态同时提供整体 state 与按字段构造的新值：active 是 8 个 child bit，migrator 保存其中
 * 唯一负责者的 bit 或 TMIGR_NONE，seq 每次更新递增以拒绝跨层乱序的旧传播。更新者只修改匿名结构字段后对
 * 整体 state cmpxchg，避免依赖大小端下的位位置；packed 保证布局恰为 u32。
 */
union tmigr_state {
	u32 state;
	struct {
		u8	active;
		u8	migrator;
		u16	seq;
	} __packed;
};

#if defined(CONFIG_SMP) && defined(CONFIG_NO_HZ_COMMON)
/*
 * timer softirq/IRQ-exit 接口：handle 执行远端到期，requires 在 IRQ-off 下预测是否需要执行；activate/deactivate
 * 在本 CPU idle 状态切换时调用。deactivate/new_timer/quick_check 以 KTIME_MAX 或绝对 expiry 与 timer idle
 * 路径交换下一唤醒责任。所有对象均借用 per-CPU/hierarchy 常驻存储，接口不转移所有权。
 */
extern void tmigr_handle_remote(void);
extern bool tmigr_requires_handle_remote(void);
extern void tmigr_cpu_activate(void);
extern u64 tmigr_cpu_deactivate(u64 nextevt);
extern u64 tmigr_cpu_new_timer(u64 nextevt);
extern u64 tmigr_quick_check(u64 nextevt);
#else
/* 无 SMP 或通用 NO_HZ 时不存在远端迁移：无条件调用入口为空，预测恒 false。 */
static inline void tmigr_handle_remote(void) { }
static inline bool tmigr_requires_handle_remote(void) { return false; }
static inline void tmigr_cpu_activate(void) { }
/* 返回 deadline 的三个接口只出现在相同配置保护的 timer idle 路径，故这里无需伪造返回值 stub。 */
#endif

#endif
