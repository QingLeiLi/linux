// SPDX-License-Identifier: GPL-2.0-only
/*
 *  kernel/sched/syscalls.c
 *
 *  Core kernel scheduler syscalls related code
 *
 *  Copyright (C) 1991-2002  Linus Torvalds
 *  Copyright (C) 1998-2024  Ingo Molnar, Red Hat
 */
/*
 * 本文件把用户可见的 policy、priority、nice、uclamp 与 affinity 请求转换为
 * 调度器内部状态。共同主线是：复制并规范化用户参数→查找且持有 task→
 * 权限/LSM/资源上限检查→在 rq/pi/cpuset 锁协议下 dequeue、修改、enqueue。
 * 任何会睡眠的校验必须在 rq 锁外完成；task_rq_lock 后若 policy 或 cpuset
 * 已变化则重新检查。affinity 的 user_mask 所有权由 context 明确转交，失败
 * 路径释放临时 mask，读取接口只承诺与相应锁一致的瞬时快照。
 */
#include <linux/sched.h>
#include <linux/cpuset.h>
#include <linux/sched/debug.h>

#include <uapi/linux/sched/types.h>

#include "sched.h"
#include "autogroup.h"

/* 把 policy、用户 RT priority 或 nice 映射为统一内部 prio；值越小优先级越高。 */
/*
 * 业务背景：调度核心需把 DL、RT 和 fair 三种用户参数统一到值越小越优先的内部 prio 坐标。
 * 入参：policy 是已校验调度策略；rt_prio 是 [0,MAX_RT_PRIO) 的 RT 优先级；nice 是 [MIN_NICE,MAX_NICE]，均仅输入。
 * 出参/返回：DL 返回 MAX_DL_PRIO-1，RT 返回反向映射值，fair 返回 NICE_TO_PRIO(nice)；无输出参。
 * 注意事项：不校验范围、无锁且不睡眠；非法输入可生成越界 prio，必须由上层门禁保证。
 */
static inline int __normal_prio(int policy, int rt_prio, int nice)
{
	int prio;

	/* DL 固定占据最高普通优先级槽；RT 反向映射，其他策略使用 nice。 */
	if (dl_policy(policy))
		prio = MAX_DL_PRIO - 1;
	else if (rt_policy(policy))
		prio = MAX_RT_PRIO - 1 - rt_prio;
	else
		prio = NICE_TO_PRIO(nice);

	return prio;
}

/*
 * Calculate the expected normal priority: i.e. priority
 * without taking RT-inheritance into account. Might be
 * boosted by interactivity modifiers. Changes upon fork,
 * setprio syscalls, and whenever the interactivity
 * estimator recalculates.
 */
/* 根据 @p 当前策略计算不含 PI boost 的 normal_prio。 */
/*
 * 业务背景：task 需保留一个排除 PI boost 的基准优先级，供策略更换和 boost 退出时恢复。
 * 入参：p 是不可空只读借用 task，policy/rt_priority/static_prio 须在调用者锁下稳定。
 * 出参/返回：返回 p 当前策略对应的基准内部 prio；无输出参和状态修改。
 * 注意事项：不取锁、不取引用且不睡眠；并发策略变更时结果只是瞬时快照。
 */
static inline int normal_prio(struct task_struct *p)
{
	return __normal_prio(p->policy, p->rt_priority, PRIO_TO_NICE(p->static_prio));
}

/*
 * Calculate the current priority, i.e. the priority
 * taken into account by the scheduler. This value might
 * be boosted by RT tasks, or might be boosted by
 * interactivity modifiers. Will be RT if the task got
 * RT-boosted. If not then it returns p->normal_prio.
 */
/* 更新 normal_prio；若 @p 正被 RT/DL PI boost 则保留当前有效 prio。 */
/*
 * 业务背景：基准优先级改变后，应更新 normal_prio，但不能覆盖正在生效的 RT/DL PI boost。
 * 入参：p 是不可空输入/输出 task，调用者必须使其调度字段稳定。
 * 出参/返回：写 p->normal_prio；未被 RT/DL boost 时返新 normal_prio，否则返现有 p->prio。
 * 注意事项：本函数不取锁；应在 rq/pi 协议保护下调用，否则可与 boost 并发而丢失状态。
 */
static int effective_prio(struct task_struct *p)
{
	p->normal_prio = normal_prio(p);
	/*
	 * If we are RT tasks or we were boosted to RT priority,
	 * keep the priority unchanged. Otherwise, update priority
	 * to the normal priority:
	 */
	if (!rt_or_dl_prio(p->prio))
		return p->normal_prio;
	return p->prio;
}

/*
 * 在 task rq 锁及 sched_change scope 下更新 @p 的 nice/权重/有效优先级；
 * 非法或相同 nice 快速返回，RT/DL 仅保存将来回到 fair 后使用的 static_prio。
 */
/*
 * 业务背景：setpriority/nice 需改变 fair task 权重，同时允许 RT/DL task 预置未来回到 fair 时的 nice。
 * 入参：p 是不可空输入/输出 task 借用指针；nice 是 [MIN_NICE,MAX_NICE] 的目标值、仅输入。
 * 出参/返回：无直接返回值、无输出参；成功更新 static_prio，fair task 还更新权重和有效 prio。
 * 注意事项：相同/越界值静默返回；内部取 task rq 锁且不睡眠，sched_change scope 负责 dequeue/enqueue 一致性。
 */
void set_user_nice(struct task_struct *p, long nice)
{
	int old_prio;

	if (task_nice(p) == nice || nice < MIN_NICE || nice > MAX_NICE)
		return;
	/* task 可能正在其他 CPU 调度，必须锁住其当前 rq 后再判断调度类。 */
	/*
	 * We have to be careful, if called from sys_setpriority(),
	 * the task might be in the middle of scheduling on another CPU.
	 */
	guard(task_rq_lock)(p);

	/*
	 * The RT priorities are set via sched_setscheduler(), but we still
	 * allow the 'normal' nice value to be set - but as expected
	 * it won't have any effect on scheduling until the task is
	 * SCHED_DEADLINE, SCHED_FIFO or SCHED_RR:
	 */
	if (task_has_dl_policy(p) || task_has_rt_policy(p)) {
		p->static_prio = NICE_TO_PRIO(nice);
		return;
	}

	/* fair task 需在出入队保护范围内同步更新权重与有效优先级。 */
	scoped_guard (sched_change, p, DEQUEUE_SAVE) {
		p->static_prio = NICE_TO_PRIO(nice);
		set_load_weight(p, true);
		old_prio = p->prio;
		p->prio = effective_prio(p);
	}
}
EXPORT_SYMBOL(set_user_nice);

/*
 * is_nice_reduction - check if nice value is an actual reduction
 *
 * Similar to can_nice() but does not perform a capability check.
 *
 * @p: task
 * @nice: nice value
 */
/* 只按 @p 的 RLIMIT_NICE 判断是否允许提高优先级，不执行 capability 检查。 */
/*
 * 业务背景：普通用户能否降低 nice 取决于目标值是否在 task 的 RLIMIT_NICE 配额内。
 * 入参：p 是不可空只读 task；nice 是 [MIN_NICE,MAX_NICE] 目标值，均仅输入。
 * 出参/返回：RLIMIT_NICE 允许返回 true，否则 false；无输出参和副作用。
 * 注意事项：不检查 CAP_SYS_NICE；读取 rlimit 快照，不取 rq 锁、不睡眠。
 */
static bool is_nice_reduction(const struct task_struct *p, const int nice)
{
	/* Convert nice value [19,-20] to rlimit style value [1,40]: */
	int nice_rlim = nice_to_rlimit(nice);

	return (nice_rlim <= task_rlimit(p, RLIMIT_NICE));
}

/*
 * can_nice - check if a task can reduce its nice value
 * @p: task
 * @nice: nice value
 */
/* RLIMIT_NICE 允许或调用者具 CAP_SYS_NICE 时返回 true。 */
/*
 * 业务背景：nice 提权同时支持资源限额授权和 CAP_SYS_NICE 管理员越权。
 * 入参：p 是不可空只读目标 task；nice 是有效目标 nice，均仅输入。
 * 出参/返回：rlimit 门禁或 CAP_SYS_NICE 任一通过返回 true，否则 false；无输出参。
 * 注意事项：capable() 可产生安全审计事件；应在便宜的 rlimit 检查失败后才执行。
 */
int can_nice(const struct task_struct *p, const int nice)
{
	return is_nice_reduction(p, nice) || capable(CAP_SYS_NICE);
}

#ifdef __ARCH_WANT_SYS_NICE

/*
 * sys_nice - change the priority of the current process.
 * @increment: priority increment
 *
 * sys_setpriority is a more generic, but much slower function that
 * does similar things.
 */
/* 当前 task nice 增量 syscall：夹取范围、检查提权/LSM，成功返回 0。 */
/*
 * 业务背景：nice(2) 以增量形式修改当前 task 优先级，是 setpriority 的轻量快捷入口。
 * 入参：increment 是用户传入增量，先夹到 [-NICE_WIDTH,NICE_WIDTH]，仅输入。
 * 出参/返回：成功返回 0；提高优先级无权返回 -EPERM，LSM 拒绝返回其 errno；无输出参。
 * 注意事项：仅 __ARCH_WANT_SYS_NICE 下存在；与 setpriority 并发时最后获锁者生效，LSM 检查可睡眠。
 */
SYSCALL_DEFINE1(nice, int, increment)
{
	long nice, retval;

	/*
	 * Setpriority might change our priority at the same moment.
	 * We don't have to worry. Conceptually one call occurs first
	 * and we have a single winner.
	 */
	/* 先约束增量及最终 nice，避免整数范围和 ABI 外输入穿透后续门禁。 */
	increment = clamp(increment, -NICE_WIDTH, NICE_WIDTH);
	nice = task_nice(current) + increment;

	nice = clamp_val(nice, MIN_NICE, MAX_NICE);
	if (increment < 0 && !can_nice(current, nice))
		return -EPERM;

	/* LSM 在真正修改前拥有最终否决权，失败时 current 保持原状态。 */
	retval = security_task_setnice(current, nice);
	if (retval)
		return retval;

	set_user_nice(current, nice);
	return 0;
}

#endif /* __ARCH_WANT_SYS_NICE */

/**
 * task_prio - return the priority value of a given task.
 * @p: the task in question.
 *
 * Return: The priority value as seen by users in /proc.
 *
 * sched policy         return value   kernel prio    user prio/nice
 *
 * normal, batch, idle     [0 ... 39]  [100 ... 139]          0/[-20 ... 19]
 * fifo, rr             [-2 ... -100]     [98 ... 0]  [1 ... 99]
 * deadline                     -101             -1           0
 */
/* 返回 /proc 使用的用户视角 priority 编码，不改变 @p。 */
/*
 * 业务背景：/proc 需把内核统一 prio 坐标转换为用户可见的 fair/RT/DL 编码。
 * 入参：p 是不可空只读 task 借用指针，prio 稳定性由调用者保证。
 * 出参/返回：返回 p->prio-MAX_RT_PRIO，fair 为 0..39，RT 为负值，DL 为 -101；无输出参。
 * 注意事项：无锁瞬时读取，不取 task 引用、不睡眠；并发调度变更可使结果立即过时。
 */
int task_prio(const struct task_struct *p)
{
	return p->prio - MAX_RT_PRIO;
}

/**
 * idle_cpu - is a given CPU idle currently?
 * @cpu: the processor in question.
 *
 * Return: 1 if the CPU is currently idle. 0 otherwise.
 */
