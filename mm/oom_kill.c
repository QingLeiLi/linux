// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/mm/oom_kill.c
 *
 *  Copyright (C)  1998,2000  Rik van Riel
 *	Thanks go out to Claus Fischer for some serious inspiration and
 *	for goading me into coding this file...
 *  Copyright (C)  2010  Google, Inc.
 *	Rewritten by David Rientjes
 *
 *  The routines in this file are used to kill a process when
 *  we're seriously out of memory. This gets called from __alloc_pages()
 *  in mm/page_alloc.c when we really run out of memory.
 *
 *  Since we won't call these routines often (on a well-configured
 *  machine) this file will double as a 'coding guide' and a signpost
 *  for newbie kernel hackers. It features several pointers to major
 *  kernel subsystems and hints as to where to find out what things do.
 */
/*
 * 本文件实现“常规页分配与回收已经无法推进”后的最后恢复路径：划定失败分配所在的内存域，
 * 选择最可能释放有效内存的用户进程，发送致命信号并在必要时由 reaper 提前拆除其私有映射。
 * 正常配置下该路径很少进入，因此源码也保留了指向调度、进程、memcg、页表与回收子系统的学习线索。
 */

#include <linux/oom.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/sched/debug.h>
/* 调度、任务、mm、memcg、notifier 与 trace 头共同定义 OOM 从分配失败到受害者退出的边界。 */
/* swap、时间与 cpuset 支持把候选资格、延迟 reaper 和约束域统一到当前分配上下文。 */
#include <linux/swap.h>
#include <linux/syscalls.h>
/* timex、jiffies 与 cpuset 让 OOM 代码处理延时、限域和系统时间相关的状态。 */
#include <linux/timex.h>
#include <linux/jiffies.h>
#include <linux/cpuset.h>
#include <linux/export.h>
#include <linux/notifier.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/security.h>
#include <linux/ptrace.h>
#include <linux/freezer.h>
/* freezer 交互决定 OOM 受害者能否实际运行退出路径，而不是仅被标记为 MEMDIE。 */
/* kthread、MMU notifier 与凭据接口分别服务后台回收、地址空间失效和受害者审计日志。 */
#include <linux/ftrace.h>
#include <linux/ratelimit.h>
/* 后台 worker 的创建与初始化阶段依赖 kthread/init，且需要 MMU 生命周期通知。 */
#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/mmu_notifier.h>
#include <linux/cred.h>
#include <linux/nmi.h>

#include <asm/tlb.h>
#include "internal.h"
#include "slab.h"

#define CREATE_TRACE_POINTS
/* tracepoint 定义把选择、标记和 reaper 事件暴露给观测工具，不能重复包含到其它编译单元。 */
#include <trace/events/oom.h>

/* OOM 路径在常规回收已经无法满足分配时，选择可释放内存的受害者并协调其退出/reap。 */
/* 0/1/2：不 panic、仅全局无约束 OOM panic、所有非 SysRq OOM panic；由 vm sysctl 更新。 */
static int sysctl_panic_on_oom;
/* 非零时优先杀死触发全局 OOM 的 current，仍受不可杀与 cpuset/adj 规则约束。 */
static int sysctl_oom_kill_allocating_task;
/* 非零时在受限速的 OOM header 中打印域内任务内存快照；不改变候选选择。 */
static int sysctl_oom_dump_tasks = 1;

/*
 * Serializes oom killer invocations (out_of_memory()) from all contexts to
 * prevent from over eager oom killing (e.g. when the oom killer is invoked
 * from different domains).
 *
 * oom_killer_disable() relies on this lock to stabilize oom_killer_disabled
 * and mark_oom_victim
 */
/*
 * oom_lock 串行化不同域的 out_of_memory() 决策，并让 disable 与 victim 标记形成同一发布边界；
 * 它不保护单个 mm 的页表或任务字段，后者仍需各自锁/引用。
 */
DEFINE_MUTEX(oom_lock);
/* Serializes oom_score_adj and oom_score_adj_min updates */
/* oom_adj_mutex 只串行用户可写的 oom_score_adj 与下限更新，不参与 victim 选择主事务。 */
DEFINE_MUTEX(oom_adj_mutex);

/*
 * is_memcg_oom() - 判断本次仲裁是否属于 memory cgroup 域。
 * 业务背景：选择器据此切换全局任务/NUMA 规则与 memcg 专属扫描和容量分母。
 * 入参：oc 为调用链持有的非 NULL 控制块；这里只读取 memcg 字段。
 * 返回/副作用：memcg 非空返回 true，否则 false；纯判断，无引用或状态变化。
 * 注意事项：不验证 memcg 生命周期；调用者必须在整个 OOM 事务中稳定该 css 引用。
 */
static inline bool is_memcg_oom(struct oom_control *oc)
{
	return oc->memcg != NULL;
}

#ifdef CONFIG_NUMA
/**
 * oom_cpuset_eligible() - check task eligibility for kill
 * @start: task struct of which task to consider
 * @oc: pointer to struct oom_control
 *
 * Task eligibility is determined by whether or not a candidate task, @tsk,
 * shares the same mempolicy nodes as current if it is bound by such a policy
 * and whether or not it has the same set of allowed cpuset nodes.
 *
 * This function is assuming oom-killer context and 'current' has triggered
 * the oom-killer.
 */
/*
 * oom_cpuset_eligible() - 判断候选线程组是否与失败分配共享 NUMA 可达域。
 * 业务背景：全局 OOM 只应杀能缓解当前受限节点压力的进程，避免释放域外内存却仍失败。
 * 入参：start 是线程组起点借用指针；oc 提供 mempolicy nodemask，或以 current cpuset 为域。
 * 返回/副作用：任一线程与域相交即 true；RCU 下只读线程链，不取得 task/mm 长期引用。
 * 注意事项：仅用于非 memcg OOM；结果是瞬时资格，后续仍须校验 mm 与不可杀规则。
 */
static bool oom_cpuset_eligible(struct task_struct *start,
				struct oom_control *oc)
{
	/* 遍历线程组是因为同一进程可有不同 task mm/cpuset 状态，任一可达线程即可作为候选。 */
	struct task_struct *tsk;
	bool ret = false;
	const nodemask_t *mask = oc->nodemask;
	/* ret 仅在找到第一个可达线程时置真，循环随即停止以减少 OOM 路径停留。 */

	/* mask 非空代表 allocation 被 mempolicy 约束，cpuset 不可扩大该节点可达域。 */
	/* RCU 保护线程链表；本函数不取得长期 task 引用，结果仅供当前扫描轮决定。 */
	rcu_read_lock();
	for_each_thread(start, tsk) {
		/* mempolicy 约束优先于 cpuset；否则使用 current 与候选的 cpuset 交集。 */
		if (mask) {
			/*
			 * If this is a mempolicy constrained oom, tsk's
			 * cpuset is irrelevant.  Only return true if its
			 * mempolicy intersects current, otherwise it may be
			 * needlessly killed.
			 */
			ret = mempolicy_in_oom_domain(tsk, mask);
		} else {
			/*
			 * This is not a mempolicy constrained oom, so only
			 * check the mems of tsk's cpuset.
			 */
			ret = cpuset_mems_allowed_intersects(current, tsk);
		}
		if (ret)
			break;
	}
	rcu_read_unlock();

	/* RCU 解锁后不再访问线程链表，ret 只是本轮约束域资格的快照。 */
	return ret;
}
#else
/*
 * oom_cpuset_eligible() - 非 NUMA 构建下的域资格恒真桩。
 * 业务背景：保持选择器调用点统一，把没有节点拓扑差异的构建折叠为恒真资格。
 * 入参：tsk/oc 均为借用但不读取；其它不可杀规则由调用者继续检查。
 * 返回/副作用：始终 true，无同步和状态变化。
 * 注意事项：只消除 NUMA/cpuset 过滤，不表示任务一定有 mm 或可被杀。
 */
static bool oom_cpuset_eligible(struct task_struct *tsk, struct oom_control *oc)
{
	return true;
}
#endif /* CONFIG_NUMA */

/*
 * The process p may have detached its own ->mm while exiting or through
 * kthread_use_mm(), but one or more of its subthreads may still have a valid
 * pointer.  Return p, or any of its subthreads with a valid ->mm, with
 * task_lock() held.
 */
/*
 * find_lock_task_mm() - 在线程组中找到仍绑定 mm 的 task 并保持其 task_lock。
 * 业务背景：leader 可能已 exit_mm，但共享地址空间的子线程仍可供 OOM 评分、日志或 kill 使用。
 * 入参：p 为稳定的线程组成员借用指针；调用者须保证组对象在调用期间存在。
 * 返回/副作用：成功返回组内 task 借用指针且 task_lock 仍持有；全组无 mm 返回 NULL且不持锁。
 * 锁/ownership：内部 RCU guard 只覆盖枚举；调用者必须 task_unlock(返回值)，mm 跨锁使用还需 mmgrab。
 * 注意事项：返回 task 可能不是传入的 leader；调用者不能解错对象的 task_lock。
 */
struct task_struct *find_lock_task_mm(struct task_struct *p)
{
	struct task_struct *t;

	guard(rcu)();

	/* group leader 已经 exit 时，仍可借子线程找到共享 mm，保证 OOM 归因不会过早丢失。 */
	for_each_thread(p, t) {
		task_lock(t);
		if (likely(t->mm))
			return t;
		task_unlock(t);
	}

	return NULL;
}

/*
 * order == -1 means the oom kill is required by sysrq, otherwise only
 * for display purposes.
 */
/*
 * is_sysrq_oom() - 识别人工 SysRq 触发的 OOM。
 * 业务背景：人工恢复路径不应被普通 notifier 短路或 panic_on_oom 升级。
 * 入参：oc 为非 NULL 借用控制块；order=-1 是构造者约定的专用哨兵。
 * 返回/副作用：命中哨兵返回 true，无状态变化；用于跳过 notifier 成功短路及 panic。
 * 注意事项：仅依据 order 判定，构造 oom_control 的调用者必须保留 -1 的唯一含义。
 */
static inline bool is_sysrq_oom(struct oom_control *oc)
{
	return oc->order == -1;
}

/* return true if the task is not adequate as candidate victim task. */
/*
 * oom_unkillable_task() - 过滤绝不能作为用户态 OOM 受害者的任务。
 * 业务背景：保护 init 和内核线程，避免以破坏系统存活性换取不可预期的内存释放。
 * 入参：p 为稳定 task 借用指针；只检查 global init 与 PF_KTHREAD 身份。
 * 返回/副作用：不可杀返回 true，否则 false；不检查 adj、mm 或约束域且不取得引用。
 * 注意事项：false 只是通过第一层过滤，后续仍须检查 oom_score_adj、mm 和域资格。
 */
