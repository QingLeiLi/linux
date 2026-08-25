// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2010-2017 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * membarrier system call
 */
#include <uapi/linux/membarrier.h>
#include "sched.h"

/*
 * For documentation purposes, here are some membarrier ordering
 * scenarios to keep in mind:
 *
 * A) Userspace thread execution after IPI vs membarrier's memory
 *    barrier before sending the IPI
 *
 * Userspace variables:
 *
 * int x = 0, y = 0;
 *
 * The memory barrier at the start of membarrier() on CPU0 is necessary in
 * order to enforce the guarantee that any writes occurring on CPU0 before
 * the membarrier() is executed will be visible to any code executing on
 * CPU1 after the IPI-induced memory barrier:
 *
 *         CPU0                              CPU1
 *
 *         x = 1
 *         membarrier():
 *           a: smp_mb()
 *           b: send IPI                       IPI-induced mb
 *           c: smp_mb()
 *         r2 = y
 *                                           y = 1
 *                                           barrier()
 *                                           r1 = x
 *
 *                     BUG_ON(r1 == 0 && r2 == 0)
 *
 * The write to y and load from x by CPU1 are unordered by the hardware,
 * so it's possible to have "r1 = x" reordered before "y = 1" at any
 * point after (b).  If the memory barrier at (a) is omitted, then "x = 1"
 * can be reordered after (a) (although not after (c)), so we get r1 == 0
 * and r2 == 0.  This violates the guarantee that membarrier() is
 * supposed by provide.
 *
 * The timing of the memory barrier at (a) has to ensure that it executes
 * before the IPI-induced memory barrier on CPU1.
 *
 * B) Userspace thread execution before IPI vs membarrier's memory
 *    barrier after completing the IPI
 *
 * Userspace variables:
 *
 * int x = 0, y = 0;
 *
 * The memory barrier at the end of membarrier() on CPU0 is necessary in
 * order to enforce the guarantee that any writes occurring on CPU1 before
 * the membarrier() is executed will be visible to any code executing on
 * CPU0 after the membarrier():
 *
 *         CPU0                              CPU1
 *
 *                                           x = 1
 *                                           barrier()
 *                                           y = 1
 *         r2 = y
 *         membarrier():
 *           a: smp_mb()
 *           b: send IPI                       IPI-induced mb
 *           c: smp_mb()
 *         r1 = x
 *         BUG_ON(r1 == 0 && r2 == 1)
 *
 * The writes to x and y are unordered by the hardware, so it's possible to
 * have "r2 = 1" even though the write to x doesn't execute until (b).  If
 * the memory barrier at (c) is omitted then "r1 = x" can be reordered
 * before (b) (although not before (a)), so we get "r1 = 0".  This violates
 * the guarantee that membarrier() is supposed to provide.
 *
 * The timing of the memory barrier at (c) has to ensure that it executes
 * after the IPI-induced memory barrier on CPU1.
 *
 * C) Scheduling userspace thread -> kthread -> userspace thread vs membarrier
 *
 *           CPU0                            CPU1
 *
 *           membarrier():
 *           a: smp_mb()
 *                                           d: switch to kthread (includes mb)
 *           b: read rq->curr->mm == NULL
 *                                           e: switch to user (includes mb)
 *           c: smp_mb()
 *
 * Using the scenario from (A), we can show that (a) needs to be paired
 * with (e). Using the scenario from (B), we can show that (c) needs to
 * be paired with (d).
 *
 * D) exit_mm vs membarrier
 *
 * Two thread groups are created, A and B.  Thread group B is created by
 * issuing clone from group A with flag CLONE_VM set, but not CLONE_THREAD.
 * Let's assume we have a single thread within each thread group (Thread A
 * and Thread B).  Thread A runs on CPU0, Thread B runs on CPU1.
 *
 *           CPU0                            CPU1
 *
 *           membarrier():
 *             a: smp_mb()
 *                                           exit_mm():
 *                                             d: smp_mb()
 *                                             e: current->mm = NULL
 *             b: read rq->curr->mm == NULL
 *             c: smp_mb()
 *
 * Using scenario (B), we can show that (c) needs to be paired with (d).
 *
 * E) kthread_{use,unuse}_mm vs membarrier
 *
 *           CPU0                            CPU1
 *
 *           membarrier():
 *           a: smp_mb()
 *                                           kthread_unuse_mm()
 *                                             d: smp_mb()
 *                                             e: current->mm = NULL
 *           b: read rq->curr->mm == NULL
 *                                           kthread_use_mm()
 *                                             f: current->mm = mm
 *                                             g: smp_mb()
 *           c: smp_mb()
 *
 * Using the scenario from (A), we can show that (a) needs to be paired
 * with (g). Using the scenario from (B), we can show that (c) needs to
 * be paired with (d).
 */