/* 返回 @cpu rq 是否当前 idle；值是无锁瞬时判断。 */
/*
 * 业务背景：调度/工作分发路径需快速判断某 CPU runqueue 当前是否只运行 idle task。
 * 入参：cpu 是 [0,nr_cpu_ids) 的有效 CPU 编号、仅输入。
 * 出参/返回：rq 当前 idle 返回 1，否则 0；无输出参。
 * 注意事项：无锁瞬时快照，不承诺返回后 CPU 仍 idle；调用者负责 hotplug 范围稳定。
 */
int idle_cpu(int cpu)
{
	return idle_rq(cpu_rq(cpu));
}

/**
 * idle_task - return the idle task for a given CPU.
 * @cpu: the processor in question.
 *
 * Return: The idle task for the CPU @cpu.
 */
/* 借用并返回 @cpu 永久 idle task 指针，不增加引用。 */
/*
 * 业务背景：调度核心和观测路径需取得每 CPU 的专属 idle task。
 * 入参：cpu 是 [0,nr_cpu_ids) 的有效 CPU 编号、仅输入。
 * 出参/返回：返回 cpu_rq(cpu)->idle 的非空借用指针；无输出参、不转移 ownership。
 * 注意事项：idle task 与 CPU 同寿命不需 put；调用者必须保证 CPU 编号/hotplug 上下文有效。
 */
struct task_struct *idle_task(int cpu)
{
	return cpu_rq(cpu)->idle;
}

#ifdef CONFIG_SCHED_CORE
/* core scheduling 启用时按 rq->curr==idle 判断，否则沿用普通 idle_cpu。 */
/*
 * 业务背景：core scheduling 下逻辑 CPU 的核级 idle 语义需根据当前 task，不能完全复用普通 rq idle 标志。
 * 入参：cpu 是 [0,nr_cpu_ids) 有效 CPU 编号、仅输入。
 * 出参/返回：core enabled 且 curr==idle 返回 1，否则返回 idle_cpu(cpu) 结果；无输出参。
 * 注意事项：仅 CONFIG_SCHED_CORE 下存在；无锁快照可立即过时，不可作为独占 CPU 的同步保证。
 */
int sched_core_idle_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (sched_core_enabled(rq) && rq->curr == rq->idle)
		return 1;

	return idle_cpu(cpu);
}
#endif /* CONFIG_SCHED_CORE */

/**
 * find_process_by_pid - find a process with a matching PID value.
 * @pid: the pid in question.
 *
 * The task of @pid, if found. %NULL otherwise.
 */
/* RCU 读侧按 vpid 查 task；pid==0 选择 current，返回借用指针或 NULL。 */
/*
 * 业务背景：调度 syscall 需按调用者 PID namespace 中的 vpid 定位目标 task，pid 0 指当前 task。
 * 入参：pid 是非负 vpid，0 表示 current，仅输入。
 * 出参/返回：找到返回 task 借用指针，未找到返回 NULL；无输出参和引用转移。
 * 注意事项：调用者必须已持 RCU 读锁；返回指针仅在 RCU 临界区有效，需跨越时必须取引用。
 */
static struct task_struct *find_process_by_pid(pid_t pid)
{
	return pid ? find_task_by_vpid(pid) : current;
}

/* RCU 查找后取得 task_struct 引用；调用者必须 put，未找到返回 NULL。 */
/*
 * 业务背景：syscall 在可睡眠的权限/参数检查期间需跨越 RCU 临界区稳定目标 task 生命期。
 * 入参：pid 是非负 vpid，0 表示 current，仅输入。
 * 出参/返回：成功返回持有一份 task_struct 引用的指针，未找到返回 NULL；无输出参。
 * 注意事项：内部 RCU 查找后 get_task_struct；调用者必须 put_task_struct，DEFINE_CLASS helper 可自动收尾。
 */
static struct task_struct *find_get_task(pid_t pid)
{
	struct task_struct *p;
	guard(rcu)();

	/* 仅在 RCU 保护内取得引用，返回后由清理类统一 put。 */
	p = find_process_by_pid(pid);
	if (likely(p))
		get_task_struct(p);

	return p;
}

DEFINE_CLASS(find_get_task, struct task_struct *, if (_T) put_task_struct(_T),
	     find_get_task(pid), pid_t pid)

/*
 * sched_setparam() passes in -1 for its policy, to let the functions
 * it calls know not to change it.
 */
#define SETPARAM_POLICY	-1

/* 已在 sched_change 临界区内提交 policy/class 参数、timer slack、prio 与负载权重。 */
/*
 * 业务背景：通过所有门禁后，策略切换需把 sched_attr 原子投影到 task 的 policy/class 专用字段。
 * 入参：p 是不可空输入/输出 task；attr 是不可空已校验只读属性，SETPARAM_POLICY 表示保留原 policy。
 * 出参/返回：无直接返回值、无输出参；更新 policy、类参数、timer slack、rt/normal prio 和 load weight。
 * 注意事项：必须在 task rq 锁与 sched_change scope 内调用；不做权限/范围检查，误用会发布不一致类状态。
 */
static void __setscheduler_params(struct task_struct *p,
		const struct sched_attr *attr)
{
	int policy = attr->sched_policy;

	/* setparam 的哨兵表示沿用锁内观察到的现有策略。 */
	if (policy == SETPARAM_POLICY)
		policy = p->policy;

	p->policy = policy;

	/* 只有含专用参数的类需要额外写入其调度实体。 */
	if (dl_policy(policy))
		__setparam_dl(p, attr);
	else if (fair_policy(policy))
		__setparam_fair(p, attr);

	/* rt-policy tasks do not have a timerslack */
	if (rt_or_dl_task_policy(p)) {
		p->timer_slack_ns = 0;
	} else if (p->timer_slack_ns == 0) {
		/* when switching back to non-rt policy, restore timerslack */
		p->timer_slack_ns = p->default_timer_slack_ns;
	}

	/* 最后统一刷新用户优先级镜像、基准优先级和负载权重。 */
	/*
	 * __sched_setscheduler() ensures attr->sched_priority == 0 when
	 * !rt_policy. Always setting this ensures that things like
	 * getparam()/getattr() don't report silly values for !rt tasks.
	 */
	p->rt_priority = attr->sched_priority;
	p->normal_prio = normal_prio(p);
	set_load_weight(p, true);
}

/*
 * Check the target process has a UID that matches the current process's:
 */
/* RCU 下比较 current euid 与目标 uid/euid，返回是否具同一用户所有权。 */
/*
 * 业务背景：非特权调度变更只允许操作与当前有效 UID 相同的 task。
 * 入参：p 是不可空只读目标 task 借用指针。
 * 出参/返回：current euid 匹配目标 uid 或 euid 返回 true，否则 false；无输出参。
 * 注意事项：内部 RCU 保护 task cred 指针；不执行 capability 检查，不保存 cred 借用指针。
 */
static bool check_same_owner(struct task_struct *p)
{
	const struct cred *cred = current_cred(), *pcred;
	guard(rcu)();

	pcred = __task_cred(p);
	return (uid_eq(cred->euid, pcred->euid) ||
		uid_eq(cred->euid, pcred->uid));
}

#ifdef CONFIG_RT_MUTEXES
/* 降低被 DL PI boost 的 task 策略时继承 top waiter 参数并请求下次 enqueue 补充。 */
/*
 * 业务背景：被 deadline waiter PI boost 的 task 即使本身切到低等级策略，仍须继承 waiter 的 DL 实体参数。
 * 入参：newprio/policy 是已计算的新优先级/策略；p 是不可空输入/输出 task；scope 是不可空 sched_change 上下文。
 * 出参/返回：无直接返回值；命中 top waiter 时输出 p->dl.pi_se 和 scope->flags|=ENQUEUE_REPLENISH。
 * 注意事项：仅 CONFIG_RT_MUTEXES 实现；必须在 PI/rq 锁协议下调用，pi_task 指针仅在该临界区借用。
 */
static inline void __setscheduler_dl_pi(int newprio, int policy,
			      struct task_struct *p,
			      struct sched_change_ctx *scope)
{
	/*
	 * In case a DEADLINE task (either proper or boosted) gets
	 * setscheduled to a lower priority class, check if it neeeds to
	 * inherit parameters from a potential pi_task. In that case make
	 * sure replenishment happens with the next enqueue.
	 */

	/* 有 DL boost 但新策略非 DL 时，从最高 waiter 继承有效 DL 参数。 */
	if (dl_prio(newprio) && !dl_policy(policy)) {
		struct task_struct *pi_task = rt_mutex_get_top_task(p);

		if (pi_task) {
			p->dl.pi_se = pi_task->dl.pi_se;
			scope->flags |= ENQUEUE_REPLENISH;
		}
	}
}
#else /* !CONFIG_RT_MUTEXES */
/* 未配置 RT_MUTEXES 时不存在 PI deadline 参数需要继承。 */
/*
 * 业务背景：未启用 RT mutex PI 时保留统一策略提交调用点。
 * 入参：newprio、policy、p、scope 均为未使用输入，p/scope 可为 NULL 因为 stub 不解引用。
 * 出参/返回：无直接返回值、无输出参，无状态修改。
 * 注意事项：仅 !CONFIG_RT_MUTEXES 下存在；不取锁、不睡眠，必须保持中性语义。
 */
static inline void __setscheduler_dl_pi(int newprio, int policy,
			      struct task_struct *p,
			      struct sched_change_ctx *scope)
{
}
#endif /* !CONFIG_RT_MUTEXES */

#ifdef CONFIG_UCLAMP_TASK

/* 合并 @p 旧 clamp 与 attr 指定端点，校验范围及 min<=max；成功在锁外启用 static key。 */
/*
 * 业务背景：uclamp 请求可只更新 min/max 一端，提交前需与 task 现值合并并验证区间。
 * 入参：p 是不可空只读 task；attr 是不可空只读属性，util 端点为 -1 或 [0,SCHED_CAPACITY_SCALE]。
 * 出参/返回：有效区间返回 0，端点越界或 min>max 返回 -EINVAL；无输出参。
 * 注意事项：成功路径 sched_uclamp_enable() 可阻塞，必须在 rq 锁外调用；本函数不提交 task clamp。
 */
static int uclamp_validate(struct task_struct *p,
			   const struct sched_attr *attr)
{
	int util_min = p->uclamp_req[UCLAMP_MIN].value;
	int util_max = p->uclamp_req[UCLAMP_MAX].value;

	/* 未请求的端点沿用 task 现值，请求的端点允许 -1 作为恢复默认哨兵。 */
	if (attr->sched_flags & SCHED_FLAG_UTIL_CLAMP_MIN) {
		util_min = attr->sched_util_min;

		if (util_min + 1 > SCHED_CAPACITY_SCALE + 1)
			return -EINVAL;
	}

	/* max 端点采用同样的无符号加一技巧，使 -1 哨兵与合法上界同时通过。 */
	if (attr->sched_flags & SCHED_FLAG_UTIL_CLAMP_MAX) {
		util_max = attr->sched_util_max;

		if (util_max + 1 > SCHED_CAPACITY_SCALE + 1)
			return -EINVAL;
	}

	if (util_min != -1 && util_max != -1 && util_min > util_max)
		return -EINVAL;

	/* 校验完成后才启用 static key；该操作会阻塞，不能挪进 rq 锁区。 */
	/*
	 * We have valid uclamp attributes; make sure uclamp is enabled.
	 *
	 * We need to do that here, because enabling static branches is a
	 * blocking operation which obviously cannot be done while holding
	 * scheduler locks.
	 */
	sched_uclamp_enable();

	return 0;
}

