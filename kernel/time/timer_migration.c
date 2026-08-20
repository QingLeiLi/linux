// SPDX-License-Identifier: GPL-2.0-only
/*
 * Infrastructure for migratable timers
 *
 * Copyright(C) 2022 linutronix GmbH
 */
#include <linux/cpuhotplug.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/timerqueue.h>
#include <trace/events/ipi.h>
#include <linux/sched/isolation.h>

#include "timer_migration.h"
#include "tick-internal.h"

#define CREATE_TRACE_POINTS
#include <trace/events/timer_migration.h>

/*
 * The timer migration mechanism is built on a hierarchy of groups. The
 * lowest level group contains CPUs, the next level groups of CPU groups
 * and so forth. The CPU groups are kept per node so for the normal case
 * lock contention won't happen across nodes. Depending on the number of
 * CPUs per node even the next level might be kept as groups of CPU groups
 * per node and only the levels above cross the node topology.
 *
 * Example topology for a two node system with 24 CPUs each.
 *
 * LVL 2                           [GRP2:0]
 *                              GRP1:0 = GRP1:M
 *
 * LVL 1            [GRP1:0]                      [GRP1:1]
 *               GRP0:0 - GRP0:2               GRP0:3 - GRP0:5
 *
 * LVL 0  [GRP0:0]  [GRP0:1]  [GRP0:2]  [GRP0:3]  [GRP0:4]  [GRP0:5]
 * CPUS     0-7       8-15      16-23     24-31     32-39     40-47
 *
 * The groups hold a timer queue of events sorted by expiry time. These
 * queues are updated when CPUs go in idle. When they come out of idle
 * ignore flag of events is set.
 *
 * Each group has a designated migrator CPU/group as long as a CPU/group is
 * active in the group. This designated role is necessary to avoid that all
 * active CPUs in a group try to migrate expired timers from other CPUs,
 * which would result in massive lock bouncing.
 *
 * When a CPU is awake, it checks in it's own timer tick the group
 * hierarchy up to the point where it is assigned the migrator role or if
 * no CPU is active, it also checks the groups where no migrator is set
 * (TMIGR_NONE).
 *
 * If it finds expired timers in one of the group queues it pulls them over
 * from the idle CPU and runs the timer function. After that it updates the
 * group and the parent groups if required.
 *
 * CPUs which go idle arm their CPU local timer hardware for the next local
 * (pinned) timer event. If the next migratable timer expires after the
 * next local timer or the CPU has no migratable timer pending then the
 * CPU does not queue an event in the LVL0 group. If the next migratable
 * timer expires before the next local timer then the CPU queues that timer
 * in the LVL0 group. In both cases the CPU marks itself idle in the LVL0
 * group.
 *
 * When CPU comes out of idle and when a group has at least a single active
 * child, the ignore flag of the tmigr_event is set. This indicates, that
 * the event is ignored even if it is still enqueued in the parent groups
 * timer queue. It will be removed when touching the timer queue the next
 * time. This spares locking in active path as the lock protects (after
 * setup) only event information. For more information about locking,
 * please read the section "Locking rules".
 *
 * If the CPU is the migrator of the group then it delegates that role to
 * the next active CPU in the group or sets migrator to TMIGR_NONE when
 * there is no active CPU in the group. This delegation needs to be
 * propagated up the hierarchy so hand over from other leaves can happen at
 * all hierarchy levels w/o doing a search.
 *
 * When the last CPU in the system goes idle, then it drops all migrator
 * duties up to the top level of the hierarchy (LVL2 in the example). It
 * then has to make sure, that it arms it's own local hardware timer for
 * the earliest event in the system.
 *
 *
 * Lifetime rules:
 * ---------------
 *
 * The groups are built up at init time or when CPUs come online. They are
 * not destroyed when a group becomes empty due to offlining. The group
 * just won't participate in the hierarchy management anymore. Destroying
 * groups would result in interesting race conditions which would just make
 * the whole mechanism slow and complex.
 *
 *
 * Locking rules:
 * --------------
 *
 * For setting up new groups and handling events it's required to lock both
 * child and parent group. The lock ordering is always bottom up. This also
 * includes the per CPU locks in struct tmigr_cpu. For updating the migrator and
 * active CPU/group information atomic_try_cmpxchg() is used instead and only
 * the per CPU tmigr_cpu->lock is held.
 *
 * During the setup of groups, hier->level_list is required. It is protected by
 * @tmigr_mutex.
 *
 * When @timer_base->lock as well as tmigr related locks are required, the lock
 * ordering is: first @timer_base->lock, afterwards tmigr related locks.
 *
 *
 * Protection of the tmigr group state information:
 * ------------------------------------------------
 *
 * The state information with the list of active children and migrator needs to
 * be protected by a sequence counter. It prevents a race when updates in child
 * groups are propagated in changed order. The state update is performed
 * lockless and group wise. The following scenario describes what happens
 * without updating the sequence counter:
 *
 * Therefore, let's take three groups and four CPUs (CPU2 and CPU3 as well
 * as GRP0:1 will not change during the scenario):
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = GRP0:1
 *                     active   = GRP0:0, GRP0:1
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = CPU0           migrator = CPU2
 *           active   = CPU0           active   = CPU2
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             active      idle           active      idle
 *
 *
 * 1. CPU0 goes idle. As the update is performed group wise, in the first step
 *    only GRP0:0 is updated. The update of GRP1:0 is pending as CPU0 has to
 *    walk the hierarchy.
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = GRP0:1
 *                     active   = GRP0:0, GRP0:1
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *       --> migrator = TMIGR_NONE     migrator = CPU2
 *       --> active   =                active   = CPU2
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *         --> idle        idle           active      idle
 *
 * 2. While CPU0 goes idle and continues to update the state, CPU1 comes out of
 *    idle. CPU1 updates GRP0:0. The update for GRP1:0 is pending as CPU1 also
 *    has to walk the hierarchy. Both CPUs (CPU0 and CPU1) now walk the
 *    hierarchy to perform the needed update from their point of view. The
 *    currently visible state looks the following:
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = GRP0:1
 *                     active   = GRP0:0, GRP0:1
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *       --> migrator = CPU1           migrator = CPU2
 *       --> active   = CPU1           active   = CPU2
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle    --> active         active      idle
 *
 * 3. Here is the race condition: CPU1 managed to propagate its changes (from
 *    step 2) through the hierarchy to GRP1:0 before CPU0 (step 1) did. The
 *    active members of GRP1:0 remain unchanged after the update since it is
 *    still valid from CPU1 current point of view:
 *
 *    LVL 1            [GRP1:0]
 *                 --> migrator = GRP0:1
 *                 --> active   = GRP0:0, GRP0:1
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = CPU1           migrator = CPU2
 *           active   = CPU1           active   = CPU2
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        active         active      idle
 *
 * 4. Now CPU0 finally propagates its changes (from step 1) to GRP1:0.
 *
 *    LVL 1            [GRP1:0]
 *                 --> migrator = GRP0:1
 *                 --> active   = GRP0:1
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = CPU1           migrator = CPU2
 *           active   = CPU1           active   = CPU2
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        active         active      idle
 *
 *
 * The race of CPU0 vs. CPU1 led to an inconsistent state in GRP1:0. CPU1 is
 * active and is correctly listed as active in GRP0:0. However GRP1:0 does not
 * have GRP0:0 listed as active, which is wrong. The sequence counter has been
 * added to avoid inconsistent states during updates. The state is updated
 * atomically only if all members, including the sequence counter, match the
 * expected value (compare-and-exchange).
 *
 * Looking back at the previous example with the addition of the sequence
 * counter: The update as performed by CPU0 in step 4 will fail. CPU1 changed
 * the sequence number during the update in step 3 so the expected old value (as
 * seen by CPU0 before starting the walk) does not match.
 *
 * Prevent race between new event and last CPU going inactive
 * ----------------------------------------------------------
 *
 * When the last CPU is going idle and there is a concurrent update of a new
 * first global timer of an idle CPU, the group and child states have to be read
 * while holding the lock in tmigr_update_events(). The following scenario shows
 * what happens, when this is not done.
 *
 * 1. Only CPU2 is active:
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = GRP0:1
 *                     active   = GRP0:1
 *                     next_expiry = KTIME_MAX
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = TMIGR_NONE     migrator = CPU2
 *           active   =                active   = CPU2
 *           next_expiry = KTIME_MAX   next_expiry = KTIME_MAX
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        idle           active      idle
 *
 * 2. Now CPU 2 goes idle (and has no global timer, that has to be handled) and
 *    propagates that to GRP0:1:
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = GRP0:1
 *                     active   = GRP0:1
 *                     next_expiry = KTIME_MAX
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = TMIGR_NONE --> migrator = TMIGR_NONE
 *           active   =            --> active   =
 *           next_expiry = KTIME_MAX   next_expiry = KTIME_MAX
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        idle       --> idle        idle
 *
 * 3. Now the idle state is propagated up to GRP1:0. As this is now the last
 *    child going idle in top level group, the expiry of the next group event
 *    has to be handed back to make sure no event is lost. As there is no event
 *    enqueued, KTIME_MAX is handed back to CPU2.
 *
 *    LVL 1            [GRP1:0]
 *                 --> migrator = TMIGR_NONE
 *                 --> active   =
 *                     next_expiry = KTIME_MAX
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = TMIGR_NONE     migrator = TMIGR_NONE
 *           active   =                active   =
 *           next_expiry = KTIME_MAX   next_expiry = KTIME_MAX
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        idle       --> idle        idle
 *
 * 4. CPU 0 has a new timer queued from idle and it expires at TIMER0. CPU0
 *    propagates that to GRP0:0:
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = TMIGR_NONE
 *                     active   =
 *                     next_expiry = KTIME_MAX
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = TMIGR_NONE     migrator = TMIGR_NONE
 *           active   =                active   =
 *       --> next_expiry = TIMER0      next_expiry  = KTIME_MAX
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        idle           idle        idle
 *
 * 5. GRP0:0 is not active, so the new timer has to be propagated to
 *    GRP1:0. Therefore the GRP1:0 state has to be read. When the stalled value
 *    (from step 2) is read, the timer is enqueued into GRP1:0, but nothing is
 *    handed back to CPU0, as it seems that there is still an active child in
 *    top level group.
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = TMIGR_NONE
 *                     active   =
 *                 --> next_expiry = TIMER0
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = TMIGR_NONE     migrator = TMIGR_NONE
 *           active   =                active   =
 *           next_expiry = TIMER0      next_expiry  = KTIME_MAX
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        idle           idle        idle
 *
 * This is prevented by reading the state when holding the lock (when a new
 * timer has to be propagated from idle path)::
 *
 *   CPU2 (tmigr_inactive_up())          CPU0 (tmigr_new_timer_up())
 *   --------------------------          ---------------------------
 *   // step 3:
 *   cmpxchg(&GRP1:0->state);
 *   tmigr_update_events() {
 *       spin_lock(&GRP1:0->lock);
 *       // ... update events ...
 *       // hand back first expiry when GRP1:0 is idle
 *       spin_unlock(&GRP1:0->lock);
 *       // ^^^ release state modification
 *   }
 *                                       tmigr_update_events() {
 *                                           spin_lock(&GRP1:0->lock)
 *                                           // ^^^ acquire state modification
 *                                           group_state = atomic_read(&GRP1:0->state)
 *                                           // .... update events ...
 *                                           // hand back first expiry when GRP1:0 is idle
 *                                           spin_unlock(&GRP1:0->lock) <3>
 *                                           // ^^^ makes state visible for other
 *                                           // callers of tmigr_new_timer_up()
 *                                       }
 *
 * When CPU0 grabs the lock directly after cmpxchg, the first timer is reported
 * back to CPU0 and also later on to CPU2. So no timer is missed. A concurrent
 * update of the group state from active path is no problem, as the upcoming CPU
 * will take care of the group events.
 *
 * Required event and timerqueue update after a remote expiry:
 * -----------------------------------------------------------
 *
 * After expiring timers of a remote CPU, a walk through the hierarchy and
 * update of events and timerqueues is required. It is obviously needed if there
 * is a 'new' global timer but also if there is no new global timer but the
 * remote CPU is still idle.
 *
 * 1. CPU0 and CPU1 are idle and have both a global timer expiring at the same
 *    time. So both have an event enqueued in the timerqueue of GRP0:0. CPU3 is
 *    also idle and has no global timer pending. CPU2 is the only active CPU and
 *    thus also the migrator:
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = GRP0:1
 *                     active   = GRP0:1
 *                 --> timerqueue = evt-GRP0:0
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = TMIGR_NONE     migrator = CPU2
 *           active   =                active   = CPU2
 *           groupevt.ignore = false   groupevt.ignore = true
 *           groupevt.cpu = CPU0       groupevt.cpu =
 *           timerqueue = evt-CPU0,    timerqueue =
 *                        evt-CPU1
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        idle           active      idle
 *
 * 2. CPU2 starts to expire remote timers. It starts with LVL0 group
 *    GRP0:1. There is no event queued in the timerqueue, so CPU2 continues with
 *    the parent of GRP0:1: GRP1:0. In GRP1:0 it dequeues the first event. It
 *    looks at tmigr_event::cpu struct member and expires the pending timer(s)
 *    of CPU0.
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = GRP0:1
 *                     active   = GRP0:1
 *                 --> timerqueue =
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = TMIGR_NONE     migrator = CPU2
 *           active   =                active   = CPU2
 *           groupevt.ignore = false   groupevt.ignore = true
 *       --> groupevt.cpu = CPU0       groupevt.cpu =
 *           timerqueue = evt-CPU0,    timerqueue =
 *                        evt-CPU1
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        idle           active      idle
 *
 * 3. Some work has to be done after expiring the timers of CPU0. If we stop
 *    here, then CPU1's pending global timer(s) will not expire in time and the
 *    timerqueue of GRP0:0 has still an event for CPU0 enqueued which has just
 *    been processed. So it is required to walk the hierarchy from CPU0's point
 *    of view and update it accordingly. CPU0's event will be removed from the
 *    timerqueue because it has no pending timer. If CPU0 would have a timer
 *    pending then it has to expire after CPU1's first timer because all timers
 *    from this period were just expired. Either way CPU1's event will be first
 *    in GRP0:0's timerqueue and therefore set in the CPU field of the group
 *    event which is then enqueued in GRP1:0's timerqueue as GRP0:0 is still not
 *    active:
 *
 *    LVL 1            [GRP1:0]
 *                     migrator = GRP0:1
 *                     active   = GRP0:1
 *                 --> timerqueue = evt-GRP0:0
 *                   /                \
 *    LVL 0  [GRP0:0]                  [GRP0:1]
 *           migrator = TMIGR_NONE     migrator = CPU2
 *           active   =                active   = CPU2
 *           groupevt.ignore = false   groupevt.ignore = true
 *       --> groupevt.cpu = CPU1       groupevt.cpu =
 *       --> timerqueue = evt-CPU1     timerqueue =
 *              /         \                /         \
 *    CPUs     0           1              2           3
 *             idle        idle           active      idle
 *
 * Now CPU2 (migrator) will continue step 2 at GRP1:0 and will expire the
 * timer(s) of CPU1.
 *
 * The hierarchy walk in step 3 can be skipped if the migrator notices that a
 * CPU of GRP0:0 is active again. The CPU will mark GRP0:0 active and take care
 * of the group as migrator and any needed updates within the hierarchy.
 */