/*
 * 上述 A/B 证明发 IPI 前后的两道完整屏障都不可省：前一道把调用者先前访问排在远端
 * IPI 屏障之前，后一道把远端先前访问排在调用者后续访问之前。C/D/E 说明即使扫描时
 * 看到 rq->curr->mm 为 NULL，也必须依靠调度切换、exit_mm 和 kthread use/unuse mm 的
 * 配对屏障闭合顺序；因此“跳过内核线程”不是没有同步，而是同步责任转交给切换边界。
 */

/*
 * Bitmask made from a "or" of all commands within enum membarrier_cmd,
 * except MEMBARRIER_CMD_QUERY.
 */
#ifdef CONFIG_ARCH_HAS_MEMBARRIER_SYNC_CORE
#define MEMBARRIER_PRIVATE_EXPEDITED_SYNC_CORE_BITMASK			\
	(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE			\
	| MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE)
#else
#define MEMBARRIER_PRIVATE_EXPEDITED_SYNC_CORE_BITMASK	0
#endif

#ifdef CONFIG_RSEQ
#define MEMBARRIER_PRIVATE_EXPEDITED_RSEQ_BITMASK		\
	(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ			\
	| MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ)
#else
#define MEMBARRIER_PRIVATE_EXPEDITED_RSEQ_BITMASK	0
#endif

#define MEMBARRIER_CMD_BITMASK						\
	(MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_GLOBAL_EXPEDITED	\
	| MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED			\
	| MEMBARRIER_CMD_PRIVATE_EXPEDITED				\
	| MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED			\
	| MEMBARRIER_PRIVATE_EXPEDITED_SYNC_CORE_BITMASK		\
	| MEMBARRIER_PRIVATE_EXPEDITED_RSEQ_BITMASK			\
	| MEMBARRIER_CMD_GET_REGISTRATIONS)

/*
 * Scoped guard for memory barriers on entry and exit.
 * Matches memory barriers before & after rq->curr modification in scheduler.
 */
/*
 * mb guard 在作用域进入和任意退出路径各执行一次 smp_mb()，与调度器修改 rq->curr 前后
 * 的屏障配对。全局 mutex 串行整批 IPI，per-CPU mutex 串行只针对一个 CPU 的 rseq IPI，
 * 防止多个请求对同一观察窗口交错。
 */
DEFINE_LOCK_GUARD_0(mb, smp_mb(), smp_mb())
static DEFINE_MUTEX(membarrier_ipi_mutex);
static DEFINE_PER_CPU(struct mutex, membarrier_cpu_mutexes);

#define SERIALIZE_IPI() guard(mutex)(&membarrier_ipi_mutex)
#define SERIALIZE_IPI_CPU(cpu_id) guard(mutex)(&per_cpu(membarrier_cpu_mutexes, cpu_id))

/* core init 阶段初始化所有 possible CPU 的串行 mutex；无失败路径，也不依赖 CPU 在线。 */
static int __init membarrier_init(void)
{
	int i;

	for_each_possible_cpu(i)
		mutex_init(&per_cpu(membarrier_cpu_mutexes, i));
	return 0;
}
core_initcall(membarrier_init);