static bool oom_unkillable_task(struct task_struct *p)
{
	if (is_global_init(p))
		return true;
	if (p->flags & PF_KTHREAD)
		return true;
	return false;
}

/*
 * Check whether unreclaimable slab amount is greater than
 * all user memory(LRU pages).
 * dump_unreclaimable_slab() could help in the case that
 * oom due to too much unreclaimable slab used by kernel.
*/
/*
 * should_dump_unreclaim_slab() - 判断全局 OOM 日志是否值得展开不可回收 slab。
 * 业务背景：当内核对象页超过全部 LRU 用户页时，单看进程 RSS 会掩盖真正压力来源。
 * 入参：无；读取全局 node 计数的近似快照。
 * 返回/副作用：slab 页数严格大于 LRU 合计返回 true；仅影响诊断，不参与选择或回收。
 * 注意事项：并发计数可变化，结论只决定是否打印昂贵诊断，不能证明 OOM 根因。
 */
static bool should_dump_unreclaim_slab(void)
{
	/* 比较全局不可回收 slab 与所有 LRU 用户页，过大时日志需提示内核对象而非误导为用户 RSS。 */
	unsigned long nr_lru;

	/* LRU 合计覆盖匿名、文件、隔离和不可驱逐页，作为用户可回收内存的近似基线。 */
	nr_lru = global_node_page_state(NR_ACTIVE_ANON) +
		 global_node_page_state(NR_INACTIVE_ANON) +
		 global_node_page_state(NR_ACTIVE_FILE) +
		 global_node_page_state(NR_INACTIVE_FILE) +
		 global_node_page_state(NR_ISOLATED_ANON) +
		 global_node_page_state(NR_ISOLATED_FILE) +
		 global_node_page_state(NR_UNEVICTABLE);

	/* slab 值以字节状态折页后和 LRU 页数比较，诊断阈值不参与 OOM 选择分数。 */
	return (global_node_page_state_pages(NR_SLAB_UNRECLAIMABLE_B) > nr_lru);
}

/**
 * oom_badness - heuristic function to determine which candidate task to kill
 * @p: task struct of which task we should calculate
 * @totalpages: total present RAM allowed for page allocation
 *
 * The heuristic for determining which task to kill is made to be as simple and
 * predictable as possible.  The goal is to return the highest value for the
 * task consuming the most memory to avoid subsequent oom failures.
 */
/*
 * oom_badness() - 计算一个线程组在当前 OOM 域中的可比较受害者分数。
 * 业务背景：优先杀实际占用 RSS、swap 和页表较多的任务，并叠加管理员 oom_score_adj 策略。
 * 入参：p 是扫描期稳定的线程组借用指针；totalpages 是当前域非零容量，用于缩放 adj。
 * 返回/副作用：不可选返回 LONG_MIN，否则返回综合分数；临时持 task_lock，不保留 task/mm 引用。
 * 注意事项：解锁后只使用已计算值；分数是选择启发式，不代表能立即释放同等页数。
 */
long oom_badness(struct task_struct *p, unsigned long totalpages)
{
	/* 返回 LONG_MIN 表示不可选；其余值是 RSS、页表、swap 与 oom_score_adj 的可比较总分。 */
	long points;
	long adj;
	/* totalpages 是当前约束域容量，不一定等于全机 RAM，保证 adj 在域内可比较。 */

	if (oom_unkillable_task(p))
		/* LONG_MIN 永远输给任何真实候选，不需要额外 boolean 过滤。 */
		return LONG_MIN;

	/* 找到并锁住仍有 mm 的线程，后续读取 signal/mm 字段必须在该 task_lock 窗口内完成。 */
	p = find_lock_task_mm(p);
	if (!p)
		return LONG_MIN;

	/*
	 * Do not even consider tasks which are explicitly marked oom
	 * unkillable or have been already oom reaped or the are in
	 * the middle of vfork
	 */
	adj = (long)p->signal->oom_score_adj;
	/* OOM_SCORE_ADJ_MIN、reaped mm 与 vfork 都无法安全当作杀后可释放对象。 */
	if (adj == OOM_SCORE_ADJ_MIN ||
			mm_flags_test(MMF_OOM_SKIP, p->mm) ||
			in_vfork(p)) {
		task_unlock(p);
		return LONG_MIN;
	}

	/*
	 * The baseline for the badness score is the proportion of RAM that each
	 * task's rss, pagetable and swap space use.
	 */
	points = get_mm_rss_sum(p->mm) + get_mm_counter_sum(p->mm, MM_SWAPENTS) +
	/* baseline 用实际 RSS、swap entries 和页表成本，避免仅按 virtual size 错杀稀疏映射任务。 */
		mm_pgtables_bytes(p->mm) / PAGE_SIZE;
	task_unlock(p);

	/* 解锁后只使用已计算分数；不得再访问 p->mm，任务可能立即退出并释放地址空间。 */
	/* Normalize to oom_score_adj units */
	adj *= totalpages / 1000;
	points += adj;

	/* adj 正负都会按当前域容量缩放，使相同配置在不同 memcg 容量下含义一致。 */
	return points;
}

/* enum 到日志名的直接映射；新增约束枚举时必须同步扩展，不能让诊断越界或错名。 */
static const char * const oom_constraint_text[] = {
	[CONSTRAINT_NONE] = "CONSTRAINT_NONE",
	[CONSTRAINT_CPUSET] = "CONSTRAINT_CPUSET",
	[CONSTRAINT_MEMORY_POLICY] = "CONSTRAINT_MEMORY_POLICY",
	[CONSTRAINT_MEMCG] = "CONSTRAINT_MEMCG",
};

/*
 * Determine the type of allocation constraint.
 */
/*
 * constrained_alloc() - 分类失败分配的约束域并建立 OOM 评分分母。
 * 业务背景：全局、cpuset、mempolicy 和 memcg 可释放的内存集合不同，不能共用全机容量评分。
 * 入参：oc 为可写控制块，zonelist/nodemask/memcg/gfp 描述原分配，生命周期由调用者稳定。
 * 返回/副作用：返回约束枚举并写 oc->totalpages；不选择任务、不回收内存、不取得域引用。
 * 注意事项：THISNODE 当前保守退回 NONE；totalpages 是 RAM 加适用 swap 的近似域容量。
 */
static enum oom_constraint constrained_alloc(struct oom_control *oc)
{
	/* 本函数为后续评分确定“可用总页数”和候选域，不直接选择或杀死任务。 */
	struct zone *zone;
	struct zoneref *z;
	enum zone_type highest_zoneidx = gfp_zone(oc->gfp_mask);
	bool cpuset_limited = false;
	int nid;
	/* zone/z 只是 zonelist 迭代游标，不在返回后保存任何 zone 指针。 */

	/* highest_zoneidx 限制扫描与本次 GFP 所能到达的最高 zone 保持一致。 */
	if (is_memcg_oom(oc)) {
		/* memcg max 为 0 时用 1 避免 score 归一化除零；候选扫描限定该 css 子树。 */
		oc->totalpages = mem_cgroup_get_max(oc->memcg) ?: 1;
		return CONSTRAINT_MEMCG;
	}

	/* Default to all available memory */
	/* 补充说明：全局 OOM 基线包含 RAM 与 swap，之后可能因 NUMA/mempolicy/cpuset 收窄。 */
	oc->totalpages = totalram_pages() + total_swap_pages;

	if (!IS_ENABLED(CONFIG_NUMA))
		/* 不启用 NUMA 时没有 cpuset/mempolicy 节点限制，global totalpages 已足够。 */
		return CONSTRAINT_NONE;

	if (!oc->zonelist)
		return CONSTRAINT_NONE;
	/*
	 * Reach here only when __GFP_NOFAIL is used. So, we should avoid
	 * to kill current.We have to random task kill in this case.
	 * Hopefully, CONSTRAINT_THISNODE...but no way to handle it, now.
	 */
	if (oc->gfp_mask & __GFP_THISNODE)
		/* THISNODE 无可靠的跨节点替代候选域，保守回退全局约束处理。 */
		return CONSTRAINT_NONE;

	/*
	 * This is not a __GFP_THISNODE allocation, so a truncated nodemask in
	 * the page allocator means a mempolicy is in effect.  Cpuset policy
	 * is enforced in get_page_from_freelist().
	 */
	if (oc->nodemask &&
	    !nodes_subset(node_states[N_MEMORY], *oc->nodemask)) {
		oc->totalpages = total_swap_pages;
		for_each_node_mask(nid, *oc->nodemask)
			oc->totalpages += node_present_pages(nid);
		return CONSTRAINT_MEMORY_POLICY;
	}

	/* Check this allocation failure is caused by cpuset's wall function */
	for_each_zone_zonelist_nodemask(zone, z, oc->zonelist,
		/* 任何候选 zone 被 cpuset 拒绝即说明全局总内存会高估本次可达资源。 */
			highest_zoneidx, oc->nodemask)
		if (!cpuset_zone_allowed(zone, oc->gfp_mask))
			cpuset_limited = true;

	if (cpuset_limited) {
		/* cpuset 域总量只累计当前允许节点；杀域外任务不能改善本次分配。 */
		oc->totalpages = total_swap_pages;
		for_each_node_mask(nid, cpuset_current_mems_allowed)
			oc->totalpages += node_present_pages(nid);
		return CONSTRAINT_CPUSET;
	}
	return CONSTRAINT_NONE;
}

/*
 * oom_evaluate_task() - 将一个域内任务与当前最佳 OOM 候选比较。
 * 业务背景：全局或 memcg 扫描逐项调用，最终得到最高 badness 或识别已有 victim 正在退出。
 * 入参：task 是扫描器临时借用；arg 必须指向本轮可写 oom_control。
 * 返回/副作用：通常 0；已有未 reaped victim 时返回 1，并把 chosen 改成 -1 中止哨兵。
 * 注意事项：普通 chosen 持一份 task 引用，替换时 put 旧引用；-1 不是真指针且不得 put。
 */