/*
 * 设计语义地图：
 *
 * 1. 层次结构：第 0 层 group 的 child 是 CPU，更高层的 child 是下一层 group；每个 group 最多容纳 8 个
 *    child，并按 NUMA node 尽量局部聚合，只有 crossnode_level 以上才跨 node。每层 timerqueue 保存各 idle
 *    child 的最早可迁移事件，根的最早事件代表当前无人 active 时仍必须有人按时唤醒处理的全局 deadline。
 * 2. migrator：只让每组一个 active child 承担向下拉取过期 timer 的职责，避免全部 active CPU 争抢同一组锁。
 *    child idle 时把职责交给另一个 active child；最后一个 active child idle 后 migrator 变 TMIGR_NONE，并把
 *    变化逐层上传。最后离开的 CPU 必须用根最早 deadline 编程本地硬件，保证全系统 idle 也不丢全局 timer。
 * 3. 事件惰性删除：CPU/child 再次 active 时只无锁置 groupevt.ignore，父队列里的旧节点留到下次持锁触碰时
 *    清理，以缩短唤醒快路径。idle 路径在锁下更新 ignore、queue 和 next_expiry，避免并发新 timer 被遗漏。
 * 4. 状态并发：migr_state 把 active mask、migrator 和 seq 放进一次 atomic cmpxchg。seq 使晚到的旧层次传播
 *    无法覆盖较新的 activate 结果；inactive 传播以 acquire/失败后的 barrier 与 activate 的 cmpxchg 排序。
 * 5. 锁序与生命周期：同时需要 timer_base 和 migration 锁时必须先 timer_base，再按 CPU/group 自底向上加锁；
 *    group 建链由 tmigr_mutex 串行，并以 release 发布 parent。group 在 CPU offline 后保留而不销毁，从根源上
 *    避免并发 walk 遇到回收对象。remote expiry 临时释放 CPU migration 锁后拉取 timer，再按规定锁序重取。
 * 6. 两类关键竞争：最后 CPU idle 与 idle CPU 新首 timer 并发时，tmigr_update_events() 必须在 group/child 锁下
 *    同时观察状态和事件；remote expiry 后即使暂时没有新事件，也必须重新向上更新队列，防止 inactive group
 *    中的后续 timer 失去代理。若目标 CPU 已唤醒/下线/正由别人处理，则 remote 路径退出并由新 owner 接管。
 */

/* 串行 hierarchy/group 的创建、跨层连接与 root 替换；运行期 event walk 不依赖此锁。 */
static DEFINE_MUTEX(tmigr_mutex);

/* 所有按 capacity 分流的 hierarchy 常驻链表，节点在初始化/CPU prepare 中创建后不删除。 */
static LIST_HEAD(tmigr_hierarchy_list);

/* 启动期估算的总层数与首个跨 NUMA 层，只读热路径频繁使用，初始化完成后保持不变。 */
static unsigned int tmigr_hierarchy_levels __read_mostly;
static unsigned int tmigr_crossnode_level __read_mostly;

/* 每 CPU 的 leaf event、所属 group、idle/available/remote 状态和锁；对象静态常驻。 */
static DEFINE_PER_CPU(struct tmigr_cpu, tmigr_cpu);

/*
 * CPUs available for timer migration.
 * Protected by cpuset_mutex (with cpus_read_lock held) or cpus_write_lock.
 * Additionally tmigr_available_mutex serializes set/clear operations with each other.
 */
/*
 * 可参与 timer migration 的 CPU 集合：cpuset_mutex+cpus_read_lock 或 cpus_write_lock 稳定 hotplug/cpuset 视图，
 * tmigr_available_mutex 额外串行 set/clear 及层次状态转换，使 mask 与 per-CPU available 字段同步提交。
 */
static cpumask_var_t tmigr_available_cpumask;
static DEFINE_MUTEX(tmigr_available_mutex);

/* Enabled during late initcall */
/* late init 完成首次隔离同步后才启用，避免早期启动在层次尚未可用时误排除 CPU。 */
static DEFINE_STATIC_KEY_FALSE(tmigr_exclude_isolated);

#define TMIGR_NONE	0xFF
#define BIT_CNT		8

/* CPU 未接入 leaf group 或已被 hotplug/isolation 标为 unavailable 时返回 true，公共入口据此退回本地 timer 路径。 */
static inline bool tmigr_is_not_available(struct tmigr_cpu *tmc)
{
	return !(tmc->tmgroup && tmc->available);
}

/*
 * Returns true if @cpu should be excluded from the hierarchy as isolated.
 * Domain isolated CPUs don't participate in timer migration, nohz_full CPUs
 * are still part of the hierarchy but become idle (from a tick and timer
 * migration perspective) when they stop their tick. This lets the timekeeping
 * CPU handle their global timers. Marking also isolated CPUs as idle would be
 * too costly, hence they are completely excluded from the hierarchy.
 * This check is necessary, for instance, to prevent offline isolated CPUs from
 * being incorrectly marked as available once getting back online.
 *
 * This function returns false during early boot and the isolation logic is
 * enabled only after isolated CPUs are marked as unavailable at late boot.
 * The tick CPU can be isolated at boot, however we cannot mark it as
 * unavailable to avoid having no global migrator for the nohz_full CPUs. This
 * should be ensured by the callers of this function: implicitly from hotplug
 * callbacks and explicitly in tmigr_init_isolation() and
 * tmigr_isolated_exclude_cpumask().
 */
/*
 * 判断 CPU 是否应以 DOMAIN 隔离身份完全排除。nohz_full CPU 仍留在层次中并在停 tick 时表现为 idle，以便
 * timekeeper 代管其全局 timer；只有非 DOMAIN housekeeper 且仍属于 KERNEL_NOISE housekeeper 的 CPU 被排除。
 * static key 在 late init 完成初次清理前恒使结果为 false；调用者还必须保留不可下线的 tick CPU。
 */
static inline bool tmigr_is_isolated(int cpu)
{
	if (!static_branch_unlikely(&tmigr_exclude_isolated))
		return false;
	return (!housekeeping_cpu(cpu, HK_TYPE_DOMAIN) &&
		housekeeping_cpu(cpu, HK_TYPE_KERNEL_NOISE));
}

/*
 * Returns true, when @childmask corresponds to the group migrator or when the
 * group is not active - so no migrator is set.
 */
/* 无锁快照 group 原子状态；child 是当前 migrator，或组已无 active child/migrator 时返回 true。允许瞬时陈旧。 */
static bool tmigr_check_migrator(struct tmigr_group *group, u8 childmask)
{
	union tmigr_state s;

	s.state = atomic_read(&group->migr_state);

	if ((s.migrator == childmask) || (s.migrator == TMIGR_NONE))
		return true;

	return false;
}

/*
 * 一次原子快照同时检查当前 child 有无迁移职责，以及 active child 数是否至多为 1；用于 idle 前的保守预测，
 * 状态并发变化只会导致多一次唤醒或走完整路径，不作为最终提交依据。
 */
static bool tmigr_check_migrator_and_lonely(struct tmigr_group *group, u8 childmask)
{
	bool lonely, migrator = false;
	unsigned long active;
	union tmigr_state s;

	s.state = atomic_read(&group->migr_state);

	if ((s.migrator == childmask) || (s.migrator == TMIGR_NONE))
		migrator = true;

	active = s.active;
	lonely = bitmap_weight(&active, BIT_CNT) <= 1;

	return (migrator && lonely);
}

/* 从一次原子状态快照判断组内 active child 是否不超过一个；仅供无锁 quick check 的保守层次扫描。 */
static bool tmigr_check_lonely(struct tmigr_group *group)
{
	unsigned long active;
	union tmigr_state s;

	s.state = atomic_read(&group->migr_state);

	active = s.active;

	return bitmap_weight(&active, BIT_CNT) <= 1;
}

/**
 * struct tmigr_walk - data required for walking the hierarchy
 * @nextexp:		Next CPU event expiry information which is handed into
 *			the timer migration code by the timer code
 *			(get_next_timer_interrupt())
 * @firstexp:		Contains the first event expiry information when
 *			hierarchy is completely idle.  When CPU itself was the
 *			last going idle, information makes sure, that CPU will
 *			be back in time. When using this value in the remote
 *			expiry case, firstexp is stored in the per CPU tmigr_cpu
 *			struct of CPU which expires remote timers. It is updated
 *			in top level group only. Be aware, there could occur a
 *			new top level of the hierarchy between the 'top level
 *			call' in tmigr_update_events() and the check for the
 *			parent group in walk_groups(). Then @firstexp might
 *			contain a value != KTIME_MAX even if it was not the
 *			final top level. This is not a problem, as the worst
 *			outcome is a CPU which might wake up a little early.
 * @evt:		Pointer to tmigr_event which needs to be queued (of idle
 *			child group)
 * @childmask:		groupmask of child group
 * @remote:		Is set, when the new timer path is executed in
 *			tmigr_handle_remote_cpu()
 * @basej:		timer base in jiffies
 * @now:		timer base monotonic
 * @check:		is set if there is the need to handle remote timers;
 *			required in tmigr_requires_handle_remote() only
 */
/*
 * 层次 walk 的跨层状态包：nextexp 是 leaf 新事件，firstexp 汇总全层次 idle 时必须本 CPU 唤醒的最早事件；
 * evt/childmask 标识刚离开的 child。remote 区分远端 expiry 后的强制队列修复，basej/now 固定一次处理的时间
 * 快照，check 只承载“已到期”判定。walk 期间 parent 增长最多造成 firstexp 偏早，不会造成 timer 迟到。
 */
struct tmigr_walk {
	u64			nextexp;
	u64			firstexp;
	struct tmigr_event	*evt;
	u8			childmask;
	bool			remote;
	unsigned long		basej;
	u64			now;
	bool			check;
};

typedef bool (*up_f)(struct tmigr_group *, struct tmigr_group *, struct tmigr_walk *);

/*
 * 从给定 child/group 向根调用 up 回调，回调返回 true 即停止；每上移一层更新 childmask。parent 以 READ_ONCE
 * 获取，与建链的 release store 配对，确保看到完整初始化；层级越界和零 mask 仅 WARN，不改变 walk 控制流。
 */