/* 普通 expedited IPI 回调只提供完整屏障；info 未使用，等待型发送保证回调完成后返回。 */
static void ipi_mb(void *info)
{
	smp_mb();	/* IPIs should be serializing but paranoid. */
}

/*
 * SYNC_CORE IPI 先显式提供内存屏障，再请求返回用户态前同步指令执行上下文。显式屏障
 * 不能只依赖可能延迟的 sync_core_before_usermode，否则会落到调用方结束屏障之后。
 */
static void ipi_sync_core(void *info)
{
	/*
	 * The smp_mb() in membarrier after all the IPIs is supposed to
	 * ensure that memory on remote CPUs that occur before the IPI
	 * become visible to membarrier()'s caller -- see scenario B in
	 * the big comment at the top of this file.
	 *
	 * A sync_core() would provide this guarantee, but
	 * sync_core_before_usermode() might end up being deferred until
	 * after membarrier()'s smp_mb().
	 */
	/* 原文结论：远端 IPI 前的访问必须在发起者的结束屏障前可见，延迟 core sync 不足以保证。 */
	smp_mb();	/* IPIs should be serializing but paranoid. */

	sync_core_before_usermode();
}

/*
 * RSEQ IPI 先让发起线程此前写入对被中断任务可见，再按 ABI 模式使临界区重启：v2
 * 记录一次调度切换事件，legacy 立即重写 CPU/节点 id 并检查临界区。
 */
static void ipi_rseq(void *info)
{
	/*
	 * Ensure that all stores done by the calling thread are visible
	 * to the current task before the current task resumes.  We could
	 * probably optimize this away on most architectures, but by the
	 * time we've already sent an IPI, the cost of the extra smp_mb()
	 * is negligible.
	 */
	/* 多数架构或可省略，但 IPI 已是主成本，保留屏障换取清晰的可见性保证。 */
	smp_mb();
	/*
	 * Legacy mode requires that IDs are written and the critical section is
	 * evaluated. V2 optimized mode handles the critical section and IDs are
	 * only updated if they change as a consequence of preemption after
	 * return from this IPI.
	 */
	/* legacy 强制更新 id 和临界区；v2 只在随后抢占确实改变状态时更新 id。 */
	if (rseq_v2(current))
		rseq_sched_switch_event(current);
	else
		rseq_force_update();
}

/*
 * 在目标 CPU 上把当前 mm 的注册状态复制到本 rq；若 CPU 已切换到别的 mm 则无需写。
 * 写后屏障保证注册之后的用户访问不会被重排到状态发布之前。
 */
static void ipi_sync_rq_state(void *info)
{
	struct mm_struct *mm = (struct mm_struct *) info;

	if (current->mm != mm)
		return;
	this_cpu_write(runqueues.membarrier_state,
		       atomic_read(&mm->membarrier_state));
	/*
	 * Issue a memory barrier after setting
	 * MEMBARRIER_STATE_GLOBAL_EXPEDITED in the current runqueue to
	 * guarantee that no memory access following registration is reordered
	 * before registration.
	 */
	smp_mb();
}

/*
 * exec 建立新地址空间语义时清空继承的所有注册：清零前屏障把旧映像的访问留在边界
 * 之前，并同步清当前 CPU rq 快照。调用者在 mm 切换/exec 串行协议内持有该对象。
 */
void membarrier_exec_mmap(struct mm_struct *mm)
{
	/*
	 * Issue a memory barrier before clearing membarrier_state to
	 * guarantee that no memory access prior to exec is reordered after
	 * clearing this state.
	 */
	smp_mb();
	atomic_set(&mm->membarrier_state, 0);
	/*
	 * Keep the runqueue membarrier_state in sync with this mm
	 * membarrier_state.
	 */
	this_cpu_write(runqueues.membarrier_state, 0);
}

/*
 * 当前任务切换/借用 mm 时，把 next_mm 的原子注册位发布到本 rq；NULL 对应内核线程
 * 状态 0。值未变快速返回，READ/WRITE_ONCE 防止编译器合并与并发观察不一致。
 */
