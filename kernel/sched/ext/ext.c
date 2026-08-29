/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 中文学习注释生成模型：OpenAI Codex（GPT-5，2026-08-28）。
 * 本文件以当前提交为准解释 sched_ext 内核侧协议；BPF 策略示例和用户态
 * 装载流程分别位于 tools/sched_ext 与 BPF struct_ops 基础设施，不在本文件展开。
 */
/*
 * BPF extensible scheduler class: Documentation/scheduler/sched-ext.rst
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */
#include <linux/bitmap.h>
#include <linux/btf_ids.h>
#include <linux/rhashtable.h>
#include <linux/sched/clock.h>
#include <linux/sched/isolation.h>
#include <linux/suspend.h>
#include <linux/sysrq.h>

/* 调度核心私有头提供 rq/PELT，cid 与 arena 模块分别实现逻辑 CPU 映射和 BPF 共享内存。 */
#include "../pelt.h"
#include "internal.h"
#include "cid.h"
#include "arena.h"
#include "idle.h"

/*
 * 上游首句指出这里实现 BPF 可扩展调度类，完整接口契约见 sched-ext 文档。
 * 文件在 core scheduler 与 BPF struct_ops 之间承担可信执行层：BPF 决定 CPU、
 * DSQ 和时间片策略，内核验证返回值并维护 rq、task 生命周期和故障退化。
 */
/*
 * sched_ext 把调度策略交给 BPF，但内核仍拥有任务生命周期、rq 锁、迁移和安全兜底。
 * runnable task 在“BPF custody、QUEUEING/QUEUED/DISPATCHING、某个 DSQ、本地 rq 执行”
 * 之间转移；ops_state 的 release/acquire 与 qseq 防止异步 dispatch 使用旧一代排队权。
 * local DSQ 受对应 rq 锁保护，global/user DSQ 使用自身 raw lock；跨 DSQ/跨 rq 移动
 * 必须按既定舞步释放并重取锁，随后复验 task 状态、CPU 亲和性和 scheduler 归属。
 *
 * enable/disable 由 scx_enable_mutex 串行，静态键只在所有 task 初始化、带宽和 per-CPU
 * 资源准备完成后发布。任何 BPF 错误、watchdog 超时或热插拔序列不匹配都会进入 bypass，
 * 让内核 DSQ 保证系统继续调度，再由 workqueue 完成 dump、逐任务退出、RCU 摘除和资源回收。
 * kfunc 的 verifier 上下文门禁是锁与 ownership 契约的一部分，返回的 task/cpumask/DSQ
 * 指针只在各自声明的 RCU、引用或回调窗口内有效。
 */
/*
 * 主调用链：注册 struct_ops → scx_ops_enable() 初始化并发布 scheduler →
 * enqueue_task_scx() 把 runnable task 交给 BPF/DSQ → balance_one() 请求 dispatch →
 * pick_next_task_scx() 取本地任务 → tick/put_prev 结算；异常则 __scx_exit() →
 * scx_ops_disable_workfn()，先 bypass 保证可调度，再撤销 task 与 scheduler。
 *
 * 核心对象：scx_sched 持有一次 BPF scheduler 实例及 DSQ/每 CPU 状态；
 * sched_ext_entity 嵌入 task_struct 并记录 custody、DSQ 节点、slice 与所属 scheduler；
 * scx_dispatch_q 是 FIFO/priq 二选一的调度队列。scheduler 由 enable mutex 串行构造，
 * 通过 RCU 发布给任务，disable 时先阻止新入口、逐任务退出，宽限期后才回收。
 */

/*
 * scheduler 全局拓扑锁：与 scx_enable_mutex 共同保护增删和父子关系；热路径读者
 * 不持 mutex 时必须处于 RCU 读侧。raw spinlock 允许在调度器不可睡眠路径使用。
 */
static DEFINE_RAW_SPINLOCK(scx_sched_lock);

/*
 * NOTE: sched_ext is in the process of growing multiple scheduler support and
 * scx_root usage is in a transitional state. Naked dereferences are safe if the
 * caller is one of the tasks attached to SCX and explicit RCU dereference is
 * necessary otherwise. Naked scx_root dereferences trigger sparse warnings but
 * are used as temporary markers to indicate that the dereferences need to be
 * updated to point to the associated scheduler instances rather than scx_root.
 */
/*
 * 多 scheduler 支持仍在过渡期：属于 SCX 的 task 可裸读 root，因为其调度归属
 * 已稳定对象寿命；其他路径必须 RCU 解引用。裸读引发的 sparse 警告被保留为迁移
 * 标记，提醒调用点最终改为读取 task 对应的 scheduler，而非永久依赖全局 root。
 */
struct scx_sched __rcu *scx_root;

/*
 * All scheds, writers must hold both scx_enable_mutex and scx_sched_lock.
 * Readers can hold either or rcu_read_lock().
 */
/*
 * 该链表枚举所有已发布 scheduler。写者同时持 enable mutex 与 sched lock，避免
 * 生命周期串行化和拓扑修改分离；读者持其中任一把锁或 RCU 即可稳定节点存储期。
 */
static LIST_HEAD(scx_sched_all);

#ifdef CONFIG_EXT_SUB_SCHED
/*
 * sub-scheduler 以 cgroup id 为稳定键，hash_node 嵌入 scheduler；结构变化持
 * scx_sched_lock，RCU 查找只借用仍处于宽限期内的对象。
 */
static const struct rhashtable_params scx_sched_hash_params = {
	.key_len		= sizeof_field(struct scx_sched, ops.sub_cgroup_id),
	.key_offset		= offsetof(struct scx_sched, ops.sub_cgroup_id),
	.head_offset		= offsetof(struct scx_sched, hash_node),
	.insecure_elasticity	= true,	/* inserted under scx_sched_lock */
	/* 该哈希项只在持 scx_sched_lock 时插入，insecure_elasticity 不放宽写侧串行要求。 */
};

static struct rhashtable scx_sched_hash;
#endif

/* see SCX_OPS_TID_TO_TASK */
/*
 * 上游提示该表服务 SCX_OPS_TID_TO_TASK。键和值节点都嵌入 p->scx，表本身不取得
 * task 引用；从 fork 插入到最终 free 摘除的专用 task 链表覆盖其生命周期。
 */
static const struct rhashtable_params scx_tid_hash_params = {
	.key_len		= sizeof_field(struct sched_ext_entity, tid),
	.key_offset		= offsetof(struct sched_ext_entity, tid),
	.head_offset		= offsetof(struct sched_ext_entity, tid_hash_node),
	.insecure_elasticity	= true,	/* inserted/removed under scx_tasks_lock */
	/* 该哈希项的插入和摘除都持 scx_tasks_lock，参数只声明哈希弹性策略。 */
};
static struct rhashtable scx_tid_hash;

/*
 * During exit, a task may schedule after losing its PIDs. When disabling the
 * BPF scheduler, we need to be able to iterate tasks in every state to
 * guarantee system safety. Maintain a dedicated task list which contains every
 * task between its fork and eventual free.
 */
/*
 * task 退出时可能先失去 PID，禁用 scheduler 却必须覆盖 sleeping、runnable、dead
 * 等所有状态；因此不能依赖 PID 哈希或 rq 枚举。scx_tasks 从 fork 到最终 free
 * 独立追踪每个 task，scx_tasks_lock 同时保护链表和 tid 哈希的插入/摘除。
 */
static DEFINE_RAW_SPINLOCK(scx_tasks_lock);
static LIST_HEAD(scx_tasks);

/* ops enable/disable */
/*
 * enable_mutex 串行整个实例装卸事务；enable_state 是外部可观察状态机，静态键是
 * 热路径发布门。fork rwsem 防止 task 构造与全量切换交错；bypass_lock 保护嵌套
 * 故障退化深度，使任务搬运只发生在 0↔1 边界。
 */
static DEFINE_MUTEX(scx_enable_mutex);
DEFINE_STATIC_KEY_FALSE(__scx_enabled);
DEFINE_STATIC_PERCPU_RWSEM(scx_fork_rwsem);
static atomic_t scx_enable_state_var = ATOMIC_INIT(SCX_DISABLED);
static DEFINE_RAW_SPINLOCK(scx_bypass_lock);
static bool scx_init_task_enabled;
static bool scx_switching_all;
DEFINE_STATIC_KEY_FALSE(__scx_switched_all);
static DEFINE_STATIC_KEY_FALSE(__scx_tid_to_task_enabled);

/*
 * True once SCX_OPS_TID_TO_TASK has been negotiated with the root scheduler
 * and the tid->task table is live. Wraps the static key so callers don't
 * take the address, and hints "likely enabled" for the common case where
 * the feature is in use.
 */
/* 静态键只在 tid 哈希表完全可用后开启，读者因此不会观察半初始化表。 */
/*
 * root scheduler 协商 TID_TO_TASK 且 tid 表已可用后才开启静态键；读者不会观察
 * 半初始化表，常见关闭状态也只付出静态分支成本。
 *
 * 业务背景：为所有 tid 反查调用提供唯一、可内联的功能门禁。
 * 入参：无。
 * 出参/返回：返回功能是否已发布；不取得引用，也不改变全局状态。
 * 注意事项：只保证表可访问，返回 task 仍须遵守具体 kfunc 的 RCU/引用契约。
 */
static inline bool scx_tid_to_task_enabled(void)
{
	return static_branch_likely(&__scx_tid_to_task_enabled);
}

static atomic_long_t scx_nr_rejected = ATOMIC_LONG_INIT(0);
static atomic_long_t scx_hotplug_seq = ATOMIC_LONG_INIT(0);

/* Global cursor for the per-CPU tid allocator. Starts at 1; tid 0 is reserved. */
/* 全局游标批量预留 tid，初值 1；0 专作“尚未分配”哨兵，不能发给 task。 */
static atomic64_t scx_tid_cursor = ATOMIC64_INIT(1);

#ifdef CONFIG_EXT_SUB_SCHED
/*
 * The sub sched being enabled. Used by scx_disable_and_exit_task() to exit
 * tasks for the sub-sched being enabled. Use a global variable instead of a
 * per-task field as all enables are serialized.
 */
/* enable_mutex 保证同一时刻至多一个启用事务，故全局借用指针无需放进每个 task。 */
static struct scx_sched *scx_enabling_sub_sched;
#else
#define scx_enabling_sub_sched	(struct scx_sched *)NULL
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

/*
 * A monotonically increasing sequence number that is incremented every time a
 * scheduler is enabled. This can be used to check if any custom sched_ext
 * scheduler has ever been used in the system.
 */
/* 单调序列记录成功启用历史，不随 disable 回退，不能当作当前实例数或引用计数。 */
static atomic_long_t scx_enable_seq = ATOMIC_LONG_INIT(0);

/*
 * Watchdog interval. All scx_sched's share a single watchdog timer and the
 * interval is half of the shortest sch->watchdog_timeout.
 */
/* 共享 watchdog 取所有实例最短 timeout 的一半，给最严格实例保留中途检查机会。 */
static unsigned long scx_watchdog_interval;

/*
 * The last time the delayed work was run. This delayed work relies on
 * ksoftirqd being able to run to service timer interrupts, so it's possible
 * that this work itself could get wedged. To account for this, we check that
 * it's not stalled in the timer tick, and trigger an error if it is.
 */
/* tick 也检查本时间戳，用独立路径发现依赖 ksoftirqd 的 watchdog work 自己被饿死。 */
static unsigned long scx_watchdog_timestamp = INITIAL_JIFFIES;

static struct delayed_work scx_watchdog_work;

/*
 * For %SCX_KICK_WAIT: Each CPU has a pointer to an array of kick_sync sequence
 * numbers. The arrays are allocated with kvzalloc() as size can exceed percpu
 * allocator limits on large machines. O(nr_cpu_ids^2) allocation, allocated
 * lazily when enabling and freed when disabling to avoid waste when sched_ext
 * isn't active.
 */
/*
 * SCX_KICK_WAIT 让发起 CPU 等目标 CPU 越过同步点。每 CPU 指针指向按目标 CPU
 * 编号索引的序列数组；大机器上总空间为 O(nr_cpu_ids²)，所以仅在 enable 时
 * 惰性分配，disable 后经 RCU 回收，避免并发读者访问旧数组时发生 UAF。
 */
struct scx_kick_syncs {
	/* rcu 串联延迟释放；syncs[] 的每项对应一个目标 CPU 的完成代际。 */
	struct rcu_head		rcu;
	unsigned long		syncs[];
};

static DEFINE_PER_CPU(struct scx_kick_syncs __rcu *, scx_kick_syncs);

/*
 * Per-CPU buffered allocator state for p->scx.tid. Each CPU pulls a chunk of
 * SCX_TID_CHUNK ids from scx_tid_cursor and hands them out locally without
 * further synchronization. See scx_alloc_tid().
 */
/*
 * next 是本 CPU 下一个可发 tid，end 是当前批次的开区间上界。只有 refill 访问
 * 全局原子游标；禁抢占的分配路径确保同一 CPU 不会并发修改这两个字段。
 */
struct scx_tid_alloc {
	u64	next;
	u64	end;
};
static DEFINE_PER_CPU(struct scx_tid_alloc, scx_tid_alloc);

/*
 * Direct dispatch marker.
 *
 * Non-NULL values are used for direct dispatch from enqueue path. A valid
 * pointer points to the task currently being enqueued. An ERR_PTR value is used
 * to indicate that direct dispatch has already happened.
 */
/*
 * enqueue 回调入口写当前 task，BPF 直派后改成 ERR_PTR 哨兵；回调返回后内核据此
 * 区分普通 enqueue、一次合法直派和重复直派错误。该 per-CPU 裸指针只在回调窗口有效。
 */
static DEFINE_PER_CPU(struct task_struct *, direct_dispatch_task);

/* user DSQ 以 64 位 id 索引；节点嵌入 DSQ，摘表后等待 RCU 才能释放对象。 */
static const struct rhashtable_params dsq_hash_params = {
	.key_len		= sizeof_field(struct scx_dispatch_q, id),
	.key_offset		= offsetof(struct scx_dispatch_q, id),
	.head_offset		= offsetof(struct scx_dispatch_q, hash_node),
};

static LLIST_HEAD(dsqs_to_free);

/* string formatting from BPF */
/*
 * BPF dump 的变参先复制到固定 data 数组，再格式化进 line，避免诊断路径动态分配；
 * 全局实例由 scx_exit_bstr_buf_lock 串行，缓冲内容只在持锁窗口有效。
 */
struct scx_bstr_buf {
	u64			data[MAX_BPRINTF_VARARGS];
	char			line[SCX_EXIT_MSG_LEN];
};

static DEFINE_RAW_SPINLOCK(scx_exit_bstr_buf_lock);
static struct scx_bstr_buf scx_exit_bstr_buf;

/* ops debug dump */
/*
 * dump_lock 串行 sysrq、错误退出和用户触发诊断；scx_dump_data 保存一次遍历的 CPU、
 * task 游标和借用 seq_buf，只有持锁的 dump 调用链可以修改。
 */
static DEFINE_RAW_SPINLOCK(scx_dump_lock);

struct scx_dump_data {
	s32			cpu;
	bool			first;
	s32			cursor;
	struct seq_buf		*s;
	const char		*prefix;
	/* 单次 dump 的格式化 scratch 由触发 CPU 独占，cursor 支持跨多次 BPF 调用拼接。 */
	struct scx_bstr_buf	buf;
};

static struct scx_dump_data scx_dump_data = {
	.cpu			= -1,
};

/* /sys/kernel/sched_ext interface */
/* kset 在全局初始化时创建；属性读取只暴露原子快照或 mutex 稳定的实例数据。 */
static struct kset *scx_kset;

/*
 * Parameters that can be adjusted through /sys/module/sched_ext/parameters.
 * There usually is no reason to modify these as normal scheduler operation
 * shouldn't be affected by them. The knobs are primarily for debugging.
 */
/*
 * 这些参数仅调节 bypass 的保底 slice 与负载均衡周期，正常 BPF scheduler 不应
 * 依赖它们。两个存储值的单位均为微秒，写入由专用 setter 做范围门禁。
 */
static unsigned int scx_slice_bypass_us = SCX_SLICE_BYPASS / NSEC_PER_USEC;
static unsigned int scx_bypass_lb_intv_us = SCX_BYPASS_LB_DFL_INTV_US;

/* 调试参数限定 100us..100s，非法文本或越界值原样返回参数解析错误。 */
/*
 * 业务背景：把 bypass 时间片限制在既保证前进又不会长时间独占 CPU 的范围。
 * 入参：val 是用户文本；kp 借用参数描述，指向待写的 unsigned int。
 * 出参/返回：成功返回 0 并写入 100..100000 微秒；解析或越界返回负 errno。
 * 注意事项：只修改参数，已在队列中的 task 在下一次补充 slice 时读取新值。
 */
static int set_slice_us(const char *val, const struct kernel_param *kp)
{
	return param_set_uint_minmax(val, kp, 100, 100 * USEC_PER_MSEC);
}

static const struct kernel_param_ops slice_us_param_ops = {
	/* set 执行范围检查，get 复用 unsigned int 的标准文本序列化。 */
	.set = set_slice_us,
	.get = param_get_uint,
};

/*
 * 业务背景：控制 bypass 跨 CPU 均衡最小间隔，0 明确表示关闭周期搬运。
 * 入参：val 是用户文本；kp 为借用参数元数据，目标值单位微秒。
 * 出参/返回：0 表示写入 0..10000000；非法文本或越界返回负 errno。
 * 注意事项：正在运行的 bypass 路径在下次读取参数时自然生效。
 */
static int set_bypass_lb_intv_us(const char *val, const struct kernel_param *kp)
{
	return param_set_uint_minmax(val, kp, 0, 10 * USEC_PER_SEC);
}

static const struct kernel_param_ops bypass_lb_intv_us_param_ops = {
	/* 读取无副作用；写入经上面的本参数范围校验。 */
	.set = set_bypass_lb_intv_us,
	.get = param_get_uint,
};

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX	"sched_ext."

module_param_cb(slice_bypass_us, &slice_us_param_ops, &scx_slice_bypass_us, 0600);
MODULE_PARM_DESC(slice_bypass_us, "bypass slice in microseconds, applied on [un]load (100us to 100ms)");
module_param_cb(bypass_lb_intv_us, &bypass_lb_intv_us_param_ops, &scx_bypass_lb_intv_us, 0600);
MODULE_PARM_DESC(bypass_lb_intv_us, "bypass load balance interval in microseconds (0 (disable) to 10s)");

/* 参数宏作用域到此结束；tracepoint 定义必须在恰好一个翻译单元声明 CREATE_TRACE_POINTS。 */
#undef MODULE_PARAM_PREFIX

#define CREATE_TRACE_POINTS
#include <trace/events/sched_ext.h>

static void run_deferred(struct rq *rq);
static bool task_dead_and_done(struct task_struct *p);
static void scx_kick_cpu(struct scx_sched *sch, s32 cpu, u64 flags);
static void scx_disable(struct scx_sched *sch, enum scx_exit_kind kind);

/*
 * 业务背景：统一把格式化退出原因送入 scx_vexit()，供 BPF 错误、watchdog 和控制
 * 路径登记 scheduler 的一次性退出；真正 disable 由工作队列在可睡眠上下文完成。
 * 入参：sch 为借用 scheduler；kind/exit_code/exit_cpu 描述原因、状态码和发生 CPU；
 * fmt 及变参只在本调用内有效，必须满足 printf 类型契约。
 * 出参/返回：返回 scx_vexit() 是否首次登记退出；不转移任何对象 ownership。
 * 注意事项：可从不可睡眠路径调用，不能在这里同步销毁仍被 rq/RCU 读者引用的实例。
 */
__printf(5, 6) bool __scx_exit(struct scx_sched *sch,
			       enum scx_exit_kind kind, s64 exit_code,
			       s32 exit_cpu, const char *fmt, ...)
{
	va_list args;
	bool ret;

	/* va_list 的创建和销毁都留在包装层，被调函数只借用一次遍历状态。 */
	va_start(args, fmt);
	ret = scx_vexit(sch, kind, exit_code, exit_cpu, fmt, args);
	va_end(args);

	return ret;
}

/*
 * 业务背景：以有符号毫秒表达两个可回绕 jiffies 时间点的先后距离。
 * 入参：at/now 为 jiffies 时间戳；允许计数器回绕，但差值须落在 time_after 有效窗。
 * 出参/返回：at 在未来为正、过去为负、相等为 0；无副作用。
 * 注意事项：先用回绕安全比较决定方向，再转换绝对差，不能直接做有符号减法。
 */
static long jiffies_delta_msecs(unsigned long at, unsigned long now)
{
	if (time_after(at, now))
		return jiffies_to_msecs(at - now);
	else
		return -(long)jiffies_to_msecs(now - at);
}

/*
 * 业务背景：比较会回绕的 32 位代际号，供 qseq/cursor 判断事件先后。
 * 入参：a、b 为同一序列空间的值，距离必须小于半个 u32 范围。
 * 出参/返回：a 在模 2^32 顺序上早于 b 时为 true；无副作用。
 * 注意事项：强转 s32 利用二补码窗口，不能用于相距任意远的普通整数。
 */
static bool u32_before(u32 a, u32 b)
{
	return (s32)(a - b) < 0;
}

#ifdef CONFIG_EXT_SUB_SCHED
/**
 * scx_next_descendant_pre - find the next descendant for pre-order walk
 * @pos: the current position (%NULL to initiate traversal)
 * @root: sched whose descendants to walk
 *
 * To be used by scx_for_each_descendant_pre(). Find the next descendant to
 * visit for pre-order traversal of @root's descendants. @root is included in
 * the iteration and the first node to be visited.
 */
/*
 * 上游契约由前面的 kernel-doc 给出：NULL cursor 从 root 开始，root 自身也是首个
 * 节点。返回值是树内借用指针，锁保证 sibling/parent 拓扑不在遍历中改变。
 *
 * 业务背景：disable、bypass 与拓扑管理需要父节点先于后代的稳定遍历顺序。
 * 入参：pos 为当前借用节点或 NULL；root 为不可空遍历根，二者属于同一树。
 * 出参/返回：下一借用 scheduler，结束时 NULL；不改变树或 ownership。
 * 注意事项：调用者必须持 enable_mutex 或 scx_sched_lock；函数不可睡眠。
 */
static struct scx_sched *scx_next_descendant_pre(struct scx_sched *pos,
						 struct scx_sched *root)
{
	struct scx_sched *next;

	lockdep_assert(lockdep_is_held(&scx_enable_mutex) ||
		       lockdep_is_held(&scx_sched_lock));

	/* if first iteration, visit @root */
	/* 首轮没有 cursor，按先序定义先交付 root，而不是直接进入第一个 child。 */
	if (!pos)
		return root;

	/* visit the first child if exists */
	/* 有子节点时立即下潜，维持“父节点先于整个子树”的顺序。 */
	next = list_first_entry_or_null(&pos->children, struct scx_sched, sibling);
	if (next)
		return next;

	/* no child, visit my or the closest ancestor's next sibling */
	/* 无 child 就逐级回溯；锁使 sibling 链与 parent 指针在搜索中保持一致。 */
	while (pos != root) {
		if (!list_is_last(&pos->sibling, &scx_parent(pos)->children))
			return list_next_entry(pos, sibling);
		pos = scx_parent(pos);
	}

	return NULL;
}

/* 在 RCU/写锁保护下按 cgroup id 查询 sub-scheduler；返回借用指针。 */
/*
 * 业务背景：控制路径按 cgroup id 定位已发布 sub-scheduler。
 * 入参：cgroup_id 为 64 位查找键。
 * 出参/返回：命中返回表拥有的借用指针，否则 NULL；不增加引用。
 * 注意事项：调用者须持 RCU 或 scheduler 写侧锁以稳定对象寿命。
 */
static struct scx_sched *scx_find_sub_sched(u64 cgroup_id)
{
	return rhashtable_lookup(&scx_sched_hash, &cgroup_id,
				 scx_sched_hash_params);
}

/* 以 RCU 指针发布 task 所属 scheduler；调用者先完成旧/新两侧状态交接。 */
/*
 * 业务背景：把 task 调度归属作为 RCU 发布点，使无锁读者只看到完整 scheduler。
 * 入参：p 为借用 task；sch 为新借用 scheduler，可按调用协议为空。
 * 出参/返回：无直接返回值；更新 p->scx.sched，不转移 scheduler ownership。
 * 注意事项：调用者先完成旧/新侧交接，并保证对象至少活过 RCU 读者窗口。
 */
static void scx_set_task_sched(struct task_struct *p, struct scx_sched *sch)
{
	rcu_assign_pointer(p->scx.sched, sch);
}
#else	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */
static inline struct scx_sched *scx_next_descendant_pre(struct scx_sched *pos, struct scx_sched *root) { return pos ? NULL : root; }
/* 未启用 sub-scheduler 时归属恒为 root，此 stub 不写 task，也不持有传入指针。 */
static inline void scx_set_task_sched(struct task_struct *p, struct scx_sched *sch) {}
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

/**
 * scx_is_descendant - Test whether sched is a descendant
 * @sch: sched to test
 * @ancestor: ancestor sched to test against
 *
 * Test whether @sch is a descendant of @ancestor.
 */
/*
 * 上游契约是判断 sch 是否属于 ancestor 子树。先比较 level，避免以后代层级索引
 * 较短的 ancestors 数组，再以预计算祖先指针 O(1) 判定；节点对自身也返回 true。
 * 入参：sch/ancestor 均为锁或 RCU 稳定的借用 scheduler，不可空。
 * 出参/返回：在同一祖先链为 true，否则 false；无副作用和引用变化。
 * 注意事项：只验证拓扑关系，不稳定对象寿命，也不检查 enable 状态。
 */
static bool scx_is_descendant(struct scx_sched *sch, struct scx_sched *ancestor)
{
	if (sch->level < ancestor->level)
		return false;
	return sch->ancestors[ancestor->level] == ancestor;
}

/**
 * scx_for_each_descendant_pre - pre-order walk of a sched's descendants
 * @pos: iteration cursor
 * @root: sched to walk the descendants of
 *
 * Walk @root's descendants. @root is included in the iteration and the first
 * node to be visited. Must be called with either scx_enable_mutex or
 * scx_sched_lock held.
 */
/*
 * 业务背景：把 CPU 映射到 scheduler 的 NUMA pnode，并返回该节点共享 global DSQ。
 * 入参：sch 为稳定借用 scheduler；cpu 必须是可能 CPU 且对应 pnode 已初始化。
 * 出参/返回：返回嵌入 pnode 的借用 DSQ；不取引用、无失败值。
 * 注意事项：调用者负责相应 rq/DSQ 锁和 scheduler 生命周期，函数不可睡眠。
 */
#define scx_for_each_descendant_pre(pos, root)					\
	for ((pos) = scx_next_descendant_pre(NULL, (root)); (pos);		\
	     (pos) = scx_next_descendant_pre((pos), (root)))

/*
 * 业务背景：把 CPU 映射到 scheduler 的 NUMA pnode，并返回该节点共享 global DSQ。
 * 入参：sch 为稳定借用 scheduler；cpu 必须是可能 CPU 且对应 pnode 已初始化。
 * 出参/返回：返回嵌入 pnode 的借用 DSQ；不取引用、无失败值。
 * 注意事项：调用者负责相应 rq/DSQ 锁和 scheduler 生命周期，函数不可睡眠。
 */
static struct scx_dispatch_q *find_global_dsq(struct scx_sched *sch, s32 cpu)
{
	return &sch->pnode[cpu_to_node(cpu)]->global_dsq;
}

/* rhashtable 查找只返回 scheduler 拥有的 user DSQ，生命周期由 RCU/控制路径稳定。 */
/*
 * 业务背景：按 BPF 指定 id 查找 scheduler 私有 user DSQ。
 * 入参：sch 为稳定借用 scheduler；dsq_id 为完整 64 位队列标识。
 * 出参/返回：命中返回借用 DSQ，未创建或已摘除返回 NULL；不取得引用。
 * 注意事项：调用者须持 RCU 或控制路径锁，并另行遵守 DSQ raw lock 协议。
 */
static struct scx_dispatch_q *find_user_dsq(struct scx_sched *sch, u64 dsq_id)
{
	return rhashtable_lookup(&sch->dsq_hash, &dsq_id, dsq_hash_params);
}

/*
 * 业务背景：setscheduler 改策略时决定 task 应落到哪个内建 class，stop task 必须
 * 保持最高优先级 stop class，不能被通用 policy/prio 映射覆盖。
 * 入参：p 为 rq 锁稳定的借用 task。
 * 出参/返回：返回静态 sched_class 指针，无失败和 ownership 转移。
 * 注意事项：这里只选择 class，不执行 dequeue/enqueue 或发布状态。
 */
static const struct sched_class *scx_setscheduler_class(struct task_struct *p)
{
	if (p->sched_class == &stop_sched_class)
		return &stop_sched_class;

	return __setscheduler_class(p->policy, p->prio);
}

/*
 * 业务背景：故障 bypass 为每 CPU 提供内核控制的保底 DSQ，绕开失效 BPF 策略。
 * 入参：sch 为稳定 scheduler；cpu 为其已分配 per-CPU 状态的 CPU。
 * 出参/返回：返回嵌入 per-CPU 状态的借用 DSQ，无失败和引用变化。
 * 注意事项：队列仍受对应锁保护，取到指针不等于已持锁。
 */
static struct scx_dispatch_q *bypass_dsq(struct scx_sched *sch, s32 cpu)
{
	return &per_cpu_ptr(sch->pcpu, cpu)->bypass_dsq;
}

/*
 * 业务背景：sub-scheduler bypass 时由最近未 bypass 的祖先接管其整棵子树，避免
 * task 留在一个不再 dispatch 的 scheduler 中；若全链退化则交给 root 保底 DSQ。
 * 入参：sch 为 task 当前 scheduler；cpu 为目标 CPU，二者在拓扑锁/RCU 下稳定。
 * 出参/返回：返回接管 scheduler 的 per-CPU bypass DSQ 借用指针。
 * 注意事项：开始 bypass 后会重入队整棵子树，保证既有 runnable task 也迁到同一规则。
 */
static struct scx_dispatch_q *bypass_enq_target_dsq(struct scx_sched *sch, s32 cpu)
{
#ifdef CONFIG_EXT_SUB_SCHED
	/*
	 * If @sch is a sub-sched which is bypassing, its tasks should go into
	 * the bypass DSQs of the nearest ancestor which is not bypassing. The
	 * not-bypassing ancestor is responsible for scheduling all tasks from
	 * bypassing sub-trees. If all ancestors including root are bypassing,
	 * all tasks should go to the root's bypass DSQs.
	 *
	 * Whenever a sched starts bypassing, all runnable tasks in its subtree
	 * are re-enqueued after scx_bypassing() is turned on, guaranteeing that
	 * all tasks are transferred to the right DSQs.
	 */
	/*
	 * 上游说明：向上找到最近未 bypass 的祖先，由它调度退化子树；若 root 也在
	 * bypass，循环停在 root 并使用 root DSQ。开启退化后全量 re-enqueue 建立一致性。
	 */
	while (scx_parent(sch) && scx_bypassing(sch, cpu))
		sch = scx_parent(sch);
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

	return bypass_dsq(sch, cpu);
}

/**
 * bypass_dsp_enabled - Check if bypass dispatch path is enabled
 * @sch: scheduler to check
 *
 * When a descendant scheduler enters bypass mode, bypassed tasks are scheduled
 * by the nearest non-bypassing ancestor, or the root scheduler if all ancestors
 * are bypassing. In the former case, the ancestor is not itself bypassing but
 * its bypass DSQs will be populated with bypassed tasks from descendants. Thus,
 * the ancestor's bypass dispatch path must be active even though its own
 * bypass_depth remains zero.
 *
 * This function checks bypass_dsp_enable_depth which is managed separately from
 * bypass_depth to enable this decoupling. See enable_bypass_dsp() and
 * disable_bypass_dsp().
 */
/*
 * 上游说明：祖先自身 bypass_depth 可为 0，却仍需从 bypass DSQ 调度已退化后代；
 * 独立的 bypass_dsp_enable_depth 正是把“自身退化”与“代管后代”解耦。
 * 入参：sch 为生命周期稳定的借用 scheduler。
 * 出参/返回：任一自身/后代请求使深度非零时为 true；无状态修改。
 * 注意事项：原子读保证并发计数快照，unlikely 只提供分支布局提示，不增加同步。
 */
static bool bypass_dsp_enabled(struct scx_sched *sch)
{
	return unlikely(atomic_read(&sch->bypass_dsp_enable_depth));
}

/**
 * rq_is_open - Is the rq available for immediate execution of an SCX task?
 * @rq: rq to test
 * @enq_flags: optional %SCX_ENQ_* of the task being enqueued
 *
 * Returns %true if @rq is currently open for executing an SCX task. After a
 * %false return, @rq is guaranteed to invoke SCX dispatch path at least once
 * before going to idle and not inserting a task into @rq's local DSQ after a
 * %false return doesn't cause @rq to stall.
 */
/* rq 只有在线且未被 CPU release 协议关闭时可接收普通 enqueue。 */
/*
 * 上游契约：判断 rq 当前是否能立刻执行 SCX task；false 保证 rq 在 idle 前至少再
 * 进入一次 SCX dispatch，所以调用者不把 task 直塞 local DSQ 也不会让 CPU 永久停顿。
 * 入参：rq 为已持 rq lock 的借用队列；enq_flags 是当前入队 task 的 SCX_ENQ 位图。
 * 出参/返回：可安全立即插入为 true，否则 false；函数只读 rq 状态。
 * 注意事项：判断同时考虑更高 class、SCX balance 中间态和 PREEMPT 对当前 slice 的清零。
 */
static bool rq_is_open(struct rq *rq, u64 enq_flags)
{
	lockdep_assert_rq_held(rq);

	/*
	 * A higher-priority class task is either running or in the process of
	 * waking up on @rq.
	 */
	/* 更高 class 已运行或正在唤醒，SCX 即使入本地队列也不能立即获得 CPU。 */
	if (sched_class_above(rq->next_class, &ext_sched_class))
		return false;

	/*
	 * @rq is either in transition to or in idle and there is no
	 * higher-priority class task waking up on it.
	 */
	/* next_class 低于 SCX 表示 rq 正向 idle/更低 class 过渡，当前可由 SCX 抢占使用。 */
	if (sched_class_above(&ext_sched_class, rq->next_class))
		return true;

	/*
	 * @rq is either picking, in transition to, or running an SCX task.
	 */
	/* 此时 rq 正在选择、切换到或运行 SCX task，下面继续区分 balance、抢占和普通占用状态。 */

	/*
	 * If we're in the dispatch path holding rq lock, $curr may or may not
	 * be ready depending on whether the on-going dispatch decides to extend
	 * $curr's slice. We say yes here and resolve it at the end of dispatch.
	 * See balance_one().
	 */
	/* balance 尚未决定是否延长 curr slice，先允许插入，末尾再统一解析竞态结果。 */
	if (rq->scx.flags & SCX_RQ_IN_BALANCE)
		return true;

	/*
	 * %SCX_ENQ_PREEMPT clears $curr's slice if on SCX and kicks dispatch,
	 * so allow it to avoid spuriously triggering reenq on a combined
	 * PREEMPT|IMMED insertion.
	 */
	/* PREEMPT 已承诺触发新一轮 dispatch，允许 IMMED 直插不会遗漏重新选任务。 */
	if (enq_flags & SCX_ENQ_PREEMPT)
		return true;

	/*
	 * @rq is either in transition to or running an SCX task and can't go
	 * idle without another SCX dispatch cycle.
	 */
	/* rq 正在切换到或运行 SCX task，进入 idle 前必然再走一轮 SCX dispatch，因此返回 false 仍不会停顿。 */
	return false;
}

/*
 * Track the rq currently locked.
 *
 * This allows kfuncs to safely operate on rq from any scx ops callback,
 * knowing which rq is already locked.
 */
/* kfunc 跨 rq 操作按统一顺序切锁，并同步 verifier 追踪的当前 locked rq。 */
DEFINE_PER_CPU(struct rq *, scx_locked_rq_state);

/* kfunc 跨 rq 操作按统一顺序切锁，并同步 verifier 追踪的当前 locked rq。 */
/*
 * 业务背景：kfunc 需要从当前 rq 切到另一个 rq，同时维护 verifier/运行时看到的
 * locked-rq 状态，避免回调误以为旧锁仍持有。
 * 入参：from 为当前已持锁 rq；to 为待取得锁 rq，均为借用指针且不可相同。
 * 出参/返回：无直接返回；释放 from、取得 to，若 from 被追踪则追踪指针同步迁移。
 * 注意事项：调用者负责全局 rq 锁顺序；切锁窗口内必须重新验证 task/rq 归属。
 */
static void switch_rq_lock(struct rq *from, struct rq *to)
{
	bool tracked = scx_locked_rq() == from;

	/* 先清追踪再解锁，禁止 kfunc 在无锁窗口获得一个虚假的已锁 rq。 */
	if (tracked)
		update_locked_rq(NULL);
	raw_spin_rq_unlock(from);
	raw_spin_rq_lock(to);
	if (tracked)
		update_locked_rq(to);
}

/*
 * Flipped on enable per sch->is_cid_type. Declared in internal.h so
 * subsystem inlines can read it.
 */
/* enable 根据 sch->is_cid_type 切换该静态键；声明位于 internal.h，供子模块内联热路径读取。 */
DEFINE_STATIC_KEY_FALSE(__scx_is_cid_type);

/**
 * scx_call_op_set_cpumask - invoke ops.set_cpumask / ops_cid.set_cmask for @task
 * @sch: scx_sched being invoked
 * @rq: rq to update as the currently-locked rq, or NULL
 * @task: task whose affinity is changing
 * @cpumask: new cpumask
 *
 * For cid-form schedulers, translate @cpumask to a cmask via the per-cpu
 * scratch in cid.c and dispatch through the ops_cid union view. Caller
 * must hold @rq's rq lock so this_cpu_ptr is stable across the call.
 */
/*
 * 上游契约：调用普通 set_cpumask 或 CID 版本 set_cmask；CID 路径把 cpumask 转为
 * 本 CPU arena scratch。rq lock + IRQ disabled 让 scratch 在整个 BPF 回调中独占。
 * 入参：sch 为 scheduler；rq 可空，非空时已持锁；task 为借用目标；cpumask 为新亲和
 * 集合的只读借用指针，回调不得保存内核地址。
 * 出参/返回：无直接返回；BPF 收到 affinity 变化通知，临时 kfunc task/rq 上下文会清空。
 * 注意事项：kf_tasks[0] 必须空；无论分派哪种 ABI，回调后都撤销临时授权。
 */
static inline void scx_call_op_set_cpumask(struct scx_sched *sch, struct rq *rq,
					   struct task_struct *task,
					   const struct cpumask *cpumask)
{
	WARN_ON_ONCE(current->scx.kf_tasks[0]);
	/* 在 current 上登记唯一可供受限 kfunc 使用的 task，回调结束必须成对清空。 */
	current->scx.kf_tasks[0] = task;
	if (rq)
		update_locked_rq(rq);

	if (scx_is_cid_type()) {
		struct scx_cmask *kern_va = *this_cpu_ptr(sch->set_cmask_scratch);
		/*
		 * Build the per-CPU arena cmask and hand BPF its arena address.
		 * Caller holds the rq lock with IRQs disabled, which makes us
		 * the sole user of the scratch area.
		 */
		/* 把普通 CPU 位图复制到 arena 对象，只把 arena 地址暴露给 CID BPF 程序。 */
		scx_cpumask_to_cmask(cpumask, kern_va);
		sch->ops_cid.set_cmask(task, scx_kaddr_to_arena(sch, kern_va));
	} else {
		sch->ops.set_cpumask(task, cpumask);
	}

	if (rq)
		update_locked_rq(NULL);
	current->scx.kf_tasks[0] = NULL;
}

/*
 * DSQ 迭代标志分两层：REV 是用户可选的逆 dispatch 顺序；HAS_SLICE/HAS_VTIME
 * 是内核记录 move helper 是否覆盖字段的内部状态，不能由 BPF 任意传入。
 */
enum scx_dsq_iter_flags {
	/* iterate in the reverse dispatch order */
	/* 从队尾向队头遍历，适合按最晚 dispatch 候选开始检查的策略。 */
	SCX_DSQ_ITER_REV		= 1U << 16,

	/* move 前设置 slice/vtime 的一次性记录，提交 task 后由内核消费。 */
	__SCX_DSQ_ITER_HAS_SLICE	= 1U << 30,
	__SCX_DSQ_ITER_HAS_VTIME	= 1U << 31,

	__SCX_DSQ_ITER_USER_FLAGS	= SCX_DSQ_ITER_REV,
	__SCX_DSQ_ITER_ALL_FLAGS	= __SCX_DSQ_ITER_USER_FLAGS |
					  __SCX_DSQ_ITER_HAS_SLICE |
					  __SCX_DSQ_ITER_HAS_VTIME,
};

/**
 * nldsq_next_task - Iterate to the next task in a non-local DSQ
 * @dsq: non-local dsq being iterated
 * @cur: current position, %NULL to start iteration
 * @rev: walk backwards
 *
 * Returns %NULL when iteration is finished.
 */
/*
 * 上游契约：在 non-local DSQ 中找 cur 后的真实 task，rev 决定方向，遍历结束 NULL。
 * 入参：dsq 为已持 dsq->lock 的借用队列；cur 可空且若非空必须仍链接在该 DSQ；
 * rev 为方向，不改变队列顺序。
 * 出参/返回：返回未加引用的 task 借用指针或 NULL；锁外不能继续使用。
 * 注意事项：链表还混入 BPF iterator cursor，必须跳过这些非 task 节点再 container_of。
 */
static struct task_struct *nldsq_next_task(struct scx_dispatch_q *dsq,
					   struct task_struct *cur, bool rev)
{
	struct list_head *list_node;
	struct scx_dsq_list_node *dsq_lnode;

	lockdep_assert_held(&dsq->lock);

	if (cur)
		list_node = &cur->scx.dsq_list.node;
	else
		list_node = &dsq->list;

	/* find the next task, need to skip BPF iteration cursors */
	/* 每轮先沿选定方向移动；遇到表头即结束，cursor 节点只作定位不能返回给调用者。 */
	do {
		if (rev)
			list_node = list_node->prev;
		else
			list_node = list_node->next;

		if (list_node == &dsq->list)
			return NULL;

		dsq_lnode = container_of(list_node, struct scx_dsq_list_node,
					 node);
		/* cursor 节点只是遍历锚，不对应 task；循环跳过所有并发 iterator 的锚点。 */
	} while (dsq_lnode->flags & SCX_DSQ_LNODE_ITER_CURSOR);

	return container_of(dsq_lnode, struct task_struct, scx.dsq_list);
}

#define nldsq_for_each_task(p, dsq)						\
	for ((p) = nldsq_next_task((dsq), NULL, false); (p);			\
	     (p) = nldsq_next_task((dsq), (p), false))

/**
 * nldsq_cursor_next_task - Iterate to the next task given a cursor in a non-local DSQ
 * @cursor: scx_dsq_list_node initialized with INIT_DSQ_LIST_CURSOR()
 * @dsq: non-local dsq being iterated
 *
 * Find the next task in a cursor based iteration. The caller must have
 * initialized @cursor using INIT_DSQ_LIST_CURSOR() and can release the DSQ lock
 * between the iteration steps.
 *
 * Only tasks which were queued before @cursor was initialized are visible. This
 * bounds the iteration and guarantees that vtime never jumps in the other
 * direction while iterating.
 */
/* cursor 只遍历初始化前已入队任务；节点代际变化时停止而不追逐新任务。 */
/*
 * 上游契约：cursor 可在步骤间放开 DSQ 锁，但只看初始化前已排队 task，限制遍历
 * 工作量并避免 vtime 排序在遍历方向上反复跳变。
 * 入参：cursor 已由 INIT_DSQ_LIST_CURSOR 初始化；dsq 为已持锁 non-local DSQ。
 * 出参/返回：返回下一个借用 task 或 NULL；同时把 cursor 移到该 task 相邻位置。
 * 注意事项：priv 保存初始化代际，较新的 dsq_seq 必须跳过；返回后放锁会使 task 失效。
 */
static struct task_struct *nldsq_cursor_next_task(struct scx_dsq_list_node *cursor,
						  struct scx_dispatch_q *dsq)
{
	bool rev = cursor->flags & SCX_DSQ_ITER_REV;
	struct task_struct *p;

	lockdep_assert_held(&dsq->lock);
	BUG_ON(!(cursor->flags & SCX_DSQ_LNODE_ITER_CURSOR));

	/* 首次空游标从队头/尾定位；后续由 cursor 节点相邻位置推进。 */
	if (list_empty(&cursor->node))
		p = NULL;
	else
		p = container_of(cursor, struct task_struct, scx.dsq_list);

	/* skip cursors and tasks that were queued after @cursor init */
	/* qseq 晚于 cursor 快照的 task 是遍历开始后加入的，不能无限扩大本轮集合。 */
	do {
		p = nldsq_next_task(dsq, p, rev);
	} while (p && unlikely(u32_before(cursor->priv, p->scx.dsq_seq)));

	if (p) {
		/* cursor 跟随已返回 task，下一步即从它之后/之前继续。 */
		if (rev)
			list_move_tail(&cursor->node, &p->scx.dsq_list.node);
		else
			list_move(&cursor->node, &p->scx.dsq_list.node);
	} else {
		/* 到达边界后摘下 cursor 并初始化为空，后续调用稳定返回 NULL。 */
		list_del_init(&cursor->node);
	}

	return p;
}

/**
 * nldsq_cursor_lost_task - Test whether someone else took the task since iteration
 * @cursor: scx_dsq_list_node initialized with INIT_DSQ_LIST_CURSOR()
 * @rq: rq @p was on
 * @dsq: dsq @p was on
 * @p: target task
 *
 * @p is a task returned by nldsq_cursor_next_task(). The locks may have been
 * dropped and re-acquired inbetween. Verify that no one else took or is in the
 * process of taking @p from @dsq.
 *
 * On %false return, the caller can assume full ownership of @p.
 */
/* 通过 qseq 判断 cursor 是否失去任务；false 表示调用者取得该 task 的完整所有权。 */
/*
 * 上游契约：cursor 返回 task 后锁可能被释放，重取 rq+DSQ 锁后必须验证它没有被
 * 其他 consumer 取走；false 才表示调用者获得本次搬运的完整 custody。
 * 入参：cursor 带原快照代际；rq/dsq 均已持锁；p 是先前返回的借用 task。
 * 出参/返回：归属、代际、holding_cpu 或 rq 任一变化返回 true，否则 false。
 * 注意事项：false 只授予队列搬运权，不增加 task 引用；锁释放后仍受生命周期限制。
 */
static bool nldsq_cursor_lost_task(struct scx_dsq_list_node *cursor,
				   struct rq *rq, struct scx_dispatch_q *dsq,
				   struct task_struct *p)
{
	lockdep_assert_rq_held(rq);
	lockdep_assert_held(&dsq->lock);

	/*
	 * @p could have already left $src_dsq, got re-enqueud, or be in the
	 * process of being consumed by someone else.
	 */
	/* 三个条件分别捕获已离队/重入队新代际/正被另一 CPU consume 的竞态。 */
	if (unlikely(p->scx.dsq != dsq ||
		     u32_before(cursor->priv, p->scx.dsq_seq) ||
		     p->scx.holding_cpu >= 0))
		return true;

	/* if @p has stayed on @dsq, its rq couldn't have changed */
	/* DSQ 身份和代际都没变时 rq 也应不变；不变量破坏按 lost 处理而非继续冒险。 */
	if (WARN_ON_ONCE(rq != task_rq(p)))
		return true;

	return false;
}

/*
 * BPF DSQ iterator. Tasks in a non-local DSQ can be iterated in [reverse]
 * dispatch order. BPF-visible iterator is opaque and larger to allow future
 * changes without breaking backward compatibility. Can be used with
 * bpf_for_each(). See bpf_iter_scx_dsq_*().
 */
/*
 * 内核 iterator 持 cursor、借用 DSQ，以及 move helper 暂存的 slice/vtime；公开 ABI
 * 仅给同尺寸对齐 opaque 数组，允许未来改变内部布局而不破坏已编译 BPF 程序。
 */
struct bpf_iter_scx_dsq_kern {
	struct scx_dsq_list_node	cursor;
	struct scx_dispatch_q		*dsq;
	u64				slice;
	u64				vtime;
} __attribute__((aligned(8)));

struct bpf_iter_scx_dsq {
	u64				__opaque[6];
} __attribute__((aligned(8)));


/*
 * 业务背景：从复用的 scx.flags 中提取 task 初始化/启用生命周期状态。
 * 入参：p 为生命周期稳定的只读借用 task。
 * 出参/返回：返回 SCX_TASK_STATE_MASK 内的单一状态值；无副作用。
 * 注意事项：其他 flags 与状态共存，调用者不能直接把整个字段当枚举比较。
 */
static u32 scx_get_task_state(const struct task_struct *p)
{
	return p->scx.flags & SCX_TASK_STATE_MASK;
}

/*
 * 业务背景：集中执行 SCX task 生命周期转换并诊断非法边，防止 enable/disable/fork
 * 各路径静默形成无法回收的半初始化 task。
 * 入参：p 为控制路径独占或锁稳定的借用 task；state 为一个 SCX_TASK_* 状态。
 * 出参/返回：无直接返回；替换 flags 状态位，进入 INIT 时还请求重置 runnable_at。
 * 注意事项：WARN 只报告错误仍完成已知状态转换；未知值直接返回且保留旧状态。
 */
static void scx_set_task_state(struct task_struct *p, u32 state)
{
	u32 prev_state = scx_get_task_state(p);
	bool warn = false;

	switch (state) {
	case SCX_TASK_NONE:
		/* 回到未初始化态允许清理未完成事务，但 DEAD 不得复活为 NONE。 */
		warn = prev_state == SCX_TASK_DEAD;
		break;
	case SCX_TASK_INIT_BEGIN:
		/* 初始化事务只能从 NONE 开始，表示 BPF init_task 尚未成功提交。 */
		warn = prev_state != SCX_TASK_NONE;
		break;
	case SCX_TASK_INIT:
		/* BPF 初始化已成功，下一次 runnable 需重建时间戳，不能沿用进入 SCX 前的值。 */
		warn = prev_state != SCX_TASK_INIT_BEGIN;
		p->scx.flags |= SCX_TASK_RESET_RUNNABLE_AT;
		break;
	case SCX_TASK_READY:
		/* READY 既可接住首次 INIT，也可接住 disable 后仍保留可再次 enable 的实体。 */
		warn = !(prev_state == SCX_TASK_INIT ||
			 prev_state == SCX_TASK_ENABLED);
		break;
	case SCX_TASK_ENABLED:
		/* 只有完成 READY 准备的 task 才能发布给 BPF scheduler 热路径。 */
		warn = prev_state != SCX_TASK_READY;
		break;
	case SCX_TASK_DEAD:
		/* DEAD 只覆盖尚未完成 init 的退出 task；已启用 task 必须先走 disable/exit。 */
		warn = !(prev_state == SCX_TASK_NONE ||
			 prev_state == SCX_TASK_INIT_BEGIN);
		break;
	default:
		/* 非法状态机边只告警并保留原状态，防止诊断代码进一步破坏生命周期。 */
		WARN_ONCE(1, "sched_ext: Invalid task state %d -> %d for %s[%d]",
			  prev_state, state, p->comm, p->pid);
		return;
	}

	WARN_ONCE(warn, "sched_ext: Invalid task state transition 0x%x -> 0x%x for %s[%d]",
		  prev_state, state, p->comm, p->pid);

	p->scx.flags &= ~SCX_TASK_STATE_MASK;
	/* 只替换生命周期子字段，保留 QUEUEING、CURSOR 等并行协议位。 */
	p->scx.flags |= state;
}

/*
 * SCX task iterator.
 */
/*
 * SCX task iterator：cursor 嵌入全 task 链表；locked_task/rq/rf 描述当前取得的 rq
 * 锁与借用 task。cnt 用于批量放锁防止长时间关中断；sub-scheduler 配置下还可用
 * css iterator 枚举 cgroup 子树，并由外层 cgroup_lock 阻止 task 迁移。
 */
struct scx_task_iter {
	struct sched_ext_entity		cursor;
	struct task_struct		*locked_task;
	struct rq			*rq;
	struct rq_flags			rf;
	u32				cnt;
	/* list_locked 表示全局 task 链锁所有权，locked_task/rq 配对记录当前 rq 锁。 */
	bool				list_locked;
#ifdef CONFIG_EXT_SUB_SCHED
	struct cgroup			*cgrp;
	struct cgroup_subsys_state	*css_pos;
	struct css_task_iter		css_iter;
#endif
};

/**
 * scx_task_iter_start - Lock scx_tasks_lock and start a task iteration
 * @iter: iterator to init
 * @cgrp: Optional root of cgroup subhierarchy to iterate
 *
 * Initialize @iter. Once initialized, @iter must eventually be stopped with
 * scx_task_iter_stop().
 *
 * If @cgrp is %NULL, scx_tasks is used for iteration and this function returns
 * with scx_tasks_lock held and @iter->cursor inserted into scx_tasks.
 *
 * If @cgrp is not %NULL, @cgrp and its descendants' tasks are walked using
 * @iter->css_iter. The caller must be holding cgroup_lock() to prevent cgroup
 * task migrations.
 *
 * The two modes of iterations are largely independent and it's likely that
 * scx_tasks can be removed in favor of always using cgroup iteration if
 * CONFIG_SCHED_CLASS_EXT depends on CONFIG_CGROUPS.
 *
 * scx_tasks_lock and the rq lock may be released using scx_task_iter_unlock()
 * between this and the first next() call or between any two next() calls. If
 * the locks are released between two next() calls, the caller is responsible
 * for ensuring that the task being iterated remains accessible either through
 * RCU read lock or obtaining a reference count.
 *
 * All tasks which existed when the iteration started are guaranteed to be
 * visited as long as they are not dead.
 */
/* 建立全 task 或 cgroup 子树快照迭代；必须最终 stop 以归还锁和引用。 */
/*
 * 上游契约：初始化全 task 链表或 cgroup 子树迭代，所有起始时存在且尚未 DEAD 的
 * task 都会访问；任何成功 start 最终必须 stop。两次 next 间放锁时由调用者通过
 * RCU 或 task 引用维持刚返回对象。
 * 入参：iter 为调用者独占输出存储；cgrp 可空，非空时调用者持 cgroup_mutex。
 * 出参/返回：无直接返回；全局模式插入 cursor 并持 tasks_lock，cgroup 模式启动 css iterator。
 * 注意事项：初始化会清空整个 iter，不能对活动 iterator 重复 start。
 */
static void scx_task_iter_start(struct scx_task_iter *iter, struct cgroup *cgrp)
{
	memset(iter, 0, sizeof(*iter));

#ifdef CONFIG_EXT_SUB_SCHED
	if (cgrp) {
		/* cgroup_mutex 冻结迁移；先序逐 css 遍历把 root 与所有后代纳入集合。 */
		lockdep_assert_held(&cgroup_mutex);
		iter->cgrp = cgrp;
		iter->css_pos = css_next_descendant_pre(NULL, &iter->cgrp->self);
		css_task_iter_start(iter->css_pos, CSS_TASK_ITER_WITH_DEAD,
				    &iter->css_iter);
		return;
	}
#endif
	raw_spin_lock_irq(&scx_tasks_lock);

	/* cursor 是伪 sched_ext_entity，标志防止 next 把它 container_of 成 task。 */
	iter->cursor = (struct sched_ext_entity){ .flags = SCX_TASK_CURSOR };
	list_add(&iter->cursor.tasks_node, &scx_tasks);
	iter->list_locked = true;
}

/*
 * 业务背景：释放 iterator 可选持有的 rq lock，并先执行该 rq 已挂 balance callback。
 * 入参：iter 为活动 iterator；locked_task 为空时允许空操作。
 * 出参/返回：无；释放 rq，清空 locked_task，task 借用指针不再受 rq lock 稳定。
 * 注意事项：tasks_lock 状态不变，供 unlock/stop 组合调用。
 */
static void __scx_task_iter_rq_unlock(struct scx_task_iter *iter)
{
	if (iter->locked_task) {
		__balance_callbacks(iter->rq, &iter->rf);
		task_rq_unlock(iter->rq, iter->locked_task, &iter->rf);
		iter->locked_task = NULL;
	}
}

/**
 * scx_task_iter_unlock - Unlock rq and scx_tasks_lock held by a task iterator
 * @iter: iterator to unlock
 *
 * If @iter is in the middle of a locked iteration, it may be locking the rq of
 * the task currently being visited in addition to scx_tasks_lock. Unlock both.
 * This function can be safely called anytime during an iteration. The next
 * iterator operation will automatically restore the necessary locking.
 */
/*
 * 上游契约：可在迭代任意时刻临时释放 rq 与 tasks_lock，下一个操作会自动重取。
 * 入参：iter 为活动 iterator，可能持其中零到两把锁。
 * 出参/返回：无；清除持锁状态，不摘除 cursor，也不结束 css iterator。
 * 注意事项：放锁后刚返回 task 只能在 RCU/显式引用保护下继续使用。
 */
static void scx_task_iter_unlock(struct scx_task_iter *iter)
{
	__scx_task_iter_rq_unlock(iter);
	if (iter->list_locked) {
		iter->list_locked = false;
		raw_spin_unlock_irq(&scx_tasks_lock);
	}
}

/* 必要时重取 tasks_lock；list_locked 使重复调用幂等，返回时 IRQ 保持关闭。 */
static void __scx_task_iter_maybe_relock(struct scx_task_iter *iter)
{
	if (!iter->list_locked) {
		raw_spin_lock_irq(&scx_tasks_lock);
		iter->list_locked = true;
	}
}

/**
 * scx_task_iter_relock - Re-acquire scx_tasks_lock and, optionally, @p's rq
 * @iter: iterator to relock
 * @p: task whose rq to lock, or %NULL for scx_tasks_lock only
 *
 * Counterpart to scx_task_iter_unlock(). Locking @p's rq is optional. Once
 * re-acquired, both locks are managed by the iterator from here on.
 */
/*
 * 上游契约：作为 unlock 的逆操作，必定恢复 tasks_lock，并可额外锁住 p 当前 rq。
 * 入参：iter 为活动 iterator；p 可空，非空时是仍有稳定生命周期的借用 task。
 * 出参/返回：无；iterator 接管相应锁，后续 unlock/stop 负责释放。
 * 注意事项：task_rq_lock 会关闭/保存 IRQ 状态，调用者不得另行解同一 rq lock。
 */
static void scx_task_iter_relock(struct scx_task_iter *iter,
				 struct task_struct *p)
{
	__scx_task_iter_maybe_relock(iter);
	if (p) {
		iter->rq = task_rq_lock(p, &iter->rf);
		iter->locked_task = p;
	}
}

/**
 * scx_task_iter_stop - Stop a task iteration and unlock scx_tasks_lock
 * @iter: iterator to exit
 *
 * Exit a previously initialized @iter. Must be called with scx_tasks_lock held
 * which is released on return. If the iterator holds a task's rq lock, that rq
 * lock is also released. See scx_task_iter_start() for details.
 */
/*
 * 上游契约：结束 start 创建的 iterator，并释放 rq/tasks 锁或 css iterator 资源。
 * 入参：iter 为唯一活动实例；全局模式允许此前临时 unlock。
 * 出参/返回：无；cursor 被摘除，所有 iterator 锁和内部借用关系失效。
 * 注意事项：必须恰好调用一次；stop 后不能 next/relock，除非重新 start。
 */
static void scx_task_iter_stop(struct scx_task_iter *iter)
{
#ifdef CONFIG_EXT_SUB_SCHED
	if (iter->cgrp) {
		/* cgroup iterator 需先结束 css 子迭代，再释放可能持有的 task rq 锁。 */
		if (iter->css_pos)
			css_task_iter_end(&iter->css_iter);
		__scx_task_iter_rq_unlock(iter);
		return;
	}
#endif
	__scx_task_iter_maybe_relock(iter);
	list_del_init(&iter->cursor.tasks_node);
	scx_task_iter_unlock(iter);
}

/**
 * scx_task_iter_next - Next task
 * @iter: iterator to walk
 *
 * Visit the next task. See scx_task_iter_start() for details. Locks are dropped
 * and re-acquired every %SCX_TASK_ITER_BATCH iterations to avoid causing stalls
 * by holding scx_tasks_lock for too long.
 */
/*
 * 上游契约：返回下一 task；每 SCX_TASK_ITER_BATCH 次放锁并 cond_resched，避免控制
 * 路径长时间关闭 IRQ/阻塞 fork/free。
 * 入参：iter 为 start 后未 stop 的活动 iterator。
 * 出参/返回：返回未加引用的 task 借用指针，结束为 NULL；可能睡眠于 cond_resched。
 * 注意事项：内部放锁后 cursor/css 机制仍保证不漏掉起始时存在且未 DEAD 的 task。
 */
static struct task_struct *scx_task_iter_next(struct scx_task_iter *iter)
{
	struct list_head *cursor = &iter->cursor.tasks_node;
	struct sched_ext_entity *pos;

	if (!(++iter->cnt % SCX_TASK_ITER_BATCH)) {
		/* 周期性让出 CPU；调用者若跨此边界保存旧 task，须自行持 RCU/引用。 */
		scx_task_iter_unlock(iter);
		cond_resched();
	}

#ifdef CONFIG_EXT_SUB_SCHED
	if (iter->cgrp) {
		/* 当前 css 枚举完后结束它，再按先序启动下一个后代 css。 */
		while (iter->css_pos) {
			struct task_struct *p;

			p = css_task_iter_next(&iter->css_iter);
			if (p)
				return p;

			css_task_iter_end(&iter->css_iter);
			/* 当前 css 耗尽后以前序遍历进入下一后代，并建立新的 task iterator。 */
			iter->css_pos = css_next_descendant_pre(iter->css_pos,
								&iter->cgrp->self);
			if (iter->css_pos)
				css_task_iter_start(iter->css_pos, CSS_TASK_ITER_WITH_DEAD,
						    &iter->css_iter);
		}
		/* 目标 cgroup 子树已遍历完，不能回落到全局 scx_tasks 再重复返回其他 task。 */
		return NULL;
	}
#endif
	__scx_task_iter_maybe_relock(iter);

	list_for_each_entry(pos, cursor, tasks_node) {
		/* cursor 向已返回实体前方移动，临时放锁后新插入 task 不会破坏既有进度。 */
		if (&pos->tasks_node == &scx_tasks)
			return NULL;
		if (!(pos->flags & SCX_TASK_CURSOR)) {
			list_move(cursor, &pos->tasks_node);
			return container_of(pos, struct task_struct, scx);
		}
	}

	/* can't happen, should always terminate at scx_tasks above */
	/* 环形链表必然遇到 scx_tasks 表头；否则表示 cursor/list 不变量已损坏。 */
	BUG();
}

/**
 * scx_task_iter_next_locked - Next non-idle task with its rq locked
 * @iter: iterator to walk
 *
 * Visit the non-idle task with its rq lock held. Allows callers to specify
 * whether they would like to filter out dead tasks. See scx_task_iter_start()
 * for details.
 */
/* 返回时持 task 当前 rq 锁；批量阈值会周期性放锁，故每轮都重新验证 task。 */
/*
 * 上游契约：取得下一非 idle、SCX 视角未 DEAD 的 task，并在返回时持其当前 rq lock。
 * 入参：iter 为活动 iterator；进入时若锁着上一 task，本函数先释放它。
 * 出参/返回：成功返回 rq lock 稳定的借用 task，结束为 NULL；锁由 iterator 接管。
 * 注意事项：批量边界可能放锁，故每轮重取 task_rq_lock 后再检查 DEAD 状态。
 */
static struct task_struct *scx_task_iter_next_locked(struct scx_task_iter *iter)
{
	struct task_struct *p;

	__scx_task_iter_rq_unlock(iter);

	while ((p = scx_task_iter_next(iter))) {
		/*
		 * scx_task_iter is used to prepare and move tasks into SCX
		 * while loading the BPF scheduler and vice-versa while
		 * unloading. The init_tasks ("swappers") should be excluded
		 * from the iteration because:
		 *
		 * - It's unsafe to use __setschduler_prio() on an init_task to
		 *   determine the sched_class to use as it won't preserve its
		 *   idle_sched_class.
		 *
		 * - ops.init/exit_task() can easily be confused if called with
		 *   init_tasks as they, e.g., share PID 0.
		 *
		 * As init_tasks are never scheduled through SCX, they can be
		 * skipped safely. Note that is_idle_task() which tests %PF_IDLE
		 * doesn't work here:
		 *
		 * - %PF_IDLE may not be set for an init_task whose CPU hasn't
		 *   yet been onlined.
		 *
		 * - %PF_IDLE can be set on tasks that are not init_tasks. See
		 *   play_idle_precise() used by CONFIG_IDLE_INJECT.
		 *
		 * Test for idle_sched_class as only init_tasks are on it.
		 */
		/*
		 * 上游解释了为何排除 per-CPU swapper：通用 class 重算会破坏 idle class，BPF
		 * 也会被共享 PID 0 迷惑；PF_IDLE 又覆盖不全并可能标到 idle injection task，
		 * 因而以只有 init_task 使用的 idle_sched_class 作精确判据。
		 */
		if (p->sched_class == &idle_sched_class)
			continue;

		iter->rq = task_rq_lock(p, &iter->rf);
		/* 取得 rq 后 task 的 class、状态与 rq 归属才在本轮检查中稳定。 */
		iter->locked_task = p;

		/*
		 * cgroup_task_dead() removes the dead tasks from cset->tasks
		 * after sched_ext_dead() and cgroup iteration may see tasks
		 * which already finished sched_ext_dead(). %SCX_TASK_DEAD is
		 * set by sched_ext_dead() under @p's rq lock. Test it to
		 * avoid visiting tasks which are already dead from SCX POV.
		 */
		/* cgroup 链摘除晚于 sched_ext_dead，故 css iterator 可能看到已完成 SCX 清理的 task。 */
		if (scx_get_task_state(p) == SCX_TASK_DEAD) {
			__scx_task_iter_rq_unlock(iter);
			continue;
		}

		return p;
	}
	return NULL;
}

/**
 * scx_add_event - Increase an event counter for 'name' by 'cnt'
 * @sch: scx_sched to account events for
 * @name: an event name defined in struct scx_event_stats
 * @cnt: the number of the event occurred
 *
 * This can be used when preemption is not disabled.
 */
/* 普通版本在可能迁移上下文安全更新当前 CPU 计数，并同步发出同名 tracepoint。 */
#define scx_add_event(sch, name, cnt) do {					\
	this_cpu_add((sch)->pcpu->event_stats.name, (cnt));			\
	trace_sched_ext_event(#name, (cnt));					\
} while(0)

/*
 * scx_add_event 可在允许抢占的路径使用：this_cpu_add 自行保护本次 per-CPU 更新，并
 * 同步发 tracepoint。name 必须是 scx_event_stats 字段，cnt 是本次事件数量。
 */

/**
 * __scx_add_event - Increase an event counter for 'name' by 'cnt'
 * @sch: scx_sched to account events for
 * @name: an event name defined in struct scx_event_stats
 * @cnt: the number of the event occurred
 *
 * This should be used only when preemption is disabled.
 */
/* __scx_add_event 省略抢占保护，只能在调用者已禁抢占时使用，否则可能写错 CPU 槽。 */
#define __scx_add_event(sch, name, cnt) do {					\
	__this_cpu_add((sch)->pcpu->event_stats.name, (cnt));			\
	trace_sched_ext_event(#name, cnt);					\
} while(0)

/* __scx_add_event 省略抢占保护，只能在调用者已禁抢占时使用，否则可能写错 CPU 槽。 */

/**
 * scx_agg_event - Aggregate an event counter 'kind' from 'src_e' to 'dst_e'
 * @dst_e: destination event stats
 * @src_e: source event stats
 * @kind: a kind of event to be aggregated
 */
/* 聚合允许近似快照：READ_ONCE 防编译器撕裂/重复读取，但不冻结并发计数更新。 */
#define scx_agg_event(dst_e, src_e, kind) do {					\
	(dst_e)->kind += READ_ONCE((src_e)->kind);				\
} while(0)

/* 聚合允许近似快照：READ_ONCE 防编译器撕裂/重复读取，但不冻结并发计数更新。 */

/**
 * scx_dump_event - Dump an event 'kind' in 'events' to 's'
 * @s: output seq_buf
 * @events: event stats
 * @kind: a kind of event to dump
 */
/* dump 宏把字段名和值输出到 seq_buf；输出失败/截断状态由 dump_line/seq_buf 累积。 */
#define scx_dump_event(s, events, kind) do {					\
	dump_line(&(s), "%40s: %16lld", #kind, (events)->kind);			\
} while (0)

/* dump 宏把字段名和值输出到 seq_buf；输出失败/截断状态由 dump_line/seq_buf 累积。 */


static void scx_read_events(struct scx_sched *sch,
			    struct scx_event_stats *events);

/*
 * 业务背景：无锁读取全局 SCX enable 状态供 sysfs 与控制路径决策。
 * 入参：无。
 * 出参/返回：返回某一 SCX_* 状态的原子快照；无副作用。
 * 注意事项：快照不稳定 scheduler 对象，需跨步骤一致性时仍持 enable_mutex。
 */
static enum scx_enable_state scx_enable_state(void)
{
	return atomic_read(&scx_enable_state_var);
}

/* 原子交换全局启用状态，返回旧值供严格状态机检查。 */
/*
 * 业务背景：以单一原子提交点切换 enable 状态并取得旧值做状态机断言。
 * 入参：to 为目标 SCX_* 状态。
 * 出参/返回：返回交换前状态；全局状态无条件变为 to。
 * 注意事项：调用者通常持 enable_mutex；atomic_xchg 不替代实例资源的发布顺序协议。
 */
static enum scx_enable_state scx_set_enable_state(enum scx_enable_state to)
{
	return atomic_xchg(&scx_enable_state_var, to);
}

/*
 * 业务背景：仅当状态仍等于 from 时提交 to，处理异步退出与 enable 的竞争。
 * 入参：to/from 分别为目标和期望旧状态。
 * 出参/返回：比较交换成功为 true；失败保持状态不变并返回 false。
 * 注意事项：失败说明并发路径已经推进状态，调用者不得覆盖其结果。
 */
static bool scx_tryset_enable_state(enum scx_enable_state to,
				    enum scx_enable_state from)
{
	int from_v = from;

	return atomic_try_cmpxchg(&scx_enable_state_var, &from_v, to);
}

/**
 * wait_ops_state - Busy-wait the specified ops state to end
 * @p: target task
 * @opss: state to wait the end of
 *
 * Busy-wait for @p to transition out of @opss. This can only be used when the
 * state part of @opss is %SCX_QUEUEING or %SCX_DISPATCHING. This function also
 * has load_acquire semantics to ensure that the caller can see the updates made
 * in the enqueueing and dispatching paths.
 */
/* 等待 task 离开指定 ops_state；只能在允许忙等且 task 生命周期稳定的路径使用。 */
/*
 * 上游契约：只等待 QUEUEING/DISPATCHING 状态结束；acquire 读取与生产路径 release
 * 发布配对，使返回后的调用者看到 enqueue/dispatch 对 task 字段的全部更新。
 * 入参：p 为生命周期稳定的借用 task；opss 是包含 qseq 的完整期望状态字。
 * 出参/返回：无；目标值改变才返回，不取得引用也不睡眠。
 * 注意事项：这是 cpu_relax 忙等，只能用于短暂中间态，不能等待任意 BPF 活动。
 */
static void wait_ops_state(struct task_struct *p, unsigned long opss)
{
	do {
		cpu_relax();
	} while (atomic_long_read_acquire(&p->scx.ops_state) == opss);
}

/* 内部纯检查：CPU 编号非负、低于 nr_cpu_ids 且属于 possible mask 才可索引 per-CPU 数据。 */
static inline bool __cpu_valid(s32 cpu)
{
	return likely(cpu >= 0 && cpu < nr_cpu_ids && cpu_possible(cpu));
}

/**
 * scx_cpu_valid - Verify a cpu number, to be used on ops input args
 * @sch: scx_sched to abort on error
 * @cpu: cpu number which came from a BPF ops
 * @where: extra information reported on error
 *
 * @cpu is a cpu number which came from the BPF scheduler and can be any value.
 * Verify that it is in range and one of the possible cpus. If invalid, trigger
 * an ops error.
 */
/*
 * 上游契约：验证来自不可信 BPF 的 CPU 编号，非法时触发 scheduler error。
 * 入参：sch 为错误归属 scheduler；cpu 可为任意 s32；where 可空诊断后缀且只借用。
 * 出参/返回：possible CPU 返回 true；否则记录错误并返回 false。
 * 注意事项：错误会异步引导 disable，调用者仍必须立即停止使用该 CPU 做数组索引。
 */
bool scx_cpu_valid(struct scx_sched *sch, s32 cpu, const char *where)
{
	if (__cpu_valid(cpu)) {
		return true;
	} else {
		scx_error(sch, "invalid CPU %d%s%s", cpu, where ? " " : "", where ?: "");
		return false;
	}
}

/**
 * ops_sanitize_err - Sanitize a -errno value
 * @sch: scx_sched to error out on error
 * @ops_name: operation to blame on failure
 * @err: -errno value to sanitize
 *
 * Verify @err is a valid -errno. If not, trigger scx_error() and return
 * -%EPROTO. This is necessary because returning a rogue -errno up the chain can
 * cause misbehaviors. For an example, a large negative return from
 * ops.init_task() triggers an oops when passed up the call chain because the
 * value fails IS_ERR() test after being encoded with ERR_PTR() and then is
 * handled as a pointer.
 */
/* BPF 只能返回合法负 errno；异常返回触发 scheduler error 并规范为安全错误。 */
/*
 * 上游契约：只接受 [-MAX_ERRNO,-1]；其他 BPF 返回会触发 scheduler error 并规范为
 * -EPROTO，防止 ERR_PTR 编码后逃过 IS_ERR 而被误当真实指针。
 * 入参：sch 为错误归属 scheduler；ops_name 为借用诊断名；err 是不可信 BPF s32。
 * 出参/返回：合法负 errno 原样返回，否则 -EPROTO；非法时还登记退出副作用。
 * 注意事项：0 也不是此 helper 的合法输入，成功分支应在调用前单独处理。
 */
static int ops_sanitize_err(struct scx_sched *sch, const char *ops_name, s32 err)
{
	if (err < 0 && err >= -MAX_ERRNO)
		return err;

	scx_error(sch, "ops.%s() returned an invalid errno %d", ops_name, err);
	return -EPROTO;
}

/* balance callback 薄包装：rq 已持锁但未 pin，run_deferred 可临时放锁迁移 task。 */
static void deferred_bal_cb_workfn(struct rq *rq)
{
	run_deferred(rq);
}

/*
 * 业务背景：没有可复用 scheduler hook 时在目标 CPU irq_work 中执行延期动作。
 * 入参：irq_work 嵌入 rq->scx，生命周期随静态 rq。
 * 出参/返回：无；container_of 恢复 rq，持 rq lock 调 run_deferred 后释放。
 * 注意事项：不可睡眠；run_deferred 负责 unpin/切锁协议而非在裸锁状态任意迁移。
 */
static void deferred_irq_workfn(struct irq_work *irq_work)
{
	struct rq *rq = container_of(irq_work, struct rq, scx.deferred_irq_work);

	raw_spin_rq_lock(rq);
	run_deferred(rq);
	raw_spin_rq_unlock(rq);
}

/**
 * schedule_deferred - Schedule execution of deferred actions on an rq
 * @rq: target rq
 *
 * Schedule execution of deferred actions on @rq. Deferred actions are executed
 * with @rq locked but unpinned, and thus can unlock @rq to e.g. migrate tasks
 * to other rqs.
 */
/* 无 rq 锁上下文以 irq_work 安排延迟动作，pending 位避免重复投递。 */
/*
 * 上游契约：把动作排到 rq 所属 CPU，回调持锁但未 pin，可为迁移临时放锁。
 * 入参：rq 为静态 per-CPU 队列借用指针，调用者当前不要求持锁。
 * 出参/返回：无；异步投递 irq_work，实际状态修改稍后发生。
 * 注意事项：远端投递避免等待发起 CPU 开中断，irq_work 自身合并重复 pending。
 */
static void schedule_deferred(struct rq *rq)
{
	/*
	 * This is the fallback when schedule_deferred_locked() can't use
	 * the cheaper balance callback or wakeup hook paths (the target
	 * CPU is not in balance or wakeup). Currently, this is primarily
	 * hit by reenqueue operations targeting a remote CPU.
	 *
	 * Queue on the target CPU. The deferred work can run from any CPU
	 * correctly - the _locked() path already processes remote rqs from
	 * the calling CPU - but targeting the owning CPU allows IPI delivery
	 * without waiting for the calling CPU to re-enable IRQs and is
	 * cheaper as the reenqueue runs locally.
	 */
	/* 优先目标 CPU 既降低远端锁开销，也能在发起 CPU 尚未开 IRQ 时先送出 IPI。 */
	irq_work_queue_on(&rq->scx.deferred_irq_work, cpu_of(rq));
}

/**
 * schedule_deferred_locked - Schedule execution of deferred actions on an rq
 * @rq: target rq
 *
 * Schedule execution of deferred actions on @rq. Equivalent to
 * schedule_deferred() but requires @rq to be locked and can be more efficient.
 */
/* rq 锁内优先挂 balance_callback，在解锁边界执行以缩短热路径。 */
/*
 * 上游契约：持 rq lock 安排与 schedule_deferred 等价的动作，优先复用 wakeup 或
 * balance 的既有解锁边界，只有无 hook 时才投 irq_work。
 * 入参：rq 为已持锁借用队列。
 * 出参/返回：无；可能只置 pending 位，也可能异步投递工作。
 * 注意事项：不能直接挂 callback 节点，因为后续临时放锁会把半准备节点暴露给 rq_pin_lock。
 */
static void schedule_deferred_locked(struct rq *rq)
{
	lockdep_assert_rq_held(rq);

	/*
	 * If in the middle of waking up a task, task_woken_scx() will be called
	 * afterwards which will then run the deferred actions, no need to
	 * schedule anything.
	 */
/* task_woken_scx 会在同一唤醒事务末尾运行 deferred，无需重复投递。 */
	if (rq->scx.flags & SCX_RQ_IN_WAKEUP)
		/* task_woken_scx 会在同一唤醒事务末尾运行 deferred，无需重复投递。 */
		return;

	/* Don't do anything if there already is a deferred operation. */
/* 已有请求覆盖本轮动作，pending 位承担合并语义。 */
	if (rq->scx.flags & SCX_RQ_BAL_CB_PENDING)
		/* 已有请求覆盖本轮动作，pending 位承担合并语义。 */
		return;

	/*
	 * If in balance, the balance callbacks will be called before rq lock is
	 * released. Schedule one.
	 *
	 *
	 * We can't directly insert the callback into the
	 * rq's list: The call can drop its lock and make the pending balance
	 * callback visible to unrelated code paths that call rq_pin_lock().
	 *
	 * Just let balance_one() know that it must do it itself.
	 */
/* 只通知 balance_one 自己安装/运行 callback，避免跨临时解锁暴露链表节点。 */
	if (rq->scx.flags & SCX_RQ_IN_BALANCE) {
		/* 只通知 balance_one 自己安装/运行 callback，避免跨临时解锁暴露链表节点。 */
		rq->scx.flags |= SCX_RQ_BAL_CB_PENDING;
		return;
	}

	/*
	 * No scheduler hooks available. Use the generic irq_work path. The
	 * above WAKEUP and BALANCE paths should cover most of the cases and the
	 * time to IRQ re-enable shouldn't be long.
	 */
/*
 * 业务背景：合并 DSQ re-enqueue 请求并把实际搬运延后到目标 rq 的安全 hook；BPF
 * kfunc 因而无需在当前锁组合下同步扫描/迁移整条队列。
 * 入参：sch/dsq 为稳定借用对象；reenq_flags 为待合并策略位；locked_rq 可空，表示
 * 调用者当前已持哪条 rq lock。
 * 出参/返回：无；local/user DSQ 记录 per-CPU deferred 节点，非法 builtin DSQ 触发退出。
 * 注意事项：bypass/dead scheduler 拒绝新请求；内存屏障与 consumer 屏障防止丢失合并。
 */
	schedule_deferred(rq);
}

/*
 * 业务背景：合并 DSQ re-enqueue 请求并把实际搬运延后到目标 rq 的安全 hook；BPF
 * kfunc 因而无需在当前锁组合下同步扫描/迁移整条队列。
 * 入参：sch/dsq 为稳定借用对象；reenq_flags 为待合并策略位；locked_rq 可空，表示
 * 调用者当前已持哪条 rq lock。
 * 出参/返回：无；local/user DSQ 记录 per-CPU deferred 节点，非法 builtin DSQ 触发退出。
 * 注意事项：bypass/dead scheduler 拒绝新请求；内存屏障与 consumer 屏障防止丢失合并。
 */
static void schedule_dsq_reenq(struct scx_sched *sch, struct scx_dispatch_q *dsq,
			       u64 reenq_flags, struct rq *locked_rq)
{
	struct rq *rq;

	/*
	 * Allowing reenqueues doesn't make sense while bypassing. This also
	 * blocks from new reenqueues to be scheduled on dead scheds.
	 */
/* bypass 已由内核接管队列，继续接受 BPF reenq 既无意义也可能访问 dead ops。 */
	if (unlikely(READ_ONCE(sch->bypass_depth)))
		/* bypass 已由内核接管队列，继续接受 BPF reenq 既无意义也可能访问 dead ops。 */
		return;

	if (dsq->id == SCX_DSQ_LOCAL) {
		rq = container_of(dsq, struct rq, scx.local_dsq);

		struct scx_sched_pcpu *sch_pcpu = per_cpu_ptr(sch->pcpu, cpu_of(rq));
		struct scx_deferred_reenq_local *drl = &sch_pcpu->deferred_reenq_local;

		/*
		 * Pairs with smp_mb() in process_deferred_reenq_locals() and
		 * guarantees that there is a reenq_local() afterwards.
		 */
		/* 与 consumer 的全屏障形成“看到 node/flags 或 consumer 已经过扫描点”二选一。 */
		smp_mb();

		if (list_empty(&drl->node) ||
		    (READ_ONCE(drl->flags) & reenq_flags) != reenq_flags) {

			guard(raw_spinlock_irqsave)(&rq->scx.deferred_reenq_lock);

			if (list_empty(&drl->node))
				list_move_tail(&drl->node, &rq->scx.deferred_reenq_locals);
			WRITE_ONCE(drl->flags, drl->flags | reenq_flags);
		}
	} else if (!(dsq->id & SCX_DSQ_FLAG_BUILTIN)) {
		/* user DSQ 没有固定 rq，按当前 CPU 槽记录，再由本 rq 延后处理该队列。 */
		rq = this_rq();

		struct scx_dsq_pcpu *dsq_pcpu = per_cpu_ptr(dsq->pcpu, cpu_of(rq));
		struct scx_deferred_reenq_user *dru = &dsq_pcpu->deferred_reenq_user;

		/*
		 * Pairs with smp_mb() in process_deferred_reenq_users() and
		 * guarantees that there is a reenq_user() afterwards.
		 */
/* 屏障后再检查/合并请求，确保消费者不会漏掉刚发布的 user reenq。 */
		smp_mb();

		/* 屏障后再检查/合并请求，确保消费者不会漏掉刚发布的 user reenq。 */
		if (list_empty(&dru->node) ||
		    (READ_ONCE(dru->flags) & reenq_flags) != reenq_flags) {

			guard(raw_spinlock_irqsave)(&rq->scx.deferred_reenq_lock);

			if (list_empty(&dru->node))
				list_move_tail(&dru->node, &rq->scx.deferred_reenq_users);
			WRITE_ONCE(dru->flags, dru->flags | reenq_flags);
		}
	} else {
		/* global 等 builtin DSQ 不支持该接口；把不可信请求转成 scheduler error。 */
		scx_error(sch, "DSQ 0x%llx not allowed for reenq", dsq->id);
		return;
	}

	if (rq == locked_rq)
		schedule_deferred_locked(rq);
	else
		schedule_deferred(rq);
}

/* root local-DSQ 便捷入口；RCU 调度读侧借用 root，未发布 root 只告警并返回。 */
static void schedule_reenq_local(struct rq *rq, u64 reenq_flags)
{
	struct scx_sched *root = rcu_dereference_sched(scx_root);

	if (WARN_ON_ONCE(!root))
		return;

	schedule_dsq_reenq(root, &rq->scx.local_dsq, reenq_flags, rq);
}

/**
 * touch_core_sched - Update timestamp used for core-sched task ordering
 * @rq: rq to read clock from, must be locked
 * @p: task to update the timestamp for
 *
 * Update @p->scx.core_sched_at timestamp. This is used by scx_prio_less() to
 * implement global or local-DSQ FIFO ordering for core-sched. Should be called
 * when a task becomes runnable and its turn on the CPU ends (e.g. slice
 * exhaustion).
 */
/*
 * 上游契约：task runnable 或 slice 用尽时刷新 core-sched FIFO 时间戳，供 SMT sibling
 * 间的 scx_prio_less 排序。
 * 入参：rq 为已持锁时钟来源；p 为借用 task。
 * 出参/返回：无；启用 SCHED_CORE 时可能写 p->scx.core_sched_at。
 * 注意事项：允许多写；sched_clock_cpu 提供跨 sibling 可比较值，但未来可换 per-core 代际。
 */
static void touch_core_sched(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);

#ifdef CONFIG_SCHED_CORE
	/*
	 * It's okay to update the timestamp spuriously. Use
	 * sched_core_disabled() which is cheaper than enabled().
	 *
	 * As this is used to determine ordering between tasks of sibling CPUs,
	 * it may be better to use per-core dispatch sequence instead.
	 */
	/* 额外刷新只影响同序 FIFO 次序，不破坏正确性；关闭 core scheduling 时完全跳过。 */
	if (!sched_core_disabled())
		p->scx.core_sched_at = sched_clock_cpu(cpu_of(rq));
#endif
}

/**
 * touch_core_sched_dispatch - Update core-sched timestamp on dispatch
 * @rq: rq to read clock from, must be locked
 * @p: task being dispatched
 *
 * If the BPF scheduler implements custom core-sched ordering via
 * ops.core_sched_before(), @p->scx.core_sched_at is used to implement FIFO
 * ordering within each local DSQ. This function is called from dispatch paths
 * and updates @p->scx.core_sched_at if custom core-sched ordering is in effect.
 */
/*
 * 上游契约：仅当 BPF 实现 core_sched_before 时，在 dispatch 点刷新 local-DSQ FIFO
 * 时间；默认 core-sched 排序无需额外写热字段。
 * 入参：rq 已锁；p 为正被 dispatch 的借用 task。
 * 出参/返回：无；配置/ops 不满足时为空操作。
 * 注意事项：读取 root ops 依赖当前 SCX/rq 上下文稳定 scheduler。
 */
static void touch_core_sched_dispatch(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);

#ifdef CONFIG_SCHED_CORE
	if (unlikely(SCX_HAS_OP(scx_root, core_sched_before)))
		touch_core_sched(rq, p);
#endif
}

/* 结算当前 SCX task 执行时间并扣 slice，同时把 runtime 传播给保护它的 DL server。 */
/*
 * 业务背景：在 tick、切换等记账点结算 curr 的真实执行量，有限 slice 归零后更新
 * core-sched 次序，同时从 ext deadline server 的预算扣除同一 runtime。
 * 入参：rq 为当前 CPU 已持锁运行队列，rq->curr 生命周期稳定。
 * 出参/返回：无；更新通用 exec 统计、SCX slice/core 时间戳与 DL server 预算。
 * 注意事项：delta<=0 不重复扣账；SCX_SLICE_INF 永不递减，但 server 仍需结算。
 */
static void update_curr_scx(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	s64 delta_exec;

	delta_exec = update_curr_common(rq);
	/* 通用 helper 返回自上次结算后的纳秒数，并同步 task/rq 执行统计。 */
	if (unlikely(delta_exec <= 0))
		return;

	if (curr->scx.slice != SCX_SLICE_INF) {
		/* min 防止无符号下溢；只有首次归零需要刷新公平次序。 */
		curr->scx.slice -= min_t(u64, curr->scx.slice, delta_exec);
		if (!curr->scx.slice)
			touch_core_sched(rq, curr);
	}

	dl_server_update(&rq->ext_server, delta_exec);
}

/*
 * 业务背景：为 DSQ priority tree 比较 task 的虚拟时间，较早 vtime 排在前面。
 * 入参：node_a/node_b 均嵌入仍在 DSQ 的 task；比较器只读。
 * 出参/返回：a 在回绕安全 64 位时间序上早于 b 时为 true。
 * 注意事项：相等值的稳定次序由 rb-tree 插入规则决定，本函数不修改节点。
 */
static bool scx_dsq_priq_less(struct rb_node *node_a,
			      const struct rb_node *node_b)
{
	const struct task_struct *a =
		container_of(node_a, struct task_struct, scx.dsq_priq);
	const struct task_struct *b =
		container_of(node_b, struct task_struct, scx.dsq_priq);

	return time_before64(a->scx.dsq_vtime, b->scx.dsq_vtime);
}

/* DSQ 入队计数与 scheduler/rq 聚合在对应锁域内同步增加。 */
/*
 * 业务背景：task 链入 DSQ 后同步增加队列及层级 runnable 计数，并持久化 IMMED
 * 保护，保证 SAVE/RESTORE 或 slice 延长期间仍遵守立即派发承诺。
 * 入参：dsq/p 为当前锁域内借用对象；enq_flags 描述本次插入语义。
 * 出参/返回：无；更新 nr、task flags 及 scheduler/rq 聚合计数。
 * 注意事项：nr 被无锁 kfunc 读取故 WRITE_ONCE；IMMED 只允许 local DSQ。
 */
static void dsq_inc_nr(struct scx_dispatch_q *dsq, struct task_struct *p, u64 enq_flags)
{
	/* scx_bpf_dsq_nr_queued() reads ->nr without locking, use WRITE_ONCE() */
	/* 无锁读只承诺单次一致快照，不承诺与随后队列遍历处于同一时刻。 */
	WRITE_ONCE(dsq->nr, dsq->nr + 1);

	/*
	 * Once @p reaches a local DSQ, it can only leave it by being dispatched
	 * to the CPU or dequeued. In both cases, the only way @p can go back to
	 * the BPF sched is through enqueueing. If being inserted into a local
	 * DSQ with IMMED, persist the state until the next enqueueing event in
	 * do_enqueue_task() so that we can maintain IMMED protection through
	 * e.g. SAVE/RESTORE cycles and slice extensions.
	 */
	/* task 到达 local DSQ 后只会因上 CPU 或 dequeue 离开，两条路径都持 rq 锁，因此 nr 更新无需额外 DSQ 锁。 */
	if (enq_flags & SCX_ENQ_IMMED) {
		if (unlikely(dsq->id != SCX_DSQ_LOCAL)) {
			WARN_ON_ONCE(!(enq_flags & SCX_ENQ_GDSQ_FALLBACK));
			return;
		}
		p->scx.flags |= SCX_TASK_IMMED;
	}

	if (p->scx.flags & SCX_TASK_IMMED) {
		struct rq *rq = container_of(dsq, struct rq, scx.local_dsq);

		/* IMMED 只能指向 local DSQ，随后直接要求该 rq 重调度而不等待 slice 耗尽。 */
		if (WARN_ON_ONCE(dsq->id != SCX_DSQ_LOCAL))
			return;

		rq->scx.nr_immed++;

		/*
		 * If @rq already had other tasks or the current task is not
		 * done yet, @p can't go on the CPU immediately. Re-enqueue.
		 */
/*
 * 业务背景：task 离开 DSQ 时与 dsq_inc_nr() 对称撤销队列和 IMMED 计数。
 * 入参：dsq 为已持相应锁的队列；p 仍带本次队列状态的借用 task。
 * 出参/返回：无；nr 减一，IMMED task 还递减 local rq 的 nr_immed。
 * 注意事项：无锁读者通过 WRITE_ONCE 得到单次快照；计数下溢或非 local IMMED 会告警。
 */
		if (unlikely(dsq->nr > 1 || !rq_is_open(rq, enq_flags)))
			schedule_reenq_local(rq, 0);
	}
}

/*
 * 业务背景：task 离开 DSQ 时与 dsq_inc_nr() 对称撤销队列和 IMMED 计数。
 * 入参：dsq 为已持相应锁的队列；p 仍带本次队列状态的借用 task。
 * 出参/返回：无；nr 减一，IMMED task 还递减 local rq 的 nr_immed。
 * 注意事项：无锁读者通过 WRITE_ONCE 得到单次快照；计数下溢或非 local IMMED 会告警。
 */
static void dsq_dec_nr(struct scx_dispatch_q *dsq, struct task_struct *p)
{
	/* see dsq_inc_nr() */
	/* 与入队侧严格成对，先更新 DSQ 总数，再处理只属于 local DSQ 的 IMMED 子计数。 */
	WRITE_ONCE(dsq->nr, dsq->nr - 1);

	if (p->scx.flags & SCX_TASK_IMMED) {
		struct rq *rq = container_of(dsq, struct rq, scx.local_dsq);

		if (WARN_ON_ONCE(dsq->id != SCX_DSQ_LOCAL) ||
		    WARN_ON_ONCE(rq->scx.nr_immed <= 0))
			return;

		rq->scx.nr_immed--;
	}
}

/*
 * 业务背景：BPF 未提供可继续使用的 slice 时，以 scheduler 默认纳秒时间片恢复前进。
 * 入参：sch 为 task 所属 scheduler；p 为当前锁域内借用 task。
 * 出参/返回：无；写 p->scx.slice 并增加 REFILL_SLICE_DFL 事件计数。
 * 注意事项：READ_ONCE 只取得配置快照，允许控制路径并发更新后在下一次 refill 生效。
 */
static void refill_task_slice_dfl(struct scx_sched *sch, struct task_struct *p)
{
	p->scx.slice = READ_ONCE(sch->slice_dfl);
	__scx_add_event(sch, SCX_EV_REFILL_SLICE_DFL, 1);
}

/*
 * Return true if @p is moving due to an internal SCX migration, false
 * otherwise.
 */
/* p 正处于 SCX 内部迁移时返回 true，否则 false；该判定区分内核搬运与普通调度 dequeue/enqueue。 */
static inline bool task_scx_migrating(struct task_struct *p)
{
	/*
	 * We only need to check sticky_cpu: it is set to the destination
	 * CPU in move_remote_task_to_local_dsq() before deactivate_task()
	 * and cleared when the task is enqueued on the destination, so it
	 * is only non-negative during an internal SCX migration.
	 */
	/*
	 * 上游说明：内部迁移在 deactivate 前把 sticky_cpu 设为目标 CPU，到目标 enqueue
	 * 才清零；因此非负窗口精确覆盖“暂时离开普通 BPF custody”的迁移事务。
	 */
	return p->scx.sticky_cpu >= 0;
}

/*
 * Call ops.dequeue() if the task is in BPF custody and not migrating.
 * Clears %SCX_TASK_IN_CUSTODY when the callback is invoked.
 */
/*
 * 上游契约：仅当 task 在 BPF custody 且不是内核内部迁移时调用 ops.dequeue，回调
 * 后无论 BPF 是否实现该 op 都清 IN_CUSTODY，形成一次且仅一次的交还边界。
 * 入参：sch 为所属 scheduler；rq 已按调用点锁定；p 为借用 task；deq_flags 描述原因。
 * 出参/返回：无；可能执行 BPF 回调并清 custody 标志。
 * 注意事项：迁移跳过回调，避免 BPF 把一次内部换 CPU 误判为 runnable 生命周期结束。
 */
static void call_task_dequeue(struct scx_sched *sch, struct rq *rq,
			      struct task_struct *p, u64 deq_flags)
{
	if (!(p->scx.flags & SCX_TASK_IN_CUSTODY) || task_scx_migrating(p))
		return;

	if (SCX_HAS_OP(sch, dequeue))
		SCX_CALL_OP_TASK(sch, dequeue, rq, p, deq_flags);

	p->scx.flags &= ~SCX_TASK_IN_CUSTODY;
}

/*
 * 业务背景：task 链入 local DSQ 后完成 custody 交还、class 可见性和 PREEMPT 语义；
 * local 入队并不保证立刻运行，因为 dispatch 过程中可能曾放 rq lock 让 RT task 唤醒。
 * 入参：sch/dsq/p 为 rq lock 稳定的借用对象；enq_flags 为本次 dispatch 标志。
 * 出参/返回：无；可能调用 ops.dequeue、提高 next_class、清 curr slice 并请求 resched。
 * 注意事项：balance 中 CPU 已在选 task，无需重复抢占；IMMED 的重入队由计数路径保证。
 */
static void local_dsq_post_enq(struct scx_sched *sch, struct scx_dispatch_q *dsq,
			       struct task_struct *p, u64 enq_flags)
{
	struct rq *rq = container_of(dsq, struct rq, scx.local_dsq);

	call_task_dequeue(sch, rq, p, 0);

	/*
	 * Note that @rq's lock may be dropped between this enqueue and @p
	 * actually getting on CPU. This gives higher-class tasks (e.g. RT)
	 * an opportunity to wake up on @rq and prevent @p from running.
	 * Here are some concrete examples:
	 *
	 * Example 1:
	 *
	 * We dispatch two tasks from a single ops.dispatch():
	 * - First, a local task to this CPU's local DSQ;
	 * - Second, a local/remote task to a remote CPU's local DSQ.
	 * We must drop the local rq lock in order to finish the second
	 * dispatch. In that time, an RT task can wake up on the local rq.
	 *
	 * Example 2:
	 *
	 * We dispatch a local/remote task to a remote CPU's local DSQ.
	 * We must drop the remote rq lock before the dispatched task can run,
	 * which gives an RT task an opportunity to wake up on the remote rq.
	 *
	 * Both examples work the same if we replace dispatching with moving
	 * the tasks from a user-created DSQ.
	 *
	 * We must detect these wakeups so that we can re-enqueue IMMED tasks
	 * from @rq's local DSQ. scx_wakeup_preempt() serves exactly this
	 * purpose, but for it to be invoked, we must ensure that we bump
	 * @rq->next_class to &ext_sched_class if it's currently idle.
	 *
	 * wakeup_preempt() does the bumping, and since we only invoke it if
	 * @rq->next_class is below &ext_sched_class, it will also
	 * resched_curr(rq).
	 */
	/*
	 * 两个上游例子共同说明“已插 local DSQ”与“下一刻可运行”之间存在放锁窗口；
	 * wakeup_preempt 把 next_class 至少提升到 ext，使后来唤醒的高 class 能触发重检。
	 */
	if (sched_class_above(p->sched_class, rq->next_class))
		wakeup_preempt(rq, p, 0);

	/*
	 * If @rq is in balance, the CPU is already vacant and looking for the
	 * next task to run. No need to preempt or trigger resched after moving
	 * @p into its local DSQ.
	 * Note that the wakeup_preempt() above may have already triggered
	 * a resched if @rq->next_class was idle. It's harmless, since
	 * need_resched is cleared immediately after task pick.
	 */
/* 当前 CPU 正在找下一任务，本地队列已经可见，额外 resched 没有推进作用。 */
	if (rq->scx.flags & SCX_RQ_IN_BALANCE)
		/* 当前 CPU 正在找下一任务，本地队列已经可见，额外 resched 没有推进作用。 */
		return;

	if ((enq_flags & SCX_ENQ_PREEMPT) && p != rq->curr &&
	    rq->curr->sched_class == &ext_sched_class) {
		rq->curr->scx.slice = 0;
		/* 清零当前 SCX slice 是强制下一 pick 重新仲裁，而不是直接切换到 p。 */
		resched_curr(rq);
	}
}

/* 将已取得 custody 的 task 链入 FIFO 或 vtime priq，并发布 QUEUED/qseq 新一代。 */
/*
 * 业务背景：这是所有 dispatch verdict 的队列提交点，把尚未属于 DSQ 的 task 链入
 * local/global/bypass/user DSQ，并在发布 ops_state 前完成计数、custody 与 fast pointer。
 * 入参：sch 为所属 scheduler；rq 为 task 当前/dispatch 上下文；dsq 为目标借用队列；
 * p 已取得 dispatch custody；enq_flags 选择 FIFO 头尾、PRIQ、IMMED 和状态清理。
 * 出参/返回：无；成功后 p->scx.dsq 指向目标并获得新 dsq_seq，可能离开或进入 BPF custody。
 * 注意事项：non-local DSQ 在函数内加锁；销毁/非法排序退化到 global/FIFO 并登记错误。
 */
static void dispatch_enqueue(struct scx_sched *sch, struct rq *rq,
			     struct scx_dispatch_q *dsq, struct task_struct *p,
			     u64 enq_flags)
{
	bool is_local = dsq->id == SCX_DSQ_LOCAL;

	WARN_ON_ONCE(p->scx.dsq || !list_empty(&p->scx.dsq_list.node));
	WARN_ON_ONCE((p->scx.dsq_flags & SCX_TASK_DSQ_ON_PRIQ) ||
		     !RB_EMPTY_NODE(&p->scx.dsq_priq));

	if (!is_local) {
		/* local DSQ 已由 rq lock 保护；其他 DSQ 使用自身 raw lock，嵌套标志声明锁层级。 */
		raw_spin_lock_nested(&dsq->lock,
			(enq_flags & SCX_ENQ_NESTED) ? SINGLE_DEPTH_NESTING : 0);

		if (unlikely(dsq->id == SCX_DSQ_INVALID)) {
			/* destroy 已先把 id 置 INVALID；放旧锁后转 global，保证 task 仍可运行。 */
			scx_error(sch, "attempting to dispatch to a destroyed dsq");
			/* fall back to the global dsq */
			/* 没有可用 local 目标时退回 scheduler 对应 NUMA 节点的 global DSQ，确保任务仍有可消费去向。 */
			raw_spin_unlock(&dsq->lock);
			dsq = find_global_dsq(sch, task_cpu(p));
			raw_spin_lock(&dsq->lock);
		}
	}

	if (unlikely((dsq->id & SCX_DSQ_FLAG_BUILTIN) &&
		     (enq_flags & SCX_ENQ_DSQ_PRIQ))) {
		/*
		 * SCX_DSQ_LOCAL and SCX_DSQ_GLOBAL DSQs always consume from
		 * their FIFO queues. To avoid confusion and accidentally
		 * starving vtime-dispatched tasks by FIFO-dispatched tasks, we
		 * disallow any internal DSQ from doing vtime ordering of
		 * tasks.
		 */
		/* builtin consumer 永远读 FIFO；若允许 PRIQ 会使 vtime task 被 FIFO task 长期遮蔽。 */
		scx_error(sch, "cannot use vtime ordering for built-in DSQs");
		enq_flags &= ~SCX_ENQ_DSQ_PRIQ;
	}

	if (enq_flags & SCX_ENQ_DSQ_PRIQ) {
		struct rb_node *rbp;

		/*
		 * A PRIQ DSQ shouldn't be using FIFO enqueueing. As tasks are
		 * linked to both the rbtree and list on PRIQs, this can only be
		 * tested easily when adding the first task.
		 */
		/* task 同时入 rb-tree 和 list；仅首元素时可无歧义检测此前是否混入 FIFO 模式。 */
		if (unlikely(RB_EMPTY_ROOT(&dsq->priq) &&
			     nldsq_next_task(dsq, NULL, false)))
			scx_error(sch, "DSQ ID 0x%016llx already had FIFO-enqueued tasks",
				  dsq->id);

		p->scx.dsq_flags |= SCX_TASK_DSQ_ON_PRIQ;
		rb_add(&p->scx.dsq_priq, &dsq->priq, scx_dsq_priq_less);

		/*
		 * Find the previous task and insert after it on the list so
		 * that @dsq->list is vtime ordered.
		 */
/* list 镜像 rb-tree 的 vtime 顺序，让统一 iterator 无需理解红黑树。 */
		rbp = rb_prev(&p->scx.dsq_priq);
		/* list 镜像 rb-tree 的 vtime 顺序，让统一 iterator 无需理解红黑树。 */
		if (rbp) {
			struct task_struct *prev =
				container_of(rbp, struct task_struct,
					     scx.dsq_priq);
			list_add(&p->scx.dsq_list.node, &prev->scx.dsq_list.node);
			/* first task unchanged - no update needed */
			/* 新节点未成为队首，first_task 指针保持不变，无需发布写。 */
		} else {
			list_add(&p->scx.dsq_list.node, &dsq->list);
			/* not builtin and new task is at head - use fastpath */
			/* user DSQ 的新节点成为队首，直接走 first_task 快速发布路径。 */
			rcu_assign_pointer(dsq->first_task, p);
		}
	} else {
		/* a FIFO DSQ shouldn't be using PRIQ enqueuing */
		/* 反向混用同样会破坏单一消费顺序，记录错误但仍以 FIFO 保证 task 前进。 */
		if (unlikely(!RB_EMPTY_ROOT(&dsq->priq)))
			scx_error(sch, "DSQ ID 0x%016llx already had PRIQ-enqueued tasks",
				  dsq->id);

		if (enq_flags & (SCX_ENQ_HEAD | SCX_ENQ_PREEMPT)) {
			list_add(&p->scx.dsq_list.node, &dsq->list);
			/* new task inserted at head - use fastpath */
			/* FIFO 头插使新 task 成为队首，更新 first_task 供无锁 peek 快速读取。 */
			if (!(dsq->id & SCX_DSQ_FLAG_BUILTIN))
				rcu_assign_pointer(dsq->first_task, p);
		} else {
			/*
			 * dsq->list can contain parked BPF iterator cursors, so
			 * list_empty() here isn't a reliable proxy for "no real
			 * task in the DSQ". Test dsq->first_task directly.
			 */
			/* cursor 也占 list 节点，first_task 才是“是否已有真实 task”的 RCU fastpath。 */
			list_add_tail(&p->scx.dsq_list.node, &dsq->list);
			if (!dsq->first_task && !(dsq->id & SCX_DSQ_FLAG_BUILTIN))
				rcu_assign_pointer(dsq->first_task, p);
		}
	}

	/* seq records the order tasks are queued, used by BPF DSQ iterator */
	/* 先递增队列代际再复制到 task，cursor 可据此排除遍历开始后的新入队。 */
	WRITE_ONCE(dsq->seq, dsq->seq + 1);
	p->scx.dsq_seq = dsq->seq;

	dsq_inc_nr(dsq, p, enq_flags);
	/* 计数和链表均完成后才写 dsq 归属，锁内读者因此不会看到半链接 task。 */
	p->scx.dsq = dsq;

	/*
	 * Update custody and call ops.dequeue() before clearing ops_state:
	 * once ops_state is cleared, waiters in ops_dequeue() can proceed
	 * and dequeue_task_scx() will RMW p->scx.flags. If we clear
	 * ops_state first, both sides would modify p->scx.flags
	 * concurrently in a non-atomic way.
	 */
	/*
	 * 必须先更新 custody/flags，再 release 清 ops_state；waiter acquire 返回后才能安全
	 * 对同一非原子 flags 做 RMW，否则 dequeue 与本路径可能互相覆盖位更新。
	 */
	if (is_local) {
		local_dsq_post_enq(sch, dsq, p, enq_flags);
	} else {
		/*
		 * Task on global/bypass DSQ: leave custody, task on
		 * non-terminal DSQ: enter custody.
		 */
		/* global/bypass DSQ 由内核直接调度，task 离开 BPF custody；非终态 DSQ 则重新进入策略 custody。 */
		if (dsq->id == SCX_DSQ_GLOBAL || dsq->id == SCX_DSQ_BYPASS)
			call_task_dequeue(sch, rq, p, 0);
		else
			p->scx.flags |= SCX_TASK_IN_CUSTODY;

		raw_spin_unlock(&dsq->lock);
	}

	/*
	 * We're transitioning out of QUEUEING or DISPATCHING. store_release to
	 * match waiters' load_acquire.
	 */
/* release 与 wait_ops_state 的 acquire 配对，提交上述队列和 task 字段写入。 */
	if (enq_flags & SCX_ENQ_CLEAR_OPSS)
		/* release 与 wait_ops_state 的 acquire 配对，提交上述队列和 task 字段写入。 */
		atomic_long_set_release(&p->scx.ops_state, SCX_OPSS_NONE);
}

/*
 * 业务背景：在已持 DSQ 保护锁时统一撤销 rb/list 双重链接、计数和 first_task fastpath。
 * 入参：p 必须仍属于 dsq；dsq 为已锁借用队列。
 * 出参/返回：无；p 节点恢复未链接态，DSQ nr/首任务指针同步更新。
 * 注意事项：本函数不清 p->scx.dsq，调用者在处理 holding_cpu 等竞态后完成归属清理。
 */
static void task_unlink_from_dsq(struct task_struct *p,
				 struct scx_dispatch_q *dsq)
{
	WARN_ON_ONCE(list_empty(&p->scx.dsq_list.node));

	if (p->scx.dsq_flags & SCX_TASK_DSQ_ON_PRIQ) {
		/* PRIQ task 同时存在 rb/list，两处都必须摘除并清节点哨兵以支持下次入队。 */
		rb_erase(&p->scx.dsq_priq, &dsq->priq);
		RB_CLEAR_NODE(&p->scx.dsq_priq);
		p->scx.dsq_flags &= ~SCX_TASK_DSQ_ON_PRIQ;
	}

	list_del_init(&p->scx.dsq_list.node);
	dsq_dec_nr(dsq, p);

	if (!(dsq->id & SCX_DSQ_FLAG_BUILTIN) && dsq->first_task == p) {
		/* user DSQ 的 RCU peek fastpath 指向下一个真实 task，iterator cursor 会被跳过。 */
		struct task_struct *first_task;

		first_task = nldsq_next_task(dsq, NULL, false);
		rcu_assign_pointer(dsq->first_task, first_task);
	}
}

/* 从当前 DSQ 摘除 task 并清队列状态；本地和非本地 DSQ 分别使用 rq/dsq 锁。 */
/*
 * 业务背景：dequeue/迁移/取消 verdict 时从 task 当前 DSQ 撤销发布，并解析与
 * dispatch_to_local_dsq() 的 holding_cpu 竞态。
 * 入参：rq 为已持锁且当前属于 p 的运行队列；p 为借用 task。
 * 出参/返回：无；正常摘链或取消 deferred/holding 状态，最终 p->scx.dsq 为 NULL。
 * 注意事项：non-local DSQ 另取 dsq lock；holding_cpu 非负表示另一 CPU 已摘链但未提交。
 */
static void dispatch_dequeue(struct rq *rq, struct task_struct *p)
{
	struct scx_dispatch_q *dsq = p->scx.dsq;
	bool is_local = dsq == &rq->scx.local_dsq;

	lockdep_assert_rq_held(rq);

	if (!dsq) {
		/*
		 * If !dsq && on-list, @p is on @rq's ddsp_deferred_locals.
		 * Unlinking is all that's needed to cancel.
		 */
		/* dsq 为空但 list 非空只可能是 direct-dispatch deferred-local 暂存节点。 */
		if (unlikely(!list_empty(&p->scx.dsq_list.node)))
			list_del_init(&p->scx.dsq_list.node);

		/*
		 * When dispatching directly from the BPF scheduler to a local
		 * DSQ, the task isn't associated with any DSQ but
		 * @p->scx.holding_cpu may be set under the protection of
		 * %SCX_OPSS_DISPATCHING.
		 */
		/* 同步直派尚未关联 DSQ，清 holding_cpu 就向搬运方宣布取消胜出。 */
		if (p->scx.holding_cpu >= 0)
			p->scx.holding_cpu = -1;

		return;
	}

	if (!is_local)
		/* local DSQ 已由 rq lock 覆盖；其他 DSQ 的链接和 holding_cpu 由自身锁稳定。 */
		raw_spin_lock(&dsq->lock);

	/*
	 * Now that we hold @dsq->lock, @p->holding_cpu and @p->scx.dsq_* can't
	 * change underneath us.
	*/
	/* 此后 p 的 DSQ 链接与搬运占有者稳定，可决定是本方摘链还是仅取消对方提交。 */
	if (p->scx.holding_cpu < 0) {
		/* @p must still be on @dsq, dequeue */
		/* 无 consumer 暂持，task 必须仍链接在 dsq，本路径取得摘除责任。 */
		task_unlink_from_dsq(p, dsq);
	} else {
		/*
		 * We're racing against dispatch_to_local_dsq() which already
		 * removed @p from @dsq and set @p->scx.holding_cpu. Clear the
		 * holding_cpu which tells dispatch_to_local_dsq() that it lost
		 * the race.
		 */
		/* 对方已摘链，不能重复 list_del；写 -1 是其重取 rq lock 后的失败判据。 */
		WARN_ON_ONCE(!list_empty(&p->scx.dsq_list.node));
		p->scx.holding_cpu = -1;
	}
	p->scx.dsq = NULL;

	if (!is_local)
		raw_spin_unlock(&dsq->lock);
}

/*
 * Abbreviated version of dispatch_dequeue() that can be used when both @p's rq
 * and dsq are locked.
 */
/*
 * 上游契约：rq 与 non-local DSQ 两锁都已持有时的精简 dequeue，不再处理 holding 竞态。
 * 入参：p 为已锁当前 rq 上且仍链接 dsq 的 task；dsq 已持锁。
 * 出参/返回：无；摘除 task 并清 p->scx.dsq。
 * 注意事项：调用者必须已排除 dispatch consumer，不能用于 holding_cpu 非负状态。
 */
static void dispatch_dequeue_locked(struct task_struct *p,
				    struct scx_dispatch_q *dsq)
{
	lockdep_assert_rq_held(task_rq(p));
	lockdep_assert_held(&dsq->lock);

	task_unlink_from_dsq(p, dsq);
	p->scx.dsq = NULL;
}

/*
 * 业务背景：把不可信 BPF DSQ verdict 解析为真实队列，并为非法 CPU/id 提供 global
 * fallback，保证 scheduler 被判错后 task 仍有可运行去处。
 * 入参：sch 为错误归属实例；rq 为当前 local 解释基准；dsq_id 为 verdict；tcpu 为
 * task CPU，用于选择 NUMA global DSQ。
 * 出参/返回：总返回一个借用 DSQ；合法 local/local-on/global/user 命中目标，错误退 global。
 * 注意事项：返回指针不带锁或引用；调用者随后按 local/non-local 协议加锁并复验状态。
 */
static struct scx_dispatch_q *find_dsq_for_dispatch(struct scx_sched *sch,
						    struct rq *rq, u64 dsq_id,
						    s32 tcpu)
{
	struct scx_dispatch_q *dsq;

	if (dsq_id == SCX_DSQ_LOCAL)
		/* 未编码 CPU 的 LOCAL 始终指当前 dispatch rq。 */
		return &rq->scx.local_dsq;

	if ((dsq_id & SCX_DSQ_LOCAL_ON) == SCX_DSQ_LOCAL_ON) {
		s32 cpu = scx_cpu_ret(sch, dsq_id & SCX_DSQ_LOCAL_CPU_MASK);

		if (!scx_cpu_valid(sch, cpu, "in SCX_DSQ_LOCAL_ON dispatch verdict"))
			/* scx_cpu_valid 已登记错误；fallback 避免用坏编号索引 cpu_rq。 */
			return find_global_dsq(sch, tcpu);

		return &cpu_rq(cpu)->scx.local_dsq;
	}

	if (dsq_id == SCX_DSQ_GLOBAL)
		dsq = find_global_dsq(sch, tcpu);
	else
		dsq = find_user_dsq(sch, dsq_id);

	if (unlikely(!dsq)) {
		/* DSQ 可能从未创建或已摘表；错误退出异步进行，本 task 先落 global。 */
		scx_error(sch, "non-existent DSQ 0x%llx", dsq_id);
		return find_global_dsq(sch, tcpu);
	}

	return dsq;
}

/* enqueue 回调期间登记一次 direct dispatch；重复或错误 task 触发 scheduler error。 */
/*
 * 业务背景：select_cpu/enqueue 回调可同步给当前 task 一个 DSQ verdict；per-CPU marker
 * 将授权绑定到这一次 enqueue，阻止 BPF 越权直派其他 task 或重复提交。
 * 入参：sch 为实例；ddsp_task 为回调入口授权值；p 为 BPF 提交 task；dsq_id/enq_flags
 * 为待暂存 verdict。
 * 出参/返回：无；合法时写 p 的 deferred verdict，任何调用都先把 marker 污染为已使用。
 * 注意事项：错误只登记退出且不写 verdict；ERR_PTR marker 永不可能与有效 task 相等。
 */
static void mark_direct_dispatch(struct scx_sched *sch,
				 struct task_struct *ddsp_task,
				 struct task_struct *p, u64 dsq_id,
				 u64 enq_flags)
{
	/*
	 * Mark that dispatch already happened from ops.select_cpu() or
	 * ops.enqueue() by spoiling direct_dispatch_task with a non-NULL value
	 * which can never match a valid task pointer.
	 */
	/* 先消费一次性授权，即使随后参数校验失败也禁止同一回调再次尝试。 */
	__this_cpu_write(direct_dispatch_task, ERR_PTR(-ESRCH));

	/* @p must match the task on the enqueue path */
	/* 只允许回调处理入口 task；ERR_PTR 表示此前已经成功或失败地消费过授权。 */
	if (unlikely(p != ddsp_task)) {
		if (IS_ERR(ddsp_task))
			scx_error(sch, "%s[%d] already direct-dispatched",
				  p->comm, p->pid);
		else
			scx_error(sch, "scheduling for %s[%d] but trying to direct-dispatch %s[%d]",
				  ddsp_task->comm, ddsp_task->pid,
				  p->comm, p->pid);
		return;
	}

	/* 入口 task 匹配后，原 verdict 必须为空；重复状态只告警并由新值覆盖。 */
	WARN_ON_ONCE(p->scx.ddsp_dsq_id != SCX_DSQ_INVALID);
	WARN_ON_ONCE(p->scx.ddsp_enq_flags);

	p->scx.ddsp_dsq_id = dsq_id;
	p->scx.ddsp_enq_flags = enq_flags;
}

/*
 * Clear @p direct dispatch state when leaving the scheduler.
 *
 * Direct dispatch state must be cleared in the following cases:
 *  - direct_dispatch(): cleared on the synchronous enqueue path, deferred
 *    dispatch keeps the state until consumed
 *  - process_ddsp_deferred_locals(): cleared after consuming deferred state,
 *  - do_enqueue_task(): cleared on enqueue fallbacks where the dispatch
 *    verdict is ignored (local/global/bypass)
 *  - dequeue_task_scx(): cleared after dispatch_dequeue(), covering deferred
 *    cancellation and holding_cpu races
 *  - scx_disable_task(): cleared for queued wakeup tasks, which are excluded by
 *    the scx_bypass() loop, so that stale state is not reused by a subsequent
 *    scheduler instance
 */
/* 上述所有终止/兜底路径都必须同时清 id 与 flags，防止下一次 enqueue 复用旧 verdict。 */
static inline void clear_direct_dispatch(struct task_struct *p)
{
	/* 两个字段共同构成 verdict；同时复位，避免新 scheduler/新 enqueue 复用旧 flags。 */
	p->scx.ddsp_dsq_id = SCX_DSQ_INVALID;
	p->scx.ddsp_enq_flags = 0;
}

/* 消费 direct-dispatch 标记并把当前 enqueue task 直接送入目标 DSQ。 */
/*
 * 业务背景：把 enqueue 回调暂存的 direct verdict 同步提交；若目标是远端 local DSQ，
 * 当前 rq 已锁且 pinned，不能双锁远端 rq，故转为 deferred-local 事务。
 * 入参：sch 为所属实例；p 为当前 rq 锁稳定 task；enq_flags 为 core 入队语义。
 * 出参/返回：无；本地/非本地直接入 DSQ，远端 local 则挂 deferred 链并稍后搬运。
 * 注意事项：远端延期前先把 ops_state 清 NONE 解除 waiter，verdict 直到 consumer 才清。
 */
static void direct_dispatch(struct scx_sched *sch, struct task_struct *p,
			    u64 enq_flags)
{
	struct rq *rq = task_rq(p);
	struct scx_dispatch_q *dsq =
		find_dsq_for_dispatch(sch, rq, p->scx.ddsp_dsq_id, task_cpu(p));
	u64 ddsp_enq_flags;

	touch_core_sched_dispatch(rq, p);

	p->scx.ddsp_enq_flags |= enq_flags;
	/* BPF verdict flags 与 core 本次 enqueue flags 合并，提交时作为一个原子语义包消费。 */

	/*
	 * We are in the enqueue path with @rq locked and pinned, and thus can't
	 * double lock a remote rq and enqueue to its local DSQ. For
	 * DSQ_LOCAL_ON verdicts targeting the local DSQ of a remote CPU, defer
	 * the enqueue so that it's executed when @rq can be unlocked.
	 */
/* select_cpu 直派可能尚未进入 QUEUEING，无需额外状态转换。 */
	if (dsq->id == SCX_DSQ_LOCAL && dsq != &rq->scx.local_dsq) {
		unsigned long opss;

		opss = atomic_long_read(&p->scx.ops_state) & SCX_OPSS_STATE_MASK;

		switch (opss & SCX_OPSS_STATE_MASK) {
		case SCX_OPSS_NONE:
			/* select_cpu 直派可能尚未进入 QUEUEING，无需额外状态转换。 */
			break;
		case SCX_OPSS_QUEUEING:
			/*
			 * As @p was never passed to the BPF side, _release is
			 * not strictly necessary. Still do it for consistency.
			 */
			/* task 未真正交给 BPF custody，但用 release 保持所有直派出口的发布契约一致。 */
			atomic_long_set_release(&p->scx.ops_state, SCX_OPSS_NONE);
			break;
		default:
			/* 其他状态违反“当前 enqueue 独占 task”前提；告警后仍清状态避免 waiter 卡死。 */
			WARN_ONCE(true, "sched_ext: %s[%d] has invalid ops state 0x%lx in direct_dispatch()",
				  p->comm, p->pid, opss);
			atomic_long_set_release(&p->scx.ops_state, SCX_OPSS_NONE);
			break;
		}

		WARN_ON_ONCE(p->scx.dsq || !list_empty(&p->scx.dsq_list.node));
		list_add_tail(&p->scx.dsq_list.node,
			      &rq->scx.ddsp_deferred_locals);
		schedule_deferred_locked(rq);
		/* 节点暂挂源 rq 私有链，run_deferred 可放锁后按锁序取得远端 rq。 */
		return;
	}

	ddsp_enq_flags = p->scx.ddsp_enq_flags;
	/* 同步路径先复制 verdict，再清一次性状态，随后 dispatch_enqueue 完成发布。 */
	clear_direct_dispatch(p);

	dispatch_enqueue(sch, rq, dsq, p, ddsp_enq_flags | SCX_ENQ_CLEAR_OPSS);
}

/*
 * 业务背景：判断 BPF 所见 ONLINE 与 core cpu_active 是否同时成立；二者合取保证
 * true 返回后，本次调度操作结束前 SCX_RQ_ONLINE 不会在无 rq lock 情况下被撤销。
 * 入参：rq 为静态借用队列，可不持锁。
 * 出参/返回：两种在线视图都为真时 true；无副作用。
 * 注意事项：只看 SCX_RQ_ONLINE 会与 hotplug 过渡竞态，只看 cpu_active 会越过 BPF 通知边界。
 */
static bool scx_rq_online(struct rq *rq)
{
	/*
	 * Test both cpu_active() and %SCX_RQ_ONLINE. %SCX_RQ_ONLINE indicates
	 * the online state as seen from the BPF scheduler. cpu_active() test
	 * guarantees that, if this function returns %true, %SCX_RQ_ONLINE will
	 * stay set until the current scheduling operation is complete even if
	 * we aren't locking @rq.
	 */
/* 建立 BPF custody 后调用 select/enqueue；bypass 或无回调时走内核安全 DSQ。 */
	return likely((rq->scx.flags & SCX_RQ_ONLINE) && cpu_active(cpu_of(rq)));
}

/* 建立 BPF custody 后调用 select/enqueue；bypass 或无回调时走内核安全 DSQ。 */
/*
 * 业务背景：这是 runnable task 的策略分流器；内部迁移/恢复、offline、bypass、退出、
 * migration-disabled、缺失 enqueue op 各自绕过 BPF，正常路径以 qseq 发布 BPF custody。
 * 入参：rq 已锁；p 已标 QUEUED 且为借用 task；enq_flags 为 SCX 语义；sticky_cpu 表示
 * 内部迁移目标，负值表示普通入队。
 * 出参/返回：无；task 最终进入 local/global/bypass DSQ、direct deferred 链或 BPF custody。
 * 注意事项：QUEUEING→QUEUED 用 release 发布；所有 fallback 都补默认 slice 并清旧 verdict。
 */
static void do_enqueue_task(struct rq *rq, struct task_struct *p, u64 enq_flags,
			    int sticky_cpu)
{
	struct scx_sched *sch = scx_task_sched(p);
	struct task_struct **ddsp_taskp;
	struct scx_dispatch_q *dsq;
	unsigned long qseq;

	WARN_ON_ONCE(!(p->scx.flags & SCX_TASK_QUEUED));

	/* internal movements - rq migration / RESTORE */
	/* sticky 指回本 rq 表示内核内部移动/RESTORE，保持原 slice 并直接回 local DSQ。 */
	if (sticky_cpu == cpu_of(rq))
		goto local_norefill;

	/*
	 * Clear persistent TASK_IMMED for fresh enqueues, see dsq_inc_nr().
	 * Note that exiting and migration-disabled tasks that skip
	 * ops.enqueue() below will lose IMMED protection unless
	 * %SCX_OPS_ENQ_EXITING / %SCX_OPS_ENQ_MIGRATION_DISABLED are set.
	 */
	/* 新业务事件先撤销跨 SAVE/RESTORE 保留的 IMMED；只有本次直派可重新建立。 */
	p->scx.flags &= ~SCX_TASK_IMMED;

	/*
	 * If !scx_rq_online(), we already told the BPF scheduler that the CPU
	 * is offline and are just running the hotplug path. Don't bother the
	 * BPF scheduler.
	 */
/* BPF 已收到 CPU offline，hotplug 收尾 task 留在内核 local DSQ，不再回调失配状态。 */
	if (!scx_rq_online(rq))
		/* BPF 已收到 CPU offline，hotplug 收尾 task 留在内核 local DSQ，不再回调失配状态。 */
		goto local;

	if (scx_bypassing(sch, cpu_of(rq))) {
		/* 故障期完全绕开 BPF，由最近可工作的祖先/root bypass DSQ 保证前进。 */
		__scx_add_event(sch, SCX_EV_BYPASS_DISPATCH, 1);
		goto bypass;
	}

	if (p->scx.ddsp_dsq_id != SCX_DSQ_INVALID)
		/* select_cpu 可能已给 verdict，enqueue 阶段直接消费而不重复调用 BPF。 */
		goto direct;

	/* see %SCX_OPS_ENQ_EXITING */
	/* 该分支对应 SCX_OPS_ENQ_EXITING：退出 task 不交给未声明支持此语义的 BPF enqueue。 */
	if (!(sch->ops.flags & SCX_OPS_ENQ_EXITING) &&
	    unlikely(p->flags & PF_EXITING)) {
		__scx_add_event(sch, SCX_EV_ENQ_SKIP_EXITING, 1);
		goto local;
	}

	/* see %SCX_OPS_ENQ_MIGRATION_DISABLED */
	/* scheduler 未声明处理 migration-disabled task 时，本地兜底避免 BPF 选到不可迁移 CPU。 */
	if (!(sch->ops.flags & SCX_OPS_ENQ_MIGRATION_DISABLED) &&
	    is_migration_disabled(p)) {
		__scx_add_event(sch, SCX_EV_ENQ_SKIP_MIGRATION_DISABLED, 1);
		goto local;
	}

	if (unlikely(!SCX_HAS_OP(sch, enqueue)))
		goto global;

	/* DSQ bypass didn't trigger, enqueue on the BPF scheduler */
	/* qseq 每 rq 单调递增并编码进 ops_state，使异步 dispatch 只能领取当前一代 custody。 */
	qseq = rq->scx.ops_qseq++ << SCX_OPSS_QSEQ_SHIFT;

	WARN_ON_ONCE(atomic_long_read(&p->scx.ops_state) != SCX_OPSS_NONE);
	atomic_long_set(&p->scx.ops_state, SCX_OPSS_QUEUEING | qseq);

	ddsp_taskp = this_cpu_ptr(&direct_dispatch_task);
	/* 回调窗口把唯一合法 direct-dispatch task 发布到 per-CPU marker。 */
	WARN_ON_ONCE(*ddsp_taskp);
	*ddsp_taskp = p;

	SCX_CALL_OP_TASK(sch, enqueue, rq, p, enq_flags);

	*ddsp_taskp = NULL;
	/* 先撤销临时 kfunc 授权，再检查 BPF 是否写入 direct verdict。 */
	if (p->scx.ddsp_dsq_id != SCX_DSQ_INVALID)
		goto direct;

	/*
	 * Task is now in BPF scheduler's custody. Set %SCX_TASK_IN_CUSTODY
	 * so ops.dequeue() is called when it leaves custody.
	 */
/* 未直派时 BPF 持有“将来 dispatch 或 dequeue”的责任，内核用该位跟踪归还回调。 */
	p->scx.flags |= SCX_TASK_IN_CUSTODY;
	/* 未直派时 BPF 持有“将来 dispatch 或 dequeue”的责任，内核用该位跟踪归还回调。 */

	/*
	 * If not directly dispatched, QUEUEING isn't clear yet and dispatch or
	 * dequeue may be waiting. The store_release matches their load_acquire.
	 */
/* waiter acquire 后可看到 IN_CUSTODY 与本次 BPF 回调产生的全部状态。 */
	atomic_long_set_release(&p->scx.ops_state, SCX_OPSS_QUEUED | qseq);
	/* waiter acquire 后可看到 IN_CUSTODY 与本次 BPF 回调产生的全部状态。 */
	return;

direct:
	direct_dispatch(sch, p, enq_flags);
	return;
local_norefill:
	/* 三个标签统一归并到 local/global/bypass DSQ，最终都由 dispatch_enqueue 建立队列所有权。 */
	dispatch_enqueue(sch, rq, &rq->scx.local_dsq, p, enq_flags);
	return;
local:
	dsq = &rq->scx.local_dsq;
	goto enqueue;
global:
	/* global 实际按 task CPU 所在 NUMA node 选择内建 DSQ，保持访问局部性。 */
	dsq = find_global_dsq(sch, task_cpu(p));
	goto enqueue;
bypass:
	dsq = bypass_enq_target_dsq(sch, task_cpu(p));
	goto enqueue;

enqueue:
	/*
	 * For task-ordering, slice refill must be treated as implying the end
	 * of the current slice. Otherwise, the longer @p stays on the CPU, the
	 * higher priority it becomes from scx_prio_less()'s POV.
	 */
	/* fallback refill 等同结束旧 slice，先刷新 core FIFO 时间，避免运行越久反而优先级越高。 */
	touch_core_sched(rq, p);
	refill_task_slice_dfl(sch, p);
	clear_direct_dispatch(p);
	dispatch_enqueue(sch, rq, dsq, p, enq_flags);
}

/* runnable_node 是否链接是 rq->scx.runnable_list 成员关系的唯一判据；返回不取引用。 */
static bool task_runnable(const struct task_struct *p)
{
	return !list_empty(&p->scx.runnable_node);
}

/*
 * 业务背景：把 task 发布到 rq 的 SCX runnable 生命周期链，并按需重置 watchdog 时间戳。
 * 入参：rq 已锁；p 尚未链接 runnable_node。
 * 出参/返回：无；p 追加到队尾，可能写 runnable_at 并清 RESET 标志。
 * 注意事项：必须 tail 插入，bypass 的稳定遍历依赖新 runnable task 不插到 cursor 前方。
 */
static void set_task_runnable(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);

	if (p->scx.flags & SCX_TASK_RESET_RUNNABLE_AT) {
		p->scx.runnable_at = jiffies;
		p->scx.flags &= ~SCX_TASK_RESET_RUNNABLE_AT;
	}

	/*
	 * list_add_tail() must be used. scx_bypass() depends on tasks being
	 * appended to the runnable_list.
	 */
	/* bypass 逐项推进时，新 task 只追加在尾部，保证本轮不会跳过旧成员或反复追逐新成员。 */
	list_add_tail(&p->scx.runnable_node, &rq->scx.runnable_list);
}

/*
 * 从 runnable 链摘除 task；reset_runnable_at 决定下次进入时是否重建 watchdog 起点。
 * 调用者持 rq lock；函数不改变 QUEUED/DSQ/custody，供分阶段 dequeue 使用。
 */
static void clr_task_runnable(struct task_struct *p, bool reset_runnable_at)
{
	list_del_init(&p->scx.runnable_node);
	if (reset_runnable_at)
		p->scx.flags |= SCX_TASK_RESET_RUNNABLE_AT;
}

/* core 入队标志转换成 SCX 语义，维护 runnable 状态后交给 do_enqueue_task。 */
/*
 * 业务背景：core scheduler 的 class enqueue 入口，建立 runnable/QUEUED 计数和 BPF
 * runnable 回调，再交给策略分流；它也是 dl_server 从 0→1 runnable 的启动边界。
 * 入参：rq 已锁；p 为属于本 rq 的借用 task；core_enq_flags 描述 wakeup/restore/migrate。
 * 出参/返回：无；task 成为 SCX runnable 并进入后续 custody/DSQ，或重复入队仅告警退出。
 * 注意事项：IN_WAKEUP 覆盖整个事务；sticky 内部迁移成功后必须清 -1，selected CPU 失配记事件。
 */
static void enqueue_task_scx(struct rq *rq, struct task_struct *p, int core_enq_flags)
{
	struct scx_sched *sch = scx_task_sched(p);
	int sticky_cpu = p->scx.sticky_cpu;
	u64 enq_flags = core_enq_flags | rq->scx.extra_enq_flags;

	if (enq_flags & ENQUEUE_WAKEUP)
		/* 标记让 schedule_deferred_locked 复用稍后的 task_woken hook，避免多投 irq_work。 */
		rq->scx.flags |= SCX_RQ_IN_WAKEUP;

	/*
	 * Restoring a running task will be immediately followed by
	 * set_next_task_scx() which expects the task to not be on the BPF
	 * scheduler as tasks can only start running through local DSQs. Force
	 * direct-dispatch into the local DSQ by setting the sticky_cpu.
	 */
/* 正在运行的恢复 task 必须先回 local DSQ，set_next 才满足“运行只来自 local”不变量。 */
	if (unlikely(enq_flags & ENQUEUE_RESTORE) && task_current(rq, p))
		/* 正在运行的恢复 task 必须先回 local DSQ，set_next 才满足“运行只来自 local”不变量。 */
		sticky_cpu = cpu_of(rq);

	if (p->scx.flags & SCX_TASK_QUEUED) {
		/* core 可能给重复 enqueue；保持已有成员关系，不重复增加 nr_running。 */
		WARN_ON_ONCE(!task_runnable(p));
		goto out;
	}

	set_task_runnable(rq, p);
	/* 先发布 runnable 链和 QUEUED 位，再更新两套 rq 计数，使回调看到一致入口状态。 */
	p->scx.flags |= SCX_TASK_QUEUED;
	rq->scx.nr_running++;
	add_nr_running(rq, 1);

	if (SCX_HAS_OP(sch, runnable) && !task_on_rq_migrating(p))
		/* 内部 rq 迁移不改变业务 runnable 生命周期，抑制虚假的 runnable 回调。 */
		SCX_CALL_OP_TASK(sch, runnable, rq, p, enq_flags);

	if (enq_flags & SCX_ENQ_WAKEUP)
		touch_core_sched(rq, p);

	/* Start dl_server if this is the first task being enqueued */
/* 第一个 SCX runnable task 才启动带宽 server；最后一个在 dequeue 对称停止。 */
	if (rq->scx.nr_running == 1)
		/* 第一个 SCX runnable task 才启动带宽 server；最后一个在 dequeue 对称停止。 */
		dl_server_start(&rq->ext_server);

	do_enqueue_task(rq, p, enq_flags, sticky_cpu);

	if (sticky_cpu >= 0)
		/* 目标 enqueue 是内部迁移提交点，清哨兵后普通 dequeue 才会再次通知 BPF。 */
		p->scx.sticky_cpu = -1;
out:
	rq->scx.flags &= ~SCX_RQ_IN_WAKEUP;

	if ((enq_flags & SCX_ENQ_CPU_SELECTED) &&
	    unlikely(cpu_of(rq) != p->scx.selected_cpu))
		__scx_add_event(sch, SCX_EV_SELECT_CPU_FALLBACK, 1);
}

/*
 * 业务背景：task 离开 runnable 前收回可能处于 QUEUED/DISPATCHING 的 BPF custody，
 * 并保证任何并发 dispatch 都在本次 dequeue 完成前提交或放弃。
 * 入参：rq 已持 p 当前 rq lock；p 为 QUEUED task；deq_flags 描述 sleep/change 等原因。
 * 出参/返回：无；摘 runnable_node，ops_state 最终 NONE，并按需调用一次 ops.dequeue。
 * 注意事项：acquire/release 与 qseq 关闭异步竞态；等待 DISPATCHING 时对方不得反取此 rq lock。
 */
static void ops_dequeue(struct rq *rq, struct task_struct *p, u64 deq_flags)
{
	struct scx_sched *sch = scx_task_sched(p);
	unsigned long opss;

	/* dequeue is always temporary, don't reset runnable_at */
	/* 最终 quiescent 与临时 dequeue 分离，此处保留 runnable_at 供重新入队继续 watchdog 计时。 */
	clr_task_runnable(p, false);

retry:
	/* acquire ensures that we see the preceding updates on QUEUED */
	/* acquire 取得 producer 在 QUEUED release 前写入的 custody、verdict 与队列状态。 */
	opss = atomic_long_read_acquire(&p->scx.ops_state);

	switch (opss & SCX_OPSS_STATE_MASK) {
	case SCX_OPSS_NONE:
		/* task 已在内核 DSQ 或并发 dispatch 已完成状态交接，继续检查 custody 位。 */
		break;
	case SCX_OPSS_QUEUEING:
		/*
		 * QUEUEING is started and finished while holding @p's rq lock.
		 * As we're holding the rq lock now, we shouldn't see QUEUEING.
		 */
		/* enqueue 的 QUEUEING 全程持同一 rq lock；观察到它说明锁/状态机已损坏。 */
		BUG();
	case SCX_OPSS_QUEUED:
		/*
		 * A queued task must always be in BPF scheduler's custody. If
		 * SCX_TASK_IN_CUSTODY is clear, finish_dispatch() on another
		 * CPU has already passed call_task_dequeue() (which clears the
		 * flag), but has not yet written SCX_OPSS_NONE. That final
		 * store does not require this rq's lock, so retrying with
		 * cpu_relax() is bounded: we will observe NONE (or DISPATCHING,
		 * handled by the fallthrough) on a subsequent iteration.
		 */
		/* custody 已清而状态未清是极短提交尾窗，忙等而不再次调用 dequeue，避免双回调。 */
		if (unlikely(!(READ_ONCE(p->scx.flags) & SCX_TASK_IN_CUSTODY))) {
			cpu_relax();
			goto retry;
		}

		if (atomic_long_try_cmpxchg(&p->scx.ops_state, &opss,
					    SCX_OPSS_NONE))
			break;
		/* cmpxchg 失败表示 dispatch 抢先把 QUEUED 改为 DISPATCHING，落入等待其完成。 */
		fallthrough;
	case SCX_OPSS_DISPATCHING:
		/*
		 * If @p is being dispatched from the BPF scheduler to a DSQ,
		 * wait for the transfer to complete so that @p doesn't get
		 * added to its DSQ after dequeueing is complete.
		 *
		 * As we're waiting on DISPATCHING with the rq locked, the
		 * dispatching side shouldn't try to lock the rq while
		 * DISPATCHING is set. See dispatch_to_local_dsq().
		 *
		 * DISPATCHING shouldn't have qseq set and control can reach
		 * here with NONE @opss from the above QUEUED case block.
		 * Explicitly wait on %SCX_OPSS_DISPATCHING instead of @opss.
		 */
		/* 精确等待无 qseq 的 DISPATCHING，避免拿旧 QUEUED 状态字等待一个永不相等的值。 */
		wait_ops_state(p, SCX_OPSS_DISPATCHING);
		BUG_ON(atomic_long_read(&p->scx.ops_state) != SCX_OPSS_NONE);
		break;
	}

	/*
	 * Call ops.dequeue() if the task is still in BPF custody.
	 *
	 * The code that clears ops_state to %SCX_OPSS_NONE does not always
	 * clear %SCX_TASK_IN_CUSTODY: in dispatch_to_local_dsq(), when
	 * we're moving a task that was in %SCX_OPSS_DISPATCHING to a
	 * remote CPU's local DSQ, we only set ops_state to %SCX_OPSS_NONE
	 * so that a concurrent dequeue can proceed, but we clear
	 * %SCX_TASK_IN_CUSTODY only when we later enqueue or move the
	 * task. So we can see NONE + IN_CUSTODY here and we must handle
	 * it. Similarly, after waiting on %SCX_OPSS_DISPATCHING we see
	 * NONE but the task may still have %SCX_TASK_IN_CUSTODY set until
	 * it is enqueued on the destination.
	 */
	/* NONE 不必然等于“不在 custody”；远端 local 搬运把清位延迟到目标入队，故最终再判一次。 */
	call_task_dequeue(sch, rq, p, deq_flags);
}

/* 先结算当前执行，再撤销 BPF custody/DSQ/runnable；迁移保留必要的任务状态。 */
/*
 * 业务背景：core class dequeue 的完整逆事务，先收回 BPF custody，再把 running 回调
 * 嵌套在 runnable→quiescent 内，更新 QUEUED/rq 计数并撤销 DSQ/direct 状态。
 * 入参：rq 已锁；p 为本 rq 借用 task；core_deq_flags 区分 sleep、迁移、core-sched pick。
 * 出参/返回：始终 true；成功后 p 不再 QUEUED/DSQ，普通退出还通知 stopping/quiescent。
 * 注意事项：内部迁移抑制生命周期回调；运行 task 提前 stopping 以维持回调严格嵌套。
 */
static bool dequeue_task_scx(struct rq *rq, struct task_struct *p, int core_deq_flags)
{
	struct scx_sched *sch = scx_task_sched(p);
	u64 deq_flags = core_deq_flags;

	/*
	 * Set %SCX_DEQ_SCHED_CHANGE when the dequeue is due to a property
	 * change (not sleep or core-sched pick).
	 */
/* 非睡眠且非 core 临时 pick 的 dequeue 是属性/策略变化，显式告诉 BPF 可重算状态。 */
	if (!(deq_flags & (DEQUEUE_SLEEP | SCX_DEQ_CORE_SCHED_EXEC)))
		/* 非睡眠且非 core 临时 pick 的 dequeue 是属性/策略变化，显式告诉 BPF 可重算状态。 */
		deq_flags |= SCX_DEQ_SCHED_CHANGE;

	if (!(p->scx.flags & SCX_TASK_QUEUED)) {
		/* 幂等防线：未 QUEUED 不得还留 runnable_node，也不能重复减计数。 */
		WARN_ON_ONCE(task_runnable(p));
		return true;
	}

	ops_dequeue(rq, p, deq_flags);

	/*
	 * A currently running task which is going off @rq first gets dequeued
	 * and then stops running. As we want running <-> stopping transitions
	 * to be contained within runnable <-> quiescent transitions, trigger
	 * ->stopping() early here instead of in put_prev_task_scx().
	 *
	 * @p may go through multiple stopping <-> running transitions between
	 * here and put_prev_task_scx() if task attribute changes occur while
	 * balance_one() leaves @rq unlocked. However, they don't contain any
	 * information meaningful to the BPF scheduler and can be suppressed by
	 * skipping the callbacks if the task is !QUEUED.
	 */
	/* 提前 stopping 保证 BPF 总看到 runnable→running→stopping→quiescent 的合法嵌套。 */
	if (SCX_HAS_OP(sch, stopping) && task_current(rq, p)) {
		update_curr_scx(rq);
		SCX_CALL_OP_TASK(sch, stopping, rq, p, false);
	}

	if (SCX_HAS_OP(sch, quiescent) && !task_on_rq_migrating(p))
		SCX_CALL_OP_TASK(sch, quiescent, rq, p, deq_flags);

	if (deq_flags & SCX_DEQ_SLEEP)
		/* 睡眠原因跨到下一次 wakeup，用于 enqueue flags/统计区分真正阻塞与临时摘队。 */
		p->scx.flags |= SCX_TASK_DEQD_FOR_SLEEP;
	else
		p->scx.flags &= ~SCX_TASK_DEQD_FOR_SLEEP;

	p->scx.flags &= ~SCX_TASK_QUEUED;
	/* 先撤销逻辑 runnable 与计数，再摘 DSQ；全程 rq lock 阻止同 CPU 观察半状态。 */
	rq->scx.nr_running--;
	sub_nr_running(rq, 1);

	dispatch_dequeue(rq, p);
	clear_direct_dispatch(p);
	return true;
}

/*
 * 业务背景：当前 SCX donor 主动 yield 时把策略机会交给 BPF；未实现 yield op 时以
 * slice=0 请求下一调度点重新 dispatch。
 * 入参：rq 已锁，rq->donor 为借用当前调度实体。
 * 出参/返回：无；可能执行 BPF 双 task 回调或清 donor slice。
 * 注意事项：to=NULL 表示普通 yield，不承诺立即切换到某个具体 task。
 */
static void yield_task_scx(struct rq *rq)
{
	struct task_struct *p = rq->donor;
	struct scx_sched *sch = scx_task_sched(p);

	if (SCX_HAS_OP(sch, yield))
		SCX_CALL_OP_2TASKS_RET(sch, yield, rq, p, NULL);
	else
		p->scx.slice = 0;
}

/*
 * 业务背景：尝试把 CPU 定向让给 to，仅同一 scheduler 实例才能由其 BPF policy 仲裁。
 * 入参：rq 已锁；to 为生命周期/rq 协议稳定的借用目标；from 取 rq->donor。
 * 出参/返回：BPF 接受定向 yield 返回 true；无 op 或跨 scheduler 返回 false。
 * 注意事项：true 表示策略接受，不代表 to 已经在此函数内完成 context switch。
 */
static bool yield_to_task_scx(struct rq *rq, struct task_struct *to)
{
	struct task_struct *from = rq->donor;
	struct scx_sched *sch = scx_task_sched(from);

	if (SCX_HAS_OP(sch, yield) && sch == scx_task_sched(to))
		return SCX_CALL_OP_2TASKS_RET(sch, yield, rq, from, to);
	else
		return false;
}

/*
 * 业务背景：SCX 内部抢占由 verdict/slice 协议完成；本 hook 只处理更高 class 抢占
 * 导致 local DSQ 的 IMMED task 无法立即运行，需要重新交给策略选择。
 * 入参：rq 已锁；p 为正在唤醒的借用 task；wake_flags 在此路径无额外策略含义。
 * 出参/返回：无；必要时安排 local DSQ reenq。
 * 注意事项：p 本身属于 ext class 时直接返回，避免与 BPF 自己的抢占决定重复。
 */
static void wakeup_preempt_scx(struct rq *rq, struct task_struct *p, int wake_flags)
{
	/*
	 * Preemption between SCX tasks is implemented by resetting the victim
	 * task's slice to 0 and triggering reschedule on the target CPU.
	 * Nothing to do.
	 */
	/* SCX 对 SCX 的抢占不由通用 wakeup_preempt 决定，enqueue verdict 会清 victim slice。 */
	if (p->sched_class == &ext_sched_class)
		return;

	/*
	 * Getting preempted by a higher-priority class. Reenqueue IMMED tasks.
	 * This captures all preemption cases including:
	 *
	 * - A SCX task is currently running.
	 *
	 * - @rq is waking from idle due to a SCX task waking to it.
	 *
	 * - A higher-priority wakes up while SCX dispatch is in progress.
	 */
	/* nr_immed 聚合上述三个窗口，只要非零就延后重扫 local DSQ。 */
	if (rq->scx.nr_immed)
		schedule_reenq_local(rq, 0);
}

/*
 * 业务背景：task 的 rq 已等于目标 rq 且源 non-local DSQ 已摘链时，省略迁移事务，
 * 直接按头/尾语义提交到 local DSQ。
 * 入参：sch 为实例；p 为借用 task；enq_flags 为插入语义；src_dsq 已锁；dst_rq 已锁。
 * 出参/返回：无；p 进入目标 local DSQ并完成计数/custody/抢占后处理。
 * 注意事项：源 DSQ 摘链由调用者先完成；holding_cpu 此时必须为负。
 */
static void move_local_task_to_local_dsq(struct scx_sched *sch,
					 struct task_struct *p, u64 enq_flags,
					 struct scx_dispatch_q *src_dsq,
					 struct rq *dst_rq)
{
	struct scx_dispatch_q *dst_dsq = &dst_rq->scx.local_dsq;

	/* @dsq is locked and @p is on @dst_rq */
/* HEAD/PREEMPT 插队，其余保持 FIFO；rq 与源 DSQ 锁共同稳定 task 和链表。 */
	lockdep_assert_held(&src_dsq->lock);
	lockdep_assert_rq_held(dst_rq);

	WARN_ON_ONCE(p->scx.holding_cpu >= 0);

	/* HEAD/PREEMPT 插队，其余保持 FIFO；rq 与源 DSQ 锁共同稳定 task 和链表。 */
	if (enq_flags & (SCX_ENQ_HEAD | SCX_ENQ_PREEMPT))
		list_add(&p->scx.dsq_list.node, &dst_dsq->list);
	else
		list_add_tail(&p->scx.dsq_list.node, &dst_dsq->list);

	dsq_inc_nr(dst_dsq, p, enq_flags);
	p->scx.dsq = dst_dsq;

	local_dsq_post_enq(sch, dst_dsq, p, enq_flags);
}

/**
 * move_remote_task_to_local_dsq - Move a task from a foreign rq to a local DSQ
 * @p: task to move
 * @enq_flags: %SCX_ENQ_*
 * @src_rq: rq to move the task from, locked on entry, released on return
 * @dst_rq: rq to move the task into, locked on return
 *
 * Move @p which is currently on @src_rq to @dst_rq's local DSQ.
 */
/* 双 rq 锁下把远端 task 迁入目标 local DSQ；锁后再次检查 CPU 可运行条件。 */
/*
 * 上游契约：入口持 src_rq，返回持 dst_rq；deactivate→set_task_cpu→activate 完成 core
 * 可见迁移，sticky_cpu 把这一段标成 SCX 内部移动以抑制虚假 BPF 生命周期回调。
 * 入参：p 为源 rq 上借用 task；enq_flags 需穿透 core 32 位 flags；src/dst 为不同 rq。
 * 出参/返回：无；task CPU 和 rq membership 转到 dst，源锁释放、目标锁持有。
 * 注意事项：extra_enq_flags 是目标 rq 独占临时通道，activate 后必须清零。
 */
static void move_remote_task_to_local_dsq(struct task_struct *p, u64 enq_flags,
					  struct rq *src_rq, struct rq *dst_rq)
{
	lockdep_assert_rq_held(src_rq);

	/*
	 * Set sticky_cpu before deactivate_task() to properly mark the
	 * beginning of an SCX-internal migration.
	 */
/* sticky 必须早于 deactivate，使其内部 dequeue 跳过 ops.dequeue/quiescent。 */
	p->scx.sticky_cpu = cpu_of(dst_rq);
	/* sticky 必须早于 deactivate，使其内部 dequeue 跳过 ops.dequeue/quiescent。 */
	deactivate_task(src_rq, p, 0);
	set_task_cpu(p, cpu_of(dst_rq));

	switch_rq_lock(src_rq, dst_rq);
	/* set_task_cpu 后切锁；返回时 dst lock 稳定新归属，旧 rq 不再访问 p 的队列字段。 */

	/*
	 * We want to pass scx-specific enq_flags but activate_task() will
	 * truncate the upper 32 bit. As we own @rq, we can pass them through
	 * @rq->scx.extra_enq_flags instead.
	 */
/* core activate 参数会截断高 32 位，借 rq 私有字段在同一锁事务内完整传递 SCX 位。 */
	WARN_ON_ONCE(!cpumask_test_cpu(cpu_of(dst_rq), p->cpus_ptr));
	WARN_ON_ONCE(dst_rq->scx.extra_enq_flags);
	dst_rq->scx.extra_enq_flags = enq_flags;
	/* core activate 参数会截断高 32 位，借 rq 私有字段在同一锁事务内完整传递 SCX 位。 */
	activate_task(dst_rq, p, 0);
	dst_rq->scx.extra_enq_flags = 0;
}

/*
 * Similar to kernel/sched/core.c::is_cpu_allowed(). However, there are two
 * differences:
 *
 * - is_cpu_allowed() asks "Can this task run on this CPU?" while
 *   task_can_run_on_remote_rq() asks "Can the BPF scheduler migrate the task to
 *   this CPU?".
 *
 *   While migration is disabled, is_cpu_allowed() has to say "yes" as the task
 *   must be allowed to finish on the CPU that it's currently on regardless of
 *   the CPU state. However, task_can_run_on_remote_rq() must say "no" as the
 *   BPF scheduler shouldn't attempt to migrate a task which has migration
 *   disabled.
 *
 * - The BPF scheduler is bypassed while the rq is offline and we can always say
 *   no to the BPF scheduler initiated migrations while offline.
 *
 * The caller must ensure that @p and @rq are on different CPUs.
 * If enforce == true, caller must hold @p's rq lock.
 */
/*
 * 上游说明：它询问“BPF 能否主动迁移”，比 core is_cpu_allowed 更严格：migration
 * disabled 和 offline 均拒绝；enforce 决定策略违规是安静 fallback 还是 scheduler error。
 * 入参：sch 为错误归属；p 与 rq 必须位于不同 CPU；enforce=true 时持 p 当前 rq lock。
 * 出参/返回：亲和性、迁移状态和 SCX online 都允许时 true，否则 false。
 * 注意事项：先查 migration-disabled 以覆盖 put_prev 到 cpus_ptr 收窄之间的窄竞态。
 */
static bool task_can_run_on_remote_rq(struct scx_sched *sch,
				      struct task_struct *p, struct rq *rq,
				      bool enforce)
{
	s32 cpu = cpu_of(rq);

	/*
	 * To prevent races with @p still running on its old CPU while switching
	 * out, make sure we're holding @p's rq lock so as not to risk
	 * erroneously killing the BPF scheduler.
	 */
	/* 为避免 p 仍在旧 CPU 运行时并发换 rq，必须持其 rq 锁；远端运行 task 只能派到本地 DSQ。 */
	if (enforce)
		lockdep_assert_rq_held(task_rq(p));

	WARN_ON_ONCE(task_cpu(p) == cpu);

	/*
	 * If @p has migration disabled, @p->cpus_ptr is updated to contain only
	 * the pinned CPU in migrate_disable_switch() while @p is being switched
	 * out. However, put_prev_task_scx() is called before @p->cpus_ptr is
	 * updated and thus another CPU may see @p on a DSQ inbetween leading to
	 * @p passing the below task_allowed_on_cpu() check while migration is
	 * disabled.
	 *
	 * Test the migration disabled state first as the race window is narrow
	 * and the BPF scheduler failing to check migration disabled state can
	 * easily be masked if task_allowed_on_cpu() is done first.
	 */
/* 即使 cpus_ptr 尚未收窄，也禁止 BPF 利用竞态把 pinned task 搬走。 */
	if (unlikely(is_migration_disabled(p))) {
		/* 即使 cpus_ptr 尚未收窄，也禁止 BPF 利用竞态把 pinned task 搬走。 */
		if (enforce)
			scx_error(sch, "SCX_DSQ_LOCAL[_ON] cannot move migration disabled %s[%d] from CPU %d to %d",
				  p->comm, p->pid, task_cpu(p), cpu);
		return false;
	}

	/*
	 * We don't require the BPF scheduler to avoid dispatching to offline
	 * CPUs mostly for convenience but also because CPUs can go offline
	 * between scx_bpf_dsq_insert() calls and here. Trigger error iff the
	 * picked CPU is outside the allowed mask.
	 */
/* offline 本身可静默 fallback，但越过 cpus_allowed 是明确策略错误。 */
	if (!task_allowed_on_cpu(p, cpu)) {
		/* offline 本身可静默 fallback，但越过 cpus_allowed 是明确策略错误。 */
		if (enforce)
			scx_error(sch, "SCX_DSQ_LOCAL[_ON] target CPU %d not allowed for %s[%d]",
				  cpu, p->comm, p->pid);
		return false;
	}

	if (!scx_rq_online(rq)) {
		/* hotplug 可在 verdict 与提交间发生，因此只计事件而不杀掉 scheduler。 */
		if (enforce)
			__scx_add_event(sch, SCX_EV_DISPATCH_LOCAL_DSQ_OFFLINE, 1);
		return false;
	}

	return true;
}

/**
 * unlink_dsq_and_lock_src_rq() - Unlink task from its DSQ and lock its task_rq
 * @p: target task
 * @dsq: locked DSQ @p is currently on
 * @src_rq: rq @p is currently on, stable with @dsq locked
 *
 * Called with @dsq locked but no rq's locked. We want to move @p to a different
 * DSQ, including any local DSQ, but are not locking @src_rq. Locking @src_rq is
 * required when transferring into a local DSQ. Even when transferring into a
 * non-local DSQ, it's better to use the same mechanism to protect against
 * dequeues and maintain the invariant that @p->scx.dsq can only change while
 * @src_rq is locked, which e.g. scx_dump_task() depends on.
 *
 * We want to grab @src_rq but that can deadlock if we try while locking @dsq,
 * so we want to unlink @p from @dsq, drop its lock and then lock @src_rq. As
 * this may race with dequeue, which can't drop the rq lock or fail, do a little
 * dancing from our side.
 *
 * @p->scx.holding_cpu is set to this CPU before @dsq is unlocked. If @p gets
 * dequeued after we unlock @dsq but before locking @src_rq, the holding_cpu
 * would be cleared to -1. While other cpus may have updated it to different
 * values afterwards, as this operation can't be preempted or recurse, the
 * holding_cpu can never become this CPU again before we're done. Thus, we can
 * tell whether we lost to dequeue by testing whether the holding_cpu still
 * points to this CPU. See dispatch_dequeue() for the counterpart.
 *
 * On return, @dsq is unlocked and @src_rq is locked. Returns %true if @p is
 * still valid. %false if lost to dequeue.
 */
/* 为避免 dsq->rq 锁反序，先以 qseq claim task，再放 DSQ 锁并取得源 rq 锁。 */
/*
 * 上游契约：入口只持 non-local dsq lock；先摘链并把 holding_cpu 设为本 CPU，再放
 * DSQ 锁取 src_rq，借此避免 dsq→rq 与 dequeue 的 rq→dsq 锁反序。
 * 入参：p 必须仍在已锁 dsq；src_rq 是 dsq lock 下稳定读取的 task_rq。
 * 出参/返回：返回时 dsq 已解锁、src_rq 已锁；仍持 claim 返回 true，输给 dequeue 为 false。
 * 注意事项：当前操作不可抢占/递归，所以 holding_cpu 一旦被清就不会伪装回本 CPU。
 */
static bool unlink_dsq_and_lock_src_rq(struct task_struct *p,
				       struct scx_dispatch_q *dsq,
				       struct rq *src_rq)
{
	s32 cpu = raw_smp_processor_id();

	lockdep_assert_held(&dsq->lock);

	WARN_ON_ONCE(p->scx.holding_cpu >= 0);
	task_unlink_from_dsq(p, dsq);
	/* 摘链后先发布本 CPU claim；dequeue 取得 dsq lock 时只能清 claim，不能重复摘链。 */
	p->scx.holding_cpu = cpu;

	raw_spin_unlock(&dsq->lock);
	raw_spin_rq_lock(src_rq);

	/* task_rq couldn't have changed if we're still the holding cpu */
	/* claim 仍属本 CPU 才拥有 p；同时断言 src_rq 未在锁舞步中悄然改变。 */
	return likely(p->scx.holding_cpu == cpu) &&
		!WARN_ON_ONCE(src_rq != task_rq(p));
}

/*
 * 业务背景：当前 rq 消费远端 task 时先放自身锁，按 DSQ→src rq claim 协议取得 task，
 * 再执行完整迁移并让返回锁重新落到 this_rq。
 * 入参：this_rq 与 dsq 已锁；p/src_rq 为 dsq 下稳定借用；enq_flags 为 local 插入语义。
 * 出参/返回：迁移成功 true 且 this_rq 持锁；竞态失败 false 且同样恢复 this_rq 锁。
 * 注意事项：失败路径必须释放 src_rq 后再重取 this_rq，保持统一锁后置条件。
 */
static bool consume_remote_task(struct rq *this_rq,
				struct task_struct *p, u64 enq_flags,
				struct scx_dispatch_q *dsq, struct rq *src_rq)
{
	raw_spin_rq_unlock(this_rq);

	/* 先释放当前 rq 再接管源 rq，避免双 rq 锁反序；失败后恢复调用者的 this_rq 锁。 */
	if (unlink_dsq_and_lock_src_rq(p, dsq, src_rq)) {
		move_remote_task_to_local_dsq(p, enq_flags, src_rq, this_rq);
		return true;
	} else {
		raw_spin_rq_unlock(src_rq);
		raw_spin_rq_lock(this_rq);
		return false;
	}
}

/**
 * move_task_between_dsqs() - Move a task from one DSQ to another
 * @sch: scx_sched being operated on
 * @p: target task
 * @enq_flags: %SCX_ENQ_*
 * @src_dsq: DSQ @p is currently on, must not be a local DSQ
 * @dst_dsq: DSQ @p is being moved to, can be any DSQ
 *
 * Must be called with @p's task_rq and @src_dsq locked. If @dst_dsq is a local
 * DSQ and @p is on a different CPU, @p will be migrated and thus its task_rq
 * will change. As @p's task_rq is locked, this function doesn't need to use the
 * holding_cpu mechanism.
 *
 * On return, @src_dsq is unlocked and only @p's new task_rq, which is the
 * return value, is locked.
 */
/* 在源 DSQ 与 task rq 所有权明确时完成移动；返回时仅新 task_rq 保持加锁。 */
/*
 * 上游契约：入口持 p 当前 rq 与非 local src_dsq；目标可为任意 DSQ。远端 local 会
 * 迁移 task 并改变 task_rq，返回值明确告诉调用者当前仍持哪条 rq lock。
 * 入参：sch 为实例；p 为已 claim task；enq_flags 为目标插入语义；src/dst 为借用 DSQ。
 * 出参/返回：返回 p 新 rq 且保持加锁；src_dsq 总在返回前解锁。
 * 注意事项：不允许 local 作为源；非法远端目标退 global 并标 GDSQ_FALLBACK。
 */
static struct rq *move_task_between_dsqs(struct scx_sched *sch,
					 struct task_struct *p, u64 enq_flags,
					 struct scx_dispatch_q *src_dsq,
					 struct scx_dispatch_q *dst_dsq)
{
	struct rq *src_rq = task_rq(p), *dst_rq;

	BUG_ON(src_dsq->id == SCX_DSQ_LOCAL);
	lockdep_assert_held(&src_dsq->lock);
	lockdep_assert_rq_held(src_rq);

	if (dst_dsq->id == SCX_DSQ_LOCAL) {
		/* local 目标决定是否需要真正跨 rq；亲和/online 失败改投源 CPU global。 */
		dst_rq = container_of(dst_dsq, struct rq, scx.local_dsq);
		if (src_rq != dst_rq &&
		    unlikely(!task_can_run_on_remote_rq(sch, p, dst_rq, true))) {
			dst_dsq = find_global_dsq(sch, task_cpu(p));
			dst_rq = src_rq;
			enq_flags |= SCX_ENQ_GDSQ_FALLBACK;
		}
	} else {
		/* no need to migrate if destination is a non-local DSQ */
		/* non-local DSQ 不绑定执行 CPU，task_rq 保持不变，仅更换 custody 队列。 */
		dst_rq = src_rq;
	}

	/*
	 * Move @p into $dst_dsq. If $dst_dsq is the local DSQ of a different
	 * CPU, @p will be migrated.
	 */
	/* 把 p 移入 dst_dsq；若目标是另一 CPU 的 local DSQ，这一步同时执行 task 迁移。 */
	if (dst_dsq->id == SCX_DSQ_LOCAL) {
		/* @p is going from a non-local DSQ to a local DSQ */
/* 同 rq 只做两条 DSQ 间摘挂，避免 deactivate/activate 产生伪生命周期事件。 */
		if (src_rq == dst_rq) {
			/* 同 rq 只做两条 DSQ 间摘挂，避免 deactivate/activate 产生伪生命周期事件。 */
			task_unlink_from_dsq(p, src_dsq);
			move_local_task_to_local_dsq(sch, p, enq_flags,
						     src_dsq, dst_rq);
			raw_spin_unlock(&src_dsq->lock);
		} else {
			/* 先放 src_dsq，再由迁移 helper 按 rq 锁序切到目标 CPU。 */
			raw_spin_unlock(&src_dsq->lock);
			move_remote_task_to_local_dsq(p, enq_flags,
						      src_rq, dst_rq);
		}
	} else {
		/*
		 * @p is going from a non-local DSQ to a non-local DSQ. As
		 * $src_dsq is already locked, do an abbreviated dequeue.
		 */
		/* 两个 non-local 队列不能同时任意加锁；先完整离开源，再由 enqueue 锁目标。 */
		dispatch_dequeue_locked(p, src_dsq);
		raw_spin_unlock(&src_dsq->lock);

		dispatch_enqueue(sch, dst_rq, dst_dsq, p, enq_flags);
	}

	return dst_rq;
}

/* 从非本地 DSQ claim 首个可运行 task 并迁入当前 local DSQ；空/竞态返回 false。 */
/*
 * 业务背景：CPU 从 global/user/bypass DSQ 拉取第一个能在本 CPU 运行的 task；同 rq
 * 直接搬 local，远端 rq 通过 holding_cpu 锁舞步迁移。
 * 入参：sch 为实例；rq 已锁；dsq 为 non-local 借用队列；enq_flags 为 local 插入语义。
 * 出参/返回：成功消费一个 task 为 true；空、无兼容 task、aborting 或竞态失败为 false。
 * 注意事项：无锁 list_empty 仅是优化；真正选择在 dsq lock 下，远端竞态失败从头重试。
 */
static bool consume_dispatch_q(struct scx_sched *sch, struct rq *rq,
			       struct scx_dispatch_q *dsq, u64 enq_flags)
{
	struct task_struct *p;
retry:
	/*
	 * The caller can't expect to successfully consume a task if the task's
	 * addition to @dsq isn't guaranteed to be visible somehow. Test
	 * @dsq->list without locking and skip if it seems empty.
	 */
/* 假阴性只延后一次消费，调用者不能据此推导 DSQ 的同步空状态。 */
	if (list_empty(&dsq->list))
		/* 假阴性只延后一次消费，调用者不能据此推导 DSQ 的同步空状态。 */
		return false;

	raw_spin_lock(&dsq->lock);

	nldsq_for_each_task(p, dsq) {
		struct rq *task_rq = task_rq(p);

		/*
		 * This loop can lead to multiple lockup scenarios, e.g. the BPF
		 * scheduler can put an enormous number of affinitized tasks into
		 * a contended DSQ, or the outer retry loop can repeatedly race
		 * against scx_bypass() dequeueing tasks from @dsq trying to put
		 * the system into the bypass mode. This can easily live-lock the
		 * machine. If aborting, exit from all non-bypass DSQs.
		 */
/* 退出时停止可能无界的普通 DSQ 扫描，只保留内核 bypass 队列保证前进。 */
		if (unlikely(READ_ONCE(sch->aborting)) && dsq->id != SCX_DSQ_BYPASS)
			/* 退出时停止可能无界的普通 DSQ 扫描，只保留内核 bypass 队列保证前进。 */
			break;

		if (rq == task_rq) {
			/* task 已属于当前 rq，无需放 rq lock，锁内摘源并挂 local。 */
			task_unlink_from_dsq(p, dsq);
			move_local_task_to_local_dsq(sch, p, enq_flags, dsq, rq);
			raw_spin_unlock(&dsq->lock);
			return true;
		}

		if (task_can_run_on_remote_rq(sch, p, rq, false)) {
			/* 筛选阶段 enforce=false：不合适 task 可留给其他 CPU，不应把策略判死。 */
			if (likely(consume_remote_task(rq, p, enq_flags, dsq, task_rq)))
				return true;
			goto retry;
		}
	}

	raw_spin_unlock(&dsq->lock);
	return false;
}

/* 根据当前 CPU NUMA node 选择 scheduler global DSQ，并复用通用 consume；返回是否拉到 task。 */
static bool consume_global_dsq(struct scx_sched *sch, struct rq *rq)
{
	int node = cpu_to_node(cpu_of(rq));

	return consume_dispatch_q(sch, rq, &sch->pnode[node]->global_dsq, 0);
}

/**
 * dispatch_to_local_dsq - Dispatch a task to a local dsq
 * @sch: scx_sched being operated on
 * @rq: current rq which is locked
 * @dst_dsq: destination DSQ
 * @p: task to dispatch
 * @enq_flags: %SCX_ENQ_*
 *
 * We're holding @rq lock and want to dispatch @p to @dst_dsq which is a local
 * DSQ. This function performs all the synchronization dancing needed because
 * local DSQs are protected with rq locks.
 *
 * The caller must have exclusive ownership of @p (e.g. through
 * %SCX_OPSS_DISPATCHING).
 */
/* 处理同 CPU、远端和正在 QUEUEING 三类目标，必要时等待发布再重新 claim。 */
/*
 * 上游契约：调用者持当前 rq lock 且独占 p（通常 DISPATCHING）；目标 local DSQ 由
 * 另一 rq lock 保护，本函数完成 holding_cpu 发布、放 ownership、切 rq 锁和迁移/fallback。
 * 入参：sch 为实例；rq 为调用者必须恢复的当前 rq；dst_dsq 为 local 目标；p 为独占
 * task；enq_flags 为 dispatch 语义。
 * 出参/返回：无；成功入目标 local，非法迁移退源 CPU global，输给 dequeue 则不再入队。
 * 注意事项：返回前总恢复 rq lock；release 清 DISPATCHING 后不得再假定 dequeue 不会介入。
 */
static void dispatch_to_local_dsq(struct scx_sched *sch, struct rq *rq,
				  struct scx_dispatch_q *dst_dsq,
				  struct task_struct *p, u64 enq_flags)
{
	struct rq *src_rq = task_rq(p);
	struct rq *dst_rq = container_of(dst_dsq, struct rq, scx.local_dsq);
	struct rq *locked_rq = rq;

	/*
	 * We're synchronized against dequeue through DISPATCHING. As @p can't
	 * be dequeued, its task_rq and cpus_allowed are stable too.
	 *
	 * If dispatching to @rq that @p is already on, no lock dancing needed.
	 */
/* 三者同 rq 时现有锁已覆盖目标队列，直接以 CLEAR_OPSS 原子提交。 */
	if (rq == src_rq && rq == dst_rq) {
		/* 三者同 rq 时现有锁已覆盖目标队列，直接以 CLEAR_OPSS 原子提交。 */
		dispatch_enqueue(sch, rq, dst_dsq, p,
				 enq_flags | SCX_ENQ_CLEAR_OPSS);
		return;
	}

	/*
	 * @p is on a possibly remote @src_rq which we need to lock to move the
	 * task. If dequeue is in progress, it'd be locking @src_rq and waiting
	 * on DISPATCHING, so we can't grab @src_rq lock while holding
	 * DISPATCHING.
	 *
	 * As DISPATCHING guarantees that @p is wholly ours, we can pretend that
	 * we're moving from a DSQ and use the same mechanism - mark the task
	 * under transfer with holding_cpu, release DISPATCHING and then follow
	 * the same protocol. See unlink_dsq_and_lock_src_rq().
	 */
/* holding_cpu 在释放 DISPATCHING 前发布临时 claim，供 dequeue 决定谁负责最终清理。 */
	p->scx.holding_cpu = raw_smp_processor_id();
	/* holding_cpu 在释放 DISPATCHING 前发布临时 claim，供 dequeue 决定谁负责最终清理。 */

	/* store_release ensures that dequeue sees the above */
/* release 让 rq-lock waiter 先看到 claim；从此本路径可能输给并发 dequeue。 */
	atomic_long_set_release(&p->scx.ops_state, SCX_OPSS_NONE);
	/* release 让 rq-lock waiter 先看到 claim；从此本路径可能输给并发 dequeue。 */

	/* switch to @src_rq lock */
/* 统一 switch helper 同步 kfunc locked-rq 追踪，切锁后必须复验 claim 与 task_rq。 */
	if (locked_rq != src_rq) {
		/* 统一 switch helper 同步 kfunc locked-rq 追踪，切锁后必须复验 claim 与 task_rq。 */
		switch_rq_lock(locked_rq, src_rq);
		locked_rq = src_rq;
	}

	/* task_rq couldn't have changed if we're still the holding cpu */
	/* holding_cpu 仍指向当前 CPU 时，其他搬运者不能改变 task_rq；不一致意味着协议已损坏。 */
	if (likely(p->scx.holding_cpu == raw_smp_processor_id()) &&
	    !WARN_ON_ONCE(src_rq != task_rq(p))) {
		bool fallback = false;
		/*
		 * If @p is staying on the same rq, there's no need to go
		 * through the full deactivate/activate cycle. Optimize by
		 * abbreviating move_remote_task_to_local_dsq().
		 */
/* 目标就是 task 当前 rq：撤 claim 后直接挂 local，无需 core 迁移。 */
		if (src_rq == dst_rq) {
			/* 目标就是 task 当前 rq：撤 claim 后直接挂 local，无需 core 迁移。 */
			p->scx.holding_cpu = -1;
			dispatch_enqueue(sch, dst_rq, &dst_rq->scx.local_dsq, p,
					 enq_flags);
		} else if (unlikely(!task_can_run_on_remote_rq(sch, p, dst_rq, true))) {
			/* verdict 已提交阶段 enforce=true；非法亲和登记错误并退源节点 global。 */
			p->scx.holding_cpu = -1;
			fallback = true;
			dispatch_enqueue(sch, src_rq, find_global_dsq(sch, task_cpu(p)),
					 p, enq_flags | SCX_ENQ_GDSQ_FALLBACK);
		} else {
			/* helper 返回时锁已从 src 切到 dst，记下 locked_rq 供尾部恢复调用者 rq。 */
			move_remote_task_to_local_dsq(p, enq_flags,
						      src_rq, dst_rq);
			/* task has been moved to dst_rq, which is now locked */
			/* p 已迁到 dst_rq，helper 返回时锁所有权也切换到该目标 rq。 */
			locked_rq = dst_rq;
		}

		/* if the destination CPU is idle, wake it up */
/* 只在目标 task class 更高时发 resched；fallback 未向 dst 发布任何 task。 */
		if (!fallback && sched_class_above(p->sched_class, dst_rq->curr->sched_class))
			/* 只在目标 task class 更高时发 resched；fallback 未向 dst 发布任何 task。 */
			resched_curr(dst_rq);
	}

	/* switch back to @rq lock */
/* 无论成功、fallback 或竞态失败，API 后置条件都要求原 rq lock 重新持有。 */
	if (locked_rq != rq)
		/* 无论成功、fallback 或竞态失败，API 后置条件都要求原 rq lock 重新持有。 */
		switch_rq_lock(locked_rq, rq);
}

/**
 * finish_dispatch - Asynchronously finish dispatching a task
 * @rq: current rq which is locked
 * @p: task to finish dispatching
 * @qseq_at_dispatch: qseq when @p started getting dispatched
 * @dsq_id: destination DSQ ID
 * @enq_flags: %SCX_ENQ_*
 *
 * Dispatching to local DSQs may need to wait for queueing to complete or
 * require rq lock dancing. As we don't wanna do either while inside
 * ops.dispatch() to avoid locking order inversion, we split dispatching into
 * two parts. scx_bpf_dsq_insert() which is called by ops.dispatch() records the
 * task and its qseq. Once ops.dispatch() returns, this function is called to
 * finish up.
 *
 * There is no guarantee that @p is still valid for dispatching or even that it
 * was valid in the first place. Make sure that the task is still owned by the
 * BPF scheduler and claim the ownership before dispatching.
 */
/* qseq 匹配才把 DISPATCHING task 提交到目标；失配说明所有权已变化并放弃。 */
/*
 * 上游契约：ops.dispatch 内只记录 p/qseq/verdict，回调返回后本函数异步 claim task，
 * 避免在 BPF 回调锁上下文等待 QUEUEING 或执行 rq 锁舞步。
 * 入参：sch 为实例；rq 已锁；p 为可能已失效 verdict 的借用 task；qseq_at_dispatch、
 * dsq_id、enq_flags 是记录时快照。
 * 出参/返回：无；仅 qseq/归属匹配并 cmpxchg 成 DISPATCHING 的 task 会提交目标 DSQ。
 * 注意事项：任何 NONE/DISPATCHING/代际失配都静默放弃；local 目标交给专用锁舞步。
 */
static void finish_dispatch(struct scx_sched *sch, struct rq *rq,
			    struct task_struct *p,
			    unsigned long qseq_at_dispatch,
			    u64 dsq_id, u64 enq_flags)
{
	struct scx_dispatch_q *dsq;
	unsigned long opss;

	touch_core_sched_dispatch(rq, p);
retry:
	/*
	 * No need for _acquire here. @p is accessed only after a successful
	 * try_cmpxchg to DISPATCHING.
	 */
/* 初读无需 acquire；只有成功 cmpxchg claim 后，原子操作才建立后续访问授权。 */
	opss = atomic_long_read(&p->scx.ops_state);
	/* 初读无需 acquire；只有成功 cmpxchg claim 后，原子操作才建立后续访问授权。 */

	switch (opss & SCX_OPSS_STATE_MASK) {
	case SCX_OPSS_DISPATCHING:
	case SCX_OPSS_NONE:
		/* someone else already got to it */
		/* 已被另一 verdict claim 或已 dequeue/提交，当前旧记录不再拥有任何权利。 */
		return;
	case SCX_OPSS_QUEUED:
		/*
		 * If qseq doesn't match, @p has gone through at least one
		 * dispatch/dequeue and re-enqueue cycle between
		 * scx_bpf_dsq_insert() and here and we have no claim on it.
		 */
/* 同一 task 已完成至少一次出队再入队，旧 qseq 绝不能调度新一代 custody。 */
		if ((opss & SCX_OPSS_QSEQ_MASK) != qseq_at_dispatch)
			/* 同一 task 已完成至少一次出队再入队，旧 qseq 绝不能调度新一代 custody。 */
			return;

		/* see SCX_EV_INSERT_NOT_OWNED definition */
/* sub-scheduler 归属已变化时记录 NOT_OWNED，不越界消费其他实例的 task。 */
		if (unlikely(!scx_task_on_sched(sch, p))) {
			/* sub-scheduler 归属已变化时记录 NOT_OWNED，不越界消费其他实例的 task。 */
			__scx_add_event(sch, SCX_EV_INSERT_NOT_OWNED, 1);
			return;
		}

		/*
		 * While we know @p is accessible, we don't yet have a claim on
		 * it - the BPF scheduler is allowed to dispatch tasks
		 * spuriously and there can be a racing dequeue attempt. Let's
		 * claim @p by atomically transitioning it from QUEUED to
		 * DISPATCHING.
		 */
		/* cmpxchg 是 custody 领取点：只有一个 CPU 能把这一代 QUEUED 改成 DISPATCHING。 */
		if (likely(atomic_long_try_cmpxchg(&p->scx.ops_state, &opss,
						   SCX_OPSS_DISPATCHING)))
			break;
		goto retry;
	case SCX_OPSS_QUEUEING:
		/*
		 * do_enqueue_task() is in the process of transferring the task
		 * to the BPF scheduler while holding @p's rq lock. As we aren't
		 * holding any kernel or BPF resource that the enqueue path may
		 * depend upon, it's safe to wait.
		 */
		/* 当前仅持 dispatch rq 资源而 enqueue 持 p 自己 rq；协议保证这里等待不会成环。 */
		wait_ops_state(p, opss);
		goto retry;
	}

	BUG_ON(!(p->scx.flags & SCX_TASK_QUEUED));

	dsq = find_dsq_for_dispatch(sch, this_rq(), dsq_id, task_cpu(p));
	/* claim 后才解析 verdict，确保 task CPU/归属处于本次 DISPATCHING 独占窗口。 */

	if (dsq->id == SCX_DSQ_LOCAL)
		dispatch_to_local_dsq(sch, rq, dsq, p, enq_flags);
	else
		dispatch_enqueue(sch, rq, dsq, p, enq_flags | SCX_ENQ_CLEAR_OPSS);
}

/* 逐项完成 BPF buffered dispatch，并无条件清空本轮 buffer/context。 */
/*
 * 业务背景：ops.dispatch 返回后消费 per-CPU verdict buffer；每项独立用 qseq 复验，
 * 因而某个失效 task 不会阻塞同批其他 task。
 * 入参：sch 为本次回调实例；rq 已锁且为 dsp_ctx 绑定 rq。
 * 出参/返回：无；尝试提交 cursor 个 verdict，累计 nr_tasks 后把 cursor 清零。
 * 注意事项：finish_dispatch 可能执行 rq 锁舞步；buffer 条目只在 cursor 清零前有效。
 */
static void flush_dispatch_buf(struct scx_sched *sch, struct rq *rq)
{
	struct scx_dsp_ctx *dspc = &this_cpu_ptr(sch->pcpu)->dsp_ctx;
	u32 u;

	for (u = 0; u < dspc->cursor; u++) {
		/* 条目保存 task 借用指针和记录时 qseq，真正所有权由 finish_dispatch 原子复验。 */
		struct scx_dsp_buf_ent *ent = &dspc->buf[u];

		finish_dispatch(sch, rq, ent->task, ent->qseq, ent->dsq_id,
				ent->enq_flags);
	}

	dspc->nr_tasks += dspc->cursor;
	/* nr_tasks 表示 BPF 本轮产生 verdict 数，不等于成功进入 local DSQ 的 task 数。 */
	dspc->cursor = 0;
}

/*
 * 业务背景：balance 中只置 pending，离开危险锁舞步后才把 deferred callback 挂到 rq。
 * 入参：rq 已锁；pending 位由 schedule_deferred_locked 设置。
 * 出参/返回：无；若 pending 则登记一次 callback 并清标志，否则空操作。
 * 注意事项：queue 后 callback 由 core 在合适解锁边界调用，可 unpin/临时放 rq lock。
 */
static inline void maybe_queue_balance_callback(struct rq *rq)
{
	lockdep_assert_rq_held(rq);

	if (!(rq->scx.flags & SCX_RQ_BAL_CB_PENDING))
		return;

	queue_balance_callback(rq, &rq->scx.deferred_bal_cb,
				deferred_bal_cb_workfn);

	rq->scx.flags &= ~SCX_RQ_BAL_CB_PENDING;
}

/*
 * One user of this function is scx_bpf_dispatch() which can be called
 * recursively as sub-sched dispatches nest. Always inline to reduce stack usage
 * from the call frame.
 */
/*
 * 上游说明：sub-scheduler 可递归 dispatch，强制内联省去每层调用栈。函数先消费 global/
 * bypass，再循环调用 BPF dispatch；每轮 flush 后复查 prev、local、global，避免锁舞步
 * 使“BPF 产出 verdict”与“本 rq 得到可运行 task”脱节。
 * 入参：sch 为当前/嵌套实例；rq 已锁；prev 为借用前驱；nested 标识是否保存外层 prev。
 * 出参/返回：找到可运行 task 或保留 prev 为 true；无结果/循环让出为 false。
 * 注意事项：循环受 SCX_DSP_MAX_LOOPS 限制，超限 kick 下一调度周期让 watchdog 得到机会。
 */
static __always_inline bool
scx_dispatch_sched(struct scx_sched *sch, struct rq *rq,
		   struct task_struct *prev, bool nested)
{
	struct scx_dsp_ctx *dspc = &this_cpu_ptr(sch->pcpu)->dsp_ctx;
	int nr_loops = SCX_DSP_MAX_LOOPS;
	s32 cpu = cpu_of(rq);
	bool prev_on_sch = (prev->sched_class == &ext_sched_class) &&
		scx_task_on_sched(sch, prev);

	if (consume_global_dsq(sch, rq))
		/* global 是内核/BPF 公共快速路径，先消费可避免无谓进入 BPF 回调。 */
		return true;

	if (bypass_dsp_enabled(sch)) {
		/* if @sch is bypassing, only the bypass DSQs are active */
/* scheduler 自身退化时只运行本 CPU bypass DSQ，彻底绕开失效 ops。 */
		if (scx_bypassing(sch, cpu))
			/* scheduler 自身退化时只运行本 CPU bypass DSQ，彻底绕开失效 ops。 */
			return consume_dispatch_q(sch, rq, bypass_dsq(sch, cpu), 0);

#ifdef CONFIG_EXT_SUB_SCHED
		/*
		 * If @sch isn't bypassing but its children are, @sch is
		 * responsible for making forward progress for both its own
		 * tasks that aren't bypassing and the bypassing descendants'
		 * tasks. The following implements a simple built-in behavior -
		 * let each CPU try to run the bypass DSQ every Nth time.
		 *
		 * Later, if necessary, we can add an ops flag to suppress the
		 * auto-consumption and a kfunc to consume the bypass DSQ and,
		 * so that the BPF scheduler can fully control scheduling of
		 * bypassed tasks.
		 */
		/* host 每 N 次强制尝试后代 bypass DSQ，在自身策略吞吐与后代前进间取舍。 */
		struct scx_sched_pcpu *pcpu = per_cpu_ptr(sch->pcpu, cpu);

		if (!(pcpu->bypass_host_seq++ % SCX_BYPASS_HOST_NTH) &&
		    consume_dispatch_q(sch, rq, bypass_dsq(sch, cpu), 0)) {
			__scx_add_event(sch, SCX_EV_SUB_BYPASS_DISPATCH, 1);
			return true;
		}
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */
	}

	if (unlikely(!SCX_HAS_OP(sch, dispatch)) || !scx_rq_online(rq))
		/* 无 dispatch op 或 BPF 已知 CPU offline 时不能创建新 verdict。 */
		return false;

	dspc->rq = rq;
	/* dsp_ctx 是本 CPU 回调上下文，kfunc 通过它校验当前 rq 与剩余 buffer 槽。 */

	/*
	 * The dispatch loop. Because flush_dispatch_buf() may drop the rq lock,
	 * the local DSQ might still end up empty after a successful
	 * ops.dispatch(). If the local DSQ is empty even after ops.dispatch()
	 * produced some tasks, retry. The BPF scheduler may depend on this
	 * looping behavior to simplify its implementation.
	 */
/* 每轮清产出计数；flush 后若 verdict 都失效且无队列 task，才决定是否重试。 */
	do {
		dspc->nr_tasks = 0;
		/* 每轮清产出计数；flush 后若 verdict 都失效且无队列 task，才决定是否重试。 */

		if (nested) {
			/* 嵌套调用沿用外层保存的 prev，不覆盖 rq->sub_dispatch_prev。 */
			SCX_CALL_OP(sch, dispatch, rq, scx_cpu_arg(cpu),
				    prev_on_sch ? prev : NULL);
		} else {
			/* stash @prev so that nested invocations can access it */
			/* 外层只在回调窗口发布 prev，返回立即清空，禁止 kfunc 跨回调保存裸指针。 */
			rq->scx.sub_dispatch_prev = prev;
			SCX_CALL_OP(sch, dispatch, rq, scx_cpu_arg(cpu),
				    prev_on_sch ? prev : NULL);
			rq->scx.sub_dispatch_prev = NULL;
		}

		flush_dispatch_buf(sch, rq);

		if ((prev->scx.flags & SCX_TASK_QUEUED) && prev->scx.slice) {
			/* BPF/flush 可能延长 prev slice；以 BAL_KEEP 让 pick 继续运行它。 */
			rq->scx.flags |= SCX_RQ_BAL_KEEP;
			return true;
		}
		if (rq->scx.local_dsq.nr)
			/* local DSQ 是本 CPU 最终可执行提交队列，非空即可结束 dispatch。 */
			return true;
		if (consume_global_dsq(sch, rq))
			return true;

		/*
		 * ops.dispatch() can trap us in this loop by repeatedly
		 * dispatching ineligible tasks. Break out once in a while to
		 * allow the watchdog to run. As IRQ can't be enabled in
		 * balance(), we want to complete this scheduling cycle and then
		 * start a new one. IOW, we want to call resched_curr() on the
		 * next, most likely idle, task, not the current one. Use
		 * __scx_bpf_kick_cpu() for deferred kicking.
		 */
/* 延迟 kick 作用于下一任务，当前 balance 不能开 IRQ 让 watchdog 立即运行。 */
		if (unlikely(!--nr_loops)) {
			/* 延迟 kick 作用于下一任务，当前 balance 不能开 IRQ 让 watchdog 立即运行。 */
			scx_kick_cpu(sch, cpu, 0);
			break;
		}
	} while (dspc->nr_tasks);

	/*
	 * Prevent the CPU from going idle while bypassed descendants have tasks
	 * queued. Without this fallback, bypassed tasks could stall if the host
	 * scheduler's ops.dispatch() doesn't yield any tasks.
	 */
/* host ops 没产出时再兜底一次，防止后代 bypass task 因取模机会错过而停滞。 */
	if (bypass_dsp_enabled(sch))
		/* host ops 没产出时再兜底一次，防止后代 bypass task 因取模机会错过而停滞。 */
		return consume_dispatch_q(sch, rq, bypass_dsq(sch, cpu), 0);

	return false;
}

/* 尝试 local/global/BPF dispatch 直至获得任务或达到循环上限；错误时转 bypass。 */
/*
 * 业务背景：core balance hook 为当前 CPU 准备下一 SCX task；先处理 CPU control 归还，
 * 再决定保留 prev、消费 local/global/bypass 或调用 BPF dispatch。
 * 入参：rq 已锁但 balance 回调可安排稍后 unpin；prev 为借用前驱 task。
 * 出参/返回：有 task/BAL_KEEP 返回 true，无可运行结果返回 false。
 * 注意事项：IN_BALANCE 覆盖整个事务；所有出口必须清它，IMMED 多任务需安排重新入队。
 */
static int balance_one(struct rq *rq, struct task_struct *prev)
{
	struct scx_sched *sch = scx_root;
	s32 cpu = cpu_of(rq);

	lockdep_assert_rq_held(rq);
	rq->scx.flags |= SCX_RQ_IN_BALANCE;
	/* 先清上轮 KEEP，当前事务只有明确证明 prev/队列可用后才能重新设置。 */
	rq->scx.flags &= ~SCX_RQ_BAL_KEEP;

	if ((sch->ops.flags & SCX_OPS_HAS_CPU_PREEMPT) &&
	    unlikely(rq->scx.cpu_released)) {
		/*
		 * If the previous sched_class for the current CPU was not SCX,
		 * notify the BPF scheduler that it again has control of the
		 * core. This callback complements ->cpu_release(), which is
		 * emitted in switch_class().
		 */
		/* higher class 释放 CPU 后第一次回到 balance，成对通知 BPF 重新获得控制权。 */
		if (sch->ops.cpu_acquire)
			SCX_CALL_OP(sch, cpu_acquire, rq, cpu, NULL);
		rq->scx.cpu_released = false;
	}

	if (prev->sched_class == &ext_sched_class) {
		/* 先结算执行时间，后续 slice 判断必须基于当前时刻而不是旧余额。 */
		update_curr_scx(rq);

		/*
		 * If @prev is runnable & has slice left, it has priority and
		 * fetching more just increases latency for the fetched tasks.
		 * Tell pick_task_scx() to keep running @prev. If the BPF
		 * scheduler wants to handle this explicitly, it should
		 * implement ->cpu_release().
		 *
		 * See scx_disable_workfn() for the explanation on the bypassing
		 * test.
		 */
/* 非 bypass 且仍 runnable/有 slice 时保留 prev，避免拉取 task 后徒增其等待延迟。 */
		if ((prev->scx.flags & SCX_TASK_QUEUED) && prev->scx.slice &&
		    !scx_bypassing(sch, cpu)) {
			rq->scx.flags |= SCX_RQ_BAL_KEEP;
			/* 非 bypass 且仍 runnable/有 slice 时保留 prev，避免拉取 task 后徒增其等待延迟。 */
			goto has_tasks;
		}
	}

	/* if there already are tasks to run, nothing to do */
/* local 已有提交 task，直接进入 pick，无需询问 BPF。 */
	if (rq->scx.local_dsq.nr)
		/* local 已有提交 task，直接进入 pick，无需询问 BPF。 */
		goto has_tasks;

	if (scx_dispatch_sched(sch, rq, prev, false))
		goto has_tasks;

	/*
	 * Didn't find another task to run. Keep running @prev unless
	 * %SCX_OPS_ENQ_LAST is in effect.
	 */
/* 无其他 task 时默认保留 prev；ENQ_LAST 要求 BPF 显式重新 enqueue 才例外。 */
	if ((prev->scx.flags & SCX_TASK_QUEUED) &&
	    (!(sch->ops.flags & SCX_OPS_ENQ_LAST) || scx_bypassing(sch, cpu))) {
		rq->scx.flags |= SCX_RQ_BAL_KEEP;
		/* 无其他 task 时默认保留 prev；ENQ_LAST 要求 BPF 显式重新 enqueue 才例外。 */
		__scx_add_event(sch, SCX_EV_DISPATCH_KEEP_LAST, 1);
		goto has_tasks;
	}
	rq->scx.flags &= ~SCX_RQ_IN_BALANCE;
	return false;

has_tasks:
	/*
	 * @rq may have extra IMMED tasks without reenq scheduled:
	 *
	 * - rq_is_open() can't reliably tell when and how slice is going to be
	 *   modified for $curr and allows IMMED tasks to be queued while
	 *   dispatch is in progress.
	 *
	 * - A non-IMMED HEAD task can get queued in front of an IMMED task
	 *   between the IMMED queueing and the subsequent scheduling event.
	 */
/* IMMED 可能被 HEAD task 或 dispatch 中间态挡住，异步重扫恢复“立即”承诺。 */
	if (unlikely(rq->scx.local_dsq.nr > 1 && rq->scx.nr_immed))
		/* IMMED 可能被 HEAD task 或 dispatch 中间态挡住，异步重扫恢复“立即”承诺。 */
		schedule_reenq_local(rq, 0);

	rq->scx.flags &= ~SCX_RQ_IN_BALANCE;
	return true;
}

/*
 * 业务背景：p 被 core 选中执行时完成 DSQ/custody 撤销、running 回调、exec 时钟和
 * NOHZ tick 依赖切换；core-sched 可绕过正常 dispatch 直接选中 QUEUED task。
 * 入参：rq 已锁；p 为即将运行的借用 task；first 由通用 class 接口提供但本实现无需区分。
 * 出参/返回：无；p 离开 runnable/DSQ，成为 rq 当前执行上下文，可能改变 tick dependency。
 * 注意事项：只有仍 QUEUED 才发 running；无限 slice 可停 tick，有限 slice 必须保留调度 tick。
 */
static void set_next_task_scx(struct rq *rq, struct task_struct *p, bool first)
{
	struct scx_sched *sch = scx_task_sched(p);

	if (p->scx.flags & SCX_TASK_QUEUED) {
		/*
		 * Core-sched might decide to execute @p before it is
		 * dispatched. Call ops_dequeue() to notify the BPF scheduler.
		 */
		/* core-sched 直接执行仍在 custody/DSQ 的 p，先按 CORE_SCHED_EXEC 收回再摘队。 */
		ops_dequeue(rq, p, SCX_DEQ_CORE_SCHED_EXEC);
		dispatch_dequeue(rq, p);
	}

	p->se.exec_start = rq_clock_task(rq);
	/* 从真正成为 next 的时刻开始下一段 runtime，避免把排队时间计入执行。 */

	/* see dequeue_task_scx() on why we skip when !QUEUED */
/* 运行态不在 runnable_list；下次 put_prev 若仍 QUEUED 会重新追加并重置 watchdog 起点。 */
	if (SCX_HAS_OP(sch, running) && (p->scx.flags & SCX_TASK_QUEUED))
		SCX_CALL_OP_TASK(sch, running, rq, p);

	clr_task_runnable(p, true);
	/* 运行态不在 runnable_list；下次 put_prev 若仍 QUEUED 会重新追加并重置 watchdog 起点。 */

	/*
	 * @p is getting newly scheduled or got kicked after someone updated its
	 * slice. Update SCX_RQ_CAN_STOP_TICK to reflect whether the tick can be
	 * stopped. See scx_can_stop_tick().
	 *
	 * Moreover, refresh the load_avgs just when transitioning in and out of
	 * nohz. In the future, we might want to add a mechanism to update
	 * load_avgs periodically on tick-stopped CPUs.
	 */
/* 无限 slice 没有基于 tick 的耗尽点，可撤销 SCHED tick 依赖并刷新离开 nohz 的统计。 */
	if (p->scx.slice == SCX_SLICE_INF) {
		/* 无限 slice 没有基于 tick 的耗尽点，可撤销 SCHED tick 依赖并刷新离开 nohz 的统计。 */
		if (!(rq->scx.flags & SCX_RQ_CAN_STOP_TICK)) {
			/*
			 * Bypass mode always assigns finite slices, so @p
			 * can't have an infinite slice while bypassing.
			 * Therefore, sched_update_tick_dependency() can safely
			 * evaluate the outgoing task.
			 */
/* 有限 slice 必须周期记账；若此前允许停 tick，先清标志并同步 load avg。 */
			rq->scx.flags |= SCX_RQ_CAN_STOP_TICK;
			sched_update_tick_dependency(rq);

			update_other_load_avgs(rq);
		}
	} else {
		/* 有限 slice 必须周期记账；若此前允许停 tick，先清标志并同步 load avg。 */
		if (rq->scx.flags & SCX_RQ_CAN_STOP_TICK) {
			rq->scx.flags &= ~SCX_RQ_CAN_STOP_TICK;
			update_other_load_avgs(rq);
		}

		/*
		 * @rq still references the outgoing scheduling context. A finite
		 * slice is sufficient by itself to require the tick.
		 */
/* 把抢占者 sched_class 映射为 BPF ABI 原因；未知/未来 class 安全归为 UNKNOWN。 */
		if (tick_nohz_full_cpu(cpu_of(rq)))
			tick_nohz_dep_set_cpu(cpu_of(rq), TICK_DEP_BIT_SCHED);
	}
}

/* 把抢占者 sched_class 映射为 BPF ABI 原因；未知/未来 class 安全归为 UNKNOWN。 */
static enum scx_cpu_preempt_reason
preempt_reason_from_class(const struct sched_class *class)
{
	if (class == &stop_sched_class)
		return SCX_CPU_PREEMPT_STOP;
	/* deadline 与实时 class 分别映射稳定 ABI 枚举，其余类归 UNKNOWN。 */
	if (class == &dl_sched_class)
		return SCX_CPU_PREEMPT_DL;
	if (class == &rt_sched_class)
		return SCX_CPU_PREEMPT_RT;
	return SCX_CPU_PREEMPT_UNKNOWN;
}

/*
 * 业务背景：CPU 从 SCX 切到更高 class 时只通知一次 cpu_release，下一次 balance_one
 * 再以 cpu_acquire 成对归还；切到更低 class 是 BPF 主动空置，不算被抢占。
 * 入参：rq 已锁；next 为即将运行的借用 task。
 * 出参/返回：无；可调用 BPF cpu_release 并置 cpu_released。
 * 注意事项：只有协商 HAS_CPU_PREEMPT 才启用协议；重复 higher-class 切换不重复回调。
 */
static void switch_class(struct rq *rq, struct task_struct *next)
{
	struct scx_sched *sch = scx_root;
	const struct sched_class *next_class = next->sched_class;

	if (!(sch->ops.flags & SCX_OPS_HAS_CPU_PREEMPT))
		return;

	/*
	 * The callback is conceptually meant to convey that the CPU is no
	 * longer under the control of SCX. Therefore, don't invoke the callback
	 * if the next class is below SCX (in which case the BPF scheduler has
	 * actively decided not to schedule any tasks on the CPU).
	 */
/* next 低于 SCX 表示策略没有投放 task，CPU 控制权概念上仍属于 SCX。 */
	if (sched_class_above(&ext_sched_class, next_class))
		/* next 低于 SCX 表示策略没有投放 task，CPU 控制权概念上仍属于 SCX。 */
		return;

	/*
	 * At this point we know that SCX was preempted by a higher priority
	 * sched_class, so invoke the ->cpu_release() callback if we have not
	 * done so already. We only send the callback once between SCX being
	 * preempted, and it regaining control of the CPU.
	 *
	 * ->cpu_release() complements ->cpu_acquire(), which is emitted the
	 *  next time that balance_one() is invoked.
	 */
/* 状态位是边沿触发门，确保 release/acquire 每个抢占区间恰好各一次。 */
	if (!rq->scx.cpu_released) {
		/* 状态位是边沿触发门，确保 release/acquire 每个抢占区间恰好各一次。 */
		if (sch->ops.cpu_release) {
			struct scx_cpu_release_args args = {
				.reason = preempt_reason_from_class(next_class),
				.task = next,
			};

			SCX_CALL_OP(sch, cpu_release, rq, cpu_of(rq), &args);
		}
		rq->scx.cpu_released = true;
	}
}

/*
 * 业务背景：SCX task 离开 CPU 时结算 runtime、发布 kick_sync、发送 stopping，并把仍
 * runnable 的 task 按剩余 slice/IMMED/ENQ_LAST 规则重新交给 local DSQ 或 BPF。
 * 入参：rq 已锁；p 为借用前驱；next 可空或为即将运行 task。
 * 出参/返回：无；p 若仍 QUEUED 会重新进入 runnable/DSQ/custody，并可能通知 class release。
 * 注意事项：IMMED 被高 class 抢占时不能滞留 busy CPU；core idle cookie 可豁免 ENQ_LAST 告警。
 */
static void put_prev_task_scx(struct rq *rq, struct task_struct *p,
			      struct task_struct *next)
{
	struct scx_sched *sch = scx_task_sched(p);

	/* see kick_sync_wait_bal_cb() */
/* release 与等待者 acquire 配对，既推进同步代际也发布此前在此 CPU 的调度进展。 */
	smp_store_release(&rq->scx.kick_sync, rq->scx.kick_sync + 1);
	/* release 与等待者 acquire 配对，既推进同步代际也发布此前在此 CPU 的调度进展。 */

	update_curr_scx(rq);

	/* see dequeue_task_scx() on why we skip when !QUEUED */
/* 仍 runnable 的前驱重新进入观察链；真正睡眠 task 已由 dequeue 清 QUEUED。 */
	if (SCX_HAS_OP(sch, stopping) && (p->scx.flags & SCX_TASK_QUEUED))
		SCX_CALL_OP_TASK(sch, stopping, rq, p, true);

	if (p->scx.flags & SCX_TASK_QUEUED) {
		/* 仍 runnable 的前驱重新进入观察链；真正睡眠 task 已由 dequeue 清 QUEUED。 */
		set_task_runnable(rq, p);

		/*
		 * If @p has slice left and is being put, @p is getting
		 * preempted by a higher priority scheduler class or core-sched
		 * forcing a different task. Leave it at the head of the local
		 * DSQ unless it was an IMMED task. IMMED tasks should not
		 * linger on a busy CPU, reenqueue them to the BPF scheduler.
		 */
/* IMMED 不应在被 higher class 占用的 CPU 排头等待，重新交给 BPF 选址。 */
		if (p->scx.slice && !scx_bypassing(sch, cpu_of(rq))) {
			if (p->scx.flags & SCX_TASK_IMMED) {
				/* IMMED 不应在被 higher class 占用的 CPU 排头等待，重新交给 BPF 选址。 */
				p->scx.flags |= SCX_TASK_REENQ_PREEMPTED;
				do_enqueue_task(rq, p, SCX_ENQ_REENQ, -1);
				p->scx.flags &= ~SCX_TASK_REENQ_REASON_MASK;
			} else {
				/* 普通 task 保留剩余 slice，插 local 头部以延续被抢占前的运行权。 */
				dispatch_enqueue(sch, rq, &rq->scx.local_dsq, p, SCX_ENQ_HEAD);
			}
			goto switch_class;
		}

		/*
		 * If @p is runnable but we're about to enter a lower
		 * sched_class, %SCX_OPS_ENQ_LAST must be set. Tell
		 * ops.enqueue() that @p is the only one available for this cpu,
		 * which should trigger an explicit follow-up scheduling event.
		 *
		 * Core scheduling can force this CPU idle while @p stays
		 * runnable. @p's cookie then won't match the core's, so skip
		 * the warning in that case.
		 */
/* 将进入低于 SCX 的 class，ENQ_LAST 告诉 BPF p 是此 CPU 唯一候选并请求显式决策。 */
		if (next && sched_class_above(&ext_sched_class, next->sched_class)) {
			/* 将进入低于 SCX 的 class，ENQ_LAST 告诉 BPF p 是此 CPU 唯一候选并请求显式决策。 */
			WARN_ON_ONCE(sched_cpu_cookie_match(rq, p) &&
				     !(sch->ops.flags & SCX_OPS_ENQ_LAST));
			do_enqueue_task(rq, p, SCX_ENQ_LAST, -1);
		} else {
			do_enqueue_task(rq, p, 0, -1);
		}
	}

switch_class:
	/* task 安置完成后再报告控制权变化，避免 release 回调观察半提交队列状态。 */
	if (next && next->sched_class != &ext_sched_class)
		switch_class(rq, next);
}

/*
 * 业务背景：SCX_KICK_WAIT 要等目标 CPU 至少经过一次 put/pick 同步点；等待期间放 rq
 * lock 并开 IRQ，防止双方各等对方处理 IPI 形成 TLB flush 等循环依赖。
 * 入参：rq 为回调所属已锁队列；cpus_to_sync 与 per-CPU 快照由 kick 发起路径准备。
 * 出参/返回：无；逐个清已推进 CPU，必要时忙等并持续推进本 rq 自己的 kick_sync。
 * 注意事项：每次重取 rq 后重扫 mask，acquire/release 保证同步点前的调度写入可见。
 */
static void kick_sync_wait_bal_cb(struct rq *rq)
{
	struct scx_kick_syncs __rcu *ks = __this_cpu_read(scx_kick_syncs);
	unsigned long *ksyncs = rcu_dereference_sched(ks)->syncs;
	bool waited;
	s32 cpu;

	/*
	 * Drop rq lock and enable IRQs while waiting. IRQs must be enabled
	 * — a target CPU may be waiting for us to process an IPI (e.g. TLB
	 * flush) while we wait for its kick_sync to advance.
	 *
	 * Also, keep advancing our own kick_sync so that new kick_sync waits
	 * targeting us, which can start after we drop the lock, cannot form
	 * cyclic dependencies.
	 */
/* 放锁窗口可能加入新的相互等待关系，完成一轮后若确实等待过就从 mask 头复验。 */
retry:
	/* 放锁窗口可能加入新的相互等待关系，完成一轮后若确实等待过就从 mask 头复验。 */
	waited = false;
	for_each_cpu(cpu, rq->scx.cpus_to_sync) {
		/*
		 * smp_load_acquire() pairs with smp_store_release() on
		 * kick_sync updates on the target CPUs.
		 */
/* 本 CPU 无需自等；目标代际已变化说明它越过同步点，可从待集删除。 */
		if (cpu == cpu_of(rq) ||
		    smp_load_acquire(&cpu_rq(cpu)->scx.kick_sync) != ksyncs[cpu]) {
			cpumask_clear_cpu(cpu, rq->scx.cpus_to_sync);
			/* 本 CPU 无需自等；目标代际已变化说明它越过同步点，可从待集删除。 */
			continue;
		}

		raw_spin_rq_unlock_irq(rq);
		/* 开 IRQ 后目标 CPU 发来的 IPI 可被处理，破除双向等待；循环中发布本 CPU 进展。 */
		while (READ_ONCE(cpu_rq(cpu)->scx.kick_sync) == ksyncs[cpu]) {
			smp_store_release(&rq->scx.kick_sync, rq->scx.kick_sync + 1);
			cpu_relax();
		}
		raw_spin_rq_lock_irq(rq);
		waited = true;
	}

	if (waited)
		goto retry;
}

/* 返回 local DSQ FIFO 首个 task 的锁内借用指针；空队列返回 NULL，不增加 task 引用。 */
static struct task_struct *first_local_task(struct rq *rq)
{
	return list_first_entry_or_null(&rq->scx.local_dsq.list,
					struct task_struct, scx.dsq_list.node);
}

/*
 * 业务背景：实际 pick 核心先 unpin rq 运行 balance（允许锁舞步），再检查更高 class
 * 是否在窗口内入队，最终保留 prev 或取 local DSQ 首项。
 * 入参：rq 已锁；rf 保存 pin 状态；force_scx 为 DL server 强制只选 SCX 的模式。
 * 出参/返回：返回借用 task、NULL 或 RETRY_TASK；不在此摘 local DSQ，set_next 后完成。
 * 注意事项：kick sync 必须延期到可开 IRQ callback；零 slice task 在选择点补默认值并告警一次。
 */
static struct task_struct *
do_pick_task_scx(struct rq *rq, struct rq_flags *rf, bool force_scx)
{
	struct task_struct *prev = rq->curr;
	bool keep_prev;
	struct task_struct *p;

	/* see kick_sync_wait_bal_cb() */
/* balance_one 可能临时放 rq lock；unpin 告诉 lockdep/core 当前锁可参与迁移舞步。 */
	smp_store_release(&rq->scx.kick_sync, rq->scx.kick_sync + 1);

	rq_modified_begin(rq, &ext_sched_class);

	rq_unpin_lock(rq, rf);
	/* balance_one 可能临时放 rq lock；unpin 告诉 lockdep/core 当前锁可参与迁移舞步。 */
	balance_one(rq, prev);
	rq_repin_lock(rq, rf);
	maybe_queue_balance_callback(rq);

	/*
	 * Defer to a balance callback which can drop rq lock and enable
	 * IRQs. Waiting directly in the pick path would deadlock against
	 * CPUs sending us IPIs (e.g. TLB flushes) while we wait for them.
	 */
/* pick 热路径不能开 IRQ 忙等，balance callback 提供满足协议的等待上下文。 */
	if (unlikely(rq->scx.kick_sync_pending)) {
		/* pick 热路径不能开 IRQ 忙等，balance callback 提供满足协议的等待上下文。 */
		rq->scx.kick_sync_pending = false;
		queue_balance_callback(rq, &rq->scx.kick_sync_bal_cb,
				       kick_sync_wait_bal_cb);
	}

	/*
	 * If any higher-priority sched class enqueued a runnable task on
	 * this rq during balance_one(), abort and return RETRY_TASK, so
	 * that the scheduler loop can restart.
	 *
	 * If @force_scx is true, always try to pick a SCHED_EXT task,
	 * regardless of any higher-priority sched classes activity.
	 */
/* balance 放锁时 higher class 入队，返回 RETRY 让 core 从最高 class 重新扫描。 */
	if (!force_scx && rq_modified_above(rq, &ext_sched_class))
		/* balance 放锁时 higher class 入队，返回 RETRY 让 core 从最高 class 重新扫描。 */
		return RETRY_TASK;

	keep_prev = rq->scx.flags & SCX_RQ_BAL_KEEP;
	if (unlikely(keep_prev &&
		     prev->sched_class != &ext_sched_class)) {
		WARN_ON_ONCE(scx_enable_state() == SCX_ENABLED);
		keep_prev = false;
		/* disable 过渡可能留下旧 KEEP；非 ext prev 绝不能按 SCX slice 继续运行。 */
	}

	/*
	 * If balance_one() is telling us to keep running @prev, replenish slice
	 * if necessary and keep running @prev. Otherwise, pop the first one
	 * from the local DSQ.
	 */
/* 保留 prev 时 slice=0 仅见于 bypass/default fallback，补片后继续执行。 */
	if (keep_prev) {
		/* 保留 prev 时 slice=0 仅见于 bypass/default fallback，补片后继续执行。 */
		p = prev;
		if (!p->scx.slice)
			refill_task_slice_dfl(scx_task_sched(p), p);
	} else {
		/* 正常 pick 只从 local DSQ 取，global/BPF 已由 balance 先搬入 local。 */
		p = first_local_task(rq);
		if (!p)
			return NULL;

		if (unlikely(!p->scx.slice)) {
			struct scx_sched *sch = scx_task_sched(p);

			/* BPF 留零 slice 会立即重调度；首次告警后补默认值保证前进。 */
			if (!scx_bypassing(sch, cpu_of(rq)) &&
			    !sch->warned_zero_slice) {
				printk_deferred(KERN_WARNING "sched_ext: %s[%d] has zero slice in %s()\n",
						p->comm, p->pid, __func__);
				sch->warned_zero_slice = true;
			}
			refill_task_slice_dfl(sch, p);
		}
		/* 此时 p 已具有非零 slice，可安全返回给 core 作为下一运行任务。 */
	}

	return p;
}

/* 从 local DSQ 选择首项；为空时通过 DL server/balance 触发策略补充。 */
/* ext sched_class 的普通 pick 薄包装；force_scx=false，允许 higher class 触发 RETRY_TASK。 */
static struct task_struct *pick_task_scx(struct rq *rq, struct rq_flags *rf)
{
	return do_pick_task_scx(rq, rf, false);
}

/*
 * Select the next task to run from the ext scheduling class.
 *
 * Use do_pick_task_scx() directly with @force_scx enabled, since the
 * dl_server must always select a sched_ext task.
 */
/* server 可能在 SCX 未发布/正在撤销时被调用，静态键关闭则没有合法 task。 */
static struct task_struct *
ext_server_pick_task(struct sched_dl_entity *dl_se, struct rq_flags *rf)
{
	/* server 可能在 SCX 未发布/正在撤销时被调用，静态键关闭则没有合法 task。 */
	if (!scx_enabled())
		return NULL;

	return do_pick_task_scx(dl_se->rq, rf, true);
	/* force_scx 忽略 higher-class 修改，因为 DL server 的客户端只能由 ext class 提供。 */
}

/*
 * Initialize the ext server deadline entity.
 */
/* 先初始化通用 deadline entity，再把 pick 回调和所属 rq 绑定成 SCX 带宽 server。 */
void ext_server_init(struct rq *rq)
{
	struct sched_dl_entity *dl_se = &rq->ext_server;

	init_dl_entity(dl_se);
	/* 先初始化通用 deadline entity，再把 pick 回调和所属 rq 绑定成 SCX 带宽 server。 */

	dl_server_init(dl_se, rq, ext_server_pick_task);
}

#ifdef CONFIG_SCHED_CORE
/**
 * scx_prio_less - Task ordering for core-sched
 * @a: task A
 * @b: task B
 * @in_fi: in forced idle state
 *
 * Core-sched is implemented as an additional scheduling layer on top of the
 * usual sched_class'es and needs to find out the expected task ordering. For
 * SCX, core-sched calls this function to interrogate the task ordering.
 *
 * Unless overridden by ops.core_sched_before(), @p->scx.core_sched_at is used
 * to implement the default task ordering. The older the timestamp, the higher
 * priority the task - the global FIFO ordering matching the default scheduling
 * behavior.
 *
 * When ops.core_sched_before() is enabled, @p->scx.core_sched_at is used to
 * implement FIFO ordering within each local DSQ. See pick_task_scx().
 */
/*
 * 上游契约：core-sched 叠加在 class 之上，需要比较同 core 候选。相同 scheduler 且
 * BPF 实现回调时交给策略，否则按 core_sched_at 做全局 FIFO，时间戳越老优先级越高。
 * 入参：a/b 为 core-sched 窗口内稳定只读 task；in_fi 表示 forced-idle，本实现不另分支。
 * 出参/返回：a 的优先级低于 b 时返回 true，具体语义与 core-sched 比较接口一致。
 * 注意事项：只在非 bypass 时信任 BPF；const 强转仅适配回调 ABI，verifier 限制实际写访问。
 */
bool scx_prio_less(const struct task_struct *a, const struct task_struct *b,
		   bool in_fi)
{
	struct scx_sched *sch_a = scx_task_sched(a);
	struct scx_sched *sch_b = scx_task_sched(b);

	/*
	 * The const qualifiers are dropped from task_struct pointers when
	 * calling ops.core_sched_before(). Accesses are controlled by the
	 * verifier.
	 */
/* 跨 scheduler 没有共同 BPF 排序域，必须回退内核时间戳。 */
	if (sch_a == sch_b && SCX_HAS_OP(sch_a, core_sched_before) &&
	    /* 跨 scheduler 没有共同 BPF 排序域，必须回退内核时间戳。 */
	    !scx_bypassing(sch_a, task_cpu(a)))
		return SCX_CALL_OP_2TASKS_RET(sch_a, core_sched_before,
					      task_rq(a),
					      (struct task_struct *)a,
					      (struct task_struct *)b);
	else
		return time_after64(a->scx.core_sched_at, b->scx.core_sched_at);
}
#endif	/* CONFIG_SCHED_CORE */
/* 以上条件编译分支到此结束；仅 CONFIG_SCHED_CORE 对应配置启用时包含其中实现。 */

/*
 * 业务背景：wakeup 选择初始 rq 并给 select_cpu BPF 回调一次 direct-local 快速机会；
 * 这只是 enqueue 所在 rq，真正运行 CPU 仍可由后续 DSQ dispatch 决定。
 * 入参：p 为唤醒协议稳定 task；prev_cpu 为上次/建议 CPU；wake_flags 描述 wake/exec。
 * 出参/返回：返回经验证 possible CPU；BPF 非法值、WF_EXEC 或默认失败退 prev_cpu。
 * 注意事项：回调窗口登记 direct_dispatch_task；默认 helper 命中 idle CPU 时预填 local verdict/slice。
 */
static int select_task_rq_scx(struct task_struct *p, int prev_cpu, int wake_flags)
{
	struct scx_sched *sch = scx_task_sched(p);
	bool bypassing;

	/*
	 * sched_exec() calls with %WF_EXEC when @p is about to exec(2) as it
	 * can be a good migration opportunity with low cache and memory
	 * footprint. Returning a CPU different than @prev_cpu triggers
	 * immediate rq migration. However, for SCX, as the current rq
	 * association doesn't dictate where the task is going to run, this
	 * doesn't fit well. If necessary, we can later add a dedicated method
	 * which can decide to preempt self to force it through the regular
	 * scheduling path.
	 */
/* exec 的低 footprint 迁移优化不适配 DSQ 模型，保持当前 rq 等常规调度决定去向。 */
	if (unlikely(wake_flags & WF_EXEC))
		/* exec 的低 footprint 迁移优化不适配 DSQ 模型，保持当前 rq 等常规调度决定去向。 */
		return prev_cpu;

	bypassing = scx_bypassing(sch, task_cpu(p));
	if (likely(SCX_HAS_OP(sch, select_cpu)) && !bypassing) {
		s32 cpu;
		struct task_struct **ddsp_taskp;

		ddsp_taskp = this_cpu_ptr(&direct_dispatch_task);
		WARN_ON_ONCE(*ddsp_taskp);
		*ddsp_taskp = p;
		/* 仅本次 select_cpu 回调可对 p 直派；退出窗口前无论结果都撤销 marker。 */

		this_rq()->scx.in_select_cpu = true;
		cpu = SCX_CALL_OP_TASK_RET(sch, select_cpu, NULL, p,
					   scx_cpu_arg(prev_cpu), wake_flags);
		cpu = scx_cpu_ret(sch, cpu);
		this_rq()->scx.in_select_cpu = false;
		p->scx.selected_cpu = cpu;
		/* 保存策略选择供 enqueue 后统计 fallback，不把该值当作不可变 task_cpu。 */
		*ddsp_taskp = NULL;
		if (scx_cpu_valid(sch, cpu, "from ops.select_cpu()"))
			return cpu;
		else
			return prev_cpu;
	} else {
		s32 cpu;

		cpu = scx_select_cpu_dfl(p, prev_cpu, wake_flags, NULL, 0);
		/* 内核默认 helper 若找到可直用 CPU 返回非负，并允许绕过后续 BPF enqueue。 */
		if (cpu >= 0) {
			refill_task_slice_dfl(sch, p);
			p->scx.ddsp_dsq_id = SCX_DSQ_LOCAL;
		} else {
			cpu = prev_cpu;
		}
		p->scx.selected_cpu = cpu;

		/* bypass 使用内核选择并计事件，让恢复后的策略能识别非 BPF 派发。 */
		if (bypassing)
			__scx_add_event(sch, SCX_EV_BYPASS_DISPATCH, 1);
		return cpu;
	}
}

/* wakeup 事务尾部复用已持 rq lock 执行 deferred 动作；p 仅是接口参数，无需单独使用。 */
static void task_woken_scx(struct rq *rq, struct task_struct *p)
{
	run_deferred(rq);
}

/*
 * 业务背景：core 更新 task affinity 后把“当前有效”cpus_ptr 通知 BPF；migrate-disable
 * 期间它可能暂时比配置 cpus_mask 更窄，策略必须按实际可运行集合决策。
 * 入参：p 为 affinity 写侧锁稳定 task；ac 描述新 mask/flags，由通用 helper 消费。
 * 出参/返回：无；更新 core affinity，未 dead task 再调用 set_cpumask/set_cmask。
 * 注意事项：BPF verifier 已细粒度控制写权限，ABI const 强转不授予持久保存内核指针。
 */
static void set_cpus_allowed_scx(struct task_struct *p,
				 struct affinity_context *ac)
{
	struct scx_sched *sch = scx_task_sched(p);

	set_cpus_allowed_common(p, ac);
	/* 先让 core 完成 cpus_ptr/迁移约束，再向策略报告最终有效结果。 */

	if (task_dead_and_done(p))
		/* 已完成 SCX exit 的 task 不再调用可能已卸载的 ops。 */
		return;

	/*
	 * The effective cpumask is stored in @p->cpus_ptr which may temporarily
	 * differ from the configured one in @p->cpus_mask. Always tell the bpf
	 * scheduler the effective one.
	 *
	 * Fine-grained memory write control is enforced by BPF making the const
	 * designation pointless. Cast it away when calling the operation.
	 */
/* rq 锁下同步内建状态与 BPF cpu_online/offline 回调；序列变化可使启用失败。 */
	if (SCX_HAS_OP(sch, set_cpumask))
		scx_call_op_set_cpumask(sch, task_rq(p), p, (struct cpumask *)p->cpus_ptr);
}

/* rq 锁下同步内建状态与 BPF cpu_online/offline 回调；序列变化可使启用失败。 */
/*
 * 业务背景：CPU hotplug 同时推进全局序列、刷新 idle 拓扑并通知 root scheduler；未实现
 * 对应 op 的实例无法安全适配拓扑变化，要求以可重启原因退出。
 * 入参：rq 为 hotplug core 稳定队列；online 为目标 BPF 可见状态。
 * 出参/返回：无；递增 hotplug_seq，可能调用 BPF、更新 idle 拓扑或触发 scheduler exit。
 * 注意事项：root 由 cpus_read_lock 稳定；不能只看 scx_enabled，回调需在静态键发布前启用。
 */
static void handle_hotplug(struct rq *rq, bool online)
{
	struct scx_sched *sch = scx_root;
	s32 cpu = cpu_of(rq);

	atomic_long_inc(&scx_hotplug_seq);
	/* enable 前后记录的序列若不同，说明初始化横跨 hotplug，控制路径会拒绝提交。 */

	/*
	 * scx_root updates are protected by cpus_read_lock() and will stay
	 * stable here. Note that we can't depend on scx_enabled() test as the
	 * hotplug ops need to be enabled before __scx_enabled is set.
	 */
/* SCX 尚无 root 时只保留序列证据，不存在可通知的 BPF 对象。 */
	if (unlikely(!sch))
		/* SCX 尚无 root 时只保留序列证据，不存在可通知的 BPF 对象。 */
		return;

	if (scx_enabled())
		scx_idle_update_selcpu_topology(&sch->ops);

	if (online && SCX_HAS_OP(sch, cpu_online))
		SCX_CALL_OP(sch, cpu_online, NULL, scx_cpu_arg(cpu));
	else if (!online && SCX_HAS_OP(sch, cpu_offline))
		SCX_CALL_OP(sch, cpu_offline, NULL, scx_cpu_arg(cpu));
	else
		/* 缺回调无法让策略修正 CPU 状态，带 RESTART/HOTPLUG 退出码交给用户态重载。 */
		scx_exit(sch, SCX_EXIT_UNREG_KERN,
			 SCX_ECODE_ACT_RESTART | SCX_ECODE_RSN_HOTPLUG,
			 "cpu %d going %s, exiting scheduler", cpu,
			 online ? "online" : "offline");
}

void scx_rq_activate(struct rq *rq)
{
	/* core CPU active 回调：转交统一 hotplug 事务，rq 生命周期由 per-CPU 静态对象保证。 */
	handle_hotplug(rq, true);
}

void scx_rq_deactivate(struct rq *rq)
{
	/* core CPU deactivate 回调：在 BPF 侧撤销 CPU 可用性并推进 hotplug 序列。 */
	handle_hotplug(rq, false);
}

static void rq_online_scx(struct rq *rq)
{
	/* rq lock/hotplug 串行下发布 BPF 已知在线位；scx_rq_online 还会与 cpu_active 合取。 */
	rq->scx.flags |= SCX_RQ_ONLINE;
}

static void rq_offline_scx(struct rq *rq)
{
	/* 先清 BPF 可调度位，后续 enqueue/dispatch 即使 core 仍在过渡也会走内核 fallback。 */
	rq->scx.flags &= ~SCX_RQ_ONLINE;
}

/* 扫描 runnable task 的最后进展时间；超 watchdog 阈值即报告 stall。 */
/*
 * 业务背景：在单个 rq 的 runnable_list 中找超过所属 scheduler timeout 仍未运行的 task，
 * 这是对 BPF 遗漏 dispatch/饿死任务的安全检测。
 * 入参：rq 为目标静态队列，函数自行 irqsave 加锁。
 * 出参/返回：发现并登记首个 stall 返回 true，否则 false；不持锁返回。
 * 注意事项：每个 task 用自己的 scheduler timeout；退出登记异步，扫描首错后停止减少噪声。
 */
static bool check_rq_for_timeouts(struct rq *rq)
{
	struct scx_sched *sch;
	struct task_struct *p;
	struct rq_flags rf;
	bool timed_out = false;

	rq_lock_irqsave(rq, &rf);
	/* rq lock 稳定 runnable 链和 runnable_at；BH RCU 读取 root 只用于确认 SCX 仍存在。 */
	sch = rcu_dereference_bh(scx_root);
	if (unlikely(!sch))
		goto out_unlock;

	list_for_each_entry(p, &rq->scx.runnable_list, scx.runnable_node) {
		/* sub-scheduler task 取自身 watchdog_timeout，不能用 root 的统一阈值。 */
		struct scx_sched *sch = scx_task_sched(p);
		unsigned long last_runnable = p->scx.runnable_at;

		if (unlikely(time_after(jiffies,
					last_runnable + READ_ONCE(sch->watchdog_timeout)))) {
			u32 dur_ms = jiffies_to_msecs(jiffies - last_runnable);

			__scx_exit(sch, SCX_EXIT_ERROR_STALL, 0, cpu_of(rq),
			/* 退出消息保留 task、CPU 和毫秒时长，workqueue 稍后执行真正 disable/dump。 */
				   "%s[%d] failed to run for %u.%03us",
				   p->comm, p->pid, dur_ms / 1000,
				   dur_ms % 1000);
			timed_out = true;
			break;
		}
	}
out_unlock:
	/* 所有扫描出口统一恢复 IRQ/rq 锁状态，返回值已由匹配分支确定。 */
	rq_unlock_irqrestore(rq, &rf);
	return timed_out;
}

/* 共享 watchdog 周期检查所有 rq/scheduler，并按最短 timeout 重新安排。 */
/*
 * 业务背景：共享 delayed_work 刷新自身心跳并轮询所有 online rq；任一 task stall 即
 * 停止本轮，其 scheduler exit 已足以触发后续诊断。
 * 入参：work 为全局 scx_watchdog_work，生命周期覆盖 SCX 子系统。
 * 出参/返回：无；更新时间戳，可能触发退出，并按当前最短 interval 重新排队。
 * 注意事项：CPU 间 cond_resched 防止大机器长时间占用 worker；ULONG_MAX 表示无需重排。
 */
static void scx_watchdog_workfn(struct work_struct *work)
{
	unsigned long intv;
	int cpu;

	WRITE_ONCE(scx_watchdog_timestamp, jiffies);
	/* 先发布 worker 自己仍在运行，tick 侧据此区分“task stall”与“watchdog 被饿死”。 */

	for_each_online_cpu(cpu) {
		/* hotplug 视图只决定本轮枚举，check helper 内部再以 rq lock 稳定 task 链。 */
		if (unlikely(check_rq_for_timeouts(cpu_rq(cpu))))
			break;

		cond_resched();
	}

	intv = READ_ONCE(scx_watchdog_interval);
	/* enable/disable 可并发重算最短 interval；单次快照足够安排下一轮。 */
	if (intv < ULONG_MAX)
		queue_delayed_work(system_dfl_wq, to_delayed_work(work), intv);
}

/*
 * 业务背景：普通 scheduler tick 提供独立于 delayed_work 的自监控路径；若 BPF 失效
 * 连 ksoftirqd/worker 都无法运行，当前 CPU tick 仍能发现 watchdog 心跳过期。
 * 入参：rq 为当前 CPU 已锁/调度上下文稳定队列。
 * 出参/返回：无；可能触发 root stall exit，并更新其他 class/load avg。
 * 注意事项：只在 SCX 静态键开启且 root 存在时检查；timeout 读取允许控制路径快照变化。
 */
void scx_tick(struct rq *rq)
{
	struct scx_sched *root;
	unsigned long last_check;

	if (!scx_enabled())
		/* 静态键关闭时 root 可能不存在或正在回收，不进入任何 SCX 数据。 */
		return;

	root = rcu_dereference_bh(scx_root);
	if (unlikely(!root))
		return;

	last_check = READ_ONCE(scx_watchdog_timestamp);
	if (unlikely(time_after(jiffies,
				last_check + READ_ONCE(root->watchdog_timeout)))) {
		u32 dur_ms = jiffies_to_msecs(jiffies - last_check);

		scx_exit(root, SCX_EXIT_ERROR_STALL, 0,
		/* 这里诊断的是 watchdog work 未 check-in，不是某个具体 runnable task。 */
			 "watchdog failed to check in for %u.%03us",
			 dur_ms / 1000, dur_ms % 1000);
	}

	update_other_load_avgs(rq);
}

/*
 * 业务背景：SCX 当前 task 的 class tick，结算 slice 并给 BPF tick hook；bypass 时不再
 * 信任策略的 slice/core 排序，强制耗尽并刷新内核 FIFO 时间。
 * 入参：rq 已锁；curr 为当前借用 task；queued 为通用接口参数，本实现由 flags 判断。
 * 出参/返回：无；更新 runtime/slice，可能调用 BPF，并在 slice 归零时请求 resched。
 * 注意事项：bypass 优先于 BPF tick，确保失效 scheduler 不能阻止 CPU 重新选择。
 */
static void task_tick_scx(struct rq *rq, struct task_struct *curr, int queued)
{
	struct scx_sched *sch = scx_task_sched(curr);

	update_curr_scx(rq);

	/*
	 * While disabling, always resched and refresh core-sched timestamp as
	 * we can't trust the slice management or ops.core_sched_before().
	 */
/* 有限保底 slice 每 tick 耗尽，促使 bypass DSQ 轮转而不信任 BPF 的无限片。 */
	if (scx_bypassing(sch, cpu_of(rq))) {
		/* 有限保底 slice 每 tick 耗尽，促使 bypass DSQ 轮转而不信任 BPF 的无限片。 */
		curr->scx.slice = 0;
		touch_core_sched(rq, curr);
	} else if (SCX_HAS_OP(sch, tick)) {
		SCX_CALL_OP_TASK(sch, tick, rq, curr);
	}

	if (!curr->scx.slice)
		/* 只置 need_resched，真正 pick 在安全调度边界发生。 */
		resched_curr(rq);
}

#ifdef CONFIG_EXT_GROUP_SCHED
/*
 * 业务背景：把 sched task_group 映射为 BPF init_task 可见 cgroup；无 group scheduling
 * 或 autogroup 没有真实 css 时统一视为默认 root。
 * 入参：tg 可空或为借用 task_group。
 * 出参/返回：返回借用 cgroup，始终非空；不增加 css 引用。
 * 注意事项：返回对象由 cgroup core/外层锁稳定，BPF 不应跨回调保存裸指针。
 */
static struct cgroup *tg_cgrp(struct task_group *tg)
{
	/*
	 * If CGROUP_SCHED is disabled, @tg is NULL. If @tg is an autogroup,
	 * @tg->css.cgroup is NULL. In both cases, @tg can be treated as the
	 * root cgroup.
	 */
	/* 只有真实 cgroup task_group 返回自身 css；NULL/autogroup 均回落 default root。 */
	if (tg && tg->css.cgroup)
		return tg->css.cgroup;
	else
		return &cgrp_dfl_root.cgrp;
}

#define SCX_INIT_TASK_ARGS_CGROUP(tg)		.cgroup = tg_cgrp(tg),

#else	/* CONFIG_EXT_GROUP_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_GROUP_SCHED 对应配置启用时包含其中实现。 */

#define SCX_INIT_TASK_ARGS_CGROUP(tg)

#endif	/* CONFIG_EXT_GROUP_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_GROUP_SCHED 对应配置启用时包含其中实现。 */

/* 调用 ops.init_task 建立 scheduler 私有状态；失败由调用者按阶段执行 cancel/exit。 */
/*
 * 业务背景：在 task 加入 scheduler 前让 BPF 分配/初始化私有状态，并处理 root 对 task
 * 设置 disallow 的永久拒绝语义。
 * 入参：sch 为目标实例；p 为尚未 ENABLED 的借用 task；fork 区分新建与加载现存 task。
 * 出参/返回：成功 0；BPF 失败返回规范化负 errno；私有资源回滚由调用者 exit_task(cancelled)。
 * 注意事项：仅 root 且非 fork 可设置 disallow；若 p 已是 SCHED_EXT，持 rq lock 改回 NORMAL。
 */
static int __scx_init_task(struct scx_sched *sch, struct task_struct *p, bool fork)
{
	int ret;

	p->scx.disallow = false;
	/* 每次新 scheduler 初始化都清旧拒绝位，BPF 必须在本次 init_task 明确重新设置。 */

	if (SCX_HAS_OP(sch, init_task)) {
		struct scx_init_task_args args = {
			SCX_INIT_TASK_ARGS_CGROUP(task_group(p))
			.fork = fork,
		};

		ret = SCX_CALL_OP_RET(sch, init_task, NULL, p, &args);
		/* 返回非零表示 BPF 没有提交初始化；sanitize 后由上层按 INIT_BEGIN 回滚。 */
		if (unlikely(ret)) {
			ret = ops_sanitize_err(sch, "init_task", ret);
			return ret;
		}
	}

	if (p->scx.disallow) {
		if (unlikely(scx_parent(sch))) {
			/* sub-scheduler 无权永久改变 task 对 root SCX policy 的可用性。 */
			scx_error(sch, "non-root ops.init_task() set task->scx.disallow for %s[%d]",
				  p->comm, p->pid);
		} else if (unlikely(fork)) {
			/* fork 返回失败之外不能把新 task 悄然改 policy，故该时点设置视为策略错误。 */
			scx_error(sch, "ops.init_task() set task->scx.disallow for %s[%d] during fork",
				  p->comm, p->pid);
		} else {
			struct rq *rq;
			struct rq_flags rf;

			rq = task_rq_lock(p, &rf);
			/* 现存 task 的 policy 与拒绝位必须在同一 rq 锁事务内改写，防止 setscheduler 穿越。 */

			/*
			 * We're in the load path and @p->policy will be applied
			 * right after. Reverting @p->policy here and rejecting
			 * %SCHED_EXT transitions from scx_check_setscheduler()
			 * guarantees that if ops.init_task() sets @p->disallow,
			 * @p can never be in SCX.
			 */
			/* load 路径稍后会正式应用 p->policy；此处不先退回 fair，避免 SCHED_EXT task 无意义地来回切 class。 */
			if (p->policy == SCHED_EXT) {
				p->policy = SCHED_NORMAL;
				atomic_long_inc(&scx_nr_rejected);
			}

			task_rq_unlock(rq, p, &rf);
		}
	}

	return 0;
}

/* 将已 INIT task 切到 ENABLED 并调用 ops.enable；rq 锁保证不与排队转换交叉。 */
/*
 * 业务背景：在 task 正式进入 SCX class 前把标准优先级转换为 cgroup 权重并依次通知
 * enable/set_weight，保证 BPF 第一眼看到的字段已是当前有效值。
 * 入参：sch 为所属实例；p 为 rq lock 稳定且已完成 init_task 的借用 task。
 * 出参/返回：无；写 p->scx.weight并调用可选 BPF ops，不改变生命周期枚举。
 * 注意事项：调用者随后才设 ENABLED；进入时 IN_CUSTODY 必须清，否则状态机已交叉。
 */
static void __scx_enable_task(struct scx_sched *sch, struct task_struct *p)
{
	struct rq *rq = task_rq(p);
	u32 weight;

	lockdep_assert_rq_held(rq);

	/*
	 * Verify the task is not in BPF scheduler's custody. If flag
	 * transitions are consistent, the flag should always be clear
	 * here.
	 */
	/* 此处 task 理应已离开 BPF custody；若标志仍置位说明 enqueue/dequeue 生命周期转换不一致。 */
	WARN_ON_ONCE(p->scx.flags & SCX_TASK_IN_CUSTODY);

	/*
	 * Set the weight before calling ops.enable() so that the scheduler
	 * doesn't see a stale value if they inspect the task struct.
	 */
/* SCHED_IDLE 使用专用最小权重，普通策略由 static_prio 查内核权重表。 */
	if (task_has_idle_policy(p))
		/* SCHED_IDLE 使用专用最小权重，普通策略由 static_prio 查内核权重表。 */
		weight = WEIGHT_IDLEPRIO;
	else
		weight = sched_prio_to_weight[p->static_prio - MAX_RT_PRIO];

	p->scx.weight = sched_weight_to_cgroup(weight);
	/* BPF ABI 使用 cgroup 1..10000 尺度，而 core load_weight 使用内部缩放值。 */

	if (SCX_HAS_OP(sch, enable))
		SCX_CALL_OP_TASK(sch, enable, rq, p);

	if (SCX_HAS_OP(sch, set_weight))
		SCX_CALL_OP_TASK(sch, set_weight, rq, p, p->scx.weight);
}

/* 包装 enable 回调后提交 ENABLED 状态；rq lock 使回调与状态发布对 sched_class 原子可见。 */
static void scx_enable_task(struct scx_sched *sch, struct task_struct *p)
{
	__scx_enable_task(sch, p);
	scx_set_task_state(p, SCX_TASK_ENABLED);
}

/*
 * 业务背景：task 离开 SCX class 时先让 BPF disable 观察最终 slice/vtime，再转 READY 并
 * 清 scheduler 管理字段，为将来重新进入或实例卸载保留已 init 的 task 对象。
 * 入参：sch 为当前实例；p 为 rq lock 下且状态 ENABLED 的借用 task。
 * 出参/返回：无；清 direct verdict，调用 disable，状态变 READY，slice/vtime 归零。
 * 注意事项：结束时不得仍在 BPF custody；否则说明 dequeue/dispatch 回收协议失配。
 */
static void scx_disable_task(struct scx_sched *sch, struct task_struct *p)
{
	struct rq *rq = task_rq(p);

	lockdep_assert_rq_held(rq);
	WARN_ON_ONCE(scx_get_task_state(p) != SCX_TASK_ENABLED);

	clear_direct_dispatch(p);
	/* queued wakeup task 可能未被 bypass 全量循环看到，先清 verdict 防止下个实例复用。 */

	if (SCX_HAS_OP(sch, disable))
		SCX_CALL_OP_TASK(sch, disable, rq, p);
	scx_set_task_state(p, SCX_TASK_READY);

	/*
	 * Reset the SCX-managed fields when @p leaves the BPF scheduler's
	 * control, after ops.disable() has observed their final values.
	 */
/* 必须晚于 ops.disable，BPF 才能在回调中读取最终值做统计或释放私有状态。 */
	p->scx.dsq_vtime = 0;
	/* 必须晚于 ops.disable，BPF 才能在回调中读取最终值做统计或释放私有状态。 */
	p->scx.slice = 0;

	/*
	 * Verify the task is not in BPF scheduler's custody. If flag
	 * transitions are consistent, the flag should always be clear
	 * here.
	 */
/* 按 ENABLED/READY/INIT 阶段逆序调用 disable/exit/cancel，最终撤销 scheduler 归属。 */
	WARN_ON_ONCE(p->scx.flags & SCX_TASK_IN_CUSTODY);
}

/* 按 ENABLED/READY/INIT 阶段逆序调用 disable/exit/cancel，最终撤销 scheduler 归属。 */
/*
 * 业务背景：统一拆除 task 的 BPF 生命周期，按状态决定是否先 disable，以及 exit_task
 * 的 cancelled 位；NONE 无资源，INIT 表示 enable 从未提交。
 * 入参：sch 为待退出实例；p 同时持 pi_lock 与当前 rq lock 的借用 task。
 * 出参/返回：无；可能把 ENABLED 降 READY 并调用一次 exit_task，但不清最终 task 状态。
 * 注意事项：未知/过渡状态只告警返回；外层负责撤销 scheduler 指针并置 NONE/DEAD。
 */
static void __scx_disable_and_exit_task(struct scx_sched *sch,
					struct task_struct *p)
{
	struct scx_exit_task_args args = {
		.cancelled = false,
	};

	lockdep_assert_held(&p->pi_lock);
	lockdep_assert_rq_held(task_rq(p));

	switch (scx_get_task_state(p)) {
	case SCX_TASK_NONE:
		/* 从未完成 init，无 BPF 资源可撤销。 */
		return;
	case SCX_TASK_INIT:
		/* init_task 成功但 enable 未运行，exit_task 必须看到 cancelled=true。 */
		args.cancelled = true;
		break;
	case SCX_TASK_READY:
		/* 已 init 且当前不在 class，直接 exit_task，cancelled=false。 */
		break;
	case SCX_TASK_ENABLED:
		/* 先让 disable 观察最终运行状态，再 exit 私有资源。 */
		scx_disable_task(sch, p);
		break;
	default:
		WARN_ON_ONCE(true);
		return;
	}

	if (SCX_HAS_OP(sch, exit_task))
		/* exit_task 与已成功 init_task 成对，args 携带取消或正常退出原因。 */
		SCX_CALL_OP_TASK(sch, exit_task, task_rq(p), p, &args);
}

/*
 * Undo a completed __scx_init_task(sch, p, false) when scx_enable_task() never
 * ran. The task state has not been transitioned, so this mirrors the
 * SCX_TASK_INIT branch in __scx_disable_and_exit_task().
 */
/* sub-enable 已 init 但尚未切 task 归属，专门以 cancelled=true 撤销子实例私有资源。 */
static void scx_sub_init_cancel_task(struct scx_sched *sch, struct task_struct *p)
{
	/* sub-enable 已 init 但尚未切 task 归属，专门以 cancelled=true 撤销子实例私有资源。 */
	struct scx_exit_task_args args = { .cancelled = true };

	lockdep_assert_held(&p->pi_lock);
	lockdep_assert_rq_held(task_rq(p));

	if (SCX_HAS_OP(sch, exit_task))
		SCX_CALL_OP_TASK(sch, exit_task, task_rq(p), p, &args);
}

/*
 * 业务背景：完成 task 从一个 scheduler 实例的最终退出，并处理 sub-enable 窗口中同时
 * 为父/子初始化、却在 enable 前退出的 SCX_TASK_SUB_INIT 特例。
 * 入参：sch 为当前归属；p 持 pi_lock/rq lock 的借用 task。
 * 出参/返回：无；所有相关 exit_task 已调用，SUB_INIT 清除，RCU 归属置 NULL，状态 NONE。
 * 注意事项：scx_enabling_sub_sched 只在 enable_mutex 串行窗口有效；不能漏撤子实例 init。
 */
static void scx_disable_and_exit_task(struct scx_sched *sch,
				      struct task_struct *p)
{
	__scx_disable_and_exit_task(sch, p);

	/*
	 * If set, @p exited between __scx_init_task() and scx_enable_task() in
	 * scx_sub_enable() and is initialized for both the associated sched and
	 * its parent. Exit for the child too - scx_enable_task() never ran for
	 * it, so undo only init_task. The flag is only set on the sub-enable
	 * path, so it's always clear when @p arrives here in %SCX_TASK_NONE.
	 */
/* child init 成功而正式 enable 未发生，父实例常规退出之外还要单独 cancel child。 */
	if (p->scx.flags & SCX_TASK_SUB_INIT) {
		/* child init 成功而正式 enable 未发生，父实例常规退出之外还要单独 cancel child。 */
		if (!WARN_ON_ONCE(!scx_enabling_sub_sched))
			scx_sub_init_cancel_task(scx_enabling_sub_sched, p);
		p->scx.flags &= ~SCX_TASK_SUB_INIT;
	}

	scx_set_task_sched(p, NULL);
	/* 先撤销 RCU scheduler 归属，再置 NONE；读者不会把已退出 task 关联到待回收实例。 */
	scx_set_task_state(p, SCX_TASK_NONE);
}

/*
 * 业务背景：task_struct 创建时把嵌入 SCX 实体置为所有链表/RB 节点可安全操作的初态。
 * 入参：scx 为调用者独占、尚未发布的嵌入对象。
 * 出参/返回：无；清字段、初始化节点，CPU 哨兵 -1、DSQ invalid、默认 slice。
 * 注意事项：只能在对象构造/完全重建时调用，不能清空仍链接队列或被 RCU 读者使用的实体。
 */
void init_scx_entity(struct sched_ext_entity *scx)
{
	memset(scx, 0, sizeof(*scx));
	/* memset 后显式恢复 list/RB 的“未链接”编码；全零不等价于合法空节点。 */
	INIT_LIST_HEAD(&scx->dsq_list.node);
	RB_CLEAR_NODE(&scx->dsq_priq);
	scx->sticky_cpu = -1;
	scx->holding_cpu = -1;
	INIT_LIST_HEAD(&scx->runnable_node);
	scx->runnable_at = jiffies;
	scx->ddsp_dsq_id = SCX_DSQ_INVALID;
	scx->slice = SCX_SLICE_DFL;
}

/* See scx_tid_alloc / scx_tid_cursor. */
/* 每 CPU 从全局 cursor 批量领取 tid，0 保留；局部区间耗尽才做原子更新。 */
/*
 * 业务背景：为 TID_TO_TASK 分配稳定非零 id；每 CPU 缓存 SCX_TID_CHUNK 个号，降低
 * fork 热路径对全局 atomic64 cacheline 的争用。
 * 入参：无。
 * 出参/返回：返回本 CPU 下一个 u64 tid；全局回绕约束由超大 64 位空间承担。
 * 注意事项：preempt guard 固定 this_cpu_ptr 生命周期；0 永不由初始游标发出。
 */
static u64 scx_alloc_tid(void)
{
	struct scx_tid_alloc *ta;

	guard(preempt)();
	ta = this_cpu_ptr(&scx_tid_alloc);

	if (unlikely(ta->next >= ta->end)) {
		/* fetch_add 原子预留互不重叠区间，本地 next/end 后续无需额外同步。 */
		ta->next = atomic64_fetch_add(SCX_TID_CHUNK, &scx_tid_cursor);
		ta->end = ta->next + SCX_TID_CHUNK;
	}
	return ta->next++;
}

/*
 * 业务背景：把新 task 的嵌入 tid_hash_node 发布到 tid→task 表。
 * 入参：p 已分配非零 tid 且从 fork 到 free 受 scx_tasks 生命周期覆盖。
 * 出参/返回：无；成功插表，重复/分配失败仅 WARN，因为调用点无法安全回滚已发布 fork。
 * 注意事项：必须持 scx_tasks_lock；哈希表不额外持 task 引用。
 */
static void scx_tid_hash_insert(struct task_struct *p)
{
	int ret;

	lockdep_assert_held(&scx_tasks_lock);

	ret = rhashtable_lookup_insert_fast(&scx_tid_hash,
					    &p->scx.tid_hash_node,
					    scx_tid_hash_params);
	WARN_ON_ONCE(ret);
}

/*
 * 业务背景：fork 开始前取得 per-CPU rwsem 读侧，使冷门 enable/disable 写侧能获得
 * “当前所有 task 集合不再产生半构造成员”的稳定窗口。
 * 入参：p 尚在构造，本函数不读取它。
 * 出参/返回：无；持有 scx_fork_rwsem 读锁，必须由 post_fork/cancel_fork 释放。
 * 注意事项：读锁可能睡眠；fork 的所有成功/失败出口都必须成对调用后置 hook。
 */
void scx_pre_fork(struct task_struct *p)
{
	/*
	 * BPF scheduler enable/disable paths want to be able to iterate and
	 * update all tasks which can become complex when racing forks. As
	 * enable/disable are very cold paths, let's use a percpu_rwsem to
	 * exclude forks.
	 */
	/* enable/disable 极冷，写侧阻塞所有 fork 比在每个 task 状态上做复杂无锁协调更可靠。 */
	percpu_down_read(&scx_fork_rwsem);
}

/*
 * 业务背景：为新 task 分配 tid，并在 SCX task init 已发布时调用所属 scheduler init_task；
 * 此时 child 尚未进入全局 scx_tasks，也不可运行。
 * 入参：p 为独占新 task；kargs 提供 cset 以在 sub-scheduler 配置下选择归属。
 * 出参/返回：成功 0；init_task 失败返回负 errno，状态回 NONE，fork core 负责销毁 child。
 * 注意事项：要求 pre_fork 读锁；成功仅到 INIT，post_fork 才发布 READY/全局链。
 */
int scx_fork(struct task_struct *p, struct kernel_clone_args *kargs)
{
	s32 ret;

	percpu_rwsem_assert_held(&scx_fork_rwsem);

	p->scx.tid = scx_alloc_tid();
	/* tid 即便当前 TID_TO_TASK 未开启也预分配，未来 enable 可无缝插表。 */

	if (scx_init_task_enabled) {
#ifdef CONFIG_EXT_SUB_SCHED
		struct scx_sched *sch = kargs->cset->dfl_cgrp->scx_sched;
#else
		struct scx_sched *sch = scx_root;
#endif
		scx_set_task_state(p, SCX_TASK_INIT_BEGIN);
		/* INIT_BEGIN 使并发死亡/enable 知道 BPF init 正在进行，失败可精确回 NONE。 */
		ret = __scx_init_task(sch, p, true);
		if (unlikely(ret)) {
			scx_set_task_state(p, SCX_TASK_NONE);
			return ret;
		}
		scx_set_task_state(p, SCX_TASK_INIT);
		scx_set_task_sched(p, sch);
	}

	return 0;
}

/*
 * 业务背景：fork 成功提交后把已 init child 转 READY，必要时立即 enable，并最终发布到
 * scx_tasks/tid 哈希；这是专用 task 生命周期索引的可见性边界。
 * 入参：p 为成功创建且仍受 fork rwsem 保护的 child。
 * 出参/返回：无；task 链/哈希可见，释放 fork 读锁；若 class=EXT 状态变 ENABLED。
 * 注意事项：链表与 tid 表在同一 tasks_lock 临界区发布，读者不会只看到其中一半。
 */
void scx_post_fork(struct task_struct *p)
{
	if (scx_init_task_enabled) {
		/* INIT→READY 表示 BPF 私有初始化已提交，但 task 未必采用 EXT policy。 */
		scx_set_task_state(p, SCX_TASK_READY);

		/*
		 * Enable the task immediately if it's running on sched_ext.
		 * Otherwise, it'll be enabled in switching_to_scx() if and
		 * when it's ever configured to run with a SCHED_EXT policy.
		 */
/* child 已选择 EXT 时持其 rq lock 调 enable，防止首次 enqueue 早于 BPF enable。 */
		if (p->sched_class == &ext_sched_class) {
			/* child 已选择 EXT 时持其 rq lock 调 enable，防止首次 enqueue 早于 BPF enable。 */
			struct rq_flags rf;
			struct rq *rq;

			rq = task_rq_lock(p, &rf);
			scx_enable_task(scx_task_sched(p), p);
			task_rq_unlock(rq, p, &rf);
		}
	}

	scoped_guard(raw_spinlock_irq, &scx_tasks_lock) {
		/* 先追加全量生命周期链，再按已协商功能插 tid 表；二者共享同一锁。 */
		list_add_tail(&p->scx.tasks_node, &scx_tasks);
		if (scx_tid_to_task_enabled())
			scx_tid_hash_insert(p);
	}

	percpu_up_read(&scx_fork_rwsem);
}

/*
 * 业务背景：fork 在 post 发布前失败时撤销可能完成的 init_task，并释放 pre_fork 读锁。
 * 入参：p 为未加入 scx_tasks 的独占失败 child。
 * 出参/返回：无；SCX 已启用时在 rq lock 下 exit/cancel 私有状态，最后释放 rwsem。
 * 注意事项：状态不应达到 READY；没有链表/哈希发布，所以无需从全局索引摘除。
 */
void scx_cancel_fork(struct task_struct *p)
{
	if (scx_enabled()) {
		struct rq *rq;
		struct rq_flags rf;

		rq = task_rq_lock(p, &rf);
		/* rq 锁稳定状态与 scheduler 指针，逐个退出后恢复为未初始化。 */
		WARN_ON_ONCE(scx_get_task_state(p) >= SCX_TASK_READY);
		scx_disable_and_exit_task(scx_task_sched(p), p);
		task_rq_unlock(rq, p, &rf);
	}

	percpu_up_read(&scx_fork_rwsem);
}

/**
 * task_dead_and_done - Is a task dead and done running?
 * @p: target task
 *
 * Once sched_ext_dead() removes the dead task from scx_tasks and exits it, the
 * task no longer exists from SCX's POV. However, certain sched_class ops may be
 * invoked on these dead tasks leading to failures - e.g. sched_setscheduler()
 * may try to switch a task which finished sched_ext_dead() back into SCX
 * triggering invalid SCX task state transitions and worse.
 *
 * Once a task has finished the final switch, sched_ext_dead() is the only thing
 * that needs to happen on the task. Use this test to short-circuit sched_class
 * operations which may be called on dead tasks.
 */
/* 最终 context switch 完成且 SCX 引用释放后，dead task 才可从全局表摘除。 */
/*
 * 上游契约：TASK_DEAD 且已离开 CPU 才表示 sched_ext_dead 之后不会再恢复执行；用于
 * 短路可能落在死亡 task 上的迟到 sched_class ops。
 * 入参：p 为已持其当前 rq lock 的借用 task。
 * 出参/返回：最终切出完成为 true，否则 false；无状态修改。
 * 注意事项：仅 TASK_DEAD 不够，仍 on_cpu 的 task 还可能执行最后一段调度收尾。
 */
static bool task_dead_and_done(struct task_struct *p)
{
	struct rq *rq = task_rq(p);

	lockdep_assert_rq_held(rq);

	/*
	 * In do_task_dead(), a dying task sets %TASK_DEAD with preemption
	 * disabled and __schedule(). If @p has %TASK_DEAD set and off CPU, @p
	 * won't ever run again.
	 */
/* __state 由死亡路径禁抢占写入，rq lock 下再结合 on_cpu 关闭最后切换窗口。 */
	return unlikely(READ_ONCE(p->__state) == TASK_DEAD) &&
		/* __state 由死亡路径禁抢占写入，rq lock 下再结合 on_cpu 关闭最后切换窗口。 */
		!task_on_cpu(rq, p);
}

/*
 * 业务背景：task 最后切出后的 SCX 终结点，先从全生命周期链/tid 表摘除阻止新查找，
 * 再在 rq lock 下撤销 BPF 私有状态并标 DEAD 与 cgroup iterator 同步。
 * 入参：p 已 TASK_DEAD、off-CPU 且由退出路径稳定的借用 task。
 * 出参/返回：无；全局索引不可见，ops disable/exit 完成或 INIT_BEGIN 被标 DEAD 等待自撤销。
 * 注意事项：摘表不立即释放 task_struct；通用 task 生命周期在本 hook 之后继续最终回收。
 */
void sched_ext_dead(struct task_struct *p)
{
	/*
	 * By the time control reaches here, @p has %TASK_DEAD set, switched out
	 * for the last time and then dropped the rq lock - task_dead_and_done()
	 * should be returning %true nullifying the straggling sched_class ops.
	 * Remove from scx_tasks and exit @p.
	 */
/* 链表与可选 tid 哈希同锁摘除，之后 enable 全量 iterator 不会再领取 p。 */
	scoped_guard(raw_spinlock_irqsave, &scx_tasks_lock) {
		/* 链表与可选 tid 哈希同锁摘除，之后 enable 全量 iterator 不会再领取 p。 */
		list_del_init(&p->scx.tasks_node);
		if (scx_tid_to_task_enabled())
			rhashtable_remove_fast(&scx_tid_hash,
					       &p->scx.tid_hash_node,
					       scx_tid_hash_params);
	}

	/*
	 * @p is off scx_tasks and wholly ours. scx_root_enable()'s READY ->
	 * ENABLED transitions can't race us. Disable ops for @p.
	 *
	 * %SCX_TASK_DEAD synchronizes against cgroup task iteration - see
	 * scx_task_iter_next_locked(). NONE tasks need no marking: cgroup
	 * iteration is only used from sub-sched paths, which require root
	 * enabled. Root enable transitions every live task to at least READY.
	 *
	 * %INIT_BEGIN means ops.init_task() is running for @p. Don't call
	 * into ops; transition to %DEAD so the post-init recheck unwinds
	 * via scx_sub_init_cancel_task().
	 */
/* NONE 从未由当前实例初始化，无需调用可能已撤销的 BPF ops。 */
	if (scx_get_task_state(p) != SCX_TASK_NONE) {
		/* NONE 从未由当前实例初始化，无需调用可能已撤销的 BPF ops。 */
		struct rq_flags rf;
		struct rq *rq;

		rq = task_rq_lock(p, &rf);
		if (scx_get_task_state(p) != SCX_TASK_INIT_BEGIN)
			/* INIT_BEGIN 的 init_task 仍在运行，不能并发 exit；只置 DEAD 让完成侧 cancel。 */
			scx_disable_and_exit_task(scx_task_sched(p), p);
		scx_set_task_state(p, SCX_TASK_DEAD);
		task_rq_unlock(rq, p, &rf);
	}
}

/*
 * 业务背景：nice/cgroup 权重变化时把 core load_weight 转为 BPF ABI 权重，并仅对仍
 * ENABLED task 调 set_weight。
 * 入参：rq 已持 p rq lock；p 为借用 task；lw 是通用调度器计算后的只读权重。
 * 出参/返回：无；更新 p->scx.weight并可能通知 BPF。
 * 注意事项：dead 或已 disable task 必须跳过，BPF 可能已在 disable 中释放 task 私有状态。
 */
static void reweight_task_scx(struct rq *rq, struct task_struct *p,
			      const struct load_weight *lw)
{
	struct scx_sched *sch = scx_task_sched(p);

	lockdep_assert_rq_held(task_rq(p));

	if (task_dead_and_done(p))
		return;

	/*
	 * When switching sched_class away from SCX, reweight_task_scx()
	 * is called _after_ scx_disable_task(). Skip calling ops.set_weight()
	 * since the BPF scheduler may have already forgotten the task in
	 * ops.disable().
	 * p->scx.weight will be recalculated in scx_enable_task() if the task
	 * ever returns to SCX class.
	 */
/* class 切出顺序是先 disable 后 reweight，不能在 READY 状态回调已忘记 task 的 BPF。 */
	if (scx_get_task_state(p) != SCX_TASK_ENABLED)
		/* class 切出顺序是先 disable 后 reweight，不能在 READY 状态回调已忘记 task 的 BPF。 */
		return;

	p->scx.weight = sched_weight_to_cgroup(scale_load_down(lw->weight));
	if (SCX_HAS_OP(sch, set_weight))
		SCX_CALL_OP_TASK(sch, set_weight, rq, p, p->scx.weight);
}

/* SCX 不按 core priority 直接重排；权重变化由 reweight_task_scx 通知，故此 hook 为空。 */
static void prio_changed_scx(struct rq *rq, struct task_struct *p, u64 oldprio)
{
}

/*
 * 业务背景：task 的 sched_class 即将切入 EXT 时提交 READY→ENABLED，并补发其在其他
 * class 期间可能变化、未经过 set_cpus_allowed_scx 的有效 affinity。
 * 入参：rq 已锁；p 为已 init/READY 的借用 task。
 * 出参/返回：无；dead task 空操作，否则调用 enable 与可选 cpumask 回调。
 * 注意事项：先 enable 再 set_cpumask，保证 BPF 已建立 task 私有状态后才接收属性通知。
 */
static void switching_to_scx(struct rq *rq, struct task_struct *p)
{
	struct scx_sched *sch = scx_task_sched(p);

	if (task_dead_and_done(p))
		return;

	scx_enable_task(sch, p);

	/*
	 * set_cpus_allowed_scx() is not called while @p is associated with a
	 * different scheduler class. Keep the BPF scheduler up-to-date.
	 */
/*
 * 业务背景：task 完成切出 EXT 后执行 ENABLED→READY；dead 或已被 parent teardown 置
 * NONE 的 task 不得再调用 BPF disable。
 * 入参：rq 已锁；p 为借用 task。
 * 出参/返回：无；正常时清 direct/运行字段并通知 disable。
 * 注意事项：NONE 特例来自 sub-scheduler fail-parent，强行 disable 会产生状态转换错误。
 */
	if (SCX_HAS_OP(sch, set_cpumask))
		scx_call_op_set_cpumask(sch, rq, p, (struct cpumask *)p->cpus_ptr);
}

/*
 * 业务背景：task 完成切出 EXT 后执行 ENABLED→READY；dead 或已被 parent teardown 置
 * NONE 的 task 不得再调用 BPF disable。
 * 入参：rq 已锁；p 为借用 task。
 * 出参/返回：无；正常时清 direct/运行字段并通知 disable。
 * 注意事项：NONE 特例来自 sub-scheduler fail-parent，强行 disable 会产生状态转换错误。
 */
static void switched_from_scx(struct rq *rq, struct task_struct *p)
{
	if (task_dead_and_done(p))
		return;

	/*
	 * %NONE means SCX is no longer tracking @p at the task level (e.g.
	 * scx_fail_parent() handed @p back to the parent at NONE pending the
	 * parent's own teardown). There is nothing to disable; calling
	 * scx_disable_task() would WARN on the non-%ENABLED state and trigger a
	 * NONE -> READY validation failure.
	 */
/* class 切换完成后无额外提交；所有 BPF enable 工作已在 switching_to_scx 前置阶段完成。 */
	if (scx_get_task_state(p) == SCX_TASK_NONE)
		return;

	scx_disable_task(scx_task_sched(p), p);
}

/* class 切换完成后无额外提交；所有 BPF enable 工作已在 switching_to_scx 前置阶段完成。 */
static void switched_to_scx(struct rq *rq, struct task_struct *p) {}

/*
 * 业务背景：root init_task 可永久拒绝某 task 使用 SCHED_EXT，此 hook 在 policy 提交前
 * 执行访问控制，防止后续再次绕过 disallow。
 * 入参：p 为 rq lock 稳定 task；policy 为请求目标策略。
 * 出参/返回：禁止的非 EXT→EXT 转换返回 -EACCES，其余 0。
 * 注意事项：SCX 未启用时不强制旧实例位；READ_ONCE 只需读取布尔策略快照。
 */
int scx_check_setscheduler(struct task_struct *p, int policy)
{
	lockdep_assert_rq_held(task_rq(p));

	/* if disallow, reject transitioning into SCX */
/*
 * 业务背景：离开 pinned enqueue 上下文后消费远端 local direct verdict；此时允许
 * dispatch_to_local_dsq 临时放 rq lock，完成此前不能执行的双 rq 舞步。
 * 入参：rq 已锁且为 deferred 链拥有者。
 * 出参/返回：无；链上每个 task 清暂存 verdict并尝试提交目标 local DSQ。
 * 注意事项：每轮取表头而不用 safe iterator，因为被调函数可放锁使预取 next 失效。
 */
	if (scx_enabled() && READ_ONCE(p->scx.disallow) &&
	    p->policy != policy && policy == SCHED_EXT)
		return -EACCES;

	return 0;
}

/*
 * 业务背景：离开 pinned enqueue 上下文后消费远端 local direct verdict；此时允许
 * dispatch_to_local_dsq 临时放 rq lock，完成此前不能执行的双 rq 舞步。
 * 入参：rq 已锁且为 deferred 链拥有者。
 * 出参/返回：无；链上每个 task 清暂存 verdict并尝试提交目标 local DSQ。
 * 注意事项：每轮取表头而不用 safe iterator，因为被调函数可放锁使预取 next 失效。
 */
static void process_ddsp_deferred_locals(struct rq *rq)
{
	struct task_struct *p;

	lockdep_assert_rq_held(rq);

	/*
	 * Now that @rq can be unlocked, execute the deferred enqueueing of
	 * tasks directly dispatched to the local DSQs of other CPUs. See
	 * direct_dispatch(). Keep popping from the head instead of using
	 * list_for_each_entry_safe() as dispatch_local_dsq() may unlock @rq
	 * temporarily.
	 */
	/* 每次从链头重取，因为 direct dispatch 可临时解锁并改变整条 deferred 链。 */
	while ((p = list_first_entry_or_null(&rq->scx.ddsp_deferred_locals,
				struct task_struct, scx.dsq_list.node))) {
		struct scx_sched *sch = scx_task_sched(p);
		struct scx_dispatch_q *dsq;
		u64 dsq_id = p->scx.ddsp_dsq_id;
		u64 enq_flags = p->scx.ddsp_enq_flags;

		list_del_init(&p->scx.dsq_list.node);
		/* 先从私有链摘除并复制 verdict，切锁期间 dequeue 才能安全取消/接管。 */
		clear_direct_dispatch(p);

		dsq = find_dsq_for_dispatch(sch, rq, dsq_id, task_cpu(p));
		if (!WARN_ON_ONCE(dsq->id != SCX_DSQ_LOCAL))
			dispatch_to_local_dsq(sch, rq, dsq, p, enq_flags);
	}
}

/*
 * Determine whether @p should be reenqueued from a local DSQ.
 *
 * @reenq_flags is mutable and accumulates state across the DSQ walk:
 *
 * - %SCX_REENQ_TSR_NOT_FIRST: Set after the first task is visited. "First"
 *   tracks position in the DSQ list, not among IMMED tasks. A non-IMMED task at
 *   the head consumes the first slot.
 *
 * - %SCX_REENQ_TSR_RQ_OPEN: Set by reenq_local() before the walk if
 *   rq_is_open() is true.
 *
 * An IMMED task is kept (returns %false) only if it's the first task in the DSQ
 * AND the current task is done — i.e. it will execute immediately. All other
 * IMMED tasks are reenqueued. This means if a non-IMMED task sits at the head,
 * every IMMED task behind it gets reenqueued.
 *
 * Reenqueued tasks go through ops.enqueue() with %SCX_ENQ_REENQ |
 * %SCX_TASK_REENQ_IMMED. If the BPF scheduler dispatches back to the same local
 * DSQ with %SCX_ENQ_IMMED while the CPU is still unavailable, this triggers
 * another reenq cycle. Repetitions are bounded by %SCX_REENQ_LOCAL_MAX_REPEAT
 * in process_deferred_reenq_locals().
 */
/*
 * 上游说明：reenq_flags 同时携带请求与遍历状态；只有 DSQ 第一个 task 且 rq open 的
 * IMMED 可保留，其他 IMMED 必须回 BPF，避免被前置 task 或 busy CPU 阻塞。
 * 入参：p 为 local DSQ 锁内借用；reenq_flags 为输入输出累计位；reason 为输出原因。
 * 出参/返回：需要 reenq 为 true；reason 写 KFUNC 或 IMMED。
 * 注意事项：first 按整个 DSQ 位置而非 IMMED 子集计算；重复周期由外层上限约束。
 */
static bool local_task_should_reenq(struct task_struct *p, u64 *reenq_flags, u32 *reason)
{
	bool first;

	first = !(*reenq_flags & SCX_REENQ_TSR_NOT_FIRST);
	/* 无论 p 是否 IMMED，访问首节点后都消费 first 槽，忠实反映真实执行次序。 */
	*reenq_flags |= SCX_REENQ_TSR_NOT_FIRST;

	*reason = SCX_TASK_REENQ_KFUNC;

	if ((p->scx.flags & SCX_TASK_IMMED) &&
	    (!first || !(*reenq_flags & SCX_REENQ_TSR_RQ_OPEN))) {
		__scx_add_event(scx_task_sched(p), SCX_EV_REENQ_IMMED, 1);
		*reason = SCX_TASK_REENQ_IMMED;
		return true;
	}

	return *reenq_flags & SCX_REENQ_ANY;
}

/*
 * 业务背景：从 local DSQ 选出目标 scheduler 子树内需重新策略化的 task，先移到私有链，
 * 再逐个 do_enqueue，避免 BPF 又直派回同一 DSQ 时在原遍历中无限重复。
 * 入参：sch 为请求根实例；rq 已锁；reenq_flags 为策略与内部遍历位。
 * 出参/返回：返回实际重新 enqueue 的 task 数；task 最终由 BPF/fallback 重新归队。
 * 注意事项：跳过 migration_pending 和非后代 scheduler；reason 位只在单次回调窗口有效。
 */
static u32 reenq_local(struct scx_sched *sch, struct rq *rq, u64 reenq_flags)
{
	LIST_HEAD(tasks);
	u32 nr_enqueued = 0;
	struct task_struct *p, *n;

	lockdep_assert_rq_held(rq);

	if (WARN_ON_ONCE(reenq_flags & __SCX_REENQ_TSR_MASK))
		/* 调用者不得伪造内部遍历状态；清除后由本函数根据当前 rq 重新建立。 */
		reenq_flags &= ~__SCX_REENQ_TSR_MASK;
	if (rq_is_open(rq, 0))
		reenq_flags |= SCX_REENQ_TSR_RQ_OPEN;

	/*
	 * The BPF scheduler may choose to dispatch tasks back to
	 * @rq->scx.local_dsq. Move all candidate tasks off to a private list
	 * first to avoid processing the same tasks repeatedly.
	 */
	/* BPF 可能把 task 再直派回本 local DSQ；先搬到私有链表可防止同一轮扫描反复重入队形成活锁。 */
	list_for_each_entry_safe(p, n, &rq->scx.local_dsq.list,
				 scx.dsq_list.node) {
		struct scx_sched *task_sch = scx_task_sched(p);
		u32 reason;

		/*
		 * If @p is being migrated, @p's current CPU may not agree with
		 * its allowed CPUs and the migration_cpu_stop is about to
		 * deactivate and re-activate @p anyway. Skip re-enqueueing.
		 *
		 * While racing sched property changes may also dequeue and
		 * re-enqueue a migrating task while its current CPU and allowed
		 * CPUs disagree, they use %ENQUEUE_RESTORE which is bypassed to
		 * the current local DSQ for running tasks and thus are not
		 * visible to the BPF scheduler.
		 */
/* stop-machine 迁移即将自行 deactivate/activate，此时 affinity 与 CPU 可暂时不一致。 */
		if (p->migration_pending)
			/* stop-machine 迁移即将自行 deactivate/activate，此时 affinity 与 CPU 可暂时不一致。 */
			continue;

		if (!scx_is_descendant(task_sch, sch))
			continue;

		if (!local_task_should_reenq(p, &reenq_flags, &reason))
			continue;

		dispatch_dequeue(rq, p);
		/* 先完全撤销 local DSQ/custody，再借同一 node 挂私有链，避免一个节点同时属于两表。 */

		if (WARN_ON_ONCE(p->scx.flags & SCX_TASK_REENQ_REASON_MASK))
			p->scx.flags &= ~SCX_TASK_REENQ_REASON_MASK;
		p->scx.flags |= reason;

		list_add_tail(&p->scx.dsq_list.node, &tasks);
	}

	list_for_each_entry_safe(p, n, &tasks, scx.dsq_list.node) {
		/* 第二阶段允许 BPF 把 task 放回任意 DSQ，不再影响候选集合。 */
		list_del_init(&p->scx.dsq_list.node);

		do_enqueue_task(rq, p, SCX_ENQ_REENQ, -1);

		p->scx.flags &= ~SCX_TASK_REENQ_REASON_MASK;
		nr_enqueued++;
	}

	return nr_enqueued;
}

/* 在 rq 解锁边界处理 local DSQ reenq 请求，序列号限制重复循环和活锁。 */
/*
 * 业务背景：合并并消费 rq 上多个 scheduler 的 local reenq 请求；同一次 run_deferred
 * 中 task 被 BPF 反复直派回来时用 seq/cnt 检测并终止活锁。
 * 入参：rq 已锁；请求链由 deferred_reenq_lock 保护。
 * 出参/返回：无；逐项清 flags/node并执行 reenq，重复超限触发 scheduler error。
 * 注意事项：consumer smp_mb 与 schedule_dsq_reenq 配对，保证请求者不会错过本次扫描。
 */
static void process_deferred_reenq_locals(struct rq *rq)
{
	u64 seq = ++rq->scx.deferred_reenq_locals_seq;

	lockdep_assert_rq_held(rq);

	while (true) {
		struct scx_sched *sch;
		u64 reenq_flags;
		bool skip = false;

		/* 锁内只领取一个请求；真正 reenq 在锁外执行，允许回调再次生产。 */
		/* 每轮锁内摘一个 user DSQ 请求，锁外扫描可能切换多个 rq。 */
		/* user consumer 与 local consumer 共用锁，但各自链表/请求槽互不混淆。 */
		/* 同一锁串行领取 user 请求；锁外 reenq_user 可安全再次生产下一轮请求。 */
		scoped_guard (raw_spinlock, &rq->scx.deferred_reenq_lock) {
			struct scx_deferred_reenq_local *drl =
				list_first_entry_or_null(&rq->scx.deferred_reenq_locals,
							 struct scx_deferred_reenq_local,
							 node);
			struct scx_sched_pcpu *sch_pcpu;

			if (!drl)
				/* 锁内确认队列空，所有此前发布请求均已消费。 */
				return;

			sch_pcpu = container_of(drl, struct scx_sched_pcpu,
						deferred_reenq_local);
			sch = sch_pcpu->sch;

			reenq_flags = drl->flags;
			WRITE_ONCE(drl->flags, 0);
			list_del_init(&drl->node);

			if (likely(drl->seq != seq)) {
				/* 新 run_deferred 代际重置重复计数，允许未来独立请求重新尝试。 */
				drl->seq = seq;
				drl->cnt = 0;
			} else {
				if (unlikely(++drl->cnt > SCX_REENQ_LOCAL_MAX_REPEAT)) {
					/* 同一代际超限说明策略把不可立即运行 task 持续 IMMED 直派回来。 */
					scx_error(sch, "SCX_ENQ_REENQ on SCX_DSQ_LOCAL repeated %u times",
						  drl->cnt);
					skip = true;
				}

				__scx_add_event(sch, SCX_EV_REENQ_LOCAL_REPEAT, 1);
			}
		}

		if (!skip) {
			/* see schedule_dsq_reenq() */
			/* 全屏障保证本次摘链与并发追加之间至少一方观察到对方状态。 */
			smp_mb();

			reenq_local(sch, rq, reenq_flags);
		}
	}
}

/* user DSQ 当前只支持显式 ANY 全量 reenq；输出 KFUNC 原因供 BPF 区分主动重排事件。 */
static bool user_task_should_reenq(struct task_struct *p, u64 reenq_flags, u32 *reason)
{
	*reason = SCX_TASK_REENQ_KFUNC;
	return reenq_flags & SCX_REENQ_ANY;
}

/*
 * 业务背景：遍历 user DSQ 并把符合条件的 task 重新交给各自 scheduler；task 分属不同
 * rq，故用 cursor 允许放 DSQ 锁并按需切换 rq lock，每次切锁后复验代际/claim。
 * 入参：rq 为调用者最终需恢复的已锁队列；dsq 为 user 队列；reenq_flags 为请求策略。
 * 出参/返回：无；候选 task 离开 user DSQ并重新 enqueue，返回时原 rq 重新持锁。
 * 注意事项：bypass 立即停止；每批放 rq lock/cpu_relax，避免大 DSQ 长时间锁死 CPU。
 */
static void reenq_user(struct rq *rq, struct scx_dispatch_q *dsq, u64 reenq_flags)
{
	struct rq *locked_rq = rq;
	struct scx_sched *sch = dsq->sched;
	struct scx_dsq_list_node cursor = INIT_DSQ_LIST_CURSOR(cursor, dsq, 0);
	struct task_struct *p;
	s32 nr_enqueued = 0;

	lockdep_assert_rq_held(rq);

	raw_spin_lock(&dsq->lock);

	while (likely(!READ_ONCE(sch->bypass_depth))) {
		/* bypass 开始后队列由内核接管，不能继续回调已退出/退出中的 BPF。 */
		struct rq *task_rq;
		u32 reason;

		p = nldsq_cursor_next_task(&cursor, dsq);
		if (!p)
			break;

		if (!user_task_should_reenq(p, reenq_flags, &reason))
			continue;

		task_rq = task_rq(p);

		if (locked_rq != task_rq) {
			/* trylock 优先避免放 DSQ；失败则按 rq→DSQ 正序重取并用 cursor 复验 p。 */
			if (locked_rq)
				raw_spin_rq_unlock(locked_rq);
			if (unlikely(!raw_spin_rq_trylock(task_rq))) {
				raw_spin_unlock(&dsq->lock);
				raw_spin_rq_lock(task_rq);
				raw_spin_lock(&dsq->lock);
			}
			locked_rq = task_rq;

			/* did we lose @p while switching locks? */
			/* 放锁后用游标代际复验，丢失时不触碰已被迁走/消费的 task。 */
			if (nldsq_cursor_lost_task(&cursor, task_rq, dsq, p))
				continue;
		}

		/* @p is on @dsq, its rq and @dsq are locked */
/* 两锁均持有时 claim 确定，先摘 DSQ 再放队列锁进入普通 enqueue。 */
		dispatch_dequeue_locked(p, dsq);
		/* 两锁均持有时 claim 确定，先摘 DSQ 再放队列锁进入普通 enqueue。 */
		raw_spin_unlock(&dsq->lock);

		if (WARN_ON_ONCE(p->scx.flags & SCX_TASK_REENQ_REASON_MASK))
			p->scx.flags &= ~SCX_TASK_REENQ_REASON_MASK;
		p->scx.flags |= reason;

		do_enqueue_task(task_rq, p, SCX_ENQ_REENQ, -1);

		p->scx.flags &= ~SCX_TASK_REENQ_REASON_MASK;

		if (!(++nr_enqueued % SCX_TASK_ITER_BATCH)) {
			/* 周期性释放 rq lock，限制 IRQ/锁占用；cursor 仍界定起始集合。 */
			raw_spin_rq_unlock(locked_rq);
			locked_rq = NULL;
			cpu_relax();
		}

		raw_spin_lock(&dsq->lock);
	}

	list_del_init(&cursor.node);
	raw_spin_unlock(&dsq->lock);

	/* 契约要求返回仍持原 rq；若遍历停在别处，按单锁顺序切回。 */
	if (locked_rq != rq) {
		if (locked_rq)
			raw_spin_rq_unlock(locked_rq);
		raw_spin_rq_lock(rq);
	}
}

/* 逐个处理 user DSQ 延迟 reenq；DSQ 销毁或归属变化时安全跳过。 */
/*
 * 业务背景：从 rq 私有 deferred 链取 user DSQ 请求，锁内复制并清节点，锁外执行
 * 可能切换多个 rq 的 reenq_user。
 * 入参：rq 已锁；链表由 deferred_reenq_lock 串行生产/消费。
 * 出参/返回：无；消费到链空，flags 合并值各执行一次。
 * 注意事项：全屏障与 producer 配对；builtin DSQ 不应出现在此链，BUG_ON 保护内部不变量。
 */
static void process_deferred_reenq_users(struct rq *rq)
{
	lockdep_assert_rq_held(rq);

	while (true) {
		struct scx_dispatch_q *dsq;
		u64 reenq_flags;

		/* 阶段 1：在共享锁内领取并清除一个 user DSQ 请求，链表空即完成本轮消费。 */
		scoped_guard (raw_spinlock, &rq->scx.deferred_reenq_lock) {
			struct scx_deferred_reenq_user *dru =
				list_first_entry_or_null(&rq->scx.deferred_reenq_users,
							 struct scx_deferred_reenq_user,
							 node);
			struct scx_dsq_pcpu *dsq_pcpu;

			if (!dru)
				return;

			/* 请求槽反推出所属 DSQ，复制合并 flags 后立即解除链表所有权。 */
			dsq_pcpu = container_of(dru, struct scx_dsq_pcpu,
						deferred_reenq_user);
			dsq = dsq_pcpu->dsq;
			reenq_flags = dru->flags;
			WRITE_ONCE(dru->flags, 0);
			list_del_init(&dru->node);
		}

		/* see schedule_dsq_reenq() */
		/* 与 producer 屏障配对后，reenq_user 必能看到本请求之前发布的 flags/队列状态。 */
		smp_mb();

		BUG_ON(dsq->id & SCX_DSQ_FLAG_BUILTIN);
		reenq_user(rq, dsq, reenq_flags);
	}
}

/* 汇总执行 deferred dispatch、reenq 与 kick，pending 位在完成后清除。 */
/*
 * 业务背景：所有 deferred hook 的统一执行器，顺序先完成 direct-local verdict，再处理
 * local/user reenq，保证 task 先到达稳定 DSQ 后才参与重排。
 * 入参：rq 已锁但调用环境允许相关 helper 临时切锁。
 * 出参/返回：无；把当前可见 deferred 队列处理至空。
 * 注意事项：生产者可在执行中追加，具体 consumer 循环负责合并和活锁边界。
 */
static void run_deferred(struct rq *rq)
{
	process_ddsp_deferred_locals(rq);

	if (!list_empty(&rq->scx.deferred_reenq_locals))
		process_deferred_reenq_locals(rq);

	if (!list_empty(&rq->scx.deferred_reenq_users))
		process_deferred_reenq_users(rq);
}

#ifdef CONFIG_NO_HZ_FULL
/*
 * 业务背景：NO_HZ_FULL 决定当前 CPU 是否可停调度 tick；非 SCX、无 SCX runnable 或
 * BPF 给当前 task 无限 slice 时可停，bypass/有限 slice 必须保留 tick 促使轮转。
 * 入参：rq 为当前 CPU 调度上下文稳定队列。
 * 出参/返回：可停 tick 为 true；无副作用。
 * 注意事项：rq->curr 可是已 dequeue 的陈旧 EXT 指针，先用 nr_running 排除它再看 slice 标志。
 */
bool scx_can_stop_tick(struct rq *rq)
{
	struct task_struct *p = rq->curr;
	struct scx_sched *sch = scx_task_sched(p);

	if (p->sched_class != &ext_sched_class)
		return true;

	/*
	 * @rq->curr may still reference an outgoing EXT task after it has been
	 * dequeued. If no EXT tasks are accounted on @rq, ignore its stale
	 * slice state. If another task is dispatched from a DSQ,
	 * set_next_task_scx() will update the dependency for the incoming task.
	 */
/* bypass 强制有限 slice 周期性回内核，故无条件保持调度 tick。 */
	if (!rq->scx.nr_running)
		return true;

	/* bypass 强制有限 slice 周期性回内核，故无条件保持调度 tick。 */
	if (scx_bypassing(sch, cpu_of(rq)))
		return false;

	/*
	 * @rq can dispatch from different DSQs, so we can't tell whether it
	 * needs the tick or not by looking at nr_running. Allow stopping ticks
	 * iff the BPF scheduler indicated so. See set_next_task_scx().
	 */
/* cgroup 属性写热路径取读侧，scheduler enable/disable 取写侧冻结全部 BPF cgroup 回调。 */
	return rq->scx.flags & SCX_RQ_CAN_STOP_TICK;
}
#endif

#ifdef CONFIG_EXT_GROUP_SCHED

DEFINE_STATIC_PERCPU_RWSEM(scx_cgroup_ops_rwsem);
/* cgroup 属性写热路径取读侧，scheduler enable/disable 取写侧冻结全部 BPF cgroup 回调。 */
static bool scx_cgroup_enabled;

/* 初始化新 task_group 的 SCX 权重、带宽与 idle 默认值；对象尚未向 BPF 发布。 */
void scx_tg_init(struct task_group *tg)
{
	tg->scx.weight = CGROUP_WEIGHT_DFL;
	tg->scx.bw_period_us = default_bw_period_us();
	tg->scx.bw_quota_us = RUNTIME_INF;
	tg->scx.idle = false;
}

/*
 * task_group online 时以当前属性调用可选 cgroup_init；成功同时置 ONLINE/INITED，
 * SCX 未启用时只记 ONLINE，稍后全量 scx_cgroup_init 再补 BPF 初始化。
 * 返回 0 或规范化负 errno；失败不置 INITED，cgroup core 负责中止上线。
 */
int scx_tg_online(struct task_group *tg)
{
	struct scx_sched *sch = scx_root;
	int ret = 0;

	WARN_ON_ONCE(tg->scx.flags & (SCX_TG_ONLINE | SCX_TG_INITED));

	if (scx_cgroup_enabled) {
		/* 已启用时 online 必须同步完成可选 BPF init，失败可阻止 cgroup 上线。 */
		if (SCX_HAS_OP(sch, cgroup_init)) {
			struct scx_cgroup_init_args args =
				{ .weight = tg->scx.weight,
				  .bw_period_us = tg->scx.bw_period_us,
				  .bw_quota_us = tg->scx.bw_quota_us,
				  .bw_burst_us = tg->scx.bw_burst_us };

			/* 回调在 ONLINE 提交前运行；拒绝时 tg 不进入 BPF 初始化状态。 */
			ret = SCX_CALL_OP_RET(sch, cgroup_init,
					      NULL, tg->css.cgroup, &args);
			if (ret)
				ret = ops_sanitize_err(sch, "cgroup_init", ret);
		}
		if (ret == 0)
			tg->scx.flags |= SCX_TG_ONLINE | SCX_TG_INITED;
	} else {
		/* scheduler 未启用只记 core ONLINE，未来全量 init 再补 BPF 状态。 */
		tg->scx.flags |= SCX_TG_ONLINE;
	}

	return ret;
}

/* group offline 与 online 对称：仅 INITED 实体调用一次 cgroup_exit，随后同时清两状态位。 */
void scx_tg_offline(struct task_group *tg)
{
	struct scx_sched *sch = scx_root;

	WARN_ON_ONCE(!(tg->scx.flags & SCX_TG_ONLINE));

	if (scx_cgroup_enabled && SCX_HAS_OP(sch, cgroup_exit) &&
	    (tg->scx.flags & SCX_TG_INITED))
		SCX_CALL_OP(sch, cgroup_exit, NULL, tg->css.cgroup);
	tg->scx.flags &= ~(SCX_TG_ONLINE | SCX_TG_INITED);
}

/*
 * 业务背景：cgroup attach 两阶段事务的 prepare；逐 task 调 cgroup_prep_move 并把源
 * cgroup 暂存在 cgrp_moving_from，任一失败则对已准备成员逆向 cancel。
 * 入参：tset 为 cgroup core 稳定的迁移集合；函数不取得 task/cgroup 长期引用。
 * 出参/返回：全部准备成功 0；失败返回规范化 errno且清所有暂存源。
 * 注意事项：identity move 跳过，使 prep/move/cancel 回调严格一一对应。
 */
int scx_cgroup_can_attach(struct cgroup_taskset *tset)
{
	struct scx_sched *sch = scx_root;
	struct cgroup_subsys_state *css;
	struct task_struct *p;
	int ret;

	if (!scx_cgroup_enabled)
		/* 无活动 BPF cgroup 协议时迁移由 core 直接放行。 */
		return 0;

	cgroup_taskset_for_each(p, css, tset) {
		struct cgroup *from = tg_cgrp(task_group(p));
		struct cgroup *to = tg_cgrp(css_tg(css));

		/* moving_from 是 prepare 日志，也是 commit/cancel 是否欠回调的判据。 */
		WARN_ON_ONCE(p->scx.cgrp_moving_from);

		/*
		 * sched_move_task() omits identity migrations. Let's match the
		 * behavior so that ops.cgroup_prep_move() and ops.cgroup_move()
		 * always match one-to-one.
		 */
/* sched_move_task 同样省略 identity move，BPF 不应收到无实际状态变化的回调。 */
		if (from == to)
			/* sched_move_task 同样省略 identity move，BPF 不应收到无实际状态变化的回调。 */
			continue;

		if (SCX_HAS_OP(sch, cgroup_prep_move)) {
			ret = SCX_CALL_OP_RET(sch, cgroup_prep_move, NULL,
					      p, from, css->cgroup);
			if (ret)
				goto err;
		}

		/* 仅 prep 成功后发布源组，错误扫描不会 cancel 未准备 task。 */
		p->scx.cgrp_moving_from = from;
	}

	return 0;

err:
	/* 已 prepare 的 task 逐个 cancel；未 prepare 的 moving_from 为空，循环可统一清理。 */
	cgroup_taskset_for_each(p, css, tset) {
		if (SCX_HAS_OP(sch, cgroup_cancel_move) &&
		    p->scx.cgrp_moving_from)
			SCX_CALL_OP(sch, cgroup_cancel_move, NULL,
				    p, p->scx.cgrp_moving_from, css->cgroup);
		p->scx.cgrp_moving_from = NULL;
	}

	return ops_sanitize_err(sch, "cgroup_prep_move", ret);
}

/* attach 提交阶段：只有 prepare 留下 moving_from 才调用一次 cgroup_move，随后无条件清暂存源。 */
void scx_cgroup_move_task(struct task_struct *p)
{
	struct scx_sched *sch = scx_root;

	if (!scx_cgroup_enabled)
		return;

	/*
	 * scx_cgroup_can_attach() sets cgrp_moving_from only when the task's
	 * cgroup changes. Migration keys off css rather than cgroup identity,
	 * so it can hand an unchanged-cgroup task here with cgrp_moving_from
	 * NULL. Nothing to report to the BPF scheduler then, so skip it and
	 * keep prep_move and move paired.
	 */
	/* NULL 日志表示 identity move 或未 prepare，不能凭当前 css 伪造 move。 */
	if (SCX_HAS_OP(sch, cgroup_move) && p->scx.cgrp_moving_from)
		SCX_CALL_OP_TASK(sch, cgroup_move, task_rq(p),
				 p, p->scx.cgrp_moving_from,
				 tg_cgrp(task_group(p)));
	p->scx.cgrp_moving_from = NULL;
}

/* attach 被 core 取消时对所有已 prepare task 发 cancel_move，并清 cgrp_moving_from。 */
void scx_cgroup_cancel_attach(struct cgroup_taskset *tset)
{
	struct scx_sched *sch = scx_root;
	struct cgroup_subsys_state *css;
	struct task_struct *p;

	if (!scx_cgroup_enabled)
		return;

	/* 每个 task 只消费自己的 prepare 日志，回调缺省时也必须清字段。 */
	cgroup_taskset_for_each(p, css, tset) {
		if (SCX_HAS_OP(sch, cgroup_cancel_move) &&
		    p->scx.cgrp_moving_from)
			SCX_CALL_OP(sch, cgroup_cancel_move, NULL,
				    p, p->scx.cgrp_moving_from, css->cgroup);
		p->scx.cgrp_moving_from = NULL;
	}
}

/*
 * 更新 cgroup 权重：rwsem 读侧与 SCX 装卸互斥；值变化且 BPF 已启用时先回调，再提交缓存。
 * tg 为稳定 task_group，weight 使用 cgroup 尺度；无直接返回，回调不可拒绝 core 更新。
 */
void scx_group_set_weight(struct task_group *tg, unsigned long weight)
{
	struct scx_sched *sch;

	percpu_down_read(&scx_cgroup_ops_rwsem);
	sch = scx_root;

	/* 值未变化省掉 BPF 回调，但缓存写仍保持 core 真值。 */
	if (scx_cgroup_enabled && SCX_HAS_OP(sch, cgroup_set_weight) &&
	    tg->scx.weight != weight)
		SCX_CALL_OP(sch, cgroup_set_weight, NULL, tg_cgrp(tg), weight);

	tg->scx.weight = weight;

	percpu_up_read(&scx_cgroup_ops_rwsem);
}

/* 更新 cgroup idle 属性并通知 BPF；无返回/失败通道，最后总把 tg 缓存设为 core 真值。 */
void scx_group_set_idle(struct task_group *tg, bool idle)
{
	struct scx_sched *sch;

	percpu_down_read(&scx_cgroup_ops_rwsem);
	sch = scx_root;

	if (scx_cgroup_enabled && SCX_HAS_OP(sch, cgroup_set_idle))
		SCX_CALL_OP(sch, cgroup_set_idle, NULL, tg_cgrp(tg), idle);

	/* Update the task group's idle state */
	/* 回调先看到旧缓存与新参数，随后本地字段成为后续 init/查询使用的当前值。 */
	tg->scx.idle = idle;

	percpu_up_read(&scx_cgroup_ops_rwsem);
}

/*
 * 更新 period/quota/burst 微秒三元组；任一字段变化才通知 BPF，rwsem 保证实例不会在
 * 回调中途卸载。无返回值，core 属性最终无条件写入 tg 缓存。
 */
void scx_group_set_bandwidth(struct task_group *tg,
			     u64 period_us, u64 quota_us, u64 burst_us)
{
	struct scx_sched *sch;

	percpu_down_read(&scx_cgroup_ops_rwsem);
	sch = scx_root;

	/* 任一字段变化才调用 BPF，三元组作为一次原子业务更新传入。 */
	if (scx_cgroup_enabled && SCX_HAS_OP(sch, cgroup_set_bandwidth) &&
	    (tg->scx.bw_period_us != period_us ||
	     tg->scx.bw_quota_us != quota_us ||
	     tg->scx.bw_burst_us != burst_us))
		SCX_CALL_OP(sch, cgroup_set_bandwidth, NULL,
			    tg_cgrp(tg), period_us, quota_us, burst_us);

	/* 三字段作为同一快照在 rwsem 临界区提交，供后续 cgroup_init 复用。 */
	tg->scx.bw_period_us = period_us;
	tg->scx.bw_quota_us = quota_us;
	tg->scx.bw_burst_us = burst_us;

	percpu_up_read(&scx_cgroup_ops_rwsem);
}
#endif	/* CONFIG_EXT_GROUP_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_GROUP_SCHED 对应配置启用时包含其中实现。 */

#if defined(CONFIG_EXT_GROUP_SCHED) || defined(CONFIG_EXT_SUB_SCHED)
/* 返回默认层级 root cgroup 的借用指针；仅在 group/sub-scheduler 配置存在。 */
static struct cgroup *root_cgroup(void)
{
	return &cgrp_dfl_root.cgrp;
}

/* 先冻结 SCX cgroup ops，再取 cgroup_mutex，形成 enable/disable 使用的统一锁序。 */
static void scx_cgroup_lock(void)
{
#ifdef CONFIG_EXT_GROUP_SCHED
	percpu_down_write(&scx_cgroup_ops_rwsem);
#endif
	cgroup_lock();
}

/* 按逆序释放 cgroup_mutex 与 ops rwsem 写侧，结束全层级稳定窗口。 */
static void scx_cgroup_unlock(void)
{
	cgroup_unlock();
#ifdef CONFIG_EXT_GROUP_SCHED
	percpu_up_write(&scx_cgroup_ops_rwsem);
#endif
}
#else	/* CONFIG_EXT_GROUP_SCHED || CONFIG_EXT_SUB_SCHED */
/* 两种层级功能均关闭时用空 stub，让 root-only 主路径无需散布条件编译。 */
static inline struct cgroup *root_cgroup(void) { return NULL; }
static inline void scx_cgroup_lock(void) {}
static inline void scx_cgroup_unlock(void) {}
#endif	/* CONFIG_EXT_GROUP_SCHED || CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_GROUP_SCHED || CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

#ifdef CONFIG_EXT_SUB_SCHED
/* 返回 sub-scheduler 绑定 cgroup 的借用指针；生命周期由 scheduler 拥有并经控制锁稳定。 */
static struct cgroup *sch_cgroup(struct scx_sched *sch)
{
	return sch->cgrp;
}

/* for each descendant of @cgrp including self, set ->scx_sched to @sch */
/*
 * 上游契约：把 cgrp 自身及全部 live 后代的 scheduler 归属以 RCU 指针批量发布。
 * 调用者持 cgroup/scheduler 控制锁；sch 可为父实例，用于子实例退出后的整树回退。
 */
static void set_cgroup_sched(struct cgroup *cgrp, struct scx_sched *sch)
{
	struct cgroup *pos;
	struct cgroup_subsys_state *css;

	cgroup_for_each_live_descendant_pre(pos, css, cgrp)
		rcu_assign_pointer(pos->scx_sched, sch);
}
#else	/* CONFIG_EXT_SUB_SCHED */
/* 无 sub 支持时 scheduler 不绑定 cgroup，归属发布退化为空操作。 */
static inline struct cgroup *sch_cgroup(struct scx_sched *sch) { return NULL; }
static inline void set_cgroup_sched(struct cgroup *cgrp, struct scx_sched *sch) {}
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

/*
 * Omitted operations:
 *
 * - migrate_task_rq: Unnecessary as task to cpu mapping is transient.
 *
 * - task_fork/dead: We need fork/dead notifications for all tasks regardless of
 *   their current sched_class. Call them directly from sched core instead.
 */
/* 该表把 core 调度类生命周期映射到上文实现；未列出的 fork/dead 由 core 全 task hook 直调。 */
DEFINE_SCHED_CLASS(ext) = {
	/* 该表把 core 调度类生命周期映射到上文实现；未列出的 fork/dead 由 core 全 task hook 直调。 */
	.enqueue_task		= enqueue_task_scx,
	.dequeue_task		= dequeue_task_scx,
	.yield_task		= yield_task_scx,
	.yield_to_task		= yield_to_task_scx,

	.wakeup_preempt		= wakeup_preempt_scx,

	.pick_task		= pick_task_scx,

	/* put_prev/set_next 成对维护 runtime、runnable 链和 DSQ 所有权。 */
	.put_prev_task		= put_prev_task_scx,
	.set_next_task		= set_next_task_scx,

	.select_task_rq		= select_task_rq_scx,
	.task_woken		= task_woken_scx,
	.set_cpus_allowed	= set_cpus_allowed_scx,

	/* CPU 生命周期与 tick 回调维护 rq 能力/时间片，不承担 task 创建销毁。 */
	.rq_online		= rq_online_scx,
	.rq_offline		= rq_offline_scx,

	.task_tick		= task_tick_scx,

	.switching_to		= switching_to_scx,
	.switched_from		= switched_from_scx,
	.switched_to		= switched_to_scx,
	/* class/优先级变化归并为权重通知，update_curr 是 runtime 唯一结算入口。 */
	.reweight_task		= reweight_task_scx,
	.prio_changed		= prio_changed_scx,

	.update_curr		= update_curr_scx,

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};

/*
 * 业务背景：构造 builtin/user DSQ 的锁、FIFO/PRIQ 状态及每 CPU deferred reenq 槽。
 * 入参：dsq 为未发布输出对象；dsq_id 为稳定标识；sch 为拥有实例借用指针。
 * 出参/返回：成功 0；per-CPU 分配失败 -ENOMEM，调用者负责释放外层 dsq。
 * 注意事项：成功后每个 pcpu 反指 dsq；哈希/RCU 发布由更高层在完全初始化后执行。
 */
static s32 init_dsq(struct scx_dispatch_q *dsq, u64 dsq_id,
		    struct scx_sched *sch)
{
	s32 cpu;

	memset(dsq, 0, sizeof(*dsq));
	/* 清派生计数后显式初始化锁/list；RB_ROOT 和 RCU fast pointer 的零值是合法空态。 */

	raw_spin_lock_init(&dsq->lock);
	INIT_LIST_HEAD(&dsq->list);
	dsq->id = dsq_id;
	dsq->sched = sch;

	dsq->pcpu = alloc_percpu(struct scx_dsq_pcpu);
	if (!dsq->pcpu)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		/* 每 rq 先作废旧 scheduler 时钟，再在 rq lock 下原子调整 ext/fair server 带宽。 */
		/* possible 而非 online：hotplug 后该 CPU 仍需已有 deferred 槽，不能运行期补分配。 */
		struct scx_dsq_pcpu *pcpu = per_cpu_ptr(dsq->pcpu, cpu);

		pcpu->dsq = dsq;
		INIT_LIST_HEAD(&pcpu->deferred_reenq_user.node);
	}

	return 0;
}

/*
 * 业务背景：DSQ 已摘表并经过 RCU 后撤销所有 per-CPU deferred 节点，再释放 pcpu。
 * 入参：dsq 已无新读者/生产者且不含 task。
 * 出参/返回：无；所有遗留 dru 从对应 rq 链摘除，释放 per-CPU 内存。
 * 注意事项：非空节点表示协议异常，仍在 rq deferred lock 下恢复，避免释放后链表 UAF。
 */
static void exit_dsq(struct scx_dispatch_q *dsq)
{
	s32 cpu;

	for_each_possible_cpu(cpu) {
		struct scx_dsq_pcpu *pcpu = per_cpu_ptr(dsq->pcpu, cpu);
		struct scx_deferred_reenq_user *dru = &pcpu->deferred_reenq_user;
		struct rq *rq = cpu_rq(cpu);

		/*
		 * There must have been a RCU grace period since the last
		 * insertion and @dsq should be off the deferred list by now.
		 */
		/* grace period 应覆盖最后 producer；告警分支是防御性清链，不是正常 ownership 路径。 */
		if (WARN_ON_ONCE(!list_empty(&dru->node))) {
			guard(raw_spinlock_irqsave)(&rq->scx.deferred_reenq_lock);
			list_del_init(&dru->node);
		}
	}

	free_percpu(dsq->pcpu);
}

/* user DSQ 从哈希和调度路径摘除后，等待 RCU 读者退出再释放。 */
/* RCU 回调拥有 dsq 最终释放权：先撤 per-CPU 资源，再 kfree 外层对象。 */
static void free_dsq_rcufn(struct rcu_head *rcu)
{
	struct scx_dispatch_q *dsq = container_of(rcu, struct scx_dispatch_q, rcu);

	exit_dsq(dsq);
	kfree(dsq);
}

/*
 * irq_work 批量取走无锁待释放链，并为每个 DSQ 启动 call_rcu；两阶段跳板避免在
 * scheduler 锁/已有 RCU 嵌套中直接安排复杂最终回收。
 */
static void free_dsq_irq_workfn(struct irq_work *irq_work)
{
	struct llist_node *to_free = llist_del_all(&dsqs_to_free);
	struct scx_dispatch_q *dsq, *tmp_dsq;

	llist_for_each_entry_safe(dsq, tmp_dsq, to_free, free_node)
		call_rcu(&dsq->rcu, free_dsq_rcufn);
}

static DEFINE_IRQ_WORK(free_dsq_irq_work, free_dsq_irq_workfn);

/* 从哈希摘除 DSQ 并迁走残留 task；实际内存回收延迟到 RCU/irq_work。 */
/*
 * 业务背景：销毁空 user DSQ；RCU 查找稳定对象，DSQ lock 串行 nr/id/hash 摘除，随后
 * INVALID id 阻止已拿旧指针的 dispatch_enqueue 继续使用。
 * 入参：sch 为拥有实例；dsq_id 为用户标识。
 * 出参/返回：无；不存在为空操作，在用触发 scheduler error，成功后异步 RCU 释放。
 * 注意事项：不能直接 kfree；irq_work 合并 llist 生产并把 call_rcu 移出 scheduler 锁域。
 */
static void destroy_dsq(struct scx_sched *sch, u64 dsq_id)
{
	struct scx_dispatch_q *dsq;
	unsigned long flags;

	rcu_read_lock();

	dsq = find_user_dsq(sch, dsq_id);
	if (!dsq)
		goto out_unlock_rcu;

	raw_spin_lock_irqsave(&dsq->lock, flags);

	if (dsq->nr) {
		/* 非空 DSQ 销毁会遗失 runnable task，判为策略错误并保持队列继续可用。 */
		scx_error(sch, "attempting to destroy in-use dsq 0x%016llx (nr=%u)",
			  dsq->id, dsq->nr);
		goto out_unlock_dsq;
	}

	if (rhashtable_remove_fast(&sch->dsq_hash, &dsq->hash_node,
				   dsq_hash_params))
		goto out_unlock_dsq;

	/*
	 * Mark dead by invalidating ->id to prevent dispatch_enqueue() from
	 * queueing more tasks. As this function can be called from anywhere,
	 * freeing is bounced through an irq work to avoid nesting RCU
	 * operations inside scheduler locks.
	 */
	/* 摘哈希后先 invalid 发布死亡状态，再只由首个 llist_add 负责投递共享 irq_work。 */
	dsq->id = SCX_DSQ_INVALID;
	if (llist_add(&dsq->free_node, &dsqs_to_free))
		irq_work_queue(&free_dsq_irq_work);

out_unlock_dsq:
	raw_spin_unlock_irqrestore(&dsq->lock, flags);
out_unlock_rcu:
	rcu_read_unlock();
}

#ifdef CONFIG_EXT_GROUP_SCHED
/* 冻结 cgroup 生命周期后以后序遍历退出所有 INITED group，子组先于父组释放 BPF 状态。 */
static void scx_cgroup_exit(struct scx_sched *sch)
{
	struct cgroup_subsys_state *css;

	scx_cgroup_enabled = false;
	/* 先关闭新属性/move 回调，再遍历现存对象，避免 teardown 中产生新 BPF 访问。 */

	/*
	 * scx_tg_on/offline() are excluded through cgroup_lock(). If we walk
	 * cgroups and exit all the inited ones, all online cgroups are exited.
	 */
	/* cgroup_lock 排除并发 tg 上下线；按后序退出已初始化组，返回时所有在线组都已完成 exit。 */
	css_for_each_descendant_post(css, &root_task_group.css) {
		struct task_group *tg = css_tg(css);

		if (!(tg->scx.flags & SCX_TG_INITED))
			continue;
		tg->scx.flags &= ~SCX_TG_INITED;

		if (!sch->ops.cgroup_exit)
			continue;

		SCX_CALL_OP(sch, cgroup_exit, NULL, css->cgroup);
	}
}

/*
 * enable 阶段先序初始化所有 ONLINE 未 INITED group，父组先于子组；全部成功后才发布
 * scx_cgroup_enabled。失败返回 BPF errno，外层调用 scx_cgroup_exit 回滚已完成成员。
 */
static int scx_cgroup_init(struct scx_sched *sch)
{
	struct cgroup_subsys_state *css;
	int ret;

	/*
	 * scx_tg_on/offline() are excluded through cgroup_lock(). If we walk
	 * cgroups and init, all online cgroups are initialized.
	 */
/* 以 tg 缓存属性构造一次性参数快照，成功后才置 INITED。 */
	css_for_each_descendant_pre(css, &root_task_group.css) {
		struct task_group *tg = css_tg(css);
		/* 以 tg 缓存属性构造一次性参数快照，成功后才置 INITED。 */
		struct scx_cgroup_init_args args = {
			.weight = tg->scx.weight,
			.bw_period_us = tg->scx.bw_period_us,
			.bw_quota_us = tg->scx.bw_quota_us,
			.bw_burst_us = tg->scx.bw_burst_us,
		};

		if ((tg->scx.flags &
		     (SCX_TG_ONLINE | SCX_TG_INITED)) != SCX_TG_ONLINE)
			continue;
		/* 只处理当前 online 且尚未 init 的 group，避免重复回调或初始化离线对象。 */

		if (!sch->ops.cgroup_init) {
			/* 无回调也置 INITED，统一让 exit/状态机知道该 group 已经过 enable 阶段。 */
			tg->scx.flags |= SCX_TG_INITED;
			continue;
		}

		ret = SCX_CALL_OP_RET(sch, cgroup_init, NULL,
				      css->cgroup, &args);
		if (ret) {
			/* 首错终止；enable 外层会对已置 INITED 的前缀逐组 exit。 */
			scx_error(sch, "ops.cgroup_init() failed (%d)", ret);
			return ret;
		}
		tg->scx.flags |= SCX_TG_INITED;
	}

	WARN_ON_ONCE(scx_cgroup_enabled);
	/* 全量成功才打开运行期 cgroup ops 门，读者不会看到半初始化层级。 */
	scx_cgroup_enabled = true;

	return 0;
}

#else
static void scx_cgroup_exit(struct scx_sched *sch) {}
static int scx_cgroup_init(struct scx_sched *sch) { return 0; }
#endif


/********************************************************************************
 * Sysfs interface and ops enable/disable.
 */
/*
 * 以下 sysfs 层把全局状态与每 scheduler 诊断暴露为只读属性；show 回调只生成单次
 * 快照，不稳定实例跨调用。后半部分承载 enable/disable 资源事务与故障恢复。
 */

#define SCX_ATTR(_name)								\
	static struct kobj_attribute scx_attr_##_name = {			\
		.attr = { .name = __stringify(_name), .mode = 0444 },		\
		.show = scx_attr_##_name##_show,				\
	}
/* SCX_ATTR 为同名 show 生成 0444 kobj_attribute；不创建 store，因此用户不能由此改状态。 */

static ssize_t scx_attr_state_show(struct kobject *kobj,
				   struct kobj_attribute *ka, char *buf)
{
	/* 原子读取 enable 枚举并映射稳定字符串；返回写入 buf 的字节数。 */
	return sysfs_emit(buf, "%s\n", scx_enable_state_str[scx_enable_state()]);
}
SCX_ATTR(state);

static ssize_t scx_attr_switch_all_show(struct kobject *kobj,
					struct kobj_attribute *ka, char *buf)
{
	/* READ_ONCE 输出 root 是否接管全部普通 task 的瞬时布尔值。 */
	return sysfs_emit(buf, "%d\n", READ_ONCE(scx_switching_all));
}
SCX_ATTR(switch_all);

static ssize_t scx_attr_nr_rejected_show(struct kobject *kobj,
					 struct kobj_attribute *ka, char *buf)
{
	/* 原子累计值统计 disallow 导致的 SCHED_EXT→NORMAL 回退次数。 */
	return sysfs_emit(buf, "%ld\n", atomic_long_read(&scx_nr_rejected));
}
SCX_ATTR(nr_rejected);

static ssize_t scx_attr_hotplug_seq_show(struct kobject *kobj,
					 struct kobj_attribute *ka, char *buf)
{
	/* hotplug 单调序列用于用户态/enable 检测初始化期间拓扑是否变化。 */
	return sysfs_emit(buf, "%ld\n", atomic_long_read(&scx_hotplug_seq));
}
SCX_ATTR(hotplug_seq);

static ssize_t scx_attr_enable_seq_show(struct kobject *kobj,
					struct kobj_attribute *ka, char *buf)
{
	/* enable_seq 是历史成功次数而非当前实例引用数。 */
	return sysfs_emit(buf, "%ld\n", atomic_long_read(&scx_enable_seq));
}
SCX_ATTR(enable_seq);

static struct attribute *scx_global_attrs[] = {
	/* NULL 终止数组供 sysfs core 遍历；各 attr 均为静态生命周期。 */
	&scx_attr_state.attr,
	&scx_attr_switch_all.attr,
	&scx_attr_nr_rejected.attr,
	&scx_attr_hotplug_seq.attr,
	&scx_attr_enable_seq.attr,
	NULL,
};

static const struct attribute_group scx_global_attr_group = {
	/* 全局目录仅公开跨实例状态；每 scheduler 诊断位于其 kobject。 */
	.attrs = scx_global_attrs,
};

static void free_pnode(struct scx_sched_pnode *pnode);
static void free_exit_info(struct scx_exit_info *ei);

/*
 * 业务背景：CID scheduler 的 set_cmask 回调需要每 CPU arena scratch，避免 rq 锁热路径
 * 动态分配；普通 CPU 类型或无 arena pool 时无需资源。
 * 入参：sch 为尚未发布实例。
 * 出参/返回：成功/不适用 0，per-CPU 或 arena 任一分配失败 -ENOMEM。
 * 注意事项：部分分配由统一 free 遍历回收；槽只在 rq lock+IRQ disabled 下独占。
 */
static s32 scx_set_cmask_scratch_alloc(struct scx_sched *sch)
{
	size_t size = struct_size_t(struct scx_cmask, bits,
				    SCX_CMASK_NR_WORDS(num_possible_cpus()));
	int cpu;

	if (!sch->is_cid_type || !sch->arena_pool)
		return 0;

	sch->set_cmask_scratch = alloc_percpu(struct scx_cmask *);
	if (!sch->set_cmask_scratch)
		return -ENOMEM;

	/* 每 CPU 槽分配 arena cmask，rq 热路径只初始化/填充而不再申请内存。 */
	for_each_possible_cpu(cpu) {
		struct scx_cmask **slot = per_cpu_ptr(sch->set_cmask_scratch, cpu);

		*slot = scx_arena_alloc(sch, size);
		if (!*slot)
			return -ENOMEM;
		scx_cmask_init(*slot, 0, num_possible_cpus());
	}
	return 0;
}

/* 释放所有 possible CPU arena cmask 与 per-CPU 槽；空指针幂等，最后清成员防重复释放。 */
static void scx_set_cmask_scratch_free(struct scx_sched *sch)
{
	size_t size = struct_size_t(struct scx_cmask, bits,
				    SCX_CMASK_NR_WORDS(num_possible_cpus()));
	int cpu;

	if (!sch->set_cmask_scratch)
		return;

	for_each_possible_cpu(cpu) {
		struct scx_cmask **slot = per_cpu_ptr(sch->set_cmask_scratch, cpu);

		/* arena_free 接受部分初始化槽；统一遍历支持 allocator 失败回滚。 */
		scx_arena_free(sch, *slot, size);
	}
	free_percpu(sch->set_cmask_scratch);
	sch->set_cmask_scratch = NULL;
}

/*
 * 业务背景：scheduler kobject 最后引用消失后，经过 RCU work 在可睡眠上下文做最终资源
 * 析构；顺序先停止异步执行源，再释放其可能访问的 per-CPU/DSQ/arena/BPF 对象。
 * 入参：work 嵌入 sch；此回调取得 scheduler 最终 ownership。
 * 出参/返回：无；销毁全部资源并 kfree sch。
 * 注意事项：先 sync irq/kthread/timer；user DSQ 仍经 destroy+RCU 路径，哈希 walk 处理 EAGAIN。
 */
static void scx_sched_free_rcu_work(struct work_struct *work)
{
	struct rcu_work *rcu_work = to_rcu_work(work);
	struct scx_sched *sch = container_of(rcu_work, struct scx_sched, rcu_work);
	struct rhashtable_iter rht_iter;
	struct scx_dispatch_q *dsq;
	int cpu, node;

	irq_work_sync(&sch->disable_irq_work);
	/* 阻止新的/在途异步回调后才可释放它们引用的 sch 字段。 */
	kthread_destroy_worker(sch->helper);
	timer_shutdown_sync(&sch->bypass_lb_timer);
	free_cpumask_var(sch->bypass_lb_donee_cpumask);
	free_cpumask_var(sch->bypass_lb_resched_cpumask);

#ifdef CONFIG_EXT_SUB_SCHED
	/* 层级对象先归还 cgroup/kset/parent 引用，再释放 task/DSQ 数据平面。 */
	kfree(sch->cgrp_path);
	if (sch_cgroup(sch))
		cgroup_put(sch_cgroup(sch));
	if (sch->sub_kset)
		kobject_put(&sch->sub_kset->kobj);
	if (scx_parent(sch))
		/* child 生存期取得的 parent 基础引用在最终 RCU 回收处归还。 */
		kobject_put(&scx_parent(sch)->kobj);
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

	for_each_possible_cpu(cpu) {
		struct scx_sched_pcpu *pcpu = per_cpu_ptr(sch->pcpu, cpu);

		/*
		 * $sch would have entered bypass mode before the RCU grace
		 * period. As that blocks new deferrals, all
		 * deferred_reenq_local_node's must be off-list by now.
		 */
/* bypass 早于 RCU 宽限期阻止新 deferral，故正常此节点已由 consumer 摘除。 */
		WARN_ON_ONCE(!list_empty(&pcpu->deferred_reenq_local.node));
		/* bypass 早于 RCU 宽限期阻止新 deferral，故正常此节点已由 consumer 摘除。 */

		exit_dsq(bypass_dsq(sch, cpu));
	}

	free_percpu(sch->pcpu);

	for_each_node_state(node, N_POSSIBLE)
		free_pnode(sch->pnode[node]);
	kfree(sch->pnode);

	rhashtable_walk_enter(&sch->dsq_hash, &rht_iter);
	/* 逐个 user DSQ 走正常 INVALID/irq_work/RCU 回收，不能直接释放仍被 fastpath 借用者。 */
	do {
		rhashtable_walk_start(&rht_iter);

		while (!IS_ERR_OR_NULL((dsq = rhashtable_walk_next(&rht_iter))))
			destroy_dsq(sch, dsq->id);

		rhashtable_walk_stop(&rht_iter);
	} while (dsq == ERR_PTR(-EAGAIN));
	rhashtable_walk_exit(&rht_iter);

	/* user DSQ 全部进入正常回收后，才销毁哈希本体与其余 scheduler 资源。 */
	rhashtable_free_and_destroy(&sch->dsq_hash, NULL, NULL);
	free_exit_info(sch->exit_info);
	scx_set_cmask_scratch_free(sch);
	scx_arena_pool_destroy(sch);
	if (sch->arena_map)
		bpf_map_put(sch->arena_map);
	kfree(sch);
}

/* kobject 最后 put 的 release 只排 RCU work，不同步析构，避免在 sysfs/锁上下文睡眠。 */
static void scx_kobj_release(struct kobject *kobj)
{
	struct scx_sched *sch = container_of(kobj, struct scx_sched, kobj);

	INIT_RCU_WORK(&sch->rcu_work, scx_sched_free_rcu_work);
	queue_rcu_work(system_dfl_wq, &sch->rcu_work);
}

/* scheduler sysfs ops 属性：从 kobject 恢复稳定实例并输出 BPF struct_ops 名称。 */
static ssize_t scx_attr_ops_show(struct kobject *kobj,
				 struct kobj_attribute *ka, char *buf)
{
	struct scx_sched *sch = container_of(kobj, struct scx_sched, kobj);

	return sysfs_emit(buf, "%s\n", sch->ops.name);
}
SCX_ATTR(ops);

#define scx_attr_event_show(buf, at, events, kind) ({				\
	sysfs_emit_at(buf, at, "%s %llu\n", #kind, (events)->kind);		\
})

/* 聚合 per-CPU event 快照后逐项输出“名称 值”；返回累计字节数，允许统计并发近似变化。 */
static ssize_t scx_attr_events_show(struct kobject *kobj,
				    struct kobj_attribute *ka, char *buf)
{
	struct scx_sched *sch = container_of(kobj, struct scx_sched, kobj);
	struct scx_event_stats events;
	int at = 0;

	scx_read_events(sch, &events);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_SELECT_CPU_FALLBACK);
	/* 选择、派发、enqueue 与 reenq 计数按固定顺序输出，用户态可稳定 diff。 */
	at += scx_attr_event_show(buf, at, &events, SCX_EV_DISPATCH_LOCAL_DSQ_OFFLINE);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_DISPATCH_KEEP_LAST);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_ENQ_SKIP_EXITING);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_ENQ_SKIP_MIGRATION_DISABLED);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_REENQ_IMMED);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_REENQ_LOCAL_REPEAT);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_REFILL_SLICE_DFL);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_BYPASS_DURATION);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_BYPASS_DISPATCH);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_BYPASS_ACTIVATE);
	/* 所有权与 sub bypass 事件置尾，输出顺序保持稳定便于用户态解析。 */
	at += scx_attr_event_show(buf, at, &events, SCX_EV_INSERT_NOT_OWNED);
	at += scx_attr_event_show(buf, at, &events, SCX_EV_SUB_BYPASS_DISPATCH);
	return at;
}
SCX_ATTR(events);

static struct attribute *scx_sched_attrs[] = {
	/* 实例目录只有 ops 名与事件快照，NULL 终止供 ATTRIBUTE_GROUPS 构造。 */
	&scx_attr_ops.attr,
	&scx_attr_events.attr,
	NULL,
};
ATTRIBUTE_GROUPS(scx_sched);

static const struct kobj_type scx_ktype = {
	.release = scx_kobj_release,
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = scx_sched_groups,
};

/*
 * 为真正 scheduler kobject 的 uevent 增加 SCXOPS=name；parent 链也会把 sub-kset 送入，
 * 因而先检查 ktype，避免把不同布局 container_of 成 scx_sched。
 */
static int scx_uevent(const struct kobject *kobj, struct kobj_uevent_env *env)
{
	const struct scx_sched *sch;

	/*
	 * scx_uevent() can be reached by both scx_sched kobjects (scx_ktype)
	 * and sub-scheduler kset kobjects (kset_ktype) through the parent
	 * chain walk. Filter out the latter to avoid invalid casts.
	 */
/* 通过类型检查后 container_of 才安全，实例名在 kobject 生命周期内稳定。 */
	if (kobj->ktype != &scx_ktype)
		return 0;

	/* 通过类型检查后 container_of 才安全，实例名在 kobject 生命周期内稳定。 */
	sch = container_of(kobj, struct scx_sched, kobj);

	return add_uevent_var(env, "SCXOPS=%s", sch->ops.name);
}

static const struct kset_uevent_ops scx_uevent_ops = {
	.uevent = scx_uevent,
};

/*
 * Used by sched_fork() and __setscheduler_prio() to pick the matching
 * sched_class. dl/rt are already handled.
 */
/*
 * 上游契约：fork/setscheduler 用它选择目标 sched_class。静态键关闭一律非 SCX；
 * switch_all 接管 OTHER/EXT；teardown 则拒绝新 EXT，避免新 task 进入正在撤销的 class。
 * 入参：policy 为目标用户调度策略。
 * 出参/返回：应使用 ext_sched_class 为 true；无副作用。
 * 注意事项：DISABLING 判断必须晚于 switch_all，否则 fair 被 class 链跳过时新 kthread 可死锁装卸。
 */
bool task_should_scx(int policy)
{
	/* if disabled, nothing should be on it */
	/* SCX 关闭时不应有 task 归属其调度类，因此快速返回 false。 */
	if (!scx_enabled())
		return false;

	/* scx is taking over all SCHED_OTHER and SCHED_EXT tasks */
/* switched_all 静态 class 链已排除 fair，过渡期仍必须把普通 task 送入 EXT。 */
	if (READ_ONCE(scx_switching_all))
		/* switched_all 静态 class 链已排除 fair，过渡期仍必须把普通 task 送入 EXT。 */
		return true;

	/*
	 * scx is tearing down - keep new SCHED_EXT tasks out.
	 *
	 * Must come after scx_switching_all test, which serves as a proxy
	 * for __scx_switched_all. While __scx_switched_all is set, we must
	 * return true via the branch above: a fork routed to fair would
	 * stall because next_active_class() skips fair.
	 *
	 * This can develop into a deadlock - scx holds scx_enable_mutex across
	 * kthread_create() in scx_alloc_and_add_sched(); if the new kthread is
	 * the stalled task, the disable path can never grab the mutex to clear
	 * scx_switching_all.
	 */
/* 非 switch_all 实例 teardown 时阻止新 SCHED_EXT，收敛待退出 task 集合。 */
	if (unlikely(scx_enable_state() == SCX_DISABLING))
		/* 非 switch_all 实例 teardown 时阻止新 SCHED_EXT，收敛待退出 task 集合。 */
		return false;

	return policy == SCHED_EXT;
}

/*
 * 业务背景：决定 try_to_wake_up 是否可用通用远端 wake queue；SCX 默认要求在当前
 * wake 路径同步执行策略，只有显式 ALLOW_QUEUED_WAKEUP 或非 EXT/无实例时允许排队。
 * 入参：p 为唤醒协议稳定只读 task。
 * 出参/返回：允许通用 queued wakeup 为 true；无状态修改。
 * 注意事项：scheduler 指针为空覆盖 enable/disable 过渡，安全回退通用机制。
 */
bool scx_allow_ttwu_queue(const struct task_struct *p)
{
	struct scx_sched *sch;

	if (!scx_enabled())
		return true;

	sch = scx_task_sched(p);
	if (unlikely(!sch))
		return true;

	if (sch->ops.flags & SCX_OPS_ALLOW_QUEUED_WAKEUP)
		return true;

	/* 非 ext task 不受 BPF wakeup 同步约束，继续使用 core 的通用远端队列。 */
	if (unlikely(p->sched_class != &ext_sched_class))
		return true;

	return false;
}

/**
 * handle_lockup - sched_ext common lockup handler
 * @fmt: format string
 *
 * Called on system stall or lockup condition and initiates abort of sched_ext
 * if enabled, which may resolve the reported lockup.
 *
 * Returns %true if sched_ext is enabled and abort was initiated, which may
 * resolve the lockup. %false if sched_ext is not enabled or abort was already
 * initiated by someone else.
 */
/*
 * 上游契约：锁死报告公共入口，在 RCU 下取得 root，仅 ENABLING/ENABLED 时尝试登记
 * 格式化错误退出；其他状态已有 teardown，不能重复 claim。
 * 入参：fmt/变参组成诊断消息，仅调用期间借用。
 * 出参/返回：本次成功发起 abort 为 true；无实例、已退出或被他人抢先为 false。
 * 注意事项：可在 watchdog/锁死路径调用，实际 disable 延后执行。
 */
static __printf(1, 2) bool handle_lockup(const char *fmt, ...)
{
	struct scx_sched *sch;
	va_list args;
	bool ret;

	guard(rcu)();

	/* root 指针仅在 RCU 区间借用，claim exit 会把真正 teardown 延后。 */
	sch = rcu_dereference(scx_root);
	if (unlikely(!sch))
		return false;

	switch (scx_enable_state()) {
	case SCX_ENABLING:
	case SCX_ENABLED:
		/* 只有仍可能控制 CPU 的状态值得恢复；scx_verror 内部一次性 claim 退出。 */
		va_start(args, fmt);
		ret = scx_verror(sch, fmt, args);
		va_end(args);
		return ret;
	default:
		return false;
	}
}

/**
 * scx_rcu_cpu_stall - sched_ext RCU CPU stall handler
 *
 * While there are various reasons why RCU CPU stalls can occur on a system
 * that may not be caused by the current BPF scheduler, try kicking out the
 * current scheduler in an attempt to recover the system to a good state before
 * issuing panics.
 *
 * Returns %true if sched_ext is enabled and abort was initiated, which may
 * resolve the reported RCU stall. %false if sched_ext is not enabled or someone
 * else already initiated abort.
 */
/* RCU stall 未必由 BPF 引起，但先撤当前 scheduler 可能在 panic 前恢复系统前进。 */
bool scx_rcu_cpu_stall(void)
{
	/* RCU stall 未必由 BPF 引起，但先撤当前 scheduler 可能在 panic 前恢复系统前进。 */
	return handle_lockup("RCU CPU stall detected!");
}

/**
 * scx_softlockup - sched_ext softlockup handler
 * @dur_s: number of seconds of CPU stuck due to soft lockup
 *
 * On some multi-socket setups (e.g. 2x Intel 8480c), the BPF scheduler can
 * live-lock the system by making many CPUs target the same DSQ to the point
 * where soft-lockup detection triggers. This function is called from
 * soft-lockup watchdog when the triggering point is close and tries to unjam
 * the system and aborting the BPF scheduler.
 */
/* 仅首次成功 claim abort 时打印禁用消息，避免多个 CPU 重复刷屏。 */
void scx_softlockup(u32 dur_s)
{
	/* 仅首次成功 claim abort 时打印禁用消息，避免多个 CPU 重复刷屏。 */
	if (!handle_lockup("soft lockup - CPU %d stuck for %us", smp_processor_id(), dur_s))
		return;

	printk_deferred(KERN_ERR "sched_ext: Soft lockup - CPU %d stuck for %us, disabling BPF scheduler\n",
			smp_processor_id(), dur_s);
}

/*
 * scx_hardlockup() runs from NMI and eventually calls scx_claim_exit(),
 * which takes scx_sched_lock. scx_sched_lock isn't NMI-safe and grabbing
 * it from NMI context can lead to deadlocks. Defer via irq_work; the
 * disable path runs off irq_work anyway.
 */
/* NMI 只原子记录首个 CPU；真正取 scx_sched_lock 的退出逻辑延期到 irq_work。 */
static atomic_t scx_hardlockup_cpu = ATOMIC_INIT(-1);
/* NMI 只原子记录首个 CPU；真正取 scx_sched_lock 的退出逻辑延期到 irq_work。 */

/* IRQ 上下文取走记录 CPU 并调用公共 lockup handler；xchg 同时重新开放下一次报告槽。 */
static void scx_hardlockup_irq_workfn(struct irq_work *work)
{
	int cpu = atomic_xchg(&scx_hardlockup_cpu, -1);

	if (cpu >= 0 && handle_lockup("hard lockup - CPU %d", cpu))
		printk_deferred(KERN_ERR "sched_ext: Hard lockup - CPU %d, disabling BPF scheduler\n",
				cpu);
}

static DEFINE_IRQ_WORK(scx_hardlockup_irq_work, scx_hardlockup_irq_workfn);

/**
 * scx_hardlockup - sched_ext hardlockup handler
 *
 * A poorly behaving BPF scheduler can trigger hard lockup by e.g. putting
 * numerous affinitized tasks in a single queue and directing all CPUs at it.
 * Try kicking out the current scheduler in an attempt to recover the system to
 * a good state before taking more drastic actions.
 *
 * Queues an irq_work; the handle_lockup() call happens in IRQ context (see
 * scx_hardlockup_irq_workfn).
 *
 * Returns %true if sched_ext is enabled and the work was queued, %false
 * otherwise.
 */
/* NMI-safe 快速门禁只用 rcu_access_pointer；无 root 不排 work。 */
bool scx_hardlockup(int cpu)
{
	/* NMI-safe 快速门禁只用 rcu_access_pointer；无 root 不排 work。 */
	if (!rcu_access_pointer(scx_root))
		return false;

	atomic_cmpxchg(&scx_hardlockup_cpu, -1, cpu);
	/* 多 CPU 同时 lockup 仅保留首个编号，irq_work 自带 pending 合并语义。 */
	irq_work_queue(&scx_hardlockup_irq_work);
	return true;
}

/* bypass 下从 donor local DSQ 向空闲 donee 批量迁移，仍遵守 task 亲和性。 */
/*
 * 业务背景：在同 NUMA node 内把过载 donor bypass DSQ 的 task 分散到低载 CPU；目标
 * 只是故障期合理前进，不追求精确最优。
 * 入参：sch/donor、可变 donee/resched mask，以及 donor/donee 目标队列长度。
 * 出参/返回：返回搬运 task 数；mask 同步删满目标并记录需 resched CPU。
 * 注意事项：持 donor rq+DSQ 扫描，嵌套锁 donee DSQ；分批放锁防止长队列垄断。
 */
static u32 bypass_lb_cpu(struct scx_sched *sch, s32 donor,
			 struct cpumask *donee_mask, struct cpumask *resched_mask,
			 u32 nr_donor_target, u32 nr_donee_target)
{
	struct rq *donor_rq = cpu_rq(donor);
	struct scx_dispatch_q *donor_dsq = bypass_dsq(sch, donor);
	struct task_struct *p, *n;
	struct scx_dsq_list_node cursor = INIT_DSQ_LIST_CURSOR(cursor, donor_dsq, 0);
	s32 delta = READ_ONCE(donor_dsq->nr) - nr_donor_target;
	u32 nr_balanced = 0, min_delta_us;

	/* delta 先按无锁队列长度估算，只有足够形成可感知排队延迟才进入重锁扫描。 */
	/*
	 * All we want to guarantee is reasonable forward progress. No reason to
	 * fine tune. Assuming every task on @donor_dsq runs their full slice,
	 * consider offloading iff the total queued duration is over the
	 * threshold.
	 */
/* 估算额外排队时间不足阈值就不搬，避免短暂轻微偏斜造成无谓迁移。 */
	min_delta_us = READ_ONCE(scx_bypass_lb_intv_us) / SCX_BYPASS_LB_MIN_DELTA_DIV;
	if (delta < DIV_ROUND_UP(min_delta_us, READ_ONCE(scx_slice_bypass_us)))
		/* 估算额外排队时间不足阈值就不搬，避免短暂轻微偏斜造成无谓迁移。 */
		return 0;

	raw_spin_rq_lock_irq(donor_rq);
	raw_spin_lock(&donor_dsq->lock);
	list_add(&cursor.node, &donor_dsq->list);
resume:
	n = container_of(&cursor, struct task_struct, scx.dsq_list);
	/* cursor 本身伪装为 task 节点，仅作为 nldsq_next_task 的恢复锚点。 */
	n = nldsq_next_task(donor_dsq, n, false);

	while ((p = n)) {
		struct scx_dispatch_q *donee_dsq;
		int donee;

		n = nldsq_next_task(donor_dsq, n, false);

		/* donor 达目标或 donee 全满即提前结束，避免继续扫描无可执行搬运。 */
		if (donor_dsq->nr <= nr_donor_target)
			break;

		if (cpumask_empty(donee_mask))
			break;

		/*
		 * If an earlier pass placed @p on @donor_dsq from a different
		 * CPU and the donee hasn't consumed it yet, @p is still on the
		 * previous CPU and task_rq(@p) != @donor_rq. @p can't be moved
		 * without its rq locked. Skip.
		 */
/* 早前 pass 放入 donor DSQ 的远端 task 尚未被消费，缺其真实 rq lock 不可再搬。 */
		if (task_rq(p) != donor_rq)
			/* 早前 pass 放入 donor DSQ 的远端 task 尚未被消费，缺其真实 rq lock 不可再搬。 */
			continue;

		donee = cpumask_any_and_distribute(donee_mask, p->cpus_ptr);
		/* 在允许亲和集合中轮转选择 donee，分摊多个可用 CPU。 */
		if (donee >= nr_cpu_ids)
			continue;

		donee_dsq = bypass_dsq(sch, donee);

		/*
		 * $p's rq is not locked but $p's DSQ lock protects its
		 * scheduling properties making this test safe.
		 */
		/* 这里未持 p 的 rq 锁，但源 DSQ 锁稳定其调度属性，足以安全判断远端运行条件。 */
		if (!task_can_run_on_remote_rq(sch, p, cpu_rq(donee), false))
			continue;

		/*
		 * Moving $p from one non-local DSQ to another. The source rq
		 * and DSQ are already locked. Do an abbreviated dequeue and
		 * then perform enqueue without unlocking $donor_dsq.
		 *
		 * We don't want to drop and reacquire the lock on each
		 * iteration as @donor_dsq can be very long and potentially
		 * highly contended. Donee DSQs are less likely to be contended.
		 * The nested locking is safe as only this LB moves tasks
		 * between bypass DSQs.
		 */
/* bypass DSQ 间移动是唯一允许该 nested 锁层级的路径，源锁保持可提升批处理效率。 */
		dispatch_dequeue_locked(p, donor_dsq);
		/* bypass DSQ 间移动是唯一允许该 nested 锁层级的路径，源锁保持可提升批处理效率。 */
		dispatch_enqueue(sch, cpu_rq(donee), donee_dsq, p, SCX_ENQ_NESTED);

		/*
		 * $donee might have been idle and need to be woken up. No need
		 * to be clever. Kick every CPU that receives tasks.
		 */
/* cursor 停在下一 task 前，放锁后可从稳定位置恢复且不追逐新入队。 */
		cpumask_set_cpu(donee, resched_mask);

		if (READ_ONCE(donee_dsq->nr) >= nr_donee_target)
			cpumask_clear_cpu(donee, donee_mask);

		nr_balanced++;
		if (!(nr_balanced % SCX_BYPASS_LB_BATCH) && n) {
			/* cursor 停在下一 task 前，放锁后可从稳定位置恢复且不追逐新入队。 */
			list_move_tail(&cursor.node, &n->scx.dsq_list.node);
			raw_spin_unlock(&donor_dsq->lock);
			raw_spin_rq_unlock_irq(donor_rq);
			cpu_relax();
			raw_spin_rq_lock_irq(donor_rq);
			raw_spin_lock(&donor_dsq->lock);
			goto resume;
		}
	}

	list_del_init(&cursor.node);
	/* 先摘游标再释放 DSQ/rq 锁，防止栈上 cursor 在返回后仍被队列遍历。 */
	raw_spin_unlock(&donor_dsq->lock);
	raw_spin_rq_unlock_irq(donor_rq);

	return nr_balanced;
}

/*
 * 业务背景：计算单 NUMA node 的平均 bypass 队列目标，标出低载 donee，再从超过 donor
 * 阈值的 CPU 分批搬运并 kick 收到 task 的 CPU。
 * 入参：sch 为 bypass host；node 为有 CPU 的 NUMA id。
 * 出参/返回：无；改变各 CPU bypass DSQ 分布并发 trace 记录前后 min/max。
 * 注意事项：统计使用 READ_ONCE 近似快照，目标是避免极端 stall而非强一致全局最优。
 */
static void bypass_lb_node(struct scx_sched *sch, int node)
{
	const struct cpumask *node_mask = cpumask_of_node(node);
	struct cpumask *donee_mask = sch->bypass_lb_donee_cpumask;
	struct cpumask *resched_mask = sch->bypass_lb_resched_cpumask;
	u32 nr_tasks = 0, nr_cpus = 0, nr_balanced = 0;
	u32 nr_target, nr_donor_target;
	u32 before_min = U32_MAX, before_max = 0;
	u32 after_min = U32_MAX, after_max = 0;
	/* min/max 仅作 trace 诊断；平衡决策使用总任务数、CPU 数和目标阈值。 */
	int cpu;

	/* count the target tasks and CPUs */
/* 只平衡 online CPU；hotplug 过渡允许本轮统计近似变化。 */
	for_each_cpu_and(cpu, cpu_online_mask, node_mask) {
		/* 只平衡 online CPU；hotplug 过渡允许本轮统计近似变化。 */
		u32 nr = READ_ONCE(bypass_dsq(sch, cpu)->nr);

		nr_tasks += nr;
		nr_cpus++;

		before_min = min(nr, before_min);
		before_max = max(nr, before_max);
	}

	if (!nr_cpus)
		return;

	/*
	 * We don't want CPUs to have more than $nr_donor_target tasks and
	 * balancing to fill donee CPUs upto $nr_target. Once targets are
	 * calculated, find the donee CPUs.
	 */
/* donee 填到平均上取整，donor 仅在高于放大阈值时输出，减少来回抖动。 */
	nr_target = DIV_ROUND_UP(nr_tasks, nr_cpus);
	/* donee 填到平均上取整，donor 仅在高于放大阈值时输出，减少来回抖动。 */
	nr_donor_target = DIV_ROUND_UP(nr_target * SCX_BYPASS_LB_DONOR_PCT, 100);

	cpumask_clear(donee_mask);
	for_each_cpu_and(cpu, cpu_online_mask, node_mask) {
		if (READ_ONCE(bypass_dsq(sch, cpu)->nr) < nr_target)
			cpumask_set_cpu(cpu, donee_mask);
	}

	/* iterate !donee CPUs and see if they should be offloaded */
/* donee 自身和未超过 donor 阈值的 CPU 不作为源。 */
	cpumask_clear(resched_mask);
	for_each_cpu_and(cpu, cpu_online_mask, node_mask) {
		if (cpumask_empty(donee_mask))
			break;
		/* donee 自身和未超过 donor 阈值的 CPU 不作为源。 */
		if (cpumask_test_cpu(cpu, donee_mask))
			continue;
		if (READ_ONCE(bypass_dsq(sch, cpu)->nr) <= nr_donor_target)
			continue;

		nr_balanced += bypass_lb_cpu(sch, cpu, donee_mask, resched_mask,
					     nr_donor_target, nr_target);
	}

	for_each_cpu(cpu, resched_mask)
		/* 搬运只改 DSQ，统一在锁外 kick 目标 CPU，避免每 task 重复 IPI。 */
		resched_cpu(cpu);

	for_each_cpu_and(cpu, cpu_online_mask, node_mask) {
		u32 nr = READ_ONCE(bypass_dsq(sch, cpu)->nr);

		after_min = min(nr, after_min);
		after_max = max(nr, after_max);

	}

	trace_sched_ext_bypass_lb(node, nr_cpus, nr_tasks, nr_balanced,
				  before_min, before_max, after_min, after_max);
}

/*
 * In bypass mode, all tasks are put on the per-CPU bypass DSQs. If the machine
 * is over-saturated and the BPF scheduler skewed tasks into few CPUs, some
 * bypass DSQs can be overloaded. If there are enough tasks to saturate other
 * lightly loaded CPUs, such imbalance can lead to very high execution latency
 * on the overloaded CPUs and thus to hung tasks and RCU stalls. To avoid such
 * outcomes, a simple load balancing mechanism is implemented by the following
 * timer which runs periodically while bypass mode is in effect.
 */
/* 周期性兜底均衡 bypass DSQ；只安排有限批次，避免 timer 长时间占用 CPU。 */
/* timer 仅在 bypass dispatch 仍启用时遍历 NUMA node；间隔为 0 时完成本轮后不再重排。 */
static void scx_bypass_lb_timerfn(struct timer_list *timer)
{
	struct scx_sched *sch = container_of(timer, struct scx_sched, bypass_lb_timer);
	int node;
	u32 intv_us;

	if (!bypass_dsp_enabled(sch))
		return;

	/* 各 NUMA node 独立均衡，避免故障兜底无谓跨节点破坏局部性。 */
	for_each_node_with_cpus(node)
		bypass_lb_node(sch, node);

	intv_us = READ_ONCE(scx_bypass_lb_intv_us);
	if (intv_us)
		mod_timer(timer, jiffies + usecs_to_jiffies(intv_us));
}

/*
 * 在 bypass_lock 下增加嵌套深度；只有 0→1 返回 true，并切默认 slice 为保底有限值、
 * 记录开始时间/激活事件。嵌套调用只计数，不重复搬运 task。
 */
static bool inc_bypass_depth(struct scx_sched *sch)
{
	lockdep_assert_held(&scx_bypass_lock);

	WARN_ON_ONCE(sch->bypass_depth < 0);
	WRITE_ONCE(sch->bypass_depth, sch->bypass_depth + 1);
	if (sch->bypass_depth != 1)
		return false;

	/* 首层切换默认 slice 并记录计时起点；后续嵌套不覆盖该时间戳。 */
	WRITE_ONCE(sch->slice_dfl, READ_ONCE(scx_slice_bypass_us) * NSEC_PER_USEC);
	sch->bypass_timestamp = ktime_get_ns();
	scx_add_event(sch, SCX_EV_BYPASS_ACTIVATE, 1);
	return true;
}

/* bypass 深度减一；只有 1→0 恢复正常默认 slice、累计持续时间并返回 true。 */
static bool dec_bypass_depth(struct scx_sched *sch)
{
	lockdep_assert_held(&scx_bypass_lock);

	WARN_ON_ONCE(sch->bypass_depth < 1);
	WRITE_ONCE(sch->bypass_depth, sch->bypass_depth - 1);
	if (sch->bypass_depth != 0)
		return false;

	/* 最后一层才恢复正常片长并把完整嵌套持续时间累计到事件。 */
	WRITE_ONCE(sch->slice_dfl, SCX_SLICE_DFL);
	scx_add_event(sch, SCX_EV_BYPASS_DURATION,
		      ktime_get_ns() - sch->bypass_timestamp);
	return true;
}

/* 首层 bypass 把 scheduler 标成内核接管，并重排已有 task 到安全 DSQ。 */
/*
 * 业务背景：开启 scheduler 及其 host 的 bypass dispatch 消费能力；sub-scheduler 的 task
 * 实际放最近未 bypass 祖先，因此两侧独立 enable_depth 都需增加。
 * 入参：sch 已完成 0→1 bypass；可在 bypass_lock 内调用。
 * 出参/返回：无；claim 防重复，增加原子深度并按配置启动 host LB timer。
 * 注意事项：先发布 enable_depth 后启动 timer，timer 永不观察“已运行但 dispatch 未启用”。
 */
static void enable_bypass_dsp(struct scx_sched *sch)
{
	struct scx_sched *host = scx_parent(sch) ?: sch;
	u32 intv_us = READ_ONCE(scx_bypass_lb_intv_us);
	s32 ret;

	/*
	 * @sch->bypass_depth transitioning from 0 to 1 triggers enabling.
	 * Shouldn't stagger.
	 */
/* claim 位把每个 scheduler 的 host 深度贡献限制为恰好一次。 */
	if (WARN_ON_ONCE(test_and_set_bit(0, &sch->bypass_dsp_claim)))
		/* claim 位把每个 scheduler 的 host 深度贡献限制为恰好一次。 */
		return;

	/*
	 * When a sub-sched bypasses, its tasks are queued on the bypass DSQs of
	 * the nearest non-bypassing ancestor or root. As enable_bypass_dsp() is
	 * called iff @sch is not already bypassed due to an ancestor bypassing,
	 * we can assume that the parent is not bypassing and thus will be the
	 * host of the bypass DSQs.
	 *
	 * While the situation may change in the future, the following
	 * guarantees that the nearest non-bypassing ancestor or root has bypass
	 * dispatch enabled while a descendant is bypassing, which is all that's
	 * required.
	 *
	 * bypass_dsp_enabled() test is used to determine whether to enter the
	 * bypass dispatch handling path from both bypassing and hosting scheds.
	 * Bump enable depth on both @sch and bypass dispatch host.
	 */
	/* sch 表示 bypass 状态，host 表示实际 DSQ 消费能力，两计数需作为同一贡献成对增减。 */
	ret = atomic_inc_return(&sch->bypass_dsp_enable_depth);
	WARN_ON_ONCE(ret <= 0);

	if (host != sch) {
		ret = atomic_inc_return(&host->bypass_dsp_enable_depth);
		WARN_ON_ONCE(ret <= 0);
	}

	/*
	 * The LB timer will stop running if bypass dispatch is disabled. Start
	 * after enabling bypass dispatch.
	 */
	/* 关闭 bypass dispatch 会让负载均衡 timer 停止；因此启用后再启动 timer，避免刚启动即失去续期条件。 */
	if (intv_us && !timer_pending(&host->bypass_lb_timer))
		mod_timer(&host->bypass_lb_timer,
			  jiffies + usecs_to_jiffies(intv_us));
}

/* may be called without holding scx_bypass_lock */
/* 最后一层退出 bypass 后重新把 task 交给 BPF；嵌套深度未归零时保持接管。 */
/* 清 claim 后对 sch 与 parent host 成对减 enable_depth；原子计数允许在 bypass_lock 外调用。 */
static void disable_bypass_dsp(struct scx_sched *sch)
{
	s32 ret;

	if (!test_and_clear_bit(0, &sch->bypass_dsp_claim))
		return;

	ret = atomic_dec_return(&sch->bypass_dsp_enable_depth);
	WARN_ON_ONCE(ret < 0);

	/* sub 的 task 使用 parent host DSQ，因此必须同步撤销曾增加的 host 深度。 */
	if (scx_parent(sch)) {
		ret = atomic_dec_return(&scx_parent(sch)->bypass_dsp_enable_depth);
		WARN_ON_ONCE(ret < 0);
	}
}

/**
 * scx_bypass - [Un]bypass scx_ops and guarantee forward progress
 * @sch: sched to bypass
 * @bypass: true for bypass, false for unbypass
 *
 * Bypassing guarantees that all runnable tasks make forward progress without
 * trusting the BPF scheduler. We can't grab any mutexes or rwsems as they might
 * be held by tasks that the BPF scheduler is forgetting to run, which
 * unfortunately also excludes toggling the static branches.
 *
 * Let's work around by overriding a couple ops and modifying behaviors based on
 * the DISABLING state and then cycling the queued tasks through dequeue/enqueue
 * to force global FIFO scheduling.
 *
 * - ops.select_cpu() is ignored and the default select_cpu() is used.
 *
 * - ops.enqueue() is ignored and tasks are queued in simple global FIFO order.
 *   %SCX_OPS_ENQ_LAST is also ignored.
 *
 * - ops.dispatch() is ignored.
 *
 * - balance_one() does not set %SCX_RQ_BAL_KEEP on non-zero slice as slice
 *   can't be trusted. Whenever a tick triggers, the running task is rotated to
 *   the tail of the queue with core_sched_at touched.
 *
 * - pick_next_task() suppresses zero slice warning.
 *
 * - scx_kick_cpu() is disabled to avoid irq_work malfunction during PM
 *   operations.
 *
 * - scx_prio_less() reverts to the default core_sched_at order.
 */
/* bypass_lock 串行嵌套深度与全 CPU 重排，保证错误恢复期间始终有可运行路径。 */
/*
 * 上游契约：不依赖任何可能被饿死 task 持有的 mutex/rwsem/static-key 更新；通过改变
 * runtime 分支并让所有 runnable task 走一次 dequeue/enqueue，强制进入内核 FIFO/bypass。
 * 入参：sch 为目标子树根；bypass=true 进入、false 退出。
 * 出参/返回：无；首/末层传播 descendant 深度、更新每 CPU 标志并重排整棵子树 task。
 * 注意事项：遍历 possible CPU 而非 online；退出必须先搬空 bypass DSQ再关闭 host 消费能力。
 */
static void scx_bypass(struct scx_sched *sch, bool bypass)
{
	struct scx_sched *pos;
	unsigned long flags;
	int cpu;

	raw_spin_lock_irqsave(&scx_bypass_lock, flags);

	if (bypass) {
		/* 只有外层 0→1 执行传播/重排；嵌套请求共享已建立的前进保证。 */
		if (!inc_bypass_depth(sch))
			goto unlock;

		enable_bypass_dsp(sch);
	} else {
		if (!dec_bypass_depth(sch))
			goto unlock;
	}

	/*
	 * Bypass state is propagated to all descendants - an scx_sched bypasses
	 * if itself or any of its ancestors are in bypass mode.
	 */
	/* 后代深度累积所有祖先请求，使 scx_bypassing 无需每次沿树查询。 */
	raw_spin_lock(&scx_sched_lock);
	scx_for_each_descendant_pre(pos, sch) {
		if (pos == sch)
			continue;
		if (bypass)
			inc_bypass_depth(pos);
		else
			dec_bypass_depth(pos);
	}
	raw_spin_unlock(&scx_sched_lock);

	/*
	 * No task property is changing. We just need to make sure all currently
	 * queued tasks are re-queued according to the new scx_bypassing()
	 * state. As an optimization, walk each rq's runnable_list instead of
	 * the scx_tasks list.
	 *
	 * This function can't trust the scheduler and thus can't use
	 * cpus_read_lock(). Walk all possible CPUs instead of online.
	 */
	/* 不改变 task 属性，只强制每个 runnable task 依据新 bypass 状态重新走入队决策。 */
	for_each_possible_cpu(cpu) {
		/* 故障恢复不能依赖 cpus_read_lock 持有者被调度，possible 集合为静态安全上界。 */
		struct rq *rq = cpu_rq(cpu);
		struct task_struct *p, *n;

		raw_spin_rq_lock(rq);
		raw_spin_lock(&scx_sched_lock);

		scx_for_each_descendant_pre(pos, sch) {
			/* per-CPU 快速位在 rq+sched lock 下与层级深度快照同步更新。 */
			struct scx_sched_pcpu *pcpu = per_cpu_ptr(pos->pcpu, cpu);

			if (pos->bypass_depth)
				pcpu->flags |= SCX_SCHED_PCPU_BYPASSING;
			else
				pcpu->flags &= ~SCX_SCHED_PCPU_BYPASSING;
		}

		raw_spin_unlock(&scx_sched_lock);

		/*
		 * We need to guarantee that no tasks are on the BPF scheduler
		 * while bypassing. Either we see enabled or the enable path
		 * sees scx_bypassing() before moving tasks to SCX.
		 */
/* 与 enable 竞争时二选一：本路径看到 enabled 并重排，或 enable 看到 bypass 后走保底。 */
		if (!scx_enabled()) {
			/* 与 enable 竞争时二选一：本路径看到 enabled 并重排，或 enable 看到 bypass 后走保底。 */
			raw_spin_rq_unlock(rq);
			continue;
		}

		/*
		 * The use of list_for_each_entry_safe_reverse() is required
		 * because each task is going to be removed from and added back
		 * to the runnable_list during iteration. Because they're added
		 * to the tail of the list, safe reverse iteration can still
		 * visit all nodes.
		 */
		/* 必须安全逆序遍历，因为每个 task 都会从当前链摘除并加入另一链，普通遍历会破坏游标。 */
		list_for_each_entry_safe_reverse(p, n, &rq->scx.runnable_list,
						 scx.runnable_node) {
			if (!scx_is_descendant(scx_task_sched(p), sch))
				continue;

			/* cycling deq/enq is enough, see the function comment */
			/* 反向 safe 遍历配合重新追加队尾，确保每个起始成员恰好访问一次。 */
			scoped_guard (sched_change, p, DEQUEUE_SAVE | DEQUEUE_MOVE) {
				/* nothing */ ;
			}
		}

		/* resched to restore ticks and idle state */
		/* 请求重新调度以恢复 tick 与 idle 状态，使退出 bypass 后 CPU 状态与普通调度重新一致。 */
		if (cpu_online(cpu) || cpu == smp_processor_id())
			resched_curr(rq);

		raw_spin_rq_unlock(rq);
	}

	/* disarming must come after moving all tasks out of the bypass DSQs */
/* 所有 task 已离开 bypass DSQ 后才撤 host 深度，避免队列中 task 无 consumer。 */
	if (!bypass)
		/* 所有 task 已离开 bypass DSQ 后才撤 host 深度，避免队列中 task 无 consumer。 */
		disable_bypass_dsp(sch);
unlock:
	raw_spin_unlock_irqrestore(&scx_bypass_lock, flags);
}

/* 释放 exit_info 聚合拥有的 kv dump、消息、回溯数组和外层对象；仅最终 owner 调用。 */
static void free_exit_info(struct scx_exit_info *ei)
{
	kvfree(ei->dump);
	kfree(ei->msg);
	kfree(ei->bt);
	kfree(ei);
}

/* 一次分配退出元数据和可变 dump 缓冲；ENOMEM 由启用路径回滚。 */
/*
 * 入参 exit_dump_len 为 BPF 诊断区字节数；成功返回拥有全部子缓冲的指针，失败 NULL
 * 且本函数已释放部分分配。exit_cpu 初值 -1 表示尚无具体触发 CPU。
 */
static struct scx_exit_info *alloc_exit_info(size_t exit_dump_len)
{
	struct scx_exit_info *ei;

	ei = kzalloc_obj(*ei);
	if (!ei)
		return NULL;

	ei->exit_cpu = -1;
	ei->bt = kzalloc_objs(ei->bt[0], SCX_EXIT_BT_LEN);
	ei->msg = kzalloc(SCX_EXIT_MSG_LEN, GFP_KERNEL);
	ei->dump = kvzalloc(exit_dump_len, GFP_KERNEL);

	/* 三个子缓冲作为一个整体拥有，任一失败统一释放，调用者不会接收半对象。 */
	if (!ei->bt || !ei->msg || !ei->dump) {
		free_exit_info(ei);
		return NULL;
	}

	return ei;
}

/* 把退出类别映射为静态只读原因字符串；未知枚举返回 <UNKNOWN>，无分配或副作用。 */
static const char *scx_exit_reason(enum scx_exit_kind kind)
{
	switch (kind) {
	case SCX_EXIT_UNREG:
		/* 用户态主动注销 struct_ops link。 */
		return "unregistered from user space";
	case SCX_EXIT_UNREG_BPF:
		/* BPF 程序通过接口主动注销自身。 */
		return "unregistered from BPF";
	case SCX_EXIT_UNREG_KERN:
		/* 内核因 hotplug/接口条件变化要求卸载。 */
		return "unregistered from the main kernel";
	case SCX_EXIT_SYSRQ:
		/* 管理员使用 sysrq-S 强制恢复内建调度。 */
		return "disabled by sysrq-S";
	case SCX_EXIT_PARENT:
		/* sub-scheduler 因父实例退出被递归撤销。 */
		return "parent exiting";
	case SCX_EXIT_ERROR:
		/* 内核侧协议/运行时通用错误。 */
		return "runtime error";
	case SCX_EXIT_ERROR_BPF:
		/* BPF 显式报告的 scx_bpf_error。 */
		return "scx_bpf_error";
	case SCX_EXIT_ERROR_STALL:
		/* runnable task 或 watchdog 超时，前进性已丢失。 */
		return "runnable task stall";
	default:
		return "<UNKNOWN>";
	}
}

/* 以 RCU replace 把所有 per-CPU kick_sync 数组置 NULL，并在宽限期后 kvfree。 */
static void free_kick_syncs(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct scx_kick_syncs __rcu **ksyncs = per_cpu_ptr(&scx_kick_syncs, cpu);
		struct scx_kick_syncs *to_free;

		to_free = rcu_replace_pointer(*ksyncs, NULL, true);
		/* 先让新读者看到 NULL，旧读者越过宽限期后再释放整块代际数组。 */
		if (to_free)
			kvfree_rcu(to_free, rcu);
	}
}

/*
 * 业务背景：scheduler 实例增删后重算共享 watchdog 间隔为最短 timeout 的一半；无实例
 * 用 ULONG_MAX 表示停表。
 * 入参：无；在 RCU 下遍历 scx_sched_all。
 * 出参/返回：无；刷新心跳/interval，并 mod 或同步取消 delayed_work。
 * 注意事项：至少 1 jiffy，防止极小 timeout 计算成零形成紧循环。
 */
static void refresh_watchdog(void)
{
	struct scx_sched *sch;
	unsigned long intv = ULONG_MAX;

	/* take the shortest timeout and use its half for watchdog interval */
/* 先发布新时间戳/间隔，再启动或同步取消 work，回调读取到同一轮配置。 */
	rcu_read_lock();
	list_for_each_entry_rcu(sch, &scx_sched_all, all)
		intv = max(min(intv, sch->watchdog_timeout / 2), 1);
	rcu_read_unlock();

	/* 先发布新时间戳/间隔，再启动或同步取消 work，回调读取到同一轮配置。 */
	WRITE_ONCE(scx_watchdog_timestamp, jiffies);
	WRITE_ONCE(scx_watchdog_interval, intv);

	if (intv < ULONG_MAX)
		mod_delayed_work(system_dfl_wq, &scx_watchdog_work, intv);
	else
		cancel_delayed_work_sync(&scx_watchdog_work);
}

/*
 * 业务背景：把完整 scheduler 发布到父 children/hash 与全局 RCU 链；父已退出时拒绝
 * 新 child，避免链接后立即落入无人管理状态。
 * 入参：sch 为未发布但资源完整的实例。
 * 出参/返回：成功 0；父退出/hash 插入失败返回 errno并登记 scheduler error。
 * 注意事项：scx_error 会重取 sched_lock，必须在 guard 释放后调用；成功后刷新 watchdog。
 */
static s32 scx_link_sched(struct scx_sched *sch)
{
	const char *err_msg = "";
	s32 ret = 0;

	scoped_guard(raw_spinlock_irq, &scx_sched_lock) {
#ifdef CONFIG_EXT_SUB_SCHED
		struct scx_sched *parent = scx_parent(sch);

		if (parent) {
			/*
			 * scx_claim_exit() propagates exit_kind transition to
			 * its sub-scheds while holding scx_sched_lock - either
			 * we can see the parent's non-NONE exit_kind or the
			 * parent can shoot us down.
			 */
/* 与 parent claim_exit 在同一 sched_lock 下串行：要么本方拒绝，要么父方传播退出。 */
			if (atomic_read(&parent->exit_kind) != SCX_EXIT_NONE) {
				/* 与 parent claim_exit 在同一 sched_lock 下串行：要么本方拒绝，要么父方传播退出。 */
				err_msg = "parent disabled";
				ret = -ENOENT;
				break;
			}

			ret = rhashtable_lookup_insert_fast(&scx_sched_hash,
					&sch->hash_node, scx_sched_hash_params);
			if (ret) {
				/* hash 失败时尚未挂 children/all 链，无需拓扑回滚。 */
				err_msg = "failed to insert into scx_sched_hash";
				break;
			}

			list_add_tail(&sch->sibling, &parent->children);
		}
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

		list_add_tail_rcu(&sch->all, &scx_sched_all);
		/* 最后加入全局 RCU 链，watchdog/查询只会看到已完成父拓扑发布的实例。 */
	}

	/*
	 * scx_error() takes scx_sched_lock via scx_claim_exit(), so it must run after
	 * the guard above is released.
	 */
	/* scx_error 经 scx_claim_exit 获取 scx_sched_lock，因此必须在释放上方 guard 后调用以避免自锁。 */
	if (ret) {
		scx_error(sch, "%s (%d)", err_msg, ret);
		return ret;
	}

	refresh_watchdog();
	return 0;
}

/* 从父 hash/children 与全局 RCU 链摘除实例，阻止新查找；对象释放仍等待 kobject/RCU。 */
static void scx_unlink_sched(struct scx_sched *sch)
{
	scoped_guard(raw_spinlock_irq, &scx_sched_lock) {
#ifdef CONFIG_EXT_SUB_SCHED
		if (scx_parent(sch)) {
			rhashtable_remove_fast(&scx_sched_hash, &sch->hash_node,
					       scx_sched_hash_params);
			list_del_init(&sch->sibling);
			/* hash 与 sibling 同锁摘除，后续 parent drain 不再把该 child 视为活动。 */
		}
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */
		list_del_rcu(&sch->all);
	}

	refresh_watchdog();
}

/*
 * Called to disable future dumps and wait for in-progress one while disabling
 * @sch. Once @sch becomes empty during disable, there's no point in dumping it.
 * This prevents calling dump ops on a dead sch.
 */
/* 设置 dump_disabled并借 dump_lock 等待正在进行的 dump 结束，防止 teardown 调 dead ops。 */
static void scx_disable_dump(struct scx_sched *sch)
{
	guard(raw_spinlock_irqsave)(&scx_dump_lock);
	sch->dump_disabled = true;
}

/* 根据 exit kind 选择 error/info 日志；错误额外输出消息和可选 stacktrace，不改变 exit_info。 */
static void scx_log_sched_disable(struct scx_sched *sch)
{
	struct scx_exit_info *ei = sch->exit_info;
	const char *type = scx_parent(sch) ? "sub-scheduler" : "scheduler";

	if (ei->kind >= SCX_EXIT_ERROR) {
		pr_err("sched_ext: BPF %s \"%s\" disabled (%s)\n", type,
		       sch->ops.name, ei->reason);

		if (ei->msg[0] != '\0')
			pr_err("sched_ext: %s: %s\n", sch->ops.name, ei->msg);
#ifdef CONFIG_STACKTRACE
		/* 只有错误类退出保存回溯；正常注销不制造无意义调用栈噪声。 */
		stack_trace_print(ei->bt, ei->bt_len, 2);
#endif
	} else {
		pr_info("sched_ext: BPF %s \"%s\" disabled (%s)\n", type,
			sch->ops.name, ei->reason);
	}
}

#ifdef CONFIG_EXT_SUB_SCHED
static DECLARE_WAIT_QUEUE_HEAD(scx_unlink_waitq);

/*
 * 等待所有 child 从 sch->children 摘除；退出传播递归保证直接 children 为空即所有后代
 * 都越过 unlink 临界阶段。可睡眠，调用者已先建立 bypass 前进保证。
 */
static void drain_descendants(struct scx_sched *sch)
{
	/*
	 * Child scheds that finished the critical part of disabling will take
	 * themselves off @sch->children. Wait for it to drain. As propagation
	 * is recursive, empty @sch->children means that all proper descendant
	 * scheds reached unlinking stage.
	 */
/*
 * 业务背景：sub-disable 把 task 交回 parent 时 parent init_task 失败，已无法让 child 单独
 * 完成无损退出，只能把 parent 也置错误/bypass，并把尚属 child 的 task 强制交 parent。
 * 入参：sch 为失败 child；failed 为触发 task；fail_code 为 parent init errno。
 * 出参/返回：无；parent 登记退出并 bypass，child 子树 task 被 exit child/改归 parent。
 * 注意事项：parent 正在死亡，NONE task 交回可能让其 BPF 后续失败，但内核前进已由 bypass 保证。
 */
	wait_event(scx_unlink_waitq, list_empty(&sch->children));
}

/*
 * 业务背景：sub-disable 把 task 交回 parent 时 parent init_task 失败，已无法让 child 单独
 * 完成无损退出，只能把 parent 也置错误/bypass，并把尚属 child 的 task 强制交 parent。
 * 入参：sch 为失败 child；failed 为触发 task；fail_code 为 parent init errno。
 * 出参/返回：无；parent 登记退出并 bypass，child 子树 task 被 exit child/改归 parent。
 * 注意事项：parent 正在死亡，NONE task 交回可能让其 BPF 后续失败，但内核前进已由 bypass 保证。
 */
static void scx_fail_parent(struct scx_sched *sch,
			    struct task_struct *failed, s32 fail_code)
{
	struct scx_sched *parent = scx_parent(sch);
	struct scx_task_iter sti;
	struct task_struct *p;

	scx_error(parent, "ops.init_task() failed (%d) for %s[%d] while disabling a sub-scheduler",
		  fail_code, failed->comm, failed->pid);

	/*
	 * Once $parent is bypassed, it's safe to put SCX_TASK_NONE tasks into
	 * it. This may cause downstream failures on the BPF side but $parent is
	 * dying anyway.
	 */
/* 必须先让 parent 内核接管，随后放入未完整 init 的 task 也不会依赖其 BPF 正确调度。 */
	scx_bypass(parent, true);
	/* 必须先让 parent 内核接管，随后放入未完整 init 的 task 也不会依赖其 BPF 正确调度。 */

	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		if (scx_task_on_sched(parent, p))
			continue;

		scoped_guard (sched_change, p, DEQUEUE_SAVE | DEQUEUE_MOVE) {
			/* parent 已在 bypass，强制归属切换无需调用可能失败的 parent init。 */
			scx_disable_and_exit_task(sch, p);
			scx_set_task_sched(p, parent);
		}
	}
	scx_task_iter_stop(&sti);
}

/*
 * 业务背景：完整撤销一个 sub-scheduler：bypass/等待后代，冻结 fork+cgroup，逐 task
 * 先为 parent init、再 exit child并切归 parent，RCU 排空后摘拓扑并在无锁处回调 detach/exit。
 * 入参：sch 为已 claim exit 的 sub 实例。
 * 出参/返回：无；所有 task/后代离开 sch，sysfs/kobject 删除，最终释放异步进行。
 * 注意事项：parent init 失败会升级为 parent failure；task 引用跨 iterator 放锁窗口显式持有。
 */
static void scx_sub_disable(struct scx_sched *sch)
{
	struct scx_sched *parent = scx_parent(sch);
	struct scx_task_iter sti;
	struct task_struct *p;
	int ret;

	/*
	 * Guarantee forward progress and wait for descendants to be disabled.
	 * To limit disruptions, $parent is not bypassed. Tasks are fully
	 * prepped and then inserted back into $parent.
	 */
/* 第一阶段先确保 child 子树 task 不再依赖 BPF，才能等待可能由其运行的 teardown 线程。 */
	scx_bypass(sch, true);
	/* 第一阶段先确保 child 子树 task 不再依赖 BPF，才能等待可能由其运行的 teardown 线程。 */
	drain_descendants(sch);

	/*
	 * Here, every runnable task is guaranteed to make forward progress and
	 * we can safely use blocking synchronization constructs. Actually
	 * disable ops.
	 */
/* 进入可阻塞事务后按固定锁序冻结 scheduler 装卸、fork 与 cgroup 迁移。 */
	mutex_lock(&scx_enable_mutex);
	/* 进入可阻塞事务后按固定锁序冻结 scheduler 装卸、fork 与 cgroup 迁移。 */
	percpu_down_write(&scx_fork_rwsem);
	scx_cgroup_lock();

	set_cgroup_sched(sch_cgroup(sch), parent);
	/* 先把新查找/迁移归属指向 parent，现存 task 再逐个完成实例私有状态交接。 */

	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		struct rq *rq;
		struct rq_flags rf;

		/* filter out duplicate visits */
		/* 过滤重复访问，保证同一 task 在本轮父子迁移中只处理一次。 */
		if (scx_task_on_sched(parent, p))
			continue;

		/*
		 * By the time control reaches here, all descendant schedulers
		 * should already have been disabled.
		 */
		/* 到达这里时所有后代 scheduler 都应已禁用；若仍存在说明父子 teardown 顺序失效。 */
		WARN_ON_ONCE(!scx_task_on_sched(sch, p));

		/*
		 * @p is pinned by the iter: css_task_iter_next() takes a
		 * reference and holds it until the next iter_next() call, so
		 * @p->usage is guaranteed > 0.
		 */
/* css iterator 的引用只到下次 next，额外 get 让放 iterator 锁后 p 仍存活。 */
		get_task_struct(p);
		/* css iterator 的引用只到下次 next，额外 get 让放 iterator 锁后 p 仍存活。 */

		scx_task_iter_unlock(&sti);

		/*
		 * $p is READY or ENABLED on @sch. Initialize for $parent,
		 * disable and exit from @sch, and then switch over to $parent.
		 *
		 * If a task fails to initialize for $parent, the only available
		 * action is disabling $parent too. While this allows disabling
		 * of a child sched to cause the parent scheduler to fail, the
		 * failure can only originate from ops.init_task() of the
		 * parent. A child can't directly affect the parent through its
		 * own failures.
		 */
/* 先构造 parent 私有状态，成功后才销毁 child 状态，失败仍可诊断并升级退出。 */
		ret = __scx_init_task(parent, p, false);
		/* 先构造 parent 私有状态，成功后才销毁 child 状态，失败仍可诊断并升级退出。 */
		if (ret) {
			scx_fail_parent(sch, p, ret);
			put_task_struct(p);
			break;
		}

		rq = task_rq_lock(p, &rf);

		if (scx_get_task_state(p) == SCX_TASK_DEAD) {
			/*
			 * sched_ext_dead() raced us between __scx_init_task()
			 * and this rq lock and ran exit_task() on @sch (the
			 * sched @p was on at that point), not on $parent.
			 * $parent's just-completed init is owed an exit_task()
			 * and we issue it here.
			 */
/* death 已替 child 调 exit，但 parent 新 init 尚欠一次 cancelled exit。 */
			scx_sub_init_cancel_task(parent, p);
			/* death 已替 child 调 exit，但 parent 新 init 尚欠一次 cancelled exit。 */
			task_rq_unlock(rq, p, &rf);
			put_task_struct(p);
			continue;
		}

		scoped_guard (sched_change, p, DEQUEUE_SAVE | DEQUEUE_MOVE) {
			/* sched_change 循环 task 出入队，在 rq lock 下把 BPF 生命周期和 RCU 归属原子切换。 */
			/*
			 * $p is initialized for $parent and still attached to
			 * @sch. Disable and exit for @sch, switch over to
			 * $parent, override the state to READY to account for
			 * $p having already been initialized, and then enable.
			 */
			/* p 已为 parent 初始化但仍挂在原 sch；先从原实例 disable/exit，再切归属并按保留状态继续。 */
			scx_disable_and_exit_task(sch, p);
			scx_set_task_state(p, SCX_TASK_INIT_BEGIN);
			scx_set_task_state(p, SCX_TASK_INIT);
			scx_set_task_sched(p, parent);
			scx_set_task_state(p, SCX_TASK_READY);
			scx_enable_task(parent, p);
		}

		task_rq_unlock(rq, p, &rf);
		/* rq ownership 已释放后才放额外 task 引用，避免退出竞态提前 free。 */
		put_task_struct(p);
	}
	scx_task_iter_stop(&sti);

	scx_disable_dump(sch);
	/* task 全部移走后禁止新 dump，并等待正在使用 child ops 的诊断结束。 */

	scx_cgroup_unlock();
	percpu_up_write(&scx_fork_rwsem);

	/*
	 * All tasks are moved off of @sch but there may still be on-going
	 * operations (e.g. ops.select_cpu()). Drain them by flushing RCU. Use
	 * the expedited version as ancestors may be waiting in bypass mode.
	 * Also, tell the parent that there is no need to keep running bypass
	 * DSQs for us.
	 */
/* 排空 select_cpu 等只持 RCU 的在途 ops；祖先仍在 bypass 等待，采用 expedited 缩短停顿。 */
	synchronize_rcu_expedited();
	/* 排空 select_cpu 等只持 RCU 的在途 ops；祖先仍在 bypass 等待，采用 expedited 缩短停顿。 */
	disable_bypass_dsp(sch);

	scx_unlink_sched(sch);

	mutex_unlock(&scx_enable_mutex);

	/*
	 * @sch is now unlinked from the parent's children list. Notify and call
	 * ops.sub_detach/exit(). Note that ops.sub_detach/exit() must be called
	 * after unlinking and releasing all locks. See scx_claim_exit().
	 */
/* children 已摘除，唤醒 parent drain_descendants；后续 BPF 回调必须在所有内核锁外。 */
	wake_up_all(&scx_unlink_waitq);
	/* children 已摘除，唤醒 parent drain_descendants；后续 BPF 回调必须在所有内核锁外。 */

	if (parent->ops.sub_detach && sch->sub_attached) {
		struct scx_sub_detach_args sub_detach_args = {
			.ops = &sch->ops,
			.cgroup_path = sch->cgrp_path,
		};
		SCX_CALL_OP(parent, sub_detach, NULL,
			    &sub_detach_args);
	}

	scx_log_sched_disable(sch);

	if (sch->ops.exit)
		/* 实例已从拓扑摘除且无 task/RCU 调用后，最后通知其自身 exit。 */
		SCX_CALL_OP(sch, exit, NULL, sch->exit_info);
	if (sch->sub_kset)
		kobject_del(&sch->sub_kset->kobj);
	kobject_del(&sch->kobj);
}
#else	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */
static inline void drain_descendants(struct scx_sched *sch) { }
static inline void scx_sub_disable(struct scx_sched *sch) { }
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

/*
 * 业务背景：root scheduler 的全系统 teardown；先 bypass/排空后代，再切 DISABLING，关闭
 * switch_all/cgroup/fork入口，逐 task 退回内建 class，恢复 DL 带宽并撤静态键/资源。
 * 入参：sch 为已 claim exit 的 root 实例。
 * 出参/返回：无；系统不再有 SCX task/root，BPF exit 与 kobject 删除完成。
 * 注意事项：恢复路径不得失败；锁顺序和静态键撤销时点保证新 fork 不落入被 class 链跳过的 fair。
 */
static void scx_root_disable(struct scx_sched *sch)
{
	struct scx_task_iter sti;
	struct task_struct *p;
	bool was_switched_all;
	int cpu;

	/* guarantee forward progress and wait for descendants to be disabled */
/* 不可信 scheduler 下先恢复所有 runnable task 前进，之后才可安全等待 mutex/rwsem。 */
	scx_bypass(sch, true);
	/* 不可信 scheduler 下先恢复所有 runnable task 前进，之后才可安全等待 mutex/rwsem。 */
	drain_descendants(sch);

	switch (scx_set_enable_state(SCX_DISABLING)) {
	case SCX_DISABLING:
		/* 重复 disable 说明退出 claim 协议异常，但现有事务继续完成唯一 teardown。 */
		WARN_ONCE(true, "sched_ext: duplicate disabling instance?");
		break;
	case SCX_DISABLED:
		/* enable 提交前已报错：没有活动 ops，只记录并恢复状态，无需逐 task 完整路径。 */
		pr_warn("sched_ext: ops error detected without ops (%s)\n",
			sch->exit_info->msg);
		WARN_ON_ONCE(scx_set_enable_state(SCX_DISABLED) != SCX_DISABLING);
		goto done;
	default:
		break;
	}

	/*
	 * Here, every runnable task is guaranteed to make forward progress and
	 * we can safely use blocking synchronization constructs. Actually
	 * disable ops.
	 */
/* 先恢复 fair class 在 class 链中的可达性，再清 switching_all 阻止新普通 task 进 EXT。 */
	mutex_lock(&scx_enable_mutex);

	was_switched_all = scx_switched_all();

	static_branch_disable(&__scx_switched_all);
	/* 先恢复 fair class 在 class 链中的可达性，再清 switching_all 阻止新普通 task 进 EXT。 */
	WRITE_ONCE(scx_switching_all, false);

	/*
	 * Shut down cgroup support before tasks so that the cgroup attach path
	 * doesn't race against scx_disable_and_exit_task().
	 */
/* cgroup attach/property 回调先停止，避免逐 task exit 同时 BPF 又收到 group 操作。 */
	scx_cgroup_lock();
	scx_cgroup_exit(sch);
	/* cgroup attach/property 回调先停止，避免逐 task exit 同时 BPF 又收到 group 操作。 */
	scx_cgroup_unlock();

	/*
	 * The BPF scheduler is going away. All tasks including %TASK_DEAD ones
	 * must be switched out and exited synchronously.
	 */
/* 冻结 fork 写侧后关闭 init 门，之后任何新 child 都不会获得待销毁实例私有状态。 */
	percpu_down_write(&scx_fork_rwsem);

	scx_init_task_enabled = false;
	/* 冻结 fork 写侧后关闭 init 门，之后任何新 child 都不会获得待销毁实例私有状态。 */

	scx_task_iter_start(&sti, NULL);
	while ((p = scx_task_iter_next_locked(&sti))) {
		unsigned int queue_flags = DEQUEUE_SAVE | DEQUEUE_MOVE | DEQUEUE_NOCLOCK;
		const struct sched_class *old_class = p->sched_class;
		const struct sched_class *new_class = scx_setscheduler_class(p);

		update_rq_clock(task_rq(p));

		if (old_class != new_class)
			/* class 真的改变才让 sched_change 执行 class-specific dequeue/enqueue 语义。 */
			queue_flags |= DEQUEUE_CLASS;

		scoped_guard (sched_change, p, queue_flags) {
			p->sched_class = new_class;
		}

		scx_disable_and_exit_task(scx_task_sched(p), p);
		/* class 已切回内建实现后撤销 BPF 私有状态与 RCU scheduler 归属。 */
	}
	scx_task_iter_stop(&sti);

	scx_disable_dump(sch);

	scx_cgroup_lock();
	set_cgroup_sched(sch_cgroup(sch), NULL);
	scx_cgroup_unlock();

	percpu_up_write(&scx_fork_rwsem);

	/*
	 * Invalidate all the rq clocks to prevent getting outdated
	 * rq clocks from a previous scx scheduler.
	 *
	 * Also re-balance the dl_server bandwidth reservations: detach
	 * ext_server (no more sched_ext tasks) and reinstate fair_server if it
	 * was previously detached because we were running in full mode.
	 *
	 * Unlike the enable path, this runs on a recovery path that cannot
	 * fail, so we use dl_server_swap_bw() to atomically free ext_server's
	 * bandwidth and reclaim it for fair_server under the same dl_b lock.
	 *
	 * The swap can still fail with -EBUSY if someone bumped ext_server's
	 * runtime via debugfs between enable and disable; in that narrow case
	 * both servers end up detached and we just WARN.
	 */
	/* 每 CPU 在同一 rq/dl_bw 临界区恢复 server 配额，并使上一实例缓存时钟失效。 */
	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		scx_rq_clock_invalidate(rq);

		scoped_guard(rq_lock_irqsave, rq) {
			update_rq_clock(rq);
			/* full 模式原先撤销 fair 配额，退出时用原子 swap 恢复；partial 只撤 ext。 */
			if (was_switched_all) {
				if (WARN_ON_ONCE(dl_server_swap_bw(&rq->ext_server,
								   &rq->fair_server)))
					pr_warn("failed to re-attach fair_server on CPU %d\n", cpu);
			} else {
				dl_server_detach_bw(&rq->ext_server);
			}
		}
	}

	/* no task is on scx, turn off all the switches and flush in-progress calls */
	/* task 全部退回后才撤热路径静态键；先阻止新调用，再 synchronize_rcu 排空旧调用。 */
	static_branch_disable(&__scx_enabled);
	static_branch_disable(&__scx_is_cid_type);
	if (sch->ops.flags & SCX_OPS_TID_TO_TASK)
		static_branch_disable(&__scx_tid_to_task_enabled);
	bitmap_zero(sch->has_op, SCX_OPI_END);
	scx_idle_disable();
	synchronize_rcu();
	/* RCU 后不再有 tid lookup/ops fastpath 使用表或 scheduler，才可销毁 tid 哈希。 */
	if (sch->ops.flags & SCX_OPS_TID_TO_TASK)
		rhashtable_free_and_destroy(&scx_tid_hash, NULL, NULL);

	scx_log_sched_disable(sch);

	if (sch->ops.exit)
		SCX_CALL_OP(sch, exit, NULL, sch->exit_info);

	scx_unlink_sched(sch);
	/* 从全局 RCU 链摘除后 watchdog 不再计入此实例，kobject 仍延迟最终释放。 */

	/*
	 * scx_root clearing must be inside cpus_read_lock(). See
	 * handle_hotplug().
	 */
/* 与 handle_hotplug 相同的 cpus_read_lock 协议提交 root=NULL，热插拔不会见半撤状态。 */
	cpus_read_lock();
	/* 与 handle_hotplug 相同的 cpus_read_lock 协议提交 root=NULL，热插拔不会见半撤状态。 */
	RCU_INIT_POINTER(scx_root, NULL);
	cpus_read_unlock();

	/*
	 * Delete the kobject from the hierarchy synchronously. Otherwise, sysfs
	 * could observe an object of the same name still in the hierarchy when
	 * the next scheduler is loaded.
	 */
/* 同步从 sysfs hierarchy 删除名字，下一实例可立即复用 root 而不碰 EEXIST。 */
#ifdef CONFIG_EXT_SUB_SCHED
	if (sch->sub_kset)
		kobject_del(&sch->sub_kset->kobj);
#endif
	kobject_del(&sch->kobj);
	/* 同步从 sysfs hierarchy 删除名字，下一实例可立即复用 root 而不碰 EEXIST。 */

	free_kick_syncs();

	mutex_unlock(&scx_enable_mutex);

	WARN_ON_ONCE(scx_set_enable_state(SCX_DISABLED) != SCX_DISABLING);
	/* 所有外部可见资源已撤销后才发布 DISABLED；旧值用于断言唯一状态机路径。 */
done:
	scx_bypass(sch, false);
}

/*
 * Claim the exit on @sch. The caller must ensure that the helper kthread work
 * is kicked before the current task can be preempted. Once exit_kind is
 * claimed, scx_error() can no longer trigger, so if the current task gets
 * preempted and the BPF scheduler fails to schedule it back, the helper work
 * will never be kicked and the whole system can wedge.
 */
/* cmpxchg 只允许首个退出原因获胜，后续故障不会覆盖诊断根因。 */
/*
 * 上游契约：调用者禁抢占，并必须在可被抢占前确保 helper work 已 kick；首个 kind 用
 * cmpxchg 从 NONE 领取，随后同步置 aborting 并向所有后代传播 PARENT exit。
 * 入参：sch 为稳定实例；kind 不得为 NONE/DONE。
 * 出参/返回：首次 claim true，已有退出 false；不覆盖首因。
 * 注意事项：传播在 sched_lock 内同步完成，和 child attach 的 scx_link_sched 互锁。
 */
static bool scx_claim_exit(struct scx_sched *sch, enum scx_exit_kind kind)
{
	int none = SCX_EXIT_NONE;

	lockdep_assert_preemption_disabled();

	if (WARN_ON_ONCE(kind == SCX_EXIT_NONE || kind == SCX_EXIT_DONE))
		kind = SCX_EXIT_ERROR;

	if (!atomic_try_cmpxchg(&sch->exit_kind, &none, kind))
		return false;

	/*
	 * Some CPUs may be trapped in the dispatch paths. Set the aborting
	 * flag to break potential live-lock scenarios, ensuring we can
	 * successfully reach scx_bypass().
	 */
/* 先让所有可能无界的 dispatch/DSQ 扫描退出，再等待 disable work 到达 bypass。 */
	WRITE_ONCE(sch->aborting, true);
	/* 先让所有可能无界的 dispatch/DSQ 扫描退出，再等待 disable work 到达 bypass。 */

	/*
	 * Propagate exits to descendants immediately. Each has a dedicated
	 * helper kthread and can run in parallel. While most of disabling is
	 * serialized, running them in separate threads allows parallelizing
	 * ops.exit(), which can take arbitrarily long prolonging bypass mode.
	 *
	 * To guarantee forward progress, this propagation must be in-line so
	 * that ->aborting is synchronously asserted for all sub-scheds. The
	 * propagation is also the interlocking point against sub-sched
	 * attachment. See scx_link_sched().
	 *
	 * This doesn't cause recursions as propagation only takes place for
	 * non-propagation exits.
	 */
/* PARENT 是传播产生的叶/中间事件，禁止再次向下递归重复传播。 */
	if (kind != SCX_EXIT_PARENT) {
		/* PARENT 是传播产生的叶/中间事件，禁止再次向下递归重复传播。 */
		scoped_guard (raw_spinlock_irqsave, &scx_sched_lock) {
			struct scx_sched *pos;
			scx_for_each_descendant_pre(pos, sch)
				scx_disable(pos, SCX_EXIT_PARENT);
		}
	}

	return true;
}

/* 可睡眠控制路径进入 bypass、dump、退出全部 task、撤静态键并释放 scheduler。 */
/*
 * disable kthread work 原子把 claimed kind 改 DONE，保存 reason 后按 root/sub 分派完整
 * teardown；DONE 也阻止重复 work 二次释放。
 */
static void scx_disable_workfn(struct kthread_work *work)
{
	struct scx_sched *sch = container_of(work, struct scx_sched, disable_work);
	struct scx_exit_info *ei = sch->exit_info;
	int kind;

	kind = atomic_read(&sch->exit_kind);
	while (true) {
		/* cmpxchg 循环保留并发更新的实际 kind，只有本 worker 成功提交 DONE 才继续。 */
		if (kind == SCX_EXIT_DONE)	/* already disabled? */
		/* DONE 表示实例已经完成禁用，直接返回以避免重复 teardown。 */
			return;
		WARN_ON_ONCE(kind == SCX_EXIT_NONE);
		if (atomic_try_cmpxchg(&sch->exit_kind, &kind, SCX_EXIT_DONE))
			break;
	}
	ei->kind = kind;
	ei->reason = scx_exit_reason(ei->kind);

	/* disable worker 是最终确认退出类别并进入 root/sub teardown 的可睡眠消费者。 */
	if (scx_parent(sch))
		scx_sub_disable(sch);
	else
		scx_root_disable(sch);
}

/* 首次调用者 claim exit reason 并安排 disable work，重复错误只保留首因。 */
/* 禁抢占 guard 覆盖 claim 到 irq_work_queue，满足“当前 task 被饿死前工作已可运行”约束。 */
static void scx_disable(struct scx_sched *sch, enum scx_exit_kind kind)
{
	guard(preempt)();
	if (scx_claim_exit(sch, kind))
		irq_work_queue(&sch->disable_irq_work);
}

/**
 * scx_flush_disable_work - flush the disable work and wait for it to finish
 * @sch: the scheduler
 *
 * sch->disable_work might still not queued, causing kthread_flush_work()
 * as a noop. Syncing the irq_work first is required to guarantee the
 * kthread work has been queued before waiting for it.
 */
/*
 * 上游契约：先 sync irq_work 确保 disable_work 已入 helper，再 flush kthread；循环直到
 * exit_kind 为 NONE（未退出）或 DONE（teardown worker 已领取/完成），关闭排队竞态窗。
 */
static void scx_flush_disable_work(struct scx_sched *sch)
{
	int kind;

	do {
		irq_work_sync(&sch->disable_irq_work);
		kthread_flush_work(&sch->disable_work);
		kind = atomic_read(&sch->exit_kind);
	} while (kind != SCX_EXIT_NONE && kind != SCX_EXIT_DONE);
}

/* 同时向 trace dump 发空行，并仅在非零容量 seq_buf 写换行，避免零尺寸 WARN。 */
static void dump_newline(struct seq_buf *s)
{
	trace_sched_ext_dump("");

	/* @s may be zero sized and seq_buf triggers WARN if so */
/*
 * 格式化一行到可选 tracepoint 与 seq_buf；静态 trace buffer 受 scx_dump_lock 保护。
 * s 可为零容量，函数仍可输出 trace；返回 void，溢出状态由 seq_buf 自身累计。
 */
	if (s->size)
		seq_buf_putc(s, '\n');
}

/*
 * 格式化一行到可选 tracepoint 与 seq_buf；静态 trace buffer 受 scx_dump_lock 保护。
 * s 可为零容量，函数仍可输出 trace；返回 void，溢出状态由 seq_buf 自身累计。
 */
static __printf(2, 3) void dump_line(struct seq_buf *s, const char *fmt, ...)
{
	va_list args;

#ifdef CONFIG_TRACEPOINTS
	if (trace_sched_ext_dump_enabled()) {
		/* protected by scx_dump_lock */
/* 完整行同时送 tracepoint；seq_buf 溢出由外层统一标记截断。 */
		static char line_buf[SCX_EXIT_MSG_LEN];

		va_start(args, fmt);
		vscnprintf(line_buf, sizeof(line_buf), fmt, args);
		va_end(args);

		trace_call__sched_ext_dump(line_buf);
		/* 完整行同时送 tracepoint；seq_buf 溢出由外层统一标记截断。 */
	}
#endif
	/* @s may be zero sized and seq_buf triggers WARN if so */
	/* s 可能容量为零，而 seq_buf 写零容量会告警，因此必须先门禁可写空间。 */
	if (s->size) {
		va_start(args, fmt);
		seq_buf_vprintf(s, fmt, args);
		va_end(args);

		seq_buf_putc(s, '\n');
	}
}

/* 把 bt 中 len 个地址逐行符号化输出；prefix 为借用字符串，输入数组不被修改。 */
static void dump_stack_trace(struct seq_buf *s, const char *prefix,
			     const unsigned long *bt, unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		dump_line(s, "%s%pS", prefix, (void *)bt[i]);
}

/* 建立一次 BPF dump 回调的全局格式上下文；要求 IRQ disabled 且外层持 dump_lock。 */
static void ops_dump_init(struct seq_buf *s, const char *prefix)
{
	struct scx_dump_data *dd = &scx_dump_data;

	lockdep_assert_irqs_disabled();

	dd->cpu = smp_processor_id();		/* allow scx_bpf_dump() */
	/* 记录当前 CPU，授权本次 dump 回调链调用 scx_bpf_dump。 */
	dd->first = true;
	dd->cursor = 0;
	dd->s = s;
	dd->prefix = prefix;
}

/*
 * 把 BPF bstr 缓冲按内嵌换行拆成独立 dump_line；首个有效输出前插空行，溢出末行也
 * 强制换行。完成后 cursor=0，保留 cpu/prefix 到 ops_dump_exit。
 */
static void ops_dump_flush(void)
{
	struct scx_dump_data *dd = &scx_dump_data;
	char *line = dd->buf.line;

	if (!dd->cursor)
		return;

	/*
	 * There's something to flush and this is the first line. Insert a blank
	 * line to distinguish ops dump.
	 */
/* 首次策略输出先插空行，后续多次调用继续拼接同一诊断段。 */
	if (dd->first) {
		/* 首次策略输出先插空行，后续多次调用继续拼接同一诊断段。 */
		dump_newline(dd->s);
		dd->first = false;
	}

	/*
	 * There may be multiple lines in $line. Scan and emit each line
	 * separately.
	 */
/* 原地逐行临时 NUL 终止并恢复，避免 dump 热故障路径再申请内存。 */
	while (true) {
		char *end = line;
		char c;

		/* 原地逐行临时 NUL 终止并恢复，避免 dump 热故障路径再申请内存。 */
		while (*end != '\n' && *end != '\0')
			end++;

		/*
		 * If $line overflowed, it may not have newline at the end.
		 * Always emit with a newline.
		 */
		/* line 溢出时末尾可能没有换行；输出层始终补换行，保持 dump 的逐行边界。 */
		c = *end;
		*end = '\0';
		dump_line(dd->s, "%s%s", dd->prefix, line);
		if (c == '\0')
			break;

		/* move to the next line */
		/* 跳过已消费换行；若它恰为尾字符则结束，否则继续扫描余下内容。 */
		end++;
		if (*end == '\0')
			break;
		line = end;
	}

	dd->cursor = 0;
}

/* flush 最后一批 BPF 文本并把 cpu 置 -1，撤销 scx_bpf_dump kfunc 的上下文授权。 */
static void ops_dump_exit(void)
{
	ops_dump_flush();
	scx_dump_data.cpu = -1;
}

/*
 * 业务背景：在 rq lock 下输出单 task 的 scheduler 归属、状态/flags、DSQ、slice、affinity，
 * 再调用可选 BPF dump_task 并采集内核栈。
 * 入参：sch 为本次 dump 主体；s/dctx 为输出与快照；rq 已锁；p 为借用 task；marker 标角色。
 * 出参/返回：无；只读 task并追加输出，静态 bt 受全局 dump_lock 串行。
 * 注意事项：own_marker 区分属于 sch 与 dump_all 带入的其他实例 task。
 */
static void scx_dump_task(struct scx_sched *sch, struct seq_buf *s, struct scx_dump_ctx *dctx,
			  struct rq *rq, struct task_struct *p, char marker)
{
	static unsigned long bt[SCX_EXIT_BT_LEN];
	struct scx_sched *task_sch = scx_task_sched(p);
	const char *own_marker;
	char sch_id_buf[32];
	char dsq_id_buf[19] = "(n/a)";
	unsigned long ops_state = atomic_long_read(&p->scx.ops_state);
	unsigned int bt_len = 0;

	/* 星号标识 task 归当前 scheduler；其余字段是允许并发近似的诊断快照。 */
	own_marker = task_sch == sch ? "*" : "";

	if (task_sch->level == 0)
		scnprintf(sch_id_buf, sizeof(sch_id_buf), "root");
	else
		scnprintf(sch_id_buf, sizeof(sch_id_buf), "sub%d-%llu",
			  task_sch->level, task_sch->ops.sub_cgroup_id);

	if (p->scx.dsq)
		/* rq/DSQ 协议让当前指针在 dump 锁住 rq 的窗口内足够稳定用于诊断快照。 */
		scnprintf(dsq_id_buf, sizeof(dsq_id_buf), "0x%llx",
			  (unsigned long long)p->scx.dsq->id);

	dump_newline(s);
	dump_line(s, " %c%c %s[%d] %s%s %+ldms",
		  marker, task_state_to_char(p), p->comm, p->pid,
		  own_marker, sch_id_buf,
		  jiffies_delta_msecs(p->scx.runnable_at, dctx->at_jiffies));
	/* 主行只放身份、归属与 runnable 时长，详细状态拆到下一行。 */
	dump_line(s, "      scx_state/flags=%u/0x%x dsq_flags=0x%x ops_state/qseq=%lu/%lu",
		  scx_get_task_state(p) >> SCX_TASK_STATE_SHIFT,
		  p->scx.flags & ~SCX_TASK_STATE_MASK,
		  p->scx.dsq_flags, ops_state & SCX_OPSS_STATE_MASK,
		  ops_state >> SCX_OPSS_QSEQ_SHIFT);
	/* 第二行展开 DSQ/custody 状态，便于判断卡在 INIT、QUEUEING 还是 DISPATCHING。 */
	dump_line(s, "      sticky/holding_cpu=%d/%d dsq_id=%s",
		  p->scx.sticky_cpu, p->scx.holding_cpu, dsq_id_buf);
	dump_line(s, "      dsq_vtime=%llu slice=%llu weight=%u",
		  p->scx.dsq_vtime, p->scx.slice, p->scx.weight);
	dump_line(s, "      cpus=%*pb no_mig=%u", cpumask_pr_args(p->cpus_ptr),
		  p->migration_disabled);

	if (SCX_HAS_OP(sch, dump_task)) {
		/* BPF 只可在 ops_dump_init/exit 授权窗口向共享 bstr 缓冲追加。 */
		ops_dump_init(s, "    ");
		SCX_CALL_OP(sch, dump_task, rq, dctx, p);
		ops_dump_exit();
	}

#ifdef CONFIG_STACKTRACE
	bt_len = stack_trace_save_tsk(p, bt, SCX_EXIT_BT_LEN, 1);
#endif
	/* 有回溯才逐帧输出；无 STACKTRACE 配置仍保留 task 主体状态。 */
	if (bt_len) {
		dump_newline(s);
		dump_stack_trace(s, "    ", bt, bt_len);
	}
}

/*
 * 输出一个 CPU rq 与其 curr/runnable task；先写入嵌套 seq_buf，idle 且 BPF 无额外输出
 * 时可整体丢弃，避免无信息 CPU 占满有限 dump。
 * 入参：sch/s/dctx、CPU id 和是否包含其他 scheduler task；函数自行 irqsave 锁 rq。
 * 出参/返回：无；必要时 commit 嵌套缓冲并传播 overflow。
 */
static void scx_dump_cpu(struct scx_sched *sch, struct seq_buf *s,
			 struct scx_dump_ctx *dctx, int cpu,
			 bool dump_all_tasks)
{
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;
	struct task_struct *p;
	struct seq_buf ns;
	size_t avail, used;
	/* ns 借父缓冲剩余区，used 用于判断 BPF 是否给 idle CPU 增加有效内容。 */
	char *buf;
	bool idle;

	rq_lock_irqsave(rq, &rf);

	idle = list_empty(&rq->scx.runnable_list) &&
		rq->curr->sched_class == &idle_sched_class;
	/* 两条件同时成立才视为空闲，防止 curr/runnable 单独快照造成误判。 */

	if (idle && !SCX_HAS_OP(sch, dump_cpu))
		goto next;

	/*
	 * We don't yet know whether ops.dump_cpu() will produce output
	 * and we may want to skip the default CPU dump if it doesn't.
	 * Use a nested seq_buf to generate the standard dump so that we
	 * can decide whether to commit later.
	 */
/* nested 直接借父剩余内存，只有确认有价值后 commit 才推进父 cursor。 */
	avail = seq_buf_get_buf(s, &buf);
	/* nested 直接借父剩余内存，只有确认有价值后 commit 才推进父 cursor。 */
	seq_buf_init(&ns, buf, avail);

	dump_newline(&ns);
	dump_line(&ns, "CPU %-4d: nr_run=%u flags=0x%x cpu_rel=%d ops_qseq=%lu ksync=%lu",
		  cpu, rq->scx.nr_running, rq->scx.flags,
		  rq->scx.cpu_released, rq->scx.ops_qseq,
		  rq->scx.kick_sync);
	dump_line(&ns, "          curr=%s[%d] class=%ps",
		  rq->curr->comm, rq->curr->pid,
		  rq->curr->sched_class);
	/* kick 状态仅在非空时输出，突出未完成的跨 CPU 请求。 */
	if (!cpumask_empty(rq->scx.cpus_to_kick))
		dump_line(&ns, "  cpus_to_kick   : %*pb",
			  cpumask_pr_args(rq->scx.cpus_to_kick));
	if (!cpumask_empty(rq->scx.cpus_to_kick_if_idle))
		dump_line(&ns, "  idle_to_kick   : %*pb",
			  cpumask_pr_args(rq->scx.cpus_to_kick_if_idle));
	if (!cpumask_empty(rq->scx.cpus_to_preempt))
		dump_line(&ns, "  cpus_to_preempt: %*pb",
			  cpumask_pr_args(rq->scx.cpus_to_preempt));
	if (!cpumask_empty(rq->scx.cpus_to_wait))
		/* 非空 kick 状态才输出，正常 CPU 不为五张空位图浪费 dump 容量。 */
		dump_line(&ns, "  cpus_to_wait   : %*pb",
			  cpumask_pr_args(rq->scx.cpus_to_wait));
	if (!cpumask_empty(rq->scx.cpus_to_sync))
		dump_line(&ns, "  cpus_to_sync   : %*pb",
			  cpumask_pr_args(rq->scx.cpus_to_sync));

	used = seq_buf_used(&ns);
	if (SCX_HAS_OP(sch, dump_cpu)) {
		/* 策略追加与标准段共用 ns，长度差决定空闲 CPU 段是否值得提交。 */
		ops_dump_init(&ns, "  ");
		SCX_CALL_OP(sch, dump_cpu, rq, dctx, scx_cpu_arg(cpu), idle);
		ops_dump_exit();
	}

	/*
	 * If idle && nothing generated by ops.dump_cpu(), there's
	 * nothing interesting. Skip.
	 */
/* BPF dump_cpu 也没增加内容，丢弃预生成的标准 idle 段。 */
	if (idle && used == seq_buf_used(&ns))
		/* BPF dump_cpu 也没增加内容，丢弃预生成的标准 idle 段。 */
		goto next;

	/*
	 * $s may already have overflowed when $ns was created. If so,
	 * calling commit on it will trigger BUG.
	 */
/* 父已溢出时 avail=0，禁止 commit 触发 seq_buf BUG。 */
	if (avail) {
		/* 父已溢出时 avail=0，禁止 commit 触发 seq_buf BUG。 */
		seq_buf_commit(s, seq_buf_used(&ns));
		if (seq_buf_has_overflowed(&ns))
			seq_buf_set_overflow(s);
	}

	if (rq->curr->sched_class == &ext_sched_class &&
	    (dump_all_tasks || scx_task_on_sched(sch, rq->curr)))
		scx_dump_task(sch, s, dctx, rq, rq->curr, '*');

	/* curr 单独标星后遍历 runnable 链，按 dump_all_tasks 决定是否跨 scheduler。 */
	list_for_each_entry(p, &rq->scx.runnable_list, scx.runnable_node)
		if (dump_all_tasks || scx_task_on_sched(sch, p))
			scx_dump_task(sch, s, dctx, rq, p, ' ');
next:
	rq_unlock_irqrestore(rq, &rf);
}

/*
 * Dump scheduler state. If @dump_all_tasks is true, dump all tasks regardless
 * of which scheduler they belong to. If false, only dump tasks owned by @sch.
 * For SysRq-D dumps, @dump_all_tasks=false since all schedulers are dumped
 * separately. For error dumps, @dump_all_tasks=true since only the failing
 * scheduler is dumped.
 */
/* dump_lock 串行全局格式缓冲，依次输出 ops、CPU 和 task 快照后截断到容量。 */
/*
 * 上游契约：dump_all_tasks 决定只看 sch 归属还是错误现场全部 task；exit CPU 优先
 * 输出以免尾部截断，并在末尾汇总事件计数。
 * 入参：sch/ei 为稳定实例与退出元数据；dump_len 为目标容量；dump_all_tasks 为范围。
 * 出参/返回：无；ei->dump 填充，溢出时尾部覆盖显式 TRUNCATED 标记。
 * 注意事项：dump_lock 同时保护静态 bt/bstr；dump_disabled 后直接返回避免调用 dead ops。
 */
static void scx_dump_state(struct scx_sched *sch, struct scx_exit_info *ei,
			   size_t dump_len, bool dump_all_tasks)
{
	static const char trunc_marker[] = "\n\n~~~~ TRUNCATED ~~~~\n";
	struct scx_dump_ctx dctx = {
		.kind = ei->kind,
		.exit_code = ei->exit_code,
		.reason = ei->reason,
		/* 两种时间基准固定在同一初始化器中，整份 dump 共用近似触发点。 */
		.at_ns = ktime_get_ns(),
		.at_jiffies = jiffies,
	};
	/* ns/jiffies 同时冻结，使内核和 BPF 输出用同一触发时刻计算相对时间。 */
	struct seq_buf s;
	struct scx_event_stats events;
	int cpu;

	guard(raw_spinlock_irqsave)(&scx_dump_lock);

	if (sch->dump_disabled)
		/* teardown 已越过可安全调用 BPF dump 的边界，宁可缺诊断也不能 UAF。 */
		return;

	seq_buf_init(&s, ei->dump, dump_len);

#ifdef CONFIG_EXT_SUB_SCHED
	if (sch->level == 0)
		dump_line(&s, "%s: root", sch->ops.name);
	else
		/* sub 标题同时含层级、cgroup id 和路径，便于还原树中位置。 */
		dump_line(&s, "%s: sub%d-%llu %s",
			  sch->ops.name, sch->level, sch->ops.sub_cgroup_id,
			  sch->cgrp_path);
#endif
	if (ei->kind == SCX_EXIT_NONE) {
		/* SysRq 仅标调试原因；真实退出才附触发 task/CPU、消息和回溯。 */
		dump_line(&s, "Debug dump triggered by %s", ei->reason);
	} else {
		if (ei->exit_cpu >= 0)
			dump_line(&s, "%s[%d] triggered exit kind %d on CPU %d:",
				  current->comm, current->pid, ei->kind,
				  ei->exit_cpu);
		else
			/* 无确定 CPU 的控制路径退出仍保留触发 current 身份。 */
			dump_line(&s, "%s[%d] triggered exit kind %d:",
				  current->comm, current->pid, ei->kind);
		dump_line(&s, "  %s (%s)", ei->reason, ei->msg);
		dump_newline(&s);
		/* 退出主体和原因在 CPU/task 大段之前输出，截断时仍保留根因摘要。 */
		dump_line(&s, "Backtrace:");
		dump_stack_trace(&s, "  ", ei->bt, ei->bt_len);
	}

	if (SCX_HAS_OP(sch, dump)) {
		ops_dump_init(&s, "");
		SCX_CALL_OP(sch, dump, NULL, &dctx);
		ops_dump_exit();
	}

	dump_newline(&s);
	/* 策略总览之后进入 CPU 分节，退出 CPU 会优先于普通遍历打印。 */
	dump_line(&s, "CPU states");
	dump_line(&s, "----------");

	/*
	 * Dump the exit CPU first so it isn't lost to dump truncation, then
	 * walk the rest in order, skipping the one already dumped.
	 */
/* 根因 CPU 最先输出，有限缓冲优先保留最相关 rq/task。 */
	if (ei->exit_cpu >= 0)
		/* 根因 CPU 最先输出，有限缓冲优先保留最相关 rq/task。 */
		scx_dump_cpu(sch, &s, &dctx, ei->exit_cpu, dump_all_tasks);
	for_each_possible_cpu(cpu) {
		if (cpu != ei->exit_cpu)
			scx_dump_cpu(sch, &s, &dctx, cpu, dump_all_tasks);
	}

	dump_newline(&s);
	/* CPU/task 现场后追加事件总计，解释异常分支是否曾频繁发生。 */
	dump_line(&s, "Event counters");
	dump_line(&s, "--------------");

	scx_read_events(sch, &events);
	scx_dump_event(s, &events, SCX_EV_SELECT_CPU_FALLBACK);
	scx_dump_event(s, &events, SCX_EV_DISPATCH_LOCAL_DSQ_OFFLINE);
	scx_dump_event(s, &events, SCX_EV_DISPATCH_KEEP_LAST);
	/* 事件保持与 sysfs 相同顺序，便于离线脚本合并两种诊断来源。 */
	scx_dump_event(s, &events, SCX_EV_ENQ_SKIP_EXITING);
	scx_dump_event(s, &events, SCX_EV_ENQ_SKIP_MIGRATION_DISABLED);
	scx_dump_event(s, &events, SCX_EV_REENQ_IMMED);
	scx_dump_event(s, &events, SCX_EV_REENQ_LOCAL_REPEAT);
	scx_dump_event(s, &events, SCX_EV_REFILL_SLICE_DFL);
	scx_dump_event(s, &events, SCX_EV_BYPASS_DURATION);
	scx_dump_event(s, &events, SCX_EV_BYPASS_DISPATCH);
	/* 尾部区分 bypass 激活、越权 insert 与 sub 层级兜底。 */
	scx_dump_event(s, &events, SCX_EV_BYPASS_ACTIVATE);
	scx_dump_event(s, &events, SCX_EV_INSERT_NOT_OWNED);
	scx_dump_event(s, &events, SCX_EV_SUB_BYPASS_DISPATCH);

	if (seq_buf_has_overflowed(&s) && dump_len >= sizeof(trunc_marker))
		/* 覆盖最后字节而非继续 append，确保即使 cursor 已满也能看见截断事实。 */
		memcpy(ei->dump + dump_len - sizeof(trunc_marker),
		       trunc_marker, sizeof(trunc_marker));
}

/*
 * claim 后的 IRQ 跳板：错误退出先在仍安全的 runtime 状态抓全量 dump，再把可睡眠 teardown
 * 排到 scheduler 专用 RT helper。入参 irq_work 嵌入 sch；无返回。
 */
static void scx_disable_irq_workfn(struct irq_work *irq_work)
{
	struct scx_sched *sch = container_of(irq_work, struct scx_sched, disable_irq_work);
	struct scx_exit_info *ei = sch->exit_info;

	if (ei->kind >= SCX_EXIT_ERROR)
		scx_dump_state(sch, ei, sch->ops.exit_dump_len, true);

	kthread_queue_work(sch->helper, &sch->disable_work);
}

/*
 * 业务背景：可变参数退出核心；禁抢占 claim 首因，保存 exit_code/stack/message/reason/CPU，
 * 然后投 irq_work，保证真正禁用在安全上下文执行。
 * 入参：sch、kind、业务 exit_code、触发 CPU、fmt 与已初始化 va_list。
 * 出参/返回：首次 claim true；已有退出 false且不覆盖原 exit_info。
 * 注意事项：字段写发生在 irq_work queue 前，work 读取同一实例；错误才采集 stack。
 */
bool scx_vexit(struct scx_sched *sch,
	       enum scx_exit_kind kind, s64 exit_code, s32 exit_cpu,
	       const char *fmt, va_list args)
{
	struct scx_exit_info *ei = sch->exit_info;

	guard(preempt)();

	if (!scx_claim_exit(sch, kind))
		/* 失败说明其他 CPU 已保存更早根因，本次不得改 ei 的任何诊断字段。 */
		return false;

	ei->exit_code = exit_code;
#ifdef CONFIG_STACKTRACE
	if (kind >= SCX_EXIT_ERROR)
		ei->bt_len = stack_trace_save(ei->bt, SCX_EXIT_BT_LEN, 1);
#endif
	vscnprintf(ei->msg, SCX_EXIT_MSG_LEN, fmt, args);

	/*
	 * Set ei->kind and ->reason for scx_dump_state(). They'll be set again
	 * in scx_disable_workfn().
	 */
	/* irq_work 可能立即 dump，排队前先临时填齐其依赖字段；worker 会最终确认同值。 */
	ei->kind = kind;
	ei->reason = scx_exit_reason(ei->kind);
	ei->exit_cpu = exit_cpu;

	irq_work_queue(&sch->disable_irq_work);
	/* work 已可运行后 guard 才允许恢复抢占，避免当前 task 被坏 scheduler 永久饿死。 */
	return true;
}

/*
 * 为每个 possible CPU 分配 nr_cpu_ids 长度的 kick 代际数组并 RCU 发布；成功 0，任一
 * NUMA 本地 kvzalloc 失败则撤销此前全部数组并返回 -ENOMEM。
 */
static int alloc_kick_syncs(void)
{
	int cpu;

	/*
	 * Allocate per-CPU arrays sized by nr_cpu_ids. Use kvzalloc as size
	 * can exceed percpu allocator limits on large machines.
	 */
/* 每个发起 CPU 需要观察所有目标 CPU 的代际，故数组长度为 nr_cpu_ids。 */
	for_each_possible_cpu(cpu) {
		struct scx_kick_syncs __rcu **ksyncs = per_cpu_ptr(&scx_kick_syncs, cpu);
		struct scx_kick_syncs *new_ksyncs;

		WARN_ON_ONCE(rcu_access_pointer(*ksyncs));

		/* 每个发起 CPU 需要观察所有目标 CPU 的代际，故数组长度为 nr_cpu_ids。 */
		new_ksyncs = kvzalloc_node(struct_size(new_ksyncs, syncs, nr_cpu_ids),
					   GFP_KERNEL, cpu_to_node(cpu));
		if (!new_ksyncs) {
			free_kick_syncs();
			return -ENOMEM;
		}

		rcu_assign_pointer(*ksyncs, new_ksyncs);
	}

	return 0;
}

/* 释放可空 NUMA pnode：先析构嵌入 global DSQ 的 per-CPU 资源，再释放外层。 */
static void free_pnode(struct scx_sched_pnode *pnode)
{
	if (!pnode)
		return;
	exit_dsq(&pnode->global_dsq);
	kfree(pnode);
}

/* 在指定 NUMA node 分配 pnode 并初始化其 builtin global DSQ；失败返回 NULL且无泄漏。 */
static struct scx_sched_pnode *alloc_pnode(struct scx_sched *sch, int node)
{
	struct scx_sched_pnode *pnode;

	pnode = kzalloc_node(sizeof(*pnode), GFP_KERNEL, node);
	if (!pnode)
		return NULL;

	if (init_dsq(&pnode->global_dsq, SCX_DSQ_GLOBAL, sch)) {
		kfree(pnode);
		return NULL;
	}

	/* global DSQ 初始化成功后 pnode 才作为完整对象返回，调用者取得唯一所有权。 */
	return pnode;
}

/*
 * scx_enable() is offloaded to a dedicated system-wide RT kthread to avoid
 * starvation. During the READY -> ENABLED task switching loop, the calling
 * thread's sched_class gets switched from fair to ext. As fair has higher
 * priority than ext, the calling thread can be indefinitely starved under
 * fair-class saturation, leading to a system hang.
 */
/* work 在全局 RT helper 执行；ops/ops_cid 二选一借用，arena_map 引用成功时转给 sch。 */
struct scx_enable_cmd {
	/* work 在全局 RT helper 执行；ops/ops_cid 二选一借用，arena_map 引用成功时转给 sch。 */
	struct kthread_work	work;
	union {
		struct sched_ext_ops		*ops;
		struct sched_ext_ops_cid	*ops_cid;
	};
	bool			is_cid_type;
	struct bpf_map		*arena_map;	/* arena ref to transfer to sch */
	/* 该 arena 引用将在启用成功时转移给 scheduler，失败时仍由命令回滚。 */
	int			ret;
};

/*
 * Allocate and initialize a new scx_sched. @cgrp's reference is always
 * consumed whether the function succeeds or fails.
 */
/* 校验 ops 后分配 scheduler 及 per-CPU/DSQ 资源，成功才加入全局索引。 */
/*
 * 业务背景：root/sub 启用流程需要一个尚未接管任务、但内部资源已经完整的 scheduler
 * 实例；本函数负责构造该实例，并在末尾通过 ops->priv 与 kobject 把它发布给读者。
 * 入参：cmd 携带 cpu-form/cid-form ops 和待转移的 arena 引用；cgrp 引用无论成功失败
 * 都由本函数消费；parent 为空表示 root，否则决定层级、祖先链和 sysfs 父节点。
 * 出参/返回：成功返回已发布的 sch；失败返回 ERR_PTR(errno)，并逆序释放已取得资源。
 * 约束：发布 ops->priv 后不能再直接 kfree，必须走 kobject/RCU 回收；arena_map 只有在
 * 所有早期失败点之后才从 cmd 转移，因此调用者始终能准确判断引用所有者。
 */
static struct scx_sched *scx_alloc_and_add_sched(struct scx_enable_cmd *cmd,
						 struct cgroup *cgrp,
						 struct scx_sched *parent)
{
	struct sched_ext_ops *ops = cmd->ops;
	struct scx_sched *sch;
	s32 level = parent ? parent->level + 1 : 0;
	s32 node, cpu, ret, bypass_fail_cpu = nr_cpu_ids;

	/* 第一阶段只建立未发布对象；任一步失败都由下方分层标签精确回滚。 */
	sch = kzalloc_flex(*sch, ancestors, level + 1);
	if (!sch) {
		ret = -ENOMEM;
		goto err_put_cgrp;
	}

	sch->exit_info = alloc_exit_info(ops->exit_dump_len);
	if (!sch->exit_info) {
		/* exit_info 是后续所有错误路径必需诊断载体，不能降级为空继续。 */
		ret = -ENOMEM;
		goto err_free_sch;
	}

	ret = rhashtable_init(&sch->dsq_hash, &dsq_hash_params);
	if (ret < 0)
		goto err_free_ei;

	/* pnode 指针数组按完整 node ID 空间分配，元素稍后逐个 NUMA 本地构造。 */
	sch->pnode = kzalloc_objs(sch->pnode[0], nr_node_ids);
	if (!sch->pnode) {
		ret = -ENOMEM;
		goto err_free_hash;
	}

	for_each_node_state(node, N_POSSIBLE) {
		/* 为每个 possible node 建 builtin global DSQ，hotplug 后无需临时分配。 */
		sch->pnode[node] = alloc_pnode(sch, node);
		if (!sch->pnode[node]) {
			ret = -ENOMEM;
			goto err_free_pnode;
		}
	}

	/* node 全部完成后再建立变长 per-CPU dispatch buffer，batch 大小来自已验证 ops。 */
	sch->dsp_max_batch = ops->dispatch_max_batch ?: SCX_DSP_DFL_MAX_BATCH;
	sch->pcpu = __alloc_percpu(struct_size_t(struct scx_sched_pcpu,
						 dsp_ctx.buf, sch->dsp_max_batch),
				   __alignof__(struct scx_sched_pcpu));
	if (!sch->pcpu) {
		/* dispatch buffer 与 pcpu 同块分配，失败时 node 资源仍由上一标签回滚。 */
		ret = -ENOMEM;
		goto err_free_pnode;
	}

	for_each_possible_cpu(cpu) {
		ret = init_dsq(bypass_dsq(sch, cpu), SCX_DSQ_BYPASS, sch);
		if (ret) {
			/* 记录首个未初始化 CPU，回滚循环只析构它之前的完整 DSQ。 */
			bypass_fail_cpu = cpu;
			goto err_free_pcpu;
		}
	}

	for_each_possible_cpu(cpu) {
		struct scx_sched_pcpu *pcpu = per_cpu_ptr(sch->pcpu, cpu);

		pcpu->sch = sch;
		INIT_LIST_HEAD(&pcpu->deferred_reenq_local.node);
	}

	/* helper 承担此 scheduler 的可睡眠 disable 工作，FIFO 防止系统繁忙时退出饥饿。 */
	sch->helper = kthread_run_worker(0, "sched_ext_helper");
	if (IS_ERR(sch->helper)) {
		ret = PTR_ERR(sch->helper);
		goto err_free_pcpu;
	}

	sched_set_fifo(sch->helper->task);

	/* ancestors 复制父链并在末槽追加自身，后续祖先判断可 O(depth) 无锁读取。 */
	if (parent)
		memcpy(sch->ancestors, parent->ancestors,
		       level * sizeof(parent->ancestors[0]));
	sch->ancestors[level] = sch;
	sch->level = level;

	if (ops->timeout_ms)
		sch->watchdog_timeout = msecs_to_jiffies(ops->timeout_ms);
	else
		sch->watchdog_timeout = SCX_WATCHDOG_MAX_TIMEOUT;

	/* 生命周期原语在任何发布前就绪，退出可从后续任一失败点安全触发。 */
	sch->slice_dfl = SCX_SLICE_DFL;
	atomic_set(&sch->exit_kind, SCX_EXIT_NONE);
	sch->disable_irq_work = IRQ_WORK_INIT_HARD(scx_disable_irq_workfn);
	kthread_init_work(&sch->disable_work, scx_disable_workfn);
	timer_setup(&sch->bypass_lb_timer, scx_bypass_lb_timerfn, 0);

	if (!alloc_cpumask_var(&sch->bypass_lb_donee_cpumask, GFP_KERNEL)) {
		/* 两张 LB mask 属于 timer scratch，必须在 timer 初始化后、发布前成对取得。 */
		ret = -ENOMEM;
		goto err_stop_helper;
	}
	if (!alloc_cpumask_var(&sch->bypass_lb_resched_cpumask, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto err_free_lb_cpumask;
	}
	/* 资源骨架完成后复制 ops；此前失败不必处理 BPF 回调表或层级发布。 */
	/*
	 * Copy ops through the right union view. For cid-form the source is
	 * struct sched_ext_ops_cid which lacks the trailing cpu_acquire/
	 * cpu_release; those stay zero from kzalloc.
	 */
	/* 必须通过正确 union 视图复制 ops；cid-form 源结构较短，不能按 cpu-form 尾部字段越界读取。 */
	if (cmd->is_cid_type) {
		sch->ops_cid = *cmd->ops_cid;
		sch->is_cid_type = true;
	} else {
		sch->ops = *cmd->ops;
	}

#ifdef CONFIG_EXT_SUB_SCHED
	/* sub 配置下保存 cgroup 身份与父子链节点；root 同样持 root cgroup 引用。 */
	char *buf = kzalloc(PATH_MAX, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto err_free_lb_resched;
	}
	/* cgroup path 复制为 scheduler 自有字符串，detach/uevent 不依赖临时 buf。 */
	cgroup_path(cgrp, buf, PATH_MAX);
	sch->cgrp_path = kstrdup(buf, GFP_KERNEL);
	kfree(buf);
	if (!sch->cgrp_path) {
		ret = -ENOMEM;
		/* path 复制失败时 cgrp 引用仍由最终错误标签消费。 */
		goto err_free_lb_resched;
	}

	sch->cgrp = cgrp;
	INIT_LIST_HEAD(&sch->children);
	INIT_LIST_HEAD(&sch->sibling);
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

	/* 第二阶段开始发布：先让 BPF ops 能反查 sch，再建立带 RCU 回收语义的 kobject。 */
	/*
	 * Publishing makes @sch visible to scx_prog_sched() readers. Failure
	 * paths after this point must free @sch through kobject_put() whose
	 * release path defers the actual freeing by an RCU grace period.
	 */
	/* 发布后 scx_prog_sched 读者可见 sch；此后的失败必须走 kobject/RCU 生命周期回收，不能直接释放。 */
	rcu_assign_pointer(ops->priv, sch);

	sch->kobj.kset = scx_kset;
	INIT_LIST_HEAD(&sch->all);

#ifdef CONFIG_EXT_SUB_SCHED
	if (parent) {
		/*
		 * Pin @parent for @sch's lifetime. The kobject hierarchy pins
		 * it only via @parent->sub_kset, which is dropped during
		 * disable. Released in scx_sched_free_rcu_work().
		 */
/* root kobject 直接位于 sched_ext kset；child 位于 parent/sub 层级。 */
		kobject_get(&parent->kobj);
		ret = kobject_init_and_add(&sch->kobj, &scx_ktype,
					   &parent->sub_kset->kobj,
					   "sub-%llu", cgroup_id(cgrp));
	} else {
		/* root kobject 直接位于 sched_ext kset；child 位于 parent/sub 层级。 */
		ret = kobject_init_and_add(&sch->kobj, &scx_ktype, NULL, "root");
	}

	if (ret < 0) {
		RCU_INIT_POINTER(ops->priv, NULL);
		kobject_put(&sch->kobj);
		return ERR_PTR(ret);
	}

	if (ops->sub_attach) {
		sch->sub_kset = kset_create_and_add("sub", NULL, &sch->kobj);
		if (!sch->sub_kset) {
			/* ops->priv 已公开，失败必须先撤指针再 put kobject 走 RCU 回收。 */
			RCU_INIT_POINTER(ops->priv, NULL);
			kobject_put(&sch->kobj);
			return ERR_PTR(-ENOMEM);
		}
	}
#else	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */
	ret = kobject_init_and_add(&sch->kobj, &scx_ktype, NULL, "root");
	if (ret < 0) {
		/* kobject init 失败仍要求 kobject_put 执行 release，而非直接释放已发布对象。 */
		RCU_INIT_POINTER(ops->priv, NULL);
		kobject_put(&sch->kobj);
		return ERR_PTR(ret);
	}
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

	/* 最后才转 arena 引用，确保此前所有同步失败仍由注册入口负责 put。 */
	/*
	 * Consume the arena_map ref bpf_scx_reg_cid() took. Defer to here so
	 * earlier failure paths leave cmd->arena_map set and bpf_scx_reg_cid
	 * drops the ref. After this point, sch owns the ref and any cleanup
	 * runs through scx_sched_free_rcu_work() which puts it.
	 */
	/* 消费 bpf_scx_reg_cid 取得的 arena_map 引用；统一延后到此处使更早失败路径仍能按 cmd 所有权回滚。 */
	sch->arena_map = cmd->arena_map;
	/* BPF arena is only available on MMU && 64BIT */
/* 失败标签严格按取得顺序逆序展开；bypass_fail_cpu 防止析构尚未初始化的 DSQ。 */
#if defined(CONFIG_MMU) && defined(CONFIG_64BIT)
	if (sch->arena_map)
		sch->arena_kern_base = bpf_arena_map_kern_vm_start(sch->arena_map);
#endif
	cmd->arena_map = NULL;
	return sch;

	/* 失败标签严格按取得顺序逆序展开；bypass_fail_cpu 防止析构尚未初始化的 DSQ。 */
#ifdef CONFIG_EXT_SUB_SCHED
err_free_lb_resched:
	free_cpumask_var(sch->bypass_lb_resched_cpumask);
#endif
err_free_lb_cpumask:
	free_cpumask_var(sch->bypass_lb_donee_cpumask);
err_stop_helper:
	kthread_destroy_worker(sch->helper);
err_free_pcpu:
	/* helper 停止后不再有异步访问，按初始化前缀销毁 bypass DSQ 与 per-CPU 内存。 */
	for_each_possible_cpu(cpu) {
		if (cpu == bypass_fail_cpu)
			break;
		exit_dsq(bypass_dsq(sch, cpu));
	}
	free_percpu(sch->pcpu);
err_free_pnode:
	/* 逐 NUMA 节点 free_pnode 接受 NULL，覆盖部分初始化和完整初始化两种路径。 */
	for_each_node_state(node, N_POSSIBLE)
		free_pnode(sch->pnode[node]);
	kfree(sch->pnode);
err_free_hash:
	rhashtable_free_and_destroy(&sch->dsq_hash, NULL, NULL);
err_free_ei:
	free_exit_info(sch->exit_info);
err_free_sch:
	kfree(sch);
err_put_cgrp:
	/* allocator 的 API 明确消费 cgrp，所有错误最终都必须归还该引用。 */
#ifdef CONFIG_EXT_SUB_SCHED
	cgroup_put(cgrp);
#endif
	return ERR_PTR(ret);
}

/*
 * 业务背景：BPF scheduler 装载与真正接管 CPU 之间可能发生 hotplug；若用户态基于旧
 * 拓扑初始化，继续启用会让其 CPU 状态机从第一刻就失真。
 * 入参 sch 用于记录完整退出原因，ops->hotplug_seq 为用户态初始化时观察到的代际。
 * 返回：未要求校验或代际相等返回 0；不相等触发可重启退出并返回 -EBUSY。
 * 并发：全局代际用 atomic_long 读取；调用者持 cpus_read_lock 阻止校验后立即变更。
 */
static int check_hotplug_seq(struct scx_sched *sch,
			      const struct sched_ext_ops *ops)
{
	unsigned long long global_hotplug_seq;

	/*
	 * If a hotplug event has occurred between when a scheduler was
	 * initialized, and when we were able to attach, exit and notify user
	 * space about it.
	 */
/* 只要初始化观察值与当前不同，就要求用户态基于新拓扑完整重建。 */
	if (ops->hotplug_seq) {
		global_hotplug_seq = atomic_long_read(&scx_hotplug_seq);
		/* 只要初始化观察值与当前不同，就要求用户态基于新拓扑完整重建。 */
		if (ops->hotplug_seq != global_hotplug_seq) {
			scx_exit(sch, SCX_EXIT_UNREG_KERN,
				 SCX_ECODE_ACT_RESTART | SCX_ECODE_RSN_HOTPLUG,
				 "expected hotplug seq %llu did not match actual %llu",
				 ops->hotplug_seq, global_hotplug_seq);
			return -EBUSY;
		}
	}

	return 0;
}

/*
 * 业务背景：struct_ops 的字段组合包含跨回调依赖，验证器的类型检查无法表达这些运行
 * 时协议；scheduler 发布前必须集中拒绝不自洽配置。
 * 入参 sch 提供 root/sub、cpu/cid 形态及错误通道，ops 是待启用回调表。
 * 返回：协议合法为 0；发现首个矛盾时记录 scx_error 并返回 -EINVAL。
 * 约束：cid-form 对象没有 cpu_acquire/release 尾字段，必须先按形态分支，禁止越界读。
 */
static int validate_ops(struct scx_sched *sch, const struct sched_ext_ops *ops)
{
	/*
	 * It doesn't make sense to specify the SCX_OPS_ENQ_LAST flag if the
	 * ops.enqueue() callback isn't implemented.
	 */
	/* 未实现 ops.enqueue 时声明 ENQ_LAST 没有语义，校验阶段应拒绝这一矛盾组合。 */
	if ((ops->flags & SCX_OPS_ENQ_LAST) && !ops->enqueue) {
		scx_error(sch, "SCX_OPS_ENQ_LAST requires ops.enqueue() to be implemented");
		return -EINVAL;
	}

	/*
	 * SCX_OPS_TID_TO_TASK is enabled by the root scheduler. A sub-sched
	 * may set it to declare a dependency; reject if the root hasn't
	 * enabled it.
	 */
	/* sub 只能声明依赖，不能独立创建全局 tid 哈希；root 必须先提供该能力。 */
	if ((ops->flags & SCX_OPS_TID_TO_TASK) && scx_parent(sch) &&
	    !(scx_root->ops.flags & SCX_OPS_TID_TO_TASK)) {
		scx_error(sch, "SCX_OPS_TID_TO_TASK requires root scheduler to enable it");
		return -EINVAL;
	}

	/* per-node idle 必须建立在 builtin idle 选择仍有效的前提上。 */
	/*
	 * SCX_OPS_BUILTIN_IDLE_PER_NODE requires built-in CPU idle
	 * selection policy to be enabled.
	 */
	/* BUILTIN_IDLE_PER_NODE 依赖内建 idle 选择；未启用基础策略时该标志无法兑现。 */
	if ((ops->flags & SCX_OPS_BUILTIN_IDLE_PER_NODE) &&
	    (ops->update_idle && !(ops->flags & SCX_OPS_KEEP_BUILTIN_IDLE))) {
		scx_error(sch, "SCX_OPS_BUILTIN_IDLE_PER_NODE requires CPU idle selection enabled");
		return -EINVAL;
	}

	/*
	 * cid-form's struct is shorter and doesn't include the cpu_acquire /
	 * cpu_release tail; reading those fields off a cid-form @ops would
	 * run past the BPF allocation. Skip for cid-form.
	 */
	/* cid-form 结构更短且没有 cpu_acquire/release 尾部，读取这些字段会越过 BPF 分配边界。 */
	if (!sch->is_cid_type && (ops->cpu_acquire || ops->cpu_release))
		pr_warn_ratelimited("ops->cpu_acquire/release() are deprecated, use sched_switch TP instead\n");

	/*
	 * Sub-scheduler support is tied to the cid-form struct_ops. A sub-sched
	 * attaches through a cid-form-only interface (sub_attach/sub_detach),
	 * and a root that accepts sub-scheds must expose cid-form state to
	 * them. Reject cpu-form schedulers on either side.
	 */
	/* 层级 API 和 arena/cid 数据模型绑定，cpu-form 无法安全承载 sub ABI。 */
	if (!sch->is_cid_type) {
		if (scx_parent(sch)) {
			scx_error(sch, "sub-sched requires cid-form struct_ops");
			return -EINVAL;
		}
		if (ops->sub_attach || ops->sub_detach) {
			scx_error(sch, "sub_attach/sub_detach requires cid-form struct_ops");
			/* root cpu-form 也不能宣告 sub 能力，否则 child 无法共享 cid/arena 模型。 */
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * 业务背景：root scheduler 的启用会改变全机任务调度类，必须由专用 RT worker 串行执行，
 * 以免调用者在 fair->ext 切换中被饿死，并保证全局仅有一个 enable/disable 事务。
 * 入参 work 内嵌于 scx_enable_cmd，借用 ops，成功时把 arena 引用转交新 sch。
 * 出参/返回：无直接返回；将同步结果写入 cmd->ret。提交前普通错误返回 errno；进入
 * ENABLING 后的错误通过 ops.exit 完整上报，cmd->ret 置 0 表示异步退出通道已接管。
 * 锁与顺序：scx_enable_mutex 串行全局状态；cpus_read_lock 固定拓扑；fork_rwsem 与
 * cgroup 锁冻结任务集合/归属；任务先全部 READY，再打开静态键并原子切换调度类。
 */
static void scx_root_enable_workfn(struct kthread_work *work)
{
	struct scx_enable_cmd *cmd = container_of(work, struct scx_enable_cmd, work);
	struct sched_ext_ops *ops = cmd->ops;
	struct cgroup *cgrp = root_cgroup();
	struct scx_sched *sch;
	struct scx_task_iter sti;
	struct task_struct *p;
	int i, cpu, ret;

	mutex_lock(&scx_enable_mutex);

	/* 阶段一：确认旧实例彻底解绑，并准备全局共享资源和未接管任务的新 sch。 */
	if (scx_enable_state() != SCX_DISABLED) {
		ret = -EBUSY;
		goto err_unlock;
	}

	/*
	 * @ops->priv binds @ops to its scx_sched instance. It is set here by
	 * scx_alloc_and_add_sched() and cleared at the tail of bpf_scx_unreg(),
	 * which runs after scx_root_disable() has dropped scx_enable_mutex. If
	 * it's still non-NULL here, a previous attachment on @ops has not
	 * finished tearing down; proceeding would let the in-flight unreg's
	 * RCU_INIT_POINTER(NULL) clobber the @ops->priv we are about to assign.
	 */
	/* 非 NULL 不仅表示已启用，也可能是旧实例尚在 RCU teardown，均须拒绝复用。 */
	if (rcu_access_pointer(ops->priv)) {
		ret = -EBUSY;
		goto err_unlock;
	}

	ret = alloc_kick_syncs();
	if (ret)
		goto err_unlock;

	/* tid 哈希必须先于 task 插入和静态键发布建立，只由 root 特性拥有。 */
	if (ops->flags & SCX_OPS_TID_TO_TASK) {
		ret = rhashtable_init(&scx_tid_hash, &scx_tid_hash_params);
		if (ret)
			goto err_free_ksyncs;
	}

#ifdef CONFIG_EXT_SUB_SCHED
	cgroup_get(cgrp);
#endif
	/* allocator 消费 root cgroup 引用并返回资源完整、但尚未接管 task 的实例。 */
	sch = scx_alloc_and_add_sched(cmd, cgrp, NULL);
	if (IS_ERR(sch)) {
		ret = PTR_ERR(sch);
		goto err_free_tid_hash;
	}

	if (sch->is_cid_type)
		static_branch_enable(&__scx_is_cid_type);

	/* 阶段二：进入 ENABLING；从这里起失败必须走统一 disable，而不能局部释放 sch。 */
	/*
	 * Transition to ENABLING and clear exit info to arm the disable path.
	 * Failure triggers full disabling from here on.
	 */
	/* 状态切到 ENABLING 并清退出信息后，disable 路径正式武装；从此失败都要完整 teardown。 */
	WARN_ON_ONCE(scx_set_enable_state(SCX_ENABLING) != SCX_DISABLED);
	WARN_ON_ONCE(scx_root);

	atomic_long_set(&scx_nr_rejected, 0);

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		rq->scx.local_dsq.sched = sch;
		rq->scx.cpuperf_target = SCX_CPUPERF_ONE;
	}

	/*
	 * Keep CPUs stable during enable so that the BPF scheduler can track
	 * online CPUs by watching ->on/offline_cpu() after ->init().
	 */
	/* enable 期间冻结 CPU hotplug，使 BPF 从 on/offline 回调建立的在线视图不会漏事件。 */
	cpus_read_lock();

	/*
	 * Build the cid mapping before publishing scx_root. The cid kfuncs
	 * dereference the cid arrays unconditionally once scx_prog_sched()
	 * returns non-NULL; the rcu_assign_pointer() below pairs with their
	 * rcu_dereference() to make the populated arrays visible.
	 */
	/* cid 数组是 root 可见性的前置依赖，失败时 scx_root 尚未发布。 */
	ret = scx_cid_init(sch);
	if (ret) {
		cpus_read_unlock();
		goto err_disable;
	}

	/*
	 * Make the scheduler instance visible. Must be inside cpus_read_lock().
	 * See handle_hotplug().
	 */
/* RCU 发布后连接 watchdog/全局链，并在稳定 CPU 拓扑下初始化 BPF 与辅助池。 */
	rcu_assign_pointer(scx_root, sch);

	/* RCU 发布后连接 watchdog/全局链，并在稳定 CPU 拓扑下初始化 BPF 与辅助池。 */
	ret = scx_link_sched(sch);
	if (ret) {
		cpus_read_unlock();
		goto err_disable;
	}

	scx_idle_enable(ops);

	if (sch->ops.init) {
		ret = SCX_CALL_OP_RET(sch, init, NULL);
		if (ret) {
			/* sanitize 限制 BPF errno，释放 CPU hotplug 读锁后再 claim error。 */
			ret = ops_sanitize_err(sch, "init", ret);
			cpus_read_unlock();
			scx_error(sch, "ops.init() failed (%d)", ret);
			goto err_disable;
		}
		sch->exit_info->flags |= SCX_EFLAG_INITIALIZED;
	}

	ret = scx_arena_pool_init(sch);
	if (ret) {
		/* arena pool 失败发生在 BPF init 后，必须经 ops.exit 而非同步 allocator 回滚。 */
		cpus_read_unlock();
		goto err_disable;
	}

	/* arena pool 成功后才能从其中分配 cid set_cmask 的 per-CPU scratch。 */
	ret = scx_set_cmask_scratch_alloc(sch);
	if (ret) {
		cpus_read_unlock();
		goto err_disable;
	}

	for (i = SCX_OPI_CPU_HOTPLUG_BEGIN; i < SCX_OPI_CPU_HOTPLUG_END; i++)
		/* 先只发布 hotplug 回调位，普通 ops 要等 ext_server/bypass 准备完毕。 */
		if (((void (**)(void))ops)[i])
			set_bit(i, sch->has_op);

	ret = check_hotplug_seq(sch, ops);
	if (ret) {
		cpus_read_unlock();
		goto err_disable;
	}
	/* hotplug 协议通过后才把当前拓扑交给 builtin idle 选择层。 */
	scx_idle_update_selcpu_topology(ops);

	cpus_read_unlock();

	ret = validate_ops(sch, ops);
	if (ret)
		goto err_disable;

	/* 阶段三：预留每 CPU ext_server 带宽；任一 CPU 失败由 disable 撤销已附加部分。 */
	/*
	 * Attach the ext_server bandwidth reservation before anything is
	 * committed so that we can fail the enable if the root domain cannot
	 * accommodate it. The matching fair_server detach is deferred to the
	 * tail of this function, after the switch is fully committed and can no
	 * longer fail.
	 *
	 * On failure, err_disable funnels into scx_root_disable() which
	 * detaches ext_server, so partially-attached state is cleaned up
	 * automatically.
	 */
/* 每 CPU attach 在 rq 锁内与 deadline 带宽域串行。 */
	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		scoped_guard(rq_lock_irqsave, rq) {
			update_rq_clock(rq);
			/* 每 CPU attach 在 rq 锁内与 deadline 带宽域串行。 */
			ret = dl_server_attach_bw(&rq->ext_server);
		}
		if (ret) {
			pr_warn("sched_ext: failed to attach ext_server on CPU %d (%d)\n",
				cpu, ret);
			goto err_disable;
		}
	}

	/*
	 * Once __scx_enabled is set, %current can be switched to SCX anytime.
	 * This can lead to stalls as some BPF schedulers (e.g. userspace
	 * scheduling) may not function correctly before all tasks are switched.
	 * Init in bypass mode to guarantee forward progress.
	 */
	/* bypass 在静态键打开前建立，任何提前切入 EXT 的 current 都有内核保底队列。 */
	scx_bypass(sch, true);

	for (i = SCX_OPI_NORMAL_BEGIN; i < SCX_OPI_NORMAL_END; i++)
		if (((void (**)(void))ops)[i])
			set_bit(i, sch->has_op);

	if (sch->ops.cpu_acquire || sch->ops.cpu_release)
		sch->ops.flags |= SCX_OPS_HAS_CPU_PREEMPT;

	/* 从此冻结 fork/cgroup 变化，直到现存 task 全部完成 init 并到达 READY。 */
	/*
	 * Lock out forks, cgroup on/offlining and moves before opening the
	 * floodgate so that they don't wander into the operations prematurely.
	 */
	/* 在发布静态键前排除 fork、cgroup 上下线和迁移，防止 task 在全量切换窗口游离于两个 scheduler。 */
	percpu_down_write(&scx_fork_rwsem);

	WARN_ON_ONCE(scx_init_task_enabled);
	scx_init_task_enabled = true;

	/* flip under fork_rwsem; the iter below covers existing tasks */
	/* 在 fork_rwsem 下翻转发布状态，随后 iterator 覆盖所有已存在 task，新 fork 则看到新状态。 */
	if (ops->flags & SCX_OPS_TID_TO_TASK)
		static_branch_enable(&__scx_tid_to_task_enabled);

	/*
	 * Enable ops for every task. Fork is excluded by scx_fork_rwsem
	 * preventing new tasks from being added. No need to exclude tasks
	 * leaving as sched_ext_free() can handle both prepped and enabled
	 * tasks. Prep all tasks first and then enable them with preemption
	 * disabled.
	 *
	 * All cgroups should be initialized before scx_init_task() so that the
	 * BPF scheduler can reliably track each task's cgroup membership from
	 * scx_init_task(). Lock out cgroup on/offlining and task migrations
	 * while tasks are being initialized so that scx_cgroup_can_attach()
	 * never sees uninitialized tasks.
	 */
/* 先初始化所有 cgroup，再逐任务执行 init_task，保证回调观察到一致的归属关系。 */
	scx_cgroup_lock();
	/* 先初始化所有 cgroup，再逐任务执行 init_task，保证回调观察到一致的归属关系。 */
	set_cgroup_sched(sch_cgroup(sch), sch);
	ret = scx_cgroup_init(sch);
	if (ret)
		goto err_disable_unlock_all;

	scx_task_iter_start(&sti, NULL);
	while ((p = scx_task_iter_next_locked(&sti))) {
		/*
		 * @p is in scx_tasks under scx_tasks_lock, and SCX_TASK_DEAD
		 * tasks are filtered by scx_task_iter_next_locked().
		 * sched_ext_dead() removes @p from scx_tasks under the same
		 * lock before put_task_struct_rcu_user() runs, so @p->usage
		 * is guaranteed > 0 here.
		 */
		/* p 在 scx_tasks_lock 下属于全局链，iterator 已过滤 DEAD；这里仍需按当前 task 状态完成准备。 */
		get_task_struct(p);

		/*
		 * Set %INIT_BEGIN under the iter's rq lock so that a concurrent
		 * sched_ext_dead() does not call ops.exit_task() on @p while
		 * ops.init_task() is running. If sched_ext_dead() runs before
		 * this store, it has already removed @p from scx_tasks and the
		 * iter won't visit @p; if it runs after, it observes
		 * %INIT_BEGIN and transitions to %DEAD without calling ops,
		 * leaving the post-init recheck below to unwind.
		 */
		/* INIT_BEGIN 把“正在无 rq 锁运行 BPF init”编码进状态机，dead 路径只标 DEAD。 */
		scx_set_task_state(p, SCX_TASK_INIT_BEGIN);
		scx_task_iter_unlock(&sti);

		ret = __scx_init_task(sch, p, false);

		scx_task_iter_relock(&sti, p);

		if (unlikely(ret)) {
			/* 未死亡 task 撤回 NONE；全局 disable 会补偿此前已初始化成员。 */
			if (scx_get_task_state(p) != SCX_TASK_DEAD)
				scx_set_task_state(p, SCX_TASK_NONE);
			scx_task_iter_stop(&sti);
			scx_error(sch, "ops.init_task() failed (%d) for %s[%d]",
				  ret, p->comm, p->pid);
			put_task_struct(p);
			goto err_disable_unlock_all;
		}

		if (scx_get_task_state(p) == SCX_TASK_DEAD) {
			/* dead 已负责旧生命周期，当前 enable 只需补本轮 init 对应的 cancelled exit。 */
			/*
			 * sched_ext_dead() observed %INIT_BEGIN and set %DEAD.
			 * ops.exit_task() is owed to the sched __scx_init_task()
			 * ran against; call it now.
			 */
/* init 成功且未死亡，按 INIT→绑定 sch→READY 顺序发布给后续提交阶段。 */
			scx_sub_init_cancel_task(sch, p);
		} else {
			/* init 成功且未死亡，按 INIT→绑定 sch→READY 顺序发布给后续提交阶段。 */
			scx_set_task_state(p, SCX_TASK_INIT);
			scx_set_task_sched(p, sch);
			scx_set_task_state(p, SCX_TASK_READY);
		}

		/*
		 * Insert into the tid hash. scx_tasks_lock is held by the iter;
		 * list_empty() guards against sched_ext_dead() having taken @p
		 * off the list while init ran unlocked.
		 */
		/* tid 插入与 tasks_node 检查同持全局迭代锁，避免已摘链对象进入哈希。 */
		if (scx_tid_to_task_enabled() && !list_empty(&p->scx.tasks_node))
			scx_tid_hash_insert(p);

		put_task_struct(p);
	}
	scx_task_iter_stop(&sti);
	scx_cgroup_unlock();
	percpu_up_write(&scx_fork_rwsem);

	/* 阶段四（提交点）：所有任务均 READY；打开快路径后只做不会失败的调度类切换。 */
	/*
	 * All tasks are READY. It's safe to turn on scx_enabled() and switch
	 * all eligible tasks.
	 */
	/* 所有 task 已进入 READY，此时开启 scx_enabled 并发布热路径不会暴露半初始化实体。 */
	WRITE_ONCE(scx_switching_all, !(ops->flags & SCX_OPS_SWITCH_PARTIAL));
	static_branch_enable(&__scx_enabled);

	/*
	 * We're fully committed and can't fail. The task READY -> ENABLED
	 * transitions here are synchronized against sched_ext_free() through
	 * scx_tasks_lock.
	 */
	/* 事务已越过不可失败边界；READY 到 ENABLED 与 fork/exit 由既有锁协议同步。 */
	percpu_down_write(&scx_fork_rwsem);
	scx_task_iter_start(&sti, NULL);
	while ((p = scx_task_iter_next_locked(&sti))) {
		unsigned int queue_flags = DEQUEUE_SAVE | DEQUEUE_MOVE;
		const struct sched_class *old_class = p->sched_class;
		const struct sched_class *new_class = scx_setscheduler_class(p);

		if (scx_get_task_state(p) != SCX_TASK_READY)
			continue;

		/* sched_change 在 rq 锁协议下切 class，同时保存任务原有排队语义。 */
		if (old_class != new_class)
			queue_flags |= DEQUEUE_CLASS;

		scoped_guard (sched_change, p, queue_flags) {
			p->scx.slice = READ_ONCE(sch->slice_dfl);
			p->sched_class = new_class;
		}
	}
	scx_task_iter_stop(&sti);
	percpu_up_write(&scx_fork_rwsem);

	scx_bypass(sch, false);

	/* exit 可能与提交竞态；CAS 失败说明退出已 claim，转入同一 disable 收尾。 */
	if (!scx_tryset_enable_state(SCX_ENABLED, SCX_ENABLING)) {
		WARN_ON_ONCE(atomic_read(&sch->exit_kind) == SCX_EXIT_NONE);
		goto err_disable;
	}

	if (!(ops->flags & SCX_OPS_SWITCH_PARTIAL))
		static_branch_enable(&__scx_switched_all);

	/*
	 * Detach the fair_server bandwidth reservation now that the switch
	 * is fully committed. In full mode (!SCX_OPS_SWITCH_PARTIAL) no
	 * task will ever run in the fair class, so give that bandwidth
	 * back to the RT class. The matching ext_server attach already
	 * happened earlier; this only releases bandwidth and cannot fail.
	 *
	 * In partial mode keep fair_server attached.
	 */
/* full 模式无 fair task，可把 fair_server 带宽归还 RT 域。 */
	if (scx_switched_all()) {
		for_each_possible_cpu(cpu) {
			struct rq *rq = cpu_rq(cpu);

			/* full 模式无 fair task，可把 fair_server 带宽归还 RT 域。 */
			guard(rq_lock_irqsave)(rq);
			update_rq_clock(rq);
			dl_server_detach_bw(&rq->fair_server);
		}
	}

	pr_info("sched_ext: BPF scheduler \"%s\" enabled%s\n",
		sch->ops.name, scx_switched_all() ? "" : " (partial)");
	kobject_uevent(&sch->kobj, KOBJ_ADD);
	/* 对外通知完成后释放 enable mutex，再递增历史序列供观察者检测新实例。 */
	mutex_unlock(&scx_enable_mutex);

	atomic_long_inc(&scx_enable_seq);

	cmd->ret = 0;
	return;

	/* 提交点前的纯资源错误仍可同步返回给 struct_ops 注册调用者。 */
err_free_tid_hash:
	if (ops->flags & SCX_OPS_TID_TO_TASK)
		rhashtable_free_and_destroy(&scx_tid_hash, NULL, NULL);
err_free_ksyncs:
	free_kick_syncs();
err_unlock:
	mutex_unlock(&scx_enable_mutex);
	/* sch 尚未进入 ENABLING 的错误同步返回注册者，不需要 ops.exit。 */
	cmd->ret = ret;
	return;

err_disable_unlock_all:
	scx_cgroup_unlock();
	percpu_up_write(&scx_fork_rwsem);
	/* we'll soon enter disable path, keep bypass on */
/* ENABLING 后统一用 exit_info + disable work 回滚，保留比单一 errno 更完整的诊断。 */
err_disable:
	/* ENABLING 后统一用 exit_info + disable work 回滚，保留比单一 errno 更完整的诊断。 */
	mutex_unlock(&scx_enable_mutex);
	/*
	 * Returning an error code here would not pass all the error information
	 * to userspace. Record errno using scx_error() for cases scx_error()
	 * wasn't already invoked and exit indicating success so that the error
	 * is notified through ops.exit() with all the details.
	 *
	 * Flush scx_disable_work to ensure that error is reported before init
	 * completion. sch's base reference will be put by bpf_scx_unreg().
	 */
	/* 这里只返回 errno 会丢失完整退出信息，因此先用 scx_error 记录上下文再进入统一失败回滚。 */
	scx_error(sch, "scx_root_enable() failed (%d)", ret);
	scx_flush_disable_work(sch);
	cmd->ret = 0;
}

#ifdef CONFIG_EXT_SUB_SCHED
/* verify that a scheduler can be attached to @cgrp and return the parent */
/*
 * 业务背景：sub scheduler 只能挂到当前覆盖目标 cgroup 的最近 scheduler 下，且不能
 * 重复占用同一 cgroup，也不能插入一个正在管理后代的既有 child 之上。
 * 入参 cgrp 为目标控制组；调用者必须持 scx_sched_lock 稳定层级和 children 链。
 * 返回：合法 parent 裸指针，或 -EBUSY/-EOPNOTSUPP；引用需由调用者在解锁前另行获取。
 */
static struct scx_sched *find_parent_sched(struct cgroup *cgrp)
{
	struct scx_sched *parent = cgrp->scx_sched;
	struct scx_sched *pos;

	lockdep_assert_held(&scx_sched_lock);

	/* can't attach twice to the same cgroup */
	/* 同一 cgroup 不能重复附着 scheduler，否则父子拓扑和 ownership 会出现两个竞争实例。 */
	if (parent->cgrp == cgrp)
		return ERR_PTR(-EBUSY);

	/* does $parent allow sub-scheds? */
	/* parent 未实现 sub_attach 即未声明层级协议，不能只靠 cgroup 指针强行挂载。 */
	if (!parent->ops.sub_attach)
		return ERR_PTR(-EOPNOTSUPP);

	/* can't insert between $parent and its exiting children */
/*
 * 检查 sub 启用迁移所要求的 task 生命周期状态。入参 p 在迭代器/rq 锁协议下有效；
 * READY 或 ENABLED 返回 true，其余状态告警并返回 false，不修改任务。
 */
	list_for_each_entry(pos, &parent->children, sibling)
		if (cgroup_is_descendant(pos->cgrp, cgrp))
			return ERR_PTR(-EBUSY);

	return parent;
}

/*
 * 检查 sub 启用迁移所要求的 task 生命周期状态。入参 p 在迭代器/rq 锁协议下有效；
 * READY 或 ENABLED 返回 true，其余状态告警并返回 false，不修改任务。
 */
static bool assert_task_ready_or_enabled(struct task_struct *p)
{
	u32 state = scx_get_task_state(p);

	switch (state) {
	case SCX_TASK_READY:
	case SCX_TASK_ENABLED:
		return true;
	default:
		/* INIT/NONE/DEAD 均不能参与 parent→child 所有权迁移。 */
		WARN_ONCE(true, "sched_ext: Invalid task state %d for %s[%d] during enabling sub sched",
			  state, p->comm, p->pid);
		return false;
	}
}

/*
 * 业务背景：sub scheduler 只接管一个 cgroup 子树；为保持服务连续性，必须先在仍归
 * parent 管理时为所有目标任务完成 child init，全部成功后再批量切换所有权。
 * 入参 work 内嵌于 cmd，ops->sub_cgroup_id 指定目标；cmd 借用 ops/arena。
 * 出参/返回：结果写 cmd->ret；创建 sch 后的失败经 exit/disable 上报并写 0。
 * 锁与回滚：enable_mutex 串行事务，scx_sched_lock 稳定父子关系，fork_rwsem+cgroup
 * 锁冻结任务/归属；SCX_TASK_SUB_INIT 是预备日志，失败时逐项调用 child exit_task。
 */
static void scx_sub_enable_workfn(struct kthread_work *work)
{
	struct scx_enable_cmd *cmd = container_of(work, struct scx_enable_cmd, work);
	struct sched_ext_ops *ops = cmd->ops;
	struct cgroup *cgrp;
	struct scx_sched *parent, *sch;
	struct scx_task_iter sti;
	struct task_struct *p;
	s32 i, ret;

	mutex_lock(&scx_enable_mutex);

	/* 阶段一：解析并引用目标 cgroup/parent，随后构造尚未接管任务的 child sch。 */
	if (!scx_enabled()) {
		ret = -ENODEV;
		goto out_unlock;
	}

	/* See scx_root_enable_workfn() for the @ops->priv check. */
/* 同一 ops 的旧 priv 未清表示 teardown 未结束，sub 注册也必须等待。 */
	if (rcu_access_pointer(ops->priv)) {
		/* 同一 ops 的旧 priv 未清表示 teardown 未结束，sub 注册也必须等待。 */
		ret = -EBUSY;
		goto out_unlock;
	}

	cgrp = cgroup_get_from_id(ops->sub_cgroup_id);
	if (IS_ERR(cgrp)) {
		ret = PTR_ERR(cgrp);
		goto out_unlock;
	}

	/* cgroup 引用取得后，所有后续早期错误必须走 out_put 或由 allocator 消费。 */
	raw_spin_lock_irq(&scx_sched_lock);
	/* 在层级锁内验证 parent 并取 kobject 引用，解锁后 parent 仍可供 allocator 使用。 */
	parent = find_parent_sched(cgrp);
	if (IS_ERR(parent)) {
		raw_spin_unlock_irq(&scx_sched_lock);
		ret = PTR_ERR(parent);
		goto out_put_cgrp;
	}
	kobject_get(&parent->kobj);
	raw_spin_unlock_irq(&scx_sched_lock);

	/* scx_alloc_and_add_sched() consumes @cgrp whether it succeeds or not */
	/* parent 临时引用跨越 allocator，返回后立即归还；child 自身另有生存期 pin。 */
	sch = scx_alloc_and_add_sched(cmd, cgrp, parent);
	kobject_put(&parent->kobj);
	if (IS_ERR(sch)) {
		ret = PTR_ERR(sch);
		goto out_unlock;
	}

	ret = scx_link_sched(sch);
	if (ret)
		goto err_disable;

	/* 阶段二：验证层级和回调协议，并先让 parent 明确接受这个 child。 */
	if (sch->level >= SCX_SUB_MAX_DEPTH) {
		scx_error(sch, "max nesting depth %d violated",
			  SCX_SUB_MAX_DEPTH);
		goto err_disable;
	}

	if (sch->ops.init) {
		ret = SCX_CALL_OP_RET(sch, init, NULL);
		/* child init 错误由 child exit_info 记录，不能归因到 parent。 */
		if (ret) {
			ret = ops_sanitize_err(sch, "init", ret);
			scx_error(sch, "ops.init() failed (%d)", ret);
			goto err_disable;
		}
		sch->exit_info->flags |= SCX_EFLAG_INITIALIZED;
	}

	/* child 的 arena/scratch 与 root 相同地在 parent 接纳前完成，失败不泄露层级状态。 */
	ret = scx_arena_pool_init(sch);
	if (ret)
		goto err_disable;

	ret = scx_set_cmask_scratch_alloc(sch);
	if (ret)
		goto err_disable;

	if (validate_ops(sch, ops))
		goto err_disable;

	/* child 自身验证完成后才调用 parent 接纳，成功结果由 sub_attached 记录以便 detach。 */
	struct scx_sub_attach_args sub_attach_args = {
		.ops = &sch->ops,
		.cgroup_path = sch->cgrp_path,
	};

	ret = SCX_CALL_OP_RET(parent, sub_attach, NULL,
			      &sub_attach_args);
	/* parent 返回值决定是否建立层级关系，sanitize 后由 child 统一退出。 */
	if (ret) {
		ret = ops_sanitize_err(sch, "sub_attach", ret);
		scx_error(sch, "parent rejected (%d)", ret);
		goto err_disable;
	}
	/* 只有成功回调才置位，disable 据此决定是否欠 parent 一次 sub_detach。 */
	sch->sub_attached = true;

	scx_bypass(sch, true);

	for (i = SCX_OPI_BEGIN; i < SCX_OPI_END; i++)
		if (((void (**)(void))ops)[i])
			set_bit(i, sch->has_op);

	percpu_down_write(&scx_fork_rwsem);
	scx_cgroup_lock();

	/* 发布 cgroup->sch 后检查 ONLINE；notifier 与本事务竞态时会触发统一退出。 */
	/*
	 * Set cgroup->scx_sched's and check CSS_ONLINE. Either we see
	 * !CSS_ONLINE or scx_cgroup_lifetime_notify() sees and shoots us down.
	 */
	/* 先发布各 cgroup 的 scx_sched 再检查 CSS_ONLINE；要么本路径发现离线，要么 notifier 观察发布并触发处理。 */
	set_cgroup_sched(sch_cgroup(sch), sch);
	if (!(cgrp->self.flags & CSS_ONLINE)) {
		scx_error(sch, "cgroup is not online");
		goto err_unlock_and_disable;
	}

	/*
	 * Initialize tasks for the new child $sch without exiting them for
	 * $parent so that the tasks can always be reverted back to $parent
	 * sched on child init failure.
	 */
/* 阶段三（可回滚预备）：只运行 child init_task，用 SUB_INIT 记录应补偿的任务。 */
	WARN_ON_ONCE(scx_enabling_sub_sched);
	scx_enabling_sub_sched = sch;

	/* 阶段三（可回滚预备）：只运行 child init_task，用 SUB_INIT 记录应补偿的任务。 */
	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		struct rq *rq;
		struct rq_flags rf;

		/*
		 * Task iteration may visit the same task twice when racing
		 * against exiting. Use %SCX_TASK_SUB_INIT to mark tasks which
		 * finished __scx_init_task() and skip if set.
		 *
		 * A task may exit and get freed between __scx_init_task()
		 * completion and scx_enable_task(). In such cases,
		 * scx_disable_and_exit_task() must exit the task for both the
		 * parent and child scheds.
		 */
/* 退出竞态可能让 iterator 重访，同一 child init 绝不能执行两次。 */
		if (p->scx.flags & SCX_TASK_SUB_INIT)
			/* 退出竞态可能让 iterator 重访，同一 child init 绝不能执行两次。 */
			continue;

		/* @p is pinned by the iter; see scx_sub_disable() */
		/* iterator 为 p 持有稳定引用，具体 pin 生命周期与 scx_sub_disable 的配对说明相同。 */
		get_task_struct(p);

		if (!assert_task_ready_or_enabled(p)) {
			ret = -EINVAL;
			goto abort;
		}

		scx_task_iter_unlock(&sti);

		/*
		 * As $p is still on $parent, it can't be transitioned to INIT.
		 * Let's worry about task state later. Use __scx_init_task().
		 */
		/* child init 在不改变当前 parent 状态下运行，SUB_INIT 充当跨阶段预备日志。 */
		ret = __scx_init_task(sch, p, false);
		if (ret)
			goto abort;

		rq = task_rq_lock(p, &rf);

		if (scx_get_task_state(p) == SCX_TASK_DEAD) {
			/*
			 * sched_ext_dead() raced us between __scx_init_task()
			 * and this rq lock and ran exit_task() on $parent (the
			 * sched @p was on at that point), not on @sch. @sch's
			 * just-completed init is owed an exit_task() and we
			 * issue it here.
			 */
/* child init 已完成却无法提交，立即补 cancelled exit 并释放 pin。 */
			scx_sub_init_cancel_task(sch, p);
			/* child init 已完成却无法提交，立即补 cancelled exit 并释放 pin。 */
			task_rq_unlock(rq, p, &rf);
			put_task_struct(p);
			continue;
		}

		p->scx.flags |= SCX_TASK_SUB_INIT;
		task_rq_unlock(rq, p, &rf);

		put_task_struct(p);
	}
	scx_task_iter_stop(&sti);

	/* 阶段四（提交）：预备全部成功后，逐任务退出 parent、改写 sch，再启用 child。 */
	/*
	 * All tasks are prepped. Disable/exit tasks for $parent and enable for
	 * the new @sch.
	 */
	/* 所有 task 已为新实例准备完毕；现在从 parent disable/exit，再切到新 sch 并 enable，形成提交阶段。 */
	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		/*
		 * Use clearing of %SCX_TASK_SUB_INIT to detect and skip
		 * duplicate iterations.
		 */
		/* 清除 SCX_TASK_SUB_INIT 作为已处理标记，后续重复遍历据此跳过同一 task。 */
		if (!(p->scx.flags & SCX_TASK_SUB_INIT))
			continue;

		scoped_guard (sched_change, p, DEQUEUE_SAVE | DEQUEUE_MOVE) {
			/*
			 * $p must be either READY or ENABLED. If ENABLED,
			 * __scx_disabled_and_exit_task() first disables and
			 * makes it READY. However, after exiting $p, it will
			 * leave $p as READY.
			 */
			/* p 必须处于 READY 或 ENABLED；若已启用先 disable/exit 回到 READY，再执行父子归属切换。 */
			assert_task_ready_or_enabled(p);
			__scx_disable_and_exit_task(parent, p);

			/*
			 * $p is now only initialized for @sch and READY, which
			 * is what we want. Assign it to @sch and enable.
			 */
/* 清日志是该 task 提交完成标志，错误回滚扫描不会再补偿它。 */
			scx_set_task_sched(p, sch);
			scx_enable_task(sch, p);

			p->scx.flags &= ~SCX_TASK_SUB_INIT;
			/* 清日志是该 task 提交完成标志，错误回滚扫描不会再补偿它。 */
		}
	}
	scx_task_iter_stop(&sti);

	scx_enabling_sub_sched = NULL;

	scx_cgroup_unlock();
	percpu_up_write(&scx_fork_rwsem);

	scx_bypass(sch, false);

	pr_info("sched_ext: BPF sub-scheduler \"%s\" enabled\n", sch->ops.name);
	kobject_uevent(&sch->kobj, KOBJ_ADD);
	/* child 没有独立全局 enable state；完成标志由拓扑、cgroup 指针和 task 归属共同体现。 */
	ret = 0;
	goto out_unlock;

out_put_cgrp:
	cgroup_put(cgrp);
out_unlock:
	mutex_unlock(&scx_enable_mutex);
	cmd->ret = ret;
	return;

abort:
	/* 当前 p 尚未交给迭代器收尾，先释放其额外引用，再扫描预备日志执行补偿。 */
	put_task_struct(p);
	scx_task_iter_stop(&sti);

	/*
	 * Undo __scx_init_task() for tasks we marked. scx_enable_task() never
	 * ran for @sch on them, so calling scx_disable_task() here would invoke
	 * ops.disable() without a matching ops.enable(). scx_enabling_sub_sched
	 * must stay set until SUB_INIT is cleared from every marked task -
	 * scx_disable_and_exit_task() reads it when a task exits concurrently.
	 */
/* 仅补偿已完成 child init、尚未进入提交阶段的 task。 */
	scx_task_iter_start(&sti, sch->cgrp);
	while ((p = scx_task_iter_next_locked(&sti))) {
		/* 仅补偿已完成 child init、尚未进入提交阶段的 task。 */
		if (p->scx.flags & SCX_TASK_SUB_INIT) {
			scx_sub_init_cancel_task(sch, p);
			p->scx.flags &= ~SCX_TASK_SUB_INIT;
		}
	}
	scx_task_iter_stop(&sti);
	/* 所有预备日志清空后再撤全局 enabling 指针，退出路径不再需要双 scheduler 补偿。 */
	scx_enabling_sub_sched = NULL;
err_unlock_and_disable:
	/* we'll soon enter disable path, keep bypass on */
/* child 已发布后失败必须由 disable 路径解绑 parent、cgroup、kobject 和 BPF 资源。 */
	scx_cgroup_unlock();
	percpu_up_write(&scx_fork_rwsem);
err_disable:
	/* child 已发布后失败必须由 disable 路径解绑 parent、cgroup、kobject 和 BPF 资源。 */
	mutex_unlock(&scx_enable_mutex);
	/*
	 * Some enable failures only return an errno (e.g. -ENOMEM from an
	 * allocation) without calling scx_error(). Record it so
	 * scx_flush_disable_work() runs the disable and ops.exit() fires.
	 */
/*
 * cgroup v2 生命周期桥接：ONLINE 时从父组继承 scheduler 的 RCU 指针；OFFLINE 时若该组
 * 正是 sub scheduler 挂载点，则触发内核注销退出。action/data 来自 notifier；始终返回
 * NOTIFY_OK，失败通过异步 disable 上报，不阻断 cgroup 自身生命周期。
 */
	scx_error(sch, "scx_sub_enable() failed (%d)", ret);
	scx_flush_disable_work(sch);
	cmd->ret = 0;
}

/*
 * cgroup v2 生命周期桥接：ONLINE 时从父组继承 scheduler 的 RCU 指针；OFFLINE 时若该组
 * 正是 sub scheduler 挂载点，则触发内核注销退出。action/data 来自 notifier；始终返回
 * NOTIFY_OK，失败通过异步 disable 上报，不阻断 cgroup 自身生命周期。
 */
static s32 scx_cgroup_lifetime_notify(struct notifier_block *nb,
				      unsigned long action, void *data)
{
	struct cgroup *cgrp = data;
	struct cgroup *parent = cgroup_parent(cgrp);

	if (!cgroup_on_dfl(cgrp))
		return NOTIFY_OK;

	/* 只处理 default hierarchy；ONLINE 发布继承，OFFLINE 关闭直接挂载实例。 */
	switch (action) {
	case CGROUP_LIFETIME_ONLINE:
		/* inherit ->scx_sched from $parent */
		/* 新对象继承 parent 的 scx_sched 归属，随后再由当前事务决定是否切到 child。 */
		if (parent)
			rcu_assign_pointer(cgrp->scx_sched, parent->scx_sched);
		break;
	case CGROUP_LIFETIME_OFFLINE:
		/* if there is a sched attached, shoot it down */
		/* 只关闭直接挂在该 cgroup 的实例；继承父指针的普通后代不得误伤 parent。 */
		if (cgrp->scx_sched && cgrp->scx_sched->cgrp == cgrp)
			scx_exit(cgrp->scx_sched, SCX_EXIT_UNREG_KERN,
				 SCX_ECODE_RSN_CGROUP_OFFLINE,
				 "cgroup %llu going offline", cgroup_id(cgrp));
		break;
	}

	/* 未识别 action 也不阻断 cgroup core，notifier 仅维护 SCX 镜像状态。 */
	return NOTIFY_OK;
}

static struct notifier_block scx_cgroup_lifetime_nb = {
	.notifier_call = scx_cgroup_lifetime_notify,
};

/* 启动早期注册 cgroup 生命周期观察者；返回 notifier 注册结果给 core_initcall。 */
static s32 __init scx_cgroup_lifetime_notifier_init(void)
{
	return blocking_notifier_chain_register(&cgroup_lifetime_notifier,
						&scx_cgroup_lifetime_nb);
}
core_initcall(scx_cgroup_lifetime_notifier_init);
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

/* 串行完成协商、资源分配、逐 task 初始化/启用和静态键发布；失败完整回滚。 */
/*
 * 串行完成协商、资源分配、逐 task 初始化/启用和静态键发布；失败完整回滚。
 * cmd 是同步命令/结果载体，link 当前不参与决策；返回 worker 写入的 errno 或 0。
 * 首次调用在 mutex 下惰性创建全局 FIFO worker，root/sub 工作都在它上面排队并同步等待。
 */
static s32 scx_enable(struct scx_enable_cmd *cmd, struct bpf_link *link)
{
	static struct kthread_worker *helper;
	static DEFINE_MUTEX(helper_mutex);

	if (housekeeping_enabled(HK_TYPE_DOMAIN_BOOT)) {
		pr_err("sched_ext: Not compatible with \"isolcpus=\" domain isolation\n");
		return -EINVAL;
	}

	if (!READ_ONCE(helper)) {
		/* 双重检查只允许 mutex 内的创建者发布永久 worker。 */
		mutex_lock(&helper_mutex);
		if (!helper) {
			struct kthread_worker *w =
				kthread_run_worker(0, "scx_enable_helper");
			if (IS_ERR_OR_NULL(w)) {
				mutex_unlock(&helper_mutex);
				return -ENOMEM;
			}
			/* FIFO 优先级避免启用线程在切换自身 class 后被 fair 饱和负载饿死。 */
			sched_set_fifo(w->task);
			WRITE_ONCE(helper, w);
		}
		/* 发布后 worker 永久存活；并发调用者在 mutex 外只读同一指针。 */
		mutex_unlock(&helper_mutex);
	}

#ifdef CONFIG_EXT_SUB_SCHED
	if (cmd->ops->sub_cgroup_id > 1)
		kthread_init_work(&cmd->work, scx_sub_enable_workfn);
	else
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */
		kthread_init_work(&cmd->work, scx_root_enable_workfn);

	kthread_queue_work(READ_ONCE(helper), &cmd->work);
	/* cmd 位于当前栈上，必须等 work 完成后才能读取 ret 并释放该栈帧。 */
	kthread_flush_work(&cmd->work);
	return cmd->ret;
}


/********************************************************************************
 * bpf_struct_ops plumbing.
 */
/*
 * 验证 struct_ops 回调上下文访问：只允许按 size 对齐地读取 BPF 参数槽范围，再由
 * btf_ctx_access 校验具体 BTF 类型。入参是偏移、宽度、访问类型及验证器上下文；
 * 合法返回 true，其余 false；不改变程序或内核状态。
 */
#include <linux/bpf_verifier.h>
#include <linux/bpf.h>
#include <linux/btf.h>

static const struct btf_type *task_struct_type;

/*
 * 验证 struct_ops 回调上下文访问：只允许按 size 对齐地读取 BPF 参数槽范围，再由
 * btf_ctx_access 校验具体 BTF 类型。入参是偏移、宽度、访问类型及验证器上下文；
 * 合法返回 true，其余 false；不改变程序或内核状态。
 */
static bool bpf_scx_is_valid_access(int off, int size,
				    enum bpf_access_type type,
				    const struct bpf_prog *prog,
				    struct bpf_insn_access_aux *info)
{
	if (type != BPF_READ)
		return false;
	/* 参数区恰为 MAX_BPF_FUNC_ARGS 个 u64 槽，且访问起点必须按宽度对齐。 */
	if (off < 0 || off >= sizeof(__u64) * MAX_BPF_FUNC_ARGS)
		return false;
	if (off % size != 0)
		return false;

	return btf_ctx_access(off, size, type, prog, info);
}

/*
 * 仅开放 task_struct 中明确列出的可写字段。reg 描述 BTF 对象，off/size 描述区间；
 * 允许时返回 SCALAR_VALUE，否则 -EACCES。slice/vtime 直写只是兼容窗口并会告警。
 */
static int bpf_scx_btf_struct_access(struct bpf_verifier_log *log,
				     const struct bpf_reg_state *reg, int off,
				     int size)
{
	const struct btf_type *t;

	t = btf_type_by_id(reg->btf, reg->btf_id);
	if (t == task_struct_type) {
		/* 仅 task_struct 参与 sched_ext 特例，其他 BTF 对象一律拒绝写。 */
		/*
		 * COMPAT: Will be removed in v6.23.
		 */
/* 标为 scalar writable，但限流告警推动程序迁移到带权限检查的 kfunc。 */
		if ((off >= offsetof(struct task_struct, scx.slice) &&
		     off + size <= offsetofend(struct task_struct, scx.slice)) ||
		    (off >= offsetof(struct task_struct, scx.dsq_vtime) &&
		     off + size <= offsetofend(struct task_struct, scx.dsq_vtime))) {
			pr_warn_ratelimited("sched_ext: Writing directly to p->scx.slice/dsq_vtime is deprecated, use scx_bpf_task_set_slice/dsq_vtime()\n");
			/* 标为 scalar writable，但限流告警推动程序迁移到带权限检查的 kfunc。 */
			return SCALAR_VALUE;
		}

		if (off >= offsetof(struct task_struct, scx.disallow) &&
		    off + size <= offsetofend(struct task_struct, scx.disallow))
			return SCALAR_VALUE;
	}

	/* 白名单之外包括跨字段和部分覆盖，统一返回访问拒绝。 */
	return -EACCES;
}

static const struct bpf_verifier_ops bpf_scx_verifier_ops = {
	.get_func_proto = bpf_base_func_proto,
	.is_valid_access = bpf_scx_is_valid_access,
	.btf_struct_access = bpf_scx_btf_struct_access,
};

/*
 * 把用户 struct_ops 的非函数成员复制并规范化进内核 kdata。t/member 定位字段，udata
 * 是用户镜像；返回 1 表示已处理，0 交给通用逻辑，负 errno 拒绝注册。这里集中限制
 * flags、batch、timeout、name 等 ABI 边界，避免启用阶段接触畸形配置。
 */
static int bpf_scx_init_member(const struct btf_type *t,
			       const struct btf_member *member,
			       void *kdata, const void *udata)
{
	const struct sched_ext_ops *uops = udata;
	struct sched_ext_ops *ops = kdata;
	u32 moff = __btf_member_bit_offset(t, member) / 8;
	/* moff 是共享 cpu/cid ABI 的字节偏移，scx_init 会编译期验证两者一致。 */
	int ret;

	switch (moff) {
	case offsetof(struct sched_ext_ops, dispatch_max_batch):
		if (*(u32 *)(udata + moff) > INT_MAX)
			return -E2BIG;
		ops->dispatch_max_batch = *(u32 *)(udata + moff);
		/* 返回 1 告诉 struct_ops core 该成员已经完整复制。 */
		return 1;
	case offsetof(struct sched_ext_ops, flags):
		if (*(u64 *)(udata + moff) & ~SCX_OPS_ALL_FLAGS)
			return -EINVAL;
		ops->flags = *(u64 *)(udata + moff);
		return 1;
	case offsetof(struct sched_ext_ops, name):
		/* 名称复制使用 BPF 对象命名规则，禁止空名和不合法字符。 */
		ret = bpf_obj_name_cpy(ops->name, uops->name,
				       sizeof(ops->name));
		if (ret < 0)
			return ret;
		if (ret == 0)
			return -EINVAL;
		/* 名称必须非空且已由 bpf_obj_name_cpy 校验字符集/终止符。 */
		return 1;
	case offsetof(struct sched_ext_ops, timeout_ms):
		if (msecs_to_jiffies(*(u32 *)(udata + moff)) >
		    SCX_WATCHDOG_MAX_TIMEOUT)
			return -E2BIG;
		ops->timeout_ms = *(u32 *)(udata + moff);
		return 1;
	/* dump 长度零值规范化为默认容量，避免启用后没有诊断缓冲。 */
	case offsetof(struct sched_ext_ops, exit_dump_len):
		ops->exit_dump_len =
			*(u32 *)(udata + moff) ?: SCX_EXIT_DUMP_DFL_LEN;
		return 1;
	case offsetof(struct sched_ext_ops, hotplug_seq):
		ops->hotplug_seq = *(u64 *)(udata + moff);
		/* hotplug_seq 原样保留，真正代际比较延迟到固定拓扑的 enable 阶段。 */
		return 1;
#ifdef CONFIG_EXT_SUB_SCHED
	case offsetof(struct sched_ext_ops, sub_cgroup_id):
		ops->sub_cgroup_id = *(u64 *)(udata + moff);
		return 1;
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */
	}

	return 0;
}

#ifdef CONFIG_EXT_SUB_SCHED
/* dispatch 私有栈检测到层级递归时，把错误归属到发起该 BPF prog 的 scheduler。 */
static void scx_pstack_recursion_on_dispatch(struct bpf_prog *prog)
{
	struct scx_sched *sch;

	guard(rcu)();
	sch = scx_prog_sched(prog->aux);
	if (unlikely(!sch))
		return;

	scx_error(sch, "dispatch recursion detected");
}
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

/*
 * 检查单个回调成员的程序属性：只有列出的初始化/退出类回调允许 sleepable，调度原子
 * 路径上的 sleepable 程序返回 -EINVAL；sub dispatch 还申请私有栈和递归报告器。
 */
static int bpf_scx_check_member(const struct btf_type *t,
				const struct btf_member *member,
				const struct bpf_prog *prog)
{
	u32 moff = __btf_member_bit_offset(t, member) / 8;

	/* 仅 dispatch 可能沿 scheduler 树递归，其他回调沿用普通 BPF 栈。 */
	switch (moff) {
	case offsetof(struct sched_ext_ops, init_task):
#ifdef CONFIG_EXT_GROUP_SCHED
	case offsetof(struct sched_ext_ops, cgroup_init):
	case offsetof(struct sched_ext_ops, cgroup_exit):
	case offsetof(struct sched_ext_ops, cgroup_prep_move):
		/* 这些生命周期回调允许阻塞，故 sleepable 标志直接放行到后续检查。 */
#endif
	case offsetof(struct sched_ext_ops, cpu_online):
	case offsetof(struct sched_ext_ops, cpu_offline):
	case offsetof(struct sched_ext_ops, init):
	case offsetof(struct sched_ext_ops, exit):
	case offsetof(struct sched_ext_ops, sub_attach):
	case offsetof(struct sched_ext_ops, sub_detach):
		break;
	default:
		/* 调度热路径回调不能睡眠；只对白名单生命周期操作例外。 */
		if (prog->sleepable)
			return -EINVAL;
	}

#ifdef CONFIG_EXT_SUB_SCHED
	/*
	 * Enable private stack for operations that can nest along the
	 * hierarchy.
	 *
	 * XXX - Ideally, we should only do this for scheds that allow
	 * sub-scheds and sub-scheds themselves but I don't know how to access
	 * struct_ops from here.
	 */
	/* dispatch 可沿 sub 层级嵌套，私有栈隔离每层帧并由 hook 报告递归。 */
	switch (moff) {
	case offsetof(struct sched_ext_ops, dispatch):
		prog->aux->priv_stack_requested = true;
		prog->aux->recursion_detected = scx_pstack_recursion_on_dispatch;
	}
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

	return 0;
}

/* cpu-form 注册入口：用栈上同步命令调用统一启用流程并原样返回结果。 */
static int bpf_scx_reg(void *kdata, struct bpf_link *link)
{
	struct scx_enable_cmd cmd = { .ops = kdata };

	return scx_enable(&cmd, link);
}

struct scx_arena_scan {
	struct bpf_map	*arena;
	int		err;
};

/*
 * The verifier enforces one arena per BPF program, so each struct_ops
 * member prog contributes at most one arena via bpf_prog_arena().
 * Require all non-NULL contributions to match.
 */
/* 每个成员最多贡献一个 arena；首次记录，后续不同则置 -EINVAL 并提前终止扫描。 */
static int scx_arena_scan_prog(struct bpf_prog *prog, void *data)
{
	struct scx_arena_scan *s = data;
	struct bpf_map *arena = NULL;

	/* arena.o, which defines these, is built only on MMU && 64BIT */
/* 非 arena prog 不贡献约束；多个非空贡献必须是同一 map 指针。 */
#if defined(CONFIG_MMU) && defined(CONFIG_64BIT)
	arena = bpf_prog_arena(prog);
#endif
	/* 非 arena prog 不贡献约束；多个非空贡献必须是同一 map 指针。 */
	if (!arena)
		return 0;
	if (s->arena && s->arena != arena) {
		s->err = -EINVAL;
		return 1;
	}
	s->arena = arena;
	return 0;
}

/*
 * cid-form 注册要求所有成员共享且必须存在一个 arena。先增引用再启用；若 allocator
 * 未清空 cmd.arena_map，说明所有权未转移，本函数负责 put。返回校验或启用结果。
 */
static int bpf_scx_reg_cid(void *kdata, struct bpf_link *link)
{
	struct scx_enable_cmd cmd = { .ops_cid = kdata, .is_cid_type = true };
	struct scx_arena_scan scan = {};
	int ret;

	/* 扫描所有成员后才能确定唯一 arena；回调提前停止时 scan.err 保存原因。 */
	bpf_struct_ops_for_each_prog(kdata, scx_arena_scan_prog, &scan);
	if (scan.err) {
		pr_err("sched_ext: cid-form scheduler uses multiple arena maps\n");
		return scan.err;
	}
	if (!scan.arena) {
		/* cid cmask 数据通过 arena 交换，因此 cid-form 不能在无 arena 时启用。 */
		pr_err("sched_ext: cid-form scheduler must use a BPF arena map\n");
		return -EINVAL;
	}

	bpf_map_inc(scan.arena);
	cmd.arena_map = scan.arena;
	ret = scx_enable(&cmd, link);
	if (cmd.arena_map)		/* not consumed by scx_alloc_and_add_sched() */
	/* 若分配启用路径未消费 arena_map，注册包装层必须归还这份引用。 */
		bpf_map_put(cmd.arena_map);
	return ret;
}

/*
 * 注销入口触发 UNREG、同步等待 disable，随后清 ops->priv 并放基础 kobject 引用；
 * 该顺序保证旧注销不会在新注册发布同一 ops 后再把新 priv 清空。
 */
static void bpf_scx_unreg(void *kdata, struct bpf_link *link)
{
	struct sched_ext_ops *ops = kdata;
	struct scx_sched *sch = rcu_dereference_protected(ops->priv, true);

	scx_disable(sch, SCX_EXIT_UNREG);
	scx_flush_disable_work(sch);
	RCU_INIT_POINTER(ops->priv, NULL);
	kobject_put(&sch->kobj);
}

/* 缓存 vmlinux BTF 的 task_struct 类型供字段白名单比较；成功返回 0。 */
static int bpf_scx_init(struct btf *btf)
{
	task_struct_type = btf_type_by_id(btf, btf_tracing_ids[BTF_TRACING_TYPE_TASK]);

	return 0;
}

/* 活跃 scheduler 不能原地替换：新注册可失败且会与注销竞态，固定返回不支持。 */
static int bpf_scx_update(void *kdata, void *old_kdata, struct bpf_link *link)
{
	/*
	 * sched_ext does not support updating the actively-loaded BPF
	 * scheduler, as registering a BPF scheduler can always fail if the
	 * scheduler returns an error code for e.g. ops.init(), ops.init_task(),
	 * etc. Similarly, we can always race with unregistration happening
	 * elsewhere, such as with sysrq.
	 */
/* 整体无额外静态校验；跨字段运行时协议由 enable 阶段 validate_ops 处理。 */
	return -EOPNOTSUPP;
}

/* 整体无额外静态校验；跨字段运行时协议由 enable 阶段 validate_ops 处理。 */
static int bpf_scx_validate(void *kdata)
{
	return 0;
}

/* 以下空函数只为 CFI 提供与 BPF 回调一致的类型签名，运行时由已验证程序替代。 */
/* 业务背景：为 select_cpu 回调建立 CFI 目标类型；入参：p、prev_cpu、wake_flags 仅定义 ABI；出参/返回：保守返回 -EINVAL；注意事项：不应作为真实调度决策执行。 */
static s32 sched_ext_ops__select_cpu(struct task_struct *p, s32 prev_cpu, u64 wake_flags) { return -EINVAL; }
/* 业务背景：为 enqueue 回调建立 CFI 目标类型；入参：p 与 enq_flags 只承载 ABI；出参/返回：void，无副作用；注意事项：真实回调由已验证 BPF 程序提供。 */
static void sched_ext_ops__enqueue(struct task_struct *p, u64 enq_flags) {}
/* 业务背景：为 dequeue 回调建立 CFI 目标类型；入参：p 与 enq_flags 只承载 ABI；出参/返回：void，无副作用；注意事项：本空框架不得代替 scheduler 的移除逻辑。 */
static void sched_ext_ops__dequeue(struct task_struct *p, u64 enq_flags) {}
/* 业务背景：为 dispatch 回调建立 CFI 目标类型；入参：prev_cpu 与可空 prev 只承载 ABI；出参/返回：void，不提交 DSQ；注意事项：本体不能保证调度前进。 */
static void sched_ext_ops__dispatch(s32 prev_cpu, struct task_struct *prev__nullable) {}
/* 业务背景：为 tick 回调建立 CFI 目标类型；入参：p 只定义 ABI；出参/返回：void，不改 slice；注意事项：真实时钟策略必须由 BPF 回调执行。 */
static void sched_ext_ops__tick(struct task_struct *p) {}
static void sched_ext_ops__runnable(struct task_struct *p, u64 enq_flags) {}
static void sched_ext_ops__running(struct task_struct *p) {}
static void sched_ext_ops__stopping(struct task_struct *p, bool runnable) {}
static void sched_ext_ops__quiescent(struct task_struct *p, u64 deq_flags) {}
/* 有返回值的 stub 使用保守失败/false，防止意外调用被当作调度决策成功。 */
static bool sched_ext_ops__yield(struct task_struct *from, struct task_struct *to__nullable) { return false; }
static bool sched_ext_ops__core_sched_before(struct task_struct *a, struct task_struct *b) { return false; }
static void sched_ext_ops__set_weight(struct task_struct *p, u32 weight) {}
static void sched_ext_ops__set_cpumask(struct task_struct *p, const struct cpumask *mask) {}
static void sched_ext_ops__update_idle(s32 cpu, bool idle) {}
static void sched_ext_ops__cpu_acquire(s32 cpu, struct scx_cpu_acquire_args *args) {}
static void sched_ext_ops__cpu_release(s32 cpu, struct scx_cpu_release_args *args) {}
/* task 生命周期 stub 保持 init 可失败、exit/enable/disable 无副作用的保守组合。 */
static s32 sched_ext_ops__init_task(struct task_struct *p, struct scx_init_task_args *args) { return -EINVAL; }
static void sched_ext_ops__exit_task(struct task_struct *p, struct scx_exit_task_args *args) {}
static void sched_ext_ops__enable(struct task_struct *p) {}
static void sched_ext_ops__disable(struct task_struct *p) {}
#ifdef CONFIG_EXT_GROUP_SCHED
/* cgroup stub 仅承载类型；init/prep 默认拒绝，其余为空操作。 */
static s32 sched_ext_ops__cgroup_init(struct cgroup *cgrp, struct scx_cgroup_init_args *args) { return -EINVAL; }
static void sched_ext_ops__cgroup_exit(struct cgroup *cgrp) {}
static s32 sched_ext_ops__cgroup_prep_move(struct task_struct *p, struct cgroup *from, struct cgroup *to) { return -EINVAL; }
static void sched_ext_ops__cgroup_move(struct task_struct *p, struct cgroup *from, struct cgroup *to) {}
static void sched_ext_ops__cgroup_cancel_move(struct task_struct *p, struct cgroup *from, struct cgroup *to) {}
static void sched_ext_ops__cgroup_set_weight(struct cgroup *cgrp, u32 weight) {}
static void sched_ext_ops__cgroup_set_bandwidth(struct cgroup *cgrp, u64 period_us, u64 quota_us, u64 burst_us) {}
static void sched_ext_ops__cgroup_set_idle(struct cgroup *cgrp, bool idle) {}
#endif	/* CONFIG_EXT_GROUP_SCHED */
/* sub、hotplug、全局生命周期和 dump 回调也汇入 cpu-form CFI 原型集合。 */
static s32 sched_ext_ops__sub_attach(struct scx_sub_attach_args *args) { return -EINVAL; }
static void sched_ext_ops__sub_detach(struct scx_sub_detach_args *args) {}
static void sched_ext_ops__cpu_online(s32 cpu) {}
static void sched_ext_ops__cpu_offline(s32 cpu) {}
static s32 sched_ext_ops__init(void) { return -EINVAL; }
static void sched_ext_ops__exit(struct scx_exit_info *info) {}
static void sched_ext_ops__dump(struct scx_dump_ctx *ctx) {}
static void sched_ext_ops__dump_cpu(struct scx_dump_ctx *ctx, s32 cpu, bool idle) {}
static void sched_ext_ops__dump_task(struct scx_dump_ctx *ctx, struct task_struct *p) {}

/* cpu-form CFI 原型表：字段类型必须与 ABI 一致，本身不是可运行 scheduler。 */
static struct sched_ext_ops __bpf_ops_sched_ext_ops = {
	.select_cpu		= sched_ext_ops__select_cpu,
	.enqueue		= sched_ext_ops__enqueue,
	.dequeue		= sched_ext_ops__dequeue,
	.dispatch		= sched_ext_ops__dispatch,
	.tick			= sched_ext_ops__tick,
	.runnable		= sched_ext_ops__runnable,
	.running		= sched_ext_ops__running,
	.stopping		= sched_ext_ops__stopping,
	.quiescent		= sched_ext_ops__quiescent,
	/* 调度策略与任务属性通知共享上面的强类型空实现。 */
	.yield			= sched_ext_ops__yield,
	.core_sched_before	= sched_ext_ops__core_sched_before,
	.set_weight		= sched_ext_ops__set_weight,
	.set_cpumask		= sched_ext_ops__set_cpumask,
	.update_idle		= sched_ext_ops__update_idle,
	.cpu_acquire		= sched_ext_ops__cpu_acquire,
	.cpu_release		= sched_ext_ops__cpu_release,
	/* task 生命周期字段连续映射，便于 CFI 按实际 BPF 回调签名校验间接调用。 */
	.init_task		= sched_ext_ops__init_task,
	.exit_task		= sched_ext_ops__exit_task,
	.enable			= sched_ext_ops__enable,
	.disable		= sched_ext_ops__disable,
#ifdef CONFIG_EXT_GROUP_SCHED
	/* 配置打开时将整组 cgroup 生命周期签名纳入 CFI 覆盖。 */
	.cgroup_init		= sched_ext_ops__cgroup_init,
	.cgroup_exit		= sched_ext_ops__cgroup_exit,
	.cgroup_prep_move	= sched_ext_ops__cgroup_prep_move,
	.cgroup_move		= sched_ext_ops__cgroup_move,
	.cgroup_cancel_move	= sched_ext_ops__cgroup_cancel_move,
	.cgroup_set_weight	= sched_ext_ops__cgroup_set_weight,
	.cgroup_set_bandwidth	= sched_ext_ops__cgroup_set_bandwidth,
	.cgroup_set_idle	= sched_ext_ops__cgroup_set_idle,
#endif
	/* 最后一组覆盖层级、CPU 生命周期、全局生命周期与诊断。 */
	.sub_attach		= sched_ext_ops__sub_attach,
	.sub_detach		= sched_ext_ops__sub_detach,
	.cpu_online		= sched_ext_ops__cpu_online,
	.cpu_offline		= sched_ext_ops__cpu_offline,
	.init			= sched_ext_ops__init,
	.exit			= sched_ext_ops__exit,
	.dump			= sched_ext_ops__dump,
	.dump_cpu		= sched_ext_ops__dump_cpu,
	.dump_task		= sched_ext_ops__dump_task,
};

/* cpu-form struct_ops 描述符把验证、注册、注销及 CFI 钩子交给 BPF 核心。 */
static struct bpf_struct_ops bpf_sched_ext_ops = {
	.verifier_ops = &bpf_scx_verifier_ops,
	.reg = bpf_scx_reg,
	.unreg = bpf_scx_unreg,
	.check_member = bpf_scx_check_member,
	.init_member = bpf_scx_init_member,
	.init = bpf_scx_init,
	.update = bpf_scx_update,
	/* update 永久拒绝原地替换，validate 的跨字段检查延迟到 enable。 */
	.validate = bpf_scx_validate,
	.name = "sched_ext_ops",
	.owner = THIS_MODULE,
	.cfi_stubs = &__bpf_ops_sched_ext_ops
};

/*
 * cid-form cfi stubs. Stubs whose signatures match the cpu-form (param types
 * identical, only param names differ across structs) are reused; only
 * set_cmask needs a fresh stub since the second argument type differs.
 */
/* cid-form 仅 set_cmask 参数类型不同，需独立 stub；其余安全复用 cpu-form 签名。 */
static void sched_ext_ops_cid__set_cmask(struct task_struct *p,
					 const struct scx_cmask *cmask) {}

/* cid-form CFI 表把 select/dump 的逻辑编号解释为 cid，并加入专属 set_cmask。 */
static struct sched_ext_ops_cid __bpf_ops_sched_ext_ops_cid = {
	.select_cid		= sched_ext_ops__select_cpu,
	.enqueue		= sched_ext_ops__enqueue,
	.dequeue		= sched_ext_ops__dequeue,
	.dispatch		= sched_ext_ops__dispatch,
	.tick			= sched_ext_ops__tick,
	.runnable		= sched_ext_ops__runnable,
	.running		= sched_ext_ops__running,
	.stopping		= sched_ext_ops__stopping,
	.quiescent		= sched_ext_ops__quiescent,
	/* 任务策略回调与 cpu-form ABI 相同，可共享其类型 stub。 */
	.yield			= sched_ext_ops__yield,
	.core_sched_before	= sched_ext_ops__core_sched_before,
	.set_weight		= sched_ext_ops__set_weight,
	.set_cmask		= sched_ext_ops_cid__set_cmask,
	.update_idle		= sched_ext_ops__update_idle,
	.init_task		= sched_ext_ops__init_task,
	.exit_task		= sched_ext_ops__exit_task,
	.enable			= sched_ext_ops__enable,
	.disable		= sched_ext_ops__disable,
#ifdef CONFIG_EXT_GROUP_SCHED
	/* 两种形态的 cgroup 字节布局由 scx_init 中 BUILD_BUG_ON 保证一致。 */
	.cgroup_init		= sched_ext_ops__cgroup_init,
	.cgroup_exit		= sched_ext_ops__cgroup_exit,
	.cgroup_prep_move	= sched_ext_ops__cgroup_prep_move,
	.cgroup_move		= sched_ext_ops__cgroup_move,
	.cgroup_cancel_move	= sched_ext_ops__cgroup_cancel_move,
	.cgroup_set_weight	= sched_ext_ops__cgroup_set_weight,
	.cgroup_set_bandwidth	= sched_ext_ops__cgroup_set_bandwidth,
	.cgroup_set_idle	= sched_ext_ops__cgroup_set_idle,
#endif
	/* cid_online/offline 与 cpu stub 参数同为 s32，具体语义由真实 BPF 程序实现。 */
	.sub_attach		= sched_ext_ops__sub_attach,
	.sub_detach		= sched_ext_ops__sub_detach,
	.cid_online		= sched_ext_ops__cpu_online,
	.cid_offline		= sched_ext_ops__cpu_offline,
	.init			= sched_ext_ops__init,
	.exit			= sched_ext_ops__exit,
	.dump			= sched_ext_ops__dump,
	.dump_cid		= sched_ext_ops__dump_cpu,
	.dump_task		= sched_ext_ops__dump_task,
};

/*
 * The cid-form struct_ops shares all bpf_struct_ops hooks with the cpu form.
 * init_member, check_member, reg, unreg, etc. process kdata as the byte block
 * verified to match by the BUILD_BUG_ON checks in scx_init().
 */
/* cid 描述符复用框架钩子，仅注册入口额外执行 arena 一致性和必需性检查。 */
static struct bpf_struct_ops bpf_sched_ext_ops_cid = {
	.verifier_ops = &bpf_scx_verifier_ops,
	.reg = bpf_scx_reg_cid,
	.unreg = bpf_scx_unreg,
	.check_member = bpf_scx_check_member,
	.init_member = bpf_scx_init_member,
	.init = bpf_scx_init,
	.update = bpf_scx_update,
	/* cid 描述符只更换注册入口/CFI 表，验证与注销语义保持一致。 */
	.validate = bpf_scx_validate,
	.name = "sched_ext_ops_cid",
	.owner = THIS_MODULE,
	.cfi_stubs = &__bpf_ops_sched_ext_ops_cid
};


/********************************************************************************
 * System integration and init.
 */

/* SysRq-S 紧急复位：RCU 读取 root，有实例则异步禁用全部 SCX，否则仅记录未加载。 */
static void sysrq_handle_sched_ext_reset(u8 key)
{
	struct scx_sched *sch;

	sch = rcu_dereference(scx_root);
	if (likely(sch))
		scx_disable(sch, SCX_EXIT_SYSRQ);
	else
		/* 无实例时不制造错误，仅告诉操作者复位动作没有目标。 */
		pr_info("sched_ext: BPF schedulers not loaded\n");
}

static const struct sysrq_key_op sysrq_sched_ext_reset_op = {
	.handler	= sysrq_handle_sched_ext_reset,
	.help_msg	= "reset-sched-ext(S)",
	.action_msg	= "Disable sched_ext and revert all tasks to CFS",
	.enable_mask	= SYSRQ_ENABLE_RTNICE,
};

/* SysRq-D 为所有已发布 scheduler 构造非退出 dump；不 claim exit、不改变运行状态。 */
static void sysrq_handle_sched_ext_dump(u8 key)
{
	struct scx_exit_info ei = {
		.kind		= SCX_EXIT_NONE,
		.exit_cpu	= -1,
		.reason		= "SysRq-D",
	};
	struct scx_sched *sch;

	/* 全局链由 RCU 保护；每实例 dump_all=false，避免 task 在多实例输出中重复。 */
	list_for_each_entry_rcu(sch, &scx_sched_all, all)
		scx_dump_state(sch, &ei, 0, false);
}

static const struct sysrq_key_op sysrq_sched_ext_dump_op = {
	.handler	= sysrq_handle_sched_ext_dump,
	.help_msg	= "dump-sched-ext(D)",
	.action_msg	= "Trigger sched_ext debug dump",
	.enable_mask	= SYSRQ_ENABLE_RTNICE,
};

/* rq 锁内仅在确定目标必经一次 SCX 调度周期时省略 idle kick；返回是否确定可跳过。 */
static bool can_skip_idle_kick(struct rq *rq)
{
	lockdep_assert_rq_held(rq);

	/*
	 * We can skip idle kicking if @rq is going to go through at least one
	 * full SCX scheduling cycle before going idle. Just checking whether
	 * curr is not idle is insufficient because we could be racing
	 * balance_one() trying to pull the next task from a remote rq, which
	 * may fail, and @rq may become idle afterwards.
	 *
	 * The race window is small and we don't and can't guarantee that @rq is
	 * only kicked while idle anyway. Skip only when sure.
	 */
/*
 * 对一个 CPU 合并 preempt/wait/resched：持目标 rq 锁检查在线和调度类，必要时清 slice、
 * 记录 kick_sync 代际。cpu/this_rq/ksyncs 是目标、发起 rq 和输出数组；返回是否需等待。
 */
	return !is_idle_task(rq->curr) && !(rq->scx.flags & SCX_RQ_IN_BALANCE);
}

/*
 * 对一个 CPU 合并 preempt/wait/resched：持目标 rq 锁检查在线和调度类，必要时清 slice、
 * 记录 kick_sync 代际。cpu/this_rq/ksyncs 是目标、发起 rq 和输出数组；返回是否需等待。
 */
static bool kick_one_cpu(s32 cpu, struct rq *this_rq, unsigned long *ksyncs)
{
	struct rq *rq = cpu_rq(cpu);
	struct scx_rq *this_scx = &this_rq->scx;
	const struct sched_class *cur_class;
	bool should_wait = false;
	unsigned long flags;

	raw_spin_rq_lock_irqsave(rq, flags);
	cur_class = rq->curr->sched_class;

	/*
	 * During CPU hotplug, a CPU may depend on kicking itself to make
	 * forward progress. Allow kicking self regardless of online state. If
	 * @cpu is running a higher class task, we have no control over @cpu.
	 * Skip kicking.
	 */
	/* 自 kick 即使 offline 也允许，确保 hotplug 状态机能驱动本 CPU 继续前进。 */
	if ((cpu_online(cpu) || cpu == cpu_of(this_rq)) &&
	    !sched_class_above(cur_class, &ext_sched_class)) {
		if (cpumask_test_cpu(cpu, this_scx->cpus_to_preempt)) {
			if (cur_class == &ext_sched_class)
				rq->curr->scx.slice = 0;
			cpumask_clear_cpu(cpu, this_scx->cpus_to_preempt);
		}

		if (cpumask_test_cpu(cpu, this_scx->cpus_to_wait)) {
			/* 仅 ext 当前任务有可等待代际；更高调度类不受 SCX 控制。 */
			if (cur_class == &ext_sched_class) {
				cpumask_set_cpu(cpu, this_scx->cpus_to_sync);
				ksyncs[cpu] = rq->scx.kick_sync;
				should_wait = true;
			}
			cpumask_clear_cpu(cpu, this_scx->cpus_to_wait);
		}

		resched_curr(rq);
	} else {
		/* 无法控制目标时清本轮 preempt/wait 请求，防止以后误用陈旧标志。 */
		cpumask_clear_cpu(cpu, this_scx->cpus_to_preempt);
		cpumask_clear_cpu(cpu, this_scx->cpus_to_wait);
	}

	raw_spin_rq_unlock_irqrestore(rq, flags);

	return should_wait;
}

/* 只在目标可能直接进入 idle 时重调度；自 kick 在 hotplug 离线过渡中仍允许。 */
static void kick_one_cpu_if_idle(s32 cpu, struct rq *this_rq)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long flags;

	raw_spin_rq_lock_irqsave(rq, flags);

	if (!can_skip_idle_kick(rq) &&
	    (cpu_online(cpu) || cpu == cpu_of(this_rq)))
		resched_curr(rq);

	raw_spin_rq_unlock_irqrestore(rq, flags);
}

/*
 * hardirq 消费当前 rq 聚合的普通/idle kick 位图；ksyncs 可与 disable 回收竞态，先检查
 * RCU 指针。WAIT 不能在 hardirq 自旋，改置 pending 让 balance callback 稍后等待。
 */
static void kick_cpus_irq_workfn(struct irq_work *irq_work)
{
	struct rq *this_rq = this_rq();
	struct scx_rq *this_scx = &this_rq->scx;
	struct scx_kick_syncs __rcu *ksyncs_pcpu = __this_cpu_read(scx_kick_syncs);
	bool should_wait = false;
	unsigned long *ksyncs;
	s32 cpu;

	/* RCU-BH 保护 per-CPU 数组直到本 irq_work 完成遍历。 */
	/* can race with free_kick_syncs() during scheduler disable */
/* 普通 kick 同时消费重叠 idle 位，确保一个目标本轮只处理一次。 */
	if (unlikely(!ksyncs_pcpu))
		return;

	ksyncs = rcu_dereference_bh(ksyncs_pcpu)->syncs;

	for_each_cpu(cpu, this_scx->cpus_to_kick) {
		/* 普通 kick 同时消费重叠 idle 位，确保一个目标本轮只处理一次。 */
		should_wait |= kick_one_cpu(cpu, this_rq, ksyncs);
		cpumask_clear_cpu(cpu, this_scx->cpus_to_kick);
		cpumask_clear_cpu(cpu, this_scx->cpus_to_kick_if_idle);
	}

	for_each_cpu(cpu, this_scx->cpus_to_kick_if_idle) {
		/* 剩余集合仅含纯 idle kick，不需要同步代际等待。 */
		kick_one_cpu_if_idle(cpu, this_rq);
		cpumask_clear_cpu(cpu, this_scx->cpus_to_kick_if_idle);
	}

	/*
	 * Can't wait in hardirq — kick_sync can't advance, deadlocking if
	 * CPUs wait for each other. Defer to kick_sync_wait_bal_cb().
	 */
	/* hardirq 中同步等待会让相互 kick 的 CPU 死锁，因此改由 balance callback 等待。 */
	if (should_wait) {
		raw_spin_rq_lock(this_rq);
		this_scx->kick_sync_pending = true;
		resched_curr(this_rq);
		raw_spin_rq_unlock(this_rq);
	}
}

/**
 * print_scx_info - print out sched_ext scheduler state
 * @log_lvl: the log level to use when printing
 * @p: target task
 *
 * If a sched_ext scheduler is enabled, print the name and state of the
 * scheduler. If @p is on sched_ext, print further information about the task.
 *
 * This function can be safely called on any task as long as the task_struct
 * itself is accessible. While safe, this function isn't synchronized and may
 * print out mixups or garbages of limited length.
 */
/*
 * 打印 scheduler 名称、全局 enable 状态和目标 SCX task 的 runnable 时长。log_lvl/p
 * 是日志前缀和可访问任务；无返回。nofault 快照容忍并发变化，输出只用于诊断。
 */
void print_scx_info(const char *log_lvl, struct task_struct *p)
{
	struct scx_sched *sch;
	enum scx_enable_state state = scx_enable_state();
	const char *all = READ_ONCE(scx_switching_all) ? "+all" : "";
	char runnable_at_buf[22] = "?";
	struct sched_class *class;
	unsigned long runnable_at;

	/* 整个读取位于 RCU 区间，sch/task 关联不会在格式化中途释放。 */
	guard(rcu)();

	sch = scx_task_sched_rcu(p);

	if (!sch)
		return;

	/*
	 * Carefully check if the task was running on sched_ext, and then
	 * carefully copy the time it's been runnable, and its state.
	 */
	/* sched_class 快照失败或已非 EXT 时，只打印实例级信息。 */
	if (copy_from_kernel_nofault(&class, &p->sched_class, sizeof(class)) ||
	    class != &ext_sched_class) {
		printk("%sSched_ext: %s (%s%s)", log_lvl, sch->ops.name,
		       scx_enable_state_str[state], all);
		return;
	}

	if (!copy_from_kernel_nofault(&runnable_at, &p->scx.runnable_at,
				      sizeof(runnable_at)))
		scnprintf(runnable_at_buf, sizeof(runnable_at_buf), "%+ldms",
			  jiffies_delta_msecs(runnable_at, jiffies));

	/* print everything onto one line to conserve console space */
	/* runnable_at 读取失败保留问号，仍保证 console 输出有界且单行。 */
	printk("%sSched_ext: %s (%s%s), task: runnable_at=%s",
	       log_lvl, sch->ops.name, scx_enable_state_str[state], all,
	       runnable_at_buf);
}

/*
 * PM notifier 在冻结用户态前打开 root bypass，恢复后关闭，避免依赖用户态组件的 BPF
 * scheduler 在 suspend/hibernate 中停顿；无 root 或未知事件均返回 NOTIFY_OK。
 */
static int scx_pm_handler(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct scx_sched *sch;

	guard(rcu)();

	sch = rcu_dereference(scx_root);
	if (!sch)
		return NOTIFY_OK;

	/*
	 * SCX schedulers often have userspace components which are sometimes
	 * involved in critial scheduling paths. PM operations involve freezing
	 * userspace which can lead to scheduling misbehaviors including stalls.
	 * Let's bypass while PM operations are in progress.
	 */
/* prepare 类事件统一在冻结用户态前建立 bypass 前进保证。 */
	switch (event) {
	case PM_HIBERNATION_PREPARE:
		/* prepare 类事件统一在冻结用户态前建立 bypass 前进保证。 */
	case PM_SUSPEND_PREPARE:
	case PM_RESTORE_PREPARE:
		scx_bypass(sch, true);
		break;
	case PM_POST_HIBERNATION:
		/* post 类事件解除本次嵌套 bypass，深度协议容纳重叠请求。 */
	case PM_POST_SUSPEND:
	case PM_POST_RESTORE:
		scx_bypass(sch, false);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block scx_pm_notifier = {
	.notifier_call = scx_pm_handler,
};

/*
 * 调度器早期初始化：固化 BTF 枚举、初始化 idle mask 和每 CPU local DSQ/kick 位图/irq_work，
 * 注册 SysRq 与 watchdog。无输入输出；启动期分配失败不可恢复，使用 BUG_ON。
 */
void __init init_sched_ext_class(void)
{
	s32 cpu, v;

	/*
	 * The following is to prevent the compiler from optimizing out the enum
	 * definitions so that BPF scheduler implementations can use them
	 * through the generated vmlinux.h.
	 */
/* idle mask 先于 rq 初始化，使队列上线时即可查询 topology。 */
	WRITE_ONCE(v, SCX_ENQ_WAKEUP | SCX_DEQ_SLEEP | SCX_KICK_PREEMPT |
		   SCX_TG_ONLINE);

	/* idle mask 先于 rq 初始化，使队列上线时即可查询 topology。 */
	scx_idle_init_masks();

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		int  n = cpu_to_node(cpu);

		/* local_dsq's sch will be set during scx_root_enable() */
/* 发起 CPU 独占请求位图，目标 rq 锁只在 irq_work 真正 kick 时获取。 */
		BUG_ON(init_dsq(&rq->scx.local_dsq, SCX_DSQ_LOCAL, NULL));

		INIT_LIST_HEAD(&rq->scx.runnable_list);
		INIT_LIST_HEAD(&rq->scx.ddsp_deferred_locals);

		/* 发起 CPU 独占请求位图，目标 rq 锁只在 irq_work 真正 kick 时获取。 */
		BUG_ON(!zalloc_cpumask_var_node(&rq->scx.cpus_to_kick, GFP_KERNEL, n));
		BUG_ON(!zalloc_cpumask_var_node(&rq->scx.cpus_to_kick_if_idle, GFP_KERNEL, n));
		BUG_ON(!zalloc_cpumask_var_node(&rq->scx.cpus_to_preempt, GFP_KERNEL, n));
		BUG_ON(!zalloc_cpumask_var_node(&rq->scx.cpus_to_wait, GFP_KERNEL, n));
		BUG_ON(!zalloc_cpumask_var_node(&rq->scx.cpus_to_sync, GFP_KERNEL, n));
		raw_spin_lock_init(&rq->scx.deferred_reenq_lock);
		/* local/user 两条 deferred 链共享这把 rq 私有锁，但节点类型各自独立。 */
		INIT_LIST_HEAD(&rq->scx.deferred_reenq_locals);
		INIT_LIST_HEAD(&rq->scx.deferred_reenq_users);
		rq->scx.deferred_irq_work = IRQ_WORK_INIT_HARD(deferred_irq_workfn);
		rq->scx.kick_cpus_irq_work = IRQ_WORK_INIT_HARD(kick_cpus_irq_workfn);

		if (cpu_online(cpu))
			/* 启动在线状态写 rq 快速位，后续由 hotplug 回调维护。 */
			cpu_rq(cpu)->scx.flags |= SCX_RQ_ONLINE;
	}

	register_sysrq_key('S', &sysrq_sched_ext_reset_op);
	register_sysrq_key('D', &sysrq_sched_ext_dump_op);
	INIT_DELAYED_WORK(&scx_watchdog_work, scx_watchdog_workfn);

#ifdef CONFIG_EXT_SUB_SCHED
	BUG_ON(rhashtable_init(&scx_sched_hash, &scx_sched_hash_params));
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */
}


/********************************************************************************
 * Helpers that can be called from the BPF scheduler.
 */
/*
 * 规范 BPF enqueue 标志：IMMED 只允许 local DSQ；scheduler 要求 local 立即入队时自动补位。
 * sch/dsq_id/enq_flags 为权限主体、目标和可写标志；合法返回 true，否则记录错误并 false。
 */
static bool scx_vet_enq_flags(struct scx_sched *sch, u64 dsq_id, u64 *enq_flags)
{
	bool is_local = dsq_id == SCX_DSQ_LOCAL ||
		(dsq_id & SCX_DSQ_LOCAL_ON) == SCX_DSQ_LOCAL_ON;

	if (*enq_flags & SCX_ENQ_IMMED) {
		if (unlikely(!is_local)) {
			/* IMMED 必须立即让某 CPU local 队列可选，非 local 目标无法兑现。 */
			scx_error(sch, "SCX_ENQ_IMMED on a non-local DSQ 0x%llx", dsq_id);
			return false;
		}
	} else if ((sch->ops.flags & SCX_OPS_ALWAYS_ENQ_IMMED) && is_local) {
		*enq_flags |= SCX_ENQ_IMMED;
	}

	return true;
}

/*
 * DSQ insert 共用前置检查：IRQ 必须关闭，p 非空、无内部标志且仍归调用 scheduler 所有，
 * 并规范目标相关标志。返回是否可提交；失败只记事件/错误，不取得 task 新引用。
 */
static bool scx_dsq_insert_preamble(struct scx_sched *sch, struct task_struct *p,
				    u64 dsq_id, u64 *enq_flags)
{
	lockdep_assert_irqs_disabled();

	if (unlikely(!p)) {
		scx_error(sch, "called with NULL task");
		return false;
	}

	if (unlikely(*enq_flags & __SCX_ENQ_INTERNAL_MASK)) {
		/* 内部位只由内核队列路径生成，BPF 传入会破坏所有权协议。 */
		scx_error(sch, "invalid enq_flags 0x%llx", *enq_flags);
		return false;
	}

	/* see SCX_EV_INSERT_NOT_OWNED definition */
/* true 仅代表允许提交，task/DSQ 状态到 commit 才发生变化。 */
	if (unlikely(!scx_task_on_sched(sch, p))) {
		__scx_add_event(sch, SCX_EV_INSERT_NOT_OWNED, 1);
		return false;
	}

	if (!scx_vet_enq_flags(sch, dsq_id, enq_flags))
		return false;

	/* true 仅代表允许提交，task/DSQ 状态到 commit 才发生变化。 */
	return true;
}

/*
 * 把已校验请求提交到直接派发槽或当前 CPU dispatch buffer。记录 qseq 使稍后 flush 能识别
 * task 已被竞争方重新入队；缓冲满则退出 scheduler。无返回，task 所有权仍由 BPF custody 协议约束。
 */
static void scx_dsq_insert_commit(struct scx_sched *sch, struct task_struct *p,
				  u64 dsq_id, u64 enq_flags)
{
	struct scx_dsp_ctx *dspc = &this_cpu_ptr(sch->pcpu)->dsp_ctx;
	struct task_struct *ddsp_task;

	ddsp_task = __this_cpu_read(direct_dispatch_task);
	if (ddsp_task) {
		/* select/enqueue 上下文使用一次性直派槽，不占 dispatch batch。 */
		mark_direct_dispatch(sch, ddsp_task, p, dsq_id, enq_flags);
		return;
	}

	if (unlikely(dspc->cursor >= sch->dsp_max_batch)) {
		/* 动态循环可能超出协商 batch，运行时溢出必须终止 scheduler。 */
		scx_error(sch, "dispatch buffer overflow");
		return;
	}

	dspc->buf[dspc->cursor++] = (struct scx_dsp_buf_ent){
		.task = p,
		.qseq = atomic_long_read(&p->scx.ops_state) & SCX_OPSS_QSEQ_MASK,
		/* DSQ/flags 与 qseq 同槽保存，flush 时按代际确认 verdict 仍有效。 */
		.dsq_id = dsq_id,
		.enq_flags = enq_flags,
	};
}

__bpf_kfunc_start_defs();

/**
 * scx_bpf_dsq_insert - Insert a task into the FIFO queue of a DSQ
 * @p: task_struct to insert
 * @dsq_id: DSQ to insert into
 * @slice: duration @p can run for in nsecs, 0 to keep the current value
 * @enq_flags: SCX_ENQ_*
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Insert @p into the FIFO queue of the DSQ identified by @dsq_id. It is safe to
 * call this function spuriously. Can be called from ops.enqueue(),
 * ops.select_cpu(), and ops.dispatch().
 *
 * When called from ops.select_cpu() or ops.enqueue(), it's for direct dispatch
 * and @p must match the task being enqueued.
 *
 * When called from ops.select_cpu(), @enq_flags and @dsp_id are stored, and @p
 * will be directly inserted into the corresponding dispatch queue after
 * ops.select_cpu() returns. If @p is inserted into SCX_DSQ_LOCAL, it will be
 * inserted into the local DSQ of the CPU returned by ops.select_cpu().
 * @enq_flags are OR'd with the enqueue flags on the enqueue path before the
 * task is inserted.
 *
 * When called from ops.dispatch(), there are no restrictions on @p or @dsq_id
 * and this function can be called upto ops.dispatch_max_batch times to insert
 * multiple tasks. scx_bpf_dispatch_nr_slots() returns the number of the
 * remaining slots. scx_bpf_dsq_move_to_local() flushes the batch and resets the
 * counter.
 *
 * This function doesn't have any locking restrictions and may be called under
 * BPF locks (in the future when BPF introduces more flexible locking).
 *
 * @p is allowed to run for @slice. The scheduling path is triggered on slice
 * exhaustion. If zero, the current residual slice is maintained. If
 * %SCX_SLICE_INF, @p never expires and the BPF scheduler must kick the CPU with
 * scx_bpf_kick_cpu() to trigger scheduling.
 *
 * Returns %true on successful insertion, %false on failure. On the root
 * scheduler, %false return triggers scheduler abort and the caller doesn't need
 * to check the return value.
 */
/* verifier 已限制上下文；p 必须仍由当前 enqueue/dispatch 回调拥有，否则报错。 */
/*
 * 业务背景：把 BPF 的 FIFO 派发判定提交到直派槽或本 CPU batch，
 * 供 select_cpu/enqueue/dispatch 三条路径最终发布到 DSQ。
 * 入参：p 是回调借用的 task；dsq_id 指定目标；slice 单位 ns，0 表示保留；
 * enq_flags 是入队属性；aux 是 verifier 隐式传入的程序归属。
 * 出参/返回：成功更新 slice 并缓冲 verdict 后返回 true；无实例或
 * 所有权/标志校验失败返回 false，不得到新 task 引用。
 * 注意事项：必须位于 verifier 允许的回调且 task 仍归该 scheduler；
 * commit 是可观察的派发提交点，batch 溢出会终止 scheduler。
 */
__bpf_kfunc bool scx_bpf_dsq_insert___v2(struct task_struct *p, u64 dsq_id,
					 u64 slice, u64 enq_flags,
					 const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();
	/* 无 scheduler 表示 prog 已解绑，兼容接口静默返回而不访问 task。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return false;

	if (!scx_dsq_insert_preamble(sch, p, dsq_id, &enq_flags))
		return false;

	/* slice=0 保留剩余值，但零剩余补 1ns，防止插入后无法前进。 */
	if (slice)
		p->scx.slice = slice;
	else
		p->scx.slice = p->scx.slice ?: 1;

	scx_dsq_insert_commit(sch, p, dsq_id, enq_flags);

	return true;
}

/*
 * COMPAT: Will be removed in v6.23 along with the ___v2 suffix.
 */
/*
 * 业务背景：为 v6.23 前的 void FIFO 插入 ABI 保留兼容入口。
 * 入参：p、dsq_id、ns 单位 slice、enq_flags 和隐式 aux 均原样借给 v2。
 * 出参/返回：void；成功/失败副作用完全来自 v2，本层不取得引用。
 * 注意事项：旧 ABI 不能观察 false；root 上校验失败由错误路径 abort。
 */
__bpf_kfunc void scx_bpf_dsq_insert(struct task_struct *p, u64 dsq_id,
				    u64 slice, u64 enq_flags,
				    const struct bpf_prog_aux *aux)
{
	scx_bpf_dsq_insert___v2(p, dsq_id, slice, enq_flags, aux);
}

/*
 * 业务背景：为新旧 vtime kfunc 集中完成 PRIQ 插入，保证排序键
 * 与派发 verdict 由同一条提交路径发布。
 * 入参：sch 是 RCU 窗口内借用的归属实例；p 是候选 task；dsq_id 是目标；
 * slice 单位 ns 且 0 表示保留；vtime 是 64 位环绕排序键；enq_flags 是入队属性。
 * 出参/返回：成功写 slice/vtime、缓冲 PRIQ verdict 并返回 true；
 * 校验失败返回 false，不发布到目标 DSQ。
 * 注意事项：p 必须仍归 sch，目标 DSQ 不能已作 FIFO 使用；本函数
 * 不取得 task/DSQ 引用，真正 ownership 转移由后续 flush 验证 qseq 后完成。
 */
static bool scx_dsq_insert_vtime(struct scx_sched *sch, struct task_struct *p,
				 u64 dsq_id, u64 slice, u64 vtime, u64 enq_flags)
{
	if (!scx_dsq_insert_preamble(sch, p, dsq_id, &enq_flags))
		return false;

	if (slice)
		p->scx.slice = slice;
	else
		p->scx.slice = p->scx.slice ?: 1;

	/* vtime 与 PRIQ verdict 在同一提交前写入，flush 后作为排序键可见。 */
	p->scx.dsq_vtime = vtime;

	scx_dsq_insert_commit(sch, p, dsq_id, enq_flags | SCX_ENQ_DSQ_PRIQ);

	return true;
}

struct scx_bpf_dsq_insert_vtime_args {
	/* @p can't be packed together as KF_RCU is not transitive */
	/* BPF 五参数上限要求把除 task/aux 外的值封装；p 单列才能保留 KF_RCU 类型约束。 */
	u64			dsq_id;
	u64			slice;
	u64			vtime;
	u64			enq_flags;
};

/**
 * __scx_bpf_dsq_insert_vtime - Arg-wrapped vtime DSQ insertion
 * @p: task_struct to insert
 * @args: struct containing the rest of the arguments
 *       @args->dsq_id: DSQ to insert into
 *       @args->slice: duration @p can run for in nsecs, 0 to keep the current value
 *       @args->vtime: @p's ordering inside the vtime-sorted queue of the target DSQ
 *       @args->enq_flags: SCX_ENQ_*
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Wrapper kfunc that takes arguments via struct to work around BPF's 5 argument
 * limit. BPF programs should use scx_bpf_dsq_insert_vtime() which is provided
 * as an inline wrapper in common.bpf.h.
 *
 * Insert @p into the vtime priority queue of the DSQ identified by
 * @args->dsq_id. Tasks queued into the priority queue are ordered by
 * @args->vtime. All other aspects are identical to scx_bpf_dsq_insert().
 *
 * @args->vtime ordering is according to time_before64() which considers
 * wrapping. A numerically larger vtime may indicate an earlier position in the
 * ordering and vice-versa.
 *
 * A DSQ can only be used as a FIFO or priority queue at any given time and this
 * function must not be called on a DSQ which already has one or more FIFO tasks
 * queued and vice-versa. Also, the built-in DSQs (SCX_DSQ_LOCAL and
 * SCX_DSQ_GLOBAL) cannot be used as priority queues.
 *
 * Returns %true on successful insertion, %false on failure. On the root
 * scheduler, %false return triggers scheduler abort and the caller doesn't need
 * to check the return value.
 */
/*
 * 业务背景：BPF 调用约定最多五个参数，本包装将 PRIQ 其余参数
 * 聚合到 args，再进入共用 vtime 提交路径。
 * 入参：p 是 KF_RCU 单独校验的借用 task；args 含 DSQ、ns slice、vtime 和
 * flags；aux 隐式绑定调用 BPF prog 的 scheduler，参数均不转移 ownership。
 * 出参/返回：成功提交 PRIQ verdict 返回 true；实例已解绑或校验失败
 * 返回 false，args 本身不作为输出。
 * 注意事项：必须位于 verifier 允许的 enqueue/dispatch 上下文；aux 而非
 * 全局 root 决定权限，因而 sub-scheduler 不会跨实例插入。
 */
__bpf_kfunc bool
__scx_bpf_dsq_insert_vtime(struct task_struct *p,
			   struct scx_bpf_dsq_insert_vtime_args *args,
			   const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	/* aux 绑定调用 prog 的 scheduler，sub 环境不会误取得 root 权限。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return false;

	return scx_dsq_insert_vtime(sch, p, args->dsq_id, args->slice,
				    args->vtime, args->enq_flags);
}

/*
 * COMPAT: Will be removed in v6.23.
 */
/*
 * 业务背景：为无 implicit aux 的旧 PRIQ ABI 保留 root-only 兼容路径。
 * 入参：p 是 RCU 窗口内借用 task；dsq_id、ns slice、vtime 和 enq_flags
 * 用于共用插入实现，均不转移 ownership。
 * 出参/返回：void；无 root 时静默返回，唯一 root 时尝试提交，存在
 * child 时对 p 的 scheduler 记录错误且不插入。
 * 注意事项：无 aux 无法证明 sub-scheduler 调用主体；CONFIG_EXT_SUB_SCHED
 * 开启且 root 有 child 时必须拒绝，新 BPF 程序应调用参数封装版。
 */
__bpf_kfunc void scx_bpf_dsq_insert_vtime(struct task_struct *p, u64 dsq_id,
					  u64 slice, u64 vtime, u64 enq_flags)
{
	struct scx_sched *sch;

	guard(rcu)();

	sch = rcu_dereference(scx_root);
	if (unlikely(!sch))
		return;

#ifdef CONFIG_EXT_SUB_SCHED
	/*
	 * Disallow if any sub-scheds are attached. There is no way to tell
	 * which scheduler called us, just error out @p's scheduler.
	 */
/* 旧接口缺 aux，存在 child 时无法证明调用主体，只能拒绝。 */
	if (unlikely(!list_empty(&sch->children))) {
		/* 旧接口缺 aux，存在 child 时无法证明调用主体，只能拒绝。 */
		scx_error(scx_task_sched(p), "__scx_bpf_dsq_insert_vtime() must be used");
		return;
	}
#endif

	/* 无 child 时旧 ABI 可安全把 root 当作唯一权限主体。 */
	scx_dsq_insert_vtime(sch, p, dsq_id, slice, vtime, enq_flags);
}

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(scx_kfunc_ids_enqueue_dispatch)
BTF_ID_FLAGS(func, scx_bpf_dsq_insert, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_dsq_insert___v2, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, __scx_bpf_dsq_insert_vtime, KF_IMPLICIT_ARGS | KF_RCU)
	/* 旧 vtime API 无 implicit aux，只能在 verifier 已提供 RCU 的上下文使用。 */
BTF_ID_FLAGS(func, scx_bpf_dsq_insert_vtime, KF_RCU)
BTF_KFUNCS_END(scx_kfunc_ids_enqueue_dispatch)

static const struct btf_kfunc_id_set scx_kfunc_set_enqueue_dispatch = {
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_enqueue_dispatch,
	.filter			= scx_kfunc_context_filter,
};

/*
 * 业务背景：iterator 只提供快照候选，真正重排必须在重新验证
 * task 所有权和游标代际后，将其从源 DSQ 原子搬到目标 DSQ。
 * 入参：kit 持有借用 DSQ/cursor 及一次性 slice/vtime；p 是候选 task；
 * dsq_id 和 enq_flags 指定目标与入队语义，入参均不向本函数转移引用。
 * 出参/返回：完成 move 返回 true；迭代未初始化、归属/标志校验失败、
 * abort 或游标丢失均返回 false；两类结果都消费一次性覆盖标志。
 * 注意事项：dispatch 上下文已持 this_rq，其他上下文则自行锁 src_rq；
 * 始终按 rq 再源 DSQ 的顺序加锁，switch_rq_lock 保持仅一把 rq 锁并恢复入口现场。
 */
static bool scx_dsq_move(struct bpf_iter_scx_dsq_kern *kit,
			 struct task_struct *p, u64 dsq_id, u64 enq_flags)
{
	struct scx_dispatch_q *src_dsq = kit->dsq, *dst_dsq;
	struct scx_sched *sch;
	struct rq *this_rq, *src_rq, *locked_rq;
	bool dispatched = false;
	bool in_balance;
	unsigned long flags;

	/*
	 * The verifier considers an iterator slot initialized on any
	 * KF_ITER_NEW return, so a BPF program may legally reach here after
	 * bpf_iter_scx_dsq_new() failed and left @kit->dsq NULL.
	 */
	/* new 失败对象仍会进入 destroy/next，NULL 源必须作为普通 false 处理。 */
	if (unlikely(!src_dsq))
		return false;

	sch = src_dsq->sched;

	if (!scx_vet_enq_flags(sch, dsq_id, &enq_flags))
		return false;

	/*
	 * If the BPF scheduler keeps calling this function repeatedly, it can
	 * cause similar live-lock conditions as consume_dispatch_q().
	 */
/* abort 后禁止新 move，避免 BPF 重试阻塞内核接管。 */
	if (unlikely(READ_ONCE(sch->aborting)))
		return false;

	/* abort 后禁止新 move，避免 BPF 重试阻塞内核接管。 */
	if (unlikely(!scx_task_on_sched(sch, p))) {
		scx_error(sch, "scx_bpf_dsq_move[_vtime]() on %s[%d] but the task belongs to a different scheduler",
			  p->comm, p->pid);
		return false;
	}

	/*
	 * Can be called from either ops.dispatch() locking this_rq() or any
	 * context where no rq lock is held. If latter, lock @p's task_rq which
	 * we'll likely need anyway.
	 */
/* dispatch 已持 this_rq；目标不同用切锁 helper 保持始终只有一个 rq 锁。 */
	src_rq = task_rq(p);

	local_irq_save(flags);
	this_rq = this_rq();
	in_balance = this_rq->scx.flags & SCX_RQ_IN_BALANCE;

	if (in_balance) {
		/* dispatch 已持 this_rq；目标不同用切锁 helper 保持始终只有一个 rq 锁。 */
		if (this_rq != src_rq)
			switch_rq_lock(this_rq, src_rq);
	} else {
		raw_spin_rq_lock(src_rq);
	}

	locked_rq = src_rq;
	raw_spin_lock(&src_dsq->lock);

	/* 锁全部取得后重新验证游标代际，避免基于过期快照移动已被别人消费的 task。 */
	/* did someone else get to it while we dropped the locks? */
	/* 前面曾切换/重取锁，必须复验 task 是否已被其他消费者取走。 */
	if (nldsq_cursor_lost_task(&kit->cursor, src_rq, src_dsq, p)) {
		raw_spin_unlock(&src_dsq->lock);
		goto out;
	}

	/* @p is still on $src_dsq and stable, determine the destination */
	/* 此时 p 仍稳定挂在源 DSQ，可在持锁状态下解析目标队列。 */
	dst_dsq = find_dsq_for_dispatch(sch, this_rq, dsq_id, task_cpu(p));

	/*
	 * Apply vtime and slice updates before moving so that the new time is
	 * visible before inserting into $dst_dsq. @p is still on $src_dsq but
	 * this is safe as we're locking it.
	 */
	/* 移动前先写新 slice/vtime；源 DSQ 锁使排序节点在重链前不会被并发观察。 */
	if (kit->cursor.flags & __SCX_DSQ_ITER_HAS_VTIME)
		p->scx.dsq_vtime = kit->vtime;
	if (kit->cursor.flags & __SCX_DSQ_ITER_HAS_SLICE)
		p->scx.slice = kit->slice;

	/* execute move */
	/* helper 可能改变实际持锁 rq，out 必须用返回值恢复调用者上下文。 */
	locked_rq = move_task_between_dsqs(sch, p, enq_flags, src_dsq, dst_dsq);
	dispatched = true;
out:
	if (in_balance) {
		if (this_rq != locked_rq)
			switch_rq_lock(locked_rq, this_rq);
	} else {
		raw_spin_rq_unlock_irqrestore(locked_rq, flags);
	}

	/* 一次性覆盖属于一次 move 尝试，竞争失败也不能泄露到下个 task。 */
	kit->cursor.flags &= ~(__SCX_DSQ_ITER_HAS_SLICE |
			       __SCX_DSQ_ITER_HAS_VTIME);
	return dispatched;
}

__bpf_kfunc_start_defs();

/**
 * scx_bpf_dispatch_nr_slots - Return the number of remaining dispatch slots
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Can only be called from ops.dispatch().
 */
/* 返回当前 CPU dispatch buffer 的剩余槽位；scheduler 已退出时返回 0。 */
/*
 * 业务背景：ops.dispatch 需要知道本轮还能提交多少个派发 verdict，避免超过协商的批量上限。
 * 入参：aux 是 verifier 隐式传入的 BPF 程序元数据，借用且不可空，由它解析所属 scheduler。
 * 出参/返回：返回当前 CPU 缓冲区剩余槽数；实例已退出时返回 0，不修改 cursor。
 * 注意事项：只允许从 ops.dispatch 调用；per-CPU cursor 在该回调上下文内由当前 CPU 独占。
 */
__bpf_kfunc u32 scx_bpf_dispatch_nr_slots(const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return 0;

	return sch->dsp_max_batch - __this_cpu_read(sch->pcpu->dsp_ctx.cursor);
}

/**
 * scx_bpf_dispatch_cancel - Cancel the latest dispatch
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Cancel the latest dispatch. Can be called multiple times to cancel further
 * dispatches. Can only be called from ops.dispatch().
 */
/* 撤销最近一个尚未 flush 的缓冲派发；空缓冲再撤销属于协议错误并触发退出。 */
/*
 * 业务背景：让 ops.dispatch 以栈式方式撤回尚未发布到 DSQ 的最后一个批量 verdict。
 * 入参：aux 为借用的隐式程序元数据，用于取得调用程序所属 scheduler。
 * 出参/返回：无直接返回值；有槽时递减 cursor，空槽时登记 scheduler error。
 * 注意事项：只能在 ops.dispatch 的当前 CPU 上调用；已 flush 的队列状态不可由本函数回滚。
 */
__bpf_kfunc void scx_bpf_dispatch_cancel(const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	struct scx_dsp_ctx *dspc;

	/* 阶段 1：在 RCU 内确认调用实例仍在线，再取得当前 CPU 独占的 dispatch buffer。 */
	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return;

	/* 当前 CPU 的 buffer 由 dispatch 回调独占，无需额外锁保护 cursor。 */
	dspc = &this_cpu_ptr(sch->pcpu)->dsp_ctx;

	if (dspc->cursor > 0)
		dspc->cursor--;
	else
		scx_error(sch, "dispatch buffer underflow");
}

/**
 * scx_bpf_dsq_move_to_local - move a task from a DSQ to the current CPU's local DSQ
 * @dsq_id: DSQ to move task from. Must be a user-created DSQ
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 * @enq_flags: %SCX_ENQ_*
 *
 * Move a task from the non-local DSQ identified by @dsq_id to the current CPU's
 * local DSQ for execution with @enq_flags applied. Can only be called from
 * ops.dispatch().
 *
 * Built-in DSQs (%SCX_DSQ_GLOBAL and %SCX_DSQ_LOCAL*) are not supported as
 * sources. Local DSQs support reenqueueing (a task can be picked up for
 * execution, dequeued for property changes, or reenqueued), but the BPF
 * scheduler cannot directly iterate or move tasks from them. %SCX_DSQ_GLOBAL
 * is similar but also doesn't support reenqueueing, as it maps to multiple
 * per-node DSQs making the scope difficult to define; this may change in the
 * future.
 *
 * This function flushes the in-flight dispatches from scx_bpf_dsq_insert()
 * before trying to move from the specified DSQ. It may also grab rq locks and
 * thus can't be called under any BPF locks.
 *
 * Returns %true if a task has been moved, %false if there isn't any task to
 * move.
 */
/*
 * 先 flush 本轮 insert，再从指定 user DSQ 消费一个 task 到当前 local DSQ。dsq_id/enq_flags
 * 是源队列和入队属性；成功 true 并增加 nr_tasks 促使 balance 重试，空队列/无权限返回 false。
 */
/*
 * 业务背景：ops.dispatch 把一个 user DSQ 的候选搬到当前 CPU local DSQ，形成真正可被 pick 的发布点。
 * 入参：dsq_id 是调用 scheduler 自有 user DSQ；enq_flags 为待应用属性；aux 为借用程序元数据。
 * 出参/返回：成功搬运返回 true，实例无效、队列无效或无任务返回 false；不向 BPF 转移 task 引用。
 * 注意事项：会先 flush 在途 insert 并取得 rq/DSQ 锁，故调用者不得持 BPF 锁；仅限 ops.dispatch。
 */
__bpf_kfunc bool scx_bpf_dsq_move_to_local___v2(u64 dsq_id, u64 enq_flags,
						const struct bpf_prog_aux *aux)
{
	struct scx_dispatch_q *dsq;
	struct scx_sched *sch;
	struct scx_dsp_ctx *dspc;

	/* 阶段 1：在 RCU 内确认调用实例仍在线，并规范只面向 local 目标的入队标志。 */
	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return false;

	if (!scx_vet_enq_flags(sch, SCX_DSQ_LOCAL, &enq_flags))
		return false;

	dspc = &this_cpu_ptr(sch->pcpu)->dsp_ctx;

	/* 先提交此前批量 insert，再从 user DSQ 搬一个 task，维持回调调用顺序。 */
	flush_dispatch_buf(sch, dspc->rq);

	dsq = find_user_dsq(sch, dsq_id);
	/* builtin DSQ 不允许作源；find_user_dsq 只解析当前 scheduler 自有队列。 */
	if (unlikely(!dsq)) {
		scx_error(sch, "invalid DSQ ID 0x%016llx", dsq_id);
		return false;
	}

	/* 阶段 3：在核心锁协议内消费一个队首；成功后通知 balance 即使暂时为空也要重试。 */
	if (consume_dispatch_q(sch, dspc->rq, dsq, enq_flags)) {
		/*
		 * A successfully consumed task can be dequeued before it starts
		 * running while the CPU is trying to migrate other dispatched
		 * tasks. Bump nr_tasks to tell balance_one() to retry on empty
		 * local DSQ.
		 */
		/* 成功消费后 task 仍可在真正运行前被迁走，nr_tasks 强制 balance 见空时重试。 */
		dspc->nr_tasks++;
		return true;
	} else {
		return false;
	}
}

/*
 * COMPAT: ___v2 was introduced in v7.1. Remove this and ___v2 tag in the future.
 */
/* 旧 ABI 使用零 enq_flags 转调 v2，返回语义保持一致。 */
/*
 * 业务背景：兼容旧 BPF 对象，把无属性的 DSQ-to-local 请求适配到 v2 核心实现。
 * 入参：dsq_id 为 user DSQ 标识；aux 为借用程序元数据，二者原样交给 v2。
 * 出参/返回：完整透传 v2 的 true/false，不产生额外副作用或 ownership 变化。
 * 注意事项：仅限 ops.dispatch；该兼容入口未来删除，新的 BPF 程序应直接使用 v2 ABI。
 */
__bpf_kfunc bool scx_bpf_dsq_move_to_local(u64 dsq_id, const struct bpf_prog_aux *aux)
{
	return scx_bpf_dsq_move_to_local___v2(dsq_id, 0, aux);
}

/**
 * scx_bpf_dsq_move_set_slice - Override slice when moving between DSQs
 * @it__iter: DSQ iterator in progress
 * @slice: duration the moved task can run for in nsecs
 *
 * Override the slice of the next task that will be moved from @it__iter using
 * scx_bpf_dsq_move[_vtime](). If this function is not called, the previous
 * slice duration is kept.
 */
/* 为下一次 iterator move 记录一次性 slice 覆盖；真正移动后无论成功失败都会清标志。 */
/*
 * 业务背景：迭代搬运前允许策略覆盖下一个 task 的运行时间片，而不立即改变 DSQ 归属。
 * 入参：it__iter 是 verifier 保证已初始化的借用迭代器；slice 为纳秒时间片，可为 0。
 * 出参/返回：无直接返回值；写入 iterator scratch 并设置 HAS_SLICE，不取得 task 引用。
 * 注意事项：覆盖只消费一次；move 成功或竞争失败都会清除，不能跨 task 复用。
 */
__bpf_kfunc void scx_bpf_dsq_move_set_slice(struct bpf_iter_scx_dsq *it__iter,
					    u64 slice)
{
	struct bpf_iter_scx_dsq_kern *kit = (void *)it__iter;

	kit->slice = slice;
	kit->cursor.flags |= __SCX_DSQ_ITER_HAS_SLICE;
}

/**
 * scx_bpf_dsq_move_set_vtime - Override vtime when moving between DSQs
 * @it__iter: DSQ iterator in progress
 * @vtime: task's ordering inside the vtime-sorted queue of the target DSQ
 *
 * Override the vtime of the next task that will be moved from @it__iter using
 * scx_bpf_dsq_move_vtime(). If this function is not called, the previous slice
 * vtime is kept. If scx_bpf_dsq_move() is used to dispatch the next task, the
 * override is ignored and cleared.
 */
/* 为下一次 PRIQ move 记录一次性 vtime；普通 FIFO move 忽略该值并同样清除。 */
/*
 * 业务背景：为下一个 iterator PRIQ 搬运预置虚拟时间排序键，使更新和入目标队列保持同一事务。
 * 入参：it__iter 为借用的有效迭代器；vtime 是按 time_before64 比较的 64 位排序值。
 * 出参/返回：无直接返回值；只更新 iterator scratch 和 HAS_VTIME 标志，无引用变化。
 * 注意事项：FIFO move 会忽略但仍清除此覆盖；调用者必须在下一次 move 前设置。
 */
__bpf_kfunc void scx_bpf_dsq_move_set_vtime(struct bpf_iter_scx_dsq *it__iter,
					    u64 vtime)
{
	struct bpf_iter_scx_dsq_kern *kit = (void *)it__iter;

	kit->vtime = vtime;
	kit->cursor.flags |= __SCX_DSQ_ITER_HAS_VTIME;
}

/**
 * scx_bpf_dsq_move - Move a task from DSQ iteration to a DSQ
 * @it__iter: DSQ iterator in progress
 * @p: task to transfer
 * @dsq_id: DSQ to move @p to
 * @enq_flags: SCX_ENQ_*
 *
 * Transfer @p which is on the DSQ currently iterated by @it__iter to the DSQ
 * specified by @dsq_id. All DSQs - local DSQs, global DSQ and user DSQs - can
 * be the destination.
 *
 * For the transfer to be successful, @p must still be on the DSQ and have been
 * queued before the DSQ iteration started. This function doesn't care whether
 * @p was obtained from the DSQ iteration. @p just has to be on the DSQ and have
 * been queued before the iteration started.
 *
 * @p's slice is kept by default. Use scx_bpf_dsq_move_set_slice() to update.
 *
 * Can be called from ops.dispatch() or any BPF context which doesn't hold a rq
 * lock (e.g. BPF timers or SYSCALL programs).
 *
 * Returns %true if @p has been consumed, %false if @p had already been
 * consumed, dequeued, or, for sub-scheds, @dsq_id points to a disallowed local
 * DSQ.
 */
/* 仅移动 iterator 当前且仍属源 DSQ 的 task；竞争失去所有权时返回 false。 */
/* 目标可为 builtin/user DSQ；函数不持久保留 p 引用，所有锁与一次性覆盖由核心实现处理。 */
/*
 * 业务背景：把迭代快照中的仍有效 task 原子转移到 FIFO 目标 DSQ，供 dispatch 或无 rq 锁上下文重排。
 * 入参：it__iter/p 均为借用对象；dsq_id 指定目标；enq_flags 指定 FIFO 入队语义。
 * 出参/返回：成功领取并搬运 p 返回 true；已被消费、摘队或目标受限返回 false，无持久引用输出。
 * 注意事项：调用前不能持 rq/BPF 锁；核心会切换 rq 锁、复验游标并在所有出口清一次性覆盖。
 */
__bpf_kfunc bool scx_bpf_dsq_move(struct bpf_iter_scx_dsq *it__iter,
				  struct task_struct *p, u64 dsq_id,
				  u64 enq_flags)
{
	return scx_dsq_move((struct bpf_iter_scx_dsq_kern *)it__iter,
			    p, dsq_id, enq_flags);
}

/**
 * scx_bpf_dsq_move_vtime - Move a task from DSQ iteration to a PRIQ DSQ
 * @it__iter: DSQ iterator in progress
 * @p: task to transfer
 * @dsq_id: DSQ to move @p to
 * @enq_flags: SCX_ENQ_*
 *
 * Transfer @p which is on the DSQ currently iterated by @it__iter to the
 * priority queue of the DSQ specified by @dsq_id. The destination must be a
 * user DSQ as only user DSQs support priority queue.
 *
 * @p's slice and vtime are kept by default. Use scx_bpf_dsq_move_set_slice()
 * and scx_bpf_dsq_move_set_vtime() to update.
 *
 * All other aspects are identical to scx_bpf_dsq_move(). See
 * scx_bpf_dsq_insert_vtime() for more information on @vtime.
 */
/* PRIQ 版本在共用 move 上补 SCX_ENQ_DSQ_PRIQ；目标必须支持 vtime，成功/失败同 FIFO 版。 */
/*
 * 业务背景：复用 iterator 搬运协议，把 task 插入按虚拟时间排序的 user DSQ。
 * 入参：it__iter/p 为借用对象；dsq_id 必须是可用 PRIQ 的 user DSQ；enq_flags 为附加属性。
 * 出参/返回：成功搬运返回 true，竞争失效或目标不合法返回 false；不输出引用。
 * 注意事项：自动追加 PRIQ 位；slice/vtime 覆盖在尝试后清除，builtin DSQ 不能作为 PRIQ 目标。
 */
__bpf_kfunc bool scx_bpf_dsq_move_vtime(struct bpf_iter_scx_dsq *it__iter,
					struct task_struct *p, u64 dsq_id,
					u64 enq_flags)
{
	return scx_dsq_move((struct bpf_iter_scx_dsq_kern *)it__iter,
			    p, dsq_id, enq_flags | SCX_ENQ_DSQ_PRIQ);
}

#ifdef CONFIG_EXT_SUB_SCHED
/**
 * scx_bpf_sub_dispatch - Trigger dispatching on a child scheduler
 * @cgroup_id: cgroup ID of the child scheduler to dispatch
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Allows a parent scheduler to trigger dispatching on one of its direct
 * child schedulers. The child scheduler runs its dispatch operation to
 * move tasks from dispatch queues to the local runqueue.
 *
 * Returns: true on success, false if cgroup_id is invalid, not a direct
 * child, or caller lacks dispatch permission.
 */
/* parent 仅能触发直接 child 的 dispatch；cgroup 无效、越级或无权限返回 false并记录错误。 */
/*
 * 业务背景：父 scheduler 在自己的 dispatch 回调中把选择权下放给一个直接 child，连接分层调度链。
 * 入参：cgroup_id 标识 child；aux 为借用的父程序元数据，用来校验调用主体和拓扑权限。
 * 出参/返回：child 完成一次 dispatch 返回 true；无实例、未找到或非直接 child 返回 false。
 * 注意事项：仅 CONFIG_EXT_SUB_SCHED 且 ops.dispatch 可用；RCU 只保对象寿命，不冻结父子内容。
 */
__bpf_kfunc bool scx_bpf_sub_dispatch(u64 cgroup_id, const struct bpf_prog_aux *aux)
{
	struct rq *this_rq = this_rq();
	struct scx_sched *parent, *child;

	guard(rcu)();
	parent = scx_prog_sched(aux);
	if (unlikely(!parent))
		return false;

	child = scx_find_sub_sched(cgroup_id);

	/* 查找只给出借用对象；随后必须确认它是 parent 的直接 child，禁止越级调度。 */
	if (unlikely(!child))
		return false;

	if (unlikely(scx_parent(child) != parent)) {
		scx_error(parent, "trying to dispatch a distant sub-sched on cgroup %llu",
			  cgroup_id);
		return false;
	}

	/* 权限复验通过后进入 child 的 dispatch；返回值直接表示该层是否产生了可运行候选。 */
	return scx_dispatch_sched(child, this_rq, this_rq->scx.sub_dispatch_prev,
				  true);
}
#endif	/* CONFIG_EXT_SUB_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_EXT_SUB_SCHED 对应配置启用时包含其中实现。 */

__bpf_kfunc_end_defs();

/* BTF 集把上述入口绑定到 dispatch 回调；每个 flags 同时声明隐式参数和 RCU 契约。 */
BTF_KFUNCS_START(scx_kfunc_ids_dispatch)
	/* dispatch 组包括 buffer 管理、DSQ move 和直接 child 派发。 */
BTF_ID_FLAGS(func, scx_bpf_dispatch_nr_slots, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_dispatch_cancel, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_dsq_move_to_local, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_dsq_move_to_local___v2, KF_IMPLICIT_ARGS)
/* scx_bpf_dsq_move*() also in scx_kfunc_ids_unlocked: callable from unlocked contexts */
	/* iterator move 也可从 unlocked 组调用，因此这里的重复登记必须保持相同 KF_RCU flags。 */
BTF_ID_FLAGS(func, scx_bpf_dsq_move_set_slice, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_dsq_move_set_vtime, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_dsq_move, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_dsq_move_vtime, KF_RCU)
#ifdef CONFIG_EXT_SUB_SCHED
BTF_ID_FLAGS(func, scx_bpf_sub_dispatch, KF_IMPLICIT_ARGS)
#endif
BTF_KFUNCS_END(scx_kfunc_ids_dispatch)

static const struct btf_kfunc_id_set scx_kfunc_set_dispatch = {
	/* filter 将整组 API 限定到 ops.dispatch，KF_RCU 标出需保持对象存活的参数。 */
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_dispatch,
	.filter			= scx_kfunc_context_filter,
};

__bpf_kfunc_start_defs();

/**
 * scx_bpf_reenqueue_local - Re-enqueue tasks on a local DSQ
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Iterate over all of the tasks currently enqueued on the local DSQ of the
 * caller's CPU, and re-enqueue them in the BPF scheduler. Returns the number of
 * processed tasks. Can only be called from ops.cpu_release().
 */
/* 在调用 CPU rq 锁内将 local DSQ 全部任务重新交给 BPF enqueue；返回处理数，无 sch 为 0。 */
/*
 * 业务背景：CPU 被调度核心收回时，把 local DSQ 中尚未运行的任务重新交给策略选择去向。
 * 入参：aux 为借用的隐式程序元数据，用于定位 scheduler；无显式 task 输入。
 * 出参/返回：返回本次重新入队的任务数；实例已退出返回 0，任务仍由内核持有。
 * 注意事项：仅 ops.cpu_release 且调用 CPU rq 锁已持有；回调可能再次进入 BPF enqueue。
 */
__bpf_kfunc u32 scx_bpf_reenqueue_local(const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	struct rq *rq;

	guard(rcu)();
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return 0;

	rq = cpu_rq(smp_processor_id());
	/* cpu_release 由调度核心持当前 rq 锁进入，断言后同步执行整队 reenq。 */
	lockdep_assert_rq_held(rq);

	return reenq_local(sch, rq, SCX_REENQ_ANY);
}

__bpf_kfunc_end_defs();

	/* 该能力集仅在 cpu_release op 生效，不能从任意 unlocked 回调借用 rq 锁契约。 */
BTF_KFUNCS_START(scx_kfunc_ids_cpu_release)
BTF_ID_FLAGS(func, scx_bpf_reenqueue_local, KF_IMPLICIT_ARGS)
BTF_KFUNCS_END(scx_kfunc_ids_cpu_release)

static const struct btf_kfunc_id_set scx_kfunc_set_cpu_release = {
	/* cpu_release 组只暴露 rq 锁内 local reenq，filter 再绑定具体 ops 成员。 */
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_cpu_release,
	.filter			= scx_kfunc_context_filter,
};

__bpf_kfunc_start_defs();

/**
 * scx_bpf_create_dsq - Create a custom DSQ
 * @dsq_id: DSQ to create
 * @node: NUMA node to allocate from
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Create a custom DSQ identified by @dsq_id. Can be called from any sleepable
 * scx callback, and any BPF_PROG_TYPE_SYSCALL prog.
 */
/* 创建 user DSQ 并原子加入 scheduler 哈希；保留 id 或重复 id 返回负 errno。 */
/* node 可为有效 NUMA 节点或 NUMA_NO_NODE；初始化在可睡眠上下文，发布失败则完整析构。 */
/*
 * 业务背景：为 BPF 策略建立带自定义 id 的共享派发队列，并在完整初始化后原子发布到 scheduler 哈希。
 * 入参：dsq_id 为非 builtin 64 位 id；node 为 NUMA 节点或 NUMA_NO_NODE；aux 为借用程序元数据。
 * 出参/返回：成功返回 0；参数、重复 id、无实例或分配失败返回负 errno；不向 BPF 输出内核指针。
 * 注意事项：必须在可睡眠 kfunc 上下文；发布失败由本函数逆序 exit_dsq/kfree，成功后归 scheduler 所有。
 */
__bpf_kfunc s32 scx_bpf_create_dsq(u64 dsq_id, s32 node, const struct bpf_prog_aux *aux)
{
	struct scx_dispatch_q *dsq;
	struct scx_sched *sch;
	s32 ret;

	/* 阶段 1：拒绝不存在的 NUMA 节点和 builtin id，避免与内核保留 DSQ 命名空间冲突。 */
	if (unlikely(node >= (int)nr_node_ids ||
		     (node < 0 && node != NUMA_NO_NODE)))
		return -EINVAL;

	if (unlikely(dsq_id & SCX_DSQ_FLAG_BUILTIN))
		return -EINVAL;

	dsq = kmalloc_node(sizeof(*dsq), GFP_KERNEL, node);
	if (!dsq)
		return -ENOMEM;
	/* 发布前先在睡眠上下文完整初始化锁、PRIQ 和 per-CPU deferred 槽。 */

	/*
	 * init_dsq() must be called in GFP_KERNEL context. Init it with NULL
	 * @sch and update afterwards.
	 */
/* 只在 scheduler 仍发布时设置 owner 并原子插哈希；否则按未发布对象析构。 */
	ret = init_dsq(dsq, dsq_id, NULL);
	if (ret) {
		kfree(dsq);
		return ret;
	}

	rcu_read_lock();

	/* 只在 scheduler 仍发布时设置 owner 并原子插哈希；否则按未发布对象析构。 */
	sch = scx_prog_sched(aux);
	if (sch) {
		dsq->sched = sch;
		ret = rhashtable_lookup_insert_fast(&sch->dsq_hash, &dsq->hash_node,
						    dsq_hash_params);
	} else {
		ret = -ENODEV;
	}

	rcu_read_unlock();
	/* 哈希插入失败时没有读者能获得 dsq，可同步 exit 并 kfree。 */
	if (ret) {
		exit_dsq(dsq);
		kfree(dsq);
	}
	return ret;
}

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(scx_kfunc_ids_unlocked)
BTF_ID_FLAGS(func, scx_bpf_create_dsq, KF_IMPLICIT_ARGS | KF_SLEEPABLE)
/* also in scx_kfunc_ids_dispatch: also callable from ops.dispatch() */
	/* move helper 不分配内存，故以 RCU 形态同时向 dispatch 和无 rq 锁上下文开放。 */
BTF_ID_FLAGS(func, scx_bpf_dsq_move_set_slice, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_dsq_move_set_vtime, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_dsq_move, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_dsq_move_vtime, KF_RCU)
/* also in scx_kfunc_ids_select_cpu: also callable from ops.select_cpu()/ops.enqueue() */
/* unlocked 组可用于可睡眠回调/SYSCALL，具体调用点仍由统一 context filter 判定。 */
BTF_ID_FLAGS(func, __scx_bpf_select_cpu_and, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_select_cpu_and, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_select_cpu_dfl, KF_IMPLICIT_ARGS | KF_RCU)
BTF_KFUNCS_END(scx_kfunc_ids_unlocked)

static const struct btf_kfunc_id_set scx_kfunc_set_unlocked = {
	/* unlocked 组可用于可睡眠回调/SYSCALL，具体调用点仍由统一 context filter 判定。 */
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_unlocked,
	.filter			= scx_kfunc_context_filter,
};

__bpf_kfunc_start_defs();

/**
 * scx_bpf_task_set_slice - Set task's time slice
 * @p: task of interest
 * @slice: time slice to set in nsecs
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Set @p's time slice to @slice. Returns %true on success, %false if the
 * calling scheduler doesn't have authority over @p.
 */
/* 仅所属 scheduler 能改 p->scx.slice；成功 true，无实例或越权 false，值在后续调度读取。 */
/*
 * 业务背景：让策略在不搬动 task 的情况下更新下一次运行预算，供 pick/tick 路径消费。
 * 入参：p 为 RCU 窗口内借用 task；slice 为纳秒预算；aux 为借用的调用程序元数据。
 * 出参/返回：有权修改时写 p->scx.slice 并返回 true，否则 false；不取得 task 引用。
 * 注意事项：只校验 scheduler ownership，不负责把已耗尽 task 重新入队，也不重排 DSQ。
 */
__bpf_kfunc bool scx_bpf_task_set_slice(struct task_struct *p, u64 slice,
					const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();
	sch = scx_prog_sched(aux);
	/* 权限检查与写入同处 RCU 区间，scheduler 解绑不能夹在两者之间。 */
	if (unlikely(!sch || !scx_task_on_sched(sch, p)))
		return false;

	p->scx.slice = slice;
	return true;
}

/**
 * scx_bpf_task_set_dsq_vtime - Set task's virtual time for DSQ ordering
 * @p: task of interest
 * @vtime: virtual time to set
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Set @p's virtual time to @vtime. Returns %true on success, %false if the
 * calling scheduler doesn't have authority over @p.
 */
/* 仅所属 scheduler 能更新 PRIQ 排序键；成功 true，越权 false，不负责重新排列已入队节点。 */
/*
 * 业务背景：策略为 task 保存下一次进入 PRIQ 时使用的虚拟时间排序键。
 * 入参：p 是 RCU 保护的借用 task；vtime 为 64 位回绕排序值；aux 标识调用 scheduler。
 * 出参/返回：权限通过则写字段并返回 true，无实例或越权返回 false；无引用转移。
 * 注意事项：对已经挂入 PRIQ 的节点只改字段会破坏顺序，调用者应在合适的队列边界使用。
 */
__bpf_kfunc bool scx_bpf_task_set_dsq_vtime(struct task_struct *p, u64 vtime,
					    const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();
	sch = scx_prog_sched(aux);
	/* vtime 写权限与 slice 相同，只允许 task 当前 owner scheduler。 */
	if (unlikely(!sch || !scx_task_on_sched(sch, p)))
		return false;

	p->scx.dsq_vtime = vtime;
	return true;
}

/*
 * kick 共用实现：关本地 IRQ，把目标位合并到当前 rq 位图并投 irq_work，避免嵌套 rq 锁。
 * PM bypass 时 IRQ 基础设施可能不可用，直接抑制；IDLE 与 PREEMPT/WAIT 组合属协议错误。
 */
/*
 * 业务背景：把可能嵌套 rq 锁的即时 kick 请求转换为当前 CPU irq_work，关闭跨 rq ABBA 窗口。
 * 入参：sch 为 RCU 稳定的借用实例；cpu 为有效物理 CPU；flags 是 SCX_KICK_* 位图。
 * 出参/返回：无直接返回值；合并目标位图并排队 irq_work，PM bypass 时无副作用。
 * 注意事项：本地 IRQ 关闭保护 per-CPU 聚合状态；IDLE 不得与 PREEMPT/WAIT 组合。
 */
static void scx_kick_cpu(struct scx_sched *sch, s32 cpu, u64 flags)
{
	struct rq *this_rq;
	unsigned long irq_flags;

	local_irq_save(irq_flags);

	this_rq = this_rq();

	/*
	 * While bypassing for PM ops, IRQ handling may not be online which can
	 * lead to irq_work_queue() malfunction such as infinite busy wait for
	 * IRQ status update. Suppress kicking.
	 */
	/* PM bypass 期间 IRQ 处理可能未上线，继续投 irq_work 可忙等永不到来的状态更新，故抑制 kick。 */
	if (scx_bypassing(sch, cpu_of(this_rq)))
		goto out;

	/*
	 * Actual kicking is bounced to kick_cpus_irq_workfn() to avoid nesting
	 * rq locks. We can probably be smarter and avoid bouncing if called
	 * from ops which don't hold a rq lock.
	 */
/* idle kick 不含强制抢占/同步等待；组合出现即报告策略错误。 */
	if (flags & SCX_KICK_IDLE) {
		struct rq *target_rq = cpu_rq(cpu);

		/* idle kick 不含强制抢占/同步等待；组合出现即报告策略错误。 */
		if (unlikely(flags & (SCX_KICK_PREEMPT | SCX_KICK_WAIT)))
			scx_error(sch, "PREEMPT/WAIT cannot be used with SCX_KICK_IDLE");

		/* trylock 成功时可精确判断目标是否已必然被唤醒；竞争时保守地保留 idle kick。 */
		if (raw_spin_rq_trylock(target_rq)) {
			if (can_skip_idle_kick(target_rq)) {
				raw_spin_rq_unlock(target_rq);
				goto out;
			}
			raw_spin_rq_unlock(target_rq);
		}
		cpumask_set_cpu(cpu, this_rq->scx.cpus_to_kick_if_idle);
	} else {
		/* 普通请求分别记录 resched、slice 清零和代际等待，irq_work 一次消费。 */
		cpumask_set_cpu(cpu, this_rq->scx.cpus_to_kick);

		if (flags & SCX_KICK_PREEMPT)
			cpumask_set_cpu(cpu, this_rq->scx.cpus_to_preempt);
		if (flags & SCX_KICK_WAIT)
			cpumask_set_cpu(cpu, this_rq->scx.cpus_to_wait);
	}

	irq_work_queue(&this_rq->scx.kick_cpus_irq_work);
out:
	local_irq_restore(irq_flags);
}

/**
 * scx_bpf_kick_cpu - Trigger reschedule on a CPU
 * @cpu: cpu to kick
 * @flags: %SCX_KICK_* flags
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Kick @cpu into rescheduling. This can be used to wake up an idle CPU or
 * trigger rescheduling on a busy CPU. This can be called from any online
 * scx_ops operation and the actual kicking is performed asynchronously through
 * an irq work.
 */
/* 合并 per-CPU kick/preempt/wait 请求，实际 IPI 在延迟路径发送以避免锁反序。 */
/* cpu 先经 scheduler 权限/范围校验；接口无返回，无实例或非法目标不产生请求。 */
/*
 * 业务背景：向 BPF 提供按物理 CPU 请求重调度、抢占或同步等待的安全入口。
 * 入参：cpu 是目标 CPU；flags 为 SCX_KICK_*；aux 为借用的隐式程序元数据。
 * 出参/返回：无直接返回值；合法请求异步发布，实例不存在或 CPU 非法时不改变状态。
 * 注意事项：返回不代表目标已响应；WAIT 的完成由 irq_work 与 balance callback 协议保证。
 */
__bpf_kfunc void scx_bpf_kick_cpu(s32 cpu, u64 flags, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();
	sch = scx_prog_sched(aux);
	if (likely(sch) && scx_cpu_valid(sch, cpu, NULL))
		scx_kick_cpu(sch, cpu, flags);
}

/**
 * scx_bpf_kick_cid - Trigger reschedule on the CPU mapped to @cid
 * @cid: cid to kick
 * @flags: %SCX_KICK_* flags
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * cid-addressed equivalent of scx_bpf_kick_cpu(). Return 0 on success,
 * -errno otherwise.
 */
/* cid 先按当前 mapping 转 CPU；成功排队返回 0，无 scheduler 或无映射返回对应负 errno。 */
/*
 * 业务背景：为使用紧凑 cid 拓扑的策略提供与物理 CPU kick 等价的入口。
 * 入参：cid 是当前映射空间编号；flags 为 SCX_KICK_*；aux 标识调用 scheduler。
 * 出参/返回：请求成功排队返回 0；无实例返回 -ENODEV，映射失败透传负 errno。
 * 注意事项：cid 映射在 RCU 区间解析；成功仍是异步发布，不表示目标 CPU 已完成同步点。
 */
__bpf_kfunc s32 scx_bpf_kick_cid(s32 cid, u64 flags, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	s32 cpu;

	guard(rcu)();
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -ENODEV;
	/* 映射失败 errno 原样返回，调用者可区分离线/越界 cid。 */
	cpu = scx_cid_to_cpu(sch, cid);
	if (cpu < 0)
		return cpu;
	scx_kick_cpu(sch, cpu, flags);
	return 0;
}

/**
 * scx_bpf_dsq_nr_queued - Return the number of queued tasks
 * @dsq_id: id of the DSQ
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Return the number of tasks in the DSQ matching @dsq_id. If not found,
 * -%ENOENT is returned.
 */
/*
 * 读取 local/local-on/user DSQ 的瞬时 nr。禁抢占稳定 this_rq 和 per-CPU 数据；找到返回
 * 非负任务数，无 scheduler 为 -ENODEV，非法/不存在目标统一 -ENOENT。
 */
/*
 * 业务背景：向策略暴露 DSQ 长度快照，用于决定是否补充 dispatch 或选择其它队列。
 * 入参：dsq_id 可表示当前/指定 CPU local DSQ 或调用实例的 user DSQ；aux 标识实例。
 * 出参/返回：返回非负瞬时任务数；实例无效为 -ENODEV，目标不存在或非法为 -ENOENT。
 * 注意事项：禁抢占只稳定 per-CPU 寻址，READ_ONCE 不冻结并发入队/出队，结果仅供启发式决策。
 */
__bpf_kfunc s32 scx_bpf_dsq_nr_queued(u64 dsq_id, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	struct scx_dispatch_q *dsq;
	s32 ret;

	preempt_disable();

	/* 阶段 1：禁抢占后解析 scheduler，使 SCX_DSQ_LOCAL 始终对应本次调用 CPU。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch)) {
		ret = -ENODEV;
		goto out;
	}

	if (dsq_id == SCX_DSQ_LOCAL) {
		/* LOCAL 是调用 CPU 队列；LOCAL_ON 编码需先转回物理 CPU 并校验。 */
		ret = READ_ONCE(this_rq()->scx.local_dsq.nr);
		goto out;
	} else if ((dsq_id & SCX_DSQ_LOCAL_ON) == SCX_DSQ_LOCAL_ON) {
		s32 cpu = scx_cpu_ret(sch, dsq_id & SCX_DSQ_LOCAL_CPU_MASK);

		if (scx_cpu_valid(sch, cpu, NULL)) {
			ret = READ_ONCE(cpu_rq(cpu)->scx.local_dsq.nr);
			goto out;
		}
	} else {
		/* 非 builtin id 只查调用 scheduler 的 user DSQ 哈希。 */
		dsq = find_user_dsq(sch, dsq_id);
		if (dsq) {
			ret = READ_ONCE(dsq->nr);
			goto out;
		}
	}
	ret = -ENOENT;
out:
	/* 所有成功和失败出口都在同一标签恢复抢占，ret 已完整编码查找结果。 */
	preempt_enable();
	return ret;
}

/**
 * scx_bpf_destroy_dsq - Destroy a custom DSQ
 * @dsq_id: DSQ to destroy
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Destroy the custom DSQ identified by @dsq_id. Only DSQs created with
 * scx_bpf_create_dsq() can be destroyed. The caller must ensure that the DSQ is
 * empty and no further tasks are dispatched to it. Ignored if called on a DSQ
 * which doesn't exist. Can be called from any online scx_ops operations.
 */
/* 销毁 user DSQ；不存在时静默。调用者必须保证队列空且不再派发，内核不替其关闭生产者。 */
/*
 * 业务背景：把不再使用的自定义 DSQ 从查找路径摘除，并经 irq_work/RCU 完成延迟回收。
 * 入参：dsq_id 为曾由 create_dsq 建立的 user id；aux 为借用程序元数据。
 * 出参/返回：无直接返回值；命中则摘除并安排释放，不存在或实例退出时无副作用。
 * 注意事项：调用者先保证队列为空且不会再生产；函数不替策略迁移遗留 task。
 */
__bpf_kfunc void scx_bpf_destroy_dsq(u64 dsq_id, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();
	sch = scx_prog_sched(aux);
	if (sch)
		destroy_dsq(sch, dsq_id);
}

/**
 * bpf_iter_scx_dsq_new - Create a DSQ iterator
 * @it: iterator to initialize
 * @dsq_id: DSQ to iterate
 * @flags: %SCX_DSQ_ITER_*
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Initialize BPF iterator @it which can be used with bpf_for_each() to walk
 * tasks in the DSQ specified by @dsq_id. Iteration using @it only includes
 * tasks which are already queued when this function is invoked.
 */
/*
 * 初始化快照边界 DSQ iterator。无论返回值都先清 kit->dsq，保证 verifier 随后无条件调用
 * next/destroy 安全；成功 0，非法 flag/队列/无 scheduler 返回负 errno。
 */
/*
 * 业务背景：为 bpf_for_each 建立“只遍历创建时已在队列中任务”的 DSQ 快照边界。
 * 入参：it 是可写 iterator 存储；dsq_id 是 user DSQ；flags 为允许的迭代位；aux 标识实例。
 * 出参/返回：成功返回 0 并初始化借用 DSQ/cursor；参数或查找失败返回负 errno，kit 仍可销毁。
 * 注意事项：verifier 无论 new 成败都会调用 next/destroy，因此入口先把 kit->dsq 清为 NULL。
 */
__bpf_kfunc int bpf_iter_scx_dsq_new(struct bpf_iter_scx_dsq *it, u64 dsq_id,
				     u64 flags, const struct bpf_prog_aux *aux)
{
	struct bpf_iter_scx_dsq_kern *kit = (void *)it;
	struct scx_sched *sch;

	/* 编译期证明公开 iterator 存储足够大、对齐一致且用户 flags 不侵占内部 cursor 位。 */
	BUILD_BUG_ON(sizeof(struct bpf_iter_scx_dsq_kern) >
		     sizeof(struct bpf_iter_scx_dsq));
	BUILD_BUG_ON(__alignof__(struct bpf_iter_scx_dsq_kern) !=
		     __alignof__(struct bpf_iter_scx_dsq));
	BUILD_BUG_ON(__SCX_DSQ_ITER_ALL_FLAGS &
		     ((1U << __SCX_DSQ_LNODE_PRIV_SHIFT) - 1));

	/*
	 * next() and destroy() will be called regardless of the return value.
	 * Always clear $kit->dsq.
	 */
	/* verifier 的 iterator 协议无条件调用收尾，因此错误对象也必须是可销毁空状态。 */
	kit->dsq = NULL;

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return -ENODEV;

	if (flags & ~__SCX_DSQ_ITER_USER_FLAGS)
		/* 内部 cursor flags 不允许由 BPF 构造。 */
		return -EINVAL;

	kit->dsq = find_user_dsq(sch, dsq_id);
	if (!kit->dsq)
		return -ENOENT;

	kit->cursor = INIT_DSQ_LIST_CURSOR(kit->cursor, kit->dsq, flags);

	return 0;
}

/**
 * bpf_iter_scx_dsq_next - Progress a DSQ iterator
 * @it: iterator to progress
 *
 * Return the next task. See bpf_iter_scx_dsq_new().
 */
/* 持 DSQ 锁推进游标并返回下个 task；初始化失败或遍历结束返回 NULL，指针受 BPF RCU 规则保护。 */
/*
 * 业务背景：实现 bpf_for_each 的逐项读取，把内核 DSQ 游标安全转换成一个瞬时 task 结果。
 * 入参：it 为 new 初始化过的输入输出 iterator，存储由 BPF 栈拥有，函数只在调用期间借用。
 * 出参/返回：返回下个受 RCU 保护的 task 借用指针；初始化失败或遍历结束返回 NULL。
 * 注意事项：持 DSQ irqsave 锁推进 cursor；返回后队列内容仍可变化，修改操作必须重新验证。
 */
__bpf_kfunc struct task_struct *bpf_iter_scx_dsq_next(struct bpf_iter_scx_dsq *it)
{
	struct bpf_iter_scx_dsq_kern *kit = (void *)it;

	if (!kit->dsq)
		return NULL;

	guard(raw_spinlock_irqsave)(&kit->dsq->lock);

	return nldsq_cursor_next_task(&kit->cursor, kit->dsq);
}

/**
 * bpf_iter_scx_dsq_destroy - Destroy a DSQ iterator
 * @it: iterator to destroy
 *
 * Undo scx_iter_scx_dsq_new().
 */
/* 销毁 iterator：若游标仍挂队列则在 DSQ 锁内摘除，最后清 dsq；可安全处理 new 失败对象。 */
/*
 * 业务背景：完成 iterator 生命周期，确保栈上 cursor 不再挂在长期存在的 DSQ 游标链。
 * 入参：it 为输入输出 iterator，可来自成功或失败的 new；函数借用其存储。
 * 出参/返回：无直接返回值；必要时摘除 cursor 并把 kit->dsq 清空，无 task 引用输出。
 * 注意事项：可重复处理空状态；未摘游标就离开 BPF 栈会让 DSQ 链留下悬空节点。
 */
__bpf_kfunc void bpf_iter_scx_dsq_destroy(struct bpf_iter_scx_dsq *it)
{
	struct bpf_iter_scx_dsq_kern *kit = (void *)it;

	if (!kit->dsq)
		return;

	if (!list_empty(&kit->cursor.node)) {
		unsigned long flags;

		/* cursor 可能仍挂在 DSQ list；必须持同一队列锁摘除栈上节点。 */
		raw_spin_lock_irqsave(&kit->dsq->lock, flags);
		list_del_init(&kit->cursor.node);
		raw_spin_unlock_irqrestore(&kit->dsq->lock, flags);
	}
	kit->dsq = NULL;
}

/**
 * scx_bpf_dsq_peek - Lockless peek at the first element.
 * @dsq_id: DSQ to examine.
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Read the first element in the DSQ. This is semantically equivalent to using
 * the DSQ iterator, but is lockfree. Of course, like any lockless operation,
 * this provides only a point-in-time snapshot, and the contents may change
 * by the time any subsequent locking operation reads the queue.
 *
 * Returns the pointer, or NULL indicates an empty queue OR internal error.
 */
/*
 * RCU 无锁读取 user DSQ 的 first_task 快照；builtin/不存在会报错，空队列或错误均 NULL。
 * 返回指针仅在调用者 KF_RCU_PROTECTED 区间有效，后续操作必须重新验证状态。
 */
/*
 * 业务背景：为只需观察队首的策略提供比完整 iterator 更轻的无锁快照。
 * 入参：dsq_id 必须是调用实例的 user DSQ；aux 为借用程序元数据。
 * 出参/返回：队首存在时返回 RCU 借用 task；空队列、无实例或错误均返回 NULL。
 * 注意事项：NULL 混合“空”和“错误”，错误另有日志；返回值不可越过 RCU 窗口或当作队列锁定承诺。
 */
__bpf_kfunc struct task_struct *scx_bpf_dsq_peek(u64 dsq_id,
						 const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	struct scx_dispatch_q *dsq;

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return NULL;

	if (unlikely(dsq_id & SCX_DSQ_FLAG_BUILTIN)) {
		/* builtin/local 队列不提供稳定单一 first_task RCU 指针，明确拒绝。 */
		scx_error(sch, "peek disallowed on builtin DSQ 0x%llx", dsq_id);
		return NULL;
	}

	dsq = find_user_dsq(sch, dsq_id);
	/* 不存在与空队列都返回 NULL，但前者另记错误便于诊断调用 bug。 */
	if (unlikely(!dsq)) {
		scx_error(sch, "peek on non-existent DSQ 0x%llx", dsq_id);
		return NULL;
	}

	return rcu_dereference(dsq->first_task);
}

/**
 * scx_bpf_dsq_reenq - Re-enqueue tasks on a DSQ
 * @dsq_id: DSQ to re-enqueue
 * @reenq_flags: %SCX_RENQ_*
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Iterate over all of the tasks currently enqueued on the DSQ identified by
 * @dsq_id, and re-enqueue them in the BPF scheduler. The following DSQs are
 * supported:
 *
 * - Local DSQs (%SCX_DSQ_LOCAL or %SCX_DSQ_LOCAL_ON | $cpu)
 * - User DSQs
 *
 * Re-enqueues are performed asynchronously. Can be called from anywhere.
 */
/*
 * 为 local/user DSQ 安排异步 reenq；过滤位为空等价 ANY，非法用户标志触发错误。
 * 禁抢占稳定当前 rq，schedule_dsq_reenq 根据当前是否持 rq 锁选择安全延迟路径。
 */
/*
 * 业务背景：请求内核异步扫描 local/user DSQ，并把匹配 task 重新交给 BPF enqueue 重新决策。
 * 入参：dsq_id 指定队列；reenq_flags 是用户可用过滤位，零表示 ANY；aux 标识调用实例。
 * 出参/返回：无直接返回值；只合并并发布 deferred 请求，实际 task 变化在稍后 rq 安全点发生。
 * 注意事项：可从任意支持上下文调用；内部位非法会终止策略，返回不表示扫描已经完成。
 */
__bpf_kfunc void scx_bpf_dsq_reenq(u64 dsq_id, u64 reenq_flags,
				   const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	struct scx_dispatch_q *dsq;

	guard(preempt)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return;

	/* 只接受用户过滤位；内部调度原因位由 kernel consumer 设置。 */
	if (unlikely(reenq_flags & ~__SCX_REENQ_USER_MASK)) {
		scx_error(sch, "invalid SCX_REENQ flags 0x%llx", reenq_flags);
		return;
	}

	/* not specifying any filter bits is the same as %SCX_REENQ_ANY */
/* 零过滤语义是全量，而非“不选任何 task”。 */
	if (!(reenq_flags & __SCX_REENQ_FILTER_MASK))
		/* 零过滤语义是全量，而非“不选任何 task”。 */
		reenq_flags |= SCX_REENQ_ANY;

	dsq = find_dsq_for_dispatch(sch, this_rq(), dsq_id, smp_processor_id());
	schedule_dsq_reenq(sch, dsq, reenq_flags, scx_locked_rq());
}

/**
 * scx_bpf_reenqueue_local - Re-enqueue tasks on a local DSQ
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Iterate over all of the tasks currently enqueued on the local DSQ of the
 * caller's CPU, and re-enqueue them in the BPF scheduler. Can be called from
 * anywhere.
 *
 * This is now a special case of scx_bpf_dsq_reenq() and may be removed in the
 * future.
 */
/* 兼容接口把当前 local DSQ、默认 ANY 过滤转给通用异步 reenq。 */
/*
 * 业务背景：保留旧版“重入队当前 local DSQ”ABI，并把语义集中到通用 DSQ reenq 实现。
 * 入参：aux 为借用的隐式程序元数据，无显式队列或过滤参数。
 * 出参/返回：无直接返回值；异步安排当前 local DSQ 的全量 reenq，无 ownership 转移。
 * 注意事项：兼容入口未来可能删除；其完成时序与 scx_bpf_dsq_reenq 一样不是同步的。
 */
__bpf_kfunc void scx_bpf_reenqueue_local___v2(const struct bpf_prog_aux *aux)
{
	scx_bpf_dsq_reenq(SCX_DSQ_LOCAL, 0, aux);
}

__bpf_kfunc_end_defs();

__printf(5, 0)
/*
 * 安全格式化 BPF bstr：校验参数数组为 8 字节元素且不超上限，nofault 复制到内核 scratch，
 * prepare/printf/cleanup 成对执行。成功返回字符数，失败记录 scheduler 错误并返回负 errno。
 */
/*
 * 业务背景：把 verifier 传入的二进制变参安全转换为退出或 dump 文本，隔离 nofault 读取和格式解析。
 * 入参：sch 为借用实例；data_buf/line_buf 为调用者输出 scratch；line_size 是字节容量；fmt/data 为借用输入。
 * 出参/返回：成功返回写入字符数并填 line_buf；校验、复制或格式化失败返回负 errno 并记录错误。
 * 注意事项：data__sz 必须是 8 字节倍数且不超上限；prepare 成功后所有出口都要 cleanup。
 */
static s32 __bstr_format(struct scx_sched *sch, u64 *data_buf, char *line_buf,
			 size_t line_size, char *fmt, unsigned long long *data,
			 u32 data__sz)
{
	struct bpf_bprintf_data bprintf_data = { .get_bin_args = true };
	s32 ret;

	/* 阶段 1：先验证 verifier 尺寸契约，再 nofault 复制，不能直接解引用 BPF 提供的数组。 */
	if (data__sz % 8 || data__sz > MAX_BPRINTF_VARARGS * 8 ||
	    (data__sz && !data)) {
		scx_error(sch, "invalid data=%p and data__sz=%u", (void *)data, data__sz);
		return -EINVAL;
	}

	ret = copy_from_kernel_nofault(data_buf, data, data__sz);
	if (ret < 0) {
		scx_error(sch, "failed to read data fields (%d)", ret);
		return ret;
	}

	/* prepare 解析格式并把二进制参数转换成受控 bin_args，cleanup 必须成对。 */
	ret = bpf_bprintf_prepare(fmt, UINT_MAX, data_buf, data__sz / 8,
				  &bprintf_data);
	if (ret < 0) {
		scx_error(sch, "format preparation failed (%d)", ret);
		return ret;
	}

	ret = bstr_printf(line_buf, line_size, fmt,
			  bprintf_data.bin_args);
	bpf_bprintf_cleanup(&bprintf_data);
	/* cleanup 在检查格式化结果前执行，失败也不泄露 prepare 临时资源。 */
	if (ret < 0) {
		scx_error(sch, "(\"%s\", %p, %u) failed to format", fmt, data, data__sz);
		return ret;
	}

	return ret;
}

__printf(3, 0)
/* 用固定 scx_bstr_buf 的 data/line 容量调用通用 formatter，返回字符数或负 errno。 */
/*
 * 业务背景：为固定大小 scx_bstr_buf 提供不重复填写容量参数的格式化包装层。
 * 入参：sch/buf 为借用实例与可写 scratch；fmt/data/data__sz 为一次格式请求。
 * 出参/返回：透传字符数或负 errno；成功更新 buf->line/data，无引用变化。
 * 注意事项：调用者必须串行 buf 的使用；本包装不取得全局格式锁，也不延长输入寿命。
 */
static s32 bstr_format(struct scx_sched *sch, struct scx_bstr_buf *buf,
		       char *fmt, unsigned long long *data, u32 data__sz)
{
	return __bstr_format(sch, buf->data, buf->line, sizeof(buf->line),
			     fmt, data, data__sz);
}

__bpf_kfunc_start_defs();

/**
 * scx_bpf_exit_bstr - Gracefully exit the BPF scheduler.
 * @exit_code: Exit value to pass to user space via struct scx_exit_info.
 * @fmt: error message format string
 * @data: format string parameters packaged using ___bpf_fill() macro
 * @data__sz: @data len, must end in '__sz' for the verifier
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Indicate that the BPF scheduler wants to exit gracefully, and initiate ops
 * disabling.
 */
/*
 * BPF 主动正常退出：全局自旋锁串行共享格式缓冲，格式化成功后以 UNREG_BPF claim exit，
 * exit_code/msg 交给 ops.exit；无 scheduler 或格式错误时不重复触发退出。
 */
/*
 * 业务背景：让 BPF 策略带结构化退出码和格式化说明主动结束，进入正常 UNREG_BPF disable 流程。
 * 入参：exit_code 传给用户态；fmt/data/data__sz 描述借用变参；aux 标识调用实例。
 * 出参/返回：无直接返回值；首次成功格式化会登记一次退出，失败或实例消失时不发布新状态。
 * 注意事项：irqsave 自旋锁串行全局 scratch；锁内不可睡眠，真正销毁由后续 worker 完成。
 */
__printf(2, 0)
__bpf_kfunc void scx_bpf_exit_bstr(s64 exit_code, char *fmt,
				   unsigned long long *data, u32 data__sz,
				   const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	unsigned long flags;

	/* exit/error 共用全局格式缓冲，irqsave 锁保证任意回调上下文串行。 */
	raw_spin_lock_irqsave(&scx_exit_bstr_buf_lock, flags);
	sch = scx_prog_sched(aux);
	if (likely(sch) &&
	    bstr_format(sch, &scx_exit_bstr_buf, fmt, data, data__sz) >= 0)
		scx_exit(sch, SCX_EXIT_UNREG_BPF, exit_code, "%s", scx_exit_bstr_buf.line);
	raw_spin_unlock_irqrestore(&scx_exit_bstr_buf_lock, flags);
}

/**
 * scx_bpf_error_bstr - Indicate fatal error
 * @fmt: error message format string
 * @data: format string parameters packaged using ___bpf_fill() macro
 * @data__sz: @data len, must end in '__sz' for the verifier
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Indicate that the BPF scheduler encountered a fatal error and initiate ops
 * disabling.
 */
/* 致命错误版 bstr：同样串行格式化，以 ERROR_BPF 和 code 0 触发 disable。 */
/*
 * 业务背景：BPF 检测到无法继续保证调度正确时，带格式化原因触发 ERROR_BPF 安全退化。
 * 入参：fmt/data/data__sz 为调用期间借用的格式参数；aux 标识出错 scheduler。
 * 出参/返回：无直接返回值；格式成功时登记 fatal exit，格式或实例失败时仅返回。
 * 注意事项：共享 scratch 由 irqsave 锁保护；一次性 exit claim 决定并发错误中谁成为主原因。
 */
__printf(1, 0)
__bpf_kfunc void scx_bpf_error_bstr(char *fmt, unsigned long long *data,
				    u32 data__sz, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	unsigned long flags;

	/* ERROR_BPF 与正常 exit 只差 kind/code，格式化和一次性 claim 协议相同。 */
	raw_spin_lock_irqsave(&scx_exit_bstr_buf_lock, flags);
	sch = scx_prog_sched(aux);
	if (likely(sch) &&
	    bstr_format(sch, &scx_exit_bstr_buf, fmt, data, data__sz) >= 0)
		scx_exit(sch, SCX_EXIT_ERROR_BPF, 0, "%s", scx_exit_bstr_buf.line);
	raw_spin_unlock_irqrestore(&scx_exit_bstr_buf_lock, flags);
}

/**
 * scx_bpf_dump_bstr - Generate extra debug dump specific to the BPF scheduler
 * @fmt: format string
 * @data: format string parameters packaged using ___bpf_fill() macro
 * @data__sz: @data len, must end in '__sz' for the verifier
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * To be called through scx_bpf_dump() helper from ops.dump(), dump_cpu() and
 * dump_task() to generate extra debug dump specific to the BPF scheduler.
 *
 * The extra dump may be multiple lines. A single line may be split over
 * multiple calls. The last line is automatically terminated.
 */
/*
 * dump 回调专用追加器：只允许发起 dump 的 CPU，格式化内容追加到 per-dump 行缓冲；遇换行
 * 或满缓冲时 flush，多次调用可拼一行。参数/上下文错误写入诊断并停止本次追加。
 */
/*
 * 业务背景：允许 ops.dump/dump_cpu/dump_task 分多次生成策略私有诊断文本并接入内核统一 dump。
 * 入参：fmt/data/data__sz 为借用格式参数；aux 标识实例；输出目标来自当前全局 dump 上下文。
 * 出参/返回：无直接返回值；成功推进行 cursor 并按换行/满缓冲 flush，失败写一条诊断。
 * 注意事项：只能由记录在 scx_dump_data.cpu 的 dump 回调调用；dump_lock 在外层串行共享状态。
 */
__printf(1, 0)
__bpf_kfunc void scx_bpf_dump_bstr(char *fmt, unsigned long long *data,
				   u32 data__sz, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	struct scx_dump_data *dd = &scx_dump_data;
	struct scx_bstr_buf *buf = &dd->buf;
	s32 ret;

	/* 阶段 1：稳定 scheduler，并确认当前 CPU 正是外层 dump 所登记的唯一生产者。 */
	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return;

	if (raw_smp_processor_id() != dd->cpu) {
		scx_error(sch, "scx_bpf_dump() must only be called from ops.dump() and friends");
		return;
	}

	/* append the formatted string to the line buf */
/* 失败直接写诊断行，不推进 cursor，后续 dump 仍可继续。 */
	ret = __bstr_format(sch, buf->data, buf->line + dd->cursor,
			    sizeof(buf->line) - dd->cursor, fmt, data, data__sz);
	/* 失败直接写诊断行，不推进 cursor，后续 dump 仍可继续。 */
	if (ret < 0) {
		dump_line(dd->s, "%s[!] (\"%s\", %p, %u) failed to format (%d)",
			  dd->prefix, fmt, data, data__sz, ret);
		return;
	}

	dd->cursor += ret;
	/* formatter 返回可能表示截断，cursor 上限钳到物理缓冲容量。 */
	dd->cursor = min_t(s32, dd->cursor, sizeof(buf->line));

	if (!dd->cursor)
		return;

	/*
	 * If the line buf overflowed or ends in a newline, flush it into the
	 * dump. This is to allow the caller to generate a single line over
	 * multiple calls. As ops_dump_flush() can also handle multiple lines in
	 * the line buf, the only case which can lead to an unexpected
	 * truncation is when the caller keeps generating newlines in the middle
	 * instead of the end consecutively. Don't do that.
	 */
	/* 缓冲溢出或行尾换行时立即 flush；连续调用可拼成一行，但不应在中间反复生成换行。 */
	if (dd->cursor >= sizeof(buf->line) || buf->line[dd->cursor - 1] == '\n')
		ops_dump_flush();
}

/**
 * scx_bpf_cpuperf_cap - Query the maximum relative capacity of a CPU
 * @cpu: CPU of interest
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Return the maximum relative capacity of @cpu in relation to the most
 * performant CPU in the system. The return value is in the range [1,
 * %SCX_CPUPERF_ONE]. See scx_bpf_cpuperf_cur().
 */
/* 返回指定 CPU 相对全机最快 CPU 的最大容量；目标无效时用 ONE 作为保守中性值。 */
/*
 * 业务背景：给异构 CPU 策略提供归一化静态算力上限，供放置决策比较。
 * 入参：cpu 为物理 CPU；aux 是借用程序元数据，用于实例与范围校验。
 * 出参/返回：合法目标返回 [1, SCX_CPUPERF_ONE] 容量；失败返回中性 ONE，无副作用。
 * 注意事项：这是架构容量上限而非当前频率；返回快照不稳定 CPU 在线状态。
 */
__bpf_kfunc u32 scx_bpf_cpuperf_cap(s32 cpu, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	/* 解析调用实例并校验物理 CPU；失败返回中性 ONE，避免把错误解释成零算力。 */
	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (likely(sch) && scx_cpu_valid(sch, cpu, NULL))
		return arch_scale_cpu_capacity(cpu);
	else
		return SCX_CPUPERF_ONE;
}

/**
 * scx_bpf_cidperf_cap - Query the maximum relative capacity of the CPU at @cid
 * @cid: cid of the CPU to query
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * cid-addressed equivalent of scx_bpf_cpuperf_cap().
 */
/* cid 版先解析映射再读容量；无 scheduler/映射时同样返回 ONE。 */
/*
 * 业务背景：为 cid-form 策略提供与 cpuperf_cap 相同的静态容量查询。
 * 入参：cid 为当前 scheduler 映射编号；aux 为借用程序元数据。
 * 出参/返回：映射成功返回目标 CPU 容量，否则返回 ONE；不改变映射或频率状态。
 * 注意事项：RCU 只稳定 scheduler 与映射存储，hotplug 后策略仍需按自身协议重建视图。
 */
__bpf_kfunc u32 scx_bpf_cidperf_cap(s32 cid, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	s32 cpu;

	guard(rcu)();

	sch = scx_prog_sched(aux);
	/* 无调用主体或非法 CPU 时返回中性 ONE，避免把错误误作零容量。 */
	if (unlikely(!sch))
		return SCX_CPUPERF_ONE;
	cpu = scx_cid_to_cpu(sch, cid);
	if (cpu < 0)
		return SCX_CPUPERF_ONE;
	return arch_scale_cpu_capacity(cpu);
}

/**
 * scx_bpf_cpuperf_cur - Query the current relative performance of a CPU
 * @cpu: CPU of interest
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Return the current relative performance of @cpu in relation to its maximum.
 * The return value is in the range [1, %SCX_CPUPERF_ONE].
 *
 * The current performance level of a CPU in relation to the maximum performance
 * available in the system can be calculated as follows:
 *
 *   scx_bpf_cpuperf_cap() * scx_bpf_cpuperf_cur() / %SCX_CPUPERF_ONE
 *
 * The result is in the range [1, %SCX_CPUPERF_ONE].
 */
/* 返回 CPU 相对自身最大频率的当前容量比例；无效目标返回 ONE，调用者可与 cap 相乘归一化。 */
/*
 * 业务背景：向策略提供当前频率相对最大频率的动态比例，用于估算即时可用性能。
 * 入参：cpu 为物理 CPU；aux 为借用程序元数据，用于权限与范围校验。
 * 出参/返回：合法目标返回 [1, ONE] 频率容量，失败返回 ONE；无状态副作用。
 * 注意事项：硬件/驱动可在返回后立即改变频率，该值只能作为瞬时调度信号。
 */
__bpf_kfunc u32 scx_bpf_cpuperf_cur(s32 cpu, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (likely(sch) && scx_cpu_valid(sch, cpu, NULL))
		return arch_scale_freq_capacity(cpu);
	else
		return SCX_CPUPERF_ONE;
}

/**
 * scx_bpf_cidperf_cur - Query the current performance of the CPU at @cid
 * @cid: cid of the CPU to query
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * cid-addressed equivalent of scx_bpf_cpuperf_cur().
 */
/* cid 版解析到 CPU 后读取频率容量；无 scheduler/映射时返回 ONE。 */
/*
 * 业务背景：让 cid-form 策略按紧凑编号查询映射 CPU 的当前频率容量。
 * 入参：cid 为映射编号；aux 标识调用 scheduler，二者均只在当前 RCU 窗口使用。
 * 出参/返回：成功返回 [1, ONE]；实例或映射无效返回 ONE，无副作用。
 * 注意事项：返回值是瞬时快照；需要全机相对性能时还要与 cidperf_cap 相乘归一化。
 */
__bpf_kfunc u32 scx_bpf_cidperf_cur(s32 cid, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	s32 cpu;

	guard(rcu)();

	sch = scx_prog_sched(aux);
	/* cid 查询先解析当前映射，无法解析同样返回中性 ONE。 */
	if (unlikely(!sch))
		return SCX_CPUPERF_ONE;
	cpu = scx_cid_to_cpu(sch, cid);
	if (cpu < 0)
		return SCX_CPUPERF_ONE;
	return arch_scale_freq_capacity(cpu);
}

/**
 * scx_bpf_cpuperf_set - Set the relative performance target of a CPU
 * @cpu: CPU of interest
 * @perf: target performance level [0, %SCX_CPUPERF_ONE]
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Set the target performance level of @cpu to @perf. @perf is in linear
 * relative scale between 0 and %SCX_CPUPERF_ONE. This determines how the
 * schedutil cpufreq governor chooses the target frequency.
 *
 * The actual performance level chosen, CPU grouping, and the overhead and
 * latency of the operations are dependent on the hardware and cpufreq driver in
 * use. Consult hardware and cpufreq documentation for more information. The
 * current performance level can be monitored using scx_bpf_cpuperf_cur().
 */
/*
 * 设置 schedutil 性能目标 [0,ONE]。若调用点已持 rq 锁，只允许操作同一 rq 以免 ABBA；
 * 未持锁则自行 irqsave 获取目标 rq、更新时间并通知 cpufreq。非法 perf/CPU 记录错误。
 */
/*
 * 业务背景：把 BPF 的归一化性能意图发布给 schedutil，使任务放置与频率选择能够协同。
 * 入参：cpu 为目标物理 CPU；perf 为 [0, ONE]；aux 为借用程序元数据。
 * 出参/返回：无直接返回值；合法时更新 rq->scx.cpuperf_target 并通知 cpufreq，错误只记日志。
 * 注意事项：已持 rq 锁时只能操作同一 rq；无锁路径可睡眠否由 rq spinlock 限制且会关 IRQ。
 */
__bpf_kfunc void scx_bpf_cpuperf_set(s32 cpu, u32 perf, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	/* 阶段 1：解析调用实例并门禁 perf 范围；错误值不能进入 rq/cpufreq 状态。 */
	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return;

	if (unlikely(perf > SCX_CPUPERF_ONE)) {
		scx_error(sch, "Invalid cpuperf target %u for CPU %d", perf, cpu);
		return;
	}

	/* 阶段 2：目标有效后选择“复用已持 rq 锁”或“自行锁目标 rq”两条互斥路径。 */
	if (scx_cpu_valid(sch, cpu, NULL)) {
		struct rq *rq = cpu_rq(cpu), *locked_rq = scx_locked_rq();
		struct rq_flags rf;

		/*
		 * When called with an rq lock held, restrict the operation
		 * to the corresponding CPU to prevent ABBA deadlocks.
		 */
		/* 已持锁只能原地更新；跨 rq 再加锁会和调度核心产生 ABBA。 */
		if (locked_rq && rq != locked_rq) {
			scx_error(sch, "Invalid target CPU %d", cpu);
			return;
		}

		/*
		 * If no rq lock is held, allow to operate on any CPU by
		 * acquiring the corresponding rq lock.
		 */
/* 无现成锁时自行锁目标 rq，并刷新时钟后通知 cpufreq。 */
		if (!locked_rq) {
			/* 无现成锁时自行锁目标 rq，并刷新时钟后通知 cpufreq。 */
			rq_lock_irqsave(rq, &rf);
			update_rq_clock(rq);
		}

		rq->scx.cpuperf_target = perf;
		cpufreq_update_util(rq, 0);

		if (!locked_rq)
			rq_unlock_irqrestore(rq, &rf);
	}
}

/**
 * scx_bpf_cidperf_set - Set the performance target of the CPU at @cid
 * @cid: cid of the CPU to target
 * @perf: target performance level [0, %SCX_CPUPERF_ONE]
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * cid-addressed equivalent of scx_bpf_cpuperf_set().
 */
/* cid 版解析映射后复用 CPU 实现；无 scheduler 或无映射时无副作用返回。 */
/*
 * 业务背景：把 cid-form 性能目标适配到物理 CPU 的统一锁和 cpufreq 更新协议。
 * 入参：cid 为当前映射编号；perf 范围 [0, ONE]；aux 为借用程序元数据。
 * 出参/返回：无直接返回值；映射成功时转调 cpuperf_set，否则不改变状态。
 * 注意事项：映射与更新在同一 RCU 窗口，但仍遵守 cpuperf_set 的同 rq 防 ABBA 限制。
 */
__bpf_kfunc void scx_bpf_cidperf_set(s32 cid, u32 perf,
				     const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	s32 cpu;

	guard(rcu)();

	/* cid set 只做映射与权限解析，实际锁约束复用 cpuperf_set。 */
	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return;
	cpu = scx_cid_to_cpu(sch, cid);
	if (cpu < 0)
		return;
	scx_bpf_cpuperf_set(cpu, perf, aux);
}

/**
 * scx_bpf_nr_node_ids - Return the number of possible node IDs
 *
 * All valid node IDs in the system are smaller than the returned value.
 */
/* 返回可能 NUMA node ID 空间上界，不代表区间内每个 node 当前在线。 */
/*
 * 业务背景：让 BPF 按内核 node id 空间安全分配数组并做边界检查。
 * 入参：无。
 * 出参/返回：返回 nr_node_ids 上界；无输出参数、引用或状态副作用。
 * 注意事项：小于上界的 id 仍可能不存在或离线，不能据此直接访问 node 数据。
 */
__bpf_kfunc u32 scx_bpf_nr_node_ids(void)
{
	return nr_node_ids;
}

/**
 * scx_bpf_nr_cpu_ids - Return the number of possible CPU IDs
 *
 * All valid CPU IDs in the system are smaller than the returned value.
 */
/* 返回 possible CPU ID 空间上界，不等同在线 CPU 数。 */
/*
 * 业务背景：向 cpu-form BPF 暴露物理 CPU id 数组所需的稳定边界。
 * 入参：无。
 * 出参/返回：返回 nr_cpu_ids；无副作用和 ownership 变化。
 * 注意事项：合法索引不等于 possible/online，实际使用仍需 cpumask 或校验 helper。
 */
__bpf_kfunc u32 scx_bpf_nr_cpu_ids(void)
{
	return nr_cpu_ids;
}

/**
 * scx_bpf_nr_cids - Return the size of the cid space
 *
 * Equals num_possible_cpus(). All valid cids are in [0, return value).
 */
/* 返回紧凑 cid 空间大小，即 possible CPU 数，所有合法 cid 位于半开区间内。 */
/*
 * 业务背景：让 cid-form 策略为紧凑映射空间分配状态，而不依赖稀疏物理 CPU id。
 * 入参：无。
 * 出参/返回：返回 num_possible_cpus()，合法 cid 范围为 [0, 返回值)；无副作用。
 * 注意事项：该大小不等于当前在线数，cid 到 CPU 的具体对应由映射表决定。
 */
__bpf_kfunc u32 scx_bpf_nr_cids(void)
{
	return num_possible_cpus();
}

/**
 * scx_bpf_nr_online_cids - Return current count of online CPUs in cid space
 *
 * Return num_online_cpus(). The standard model restarts the scheduler on
 * hotplug, which lets schedulers treat [0, nr_online_cids) as the online
 * range. Schedulers that prefer to handle hotplug without a restart should
 * install a custom mapping via scx_bpf_cid_override() and track onlining
 * through the ops.cid_online / ops.cid_offline callbacks.
 */
/* 返回当前在线 cid 数；默认映射配合 hotplug 重启可视为连续区间，自定义映射需自行跟踪。 */
/*
 * 业务背景：给采用默认紧凑映射的策略提供当前在线范围长度。
 * 入参：无。
 * 出参/返回：返回调用瞬间 num_online_cpus()；无状态或引用副作用。
 * 注意事项：自定义映射和免重启 hotplug 模型不能假定 [0,n) 都在线，必须跟踪 cid 回调。
 */
__bpf_kfunc u32 scx_bpf_nr_online_cids(void)
{
	return num_online_cpus();
}

/**
 * scx_bpf_this_cid - Return the cid of the CPU this program is running on
 *
 * cid-addressed equivalent of bpf_get_smp_processor_id() for scx programs.
 * The current cpu is trivially valid, so this is just a table lookup. Return
 * -EINVAL if called from a non-SCX program before any scheduler has ever
 * been enabled (the cid table is still unallocated at that point).
 */
/* 查当前 CPU 的 cid；表尚未因任何 scheduler 启用而分配时返回 -EINVAL。 */
/*
 * 业务背景：为 cid-form 程序提供当前执行 CPU 的紧凑身份，等价于 cid 版 smp_processor_id。
 * 入参：无。
 * 出参/返回：映射表存在时返回当前 CPU cid；从未启用 SCX、表未分配时返回 -EINVAL。
 * 注意事项：调用上下文须保证 CPU 身份稳定；READ_ONCE 只稳定表指针快照。
 */
__bpf_kfunc s32 scx_bpf_this_cid(void)
{
	s16 *tbl = READ_ONCE(scx_cpu_to_cid_tbl);

	if (!tbl)
		return -EINVAL;
	return tbl[raw_smp_processor_id()];
}

/**
 * scx_bpf_get_possible_cpumask - Get a referenced kptr to cpu_possible_mask
 */
/* 返回只读全局 possible mask 的 verifier trusted kptr；配对 put 仅履行类型协议。 */
/*
 * 业务背景：让 BPF 安全读取内核 possible CPU 集合，而无需复制可变长度 cpumask。
 * 入参：无。
 * 出参/返回：返回非空只读全局 mask，verifier 记为 acquire；内核不增加真实引用。
 * 注意事项：调用者必须用 scx_bpf_put_cpumask 配对释放 verifier 令牌，不能修改返回对象。
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_possible_cpumask(void)
{
	return cpu_possible_mask;
}

/**
 * scx_bpf_get_online_cpumask - Get a referenced kptr to cpu_online_mask
 */
/* 返回只读全局 online mask trusted kptr；内容可能随 hotplug 改变。 */
/*
 * 业务背景：为运行时 CPU 选择提供当前 online 集合的 trusted kptr 视图。
 * 入参：无。
 * 出参/返回：返回全局 online mask 并取得 verifier acquire 令牌；无真实引用计数变化。
 * 注意事项：hotplug 可并发改变位图内容；调用者只可读并须以 put_cpumask 配对。
 */
__bpf_kfunc const struct cpumask *scx_bpf_get_online_cpumask(void)
{
	return cpu_online_mask;
}

/**
 * scx_bpf_put_cpumask - Release a possible/online cpumask
 * @cpumask: cpumask to release
 */
/* 释放 verifier 的 acquire 令牌；全局 mask 永久存在，内核无需实际减引用。 */
/*
 * 业务背景：结束 get_possible/get_online 返回 kptr 的 verifier 所有权生命周期。
 * 入参：cpumask 必须是相应 get kfunc 返回的只读借用全局对象，不可空误配。
 * 出参/返回：无直接返回值且函数体无运行时副作用；仅 verifier 将指针标记为已释放。
 * 注意事项：这是类型系统 acquire/release，不是对象引用计数；释放后 BPF 不得继续使用指针。
 */
__bpf_kfunc void scx_bpf_put_cpumask(const struct cpumask *cpumask)
{
	/*
	 * Empty function body because we aren't actually acquiring or releasing
	 * a reference to a global cpumask, which is read-only in the caller and
	 * is never released. The acquire / release semantics here are just used
	 * to make the cpumask is a trusted pointer in the caller.
	 */
	/* 全局 cpumask 只读且永不释放，空函数仅让 verifier 闭合 trusted pointer 的 acquire/release 协议。 */
}

/**
 * scx_bpf_task_running - Is task currently running?
 * @p: task of interest
 */
/* RCU 保护下比较 task 所在 rq 的 curr，返回该 task 此刻是否正在运行。 */
/*
 * 业务背景：让策略判断 task 是否正占用其关联 CPU，辅助诊断和放置启发式。
 * 入参：p 为 KF_RCU 保护的不可空借用 task，不取得长期引用。
 * 出参/返回：比较瞬间为 rq->curr 返回 true，否则 false；无状态副作用。
 * 注意事项：结果可能在返回后立即变化，RCU 保对象寿命但不锁定 curr 或迁移状态。
 */
__bpf_kfunc bool scx_bpf_task_running(const struct task_struct *p)
{
	return task_rq(p)->curr == p;
}

/**
 * scx_bpf_task_cpu - CPU a task is currently associated with
 * @p: task of interest
 */
/* 返回 task 当前关联 CPU 的瞬时快照；调用者持 RCU，但迁移后值可立即过期。 */
/*
 * 业务背景：向策略暴露 task 当前物理 CPU 归属，供统计和候选选择。
 * 入参：p 为 KF_RCU 窗口内不可空的借用 task。
 * 出参/返回：返回调用瞬间 task_cpu(p)；无副作用、无引用转移。
 * 注意事项：不持 rq/pi 锁，迁移可使返回值立即陈旧，不能据此无锁修改目标 rq。
 */
__bpf_kfunc s32 scx_bpf_task_cpu(const struct task_struct *p)
{
	return task_cpu(p);
}

/**
 * scx_bpf_task_cid - cid a task is currently associated with
 * @p: task of interest
 *
 * cid-addressed equivalent of scx_bpf_task_cpu(). task_cpu(p) is always a
 * valid cpu, so this is just a table lookup. Return -EINVAL if called from
 * a non-SCX program before any scheduler has ever been enabled.
 */
/* 将 task_cpu 快照经全局表映射为 cid；表未初始化返回 -EINVAL。 */
/*
 * 业务背景：为 cid-form 策略把 task 的物理 CPU 归属转换为紧凑编号。
 * 入参：p 为 KF_RCU 保护的不可空借用 task。
 * 出参/返回：表存在时返回映射 cid；SCX 从未启用、表未分配时返回 -EINVAL。
 * 注意事项：task 可并发迁移，结果只是瞬时快照；本函数不取得 task 或映射引用。
 */
__bpf_kfunc s32 scx_bpf_task_cid(const struct task_struct *p)
{
	s16 *tbl = READ_ONCE(scx_cpu_to_cid_tbl);

	if (!tbl)
		return -EINVAL;
	return tbl[task_cpu(p)];
}

/**
 * scx_bpf_cpu_rq - Fetch the rq of a CPU
 * @cpu: CPU of the rq
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 */
/* 已弃用的 rq 裸指针查询；校验 scheduler/CPU 后返回并一次性告警，远端 curr 应用安全替代 API。 */
/*
 * 业务背景：兼容旧 BPF 程序按 CPU 获取 rq 的接口，同时引导迁移到受锁或窄用途 API。
 * 入参：cpu 为物理 CPU；aux 为借用程序元数据，用于实例与范围校验。
 * 出参/返回：合法时返回内核长期存在的 rq 借用指针；失败 NULL，并可能打印一次弃用告警。
 * 注意事项：返回 rq 不代表持锁，直接读取可变字段会竞态；新程序应使用 locked_rq/cpu_curr。
 */
__bpf_kfunc struct rq *scx_bpf_cpu_rq(s32 cpu, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return NULL;

	if (!scx_cpu_valid(sch, cpu, NULL))
		return NULL;

	/* 返回裸 rq 仅为兼容；一次性告警提示使用 locked_rq/cpu_curr。 */
	if (!sch->warned_deprecated_rq) {
		printk_deferred(KERN_WARNING "sched_ext: %s() is deprecated; "
				"use scx_bpf_locked_rq() when holding rq lock "
				"or scx_bpf_cpu_curr() to read remote curr safely.\n", __func__);
		sch->warned_deprecated_rq = true;
	}

	return cpu_rq(cpu);
}

/**
 * scx_bpf_locked_rq - Return the rq currently locked by SCX
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Returns the rq if a rq lock is currently held by SCX.
 * Otherwise emits an error and returns NULL.
 */
/* 返回当前 SCX 调用链已经持锁的 rq；无锁访问被视为 scheduler 错误并返回 NULL。 */
/*
 * 业务背景：只在内核已经替当前 SCX 回调持 rq 锁时暴露对应 rq，避免裸指针误用。
 * 入参：aux 为借用程序元数据，用于把缺锁调用归因到具体 scheduler。
 * 出参/返回：持锁时返回该 rq 借用指针；否则记录错误并返回 NULL，无引用变化。
 * 注意事项：指针有效和字段稳定性只持续到外层释放 rq 锁；guard(preempt) 稳定当前调用上下文。
 */
__bpf_kfunc struct rq *scx_bpf_locked_rq(const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	struct rq *rq;

	guard(preempt)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return NULL;

	rq = scx_locked_rq();
	if (!rq) {
		/* 缺锁不是普通 NULL 查询，而是可能 UAF/竞态的 BPF 协议错误。 */
		scx_error(sch, "accessing rq without holding rq lock");
		return NULL;
	}

	return rq;
}

/**
 * scx_bpf_cpu_curr - Return remote CPU's curr task
 * @cpu: CPU of interest
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * Callers must hold RCU read lock (KF_RCU).
 */
/* 校验目标后 RCU 读取远端 rq->curr；成功返回受 RCU 保护 task，失败 NULL。 */
/*
 * 业务背景：提供无需持远端 rq 锁的窄接口，只读取目标 CPU 当前 task 并保证其内存寿命。
 * 入参：cpu 为物理 CPU；aux 为借用程序元数据，调用者由 KF_RCU_PROTECTED 保持读侧窗口。
 * 出参/返回：成功返回 RCU 借用 curr task；无实例或非法 CPU 返回 NULL，不增加 task 引用。
 * 注意事项：RCU 不冻结 curr 字段，返回 task 可能已不再运行；不可越过读侧窗口保存。
 */
__bpf_kfunc struct task_struct *scx_bpf_cpu_curr(s32 cpu, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;

	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		return NULL;

	if (!scx_cpu_valid(sch, cpu, NULL))
		return NULL;

	/* curr 由 rq 切换路径以 RCU 语义发布，返回指针限调用者保护区间。 */
	return rcu_dereference(cpu_rq(cpu)->curr);
}

/**
 * scx_bpf_cid_curr - Return the curr task on the CPU at @cid
 * @cid: cid of interest
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * cid-addressed equivalent of scx_bpf_cpu_curr(). Callers must hold RCU
 * read lock (KF_RCU).
 */
/* cid 版先解析 CPU，再 RCU 读取 curr；无实例/映射返回 NULL。 */
/*
 * 业务背景：为 cid-form 策略提供远端当前 task 的窄读取接口，避免暴露无锁 rq。
 * 入参：cid 为当前映射编号；aux 为借用程序元数据，调用者保持 KF_RCU_PROTECTED 窗口。
 * 出参/返回：映射成功返回 RCU 借用 curr task；无实例或无映射返回 NULL，无引用增加。
 * 注意事项：返回仅稳定对象存储期，不保证 task 仍在该 CPU 运行，不能跨 RCU 窗口保留。
 */
__bpf_kfunc struct task_struct *scx_bpf_cid_curr(s32 cid, const struct bpf_prog_aux *aux)
{
	struct scx_sched *sch;
	s32 cpu;

	guard(rcu)();

	sch = scx_prog_sched(aux);
	/* 无 scheduler 或 cid 映射失败均无可信远端 curr。 */
	if (unlikely(!sch))
		return NULL;
	cpu = scx_cid_to_cpu(sch, cid);
	if (cpu < 0)
		return NULL;
	return rcu_dereference(cpu_rq(cpu)->curr);
}

/**
 * scx_bpf_tid_to_task - Look up a task by its scx tid
 * @tid: task ID previously read from p->scx.tid
 *
 * Returns the task with the given tid, or NULL if no such task exists. The
 * returned pointer is valid until the end of the current RCU read section
 * (KF_RCU_PROTECTED). Requires SCX_OPS_TID_TO_TASK to be set on the root
 * scheduler; otherwise an error is raised and NULL returned.
 */
/* RCU 哈希查询 tid；返回 trusted 引用的可用范围由 KF_ACQUIRE/释放协议约束。 */
/* root 未开启 TID_TO_TASK 时记录错误并 NULL；命中返回嵌入 scx 的 task，生命周期限当前 RCU 区间。 */
/*
 * 业务背景：把不复用的 SCX tid 反查为 task，使 BPF 可在 PID 已丢失等阶段稳定标识任务。
 * 入参：tid 为先前从 p->scx.tid 读取的 64 位值；无显式 scheduler 参数。
 * 出参/返回：能力启用且命中时返回 RCU 保护的借用 task，否则 NULL；不增加 task 引用。
 * 注意事项：root 必须协商 TID_TO_TASK；哈希节点寿命只由当前 RCU 窗口保证，不能长期保存。
 */
__bpf_kfunc struct task_struct *scx_bpf_tid_to_task(u64 tid)
{
	struct sched_ext_entity *scx;

	if (!scx_tid_to_task_enabled()) {
		struct scx_sched *sch = rcu_dereference(scx_root);

		if (sch)
			scx_error(sch, "scx_bpf_tid_to_task() called without SCX_OPS_TID_TO_TASK");
		/* 能力未协商时不触碰可能尚未初始化或已销毁的哈希。 */
		return NULL;
	}

	scx = rhashtable_lookup(&scx_tid_hash, &tid, scx_tid_hash_params);
	if (!scx)
		return NULL;

	/* 节点嵌入 task->scx，container_of 不增引用，生命周期由 RCU 区间保证。 */
	return container_of(scx, struct task_struct, scx);
}

/**
 * scx_bpf_now - Returns a high-performance monotonically non-decreasing
 * clock for the current CPU. The clock returned is in nanoseconds.
 *
 * It provides the following properties:
 *
 * 1) High performance: Many BPF schedulers call bpf_ktime_get_ns() frequently
 *  to account for execution time and track tasks' runtime properties.
 *  Unfortunately, in some hardware platforms, bpf_ktime_get_ns() -- which
 *  eventually reads a hardware timestamp counter -- is neither performant nor
 *  scalable. scx_bpf_now() aims to provide a high-performance clock by
 *  using the rq clock in the scheduler core whenever possible.
 *
 * 2) High enough resolution for the BPF scheduler use cases: In most BPF
 *  scheduler use cases, the required clock resolution is lower than the most
 *  accurate hardware clock (e.g., rdtsc in x86). scx_bpf_now() basically
 *  uses the rq clock in the scheduler core whenever it is valid. It considers
 *  that the rq clock is valid from the time the rq clock is updated
 *  (update_rq_clock) until the rq is unlocked (rq_unpin_lock).
 *
 * 3) Monotonically non-decreasing clock for the same CPU: scx_bpf_now()
 *  guarantees the clock never goes backward when comparing them in the same
 *  CPU. On the other hand, when comparing clocks in different CPUs, there
 *  is no such guarantee -- the clock can go backward. It provides a
 *  monotonically *non-decreasing* clock so that it would provide the same
 *  clock values in two different scx_bpf_now() calls in the same CPU
 *  during the same period of when the rq clock is valid.
 */
/* 优先返回当前 locked rq 的稳定时钟，否则使用全局调度时钟；不建立时间冻结保证。 */
/* 禁抢占保证同 CPU 单调不减；跨 CPU 不承诺顺序，rq 锁外每次取新 sched_clock。 */
/*
 * 业务背景：为高频 BPF 记账提供比硬件时钟更便宜、同 CPU 单调不减的纳秒时间源。
 * 入参：无。
 * 出参/返回：返回当前 CPU 纳秒时钟；无输出参数、引用或调度状态副作用。
 * 注意事项：同一 rq 有效窗可重复同值，跨 CPU 不保证顺序；禁抢占避免一次调用中切换 CPU。
 */
__bpf_kfunc u64 scx_bpf_now(void)
{
	struct rq *rq;
	u64 clock;

	preempt_disable();

	rq = this_rq();
	if (smp_load_acquire(&rq->scx.flags) & SCX_RQ_CLK_VALID) {
		/*
		 * If the rq clock is valid, use the cached rq clock.
		 *
		 * Note that scx_bpf_now() is re-entrant between a process
		 * context and an interrupt context (e.g., timer interrupt).
		 * However, we don't need to consider the race between them
		 * because such race is not observable from a caller.
		 */
		/* acquire 观察 VALID 后，配对发布保证缓存 clock 已完整写入。 */
		clock = READ_ONCE(rq->scx.clock);
	} else {
		/*
		 * Otherwise, return a fresh rq clock.
		 *
		 * The rq clock is updated outside of the rq lock.
		 * In this case, keep the updated rq clock invalid so the next
		 * kfunc call outside the rq lock gets a fresh rq clock.
		 */
		/* 锁外读不设置 VALID，下一次调用仍会取得新时钟。 */
		clock = sched_clock_cpu(cpu_of(rq));
	}

	preempt_enable();

	return clock;
}

/* 清零输出并汇总 sch 所有 possible CPU 的事件计数；调用者负责稳定 sch 生命周期。 */
/*
 * 业务背景：把每 CPU 热路径计数合成为一个实例级快照，避免事件更新争用全局 cacheline。
 * 入参：sch 为生命周期稳定的借用实例；events 为不可空可写输出结构。
 * 出参/返回：无直接返回值；先清零再逐 CPU 累加全部事件字段，不转移内存 ownership。
 * 注意事项：无全局冻结，快照可混合并发时刻；调用者负责 RCU/控制锁稳定 sch 与 pcpu。
 */
static void scx_read_events(struct scx_sched *sch, struct scx_event_stats *events)
{
	struct scx_event_stats *e_cpu;
	int cpu;

	/* Aggregate per-CPU event counters into @events. */
/* 调度选择、派发和 enqueue 类计数按字段累加，保持 ABI 字段语义独立。 */
	memset(events, 0, sizeof(*events));
	for_each_possible_cpu(cpu) {
		e_cpu = &per_cpu_ptr(sch->pcpu, cpu)->event_stats;
		/* 调度选择、派发和 enqueue 类计数按字段累加，保持 ABI 字段语义独立。 */
		scx_agg_event(events, e_cpu, SCX_EV_SELECT_CPU_FALLBACK);
		scx_agg_event(events, e_cpu, SCX_EV_DISPATCH_LOCAL_DSQ_OFFLINE);
		scx_agg_event(events, e_cpu, SCX_EV_DISPATCH_KEEP_LAST);
		scx_agg_event(events, e_cpu, SCX_EV_ENQ_SKIP_EXITING);
		scx_agg_event(events, e_cpu, SCX_EV_ENQ_SKIP_MIGRATION_DISABLED);
		scx_agg_event(events, e_cpu, SCX_EV_REENQ_IMMED);
		scx_agg_event(events, e_cpu, SCX_EV_REENQ_LOCAL_REPEAT);
		scx_agg_event(events, e_cpu, SCX_EV_REFILL_SLICE_DFL);
		/* bypass 与所有权异常单独聚合，便于区分安全降级和 BPF 协议错误。 */
		scx_agg_event(events, e_cpu, SCX_EV_BYPASS_DURATION);
		scx_agg_event(events, e_cpu, SCX_EV_BYPASS_DISPATCH);
		scx_agg_event(events, e_cpu, SCX_EV_BYPASS_ACTIVATE);
		scx_agg_event(events, e_cpu, SCX_EV_INSERT_NOT_OWNED);
		scx_agg_event(events, e_cpu, SCX_EV_SUB_BYPASS_DISPATCH);
	}
}

/*
 * scx_bpf_events - Get a system-wide event counter to
 * @events: output buffer from a BPF program
 * @events__sz: @events len, must end in '__sz'' for the verifier
 */
/*
 * 获取 root 的全机事件快照；无 root 返回全零。events 是 BPF 输出缓冲，复制长度取用户
 * 编译尺寸与当前内核结构体较小者，兼容新旧 vmlinux.h 并避免越界。
 */
/*
 * 业务背景：向 BPF 输出 root scheduler 的全机事件快照，并保持不同 vmlinux.h 结构尺寸兼容。
 * 入参：events 是 BPF 提供的可写输出缓冲；events__sz 是 verifier 关联的字节长度。
 * 出参/返回：无直接返回值；复制 min(调用者大小, 内核大小)，无 root 时输出相同范围的零。
 * 注意事项：先在 RCU 内聚合到栈快照再复制；尺寸钳制是防止新旧 ABI 造成越界写的边界。
 */
__bpf_kfunc void scx_bpf_events(struct scx_event_stats *events,
				size_t events__sz)
{
	struct scx_sched *sch;
	struct scx_event_stats e_sys;

	/* 阶段 1：在 RCU 内把 root 的 per-CPU 计数聚合到栈快照；无 root 则建立全零快照。 */
	rcu_read_lock();
	sch = rcu_dereference(scx_root);
	if (sch)
		scx_read_events(sch, &e_sys);
	else
		memset(&e_sys, 0, sizeof(e_sys));
	rcu_read_unlock();

	/* 新旧 vmlinux.h 结构尺寸可能不同，复制双方可用范围的交集。 */
	/*
	 * We cannot entirely trust a BPF-provided size since a BPF program
	 * might be compiled against a different vmlinux.h, of which
	 * scx_event_stats would be larger (a newer vmlinux.h) or smaller
	 * (an older vmlinux.h). Hence, we use the smaller size to avoid
	 * memory corruption.
	 */
	/* BPF 可按不同 vmlinux.h 编译，因此只复制内核与调用者结构大小的较小值，避免越界。 */
	events__sz = min(events__sz, sizeof(*events));
	memcpy(events, &e_sys, events__sz);
}

#ifdef CONFIG_CGROUP_SCHED
/**
 * scx_bpf_task_cgroup - Return the sched cgroup of a task
 * @p: task of interest
 * @aux: implicit BPF argument to access bpf_prog_aux hidden from BPF progs
 *
 * @p->sched_task_group->css.cgroup represents the cgroup @p is associated with
 * from the scheduler's POV. SCX operations should use this function to
 * determine @p's current cgroup as, unlike following @p->cgroups,
 * @p->sched_task_group is stable for the duration of the SCX op. See
 * SCX_CALL_OP_TASK() for details.
 */
/*
 * 返回 scheduler 视角下稳定的 task cgroup 并取得引用。无 scheduler/越权时返回已引用
 * default root；成功返回 tg 对应组。调用者必须按 KF_ACQUIRE 协议释放。
 */
/*
 * 业务背景：在 SCX op 的稳定调度分组视图中取得 task cgroup，避免跟随可能并发改变的 p->cgroups。
 * 入参：p 为 RCU 保护的借用 task；aux 为借用程序元数据，用于 scheduler ownership 校验。
 * 出参/返回：返回带真实引用的 task cgroup；无实例或越权时返回带引用的默认 root cgroup。
 * 注意事项：KF_ACQUIRE 要求 BPF 配对 put；goto out 的所有路径都执行 cgroup_get，不能漏放引用。
 */
__bpf_kfunc struct cgroup *scx_bpf_task_cgroup(struct task_struct *p,
					       const struct bpf_prog_aux *aux)
{
	struct task_group *tg = p->sched_task_group;
	struct cgroup *cgrp = &cgrp_dfl_root.cgrp;
	struct scx_sched *sch;

	/* 默认先指向 root，使无实例和权限失败路径也能统一取得一个有效 cgroup 引用。 */
	guard(rcu)();

	sch = scx_prog_sched(aux);
	if (unlikely(!sch))
		goto out;

	if (!scx_kf_arg_task_ok(sch, p))
		goto out;

	/* 权限通过后 sched_task_group 在当前 SCX op 期间保持稳定。 */
	cgrp = tg_cgrp(tg);

out:
	/* 成功或 root fallback 都取得真实引用，与 KF_ACQUIRE 规则配对。 */
	cgroup_get(cgrp);
	return cgrp;
}
#endif	/* CONFIG_CGROUP_SCHED */
/* 以上条件编译分支到此结束；仅 CONFIG_CGROUP_SCHED 对应配置启用时包含其中实现。 */

__bpf_kfunc_end_defs();

BTF_KFUNCS_START(scx_kfunc_ids_any)
	/* any 组不依赖具体 ops；每项 flags 同时编码 RCU、可空和 acquire/release 所有权。 */
BTF_ID_FLAGS(func, scx_bpf_task_set_slice, KF_IMPLICIT_ARGS | KF_RCU);
BTF_ID_FLAGS(func, scx_bpf_task_set_dsq_vtime, KF_IMPLICIT_ARGS | KF_RCU);
BTF_ID_FLAGS(func, scx_bpf_kick_cpu, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_kick_cid, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_dsq_nr_queued, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_destroy_dsq, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_dsq_peek, KF_IMPLICIT_ARGS | KF_RCU_PROTECTED | KF_RET_NULL)
BTF_ID_FLAGS(func, scx_bpf_dsq_reenq, KF_IMPLICIT_ARGS)
	/* iterator 三件套的 NEW/NEXT/DESTROY 标志让 verifier 跟踪资源生命周期。 */
BTF_ID_FLAGS(func, scx_bpf_reenqueue_local___v2, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, bpf_iter_scx_dsq_new, KF_IMPLICIT_ARGS | KF_ITER_NEW | KF_RCU_PROTECTED)
BTF_ID_FLAGS(func, bpf_iter_scx_dsq_next, KF_ITER_NEXT | KF_RET_NULL)
BTF_ID_FLAGS(func, bpf_iter_scx_dsq_destroy, KF_ITER_DESTROY)
BTF_ID_FLAGS(func, scx_bpf_exit_bstr, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_error_bstr, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_dump_bstr, KF_IMPLICIT_ARGS)
	/* 性能、拓扑与 task 查询均可在在线 SCX 回调中调用。 */
BTF_ID_FLAGS(func, scx_bpf_cpuperf_cap, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cpuperf_cur, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cpuperf_set, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cidperf_cap, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cidperf_cur, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cidperf_set, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_nr_node_ids)
	/* topology 数值无引用；cpumask 获取/释放在后续条目明确 acquire/release。 */
BTF_ID_FLAGS(func, scx_bpf_nr_cpu_ids)
BTF_ID_FLAGS(func, scx_bpf_nr_cids)
BTF_ID_FLAGS(func, scx_bpf_nr_online_cids)
BTF_ID_FLAGS(func, scx_bpf_this_cid)
BTF_ID_FLAGS(func, scx_bpf_get_possible_cpumask, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_online_cpumask, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_put_cpumask, KF_RELEASE)
	/* task/rq 指针类接口显式标注其 RCU 保护和 NULL 返回边界。 */
BTF_ID_FLAGS(func, scx_bpf_task_running, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_task_cpu, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_task_cid, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_cpu_rq, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_locked_rq, KF_IMPLICIT_ARGS | KF_RET_NULL)
BTF_ID_FLAGS(func, scx_bpf_cpu_curr, KF_IMPLICIT_ARGS | KF_RET_NULL | KF_RCU_PROTECTED)
BTF_ID_FLAGS(func, scx_bpf_cid_curr, KF_IMPLICIT_ARGS | KF_RET_NULL | KF_RCU_PROTECTED)
	/* tid、时钟、事件与 cgroup 构成不依赖具体 op 的观察接口，但各自保留 RCU/acquire 限制。 */
BTF_ID_FLAGS(func, scx_bpf_tid_to_task, KF_RET_NULL | KF_RCU_PROTECTED)
BTF_ID_FLAGS(func, scx_bpf_now)
BTF_ID_FLAGS(func, scx_bpf_events)
#ifdef CONFIG_CGROUP_SCHED
BTF_ID_FLAGS(func, scx_bpf_task_cgroup, KF_IMPLICIT_ARGS | KF_RCU | KF_ACQUIRE)
#endif
BTF_KFUNCS_END(scx_kfunc_ids_any)

static const struct btf_kfunc_id_set scx_kfunc_set_any = {
	/* 所有 any kfunc 仍交给统一 filter，后者负责 cpu/cid 形态而非 op 专属能力。 */
	.owner			= THIS_MODULE,
	.set			= &scx_kfunc_ids_any,
	.filter			= scx_kfunc_context_filter,
};

/* any 仍经过 filter，不能借该分组绕过 cpu/cid 形态限制。 */
/*
 * cpu-form kfuncs that are forbidden from cid-form schedulers
 * (bpf_sched_ext_ops_cid). Programs targeting the cid struct_ops type must
 * use the cid-form alternative (cid/cmask kfuncs).
 *
 * Membership overlaps with scx_kfunc_ids_{any,idle,select_cpu}; the filter
 * tests this set independently and rejects matches before the per-op
 * allow-list check runs.
 *
 * pahole/resolve_btfids scans every BTF_ID_FLAGS() at build time and
 * intersects flags across duplicate entries, so each entry must carry the
 * same flags as the kfunc's primary declaration; otherwise the flags get
 * dropped globally.
 */
/* cid-form 在验证期拒绝本集合，防止同为 s32 的 CPU 与 cid 被静默混用。 */
BTF_KFUNCS_START(scx_kfunc_ids_cpu_only)
	/* cid-form 在验证期拒绝本集合，防止同为 s32 的 CPU 与 cid 被静默混用。 */
BTF_ID_FLAGS(func, scx_bpf_kick_cpu, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_task_cpu, KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_cpu_rq, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cpu_curr, KF_IMPLICIT_ARGS | KF_RET_NULL | KF_RCU_PROTECTED)
BTF_ID_FLAGS(func, scx_bpf_cpu_node, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cpuperf_cap, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cpuperf_cur, KF_IMPLICIT_ARGS)
BTF_ID_FLAGS(func, scx_bpf_cpuperf_set, KF_IMPLICIT_ARGS)
	/* 物理 cpumask、select 与 idle API 同样属于 cpu-only，cid-form 验证时整体拒绝。 */
	/* cpumask 与 select/idle 系列也直接使用物理 CPU 编号，必须归入 cpu-only。 */
BTF_ID_FLAGS(func, scx_bpf_get_possible_cpumask, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_online_cpumask, KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_put_cpumask, KF_RELEASE)
BTF_ID_FLAGS(func, scx_bpf_select_cpu_dfl, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, __scx_bpf_select_cpu_and, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_select_cpu_and, KF_RCU)
	/* idle mask 和 pick helper 都接收物理 CPU/cpumask，cid-form 必须改用 cid/cmask 对应接口。 */
BTF_ID_FLAGS(func, scx_bpf_get_idle_cpumask, KF_IMPLICIT_ARGS | KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_cpumask_node, KF_IMPLICIT_ARGS | KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_smtmask, KF_IMPLICIT_ARGS | KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_get_idle_smtmask_node, KF_IMPLICIT_ARGS | KF_ACQUIRE)
BTF_ID_FLAGS(func, scx_bpf_put_idle_cpumask, KF_RELEASE)
BTF_ID_FLAGS(func, scx_bpf_test_and_clear_cpu_idle, KF_IMPLICIT_ARGS)
	/* pick 系列返回候选编号而非对象引用，但仍需 RCU 稳定 scheduler 的拓扑视图。 */
BTF_ID_FLAGS(func, scx_bpf_pick_idle_cpu, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_pick_idle_cpu_node, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_pick_any_cpu, KF_IMPLICIT_ARGS | KF_RCU)
BTF_ID_FLAGS(func, scx_bpf_pick_any_cpu_node, KF_IMPLICIT_ARGS | KF_RCU)
BTF_KFUNCS_END(scx_kfunc_ids_cpu_only)

/*
 * Per-op kfunc allow flags. Each bit corresponds to a context-sensitive kfunc
 * group; an op may permit zero or more groups, with the union expressed in
 * scx_kf_allow_flags[]. The verifier-time filter (scx_kfunc_context_filter())
 * consults this table to decide whether a context-sensitive kfunc is callable
 * from a given SCX op.
 */
/* 能力位只服务 verifier allow-list；未列出的 op 默认只能调用 any/idle 集。 */
enum scx_kf_allow_flags {
	SCX_KF_ALLOW_UNLOCKED		= 1 << 0,
	SCX_KF_ALLOW_INIT		= 1 << 1,
	SCX_KF_ALLOW_CPU_RELEASE	= 1 << 2,
	SCX_KF_ALLOW_DISPATCH		= 1 << 3,
	SCX_KF_ALLOW_ENQUEUE		= 1 << 4,
	SCX_KF_ALLOW_SELECT_CPU		= 1 << 5,
};

/*
 * Map each SCX op to the union of kfunc groups it permits, indexed by
 * SCX_OP_IDX(op). Ops not listed only permit kfuncs that are not
 * context-sensitive.
 */
/* select/enqueue 可直接派发，dispatch 可派发并移动，release 只开放本地 reenq。 */
static const u32 scx_kf_allow_flags[] = {
	/* select/enqueue 可直接派发，dispatch 可派发并移动，release 只开放本地 reenq。 */
	[SCX_OP_IDX(select_cpu)]	= SCX_KF_ALLOW_SELECT_CPU | SCX_KF_ALLOW_ENQUEUE,
	[SCX_OP_IDX(enqueue)]		= SCX_KF_ALLOW_SELECT_CPU | SCX_KF_ALLOW_ENQUEUE,
	[SCX_OP_IDX(dispatch)]		= SCX_KF_ALLOW_ENQUEUE | SCX_KF_ALLOW_DISPATCH,
	[SCX_OP_IDX(cpu_release)]	= SCX_KF_ALLOW_CPU_RELEASE,
	[SCX_OP_IDX(init_task)]		= SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(dump)]		= SCX_KF_ALLOW_UNLOCKED,
#ifdef CONFIG_EXT_GROUP_SCHED
	/* 可睡眠 cgroup 生命周期回调只需要 unlocked 组，不获得 dispatch 权限。 */
	[SCX_OP_IDX(cgroup_init)]	= SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(cgroup_exit)]	= SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(cgroup_prep_move)]	= SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(cgroup_cancel_move)] = SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(cgroup_set_weight)]	= SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(cgroup_set_bandwidth)] = SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(cgroup_set_idle)]	= SCX_KF_ALLOW_UNLOCKED,
#endif	/* CONFIG_EXT_GROUP_SCHED */
	/* sub/hotplug/lifecycle 回调同属 unlocked；只有 init 额外获得 init 专用 API。 */
	[SCX_OP_IDX(sub_attach)]	= SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(sub_detach)]	= SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(cpu_online)]	= SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(cpu_offline)]	= SCX_KF_ALLOW_UNLOCKED,
	[SCX_OP_IDX(init)]		= SCX_KF_ALLOW_UNLOCKED | SCX_KF_ALLOW_INIT,
	[SCX_OP_IDX(exit)]		= SCX_KF_ALLOW_UNLOCKED,
};

/*
 * Verifier-time filter for SCX kfuncs. Registered via the .filter field on
 * each per-group btf_kfunc_id_set. The BPF core invokes this for every kfunc
 * call in the registered hook (BPF_PROG_TYPE_STRUCT_OPS or
 * BPF_PROG_TYPE_SYSCALL), regardless of which set originally introduced the
 * kfunc - so the filter must short-circuit on kfuncs it doesn't govern by
 * falling through to "allow" when none of the SCX sets contain the kfunc.
 */
/*
 * verifier 时按 prog 类型、cpu/cid struct_ops 形态和具体回调成员过滤 kfunc。返回 0 允许，
 * -EACCES 拒绝。未归入任何 SCX 集合的 kfunc 必须放行，避免该 filter 干扰其他子系统。
 */
/*
 * 业务背景：在 BPF 加载验证期把每个 SCX kfunc 限定到能满足其 rq 锁、RCU 和回调上下文契约的位置。
 * 入参：prog 为 verifier 持有的借用程序；kfunc_id 是待调用 BTF id，均不可空/无效。
 * 出参/返回：允许返回 0，不兼容程序类型、cpu/cid 形态或 op 能力返回 -EACCES；无运行时副作用。
 * 注意事项：首轮 st_ops 未绑定时暂放行，主验证会重跑；非 SCX kfunc 必须原样放行。
 */
int scx_kfunc_context_filter(const struct bpf_prog *prog, u32 kfunc_id)
{
	bool in_unlocked = btf_id_set8_contains(&scx_kfunc_ids_unlocked, kfunc_id);
	bool in_init = btf_id_set8_contains(&scx_kfunc_ids_init, kfunc_id);
	bool in_select_cpu = btf_id_set8_contains(&scx_kfunc_ids_select_cpu, kfunc_id);
	bool in_enqueue = btf_id_set8_contains(&scx_kfunc_ids_enqueue_dispatch, kfunc_id);
	bool in_dispatch = btf_id_set8_contains(&scx_kfunc_ids_dispatch, kfunc_id);
	bool in_cpu_release = btf_id_set8_contains(&scx_kfunc_ids_cpu_release, kfunc_id);
	/* any/idle 是跨 op 基础集，cpu_only 额外用于拒绝 cid-form 对物理 CPU API 的误调。 */
	bool in_idle = btf_id_set8_contains(&scx_kfunc_ids_idle, kfunc_id);
	bool in_any = btf_id_set8_contains(&scx_kfunc_ids_any, kfunc_id);
	bool in_cpu_only = btf_id_set8_contains(&scx_kfunc_ids_cpu_only, kfunc_id);
	u32 moff, flags;

	/* 先分类集合，再按 prog 类型、struct_ops 形态与 attach member 逐层收窄。 */
	/* Not an SCX kfunc - allow. */
	/* 若 kfunc 不在任一 SCX 集合中，本过滤器不对其施加限制。 */
	if (!(in_unlocked || in_init || in_select_cpu || in_enqueue || in_dispatch ||
	      in_cpu_release || in_idle || in_any))
		return 0;

	/* SYSCALL progs (e.g. BPF test_run()) may call unlocked and select_cpu kfuncs. */
/* 首轮收集调用时 st_ops 尚未绑定，暂放行；主验证会在绑定后重跑并作最终裁决。 */
	if (prog->type == BPF_PROG_TYPE_SYSCALL)
		return (in_unlocked || in_select_cpu || in_idle || in_any) ? 0 : -EACCES;

	if (prog->type != BPF_PROG_TYPE_STRUCT_OPS)
		return (in_any || in_idle) ? 0 : -EACCES;

	/* 首轮收集调用时 st_ops 尚未绑定，暂放行；主验证会在绑定后重跑并作最终裁决。 */
	/*
	 * add_subprog_and_kfunc() collects all kfunc calls, including dead code
	 * guarded by bpf_ksym_exists(), before check_attach_btf_id() sets
	 * prog->aux->st_ops. Allow all kfuncs when st_ops is not yet set;
	 * do_check_main() re-runs the filter with st_ops set and enforces the
	 * actual restrictions.
	 */
	/* 首轮会在 st_ops 绑定前收集死代码中的调用，此处暂放行，主验证会绑定后重跑并执行真实限制。 */
	if (!prog->aux->st_ops)
		return 0;

	/*
	 * Non-SCX struct_ops: SCX kfuncs are not permitted.
	 *
	 * Both bpf_sched_ext_ops (cpu-form) and bpf_sched_ext_ops_cid
	 * (cid-form) are valid SCX struct_ops. Member offsets match between
	 * the two (verified by BUILD_BUG_ON in scx_init()), so the shared
	 * scx_kf_allow_flags[] table indexed by SCX_MOFF_IDX(moff) applies to
	 * both.
	 */
	/* 非 SCX struct_ops 不得调用 SCX kfunc；cpu/cid 两种 ops 的成员偏移经编译期断言一致，因而可共用 allow 表。 */
	if (prog->aux->st_ops != &bpf_sched_ext_ops &&
	    prog->aux->st_ops != &bpf_sched_ext_ops_cid)
		return -EACCES;

	/*
	 * cid-form schedulers must use cid/cmask kfuncs. cid and cpu are both
	 * small s32s and trivially confused, so cpu-only kfuncs are rejected at
	 * load time. The reverse (cpu-form calling cid-form kfuncs) is
	 * intentionally permissive to ease gradual cpumask -> cid migration.
	 */
/* any/idle 是跨回调稳定 API；其余按 member offset 映射的能力位逐组匹配。 */
	if (prog->aux->st_ops == &bpf_sched_ext_ops_cid && in_cpu_only)
		return -EACCES;

	/* any/idle 是跨回调稳定 API；其余按 member offset 映射的能力位逐组匹配。 */
	/* SCX struct_ops: check the per-op allow list. */
	/* 已确认为 SCX struct_ops；下一步按 attach member 检查每个 op 的 kfunc 白名单。 */
	if (in_any || in_idle)
		return 0;

	moff = prog->aux->attach_st_ops_member_off;
	flags = scx_kf_allow_flags[SCX_MOFF_IDX(moff)];

	if ((flags & SCX_KF_ALLOW_UNLOCKED) && in_unlocked)
		return 0;
	if ((flags & SCX_KF_ALLOW_INIT) && in_init)
		return 0;
	if ((flags & SCX_KF_ALLOW_CPU_RELEASE) && in_cpu_release)
		return 0;
	/* kfunc 可属于多组，只要一个所属组被当前回调能力位允许即可。 */
	if ((flags & SCX_KF_ALLOW_DISPATCH) && in_dispatch)
		return 0;
	if ((flags & SCX_KF_ALLOW_ENQUEUE) && in_enqueue)
		return 0;
	if ((flags & SCX_KF_ALLOW_SELECT_CPU) && in_select_cpu)
		return 0;

	return -EACCES;
}

/*
 * late init 注册 SCX kfunc 集、idle/cid 辅助层、两种 struct_ops、PM notifier 与 sysfs。
 * 无输入；成功 0，任一步失败记录并返回 errno 阻止后续注册。先用 BUILD_BUG_ON 验证
 * cpu/cid ABI 共享前缀，确保按 cpu-form offset 处理 cid kdata 不越界或错位。
 */
/*
 * 业务背景：在 late_initcall 阶段一次性建立 sched_ext 的 BTF kfunc、struct_ops、sysfs、PM 与辅助子系统入口。
 * 入参：无。
 * 出参/返回：全部注册成功返回 0；任一步失败返回对应负 errno 并停止后续初始化。
 * 注意事项：BUILD_BUG_ON 先验证 cpu/cid ABI 偏移；该启动路径可睡眠，已成功注册项按各子系统寿命存在。
 */
static int __init scx_init(void)
{
	int ret;

	/*
	 * sched_ext_ops_cid mirrors sched_ext_ops up to and including @priv.
	 * Both bpf_scx_init_member() and bpf_scx_check_member() use offsets
	 * from struct sched_ext_ops; sched_ext_ops_cid relies on those offsets
	 * matching for the shared fields. Catch any drift at boot.
	 */
/* 每次展开为编译期断言，不产生运行时字段读写。 */
#define CID_OFFSET_MATCH(cpu_field, cid_field)					\
	BUILD_BUG_ON(offsetof(struct sched_ext_ops, cpu_field) !=		\
		     offsetof(struct sched_ext_ops_cid, cid_field))
	/* data fields used by bpf_scx_init_member() */
/* 数据字段检查覆盖启用协商和 sub 归属，随后再检查所有共享回调槽。 */
	CID_OFFSET_MATCH(dispatch_max_batch, dispatch_max_batch);
	CID_OFFSET_MATCH(flags, flags);
	CID_OFFSET_MATCH(name, name);
	CID_OFFSET_MATCH(timeout_ms, timeout_ms);
	CID_OFFSET_MATCH(exit_dump_len, exit_dump_len);
	/* 数据字段检查覆盖启用协商和 sub 归属，随后再检查所有共享回调槽。 */
	CID_OFFSET_MATCH(hotplug_seq, hotplug_seq);
	CID_OFFSET_MATCH(sub_cgroup_id, sub_cgroup_id);
	/* 共享回调必须逐槽位等偏移，保证 union 视图和 context filter 的 member index 一致。 */
	/* shared callbacks: the union view requires byte-for-byte offset match */
	/* 共享回调的字节偏移必须完全相同，否则 union 视图会把成员误认为另一回调。 */
	CID_OFFSET_MATCH(enqueue, enqueue);
	CID_OFFSET_MATCH(dequeue, dequeue);
	CID_OFFSET_MATCH(dispatch, dispatch);
	CID_OFFSET_MATCH(tick, tick);
	CID_OFFSET_MATCH(runnable, runnable);
	CID_OFFSET_MATCH(running, running);
	CID_OFFSET_MATCH(stopping, stopping);
	CID_OFFSET_MATCH(quiescent, quiescent);
	/* 运行态回调组结束后继续核对比较、权重和 idle 更新槽，防止中间插字段造成整体错位。 */
	CID_OFFSET_MATCH(yield, yield);
	CID_OFFSET_MATCH(core_sched_before, core_sched_before);
	CID_OFFSET_MATCH(set_weight, set_weight);
	CID_OFFSET_MATCH(update_idle, update_idle);
	/* task 生命周期与 dump 回调也必须同槽，才能共享 CFI/offset 逻辑。 */
	CID_OFFSET_MATCH(init_task, init_task);
	CID_OFFSET_MATCH(exit_task, exit_task);
	CID_OFFSET_MATCH(enable, enable);
	CID_OFFSET_MATCH(disable, disable);
	CID_OFFSET_MATCH(dump, dump);
	/* 生命周期与诊断回调同样由公共 union/offset 代码访问，必须保持一一对应。 */
	CID_OFFSET_MATCH(dump_task, dump_task);
	CID_OFFSET_MATCH(sub_attach, sub_attach);
	CID_OFFSET_MATCH(sub_detach, sub_detach);
	CID_OFFSET_MATCH(init, init);
	CID_OFFSET_MATCH(exit, exit);
#ifdef CONFIG_EXT_GROUP_SCHED
	/* 可选 cgroup 回调也属于 ABI 公共前缀，配置启用时逐项编译期检查。 */
	CID_OFFSET_MATCH(cgroup_init, cgroup_init);
	CID_OFFSET_MATCH(cgroup_exit, cgroup_exit);
	CID_OFFSET_MATCH(cgroup_prep_move, cgroup_prep_move);
	CID_OFFSET_MATCH(cgroup_move, cgroup_move);
	CID_OFFSET_MATCH(cgroup_cancel_move, cgroup_cancel_move);
	CID_OFFSET_MATCH(cgroup_set_weight, cgroup_set_weight);
	CID_OFFSET_MATCH(cgroup_set_bandwidth, cgroup_set_bandwidth);
	CID_OFFSET_MATCH(cgroup_set_idle, cgroup_set_idle);
	/* 条件字段仅在相同配置下比较，保证两种结构共同前缀不漂移。 */
#endif
	/* renamed callbacks must occupy the same slot as their cpu-form sibling */
/* 改名字段语义不同但槽位必须相同；priv 是 cid 结构尾和 cpu 公共前缀尾。 */
	CID_OFFSET_MATCH(select_cpu, select_cid);
	CID_OFFSET_MATCH(set_cpumask, set_cmask);
	CID_OFFSET_MATCH(cpu_online, cid_online);
	CID_OFFSET_MATCH(cpu_offline, cid_offline);
	CID_OFFSET_MATCH(dump_cpu, dump_cid);
	/* 改名字段语义不同但槽位必须相同；priv 是 cid 结构尾和 cpu 公共前缀尾。 */
	/* @priv tail must align since both share the same data block */
	/* cpu/cid 视图共享同一数据块，因此尾部 priv 的偏移也必须对齐。 */
	CID_OFFSET_MATCH(priv, priv);
	/*
	 * cid-form must end exactly at @priv - validate_ops() skips
	 * cpu_acquire/cpu_release for cid-form because reading those fields
	 * past the BPF allocation would be UB.
	 */
	/* cid-form 必须在 priv 后精确结束；否则 validate_ops 略过 cpu-only 字段时仍可越过 BPF 分配边界引发 UB。 */
	BUILD_BUG_ON(offsetof(struct sched_ext_ops_cid, __end) !=
		     offsetofend(struct sched_ext_ops, priv));
#undef CID_OFFSET_MATCH

	/*
	 * kfunc registration can't be done from init_sched_ext_class() as
	 * register_btf_kfunc_id_set() needs most of the system to be up.
	 *
	 * Some kfuncs are context-sensitive and can only be called from
	 * specific SCX ops. They are grouped into per-context BTF sets, each
	 * registered with scx_kfunc_context_filter as its .filter callback. The
	 * BPF core dedups identical filter pointers per hook
	 * (btf_populate_kfunc_set()), so the filter is invoked exactly once per
	 * kfunc lookup; it consults scx_kf_allow_flags[] to enforce per-op
	 * restrictions at verify time.
	 */
	/* 覆盖 struct_ops、syscall、tracing 三类 prog；短路表达式在首错处停止。 */
	if ((ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_enqueue_dispatch)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_dispatch)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_cpu_release)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_unlocked)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_SYSCALL,
					     &scx_kfunc_set_unlocked)) ||
	    /* any 集同时服务 struct_ops、tracing 与 syscall，三类都必须经同一上下文 filter。 */
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS,
					     &scx_kfunc_set_any)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_TRACING,
					     &scx_kfunc_set_any)) ||
	    (ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_SYSCALL,
					     &scx_kfunc_set_any))) {
		pr_err("sched_ext: Failed to register kfunc sets (%d)\n", ret);
		return ret;
	}

	/* verifier API 齐备后，才注册它们依赖的 idle/cid 实现和 struct_ops。 */
	/* 基础能力按依赖顺序注册；后一步只在前一步成功后执行，错误立即返回。 */
	ret = scx_idle_init();
	if (ret) {
		pr_err("sched_ext: Failed to initialize idle tracking (%d)\n", ret);
		return ret;
	}

	ret = scx_cid_kfunc_init();
	if (ret) {
		pr_err("sched_ext: Failed to register cid kfuncs (%d)\n", ret);
		return ret;
	}

	/* 两种 struct_ops ABI 分别服务物理 CPU 与 cid；任一缺失都会让公开能力不完整。 */
	ret = register_bpf_struct_ops(&bpf_sched_ext_ops, sched_ext_ops);
	if (ret) {
		pr_err("sched_ext: Failed to register struct_ops (%d)\n", ret);
		return ret;
	}

	/* cpu-form 成功后注册 cid-form；失败不发布 PM/sysfs 观察面。 */
	ret = register_bpf_struct_ops(&bpf_sched_ext_ops_cid, sched_ext_ops_cid);
	if (ret) {
		pr_err("sched_ext: Failed to register cid struct_ops (%d)\n", ret);
		return ret;
	}

	/* 两种 ABI 都注册成功后再接入 PM 和 sysfs，避免用户看到半可用框架。 */
	ret = register_pm_notifier(&scx_pm_notifier);
	if (ret) {
		pr_err("sched_ext: Failed to register PM notifier (%d)\n", ret);
		return ret;
	}

	scx_kset = kset_create_and_add("sched_ext", &scx_uevent_ops, kernel_kobj);
	if (!scx_kset) {
		pr_err("sched_ext: Failed to create /sys/kernel/sched_ext\n");
		return -ENOMEM;
	}

	/* global attrs 最后发布；init 返回 0 时目录才具备完整观察面。 */
	ret = sysfs_create_group(&scx_kset->kobj, &scx_global_attr_group);
	if (ret < 0) {
		pr_err("sched_ext: Failed to add global attributes\n");
		return ret;
	}

	return 0;
}
__initcall(scx_init);