/* 判断 @clamp_id 是否应恢复策略默认值：类切换的非用户值或显式 -1。 */
/*
 * 业务背景：策略切换时内核默认 clamp 应重置，但用户显式设定值需保留。
 * 入参：attr 是不可空只读属性；clamp_id 是 UCLAMP_MIN/MAX；uc_se 是不可空只读 task clamp 端点。
 * 出参/返回：应恢复默认值返回 true，应保留/提交用户值返回 false；无输出参。
 * 注意事项：-1 是显式 reset 哨兵；函数不取锁、不修改状态，调用者须保证 uc_se 稳定。
 */
static bool uclamp_reset(const struct sched_attr *attr,
			 enum uclamp_id clamp_id,
			 struct uclamp_se *uc_se)
{
	/* 未带显式 clamp 时，仅重置从旧策略继承且非用户定义的默认端点。 */
	/* Reset on sched class change for a non user-defined clamp value. */
	if (likely(!(attr->sched_flags & SCHED_FLAG_UTIL_CLAMP)) &&
	    !uc_se->user_defined)
		return true;

	/* Reset on sched_util_{min,max} == -1. */
	if (clamp_id == UCLAMP_MIN &&
	    attr->sched_flags & SCHED_FLAG_UTIL_CLAMP_MIN &&
	    attr->sched_util_min == -1) {
		return true;
	}

	/* max 与 min 对称处理，只有对应 flag 和 -1 同时出现才表示 reset。 */
	if (clamp_id == UCLAMP_MAX &&
	    attr->sched_flags & SCHED_FLAG_UTIL_CLAMP_MAX &&
	    attr->sched_util_max == -1) {
		return true;
	}

	return false;
}

/* 在调度状态修改临界区重置策略默认 clamp，再提交 attr 显式用户 clamp。 */
/*
 * 业务背景：策略变更提交时要先应用 class 默认 clamp，再用 attr 中的显式端点覆盖。
 * 入参：p 是不可空输入/输出 task；attr 是不可空、已经 uclamp_validate() 的只读属性。
 * 出参/返回：无直接返回值、无输出参；更新 p->uclamp_req 两个端点和 user_defined 标志。
 * 注意事项：必须在 sched_change/rq 锁临界区调用；RT UCLAMP_MIN 默认受 sysctl 影响，本函数不睡眠。
 */
static void __setscheduler_uclamp(struct task_struct *p,
				  const struct sched_attr *attr)
{
	enum uclamp_id clamp_id;

	/* 先逐端点恢复策略默认值，保留仍由用户显式拥有的 clamp。 */
	for_each_clamp_id(clamp_id) {
		struct uclamp_se *uc_se = &p->uclamp_req[clamp_id];
		unsigned int value;

		if (!uclamp_reset(attr, clamp_id, uc_se))
			continue;

		/* RT 的 min 默认值可由 sysctl 调整，其余端点使用各自无约束值。 */
		/*
		 * RT by default have a 100% boost value that could be modified
		 * at runtime.
		 */
		if (unlikely(rt_task(p) && clamp_id == UCLAMP_MIN))
			value = sysctl_sched_uclamp_util_min_rt_default;
		else
			value = uclamp_none(clamp_id);

		uclamp_se_set(uc_se, value, false);

	}

	if (likely(!(attr->sched_flags & SCHED_FLAG_UTIL_CLAMP)))
		return;

	/* 显式非 -1 的端点在默认值之后提交，并标记为用户定义。 */
	if (attr->sched_flags & SCHED_FLAG_UTIL_CLAMP_MIN &&
	    attr->sched_util_min != -1) {
		uclamp_se_set(&p->uclamp_req[UCLAMP_MIN],
			      attr->sched_util_min, true);
	}

	/* max 的显式值独立提交，允许只改变两个端点中的一个。 */
	if (attr->sched_flags & SCHED_FLAG_UTIL_CLAMP_MAX &&
	    attr->sched_util_max != -1) {
		uclamp_se_set(&p->uclamp_req[UCLAMP_MAX],
			      attr->sched_util_max, true);
	}
}

#else /* !CONFIG_UCLAMP_TASK: */

/* 未配置 UCLAMP 时任何 clamp 请求返回 -EOPNOTSUPP。 */
/*
 * 业务背景：未启用 UCLAMP_TASK 的内核仍需统一验证接口，并对该能力显式拒绝。
 * 入参：p/attr 是未使用输入，两指针可为 NULL 因为 stub 不解引用。
 * 出参/返回：恒返回 -EOPNOTSUPP；无输出参和状态修改。
 * 注意事项：仅 !CONFIG_UCLAMP_TASK 下存在；不取锁、不睡眠，调用者应向用户传播不支持。
 */
static inline int uclamp_validate(struct task_struct *p,
				  const struct sched_attr *attr)
{
	return -EOPNOTSUPP;
}
/* 未配置 UCLAMP 时提交 helper 无副作用。 */
/*
 * 业务背景：未启用 UCLAMP_TASK 时保留策略提交的统一调用点。
 * 入参：p/attr 均是未使用输入，可为 NULL 因为 stub 不解引用。
 * 出参/返回：无直接返回值、无输出参和状态修改。
 * 注意事项：仅 !CONFIG_UCLAMP_TASK 下存在；必须保持中性语义，不取锁、不睡眠。
 */
static void __setscheduler_uclamp(struct task_struct *p,
				  const struct sched_attr *attr) { }
#endif /* !CONFIG_UCLAMP_TASK */

/*
 * Allow unprivileged RT tasks to decrease priority.
 * Only issue a capable test if needed and only once to avoid an audit
 * event on permitted non-privileged operations:
 */
/*
 * 用 RLIMIT_NICE/RTPRIO、ownership、reset-on-fork 约束普通用户；只有确需
 * 越权时才检查 CAP_SYS_NICE，避免对本来允许的请求产生审计事件。
 */
/*
 * 业务背景：用户态修改调度策略前，要把资源限制、目标归属和特权检查集中在锁外完成。
 * 入参：p 是不可空的目标 task，只读且由调用者持有引用；attr 是不可空只读请求；policy 是已解析策略；reset_on_fork 是目标布尔状态。
 * 出参/返回：请求被当前用户或 CAP_SYS_NICE 允许时返回 0，否则返回 -EPERM；无输出参。
 * 注意事项：本函数不提交调度状态；capable() 可能产生审计记录，所以仅在普通规则拒绝后调用，调用期间 p/attr 必须保持有效。
 */
static int user_check_sched_setscheduler(struct task_struct *p,
					 const struct sched_attr *attr,
					 int policy, int reset_on_fork)
{
	/* fair 提高优先级必须落在 RLIMIT_NICE 内，否则转入统一特权检查。 */
	if (fair_policy(policy)) {
		if (attr->sched_nice < task_nice(p) &&
		    !is_nice_reduction(p, attr->sched_nice))
			goto req_priv;
	}

	if (rt_policy(policy)) {
		unsigned long rlim_rtprio = task_rlimit(p, RLIMIT_RTPRIO);

		/* 无 RT 配额不能切入 RT 类，提高 RT priority 也不能超过配额。 */
		/* Can't set/change the rt policy: */
		if (policy != p->policy && !rlim_rtprio)
			goto req_priv;

		/* Can't increase priority: */
		if (attr->sched_priority > p->rt_priority &&
		    attr->sched_priority > rlim_rtprio)
			goto req_priv;
	}

	/*
	 * Can't set/change SCHED_DEADLINE policy at all for now
	 * (safest behavior); in the future we would like to allow
	 * unprivileged DL tasks to increase their relative deadline
	 * or reduce their runtime (both ways reducing utilization)
	 */
	if (dl_policy(policy))
		goto req_priv;

	/* 离开 SCHED_IDLE 等同从 nice 20 提权，同样受 RLIMIT_NICE 限制。 */
	/*
	 * Treat SCHED_IDLE as nice 20. Only allow a switch to
	 * SCHED_NORMAL if the RLIMIT_NICE would normally permit it.
	 */
	if (task_has_idle_policy(p) && !idle_policy(policy)) {
		if (!is_nice_reduction(p, task_nice(p)))
			goto req_priv;
	}

	/* Can't change other user's priorities: */
	if (!check_same_owner(p))
		goto req_priv;

	/* Normal users shall not reset the sched_reset_on_fork flag: */
	if (p->sched_reset_on_fork && !reset_on_fork)
		goto req_priv;

	/* 所有普通用户规则均通过时避免调用 capable()，也避免额外审计事件。 */
	return 0;

req_priv:
	if (!capable(CAP_SYS_NICE))
		return -EPERM;

	return 0;
}

/*
 * 策略变更核心：先做可睡眠校验，再锁 task rq，竞态时 recheck；将 task
 * 从旧 class 队列暂时摘下，提交参数/PI/uclamp，按新 class 重新入队并回调。
 * 成功返回 0；参数、权限、admission、LSM 或竞态约束失败返回负 errno。
 */
/*
 * 业务背景：所有调度策略变更最终在此完成校验、带宽准入、队列迁移和调度类切换，保证观察到原子的新状态。
 * 入参：p 是不可空输入/输出 task，调用者持有有效引用；attr 是不可空只读请求；user 表示执行用户权限/LSM 检查；pi 表示同步 PI 链。
 * 出参/返回：成功返回 0 并更新 p 的策略、优先级、uclamp 与队列状态；失败返回 -EINVAL/-EPERM/-EOPNOTSUPP/-EBUSY 等负 errno，无输出参。
 * 注意事项：可能因 uclamp static key 或 cpuset_lock 睡眠；内部获取 cpuset、pi_lock/rq 锁并处理策略竞态，禁止 pi=true 的中断上下文调用。
 */