static int oom_evaluate_task(struct task_struct *task, void *arg)
{
	/* 回调在 memcg 或全局任务遍历中运行；oc->chosen 的引用所有权由本函数维护。 */
	struct oom_control *oc = arg;
	long points;

	if (oom_unkillable_task(task))
		/* init/kthread 直接跳过，不参与 score 也不改变当前最佳候选。 */
		goto next;

	/* task 可能没有可释放 RSS，但 score helper 会在锁定 mm 后进一步过滤。 */
	/* p may not have freeable memory in nodemask */
	if (!is_memcg_oom(oc) && !oom_cpuset_eligible(task, oc))
		/* 全局 OOM 仍必须尊重触发分配所受的 NUMA/cpuset 可达域。 */
		goto next;

	/*
	 * This task already has access to memory reserves and is being killed.
	 * Don't allow any other task to have access to the reserves unless
	 * the task has MMF_OOM_SKIP because chances that it would release
	 * any memory is quite low.
	 */
	if (!is_sysrq_oom(oc) && tsk_is_oom_victim(task)) {
		/* 已有活跃受害者通常会释放 reserve；若尚未 OOM_SKIP 则中止并等待它。 */
		if (mm_flags_test(MMF_OOM_SKIP, task->signal->oom_mm))
			goto next;
		goto abort;
	}

	/*
	 * If task is allocating a lot of memory and has been marked to be
	 * killed first if it triggers an oom, then select it.
	 */
	if (oom_task_origin(task)) {
		/* 显式 origin 标记把本次触发者提升为最高分，避免扫描其它大进程。 */
		points = LONG_MAX;
		goto select;
	}

	points = oom_badness(task, oc->totalpages);
	/* 分数相等时后扫描者替换 chosen，内核不承诺稳定的同分排序。 */
	if (points == LONG_MIN || points < oc->chosen_points)
		goto next;

select:
	/* 用 task_struct 引用替换旧 chosen，保证选择结束到 kill 阶段之间对象不消失。 */
	if (oc->chosen)
		/* 新 chosen 接管前先 put 旧引用，防止长扫描随候选数泄漏 task_struct。 */
		put_task_struct(oc->chosen);
	get_task_struct(task);
	oc->chosen = task;
	oc->chosen_points = points;
next:
	return 0;
abort:
	/* -1 哨兵传达“已有未 reaped victim”，select_bad_process 不得再返回普通任务。 */
	if (oc->chosen)
		put_task_struct(oc->chosen);
	oc->chosen = (void *)-1UL;
	return 1;
}

/*
 * Simple selection loop. We choose the process with the highest number of
 * 'points'. In case scan was aborted, oc->chosen is set to -1.
 */
/*
 * select_bad_process() - 扫描当前 OOM 域并选出最高分候选。
 * 业务背景：把全局进程链或 memcg 成员扫描统一到 oom_evaluate_task() 的引用与哨兵协议。
 * 入参：oc 为可写控制块，totalpages、nodemask 和 memcg 已完成约束分类。
 * 返回/副作用：无返回；chosen 最终为 NULL、持引用 task 或 -1 中止哨兵，并更新 chosen_points。
 * 注意事项：全局扫描持 RCU；真正 kill 由 oom_lock 外层串行，调用者负责消费 chosen 引用。
 */
static void select_bad_process(struct oom_control *oc)
{
	/* LONG_MIN 使任意可杀候选都胜出；扫描结束后 chosen 由调用者接管/put。 */
	oc->chosen_points = LONG_MIN;
	/* 每次 OOM 选择从空分数开始，oc 不可复用前一次 chosen_points。 */

	/* memcg 使用 css 专属扫描；全局扫描在 RCU 下遍历进程链表。 */
	if (is_memcg_oom(oc))
		mem_cgroup_scan_tasks(oc->memcg, oom_evaluate_task, oc);
	else {
		/* global scan 只借用进程链，chosen 引用由 evaluate 立即获取。 */
		struct task_struct *p;

		rcu_read_lock();
		for_each_process(p)
			if (oom_evaluate_task(p, oc))
				break;
		rcu_read_unlock();
	}
}

/*
 * dump_task() - 输出一个具备当前域 OOM 资格且仍有 mm 的任务快照。
 * 业务背景：任务表诊断复用选择器过滤规则，避免展示域外或根本不可杀的进程。
 * 入参：p 是扫描期借用 task；arg 指向只读 oom_control，提供 memcg/NUMA/cpuset 域。
 * 返回/副作用：始终 0 继续扫描；合格时打印一行，内部取得并释放实际 mm 线程的 task_lock。
 * 注意事项：计数是并发快照，不持 mm 长期引用，也不影响评分或 kill 决策。
 */
static int dump_task(struct task_struct *p, void *arg)
{
	/* 诊断输出复用与选择相同的可杀和域资格规则，避免日志展示不可能被杀的任务。 */
	struct oom_control *oc = arg;
	struct task_struct *task;

	/* arg 为当前 oom_control 借用指针，决定打印任务是否与本次分配在同一域。 */
	if (oom_unkillable_task(p))
		return 0;

	/* p may not have freeable memory in nodemask */
	if (!is_memcg_oom(oc) && !oom_cpuset_eligible(p, oc))
		return 0;

	/* 取得带 task_lock 的活 mm 线程；打印完成前不得释放这把锁。 */
	task = find_lock_task_mm(p);
	if (!task) {
		/*
		 * All of p's threads have already detached their mm's. There's
		 * no need to report them; they can't be oom killed anyway.
		 */
		return 0;
	}

	pr_info("[%7d] %5d %5d %8lu %8lu %8lu %8lu %9lu %8ld %8lu         %5hd %s\n",
	/* 日志字段由持锁的 task/mm 读取，task_unlock 后不能再引用这些字段。 */
		task->pid, from_kuid(&init_user_ns, task_uid(task)),
		task->tgid, task->mm->total_vm, get_mm_rss_sum(task->mm),
		get_mm_counter_sum(task->mm, MM_ANONPAGES), get_mm_counter_sum(task->mm, MM_FILEPAGES),
		get_mm_counter_sum(task->mm, MM_SHMEMPAGES), mm_pgtables_bytes(task->mm),
		get_mm_counter_sum(task->mm, MM_SWAPENTS),
		task->signal->oom_score_adj, task->comm);
	task_unlock(task);

	/* 输出回调始终返回零，扫描器据此继续枚举其它候选任务。 */
	return 0;
}

/**
 * dump_tasks - dump current memory state of all system tasks
 * @oc: pointer to struct oom_control
 *
 * Dumps the current memory state of all eligible tasks.  Tasks not in the same
 * memcg, not in the same cpuset, or bound to a disjoint set of mempolicy nodes
 * are not shown.
 * State information includes task's pid, uid, tgid, vm size, rss,
 * pgtables_bytes, swapents, oom_score_adj value, and name.
 */
/*
 * dump_tasks() - 输出当前 OOM 域内所有合格任务的内存状态表。
 * 业务背景：在选择/kill 前保留可审计快照，帮助解释 badness 结果和不可回收内存来源。
 * 入参：oc 为只读域控制块；memcg 或 NUMA/cpuset 字段决定任务过滤范围。
 * 返回/副作用：无返回；打印表头与任务行，不取得长期 task/mm 引用、不改变选择状态。
 * 注意事项：全局遍历持 RCU 并定期触碰 watchdog；开销较大，只应由限速诊断路径调用。
 */
static void dump_tasks(struct oom_control *oc)
{
	/* 此函数只输出瞬时诊断，不取得长期 mm 引用；遍历期间允许任务退出或内存继续变化。 */
	pr_info("Tasks state (memory values in pages):\n");
	/* 表头单位为页，便于与 mm counter 直接对应而不在 OOM 热路径做额外转换。 */
	pr_info("[  pid  ]   uid  tgid total_vm      rss rss_anon rss_file rss_shmem pgtables_bytes swapents oom_score_adj name\n");

	/* 输出域与 select_bad_process 保持一致：memcg 回调或全局 RCU 进程遍历。 */
	if (is_memcg_oom(oc))
		mem_cgroup_scan_tasks(oc->memcg, dump_task, oc);
	else {
		struct task_struct *p;
		int i = 0;

		rcu_read_lock();
		/* 长任务表每 1024 项触碰 watchdog，防止 OOM 日志本身造成 soft lockup。 */
		for_each_process(p) {
			/* Avoid potential softlockup warning */
			if ((++i & 1023) == 0)
				touch_softlockup_watchdog();
			dump_task(p, oc);
		}
		rcu_read_unlock();
	}
}

/*
 * dump_oom_victim() - 输出一次 OOM 决策的约束域与最终受害者摘要。
 * 业务背景：把分配约束和被选任务绑定到同一行，供运维关联一次 OOM kill 事务。
 * 入参：oc 为本轮只读控制块；victim 是持引用且身份稳定的已选任务借用指针。
 * 返回/副作用：无返回；串接 constraint、nodemask、cpuset、memcg、任务名/pid/uid 到日志。
 * 注意事项：只做观测，不发送信号或改变受害者状态；字段仍可能与并发退出产生瞬时差异。
 */
static void dump_oom_victim(struct oom_control *oc, struct task_struct *victim)
{
	/* 汇总当前约束、节点、memcg 与最终受害者身份，作为一次 kill 事务的日志头。 */
	/* one line summary of the oom killer context. */
	pr_info("oom-kill:constraint=%s,nodemask=%*pbl",
			oom_constraint_text[oc->constraint],
			nodemask_pr_args(oc->nodemask));
	cpuset_print_current_mems_allowed();
	mem_cgroup_print_oom_context(oc->memcg, victim);
	pr_cont(",task=%s,pid=%d,uid=%d\n", victim->comm, victim->pid,
		from_kuid(&init_user_ns, task_uid(victim)));
}

/*
 * dump_header() - 输出触发 OOM 的分配上下文、内存状态和可选任务表。
 * 业务背景：kill 前集中记录调用栈、域内存和保护配置，为事后定位回收失败原因。
 * 入参：oc 为当前只读控制块，constraint/域字段须已建立；current 是触发上下文。
 * 返回/副作用：无返回；打印栈、memcg 或全局内存、protected memory，按 sysctl 决定任务表。
 * 注意事项：可能昂贵且获取多个内部锁，调用点应限速；函数不承担选择、kill 或引用释放。
 */