void membarrier_update_current_mm(struct mm_struct *next_mm)
{
	struct rq *rq = this_rq();
	int membarrier_state = 0;

	if (next_mm)
		membarrier_state = atomic_read(&next_mm->membarrier_state);
	if (READ_ONCE(rq->membarrier_state) == membarrier_state)
		return;
	WRITE_ONCE(rq->membarrier_state, membarrier_state);
}

/*
 * 向所有在线且 rq 已登记 GLOBAL_EXPEDITED、当前运行用户 mm 的远端 CPU 发送屏障 IPI。
 * 单 CPU 快速成功；cpumask 分配失败返回 -ENOMEM。mb guard 包围扫描和等待，IPI mutex
 * 串行并发全局请求，cpus_read_lock 稳定在线集合，RCU 稳定 rq->curr 借用指针。
 */
static int membarrier_global_expedited(void)
{
	cpumask_var_t __free(free_cpumask_var) tmpmask = CPUMASK_VAR_NULL;
	int cpu;

	if (num_online_cpus() == 1)
		return 0;

	if (!zalloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;

	guard(mb)();
	SERIALIZE_IPI();
	guard(cpus_read_lock)();

	rcu_read_lock();
	for_each_online_cpu(cpu) {
		struct task_struct *p;

		/*
		 * Skipping the current CPU is OK even through we can be
		 * migrated at any point. The current CPU, at the point
		 * where we read raw_smp_processor_id(), is ensured to
		 * be in program order with respect to the caller
		 * thread. Therefore, we can skip this CPU from the
		 * iteration.
		 */
		/*
		 * 发起线程即使随后迁移，读取 raw CPU 的动作仍与其程序序有序；当前 CPU
		 * 由 guard 的本地屏障覆盖，迁移涉及的调度屏障覆盖旧/新 CPU。
		 */
		if (cpu == raw_smp_processor_id())
			continue;

		if (!(READ_ONCE(cpu_rq(cpu)->membarrier_state) &
		    MEMBARRIER_STATE_GLOBAL_EXPEDITED))
			continue;

		/*
		 * Skip the CPU if it runs a kernel thread which is not using
		 * a task mm.
		 */
		/* 跳过 mm==NULL 的纯内核线程；它返回用户 mm 前必经调度切换配对屏障。 */
		p = rcu_dereference(cpu_rq(cpu)->curr);
		if (!p->mm)
			continue;

		__cpumask_set_cpu(cpu, tmpmask);
	}
	rcu_read_unlock();

	preempt_disable();
	smp_call_function_many(tmpmask, ipi_mb, NULL, 1);
	preempt_enable();

	return 0;
}

/*
 * 对 current->mm 执行普通、SYNC_CORE 或 RSEQ 私有 expedited 请求。先验证配置和该 mm
 * 已完成对应注册，未注册返回 -EPERM；普通/RSEQ 在单 mm 用户或单 CPU 时可快速成功，
 * SYNC_CORE 仍需覆盖本 CPU 指令流。cpu_id>=0 只检查并中断指定 CPU，否则扫描所有运行
 * 同一 mm 的在线 CPU；掩码分配失败返回 -ENOMEM，无效/离线/已切换目标视为无需处理。
 */
static int membarrier_private_expedited(int flags, int cpu_id)
{
	struct mm_struct *mm = current->mm;
	smp_call_func_t ipi_func = ipi_mb;

	if (flags == MEMBARRIER_FLAG_SYNC_CORE) {
		if (!IS_ENABLED(CONFIG_ARCH_HAS_MEMBARRIER_SYNC_CORE))
			return -EINVAL;
		if (!(atomic_read(&mm->membarrier_state) &
		      MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE_READY))
			return -EPERM;
		ipi_func = ipi_sync_core;
		prepare_sync_core_cmd(mm);
	} else if (flags == MEMBARRIER_FLAG_RSEQ) {
		if (!IS_ENABLED(CONFIG_RSEQ))
			return -EINVAL;
		if (!(atomic_read(&mm->membarrier_state) &
		      MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ_READY))
			return -EPERM;
		ipi_func = ipi_rseq;
	} else {
		WARN_ON_ONCE(flags);
		if (!(atomic_read(&mm->membarrier_state) &
		      MEMBARRIER_STATE_PRIVATE_EXPEDITED_READY))
			return -EPERM;
	}

	if (flags != MEMBARRIER_FLAG_SYNC_CORE &&
	    (atomic_read(&mm->mm_users) == 1 || num_online_cpus() == 1))
		return 0;

	/*
	 * Matches memory barriers after rq->curr modification in
	 * scheduler.
	 *
	 * On RISC-V, this barrier pairing is also needed for the
	 * SYNC_CORE command when switching between processes, cf.
	 * the inline comments in membarrier_arch_switch_mm().
	 *
	 * Memory barrier on the caller thread _after_ we finished
	 * waiting for the last IPI. Matches memory barriers before
	 * rq->curr modification in scheduler.
	 */
	/*
	 * 作用域首尾屏障分别与调度器 rq->curr 更新后/前屏障配对；RISC-V 的 SYNC_CORE
	 * 进程切换也依赖此配对，故不能因回调已有同步核心操作而删除。
	 */
	guard(mb)();
	if (cpu_id >= 0) {
		/* UAPI 对不可能或越界 CPU 定义为没有目标工作，返回成功而不是参数错误。 */
		if (cpu_id >= nr_cpu_ids || !cpu_possible(cpu_id))
			return 0;

		SERIALIZE_IPI_CPU(cpu_id);
		guard(cpus_read_lock)();
		struct task_struct *p;

		if (!cpu_online(cpu_id))
			return 0;

		rcu_read_lock();
		p = rcu_dereference(cpu_rq(cpu_id)->curr);
		if (!p || p->mm != mm) {
			rcu_read_unlock();
			return 0;
		}
		rcu_read_unlock();
		/*
		 * smp_call_function_single() will call ipi_func() if cpu_id
		 * is the calling CPU.
		 */
		/* 指定当前 CPU 也会同步执行回调，因此 RSEQ/CORE 语义不会被跳过。 */
		smp_call_function_single(cpu_id, ipi_func, NULL, 1);
	} else {
		cpumask_var_t __free(free_cpumask_var) tmpmask = CPUMASK_VAR_NULL;
		int cpu;

		if (!zalloc_cpumask_var(&tmpmask, GFP_KERNEL))
			return -ENOMEM;

		SERIALIZE_IPI();
		guard(cpus_read_lock)();

		rcu_read_lock();
		for_each_online_cpu(cpu) {
			struct task_struct *p;

			p = rcu_dereference(cpu_rq(cpu)->curr);
			if (p && p->mm == mm)
				__cpumask_set_cpu(cpu, tmpmask);
		}
		rcu_read_unlock();
		/*
		 * For regular membarrier, we can save a few cycles by
		 * skipping the current cpu -- we're about to do smp_mb()
		 * below, and if we migrate to a different cpu, this cpu
		 * and the new cpu will execute a full barrier in the
		 * scheduler.
		 *
		 * For SYNC_CORE, we do need a barrier on the current cpu --
		 * otherwise, if we are migrated and replaced by a different
		 * task in the same mm just before, during, or after
		 * membarrier, we will end up with some thread in the mm
		 * running without a core sync.
		 *
		 * For RSEQ, don't invoke rseq_sched_switch_event() on the
		 * caller.  User code is not supposed to issue syscalls at
		 * all from inside an rseq critical section.
		 */
		/*
		 * 普通/RSEQ 的 many 接口跳过当前 CPU，结束屏障或 syscall 禁入 rseq CS 足够；
		 * SYNC_CORE 必须连当前 CPU 一并执行，防止同 mm 的替换线程未经 core sync 运行。
		 */
		if (flags != MEMBARRIER_FLAG_SYNC_CORE) {
			preempt_disable();
			smp_call_function_many(tmpmask, ipi_func, NULL, true);
			preempt_enable();
		} else {
			on_each_cpu_mask(tmpmask, ipi_func, NULL, true);
		}
	}

	return 0;
}

/*
 * 注册位写入 mm 后，把状态同步到当前正在运行该 mm 的所有 rq。单用户/单 CPU 可直接
 * 更新本 rq 并用屏障发布；多用户先 synchronize_rcu() 越过旧调度观察，再锁定 CPU
 * 在线集合、扫描 curr 并同步 IPI。分配失败返回 -ENOMEM，调用者保留已置的非 READY 位，
 * 后续注册可重试；只有同步成功后上层才发布 READY。
 */
static int sync_runqueues_membarrier_state(struct mm_struct *mm)
{
	int membarrier_state = atomic_read(&mm->membarrier_state);
	cpumask_var_t tmpmask;
	int cpu;

	if (atomic_read(&mm->mm_users) == 1 || num_online_cpus() == 1) {
		this_cpu_write(runqueues.membarrier_state, membarrier_state);

		/*
		 * For single mm user, we can simply issue a memory barrier
		 * after setting MEMBARRIER_STATE_GLOBAL_EXPEDITED in the
		 * mm and in the current runqueue to guarantee that no memory
		 * access following registration is reordered before
		 * registration.
		 */
		/* 单 mm 用户没有其他并行执行者，本 CPU rq 写入加完整屏障即可建立注册边界。 */
		smp_mb();
		return 0;
	}

	if (!zalloc_cpumask_var(&tmpmask, GFP_KERNEL))
		return -ENOMEM;

	/*
	 * For mm with multiple users, we need to ensure all future
	 * scheduler executions will observe @mm's new membarrier
	 * state.
	 */
	/* 等待既有 RCU 读侧退出，使后续调度/扫描不会继续使用注册前的观察状态。 */
	synchronize_rcu();

	/*
	 * For each cpu runqueue, if the task's mm match @mm, ensure that all
	 * @mm's membarrier state set bits are also set in the runqueue's
	 * membarrier state. This ensures that a runqueue scheduling
	 * between threads which are users of @mm has its membarrier state
	 * updated.
	 */
	/* 只向扫描瞬间运行该 mm 的 CPU 发 IPI；未来切入者由 scheduler switch 路径同步。 */
	SERIALIZE_IPI();
	cpus_read_lock();
	rcu_read_lock();
	for_each_online_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		struct task_struct *p;

		p = rcu_dereference(rq->curr);
		if (p && p->mm == mm)
			__cpumask_set_cpu(cpu, tmpmask);
	}
	rcu_read_unlock();

	on_each_cpu_mask(tmpmask, ipi_sync_rq_state, mm, true);

	free_cpumask_var(tmpmask);
	cpus_read_unlock();

	return 0;
}