int __sched_setscheduler(struct task_struct *p,
			 const struct sched_attr *attr,
			 bool user, bool pi)
{
	int oldpolicy = -1, policy = attr->sched_policy;
	int retval, oldprio, newprio;
	const struct sched_class *prev_class, *next_class;
	/* head 保存解锁后运行的 balance callbacks，rf 配对 rq 锁状态。 */
	struct balance_callback *head;
	struct rq_flags rf;
	int reset_on_fork;
	int queue_flags = DEQUEUE_SAVE | DEQUEUE_MOVE | DEQUEUE_NOCLOCK;
	struct rq *rq;
	bool cpuset_locked = false;

	/* PI 调整要求可响应中断；随后每次 recheck 都重新解析可能变化的策略。 */
	/* The pi code expects interrupts enabled */
	BUG_ON(pi && in_interrupt());
recheck:
	/* SETPARAM_POLICY 在每轮按 task 当前 policy 解析；显式策略则先验证枚举。 */
	/* Double check policy once rq lock held: */
	if (policy < 0) {
		reset_on_fork = p->sched_reset_on_fork;
		policy = oldpolicy = p->policy;
	} else {
		reset_on_fork = !!(attr->sched_flags & SCHED_FLAG_RESET_ON_FORK);

		if (!valid_policy(policy))
			return -EINVAL;
	}

	if (attr->sched_flags & ~(SCHED_FLAG_ALL | SCHED_FLAG_SUGOV))
		return -EINVAL;

	/* priority 必须与策略类型匹配，DL 还需验证 runtime/deadline/period。 */
	/*
	 * Valid priorities for SCHED_FIFO and SCHED_RR are
	 * 1..MAX_RT_PRIO-1, valid priority for SCHED_NORMAL,
	 * SCHED_BATCH and SCHED_IDLE is 0.
	 */
	if (attr->sched_priority > MAX_RT_PRIO-1)
		return -EINVAL;
	if ((dl_policy(policy) && !__checkparam_dl(attr)) ||
	    (rt_policy(policy) != (attr->sched_priority != 0)))
		return -EINVAL;

	if (user) {
		/* 用户请求依次经过 rlimit/owner/capability、内部标志和 LSM 门禁。 */
		retval = user_check_sched_setscheduler(p, attr, policy, reset_on_fork);
		if (retval)
			return retval;

		if (attr->sched_flags & SCHED_FLAG_SUGOV)
			return -EINVAL;

		retval = security_task_setscheduler(p);
		if (retval)
			return retval;
	}

	/* Update task specific "requested" clamps */
	/* uclamp static key 可能睡眠，因此必须在 cpuset 和 rq 锁之前验证。 */
	if (attr->sched_flags & SCHED_FLAG_UTIL_CLAMP) {
		retval = uclamp_validate(p, attr);
		if (retval)
			return retval;
	}

	/*
	 * SCHED_DEADLINE bandwidth accounting relies on stable cpusets
	 * information.
	 */
	if (dl_policy(policy) || dl_policy(p->policy)) {
		/* DL 准入依赖稳定 cpuset，锁一直持有到提交或错误回滚完成。 */
		cpuset_locked = true;
		cpuset_lock();
	}

	/*
	 * Make sure no PI-waiters arrive (or leave) while we are
	 * changing the priority of the task:
	 *
	 * To be able to change p->policy safely, the appropriate
	 * runqueue lock must be held.
	 */
	/* task_rq_lock 同时稳定 PI waiter 与 p 所属 rq，之后才能修改 policy。 */
	rq = task_rq_lock(p, &rf);
	update_rq_clock(rq);

	/*
	 * Changing the policy of the stop threads its a very bad idea:
	 */
	if (p == rq->stop) {
		retval = -EINVAL;
		goto unlock;
	}

	retval = scx_check_setscheduler(p, policy);
	if (retval)
		goto unlock;

	/* 完全无变化时只保存 reset-on-fork，避免无意义的出队和 class 回调。 */
	/*
	 * If not changing anything there's no need to proceed further,
	 * but store a possible modification of reset_on_fork.
	 */
	if (unlikely(policy == p->policy)) {
		if (fair_policy(policy) &&
		    (attr->sched_nice != task_nice(p) ||
		     (attr->sched_runtime != p->se.slice)))
			goto change;
		if (rt_policy(policy) && attr->sched_priority != p->rt_priority)
			goto change;
		/* DL 参数或任一 clamp 改变也不能走仅更新 reset-on-fork 的快路径。 */
		if (dl_policy(policy) && dl_param_changed(p, attr))
			goto change;
		if (attr->sched_flags & SCHED_FLAG_UTIL_CLAMP)
			goto change;

		/* 唯一变化只剩 reset-on-fork，可在 rq 锁内直接发布后退出。 */
		p->sched_reset_on_fork = reset_on_fork;
		retval = 0;
		goto unlock;
	}
change:

	if (user) {
#ifdef CONFIG_RT_GROUP_SCHED
		/* 用户不能把 RT task 放进零 runtime 的非 autogroup 调度组。 */
		/*
		 * Do not allow real-time tasks into groups that have no runtime
		 * assigned.
		 */
		if (rt_group_sched_enabled() &&
				rt_bandwidth_enabled() && rt_policy(policy) &&
				task_group(p)->rt_bandwidth.rt_runtime == 0 &&
				!task_group_is_autogroup(task_group(p))) {
			retval = -EPERM;
			goto unlock;
		}
#endif /* CONFIG_RT_GROUP_SCHED */
		/* deadline 的用户请求还需通过 root_domain 范围和非零带宽门禁。 */
		if (dl_bandwidth_enabled() && dl_policy(policy) &&
				!(attr->sched_flags & SCHED_FLAG_SUGOV)) {
			cpumask_t *span = rq->rd->span;

			/* 普通 DL task 必须覆盖整个 root_domain，且域内需配置可用带宽。 */
			/*
			 * Don't allow tasks with an affinity mask smaller than
			 * the entire root_domain to become SCHED_DEADLINE. We
			 * will also fail if there's no bandwidth available.
			 */
			if (!cpumask_subset(span, p->cpus_ptr) ||
			    rq->rd->dl_bw.bw == 0) {
				retval = -EPERM;
				goto unlock;
			}
		}
	}

	/* 锁外解析 SETPARAM_POLICY 后策略可能变化；发现竞态就完整释放并重试。 */
	/* Re-check policy now with rq lock held: */
	if (unlikely(oldpolicy != -1 && oldpolicy != p->policy)) {
		policy = oldpolicy = -1;
		task_rq_unlock(rq, p, &rf);
		if (cpuset_locked)
			cpuset_unlock();
		goto recheck;
	}

	/*
	 * If setscheduling to SCHED_DEADLINE (or changing the parameters
	 * of a SCHED_DEADLINE task) we need to check if enough bandwidth
	 * is available.
	 */
	/* 新旧任一侧属于 DL 都要做带宽记账，超额时不发布任何新参数。 */
	if ((dl_policy(policy) || dl_task(p)) && sched_dl_overflow(p, policy, attr)) {
		retval = -EBUSY;
		goto unlock;
	}

	p->sched_reset_on_fork = reset_on_fork;
	oldprio = p->prio;

	newprio = __normal_prio(policy, attr->sched_priority, attr->sched_nice);
	if (pi) {
		/* PI boost 可能覆盖请求的 normal prio；有效值未变时可省去队列移动。 */
		/*
		 * Take priority boosted tasks into account. If the new
		 * effective priority is unchanged, we just store the new
		 * normal parameters and do not touch the scheduler class and
		 * the runqueue. This will be done when the task deboost
		 * itself.
		 */
		newprio = rt_effective_prio(p, newprio);
		if (newprio == oldprio && !dl_prio(newprio))
			queue_flags &= ~DEQUEUE_MOVE;
	}

	prev_class = p->sched_class;
	next_class = __setscheduler_class(policy, newprio);

	if (prev_class != next_class)
		queue_flags |= DEQUEUE_CLASS;

	/* scope 统一完成 dequeue/put_prev、字段提交、enqueue/set_next 和类回调。 */
	scoped_guard (sched_change, p, queue_flags) {

		if (!(attr->sched_flags & SCHED_FLAG_KEEP_PARAMS)) {
			__setscheduler_params(p, attr);
			p->sched_class = next_class;
			p->prio = newprio;
			__setscheduler_dl_pi(newprio, policy, p, scope);
		}
		__setscheduler_uclamp(p, attr);

		/* 用户视角优先级降低时头插，避免因重排额外延迟当前可运行 task。 */
		if (scope->queued) {
			/*
			 * We enqueue to tail when the priority of a task is
			 * increased (user space view).
			 */
			if (oldprio < p->prio)
				scope->flags |= ENQUEUE_HEAD;
		}
	}

	/* Avoid rq from going away on us: */
	preempt_disable();
	head = splice_balance_callbacks(rq);
	task_rq_unlock(rq, p, &rf);

	/* 先释放 rq，再调整 PI 链；balance callback 必须等 PI 有效状态稳定后运行。 */
	if (pi) {
		if (cpuset_locked)
			cpuset_unlock();
		rt_mutex_adjust_pi(p);
	}

	/* Run balance callbacks after we've adjusted the PI chain: */
	balance_callbacks(rq, head);
	preempt_enable();

	return 0;

unlock:
	/* 所有持锁失败路径在此成对释放 rq 与可选 cpuset 锁。 */
	task_rq_unlock(rq, p, &rf);
	if (cpuset_locked)
		cpuset_unlock();
	return retval;
}

/* 将旧 sched_param ABI 转成 sched_attr，并拆出 policy 高位的 RESET_ON_FORK。 */
/*
 * 业务背景：旧 sched_param ABI 需要转换成统一 sched_attr 请求后复用核心提交路径。
 * 入参：p 是不可空输入/输出 task；policy 是策略或 SETPARAM_POLICY 哨兵；param 是不可空只读 RT 参数；check 决定是否执行用户权限检查。
 * 出参/返回：返回 __sched_setscheduler() 的 0 或负 errno；成功可修改 p，无输出参。
 * 注意事项：p/param 由调用者保证生命周期；该路径允许睡眠并获取调度锁，policy 高位 RESET_ON_FORK 会被拆成 attr 标志。
 */
static int _sched_setscheduler(struct task_struct *p, int policy,
			       const struct sched_param *param, bool check)
{
	struct sched_attr attr = {
		.sched_policy   = policy,
		.sched_priority = param->sched_priority,
		.sched_nice	= PRIO_TO_NICE(p->static_prio),
	};

	/* fair 自定义 slice 需随旧 ABI 请求保留，否则统一 attr 默认值为零。 */
	if (p->se.custom_slice)
		attr.sched_runtime = p->se.slice;

	/* Fixup the legacy SCHED_RESET_ON_FORK hack. */
	if ((policy != SETPARAM_POLICY) && (policy & SCHED_RESET_ON_FORK)) {
		attr.sched_flags |= SCHED_FLAG_RESET_ON_FORK;
		policy &= ~SCHED_RESET_ON_FORK;
		attr.sched_policy = policy;
	}

	return __sched_setscheduler(p, &attr, check, true);
}
/**
 * sched_setscheduler - change the scheduling policy and/or RT priority of a thread.
 * @p: the task in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Use sched_set_fifo(), read its comment.
 *
 * Return: 0 on success. An error code otherwise.
 *
 * NOTE that the task may be already dead.
 */
/* 内核入口但执行用户权限检查；返回 0 或 __sched_setscheduler 的 errno。 */
/*
 * 业务背景：内核调用者使用旧 ABI 修改线程策略时，仍按用户可见权限规则约束请求。
 * 入参：p 是不可空输入/输出 task；policy 是目标策略；param 是不可空只读 sched_param。
 * 出参/返回：成功返回 0，失败返回参数、权限或准入相关负 errno；无输出参。
 * 注意事项：调用者须持有 p 生命周期并允许睡眠；目标即使已退出也可能传入，底层负责一致性而非复活任务。
 */
int sched_setscheduler(struct task_struct *p, int policy,
		       const struct sched_param *param)
{
	return _sched_setscheduler(p, policy, param, true);
}

/* 以扩展 attr 修改 @p，并执行权限/LSM/资源限制检查。 */
/*
 * 业务背景：为内核调用者提供扩展 sched_attr 入口，同时保留与用户请求相同的权限和安全检查。
 * 入参：p 是不可空输入/输出 task；attr 是不可空、字段已由调用者初始化的只读请求。
 * 出参/返回：成功返回 0 并提交调度属性，失败返回负 errno；无输出参。
 * 注意事项：调用者保证两指针生命周期且可睡眠；函数会获取调度相关锁并可能触发 PI 调整。
 */
int sched_setattr(struct task_struct *p, const struct sched_attr *attr)
{
	return __sched_setscheduler(p, attr, true, true);
}

/* 可信内核调用入口，跳过用户权限检查但仍保留参数和调度不变量校验。 */
/*
 * 业务背景：可信内核路径需要设置扩展调度属性，但不应被当前进程的用户凭据限制。
 * 入参：p 是不可空输入/输出 task；attr 是不可空只读请求。
 * 出参/返回：成功返回 0 并更新 p，参数、带宽或调度约束失败返回负 errno；无输出参。
 * 注意事项：跳过 user/LSM 权限检查不等于跳过安全不变量；调用者须持有 p 生命周期并允许睡眠。
 */
int sched_setattr_nocheck(struct task_struct *p, const struct sched_attr *attr)
{
	return __sched_setscheduler(p, attr, false, true);
}
EXPORT_SYMBOL_GPL(sched_setattr_nocheck);

/**
 * sched_setscheduler_nocheck - change the scheduling policy and/or RT priority of a thread from kernel-space.
 * @p: the task in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Just like sched_setscheduler, only don't bother checking if the
 * current context has permission.  For example, this is needed in
 * stop_machine(): we create temporary high priority worker threads,
 * but our caller might not have that capability.
 *
 * Return: 0 on success. An error code otherwise.
 */