static void dump_header(struct oom_control *oc)
{
	/* 在真正选择前记录触发者、GFP/order 和可用内存上下文，供事后区分回收失败原因。 */
	pr_warn("%s invoked oom-killer: gfp_mask=%#x(%pGg), order=%d, oom_score_adj=%d\n",
		current->comm, oc->gfp_mask, &oc->gfp_mask, oc->order,
			current->signal->oom_score_adj);
	if (!IS_ENABLED(CONFIG_COMPACTION) && oc->order)
		/* 高阶分配无 compaction 时失败更可能是物理碎片而非纯容量耗尽。 */
		pr_warn("COMPACTION is disabled!!!\n");

	dump_stack();
	/* 调用栈定位分配失败者；随后按 memcg/全局域选择对应内存统计来源。 */
	if (is_memcg_oom(oc))
		mem_cgroup_print_oom_meminfo(oc->memcg);
	else {
		__show_mem(SHOW_MEM_FILTER_NODES, oc->nodemask, gfp_zone(oc->gfp_mask));
		if (should_dump_unreclaim_slab())
			dump_unreclaimable_slab();
	}
	mem_cgroup_show_protected_memory(oc->memcg);
	/* 受保护内存可能解释为何表面仍有页却无法满足当前回收请求。 */
	if (sysctl_oom_dump_tasks)
		dump_tasks(oc);
}

/*
 * Number of OOM victims in flight
 */
/* 已获 TIF_MEMDIE 的在飞受害者数；mark 增加、exit_oom_victim 减少。 */
static atomic_t oom_victims = ATOMIC_INIT(0);
/* disable 等待该计数归零；最后一个退出者负责 wake_up_all。 */
static DECLARE_WAIT_QUEUE_HEAD(oom_victims_wait);

/* 由 oom_lock 串行发布的全局禁用开关；常规读取多、写入极少。 */
static bool oom_killer_disabled __read_mostly;

/*
 * task->mm can be NULL if the task is the exited group leader.  So to
 * determine whether the task is using a particular mm, we examine all the
 * task's threads: if one of those is using this mm then this task was also
 * using it.
 */
/*
 * process_shares_mm() - 判断线程组中是否有线程当前绑定指定 mm。
 * 业务背景：OOM kill/reaper 必须识别跨线程组共享地址空间者，避免漏杀持锁者或错误提前回收。
 * 入参：p 是线程组借用起点，mm 是生命周期稳定的待比较地址空间；调用者负责稳定线程链。
 * 返回/副作用：任一线程的 READ_ONCE(t->mm) 命中即 true，否则 false；不取 task/mm 引用。
 * 注意事项：结果只是调用瞬间快照；引用保证 mm 存在但不冻结线程的 mm 绑定变化。
 */
bool process_shares_mm(const struct task_struct *p, const struct mm_struct *mm)
{
	/* 线程组可能在 exit/kthread_use_mm 中临时改变 leader->mm，故逐线程 READ_ONCE 查询。 */
	const struct task_struct *t;

	for_each_thread(p, t) {
		/* thread 指针来自稳定的任务链表调用者；READ_ONCE 只取得当前 mm 绑定快照。 */
		const struct mm_struct *t_mm = READ_ONCE(t->mm);
		if (t_mm)
			return t_mm == mm;
	}
	/* leader 与所有子线程均未绑定该 mm，调用者可排除该进程组。 */
	return false;
}

#ifdef CONFIG_MMU
/*
 * OOM Reaper kernel thread which tries to reap the memory used by the OOM
 * victim (if that is possible) to help the OOM killer to move on.
 */
/* reaper_th 是常驻 worker；wait/list/lock 组成 timer 向 worker 移交 task 引用的单链队列。 */
static struct task_struct *oom_reaper_th;
/* 队列从空变为非空后唤醒 worker；条件读取与队列发布由 reaper lock 配合。 */
static DECLARE_WAIT_QUEUE_HEAD(oom_reaper_wait);
/* 单链表头；每个节点的 oom_reaper_list 字段携带一份排队 task 引用。 */
static struct task_struct *oom_reaper_list;
/* timer 软中断入队与 worker 出队的 irq-safe 串行边界。 */
static DEFINE_SPINLOCK(oom_reaper_lock);

/*
 * __oom_reap_task_mm() - zap 一个受害者 mm 中可安全丢弃的私有/匿名映射。
 * 业务背景：无需等待完整 exit_mmap 就先释放急需页面，缩短系统停留在 OOM 的时间。
 * 入参：mm 持有效引用且调用者持 mmap_read_lock，Maple Tree/VMA 在该锁内稳定。
 * 返回/副作用：全部目标成功为 true，任一部分失败为 false；始终置 MMF_UNSTABLE 并拆目标映射。
 * 注意事项：跳过 hugetlb、PFNMAP 和共享文件映射；不结束任务，也不消费 mm 引用。
 */
static bool __oom_reap_task_mm(struct mm_struct *mm)
{
	/* reaper 仅释放地址空间可安全丢弃的页表映射；返回 false 表示部分 VMA 需稍后重试。 */
	struct vm_area_struct *vma;
	bool ret = true;
	MA_STATE(mas, &mm->mm_mt, ULONG_MAX, ULONG_MAX);
	/* Maple Tree 游标从最高地址向下遍历，VMA 指针只在 mmap_read_lock 持有期间使用。 */

	/*
	 * Tell all users of get_user/copy_from_user etc... that the content
	 * is no longer stable. No barriers really needed because unmapping
	 * should imply barriers already and the reader would hit a page fault
	 * if it stumbled over a reaped memory.
	 */
	/* MMF_UNSTABLE 令 user-copy 类访问感知内容不可靠，故障后会重新 fault 而非使用旧数据。 */
	mm_flags_set(MMF_UNSTABLE, mm);

	/*
	 * It might start racing with the dying task and compete for shared
	 * resources - e.g. page table lock contention has been observed.
	 * Reduce those races by reaping the oom victim from the other end
	 * of the address space.
	 */
	/* 逆序扫描降低与退出路径操作低地址 VMA 的资源竞争。 */
	mas_for_each_rev(&mas, vma, 0) {
		if (vma->vm_flags & (VM_HUGETLB|VM_PFNMAP))
			continue;

		/*
		 * Only anonymous pages have a good chance to be dropped
		 * without additional steps which we cannot afford as we
		 * are OOM already.
		 *
		 * We do not even care about fs backed pages because all
		 * which are reclaimable have already been reclaimed and
		 * we do not want to block exit_mmap by keeping mm ref
		 * count elevated without a good reason.
		 */
		/* 匿名或私有映射可直接 zap；共享文件映射优先留给常规回收/退出处理。 */
		if (vma_is_anonymous(vma) || !(vma->vm_flags & VM_SHARED)) {
			if (zap_vma_for_reaping(vma))
				ret = false;
		}
	}

	return ret;
}

/*
 * Reaps the address space of the given task.
 *
 * Returns true on success and false if none or part of the address space
 * has been reclaimed and the caller should retry later.
 */
/*
 * oom_reap_task_mm() - 执行一次带 mmap 锁同步的受害者 mm 回收尝试。
 * 业务背景：与 exit_mmap 的 OOM_SKIP 发布串行，且用 trylock 避免 reaper 在内存危机中阻塞。
 * 入参：tsk 持 reaper 队列引用用于日志；mm 由 signal->oom_mm 的引用稳定。
 * 返回/副作用：锁忙或部分 zap 为 false；已 SKIP 或完整处理为 true；输出配对 trace 和成功日志。
 * 注意事项：所有 VMA 访问都在 mmap 读锁内，返回前解锁；false 只表示值得有限重试。
 */
static bool oom_reap_task_mm(struct task_struct *tsk, struct mm_struct *mm)
{
	/* mmap_read_trylock 避免 OOM reaper 阻塞；锁忙或 skip 时让队列稍后重试/放弃。 */
	bool ret = true;

	if (!mmap_read_trylock(mm)) {
		/* 不等待写者，避免 reaper 与 exit_mmap 互相阻塞而延长 OOM。 */
		trace_skip_task_reaping(tsk->pid);
		return false;
	}

	/*
	 * MMF_OOM_SKIP is set by exit_mmap when the OOM reaper can't
	 * work on the mm anymore. The check for MMF_OOM_SKIP must run
	 * under mmap_lock for reading because it serializes against the
	 * mmap_write_lock();mmap_write_unlock() cycle in exit_mmap().
	 */
	if (mm_flags_test(MMF_OOM_SKIP, mm)) {
		/* exit_mmap 已接管或完成 mm，reaper 不得再触碰其页表。 */
		trace_skip_task_reaping(tsk->pid);
		goto out_unlock;
	}

	trace_start_task_reaping(tsk->pid);
	/* trace 开始后无论部分失败或成功都会走 finish，形成可配对观测事件。 */

	/* failed to reap part of the address space. Try again later */
	/* 锁内执行实际 zap，任何失败保留 ret=false 并在 finish trace 后释放 mmap 锁。 */
	ret = __oom_reap_task_mm(mm);
	if (!ret)
		goto out_finish;

	/* 仅完整 reap 才读取剩余 RSS 作成功日志；这些计数在 mmap 读锁保护区内取样。 */
	pr_info("oom_reaper: reaped process %d (%s), now anon-rss:%lukB, file-rss:%lukB, shmem-rss:%lukB\n",
			task_pid_nr(tsk), tsk->comm,
			K(get_mm_counter_sum(mm, MM_ANONPAGES)),
			K(get_mm_counter_sum(mm, MM_FILEPAGES)),
			K(get_mm_counter_sum(mm, MM_SHMEMPAGES)));
out_finish:
	trace_finish_task_reaping(tsk->pid);
out_unlock:
	mmap_read_unlock(mm);

	/* mmap 锁释放后 mm 仍由受害者的 oom_mm 生命周期持有，返回值仅驱动有限重试。 */
	return ret;
}

#define MAX_OOM_REAP_RETRIES 10
/*
 * oom_reap_task() - 对一个已脱链受害者执行有限次数的异步 reap。
 * 业务背景：避免锁冲突让唯一 reaper worker 永久卡住，同时给短暂 mmap writer 让路。
 * 入参：tsk 携带 queue_oom_reaper 取得的一份 task 引用，oom_mm 由 signal 生命周期稳定。
 * 返回/副作用：无返回；最多重试十次，最终置 OOM_SKIP、清链字段并消费 task 引用。
 * 注意事项：OOM_SKIP 表示不再等待更多回收，不保证全部 RSS 已被成功释放。
 */