/*
 * 幂等注册 GLOBAL_EXPEDITED：先设置功能位，使 rq 同步逻辑知道要传播什么；传播成功
 * 后再设置 READY，防止 syscall 在 rq 状态尚未覆盖所有执行者时提前使用。失败可重试。
 */
static int membarrier_register_global_expedited(void)
{
	struct task_struct *p = current;
	struct mm_struct *mm = p->mm;
	int ret;

	if (atomic_read(&mm->membarrier_state) &
	    MEMBARRIER_STATE_GLOBAL_EXPEDITED_READY)
		return 0;
	atomic_or(MEMBARRIER_STATE_GLOBAL_EXPEDITED, &mm->membarrier_state);
	ret = sync_runqueues_membarrier_state(mm);
	if (ret)
		return ret;
	atomic_or(MEMBARRIER_STATE_GLOBAL_EXPEDITED_READY,
		  &mm->membarrier_state);

	return 0;
}

/*
 * 幂等注册私有 expedited 及可选 SYNC_CORE/RSEQ 能力。配置不支持返回 -EINVAL；状态
 * 属于 mm 而非线程组，覆盖 CLONE_VM 但非 CLONE_THREAD 的共享者。先发布功能位并同步
 * rq，成功后才发布对应 READY；同步分配失败返回 -ENOMEM 且保留可重试的功能位。
 */