/* 可信内核旧 ABI 入口；跳过 capability 检查，成功 0、失败负 errno。 */
/*
 * 业务背景：stop_machine 等内核机制要临时提升线程优先级，不能依赖当前调用者的用户权限。
 * 入参：p 是不可空输入/输出 task；policy 是目标策略；param 是不可空只读 RT 参数。
 * 出参/返回：成功返回 0，参数或调度资源约束失败返回负 errno；无输出参。
 * 注意事项：仅跳过权限检查，仍执行策略、带宽和队列一致性校验；调用者须允许睡眠并保证 p 存活。
 */
int sched_setscheduler_nocheck(struct task_struct *p, int policy,
			       const struct sched_param *param)
{
	return _sched_setscheduler(p, policy, param, false);
}

/*
 * SCHED_FIFO is a broken scheduler model; that is, it is fundamentally
 * incapable of resource management, which is the one thing an OS really should
 * be doing.
 *
 * This is of course the reason it is limited to privileged users only.
 *
 * Worse still; it is fundamentally impossible to compose static priority
 * workloads. You cannot take two correctly working static prio workloads
 * and smash them together and still expect them to work.
 *
 * For this reason 'all' FIFO tasks the kernel creates are basically at:
 *
 *   MAX_RT_PRIO / 2
 *
 * The administrator _MUST_ configure the system, the kernel simply doesn't
 * know enough information to make a sensible choice.
 */
/* 将内核线程设为中等 SCHED_FIFO；失败仅 WARN，资源管理仍由管理员配置。 */
/*
 * 业务背景：内核线程需要高于普通任务的固定 FIFO 优先级时使用统一的中档默认值。
 * 入参：p 是不可空输入/输出 task，调用者须持有其生命周期。
 * 出参/返回：无直接返回值、无输出参；成功把 p 设为 SCHED_FIFO，中途失败仅触发一次 WARN。
 * 注意事项：可能睡眠并获取调度锁；FIFO 不提供资源隔离，调用者不能把该 helper 当作带宽保证。
 */
void sched_set_fifo(struct task_struct *p)
{
	struct sched_param sp = { .sched_priority = MAX_RT_PRIO / 2 };
	WARN_ON_ONCE(sched_setscheduler_nocheck(p, SCHED_FIFO, &sp) != 0);
}
EXPORT_SYMBOL_GPL(sched_set_fifo);

/*
 * For when you don't much care about FIFO, but want to be above SCHED_NORMAL.
 */
/* 将 @p 设为最低 RT FIFO，使其仅高于普通策略；失败仅 WARN。 */
/*
 * 业务背景：内核任务只需略高于普通调度类时，以最低合法 RT 优先级减少抢占影响。
 * 入参：p 是不可空输入/输出 task，调用者保证引用有效。
 * 出参/返回：无直接返回值、无输出参；成功把 p 设为优先级 1 的 SCHED_FIFO，失败仅 WARN。
 * 注意事项：调用可能睡眠；即使最低 FIFO 仍可长期压制普通任务，不能用于等待或资源管理。
 */
void sched_set_fifo_low(struct task_struct *p)
{
	struct sched_param sp = { .sched_priority = 1 };
	WARN_ON_ONCE(sched_setscheduler_nocheck(p, SCHED_FIFO, &sp) != 0);
}
EXPORT_SYMBOL_GPL(sched_set_fifo_low);

/*
 * Used when the primary interrupt handler is forced into a thread, in addition
 * to the (always threaded) secondary handler.  The secondary handler gets a
 * slightly lower priority so that the primary handler can preempt it, thereby
 * emulating the behavior of a non-PREEMPT_RT system where the primary handler
 * runs in hard interrupt context.
 */
/* 为线程化次级 IRQ 设置略低于主处理线程的 FIFO priority。 */
/*
 * 业务背景：PREEMPT_RT 将主、次中断处理都线程化时，次级处理器应允许主处理线程抢占。
 * 入参：p 是不可空输入/输出次级 IRQ task，调用者保证其生命周期。
 * 出参/返回：无直接返回值、无输出参；成功设置 SCHED_FIFO 和预定次级优先级，失败仅 WARN。
 * 注意事项：可能睡眠并获取调度锁；优先级关系依赖主处理线程采用约定的中档 FIFO 值。
 */
void sched_set_fifo_secondary(struct task_struct *p)
{
	struct sched_param sp = { .sched_priority = MAX_RT_PRIO / 2 - 1 };
	WARN_ON_ONCE(sched_setscheduler_nocheck(p, SCHED_FIFO, &sp) != 0);
}

/* 可信内核入口把 @p 切回 SCHED_NORMAL 并设置 @nice；失败仅 WARN。 */
/*
 * 业务背景：内核临时提升任务后需要恢复普通策略并指定静态 nice 值。
 * 入参：p 是不可空输入/输出 task；nice 是目标 nice，调用者应传入 [MIN_NICE,MAX_NICE]。
 * 出参/返回：无直接返回值、无输出参；成功更新 p，失败仅触发一次 WARN。
 * 注意事项：底层仍校验范围和调度不变量且可能睡眠；本 helper 不向调用者传播 errno。
 */
void sched_set_normal(struct task_struct *p, int nice)
{
	struct sched_attr attr = {
		.sched_policy = SCHED_NORMAL,
		.sched_nice = nice,
	};
	WARN_ON_ONCE(sched_setattr_nocheck(p, &attr) != 0);
}
EXPORT_SYMBOL_GPL(sched_set_normal);

/* 复制旧 sched_param、按 pid 取得 task 引用并调用权限检查入口。 */
/*
 * 业务背景：两个旧调度 syscall 共用用户复制、目标查找和权限检查流程。
 * 入参：pid 为非负目标线程号，0 表示 current；policy 是策略或 SETPARAM_POLICY；param 是不可空用户只读指针。
 * 出参/返回：成功返回 0；非法输入、用户复制、目标查找或调度提交失败返回相应负 errno，无输出参。
 * 注意事项：copy_from_user() 可睡眠并失败；CLASS(find_get_task) 管理 task 引用，用户指针只在复制阶段访问。
 */
static int
do_sched_setscheduler(pid_t pid, int policy, struct sched_param __user *param)
{
	struct sched_param lparam;

	/* 先复制完整旧 ABI 结构，后续路径不再持有或重复访问用户指针。 */
	if (unlikely(!param || pid < 0))
		return -EINVAL;
	if (copy_from_user(&lparam, param, sizeof(struct sched_param)))
		return -EFAULT;

	CLASS(find_get_task, p)(pid);
	if (!p)
		return -ESRCH;

	return sched_setscheduler(p, policy, &lparam);
}

/*
 * Mimics kernel/events/core.c perf_copy_attr().
 */
/*
 * 按 size 版本化复制用户 attr；短结构零填充，未知非零尾部拒绝，size 错误
 * 回写内核期望大小并返回 -E2BIG，nice 为兼容旧 ABI 被夹到合法范围。
 */
/*
 * 业务背景：sched_attr 是可扩展用户 ABI，内核必须兼容旧短结构并拒绝无法理解的非零扩展字段。
 * 入参：uattr 是不可空用户只读指针，错误大小时其 size 字段也作为输出；attr 是不可空内核输出缓冲。
 * 出参/返回：成功返回 0 并完整初始化 *attr；用户访问失败返回 -EFAULT，大小不兼容返回 -E2BIG，版本不支持返回 -EINVAL。
 * 注意事项：用户复制可能睡眠；-E2BIG 路径尝试回写期望大小且回写失败不覆盖主错误，短结构未提供字段保持零。
 */
static int sched_copy_attr(struct sched_attr __user *uattr, struct sched_attr *attr)
{
	u32 size;
	int ret;

	/* Zero the full structure, so that a short copy will be nice: */
	memset(attr, 0, sizeof(*attr));

	/* size 本身先单独读取，0 按最早版本解释并限制到一页以内。 */
	ret = get_user(size, &uattr->size);
	if (ret)
		return ret;

	/* ABI compatibility quirk: */
	if (!size)
		size = SCHED_ATTR_SIZE_VER0;
	if (size < SCHED_ATTR_SIZE_VER0 || size > PAGE_SIZE)
		goto err_size;

	/* 通用版本化复制会检查内核未知的用户尾部是否全部为零。 */
	ret = copy_struct_from_user(attr, sizeof(*attr), uattr, size);
	if (ret) {
		if (ret == -E2BIG)
			goto err_size;
		return ret;
	}

	if ((attr->sched_flags & SCHED_FLAG_UTIL_CLAMP) &&
	    size < SCHED_ATTR_SIZE_VER1)
		return -EINVAL;

	/* 历史 ABI 对越界 nice 采取夹取而非拒绝，必须保持兼容。 */
	/*
	 * XXX: Do we want to be lenient like existing syscalls; or do we want
	 * to be strict and return an error on out-of-bounds values?
	 */
	attr->sched_nice = clamp(attr->sched_nice, MIN_NICE, MAX_NICE);

	return 0;

err_size:
	/* 向用户提示当前内核结构大小，但主返回值固定为 -E2BIG。 */
	put_user(sizeof(*attr), &uattr->size);
	return -E2BIG;
}

/* 按 @p 当前 class 把 DL/RT/fair 参数填入调用者 attr，不修改 task。 */
/*
 * 业务背景：getattr 与 KEEP_PARAMS 都需把 task 当前调度类参数归一化为 sched_attr。
 * 入参：p 是不可空只读 task；attr 是不可空输出缓冲；flags 是查询选项并传给 DL 参数读取器。
 * 出参/返回：无直接返回值；按当前策略写 attr 的 DL、RT 或 fair 字段。
 * 注意事项：不取锁且不清空 attr，调用者须提供适当 RCU/锁保护和已初始化缓冲；并发更新时只保证调用点允许的一致性。
 */
static void get_params(struct task_struct *p, struct sched_attr *attr, unsigned int flags)
{
	/* 调度类决定有效字段：DL 自有三元组，RT 仅 priority，fair 使用 nice/slice。 */
	if (task_has_dl_policy(p)) {
		__getparam_dl(p, attr, flags);
	} else if (task_has_rt_policy(p)) {
		attr->sched_priority = p->rt_priority;
	} else {
		attr->sched_nice = task_nice(p);
		attr->sched_runtime = p->se.slice;
	}
}

/**
 * sys_sched_setscheduler - set/change the scheduler policy and RT priority
 * @pid: the pid in question.
 * @policy: new policy.
 * @param: structure containing the new RT priority.
 *
 * Return: 0 on success. An error code otherwise.
 */
/* 用户旧 ABI 设置 policy/RT priority；负 policy 拒绝，其余交共同路径。 */
/*
 * 业务背景：实现 sched_setscheduler(2) 的旧 policy 加 sched_param 用户 ABI。
 * 入参：pid 为非负目标线程号，0 表示 current；policy 是非负目标策略；param 是不可空用户只读参数指针。
 * 出参/返回：成功返回 0，非法参数、用户访问、权限、目标或准入失败返回负 errno；无输出参。
 * 注意事项：可睡眠并访问用户内存；真正的 task 引用、权限检查和锁顺序由共同路径负责。
 */
SYSCALL_DEFINE3(sched_setscheduler, pid_t, pid, int, policy, struct sched_param __user *, param)
{
	if (policy < 0)
		return -EINVAL;

	return do_sched_setscheduler(pid, policy, param);
}

/**
 * sys_sched_setparam - set/change the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the new RT priority.
 *
 * Return: 0 on success. An error code otherwise.
 */
/* 只更新目标的策略参数而保留 policy，以 SETPARAM_POLICY 哨兵进入共同路径。 */
/*
 * 业务背景：实现 sched_setparam(2)，只修改当前策略可用的参数而不切换调度策略。
 * 入参：pid 为非负目标线程号，0 表示 current；param 是不可空用户只读 sched_param。
 * 出参/返回：成功返回 0，复制、查找、权限或参数失败返回负 errno；无输出参。
 * 注意事项：可睡眠并访问用户内存；SETPARAM_POLICY 仅是内核哨兵，最终策略在持锁提交前重读。
 */