static void oom_reap_task(struct task_struct *tsk)
{
	/* 队列持有一份 task 引用；无论重试成功、skip 或耗尽，都必须在 done 统一 put。 */
	int attempts = 0;
	struct mm_struct *mm = tsk->signal->oom_mm;

	/* Retry the mmap_read_trylock(mm) a few times */
	/* trylock 冲突只短暂等待，避免 reaper 永久阻塞而让 OOM reserve 长期被受害者占用。 */
	while (attempts++ < MAX_OOM_REAP_RETRIES && !oom_reap_task_mm(tsk, mm))
		schedule_timeout_idle(HZ/10);

	if (attempts <= MAX_OOM_REAP_RETRIES ||
	    mm_flags_test(MMF_OOM_SKIP, mm))
		goto done;

	pr_info("oom_reaper: unable to reap pid:%d (%s)\n",
		task_pid_nr(tsk), tsk->comm);
	sched_show_task(tsk);
	debug_show_all_locks();

done:
	/* 从 reaper 链摘除并设置 OOM_SKIP，后续 OOM 选择器知道该 mm 不再是可释放候选。 */
	tsk->oom_reaper_list = NULL;

	/*
	 * Hide this mm from OOM killer because it has been either reaped or
	 * somebody can't call mmap_write_unlock(mm).
	 */
	mm_flags_set(MMF_OOM_SKIP, mm);

	/* Drop a reference taken by queue_oom_reaper */
	put_task_struct(tsk);
}

/*
 * oom_reaper() - 常驻 worker 串行消费延迟入队的 OOM 受害者。
 * 业务背景：把可睡眠的地址空间 zap 从分配失败和 timer 上下文移到专用 freezable kthread。
 * 入参：unused 恒为 NULL且不读取；队列每个节点都携带一份 task 引用。
 * 返回/副作用：设计为无限循环；锁下弹出、锁外 reap，并由 oom_reap_task 消费引用。
 * 注意事项：等待可冻结，伪唤醒只会重试；自旋锁临界区不得执行 mmap 回收。
 */
static int oom_reaper(void *unused)
{
	/* 内核线程在 waitqueue 睡眠，唤醒后从锁保护的单链表弹出一个受害者串行回收。 */
	set_freezable();

	while (true) {
		/* reaper 线程永不退出；每次仅处理一个任务，保持对 mm zap 的串行压力。 */
		struct task_struct *tsk = NULL;

		/* freezable 允许系统冻结；条件只读取发布在 reaper lock 下的队列头。 */
		wait_event_freezable(oom_reaper_wait, oom_reaper_list != NULL);
		spin_lock_irq(&oom_reaper_lock);
		if (oom_reaper_list != NULL) {
			tsk = oom_reaper_list;
			oom_reaper_list = tsk->oom_reaper_list;
		}
		spin_unlock_irq(&oom_reaper_lock);

		/* 已脱链的本地 task 可在锁外睡眠/zap；空队列的伪唤醒直接进入下一轮等待。 */
		if (tsk)
			oom_reap_task(tsk);
	}

	return 0;
}

/*
 * wake_oom_reaper() - timer 到期后把仍需回收的 victim 移交给 worker 队列。
 * 业务背景：先给自然退出路径留时间，再在软中断上下文只做不可睡眠的入队工作。
 * 入参：timer 嵌入目标 task；此前 queue_oom_reaper 已为 timer/队列持一份 task 引用。
 * 返回/副作用：已 OOM_SKIP 时 put；否则 irqsave 锁下入队、trace 并唤醒 worker。
 * 注意事项：入队后引用由 oom_reap_task 消费；本回调不得额外 put 或执行实际 zap。
 */
static void wake_oom_reaper(struct timer_list *timer)
{
	/* timer 从 task 嵌入对象恢复受害者；队列接管此前 queue_oom_reaper 取得的 task 引用。 */
	struct task_struct *tsk = container_of(timer, struct task_struct,
			oom_reaper_timer);
	struct mm_struct *mm = tsk->signal->oom_mm;
	unsigned long flags;
	/* timer 执行在软中断上下文，队列操作使用 irqsave 自旋锁而不能取得睡眠锁。 */

	/* The victim managed to terminate on its own - see exit_mmap */
	if (mm_flags_test(MMF_OOM_SKIP, mm)) {
		/* 受害者自然退出时 exit_mmap 已完成回收，定时器只归还引用而不重复入队。 */
		put_task_struct(tsk);
		return;
	}

	/* 链表插入与 worker 弹出由同一锁串行，避免两个 timer 丢失彼此 next 指针。 */
	spin_lock_irqsave(&oom_reaper_lock, flags);
	tsk->oom_reaper_list = oom_reaper_list;
	oom_reaper_list = tsk;
	spin_unlock_irqrestore(&oom_reaper_lock, flags);
	trace_wake_reaper(tsk->pid);
	/* 唤醒在解锁后进行，worker 重新检查队列头，避免持锁进入调度路径。 */
	wake_up(&oom_reaper_wait);
}

/*
 * Give the OOM victim time to exit naturally before invoking the oom_reaping.
 * The timers timeout is arbitrary... the longer it is, the longer the worst
 * case scenario for the OOM can take. If it is too small, the oom_reaper can
 * get in the way and release resources needed by the process exit path.
 * e.g. The futex robust list can sit in Anon|Private memory that gets reaped
 * before the exit path is able to wake the futex waiters.
 */
/*
 * 延迟长度是在最坏 OOM 恢复时间与自然退出完整性之间折中：过长会延后紧急释放，过短则可能先
 * zap 掉 robust futex 等退出路径仍需读取的匿名私有数据，使等待者来不及被正常唤醒。
 */
#define OOM_REAPER_DELAY (2*HZ)
/*
 * queue_oom_reaper() - 为受害者安排一次去重的延迟异步回收。
 * 业务背景：给自然退出和 robust futex 清理留出窗口，超时后再由专用 worker zap 私有映射。
 * 入参：tsk 是已标记 victim 的稳定借用 task，signal->oom_mm 必须非 NULL。
 * 返回/副作用：无返回；首次置 QUEUED、取得 task 引用并启动 timer，重复调用无操作。
 * 注意事项：timer/队列最终在自然退出快路或 oom_reap_task 中恰好消费该引用。
 */
static void queue_oom_reaper(struct task_struct *tsk)
{
	/* test_and_set 把一次 mm 的回收排队去重；同一 mm 多线程只能拥有一个 reaper timer。 */
	/* mm is already queued? */
	if (mm_flags_test_and_set(MMF_OOM_REAP_QUEUED, tsk->signal->oom_mm))
		/* 原子位同时是去重和“已有引用/定时器即将接管”状态标记。 */
		return;

	/* timer 可在原调用者释放 task 后触发，故先获取独立生命周期引用。 */
	get_task_struct(tsk);
	timer_setup(&tsk->oom_reaper_timer, wake_oom_reaper, 0);
	tsk->oom_reaper_timer.expires = jiffies + OOM_REAPER_DELAY;
	add_timer(&tsk->oom_reaper_timer);
}