static void __walk_groups_from(up_f up, struct tmigr_walk *data,
			       struct tmigr_group *child, struct tmigr_group *group)
{
	do {
		WARN_ON_ONCE(group->level >= tmigr_hierarchy_levels);

		if (up(group, child, data))
			break;

		child = group;
		/*
		 * Pairs with the store release on group connection
		 * to make sure group initialization is visible.
		 */
		/* 与 group 建链的 release store 配对，先看到 parent 指针时也必须看到 parent 的完整初始化。 */
		group = READ_ONCE(group->parent);
		data->childmask = child->groupmask;
		WARN_ON_ONCE(!data->childmask);
	} while (group);
}

/* 以当前 CPU 的 leaf group 为起点执行无锁框架 walk；具体回调自行获取所需 group 锁。 */
static void __walk_groups(up_f up, struct tmigr_walk *data,
			  struct tmigr_cpu *tmc)
{
	__walk_groups_from(up, data, NULL, tmc->tmgroup);
}

/* 要求调用者持有 tmc->lock 的包装，固定 leaf event/CPU 状态后再开始向上 walk。 */
static void walk_groups(up_f up, struct tmigr_walk *data, struct tmigr_cpu *tmc)
{
	lockdep_assert_held(&tmc->lock);

	__walk_groups(up, data, tmc);
}

/*
 * Returns the next event of the timerqueue @group->events
 *
 * Removes timers with ignore flag and update next_expiry of the group. Values
 * of the group event are updated in tmigr_update_events() only.
 */
/*
 * 在 group->lock 下取得队列首个非 ignore 事件，并同步 next_expiry；沿途惰性删除 ignore 节点。若队列为空或
 * 删除异常则返回 NULL、next_expiry 保持 KTIME_MAX；这里只清理/选择，不改 groupevt 的 expiry 内容。
 */
static struct tmigr_event *tmigr_next_groupevt(struct tmigr_group *group)
{
	struct timerqueue_node *node = NULL;
	struct tmigr_event *evt = NULL;

	lockdep_assert_held(&group->lock);

	WRITE_ONCE(group->next_expiry, KTIME_MAX);

	while ((node = timerqueue_getnext(&group->events))) {
		evt = container_of(node, struct tmigr_event, nextevt);

		if (!READ_ONCE(evt->ignore)) {
			WRITE_ONCE(group->next_expiry, evt->nextevt.expires);
			return evt;
		}

		/*
		 * Remove next timers with ignore flag, because the group lock
		 * is held anyway
		 */
		/* 既已持有 group 锁，顺便从队首清理 active child 留下的 ignore 事件。 */
		if (!timerqueue_del(&group->events, node))
			break;
	}

	return NULL;
}

/*
 * Return the next event (with the expiry equal or before @now)
 *
 * Event, which is returned, is also removed from the queue.
 */
/*
 * 返回并摘除 expiry <= now 的首个有效 group 事件，同时刷新下一 expiry；无事件或尚未到期返回 NULL，调用者
 * 仍持 group->lock，返回事件对象继续由其 child/CPU 持有。
 */
static struct tmigr_event *tmigr_next_expired_groupevt(struct tmigr_group *group,
						       u64 now)
{
	struct tmigr_event *evt = tmigr_next_groupevt(group);

	if (!evt || now < evt->nextevt.expires)
		return NULL;

	/*
	 * The event is ready to expire. Remove it and update next group event.
	 */
	/* 已到期节点先摘除，再清理 ignore 队首并发布新的 group->next_expiry。 */
	timerqueue_del(&group->events, &evt->nextevt);
	tmigr_next_groupevt(group);

	return evt;
}

/* 在 group->lock 下返回清理 ignore 节点后的有效队首 expiry；无有效事件返回 KTIME_MAX。 */
static u64 tmigr_next_groupevt_expires(struct tmigr_group *group)
{
	struct tmigr_event *evt;

	evt = tmigr_next_groupevt(group);

	if (!evt)
		return KTIME_MAX;
	else
		return evt->nextevt.expires;
}

/*
 * 把当前 child 激活原子传播到 group：必要时认领空缺 migrator，并递增 seq 防旧传播覆盖。若新认领 migrator
 * 返回 false 要求继续向上，否则可停止；随后无锁置 groupevt.ignore，让本组 migrator 接回本组 timer。
 */
static bool tmigr_active_up(struct tmigr_group *group,
			    struct tmigr_group *child,
			    struct tmigr_walk *data)
{
	union tmigr_state curstate, newstate;
	bool walk_done;
	u8 childmask;

	childmask = data->childmask;
	/*
	 * No memory barrier is required here in contrast to
	 * tmigr_inactive_up(), as the group state change does not depend on the
	 * child state.
	 */
	/* activate 只依赖 group 原子状态，不读取 child 状态，因此无需 inactive 路径的 acquire 配对。 */
	curstate.state = atomic_read(&group->migr_state);

	do {
		newstate = curstate;
		walk_done = true;

		if (newstate.migrator == TMIGR_NONE) {
			newstate.migrator = childmask;

			/* Changes need to be propagated */
			/* 组从无人 active 变为 active，父层也必须重新登记本 child。 */
			walk_done = false;
		}

		newstate.active |= childmask;
		newstate.seq++;

	} while (!atomic_try_cmpxchg(&group->migr_state, &curstate.state, newstate.state));

	trace_tmigr_group_set_cpu_active(group, newstate, childmask);

	/*
	 * The group is active (again). The group event might be still queued
	 * into the parent group's timerqueue but can now be handled by the
	 * migrator of this group. Therefore the ignore flag for the group event
	 * is updated to reflect this.
	 *
	 * The update of the ignore flag in the active path is done lockless. In
	 * worst case the migrator of the parent group observes the change too
	 * late and expires remotely all events belonging to this group. The
	 * lock is held while updating the ignore flag in idle path. So this
	 * state change will not be lost.
	 */
	/*
	 * 本组重新 active 后旧父队列节点可忽略；无锁写最坏让父 migrator 多拉取一次，idle 路径持锁写可保证此状态
	 * 不会覆盖掉需要保留的新事件。
	 */
	WRITE_ONCE(group->groupevt.ignore, true);

	return walk_done;
}

/* 在 tmc->lock 下清除 idle 唤醒承诺、忽略 leaf 旧事件，并把 active/migrator 变化向上传播。 */
static void __tmigr_cpu_activate(struct tmigr_cpu *tmc)
{
	struct tmigr_walk data;

	data.childmask = tmc->groupmask;

	trace_tmigr_cpu_active(tmc);

	tmc->cpuevt.ignore = true;
	WRITE_ONCE(tmc->wakeup, KTIME_MAX);

	walk_groups(&tmigr_active_up, &data, tmc);
}

/**
 * tmigr_cpu_activate() - set this CPU active in timer migration hierarchy
 *
 * Call site timer_clear_idle() is called with interrupts disabled.
 */
/*
 * 当前 CPU 退出 timer idle 时调用；要求本地 IRQ 已关闭。未接入/不可用直接返回，非 idle 属协议错误仅 WARN；
 * 否则在 per-CPU 锁下先发布 idle=false，再恢复 leaf 至根的 active/migrator 状态，无返回失败。
 */
void tmigr_cpu_activate(void)
{
	struct tmigr_cpu *tmc = this_cpu_ptr(&tmigr_cpu);

	if (tmigr_is_not_available(tmc))
		return;

	if (WARN_ON_ONCE(!tmc->idle))
		return;

	raw_spin_lock(&tmc->lock);
	tmc->idle = false;
	__tmigr_cpu_activate(tmc);
	raw_spin_unlock(&tmc->lock);
}

/*
 * Returns true, if there is nothing to be propagated to the next level
 *
 * @data->firstexp is set to expiry of first global event of the (top level of
 * the) hierarchy, but only when hierarchy is completely idle.
 *
 * The child and group states need to be read under the lock, to prevent a race
 * against a concurrent tmigr_inactive_up() run when the last CPU goes idle. See
 * also section "Prevent race between new event and last CPU going inactive" in
 * the documentation at the top.
 *
 * This is the only place where the group event expiry value is set.
 */
/*
 * 把 leaf 或 inactive child 的首事件登记/更新到 parent group，并决定是否继续向上。读取 child/group 状态和
 * 队列更新必须同处自底向上的锁区，封闭“最后 CPU idle”与并发新首 timer 的竞争。只有这里写 group event
 * expiry；完全 idle 的根把最早事件写入 data->firstexp。返回 true 表示上层无需继续，false 表示仍需传播。
 */
static
bool tmigr_update_events(struct tmigr_group *group, struct tmigr_group *child,
			 struct tmigr_walk *data)
{
	struct tmigr_event *evt, *first_childevt;
	union tmigr_state childstate, groupstate;
	bool remote = data->remote;
	bool walk_done = false;
	bool ignore;
	u64 nextexp;

	if (child) {
		raw_spin_lock(&child->lock);
		raw_spin_lock_nested(&group->lock, SINGLE_DEPTH_NESTING);

		childstate.state = atomic_read(&child->migr_state);
		groupstate.state = atomic_read(&group->migr_state);

		if (childstate.active) {
			walk_done = true;
			goto unlock;
		}

		first_childevt = tmigr_next_groupevt(child);
		nextexp = child->next_expiry;
		evt = &child->groupevt;

		/*
		 * This can race with concurrent idle exit (activate).
		 * If the current writer wins, a useless remote expiration may
		 * be scheduled. If the activate wins, the event is properly
		 * ignored.
		 */
		/* 与并发 activate 竞争：本写者先行最多安排一次无用 remote expiry；activate 先行则事件被正确忽略。 */
		ignore = (nextexp == KTIME_MAX) ? true : false;
		WRITE_ONCE(evt->ignore, ignore);
	} else {
		nextexp = data->nextexp;

		first_childevt = evt = data->evt;
		ignore = evt->ignore;

		/*
		 * Walking the hierarchy is required in any case when a
		 * remote expiry was done before. This ensures to not lose
		 * already queued events in non active groups (see section
		 * "Required event and timerqueue update after a remote
		 * expiry" in the documentation at the top).
		 *
		 * The two call sites which are executed without a remote expiry
		 * before, are not prevented from propagating changes through
		 * the hierarchy by the return:
		 *  - When entering this path by tmigr_new_timer(), @evt->ignore
		 *    is never set.
		 *  - tmigr_inactive_up() takes care of the propagation by
		 *    itself and ignores the return value. But an immediate
		 *    return is possible if there is a parent, sparing group
		 *    locking at this level, because the upper walking call to
		 *    the parent will take care about removing this event from
		 *    within the group and update next_expiry accordingly.
		 *
		 * However if there is no parent, ie: the hierarchy has only a
		 * single level so @group is the top level group, make sure the
		 * first event information of the group is updated properly and
		 * also handled properly, so skip this fast return path.
		 */
		/*
		 * remote expiry 后必须继续 walk，才能重新暴露 inactive group 中仍排队的事件。普通 new-timer 的 evt
		 * 不会 ignore；inactive 路径自行负责传播。只有非根且无需 remote 修复时，ignore 才可直接结束本层。
		 */
		if (ignore && !remote && group->parent)
			return true;

		raw_spin_lock(&group->lock);

		childstate.state = 0;
		groupstate.state = atomic_read(&group->migr_state);
	}

	/*
	 * If the child event is already queued in the group, remove it from the
	 * queue when the expiry time changed only or when it could be ignored.
	 */
	/* 已排队节点仅在 expiry 改变或应 ignore 时重排；同 expiry 仍要刷新 cpu owner，避免错过新 leaf 首事件。 */
	if (timerqueue_node_queued(&evt->nextevt)) {
		if ((evt->nextevt.expires == nextexp) && !ignore) {
			/* Make sure not to miss a new CPU event with the same expiry */
			/* expiry 相同不代表 owner 未变，必须同步新队首事件的 CPU。 */
			evt->cpu = first_childevt->cpu;
			goto check_toplvl;
		}

		if (!timerqueue_del(&group->events, &evt->nextevt))
			WRITE_ONCE(group->next_expiry, KTIME_MAX);
	}