SYSCALL_DEFINE2(sched_setparam, pid_t, pid, struct sched_param __user *, param)
{
	return do_sched_setscheduler(pid, SETPARAM_POLICY, param);
}

/**
 * sys_sched_setattr - same as above, but with extended sched_attr
 * @pid: the pid in question.
 * @uattr: structure containing the extended parameters.
 * @flags: for future extension.
 */
/* 扩展 ABI：复制版本化 attr、处理 KEEP 标志、持有目标引用后提交。 */
/*
 * 业务背景：实现 sched_setattr(2)，承载 deadline、uclamp 和可扩展调度属性。
 * 入参：pid 为非负目标线程号；uattr 是不可空用户只读属性指针；flags 当前必须为 0。
 * 出参/返回：成功返回 0；ABI 复制、参数、目标、权限或资源校验失败返回负 errno，无输出参。
 * 注意事项：可能睡眠；KEEP_PARAMS 读取目标现值后仍可能遇到并发变化，最终提交路径负责锁内竞态检查。
 */
SYSCALL_DEFINE3(sched_setattr, pid_t, pid, struct sched_attr __user *, uattr,
			       unsigned int, flags)
{
	struct sched_attr attr;
	int retval;

	if (unlikely(!uattr || pid < 0 || flags))
		return -EINVAL;

	/* 完成用户 ABI 归一化后，后续逻辑仅操作内核 attr 副本。 */
	retval = sched_copy_attr(uattr, &attr);
	if (retval)
		return retval;

	if ((int)attr.sched_policy < 0)
		return -EINVAL;
	if (attr.sched_flags & SCHED_FLAG_KEEP_POLICY)
		attr.sched_policy = SETPARAM_POLICY;

	/* task 引用跨越 KEEP_PARAMS 快照与最终可睡眠提交路径。 */
	CLASS(find_get_task, p)(pid);
	if (!p)
		return -ESRCH;

	if (attr.sched_flags & SCHED_FLAG_KEEP_PARAMS)
		get_params(p, &attr, 0);

	return sched_setattr(p, &attr);
}

/**
 * sys_sched_getscheduler - get the policy (scheduling class) of a thread
 * @pid: the pid in question.
 *
 * Return: On success, the policy of the thread. Otherwise, a negative error
 * code.
 */
/* RCU 查目标并经 LSM 返回 policy，可在高位附带 RESET_ON_FORK。 */
/*
 * 业务背景：实现 sched_getscheduler(2)，向用户报告线程策略及旧 ABI 的 reset-on-fork 标志。
 * 入参：pid 为非负目标线程号，0 表示 current。
 * 出参/返回：成功返回策略值，必要时按位包含 SCHED_RESET_ON_FORK；失败返回 -EINVAL/-ESRCH 或 LSM 负 errno，无输出参。
 * 注意事项：目标只在 RCU 读侧解引用，不获得长期引用；返回的是瞬时快照，退出 RCU 后策略可立即变化。
 */
SYSCALL_DEFINE1(sched_getscheduler, pid_t, pid)
{
	struct task_struct *p;
	int retval;

	if (pid < 0)
		return -EINVAL;

	/* RCU 保护查找及字段读取，LSM 拒绝时直接返回其错误。 */
	guard(rcu)();
	p = find_process_by_pid(pid);
	if (!p)
		return -ESRCH;

	retval = security_task_getscheduler(p);
	if (!retval) {
		/* reset-on-fork 通过旧 ABI 的 policy 高位返回，而不是单独输出字段。 */
		retval = p->policy;
		if (p->sched_reset_on_fork)
			retval |= SCHED_RESET_ON_FORK;
	}
	return retval;
}

/**
 * sys_sched_getparam - get the RT priority of a thread
 * @pid: the pid in question.
 * @param: structure containing the RT priority.
 *
 * Return: On success, 0 and the RT priority is in @param. Otherwise, an error
 * code.
 */
/* RCU 下读取 RT priority，退出 RCU 后才 copy_to_user；非 RT 输出 0。 */
/*
 * 业务背景：实现 sched_getparam(2)，以旧 ABI 返回目标线程的实时优先级。
 * 入参：pid 为非负目标线程号；param 是不可空用户输出指针。
 * 出参/返回：成功返回 0 并写 *param，非 RT task 写优先级 0；查找、LSM 或用户复制失败返回负 errno。
 * 注意事项：task 字段在 RCU 下读取，copy_to_user() 可能睡眠所以在退出 RCU 后执行；结果只是查询时快照。
 */
SYSCALL_DEFINE2(sched_getparam, pid_t, pid, struct sched_param __user *, param)
{
	struct sched_param lp = { .sched_priority = 0 };
	struct task_struct *p;
	int retval;

	if (unlikely(!param || pid < 0))
		return -EINVAL;

	/* RCU 内完成查找、LSM 和 RT 字段快照，用户复制留到临界区外。 */
	scoped_guard (rcu) {
		p = find_process_by_pid(pid);
		if (!p)
			return -ESRCH;

		retval = security_task_getscheduler(p);
		if (retval)
			return retval;

		/* 旧 ABI 对非 RT 策略保持初始化的 priority=0。 */
		if (task_has_rt_policy(p))
			lp.sched_priority = p->rt_priority;
	}

	/*
	 * This one might sleep, we cannot do it with a spinlock held ...
	 */
	return copy_to_user(param, &lp, sizeof(*param)) ? -EFAULT : 0;
}

/**
 * sys_sched_getattr - similar to sched_getparam, but with sched_attr
 * @pid: the pid in question.
 * @uattr: structure containing the extended parameters.
 * @usize: sizeof(attr) for fwd/bwd comp.
 * @flags: for future extension.
 */
/* 读取扩展属性快照并按用户 usize 版本化复制；uclamp 并发读可见旧值或新值。 */
/*
 * 业务背景：实现 sched_getattr(2)，按用户声明大小返回可扩展 sched_attr 快照。
 * 入参：pid 为非负目标线程号；uattr 是不可空用户输出指针；usize 是 [VER0,PAGE_SIZE] 缓冲大小；flags 仅允许受支持的 DL 动态查询标志。
 * 出参/返回：成功返回 0 并写入可容纳字段；非法请求、目标、LSM 或用户复制失败返回负 errno。
 * 注意事项：task 在 RCU 下读取，uclamp 可观察到完整旧值或新值但不保证同一更新批次；用户复制在退出 RCU 后执行并可能睡眠。
 */
SYSCALL_DEFINE4(sched_getattr, pid_t, pid, struct sched_attr __user *, uattr,
		unsigned int, usize, unsigned int, flags)
{
	struct sched_attr kattr = { };
	struct task_struct *p;
	int retval;

	if (unlikely(!uattr || pid < 0 || usize > PAGE_SIZE ||
		     usize < SCHED_ATTR_SIZE_VER0))
		return -EINVAL;

	/* RCU 临界区内构造全内核快照，避免把 task 借用带到用户复制阶段。 */
	scoped_guard (rcu) {
		p = find_process_by_pid(pid);
		if (!p)
			return -ESRCH;

		if (flags) {
			/* 动态查询目前只对 DL task 开放且必须精确匹配唯一已知标志。 */
			if (!task_has_dl_policy(p) ||
			    flags != SCHED_GETATTR_FLAG_DL_DYNAMIC)
				return -EINVAL;
		}

		retval = security_task_getscheduler(p);
		if (retval)
			return retval;

		/* 公共字段与类专用参数先填充，再裁剪不允许从内核泄露的 flag。 */
		kattr.sched_policy = p->policy;
		if (p->sched_reset_on_fork)
			kattr.sched_flags |= SCHED_FLAG_RESET_ON_FORK;
		get_params(p, &kattr, flags);
		kattr.sched_flags &= SCHED_FLAG_ALL;

#ifdef CONFIG_UCLAMP_TASK
		/* clamp 单字段读取不会产生垃圾值，允许并发提交时观察旧或新端点。 */
		/*
		 * This could race with another potential updater, but this is fine
		 * because it'll correctly read the old or the new value. We don't need
		 * to guarantee who wins the race as long as it doesn't return garbage.
		 */
		kattr.sched_util_min = p->uclamp_req[UCLAMP_MIN].value;
		kattr.sched_util_max = p->uclamp_req[UCLAMP_MAX].value;
#endif
	}

	/* size 报告实际可见长度，通用 helper 负责新旧用户结构的尾部规则。 */
	kattr.size = min(usize, sizeof(kattr));
	return copy_struct_to_user(uattr, usize, &kattr, sizeof(kattr), NULL);
}

/*
 * admission control 开启时要求普通 DL task 的新 mask 覆盖整个 root_domain；
 * 非 DL、特殊 DL 或带宽控制关闭时返回 0，不满足返回 -EBUSY。
 */
/*
 * 业务背景：deadline 带宽按 root_domain 准入，普通 DL task 不能通过缩小 affinity 逃离已计入的 CPU 范围。
 * 入参：p 是不可空只读 task；mask 是不可空只读候选 CPU 集合，生命周期覆盖调用。
 * 出参/返回：允许变更返回 0，普通 DL mask 未覆盖 root_domain 时返回 -EBUSY；无输出参。
 * 注意事项：root_domain 在 RCU 下读取；special/sugov DL 和关闭带宽控制时跳过该限制，本函数不提交 affinity。
 */
int dl_task_check_affinity(struct task_struct *p, const struct cpumask *mask)
{
	/* 非普通 DL 实体无需参与 root_domain 带宽覆盖约束。 */
	/*
	 * If the task isn't a deadline task or admission control is
	 * disabled then we don't care about affinity changes.
	 */
	if (!task_has_dl_policy(p) || !dl_bandwidth_enabled())
		return 0;

	/*
	 * The special/sugov task isn't part of regular bandwidth/admission
	 * control so let userspace change affinities.
	 */
	if (dl_entity_is_special(&p->dl))
		return 0;

	/* root_domain 指针受 RCU 保护，候选 mask 必须包含域的完整 span。 */
	/*
	 * Since bandwidth control happens on root_domain basis,
	 * if admission test is enabled, we only admit -deadline
	 * tasks allowed to run on all the CPUs in the task's
	 * root_domain.
	 */
	guard(rcu)();
	if (!cpumask_subset(task_rq(p)->rd->span, mask))
		return -EBUSY;

	return 0;
}

/*
 * 将请求 mask 与 cpuset 相交并提交；提交后重读 cpuset 检测并发收缩，必要时
 * 再限制一次并向用户返回 -EINVAL。临时 mask 在全部出口释放。
 */
/*
 * 业务背景：affinity 请求必须同时服从用户 mask、cpuset 约束和 DL 准入，并处理提交期间 cpuset 并发收缩。
 * 入参：p 是不可空输入/输出 task；ctx 是不可空输入/输出上下文，其 new_mask 必须有效，函数会临时替换该指针并增添 SCA_CHECK。
 * 出参/返回：首次提交且约束稳定返回 0；分配失败 -ENOMEM，DL/提交错误原样返回，cpuset 竞态修正后返回 -EINVAL。
 * 注意事项：会分配内存、睡眠并获取 affinity 锁；临时 mask 仅在函数内有效，调用者不得依赖返回后的 ctx->new_mask 值。
 */