static int membarrier_register_private_expedited(int flags)
{
	struct task_struct *p = current;
	struct mm_struct *mm = p->mm;
	int ready_state = MEMBARRIER_STATE_PRIVATE_EXPEDITED_READY,
	    set_state = MEMBARRIER_STATE_PRIVATE_EXPEDITED,
	    ret;

	if (flags == MEMBARRIER_FLAG_SYNC_CORE) {
		if (!IS_ENABLED(CONFIG_ARCH_HAS_MEMBARRIER_SYNC_CORE))
			return -EINVAL;
		ready_state =
			MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE_READY;
	} else if (flags == MEMBARRIER_FLAG_RSEQ) {
		if (!IS_ENABLED(CONFIG_RSEQ))
			return -EINVAL;
		ready_state =
			MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ_READY;
	} else {
		WARN_ON_ONCE(flags);
	}

	/*
	 * We need to consider threads belonging to different thread
	 * groups, which use the same mm. (CLONE_VM but not
	 * CLONE_THREAD).
	 */
	/* 原文强调：不能只遍历 current 线程组，因为不同线程组也可能共享同一个 mm。 */
	if ((atomic_read(&mm->membarrier_state) & ready_state) == ready_state)
		return 0;
	if (flags & MEMBARRIER_FLAG_SYNC_CORE)
		set_state |= MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE;
	if (flags & MEMBARRIER_FLAG_RSEQ)
		set_state |= MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ;
	atomic_or(set_state, &mm->membarrier_state);
	ret = sync_runqueues_membarrier_state(mm);
	if (ret)
		return ret;
	atomic_or(ready_state, &mm->membarrier_state);

	return 0;
}