	if (ignore) {
		/*
		 * When the next child event could be ignored (nextexp is
		 * KTIME_MAX) and there was no remote timer handling before or
		 * the group is already active, there is no need to walk the
		 * hierarchy even if there is a parent group.
		 *
		 * The other way round: even if the event could be ignored, but
		 * if a remote timer handling was executed before and the group
		 * is not active, walking the hierarchy is required to not miss
		 * an enqueued timer in the non active group. The enqueued timer
		 * of the group needs to be propagated to a higher level to
		 * ensure it is handled.
		 */
		/* ignore 通常可停止；但 remote 已发生且组仍 inactive 时必须把组内剩余事件继续向父层登记。 */
		if (!remote || groupstate.active)
			walk_done = true;
	} else {
		evt->nextevt.expires = nextexp;
		evt->cpu = first_childevt->cpu;

		if (timerqueue_add(&group->events, &evt->nextevt))
			WRITE_ONCE(group->next_expiry, nextexp);
	}

check_toplvl:
	if (!group->parent && (groupstate.migrator == TMIGR_NONE)) {
		walk_done = true;

		/*
		 * Nothing to do when update was done during remote timer
		 * handling. First timer in top level group which needs to be
		 * handled when top level group is not active, is calculated
		 * directly in tmigr_handle_remote_up().
		 */
		/* remote handler 自己维护根的下一 wakeup，不在这里改 firstexp。 */
		if (remote)
			goto unlock;

		/*
		 * The top level group is idle and it has to be ensured the
		 * global timers are handled in time. (This could be optimized
		 * by keeping track of the last global scheduled event and only
		 * arming it on the CPU if the new event is earlier. Not sure if
		 * its worth the complexity.)
		 */
		/* 根完全 idle 时无人作为 migrator，调用 CPU 必须用根有效队首设置本地唤醒。 */
		data->firstexp = tmigr_next_groupevt_expires(group);
	}

	trace_tmigr_update_events(child, group, childstate, groupstate,
				  nextexp);

unlock:
	raw_spin_unlock(&group->lock);

	if (child)
		raw_spin_unlock(&child->lock);

	return walk_done;
}

/* new timer/inactive/remote 修复 walk 的薄适配器，直接复用 tmigr_update_events() 的停止语义。 */
static bool tmigr_new_timer_up(struct tmigr_group *group,
			       struct tmigr_group *child,
			       struct tmigr_walk *data)
{
	return tmigr_update_events(group, child, data);
}

/*
 * Returns the expiry of the next timer that needs to be handled. KTIME_MAX is
 * returned, if an active CPU will handle all the timer migration hierarchy
 * timers.
 */
/*
 * 在 tmc->lock 下把 idle CPU 的新全局首 timer 从 leaf 向上登记。CPU 正由 remote expiry 处理时返回 KTIME_MAX，
 * 避免并发改队列；否则清 ignore 并 walk。仅当整个 hierarchy idle 时返回需本 CPU 兜底唤醒的根最早 expiry。
 */
static u64 tmigr_new_timer(struct tmigr_cpu *tmc, u64 nextexp)
{
	struct tmigr_walk data = { .nextexp = nextexp,
				   .firstexp = KTIME_MAX,
				   .evt = &tmc->cpuevt };

	lockdep_assert_held(&tmc->lock);

	if (tmc->remote)
		return KTIME_MAX;

	trace_tmigr_cpu_new_timer(tmc);

	tmc->cpuevt.ignore = false;
	data.remote = false;

	walk_groups(&tmigr_new_timer_up, &data, tmc);

	/* If there is a new first global event, make sure it is handled */
	/* 只有出现新的全局最早事件且无人 active 时，调用 CPU 才需据此重编本地硬件。 */
	return data.firstexp;
}

/*
 * 代表 migrator 拉取一个 idle CPU 已到期的 global timer。先在 tmc->lock 下复核 available/remote/ignore/expiry，
 * 认领 remote 后释放锁执行 timer_expire_remote()，允许目标同时退出 idle；随后严格按 timer_base→tmc 锁序重取
 * 其下一事件并修复整个层次。目标已下线或唤醒则无需 walk。函数无返回，重复/过时请求安全地成为无操作。
 */
static void tmigr_handle_remote_cpu(unsigned int cpu, u64 now,
				    unsigned long jif)
{
	struct timer_events tevt;
	struct tmigr_walk data;
	struct tmigr_cpu *tmc;

	tmc = per_cpu_ptr(&tmigr_cpu, cpu);

	raw_spin_lock_irq(&tmc->lock);

	/*
	 * If the remote CPU is offline then the timers have been migrated to
	 * another CPU.
	 *
	 * If tmigr_cpu::remote is set, at the moment another CPU already
	 * expires the timers of the remote CPU.
	 *
	 * If tmigr_event::ignore is set, then the CPU returns from idle and
	 * takes care of its timers.
	 *
	 * If the next event expires in the future, then the event has been
	 * updated and there are no timers to expire right now. The CPU which
	 * updated the event takes care when hierarchy is completely
	 * idle. Otherwise the migrator does it as the event is enqueued.
	 */
	/* 下线、已有代理、目标已唤醒或事件被推迟时均放弃；对应 timer 已有明确的新 owner/未来 migrator。 */
	if (!tmc->available || tmc->remote || tmc->cpuevt.ignore ||
	    now < tmc->cpuevt.nextevt.expires) {
		raw_spin_unlock_irq(&tmc->lock);
		return;
	}

	trace_tmigr_handle_remote_cpu(tmc);

	tmc->remote = true;
	WRITE_ONCE(tmc->wakeup, KTIME_MAX);

	/* Drop the lock to allow the remote CPU to exit idle */
	/* 拉取 timer 回调前释放 migration 锁，目标 CPU 可并发退出 idle，remote 标志阻止第二个代理。 */
	raw_spin_unlock_irq(&tmc->lock);

	/*
	 * This can't exclude the local CPU because jiffies might have advanced
	 * after the timer softirq invoked run_timer_base(BASE_GLOBAL) and the
	 * point where the jiffies snapshot @jif was taken in tmigr_handle_remote().
	 */
	/* jiffies 快照可能晚于本地 global base softirq，不能据此排除 migrator 自己也仍有需要补处理的 timer。 */
	timer_expire_remote(cpu);

	/*
	 * Lock ordering needs to be preserved - timer_base locks before tmigr
	 * related locks (see section "Locking rules" in the documentation at
	 * the top). During fetching the next timer interrupt, also tmc->lock
	 * needs to be held. Otherwise there is a possible race window against
	 * the CPU itself when it comes out of idle, updates the first timer in
	 * the hierarchy and goes back to idle.
	 *
	 * timer base locks are dropped as fast as possible: After checking
	 * whether the remote CPU went offline in the meantime and after
	 * fetching the next remote timer interrupt. Dropping the locks as fast
	 * as possible keeps the locking region small and prevents holding
	 * several (unnecessary) locks during walking the hierarchy for updating
	 * the timerqueue and group events.
	 */
	/*
	 * 为遵守 timer_base→migration 锁序，先锁远端 bases 再取 tmc；只在读取下一 timer 前同时持有，随后尽快释放
	 * base 锁，避免带着多个 base 锁执行可能跨多层的 group walk。
	 */
	local_irq_disable();
	timer_lock_remote_bases(cpu);
	raw_spin_lock(&tmc->lock);

	/*
	 * When the CPU went offline in the meantime, no hierarchy walk has to
	 * be done for updating the queued events, because the walk was
	 * already done during marking the CPU offline in the hierarchy.
	 *
	 * When the CPU is no longer idle, the CPU takes care of the timers and
	 * also of the timers in the hierarchy.
	 *
	 * (See also section "Required event and timerqueue update after a
	 * remote expiry" in the documentation at the top)
	 */
	/* 下线已在 hotplug 路径完成层次摘除；唤醒则由目标 CPU 自己更新首事件并接管 hierarchy。 */
	if (!tmc->available || !tmc->idle) {
		timer_unlock_remote_bases(cpu);
		goto unlock;
	}

	/* next	event of CPU */
	/* 在 timer bases 与 tmc 状态同时稳定时取得目标 CPU 新的 global 首事件。 */
	fetch_next_timer_interrupt_remote(jif, now, &tevt, cpu);
	timer_unlock_remote_bases(cpu);

	data.nextexp = tevt.global;
	data.firstexp = KTIME_MAX;
	data.evt = &tmc->cpuevt;
	data.remote = true;

	/*
	 * The update is done even when there is no 'new' global timer pending
	 * on the remote CPU (see section "Required event and timerqueue update
	 * after a remote expiry" in the documentation at the top)
	 */
	/* 即使新首事件为 KTIME_MAX 也必须 walk，以删除旧节点并向上暴露 inactive sibling 的剩余事件。 */
	walk_groups(&tmigr_new_timer_up, &data, tmc);

unlock:
	tmc->remote = false;
	raw_spin_unlock_irq(&tmc->lock);
}

/*
 * 作为层次回调处理当前 group 的全部到期事件。只有 child 是 migrator 或组无 migrator 才有代理权；循环在锁下
 * 摘一个事件、锁外拉取目标 CPU，直到无到期项。把当前有效队首写入 firstexp 并返回 false 继续检查父层。
 */
static bool tmigr_handle_remote_up(struct tmigr_group *group,
				   struct tmigr_group *child,
				   struct tmigr_walk *data)
{
	struct tmigr_event *evt;
	unsigned long jif;
	u8 childmask;
	u64 now;

	jif = data->basej;
	now = data->now;

	childmask = data->childmask;

	trace_tmigr_handle_remote(group);
again:
	/*
	 * Handle the group only if @childmask is the migrator or if the
	 * group has no migrator. Otherwise the group is active and is
	 * handled by its own migrator.
	 */
	/* 有别的 active migrator 时停止本分支，避免两 CPU 对同组重复拉取。 */
	if (!tmigr_check_migrator(group, childmask))
		return true;

	raw_spin_lock_irq(&group->lock);

	evt = tmigr_next_expired_groupevt(group, now);

	if (evt) {
		unsigned int remote_cpu = evt->cpu;

		raw_spin_unlock_irq(&group->lock);

		tmigr_handle_remote_cpu(remote_cpu, now, jif);

		/* check if there is another event, that needs to be handled */
		/* 单次只在锁下摘一个，处理完目标后重取队首以吸收并发更新。 */
		goto again;
	}

	/*
	 * Keep track of the expiry of the first event that needs to be handled
	 * (group->next_expiry was updated by tmigr_next_expired_groupevt(),
	 * next was set by tmigr_handle_remote_cpu()).
	 */
	/* 保存本组下一事件；向上 walk 可能以更高层更早事件覆盖，最终成为本 CPU 的 wakeup 承诺。 */
	data->firstexp = group->next_expiry;

	raw_spin_unlock_irq(&group->lock);

	return false;
}

/**
 * tmigr_handle_remote() - Handle global timers of remote idle CPUs
 *
 * Called from the timer soft interrupt with interrupts enabled.
 */
/*
 * timer softirq 中由当前 CPU 作为 migrator 拉取远端 idle CPU 的到期 global timers。不可用直接返回；先用 leaf
 * migrator/wakeup 快检减少空 walk，再固定 now/jiffies 快照逐层处理，最后在 tmc 锁下发布下一 remote wakeup。
 * 入口时 IRQ 开启，内部回调按需 irq-safe 加锁；无错误返回。
 */
void tmigr_handle_remote(void)
{
	struct tmigr_cpu *tmc = this_cpu_ptr(&tmigr_cpu);
	struct tmigr_walk data;

	if (tmigr_is_not_available(tmc))
		return;

	data.childmask = tmc->groupmask;
	data.firstexp = KTIME_MAX;

	/*
	 * NOTE: This is a doubled check because the migrator test will be done
	 * in tmigr_handle_remote_up() anyway. Keep this check to speed up the
	 * return when nothing has to be done.
	 */
	/* 回调仍会复核 migrator；这里仅让无职责且无既定 wakeup 的常见路径尽早返回。 */
	if (!tmigr_check_migrator(tmc->tmgroup, tmc->groupmask)) {
		/*
		 * If this CPU was an idle migrator, make sure to clear its wakeup
		 * value so it won't chase timers that have already expired elsewhere.
		 * This avoids endless requeue from tmigr_new_timer().
		 */
		/* idle migrator 若 wakeup 已被清为 KTIME_MAX，说明旧事件已由别处处理，不再追逐并反复入队。 */
		if (READ_ONCE(tmc->wakeup) == KTIME_MAX)
			return;
	}

	data.now = get_jiffies_update(&data.basej);

	/*
	 * Update @tmc->wakeup only at the end and do not reset @tmc->wakeup to
	 * KTIME_MAX. Even if tmc->lock is not held during the whole remote
	 * handling, tmc->wakeup is fine to be stale as it is called in
	 * interrupt context and tick_nohz_next_event() is executed in interrupt
	 * exit path only after processing the last pending interrupt.
	 */
	/*
	 * walk 期间 wakeup 可短暂陈旧；它在中断上下文使用，IRQ exit 会等当前 pending interrupts 处理完才重新计算
	 * 下一事件，因此只在 walk 结束一次性写入 firstexp，无需开头先发布 KTIME_MAX。
	 */

	__walk_groups(&tmigr_handle_remote_up, &data, tmc);

	raw_spin_lock_irq(&tmc->lock);
	WRITE_ONCE(tmc->wakeup, data.firstexp);
	raw_spin_unlock_irq(&tmc->lock);
}