#ifdef CONFIG_SYSCTL
/* vm OOM sysctl 表只发布策略/诊断开关；不拥有对应整数的独立生命周期。 */
static const struct ctl_table vm_oom_kill_table[] = {
	/* 0/1/2 分别控制关闭、条件 panic 和无条件 panic，minmax 防止越界配置。 */
	{
		.procname	= "panic_on_oom",
		.data		= &sysctl_panic_on_oom,
		.maxlen		= sizeof(sysctl_panic_on_oom),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO,
	},
	/* 该开关让分配者优先成为受害者，适合调试但会绕过正常 badness 选择。 */
	{
		.procname	= "oom_kill_allocating_task",
		.data		= &sysctl_oom_kill_allocating_task,
		.maxlen		= sizeof(sysctl_oom_kill_allocating_task),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	/* 仅影响诊断输出量；关闭后不改变 OOM 决策、信号或 reaper 生命周期。 */
	{
		.procname	= "oom_dump_tasks",
		.data		= &sysctl_oom_dump_tasks,
		.maxlen		= sizeof(sysctl_oom_dump_tasks),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
};
#endif

/*
 * oom_init() - 建立 OOM reaper 服务与 vm sysctl。
 * 业务背景：运行期 OOM 发生前必须先有异步回收 worker 和管理员策略入口。
 * 入参：无；在 subsys_initcall 阶段依赖调度器和 sysctl 基础设施。
 * 返回/副作用：始终 0；创建常驻线程，并在启用配置时注册三项 sysctl。
 * 注意事项：当前实现未检查 kthread_run 的 ERR_PTR，后续路径假定 worker 建立成功。
 */
static int __init oom_init(void)
{
	oom_reaper_th = kthread_run(oom_reaper, NULL, "oom_reaper");
#ifdef CONFIG_SYSCTL
	register_sysctl_init("vm", vm_oom_kill_table);
#endif
	return 0;
}
subsys_initcall(oom_init)
#else
/*
 * queue_oom_reaper() - CONFIG_MMU=n 的空实现。
 * 业务背景：无页表可 zap 时保持 OOM 核心调用点与 MMU 构建一致。
 * 入参：tsk 为借用但不读取。
 * 返回/副作用：无返回，不排队、不持引用、不改地址空间状态。
 * 注意事项：victim 只能依赖其正常退出路径释放可用资源。
 */
static inline void queue_oom_reaper(struct task_struct *tsk)
{
	/* CONFIG_MMU=n 空桩：tsk 为借用但不读取；无返回、无排队、无引用和地址空间副作用。 */
}
#endif /* CONFIG_MMU */

/**
 * mark_oom_victim - mark the given task as OOM victim
 * @tsk: task to mark
 *
 * Has to be called with oom_lock held and never after
 * oom has been disabled already.
 *
 * tsk->mm has to be non NULL and caller has to guarantee it is stable (either
 * under task_lock or operate on the current).
 */
/*
 * mark_oom_victim() - 首次标记任务为 OOM victim 并授予内存 reserve 访问。
 * 业务背景：让已收到致命退出语义的任务能在内存耗尽时继续完成退出和释放资源。
 * 入参：tsk->mm 非 NULL且由 task_lock/current 稳定；调用者持 oom_lock，killer 尚未禁用。
 * 返回/副作用：无返回；首次置 MEMDIE、mmgrab oom_mm、thaw、增加 victim 计数并 trace；重复无操作。
 * 注意事项：新增计数必须由 exit_oom_victim() 配对；核心 kill 路径先发 SIGKILL，已在退出的快路
 * 可直接标记而不重复发信号。
 */
static void mark_oom_victim(struct task_struct *tsk)
{
	/* oom_lock 已由调用者持有；TIF_MEMDIE 是允许受害者使用内存 reserve 的发布标志。 */
	const struct cred *cred;
	struct mm_struct *mm = tsk->mm;

	WARN_ON(oom_killer_disabled);
	/* OOM killer might race with memcg OOM */
	if (test_and_set_tsk_thread_flag(tsk, TIF_MEMDIE))
		/* 已标记的任务不能再次增加 oom_victims 或重复绑定 oom_mm。 */
		return;

	/* oom_mm is bound to the signal struct life time. */
	/* signal->oom_mm 绑定线程组生命周期；cmpxchg 选定唯一 mm 后 mmgrab 延长其存在期。 */
	if (!cmpxchg(&tsk->signal->oom_mm, NULL, mm))
		mmgrab(tsk->signal->oom_mm);

	/*
	 * Make sure that the process is woken up from uninterruptible sleep
	 * if it is frozen because OOM killer wouldn't be able to free any
	 * memory and livelock. The freezer will thaw the tasks that are OOM
	 * victims regardless of the PM freezing and cgroup freezing states.
	 */
	/* 冻结受害者不会执行 exit/free，必须先 thaw 才能真正释放本次 OOM 所需内存。 */
	thaw_process(tsk);
	atomic_inc(&oom_victims);
	cred = get_task_cred(tsk);
	trace_mark_victim(tsk, cred->uid.val);
	put_cred(cred);
}

/**
 * exit_oom_victim - note the exit of an OOM victim
 */
/*
 * exit_oom_victim() - 在 victim 退出时撤销 reserve 标志并完成在飞计数。
 * 业务背景：oom_killer_disable() 必须等所有已授权 victim 离开后才能安全完成禁用。
 * 入参：无显式参数；current 必须已设置 TIF_MEMDIE，且本路径只执行一次。
 * 返回/副作用：无返回；清标志、递减 oom_victims，降为零时唤醒全部等待者。
 * 注意事项：重复调用会使计数下溢；oom_mm 引用由 signal 生命周期其它退出代码释放。
 */
void exit_oom_victim(void)
{
	/* 退出路径撤销本任务的 reserve-victim 状态并在最后一个受害者离开时唤醒等待 disable 者。 */
	clear_thread_flag(TIF_MEMDIE);

	if (!atomic_dec_return(&oom_victims))
		wake_up_all(&oom_victims_wait);
}

/**
 * oom_killer_enable - enable OOM killer
 */
/*
 * oom_killer_enable() - 清除全局 OOM 禁用状态。
 * 业务背景：系统冻结流程结束或禁用等待失败后，允许新分配失败再次选择 victim。
 * 入参：无；调用者负责与 disable 的外部时序协调。
 * 返回/副作用：无返回；清 disabled 并记录日志，不等待或修改已有 victims。
 * 注意事项：函数自身不取 oom_lock，不能与任意未协调写者并发使用。
 */
void oom_killer_enable(void)
{
	oom_killer_disabled = false;
	pr_info("OOM killer enabled.\n");
}

/**
 * oom_killer_disable - disable OOM killer
 * @timeout: maximum timeout to wait for oom victims in jiffies
 *
 * Forces all page allocations to fail rather than trigger OOM killer.
 * Will block and wait until all OOM victims are killed or the given
 * timeout expires.
 *
 * The function cannot be called when there are runnable user tasks because
 * the userspace would see unexpected allocation failures as a result. Any
 * new usage of this function should be consulted with MM people.
 *
 * Returns true if successful and false if the OOM killer cannot be
 * disabled.
 */
/*
 * oom_killer_disable() - 阻止新 victim 并等待所有在飞 OOM victims 退出。
 * 业务背景：系统冻结等阶段需要让内存分配失败可预测，而不是并发启动新的 kill 事务。
 * 入参：timeout 是最大可中断等待 jiffies；只允许无可运行普通用户任务的系统级调用者。
 * 返回/副作用：成功保持 disabled 并返回 true；加锁中断、等待超时/中断返回 false，后两者重新启用。
 * 注意事项：先以 killable oom_lock 串行发布禁用，再等待原子 victim 计数归零。
 */
bool oom_killer_disable(signed long timeout)
{
	/* 禁用先阻止新 victim，再等待现有计数归零；超时返回 false 并恢复可用状态。 */
	signed long ret;
	/* 该 API 假定没有普通用户任务运行，否则 disabled 会把其内存请求变成意外失败。 */

	/*
	 * Make sure to not race with an ongoing OOM killer. Check that the
	 * current is not killed (possibly due to sharing the victim's memory).
	 */
	if (mutex_lock_killable(&oom_lock))
		/* 被信号中断时未改变 disabled 状态，调用者按 false 继续常规错误处理。 */
		return false;
	oom_killer_disabled = true;
	mutex_unlock(&oom_lock);

	ret = wait_event_interruptible_timeout(oom_victims_wait,
	/* 条件由 exit_oom_victim 的 atomic dec 发布；timeout 以 jiffies 计。 */
			!atomic_read(&oom_victims), timeout);
	if (ret <= 0) {
		oom_killer_enable();
		return false;
	}
	pr_info("OOM killer disabled.\n");

	return true;
}

/*
 * __task_will_free_mem() - 判断单个线程组是否已经承诺及时退出。
 * 业务背景：作为“无需再杀新 victim”的第一层筛选，排除长时间 coredump 等伪退出状态。
 * 入参：task 为稳定借用对象；调用者负责 signal/task 状态可安全读取。
 * 返回/副作用：组退出或最后线程 PF_EXITING 为 true，coredump/其它状态为 false；纯判断。
 * 注意事项：不检查 mm、OOM_SKIP 或跨进程共享者，必须由 task_will_free_mem() 补全。
 */
static inline bool __task_will_free_mem(struct task_struct *task)
{
	/* 只判断单任务/线程组退出状态；共享 mm 是否都会退出由外层 task_will_free_mem 证明。 */
	struct signal_struct *sig = task->signal;

	/*
	 * A coredumping process may sleep for an extended period in
	 * coredump_task_exit(), so the oom killer cannot assume that
	 * the process will promptly exit and release memory.
	 */
	if (sig->core_state)
		/* core dump 可能长时间持有 mm，不能把“正在退出”误判为即将释放内存。 */
		return false;

	if (sig->flags & SIGNAL_GROUP_EXIT)
		/* 已进入组退出时所有线程都会收到终止语义，地址空间最终将被 exit_mmap 释放。 */
		return true;

	if (thread_group_empty(task) && (task->flags & PF_EXITING))
		/* 最后线程正在退出也满足释放预期，即使尚未置 SIGNAL_GROUP_EXIT。 */
		return true;

	return false;
}

/*
 * Checks whether the given task is dying or exiting and likely to
 * release its address space. This means that all threads and processes
 * sharing the same mm have to be killed or exiting.
 * Caller has to make sure that task->mm is stable (hold task_lock or
 * it operates on the current).
 */
/*
 * task_will_free_mem() - 证明目标及所有跨组 mm 共享者都会退出并释放地址空间。
 * 业务背景：满足该强条件时，OOM 可把 reserve 给现有退出者，而无需再杀一个无关进程。
 * 入参：task->mm 由 task_lock 或 current 身份稳定；函数内部以 RCU 枚举其它进程组。
 * 返回/副作用：有 mm、未 OOM_SKIP 且所有共享者承诺退出时 true，否则 false；不取长期引用。
 * 注意事项：结论只用于 oom_lock 串行决策窗口；引用存在不等于 mm 字段不再变化。
 */
static bool task_will_free_mem(struct task_struct *task)
{
	/* 返回 true 是 OOM 可把 reserve 交给该任务而不另杀候选的强保证。 */
	struct mm_struct *mm = task->mm;
	struct task_struct *p;
	bool ret = true;

	/*
	 * Skip tasks without mm because it might have passed its exit_mm and
	 * exit_oom_victim. oom_reaper could have rescued that but do not rely
	 * on that for now. We can consider find_lock_task_mm in future.
	 */
	if (!mm)
		/* exit_mm 后无法再从该任务释放地址空间，不能把它作为 OOM 快速成功。 */
		return false;

	if (!__task_will_free_mem(task))
		/* 单任务状态尚未承诺退出时，不冒险把 reserve 交给它。 */
		return false;

	/*
	 * This task has already been drained by the oom reaper so there are
	 * only small chances it will free some more
	 */
	if (mm_flags_test(MMF_OOM_SKIP, mm))
		/* skip 表示 reaper/exit 已完成可做工作，再等待不会产生有效回收。 */
		return false;

	if (atomic_read(&mm->mm_users) <= 1)
		/* 单用户 mm 没有跨线程组 pin，退出/重启 reaper 均可直接释放它。 */
		return true;

	/*
	 * Make sure that all tasks which share the mm with the given tasks
	 * are dying as well to make sure that a) nobody pins its mm and
	 * b) the task is also reapable by the oom reaper.
	 */
	/* 多进程共享 mm 时在 RCU 下检查每个其他组也处于会释放状态。 */
	rcu_read_lock();
	for_each_process(p) {
		/* 跨线程组共享 mm 的每个成员必须也满足 will-free，不然 mmap 仍可能被 pin。 */
		if (!process_shares_mm(p, mm))
			continue;
		if (same_thread_group(task, p))
			continue;
		ret = __task_will_free_mem(p);
		if (!ret)
			break;
	}
	rcu_read_unlock();

	/* RCU 解锁后结果已确定；调用者据此决定是否能安全抢占该共享 mm 的页。 */
	return ret;
}

/*
 * __oom_kill_process() - 把已选候选提交为 victim，并处理跨线程组共享同一 mm 的用户进程。
 * 业务背景：SIGKILL、reserve 授权和 reaper 必须按顺序发布，且共享者不能继续 pin mmap_lock。
 * 入参：victim 转交一份 task 引用；message 是调用期有效日志串；调用者处于串行 OOM 决策。
 * 返回/副作用：无返回；发送信号、标记/记账、可选排 reaper，并始终消费传入 task 引用。
 * 注意事项：内部 mmgrab 至末尾 mmdrop；若 init 共享 mm，则置 OOM_SKIP 并禁止 reaper。
 */
static void __oom_kill_process(struct task_struct *victim, const char *message)
{
	/* 此核心 kill 路径接管 chosen 的 task 引用，负责发 SIGKILL、标记 victim、排 reaper 并最终 put。 */
	struct task_struct *p;
	struct mm_struct *mm;
	bool can_oom_reap = true;
	/* can_oom_reap 在发现 init 共享 mm 时关闭，避免清空不可回收的系统关键地址空间。 */

	/* 返回的 p 持 task_lock；若 leader 已退出则转为实际仍持 mm 的线程作为 victim。 */
	p = find_lock_task_mm(victim);
	if (!p) {
		/* selected task 已无 mm 时，仅释放 chosen 引用，不能再发信号或排 reaper。 */
		pr_info("%s: OOM victim %d (%s) is already exiting. Skip killing the task\n",
			message, task_pid_nr(victim), victim->comm);
		put_task_struct(victim);
		return;
	} else if (victim != p) {
		/* 选择结果的 leader 可能失去 mm；转换为 p 后转移引用所有权。 */
		get_task_struct(p);
		put_task_struct(victim);
		victim = p;
	}

	/* Get a reference to safely compare mm after task_unlock(victim) */
	/* mmgrab 允许在解 task_lock 后遍历其它共享者并排队 reaper，直到最终 mmdrop。 */
	mm = victim->mm;
	mmgrab(mm);

	/* Raise event before sending signal: task reaper must see this */
	count_vm_event(OOM_KILL);
	/* event 和 memcg event 记录在 SIGKILL 前，确保所有观察者都看到同一 kill 尝试。 */
	memcg_memory_event_mm(mm, MEMCG_OOM_KILL);

	/*
	 * We should send SIGKILL before granting access to memory reserves
	 * in order to prevent the OOM victim from depleting the memory
	 * reserves from the user space under its control.
	 */
	/* 先发致命信号再授予 TIF_MEMDIE reserve，防止受害者借 reserve 继续扩张内存。 */
	do_send_sig_info(SIGKILL, SEND_SIG_PRIV, victim, PIDTYPE_TGID);
	mark_oom_victim(victim);
	/* mark 之后 victim 能使用 reserve，日志仍在 task_lock 内取一致的 mm/signal 快照。 */
	pr_err("%s: Killed process %d (%s) total-vm:%lukB, anon-rss:%lukB, file-rss:%lukB, shmem-rss:%lukB, UID:%u pgtables:%lukB oom_score_adj:%d\n",
		message, task_pid_nr(victim), victim->comm, K(mm->total_vm),
		K(get_mm_counter_sum(mm, MM_ANONPAGES)),
		K(get_mm_counter_sum(mm, MM_FILEPAGES)),
		K(get_mm_counter_sum(mm, MM_SHMEMPAGES)),
		from_kuid(&init_user_ns, task_uid(victim)),
		mm_pgtables_bytes(mm) >> 10, victim->signal->oom_score_adj);
	task_unlock(victim);

	/*
	 * Kill all user processes sharing victim->mm in other thread groups, if
	 * any.  They don't get access to memory reserves, though, to avoid
	 * depletion of all memory.  This prevents mm->mmap_lock livelock when an
	 * oom killed thread cannot exit because it requires the semaphore and
	 * its contended by another thread trying to allocate memory itself.
	 * That thread will now get access to memory reserves since it has a
	 * pending fatal signal.
	 */
	/* 同 mm 的其它线程组也必须收到 SIGKILL，否则它们可持 mmap_lock 阻塞受害者退出。 */
	rcu_read_lock();
	for_each_process(p) {
		/* 遍历仅发送信号，不能在 RCU 临界区等待其它组退出。 */
		if (!process_shares_mm(p, mm))
			continue;
		if (same_thread_group(p, victim))
			continue;
		if (is_global_init(p)) {
			/* init 不能杀；其持有该 mm 时禁止 reaper，避免把系统关键地址空间强行 zap。 */
			can_oom_reap = false;
			mm_flags_set(MMF_OOM_SKIP, mm);
			pr_info("oom killer %d (%s) has mm pinned by %d (%s)\n",
					task_pid_nr(victim), victim->comm,
					task_pid_nr(p), p->comm);
			continue;
		}
		/*
		 * No kthread_use_mm() user needs to read from the userspace so
		 * we are ok to reap it.
		 */
		if (unlikely(p->flags & PF_KTHREAD))
			/* kthread_use_mm 用户不从用户态读取，跳过信号但仍允许正常 reaper。 */
			continue;
		do_send_sig_info(SIGKILL, SEND_SIG_PRIV, p, PIDTYPE_TGID);
	}
	rcu_read_unlock();

	if (can_oom_reap)
		/* timer 延迟回收给自然 exit 留时间，避免与退出路径争抢资源。 */
		queue_oom_reaper(victim);

	/* mmdrop 与 put_task_struct 分别归还本函数的 mmgrab 和 chosen/替换后 task 引用。 */
	mmdrop(mm);
	put_task_struct(victim);
}

/*
 * Kill provided task unless it's secured by setting
 * oom_score_adj to OOM_SCORE_ADJ_MIN.
 */
/*
 * oom_kill_memcg_member() - 对 oom.group 中一个非受保护成员执行核心 kill。
 * 业务背景：memory.oom.group 要求整组终止，不能只处理 badness 最高的主 victim。
 * 入参：task 是 memcg 扫描借用对象；message 是只读日志字符串透传。
 * 返回/副作用：始终 0 继续；合格成员先 get task 引用，再由核心路径消费。
 * 注意事项：OOM_SCORE_ADJ_MIN 与 global init 保持受保护；重复成员由底层 MEMDIE/退出状态处理。
 */
static int oom_kill_memcg_member(struct task_struct *task, void *message)
{
	/* group kill 回调逐任务执行；受 OOM_SCORE_ADJ_MIN 或 init 保护的成员保持存活。 */
	if (task->signal->oom_score_adj != OOM_SCORE_ADJ_MIN &&
	    !is_global_init(task)) {
		get_task_struct(task);
		__oom_kill_process(task, message);
	}
	return 0;
}

/*
 * oom_kill_process() - 消费 chosen 并执行自然退出快路、主 kill 及可选 memcg group kill。
 * 业务背景：把选择结果统一提交，确保诊断限速不影响信号、引用和整组 kill 语义。
 * 入参：oc->chosen 必须是真实持引用 task而非 NULL/-1；message 是调用期日志串。
 * 返回/副作用：无返回且总消费 chosen 引用；可能仅加速已有退出者，或发送一个/整组 SIGKILL。
 * 注意事项：oom_group 引用在组扫描后 put；reaper 排队会另取自己的 task 引用。
 */
static void oom_kill_process(struct oom_control *oc, const char *message)
{
	/* 先处理“已将退出”的 chosen，随后限速日志、kill 主受害者，必要时扩展为整个 memcg。 */
	struct task_struct *victim = oc->chosen;
	struct mem_cgroup *oom_group;
	static DEFINE_RATELIMIT_STATE(oom_rs, DEFAULT_RATELIMIT_INTERVAL,
					      DEFAULT_RATELIMIT_BURST);
	/* victim 是 oc->chosen 的已持有引用，函数所有出口都会由下层路径 put。 */
	/* ratelimit state 是静态全局诊断限流，不保护实际 victim 选择和 kill。 */

	/*
	 * If the task is already exiting, don't alarm the sysadmin or kill
	 * its children or threads, just give it access to memory reserves
	 * so it can die quickly
	 */
	/* task_lock 稳定 victim->mm 与退出状态，避免在判断后立即 exit_mm。 */
	task_lock(victim);
	if (task_will_free_mem(victim)) {
		/* 已退出 victim 不杀整个共享组，只授 reserve/reaper 加速自然退出。 */
		mark_oom_victim(victim);
		queue_oom_reaper(victim);
		task_unlock(victim);
		put_task_struct(victim);
		return;
	}
	task_unlock(victim);

	if (__ratelimit(&oom_rs)) {
		/* 诊断不应在并发 OOM 风暴中刷屏；kill 语义不受限速影响。 */
		dump_header(oc);
		dump_oom_victim(oc, victim);
	}

	/*
	 * Do we need to kill the entire memory cgroup?
	 * Or even one of the ancestor memory cgroups?
	 * Check this out before killing the victim task.
	 */
	/* 获取 oom_group 引用在杀主任务前决定，避免 victim 退出导致 cgroup 选择失效。 */
	oom_group = mem_cgroup_get_oom_group(victim, oc->memcg);
	/* oom_group 为 NULL 时只杀 selected victim；非 NULL 时其后扫描所有组成员。 */

	__oom_kill_process(victim, message);

	/*
	 * If necessary, kill all tasks in the selected memory cgroup.
	 */
	if (oom_group) {
		/* group kill 事件、扫描和 put 构成完整 css 引用生命周期。 */
		memcg_memory_event(oom_group, MEMCG_OOM_GROUP_KILL);
		mem_cgroup_print_oom_group(oom_group);
		mem_cgroup_scan_tasks(oom_group, oom_kill_memcg_member,
				      (void *)message);
		mem_cgroup_put(oom_group);
	}
}

/*
 * Determines whether the kernel must panic because of the panic_on_oom sysctl.
 */
/*
 * check_panic_on_oom() - 按 sysctl 与约束域决定是否立即 panic。
 * 业务背景：管理员可选择让不可接受的系统级或任意 OOM 直接崩溃，而非继续杀用户任务。
 * 入参：oc 为只读控制块，constraint 已分类；SysRq 通过 order=-1 识别。
 * 返回/副作用：无需 panic 时返回 void；命中策略会打印 header 后调用 panic，不再返回。
 * 注意事项：值 1 只覆盖 CONSTRAINT_NONE，值 2 覆盖全部非 SysRq OOM。
 */
static void check_panic_on_oom(struct oom_control *oc)
{
	/* panic_on_oom=2 对所有非 SysRq 域生效；值 1 仅在无约束全局 OOM 中升级为 panic。 */
	if (likely(!sysctl_panic_on_oom))
		/* 默认不 panic，继续走选择/kill。 */
		return;
	if (sysctl_panic_on_oom != 2) {
		/*
		 * panic_on_oom == 1 only affects CONSTRAINT_NONE, the kernel
		 * does not panic for cpuset, mempolicy, or memcg allocation
		 * failures.
		 */
		if (oc->constraint != CONSTRAINT_NONE)
			return;
	}
	/* Do not panic for oom kills triggered by sysrq */
	if (is_sysrq_oom(oc))
		/* 人工 SysRq 诊断必须保持系统可操作，不受 panic_on_oom 影响。 */
		return;
	dump_header(oc);
	panic("Out of memory: %s panic_on_oom is enabled\n",
		sysctl_panic_on_oom == 2 ? "compulsory" : "system-wide");
}

/* notifier 链给予驱动/子系统一次回收机会；只在全局 OOM 前调用，不能替代 memcg 强制选择。 */
static BLOCKING_NOTIFIER_HEAD(oom_notify_list);

/*
 * register_oom_notifier() - 把回收机会回调注册到全局 OOM blocking notifier 链。
 * 业务背景：全局 kill 前允许驱动/子系统同步释放缓存，从而避免不必要的 victim。
 * 入参：nb 由注册者持有并保证到注销及在飞回调结束后仍有效；回调允许睡眠。
 * 返回/副作用：透传 blocking 链注册结果；成功后链持有节点关系但不接管对象内存。
 * 注意事项：只在全局 OOM 调用，不能依赖它缓解 memcg 域压力。
 */
int register_oom_notifier(struct notifier_block *nb)
{
	/* blocking 链允许通知回调睡眠；注册者必须在注销后自行保证回调对象生命周期。 */
	return blocking_notifier_chain_register(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(register_oom_notifier);

/*
 * unregister_oom_notifier() - 从全局 OOM blocking notifier 链移除回调。
 * 业务背景：模块卸载前必须阻止后续 OOM 再调用即将销毁的 notifier 对象。
 * 入参：nb 必须是此前成功注册且生命周期仍有效的同一对象。
 * 返回/副作用：透传注销结果；成功后不再接受新调用，注册者随后管理对象内存。
 * 注意事项：调用者仍须遵守 blocking notifier 核心对在飞回调和注销上下文的同步契约。
 */
int unregister_oom_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&oom_notify_list, nb);
}
EXPORT_SYMBOL_GPL(unregister_oom_notifier);

/**
 * out_of_memory - kill the "best" process when we run out of memory
 * @oc: pointer to struct oom_control
 *
 * If we run out of memory, we have the choice between either
 * killing a random task (bad), letting the system crash (worse)
 * OR try to be smart about which process to kill. Note that we
 * don't have to be perfect here, we just have to be good.
 */
/*
 * out_of_memory() - 对一次最终分配失败执行 OOM 仲裁。
 * 业务背景：在常规回收耗尽后，依次尝试 notifier、已有退出者、panic 策略和最佳 victim 来恢复进展。
 * 入参：oc 为本轮可写控制块，携带原分配 gfp/order/域；调用者通常持 oom_lock 稳定全局决策。
 * 返回/副作用：true 表示已释放/已触发退出或应重试，false 仅表示 killer 被禁用；可 panic 或发送信号。
 * 注意事项：chosen 存在 NULL、-1 哨兵和持引用 task 三态；本函数只把真实 task 交给 kill 路径。
 */
bool out_of_memory(struct oom_control *oc)
{
	/* 这是分配失败的最终仲裁：返回 true 表示已回收/已触发退出可重试，false 表示 OOM 被禁用。 */
	unsigned long freed = 0;
	/* freed 是 notifier 报告的即时可用页数，仅用于避免不必要的本轮杀进程。 */

	if (oom_killer_disabled)
		/* disable 调用者要求所有分配直接失败，不能新选 victim 打破该隔离语义。 */
		return false;

	if (!is_memcg_oom(oc)) {
		/* 全局 OOM 先让 notifier 同步释放缓存；memcg 不能把域外释放当作成功。 */
		blocking_notifier_call_chain(&oom_notify_list, 0, &freed);
		if (freed > 0 && !is_sysrq_oom(oc))
			/* Got some memory back in the last second. */
			return true;
	}

	/*
	 * If current has a pending SIGKILL or is exiting, then automatically
	 * select it.  The goal is to allow it to allocate so that it may
	 * quickly exit and free its memory.
	 */
	if (task_will_free_mem(current)) {
		/* 触发者已带致命退出状态时给它 reserve/reaper，比再杀无关任务更快收敛。 */
		mark_oom_victim(current);
		queue_oom_reaper(current);
		return true;
	}

	/*
	 * The OOM killer does not compensate for IO-less reclaim.
	 * But mem_cgroup_oom() has to invoke the OOM killer even
	 * if it is a GFP_NOFS allocation.
	 */
	if (!(oc->gfp_mask & __GFP_FS) && !is_memcg_oom(oc))
		/* 非文件系统分配不应在全局 OOM 里打破 NOFS 约束；让调用者重试/回退。 */
		return true;

	/*
	 * Check if there were limitations on the allocation (only relevant for
	 * NUMA and memcg) that may require different handling.
	 */
	/* 约束分类同时设置评分分母与 nodemask；mempolicy 以外可清 nodemask 扩大安全候选域。 */
	oc->constraint = constrained_alloc(oc);
	if (oc->constraint != CONSTRAINT_MEMORY_POLICY)
		oc->nodemask = NULL;
	check_panic_on_oom(oc);

	if (!is_memcg_oom(oc) && sysctl_oom_kill_allocating_task &&
	    current->mm && !oom_unkillable_task(current) &&
	    oom_cpuset_eligible(current, oc) &&
	    current->signal->oom_score_adj != OOM_SCORE_ADJ_MIN) {
		/* sysctl 强制直接选择 current，仍先取 task 引用再进入统一 kill 路径。 */
		get_task_struct(current);
		oc->chosen = current;
		oom_kill_process(oc, "Out of memory (oom_kill_allocating_task)");
		return true;
	}

	/* 常规路径扫描域内最高分任务；chosen 可能是 NULL 或 -1 哨兵。 */
	select_bad_process(oc);
	/* Found nothing?!?! */
	if (!oc->chosen) {
		/* 没有可杀任务时全局非 SysRq OOM 无法恢复，panic 防止分配器无限循环。 */
		dump_header(oc);
		pr_warn("Out of memory and no killable processes...\n");
		/*
		 * If we got here due to an actual allocation at the
		 * system level, we cannot survive this and will enter
		 * an endless loop in the allocator. Bail out now.
		 */
		if (!is_sysrq_oom(oc) && !is_memcg_oom(oc))
			panic("System is deadlocked on memory\n");
	}
	if (oc->chosen && oc->chosen != (void *)-1UL)
		/* -1 表示已有活跃 victim，不能当 task 指针解引用；普通 chosen 则完成 kill。 */
		oom_kill_process(oc, !is_memcg_oom(oc) ? "Out of memory" :
				 "Memory cgroup out of memory");
	return !!oc->chosen;
}

/*
 * The pagefault handler calls here because some allocation has failed. We have
 * to take care of the memcg OOM here because this is the only safe context without
 * any locks held but let the oom killer triggered from the allocation context care
 * about the global OOM.
 */
/*
 * pagefault_out_of_memory() - 处理泄漏到缺页顶层的 VM_FAULT_OOM。
 * 业务背景：缺页点是无额外锁的安全 memcg 同步位置，但全局 OOM 应留给原分配上下文处理。
 * 入参：无；current 是发生 page fault 的任务。
 * 返回/副作用：无返回；先同步 memcg，未处理且无 fatal signal 时仅限速告警并让缺页重试。
 * 注意事项：不直接调用全局 out_of_memory()，避免上下文/锁不明时重复选择 victim。
 */
void pagefault_out_of_memory(void)
{
	/* 缺页上下文先同步 memcg OOM；全局 OOM 留给原分配上下文，避免持锁/递归风险。 */
	static DEFINE_RATELIMIT_STATE(pfoom_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	if (mem_cgroup_oom_synchronize(true))
		/* 已由 memcg 处理时不重复向全局 OOM 路径报告。 */
		return;

	if (fatal_signal_pending(current))
		/* 即将退出的任务无需为了 page fault 再触发诊断或选择 victim。 */
		return;

	if (__ratelimit(&pfoom_rs))
		pr_warn("Huh VM_FAULT_OOM leaked out to the #PF handler. Retrying PF\n");
}

/*
 * process_mrelease() - 通过 pidfd 主动 reap 一个已确定退出任务的私有地址空间。
 * 业务背景：用户态 supervisor 可在进程死亡但 exit_mmap 尚未完成时加速释放内存。
 * 入参：pidfd 标识稳定 task；flags 当前必须为 0。目标及全部 mm 共享者必须都会退出。
 * 返回/副作用：成功/已处理返回 0；按参数、任务、信号、锁冲突返回 errno；可能置 MMF_UNSTABLE 并 zap。
 * 注意事项：task/mm 引用和 task/mmap 锁按标签逆序释放；CONFIG_MMU=n 固定返回 -ENOSYS。
 */
SYSCALL_DEFINE2(process_mrelease, int, pidfd, unsigned int, flags)
{
#ifdef CONFIG_MMU
	/* pidfd 提供稳定任务身份；本调用只允许已注定退出的任务主动释放其用户页。 */
	struct mm_struct *mm = NULL;
	struct task_struct *task;
	struct task_struct *p;
	unsigned int f_flags;
	bool reap = false;
	long ret = 0;

	if (flags)
		/* 预留 flags 目前必须为零，拒绝未知语义而不冒险改变回收范围。 */
		return -EINVAL;

	/* pidfd_get_task 获取 task 引用，所有后续路径都必须经 put_task 标签归还。 */
	task = pidfd_get_task(pidfd, &f_flags);
	if (IS_ERR(task))
		return PTR_ERR(task);

	/*
	 * Make sure to choose a thread which still has a reference to mm
	 * during the group exit
	 */
	p = find_lock_task_mm(task);
	if (!p) {
		ret = -ESRCH;
		goto put_task;
	}

	mm = p->mm;
	/* 在释放 task_lock 前 mmgrab 固定地址空间对象，避免组退出使 p->mm 失效。 */
	mmgrab(mm);

	/* 仅当所有共享者都会释放内存时允许 reap，避免用户主动清空仍被运行者使用的 mm。 */
	if (task_will_free_mem(p))
		reap = true;
	else {
		/* Error only if the work has not been done already */
		/* 已置 OOM_SKIP 说明 exit/reaper 已做过工作，幂等请求保持成功而非报错。 */
		if (!mm_flags_test(MMF_OOM_SKIP, mm))
			ret = -EINVAL;
	}
	task_unlock(p);

	/* 先释放 task_lock；后续对独立 mm 引用取得 mmap 锁，不能把两种锁次序叠加。 */
	if (!reap)
		goto drop_mm;

	if (mmap_read_lock_killable(mm)) {
		ret = -EINTR;
		goto drop_mm;
	}
	/* mmap 读锁与 exit_mmap 的写锁序列化，第二次检查避免依据过期的 OOM_SKIP 执行 zap。 */
	/*
	 * Check MMF_OOM_SKIP again under mmap_read_lock protection to ensure
	 * possible change in exit_mmap is seen
	 */
	if (!mm_flags_test(MMF_OOM_SKIP, mm) && !__oom_reap_task_mm(mm))
		ret = -EAGAIN;
	mmap_read_unlock(mm);

drop_mm:
	/* mmgrab 与 mmdrop 成对，即使 reaping 未执行也必须在此归还临时地址空间引用。 */
	mmdrop(mm);
put_task:
	/* pidfd_get_task 的 task 引用覆盖所有错误分支，统一释放后再向用户态返回 errno。 */
	put_task_struct(task);
	return ret;
#else
	return -ENOSYS;
#endif /* CONFIG_MMU */
}