int __sched_setaffinity(struct task_struct *p, struct affinity_context *ctx)
{
	int retval;
	cpumask_var_t cpus_allowed, new_mask;

	/* 两个临时 mask 分别保存 cpuset 快照和本次可提交交集。 */
	if (!alloc_cpumask_var(&cpus_allowed, GFP_KERNEL))
		return -ENOMEM;

	if (!alloc_cpumask_var(&new_mask, GFP_KERNEL)) {
		retval = -ENOMEM;
		goto out_free_cpus_allowed;
	}

	/* 首次提交只能使用用户请求与当前 cpuset 允许范围的交集。 */
	cpuset_cpus_allowed(p, cpus_allowed);
	cpumask_and(new_mask, ctx->new_mask, cpus_allowed);

	ctx->new_mask = new_mask;
	ctx->flags |= SCA_CHECK;

	retval = dl_task_check_affinity(p, new_mask);
	if (retval)
		goto out_free_new_mask;

	/* SCA_CHECK 让底层同时验证迁移和调度器自身的 CPU 可用性约束。 */
	retval = __set_cpus_allowed_ptr(p, ctx);
	if (retval)
		goto out_free_new_mask;

	/* 提交后重读 cpuset；若期间收缩，就按新范围进行一次补偿提交。 */
	cpuset_cpus_allowed(p, cpus_allowed);
	if (!cpumask_subset(new_mask, cpus_allowed)) {
		/*
		 * We must have raced with a concurrent cpuset update.
		 * Just reset the cpumask to the cpuset's cpus_allowed.
		 */
		cpumask_copy(new_mask, cpus_allowed);

		/* 二次 SCA_USER 会恢复旧 user_cpus_ptr，因此还需与旧用户意图求交。 */
		/*
		 * If SCA_USER is set, a 2nd call to __set_cpus_allowed_ptr()
		 * will restore the previous user_cpus_ptr value.
		 *
		 * In the unlikely event a previous user_cpus_ptr exists,
		 * we need to further restrict the mask to what is allowed
		 * by that old user_cpus_ptr.
		 */
		if (unlikely((ctx->flags & SCA_USER) && ctx->user_mask)) {
			bool empty = !cpumask_and(new_mask, new_mask,
						  ctx->user_mask);

			if (empty)
				cpumask_copy(new_mask, cpus_allowed);
		}
		__set_cpus_allowed_ptr(p, ctx);
		retval = -EINVAL;
	}

	/* ctx->new_mask 指向临时对象，释放后调用者不得再解引用该字段。 */
out_free_new_mask:
	free_cpumask_var(new_mask);
out_free_cpus_allowed:
	free_cpumask_var(cpus_allowed);
	return retval;
}

/* 查找并持有目标、检查 owner/capability/LSM，建立 user_mask 后提交 affinity。 */
/*
 * 业务背景：内核公共入口把用户意图转换为带 user_mask 所有权记录的 affinity 变更。
 * 入参：pid 为非负目标线程号，0 表示 current；in_mask 是不可空只读 CPU 集合，调用期间保持有效。
 * 出参/返回：成功返回 0；目标、权限、安全、内存、DL 或 cpuset 约束失败返回负 errno，无输出参。
 * 注意事项：可能睡眠；函数临时持有 task 引用并拥有新分配 user_mask，提交后无论成功失败均释放尚未转移的内存。
 */
long sched_setaffinity(pid_t pid, const struct cpumask *in_mask)
{
	struct affinity_context ac;
	struct cpumask *user_mask;
	int retval;

	/* 自动清理类稳定 task 生命周期，PF_NO_SETAFFINITY 是目标的硬拒绝标志。 */
	CLASS(find_get_task, p)(pid);
	if (!p)
		return -ESRCH;

	if (p->flags & PF_NO_SETAFFINITY)
		return -EINVAL;

	if (!check_same_owner(p)) {
		/* 跨用户操作要求目标 user namespace 内的 CAP_SYS_NICE。 */
		guard(rcu)();
		if (!ns_capable(__task_cred(p)->user_ns, CAP_SYS_NICE))
			return -EPERM;
	}

	retval = security_task_setscheduler(p);
	if (retval)
		return retval;

	/* user_mask 记录原始用户意图，供 cpuset 日后放宽时恢复可用 CPU。 */
	/*
	 * With non-SMP configs, user_cpus_ptr/user_mask isn't used and
	 * alloc_user_cpus_ptr() returns NULL.
	 */
	user_mask = alloc_user_cpus_ptr(NUMA_NO_NODE);
	if (user_mask) {
		cpumask_copy(user_mask, in_mask);
	} else {
		return -ENOMEM;
	}

	/* context 同时携带本次候选 mask 和以后恢复 affinity 所需的用户原始 mask。 */
	ac = (struct affinity_context){
		.new_mask  = in_mask,
		.user_mask = user_mask,
		.flags     = SCA_USER,
	};

	/* 提交 helper 可转移/替换 user_mask，最终按返回后的 ac 所有权释放。 */
	retval = __sched_setaffinity(p, &ac);
	kfree(ac.user_mask);

	return retval;
}

/* 从用户复制最多内核 cpumask 大小；短输入先清高位，失败返回 -EFAULT。 */
/*
 * 业务背景：不同用户 ABI 长度的 CPU 位图要安全归一化为固定大小的内核 cpumask。
 * 入参：user_mask_ptr 是用户只读位图指针且应可访问 len 字节；len 是字节数；new_mask 是不可空内核输出缓冲。
 * 出参/返回：复制成功返回 0 并写 *new_mask，用户访问失败返回 -EFAULT。
 * 注意事项：只复制 min(len,cpumask_size())；短输入先清零高位，长输入被截断，copy_from_user() 可能睡眠。
 */
static int get_user_cpu_mask(unsigned long __user *user_mask_ptr, unsigned len,
			     struct cpumask *new_mask)
{
	if (len < cpumask_size())
		cpumask_clear(new_mask);
	else if (len > cpumask_size())
		len = cpumask_size();

	return copy_from_user(new_mask, user_mask_ptr, len) ? -EFAULT : 0;
}

/**
 * sys_sched_setaffinity - set the CPU affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to the new CPU mask
 *
 * Return: 0 on success. An error code otherwise.
 */
/* 分配内核 mask、复制用户位图并调用权限/cpuset 约束路径；所有出口释放 mask。 */
/*
 * 业务背景：实现 sched_setaffinity(2)，把用户位图转换为内核 mask 后执行统一权限和 cpuset 校验。
 * 入参：pid 为目标线程号；len 是用户位图字节数；user_mask_ptr 是用户只读位图指针。
 * 出参/返回：成功返回 0；分配、复制、目标、权限或约束失败返回负 errno，无输出参。
 * 注意事项：会分配内存并可能睡眠；超出内核 mask 的尾部被忽略，临时 cpumask 在所有出口释放。
 */
SYSCALL_DEFINE3(sched_setaffinity, pid_t, pid, unsigned int, len,
		unsigned long __user *, user_mask_ptr)
{
	cpumask_var_t new_mask;
	int retval;

	/* 临时 mask 在 syscall 生命周期内拥有，复制或提交失败也统一释放。 */
	if (!alloc_cpumask_var(&new_mask, GFP_KERNEL))
		return -ENOMEM;

	retval = get_user_cpu_mask(user_mask_ptr, len, new_mask);
	if (retval == 0)
		retval = sched_setaffinity(pid, new_mask);
	free_cpumask_var(new_mask);
	return retval;
}

/* RCU/LSM 校验后在 pi_lock 下输出 task 允许且 CPU active 的交集。 */
/*
 * 业务背景：公共查询入口只向调用者暴露目标允许且当前在线可调度的 CPU 集合。
 * 入参：pid 为非负目标线程号，0 表示 current；mask 是不可空内核输出缓冲。
 * 出参/返回：成功返回 0 并写 *mask；目标不存在或 LSM 拒绝返回负 errno。
 * 注意事项：task 在 RCU 下查找并在 pi_lock irqsave 下读取 affinity；输出是瞬时快照，函数不访问用户内存。
 */
long sched_getaffinity(pid_t pid, struct cpumask *mask)
{
	struct task_struct *p;
	int retval;

	/* RCU 稳定目标指针，随后 LSM 决定调用者是否可观察其调度信息。 */
	guard(rcu)();
	p = find_process_by_pid(pid);
	if (!p)
		return -ESRCH;

	retval = security_task_getscheduler(p);
	if (retval)
		return retval;

	/* pi_lock 使 cpus_mask 与迁移状态一致，再过滤已经不 active 的 CPU。 */
	guard(raw_spinlock_irqsave)(&p->pi_lock);
	cpumask_and(mask, &p->cpus_mask, cpu_active_mask);

	return 0;
}

/**
 * sys_sched_getaffinity - get the CPU affinity of a process
 * @pid: pid of the process
 * @len: length in bytes of the bitmask pointed to by user_mask_ptr
 * @user_mask_ptr: user-space pointer to hold the current CPU mask
 *
 * Return: size of CPU mask copied to user_mask_ptr on success. An
 * error code otherwise.
 */
/* 校验用户缓冲长度/对齐，读取 affinity 后复制并返回实际字节数。 */
/*
 * 业务背景：实现 sched_getaffinity(2)，按用户缓冲能力返回当前有效 CPU 位图。
 * 入参：pid 为目标线程号；len 是按 unsigned long 对齐且足以容纳 nr_cpu_ids 的字节数；user_mask_ptr 是用户输出指针。
 * 出参/返回：成功返回实际复制字节数并写用户位图；参数、分配、查找、LSM 或复制失败返回负 errno。
 * 注意事项：会分配内存并访问用户空间，因而可能睡眠；返回长度不超过 cpumask_size()，结果是查询瞬时快照。
 */
SYSCALL_DEFINE3(sched_getaffinity, pid_t, pid, unsigned int, len,
		unsigned long __user *, user_mask_ptr)
{
	int ret;
	cpumask_var_t mask;

	/* ABI 要求缓冲覆盖所有可能 CPU 且长度按机器字对齐。 */
	if ((len * BITS_PER_BYTE) < nr_cpu_ids)
		return -EINVAL;
	if (len & (sizeof(unsigned long)-1))
		return -EINVAL;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
		return -ENOMEM;

	/* 内核查询成功后只复制用户缓冲与内核 mask 中较小的长度。 */
	ret = sched_getaffinity(pid, mask);
	if (ret == 0) {
		unsigned int retlen = min(len, cpumask_size());

		if (copy_to_user(user_mask_ptr, cpumask_bits(mask), retlen))
			ret = -EFAULT;
		else
			ret = retlen;
	}
	/* 无论查询还是复制失败，临时零初始化 mask 都在返回前释放。 */
	free_cpumask_var(mask);

	return ret;
}

/* 在本 rq 锁下调用当前 class yield hook，解锁后 schedule；yield 不保证换人运行。 */
/*
 * 业务背景：yield 的共同实现通知当前调度类放弃本轮机会，再主动进入一次调度。
 * 入参：无。
 * 出参/返回：无直接返回值、无输出参；可能更新 rq yield 统计并触发任务切换。
 * 注意事项：内部以 irqsave 方式锁当前 rq，释放锁后才 schedule()；不保证其他任务运行，更不能提供进度或同步语义。
 */
static void do_sched_yield(void)
{
	struct rq_flags rf;
	struct rq *rq;

	/* 关中断锁本地 rq，防止当前实体和 class hook 在操作中变化。 */
	rq = this_rq_lock_irq(&rf);

	schedstat_inc(rq->yld_count);
	rq->donor->sched_class->yield_task(rq);

	/* 解锁到 schedule 之间保持禁止抢占，避免丢失刚建立的让渡状态。 */
	preempt_disable();
	rq_unlock_irq(rq, &rf);
	sched_preempt_enable_no_resched();

	schedule();
}

/**
 * sys_sched_yield - yield the current processor to other threads.
 *
 * This function yields the current CPU to other tasks. If there are no
 * other threads running on this CPU then this function will return.
 *
 * Return: 0.
 */