/*
 * 把 mm 内部功能位/READY 位对转换成 UAPI REGISTER 命令掩码；任一配对位存在即报告
 * 该注册，随后清除已识别状态并警告未知残留。纯原子快照，不发送 IPI、不分配内存。
 */
static int membarrier_get_registrations(void)
{
	struct task_struct *p = current;
	struct mm_struct *mm = p->mm;
	int registrations_mask = 0, membarrier_state, i;
	static const int states[] = {
		MEMBARRIER_STATE_GLOBAL_EXPEDITED |
			MEMBARRIER_STATE_GLOBAL_EXPEDITED_READY,
		MEMBARRIER_STATE_PRIVATE_EXPEDITED |
			MEMBARRIER_STATE_PRIVATE_EXPEDITED_READY,
		MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE |
			MEMBARRIER_STATE_PRIVATE_EXPEDITED_SYNC_CORE_READY,
		MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ |
			MEMBARRIER_STATE_PRIVATE_EXPEDITED_RSEQ_READY
	};
	static const int registration_cmds[] = {
		MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED,
		MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
		MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE,
		MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ
	};
	BUILD_BUG_ON(ARRAY_SIZE(states) != ARRAY_SIZE(registration_cmds));

	membarrier_state = atomic_read(&mm->membarrier_state);
	for (i = 0; i < ARRAY_SIZE(states); ++i) {
		if (membarrier_state & states[i]) {
			registrations_mask |= registration_cmds[i];
			membarrier_state &= ~states[i];
		}
	}
	WARN_ON_ONCE(membarrier_state != 0);
	return registrations_mask;
}