/*
 * 无副作用地向上寻找是否已有应处理的 remote event。只沿当前 child 有 migrator 权限的组扫描；64 位可原子读
 * next_expiry，32 位在 group 锁下避免撕裂。发现 now >= expiry 时置 data->check 并停止，否则继续到父层。
 */
static bool tmigr_requires_handle_remote_up(struct tmigr_group *group,
					    struct tmigr_group *child,
					    struct tmigr_walk *data)
{
	u8 childmask;

	childmask = data->childmask;

	/*
	 * Handle the group only if the child is the migrator or if the group
	 * has no migrator. Otherwise the group is active and is handled by its
	 * own migrator.
	 */
	/* 仅当当前 child 是 migrator 或组无人 active 时才代理；否则交给该组自己的 migrator。 */
	if (!tmigr_check_migrator(group, childmask))
		return true;
	/*
	 * The lock is required on 32bit architectures to read the variable
	 * consistently with a concurrent writer. On 64bit the lock is not
	 * required because the read operation is not split and so it is always
	 * consistent.
	 */
	/* 32 位读取 u64 可能撕裂，需 group 锁；64 位 READ_ONCE 已能取得一致标量快照。 */
	if (IS_ENABLED(CONFIG_64BIT)) {
		data->firstexp = READ_ONCE(group->next_expiry);
		if (data->now >= data->firstexp) {
			data->check = true;
			return true;
		}
	} else {
		raw_spin_lock(&group->lock);
		data->firstexp = group->next_expiry;
		if (data->now >= group->next_expiry) {
			data->check = true;
			raw_spin_unlock(&group->lock);
			return true;
		}
		raw_spin_unlock(&group->lock);
	}

	return false;
}

/**
 * tmigr_requires_handle_remote() - Check the need of remote timer handling
 *
 * Must be called with interrupts disabled.
 */
/*
 * 预测当前 IRQ exit 是否需要运行 remote timer handler。要求本地 IRQ 关闭；active CPU 按 migrator 权限扫描各层
 * next_expiry，idle CPU 比较已承诺的 tmc->wakeup。64 位无锁读 u64，32 位持相应锁。不可用返回 false。
 */
bool tmigr_requires_handle_remote(void)
{
	struct tmigr_cpu *tmc = this_cpu_ptr(&tmigr_cpu);
	struct tmigr_walk data;
	unsigned long jif;
	bool ret = false;

	if (tmigr_is_not_available(tmc))
		return ret;

	data.now = get_jiffies_update(&jif);
	data.childmask = tmc->groupmask;
	data.firstexp = KTIME_MAX;
	data.check = false;

	/*
	 * If the CPU is active, walk the hierarchy to check whether a remote
	 * expiry is required.
	 *
	 * Check is done lockless as interrupts are disabled and @tmc->idle is
	 * set only by the local CPU.
	 */
	/* IRQ-off 稳定本地 idle 标志；active CPU 逐层判断是否有已到期 remote 事件。 */
	if (!tmc->idle) {
		__walk_groups(&tmigr_requires_handle_remote_up, &data, tmc);

		return data.check;
	}

	/*
	 * When the CPU is idle, compare @tmc->wakeup with @data.now. The lock
	 * is required on 32bit architectures to read the variable consistently
	 * with a concurrent writer. On 64bit the lock is not required because
	 * the read operation is not split and so it is always consistent.
	 */
	/* idle CPU 已保存代理 wakeup，直接与 now 比较；32 位仍需 tmc 锁避免 u64 撕裂。 */
	if (IS_ENABLED(CONFIG_64BIT)) {
		if (data.now >= READ_ONCE(tmc->wakeup))
			return true;
	} else {
		raw_spin_lock(&tmc->lock);
		if (data.now >= tmc->wakeup)
			ret = true;
		raw_spin_unlock(&tmc->lock);
	}

	return ret;
}

/**
 * tmigr_cpu_new_timer() - enqueue next global timer into hierarchy (idle tmc)
 * @nextexp:	Next expiry of global timer (or KTIME_MAX if not)
 *
 * The CPU is already deactivated in the timer migration
 * hierarchy. tick_nohz_get_sleep_length() calls tick_nohz_next_event()
 * and thereby the timer idle path is executed once more. @tmc->wakeup
 * holds the first timer, when the timer migration hierarchy is
 * completely idle.
 *
 * Returns the first timer that needs to be handled by this CPU or KTIME_MAX if
 * nothing needs to be done.
 */
/*
 * 已 deactivate 的当前 CPU 在 idle 重算中更新 global 首 timer。持 tmc 锁；仅当有限 nextexp 与已登记值/ignore
 * 不一致时重新入层次，并把完全 idle 时需兜底的 firstexp 发布到 wakeup。无可用 hierarchy 时原样返回 nextexp。
 */
u64 tmigr_cpu_new_timer(u64 nextexp)
{
	struct tmigr_cpu *tmc = this_cpu_ptr(&tmigr_cpu);
	u64 ret;

	if (tmigr_is_not_available(tmc))
		return nextexp;

	raw_spin_lock(&tmc->lock);

	ret = READ_ONCE(tmc->wakeup);
	if (nextexp != KTIME_MAX) {
		if (nextexp != tmc->cpuevt.nextevt.expires ||
		    tmc->cpuevt.ignore) {
			ret = tmigr_new_timer(tmc, nextexp);
			/*
			 * Make sure the reevaluation of timers in idle path
			 * will not miss an event.
			 */
			/* idle 路径再次评估前先发布 wakeup，避免窗口内漏掉新事件。 */
			WRITE_ONCE(tmc->wakeup, ret);
		}
	}
	trace_tmigr_cpu_new_timer_idle(tmc, nextexp);
	raw_spin_unlock(&tmc->lock);
	return ret;
}

/*
 * 把一个 child 的 inactive 状态原子传播到 group。以 acquire 观察 child/group 顺序，清 active bit；若离开的
 * child 是 migrator，则转交给最低 active bit，或设 TMIGR_NONE 并继续向上。每次递增 seq，cmpxchg 失败后补
 * barrier 再重读；随后无条件更新事件队列。返回 true 表示 migrator 仍在本组、无需继续传播。
 */
static bool tmigr_inactive_up(struct tmigr_group *group,
			      struct tmigr_group *child,
			      struct tmigr_walk *data)
{
	union tmigr_state curstate, newstate, childstate;
	bool walk_done;
	u8 childmask;

	childmask = data->childmask;
	childstate.state = 0;

	/*
	 * The memory barrier is paired with the cmpxchg() in tmigr_active_up()
	 * to make sure the updates of child and group states are ordered. The
	 * ordering is mandatory, as the group state change depends on the child
	 * state.
	 */
	/* inactive 依赖 child 状态，acquire 与 activate 的 cmpxchg 配对，防旧状态跨层倒灌。 */
	curstate.state = atomic_read_acquire(&group->migr_state);

	for (;;) {
		if (child)
			childstate.state = atomic_read(&child->migr_state);

		newstate = curstate;
		walk_done = true;

		/* Reset active bit when the child is no longer active */
		/* child 可能已并发重新 active，只有快照仍 inactive 时才清位。 */
		if (!childstate.active)
			newstate.active &= ~childmask;

		if (newstate.migrator == childmask) {
			/*
			 * Find a new migrator for the group, because the child
			 * group is idle!
			 */
			/* 离开的 child 确已 idle 时，才需要从剩余 active mask 中选择新 migrator。 */
			if (!childstate.active) {
				unsigned long new_migr_bit, active = newstate.active;

				new_migr_bit = find_first_bit(&active, BIT_CNT);

				if (new_migr_bit != BIT_CNT) {
					newstate.migrator = BIT(new_migr_bit);
				} else {
					newstate.migrator = TMIGR_NONE;

					/* Changes need to be propagated */
					/* 本组无人 active，父层也必须清除本组的 active/migrator 资格。 */
					walk_done = false;
				}
			}
		}

		newstate.seq++;

		WARN_ON_ONCE((newstate.migrator != TMIGR_NONE) && !(newstate.active));

		if (atomic_try_cmpxchg(&group->migr_state, &curstate.state, newstate.state)) {
			trace_tmigr_group_set_cpu_inactive(group, newstate, childmask);
			break;
		}

		/*
		 * The memory barrier is paired with the cmpxchg() in
		 * tmigr_active_up() to make sure the updates of child and group
		 * states are ordered. It is required only when the above
		 * try_cmpxchg() fails.
		 */
		/* cmpxchg 失败说明状态已变；补排序后用回填的新 curstate 重新计算。 */
		smp_mb__after_atomic();
	}

	data->remote = false;

	/* Event Handling */
	/* 状态提交后更新 child 事件；是否继续向上由本回调的 migrator 结果决定。 */
	tmigr_update_events(group, child, data);

	return walk_done;
}

/*
 * 在 tmc->lock 下把当前 CPU 从 leaf 到根标为 inactive，并登记其 nextexp。KTIME_MAX 表示本地 pinned timer 更早、
 * 无 global timer 或即将 offline，因此 leaf event 保持 ignore。返回全层次 idle 时当前 CPU 必须兜底的最早 expiry。
 */
static u64 __tmigr_cpu_deactivate(struct tmigr_cpu *tmc, u64 nextexp)
{
	struct tmigr_walk data = { .nextexp = nextexp,
				   .firstexp = KTIME_MAX,
				   .evt = &tmc->cpuevt,
				   .childmask = tmc->groupmask };

	/*
	 * If nextexp is KTIME_MAX, the CPU event will be ignored because the
	 * local timer expires before the global timer, no global timer is set
	 * or CPU goes offline.
	 */
	/* 只有有限 global deadline 才让 leaf event 参与父队列；KTIME_MAX 保持 ignore。 */
	if (nextexp != KTIME_MAX)
		tmc->cpuevt.ignore = false;

	walk_groups(&tmigr_inactive_up, &data, tmc);
	return data.firstexp;
}

/**
 * tmigr_cpu_deactivate() - Put current CPU into inactive state
 * @nextexp:	The next global timer expiry of the current CPU
 *
 * Must be called with interrupts disabled.
 *
 * Return: the next event expiry of the current CPU or the next event expiry
 * from the hierarchy if this CPU is the top level migrator or the hierarchy is
 * completely idle.
 */
/*
 * 当前 CPU 进入 timer idle；要求 IRQ 关闭。不可用时返回自身 nextexp。持 tmc 锁先完成 inactive/migrator/事件
 * 传播，再置 idle=true 并发布 wakeup，返回本地 deadline 与 hierarchy 兜底 deadline 的协议结果。
 */
u64 tmigr_cpu_deactivate(u64 nextexp)
{
	struct tmigr_cpu *tmc = this_cpu_ptr(&tmigr_cpu);
	u64 ret;

	if (tmigr_is_not_available(tmc))
		return nextexp;

	raw_spin_lock(&tmc->lock);

	ret = __tmigr_cpu_deactivate(tmc, nextexp);

	tmc->idle = true;

	/*
	 * Make sure the reevaluation of timers in idle path will not miss an
	 * event.
	 */
	/* 在解锁前发布根兜底 wakeup，供后续 idle 重算和硬件编程使用。 */
	WRITE_ONCE(tmc->wakeup, ret);

	trace_tmigr_cpu_idle(tmc, nextexp);
	raw_spin_unlock(&tmc->lock);
	return ret;
}

/**
 * tmigr_quick_check() - Quick forecast of next tmigr event when CPU wants to
 *			 go idle
 * @nextevt:	The next global timer expiry of the current CPU
 *
 * Return:
 * * KTIME_MAX		- when it is probable that nothing has to be done (not
 *			  the only one in the level 0 group; and if it is the
 *			  only one in level 0 group, but there are more than a
 *			  single group active on the way to top level)
 * * nextevt		- when CPU is offline and has to handle timer on its own
 *			  or when on the way to top in every group only a single
 *			  child is active but @nextevt is before the lowest
 *			  next_expiry encountered while walking up to top level.
 * * next_expiry	- value of lowest expiry encountered while walking groups
 *			  if only a single child is active on each and @nextevt
 *			  is after this lowest expiry.
 */
/*
 * CPU 真正 deactivate 前的无锁保守预测：只有本 CPU 可能是每层唯一 migrator 时才一路取最小 next_expiry；任一
 * 层有多个 active child 则返回 KTIME_MAX，表示预计其他 migrator 会处理。结果只用于预估，最终由完整锁路径确认。
 */