/* 用户 yield syscall；执行一次调度尝试后恒返回 0。 */
/*
 * 业务背景：实现 sched_yield(2)，让当前任务向其调度类声明本轮让出处理器。
 * 入参：无。
 * 出参/返回：恒返回 0，无输出参；可能发生一次上下文切换。
 * 注意事项：不保证换到其他任务，也不保证等待条件推进；调用者不能把它当作阻塞或同步原语。
 */
SYSCALL_DEFINE0(sched_yield)
{
	do_sched_yield();
	return 0;
}

/**
 * yield - yield the current processor to other threads.
 *
 * Do not ever use this function, there's a 99% chance you're doing it wrong.
 *
 * The scheduler is at all times free to pick the calling task as the most
 * eligible task to run, if removing the yield() call from your code breaks
 * it, it's already broken.
 *
 * Typical broken usage is:
 *
 * while (!event)
 *	yield();
 *
 * where one assumes that yield() will let 'the other' process run that will
 * make event true. If the current task is a SCHED_FIFO task that will never
 * happen. Never use yield() as a progress guarantee!!
 *
 * If you want to use yield() to wait for something, use wait_event().
 * If you want to use yield() to be 'nice' for others, use cond_resched().
 * If you still want to use yield(), do not!
 */
/* 内核 yield 包装：保持 TASK_RUNNING 并尝试重调度，不能作为等待进度保证。 */
/*
 * 业务背景：内核内部兼容入口把 current 保持为可运行状态后执行一次 yield。
 * 入参：无，隐式作用对象是 current。
 * 出参/返回：无直接返回值、无输出参；可能触发上下文切换。
 * 注意事项：可调度上下文才能调用；不提供进度保证，等待事件应使用 wait_event，礼让应优先 cond_resched。
 */
void __sched yield(void)
{
	set_current_state(TASK_RUNNING);
	do_sched_yield();
}
EXPORT_SYMBOL(yield);

/**
 * yield_to - yield the current processor to another thread in
 * your thread group, or accelerate that thread toward the
 * processor it's on.
 * @p: target task
 * @preempt: whether task preemption is allowed or not
 *
 * It's the caller's job to ensure that the target task struct
 * can't go away on us before we can do any checks.
 *
 * Return:
 *	true (>0) if we indeed boosted the target task.
 *	false (0) if we failed to boost the target.
 *	-ESRCH if there's no task to yield to.
 */
/*
 * 在 @p pi_lock 与双 rq 锁下验证同 class/可运行/未在 CPU，再调用 class
 * yield_to hook；成功可 resched 远端并由当前 task schedule，返回 1/0/-ESRCH。
 */
/*
 * 业务背景：允许当前任务定向提升同调度类目标的运行机会，用于已有明确目标的协作式让渡。
 * 入参：p 是不可空目标 task，调用者必须持有其生命周期；preempt 表示跨 rq 成功时是否立即请求目标 CPU 重调度。
 * 出参/返回：成功提升返回正值，无可用 hook/目标状态不合适返回 0，无有意义让渡对象返回 -ESRCH；无输出参。
 * 注意事项：内部获取 p->pi_lock 和双 rq 锁并可能 schedule()；仅同 class 生效，返回成功也不构成目标完成工作的同步保证。
 */
int __sched yield_to(struct task_struct *p, bool preempt)
{
	struct task_struct *curr;
	struct rq *rq, *p_rq;
	int yielded = 0;

	/* pi_lock 稳定目标的 rq/优先级关联，双 rq 锁再稳定双方队列。 */
	scoped_guard (raw_spinlock_irqsave, &p->pi_lock) {
		rq = this_rq();
		curr = rq->donor;

again:
		p_rq = task_rq(p);
		/* 双方都只有一个 runnable task 时没有可交换的调度机会。 */
		/*
		 * If we're the only runnable task on the rq and target rq also
		 * has only one task, there's absolutely no point in yielding.
		 */
		if (rq->nr_running == 1 && p_rq->nr_running == 1)
			return -ESRCH;

		guard(double_rq_lock)(rq, p_rq);
		if (task_rq(p) != p_rq)
			goto again;

		/* 定向让渡要求 class 提供 hook，且当前与目标必须属于同一 class。 */
		if (!curr->sched_class->yield_to_task)
			return 0;

		if (curr->sched_class != p->sched_class)
			return 0;

		if (task_on_cpu(p_rq, p) || !task_is_running(p))
			return 0;

		/* hook 成功后记统计；跨 rq 且允许抢占时通知目标 CPU 尽快重选。 */
		yielded = curr->sched_class->yield_to_task(rq, p);
		if (yielded) {
			schedstat_inc(rq->yld_count);
			/*
			 * Make p's CPU reschedule; pick_next_entity
			 * takes care of fairness.
			 */
			if (preempt && rq != p_rq)
				resched_curr(p_rq);
		}
	}

	/* 只有 hook 明确成功才让 current 进入调度，锁已由 scope 全部释放。 */
	if (yielded)
		schedule();

	return yielded;
}
EXPORT_SYMBOL_GPL(yield_to);

/**
 * sys_sched_get_priority_max - return maximum RT priority.
 * @policy: scheduling class.
 *
 * Return: On success, this syscall returns the maximum
 * rt_priority that can be used by a given scheduling class.
 * On failure, a negative error code is returned.
 */
/* 返回指定 policy 的最大用户 RT priority；未知策略 -EINVAL。 */
/*
 * 业务背景：实现 POSIX 调度接口查询某策略可接受的最大 sched_priority。
 * 入参：policy 是 SCHED_FIFO/RR/DL/NORMAL/BATCH/IDLE/EXT 之一。
 * 出参/返回：FIFO/RR 返回 MAX_RT_PRIO-1，其他已知策略返回 0，未知策略返回 -EINVAL；无输出参。
 * 注意事项：纯查询，不取锁、不睡眠；返回的是用户 ABI 优先级范围而非内核内部 prio 编码。
 */
SYSCALL_DEFINE1(sched_get_priority_max, int, policy)
{
	int ret = -EINVAL;

	/* 仅 FIFO/RR 使用非零 sched_priority，其余已知策略的唯一范围是 0。 */
	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = MAX_RT_PRIO-1;
		break;
	/* 非 RT 策略不消费 sched_priority，因此最大值与最小值均为零。 */
	case SCHED_DEADLINE:
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_IDLE:
	case SCHED_EXT:
		ret = 0;
		break;
	}
	return ret;
}

/**
 * sys_sched_get_priority_min - return minimum RT priority.
 * @policy: scheduling class.
 *
 * Return: On success, this syscall returns the minimum
 * rt_priority that can be used by a given scheduling class.
 * On failure, a negative error code is returned.
 */
/* 返回指定 policy 的最小用户 RT priority；非 RT 为 0，未知策略 -EINVAL。 */
/*
 * 业务背景：实现 POSIX 调度接口查询某策略可接受的最小 sched_priority。
 * 入参：policy 是 SCHED_FIFO/RR/DL/NORMAL/BATCH/IDLE/EXT 之一。
 * 出参/返回：FIFO/RR 返回 1，其他已知策略返回 0，未知策略返回 -EINVAL；无输出参。
 * 注意事项：纯查询，不取锁、不睡眠；0 对非 RT 策略表示该字段必须为零。
 */
SYSCALL_DEFINE1(sched_get_priority_min, int, policy)
{
	int ret = -EINVAL;

	/* RT 策略最小值为 1，非 RT 策略仍以 0 表示不使用该字段。 */
	switch (policy) {
	case SCHED_FIFO:
	case SCHED_RR:
		ret = 1;
		break;
	/* DL 参数使用 runtime/deadline/period，其他普通类使用 nice 或内部规则。 */
	case SCHED_DEADLINE:
	case SCHED_NORMAL:
	case SCHED_BATCH:
	case SCHED_IDLE:
	case SCHED_EXT:
		ret = 0;
	}
	return ret;
}

/* 查 task、LSM 校验并在 rq 锁下调用 class interval hook，输出 timespec64。 */
/*
 * 业务背景：RR 时间片查询需在目标 rq 锁下调用调度类 hook，再转换为稳定的 timespec64 ABI 值。
 * 入参：pid 为非负目标线程号，0 表示 current；t 是不可空内核输出缓冲。
 * 出参/返回：成功返回 0 并写 *t，0 时长表示无限；非法 pid、目标或 LSM 失败返回负 errno。
 * 注意事项：task 在 RCU 下查找，时间片在 task_rq_lock 下读取；只返回瞬时配置，不保证后续实际运行时长。
 */
static int sched_rr_get_interval(pid_t pid, struct timespec64 *t)
{
	unsigned int time_slice = 0;
	int retval;

	if (pid < 0)
		return -EINVAL;

	/* RCU 稳定 task，rq 锁确保 class 与对应 interval hook 同属一个快照。 */
	scoped_guard (rcu) {
		struct task_struct *p = find_process_by_pid(pid);
		if (!p)
			return -ESRCH;

		retval = security_task_getscheduler(p);
		if (retval)
			return retval;

		/* 没有 hook 的调度类保留 time_slice=0，按 ABI 表示无限。 */
		scoped_guard (task_rq_lock, p) {
			struct rq *rq = scope.rq;
			if (p->sched_class->get_rr_interval)
				time_slice = p->sched_class->get_rr_interval(rq, p);
		}
	}

	/* 锁外执行单位转换，输出缓冲仅在成功路径写入。 */
	jiffies_to_timespec64(time_slice, t);
	return 0;
}

/**
 * sys_sched_rr_get_interval - return the default time-slice of a process.
 * @pid: pid of the process.
 * @interval: userspace pointer to the time-slice value.
 *
 * this syscall writes the default time-slice value of a given process
 * into the user-space timespec buffer. A value of '0' means infinity.
 *
 * Return: On success, 0 and the time-slice is in @interval. Otherwise,
 * an error code.
 */
/* 获取 timespec64 interval 后复制到用户；0 表示无限时间片。 */
/*
 * 业务背景：实现原生时间 ABI 的 sched_rr_get_interval(2)。
 * 入参：pid 为目标线程号；interval 是不可空用户输出 timespec 指针。
 * 出参/返回：成功返回 0 并写 *interval；查询或用户复制失败返回负 errno。
 * 注意事项：用户复制可能睡眠；即使非 RR 策略也可成功返回 0 时长，表示调度类未定义有限轮转片。
 */
SYSCALL_DEFINE2(sched_rr_get_interval, pid_t, pid,
		struct __kernel_timespec __user *, interval)
{
	struct timespec64 t;
	int retval = sched_rr_get_interval(pid, &t);

	if (retval == 0)
		retval = put_timespec64(&t, interval);

	return retval;
}

#ifdef CONFIG_COMPAT_32BIT_TIME
/* 32 位时间兼容入口；共用查询逻辑，再转换为 old_timespec32。 */
/*
 * 业务背景：为 32 位旧时间 ABI 复用同一 RR 时间片查询，并在边界处转换结构。
 * 入参：pid 为目标线程号；interval 是不可空用户输出 old_timespec32 指针。
 * 出参/返回：成功返回 0 并写 *interval；查询、时间转换或用户复制失败返回负 errno。
 * 注意事项：仅 CONFIG_COMPAT_32BIT_TIME 下存在；用户访问可能睡眠，转换须保留 0 表示无限的语义。
 */
SYSCALL_DEFINE2(sched_rr_get_interval_time32, pid_t, pid,
		struct old_timespec32 __user *, interval)
{
	struct timespec64 t;
	int retval = sched_rr_get_interval(pid, &t);

	if (retval == 0)
		retval = put_old_timespec32(&t, interval);
	return retval;
}
#endif