/**
 * sys_membarrier - issue memory barriers on a set of threads
 * @cmd:    Takes command values defined in enum membarrier_cmd.
 * @flags:  Currently needs to be 0 for all commands other than
 *          MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ: in the latter
 *          case it can be MEMBARRIER_CMD_FLAG_CPU, indicating that @cpu_id
 *          contains the CPU on which to interrupt (= restart)
 *          the RSEQ critical section.
 * @cpu_id: if @flags == MEMBARRIER_CMD_FLAG_CPU, indicates the cpu on which
 *          RSEQ CS should be interrupted (@cmd must be
 *          MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ).
 *
 * If this system call is not implemented, -ENOSYS is returned. If the
 * command specified does not exist, not available on the running
 * kernel, or if the command argument is invalid, this system call
 * returns -EINVAL. For a given command, with flags argument set to 0,
 * if this system call returns -ENOSYS or -EINVAL, it is guaranteed to
 * always return the same value until reboot. In addition, it can return
 * -ENOMEM if there is not enough memory available to perform the system
 * call.
 *
 * All memory accesses performed in program order from each targeted thread
 * is guaranteed to be ordered with respect to sys_membarrier(). If we use
 * the semantic "barrier()" to represent a compiler barrier forcing memory
 * accesses to be performed in program order across the barrier, and
 * smp_mb() to represent explicit memory barriers forcing full memory
 * ordering across the barrier, we have the following ordering table for
 * each pair of barrier(), sys_membarrier() and smp_mb():
 *
 * The pair ordering is detailed as (O: ordered, X: not ordered):
 *
 *                        barrier()   smp_mb() sys_membarrier()
 *        barrier()          X           X            O
 *        smp_mb()           X           O            O
 *        sys_membarrier()   O           O            O
 */
/*
 * 系统调用分派首先限制 flags：仅 PRIVATE_EXPEDITED_RSEQ 接受 CPU 定向标志，否则必须
 * 为 0；未定向时把 cpu_id 规范为 -1。QUERY 返回本内核/配置支持集，nohz_full 下移除
 * GLOBAL；传统 GLOBAL 以 synchronize_rcu() 建立慢速全局屏障且与 nohz_full 不兼容。
 * 其余命令分别进入注册或 expedited 路径，未知命令返回 -EINVAL。
 */
SYSCALL_DEFINE3(membarrier, int, cmd, unsigned int, flags, int, cpu_id)
{
	switch (cmd) {
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ:
		if (unlikely(flags && flags != MEMBARRIER_CMD_FLAG_CPU))
			return -EINVAL;
		break;
	default:
		if (unlikely(flags))
			return -EINVAL;
	}

	if (!(flags & MEMBARRIER_CMD_FLAG_CPU))
		cpu_id = -1;

	switch (cmd) {
	case MEMBARRIER_CMD_QUERY:
	{
		int cmd_mask = MEMBARRIER_CMD_BITMASK;

		if (tick_nohz_full_enabled())
			/* GLOBAL 依赖周期性内核/RCU 边界，full-nohz CPU 不能提供该保证。 */
			cmd_mask &= ~MEMBARRIER_CMD_GLOBAL;
		return cmd_mask;
	}
	case MEMBARRIER_CMD_GLOBAL:
		/* MEMBARRIER_CMD_GLOBAL is not compatible with nohz_full. */
		/* 传统全局命令不能用于 nohz_full；expedited IPI 变体仍可使用。 */
		if (tick_nohz_full_enabled())
			return -EINVAL;
		if (num_online_cpus() > 1)
			synchronize_rcu();
		return 0;
	case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
		return membarrier_global_expedited();
	case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
		return membarrier_register_global_expedited();
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
		return membarrier_private_expedited(0, cpu_id);
	case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
		return membarrier_register_private_expedited(0);
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
		return membarrier_private_expedited(MEMBARRIER_FLAG_SYNC_CORE, cpu_id);
	case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
		return membarrier_register_private_expedited(MEMBARRIER_FLAG_SYNC_CORE);
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ:
		return membarrier_private_expedited(MEMBARRIER_FLAG_RSEQ, cpu_id);
	case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ:
		return membarrier_register_private_expedited(MEMBARRIER_FLAG_RSEQ);
	case MEMBARRIER_CMD_GET_REGISTRATIONS:
		return membarrier_get_registrations();
	default:
		return -EINVAL;
	}
}