u64 tmigr_quick_check(u64 nextevt)
{
	struct tmigr_cpu *tmc = this_cpu_ptr(&tmigr_cpu);
	struct tmigr_group *group = tmc->tmgroup;

	if (tmigr_is_not_available(tmc))
		return nextevt;

	if (WARN_ON_ONCE(tmc->idle))
		return nextevt;

	if (!tmigr_check_migrator_and_lonely(tmc->tmgroup, tmc->groupmask))
		return KTIME_MAX;

	do {
		if (!tmigr_check_lonely(group))
			return KTIME_MAX;

		/*
		 * Since current CPU is active, events may not be sorted
		 * from bottom to the top because the CPU's event is ignored
		 * up to the top and its sibling's events not propagated upwards.
		 * Thus keep track of the lowest observed expiry.
		 */
		/* active CPU 的 leaf 事件被忽略且 sibling 可能未上传，各层不保证单调，故累计最小值。 */
		nextevt = min_t(u64, nextevt, READ_ONCE(group->next_expiry));
		group = group->parent;
	} while (group);

	return nextevt;
}

/*
 * tmigr_trigger_active() - trigger a CPU to become active again
 *
 * This function is executed on a CPU which is part of cpu_online_mask, when the
 * last active CPU in the hierarchy is offlining. With this, it is ensured that
 * the other CPU is active and takes over the migrator duty.
 */
/*
 * 在最后 active CPU 下线时，通过 work_on_cpu() 同步调度到仍 online 的接班 CPU；函数本身不改状态，只利用
 * 调度/执行边界确认目标确实 available 且非 idle，使其已有 active 状态承担 migrator 职责。异常仅 WARN。
 */
static long tmigr_trigger_active(void *unused)
{
	struct tmigr_cpu *tmc = this_cpu_ptr(&tmigr_cpu);

	WARN_ON_ONCE(!tmc->available || tmc->idle);

	return 0;
}

/*
 * 返回 CPU 所属 hierarchy 的 capacity 分类。nohz_full 时强制统一为 SCHED_CAPACITY_SCALE，确保 timekeeper 与
 * 所有 nohz_full CPU 同层次且始终有人代管；当前非 BROKEN 构建也统一容量，实验配置才读架构 capacity。
 */
static unsigned int tmigr_get_capacity(int cpu)
{
	/*
	 * nohz_full CPUs need to make sure there is always an available (online)
	 * and never idle migrator to handle all their global timers. That duty
	 * is served by the timekeeper which then never stops its tick. But the
	 * timekeeper must then belong to the same hierarchy as all the nohz_full
	 * CPUs. Simply turn off capacity awareness when nohz_full is running.
	 */
	/* nohz_full 需要永不 idle 的 timekeeper 与其共享 hierarchy，因此禁用异构 capacity 分流。 */
	if (tick_nohz_full_enabled() || !IS_ENABLED(CONFIG_BROKEN))
		return SCHED_CAPACITY_SCALE;
	else
		return arch_scale_cpu_capacity(cpu);
}

/* 在 tmigr_mutex 保护的常驻链表中按 CPU capacity 查找 hierarchy；仅借用返回，未找到为 NULL。 */
static struct tmigr_hierarchy *__tmigr_get_hierarchy(int cpu)
{
	unsigned int capacity = tmigr_get_capacity(cpu);
	struct tmigr_hierarchy *iter;

	list_for_each_entry(iter, &tmigr_hierarchy_list, node) {
		if (iter->capacity == capacity)
			return iter;
	}

	return NULL;
}

/*
 * 把本地 CPU 从 available mask 和 hierarchy 运行状态中摘除。由本 CPU 的 hotplug/isolation work 调用；
 * available_mutex 串行 mask/字段，tmc 锁下以 KTIME_MAX deactivate。若它是最后 migrator 且返回待代理事件，
 * 则选择同 hierarchy 的可用 CPU 同步触发接班；找不到 hierarchy 返回 -EINVAL，其余成功/幂等返回 0。
 */
static int tmigr_clear_cpu_available(unsigned int cpu)
{
	struct tmigr_cpu *tmc = this_cpu_ptr(&tmigr_cpu);
	int migrator;
	u64 firstexp;

	guard(mutex)(&tmigr_available_mutex);

	cpumask_clear_cpu(cpu, tmigr_available_cpumask);
	scoped_guard(raw_spinlock_irq, &tmc->lock) {
		if (!tmc->available)
			return 0;
		tmc->available = false;
		WRITE_ONCE(tmc->wakeup, KTIME_MAX);

		/*
		 * CPU has to handle the local events on his own, when on the way to
		 * offline; Therefore nextevt value is set to KTIME_MAX
		 */
		/* 下线途中 CPU 自己处理本地事件，向 migration hierarchy 登记 KTIME_MAX 以撤销代理事件。 */
		firstexp = __tmigr_cpu_deactivate(tmc, KTIME_MAX);
		trace_tmigr_cpu_unavailable(tmc);
	}

	if (firstexp != KTIME_MAX) {
		struct tmigr_hierarchy *hier = __tmigr_get_hierarchy(cpu);

		if (WARN_ON_ONCE(!hier))
			return -EINVAL;

		migrator = cpumask_any_and(tmigr_available_cpumask, hier->cpumask);
		if (migrator < nr_cpu_ids) {
			work_on_cpu(migrator, tmigr_trigger_active, NULL);
		} else {
			/*
			 * If deactivation returned an expiration, it belongs to an available
			 * nohz CPU in the hierarchy.
			 */
			/* 有限 expiry 说明 hierarchy 内仍有 nohz CPU 的事件，需要一个 available CPU 接班。 */
			WARN_ONCE(1, "Expected available CPU in the hierarchy\n");
		}
	}

	return 0;
}

/*
 * 把本地已完成 prepare 的 CPU 加入 available 集合。初始化缺失返回 -EINVAL；在 available_mutex+tmc 锁下读取
 * timer base idle 状态，若当前 active 则先向上激活，再发布 available=true。重复调用幂等返回 0。
 */
static int __tmigr_set_cpu_available(unsigned int cpu)
{
	struct tmigr_cpu *tmc = this_cpu_ptr(&tmigr_cpu);

	/* Check whether CPU data was successfully initialized */
	/* prepare 尚未建立 leaf group 时不能发布 available。 */
	if (WARN_ON_ONCE(!tmc->tmgroup))
		return -EINVAL;

	guard(mutex)(&tmigr_available_mutex);

	cpumask_set_cpu(cpu, tmigr_available_cpumask);
	scoped_guard(raw_spinlock_irq, &tmc->lock) {
		if (tmc->available)
			return 0;
		trace_tmigr_cpu_available(tmc);
		tmc->idle = timer_base_is_idle();
		if (!tmc->idle)
			__tmigr_cpu_activate(tmc);
		tmc->available = true;
	}
	return 0;
}

/* CPU online 包装：DOMAIN 隔离 CPU 保持 excluded 并成功返回，其余走实际 available 提交。 */
static int tmigr_set_cpu_available(unsigned int cpu)
{
	if (tmigr_is_isolated(cpu))
		return 0;

	return __tmigr_set_cpu_available(cpu);
}

/* 在目标 CPU 本地执行隔离摘除；work 参数无状态，错误只能由内部 WARN/trace 观察。 */
static void tmigr_cpu_isolate(struct work_struct *ignored)
{
	tmigr_clear_cpu_available(smp_processor_id());
}

/*
 * 在目标 CPU 本地恢复 available；调用者已经正确持 cpuset 锁，但 lockdep 无法沿 workqueue 追踪，所以直接调用
 * 内部版本，避免再次经过 housekeeping_cpu() 的锁依赖检查。
 */
static void tmigr_cpu_unisolate(struct work_struct *ignored)
{
	/*
	 * Don't call tmigr_is_isolated() ->housekeeping_cpu() directly because
	 * the cpuset mutex is correctly held by the workqueue caller but lockdep
	 * doesn't know that.
	 */
	/* cpuset mutex 由排队方正确持有，但 lockdep 不跨 work 识别，故绕过再次调用 housekeeping_cpu()。 */
	__tmigr_set_cpu_available(smp_processor_id());
}

/**
 * tmigr_isolated_exclude_cpumask - Exclude given CPUs from hierarchy
 * @exclude_cpumask: the cpumask to be excluded from timer migration hierarchy
 *
 * This function can be called from cpuset code to provide the new set of
 * isolated CPUs that should be excluded from the hierarchy.
 * Online CPUs not present in exclude_cpumask but already excluded are brought
 * back to the hierarchy.
 * Functions to isolate/unisolate need to be called locally and can sleep.
 */
/*
 * tmigr_isolated_exclude_cpumask() - 让 timer migration 层次跟随隔离 CPU 集合。
 *
 * @exclude_cpumask 是 cpuset/启动初始化构造的借用输入掩码，表示应排除出 timer
 * migration hierarchy 的 CPU；函数不修改、不保存也不释放它。
 * 调用者位于可睡眠进程上下文。函数分配每 CPU work 和临时 cpumask，持有
 * cpus_read_lock 稳定 online 集合，并把 isolate/unisolate 操作投递到目标 CPU
 * 本地执行后逐项 flush，因此返回前所有成功调度的状态转换均已完成。
 *
 * 返回 0 表示层次已与新集合收敛；临时对象分配失败返回 -ENOMEM，且失败发生在
 * 任何 CPU 状态变化前。__free cleanup 属性保证所有 return 都自动释放 works
 * 和 cpumask；它只结构化 ownership，不改变 work 必须先 flush 的同步要求。
 */
int tmigr_isolated_exclude_cpumask(struct cpumask *exclude_cpumask)
{
	/*
	 * 变量地图：
	 *   works   每个 CPU 一个同步 work，离开作用域时 free_percpu；
	 *   cpumask 当前阶段需要发生状态转换的在线 CPU 临时集合；
	 *   cpu     遍历目标 CPU 编号。
	 */
	struct work_struct __percpu *works __free(free_percpu) =
		alloc_percpu(struct work_struct);
	cpumask_var_t cpumask __free(free_cpumask_var) = CPUMASK_VAR_NULL;
	int cpu;

	/* 两次分配都在修改全局 timer migration 状态前完成，失败可干净返回。 */
	if (!works)
		return -ENOMEM;
	if (!alloc_cpumask_var(&cpumask, GFP_KERNEL))
		return -ENOMEM;

	/*
	 * First set previously isolated CPUs as available (unisolate).
	 * This cpumask contains only CPUs that switched to available now.
	 */
	/*
	 * 第一阶段先把“旧策略排除、但新策略不再排除”的在线 CPU 恢复为 available。
	 * 临时 cpumask 只包含本次刚转为可用的 CPU。先 unisolate 再 isolate 可使层次
	 * 在过渡中拥有更多而非更少迁移目标，降低暂时无可用 migrator 的风险。
	 */
	/*
	 * scope guard 在函数退出时自动 cpus_read_unlock()；该读锁稳定 cpu_online_mask
	 * 并阻止目标 CPU 在本地 work 执行/flush 期间被并发下线。
	 */
	guard(cpus_read_lock)();
	cpumask_andnot(cpumask, cpu_online_mask, exclude_cpumask);
	cpumask_andnot(cpumask, cpumask, tmigr_available_cpumask);

	/* 每个状态操作必须在对应 CPU 本地执行，先全部排队再统一等待以允许并行推进。 */
	for_each_cpu(cpu, cpumask) {
		struct work_struct *work = per_cpu_ptr(works, cpu);

		INIT_WORK(work, tmigr_cpu_unisolate);
		schedule_work_on(cpu, work);
	}
	for_each_cpu(cpu, cpumask)
		flush_work(per_cpu_ptr(works, cpu));

	/*
	 * Then clear previously available CPUs (isolate).
	 * This cpumask contains only CPUs that switched to not available now.
	 * There cannot be overlap with the newly available ones.
	 */
	/*
	 * 第二阶段再把“新策略要求排除、当前仍 available”的 CPU 隔离。该集合与
	 * 上一阶段刚恢复的集合按定义不重叠；再与 KERNEL_NOISE housekeeper 相交，
	 * 只处理 timer migration 逻辑实际认为可承接内核噪声的 CPU。
	 */
	cpumask_and(cpumask, exclude_cpumask, tmigr_available_cpumask);
	cpumask_and(cpumask, cpumask, housekeeping_cpumask(HK_TYPE_KERNEL_NOISE));
	/*
	 * Handle this here and not in the cpuset code because exclude_cpumask
	 * might include also the tick CPU if included in isolcpus.
	 */
	/*
	 * 这项处理放在 timer migration 内而非 cpuset：exclude_cpumask 也可能包含
	 * isolcpus 指定的 tick_do_timer_cpu。该 CPU 代表 full-dynticks CPU 承担
	 * timekeeping 等 housekeeping，在 nohz_full 启用时不可下线，也不能从迁移
	 * 层次排除。当前实现找到它后从本批次清除并停止搜索。
	 */
	for_each_cpu(cpu, cpumask) {
		if (!tick_nohz_cpu_hotpluggable(cpu)) {
			cpumask_clear_cpu(cpu, cpumask);
			break;
		}
	}

	/* 与恢复阶段相同，本地执行 isolate，并在返回前等待每个 work 完成。 */
	for_each_cpu(cpu, cpumask) {
		struct work_struct *work = per_cpu_ptr(works, cpu);

		INIT_WORK(work, tmigr_cpu_isolate);
		schedule_work_on(cpu, work);
	}
	for_each_cpu(cpu, cpumask)
		flush_work(per_cpu_ptr(works, cpu));

	return 0;
}

/*
 * tmigr_init_isolation() - 启动后把 boot-time DOMAIN 隔离同步到 timer hierarchy。
 *
 * late_initcall 调用，无参数。先启用 timer migration 自身的 isolated static key；
 * 若 HK_TYPE_DOMAIN 未配置则无需构造排除集合。配置存在时用
 * possible_mask - DOMAIN 得到隔离 CPU，并复用运行期更新函数。
 * 返回 0、-ENOMEM 或下层错误；临时 cpumask 由 __free 自动释放。
 */
static int __init tmigr_init_isolation(void)
{
	cpumask_var_t cpumask __free(free_cpumask_var) = CPUMASK_VAR_NULL;

	static_branch_enable(&tmigr_exclude_isolated);

	if (!housekeeping_enabled(HK_TYPE_DOMAIN))
		return 0;
	if (!alloc_cpumask_var(&cpumask, GFP_KERNEL))
		return -ENOMEM;

	cpumask_andnot(cpumask, cpu_possible_mask, housekeeping_cpumask(HK_TYPE_DOMAIN));

	/* Protect against RCU torture hotplug testing */
	/*
	 * 即使是启动初始化也走带 cpus_read_lock 和本地 work flush 的公共路径，以防
	 * RCU torture 等测试在 late_initcall 阶段并发触发 CPU hotplug。
	 */
	return tmigr_isolated_exclude_cpumask(cpumask);
}
late_initcall(tmigr_init_isolation);

/*
 * 初始化新 group 的锁、层级/NUMA 归属、空 migr_state、timerqueue 与内嵌 groupevt。所有 expiry 置 KTIME_MAX、
 * event 初始 ignore；对象尚未发布给 child，调用者持 tmigr_mutex，函数不分配资源也不失败。
 */
static void tmigr_init_group(struct tmigr_group *group, unsigned int lvl,
			     int node)
{
	union tmigr_state s;

	raw_spin_lock_init(&group->lock);

	group->level = lvl;
	group->numa_node = lvl < tmigr_crossnode_level ? node : NUMA_NO_NODE;

	group->num_children = 0;

	s.migrator = TMIGR_NONE;
	s.active = 0;
	s.seq = 0;
	atomic_set(&group->migr_state, s.state);

	timerqueue_init_head(&group->events);
	timerqueue_init(&group->groupevt.nextevt);
	group->groupevt.nextevt.expires = KTIME_MAX;
	WRITE_ONCE(group->next_expiry, KTIME_MAX);
	group->groupevt.ignore = true;
}

/*
 * 在指定 level/NUMA 范围复用仍有 child 容量的 group，否则按 node 分配并初始化新 group、挂入 level_list。
 * 要求持 tmigr_mutex；成功返回常驻借用指针，内存不足返回 ERR_PTR(-ENOMEM)，失败前不发布半初始化对象。
 */
static struct tmigr_group *tmigr_get_group(struct tmigr_hierarchy *hier, int node, unsigned int lvl)
{
	struct tmigr_group *tmp, *group = NULL;

	lockdep_assert_held(&tmigr_mutex);

	/* Try to attach to an existing group first */
	/* 优先复用同层仍有容量且 NUMA 归属匹配的常驻 group。 */
	list_for_each_entry(tmp, &hier->level_list[lvl], list) {
		/*
		 * If @lvl is below the cross NUMA node level, check whether
		 * this group belongs to the same NUMA node.
		 */
		/* crossnode_level 以下维持 node 局部性，更高层才允许跨 node 聚合。 */
		if (lvl < tmigr_crossnode_level && tmp->numa_node != node)
			continue;

		/* Capacity left? */
		/* groupmask 只有 8 位，达到每组 child 上限后继续寻找。 */
		if (tmp->num_children >= TMIGR_CHILDREN_PER_GROUP)
			continue;

		/*
		 * TODO: A possible further improvement: Make sure that all CPU
		 * siblings end up in the same group of the lowest level of the
		 * hierarchy. Rely on the topology sibling mask would be a
		 * reasonable solution.
		 */
		/* 待优化：最低层尚未强制拓扑 sibling 同组，当前只按遍历顺序和容量装箱。 */

		group = tmp;
		break;
	}

	if (group)
		return group;

	/* Allocate and	set up a new group */
	/* 无可复用对象时在目标 node 分配；失败不改 level_list。 */
	group = kzalloc_node(sizeof(*group), GFP_KERNEL, node);
	if (!group)
		return ERR_PTR(-ENOMEM);

	tmigr_init_group(group, lvl, node);

	/* Setup successful. Add it to the hierarchy */
	/* 完整初始化后才挂入层次链表并可被后续连接看到。 */
	list_add(&group->list, &hier->level_list[lvl]);
	trace_tmigr_group_set(group);
	return group;
}

/*
 * 判断尚无 parent 且不同于旧 root 的 group 是否成为新顶层；若是预设其未来作为 child 0 的 groupmask，并要求
 * 本阶段不是 activation 连接。返回 true 让调用者预记旧 root child，普通情况返回 false。
 */
static bool tmigr_init_root(struct tmigr_hierarchy *hier, struct tmigr_group *group, bool activate)
{
	if (!group->parent && group != hier->root) {
		/*
		 * This is the new top-level, prepare its groupmask in advance
		 * to avoid accidents where yet another new top-level is
		 * created in the future and made visible before this groupmask.
		 */
		/* 新顶层提前固定自己未来作为更高层 child 0 的 mask，防再次扩层先发布 parent。 */
		group->groupmask = BIT(0);
		WARN_ON_ONCE(activate);

		return true;
	}

	return false;

}

/*
 * 在 tmigr_mutex 下把 child 接到 parent：区分旧 root 扩层与普通新增 child，分配唯一 groupmask/更新 child 数；
 * 最后以 release store 发布 parent，与运行期 walk 的 READ_ONCE 配对，保证 racing idle/active 看见完整初始化。
 */
static void tmigr_connect_child_parent(struct tmigr_hierarchy *hier, struct tmigr_group *child,
				       struct tmigr_group *parent, bool activate)
{
	if (tmigr_init_root(hier, parent, activate)) {
		/*
		 * The previous top level had prepared its groupmask already,
		 * simply account it in advance as the first child. If some groups
		 * have been created between the old and new root due to node
		 * mismatch, the new root's child will be intialized accordingly.
		 */
		/* 旧顶层已预留 mask，把它预记为新 root 的第一个 child；node mismatch 中间层同理。 */
		parent->num_children = 1;
	}

	/* Connecting old root to new root ? */
	/* activate 表示正在把已有 active 旧 root 接到更高的新 root。 */
	if (!parent->parent && activate) {
		/*
		 * @child is the old top, or in case of node mismatch, some
		 * intermediate group between the old top and the new one in
		 * @parent. In this case the @child must be pre-accounted above
		 * as the first child. Its new inactive sibling corresponding
		 * to the CPU going up has been accounted as the second child.
		 */
		/* 扩层时旧 root 必须是预记 child 0，新 CPU 分支已占 child 1；计数不符说明建链协议破坏。 */
		WARN_ON_ONCE(parent->num_children != 2);
		child->groupmask = BIT(0);
	} else {
		/* Common case adding @child for the CPU going up to @parent. */
		/* 普通路径按当前 child 数分配唯一 bit，并递增容量计数。 */
		child->groupmask = BIT(parent->num_children++);
	}

	/*
	 * Make sure parent initialization is visible before publishing it to a
	 * racing CPU entering/exiting idle. This RELEASE barrier enforces an
	 * address dependency that pairs with the READ_ONCE() in __walk_groups().
	 */
	/* release 最后发布 parent，运行期 walk 先见指针时必然也见 groupmask、锁、队列和状态初始化。 */
	smp_store_release(&child->parent, parent);

	trace_tmigr_connect_child_parent(hier, child);
}

/*
 * 为 CPU 或已有 start root 自底向上寻找/创建所需 groups，再逆序建立 CPU→leaf 和 child→parent 连接；可按实际
 * CPU/node 数提前停止。失败删除本次尚未连接的 groups 并返回负 errno。activate=true 时还把旧 active root
 * 状态传播进新层，最后原子语义上更新 hierarchy root；调用者持 tmigr_mutex。
 */
static int tmigr_setup_groups(struct tmigr_hierarchy *hier, unsigned int cpu,
			      unsigned int node, struct tmigr_group *start, bool activate)
{
	struct tmigr_group *root = hier->root, *group, *child, **stack;
	int i, top = 0, err = 0, start_lvl = 0;
	bool root_mismatch = false;

	stack = kzalloc_objs(*stack, tmigr_hierarchy_levels);
	if (!stack)
		return -ENOMEM;

	if (start) {
		stack[start->level] = start;
		start_lvl = start->level + 1;
	}

	if (root)
		root_mismatch = root->numa_node != node;

	for (i = start_lvl; i < tmigr_hierarchy_levels; i++) {
		group = tmigr_get_group(hier, node, i);
		if (IS_ERR(group)) {
			err = PTR_ERR(group);
			i--;
			break;
		}

		top = i;
		stack[i] = group;

		/*
		 * When booting only less CPUs of a system than CPUs are
		 * available, not all calculated hierarchy levels are required,
		 * unless a node mismatch is detected.
		 *
		 * The loop is aborted as soon as the highest level, which might
		 * be different from tmigr_hierarchy_levels, contains only a
		 * single group, unless the nodes mismatch below tmigr_crossnode_level
		 */
		/*
		 * possible 拓扑估算可能高于实际 online 需求；当前层已有单 root 且无需继续解决 node mismatch 时可提前停止，
		 * 已存在 parent 也说明此分支接入了现有上层。
		 */
		if (group->parent)
			break;
		if ((!root_mismatch || i >= tmigr_crossnode_level) &&
		    list_is_singular(&hier->level_list[i]))
			break;
	}

	/* Assert single root without parent */
	/* 超出估算层数仍未形成单 root 属拓扑计算错误，拒绝发布连接。 */
	if (WARN_ON_ONCE(i >= tmigr_hierarchy_levels))
		return -EINVAL;

	for (; i >= start_lvl; i--) {
		group = stack[i];

		if (err < 0) {
			list_del(&group->list);
			kfree(group);
			continue;
		}

		WARN_ON_ONCE(i != group->level);

		/*
		 * Update tmc -> group / child -> group connection
		 */
		/* 逆序处理 stack：第 0 层连接 CPU，其他层把较低 child 发布到 parent。 */
		if (i == 0) {
			struct tmigr_cpu *tmc = per_cpu_ptr(&tmigr_cpu, cpu);

			tmc->tmgroup = group;
			tmc->groupmask = BIT(group->num_children++);

			tmigr_init_root(hier, group, activate);

			trace_tmigr_connect_cpu_parent(hier, tmc);

			/* There are no children that need to be connected */
			/* CPU→leaf 已是最低边，不存在更低 group child。 */
			continue;
		} else {
			child = stack[i - 1];
			tmigr_connect_child_parent(hier, child, group, activate);
		}
	}

	if (err < 0)
		goto out;

	if (activate) {
		struct tmigr_walk data;
		union tmigr_state state;

		/*
		 * To prevent inconsistent states, active children need to be active in
		 * the new parent as well. Inactive children are already marked inactive
		 * in the parent group:
		 *
		 * * When new groups were created by tmigr_setup_groups() starting from
		 *   the lowest level, then they are not active. They will be set active
		 *   when the new online CPU comes active.
		 *
		 * * But if new groups above the current top level are required, it is
		 *   mandatory to propagate the active state of the already existing
		 *   child to the new parents. So tmigr_active_up() activates the
		 *   new parents while walking up from the old root to the new.
		 *
		 * * It is ensured that @start is active, (or on the way to be activated
		 *   by another CPU that woke up before the current one) as this setup path
		 *   is executed in hotplug prepare callback. This is executed by an already
		 *   connected and !idle CPU in the hierarchy.
		 *
		 * * The below RmW atomic operation ensures that:
		 *
		 *   1) If the old root has been completely activated, the latest state is
		 *      acquired (the below implicit acquire pairs with the implicit release
		 *      from cmpxchg() in tmigr_active_up()).
		 *
		 *   2) If the old root is still on the way to be activated, the lagging behind
		 *      CPU performing the activation will acquire the links up to the new root.
		 *      (The below implicit release pairs with the implicit acquire from cmpxchg()
		 *      in tmigr_active_up()).
		 *
		 *   3) Every subsequent CPU below the old root will acquire the new links while
		 *      walking through the old root (The below implicit release pairs with the
		 *      implicit acquire from cmpxchg() in either tmigr_active_up()) or
		 *      tmigr_inactive_up().
		 */
		/*
		 * root 扩层不能让已有 active child 在新 parent 中暂时显示 inactive。原子 RMW 取得旧 root 最新状态并与
		 * activate 排序；若已 active 立即向上传播，否则并发激活者会沿 release 发布的新链补齐。
		 */
		state.state = atomic_fetch_or(0, &start->migr_state);
		WARN_ON_ONCE(!start->parent);
		/*
		 * If the state of the old root is inactive, another CPU is on its way to activate
		 * it and propagate to the new root.
		 */
		/* 旧 root 尚 inactive 表示另一个 CPU 正在激活，它会观察新 parent 并自行传播。 */
		if (state.active) {
			data.childmask = start->groupmask;
			__walk_groups_from(tmigr_active_up, &data, start, start->parent);
		}
	} else if (start) {
		union tmigr_state state;

		/* Remote activation assumes the whole target's hierarchy is inactive */
		/* 无可用远端 CPU 时只连 inactive root；active 状态说明调用策略错误。 */
		state.state = atomic_read(&start->migr_state);
		WARN_ON_ONCE(state.active);
	}

	/* Root update */
	/* 最高已用层只有一个无 parent group 时，才把它发布为 hierarchy root。 */
	if (list_is_singular(&hier->level_list[top])) {
		group = list_first_entry(&hier->level_list[top], typeof(*group), list);
		WARN_ON_ONCE(group->parent);
		if (root) {
			/* Old root should be the same or below */
			/* 扩层只能维持或提高 root level，不能倒退。 */
			WARN_ON_ONCE(root->level > top);
		}
		hier->root = group;
	}
out:
	kfree(stack);

	return err;
}

/*
 * 查找 CPU capacity 对应的 hierarchy；不存在则分配含 level_list 的主体和 cpumask，初始化各层链表并挂入全局
 * 常驻列表。要求 tmigr_mutex；成功返回借用指针，任一分配失败完整回滚并返回 ERR_PTR(-ENOMEM)。
 */
static struct tmigr_hierarchy *tmigr_get_hierarchy(int cpu)
{
	struct tmigr_hierarchy *hier;

	hier = __tmigr_get_hierarchy(cpu);

	if (hier)
		return hier;

	hier = kzalloc_flex(*hier, level_list, tmigr_hierarchy_levels);
	if (!hier)
		return ERR_PTR(-ENOMEM);

	hier->cpumask = kzalloc(cpumask_size(), GFP_KERNEL);
	if (!hier->cpumask) {
		kfree(hier);
		return ERR_PTR(-ENOMEM);
	}

	for (int i = 0; i < tmigr_hierarchy_levels; i++)
		INIT_LIST_HEAD(&hier->level_list[i]);

	hier->capacity = tmigr_get_capacity(cpu);
	list_add_tail(&hier->node, &tmigr_hierarchy_list);

	return hier;
}

/*
 * 把 old_root 作为 start 接到新建的更高层。一般必须在非目标 CPU 上执行，activate=true 还要求执行 CPU 已在
 * 同 hierarchy available，以可靠取得并传播旧 root active 状态；返回 setup 的 0/负 errno。
 */
static int tmigr_connect_old_root(struct tmigr_hierarchy *hier, int cpu,
				  struct tmigr_group *old_root,	bool activate)
{
	/*
	 * The target CPU must never do the prepare work, except
	 * on early boot when the boot CPU is the target. Otherwise
	 * it may spuriously activate the old top level group inside
	 * the new one (nevertheless whether old top level group is
	 * active or not) and/or release an uninitialized childmask.
	 */
	/*
	 * 除 boot CPU 早期特例外，不得在正被 prepare 的目标 CPU 上扩层；否则可能把旧 root 无条件伪激活，或在
	 * groupmask 完成前让运行期 walk 看见新链。
	 */
	WARN_ON_ONCE(cpu == smp_processor_id());
	if (activate) {
		/*
		 * The current CPU is expected to be online in the hierarchy,
		 * otherwise the old root may not be active as expected.
		 */
		/* active 扩层必须由该 hierarchy 内已 online/available 的 CPU 执行，才能保证旧 root 的 active 前提。 */
		WARN_ON_ONCE(!__this_cpu_read(tmigr_cpu.available));
	}

	return tmigr_setup_groups(hier, -1, old_root->numa_node, old_root, activate);
}

/* work_on_cpu 适配器：在目标 CPU 上查找其 hierarchy，并以 active 语义连接传入 old_root；缺失返回 -EINVAL。 */
static long connect_old_root_work(void *arg)
{
	struct tmigr_group *old_root = arg;
	struct tmigr_hierarchy *hier;
	int cpu = smp_processor_id();

	hier = __tmigr_get_hierarchy(cpu);
	if (WARN_ON_ONCE(!hier))
		return -EINVAL;

	return tmigr_connect_old_root(hier, cpu, old_root, true);
}

/*
 * CPU 首次 prepare 时在 tmigr_mutex 下取得/创建 hierarchy 和 group 链。若新增 CPU 导致 root 升层，按当前 CPU
 * 是否属于该 hierarchy 选择本地、远端 available CPU 或 inactive 连接，避免错误激活旧 root。成功后才把 CPU
 * 加入 hierarchy cpumask；分配/连接失败返回负 errno。
 */
static int tmigr_add_cpu(unsigned int cpu)
{
	struct tmigr_hierarchy *hier;
	struct tmigr_group *old_root;
	int node = cpu_to_node(cpu);
	int ret;

	guard(mutex)(&tmigr_mutex);

	hier = tmigr_get_hierarchy(cpu);
	if (IS_ERR(hier))
		return PTR_ERR(hier);

	old_root = hier->root;

	ret = tmigr_setup_groups(hier, cpu, node, NULL, false);

	if (ret < 0)
		return ret;

	/* Root has changed? Connect the old one to the new */
	/* 新 CPU 使 root 升层时，还需把旧 root 作为 child 接到新 root。 */
	if (old_root && old_root != hier->root) {
		guard(migrate)();

		if (cpumask_test_cpu(smp_processor_id(), hier->cpumask)) {
			/*
			 * If the target belong to the same hierarchy, the old root is expected
			 * to be active. Link and propagate to the new root.
			 */
			/* 当前执行 CPU 已在同 hierarchy，可本地读取/传播 active 旧 root。 */
			ret = tmigr_connect_old_root(hier, cpu, old_root, true);
		} else {
			int target = cpumask_first_and(hier->cpumask, tmigr_available_cpumask);

			if (target < nr_cpu_ids) {
				/*
				 * If the target doesn't belong to the same hierarchy as the current
				 * CPU, activate from a relevant one to make sure the old root is
				 * active.
				 */
				/* 跨 hierarchy 时把连接工作同步派到目标 hierarchy 的 available CPU，确保旧 root active。 */
				ret = work_on_cpu(target, connect_old_root_work, old_root);
			} else {
				/*
				 * No other available CPUs in the remote hierarchy. Link the
				 * old root remotely but don't propagate activation since the
				 * old root is not expected to be active.
				 */
				/* 远端 hierarchy 无 available CPU，旧 root 理应 inactive，只建链而不伪造 activation。 */
				ret = tmigr_connect_old_root(hier, cpu, old_root, false);
			}
		}
	}

	if (ret >= 0)
		cpumask_set_cpu(cpu, hier->cpumask);

	return ret;
}

/*
 * CPUHP prepare 回调，只在首次 online 初始化 per-CPU 锁、leaf event、owner/remote/wakeup，再调用 tmigr_add_cpu
 * 建链。后续 online 复用常驻对象幂等返回；建链失败或 groupmask 异常返回负 errno。
 */
static int tmigr_cpu_prepare(unsigned int cpu)
{
	struct tmigr_cpu *tmc = per_cpu_ptr(&tmigr_cpu, cpu);
	int ret = 0;

	/* Not first online attempt? */
	/* group 常驻不随 offline 销毁，后续 online 直接复用首次 prepare 结果。 */
	if (tmc->tmgroup)
		return ret;

	raw_spin_lock_init(&tmc->lock);
	timerqueue_init(&tmc->cpuevt.nextevt);
	tmc->cpuevt.nextevt.expires = KTIME_MAX;
	tmc->cpuevt.ignore = true;
	tmc->cpuevt.cpu = cpu;
	tmc->remote = false;
	WRITE_ONCE(tmc->wakeup, KTIME_MAX);

	ret = tmigr_add_cpu(cpu);
	if (ret < 0)
		return ret;

	if (tmc->groupmask == 0)
		return -EINVAL;

	return ret;
}

/*
 * early init 入口：UP 无操作；SMP 分配 available mask，按 possible CPU/node 和每组 8 child 估算层数及跨 NUMA
 * 层，随后注册 prepare 与 online/offline 两组 CPUHP 回调。任一失败打印错误并返回 errno；已注册状态由启动
 * 框架生命周期管理，本函数不提供运行期 teardown。
 */
static int __init tmigr_init(void)
{
	unsigned int cpulvl, nodelvl, cpus_per_node;
	unsigned int nnodes = num_possible_nodes();
	unsigned int ncpus = num_possible_cpus();
	int ret = -ENOMEM;

	BUILD_BUG_ON_NOT_POWER_OF_2(TMIGR_CHILDREN_PER_GROUP);

	/* Nothing to do if running on UP */
	/* 单 CPU 不存在远端 timer 迁移，保持 static 数据未启用。 */
	if (ncpus == 1)
		return 0;

	if (!zalloc_cpumask_var(&tmigr_available_cpumask, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto err;
	}

	/*
	 * Calculate the required hierarchy levels. Unfortunately there is no
	 * reliable information available, unless all possible CPUs have been
	 * brought up and all NUMA nodes are populated.
	 *
	 * Estimate the number of levels with the number of possible nodes and
	 * the number of possible CPUs. Assume CPUs are spread evenly across
	 * nodes. We cannot rely on cpumask_of_node() because it only works for
	 * online CPUs.
	 */
	/*
	 * possible CPU/node 是启动时唯一稳定上界；假设 CPU 均匀分布估算足够容纳的层数，不能使用只反映 online CPU
	 * 的 cpumask_of_node()，实际建链可在单 root 形成后提前停止。
	 */
	cpus_per_node = DIV_ROUND_UP(ncpus, nnodes);

	/* Calc the hierarchy levels required to hold the CPUs of a node */
	/* 以每组 8 child 的对数层数容纳单 node 估算 CPU 数。 */
	cpulvl = DIV_ROUND_UP(order_base_2(cpus_per_node),
			      ilog2(TMIGR_CHILDREN_PER_GROUP));

	/* Calculate the extra levels to connect all nodes */
	/* 再计算聚合所有 possible nodes 所需的跨 node 层。 */
	nodelvl = DIV_ROUND_UP(order_base_2(nnodes),
			       ilog2(TMIGR_CHILDREN_PER_GROUP));

	tmigr_hierarchy_levels = cpulvl + nodelvl;

	/*
	 * If a NUMA node spawns more than one CPU level group then the next
	 * level(s) of the hierarchy contains groups which handle all CPU groups
	 * of the same NUMA node. The level above goes across NUMA nodes. Store
	 * this information for the setup code to decide in which level node
	 * matching is no longer required.
	 */
	/* 保存 node 内层数边界；其下选择 group 必须 NUMA 匹配，其上可跨 node 合并。 */
	tmigr_crossnode_level = cpulvl;

	pr_info("Timer migration: %d hierarchy levels; %d children per group;"
		" %d crossnode level\n",
		tmigr_hierarchy_levels, TMIGR_CHILDREN_PER_GROUP,
		tmigr_crossnode_level);

	ret = cpuhp_setup_state(CPUHP_TMIGR_PREPARE, "tmigr:prepare",
				tmigr_cpu_prepare, NULL);
	if (ret)
		goto err;

	ret = cpuhp_setup_state(CPUHP_AP_TMIGR_ONLINE, "tmigr:online",
				tmigr_set_cpu_available, tmigr_clear_cpu_available);
	if (ret)
		goto err;

	return 0;

err:
	pr_err("Timer migration setup failed\n");
	return ret;
}
early_initcall(tmigr_init);
